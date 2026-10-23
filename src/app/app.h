#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/feature.h"
#include "app/file_dialogs.h"
#include "app/server_manager.h"
#include "app/settings.h"
#include "io/dng_loader.h"

namespace r2r {

// The application shell: owns the features and the servers it launches, shows the
// home screen, the menu bar and the navigation bar above each feature, routes input
// to the current feature and offers the services features share (file dialogs,
// textures, navigation).
class App {
public:
    App(SDL_Window* window, SDL_Renderer* renderer);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    SDL_Window* Window() const { return window_; }
    SDL_Renderer* Renderer() const { return renderer_; }
    uint32_t MaxTextureSize() const { return maxTextureSize_; }

    // ---- Main loop ------------------------------------------------------------

    // Handles an SDL event meant for the app (dropped files, dialog results).
    void HandleEvent(const SDL_Event& event);
    void Poll();
    bool Busy() const;
    // True once when something changed in the background that the screen should show.
    bool TakeRedrawRequest() { return std::exchange(redraw_, false); }
    void RequestRedraw() { redraw_ = true; }
    void DrawFrame();
    void RequestQuit();

    // Stops the servers the app launched, then the features. Call before the DNG
    // SDK and the renderer are torn down; the destructor calls it too.
    void Shutdown();

    // ---- Settings and services shared by the features -----------------------------

    Settings& GetSettings() { return settings_; }
    const Settings& GetSettings() const { return settings_; }
    void SaveSettings();
    ServerManager& ExportServer() { return exportServer_; }
    ServerManager& MeterServer() { return meterServer_; }
    FileDialogs& Dialogs() { return dialogs_; }

    // Copies a render into tex, recreating the texture if the size changed.
    std::expected<void, std::string> UploadTexture(SDL_Texture*& tex, const RenderedImage& image);

    // ---- Navigation -----------------------------------------------------------

    // Shows a feature in the window. returnTo is where GoBack() leads from there
    // (the home screen if null).
    void Activate(Feature* feature, Feature* returnTo = nullptr);
    void GoHome();
    void GoBack();
    Feature* Current() const { return current_; }
    Feature* ReturnTarget() const { return returnTo_; }

    const std::vector<std::unique_ptr<Feature>>& Features() const { return features_; }

    template <typename T>
    T* Find() const {
        for (const auto& feature : features_) {
            if (auto* match = dynamic_cast<T*>(feature.get())) {
                return match;
            }
        }
        return nullptr;
    }

    // Hands a path (dropped, chosen or given on the command line) to the feature
    // that can open it best, and shows that feature. Several paths go together to
    // the feature that scores best for any of them.
    void OpenPath(const fs::path& path);
    void OpenPaths(const std::vector<fs::path>& paths);

private:
    void StopServers();
    void HandleShortcuts();
    void DrawMenuBar();
    void DrawNavigationBar(const Feature& shown, const Feature* returnTo);
    void DrawHome();  // home_screen.cpp
    void UpdateWindowTitle();

    SDL_Window* window_;
    SDL_Renderer* renderer_;
    uint32_t maxTextureSize_ = 8192;

    Settings settings_;
    std::string settingsError_;
    ServerManager exportServer_;
    ServerManager meterServer_;
    FileDialogs dialogs_;
    bool redraw_ = false;

    std::vector<std::unique_ptr<Feature>> features_;
    Feature* current_ = nullptr;  // null = home screen
    Feature* returnTo_ = nullptr;

    // Files of one drop, which arrive one event at a time and are opened together.
    std::vector<fs::path> dropped_;

    std::string windowTitle_;
};

}  // namespace r2r
