# Owned input snapshots for the NVIDIA backend

`bridge/input_snapshots.*` is a standalone transport component. It is built as
a static library and exercised by Windows tests. It is **not linked into the
observer DLL or attached to LS yet**. It does not implement NVOF/NGX inference,
an output replacement, or a new presenter.

## Why this exists

An LS source texture can keep the same object identity while being overwritten
with a later image. `AddRef`, shader bindings, object IDs, and Present counts
cannot preserve its pixels or prove a source-frame pair. This module records an
ordered D3D11 copy into an owned texture, then protects that copy until the
D3D12 reader has finished. Its `SourceSubmission` describes a caller-observed
write submission; it is explicitly **not** a verified game-frame number.

The required future capture hook must establish source ownership, completed
external-producer synchronization, the full image extent, ordering of writes,
and capture timestamps. This module cannot infer these from a texture pointer.
In particular, capture from a externally shared texture without the producer's
ownership/synchronization protocol is not certified by making another copy.

## Contract

- Initialize outside a native render callback. Supply the actual LS immediate
  context, a D3D12 device on its adapter LUID, and a DIRECT queue belonging to
  that device. No selection of the first adapter or the game-rendering adapter.
- All pool calls must be serialized with the owner of that immediate context.
  The module does not acquire a render-thread lock or change multithread state.
- Preallocate 2–8 shared SDR textures. RGBA8/BGRA8 UNORM, one mip, one slice,
  one sample, matching dimensions and native source device only. Unsupported
  formats require a separate, explicitly validated conversion path.
  Create these allocations in D3D11 with SHARED/NTHANDLE and import them into
  D3D12. The first Windows run rejected opening a D3D12-created allocation in
  D3D11 with E_INVALIDARG; the creation direction is part of the compatibility
  contract, not a reason to skip the interop test.
- Supply monotonically increasing observed write-submission IDs across this
  context's source stream. Reusing a source object is valid; reusing a write ID
  is rejected. These IDs do not establish previous/current temporal order.
- Capture inserts `CopyResource` followed by a D3D11 shared-fence signal.
  It never calls native Flush. The existing host submission makes these commands
  progress; an idle host may delay them. No promise of meeting an LS slot deadline.
- A ticket includes the pool, strictly increasing session epoch, allocation
  serial, slot, source object/write IDs, and producer signal. A changed or consumed
  ticket cannot borrow a resource or submit another read.
- Record commands that read the declared ticket resources and leave them in
  COMMON state. Submit a closed DIRECT list using `SubmitReads`; do **not** submit
  it independently. All accessed snapshot leases must be listed. The command
  allocator, list, descriptors and other backend resources remain the caller's
  responsibility until the reader fence completes.
- `SubmitReads` enqueues GPU waits for every input, then executes the list and
  signals a separate reader fence. No CPU wait. Separate producer and consumer
  timelines avoid signalling a larger value while older work on another queue
  is still incomplete.
- NVOFA is an independent engine, not work certified by the D3D12 queue's own
  completion. Call `PinExternalReads` **before** dispatching an external reader;
  pass its separate, pending completion fence/value. That engine must wait for
  the shared producer fence and signal only after finishing all input reads.
  Combined D3D12 submission waits for both producer and external completion.
  Discard retains a pinned image until both complete. On an uncertain NVOF
  submission failure, never manufacture completion or recycle the leases.
  Registration handles must be unregistered outside callbacks before closing
  the pool; input leases do not own the NVIDIA registration/session lifecycle.
- A captured lease remains reserved even when its copy completes. A submitted
  lease becomes reusable only after reader completion; a discarded lease only
  after producer completion. Pool exhaustion returns Busy immediately.
- `Collect` and `CloseIfIdle` query completion without waiting. Reset/resize must
  drain leases and use a new epoch. Device/fence/API failure disables this pool;
  uncertain in-flight resource graphs are retained for process lifetime instead
  of being freed from a render callback or destructor. This is bounded per pool,
  but repeated failed pool creation is not a recovery policy and must not be
  enabled in the host integration.

## Return into the native output

`bridge/output_image.*` implements one preallocated output lease (the future
host owns a bounded set). Its shared D3D11 allocation supports UAV writes through
the imported D3D12 resource. It checks the actual adapter and queue, dimensions,
SDR format, session epoch and exact midpoint slot identity.

The backend list declares its input leases and is submitted through the same
queue as `InputSnapshots`. Input retirement, then output completion, have distinct
signals. The source pool must also belong to the same native device, session
epoch, extent and format; sharing a queue or adapter alone is insufficient.
The worker must publish the actual inference success and
disable-interpolation result after GPU completion. Unknown or disabled output is
not copied. `TryCopy` additionally applies the existing verified-slot policy and
checks the destination's native device, extent and format. It never waits or
flushes the native context and returns native fallback unless the exact image is
ready and authorized. One output can be submitted into a native slot only once.

The output remains reserved **after** backend completion, until the D3D11 copy
retirement fence completes. A backend result discarded before copying retires
on its backend fence instead. An unsubmitted reservation can be discarded
immediately. Shutdown/resize cannot free an active lease without draining it.

Pipeline bindings and native output-slot ownership still require a verified
LS hook; the transport does not discover or change bindings. The
`VerifiedContract` flags are supplied as trusted fixture values in the test;
that does not establish them for LS. Source-pair provenance, runtime disabled-flag
readback and controlled LS image/display tests remain prerequisites. Neither
transport component is attached to LS or calls NVIDIA inference yet.

## Windows test

The WARP test uses native D3D11 and D3D12 devices on the same software adapter,
shared NT texture handles and a shared producer fence. Two different patterns
are copied from **the same** source texture, which is then overwritten a third
time. A two-input readback must contain the first two patterns exactly, including
nonuniform pixels and padded readback row pitches. The test holds the D3D12 queue
behind a separate fence to verify no recycling while the reader is outstanding.
It also exercises duplicate writes/tickets, changed provenance, epoch reuse,
discard, source-device mismatch on the same LUID, and nonblocking teardown.
An independent WARP queue simulates the external-engine timeline: combined
consumption and discard must both retain inputs while its fence is incomplete.
This validates dependencies; it does not execute or certify NVOFA.

The no-op backend then copies a captured image in D3D12 into the owned output and
returns it into a D3D11 destination. Readback must be bit exact in both SDR
formats. Separate artificial gates hold the backend queue and the native copy:
unfinished output cannot be published, and finished backend output cannot be
reused while the native copy is pending. Unverified/wrong slots and unknown
backend decisions prohibit the copy. This tests transport in isolation, not an
actual LS generated-image pass or its queue cadence.

CPU completion events, native Flush and GPU readback are **test harness actions**.
They do not exist in the transport. This test measures content isolation and
resource lifetime, not NVIDIA support, VRR, physical monitor cadence, or LS
integration performance.

## API references

- [D3D11 context Signal](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-signal)
- [D3D12 interop contracts](https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-12-with-direct3d-11--direct-2d-and-gdi)
- [Shared heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps)
- [D3D12 queue Wait](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait)
