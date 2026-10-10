#include "../../backend/native_shaders.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
void Check(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
void Hr(HRESULT hr, const char* message) { Check(SUCCEEDED(hr), message); }
ComPtr<ID3DBlob> Compile(const char* shader, const char* entry, const char* target) {
    ComPtr<ID3DBlob> result, error;
    const HRESULT hr=D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &result, &error);
    if (FAILED(hr) && error) std::cerr.write(static_cast<const char*>(error->GetBufferPointer()), static_cast<std::streamsize>(error->GetBufferSize()));
    Hr(hr, "flow shader compile"); return result;
}
float Half(uint16_t h) {
    const float sign = h & 0x8000 ? -1.0f : 1.0f;
    const int exponent = (h >> 10) & 31;
    return sign * (exponent ? std::ldexp(1.0f + float(h & 1023) / 1024.0f, exponent - 15) : std::ldexp(float(h & 1023), -24));
}
}
int main() { try {
    std::cout << "Compile GPU decision flag\n"; Compile(ls_native::shaders::flag,"main","cs_5_0");
    std::cout << "Compile full-image comparison\n"; Compile(ls_native::shaders::compare,"main","cs_5_0");
    std::cout << "Compile economy/HUD compositor\n"; Compile(ls_native::shaders::composite,"main","cs_5_0");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &ctx), "flow WARP");
    auto texture = [&](UINT w, UINT h, DXGI_FORMAT format, UINT bind, const void* pixels, UINT pitch) {
        D3D11_TEXTURE2D_DESC d{}; d.Width=w; d.Height=h; d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=format; d.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem=pixels; initial.SysMemPitch=pitch;
        ComPtr<ID3D11Texture2D> result; Hr(device->CreateTexture2D(&d, pixels ? &initial : nullptr, &result), "flow image"); return result;
    };
    auto read = [&](ID3D11Texture2D* image) {
        D3D11_TEXTURE2D_DESC d{}; image->GetDesc(&d); d.Usage=D3D11_USAGE_STAGING; d.BindFlags=0; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging; Hr(device->CreateTexture2D(&d,nullptr,&staging), "flow staging");
        ctx->CopyResource(staging.Get(),image); D3D11_MAPPED_SUBRESOURCE m{};
        Hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m), "flow test readback"); // TEST ONLY
        std::vector<uint8_t> result(size_t(d.Width)*d.Height*4);
        for (UINT y=0;y!=d.Height;++y) std::memcpy(result.data()+size_t(y)*d.Width*4,
            static_cast<const uint8_t*>(m.pData)+size_t(y)*m.RowPitch,size_t(d.Width)*4);
        ctx->Unmap(staging.Get(),0); return result;
    };
    // FP16 source is untouched; only the analysis image clamps to SDR/opaque.
    std::vector<std::array<uint16_t,4>> source(19*7, {0xb800,0x3800,0x4000,0}); // -.5, .5, 2, 0
    auto input=texture(19,7,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE,source.data(),19*8);
    auto analysis=texture(10,4,DXGI_FORMAT_B8G8R8A8_UNORM,D3D11_BIND_RENDER_TARGET,nullptr,0);
    ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11RenderTargetView> rtv;
    Hr(device->CreateShaderResourceView(input.Get(),nullptr,&srv), "flow source SRV");
    Hr(device->CreateRenderTargetView(analysis.Get(),nullptr,&rtv), "flow analysis RTV");
    auto vs=Compile(ls_native::shaders::input,"vs","vs_5_0"), ps=Compile(ls_native::shaders::input,"ps","ps_5_0");
    ComPtr<ID3D11VertexShader> vertex; ComPtr<ID3D11PixelShader> pixel;
    Hr(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex), "flow VS");
    Hr(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel), "flow PS");
    D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler; Hr(device->CreateSamplerState(&sd,&sampler), "flow sampler");
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster; Hr(device->CreateRasterizerState(&rd,&raster), "flow raster");
    std::array<uint32_t,8> constants{10,4,0,0,0,0,0,0};
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=32; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem=constants.data();
    ComPtr<ID3D11Buffer> cb; Hr(device->CreateBuffer(&bd,&initial,&cb), "analysis constants");
    D3D11_VIEWPORT viewport{0,0,10,4,0,1}; ctx->RSSetViewports(1,&viewport); ctx->RSSetState(raster.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vertex.Get(),nullptr,0); ctx->PSSetShader(pixel.Get(),nullptr,0);
    auto* s=srv.Get(); auto* sam=sampler.Get(); auto* b=cb.Get(); auto* target=rtv.Get();
    ctx->PSSetShaderResources(0,1,&s); ctx->PSSetSamplers(0,1,&sam); ctx->PSSetConstantBuffers(0,1,&b);
    ctx->OMSetRenderTargets(1,&target,nullptr); ctx->Draw(3,0); ctx->OMSetRenderTargets(0,nullptr,nullptr);
    auto bytes=read(analysis.Get());
    for(size_t i=0;i!=bytes.size();i+=4) Check(bytes[i]==255&&bytes[i+1]==128&&bytes[i+2]==0&&bytes[i+3]==255,"analysis clamp/channel layout/alpha");
    // Known signed S10.5 displacement, including negative direction and borders.
    std::array<int16_t,6> vectors{-64,32,-64,32,-64,32};
    auto coarse=texture(3,1,DXGI_FORMAT_R16G16_SINT,D3D11_BIND_SHADER_RESOURCE,vectors.data(),12);
    auto motion=texture(19,7,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_UNORDERED_ACCESS,nullptr,0);
    ComPtr<ID3D11ShaderResourceView> coarse_view; ComPtr<ID3D11UnorderedAccessView> motion_view;
    Hr(device->CreateShaderResourceView(coarse.Get(),nullptr,&coarse_view), "coarse view");
    Hr(device->CreateUnorderedAccessView(motion.Get(),nullptr,&motion_view), "motion view");
    auto dense=Compile(ls_native::shaders::densify,"main","cs_5_0"); ComPtr<ID3D11ComputeShader> compute;
    Hr(device->CreateComputeShader(dense->GetBufferPointer(),dense->GetBufferSize(),nullptr,&compute), "dense CS");
    constants={19,7,4,0,0,0,0,0}; const float scales[2]{1.9f,1.75f}; std::memcpy(constants.data()+4,scales,sizeof(scales));
    ctx->UpdateSubresource(cb.Get(),0,nullptr,constants.data(),0,0);
    auto* c=coarse_view.Get(); auto* u=motion_view.Get();
    ctx->CSSetShader(compute.Get(),nullptr,0); ctx->CSSetShaderResources(0,1,&c); ctx->CSSetUnorderedAccessViews(0,1,&u,nullptr);
    ctx->CSSetConstantBuffers(0,1,&b); ctx->Dispatch(3,1,1); u=nullptr; ctx->CSSetUnorderedAccessViews(0,1,&u,nullptr);
    bytes=read(motion.Get());
    for(size_t i=0;i!=bytes.size();i+=4) {
        uint16_t x=0,y=0; std::memcpy(&x,bytes.data()+i,2); std::memcpy(&y,bytes.data()+i+2,2);
        Check(std::abs(Half(x)+3.8f)<0.003f&&std::abs(Half(y)-1.75f)<0.001f,"S10.5 motion scaling/direction/border");
    }
    std::cout<<"Optical Flow analysis conversion and signed full-resolution vector densification passed on WARP\n"; return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
