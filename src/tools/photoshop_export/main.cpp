// r2r-photoshop-export <path to Photoshop.exe> [--port 31000]
//
// Serves HTTP on 127.0.0.1 and exports raw files through Photoshop's Camera Raw,
// with the settings stored in each file, to TIFF/JPEG/PNG/PSD. See
// tools/common/export_service.h for the protocol and photoshop.h for the options.

#include <cstdio>

#include "tools/common/export_service.h"
#include "tools/photoshop_export/photoshop.h"

int wmain(int argc, wchar_t* argv[]) {
    using namespace r2r::tools;
    const ToolOptions options = ParseToolOptions(argc, argv);
    if (options.help || !options.error.empty()) {
        if (!options.error.empty()) {
            std::fprintf(stderr, "%s\n\n", options.error.c_str());
        }
        std::fprintf(stderr,
                     "Usage: r2r-photoshop-export <path to Photoshop.exe> [--port N]\n\n"
                     "Starts Photoshop if it is not running and serves export requests on http://127.0.0.1:N/\n"
                     "(default port 31000). POST /export with JSON {\"input\": ..., \"output\": ..., ...};\n"
                     "the answer comes when the export is done. Requests queue: Photoshop runs one at a time.\n");
        return options.help ? 0 : 2;
    }
    PhotoshopBackend backend(options.program);
    if (const std::string error = backend.Connect(); !error.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("Connected to %s\n", backend.Status()["photoshop"].get<std::string>().c_str());
    return RunExportService(backend, options.port);
}
