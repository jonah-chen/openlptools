#include "io/tiff_loader.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>

#include "common/parallel.h"
#include "zlib.h"

namespace fs = std::filesystem;

namespace {

std::string Utf8(const fs::path& path) {
    const std::u8string s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

class Error : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The data needed lies beyond the bytes read so far.
class Truncated : public Error {
    using Error::Error;
};

// TIFF tags used here.
enum Tag : uint16_t {
    kImageWidth = 256,
    kImageLength = 257,
    kBitsPerSample = 258,
    kCompression = 259,
    kPhotometric = 262,
    kStripOffsets = 273,
    kOrientation = 274,
    kSamplesPerPixel = 277,
    kRowsPerStrip = 278,
    kStripByteCounts = 279,
    kPlanarConfig = 284,
    kSoftware = 305,
    kPredictor = 317,
    kTileWidth = 322,
    kTileLength = 323,
    kTileOffsets = 324,
    kTileByteCounts = 325,
    kSampleFormat = 339,
    kIccProfile = 34675,
};

enum Compression : uint16_t {
    kUncompressed = 1,
    kLzw = 5,
    kJpegOld = 6,
    kJpeg = 7,
    kAdobeDeflate = 8,
    kPackBits = 32773,
    kDeflate = 32946,
};

enum Photometric : uint16_t {
    kWhiteIsZero = 0,
    kBlackIsZero = 1,
    kRgb = 2,
    kPalette = 3,
    kSeparated = 5,  // CMYK
    kYCbCr = 6,
    kCieLab = 8,
};

const char* CompressionName(uint16_t c) {
    switch (c) {
        case kUncompressed: return "none";
        case kLzw: return "LZW";
        case kJpegOld:
        case kJpeg: return "JPEG";
        case kAdobeDeflate:
        case kDeflate: return "ZIP";
        case kPackBits: return "PackBits";
        default: return "unknown";
    }
}

// Bounds-checked access to the file in its byte order.
struct Reader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool littleEndian = true;

    void Check(size_t offset, size_t length) const {
        if (offset > size || length > size - offset) {
            throw Truncated("the file is truncated");
        }
    }
    uint8_t U8(size_t offset) const {
        Check(offset, 1);
        return data[offset];
    }
    uint16_t U16(size_t offset) const {
        Check(offset, 2);
        return littleEndian ? static_cast<uint16_t>(data[offset] | data[offset + 1] << 8)
                            : static_cast<uint16_t>(data[offset] << 8 | data[offset + 1]);
    }
    uint32_t U32(size_t offset) const {
        Check(offset, 4);
        return littleEndian ? static_cast<uint32_t>(data[offset]) | static_cast<uint32_t>(data[offset + 1]) << 8 |
                                  static_cast<uint32_t>(data[offset + 2]) << 16 |
                                  static_cast<uint32_t>(data[offset + 3]) << 24
                            : static_cast<uint32_t>(data[offset]) << 24 | static_cast<uint32_t>(data[offset + 1]) << 16 |
                                  static_cast<uint32_t>(data[offset + 2]) << 8 | static_cast<uint32_t>(data[offset + 3]);
    }
};

struct Entry {
    uint16_t type = 0;
    uint32_t count = 0;
    size_t valueOffset = 0;  // where the values are, inline in the entry or elsewhere
};

size_t TypeSize(uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;    // BYTE, ASCII, SBYTE, UNDEFINED
        case 3: case 8: return 2;                    // SHORT, SSHORT
        case 4: case 9: case 11: return 4;           // LONG, SLONG, FLOAT
        case 5: case 10: case 12: return 8;          // RATIONAL, SRATIONAL, DOUBLE
        case 16: case 17: case 18: return 8;         // LONG8, SLONG8, IFD8 (BigTIFF)
        default: return 0;
    }
}

using Ifd = std::map<uint16_t, Entry>;

Ifd ReadIfd(const Reader& r, size_t offset) {
    Ifd entries;
    const uint16_t n = r.U16(offset);
    for (uint16_t i = 0; i < n; ++i) {
        const size_t base = offset + 2 + static_cast<size_t>(i) * 12;
        Entry e;
        const uint16_t tag = r.U16(base);
        e.type = r.U16(base + 2);
        e.count = r.U32(base + 4);
        const size_t bytes = TypeSize(e.type) * static_cast<size_t>(e.count);
        e.valueOffset = bytes <= 4 ? base + 8 : r.U32(base + 8);
        entries[tag] = e;
    }
    return entries;
}

uint32_t ReadUInt(const Reader& r, const Entry& e, size_t index) {
    switch (e.type) {
        case 1: return r.U8(e.valueOffset + index);
        case 3: return r.U16(e.valueOffset + index * 2);
        case 4: return r.U32(e.valueOffset + index * 4);
        default: throw Error("a tag has an unexpected type");
    }
}

