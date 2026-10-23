#include "app/image_loading.h"

namespace r2r {

std::expected<LoadResult, std::string> LoadAndRender(const std::filesystem::path& path, uint32_t preferredSize,
                                                     const RenderSettings& settings, uint32_t maxSize) {
    auto doc = DngDocument::Open(path, preferredSize);
    if (!doc) {
        return std::unexpected(doc.error());
    }
    auto image = (*doc)->Render(settings, maxSize);
    if (!image) {
        return std::unexpected(image.error());
    }
    return LoadResult{std::move(*doc), std::move(*image)};
}

}  // namespace r2r
