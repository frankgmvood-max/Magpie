# REA: native LS Type integration, 0.3.0

Target is the user's exact LS 3.2.2 managed WPF assembly, SHA256
`e4ea2dbb1371ea1920d73c87202f19b6f095ef6f521a8a7a139d19f34f1fe866`.
This investigation actually invoked REA 6.3.0's upstream MCP tools
`inspect_managed_artifact`, `inspect_managed_members`,
`inspect_managed_native_boundaries` and `compare_managed_members`.
Evidence identifiers and original hook tokens/body digests are in
[ls-native-ui-contract.json](ls-native-ui-contract.json).
Private raw commercial CIL, full Evidence and patched target DLL stay outside git.

REA establishes a .NET 9 WPF UI, nested `UI.Profile.FrameGenerationEnum`,
`UI.Pages.ProfilePage`, `UI.MainWindow.ApplyProfileToCore` and original XML
settings serialization. Type is assigned to the actual selected Profile and
`ProfileChanged` saves it, applying the active profile to native code.
The FG card is a WPF Border; its original ComboBox and native controls are kept.
A new enum member DLSS=6 is separate from obsolete persisted values 4/5.
The helper appends one item at the real runtime item count; its index is not
assumed equal to the serialized enum. Profile load guards prevent the original
SelectionChanged handler from overwriting enum 6 with that new list index.

REA's managed artifact coverage is complete; member coverage is partial.
The managed comparison is also partial and reports substantial changed/unmatched
metadata after Cecil reorders signature/token references. **It does not certify
that only the intended methods changed or that the UI runs.** Independent
source-owned Cecil verification compares resolved semantic instruction operands
and resource digests: all 533 original declarations and P/Invoke definitions
remain, exactly six original bodies receive hooks, and embedded WPF/localization
resource bytes are identical. That verification is not presented as REA output.
`tools/verify_managed_patch.cs` can reproduce it against private originals.

The native boundary was checked through the pinned REA/Ghidra file-batch adapter,
with two bounded requests at `ApplySettings`, RVA 0x2be80. This is an actual REA
bridge response, **not** an upstream MCP native Evidence ledger. REA's decompiler
and instruction listing show stores from 32 used native arguments. The managed
P/Invoke declares an additional final setup parameter, unused in this native
function. The working proxy's 32-argument forwarding ABI is retained.
Only native type=6 maps to the existing verified LSFG3 Fixed x2 SDR scheduler;
the stored original native parameters are untouched and return with native Type.
The new UI bridge sorts last at ordinal 12, preserving all original 11 ordinals.

The patcher is our C# source plus pinned Mono.Cecil 0.11.6, not an upstream REA
rewriter. It rejects unrecognized managed bytes, leaves BAML intact and loads
our own UI helper explicitly before the patched page is JIT-compiled. The
installer generates the changed target locally; no commercial patched binary
is redistributed. Backup/rollback includes UI and backend, and converts the
unsupported XML enum DLSS to LSFG3 when restoring an unpatched UI.

The WPF stand-in and ABI/WARP tests are source-owned controlled fixtures.
They validate hooks, original item indices, XML/copy semantics, native return,
bridge validation, policy and GPU dispatch safeguards. They do not execute the
commercial WPF application's full startup or verify NVIDIA, SM86, two GPUs,
physical VRR, frame pacing or generated pixel quality. Those require the user's
hardware run and resulting logs. Optical Flow graph quality changes still
require full LS restart; process-pinned GPU graphs are not destructively rebuilt.
