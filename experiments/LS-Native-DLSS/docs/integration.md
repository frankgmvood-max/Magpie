# Adapter design and acceptance gates

## Ownership

LS remains the sole owner of the swap chain, capture cadence, output queue and
Present. No replacement swap chain, additional Present, limiter/sleep inside
OnPresent, wait on a CPU fence in a dispatch callback, or driver FG presenter.
The old LS-DLSSFG addon/OutputBridge do not satisfy this ownership model. Reuse
preprocessing/NGX code selectively; do not run the old addon concurrently.

The target excludes the addon manager. `observer/` now implements the standalone
Lossless.dll proxy and does not build against the manager or its SDK. It forwards
the native ABI and observes selected compute commands. The experimental backend
conditionally replaces final synthesis pixels after the original Dispatch;
native analysis and synthesis still run as a fallback. No addon is required.
Manager SDK 1.2 provides pre/post Dispatch hooks, not authoritative pair IDs,
interpolation phase or a native generated-frame provider. Its shader-intercepted
event is reserved and not sent. Do not assume it supplies a ready integration API.
The manager source was used to check ABI conventions only. The standalone
observer uses the actual intercepted context and its device, not a newest-device
global. Its full DLL/shader hash gates and limitations are documented separately.

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
The separate `bridge/` now implements owned input snapshots and output-copy
retirement for isolated Windows tests, without attaching either component to LS.
It creates shared textures in D3D11, imports them into the backend device on the
actual output adapter, and uses distinct producer/reader/output/native-copy
signals. See [input-snapshots.md](input-snapshots.md); this transport does not
make an unverified source pair eligible for replacement.

Earlier design goal: feature/device recreation outside rendering callbacks.
Implementation 0.1 initializes a bounded preallocated session on the first
source callback on the owning native thread, to respect SINGLETHREADED D3D11
devices. This may cause a startup hitch, but not recurring allocation per frame.
Restart, minimize/restore, resize, color-space or adapter change increments the
epoch and drains/invalidates old jobs. Never copy a previous epoch into a new slot.
No CPU Readback/Map, formatted logging or allocation remains in the synthesis
callback. Current bindings are still inspected to reject incompatible slots.
The worker maps only a completed four-byte private D3D12 flag for sampled stats.

## Implemented experimental path (0.1.0)

The portable ready-only policy remains a separate conservative contract, not
a verified production contract. The backend has its own hash/profile/exact-pair
gates. Real LS semantic acceptance, full external-write coverage, image quality
and physical VRR remain unverified.

Default GPUOrdered=1 inserts an immediate-context GPU dependency after LS's
original synthesis. GPU fence ordering replaces CPU "is ready" checking because
the native D3D11 stream may not submit the producer signal until a later Present.
GPUOrdered=0 retains the already-ready-only experiment.

Dependencies: native source update → owned copies → producer P; backend queue
waits P → analysis conversion → C; NVOF waits C → flow F; backend waits F → NGX
and actual flag → ready R; native queue waits R → conditional stores → retirement.
The graph is acyclic. Shared images return to COMMON for cross-API handoff.
A job is reusable only after backend ready AND native-store retirement.
GPU latency can still grow despite avoiding CPU stalls.

Both source images are privately snapshotted. Stateful NGX gets the current
backbuffer of each consecutive successful pair. A skipped evaluation/generation,
profile change, epoch change or >250 ms input pause triggers reset/warmup. Only
exact phase 0.5 in LSFG3 Fixed x2 SDR and same device/LUID can be replaced.
Original/endpoint frames, unmatched history and disabled flags stay native.

Four session graphs maximum remain pinned until process exit. No hot shutdown,
driver-module unload or blocking resource drain occurs during native profile
teardown. Restart LS after many device/context/extent changes or changing INI.

The WARP synthetic provider compiles only in test executables. Tests cover GPU
pixel transport, actual disable flag path, state restoration and end-to-end real
Dispatch detours. They do not test the NVIDIA runtime or LS itself.

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
