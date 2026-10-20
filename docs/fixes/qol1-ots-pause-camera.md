# QoL 1: middle mouse button turns the OTS camera during active pause

## Cause

In the isometric and hybrid cameras, holding the middle mouse button turns the camera with the mouse, also during active pause (Space). In the over-the-shoulder (OTS) camera the mouse always turns the camera, so the game never wired the middle button there. Active pause in OTS shows the cursor, and then only edge scrolling moves the camera.

How the engine does it (Steam 1.5.0.1304):

- The input handler `FUN_00423bd0` (`this` = the in-game input handler, in ESI; button state in EBX: 1 down, -1 repeat, 0 up) gets the middle button as event `0x72`. At `0x00424213` it compares the camera mode `[0x00C500F0]` with 2 (OTS) and skips the button (`jz 0x00425174` at `0x00424219`).
- In the other cameras: down → `FUN_0041bb60` → `FUN_0041b830`, which (while `g_cAuroraSettings.m_bMMBCameraTurn` at `0x00C5044C` is on, the default) hides the cursor, saves its position (`+0x2A4`/`+0x2A8`), sets the turn flag `+0x348` and lets the mouse turn the camera. Up → `FUN_0041bbf0` → `FUN_0041b9b0` (ends the turn, shows the cursor), then `SetCursorPos` to the saved position. `FUN_0041bbf0` skips `FUN_0041b9b0` in OTS.
- OTS mouse look is the same turn mode: unpausing calls `FUN_0041b830(1, 0)`, pausing `FUN_0041b9b0(1)` (`FUN_00444cb0`, behind the Lua `SetActivePauseState`).
- The pause state is the byte `[handler+0x4F0]` (`FUN_00404860`, what `GetActivePauseState` returns): bit 4 is active pause. Read live in OTS: 0 while playing (turn flag 1), 4 in active pause (turn flag 0).

## Patch

The 6-byte `jz` at `0x00424219` (`0F 84 55 0F 00 00`) jumps to a stub instead (`pushfd`, `pushad`, `call OtsMmb(handler, state)`, `popad`, `popfd`, `jmp 0x00425174`), so the game's own path continues as before and the stub only adds the turn:

- down, in active pause (bit 4) and not already turning: call `FUN_0041bb60`, as the other cameras do;
- up, if we started the turn and the game is still paused: `FUN_0041b9b0` and `SetCursorPos` back to where the cursor was;
- up after unpausing while the button was held: nothing, the game has already switched OTS mouse look back on.

Outside active pause the middle button does exactly what it did before (nothing in OTS). On by default; `[qol] ots_pause_mmb_camera=0` turns it off. The first three presses are logged (`qol 1: middle button ... pause state ..., camera turn ...`) to check the pause bit in game.

## Verification

Harness (the original `jz` at `0x00424219` behind `xor eax,eax`, fake `FUN_0041bb60`/`FUN_0041b9b0` that set/clear `+0x348` and count calls; dinput8.dll loaded): not paused → nothing; active pause → down starts the turn, a repeat does nothing more, up ends it; unpaused while held → up leaves the turn flag alone; already turning → nothing; another pause bit (1) → nothing; the skip target is reached in every case. Not yet tested in game.
