// SPDX-License-Identifier: MIT
#pragma once
#include <d3d11_4.h>
#include <d3d10.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
namespace ls {
using Microsoft::WRL::ComPtr;
// Copies, fences and Flush are the only operations made on LS's context.
// All shaders and NVIDIA evaluation use the private, same-adapter context.
class SharedGpu {
public:
    ~SharedGpu(){if(event)CloseHandle(event);}
    bool Init(ID3D11Device* host,bool requireNvidia=true) {
        if(!host || FAILED(host->QueryInterface(IID_PPV_ARGS(&hostDevice))))return false;
        ComPtr<ID3D11DeviceContext> c;host->GetImmediateContext(&c);
        if(FAILED(c.As(&hostContext)))return false;
        ComPtr<IDXGIDevice> dx;
        if(FAILED(host->QueryInterface(IID_PPV_ARGS(&dx))) || FAILED(dx->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&adapterDesc)))return false;
        if(requireNvidia && adapterDesc.VendorId!=0x10de)return false;
        ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> p;
        if(FAILED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&p)) ||
           FAILED(d.As(&device)) || FAILED(p.As(&context)))return false;
        ComPtr<ID3D10Multithread> protection;
        if(SUCCEEDED(device.As(&protection)))protection->SetMultithreadProtected(TRUE);
        if(!Fence(hostDevice.Get(),device.Get(),incomingHost,incomingPrivate) || !Fence(device.Get(),hostDevice.Get(),outgoingPrivate,outgoingHost))return false;
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);return event!=nullptr;
    }
    bool Texture(UINT width,UINT height,DXGI_FORMAT format,UINT bind,ComPtr<ID3D11Texture2D>& privateTexture,ComPtr<ID3D11Texture2D>& hostTexture) {
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.Format=format;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;
        d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&privateTexture)))return false;
        ComPtr<IDXGIResource1> resource;HANDLE handle=nullptr;
        if(FAILED(privateTexture.As(&resource)) || FAILED(resource->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle)))return false;
        const HRESULT hr=hostDevice->OpenSharedResource1(handle,IID_PPV_ARGS(&hostTexture));CloseHandle(handle);return SUCCEEDED(hr);
    }
    bool Upload(ID3D11Texture2D* destinationHost,ID3D11Texture2D* sourceHost) {
        if(lost || !destinationHost || !sourceHost)return false;
        hostContext->CopyResource(destinationHost,sourceHost);
        if(FAILED(hostContext->Signal(incomingHost.Get(),++inputValue)))return false;
        hostContext->Flush();return SUCCEEDED(context->Wait(incomingPrivate.Get(),inputValue));
    }
    bool Done(bool cpuWait=true) {
        if(lost || FAILED(context->Signal(outgoingPrivate.Get(),++outputValue)))return false;
        context->Flush();
        if(FAILED(hostContext->Wait(outgoingHost.Get(),outputValue)))return false;
        return !cpuWait || Wait();
    }
    bool Wait() {
        if(!outputValue)return true;
        const auto done=outgoingPrivate->GetCompletedValue();
        if(done==UINT64_MAX){lost=true;return false;}
        if(done>=outputValue)return true;
        ResetEvent(event);
        if(FAILED(outgoingPrivate->SetEventOnCompletion(outputValue,event)) || WaitForSingleObject(event,500)!=WAIT_OBJECT_0 ||
           outgoingPrivate->GetCompletedValue()==UINT64_MAX || outgoingPrivate->GetCompletedValue()<outputValue){lost=true;return false;}
        return true;
    }
    ComPtr<ID3D11Device5> hostDevice,device;
    ComPtr<ID3D11DeviceContext4> hostContext,context;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    bool lost=false;
private:
    static bool Fence(ID3D11Device5* producer,ID3D11Device5* consumer,ComPtr<ID3D11Fence>& p,ComPtr<ID3D11Fence>& c) {
        HANDLE handle=nullptr;
        if(FAILED(producer->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&p))) || FAILED(p->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle)))return false;
        const HRESULT hr=consumer->OpenSharedFence(handle,IID_PPV_ARGS(&c));CloseHandle(handle);return SUCCEEDED(hr);
    }
    ComPtr<ID3D11Fence> incomingHost,incomingPrivate,outgoingPrivate,outgoingHost;
    HANDLE event=nullptr;uint64_t inputValue=0,outputValue=0;
};
}
