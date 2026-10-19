# Witcher Engine Fix

Crash and performance fixes for **The Witcher: Enhanced Edition**. A small DLL that the game loads by itself and that corrects specific engine bugs in memory while the game runs. `witcher.exe` and your saves are never modified.

## What it fixes

| | What you notice | Default |
|---|---|---|
| **Crash in big fights** | A random crash when a death effect fires after the creature that caused it is already gone. | on |
| **Slow load/save menu** | With many saves the menu took seconds to open. 531 saves: **14 s → under 1 s**. | on |
| **Faster saving and loading** | The save checksum is computed 4× faster (a 230 MB save: ~0.33 s → 0.08 s). Identical result, so saves stay compatible with the unmodified game. | on |
| **Faster area loading** | The game rebuilt the mipmaps of textures that ship without them (menus, fog of war) on the CPU at every loading screen. They are now built once and kept in a cache, with byte-identical results: about **1 s less per area transition** once an area has been visited (menu textures 0.63 s → 0.05 s; the first menu load at startup 0.44 s → 0.07 s). | on |
| **Shorter wait after loading** | After a loading screen the game waited for the texture loader in 250 ms steps, sometimes 0.25 s for nothing. It now checks every millisecond. | on |
| **Slow first load/save menu after a PC restart** | The first time the menu opened after a restart, every save was opened cold (the antivirus scans each file again): 3.5-6 s with 575 saves instead of ~1 s. The saves are now read once in the background while the game starts. | on |
| **Main thread forced onto CPU core 0** | A 2007-era workaround made the game share core 0 with Windows' interrupt work. Windows can now pick a free core. | on |
| **Hair depends on frame rate** | Hair damping was applied once per frame, so hair moved differently above 60 FPS. It now behaves as at 60 FPS at any frame rate. | on |
| **Locked door in "The Source"** | Chapter III: Dandelion leads you to Alvin's kidnappers, but their door stays locked. The door is now unlocked when the game failed to, also in saves that are already stuck there. | on |

Each fix can be switched off on its own. Details for each fix are in [`docs/fixes/`](docs/fixes/).

## Requirements

- The Witcher: Enhanced Edition, patch **1.5**. Tested on **Steam 1.5.0.1304** (Windows 11, DXVK).
- GOG: same game build, but not tested yet. Every fix checks the exact game code before it patches anything. On a different build it skips itself and writes that to the log, so the worst case is that a fix does nothing.

## Installation

1. Download `WitcherEngineFix-<version>.zip` from [Releases](https://github.com/MariusTBjercke/witcher-enginefix/releases).
2. Extract the `System` and `Data` folders into your game folder and merge them with the existing ones, for example:
   `...\steamapps\common\The Witcher Enhanced Edition\`
3. Start the game as usual.

To check that it works, open `System\witcher_enginefix.log` after starting the game. Each fix should say `applied`.

| File | Purpose |
|---|---|
| `System\dinput8.dll` | The engine fix. The game loads it automatically |
| `System\witcher_enginefix.ini` | Settings |
| `System\Scripts\enginefix_hook.lua` | The load/save menu fix and the "The Source" door fix |
| `Data\Override\EngineFix\` | The "The Source" door script (`.ncs`) and its source (`.nss`) |
| `System\Scripts\testtools.lua` | Shared startup loader (see [Compatibility](#compatibility)) |

## Configuration

`System\witcher_enginefix.ini`, 1 = on, 0 = off. Restart the game after a change.

```ini
[fixes]
fix1=1   ; crash in big fights (death effect)
fix2=1   ; do not force the main thread onto CPU core 0
fix3=0   ; experimental, leave off
fix4=1   ; load/save menu: do not rewrite save thumbnails that are already up to date
fix5=1   ; faster save checksum
fix6=1   ; hair independent of frame rate
fix7=1   ; read the saves in the background at startup (first load/save menu after a PC restart)
fix8=1   ; area loading: cache the mipmaps the game builds on the CPU
fix9=1   ; shorter wait for the texture loader after a loading screen
```

The `[debug]` section holds diagnostics for development (logging only); leave them off.

Two fixes are switched in `System\Scripts\enginefix_hook.lua` instead: `local ENABLED = false` turns off the menu's Lua part, `local KIDNAP_DOOR_FIX = false` the "The Source" door fix.

## Compatibility

- **Another `dinput8.dll`:** if your `System` folder already has a `dinput8.dll` from another mod, do not overwrite it. Both mods use the same file name, so only one can be loaded. Please open an issue.
- **DXVK** and texture packs (tested with the AiO texture pack) work alongside it. The area loading cache is keyed on the texture data itself, so changing or removing a texture pack is fine.
- **Area loading cache:** needs a CPU with SSE4.2 (any CPU since about 2008); without it that fix skips itself and says so in the log. ReShade uses `d3d9.dll`, not `dinput8.dll`, so it should too (untested).
- **[Witcher Speed Toggle](https://github.com/MariusTBjercke/witcher-speed-toggle) and [Witcher Quick Loot](https://github.com/MariusTBjercke/witcher-quick-loot)** share `System\Scripts\testtools.lua` with this mod. This mod ships loader version 2, which runs all three. If you install one of the others afterwards and it puts back version 1, the menu fix stops working, and the log says so (`Lua save-list fix: INACTIVE`). Copy `testtools.lua` from this mod back.
- **Above 60 FPS:** the game is usually capped at 60 FPS because of glitches above it. This mod fixes the known hair cause, but other high-frame-rate problems have not been ruled out yet. If you play above 60 and notice something, please report it.

## Uninstall

Delete `System\dinput8.dll`, `System\witcher_enginefix.ini`, `System\witcher_enginefix*.log`, `System\Scripts\enginefix_hook.lua` and `Data\Override\EngineFix`. Delete `System\Scripts\testtools.lua` too, unless Speed Toggle or Quick Loot is installed. The menu fix keeps a thumbnail cache in `%LOCALAPPDATA%\The Witcher\enginefix_thumbs` and the area loading fix a texture cache in `%LOCALAPPDATA%\The Witcher\enginefix_mips` (small: about 10 MB after a handful of areas); both are safe to delete at any time.

## Reporting a problem

Open an [issue](https://github.com/MariusTBjercke/witcher-enginefix/issues) and attach `System\witcher_enginefix.log`. Say which store version you have (Steam/GOG) and whether you use DXVK or other mods.

## For developers

`build.bat` builds `build\dinput8.dll` (Visual Studio with the C++ x86 tools). `tools\install.ps1 -GamePath <game>` installs a build, and `tools\package-release.ps1` builds the release zip. How each fix was found, the addresses involved and open leads are in [`docs/fixes/`](docs/fixes/), [`docs/research.md`](docs/research.md) and [AGENTS.md](AGENTS.md).

## License

[MIT](LICENSE). The Witcher is a trademark of CD PROJEKT S.A. This is an unofficial fan mod.
