#include "app/filmstrip.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

#include "app/app.h"
#include "app/ui.h"
#include "imgui.h"

namespace r2r {

namespace {

constexpr uint32_t kThumbnailSize = 320;  // long side of the rendered thumbnails
constexpr uint32_t kDecodeThreads = 2;    // per thumbnail; several render at once

// Sizes are device independent (multiplied by Dpi() when drawn).
constexpr float kDefaultHeight = 130.0f;
constexpr float kMinHeight = 60.0f;
constexpr float kMaxHeight = 420.0f;
constexpr float kSplitterHeight = 5.0f;
constexpr float kHiddenBarHeight = 12.0f;
constexpr float kCellPadding = 5.0f;
constexpr float kCellGap = 4.0f;
constexpr float kCellAspect = 1.5f;        // width / height of a cell's image area: 3:2 fills it
constexpr float kLabelMinHeight = 100.0f;  // strips shorter than this show no file names

// Lightroom's scheme: selected cells are lighter than the strip, the active one lighter still.
constexpr ImU32 kStripColor = IM_COL32(30, 30, 33, 255);
constexpr ImU32 kHoverColor = IM_COL32(42, 43, 48, 255);
constexpr ImU32 kSelectedColor = IM_COL32(64, 66, 74, 255);
constexpr ImU32 kActiveColor = IM_COL32(104, 106, 116, 255);
constexpr ImU32 kPlaceholderColor = IM_COL32(40, 40, 44, 255);

}  // namespace

Filmstrip::Filmstrip(App& app) : app_(app), height_(kDefaultHeight) {}

Filmstrip::~Filmstrip() {
    Shutdown();
}

// ---- Contents ---------------------------------------------------------------------

void Filmstrip::SetFiles(std::vector<fs::path> files, int active) {
    // Carry over the thumbnails of files that stay in the strip.
    std::unordered_map<fs::path, size_t> oldIndex;
    for (size_t i = 0; i < items_.size(); ++i) {
        oldIndex.emplace(items_[i].path, i);
    }
    std::vector<Item> items;
    items.reserve(files.size());
    for (fs::path& path : files) {
        Item item;
        item.name = Utf8(path.filename());
        item.path = std::move(path);
        if (const auto it = oldIndex.find(item.path); it != oldIndex.end()) {
            const Item& previous = items_[it->second];
            if (previous.state == Item::State::Done) {
                item.state = previous.state;
                item.texture = previous.texture;
                item.aspect = previous.aspect;
            }
        }
        items.push_back(std::move(item));
    }
    AssignItems(std::move(items), active);
}

void Filmstrip::CopyFrom(const Filmstrip& other) {
    std::vector<Item> items;
    items.reserve(other.items_.size());
    for (const Item& item : other.items_) {
        Item copy;
        copy.path = item.path;
        copy.name = item.name;
        if (item.state == Item::State::Done) {
            copy.state = item.state;
            copy.texture = item.texture;
            copy.aspect = item.aspect;
        }
        items.push_back(std::move(copy));
    }
    AssignItems(std::move(items), other.active_);
    selected_ = other.selected_;
    anchor_ = other.anchor_;
}

// Installs the items, selects `active` alone and queues the thumbnails still missing.
void Filmstrip::AssignItems(std::vector<Item> items, int active) {
    items_ = std::move(items);
    selected_.assign(items_.size(), 0);
    active_ = anchor_ = -1;
    activeChanged_ = false;
    if (!items_.empty()) {
        Select(std::clamp(active, 0, static_cast<int>(items_.size()) - 1));
        activeChanged_ = false;  // the owner chose the file; it needs no notice
        scrollToActive_ = true;
        hint_ = static_cast<size_t>(active_);
    }

    // Replace the work queue; results of the old files are dropped as they come in.
    {
        std::scoped_lock lock(mutex_);
        ++generation_;
        jobs_.clear();
        results_.clear();
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].state == Item::State::Pending) {
                jobs_.push_back({generation_, i, items_[i].path});
            }
        }
    }
    if (!jobs_.empty()) {
        StartWorkers();
        wake_.notify_all();
    }
}

