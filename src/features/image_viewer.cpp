#include "features/image_viewer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <vector>

#include "app/app.h"
#include "net/http_client.h"
#include "common/util.h"
#include "features/raw_processor.h"
#include "features/settings.h"
#include "io/tiff_loader.h"

namespace r2r {

namespace {

constexpr float kInfoPanelWidth = 300.0f;
constexpr float kAdjustPanelWidth = 280.0f;
constexpr float kMinZoom = 0.01f;
constexpr float kMaxZoom = 32.0f;
constexpr float kMaxExposure = 5.0f;

// Long side of the quick renders shown while a slider is being dragged; the full
// resolution render follows when the slider is released.
constexpr uint32_t kPreviewSize = 1600;

// The DNGs of a folder in name order.
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

}  // namespace

ImageViewerFeature::ImageViewerFeature(App& app)
    : Feature(app, {"Image Viewer",
                    "Open DNGs: zoom and pan, read the metadata and histogram, adjust the exposure, and pick "
                    "files in the filmstrip for the other tools."}),
      filmstrip_(app) {
    AddAction("Open DNG...", "Ctrl+O", ImGuiMod_Ctrl | ImGuiKey_O, [this] { ShowOpenDialog(); });
    AddAction("Open folder in viewer...", "Ctrl+Alt+O", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_O,
              [this] { ShowOpenFolderDialog(); });
    filmstrip_.contextMenu = [this] {
        if (ImGui::MenuItem("Export selected...", "Ctrl+E")) {
            BeginExport();
        }
        if (ImGui::MenuItem("Use in Raw Processor", "Ctrl+P")) {
            SendToProcessor();
        }
    };
}

// Hands the strip (its selection and highlighted frame) to the Raw Processor.
void ImageViewerFeature::SendToProcessor() {
    if (filmstrip_.Empty()) {
        return;
    }
    if (auto* processor = app_.Find<RawProcessorFeature>()) {
        processor->UseFilmstrip(filmstrip_, this);
    }
}

// ---- Opening files ----------------------------------------------------------------

int ImageViewerFeature::OpenScore(const fs::path& path) const {
    std::error_code ec;
    if (fs::is_directory(path, ec) || IsTiffPath(path)) {
        return 0;
    }
    return IsDngPath(path) ? 10 : 1;
}

// The filmstrip holds exactly what was opened: one file, the files chosen or
// dropped together, or a folder's DNGs.
void ImageViewerFeature::Open(const fs::path& path) {
    OpenFiles({path});
}

void ImageViewerFeature::OpenFiles(const std::vector<fs::path>& paths) {
    if (paths.empty()) {
        return;
    }
    std::vector<fs::path> files = paths;
    std::ranges::sort(files);
    files.erase(std::ranges::unique(files).begin(), files.end());
    filmstrip_.SetFiles(files, 0);
    Load(files.front());
}

void ImageViewerFeature::OpenFolder(const fs::path& folder) {
    std::vector<fs::path> files = DngsInFolder(folder);
    if (files.empty()) {
        error_ = std::format("No DNG files in {}", Utf8(folder));
        return;
    }
    OpenFiles(files);
}

void ImageViewerFeature::Load(const fs::path& path) {
    // Stop showing the previous file at once: until the new one is in, the view
    // shows its thumbnail (or nothing) and the panes show no stale metadata.
    if (doc_ && doc_->Path() != path) {
        doc_.reset();
        error_.clear();
    }
    if (Loading()) {
        queuedPath_ = path;
        return;
    }
    pendingPath_ = path;
    loadStart_ = std::chrono::steady_clock::now();
    pending_ = std::async(std::launch::async, LoadAndRender, path, 0u, RenderSettings{}, app_.MaxTextureSize());
}

void ImageViewerFeature::OpenWithExposure(const fs::path& path, std::optional<double> exposure) {
    exposureOnLoad_ = exposure;
    Open(path);
}

void ImageViewerFeature::ShowOpenDialog() {
    static const SDL_DialogFileFilter filters[] = {
        {"DNG images", "dng"},
        {"All files", "*"},
    };
    const auto current = CurrentPath();
    const std::string folder = current ? Utf8(current->parent_path()) : std::string();
    app_.Dialogs().PickFiles(filters, folder, [this](const std::vector<fs::path>& paths) {
        app_.Activate(this);
        OpenFiles(paths);
    });
}

void ImageViewerFeature::ShowOpenFolderDialog() {
    const auto current = CurrentPath();
    const std::string folder = current ? Utf8(current->parent_path()) : std::string();
    app_.Dialogs().PickFolder(folder, [this](const fs::path& path) {
        app_.Activate(this);
        OpenFolder(path);
    });
}

std::string ImageViewerFeature::WindowTitle() const {
    if (!doc_) {
        return Feature::WindowTitle();
    }
    const size_t selected = filmstrip_.SelectedCount();
    if (selected > 1) {
        return std::format("{} ({} selected)", Utf8(doc_->Path().filename()), selected);
    }
    return Utf8(doc_->Path().filename());
}

void ImageViewerFeature::Shutdown() {
    // Let in-flight work finish before the SDK it uses is torn down.
    if (pending_.valid()) {
        pending_.wait();
    }
    if (rendering_.valid()) {
        rendering_.wait();
    }
    filmstrip_.Shutdown();
    doc_.reset();
    renderingDoc_.reset();
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
}

// ---- Loading and rendering ------------------------------------------------------------

bool ImageViewerFeature::Upload(const RenderedImage& image) {
    if (auto uploaded = app_.UploadTexture(texture_, image); !uploaded) {
        error_ = uploaded.error();
        return false;
    }
    histogram_.Set(image);
    return true;
}

void ImageViewerFeature::Poll() {
    filmstrip_.Poll();
    if (filmstrip_.TakeActiveChanged()) {
        if (const fs::path* path = filmstrip_.ActivePath(); path && *path != CurrentPath()) {
            Load(*path);
        }
    }
    PollLoad();
    PollRender();
    StartRenderIfNeeded();
}

void ImageViewerFeature::PollLoad() {
    if (!Loading() || pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    auto result = pending_.get();
    if (queuedPath_) {
        // A newer file was requested while this one loaded, so skip it.
        const fs::path next = *queuedPath_;
        queuedPath_.reset();
        Load(next);
        return;
    }
    if (!result) {
        error_ = std::format("Could not open {}: {}", Utf8(pendingPath_.filename()), result.error());
        exposureOnLoad_.reset();
        return;
    }
    if (!Upload(result->image)) {
        return;
    }
    doc_ = std::move(result->doc);
    displayWidth_ = static_cast<float>(result->image.width);
    displayHeight_ = static_cast<float>(result->image.height);
    settings_ = shownSettings_ = RenderSettings{};
    if (exposureOnLoad_) {
        settings_.exposure = *exposureOnLoad_;  // the texture shows 0 EV; a render follows
        exposureOnLoad_.reset();
    }
    shownFull_ = true;
    error_.clear();
    fit_ = true;
    loadTime_ = std::format("{:.2f} s", SecondsSince(loadStart_));
    renderTime_.clear();
}

void ImageViewerFeature::PollRender() {
    if (!rendering_.valid() || rendering_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    auto result = rendering_.get();
    const bool current = renderingDoc_ == doc_;
    renderingDoc_.reset();
    if (!current) {
        return;  // a different file was opened meanwhile
    }
    // Record the attempt even on failure, so a bad setting isn't retried every frame.
    shownSettings_ = renderingSettings_;
    shownFull_ = renderingFull_;
    if (!result) {
        error_ = std::format("Could not render: {}", result.error());
        return;
    }
    if (Upload(*result)) {
        renderTime_ = std::format("{:.0f} ms ({} x {})", SecondsSince(renderStart_) * 1000.0, result->width,
                                  result->height);
    }
}

void ImageViewerFeature::StartRenderIfNeeded() {
    if (!doc_ || Loading() || rendering_.valid()) {
        return;
    }
    const bool full = !sliderActive_;
    if (settings_ == shownSettings_ && (shownFull_ || !full)) {
        return;
    }
    renderingDoc_ = doc_;
    renderingSettings_ = settings_;
    renderingFull_ = full;
    renderStart_ = std::chrono::steady_clock::now();
    const uint32_t size = full ? app_.MaxTextureSize() : std::min(kPreviewSize, app_.MaxTextureSize());
    rendering_ = std::async(std::launch::async,
                            [doc = renderingDoc_, s = settings_, size] { return doc->Render(s, size); });
}

// The path the user is looking at, or about to.
std::optional<fs::path> ImageViewerFeature::CurrentPath() const {
    if (Loading()) {
        return queuedPath_.value_or(pendingPath_);
    }
    if (doc_) {
        return doc_->Path();
    }
    return std::nullopt;
}

void ImageViewerFeature::ResetZoomToActualPixels() {
    fit_ = false;
    zoom_ = 1.0f / ImGui::GetIO().DisplayFramebufferScale.x;
}

// ---- Menus and shortcuts -----------------------------------------------------------------

void ImageViewerFeature::DrawMenus() {
    if (ImGui::BeginMenu("View")) {
        const bool image = doc_ != nullptr;
        if (ImGui::MenuItem("Fit to window", "F", fit_, image)) {
            fit_ = true;
        }
        if (ImGui::MenuItem("Actual pixels", "1", false, image)) {
            ResetZoomToActualPixels();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Filmstrip", "F6", filmstrip_.Visible())) {
            filmstrip_.SetVisible(!filmstrip_.Visible());
        }
        const bool files = !filmstrip_.Empty();
        if (ImGui::MenuItem("Previous", "Left", false, files)) {
            filmstrip_.Step(-1, false);
        }
        if (ImGui::MenuItem("Next", "Right", false, files)) {
            filmstrip_.Step(+1, false);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select all", "Ctrl+A", false, files)) {
            filmstrip_.SelectAll();
        }
        if (ImGui::MenuItem("Deselect all", "Ctrl+D", false, files)) {
            filmstrip_.DeselectAll();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Export selected...", "Ctrl+E", false, files)) {
            BeginExport();
        }
        if (ImGui::MenuItem("Use in Raw Processor", "Ctrl+P", false, files)) {
            SendToProcessor();
        }
        ImGui::EndMenu();
    }
}

void ImageViewerFeature::HandleShortcuts() {
    // Arrows step through the filmstrip; with Shift they extend the selection.
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
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P)) {
        SendToProcessor();
        return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E)) {
        BeginExport();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
        filmstrip_.SetVisible(!filmstrip_.Visible());
    }
    if (doc_ && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        fit_ = true;
    }
    if (doc_ && ImGui::IsKeyPressed(ImGuiKey_1, false)) {
        ResetZoomToActualPixels();
    }
}

