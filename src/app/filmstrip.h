#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "common/util.h"
#include "io/dng_loader.h"

namespace r2r {

class App;

// A strip of thumbnails along the bottom of a feature, as in Lightroom. One item is
// active (the one shown large) and any number are selected, for a tool that works on
// several files at once. Thumbnails are rendered on background threads, those near
// the visible part of the strip first.
//
// Selection follows the usual conventions: a click selects one item; Ctrl+click
// toggles an item; Shift+click selects the range from the last plain click;
// Ctrl+Shift+click adds that range. Clicking an item that is already selected only
// makes it the active one and keeps the selection.
class Filmstrip {
public:
    explicit Filmstrip(App& app);
    ~Filmstrip();
    Filmstrip(const Filmstrip&) = delete;
    Filmstrip& operator=(const Filmstrip&) = delete;

    // ---- Contents ---------------------------------------------------------------

    // Replaces the files in the strip (thumbnails of files already present are
    // kept) and makes `active` the only selected item.
    void SetFiles(std::vector<fs::path> files, int active);
    // Takes over another strip's files, selection and active item, sharing the
    // thumbnails it has rendered so far.
    void CopyFrom(const Filmstrip& other);
    void Clear();

    // Drawn inside a popup when an item is right-clicked (an unselected item is
    // selected first); add ImGui::MenuItem calls. No menu without it.
    std::function<void()> contextMenu;

    size_t Count() const { return items_.size(); }
    bool Empty() const { return items_.empty(); }
    const fs::path& Path(size_t index) const { return items_[index].path; }
    std::vector<fs::path> Paths() const;

    // The file's thumbnail texture, or null if it is not in the strip or not rendered
    // yet. aspect receives its width / height.
    SDL_Texture* Thumbnail(const fs::path& path, float* aspect) const;

    // ---- Selection --------------------------------------------------------------

    int Active() const { return active_; }  // -1 while the strip is empty
    const fs::path* ActivePath() const;
    bool IsSelected(size_t index) const { return selected_[index] != 0; }
    size_t SelectedCount() const;
    std::vector<fs::path> SelectedPaths() const;

    // A mouse click on an item with the modifier keys held.
    void Click(int index, bool shift, bool ctrl);
    // Makes the item the active one and the only selected one.
    void Select(int index);
    // Keyboard navigation: moves the active item by `direction` items, or to an
    // index. With extend set, the selection becomes the range from the anchor
    // (the last plain click) to there, as with Shift+click.
    void Step(int direction, bool extend);
    void StepTo(int index, bool extend);
    void SelectAll();
    void DeselectAll();

    // True once after the active item changed (through the strip itself, not
    // SetFiles), for the owner to show the new one.
    bool TakeActiveChanged();

    // ---- Drawing ----------------------------------------------------------------

    bool Visible() const { return visible_; }
    void SetVisible(bool visible);

    // The height the strip takes at the bottom of the window: the strip and its
    // splitter, or a thin bar that shows it again while hidden.
    float Height() const;

    // Draws the strip at the cursor, Height() tall and as wide as the space available.
    void Draw();

    // ---- Background work ----------------------------------------------------------

    // Picks up finished thumbnails. Call every frame from the main thread.
    void Poll();
    bool Busy() const;

    // Stops the thumbnail threads and frees the textures. Called before the DNG SDK
    // and the renderer are torn down.
    void Shutdown();

private:
    struct Item {
        enum class State { Pending, Done, Failed };

        fs::path path;
        std::string name;
        State state = State::Pending;
        std::string error;
        std::shared_ptr<SDL_Texture> texture;  // shared between strips showing the same file
        float aspect = 1.5f;                   // width / height of the thumbnail
    };

    // A thumbnail to render, and one rendered. generation tells results of files
    // that have been replaced meanwhile from current ones.
    struct Job {
        uint64_t generation = 0;
        size_t index = 0;
        fs::path path;
    };
    struct Result {
        uint64_t generation = 0;
        size_t index = 0;
        RenderedImage image;
        std::string error;
    };

    void AssignItems(std::vector<Item> items, int active);
    void StartWorkers();
    void Worker(std::stop_token stop);
    int NearestSelected(int index) const;
    void SetActive(int index);

    void DrawHiddenBar(float width);
    void DrawSplitter(float width);
    void DrawItems();

    App& app_;
    std::vector<Item> items_;
    std::vector<uint8_t> selected_;
    int active_ = -1;
    int anchor_ = -1;  // where a Shift+click range starts
    bool activeChanged_ = false;
    bool scrollToActive_ = false;

    bool visible_ = true;
    float height_;             // of the strip proper, device independent
    float lastStride_ = 0.0f;  // cell pitch at the last draw, to keep the view steady when it changes

    // Thumbnail rendering. jobs_ and results_ are shared with the workers; items_ is
    // only touched on the main thread. hint_ is the index at the middle of the
    // visible part of the strip, which the workers serve first.
    std::vector<std::jthread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::vector<Job> jobs_;
    std::vector<Result> results_;
    uint64_t generation_ = 0;
    std::atomic<size_t> hint_{0};
    std::atomic<int> inFlight_{0};
};

}  // namespace r2r
