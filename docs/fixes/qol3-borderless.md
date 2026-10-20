# QoL 3: borderless window

Options > Graphics > "Borderless window (when not full screen)", off by default.

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

DLL (`ApplyBorderless`), only when the option is on (all original bytes checked first):

- the windowed style becomes `0x82000000`: at the desktop resolution the window covers the primary monitor without a border, and Direct3D stays windowed;
- the first four sites in the table call `BorderlessFullscreen` instead of the vfunc and get "fullscreen", so the cursor is clipped, the edge camera turn works and the game pauses in the background as in fullscreen. Device creation and the device-lost path still ask the real question;
- a thread releases the cursor clip (`ClipCursor(NULL)`, which is global) when another window gets the focus.

Lua (`enginefix_hook.lua`): `CEngineFixBorderlessSetting`, a `CCheckBoxSetting` in the Graphics category, registered with `RegisterLuaSetting` and read right away (the game reads its settings before the hook runs). The game stores it like its own settings, as the registry value `EngineFixBorderless` under `HKCU\Software\CD Projekt RED\Witcher\Settings`. A change asks for a restart (`NeedsRestart`), as "Full screen" does. The label is English only (no talk-table entry). The DLL reads the value at startup; until the menu has saved it, `[qol] borderless` in the ini decides.

To use it: Options > Graphics, "Full screen" off, "Borderless window" on, the desktop resolution, restart.

## Verification

Harness (unpacked exe mapped at its addresses): with the option on, the style is `0x82000000` and the four sites call into the DLL and get 1; with it off nothing is patched. Not tested in game yet: the menu entry, the window, alt-tab, cursor clipping, the edge camera turn, the pause in the background, G-Sync.