// ---- Drawing -------------------------------------------------------------------------------

void ImageViewerFeature::DrawInfoPanel() {
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Open DNG...", ImVec2(half, 0))) {
        ShowOpenDialog();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("The files chosen (Ctrl+A in the dialog for all) make up the filmstrip.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open folder...", ImVec2(-FLT_MIN, 0))) {
        ShowOpenFolderDialog();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Every DNG of a folder in the filmstrip.");
    }
    ImGui::BeginDisabled(filmstrip_.Empty());
    if (ImGui::Button("Export selected...", ImVec2(-FLT_MIN, 0))) {
        BeginExport();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("TIFF/JPEG with the Camera Raw edits applied, through the export server chosen in Settings.");
    }
    ImGui::Spacing();

    if (Loading()) {
        ImGui::TextWrapped("Loading %s...", Utf8(CurrentPath()->filename()).c_str());
        ImGui::Spacing();
    }
    if (!error_.empty()) {
        ErrorText(error_);
        ImGui::Spacing();
    }
    if (!doc_ && Loading()) {
        return;
    }
    if (!doc_) {
        ImGui::TextDisabled("Open a file, drop one on");
        ImGui::TextDisabled("the window, or press Ctrl+O.");
        return;
    }

    if (ImGui::BeginTable("info", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& [label, value] : doc_->Info()) {
            InfoRow(label, value);
        }
        InfoRow("Shown at", std::format("{:.0f} x {:.0f}", displayWidth_, displayHeight_));
        InfoRow("Load time", loadTime_);
        if (!renderTime_.empty()) {
            InfoRow("Last render", renderTime_);
        }
        InfoRow("Zoom", std::format("{:.0f}%", zoom_ * ImGui::GetIO().DisplayFramebufferScale.x * 100.0f));
        if (!filmstrip_.Empty()) {
            InfoRow("Filmstrip", std::format("{} of {} selected", filmstrip_.SelectedCount(), filmstrip_.Count()));
        }
        ImGui::EndTable();
    }
}

