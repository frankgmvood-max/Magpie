#include "nvs30/nvpresent.hpp"

#include "nvs30/config.hpp"
#include "nvs30/fatbin.hpp"
#include "nvs30/log.hpp"
#include "nvs30/pe.hpp"

#include <intrin.h>

namespace nvs30::nvpresent {
namespace {
using CuModuleLoadData = int (WINAPI*)(void** module, const void* image);
using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);
// The inspected export returns a C++ bool in AL, not a Win32 BOOL in EAX.
using NvpInitD3D = bool (WINAPI*)();

HMODULE g_nvp{};
CuModuleLoadData g_real_cu_load{};
GetProcAddressFn g_real_getproc{};
std::vector<void**> g_cuda_slots;
void** g_getproc_slot{};
std::atomic_bool g_initialized{};
std::mutex g_cuda_mutex;
std::atomic_uint64_t g_cuda_intercepts{};
std::atomic_uint64_t g_graph_launches{},g_retargets{};
using GraphLaunch=int (WINAPI*)(void*,void*);
GraphLaunch g_real_graph{};
std::vector<void**> g_graph_slots;
int WINAPI hooked_graph_launch(void* graph,void* stream) {
    const int rc=g_real_graph?g_real_graph(graph,stream):3;
    if(rc==0)++g_graph_launches;
    return rc;
}
bool install_graph_hook() {
    HMODULE cuda=GetModuleHandleW(L"nvcuda.dll");
    if(!cuda)return false;
    for(const char* name:{"cuGraphLaunch","cuGraphLaunch_ptsz"}) {
        const auto real=reinterpret_cast<GraphLaunch>(GetProcAddress(cuda,name));
        if(!real)continue;
        auto slots=pe::find_data_pointer_slots(g_nvp,reinterpret_cast<void*>(real));
        if(auto** direct=pe::find_import_slot(g_nvp,"nvcuda.dll",name)) {
            // Only a resolved import with this exact ABI can be intercepted.
            if(*direct==reinterpret_cast<void*>(real))slots.push_back(direct);
        }
        if(slots.empty())continue;
        g_real_graph=real;
        for(auto** slot:slots) {
            if(g_graph_slots.size()==16)break;
            if(*slot==reinterpret_cast<void*>(real) && pe::write_pointer(slot,reinterpret_cast<void*>(&hooked_graph_launch)))g_graph_slots.push_back(slot);
        }
        if(!g_graph_slots.empty())return true;
    }
    return false;
}
void* g_present_trampoline{};
void* g_present1_trampoline{};

void log_nvp_exports() {
    // List NvPresent's export names so the log shows whether a per-device /
    // per-swapchain registration entry point exists that the bridge should
    // call after NVP_Init_D3D. Bounds-checked against the image size; never
    // throws.
    const auto* base = reinterpret_cast<const std::byte*>(g_nvp);
    const std::size_t image = pe::image_size(g_nvp);
    if (!base || image < sizeof(IMAGE_DOS_HEADER)) return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image)
        return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size ||
        static_cast<std::size_t>(dir.VirtualAddress) + dir.Size > image)
        return;
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    if (exp->NumberOfNames == 0 || exp->NumberOfNames > 512) {
        logf("[nvs30] NvPresent exports: %u functions.\n", exp->NumberOfFunctions);
        return;
    }
    if (static_cast<std::size_t>(exp->AddressOfNames) + exp->NumberOfNames * 4 > image) return;
    const auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
    char line[1024]{};
    std::size_t pos = 0;
    unsigned count = 0;
    for (DWORD i = 0; i < exp->NumberOfNames && pos + 32 < sizeof(line); ++i) {
        if (names[i] >= image) continue;
        const char* s = reinterpret_cast<const char*>(base + names[i]);
        std::size_t len = 0;
        while (len < 64 && s + len < reinterpret_cast<const char*>(base + image) && s[len]) ++len;
        if (len == 0 || len >= 64) continue;
        if (pos + len + 1 >= sizeof(line)) break;
        if (count) line[pos++] = ' ';
        std::memcpy(line + pos, s, len);
        pos += len;
        ++count;
    }
    line[pos] = '\0';
    logf("[nvs30] NvPresent exports (%u): %s\n", count, line);
}

