#include "features/exposure_match.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <system_error>

#include "app/app.h"
#include "common/util.h"
#include "io/dng_loader.h"
#include "features/image_viewer.h"
#include "features/raw_processor.h"
#include "features/settings.h"
#include "features/video_render.h"
#include "net/http_client.h"

namespace r2r {

namespace {

constexpr float kSequencePaneWidth = 380.0f;
constexpr float kGraphHeight = 280.0f;
constexpr float kListThumbHeight = 40.0f;

// Long side of the preview rendered for the frame selected in a sequence.
constexpr uint32_t kFramePreviewSize = 1200;
constexpr double kFramePreviewDelay = 0.25;  // seconds the selection must hold before rendering

// How many differing settings the write popup lists before summing up the rest.
constexpr size_t kMaxListedDifferences = 12;

// crs: properties that are not part of the look: the file's own name, the writer's
// version, and the exposure this tool sets itself.
bool IgnoredForComparison(const std::string& name) {
    return name == "RawFileName" || name == "Version" || name == "Exposure2012";
}

}  // namespace

ExposureMatchFeature::ExposureMatchFeature(App& app)
    : Feature(app, {"Exposure Match",
                    "Meter a folder (or a set) of DNGs and write the exposure adjustments that make a time-lapse "
                    "follow a smooth curve through its keyframes, as a CSV and XMP sidecars carrying the keyframes' edits."}),
      filmstrip_(app) {
    AddAction("Open sequence folder...", "Ctrl+Shift+O", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O,
              [this] { ShowOpenFolderDialog(); });
    AddAction("Open sequence files...", "Ctrl+Alt+Shift+O", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_O,
              [this] { ShowOpenFilesDialog(); });
    filmstrip_.contextMenu = [this] {
        if (ImGui::MenuItem("Open in viewer", "Enter")) {
            OpenSelectedInViewer();
        }
        if (ImGui::MenuItem("Use in Raw Processor")) {
            if (auto* processor = app_.Find<RawProcessorFeature>()) {
                processor->UseFilmstrip(filmstrip_, this);
            }
        }
    };
}

// ---- Opening and closing -----------------------------------------------------------

int ExposureMatchFeature::OpenScore(const fs::path& path) const {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        return 0;
    }
    // A folder of DNGs is a sequence to analyze. Any other folder is claimed too (at
    // a low score) so the user gets the "no DNG files" message instead of nothing.
    return FolderHas(path, IsDngPath) ? 10 : 1;
}

void ExposureMatchFeature::Open(const fs::path& path) {
    OpenSequence({.folder = path}, static_cast<size_t>(std::max(0, limitInput_)));
}

std::string ExposureMatchFeature::DialogStart() const {
    return sequence_ ? Utf8(sequence_->Folder()) : std::string();
}

void ExposureMatchFeature::ShowOpenFolderDialog() {
    app_.Dialogs().PickFolder(DialogStart(), [this](const fs::path& folder) {
        app_.Activate(this);
        Open(folder);
    });
}

// Ctrl+A in the dialog takes a whole folder; Ctrl/Shift+click picks some of its frames.
void ExposureMatchFeature::ShowOpenFilesDialog() {
    static const SDL_DialogFileFilter filters[] = {{"DNG images", "dng"}};
    app_.Dialogs().PickFiles(filters, DialogStart(), [this](const std::vector<fs::path>& paths) {
        SequenceSource source{.files = paths};
        std::ranges::sort(source.files);
        const auto [first, last] = std::ranges::unique(source.files);
        source.files.erase(first, last);
        app_.Activate(this);
        OpenSequence(std::move(source), static_cast<size_t>(std::max(0, limitInput_)));
    });
}

std::vector<fs::path> ExposureMatchFeature::SequenceSource::Frames() const {
    return folder.empty() ? files : ListDngs(folder);
}

std::string ExposureMatchFeature::SequenceSource::Description() const {
    if (!folder.empty()) {
        return Utf8(folder);
    }
    if (files.size() == 1) {
        return Utf8(files.front());
    }
    return std::format("{} files in {}", files.size(), files.empty() ? std::string() : Utf8(files.front().parent_path()));
}

std::string ExposureMatchFeature::WindowTitle() const {
    if (!sequence_) {
        return Feature::WindowTitle();
    }
    const std::string name = Utf8(sequence_->Folder().filename());
    if (!source_.folder.empty()) {
        return std::format("{} ({} frames)", name, sequence_->Count());
    }
    return std::format("{} ({} chosen frames)", name, sequence_->Count());
}

void ExposureMatchFeature::Shutdown() {
    CloseSequence();
    filmstrip_.Shutdown();
    if (previewPending_.valid()) {
        previewPending_.wait();
    }
}

void ExposureMatchFeature::CloseSequence() {
    sequence_.reset();  // joins the analysis thread
    filmstrip_.Clear();
    SDL_DestroyTexture(framePreview_.texture);
    framePreview_ = FramePreview{};
    previewWanted_ = false;
    selectedFrame_ = -1;
    status_.clear();
    keyframes_.clear();
    keyframesAuto_ = false;
    keyframesDefaulted_ = false;
    detectionReason_.clear();
    settingsGenerationSeen_ = 0;
    writeRequested_ = false;
    pendingWrite_.reset();
}

// The metering runs on the exposure-match server chosen in Settings. While that
// server is still starting (or not yet checked), the sequence waits for it.
void ExposureMatchFeature::OpenSequence(SequenceSource source, size_t limit) {
    CloseSequence();
    pendingOpen_.reset();
    ServerManager& server = app_.MeterServer();
    if (!server.Health().reachable) {
        if (server.Starting() || !server.Checked()) {
            pendingOpen_ = std::make_pair(std::move(source), limit);
            error_.clear();
            server.CheckNow();
            return;
        }
        error_ = std::format("No exposure-match server is running at {}{}{}. Start one in Settings, then open the sequence again.",
                             server.Address(), server.Health().error.empty() ? "" : ": ", server.Health().error);
        return;
    }
    MeterEndpoint endpoint;
    if (!ParseAddress(server.Address(), endpoint.host, endpoint.port)) {
        error_ = "The exposure-match server address in Settings is not host:port.";
        return;
    }
    auto analysis = std::make_unique<SequenceAnalysis>(source.Frames(), limit, endpoint,
                                                       static_cast<unsigned>(app_.GetSettings().meterParallel));
    if (analysis->Count() == 0) {
        error_ = source.folder.empty() ? "None of the chosen files is a DNG."
                                       : std::format("No DNG files in {}", Utf8(source.folder));
        return;
    }
    sequence_ = std::move(analysis);
    source_ = std::move(source);
    filmstrip_.SetFiles(sequence_->Paths(), 0);
    selectedFrame_ = 0;
    error_.clear();
    sequence_->Start();
}

void ExposureMatchFeature::OpenFrameInViewer(const SequenceFrame& frame, const FrameExposure& exposure) {
    if (ImageViewerFeature* viewer = app_.Find<ImageViewerFeature>()) {
        viewer->OpenWithExposure(frame.path, exposure.adjustmentEv);
        app_.Activate(viewer, this);
    }
}

