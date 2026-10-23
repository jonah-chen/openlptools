#pragma once

// What the export tools share: the request they accept, the HTTP service that
// receives it, and the command line that starts them.
//
//   POST /export      body: an export request (below). Answers when the export is
//                     done: {"ok":true,"id":..,"output":..,"width":..,"height":..,
//                     "seconds":..} or {"ok":false,"id":..,"error":"..."}.
//   GET  /health      {"ok":true,"tool":..,"active":n,"queued":n, ...backend status}
//   POST /shutdown    stops the service once the exports in flight have answered.
//
// Request fields common to the tools (a backend documents its own, which live in
// an object named after it, e.g. "photoshop": {...}):
//   input      path of the raw/DNG                                      required
//   output     path of the file to write; its extension names the format required
//   format     "tiff" | "jpeg" | "png" | ... overrides the extension's format
//   bitDepth   8 | 16
//   colorSpace "sRGB" | "AdobeRGB" | "ProPhotoRGB" | ...
//   quality    JPEG quality 1..100
//   longEdge   resize so the long edge is this many pixels
//   ppi        resolution written into the file
//   id         anything; echoed in the answer

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "tools/common/json.h"

namespace r2r::tools {

namespace fs = std::filesystem;

struct ExportRequest {
    Json raw;  // the whole request, for backend-specific fields
    Json id;
    fs::path input;
    fs::path output;
    std::string format;  // lower case, without a dot: "tiff", "jpeg", "png", "psd", "dng", ...
    int bitDepth = 0;    // 0 = the backend's default
    std::string colorSpace;
    int quality = 0;
    int longEdge = 0;
    int ppi = 0;

    // The backend's own options, e.g. request.Section("photoshop")["size"].
    Json Section(const char* name) const;
};

std::expected<ExportRequest, std::string> ParseExportRequest(const std::string& body);

// What a tool knows how to do. Export runs on the connection's thread; a backend
// that cannot work in parallel serializes inside.
class ExportBackend {
public:
    virtual ~ExportBackend() = default;
    virtual std::string Name() const = 0;
    virtual Json Status() = 0;
    // Returns {"width":..,"height":..} and anything else worth reporting, or throws
    // std::runtime_error with the reason.
    virtual Json Export(const ExportRequest& request) = 0;
};

struct ToolOptions {
    fs::path program;  // the application the tool drives
    uint16_t port = 31000;
    int jobs = 0;      // parallel exports; 0 = the backend's default
    std::string error;  // bad command line
    bool help = false;
};

// <program> [--port N] [--jobs N]; the program may be left out when requireProgram is false.
ToolOptions ParseToolOptions(int argc, wchar_t* argv[], bool requireProgram = true);

// Serves the backend until /shutdown or Ctrl+C. Returns the process exit code.
int RunExportService(ExportBackend& backend, uint16_t port);

}  // namespace r2r::tools
