#include "backend.h"
#include "driver.h"
#include <nvs30/fatbin.hpp>
#include <nvs30/elf_flags.hpp>
#include <generation_lease.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
namespace {
void Check(bool b,const char* why){if(!b){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
uint64_t graphs=0;
bool Enable(IDXGISwapChain*){return true;}
uint64_t Graphs(){return graphs;}
uint64_t Retargets(){return 1;}
HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain*,UINT sync,UINT flags){Check(sync==0 && !(flags&DXGI_PRESENT_ALLOW_TEARING),"plain chain never receives illegal tearing");++graphs;return S_OK;}
void Replace(void** t,unsigned slot,void* fn){DWORD old=0,unused=0;Check(VirtualProtect(t+slot,sizeof(void*),PAGE_READWRITE,&old)!=FALSE,"table protection");InterlockedExchangePointer(t+slot,fn);VirtualProtect(t+slot,sizeof(void*),old,&unused);}
template<class T>void Write(std::vector<std::byte>& b,size_t p,T v){std::memcpy(b.data()+p,&v,sizeof v);}
void Elf(std::vector<std::byte>& b,size_t p,uint32_t flags,bool modern) {
    b[p]=std::byte{0x7f};b[p+1]=std::byte{'E'};b[p+2]=std::byte{'L'};b[p+3]=std::byte{'F'};
    b[p+4]=std::byte{2};b[p+5]=b[p+6]=std::byte{1};
    b[p+7]=std::byte(modern?65:51);b[p+8]=std::byte(modern?8:7);
    Write<uint16_t>(b,p+0x12,190);Write<uint16_t>(b,p+0x34,0x40);Write<uint32_t>(b,p+0x30,flags);
}
}
int main(){
    // A mutex would incorrectly allow both owners on the same render thread.
    ls::GenerationLease a,b;Check(a.Acquire() && !b.Acquire(),"same-thread generators are mutually exclusive");a.Reset();Check(b.Acquire(),"ownership is restored on stop");b.Reset();
    wchar_t temp[MAX_PATH]{},file[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);GetTempFileNameW(temp,L"smp",0,file);
    {std::ofstream f(std::filesystem::path(file),std::ios::binary);f<<"not an NVIDIA driver";}
    std::string sha;Check(sm::CheckProfile(file,sha)==sm::Profile::Unsupported && sha.size()==64,"unknown runtime profile is hashed and rejected before loading");DeleteFileW(file);
    std::vector<std::byte> fatbin(0x10+0x20+0x40);
    Write<uint32_t>(fatbin,0,0xba55ed50);Write<uint16_t>(fatbin,6,0x10);Write<uint64_t>(fatbin,8,0x60);
    Write<uint16_t>(fatbin,0x10,2);Write<uint32_t>(fatbin,0x14,0x20);Write<uint32_t>(fatbin,0x18,0x40);Write<uint32_t>(fatbin,0x2c,0x59);
    Elf(fatbin,0x30,0x06005904,true);
    const auto rewritten=nvs30::fatbin::rewrite_sm89_to_sm86(fatbin.data());Check(rewritten.valid && rewritten.stats.sm89_to_sm86==1 && rewritten.stats.elf_headers==1,"fatbin metadata retarget");
    uint32_t arch=0,flags=0;std::memcpy(&arch,rewritten.bytes.data()+0x2c,4);std::memcpy(&flags,rewritten.bytes.data()+0x60,4);Check(arch==0x56 && flags==0x06005604,"exact SM86 flags");
    Write<uint32_t>(fatbin,0x2c,0x78);const auto untouched=nvs30::fatbin::rewrite_sm89_to_sm86(fatbin.data());Check(untouched.valid && untouched.stats.sm89_to_sm86==0 && untouched.bytes==fatbin,"SM120 remains unchanged");
    // Reproduce the inspected DLL's mixed ABI container: header 0x60 for
    // SM120, header 0x40 for SM89. Only the reviewed SM89 fields may change.
    std::vector<std::byte> mixed(0x10+0x60+0x40+0x40+0x40);
    Write<uint32_t>(mixed,0,0xba55ed50);Write<uint16_t>(mixed,6,0x10);Write<uint64_t>(mixed,8,mixed.size()-0x10);
    Write<uint16_t>(mixed,0x10,2);Write<uint32_t>(mixed,0x14,0x60);Write<uint32_t>(mixed,0x18,0x40);Write<uint32_t>(mixed,0x2c,0x78);Elf(mixed,0x70,0x06007802,true);
    const size_t second=0xb0,payload=0xf0;
    Write<uint16_t>(mixed,second,2);Write<uint32_t>(mixed,second+4,0x40);Write<uint32_t>(mixed,second+8,0x40);Write<uint32_t>(mixed,second+0x1c,0x59);Elf(mixed,payload,0x00590559,false);
    const auto legacy=nvs30::fatbin::rewrite_sm89_to_sm86(mixed.data());
    Check(legacy.valid && legacy.stats.entries==2 && legacy.stats.sm89_to_sm86==1 && legacy.stats.sm120_left==1,"mixed CUDA ELF ABIs accepted");
    auto expected=mixed;Write<uint32_t>(expected,second+0x1c,0x56);Write<uint32_t>(expected,payload+0x30,0x00560556);
    Check(legacy.bytes==expected,"SM89 real/virtual arch changed; all other bytes including SM120 preserved");
    Check(nvs30::fatbin::sm86_elf_flags(65,8,0x16005984)==0x16005684,"modern ELF flags unrelated bits preserved");
    Check(nvs30::fatbin::sm86_elf_flags(51,7,0x80591559)==0x80561556,"legacy ELF flags unrelated bits preserved");
    mixed[payload+8]=std::byte{99};
    Check(!nvs30::fatbin::rewrite_sm89_to_sm86(mixed.data()).valid,"unknown ELF ABI fails without a partial rewrite");
    mixed[payload+8]=std::byte{7};Write<uint32_t>(mixed,payload+0x30,0x00590556);
    Check(!nvs30::fatbin::rewrite_sm89_to_sm86(mixed.data()).valid,"container/ELF architecture mismatch rejected");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"WARP");
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SMTestParent";RegisterClassW(&wc);
    HWND parent=CreateWindowW(wc.lpszClassName,L"test",WS_POPUP,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);Check(parent!=nullptr,"parent");ShowWindow(parent,SW_SHOWNOACTIVATE);
    const sm::DriverHooks hooks{Enable,Graphs,Retargets};
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT}){
        D3D11_TEXTURE2D_DESC d{};d.Width=7;d.Height=5;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        const unsigned bytes=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;std::vector<unsigned char> pixels(d.Width*d.Height*bytes);
        for(unsigned i=0;i<pixels.size();++i)pixels[i]=format==DXGI_FORMAT_R16G16B16A16_FLOAT?0:static_cast<unsigned char>(i);
        if(bytes==8)for(unsigned i=0;i<pixels.size();i+=2){pixels[i]=0;pixels[i+1]=0x3c;} // four FP16 1.0 values
        D3D11_SUBRESOURCE_DATA data{pixels.data(),d.Width*bytes,0};ComPtr<ID3D11Texture2D> input;Check(SUCCEEDED(device->CreateTexture2D(&d,&data,&input)),"source");
        sm::Settings settings;settings.targetFPS=0;settings.devicePath=1;
        const HWND foreground=GetForegroundWindow();sm::Backend backend;
        Check(backend.Init(device.Get(),parent,d,settings,true,[](const char* s){std::puts(s);},&hooks),"bridge initialization");
        ComPtr<IDXGIDevice> dx;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC ad{};device.As(&dx);dx->GetAdapter(&adapter);adapter->GetDesc(&ad);
        const auto luid=backend.AdapterLuid();Check(luid.HighPart==ad.AdapterLuid.HighPart && luid.LowPart==ad.AdapterLuid.LowPart,"exact adapter LUID");
        auto* chain=backend.TestChain();HWND child=nullptr;chain->GetHwnd(&child);Check(child && child!=parent && GetParent(child)==parent && !IsWindowEnabled(child),"separate disabled child HWND");
        Check((GetWindowLongPtrW(child,GWL_EXSTYLE)&WS_EX_NOACTIVATE)!=0 && GetForegroundWindow()==foreground,"focus remains unchanged");
        void** table=*reinterpret_cast<void***>(chain);void* old=table[8];Replace(table,8,reinterpret_cast<void*>(Present));
        Check(backend.Prepare(input.Get()) && backend.Present(0)==S_OK && backend.Confirmed(),"simulated wrapper/inference confirmation after real GPU copy");
        Check(backend.Prepare(input.Get()) && backend.Duplicate(),"unchanged input filtered after confirmation");
        // Read the real D3D12 backbuffer filled by the bridge, including odd
        // dimensions and FP16 HDR. The inference controller alone is fake.
        ComPtr<ID3D12Device> d12;chain->GetDevice(IID_PPV_ARGS(&d12));ComPtr<ID3D12Resource> output;chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&output));
        const auto desc=output->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows=0;UINT64 rowBytes=0,total=0;d12->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&total);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=total;rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> read;Check(SUCCEEDED(d12->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read))),"readback resource");
        D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;ComPtr<ID3D12CommandQueue> q;ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> cmd;
        d12->CreateCommandQueue(&qd,IID_PPV_ARGS(&q));d12->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&alloc));d12->CreateCommandList(0,qd.Type,alloc.Get(),nullptr,IID_PPV_ARGS(&cmd));
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE};cmd->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);cmd->ResourceBarrier(1,&barrier);cmd->Close();ID3D12CommandList* lists[]={cmd.Get()};q->ExecuteCommandLists(1,lists);
        ComPtr<ID3D12Fence> fence;d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);q->Signal(fence.Get(),1);fence->SetEventOnCompletion(1,event);Check(WaitForSingleObject(event,2000)==WAIT_OBJECT_0,"readback completion");CloseHandle(event);
        void* mapped=nullptr;D3D12_RANGE range{0,size_t(total)};Check(SUCCEEDED(read->Map(0,&range,&mapped)),"mapped GPU copy");
        for(unsigned y=0;y<d.Height;++y)Check(std::memcmp(static_cast<unsigned char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch,pixels.data()+y*d.Width*bytes,d.Width*bytes)==0,"all D3D11 -> D3D12 color/HDR pixels preserved");D3D12_RANGE written{0,0};read->Unmap(0,&written);
        Replace(table,8,old);backend.Hide();
    }
    DestroyWindow(parent);std::puts("mutual exclusion, unknown-profile refusal, fatbin flags, input-safe HWND and real WARP RGBA/BGRA/FP16 transport passed (NvPresent controller simulated)");
}
