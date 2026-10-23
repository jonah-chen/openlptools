#include "net/export_client.h"

#include <algorithm>
#include <format>

#include "net/http_client.h"
#include "json.hpp"
#include "common/parallel.h"

namespace {

using Json = nlohmann::json;

constexpr uint32_t kHealthTimeoutMs = 3000;
constexpr uint32_t kExportTimeoutMs = 30 * 60 * 1000;

std::string Utf8(const std::filesystem::path& path) {
    const std::u8string s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

}  // namespace

ServerHealth CheckExportServer(const std::string& host, uint16_t port) {
    ServerHealth h;
    auto reply = HttpRequest("GET", host, port, "/health", "", kHealthTimeoutMs);
    if (!reply) {
        h.error = reply.error();
        return h;
    }
    Json j = Json::parse(reply->body, nullptr, false);
    if (reply->status != 200 || j.is_discarded() || !j.is_object() || !j.value("ok", false)) {
        h.error = std::format("something answers at that address, but not an export server (HTTP {})", reply->status);
        return h;
    }
    h.reachable = true;
    h.tool = j.value("tool", "");
    h.active = j.value("active", 0);
    h.waiting = j.value("waiting", 0);
    if (j.contains(h.tool) && j[h.tool].is_string()) {
        h.version = j[h.tool].get<std::string>();  // "photoshop": "27.2.0 Adobe Photoshop"
    }
    return h;
}

std::string ExportFormatExtension(const std::string& format) {
    if (format == "tiff") return "tif";
    if (format == "jpeg") return "jpg";
    return format;
}

ExportJob::ExportJob(std::string host, uint16_t port, std::vector<ExportItem> items, ExportOptions options, int parallel)
    : host_(std::move(host)), port_(port), items_(std::move(items)), options_(std::move(options)),
      parallel_(std::clamp(parallel, 1, 16)) {}

ExportJob::~ExportJob() {
    Cancel();
}

void ExportJob::Start() {
    if (running_ || finished_) {
        return;
    }
    running_ = true;
    worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}

void ExportJob::Cancel() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

std::vector<ExportItem> ExportJob::Items() const {
    std::scoped_lock lock(mutex_);
    return items_;
}

void ExportJob::Run(std::stop_token stop) {
    ParallelForEach(items_.size(), static_cast<unsigned>(parallel_), [&](unsigned, size_t i) {
        if (stop.stop_requested()) {
            return;
        }
        ExportItem item;
        {
            std::scoped_lock lock(mutex_);
            item = items_[i];
        }
        Json request = {
            {"id", static_cast<int>(i)},
            {"input", Utf8(item.input)},
            {"output", Utf8(item.output)},
            {"format", options_.format},
            {"colorSpace", options_.colorSpace},
            {"photoshop", {{"tiffCompression", "zip"}}},
            {"lightcraft", {{"metadata", "all"}}},
        };
        if (options_.format != "jpeg" && options_.bitDepth > 0) {
            request["bitDepth"] = options_.bitDepth;
        }
        if (options_.format == "jpeg" && options_.quality > 0) {
            request["quality"] = options_.quality;
        }
        if (options_.longEdge > 0) {
            request["longEdge"] = options_.longEdge;
        }
        auto reply = HttpRequest("POST", host_, port_, "/export", request.dump(), kExportTimeoutMs);
        item.done = true;
        if (!reply) {
            item.error = reply.error();
        } else {
            Json j = Json::parse(reply->body, nullptr, false);
            if (j.is_discarded() || !j.is_object()) {
                item.error = std::format("unreadable answer (HTTP {})", reply->status);
            } else if (!j.value("ok", false)) {
                item.error = j.value("error", std::format("HTTP {}", reply->status));
            } else {
                item.ok = true;
                item.width = j.value("width", 0);
                item.height = j.value("height", 0);
                item.seconds = j.value("seconds", 0.0);
            }
        }
        {
            std::scoped_lock lock(mutex_);
            items_[i] = item;
        }
        ++completed_;
        if (!item.ok) {
            ++failed_;
        }
    });
    cancelled_ = stop.stop_requested() && completed_ < static_cast<int>(items_.size());
    finished_ = true;
    running_ = false;
}
