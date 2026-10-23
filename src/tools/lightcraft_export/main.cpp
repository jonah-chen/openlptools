// r2r-lightcraft-export <path to lightcraft-cli.exe> [--port 31000] [--jobs N]
//
// Serves HTTP on 127.0.0.1 and exports raw files through LightCraft's renderer,
// which applies the Camera Raw settings stored in the file as far as it maps them.
// See tools/common/export_service.h for the protocol and lightcraft.h for the options.

#include <algorithm>
#include <cstdio>
#include <thread>

#include "tools/common/export_service.h"
#include "tools/lightcraft_export/lightcraft.h"

int wmain(int argc, wchar_t* argv[]) {
    using namespace r2r::tools;
    const ToolOptions options = ParseToolOptions(argc, argv);
    if (options.help || !options.error.empty()) {
        if (!options.error.empty()) {
            std::fprintf(stderr, "%s\n\n", options.error.c_str());
        }
        std::fprintf(stderr,
                     "Usage: r2r-lightcraft-export <path to lightcraft-cli.exe> [--port N] [--jobs N]\n\n"
                     "Serves export requests on http://127.0.0.1:N/ (default port 31000), running up to\n"
                     "--jobs renders at once (default: half the cores). POST /export with JSON\n"
                     "{\"input\": ..., \"output\": ..., ...}; the answer comes when the export is done.\n");
        return options.help ? 0 : 2;
    }
    const int jobs = options.jobs > 0 ? options.jobs : std::max(1u, std::thread::hardware_concurrency() / 2);
    LightCraftBackend backend(options.program, jobs);
    if (const std::string error = backend.Check(); !error.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("Using %s, %d render(s) at a time\n", backend.Status()["lightcraft"].get<std::string>().c_str(), jobs);
    return RunExportService(backend, options.port);
}
