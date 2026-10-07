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
| Separate source-update function | 0x20260 | Receives an input SRV; toggles source ring index at +0x40 before writing |
| Load input-update shader | 0x20200 → 0x2020c | RCDATA 254, shader stored at +0x1128 |
| Bind current source UAV | 0x20304 → 0x20326 | +0x2d8[current index] to u0; external input SRV bound as t0 at 0x202ea → 0x202fe |
| Input-update Dispatch | 0x20393 | Shader +0x1128, group 8×8; followed by unbinding and analysis preparation |
| Source texture creation | 0x1eac0 → 0x1ebb7 | Two DEFAULT textures, one mip/slice/sample, SRV+UAV, MiscFlags=0; UAVs +0x2d8, SRVs +0x2e8 |
| Final t0 binding | 0x244af → 0x244c4 | +0x2e8[1-current index]: previous source |
| Final t1 binding | 0x244ca → 0x244e1 | +0x2e8[current index]: newly updated source |
| Load final shader | 0x20231 → 0x20236 | Resource 256 passed to helper; output shader stored at object +0x11f0 |
| Bind final shader | 0x24570 → 0x2457d | Object +0x11f0 bound through context offset 0x228 (CSSetShader) |
| Final synthesis dispatch | 0x245cd | Context offset 0x148 (Dispatch), dimensions derived from output width/height |
| Phase-like value propagation | 0x21961, 0x21a68 | XMM2 copied to XMM9; written at byte 28 of per-level constant data |
| Constant buffer recreation | 0x21ab1 → 0x21ae3 | ByteWidth=48, Usage=2 (DYNAMIC), BindFlags=4 (CONSTANT_BUFFER); CreateBuffer at device vtable offset 0x18 |

RTTI does not recover names or prototypes. `.pdata` contains split function
fragments; adjacent unwind entries must not be interpreted as whole function
boundaries. The separate synthesis function is not a fourth vtable entry.

## Early source update, resource 254

RCDATA [10,254,1033] is 1216 bytes, SHA256
`214a8ad496c0ffff58e9739d4be085a450d82eaaf23e2ad21dea8b251872880f`.
Independent RDEF/SHEX parsing finds cs_5_0, 8×8×1 groups, one input texture t0,
one typed output UAV u0, and b0. InputOffset (uint2, byte 0) is used; Timestamp
and ResolutionInvScale are unused in this kernel. Its twelve instruction records
are consistent with input loading/output storage, rather than optical-flow synthesis.
The CPU toggles +0x40 at 0x20289..0x2029b and writes the selected owned texture.
The next kernel (255, +0x1130) processes that newly updated texture for analysis.

The position immediately **after Dispatch 254 submission** is an earlier candidate
for enqueueing a snapshot of the owned LS source image. This preserves LS's input
crop/offset handling; the external t0 may have a different extent. A Dispatch
return establishes submission order, not GPU completion or a unique game frame.
The observer performs no snapshot copies yet. There is no stable public ABI at
0x20260 and this work does not inject a guessed native CPU function prototype.

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
index at object +0x40. In this statically mapped path t0 is previous and t1 is
newly updated; endpoint branches return the same corresponding originals. This
order remains to be checked on actual controlled native images. t2/t3/t4 are bound from +0xfa8/+0x1018/+0x1078.
These three intermediate textures must not be called NGX motion vectors merely
because they are used by interpolation. Their packing/direction/units are unverified.

Subsequent local translated-assembly inspection narrows their *consumer* roles:
t2/t3 each provide two xy/zw displacement pairs; t4 supplies four blend-logit
samples. Producer conventions and temporal interpretation remain unverified.
See [synthesis-inputs.md](synthesis-inputs.md); this is not GPU validation.

UAV comes from the output ring around +0x1108; the function returns an SRV from
+0x1118 indexed by +0x48, with an alternate branch returning +0x10d8. Texture
identity and conversion branches require runtime verification. The two endpoint
branches near 0x21976 and 0x219a0 return original image views without synthesis.

Timestamp is consistent with a continuous interpolation parameter, but midpoint
0.5, frame direction, end-point tolerance and source order have not been checked
against controlled images on the user's machine. No readiness/deadline is recovered
from this float. It is not an absolute presentation timestamp just because of its name.

The 32-byte size above is the shader's reflected layout, not the CPU allocation.
The inspected CPU path releases the earlier buffer (+0x298), writes the phase
into the CPU data (+0x258 → byte 28), and creates a **48-byte dynamic** buffer
using initial pSysMem. This supports an observation-only CreateBuffer snapshot
without GPU readback. DYNAMIC is value 2; IMMUTABLE is value 1. A creation
snapshot of a dynamic buffer is valid only until a later write. The observer
invalidates it on Map, CopyResource/CopySubresourceRegion(1), UpdateSubresource(1),
and invalidates all dynamic epochs on ExecuteCommandList. It never reads a
native mapped write-combined pointer. An incomplete mutation-hook installation
stops observation. This does not assume another mode/version follows the path. Constant-buffer subrange offsets
are decoded from CSGetConstantBuffers1 where that interface exists.

Resources 262/266/281 also use Timestamp and multiple inputs, but are loaded into
other object members (+0x1170 in alternative modes). This is why the earlier heuristic
"six inputs and one output = final synthesis" is insufficient. Replacing them would
touch internal analysis instead of the observed final synthesis dispatch.

## Remaining evidence

1. Trace controlled translations and static/HUD scenes with native Fixed ×2.
2. Confirm the statically mapped pair order on controlled native images; establish capture IDs, phase, lifetime and final copy path.
3. Observe resource reuse during focus loss, resizing, capture/device changes.
4. Validate the early Dispatch 254 position and image extent before connecting asynchronous snapshots/NGX.
5. Demonstrate a bit-identical no-op replacement before substituting DLSS output.

No GPU run, CPU decompilation into rebuildable original source, or DLL patch was performed.
