# Witcher Engine Fix

Crash, performance and quality-of-life fixes for **The Witcher: Enhanced Edition**. A small DLL that the game loads by itself and that corrects specific engine bugs in memory while the game runs. `witcher.exe` and your saves are never modified.

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
| **Cursor jumps to the top-left** | After the mouse has turned the camera (active pause or Shift in the over-the-shoulder camera), the cursor came back in the top-left area at resolutions above 1366x768. It now comes back in the middle of the screen. | on |
| **Slow return after alt-tab** | After alt-tabbing back, the game could take up to a second to react (it finished a one-second background sleep first). It now reacts at once, and still pauses while in the background. | on |
| **Exit game from the pause menu** | Quality of life: the pause menu (Esc) gets an "Exit game" button under "Exit to main menu", with the same "Are you sure?" question, so you no longer have to go through the main menu to quit. | on |
| **Quick loot** | Quality of life: Space in the loot window presses "Take all". In shops and storage it does nothing; everywhere else it still pauses, as before. The same as the [Witcher Quick Loot](https://github.com/MariusTBjercke/witcher-quick-loot) mod, which can stay installed alongside (it then takes over). | on |
| **Camera in active pause (over-the-shoulder)** | Quality of life: in the over-the-shoulder camera, active pause (Space) shows the cursor and only edge scrolling moved the camera. Hold the middle mouse button to turn it with the mouse, as in the other cameras. | on |
| **Borderless window** | Quality of life, off by default: Options > Graphics > "Borderless window". With full screen off and the desktop resolution, the game fills the screen without a border and alt-tab is instant, while the cursor, edge camera turn and pause-in-the-background behave as in full screen. | off |
| **Skip the intro** | Quality of life, off by default: `[qol] skip_intro=1` goes straight to the main menu at startup, without the logos and the intro cinematic. Uses the game's own "don't play intro" switch. | off |
| **Longer draw distances** | Optional, off by default: `[graphics] detail=1` (high) or `2` (ultra) keeps full-detail creatures and trees, grass, shadows and lights out to 1.5-3× the game's distances (scenery keeps the game's own level-of-detail distances), on top of your options menu. No debug mode or console commands needed. Ctrl+F12 in game cycles off/high/ultra and shows the level on screen, so you can compare. On a modern PC it cost under 0.5 ms per frame (72 FPS held at 4K). | off |

Each fix can be switched off on its own. Details for each fix are in [`docs/fixes/`](docs/fixes/).

