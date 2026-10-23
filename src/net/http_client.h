#pragma once

// A small HTTP client for talking to the local export services.

#include <cstdint>
#include <expected>
#include <string>

struct HttpReply {
    int status = 0;
    std::string body;
};

// Sends one request and reads the whole reply. timeoutMs bounds the wait for the
// reply (an export can take minutes). Errors are transport errors; an HTTP error
// status comes back as a reply.
std::expected<HttpReply, std::string> HttpRequest(const std::string& method, const std::string& host, uint16_t port,
                                                  const std::string& path, const std::string& body, uint32_t timeoutMs);

// Splits "host:port" (or "host", port 31000 by default). False if malformed.
bool ParseAddress(const std::string& text, std::string& host, uint16_t& port);
