#pragma once

// User settings, kept in %APPDATA%\raw2raw2\settings.json.

#include <cstdint>
#include <expected>
#include <string>

#include "common/util.h"
#include "net/export_client.h"

namespace r2r {

// Which export service the app talks to.
enum class ExportTool { LightCraft, Photoshop, Custom };

const char* ExportToolName(ExportTool tool);  // "lightcraft", "photoshop", "custom"
const char* ExportToolLabel(ExportTool tool); // "LightCraft export", ...

// Which exposure-match (metering) server the app asks.
enum class MeterServer { BuiltIn, Custom };

// The addresses the app's own tools listen on.
inline constexpr const char* kDefaultExportAddress = "127.0.0.1:31000";
inline constexpr const char* kDefaultMeterAddress = "127.0.0.1:31001";

struct Settings {
    ExportTool exportTool = ExportTool::LightCraft;
    std::string customAddress = kDefaultExportAddress;  // used with ExportTool::Custom
    MeterServer meterServer = MeterServer::BuiltIn;
    std::string customMeterAddress = kDefaultMeterAddress;  // used with MeterServer::Custom
    int meterParallel = 4;                                  // comparisons requested at once
    fs::path photoshopExe;   // the applications last chosen for the tools
    fs::path lightcraftExe;
    ExportOptions exportDefaults;
    std::string exportSubfolder = "export";  // under each file's folder, when no destination is chosen
    bool confirmReplace = true;              // warn before exports replace existing files

    // "host:port" for the chosen tool: fixed for the app's own tools, free for Custom.
    std::string Address() const;
    std::string MeterAddress() const;

    static fs::path Path();
    std::expected<void, std::string> Load();
    std::expected<void, std::string> Save() const;
};

}  // namespace r2r