std::optional<uint32_t> GetUInt(const Reader& r, const Ifd& ifd, uint16_t tag) {
    auto it = ifd.find(tag);
    if (it == ifd.end() || it->second.count == 0) {
        return std::nullopt;
    }
    return ReadUInt(r, it->second, 0);
}

uint32_t GetUInt(const Reader& r, const Ifd& ifd, uint16_t tag, uint32_t fallback) {
    return GetUInt(r, ifd, tag).value_or(fallback);
}

std::vector<uint32_t> GetUInts(const Reader& r, const Ifd& ifd, uint16_t tag) {
    std::vector<uint32_t> values;
    auto it = ifd.find(tag);
    if (it == ifd.end()) {
        return values;
    }
    values.reserve(it->second.count);
    for (uint32_t i = 0; i < it->second.count; ++i) {
        values.push_back(ReadUInt(r, it->second, i));
    }
    return values;
}

std::string GetAscii(const Reader& r, const Ifd& ifd, uint16_t tag) {
    auto it = ifd.find(tag);
    if (it == ifd.end() || it->second.type != 2 || it->second.count == 0) {
        return {};
    }
    r.Check(it->second.valueOffset, it->second.count);
    std::string s(reinterpret_cast<const char*>(r.data + it->second.valueOffset), it->second.count);
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

// ---- Decompression -----------------------------------------------------------

void Inflate(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    if (srcLen > UINT32_MAX || dstLen > UINT32_MAX) {
        throw Error("a compressed strip is too large");
    }
    z_stream s{};
    s.next_in = const_cast<Bytef*>(src);
    s.avail_in = static_cast<uInt>(srcLen);
    s.next_out = dst;
    s.avail_out = static_cast<uInt>(dstLen);
    if (inflateInit(&s) != Z_OK) {
        throw Error("zlib could not be initialized");
    }
    const int rc = inflate(&s, Z_FINISH);
    const size_t produced = s.total_out;
    inflateEnd(&s);
    if (rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR) {
        throw Error("corrupt ZIP data");
    }
    if (produced < dstLen) {
        throw Error("a ZIP strip is shorter than expected");
    }
}

// TIFF LZW: MSB-first codes of 9 to 12 bits, with the code width growing one code
// early ("early change"), as every writer since TIFF 5.0 produces.
void DecodeLzw(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    constexpr int kClear = 256;
    constexpr int kEnd = 257;
    constexpr int kTableSize = 4096;
    std::vector<uint16_t> prefix(kTableSize);
    std::vector<uint8_t> suffix(kTableSize);
    std::vector<uint16_t> length(kTableSize);
    std::vector<uint8_t> firstByte(kTableSize);
    for (int i = 0; i < 256; ++i) {
        suffix[i] = firstByte[i] = static_cast<uint8_t>(i);
        length[i] = 1;
    }
    std::vector<uint8_t> text(kTableSize);

    size_t bitPos = 0;
    const size_t bitEnd = srcLen * 8;
    auto read = [&](int bits) -> int {
        if (bitPos + static_cast<size_t>(bits) > bitEnd) {
            return -1;
        }
        const size_t byte = bitPos >> 3;
        const int shift = static_cast<int>(bitPos & 7);
        uint32_t v = static_cast<uint32_t>(src[byte]) << 16;
        if (byte + 1 < srcLen) {
            v |= static_cast<uint32_t>(src[byte + 1]) << 8;
        }
        if (byte + 2 < srcLen) {
            v |= src[byte + 2];
        }
        bitPos += static_cast<size_t>(bits);
        return static_cast<int>((v >> (24 - shift - bits)) & ((1u << bits) - 1));
    };
    // Appends the table entry's string, cut short at the end of the output.
    size_t out = 0;
    auto write = [&](int code) {
        const size_t len = length[code];
        int c = code;
        for (size_t k = len; k-- > 0;) {
            text[k] = suffix[c];
            c = prefix[c];
        }
        const size_t n = std::min(len, dstLen - out);
        std::memcpy(dst + out, text.data(), n);
        out += n;
    };

    int nextCode = 258;
    int codeBits = 9;
    int prev = -1;
    while (out < dstLen) {
        const int code = read(codeBits);
        if (code < 0 || code == kEnd) {
            break;
        }
        if (code == kClear) {
            nextCode = 258;
            codeBits = 9;
            prev = -1;
            continue;
        }
        int first;
        if (code < nextCode) {
            write(code);
            first = firstByte[code];
        } else if (code == nextCode && prev >= 0) {
            first = firstByte[prev];
            write(prev);
            if (out < dstLen) {
                dst[out++] = static_cast<uint8_t>(first);
            }
        } else {
            throw Error("corrupt LZW data");
        }
        if (prev >= 0 && nextCode < kTableSize) {
            prefix[nextCode] = static_cast<uint16_t>(prev);
            suffix[nextCode] = static_cast<uint8_t>(first);
            length[nextCode] = static_cast<uint16_t>(length[prev] + 1);
            firstByte[nextCode] = firstByte[prev];
            ++nextCode;
        }
        if (nextCode >= (1 << codeBits) - 1 && codeBits < 12) {
            ++codeBits;
        }
        prev = code;
    }
    if (out < dstLen) {
        throw Error("an LZW strip is shorter than expected");
    }
}

void DecodePackBits(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    size_t in = 0, out = 0;
    while (out < dstLen && in < srcLen) {
        const int n = static_cast<int8_t>(src[in++]);
        if (n >= 0) {
            const size_t count = std::min<size_t>(static_cast<size_t>(n) + 1, dstLen - out);
            if (in + count > srcLen) {
                throw Error("corrupt PackBits data");
            }
            std::memcpy(dst + out, src + in, count);
            in += static_cast<size_t>(n) + 1;
            out += count;
        } else if (n != -128) {
            if (in >= srcLen) {
                throw Error("corrupt PackBits data");
            }
            const size_t count = std::min<size_t>(static_cast<size_t>(1 - n), dstLen - out);
            std::memset(dst + out, src[in++], count);
            out += count;
        }
    }
    if (out < dstLen) {
        throw Error("a PackBits strip is shorter than expected");
    }
}

// Undoes horizontal differencing (Predictor = 2) on one row.
void UndoPredictor8(uint8_t* row, size_t pixels, size_t samples) {
    for (size_t i = samples; i < pixels * samples; ++i) {
        row[i] = static_cast<uint8_t>(row[i] + row[i - samples]);
    }
}

void UndoPredictor16(uint16_t* row, size_t pixels, size_t samples) {
    for (size_t i = samples; i < pixels * samples; ++i) {
        row[i] = static_cast<uint16_t>(row[i] + row[i - samples]);
    }
}

// ---- ICC profiles --------------------------------------------------------------

struct BigEndian {
    const uint8_t* data;
    size_t size;
    void Check(size_t offset, size_t length) const {
        if (offset > size || length > size - offset) {
            throw Error("truncated ICC profile");
        }
    }
    uint16_t U16(size_t o) const {
        Check(o, 2);
        return static_cast<uint16_t>(data[o] << 8 | data[o + 1]);
    }
    uint32_t U32(size_t o) const {
        Check(o, 4);
        return static_cast<uint32_t>(data[o]) << 24 | static_cast<uint32_t>(data[o + 1]) << 16 |
               static_cast<uint32_t>(data[o + 2]) << 8 | static_cast<uint32_t>(data[o + 3]);
    }
    double S15Fixed16(size_t o) const { return static_cast<int32_t>(U32(o)) / 65536.0; }
    std::string Signature(size_t o) const {
        Check(o, 4);
        return std::string(reinterpret_cast<const char*>(data + o), 4);
    }
};

struct IccTag {
    size_t offset = 0;
    size_t size = 0;
};

// A tone curve sampled into a lookup table over the encoded range [0, 1].
using Curve = std::vector<float>;

Curve IdentityCurve() {
    Curve c(ColorTransform::kLutSize);
    for (size_t i = 0; i < c.size(); ++i) {
        c[i] = static_cast<float>(i) / static_cast<float>(c.size() - 1);
    }
    return c;
}

Curve ParseCurve(const BigEndian& p, const IccTag& tag) {
    const std::string type = p.Signature(tag.offset);
    Curve curve(ColorTransform::kLutSize);
    const auto x = [&](size_t i) { return static_cast<double>(i) / static_cast<double>(curve.size() - 1); };
    if (type == "curv") {
        const uint32_t n = p.U32(tag.offset + 8);
        if (n == 0) {
            return IdentityCurve();
        }
        if (n == 1) {
            const double gamma = p.U16(tag.offset + 12) / 256.0;
            for (size_t i = 0; i < curve.size(); ++i) {
                curve[i] = static_cast<float>(std::pow(x(i), gamma));
            }
            return curve;
        }
        p.Check(tag.offset + 12, static_cast<size_t>(n) * 2);
        for (size_t i = 0; i < curve.size(); ++i) {
            const double pos = x(i) * (n - 1);
            const size_t k = std::min<size_t>(static_cast<size_t>(pos), n - 2);
            const double t = pos - static_cast<double>(k);
            const double a = p.U16(tag.offset + 12 + k * 2) / 65535.0;
            const double b = p.U16(tag.offset + 12 + (k + 1) * 2) / 65535.0;
            curve[i] = static_cast<float>(a + (b - a) * t);
        }
        return curve;
    }
    if (type == "para") {
        const uint16_t function = p.U16(tag.offset + 8);
        double g = 1, a = 1, b = 0, c = 0, d = 0, e = 0, f = 0;
        const int counts[] = {1, 3, 4, 5, 7};
        if (function > 4) {
            throw Error("unknown parametric curve");
        }
        double* params[] = {&g, &a, &b, &c, &d, &e, &f};
        for (int i = 0; i < counts[function]; ++i) {
            *params[i] = p.S15Fixed16(tag.offset + 12 + static_cast<size_t>(i) * 4);
        }
        for (size_t i = 0; i < curve.size(); ++i) {
            const double v = x(i);
            double y;
            switch (function) {
                case 0: y = std::pow(v, g); break;
                case 1: y = v >= -b / a ? std::pow(a * v + b, g) : 0.0; break;
                case 2: y = v >= -b / a ? std::pow(a * v + b, g) + c : c; break;
                case 3: y = v >= d ? std::pow(a * v + b, g) : c * v; break;
                default: y = v >= d ? std::pow(a * v + b, g) + e : c * v + f; break;
            }
            curve[i] = static_cast<float>(std::clamp(y, 0.0, 1.0));
        }
        return curve;
    }
    throw Error("unknown curve type in ICC profile");
}

std::string ParseDescription(const BigEndian& p, const IccTag& tag) {
    const std::string type = p.Signature(tag.offset);
    std::string text;
    if (type == "desc") {
        const uint32_t n = p.U32(tag.offset + 8);
        p.Check(tag.offset + 12, n);
        text.assign(reinterpret_cast<const char*>(p.data + tag.offset + 12), n);
    } else if (type == "mluc") {
        const uint32_t records = p.U32(tag.offset + 8);
        if (records > 0) {
            const uint32_t length = p.U32(tag.offset + 20);
            const uint32_t offset = p.U32(tag.offset + 24);
            p.Check(tag.offset + offset, length);
            for (uint32_t i = 0; i + 1 < length; i += 2) {
                const uint16_t ch = p.U16(tag.offset + offset + i);
                text += ch < 128 ? static_cast<char>(ch) : '?';
            }
        }
    }
    while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

constexpr std::array<double, 9> kXyzD50ToSrgb = {3.1338561, -1.6168667, -0.4906146,   // Bradford-adapted sRGB
                                                 -0.9787684, 1.9161415, 0.0334540,
                                                 0.0719453, -0.2289914, 1.4052427};
constexpr std::array<double, 9> kSrgbToXyzD50 = {0.4360747, 0.3850649, 0.1430804,
                                                 0.2225045, 0.7168786, 0.0606169,
                                                 0.0139322, 0.0971045, 0.7141733};

double SrgbToLinear(double v) {
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double LinearToSrgb(double v) {
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

// Builds the file's color transform from its ICC profile; `profile` may be empty.
ColorTransform MakeColorTransform(const uint8_t* profile, size_t size) {
    ColorTransform t;
    if (size == 0) {
        t.description = "none (treated as sRGB)";
        return t;
    }
    const BigEndian p{profile, size};
    std::string name;
    try {
        if (size < 132) {
            throw Error("truncated ICC profile");
        }
        const std::string space = p.Signature(16);
        const std::string pcs = p.Signature(20);
        std::map<std::string, IccTag> tags;
        const uint32_t count = p.U32(128);
        for (uint32_t i = 0; i < count; ++i) {
            const size_t base = 132 + static_cast<size_t>(i) * 12;
            IccTag tag{p.U32(base + 4), p.U32(base + 8)};
            p.Check(tag.offset, tag.size);
            tags[p.Signature(base)] = tag;
        }
        if (auto d = tags.find("desc"); d != tags.end()) {
            name = ParseDescription(p, d->second);
        }
        if (name.empty()) {
            name = "unnamed profile";
        }
        const char* needed[] = {"rXYZ", "gXYZ", "bXYZ", "rTRC", "gTRC", "bTRC"};
        const bool matrixTrc = space == "RGB " && pcs == "XYZ " &&
                               std::ranges::all_of(needed, [&](const char* s) { return tags.contains(s); });
        if (!matrixTrc) {
            t.description = name + " (not a matrix profile; treated as sRGB)";
            return t;
        }

        // Colorants are the columns of the RGB -> XYZ (D50) matrix.
        std::array<double, 9> toXyz{};
        const char* colorants[] = {"rXYZ", "gXYZ", "bXYZ"};
        for (int c = 0; c < 3; ++c) {
            const IccTag& tag = tags[colorants[c]];
            for (int row = 0; row < 3; ++row) {
                toXyz[static_cast<size_t>(row) * 3 + static_cast<size_t>(c)] =
                    p.S15Fixed16(tag.offset + 8 + static_cast<size_t>(row) * 4);
            }
        }
        std::array<double, 9> full{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                for (int k = 0; k < 3; ++k) {
                    full[static_cast<size_t>(r) * 3 + c] +=
                        kXyzD50ToSrgb[static_cast<size_t>(r) * 3 + k] * toXyz[static_cast<size_t>(k) * 3 + c];
                }
            }
        }
        const char* curves[] = {"rTRC", "gTRC", "bTRC"};
        for (int c = 0; c < 3; ++c) {
            t.toLinear[static_cast<size_t>(c)] = ParseCurve(p, tags[curves[c]]);
        }

        // Close enough to sRGB to skip the conversion?
        bool srgb = true;
        for (size_t i = 0; i < 9; ++i) {
            srgb = srgb && std::abs(toXyz[i] - kSrgbToXyzD50[i]) < 0.01;
        }
        for (size_t c = 0; c < 3 && srgb; ++c) {
            const Curve& curve = t.toLinear[c];
            for (size_t i = 0; i < curve.size(); i += 256) {
                const double x = static_cast<double>(i) / static_cast<double>(curve.size() - 1);
                srgb = srgb && std::abs(curve[i] - SrgbToLinear(x)) < 0.004;
            }
        }
        if (srgb) {
            t.description = name;
            return t;
        }
        t.identity = false;
        t.description = name + " (converted to sRGB)";
        for (size_t i = 0; i < 9; ++i) {
            t.matrix[i] = static_cast<float>(full[i]);
        }
        return t;
    } catch (const Error& e) {
        t = ColorTransform{};
        t.description = (name.empty() ? std::string("unreadable profile") : name) + " (" + e.what() + "; treated as sRGB)";
        return t;
    }
}

// ---- Decoding ------------------------------------------------------------------

struct Header {
    Reader reader;
    Ifd ifd;
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t bits = 8;
    uint16_t samples = 3;
    uint16_t compression = kUncompressed;
    uint16_t photometric = kRgb;
    uint16_t orientation = 1;
    uint16_t predictor = 1;
    bool tiled = false;
};

Header ParseHeader(const uint8_t* data, size_t size) {
    Header h;
    if (size < 8 || !((data[0] == 'I' && data[1] == 'I') || (data[0] == 'M' && data[1] == 'M'))) {
        throw Error("not a TIFF file");
    }
    h.reader = Reader{data, size, data[0] == 'I'};
    const uint16_t magic = h.reader.U16(2);
    if (magic == 43) {
        throw Error("BigTIFF files are not supported");
    }
    if (magic != 42) {
        throw Error("not a TIFF file");
    }
    h.ifd = ReadIfd(h.reader, h.reader.U32(4));
    const Reader& r = h.reader;
    h.width = GetUInt(r, h.ifd, kImageWidth, 0);
    h.height = GetUInt(r, h.ifd, kImageLength, 0);
    if (h.width == 0 || h.height == 0) {
        throw Error("the image has no size");
    }
    const std::vector<uint32_t> bits = GetUInts(r, h.ifd, kBitsPerSample);
    h.bits = bits.empty() ? 1 : static_cast<uint16_t>(bits.front());
    for (uint32_t b : bits) {
        if (b != h.bits) {
            throw Error("channels with different bit depths are not supported");
        }
    }
    h.samples = static_cast<uint16_t>(GetUInt(r, h.ifd, kSamplesPerPixel, 1));
    h.compression = static_cast<uint16_t>(GetUInt(r, h.ifd, kCompression, kUncompressed));
    h.photometric = static_cast<uint16_t>(GetUInt(r, h.ifd, kPhotometric, h.samples >= 3 ? kRgb : kBlackIsZero));
    h.orientation = static_cast<uint16_t>(GetUInt(r, h.ifd, kOrientation, 1));
    if (h.orientation < 1 || h.orientation > 8) {
        h.orientation = 1;
    }
    h.predictor = static_cast<uint16_t>(GetUInt(r, h.ifd, kPredictor, 1));
    h.tiled = h.ifd.contains(kTileOffsets);
    return h;
}

// Checks that the pixel format is one this decoder handles.
void CheckSupported(const Header& h) {
    if (h.bits != 8 && h.bits != 16) {
        throw Error(std::format("{}-bit samples are not supported (only 8 and 16)", h.bits));
    }
    const uint32_t sampleFormat = GetUInt(h.reader, h.ifd, kSampleFormat, 1);
    if (sampleFormat != 1 && sampleFormat != 4) {
        throw Error(sampleFormat == 3 ? "floating point samples are not supported" : "signed samples are not supported");
    }
    switch (h.photometric) {
        case kRgb:
            if (h.samples < 3) {
                throw Error("an RGB image needs at least 3 samples per pixel");
            }
            break;
        case kWhiteIsZero:
        case kBlackIsZero:
            break;
        case kPalette: throw Error("palette (indexed color) images are not supported");
        case kSeparated: throw Error("CMYK images are not supported");
        case kYCbCr: throw Error("YCbCr images are not supported");
        case kCieLab: throw Error("CIE Lab images are not supported");
        default: throw Error(std::format("photometric interpretation {} is not supported", h.photometric));
    }
    switch (h.compression) {
        case kUncompressed:
        case kLzw:
        case kAdobeDeflate:
        case kDeflate:
        case kPackBits:
            break;
        case kJpegOld:
        case kJpeg: throw Error("JPEG-compressed TIFFs are not supported");
        default: throw Error(std::format("compression scheme {} is not supported", h.compression));
    }
    if (GetUInt(h.reader, h.ifd, kPlanarConfig, 1) != 1) {
        throw Error("planar (separate channel) layout is not supported");
    }
    if (h.predictor != 1 && h.predictor != 2) {
        throw Error(h.predictor == 3 ? "the floating point predictor is not supported"
                                     : std::format("predictor {} is not supported", h.predictor));
    }
}

TiffInfo MakeInfo(const Header& h) {
    TiffInfo info;
    const bool swapped = h.orientation >= 5;
    info.width = swapped ? h.height : h.width;
    info.height = swapped ? h.width : h.height;
    info.bitsPerSample = h.bits;
    info.samplesPerPixel = h.samples;
    info.compression = h.compression;
    info.photometric = h.photometric;
    info.orientation = h.orientation;
    info.compressionName = CompressionName(h.compression);
    info.software = GetAscii(h.reader, h.ifd, kSoftware);
    info.colorProfile = "none";
    if (auto it = h.ifd.find(kIccProfile); it != h.ifd.end() && it->second.count > 0) {
        h.reader.Check(it->second.valueOffset, it->second.count);
        const ColorTransform t = MakeColorTransform(h.reader.data + it->second.valueOffset, it->second.count);
        info.colorProfile = t.description;
    }
    return info;
}

// Rearranges the decoded pixels for Orientation values other than 1.
TiffImage Reorient(TiffImage image, uint16_t orientation) {
    if (orientation == 1) {
        return image;
    }
    const uint32_t w = image.width, h = image.height;
    const bool swap = orientation >= 5;
    TiffImage out;
    out.width = swap ? h : w;
    out.height = swap ? w : h;
    out.bitsPerSample = image.bitsPerSample;
    out.color = std::move(image.color);
    out.info = std::move(image.info);
    const size_t px = 3 * (image.bitsPerSample / 8);
    out.pixels.resize(static_cast<size_t>(out.width) * out.height * px);
    const size_t srcRow = static_cast<size_t>(w) * px;
    const size_t dstRow = static_cast<size_t>(out.width) * px;
    ParallelForEach(out.height, 0, [&](unsigned, size_t y) {
        uint8_t* dst = out.pixels.data() + y * dstRow;
        for (uint32_t x = 0; x < out.width; ++x) {
            uint32_t sx, sy;
            switch (orientation) {
                case 2: sx = w - 1 - x; sy = static_cast<uint32_t>(y); break;
                case 3: sx = w - 1 - x; sy = h - 1 - static_cast<uint32_t>(y); break;
                case 4: sx = x; sy = h - 1 - static_cast<uint32_t>(y); break;
                case 5: sx = static_cast<uint32_t>(y); sy = x; break;
                case 6: sx = static_cast<uint32_t>(y); sy = h - 1 - x; break;
                case 7: sx = w - 1 - static_cast<uint32_t>(y); sy = h - 1 - x; break;
                default: sx = w - 1 - static_cast<uint32_t>(y); sy = x; break;  // 8
            }
            std::memcpy(dst + x * px, image.pixels.data() + sy * srcRow + sx * px, px);
        }
    });
    return out;
}

TiffImage Decode(const uint8_t* data, size_t size, unsigned threads) {
    const Header h = ParseHeader(data, size);
    CheckSupported(h);
    const Reader& r = h.reader;
    const uint32_t w = h.width, hgt = h.height;
    const size_t bytesPerSample = h.bits / 8;
    const size_t rowBytes = static_cast<size_t>(w) * 3 * bytesPerSample;
    if (static_cast<double>(rowBytes) * hgt > 8e9) {
        throw Error("the image is too large");
    }

    // The chunks (strips or tiles) and their placement.
    std::vector<uint32_t> offsets, counts;
    uint32_t chunkWidth = w, chunkHeight, across = 1;
    if (h.tiled) {
        chunkWidth = GetUInt(r, h.ifd, kTileWidth, 0);
        chunkHeight = GetUInt(r, h.ifd, kTileLength, 0);
        if (chunkWidth == 0 || chunkHeight == 0) {
            throw Error("the tiles have no size");
        }
        offsets = GetUInts(r, h.ifd, kTileOffsets);
        counts = GetUInts(r, h.ifd, kTileByteCounts);
        across = (w + chunkWidth - 1) / chunkWidth;
    } else {
        chunkHeight = std::clamp<uint32_t>(GetUInt(r, h.ifd, kRowsPerStrip, hgt), 1, hgt);
        offsets = GetUInts(r, h.ifd, kStripOffsets);
        counts = GetUInts(r, h.ifd, kStripByteCounts);
    }
    const uint32_t down = (hgt + chunkHeight - 1) / chunkHeight;
    const size_t chunkCount = static_cast<size_t>(across) * down;
    if (offsets.size() < chunkCount) {
        throw Error("the file lists fewer strips or tiles than the image needs");
    }
    const size_t chunkRowBytes = static_cast<size_t>(chunkWidth) * h.samples * bytesPerSample;
    if (counts.size() < chunkCount) {
        if (h.compression != kUncompressed || h.tiled) {
            throw Error("the file does not list its strip sizes");
        }
        counts.assign(chunkCount, 0);
        for (size_t i = 0; i < chunkCount; ++i) {
            const uint32_t rows = std::min(chunkHeight, hgt - static_cast<uint32_t>(i) * chunkHeight);
            counts[i] = static_cast<uint32_t>(chunkRowBytes * rows);
        }
    }

    TiffImage image;
    image.width = w;
    image.height = hgt;
    image.bitsPerSample = h.bits;
    image.pixels.resize(rowBytes * hgt);
    const bool gray = h.photometric != kRgb;
    const bool invert = h.photometric == kWhiteIsZero;
    // Strips of plain RGB decode straight into the image; everything else goes
    // through a scratch buffer and is copied pixel by pixel.
    const bool direct = !h.tiled && h.samples == 3 && !gray;
    const bool swapBytes = h.bits == 16 && r.littleEndian != (std::endian::native == std::endian::little);

    if (threads == 0) {
        threads = std::max(1u, std::thread::hardware_concurrency());
    }
    std::vector<std::vector<uint8_t>> scratch(threads);
    ParallelForEach(chunkCount, threads, [&](unsigned thread, size_t index) {
        const uint32_t x0 = h.tiled ? static_cast<uint32_t>(index % across) * chunkWidth : 0;
        const uint32_t y0 = h.tiled ? static_cast<uint32_t>(index / across) * chunkHeight
                                    : static_cast<uint32_t>(index) * chunkHeight;
        const uint32_t rows = h.tiled ? chunkHeight : std::min(chunkHeight, hgt - y0);
        const size_t expected = chunkRowBytes * rows;
        r.Check(offsets[index], counts[index]);
        const uint8_t* src = r.data + offsets[index];
        const size_t srcLen = counts[index];

        uint8_t* dst;
        if (direct) {
            dst = image.pixels.data() + static_cast<size_t>(y0) * rowBytes;
        } else {
            scratch[thread].resize(expected);
            dst = scratch[thread].data();
        }
        switch (h.compression) {
            case kUncompressed:
                if (srcLen < expected) {
                    throw Error("a strip is shorter than expected");
                }
                std::memcpy(dst, src, expected);
                break;
            case kLzw: DecodeLzw(src, srcLen, dst, expected); break;
            case kAdobeDeflate:
            case kDeflate: Inflate(src, srcLen, dst, expected); break;
            case kPackBits: DecodePackBits(src, srcLen, dst, expected); break;
            default: break;
        }
        if (swapBytes) {
            for (size_t i = 0; i + 1 < expected; i += 2) {
                std::swap(dst[i], dst[i + 1]);
            }
        }
        if (h.predictor == 2) {
            for (uint32_t row = 0; row < rows; ++row) {
                if (h.bits == 8) {
                    UndoPredictor8(dst + row * chunkRowBytes, chunkWidth, h.samples);
                } else {
                    UndoPredictor16(reinterpret_cast<uint16_t*>(dst + row * chunkRowBytes), chunkWidth, h.samples);
                }
            }
        }
        if (direct) {
            return;
        }
        // Copy the chunk's pixels into the image, keeping the first three channels
        // (or spreading a gray value over them) and clipping tiles at the edges.
        const size_t srcPx = static_cast<size_t>(h.samples) * bytesPerSample;
        const size_t dstPx = 3 * bytesPerSample;
        for (uint32_t row = 0; row < rows && y0 + row < hgt; ++row) {
            const uint8_t* s = dst + row * chunkRowBytes;
            uint8_t* d = image.pixels.data() + static_cast<size_t>(y0 + row) * rowBytes + static_cast<size_t>(x0) * dstPx;
            const uint32_t cols = std::min(chunkWidth, w - x0);
            if (!gray) {
                for (uint32_t x = 0; x < cols; ++x) {
                    std::memcpy(d + x * dstPx, s + x * srcPx, dstPx);
                }
            } else if (h.bits == 8) {
                for (uint32_t x = 0; x < cols; ++x) {
                    const uint8_t v = invert ? static_cast<uint8_t>(255 - s[x * srcPx]) : s[x * srcPx];
                    d[x * 3] = d[x * 3 + 1] = d[x * 3 + 2] = v;
                }
            } else {
                for (uint32_t x = 0; x < cols; ++x) {
                    uint16_t v;
                    std::memcpy(&v, s + x * srcPx, 2);
                    if (invert) {
                        v = static_cast<uint16_t>(65535 - v);
                    }
                    uint16_t* dp = reinterpret_cast<uint16_t*>(d + x * dstPx);
                    dp[0] = dp[1] = dp[2] = v;
                }
            }
        }
    });

    image.info = MakeInfo(h);
    if (!gray) {
        if (auto it = h.ifd.find(kIccProfile); it != h.ifd.end() && it->second.count > 0) {
            image.color = MakeColorTransform(r.data + it->second.valueOffset, it->second.count);
        } else {
            image.color = MakeColorTransform(nullptr, 0);
        }
    } else {
        image.color.description = "grayscale (treated as sRGB)";
    }
    return Reorient(std::move(image), h.orientation);
}

template <typename F>
auto CatchErrors(const std::string& name, F&& f) -> std::expected<decltype(f()), std::string> {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        return std::unexpected(name + ": out of memory");
    } catch (const std::exception& e) {
        return std::unexpected(name + ": " + e.what());
    }
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw Error("could not open the file");
    }
    const std::streamsize size = in.tellg();
    if (size < 0) {
        throw Error("could not read the file");
    }
    std::vector<uint8_t> data(static_cast<size_t>(size));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size)) {
        throw Error("could not read the file");
    }
    return data;
}

}  // namespace

