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

Mode 6 (6-Opaque-native-input.cmd): opaque NOACTIVATE output with the usual
cursor mapping and 3D clipping retained. A 65x65 physical-pixel window-region
aperture lets Windows deliver native mouse input to the source. No game hooks
or synthetic messages. The aperture exposes a small patch of the source;
non-rectangular output may disable DirectFlip/G-SYNC. Fast clicks may outrun
frame-based aperture updates. This is experimental, not a VRR guarantee.

Mode 7: 7-Opaque-136FPS.cmd retains mode-6 input. DLSSFG uses a fixed
136 FPS deadline, prepares frames ahead of submission, bounds catch-up to
5% of one period and reanchors after stalls. A 200us precision wait tail
returns on messages/events. FG x2 needs a sustainable 68 FPS source.
The known aperture/cursor artifact is unchanged; compare against mode 6.
