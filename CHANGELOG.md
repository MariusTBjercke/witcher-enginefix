# Changelog

## Unreleased

- QoL 2: "Exit game" in the pause menu, under "Exit to main menu" (same confirmation; uses the game's own translated texts). `local PAUSE_EXIT_BUTTON` in `enginefix_hook.lua`.
- Fix 10: after mouse look (active pause or Shift in the over-the-shoulder camera) the cursor came back in the top-left area at resolutions above 1366x768, because the game parked it at half the GUI size taken as pixels. It is now parked at the middle of the window.
- QoL 1: in the over-the-shoulder camera, holding the middle mouse button during active pause turns the camera with the mouse, as in the other cameras (on by default, `[qol] ots_pause_mmb_camera`).
- Diagnostics: `frame_log` also logs time in Present, how long Sleep(1) lasts, a line after each return to the game (window, display and timer state) and, with `frame_sampler`, profiles the alt-tab lag.

## 0.3.0

- Fix 7: the first load/save list after a PC restart is no longer slow (3.5-6 s with ~575 saves): the saves are read once in the background at startup.
- Fix 8: area loads no longer rebuild the mipmaps of textures that ship without them (UI, fog of war; ~1-1.5 s of a ~2.8 s transition) on the CPU every time: the levels D3DX built are kept in a disk cache (`%LOCALAPPDATA%\The Witcher\enginefix_mips`) and reused, with byte-identical results.
- Fix 9: after a load the game no longer waits for the texture loader in 250 ms steps (up to ~240 ms per load).
- The log no longer drops lines when two threads write at the same moment.
- Diagnostics: `[debug] wait_timing` (Sleep polling per caller, texture wait after loads), `texture_timing` (time per texture during loads) and `frame_log` (stretches of low frame rate with the window state).

## 0.2.0

- "The Source" (Chapter III): the kidnappers' hideout door is unlocked when the game failed to (its unlock script can run while the door's area is not loaded, so it misses the door). Works on saves that are already stuck at the door. No game file is replaced: `enginefix_hook.lua` runs `Data\Override\EngineFix\ef_q3043_door.ncs` after each loading screen.
- Tools: `tools/witcherres.py` (KEY/BIF, MOD and GFF reader), `tools/ncsdis.py` (NCS disassembler), `tools/build_ef_ncs.py`.

## 0.1.0

First release.

- Fix 1: crash when a death effect's creator no longer exists.
- Fix 2: the main thread is no longer pinned to CPU 0.
- Fix 3 (experimental, off): shader effect cache keep-alive.
- Fix 4: the load/save list no longer rewrites unchanged save thumbnails; a thumbnail cache survives between sessions.
- Fix 5: saves use a slicing-by-8 CRC32 instead of zlib 1.1.4's byte-at-a-time version (4x faster, identical result).
- Fix 6: hair (dangly nodes) is damped per second like at 60 FPS instead of once per frame.
- Lua: the load/save list is laid out once instead of once per row (531 saves: 14.1 s → under 1 s together with fix 4).
- Ships `testtools.lua` loader version 2, which also runs `enginefix_hook`. The log says when the Lua part is inactive.
- Diagnostics (`[debug]` in the ini) are off by default.
