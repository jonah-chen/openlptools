#include "features/raw_processor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>

#include "app/app.h"
#include "app/ui.h"
#include "common/util.h"
#include "features/image_viewer.h"

namespace r2r {

namespace {

constexpr float kSettingsPaneWidth = 380.0f;

constexpr const char* kModeLabels = "Mean (a longer exposure)\0Median (what stays still)\0";

std::vector<fs::path> DngsInFolder(const fs::path& folder) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec) && IsDngPath(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

std::string GigabyteText(uint64_t bytes) {
    return std::format("{:.1f} GB", static_cast<double>(bytes) / (1ull << 30));
}

}  // namespace

RawProcessorFeature::RawProcessorFeature(App& app)
    : Feature(app, {"Raw Processor",
                    "Combine a set of DNGs shot in sequence into one DNG: their mean for a longer exposure, or "
                    "their median to keep what stays still."}),
      filmstrip_(app) {}

// ---- Files ----------------------------------------------------------------------------

// Files dropped while this tool is on screen stay here; otherwise the viewer takes them.
int RawProcessorFeature::OpenScore(const fs::path& path) const {
    std::error_code ec;
    if (app_.Current() != this || fs::is_directory(path, ec) || !IsDngPath(path)) {
        return 0;
    }
    return 11;
}

void RawProcessorFeature::Open(const fs::path& path) {
    OpenFiles({path});
}

// The files opened make up the strip, all selected, the first highlighted.
void RawProcessorFeature::OpenFiles(const std::vector<fs::path>& paths) {
    if (paths.empty()) {
        return;
    }
    std::vector<fs::path> files = paths;
    std::ranges::sort(files);
    files.erase(std::ranges::unique(files).begin(), files.end());
    filmstrip_.SetFiles(files, 0);
    filmstrip_.SelectAll();
    job_.reset();
    error_.clear();
    outputEdited_ = false;
}

void RawProcessorFeature::OpenFolder(const fs::path& folder) {
    std::vector<fs::path> files = DngsInFolder(folder);
    if (files.empty()) {
        error_ = std::format("No DNG files in {}", Utf8(folder));
        return;
    }
    OpenFiles(files);
}

void RawProcessorFeature::UseFilmstrip(const Filmstrip& strip, Feature* returnTo) {
    filmstrip_.CopyFrom(strip);
    job_.reset();
    error_.clear();
    outputEdited_ = false;
    app_.Activate(this, returnTo);
}

void RawProcessorFeature::ShowOpenDialog() {
    static const SDL_DialogFileFilter filters[] = {
        {"DNG images", "dng"},
        {"All files", "*"},
    };
    const fs::path* active = filmstrip_.ActivePath();
    const std::string folder = active ? Utf8(active->parent_path()) : std::string();
    app_.Dialogs().PickFiles(filters, folder, [this](const std::vector<fs::path>& paths) {
        app_.Activate(this, app_.ReturnTarget());
        OpenFiles(paths);
    });
}

void RawProcessorFeature::ShowOpenFolderDialog() {
    const fs::path* active = filmstrip_.ActivePath();
    const std::string folder = active ? Utf8(active->parent_path()) : std::string();
    app_.Dialogs().PickFolder(folder, [this](const fs::path& path) {
        app_.Activate(this, app_.ReturnTarget());
        OpenFolder(path);
    });
}

std::string RawProcessorFeature::WindowTitle() const {
    if (filmstrip_.Empty()) {
        return Feature::WindowTitle();
    }
    return std::format("{} ({} of {} frames)", Feature::WindowTitle(), filmstrip_.SelectedCount(), filmstrip_.Count());
}

void RawProcessorFeature::Shutdown() {
    job_.reset();  // cancels and waits
    filmstrip_.Shutdown();
}

// ---- State ------------------------------------------------------------------------------

bool RawProcessorFeature::Busy() const {
    return (job_ && job_->Running()) || filmstrip_.Busy();
}

void RawProcessorFeature::Poll() {
    filmstrip_.Poll();
    filmstrip_.TakeActiveChanged();  // the preview follows the strip directly
    if (!(job_ && job_->Running())) {
        SyncDefaultOutput();
    }
}

// <highlighted stem>_<mode>_x<count>.dng next to the highlighted frame.
fs::path RawProcessorFeature::DefaultOutput() const {
    const fs::path* active = filmstrip_.ActivePath();
    if (!active) {
        return {};
    }
    const std::string name = std::format("{}_{}_x{}.dng", Utf8(active->stem()), StackModeName(mode_),
                                         filmstrip_.SelectedCount());
    return active->parent_path() / fs::path(reinterpret_cast<const char8_t*>(name.c_str()));
}

void RawProcessorFeature::SyncDefaultOutput() {
    if (outputEdited_) {
        return;
    }
    const fs::path wanted = DefaultOutput();
    if (wanted != output_) {
        SetOutput(wanted, false);
    }
}

void RawProcessorFeature::SetOutput(const fs::path& path, bool edited) {
    output_ = path;
    if (!output_.empty() && !output_.has_extension()) {
        output_ += ".dng";
    }
    const std::string text = Utf8(output_);
    std::snprintf(outputText_, sizeof(outputText_), "%s", text.c_str());
    outputEdited_ = edited;
}

const CaptureInfo* RawProcessorFeature::ActiveCapture() {
    const fs::path* active = filmstrip_.ActivePath();
    if (!active) {
        return nullptr;
    }
    auto it = captures_.find(*active);
    if (it == captures_.end()) {
        auto probe = DngDocument::Probe(*active);
        it = captures_.emplace(*active, probe ? std::optional<CaptureInfo>(*probe) : std::nullopt).first;
    }
    return it->second ? &*it->second : nullptr;
}

std::string RawProcessorFeature::Blocker() const {
    const size_t selected = filmstrip_.SelectedCount();
    if (selected < 2) {
        return "Select at least two frames in the filmstrip.";
    }
    const int active = filmstrip_.Active();
    if (active < 0 || !filmstrip_.IsSelected(static_cast<size_t>(active))) {
        return "Highlight one of the selected frames (click it) to take the metadata from.";
    }
    if (outputText_[0] == '\0') {
        return "Choose an output file.";
    }
    return {};
}

void RawProcessorFeature::StartJob() {
    StackRequest request;
    request.frames = filmstrip_.SelectedPaths();
    request.metadataSource = *filmstrip_.ActivePath();
    request.mode = mode_;
    request.output = PathFromUtf8(outputText_);
    SetOutput(request.output, outputEdited_);
    job_ = std::make_unique<StackJob>(std::move(request));
    job_->Start();
}

// ---- Menus and shortcuts ---------------------------------------------------------------------

void RawProcessorFeature::DrawMenus() {
    if (ImGui::BeginMenu("Frames")) {
        if (ImGui::MenuItem("Open DNG files...")) {
            ShowOpenDialog();
        }
        if (ImGui::MenuItem("Open folder...")) {
            ShowOpenFolderDialog();
        }
        ImGui::Separator();
        const bool files = !filmstrip_.Empty();
        if (ImGui::MenuItem("Select all", "Ctrl+A", false, files)) {
            filmstrip_.SelectAll();
        }
        if (ImGui::MenuItem("Deselect all", "Ctrl+D", false, files)) {
            filmstrip_.DeselectAll();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Filmstrip", "F6", filmstrip_.Visible())) {
            filmstrip_.SetVisible(!filmstrip_.Visible());
        }
        ImGui::EndMenu();
    }
}

void RawProcessorFeature::HandleShortcuts() {
    const bool extend = ImGui::GetIO().KeyShift;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        filmstrip_.Step(-1, extend);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        filmstrip_.Step(+1, extend);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
        filmstrip_.StepTo(0, extend);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
        filmstrip_.StepTo(static_cast<int>(filmstrip_.Count()) - 1, extend);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        filmstrip_.SelectAll();
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) {
        filmstrip_.DeselectAll();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
        filmstrip_.SetVisible(!filmstrip_.Visible());
    }
}

