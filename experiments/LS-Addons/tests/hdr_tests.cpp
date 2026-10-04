#include "backend.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
using Microsoft::WRL::ComPtr;
namespace {
void Check(bool b,const char* why){if(!b){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
struct Fake {ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11ComputeShader> shader;};
unsigned draws=0;MagpieRtxHdrSettings recorded{};
HRESULT WINAPI Create(ID3D11Device* d,const wchar_t*,void** out,uint32_t* status){
    auto* s=new Fake;s->device=d;d->GetImmediateContext(&s->context);
    constexpr char shader[]=R"(Texture2D<float4> input:register(t0);RWTexture2D<float4> output:register(u0);[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h;output.GetDimensions(w,h);if(p.x<w && p.y<h)output[p.xy]=float4(input.Load(int3(p.xy,0)).rgb*2,1);})";
    ComPtr<ID3DBlob> code,error;*status=0;
    Check(SUCCEEDED(D3DCompile(shader,sizeof shader-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,&error)),"test provider shader");
    Check(SUCCEEDED(d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&s->shader)),"test provider compute");*out=s;return S_OK;
}
HRESULT WINAPI Draw(void* instance,ID3D11Texture2D* input,ID3D11Texture2D* output,const MagpieRtxHdrSettings* options,uint32_t* status){
    auto& s=*static_cast<Fake*>(instance);D3D11_TEXTURE2D_DESC a{},b{};input->GetDesc(&a);output->GetDesc(&b);Check(MagpieRtxHdrEndpointsSupported(a,b),"actual upstream endpoint contract");
    recorded=*options;++draws;*status=0;
    ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;
    Check(SUCCEEDED(s.device->CreateShaderResourceView(input,nullptr,&srv)) && SUCCEEDED(s.device->CreateUnorderedAccessView(output,nullptr,&uav)),"provider views");
    ID3D11ShaderResourceView* v=srv.Get();ID3D11UnorderedAccessView* u=uav.Get();s.context->CSSetShader(s.shader.Get(),nullptr,0);s.context->CSSetShaderResources(0,1,&v);s.context->CSSetUnorderedAccessViews(0,1,&u,nullptr);s.context->Dispatch((a.Width+7)/8,(a.Height+7)/8,1);return S_OK;
}
HRESULT WINAPI Destroy(void* instance){delete static_cast<Fake*>(instance);return S_OK;}
float Tone(float v){float l=2*(0.2126f*0.1f+0.7152f*0.3f+0.0722f*0.6f);v=2*v/(1+l);return v<=.0031308f?v*12.92f:1.055f*std::pow(v,1.f/2.4f)-.055f;}
}
int main(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"WARP");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> sentinel;
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&sentinel)),"sentinel");ID3D11Buffer* bound=sentinel.Get();context->CSSetConstantBuffers(0,1,&bound);
    const hdr::Provider provider{Create,Draw,Destroy};
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}){
        D3D11_TEXTURE2D_DESC d{};d.Width=7;d.Height=5;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::vector<unsigned char> pixels(d.Width*d.Height*4);for(unsigned i=0;i<pixels.size();i+=4){pixels[i]=format==DXGI_FORMAT_R8G8B8A8_UNORM?26:153;pixels[i+1]=77;pixels[i+2]=format==DXGI_FORMAT_R8G8B8A8_UNORM?153:26;pixels[i+3]=255;}
        D3D11_SUBRESOURCE_DATA data{pixels.data(),d.Width*4,0};ComPtr<ID3D11Texture2D> input;Check(SUCCEEDED(device->CreateTexture2D(&d,&data,&input)),"input");
        hdr::Settings settings;settings.enabled=settings.nativeHdrDisabled=true;
        hdr::Backend backend;Check(backend.Init(device.Get(),d,L".",settings,[](const char* s){std::puts(s);},&provider),"backend initialization");
        unsigned before=draws;Check(backend.Process(input.Get(),settings,false) && draws==before+1 && !backend.Duplicate(),"TrueHDR provider receives SDR");
        auto read=d;read.Usage=D3D11_USAGE_STAGING;read.BindFlags=0;read.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;Check(SUCCEEDED(device->CreateTexture2D(&read,nullptr,&staging)),"readback");
        context->CopyResource(staging.Get(),backend.Output());D3D11_MAPPED_SUBRESOURCE mapped{};Check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"read tone map");
        const auto* p=static_cast<const unsigned char*>(mapped.pData);const unsigned r=format==DXGI_FORMAT_R8G8B8A8_UNORM?0:2,b=format==DXGI_FORMAT_R8G8B8A8_UNORM?2:0;
        Check(std::abs(int(p[r])-int(Tone(26.f/255)*255))<=3 && std::abs(int(p[1])-int(Tone(77.f/255)*255))<=3 && std::abs(int(p[b])-int(Tone(153.f/255)*255))<=3,"SDR tone map, BGRA swizzle and full triangle coverage");context->Unmap(staging.Get(),0);
        for(unsigned i=3;i<pixels.size();i+=4)pixels[i]=17;context->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),d.Width*4,0);
        Check(backend.Process(input.Get(),settings,false) && backend.Duplicate() && draws==before+1,"alpha-only changes reuse RGB result");
        ++settings.contrast;Check(backend.Process(input.Get(),settings,false) && draws==before+2 && recorded.contrast==101,"live TrueHDR parameter invalidates duplicate cache");
        Check(backend.Process(input.Get(),settings,true) && draws==before+3,"HDR mode preserves floating output");D3D11_TEXTURE2D_DESC out{};backend.Output()->GetDesc(&out);Check(out.Format==DXGI_FORMAT_R16G16B16A16_FLOAT,"scRGB FP16 endpoint");
        ComPtr<ID3D11Buffer> actual;context->CSGetConstantBuffers(0,1,&actual);Check(actual.Get()==sentinel.Get(),"LS compute state unchanged by provider, tone map and duplicate check");
    }
    std::puts("Real WARP/shared fences, RGBA/BGRA SDR mapping, scRGB FP16, exact cache invalidation and preserved LS state passed (TrueHDR provider simulated)");
}
