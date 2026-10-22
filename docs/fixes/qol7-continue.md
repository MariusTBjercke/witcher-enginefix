# QoL 7: "Continue" in the main menu

## What it does

With at least one save, the main menu gets a "Continue" button at the top that loads the newest save. The newest is the one with the latest save time, quicksaves and autosaves included. Without saves the menu is the game's own. `[qol] continue_button=0` turns it off. The label is in all ten text languages (`continue` in `TEXTS`, [texts.md](texts.md)).

## How the menu is built

- `gui_defs_mm_v2.luc` defines `g_aGuiDefinitions["MainMenuPanel"]`. Its buttons come from the local `mmDefineLabel(strRef, y)`: a `StandardButton` at X -206, size 412x90, a hot spot at (X+90, Y+30) of 232x40, font `gui_infopanel`, the main menu button sounds, layers `Clicked` (`ui_main_select_h`) and `Hilight`. New Game -2, Load Game -64, Options -130, Credits -196, Exit -358.
- `gui_mainmenu.luc` `CMainMenuPanel:new` builds the menu with `defineGUIPanel` from these definitions, and `OnPostAttachmentInitialize` sets each button's `OnClick` on `lm_pPanel.m_Controls[name]`.
- Load Game opens the system panel's load list (`CGuiNewSystemLoadPanel`). Its `SetType("LOAD")` calls `GetLoadSaveSystem():Lua_GetSavedGameList()`. The engine (`FUN_004c41a0`) then calls `CGuiMan:AddSaveGame(info)` for every save (`SGuiSaveGameInfo`: `m_szSaveName` "000722 - The Outskirts of Vizima-713", `m_szSaveFileName`, `m_fYear` ... `m_fSecond`), which goes on to the load list's `AddSaveGame`. That turns each into `{ ID, SaveName, SaveFileName }`. Loading is `GetLoadSaveSystem():LoadSavedGame(ID, SaveName, SaveFileName)`, then the system panel is closed.

## Change

`enginefix_hook.lua` wraps `CMainMenuPanel.new`. Before the menu is built:

- it asks the engine for the save list, with `CGuiMan.AddSaveGame` pointed for a moment at the load list's own `AddSaveGame` on a stand-in list (no GUI items), and keeps the save with the latest time stamp (ties: the higher ID);
- with a save, it adds a copy of New Game's definition with the label "Continue" at New Game's place, and moves New Game, Load Game, Options and Credits one slot down (Credits by the step above it, to -262). Exit stays at -358. Without one, the definitions are the game's own again;
- after the menu is built, the button's `OnClick` asks for the save list again (so a save deleted in the load list in the meantime does not count) and loads the newest save.

The menu logs `main menu: Continue button shown (newest save ...)` or `hidden`, with the time the save list took, and `main menu: Continue: loading ...` on a click (`witcher_enginefix_lua.log`).

## Verification

Mock test in Lua 5.1 with the real script (stand-ins for the menu, the save list and the load system): no button without saves; with saves the button is at -2 and the others at -64/-130/-196/-262 with Exit at -358, hot spots moved along; a click loads the save with the latest time stamp, not the highest ID; `CGuiMan.AddSaveGame` is restored; reopening the menu does not move the buttons twice; with the saves gone the menu is the game's own again; a skin reload (fresh definitions) is laid out again. Verified in game 2026-10-22 (owner): the button sits at the top with the others evenly spaced under it, and loaded the newest save (`main menu: Continue: loading 723 - The Outskirts of Vizima-714`, 21:33:35). With 715 saves the list takes about 0.75 s when the menu opens.
