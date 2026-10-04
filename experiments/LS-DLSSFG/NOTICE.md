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
