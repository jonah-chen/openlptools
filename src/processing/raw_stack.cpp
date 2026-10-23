#include "processing/raw_stack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// The SDK's writer header passes an enum where it takes an unsigned; its warning, not ours.
#pragma warning(push)
#pragma warning(disable : 4245)
#include "dng_auto_ptr.h"
#include "dng_camera_profile.h"
#include "dng_color_space.h"
#include "dng_date_time.h"
#include "dng_exif.h"
#include "dng_file_stream.h"
#include "dng_host.h"
#include "dng_image.h"
#include "dng_image_writer.h"
#include "dng_info.h"
#include "io/dng_internal.h"
#include "dng_linearization_info.h"
#include "dng_memory.h"
#include "dng_mosaic_info.h"
#include "dng_negative.h"
#include "dng_pixel_buffer.h"
#include "dng_preview.h"
#include "dng_render.h"
#include "dng_tag_values.h"
#include "dng_xmp.h"
#pragma warning(pop)
#include "processing/median_select.h"
#include "common/parallel.h"

namespace fs = std::filesystem;

const char* StackModeName(StackMode mode) {
    return mode == StackMode::Mean ? "mean" : "median";
}

namespace {

// ---- Reading frames ------------------------------------------------------------

// A parsed DNG: the SDK objects needed to read its image or to write it back.
struct OpenedDng {
    AutoPtr<dng_host> host;
    AutoPtr<dng_file_stream> stream;
    dng_info info;
    AutoPtr<dng_negative> negative;

    OpenedDng(const fs::path& path, uint32_t threads) {
        host.Reset(NewThreadedHost(threads));
        stream.Reset(new dng_file_stream(path.c_str()));
        info.Parse(*host, *stream);
        info.PostParse(*host);
        if (!info.IsValidDNG()) {
            ThrowBadFormat();
        }
        negative.Reset(host->Make_dng_negative());
        negative->Parse(*host, *stream, info);
        negative->PostParse(*host, *stream, info);
    }

    const dng_ifd& MainIfd() const { return *info.fIFD[info.fMainIndex]; }
};

// The samples of an 8- or 16-bit image as 16-bit values, planes interleaved.
std::vector<uint16_t> CopySamples(const dng_image& image) {
    const dng_rect bounds = image.Bounds();
    const uint32 planes = image.Planes();
    std::vector<uint16_t> out(static_cast<size_t>(bounds.W()) * bounds.H() * planes);
    if (image.PixelType() == ttShort) {
        dng_pixel_buffer buffer(bounds, 0, planes, ttShort, pcInterleaved, out.data());
        image.Get(buffer);
    } else if (image.PixelType() == ttByte) {
        std::vector<uint8_t> bytes(out.size());
        dng_pixel_buffer buffer(bounds, 0, planes, ttByte, pcInterleaved, bytes.data());
        image.Get(buffer);
        std::copy(bytes.begin(), bytes.end(), out.begin());
    } else {
        throw std::runtime_error("only 8- and 16-bit integer DNGs can be stacked (floating-point and 32-bit files cannot)");
    }
    return out;
}

// The reverse: fills an 8- or 16-bit image from 16-bit samples.
void PutSamples(dng_image& image, const std::vector<uint16_t>& samples) {
    const dng_rect bounds = image.Bounds();
    const uint32 planes = image.Planes();
    if (image.PixelType() == ttShort) {
        dng_pixel_buffer buffer(bounds, 0, planes, ttShort, pcInterleaved, const_cast<uint16_t*>(samples.data()));
        image.Put(buffer);
    } else {
        std::vector<uint8_t> bytes(samples.size());
        for (size_t i = 0; i < samples.size(); ++i) {
            bytes[i] = static_cast<uint8_t>(std::min<uint16_t>(samples[i], 255));
        }
        dng_pixel_buffer buffer(bounds, 0, planes, ttByte, pcInterleaved, bytes.data());
        image.Put(buffer);
    }
}

// ---- Checking that the frames match --------------------------------------------------

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

double TimeSeconds(const dng_date_time_info& info) {
    const dng_date_time& dt = info.DateTime();
    double fraction = 0.0;
    const std::string sub = info.Subseconds().Get();
    if (!sub.empty() && std::ranges::all_of(sub, [](unsigned char c) { return std::isdigit(c); })) {
        fraction = std::stod("0." + sub);
    }
    return static_cast<double>(DaysFromCivil(dt.fYear, dt.fMonth, dt.fDay)) * 86400.0 + dt.fHour * 3600.0 +
           dt.fMinute * 60.0 + dt.fSecond + fraction;
}

std::string Rational(const dng_urational& r, const char* unit = "") {
    return r.IsValid() ? std::format("{}/{}{}", r.n, r.d, unit) : std::string("not set");
}

std::string Rational(const dng_srational& r, const char* unit = "") {
    return r.IsValid() ? std::format("{}/{}{}", r.n, r.d, unit) : std::string("not set");
}

std::string Text(const dng_string& s) {
    return s.IsEmpty() ? std::string("not set") : std::string(s.Get());
}

std::string RectText(const dng_rect& r) {
    return std::format("top {} left {} bottom {} right {}", r.t, r.l, r.b, r.r);
}

std::string Hex(uint64_t v) {
    return std::format("{:016x}", v);
}

uint64_t Fnv1a(const void* data, size_t bytes) {
    uint64_t hash = 14695981039346656037ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        hash = (hash ^ p[i]) * 1099511628211ull;
    }
    return hash;
}

