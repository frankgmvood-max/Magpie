// SPDX-License-Identifier: MIT
// Real cross-device GPU execution: no NGX or simulated fence operations.
#include "gpu_helpers.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <utility>
using namespace fg;
void Check(bool ok,const char* why) {if(!ok){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
int main() {
    Ptr<ID3D11Device> host,worker;Ptr<ID3D11DeviceContext> hostBase,workerBase;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&host,nullptr,&hostBase)),"host WARP device");
    Ptr<IDXGIDevice> dx;Ptr<IDXGIAdapter> adapter;
    Check(SUCCEEDED(host.As(&dx)) && SUCCEEDED(dx->GetAdapter(&adapter)),"host adapter");
    Check(SUCCEEDED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&worker,nullptr,&workerBase)),"private device on same adapter");
    Ptr<ID3D11Device5> host5,worker5;Ptr<ID3D11DeviceContext4> host4,worker4;
    Check(SUCCEEDED(host.As(&host5)) && SUCCEEDED(worker.As(&worker5)) && SUCCEEDED(hostBase.As(&host4)) && SUCCEEDED(workerBase.As(&worker4)),"D3D11 shared fence interfaces");
    Ptr<ID3D12Device> d12;Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12))),"D3D12 on same adapter");
    Ptr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    Check(SUCCEEDED(d12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"D3D12 queue");
    Ptr<ID3D12CommandAllocator> allocator;Ptr<ID3D12GraphicsCommandList> command;
    Check(SUCCEEDED(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(d12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command))) && SUCCEEDED(command->Close()),"D3D12 commands");
    Ptr<ID3D11Fence> inputFence,workerInput,workerReady,hostCompletion;
    Ptr<ID3D12Fence> input12,ready12,completion;
    HANDLE handle=nullptr;
    Check(SUCCEEDED(host5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&inputFence))) &&
        SUCCEEDED(inputFence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle)),"input fence export");
    Check(SUCCEEDED(worker5->OpenSharedFence(handle,IID_PPV_ARGS(&workerInput))) && SUCCEEDED(d12->OpenSharedHandle(handle,IID_PPV_ARGS(&input12))),"input fence imports");CloseHandle(handle);
    Check(SUCCEEDED(worker5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&workerReady))) &&
        SUCCEEDED(workerReady->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle)),"private fence export");
    Check(SUCCEEDED(d12->OpenSharedHandle(handle,IID_PPV_ARGS(&ready12))),"private-to-D3D12 fence");CloseHandle(handle);
    Check(SUCCEEDED(d12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&completion))) &&
        SUCCEEDED(d12->CreateSharedHandle(completion.Get(),nullptr,GENERIC_ALL,nullptr,&handle)),"D3D12 completion export");
    Check(SUCCEEDED(host5->OpenSharedFence(handle,IID_PPV_ARGS(&hostCompletion))),"D3D12-to-host completion fence");CloseHandle(handle);
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"completion event");
    constexpr char transform[]=R"(
