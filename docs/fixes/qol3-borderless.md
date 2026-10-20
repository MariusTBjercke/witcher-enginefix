# QoL 3: borderless window

Options > Graphics > "Borderless window", off by default. Greyed out while "Full screen" is ticked; takes effect at once.

## Background

In fullscreen the game runs Direct3D in exclusive mode. Every alt-tab recreates the swapchain (DXVK) and about 200 GUI textures, and it is where the alt-tab lag was seen ([research.md](../research.md), "Lag after alt-tab"). Windowed mode avoids that, but has a title bar and border, so at the desktop resolution the bottom of the picture is off-screen.

How the window is set up (Steam 1.5.0.1304), in `FUN_005932f0`, which creates the Direct3D device:

- windowed: `SetWindowLongA(GWL_STYLE, 0x02CA0000)` (caption, system menu, minimize box; `mov esi` at `0x0059334D`), `AdjustWindowRect` so the client area is the game's resolution, `SetWindowPos` at (0, 0), `HWND_TOP`;
- fullscreen: style `0x82000000` (popup), `HWND_TOPMOST`.

Other behaviour is tied to fullscreen through `[0x00EE47BC]->vfunc+4`:

| Site | What it does in fullscreen |
|---|---|
| `FUN_004e2100` (Lua `CGuiInGame` cursor clipping) | `ClipCursor` to the screen |
| `FUN_0043f6c0` | `ClipCursor(NULL)` when leaving that state |
| `FUN_004262f0` | the camera turns when the cursor is at the screen edge |
| `0x007532E7` in the main loop (`FUN_007530d0`) | sleeps 1 s per frame while the window is inactive, which pauses the game after an alt-tab |
| `FUN_005932f0`, `FUN_0053e5e0` | device creation and the device-lost path: windowed or exclusive |
| `FUN_0074c2d0`, `0x0074EEEC` | debug server, a call whose result is unused |

In windowed mode `GetAllVideoModes` (`hwdepsettings.luc`) lists the modes up to the desktop size, so the desktop resolution can be picked.

## Change

DLL (`ApplyBorderless`), always installed once the original bytes are checked, and inert while the option is off:

- `mov esi,0x02CA0000` at `0x0059334D` becomes `call BlStyleStub`, which reads the stored option at that moment and loads the windowed style from it: `0x02CA0000` (the original) while the option is off, `0x82000000` (popup) while it is on (all registers but ESI kept, as before). At the desktop resolution the popup covers the primary monitor without a border, and Direct3D stays windowed;
- the first four sites in the table call `BorderlessFullscreen` instead of the vfunc. It returns "fullscreen" while the option is on (cursor clip, edge camera turn, pause in the background as in fullscreen) and the real answer while it is off. Device creation and the device-lost path still ask the real question;
- a thread reads the stored option ~3 times a second. When it changes and the game is windowed at that moment, the thread restyles the game window itself (`SetWindowLongA` with `WS_VISIBLE` + `SetWindowPos` with `SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS`, sized like `FUN_005932f0`: client area kept, window at (0, 0); not while minimized). The game's own styles lack `WS_VISIBLE`; a first version copied the bit from the window and could hide it, which froze the game on a black screen (an invisible window never has the focus, so the game paused itself). The thread also releases the cursor clip (`ClipCursor(NULL)`, which is global) when another window gets the focus.

Lua (`enginefix_hook.lua`): `CEngineFixBorderlessSetting`, a `CCheckBoxSetting` in the Graphics category, registered with `RegisterLuaSetting` and read right away (the game reads its settings before the hook runs). `CFullScreenSetting.ApplyChanges` is wrapped to store the option first: OK applies "Full screen" before it, and the switch to windowed reads the stored option. The game stores it like its own settings when OK is pressed, as the registry value `EngineFixBorderless` under `HKCU\Software\CD Projekt RED\Witcher\Settings`. `IsEnabled` is false while the "Full screen" setting's current value (ticked in the panel, not only applied) is 1, looked up as `CGammaSetting:IsEnabled` does (`GetSetting("CFullScreenSetting")`; searching by index found nothing); the panel asks every setting again after each checkbox click (`UpdateSettings`), so the box greys out at once, and it keeps its value for when full screen is turned off: the panel discards unsaved changes of greyed-out settings in `UpdateSettings`, which would undo an untick made just before ticking "Full screen", so `DiscardChanges` does nothing while `UpdateSettings` runs (Cancel, `RevertSettings`, still discards). The label and the tooltip (`g_tSettingsDescriptions`, which takes plain text through `processItem`) are English only. Until the menu has saved the option, `[qol] borderless` in the ini decides.

To use it: Options > Graphics, "Full screen" off, "Borderless window" on, the desktop resolution, OK.

## Verification

Harness (unpacked exe mapped at its addresses, a fake video object that says "windowed"): with the option off the style stub gives `0x02CA0000` and the four sites return the real answer (0); flipping the stored option makes the style stub follow it at once and the sites within a second; with a real window as the game's (framed, visible, 640x360 client) the thread restyles it to `0x96000000` and back to `0x16CA0000`, visible, client size kept; started with it on, the stub gives `0x82000000` and the sites return 1.

In game, 2026-10-20 12:45 (first version, applied at startup): window 3840x2160 at (0, 0), style `0x96000000` (popup, no caption, not topmost), three alt-tabs back at 72 FPS at once, game paused while away. In game, 2026-10-20 13:06-13:28 (the final version): full screen and borderless switched both ways without a restart (`96000000` borderless, `16ca0000` with a frame), and borderless on/off while windowed restyled the window, which stayed visible. The first in-game try of the live switch had frozen on a black screen (the window lost `WS_VISIBLE`, fixed), and a first gray-out looked the setting up by index and never greyed it (fixed). Alt-tab back: the game reacts within one frame (see [fix 11](fix11-inactive-sleep.md)); switching still feels a little slower than the framed window because Windows changes the presentation mode of a window that covers the screen. Not checked separately: cursor clipping (one monitor here), the edge camera turn, G-Sync.
