#include "io/dng_loader.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <exception>
#include <format>
#include <functional>
#include <mutex>
#include <new>
#include <optional>
#include <thread>

#include "dng_area_task.h"
#include "dng_auto_ptr.h"
#include "io/dng_internal.h"
#include "dng_camera_profile.h"
#include "dng_color_space.h"
#include "dng_color_spec.h"
#include "dng_exceptions.h"
#include "dng_exif.h"
#include "dng_file_stream.h"
#include "dng_filter_task.h"
#include "dng_host.h"
#include "dng_image.h"
#include "dng_info.h"
#include "dng_negative.h"
#include "dng_pixel_buffer.h"
#include "dng_render.h"
#include "dng_sdk_limits.h"
#include "dng_tag_values.h"

namespace {

// A dng_negative whose stage 3 image can be swapped out for one render.
class Negative : public dng_negative {
public:
    static Negative* Make(dng_host& host) {
        AutoPtr<Negative> negative(new Negative(host));
        negative->Initialize();
        return negative.Release();
    }

    // Installs image as the stage 3 image and returns the previous one.
    dng_image* ReplaceStage3(dng_image* image) {
        dng_image* previous = fStage3Image.Release();
        fStage3Image.Reset(image);
        return previous;
    }

private:
    explicit Negative(dng_host& host) : dng_negative(host) {}
};

// The per-channel clip dng_render applies to camera values: the brightest value
// each channel can reach while staying neutral under the image's white balance.
std::vector<real32> CameraWhite(const dng_negative& negative, uint32 planes) {
    std::vector<real32> white(planes, 1.0f);
    if (negative.IsMonochrome()) {
        return white;
    }
    AutoPtr<dng_color_spec> spec(negative.MakeColorSpec(dng_camera_profile_id()));
    if (negative.HasCameraNeutral()) {
        spec->SetWhiteXY(spec->NeutralToXY(negative.CameraNeutral()));
    } else if (negative.HasCameraWhiteXY()) {
        spec->SetWhiteXY(negative.CameraWhiteXY());
    } else {
        spec->SetWhiteXY(D55_xy_coord());
    }
    const dng_vector& cameraWhite = spec->CameraWhite();
    for (uint32 i = 0; i < std::min(planes, cameraWhite.Count()); ++i) {
        white[i] = static_cast<real32>(cameraWhite[i]);
    }
    return white;
}

// Copies a stage 3 image, clipping each channel at the camera white point (as
// dng_render would) and then multiplying by gain. Values are scaled above the
// stage 3 black level, which dng_render subtracts later.
class DarkenTask : public dng_filter_task {
public:
    DarkenTask(const dng_image& src, dng_image& dst, std::vector<real32> white, real32 gain, real32 blackLevel)
        : dng_filter_task("DarkenTask", src, dst), white_(std::move(white)), gain_(gain), black_(blackLevel) {
        // 16-bit images (the usual case) go through a lookup table per plane;
        // anything else is processed as float.
        if (src.PixelType() == ttShort) {
            tables_.resize(fSrcPlanes);
            for (uint32 plane = 0; plane < fSrcPlanes; ++plane) {
                tables_[plane].resize(65536);
                for (uint32 v = 0; v < 65536; ++v) {
                    const real32 out = Darken(static_cast<real32>(v) / 65535.0f, plane);
                    tables_[plane][v] = static_cast<uint16>(Round_uint32(out * 65535.0f));
                }
            }
        } else {
            fSrcPixelType = ttFloat;
            fDstPixelType = ttFloat;
        }
    }

    void ProcessArea(uint32, dng_pixel_buffer& src, dng_pixel_buffer& dst) override {
        const dng_rect area = dst.fArea;
        for (uint32 plane = 0; plane < fSrcPlanes; ++plane) {
            for (int32 row = area.t; row < area.b; ++row) {
                if (!tables_.empty()) {
                    const uint16* table = tables_[plane].data();
                    const uint16* s = src.ConstPixel_uint16(row, area.l, plane);
                    uint16* d = dst.DirtyPixel_uint16(row, area.l, plane);
                    for (uint32 col = 0; col < area.W(); ++col) {
                        d[col] = table[s[col]];
                    }
                } else {
                    const real32* s = src.ConstPixel_real32(row, area.l, plane);
                    real32* d = dst.DirtyPixel_real32(row, area.l, plane);
                    for (uint32 col = 0; col < area.W(); ++col) {
                        d[col] = Darken(s[col], plane);
                    }
                }
            }
        }
    }

private:
    real32 Darken(real32 x, uint32 plane) const {
        const real32 range = 1.0f - black_;
        const real32 value = Pin_real32(0.0f, (x - black_) / range, white_[plane]);
        return black_ + value * gain_ * range;
    }

