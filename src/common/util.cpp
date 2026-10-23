#include "common/util.h"

#include <cctype>
#include <cmath>
#include <format>

namespace r2r {

fs::path PathFromUtf8(const char* utf8) {
    return fs::path(reinterpret_cast<const char8_t*>(utf8));
}

std::string Utf8(const fs::path& path) {
    const std::u8string s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

std::string FileUrl(const fs::path& path) {
    const std::u8string s = path.generic_u8string();
    std::string url = "file:///";
    for (char8_t c : s) {
        const bool keep = std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == ':' || c == '-' ||
                          c == '_' || c == '.' || c == '~';
        if (keep) {
            url += static_cast<char>(c);
        } else {
            url += std::format("%{:02X}", static_cast<unsigned>(c));
        }
    }
    return url;
}

double SecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::string ByteText(uint64_t bytes) {
    if (bytes >= 1ull << 30) {
        return std::format("{:.2f} GB", static_cast<double>(bytes) / (1ull << 30));
    }
    if (bytes >= 1ull << 20) {
        return std::format("{:.1f} MB", static_cast<double>(bytes) / (1ull << 20));
    }
    return std::format("{:.0f} KB", static_cast<double>(bytes) / (1ull << 10));
}

std::string ShutterText(double seconds) {
    if (seconds <= 0.0) {
        return "";
    }
    if (seconds < 1.0) {
        return std::format("1/{:.0f}", 1.0 / seconds);
    }
    return std::format("{:.1f} s", seconds);
}

std::string ApertureText(double fNumber) {
    return fNumber > 0.0 ? std::format("f/{:.3g}", fNumber) : "";
}

std::string EvText(const std::optional<double>& ev, int decimals) {
    return ev ? std::format("{:+.{}f}", *ev, decimals) : "";
}

std::string ClockText(double seconds) {
    const int total = static_cast<int>(std::lround(seconds));
    if (total >= 3600) {
        return std::format("{}:{:02}:{:02}", total / 3600, (total / 60) % 60, total % 60);
    }
    return std::format("{}:{:02}", total / 60, total % 60);
}

double NiceStep(double rough, std::span<const double> candidates) {
    for (double c : candidates) {
        if (c >= rough) {
            return c;
        }
    }
    return candidates.back();
}

}  // namespace r2r
