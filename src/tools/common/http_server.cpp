#include "tools/common/http_server.h"

#include <algorithm>
#include <cctype>
#include <format>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

namespace r2r::tools {

namespace {

constexpr size_t kMaxRequestBytes = 16u << 20;  // headers plus body
constexpr int kAcceptPollMs = 200;

std::string Lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) {
        return {};
    }
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Reads one request. Returns false if the connection closed or the request is malformed.
bool ReadRequest(SOCKET socket, HttpRequest& request, int& errorStatus) {
    std::string buffer;
    size_t headerEnd = std::string::npos;
    char chunk[8192];
    while (headerEnd == std::string::npos) {
        const int n = recv(socket, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(n));
        headerEnd = buffer.find("\r\n\r\n");
        if (buffer.size() > kMaxRequestBytes) {
            errorStatus = 413;
            return false;
        }
    }

    // Request line and headers.
    const std::string head = buffer.substr(0, headerEnd);
    size_t lineEnd = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineEnd);
    const size_t sp1 = requestLine.find(' ');
    const size_t sp2 = requestLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        errorStatus = 400;
        return false;
    }
    request.method = requestLine.substr(0, sp1);
    request.path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    if (const size_t q = request.path.find('?'); q != std::string::npos) {
        request.path.resize(q);
    }
    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t next = head.find("\r\n", pos);
        if (next == std::string::npos) {
            next = head.size();
        }
        const std::string line = head.substr(pos, next - pos);
        if (const size_t colon = line.find(':'); colon != std::string::npos) {
            request.headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
        }
        pos = next + 2;
    }

    // Body.
    size_t contentLength = 0;
    if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
        try {
            contentLength = std::stoull(it->second);
        } catch (...) {
            errorStatus = 400;
            return false;
        }
    }
    if (contentLength > kMaxRequestBytes) {
        errorStatus = 413;
        return false;
    }
    request.body = buffer.substr(headerEnd + 4);
    while (request.body.size() < contentLength) {
        const int n = recv(socket, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;
        }
        request.body.append(chunk, static_cast<size_t>(n));
    }
    request.body.resize(contentLength);
    return true;
}

void SendResponse(SOCKET socket, const HttpResponse& response) {
    const std::string head = std::format(
        "HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        response.status, HttpStatusText(response.status), response.contentType, response.body.size());
    const std::string all = head + response.body;
    size_t sent = 0;
    while (sent < all.size()) {
        const int n = send(socket, all.data() + sent, static_cast<int>(std::min<size_t>(all.size() - sent, 1 << 20)), 0);
        if (n <= 0) {
            return;
        }
        sent += static_cast<size_t>(n);
    }
    shutdown(socket, SD_SEND);
}

}  // namespace

const char* HttpStatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

struct HttpServer::Connection {
    SOCKET socket = INVALID_SOCKET;
    std::atomic<bool> done{false};
    std::thread thread;
};

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() {
    for (auto& c : connections_) {
        if (c->thread.joinable()) {
            c->thread.join();
        }
    }
}

std::string HttpServer::Start(uint16_t port, Handler handler) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return "WSAStartup failed";
    }
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return std::format("socket() failed: {}", WSAGetLastError());
    }
    // Only this machine may connect: the tools drive desktop applications.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    const BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int error = WSAGetLastError();
        closesocket(s);
        return error == WSAEADDRINUSE ? std::format("port {} is already in use", port)
                                      : std::format("bind() failed: {}", error);
    }
    if (listen(s, 64) != 0) {
        closesocket(s);
        return std::format("listen() failed: {}", WSAGetLastError());
    }
    listenSocket_ = static_cast<uintptr_t>(s);
    port_ = port;
    handler_ = std::move(handler);
    return {};
}

void HttpServer::Run() {
    const SOCKET listenSocket = static_cast<SOCKET>(listenSocket_);
    while (!stopping_) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listenSocket, &readable);
        timeval timeout{0, kAcceptPollMs * 1000};
        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            PruneConnections();
            continue;
        }
        const SOCKET client = accept(listenSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            continue;
        }
        auto connection = std::make_unique<Connection>();
        connection->socket = client;
        Connection* raw = connection.get();
        {
            std::scoped_lock lock(connectionsMutex_);
            connections_.push_back(std::move(connection));
        }
        raw->thread = std::thread([this, raw] {
            Serve(*raw);
            raw->done = true;
        });
        PruneConnections();
    }
    closesocket(listenSocket);
    // Let the exports in flight answer their clients.
    std::vector<std::unique_ptr<Connection>> remaining;
    {
        std::scoped_lock lock(connectionsMutex_);
        remaining.swap(connections_);
    }
    for (auto& c : remaining) {
        if (c->thread.joinable()) {
            c->thread.join();
        }
    }
    WSACleanup();
}

void HttpServer::Stop() {
    stopping_ = true;
}

void HttpServer::Serve(Connection& connection) {
    const SOCKET socket = connection.socket;
    HttpRequest request;
    int errorStatus = 0;
    HttpResponse response;
    if (ReadRequest(socket, request, errorStatus)) {
        try {
            response = handler_(request);
        } catch (const std::exception& e) {
            response.status = 500;
            response.body = std::format("{{\"ok\":false,\"error\":\"{}\"}}", e.what());
        }
    } else if (errorStatus != 0) {
        response.status = errorStatus;
        response.body = std::format("{{\"ok\":false,\"error\":\"{}\"}}", HttpStatusText(errorStatus));
    } else {
        closesocket(socket);
        return;
    }
    SendResponse(socket, response);
    closesocket(socket);
}

void HttpServer::PruneConnections() {
    std::scoped_lock lock(connectionsMutex_);
    std::erase_if(connections_, [](const std::unique_ptr<Connection>& c) {
        if (!c->done) {
            return false;
        }
        if (c->thread.joinable()) {
            c->thread.join();
        }
        return true;
    });
}

}  // namespace r2r::tools
