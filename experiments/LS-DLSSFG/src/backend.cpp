// SPDX-License-Identifier: MIT
#include "backend.h"
#include "optical_flow.h"
#include "duplicates.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_dlssg.h>
#include <atomic>
#include <filesystem>
#include <cstdio>
#include <utility>
#include <chrono>
#include <limits>

using Microsoft::WRL::ComPtr;
namespace fg {
namespace {
std::atomic<bool> ngxFault{false};
template<class F> NVSDK_NGX_Result Guard(const F& fn) {
    if (ngxFault.load()) return NVSDK_NGX_Result_FAIL_PlatformError;
    __try { return fn(); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        ngxFault.store(true);
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* p, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {p, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b}; return x;
}
bool Shared(ID3D11Device* d11, ID3D12Device* d12, D3D11_TEXTURE2D_DESC desc,
            bool output, ComPtr<ID3D11Texture2D>& p11, ComPtr<ID3D12Resource>& p12,
            ID3D11Device1* peer=nullptr,ComPtr<ID3D11Texture2D>* peerTexture=nullptr) {
    desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (output ? D3D11_BIND_UNORDERED_ACCESS : 0);
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (FAILED(d11->CreateTexture2D(&desc, nullptr, &p11))) return false;
    ComPtr<IDXGIResource1> dxgi;
    if (FAILED(p11.As(&dxgi))) return false;
    HANDLE h = nullptr;
    if (FAILED(dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h))) return false;
    HRESULT hr = d12->OpenSharedHandle(h, IID_PPV_ARGS(&p12));
    if(SUCCEEDED(hr) && peer && peerTexture) hr=peer->OpenSharedResource1(h,IID_PPV_ARGS(&*peerTexture));
    CloseHandle(h); return SUCCEEDED(hr);
}
bool Import(ID3D12Device* device,ID3D11Texture2D* texture,ComPtr<ID3D12Resource>& target) {
    ComPtr<IDXGIResource1> dxgi;HANDLE handle=nullptr;
    if(FAILED(texture->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle))) return false;
    const HRESULT hr=device->OpenSharedHandle(handle,IID_PPV_ARGS(&target));CloseHandle(handle);return SUCCEEDED(hr);
}
void Identity(float a[4][4]) { for (unsigned i=0;i<4;++i) a[i][i]=1; }
}
struct Backend::State {
    ComPtr<ID3D11Device5> d11;
    ComPtr<ID3D11DeviceContext4> c11;
    ComPtr<ID3D11Device5> privateDevice;
    ComPtr<ID3D11DeviceContext4> privateContext;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D11Texture2D> input11, privateInput, output11[3];
    ComPtr<ID3D12Resource> input12, output12[3], motion, opticalMotion, depth, disable, readback;
    ComPtr<ID3D11Fence> privateInputFence,privateReady11;
    ComPtr<ID3D12Fence> privateReady12;
    ComPtr<ID3D12DescriptorHeap> heap, cpuHeap;
    ComPtr<ID3D11Fence> in11, out11;
    ComPtr<ID3D12Fence> in12, out12;
    HANDLE event = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    Log log;
    Settings settings;
    std::unique_ptr<OpticalFlow> flow;
    std::unique_ptr<DuplicateFilter> duplicate;
    unsigned multiplier=2,maxMultiplier=2,outputCount=0;
    bool flowUsable=false,realMotion=false,hdr=false,runtimeRejected=false;
    uint64_t privateValue=0;
    double preprocessMs=0;
    UINT width = 0, height = 0;
    uint64_t inputValue = 0, outputValue = 0, frame = 0;
    bool initialized = false, lost = false;

