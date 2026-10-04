// SPDX-License-Identifier: MIT
#include "nvs30/profile.hpp"
#include "nvs30/pe.hpp"

namespace nvs30::profiles {
namespace {
bool readable(const void* address,std::size_t size) {
    MEMORY_BASIC_INFORMATION m{};
    if(!address || !size || !VirtualQuery(address,&m,sizeof m) || m.State!=MEM_COMMIT ||
       (m.Protect & (PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto a=reinterpret_cast<std::uintptr_t>(address),b=reinterpret_cast<std::uintptr_t>(m.BaseAddress);
    return a>=b && size<=m.RegionSize && a-b<=m.RegionSize-size;
}
bool section_range(HMODULE module,const void* address,std::size_t size,bool code) {
    const auto a=reinterpret_cast<std::uintptr_t>(address);
    for(const auto& s:pe::sections(module)) {
        const auto b=reinterpret_cast<std::uintptr_t>(s.begin);
        if(a>=b && size<=s.size && a-b<=s.size-size &&
           (s.characteristics & IMAGE_SCN_MEM_READ) &&
           bool(s.characteristics & IMAGE_SCN_MEM_EXECUTE)==code)return true;
    }
    return false;
}
template<std::size_t N>bool code_is(HMODULE module,std::size_t rva,const unsigned char (&bytes)[N]) {
    const auto* p=reinterpret_cast<const std::byte*>(module)+rva;
    return section_range(module,p,N,true) && std::memcmp(p,bytes,N)==0;
}
}
bool validate_layout(HMODULE module,const RuntimeProfile& profile) {
    if(!pe::valid_image(module))return false;
    if(!profile.inspected_layout)return true;
    if(pe::image_size(module)!=0x80c000)return false;
    const auto* base=reinterpret_cast<const std::byte*>(module);
    constexpr unsigned char cmp[]{0x83,0x79,0x14,0x03};
    constexpr unsigned char setge[]{0x40,0x0f,0x9d,0xc6};
    constexpr unsigned char init[]{0x48,0x83,0xec,0x28,0xba,0x03,0x00,0x00,0x00,
        0x48,0x8d,0x0d,0x60,0xd3,0x7c,0x00};
    // This initializer consumes config+e1 and caches it at config+129c.
    constexpr unsigned char enabled[]{0x40,0x38,0x77,0x4c,0x75,0x04,0x33,0xc0,0xeb,0x07,
        0x0f,0xb6,0x87,0xe1,0x00,0x00,0x00,0x88,0x87,0x9c,0x12,0x00,0x00};
    constexpr unsigned char enable_method[]{0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,
        0x88,0x51,0x50,0x0f,0xb6,0xf2,0x48,0x8b,0xb9,0x10,0x13,0x00,0x00};
    constexpr unsigned char option_method[]{0x88,0x51,0x51,0xc3};
    if(!code_is(module,0xc3ac,cmp) || !code_is(module,0xc3c7,setge) ||
       !code_is(module,0x59e0,init) || !code_is(module,0xa060,enabled) ||
       !code_is(module,0x12f50,enable_method) || !code_is(module,0x12ed0,option_method) ||
       !pe::address_in_writable_section(module,base+0x7d2d50,0x12a6))return false;
    const auto* table=reinterpret_cast<void* const*>(base+profile.wrapper_table);
    if(!section_range(module,table,27*sizeof(void*),false) ||
       table[25]!=base+0x12f50 || table[26]!=base+0x12ed0)return false;
    return pe::find_import_slot(module,"nvcuda.dll","cuModuleLoadData")==reinterpret_cast<void**>(const_cast<std::byte*>(base)+0x1d0810) &&
           pe::find_import_slot(module,"nvcuda.dll","cuGraphLaunch")==reinterpret_cast<void**>(const_cast<std::byte*>(base)+0x1d07a8);
}
bool wrapper_controls(HMODULE module,const RuntimeProfile& profile,const void* object,void*& enable,void*& option) {
    enable=option=nullptr;
    if(!readable(object,sizeof(void*)))return false;
    void** table{};std::memcpy(&table,object,sizeof table);
    const auto count=std::max(profile.enable_slot,profile.option_slot)+1;
    if(!readable(table,count*sizeof(void*)))return false;
    const auto* base=reinterpret_cast<const std::byte*>(module);
    if(profile.inspected_layout) {
        // D3D12's private controller has more methods than the upstream one.
        // Do not call slots 19/20 merely because they point inside NvPresent.
        if(reinterpret_cast<const std::byte*>(table)!=base+profile.wrapper_table ||
           table[profile.enable_slot]!=base+0x12f50 || table[profile.option_slot]!=base+0x12ed0 ||
           !readable(object,0x1318))return false;
    } else if(!section_range(module,table[profile.enable_slot],1,true) ||
              !section_range(module,table[profile.option_slot],1,true))return false;
    enable=table[profile.enable_slot];option=table[profile.option_slot];return true;
}
}