void ColorTransform::Apply(const float* rgb, uint8_t* out, size_t pixels) const {
    auto to8 = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    if (identity) {
        for (size_t i = 0; i < pixels * 3; ++i) {
            out[i] = to8(rgb[i]);
        }
        return;
    }
    // Linear light -> sRGB encoding, tabulated once over [0, 1].
    static const std::vector<float> encode = [] {
        std::vector<float> lut(kLutSize);
        for (size_t i = 0; i < kLutSize; ++i) {
            lut[i] = static_cast<float>(LinearToSrgb(static_cast<double>(i) / static_cast<double>(kLutSize - 1)));
        }
        return lut;
    }();
    auto lookup = [](const std::vector<float>& lut, float v) {
        const float pos = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(kLutSize - 1);
        const size_t k = std::min<size_t>(static_cast<size_t>(pos), kLutSize - 2);
        const float t = pos - static_cast<float>(k);
        return lut[k] + (lut[k + 1] - lut[k]) * t;
    };
    for (size_t i = 0; i < pixels; ++i) {
        const float r = lookup(toLinear[0], rgb[i * 3]);
        const float g = lookup(toLinear[1], rgb[i * 3 + 1]);
        const float b = lookup(toLinear[2], rgb[i * 3 + 2]);
        for (size_t c = 0; c < 3; ++c) {
            const float lin = matrix[c * 3] * r + matrix[c * 3 + 1] * g + matrix[c * 3 + 2] * b;
            out[i * 3 + c] = to8(lookup(encode, lin));
        }
    }
}

