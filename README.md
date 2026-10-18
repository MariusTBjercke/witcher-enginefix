# Witcher Engine Fix

Crash fixes and performance fixes for **The Witcher: Enhanced Edition**, patched into the running game. It works like the community "engine fixes" for other old games: a small DLL that the game loads by itself, which corrects specific engine bugs in memory. `witcher.exe` on disk is never modified.

> **Status: work in progress.** There is no release yet. Every fix is verified in the game, but only on one setup (Steam 1.5.0.1304, Windows 11, DXVK).

## Fixes

| # | Fix | Type | Default |
|---|---|---|---|
| 1 | Crash when a death effect's creator no longer exists (big fights) | crash | on |
| 2 | The main thread is pinned to CPU core 0 | performance (frame pacing) | on |
| 3 | Shader effect cache keep-alive | experimental, no measured benefit | **off** |
| 4 | The load/save list rewrites every save thumbnail each time it opens | load-menu speed | on |
| 5 | Saving uses zlib's byte-at-a-time CRC32 | save/autosave speed (4x faster CRC, identical result) | on |
| Lua | The load/save list re-lays out every row for each row it adds | load-menu speed | on |

With 531 saves, opening the load menu went from **14.1 s to under 1 s**, including the first open after starting the game. Details for each fix are in [`docs/fixes/`](docs/fixes/).

## Requirements

- The Witcher: Enhanced Edition, patch **1.5**. Developed and tested on **Steam 1.5.0.1304**. Every fix checks the exact original bytes before it patches anything, so on another build it skips itself and logs why instead of patching blindly.
- The game folder must not already contain a `dinput8.dll` from another mod.

## Installation (from source, until there is a release)

1. Build: run `build.bat` (needs Visual Studio with the C++ x86 tools). This writes `build\dinput8.dll`.
2. Close the game, then install:
   ```
   powershell -ExecutionPolicy Bypass -File tools\install.ps1 -GamePath "D:\SteamLibrary\steamapps\common\The Witcher Enhanced Edition"
   ```
   This copies `System\dinput8.dll`, `System\witcher_enginefix.ini` (an existing one is kept) and `System\Scripts\enginefix_hook.lua`.
3. The Lua fix is loaded by the shared `System\Scripts\testtools.lua` loader from [Witcher Speed Toggle](https://github.com/MariusTBjercke/witcher-speed-toggle) and [Witcher Quick Loot](https://github.com/MariusTBjercke/witcher-quick-loot). Add `"enginefix_hook"` to its `HOOKS` list. A loader that does this on its own is planned (see [AGENTS.md](AGENTS.md)).

Start the game through Steam as usual. `System\witcher_enginefix.log` shows which fixes were applied.

## Configuration

`System\witcher_enginefix.ini`. Restart the game after changing it.

```ini
[fixes]
fix1=1   ; crash fix: death effect with a creator that no longer exists
fix2=1   ; do not pin the main thread to CPU 0
fix3=0   ; experimental: keep shader effects cached
fix4=1   ; save list: do not rewrite thumbnails that are already up to date
fix5=1   ; saves: faster crc32 (identical result)

[debug]
savelist_timing=1   ; log how long building the load/save list takes, per step
crc_timing=1        ; log crc32 time and bytes per save (with fix5=0 it times the original)
```

The Lua fix is switched off with `ENABLED = false` at the top of `System\Scripts\enginefix_hook.lua`.

## Uninstall

Delete `System\dinput8.dll`, `System\witcher_enginefix.ini`, `System\witcher_enginefix*.log` and `System\Scripts\enginefix_hook.lua`, and remove `"enginefix_hook"` from `testtools.lua`. Fix 4 also keeps a thumbnail cache in `%LOCALAPPDATA%\The Witcher\enginefix_thumbs`, which is safe to delete.

## Building

`build.bat` builds `build\dinput8.dll` and the profiler `build\sampler.exe`. Set `VCVARS` to your `vcvars32.bat` if Visual Studio is not in the default location. Developer notes, addresses and the research behind each fix are in [AGENTS.md](AGENTS.md) and [`docs/`](docs/).

## License

[MIT](LICENSE)
