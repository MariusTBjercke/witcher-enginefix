# QoL 2: "Exit game" in the pause menu

## Cause

The in-game system panel (Esc) offers Save, Load, Options, "Exit to main menu" and Resume. Quitting the game means exiting to the main menu (a loading screen) and then pressing Exit there.

How the panel is built (Steam 1.5.0.1304, `System\Scripts`):

- `gui_defs_sys_v2.luc` defines `g_aGuiDefinitions["SystemPanel"]`. Its buttons come from the local `sysDefineLabel(strRef, name, y)`: a `StandardButton` at X -206, size 412x90, a hot spot at (X+90, Y+30) of 232x40, the `gui_infopanel` font, the main menu button sounds and textures, and `Scripts.OnClick = g_GuiLayoutManager:PerformButtonAction(name)`. Save -2, Load -64, Options -130, "Exit to main menu" (`ExitButton`, `GuiBSystemExit`) -196, Resume -358.
- `gui_new_systempanel.luc` `CGuiNewSystemPanel:new` builds the panel with `defineGUIPanel`, hides each button's `Selected` layer and binds each name with `g_GuiLayoutManager:SetButtonAction`. The "Exit to main menu" action is `createOKCancelPanel("GuiLExitToMainMenu", nil, onOK, self, nil)`, onOK = `self:ToggleOff()` + `g_GuiInGame:ShutdownToMainMenu()`. `UnToggleOff` enables that button only when `g_GuiInGame` exists. The panel is created by `CGuiInGame:OnInitialize` and by the main menu, both after startup.
- The main menu's Exit (`gui_mainmenu.luc` `_ExitProgram`) plays `gui_quit` and calls `g_pClientExoApp:ExitProgram(0)` (after deleting the main menu panel).
- Texts (`Data\Scripts\datadescriptions.lua`, `Data\dialog_*.tlk`): `GuiLMMExit` = 81 "EXIT GAME", `GuiBSystemExit` = 2392 "EXIT TO MAIN MENU", `GuiLExitToMainMenu` = 1749 "Are you sure you want to exit?". All languages have them.

## Change

`enginefix_hook.lua` (run by `testtools.lua` near the end of startup, before any system panel exists):

- `g_aGuiDefinitions` is not there yet when the hook runs: `startup.luc` calls `g_SkinLoader:ReadDefaults()` and `g_SkinLoader:Load()` (`CSkinLoader.luc`, which plays `gui_defs*_v2`) right after `testtools`, and the skin can be loaded again later. So `CSkinLoader.Load` is wrapped, and after every load it the `ExitButton` definition to `EngineFixExitGameButton`, 66 units lower (Y -262, between "Exit to main menu" and Resume), with `StrRef = "GuiLMMExit"` and its own `PerformButtonAction` name;
- wraps `CGuiNewSystemPanel.new`: after the original, hides the new button's `Selected` layer and binds it to the same `createOKCancelPanel("GuiLExitToMainMenu", ...)`, whose OK closes the panel and calls `g_pClientExoApp:ExitProgram(0)`;
- wraps `UnToggleOff` so the button is enabled exactly when "Exit to main menu" is (in game).

`local PAUSE_EXIT_BUTTON = false` in the script turns it off. Pressing it logs `pause menu: Exit game` to `witcher_enginefix_lua.log`; a failure to add the button is logged too.

## Verification

First in-game try: no button, because the definition was added at hook time, before the skin was loaded. Fixed by adding it after `CSkinLoader.Load`; not yet tested in game again. To check: the button's position and look, the confirmation, that the game closes cleanly (and whether it hits the known exit hang), and the panel opened from the main menu (button disabled).