void ImageViewerFeature::DrawAdjustPanel() {
    ImGui::SeparatorText("Histogram");
    DrawHistogramGraph(doc_ ? histogram_ : HistogramData{});

    ImGui::Spacing();
    ImGui::SeparatorText("Adjustments");
    if (!doc_) {
        ImGui::TextDisabled("Open a DNG to edit it.");
        sliderActive_ = false;
        return;
    }

    ImGui::TextUnformatted("Exposure");
    ImGui::SameLine();
    const float resetWidth = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - resetWidth);
    ImGui::BeginDisabled(settings_.exposure == 0.0);
    if (ImGui::SmallButton("Reset")) {
        settings_.exposure = 0.0;
    }
    ImGui::EndDisabled();

    float exposure = static_cast<float>(settings_.exposure);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##exposure", &exposure, -kMaxExposure, kMaxExposure, "%+.2f EV")) {
        settings_.exposure = exposure;
    }
    sliderActive_ = ImGui::IsItemActive();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Ctrl+click to type a value");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Edits are not saved.");
    if (rendering_.valid()) {
        ImGui::TextDisabled("Rendering...");
    }
}

void ImageViewerFeature::DrawImageView() {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x <= 0 || size.y <= 0) {
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(24, 24, 26, 255));

    ImGui::InvisibleButton("##view", size, ImGuiButtonFlags_MouseButtonLeft);
    if (!doc_ && Loading()) {
        // The file's filmstrip thumbnail, fitted, until the full render is in.
        float aspect = 1.0f;
        if (SDL_Texture* thumb = filmstrip_.Thumbnail(*CurrentPath(), &aspect)) {
            const float w = std::min(size.x, size.y * aspect);
            const float h = w / aspect;
            const ImVec2 p0{origin.x + (size.x - w) * 0.5f, origin.y + (size.y - h) * 0.5f};
            draw->AddImage(TextureRef(thumb), p0, ImVec2(p0.x + w, p0.y + h));
        }
        return;
    }
    if (!doc_ || !texture_) {
        return;
    }

    const ImVec2 imageSize{displayWidth_, displayHeight_};
    const ImVec2 viewCenter{origin.x + size.x * 0.5f, origin.y + size.y * 0.5f};
    const ImGuiIO& io = ImGui::GetIO();

    if (fit_) {
        zoom_ = std::min(size.x / imageSize.x, size.y / imageSize.y);
        center_ = {imageSize.x * 0.5f, imageSize.y * 0.5f};
    }

    auto zoomAt = [&](float newZoom, ImVec2 screenPoint) {
        newZoom = std::clamp(newZoom, kMinZoom, kMaxZoom);
        // Keep the image point under screenPoint fixed.
        const ImVec2 offset{screenPoint.x - viewCenter.x, screenPoint.y - viewCenter.y};
        center_.x += offset.x / zoom_ - offset.x / newZoom;
        center_.y += offset.y / zoom_ - offset.y / newZoom;
        zoom_ = newZoom;
        fit_ = false;
    };

    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
        zoomAt(zoom_ * std::pow(1.2f, io.MouseWheel), io.MousePos);
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        center_.x -= io.MouseDelta.x / zoom_;
        center_.y -= io.MouseDelta.y / zoom_;
        fit_ = false;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const float actual = 1.0f / io.DisplayFramebufferScale.x;
        if (fit_) {
            zoomAt(actual, io.MousePos);
        } else {
            fit_ = true;
        }
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGui::IsItemActive() ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
    }

    const ImVec2 topLeft{viewCenter.x - center_.x * zoom_, viewCenter.y - center_.y * zoom_};
    const ImVec2 bottomRight{topLeft.x + imageSize.x * zoom_, topLeft.y + imageSize.y * zoom_};

    // Show individual pixels when zoomed past 100%, smooth sampling otherwise.
    const ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    const bool nearest = zoom_ * io.DisplayFramebufferScale.x > 1.0f;
    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    if (nearest) {
        draw->AddCallback(platform.DrawCallback_SetSamplerNearest, nullptr);
    }
    draw->AddImage(TextureRef(texture_), topLeft, bottomRight);
    if (nearest) {
        draw->AddCallback(platform.DrawCallback_SetSamplerLinear, nullptr);
    }
    draw->PopClipRect();
}