void Filmstrip::Clear() {
    SetFiles({}, 0);
}

std::vector<fs::path> Filmstrip::Paths() const {
    std::vector<fs::path> paths;
    paths.reserve(items_.size());
    for (const Item& item : items_) {
        paths.push_back(item.path);
    }
    return paths;
}

SDL_Texture* Filmstrip::Thumbnail(const fs::path& path, float* aspect) const {
    // The active item is the usual question, so try it before searching.
    const Item* match = nullptr;
    if (active_ >= 0 && items_[static_cast<size_t>(active_)].path == path) {
        match = &items_[static_cast<size_t>(active_)];
    } else if (const auto it = std::ranges::find(items_, path, &Item::path); it != items_.end()) {
        match = &*it;
    }
    if (!match || !match->texture) {
        return nullptr;
    }
    *aspect = match->aspect;
    return match->texture.get();
}

// ---- Selection ----------------------------------------------------------------------

const fs::path* Filmstrip::ActivePath() const {
    return active_ >= 0 ? &items_[static_cast<size_t>(active_)].path : nullptr;
}

size_t Filmstrip::SelectedCount() const {
    return static_cast<size_t>(std::ranges::count(selected_, uint8_t{1}));
}

std::vector<fs::path> Filmstrip::SelectedPaths() const {
    std::vector<fs::path> paths;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (selected_[i]) {
            paths.push_back(items_[i].path);
        }
    }
    return paths;
}

void Filmstrip::SetActive(int index) {
    if (index != active_) {
        active_ = index;
        activeChanged_ = true;
    }
}

// The selected item closest to index, or index itself if none is selected.
int Filmstrip::NearestSelected(int index) const {
    int best = index;
    int bestDistance = INT32_MAX;
    for (size_t i = 0; i < items_.size(); ++i) {
        const int distance = std::abs(static_cast<int>(i) - index);
        if (selected_[i] && distance < bestDistance) {
            best = static_cast<int>(i);
            bestDistance = distance;
        }
    }
    return best;
}

void Filmstrip::Click(int index, bool shift, bool ctrl) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }
    if (shift) {
        // A range from the anchor. Shift alone replaces the selection, with Ctrl it adds.
        const int from = anchor_ >= 0 ? anchor_ : (active_ >= 0 ? active_ : index);
        if (!ctrl) {
            std::ranges::fill(selected_, uint8_t{0});
        }
        for (int i = std::min(from, index); i <= std::max(from, index); ++i) {
            selected_[static_cast<size_t>(i)] = 1;
        }
        SetActive(index);
    } else if (ctrl) {
        uint8_t& flag = selected_[static_cast<size_t>(index)];
        flag = !flag;
        if (flag) {
            SetActive(index);
        } else if (active_ == index) {
            // The shown item was deselected: show another selected one if there is one.
            SetActive(NearestSelected(index));
        }
        anchor_ = active_;
    } else {
        // A plain click on a selected item keeps the selection and only changes
        // which of them is shown; on any other item it becomes the one selected.
        if (!selected_[static_cast<size_t>(index)]) {
            std::ranges::fill(selected_, uint8_t{0});
            selected_[static_cast<size_t>(index)] = 1;
        }
        SetActive(index);
        anchor_ = index;
    }
}

void Filmstrip::Select(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }
    std::ranges::fill(selected_, uint8_t{0});
    selected_[static_cast<size_t>(index)] = 1;
    SetActive(index);
    anchor_ = index;
}

void Filmstrip::Step(int direction, bool extend) {
    if (items_.empty()) {
        return;
    }
    StepTo((active_ >= 0 ? active_ : 0) + direction, extend);
}

void Filmstrip::StepTo(int index, bool extend) {
    if (items_.empty()) {
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(items_.size()) - 1);
    if (extend) {
        Click(index, true, false);
    } else {
        Select(index);
    }
    scrollToActive_ = true;
}

void Filmstrip::SelectAll() {
    std::ranges::fill(selected_, uint8_t{1});
}

void Filmstrip::DeselectAll() {
    std::ranges::fill(selected_, uint8_t{0});
}