void ExposureMatchFeature::OpenSelectedInViewer() {
    if (!sequence_ || selectedFrame_ < 0) {
        return;
    }
    sequence_->WithFrames([&](const std::vector<SequenceFrame>& frames) {
        const size_t i = static_cast<size_t>(selectedFrame_);
        if (i < frames.size()) {
            const std::vector<FrameExposure> exposures = ComputeExposures(frames, fitSettings_, keyframes_);
            OpenFrameInViewer(frames[i], exposures[i]);  // the adjustment is empty until the frame is metered
        }
    });
}

void ExposureMatchFeature::SelectFrame(int index, int count) {
    if (count <= 0) {
        return;
    }
    selectedFrame_ = std::clamp(index, 0, count - 1);
    scrollToSelected_ = true;
    if (filmstrip_.Active() != selectedFrame_) {
        filmstrip_.Select(selectedFrame_);
        filmstrip_.TakeActiveChanged();
    }
}

// ---- Keyframes --------------------------------------------------------------------------

// Takes the frames with edits as keyframes when they qualify. Otherwise the user's
// own choice stays, or the first frame stands in until they make one.
void ExposureMatchFeature::DetectKeyframes(const std::vector<SequenceFrame>& frames) {
    const KeyframeDetection detection = ::DetectKeyframes(frames);
    detectionReason_ = detection.ok ? std::string() : detection.reason;
    if (detection.ok) {
        keyframes_ = detection.edited;
        keyframesAuto_ = true;
        keyframesDefaulted_ = false;
        return;
    }
    keyframesAuto_ = false;
    if (keyframes_.empty() && !frames.empty()) {
        keyframes_ = {0};
        keyframesDefaulted_ = true;
    }
}

void ExposureMatchFeature::ToggleKeyframe(int frame) {
    if (!sequence_ || frame < 0 || static_cast<size_t>(frame) >= sequence_->Count()) {
        return;
    }
    const auto it = std::ranges::lower_bound(keyframes_, frame);
    if (it != keyframes_.end() && *it == frame) {
        keyframes_.erase(it);
    } else {
        keyframes_.insert(it, frame);
    }
    keyframesAuto_ = false;
    keyframesDefaulted_ = false;
}

// ---- Background work -------------------------------------------------------------------

bool ExposureMatchFeature::Busy() const {
    return previewPending_.valid() || previewWanted_ || filmstrip_.Busy() || pendingOpen_.has_value() ||
           (sequence_ && sequence_->Running());
}

void ExposureMatchFeature::Poll() {
    if (pendingOpen_) {
        const ServerManager& server = app_.MeterServer();
        if (server.Health().reachable) {
            auto [source, limit] = std::move(*pendingOpen_);
            pendingOpen_.reset();
            OpenSequence(std::move(source), limit);
        } else if (server.Checked() && !server.Starting()) {
            error_ = std::format("No exposure-match server is running at {}{}{}. Start one in Settings, then open the sequence again.",
                                 server.Address(), server.Health().error.empty() ? "" : ": ", server.Health().error);
            pendingOpen_.reset();
        }
    }
    // The keyframes follow the frames' edits each time those have been (re)read.
    if (sequence_ && sequence_->SettingsGeneration() != settingsGenerationSeen_) {
        settingsGenerationSeen_ = sequence_->SettingsGeneration();
        sequence_->WithFrames([&](const std::vector<SequenceFrame>& frames) { DetectKeyframes(frames); });
    }
    filmstrip_.Poll();
    if (filmstrip_.TakeActiveChanged() && filmstrip_.Active() >= 0) {
        selectedFrame_ = filmstrip_.Active();
        scrollToSelected_ = true;
    }
    PollFramePreview();
}

// Decides which preview the right pane needs; PollFramePreview renders it.
void ExposureMatchFeature::WantFramePreview(const std::vector<SequenceFrame>& frames,
                                            const std::vector<FrameExposure>& exposures) {
    if (selectedFrame_ < 0 || selectedFrame_ >= static_cast<int>(frames.size()) ||
        frames[static_cast<size_t>(selectedFrame_)].state != SequenceFrame::State::Done) {
        previewWanted_ = false;
        return;
    }
    const size_t i = static_cast<size_t>(selectedFrame_);
    const double exposure = applyAdjustmentToPreview_ ? exposures[i].adjustmentEv.value_or(0.0) : 0.0;
    auto same = [&](int frame, double ev) { return frame == selectedFrame_ && std::abs(ev - exposure) < 0.005; };
    if (same(framePreview_.frame, framePreview_.exposure) ||
        (previewPending_.valid() && same(previewPendingFrame_, previewPendingExposure_))) {
        previewWanted_ = false;
        return;
    }
    if (!previewWanted_ || !same(previewWantedFrame_, previewWantedExposure_)) {
        previewWanted_ = true;
        previewWantedFrame_ = selectedFrame_;
        previewWantedExposure_ = exposure;
        previewWantedPath_ = frames[i].path;
        previewWantedSince_ = std::chrono::steady_clock::now();
    }
}

void ExposureMatchFeature::PollFramePreview() {
    if (previewPending_.valid() && previewPending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result = previewPending_.get();
        if (sequence_ && previewPendingFrame_ < static_cast<int>(sequence_->Count())) {
            framePreview_.frame = previewPendingFrame_;
            framePreview_.exposure = previewPendingExposure_;
            if (!result) {
                framePreview_.error = result.error();
            } else if (auto uploaded = app_.UploadTexture(framePreview_.texture, result->image)) {
                framePreview_.error.clear();
                framePreview_.width = static_cast<float>(result->image.width);
                framePreview_.height = static_cast<float>(result->image.height);
                framePreview_.histogram.Set(result->image);
                framePreview_.info = result->doc->Info();
            } else {
                framePreview_.error = uploaded.error();
            }
        }
    }
    if (previewWanted_ && !previewPending_.valid() && SecondsSince(previewWantedSince_) >= kFramePreviewDelay) {
        previewPendingFrame_ = previewWantedFrame_;
        previewPendingExposure_ = previewWantedExposure_;
        previewWanted_ = false;
        const uint32_t size = std::min(kFramePreviewSize, app_.MaxTextureSize());
        previewPending_ = std::async(std::launch::async, LoadAndRender, previewWantedPath_, size,
                                     RenderSettings{previewWantedExposure_}, size);
    }
}

// ---- Writing sidecars -----------------------------------------------------------------------