void ImageViewerFeature::Draw() {
    const float dpi = Dpi();
    // The three panes fill the window above the filmstrip.
    const float strip = filmstrip_.Height();
    ImGui::BeginChild("##info", ImVec2(kInfoPanelWidth * dpi, -strip), ImGuiChildFlags_AlwaysUseWindowPadding);
    DrawInfoPanel();
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    ImGui::BeginChild("##image", ImVec2(-kAdjustPanelWidth * dpi, -strip), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawImageView();
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    ImGui::BeginChild("##adjust", ImVec2(0, -strip), ImGuiChildFlags_AlwaysUseWindowPadding);
    DrawAdjustPanel();
    ImGui::EndChild();

    filmstrip_.Draw();
    DrawExportPopup();
}

// ---- Export -----------------------------------------------------------------------------------

// Opens the export dialog for the selected frames (the shown one if none is selected).
void ImageViewerFeature::BeginExport() {
    if (filmstrip_.Empty() || (export_.job && export_.job->Running())) {
        return;
    }
    export_.inputs = filmstrip_.SelectedPaths();
    if (export_.inputs.empty()) {
        if (const fs::path* active = filmstrip_.ActivePath()) {
            export_.inputs.push_back(*active);
        }
    }
    if (export_.inputs.empty()) {
        return;
    }
    export_.options = app_.GetSettings().exportDefaults;
    export_.destination[0] = '\0';
    export_.job.reset();
    export_.open = true;
    app_.ExportServer().CheckNow();
}

// Where each input goes: the chosen destination, or <its folder>/<subfolder>.
std::vector<ExportItem> ImageViewerFeature::ExportItems() const {
    const fs::path destination = PathFromUtf8(export_.destination);
    const std::string extension = "." + ExportFormatExtension(export_.options.format);
    std::vector<ExportItem> items;
    for (const fs::path& input : export_.inputs) {
        ExportItem item;
        item.input = input;
        const fs::path folder = destination.empty() ? input.parent_path() / app_.GetSettings().exportSubfolder : destination;
        item.output = folder / (input.stem().wstring() + fs::path(extension).wstring());
        items.push_back(std::move(item));
    }
    return items;
}

void ImageViewerFeature::DrawExportPopup() {
    if (export_.open) {
        ImGui::OpenPopup("Export");
        export_.open = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // A fixed width; the height follows the content (the form, then the progress).
    ImGui::SetNextWindowSizeConstraints(ImVec2(620 * Dpi(), 0), ImVec2(620 * Dpi(), FLT_MAX));
    if (!ImGui::BeginPopupModal("Export", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (export_.job) {
        DrawExportProgress();
    } else {
        DrawExportForm();
    }
    ImGui::EndPopup();
}

void ImageViewerFeature::DrawExportForm() {
    const float dpi = Dpi();
    const ImVec2 buttonSize(120 * dpi, 0);
    ServerManager& server = app_.ExportServer();
    const ServerHealth& health = server.Health();
    const size_t count = export_.inputs.size();

    if (!health.reachable) {
        // No server: point at Settings instead of a form that cannot work.
        ImGui::TextWrapped("No export server is running at %s%s%s.", server.Address().c_str(),
                           health.error.empty() ? "" : ": ", health.error.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped(server.Starting()
                               ? "It is starting; this dialog will offer the export once it answers."
                               : "Choose and start one in Settings (Photoshop or LightCraft export), then come back.");
        ImGui::Spacing();
        if (ImGui::Button("Open Settings", ImVec2(140 * dpi, 0))) {
            ImGui::CloseCurrentPopup();
            if (auto* settings = app_.Find<SettingsFeature>()) {
                settings->Show();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", buttonSize) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        return;
    }

    ExportOptions& o = export_.options;
    ImGui::Text("Export %zu file%s with the %s server", count, count == 1 ? "" : "s", health.tool.c_str());
    if (!health.version.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", health.version.c_str());
    }
    ImGui::Spacing();

    const float itemWidth = 200 * dpi;
    static constexpr const char* kFormats[] = {"tiff", "jpeg", "png"};
    static constexpr const char* kSpaces[] = {"sRGB", "AdobeRGB", "ProPhotoRGB"};
    auto indexOf = [](const char* const* values, size_t n, const std::string& v) {
        for (size_t i = 0; i < n; ++i) {
            if (v == values[i]) return static_cast<int>(i);
        }
        return 0;
    };
    ImGui::SetNextItemWidth(itemWidth);
    int format = indexOf(kFormats, 3, o.format);
    if (ImGui::Combo("Format", &format, "TIFF\0JPEG\0PNG\0")) {
        o.format = kFormats[format];
    }
    ImGui::BeginDisabled(o.format == "jpeg");
    ImGui::SetNextItemWidth(itemWidth);
    int depth = o.bitDepth == 8 ? 0 : 1;
    if (ImGui::Combo("Bit depth", &depth, "8 bits per channel\0" "16 bits per channel\0")) {
        o.bitDepth = depth == 0 ? 8 : 16;
    }
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(itemWidth);
    int space = indexOf(kSpaces, 3, o.colorSpace);
    if (ImGui::Combo("Color space", &space, "sRGB\0Adobe RGB\0ProPhoto RGB\0")) {
        o.colorSpace = kSpaces[space];
    }
    ImGui::BeginDisabled(o.format != "jpeg");
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt("JPEG quality", &o.quality, 1, 100);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(itemWidth);
    if (ImGui::InputInt("Long edge (0 = full size)", &o.longEdge, 0, 0)) {
        o.longEdge = std::clamp(o.longEdge, 0, 65535);
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Destination");
    ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2 +
                              ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##destination",
                             std::format("<each file's folder>\\{}", app_.GetSettings().exportSubfolder).c_str(),
                             export_.destination, sizeof(export_.destination));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        const fs::path start = export_.destination[0] ? PathFromUtf8(export_.destination) : export_.inputs.front().parent_path();
        app_.Dialogs().PickFolder(Utf8(start), [this](const fs::path& folder) {
            std::snprintf(export_.destination, sizeof(export_.destination), "%s", Utf8(folder).c_str());
        });
    }

    const std::vector<ExportItem> items = ExportItems();
    if (app_.GetSettings().confirmReplace) {
        std::error_code ec;
        const auto existing = std::ranges::count_if(items, [&](const ExportItem& i) { return fs::exists(i.output, ec); });
        if (existing > 0) {
            ImGui::TextColored(kWarningColor, "%lld of the %zu output files exist and will be replaced.",
                               static_cast<long long>(existing), items.size());
        }
    }
    ImGui::TextDisabled("First: %s", Utf8(items.front().output).c_str());

    ImGui::Spacing();
    if (ImGui::Button("Export", buttonSize) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        std::string host;
        uint16_t port = 0;
        ParseAddress(server.Address(), host, port);
        export_.job = std::make_unique<ExportJob>(host, port, items, o, 4);
        export_.job->Start();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", buttonSize) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
}

void ImageViewerFeature::DrawExportProgress() {
    const float dpi = Dpi();
    const ImVec2 buttonSize(120 * dpi, 0);
    ExportJob& job = *export_.job;
    const int completed = job.Completed();
    const int total = static_cast<int>(job.Count());
    const std::vector<ExportItem> items = job.Items();

    if (job.Running()) {
        ImGui::ProgressBar(static_cast<float>(completed) / static_cast<float>(std::max(1, total)), ImVec2(-FLT_MIN, 0),
                           std::format("{} / {}", completed, total).c_str());
        ImGui::TextDisabled("Exporting with the %s server...", app_.ExportServer().Health().tool.c_str());
    } else if (job.Cancelled()) {
        ImGui::Text("Cancelled after %d of %d file%s.", completed, total, total == 1 ? "" : "s");
    } else if (job.Failed() == 0) {
        ImGui::Text("Exported %d file%s.", total, total == 1 ? "" : "s");
    } else {
        ImGui::TextColored(kWarningColor, "Exported %d of %d file%s; %d failed.", total - job.Failed(), total,
                           total == 1 ? "" : "s", job.Failed());
    }
    // The failures, as they come.
    if (job.Failed() > 0) {
        ImGui::BeginChild("##failures", ImVec2(0, 120 * dpi), ImGuiChildFlags_Borders);
        for (const ExportItem& item : items) {
            if (item.done && !item.ok) {
                ImGui::TextColored(kErrorColor, "%s: %s", Utf8(item.input.filename()).c_str(), item.error.c_str());
            }
        }
        ImGui::EndChild();
    }
    ImGui::Spacing();
    if (job.Running()) {
        if (ImGui::Button("Cancel", buttonSize)) {
            job.Cancel();
        }
        return;
    }
    if (completed - job.Failed() > 0) {
        if (ImGui::Button("Show in folder", ImVec2(140 * dpi, 0))) {
            for (const ExportItem& item : items) {
                if (item.ok) {
                    SDL_OpenURL(FileUrl(item.output.parent_path()).c_str());
                    break;
                }
            }
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Close", buttonSize) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        export_.job.reset();
        ImGui::CloseCurrentPopup();
    }
}

R2R_REGISTER_FEATURE(ImageViewerFeature, 10);

}  // namespace r2r
