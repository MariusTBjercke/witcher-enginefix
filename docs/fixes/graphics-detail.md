# Graphics detail: longer draw distances

`[graphics] detail=0|1|2` (off / high / ultra), off by default.

## Background

The engine keeps its distances in `g_cAuroraSettings` (object at `0x00C500E8`, constructor `FUN_00401190`). Every member is also a console variable: static initialisers register it with `push flag; push <variable>; push "g_cAuroraSettings.<name>"`, which is how the addresses below were found.

[Higher LOD](https://www.nexusmods.com/witcher/mods/1228) on Nexus sets 17 of them at startup through console commands. To get its commands run it turns on `m_nDebugMode`: `startup.luc` plays `startup_local` only in debug mode. Debug mode does more than that. The input handler `FUN_00423bd0` enables several debug actions with it (TGA screenshots and others), and about 20 other engine functions read it. That mod also sets fixed values that the options menu overwrites (the grass slider), and it changes gameplay (`m_fMapRevealRange` 30 → 100, how much of the map is uncovered around you).

What sets what (Steam 1.5.0.1304):

| Setting | Constructor | Set by the game | Used |
|---|---|---|---|
| `m_fSceneLODDistancesMultiplier` `0xC5058C` | 1 | never | `FUN_00594c70`: LOD blend = (d − k·first) / (k·last − k·first), first/last 5 / 45 m by default |
| `m_fCreatureLODDistancesMultiplier` `0xC50588` | 1 | never | `FUN_005557b0`, same formula for creatures |
| `m_fSpeedTreeLODDistancesMultiplier` `0xC50590` | 1 | `startup.luc` `BetterPerformance` (not called by any script) | `FUN_006fc050` recomputes the tree LOD distances when it changes |
| `m_fGrassVisibilityRange` / `m_fGrassFadeRange` `0xC50544`/`0xC50548` | −1 | options menu, `CGrassQualitySetting.ApplyChanges`: 60 m × slider/3, fade 0.8 × range | grass |
| `m_fMaxShadowDistance` `0xC50598` | 30 | never | `FUN_00591260`, `FUN_0070da00`: objects within this distance of the camera cast shadows |
| `m_nMaxShadowLights` `0xC506C4` | 4 | `gfxoptions.luc`: 1 on cards without blend-op support | `FUN_006bc4e0`, `FUN_006bc8c0`, `FUN_006b7c40`; the light lists grow with it (no fixed array) |
| `m_fLightsFadeDistanceFactor` `0xC505E8` | 7 | never | `FUN_0070ca90`: a light is on within its radius × this |
| `m_fMaxDecalsDist` `0xC508E8` | 80 | `startup.luc` 80 | decals (blood etc.) |
| `m_fMaxWeaponVisualsDist` `0xC508E4` | 40 | `startup.luc` 40 | weapon effects |
| `m_nReflectionRTSize` `0xC5051C` | 512 | never | no direct reads (read through a pointer); `CAurScene:RecreateReflectionRTs` (Lua binding `FUN_005d2bb0`, vtable +0x18C) rebuilds the reflection targets, so the size is meant to change. Added 2026-10-20; water in the swamp looked fine at ultra, no visible difference measured |

The registry (`HKCU\Software\CD Projekt RED\Witcher\Settings`) stores only the menu's slider positions (`GrassQuality`, `ShadowsQuality` ...), never these values.

Left out on purpose:

- `m_nMaxSceneLOD` / `m_nMaxCharacterLOD` (5): an upper bound on a model's LOD index when its LoD dummies are set up (`FUN_0059e6f0`), with `m_bCreateMissingLODs` = 1. Higher LOD's 150 most likely does nothing; it is not a distance.
- `m_fCharacterVisibility`: set per location from a 2DA and the "character visibility" slider (`AutoSetCharacterVisibility`, `FUN_0040b420`) on every area load. Characters are the most expensive objects on the single-threaded main loop.
- `m_fGrassDensityModifier` (more grass per square metre): set by the grass slider (1.0 at most); not checked yet.
- `m_fMapRevealRange`, `m_nDebugMode`: not graphics.

## Change

`ApplyDetail` checks, per setting, that the name string and the registration's `push <variable>` are the original bytes (otherwise that setting is skipped and logged). A thread then looks at the values every 50 ms. A value that differs from what it last wrote was set by the game (constructor, `startup.luc`, the grass slider), so it becomes the base. The thread writes base × factor; values ≤ 0 are left as they are (grass is −1 until the menu applies). Level 0 starts no thread and writes nothing.

| | high (1) | ultra (2) |
|---|---|---|
| LOD distances (scenery, creatures, trees) | × 2 (10-90 m) | × 3 (15-135 m) |
| shadow distance | × 2 (60 m) | × 3 (90 m) |
| grass range and fade (of the slider's value) | × 1.5 (90 m) | × 2 (120 m) |
| shadow-casting lights | 6 | 8 |
| light fade distance, decals, weapon effects | × 1.5 | × 2 |
| water reflection texture (`m_nReflectionRTSize` `0xC5051C`, 512 px) | 1024 | 2048 |

The log has one line each time the game sets some of the values, e.g. `graphics detail 2: the game set fGrassVisibilityRange 60 -> 120, fGrassFadeRange 48 -> 96`.

`[graphics] hotkey=1` (default) makes Ctrl+F12 cycle the level in game until it exits (`detail=` is the level at startup). Each press shows "Graphics detail: High (Ctrl+F12)" at the top of the screen for 2.5 s: on the first press the device's `Present` is hooked (vtable slots 16/17, shared with the frame log), and right before each `Present` the text is drawn on the back buffer with D3DX's font from `d3dx9_35.dll`, which the game loads (raw vtable calls; the font's sprite saves and restores the render states; `OnLostDevice`/`OnResetDevice` around `Reset`). With `[debug] frame_log=1` a line every 10 s gives the frame rate and the main thread's time per frame outside `Present`, which is the game's own work (the DXVK frame limiter waits inside `Present`). The old `[debug] detail_hotkey` still turns the hotkey on. Verified in game 2026-10-20 (DXVK, 4K, borderless and full screen): the message shows, 72 px, and fades out.

## Verification

Harness (unpacked exe mapped at its addresses, the constructor's and the grass slider's writes simulated): levels 0, 1 and 2 give the values in the table, slider changes are rescaled, and nothing compounds.

In game, 2026-10-20 (3840x2160, DXVK, capped at 72 FPS, all menu options at maximum; standing still, the level cycled with Ctrl+F12):

- The game's own writes were picked up as planned: the constructor's values at startup, the grass slider (`fGrassVisibilityRange 60 -> 120`) and `startup.luc`'s decal and weapon distances 3 s later.
- Vizima at night and the swamp: 72.0 FPS at every level. The main thread's own work per frame (outside `Present`) went up by 0-0.5 ms from off to ultra in back-to-back measurements at the same spot (Vizima ~7.0 → 7.5 ms, swamp ~5.5 → 5.8 ms), out of a 13.9 ms frame at 72 FPS. The GPU kept up (it would show as missed frames under the cap).
- Visible: distant lights in Vizima at night (they stay on much farther away), trees and grass in the swamp. No artefacts noticed.

Not measured: combat with many creatures, and slower PCs. Hence off by default.
