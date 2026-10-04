// SPDX-License-Identifier: MIT
// NVOF conventions/profile choices reviewed against Magpie 0.6.9 and NVIDIA's
// programming guide. This implementation uses an addon-owned D3D11 context.
#include "optical_flow.h"
#include <nvOpticalFlowD3D11.h>
#include <vector>
#include <algorithm>
#include <cstdio>
namespace fg {
#ifdef FG_TEST_NVOF
const NV_OF_D3D11_API_FUNCTION_LIST* testOpticalFlowApi=nullptr;
void SetOpticalFlowTestApi(const NV_OF_D3D11_API_FUNCTION_LIST* api) {testOpticalFlowApi=api;}
#endif
namespace {
template<class F> NV_OF_STATUS Call(const F& fn) {
    __try {return fn();} __except(EXCEPTION_EXECUTE_HANDLER) {return NV_OF_ERR_GENERIC;}
}
constexpr char convert[]=R"(
Texture2D<float4> image : register(t0);
cbuffer Format : register(b0) {uint hdr;uint3 padding;};
float4 vs(uint vertex:SV_VertexID):SV_Position {
    float2 p=float2((vertex<<1)&2,vertex&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float4 ps(float4 position:SV_Position):SV_Target {
    float3 c=image.Load(int3(int2(position.xy),0)).rgb;
    // Only the OF analysis image is mapped to SDR. LS/DLSS colour is untouched.
    if(hdr) c=max(c,0)/(1+max(c,0));
    return float4(saturate(c),1);
})";
constexpr char dense[]=R"(
Texture2D<int2> coarse : register(t0);
RWTexture2D<float2> motion : register(u0);
cbuffer Layout : register(b0) {uint width;uint height;uint grid;uint padding;};
float2 vectorAt(int2 p) {
    uint w,h;coarse.GetDimensions(w,h);
    return float2(coarse.Load(int3(clamp(p,int2(0,0),int2(w,h)-1),0)))/32.0;
}
[numthreads(8,8,1)]
void main(uint3 pixel:SV_DispatchThreadID) {
    if(pixel.x>=width || pixel.y>=height) return;
    float2 location=(float2(pixel.xy)+0.5)/grid-0.5;
    int2 cell=int2(floor(location));float2 fraction=frac(location);
    float2 top=lerp(vectorAt(cell),vectorAt(cell+int2(1,0)),fraction.x);
    float2 bottom=lerp(vectorAt(cell+int2(0,1)),vectorAt(cell+int2(1,1)),fraction.x);
    // input=current, reference=previous: displacement is current-to-previous.
    motion[pixel.xy]=lerp(top,bottom,fraction.y);
})";
bool Compile(const char* entry,const char* target,Ptr<ID3DBlob>& blob) {
    Ptr<ID3DBlob> error;
    return SUCCEEDED(D3DCompile(convert,sizeof(convert)-1,"OF input",nullptr,nullptr,entry,target,
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error));
}
}
struct OpticalFlow::State {
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> input[2],coarse,motion;
    Ptr<ID3D11ShaderResourceView> coarseView;
    Ptr<ID3D11RenderTargetView> targets[2];
    Ptr<ID3D11UnorderedAccessView> motionView;
    Ptr<ID3D11ComputeShader> densify;
    Ptr<ID3D11VertexShader> vertex;
    Ptr<ID3D11PixelShader> pixel;
    Ptr<ID3D11Buffer> params,format;
    HMODULE module=nullptr;
    NV_OF_D3D11_API_FUNCTION_LIST api{};
    NvOFHandle session=nullptr;
    NvOFGPUBufferHandle registered[3]{};
    unsigned width=0,height=0,grid=4,quality=2,previous=0;
    bool history=false,temporalReset=true;
    DXGI_FORMAT inputFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
    Log log;
    ~State() {
        for(auto handle:registered) if(handle && api.nvOFUnregisterResourceD3D11)
            Call([&]{return api.nvOFUnregisterResourceD3D11(handle);});
        if(session && api.nvOFDestroy) Call([&]{return api.nvOFDestroy(session);});
        if(module) FreeLibrary(module);
    }
    bool Formats(NV_OF_BUFFER_USAGE usage,DXGI_FORMAT required) {
        uint32_t count=0;
        if(Call([&]{return api.nvOFGetSurfaceFormatCountD3D11(session,usage,NV_OF_MODE_OPTICALFLOW,&count);})!=NV_OF_SUCCESS || !count || count>64) return false;
        std::vector<DXGI_FORMAT> formats(count);
        return Call([&]{return api.nvOFGetSurfaceFormatD3D11(session,usage,NV_OF_MODE_OPTICALFLOW,formats.data());})==NV_OF_SUCCESS &&
            std::find(formats.begin(),formats.end(),required)!=formats.end();
    }
};
OpticalFlow::OpticalFlow()=default;
OpticalFlow::~OpticalFlow()=default;
bool OpticalFlow::Init(ID3D11Device* device,const D3D11_TEXTURE2D_DESC& desc,unsigned quality,Log log) {
    s_=std::make_unique<State>();auto& s=*s_;s.device=device;device->GetImmediateContext(&s.context);
    s.width=desc.Width;s.height=desc.Height;s.quality=std::clamp(quality,1u,5u);s.log=std::move(log);
    bool apiReady=false;
#ifdef FG_TEST_NVOF
    if(testOpticalFlowApi) {s.api=*testOpticalFlowApi;apiReady=true;}
    else
#endif
    {
        s.module=LoadLibraryExW(L"nvofapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!s.module) return false;
        using CreateApi=NV_OF_STATUS(NVOFAPI*)(uint32_t,NV_OF_D3D11_API_FUNCTION_LIST*);
        const auto create=reinterpret_cast<CreateApi>(GetProcAddress(s.module,"NvOFAPICreateInstanceD3D11"));
        apiReady=create && Call([&]{return create(NV_OF_API_VERSION,&s.api);})==NV_OF_SUCCESS;
    }
    if(!apiReady || !s.api.nvCreateOpticalFlowD3D11 || !s.api.nvOFInit || !s.api.nvOFGetCaps ||
        !s.api.nvOFExecute || !s.api.nvOFRegisterResourceD3D11 || !s.api.nvOFUnregisterResourceD3D11 ||
        !s.api.nvOFDestroy || !s.api.nvOFGetSurfaceFormatCountD3D11 || !s.api.nvOFGetSurfaceFormatD3D11) return false;
    if(Call([&]{return s.api.nvCreateOpticalFlowD3D11(device,s.context.Get(),&s.session);})!=NV_OF_SUCCESS || !s.session) return false;
    if(!s.Formats(NV_OF_BUFFER_USAGE_INPUT,s.inputFormat)) {
        s.inputFormat=DXGI_FORMAT_R8G8B8A8_UNORM;
        if(!s.Formats(NV_OF_BUFFER_USAGE_INPUT,s.inputFormat)) return false;
    }
    if(!s.Formats(NV_OF_BUFFER_USAGE_OUTPUT,DXGI_FORMAT_R16G16_SINT)) return false;
    uint32_t count=0;
    if(Call([&]{return s.api.nvOFGetCaps(s.session,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,nullptr,&count);})!=NV_OF_SUCCESS || !count || count>64) return false;
    std::vector<uint32_t> grids(count);
    if(Call([&]{return s.api.nvOFGetCaps(s.session,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,grids.data(),&count);})!=NV_OF_SUCCESS || count>grids.size()) return false;
    s.grid=s.quality>=4?2u:4u;
    if(std::find(grids.begin(),grids.begin()+count,s.grid)==grids.begin()+count) {
        if(std::find(grids.begin(),grids.begin()+count,4u)==grids.begin()+count) return false;
        s.quality=s.quality==5?3u:2u;s.grid=4;
        s.log("NVOF: requested 2x2 grid unavailable; using the reported 4x4 quality profile");
    }
    NV_OF_INIT_PARAMS init{};init.width=s.width;init.height=s.height;
    init.outGridSize=static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(s.grid);
    init.mode=NV_OF_MODE_OPTICALFLOW;
    init.perfLevel=s.quality==1?NV_OF_PERF_LEVEL_FAST:(s.quality==3 || s.quality==5)?NV_OF_PERF_LEVEL_SLOW:NV_OF_PERF_LEVEL_MEDIUM;
    init.predDirection=NV_OF_PRED_DIRECTION_FORWARD;init.inputBufferFormat=NV_OF_BUFFER_FORMAT_ABGR8;
    if(Call([&]{return s.api.nvOFInit(s.session,&init);})!=NV_OF_SUCCESS) return false;
    for(unsigned i=0;i<2;++i) {
        auto td=TextureDesc(s.width,s.height,s.inputFormat,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE);
        if(FAILED(device->CreateTexture2D(&td,nullptr,&s.input[i])) ||
           FAILED(device->CreateRenderTargetView(s.input[i].Get(),nullptr,&s.targets[i])) ||
           Call([&]{return s.api.nvOFRegisterResourceD3D11(s.session,s.input[i].Get(),&s.registered[i]);})!=NV_OF_SUCCESS || !s.registered[i]) return false;
    }
    auto td=TextureDesc((s.width+s.grid-1)/s.grid,(s.height+s.grid-1)/s.grid,DXGI_FORMAT_R16G16_SINT,D3D11_BIND_SHADER_RESOURCE);
    if(FAILED(device->CreateTexture2D(&td,nullptr,&s.coarse)) || FAILED(device->CreateShaderResourceView(s.coarse.Get(),nullptr,&s.coarseView)) ||
       Call([&]{return s.api.nvOFRegisterResourceD3D11(s.session,s.coarse.Get(),&s.registered[2]);})!=NV_OF_SUCCESS || !s.registered[2]) return false;
    td=TextureDesc(s.width,s.height,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
    td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if(FAILED(device->CreateTexture2D(&td,nullptr,&s.motion)) || FAILED(device->CreateUnorderedAccessView(s.motion.Get(),nullptr,&s.motionView)) || !Shader(device,dense,s.densify)) return false;
    Ptr<ID3DBlob> vs,ps;
    if(!Compile("vs","vs_5_0",vs) || !Compile("ps","ps_5_0",ps) ||
       FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&s.vertex)) ||
       FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&s.pixel))) return false;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    const unsigned dimensions[]={s.width,s.height,s.grid,0};D3D11_SUBRESOURCE_DATA data{};data.pSysMem=dimensions;
    if(FAILED(device->CreateBuffer(&bd,&data,&s.params))) return false;
    const unsigned encoding[]={desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?1u:0u,0,0,0};data.pSysMem=encoding;
    if(FAILED(device->CreateBuffer(&bd,&data,&s.format))) return false;
    char message[192];std::snprintf(message,sizeof message,"NVOF initialized on private LS-adapter context: quality=%u grid=%ux%u %ux%u; current-to-previous pixels; no engine depth",s.quality,s.grid,s.grid,s.width,s.height);s.log(message);
    return true;
}
bool OpticalFlow::Process(ID3D11Texture2D* input,bool reset,bool& realMotion) {
    realMotion=false;auto& s=*s_;if(reset) {s.history=false;s.temporalReset=true;}
    const unsigned current=s.history?1-s.previous:0;
    Ptr<ID3D11ShaderResourceView> view;
    if(FAILED(s.device->CreateShaderResourceView(input,nullptr,&view))) return false;
    ID3D11ShaderResourceView* srv=view.Get();ID3D11RenderTargetView* rtv=s.targets[current].Get();ID3D11Buffer* cb=s.format.Get();
    s.context->OMSetRenderTargets(1,&rtv,nullptr);s.context->PSSetShaderResources(0,1,&srv);s.context->PSSetConstantBuffers(0,1,&cb);
    s.context->VSSetShader(s.vertex.Get(),nullptr,0);s.context->PSSetShader(s.pixel.Get(),nullptr,0);
    s.context->RSSetState(nullptr);s.context->OMSetBlendState(nullptr,nullptr,0xffffffff);s.context->OMSetDepthStencilState(nullptr,0);
    s.context->IASetInputLayout(nullptr);s.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D11_VIEWPORT viewport{0,0,float(s.width),float(s.height),0,1};s.context->RSSetViewports(1,&viewport);s.context->Draw(3,0);
    srv=nullptr;s.context->PSSetShaderResources(0,1,&srv);s.context->OMSetRenderTargets(0,nullptr,nullptr);
    if(!s.history) {
        const float zeros[4]{};s.context->ClearUnorderedAccessViewFloat(s.motionView.Get(),zeros);
    } else {
        NV_OF_EXECUTE_INPUT_PARAMS in{};in.inputFrame=s.registered[current];in.referenceFrame=s.registered[s.previous];
        in.disableTemporalHints=s.temporalReset?NV_OF_TRUE:NV_OF_FALSE;
        NV_OF_EXECUTE_OUTPUT_PARAMS out{};out.outputBuffer=s.registered[2];
        if(Call([&]{return s.api.nvOFExecute(s.session,&in,&out);})!=NV_OF_SUCCESS) {s.history=false;s.temporalReset=true;return false;}
        s.temporalReset=false;
        srv=s.coarseView.Get();ID3D11UnorderedAccessView* uav=s.motionView.Get();cb=s.params.Get();
        s.context->CSSetShaderResources(0,1,&srv);s.context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        s.context->CSSetConstantBuffers(0,1,&cb);s.context->CSSetShader(s.densify.Get(),nullptr,0);
        s.context->Dispatch((s.width+7)/8,(s.height+7)/8,1);Unbind(s.context.Get());realMotion=true;
    }
    s.previous=current;s.history=true;return true;
}
ID3D11Texture2D* OpticalFlow::Motion() const {return s_?s_->motion.Get():nullptr;}
unsigned OpticalFlow::Quality() const {return s_?s_->quality:0;}
}
