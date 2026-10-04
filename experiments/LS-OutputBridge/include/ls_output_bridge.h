// SPDX-License-Identifier: MIT
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdint>

#ifdef LS_OUTPUT_BRIDGE_BUILD
#define LS_BRIDGE_API extern "C" __declspec(dllexport)
#else
#define LS_BRIDGE_API extern "C" __declspec(dllimport)
#endif
enum LsBridgeKind : uint32_t { LS_BRIDGE_GENERATOR, LS_BRIDGE_FILTER, LS_BRIDGE_SINK };
enum LsBridgeOwner : uint32_t { LS_OWNER_DLSSFG=1, LS_OWNER_SMOOTH=2, LS_OWNER_HDR=3 };
// One context for each actual DXGI submission. A generator's intermediates
// each receive their own filter/sink context before the outer real frame.
// replacement is borrowed until the after callbacks finish. No COM ownership
// crosses the addon ABI. A sink must capture its input BEFORE DXGI rotates it.
struct LsBridgeFrame {
    uint32_t size=sizeof(LsBridgeFrame);
    BOOL generated=FALSE, present1=FALSE, externalPresented=FALSE;
    ID3D11Texture2D* replacement=nullptr;
    uint32_t replacementColorSpace=0;
};
using LsBridgeBefore=HRESULT(WINAPI*)(IDXGISwapChain*,UINT*,UINT*,BOOL*,LsBridgeFrame*,void*);
using LsBridgeAfter=void(WINAPI*)(IDXGISwapChain*,HRESULT,LsBridgeFrame*,void*);
using LsBridgeLog=void(WINAPI*)(const char*,void*);
struct LsBridgeCallbacks {
    uint32_t size=sizeof(LsBridgeCallbacks), owner=0;
    LsBridgeKind kind=LS_BRIDGE_GENERATOR;
    LsBridgeBefore before=nullptr;
    LsBridgeAfter after=nullptr;
    void* user=nullptr;
};
// CPU timestamps around the underlying DXGI call, after filters have run.
// This is submission timing, not a measurement of physical monitor refresh.
// Kept separate from the original callback structs to preserve their ABI.
struct LsBridgePresentTiming {
    uint32_t size=sizeof(LsBridgePresentTiming);
    BOOL generated=FALSE, present1=FALSE;
    UINT sync=0,flags=0;
    HRESULT result=E_FAIL;
    uint64_t sequence=0;
    int64_t beginQpc=0,endQpc=0,frequency=0;
};
LS_BRIDGE_API uint32_t WINAPI LsBridgeVersion();
// Only the last completed call on this thread and this chain is returned.
LS_BRIDGE_API BOOL WINAPI LsBridgeGetPresentTiming(IDXGISwapChain*,LsBridgePresentTiming*);
LS_BRIDGE_API BOOL WINAPI LsBridgeRegister(const LsBridgeCallbacks*);
LS_BRIDGE_API BOOL WINAPI LsBridgeUnregister(uint32_t owner);
LS_BRIDGE_API BOOL WINAPI LsBridgeInstall(IDXGISwapChain*,LsBridgeLog=nullptr,void* user=nullptr);
LS_BRIDGE_API HRESULT WINAPI LsBridgePresent(IDXGISwapChain*,UINT sync,UINT flags,uint32_t api=0);
LS_BRIDGE_API BOOL WINAPI LsBridgeUsesPresent1();
LS_BRIDGE_API uint32_t WINAPI LsBridgeHits();
