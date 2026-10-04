// SPDX-License-Identifier: MIT
#include <eam/addon_sdk.h>
#include "backend.h"
#include "present_hook.h"
#include "policy.h"
#include "output_state.h"
#include "recovery.h"
#include <generation_lease.h>
#include <wrl/client.h>
#include <filesystem>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <shlwapi.h>
#include <dxgi1_5.h>
#include <imgui.h>
#include <eam/widgets.h>
#include <cwchar>

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
fg::Recovery recovery;
ls::GenerationLease generationLease;
bool resetPending=false,automaticRecovery=true;
unsigned recoveryAttempts=0;
uint64_t recoveryWindow=0;
fg::OutputQueue outputQueue;
fg::GSyncProbe gsyncProbe;
fg::GSyncState gsyncState;
uint64_t probeAt=0;
unsigned maximumFrameLatency=1;
fg::Settings configuration;
bool settingsPending=false,uiReady=false;
uint64_t configurationRevision=0,duplicates=0;
HANDLE latencyWait=nullptr;
double latestGenerationMs=0,latestPreprocessMs=0;
unsigned effectiveMultiplier=2,runtimeMultiplier=2,actualFlowQuality=0;
bool lastRealMotion=false;
uintptr_t selected=0;
uint64_t epoch=1, statusAt=0, generated=0, baseFrames=0;
UINT width=0,height=0;
DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
HANDLE timer=nullptr;
bool armed=false, disabled=false, resetHistory=true, callbacksReady=false;
bool preferVRR=true, presentationReported=false;
enum class PendingOutput {None,Duplicate,History,Generated};
PendingOutput pendingOutput=PendingOutput::None;
UINT pendingSync=0;
bool pendingVRR=false;
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
    char line[768];const int n=std::snprintf(line,sizeof line,"%04u-%02u-%02u %02u:%02u:%02u pid=%lu v0.3.0 %s\r\n",
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
void Off(const char* why) noexcept {
    disabled=true; resetHistory=true;pendingOutput=PendingOutput::None;
    outputQueue.Reset();
    generationLease.Reset();
    try {Log(why);} catch(...) {StartupTrace(why);}
    try {if(host) host->SetStatus(id,why,3);} catch(...) {}
}
void Reset() {
    backend.reset();outputQueue.Reset();gsyncProbe.Reset();gsyncState={};probeAt=0;
    real.Reset(); device.Reset(); selected=0;
    if(latencyWait) {CloseHandle(latencyWait);latencyWait=nullptr;}
    effectiveMultiplier=runtimeMultiplier=2;actualFlowQuality=0;lastRealMotion=false;
    width=height=0; format=DXGI_FORMAT_UNKNOWN; timeline.Reset(); resetHistory=true; presentationReported=false;pendingOutput=PendingOutput::None;
    recovery.Reset();
}
void Settings() {
    const auto ini=(std::filesystem::path(directory)/L"LS_DLSSFG.ini").wstring();
    auto get=[&](const wchar_t* key,unsigned fallback){return GetPrivateProfileIntW(L"FrameGeneration",key,fallback,ini.c_str());};
    configuration.nativeFGDisabled=get(L"NativeLSFGDisabled",0)==1;
    configuration.multiplier=get(L"Multiplier",2);
    configuration.flow=static_cast<fg::FlowMethod>(get(L"OpticalFlowMethod",2));
    configuration.flowQuality=get(L"NvidiaOpticalFlowQuality",2);
    configuration.flowScale=get(L"OpticalFlowScale",50);
    configuration.duplicateFiltering=get(L"DuplicateFrameFiltering",1)==1;
    configuration.preferVRR=get(L"PreferVRR",1)==1;
    configuration.maximumFrameLatency=get(L"MaximumFrameLatency",1);
    wchar_t fps[64]{};GetPrivateProfileStringW(L"FrameGeneration",L"TargetFPS",L"136",fps,64,ini.c_str());
    configuration.targetFPS=fg::ParseTargetFPS(fps);
    configuration.presentApi=static_cast<fg::PresentApi>(get(L"GeneratedPresentAPI",0));
    configuration.Validate();
    automaticRecovery=get(L"AutomaticRecovery",1)==1;
    armed=configuration.nativeFGDisabled;preferVRR=configuration.preferVRR;
    maximumFrameLatency=configuration.maximumFrameLatency;targetFps=configuration.targetFPS;
    settingsPending=false;++configurationRevision;
    disabled=false; Reset();generationLease.Reset();resetPending=false; ++epoch;
}
void SettingsStatus(IHost* h) {
    h->SetStatus(id,armed?"Ready; waiting for LS output compute pass":"Disable native LSFG, then confirm in addon Settings",armed?0:2);
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
    if(settingsPending && callbacksReady && host) Settings();
    if(!callbacksReady || !host || !armed || disabled || !fg::SafePresent(sync,flags)) return S_OK;
    uint64_t tag=0;UINT size=sizeof tag;
    if(FAILED(sc->GetPrivateData(outputTag,&size,&tag)) || size!=sizeof tag || tag!=epoch) return S_OK;
    if(resetPending){Reset();disabled=false;resetPending=false;}
    if(!generationLease.Acquire()){Status("Another FG addon owns output; disable it before enabling DLSS FG",2);return S_OK;}
    pendingOutput=PendingOutput::None;
    const UINT originalSync=sync,originalFlags=flags;
    ComPtr<ID3D11DeviceContext> restoreContext;
    bool restoreNeeded=false;
    try {
        DXGI_SWAP_CHAIN_DESC sd{};
        if(FAILED(sc->GetDesc(&sd)) || sd.SampleDesc.Count!=1 ||
           (sd.SwapEffect!=DXGI_SWAP_EFFECT_FLIP_DISCARD && sd.SwapEffect!=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)) return S_OK;
        const double arrived=Now();
        const HWND root=sd.OutputWindow?GetAncestor(sd.OutputWindow,GA_ROOT):nullptr;
        const bool visible=root && IsWindow(root) && IsWindowVisible(root) && !IsIconic(root);
        const auto recoveryAction=recovery.Observe(arrived,reinterpret_cast<uintptr_t>(sd.OutputWindow),visible,true);
        if(recoveryAction==fg::RecoveryAction::Suspend){resetHistory=true;timeline.Reset();return S_OK;}
        if(automaticRecovery && recoveryAction==fg::RecoveryAction::Recreate){Reset();Log("FG recovery: output resumed / presentation gap; recreating NGX, NVOF and duplicate history");}
        ComPtr<ID3D11Device> currentDevice;ComPtr<ID3D11Texture2D> back;
        if(FAILED(sc->GetDevice(IID_PPV_ARGS(&currentDevice))) || FAILED(sc->GetBuffer(0,IID_PPV_ARGS(&back)))) return S_OK;
        D3D11_TEXTURE2D_DESC d{};back->GetDesc(&d);
        if(d.SampleDesc.Count!=1 || d.ArraySize!=1 || d.MipLevels!=1) return S_OK;
        if(d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT) {
            Status("Unsupported output format; use SDR or scRGB",2);return S_OK;
        }
        if(selected!=reinterpret_cast<uintptr_t>(sc) || device.Get()!=currentDevice.Get() || width!=d.Width || height!=d.Height || format!=d.Format) {
            Reset();selected=reinterpret_cast<uintptr_t>(sc);device=currentDevice;
            width=d.Width;height=d.Height;format=d.Format;
            auto rd=d;rd.BindFlags=0;rd.MiscFlags=0;rd.CPUAccessFlags=0;rd.Usage=D3D11_USAGE_DEFAULT;
            if(FAILED(device->CreateTexture2D(&rd,nullptr,&real))) {Off("Cannot retain the real LS frame");return S_OK;}
            const auto runtime=(std::filesystem::path(directory)/L"runtime").wstring();
            if(!std::filesystem::exists(std::filesystem::path(runtime)/L"nvngx_dlssg.dll")) {Off("Put your compatible nvngx_dlssg.dll in addon runtime; restart LS");return S_OK;}
            gsyncProbe.Init(device.Get(),Log);
            Log("G-SYNC telemetry runs after the outer LS Present; driver state is not a measurement of monitor refresh rate");
            backend=std::make_unique<fg::Backend>();
            if(!backend->Init(device.Get(),d,runtime,configuration,Log)) {Off("DLSS FG unavailable; see manager Logs. LS frames pass through");return S_OK;}
            effectiveMultiplier=backend->Multiplier();runtimeMultiplier=backend->MaxMultiplier();actualFlowQuality=backend->FlowQuality();
            if(maximumFrameLatency) {
                const bool applied=outputQueue.Set(sc,maximumFrameLatency);
                char q[160];std::snprintf(q,sizeof q,"Output queue: applied=%d previous=%u requested=%u api=%s; restored on stop/failure",int(applied),outputQueue.Previous(),maximumFrameLatency,outputQueue.PerSwapChain()?"swapchain":"device");Log(q);
            }
            if(sd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
                ComPtr<IDXGISwapChain2> chain2;
                if(SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&chain2)))) latencyWait=chain2->GetFrameLatencyWaitableObject();
            }
        }
        if(timeline.NeedsReset(arrived)) resetHistory=true;
        ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
        restoreContext=context;
        context->CopyResource(real.Get(),back.Get());
        const double start=Now();const auto result=backend->Generate(back.Get(),resetHistory);const double generateMs=(Now()-start)*1000;
        if(result==fg::Result::Duplicate) {
            ++duplicates;pendingOutput=PendingOutput::Duplicate;return S_OK;
        }
        const bool timingReady=timeline.Observe(arrived);++baseFrames;
        latestGenerationMs=generateMs;latestPreprocessMs=backend->PreprocessMilliseconds();actualFlowQuality=backend->FlowQuality();lastRealMotion=backend->RealMotion();
        if(result==fg::Result::Failed) {Off("DLSS FG failed; LS output preserved until settings reapply/restart");return S_OK;}
        if(automaticRecovery && recovery.RuntimeRejected(backend->OutputDisabledByRuntime())) {
            const auto tick=GetTickCount64();
            if(tick-recoveryWindow>60000){recoveryWindow=tick;recoveryAttempts=0;}
            if(recoveryAttempts<3){++recoveryAttempts;resetPending=true;Log("FG recovery: runtime rejected interpolation for 60 unique frames; scheduling feature recreation");}
        }
        resetHistory=false;
        if(result==fg::Result::HistoryOnly || !timingReady) {pendingOutput=PendingOutput::History;return S_OK;}
        BOOL fullscreen=TRUE;const bool windowed=SUCCEEDED(sc->GetFullscreenState(&fullscreen,nullptr)) && !fullscreen;
        const auto mode=fg::ChoosePresent(sync,flags,preferVRR,(sd.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)!=0,windowed);
        if(!presentationReported) {
            char report[256];
            std::snprintf(report,sizeof report,"Output Present: LS sync=%u flags=0x%x; chain flags=0x%x; windowed=%d; FG sync=%u flags=0x%x; VRR request=%s; requestedAPI=%u",sync,flags,sd.Flags,int(windowed),mode.sync,mode.flags,mode.vrrRequested?"yes":"no",unsigned(configuration.presentApi));Log(report);
            presentationReported=true;
        }
        sync=mode.sync;flags=mode.flags;const double step=timeline.Step(targetFps,effectiveMultiplier);
        HRESULT made=S_OK;double presented=0;unsigned submitted=0;
        for(unsigned index=0;index<backend->OutputCount();++index) {
            const double due=index?presented+step:timeline.GeneratedDue(Now(),step);
            if(sync==0) WaitUntil(due);
            if(latencyWait && WaitForSingleObject(latencyWait,500)!=WAIT_OBJECT_0) {made=E_FAIL;break;}
            back.Reset();if(FAILED(sc->GetBuffer(0,IID_PPV_ARGS(&back)))) {made=E_FAIL;break;}
            restoreNeeded=true;
            context->CopyResource(back.Get(),backend->Output(index));context->Flush();
            made=PresentHook::PresentOriginal(sc,sync,flags,configuration.presentApi);presented=Now();
            if(made!=S_OK) break;
            ++submitted;
        }
        generated+=submitted;
        // Every generated flip rotates the buffer. Restore the current buffer,
        // including on a partially submitted group, before LS's outer Present.
        back.Reset();if(FAILED(sc->GetBuffer(0,IID_PPV_ARGS(&back)))) {handled=true;Off("Cannot restore real frame after flip; restart LS");return FAILED(made)?made:E_FAIL;}
        context->CopyResource(back.Get(),real.Get());context->Flush();
        restoreNeeded=false;
        if(made!=S_OK) {
            sync=originalSync;flags=originalFlags;resetHistory=true;timeline.Reset();
            if(FAILED(made)) Off("Generated Present/queue failed; original LS frame restored");return S_OK;
        }
        const double realDue=presented+step;timeline.Finish(realDue,step);if(sync==0) WaitUntil(realDue);
        pendingOutput=PendingOutput::Generated;pendingSync=sync;pendingVRR=mode.vrrRequested;
        return S_OK;
    } catch(...) {
        sync=originalSync;flags=originalFlags;
        // A host logging/metric callback can throw after a generated flip.
        // Restore the newly current buffer before allowing LS's outer Present.
        if(restoreNeeded) {
            ComPtr<ID3D11Texture2D> current;
            if(!restoreContext || !real || FAILED(sc->GetBuffer(0,IID_PPV_ARGS(&current)))) {
                handled=true;Off("Addon exception; cannot restore real frame after flip");return E_FAIL;
            }
            restoreContext->CopyResource(current.Get(),real.Get());restoreContext->Flush();
        }
        Off("Addon exception; real LS frame restored and passed through");return S_OK;
    }
}

