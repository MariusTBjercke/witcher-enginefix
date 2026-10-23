# Fix 13: hotkeys stop working after Alt+Tab in exclusive full screen

## Cause

Mods bind their hotkeys with the engine command `bindkey` (Witcher Speed Toggle's V/B, Witcher Quick Loot or this mod's quick loot on a key other than Space). They are saved in `System\keybinding.ini`, and at startup `InitShortcuts` (`FUN_0041f030`) runs every line of that file through `bindkey` again.

`bindkey` (`FUN_0041eeb0`) does two things: it stores the macro in the map at `0x00C51304`, and it registers the key as an action with the input manager (`FUN_004187a0` → `FUN_004ccaa0` on `*([0x00EE47A4]+4)`). The video reinit `FUN_007503e0` deletes that input manager, creates a new one (`FUN_004186e0`) and registers only the game's own keys on it (`FUN_00402eb0` → `FUN_004077d0`). From then on every `bindkey` hotkey is dead until the game is restarted. Keys the game uses itself (Space) keep their game action.

`FUN_007503e0` has two callers:

| Caller | When | Covered by |
|---|---|---|
| `FUN_00404ff0` | "Full screen" or the resolution changed in the options | the shared Lua loader (version 3, `testtools.lua`): it wraps `OnReInitAuroraEnd`, which this path calls right after, and replays `keybinding.ini` |
| `FUN_0053e5e0` | Present returned `D3DERR_DEVICELOST` and `TestCooperativeLevel` returns `D3DERR_DEVICENOTRESET`: Alt+Tab back into exclusive full screen without DXVK | nothing: this path calls no Lua |

`FUN_0053e5e0` runs every frame (from `FUN_0074e7e0` and the main loop `FUN_007530d0`).

## Patch

The call `call FUN_007503e0` at `0x0053E695` (cdecl, 4 arguments, `add esp,0x10` after it) now goes to a C function that calls `FUN_007503e0` with the same arguments and then replays `keybinding.ini` the way `InitShortcuts` does: each line through `bindkey`, with `[0x00C512EA]` set so `bindkey` does not save the file again while it is being read. The file is opened as `InitShortcuts` opens it (`[0x00C280C4]` = `L"keybinding.ini"`, mode `rt`, shared), with the same line buffer size (`0x3FF0`).

`InitShortcuts` itself is not called: it opens the file and never closes it (no `fclose` in `FUN_0041f030`), so every Alt+Tab would leak a C runtime stream. It also clears the macro map first; the replay does not need to, because `bindkey` overwrites a key's macro in the map.

The replay only runs when `FUN_007503e0` actually replaced the manager (it deletes it only while the renderer `[0x00EE47BC]` and the game window `[0x00EE4798]` exist) and a manager exists afterwards. So a key is registered once on each new manager, never twice on the same one. The options path is left to the Lua loader. Each replay is logged: `fix 13: device reset, N of M keybinding.ini lines bound again`.

With DXVK (the usual setup) Alt+Tab does not lose the device, so this path does not run and nothing changes.

## Verification

Harness (fixed-base test exe: the call site with its pushes and `add esp,0x10`, `FUN_007503e0` and `bindkey` faked at their addresses, a test `keybinding.ini`): the call is redirected, the four arguments arrive unchanged, every line goes to `bindkey` with `[0x00C512EA]` set and the flag is restored afterwards (also when the game already had it set), no replay without a window, and no replay or crash without `keybinding.ini`.

In game (2026-10-23, without DXVK, exclusive full screen at 3840x2160, owner): after each of two Alt+Tabs the log shows `fix 13: device reset, 3 of 3 keybinding.ini lines bound again`, and V/B in Speed Toggle changed the speed one step per press. Full screen toggled in the options reset the device without a fix 13 line (that path is the loader's) and gave no double steps.

The screen stays dark for 1-2 s when coming back. That is the device reset itself, not the replay: the frame log showed `Reset` taking 190-310 ms and a longest frame of 1.5-1.8 s after each return, and the same 1.5 s frame after the full screen change in the options, where fix 13 does not run. DXVK, or QoL 3's borderless window, avoids the reset altogether.
