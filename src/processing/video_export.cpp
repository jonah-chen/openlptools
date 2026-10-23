#include "processing/video_export.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <expected>
#include <format>
#include <fstream>
#include <future>
#include <system_error>

#include "common/parallel.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kPreviewWidth = 480;  // widest the live preview gets

std::string Utf8(const fs::path& path) {
    const std::u8string s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

uint32_t Even(double v) {
    return static_cast<uint32_t>(std::max(2.0, std::round(v / 2.0) * 2.0));
}

// Which source samples each destination sample along one axis averages, with
// weights proportional to the overlap of the destination pixel's footprint.
struct Axis {
    std::vector<uint32_t> start;
    std::vector<uint32_t> count;
    std::vector<size_t> offset;  // into weights
    std::vector<float> weights;
};

Axis MakeAxis(double origin, double length, uint32_t sourceSize, uint32_t destSize) {
    Axis axis;
    axis.start.resize(destSize);
    axis.count.resize(destSize);
    axis.offset.resize(destSize);
    const double step = length / destSize;
    for (uint32_t d = 0; d < destSize; ++d) {
        const double s0 = origin + d * step;
        const double s1 = s0 + step;
        long long i0 = static_cast<long long>(std::floor(s0));
        long long i1 = static_cast<long long>(std::ceil(s1));
        i0 = std::clamp<long long>(i0, 0, sourceSize - 1);
        i1 = std::clamp<long long>(i1, i0 + 1, sourceSize);
        axis.start[d] = static_cast<uint32_t>(i0);
        axis.offset[d] = axis.weights.size();
        double sum = 0.0;
        for (long long i = i0; i < i1; ++i) {
            const double w = std::max(0.0, std::min(s1, static_cast<double>(i + 1)) - std::max(s0, static_cast<double>(i)));
            axis.weights.push_back(static_cast<float>(w));
            sum += w;
        }
        axis.count[d] = static_cast<uint32_t>(i1 - i0);
        if (sum <= 0.0) {
            // The footprint fell outside the image (rounding at the edge): take the nearest sample.
            axis.weights[axis.offset[d]] = 1.0f;
            axis.count[d] = 1;
            axis.weights.resize(axis.offset[d] + 1);
        } else {
            for (size_t k = axis.offset[d]; k < axis.weights.size(); ++k) {
                axis.weights[k] = static_cast<float>(axis.weights[k] / sum);
            }
        }
    }
    return axis;
}

template <typename T>
void ResampleRow(const T* src, const Axis& axis, float* out, float scale) {
    for (size_t d = 0; d < axis.start.size(); ++d) {
        float r = 0, g = 0, b = 0;
        const T* s = src + static_cast<size_t>(axis.start[d]) * 3;
        const float* w = axis.weights.data() + axis.offset[d];
        for (uint32_t k = 0; k < axis.count[d]; ++k, s += 3) {
            r += static_cast<float>(s[0]) * w[k];
            g += static_cast<float>(s[1]) * w[k];
            b += static_cast<float>(s[2]) * w[k];
        }
        out[d * 3] = r * scale;
        out[d * 3 + 1] = g * scale;
        out[d * 3 + 2] = b * scale;
    }
}

// Area-averages the source rectangle (origin and size in source pixels) into the
// destination rectangle of frame, converting colors to sRGB on the way.
void Resample(const TiffImage& image, double srcX, double srcY, double srcW, double srcH, RgbImage& frame,
              uint32_t dstX, uint32_t dstY, uint32_t dstW, uint32_t dstH, unsigned threads) {
    const Axis xAxis = MakeAxis(srcX, srcW, image.width, dstW);
    const Axis yAxis = MakeAxis(srcY, srcH, image.height, dstH);
    if (threads == 0) {
        threads = std::max(1u, std::thread::hardware_concurrency());
    }
    struct Scratch {
        std::vector<float> row, acc;
    };
    std::vector<Scratch> scratch(threads);
    const float scale = image.bitsPerSample == 8 ? 1.0f / 255.0f : 1.0f / 65535.0f;
    ParallelForEach(dstH, threads, [&](unsigned thread, size_t y) {
        Scratch& s = scratch[thread];
        s.row.resize(static_cast<size_t>(dstW) * 3);
        s.acc.assign(static_cast<size_t>(dstW) * 3, 0.0f);
        const float* wy = yAxis.weights.data() + yAxis.offset[y];
        for (uint32_t k = 0; k < yAxis.count[y]; ++k) {
            const size_t sy = yAxis.start[y] + k;
            const uint8_t* srcRow = image.pixels.data() + sy * image.RowBytes();
            if (image.bitsPerSample == 8) {
                ResampleRow(srcRow, xAxis, s.row.data(), scale);
            } else {
                ResampleRow(reinterpret_cast<const uint16_t*>(srcRow), xAxis, s.row.data(), scale);
            }
            for (size_t i = 0; i < s.acc.size(); ++i) {
                s.acc[i] += s.row[i] * wy[k];
            }
        }
        uint8_t* out = frame.rgb.data() + ((dstY + y) * static_cast<size_t>(frame.width) + dstX) * 3;
        image.color.Apply(s.acc.data(), out, dstW);
    });
}

// BT.709, limited range: Y 16-235, Cb/Cr 16-240.
std::vector<uint8_t> ToNv12(const RgbImage& frame, unsigned threads) {
    const uint32_t w = frame.width, h = frame.height;
    std::vector<uint8_t> nv12(static_cast<size_t>(w) * h * 3 / 2);
    uint8_t* yPlane = nv12.data();
    uint8_t* uvPlane = nv12.data() + static_cast<size_t>(w) * h;
    auto luma = [](float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; };
    auto clampByte = [](float v, float lo, float hi) {
        return static_cast<uint8_t>(std::clamp(std::round(v), lo, hi));
    };
    ParallelForEach(h / 2, threads, [&](unsigned, size_t pair) {
        const uint32_t y0 = static_cast<uint32_t>(pair) * 2;
        for (uint32_t dy = 0; dy < 2; ++dy) {
            const uint8_t* src = frame.rgb.data() + static_cast<size_t>(y0 + dy) * w * 3;
            uint8_t* dst = yPlane + static_cast<size_t>(y0 + dy) * w;
            for (uint32_t x = 0; x < w; ++x) {
                const float yv = luma(src[x * 3], src[x * 3 + 1], src[x * 3 + 2]);
                dst[x] = clampByte(16.0f + yv * (219.0f / 255.0f), 16.0f, 235.0f);
            }
        }
        const uint8_t* row0 = frame.rgb.data() + static_cast<size_t>(y0) * w * 3;
        const uint8_t* row1 = row0 + static_cast<size_t>(w) * 3;
        uint8_t* uv = uvPlane + static_cast<size_t>(pair) * w;
        for (uint32_t x = 0; x + 1 < w; x += 2) {
            float r = 0, g = 0, b = 0;
            for (const uint8_t* p : {row0 + x * 3, row0 + x * 3 + 3, row1 + x * 3, row1 + x * 3 + 3}) {
                r += p[0];
                g += p[1];
                b += p[2];
            }
            r *= 0.25f;
            g *= 0.25f;
            b *= 0.25f;
            const float yv = luma(r, g, b);
            uv[x] = clampByte(128.0f + (b - yv) * (224.0f / (1.8556f * 255.0f)), 16.0f, 240.0f);
            uv[x + 1] = clampByte(128.0f + (r - yv) * (224.0f / (1.5748f * 255.0f)), 16.0f, 240.0f);
        }
    });
    return nv12;
}

std::expected<std::vector<uint8_t>, std::string> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::unexpected(Utf8(path.filename()) + ": could not open the file");
    }
    const std::streamsize size = in.tellg();
    std::vector<uint8_t> data(static_cast<size_t>(std::max<std::streamsize>(size, 0)));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size)) {
        return std::unexpected(Utf8(path.filename()) + ": could not read the file");
    }
    return data;
}

}  // namespace

