#include "cli/cli.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <thread>
#include <vector>

#include "common/util.h"
#include "io/dng_loader.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace r2r {

namespace {

#ifdef _WIN32
// Lets a GUI program print to the console it was started from, if any. A stream
// the parent redirected to a file or pipe is left as it is.
bool AttachParentConsole() {
    const bool outRedirected = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN;
    const bool errRedirected = GetFileType(GetStdHandle(STD_ERROR_HANDLE)) != FILE_TYPE_UNKNOWN;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return outRedirected;
    }
    FILE* f = nullptr;
    if (!outRedirected) {
        freopen_s(&f, "CONOUT$", "w", stdout);
    }
    if (!errRedirected) {
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    return true;
}
#endif

}  // namespace

CommandLine ParseCommandLine(int argc, char* argv[]) {
    CommandLine cli;
    std::optional<VideoCommand>& video = cli.video;
    std::optional<StackCommand>& stack = cli.stack;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--limit" && hasValue) {
            cli.limit = std::max(0, std::atoi(argv[++i]));
        } else if (arg == "--stack" && hasValue) {
            stack = StackCommand{};
            const std::string mode = argv[++i];
            if (mode == "mean") {
                stack->request.mode = StackMode::Mean;
            } else if (mode == "median") {
                stack->request.mode = StackMode::Median;
            } else {
                stack->error = "--stack takes mean or median";
            }
        } else if (stack && arg == "--out" && hasValue) {
            stack->request.output = PathFromUtf8(argv[++i]);
        } else if (stack && arg == "--meta" && hasValue) {
            stack->request.metadataSource = PathFromUtf8(argv[++i]);
        } else if (stack && !arg.starts_with("--")) {
            stack->request.frames.push_back(PathFromUtf8(argv[i]));
        } else if (stack) {
            stack->error = "unknown option " + arg;
        } else if (arg == "--video" && hasValue) {
            video = VideoCommand{};
            video->folder = PathFromUtf8(argv[++i]);
            video->options.megabitsPerSecond = 0.0;  // derived from the size unless given
        } else if (video && arg == "--out" && hasValue) {
            video->options.output = PathFromUtf8(argv[++i]);
        } else if (video && arg == "--width" && hasValue) {
            video->options.width = static_cast<uint32_t>(std::clamp(std::atoi(argv[++i]), 16, 16384));
        } else if (video && arg == "--fps" && hasValue) {
            video->options.fps = static_cast<uint32_t>(std::clamp(std::atoi(argv[++i]), 1, 240));
        } else if (video && arg == "--mbps" && hasValue) {
            video->options.megabitsPerSecond = std::clamp(std::atof(argv[++i]), 0.1, 1000.0);
        } else if (video && arg == "--fit" && hasValue) {
            const std::string fit = argv[++i];
            if (fit == "crop") {
                video->options.fit = FrameFit::Crop;
            } else if (fit == "letterbox") {
                video->options.fit = FrameFit::Letterbox;
            } else if (fit == "photo") {
                video->options.fit = FrameFit::Photo;
            } else {
                video->error = "--fit takes crop, letterbox or photo";
            }
        } else if (video && arg == "--codec" && hasValue) {
            const std::string codec = argv[++i];
            if (codec == "h264") {
                video->options.codec = VideoCodec::H264;
            } else if (codec == "hevc" || codec == "h265") {
                video->options.codec = VideoCodec::Hevc;
            } else {
                video->error = "--codec takes h264 or hevc";
            }
        } else if (!arg.starts_with("--")) {
            cli.startPath = PathFromUtf8(argv[i]);
        } else if (video) {
            video->error = "unknown option " + arg;
        }
    }
    return cli;
}

