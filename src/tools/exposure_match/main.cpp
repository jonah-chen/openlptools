// r2r-exposure-match [--port 31001] [--jobs N]
//
// Serves one question over HTTP on 127.0.0.1: how many stops brighter is one raw
// file than another? The app's Exposure Match asks it for every frame of a sequence
// and does the rest (fitting, CSV, sidecars) itself.
//
//   POST /compare   {"id": any, "reference": "C:/a.dng", "image": "C:/b.dng"}
//                   -> {"ok": true, "id": any, "stops": 0.31, "spreadEv": 0.02,
//                       "validFraction": 0.97, "method": "pixels" | "percentiles", "seconds": 1.2}
//                   stops = log2(brightness of image / brightness of reference): positive
//                   means image is brighter. A bad request answers HTTP 400; a file that
//                   cannot be read answers {"ok": false, "error": "..."}.
//   GET  /health    {"ok": true, "tool": "exposure-match", "version": "1", "cached": n, "active": n}
//   POST /shutdown
//
// Metered images are cached by path, so a sequence compared against one reference
// decodes each file once. Up to --jobs files decode at once (default: a quarter of
// the cores, each decode using the rest).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <future>
#include <list>
#include <mutex>
#include <semaphore>
#include <thread>
#include <unordered_map>

#include "dng_xmp_sdk.h"
#include "processing/metering.h"
#include "tools/common/export_service.h"
#include "tools/common/http_server.h"
#include "tools/common/process.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

using namespace r2r::tools;

constexpr size_t kCacheEntries = 512;  // about 200 KB each

// Metered images by file, with the decodes in flight shared between requests.
class MeterCache {
public:
    MeterCache(int jobs, uint32_t threadsPerDecode)
        : slots_(std::clamp(jobs, 1, 64)), threads_(threadsPerDecode) {}

    std::shared_ptr<const std::expected<MeterImage, std::string>> Get(const fs::path& path) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        const auto time = fs::last_write_time(path, ec);
        const std::wstring key = path.wstring();
        std::shared_future<std::shared_ptr<const std::expected<MeterImage, std::string>>> future;
        {
            std::scoped_lock lock(mutex_);
            if (auto it = entries_.find(key); it != entries_.end() && it->second.size == size && it->second.time == time) {
                order_.splice(order_.begin(), order_, it->second.where);  // most recently used
                future = it->second.future;
            } else {
                std::promise<std::shared_ptr<const std::expected<MeterImage, std::string>>> promise;
                future = promise.get_future().share();
                if (it != entries_.end()) {
                    order_.erase(it->second.where);
                    entries_.erase(it);
                }
                order_.push_front(key);
                entries_[key] = Entry{size, time, future, order_.begin()};
                while (entries_.size() > kCacheEntries) {
                    entries_.erase(order_.back());
                    order_.pop_back();
                }
                // This request decodes; others wanting the same file wait on the future.
                std::thread([this, path, p = std::move(promise)]() mutable {
                    slots_.acquire();
                    ++active_;
                    auto result = std::make_shared<const std::expected<MeterImage, std::string>>(MeterFile(path, threads_));
                    --active_;
                    slots_.release();
                    p.set_value(std::move(result));
                }).detach();
            }
        }
        return future.get();
    }

    size_t Size() {
        std::scoped_lock lock(mutex_);
        return entries_.size();
    }
    int Active() const { return active_; }

private:
    struct Entry {
        uintmax_t size;
        fs::file_time_type time;
        std::shared_future<std::shared_ptr<const std::expected<MeterImage, std::string>>> future;
        std::list<std::wstring>::iterator where;
    };
    std::counting_semaphore<64> slots_;
    uint32_t threads_;
    std::mutex mutex_;
    std::unordered_map<std::wstring, Entry> entries_;
    std::list<std::wstring> order_;  // most recently used first
    std::atomic<int> active_{0};
};

HttpResponse Reply(int status, const Json& body) {
    return HttpResponse{status, "application/json; charset=utf-8", body.dump()};
}

std::atomic<bool> gStopping{false};
HttpServer* gServer = nullptr;

