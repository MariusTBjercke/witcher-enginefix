WITCHER ENGINE FIX
Crash and performance fixes for The Witcher: Enhanced Edition (patch 1.5)
https://github.com/MariusTBjercke/witcher-enginefix
=========================================================================

A small DLL that the game loads by itself and that corrects specific engine bugs
in memory while the game runs. witcher.exe and your saves are never modified.

What it fixes:
- Crash in big fights: a death effect firing after the creature that caused it
  is already gone.
- Slow load/save menu with many saves (531 saves: 14 s -> under 1 s).
- Faster saving and loading: the save checksum is 4x faster, with an identical
  result, so saves stay compatible with the unmodified game.
- The main thread is no longer forced onto CPU core 0 (a 2007-era workaround).
- Hair no longer behaves differently above 60 FPS.


INSTALLATION
------------
1. Copy the "System" folder into your game folder, e.g.
     ...\steamapps\common\The Witcher Enhanced Edition\
   (merge with the existing folder).
2. Start the game as usual.

To check that it works, open System\witcher_enginefix.log after starting the
game. Each fix should say "applied".

Files installed:
  System\dinput8.dll                  the engine fix (loaded by the game)
  System\witcher_enginefix.ini        settings
  System\Scripts\enginefix_hook.lua   the load/save menu fix
  System\Scripts\testtools.lua        shared startup loader (see COMPATIBILITY)


CONFIGURATION
-------------
System\witcher_enginefix.ini: 1 = on, 0 = off. Restart the game after a change.
The menu fix's Lua part is switched off with "local ENABLED = false" at the top
of System\Scripts\enginefix_hook.lua.


COMPATIBILITY
-------------
- If System already has a dinput8.dll from another mod, do not overwrite it:
  only one of the two can be loaded. Please open an issue instead.
- DXVK and texture packs work alongside it.
- Witcher Speed Toggle and Witcher Quick Loot share System\Scripts\testtools.lua
  with this mod. This mod ships loader version 2, which runs all three. If
  another mod puts back version 1, the menu fix stops working and the log says
  "Lua save-list fix: INACTIVE". Copy testtools.lua from this mod back.
- Tested on Steam 1.5.0.1304 (Windows 11, DXVK). GOG is the same game build but
  untested. On a different build a fix skips itself and logs why.
- The game is usually capped at 60 FPS. This mod fixes the known hair problem
  above 60 FPS; other high-frame-rate problems have not been ruled out yet.


UNINSTALL
---------
Delete System\dinput8.dll, System\witcher_enginefix.ini,
System\witcher_enginefix*.log and System\Scripts\enginefix_hook.lua. Delete
System\Scripts\testtools.lua too, unless Speed Toggle or Quick Loot is installed.
The thumbnail cache in %LOCALAPPDATA%\The Witcher\enginefix_thumbs is safe to
delete.


PROBLEMS
--------
Open an issue on the GitHub page and attach System\witcher_enginefix.log.
