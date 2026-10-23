#pragma once

// A small HTTP/1.1 server for local tools: every connection is served on its own
// thread, so a handler may block for as long as its work takes, and the client
// gets the answer when it is done. Requests are read whole (headers plus a
// Content-Length body); responses close the connection.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace r2r::tools {

struct HttpRequest {
    std::string method;  // "GET", "POST", ...
    std::string path;    // "/export" (no query string handling)
    std::map<std::string, std::string> headers;  // names in lower case
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds 127.0.0.1:port. Returns an error message, or empty on success.
    std::string Start(uint16_t port, Handler handler);

    // Serves connections until Stop() is called, then waits for the handlers
    // still running.
    void Run();
    void Stop();

    uint16_t Port() const { return port_; }

private:
    struct Connection;
    void Serve(Connection& connection);
    void PruneConnections();

    uintptr_t listenSocket_ = ~uintptr_t{0};
    uint16_t port_ = 0;
    Handler handler_;
    std::atomic<bool> stopping_{false};
    std::mutex connectionsMutex_;
    std::vector<std::unique_ptr<Connection>> connections_;
};

// Reason phrase for the status codes the tools use.
const char* HttpStatusText(int status);

}  // namespace r2r::tools
