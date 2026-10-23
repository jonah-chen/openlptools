#pragma once

// Helpers shared by the engines built on the DNG SDK (dng_loader, raw_stack):
// error translation, path text and the multithreaded host. Not for the UI.

#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <new>
#include <string>

#include "dng_exceptions.h"

class dng_host;

std::string DngErrorMessage(dng_error_code code);

// A dng_host that runs area tasks (demosaic, color conversion, ...) on up to
// `threads` threads (0 = one per core). The caller owns the result.
dng_host* NewThreadedHost(uint32_t threads);

inline std::string PathUtf8(const std::filesystem::path& path) {
    const std::u8string s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

// Runs f, turning the exceptions the DNG SDK and the standard library throw into
// an error message.
template <typename F>
auto CatchErrors(F&& f) -> std::expected<decltype(f()), std::string> {
    try {
        return f();
    } catch (const dng_exception& e) {
        return std::unexpected(DngErrorMessage(e.ErrorCode()));
    } catch (const std::bad_alloc&) {
        return std::unexpected(std::string("out of memory"));
    } catch (const std::exception& e) {
        return std::unexpected(std::string(e.what()));
    }
}
