// SPDX-License-Identifier: MIT
#include "backend.h"
#include "duplicates.h"
#include <nvs30/nvpresent.hpp>
#include <output_window.h>
#include <presentation.h>
#include <shared_gpu.h>
#include <chrono>
#include <cstdio>
#include <cstring>
namespace sm {
using Microsoft::WRL::ComPtr;
namespace {
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};return x;
}
bool EnableWrapper(IDXGISwapChain* chain){
    return nvs30::nvpresent::enable_wrapper(chain);
}
}
struct Backend::State {
    ComPtr<ID3D11Device> host;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;ComPtr<ID3D11Query> copyDone;
    std::unique_ptr<ls::SharedGpu> analysis;std::unique_ptr<fg::DuplicateFilter> duplicate;
    ComPtr<ID3D11Texture2D> analysisPrivate,analysisHost;
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Resource> shared;ComPtr<ID3D12Fence> completion;
    ComPtr<IDXGISwapChain3> chain;ls::OutputWindow window;
    DriverHooks hooks{};std::function<void(const char*)> log;Settings settings;
    HANDLE event=nullptr,timer=nullptr;uint64_t value=0,graphsSeen=0;unsigned presents=0;
    bool lost=false,enabled=false,confirmed=false,wasDuplicate=false,tearing=false,probeTimeout=false;
    UINT width=0,height=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;LUID luid{};
    double milliseconds=0,next=0;
    bool Wait(){
        if(!value)return true;const auto v=completion->GetCompletedValue();if(v==UINT64_MAX){lost=true;return false;}if(v>=value)return true;
        ResetEvent(event);if(FAILED(completion->SetEventOnCompletion(value,event)) || WaitForSingleObject(event,500)!=WAIT_OBJECT_0 || completion->GetCompletedValue()<value || completion->GetCompletedValue()==UINT64_MAX){lost=true;return false;}return true;
    }
    bool Error(const char* step,HRESULT hr=E_FAIL){char text[256];std::snprintf(text,sizeof text,"Smooth Motion: %s failed (0x%08x); LS output preserved",step,unsigned(hr));if(log)log(text);return false;}
    ~State(){window.Show(false);chain.Reset();if(event)CloseHandle(event);if(timer)CloseHandle(timer);}
    double Now(){LARGE_INTEGER n{},f{};QueryPerformanceCounter(&n);QueryPerformanceFrequency(&f);return double(n.QuadPart)/f.QuadPart;}
    void Pace(){
        if(!settings.targetFPS)return;
        const double step=2.0/settings.targetFPS,now=Now();
        if(!next || now>next+step)next=now;
        const double left=next-now;if(left>0 && left<0.08 && timer){LARGE_INTEGER due{};due.QuadPart=-LONGLONG(left*10000000);if(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer,80);}
        next=std::max(next,Now())+step;
    }
};
Backend::Backend()=default;
Backend::~Backend(){if(state_){state_->window.Show(false);if(state_->lost || !state_->Wait() || (state_->analysis && (state_->analysis->lost || !state_->analysis->Wait())))(void)state_.release();}}
bool Backend::Init(ID3D11Device* host,HWND parent,const D3D11_TEXTURE2D_DESC& d,const Settings& settings,bool plain,
                   std::function<void(const char*)> log,const DriverHooks* testHooks){
    state_=std::make_unique<State>();auto& s=*state_;s.log=std::move(log);s.host=host;s.settings=settings;s.width=d.Width;s.height=d.Height;s.format=d.Format;
    if(!host || !parent || !d.Width || !d.Height || d.SampleDesc.Count!=1 || d.ArraySize!=1 || d.MipLevels!=1 ||
       (d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT))return s.Error("source endpoint contract");
    s.hooks=testHooks?*testHooks:DriverHooks{EnableWrapper,nvs30::nvpresent::graph_launch_count,nvs30::nvpresent::retarget_count};
    ComPtr<IDXGIDevice> dx;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC ad{};
    if(FAILED(host->QueryInterface(IID_PPV_ARGS(&dx))) || FAILED(dx->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&ad)) || (!testHooks && ad.VendorId!=0x10de))return s.Error("LS output adapter");
    s.luid=ad.AdapterLuid;host->GetImmediateContext(&s.context);
    // The reference driver observes the default-device path on some systems.
    // Use it only after checking adapter zero is the exact LS output adapter;
    // always verify the resulting D3D12 LUID. Never silently use the other GPU.
    // NvPresent initializes its native factory2 path. The manager's virtual
    // output proxy hooks factory1; keep this private output on real DXGI.
    ComPtr<IDXGIFactory2> factory;if(FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))))return s.Error("fresh physical DXGI factory2");
    ComPtr<IDXGIAdapter> first;DXGI_ADAPTER_DESC firstDesc{};const bool defaultMatches=SUCCEEDED(factory->EnumAdapters(0,&first)) && SUCCEEDED(first->GetDesc(&firstDesc)) && ls::SameLuid(firstDesc.AdapterLuid,s.luid);
    const bool useDefault=settings.devicePath!=1 && defaultMatches;
    if(FAILED(D3D12CreateDevice(useDefault?nullptr:adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&s.device))))return s.Error("same-adapter D3D12 device");
    if(!ls::SameLuid(s.device->GetAdapterLuid(),s.luid))return s.Error("D3D12 LUID mismatch; second GPU is never accepted");
    char text[200];std::snprintf(text,sizeof text,"Smooth Motion: LS/D3D12 same LUID %08x:%08x; creation=%s; %ux%u format=%u",unsigned(s.luid.HighPart),s.luid.LowPart,useDefault?"verified default":"explicit adapter",d.Width,d.Height,unsigned(d.Format));if(s.log)s.log(text);
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(FAILED(s.device->CreateCommandQueue(&q,IID_PPV_ARGS(&s.queue))) || FAILED(s.device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&s.allocator))) ||
       FAILED(s.device->CreateCommandList(0,q.Type,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list))) || FAILED(s.list->Close()) || FAILED(s.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.completion))))return s.Error("D3D12 copy objects");
    s.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);s.timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);if(!s.timer)s.timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    if(!s.event || !s.window.Create(parent,d.Width,d.Height))return s.Error("separate input-transparent output HWND");
    s.tearing=!plain && settings.preferVRR && ls::TearingSupported(factory.Get());
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=d.Width;sd.Height=d.Height;sd.Format=d.Format;sd.SampleDesc.Count=1;
    sd.BufferCount=2;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.Scaling=DXGI_SCALING_STRETCH;sd.AlphaMode=DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags=s.tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    ComPtr<IDXGISwapChain1> chain;if(FAILED(factory->CreateSwapChainForHwnd(s.queue.Get(),s.window.Get(),&sd,nullptr,nullptr,&chain)) || FAILED(chain.As(&s.chain)))return s.Error("D3D12 output swapchain on distinct HWND");
    if(d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT && FAILED(s.chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)))return s.Error("scRGB output color space");
    auto td=d;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if(FAILED(host->CreateTexture2D(&td,nullptr,&s.staging)))return s.Error("shared D3D11 source");
    ComPtr<IDXGIResource1> resource;HANDLE handle=nullptr;
    if(FAILED(s.staging.As(&resource)) || FAILED(resource->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle)))return s.Error("shared NT handle");
    const HRESULT opened=s.device->OpenSharedHandle(handle,IID_PPV_ARGS(&s.shared));CloseHandle(handle);if(FAILED(opened))return s.Error("same-adapter shared resource",opened);
    D3D11_QUERY_DESC query{};query.Query=D3D11_QUERY_EVENT;if(FAILED(host->CreateQuery(&query,&s.copyDone)))return s.Error("D3D11 completion query");
    if(settings.filterDuplicates){
        s.analysis=std::make_unique<ls::SharedGpu>();s.duplicate=std::make_unique<fg::DuplicateFilter>();
        if(!s.analysis->Init(host,testHooks==nullptr) || !s.analysis->Texture(d.Width,d.Height,d.Format,D3D11_BIND_SHADER_RESOURCE,s.analysisPrivate,s.analysisHost) || !s.duplicate->Init(s.analysis->device.Get(),d))return s.Error("private duplicate filter");
    }
    s.enabled=s.hooks.enable && s.hooks.enable(s.chain.Get());s.graphsSeen=s.hooks.graphs?s.hooks.graphs():0;return true;
}
bool Backend::Prepare(ID3D11Texture2D* input){
    auto& s=*state_;if(s.lost || !input || !s.Wait())return false;
    const auto start=std::chrono::steady_clock::now();s.wasDuplicate=false;
    D3D11_TEXTURE2D_DESC d{};input->GetDesc(&d);ComPtr<ID3D11Device> sourceDevice;input->GetDevice(&sourceDevice);
    if(sourceDevice.Get()!=s.host.Get() || d.Width!=s.width || d.Height!=s.height || d.Format!=s.format)return s.Error("changed source device/shape");
    if(s.duplicate){
        if(!s.analysis->Upload(s.analysisHost.Get(),input))return s.Error("duplicate upload fence");
        const auto result=s.duplicate->Check(s.analysis->context.Get(),s.analysisPrivate.Get());
        if(result==fg::DuplicateResult::Failed)return s.Error("duplicate comparison");
        if(result==fg::DuplicateResult::Duplicate && s.confirmed){s.wasDuplicate=true;return true;}
    }
    s.Pace();s.context->CopyResource(s.staging.Get(),input);s.context->End(s.copyDone.Get());s.context->Flush();
    const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
    for(;;){const HRESULT hr=s.context->GetData(s.copyDone.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);if(hr==S_OK)break;if(FAILED(hr) || std::chrono::steady_clock::now()>=until){s.lost=true;return s.Error("D3D11 copy / 500 ms timeout",hr);}SwitchToThread();}
    if(FAILED(s.allocator->Reset()) || FAILED(s.list->Reset(s.allocator.Get(),nullptr)))return s.Error("command list reset");
    ComPtr<ID3D12Resource> back;if(FAILED(s.chain->GetBuffer(s.chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back))))return s.Error("D3D12 current backbuffer");
    D3D12_RESOURCE_BARRIER barriers[]={Transition(s.shared.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE),Transition(back.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST)};
    s.list->ResourceBarrier(2,barriers);s.list->CopyResource(back.Get(),s.shared.Get());for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);s.list->ResourceBarrier(2,barriers);
    if(FAILED(s.list->Close()))return s.Error("copy list close");ID3D12CommandList* lists[]={s.list.Get()};s.queue->ExecuteCommandLists(1,lists);
    if(FAILED(s.queue->Signal(s.completion.Get(),++s.value)) || !s.Wait()){s.lost=true;return s.Error("D3D12 copy completion / 500 ms timeout");}
    s.milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return true;
}
HRESULT Backend::Present(UINT lsSync){
    auto& s=*state_;if(s.wasDuplicate)return S_OK;
    // The copied real frame is valid even while the neural backend warms up.
    // NvPresent must see a visible surface to run its model; status remains
    // unconfirmed until a CUDA graph launch was actually observed.
    s.window.Show(true);
    if(!s.enabled && s.hooks.enable)s.enabled=s.hooks.enable(s.chain.Get());
    const UINT sync=s.settings.syncMode==2?1:s.settings.syncMode==1?std::min(lsSync,1u):0;
    const HRESULT hr=s.chain->Present(sync,ls::PresentFlags(sync,s.tearing));++s.presents;
    const uint64_t after=s.hooks.graphs?s.hooks.graphs():0;
    // CUDA can launch on a driver worker between two Presents. Compare
    // against this chain's initialization baseline, not only the duration
    // of the current Present call; stale launches from an old chain don't
    // count toward the new one.
    if(hr==S_OK && s.enabled && after>s.graphsSeen && s.hooks.retargets && s.hooks.retargets()>0){
        if(!s.confirmed && s.log)s.log("Smooth Motion: private controller enabled and new CUDA inference observed");
        s.confirmed=true;s.graphsSeen=after;s.window.Show(true);
    }
    if(!s.confirmed && s.presents>=64){
        if(!s.probeTimeout && s.log){char text[320];std::snprintf(text,sizeof text,
            "Smooth Motion warmup stopped: presents=%u controller_enabled=%u retargeted=%llu new_graphs=%llu graph_attempts=%llu last_graph_error=%d",
            s.presents,unsigned(s.enabled),static_cast<unsigned long long>(s.hooks.retargets?s.hooks.retargets():0),
            static_cast<unsigned long long>(after>=s.graphsSeen?after-s.graphsSeen:0),
            static_cast<unsigned long long>(nvs30::nvpresent::graph_attempt_count()),nvs30::nvpresent::graph_last_error());s.log(text);}
        s.probeTimeout=true;s.window.Show(false);
    }
    if(hr!=S_OK)s.window.Show(false);return hr;
}
bool Backend::Confirmed()const{return state_ && state_->confirmed;}
bool Backend::Duplicate()const{return state_ && state_->wasDuplicate;}
bool Backend::TimedOut()const{return state_ && state_->probeTimeout;}
bool Backend::Tearing()const{return state_ && state_->tearing;}
double Backend::Milliseconds()const{return state_?state_->milliseconds:0;}
LUID Backend::AdapterLuid()const{return state_?state_->luid:LUID{};}
void Backend::Hide(){if(state_)state_->window.Show(false);}
#ifdef LS_SM_TEST
IDXGISwapChain3* Backend::TestChain()const{return state_?state_->chain.Get():nullptr;}
#endif
}
