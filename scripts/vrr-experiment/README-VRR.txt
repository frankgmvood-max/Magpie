VRR complete experiment - test suite

Close Magpie fully before switching launchers. Do not copy an older EXE or
runtime DLL over this build. Keep your existing compatible FG runtime DLLs.
Use FULLSCREEN scaling, not windowed scaling. Select your usual FG profile.
For a clean test use a moving video / benchmark that keeps running unfocused.

1-Safe-VRR.cmd: normal input, forced DXGI VRR path, two buffers, latency one,
Reflex Sleep and async markers disabled for this experiment.
3-Maximum-opaque-foreground.cmd: strongest ordinary HWND candidate; opaque
fullscreen with focus belonging to Magpie. VIEW-ONLY: game input is not routed.
2-Opaque-source-focus.cmd: same opaque output while source retains focus;
VIEW-ONLY: opaque HWND cannot provide layered mouse pass-through.
4-Layered-foreground.cmd: focus test with layered rendering. VIEW-ONLY.
5-Driver-vsync-probe.cmd: normal input, Present(1,0), checks driver selection
with vsync presentation. This is a comparison, not proof of VRR.
0-Restore-normal.cmd: original user settings and input policy.
The launchers write vrr-experiment-mode.txt beside Magpie.exe so automatic
restarts retain the selected mode; launcher 0 or deleting this file resets it.

Start and stop with the usual global Scale shortcut. Alt-Tab remains available.
Run each mode for 20 seconds with moving content. Check monitor refresh OSD.
Attach magpie.log AND magpie.1.log; automatic Magpie restarts can rotate logs.
G-SYNC is sampled every two seconds, with the selected mode and adapter LUIDs.
No registry/driver profile changes, game injection, or NoFocusLoss hooks occur.
A failure of these modes does NOT establish that VRR is impossible: driver
settings, scanout mode, and OS composition still require hardware verification.