// What is known about a frame from its tags alone: everything that must agree
// across a stack, as label/value text so a difference can be reported in words,
// plus what the processing needs.
struct FrameProbe {
    fs::path path;
    std::string name;
    std::map<std::string, std::string> props;  // label -> value
    std::vector<std::string> order;            // labels in display order

    uint32 width = 0, height = 0, planes = 1, pixelType = ttShort;  // the stored (stage 1) image
    uint32 activeWidth = 0, activeHeight = 0;                       // the linearized (stage 2) image
    dng_date_time_info dateTimeOriginal, dateTimeDigitized;
    bool hasTime = false;
    double timeSeconds = 0.0;
    double exposureTime = 0.0;

    void Add(const char* label, std::string value) {
        if (props.emplace(label, std::move(value)).second) {
            order.emplace_back(label);
        }
    }
};

FrameProbe ProbeFrame(const fs::path& path) {
    OpenedDng dng(path, 1);
    dng_negative& negative = *dng.negative;  // GetExif() has no const form
    const dng_ifd& ifd = dng.MainIfd();
    FrameProbe f;
    f.path = path;
    f.name = PathUtf8(path.filename());

    // Camera and lens.
    if (const dng_exif* exif = negative.GetExif()) {
        f.Add("Camera make", Text(exif->fMake));
        f.Add("Camera model", Text(exif->fModel));
        f.Add("Unique camera model", Text(negative.ModelName()));
        f.Add("Body serial number", Text(exif->fCameraSerialNumber));
        f.Add("Firmware", Text(exif->fFirmware));
        f.Add("Lens", Text(exif->fLensName));
        f.Add("Lens serial number", Text(exif->fLensSerialNumber));
        f.Add("Lens specification", std::format("{} {} {} {}", Rational(exif->fLensInfo[0]), Rational(exif->fLensInfo[1]),
                                                Rational(exif->fLensInfo[2]), Rational(exif->fLensInfo[3])));
        f.Add("Focal length", Rational(exif->fFocalLength, " mm"));
        f.Add("Focal length in 35 mm", std::to_string(exif->fFocalLengthIn35mmFilm));

        // Exposure settings.
        f.Add("Shutter speed", Rational(exif->fExposureTime, " s"));
        f.Add("Aperture", exif->fFNumber.IsValid() ? std::format("f/{:.2f}", exif->fFNumber.As_real64()) : "not set");
        f.Add("ISO", std::to_string(exif->fISOSpeedRatings[0]));
        f.Add("Exposure compensation", Rational(exif->fExposureBiasValue, " EV"));
        f.Add("Exposure program", std::to_string(exif->fExposureProgram));
        f.Add("Exposure mode", std::to_string(exif->fExposureMode));
        f.Add("Metering mode", std::to_string(exif->fMeteringMode));
        f.Add("White balance mode", std::to_string(exif->fWhiteBalance));
        f.Add("Flash", std::to_string(exif->fFlash));
        f.Add("Sensitivity type", std::to_string(exif->fSensitivityType));

        f.dateTimeOriginal = exif->fDateTimeOriginal;
        f.dateTimeDigitized = exif->fDateTimeDigitized;
        if (exif->fDateTimeOriginal.IsValid()) {
            f.hasTime = true;
            f.timeSeconds = TimeSeconds(exif->fDateTimeOriginal);
        }
        if (exif->fExposureTime.IsValid()) {
            f.exposureTime = exif->fExposureTime.As_real64();
        }
    }

    // The stored image.
    f.width = ifd.fImageWidth;
    f.height = ifd.fImageLength;
    f.planes = ifd.fSamplesPerPixel;
    f.pixelType = ifd.PixelType();
    f.Add("Image size", std::format("{} x {}", f.width, f.height));
    f.Add("Samples per pixel", std::to_string(ifd.fSamplesPerPixel));
    f.Add("Bits per sample", std::to_string(ifd.fBitsPerSample[0]));
    f.Add("Sample format", std::to_string(ifd.fSampleFormat[0]));
    f.Add("Photometric interpretation", std::to_string(ifd.fPhotometricInterpretation));
    f.Add("Compression", std::to_string(ifd.fCompression));
    f.Add("Orientation", std::to_string(dng.negative->Orientation().GetAdobe()));
    {
        const uint32 version = dng.info.fShared->fDNGVersion;
        f.Add("DNG version", std::format("{}.{}.{}.{}", version >> 24, (version >> 16) & 0xFF, (version >> 8) & 0xFF,
                                         version & 0xFF));
    }

    // Levels and linearization.
    f.activeWidth = f.width;
    f.activeHeight = f.height;
    if (const dng_linearization_info* lin = negative.GetLinearizationInfo()) {
        if (lin->fActiveArea.NotEmpty()) {
            f.activeWidth = lin->fActiveArea.W();
            f.activeHeight = lin->fActiveArea.H();
        }
        f.Add("Active area", lin->fActiveArea.NotEmpty() ? RectText(lin->fActiveArea) : "whole image");
        std::string masked;
        for (uint32 i = 0; i < lin->fMaskedAreaCount; ++i) {
            masked += (i ? "; " : "") + RectText(lin->fMaskedArea[i]);
        }
        f.Add("Masked areas", masked.empty() ? "none" : masked);
        std::string blacks = std::format("{} x {} pattern:", lin->fBlackLevelRepeatRows, lin->fBlackLevelRepeatCols);
        for (uint32 r = 0; r < lin->fBlackLevelRepeatRows; ++r) {
            for (uint32 c = 0; c < lin->fBlackLevelRepeatCols; ++c) {
                for (uint32 p = 0; p < f.planes; ++p) {
                    blacks += std::format(" {:g}", lin->fBlackLevel[r][c][p]);
                }
            }
        }
        f.Add("Black level", blacks);
        f.Add("Black level deltas",
              std::format("{} per row, {} per column", lin->fBlackDeltaV.Get() ? lin->fBlackDeltaV->LogicalSize() / 8 : 0,
                          lin->fBlackDeltaH.Get() ? lin->fBlackDeltaH->LogicalSize() / 8 : 0));
        std::string whites;
        for (uint32 p = 0; p < f.planes; ++p) {
            whites += std::format("{}{:g}", p ? " " : "", lin->fWhiteLevel[p]);
        }
        f.Add("White level", whites);
        if (const dng_memory_block* table = lin->fLinearizationTable.Get()) {
            f.Add("Linearization table",
                  std::format("{} entries, hash {}", table->LogicalSize() / 2, Hex(Fnv1a(table->Buffer(), table->LogicalSize()))));
        } else {
            f.Add("Linearization table", "none");
        }
    } else {
        f.Add("Active area", "whole image");
        f.Add("Black level", "default");
        f.Add("White level", "default");
        f.Add("Linearization table", "none");
    }

    // Mosaic.
    if (const dng_mosaic_info* mosaic = negative.GetMosaicInfo(); mosaic && mosaic->IsColorFilterArray()) {
        std::string pattern = std::format("{} x {}:", mosaic->fCFAPatternSize.v, mosaic->fCFAPatternSize.h);
        for (int32 r = 0; r < mosaic->fCFAPatternSize.v; ++r) {
            for (int32 c = 0; c < mosaic->fCFAPatternSize.h; ++c) {
                pattern += std::format(" {}", mosaic->fCFAPattern[r][c]);
            }
        }
        f.Add("CFA pattern", pattern);
        std::string colors;
        for (uint32 p = 0; p < mosaic->fColorPlanes; ++p) {
            colors += std::format("{}{}", p ? " " : "", mosaic->fCFAPlaneColor[p]);
        }
        f.Add("CFA plane colors", colors);
        f.Add("CFA layout", std::to_string(mosaic->fCFALayout));
        f.Add("Bayer green split", std::to_string(mosaic->fBayerGreenSplit));
    } else {
        f.Add("CFA pattern", "none (demosaiced image)");
    }

    // Color and rendering hints.
    f.Add("Color channels", std::to_string(negative.ColorChannels()));
    if (negative.HasCameraNeutral()) {
        const dng_vector& n = negative.CameraNeutral();
        std::string neutral;
        for (uint32 i = 0; i < n.Count(); ++i) {
            neutral += std::format("{}{:.5f}", i ? " " : "", n[i]);
        }
        f.Add("As-shot neutral", neutral);
    } else if (negative.HasCameraWhiteXY()) {
        const dng_xy_coord& xy = negative.CameraWhiteXY();
        f.Add("As-shot white", std::format("x {:.5f} y {:.5f}", xy.x, xy.y));
    } else {
        f.Add("As-shot neutral", "not set");
    }
    f.Add("Baseline exposure", std::format("{:.3f}", negative.BaselineExposure()));
    f.Add("Baseline noise", std::format("{:.3f}", negative.BaselineNoise()));
    f.Add("Shadow scale", std::format("{:.3f}", negative.ShadowScale()));
    f.Add("Linear response limit", std::format("{:.3f}", negative.LinearResponseLimit()));
    f.Add("Default crop", std::format("origin {} {} size {} {}", Rational(negative.DefaultCropOriginH()),
                                      Rational(negative.DefaultCropOriginV()), Rational(negative.DefaultCropSizeH()),
                                      Rational(negative.DefaultCropSizeV())));
    f.Add("Default scale", std::format("{} {}", Rational(negative.DefaultScaleH()), Rational(negative.DefaultScaleV())));
    if (negative.HasNoiseProfile()) {
        const dng_noise_profile& profile = negative.NoiseProfile();
        std::string noise;
        for (uint32 i = 0; i < profile.NumFunctions(); ++i) {
            noise += std::format("{}scale {:.4g} offset {:.4g}", i ? "; " : "", profile.NoiseFunction(i).Scale(),
                                 profile.NoiseFunction(i).Offset());
        }
        f.Add("Noise profile", noise);
    } else {
        f.Add("Noise profile", "none");
    }
    {
        std::string profiles = std::format("{}:", negative.ProfileCount());
        for (uint32 i = 0; i < negative.ProfileCount(); ++i) {
            const dng_camera_profile& profile = negative.ProfileByIndex(i);
            profiles += std::format(" {} ({})", Text(profile.Name()), profile.Fingerprint().ToUtf8HexString().Get());
        }
        f.Add("Camera profiles", profiles);
    }
    f.Add("Opcode lists", std::format("{} / {} / {} opcodes", negative.OpcodeList1().Count(),
                                      negative.OpcodeList2().Count(), negative.OpcodeList3().Count()));
    return f;
}

