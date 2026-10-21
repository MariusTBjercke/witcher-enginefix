# Changelog

## Unreleased

- Graphics detail: high and ultra stretch the level of detail for scenery again, now without holes. In the areas that have it (the Outskirts and a few others), buildings, fences, trees and bushes placed in the area, and the ground keep their full detail out to 2-3x the distance. The holes came from the game's visibility data per area section, which skipped detailed ground farther away than the game ever drew it; at high and ultra that check is skipped for these level-of-detail models only.

## 0.6.1

- Graphics detail: high and ultra no longer stretch the LOD distances for scenery. Parts of distant hills could go missing (the sky showed through), for example in the Outskirts. Creatures and trees still get the longer LOD distances, and grass, shadows, lights, decals and reflections are unchanged. Reported by forfunandmore.
- QoL 5, skip the intro (off by default): `[qol] skip_intro=1` in the ini goes straight to the main menu at startup, without the logos and the intro cinematic. Suggested by forfunandmore.
- French: the pause menu's "Exit to main menu" now reads "RETOUR AU MENU PRINCIPAL" instead of the game's "QUITTER", so it no longer looks like the "QUITTER LE JEU" button under it, and the "Borderless window" tooltip reads more naturally. Both from Illuzio.
- Diagnostics: `[debug] engine_log` copies the engine's own messages (such as why a conversation was interrupted) into the log, with changes of the in-conversation state.

## 0.6.0

- QoL 4, quick loot (on by default): Space in the loot window presses "Take all"; in shops and storage it does nothing, everywhere else it still pauses and unpauses. The same as the Witcher Quick Loot mod, now built in. `[qol] quick_loot=0` turns it off, `quick_loot_key` picks another key (that one then only takes all). With Witcher Quick Loot installed as well, Quick Loot's hotkey and settings are used and this one stays unbound, whichever was installed first. A Space binding left behind by a removed Quick Loot (Space without pause) is cleaned up.
- The mod's own texts follow the game's text language, in all ten languages the game ships (English, Polish, German, French, Spanish, Italian, Russian, Czech, Hungarian, Traditional Chinese): the pause menu's "Exit game" button, "Borderless window" and its tooltip, and the Ctrl+F12 message. In French, German and Polish the button used to read the same as "Exit to main menu" (the game's own text for both). French texts from Illuzio.

## 0.5.1

- The Ctrl+F12 on-screen message now also shows without DXVK. Windows' own Direct3D 9 rewrites the device's Present function after the first frames, which unhooked the message; it is hooked again when that happens.
- The "Borderless window" tooltip is shorter.

## 0.5.0

- Graphics detail (optional, off by default): `[graphics] detail=1` (high) or `2` (ultra) in the ini multiplies the engine's LOD distances for scenery, creatures and trees, grass range, shadow distance and shadow-casting lights, how far lights stay on, decal and weapon effect distances, and the water reflection texture size, on top of what the options menu sets. No debug mode or console commands. At 4K on a modern PC ultra cost under 0.5 ms per frame. Ctrl+F12 in game cycles off/high/ultra and shows the level on screen for a moment (`[graphics] hotkey`, on).
- QoL 3: "Borderless window" in Options > Graphics (off by default). With full screen off, the game fills the screen without a border and alt-tab is instant, while the cursor clip, the edge camera turn and the pause in the background work as in full screen. Takes effect at once; greyed out while full screen is on.
- Fix 11: after an alt-tab back the game reacted up to 1 s late (it finished a 1 s background sleep first); it now reacts at once and still pauses in the background.
- The journal's tabs fill like the load/save list: one refresh per list instead of one per entry.
- Diagnostics: with `frame_log`, a line every 10 s with the frame rate and main-thread time for the current detail level, and the time from the game window getting the focus to the next frame.

## 0.4.0

- Fix 10: after mouse look (active pause or Shift in the over-the-shoulder camera) the cursor came back in the top-left area at resolutions above 1366x768, because the game parked it at half the GUI size taken as pixels. It is now parked at the middle of the window.
- QoL 1: in the over-the-shoulder camera, holding the middle mouse button during active pause turns the camera with the mouse, as in the other cameras (on by default, `[qol] ots_pause_mmb_camera`).
- QoL 2: "Exit game" in the pause menu, under "Exit to main menu" (same confirmation; uses the game's own translated texts). `local PAUSE_EXIT_BUTTON` in `enginefix_hook.lua`.
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
