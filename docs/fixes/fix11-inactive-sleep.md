# Fix 11: up to 1 s late after coming back to the game

## Cause

While the game window is inactive (fullscreen without the active caption, or minimized; flag `0x00C2A324` = 0), the main loop (`FUN_007530d0`) sleeps 1 s per frame: `push 1000; call [Sleep]` at `0x007536B9`/`0x007536BE`. That is what keeps the game paused in the background. When the window comes back to the front, the main loop first finishes the Sleep it is in, so the game reacted up to 1 s late (0.5 s on average) after an alt-tab. It was most noticeable in the borderless window (QoL 3), which pauses in the background like fullscreen, next to the plain windowed mode, which does not sleep at all.

## Change

The `call [Sleep]` (6 bytes) becomes `call InactiveSleep; nop`. `InactiveSleep` waits the same 1000 ms in 10 ms steps and returns early as soon as the game window (`[0x00EE4798]`) is the foreground window and not minimized. In the background nothing changes: one frame per second, so the game stays paused.

## Verification

Harness: with no game window in front the call takes 1000 ms; with the game window in front it returns at once (0 ms).

In game, 2026-10-20 (`frame_log`, time from the game window getting the focus): 16 alt-tabs in the main menu and in Vizima, borderless and windowed. The next frame came 3-8 ms after the focus and was presented after 4-20 ms, about one frame, in both modes. Borderless still feels a little slower to switch than the framed window; that is outside the game: the borderless window covers the screen, so Windows presents it directly (with G-Sync), and an alt-tab switches the presentation mode, while the framed window (larger than the screen) is always composed.