// Reads the keyframes' edits again (they may have changed in Lightroom since the
// analysis), compares them setting by setting, and opens the confirmation popup.
// Called with the frames unlocked.
void ExposureMatchFeature::PrepareWrite() {
    sequence_->RereadSettings(keyframes_);
    WritePlan plan;
    std::string problem;
    sequence_->WithFrames([&](const std::vector<SequenceFrame>& frames) {
        const std::vector<FrameExposure> exposures = ComputeExposures(frames, fitSettings_, keyframes_);
        for (int k : keyframes_) {
            if (k < 0 || static_cast<size_t>(k) >= frames.size()) {
                continue;
            }
            const SequenceFrame& f = frames[static_cast<size_t>(k)];
            if (f.state != SequenceFrame::State::Done) {
                continue;
            }
            WritePlan::Keyframe key{.frame = k, .name = f.name, .x = exposures[static_cast<size_t>(k)].x,
                                    .settings = f.settings};
            if (auto properties = ListDevelopProperties(f.settings)) {
                key.properties = std::move(*properties);
            } else {
                problem = std::format("cannot read the edits of {}: {}", f.name, properties.error());
            }
            plan.keyframes.push_back(std::move(key));
        }
        std::error_code ec;
        for (size_t i = 0; i < frames.size(); ++i) {
            if (exposures[i].adjustmentEv) {
                ++plan.targets;
                plan.existingSidecars += fs::exists(SidecarPath(frames[i].path), ec) ? 1 : 0;
            }
        }
    });
    if (!problem.empty()) {
        status_ = "Cannot write: " + problem;
        return;
    }
    if (plan.keyframes.empty()) {
        status_ = "Cannot write: none of the keyframes is analyzed";
        return;
    }
    std::ranges::sort(plan.keyframes, {}, &WritePlan::Keyframe::x);

    // Every setting of any keyframe, with its value on each keyframe (null = not set there).
    const size_t n = plan.keyframes.size();
    std::vector<std::string> names;
    std::map<std::string, std::vector<const DevelopProperty*>> byName;
    for (size_t j = 0; j < n; ++j) {
        for (const DevelopProperty& p : plan.keyframes[j].properties) {
            if (IgnoredForComparison(p.name)) {
                continue;
            }
            auto [it, inserted] = byName.try_emplace(p.name, std::vector<const DevelopProperty*>(n, nullptr));
            if (inserted) {
                names.push_back(p.name);
            }
            it->second[j] = &p;
        }
    }
    for (const std::string& name : names) {
        const std::vector<const DevelopProperty*>& values = byName[name];
        const DevelopProperty* first = nullptr;
        bool same = true, numeric = true;
        for (const DevelopProperty* v : values) {
            if (!v) {
                same = numeric = false;
                continue;
            }
            if (!first) {
                first = v;
            } else if (v->value != first->value) {
                same = false;
            }
            numeric = numeric && v->numeric;
        }
        if (same) {
            continue;
        }
        if (numeric && name != "ProcessVersion") {
            WritePlan::Interpolated ip{.name = name};
            std::string text = name + ":";
            for (size_t j = 0; j < n; ++j) {
                ip.samples.push_back(values[j]->value);
                ip.values.push_back(std::stod(values[j]->value));
                text += std::format("{} {} ({})", j ? "," : "", values[j]->value, plan.keyframes[j].name);
            }
            plan.interpolated.push_back(std::move(ip));
            plan.differences.push_back(std::move(text));
        } else {
            const char* why = !first                    ? "not set on every keyframe"
                              : first->composite        ? "not a single value"
                              : name == "ProcessVersion" ? "the process version"
                                                         : "not a number on every keyframe";
            plan.differences.push_back(std::format("{}: differs ({}), so each frame takes its nearest keyframe's", name, why));
        }
    }
    pendingWrite_ = std::move(plan);
    ImGui::OpenPopup("Write sidecar files");
}

void ExposureMatchFeature::PerformWrite(const std::vector<SequenceFrame>& frames,
                                        const std::vector<FrameExposure>& exposures) {
    const WritePlan& plan = *pendingWrite_;
    const fs::path folder = sequence_->Folder();
    if (auto csv = WriteCsv(CsvPath(folder), frames, exposures, fitSettings_, keyframes_); !csv) {
        status_ = "Failed: " + csv.error();
        return;
    }
    std::vector<double> kx;
    for (const WritePlan::Keyframe& k : plan.keyframes) {
        kx.push_back(k.x);
    }
    int written = 0;
    std::vector<std::string> errors;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!exposures[i].adjustmentEv) {
            continue;
        }
        SidecarContent content;
        content.exposure2012 = *exposures[i].adjustmentEv;
        const WritePlan::Keyframe* source = nullptr;
        if (auto own = std::ranges::find(plan.keyframes, static_cast<int>(i), &WritePlan::Keyframe::frame);
            own != plan.keyframes.end()) {
            // A keyframe keeps its own edits, and they stay the user's.
            source = &*own;
            content.generated = false;
        } else {
            // Any other frame takes the nearest keyframe's (the earlier one halfway
            // between two), with the differing numbers interpolated.
            size_t best = 0;
            for (size_t j = 1; j < kx.size(); ++j) {
                if (std::abs(kx[j] - exposures[i].x) < std::abs(kx[best] - exposures[i].x)) {
                    best = j;
                }
            }
            source = &plan.keyframes[best];
            for (const WritePlan::Interpolated& ip : plan.interpolated) {
                content.overrides.emplace_back(
                    ip.name, FormatLikeSamples(InterpolateSmooth(kx, ip.values, exposures[i].x), ip.samples));
            }
        }
        if (source->settings.settingCount > 0) {
            content.base = &source->settings;
        }
        content.processVersion = source->settings.processVersion.empty() ? "11.0" : source->settings.processVersion;
        if (auto r = WriteSidecar(frames[i].path, content)) {
            ++written;
        } else {
            errors.push_back(r.error());
        }
    }
    status_ = std::format("Wrote {} and {} .xmp file(s){}", Utf8(CsvPath(folder).filename()), written,
                          plan.interpolated.empty() ? "" : " with interpolated settings");
    if (!errors.empty()) {
        status_ += std::format("; {} failed ({})", errors.size(), errors.front());
    }
    // The sidecars now carry the keyframes' edits (and mark the other frames as
    // generated), so reading them back makes the keyframes the detected ones.
    sequence_->ReloadSettings();
}

