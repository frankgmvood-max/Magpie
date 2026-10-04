#include <eam/addon_sdk.h>
#include <imgui.h>
#include <windows.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
void Check(bool b,const char* why){if(!b){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
struct Host final:IHost {
    struct Subscription{uint32_t e;EamEventCallback cb;void* data;};std::vector<Subscription> subscriptions;
    EamPostDispatchCallback post=nullptr;void* postData=nullptr;unsigned deviceReads=0,logs=0;
    void Log(EamLogLevel,const char*)override{++logs;}
    const char* GetConfig(const char*,const char*,const char* d)override{return d;}
    void SetConfig(const char*,const char*,const char*)override{}
    void SaveConfig()override{}
    uint32_t GetHostVersion()override{return 0x010200;}
    void SubscribeEvent(uint32_t e,EamEventCallback cb,void* d)override{subscriptions.push_back({e,cb,d});}
    void UnsubscribeEvent(uint32_t e,EamEventCallback cb)override{for(auto i=subscriptions.begin();i!=subscriptions.end();)if(i->e==e && i->cb==cb)i=subscriptions.erase(i);else ++i;}
    void PublishEvent(uint32_t e,const void* d,uint32_t n)override{const auto copy=subscriptions;for(auto x:copy)if(x.e==e)x.cb(e,d,n,x.data);}
    void* GetD3D11Device()override{++deviceReads;return reinterpret_cast<void*>(uintptr_t(1));}
    void* GetD3D11DeviceContext()override{++deviceReads;return reinterpret_cast<void*>(uintptr_t(1));}
    void SetPreDispatchCallback(EamPreDispatchCallback,void*)override{}
    void SetPostDispatchCallback(EamPostDispatchCallback cb,void* data)override{
        // A callback can arrive while the host registers/unregisters it.
        // This detects addon/host lock-order inversions during lifecycle.
        if(!cb && post)post(1,1,1,postData);post=cb;postData=data;if(cb)cb(1,1,1,data);
    }
    void* GetCurrentComputeShader()override{return nullptr;}
    uint32_t GetDispatchCount()override{return 0;}
    void SetStatus(const char*,const char*,int)override{}
    void PublishMetric(const char*,const char*,double,const char*)override{}
    void* GetDispatchingContext()override{return nullptr;}
    void* CreateImage(const void*,uint32_t,uint32_t,uint32_t)override{return nullptr;}
    void ReleaseImage(void*)override{}
};
int main(int argc,char** argv){
    Check(argc==3,"supply HDR and SM DLLs");
    for(int addon=1;addon<argc;++addon){
        HMODULE dll=LoadLibraryA(argv[addon]);Check(dll!=nullptr,"addon cannot load / shared dependency missing");
        auto init=reinterpret_cast<AddonInit_t>(GetProcAddress(dll,"AddonInitialize"));auto stop=reinterpret_cast<AddonShutdown_t>(GetProcAddress(dll,"AddonShutdown"));auto panel=reinterpret_cast<AddonRenderSettings_t>(GetProcAddress(dll,"AddonRenderSettings"));auto caps=reinterpret_cast<GetAddonCaps_t>(GetProcAddress(dll,"GetAddonCapabilities"));auto version=reinterpret_cast<GetAddonVersion_t>(GetProcAddress(dll,"GetAddonVersion"));
        Check(init && stop && panel && caps && version && std::string(version())==(addon==1?"0.1.0":"0.1.1"),"exports / version");Check((caps()&(EAM_CAP_HAS_SETTINGS|EAM_CAP_DISPATCH_HOOK))==(EAM_CAP_HAS_SETTINGS|EAM_CAP_DISPATCH_HOOK),"settings capabilities");
        Host host;
        for(unsigned attempt=0;attempt<3;++attempt){
            ImGuiContext* ctx=ImGui::CreateContext();ImGuiMemAllocFunc allocate=nullptr;ImGuiMemFreeFunc release=nullptr;void* data=nullptr;ImGui::GetAllocatorFunctions(&allocate,&release,&data);
            init(&host,ctx,reinterpret_cast<void*>(allocate),reinterpret_cast<void*>(release),data);Check(host.subscriptions.size()==3 && host.post,"host registration");
            host.PublishEvent(EAM_EVENT_D3D11_DEVICE_READY,nullptr,0);host.PublishEvent(EAM_EVENT_D3D11_DEVICE_CHANGED,nullptr,0);host.PublishEvent(EAM_EVENT_SETTINGS_APPLIED,nullptr,0);host.post(1,1,1,host.postData);
            auto& io=ImGui::GetIO();io.DisplaySize=ImVec2(1280,1000);io.DeltaTime=1.f/60;unsigned char* pixels=nullptr;int w=0,h=0;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
            // A new ImGui window can be hidden for its initial auto-fit frame.
            // Exercise real settings on subsequent frames with a known size.
            for(unsigned frame=0;frame<3;++frame){
                ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(10,10),ImGuiCond_Always);ImGui::SetNextWindowSize(ImVec2(1100,900),ImGuiCond_Always);
                ImGui::Begin("Addon settings");panel();ImGui::End();ImGui::Render();
            }
            Check(ImGui::GetDrawData() && ImGui::GetDrawData()->TotalVtxCount>0,"actual settings rendering");
            stop();Check(host.subscriptions.empty() && !host.post && !host.deviceReads,"shutdown, registration race or stale borrowed-device access");ImGui::DestroyContext(ctx);
        }
        Check(host.logs>0,"host logging");FreeLibrary(dll);
    }
    std::puts("HDR and Smooth Motion exports, real ImGui menus, restart, event/dispatch registration and stale-device startup passed");
}