    std::vector<real32> white_;
    real32 gain_;
    real32 black_;
    std::vector<std::vector<uint16>> tables_;
};

// For the lifetime of this object, the negative's stage 3 image is replaced by a
// copy darkened by gain (< 1).
class ScopedDarkenedStage3 {
public:
    ScopedDarkenedStage3(dng_host& host, Negative& negative, double gain) : negative_(negative) {
        const dng_image& stage3 = *negative.Stage3Image();
        AutoPtr<dng_image> darkened(host.Make_dng_image(stage3.Bounds(), stage3.Planes(), stage3.PixelType()));
        DarkenTask task(stage3, *darkened, CameraWhite(negative, stage3.Planes()), static_cast<real32>(gain),
                        static_cast<real32>(negative.Stage3BlackLevelNormalized()));
        host.PerformAreaTask(task, stage3.Bounds());
        original_ = negative.ReplaceStage3(darkened.Release());
    }

    ~ScopedDarkenedStage3() { delete negative_.ReplaceStage3(original_); }

    ScopedDarkenedStage3(const ScopedDarkenedStage3&) = delete;
    ScopedDarkenedStage3& operator=(const ScopedDarkenedStage3&) = delete;

private:
    Negative& negative_;
    dng_image* original_ = nullptr;
};

// Renders negative with exposure EV added to the file's baseline exposure.
// configure sets the remaining dng_render options (output space, size, ...).
//
// dng_render brightens with a linear gain, but darkens by bending the tone curve
// so that white stays white. To darken the way a lower ISO would, the camera data
// is scaled here instead and rendered as if it had been captured that way.
dng_image* RenderWithExposure(dng_host& host, Negative& negative, double exposure,
                              const std::function<void(dng_render&)>& configure) {
    std::optional<ScopedDarkenedStage3> darkened;
    if (exposure < 0.0) {
        darkened.emplace(host, negative, std::exp2(exposure));
        exposure = 0.0;
    }

    dng_render render(host, negative);
    configure(render);
    render.SetExposure(exposure);
    return render.Render();
}

// dng_host runs every area task (demosaic, color conversion, ...) on one thread.
// This host splits each task into horizontal bands and processes them on up to
// threads threads (0 = all cores).
class ThreadedHost : public dng_host {
public:
    explicit ThreadedHost(uint32 threads)
        : threads_(threads ? threads : std::max(1u, std::thread::hardware_concurrency())) {}

    dng_negative* Make_dng_negative() override { return Negative::Make(*this); }

    uint32 PerformAreaTaskThreads() override { return ThreadCount(); }

