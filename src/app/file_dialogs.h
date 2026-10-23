#pragma once

// Native file dialogs. SDL shows them without blocking and reports the choice from
// another thread; this brings the result back to the main thread as an SDL event and
// runs the caller's callback there.

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/util.h"

namespace r2r {

class FileDialogs {
public:
    explicit FileDialogs(SDL_Window* window);
    FileDialogs(const FileDialogs&) = delete;
    FileDialogs& operator=(const FileDialogs&) = delete;

    // The callback runs on the main thread with the chosen path(s); it is not called
    // if the dialog is cancelled. The filters must outlive the dialog (use static
    // arrays). defaultLocation may be empty.
    using PathCallback = std::function<void(const fs::path&)>;
    using PathsCallback = std::function<void(const std::vector<fs::path>&)>;
    void PickFile(std::span<const SDL_DialogFileFilter> filters, const std::string& defaultLocation, PathCallback done);
    void PickFiles(std::span<const SDL_DialogFileFilter> filters, const std::string& defaultLocation,
                   PathsCallback done);
    void PickFolder(const std::string& defaultLocation, PathCallback done);
    void PickSaveFile(std::span<const SDL_DialogFileFilter> filters, const std::string& defaultLocation,
                      PathCallback done);

    // Runs the callback of a dialog that has answered. Returns false if the event
    // is not a dialog's.
    bool HandleEvent(const SDL_Event& event);

    // Forgets the dialogs still open, so their callbacks never run (at shutdown,
    // before what they would touch goes away).
    void Clear() { pending_.clear(); }

private:
    uintptr_t Register(PathsCallback done);

    SDL_Window* window_;
    uint32_t eventType_ = 0;
    uintptr_t nextId_ = 1;
    std::unordered_map<uintptr_t, PathsCallback> pending_;
};

}  // namespace r2r
