#include "app/app.h"

#include <algorithm>
#include <format>
#include <thread>

#include "app/ui.h"
#include "imgui.h"

namespace r2r {

namespace {

constexpr float kNavBarPadding = 6.0f;
constexpr ImU32 kNavBarBackground = IM_COL32(30, 31, 36, 255);
constexpr ImU32 kNavBarBorder = IM_COL32(52, 54, 62, 255);

}  // namespace

App::App(SDL_Window* window, SDL_Renderer* renderer)
    : window_(window), renderer_(renderer), exportServer_(settings_, ServerRole::Export),
      meterServer_(settings_, ServerRole::Meter), dialogs_(window) {
    maxTextureSize_ = static_cast<uint32_t>(
        SDL_GetNumberProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 8192));
    if (auto loaded = settings_.Load(); !loaded) {
        settingsError_ = loaded.error();
    }
    features_ = CreateRegisteredFeatures(*this);
    exportServer_.AutoStart();
    meterServer_.AutoStart();
}

App::~App() {
    Shutdown();
}

void App::SaveSettings() {
    if (auto saved = settings_.Save(); !saved) {
        settingsError_ = saved.error();
    }
    exportServer_.CheckNow();
    meterServer_.CheckNow();
}

// ---- Main loop ----------------------------------------------------------------

void App::HandleEvent(const SDL_Event& event) {
    if (dialogs_.HandleEvent(event)) {
        return;
    }
    if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
        // One event per file; SDL_EVENT_DROP_COMPLETE follows the last.
        dropped_.push_back(PathFromUtf8(event.drop.data));
    } else if (event.type == SDL_EVENT_DROP_COMPLETE) {
        const std::vector<fs::path> paths = std::move(dropped_);
        dropped_.clear();
        OpenPaths(paths);
    }
}

void App::Poll() {
    for (const auto& feature : features_) {
        feature->Poll();
    }
    exportServer_.Poll();
    meterServer_.Poll();
    if (exportServer_.TakeChanged() || meterServer_.TakeChanged()) {
        redraw_ = true;
    }
    UpdateWindowTitle();
}

bool App::Busy() const {
    return exportServer_.Busy() || meterServer_.Busy() ||
           std::ranges::any_of(features_, [](const auto& feature) { return feature->Busy(); });
}

void App::RequestQuit() {
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

// The servers go first: the features' requests in flight then fail fast instead of
// keeping the features (and the app) waiting for answers.
void App::Shutdown() {
    StopServers();
    dialogs_.Clear();
    current_ = nullptr;
    returnTo_ = nullptr;
    for (const auto& feature : features_) {
        feature->Shutdown();
    }
    features_.clear();
}

// Both at once, as each may wait a few seconds for its work in flight.
void App::StopServers() {
    std::jthread meter([this] { meterServer_.StopLaunched(); });
    exportServer_.StopLaunched();
}

void App::UpdateWindowTitle() {
    const std::string title =
        current_ ? std::format("{} - raw2raw2", current_->WindowTitle()) : std::string("raw2raw2");
    if (title != windowTitle_) {
        windowTitle_ = title;
        SDL_SetWindowTitle(window_, windowTitle_.c_str());
    }
}

void App::DrawFrame() {
    HandleShortcuts();
    DrawMenuBar();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);

    // Navigating from the bar takes effect next frame; this one finishes as it began.
    if (Feature* shown = current_) {
        DrawNavigationBar(*shown, returnTo_);
        shown->Draw();
    } else {
        DrawHome();
    }
    ImGui::End();
}

// ---- Navigation -----------------------------------------------------------------

void App::Activate(Feature* feature, Feature* returnTo) {
    current_ = feature;
    returnTo_ = feature ? returnTo : nullptr;
}

void App::GoHome() {
    Activate(nullptr);
}

void App::GoBack() {
    Activate(returnTo_);
}

void App::OpenPath(const fs::path& path) {
    Feature* best = nullptr;
    int bestScore = 0;
    for (const auto& feature : features_) {
        const int score = feature->OpenScore(path);
        if (score > bestScore) {
            best = feature.get();
            bestScore = score;
        }
    }
    if (best) {
        Activate(best);
        best->Open(path);
    }
}

