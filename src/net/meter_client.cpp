#include "net/meter_client.h"

#include <format>

#include "net/http_client.h"
#include "json.hpp"

namespace {

using Json = nlohmann::json;

constexpr uint32_t kCompareTimeoutMs = 10 * 60 * 1000;  // two large decodes, behind a queue

std::string Utf8(const std::filesystem::path& path) {
    const std::u8string s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

}  // namespace

std::expected<Comparison, std::string> CompareRemote(const MeterEndpoint& endpoint, const std::filesystem::path& reference,
                                                     const std::filesystem::path& image) {
    const Json request = {{"reference", Utf8(reference)}, {"image", Utf8(image)}};
    auto reply = HttpRequest("POST", endpoint.host, endpoint.port, "/compare", request.dump(), kCompareTimeoutMs);
    if (!reply) {
        return std::unexpected(std::format("exposure-match server at {}:{}: {}", endpoint.host, endpoint.port, reply.error()));
    }
    Json j = Json::parse(reply->body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return std::unexpected(std::format("unreadable answer from the exposure-match server (HTTP {})", reply->status));
    }
    if (!j.value("ok", false)) {
        return std::unexpected(j.value("error", std::format("HTTP {}", reply->status)));
    }
    Comparison c;
    c.deltaEv = j.value("stops", 0.0);
    c.spreadEv = j.value("spreadEv", 0.0);
    c.validFraction = j.value("validFraction", 0.0);
    c.percentile = j.value("method", "pixels") == "percentiles";
    return c;
}
