#include "output_state.h"
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
void Check(bool ok,const char* why) {if(!ok){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
int main() {
    ComPtr<ID3D11Device> device;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr)),"software device");
    ComPtr<IDXGIDevice> dx;ComPtr<IDXGIDevice1> dx1;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory2> factory;
    Check(SUCCEEDED(device.As(&dx)) && SUCCEEDED(device.As(&dx1)) && SUCCEEDED(dx->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))),"factory");
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"FGQueueTest";RegisterClassW(&wc);
    for(bool waitable:{false,true}) {
        HWND window=CreateWindowW(wc.lpszClassName,L"test",WS_POPUP,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);Check(window!=nullptr,"window");
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=64;desc.Height=64;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags=waitable?DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT:0;
        ComPtr<IDXGISwapChain1> chain;ComPtr<IDXGISwapChain2> chain2;
        Check(SUCCEEDED(factory->CreateSwapChainForHwnd(device.Get(),window,&desc,nullptr,nullptr,&chain)) && SUCCEEDED(chain.As(&chain2)),"chain");
        UINT previous=0,current=0;
        auto get=[&](UINT* value){return waitable?chain2->GetMaximumFrameLatency(value):dx1->GetMaximumFrameLatency(value);};
        auto set=[&](UINT value){return waitable?chain2->SetMaximumFrameLatency(value):dx1->SetMaximumFrameLatency(value);};
        Check(SUCCEEDED(get(&previous)) && SUCCEEDED(set(10)),"set LS-like queue");
        fg::OutputQueue queue;
        Check(queue.Set(chain.Get(),1) && queue.Previous()==10 && queue.PerSwapChain()==waitable,"apply queue via correct DXGI API");
        Check(SUCCEEDED(get(&current)) && current==1,"queue reduced to 1");
        queue.Reset();Check(SUCCEEDED(get(&current)) && current==10,"original queue restored");
        Check(queue.Set(chain.Get(),1) && SUCCEEDED(set(2)),"LS changes queue while addon active");
        queue.Reset();Check(SUCCEEDED(get(&current)) && current==2,"new LS setting not overwritten");
        Check(!queue.Set(chain.Get(),0) && !queue.Set(chain.Get(),17),"invalid queue requests rejected");
        set(previous);chain2.Reset();chain.Reset();DestroyWindow(window);
    }
    fg::GSyncProbe probe;probe.Init(nullptr,[](const char*){});
    Check(probe.Query(nullptr).activeStatus!=0,"unavailable driver probe reports unknown");
    std::puts("device/swapchain queue application, restoration and later LS override preservation passed");
}
