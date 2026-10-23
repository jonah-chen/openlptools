#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/feature.h"
#include "app/filmstrip.h"
#include "app/image_loading.h"
#include "app/ui.h"
#include "processing/sequence.h"
#include "io/xmp_settings.h"

namespace r2r {

// Meters every DNG of a folder and works out the exposure adjustment that makes
// the frames follow a smooth brightness curve through the keyframes, e.g. to
// remove time-lapse flicker and the camera's exposure steps. The keyframes are the
// frames edited in Lightroom (detected automatically when they are few) or chosen
// by hand. The result is written as a CSV and as XMP sidecars for Camera Raw /
// Lightroom that carry the keyframes' edits to every frame.
class ExposureMatchFeature : public Feature {
public:
    explicit ExposureMatchFeature(App& app);

    void Poll() override;
    bool Busy() const override;  // includes waiting for the server to start
    void Draw() override;
    void DrawMenus() override;
    void HandleShortcuts() override;
    int OpenScore(const fs::path& path) const override;
    void Open(const fs::path& path) override;
    std::string WindowTitle() const override;
    void Shutdown() override;

    // How many files to analyze when a sequence is opened (0 = all).
    void SetLimit(int limit) { limitInput_ = std::max(0, limit); }

private:
    // What a sequence is made of: every DNG of a folder (listed again on each
    // analysis, so frames added since are picked up) or files chosen one by one.
    struct SequenceSource {
        fs::path folder;              // a folder sequence; empty for chosen files
        std::vector<fs::path> files;  // the chosen files, sorted by name

        std::vector<fs::path> Frames() const;
        std::string Description() const;  // for messages: the folder, or "N files in <folder>"
    };

    // The preview of the frame selected in the sequence.
    struct FramePreview {
        int frame = -1;
        double exposure = 0.0;
        SDL_Texture* texture = nullptr;
        float width = 0;
        float height = 0;
        HistogramData histogram;
        std::vector<std::pair<std::string, std::string>> info;
        std::string error;
    };

    // A write of the CSV and sidecars, prepared from the keyframes' edits as they
    // are on disk and waiting for the user's confirmation in a popup.
    struct WritePlan {
        struct Keyframe {
            int frame = -1;
            std::string name;
            double x = 0.0;  // on the graph's x axis; the keyframes are sorted by it
            DevelopSettings settings;
            std::vector<DevelopProperty> properties;
        };
        // A numeric setting that differs between keyframes and is interpolated.
        struct Interpolated {
            std::string name;
            std::vector<std::string> samples;  // as written on each keyframe
            std::vector<double> values;
        };
        std::vector<Keyframe> keyframes;
        std::vector<Interpolated> interpolated;
        std::vector<std::string> differences;  // every setting that differs, described
        int targets = 0;                       // frames that get a sidecar
        int existingSidecars = 0;              // of which replace one
    };

    void ShowOpenFolderDialog();
    void ShowOpenFilesDialog();
    std::string DialogStart() const;  // the open sequence's folder, or empty
    void OpenSequence(SequenceSource source, size_t limit);
    void CloseSequence();
    void OpenFrameInViewer(const SequenceFrame& frame, const FrameExposure& exposure);
    void OpenSelectedInViewer();
    void SelectFrame(int index, int count);

    void DetectKeyframes(const std::vector<SequenceFrame>& frames);
    bool IsKeyframe(int frame) const { return std::ranges::binary_search(keyframes_, frame); }
    void ToggleKeyframe(int frame);

    void WantFramePreview(const std::vector<SequenceFrame>& frames, const std::vector<FrameExposure>& exposures);
    void PollFramePreview();

    void PrepareWrite();
    void PerformWrite(const std::vector<SequenceFrame>& frames, const std::vector<FrameExposure>& exposures);
    void DrawWritePopup(const std::vector<SequenceFrame>& frames, const std::vector<FrameExposure>& exposures);

    void DrawEmpty();
    void DrawSequenceLayout();
    void DrawGraphControls(const std::vector<SequenceFrame>& frames, bool usesTime);
    void DrawGraph(const std::vector<SequenceFrame>& frames, const std::vector<FrameExposure>& exposures,
                   bool usesTime);
    void DrawFrameList(const std::vector<SequenceFrame>& frames, const std::vector<FrameExposure>& exposures);
    void DrawToolbar(const std::vector<SequenceFrame>& frames, const std::vector<FrameExposure>& exposures);
    void DrawFramePane(const std::vector<SequenceFrame>& frames, const std::vector<FrameExposure>& exposures);

    std::unique_ptr<SequenceAnalysis> sequence_;
    SequenceSource source_;  // what sequence_ was made from, for Re-analyze
    Filmstrip filmstrip_;  // the sequence's frames: thumbnails for the list, selection for the other tools
    std::string error_;
    // A sequence to open once the metering server answers (it may still be starting).
    std::optional<std::pair<SequenceSource, size_t>> pendingOpen_;
    int limitInput_ = 0;  // 0 = all files
    bool reanalyzeRequested_ = false;
    FitSettings fitSettings_;
    int selectedFrame_ = -1;
    bool scrollToSelected_ = false;
    std::string status_;

    // The keyframes, as sorted frame indices: detected from the frames' edits when
    // that works (see DetectKeyframes in sequence.h), else chosen by hand.
    std::vector<int> keyframes_;
    bool keyframesAuto_ = false;
    bool keyframesDefaulted_ = false;  // the first frame stands in until the user picks
    std::string detectionReason_;      // why the last detection failed; empty if it worked
    int settingsGenerationSeen_ = 0;

    // Preview of the selected frame, rendered in the background once the selection
    // and its adjustment have held still for a moment.
    FramePreview framePreview_;
    bool applyAdjustmentToPreview_ = true;
    bool previewWanted_ = false;
    int previewWantedFrame_ = -1;
    double previewWantedExposure_ = 0.0;
    fs::path previewWantedPath_;
    std::chrono::steady_clock::time_point previewWantedSince_;
    std::future<std::expected<LoadResult, std::string>> previewPending_;
    int previewPendingFrame_ = -1;
    double previewPendingExposure_ = 0.0;

    // Writing: requested from the toolbar, prepared once the frames are unlocked.
    bool writeRequested_ = false;
    std::optional<WritePlan> pendingWrite_;
};

}  // namespace r2r
