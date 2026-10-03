// Derived from Echo-Storm/ls-addon-manager fbcc3c1 (MIT).
// Modified: callbacks may handle both presents; preserve partial Present1 semantics.
#include "present_hook.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

// Why patch the function table and not the code: something else in the process (the NVIDIA overlay, which hooks Present at the first present)
// hooks dxgi's Present after us and calls on to an untouched copy of the code, which quietly bypasses a code hook placed before it (seen in the
// offline host: our jump replaced, the hit count stuck at 1). The swap chain's function table is a static table in dxgi.dll, shared by every swap
// chain of that kind and never refreshed per object, so a patched slot keeps working and still reaches whatever code hook sits on the function.
namespace {
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
constexpr int kPresentSlot = 8, kPresent1Slot = 22;

// Once patched, the table keeps our functions for the life of the process, and this DLL is pinned so they stay in memory: something that
// hooked after us (the other addon of the pair, an overlay) calls on through them to the real Present, even after the manager unloads this
// addon. Uninstall only clears the callback, which makes them pass straight through, and a second Install (the addon started again) only
// sets it again: patching a second time would make a loop of the hook chain.
void** g_patched = nullptr;
std::atomic<PresentFn> g_present{nullptr};
std::atomic<Present1Fn> g_present1{nullptr};
void** g_table = nullptr;
std::atomic<PresentHook::Callback> g_callback{ nullptr };
std::atomic<unsigned> g_hits{ 0 };
thread_local int t_nesting = 0;

// Only the outermost present on a thread runs the callback, so a present made inside it (or by a hook we call on to) does not run it again.
HRESULT Before(IDXGISwapChain* sc, UINT sync, UINT flags, bool& handled) {
    g_hits.fetch_add(1, std::memory_order_relaxed);
    if (t_nesting == 1 && !(flags & DXGI_PRESENT_TEST))
        if (const PresentHook::Callback cb = g_callback.load(std::memory_order_acquire)) return cb(sc, sync, flags, handled);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE OnPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    ++t_nesting; bool handled = false; const HRESULT result = Before(sc, sync, flags, handled);
    const auto original = g_present.load(std::memory_order_acquire);
    const HRESULT hr = handled ? result : (original ? original(sc, sync, flags) : E_FAIL); --t_nesting;
    return hr;
}
HRESULT STDMETHODCALLTYPE OnPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    ++t_nesting; bool handled = false;
    // Dirty/scroll presents cannot be expanded into two full-screen presents.
    const HRESULT result = (!params || (!params->DirtyRectsCount && !params->pScrollRect))
        ? Before(sc, sync, flags, handled) : S_OK;
    const auto original = g_present1.load(std::memory_order_acquire);
    const HRESULT hr = handled ? result : (original ? original(sc, sync, flags, params) : E_FAIL); --t_nesting;
    return hr;
}

// Store the original BEFORE publishing our hook; UI Present can race installation.
template<class Fn> bool Swap(void** table, int slot, void* fn, std::atomic<Fn>& original) {
    DWORD protect = 0;
    if (!VirtualProtect(table + slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
    bool changed = false;
    for (unsigned attempt=0; attempt<4 && !changed; ++attempt) {
        void* was=InterlockedCompareExchangePointer(table + slot,nullptr,nullptr);
        original.store(reinterpret_cast<Fn>(was),std::memory_order_release);
        changed=InterlockedCompareExchangePointer(table + slot,fn,was)==was;
    }
    DWORD restored = 0; VirtualProtect(table + slot,sizeof(void*),protect,&restored);
    return changed;
}
} // namespace

bool PresentHook::Install(IDXGISwapChain* chain, Callback cb, LogFn log) {
    if (g_table) return true;
    IDXGISwapChain1* view=nullptr;
    if (!chain || FAILED(chain->QueryInterface(IID_PPV_ARGS(&view)))) {
        log("PresentHook: output has no IDXGISwapChain1"); return false;
    }
    void** const table=*reinterpret_cast<void***>(view);
    view->Release(); // caller owns the live chain throughout installation
    if (g_patched) {
        if (g_patched!=table) {log("PresentHook: output vtable changed; restart LS");return false;}
        g_callback.store(cb,std::memory_order_release);g_table=g_patched;return true;
    }
    HMODULE self=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&OnPresent),&self)) {log("PresentHook: cannot pin addon module");return false;}
    if (!Swap(table,kPresentSlot,reinterpret_cast<void*>(&OnPresent),g_present)) {
        log("PresentHook: cannot patch output Present slot");return false;
    }
    const bool present1=Swap(table,kPresent1Slot,reinterpret_cast<void*>(&OnPresent1),g_present1);
    g_table=g_patched=table;
    g_callback.store(cb,std::memory_order_release);
    char text[180];
    std::snprintf(text,sizeof text,"PresentHook: installed directly on live LS output %p; Present1=%s; no probe window/device",
        static_cast<void*>(chain),present1?"yes":"no");
    log(text);return true;
}

void PresentHook::Uninstall() {
    g_callback.store(nullptr, std::memory_order_release);   // our functions stay in the table and pass straight through (see above)
    g_table = nullptr;
}

bool PresentHook::Installed() { return g_table != nullptr; }
HRESULT PresentHook::PresentOriginal(IDXGISwapChain* sc, UINT sync, UINT flags) {
    const auto original=g_present.load(std::memory_order_acquire);return original?original(sc,sync,flags):E_FAIL;
}
unsigned PresentHook::Hits() { return g_hits.load(std::memory_order_relaxed); }

void PresentHook::DumpState(LogFn log) {
    if (!g_table) { log("PresentHook: not installed"); return; }
    char text[200];
    snprintf(text, sizeof text, "PresentHook: %u presents; table %p: Present slot %s, Present1 slot %s", Hits(), (void*)g_table,
             g_table[kPresentSlot] == (void*)&OnPresent ? "ours" : "taken by another hook",
             g_table[kPresent1Slot] == (void*)&OnPresent1 ? "ours" : "not ours");
    log(text);
}
