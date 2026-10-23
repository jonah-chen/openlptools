#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Camera Raw develop settings (the crs: XMP namespace) attached to a raw file.
struct DevelopSettings {
    std::string xmp;     // the full XMP packet; empty if the file has none
    std::string source;  // where it came from, for display: "sidecar DSC00001.xmp", "embedded in DSC00001.dng", "none"
    std::optional<double> exposure2012;  // crs:Exposure2012, if set
    std::string processVersion;          // crs:ProcessVersion, if set
    int settingCount = 0;                // number of crs: properties
    // True if Exposure Match wrote these settings and nothing has saved the file's
    // metadata since: they are this program's output rather than the user's edits.
    bool generated = false;

    bool HasEdits() const { return settingCount > 0 && !generated; }
};

// Sets up the XMP SDK for this program: dng_xmp_sdk::InitializeSDK() plus the
// namespace that marks generated sidecars. Pair with dng_xmp_sdk::TerminateSDK().
void InitializeXmpSdk();

std::filesystem::path SidecarPath(const std::filesystem::path& raw);

// Reads the develop settings Camera Raw would use for a file: its sidecar .xmp when
// present and at least as new as the file, otherwise the XMP embedded in the DNG.
std::expected<DevelopSettings, std::string> ReadDevelopSettings(const std::filesystem::path& raw);

// A top-level crs: property. A simple property carries its text; a composite one
// (tone curve, masks, look, ...) carries a digest of its whole subtree, for comparing.
struct DevelopProperty {
    std::string name;  // without the crs: prefix
    std::string value;
    bool composite = false;
    bool numeric = false;  // a simple property whose text is a number
};
std::expected<std::vector<DevelopProperty>, std::string> ListDevelopProperties(const DevelopSettings& settings);

// What a sidecar should hold.
struct SidecarContent {
    const DevelopSettings* base = nullptr;  // crs: settings copied whole; null for an exposure-only sidecar
    std::vector<std::pair<std::string, std::string>> overrides;  // simple crs: properties set after the copy
    double exposure2012 = 0.0;
    std::string processVersion;  // used when base has none
    bool generated = true;       // mark as this program's output (see DevelopSettings::generated)
};

// Writes <raw stem>.xmp so Camera Raw and Lightroom apply the content to the file.
// Returns the sidecar's path.
std::expected<std::filesystem::path, std::string> WriteSidecar(const std::filesystem::path& raw,
                                                               const SidecarContent& content);

// Formats an EV value the way Lightroom writes crs:Exposure2012 ("+0.35", "-1.20", "0.00").
std::string FormatExposure2012(double exposure);

// Formats a value the way the samples it was interpolated from are written: whole
// when they are whole, else with as many decimals, and with a + when they carry one.
std::string FormatLikeSamples(double value, const std::vector<std::string>& samples);