// A report of every property that differs from the reference frame, or empty.
std::string MismatchReport(const std::vector<FrameProbe>& frames, size_t reference) {
    const FrameProbe& ref = frames[reference];
    std::string report;
    for (const std::string& label : ref.order) {
        const std::string& value = ref.props.at(label);
        const FrameProbe* first = nullptr;
        int count = 0;
        for (size_t i = 0; i < frames.size(); ++i) {
            if (i == reference) {
                continue;
            }
            const auto it = frames[i].props.find(label);
            const std::string other = it == frames[i].props.end() ? std::string("not present") : it->second;
            if (other != value) {
                if (!first) {
                    first = &frames[i];
                }
                ++count;
            }
        }
        if (first) {
            report += std::format("\n  - {}: {} here; {} in {}", label, value, first->props.contains(label) ? first->props.at(label) : "not present",
                                  first->name);
            if (count > 1) {
                report += std::format(" and {} more frame{}", count - 1, count > 2 ? "s" : "");
            }
        }
    }
    if (report.empty()) {
        return report;
    }
    return std::format(
        "The {} frames cannot be stacked because they are not alike. A stack must come from one camera shooting a "
        "sequence with the settings unchanged, so every camera, lens, exposure and raw-format property has to agree. "
        "Compared with {} (the highlighted frame), the others differ in:{}",
        frames.size(), ref.name, report);
}

