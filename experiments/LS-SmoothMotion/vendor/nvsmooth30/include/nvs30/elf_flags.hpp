// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <optional>

namespace nvs30::fatbin {
// CUDA's ELF ABI 51/7 stores real/virtual SM in bits 0..7 and 16..23.
// ABI 65/8 stores SM in bits 8..15. Keep each ABI's remaining flags.
inline constexpr std::optional<std::uint32_t> sm86_elf_flags(
    std::uint8_t osabi,std::uint8_t abi_version,std::uint32_t flags) {
    if(osabi==51 && abi_version==7 && (flags & 0xff)==89 && ((flags>>16)&0xff)==89)
        return (flags & ~std::uint32_t{0x00ff00ff}) | 0x00560056;
    if(osabi==65 && abi_version==8 && ((flags>>8)&0xff)==89)
        return (flags & ~std::uint32_t{0x0000ff00}) | 0x00005600;
    return std::nullopt;
}
}
