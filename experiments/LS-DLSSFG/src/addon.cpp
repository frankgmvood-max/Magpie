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
#include <stdexcept>
#include <shlwapi.h>
#include <dxgi1_5.h>

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
bool armed=false, disabled=false, resetHistory=true, callbacksReady=false;
bool preferVRR=true, presentationReported=false;
double targetFps=136;
const char* volatile startupStage="not_started";

// Win32-only breadcrumbs survive faults in the host ABI or C++ initialization.
// They do not depend on IHost, the GPU, writable Steam folders, or C++ locks.
void StartupTrace(const char* message) {
    OutputDebugStringA(message); OutputDebugStringA("\n");
    wchar_t base[MAX_PATH]{},folder[MAX_PATH]{},path[MAX_PATH]{};
    const DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH);
    if(!length || length>=MAX_PATH || !PathCombineW(folder,base,L"LS-DLSSFG")) return;
    CreateDirectoryW(folder,nullptr);
    if(!PathCombineW(path,folder,L"startup.log")) return;
    HANDLE file=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return;
    SYSTEMTIME now{};GetLocalTime(&now);
    char line[768];const int n=std::snprintf(line,sizeof line,"%04u-%02u-%02u %02u:%02u:%02u pid=%lu v0.1.2 %s\r\n",
        now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,GetCurrentProcessId(),message);
    DWORD written=0;if(n>0 && n<int(sizeof line)) WriteFile(file,line,DWORD(n),&written,nullptr);
    CloseHandle(file);
}
void StartupStep(const char* step) {startupStage=step;StartupTrace(step);}
LONG StartupFault(EXCEPTION_POINTERS* fault) {
    char message[512];
    const auto* record=fault->ExceptionRecord;
    std::snprintf(message,sizeof message,"fault stage=%s code=0x%08lx address=%p detail=%llu:%llu",
        startupStage,record->ExceptionCode,record->ExceptionAddress,
        record->NumberParameters>0?static_cast<unsigned long long>(record->ExceptionInformation[0]):0,
        record->NumberParameters>1?static_cast<unsigned long long>(record->ExceptionInformation[1]):0);
    StartupTrace(message);
    // Preserve the manager's Faulted indication instead of hiding a failed init.
    return EXCEPTION_CONTINUE_SEARCH;
}

