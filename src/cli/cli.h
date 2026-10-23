#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "processing/raw_stack.h"
#include "processing/video_export.h"

namespace r2r {

// Options of the --video mode: render a folder of TIFFs without opening a window.
struct VideoCommand {
    std::filesystem::path folder;
    VideoOptions options;
    std::string error;  // a bad option
};

// Options of the --stack mode: combine DNGs into one without opening a window.
struct StackCommand {
    StackRequest request;
    std::string error;  // a bad option
};

struct CommandLine {
    std::optional<std::filesystem::path> startPath;  // a file or folder to open at startup
    std::optional<VideoCommand> video;
    std::optional<StackCommand> stack;
    int limit = 0;  // --limit: analyze only the first N files of a sequence
};

// raw2raw2 [path] [--limit N]: path is a DNG or a folder of DNGs (a sequence);
// --limit analyzes only the first N files of a sequence.
// raw2raw2 --video <folder> [--out <file.mp4>] [--width W] [--fit crop|letterbox|photo]
//          [--fps N] [--mbps M] [--codec h264|hevc]: renders the folder's TIFFs to a
// video without opening a window.
// raw2raw2 --stack mean|median [--out <file.dng>] [--meta <frame.dng>] <frame.dng>...:
// combines the frames into one DNG; --meta names the frame whose metadata the
// result carries (the first frame if omitted).
CommandLine ParseCommandLine(int argc, char* argv[]);

// Renders a folder of TIFFs to a video, printing the progress to the console the
// program was started from. Returns the process exit code.
int RunVideoCommand(const VideoCommand& command);

// Combines DNGs into one, printing the progress and the result's settings to the
// console. Returns the process exit code.
int RunStackCommand(const StackCommand& command);

}  // namespace r2r
