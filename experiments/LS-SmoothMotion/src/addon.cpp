// SPDX-License-Identifier: MIT
#include <eam/addon_sdk.h>
#include <eam/widgets.h>
#include <imgui.h>
#include <ls_output_bridge.h>
#include <generation_lease.h>
#include <presentation.h>
#include "backend.h"
#include "driver.h"
#include <filesystem>
#include <mutex>
#include <cwchar>
#include <cstdio>
#include <cmath>
#include <nvs30/nvpresent.hpp>
namespace {
constexpr const char* id="LS_SmoothMotion";
constexpr GUID tagGuid={0x9fd96cc2,0x3d35,0x4028,{0x84,0xc6,0x27,0x75,0xa7,0x0f,0xcd,0x11}};
std::mutex mutex;IHost* host=nullptr;std::wstring directory,driverPath;
sm::Settings settings;ls::GenerationLease lease;std::unique_ptr<sm::Backend> backend;
ls::ComPtr<ID3D11Device> device;uintptr_t selected=0;UINT width=0,height=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
uint64_t epoch=1,revision=0,statusAt=0,lastFrameAt=0,submitted=0,duplicates=0;
bool ready=false,ui=false,pendingSettings=false,resetPending=false,disabled=false,plainFallback=false,shutdownQuarantined=false;
unsigned recoveryAttempts=0;uint64_t recoveryWindow=0;
double milliseconds=0;UINT currentSync=0;
void Log(const char* text){if(host)host->Log(EAM_LOG_INFO,text);}
void WINAPI BridgeLog(const char* text,void*){Log(text);}
void Status(const char* text,int level){if(host && GetTickCount64()-statusAt>=500){statusAt=GetTickCount64();host->SetStatus(id,text,level);}}
void Reset(){if(backend)backend->Hide();backend.reset();device.Reset();selected=0;width=height=0;format=DXGI_FORMAT_UNKNOWN;lastFrameAt=0;}
void Read(){
    const auto path=(std::filesystem::path(directory)/L"LS_SmoothMotion.ini").wstring();
    auto get=[&](const wchar_t* key,unsigned d){return GetPrivateProfileIntW(L"SmoothMotion",key,d,path.c_str());};
    const bool wasEnabled=settings.enabled;const std::wstring oldPath=driverPath;
    settings.enabled=get(L"Enabled",0)==1;settings.nativeFGDisabled=get(L"NativeLSFGDisabled",0)==1;
    settings.preferVRR=get(L"PreferVRR",1)==1;settings.filterDuplicates=get(L"DuplicateFrameFiltering",1)==1;
    settings.preserveLsPresent=get(L"PreserveLSPresent",0)==1;settings.diagnostics=get(L"Diagnostics",0)==1;
    settings.automaticRecovery=get(L"AutomaticRecovery",1)==1;settings.syncMode=get(L"SyncMode",0);settings.devicePath=get(L"DeviceCreationPath",0);
    wchar_t fps[64]{};GetPrivateProfileStringW(L"SmoothMotion",L"TargetOutputFPS",L"136",fps,64,path.c_str());const float parsed=std::wcstof(fps,nullptr);settings.targetFPS=std::isfinite(parsed)?parsed:136;
    wchar_t runtime[32768]{};GetPrivateProfileStringW(L"SmoothMotion",L"NvPresentPath",L"",runtime,32768,path.c_str());driverPath=runtime;settings.Validate();
    Reset();if((wasEnabled && !settings.enabled) || oldPath!=driverPath)sm::StopDriver();
    lease.Reset();plainFallback=false;disabled=false;pendingSettings=false;resetPending=false;++revision;++epoch;
}
bool Tagged(IDXGISwapChain* sc){uint64_t tag=0;UINT n=sizeof tag;return SUCCEEDED(sc->GetPrivateData(tagGuid,&n,&tag)) && n==sizeof tag && tag==epoch;}
void Fail(const char* why){disabled=true;Reset();Log(why);Status(why,3);lease.Reset();sm::StopDriver();}
bool ScheduleRecovery(const char* reason){
    if(!settings.automaticRecovery)return false;
    const auto now=GetTickCount64();if(now-recoveryWindow>60000){recoveryWindow=now;recoveryAttempts=0;}
    if(recoveryAttempts>=3)return false;
    ++recoveryAttempts;resetPending=true;if(backend)backend->Hide();Log(reason);return true;
}
HRESULT WINAPI Before(IDXGISwapChain* sc,UINT* sync,UINT* flags,BOOL* handled,LsBridgeFrame* frame,void*){
    std::lock_guard<std::mutex> lock(mutex);
    if(!ready || !host || !settings.enabled || !settings.nativeFGDisabled || disabled || !ls::LegalPresent(*flags) || !Tagged(sc))return S_OK;
    if(!lease.Acquire()){Status("DLSS FG owns generation; disable it before enabling Smooth Motion",2);return S_OK;}
    try {
        DXGI_SWAP_CHAIN_DESC sd{};if(FAILED(sc->GetDesc(&sd)))return S_OK;
        const HWND root=GetAncestor(sd.OutputWindow,GA_ROOT);
        if(!root || !IsWindowVisible(root) || IsIconic(root)){if(backend)backend->Hide();resetPending=true;return S_OK;}
        const auto now=GetTickCount64();if(settings.automaticRecovery && lastFrameAt && now-lastFrameAt>250)resetPending=true;
        if(resetPending){Reset();resetPending=false;}lastFrameAt=now;
        if(!sm::InitializeDriver(driverPath,settings,Log)){Fail(sm::DriverStatus().c_str());return S_OK;}
        ls::ComPtr<ID3D11Device> current;ls::ComPtr<ID3D11Texture2D> source;
        if(FAILED(sc->GetDevice(IID_PPV_ARGS(&current))))return S_OK;
        if(frame->replacement)source=frame->replacement;
        else if(FAILED(sc->GetBuffer(0,IID_PPV_ARGS(&source))))return S_OK;
        D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
        if(selected!=reinterpret_cast<uintptr_t>(sc) || current.Get()!=device.Get() || width!=d.Width || height!=d.Height || format!=d.Format){
            Reset();selected=reinterpret_cast<uintptr_t>(sc);device=current;width=d.Width;height=d.Height;format=d.Format;lastFrameAt=now;
            backend=std::make_unique<sm::Backend>();
            if(!backend->Init(device.Get(),sd.OutputWindow,d,settings,plainFallback,Log)){Fail("Smooth Motion D3D12 bridge initialization failed; LS output preserved");return S_OK;}
        }
        if(!backend->Prepare(source.Get())){
            if(ScheduleRecovery("Smooth Motion: copy/queue recovery scheduled (bounded to 3 per minute)"))return S_OK;
            Fail("Smooth Motion GPU synchronization failed; LS output preserved");return S_OK;
        }
        currentSync=*sync;const HRESULT hr=backend->Present(*sync);milliseconds=backend->Milliseconds();
        if(hr!=S_OK){if(!ScheduleRecovery("Smooth Motion: output Present recovery scheduled (bounded to 3 per minute)"))Fail("Smooth Motion output repeatedly unavailable; LS output preserved");return S_OK;}
        if(backend->TimedOut()){
            if(!plainFallback && settings.preferVRR){plainFallback=true;resetPending=true;Log("Smooth Motion: no verified inference on tearing chain; trying the reference plain-chain descriptor once");return S_OK;}
            Fail("Smooth Motion: wrapper/CUDA inference not confirmed after warmup; LS output preserved");return S_OK;
        }
        frame->externalPresented=TRUE; // also transports valid base/HDR frames during model warmup
        if(backend->Confirmed()){
            if(backend->Duplicate())++duplicates;else ++submitted;
            frame->externalPresented=TRUE;
            // Once the driver actually ran inference on the separate output,
            // it replaces the outer LS submission. The original LS backbuffer
            // stays writable; passthrough resumes on suspend/failure. Keeping
            // both submissions is an explicit compatibility option, not an
            // accidental second driver FG invocation on the source window.
            if(!settings.preserveLsPresent){*handled=TRUE;return hr;}
        }
    }catch(...){Fail("Smooth Motion exception; original LS output preserved");}
    return S_OK;
}
void WINAPI After(IDXGISwapChain* sc,HRESULT hr,LsBridgeFrame*,void*){
    std::lock_guard<std::mutex> lock(mutex);
    if(!ready || !host || disabled || selected!=reinterpret_cast<uintptr_t>(sc) || !backend)return;
    try {
        if(hr!=S_OK){if(!ScheduleRecovery("Smooth Motion: source Present recovery scheduled (bounded to 3 per minute)"))Fail("Smooth Motion source output repeatedly unavailable; LS output preserved");return;}
        const bool active=backend->Confirmed();
        host->PublishMetric(id,"copy_ms",milliseconds,"ms");host->PublishMetric(id,"base_frames_submitted",double(submitted),"frames");
        host->PublishMetric(id,"cuda_graph_launches",double(nvs30::nvpresent::graph_launch_count()),"launches");
        char text[256];std::snprintf(text,sizeof text,"Smooth Motion x2: %s; copy %.2f ms; %s; base frames %llu; duplicates %llu",active?"wrapper + CUDA inference confirmed":"warming driver / inference unconfirmed",milliseconds,backend->Tearing()?"VRR-compatible output":"reference plain output; VRR not forced",static_cast<unsigned long long>(submitted),static_cast<unsigned long long>(duplicates));Status(text,active?1:2);
    }catch(...){Fail("Smooth Motion post-Present exception");}
}
void Dispatch(uint32_t,uint32_t,uint32_t,void*){
    std::lock_guard<std::mutex> lock(mutex);if(!ready || !host)return;if(pendingSettings)Read();
    if(!settings.enabled || !settings.nativeFGDisabled || disabled)return;
    auto* ctx=static_cast<ID3D11DeviceContext*>(host->GetDispatchingContext());if(!ctx)return;
    ID3D11UnorderedAccessView* views[D3D11_PS_CS_UAV_REGISTER_COUNT]{};ctx->CSGetUnorderedAccessViews(0,D3D11_PS_CS_UAV_REGISTER_COUNT,views);
    for(auto* v:views){if(!v)continue;ls::ComPtr<ID3D11Resource> r;v->GetResource(&r);v->Release();ls::ComPtr<IDXGISurface> s;ls::ComPtr<IDXGISwapChain> chain;
        if(SUCCEEDED(r.As(&s)) && SUCCEEDED(s->GetParent(IID_PPV_ARGS(&chain)))){chain->SetPrivateData(tagGuid,sizeof epoch,&epoch);if(!LsBridgeInstall(chain.Get(),BridgeLog,nullptr)){disabled=true;Log("Smooth Motion: live output hook installation failed");}}}
}
void Event(uint32_t event,const void*,uint32_t,void*){
    std::lock_guard<std::mutex> lock(mutex);if(!ready)return;
    if(event==EAM_EVENT_SETTINGS_APPLIED){pendingSettings=true;return;}
    resetPending=true;disabled=false;++epoch;
}
std::string Utf8(const std::wstring& w){if(w.empty())return {};const int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),int(w.size()),nullptr,0,nullptr,nullptr);std::string s(n,0);WideCharToMultiByte(CP_UTF8,0,w.c_str(),int(w.size()),s.data(),n,nullptr,nullptr);return s;}
std::wstring Wide(const char* s){const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,nullptr,0);if(!n)return {};std::wstring w(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,w.data(),n);w.pop_back();return w;}
}
EAM_EXPORT void AddonInitialize(IHost* h,ImGuiContext* ctx,void* allocate,void* release,void* user){
    if(!h || h->GetHostVersion()<0x010100)return;
    if(ctx && allocate && release){ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(allocate),reinterpret_cast<ImGuiMemFreeFunc>(release),user);ImGui::SetCurrentContext(ctx);eam::ui::InitAddonImGui();ui=true;}else ui=false;
    {std::lock_guard<std::mutex> lock(mutex);if(host)return;if(shutdownQuarantined){h->Log(EAM_LOG_ERROR,"Previous output did not drain; restart LS before reinitializing this addon");return;}host=h;HMODULE self=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&AddonInitialize),&self);wchar_t path[32768]{};GetModuleFileNameW(self,path,32768);directory=std::filesystem::path(path).parent_path().wstring();Read();}
    LsBridgeCallbacks cb{};cb.owner=LS_OWNER_SMOOTH;cb.kind=LS_BRIDGE_SINK;cb.before=Before;cb.after=After;
    if(!LsBridgeRegister(&cb)){h->Log(EAM_LOG_ERROR,"Smooth Motion: bridge registration failed");return;}
    for(uint32_t e:{EAM_EVENT_D3D11_DEVICE_READY,EAM_EVENT_D3D11_DEVICE_CHANGED,EAM_EVENT_SETTINGS_APPLIED})h->SubscribeEvent(e,Event);
    h->SetPostDispatchCallback(Dispatch);{std::lock_guard<std::mutex> lock(mutex);ready=true;}
    h->Log(EAM_LOG_INFO,"LS_SmoothMotion 0.1.1 initialized without GPU/driver access; native LSFG and DLSS FG must be off");
}
EAM_EXPORT void AddonShutdown(){
    IHost* h=nullptr;{std::lock_guard<std::mutex> lock(mutex);ready=false;h=host;}
    const bool drained=LsBridgeUnregister(LS_OWNER_SMOOTH)!=FALSE;
    if(h){h->SetPostDispatchCallback(nullptr);for(uint32_t e:{EAM_EVENT_D3D11_DEVICE_READY,EAM_EVENT_D3D11_DEVICE_CHANGED,EAM_EVENT_SETTINGS_APPLIED})h->UnsubscribeEvent(e,Event);h->SetStatus(id,"",0);}
    std::lock_guard<std::mutex> lock(mutex);
    if(drained){Reset();lease.Reset();sm::StopDriver();}
    else{shutdownQuarantined=true;sm::DetachDriverLogger();OutputDebugStringA("Smooth Motion shutdown: in-flight output resources retained until process exit\n");}
    host=nullptr;
}
EAM_EXPORT uint32_t GetAddonCapabilities(){return EAM_CAP_HAS_SETTINGS|EAM_CAP_REQUIRES_RESTART|EAM_CAP_D3D11_DEVICE_ACCESS|EAM_CAP_DISPATCH_HOOK;}
EAM_EXPORT const char* GetAddonName(){return "Smooth Motion for RTX 30 (experimental)";}
EAM_EXPORT const char* GetAddonVersion(){return "0.1.1";}
EAM_EXPORT const char* GetAddonAuthor(){return "Anton / Magpie experiments; ItsAdeline NVSmooth30";}
EAM_EXPORT const char* GetAddonDescription(){return "Driver Smooth Motion x2 via a same-GPU D3D12 output. Validated NvPresent profile required. No NVIDIA runtime or process proxy bundled.";}
EAM_EXPORT void AddonRenderSettings(){
    if(!ui)return;static sm::Settings draft;static char path[2048]{};static uint64_t seen=UINT64_MAX;static bool dirty=false;
    sm::Settings current;std::wstring savedPath;uint64_t rev=0;bool active=false;double ms=0;std::string driver;
    {std::lock_guard<std::mutex> lock(mutex);if(!host)return;current=settings;savedPath=driverPath;rev=revision;active=backend && backend->Confirmed();ms=milliseconds;driver=sm::DriverStatus();}
    if(seen!=rev && !dirty){draft=current;const auto text=Utf8(savedPath);std::snprintf(path,sizeof path,"%s",text.c_str());seen=rev;}
    ImGui::TextWrapped("An alternative x2 generator using NVIDIA NvPresent and the open NVSmooth30 Ampere compatibility path. Disable native LSFG, this suite's DLSS FG addon and all other Smooth Motion loaders. A GPU model alone does not establish runtime compatibility.");
    dirty|=ImGui::Checkbox("Enable Smooth Motion",&draft.enabled);dirty|=ImGui::Checkbox("I have disabled native LS frame generation",&draft.nativeFGDisabled);
    ImGui::Text("Frame multiplier: 2x (driver contract)");
    bool adaptive=draft.targetFPS==0;if(ImGui::Checkbox("Uncapped base input",&adaptive)){draft.targetFPS=adaptive?0.f:136.f;dirty=true;}
    if(!adaptive){dirty|=ImGui::InputFloat("Target output FPS",&draft.targetFPS,1,10,"%.2f");ImGui::Text("Base submission / suggested game cap: %.2f FPS",draft.targetFPS/2);}
    int sync=int(draft.syncMode);if(ImGui::Combo("Output synchronization",&sync,"VRR-compatible (sync 0)\0Follow LS sync interval\0Vsync (sync 1)\0")){draft.syncMode=unsigned(sync);dirty=true;}
    dirty|=ImGui::Checkbox("Prefer tearing-capable output (VRR request)",&draft.preferVRR);
    dirty|=ImGui::Checkbox("Duplicate Frame Filtering (exact RGB)",&draft.filterDuplicates);
    dirty|=ImGui::Checkbox("Automatic recovery after minimize / output reset",&draft.automaticRecovery);
    int mode=int(draft.devicePath);if(ImGui::Combo("D3D12 device creation",&mode,"Auto (default only when GPU matches)\0Explicit LS adapter\0Default path, verified LS adapter\0")){draft.devicePath=unsigned(mode);dirty=true;}
    dirty|=ImGui::Checkbox("Preserve original LS Present (compatibility option)",&draft.preserveLsPresent);
    dirty|=ImGui::Checkbox("Driver diagnostics",&draft.diagnostics);
    dirty|=ImGui::InputText("NvPresent64.dll (empty = active driver)",path,sizeof path);
    ImGui::TextWrapped("The active driver package is used; old DriverStore versions are not searched by timestamp. The upstream reference and inspected 66ace profile are enabled. Unknown versions pass LS frames through without driver patching. After changing the driver path or disabling an initialized driver backend, restart LS. No version.dll/dxgi.dll proxy replaces the addon manager.");
    ImGui::TextWrapped("Output uses a disabled, nonactivating child of the LS window on the exact LS GPU. It carries valid base frames while the model warms; the active status requires observed CUDA inference. If a tearing chain is rejected, the reference plain-chain descriptor is attempted once. This does not guarantee physical VRR. RTX HDR from this suite can supply scRGB directly.");
    if(ImGui::Button("Reinitialize output")){std::lock_guard<std::mutex> lock(mutex);disabled=false;resetPending=true;plainFallback=false;}
    ImGui::SameLine();if(ImGui::Button("136 FPS profile")){const bool enabled=draft.enabled,confirmed=draft.nativeFGDisabled;draft={};draft.enabled=enabled;draft.nativeFGDisabled=confirmed;dirty=true;}
    if(ImGui::Button("Save and apply")){
        if(!std::isfinite(draft.targetFPS))draft.targetFPS=136;draft.Validate();std::lock_guard<std::mutex> lock(mutex);const auto ini=(std::filesystem::path(directory)/L"LS_SmoothMotion.ini").wstring();bool ok=true;
        auto put=[&](const wchar_t* key,unsigned v){wchar_t t[32];std::swprintf(t,32,L"%u",v);ok=WritePrivateProfileStringW(L"SmoothMotion",key,t,ini.c_str()) && ok;};
        put(L"Enabled",draft.enabled);put(L"NativeLSFGDisabled",draft.nativeFGDisabled);put(L"PreferVRR",draft.preferVRR);put(L"DuplicateFrameFiltering",draft.filterDuplicates);put(L"PreserveLSPresent",draft.preserveLsPresent);put(L"Diagnostics",draft.diagnostics);put(L"AutomaticRecovery",draft.automaticRecovery);put(L"SyncMode",draft.syncMode);put(L"DeviceCreationPath",draft.devicePath);
        wchar_t fps[64];std::swprintf(fps,64,L"%.3f",draft.targetFPS);ok=WritePrivateProfileStringW(L"SmoothMotion",L"TargetOutputFPS",fps,ini.c_str()) && ok;
        const auto file=Wide(path);ok=WritePrivateProfileStringW(L"SmoothMotion",L"NvPresentPath",file.c_str(),ini.c_str()) && ok;
        if(ok){pendingSettings=true;dirty=false;}else Status("Cannot write Smooth Motion settings",3);
    }
    if(dirty)ImGui::TextDisabled("Unsaved changes");
    ImGui::TextWrapped("Driver: %s",driver.c_str());ImGui::Text("Inference confirmed: %s; staging %.2f ms",active?"yes":"no",ms);
}
