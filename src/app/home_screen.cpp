// The home screen: every feature as a card in a grid that wraps to the window width
// and scrolls when there are more cards than fit.

#include <algorithm>
#include <cmath>

#include "app/app.h"
#include "app/ui.h"
#include "imgui.h"

namespace r2r {

namespace {

constexpr float kHomeMargin = 28.0f;
constexpr float kCardMinWidth = 280.0f;
constexpr float kCardMaxWidth = 380.0f;
constexpr float kCardHeight = 150.0f;
constexpr float kCardGap = 16.0f;
constexpr float kCardPadding = 16.0f;
constexpr float kIconSize = 44.0f;

// A stable accent color per feature, derived from its name.
ImU32 AccentColor(const std::string& name) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : name) {
        hash = (hash ^ c) * 16777619u;
    }
    float r = 0, g = 0, b = 0;
    ImGui::ColorConvertHSVtoRGB(static_cast<float>(hash % 360) / 360.0f, 0.45f, 0.78f, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

}  // namespace

void App::DrawHome() {
    const float dpi = Dpi();
    ImGui::BeginChild("##home", ImVec2(0, 0), ImGuiChildFlags_None);

    const float margin = kHomeMargin * dpi;
    const float gap = kCardGap * dpi;
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float minWidth = kCardMinWidth * dpi;
    const int columns = std::max(1, static_cast<int>((availWidth - 2 * margin + gap) / (minWidth + gap)));
    const float cardWidth = std::min(kCardMaxWidth * dpi, std::floor((availWidth - 2 * margin - gap * (columns - 1)) / columns));
    const float cardHeight = kCardHeight * dpi;

    // Header.
    ImGui::SetCursorPos(ImVec2(margin, margin));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.7f);
    ImGui::TextUnformatted("raw2raw2");
    ImGui::PopFont();
    ImGui::SetCursorPosX(margin);
    ImGui::TextDisabled("Choose a tool, or drop a file or folder on the window to open it.");
    ImGui::Dummy(ImVec2(0, margin * 0.5f));

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 start{origin.x + margin, origin.y};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float pad = kCardPadding * dpi;
    const float icon = kIconSize * dpi;

    for (size_t i = 0; i < features_.size(); ++i) {
        Feature& feature = *features_[i];
        const FeatureInfo& info = feature.Info();
        const int row = static_cast<int>(i) / columns;
        const int col = static_cast<int>(i) % columns;
        const ImVec2 p0{start.x + static_cast<float>(col) * (cardWidth + gap),
                        start.y + static_cast<float>(row) * (cardHeight + gap)};
        const ImVec2 p1{p0.x + cardWidth, p0.y + cardHeight};

        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(static_cast<int>(i));
        const bool clicked = ImGui::InvisibleButton("##card", ImVec2(cardWidth, cardHeight));
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        ImGui::PopID();
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        const ImU32 accent = AccentColor(info.name);
        const ImU32 background = held      ? IM_COL32(52, 54, 62, 255)
                                 : hovered ? IM_COL32(44, 46, 54, 255)
                                           : IM_COL32(34, 35, 41, 255);
        const ImU32 border = hovered ? accent : IM_COL32(60, 62, 70, 255);
        const float rounding = 8.0f * dpi;
        draw->AddRectFilled(p0, p1, background, rounding);
        draw->AddRect(p0, p1, border, rounding, 0, hovered ? 2.0f : 1.0f);

        // Monogram badge.
        const ImVec2 i0{p0.x + pad, p0.y + pad};
        const ImVec2 i1{i0.x + icon, i0.y + icon};
        draw->AddRectFilled(i0, i1, accent, 8.0f * dpi);
        const std::string letter = info.name.empty() ? "?" : info.name.substr(0, 1);
        const float letterSize = fontSize * 1.6f;
        const ImVec2 letterExtent = font->CalcTextSizeA(letterSize, FLT_MAX, 0.0f, letter.c_str());
        draw->AddText(font, letterSize,
                      ImVec2(i0.x + (icon - letterExtent.x) / 2, i0.y + (icon - letterExtent.y) / 2),
                      IM_COL32(20, 20, 24, 255), letter.c_str());

        // Title, next to the badge.
        const float titleSize = fontSize * 1.2f;
        const ImVec2 titleExtent = font->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, info.name.c_str());
        const float textX = i1.x + pad * 0.8f;
        draw->PushClipRect(ImVec2(p0.x, p0.y), ImVec2(p1.x - pad * 0.5f, p1.y), true);
        draw->AddText(font, titleSize, ImVec2(textX, i0.y + (icon - titleExtent.y) / 2), IM_COL32(235, 235, 240, 255),
                      info.name.c_str());

        // Description, wrapped, below.
        draw->AddText(font, fontSize, ImVec2(p0.x + pad, i1.y + pad * 0.8f), IM_COL32(160, 162, 172, 255),
                      info.description.c_str(), nullptr, cardWidth - 2 * pad);

        if (feature.Busy()) {
            const char* busy = "Working...";
            const ImVec2 extent = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, busy);
            draw->AddText(font, fontSize, ImVec2(p1.x - pad - extent.x, p1.y - pad - extent.y), accent, busy);
        }
        draw->PopClipRect();

        if (clicked) {
            Activate(&feature);
        }
    }

    // Extend the scrollable area below the last row.
    const int rows = (static_cast<int>(features_.size()) + columns - 1) / columns;
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + static_cast<float>(rows) * (cardHeight + gap)));
    ImGui::Dummy(ImVec2(0, margin));

    ImGui::EndChild();
}

}  // namespace r2r
