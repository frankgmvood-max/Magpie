#pragma once
#include <Windows.h>

namespace Magpie {
// Process-local experiment selection. The launchers set this before Magpie
// starts; self-restarts inherit it. No registry or saved profile is modified.
inline unsigned VrrExperimentMode() noexcept {
    static const unsigned mode = []() noexcept -> unsigned {
        wchar_t value[4]{};
        const DWORD length = GetEnvironmentVariableW(L"MAGPIE_VRR_EXPERIMENT", value, 4);
        return length == 1 && value[0] >= L'1' && value[0] <= L'5'
            ? static_cast<unsigned>(value[0] - L'0') : 0u;
    }();
    return mode;
}
inline bool VrrExperimentOpaque() noexcept {
    return VrrExperimentMode() == 2 || VrrExperimentMode() == 3;
}
inline bool VrrExperimentForeground() noexcept {
    return VrrExperimentMode() == 3 || VrrExperimentMode() == 4;
}
inline bool VrrExperimentSpectator() noexcept {
    return VrrExperimentOpaque() || VrrExperimentForeground();
}
}
