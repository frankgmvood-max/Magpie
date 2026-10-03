This experimental addon is MIT licensed. Its Present hook is derived from
Echo-Storm/ls-addon-manager commit fbcc3c179e4052785c032d157d8eb785608eb28e,
under the MIT terms in LICENSE-EAM. Changes add callback handling and reject
partial Present1 calls. The manager SDK is consumed from that pinned commit.

The build uses NVIDIA/DLSS SDK commit
374959484e79a640feaba44c93ac8cfb0a03f5b5. NVIDIA's SDK and the linked NGX object
code retain NVIDIA's terms (NVIDIA-LICENSE.txt in the compiled package).
Runtime DLLs and third-party compatibility mods are not bundled.

The addon is a separately built experiment. No Magpie source is linked into
its DLL. The enclosing repository's license remains unchanged.
