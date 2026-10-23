#include "app/settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "json.hpp"

namespace r2r {

namespace {

using Json = nlohmann::json;

ExportTool ToolFromName(const std::string& name) {
    if (name == "photoshop") return ExportTool::Photoshop;
    if (name == "custom") return ExportTool::Custom;
    return ExportTool::LightCraft;
}

}  // namespace

const char* ExportToolName(ExportTool tool) {
    switch (tool) {
        case ExportTool::Photoshop: return "photoshop";
        case ExportTool::Custom: return "custom";
        default: return "lightcraft";
    }
}

const char* ExportToolLabel(ExportTool tool) {
    switch (tool) {
        case ExportTool::Photoshop: return "Photoshop export";
        case ExportTool::Custom: return "Custom export";
        default: return "LightCraft export";
    }
}

std::string Settings::Address() const {
    return exportTool == ExportTool::Custom ? customAddress : std::string(kDefaultExportAddress);
}

std::string Settings::MeterAddress() const {
    return meterServer == MeterServer::Custom ? customMeterAddress : std::string(kDefaultMeterAddress);
}

fs::path Settings::Path() {
    wchar_t* appData = nullptr;
    size_t length = 0;
    fs::path base;
    if (_wdupenv_s(&appData, &length, L"APPDATA") == 0 && appData) {
        base = appData;
        free(appData);
    } else {
        base = fs::temp_directory_path();
    }
    return base / "raw2raw2" / "settings.json";
}

std::expected<void, std::string> Settings::Load() {
    std::ifstream file(Path(), std::ios::binary);
    if (!file) {
        return {};  // first run: defaults
    }
    std::stringstream text;
    text << file.rdbuf();
    Json j = Json::parse(text.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return std::unexpected("settings.json is not valid JSON; using defaults");
    }
    exportTool = ToolFromName(j.value("exportTool", "lightcraft"));
    customAddress = j.value("customAddress", std::string(kDefaultExportAddress));
    meterServer = j.value("meterServer", "builtIn") == "custom" ? MeterServer::Custom : MeterServer::BuiltIn;
    customMeterAddress = j.value("customMeterAddress", std::string(kDefaultMeterAddress));
    meterParallel = std::clamp(j.value("meterParallel", meterParallel), 1, 16);
    photoshopExe = PathFromUtf8(j.value("photoshopExe", "").c_str());
    lightcraftExe = PathFromUtf8(j.value("lightcraftExe", "").c_str());
    if (const auto it = j.find("exportDefaults"); it != j.end() && it->is_object()) {
        exportDefaults.format = it->value("format", exportDefaults.format);
        exportDefaults.bitDepth = it->value("bitDepth", exportDefaults.bitDepth);
        exportDefaults.colorSpace = it->value("colorSpace", exportDefaults.colorSpace);
        exportDefaults.quality = it->value("quality", exportDefaults.quality);
        exportDefaults.longEdge = it->value("longEdge", exportDefaults.longEdge);
    }
    exportSubfolder = j.value("exportSubfolder", exportSubfolder);
    confirmReplace = j.value("confirmReplace", confirmReplace);
    return {};
}

std::expected<void, std::string> Settings::Save() const {
    Json j = {
        {"exportTool", ExportToolName(exportTool)},
        {"customAddress", customAddress},
        {"meterServer", meterServer == MeterServer::Custom ? "custom" : "builtIn"},
        {"customMeterAddress", customMeterAddress},
        {"meterParallel", meterParallel},
        {"photoshopExe", Utf8(photoshopExe)},
        {"lightcraftExe", Utf8(lightcraftExe)},
        {"exportDefaults",
         {{"format", exportDefaults.format},
          {"bitDepth", exportDefaults.bitDepth},
          {"colorSpace", exportDefaults.colorSpace},
          {"quality", exportDefaults.quality},
          {"longEdge", exportDefaults.longEdge}}},
        {"exportSubfolder", exportSubfolder},
        {"confirmReplace", confirmReplace},
    };
    std::error_code ec;
    fs::create_directories(Path().parent_path(), ec);
    std::ofstream file(Path(), std::ios::binary | std::ios::trunc);
    if (!file) {
        return std::unexpected("cannot write " + Utf8(Path()));
    }
    file << j.dump(2) << '\n';
    return {};
}

}  // namespace r2r