    void PerformAreaTask(dng_area_task& task, const dng_rect& area, dng_area_task_progress* progress) override {
        // Tasks size their per-thread buffers by kMaxMPThreads.
        uint32 threads = std::min({task.MaxThreads(), ThreadCount(), kMaxMPThreads});
        const uint64 minArea = std::max<uint64>(task.MinTaskArea(), 1);
        threads = static_cast<uint32>(std::min<uint64>(threads, area.W() * static_cast<uint64>(area.H()) / minArea));
        if (threads <= 1) {
            dng_host::PerformAreaTask(task, area, progress);
            return;
        }

        // Bands are whole multiples of the tile height, so each band's tiles line
        // up with those the single-threaded path would use. Several bands per
        // thread keep the cores busy when some bands are slower than others.
        const dng_point tileSize = task.FindTileSize(area);
        const int32 tileRows = (area.H() + tileSize.v - 1) / tileSize.v;
        const int32 bandCount = std::min<int32>(tileRows, static_cast<int32>(threads) * 4);
        threads = std::min<uint32>(threads, static_cast<uint32>(bandCount));
        auto band = [&](int32 i) {
            const int32 top = area.t + (tileRows * i / bandCount) * tileSize.v;
            const int32 bottom = std::min(area.b, area.t + (tileRows * (i + 1) / bandCount) * tileSize.v);
            return dng_rect(top, area.l, bottom, area.r);
        };

        task.Start(threads, area, tileSize, &Allocator(), Sniffer());

        std::atomic<int32> next{0};
        std::exception_ptr failure;
        std::mutex failureMutex;
        auto worker = [&](uint32 threadIndex) {
            try {
                for (int32 i = next++; i < bandCount; i = next++) {
                    task.ProcessOnThread(threadIndex, band(i), tileSize, Sniffer(), progress);
                }
            } catch (...) {
                next = bandCount;  // stop the other threads early
                std::scoped_lock lock(failureMutex);
                if (!failure) {
                    failure = std::current_exception();
                }
            }
        };
        {
            std::vector<std::jthread> pool;
            for (uint32 t = 1; t < threads; ++t) {
                pool.emplace_back(worker, t);
            }
            worker(0);
        }
        if (failure) {
            std::rethrow_exception(failure);
        }

        task.Finish(threads);
    }

private:
    uint32 ThreadCount() const { return threads_; }

    uint32 threads_;
};

}  // namespace

std::string DngErrorMessage(dng_error_code code) {
    switch (code) {
        case dng_error_memory:
            return "out of memory";
        case dng_error_bad_format:
            return "not a valid DNG file";
        case dng_error_matrix_math:
            return "invalid color matrix";
        case dng_error_open_file:
            return "could not open the file";
        case dng_error_read_file:
            return "could not read the file";
        case dng_error_end_of_file:
            return "file is truncated";
        case dng_error_file_is_damaged:
            return "file is damaged";
        case dng_error_image_too_big_dng:
            return "image is too large";
        case dng_error_unsupported_dng:
            return "unsupported DNG version or feature";
        default:
            return std::format("DNG SDK error {}", static_cast<int>(code));
    }
}

dng_host* NewThreadedHost(uint32_t threads) {
    return new ThreadedHost(threads);
}

namespace {

void AddExifInfo(const dng_exif& exif, std::vector<std::pair<std::string, std::string>>& info) {
    auto add = [&info](const char* label, std::string value) { info.emplace_back(label, std::move(value)); };
    if (exif.fMake.NotEmpty() || exif.fModel.NotEmpty()) {
        add("Camera", std::format("{} {}", exif.fMake.Get(), exif.fModel.Get()));
    }
    if (exif.fLensName.NotEmpty()) {
        add("Lens", exif.fLensName.Get());
    }
    if (exif.fISOSpeedRatings[0] != 0) {
        add("ISO", std::to_string(exif.fISOSpeedRatings[0]));
    }
    if (exif.fExposureTime.IsValid()) {
        const double t = exif.fExposureTime.As_real64();
        add("Exposure", t < 1.0 && t > 0.0 ? std::format("1/{:.0f} s", 1.0 / t) : std::format("{:.1f} s", t));
    }
    if (exif.fFNumber.IsValid()) {
        add("Aperture", std::format("f/{:.1f}", exif.fFNumber.As_real64()));
    }
    if (exif.fFocalLength.IsValid()) {
        add("Focal length", std::format("{:.0f} mm", exif.fFocalLength.As_real64()));
    }
    if (exif.fDateTimeOriginal.IsValid()) {
        const dng_date_time& dt = exif.fDateTimeOriginal.DateTime();
        add("Taken", std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", dt.fYear, dt.fMonth, dt.fDay, dt.fHour,
                                 dt.fMinute, dt.fSecond));
    }
}

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

