#pragma once

// Metering: how much brighter one raw frame is than another. The DNG is demosaiced
// at reduced size, averaged down to a small linear image, and the two images are
// compared pixel by pixel (the median of the log ratios, which ignores the minority
// of pixels where something moved) or, failing that, by brightness percentiles.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include "io/dng_loader.h"

inline constexpr uint32_t kMeterDecodeSize = 1024;  // long side of the reduced demosaic
inline constexpr uint32_t kMeterWidth = 160;        // width of the metering image

// Result of metering one frame against another: how much brighter it is.
struct Comparison {
    double deltaEv = 0.0;        // log2 of the brightness ratio (image over reference)
    double spreadEv = 0.0;       // robust spread of the per-pixel ratios; small means a clean match
    double validFraction = 0.0;  // share of pixels usable in both frames (not clipped, not black)
    bool percentile = false;     // true if the images could not be compared pixel by pixel
};

// Decodes and meters a file. threads limits the decoder (0 = all cores).
std::expected<MeterImage, std::string> MeterFile(const std::filesystem::path& path, uint32_t threads);

// Compares image against reference.
Comparison CompareMeters(const MeterImage& reference, const MeterImage& image);