// ---- Drawing ------------------------------------------------------------------------------------

void RawProcessorFeature::Draw() {
    const float dpi = Dpi();
    const float strip = filmstrip_.Height();
    ImGui::BeginChild("##settings", ImVec2(kSettingsPaneWidth * dpi, -strip), ImGuiChildFlags_AlwaysUseWindowPadding);
    DrawSettingsPane();
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    ImGui::BeginChild("##preview", ImVec2(0, -strip), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawPreview();
    ImGui::EndChild();

    filmstrip_.Draw();
    DrawConfirmPopup();
}

void RawProcessorFeature::DrawSettingsPane() {
    const float dpi = Dpi();
    StackJob* job = job_.get();
    const bool running = job && job->Running();

    ImGui::BeginDisabled(running);
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Open DNG files...", ImVec2(half, 0))) {
        ShowOpenDialog();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open folder...", ImVec2(-FLT_MIN, 0))) {
        ShowOpenFolderDialog();
    }
    ImGui::EndDisabled();
    if (!error_.empty()) {
        ImGui::Spacing();
        ErrorText(error_);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Frames");
    if (filmstrip_.Empty()) {
        ImGui::TextWrapped("Open the DNGs to combine, or select them in the Image Viewer and choose "
                           "\"Use in Raw Processor\" (right-click the filmstrip, or Ctrl+P).");
        return;
    }
    const size_t selected = filmstrip_.SelectedCount();
    ImGui::Text("%zu of %zu frames selected", selected, filmstrip_.Count());
    ImGui::TextDisabled("Click, Shift+click and Ctrl+click in the filmstrip; Ctrl+A for all.");
    ImGui::Spacing();
    if (const fs::path* active = filmstrip_.ActivePath()) {
        ImGui::TextUnformatted("Metadata and Camera Raw settings from");
        ImGui::SameLine();
        ImGui::TextColored(kWarningColor, "%s", Utf8(active->filename()).c_str());
        if (const CaptureInfo* c = ActiveCapture()) {
            ImGui::TextDisabled("%s %s, %s, %s, ISO %u, %u x %u", c->make.c_str(), c->model.c_str(),
                                ShutterText(c->exposureTime).c_str(), ApertureText(c->fNumber).c_str(), c->iso,
                                c->rawWidth, c->rawHeight);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Processing");
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(-FLT_MIN);
    int mode = static_cast<int>(mode_);
    if (ImGui::Combo("##mode", &mode, kModeLabels)) {
        mode_ = static_cast<StackMode>(mode);
    }
    ImGui::EndDisabled();
    const int count = static_cast<int>(selected);
    if (count >= 2) {
        ImGui::TextWrapped("%s", StackMethodText(mode_, count).c_str());
        if (mode_ == StackMode::Mean) {
            ImGui::TextWrapped("The result is written as the exposure the frames add up to: ISO divided by %d, "
                               "shutter speed multiplied by %d.",
                               count, count);
        } else if (const CaptureInfo* c = ActiveCapture()) {
            const uint64_t needed = MedianMemoryBytes(count, c->rawWidth, c->rawHeight, 1);
            const uint64_t available = AvailableMemoryBytes();
            ImGui::TextColored(needed > available ? kErrorColor : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled],
                               "Needs about %s of memory; %s is free.", GigabyteText(needed).c_str(),
                               GigabyteText(available).c_str());
        }
        if (count > R2R_MAX_STACK_FRAMES) {
            ImGui::TextColored(kErrorColor, "This build stacks at most %d frames (CMake option RAW2RAW2_MAX_STACK_FRAMES).",
                               R2R_MAX_STACK_FRAMES);
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("Every frame must match the others in camera, lens, exposure and raw format.");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::SeparatorText("Output");
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2 +
                              ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::InputText("##output", outputText_, sizeof(outputText_))) {
        output_ = PathFromUtf8(outputText_);
        outputEdited_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        static const SDL_DialogFileFilter filters[] = {{"DNG images", "dng"}};
        app_.Dialogs().PickSaveFile(filters, Utf8(output_), [this](const fs::path& path) { SetOutput(path, true); });
    }
    if (outputEdited_) {
        if (ImGui::SmallButton("Default name")) {
            outputEdited_ = false;
            SyncDefaultOutput();
        }
    }
    {
        std::error_code ec;
        if (!running && outputText_[0] && fs::exists(PathFromUtf8(outputText_), ec)) {
            ImGui::TextColored(kWarningColor, "This file exists and will be replaced.");
        }
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const ImVec2 buttonSize(130 * dpi, 0);
    if (running) {
        const int total = job->Total();
        const float fraction = total > 0 ? static_cast<float>(job->Completed()) / static_cast<float>(total)
                                         : -static_cast<float>(ImGui::GetTime());  // animated
        const std::string label = total > 0 ? std::format("{} / {}", job->Completed(), total) : std::string();
        ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), label.c_str());
        ImGui::TextWrapped("%s...", job->Status().c_str());
        ImGui::TextDisabled("%.0f s", job->Seconds());
        if (ImGui::Button("Cancel", buttonSize)) {
            job->Cancel();
        }
        return;
    }
    if (job && job->Finished()) {
        if (job->Failed()) {
            ErrorText(job->Status());
        } else if (job->Cancelled()) {
            ImGui::TextDisabled("Cancelled.");
        } else {
            ImGui::TextWrapped("%s", job->Status().c_str());
            ImGui::TextDisabled("%s, %.1f s", ByteText(job->OutputBytes()).c_str(), job->Seconds());
            if (ImGui::Button("Open in viewer", buttonSize)) {
                if (auto* viewer = app_.Find<ImageViewerFeature>()) {
                    viewer->Open(job->Request().output);
                    app_.Activate(viewer, this);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Show in folder", buttonSize)) {
                SDL_OpenURL(FileUrl(job->Request().output.parent_path()).c_str());
            }
        }
        ImGui::Spacing();
    }
    const std::string blocker = Blocker();
    ImGui::BeginDisabled(!blocker.empty());
    if (ImGui::Button("Process...", buttonSize)) {
        openConfirm_ = true;
    }
    ImGui::EndDisabled();
    if (!blocker.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", blocker.c_str());
    }
}

// The highlighted frame's thumbnail, large, so the metadata source is in view.
void RawProcessorFeature::DrawPreview() {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x <= 0 || size.y <= 0) {
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(24, 24, 26, 255));
    const fs::path* active = filmstrip_.ActivePath();
    if (!active) {
        return;
    }
    float aspect = 1.0f;
    const float pad = 24.0f * Dpi();
    if (SDL_Texture* thumb = filmstrip_.Thumbnail(*active, &aspect)) {
        const float w = std::min(size.x - 2 * pad, (size.y - 2 * pad) * aspect);
        const float h = w / aspect;
        const ImVec2 p0{origin.x + (size.x - w) * 0.5f, origin.y + (size.y - h) * 0.5f};
        draw->AddImage(TextureRef(thumb), p0, ImVec2(p0.x + w, p0.y + h));
    }
    const std::string caption = std::format("{}  (highlighted: metadata source)", Utf8(active->filename()));
    const ImVec2 extent = ImGui::CalcTextSize(caption.c_str());
    draw->AddText(ImVec2(origin.x + (size.x - extent.x) * 0.5f, origin.y + size.y - pad * 0.5f - extent.y),
                  IM_COL32(200, 200, 205, 255), caption.c_str());
}

// Option A: what the result will carry, confirmed before anything is written.
void RawProcessorFeature::DrawConfirmPopup() {
    if (openConfirm_) {
        ImGui::OpenPopup("Process frames");
        openConfirm_ = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560 * Dpi(), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Process frames", nullptr, ImGuiWindowFlags_NoResize)) {
        return;
    }
    const fs::path* active = filmstrip_.ActivePath();
    const int count = static_cast<int>(filmstrip_.SelectedCount());
    const fs::path output = PathFromUtf8(outputText_);
    std::string text = std::format("Combine the {} selected frames into their {}.\n\n", count, StackModeName(mode_));
    if (active) {
        text += std::format("The result carries the metadata and the Camera Raw settings (XMP) of {}, the "
                            "highlighted frame. Its capture time becomes that of the earliest frame.",
                            Utf8(active->filename()));
    }
    if (mode_ == StackMode::Mean) {
        if (const CaptureInfo* c = ActiveCapture(); c && c->iso && c->exposureTime > 0.0) {
            text += std::format(" As the frames add up to one longer exposure, ISO {} becomes {} and {} becomes {}.",
                                c->iso, std::max(1L, std::lround(static_cast<double>(c->iso) / count)),
                                ShutterText(c->exposureTime), ShutterText(c->exposureTime * count));
        } else {
            text += std::format(" ISO is divided by {} and the shutter speed multiplied by {}.", count, count);
        }
    }
    std::error_code ec;
    text += std::format("\n\nOutput: {}{}", Utf8(output), fs::exists(output, ec) ? "\n(replacing the existing file)" : "");
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::Spacing();
    const ImVec2 buttonSize(120 * Dpi(), 0);
    if (ImGui::Button("Process", buttonSize) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        StartJob();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", buttonSize) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

R2R_REGISTER_FEATURE(RawProcessorFeature, 15);

}  // namespace r2r