bool Filmstrip::TakeActiveChanged() {
    return std::exchange(activeChanged_, false);
}

// ---- Background work --------------------------------------------------------------------

void Filmstrip::StartWorkers() {
    if (!workers_.empty()) {
        return;
    }
    // Each thumbnail decodes on a couple of threads; a few decode at once.
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const unsigned count = std::clamp(cores / kDecodeThreads, 1u, 4u);
    for (unsigned i = 0; i < count; ++i) {
        workers_.emplace_back([this](std::stop_token stop) { Worker(stop); });
    }
}

void Filmstrip::Worker(std::stop_token stop) {
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [&] { return !jobs_.empty(); })) {
                return;  // stop requested
            }
            // The job nearest to the visible part of the strip.
            const size_t hint = hint_.load();
            auto best = jobs_.begin();
            size_t bestDistance = SIZE_MAX;
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                const size_t distance = it->index > hint ? it->index - hint : hint - it->index;
                if (distance < bestDistance) {
                    best = it;
                    bestDistance = distance;
                }
            }
            job = std::move(*best);
            jobs_.erase(best);
            ++inFlight_;
        }

        Result result{job.generation, job.index};
        auto doc = DngDocument::Open(job.path, kThumbnailSize, kDecodeThreads);
        if (!doc) {
            result.error = doc.error();
        } else if (auto image = (*doc)->Render(RenderSettings{}, kThumbnailSize)) {
            result.image = std::move(*image);
        } else {
            result.error = image.error();
        }
        doc = {};  // free the decoded image before taking the lock

        std::scoped_lock lock(mutex_);
        results_.push_back(std::move(result));
        --inFlight_;
    }
}

void Filmstrip::Poll() {
    std::vector<Result> results;
    {
        std::scoped_lock lock(mutex_);
        results.swap(results_);
    }
    for (Result& result : results) {
        if (result.generation != generation_ || result.index >= items_.size()) {
            continue;  // for a file no longer in the strip
        }
        Item& item = items_[result.index];
        if (result.error.empty()) {
            SDL_Texture* texture = nullptr;
            if (auto uploaded = app_.UploadTexture(texture, result.image)) {
                item.texture.reset(texture, SDL_DestroyTexture);
                item.aspect = static_cast<float>(result.image.width) / static_cast<float>(result.image.height);
                item.state = Item::State::Done;
                continue;
            } else {
                result.error = uploaded.error();
            }
        }
        item.state = Item::State::Failed;
        item.error = std::move(result.error);
    }
}

bool Filmstrip::Busy() const {
    if (inFlight_ > 0) {
        return true;
    }
    std::scoped_lock lock(mutex_);
    return !jobs_.empty() || !results_.empty();
}

void Filmstrip::Shutdown() {
    for (std::jthread& worker : workers_) {
        worker.request_stop();
    }
    wake_.notify_all();
    workers_.clear();  // joins
    {
        std::scoped_lock lock(mutex_);
        jobs_.clear();
        results_.clear();
    }
    for (Item& item : items_) {
        item.texture.reset();  // destroyed with the last strip that shares it
    }
}

// ---- Drawing --------------------------------------------------------------------------------

void Filmstrip::SetVisible(bool visible) {
    visible_ = visible;
    scrollToActive_ = true;
}

float Filmstrip::Height() const {
    return (visible_ ? kSplitterHeight + height_ : kHiddenBarHeight) * Dpi();
}

void Filmstrip::Draw() {
    const float width = ImGui::GetContentRegionAvail().x;
    if (!visible_) {
        DrawHiddenBar(width);
        return;
    }
    DrawSplitter(width);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kStripColor);
    // The horizontal scrollbar is always there so the layout doesn't jump when the
    // strip grows past the window; the wheel is handled by hand (it scrolls sideways).
    ImGui::BeginChild("##filmstrip", ImVec2(width, height_ * Dpi()), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    DrawItems();
    ImGui::EndChild();
}