BOOL WINAPI OnConsoleCtrl(DWORD) {
    if (gServer) {
        gServer->Stop();
    }
    return TRUE;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    const ToolOptions options = ParseToolOptions(argc, argv, false);
    if (options.help || !options.error.empty()) {
        if (!options.error.empty()) {
            std::fprintf(stderr, "%s\n\n", options.error.c_str());
        }
        std::fprintf(stderr,
                     "Usage: r2r-exposure-match [--port N] [--jobs N]\n\n"
                     "Serves POST /compare {\"reference\": path, \"image\": path} on http://127.0.0.1:N/ (default\n"
                     "port 31001), answering {\"ok\": true, \"stops\": s, ...}: how many stops brighter the image\n"
                     "is than the reference. --jobs files decode at once (default: a quarter of the cores).\n");
        return options.help ? 0 : 2;
    }
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const int jobs = options.jobs > 0 ? options.jobs : static_cast<int>(std::max(1u, cores / 4));
    const uint32_t threadsPerDecode = std::max(1u, cores / static_cast<unsigned>(jobs));
    const uint16_t port = options.port == 31000 ? 31001 : options.port;  // the tools' default is the export port

    dng_xmp_sdk::InitializeSDK();
    MeterCache cache(jobs, threadsPerDecode);
    HttpServer server;
    std::atomic<int> active{0};
    const std::string started = server.Start(port, [&](const HttpRequest& http) -> HttpResponse {
        if (http.path == "/health" || http.path == "/") {
            return Reply(200, {{"ok", true}, {"tool", "exposure-match"}, {"version", "1"}, {"cached", cache.Size()},
                               {"active", active.load()}, {"decoding", cache.Active()}, {"jobs", jobs}});
        }
        if (http.path == "/shutdown") {
            if (http.method != "POST") {
                return Reply(405, {{"ok", false}, {"error", "POST /shutdown"}});
            }
            server.Stop();
            return Reply(200, {{"ok", true}});
        }
        if (http.path != "/compare") {
            return Reply(404, {{"ok", false}, {"error", "unknown path; use POST /compare, GET /health, POST /shutdown"}});
        }
        if (http.method != "POST") {
            return Reply(405, {{"ok", false}, {"error", "POST /compare with a JSON body"}});
        }
        Json request = Json::parse(http.body, nullptr, false);
        if (request.is_discarded() || !request.is_object()) {
            return Reply(400, {{"ok", false}, {"error", "the body is not a JSON object"}});
        }
        Json answer = {{"ok", false}};
        if (request.contains("id")) {
            answer["id"] = request["id"];
        }
        auto pathField = [&](const char* name) -> fs::path {
            const auto it = request.find(name);
            return it != request.end() && it->is_string() ? fs::path(Widen(it->get<std::string>())) : fs::path();
        };
        const fs::path reference = pathField("reference");
        const fs::path image = pathField("image");
        std::error_code ec;
        for (const auto& [name, path] : {std::pair{"reference", reference}, std::pair{"image", image}}) {
            if (path.empty()) {
                answer["error"] = std::format("\"{}\" (a file path) is missing", name);
                return Reply(400, answer);
            }
            if (!fs::is_regular_file(path, ec)) {
                answer["error"] = std::format("{} not found: {}", name, Narrow(path.wstring()));
                return Reply(400, answer);
            }
        }

        const auto t0 = std::chrono::steady_clock::now();
        ++active;
        // Both decodes run at once when the cache has neither.
        auto imageFuture = std::async(std::launch::async, [&] { return cache.Get(image); });
        const auto ref = cache.Get(reference);
        const auto img = imageFuture.get();
        --active;
        if (!*ref) {
            answer["error"] = std::format("cannot read the reference {}: {}", Narrow(reference.filename().wstring()), ref->error());
            return Reply(200, answer);
        }
        if (!*img) {
            answer["error"] = std::format("cannot read {}: {}", Narrow(image.filename().wstring()), img->error());
            return Reply(200, answer);
        }
        const Comparison c = CompareMeters(**ref, **img);
        answer["ok"] = true;
        answer["stops"] = c.deltaEv;
        answer["spreadEv"] = c.spreadEv;
        answer["validFraction"] = c.validFraction;
        answer["method"] = c.percentile ? "percentiles" : "pixels";
        answer["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return Reply(200, answer);
    });
    if (!started.empty()) {
        std::fprintf(stderr, "Cannot start the service: %s\n", started.c_str());
        dng_xmp_sdk::TerminateSDK();
        return 1;
    }
    gServer = &server;
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    std::printf("exposure-match service on http://127.0.0.1:%u/  (POST /compare, GET /health, POST /shutdown; %d decode(s) at a time)\n",
                server.Port(), jobs);
    std::fflush(stdout);
    server.Run();
    gServer = nullptr;
    dng_xmp_sdk::TerminateSDK();
    std::printf("Stopped.\n");
    return 0;
}
