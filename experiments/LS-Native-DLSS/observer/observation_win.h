#pragma once
#include <windows.h>
#include <filesystem>
#include <cstring>

template<class Function> Function NativeExport(HMODULE module, const char* name) {
    const FARPROC address = GetProcAddress(module, name);
    static_assert(sizeof(Function) == sizeof(address), "Windows function pointer ABI");
    Function function = nullptr;
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

// Called only from an exported application call, never from DllMain.
// Failures leave all native LS calls running. This stage cannot enable DLSS.
void StartNativeObservation(HMODULE native, HMODULE proxy,
                            const std::filesystem::path& folder) noexcept;
