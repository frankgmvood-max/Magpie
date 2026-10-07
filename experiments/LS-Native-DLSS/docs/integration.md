# Adapter design and acceptance gates

## Ownership

LS remains the sole owner of the swap chain, capture cadence, output queue and
Present. No replacement swap chain, additional Present, limiter/sleep inside
OnPresent, wait on a CPU fence in a dispatch callback, or driver FG presenter.
The old LS-DLSSFG addon/OutputBridge do not satisfy this ownership model. Reuse
preprocessing/NGX code selectively; do not run the old addon concurrently.

The addon manager could be a temporary observation/bootstrap mechanism. It is
not architecturally required by the replacement backend; a standalone proxy
must forward the exact native ABI and settings before it can replace the manager.
Manager SDK 1.2 provides pre/post Dispatch hooks, not authoritative pair IDs,
interpolation phase or a native generated-frame provider. Its shader-intercepted
event is reserved and not sent. Do not assume it supplies a ready integration API.

## Initial implementation sequence

1. Read-only observation keyed to full native-DLL and shader SHA256. Keep native
   generation active. Build a proven source/phase/output mapping from actual commands.
2. A no-op provider supplies exactly the original generated image into the same
   slot; compare image bytes and displayed cadence. This is the preservation baseline.
3. Run the compatible SM86 NGX backend in parallel on the output RTX 3080, on a
   separate context/queue. Compare its result without replacing the native image yet.
4. Enable ready-only replacement in the verified Fixed ×2 SDR synthesis slot.
   If the GPU result for the exact slot is not complete, execute native synthesis.
5. Only after stable image and output tests, consider avoiding native analysis
   work or reusing LS motion. Those changes affect dependencies of later passes.

This sequence intentionally keeps native work at first, so its extra GPU cost
must be measured. It is not a promise of a free or artifact-free improvement.
Frequent ready/not-ready switching needs a session fallback/cooldown policy.

## Resource and time contract

Tag a job with device epoch, output-adapter LUID, exact previous/current source
IDs, native slot sequence, dimensions, DXGI format, color space and decoded phase.
Do not derive source IDs or phase from backbuffer changes or Present counters.
Retain original inputs until all GPU consumers have retired. Retain replacement
output until the native-context copy has retired, not only until NGX evaluation ends.

D3D11/D3D12 shared resources require a validated format and state transition,
explicit fence ordering and separate leases. A completed D3D12 fence is checked
without waiting. Restore every affected D3D11 binding and handle the currently
bound output UAV before copying; submit the copy before native consumption.
The initial policy in `include/native_slot_policy.h` checks metadata only. It
does not implement resource transfer or prove these lifetimes in the driver.

Feature/device recreation is performed outside native rendering callbacks.
Restart, minimize/restore, resize, color-space or adapter change increments the
epoch and drains/invalidates old jobs. Never copy a previous epoch into a new slot.
No CPU Readback/Map, formatted logging, allocation, or resource discovery should
remain in the shipping synthesis callback. Observation overhead is measured separately.

## DLSS-specific limits

- Keep the known compatible RTX 3080 SM86 runtime; no stock-support assumption.
- Color-only reconstruction still lacks real game depth, motion and camera data.
  Synthetic depth/identity camera are explicit experimental compromises.
- Verify motion-vector direction, full-resolution pixels versus normalized units,
  scale and NGX disable-interpolation flag on controlled translations first.
- Initial phase is exactly 0.5, Fixed ×2. Do not map arbitrary LS Adaptive phases
  to NGX multi-frame indices without proving the runtime's supported contract.
- Flow quality is chosen only after identical-frame quality and GPU-time tests.
  Generating and retaining LS Flow does not establish its suitability for NGX.

## Acceptance

Compare native and candidate using identical source frames and settings. Include
fast turns, disocclusion, HUD/text, camera cuts, particles, static frames and
minimize/restore loops. Compare artifacts at original output resolution, not only PSNR.
Record GPU generation and copy time; input-to-display latency; displayed real and
generated frame order, missed slots, repeats and p95/p99 intervals over equal runs.
Use PresentMon/ETW plus physical monitor evidence for VRR. CPU Present spacing,
G-SYNC indicator and DXGI composition labels do not prove variable physical refresh.
The two-GPU topology is part of every acceptance run. GitHub CI cannot test it.
