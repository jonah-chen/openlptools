#pragma once

// Widgets and constants shared by the features.

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>

#include "io/dng_loader.h"
#include "imgui.h"

namespace r2r {

inline constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};
inline constexpr ImVec4 kWarningColor{1.0f, 0.75f, 0.4f, 1.0f};

// Device-independent scale of the UI (the display's content scale).
inline float Dpi() {
    return ImGui::GetStyle().FontScaleDpi;
}

inline ImTextureRef TextureRef(SDL_Texture* texture) {
    return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)));
}

void ErrorText(const std::string& text);

// A button with a left-pointing chevron before the label.
bool BackButton(const char* label);

// A label / value row of a two column table (label disabled, value wrapped).
void InfoRow(const std::string& label, const std::string& value);

// Histogram data for the histogram graph.
struct HistogramData {
    std::array<std::array<uint32_t, 256>, 3> counts{};
    uint64_t pixels = 0;
    std::array<uint64_t, 4> clip{};  // black, white, any channel at 0, any at 255

    void Set(const RenderedImage& image);
};

void DrawHistogramGraph(const HistogramData& data);

}  // namespace r2r