CaptureInfo ReadCaptureInfo(dng_negative& negative, const dng_rect& rawBounds) {
    CaptureInfo c;
    c.rawWidth = rawBounds.W();
    c.rawHeight = rawBounds.H();
    const dng_exif* exif = negative.GetExif();
    if (!exif) {
        return c;
    }
    c.make = exif->fMake.Get();
    c.model = exif->fModel.Get();
    if (exif->fExposureTime.IsValid()) {
        c.exposureTime = exif->fExposureTime.As_real64();
    }
    if (exif->fFNumber.IsValid()) {
        c.fNumber = exif->fFNumber.As_real64();
    }
    c.iso = exif->fISOSpeedRatings[0];
    if (exif->fDateTimeOriginal.IsValid()) {
        const dng_date_time& dt = exif->fDateTimeOriginal.DateTime();
        // Subseconds are the digits after the decimal point: "411" means .411 s.
        const std::string sub = exif->fDateTimeOriginal.Subseconds().Get();
        double fraction = 0.0;
        if (!sub.empty() && std::ranges::all_of(sub, [](unsigned char ch) { return std::isdigit(ch); })) {
            fraction = std::stod("0." + sub);
        }
        c.hasTime = true;
        c.timeSeconds = static_cast<double>(DaysFromCivil(dt.fYear, dt.fMonth, dt.fDay)) * 86400.0 +
                        dt.fHour * 3600.0 + dt.fMinute * 60.0 + dt.fSecond + fraction;
        c.timeText = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", dt.fYear, dt.fMonth, dt.fDay, dt.fHour,
                                 dt.fMinute, dt.fSecond);
        if (!sub.empty()) {
            c.timeText += "." + sub;
        }
    }
    return c;
}

}  // namespace

std::optional<double> CameraExposureEv(const CaptureInfo& c) {
    if (c.exposureTime <= 0.0 || c.fNumber <= 0.0 || c.iso == 0) {
        return std::nullopt;
    }
    return std::log2(c.exposureTime) - 2.0 * std::log2(c.fNumber) + std::log2(c.iso / 100.0);
}

DngDocument::~DngDocument() = default;

std::expected<CaptureInfo, std::string> DngDocument::Probe(const std::filesystem::path& path) {
    return CatchErrors([&] {
        dng_host host;
        dng_file_stream stream(path.c_str());
        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);
        if (!info.IsValidDNG()) {
            ThrowBadFormat();
        }
        AutoPtr<dng_negative> negative(host.Make_dng_negative());
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);
        const dng_ifd& raw = *info.fIFD[info.fMainIndex];
        return ReadCaptureInfo(*negative, dng_rect(raw.fImageLength, raw.fImageWidth));
    });
}

std::expected<std::shared_ptr<DngDocument>, std::string> DngDocument::Open(const std::filesystem::path& path,
                                                                           uint32_t preferredSize, uint32_t threads) {
    return CatchErrors([&] {
        std::shared_ptr<DngDocument> doc(new DngDocument());
        doc->path_ = path;
        doc->host_ = std::make_unique<ThreadedHost>(threads);
        dng_host& host = *doc->host_;
        host.SetPreferredSize(preferredSize);

        dng_file_stream stream(path.c_str());
        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);
        if (!info.IsValidDNG()) {
            ThrowBadFormat();
        }

        doc->negative_.reset(host.Make_dng_negative());
        dng_negative& negative = *doc->negative_;
        negative.Parse(host, stream, info);
        negative.PostParse(host, stream, info);
        negative.ReadStage1Image(host, stream, info);
        if (info.fMaskIndex != -1) {
            negative.ReadTransparencyMask(host, stream, info);
        }
        negative.ValidateRawImageDigest(host);

        const dng_rect rawBounds = negative.Stage1Image()->Bounds();

        negative.SynchronizeMetadata();
        negative.BuildStage2Image(host);
        negative.BuildStage3Image(host);
        doc->capture_ = ReadCaptureInfo(negative, rawBounds);

        const uint32 version = info.fShared->fDNGVersion;
        auto& out = doc->info_;
        out.emplace_back("File", PathUtf8(path.filename()));
        out.emplace_back("Folder", PathUtf8(path.parent_path()));
        out.emplace_back("Raw size", std::format("{} x {}", rawBounds.W(), rawBounds.H()));
        out.emplace_back("DNG version", std::format("{}.{}.{}.{}", version >> 24, (version >> 16) & 0xFF,
                                                    (version >> 8) & 0xFF, version & 0xFF));
        if (const dng_exif* exif = negative.GetExif()) {
            AddExifInfo(*exif, out);
        }
        return doc;
    });
}

