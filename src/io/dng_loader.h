#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class dng_host;
class dng_negative;

// User adjustments applied when rendering. Defaults leave the image as shot.
struct RenderSettings {
    // EV, on top of the file's baseline exposure. Both directions are a linear gain
    // on the camera data, like changing ISO; brightening clips at white.
    double exposure = 0.0;

    bool operator==(const RenderSettings&) const = default;
};

// A render converted to 8-bit sRGB, ready for display.
struct RenderedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;  // width * height * 4 bytes, row-major, alpha = 255

    // Pixel counts for each 8-bit value of the R, G and B channels.
    std::array<std::array<uint32_t, 256>, 3> histogram{};

    // Pixels with all channels at 0 / 255, and with at least one channel there.
    uint64_t blackPixels = 0;
    uint64_t whitePixels = 0;
    uint64_t anyChannelAt0 = 0;
    uint64_t anyChannelAt255 = 0;
};

// Camera settings and identity read from the file's EXIF data.
struct CaptureInfo {
    std::string make;
    std::string model;
    double exposureTime = 0.0;  // seconds; 0 if unknown
    double fNumber = 0.0;       // 0 if unknown
    uint32_t iso = 0;           // 0 if unknown
    bool hasTime = false;
    double timeSeconds = 0.0;  // capture time as seconds since 1970 on the camera's clock, with subseconds
    std::string timeText;      // "2025-01-25 13:14:31.411"
    uint32_t rawWidth = 0;
    uint32_t rawHeight = 0;
};

// The camera's exposure on a scale where 1 s, f/1.0, ISO 100 is 0 EV:
// log2(t) - 2 log2(N) + log2(ISO / 100). Missing settings give nullopt.
std::optional<double> CameraExposureEv(const CaptureInfo& capture);

// Linear camera-space RGB after demosaicing, with 0 = black and 1 = clipping.
// Values are proportional to the light each pixel received.
struct MeterImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rgb;  // width * height * 3
};

// A decoded DNG (demosaiced, still in camera color space) that can be rendered
// repeatedly with different settings. Render() must not run on two threads at once
// for the same document; different documents are independent.
// dng_xmp_sdk::InitializeSDK() must have run before Open().
class DngDocument {
public:
    // preferredSize > 0 demosaics at a reduced size whose long side is about that
    // many pixels, which is several times faster and enough for previews and metering.
    // threads limits the decoder's threads (0 = all cores).
    static std::expected<std::shared_ptr<DngDocument>, std::string> Open(const std::filesystem::path& path,
                                                                         uint32_t preferredSize = 0,
                                                                         uint32_t threads = 0);

    // Reads a file's camera settings and raw size without decoding the image.
    static std::expected<CaptureInfo, std::string> Probe(const std::filesystem::path& path);

    ~DngDocument();
    DngDocument(const DngDocument&) = delete;
    DngDocument& operator=(const DngDocument&) = delete;

    // Renders with the DNG SDK's default pipeline. The long side of the result is
    // limited to maxSize pixels.
    std::expected<RenderedImage, std::string> Render(const RenderSettings& settings, uint32_t maxSize);

    // Box-downsamples the demosaiced image (within the default crop) to the given width.
    MeterImage Meter(uint32_t width) const;

    const std::filesystem::path& Path() const { return path_; }
    const CaptureInfo& Capture() const { return capture_; }

    // Label/value pairs shown in the info panel, in display order.
    const std::vector<std::pair<std::string, std::string>>& Info() const { return info_; }

private:
    DngDocument() = default;

    std::filesystem::path path_;
    CaptureInfo capture_;
    std::vector<std::pair<std::string, std::string>> info_;
    std::unique_ptr<dng_host> host_;  // owns the memory allocator negative_ uses
    std::unique_ptr<dng_negative> negative_;
};

bool IsDngPath(const std::filesystem::path& path);
