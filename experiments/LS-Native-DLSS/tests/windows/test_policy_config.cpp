#include "../../observer/policy_config.h"
#include <iostream>
#include <stdexcept>
using namespace ls_native;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    wchar_t folder[MAX_PATH]{}, file[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, folder) || !GetTempFileNameW(folder, L"LSP", 0, file)) return 1;
    try {
        const auto write = [&](const wchar_t* section, const wchar_t* key, const wchar_t* value) {
            Require(WritePrivateProfileStringW(section, key, value, file) != 0, "write fixture");
        };
        write(L"NativeDLSS",L"Quality",L"5");
        write(L"NativeDLSS",L"HUD1",L"0,0,5000,10000");
        write(L"NativeDLSS",L"HUD2",L"0,0,10001,10000");
        auto c = ReadPolicyConfig(file);
        Require(c.policy.mode == RenderMode::Economy && c.backend.quality == 5 && c.backend.flow_preset == 0,
            "legacy quality retained without inventing a new preset");
        Require(c.policy.hud[0].right == 5000 && c.policy.hud[1].right == 0, "HUD bounds");
        write(L"NativeDLSS",L"ActiveProfile",L"WoW.exe");
        write(L"Profile:WoW.exe",L"Mode",L"1");
        write(L"Profile:WoW.exe",L"FlowPreset",L"1");
        write(L"Profile:WoW.exe",L"AnalysisPercent",L"25");
        write(L"Profile:WoW.exe",L"HUD1",L"");
        write(L"Profile:WoW.exe",L"HUD3",L"1,2,3,4 trailing");
        write(L"Profile:WoW.exe",L"CutPercent",L"1000");
        c = ReadPolicyConfig(file);
        Require(c.profile == L"WoW.exe" && c.policy.mode == RenderMode::Hybrid && c.backend.flow_preset == 1 &&
            c.backend.analysis_percent == 25 && c.backend.quality == 5, "profile override and global inheritance");
        Require(c.policy.hud[0].right == 0 && c.policy.hud[2].right == 0 && c.policy.cut_percent == 100,
            "explicit empty HUD, malformed input and bounded threshold");
        write(L"Profile:WoW.exe",L"FlowGrid",L"2");
        write(L"Profile:WoW.exe",L"Slots",L"99");
        c = ReadPolicyConfig(file);
        Require(c.backend.flow_grid == 2 && c.backend.slots == 4, "independent flow grid and bounded ring");
        write(L"NativeDLSS",L"GPUOrdered",L"0");
        write(L"Diagnostics",L"Level",L"1"); write(L"Diagnostics",L"IntervalMs",L"1");
        write(L"Diagnostics",L"FrameEvery",L"0"); write(L"Diagnostics",L"MaxLogMiB",L"999");
        write(L"Diagnostics",L"KeepLogFiles",L"99");
        c = ReadPolicyConfig(file);
        UiSettings ui; ui.type = 6; ui.analysis = 75; ui.slots = 2; ui.preset = 3;
        const auto native_ui = UiBackendOptions(c.backend, ui);
        Require(!native_ui.gpu_ordered && native_ui.optical_flow && native_ui.analysis_percent == 75 &&
            native_ui.slots == 2 && native_ui.flow_preset == 3 && native_ui.diagnostics_level == 1,
            "native UI preserves INI ordering/diagnostics while applying selected quality");
        Require(c.diagnostics.interval_ms == 100 && c.diagnostics.frame_every == 1 &&
            c.diagnostics.max_log_mib == 128 && c.diagnostics.keep_log_files == 8, "bounded diagnostics config");
        DeleteFileW(file);
        std::cout << "Legacy migration, application profiles, HUD validation and independent quality controls passed\n";
        return 0;
    } catch (const std::exception& error) {
        DeleteFileW(file); std::cerr << error.what() << '\n'; return 1;
    }
}