std::expected<TiffInfo, std::string> ProbeTiff(const fs::path& path) {
    return CatchErrors(Utf8(path.filename()), [&] {
        // The tags are at the start or the end of the file, so reading everything
        // would be wasteful; read the header and follow the IFD pointer instead.
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            throw Error("could not open the file");
        }
        const std::streamsize size = in.tellg();
        in.seekg(0);
        std::vector<uint8_t> data(static_cast<size_t>(std::min<std::streamsize>(size, 1 << 20)));
        if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
            throw Error("could not read the file");
        }
        try {
            const Header h = ParseHeader(data.data(), data.size());
            CheckSupported(h);
            return MakeInfo(h);
        } catch (const Truncated&) {
            // The tags may lie beyond the first megabyte; read the whole file.
            data = ReadFile(path);
            const Header h = ParseHeader(data.data(), data.size());
            CheckSupported(h);
            return MakeInfo(h);
        }
    });
}

std::expected<TiffImage, std::string> LoadTiff(const fs::path& path, unsigned threads) {
    return CatchErrors(Utf8(path.filename()), [&] {
        const std::vector<uint8_t> data = ReadFile(path);
        return Decode(data.data(), data.size(), threads);
    });
}

std::expected<TiffImage, std::string> DecodeTiff(const std::vector<uint8_t>& file, const std::string& name,
                                                 unsigned threads) {
    return CatchErrors(name, [&] { return Decode(file.data(), file.size(), threads); });
}

bool IsTiffPath(const fs::path& path) {
    std::string ext = Utf8(path.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".tif" || ext == ".tiff";
}