double Now() { LARGE_INTEGER n{}, f{}; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f); return double(n.QuadPart)/f.QuadPart; }
void Log(const char* text) { if(host) host->Log(EAM_LOG_INFO,text); }
void Status(const char* text,int level) { if(host && GetTickCount64()-statusAt>=500) {statusAt=GetTickCount64();host->SetStatus(id,text,level);} }
void Off(const char* why) {
    disabled=true; resetHistory=true;
    Log(why); if(host) host->SetStatus(id,why,3);
}
void Reset() {
    backend.reset(); real.Reset(); device.Reset(); selected=0;
    width=height=0; format=DXGI_FORMAT_UNKNOWN; timeline.Reset(); resetHistory=true; presentationReported=false;
}
void Settings() {
    const auto ini=(std::filesystem::path(directory)/L"LS_DLSSFG.ini").wstring();
    armed=GetPrivateProfileIntW(L"FrameGeneration",L"NativeLSFGDisabled",0,ini.c_str())==1;
    // Missing key in an existing 0.1.1 INI enables the new policy on update.
    preferVRR=GetPrivateProfileIntW(L"FrameGeneration",L"PreferVRR",1,ini.c_str())==1;
    targetFps=GetPrivateProfileIntW(L"FrameGeneration",L"TargetFPS",136,ini.c_str());
    if (targetFps!=0 && (targetFps<30 || targetFps>240)) targetFps=136;
    disabled=false; Reset(); ++epoch;
}
void SettingsStatus(IHost* h) {
    h->SetStatus(id,armed?"Ready; waiting for LS output compute pass":"Disable native LSFG, then run Activate-136FPS.cmd",armed?0:2);
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
HRESULT OnPresent(IDXGISwapChain* sc,UINT& sync,UINT& flags,bool& handled) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!callbacksReady || !host || !armed || disabled || !fg::SafePresent(sync,flags)) return S_OK;
    uint64_t tag=0; UINT size=sizeof tag;
    // Only a swap chain actually written by an LS compute pass is eligible.
    // Manager/UI swap chains never acquire this tag, even when maximized.
    if(FAILED(sc->GetPrivateData(outputTag,&size,&tag)) || size!=sizeof tag || tag!=epoch) return S_OK;
    const UINT originalSync=sync, originalFlags=flags;
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
        BOOL fullscreen=TRUE;
        const bool windowed=SUCCEEDED(sc->GetFullscreenState(&fullscreen,nullptr)) && !fullscreen;
        const auto mode=fg::ChoosePresent(sync,flags,preferVRR,
            (sd.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)!=0,windowed);
        if(!presentationReported) {
            char report[256];std::snprintf(report,sizeof report,
                "Output Present: LS sync=%u flags=0x%x; chain flags=0x%x; windowed=%d; FG sync=%u flags=0x%x; VRR request=%s (monitor activation not measured)",
                sync,flags,sd.Flags,int(windowed),mode.sync,mode.flags,mode.vrrRequested?"yes":"no");
            Log(report);presentationReported=true;
        }
        // Both generated AND outer real-frame Present receive the same policy.
        // The hook forwards these references, including the Present1 path.
        sync=mode.sync;flags=mode.flags;
        const double step=timeline.Step(targetFps);
        // CPU spacing applies to the effective sync=0 VRR path. With sync=1,
        // Present already waits; an extra half-frame wait would double pace.
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
            sync=originalSync;flags=originalFlags;
            resetHistory=true; timeline.Reset(); return S_OK;
        }
        ++generated;
        const double realDue=timeline.RealDue(presented,step);
        if(sync==0) WaitUntil(realDue);
        char message[192]; std::snprintf(message,sizeof message,"DLSS FG x2: %llu generated; %.2f ms; %s; %s",
            static_cast<unsigned long long>(generated),generateMs,sync?"LS vsync":targetFps?"profile pacing":"adaptive pacing",
            mode.vrrRequested?"VRR-compatible Present":preferVRR?"VRR request unavailable on LS output":"LS Present preserved");
        Status(message,1);
        host->PublishMetric(id,"real_interval_ms",timeline.Interval()*1000,"ms");
        host->PublishMetric(id,"generated_total",double(generated),"frames");
        // Let LS's original Present/Present1 finish the restored real frame.
        return S_OK;
    } catch(...) { sync=originalSync;flags=originalFlags;Off("Addon exception; passing original LS frames through"); return S_OK; }
}
void Install(IDXGISwapChain* chain) {
    if(chain && !PresentHook::Installed() && !PresentHook::Install(chain,OnPresent,Log)) Off("Could not install LS Present hook");
}
void PostDispatch(uint32_t,uint32_t,uint32_t,void*) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!callbacksReady || !host || !armed || disabled) return;
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
            Install(chain.Get());
        }
    }
}
void Event(uint32_t event,const void*,uint32_t,void*) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!callbacksReady || !host) return;
    if(event==EAM_EVENT_D3D11_DEVICE_CHANGED) {Reset();++epoch;disabled=false;}
    // DEVICE_READY supplies no owned device. Hook only from a live compute pass.
    if(event==EAM_EVENT_SETTINGS_APPLIED) {Settings();SettingsStatus(host);}
}
void Initialize(IHost* h) {
    StartupStep("host_api");
    if(!h || h->GetHostVersion()<0x010100) return;
    StartupStep("local_state");
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(host) return; // manager initialization must be idempotent
        host=h;callbacksReady=false;
        HMODULE self=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Initialize),&self)) throw std::runtime_error("Cannot locate addon module");
        wchar_t path[32768]{};
        const DWORD length=GetModuleFileNameW(self,path,32768);
        if(!length || length>=32768) throw std::runtime_error("Cannot locate addon directory");
        directory=std::filesystem::path(path).parent_path().wstring();
        timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        if(!timer) timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
        Settings();
    }
    // Host callback registration takes its own locks. Never hold our mutex:
    // a render callback can arrive concurrently and need that same mutex.
    for(uint32_t e:{EAM_EVENT_D3D11_DEVICE_READY,EAM_EVENT_D3D11_DEVICE_CHANGED,EAM_EVENT_SETTINGS_APPLIED}) {
        StartupStep("subscribe_event");h->SubscribeEvent(e,Event,nullptr);
    }
    StartupStep("register_dispatch");h->SetPostDispatchCallback(PostDispatch,nullptr);
    StartupStep("initial_status");SettingsStatus(h);
    StartupStep("initial_log");h->Log(EAM_LOG_INFO,"LS_DLSSFG 0.1.2 initialized without GPU access; waiting for a live LS output pass");
    {
        std::lock_guard<std::mutex> lock(mutex);callbacksReady=true;
    }
    StartupStep("initialization_complete");
}
}
EAM_EXPORT void AddonInitialize(IHost* h,ImGuiContext*,void*,void*,void*) {
    // No C++ objects here: SEH reports a failure, then the manager handles it.
    __try {Initialize(h);}
    __except(StartupFault(GetExceptionInformation())) {}
}
EAM_EXPORT void AddonShutdown() {
    IHost* h=nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);callbacksReady=false;h=host;
    }
    PresentHook::Uninstall();
    if(h) {
        h->SetPostDispatchCallback(nullptr,nullptr);
        for(uint32_t e:{EAM_EVENT_D3D11_DEVICE_READY,EAM_EVENT_D3D11_DEVICE_CHANGED,EAM_EVENT_SETTINGS_APPLIED}) h->UnsubscribeEvent(e,Event);
        h->SetStatus(id,"",0);
    }
    std::lock_guard<std::mutex> lock(mutex);
    Reset(); if(timer) {CloseHandle(timer);timer=nullptr;} host=nullptr;armed=false;
}
EAM_EXPORT uint32_t GetAddonCapabilities() {return EAM_CAP_REQUIRES_RESTART|EAM_CAP_D3D11_DEVICE_ACCESS|EAM_CAP_DISPATCH_HOOK;}
EAM_EXPORT const char* GetAddonName() {return "DLSS Frame Generation (experimental)";}
EAM_EXPORT const char* GetAddonVersion() {return "0.1.2";}
EAM_EXPORT const char* GetAddonAuthor() {return "Anton / Magpie experiments";}
EAM_EXPORT const char* GetAddonDescription() {return "DLSS FG x2 on LS output, keeping LS input and window. Disable native LSFG first.";}
