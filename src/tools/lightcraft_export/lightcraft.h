#pragma once

// Exports through LightCraft's command line (`lightcraft-cli render`), which
// develops a raw with the Camera Raw settings it finds in the file or its sidecar,
// mapped to LightCraft's own controls, and writes JPEG/PNG/TIFF/WebP/AVIF/JXL/PSD/DNG.
// Several renders run at once, up to the job limit.
//
// Request section "lightcraft":
//   set       {"light.exposure": 0.5, ...}   develop controls to set on top of the file's edits
//   settings  path of a partial develop-settings JSON or an .xmp/.lcpreset preset
//   preset    a preset id
//   metadata  "all" (default) | "allExceptCamera" | "copyright" | "none"
//   opt       {"key": value, ...}            any other export option of app.export
// Common fields used: bitDepth, colorSpace, quality, longEdge, ppi.

#include <semaphore>
#include <string>

#include "tools/common/export_service.h"

namespace r2r::tools {

class LightCraftBackend : public ExportBackend {
public:
    LightCraftBackend(fs::path cli, int jobs);

    // Checks that the program runs. Returns an error message, or empty.
    std::string Check();

    std::string Name() const override { return "lightcraft"; }
    Json Status() override;
    Json Export(const ExportRequest& request) override;

private:
    fs::path cli_;
    int jobs_;
    std::counting_semaphore<64> slots_;
    std::string version_;
};

}  // namespace r2r::tools
