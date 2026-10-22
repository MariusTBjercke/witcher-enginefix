# QoL 6: Windows key combinations and Print Screen in game

## Cause

While the game is in front, Win+Shift+S (Snipping Tool), Print Screen (when Windows opens the Snipping Tool with it) and every other Windows key combination did nothing, in full screen and borderless alike.

The game's DirectInput keyboard (`FUN_004cc720`: `GUID_SysKeyboard`, `c_dfDIKeyboard`, buffer of 256) is set up with `SetCooperativeLevel(top-level window, 0x16)` = `DISCL_NONEXCLUSIVE | DISCL_FOREGROUND | DISCL_NOWINKEY` (`push 0x16` at `0x004CC7C4`). `DISCL_NOWINKEY` blocks the Windows key while the game is in front. A test window with the same setup blocked both Win+Shift+S and Print Screen; with `0x06` both opened the Snipping Tool. The game's exclusive DirectInput mouse (`DISCL_EXCLUSIVE | DISCL_FOREGROUND`) blocked neither.

## Change

- `push 0x16` becomes `push 0x06`: the same keyboard without `DISCL_NOWINKEY`. The game reads the keyboard as before.
- Without the block, a Windows key pressed and released on its own would open the Start menu in the middle of the game. A thread checks the key (`GetAsyncKeyState`, every 5 ms) while the game is in front; as soon as it goes down it presses and releases the unassigned key `0xE8` (AutoHotkey's menu mask key). The shell then takes the Windows key's release as the end of a combination and leaves the Start menu closed. Combinations work as usual, since the Windows key is still held.
- A low-level keyboard hook (the usual way) does not work here: while the game's DirectInput keyboard is acquired, a hook in the game's process saw none of the keys (tested with a window set up like the game).

`[qol] windows_key=0` turns it off (the game blocks the Windows key again). The first masked press of a session is logged (`qol 6: Windows key pressed in game, masked`).

Side effect: every Windows key combination works in game, including ones that leave it (Win+D, Win+Tab), like alt-tab does.

## Verification

Harness (a window set up like the game: DirectInput keyboard `0x06` and exclusive mouse, keys sent with `SendInput` 60 ms apart, `push 0x16` at `0x004CC7C4` patched to `0x06`): a lone Windows key opened the Start menu without the DLL and left the window in front with it (twice); Win+Shift+S and Print Screen opened the Snipping Tool. Verified in game 2026-10-22 21:27-21:34 (owner): Win+Shift+S and Print Screen open the Snipping Tool over the game, and a lone Windows key does not open the Start menu (`qol 6: Windows key pressed in game, masked`).
