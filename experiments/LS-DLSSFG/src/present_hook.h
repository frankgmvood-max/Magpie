// Generator facade over the shared output bridge. Each live DXGI vtable keeps
// its own original Present/Present1 pair; no probe swapchain is created.
#pragma once
#include <dxgi1_2.h>
#include "settings.h"
#include <functional>

namespace PresentHook {
    using LogFn = std::function<void(const char*)>;
    // Called before the original Present, on the presenting thread, with that present's sync interval and flags. Not called for a
    // DXGI_PRESENT_TEST present.
    // Reference arguments also update the outer real-frame Present/Present1.
    using Callback = HRESULT (*)(IDXGISwapChain* sc, UINT& sync, UINT& flags, bool& handled);
    // Invoked once after the outer full-frame LS Present has returned, while
    // nesting protection is still active. Not for TEST, partial or handled calls.
    using AfterCallback = void (*)(IDXGISwapChain* sc, HRESULT result) noexcept;
    bool Install(IDXGISwapChain* chain, Callback cb, LogFn log, AfterCallback after=nullptr);
    bool Uninstall(); // false: an in-flight frame still owns backend resources
    bool Installed();
    unsigned Hits();             // presents seen in the process, by anyone
    // A present of our own, from inside the callback (a frame of our own before Lossless Scaling's): the original Present, which does not
    // run the callback again.
    HRESULT PresentOriginal(IDXGISwapChain* sc, UINT sync, UINT flags,fg::PresentApi api=fg::PresentApi::Auto);
    bool UsesPresent1();         // entry point of the current LS present
    void DumpState(LogFn log);   // diagnostics: the hit count, and whether the patched slots are still ours
}
