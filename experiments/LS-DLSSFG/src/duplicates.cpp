// SPDX-License-Identifier: MIT
#include "duplicates.h"
namespace fg {
namespace {
constexpr char compare[]=R"(
Texture2D<float4> current : register(t0);
Texture2D<float4> previous : register(t1);
RWBuffer<uint> changed : register(u0);
groupshared uint groupChanged;
[numthreads(16,16,1)]
void main(uint3 pixel:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    if(lane==0) groupChanged=0;
    GroupMemoryBarrierWithGroupSync();
    uint w,h;current.GetDimensions(w,h);
    if(pixel.x<w && pixel.y<h) {
        if(any(asuint(current.Load(int3(pixel.xy,0)).rgb)!=asuint(previous.Load(int3(pixel.xy,0)).rgb)))
            InterlockedOr(groupChanged,1);
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane==0 && groupChanged) InterlockedOr(changed[0],1);
})";
}
bool DuplicateFilter::Init(ID3D11Device* d,const D3D11_TEXTURE2D_DESC& desc) {
    device_=d;width_=desc.Width;height_=desc.Height;valid_=false;
    auto td=TextureDesc(width_,height_,desc.Format,D3D11_BIND_SHADER_RESOURCE);
    if(FAILED(d->CreateTexture2D(&td,nullptr,&previous_)) ||
       FAILED(d->CreateShaderResourceView(previous_.Get(),nullptr,&previousView_)) || !Shader(d,compare,shader_)) return false;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=4;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    if(FAILED(d->CreateBuffer(&bd,nullptr,&result_))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R32_UINT;
    ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=1;
    if(FAILED(d->CreateUnorderedAccessView(result_.Get(),&ud,&resultView_))) return false;
    bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;bd.BindFlags=0;
    D3D11_QUERY_DESC qd{};qd.Query=D3D11_QUERY_EVENT;
    return SUCCEEDED(d->CreateBuffer(&bd,nullptr,&readback_)) && SUCCEEDED(d->CreateQuery(&qd,&completion_));
}
DuplicateResult DuplicateFilter::Check(ID3D11DeviceContext* context,ID3D11Texture2D* input) {
    if(!valid_) {context->CopyResource(previous_.Get(),input);valid_=true;return DuplicateResult::NewFrame;}
    Ptr<ID3D11ShaderResourceView> currentView;
    if(FAILED(device_->CreateShaderResourceView(input,nullptr,&currentView))) return DuplicateResult::Failed;
    const UINT zero[4]{};context->ClearUnorderedAccessViewUint(resultView_.Get(),zero);
    ID3D11ShaderResourceView* views[]={currentView.Get(),previousView_.Get()};
    ID3D11UnorderedAccessView* uav=resultView_.Get();
    context->CSSetShaderResources(0,2,views);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    context->CSSetShader(shader_.Get(),nullptr,0);context->Dispatch((width_+15)/16,(height_+15)/16,1);
    Unbind(context);context->CopyResource(readback_.Get(),result_.Get());context->End(completion_.Get());context->Flush();
    const ULONGLONG deadline=GetTickCount64()+500;
    HRESULT hr=S_FALSE;
    while((hr=context->GetData(completion_.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH))==S_FALSE && GetTickCount64()<deadline) SwitchToThread();
    if(hr!=S_OK) return DuplicateResult::Failed;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(context->Map(readback_.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped))) return DuplicateResult::Failed;
    const bool same=*static_cast<const unsigned*>(mapped.pData)==0;context->Unmap(readback_.Get(),0);
    if(!same) context->CopyResource(previous_.Get(),input);
    return same?DuplicateResult::Duplicate:DuplicateResult::NewFrame;
}
}