void ExposureMatchFeature::DrawWritePopup(const std::vector<SequenceFrame>& frames,
                                          const std::vector<FrameExposure>& exposures) {
    const ImVec2 viewportCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(viewportCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(600 * Dpi(), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Write sidecar files", nullptr, ImGuiWindowFlags_NoResize)) {
        return;
    }
    const WritePlan& plan = *pendingWrite_;
    std::string keyframes;
    for (const WritePlan::Keyframe& k : plan.keyframes) {
        keyframes += std::format("{}{} ({})", keyframes.empty() ? "" : ", ", k.name,
                                 k.settings.settingCount ? std::format("{} settings", k.settings.settingCount) : "no edits");
    }
    ImGui::TextWrapped(
        "Writes %s and a sidecar .xmp for each of the %d analyzed frames into\n%s\n\n"
        "Each sidecar holds the edits of the frame's nearest keyframe (%s) with Exposure2012 set to the frame's "
        "adjustment; the keyframes keep their own edits. Camera Raw and Lightroom use the sidecars in place of any "
        "settings stored in the DNGs.\n\n%d existing .xmp file(s) will be replaced.",
        Utf8(CsvPath(sequence_->Folder()).filename()).c_str(), plan.targets, Utf8(sequence_->Folder()).c_str(),
        keyframes.c_str(), plan.existingSidecars);
    const bool differ = !plan.differences.empty();
    if (differ) {
        ImGui::Spacing();
        ImGui::TextColored(kWarningColor, "The keyframes' edits differ in more than the exposure:");
        for (size_t i = 0; i < std::min(plan.differences.size(), kMaxListedDifferences); ++i) {
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextWrapped("%s", plan.differences[i].c_str());
        }
        if (plan.differences.size() > kMaxListedDifferences) {
            ImGui::TextDisabled("... and %d more", static_cast<int>(plan.differences.size() - kMaxListedDifferences));
        }
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Interpolate: the numbers above change smoothly from keyframe to keyframe (rounded the way Lightroom "
            "writes them); any other setting switches halfway between keyframes. Quit: write nothing, so you can "
            "check the keyframes in Lightroom first.");
    }
    ImGui::Spacing();
    bool close = false;
    if (ImGui::Button(differ ? "Interpolate and write" : "Write", ImVec2(differ ? 170.0f : 120.0f, 0))) {
        PerformWrite(frames, exposures);
        close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(differ ? "Quit" : "Cancel", ImVec2(120, 0))) {
        close = true;
    }
    if (close) {
        pendingWrite_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---- Menus and shortcuts ------------------------------------------------------------------------

void ExposureMatchFeature::DrawMenus() {
    if (sequence_ && ImGui::BeginMenu("Sequence")) {
        if (ImGui::MenuItem("Cancel analysis", nullptr, false, sequence_->Running())) {
            sequence_->Cancel();
        }
        if (ImGui::MenuItem("Re-analyze", nullptr, false, !sequence_->Running())) {
            OpenSequence(source_, static_cast<size_t>(std::max(0, limitInput_)));
        }
        if (ImGui::MenuItem("Reload edits", nullptr, false, !sequence_->Running())) {
            sequence_->ReloadSettings();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Toggle keyframe", "K", false, selectedFrame_ >= 0)) {
            ToggleKeyframe(selectedFrame_);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Close sequence")) {
            CloseSequence();
        }
        ImGui::EndMenu();
    }
}

void ExposureMatchFeature::HandleShortcuts() {
    if (!sequence_) {
        return;
    }
    const int count = static_cast<int>(sequence_->Count());
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        SelectFrame(selectedFrame_ - 1, count);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        SelectFrame(selectedFrame_ + 1, count);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        SelectFrame(0, count);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End)) {
        SelectFrame(count - 1, count);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && selectedFrame_ >= 0) {
        OpenSelectedInViewer();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_K, false) && selectedFrame_ >= 0) {
        ToggleKeyframe(selectedFrame_);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
        filmstrip_.SetVisible(!filmstrip_.Visible());
    }
}

// ---- Drawing -----------------------------------------------------------------------------------------

void ExposureMatchFeature::Draw() {
    if (sequence_) {
        DrawSequenceLayout();
    } else {
        DrawEmpty();
    }
}