    bool Error(const char* where, unsigned code = 0) {
        char msg[240]; std::snprintf(msg, sizeof msg, "DLSS FG: %s failed (0x%08x%s); passing LS frames through",
            where, code, ngxFault.load() ? ", NGX exception" : "");
        if (log) log(msg); return false;
    }
    bool Wait() {
        if (!outputValue) return true;
        const auto completed = out12->GetCompletedValue();
        if (completed == UINT64_MAX) { lost = true; return false; }
        if (completed >= outputValue) return true;
        ResetEvent(event);
        if (FAILED(out12->SetEventOnCompletion(outputValue, event)) ||
            WaitForSingleObject(event, 500) != WAIT_OBJECT_0 ||
            out12->GetCompletedValue() == UINT64_MAX || out12->GetCompletedValue() < outputValue) {
            lost = true; return Error("GPU completion / 500 ms timeout");
        }
        return true;
    }
    bool Submit() {
        if (FAILED(cmd->Close())) return Error("command list close");
        ID3D12CommandList* lists[] = {cmd.Get()}; q->ExecuteCommandLists(1, lists);
        if (FAILED(q->Signal(out12.Get(), ++outputValue))) { lost = true; return Error("GPU signal"); }
        return Wait();
    }
    bool Fence(ComPtr<ID3D11Fence>& a, ComPtr<ID3D12Fence>& b,bool incoming=false) {
        if (FAILED(d11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&a)))) return false;
        HANDLE h = nullptr;
        if (FAILED(a->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h))) return false;
        HRESULT hr = d12->OpenSharedHandle(h, IID_PPV_ARGS(&b));
        if(SUCCEEDED(hr) && incoming) hr=privateDevice->OpenSharedFence(h,IID_PPV_ARGS(&privateInputFence));
        CloseHandle(h);
        return SUCCEEDED(hr);
    }
    bool PrivateFence() {
        HANDLE h=nullptr;
        if(FAILED(privateDevice->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&privateReady11)))) return false;
        if(FAILED(privateReady11->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&h))) return false;
        const HRESULT hr=d12->OpenSharedHandle(h,IID_PPV_ARGS(&privateReady12));CloseHandle(h);return SUCCEEDED(hr);
    }
    bool WaitPrivate() {
        if(!privateValue) return true;
        if(privateReady11->GetCompletedValue()==UINT64_MAX) {lost=true;return false;}
        if(privateReady11->GetCompletedValue()>=privateValue) return true;
        ResetEvent(event);
        if(FAILED(privateReady11->SetEventOnCompletion(privateValue,event)) || WaitForSingleObject(event,500)!=WAIT_OBJECT_0 ||
           privateReady11->GetCompletedValue()==UINT64_MAX || privateReady11->GetCompletedValue()<privateValue) {lost=true;return false;}
        return true;
    }
    bool ZeroTexture(DXGI_FORMAT format, unsigned index, ComPtr<ID3D12Resource>& dst) {
        D3D12_RESOURCE_DESC r{}; r.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        r.Width=width; r.Height=height; r.DepthOrArraySize=1; r.MipLevels=1;
        r.Format=format; r.SampleDesc.Count=1; r.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES h{}; h.Type=D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(d12->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &r,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dst)))) return false;
        auto cpu=cpuHeap->GetCPUDescriptorHandleForHeapStart();
        auto visibleCpu=heap->GetCPUDescriptorHandleForHeapStart();
        auto gpu=heap->GetGPUDescriptorHandleForHeapStart();
        const unsigned stride=d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cpu.ptr+=index*stride; visibleCpu.ptr+=index*stride; gpu.ptr+=index*stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format=format; u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        d12->CreateUnorderedAccessView(dst.Get(), nullptr, &u, cpu);
        // ClearUAV requires its CPU descriptor in a NON shader-visible heap.
        d12->CopyDescriptorsSimple(1,visibleCpu,cpu,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const float zeros[4]{}; cmd->ClearUnorderedAccessViewFloat(gpu,cpu,dst.Get(),zeros,0,nullptr);
        auto barrier=Transition(dst.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1,&barrier); return true;
    }
    ~State() {
        if (feature) Guard([&]{ return NVSDK_NGX_D3D12_ReleaseFeature(feature); });
        if (params) Guard([&]{ return NVSDK_NGX_D3D12_DestroyParameters(params); });
        if (initialized) Guard([&]{ return NVSDK_NGX_D3D12_Shutdown1(d12.Get()); });
        flow.reset();
        if (event) CloseHandle(event);
    }
};
Backend::Backend() = default;
Backend::~Backend() {
    // Keep outstanding GPU/NGX objects alive on a fault. The DLL is pinned by
    // PresentHook and LS restart is required; releasing these could crash LS.
    if (s_ && (s_->lost || ngxFault.load() || !s_->Wait() || !s_->WaitPrivate())) (void)s_.release();
}
bool Backend::Init(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& desc,
                   const std::wstring& runtime, const Settings& settings,Log log) {
    s_ = std::make_unique<State>(); auto& s=*s_; s.log=std::move(log);
    s.settings=settings;
    s.width=desc.Width; s.height=desc.Height;
    s.hdr=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&s.d11)))) return s.Error("D3D11.4 device");
    ComPtr<ID3D11DeviceContext> ctx; dev->GetImmediateContext(&ctx);
    if (FAILED(ctx.As(&s.c11))) return s.Error("D3D11.4 context");
    ComPtr<IDXGIDevice> dx; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC ad{};
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dx))) || FAILED(dx->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&ad))) return s.Error("adapter query");
    if (ad.VendorId!=0x10de) return s.Error("LS output adapter is not NVIDIA");
    // Only Lossless_original.dll's CreateDevice import is watched by EAM.
    // This device/context is deliberately private and cannot become an LS
    // dispatch source or modify LS's bound compute/pixel resources.
    ComPtr<ID3D11Device> privateDevice;ComPtr<ID3D11DeviceContext> privateContext;
    if(FAILED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&privateDevice,nullptr,&privateContext)) ||
        FAILED(privateDevice.As(&s.privateDevice)) || FAILED(privateContext.As(&s.privateContext))) return s.Error("private preprocessing device");
    char text[200]; std::snprintf(text,sizeof text,"DLSS FG: LS output adapter LUID %08x:%08x, %ux%u, format %u",
        unsigned(ad.AdapterLuid.HighPart),ad.AdapterLuid.LowPart,s.width,s.height,unsigned(desc.Format)); s.log(text);
    if (FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&s.d12)))) return s.Error("D3D12 device");
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(s.d12->CreateCommandQueue(&qd,IID_PPV_ARGS(&s.q))) ||
        FAILED(s.d12->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&s.alloc))) ||
        FAILED(s.d12->CreateCommandList(0,qd.Type,s.alloc.Get(),nullptr,IID_PPV_ARGS(&s.cmd)))) return s.Error("command objects");
    s.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!s.event || !s.Fence(s.in11,s.in12,true) || !s.Fence(s.out11,s.out12) || !s.PrivateFence()) return s.Error("shared fences");
    if (!Shared(dev,s.d12.Get(),desc,false,s.input11,s.input12,s.privateDevice.Get(),&s.privateInput)) return s.Error("shared input");
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors=2; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(s.d12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.heap)))) return s.Error("descriptor heap");
    hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(s.d12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.cpuHeap)))) return s.Error("CPU descriptor heap");
    ID3D12DescriptorHeap* heaps[]={s.heap.Get()}; s.cmd->SetDescriptorHeaps(1,heaps);
    if (!s.ZeroTexture(DXGI_FORMAT_R16G16_FLOAT,0,s.motion) || !s.ZeroTexture(DXGI_FORMAT_R32_FLOAT,1,s.depth)) return s.Error("virtual guidance");
    D3D12_RESOURCE_DESC br{}; br.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    br.Width=4; br.Height=1; br.DepthOrArraySize=1; br.MipLevels=1; br.SampleDesc.Count=1;
    br.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; br.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(s.d12->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&br,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&s.disable)))) return s.Error("disable output");
    hp.Type=D3D12_HEAP_TYPE_READBACK; br.Flags=D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(s.d12->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&br,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.readback)))) return s.Error("flag readback");
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
    const auto app=std::filesystem::path(executable).parent_path().wstring();
    const wchar_t* paths[]={runtime.c_str(),app.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo.Path=paths; info.PathListInfo.Length=2;
    auto result=Guard([&]{return NVSDK_NGX_D3D12_Init_with_ProjectID("eae67294-65c6-4bb7-a34a-51df0b8c95de",
        NVSDK_NGX_ENGINE_TYPE_CUSTOM,"LS-DLSSFG-0.1",runtime.c_str(),s.d12.Get(),&info,NVSDK_NGX_Version_API);});
    if (!NVSDK_NGX_SUCCEED(result)) return s.Error("NGX initialization",unsigned(result));
    s.initialized=true;
    result=Guard([&]{return NVSDK_NGX_D3D12_GetCapabilityParameters(&s.params);});
    if (!NVSDK_NGX_SUCCEED(result) || !s.params) return s.Error("NGX parameters",unsigned(result));
    int available=0;
    result=Guard([&]{return NVSDK_NGX_Parameter_GetI(s.params,NVSDK_NGX_Parameter_FrameGeneration_Available,&available);});
    if (!NVSDK_NGX_SUCCEED(result) || !available) {
        int reason=0; Guard([&]{return NVSDK_NGX_Parameter_GetI(s.params,NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult,&reason);});
        return s.Error("FG unavailable in current runtime/GPU",unsigned(reason));
    }
    unsigned maxFrames=1;
    result=Guard([&]{return NVSDK_NGX_Parameter_GetUI(s.params,NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,&maxFrames);});
    if(!NVSDK_NGX_SUCCEED(result)) maxFrames=1;
    s.maxMultiplier=SupportedMultiplier(4,maxFrames);
    s.multiplier=SupportedMultiplier(settings.multiplier,maxFrames);
    for(unsigned i=0;i<s.multiplier-1;++i)
        if(!Shared(dev,s.d12.Get(),desc,true,s.output11[i],s.output12[i])) return s.Error("shared MFG output");
    const unsigned unused=NVSDK_NGX_DLSSG_ResourceFlags_HUDLess | NVSDK_NGX_DLSSG_ResourceFlags_UI |
        NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha | NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField |
        NVSDK_NGX_DLSSG_ResourceFlags_OutputReal;
    result=Guard([&]{NVSDK_NGX_Parameter_SetUI(s.params,NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags,unused); return NVSDK_NGX_Result_Success;});
    if (!NVSDK_NGX_SUCCEED(result)) return s.Error("NGX resource flags",unsigned(result));
    NVSDK_NGX_DLSSG_Create_Params cp{}; cp.Width=s.width; cp.Height=s.height;
    cp.RenderWidth=s.width; cp.RenderHeight=s.height; cp.NativeBackbufferFormat=desc.Format;
    result=Guard([&]{return NGX_D3D12_CREATE_DLSSG(s.cmd.Get(),1,1,&s.feature,s.params,&cp);});
    if (!NVSDK_NGX_SUCCEED(result) || !s.feature) return s.Error("FG creation",unsigned(result));
    if (!s.Submit()) return false;
    if(settings.duplicateFiltering) {
        s.duplicate=std::make_unique<DuplicateFilter>();
        if(!s.duplicate->Init(s.privateDevice.Get(),desc)) {s.duplicate.reset();s.log("Duplicate Frame Filtering unavailable; treating all LS frames as new");}
    }
    if(settings.flow==FlowMethod::Nvidia) {
        s.flow=std::make_unique<OpticalFlow>();
        s.flowUsable=s.flow->Init(s.privateDevice.Get(),desc,settings.flowQuality,s.log,settings.flowScale);
        if(s.flowUsable && !Import(s.d12.Get(),s.flow->Motion(),s.opticalMotion)) s.flowUsable=false;
        if(!s.flowUsable) s.log("NVOF unavailable; using zero motion until settings are reapplied (LS remains active)");
    }
    char ready[192];std::snprintf(ready,sizeof ready,"DLSS FG initialized: requested x%u, effective x%u, runtime max x%u; flow=%s, duplicate filter=%d, zero engine depth",
        settings.multiplier,s.multiplier,s.maxMultiplier,s.flowUsable?"NVOF":"None",int(bool(s.duplicate)));s.log(ready);
    return true;
}
Result Backend::Generate(ID3D11Texture2D* input, bool reset) {
    auto& s=*s_;
    s.outputCount=0;s.realMotion=false;s.runtimeRejected=false;s.preprocessMs=0;
    if (s.lost || ngxFault.load() || !s.Wait()) return Result::Failed;
    s.c11->CopyResource(s.input11.Get(),input);
    if (FAILED(s.c11->Signal(s.in11.Get(),++s.inputValue))) return Result::Failed;
    s.c11->Flush();
    if(FAILED(s.privateContext->Wait(s.privateInputFence.Get(),s.inputValue))) return Result::Failed;
    const auto preStart=std::chrono::steady_clock::now();
    if(s.duplicate) {
        // A history reset must reach NGX even if the resumed scene has exactly
        // the same pixels as the last frame before suspension.
        if(reset)s.duplicate->Reset();
        const auto duplicate=s.duplicate->Check(s.privateContext.Get(),s.privateInput.Get());
        if(duplicate==DuplicateResult::Duplicate){s.preprocessMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-preStart).count();return Result::Duplicate;}
        if(duplicate==DuplicateResult::Failed) {
            s.duplicate.reset();s.log("Duplicate filter readback failed; filter disabled until restart, LS frames retained");
        }
    }
    if(s.flowUsable && !s.flow->Process(s.privateInput.Get(),reset,s.realMotion)) {
        s.flowUsable=false;s.realMotion=false;reset=true;
        s.log("NVOF failed; resetting DLSS history and falling back to zero motion");
    }
    if(FAILED(s.privateContext->Signal(s.privateReady11.Get(),++s.privateValue))) return Result::Failed;
    s.privateContext->Flush();
    if(FAILED(s.q->Wait(s.privateReady12.Get(),s.privateValue))) return Result::Failed;
    s.preprocessMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-preStart).count();
    const auto plan=Plan(s.multiplier,reset);
    const auto frame=++s.frame;
    // Every intermediate in a group uses this same base-frame id and input.
    for(unsigned index=0;index<plan.count;++index) {
    if(FAILED(s.alloc->Reset()) || FAILED(s.cmd->Reset(s.alloc.Get(),nullptr))) return Result::Failed;
    D3D12_RESOURCE_BARRIER barriers[]={Transition(s.input12.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(s.output12[index].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    s.cmd->ResourceBarrier(2,barriers);
    if(s.flowUsable) {auto b=Transition(s.opticalMotion.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);s.cmd->ResourceBarrier(1,&b);}
    auto result=Guard([&]{NVSDK_NGX_Parameter_SetULL(s.params,NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,frame); return NVSDK_NGX_Result_Success;});
    if (!NVSDK_NGX_SUCCEED(result)) return Result::Failed;
    NVSDK_NGX_D3D12_DLSSG_Eval_Params ep{}; ep.pBackbuffer=s.input12.Get(); ep.pMVecs=s.flowUsable?s.opticalMotion.Get():s.motion.Get(); ep.pDepth=s.depth.Get();
    ep.pOutputInterpFrame=s.output12[index].Get(); ep.pOutputDisableInterpolation=s.disable.Get();
    NVSDK_NGX_DLSSG_Opt_Eval_Params op{}; op.multiFrameCount=plan.count; op.multiFrameIndex=index+1; op.reset=plan.reset;
    op.cameraMotionIncluded=s.realMotion;
    op.motionVectorsDilated=s.realMotion;
    // Dense NVOF writes every pixel; stationary zero vectors are valid too.
    op.motionVectorsInvalidValue=s.realMotion?std::numeric_limits<float>::max():0;
    op.colorBuffersHDR=s.hdr;
    Identity(op.cameraViewToClip); Identity(op.clipToCameraView); Identity(op.clipToLensClip); Identity(op.clipToPrevClip); Identity(op.prevClipToClip);
    op.mvecScale[0]=op.mvecScale[1]=1; op.cameraUp[1]=op.cameraRight[0]=op.cameraFwd[2]=1;
    op.cameraNear=0.1f; op.cameraFar=1000; op.cameraFOV=1.04719755f; op.cameraAspectRatio=float(s.width)/s.height;
    op.mvecsSubrectSize=op.depthSubrectSize=op.backbufferSubrectSize=op.outputInterpSubrectSize={s.width,s.height};
    result=Guard([&]{return NGX_D3D12_EVALUATE_DLSSG(s.cmd.Get(),s.feature,s.params,&ep,&op);});
    if (!NVSDK_NGX_SUCCEED(result)) { s.Error("FG evaluate",unsigned(result)); return Result::Failed; }
    auto b=Transition(s.disable.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    s.cmd->ResourceBarrier(1,&b); s.cmd->CopyBufferRegion(s.readback.Get(),0,s.disable.Get(),0,4);
    std::swap(b.Transition.StateBefore,b.Transition.StateAfter); s.cmd->ResourceBarrier(1,&b);
    for(auto& x:barriers) std::swap(x.Transition.StateBefore,x.Transition.StateAfter);
    s.cmd->ResourceBarrier(2,barriers);
    if(s.flowUsable) {auto motionBarrier=Transition(s.opticalMotion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);s.cmd->ResourceBarrier(1,&motionBarrier);}
    if (!s.Submit() || FAILED(s.c11->Wait(s.out11.Get(),s.outputValue))) return Result::Failed;
    void* mapped=nullptr; const D3D12_RANGE read={0,4};
    if (FAILED(s.readback->Map(0,&read,&mapped)) || !mapped) return Result::Failed;
    const bool disabled=*static_cast<const unsigned char*>(mapped)!=0;
    const D3D12_RANGE written={0,0}; s.readback->Unmap(0,&written);
    if(disabled || reset) {s.runtimeRejected=disabled && !reset;s.outputCount=0;return Result::HistoryOnly;}
    ++s.outputCount;
    }
    return Result::Ready;
}
ID3D11Texture2D* Backend::Output(unsigned index) const {return s_ && index<s_->outputCount?s_->output11[index].Get():nullptr;}
unsigned Backend::OutputCount() const {return s_?s_->outputCount:0;}
unsigned Backend::Multiplier() const {return s_?s_->multiplier:2;}
unsigned Backend::MaxMultiplier() const {return s_?s_->maxMultiplier:2;}
unsigned Backend::FlowQuality() const {return s_ && s_->flowUsable?s_->flow->Quality():0;}
bool Backend::RealMotion() const {return s_ && s_->realMotion;}
double Backend::PreprocessMilliseconds() const {return s_?s_->preprocessMs:0;}
bool Backend::OutputDisabledByRuntime() const {return s_ && s_->runtimeRejected;}
}
