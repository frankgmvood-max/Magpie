#pragma once
#include <array>
#include <cstdint>

namespace ls_native {
enum class RenderMode : uint32_t { Native = 0, Hybrid = 1, Economy = 2 };
struct HudRect { uint32_t left = 0, top = 0, right = 0, bottom = 0; };
// Coordinates are ten-thousandths of the captured image, not screen pixels.
struct RenderPolicy {
    RenderMode mode = RenderMode::Hybrid;
    bool duplicate_filter = false, scene_cut = false;
    uint32_t duplicate_delta = 0, cut_delta = 64, cut_percent = 65;
    std::array<HudRect, 8> hud{};
};
inline const char* RenderModeName(RenderMode mode) {
    switch (mode) {
    case RenderMode::Economy: return "dlss_economy";
    case RenderMode::Hybrid: return "dlss_hybrid";
    default: return "ls_native";
    }
}
}
