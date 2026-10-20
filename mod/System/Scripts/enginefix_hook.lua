-- Witcher Engine Fix: the Lua part of the fix.
--
-- 1. Fast lists. CListControl:AddListItem calls self:Refresh() after every item, and Refresh re-attaches
--    every item already in the list to the content panel, so filling a list is quadratic (531 saves
--    took ~14 s, 51 saves ~0.25 s). While CGuiNewSystemLoadPanel:SetType (load/save list) or
--    CGuiNewDiaryPanel:OnTabSelect (journal tabs) fills its lists, items are added without the per-item
--    Refresh, and each touched list is refreshed once at the end (for the journal also right before it
--    selects the last selected entry). Every other list keeps the original behaviour.
-- 2. Locked kidnappers' door in "The Source" (Chapter III). After every loading screen this runs
--    Data\Override\EngineFix\ef_q3043_door.ncs, which unlocks the door only when the quest is at
--    that step and the game failed to unlock it (see that script's .nss).
-- 3. "Exit game" in the pause menu (QoL). The in-game system panel (Esc) only had "Exit to main menu".
--    A copy of that button, labelled with the main menu's "Exit game" text (StrRef 81), goes right
--    under it. It asks the same "Are you sure you want to exit?" (1749) and then exits the way the main
--    menu's Exit button does (g_pClientExoApp:ExitProgram).
--
-- 4. "Borderless window" in Options > Graphics (QoL 3). A checkbox setting like the game's own (CCheckBoxSetting,
--    RegisterLuaSetting); the game stores it with its other settings (registry value EngineFixBorderless), and
--    dinput8.dll reads it at startup, so a change asks for a restart like "Full screen" does.
--
-- Run at startup by the shared loader testtools.lua.

local ENABLED = true           -- false turns the load/save list and journal fix off
local LOG_TIMING = false       -- true appends the list build time to System\witcher_enginefix_lua.log
local KIDNAP_DOOR_FIX = true   -- false turns the "The Source" door fix off
local PAUSE_EXIT_BUTTON = true -- false removes the "Exit game" button from the pause menu
local BORDERLESS_OPTION = true -- false removes "Borderless window" from Options > Graphics

