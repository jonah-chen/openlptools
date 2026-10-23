#include "io/xmp_settings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>

#include "dng_auto_ptr.h"
#include "dng_exceptions.h"
#include "dng_file_stream.h"
#include "dng_host.h"
#include "dng_info.h"
#include "dng_memory.h"
#include "dng_negative.h"
#include "dng_string.h"
#include "dng_xmp.h"
#include "dng_xmp_sdk.h"

namespace fs = std::filesystem;

namespace {

// This program's own namespace. A sidecar it writes for a frame that is not a
// keyframe carries r2r:ExposureMatch equal to the xmp:MetadataDate it wrote; any
// later save by Lightroom or Camera Raw stamps a new date, so the marker then no
// longer matches and the settings count as the user's.
const char* const kR2rNamespace = "http://raw2raw2/exposure-match/1.0/";
const char* const kMarkerProperty = "ExposureMatch";

std::string Utf8(const fs::path& path) {
    const std::u8string s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

template <typename F>
auto CatchErrors(F&& f) -> std::expected<decltype(f()), std::string> {
    try {
        return f();
    } catch (const dng_exception& e) {
        return std::unexpected(std::format("DNG SDK error {}", static_cast<int>(e.ErrorCode())));
    } catch (const std::exception& e) {
        return std::unexpected(std::string(e.what()));
    }
}

// Fills in the summary fields of settings from its parsed XMP.
void Summarize(const dng_xmp& xmp, DevelopSettings& settings) {
    dng_string s;
    if (xmp.GetString(XMP_NS_CRS, "Exposure2012", s)) {
        try {
            settings.exposure2012 = std::stod(s.Get());
        } catch (const std::exception&) {
        }
    }
    if (xmp.GetString(XMP_NS_CRS, "ProcessVersion", s)) {
        settings.processVersion = s.Get();
    }
    int count = 0;
    xmp.IteratePaths(
        [](const char*, const char*, void* data) {
            ++*static_cast<int*>(data);
            return true;
        },
        &count, XMP_NS_CRS, nullptr, true);
    settings.settingCount = count;
    dng_string marker, date;
    settings.generated = xmp.GetString(kR2rNamespace, kMarkerProperty, marker) &&
                         xmp.GetString(XMP_NS_XAP, "MetadataDate", date) && std::string(marker.Get()) == date.Get();
}

// Parses an XMP/ISO 8601 date ("2026-10-17T03:27:06.5-04:00", "...Z", or without a
// zone) into seconds since 1970, for comparing two dates.
std::optional<double> ParseIso8601(const std::string& s) {
    int year, month, day, hour = 0, minute = 0;
    double second = 0.0;
    int consumed = 0;
    if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%lf%n", &year, &month, &day, &hour, &minute, &second, &consumed) < 5 &&
        sscanf_s(s.c_str(), "%d-%d-%d%n", &year, &month, &day, &consumed) < 3) {
        return std::nullopt;
    }
    // Days since 1970-01-01 (proleptic Gregorian).
    const int y = year - (month <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const double days = era * 146097.0 + doe - 719468.0;
    double seconds = days * 86400.0 + hour * 3600.0 + minute * 60.0 + second;

    // Time zone offset, if any.
    const std::string rest = s.substr(static_cast<size_t>(consumed));
    int zoneHour = 0, zoneMinute = 0;
    if (!rest.empty() && (rest[0] == '+' || rest[0] == '-') &&
        sscanf_s(rest.c_str() + 1, "%d:%d", &zoneHour, &zoneMinute) >= 1) {
        const double offset = zoneHour * 3600.0 + zoneMinute * 60.0;
        seconds += rest[0] == '+' ? -offset : offset;
    }
    return seconds;
}

std::optional<double> MetadataDate(const dng_xmp& xmp) {
    dng_string s;
    if (xmp.GetString(XMP_NS_XAP, "MetadataDate", s)) {
        return ParseIso8601(s.Get());
    }
    return std::nullopt;
}

std::string NowIso8601() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    try {
        return std::format("{:%FT%T%Ez}", std::chrono::zoned_time(std::chrono::current_zone(), now));
    } catch (const std::exception&) {
        return std::format("{:%FT%T}Z", now);
    }
}

std::string SerializeXmp(const dng_xmp& xmp) {
    AutoPtr<dng_memory_block> block(xmp.Serialize(true, 0, 0, false, true));
    return {block->Buffer_char(), block->LogicalSize()};
}

// Parses an XMP packet; the caller keeps the host alive while using the result.
std::unique_ptr<dng_xmp> ParseXmp(dng_host& host, const std::string& packet) {
    auto xmp = std::make_unique<dng_xmp>(host.Allocator());
    xmp->Parse(host, packet.data(), static_cast<uint32>(packet.size()));
    return xmp;
}

bool IsNumber(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size();
}

}  // namespace