// Slow driver queries and host callbacks belong after the real Present. Keeping
// them between generated and real flips consumes the scheduled frame interval.
void AfterPresent(IDXGISwapChain* sc,HRESULT result) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        if(!callbacksReady || !host || disabled || selected!=reinterpret_cast<uintptr_t>(sc)) return;
        const auto pending=pendingOutput;pendingOutput=PendingOutput::None;
        if(pending==PendingOutput::None) return;
        if(result!=S_OK) {resetHistory=true;timeline.Reset();if(automaticRecovery)resetPending=true;return;}
        if(pending==PendingOutput::Duplicate) {
            host->PublishMetric(id,"duplicates_total",double(duplicates),"frames");
            Status("Duplicate LS frame: FG skipped; original LS output preserved",0);return;
        }
        if(GetTickCount64()>=probeAt) {
            probeAt=GetTickCount64()+2000;
            ComPtr<ID3D11Texture2D> back;
            gsyncState=SUCCEEDED(sc->GetBuffer(0,IID_PPV_ARGS(&back)))?gsyncProbe.Query(back.Get()):fg::GSyncState{};
            ComPtr<IDXGISwapChainMedia> media;DXGI_FRAME_STATISTICS_MEDIA stats{};DXGI_SWAP_CHAIN_DESC sd{};
            const HRESULT statsHr=SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&media)))?media->GetFrameStatisticsMedia(&stats):E_NOINTERFACE;
            sc->GetDesc(&sd);
            RECT windowRect{};MONITORINFO monitor{sizeof(MONITORINFO)};
            const bool covers=sd.OutputWindow && GetWindowRect(sd.OutputWindow,&windowRect) && GetMonitorInfoW(MonitorFromWindow(sd.OutputWindow,MONITOR_DEFAULTTONEAREST),&monitor) && EqualRect(&windowRect,&monitor.rcMonitor);
            char measured[640];std::snprintf(measured,sizeof measured,"FG output: G-SYNC=%s capable=%d statuses=%d/%d/%d; unique_interval_ms=%.3f generation_ms=%.3f preprocess_cpu_ms=%.3f multiplier=%u flowQuality=%u flowScale=%u duplicates=%llu; LS presentAPI=%s requestedAPI=%u; mediaHr=0x%08x composition=%u; fullscreenBounds=%d foregroundOutput=%d exStyle=0x%llx; queryAfterRealPresent=1 monitorHz=unmeasured",
                gsyncState.Label(),int(gsyncState.capable),gsyncState.handleStatus,gsyncState.capableStatus,gsyncState.activeStatus,timeline.Interval()*1000,latestGenerationMs,latestPreprocessMs,effectiveMultiplier,actualFlowQuality,configuration.flowScale,static_cast<unsigned long long>(duplicates),PresentHook::UsesPresent1()?"Present1":"Present",unsigned(configuration.presentApi),unsigned(statsHr),SUCCEEDED(statsHr)?unsigned(stats.CompositionMode):~0u,int(covers),int(GetForegroundWindow()==sd.OutputWindow),static_cast<unsigned long long>(GetWindowLongPtrW(sd.OutputWindow,GWL_EXSTYLE)));Log(measured);
        }
        host->PublishMetric(id,"generation_ms",latestGenerationMs,"ms");host->PublishMetric(id,"preprocess_cpu_ms",latestPreprocessMs,"ms");
        if(pending==PendingOutput::History) {Status("DLSS FG: priming history / interpolation disabled by runtime",2);return;}
        const double step=timeline.Step(targetFps,effectiveMultiplier);
        // The current synchronous path must finish inference before presenting
        // any intermediates. A target step is a budget, not achieved throughput.
        const bool overBudget=pendingSync==0 && latestGenerationMs>step*1000;
        char message[320];std::snprintf(message,sizeof message,"DLSS FG x%u: %llu submitted; %.2f ms; OF=%s; %s; %s; driver G-SYNC %s%s",effectiveMultiplier,static_cast<unsigned long long>(generated),latestGenerationMs,actualFlowQuality?"NVOF":"None",pendingSync?"LS vsync":targetFps?"profile pacing":"adaptive pacing",pendingVRR?"VRR-compatible Present":preferVRR?"VRR request unavailable":"LS Present preserved",gsyncState.Label(),overBudget?"; processing exceeds output-step budget":"");Status(message,overBudget?2:1);
        host->PublishMetric(id,"real_interval_ms",timeline.Interval()*1000,"ms");host->PublishMetric(id,"generated_total",double(generated),"submitted frames");
    } catch(...) {std::lock_guard<std::mutex> lock(mutex);Off("Post-Present telemetry failed; original LS frame was already submitted");}
}