// ---- Combining ------------------------------------------------------------------------

// Rows of the frames to process at a time; several per thread balance the work.
constexpr size_t kBandRows = 16;

// Per-pixel median of frames (all width * height * planes samples) into out.
void MedianFrames(const std::vector<std::vector<uint16_t>>& frames, size_t rowSamples, size_t rows,
                  std::vector<uint16_t>& out) {
    const int n = static_cast<int>(frames.size());
    const size_t bands = (rows + kBandRows - 1) / kBandRows;
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::vector<uint16_t>> scratch(threads);
    std::vector<std::vector<const uint16_t*>> rowPointers(threads, std::vector<const uint16_t*>(frames.size()));
    ParallelForEach(bands, threads, [&](unsigned thread, size_t band) {
        const size_t first = band * kBandRows;
        const size_t count = std::min(kBandRows, rows - first) * rowSamples;
        const size_t offset = first * rowSamples;
        std::vector<const uint16_t*>& ptrs = rowPointers[thread];
        for (size_t k = 0; k < frames.size(); ++k) {
            ptrs[k] = frames[k].data() + offset;
        }
        median::MedianRows(ptrs.data(), n, out.data() + offset, count, scratch[thread]);
    });
}

// ---- Writing the result ---------------------------------------------------------------------

constexpr const char* kSoftwareName = "raw2raw2";

