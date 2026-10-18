# Changelog

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