std::vector<fs::path> ListTiffs(const fs::path& folder) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec) && IsTiffPath(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

std::pair<uint32_t, uint32_t> VideoFrameSize(const VideoOptions& options, uint32_t photoWidth, uint32_t photoHeight) {
    const uint32_t width = Even(std::max<double>(16.0, options.width));
    double aspect = options.aspect;
    if (options.fit == FrameFit::Photo && photoWidth > 0 && photoHeight > 0) {
        aspect = static_cast<double>(photoWidth) / photoHeight;
    }
    if (!(aspect > 0.01) || !(aspect < 100.0)) {
        aspect = 16.0 / 9.0;
    }
    return {width, Even(width / aspect)};
}

double DefaultMegabits(uint32_t width, uint32_t height, uint32_t fps) {
    const double pixelsPerSecond = static_cast<double>(width) * height * std::max(1u, fps);
    const double reference = 3840.0 * 2160.0 * 30.0;  // 40 Mbit/s
    return std::clamp(40.0 * pixelsPerSecond / reference, 2.0, 200.0);
}

RgbImage FitFrame(const TiffImage& image, uint32_t width, uint32_t height, FrameFit fit, unsigned threads) {
    RgbImage frame;
    frame.width = width;
    frame.height = height;
    frame.rgb.assign(static_cast<size_t>(width) * height * 3, 0);
    if (image.width == 0 || image.height == 0 || width == 0 || height == 0) {
        return frame;
    }
    const double sw = image.width, sh = image.height;
    if (fit == FrameFit::Crop) {
        const double target = static_cast<double>(width) / height;
        double cropW = sw, cropH = sh;
        if (sw / sh > target) {
            cropW = sh * target;
        } else {
            cropH = sw / target;
        }
        Resample(image, (sw - cropW) / 2, (sh - cropH) / 2, cropW, cropH, frame, 0, 0, width, height, threads);
    } else {
        const double scale = std::min(width / sw, height / sh);
        const uint32_t dstW = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(sw * scale)), 1, width);
        const uint32_t dstH = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(sh * scale)), 1, height);
        Resample(image, 0, 0, sw, sh, frame, (width - dstW) / 2, (height - dstH) / 2, dstW, dstH, threads);
    }
    return frame;
}

