#pragma once
#include "render_policy.h"

namespace ls_native {
// Fixed-width C bridge; UI has no access to devices, threads or GPU objects.
struct UiSettings {
    uint32_t version = 1, size = 176, type = 0, preset = 2, grid = 4;
    uint32_t analysis = 50, slots = 3, duplicate = 1, scene = 1;
    uint32_t duplicate_delta = 0, cut_delta = 64, cut_percent = 65;
    std::array<HudRect, 8> hud{};
};
static_assert(sizeof(UiSettings) == 176, "Managed/native UI ABI");
inline bool ValidUiSettings(const UiSettings& s) {
    if (s.version != 1 || s.size != sizeof(s) || s.type > 6 || s.preset < 1 || s.preset > 3 ||
        (s.grid != 2 && s.grid != 4) || s.analysis < 10 || s.analysis > 100 || s.slots < 2 || s.slots > 4 ||
        s.duplicate > 1 || s.scene > 1 || s.duplicate_delta > 8 || s.cut_delta < 1 || s.cut_delta > 255 || s.cut_percent < 1 || s.cut_percent > 100) return false;
    for (const auto& r : s.hud) {
        if (!r.left && !r.top && !r.right && !r.bottom) continue;
        if (r.left >= r.right || r.top >= r.bottom || r.right > 10000 || r.bottom > 10000) return false;
    }
    return true;
}
inline RenderPolicy UiPolicy(const UiSettings& s) {
    RenderPolicy p;
    p.mode = s.type == 6 ? RenderMode::Economy : RenderMode::Native;
    p.duplicate_filter = s.duplicate == 1; p.scene_cut = s.scene == 1;
    p.duplicate_delta = s.duplicate_delta; p.cut_delta = s.cut_delta; p.cut_percent = s.cut_percent; p.hud = s.hud;
    return p;
}
}