void InitializeXmpSdk() {
    static dng_xmp_namespace namespaces[] = {{kR2rNamespace, "r2r"}, {nullptr, nullptr}};
    dng_xmp_sdk::InitializeSDK(namespaces);
}

fs::path SidecarPath(const fs::path& raw) {
    fs::path sidecar = raw;
    sidecar.replace_extension(".xmp");
    return sidecar;
}

std::string FormatExposure2012(double exposure) {
    if (std::abs(exposure) < 0.005) {
        return "0.00";
    }
    return std::format("{:+.2f}", exposure);
}

std::string FormatLikeSamples(double value, const std::vector<std::string>& samples) {
    int decimals = 0;
    bool plus = false;
    for (const std::string& s : samples) {
        plus = plus || (!s.empty() && s[0] == '+');
        if (const size_t dot = s.find('.'); dot != std::string::npos) {
            decimals = std::max(decimals, static_cast<int>(s.size() - dot - 1));
        }
    }
    decimals = std::min(decimals, 6);
    std::string text = std::format("{:.{}f}", value, decimals);
    if (text.find_first_not_of("-0.") == std::string::npos) {
        return text.substr(text[0] == '-' ? 1 : 0);  // "-0" and "0.00" are written unsigned
    }
    return plus && text[0] != '-' ? "+" + text : text;
}