VideoExportJob::VideoExportJob(std::vector<fs::path> files, VideoOptions options, uint32_t width, uint32_t height)
    : files_(std::move(files)), options_(std::move(options)), width_(width), height_(height) {}

VideoExportJob::~VideoExportJob() {
    Cancel();
}

void VideoExportJob::Start() {
    if (running_ || finished_) {
        return;
    }
    running_ = true;
    worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}

void VideoExportJob::Cancel() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

std::string VideoExportJob::Status() const {
    std::scoped_lock lock(mutex_);
    return status_;
}

std::string VideoExportJob::Encoder() const {
    std::scoped_lock lock(mutex_);
    return encoder_;
}

double VideoExportJob::Seconds() const {
    return seconds_;
}

uint64_t VideoExportJob::OutputBytes() const {
    return outputBytes_;
}

bool VideoExportJob::Preview(uint64_t& version, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba) const {
    std::scoped_lock lock(mutex_);
    if (version == previewVersion_ || previewRgba_.empty()) {
        return false;
    }
    version = previewVersion_;
    width = previewWidth_;
    height = previewHeight_;
    rgba = previewRgba_;
    return true;
}

void VideoExportJob::SetStatus(std::string text) {
    std::scoped_lock lock(mutex_);
    status_ = std::move(text);
}

