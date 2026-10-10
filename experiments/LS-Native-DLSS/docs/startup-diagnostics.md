# Startup diagnostics from the 0.1.1 runtime logs

The two latest user logs each have the header, configuration, and one profile
sample. The Fixed log admits LSFG3/Fixed/x2/SDR (`eligible=1`) with zero source
updates and no backend session snapshot. The other selects Adaptive and remains
ineligible. Private user logs are not committed to this repository.

The Fixed sample establishes profile admission only. It does not establish a
successful D3D11 hook attachment, original-shader match, backend initialization,
NVOF/NGX failure, crash or hang. In 0.1.1 the writer returns without output when
the controller mutex is busy, and exits without a final record when the observer
is deactivated. `Source` calls `Initialize` while holding that mutex. Therefore
both a long initialization and a stopped observer could leave a short log.
There is no basis to select either as the cause from these samples.

0.1.2 records a heartbeat outside the controller mutex even when session sampling
is unavailable. `diagnostics_busy=1` distinguishes that skipped session sample.
`backend_phase` identifies Initialize/Source/Composite under the existing mutex.
The worker records active state, elapsed time, native CreateDevice calls/results,
successful attachments, shader/buffer tags, observed dispatches and rejections.
Dispatch callback counts can include shared runtime methods on non-LS devices.
They do not prove a new game frame or successful presentation.

Hook failures retain the method, create/enable/capacity stage, and MinHook error
in one atomic record. A packed stop record retains the first reason and code.
The last hook failure is diagnostic context, not necessarily the first failing
operation. Required hook failure leaves native forwarding active while stopping
observation. Tagging/attachment exceptions also stop observation conservatively.
No failed or partial attachment authorizes replacement. Startup records precede
potentially slow WARP probes and identify their HRESULTs. The periodic worker
always writes a terminal snapshot and `writer=stopped` when it observes inactive
state, including when it starts after activation has already failed. A process
crash, forced exit, or frozen worker need not produce this terminal record.

These changes do not relax profile, binary-hash, shader, source-pair or GPU-ready
gates. The native output and Present path remain owned by LS.

Regression checks in the Windows native-hook test:

1. Hold the controller mutex while the diagnostics worker runs; require a bounded
   heartbeat with `backend_phase=initializing diagnostics_busy=1`.
2. Exercise an actual MinHook rejection of a non-executable target. Require the
   rejected operation and symbolic error in the inactive worker's terminal log.
3. Preserve the first stop reason when a later normal trace-stop occurs.

The earlier blocked-log test still checks that slow stream I/O cannot take away
the source generation or expected synthetic GPU midpoint. These WARP tests
validate observation and diagnostics, not NVIDIA compatibility or LS pacing.
