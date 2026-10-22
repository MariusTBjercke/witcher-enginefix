# Fix 12: the cursor vanishes in the main menu (Shift, coming back to the game)

## Cause

With the over-the-shoulder (OTS) camera chosen, a Shift press in the main menu hid the cursor. It only showed while Shift was held, and alt-tab did not bring it back. Found through Win+Shift+S (the Snipping Tool): the cursor was gone after it, and the Snipping Tool overlay got no mouse.

The input handler `FUN_00423bd0` (ESI = the client object, the same one `FUN_00417e50` runs) handles Shift in the OTS camera (`[0x00C500F0] = 2`):

| | Where | What |
|---|---|---|
| Shift down | `0x004245E1` | `FUN_0043ee00(0)` on `[ESI+0x48]` (mouse no longer turns the camera), `FUN_009b8fc0` (ShowCursor 1) |
| Shift up | `0x00424650` | `FUN_0043ee00(1)` (`[ESI+0x48]+0x94` = 1, mouse turns the camera), `FUN_009b9020` (ShowCursor 0) |

The same happens when the game gets the focus back: `FUN_0074dcf0` (called at `0x0074EED7`; it does this only while the byte `[0x00EE49F1]` is set, and clears it; what sets it was not looked up. Seen after the Snipping Tool's overlay closed; the earlier alt-tabs in the main menu did not do it) switches mouse look back on in the OTS camera unless Shift is held (`FUN_0043ee00(1)`, `FUN_009b9020`). There Shift did not even bring the cursor back, since fix 12's first version already kept Shift out of the main menu.

The Shift paths skip only when a fullscreen panel is open (`[[ESI+0x80]]+0x119`), during active pause (`[ESI+0x4F0] & 4`) or when `[ESI+0x48]` is NULL. In the main menu none of these hold, so Shift up turns mouse look on. `FUN_00419b50` then parks the hidden cursor in the middle every frame (`[ESI+0x48]+0x94` set, exclusive DirectInput mouse acquired). The exclusive mouse (`FUN_004cc670`: `GUID_SysMouse`, `DISCL_EXCLUSIVE | DISCL_FOREGROUND`) is acquired through `FUN_00412650` already at startup (called from `FUN_00752af0`) and released when the game loses the focus (from `0x0074EF2A`), main menu included. While the cursor is hidden like this, other programs that open over the game (the Snipping Tool overlay) get no mouse either.

`[debug] mouse_log` showed it: in the main menu, Shift up made no call to `FUN_0041b830` (the usual start of a camera turn), and the exclusive mouse was acquired at startup (`FUN_00412650`), not by Shift.

The main menu sets `[ESI+0x609]` while it is open (`CClientExoApp:SetMainMenuPanelActive`, read by `GetMainMenuPanelActive`: `[[app]+0x10]+0x609`).

## Patch

Each path loads the camera object and skips everything when it is NULL:

- Shift: `mov ecx,[esi+0x48]; test ecx,ecx; jz <skip>` (`0x00424612`, `0x0042467D`). The first 5 bytes become a call to a stub that does the same load and test, but gives NULL while `[ESI+0x609]` is set.
- Focus back: `call FUN_00402980` (`0x0074DDF7`, ECX = the app; returns `[[app+0x10]+0x48]`), then `test eax,eax; jz`. The call goes to a stub that does the same, but returns NULL while `[[app+0x10]+0x609]` is set.

In a level nothing changes.

## Verification

Harness (both sites with their `jz` and markers behind them, `ESI` a fake client): before the DLL and in a level both paths run; with the main-menu byte set, and with no camera object, both skip. Verified in game 2026-10-22 21:05 (owner): Shift in the main menu leaves the cursor visible. The focus path was added after the cursor vanished once the Snipping Tool closed (21:27, `[cam]+0x94 = 1` read from the running game, no `FUN_0041b830` call and no Shift); harness for it (the call, `FUN_00402980` and `FUN_00404610` at their addresses, a fake app and client): before the DLL and in a level the path runs, in the main menu and without a camera it skips. Verified in game 2026-10-22 21:33 (owner): the cursor stays after the Snipping Tool closes, and Shift in a level works as before (mouse look starts from the game's usual places, camera mode 2).

The Snipping Tool not opening over the game is a separate cause: the game's DirectInput keyboard (`FUN_004cc720`) uses `DISCL_NONEXCLUSIVE | DISCL_FOREGROUND | DISCL_NOWINKEY` (`0x16`). A test window with the same setup blocked both Win+Shift+S and Print Screen; with `0x06` (no `DISCL_NOWINKEY`) both opened the Snipping Tool. The exclusive mouse did not block either. Handled by [QoL 6](qol6-windows-key.md).
