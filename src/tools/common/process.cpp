#include "tools/common/process.h"

#include <format>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace r2r::tools {

namespace {

std::wstring CommandLine(const std::filesystem::path& exe, const std::vector<std::wstring>& args) {
    std::wstring line = QuoteArgument(exe.wstring());
    for (const std::wstring& arg : args) {
        line += L' ';
        line += QuoteArgument(arg);
    }
    return line;
}

std::string LastErrorText() {
    const DWORD code = GetLastError();
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string result = text ? Narrow(text) : std::string();
    LocalFree(text);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' ')) {
        result.pop_back();
    }
    return std::format("{} (error {})", result, code);
}

}  // namespace

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// The quoting CommandLineToArgvW undoes: backslashes only matter before a quote.
std::wstring QuoteArgument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

ProcessResult RunProcess(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                         std::chrono::milliseconds timeout) {
    ProcessResult result;

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0)) {
        result.error = "CreatePipe failed: " + LastErrorText();
        return result;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;
    PROCESS_INFORMATION process{};
    std::wstring line = CommandLine(exe, args);
    const BOOL started = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &startup, &process);
    CloseHandle(writeEnd);
    if (!started) {
        result.error = std::format("could not start {}: {}", Narrow(exe.wstring()), LastErrorText());
        CloseHandle(readEnd);
        return result;
    }
    CloseHandle(process.hThread);

    // Drain the pipe while the process runs; it blocks on a full pipe otherwise.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[16384];
    bool exited = false;
    while (true) {
        DWORD available = 0;
        if (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD read = 0;
            if (ReadFile(readEnd, buffer, static_cast<DWORD>(std::min<DWORD>(available, sizeof(buffer))), &read, nullptr) && read) {
                result.output.append(buffer, read);
            }
            continue;
        }
        if (exited) {
            break;
        }
        if (WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0) {
            exited = true;  // one more pass drains what is left
            continue;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            result.timedOut = true;
            exited = true;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    result.exitCode = static_cast<int>(code);
    CloseHandle(process.hProcess);
    CloseHandle(readEnd);
    return result;
}

std::string StartDetached(const std::filesystem::path& exe, const std::vector<std::wstring>& args) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring line = CommandLine(exe, args);
    // Leave the job raw2raw2 runs us in (it ends the job's processes when it exits),
    // so the application outlives us. Outside such a job, or in one that forbids
    // leaving, start it normally.
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr,
                        &startup, &process) &&
        !CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        return std::format("could not start {}: {}", Narrow(exe.wstring()), LastErrorText());
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return {};
}

}  // namespace r2r::tools
