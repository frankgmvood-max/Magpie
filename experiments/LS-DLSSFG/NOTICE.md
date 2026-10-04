This experimental addon is MIT licensed. Its Present hook is derived from
Echo-Storm/ls-addon-manager commit fbcc3c179e4052785c032d157d8eb785608eb28e,
under the MIT terms in LICENSE-EAM. Changes add callback handling, reject partial
Present1 calls, and install directly on the live output without a probe window. The manager SDK is consumed from that pinned commit.

The build uses NVIDIA/DLSS SDK commit
374959484e79a640feaba44c93ac8cfb0a03f5b5. NVIDIA's SDK and the linked NGX object
code retain NVIDIA's terms (NVIDIA-LICENSE.txt in the compiled package).
Runtime DLLs and third-party compatibility mods are not bundled.

Read-only G-SYNC telemetry uses official NVIDIA NVAPI header declarations at
revision 87dca625e83fd89a983e19b904e5f3a580da90d2, fetched and SHA-256 checked by
scripts/Fetch-NvapiHeaders.ps1. NVAPI terms are in NVIDIA-NVAPI.txt. No NVIDIA
driver DLL is bundled; only system nvapi64.dll is dynamically loaded.

The addon is a separately built experiment. No Magpie source is linked into
its DLL. The enclosing repository's license remains unchanged.

0.2.0 studies Magpie Experimental 0.6.9 revision
2fceab5e241bc9f8ded001ab3266762f1f8bc51e (GPL-3.0). Profile semantics,
current-to-previous S10.5 motion, 1/1 MFG resets and exact RGB duplicate checking
were used as design references. The addon has its own implementation; Magpie's
Renderer/FrameSource/DeviceResources source is not copied or linked.
The untouched upstream source is preserved in branch upstream-magpie-0.6.9.

NVOF API header licences/provenance are in third_party/nvof/NOTICE.md.
ImGui is pinned to the manager's exact revision
367b2c24f399988ddafc0bb4628da0106bcc09be (MIT). Its licence is included in the
compiled package. Only System32 nvofapi64.dll is loaded; no OF driver is bundled.
