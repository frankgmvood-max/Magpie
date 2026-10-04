// SPDX-License-Identifier: MIT
#include <nvs30/profile.hpp>
#include <nvs30/pe.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
namespace {
void Check(bool value,const char* why){if(!value){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
struct Fixture {
    unsigned char* bytes=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x80c000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    HMODULE module=reinterpret_cast<HMODULE>(bytes);
    void Put(size_t at,std::initializer_list<unsigned char> values){std::memcpy(bytes+at,values.begin(),values.size());}
    Fixture(){
        Check(bytes!=nullptr,"fixture allocation");
        auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(bytes);dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x80;
        auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes+0x80);nt->Signature=IMAGE_NT_SIGNATURE;
        nt->FileHeader.NumberOfSections=3;nt->FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt->OptionalHeader.SizeOfImage=0x80c000;
        auto* sections=IMAGE_FIRST_SECTION(nt);
        const DWORD va[]{0x1000,0x1d0000,0x22c000},size[]{0x1ce400,0x5bc00,0x5b0bd4};
        for(int i=0;i<3;++i){sections[i].VirtualAddress=va[i];sections[i].Misc.VirtualSize=size[i];sections[i].Characteristics=IMAGE_SCN_MEM_READ|(i==0?IMAGE_SCN_MEM_EXECUTE:i==2?IMAGE_SCN_MEM_WRITE:0);}
        Put(0xc3ac,{0x83,0x79,0x14,0x03});Put(0xc3c7,{0x40,0x0f,0x9d,0xc6});
        Put(0x59e0,{0x48,0x83,0xec,0x28,0xba,0x03,0,0,0,0x48,0x8d,0x0d,0x60,0xd3,0x7c,0});
        Put(0xa060,{0x40,0x38,0x77,0x4c,0x75,4,0x33,0xc0,0xeb,7,0x0f,0xb6,0x87,0xe1,0,0,0,0x88,0x87,0x9c,0x12,0,0});
        Put(0x12f50,{0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x88,0x51,0x50,0x0f,0xb6,0xf2,0x48,0x8b,0xb9,0x10,0x13,0,0});
        Put(0x12ed0,{0x88,0x51,0x51,0xc3});
        Put(0x516c9,{0x48,0x8d,0x05,0x38,0x06,0x18,0,0x48,0x89,0x06});
        // Preserve the real adjacency: the preceding tables have three
        // methods each. The constructor-bound controller starts at 1d1d08.
        auto** table=reinterpret_cast<void**>(bytes+0x1d1d08);
        table[0]=bytes+0x52ad0;table[1]=bytes+0x56020;
        table[19]=bytes+0x12f50;table[20]=bytes+0x12ed0;
        // A synthetic named IAT with graph at slot 0 and module-load at 13.
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]={0x1d0b00,2*sizeof(IMAGE_IMPORT_DESCRIPTOR)};
        auto* desc=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(bytes+0x1d0b00);desc->Name=0x1d2000;desc->OriginalFirstThunk=0x1d1000;desc->FirstThunk=0x1d07a8;
        strcpy_s(reinterpret_cast<char*>(bytes+desc->Name),64,"nvcuda.dll");
        auto* names=reinterpret_cast<IMAGE_THUNK_DATA64*>(bytes+desc->OriginalFirstThunk);
        for(unsigned i=0;i<=13;++i){const auto rva=0x1d2100+i*64;names[i].u1.AddressOfData=rva;
            strcpy_s(reinterpret_cast<char*>(bytes+rva+2),62,i==0?"cuGraphLaunch":i==13?"cuModuleLoadData":"unused");}
    }
    ~Fixture(){if(bytes)VirtualFree(bytes,0,MEM_RELEASE);}
};
}
int main(){
    using namespace nvs30::profiles;
    Check(find(inspected.sha256)==&inspected && inspected.smooth_enable==0xe1,"inspected profile selected");
    Check(find(reference.sha256)==&reference && reference.smooth_enable==0xe9,"upstream reference retained");
    Check(find("66ace-but-not-the-inspected-file")==nullptr,"partial or unknown hashes refused");
    Fixture image;Check(validate_layout(image.module,inspected),"synthetic inspected image accepted");
    image.bytes[0xa06d]=0xe9;Check(!validate_layout(image.module,inspected),"old config field on new image rejected");image.bytes[0xa06d]=0xe1;
    image.bytes[0xc3af]=2;Check(!validate_layout(image.module,inspected),"externally patched gate rejected");image.bytes[0xc3af]=3;
    RuntimeProfile adjacent=inspected;adjacent.wrapper_table=0x1d1cd8;adjacent.enable_slot=25;adjacent.option_slot=26;
    Check(!validate_layout(image.module,adjacent),"adjacent table refused despite matching enable addresses across its boundary");
    image.bytes[0x516cc]=8;Check(!validate_layout(image.module,inspected),"constructor pointing to adjacent class rejected");image.bytes[0x516cc]=0x38;
    auto** table=reinterpret_cast<void**>(image.bytes+inspected.wrapper_table);const auto saved=table[19];table[19]=image.bytes+0x12ed0;
    Check(!validate_layout(image.module,inspected),"wrong D3D12 enable method rejected");table[19]=saved;
    auto* controller=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x2000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));Check(controller!=nullptr,"controller allocation");
    std::memcpy(controller,&table,sizeof table);void* enable=nullptr,*option=nullptr;
    Check(wrapper_controls(image.module,inspected,controller,enable,option) && enable==image.bytes+0x12f50 && option==image.bytes+0x12ed0,"constructor-bound private methods selected at 19/20");
    auto** adjacentTable=reinterpret_cast<void**>(image.bytes+0x1d1cd8);std::memcpy(controller,&adjacentTable,sizeof adjacentTable);
    Check(!wrapper_controls(image.module,inspected,controller,enable,option),"object of adjacent class is not a D3D12 controller");
    void* impostor[21]{};impostor[19]=table[19];impostor[20]=table[20];auto* other=impostor;std::memcpy(controller,&other,sizeof other);
    Check(!wrapper_controls(image.module,inspected,controller,enable,option) && !enable && !option,"matching method addresses on a different controller are insufficient");
    std::memcpy(controller+0x1ff8,&table,sizeof table);Check(!wrapper_controls(image.module,inspected,controller+0x1ff8,enable,option),"short controller range refused before calling driver");
    VirtualFree(controller,0,MEM_RELEASE);
    std::puts("inspected config/IAT/gate/vtable identity and rejection cases passed; no NVIDIA DLL loaded");
}
