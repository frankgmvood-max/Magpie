#pragma once
#include <d3d11_4.h>
#include <memory>
#include <string>
#include <functional>
#include "settings.h"

namespace fg {
using Log = std::function<void(const char*)>;
enum class Result { Ready, HistoryOnly, Duplicate, Failed };
class Backend {
public:
    Backend();
    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    bool Init(ID3D11Device*, const D3D11_TEXTURE2D_DESC&, const std::wstring& runtime, const Settings&, Log);
    Result Generate(ID3D11Texture2D* input, bool reset);
    // Borrowed, valid until destruction or next Generate.
    ID3D11Texture2D* Output(unsigned index=0) const;
    unsigned OutputCount() const;
    unsigned Multiplier() const;
    unsigned MaxMultiplier() const;
    unsigned FlowQuality() const;
    bool RealMotion() const;
    double PreprocessMilliseconds() const;
private:
    struct State;
    std::unique_ptr<State> s_;
};
}
