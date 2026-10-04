# Offline-inspected NvPresent profile 66ace

SHA256: `66aceaa6f7539d3171de14e88b725a96f86fb9a370fe4d2e151e451cd11fd712`.
File size: 8,396,632 bytes. PE32+ AMD64 image size: `0x80c000`.
The driver's marketing version cannot be inferred from these facts. The DLL
has not been executed in this inspection and is not committed or packaged.

The architecture gate is unique: compare immediate RVA `0xc3af`, SETGE SIL
RVA `0xc3c7`. `NVP_Init_D3D` is at `0x59e0` and points to configuration
`0x7d2d50`. The initializer returns C++ bool in AL. Its enable calculation
consumes `config+0xe1`, not the reference profile's `+0xe9`; startup derives
`+0x129c`/`+0x129d`. Runtime layout validation checks these instruction
signatures before any code or data patch. A configuration already initialized
by another path is rejected, rather than resetting undocumented caches.

The configuration's own log labels confirm `+0x4f` as DX11 allowance and
`+0x50` as DX12 allowance. The addon enables DX12 and disables DX11 in its
loaded NvPresent so the LS D3D11 surface is not also interpolated. Patches
are snapshotted/restored in memory; no NVIDIA profile/registry is written.
NVP's native initialization obtains CreateDXGIFactory2 and installs factory
method hooks. The private output uses a fresh factory2 rather than the
manager's virtual factory1 wrapper.

Direct named CUDA imports are present: cuModuleLoadData IAT `0x1d0810`,
cuGraphLaunch IAT `0x1d07a8`. cuGetProcAddress_v2 is used for cuCtxCreate
in this image; module load and graph launch call their named thunks. No
additional CUDA resolver hook is justified for this inspected binary.

The D3D12 private-controller vtable is at `0x1d1d08`. The actual constructor
at `0x516c9` loads this address and writes it to the object at `0x516d0`.
Its enable/option methods are **19/20**, at `0x12f50`/`0x12ed0`, using a
bool in DL and controller fields `+0x50`/`+0x51`.
The adjacent tables at `0x1d1cd8` and `0x1d1cf0` each have three methods
and belong to other classes. The previous profile incorrectly indexed
25/26 from the first adjacent table; these indices crossed its boundary
and happened to reach the same method addresses, but its vptr did not
identify the real controller. The constructor reference, controller-table
prefix, method addresses and readable object range are now checked.

There are 37 valid fatbin containers, each containing SM120 and SM89 cubins.
The SM120 entries use header size `0x60`, ELF OSABI/ABI 65/8, e_flags
`0x06007802`. SM89 entries use header size `0x40`, ELF OSABI/ABI 51/7,
e_flags `0x00590559`. SM89 container arch becomes 86 and e_flags becomes
`0x00560556`, preserving all non-architecture bits and the original ABI.
SM120 and other bytes remain unchanged. Applying `0x06005604` to the
51/7 ELF would change its layout semantics, so the old unconditional
constant has been removed. Unsupported ELF layouts fail without passing
a partial rewrite to CUDA.

Validation covers profile mismatch/rejection cases, private-controller
identity, mixed CUDA ELF ABIs and the existing GPU transport tests. The
actual CPU rewriting implementation also verifies all 37 supplied
containers offline. Metadata retargeting is not a general SASS translator;
successful SM86 model execution, actual generated frames and physical VRR
still require the NVIDIA GPU. The active UI status continues to require
successful retargeted module loads and observed successful CUDA graphs.
The graph counter is compared to a per-chain initialization baseline,
including successful launches on a worker between Presents. The current
checks do not infer physical scanout or generated-frame cadence from this.