void AddPreviews(dng_host& host, dng_negative& negative, dng_preview_list& previews) {
    dng_date_time_info now;
    CurrentDateTimeAndZone(now);
    dng_image_writer writer;
    for (int i = 0; i < 2; ++i) {
        dng_render render(host, negative);
        render.SetFinalSpace(dng_space_sRGB::Get());
        render.SetFinalPixelType(ttByte);
        render.SetMaximumSize(i == 0 ? 256 : 1024);
        AutoPtr<dng_image> image(render.Render());
        AutoPtr<dng_preview> preview;
        if (i == 0) {
            auto* thumbnail = new dng_image_preview;
            preview.Reset(thumbnail);
            thumbnail->SetImage(host, image);
        } else {
            auto* jpeg = new dng_jpeg_preview;
            preview.Reset(jpeg);
            writer.EncodeJPEGPreview(host, *image, *jpeg, 5);
        }
        preview->fInfo.fApplicationName.Set(kSoftwareName);
        preview->fInfo.fSettingsName.Set("Default");
        preview->fInfo.fColorSpace = previewColorSpace_sRGB;
        preview->fInfo.fDateTime = now.Encode_ISO_8601();
        previews.Append(preview);
    }
}

}  // namespace

// ---- Public helpers -----------------------------------------------------------------------

std::string StackMethodText(StackMode mode, int count) {
    if (mode == StackMode::Mean) {
        return std::format("Each frame is linearized and added into a floating-point sum as it is read, so {} frames need "
                           "the memory of about two. The result is the sum divided by {}, written as 16-bit linear data.",
                           count, count);
    }
    return std::format("All {} frames stay in memory; each pixel's values are ranked with {} and the middle one is kept, "
                       "in the frames' own encoding.",
                       count, median::MethodName(count));
}

uint64_t MedianMemoryBytes(int count, uint64_t width, uint64_t height, uint64_t planes) {
    const uint64_t frame = width * height * planes * 2;
    // Every input frame, the result, and about four frames' worth for rendering the previews.
    return frame * (static_cast<uint64_t>(count) + 1 + 4);
}

uint64_t AvailableMemoryBytes() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return status.ullAvailPhys;
    }
#endif
    return 8ull << 30;
}

// ---- StackJob -------------------------------------------------------------------------------

