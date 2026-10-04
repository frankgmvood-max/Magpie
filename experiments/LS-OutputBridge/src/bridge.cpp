// SPDX-License-Identifier: MIT
// Live-vtable hook design derived from Echo-Storm/ls-addon-manager (MIT).
#define LS_OUTPUT_BRIDGE_BUILD
#include "ls_output_bridge.h"
#include <array>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdio>

namespace {
using Present=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using Present1=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
constexpr unsigned maxTables=32,maxCallbacks=8;
struct Table {void** address=nullptr;std::atomic<Present> present{nullptr};std::atomic<Present1> present1{nullptr};};
struct Handler {LsBridgeCallbacks callbacks{};unsigned running=0;bool enabled=false;};
std::array<Table,maxTables> tables;
std::array<Handler,maxCallbacks> handlers;
std::mutex tableMutex,handlerMutex;
std::condition_variable handlerIdle;
std::atomic<uint32_t> hits{0};
thread_local unsigned nesting=0,originalNesting=0,tableIndex=0;
thread_local bool currentPresent1=false;

struct Calls {
    std::array<Handler*,maxCallbacks> held{};
    std::array<LsBridgeCallbacks,maxCallbacks> callbacks{};
    unsigned count=0;
    Calls(LsBridgeKind kind) {
        std::lock_guard<std::mutex> lock(handlerMutex);
        for(auto& h:handlers) if(h.enabled && h.callbacks.kind==kind) {
            held[count]=&h;callbacks[count++]=h.callbacks;++h.running;
        }
    }
    ~Calls() {
        std::lock_guard<std::mutex> lock(handlerMutex);
        for(unsigned i=0;i<count;++i)--held[i]->running;
        handlerIdle.notify_all();
    }
    HRESULT Before(IDXGISwapChain* sc,UINT& sync,UINT& flags,BOOL& handled,LsBridgeFrame& frame) {
        for(unsigned i=0;i<count;++i) {
            const auto& cb=callbacks[i];
            if(cb.before) {const HRESULT hr=cb.before(sc,&sync,&flags,&handled,&frame,cb.user);if(handled)return hr;}
        }
        return S_OK;
    }
    void After(IDXGISwapChain* sc,HRESULT hr,LsBridgeFrame& frame) {
        for(unsigned i=0;i<count;++i) {
            const auto& cb=callbacks[i];if(cb.after)cb.after(sc,hr,&frame,cb.user);
        }
    }
};
bool Full(UINT flags,const DXGI_PRESENT_PARAMETERS* p) {
    return !(flags & (DXGI_PRESENT_TEST|DXGI_PRESENT_DO_NOT_WAIT)) &&
        (!p || (!p->DirtyRectsCount && !p->pScrollRect && !p->pScrollOffset));
}
HRESULT Actual(unsigned index,IDXGISwapChain* sc,UINT sync,UINT flags,bool p1,
               const DXGI_PRESENT_PARAMETERS* params,bool generated,bool process) {
    LsBridgeFrame frame{};frame.generated=generated;frame.present1=p1;
    BOOL handled=FALSE;
    Calls filters(LS_BRIDGE_FILTER),sinks(LS_BRIDGE_SINK);
    if(process) {
        const auto hr=filters.Before(sc,sync,flags,handled,frame);
        if(handled) return hr;
        const auto sinkHr=sinks.Before(sc,sync,flags,handled,frame);
        if(handled){sinks.After(sc,sinkHr,frame);filters.After(sc,sinkHr,frame);return sinkHr;}
    }
    ++originalNesting;
    const HRESULT hr=p1 ? (tables[index].present1.load()?tables[index].present1.load()(static_cast<IDXGISwapChain1*>(sc),sync,flags,params):E_NOINTERFACE)
                       : (tables[index].present.load()?tables[index].present.load()(sc,sync,flags):E_FAIL);
    --originalNesting;
    if(process) {
        // SM gets first refusal on the processed HDR texture; HDR presents
        // its own output only when no alternative generator presented it.
        sinks.After(sc,hr,frame);
        filters.After(sc,hr,frame);
    }
    return hr;
}
HRESULT Dispatch(unsigned index,IDXGISwapChain* sc,UINT sync,UINT flags,bool p1,const DXGI_PRESENT_PARAMETERS* params) {
    const unsigned oldIndex=tableIndex;const bool oldP1=currentPresent1;
    tableIndex=index;currentPresent1=p1;++nesting;hits.fetch_add(1,std::memory_order_relaxed);
    const bool full=Full(flags,params),outer=nesting==1 && !originalNesting && full;
    LsBridgeFrame group{};group.present1=p1;BOOL handled=FALSE;
    Calls generators(LS_BRIDGE_GENERATOR);
    const HRESULT made=outer?generators.Before(sc,sync,flags,handled,group):S_OK;
    const HRESULT hr=handled?made:Actual(index,sc,sync,flags,p1,params,false,outer);
    if(outer && !handled) generators.After(sc,hr,group);
    --nesting;tableIndex=oldIndex;currentPresent1=oldP1;return hr;
}
template<unsigned I> HRESULT STDMETHODCALLTYPE Hook(IDXGISwapChain* sc,UINT s,UINT f){return Dispatch(I,sc,s,f,false,nullptr);}
template<unsigned I> HRESULT STDMETHODCALLTYPE Hook1(IDXGISwapChain1* sc,UINT s,UINT f,const DXGI_PRESENT_PARAMETERS* p){return Dispatch(I,sc,s,f,true,p);}
template<size_t... I> constexpr auto Hooks(std::index_sequence<I...>){return std::array<Present,sizeof...(I)>{&Hook<I>...};}
template<size_t... I> constexpr auto Hooks1(std::index_sequence<I...>){return std::array<Present1,sizeof...(I)>{&Hook1<I>...};}
constexpr auto hooks=Hooks(std::make_index_sequence<maxTables>{});
constexpr auto hooks1=Hooks1(std::make_index_sequence<maxTables>{});
template<class Fn> bool Patch(void** table,unsigned slot,void* replacement,std::atomic<Fn>& original) {
    DWORD protect=0;if(!VirtualProtect(table+slot,sizeof(void*),PAGE_READWRITE,&protect)) return false;
    bool changed=false;
    for(unsigned n=0;n<8 && !changed;++n) {
        void* prior=InterlockedCompareExchangePointer(table+slot,nullptr,nullptr);
        original.store(reinterpret_cast<Fn>(prior),std::memory_order_release);
        changed=InterlockedCompareExchangePointer(table+slot,replacement,prior)==prior;
    }
    DWORD ignored=0;VirtualProtect(table+slot,sizeof(void*),protect,&ignored);return changed;
}
}
uint32_t WINAPI LsBridgeVersion(){return 0x010000;}
BOOL WINAPI LsBridgeRegister(const LsBridgeCallbacks* cb) {
    if(!cb || cb->size!=sizeof(*cb) || !cb->owner || cb->kind>LS_BRIDGE_SINK) return FALSE;
    const auto address=cb->before?reinterpret_cast<LPCWSTR>(cb->before):reinterpret_cast<LPCWSTR>(cb->after);
    HMODULE module=nullptr;
    if(!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,address,&module)) return FALSE;
    std::lock_guard<std::mutex> lock(handlerMutex);
    for(auto& h:handlers) if(h.callbacks.owner==cb->owner || !h.callbacks.owner) {
        if(h.running) return FALSE;
        h.callbacks=*cb;h.enabled=true;return TRUE;
    }
    return FALSE;
}
BOOL WINAPI LsBridgeUnregister(uint32_t owner) {
    std::unique_lock<std::mutex> lock(handlerMutex);
    for(auto& h:handlers) if(h.callbacks.owner==owner) {
        h.enabled=false;
        if(!handlerIdle.wait_for(lock,std::chrono::milliseconds(1000),[&]{return h.running==0;})) return FALSE;
        h.callbacks={};return TRUE;
    }
    return TRUE;
}
BOOL WINAPI LsBridgeInstall(IDXGISwapChain* chain,LsBridgeLog log,void* user) {
    if(!chain) return FALSE;
    IDXGISwapChain1* view=nullptr;if(FAILED(chain->QueryInterface(IID_PPV_ARGS(&view)))) return FALSE;
    auto** table=*reinterpret_cast<void***>(view);view->Release();
    std::lock_guard<std::mutex> lock(tableMutex);
    for(unsigned i=0;i<maxTables;++i) {
        if(tables[i].address==table && table[8]==reinterpret_cast<void*>(hooks[i]) &&
           table[22]==reinterpret_cast<void*>(hooks1[i])) return TRUE;
        // If another hook replaced either slot, use a fresh trampoline pair.
        // An overlay may still call the old pair. Reusing its saved original
        // would create a loop through the overlay's trampoline.
        if(tables[i].address) continue;
        HMODULE self=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&LsBridgeInstall),&self)) return FALSE;
        if(!Patch(table,8,reinterpret_cast<void*>(hooks[i]),tables[i].present)) return FALSE;
        tables[i].address=table;
        if(!Patch(table,22,reinterpret_cast<void*>(hooks1[i]),tables[i].present1)) return FALSE;
        if(log){char text[192];std::snprintf(text,sizeof text,"LS output bridge: live Present table #%u installed at %p (chain %p)",i,static_cast<void*>(table),static_cast<void*>(chain));log(text,user);}
        return TRUE;
    }
    if(log)log("LS output bridge: live table limit reached; restart LS",user);return FALSE;
}
HRESULT WINAPI LsBridgePresent(IDXGISwapChain* sc,UINT sync,UINT flags,uint32_t api) {
    if(!sc || !nesting || api>2) return E_INVALIDARG;
    const bool p1=api==2 || (api==0 && currentPresent1);
    const DXGI_PRESENT_PARAMETERS full{};
    return Actual(tableIndex,sc,sync,flags,p1,p1?&full:nullptr,true,Full(flags,nullptr));
}
BOOL WINAPI LsBridgeUsesPresent1(){return currentPresent1;}
uint32_t WINAPI LsBridgeHits(){return hits.load();}
