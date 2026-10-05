#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include "CompositionSwapChainAttachment.h"
#include "NvidiaOpticalFlowShaders.h"
#include "OpticalFlowResolution.h"
#include <array>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <cstring>
using Microsoft::WRL::ComPtr;
using namespace Magpie;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void HR(HRESULT value, const char* message) {
    if (FAILED(value)) { std::cerr << message << " HRESULT=" << std::hex << value << std::dec << '\n'; throw std::runtime_error(message); }
}
struct GPU {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    GPU() { HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Create WARP"); }
    ComPtr<ID3DBlob> Compile(const char* source,const char* entry,const char* target) {
        ComPtr<ID3DBlob> blob,error;
        auto hr=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,entry,target,D3DCOMPILE_ENABLE_STRICTNESS,0,&blob,&error);
        if (FAILED(hr) && error) std::cerr<<static_cast<const char*>(error->GetBufferPointer());
        HR(hr,"Compile production shader");return blob;
    }
    ComPtr<ID3D11Texture2D> Texture(UINT width,UINT height,DXGI_FORMAT format,UINT bind,const void* data=nullptr,UINT pitch=0) {
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=width;desc.Height=height;desc.MipLevels=1;desc.ArraySize=1;desc.Format=format;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA init{data,pitch,0}; ComPtr<ID3D11Texture2D> texture;
        HR(device->CreateTexture2D(&desc,data?&init:nullptr,&texture),"Create texture");return texture;
    }
    std::vector<unsigned char> Read(ID3D11Texture2D* texture,UINT pixelBytes) {
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;HR(device->CreateTexture2D(&desc,nullptr,&staging),"Create staging");context->CopyResource(staging.Get(),texture);
        D3D11_MAPPED_SUBRESOURCE map{}; HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"Read staging");
        std::vector<unsigned char> bytes(size_t(desc.Width)*desc.Height*pixelBytes);
        for(UINT y=0;y<desc.Height;++y) memcpy(bytes.data()+size_t(y)*desc.Width*pixelBytes,static_cast<unsigned char*>(map.pData)+size_t(y)*map.RowPitch,size_t(desc.Width)*pixelBytes);
        context->Unmap(staging.Get(),0);return bytes;
    }
    ComPtr<ID3D11Buffer> Buffer(const void* data,UINT bytes) {
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{data,0,0};ComPtr<ID3D11Buffer> buffer;HR(device->CreateBuffer(&desc,&init,&buffer),"Create CB");return buffer;
    }
};
static void DenseTest(GPU& gpu,UINT width,UINT height,UINT percent,bool consistent) {
    const UINT aw=OpticalFlowAnalysisDimension(width,percent),ah=OpticalFlowAnalysisDimension(height,percent),gw=(aw+3)/4,gh=(ah+3)/4;
    std::vector<std::array<short,2>> forward(size_t(gw)*gh,{64,-32}),backward(size_t(gw)*gh,{short(consistent?-64:64),32});
    std::vector<unsigned char> costs(size_t(gw)*gh,0);
    auto f=gpu.Texture(gw,gh,DXGI_FORMAT_R16G16_SINT,D3D11_BIND_SHADER_RESOURCE,forward.data(),gw*4);
    auto b=gpu.Texture(gw,gh,DXGI_FORMAT_R16G16_SINT,D3D11_BIND_SHADER_RESOURCE,backward.data(),gw*4);
    auto c=gpu.Texture(gw,gh,DXGI_FORMAT_R8_UINT,D3D11_BIND_SHADER_RESOURCE,costs.data(),gw);
    auto motion=gpu.Texture(width,height,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
    auto confidence=gpu.Texture(width,height,DXGI_FORMAT_R8_UNORM,D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11ShaderResourceView> fs,bs,cs; ComPtr<ID3D11UnorderedAccessView> mu,cu;
    HR(gpu.device->CreateShaderResourceView(f.Get(),nullptr,&fs),"Forward SRV");HR(gpu.device->CreateShaderResourceView(b.Get(),nullptr,&bs),"Backward SRV");HR(gpu.device->CreateShaderResourceView(c.Get(),nullptr,&cs),"Cost SRV");
    HR(gpu.device->CreateUnorderedAccessView(motion.Get(),nullptr,&mu),"Motion UAV");HR(gpu.device->CreateUnorderedAccessView(confidence.Get(),nullptr,&cu),"Confidence UAV");
    const UINT params[]{width,height,aw,ah,gw,gh,4,1,1,1,0,0};auto cb=gpu.Buffer(params,sizeof(params));
    auto code=gpu.Compile(DENSIFY_FLOW_HLSL,"Densify","cs_5_0");ComPtr<ID3D11ComputeShader> shader;HR(gpu.device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader),"Create densify CS");
    ID3D11ShaderResourceView* srvs[]{fs.Get(),bs.Get(),cs.Get(),cs.Get()};ID3D11UnorderedAccessView* uavs[]{mu.Get(),cu.Get()};ID3D11Buffer* buffers[]{cb.Get()};
    gpu.context->ClearState();gpu.context->CSSetShader(shader.Get(),nullptr,0);gpu.context->CSSetShaderResources(0,4,srvs);gpu.context->CSSetUnorderedAccessViews(0,2,uavs,nullptr);gpu.context->CSSetConstantBuffers(0,1,buffers);gpu.context->Dispatch((width+7)/8,(height+7)/8,1);gpu.context->ClearState();
    const auto vectors=gpu.Read(motion.Get(),4),quality=gpu.Read(confidence.Get(),1);
    for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) {
        const size_t pixel=size_t(y)*width+x;std::array<uint16_t,2> half{};memcpy(half.data(),vectors.data()+pixel*4,4);
        Check(std::abs(DirectX::PackedVector::XMConvertHalfToFloat(half[0])-2.0f*float(width)/float(aw))<0.01f,"Scaled X motion");
        Check(std::abs(DirectX::PackedVector::XMConvertHalfToFloat(half[1])+float(height)/float(ah))<0.01f,"Scaled Y motion/sign");
        if(x==width/2 && y==height/2) Check(consistent?quality[pixel]>250:quality[pixel]==0,"Forward/backward confidence");
    }
}
static void InputTest(GPU& gpu,UINT width,UINT height,UINT percent,bool hdr) {
    const UINT aw=OpticalFlowAnalysisDimension(width,percent),ah=OpticalFlowAnalysisDimension(height,percent);
    std::vector<std::array<float,4>> input(size_t(width)*height);
    for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) input[size_t(y)*width+x]={hdr?2.25f:float(x)/float(width-1),hdr?1.125f:float(y)/float(height-1),hdr?4.5f:0.25f,0};
    auto source=gpu.Texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,input.data(),width*16);
    auto target=gpu.Texture(aw,ah,DXGI_FORMAT_B8G8R8A8_UNORM,D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11RenderTargetView> rtv;
    HR(gpu.device->CreateShaderResourceView(source.Get(),nullptr,&srv),"Input SRV");HR(gpu.device->CreateRenderTargetView(target.Get(),nullptr,&rtv),"BGRA input RTV");
    auto vsCode=gpu.Compile(NVOF_INPUT_HLSL,"InputVS","vs_5_0"),psCode=gpu.Compile(NVOF_INPUT_HLSL,"InputPS","ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    HR(gpu.device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs),"Input VS");HR(gpu.device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps),"Input PS");
    const UINT params[]{aw,ah,hdr?1u:0u,0};auto cb=gpu.Buffer(params,sizeof(params));D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;HR(gpu.device->CreateSamplerState(&sd,&sampler),"Linear sampler");
    ID3D11RenderTargetView* targets[]{rtv.Get()};ID3D11ShaderResourceView* srvs[]{srv.Get()};ID3D11SamplerState* samplers[]{sampler.Get()};ID3D11Buffer* buffers[]{cb.Get()};
    D3D11_VIEWPORT viewport{0,0,float(aw),float(ah),0,1};gpu.context->ClearState();gpu.context->OMSetRenderTargets(1,targets,nullptr);gpu.context->RSSetViewports(1,&viewport);gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);gpu.context->VSSetShader(vs.Get(),nullptr,0);gpu.context->PSSetShader(ps.Get(),nullptr,0);gpu.context->PSSetShaderResources(0,1,srvs);gpu.context->PSSetSamplers(0,1,samplers);gpu.context->PSSetConstantBuffers(0,1,buffers);gpu.context->Draw(3,0);gpu.context->ClearState();
    const auto result=gpu.Read(target.Get(),4);
    for(UINT y=0;y<ah;++y) for(UINT x=0;x<aw;++x) {
        const size_t p=(size_t(y)*aw+x)*4;
        const auto red=hdr?0.5f:std::clamp(((float(x)+0.5f)*float(width)/float(aw)-0.5f)/float(width-1),0.0f,1.0f);
        const auto green=hdr?0.25f:std::clamp(((float(y)+0.5f)*float(height)/float(ah)-0.5f)/float(height-1),0.0f,1.0f);
        Check(std::abs(float(result[p+2])/255-red)<0.006f && std::abs(float(result[p+1])/255-green)<0.006f,"Linear downsample/HDR color");
        Check(result[p+3]==255,"Opaque NVOF input");
    }
}
static void CompositionTest(GPU& gpu) {
    WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MagpieVrrTestWindow";
    Check(RegisterClassW(&cls)!=0,"Register actual test window class");
    const DWORD hostEx=WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP;
    HWND source=CreateWindowExW(WS_EX_TOPMOST,cls.lpszClassName,L"VRR source",WS_POPUP,20,20,240,180,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HWND host=CreateWindowExW(hostEx,cls.lpszClassName,L"VRR host",WS_POPUP,20,20,240,180,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(source && host,"Create native windows");
    SetLayeredWindowAttributes(host,0,255,LWA_ALPHA);ShowWindow(source,SW_SHOWNOACTIVATE);ShowWindow(host,SW_SHOWNOACTIVATE);
    SetWindowPos(host,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    const auto foreground=GetForegroundWindow();
    ComPtr<IDXGIFactory5> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create factory");BOOL tearing=FALSE;
    HR(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing)),"Tearing capability");
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=240;desc.Height=180;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=3;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.Scaling=DXGI_SCALING_STRETCH;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;desc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT|(tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0);
    ComPtr<IDXGISwapChain1> chain;HR(factory->CreateSwapChainForComposition(gpu.device.Get(),&desc,nullptr,&chain),"Composition flip/waitable chain");
    auto chain2=ComPtr<IDXGISwapChain2>{};HR(chain.As(&chain2),"Chain2");HR(chain2->SetMaximumFrameLatency(1),"Latency1");
    HANDLE capacity=chain2->GetFrameLatencyWaitableObject();Check(capacity!=nullptr,"Waitable handle");
    {
        CompositionSwapChainAttachment attachment;HR(attachment.Initialize(host,chain.Get()),"Production DComp attachment");
        for(int pass=0;pass<3;++pass) {
            ComPtr<ID3D11Texture2D> buffer;HR(chain->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Backbuffer");ComPtr<ID3D11RenderTargetView> rtv;HR(gpu.device->CreateRenderTargetView(buffer.Get(),nullptr,&rtv),"Backbuffer RTV");const float color[]{0.2f,0.3f,0.4f,1};gpu.context->ClearRenderTargetView(rtv.Get(),color);gpu.context->Flush();
            HR(chain->Present(0,tearing?DXGI_PRESENT_ALLOW_TEARING:0),"Present0");
            buffer.Reset();rtv.Reset();gpu.context->ClearState();gpu.context->Flush();
            HR(chain->ResizeBuffers(0,240+UINT(pass)*8,180+UINT(pass)*8,DXGI_FORMAT_UNKNOWN,desc.Flags),"Resize preserves tearing/waitable flags");
            DXGI_SWAP_CHAIN_DESC1 actual{};HR(chain->GetDesc1(&actual),"Description");Check(actual.Flags==desc.Flags && actual.BufferCount==3,"Immutable flags and buffer count");
        }
        Check(GetForegroundWindow()==foreground,"No focus stealing");
        const auto hit=WindowFromPoint({50,50});
        std::cout<<"Input check: hit="<<hit<<" source="<<source<<" host="<<host<<" visible="<<IsWindowVisible(source)<<"/"<<IsWindowVisible(host)<<" foreground="<<foreground<<"\n";
        Check(hit==source,"Native mouse transparency");
    }
    CloseHandle(capacity);chain2.Reset();chain.Reset();DestroyWindow(host);DestroyWindow(source);UnregisterClassW(cls.lpszClassName,cls.hInstance);
    std::cout<<"PASS: actual composition flip attachment, Present0, latency1, three resize cycles, native mouse transparency and unchanged focus; tearingSupported="<<tearing<<" (WARP does not verify G-SYNC)\n";
}
int main() {
    try {
        GPU gpu;
        for(auto size : {std::array<UINT,2>{128,64},std::array<UINT,2>{127,63}}) for(UINT scale : {25u,50u,75u,100u}) {
            DenseTest(gpu,size[0],size[1],scale,true);DenseTest(gpu,size[0],size[1],scale,false);
            InputTest(gpu,size[0],size[1],scale,false);InputTest(gpu,size[0],size[1],scale,true);
        }
        std::cout<<"PASS: production NVOF shaders on WARP, 32 translation/confidence/color/HDR cases, odd dimensions, 25/50/75/100%\n";
        CompositionTest(gpu);
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
