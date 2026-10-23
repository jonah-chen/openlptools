#pragma once

// Small helpers shared by the app shell and the features: paths, text formatting
// and a few numeric utilities.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace r2r {

namespace fs = std::filesystem;

// UTF-8 <-> fs::path
fs::path PathFromUtf8(const char* utf8);
std::string Utf8(const fs::path& path);

// Percent-encodes a path as a file: URL for SDL_OpenURL.
std::string FileUrl(const fs::path& path);

// True if the folder directly contains a regular file for which pred(path) holds.
template <typename Pred>
bool FolderHas(const fs::path& folder, Pred pred) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec) && pred(entry.path())) {
            return true;
        }
    }
    return false;
}

double SecondsSince(std::chrono::steady_clock::time_point start);

std::string ByteText(uint64_t bytes);
std::string ShutterText(double seconds);
std::string ApertureText(double fNumber);
std::string EvText(const std::optional<double>& ev, int decimals = 2);
std::string ClockText(double seconds);

// Smallest candidate step at least as large as rough.
double NiceStep(double rough, std::span<const double> candidates);

}  // namespace r2r