// A thin bar with an arrow in the middle; clicking it shows the strip again.
void Filmstrip::DrawHiddenBar(float width) {
    const float dpi = Dpi();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size{width, kHiddenBarHeight * dpi};
    const ImVec2 p1{p0.x + size.x, p0.y + size.y};
    ImGui::InvisibleButton("##filmstrip_show", size);
    const bool hovered = ImGui::IsItemHovered();
    draw->AddRectFilled(p0, p1, hovered ? kHoverColor : kStripColor);
    const ImVec2 center{(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
    const float h = 3.5f * dpi;
    draw->AddTriangleFilled(ImVec2(center.x - 1.4f * h, center.y + h * 0.6f),
                            ImVec2(center.x + 1.4f * h, center.y + h * 0.6f), ImVec2(center.x, center.y - h * 0.6f),
                            hovered ? IM_COL32(230, 230, 235, 255) : IM_COL32(150, 152, 160, 255));
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Show the filmstrip (F6)");
    }
    if (ImGui::IsItemClicked()) {
        SetVisible(true);
    }
}

// The bar above the strip; dragging it changes the strip's height.
void Filmstrip::DrawSplitter(float width) {
    const float dpi = Dpi();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size{width, kSplitterHeight * dpi};
    ImGui::InvisibleButton("##filmstrip_splitter", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hovered || active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (active) {
        const float delta = ImGui::GetIO().MouseDelta.y / dpi;
        height_ = std::clamp(height_ - delta, kMinHeight, kMaxHeight);
    }
    draw->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y),
                        active ? IM_COL32(110, 112, 122, 255) : hovered ? IM_COL32(80, 82, 90, 255) : IM_COL32(44, 45, 50, 255));
}