void App::OpenPaths(const std::vector<fs::path>& paths) {
    if (paths.size() == 1) {
        OpenPath(paths.front());
        return;
    }
    Feature* best = nullptr;
    int bestScore = 0;
    for (const auto& feature : features_) {
        for (const fs::path& path : paths) {
            const int score = feature->OpenScore(path);
            if (score > bestScore) {
                best = feature.get();
                bestScore = score;
            }
        }
    }
    if (!best) {
        return;
    }
    std::vector<fs::path> accepted;
    for (const fs::path& path : paths) {
        if (best->OpenScore(path) > 0) {
            accepted.push_back(path);
        }
    }
    Activate(best);
    best->OpenFiles(accepted);
}

// ---- Services ---------------------------------------------------------------------

std::expected<void, std::string> App::UploadTexture(SDL_Texture*& tex, const RenderedImage& image) {
    float w = 0, h = 0;
    if (!tex || !SDL_GetTextureSize(tex, &w, &h) || w != static_cast<float>(image.width) ||
        h != static_cast<float>(image.height)) {
        SDL_Texture* created = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                                 static_cast<int>(image.width), static_cast<int>(image.height));
        if (!created) {
            return std::unexpected(std::format("Could not create a texture: {}", SDL_GetError()));
        }
        SDL_DestroyTexture(tex);
        tex = created;
    }
    if (!SDL_UpdateTexture(tex, nullptr, image.rgba.data(), static_cast<int>(image.width) * 4)) {
        return std::unexpected(std::format("Could not update the texture: {}", SDL_GetError()));
    }
    return {};
}

// ---- Shortcuts, menus and the navigation bar ------------------------------------------

void App::HandleShortcuts() {
    if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }
    for (const auto& feature : features_) {
        for (const FeatureAction& action : feature->Actions()) {
            if (action.shortcut != 0 && ImGui::IsKeyChordPressed(action.shortcut)) {
                action.run();
                return;  // the action may have changed the current feature
            }
        }
    }
    if (!current_) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        GoBack();
        return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_H)) {
        GoHome();
        return;
    }
    current_->HandleShortcuts();
}

void App::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        bool any = false;
        for (const auto& feature : features_) {
            for (const FeatureAction& action : feature->Actions()) {
                const char* shortcut = action.shortcutText.empty() ? nullptr : action.shortcutText.c_str();
                if (ImGui::MenuItem(action.label.c_str(), shortcut)) {
                    action.run();
                }
                any = true;
            }
        }
        if (any) {
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Back", "Esc", false, current_ != nullptr)) {
            GoBack();
        }
        if (ImGui::MenuItem("Home", "Ctrl+H", false, current_ != nullptr)) {
            GoHome();
        }
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
            RequestQuit();
        }
        ImGui::EndMenu();
    }
    if (current_) {
        current_->DrawMenus();
    }
    ImGui::EndMainMenuBar();
}

// A strip above the current feature: a back button (to the feature it was opened
// from, or the home screen), a home button when those differ, and the feature's name.
// Drawn from the state the frame began with: the buttons change current_ and returnTo_.
void App::DrawNavigationBar(const Feature& shown, const Feature* returnTo) {
    const float dpi = Dpi();
    const float pad = kNavBarPadding * dpi;
    const float height = ImGui::GetFrameHeight() + 2 * pad;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kNavBarBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2 * pad, pad));
    ImGui::BeginChild("##navbar", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const std::string back = returnTo ? std::format("Back to {}", returnTo->Info().name) : std::string("Home");
    if (BackButton(back.c_str())) {
        GoBack();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Esc");
    }
    if (returnTo) {
        ImGui::SameLine();
        if (ImGui::Button("Home")) {
            GoHome();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("Ctrl+H");
        }
    }
    ImGui::SameLine(0, 3 * pad);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", shown.Info().name.c_str());

    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x, p0.y + size.y - 1), ImVec2(p0.x + size.x, p0.y + size.y - 1),
                                        kNavBarBorder);
    ImGui::EndChild();
}

}  // namespace r2r
