# NGX runtime discovery, 0.1.3

The 0.1.2 user log ran for about 351 seconds with the eligible LSFG3 Fixed x2
SDR profile. Four native devices attached without hook errors. 16,889 source
updates and 16,885 matching synthesis slots were observed. Each backend failed
at capability discovery with `0xBAD00004`; no evaluations or composites were
submitted. These CPU observations do not prove the source pixel contents.

The pinned NVIDIA SDK defines this status as `FAIL_FeatureNotFound`, separately
from `FAIL_FeatureNotSupported` (`0xBAD00001`). The old combined error message
did not report whether Available's query or FeatureInitResult supplied it.
It therefore cannot establish GPU, HAGS, driver, signature or path rejection.

## Early utility proxy loading

The supplied archive contains a SM86 `version.dll`, SHA256
`c3934a09399f022504227c72df0bf8c0de55f9a08880dddde898c5262cefa838`.
Its exports include `DlssgProxy_Name` and `DlssgProxy_Role`; it embeds 310.9.1
runtime data. This is metadata from the supplied binary, not runtime acceptance.
SM86's published installation guide describes utility proxy activation and
LoadLibrary interception, including the load-order sensitivity of dxgi/d3d12.

Replacing the addon manager removes its import dependencies. Automatic utility
DLL loading also depends on the modules already loaded by .NET. This is a
plausible integration defect; the 0.1.2 log does not establish which proxy was
loaded. Before hook installation and all native NGX calls, the observer worker
hashes the local utility proxy and explicitly loads this recognized version by
absolute path with DLL-load-directory/System32 dependency resolution. It pins
the resulting load reference and logs the actual module path and marker export.
No utility DLL is distributed, copied, renamed or modified. Unknown hashes are
reported but never explicitly loaded. Existing dxgi-based SM86 continues to use
its normal loading path. `loaded` does not prove successful backend installation;
SM86's own install/runtime_redirect/backend_install logs are needed for that.

## Diagnostics

- Startup reports existence/SHA256 of the three runtime candidates and local
  version/dxgi/SM86 INI. NGX's documented search path list remains unchanged.
- Own-process module snapshots run only on the worker before NGX and after the
  first completed initialization attempt. They print only relevant NGX/SM86 and
  D3D loader paths. No diagnostic-only DLL loading or foreign-process inspection.
- Each session reports Init/GetCapabilityParameters and five individual GetI
  results/values: Available, FeatureInitResult, NeedsUpdatedDriver, and minimum
  driver major/minor. A value is valid only when its query succeeds (`0x1`).
- The NGX logging callback owns a sanitized copy of up to 1023 bytes. A 256-entry
  multi-producer try-lock queue drops contention/full records. Total verbose
  callback output is limited to 2048 messages per process; losses are counted.
  Callback execution performs no file I/O, GPU wait or backend/controller lock.
  NGX's other logging sinks are disabled to avoid synchronous driver file I/O.
  The diagnostics worker drains after releasing all backend/controller locks.
- `Collect-Logs.cmd` collects bounded snapshots of native, NGX and SM86 logs,
  two INIs, and file version/size/hash metadata. It excludes DLL/EXE contents,
  unrelated root logs, and oversized log files. Original settings are untouched.

Windows tests exercise callback ownership/truncation, concurrent producers,
drop accounting/output bounds, rejection of an unknown utility DLL and the
collector's file selection/preservation. WARP tests do not execute SM86 or NVIDIA
FG. Actual load redirection, NVIDIA feature creation, generated pixels and LS
presentation timing remain to be tested on the user's RTX 3080.

Primary references:

- [Pinned NVIDIA SDK definitions](https://github.com/NVIDIA/DLSS/blob/374959484e79a640feaba44c93ac8cfb0a03f5b5/include/nvsdk_ngx_defs.h)
- [SM86 installation guide](https://github.com/sdli1995/dlssg_for_sm86/blob/main/docs/INSTALL.en.md)
