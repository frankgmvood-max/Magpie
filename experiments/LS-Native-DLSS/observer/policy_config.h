#pragma once
#include "../backend/native_backend.h"
#include <algorithm>
#include <cwchar>
#include <string>

namespace ls_native {
struct PolicyConfig {
    RenderPolicy policy;
    BackendOptions backend;
    std::wstring profile;
};
inline PolicyConfig ReadPolicyConfig(const std::filesystem::path& ini) {
    PolicyConfig c;
    wchar_t name[128]{};
    GetPrivateProfileStringW(L"NativeDLSS", L"ActiveProfile", L"", name, 128, ini.c_str());
    c.profile = name;
    const std::wstring section = c.profile.empty() ? L"NativeDLSS" : L"Profile:" + c.profile;
    const auto value = [&](const wchar_t* key, int fallback) {
        const auto global = GetPrivateProfileIntW(L"NativeDLSS", key, fallback, ini.c_str());
        return GetPrivateProfileIntW(section.c_str(), key, global, ini.c_str());
    };
    const uint32_t mode = value(L"Mode", 2);
    c.policy.mode = mode <= 2 ? static_cast<RenderMode>(mode) : RenderMode::Hybrid;
    c.policy.duplicate_filter = value(L"DuplicateFilter", 1) == 1;
    c.policy.scene_cut = value(L"SceneCut", 1) == 1;
    c.policy.duplicate_delta = std::min(value(L"DuplicateDelta", 0), 8u);
    c.policy.cut_delta = std::clamp(value(L"CutDelta", 64), 1u, 255u);
    c.policy.cut_percent = std::clamp(value(L"CutPercent", 65), 1u, 100u);
    for (size_t i = 0; i != c.policy.hud.size(); ++i) {
        const std::wstring key = L"HUD" + std::to_wstring(i + 1);
        wchar_t global[128]{}, text[128]{};
        GetPrivateProfileStringW(L"NativeDLSS", key.c_str(), L"", global, 128, ini.c_str());
        GetPrivateProfileStringW(section.c_str(), key.c_str(), global, text, 128, ini.c_str());
        unsigned a = 0, b = 0, d = 0, e = 0; wchar_t extra = 0;
#ifdef _MSC_VER
        const int count = swscanf_s(text, L"%u,%u,%u,%u %c", &a, &b, &d, &e, &extra, 1u);
#else
        const int count = std::swscanf(text, L"%u,%u,%u,%u %lc", &a, &b, &d, &e, &extra);
#endif
        if (count == 4 && a < d && b < e && d <= 10000 && e <= 10000) c.policy.hud[i] = {a,b,d,e};
    }
    c.backend.enabled = value(L"Enabled", 1) == 1;
    c.backend.optical_flow = value(L"OpticalFlow", 1) == 1;
    c.backend.gpu_ordered = value(L"GPUOrdered", 1) == 1;
    c.backend.quality = std::clamp(value(L"Quality", 2), 1u, 5u);
    c.backend.flow_preset = std::min(value(L"FlowPreset", 0), 3u);
    const auto grid = value(L"FlowGrid", 0);
    c.backend.flow_grid = grid == 2 ? 2u : grid ? 4u : 0u;
    c.backend.analysis_percent = std::clamp(value(L"AnalysisPercent", 50), 10u, 100u);
    c.backend.slots = std::clamp(value(L"Slots", 3), 2u, 4u);
    return c;
}
}