void Install(IDXGISwapChain* chain) {
    if(chain && !PresentHook::Install(chain,OnPresent,Log,AfterPresent)) Off("Could not install LS Present hook on current output table");
}
void PostDispatch(uint32_t,uint32_t,uint32_t,void*) {
    std::lock_guard<std::mutex> lock(mutex);
    if(settingsPending && callbacksReady && host)Settings();
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
    // READY is also sent when a COM address is reused: CHANGED is absent in
    // that case. Defer GPU destruction to the next tagged presentation.
    if(event==EAM_EVENT_D3D11_DEVICE_READY || event==EAM_EVENT_D3D11_DEVICE_CHANGED) {resetPending=true;++epoch;disabled=false;}
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
    StartupStep("initial_log");h->Log(EAM_LOG_INFO,"LS_DLSSFG 0.3.0 initialized without GPU access; waiting for a live LS output pass");
    {
        std::lock_guard<std::mutex> lock(mutex);callbacksReady=true;
    }
    StartupStep("initialization_complete");
}
}
EAM_EXPORT void AddonInitialize(IHost* h,ImGuiContext* ctx,void* alloc,void* free,void* user) {
    // No C++ objects here: SEH reports a failure, then the manager handles it.
    __try {
        if(ctx && alloc && free) {
            ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(alloc),reinterpret_cast<ImGuiMemFreeFunc>(free),user);
            ImGui::SetCurrentContext(ctx);eam::ui::InitAddonImGui();uiReady=true;
        } else uiReady=false;
        Initialize(h);
    }
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
    Reset();generationLease.Reset(); if(timer) {CloseHandle(timer);timer=nullptr;} host=nullptr;armed=false;
}
EAM_EXPORT uint32_t GetAddonCapabilities() {return EAM_CAP_HAS_SETTINGS|EAM_CAP_REQUIRES_RESTART|EAM_CAP_D3D11_DEVICE_ACCESS|EAM_CAP_DISPATCH_HOOK;}
EAM_EXPORT const char* GetAddonName() {return "DLSS Frame Generation (experimental)";}
EAM_EXPORT const char* GetAddonVersion() {return "0.3.0";}
EAM_EXPORT const char* GetAddonAuthor() {return "Anton / Magpie experiments";}
EAM_EXPORT const char* GetAddonDescription() {return "DLSS FG x2-x4, NVOF and Duplicate Frame Filtering on LS output. Disable native LSFG first.";}