The mod's own texts (the "Exit game" button, "Borderless window" and the Ctrl+F12 message) follow the game's text language, in all ten languages the game ships: English, Polish, German, French, Spanish, Italian, Russian, Czech, Hungarian and Traditional Chinese. Corrections are welcome ([how](docs/fixes/texts.md#adding-or-fixing-a-translation)).

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
| `System\Scripts\enginefix_hook.lua` | The load/save menu fix, the "The Source" door fix, the pause menu's "Exit game", "Borderless window" in the options and quick loot |
| `Data\Override\EngineFix\` | The "The Source" door script and the quick loot hotkey script (`.ncs`), with their sources (`.nss`) |
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
fix10=1  ; cursor comes back in the middle of the screen after mouse look, not in the top-left area
fix11=1  ; react at once when coming back to the game after an alt-tab

[qol]
ots_pause_mmb_camera=1   ; over-the-shoulder camera: hold the middle mouse button to turn it in active pause
borderless=0             ; borderless window; normally set in Options > Graphics, this line counts until the menu has saved it
quick_loot=1             ; quick loot: the key below presses "Take all" in the loot window
quick_loot_key=SPACE     ; one engine key name; with SPACE it still pauses outside the loot window
skip_intro=0             ; 1 = skip the logos and the intro cinematic at startup

[graphics]
detail=0   ; longer LOD, grass, shadow and light distances: 0 = the game's own, 1 = high, 2 = ultra
hotkey=1   ; Ctrl+F12 in game cycles off/high/ultra and shows the level on screen (until you quit)
```

The `[debug]` section holds diagnostics for development (logging only); leave them off.

Quick loot binds its key with the game's own key macros (saved in `System\keybinding.ini`). A bound key does nothing else, so with `SPACE` the mod performs the pause itself outside the loot window; any other key only takes all. Key names: `A`-`Z`, `0`-`9`, `F1`-`F12`, `NUMPAD0`-`NUMPAD9`, `INSERT`, `DELETE`, `HOME`, `END`, `PAGEUP`, `PAGEDOWN`, `SPACE`, `TAB`, `BACK`, `RETURN` and a few more ([full list](https://github.com/MariusTBjercke/witcher-quick-loot#what-the-hotkey-can-be)); no modifiers. A key changed here is unbound on the next start.

Three are switched in `System\Scripts\enginefix_hook.lua` instead: `local ENABLED = false` turns off the menu's Lua part, `local KIDNAP_DOOR_FIX = false` the "The Source" door fix, `local PAUSE_EXIT_BUTTON = false` the pause menu's "Exit game" button.

## Compatibility

- **Another `dinput8.dll`:** if your `System` folder already has a `dinput8.dll` from another mod, do not overwrite it. Both mods use the same file name, so only one can be loaded. Please open an issue.
- **DXVK** and texture packs (tested with the AiO texture pack) work alongside it. The area loading cache is keyed on the texture data itself, so changing or removing a texture pack is fine.
- **Area loading cache:** needs a CPU with SSE4.2 (any CPU since about 2008); without it that fix skips itself and says so in the log. ReShade uses `d3d9.dll`, not `dinput8.dll`, so it should too (untested).
- **[Witcher Speed Toggle](https://github.com/MariusTBjercke/witcher-speed-toggle) and [Witcher Quick Loot](https://github.com/MariusTBjercke/witcher-quick-loot)** share `System\Scripts\testtools.lua` with this mod. All three ship the same loader (version 2) since Speed Toggle 1.2.1 and Quick Loot 1.0.2, so they can be installed in any order. Older versions of those two put back version 1, which turns this mod's menu and door fixes off, and the log says so (`Lua save-list fix: INACTIVE`): update them, or copy `testtools.lua` from this mod back.
- **Witcher Quick Loot** is no longer needed (this mod does the same), but keeping it is fine: while its `quickloot_hook.lua` is installed, its hotkey and settings are used and this mod's quick loot stays out of the way. The log says which one is active (`qol 4`).
- **Above 60 FPS:** the game is usually capped at 60 FPS because of glitches above it. This mod fixes the known hair cause, but other high-frame-rate problems have not been ruled out yet. If you play above 60 and notice something, please report it.

## Uninstall

Delete `System\dinput8.dll`, `System\witcher_enginefix.ini`, `System\witcher_enginefix*.log`, `System\Scripts\enginefix_hook.lua` and `Data\Override\EngineFix`. Delete `System\Scripts\testtools.lua` too, unless Speed Toggle or Quick Loot is installed. Remove the line `SPACE runscript ef_lootall` from `System\keybinding.ini` (or the whole file, which only holds key macros), otherwise Space stays without its pause; setting `quick_loot=0` and starting the game once does the same. The menu fix keeps a thumbnail cache in `%LOCALAPPDATA%\The Witcher\enginefix_thumbs` and the area loading fix a texture cache in `%LOCALAPPDATA%\The Witcher\enginefix_mips` (small: about 10 MB after a handful of areas); both are safe to delete at any time.

## Reporting a problem

Open an [issue](https://github.com/MariusTBjercke/witcher-enginefix/issues) and attach `System\witcher_enginefix.log`. Say which store version you have (Steam/GOG) and whether you use DXVK or other mods.

## For developers

`build.bat` builds `build\dinput8.dll` (Visual Studio with the C++ x86 tools). `tools\install.ps1 -GamePath <game>` installs a build, and `tools\package-release.ps1` builds the release zip. How each fix was found, the addresses involved and open leads are in [`docs/fixes/`](docs/fixes/), [`docs/research.md`](docs/research.md) and [AGENTS.md](AGENTS.md).

## License

[MIT](LICENSE). The Witcher is a trademark of CD PROJEKT S.A. This is an unofficial fan mod.
