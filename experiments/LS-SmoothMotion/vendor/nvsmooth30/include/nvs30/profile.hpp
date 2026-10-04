// SPDX-License-Identifier: MIT
#pragma once
#include "common.hpp"
#include <string_view>

namespace nvs30::profiles {
struct RuntimeProfile {
    const char* sha256;
    const char* name;
    std::size_t smooth_enable;
    bool inspected_layout;
    std::size_t wrapper_table;
    unsigned enable_slot, option_slot;
};
// The first profile retains the upstream reference path. The second was
// inspected offline; its loaded image is also checked before any patch.
inline constexpr RuntimeProfile reference{
    "cd395d58f41c6e393c31a9898f2be3da83f7bc109a2228935c23e8c89b944c15",
    "upstream cd395", 0xe9, false, 0, 19, 20};
inline constexpr RuntimeProfile inspected{
    "66aceaa6f7539d3171de14e88b725a96f86fb9a370fe4d2e151e451cd11fd712",
    "inspected 66ace", 0xe1, true, 0x1d1d08, 19, 20};
inline const RuntimeProfile* find(std::string_view sha) {
    if(sha==reference.sha256)return &reference;
    if(sha==inspected.sha256)return &inspected;
    return nullptr;
}
bool validate_layout(HMODULE module, const RuntimeProfile& profile);
bool wrapper_controls(HMODULE module, const RuntimeProfile& profile,
                      const void* object, void*& enable, void*& option);
}
