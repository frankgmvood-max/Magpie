# Static mapping of the provided native DLL

Input filename: Lossless_original.dll. Version recorded in PE: 3.2.2.0.
Size 7521280. SHA256 `626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb`.
All addresses below are RVAs, not stable runtime addresses. Do not hook another
version using these offsets. Reproduce the metadata with `verify_reference.py`.

## Observations from this input (2026-10-07)

| Object / operation | RVA / offset | Evidence and limit |
|---|---:|---|
| LSFG3 type descriptor | 0x4da88 | MSVC RTTI name and COL links |
| LSFG3 vtable | 0x441f8 | Three contiguous function pointers, validated against executable sections |
| First vtable entry | 0x1e800 | Leads into initialization and resource creation; original method name unknown |
| Second vtable entry | 0x20d90 | Runs preparation passes; returns at 0x21937 before a separate synthesis function |
| Third vtable entry | 0x24670 | Releases owned COM resources; teardown candidate |
| Separate synthesis function | 0x21940 | Receives this in RCX, output pointer in RDX, float argument in XMM2 and boundary parameter in XMM3 |
| Shader loading helper | 0x28730 | RCDATA type 10, resource lookup/loading, device call at vtable offset 0x90 (CreateComputeShader) |
| Load final shader | 0x20231 → 0x20236 | Resource 256 passed to helper; output shader stored at object +0x11f0 |
| Bind final shader | 0x24570 → 0x2457d | Object +0x11f0 bound through context offset 0x228 (CSSetShader) |
| Final synthesis dispatch | 0x245cd | Context offset 0x148 (Dispatch), dimensions derived from output width/height |
| Phase-like value propagation | 0x21961, 0x21a68 | XMM2 copied to XMM9; written at byte 28 of per-level constant data |

RTTI does not recover names or prototypes. `.pdata` contains split function
fragments; adjacent unwind entries must not be interpreted as whole function
boundaries. The separate synthesis function is not a fourth vtable entry.

## Final synthesis candidate, resource 256

The independently parsed RDEF gives five texture SRVs (t0..t4), one output UAV
(u0), two samplers, and cbuffer b0. Shader model cs_5_0, group 16×16×1.
DXBC hash `3af0031e97f43a9372c23749ebbbe91506119268d84e869ba715ffe71bb9d45a`.
Use the generated JSON as authority if this transcribed hash ever differs.

| Constant | Offset | Type | Compiler says used |
|---|---:|---|---|
| InputOffset | 0 | uint2 | No |
| FirstIter | 8 | uint | No |
| FirstIterS | 12 | uint | No |
| AdvancedColorKind | 16 | uint | No |
| HdrSupport | 20 | uint | No |
| ResolutionInvScale | 24 | float | Yes |
| Timestamp | 28 | float | Yes |

The CPU binds t0 and t1 using the two original-frame SRV slots and the alternating
index at object +0x40. t2/t3/t4 are bound from +0xfa8/+0x1018/+0x1078.
These three intermediate textures must not be called NGX motion vectors merely
because they are used by interpolation. Their packing/direction/units are unverified.

UAV comes from the output ring around +0x1108; the function returns an SRV from
+0x1118 indexed by +0x48, with an alternate branch returning +0x10d8. Texture
identity and conversion branches require runtime verification. The two endpoint
branches near 0x21976 and 0x219a0 return original image views without synthesis.

Timestamp is consistent with a continuous interpolation parameter, but midpoint
0.5, frame direction, end-point tolerance and source order have not been checked
against controlled images on the user's machine. No readiness/deadline is recovered
from this float. It is not an absolute presentation timestamp just because of its name.

Resources 262/266/281 also use Timestamp and multiple inputs, but are loaded into
other object members (+0x1170 in alternative modes). This is why the earlier heuristic
"six inputs and one output = final synthesis" is insufficient. Replacing them would
touch internal analysis instead of the observed final synthesis dispatch.

## Remaining evidence

1. Trace controlled translations and static/HUD scenes with native Fixed ×2.
2. Establish original pair order, frame IDs, phase, resource lifetime and final copy path.
3. Observe resource reuse during focus loss, resizing, capture/device changes.
4. Identify the earlier point where a complete source pair can start asynchronous NGX.
5. Demonstrate a bit-identical no-op replacement before substituting DLSS output.

No GPU run, CPU decompilation into rebuildable original source, or DLL patch was performed.
