# Changelog

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
