#include "app/gui.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>

#include "app/app.h"
#include "dng_xmp_sdk.h"
#include "io/xmp_settings.h"
#include "features/exposure_match.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

namespace r2r {

namespace {

// The SDL window and renderer with Dear ImGui on top, torn down in reverse order.
class MainWindow {
public:
    MainWindow() = default;
    ~MainWindow() {
        if (imgui_) {
            ImGui_ImplSDLRenderer3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
        }
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        if (sdl_) {
            SDL_Quit();
        }
    }
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Returns false, after telling the user, if the window could not be created.
    bool Create() {
        sdl_ = SDL_Init(SDL_INIT_VIDEO);
        if (!sdl_) {
            return Fail();
        }
        const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        window_ = SDL_CreateWindow("raw2raw2", static_cast<int>(1600 * scale), static_cast<int>(950 * scale),
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        renderer_ = window_ ? SDL_CreateRenderer(window_, nullptr) : nullptr;
        if (!renderer_) {
            return Fail();
        }
        SDL_SetRenderVSync(renderer_, 1);
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_ShowWindow(window_);
        SetUpImGui(scale);
        return true;
    }

    SDL_Window* Window() const { return window_; }
    SDL_Renderer* Renderer() const { return renderer_; }

    bool IsCloseRequest(const SDL_Event& event) const {
        return event.type == SDL_EVENT_QUIT ||
               (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window_));
    }

    bool Minimized() const { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0; }

    void Render(App& app) {
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.DrawFrame();
        ImGui::Render();

        const ImGuiIO& io = ImGui::GetIO();
        SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer_, 24, 24, 26, 255);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    }

private:
    bool Fail() {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "raw2raw2", SDL_GetError(), nullptr);
        return false;
    }

    void SetUpImGui(float scale) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        imgui_ = true;
        ImGui::GetIO().IniFilename = nullptr;

        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.FrameRounding = 3.0f;
        style.GrabRounding = 3.0f;
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;
        if (std::filesystem::exists("C:/Windows/Fonts/segoeui.ttf")) {
            ImGui::GetIO().Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 17.0f);
        }

        ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
        ImGui_ImplSDLRenderer3_Init(renderer_);
    }

    bool sdl_ = false;
    bool imgui_ = false;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
};

// Redraws only when something happens, plus a few frames after so ImGui can settle.
// Idle, the loop still wakes once a second so background checks (the export
// server's health) run and can ask for a redraw.
void RunEventLoop(MainWindow& window, App& app) {
    constexpr int kSettleFrames = 3;
    int framesToDraw = kSettleFrames;
    while (true) {
        const bool idle = framesToDraw <= 0 && !app.Busy();
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, idle ? 1000 : 0)) {
            do {
                framesToDraw = kSettleFrames;
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (window.IsCloseRequest(event)) {
                    return;
                }
                app.HandleEvent(event);
            } while (SDL_PollEvent(&event));
        }

        if (window.Minimized()) {
            SDL_Delay(10);
            continue;
        }

        app.Poll();
        if (app.TakeRedrawRequest()) {
            framesToDraw = kSettleFrames;
        }
        framesToDraw = app.Busy() ? kSettleFrames : framesToDraw - 1;
        if (framesToDraw < 0) {
            continue;  // nothing to draw: back to waiting
        }
        window.Render(app);
    }
}

}  // namespace

int RunGui(const CommandLine& cli) {
    MainWindow window;
    if (!window.Create()) {
        return 1;
    }

    InitializeXmpSdk();
    auto app = std::make_unique<App>(window.Window(), window.Renderer());
    if (auto* exposureMatch = app->Find<ExposureMatchFeature>()) {
        exposureMatch->SetLimit(cli.limit);
    }
    if (cli.startPath) {
        app->OpenPath(*cli.startPath);
    }

    RunEventLoop(window, *app);

    // Closed: hide the window at once, since stopping the servers we launched and
    // the work in flight can take a few seconds, then tear down in order (the
    // features use the DNG SDK and the renderer).
    SDL_HideWindow(window.Window());
    app->Shutdown();
    app.reset();
    dng_xmp_sdk::TerminateSDK();
    return 0;
}

}  // namespace r2r
