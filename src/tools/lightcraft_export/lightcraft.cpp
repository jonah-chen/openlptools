#include "tools/lightcraft_export/lightcraft.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <regex>
#include <stdexcept>

#include "tools/common/process.h"

namespace r2r::tools {

namespace {

constexpr auto kRenderTimeout = std::chrono::minutes(30);

std::string Lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// LightCraft's names for the output colour spaces.
std::string ColorSpaceName(const std::string& name) {
    const std::string n = Lower(name);
    if (n.empty()) return {};
    if (n == "srgb") return "sRGB";
    if (n == "adobergb" || n == "adobergb1998" || n == "adobe rgb") return "adobeRGB";
    if (n == "prophotorgb" || n == "prophoto") return "proPhoto";
    if (n == "displayp3" || n == "p3") return "displayP3";
    if (n == "rec2020") return "rec2020";
    return name;  // passed through for anything newer
}

std::string OptionValue(const Json& v) {
    return v.is_string() ? v.get<std::string>() : v.dump();
}

// The last lines of a program's output, for an error message.
std::string Tail(const std::string& output, size_t lines) {
    size_t pos = output.size();
    for (size_t i = 0; i < lines && pos > 0; ++i) {
        pos = output.find_last_of('\n', pos - 1);
        if (pos == std::string::npos) {
            pos = 0;
            break;
        }
    }
    std::string tail = output.substr(pos);
    while (!tail.empty() && std::isspace(static_cast<unsigned char>(tail.front()))) {
        tail.erase(0, 1);
    }
    while (!tail.empty() && std::isspace(static_cast<unsigned char>(tail.back()))) {
        tail.pop_back();
    }
    return tail;
}

}  // namespace

LightCraftBackend::LightCraftBackend(fs::path cli, int jobs)
    : cli_(std::move(cli)), jobs_(std::clamp(jobs, 1, 64)), slots_(std::clamp(jobs, 1, 64)) {}

std::string LightCraftBackend::Check() {
    std::error_code ec;
    if (!fs::is_regular_file(cli_, ec)) {
        return std::format("{} does not exist", Narrow(cli_.wstring()));
    }
    const ProcessResult r = RunProcess(cli_, {L"--version"}, std::chrono::seconds(60));
    if (!r.error.empty()) {
        return r.error;
    }
    version_ = Tail(r.output, 1);
    if (version_.empty()) {
        version_ = Narrow(cli_.filename().wstring());
    }
    return {};
}

Json LightCraftBackend::Status() {
    return {{"lightcraft", version_}, {"jobs", jobs_}};
}

Json LightCraftBackend::Export(const ExportRequest& request) {
    const Json lc = request.Section("lightcraft");
    std::vector<std::wstring> args = {L"render", request.input.wstring(), L"-o", request.output.wstring()};
    auto opt = [&](const std::string& key, const std::string& value) {
        args.push_back(L"--opt");
        args.push_back(Widen(key + "=" + value));
    };
    if (request.longEdge > 0) {
        args.push_back(L"--size");
        args.push_back(std::to_wstring(request.longEdge));
    }
    if (request.quality > 0) {
        args.push_back(L"--quality");
        args.push_back(std::to_wstring(request.quality));
    }
    if (request.bitDepth > 0) {
        opt("bitDepth", std::to_string(request.bitDepth));
    }
    if (const std::string space = ColorSpaceName(request.colorSpace); !space.empty()) {
        opt("colorSpace", space);
    }
    if (request.ppi > 0) {
        opt("ppi", std::to_string(request.ppi));
    }
    if (!request.output.has_extension() || Lower(Narrow(request.output.extension().wstring())) != "." + (request.format == "jpeg" ? "jpg" : request.format)) {
        // The extension decides the format unless told otherwise.
        opt("format", request.format == "jpeg" ? "jpg" : request.format);
    }
    if (const auto it = lc.find("metadata"); it != lc.end() && it->is_string()) {
        opt("metadata", it->get<std::string>());
    }
    if (const auto it = lc.find("opt"); it != lc.end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
            opt(key, OptionValue(value));
        }
    }
    if (const auto it = lc.find("set"); it != lc.end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
            if (!value.is_number()) {
                throw std::runtime_error("lightcraft.set values must be numbers");
            }
            args.push_back(L"--set");
            args.push_back(Widen(key + "=" + value.dump()));
        }
    }
    if (const auto it = lc.find("settings"); it != lc.end() && it->is_string()) {
        args.push_back(L"--settings");
        args.push_back(Widen(it->get<std::string>()));
    }
    if (const auto it = lc.find("preset"); it != lc.end() && it->is_string()) {
        args.push_back(L"--preset");
        args.push_back(Widen(it->get<std::string>()));
    }

    slots_.acquire();  // at most `jobs` renders at once
    const ProcessResult r = RunProcess(cli_, args, kRenderTimeout);
    slots_.release();

    if (!r.error.empty()) {
        throw std::runtime_error(r.error);
    }
    if (r.timedOut) {
        throw std::runtime_error("lightcraft-cli did not finish within 30 minutes");
    }
    if (r.exitCode != 0) {
        throw std::runtime_error(std::format("lightcraft-cli exited with {}: {}", r.exitCode, Tail(r.output, 6)));
    }
    // "lightcraft-cli: wrote OUT (W×H)"
    Json result = Json::object();
    static const std::regex kWrote(R"(wrote .*\((\d+)\D+(\d+)\)\s*$)");
    std::smatch m;
    if (std::regex_search(r.output, m, kWrote)) {
        result["width"] = std::stoi(m[1]);
        result["height"] = std::stoi(m[2]);
    }
    if (const std::string warnings = Tail(r.output, 3); warnings.find("warning") != std::string::npos) {
        result["warnings"] = warnings;
    }
    return result;
}

}  // namespace r2r::tools