void ExposureMatchFeature::DrawEmpty() {
    ImGui::BeginChild("##empty", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::SeparatorText("Exposure Match");
    ImGui::TextWrapped("%s", Info().description.c_str());
    ImGui::Spacing();
    const ImVec2 buttonSize(220 * Dpi(), 0);
    if (ImGui::Button("Open sequence folder...", buttonSize)) {
        ShowOpenFolderDialog();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Every DNG in the folder, in name order.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open DNG files...", buttonSize)) {
        ShowOpenFilesDialog();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Only the DNGs you choose (Ctrl+click or Shift+click in the dialog), in name order.");
    }
    ImGui::Spacing();
    if (!error_.empty()) {
        ErrorText(error_);
        ImGui::Spacing();
    }
    if (pendingOpen_) {
        ImGui::TextColored(kWarningColor, "Waiting for the exposure-match server to start, then opening %s...",
                           pendingOpen_->first.Description().c_str());
        ImGui::Spacing();
    }
    ImGui::TextDisabled("Choose a folder of DNGs (Ctrl+Shift+O) or some of its files (Ctrl+Alt+Shift+O), "
                        "or drop a folder on the window.");
    ImGui::Spacing();
    const ServerManager& server = app_.MeterServer();
    if (server.Health().reachable) {
        ImGui::TextDisabled("Metering: %s server at %s.", server.Health().tool.c_str(), server.Address().c_str());
    } else {
        ImGui::TextColored(kWarningColor, "No exposure-match server at %s%s%s.", server.Address().c_str(),
                           server.Health().error.empty() ? "" : ": ", server.Health().error.c_str());
        if (ImGui::Button("Open Settings")) {
            if (auto* settings = app_.Find<SettingsFeature>()) {
                settings->Show();
            }
        }
    }
    ImGui::EndChild();
}

void ExposureMatchFeature::DrawGraphControls(const std::vector<SequenceFrame>& frames, bool usesTime) {
    const float dpi = Dpi();
    int axis = static_cast<int>(fitSettings_.xAxis);
    ImGui::SetNextItemWidth(120 * dpi);
    if (ImGui::Combo("X axis", &axis, "Frame #\0Capture time\0")) {
        fitSettings_.xAxis = static_cast<XAxis>(axis);
    }
    if (fitSettings_.xAxis == XAxis::CaptureTime && !usesTime) {
        ImGui::SameLine();
        ImGui::TextDisabled("(no capture times yet)");
    }
    ImGui::SameLine(0, 14 * dpi);
    int compare = static_cast<int>(fitSettings_.compare);
    ImGui::SetNextItemWidth(130 * dpi);
    if (ImGui::Combo("Compare with", &compare, "First frame\0Previous frame\0")) {
        fitSettings_.compare = static_cast<CompareMode>(compare);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("First frame: every frame is metered against the first; no drift, but the scene must stay similar.\n"
                          "Previous frame: neighbors are compared and the differences added up; follows a changing scene.");
    }
    ImGui::SameLine(0, 14 * dpi);
    int model = static_cast<int>(fitSettings_.model);
    ImGui::SetNextItemWidth(130 * dpi);
    if (ImGui::Combo("Fit", &model, "None (measured)\0Polynomial\0Moving average\0")) {
        fitSettings_.model = static_cast<FitModel>(model);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("The scene's brightness is taken to change smoothly: this curve through the measured values\n"
                          "is the estimate of each frame's brightness, and what the adjustments are computed from.");
    }
    if (fitSettings_.model == FitModel::Polynomial) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100 * dpi);
        ImGui::SliderInt("Degree", &fitSettings_.degree, 0, 8);
    } else if (fitSettings_.model == FitModel::MovingAverage) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100 * dpi);
        ImGui::SliderInt("Sigma", &fitSettings_.window, 1, 50);
    }
    ImGui::SameLine(0, 14 * dpi);
    ImGui::Checkbox("Deflicker", &fitSettings_.deflicker);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Off: a frame's difference from the fitted curve is taken as metering noise and left alone;\n"
                          "only the smooth trend and the camera's steps are corrected.\n"
                          "On: every frame's metered brightness is corrected, flicker included.");
    }

    // Keyframes.
    const size_t n = keyframes_.size();
    auto names = [&](size_t limit) {
        std::string text;
        for (size_t i = 0; i < std::min(n, limit); ++i) {
            const int k = keyframes_[i];
            text += (i ? ", " : "") + (static_cast<size_t>(k) < frames.size() ? frames[static_cast<size_t>(k)].name
                                                                               : std::to_string(k + 1));
        }
        return n > limit ? text + ", ..." : text;
    };
    std::string text;
    ImVec4 color = ImGui::GetStyle().Colors[ImGuiCol_Text];
    if (n == 0) {
        text = settingsGenerationSeen_ == 0 ? "Keyframes: reading the frames' edits..."
                                            : "Keyframes: none. Tick frames in the list, or press K on the selected frame.";
        color = kWarningColor;
    } else if (keyframesAuto_) {
        text = std::format("Keyframes: {} detected from the frames' edits: {}", n, names(kMaxAutoKeyframes));
    } else if (keyframesDefaulted_) {
        text = std::format("Keyframes: none detected ({}). The first frame stands in until you tick others in the list.",
                           detectionReason_);
        color = kWarningColor;
    } else {
        text = std::format("Keyframes: {} chosen by hand: {}{}", n, names(8),
                           detectionReason_.empty() ? "" : std::format(" (none detected: {})", detectionReason_));
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(color, "%s", text.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("A keyframe's edits (its Exposure2012 above all) set the look at that point; the frames between\n"
                          "follow a smooth curve from one keyframe to the next.\n"
                          "Detected automatically: every frame with edits, when there are 1 to %d of them and no more than\n"
                          "a third of the frames. Sidecars written here for the other frames do not count as edits.",
                          kMaxAutoKeyframes);
    }
    ImGui::SameLine(0, 12 * dpi);
    if (ImGui::SmallButton("Detect again")) {
        DetectKeyframes(frames);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Takes the frames with edits as keyframes again, as read at the last analysis or Reload edits.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(n == 0);
    if (ImGui::SmallButton("Clear")) {
        keyframes_.clear();
        keyframesAuto_ = false;
        keyframesDefaulted_ = false;
    }
    ImGui::EndDisabled();
}

void ExposureMatchFeature::DrawGraph(const std::vector<SequenceFrame>& frames,
                                     const std::vector<FrameExposure>& exposures, bool usesTime) {
    const float dpi = Dpi();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 50 || avail.y < 50) {
        return;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(18, 18, 20, 255), 3.0f * dpi);
    ImGui::InvisibleButton("##graph", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();

    const ImVec2 p0{origin.x + 56 * dpi, origin.y + 10 * dpi};
    const ImVec2 p1{origin.x + avail.x - 14 * dpi, origin.y + avail.y - 22 * dpi};
    if (p1.x <= p0.x || p1.y <= p0.y) {
        return;
    }

    // Value range over everything plotted.
    double ymin = 1e9, ymax = -1e9, xmin = 1e18, xmax = -1e18;
    bool any = false;
    for (size_t i = 0; i < frames.size(); ++i) {
        for (const std::optional<double>& v :
             {frames[i].cameraEv, exposures[i].measuredEv, exposures[i].fittedEv, exposures[i].targetEv}) {
            if (v && frames[i].state == SequenceFrame::State::Done) {
                ymin = std::min(ymin, *v);
                ymax = std::max(ymax, *v);
                xmin = std::min(xmin, exposures[i].x);
                xmax = std::max(xmax, exposures[i].x);
                any = true;
            }
        }
    }
    if (!any) {
        const char* text = "Waiting for the first frame...";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2((p0.x + p1.x - ts.x) / 2, (p0.y + p1.y - ts.y) / 2), IM_COL32(140, 140, 140, 255), text);
        return;
    }
    // Show the whole sequence on the x axis even before it is all analyzed.
    for (const FrameExposure& e : exposures) {
        xmin = std::min(xmin, e.x);
        xmax = std::max(xmax, e.x);
    }
    const double yspan = std::max(ymax - ymin, 0.1);
    ymin = (ymin + ymax) / 2 - yspan * 0.6;
    ymax = (ymin + yspan * 1.2);
    if (xmax <= xmin) {
        xmax = xmin + 1.0;
    }
    const double xpad = (xmax - xmin) * 0.02;
    xmin -= xpad;
    xmax += xpad;
    auto X = [&](double x) { return static_cast<float>(p0.x + (x - xmin) / (xmax - xmin) * (p1.x - p0.x)); };
    auto Y = [&](double y) { return static_cast<float>(p1.y - (y - ymin) / (ymax - ymin) * (p1.y - p0.y)); };

    // Grid and axis labels.
    static constexpr double kEvSteps[] = {0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0};
    const double ystep = NiceStep((ymax - ymin) / 6.0, kEvSteps);
    const ImU32 gridColor = IM_COL32(50, 50, 54, 255);
    const ImU32 labelColor = IM_COL32(150, 150, 150, 255);
    for (double y = std::ceil(ymin / ystep) * ystep; y <= ymax; y += ystep) {
        draw->AddLine(ImVec2(p0.x, Y(y)), ImVec2(p1.x, Y(y)), gridColor);
        const std::string label = ystep < 0.1 ? std::format("{:+.2f}", y) : std::format("{:+.1f}", y);
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        draw->AddText(ImVec2(p0.x - ts.x - 6 * dpi, Y(y) - ts.y / 2), labelColor, label.c_str());
    }
    if (usesTime) {
        static constexpr double kTimeSteps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200};
        const double xstep = NiceStep((xmax - xmin) / 8.0, kTimeSteps);
        for (double x = std::ceil(std::max(0.0, xmin) / xstep) * xstep; x <= xmax; x += xstep) {
            draw->AddLine(ImVec2(X(x), p0.y), ImVec2(X(x), p1.y), gridColor);
            const std::string label = ClockText(x);
            const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            draw->AddText(ImVec2(X(x) - ts.x / 2, p1.y + 4 * dpi), labelColor, label.c_str());
        }
    } else {
        static constexpr double kFrameSteps[] = {1, 2, 5, 10, 20, 25, 50, 100, 200, 500, 1000};
        const double xstep = NiceStep((xmax - xmin) / 10.0, kFrameSteps);
        for (double x = std::ceil(std::max(0.0, xmin) / xstep) * xstep; x <= xmax; x += xstep) {
            draw->AddLine(ImVec2(X(x), p0.y), ImVec2(X(x), p1.y), gridColor);
            const std::string label = std::format("#{}", static_cast<int>(x) + 1);
            const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            draw->AddText(ImVec2(X(x) - ts.x / 2, p1.y + 4 * dpi), labelColor, label.c_str());
        }
    }

    const ImU32 cameraColor = IM_COL32(150, 150, 150, 255);
    const ImU32 measuredColor = IM_COL32(90, 150, 240, 255);
    const ImU32 fitColor = IM_COL32(245, 160, 60, 255);
    const ImU32 targetColor = IM_COL32(120, 220, 140, 255);
    const ImU32 keyframeColor = IM_COL32(255, 220, 90, 255);
    draw->PushClipRect(p0, p1, true);

    // Selected frame, and the keyframes' positions.
    if (selectedFrame_ >= 0 && selectedFrame_ < static_cast<int>(frames.size())) {
        const float x = X(exposures[static_cast<size_t>(selectedFrame_)].x);
        draw->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 60), 2.0f);
    }
    for (int k : keyframes_) {
        if (k >= 0 && static_cast<size_t>(k) < frames.size()) {
            const float x = X(exposures[static_cast<size_t>(k)].x);
            draw->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 220, 90, 50), 1.0f);
        }
    }

    // Camera settings as a step line.
    std::optional<ImVec2> last;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].state != SequenceFrame::State::Done || !frames[i].cameraEv) {
            continue;
        }
        const ImVec2 p{X(exposures[i].x), Y(*frames[i].cameraEv)};
        if (last) {
            draw->AddLine(*last, ImVec2(p.x, last->y), cameraColor, 1.5f);
            draw->AddLine(ImVec2(p.x, last->y), p, cameraColor, 1.5f);
        }
        last = p;
    }

    // Fit, the target curve, then the measured points on top.
    for (int pass = 0; pass < 3; ++pass) {
        last.reset();
        for (size_t i = 0; i < frames.size(); ++i) {
            const std::optional<double>& v = pass == 0   ? exposures[i].fittedEv
                                             : pass == 1 ? exposures[i].targetEv
                                                         : exposures[i].measuredEv;
            if (!v) {
                continue;
            }
            const ImVec2 p{X(exposures[i].x), Y(*v)};
            if (last) {
                const ImU32 color = pass == 0 ? fitColor : pass == 1 ? targetColor : IM_COL32(90, 150, 240, 110);
                draw->AddLine(*last, p, color, pass == 2 ? 1.0f : 2.0f);
            }
            if (pass == 2) {
                const float radius = frames.size() > 150 ? 2.0f : 3.0f;
                draw->AddCircleFilled(p, radius * dpi, measuredColor);
            }
            last = p;
        }
    }

    // Keyframes as diamonds on the target curve.
    for (int k : keyframes_) {
        if (k < 0 || static_cast<size_t>(k) >= frames.size() || !exposures[static_cast<size_t>(k)].targetEv) {
            continue;
        }
        const FrameExposure& e = exposures[static_cast<size_t>(k)];
        const float r = 6.0f * dpi;
        const ImVec2 c{X(e.x), Y(*e.targetEv)};
        const ImVec2 points[] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
        draw->AddConvexPolyFilled(points, 4, keyframeColor);
        draw->AddPolyline(points, 4, IM_COL32(20, 20, 20, 255), ImDrawFlags_Closed, 1.5f);
    }
    draw->PopClipRect();

    // Legend.
    {
        float x = p0.x + 8 * dpi;
        const float y = p0.y + 4 * dpi;
        for (const auto& [color, text] : {std::pair{cameraColor, "Camera settings"},
                                          std::pair{measuredColor, "Scene brightness (measured)"},
                                          std::pair{fitColor, "Fit (estimate)"},
                                          std::pair{targetColor, "Target"},
                                          std::pair{keyframeColor, "Keyframe"}}) {
            draw->AddRectFilled(ImVec2(x, y + 3 * dpi), ImVec2(x + 10 * dpi, y + 13 * dpi), color);
            draw->AddText(ImVec2(x + 14 * dpi, y), IM_COL32(200, 200, 200, 255), text);
            x += 14 * dpi + ImGui::CalcTextSize(text).x + 16 * dpi;
        }
    }

    // Hover: nearest frame along x. Left click selects it, right click toggles it as a keyframe.
    if (hovered) {
        const ImVec2 mouse = ImGui::GetMousePos();
        int nearest = -1;
        float best = 1e9f;
        for (size_t i = 0; i < frames.size(); ++i) {
            if (frames[i].state != SequenceFrame::State::Done) {
                continue;
            }
            const float d = std::abs(X(exposures[i].x) - mouse.x);
            if (d < best) {
                best = d;
                nearest = static_cast<int>(i);
            }
        }
        if (nearest >= 0 && best < 20 * dpi) {
            const SequenceFrame& f = frames[static_cast<size_t>(nearest)];
            const FrameExposure& e = exposures[static_cast<size_t>(nearest)];
            const float x = X(e.x);
            draw->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 40));
            ImGui::SetTooltip("%s%s\nCamera %s EV   Raw vs first %s EV\nScene %s EV   Fit %s EV\n"
                              "Target %s EV   Adjustment %s EV\n\nClick: select   Right-click: toggle keyframe",
                              f.name.c_str(), IsKeyframe(nearest) ? " (keyframe)" : "", EvText(f.cameraEv).c_str(),
                              EvText(e.rawDeltaEv).c_str(), EvText(e.measuredEv).c_str(), EvText(e.fittedEv).c_str(),
                              EvText(e.targetEv).c_str(), EvText(e.adjustmentEv).c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                SelectFrame(nearest, static_cast<int>(frames.size()));
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ToggleKeyframe(nearest);
            }
        }
    }
}

