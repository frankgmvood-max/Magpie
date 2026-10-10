# Native context restart recovery, 0.1.4

## Evidence from the user's 0.1.2 logs

All four logs use the eligible LSFG3 Fixed x2 SDR profile and show successful
backend initialization, NVIDIA submissions, conditional composites and enabled
disable-flag samples. No hook installation failures or disabled-flag samples
are recorded. Samples are diagnostic observations, not presented-frame counts.

| Log process | Attached devices | Submissions | Queued composites | Final session_limit |
| --- | ---: | ---: | ---: | ---: |
| 1960 | 3 | 2,851 | 2,845 | 0 |
| 18796 | 2 | 2,782 | 2,778 | 0 |
| 23972 | 2 | 7,275 | 7,271 | 0 |
| 8972 | 10 | 6,522 | 6,513 | 10,289 |

The last log first reports a nonzero limit around uptime 380,031 ms. Four
retained sessions (contexts 2, 14, 26, 38) keep reporting `ready`, but their
submission counters stop increasing. Native source updates and device creation
continue. The controller allocated four graphs for the lifetime of the process,
never reused them, and skipped backend calls for every new context afterwards.
The original LS shader still executes, so this is a concrete fallback to native
LS interpolation. It is not an NGX disable flag or a reported NVIDIA failure.

The shorter logs still submit/composite across two or three contexts. Therefore
the user's disappearing source-FPS ceiling after minimizing/restarting cannot
by itself establish a switch from DLSS to LS. These logs have no source-FPS,
Present interval or refresh/VRR telemetry. The backend implements no limiter,
Reflex, swapchain or Present. The pacing cause remains unresolved.

## Reattachment

The controller keeps the four-graph memory bound. A graph with the same source
extent/format and adapter LUID becomes a reattachment candidate after at least
one second without source callbacks. Active contexts cannot be stolen merely
because their GPU queue happens to be idle. Incompatible or failed graphs stay
retained; different resolutions/adapters can still exhaust the bound.

On the new native context's owning thread, `Reattach` uses a try-lock and checks
the completed D3D12 fence values for producer copies, generation, native
composite retirement, conversion, optical flow and NVOF registration. Pending
work returns `S_FALSE`; removed devices are rejected. It performs no CPU fence
wait, Flush or Present, and never calls the previous D3D11 device/context.
Composite retirement now also has a D3D12 fence mirror.

Shared NT resource/fence handles are retained once when the graph is created.
The new same-adapter D3D11 device opens those handles, creates its SRVs and
composite shader, and commits the new binding only after every operation
succeeds. An import failure leaves the previous graph binding intact and is
retried at most once per 250 ms for that candidate/context. The D3D12 device,
NGX feature, NVOF session/registrations and private resources are retained.
Fence values, NGX frame IDs and cumulative counters remain monotonic. Source
history/job metadata reset, so the first pair is warmup/native before ordinary
conditional replacement resumes. Unpolled old diagnostic flag samples may be
discarded at reattachment; no CPU readback is added to the native frame path.

The periodic log adds the latest observed source context, selected context,
selection reason, source age, reattachment counts and last reattachment result.
`session_available` describes routing only; eligibility, GPU readiness, pair
matching, warmup and the actual NVIDIA disable flag still determine replacement.
A backend snapshot sampled across a reattachment is marked stale rather than
attributed to the wrong context. Graph/state addresses remain process-lifetime
pinned, preserving callback/logger lifetime guarantees without NGX teardown.

## Validation scope

Windows WARP tests exercise nine successive new D3D11 devices against one graph:
shared imports, cumulative counters, incompatible-extent rejection with the old
binding still usable, rejection of old-device inputs, history reset, restored
native bindings and actual midpoint pixels. A held native GPU queue must reject
reattachment without waiting. The real observer-hook test repeats nine device
restarts with exact-pair detection and pixel readback, verifies that only one
graph is retained and that `session_limit` does not grow.

These tests validate resource transport and controller recovery. They do not
execute NVIDIA NGX/NVOF on RTX 3080, establish VRR, or explain source-FPS pacing.
User validation should keep the working SM86/runtime unchanged, restart FG more
than four times, and correlate `routing` plus advancing active-session counters
with the observed FPS ceiling before/after minimizing.

Primary API reference: [Microsoft OpenSharedResource1](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-opensharedresource1).
