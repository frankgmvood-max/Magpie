#pragma once
#include <Windows.h>
#include <string>

namespace Magpie {
// Process-local experiment selection. The launchers set this before Magpie
// starts; self-restarts retain it through a sidecar selector. No saved profile is modified.
inline unsigned VrrExperimentMode() noexcept {
    static const unsigned mode = []() noexcept -> unsigned {
        wchar_t value[4]{};
        const DWORD length = GetEnvironmentVariableW(L"MAGPIE_VRR_EXPERIMENT", value, 4);
        if (length == 1 && value[0] >= L'0' && value[0] <= L'6')
            return static_cast<unsigned>(value[0] - L'0');
        // ShellExecute/UAC restarts may not inherit the caller's environment.
        // Launchers also write a small selector beside the executable.
        wchar_t executable[MAX_PATH]{};
        const DWORD pathLength = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        if (!pathLength || pathLength >= MAX_PATH) return 0u;
        std::wstring path(executable, pathLength);
        const auto slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return 0u;
        path.resize(slash + 1);
        path += L"vrr-experiment-mode.txt";
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 0u;
        char selected = '0';
        DWORD bytes = 0;
        const BOOL read = ReadFile(file, &selected, 1, &bytes, nullptr);
        CloseHandle(file);
        return read && bytes == 1 && selected >= '0' && selected <= '6'
            ? static_cast<unsigned>(selected - '0') : 0u;
    }();
    return mode;
}
inline bool VrrExperimentOpaque() noexcept {
    return VrrExperimentMode() == 2 || VrrExperimentMode() == 3 || VrrExperimentMode() == 6;
}
inline bool VrrExperimentForeground() noexcept {
    return VrrExperimentMode() == 3 || VrrExperimentMode() == 4;
}
inline bool VrrExperimentNativeMouse() noexcept {
    return VrrExperimentMode() == 6;
}
inline bool VrrExperimentSpectator() noexcept {
    return VrrExperimentMode() == 2 || VrrExperimentMode() == 3 || VrrExperimentMode() == 4;
}
}
