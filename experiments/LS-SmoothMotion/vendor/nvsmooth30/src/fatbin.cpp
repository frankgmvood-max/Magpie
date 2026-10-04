#include "nvs30/fatbin.hpp"
#include "nvs30/elf_flags.hpp"

namespace nvs30::fatbin {
namespace {
constexpr std::uint32_t kFatbinMagic = 0xBA55ED50;
constexpr std::uint32_t kSm86 = 0x56;
constexpr std::uint32_t kSm89 = 0x59;
constexpr std::uint32_t kSm120 = 0x78;
constexpr std::size_t kMaximumFatbin = 64u * 1024u * 1024u;

template <class T>
T read(const std::byte* p) {
    T value{};
    std::memcpy(&value, p, sizeof(value));
    return value;
}

template <class T>
void write(std::byte* p, T value) {
    std::memcpy(p, &value, sizeof(value));
}

bool retarget_elf64(std::byte* p, std::size_t size) {
    if(size<0x40 || p[0]!=std::byte{0x7f} || p[1]!=std::byte{0x45} ||
       p[2]!=std::byte{0x4c} || p[3]!=std::byte{0x46} ||
       p[4]!=std::byte{2} || p[5]!=std::byte{1} || p[6]!=std::byte{1} ||
       read<std::uint16_t>(p+0x12)!=190 || read<std::uint16_t>(p+0x34)!=0x40)return false;
    const auto flags=sm86_elf_flags(std::to_integer<std::uint8_t>(p[7]),
                                  std::to_integer<std::uint8_t>(p[8]),read<std::uint32_t>(p+0x30));
    if(!flags)return false;
    write<std::uint32_t>(p+0x30,*flags);return true;
}
}

RewrittenImage rewrite_sm89_to_sm86(const void* image) {
    RewrittenImage result;
    if (!image) return result;
    const auto* source = static_cast<const std::byte*>(image);

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(source, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return result;
    const auto available = static_cast<std::size_t>(
        static_cast<const std::byte*>(mbi.BaseAddress) + mbi.RegionSize - source);
    if (available < 0x10 || read<std::uint32_t>(source) != kFatbinMagic) return result;

    const std::uint16_t header_size = read<std::uint16_t>(source + 6);
    const std::uint64_t payload_size = read<std::uint64_t>(source + 8);
    if (header_size < 0x10 || header_size > 0x100 || header_size > available || payload_size > kMaximumFatbin ||
        payload_size > available - header_size) return result;

    const std::size_t total = header_size + static_cast<std::size_t>(payload_size);
    result.bytes.assign(source, source + total);
    auto* base = result.bytes.data();
    std::size_t cursor = header_size;

    while (cursor < total) {
        if (total - cursor < 0x20) return {};
        auto* entry = base + cursor;
        const std::uint16_t kind = read<std::uint16_t>(entry);
        const std::uint32_t entry_header = read<std::uint32_t>(entry + 4);
        const std::uint32_t data_size = read<std::uint32_t>(entry + 8);
        const std::uint32_t arch = read<std::uint32_t>(entry + 0x1c);
        if (entry_header < 0x20 || entry_header > 0x400 || entry_header > total - cursor)
            return {};
        const std::size_t data_offset = cursor + entry_header;
        if (data_size > total - data_offset) return {};

        ++result.stats.entries;
        if (kind == 2) ++result.stats.cubins;
        if (arch == kSm120) ++result.stats.sm120_left;

        auto* payload = base + data_offset;
        if (kind == 2 && arch == kSm89) {
            // Retarget only a reviewed ELF format. Never replace an ABI 51
            // flags word with ABI 65 flags or pass a partly rewritten image.
            if(!retarget_elf64(payload,data_size))return {};
            write<std::uint32_t>(entry + 0x1c, kSm86);
            ++result.stats.sm89_to_sm86;
            ++result.stats.elf_headers;
        }

        const std::size_t next = align_up(data_offset + data_size, std::size_t{8});
        if (next <= cursor || next > total) return {};
        cursor = next;
    }

    result.valid = cursor == total && result.stats.entries != 0;
    return result;
}
}