struct BytePatch {
    std::byte* address{};
    std::vector<std::byte> original;
    std::vector<std::byte> replacement;
    bool active{};

    bool apply(const void* new_bytes, std::size_t size) {
        if (!address || !new_bytes || !size) return false;
        original.assign(address, address + size);
        const auto* bytes=static_cast<const std::byte*>(new_bytes);
        this->replacement.assign(bytes,bytes+size);
        active = pe::write_memory(address, new_bytes, size);
        return active;
    }
    bool reassert() { return active && pe::write_memory(address,replacement.data(),replacement.size()); }
    void restore() {
        if (active && !original.empty()) pe::write_memory(address, original.data(), original.size());
        active = false;
    }
};

BytePatch g_arch_patch;
BytePatch g_capability_patch;
std::array<BytePatch, 4> g_config_patches;

std::wstring locate_nvpresent() {
    // The addon resolves the active NVIDIA UMD package and validates its
    // profile before this function is ever called. Do not scan old driver
    // packages by timestamp or load a proxy from an arbitrary runtime folder.
    return config().nvpresent_path;
}

bool decode_reference_gate_tail(const std::byte* instruction, std::size_t available,
                                std::array<std::byte, 4>& output,
                                std::size_t& length) {
    output.fill(std::byte{0x90});

    // Reference NvPresent builds use SIL for the capability result. Accept
    // either the original SETGE encoding or the already-patched MOV SIL,1
    // form so initialization is idempotent across repeated loads.
    if (available >= 4 &&
        instruction[0] == std::byte{0x40} &&
        instruction[1] == std::byte{0x0f} &&
        instruction[2] == std::byte{0x9d} &&
        instruction[3] == std::byte{0xc6}) {
        output = {std::byte{0x40}, std::byte{0xb6}, std::byte{0x01}, std::byte{0x90}};
        length = 4;
        return true;
    }
    if (available >= 4 &&
        instruction[0] == std::byte{0x40} &&
        instruction[1] == std::byte{0xb6} &&
        instruction[2] == std::byte{0x01} &&
        instruction[3] == std::byte{0x90}) {
        output = {std::byte{0x40}, std::byte{0xb6}, std::byte{0x01}, std::byte{0x90}};
        length = 4;
        return true;
    }
    if (available >= 3 &&
        instruction[0] == std::byte{0x0f} &&
        instruction[1] == std::byte{0x9d} &&
        instruction[2] == std::byte{0xc6}) {
        output[0] = std::byte{0xb6};
        output[1] = std::byte{0x01};
        output[2] = std::byte{0x90};
        length = 3;
        return true;
    }
    if (available >= 3 &&
        instruction[0] == std::byte{0xb6} &&
        instruction[1] == std::byte{0x01} &&
        instruction[2] == std::byte{0x90}) {
        output[0] = std::byte{0xb6};
        output[1] = std::byte{0x01};
        output[2] = std::byte{0x90};
        length = 3;
        return true;
    }
    return false;
}

struct GateMatch {
    std::byte* immediate{};
    std::byte* setge{};
    std::array<std::byte, 4> force_true{};
    std::size_t setge_size{};
};

std::uintptr_t module_offset(const void* address) {
    return reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(g_nvp);
}

