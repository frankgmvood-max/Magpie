# Standalone native observer (stage 1)

The target architecture replaces native optical flow and synthesis with NVIDIA
work while LS retains scaling, capture, profiles, cursor handling, HDR behaviour,
its output window, queue and every Present. The addon manager is excluded.

This commit implements **observation**, not DLSS/NVOF replacement. It must not
be described as a working quality or VRR fix. Native synthesis still runs exactly
once for every intercepted Dispatch. No texture contents are changed.

## Bootstrap and compatibility

- x64 `Lossless.dll`: ten PE forwarders to `Lossless_original.dll`, preserving
  names and ordinals. Only ApplySettings is wrapped, with all 32 native arguments
  passed unchanged. Init, UnInit and native settings/driver functions remain LS's.
- DllMain only stores the module handle and disables thread notifications.
  First ApplySettings resolves the sibling native library; optional observation
  starts outside loader lock. If no ApplySettings call occurs, observation does
  not start. Wrong/missing observation config leaves the ordinary proxy active.
- Observation requires the full provided native DLL's SHA256 and both shaders 254/256's
  exact SHA256. A mismatch leaves native generation running. This is not an ABI
  promise for future LS versions: revalidate the original DLL and settings ABI.
- No manager window, addon discovery, SDK, resource replacement callback, or old
  OutputBridge. MinHook 1.3.4 uses the fork's existing BSD-2-Clause sources.
- The native module's D3D11CreateDevice delay import is intercepted. Successfully
  created LS devices receive independent IDs and actual DXGI adapter LUIDs.
  CreateComputeShader tags only an exact matching bytecode payload; CreateBuffer
  snapshots immutable/dynamic constant buffers with initial CPU data, 32..64 bytes.
- Native immediate-context Dispatch is intercepted at the runtime entry points.
  WARP probes discover regular/single-threaded and protected/unprotected refresh
  entries before native capture starts. Nested runtime calls are counted once.
  Deferred-context synthesis is unsupported and reported as untagged.

Only the observer's private COM metadata GUIDs and import/detour bindings are
changed. Shader bytes, texture pixels and native CS bindings are not changed.
Private-data calls and metadata queries still have CPU overhead. Benchmarking
native frame pacing with observation enabled is not a shipping acceptance run.

## Trace contents and limitations

Schema 2 records input-update 254 submissions after native Dispatch and synthesis
256 bindings before Dispatch. The sequence counter covers **both** event types.
The analyzer remains compatible with schema 1 synthesis-only captures. Records contain:

- QPC/thread/context/device IDs, adapter LUID, Dispatch group counts;
- b0 subrange, captured allocation size, phase bits and resolution-scale bits;
- five input texture IDs and output UAV texture ID, resource/view formats,
  dimensions, mip, samples and array size;
- input update epochs/generations and the last two per-context submissions;
- whether synthesis t0/t1 match those observed writes, including actual texture,
  device/context/thread identity, extent and command-list epoch;
- header identities for both kernels and footer counters, including lost records
  and hook/tag failures. Generations are submission versions, not game frame IDs.

No observer-initiated GPU Map/readback, additional Copy, wait, Flush, sleep/limiter
or Present is executed from the rendering callback. Native Map/Copy/update calls
are forwarded unchanged; their destination snapshots are invalidated beforehand. The observer's bounded queue uses try-lock;
it drops records instead of blocking if full or contended. Formatting and disk
I/O run on a separate writer thread. The writer stops after the configured time
or maximum identified records (input updates plus synthesis slots). Disabling observation takes effect on next launch.

Executable detours/trampolines and state are pinned for process lifetime.
The worker stops and closes the trace outside DllMain; hooks then only forward.
No unsafe FreeLibrary/hot-unload or trampoline removal is attempted. Exit before
the footer can produce an interrupted trace; the analyzer rejects it as complete
evidence. Configuration or log failures never activate a replacement backend.

**Texture object IDs are not source frame IDs.** Reused textures may contain new
image data, and externally shared textures can change without an LS write.
A source texture's private stamp changes at each observed native input update.
Consecutive updates can be associated with the statically mapped previous/current
bindings even when an object is reused. Map/Copy/update destinations, unknown
compute writes to the first eight UAV slots, missed records and deferred execution
invalidate continuity. Same-object consecutive updates, extent changes, thread
changes and new sessions cannot establish a pair. A repeated synthesis slot can
use the same pair; synthesis slots are not mistaken for new input images.

