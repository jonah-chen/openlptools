#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app/gui.h"
#include "cli/cli.h"
#include "dng_xmp_sdk.h"
#include "io/xmp_settings.h"

// raw2raw2 opens its window unless the command line asks for a job to run in the
// console (see cli/cli.h).
int main(int argc, char* argv[]) {
    const r2r::CommandLine cli = r2r::ParseCommandLine(argc, argv);
    if (cli.video) {
        return r2r::RunVideoCommand(*cli.video);
    }
    if (cli.stack) {
        // The stack engine reads and writes XMP through the SDK.
        InitializeXmpSdk();
        const int code = r2r::RunStackCommand(*cli.stack);
        dng_xmp_sdk::TerminateSDK();
        return code;
    }
    return r2r::RunGui(cli);
}