std::optional<GateMatch> find_gate(NvpInitD3D /*init*/) {
    // Match the working rehost's structural signature exactly:
    //   cmp dword ptr [rcx+14h], 2/3
    // followed within 40 bytes by SETGE SIL (or its already-patched form).
    // Avoid the broader "any [reg+disp8]" scan used by the second DLL; that
    // can select unrelated comparisons in newer NvPresent text sections.
    for (const auto& section : pe::sections(g_nvp)) {
        if (!(section.characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (section.size < 8) continue;

        for (std::size_t i = 0; i + 8 <= section.size; ++i) {
            auto* p = section.begin + i;
            if (p[0] != std::byte{0x83} ||
                p[1] != std::byte{0x79} ||
                p[2] != std::byte{0x14} ||
                (p[3] != std::byte{0x03} && p[3] != std::byte{0x02}))
                continue;

            const std::size_t limit = std::min<std::size_t>(40, section.size - i);
            for (std::size_t j = 4; j < limit; ++j) {
                GateMatch match{};
                match.immediate = p + 3;
                match.setge = p + j;
                if (!decode_reference_gate_tail(p + j, section.size - i - j,
                                                match.force_true,
                                                match.setge_size))
                    continue;

                logf("[nvs30] Gate located dynamically: cmp=+0x%zx setge=+0x%zx len=%zu.\n",
                     module_offset(match.immediate), module_offset(match.setge),
                     match.setge_size);
                return match;
            }
        }
    }
    return std::nullopt;
}

std::byte* find_config(NvpInitD3D init) {
    auto* code = reinterpret_cast<std::byte*>(init);
    for (std::size_t i = 0; i + 7 <= 96; ++i) {
        if (code[i] != std::byte{0x48} || code[i + 1] != std::byte{0x8d} ||
            code[i + 2] != std::byte{0x0d}) continue;
        std::int32_t displacement{};
        std::memcpy(&displacement, code + i + 3, sizeof(displacement));
        auto* target = code + i + 7 + displacement;
        if (pe::address_in_writable_section(g_nvp, target, 0x12a6)) return target;
    }
    return nullptr;
}

int WINAPI hooked_cu_module_load_data(void** module_out, const void* image) {
    if (!g_real_cu_load) return 3;
    std::scoped_lock lock(g_cuda_mutex);
    static long serial = 0;
    const long id = ++serial;
    ++g_cuda_intercepts;
    auto rewritten = fatbin::rewrite_sm89_to_sm86(image);
    if (!rewritten.valid || rewritten.stats.sm89_to_sm86 == 0) {
        if (config().diagnostics)
            logf("[nvs30] CUDA fatbin #%ld left unmodified: valid=%d entries=%u sm89=%u sm120=%u.\n",
                 id, rewritten.valid ? 1 : 0, rewritten.stats.entries,
                 rewritten.stats.sm89_to_sm86, rewritten.stats.sm120_left);
        return g_real_cu_load(module_out, image);
    }
    const int rc = g_real_cu_load(module_out, rewritten.bytes.data());
    if(rc==0)g_retargets.fetch_add(rewritten.stats.sm89_to_sm86);
    logf("[nvs30] CUDA fatbin #%ld: bytes=%zu entries=%u cubins=%u sm89->86=%u sm120-left=%u elf=%u rc=%d\n",
         id, rewritten.bytes.size(), rewritten.stats.entries, rewritten.stats.cubins,
         rewritten.stats.sm89_to_sm86, rewritten.stats.sm120_left,
         rewritten.stats.elf_headers, rc);
    return rc;
}

FARPROC WINAPI hooked_get_proc_address(HMODULE module, LPCSTR name) {
    if (reinterpret_cast<std::uintptr_t>(name) > 0xffff && name &&
        std::strcmp(name, "cuModuleLoadData") == 0)
        return reinterpret_cast<FARPROC>(&hooked_cu_module_load_data);
    return g_real_getproc(module, name);
}

bool install_cuda_hook(const GateMatch& discovered_gate, const std::byte* discovered_cfg) {
    // First try the exact path used by the working reference build: locate the
    // named cuModuleLoadData thunk in NvPresent's normal import table and patch
    // that slot directly.
    if (auto** direct = pe::find_import_slot(g_nvp, "nvcuda.dll", "cuModuleLoadData")) {
        g_real_cu_load = reinterpret_cast<CuModuleLoadData>(*direct);
        if (!g_real_cu_load ||
            !pe::write_pointer(direct, reinterpret_cast<void*>(&hooked_cu_module_load_data)))
            return false;
        g_cuda_slots.push_back(direct);
        logf("[nvs30] cuModuleLoadData IAT found dynamically: +0x%zx; direct hook installed.\n",
             module_offset(direct));
        return true;
    }

    // The working reference DLL contains a guarded compatibility path for the
    // current NvPresent layout used by NVIDIA's driver package.  Its normal
    // import walk can fail even though a CUDA dispatch slot exists.  Before
    // falling back to generic resolved-pointer searches, reproduce that path:
    // validate the resolver stub at +0x1348D0, call it, then consume the
    // populated cuModuleLoadData slot at +0x7FB628.  These offsets are never
    // used blindly: both addresses must lie in the image, the resolver must be
    // executable, the slot must be non-executable/readable, and the exact
    // eight-byte resolver prologue checked by the reference DLL must match.
    {
        constexpr std::size_t kReferenceResolverRva = 0x1348d0;
        constexpr std::size_t kReferenceCudaSlotRva = 0x7fb628;
        constexpr std::array<std::byte, 8> kReferenceResolverPrefix{
            std::byte{0x48}, std::byte{0x83}, std::byte{0xec}, std::byte{0x28},
            std::byte{0x45}, std::byte{0x33}, std::byte{0xc9}, std::byte{0x48}
        };

        const auto image = pe::image_size(g_nvp);
        auto* base = reinterpret_cast<std::byte*>(g_nvp);
        const bool reference_layout =
            module_offset(discovered_gate.immediate) == 0xc41f &&
            module_offset(discovered_gate.setge) == 0xc437 &&
            module_offset(discovered_cfg) == 0x7f0cd0;
        if (reference_layout &&
            image > kReferenceResolverRva + kReferenceResolverPrefix.size() &&
            image > kReferenceCudaSlotRva + sizeof(void*)) {
            auto* resolver = base + kReferenceResolverRva;
            auto** slot = reinterpret_cast<void**>(base + kReferenceCudaSlotRva);

            bool resolver_executable = false;
            bool slot_readable_nonexec = false;
            for (const auto& section : pe::sections(g_nvp)) {
                const auto* begin = section.begin;
                const auto* end = section.begin + section.size;
                if (resolver >= begin &&
                    resolver + kReferenceResolverPrefix.size() <= end &&
                    (section.characteristics & IMAGE_SCN_MEM_EXECUTE))
                    resolver_executable = true;
                const auto* slot_bytes = reinterpret_cast<const std::byte*>(slot);
                if (slot_bytes >= begin && slot_bytes + sizeof(void*) <= end &&
                    (section.characteristics & IMAGE_SCN_MEM_READ) &&
                    !(section.characteristics & IMAGE_SCN_MEM_EXECUTE))
                    slot_readable_nonexec = true;
            }

            if (resolver_executable && slot_readable_nonexec &&
                std::memcmp(resolver, kReferenceResolverPrefix.data(),
                            kReferenceResolverPrefix.size()) == 0) {
                using ReferenceResolver = int (WINAPI*)();
                const int resolver_rc =
                    reinterpret_cast<ReferenceResolver>(resolver)();
                if (resolver_rc == 0 && *slot) {
                    g_real_cu_load = reinterpret_cast<CuModuleLoadData>(*slot);
                    if (pe::write_pointer(
                            slot, reinterpret_cast<void*>(&hooked_cu_module_load_data))) {
                        g_cuda_slots.push_back(slot);
                        logf("[nvs30] cuModuleLoadData IAT found dynamically: +0x%zx "
                             "(validated reference resolver fallback).\n",
                             module_offset(slot));
                        return true;
                    }
                    logf("[nvs30] validated reference CUDA slot +0x%zx but could not patch it.\n",
                         module_offset(slot));
                } else if (config().diagnostics) {
                    logf("[nvs30] reference CUDA resolver candidate rejected: rc=%d slot=%p.\n",
                         resolver_rc, *slot);
                }
            } else if (config().diagnostics) {
                logf("[nvs30] reference CUDA fallback signature not present; continuing generic discovery.\n");
            }
        }
    }

    // Newer/previously-initialized NvPresent builds can have the CUDA entry
    // point resolved before we arrive, leaving no name-addressable thunk for a
    // normal PE import lookup. Resolve the real driver entry point ourselves
    // and search NvPresent's non-executable image data for slots that already contain
    // that exact function pointer. This catches both a bound IAT and driver
    // dispatch globals while staying away from executable code.
    HMODULE cuda = GetModuleHandleW(L"nvcuda.dll");
    if (!cuda) cuda = LoadLibraryW(L"nvcuda.dll");
    if (cuda)
        g_real_cu_load = reinterpret_cast<CuModuleLoadData>(
            GetProcAddress(cuda, "cuModuleLoadData"));

    if (g_real_cu_load) {
        if (auto** delay = pe::find_delay_import_slot(g_nvp, "nvcuda.dll", "cuModuleLoadData")) {
            if (!pe::write_pointer(delay, reinterpret_cast<void*>(&hooked_cu_module_load_data)))
                return false;
            g_cuda_slots.push_back(delay);
            logf("[nvs30] cuModuleLoadData delay-IAT found dynamically: +0x%zx; direct hook installed.\n",
                 module_offset(delay));
            return true;
        }

        if (auto** slot = pe::find_iat_value(g_nvp, reinterpret_cast<void*>(g_real_cu_load))) {
            if (!pe::write_pointer(slot, reinterpret_cast<void*>(&hooked_cu_module_load_data)))
                return false;
            g_cuda_slots.push_back(slot);
            logf("[nvs30] cuModuleLoadData IAT found by resolved value: +0x%zx; direct hook installed.\n",
                 module_offset(slot));
            return true;
        }

        auto slots = pe::find_data_pointer_slots(
            g_nvp, reinterpret_cast<void*>(g_real_cu_load));
        if (!slots.empty()) {
            std::size_t patched = 0;
            for (auto** slot : slots) {
                // Limit the fallback to a small number of exact resolved-value
                // slots. Multiple driver dispatch aliases are safe to redirect
                // because they all originally call the same CUDA entry point.
                if (patched >= 16) break;
                if (!pe::write_pointer(slot, reinterpret_cast<void*>(&hooked_cu_module_load_data)))
                    continue;
                g_cuda_slots.push_back(slot);
                logf("[nvs30] cuModuleLoadData resolved pointer slot hooked: +0x%zx.\n",
                     module_offset(slot));
                ++patched;
            }
            if (patched) {
                logf("[nvs30] direct CUDA interception installed through %zu resolved NvPresent slot(s).\n",
                     patched);
                return true;
            }
        }
    }

    // Last resort only. This works when NvPresent resolves CUDA after our hook
    // is installed, but cannot repair a function pointer that was cached before
    // NVSmooth30 loaded; the resolved-pointer scan above exists for that case.
    if (!g_real_cu_load) return false;
    g_real_getproc = &GetProcAddress;
    g_getproc_slot = pe::find_import_slot(g_nvp, "KERNEL32.dll", "GetProcAddress");
    if (!g_getproc_slot)
        g_getproc_slot = pe::find_iat_value(g_nvp, reinterpret_cast<void*>(GetProcAddress));
    if (!g_getproc_slot ||
        !pe::write_pointer(g_getproc_slot, reinterpret_cast<void*>(&hooked_get_proc_address)))
        return false;
    logf("[nvs30] adaptive CUDA hook installed through NvPresent64 GetProcAddress IAT (last resort).\n");
    return true;
}

void restore_all() {
    for(auto** slot:g_graph_slots)
        if(slot && *slot==reinterpret_cast<void*>(&hooked_graph_launch))pe::write_pointer(slot,reinterpret_cast<void*>(g_real_graph));
    g_graph_slots.clear();
    for (auto& patch : g_config_patches) patch.restore();
    g_capability_patch.restore();
    g_arch_patch.restore();
    if (g_real_cu_load) {
        for (auto** slot : g_cuda_slots)
            if (slot) pe::write_pointer(slot, reinterpret_cast<void*>(g_real_cu_load));
    }
    g_cuda_slots.clear();
    if (g_getproc_slot && g_real_getproc)
        pe::write_pointer(g_getproc_slot, reinterpret_cast<void*>(g_real_getproc));
}

bool readable_range(const void* address, std::size_t size) {
    if (!address || !size) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    return begin >= region && size <= info.RegionSize && begin - region <= info.RegionSize - size;
}

bool nvp_vtable(void* object) {
    if (!readable_range(object, sizeof(void*))) return false;
    void** table{};
    std::memcpy(&table, object, sizeof(table));
    // Swapchain vtables are shared, and both NVSmooth30 and NvPresent may
    // patch Present in place. Check the object identity slots (0/7) for a
    // full wrapper object, plus the Present slots (8/22) for an in-place
    // Present hook living inside NvPresent.
    if (!readable_range(table, 24 * sizeof(void*))) {
        if (!readable_range(table, 9 * sizeof(void*))) return false;
        return pe::address_in_image(g_nvp, table[0]) || pe::address_in_image(g_nvp, table[7]) ||
               pe::address_in_image(g_nvp, table[8]);
    }
    return pe::address_in_image(g_nvp, table[0]) || pe::address_in_image(g_nvp, table[7]) ||
           pe::address_in_image(g_nvp, table[8]) || pe::address_in_image(g_nvp, table[22]);
}
}

bool initialize() {
    if (g_initialized.load()) return true;
    const std::wstring path = locate_nvpresent();
    if (path.empty()) {
        logf("[nvs30] NvPresent64.dll was not found.\n");
        return false;
    }
    g_nvp = LoadLibraryW(path.c_str());
    if (!g_nvp || !pe::valid_image(g_nvp)) return false;
    logf("[nvs30] Loaded NvPresent64: %ls\n", path.c_str());
    log_nvp_exports();

    auto* init = reinterpret_cast<NvpInitD3D>(GetProcAddress(g_nvp, "NVP_Init_D3D"));
    if (!init) {
        logf("[nvs30] NVP_Init_D3D export not found; no NvPresent patches retained.\n");
        return false;
    }
    const auto* profile=config().runtime_profile;
    if(!profile || !profiles::validate_layout(g_nvp,*profile)) {
        logf("[nvs30] loaded NvPresent layout differs from the inspected profile; no patches applied.\n");
        return false;
    }
    const auto gate = find_gate(init);
    auto* cfg = init ? find_config(init) : nullptr;
    if (!gate || !cfg) {
        logf("[nvs30] gate/config validation failed; no NvPresent patches retained.\n");
        return false;
    }
    if(profile->inspected_layout && (module_offset(init)!=0x59e0 ||
       module_offset(gate->immediate)!=0xc3af || module_offset(gate->setge)!=0xc3c7 ||
       module_offset(cfg)!=0x7d2d50 || cfg[2]!=std::byte{} || cfg[3]!=std::byte{})) {
        logf("[nvs30] inspected gate/config or fresh-initializer state mismatch; restart LS required.\n");
        return false;
    }
    logf("[nvs30] Config structure resolved dynamically: +0x%zx.\n", module_offset(cfg));
    if (!install_cuda_hook(*gate, cfg)) {
        logf("[nvs30] no CUDA interception path available; rolling back NvPresent patches.\n");
        restore_all();
        return false;
    }

    if(!install_graph_hook()) {
        logf("[nvs30] CUDA graph execution cannot be observed; rolling back before enabling Smooth Motion.\n");
        restore_all();return false;
    }
    // Transaction: snapshot-then-patch arch gate, capability gate, CUDA IAT
    // slot, and config bytes as one unit; any failure restores everything.
    const std::byte arch{2};
    g_arch_patch.address = gate->immediate;
    if (!g_arch_patch.apply(&arch, 1)) { restore_all(); return false; }
    g_capability_patch.address = gate->setge;
    if (!g_capability_patch.apply(gate->force_true.data(), gate->setge_size)) {
        restore_all(); return false;
    }

    const std::byte one{1},zero{0};
    logf("[nvs30] Config before init: enable +0x%zx=%u bypass4c=%u DX11=%u DX12=%u\n",
         profile->smooth_enable,std::to_integer<unsigned>(cfg[profile->smooth_enable]),
         std::to_integer<unsigned>(cfg[0x4c]),std::to_integer<unsigned>(cfg[0x4f]),std::to_integer<unsigned>(cfg[0x50]));
    g_config_patches[0].address = cfg + 0x4c;
    g_config_patches[1].address = cfg + profile->smooth_enable;
    if (!g_config_patches[0].apply(&one, 1) || !g_config_patches[1].apply(&one, 1)) {
        restore_all(); return false;
    }
    if(profile->inspected_layout) {
        // This addon feeds a D3D12 output. Disable NvPresent's D3D11 path so
        // the original LS surface cannot become a second SM generator.
        g_config_patches[2].address=cfg+0x4f;
        g_config_patches[3].address=cfg+0x50;
        if(!g_config_patches[2].apply(&zero,1) || !g_config_patches[3].apply(&one,1)) {
            restore_all();return false;
        }
    }
    logf("[nvs30] NvPresent gate patches installed (arch + capability) and CUDA IAT hooked.\n");
    if (!init()) {
        logf("[nvs30] NVP_Init_D3D returned FALSE; rolling back.\n");
        restore_all();
        return false;
    }
    // The working DLL reasserts the two config enables after NvPresent's
    // initializer returns, because some driver builds rewrite the structure
    // during NVP_Init_D3D.
    for(auto& patch:g_config_patches)if(patch.active && !patch.reassert()){restore_all();return false;}
    logf("[nvs30] Config after init: enable +0x%zx=%u bypass4c=%u DX11=%u DX12=%u; profile=%s\n",
         profile->smooth_enable,std::to_integer<unsigned>(cfg[profile->smooth_enable]),
         std::to_integer<unsigned>(cfg[0x4c]),std::to_integer<unsigned>(cfg[0x4f]),std::to_integer<unsigned>(cfg[0x50]),profile->name);
    logf("[nvs30] NVP_Init_D3D=TRUE.\n");
    g_initialized = true;
    return true;
}

void shutdown() { restore_all(); g_initialized = false; }
HMODULE module() { return g_nvp; }
bool contains_address(const void* address) { return pe::address_in_image(g_nvp, address); }
void note_present_trampoline(void* present, void* present1) {
    g_present_trampoline = present;
    g_present1_trampoline = present1;
}
bool present_hook_on_path() {
    // Our Present hooks chain to the previously installed Present target.
    // When NvPresent patched the shared DXGI vtable before us, that saved
    // trampoline lives inside NvPresent and every shadow Present we forward
    // still flows through NvPresent even though the live vtable now points
    // at our own hook.
    return (g_present_trampoline && pe::address_in_image(g_nvp, g_present_trampoline)) ||
           (g_present1_trampoline && pe::address_in_image(g_nvp, g_present1_trampoline));
}
bool read_offset_candidate(const std::byte* swap_bytes, std::size_t offset, void*& candidate) {
    candidate = nullptr;
    const void* field = swap_bytes + offset;
    if (!readable_range(field, sizeof(void*))) return false;
    std::memcpy(&candidate, field, sizeof(candidate));
    return true;
}
bool wrapper_active(IDXGISwapChain* swapchain, void** wrapper, std::size_t* found_offset) {
    if (wrapper) *wrapper = nullptr;
    if (found_offset) *found_offset = 0;
    if (!swapchain || !g_nvp) return false;
    if (nvp_vtable(swapchain)) {
        if (wrapper) *wrapper = swapchain;
        return true;
    }

    // The wrapper object has historically lived at +0x18 behind the public
    // swapchain; validate that location first, then fall back to a bounded
    // dynamic scan. Every candidate is accepted only when its vtable lives
    // inside the validated NvPresent image.
    const auto* bytes = reinterpret_cast<const std::byte*>(swapchain);
    void* candidate{};
    if (read_offset_candidate(bytes, 0x18, candidate) && candidate != swapchain &&
        nvp_vtable(candidate)) {
        if (wrapper) *wrapper = candidate;
        if (found_offset) *found_offset = 0x18;
        return true;
    }
    for (std::size_t offset = sizeof(void*); offset <= 0x80; offset += sizeof(void*)) {
        if (offset == 0x18) continue;  // already validated above
        if (!read_offset_candidate(bytes, offset, candidate)) break;
        if (candidate != swapchain && nvp_vtable(candidate)) {
            if (wrapper) *wrapper = candidate;
            if (found_offset) *found_offset = offset;
            return true;
        }
    }
    // Fall back to the chained-trampoline signal: when NvPresent patched
    // Present in place before we installed our hooks, the live vtable points
    // at our hook, but forwarding still reaches NvPresent through the saved
    // trampoline. Treat that as active so the bridge is not disabled by a
    // false-negative vtable read (and so NvPresent gets a chance to attach
    // lazily over the first few shadow Presents).
    if (present_hook_on_path()) {
        if (wrapper) *wrapper = g_present_trampoline ? g_present_trampoline : g_present1_trampoline;
        return true;
    }
    return false;
}
bool wrapper_active(IDXGISwapChain* swapchain, void** wrapper) {
    return wrapper_active(swapchain, wrapper, nullptr);
}
namespace {
bool toggle_guard(void* object,void* enable,void* option) {
    __try {using Toggle=void(WINAPI*)(void*,bool);
        reinterpret_cast<Toggle>(enable)(object,true);
        reinterpret_cast<Toggle>(option)(object,true);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
bool enable_wrapper(IDXGISwapChain* swapchain) {
    const auto* profile=config().runtime_profile;
    if(!swapchain || !g_nvp || !profile)return false;
    const auto* bytes=reinterpret_cast<const std::byte*>(swapchain);
    // Check the known private-controller field first, then other bounded
    // fields. For the inspected profile the exact D3D12 vtable is required.
    for(std::size_t probe=0;probe<=16;++probe) {
        const auto offset=probe==0?std::size_t{0x18}:probe*sizeof(void*);
        if(probe && offset==0x18)continue;
        void* candidate{};
        if(!read_offset_candidate(bytes,offset,candidate))continue;
        void* enable{},*option{};
        if(candidate==swapchain || !profiles::wrapper_controls(g_nvp,*profile,candidate,enable,option))continue;
        if(!toggle_guard(candidate,enable,option))return false;
        logf("[nvs30] D3D12 wrapper enabled: offset=+0x%zx methods=%u/%u profile=%s\n",
             offset,profile->enable_slot,profile->option_slot,profile->name);
        return true;
    }
    return false;
}
std::uint64_t cuda_intercept_count() { return g_cuda_intercepts.load(); }
bool initialized() { return g_initialized.load(); }
std::uint64_t graph_launch_count() {return g_graph_launches.load();}
std::uint64_t retarget_count() {return g_retargets.load();}
}
