#pragma once

// Client of the exposure-match server (src/tools/exposure_match): compares two
// raw files and returns how many stops brighter the second is.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include "processing/metering.h"

struct MeterEndpoint {
    std::string host = "127.0.0.1";
    uint16_t port = 31001;
};

// POST /compare {"reference": a, "image": b}. Transport and server errors come back
// as the error string.
std::expected<Comparison, std::string> CompareRemote(const MeterEndpoint& endpoint, const std::filesystem::path& reference,
                                                     const std::filesystem::path& image);