void ExposureMatchFeature::DrawFrameList(const std::vector<SequenceFrame>& frames,
                                         const std::vector<FrameExposure>& exposures) {
    const float dpi = Dpi();
    const float thumbHeight = kListThumbHeight * dpi;
    const float rowHeight = thumbHeight + ImGui::GetStyle().CellPadding.y * 2;
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("frames", 16, flags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 36 * dpi);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 34 * dpi);
    ImGui::TableSetupColumn("##thumb", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,
                            thumbHeight * 1.5f);
    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed, 110 * dpi);
    ImGui::TableSetupColumn("Scene EV", ImGuiTableColumnFlags_WidthFixed, 72 * dpi);
    ImGui::TableSetupColumn("Target EV", ImGuiTableColumnFlags_WidthFixed, 72 * dpi);
    ImGui::TableSetupColumn("Adjust EV", ImGuiTableColumnFlags_WidthFixed, 76 * dpi);
    ImGui::TableSetupColumn("Fit EV", ImGuiTableColumnFlags_WidthFixed, 66 * dpi);
    ImGui::TableSetupColumn("Raw vs 1st", ImGuiTableColumnFlags_WidthFixed, 76 * dpi);
    ImGui::TableSetupColumn("Camera EV", ImGuiTableColumnFlags_WidthFixed, 76 * dpi);
    ImGui::TableSetupColumn("Shutter", ImGuiTableColumnFlags_WidthFixed, 60 * dpi);
    ImGui::TableSetupColumn("f", ImGuiTableColumnFlags_WidthFixed, 44 * dpi);
    ImGui::TableSetupColumn("ISO", ImGuiTableColumnFlags_WidthFixed, 50 * dpi);
    ImGui::TableSetupColumn("Capture time", ImGuiTableColumnFlags_WidthFixed, 150 * dpi);
    ImGui::TableSetupColumn("Spread", ImGuiTableColumnFlags_WidthFixed, 56 * dpi);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(frames.size()), rowHeight);
    if (scrollToSelected_ && selectedFrame_ >= 0) {
        clipper.IncludeItemByIndex(selectedFrame_);
    }
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const size_t i = static_cast<size_t>(row);
            const SequenceFrame& f = frames[i];
            const FrameExposure& e = exposures[i];
            const bool done = f.state == SequenceFrame::State::Done;
            const bool keyframe = IsKeyframe(row);
            ImGui::PushID(row);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            if (keyframe) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 220, 90, 30));
            }
            ImGui::TableNextColumn();
            const std::string label = std::to_string(row + 1);
            if (ImGui::Selectable(label.c_str(), selectedFrame_ == row,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, thumbHeight))) {
                selectedFrame_ = row;
                filmstrip_.Select(row);
                filmstrip_.TakeActiveChanged();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && done) {
                OpenFrameInViewer(f, e);
            }
            if (scrollToSelected_ && selectedFrame_ == row) {
                ImGui::SetScrollHereY(0.5f);
                scrollToSelected_ = false;
            }

            ImGui::TableNextColumn();
            {
                bool key = keyframe;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (thumbHeight - ImGui::GetFrameHeight()) / 2);
                if (ImGui::Checkbox("##key", &key)) {
                    ToggleKeyframe(row);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip("Keyframe: this frame's edits set the look here (K).");
                }
            }

            ImGui::TableNextColumn();
            {
                float aspect = 1.5f;
                if (SDL_Texture* tex = filmstrip_.Thumbnail(f.path, &aspect)) {
                    const float w = thumbHeight * aspect;
                    ImGui::Image(TextureRef(tex), ImVec2(std::min(w, thumbHeight * 1.5f), thumbHeight));
                }
            }

            auto cell = [](const std::string& text) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(text.c_str());
            };
            cell(f.name);
            cell(EvText(e.measuredEv));
            cell(EvText(e.targetEv));
            ImGui::TableNextColumn();
            if (e.adjustmentEv) {
                ImGui::TextColored(kWarningColor, "%s", EvText(e.adjustmentEv).c_str());
            }
            cell(EvText(e.fittedEv));
            cell(EvText(e.rawDeltaEv));
            cell(EvText(f.cameraEv));
            cell(ShutterText(f.capture.exposureTime));
            cell(ApertureText(f.capture.fNumber));
            cell(f.capture.iso ? std::to_string(f.capture.iso) : "");
            cell(f.capture.timeText);
            ImGui::TableNextColumn();
            if (done) {
                const Comparison& cmp = fitSettings_.compare == CompareMode::FirstFrame ? f.toFirst : f.toPrevious;
                const bool noisy = cmp.spreadEv > 0.3 || cmp.percentile;
                ImGui::TextColored(noisy ? kWarningColor : ImGui::GetStyle().Colors[ImGuiCol_Text], "%.3f",
                                   cmp.spreadEv);
            }
            ImGui::TableNextColumn();
            if (f.state == SequenceFrame::State::Failed) {
                ImGui::TextColored(kErrorColor, "%s", f.error.c_str());
            } else if (!done) {
                ImGui::TextDisabled("pending");
            } else if (f.settings.settingCount > 0) {
                ImGui::TextDisabled("Camera Raw now: %s (%s)",
                                    FormatExposure2012(f.settings.exposure2012.value_or(0.0)).c_str(),
                                    f.settings.generated ? "written by Exposure Match" : "edited");
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void ExposureMatchFeature::DrawToolbar(const std::vector<SequenceFrame>& frames,
                                       const std::vector<FrameExposure>& exposures) {
    (void)exposures;
    const float dpi = Dpi();
    const int count = static_cast<int>(frames.size());
    int done = 0, failed = 0;
    for (const SequenceFrame& f : frames) {
        done += f.state == SequenceFrame::State::Done;
        failed += f.state == SequenceFrame::State::Failed;
    }

    if (sequence_->Running()) {
        const int read = sequence_->MetadataRead();
        const int completed = sequence_->Completed();
        if (sequence_->Reloading()) {
            ImGui::ProgressBar(static_cast<float>(read) / static_cast<float>(count), ImVec2(240 * dpi, 0),
                               std::format("Reading edits {} / {}", read, count).c_str());
        } else if (read < count && completed == 0) {
            ImGui::ProgressBar(static_cast<float>(read) / static_cast<float>(count), ImVec2(240 * dpi, 0),
                               std::format("Reading frames {} / {}", read, count).c_str());
        } else {
            ImGui::ProgressBar(static_cast<float>(completed) / static_cast<float>(count), ImVec2(240 * dpi, 0),
                               std::format("Analyzing {} / {}", completed, count).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%u comparisons at a time, server at %s:%u", sequence_->Parallel(),
                                sequence_->Endpoint().host.c_str(), sequence_->Endpoint().port);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            sequence_->Cancel();
        }
    } else {
        ImGui::AlignTextToFramePadding();
        if (failed) {
            ImGui::Text("%d of %d frames analyzed, %d failed", done, count, failed);
        } else {
            ImGui::Text("%d of %d frames analyzed", done, count);
        }
        ImGui::SameLine(0, 16 * dpi);
        ImGui::TextUnformatted("Analyze first");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * dpi);
        ImGui::InputInt("##limit", &limitInput_, 0, 0);
        limitInput_ = std::max(0, limitInput_);
        ImGui::SameLine();
        ImGui::TextUnformatted("files (0 = all)");
        ImGui::SameLine();
        if (ImGui::Button("Re-analyze")) {
            reanalyzeRequested_ = true;  // done after the frames are unlocked
        }
    }

    ImGui::BeginDisabled(sequence_->Running() || done == 0 || keyframes_.empty());
    if (ImGui::Button("Write CSV + XMP")) {
        writeRequested_ = true;  // prepared after the frames are unlocked
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(keyframes_.empty()
                              ? "Choose at least one keyframe first."
                              : "Writes exposure_match.csv and one .xmp sidecar per analyzed frame: the keyframes' edits\n"
                                "with Exposure2012 set to the frame's adjustment.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(sequence_->Running());
    if (ImGui::Button("Reload edits")) {
        sequence_->ReloadSettings();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Reads every frame's develop settings again, e.g. after editing keyframes in Lightroom,\n"
                          "and detects the keyframes anew. Nothing is metered again.");
    }
    if (VideoRenderFeature* video = app_.Find<VideoRenderFeature>()) {
        ImGui::SameLine();
        if (ImGui::Button("Render TIFFs to video...")) {
            video->BrowseForFolder(this);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("Choose the folder of TIFFs exported from Lightroom with the sidecars applied,\n"
                              "and encode them as an MP4 video.");
        }
    }
    if (!status_.empty()) {
        ImGui::SameLine(0, 16 * dpi);
        ImGui::TextDisabled("%s", status_.c_str());
    }
}

void ExposureMatchFeature::DrawFramePane(const std::vector<SequenceFrame>& frames,
                                         const std::vector<FrameExposure>& exposures) {
    if (selectedFrame_ < 0 || selectedFrame_ >= static_cast<int>(frames.size())) {
        ImGui::TextDisabled("Select a frame.");
        return;
    }
    const size_t i = static_cast<size_t>(selectedFrame_);
    const SequenceFrame& f = frames[i];
    const FrameExposure& e = exposures[i];
    const bool done = f.state == SequenceFrame::State::Done;

    ImGui::SeparatorText(f.name.c_str());
    {
        bool key = IsKeyframe(selectedFrame_);
        if (ImGui::Checkbox("Keyframe", &key)) {
            ToggleKeyframe(selectedFrame_);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("This frame's edits set the look here; the frames between keyframes follow a smooth curve. (K)");
        }
    }
    if (!done) {
        ImGui::TextDisabled(f.state == SequenceFrame::State::Failed ? "Could not read this frame." : "Not analyzed yet.");
        if (f.state == SequenceFrame::State::Failed) {
            ImGui::TextWrapped("%s", f.error.c_str());
        }
        return;
    }

    // Preview: the rendered one when it is for this frame, else the filmstrip's thumbnail.
    const bool previewReady = framePreview_.texture && framePreview_.frame == selectedFrame_;
    float aspect = 1.5f;
    SDL_Texture* thumb = filmstrip_.Thumbnail(f.path, &aspect);
    SDL_Texture* tex = previewReady ? framePreview_.texture : thumb;
    float w = previewReady ? framePreview_.width : aspect;
    float h = previewReady ? framePreview_.height : 1.0f;
    const float availWidth = ImGui::GetContentRegionAvail().x;
    if (tex && w > 0 && h > 0) {
        const float scale = std::min(availWidth / w, (availWidth * 0.75f) / h);
        ImGui::Image(TextureRef(tex), ImVec2(w * scale, h * scale));
    }
    const double wantedExposure = applyAdjustmentToPreview_ ? e.adjustmentEv.value_or(0.0) : 0.0;
    if (!previewReady || std::abs(framePreview_.exposure - wantedExposure) >= 0.005) {
        ImGui::TextDisabled("Rendering preview...");
    } else if (framePreview_.exposure != 0.0) {
        ImGui::TextDisabled("Rendered with %+.2f EV", framePreview_.exposure);
    } else {
        ImGui::TextDisabled("Rendered as shot");
    }
    if (!framePreview_.error.empty() && framePreview_.frame == selectedFrame_) {
        ImGui::TextColored(kErrorColor, "%s", framePreview_.error.c_str());
    }
    ImGui::Checkbox("Apply adjustment", &applyAdjustmentToPreview_);
    ImGui::SameLine();
    if (ImGui::Button("Open in viewer")) {
        OpenFrameInViewer(f, e);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Histogram");
    DrawHistogramGraph(previewReady ? framePreview_.histogram : HistogramData{});

    ImGui::Spacing();
    ImGui::SeparatorText("Metadata");
    if (ImGui::BeginTable("frameinfo", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        const Comparison& cmp = fitSettings_.compare == CompareMode::FirstFrame ? f.toFirst : f.toPrevious;
        InfoRow("Scene brightness", EvText(e.measuredEv, 3) + " EV");
        InfoRow("Fit", EvText(e.fittedEv, 3) + " EV");
        InfoRow("Target", EvText(e.targetEv, 3) + " EV");
        InfoRow("Adjustment", EvText(e.adjustmentEv, 3) + " EV");
        InfoRow("Raw vs first", EvText(e.rawDeltaEv, 3) + " EV");
        InfoRow("Camera EV", EvText(f.cameraEv, 3));
        InfoRow("Metering",
                std::format("{} vs {}, spread {:.3f} EV, {:.0f}% of pixels", cmp.percentile ? "percentiles" : "pixels",
                            fitSettings_.compare == CompareMode::FirstFrame ? "first frame" : "previous frame",
                            cmp.spreadEv, cmp.validFraction * 100.0));
        if (f.settings.settingCount > 0) {
            InfoRow("Edits", std::format("{}: {} settings, Exposure2012 {}{}", f.settings.source,
                                         f.settings.settingCount,
                                         f.settings.exposure2012 ? FormatExposure2012(*f.settings.exposure2012) : "not set",
                                         f.settings.generated ? " (written by Exposure Match)" : ""));
        } else {
            InfoRow("Edits", "none");
        }
        InfoRow("Camera", f.capture.make + " " + f.capture.model);
        InfoRow("Settings", std::format("{} s  {}  ISO {}", ShutterText(f.capture.exposureTime),
                                        ApertureText(f.capture.fNumber), f.capture.iso));
        InfoRow("Captured", f.capture.timeText);
        InfoRow("Raw size", std::format("{} x {}", f.capture.rawWidth, f.capture.rawHeight));
        if (previewReady) {
            for (const auto& [label, value] : framePreview_.info) {
                if (label == "Lens" || label == "Focal length" || label == "DNG version") {
                    InfoRow(label, value);
                }
            }
        }
        ImGui::EndTable();
    }
}

void ExposureMatchFeature::DrawSequenceLayout() {
    const float dpi = Dpi();
    const float strip = filmstrip_.Height();
    sequence_->WithFrames([&](const std::vector<SequenceFrame>& frames) {
        bool usesTime = false;
        const std::vector<FrameExposure> exposures = ComputeExposures(frames, fitSettings_, keyframes_, &usesTime);

        ImGui::BeginChild("##seqleft", ImVec2(-kSequencePaneWidth * dpi, -strip), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::BeginChild("##graph", ImVec2(0, kGraphHeight * dpi), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawGraphControls(frames, usesTime);
        DrawGraph(frames, exposures, usesTime);
        ImGui::EndChild();

        const float toolbarHeight = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetStyle().WindowPadding.y * 2;
        ImGui::BeginChild("##list", ImVec2(0, -toolbarHeight), ImGuiChildFlags_None);
        DrawFrameList(frames, exposures);
        ImGui::EndChild();

        ImGui::BeginChild("##toolbar", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        DrawToolbar(frames, exposures);
        ImGui::EndChild();
        ImGui::EndChild();

        ImGui::SameLine(0, 0);
        ImGui::BeginChild("##seqpane", ImVec2(0, -strip), ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawFramePane(frames, exposures);
        ImGui::EndChild();

        WantFramePreview(frames, exposures);
    });
    filmstrip_.Draw();

    // Writing re-reads the keyframes' edits, which needs the frames unlocked; the
    // popup then lives at this level of the window, where it was opened.
    if (writeRequested_) {
        writeRequested_ = false;
        PrepareWrite();
    }
    if (pendingWrite_) {
        sequence_->WithFrames([&](const std::vector<SequenceFrame>& frames) {
            const std::vector<FrameExposure> exposures = ComputeExposures(frames, fitSettings_, keyframes_);
            DrawWritePopup(frames, exposures);
        });
    }

    // Replacing the sequence destroys the frames, so it waits until they are unlocked.
    if (reanalyzeRequested_) {
        reanalyzeRequested_ = false;
        OpenSequence(source_, static_cast<size_t>(std::max(0, limitInput_)));
    }
}

R2R_REGISTER_FEATURE(ExposureMatchFeature, 20);

}  // namespace r2r
