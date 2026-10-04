// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "settings.h"
#include "RtxVideoBridge.h"
#include <memory>
#include <string>
#include <functional>
namespace hdr {
struct Provider {RtxHdrCreateFn create=nullptr;RtxHdrDrawFn draw=nullptr;RtxHdrDestroyFn destroy=nullptr;};
class Backend {
public:
    Backend();~Backend();
    bool Init(ID3D11Device*,const D3D11_TEXTURE2D_DESC&,const std::wstring& directory,const Settings&,
        std::function<void(const char*)>,const Provider* testProvider=nullptr);
    bool Process(ID3D11Texture2D*,const Settings&,bool hdrOutput);
    ID3D11Texture2D* Output()const;
    double Milliseconds()const;
    bool Duplicate()const;
private:
    struct State;std::unique_ptr<State> state_;
};
}
