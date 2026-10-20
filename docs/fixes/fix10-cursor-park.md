# Fix 10: cursor reappears in the top-left area after mouse look

## Cause

When the cursor comes back after the mouse has turned the camera (active pause or Shift in the over-the-shoulder camera, releasing the middle button in the other cameras), it shows up in the top-left area of the screen, not in the middle or where it was.

While the mouse turns the camera, `FUN_00419b50` (called from `FUN_004262f0` every frame) measures the mouse movement with `GetCursorPos` and parks the hidden Windows cursor again:

```
FUN_00418900([0x00EE47B4]+0xA0 >> 1, [0x00EE47B4]+0xA4 >> 1)   ; 0x00419BC3-0x00419BE0
  -> FUN_004cc920(x, y): ClientToScreen([0x00EE4798], &pt); SetCursorPos(pt)   ; stdcall, ret 8
```

`[0x00EE47B4]+0xA0/+0xA4` is the GUI size, not the window size: 1365x768 at 3840x2160 (read live). So the cursor is parked at window pixel (682, 384), the top-left area at 4K, and that is where it becomes visible again. At 1366x768 those numbers were the middle of the screen, so the bug only shows above that resolution.

## Patch

The call at `0x00419BE0` (`E8` to `FUN_00418900`) calls `ParkCursor(x, y)` instead, stdcall like the original: it takes the window's client rectangle, parks the cursor at its middle (`ClientToScreen` + `SetCursorPos`) and ignores the GUI numbers. The movement is still measured from the parked position (`GetCursorPos` right after, as before), so mouse look is unchanged; the cursor only reappears in the middle of the screen.

## Verification

Harness (the game's `push`/`push`/`call` at `0x00419BD6-0x00419BE4`, a fake `FUN_00418900` with `ret 8`, a real window as `[0x00EE4798]`, `SetCursorPos` in the DLL's imports replaced by a recorder): before the DLL the original is called; after it the original is no longer called, `SetCursorPos` gets the client centre in screen coordinates, the stack is balanced, and with no window nothing is set. Verified in game 2026-10-20 at 3840x2160 (owner: the cursor comes back in the middle).
