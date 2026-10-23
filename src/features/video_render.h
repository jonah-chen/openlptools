#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/feature.h"
#include "io/tiff_loader.h"
#include "processing/video_export.h"

namespace r2r {

// Encodes a folder of TIFFs (as exported from Lightroom) into an MP4 video.
class VideoRenderFeature : public Feature {
public:
    explicit VideoRenderFeature(App& app);

    bool Busy() const override { return job_ && job_->Running(); }
    void Draw() override;
    int OpenScore(const fs::path& path) const override;
    void Open(const fs::path& path) override;
    void Shutdown() override;

    // Asks for a folder of TIFFs and shows this feature with it; GoBack() then
    // returns to returnTo (the home screen if null).
    void BrowseForFolder(Feature* returnTo = nullptr);

private:
    void OpenFolder(const fs::path& folder);
    void CloseFolder();
    void Close();
    void SetOutput(fs::path path);
    std::pair<uint32_t, uint32_t> VideoSize() const;
    void SyncBitrate();
    void StartJob();
    void BrowseForOutput();

    void DrawEmpty();
    void DrawForm();
    void DrawPreview();
    void DrawButtons();

    std::string error_;

    fs::path folder_;
    std::vector<fs::path> files_;
    std::optional<TiffInfo> first_;  // the first file's format, if it could be read
    std::string firstError_;

    VideoOptions options_;
    int widthPreset_ = 3;  // index into the width presets; one past the end = custom
    int customWidth_ = 3840;
    int fpsIndex_ = 2;
    bool bitrateEdited_ = false;  // the user set a bitrate: stop deriving it from the size
    char outputText_[1024] = "";

    std::unique_ptr<VideoExportJob> job_;
    SDL_Texture* preview_ = nullptr;
    uint64_t previewVersion_ = 0;
    float previewWidth_ = 0;
    float previewHeight_ = 0;
};

}  // namespace r2r
