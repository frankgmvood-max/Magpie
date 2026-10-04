# Implementation and VRR review — 0.2.1

References checked 2026-10-04. Hardware VRR success is still unverified.

## 0.2.1: measured processing cost and presentation gap

The supplied 0.2.0 log contains a long NVOF x2 session at 3440x1440:
208 telemetry samples, median generation_ms 8.449 and unique_interval_ms 21.075.
A later zero-motion x2 session has 56 samples, median generation_ms 3.569
and unique_interval_ms 17.132. These are sequential, differently timed sessions,
not a controlled benchmark of OF-only GPU utilization. preprocessing_cpu_ms
measures CPU submission/checks; it is not an NVOF GPU duration.

The long OF session reports G-SYNC active in 207/208 samples after its initial
query. This does not override the user's observed fixed refresh/tearing.
Driver status alone does not measure physical scanout frequency.

0.2.1 reduces the OF analysis image to 50% width/height by default, retaining
full-resolution LS colour, DLSSG output and dense motion. 25/50/75/100% are
selectable. Driver minimum extents are queried; motion displacement and grid
coordinates are scaled independently per axis, including odd-sized extents.
Analysis pixel count at 50% is one quarter; end-to-end speedup is unmeasured.

Driver queries, media statistics, regular status and metric publishing now
run after the outer real LS Present. They no longer occupy the generated-to-real
gap. NVIDIA states IsGSyncActive/IsGSyncCapable are reliable only after the first
completed Present and may take significant time:
https://docs.nvidia.com/nvapi/group__dx.html
Queries remain rate-limited to once every two seconds. Tests verify callback
order, one callback per x4 group, nesting, HRESULT propagation and bypasses.

DXGI media CompositionMode 1 is OVERLAY, not COMPOSED (0):
https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/ne-dxgi1_3-dxgi_frame_presentation_mode
Neither this enum nor successful ALLOW_TEARING establishes monitor VRR activity.
No monitor-Hz value is fabricated from CPU or submitted-frame counters.

* Magpie Experimental 0.6.9, 2fceab5e241bc9f8ded001ab3266762f1f8bc51e:
  https://github.com/SAOG0721/Magpie/tree/2fceab5e241bc9f8ded001ab3266762f1f8bc51e
  NvidiaOpticalFlowProvider.cpp, DLSSFrameGenerator.cpp, FrameSourceBase.cpp,
  shaders/DuplicateFrameCS.hlsl and DLSS_FrameGeneration.hlsl reviewed.
* NVIDIA NVOFA guide: https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html
  Formats/caps are queried. Current -> previous, S10.5 / 32, pixel coordinates,
  fixed five profile choices. Driver API lives on a private D3D11 context.
* NVIDIA NGX multi-frame evaluation declarations:
  https://github.com/NVIDIA/DLSS/blob/374959484e79a640feaba44c93ac8cfb0a03f5b5/include/nvsdk_ngx_params_dlssg.h
  MultiFrameCount is intermediate count, index is 1-based. Reset is 1/1;
  all evaluations of a group share one BackbufferFrameID. Runtime capability
  caps requests at x2-x4; absence of the cap is treated as x2.
* EAM 0.9.38: https://github.com/Echo-Storm/ls-addon-manager/tree/fbcc3c179e4052785c032d157d8eb785608eb28e
  Only LS's D3D11CreateDevice delay import is watched, not the addon's import.
  Private context work cannot enter EAM's LS dispatch callbacks. No native LS
  shader is skipped/replaced, no HWND, cursor, focus, capture or LS profile is
  rewritten. API 1.1 does not expose native LSFG state; explicit confirmation
  is required. GPU addon conflicts are kept in the manifest.
* VRR eligibility: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
  https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present
  Windowed flip chain, creation ALLOW_TEARING, sync=0, legal present flag.
  These are requests and do not establish G-SYNC activity. Existing nontearing
  chains cannot be converted by adding flags to Present/ResizeBuffers.
* Composition: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model
  Independent Flip/MPO is decided by OS/driver/window coverage/overlays.
  The addon preserves the one LS swapchain/window, full-frame Present1 contract,
  LS input, format, and outer real Present. Partial/TEST/nonblocking calls bypass.
* LS developer notes: https://steamcommunity.com/app/993090/allnews/?l=english
  Historical 24H2 WGC + hardware cursor + VRR without MPO and DXGI capture issues
  can affect composition/cadence. These are conditional blockers, not a diagnosis
  of this user's machine; native LSFG VRR working is the baseline control.

## Paths implemented

1. Follow LS Present API (default), explicit generated Present or Present1 as
   opt-in alternatives; outer LS entry point is preserved in every case.
2. Eligible tearing policy on generated and real frames, or keep LS policy.
3. Keep LS queue or reversible queue override; swapchain/device API selected
   from creation flags, waitable queue handle used only if it exists already.
4. Cadence uses unique RGB frames (when filtering), x2/x3/x4 spacing, reset after
   pause, no catchup burst; copy/restore the rotating buffer at every flip.
5. Rate-limited driver G-SYNC query after the outer real LS Present; failure is
   unknown. Composition/window and CPU timings are logged after presentation.

## Limits that cannot be resolved by a checkbox

No game engine depth/HUD/motion, no private engine timing/control from LS,
no second-GPU transfer, no forced Independent Flip, no hardware confirmation
on RTX 3080, no independent asynchronous LS presentation worker. Current
inference/pacing still runs inside LS's output callback and can backpressure it.
A worker that presents the host's swapchain concurrently with LS is unsafe:
LS can overwrite buffers or resize/free its output. It requires a separately
verified ownership/capture/presentation architecture, not an untested toggle.
No NVIDIA DRS/registry/MPO/driver settings, fullscreen mode or focus are forced.
A 136 FPS target cannot replace missing unique input frames or GPU throughput.
