#include "app/file_dialogs.h"

#include <memory>

namespace r2r {

namespace {

// What SDL calls when a dialog answers, possibly from another thread. It pushes an
// event whose type the userdata carries, with data1 a new'd vector of UTF-8 paths
// (empty if the dialog was cancelled or failed) and data2 the dialog's id.
struct DialogTicket {
    uint32_t eventType;
    uintptr_t id;
};

void SDLCALL DialogCallback(void* userdata, const char* const* paths, int) {
    std::unique_ptr<DialogTicket> ticket(static_cast<DialogTicket*>(userdata));
    auto* list = new std::vector<std::string>();
    for (; paths && *paths; ++paths) {
        list->emplace_back(*paths);
    }
    SDL_Event event{};
    event.type = ticket->eventType;
    event.user.data1 = list;
    event.user.data2 = reinterpret_cast<void*>(ticket->id);
    if (!SDL_PushEvent(&event)) {
        delete list;
    }
}

const char* LocationOrNull(const std::string& location) {
    return location.empty() ? nullptr : location.c_str();
}

FileDialogs::PathsCallback FirstPath(FileDialogs::PathCallback done) {
    return [done = std::move(done)](const std::vector<fs::path>& paths) { done(paths.front()); };
}

}  // namespace

FileDialogs::FileDialogs(SDL_Window* window) : window_(window), eventType_(SDL_RegisterEvents(1)) {}

uintptr_t FileDialogs::Register(PathsCallback done) {
    const uintptr_t id = nextId_++;
    pending_[id] = std::move(done);
    return id;
}

void FileDialogs::PickFile(std::span<const SDL_DialogFileFilter> filters, const std::string& defaultLocation,
                           PathCallback done) {
    auto* ticket = new DialogTicket{eventType_, Register(FirstPath(std::move(done)))};
    SDL_ShowOpenFileDialog(DialogCallback, ticket, window_, filters.data(), static_cast<int>(filters.size()),
                           LocationOrNull(defaultLocation), false);
}

void FileDialogs::PickFiles(std::span<const SDL_DialogFileFilter> filters, const std::string& defaultLocation,
                            PathsCallback done) {
    auto* ticket = new DialogTicket{eventType_, Register(std::move(done))};
    SDL_ShowOpenFileDialog(DialogCallback, ticket, window_, filters.data(), static_cast<int>(filters.size()),
                           LocationOrNull(defaultLocation), true);
}

void FileDialogs::PickFolder(const std::string& defaultLocation, PathCallback done) {
    auto* ticket = new DialogTicket{eventType_, Register(FirstPath(std::move(done)))};
    SDL_ShowOpenFolderDialog(DialogCallback, ticket, window_, LocationOrNull(defaultLocation), false);
}

void FileDialogs::PickSaveFile(std::span<const SDL_DialogFileFilter> filters, const std::string& defaultLocation,
                               PathCallback done) {
    auto* ticket = new DialogTicket{eventType_, Register(FirstPath(std::move(done)))};
    SDL_ShowSaveFileDialog(DialogCallback, ticket, window_, filters.data(), static_cast<int>(filters.size()),
                           LocationOrNull(defaultLocation));
}

bool FileDialogs::HandleEvent(const SDL_Event& event) {
    if (event.type != eventType_) {
        return false;
    }
    std::unique_ptr<std::vector<std::string>> list(static_cast<std::vector<std::string>*>(event.user.data1));
    const auto it = pending_.find(reinterpret_cast<uintptr_t>(event.user.data2));
    if (it == pending_.end()) {
        return true;
    }
    PathsCallback done = std::move(it->second);
    pending_.erase(it);
    if (list && !list->empty()) {
        std::vector<fs::path> paths;
        for (const std::string& path : *list) {
            paths.push_back(PathFromUtf8(path.c_str()));
        }
        done(paths);
    }
    return true;
}

}  // namespace r2r
