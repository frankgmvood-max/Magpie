// SPDX-License-Identifier: GPL-3.0-only
// The SDR display shader is adapted from Magpie 0.6.9 HdrSurfaceAdapter.cpp,
// mode 8. TrueHDR evaluation itself uses the unchanged upstream SDK bridge.
#include "backend.h"
#include <shared_gpu.h>
#include "duplicates.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <cstring>
namespace hdr {
using Microsoft::WRL::ComPtr;
namespace {
constexpr char toneShader[]=R"(
cbuffer Options:register(b0){float exposure;float whiteNits;float shoulder;float pad;};
Texture2D<float4> input:register(t0);
float Encode(float v){v=saturate(v);return v<=0.0031308?v*12.92:1.055*pow(v,1.0/2.4)-0.055;}
float4 VS(uint v:SV_VertexID):SV_Position{return float4((v==2?3:-1),(v==1?-3:1),0,1);}
float4 PS(float4 p:SV_Position):SV_Target{
 float3 color=max(input.Load(int3(p.xy,0)).rgb,0)*exposure/(whiteNits/80.0);
 float l=dot(color,float3(0.2126,0.7152,0.0722));
 float3 mapped=color/(1+l/max(shoulder,0.001));
 return float4(Encode(mapped.r),Encode(mapped.g),Encode(mapped.b),1);
})";
bool Compile(const char* entry,const char* target,ComPtr<ID3DBlob>& out){
    ComPtr<ID3DBlob> error;return SUCCEEDED(D3DCompile(toneShader,sizeof(toneShader)-1,nullptr,nullptr,nullptr,entry,target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&out,&error));
}
}
struct Backend::State {
    ls::SharedGpu gpu;
    ComPtr<ID3D11Texture2D> input,inputHost,hdr,hdrHost,sdr,sdrHost,output;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11ShaderResourceView> hdrView;
    ComPtr<ID3D11RenderTargetView> sdrTarget;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11Buffer> constants;
    std::unique_ptr<fg::DuplicateFilter> duplicate;
    Provider provider{};HMODULE module=nullptr;void* instance=nullptr;
    std::function<void(const char*)> log;
    unsigned width=0,height=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    bool faulted=false,wasDuplicate=false,lastHdrOutput=false,valid=false;
    MagpieRtxHdrSettings lastSettings{};
    float lastWhite=0,lastExposure=0,lastShoulder=0;
    double milliseconds=0;
    ~State(){
        if(instance && provider.destroy && FAILED(provider.destroy(instance)))faulted=true;
        // NVIDIA may retain callbacks after an exception. The SDK bridge is
        // pinned in that case; executable code cannot disappear underneath it.
        if(module && !faulted)FreeLibrary(module);
    }
    bool Error(const char* step,HRESULT hr=E_FAIL,uint32_t status=0){
        char text[256];std::snprintf(text,sizeof text,"RTX HDR: %s failed: HRESULT=0x%08x NVIDIA/exception=0x%08x; LS output preserved",step,unsigned(hr),status);
        if(log)log(text);return false;
    }
};
Backend::Backend()=default;
Backend::~Backend(){if(state_ && (state_->gpu.lost || !state_->gpu.Wait()))(void)state_.release();}
bool Backend::Init(ID3D11Device* device,const D3D11_TEXTURE2D_DESC& d,const std::wstring& directory,
                  const Settings& settings,std::function<void(const char*)> log,const Provider* testProvider){
    state_=std::make_unique<State>();auto& s=*state_;s.log=std::move(log);s.width=d.Width;s.height=d.Height;s.format=d.Format;
    if(!d.Width || !d.Height || d.MipLevels!=1 || d.ArraySize!=1 || d.SampleDesc.Count!=1 ||
       (d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM))return s.Error("SDR endpoint contract");
    if(!s.gpu.Init(device,testProvider==nullptr))return s.Error("private device on LS output adapter");
    if(!s.gpu.Texture(d.Width,d.Height,d.Format,D3D11_BIND_SHADER_RESOURCE,s.input,s.inputHost) ||
       !s.gpu.Texture(d.Width,d.Height,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE,s.hdr,s.hdrHost) ||
       !s.gpu.Texture(d.Width,d.Height,d.Format,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,s.sdr,s.sdrHost))return s.Error("shared SDR/scRGB surfaces");
    if(FAILED(s.gpu.device->CreateShaderResourceView(s.hdr.Get(),nullptr,&s.hdrView)) ||
       FAILED(s.gpu.device->CreateRenderTargetView(s.sdr.Get(),nullptr,&s.sdrTarget)))return s.Error("private HDR views");
    ComPtr<ID3DBlob> vs,ps;
    if(!Compile("VS","vs_5_0",vs) || !Compile("PS","ps_5_0",ps) ||
       FAILED(s.gpu.device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&s.vertex)) ||
       FAILED(s.gpu.device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&s.pixel)))return s.Error("Magpie SDR display tone map shader");
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(s.gpu.device->CreateBuffer(&cb,nullptr,&s.constants)))return s.Error("tone map parameters");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    if(FAILED(s.gpu.device->CreateRasterizerState(&raster,&s.rasterizer)))return s.Error("private tone map rasterizer");
    if(settings.duplicateFiltering){s.duplicate=std::make_unique<fg::DuplicateFilter>();if(!s.duplicate->Init(s.gpu.device.Get(),d))return s.Error("duplicate filter");}
    if(testProvider)s.provider=*testProvider;
    else {
        const auto path=std::filesystem::path(directory)/L"LS_RtxVideoRuntime.dll";
        s.module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!s.module)return s.Error("load isolated RTX Video SDK bridge",HRESULT_FROM_WIN32(GetLastError()));
        s.provider.create=reinterpret_cast<RtxHdrCreateFn>(GetProcAddress(s.module,"MagpieRtxHdrCreate"));
        s.provider.draw=reinterpret_cast<RtxHdrDrawFn>(GetProcAddress(s.module,"MagpieRtxHdrDraw"));
        s.provider.destroy=reinterpret_cast<RtxHdrDestroyFn>(GetProcAddress(s.module,"MagpieRtxHdrDestroy"));
    }
    if(!s.provider.create || !s.provider.draw || !s.provider.destroy)return s.Error("TrueHDR bridge exports");
    const auto runtime=(std::filesystem::path(directory)/L"runtime").wstring();uint32_t status=0;
    const HRESULT hr=s.provider.create(s.gpu.device.Get(),runtime.c_str(),&s.instance,&status);
    if(FAILED(hr)){s.faulted=hr==E_UNEXPECTED;return s.Error("TrueHDR availability / creation",hr,status);}
    char text[200];std::snprintf(text,sizeof text,"RTX HDR: isolated RTX Video SDK 1.1; LS adapter LUID %08x:%08x; %ux%u SDR -> linear scRGB FP16",unsigned(s.gpu.adapterDesc.AdapterLuid.HighPart),s.gpu.adapterDesc.AdapterLuid.LowPart,d.Width,d.Height);
    if(s.log)s.log(text);return true;
}
bool Backend::Process(ID3D11Texture2D* input,const Settings& settings,bool hdrOutput){
    auto& s=*state_;if(s.faulted || s.gpu.lost || !input)return false;
    D3D11_TEXTURE2D_DESC d{};input->GetDesc(&d);
    if(d.Width!=s.width || d.Height!=s.height || d.Format!=s.format || d.SampleDesc.Count!=1)return s.Error("changed input endpoint");
    const auto start=std::chrono::steady_clock::now();s.wasDuplicate=false;
    const MagpieRtxHdrSettings values{settings.contrast,settings.saturation,settings.middleGray,settings.peakNits};
    const bool sameSettings=s.valid && s.lastHdrOutput==hdrOutput && std::memcmp(&values,&s.lastSettings,sizeof values)==0 &&
        settings.sdrWhiteNits==s.lastWhite && settings.exposure==s.lastExposure && settings.shoulder==s.lastShoulder;
    if(!s.gpu.Upload(s.inputHost.Get(),input))return s.Error("SDR upload fence");
    if(s.duplicate){
        if(!sameSettings)s.duplicate->Reset();
        const auto result=s.duplicate->Check(s.gpu.context.Get(),s.input.Get());
        if(result==fg::DuplicateResult::Failed)return s.Error("duplicate comparison");
        if(result==fg::DuplicateResult::Duplicate){s.wasDuplicate=true;s.milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return true;}
    }
    s.gpu.context->ClearState();uint32_t status=0;
    const HRESULT hr=s.provider.draw(s.instance,s.input.Get(),s.hdr.Get(),&values,&status);
    s.gpu.context->ClearState();
    if(FAILED(hr)){s.faulted=hr==E_UNEXPECTED;return s.Error("TrueHDR evaluation",hr,status);}
    if(!hdrOutput){
        const float toneValues[4]={settings.exposure,settings.sdrWhiteNits,settings.shoulder,0};
        s.gpu.context->UpdateSubresource(s.constants.Get(),0,nullptr,toneValues,0,0);
        ID3D11RenderTargetView* rt=s.sdrTarget.Get();ID3D11ShaderResourceView* source=s.hdrView.Get();ID3D11Buffer* cb=s.constants.Get();
        s.gpu.context->OMSetRenderTargets(1,&rt,nullptr);
        const D3D11_VIEWPORT viewport{0,0,float(s.width),float(s.height),0,1};s.gpu.context->RSSetViewports(1,&viewport);
        s.gpu.context->RSSetState(s.rasterizer.Get());
        s.gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);s.gpu.context->VSSetShader(s.vertex.Get(),nullptr,0);
        s.gpu.context->PSSetShader(s.pixel.Get(),nullptr,0);s.gpu.context->PSSetShaderResources(0,1,&source);s.gpu.context->PSSetConstantBuffers(0,1,&cb);
        s.gpu.context->Draw(3,0);s.gpu.context->ClearState();
    }
    if(!s.gpu.Done())return s.Error("HDR completion / 500 ms timeout");
    s.output=hdrOutput?s.hdrHost:s.sdrHost;s.lastSettings=values;s.lastHdrOutput=hdrOutput;s.lastWhite=settings.sdrWhiteNits;s.lastExposure=settings.exposure;s.lastShoulder=settings.shoulder;s.valid=true;
    s.milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return true;
}
ID3D11Texture2D* Backend::Output()const{return state_?state_->output.Get():nullptr;}
double Backend::Milliseconds()const{return state_?state_->milliseconds:0;}
bool Backend::Duplicate()const{return state_ && state_->wasDuplicate;}
}
