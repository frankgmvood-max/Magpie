#include "output_state.h"
#include <nvapi.h>
#include <nvapi_interface.h>
#include <cstring>

namespace fg {
bool OutputQueue::Set(IDXGISwapChain* chain,unsigned limit) {
    Reset();if(!chain || limit<1 || limit>16) return false;
    DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(chain->GetDesc(&desc))) return false;
    HRESULT hr=E_FAIL;
    if(desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        if(FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain_))) ||
           FAILED(chain_->GetMaximumFrameLatency(&previous_))) {Reset();return false;}
        hr=chain_->SetMaximumFrameLatency(limit);
    } else {
        // Non-waitable chains (including the user's flags=0x800) require the
        // device API. Swapchain2::SetMaximumFrameLatency is invalid there.
        if(FAILED(chain->GetDevice(IID_PPV_ARGS(&device_))) ||
           FAILED(device_->GetMaximumFrameLatency(&previous_))) {Reset();return false;}
        hr=device_->SetMaximumFrameLatency(limit);
    }
    if(FAILED(hr)) {Reset();return false;}
    applied_=limit;return true;
}
void OutputQueue::Reset() {
    // Do not overwrite a new LS/user queue setting applied after ours.
    UINT current=0;
    if(applied_ && chain_ && SUCCEEDED(chain_->GetMaximumFrameLatency(&current)) && current==applied_)
        chain_->SetMaximumFrameLatency(previous_);
    if(applied_ && device_ && SUCCEEDED(device_->GetMaximumFrameLatency(&current)) && current==applied_)
        device_->SetMaximumFrameLatency(previous_);
    chain_.Reset();device_.Reset();previous_=applied_=0;
}
namespace {
using QueryInterface=void* (__cdecl*)(unsigned);
void* Resolve(QueryInterface query,const char* name) {
    for(const auto& entry:nvapi_interface_table)
        if(std::strcmp(entry.func,name)==0) return query(entry.id);
    return nullptr;
}
}
void GSyncProbe::Init(ID3D11Device* device,Log log) {
    Reset();if(!device) return;
    // Load only NVIDIA's system driver DLL, never an addon or game directory.
    module_=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module_) {log("G-SYNC probe: system NVAPI unavailable");return;}
    const auto query=reinterpret_cast<QueryInterface>(GetProcAddress(module_,"nvapi_QueryInterface"));
    if(!query) {log("G-SYNC probe: NVAPI query unavailable");Reset();return;}
    initialize_=Resolve(query,"NvAPI_Initialize");unload_=Resolve(query,"NvAPI_Unload");
    object_=Resolve(query,"NvAPI_D3D_GetObjectHandleForResource");
    capable_=Resolve(query,"NvAPI_D3D_IsGSyncCapable");active_=Resolve(query,"NvAPI_D3D_IsGSyncActive");
    if(!initialize_ || !unload_ || !object_ || !capable_ || !active_ ||
       reinterpret_cast<decltype(&NvAPI_Initialize)>(initialize_)()!=NVAPI_OK) {
        log("G-SYNC probe: driver interfaces unavailable");Reset();return;
    }
    initialized_=true;device_=device;log("G-SYNC probe: read-only driver queries ready");
}
void GSyncProbe::Reset() {
    device_.Reset();if(initialized_ && unload_) reinterpret_cast<decltype(&NvAPI_Unload)>(unload_)();
    initialized_=false;if(module_) FreeLibrary(module_);module_=nullptr;
    initialize_=unload_=object_=capable_=active_=nullptr;
}
GSyncState GSyncProbe::Query(ID3D11Resource* surface) const {
    GSyncState state;if(!initialized_ || !device_ || !surface) return state;
    NVDX_ObjectHandle handle=NVDX_OBJECT_NONE;
    state.handleStatus=reinterpret_cast<decltype(&NvAPI_D3D_GetObjectHandleForResource)>(object_)(device_.Get(),surface,&handle);
    if(state.handleStatus!=NVAPI_OK || handle==NVDX_OBJECT_NONE) return state;
    BOOL capable=FALSE,active=FALSE;
    state.capableStatus=reinterpret_cast<decltype(&NvAPI_D3D_IsGSyncCapable)>(capable_)(device_.Get(),handle,&capable);
    state.activeStatus=reinterpret_cast<decltype(&NvAPI_D3D_IsGSyncActive)>(active_)(device_.Get(),handle,&active);
    state.capable=state.capableStatus==NVAPI_OK && capable;
    state.active=state.activeStatus==NVAPI_OK && active;
    return state;
}
}
