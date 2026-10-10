#pragma once
#include <array>
#include <cstdint>

namespace ls_native {
// Derived from actual REA 6.3.0 / Ghidra 12.1.4 responses. The observer also
// gates the complete DLL hash and exact shader bytes. No native CPU ABI patch.
constexpr std::array<uint32_t, 25> kAnalysisReturns{{
    0x20484, 0x20670, 0x20816, 0x20a46, 0x20d09,
    0x21318, 0x2149e, 0x21619, 0x21785, 0x218c1,
    0x21f24, 0x22148, 0x223b3, 0x2261e, 0x22898,
    0x22d2a, 0x22f8c, 0x2323b, 0x234ea, 0x2379e,
    0x23c7a, 0x23e8e, 0x24063, 0x24238, 0x2441b
}};
constexpr uint32_t kSourceReturn = 0x20399, kSynthesisReturn = 0x245d3;
inline bool IsAnalysisReturn(uint32_t rva) {
    for (const auto value : kAnalysisReturns) if (rva == value) return true;
    return false;
}
inline bool IsAnalysisResource(uint32_t id) { return id == 255 || (id >= 257 && id <= 302); }
}
