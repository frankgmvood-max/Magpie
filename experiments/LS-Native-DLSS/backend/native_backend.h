#pragma once
#include "../include/source_pair.h"
#include "../include/render_policy.h"
#include <d3d11_4.h>
#include <filesystem>
#include <memory>

namespace ls_native {
struct BackendOptions {
    bool enabled = false, optical_flow = true, gpu_ordered = true;
    uint32_t quality = 2, analysis_percent = 50, slots = 3;
    uint32_t flow_preset = 0, flow_grid = 0; // 0 migrates legacy Quality.
    float motion_scale = 1.0f;
#ifdef LS_NATIVE_TEST
    bool synthetic_test = false, test_disable = false;
#endif
};
struct BackendCounters {
    uint64_t submitted = 0, composite_queued = 0, busy = 0, mismatched = 0;
    uint64_t gpu_disabled = 0, gpu_enabled = 0, failed = 0;
    uint64_t ineligible = 0, destination_rejected = 0, warmup = 0;
    uint64_t repeat_queued = 0, duplicate_samples = 0, scene_cut_samples = 0, scene_resets = 0;
};
struct NgxValue {
    uint32_t result = 0;
    int value = 0;
};
struct NgxDiagnostics {
    uint32_t init = 0, parameters = 0;
    NgxValue available, feature_init, needs_driver, min_driver_major, min_driver_minor;
};
struct NgxLogMessage {
    uint32_t level = 0, feature = 0;
    bool truncated = false;
    std::array<char, 1024> text{};
};
struct BackendDiagnostics {
    BackendCounters counters;
    NgxDiagnostics ngx;
    const char* step = "not initialized";
    uint32_t code = 0, analysis_width = 0, analysis_height = 0, grid = 0, analysis_format = 0;
    uint64_t reattachments = 0;
    uint32_t reattach_code = 0;
    uint64_t adapter_luid = 0, timing_samples = 0;
    double conversion_ms = 0, flow_dependency_ms = 0, generation_ms = 0;
    std::array<wchar_t, 128> adapter_name{};
    RenderMode mode = RenderMode::Hybrid;
};
// Fixed x2 SDR backend. Hybrid conditionally replaces a native midpoint;
// economy fills the midpoint with DLSS or the current original source.
// Exact pair, geometry, device and GPU disable gates remain mandatory.
// No CPU fence waits, Flush, Present, swapchain or limiter exist in this class.
class NativeBackend {
public:
    NativeBackend();
    ~NativeBackend();
    HRESULT Initialize(ID3D11DeviceContext* context, const TextureObservation& source,
                       const BackendOptions& options, const std::filesystem::path& folder);
    // Native owning thread only, after the controller has observed inactivity.
    // Reopens the retained graph on a same-adapter, same-extent D3D11 device.
    // S_FALSE means GPU work/another callback has not retired; never CPU-waits.
    // Failure leaves the previous binding and its GPU resources intact.
    HRESULT Reattach(ID3D11DeviceContext* context, const TextureObservation& source);
    bool Source(ID3D11Texture2D* current, const SourceWriteObservation& write);
    bool Composite(const DispatchObservation& slot, ID3D11UnorderedAccessView* destination);
    // Owning thread only. Economy and recovery always fill a valid destination,
    // even without a matched generated job, by repeating the current native SRV.
    bool Composite(const DispatchObservation& slot, ID3D11UnorderedAccessView* destination, bool repeat);
    bool SetPolicy(const RenderPolicy& policy);
    // Diagnostic/status metadata only, after completed D3D12 fences. Source also
    // polls retired metadata before reusing a job so scene resets cannot be lost.
    BackendCounters PollCounters();
    // Worker only. Never competes with a frame callback by waiting for its lock.
    bool TryPollDiagnostics(BackendDiagnostics& diagnostics);
    // Bounded, process-wide driver callback queue. Consumer only performs I/O.
    static bool TryPopNgxLog(NgxLogMessage& message);
    static uint64_t DroppedNgxLogs();
#ifdef LS_NATIVE_TEST
    static void TestNgxLog(const char* message);
#endif
    const char* FailureStep() const;
    uint32_t ErrorCode() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ls_native
