#include <ls_output_bridge.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <array>
using Microsoft::WRL::ComPtr;
namespace {
IDXGISwapChain* selected=nullptr;
unsigned originalCalls=0,filtered=0,sunk=0,groups=0;bool replace=false;
std::mutex mutex;std::condition_variable condition;bool blocked=false,entered=false,released=false;
HRESULT STDMETHODCALLTYPE Capture(IDXGISwapChain*,UINT,UINT){++originalCalls;return S_OK;}
HRESULT STDMETHODCALLTYPE Capture1(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*){++originalCalls;return S_OK;}
void Check(bool b,const char* why){if(!b){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
HRESULT WINAPI Generate(IDXGISwapChain* sc,UINT* s,UINT* f,BOOL*,LsBridgeFrame*,void*){
    if(sc!=selected)return S_OK;++groups;
    for(unsigned i=0;i<3;++i)Check(LsBridgePresent(sc,*s,*f)==S_OK,"intermediate forwarding");return S_OK;
}
HRESULT WINAPI Filter(IDXGISwapChain* sc,UINT*,UINT*,BOOL*,LsBridgeFrame*,void*){
    if(sc!=selected)return S_OK;++filtered;
    if(blocked){std::unique_lock<std::mutex> lock(mutex);entered=true;condition.notify_all();condition.wait(lock,[]{return released;});}
    return S_OK;
}
HRESULT WINAPI Sink(IDXGISwapChain* sc,UINT*,UINT*,BOOL* h,LsBridgeFrame* frame,void*){
    if(sc!=selected)return S_OK;++sunk;if(replace){frame->externalPresented=TRUE;*h=TRUE;}return S_OK;
}
void WINAPI FilterAfter(IDXGISwapChain* sc,HRESULT,LsBridgeFrame* frame,void*){
    if(sc==selected && replace)Check(frame->externalPresented,"filter must observe alternative output, including handled submissions");
}
void Replace(void** t,unsigned slot,void* fn){DWORD old=0,unused=0;Check(VirtualProtect(t+slot,sizeof(void*),PAGE_READWRITE,&old)!=FALSE,"table protection");InterlockedExchangePointer(t+slot,fn);VirtualProtect(t+slot,sizeof(void*),old,&unused);}
}
int main(){
    ComPtr<ID3D11Device> device;Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr)),"WARP device");
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"LSBridgeTest";RegisterClassW(&wc);
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"test",WS_POPUP,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);Check(hwnd!=nullptr,"window");
    ComPtr<IDXGIDevice> dx;ComPtr<IDXGIAdapter> a;ComPtr<IDXGIFactory2> f;device.As(&dx);dx->GetAdapter(&a);a->GetParent(IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 d{};d.Width=d.Height=64;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BufferCount=2;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;Check(SUCCEEDED(f->CreateSwapChainForHwnd(device.Get(),hwnd,&d,nullptr,nullptr,&chain)),"chain");selected=chain.Get();
    void** table=*reinterpret_cast<void***>(chain.Get());void* old=table[8];void* old1=table[22];Replace(table,8,reinterpret_cast<void*>(Capture));Replace(table,22,reinterpret_cast<void*>(Capture1));
    LsBridgeCallbacks filter{};filter.owner=LS_OWNER_HDR;filter.kind=LS_BRIDGE_FILTER;filter.before=Filter;filter.after=FilterAfter;
    LsBridgeCallbacks generator{};generator.owner=LS_OWNER_DLSSFG;generator.before=Generate;
    LsBridgeCallbacks sink{};sink.owner=LS_OWNER_SMOOTH;sink.kind=LS_BRIDGE_SINK;sink.before=Sink;
    // Register in the opposite order to processing; ordering is by phase.
    Check(LsBridgeRegister(&filter) && LsBridgeRegister(&sink) && LsBridgeRegister(&generator) && LsBridgeInstall(chain.Get()),"registrations / install");
    Check(chain->Present(0,0)==S_OK && originalCalls==4 && filtered==4 && sunk==4 && groups==1,"HDR and sink see every x4 frame");
    const DXGI_PRESENT_PARAMETERS full{};Check(chain->Present1(0,0,&full)==S_OK && originalCalls==8 && filtered==8 && groups==2,"Present1 x4 processing");
    replace=true;Check(chain->Present(0,0)==S_OK && originalCalls==8 && filtered==12 && sunk==12,"alternative output replaces submissions while HDR receives after callback");replace=false;
    RECT dirty{};DXGI_PRESENT_PARAMETERS partial{};partial.DirtyRectsCount=1;partial.pDirtyRects=&dirty;
    const auto before=filtered;chain->Present1(0,0,&partial);chain->Present(0,DXGI_PRESENT_TEST);chain->Present(0,DXGI_PRESENT_DO_NOT_WAIT);
    Check(filtered==before && originalCalls==11,"partial, TEST and nonblocking remain unchanged");
    // Real COM object, a distinct vtable image: exercise device/chain changes
    // without assuming every output uses the first DXGI table forever.
    std::array<void*,41> second{};for(unsigned i=0;i<second.size();++i)second[i]=table[i];second[8]=reinterpret_cast<void*>(Capture);second[22]=reinterpret_cast<void*>(Capture1);
    *reinterpret_cast<void***>(chain.Get())=second.data();Check(LsBridgeInstall(chain.Get()),"new live table installation");
    Check(chain->Present(0,0)==S_OK && originalCalls==15 && filtered==before+4,"new table dispatches generation and filters");
    Check(LsBridgeUnregister(LS_OWNER_DLSSFG) && LsBridgeUnregister(LS_OWNER_SMOOTH),"remove generator and sink");
    blocked=true;
    std::thread render([&]{chain->Present(0,0);});
    {std::unique_lock<std::mutex> lock(mutex);Check(condition.wait_for(lock,std::chrono::seconds(2),[]{return entered;}),"entered filter");}
    std::atomic<bool> drained{false};std::thread shutdown([&]{Check(LsBridgeUnregister(LS_OWNER_HDR),"filter drain");drained=true;});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));Check(!drained,"shutdown waits for borrowed frame resources");
    {std::lock_guard<std::mutex> lock(mutex);released=true;}condition.notify_all();render.join();shutdown.join();Check(drained,"shutdown completes after frame exits");
    *reinterpret_cast<void***>(chain.Get())=table;Replace(table,8,old);Replace(table,22,old1);chain.Reset();DestroyWindow(hwnd);
    std::puts("phase order, x4/Present1, alternative sink, partial bypass, new table and callback lifetime passed");
}
