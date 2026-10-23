#include "processing/metering.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr float kDarkLimit = 0.002f;        // below this the ratio is dominated by noise
constexpr float kClipLimit = 0.9f;          // above this a channel is about to clip
constexpr double kMinValidFraction = 0.05;  // fewer usable pixels than this: fall back to percentiles

double Median(std::vector<double>& v) {
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(mid), v.end());
    return v[mid];
}

// Robust spread of v around center: 1.4826 * median absolute deviation, which
// equals the standard deviation for normally distributed values.
double Spread(std::vector<double> v, double center) {
    for (double& x : v) {
        x = std::abs(x - center);
    }
    return 1.4826 * Median(v);
}

float Luminance(const float* rgb) {
    return (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
}

bool Usable(const float* rgb) {
    return Luminance(rgb) >= kDarkLimit && std::max({rgb[0], rgb[1], rgb[2]}) <= kClipLimit;
}

}  // namespace

std::expected<MeterImage, std::string> MeterFile(const std::filesystem::path& path, uint32_t threads) {
    auto doc = DngDocument::Open(path, kMeterDecodeSize, threads);
    if (!doc) {
        return std::unexpected(doc.error());
    }
    return (*doc)->Meter(kMeterWidth);
}

// With equal sizes the median of per-pixel log ratios is used. Otherwise (or when
// almost nothing is usable in both) matching brightness percentiles are compared.
Comparison CompareMeters(const MeterImage& a, const MeterImage& b) {
    Comparison result;
    const size_t pixelsA = static_cast<size_t>(a.width) * a.height;
    const size_t pixelsB = static_cast<size_t>(b.width) * b.height;
    if (pixelsA == 0 || pixelsB == 0) {
        return result;
    }

    if (a.width == b.width && a.height == b.height) {
        std::vector<double> ratios;
        ratios.reserve(pixelsA);
        for (size_t i = 0; i < pixelsA; ++i) {
            const float* pa = &a.rgb[i * 3];
            const float* pb = &b.rgb[i * 3];
            if (Usable(pa) && Usable(pb)) {
                ratios.push_back(std::log2(Luminance(pb) / Luminance(pa)));
            }
        }
        result.validFraction = static_cast<double>(ratios.size()) / static_cast<double>(pixelsA);
        if (result.validFraction >= kMinValidFraction) {
            result.deltaEv = Median(ratios);
            result.spreadEv = Spread(ratios, result.deltaEv);
            return result;
        }
    }

    // Percentile matching: sort the usable luminances of each image and compare the
    // 10th to 90th percentiles, which only assumes the two show a similar scene.
    auto sortedLuminance = [](const MeterImage& img) {
        std::vector<double> lum;
        for (size_t i = 0; i < img.rgb.size(); i += 3) {
            if (Usable(&img.rgb[i])) {
                lum.push_back(Luminance(&img.rgb[i]));
            }
        }
        std::ranges::sort(lum);
        return lum;
    };
    const std::vector<double> la = sortedLuminance(a);
    const std::vector<double> lb = sortedLuminance(b);
    result.percentile = true;
    result.validFraction = static_cast<double>(std::min(la.size(), lb.size())) / static_cast<double>(pixelsA);
    if (la.size() < 16 || lb.size() < 16) {
        return result;
    }
    std::vector<double> ratios;
    for (int p = 10; p <= 90; ++p) {
        const double va = la[la.size() * p / 100];
        const double vb = lb[lb.size() * p / 100];
        ratios.push_back(std::log2(vb / va));
    }
    result.deltaEv = Median(ratios);
    result.spreadEv = Spread(ratios, result.deltaEv);
    return result;
}
