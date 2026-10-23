#pragma once

#include <functional>
#include <string>

#include "app/server_manager.h"
#include "app/feature.h"
#include "app/settings.h"

namespace r2r {

// The app's settings: which export server to use (and starting it), the export
// defaults, and a few general switches. Always the first card.
class SettingsFeature : public Feature {
public:
    explicit SettingsFeature(App& app);

    void Draw() override;

    // Shows the settings, returning to the current feature on Esc.
    void Show();

private:
    void DrawExportServer();
    void DrawMeterServer();
    void DrawServerStatus(ServerManager& server, std::string& launchError, const std::function<void()>& launch,
                          const char* launchTooltip);
    void DrawExportDefaults();
    void DrawGeneral();
    void LaunchServer(ExportTool tool);
    void SyncAddressText();

    char addressText_[256] = "";
    char meterAddressText_[256] = "";
    char subfolderText_[256] = "";
    std::string exportLaunchError_;
    std::string meterLaunchError_;
    bool textsSynced_ = false;
};

}  // namespace r2r
