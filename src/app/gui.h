#pragma once

#include "cli/cli.h"

namespace r2r {

// Opens the window and runs the app until it is closed. Returns the process exit code.
int RunGui(const CommandLine& cli);

}  // namespace r2r