std::expected<DevelopSettings, std::string> ReadDevelopSettings(const fs::path& raw) {
    return CatchErrors([&] {
        DevelopSettings settings;
        settings.source = "none";
        dng_host host;

        // The sidecar, if there is one.
        std::unique_ptr<dng_xmp> sidecarXmp;
        const fs::path sidecar = SidecarPath(raw);
        std::error_code ec;
        if (fs::is_regular_file(sidecar, ec)) {
            std::ifstream in(sidecar, std::ios::binary);
            std::string packet((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!in && !in.eof()) {
                throw std::runtime_error("could not read " + Utf8(sidecar.filename()));
            }
            sidecarXmp = ParseXmp(host, packet);
        }

        // The XMP block inside the DNG. Parsing the negative reads only the metadata,
        // not the image; the XMP is decoded in PostParse.
        std::unique_ptr<dng_xmp> embeddedXmp;
        {
            dng_file_stream stream(raw.c_str());
            dng_info info;
            info.Parse(host, stream);
            info.PostParse(host);
            if (!info.IsValidDNG()) {
                ThrowBadFormat();
            }
            AutoPtr<dng_negative> negative(host.Make_dng_negative());
            negative->Parse(host, stream, info);
            negative->PostParse(host, stream, info);
            if (const dng_xmp* xmp = negative->GetXMP(); xmp && xmp->HasMeta()) {
                embeddedXmp = std::make_unique<dng_xmp>(*xmp);
            }
        }

        // Lightroom and Camera Raw prefer a sidecar. If both carry a MetadataDate (which
        // they stamp on every save, and this program does too), the later one wins;
        // file modification times are not used because copies often change them.
        bool useSidecar = sidecarXmp != nullptr;
        if (sidecarXmp && embeddedXmp) {
            const std::optional<double> sidecarDate = MetadataDate(*sidecarXmp);
            const std::optional<double> embeddedDate = MetadataDate(*embeddedXmp);
            if (sidecarDate && embeddedDate && *embeddedDate > *sidecarDate) {
                useSidecar = false;
            }
        }
        if (useSidecar) {
            settings.xmp = SerializeXmp(*sidecarXmp);
            settings.source = "sidecar " + Utf8(sidecar.filename());
            Summarize(*sidecarXmp, settings);
        } else if (embeddedXmp) {
            settings.xmp = SerializeXmp(*embeddedXmp);
            settings.source = "embedded in " + Utf8(raw.filename());
            Summarize(*embeddedXmp, settings);
        }
        return settings;
    });
}

std::expected<std::vector<DevelopProperty>, std::string> ListDevelopProperties(const DevelopSettings& settings) {
    return CatchErrors([&] {
        std::vector<DevelopProperty> props;
        if (settings.xmp.empty()) {
            return props;
        }
        dng_host host;
        const std::unique_ptr<dng_xmp> xmp = ParseXmp(host, settings.xmp);

        // Every path of the namespace: "crs:Highlights2012", "crs:ToneCurvePV2012[1]",
        // "crs:Look/crs:Parameters/crs:Dehaze", ... in document order.
        std::vector<std::string> paths;
        xmp->IteratePaths(
            [](const char*, const char* path, void* data) {
                if (*path) {
                    static_cast<std::vector<std::string>*>(data)->push_back(path);
                }
                return true;
            },
            &paths, XMP_NS_CRS, nullptr, false);

        for (const std::string& path : paths) {
            const size_t end = path.find_first_of("/[");
            std::string top = path.substr(0, end);
            top = top.substr(top.find(':') + 1);
            auto it = std::ranges::find(props, top, &DevelopProperty::name);
            if (it == props.end()) {
                props.push_back({.name = top});
                it = props.end() - 1;
            }
            dng_string s;
            const bool has = xmp->GetString(XMP_NS_CRS, path.c_str(), s);
            if (end == std::string::npos) {
                it->value = has ? s.Get() : "";
            } else {
                // A node below a top-level property: the property is a composite and
                // its value is the digest of everything inside it.
                it->composite = true;
                it->value += path.substr(end) + "=" + (has ? s.Get() : "") + "\n";
            }
        }
        for (DevelopProperty& p : props) {
            p.numeric = !p.composite && IsNumber(p.value);
        }
        return props;
    });
}

std::expected<fs::path, std::string> WriteSidecar(const fs::path& raw, const SidecarContent& content) {
    return CatchErrors([&] {
        dng_host host;
        dng_xmp xmp(host.Allocator());
        if (content.base && !content.base->xmp.empty()) {
            // Only the develop settings are copied: the base's other namespaces
            // (ratings, captions, EXIF mirrors) describe the base's own file.
            const std::unique_ptr<dng_xmp> source = ParseXmp(host, content.base->xmp);
            xmp.DuplicateNameSpace(*source, XMP_NS_CRS);
        }
        for (const auto& [name, value] : content.overrides) {
            xmp.Set(XMP_NS_CRS, name.c_str(), value.c_str());
        }
        if (!xmp.Exists(XMP_NS_CRS, "ProcessVersion") && !content.processVersion.empty()) {
            xmp.Set(XMP_NS_CRS, "ProcessVersion", content.processVersion.c_str());
        }
        xmp.Set(XMP_NS_CRS, "Exposure2012", FormatExposure2012(content.exposure2012).c_str());
        xmp.Set(XMP_NS_CRS, "HasSettings", "True");
        xmp.Set(XMP_NS_CRS, "RawFileName", Utf8(raw.filename()).c_str());
        const std::string date = NowIso8601();
        xmp.Set(XMP_NS_XAP, "MetadataDate", date.c_str());
        if (content.generated) {
            xmp.Set(kR2rNamespace, kMarkerProperty, date.c_str());
        }

        const fs::path sidecar = SidecarPath(raw);
        const std::string packet = SerializeXmp(xmp);
        std::ofstream out(sidecar, std::ios::binary | std::ios::trunc);
        out.write(packet.data(), static_cast<std::streamsize>(packet.size()));
        if (!out) {
            throw std::runtime_error("could not write " + Utf8(sidecar.filename()));
        }
        return sidecar;
    });
}
