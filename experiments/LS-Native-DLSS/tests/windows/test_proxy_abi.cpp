#include "../../observer/observation_win.h"
#include <cstdint>
#include <iostream>

// This prototype is deliberately independent of proxy.cpp's forwarding macros.
using Apply = void(__fastcall*)(int,int,int,int,float,uint8_t,int,uint8_t,int,int,int,
    float,float,int,uint8_t,uint8_t,uint8_t,uint8_t,int,int,uint8_t,uint8_t,int,int,
    uint8_t,int,int,int,int,int,int,uint8_t);
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto folder = std::filesystem::absolute(argv[1]);
    const auto path = folder / L"Lossless.dll";
    HMODULE proxy = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!proxy) { std::cerr << "proxy load error: " << GetLastError() << '\n'; return 1; }
    const char* names[] = {"Activate", "GetAdapterNames", "GetDisplayNames", "GetDwmRefreshRate", "GetForegroundWindowEx",
        "Init", "IsWindowsBuildAtLeast", "SetDriverSettings", "SetWindowsSettings", "UnInit"};
    const int results[] = {1,3,4,5,6,7,8,9,10,11};
    for (size_t i = 0; i < 10; ++i) {
        const auto fn = NativeExport<int(*)()>(proxy, names[i]);
        if (!fn || fn() != results[i]) { std::cerr << "forwarding failed: " << names[i] << '\n'; return 1; }
    }
    const auto apply = NativeExport<Apply>(proxy, "ApplySettings");
    if (!apply) return 1;
    apply(101,102,103,104,1.25f,201,107,202,109,110,111,2.5f,3.75f,114,203,204,205,206,
          119,120,207,208,123,124,209,126,127,128,129,130,131,210);
    const auto native = GetModuleHandleW(L"Lossless_original.dll");
    const auto check = native ? NativeExport<int(*)()>(native, "TestSettingsApplied") : nullptr;
    if (!check || check() != 1) { std::cerr << "32 settings arguments were not preserved\n"; return 1; }
    FreeLibrary(proxy);
    std::cout << "10 PE forwarders and all 32 settings arguments passed\n";
}