EAM_EXPORT void AddonRenderSettings() {
    if(!uiReady) return;
    static fg::Settings draft;
    static uint64_t seen=UINT64_MAX;
    static bool dirty=false,draftRecovery=true;
    fg::Settings current;uint64_t revision=0,duplicateCount=0,submitted=0;
    unsigned effective=2,maximum=2,quality=0;
    bool active=false,motion=false,currentRecovery=true;
    double genMs=0,preMs=0,interval=0;
    fg::GSyncState gsync;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(!host) return;
        currentRecovery=automaticRecovery;current=configuration;revision=configurationRevision;duplicateCount=duplicates;submitted=generated;
        effective=effectiveMultiplier;maximum=runtimeMultiplier;quality=actualFlowQuality;motion=lastRealMotion;
        active=backend && !disabled;genMs=latestGenerationMs;preMs=latestPreprocessMs;interval=timeline.Interval();gsync=gsyncState;
    }
    if(seen!=revision && !dirty) {draft=current;draftRecovery=currentRecovery;seen=revision;}
    ImGui::TextWrapped("DLSS FG uses the LS output window and GPU. Turn native LSFG OFF before enabling this addon. Your compatible runtime/mod stays unchanged.");
    dirty|=ImGui::Checkbox("I have disabled native LS frame generation",&draft.nativeFGDisabled);
    int mult=int(draft.multiplier)-2;
    if(ImGui::Combo("Frame Multiplier",&mult,"2x\0 3x\0 4x\0")) {draft.multiplier=unsigned(mult+2);dirty=true;}
    ImGui::TextWrapped("Runtime capability is checked on start. Unsupported multipliers fall back to the reported maximum. Reset frames always use 1/1 internally.");
    if(active) ImGui::Text("Active: x%u; runtime maximum: x%u",effective,maximum);
    int flow=draft.flow==fg::FlowMethod::Nvidia?1:0;
    if(ImGui::Combo("Optical Flow Method",&flow,"None (zero motion)\0NVIDIA NVOF\0")) {draft.flow=flow?fg::FlowMethod::Nvidia:fg::FlowMethod::None;dirty=true;}
    if(draft.flow==fg::FlowMethod::Nvidia) {
        int q=int(draft.flowQuality)-1;
        if(ImGui::Combo("OF Quality",&q,"Performance (4x4 Fast)\0Balanced (4x4 Medium)\0Quality (4x4 Slow)\0High Quality (2x2 Medium)\0Highest Quality (2x2 Slow)\0")) {draft.flowQuality=unsigned(q+1);dirty=true;}
        int scale=int(draft.flowScale/25)-1;
        if(ImGui::Combo("OF Analysis Resolution",&scale,"25%\0 50% (recommended)\0 75%\0 100%\0")) {draft.flowScale=unsigned(scale+1)*25;dirty=true;}
        ImGui::TextWrapped("Only the motion-analysis image is reduced. Output resolution stays unchanged; vectors are scaled to output pixels. 50% uses one quarter of the analysis pixels. Try Performance at 50% if OF causes stutter.");
        ImGui::TextWrapped("NVOF estimates screenshot motion; engine depth is unavailable. Higher quality adds cost. Unsupported 2x2 profiles fall back to the matching 4x4 preset; unavailable NVOF falls back to None and is reported.");
    }
    dirty|=ImGui::Checkbox("Duplicate Frame Filtering (exact RGB)",&draft.duplicateFiltering);
    ImGui::TextWrapped("Matches all RGB pixels at the LS output. Repeated frames skip OF/FG without suppressing LS's original Present or cursor handling. Animated overlays and a captured cursor can count as changes. This check adds GPU/readback cost.");
    bool automatic=draft.targetFPS==0;
    if(ImGui::Checkbox("Adaptive output pacing",&automatic)) {draft.targetFPS=automatic?0:136;dirty=true;}
    if(!automatic) {
        float fps=float(draft.targetFPS);
        if(ImGui::InputFloat("Target output FPS",&fps,1,10,"%.2f")) {draft.targetFPS=fps;dirty=true;}
        ImGui::Text("Suggested game cap: %.2f FPS",draft.targetFPS/draft.multiplier);
    }
    dirty|=ImGui::Checkbox("Request windowed VRR-compatible Present",&draft.preferVRR);
    ImGui::TextWrapped("Uses sync=0 and ALLOW_TEARING only where legal. This is a presentation request, not a switch that forces G-SYNC or Independent Flip.");
    int latency=int(draft.maximumFrameLatency);
    if(ImGui::SliderInt("Maximum Frame Latency (0 = keep LS)",&latency,0,3)) {draft.maximumFrameLatency=unsigned(latency);dirty=true;}
    int api=int(draft.presentApi);
    if(ImGui::Combo("Generated frame Present API",&api,"Follow LS (recommended)\0Present\0Present1\0")) {draft.presentApi=static_cast<fg::PresentApi>(api);dirty=true;}
    ImGui::TextWrapped("Alternate API choices affect full generated frames only. LS's original Present/Present1 and partial updates remain intact.");
    dirty|=ImGui::Checkbox("Automatic recovery after minimize / runtime stalls",&draftRecovery);
    if(ImGui::Button("Reinitialize generator now")) {std::lock_guard<std::mutex> lock(mutex);disabled=false;resetPending=true;}
    if(ImGui::Button("136 FPS profile")) {
        const bool confirmed=draft.nativeFGDisabled;draft={};draft.nativeFGDisabled=confirmed;dirty=true;
    }
    ImGui::SameLine();
    if(ImGui::Button("Reload saved settings")) {draft=current;draftRecovery=currentRecovery;seen=revision;dirty=false;}
    if(ImGui::Button("Save and apply")) {
        draft.Validate();
        std::lock_guard<std::mutex> lock(mutex);
        const auto ini=(std::filesystem::path(directory)/L"LS_DLSSFG.ini").wstring();
        bool ok=true;
        auto save=[&](const wchar_t* key,unsigned value){wchar_t text[32]{};std::swprintf(text,32,L"%u",value);ok=WritePrivateProfileStringW(L"FrameGeneration",key,text,ini.c_str()) && ok;};
        save(L"AutomaticRecovery",draftRecovery);save(L"NativeLSFGDisabled",draft.nativeFGDisabled);save(L"Multiplier",draft.multiplier);
        save(L"OpticalFlowMethod",unsigned(draft.flow));save(L"NvidiaOpticalFlowQuality",draft.flowQuality);
        save(L"OpticalFlowScale",draft.flowScale);
        save(L"DuplicateFrameFiltering",draft.duplicateFiltering);save(L"PreferVRR",draft.preferVRR);
        save(L"MaximumFrameLatency",draft.maximumFrameLatency);save(L"GeneratedPresentAPI",unsigned(draft.presentApi));
        wchar_t fps[64]{};std::swprintf(fps,64,L"%.3f",draft.targetFPS);ok=WritePrivateProfileStringW(L"FrameGeneration",L"TargetFPS",fps,ini.c_str()) && ok;
        if(ok) {settingsPending=true;dirty=false;seen=UINT64_MAX;Status("Settings saved; will apply at the next LS output frame",0);}
        else {Status("Cannot save addon INI; check folder write permissions",3);}
    }
    if(dirty) ImGui::TextDisabled("Unsaved changes");
    ImGui::Separator();
    ImGui::Text("G-SYNC driver query: %s",gsync.Label());
    ImGui::Text("NVOF effective quality: %u; motion: %s",quality,motion?"estimated":"zero / history reset");
    if(quality) ImGui::Text("OF analysis scale: %u%%",current.flowScale);
    ImGui::Text("Generation: %.2f ms; preprocessing CPU submit/check: %.2f ms",genMs,preMs);
    ImGui::Text("Unique LS-input interval: %.2f ms; duplicates filtered: %llu",interval*1000,static_cast<unsigned long long>(duplicateCount));
    ImGui::Text("Generated frames submitted: %llu",static_cast<unsigned long long>(submitted));
    ImGui::TextWrapped("Submitted frames are not a measurement of monitor scanout. The manager Performance tab contains the timing series. Keep the manager window/overlays out of the game area when checking VRR.");
}
