#include "app/server_manager.h"

#include <SDL3/SDL.h>

#include <format>

#include "net/http_client.h"

namespace r2r {

namespace {

constexpr auto kCheckInterval = std::chrono::seconds(5);
constexpr auto kCheckIntervalWhileStarting = std::chrono::seconds(1);
constexpr auto kStartTimeout = std::chrono::minutes(4);  // Photoshop can take a while
constexpr int kShutdownRequestTimeoutMs = 2000;
constexpr auto kShutdownGrace = std::chrono::seconds(5);  // for the exports in flight to answer

std::wstring QuoteArgument(const std::wstring& arg) {
    if (arg.find_first_of(L" \t\"") == std::wstring::npos && !arg.empty()) {
        return arg;
    }
    std::wstring out = L"\"";
    for (wchar_t c : arg) {
        if (c == L'"') {
            out += L'\\';
        }
        out += c;
    }
    return out + L'"';
}

}  // namespace

ServerManager::ServerManager(Settings& settings, ServerRole role) : settings_(settings), role_(role) {}

ServerManager::~ServerManager() {
    StopLaunched();
    if (pending_.valid()) {
        pending_.wait();
    }
}

fs::path ServerManager::AppDirectory() {
    const char* base = SDL_GetBasePath();
    return base ? PathFromUtf8(base) : fs::current_path();
}

std::string ServerManager::Address() const {
    return role_ == ServerRole::Export ? settings_.Address() : settings_.MeterAddress();
}

bool ServerManager::BuiltIn() const {
    return role_ == ServerRole::Export ? settings_.exportTool != ExportTool::Custom
                                       : settings_.meterServer == MeterServer::BuiltIn;
}

std::string ServerManager::ExpectedTool() const {
    if (role_ == ServerRole::Meter) {
        return "exposure-match";
    }
    return ExportToolName(settings_.exportTool);
}

fs::path ServerManager::ToolExecutable() const {
    if (role_ == ServerRole::Meter) {
        return AppDirectory() / "r2r-exposure-match.exe";
    }
    switch (settings_.exportTool) {
        case ExportTool::Photoshop:
            return AppDirectory() / "r2r-photoshop-export.exe";
        case ExportTool::LightCraft:
            return AppDirectory() / "r2r-lightcraft-export.exe";
        default:
            return {};
    }
}

void ServerManager::CheckNow() {
    checkSoon_ = true;
}

bool ServerManager::TakeChanged() {
    return std::exchange(changed_, false);
}

void ServerManager::Poll() {
    const auto now = std::chrono::steady_clock::now();

    // A tool we started: notice when it exits or when it comes up.
    if (const std::optional<uint32_t> code = process_.PollExit()) {
        if (starting_) {
            launchError_ = std::format("The server exited right away (code {}). Run {} from a terminal to see why.",
                                       *code, Utf8(ToolExecutable().filename()));
        }
        starting_ = false;
        changed_ = true;
    } else if (process_.Running() && starting_ && now - launchedAt_ > kStartTimeout) {
        launchError_ = "The server is taking too long to start.";
        starting_ = false;
        changed_ = true;
    }

    // Pick up the check in flight.
    if (pending_.valid() && pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        ServerHealth health = pending_.get();
        const bool differs = health.reachable != health_.reachable || health.tool != health_.tool ||
                             health.version != health_.version || health.active != health_.active ||
                             health.waiting != health_.waiting || health.error != health_.error ||
                             pendingAddress_ != healthAddress_;
        health_ = std::move(health);
        healthAddress_ = pendingAddress_;
        lastCheck_ = now;
        if (health_.reachable && starting_) {
            starting_ = false;
            launchError_.clear();
        }
        changed_ = changed_ || differs;

        // The first answer after AutoStart(): launch unless someone already serves the port.
        if (const std::optional<fs::path> application = std::exchange(autoStart_, std::nullopt)) {
            if (!health_.reachable) {
                launchError_ = Launch(*application);
            }
            changed_ = true;
        }
    }

    // Start the next one when due.
    const auto interval = starting_ ? kCheckIntervalWhileStarting : kCheckInterval;
    const bool due = checkSoon_ || lastCheck_.time_since_epoch().count() == 0 || now - lastCheck_ > interval ||
                     healthAddress_ != Address();
    if (!pending_.valid() && due) {
        checkSoon_ = false;
        pendingAddress_ = Address();
        std::string host;
        uint16_t port = 0;
        if (!ParseAddress(pendingAddress_, host, port)) {
            health_ = ServerHealth{};
            health_.error = "the address must look like host:port";
            healthAddress_ = pendingAddress_;
            lastCheck_ = now;
            changed_ = true;
            return;
        }
        pending_ = std::async(std::launch::async, CheckExportServer, host, port);
    }
}

std::string ServerManager::Launch(const fs::path& application) {
    const fs::path exe = ToolExecutable();
    std::error_code ec;
    if (exe.empty() || !BuiltIn()) {
        return "A custom server is not started by this app.";
    }
    if (!fs::is_regular_file(exe, ec)) {
        return std::format("{} is not next to raw2raw2.exe. Build the export tools (they are part of the project).",
                           Utf8(exe.filename()));
    }
    if (role_ == ServerRole::Export && !fs::is_regular_file(application, ec)) {
        return std::format("{} does not exist.", Utf8(application));
    }
    StopLaunched();

    std::string host;
    uint16_t port = 31000;
    ParseAddress(Address(), host, port);
    std::wstring line = QuoteArgument(exe.wstring());
    if (role_ == ServerRole::Export) {
        line += L" " + QuoteArgument(application.wstring());
    }
    line += L" --port " + std::to_wstring(port);
    if (std::string error = process_.Start(exe, std::move(line)); !error.empty()) {
        return error;
    }
    launchedPort_ = port;
    starting_ = true;
    launchError_.clear();
    launchedAt_ = std::chrono::steady_clock::now();
    changed_ = true;
    checkSoon_ = true;

    if (role_ == ServerRole::Export) {
        if (settings_.exportTool == ExportTool::Photoshop) {
            settings_.photoshopExe = application;
        } else {
            settings_.lightcraftExe = application;
        }
        (void)settings_.Save();
    }
    return {};
}

void ServerManager::StopLaunched() {
    if (!process_.Running()) {
        return;
    }
    starting_ = false;
    // Ask politely (the work in flight finishes), then make sure. The tools listen
    // on 127.0.0.1 at the port they were started with, whatever the settings say now.
    (void)HttpRequest("POST", "127.0.0.1", launchedPort_, "/shutdown", "{}", kShutdownRequestTimeoutMs);
    process_.Stop(kShutdownGrace);
    changed_ = true;
    checkSoon_ = true;
}

void ServerManager::AutoStart() {
    if (!BuiltIn()) {
        return;
    }
    fs::path application;
    if (role_ == ServerRole::Export) {
        if (settings_.exportTool != ExportTool::LightCraft) {
            return;
        }
        application = AppDirectory() / "lightcraft-cli.exe";
        std::error_code ec;
        if (!fs::is_regular_file(application, ec)) {
            return;
        }
    }
    autoStart_ = std::move(application);
    checkSoon_ = true;
}

}  // namespace r2r
