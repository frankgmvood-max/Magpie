#pragma once
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <functional>

namespace fg {
// A bounded output queue; no waitable handle ownership or HWND/focus changes.
class OutputQueue {
public:
    ~OutputQueue() { Reset(); }
    bool Set(IDXGISwapChain* chain,unsigned limit);
    void Reset();
    unsigned Previous() const {return previous_;}
    unsigned Applied() const {return applied_;}
    bool PerSwapChain() const {return bool(chain_);}
private:
    Microsoft::WRL::ComPtr<IDXGISwapChain2> chain_;
    Microsoft::WRL::ComPtr<IDXGIDevice1> device_;
    unsigned previous_=0,applied_=0;
};
struct GSyncState {
    int handleStatus=-3,capableStatus=-3,activeStatus=-3;
    bool capable=false,active=false;
    const char* Label() const {return activeStatus==0?(active?"active":"inactive"):"unknown";}
};
// Read-only driver telemetry. No DRS settings, Reflex sleeps or registry writes.
class GSyncProbe {
public:
    using Log=std::function<void(const char*)>;
    ~GSyncProbe() {Reset();}
    void Init(ID3D11Device* device,Log log);
    void Reset();
    GSyncState Query(ID3D11Resource* surface) const;
private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    HMODULE module_=nullptr;
    bool initialized_=false;
    void* initialize_=nullptr;
    void* unload_=nullptr;
    void* object_=nullptr;
    void* capable_=nullptr;
    void* active_=nullptr;
};
}
