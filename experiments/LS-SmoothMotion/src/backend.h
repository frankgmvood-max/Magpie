// SPDX-License-Identifier: MIT
#pragma once
#include "settings.h"
#include <d3d11.h>
#include <dxgi1_4.h>
#include <memory>
#include <functional>
namespace sm {
struct DriverHooks {
    bool (*enable)(IDXGISwapChain*)=nullptr;
    uint64_t (*graphs)()=nullptr;
    uint64_t (*retargets)()=nullptr;
};
class Backend {
public:
    Backend();~Backend();
    bool Init(ID3D11Device*,HWND parent,const D3D11_TEXTURE2D_DESC&,const Settings&,bool plainChain,
              std::function<void(const char*)>,const DriverHooks* testHooks=nullptr);
    bool Prepare(ID3D11Texture2D*);
    HRESULT Present(UINT lsSync);
    bool Confirmed()const;
    bool Duplicate()const;
    bool TimedOut()const;
    bool Tearing()const;
    double Milliseconds()const;
    LUID AdapterLuid()const;
    void Hide();
#ifdef LS_SM_TEST
    IDXGISwapChain3* TestChain()const;
#endif
private:
    struct State;std::unique_ptr<State> state_;
};
}
