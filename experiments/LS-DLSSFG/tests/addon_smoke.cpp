#include <eam/addon_sdk.h>
#include <windows.h>
#include <cstdio>
#include <vector>
#include <cstdlib>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <imgui.h>
using Microsoft::WRL::ComPtr;
struct Host final : IHost {
    struct Subscription {uint32_t event;EamEventCallback cb;void* data;};
    std::vector<Subscription> subscriptions;
    EamPostDispatchCallback post=nullptr;void* postData=nullptr;int statuses=0,logs=0;
    ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;
    bool rejectedSoftware=false;
    unsigned borrowedDeviceReads=0;
    bool dispatchDuringRegistration=false;
    void Log(EamLogLevel,const char* text) override {++logs;if(std::string(text).find("LS output adapter is not NVIDIA")!=std::string::npos)rejectedSoftware=true;}
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
    void* GetD3D11Device() override {++borrowedDeviceReads;return device;}
    void* GetD3D11DeviceContext() override {return context;}
    void SetPreDispatchCallback(EamPreDispatchCallback,void*) override {}
    void SetPostDispatchCallback(EamPostDispatchCallback cb,void* d) override {
        // Host registration/unregistration may overlap a render callback.
        // Calling it synchronously exposes the addon's lock ordering directly.
        if(dispatchDuringRegistration && !cb && post) post(1,1,1,postData);
        post=cb;postData=d;
        if(dispatchDuringRegistration && cb) cb(1,1,1,d);
    }
    void* GetCurrentComputeShader() override {return nullptr;}
    uint32_t GetDispatchCount() override {return 0;}
    void SetStatus(const char*,const char*,int) override {++statuses;}
    void PublishMetric(const char*,const char*,double,const char*) override {}
    void* GetDispatchingContext() override {return context;}
    void* CreateImage(const void*,uint32_t,uint32_t,uint32_t) override {return nullptr;}
    void ReleaseImage(void*) override {}
};
void Check(bool ok,const char* message) {if(!ok){std::fprintf(stderr,"%s\n",message);std::exit(1);}}
DWORD InitializeGuarded(AddonInit_t init,IHost* host) {
    __try {init(host,nullptr,nullptr,nullptr,nullptr);return 0;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return GetExceptionCode();}
}
int main(int argc,char** argv) {
    Check(argc==2,"supply addon DLL path");
    wchar_t temp[MAX_PATH]{}, file[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH,temp)!=0 && GetTempFileNameW(temp,L"lfg",0,file)!=0,"temporary test path");
    DeleteFileW(file);Check(CreateDirectoryW(file,nullptr)!=FALSE,"temporary test folder");
    const std::filesystem::path folder(file),dllPath=folder/L"LS_DLSSFG.dll";
    std::filesystem::copy_file(argv[1],dllPath);
    HMODULE dll=LoadLibraryW(dllPath.c_str());Check(dll!=nullptr,"DLL cannot load / missing dependency");
    auto init=reinterpret_cast<AddonInit_t>(GetProcAddress(dll,"AddonInitialize"));
    auto stop=reinterpret_cast<AddonShutdown_t>(GetProcAddress(dll,"AddonShutdown"));
    auto caps=reinterpret_cast<GetAddonCaps_t>(GetProcAddress(dll,"GetAddonCapabilities"));
    auto version=reinterpret_cast<GetAddonVersion_t>(GetProcAddress(dll,"GetAddonVersion"));
    Check(init && stop && caps && version,"missing SDK export");
    Check((caps()&EAM_CAP_DISPATCH_HOOK)!=0 && (caps()&EAM_CAP_REQUIRES_RESTART)!=0,"missing capabilities");
    Check((caps()&EAM_CAP_HAS_SETTINGS)!=0,"missing settings capability");
    auto panel=reinterpret_cast<AddonRenderSettings_t>(GetProcAddress(dll,"AddonRenderSettings"));
    Check(panel!=nullptr && std::string(version())=="0.3.0","settings export / version");
    Check(InitializeGuarded(init,reinterpret_cast<IHost*>(uintptr_t(1)))==EXCEPTION_ACCESS_VIOLATION,
        "init fault must propagate to the manager after recording a breadcrumb");
    Host host;
    {
        ImGuiContext* ui=ImGui::CreateContext();
        ImGuiMemAllocFunc allocate=nullptr;ImGuiMemFreeFunc release=nullptr;void* user=nullptr;
        ImGui::GetAllocatorFunctions(&allocate,&release,&user);
        init(&host,ui,reinterpret_cast<void*>(allocate),reinterpret_cast<void*>(release),user);
        ImGuiIO& io=ImGui::GetIO();io.DisplaySize=ImVec2(1280,900);io.DeltaTime=1.0f/60;
        unsigned char* pixels=nullptr;int w=0,h=0;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
        ImGui::NewFrame();ImGui::Begin("Addon settings");panel();ImGui::End();ImGui::Render();
        Check(ImGui::GetDrawData()!=nullptr,"settings render did not produce draw data");
        stop();ImGui::DestroyContext(ui);
    }
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
    // Exercise the actual DXGI hook and output selection on the software GPU.
    // A valid tagged output must fail NGX eligibility without breaking Present.
    std::ofstream(folder/L"LS_DLSSFG.ini")<<"[FrameGeneration]\nNativeLSFGDisabled=1\nTargetFPS=136\n";
    // A prior scaling device may already have died when the GUI starts addons.
    // Neither startup nor DEVICE_READY may read/use that borrowed pointer.
    host.device=reinterpret_cast<ID3D11Device*>(uintptr_t(1));
    host.dispatchDuringRegistration=true;
    Check(InitializeGuarded(init,&host)==0,"armed startup used an invalid old device");
    host.PublishEvent(EAM_EVENT_D3D11_DEVICE_READY,nullptr,0);
    Check(host.borrowedDeviceReads==0,"startup/device-ready consulted a borrowed device");
    stop();host.device=nullptr;host.dispatchDuringRegistration=false;
    std::filesystem::create_directory(folder/L"runtime");
    std::ofstream(folder/L"runtime"/L"nvngx_dlssg.dll"); // never loaded on WARP
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"create software device");
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"LSFGSmoke";
    RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"LS test",WS_POPUP,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
    Check(window!=nullptr,"create test window");ShowWindow(window,SW_SHOWNOACTIVATE);
    ComPtr<IDXGIDevice> dx;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory2> factory;
    Check(SUCCEEDED(device.As(&dx)) && SUCCEEDED(dx->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))),"software DXGI factory");
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=64;desc.Height=64;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT|DXGI_USAGE_UNORDERED_ACCESS;
    desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    Check(SUCCEEDED(factory->CreateSwapChainForHwnd(device.Get(),window,&desc,nullptr,nullptr,&chain)),"create flip swapchain");
    host.device=device.Get();host.context=context.Get();init(&host,nullptr,nullptr,nullptr,nullptr);
    Check(SUCCEEDED(chain->Present(0,0)),"untagged/UI present pass-through");
    Check(!host.rejectedSoftware,"untagged swapchain was processed");
    ComPtr<ID3D11Texture2D> back;ComPtr<ID3D11UnorderedAccessView> uav;
    Check(SUCCEEDED(chain->GetBuffer(0,IID_PPV_ARGS(&back))) && SUCCEEDED(device->CreateUnorderedAccessView(back.Get(),nullptr,&uav)),"output UAV");
    ID3D11UnorderedAccessView* view=uav.Get();context->CSSetUnorderedAccessViews(0,1,&view,nullptr);
    host.post(1,1,1,host.postData);
    const GUID tagGuid={0x396afbe7,0xe6cf,0x4174,{0xa2,0x8b,0xb1,0x89,0xc9,0xdc,0xf8,0x40}};
    uint64_t tag=0;UINT size=sizeof tag;
    Check(SUCCEEDED(chain->GetPrivateData(tagGuid,&size,&tag)) && tag!=0,"compute output tagging failed");
    Check(SUCCEEDED(chain->Present(0,DXGI_PRESENT_TEST)),"TEST present pass-through");
    Check(!host.rejectedSoftware,"TEST present invoked FG");
    Check(SUCCEEDED(chain->Present(0,0)),"FG failure broke original Present");
    Check(host.rejectedSoftware,"tagged output did not reach adapter eligibility check");
    Check(SUCCEEDED(chain->Present(0,0)),"disabled addon broke subsequent presents");
    stop();Check(host.subscriptions.empty() && !host.post,"GPU callbacks remained after shutdown");
    context->ClearState();context->Flush();back.Reset();uav.Reset();chain.Reset();DestroyWindow(window);
    FreeLibrary(dll);
    // Hook pins the DLL until exit. Other temporary files can be removed now.
    std::filesystem::remove(folder/L"LS_DLSSFG.ini");std::filesystem::remove_all(folder/L"runtime");
    std::puts("addon startup (including stale device), ABI, lifecycle, output selection and WARP fallback passed");
}