StackJob::StackJob(StackRequest request) : request_(std::move(request)) {}

StackJob::~StackJob() {
    Cancel();
}

void StackJob::Start() {
    if (running_ || finished_) {
        return;
    }
    running_ = true;
    start_ = std::chrono::steady_clock::now();
    worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}

void StackJob::Cancel() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

std::string StackJob::Status() const {
    std::scoped_lock lock(mutex_);
    return status_;
}

double StackJob::Seconds() const {
    if (running_) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    return seconds_;
}

void StackJob::SetPhase(std::string text, int total) {
    std::scoped_lock lock(mutex_);
    status_ = std::move(text);
    completed_ = 0;
    total_ = total;
}

void StackJob::Run(std::stop_token stop) {
    auto result = Execute(stop);
    seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    {
        std::scoped_lock lock(mutex_);
        if (stop.stop_requested()) {
            cancelled_ = true;
            status_ = "Cancelled.";
        } else if (!result) {
            failed_ = true;
            status_ = result.error();
        } else {
            status_ = std::format("Wrote {}", PathUtf8(request_.output));
        }
    }
    finished_ = true;
    running_ = false;
}

std::expected<void, std::string> StackJob::Execute(std::stop_token stop) {
    const StackRequest& req = request_;
    const int count = static_cast<int>(req.frames.size());
    if (count < 2) {
        return std::unexpected("A stack needs at least two frames.");
    }
    if (count > R2R_MAX_STACK_FRAMES) {
        return std::unexpected(std::format(
            "{} frames were selected, but this build stacks at most {}. Select fewer frames, or rebuild with the CMake "
            "option RAW2RAW2_MAX_STACK_FRAMES set higher (the median keeps every frame in memory, so make sure the "
            "machine has enough).",
            count, R2R_MAX_STACK_FRAMES));
    }
    const auto refIt = std::ranges::find(req.frames, req.metadataSource);
    if (refIt == req.frames.end()) {
        return std::unexpected("The highlighted frame, whose metadata the result takes, is not among the selected frames.");
    }
    const size_t reference = static_cast<size_t>(refIt - req.frames.begin());

    // Read every frame's tags and check that they agree.
    SetPhase("Checking the frames", count);
    std::vector<FrameProbe> probes;
    probes.reserve(req.frames.size());
    for (const fs::path& path : req.frames) {
        if (stop.stop_requested()) {
            return {};
        }
        auto probe = CatchErrors([&] { return ProbeFrame(path); });
        if (!probe) {
            return std::unexpected(std::format("Cannot read {}: {}", PathUtf8(path.filename()), probe.error()));
        }
        probes.push_back(std::move(*probe));
        ++completed_;
    }
    if (std::string report = MismatchReport(probes, reference); !report.empty()) {
        return std::unexpected(report);
    }
    const FrameProbe& ref = probes[reference];
    if (ref.pixelType != ttShort && ref.pixelType != ttByte) {
        return std::unexpected("Only 8- and 16-bit integer DNGs can be stacked; these frames hold floating-point or 32-bit data.");
    }

    // The earliest frame gives the capture time.
    const FrameProbe* earliest = nullptr;
    for (const FrameProbe& p : probes) {
        if (p.hasTime && (!earliest || p.timeSeconds < earliest->timeSeconds)) {
            earliest = &p;
        }
    }

    // Memory for a median: every frame at once.
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const unsigned inFlight = std::clamp(cores / 4, 1u, 4u);
    const unsigned threadsPerFrame = std::max(1u, cores / inFlight);
    const size_t frameSamples = static_cast<size_t>(ref.width) * ref.height * ref.planes;
    if (req.mode == StackMode::Median) {
        const uint64_t needed = MedianMemoryBytes(count, ref.width, ref.height, ref.planes);
        const uint64_t available = AvailableMemoryBytes();
        if (needed > available) {
            return std::unexpected(std::format(
                "A median of {} frames of {} x {} pixels needs about {:.1f} GB of memory, but only {:.1f} GB is free. "
                "Close other programs or select fewer frames.",
                count, ref.width, ref.height, needed / 1073741824.0, available / 1073741824.0));
        }
    }

    // Combine.
    std::vector<uint16_t> result;
    uint16_t linearBlack = 0;  // black level of the linearized data (mean output)
    std::string readError;
    std::mutex readMutex;
    SetPhase(req.mode == StackMode::Mean ? "Reading and adding the frames" : "Reading the frames", count);
    if (req.mode == StackMode::Mean) {
        const size_t activeSamples = static_cast<size_t>(ref.activeWidth) * ref.activeHeight * ref.planes;
        std::vector<double> sum(activeSamples, 0.0);
        ParallelForEach(req.frames.size(), inFlight, [&](unsigned, size_t i) {
            if (stop.stop_requested()) {
                return;
            }
            auto read = CatchErrors([&] {
                OpenedDng dng(req.frames[i], threadsPerFrame);
                dng.negative->ReadStage1Image(*dng.host, *dng.stream, dng.info);
                dng.negative->SynchronizeMetadata();
                dng.negative->BuildStage2Image(*dng.host);
                const dng_image& stage2 = *dng.negative->Stage2Image();
                if (stage2.PixelType() != ttShort) {
                    throw std::runtime_error("the linearized data is not 16-bit");
                }
                std::vector<uint16_t> samples = CopySamples(stage2);
                if (samples.size() != activeSamples) {
                    throw std::runtime_error("the linearized image has an unexpected size");
                }
                std::scoped_lock lock(readMutex);
                for (size_t s = 0; s < activeSamples; ++s) {
                    sum[s] += samples[s];
                }
                linearBlack = dng.negative->Stage3BlackLevel();
                return 0;
            });
            if (!read) {
                std::scoped_lock lock(readMutex);
                if (readError.empty()) {
                    readError = std::format("Cannot read {}: {}", PathUtf8(req.frames[i].filename()), read.error());
                }
                return;
            }
            ++completed_;
        });
        if (stop.stop_requested()) {
            return {};
        }
        if (!readError.empty()) {
            return std::unexpected(readError);
        }
        SetPhase("Averaging", 0);
        result.resize(activeSamples);
        const double scale = 1.0 / count;
        for (size_t s = 0; s < activeSamples; ++s) {
            result[s] = static_cast<uint16_t>(std::min(65535.0, std::round(sum[s] * scale)));
        }
    } else {
        std::vector<std::vector<uint16_t>> frames(req.frames.size());
        ParallelForEach(req.frames.size(), inFlight, [&](unsigned, size_t i) {
            if (stop.stop_requested()) {
                return;
            }
            auto read = CatchErrors([&] {
                OpenedDng dng(req.frames[i], threadsPerFrame);
                dng.negative->ReadStage1Image(*dng.host, *dng.stream, dng.info);
                std::vector<uint16_t> samples = CopySamples(*dng.negative->Stage1Image());
                if (samples.size() != frameSamples) {
                    throw std::runtime_error("the image has an unexpected size");
                }
                frames[i] = std::move(samples);
                return 0;
            });
            if (!read) {
                std::scoped_lock lock(readMutex);
                if (readError.empty()) {
                    readError = std::format("Cannot read {}: {}", PathUtf8(req.frames[i].filename()), read.error());
                }
                return;
            }
            ++completed_;
        });
        if (stop.stop_requested()) {
            return {};
        }
        if (!readError.empty()) {
            return std::unexpected(readError);
        }
        SetPhase(std::format("Taking the median ({})", median::MethodName(count)), 0);
        result.resize(frameSamples);
        MedianFrames(frames, static_cast<size_t>(ref.width) * ref.planes, ref.height, result);
        frames.clear();
    }
    if (stop.stop_requested()) {
        return {};
    }

    // Build the output from the reference frame's negative with the new pixels.
    SetPhase("Rendering the previews", 0);
    auto written = CatchErrors([&] {
        OpenedDng out(req.metadataSource, 0);
        dng_host& host = *out.host;
        dng_negative& negative = *out.negative;
        host.SetSaveDNGVersion(dngVersion_SaveDefault);
        host.SetSaveLinearDNG(false);
        host.SetKeepOriginalFile(false);
        negative.SynchronizeMetadata();

        if (req.mode == StackMode::Mean) {
            // Linear data the size of the active area, black at the SDK's linearized
            // black, white at 65535; the opcodes before and during linearization are
            // already applied.
            AutoPtr<dng_image> image(host.Make_dng_image(dng_rect(ref.activeHeight, ref.activeWidth), ref.planes, ttShort));
            PutSamples(*image, result);
            negative.SetStage1Image(image);
            negative.ClearLinearizationInfo();
            negative.SetActiveArea(dng_rect(ref.activeHeight, ref.activeWidth));
            negative.SetBlackLevel(linearBlack);
            negative.SetWhiteLevel(65535);
            negative.OpcodeList1().Clear();
            negative.OpcodeList2().Clear();
        } else {
            AutoPtr<dng_image> image(host.Make_dng_image(dng_rect(ref.height, ref.width), ref.planes, ref.pixelType));
            PutSamples(*image, result);
            negative.SetStage1Image(image);
        }
        result.clear();
        result.shrink_to_fit();
        negative.ClearRawImageDigest();
        negative.SetHasOriginalRawFileData(false);
        {
            AutoPtr<dng_memory_block> none;
            negative.SetOriginalRawFileData(none);
        }

        // Metadata: the earliest capture time; for a mean, the exposure the sum amounts to.
        // The XMP copies of the dates are set too, since the writer's EXIF/XMP sync
        // prefers XMP dates (they may carry a time zone).
        if (dng_exif* exif = negative.GetExif()) {
            if (earliest) {
                exif->fDateTimeOriginal = earliest->dateTimeOriginal;
                if (earliest->dateTimeDigitized.IsValid()) {
                    exif->fDateTimeDigitized = earliest->dateTimeDigitized;
                }
                if (dng_xmp* xmp = negative.GetXMP()) {
                    const dng_string original = exif->fDateTimeOriginal.Encode_ISO_8601();
                    xmp->SetString(XMP_NS_EXIF, "DateTimeOriginal", original);
                    xmp->SetString(XMP_NS_PHOTOSHOP, "DateCreated", original);
                    if (exif->fDateTimeDigitized.IsValid()) {
                        xmp->SetString(XMP_NS_EXIF, "DateTimeDigitized", exif->fDateTimeDigitized.Encode_ISO_8601());
                    }
                }
            }
            if (req.mode == StackMode::Mean) {
                for (uint32& iso : exif->fISOSpeedRatings) {
                    if (iso) {
                        iso = std::max(1u, static_cast<uint32>(std::lround(static_cast<double>(iso) / count)));
                    }
                }
                for (uint32* v : {&exif->fStandardOutputSensitivity, &exif->fRecommendedExposureIndex}) {
                    if (*v) {
                        *v = std::max(1u, static_cast<uint32>(std::lround(static_cast<double>(*v) / count)));
                    }
                }
                if (ref.exposureTime > 0.0) {
                    const double total = ref.exposureTime * count;
                    exif->SetExposureTime(total, false);
                    exif->fShutterSpeedValue.Set_real64(-std::log2(total), 1000);
                }
            }
            exif->fSoftware.Set(kSoftwareName);
        }
        negative.RecomputeRawDataUniqueID(host);

        // The previews need the rendered image; building it also clones the new
        // stage 1 image as the raw data to write.
        negative.BuildStage2Image(host);
        negative.BuildStage3Image(host);
        dng_preview_list previews;
        AddPreviews(host, negative, previews);
        if (stop.stop_requested()) {
            return 0;
        }

        SetPhase("Writing the DNG", 0);
        const fs::path temp = req.output.wstring() + L".part";
        {
            dng_file_stream stream(temp.c_str(), true);
            dng_image_writer writer;
            writer.WriteDNG(host, stream, negative, &previews, dngVersion_SaveDefault, false);
            stream.Flush();
        }
        std::error_code ec;
        fs::rename(temp, req.output, ec);
        if (ec) {
            fs::remove(temp, ec);
            throw std::runtime_error(std::format("could not replace {}: {}", PathUtf8(req.output), ec.message()));
        }
        outputBytes_ = fs::file_size(req.output, ec);
        return 0;
    });
    if (!written) {
        std::error_code ec;
        fs::remove(req.output.wstring() + L".part", ec);
        return std::unexpected(std::format("Could not write {}: {}", PathUtf8(req.output.filename()), written.error()));
    }
    return {};
}
