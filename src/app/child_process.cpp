#include "app/child_process.h"

#include <format>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace r2r {

namespace {

// A job that ends its processes when its last handle closes. Processes in it may
// start others outside it (CREATE_BREAKAWAY_FROM_JOB): the Photoshop export tool
// does so for Photoshop, which holds the user's documents and must outlive us.
HANDLE CreateKillOnCloseJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        return nullptr;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

}  // namespace

ChildProcess::~ChildProcess() {
    Stop(std::chrono::milliseconds(0));
}

std::string ChildProcess::Start(const fs::path& exe, std::wstring commandLine) {
    Stop(std::chrono::milliseconds(0));

    // Start suspended so the process is in the job before it can start anything.
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup,
                        &process)) {
        return std::format("Could not start {} (error {}).", Utf8(exe.filename()), GetLastError());
    }
    HANDLE job = CreateKillOnCloseJob();
    if (job && !AssignProcessToJobObject(job, process.hProcess)) {
        // Still run it; it is then only stopped by Stop(), not if the app crashes.
        CloseHandle(job);
        job = nullptr;
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    process_ = process.hProcess;
    job_ = job;
    return {};
}

std::optional<uint32_t> ChildProcess::PollExit() {
    if (!process_ || WaitForSingleObject(static_cast<HANDLE>(process_), 0) != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    Close();
    return code;
}

void ChildProcess::Stop(std::chrono::milliseconds grace) {
    if (!process_) {
        return;
    }
    HANDLE process = static_cast<HANDLE>(process_);
    if (WaitForSingleObject(process, static_cast<DWORD>(grace.count())) != WAIT_OBJECT_0) {
        if (job_) {
            TerminateJobObject(static_cast<HANDLE>(job_), 1);
        } else {
            TerminateProcess(process, 1);
        }
        WaitForSingleObject(process, 5000);
    }
    Close();
}

// Closing the job ends whatever the process left running (renders it started, say).
void ChildProcess::Close() {
    if (job_) {
        CloseHandle(static_cast<HANDLE>(job_));
        job_ = nullptr;
    }
    if (process_) {
        CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
}

}  // namespace r2r