void VideoExportJob::Run(std::stop_token stop) {
    const auto start = std::chrono::steady_clock::now();
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    std::string error;

    SetStatus("Starting the encoder");
    const uint32_t bitsPerSecond = static_cast<uint32_t>(std::clamp(options_.megabitsPerSecond, 0.1, 1000.0) * 1e6);
    auto writer = VideoWriter::Create(options_.output, width_, height_, options_.fps, bitsPerSecond, options_.codec);
    if (!writer) {
        error = writer.error();
    } else {
        std::scoped_lock lock(mutex_);
        encoder_ = (*writer)->EncoderDescription();
    }

    // The next file is read from disk while the current one is decoded.
    std::future<std::expected<std::vector<uint8_t>, std::string>> nextFile;
    if (error.empty() && !files_.empty()) {
        nextFile = std::async(std::launch::async, ReadFile, files_.front());
    }
    for (size_t i = 0; error.empty() && i < files_.size(); ++i) {
        if (stop.stop_requested()) {
            cancelled_ = true;
            break;
        }
        const std::string name = Utf8(files_[i].filename());
        SetStatus(std::format("Reading {}", name));
        auto bytes = nextFile.get();
        if (i + 1 < files_.size()) {
            nextFile = std::async(std::launch::async, ReadFile, files_[i + 1]);
        }
        if (!bytes) {
            error = bytes.error();
            break;
        }
        SetStatus(std::format("Decoding {}", name));
        RgbImage frame;
        {
            auto image = DecodeTiff(*bytes, name, threads);
            bytes->clear();
            bytes->shrink_to_fit();
            if (!image) {
                error = image.error();
                break;
            }
            frame = FitFrame(*image, width_, height_, options_.fit, threads);
        }
        const std::vector<uint8_t> nv12 = ToNv12(frame, threads);

        // Reduced copy for the dialog.
        {
            const uint32_t factor = std::max(1u, (frame.width + kPreviewWidth - 1) / kPreviewWidth);
            const uint32_t pw = std::max(1u, frame.width / factor), ph = std::max(1u, frame.height / factor);
            std::vector<uint8_t> rgba(static_cast<size_t>(pw) * ph * 4);
            for (uint32_t y = 0; y < ph; ++y) {
                const uint8_t* src = frame.rgb.data() + (static_cast<size_t>(y) * factor + factor / 2) * frame.width * 3;
                for (uint32_t x = 0; x < pw; ++x) {
                    const uint8_t* p = src + (static_cast<size_t>(x) * factor + factor / 2) * 3;
                    uint8_t* d = rgba.data() + (static_cast<size_t>(y) * pw + x) * 4;
                    d[0] = p[0];
                    d[1] = p[1];
                    d[2] = p[2];
                    d[3] = 255;
                }
            }
            std::scoped_lock lock(mutex_);
            previewWidth_ = pw;
            previewHeight_ = ph;
            previewRgba_ = std::move(rgba);
            ++previewVersion_;
        }

        SetStatus(std::format("Encoding {}", name));
        if (auto written = (*writer)->WriteFrame(nv12.data(), nv12.size()); !written) {
            error = written.error();
            break;
        }
        completed_ = static_cast<int>(i + 1);
    }
    if (nextFile.valid()) {
        nextFile.wait();
    }

    if (!error.empty()) {
        writer = {};  // drops the half-written file
        std::error_code ec;
        fs::remove(options_.output, ec);
        failed_ = true;
        SetStatus("Failed: " + error);
    } else {
        SetStatus("Finishing the file");
        if (auto finished = (*writer)->Finish(); !finished) {
            failed_ = true;
            SetStatus("Failed: " + finished.error());
        } else {
            std::error_code ec;
            outputBytes_ = fs::file_size(options_.output, ec);
            const int frames = completed_;
            const double seconds = static_cast<double>(frames) / std::max(1u, options_.fps);
            if (cancelled_) {
                SetStatus(std::format("Cancelled after {} of {} frames; the {:.1f} s of video so far were kept", frames,
                                      files_.size(), seconds));
            } else {
                SetStatus(std::format("Wrote {} frames, {:.1f} s of video at {} fps", frames, seconds, options_.fps));
            }
        }
    }
    writer = {};
    seconds_ = elapsed();
    finished_ = true;
    running_ = false;
}
