#pragma once

// Keeps an eye on one of the servers the settings point at (the export server, or
// the exposure-match server) and starts the app's own tools, built next to
// raw2raw2.exe, when asked:
//   export:   r2r-photoshop-export.exe <Photoshop.exe> / r2r-lightcraft-export.exe <lightcraft-cli.exe>
//   metering: r2r-exposure-match.exe (needs no application)

#include <chrono>
#include <future>
#include <optional>
#include <string>

#include "app/child_process.h"
#include "app/settings.h"
#include "net/export_client.h"

namespace r2r {

enum class ServerRole { Export, Meter };

class ServerManager {
public:
    ServerManager(Settings& settings, ServerRole role);
    ~ServerManager();
    ServerManager(const ServerManager&) = delete;
    ServerManager& operator=(const ServerManager&) = delete;

    ServerRole Role() const { return role_; }

    // Call every frame: picks up a finished health check and starts the next when
    // due (every few seconds, sooner after a change).
    void Poll();
    void CheckNow();
    bool Busy() const { return pending_.valid(); }
    bool TakeChanged();  // the health changed since the last call

    const ServerHealth& Health() const { return health_; }
    bool Checked() const { return !healthAddress_.empty(); }  // at least one health check has answered
    // host:port from the settings, and whether the user may launch the app's own tool there.
    std::string Address() const;
    bool BuiltIn() const;
    // The tool name a healthy built-in server reports ("lightcraft", "photoshop", "exposure-match").
    std::string ExpectedTool() const;

    // Starts the app's tool; `application` is what an export tool drives (ignored
    // for metering). Returns an error message, or empty.
    std::string Launch(const fs::path& application);
    bool LaunchedRunning() const { return process_.Running(); }
    bool Starting() const { return starting_ || autoStart_.has_value(); }
    std::string LaunchError() const { return launchError_; }
    // Asks the tool we launched to finish the work in flight and exit, and ends it
    // (with anything it started) if it has not within a few seconds. Blocks until then.
    void StopLaunched();

    // At startup: the metering server always; the LightCraft export server when
    // lightcraft-cli.exe sits next to the app. Does not block (on Windows, a connect
    // to a closed local port takes about 2 s to fail): the tool is launched
    // from Poll() once the first health check finds nobody serving the port (an
    // earlier instance of this app may be).
    void AutoStart();

    static fs::path AppDirectory();
    fs::path ToolExecutable() const;

private:
    Settings& settings_;
    ServerRole role_;
    std::future<ServerHealth> pending_;
    std::string pendingAddress_;
    ServerHealth health_;
    std::string healthAddress_;
    bool changed_ = false;
    bool checkSoon_ = true;
    std::chrono::steady_clock::time_point lastCheck_;

    ChildProcess process_;      // the tool we launched
    std::string launchedHost_;  // where it listens, whatever the settings say since
    uint16_t launchedPort_ = 0;
    bool starting_ = false;
    std::optional<fs::path> autoStart_;  // the application to launch with, until the first check answers
    std::string launchError_;
    std::chrono::steady_clock::time_point launchedAt_;
};

}  // namespace r2r
