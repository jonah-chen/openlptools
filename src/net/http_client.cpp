#include "net/http_client.h"

#include <format>
#include <memory>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <winhttp.h>

namespace {

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string ErrorText(const char* what) {
    const DWORD code = GetLastError();
    switch (code) {
        case ERROR_WINHTTP_CANNOT_CONNECT:
            return "nothing is listening at that address";
        case ERROR_WINHTTP_TIMEOUT:
            return "the server did not answer in time";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return "the host name is unknown";
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return "the connection was closed";
        default:
            return std::format("{} failed (error {})", what, code);
    }
}

struct HandleCloser {
    void operator()(HINTERNET h) const {
        if (h) {
            WinHttpCloseHandle(h);
        }
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;

}  // namespace

std::expected<HttpReply, std::string> HttpRequest(const std::string& method, const std::string& host, uint16_t port,
                                                  const std::string& path, const std::string& body, uint32_t timeoutMs) {
    Handle session(WinHttpOpen(L"raw2raw2/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return std::unexpected(ErrorText("WinHttpOpen"));
    }
    // Connecting to a local port that is closed fails at once; the long timeout is for the reply.
    WinHttpSetTimeouts(session.get(), 5000, 5000, static_cast<int>(timeoutMs), static_cast<int>(timeoutMs));
    Handle connection(WinHttpConnect(session.get(), Widen(host).c_str(), port, 0));
    if (!connection) {
        return std::unexpected(ErrorText("WinHttpConnect"));
    }
    Handle request(WinHttpOpenRequest(connection.get(), Widen(method).c_str(), Widen(path).c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!request) {
        return std::unexpected(ErrorText("WinHttpOpenRequest"));
    }
    const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json; charset=utf-8";
    if (!WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1L),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)) {
        return std::unexpected(ErrorText("send"));
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        return std::unexpected(ErrorText("receive"));
    }
    HttpReply reply;
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    reply.status = static_cast<int>(status);
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available) || available == 0) {
            break;
        }
        const size_t offset = reply.body.size();
        reply.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), reply.body.data() + offset, available, &read)) {
            return std::unexpected(ErrorText("read"));
        }
        reply.body.resize(offset + read);
    }
    return reply;
}

bool ParseAddress(const std::string& text, std::string& host, uint16_t& port) {
    std::string s = text;
    while (!s.empty() && (s.back() == ' ' || s.back() == '/')) {
        s.pop_back();
    }
    while (!s.empty() && s.front() == ' ') {
        s.erase(0, 1);
    }
    if (s.starts_with("http://")) {
        s.erase(0, 7);
    }
    if (s.empty()) {
        return false;
    }
    port = 31000;
    const size_t colon = s.rfind(':');
    if (colon != std::string::npos) {
        try {
            const int p = std::stoi(s.substr(colon + 1));
            if (p <= 0 || p > 65535) {
                return false;
            }
            port = static_cast<uint16_t>(p);
        } catch (...) {
            return false;
        }
        s.resize(colon);
    }
    if (s.empty() || s.find_first_of(" /\\") != std::string::npos) {
        return false;
    }
    host = s;
    return true;
}