void Filmstrip::DrawItems() {
    const float dpi = Dpi();
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();  // scrolled
    const ImVec2 avail = ImGui::GetContentRegionAvail();  // above the scrollbar

    if (items_.empty()) {
        const char* text = "The filmstrip shows the open files.";
        const ImVec2 extent = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2((avail.x - extent.x) * 0.5f, (avail.y - extent.y) * 0.5f));
        ImGui::TextDisabled("%s", text);
        return;
    }

    // Uniform cells, so the visible range follows from the scroll position.
    const int count = static_cast<int>(items_.size());
    const float pad = kCellPadding * dpi;
    const float gap = kCellGap * dpi;
    const bool labels = height_ >= kLabelMinHeight;
    const float labelHeight = labels ? fontSize + 2 * dpi : 0.0f;
    const float cellHeight = avail.y - gap;
    const float thumbHeight = std::max(1.0f, cellHeight - 2 * pad - labelHeight);
    const float thumbWidth = thumbHeight * kCellAspect;
    const float cellWidth = thumbWidth + 2 * pad;
    const float stride = cellWidth + gap;
    const float totalWidth = gap + static_cast<float>(count) * stride;
    ImGui::Dummy(ImVec2(totalWidth, cellHeight));  // the scrollable extent

    // The scroll position for this frame: the wheel scrolls sideways; when the cells
    // changed size (the strip was resized) the same frames stay in view, and the
    // active one is kept or brought into view.
    const float viewWidth = avail.x;
    float scrollX = ImGui::GetScrollX();
    if (ImGui::IsWindowHovered() && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)) {
        scrollX -= (io.MouseWheel + io.MouseWheelH) * stride;
    }
    if (lastStride_ > 0.0f && stride != lastStride_) {
        scrollX *= stride / lastStride_;
        scrollToActive_ = true;
    }
    lastStride_ = stride;
    if (scrollToActive_ && active_ >= 0) {
        const float x0 = gap + static_cast<float>(active_) * stride;
        const float x1 = x0 + cellWidth;
        if (x0 < scrollX) {
            scrollX = x0 - gap;
        } else if (x1 > scrollX + viewWidth) {
            scrollX = x1 + gap - viewWidth;
        }
        scrollToActive_ = false;
    }
    scrollX = std::clamp(scrollX, 0.0f, std::max(0.0f, totalWidth - viewWidth));
    if (scrollX != ImGui::GetScrollX()) {
        ImGui::SetScrollX(scrollX);
    }

    const int first = std::clamp(static_cast<int>(std::floor((scrollX - gap) / stride)), 0, count - 1);
    const int last = std::clamp(static_cast<int>(std::ceil((scrollX + viewWidth) / stride)), 0, count - 1);
    hint_ = static_cast<size_t>((first + last) / 2);

    const float cellTop = origin.y + gap * 0.5f;
    for (int i = first; i <= last; ++i) {
        const Item& item = items_[static_cast<size_t>(i)];
        const ImVec2 p0{origin.x + gap + static_cast<float>(i) * stride, cellTop};
        const ImVec2 p1{p0.x + cellWidth, p0.y + cellHeight};

        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##cell", ImVec2(cellWidth, cellHeight));
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            Click(i, io.KeyShift, io.KeyCtrl);
        }
        if (contextMenu && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            if (!selected_[static_cast<size_t>(i)]) {
                Select(i);
            }
            ImGui::OpenPopup("##filmstrip_menu");
        }

        const bool selected = selected_[static_cast<size_t>(i)] != 0;
        const bool active = i == active_;
        const ImU32 background = active ? kActiveColor : selected ? kSelectedColor : hovered ? kHoverColor : 0;
        if (background) {
            draw->AddRectFilled(p0, p1, background, 3.0f * dpi);
        }

        // The thumbnail, fitted into the image area of the cell.
        const ImVec2 a0{p0.x + pad, p0.y + pad};
        const ImVec2 a1{a0.x + thumbWidth, a0.y + thumbHeight};
        if (item.texture) {
            float w = thumbWidth, h = thumbHeight;
            if (item.aspect >= kCellAspect) {
                h = w / item.aspect;
            } else {
                w = h * item.aspect;
            }
            const ImVec2 q0{a0.x + (thumbWidth - w) * 0.5f, a0.y + (thumbHeight - h) * 0.5f};
            draw->AddImage(TextureRef(item.texture.get()), q0, ImVec2(q0.x + w, q0.y + h));
        } else {
            draw->AddRectFilled(a0, a1, kPlaceholderColor, 2.0f * dpi);
            if (item.state == Item::State::Failed) {
                const char* mark = "!";
                const ImVec2 extent = font->CalcTextSizeA(fontSize * 1.5f, FLT_MAX, 0.0f, mark);
                draw->AddText(font, fontSize * 1.5f,
                              ImVec2((a0.x + a1.x - extent.x) * 0.5f, (a0.y + a1.y - extent.y) * 0.5f),
                              ImGui::ColorConvertFloat4ToU32(kErrorColor), mark);
            }
        }

        // Frame number in the corner, with a shadow so it reads on any image.
        const std::string number = std::to_string(i + 1);
        const ImVec2 numberPos{a0.x + 3 * dpi, a0.y + 2 * dpi};
        draw->AddText(ImVec2(numberPos.x + dpi, numberPos.y + dpi), IM_COL32(0, 0, 0, 160), number.c_str());
        draw->AddText(numberPos, IM_COL32(225, 225, 230, 220), number.c_str());

        if (labels) {
            const ImVec2 extent = ImGui::CalcTextSize(item.name.c_str());
            const float x = std::max(p0.x + 2 * dpi, (p0.x + p1.x - extent.x) * 0.5f);
            draw->PushClipRect(ImVec2(p0.x + 2 * dpi, p0.y), ImVec2(p1.x - 2 * dpi, p1.y), true);
            draw->AddText(ImVec2(x, a1.y + 1 * dpi),
                          active ? IM_COL32(240, 240, 245, 255) : IM_COL32(170, 172, 180, 255), item.name.c_str());
            draw->PopClipRect();
        }

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            if (item.state == Item::State::Failed) {
                ImGui::SetTooltip("%s\n%s", item.name.c_str(), item.error.c_str());
            } else {
                ImGui::SetTooltip("%s", item.name.c_str());
            }
        }
    }

    if (contextMenu && ImGui::BeginPopup("##filmstrip_menu")) {
        contextMenu();
        ImGui::EndPopup();
    }
}

}  // namespace r2r
