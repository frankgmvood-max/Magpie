# REA integration and LS follow-up (2026-10-10)

[REA](https://github.com/morluto/rea) is installed locally as `rea-agents@6.3.0`.
The verified provider is Ghidra 12.1.4 with Temurin JDK 21.0.12.1+1. Node
24.19.0 runs the CLI and the MCP client. No global agent configuration is changed.
The provider-scoped `doctor --provider ghidra --json` reports healthy.

## Two execution paths

`tools/rea_session.mjs` connects one persistent MCP session through stdin/stdout,
obtains the advertised schemas and saves tool responses in a private directory.
The tested catalog contains 139 tools; `open_binary` accepts the original PE.
The actual CLI entry is `scripts/rea.mjs --mcp`, not `dist/cli.js` or the `mcp`
registration subcommand. Client API: `@modelcontextprotocol/client@2.3.1`.

The standard native provider could not launch in this managed Linux environment:
its process ownership inspection relies on `ps`, but the PID namespace and
inherited `/proc` do not agree. Ghidra prerequisites being healthy does not
establish successful native analysis. This adapter does not weaken or fake
REA's daemon ownership checks.

`tools/rea_headless_batch.py` supplies a separate, bounded file transport for the
REA Java bridge. Ghidra runs as an ordinary foreground headless child; no provider
daemon, process-table inspection or listening socket is needed. It modifies a
private copy of the pinned bridge at one verified location, replacing its socket
server call with private request/response files. Upstream MIT attribution is kept.
The import digest, provider version, request authentication, schema checks,
decompiler and analysis-complete checks still run. Only an explicit read-only
method allowlist is admitted; database annotation is unavailable. Results are
REA **bridge responses**, not the upstream MCP evidence ledger. The adapter
rejects another source digest and existing output directories. Bearer request
files are removed on completion or launch failure.

Set `LS_REA_PREFIX` to the npm installation prefix, `GHIDRA_INSTALL_DIR` to the
extracted Ghidra directory and `JAVA_HOME` to JDK 21. Install the local npm runtime:

```sh
npm install --prefix /path/to/rea-runtime --no-audit --no-fund --save-exact \
  rea-agents@6.3.0 @modelcontextprotocol/client@2.3.1
node "$LS_REA_PREFIX/node_modules/rea-agents/scripts/rea.mjs" doctor --provider ghidra --json
node tools/rea_session.mjs local/rea-mcp
```

Each stdin line names a tool and its arguments, for example:

```json
{"name":"open_binary","arguments":{"path":"/absolute/path/Lossless_original.dll","provider_id":"ghidra"},"label":"open"}
```

End with `{"command":"exit"}`; an opened binary is closed before disconnect.
On a managed host with the process inspection limitation, run the file adapter
from the experiment directory instead:

```sh
python tools/rea_headless_batch.py \
  --binary /absolute/path/Lossless_original.dll \
  --sha256 626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb \
  --requests analysis/rea-ls322-requests.json --output local/rea-ls322
```

The original commercial DLL, decompilation, disassembly and raw responses stay in
ignored `local/`; they are not published in this repository. The request manifest
contains analyst-authored addresses and methods only.

## Verified input and observations

Two batches completed: 18 and 13 responses, zero bridge errors. Both report
`analysis_complete=true`, `analysis_timed_out=false`, PE x86-64/windows, image
base `0x180000000`, and the admitted SHA256. Below, addresses are **RVAs**.
Response IDs refer to the private batches `headless-01` and `headless-02`.

| Finding | RVA / object offset | Bridge evidence |
|---|---|---|
| Source-update body ends before preparation | 0x20260..0x20d85; preparation 0x20d90..0x21937 | 01: 3–6, 11–14 |
| Source write toggles the two-slot ring, then writes current u0 | +0x40, +0x2d8[index]; Dispatch 0x20393 | 01: 4–5 |
| Final t0/t1 retain previous/current source order | +0x2e8[1-index], +0x2e8[index] | 01: 8–9 |
| Phase selects original endpoints outside the interior interval | phase <= tolerance → previous; phase >= 1-tolerance → current | 01: 8–9; tolerance value remains caller-dependent |
| Interior phase is stored at byte 28; CPU buffer allocation is 48 bytes, DYNAMIC | phase storage +0x258[level] +0x1c; buffer +0x298[level] | 01: 8–9 |
| Output ring toggles independently; synthesis writes its selected UAV | +0x48, +0x1108[index]; Dispatch 0x245cd | 01: 8–9 |
| Final returned SRV has a native alternate branch | +0x1118[index] or +0x10d8 | 01: 8–9 |
| Direct caller updates the source, invokes preparation, then synthesizes in its phase loop | caller 0x14c90; source call 0x15749, synth call 0x158bb | 02: 2–4; 01: 6,10 |
| The synthesized view returns to the caller's native output method | call at 0x158ca, caller vtable offset +0x20 | 02: 3–4; method identity is not recovered |
| Initialization entry wraps teardown/resource creation/shader initialization | 0x1e800 → 0x1e890 and 0x1fe10; teardown 0x24670 | 01: 16–17; 02: 6–12 |

The analysis corroborates the existing Dispatch hooks; it does not justify a new
guessed CPU ABI or moving Present ownership. Source-update dispatch is a recorded
write, not proof of a new game frame or GPU completion. Ghidra database signatures
can remain `undefined` before decompilation; inferred pseudocode is not original
source. The caller's indirect output/presentation methods need additional type
resolution and runtime verification. Graphics-stage and external mutations of
sources are not exhaustively covered by this investigation. NVIDIA readiness,
dual-adapter behavior, VRR pacing and real LS images remain hardware questions.

Private response SHA256 values:

- Batch 01: `339eb7a028927bd3476ca92dde8b184e7a95eca190a94c543a2d31d6b3e5c046`.
- Batch 02: `17a23114176ae4d1cd5e6e7ad24f6a085dc3e4f9b953b94705204788ff6fc42c`.

Toolchain archive hashes checked before extraction:

- Ghidra `ghidra_12.1.4_PUBLIC_20260921.zip`:
  `ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db`.
- Temurin `OpenJDK21U-jdk_x64_linux_hotspot_21.0.12.1_1.tar.gz`:
  `ce79869e1307ed8ee1e2baa86a412b1eb5b75d10a01006d788a6f968bcfaee94`.
- REA Java bridge source:
  `f846f430fb225f2dd839699a25b6b59cc5338733cf212e1ca5fb8e396eeba590`.

## LS 0.1.1 correction

The diagnostic writer previously held the controller mutex while formatting and
flushing its log. A slow write made the native callback miss that mutex, discard
a source update and invalidate pairing history. The writer now takes a short
snapshot of process-lifetime-pinned sessions, releases the controller mutex,
tries each backend's diagnostic lock and writes only after releasing all locks.
Busy diagnostics skip that sample. Completed-fence flag reads remain bounded
backend polling, not proof of a rendered NVIDIA frame.

The WARP native-hook regression deliberately blocks the diagnostic stream while
the owning render thread submits a new source and synthesizes the expected
midpoint. It checks unchanged controller-busy and source-epoch counters. Logs now
also show actual analysis dimensions, format and negotiated Optical Flow grid,
including a grid fallback. Real RTX 3080 testing is still required.
