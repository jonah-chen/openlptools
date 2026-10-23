#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/feature.h"
#include "app/filmstrip.h"
#include "app/image_loading.h"
#include "app/ui.h"
#include "io/dng_loader.h"
#include "net/export_client.h"

namespace r2r {

// Shows one DNG at a time: zoom and pan, metadata, histogram and an exposure
// adjustment. A filmstrip along the bottom holds the open files (one, several, or
// a folder's worth). Several files can be selected in the strip for a tool that
// works on a set.
class ImageViewerFeature : public Feature {
public:
    explicit ImageViewerFeature(App& app);

    void Poll() override;
    bool Busy() const override {
        return Loading() || rendering_.valid() || filmstrip_.Busy() || (export_.job && export_.job->Running());
    }
    void Draw() override;
    void DrawMenus() override;
    void HandleShortcuts() override;
    int OpenScore(const fs::path& path) const override;
    void Open(const fs::path& path) override;
    void OpenFiles(const std::vector<fs::path>& paths) override;
    void OpenFolder(const fs::path& folder);  // every DNG of the folder
    std::string WindowTitle() const override;
    void Shutdown() override;

    // Opens a frame of a sequence, starting with the exposure adjustment computed for it (if any).
    void OpenWithExposure(const fs::path& path, std::optional<double> exposure);

    // The files in the filmstrip and which of them are selected.
    const Filmstrip& Strip() const { return filmstrip_; }

private:
    bool Loading() const { return pending_.valid(); }
    void Load(const fs::path& path);
    bool Upload(const RenderedImage& image);
    void PollLoad();
    void PollRender();
    void StartRenderIfNeeded();
    std::optional<fs::path> CurrentPath() const;
    void ShowOpenDialog();
    void ShowOpenFolderDialog();
    void SendToProcessor();
    void ResetZoomToActualPixels();

    void DrawInfoPanel();
    void DrawAdjustPanel();
    void DrawImageView();

    // Export of the selected files through the export server (Settings).
    void BeginExport();
    std::vector<ExportItem> ExportItems() const;
    void DrawExportPopup();
    void DrawExportForm();
    void DrawExportProgress();

    Filmstrip filmstrip_;

    struct ExportDialog {
        bool open = false;  // asks the popup to open on the next frame
        std::vector<fs::path> inputs;
        ExportOptions options;
        char destination[1024] = "";
        std::unique_ptr<ExportJob> job;
    } export_;

    // The open document and what is on screen. displayWidth/Height are the size of
    // the full resolution render; a preview texture is stretched to that size.
    std::shared_ptr<DngDocument> doc_;
    SDL_Texture* texture_ = nullptr;
    float displayWidth_ = 0;
    float displayHeight_ = 0;
    HistogramData histogram_;

    // Adjustments as set in the UI, and those the texture was rendered with.
    // Not saved anywhere yet; they reset when another file opens.
    RenderSettings settings_;
    RenderSettings shownSettings_;
    bool shownFull_ = true;
    bool sliderActive_ = false;
    std::optional<double> exposureOnLoad_;  // exposure to start with for the file being loaded

    // Background load of a new file. A request that arrives while loading waits in queuedPath_.
    std::future<std::expected<LoadResult, std::string>> pending_;
    fs::path pendingPath_;
    std::optional<fs::path> queuedPath_;
    std::chrono::steady_clock::time_point loadStart_;
    std::string loadTime_;

    // Background re-render of the open document after an adjustment.
    std::future<std::expected<RenderedImage, std::string>> rendering_;
    std::shared_ptr<DngDocument> renderingDoc_;
    RenderSettings renderingSettings_;
    bool renderingFull_ = false;
    std::chrono::steady_clock::time_point renderStart_;
    std::string renderTime_;

    std::string error_;

    // View transform: zoom is screen pixels per image pixel; center is the image
    // coordinate at the middle of the view. While fit is set, both follow the view.
    bool fit_ = true;
    float zoom_ = 1.0f;
    ImVec2 center_{0, 0};
};

}  // namespace r2r
