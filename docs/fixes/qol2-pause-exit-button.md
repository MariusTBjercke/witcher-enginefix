# QoL 2: "Exit game" in the pause menu

## Cause

The in-game system panel (Esc) offers Save, Load, Options, "Exit to main menu" and Resume. Quitting the game means exiting to the main menu (a loading screen) and then pressing Exit there.

How the panel is built (Steam 1.5.0.1304, `System\Scripts`):

- `gui_defs_sys_v2.luc` defines `g_aGuiDefinitions["SystemPanel"]`. Its buttons come from the local `sysDefineLabel(strRef, name, y)`: a `StandardButton` at X -206, size 412x90, a hot spot at (X+90, Y+30) of 232x40, the `gui_infopanel` font, the main menu button sounds and textures, and `Scripts.OnClick = g_GuiLayoutManager:PerformButtonAction(name)`. Save -2, Load -64, Options -130, "Exit to main menu" (`ExitButton`, `GuiBSystemExit`) -196, Resume -358.
- `gui_new_systempanel.luc` `CGuiNewSystemPanel:new` builds the panel with `defineGUIPanel`, hides each button's `Selected` layer and binds each name with `g_GuiLayoutManager:SetButtonAction`. The "Exit to main menu" action is `createOKCancelPanel("GuiLExitToMainMenu", nil, onOK, self, nil)`, onOK = `self:ToggleOff()` + `g_GuiInGame:ShutdownToMainMenu()`. `UnToggleOff` enables that button only when `g_GuiInGame` exists. The panel is created by `CGuiInGame:OnInitialize` and by the main menu, both after startup.
- The main menu's Exit (`gui_mainmenu.luc` `_ExitProgram`) plays `gui_quit` and calls `g_pClientExoApp:ExitProgram(0)` (after deleting the main menu panel).
- Texts (`Data\Scripts\datadescriptions.lua`, `Data\dialog_*.tlk`): `GuiLMMExit` = 81 "EXIT GAME", `GuiBSystemExit` = 2392 "EXIT TO MAIN MENU", `GuiLExitToMainMenu` = 1749 "Are you sure you want to exit?". All languages have them, but in French, German and Polish 81 and 2392 are the same text, so since 0.6.0 the button has its own label per language ([texts.md](texts.md)).

## Change

`enginefix_hook.lua` (run by `testtools.lua` near the end of startup, before any system panel exists):

- `g_aGuiDefinitions` is not there yet when the hook runs: `startup.luc` calls `g_SkinLoader:ReadDefaults()` and `g_SkinLoader:Load()` (`CSkinLoader.luc`, which plays `gui_defs*_v2`) right after `testtools`, and the skin can be loaded again later. So `CSkinLoader.Load` is wrapped, and after every load it copies the `ExitButton` definition to `EngineFixExitGameButton`, 66 units lower (Y -262, between "Exit to main menu" and Resume), with `StrRef = "GuiLMMExit"` (since 0.6.0: `StrRef = nil` and `String` = the label in the current text language) and its own `PerformButtonAction` name;
- wraps `CGuiNewSystemPanel.new`: after the original, hides the new button's `Selected` layer and binds it to the same `createOKCancelPanel("GuiLExitToMainMenu", ...)`, whose OK closes the panel and calls `g_pClientExoApp:ExitProgram(0)`;
- wraps `UnToggleOff` so the button is enabled exactly when "Exit to main menu" is (in game).

`local PAUSE_EXIT_BUTTON = false` in the script turns it off. Pressing it logs `pause menu: Exit game` to `witcher_enginefix_lua.log`; a failure to add the button is logged too.

## Verification

First in-game try: no button, because the definition was added at hook time, before the skin was loaded. Fixed by adding it after `CSkinLoader.Load`. Verified in game 2026-10-20: the button sits under "Exit to main menu", asks for confirmation and quits; the process was gone 5 s after the click (`pause menu: Exit game` at 11:21:51, exit at 11:21:56), no hang.

## Exit crash (found 2026-10-23, fix not yet verified in game)

The first version of this button called `ExitProgram(0)` with the system panel still registered, which crashed the engine during shutdown. The player never saw it (the process was already exiting), but it filled the Event Log.

Evidence:

- Four WER full dumps (`witcher-crash-lab\logs\dumps\wer`, all 2026-10-23) are shutdown crashes, two signatures: `0x4CF360` = `FUN_004cf340` with `this = NULL`, failing on the first access `[this+0x164]`; `0x53943F` = `FUN_005393c0` (the script/console command dispatcher, `sgrp_`/`hidewfx_`/`voice:`) with a garbage `this`, writing into the command list. Both run from `msvcr80!exit` after `UnloadArea()`.
- Event Log, 120 days, 37 `witcher.exe` faults: the exit family (`0x4CF360` 15, `0x4AC7D6` 5, `0x13943F` 5, `0x1BD6A0` 2) is 27 of them, and **none exists before 2026-10-20 11:21** — the first time the button was pressed.
- 18 of 18 `pause menu: Exit game` lines in `witcher_enginefix_lua.log` are followed 1-2 s later by one of those fault events.
- `gui_mainmenu.luc` `_ExitProgram` does `g_pMainMenuPanel.lm_pPanel:Deactivate()`, `g_pMainMenuPanel:delete()`, `g_pMainMenuPanel = nil` **before** `g_pClientExoApp:ExitProgram(0)`. This button skipped all three.

Change (first attempt, **did not help**): `ExitGame(pPanel)` tears the panel down (`lm_pPanel:Deactivate()`, `self:delete()` from `gui_new_systempanel.luc`, then clears `g_pGuiMan.lm_pInGameNewSystemPanel`) before `ExitProgram(0)`, mirroring the main menu. Tested in game 2026-10-23 21:43: `pause menu: Exit game` at 21:43:01, fault `0x13943F` at 21:43:02. The panel was not the problem.

Where it stands (2026-10-23): the remaining difference from the main menu is that we quit with an area loaded. The fault is in `FUN_005752a0`, the `CAurObject` destructor, which dispatches the `Dying` script event through `FUN_005393c0` while the script system is already gone (`this` is garbage, `[this+4] = 0x2FC3DCA8`). Quitting from the main menu, with no area loaded, has produced none of these faults in 120 days of Event Log. So the engine's shutdown cannot cope with an in-game exit — which is presumably why the game itself only offers "Exit to main menu" while playing. Options: route the button through `ShutdownToMainMenu()` and quit once the main menu is up (no crash, but a loading screen), or accept an invisible crash and document it.

