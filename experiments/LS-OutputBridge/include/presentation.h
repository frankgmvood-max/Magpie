// SPDX-License-Identifier: MIT
#pragma once
#include <dxgi1_6.h>
#include <wrl/client.h>
namespace ls {
using Microsoft::WRL::ComPtr;
inline bool TearingSupported(IDXGIFactory2* factory){
    Microsoft::WRL::ComPtr<IDXGIFactory5> f;BOOL supported=FALSE;
    return factory && SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f))) && SUCCEEDED(f->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&supported,sizeof supported)) && supported;
}
inline bool SameLuid(LUID a,LUID b){return a.HighPart==b.HighPart && a.LowPart==b.LowPart;}
inline bool LegalPresent(UINT flags){return !(flags & ~UINT(DXGI_PRESENT_ALLOW_TEARING));}
inline UINT PresentFlags(UINT sync,bool tearingChain){return sync==0 && tearingChain?DXGI_PRESENT_ALLOW_TEARING:0;}
}