std::expected<RenderedImage, std::string> DngDocument::Render(const RenderSettings& settings, uint32_t maxSize) {
    return CatchErrors([&] {
        AutoPtr<dng_image> rendered(RenderWithExposure(*host_, static_cast<Negative&>(*negative_),
                                                       settings.exposure, [&](dng_render& render) {
                                                           render.SetFinalSpace(dng_space_sRGB::Get());
                                                           render.SetFinalPixelType(ttByte);
                                                           render.SetMaximumSize(maxSize);
                                                       }));
        rendered->Rotate(negative_->Orientation());

        const dng_rect bounds = rendered->Bounds();
        const uint32 planes = rendered->Planes();

        RenderedImage out;
        out.width = bounds.W();
        out.height = bounds.H();
        out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);

        // Read the rendered planes straight into the RGBA buffer, then fill in alpha
        // (and copy gray into G and B for monochrome DNGs).
        dng_pixel_buffer buffer(bounds, 0, std::min<uint32>(planes, 3), ttByte, pcInterleaved, out.rgba.data());
        buffer.fColStep = 4;
        buffer.fRowStep = static_cast<int32>(out.width) * 4;
        rendered->Get(buffer);
        for (size_t i = 0; i < out.rgba.size(); i += 4) {
            if (planes == 1) {
                out.rgba[i + 1] = out.rgba[i + 2] = out.rgba[i];
            }
            out.rgba[i + 3] = 255;
            const uint8_t r = out.rgba[i], g = out.rgba[i + 1], b = out.rgba[i + 2];
            ++out.histogram[0][r];
            ++out.histogram[1][g];
            ++out.histogram[2][b];
            const uint8_t lo = std::min({r, g, b}), hi = std::max({r, g, b});
            out.blackPixels += hi == 0;
            out.whitePixels += lo == 255;
            out.anyChannelAt0 += lo == 0;
            out.anyChannelAt255 += hi == 255;
        }
        return out;
    });
}

MeterImage DngDocument::Meter(uint32_t width) const {
    const dng_image& stage3 = *negative_->Stage3Image();
    const dng_rect crop = negative_->DefaultCropArea() & stage3.Bounds();
    const uint32 planes = stage3.Planes();

    MeterImage out;
    out.width = std::max(1u, std::min(width, crop.W()));
    out.height = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(out.width) * crop.H() / crop.W())));
    out.rgb.assign(static_cast<size_t>(out.width) * out.height * 3, 0.0f);
    std::vector<uint32_t> counts(static_cast<size_t>(out.width) * out.height, 0);

    // Average the source pixels that fall in each output cell, one row at a time.
    const float black = static_cast<float>(negative_->Stage3BlackLevelNormalized());
    const float scale = 1.0f / (1.0f - black);
    std::vector<float> row(static_cast<size_t>(crop.W()) * planes);
    for (int32 y = crop.t; y < crop.b; ++y) {
        dng_pixel_buffer buffer(dng_rect(y, crop.l, y + 1, crop.r), 0, planes, ttFloat, pcInterleaved, row.data());
        stage3.Get(buffer);
        const uint32_t ty = static_cast<uint32_t>(y - crop.t) * out.height / crop.H();
        for (uint32 x = 0; x < crop.W(); ++x) {
            const uint32_t tx = x * out.width / crop.W();
            const size_t cell = static_cast<size_t>(ty) * out.width + tx;
            for (uint32_t c = 0; c < 3; ++c) {
                const float v = row[static_cast<size_t>(x) * planes + std::min(c, planes - 1)];
                out.rgb[cell * 3 + c] += (v - black) * scale;
            }
            ++counts[cell];
        }
    }
    for (size_t cell = 0; cell < counts.size(); ++cell) {
        const float n = static_cast<float>(std::max(1u, counts[cell]));
        for (uint32_t c = 0; c < 3; ++c) {
            out.rgb[cell * 3 + c] /= n;
        }
    }
    return out;
}

bool IsDngPath(const std::filesystem::path& path) {
    std::string ext = PathUtf8(path.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".dng";
}
