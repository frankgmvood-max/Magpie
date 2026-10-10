// An independently declared ABI fixture, not an NVIDIA/LS binary.
#include <windows.h>
#include <cstdint>
#include <cstring>
namespace { bool applied = false, common = false; int fg_type = 0, fg_mode = 0, fg_hdr = 0; float fg_multiplier = 0; }
extern "C" __declspec(dllexport) void __fastcall ApplySettings(
    int a, int b, int c, int d, float e, uint8_t f, int h, uint8_t i,
    int j, int k, int l, float m, float n, int o, uint8_t p, uint8_t q,
    uint8_t r, uint8_t s, int t, int u, uint8_t v, uint8_t w, int x, int y,
    uint8_t z, int aa, int ab, int ac, int ad, int ae, int af, uint8_t ag) {
    fg_type = j; fg_mode = l; fg_multiplier = m; fg_hdr = w;
    common = a == 101 && b == 102 && c == 103 && d == 104 && e == 1.25f && f == 201 &&
        h == 107 && i == 202 && k == 110 && n == 3.75f && o == 114 && p == 203 && q == 204 &&
        r == 205 && s == 206 && t == 119 && u == 120 && v == 207 && x == 123 && y == 124 &&
        z == 209 && aa == 126 && ab == 127 && ac == 128 && ad == 129 && ae == 130 && af == 131 && ag == 210;
    applied = a == 101 && b == 102 && c == 103 && d == 104 && e == 1.25f && f == 201 &&
        h == 107 && i == 202 && j == 109 && k == 110 && l == 111 && m == 2.5f && n == 3.75f &&
        o == 114 && p == 203 && q == 204 && r == 205 && s == 206 && t == 119 && u == 120 &&
        v == 207 && w == 208 && x == 123 && y == 124 && z == 209 && aa == 126 && ab == 127 &&
        ac == 128 && ad == 129 && ae == 130 && af == 131 && ag == 210;
}
extern "C" __declspec(dllexport) int TestSettingsApplied() { return applied ? 1 : 0; }
extern "C" __declspec(dllexport) int TestUiSettingsApplied(int type, int mode, float multiplier, int hdr) {
    return common && fg_type == type && fg_mode == mode && fg_multiplier == multiplier && fg_hdr == hdr ? 1 : 0;
}
#define EXPORT(name, result) extern "C" __declspec(dllexport) int name() { return result; }
EXPORT(Activate, 1)
EXPORT(GetAdapterNames, 3)
EXPORT(GetDisplayNames, 4)
EXPORT(GetDwmRefreshRate, 5)
EXPORT(GetForegroundWindowEx, 6)
EXPORT(Init, 7)
EXPORT(IsWindowsBuildAtLeast, 8)
EXPORT(SetDriverSettings, 9)
EXPORT(SetWindowsSettings, 10)
EXPORT(UnInit, 11)