int RunVideoCommand(const VideoCommand& command) {
#ifdef _WIN32
    AttachParentConsole();
#endif
    if (!command.error.empty()) {
        std::fprintf(stderr, "%s\n", command.error.c_str());
        return 2;
    }
    const std::vector<fs::path> files = ListTiffs(command.folder);
    if (files.empty()) {
        std::fprintf(stderr, "No TIFF files in %s\n", Utf8(command.folder).c_str());
        return 1;
    }
    auto first = ProbeTiff(files.front());
    if (!first) {
        std::fprintf(stderr, "Cannot read %s\n", first.error().c_str());
        return 1;
    }
    VideoOptions options = command.options;
    if (options.output.empty()) {
        std::string name = Utf8(command.folder.filename());
        options.output = command.folder / ((name.empty() ? "video" : name) + ".mp4");
    } else if (!options.output.has_extension()) {
        options.output += ".mp4";
    }
    const auto [width, height] = VideoFrameSize(options, first->width, first->height);
    if (options.megabitsPerSecond <= 0.0) {
        options.megabitsPerSecond = std::round(DefaultMegabits(width, height, options.fps));
    }
    std::printf("Rendering %zu TIFF files (%u x %u, %u-bit, %s, profile %s) to %s\n%u x %u, %u fps, %s, %.0f Mbit/s\n",
                files.size(), first->width, first->height, first->bitsPerSample, first->compressionName.c_str(),
                first->colorProfile.c_str(), Utf8(options.output).c_str(), width, height, options.fps,
                VideoCodecName(options.codec), options.megabitsPerSecond);
    std::fflush(stdout);

    VideoExportJob job(files, options, width, height);
    job.Start();
    int reported = -1;
    while (!job.Finished()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int completed = job.Completed();
        if (completed != reported) {
            reported = completed;
            std::printf("%d / %zu\n", completed, files.size());
            std::fflush(stdout);
        }
    }
    std::printf("%s\n", job.Status().c_str());
    if (!job.Failed()) {
        std::printf("%s, %s, %s, %.1f s\n", Utf8(options.output).c_str(), ByteText(job.OutputBytes()).c_str(),
                    job.Encoder().c_str(), job.Seconds());
    }
    std::fflush(stdout);
    return job.Failed() ? 1 : 0;
}

int RunStackCommand(const StackCommand& command) {
#ifdef _WIN32
    AttachParentConsole();
#endif
    if (!command.error.empty()) {
        std::fprintf(stderr, "%s\n", command.error.c_str());
        return 2;
    }
    StackRequest request = command.request;
    if (request.frames.size() < 2) {
        std::fprintf(stderr, "--stack needs at least two DNG files\n");
        return 2;
    }
    if (request.metadataSource.empty()) {
        request.metadataSource = request.frames.front();
    }
    if (request.output.empty()) {
        const std::string name = std::format("{}_{}_x{}.dng", Utf8(request.metadataSource.stem()),
                                             StackModeName(request.mode), request.frames.size());
        request.output = request.metadataSource.parent_path() / fs::path(reinterpret_cast<const char8_t*>(name.c_str()));
    } else if (!request.output.has_extension()) {
        request.output += ".dng";
    }
    std::printf("Combining %zu frames (%s) into %s\nMetadata from %s\n", request.frames.size(),
                StackModeName(request.mode), Utf8(request.output).c_str(), Utf8(request.metadataSource.filename()).c_str());
    std::fflush(stdout);

    StackJob job(request);
    job.Start();
    std::string reportedStatus;
    int reportedCompleted = -1;
    while (!job.Finished()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string status = job.Status();
        const int completed = job.Completed();
        if (status != reportedStatus) {
            reportedStatus = status;
            reportedCompleted = -1;
            std::printf("%s\n", status.c_str());
        }
        if (job.Total() > 0 && completed != reportedCompleted) {
            reportedCompleted = completed;
            std::printf("  %d / %d\n", completed, job.Total());
        }
        std::fflush(stdout);
    }
    if (job.Status() != reportedStatus) {
        std::printf("%s\n", job.Status().c_str());
    }
    if (!job.Failed() && !job.Cancelled()) {
        std::printf("%s, %.1f s\n", ByteText(job.OutputBytes()).c_str(), job.Seconds());
        if (auto probe = DngDocument::Probe(request.output)) {
            std::printf("Result: %s %s, %u x %u, ISO %u, %s, %s, taken %s\n", probe->make.c_str(), probe->model.c_str(),
                        probe->rawWidth, probe->rawHeight, probe->iso, ShutterText(probe->exposureTime).c_str(),
                        ApertureText(probe->fNumber).c_str(), probe->timeText.c_str());
        } else {
            std::printf("The result cannot be read back: %s\n", probe.error().c_str());
        }
    }
    std::fflush(stdout);
    return job.Failed() ? 1 : 0;
}

}  // namespace r2r
