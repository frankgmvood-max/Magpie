// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <output_window.h>
#include <presentation.h>
#include <d3d11.h>
namespace hdr {
class Presenter {
public:
    ~Presenter(){Reset();}
    bool Init(ID3D11Device* device,HWND parent,UINT width,UINT height){
        Reset();if(!window_.Create(parent,width,height))return false;
        ls::ComPtr<IDXGIDevice> dx;ls::ComPtr<IDXGIAdapter> adapter;ls::ComPtr<IDXGIFactory2> factory;
        if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dx))) || FAILED(dx->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))return false;
        tearing_=ls::TearingSupported(factory.Get());
        DXGI_SWAP_CHAIN_DESC1 d{};d.Width=width;d.Height=height;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.SampleDesc.Count=1;
        d.BufferCount=2;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT|(tearing_?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0);
        ls::ComPtr<IDXGISwapChain1> chain;
        if(FAILED(factory->CreateSwapChainForHwnd(device,window_.Get(),&d,nullptr,nullptr,&chain)) || FAILED(chain.As(&chain_)))return false;
        UINT support=0;
        if(FAILED(chain_->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,&support)) ||
           !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) || FAILED(chain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)))return false;
        chain_->SetMaximumFrameLatency(1);latency_=chain_->GetFrameLatencyWaitableObject();device->GetImmediateContext(&context_);return true;
    }
    bool Present(ID3D11Texture2D* texture){
        if(!chain_ || !texture)return false;
        if(latency_ && WaitForSingleObject(latency_,500)!=WAIT_OBJECT_0){window_.Show(false);return false;}
        ls::ComPtr<ID3D11Texture2D> back;
        if(FAILED(chain_->GetBuffer(0,IID_PPV_ARGS(&back))))return false;
        context_->CopyResource(back.Get(),texture);context_->Flush();
        const HRESULT hr=chain_->Present(0,ls::PresentFlags(0,tearing_));window_.Show(hr==S_OK);return hr==S_OK;
    }
    void Reset(){window_.Show(false);chain_.Reset();context_.Reset();if(latency_)CloseHandle(latency_);latency_=nullptr;window_.Reset();}
    void Hide(){window_.Show(false);}
    bool Ready()const{return bool(chain_);}
private:
    ls::OutputWindow window_;ls::ComPtr<IDXGISwapChain3> chain_;ls::ComPtr<ID3D11DeviceContext> context_;HANDLE latency_=nullptr;bool tearing_=false;
};
}
