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
- Faster area loading: the mipmaps the game built on the CPU at every loading
  screen (menus, fog of war) are built once and cached, with byte-identical
  results. About 1 s less per area transition once an area has been visited.
- Shorter wait after loading screens (the texture loader was checked only every
  250 ms).
- The first load/save menu after a PC restart no longer opens slowly: the saves
  are read once in the background while the game starts.
- The main thread is no longer forced onto CPU core 0 (a 2007-era workaround).
- Hair no longer behaves differently above 60 FPS.
- "The Source" (Chapter III): the door to Alvin's kidnappers no longer stays
  locked, also in saves that are already stuck there.


INSTALLATION
------------
1. Copy the "System" and "Data" folders into your game folder, e.g.
     ...\steamapps\common\The Witcher Enhanced Edition\
   (merge with the existing folders).
2. Start the game as usual.

To check that it works, open System\witcher_enginefix.log after starting the
game. Each fix should say "applied".

Files installed:
  System\dinput8.dll                  the engine fix (loaded by the game)
  System\witcher_enginefix.ini        settings
  System\Scripts\enginefix_hook.lua   the load/save menu and "The Source" door fixes
  Data\Override\EngineFix\           the "The Source" door script
  System\Scripts\testtools.lua        shared startup loader (see COMPATIBILITY)


CONFIGURATION
-------------
System\witcher_enginefix.ini: 1 = on, 0 = off. Restart the game after a change.
The [debug] section holds diagnostics for development; leave them off.
In System\Scripts\enginefix_hook.lua, "local ENABLED = false" turns off the menu
fix's Lua part and "local KIDNAP_DOOR_FIX = false" the "The Source" door fix.


COMPATIBILITY
-------------
- If System already has a dinput8.dll from another mod, do not overwrite it:
  only one of the two can be loaded. Please open an issue instead.
- DXVK and texture packs work alongside it.
- The area loading cache needs a CPU with SSE4.2 (any CPU since about 2008);
  without it that fix skips itself and logs why.
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
Delete Data\Override\EngineFix, System\dinput8.dll, System\witcher_enginefix.ini,
System\witcher_enginefix*.log and System\Scripts\enginefix_hook.lua. Delete
System\Scripts\testtools.lua too, unless Speed Toggle or Quick Loot is installed.
The caches in %LOCALAPPDATA%\The Witcher\enginefix_thumbs and
%LOCALAPPDATA%\The Witcher\enginefix_mips are safe to delete at any time.


PROBLEMS
--------
Open an issue on the GitHub page and attach System\witcher_enginefix.log.
