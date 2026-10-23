#include "tools/common/export_service.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <format>
#include <stdexcept>

#include "tools/common/http_server.h"
#include "tools/common/process.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace r2r::tools {

namespace {

std::string Lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

fs::path PathField(const Json& j, const char* name) {
    if (const auto it = j.find(name); it != j.end() && it->is_string()) {
        return fs::path(Widen(it->get<std::string>()));
    }
    return {};
}

int IntField(const Json& j, const char* name) {
    if (const auto it = j.find(name); it != j.end() && it->is_number()) {
        return it->get<int>();
    }
    return 0;
}

std::string StringField(const Json& j, const char* name) {
    if (const auto it = j.find(name); it != j.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return {};
}

HttpResponse JsonResponse(int status, const Json& body) {
    return HttpResponse{status, "application/json; charset=utf-8", body.dump()};
}

std::atomic<bool> gInterrupted{false};
HttpServer* gServer = nullptr;

BOOL WINAPI OnConsoleCtrl(DWORD) {
    gInterrupted = true;
    if (gServer) {
        gServer->Stop();
    }
    return TRUE;
}

}  // namespace

Json ExportRequest::Section(const char* name) const {
    if (const auto it = raw.find(name); it != raw.end() && it->is_object()) {
        return *it;
    }
    return Json::object();
}

std::expected<ExportRequest, std::string> ParseExportRequest(const std::string& body) {
    Json j = Json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return std::unexpected("the body is not a JSON object");
    }
    ExportRequest r;
    r.raw = j;
    if (j.contains("id")) {
        r.id = j["id"];
    }
    r.input = PathField(j, "input");
    r.output = PathField(j, "output");
    if (r.input.empty()) {
        return std::unexpected("\"input\" (the file to export) is missing");
    }
    if (r.output.empty()) {
        return std::unexpected("\"output\" (the file to write) is missing");
    }
    std::error_code ec;
    if (!fs::is_regular_file(r.input, ec)) {
        return std::unexpected(std::format("input file not found: {}", Narrow(r.input.wstring())));
    }
    r.format = Lower(StringField(j, "format"));
    if (r.format.empty()) {
        std::string ext = Lower(Narrow(r.output.extension().wstring()));
        if (!ext.empty() && ext[0] == '.') {
            ext.erase(0, 1);
        }
        r.format = ext;
    }
    if (r.format == "tif") {
        r.format = "tiff";
    } else if (r.format == "jpg") {
        r.format = "jpeg";
    }
    if (r.format.empty()) {
        return std::unexpected("\"format\" is missing and the output has no extension");
    }
    r.bitDepth = IntField(j, "bitDepth");
    if (r.bitDepth != 0 && r.bitDepth != 8 && r.bitDepth != 16) {
        return std::unexpected("\"bitDepth\" must be 8 or 16");
    }
    r.colorSpace = StringField(j, "colorSpace");
    r.quality = std::clamp(IntField(j, "quality"), 0, 100);
    r.longEdge = std::max(0, IntField(j, "longEdge"));
    r.ppi = std::max(0, IntField(j, "ppi"));
    return r;
}

ToolOptions ParseToolOptions(int argc, wchar_t* argv[], bool requireProgram) {
    ToolOptions o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == L"--port" && hasValue) {
            const int port = _wtoi(argv[++i]);
            if (port <= 0 || port > 65535) {
                o.error = "--port takes a number from 1 to 65535";
            }
            o.port = static_cast<uint16_t>(port);
        } else if (arg == L"--jobs" && hasValue) {
            o.jobs = std::max(1, _wtoi(argv[++i]));
        } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            o.help = true;
        } else if (arg.starts_with(L"--")) {
            o.error = "unknown option " + Narrow(arg);
        } else if (o.program.empty()) {
            o.program = fs::path(arg);
        } else {
            o.error = "unexpected argument " + Narrow(arg);
        }
    }
    if (o.error.empty() && !o.help && o.program.empty() && requireProgram) {
        o.error = "the path of the program to drive is missing";
    }
    return o;
}

int RunExportService(ExportBackend& backend, uint16_t port) {
    HttpServer server;
    std::atomic<int> active{0};
    std::atomic<int> waiting{0};

    const std::string started = server.Start(port, [&](const HttpRequest& http) -> HttpResponse {
        if (http.path == "/health" || http.path == "/") {
            Json status = backend.Status();
            status["ok"] = true;
            status["tool"] = backend.Name();
            status["active"] = active.load();
            status["waiting"] = waiting.load();
            return JsonResponse(200, status);
        }
        if (http.path == "/shutdown") {
            if (http.method != "POST") {
                return JsonResponse(405, {{"ok", false}, {"error", "POST /shutdown"}});
            }
            server.Stop();
            return JsonResponse(200, {{"ok", true}, {"message", "stopping after the exports in flight"}});
        }
        if (http.path != "/export") {
            return JsonResponse(404, {{"ok", false}, {"error", "unknown path; use POST /export, GET /health, POST /shutdown"}});
        }
        if (http.method != "POST") {
            return JsonResponse(405, {{"ok", false}, {"error", "POST /export with a JSON body"}});
        }
        auto parsed = ParseExportRequest(http.body);
        if (!parsed) {
            return JsonResponse(400, {{"ok", false}, {"error", parsed.error()}});
        }
        const ExportRequest& request = *parsed;
        Json answer = {{"ok", false}, {"id", request.id}, {"output", Narrow(request.output.wstring())}};

        std::error_code ec;
        if (const fs::path dir = request.output.parent_path(); !dir.empty()) {
            fs::create_directories(dir, ec);
        }
        const auto t0 = std::chrono::steady_clock::now();
        ++waiting;
        try {
            ++active;
            --waiting;
            Json result = backend.Export(request);
            --active;
            for (auto& [k, v] : result.items()) {
                answer[k] = v;
            }
            answer["ok"] = true;
        } catch (const std::exception& e) {
            --active;
            answer["error"] = e.what();
        }
        answer["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s %s -> %s%s\n", answer["ok"].get<bool>() ? "done " : "FAILED", Narrow(request.input.filename().wstring()).c_str(),
                    Narrow(request.output.filename().wstring()).c_str(),
                    answer["ok"].get<bool>() ? "" : (": " + answer["error"].get<std::string>()).c_str());
        std::fflush(stdout);
        return JsonResponse(200, answer);
    });
    if (!started.empty()) {
        std::fprintf(stderr, "Cannot start the service: %s\n", started.c_str());
        return 1;
    }

    gServer = &server;
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    std::printf("%s export service on http://127.0.0.1:%u/  (POST /export, GET /health, POST /shutdown; Ctrl+C to stop)\n",
                backend.Name().c_str(), server.Port());
    std::fflush(stdout);
    server.Run();
    gServer = nullptr;
    std::printf("Stopped.\n");
    return 0;
}

}  // namespace r2r::tools
