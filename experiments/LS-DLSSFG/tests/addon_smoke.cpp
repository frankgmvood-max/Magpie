#include <eam/addon_sdk.h>
#include <windows.h>
#include <cstdio>
#include <vector>
#include <cstdlib>
struct Host final : IHost {
    struct Subscription {uint32_t event;EamEventCallback cb;void* data;};
    std::vector<Subscription> subscriptions;
    EamPostDispatchCallback post=nullptr;void* postData=nullptr;int statuses=0,logs=0;
    void Log(EamLogLevel,const char*) override {++logs;}
    const char* GetConfig(const char*,const char*,const char* d) override {return d;}
    void SetConfig(const char*,const char*,const char*) override {}
    void SaveConfig() override {}
    uint32_t GetHostVersion() override {return 0x010200;}
    void SubscribeEvent(uint32_t e,EamEventCallback cb,void* d) override {subscriptions.push_back({e,cb,d});}
    void UnsubscribeEvent(uint32_t e,EamEventCallback cb) override {
        for(auto it=subscriptions.begin();it!=subscriptions.end();) {
            if(it->event==e && it->cb==cb) it=subscriptions.erase(it);else ++it;
        }
    }
    void PublishEvent(uint32_t e,const void* d,uint32_t size) override {
        const auto copy=subscriptions;for(const auto& x:copy) if(x.event==e) x.cb(e,d,size,x.data);
    }
    void* GetD3D11Device() override {return nullptr;}
    void* GetD3D11DeviceContext() override {return nullptr;}
    void SetPreDispatchCallback(EamPreDispatchCallback,void*) override {}
    void SetPostDispatchCallback(EamPostDispatchCallback cb,void* d) override {post=cb;postData=d;}
    void* GetCurrentComputeShader() override {return nullptr;}
    uint32_t GetDispatchCount() override {return 0;}
    void SetStatus(const char*,const char*,int) override {++statuses;}
    void PublishMetric(const char*,const char*,double,const char*) override {}
    void* GetDispatchingContext() override {return nullptr;}
    void* CreateImage(const void*,uint32_t,uint32_t,uint32_t) override {return nullptr;}
    void ReleaseImage(void*) override {}
};
void Check(bool ok,const char* message) {if(!ok){std::fprintf(stderr,"%s\n",message);std::exit(1);}}
int main(int argc,char** argv) {
    Check(argc==2,"supply addon DLL path");
    HMODULE dll=LoadLibraryA(argv[1]);Check(dll!=nullptr,"DLL cannot load / missing dependency");
    auto init=reinterpret_cast<AddonInit_t>(GetProcAddress(dll,"AddonInitialize"));
    auto stop=reinterpret_cast<AddonShutdown_t>(GetProcAddress(dll,"AddonShutdown"));
    auto caps=reinterpret_cast<GetAddonCaps_t>(GetProcAddress(dll,"GetAddonCapabilities"));
    auto version=reinterpret_cast<GetAddonVersion_t>(GetProcAddress(dll,"GetAddonVersion"));
    Check(init && stop && caps && version,"missing SDK export");
    Check((caps()&EAM_CAP_DISPATCH_HOOK)!=0 && (caps()&EAM_CAP_REQUIRES_RESTART)!=0,"missing capabilities");
    Host host;
    for(int i=0;i<3;++i) {
        init(&host,nullptr,nullptr,nullptr,nullptr);
        Check(host.subscriptions.size()==3 && host.post && host.statuses>0,"initialization failed");
        host.PublishEvent(EAM_EVENT_D3D11_DEVICE_CHANGED,nullptr,0);
        host.PublishEvent(EAM_EVENT_D3D11_DEVICE_READY,nullptr,0);
        host.PublishEvent(EAM_EVENT_SETTINGS_APPLIED,nullptr,0);
        host.post(1,1,1,host.postData); // no GPU/device: must safely do nothing
        stop();
        Check(host.subscriptions.empty() && host.post==nullptr,"callbacks remained after shutdown");
    }
    Check(host.logs>=3,"host log not used");
    FreeLibrary(dll);std::puts("addon ABI/lifecycle passed");
}
