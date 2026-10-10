# Actual REA analysis for the economy dispatch contract

This change used REA 6.3.0's Ghidra Java bridge via the reviewed file-batch
adapter, with Ghidra 12.1.4 and Java 21. It did not use an invented native CPU
signature or patch the commercial DLL. The private run `rea-bypass-015-01`
returned 23 successful responses. The ping response reported complete analysis
without a timeout. The imported original and its immutable snapshot have SHA-256
`626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb`.

`ls-dispatch-contract.json` is derived by `tools/derive_ls_bypass.py` from those
actual responses and provenance. It contains our address/resource observations,
not commercial disassembly or pseudocode. The response digest is
`3b8f8f668a56e0af153c28c560c41ddc43b2d83ba0270c82c973737ab6949ce2`.
Raw response files remain private. This is **bridge evidence**, not the upstream
MCP evidence ledger, and is **static analysis**, not NVIDIA runtime evidence.

| Native procedure RVA | Observed Dispatch calls | Runtime treatment |
|---|---:|---|
| 0x20260 source update | 6 | Preserve resource 254/call 0x20393; other five are analysis |
| 0x20d90 preparation | 5 | Analysis bypass candidates |
| 0x21940 synthesis | 16 | Fifteen analysis candidates; resource 256/call 0x245cd is the output pass |
| 0x1fe10 shader initialization | — | Maps resource IDs to shader fields used at those calls |

Each indirect call is six bytes; the published return address is its following
instruction. The bypass allowlist has 25 exact return RVAs. It is also gated by
the complete original DLL hash, exact native shader bytes, Fixed ×2 SDR profile,
initialized backend, native context binding and previously confirmed final
output call on that context. Unknown callers execute normally and are counted.
The source/preparation procedures contain the compute bindings and Dispatch
calls, without CPU readback of their analysis textures. Synthesis also retains
its CPU constant allocation and output-buffer bookkeeping.

LS analysis textures are still allocated, and all CPU methods still run. This
implementation removes selected GPU dispatch work, not every allocation or the
entire LSFG subsystem. It does not change capture, cursor, Present, swapchains,
frame limiting or NVIDIA Inspector settings.

A test-only synthetic caller-RVA fixture checks bypass behavior and output
pixels. That fixture cannot prove that the real LS/driver reaches the same
return addresses. Real acceptance requires increasing `analysis_skipped` and
`synthesis_skipped`, valid backend counters, correct pixels and user verification
of resume/restarts/VRR on the two-GPU system.