-- Appends one line to System\witcher_enginefix_lua.log (the game's working directory).
local function Log(sText)
    local file = io.open("witcher_enginefix_lua.log", "a")
    if file then
        file:write((os.date and os.date("%H:%M:%S ") or "") .. sText .. "\n")
        file:close()
    end
end

if ENABLED and CListControl and CListControl.Refresh then
    local origAddListItem = CListControl.AddListItem
    local origRefresh = CListControl.Refresh
    local batching = 0
    local dirtyLists = {}
    local nBatched = 0 -- items added while batching (for LOG_TIMING)

    CListControl.AddListItem = function(self, nIndex, sName, tAdditionalIcons, sTooltipText, bNoWrap)
        if batching == 0 then
            return origAddListItem(self, nIndex, sName, tAdditionalIcons, sTooltipText, bNoWrap)
        end
        self:_AddListItem(nIndex, sName, tAdditionalIcons, sTooltipText, bNoWrap)
        dirtyLists[self] = true
        nBatched = nBatched + 1
    end

    CListControl.Refresh = function(self)
        dirtyLists[self] = nil
        return origRefresh(self)
    end

    local clock = os and os.clock

    local function RefreshDirty()
        local lists = dirtyLists
        dirtyLists = {}
        for list in pairs(lists) do
            list:Refresh()
        end
    end

    -- Runs fn with AddListItem batched, then refreshes every list it left dirty.
    local function Batched(sWhat, fn, ...)
        local t0 = clock and clock()
        if batching == 0 then nBatched = 0 end
        batching = batching + 1
        local ok, err = pcall(fn, unpack(arg))
        batching = batching - 1
        local t1 = clock and clock()
        if batching == 0 then RefreshDirty() end
        if LOG_TIMING and clock then
            Log(string.format("%s: %d items, fill %.0f ms, final refresh %.0f ms", sWhat, nBatched, (t1 - t0) * 1000,
                (clock() - t1) * 1000))
        end
        if not ok then
            error(err)
        end
    end

    -- Load/save list: one item per save.
    if CGuiNewSystemLoadPanel and CGuiNewSystemLoadPanel.SetType then
        local origSetType = CGuiNewSystemLoadPanel.SetType
        CGuiNewSystemLoadPanel.SetType = function(self, sType)
            Batched("save list (" .. tostring(sType) .. ")", origSetType, self, sType)
        end
    end

    -- Journal: each tab (quests, characters, bestiary, ...) rebuilds its list, one item per entry.
    if CGuiNewDiaryPanel and CGuiNewDiaryPanel.OnTabSelect then
        local origOnTabSelect = CGuiNewDiaryPanel.OnTabSelect
        CGuiNewDiaryPanel.OnTabSelect = function(self, nTab, bSwitch)
            Batched("journal tab " .. tostring(nTab), origOnTabSelect, self, nTab, bSwitch)
        end
        -- OnTabSelect ends by selecting the entry that was selected last; the list is laid out first.
        if CGuiNewDiaryPanel.ClickItem then
            local origClickItem = CGuiNewDiaryPanel.ClickItem
            CGuiNewDiaryPanel.ClickItem = function(self, ...)
                if batching > 0 then RefreshDirty() end
                return origClickItem(self, unpack(arg))
            end
        end
    end
end

if KIDNAP_DOOR_FIX and CGuiInGame and CGuiInGame.PerformGUIActionsOnGameLoadingEnd and console then
    local origLoadingEnd = CGuiInGame.PerformGUIActionsOnGameLoadingEnd

    -- The engine calls this once each loading screen (area transition or loaded save) has finished.
    CGuiInGame.PerformGUIActionsOnGameLoadingEnd = function(self)
        origLoadingEnd(self)
        pcall(console, "runscript ef_q3043_door")
    end

    -- Called from ef_q3043_door.ncs through RunClientLua when it had to unlock the door.
    function EngineFix_KidnapDoorUnlocked()
        Log("The Source: the kidnappers' hideout door was still locked; unlocked it")
    end
end

if PAUSE_EXIT_BUTTON and CSkinLoader and CSkinLoader.Load and CGuiNewSystemPanel and createOKCancelPanel then
    local QUIT = "EngineFixExitGameButton"
    local STEP = -66 -- the buttons above are 62-66 units apart

    local function Copy(t)
        if type(t) ~= "table" then return t end
        local c = {}
        for k, v in pairs(t) do c[k] = Copy(v) end
        return c
    end

    -- Same button as "Exit to main menu" (gui_defs_sys_v2.luc sysDefineLabel), one step lower. The
    -- definitions are (re)built by g_SkinLoader:Load (gui_defs_*_v2), which runs after this script.
    local function AddButtonDef()
        local tSystemDefs = g_aGuiDefinitions and g_aGuiDefinitions["SystemPanel"]
        local tControls = tSystemDefs and tSystemDefs.Controls
        if not (tControls and tControls["ExitButton"] and g_GuiLayoutManager) or tControls[QUIT] then return end
        local tDef = Copy(tControls["ExitButton"])
        tDef.Position.Y = tDef.Position.Y + STEP
        if tDef.HotSpot and tDef.HotSpot.Position then
            tDef.HotSpot.Position.Y = tDef.HotSpot.Position.Y + STEP
        end
        tDef.String.StrRef = "GuiLMMExit"
        tDef.Scripts = { OnClick = g_GuiLayoutManager:PerformButtonAction(QUIT) }
        tControls[QUIT] = tDef
    end

    local origLoad = CSkinLoader.Load
    CSkinLoader.Load = function(self)
        origLoad(self)
        local ok, err = pcall(AddButtonDef)
        if not ok then Log("pause menu: Exit game button not defined: " .. tostring(err)) end
    end
    pcall(AddButtonDef) -- in case the skin is already loaded

    local function ExitGame()
        Log("pause menu: Exit game")
        pcall(PlayGuiSound, "gui_quit", 0)
        if g_pClientExoApp then
            g_pClientExoApp:ExitProgram(0)
        end
    end

    local origNew = CGuiNewSystemPanel.new
    CGuiNewSystemPanel.new = function(...)
        local self = origNew(unpack(arg))
        local ok, err = pcall(function()
            local pButton = self.lm_pPanel:GetControl(QUIT)
            if not pButton then Log("pause menu: Exit game button missing from the panel"); return end
            pButton:PlayAnimation("Selected", "Hide", false, 0)
            self.lm_tButtons[QUIT] = pButton
            g_GuiLayoutManager:SetButtonAction(QUIT, function()
                createOKCancelPanel("GuiLExitToMainMenu", nil, function()
                    self:ToggleOff()
                    ExitGame()
                end, self, nil)
            end)
        end)
        if not ok then Log("pause menu: Exit game button not added: " .. tostring(err)) end
        return self
    end

    -- Enabled exactly when "Exit to main menu" is: in game, not when the panel is opened from the main menu.
    local origUnToggleOff = CGuiNewSystemPanel.UnToggleOff
    CGuiNewSystemPanel.UnToggleOff = function(self, sStartingPanel, bEndGame)
        origUnToggleOff(self, sStartingPanel, bEndGame)
        local pButton = self.lm_tButtons and self.lm_tButtons[QUIT]
        if pButton then
            if g_GuiInGame then pButton:EnableButton() else pButton:DisableButton() end
        end
    end
elseif PAUSE_EXIT_BUTTON then
    Log("pause menu: Exit game button not added (CSkinLoader, CGuiNewSystemPanel or createOKCancelPanel missing)")
end

if BORDERLESS_OPTION and makeClass and CCheckBoxSetting and RegisterLuaSetting then
    local ok, err = pcall(function()
        CEngineFixBorderlessSetting = makeClass(CCheckBoxSetting)
        CEngineFixBorderlessSetting.new = function(self)
            return CEngineFixBorderlessSetting:create()
        end
        CEngineFixBorderlessSetting.GetClassName = function(self) return "EngineFixBorderless" end
        CEngineFixBorderlessSetting.GetCategory = function(self) return "Graphics" end
        CEngineFixBorderlessSetting.GetDefaultValue = function(self) return 0 end
        -- No talk-table entry for this one; English only. It applies when "Full screen" is off.
        CEngineFixBorderlessSetting.GetSettingName = function(self) return "Borderless window (when not full screen)" end
        CEngineFixBorderlessSetting.ApplyChanges = function(self)
            self:ApplyChangesInternal()
        end
        CEngineFixBorderlessSetting.NeedsRestart = function(self)
            return self.lm_ChangedValue ~= nil and self.lm_ChangedValue ~= self.lm_Value
        end
        local pSetting = CEngineFixBorderlessSetting:new()
        RegisterLuaSetting(pSetting)
        pSetting:Read() -- the game read its settings before this script ran
    end)
    if not ok then Log("options: Borderless window not added: " .. tostring(err)) end
end