**Mutation coverage is incomplete.** Draw/Clear/Resolve/Discard, higher UAV slots
and external writes are not all intercepted. Therefore headers/reports explicitly
keep `source_mutation_coverage_complete` and `source_content_verified` false.
`pair_matches_observed_updates` is only agreement with observed submissions;
it never authorizes DLSS replacement or proves the pixels stayed unchanged.
The trace establishes bindings and phase observations, not verified pair pixels,
source capture IDs, GPU retirement, motion units/direction, SDR/HDR semantics,
displayed frame order or physical VRR. Those remain separate acceptance gates.
The game GPU exists in another process; observing one output adapter here does
not prove or disprove the user's two-GPU topology.

## Build and tests

Developer shell on Windows with CMake, VS/Windows SDK:

```bat
cmake -S experiments\LS-Native-DLSS\observer -B build\native-observer -A x64
cmake --build build\native-observer --config Release --parallel
ctest --test-dir build\native-observer -C Release --output-on-failure
```

`abi-fixture/` contains a **fake** original DLL exclusively for automated tests.
It is not LS; never put it in an LS installation. The fixture independently
declares the ABI, verifies all 32 differently typed settings arguments, checks
ten export forwarders, and requests observation with a deliberately unsupported
DLL hash to exercise normal pass-through after a failed observation gate.
Assertion-based tests explicitly undefine NDEBUG in Release builds.

Local Linux validation: the real supplied DLL metadata match, Python rejection
tests, native C++ queue/slot tests, MinGW Windows x64 compilation with warnings as
errors, and PE export verification. Cross compilation is not a Windows runtime
test; CI does not contain the proprietary DLL or RTX 3080 runtime.

Windows MSVC/WARP validation subsequently passed in
[run 37662866543](https://github.com/frankgmvood-max/Magpie/actions/runs/37662866543):
four tests including real CreateBuffer/Map/ExecuteCommandList detours on WARP,
the independent ABI fixture, and the native name/ordinal PE-table check. No real
LS, SM86 NGX, hardware image-quality test or physical VRR test was performed.

## Research installation, once Windows tests pass

Use a separate copy of the user's licensed LS installation, with LS stopped.
Preserve the manager proxy so it can be restored. Keep the **actual** native
Lossless_original.dll (expected hash); use only the built standalone Lossless.dll
and observer/NativeDLSS.ini beside the application. The addons folder need not
be moved or deleted: this proxy never loads it. Existing old addon/OutputBridge
code is not part of this path. Do not substitute fixture files.

Observation defaults off. To capture, set [Observation] Enabled=1, then launch
and activate a native Fixed x2 profile. DurationSeconds and MaxRecords bound the
capture (1..120 seconds, 1..65536 identified records). This stage still generates
with LSFG. Logs are `logs/native-observation-PID-QPC.jsonl`.

```sh
python experiments/LS-Native-DLSS/tools/analyze_trace.py /path/to/trace.jsonl --output local/trace-summary.json
```

The analysis has separate `observation_valid`, `replacement_verified`,
`source_frame_ids_verified` and `physical_vrr_verified` fields. Only the first
can become true from this stage; passing metadata is not permission to substitute
an NGX result. Next work: capture-content provenance and lifetime, a native
bit-identical no-op copy, then compatible SM86/NVOF shadow execution and measured
ready-only substitution at verified midpoint slots.

## Source-pair regression coverage

Portable tests check first-frame warm-up, order reversal, reused texture objects,
stale stamps, different device/context/thread, command-list epochs, dimensions,
missing generations and repeated synthesis slots. The Windows WARP test compiles
its **own** 8×8 input-copy and 16×16 interpolation kernels, exercises real
CreateComputeShader/Dispatch hooks, and checks post-submission stamps, pair
matching, CopyResource invalidation, reversed inputs and repeated-resource writes.
It contains no commercial shader bytecode, NVIDIA runtime or native LS execution.
The transport tests remain separate; this observer is not yet connected to their
GPU-copy pools. Passing WARP cannot certify physical VRR or native cadence.
