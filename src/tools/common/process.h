#pragma once

// Running other programs from the tools.

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace r2r::tools {

struct ProcessResult {
    int exitCode = -1;
    std::string output;      // stdout and stderr, interleaved as they came
    bool timedOut = false;
    std::string error;       // could not start, etc.; empty when the process ran
};

// Runs exe with args, capturing its output, and waits for it to end (or kills it
// after timeout). The arguments are quoted for the Windows command line.
ProcessResult RunProcess(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                         std::chrono::milliseconds timeout);

// Starts exe with args without waiting, outside any job we run in, so it keeps
// running when we (and the app that started us) exit. Returns an error message, or empty.
std::string StartDetached(const std::filesystem::path& exe, const std::vector<std::wstring>& args);

std::wstring QuoteArgument(const std::wstring& arg);

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);

}  // namespace r2r::tools
