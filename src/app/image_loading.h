#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

#include "io/dng_loader.h"

namespace r2r {

struct LoadResult {
    std::shared_ptr<DngDocument> doc;
    RenderedImage image;
};

// Opens a DNG and renders it. Meant to run on a worker thread.
std::expected<LoadResult, std::string> LoadAndRender(const std::filesystem::path& path, uint32_t preferredSize,
                                                     const RenderSettings& settings, uint32_t maxSize);

}  // namespace r2r
