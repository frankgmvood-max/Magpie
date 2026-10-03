#include "present_hook.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
namespace {
struct Call {UINT sync,flags;bool present1;const DXGI_PRESENT_PARAMETERS* params;};
Call calls[16]{};unsigned count=0, callbacks=0;
IDXGISwapChain* selected=nullptr;
bool reject=false;
void Check(bool ok,const char* why) {if(!ok){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
HRESULT STDMETHODCALLTYPE CapturePresent(IDXGISwapChain*,UINT sync,UINT flags) {
    Check(count<16,"too many presents");calls[count++]={sync,flags,false,nullptr};return S_OK;
}
HRESULT STDMETHODCALLTYPE CapturePresent1(IDXGISwapChain1*,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* p) {
    Check(count<16,"too many presents");calls[count++]={sync,flags,true,p};return S_OK;
}
void Replace(void** table,int slot,void* fn) {
    DWORD old=0,ignored=0;Check(VirtualProtect(table+slot,sizeof(void*),PAGE_READWRITE,&old)!=FALSE,"protect table");
    InterlockedExchangePointer(table+slot,fn);VirtualProtect(table+slot,sizeof(void*),old,&ignored);
}
HRESULT Callback(IDXGISwapChain* sc,UINT& sync,UINT& flags,bool&) {
    if(sc!=selected) return S_OK;
    ++callbacks;
    if(reject) return S_OK; // runtime rejection must leave the original contract
    sync=0;flags=DXGI_PRESENT_ALLOW_TEARING;
    return PresentHook::PresentOriginal(sc,sync,flags);
}
}
int main() {
    ComPtr<ID3D11Device> device;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr)),"software device");
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"FGPresentContract";
    RegisterClassW(&wc);HWND window=CreateWindowW(wc.lpszClassName,L"test",WS_POPUP,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
    Check(window!=nullptr,"test window");
    ComPtr<IDXGIDevice> dx;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory2> factory;
    Check(SUCCEEDED(device.As(&dx)) && SUCCEEDED(dx->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))),"factory");
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=64;desc.Height=64;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;Check(SUCCEEDED(factory->CreateSwapChainForHwnd(device.Get(),window,&desc,nullptr,nullptr,&chain)),"swapchain");
    selected=chain.Get();void** table=*reinterpret_cast<void***>(chain.Get());
    void* original=table[8];void* original1=table[22];
    // Capture stubs terminate at the DXGI boundary: this tests hook forwarding,
    // not hardware VRR. Legal swapchain eligibility is tested by policy tests.
    Replace(table,8,reinterpret_cast<void*>(&CapturePresent));Replace(table,22,reinterpret_cast<void*>(&CapturePresent1));
    Check(PresentHook::Install(chain.Get(),Callback,[](const char*){}),"install live hook");
    Check(chain->Present(1,0)==S_OK && count==2,"generated plus outer real Present");
    Check(calls[0].sync==0 && calls[1].sync==0 && calls[0].flags==0x200 && calls[1].flags==0x200,"both Present calls receive VRR arguments");
    DXGI_PRESENT_PARAMETERS p{};
    Check(chain->Present1(1,0,&p)==S_OK && count==4,"generated plus outer real Present1");
    Check(calls[2].sync==0 && calls[3].sync==0 && calls[2].flags==0x200 && calls[3].flags==0x200 && calls[3].present1 && calls[3].params==&p,"Present1 receives modified args and original parameters");
    reject=true;Check(chain->Present(1,0)==S_OK && count==5 && calls[4].sync==1 && calls[4].flags==0,"rejection preserves original Present");reject=false;
    unsigned before=callbacks;
    Check(chain->Present(0,DXGI_PRESENT_TEST)==S_OK && callbacks==before && count==6 && calls[5].flags==DXGI_PRESENT_TEST,"TEST bypass");
    RECT dirty{0,0,16,16};p.DirtyRectsCount=1;p.pDirtyRects=&dirty;
    Check(chain->Present1(1,0,&p)==S_OK && callbacks==before && count==7 && calls[6].sync==1 && calls[6].params==&p,"partial Present1 bypass");
    selected=nullptr;Check(chain->Present(1,0)==S_OK && callbacks==before && count==8 && calls[7].sync==1,"unselected UI bypass");
    PresentHook::Uninstall();Check(chain->Present(1,0)==S_OK && count==9 && calls[8].sync==1,"shutdown pass-through");
    Replace(table,8,original);Replace(table,22,original1);
    chain.Reset();DestroyWindow(window);
    std::puts("generated and real Present/Present1 argument forwarding, rejection, TEST, partial and UI bypass passed");
}
