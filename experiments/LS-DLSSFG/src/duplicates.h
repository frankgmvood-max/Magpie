// SPDX-License-Identifier: MIT
#pragma once
#include "gpu_helpers.h"
namespace fg {
enum class DuplicateResult { NewFrame, Duplicate, Failed };
// Exact RGB equality at the addon input, not a sampled hash or fuzzy threshold.
// The supplied context must be addon-owned, never LS's immediate context.
class DuplicateFilter {
public:
    bool Init(ID3D11Device*,const D3D11_TEXTURE2D_DESC&);
    DuplicateResult Check(ID3D11DeviceContext*,ID3D11Texture2D*);
    void Reset() { valid_=false; }
private:
    Ptr<ID3D11Texture2D> previous_;
    Ptr<ID3D11ShaderResourceView> previousView_;
    Ptr<ID3D11ComputeShader> shader_;
    Ptr<ID3D11Buffer> result_,readback_;
    Ptr<ID3D11UnorderedAccessView> resultView_;
    Ptr<ID3D11Query> completion_;
    Ptr<ID3D11Device> device_;
    bool valid_=false;
    unsigned width_=0,height_=0;
};
}
