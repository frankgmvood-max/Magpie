// SPDX-License-Identifier: MIT
#include "present_hook.h"
#include <ls_output_bridge.h>
#include <atomic>
namespace {
std::atomic<PresentHook::Callback> callback{nullptr};
std::atomic<PresentHook::AfterCallback> afterCallback{nullptr};
std::atomic<bool> registered{false};
HRESULT WINAPI Before(IDXGISwapChain* sc,UINT* sync,UINT* flags,BOOL* handled,LsBridgeFrame*,void*) {
    bool h=false;const auto cb=callback.load();const HRESULT hr=cb?cb(sc,*sync,*flags,h):S_OK;*handled=h;return hr;
}
void WINAPI After(IDXGISwapChain* sc,HRESULT hr,LsBridgeFrame*,void*) {
    if(const auto cb=afterCallback.load())cb(sc,hr);
}
void WINAPI Log(const char* text,void* value) {
    const auto* fn=static_cast<PresentHook::LogFn*>(value);if(fn && *fn)(*fn)(text);
}
}
bool PresentHook::Install(IDXGISwapChain* sc,Callback cb,LogFn log,AfterCallback after) {
    if(!registered.load()) {
        callback.store(cb);afterCallback.store(after);
        LsBridgeCallbacks callbacks{};callbacks.owner=LS_OWNER_DLSSFG;callbacks.before=Before;callbacks.after=After;
        if(!LsBridgeRegister(&callbacks))return false;
        registered.store(true);
    }
    return LsBridgeInstall(sc,Log,&log)!=FALSE;
}
void PresentHook::Uninstall() {
    callback.store(nullptr);afterCallback.store(nullptr);
    if(registered.exchange(false))LsBridgeUnregister(LS_OWNER_DLSSFG);
}
bool PresentHook::Installed(){return registered.load();}
unsigned PresentHook::Hits(){return LsBridgeHits();}
HRESULT PresentHook::PresentOriginal(IDXGISwapChain* sc,UINT sync,UINT flags,fg::PresentApi api){
    return LsBridgePresent(sc,sync,flags,unsigned(api));
}
bool PresentHook::UsesPresent1(){return LsBridgeUsesPresent1()!=FALSE;}
void PresentHook::DumpState(LogFn log){if(log)log(registered.load()?"FG registered in shared LS output bridge":"FG output callback disabled");}
