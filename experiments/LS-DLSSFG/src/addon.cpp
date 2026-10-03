// SPDX-License-Identifier: MIT
#include <eam/addon_sdk.h>
#include "backend.h"
#include "present_hook.h"
#include "policy.h"
#include <wrl/client.h>
#include <filesystem>
#include <mutex>
#include <atomic>
#include <cstdio>

using Microsoft::WRL::ComPtr;
namespace {
constexpr const char* id="LS_DLSSFG";
constexpr GUID outputTag={0x396afbe7,0xe6cf,0x4174,{0xa2,0x8b,0xb1,0x89,0xc9,0xdc,0xf8,0x40}};
std::mutex mutex;
IHost* host=nullptr;
std::wstring directory;
std::unique_ptr<fg::Backend> backend;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11Texture2D> real;
fg::Timeline timeline;
uintptr_t selected=0;
uint64_t epoch=1, statusAt=0, generated=0, baseFrames=0;
UINT width=0,height=0;
DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
HANDLE timer=nullptr;
bool armed=false, disabled=false, resetHistory=true;
double targetFps=136;

double Now() { LARGE_INTEGER n{}, f{}; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f); return double(n.QuadPart)/f.QuadPart; }
void Log(const char* text) { if(host) host->Log(EAM_LOG_INFO,text); }
void Status(const char* text,int level) { if(host && GetTickCount64()-statusAt>=500) {statusAt=GetTickCount64();host->SetStatus(id,text,level);} }
void Off(const char* why) {
    disabled=true; resetHistory=true;
    Log(why); if(host) host->SetStatus(id,why,3);
}
void Reset() {
    backend.reset(); real.Reset(); device.Reset(); selected=0;
    width=height=0; format=DXGI_FORMAT_UNKNOWN; timeline.Reset(); resetHistory=true;
}
void Settings() {
    const auto ini=(std::filesystem::path(directory)/L"LS_DLSSFG.ini").wstring();
    armed=GetPrivateProfileIntW(L"FrameGeneration",L"NativeLSFGDisabled",0,ini.c_str())==1;
    targetFps=GetPrivateProfileIntW(L"FrameGeneration",L"TargetFPS",136,ini.c_str());
    if (targetFps!=0 && (targetFps<30 || targetFps>240)) targetFps=136;
    disabled=false; Reset(); ++epoch;
    if(host) host->SetStatus(id,armed?"Ready; waiting for LS output compute pass":"Disable native LSFG, then run Activate-136FPS.cmd",armed?0:2);
}
void WaitUntil(double due) {
    double left=due-Now();
    if(left<=0 || !timer) return;
    if(left>0.0003) {
        LARGE_INTEGER rel{}; rel.QuadPart=-static_cast<LONGLONG>((left-0.00015)*10000000);
        if(SetWaitableTimer(timer,&rel,0,nullptr,nullptr,FALSE)) WaitForSingleObject(timer,50);
    }
    // A bounded final wait, with no priority or system timer-resolution changes.
    const double stop=Now()+0.0004;
    while(Now()<due && Now()<stop) YieldProcessor();
}
HRESULT OnPresent(IDXGISwapChain* sc,UINT sync,UINT flags,bool& handled) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!host || !armed || disabled || !fg::SafePresent(sync,flags)) return S_OK;
    uint64_t tag=0; UINT size=sizeof tag;
    // Only a swap chain actually written by an LS compute pass is eligible.
    // Manager/UI swap chains never acquire this tag, even when maximized.
    if(FAILED(sc->GetPrivateData(outputTag,&size,&tag)) || size!=sizeof tag || tag!=epoch) return S_OK;
    try {
        DXGI_SWAP_CHAIN_DESC sd{};
        if(FAILED(sc->GetDesc(&sd)) || sd.SampleDesc.Count!=1 ||
           (sd.SwapEffect!=DXGI_SWAP_EFFECT_FLIP_DISCARD && sd.SwapEffect!=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)) return S_OK;
        ComPtr<ID3D11Device> currentDevice;
        ComPtr<ID3D11Texture2D> back;
        if(FAILED(sc->GetDevice(IID_PPV_ARGS(&currentDevice))) || FAILED(sc->GetBuffer(0,IID_PPV_ARGS(&back)))) return S_OK;
        D3D11_TEXTURE2D_DESC d{}; back->GetDesc(&d);
        if(d.SampleDesc.Count!=1 || d.ArraySize!=1 || d.MipLevels!=1) return S_OK;
        if(d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT) {
            Status("Unsupported output format; use SDR or scRGB",2); return S_OK;
        }
        if(selected!=reinterpret_cast<uintptr_t>(sc) || device.Get()!=currentDevice.Get() ||
            width!=d.Width || height!=d.Height || format!=d.Format) {
            Reset(); selected=reinterpret_cast<uintptr_t>(sc); device=currentDevice;
            width=d.Width;height=d.Height;format=d.Format;
            auto rd=d;rd.BindFlags=0;rd.MiscFlags=0;rd.CPUAccessFlags=0;rd.Usage=D3D11_USAGE_DEFAULT;
            if(FAILED(device->CreateTexture2D(&rd,nullptr,&real))) {Off("Cannot retain the real LS frame");return S_OK;}
            const auto runtime=(std::filesystem::path(directory)/L"runtime").wstring();
            if(!std::filesystem::exists(std::filesystem::path(runtime)/L"nvngx_dlssg.dll")) {
                Off("Put your compatible nvngx_dlssg.dll in addon runtime; restart LS");return S_OK;
            }
            backend=std::make_unique<fg::Backend>();
            if(!backend->Init(device.Get(),d,runtime,Log)) {Off("DLSS FG unavailable; see manager Logs. LS frames pass through");return S_OK;}
        }
        const double arrived=Now();
        const bool timingReady=timeline.Observe(arrived);
        if(!timingReady) resetHistory=true;
        ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
        context->CopyResource(real.Get(),back.Get());
        const double start=Now();
        const auto result=backend->Generate(back.Get(),resetHistory);
        const double generateMs=(Now()-start)*1000;
        ++baseFrames;
        host->PublishMetric(id,"generation_ms",generateMs,"ms");
        if(result==fg::Result::Failed) {Off("DLSS FG failed; LS output preserved until restart");return S_OK;}
        resetHistory=false;
        if(result==fg::Result::HistoryOnly || !timingReady) {
            Status("DLSS FG: priming history / interpolation disabled by runtime",2);return S_OK;
        }
        const double step=timeline.Step(targetFps);
        // Preserve LS's chosen sync interval and present flags. With vsync,
        // Present already waits; adding a CPU half-frame wait would double pace.
        const double due=timeline.GeneratedDue(Now(),step);
        if(sync==0) WaitUntil(due);
        context->CopyResource(back.Get(),backend->Output()); context->Flush();
        const HRESULT made=PresentHook::PresentOriginal(sc,sync,flags);
        const double presented=Now();
        // Get the NEXT buffer after flip: the old pointer is no longer the
        // buffer LS's outer Present will submit.
        back.Reset();
        if(FAILED(sc->GetBuffer(0,IID_PPV_ARGS(&back)))) {
            handled=true; Off("Cannot restore real frame after flip; restart LS"); return FAILED(made)?made:E_FAIL;
        }
        context->CopyResource(back.Get(),real.Get()); context->Flush();
        if(made!=S_OK) {
            // Occluded/device lost/nonblocking statuses are never counted as FG.
            resetHistory=true; timeline.Reset(); return S_OK;
        }
        ++generated;
        const double realDue=timeline.RealDue(presented,step);
        if(sync==0) WaitUntil(realDue);
        char message[160]; std::snprintf(message,sizeof message,"DLSS FG x2: %llu generated; %.2f ms; %s",
            static_cast<unsigned long long>(generated),generateMs,sync?"LS vsync":targetFps?"136/profile pacing":"adaptive pacing");
        Status(message,1);
        host->PublishMetric(id,"real_interval_ms",timeline.Interval()*1000,"ms");
        host->PublishMetric(id,"generated_total",double(generated),"frames");
        // Let LS's original Present/Present1 finish the restored real frame.
        return S_OK;
    } catch(...) { Off("Addon exception; passing original LS frames through"); return S_OK; }
}
void Install(ID3D11Device* d) {
    if(d && !PresentHook::Installed() && !PresentHook::Install(d,OnPresent,Log)) Off("Could not install LS Present hook");
}
void PostDispatch(uint32_t,uint32_t,uint32_t,void*) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!host || !armed || disabled) return;
    auto* ctx=static_cast<ID3D11DeviceContext*>(host->GetDispatchingContext());
    if(!ctx) return;
    // This pass writes a backbuffer through a UAV. Querying its surface parent
    // identifies the output without undocumented window titles or focus tricks.
    ID3D11UnorderedAccessView* views[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
    ctx->CSGetUnorderedAccessViews(0,D3D11_PS_CS_UAV_REGISTER_COUNT,views);
    for(auto* view:views) {
        if(!view) continue;
        ComPtr<ID3D11Resource> resource; view->GetResource(&resource); view->Release();
        ComPtr<IDXGISurface> surface; ComPtr<IDXGISwapChain> chain;
        if(SUCCEEDED(resource.As(&surface)) && SUCCEEDED(surface->GetParent(IID_PPV_ARGS(&chain)))) {
            chain->SetPrivateData(outputTag,sizeof epoch,&epoch);
            ComPtr<ID3D11Device> d;ctx->GetDevice(&d);Install(d.Get());
        }
    }
}
void Event(uint32_t event,const void*,uint32_t,void*) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!host) return;
    if(event==EAM_EVENT_D3D11_DEVICE_CHANGED) {Reset();++epoch;disabled=false;}
    if(event==EAM_EVENT_D3D11_DEVICE_READY && armed) Install(static_cast<ID3D11Device*>(host->GetD3D11Device()));
    if(event==EAM_EVENT_SETTINGS_APPLIED) Settings();
}
}
EAM_EXPORT void AddonInitialize(IHost* h,ImGuiContext*,void*,void*,void*) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!h || h->GetHostVersion()<0x010100) return;
    host=h;
    HMODULE self=nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&AddonInitialize),&self);
    wchar_t path[32768]{}; GetModuleFileNameW(self,path,32768);
    directory=std::filesystem::path(path).parent_path().wstring();
    timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(!timer) timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    Settings();
    for(uint32_t e:{EAM_EVENT_D3D11_DEVICE_READY,EAM_EVENT_D3D11_DEVICE_CHANGED,EAM_EVENT_SETTINGS_APPLIED}) host->SubscribeEvent(e,Event,nullptr);
    host->SetPostDispatchCallback(PostDispatch,nullptr);
    if(armed) Install(static_cast<ID3D11Device*>(host->GetD3D11Device()));
    Log("LS_DLSSFG 0.1.0 loaded; native LSFG must be OFF; existing LS window/input retained");
}
EAM_EXPORT void AddonShutdown() {
    std::lock_guard<std::mutex> lock(mutex);
    PresentHook::Uninstall();
    if(host) {
        host->SetPostDispatchCallback(nullptr,nullptr);
        for(uint32_t e:{EAM_EVENT_D3D11_DEVICE_READY,EAM_EVENT_D3D11_DEVICE_CHANGED,EAM_EVENT_SETTINGS_APPLIED}) host->UnsubscribeEvent(e,Event);
        host->SetStatus(id,"",0);
    }
    Reset(); if(timer) {CloseHandle(timer);timer=nullptr;} host=nullptr;armed=false;
}
EAM_EXPORT uint32_t GetAddonCapabilities() {return EAM_CAP_REQUIRES_RESTART|EAM_CAP_D3D11_DEVICE_ACCESS|EAM_CAP_DISPATCH_HOOK;}
EAM_EXPORT const char* GetAddonName() {return "DLSS Frame Generation (experimental)";}
EAM_EXPORT const char* GetAddonVersion() {return "0.1.0";}
EAM_EXPORT const char* GetAddonAuthor() {return "Anton / Magpie experiments";}
EAM_EXPORT const char* GetAddonDescription() {return "DLSS FG x2 on LS output, keeping LS input and window. Disable native LSFG first.";}
