// SPDX-License-Identifier: MIT
#pragma once
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
namespace fg {
template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
inline bool Shader(ID3D11Device* device,const char* source,Ptr<ID3D11ComputeShader>& shader) {
    Ptr<ID3DBlob> code,error;
    if(FAILED(D3DCompile(source,std::strlen(source),"LS_DLSSFG",nullptr,nullptr,"main","cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error))) return false;
    return SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
}
inline void Unbind(ID3D11DeviceContext* context) {
    ID3D11ShaderResourceView* srvs[4]{};ID3D11UnorderedAccessView* uavs[2]{};
    context->CSSetShaderResources(0,4,srvs);context->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
    context->CSSetShader(nullptr,nullptr,0);
}
inline D3D11_TEXTURE2D_DESC TextureDesc(unsigned width,unsigned height,DXGI_FORMAT format,unsigned bind) {
    D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.Format=format;
    d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind;return d;
}
}
