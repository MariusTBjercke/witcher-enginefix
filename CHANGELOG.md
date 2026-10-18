# Changelog

## Unreleased

- Fix 1: crash when a death effect's creator no longer exists.
- Fix 2: the main thread is no longer pinned to CPU 0.
- Fix 3 (experimental, off): shader effect cache keep-alive.
- Fix 4: the load/save list no longer rewrites unchanged save thumbnails; a thumbnail cache survives between sessions.
- Fix 5: saves use a slicing-by-8 CRC32 instead of zlib 1.1.4's byte-at-a-time version (4x faster, identical result).
- Lua: the load/save list is laid out once instead of once per row (531 saves: 14.1 s → 0.84 s together with fix 4).
- Diagnostics: per-step timing of the load/save list (`[debug] savelist_timing`) and crc32 time per save (`[debug] crc_timing`).
