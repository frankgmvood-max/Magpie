# MinHook 1.3.4

Vendored from https://github.com/TsudaKageyu/minhook/tree/v1.3.4.
Unmodified upstream sources; BSD 2-Clause license in LICENSE.txt.
Compiled into the static Magpie.Core library on x64 and Win32 only.

NoFocusLoss keeps the original foreground trampoline allocated for the process
lifetime. Stopping a scaling session clears its window snapshot and disables
the hook; it never frees a trampoline which another thread may still execute.
