#include "features/settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <vector>

#include "app/app.h"
#include "app/ui.h"

namespace r2r {

namespace {

constexpr float kColumnWidth = 680.0f;
constexpr float kPagePadding = 24.0f;

constexpr const char* kToolLabels = "LightCraft export\0Photoshop export\0Custom export\0";
constexpr const char* kFormatLabels = "TIFF\0JPEG\0PNG\0";
constexpr const char* kFormats[] = {"tiff", "jpeg", "png"};
constexpr const char* kColorSpaceLabels = "sRGB\0Adobe RGB\0ProPhoto RGB\0";
constexpr const char* kColorSpaces[] = {"sRGB", "AdobeRGB", "ProPhotoRGB"};

int IndexOf(const char* const* values, size_t count, const std::string& value) {
    for (size_t i = 0; i < count; ++i) {
        if (value == values[i]) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

// The newest "Adobe Photoshop <year>" folder under Program Files, if any.
fs::path PhotoshopInstallFolder() {
    std::error_code ec;
    fs::path best;
    for (const char* root : {"C:\\Program Files\\Adobe", "C:\\Program Files (x86)\\Adobe"}) {
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (entry.is_directory(ec) && Utf8(entry.path().filename()).starts_with("Adobe Photoshop")) {
                if (best.empty() || entry.path().filename() > best.filename()) {
                    best = entry.path();
                }
            }
        }
    }
    return best;
}

}  // namespace

SettingsFeature::SettingsFeature(App& app)
    : Feature(app, {"Settings", "Choose and start the export server (Photoshop, LightCraft or your own), and set "
                                "the export defaults."}) {
    AddAction("Settings...", "Ctrl+,", ImGuiMod_Ctrl | ImGuiKey_Comma, [this] { Show(); });
}

void SettingsFeature::Show() {
    Feature* from = app_.Current();
    if (from != this) {
        app_.Activate(this, from);
    }
}

void SettingsFeature::SyncAddressText() {
    const Settings& s = app_.GetSettings();
    std::snprintf(addressText_, sizeof(addressText_), "%s", s.Address().c_str());
    std::snprintf(meterAddressText_, sizeof(meterAddressText_), "%s", s.MeterAddress().c_str());
    std::snprintf(subfolderText_, sizeof(subfolderText_), "%s", s.exportSubfolder.c_str());
    textsSynced_ = true;
}

// ---- Drawing ------------------------------------------------------------------------

void SettingsFeature::Draw() {
    if (!textsSynced_) {
        SyncAddressText();
    }
    const float dpi = Dpi();
    ImGui::BeginChild("##settings", ImVec2(0, 0), ImGuiChildFlags_None);
    const float padding = kPagePadding * dpi;
    const float available = ImGui::GetContentRegionAvail().x;
    const float width = std::max(100.0f, std::min(available - 2 * padding, kColumnWidth * dpi));
    ImGui::SetCursorPos(ImVec2(std::max(padding, (available - width) / 2), padding));
    ImGui::BeginChild("##column", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);

    DrawExportServer();
    ImGui::Spacing();
    DrawMeterServer();
    ImGui::Spacing();
    DrawExportDefaults();
    ImGui::Spacing();
    DrawGeneral();
    ImGui::EndChild();
    ImGui::EndChild();
}

void SettingsFeature::DrawExportServer() {
    const float dpi = Dpi();
    Settings& s = app_.GetSettings();
    ServerManager& server = app_.ExportServer();

    ImGui::SeparatorText("Export server");
    ImGui::TextWrapped("Exports (TIFF, JPEG, ...) with the Camera Raw edits applied are made by a server that drives "
                       "another application. Photoshop renders exactly what Lightroom shows; LightCraft is an open-source "
                       "approximation; a custom server is any program speaking the same protocol.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(220 * dpi);
    int tool = static_cast<int>(s.exportTool);
    if (ImGui::Combo("##tool", &tool, kToolLabels)) {
        s.exportTool = static_cast<ExportTool>(tool);
        exportLaunchError_.clear();
        app_.SaveSettings();
        SyncAddressText();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("at");
    ImGui::SameLine();
    const bool custom = s.exportTool == ExportTool::Custom;
    ImGui::BeginDisabled(!custom);
    ImGui::SetNextItemWidth(200 * dpi);
    if (ImGui::InputText("##address", addressText_, sizeof(addressText_), ImGuiInputTextFlags_EnterReturnsTrue) ||
        (custom && ImGui::IsItemDeactivatedAfterEdit())) {
        s.customAddress = addressText_;
        app_.SaveSettings();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(custom ? "host:port of your export server" : "The app's own export tools listen here.");
    }

    DrawServerStatus(server, exportLaunchError_, [this, &s] { LaunchServer(s.exportTool); },
                     s.exportTool == ExportTool::Photoshop
                         ? "Choose Photoshop.exe; the server starts Photoshop and keeps it open."
                         : "Choose lightcraft-cli.exe from LightCraft's portable build.");
    ImGui::Spacing();
    ImGui::TextDisabled("The servers are r2r-photoshop-export.exe and r2r-lightcraft-export.exe next to raw2raw2.exe.");
    ImGui::TextDisabled("Put lightcraft-cli.exe next to raw2raw2.exe and the LightCraft server starts with the app.");
}

// Status line and Launch / Stop / Check buttons for one server.
void SettingsFeature::DrawServerStatus(ServerManager& server, std::string& launchError,
                                       const std::function<void()>& launch, const char* launchTooltip) {
    const float dpi = Dpi();
    const ServerHealth& health = server.Health();
    const bool builtIn = server.BuiltIn();
    if (server.Starting()) {
        ImGui::TextColored(kWarningColor, "Starting the %s server...", server.ExpectedTool().c_str());
    } else if (health.reachable) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "Running: %s server%s%s%s", health.tool.c_str(),
                           health.version.empty() ? "" : " (", health.version.c_str(), health.version.empty() ? "" : ")");
        if (health.active || health.waiting) {
            ImGui::SameLine();
            ImGui::TextDisabled("%d working, %d waiting", health.active, health.waiting);
        }
        if (builtIn && health.tool != server.ExpectedTool()) {
            ImGui::TextColored(kWarningColor, "That is the %s server, not the %s one expected here.", health.tool.c_str(),
                               server.ExpectedTool().c_str());
        }
    } else {
        ImGui::TextDisabled("Not running%s%s", health.error.empty() ? "" : ": ", health.error.c_str());
    }
    if (!launchError.empty()) {
        ErrorText(launchError);
    } else if (!server.LaunchError().empty()) {
        ErrorText(server.LaunchError());
    }

    const ImVec2 buttonSize(150 * dpi, 0);
    if (builtIn && !health.reachable && !server.Starting()) {
        if (ImGui::Button(server.Role() == ServerRole::Export ? "Launch server..." : "Launch server", buttonSize)) {
            launch();
        }
        if (launchTooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s", launchTooltip);
        }
        ImGui::SameLine();
    }
    if (server.LaunchedRunning()) {
        if (ImGui::Button("Stop server", buttonSize)) {
            server.StopLaunched();
            launchError.clear();
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Check now", buttonSize)) {
        server.CheckNow();
    }
}

void SettingsFeature::DrawMeterServer() {
    const float dpi = Dpi();
    Settings& s = app_.GetSettings();
    ServerManager& server = app_.MeterServer();

    ImGui::SeparatorText("Exposure match server");
    ImGui::TextWrapped("Exposure Match asks a server how many stops brighter one frame is than another, one pair at "
                       "a time; everything else (sequencing, fitting, CSV and sidecars) happens in this app. The "
                       "built-in server is r2r-exposure-match.exe next to raw2raw2.exe and starts with the app; a "
                       "custom server is any program answering POST /compare the same way.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(220 * dpi);
    int mode = static_cast<int>(s.meterServer);
    if (ImGui::Combo("##meter", &mode, "Built-in server\0Custom server\0")) {
        s.meterServer = static_cast<MeterServer>(mode);
        meterLaunchError_.clear();
        app_.SaveSettings();
        SyncAddressText();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("at");
    ImGui::SameLine();
    const bool custom = s.meterServer == MeterServer::Custom;
    ImGui::BeginDisabled(!custom);
    ImGui::SetNextItemWidth(200 * dpi);
    if (ImGui::InputText("##meteraddress", meterAddressText_, sizeof(meterAddressText_), ImGuiInputTextFlags_EnterReturnsTrue) ||
        (custom && ImGui::IsItemDeactivatedAfterEdit())) {
        s.customMeterAddress = meterAddressText_;
        app_.SaveSettings();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * dpi);
    if (ImGui::InputInt("at a time", &s.meterParallel, 0, 0)) {
        s.meterParallel = std::clamp(s.meterParallel, 1, 16);
        app_.SaveSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Comparisons requested at once. The built-in server decodes a quarter of the cores' worth\n"
                          "of files in parallel; more requests than that only wait in its queue.");
    }

    DrawServerStatus(server, meterLaunchError_, [this] { meterLaunchError_ = app_.MeterServer().Launch({}); }, nullptr);
}

void SettingsFeature::LaunchServer(ExportTool tool) {
    static const SDL_DialogFileFilter photoshopFilters[] = {{"Photoshop.exe", "exe"}, {"All files", "*"}};
    static const SDL_DialogFileFilter lightcraftFilters[] = {{"lightcraft-cli.exe", "exe"}, {"All files", "*"}};
    const Settings& s = app_.GetSettings();
    std::error_code ec;
    std::string start;
    if (tool == ExportTool::Photoshop) {
        if (fs::is_regular_file(s.photoshopExe, ec)) {
            start = Utf8(s.photoshopExe.parent_path());
        } else if (const fs::path folder = PhotoshopInstallFolder(); !folder.empty()) {
            start = Utf8(folder);
        } else {
            start = "C:\\";
        }
    } else {
        start = fs::is_regular_file(s.lightcraftExe, ec) ? Utf8(s.lightcraftExe.parent_path()) : std::string("C:\\");
    }
    app_.Dialogs().PickFile(tool == ExportTool::Photoshop ? std::span<const SDL_DialogFileFilter>(photoshopFilters)
                                                : std::span<const SDL_DialogFileFilter>(lightcraftFilters),
                  start, [this](const fs::path& path) {
                      exportLaunchError_ = app_.ExportServer().Launch(path);
                      app_.RequestRedraw();
                  });
}

void SettingsFeature::DrawExportDefaults() {
    const float dpi = Dpi();
    Settings& s = app_.GetSettings();
    ExportOptions& d = s.exportDefaults;
    ImGui::SeparatorText("Export defaults");
    ImGui::TextDisabled("What the export dialog starts with; it can be changed for each export.");
    const float itemWidth = 200 * dpi;

    ImGui::SetNextItemWidth(itemWidth);
    int format = IndexOf(kFormats, std::size(kFormats), d.format);
    if (ImGui::Combo("Format", &format, kFormatLabels)) {
        d.format = kFormats[format];
        app_.SaveSettings();
    }
    ImGui::BeginDisabled(d.format == "jpeg");
    ImGui::SetNextItemWidth(itemWidth);
    int depth = d.bitDepth == 8 ? 0 : 1;
    if (ImGui::Combo("Bit depth", &depth, "8 bits per channel\0" "16 bits per channel\0")) {
        d.bitDepth = depth == 0 ? 8 : 16;
        app_.SaveSettings();
    }
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(itemWidth);
    int space = IndexOf(kColorSpaces, std::size(kColorSpaces), d.colorSpace);
    if (ImGui::Combo("Color space", &space, kColorSpaceLabels)) {
        d.colorSpace = kColorSpaces[space];
        app_.SaveSettings();
    }
    ImGui::BeginDisabled(d.format != "jpeg");
    ImGui::SetNextItemWidth(itemWidth);
    if (ImGui::SliderInt("JPEG quality", &d.quality, 1, 100)) {
        app_.SaveSettings();
    }
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(itemWidth);
    if (ImGui::InputInt("Long edge (pixels, 0 = full size)", &d.longEdge, 0, 0)) {
        d.longEdge = std::clamp(d.longEdge, 0, 65535);
        app_.SaveSettings();
    }
    ImGui::SetNextItemWidth(itemWidth);
    if (ImGui::InputText("Subfolder for exports", subfolderText_, sizeof(subfolderText_)) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
        s.exportSubfolder = subfolderText_;
        app_.SaveSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Exports go into this folder under each file's folder, unless a destination is chosen.");
    }
}

void SettingsFeature::DrawGeneral() {
    Settings& s = app_.GetSettings();
    ImGui::SeparatorText("General");
    if (ImGui::Checkbox("Warn before an export replaces existing files", &s.confirmReplace)) {
        app_.SaveSettings();
    }
    ImGui::TextDisabled("Settings file: %s", Utf8(Settings::Path()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Show")) {
        SDL_OpenURL(FileUrl(Settings::Path().parent_path()).c_str());
    }
}

R2R_REGISTER_FEATURE(SettingsFeature, 0);

}  // namespace r2r
