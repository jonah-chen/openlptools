#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

// What a TIFF holds, read from its tags without decoding the pixels.
struct TiffInfo {
    uint32_t width = 0;
    uint32_t height = 0;  // after the Orientation tag is applied
    uint16_t bitsPerSample = 0;
    uint16_t samplesPerPixel = 0;
    uint16_t compression = 1;
    uint16_t photometric = 2;
    uint16_t orientation = 1;
    std::string compressionName;  // "none", "LZW", "ZIP", ...
    std::string colorProfile;     // the embedded ICC profile's name, or "none"
    std::string software;         // the program that wrote the file, if recorded
};

// Converts the encoded RGB values of a file (0..1) to 8-bit sRGB. Built from the
// file's ICC profile when it is a matrix/TRC profile (sRGB, Adobe RGB, ProPhoto,
// Display P3, ...); files without a usable profile are taken to be sRGB already.
struct ColorTransform {
    bool identity = true;     // the file is sRGB: values pass straight through
    std::string description;  // the profile's name and whether it is applied
    std::array<std::vector<float>, 3> toLinear;  // per channel: encoded value -> linear light, kLutSize entries over [0, 1]
    std::array<float, 9> matrix{};               // linear profile RGB -> linear sRGB, row-major

    static constexpr size_t kLutSize = 4097;

    // Converts `pixels` RGB triples in rgb (encoded, 0..1, clamped) to 8-bit sRGB.
    void Apply(const float* rgb, uint8_t* out, size_t pixels) const;
};

// A decoded TIFF: 8- or 16-bit RGB in the file's color encoding, with the
// Orientation tag applied. 16-bit samples are in host byte order.
struct TiffImage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t bitsPerSample = 8;   // 8 or 16
    std::vector<uint8_t> pixels;  // width * height * 3 samples, row-major
    ColorTransform color;
    TiffInfo info;

    size_t RowBytes() const { return static_cast<size_t>(width) * 3 * (bitsPerSample / 8); }
};

// Reads the tags of a TIFF's first image.
std::expected<TiffInfo, std::string> ProbeTiff(const std::filesystem::path& path);

// Decodes a TIFF's first image. Supports 8- and 16-bit RGB and grayscale, chunky
// layout, in strips or tiles, uncompressed or LZW / ZIP (Deflate) / PackBits
// compressed, with the horizontal predictor. Chunks are decoded on up to `threads`
// threads (0 = one per core).
std::expected<TiffImage, std::string> LoadTiff(const std::filesystem::path& path, unsigned threads = 0);

// Same, from the file's bytes already in memory. `name` is used in error messages.
std::expected<TiffImage, std::string> DecodeTiff(const std::vector<uint8_t>& file, const std::string& name,
                                                 unsigned threads = 0);

bool IsTiffPath(const std::filesystem::path& path);
