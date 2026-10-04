#include "present_hook.h"
#include <ls_output_bridge.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
namespace {
struct Call {UINT sync,flags;bool present1;const DXGI_PRESENT_PARAMETERS* params;};
Call calls[32]{};unsigned count=0, callbacks=0,intermediateCount=1;
unsigned afterCalls=0,groupStart=0;
uint64_t lastSequence=0;
HRESULT captureResult=S_OK,lastAfterResult=S_OK;
fg::PresentApi requestedApi=fg::PresentApi::Auto;
IDXGISwapChain* selected=nullptr;
bool reject=false;
bool handle=false,reenterAfter=false;
void Check(bool ok,const char* why) {if(!ok){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
HRESULT STDMETHODCALLTYPE CapturePresent(IDXGISwapChain*,UINT sync,UINT flags) {
    Check(count<32,"too many presents");calls[count++]={sync,flags,false,nullptr};return captureResult;
}
HRESULT STDMETHODCALLTYPE CapturePresent1(IDXGISwapChain1*,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* p) {
    Check(count<32,"too many presents");calls[count++]={sync,flags,true,p};return captureResult;
}
void Replace(void** table,int slot,void* fn) {
    DWORD old=0,ignored=0;Check(VirtualProtect(table+slot,sizeof(void*),PAGE_READWRITE,&old)!=FALSE,"protect table");
    InterlockedExchangePointer(table+slot,fn);VirtualProtect(table+slot,sizeof(void*),old,&ignored);
}
HRESULT Callback(IDXGISwapChain* sc,UINT& sync,UINT& flags,bool& handled) {
    if(sc!=selected) return S_OK;
    ++callbacks;
    groupStart=count;
    if(handle) {handled=true;return DXGI_ERROR_DEVICE_REMOVED;}
    if(reject) return S_OK; // runtime rejection must leave the original contract
    sync=0;flags=DXGI_PRESENT_ALLOW_TEARING;
    for(unsigned index=0;index<intermediateCount;++index) {
        const HRESULT hr=PresentHook::PresentOriginal(sc,sync,flags,requestedApi);
        LsBridgePresentTiming timing{};
        Check(LsBridgeGetPresentTiming(sc,&timing) && timing.sequence>lastSequence && timing.generated && timing.frequency>0 && timing.endQpc>=timing.beginQpc && timing.result==hr,"generated frame exposes a fresh underlying DXGI timestamp");
        Check(timing.sync==sync && timing.flags==flags,"timing keeps actual presentation arguments");lastSequence=timing.sequence;
        if(hr!=S_OK) return hr;
    }
    return S_OK;
}
void After(IDXGISwapChain* sc,HRESULT result) noexcept {
    if(sc!=selected) return;
    ++afterCalls;lastAfterResult=result;
    LsBridgePresentTiming timing{};
    Check(LsBridgeGetPresentTiming(sc,&timing) && timing.sequence>lastSequence && !timing.generated && timing.result==result,"outer real frame gets its own original-call timestamp");lastSequence=timing.sequence;
    if(result==S_OK) Check(count==groupStart+(reject?1:intermediateCount+1),"telemetry must follow all intermediate AND outer real presents");
    if(reenterAfter) {reenterAfter=false;Check(sc->Present(1,0)==captureResult,"telemetry reentry passes through without generation or a second after callback");}
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
    Check(PresentHook::Install(chain.Get(),Callback,[](const char*){},After),"install live hook");
    LsBridgePresentTiming invalid{};invalid.size=0;
    Check(!LsBridgeGetPresentTiming(chain.Get(),&invalid) && !LsBridgeGetPresentTiming(nullptr,&invalid),"invalid timing ABI and chain are rejected");
    Check(chain->Present(1,0)==S_OK && count==2,"generated plus outer real Present");
    Check(calls[0].sync==0 && calls[1].sync==0 && calls[0].flags==0x200 && calls[1].flags==0x200,"both Present calls receive VRR arguments");
    DXGI_PRESENT_PARAMETERS p{};
    Check(chain->Present1(1,0,&p)==S_OK && count==4,"generated plus outer real Present1");
    Check(calls[2].sync==0 && calls[3].sync==0 && calls[2].flags==0x200 && calls[3].flags==0x200 && calls[2].present1 && calls[3].present1 && calls[3].params==&p,"generated and real frames both retain Present1");
    Check(!calls[0].present1 && !calls[1].present1,"Present path retains Present for both frames");
    Check(afterCalls==2,"one telemetry callback after each outer real frame");
    reject=true;Check(chain->Present(1,0)==S_OK && count==5 && calls[4].sync==1 && calls[4].flags==0,"rejection preserves original Present");reject=false;
    unsigned before=callbacks;
    const unsigned afterBefore=afterCalls;
    Check(chain->Present(0,DXGI_PRESENT_TEST)==S_OK && callbacks==before && count==6 && calls[5].flags==DXGI_PRESENT_TEST,"TEST bypass");
    RECT dirty{0,0,16,16};p.DirtyRectsCount=1;p.pDirtyRects=&dirty;
    Check(chain->Present1(1,0,&p)==S_OK && callbacks==before && count==7 && calls[6].sync==1 && calls[6].params==&p,"partial Present1 bypass");
    selected=nullptr;Check(chain->Present(1,0)==S_OK && callbacks==before && count==8 && calls[7].sync==1,"unselected UI bypass");
    Check(afterCalls==afterBefore,"TEST, dirty rects and unselected UI cannot publish output telemetry");
    selected=chain.Get();intermediateCount=3;requestedApi=fg::PresentApi::Present1;
    Check(chain->Present(1,0)==S_OK && count==12,"x4 group: three generated frames then outer real Present");
    for(unsigned i=8;i<11;++i) Check(calls[i].present1 && calls[i].sync==0 && calls[i].flags==0x200,"explicit Present1 applies to all generated frames");
    Check(!calls[11].present1,"explicit generated Present1 preserves outer LS Present");
    requestedApi=fg::PresentApi::Present;p={};
    Check(chain->Present1(1,0,&p)==S_OK && count==16,"x4 group on LS Present1");
    for(unsigned i=12;i<15;++i) Check(!calls[i].present1 && calls[i].sync==0 && calls[i].flags==0x200,"explicit Present applies to all generated frames");
    Check(calls[15].present1 && calls[15].params==&p,"explicit generated Present preserves outer LS Present1 parameters");
    Check(afterCalls==afterBefore+2,"x4 groups still publish telemetry once per outer real frame");
    handle=true;const unsigned beforeHandled=afterCalls;
    Check(chain->Present(1,0)==DXGI_ERROR_DEVICE_REMOVED && count==16 && afterCalls==beforeHandled,"handled failures skip original Present and telemetry");handle=false;
    reject=true;captureResult=DXGI_STATUS_OCCLUDED;
    Check(chain->Present(1,0)==DXGI_STATUS_OCCLUDED && count==17 && lastAfterResult==DXGI_STATUS_OCCLUDED,"outer Present HRESULT forwarded unchanged to caller and telemetry");captureResult=S_OK;
    reenterAfter=true;before=callbacks;
    Check(chain->Present(1,0)==S_OK && count==19 && callbacks==before+1 && afterCalls==beforeHandled+2,"post-Present callback retains nesting protection");
    const unsigned beforeNonblocking=afterCalls;
    Check(chain->Present(0,DXGI_PRESENT_DO_NOT_WAIT)==S_OK && count==20 && afterCalls==beforeNonblocking,"nonblocking Present skips telemetry");reject=false;
    PresentHook::Uninstall();Check(chain->Present(1,0)==S_OK && count==21 && calls[20].sync==1 && afterCalls==beforeNonblocking,"shutdown pass-through disables before and after callbacks");
    Replace(table,8,original);Replace(table,22,original1);
    chain.Reset();DestroyWindow(window);
    std::puts("Auto/Present/Present1, x4, ordered post-real telemetry, nesting, HRESULT, handled, TEST/partial/nonblocking/UI and shutdown passed");
}
