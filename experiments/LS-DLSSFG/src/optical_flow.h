// SPDX-License-Identifier: MIT
#pragma once
#include "gpu_helpers.h"
#include "backend.h"
namespace fg {
class OpticalFlow {
public:
    OpticalFlow();
    ~OpticalFlow();
    bool Init(ID3D11Device*,const D3D11_TEXTURE2D_DESC&,unsigned quality,Log,unsigned analysisScale=50);
    // First/reset frames return zero motion; failures are reported to the caller.
    bool Process(ID3D11Texture2D* input,bool reset,bool& realMotion);
    ID3D11Texture2D* Motion() const;
    unsigned Quality() const;
private:
    struct State;
    std::unique_ptr<State> s_;
};
}
