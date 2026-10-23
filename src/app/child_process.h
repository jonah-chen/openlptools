#pragma once

// A process the app starts and owns, such as one of its export or metering servers.
// It runs in a job object that only the app holds, so when the app goes away (even
// by crashing or being killed) Windows ends the process and everything it started.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "common/util.h"

namespace r2r {

class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();  // ends the process (and what it started) if it still runs
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Starts exe without a console window. commandLine is the whole line, exe
    // included. Returns an error message, or empty.
    std::string Start(const fs::path& exe, std::wstring commandLine);

    bool Running() const { return process_ != nullptr; }

    // Does not block: the exit code once the process has ended by itself (after
    // which Running() is false), otherwise nothing.
    std::optional<uint32_t> PollExit();

    // Gives the process up to `grace` to end by itself (ask it to first), then ends
    // it and everything it started.
    void Stop(std::chrono::milliseconds grace);

private:
    void Close();

    void* process_ = nullptr;  // HANDLE
    void* job_ = nullptr;      // HANDLE
};

}  // namespace r2r
