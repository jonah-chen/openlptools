#include "app/ui.h"

#include <algorithm>
#include <cmath>

namespace r2r {

namespace {

constexpr float kHistogramHeight = 120.0f;

}  // namespace

void ErrorText(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

bool BackButton(const char* label) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float font = ImGui::GetFontSize();
    const float arrowWidth = std::round(font * 0.25f);
    const float arrowHalfHeight = std::round(font * 0.3f);
    const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 size{text.x + arrowWidth + style.ItemInnerSpacing.x + 2 * style.FramePadding.x,
                      ImGui::GetFrameHeight()};
    const ImVec2 p0 = ImGui::GetCursorScreenPos();

    // The label sits at the right of the padding; the chevron fills the room left of it.
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(1.0f, 0.5f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar();

    const float x = p0.x + style.FramePadding.x;
    const float y = p0.y + size.y * 0.5f;
    const ImVec2 points[3] = {{x + arrowWidth, y - arrowHalfHeight}, {x, y}, {x + arrowWidth, y + arrowHalfHeight}};
    ImGui::GetWindowDrawList()->AddPolyline(points, 3, ImGui::GetColorU32(ImGuiCol_Text), ImDrawFlags_None,
                                            std::max(1.0f, font / 9.0f));
    return pressed;
}

void InfoRow(const std::string& label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label.c_str());
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

void HistogramData::Set(const RenderedImage& image) {
    counts = image.histogram;
    pixels = static_cast<uint64_t>(image.width) * image.height;
    clip = {image.blackPixels, image.whitePixels, image.anyChannelAt0, image.anyChannelAt255};
}

void DrawHistogramGraph(const HistogramData& data) {
    const float dpi = Dpi();
    const ImVec2 size{ImGui::GetContentRegionAvail().x, kHistogramHeight * dpi};
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1{p0.x + size.x, p0.y + size.y};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p0, p1, IM_COL32(18, 18, 20, 255), 3.0f * dpi);
    ImGui::Dummy(size);
    if (data.pixels == 0) {
        return;
    }

    // Scale to the tallest bin, leaving out the end bins where clipped pixels
    // pile up, so a blown sky doesn't flatten the rest of the graph.
    uint32_t peak = 1;
    for (const auto& channel : data.counts) {
        peak = std::max(peak, *std::max_element(channel.begin() + 1, channel.end() - 1));
    }

    // Channels overlap additively, as in Lightroom: where all three cover a
    // column it is gray, two give yellow/cyan/magenta, one its own color.
    // Indexed by a bitmask of channels (1 = R, 2 = G, 4 = B).
    static const ImU32 kColors[8] = {
        0,
        IM_COL32(205, 60, 60, 255),    // R
        IM_COL32(70, 185, 70, 255),    // G
        IM_COL32(205, 200, 70, 255),   // R+G
        IM_COL32(70, 105, 225, 255),   // B
        IM_COL32(190, 80, 200, 255),   // R+B
        IM_COL32(70, 190, 205, 255),   // G+B
        IM_COL32(175, 175, 175, 255),  // R+G+B
    };
    const int columns = std::max(1, static_cast<int>(size.x));
    for (int col = 0; col < columns; ++col) {
        const int first = col * 256 / columns;
        const int last = std::max(first + 1, (col + 1) * 256 / columns);
        std::array<float, 3> heights{};
        for (int c = 0; c < 3; ++c) {
            const uint32_t count = *std::max_element(data.counts[c].begin() + first, data.counts[c].begin() + last);
            heights[c] = std::min(1.0f, static_cast<float>(count) / static_cast<float>(peak)) * size.y;
        }
        // Draw from the bottom up: the band all channels share, then the band
        // the two tallest share, then the tallest alone.
        std::array<int, 3> order{0, 1, 2};
        std::ranges::sort(order, [&](int a, int b) { return heights[a] > heights[b]; });
        const float x0 = p0.x + static_cast<float>(col);
        const float x1 = x0 + 1.0f;
        float bottom = 0.0f;
        int mask = 7;
        for (int band = 2; band >= 0; --band) {
            const float top = heights[order[band]];
            if (top > bottom) {
                draw->AddRectFilled(ImVec2(x0, p1.y - top), ImVec2(x1, p1.y - bottom), kColors[mask]);
                bottom = top;
            }
            mask &= ~(1 << order[band]);
        }
    }

    const auto percent = [&](uint64_t count) { return 100.0 * count / static_cast<double>(data.pixels); };
    ImGui::TextDisabled("Black %.2f%%   White %.2f%%", percent(data.clip[0]), percent(data.clip[1]));
    ImGui::TextDisabled("A channel at 0: %.2f%%   at 255: %.2f%%", percent(data.clip[2]), percent(data.clip[3]));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip(
            "Pixels with one or two channels at the limit. Besides over- or underexposure,\n"
            "this happens for colors more saturated than sRGB can show.");
    }
}

}  // namespace r2r
