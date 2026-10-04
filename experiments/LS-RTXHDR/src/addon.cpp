// SPDX-License-Identifier: GPL-3.0-only
#include <eam/addon_sdk.h>
#include <eam/widgets.h>
#include <imgui.h>
#include <ls_output_bridge.h>
#include "backend.h"
#include "presenter.h"
#include "display.h"
#include <filesystem>
#include <mutex>
#include <cwchar>
#include <cstdio>
#include <cmath>
namespace {
constexpr const char* id="LS_RTXHDR";
constexpr GUID tagGuid={0x6cde6975,0x0415,0x4d67,{0x83,0x2b,0x48,0xdb,0x13,0xa2,0x99,0x01}};
std::mutex mutex;IHost* host=nullptr;std::wstring directory;
hdr::Settings settings;std::unique_ptr<hdr::Backend> backend;hdr::Presenter presenter;
ls::ComPtr<ID3D11Device> device;uintptr_t selected=0;UINT width=0,height=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
uint64_t epoch=1,revision=0,statusAt=0,displayAt=0,processed=0,duplicates=0;
bool ready=false,ui=false,pendingSettings=false,resetPending=false,disabled=false,displayHdr=false,hdrOutput=false;
double milliseconds=0;uint64_t lastFrameAt=0;
void Log(const char* text){if(host)host->Log(EAM_LOG_INFO,text);}
void WINAPI BridgeLog(const char* text,void*){Log(text);}
void Status(const char* text,int level){if(host && GetTickCount64()-statusAt>=500){statusAt=GetTickCount64();host->SetStatus(id,text,level);}}
void Reset(){presenter.Reset();backend.reset();device.Reset();selected=0;width=height=0;format=DXGI_FORMAT_UNKNOWN;lastFrameAt=0;}
void Read(){
    const auto path=(std::filesystem::path(directory)/L"LS_RTXHDR.ini").wstring();
    auto get=[&](const wchar_t* key,unsigned d){return GetPrivateProfileIntW(L"RTXHDR",key,d,path.c_str());};
    settings.enabled=get(L"Enabled",0)==1;settings.nativeHdrDisabled=get(L"NativeLSHDRDisabled",0)==1;
    settings.contrast=get(L"Contrast",100);settings.saturation=get(L"Saturation",100);settings.middleGray=get(L"MiddleGray",50);settings.peakNits=get(L"PeakNits",1000);
    settings.outputMode=get(L"OutputMode",0);settings.duplicateFiltering=get(L"DuplicateFrameFiltering",1)==1;
    auto number=[&](const wchar_t* key,const wchar_t* fallback,float d){wchar_t text[64];GetPrivateProfileStringW(L"RTXHDR",key,fallback,text,64,path.c_str());const float v=std::wcstof(text,nullptr);return std::isfinite(v)?v:d;};
    settings.sdrWhiteNits=number(L"SDRWhiteNits",L"80",80);settings.exposure=number(L"SDRExposure",L"1",1);settings.shoulder=number(L"SDRShoulder",L"1",1);settings.Validate();
    ++revision;pendingSettings=false;disabled=false;Reset();++epoch;
}
bool Tagged(IDXGISwapChain* sc){uint64_t tag=0;UINT n=sizeof tag;return SUCCEEDED(sc->GetPrivateData(tagGuid,&n,&tag)) && n==sizeof tag && tag==epoch;}
HRESULT WINAPI Before(IDXGISwapChain* sc,UINT*,UINT* flags,BOOL*,LsBridgeFrame* frame,void*){
    std::lock_guard<std::mutex> lock(mutex);
    if(!ready || !host || !settings.enabled || !settings.nativeHdrDisabled || disabled || !ls::LegalPresent(*flags) || !Tagged(sc))return S_OK;
    try {
        DXGI_SWAP_CHAIN_DESC sd{};if(FAILED(sc->GetDesc(&sd)))return S_OK;
        const HWND root=GetAncestor(sd.OutputWindow,GA_ROOT);
        if(!root || !IsWindowVisible(root) || IsIconic(root)){presenter.Hide();resetPending=true;return S_OK;}
        const auto now=GetTickCount64();if(lastFrameAt && now-lastFrameAt>250)resetPending=true;
        if(resetPending){Reset();resetPending=false;}lastFrameAt=now;
        ls::ComPtr<ID3D11Device> current;ls::ComPtr<ID3D11Texture2D> back;
        if(FAILED(sc->GetDevice(IID_PPV_ARGS(&current))) || FAILED(sc->GetBuffer(0,IID_PPV_ARGS(&back))))return S_OK;
        D3D11_TEXTURE2D_DESC d{};back->GetDesc(&d);
        if(d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM){presenter.Hide();Status("RTX HDR expects SDR RGBA8/BGRA8; existing HDR output is passed through",2);return S_OK;}
        if(now>=displayAt){displayAt=now+1000;displayHdr=hdr::DisplayHdr(sd.OutputWindow);}
        const bool output=settings.outputMode==2 || (settings.outputMode==0 && displayHdr);
        if(output && !displayHdr){presenter.Hide();Status("HDR output requires Windows HDR on this monitor; select SDR display mode",2);return S_OK;}
        if(selected!=reinterpret_cast<uintptr_t>(sc) || current.Get()!=device.Get() || width!=d.Width || height!=d.Height || format!=d.Format || hdrOutput!=output){
            Reset();selected=reinterpret_cast<uintptr_t>(sc);device=current;width=d.Width;height=d.Height;format=d.Format;hdrOutput=output;lastFrameAt=now;
            backend=std::make_unique<hdr::Backend>();
            if(!backend->Init(device.Get(),d,directory,settings,Log)){disabled=true;Status("RTX HDR unavailable; see NVIDIA status in Logs; LS frames preserved",3);return S_OK;}
        }
        if(!backend->Process(back.Get(),settings,hdrOutput)){disabled=true;presenter.Hide();Status("RTX HDR evaluation failed; LS output preserved",3);return S_OK;}
        ++processed;if(backend->Duplicate())++duplicates;milliseconds=backend->Milliseconds();
        frame->replacement=backend->Output();frame->replacementColorSpace=hdrOutput?DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        if(!hdrOutput){ls::ComPtr<ID3D11DeviceContext> ctx;device->GetImmediateContext(&ctx);ctx->CopyResource(back.Get(),frame->replacement);ctx->Flush();}
    }catch(...){disabled=true;presenter.Hide();Log("RTX HDR exception; LS output preserved");}
    return S_OK;
}
void WINAPI After(IDXGISwapChain* sc,HRESULT hr,LsBridgeFrame* frame,void*){
    std::lock_guard<std::mutex> lock(mutex);
    if(!ready || !host || disabled || !frame->replacement || selected!=reinterpret_cast<uintptr_t>(sc))return;
    try {
        if(hr!=S_OK){presenter.Hide();resetPending=true;return;}
        if(hdrOutput && !frame->externalPresented){
            DXGI_SWAP_CHAIN_DESC sd{};sc->GetDesc(&sd);
            // Presenter creation is lazy: a Smooth Motion sink can consume the
            // FP16 HDR texture directly without creating a second HDR surface.
            if(!presenter.Ready() && !presenter.Init(device.Get(),sd.OutputWindow,width,height)){disabled=true;Log("RTX HDR: HDR output surface creation failed; LS output preserved");return;}
            if(!presenter.Present(frame->replacement)){resetPending=true;Log("RTX HDR: HDR output Present failed; retrying after reset");}
        }else presenter.Hide();
        host->PublishMetric(id,"hdr_ms",milliseconds,"ms");host->PublishMetric(id,"frames_processed",double(processed),"frames");
        char text[192];std::snprintf(text,sizeof text,"RTX Video HDR: %.2f ms; %s; %llu frames; %llu duplicates",milliseconds,hdrOutput?frame->externalPresented?"scRGB via FG output":"scRGB HDR output":"SDR display tone map",static_cast<unsigned long long>(processed),static_cast<unsigned long long>(duplicates));Status(text,1);
    }catch(...){disabled=true;presenter.Hide();Log("RTX HDR post-Present exception");}
}
void Dispatch(uint32_t,uint32_t,uint32_t,void*){
    std::lock_guard<std::mutex> lock(mutex);if(!ready || !host)return;
    if(pendingSettings)Read();if(!settings.enabled || !settings.nativeHdrDisabled || disabled)return;
    auto* ctx=static_cast<ID3D11DeviceContext*>(host->GetDispatchingContext());if(!ctx)return;
    ID3D11UnorderedAccessView* views[D3D11_PS_CS_UAV_REGISTER_COUNT]{};ctx->CSGetUnorderedAccessViews(0,D3D11_PS_CS_UAV_REGISTER_COUNT,views);
    for(auto* v:views){if(!v)continue;ls::ComPtr<ID3D11Resource> r;v->GetResource(&r);v->Release();ls::ComPtr<IDXGISurface> s;ls::ComPtr<IDXGISwapChain> chain;
        if(SUCCEEDED(r.As(&s)) && SUCCEEDED(s->GetParent(IID_PPV_ARGS(&chain)))){chain->SetPrivateData(tagGuid,sizeof epoch,&epoch);if(!LsBridgeInstall(chain.Get(),BridgeLog,nullptr)){disabled=true;Log("RTX HDR: could not hook current live output");}}}
}
void Event(uint32_t event,const void*,uint32_t,void*){
    std::lock_guard<std::mutex> lock(mutex);if(!ready)return;
    if(event==EAM_EVENT_SETTINGS_APPLIED){pendingSettings=true;return;}
    resetPending=true;disabled=false;++epoch;
}
}
EAM_EXPORT void AddonInitialize(IHost* h,ImGuiContext* ctx,void* allocate,void* release,void* user){
    if(!h || h->GetHostVersion()<0x010100)return;
    if(ctx && allocate && release){ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(allocate),reinterpret_cast<ImGuiMemFreeFunc>(release),user);ImGui::SetCurrentContext(ctx);eam::ui::InitAddonImGui();ui=true;}else ui=false;
    {std::lock_guard<std::mutex> lock(mutex);if(host)return;host=h;HMODULE self=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&AddonInitialize),&self);wchar_t path[32768];GetModuleFileNameW(self,path,32768);directory=std::filesystem::path(path).parent_path().wstring();Read();}
    LsBridgeCallbacks cb{};cb.owner=LS_OWNER_HDR;cb.kind=LS_BRIDGE_FILTER;cb.before=Before;cb.after=After;
    if(!LsBridgeRegister(&cb)){h->Log(EAM_LOG_ERROR,"RTX HDR: bridge registration failed");return;}
    for(uint32_t e:{EAM_EVENT_D3D11_DEVICE_READY,EAM_EVENT_D3D11_DEVICE_CHANGED,EAM_EVENT_SETTINGS_APPLIED})h->SubscribeEvent(e,Event);
    h->SetPostDispatchCallback(Dispatch);{std::lock_guard<std::mutex> lock(mutex);ready=true;}
    h->Log(EAM_LOG_INFO,"LS_RTXHDR 0.1.0 initialized; TrueHDR backend from Magpie 0.6.9; no GPU access at startup");
}
EAM_EXPORT void AddonShutdown(){
    IHost* h=nullptr;{std::lock_guard<std::mutex> lock(mutex);ready=false;h=host;}
    LsBridgeUnregister(LS_OWNER_HDR);
    if(h){h->SetPostDispatchCallback(nullptr);for(uint32_t e:{EAM_EVENT_D3D11_DEVICE_READY,EAM_EVENT_D3D11_DEVICE_CHANGED,EAM_EVENT_SETTINGS_APPLIED})h->UnsubscribeEvent(e,Event);h->SetStatus(id,"",0);}
    std::lock_guard<std::mutex> lock(mutex);Reset();host=nullptr;
}
EAM_EXPORT uint32_t GetAddonCapabilities(){return EAM_CAP_HAS_SETTINGS|EAM_CAP_REQUIRES_RESTART|EAM_CAP_D3D11_DEVICE_ACCESS|EAM_CAP_DISPATCH_HOOK;}
EAM_EXPORT const char* GetAddonName(){return "RTX Video HDR (Magpie backend)";}
EAM_EXPORT const char* GetAddonVersion(){return "0.1.0";}
EAM_EXPORT const char* GetAddonAuthor(){return "Anton / Magpie experiments; Magpie contributors";}
EAM_EXPORT const char* GetAddonDescription(){return "NVIDIA TrueHDR from Magpie 0.6.9 with contrast, saturation, middle gray and peak nits. Disable native LS HDR.";}
EAM_EXPORT void AddonRenderSettings(){
    if(!ui)return;static hdr::Settings draft;static uint64_t seen=UINT64_MAX;static bool dirty=false;
    hdr::Settings current;uint64_t rev=0;bool display=false;double ms=0;uint64_t count=0;
    {std::lock_guard<std::mutex> lock(mutex);if(!host)return;current=settings;rev=revision;display=displayHdr;ms=milliseconds;count=processed;}
    if(seen!=rev && !dirty){draft=current;seen=rev;}
    ImGui::TextWrapped("RTX Video TrueHDR uses the same NVIDIA backend and parameter ranges as Magpie 0.6.9. Disable LS HDR and other HDR conversion filters. SDR sources only.");
    dirty|=ImGui::Checkbox("Enable RTX Video HDR",&draft.enabled);dirty|=ImGui::Checkbox("I have disabled native LS HDR",&draft.nativeHdrDisabled);
    auto slider=[&](const char* label,unsigned& value,int low,int high){int v=int(value);if(ImGui::SliderInt(label,&v,low,high)){value=unsigned(v);dirty=true;}};
    slider("Contrast",draft.contrast,0,200);slider("Saturation",draft.saturation,0,200);slider("Middle gray",draft.middleGray,10,100);slider("Peak brightness (nits)",draft.peakNits,400,2000);
    int output=int(draft.outputMode);if(ImGui::Combo("Output",&output,"Auto (monitor HDR state)\0SDR display tone map\0HDR scRGB (Windows HDR required)\0")){draft.outputMode=unsigned(output);dirty=true;}
    dirty|=ImGui::Checkbox("Reuse unchanged frames (exact RGB)",&draft.duplicateFiltering);
    if(draft.outputMode!=2){dirty|=ImGui::SliderFloat("SDR white (nits)",&draft.sdrWhiteNits,40,300,"%.0f");dirty|=ImGui::SliderFloat("SDR display exposure",&draft.exposure,0.1f,4,"%.2f");dirty|=ImGui::SliderFloat("SDR display shoulder",&draft.shoulder,0.1f,4,"%.2f");}
    ImGui::TextWrapped("The four RTX settings affect NVIDIA TrueHDR. The SDR display settings only adapt its FP16 result to an SDR monitor. HDR mode keeps scRGB FP16 and uses an input-transparent child output surface. DLSS FG intermediates are processed too; Smooth Motion can take the FP16 texture directly.");
    if(ImGui::Button("Magpie defaults")){const bool enabled=draft.enabled,confirmed=draft.nativeHdrDisabled;draft={};draft.enabled=enabled;draft.nativeHdrDisabled=confirmed;dirty=true;}
    ImGui::SameLine();if(ImGui::Button("Reinitialize HDR")){std::lock_guard<std::mutex> lock(mutex);disabled=false;resetPending=true;}
    if(ImGui::Button("Save and apply")){
        draft.Validate();std::lock_guard<std::mutex> lock(mutex);const auto path=(std::filesystem::path(directory)/L"LS_RTXHDR.ini").wstring();bool ok=true;
        auto put=[&](const wchar_t* key,unsigned v){wchar_t t[32];std::swprintf(t,32,L"%u",v);ok=WritePrivateProfileStringW(L"RTXHDR",key,t,path.c_str()) && ok;};
        auto number=[&](const wchar_t* key,float v){wchar_t t[64];std::swprintf(t,64,L"%.4f",v);ok=WritePrivateProfileStringW(L"RTXHDR",key,t,path.c_str()) && ok;};
        put(L"Enabled",draft.enabled);put(L"NativeLSHDRDisabled",draft.nativeHdrDisabled);put(L"Contrast",draft.contrast);put(L"Saturation",draft.saturation);put(L"MiddleGray",draft.middleGray);put(L"PeakNits",draft.peakNits);put(L"OutputMode",draft.outputMode);put(L"DuplicateFrameFiltering",draft.duplicateFiltering);number(L"SDRWhiteNits",draft.sdrWhiteNits);number(L"SDRExposure",draft.exposure);number(L"SDRShoulder",draft.shoulder);
        if(ok){pendingSettings=true;dirty=false;}else Status("Cannot write HDR settings",3);
    }
    if(dirty)ImGui::TextDisabled("Unsaved changes");
    ImGui::Text("Monitor Windows HDR: %s; processing %.2f ms; frames %llu",display?"enabled":"disabled / not queried",ms,static_cast<unsigned long long>(count));
}
