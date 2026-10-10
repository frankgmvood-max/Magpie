#pragma once
#include "../include/source_pair.h"
#include <d3d11_4.h>
#include <filesystem>
#include <memory>

namespace ls_native {
struct BackendOptions {
    bool enabled = false, optical_flow = true, gpu_ordered = true;
    uint32_t quality = 2, analysis_percent = 50, slots = 3;
    float motion_scale = 1.0f;
#ifdef LS_NATIVE_TEST
    bool synthetic_test = false, test_disable = false;
#endif
};
struct BackendCounters {
    uint64_t submitted = 0, composite_queued = 0, busy = 0, mismatched = 0;
    uint64_t gpu_disabled = 0, gpu_enabled = 0, failed = 0;
    uint64_t ineligible = 0, destination_rejected = 0, warmup = 0;
};
struct BackendDiagnostics {
    BackendCounters counters;
    const char* step = "not initialized";
    uint32_t code = 0, analysis_width = 0, analysis_height = 0, grid = 0, analysis_format = 0;
};
// Initial experimental Fixed x2 SDR path. LS still executes its original
// synthesis. GPU-side conditional stores replace only a matched midpoint and
// leave native pixels untouched when NGX's actual disable flag is set.
// No CPU fence waits, Flush, Present, swapchain or limiter exist in this class.
class NativeBackend {
public:
    NativeBackend();
    ~NativeBackend();
    HRESULT Initialize(ID3D11DeviceContext* context, const TextureObservation& source,
                       const BackendOptions& options, const std::filesystem::path& folder);
    bool Source(ID3D11Texture2D* current, const SourceWriteObservation& write);
    bool Composite(const DispatchObservation& slot, ID3D11UnorderedAccessView* destination);
    // Worker only. Reads four bytes only after a completed D3D12 fence.
    BackendCounters PollCounters();
    // Worker only. Never competes with a frame callback by waiting for its lock.
    bool TryPollDiagnostics(BackendDiagnostics& diagnostics);
    const char* FailureStep() const;
    uint32_t ErrorCode() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ls_native
