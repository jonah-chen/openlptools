#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/feature.h"
#include "app/filmstrip.h"
#include "io/dng_loader.h"
#include "processing/raw_stack.h"

namespace r2r {

// Combines the frames selected in a filmstrip into one DNG: their mean (a longer
// exposure) or median (what stays still). The frames come from this tool's own
// Open buttons or from the viewer's selection; the highlighted frame lends the
// result its metadata and Camera Raw settings.
class RawProcessorFeature : public Feature {
public:
    explicit RawProcessorFeature(App& app);

    void Poll() override;
    bool Busy() const override;
    void Draw() override;
    void DrawMenus() override;
    void HandleShortcuts() override;
    int OpenScore(const fs::path& path) const override;
    void Open(const fs::path& path) override;
    void OpenFiles(const std::vector<fs::path>& paths) override;
    std::string WindowTitle() const override;
    void Shutdown() override;

    // Takes over a strip (files, selection, highlighted frame) and shows the tool;
    // GoBack() then returns to returnTo.
    void UseFilmstrip(const Filmstrip& strip, Feature* returnTo);

private:
    void OpenFolder(const fs::path& folder);
    void ShowOpenDialog();
    void ShowOpenFolderDialog();
    void SetOutput(const fs::path& path, bool edited);
    void SyncDefaultOutput();
    fs::path DefaultOutput() const;
    const CaptureInfo* ActiveCapture();
    std::string Blocker() const;  // why Process is disabled, or empty
    void StartJob();

    void DrawSettingsPane();
    void DrawPreview();
    void DrawConfirmPopup();

    Filmstrip filmstrip_;
    StackMode mode_ = StackMode::Mean;
    fs::path output_;
    char outputText_[1024] = "";
    bool outputEdited_ = false;  // the user chose the name: stop deriving it

    std::unique_ptr<StackJob> job_;
    std::string error_;
    bool openConfirm_ = false;

    // Camera settings of frames, read once per file for the summary and the confirmation.
    std::unordered_map<fs::path, std::optional<CaptureInfo>> captures_;
};

}  // namespace r2r