Texture2D<float4> image : register(t0);
RWTexture2D<float2> motion : register(u0);
[numthreads(8,8,1)] void main(uint3 pixel:SV_DispatchThreadID) {
    uint w,h;image.GetDimensions(w,h);if(pixel.x>=w || pixel.y>=h) return;
    motion[pixel.xy]=image.Load(int3(pixel.xy,0)).rg*float2(2,-4);
})";
    Ptr<ID3D11ComputeShader> shader,sentinel;
    Check(Shader(worker.Get(),transform,shader) && Shader(host.Get(),"[numthreads(1,1,1)] void main() {}",sentinel),"private shader and host state sentinel");
    host4->CSSetShader(sentinel.Get(),nullptr,0);
    unsigned long long value=0;
    for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        auto desc=TextureDesc(33,19,format,D3D11_BIND_SHADER_RESOURCE);
        desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        Ptr<ID3D11Texture2D> colour,privateColour;Ptr<ID3D12Resource> colour12;
        Check(SUCCEEDED(host->CreateTexture2D(&desc,nullptr,&colour)),"host shared colour");
        Ptr<IDXGIResource1> dxColour;Check(SUCCEEDED(colour.As(&dxColour)) && SUCCEEDED(dxColour->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle)),"colour NT handle");
        Check(SUCCEEDED(worker5->OpenSharedResource1(handle,IID_PPV_ARGS(&privateColour))) && SUCCEEDED(d12->OpenSharedHandle(handle,IID_PPV_ARGS(&colour12))),"colour opens on private D3D11 and D3D12");CloseHandle(handle);
        Ptr<ID3D11ShaderResourceView> source,hostSource;
        Check(SUCCEEDED(worker->CreateShaderResourceView(privateColour.Get(),nullptr,&source)) && SUCCEEDED(host->CreateShaderResourceView(colour.Get(),nullptr,&hostSource)),"shared colour SRVs");
        ID3D11ShaderResourceView* keep=hostSource.Get();host4->CSSetShaderResources(5,1,&keep);
        desc=TextureDesc(33,19,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE);
        desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        Ptr<ID3D11Texture2D> motion;Ptr<ID3D11UnorderedAccessView> motionView;Ptr<ID3D12Resource> motion12;
        Check(SUCCEEDED(worker->CreateTexture2D(&desc,nullptr,&motion)) && SUCCEEDED(worker->CreateUnorderedAccessView(motion.Get(),nullptr,&motionView)),"private shared dense motion");
        Ptr<IDXGIResource1> dxMotion;Check(SUCCEEDED(motion.As(&dxMotion)) && SUCCEEDED(dxMotion->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle)),"motion NT handle");
        Check(SUCCEEDED(d12->OpenSharedHandle(handle,IID_PPV_ARGS(&motion12))),"dense motion imports into D3D12");CloseHandle(handle);
        const unsigned bpp=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
        std::vector<unsigned short> hdr(33*19*4,0x3c00);
        std::vector<unsigned char> sdr(33*19*4,255);
        host4->UpdateSubresource(colour.Get(),0,nullptr,bpp==8?static_cast<const void*>(hdr.data()):static_cast<const void*>(sdr.data()),33*bpp,0);
        Check(SUCCEEDED(host4->Signal(inputFence.Get(),++value)),"host frame signal");host4->Flush();
        Check(SUCCEEDED(worker4->Wait(workerInput.Get(),value)),"private waits for LS frame");
        ID3D11ShaderResourceView* srv=source.Get();ID3D11UnorderedAccessView* uav=motionView.Get();
        worker4->CSSetShaderResources(0,1,&srv);worker4->CSSetUnorderedAccessViews(0,1,&uav,nullptr);worker4->CSSetShader(shader.Get(),nullptr,0);
        worker4->Dispatch(5,3,1);Unbind(worker4.Get());
        Check(SUCCEEDED(worker4->Signal(workerReady.Get(),value)),"private motion signal");worker4->Flush();
        Check(SUCCEEDED(queue->Wait(ready12.Get(),value)),"D3D12 waits for private preprocessing");
        Check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(command->Reset(allocator.Get(),nullptr)),"reset D3D12 commands");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;auto resourceDesc=motion12->GetDesc();
        d12->GetCopyableFootprints(&resourceDesc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=1;buffer.DepthOrArraySize=1;
        buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;Ptr<ID3D12Resource> readback;
        Check(SUCCEEDED(d12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))),"D3D12 readback");
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={motion12.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
        command->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=motion12.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
        command->CopyTextureRegion(&to,0,0,0,&from,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);command->ResourceBarrier(1,&barrier);
        Check(SUCCEEDED(command->Close()),"close motion copy");ID3D12CommandList* commands[]={command.Get()};queue->ExecuteCommandLists(1,commands);
        Check(SUCCEEDED(queue->Signal(completion.Get(),value)) && SUCCEEDED(host4->Wait(hostCompletion.Get(),value)),"return completion to LS context");host4->Flush();
        Check(SUCCEEDED(completion->SetEventOnCompletion(value,event)) && WaitForSingleObject(event,3000)==WAIT_OBJECT_0,"whole GPU chain completes");
        void* data=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};Check(SUCCEEDED(readback->Map(0,&range,&data)),"read D3D12 dense motion");
        for(unsigned y:{0u,9u,18u}) for(unsigned x:{0u,16u,32u}) {
            const auto* pixel=reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(data)+footprint.Offset+y*footprint.Footprint.RowPitch+x*4);
            Check(pixel[0]==0x4000 && pixel[1]==0xc400,"D3D12 sees completed +2/-4 private motion at image edges");
        }
        D3D12_RANGE written{};readback->Unmap(0,&written);
        Ptr<ID3D11ComputeShader> retained;host4->CSGetShader(&retained,nullptr,nullptr);
        Ptr<ID3D11ShaderResourceView> retainedSource;host4->CSGetShaderResources(5,1,&retainedSource);
        Check(retained.Get()==sentinel.Get() && retainedSource.Get()==hostSource.Get(),"LS shader/SRV state preserved by private preprocessing");
        keep=nullptr;host4->CSSetShaderResources(5,1,&keep);
    }
    CloseHandle(event);std::puts("Real host D3D11 -> private D3D11 -> D3D12 -> host fences, RGB/BGR/scRGB shared textures and LS context isolation passed");
}
