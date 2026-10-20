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
--    A copy of that button, labelled "Exit game" in the game's text language (TEXTS below; the main menu's
--    StrRef 81 reads the same as "Exit to main menu" in French, German and Polish), goes right under it.
--    It asks the same "Are you sure you want to exit?" (1749) and then exits the way the main menu's Exit
--    button does (g_pClientExoApp:ExitProgram). In French, where the game's "Exit to main menu" reads only
--    "QUITTER", that button is relabelled "RETOUR AU MENU PRINCIPAL".
--
-- 4. "Borderless window" in Options > Graphics (QoL 3). A checkbox setting like the game's own (CCheckBoxSetting,
--    RegisterLuaSetting), greyed out while "Full screen" is ticked. The game stores it with its other settings
--    (registry value EngineFixBorderless); dinput8.dll reads it there a few times a second, so it applies at once.
--
-- 5. Quick loot (QoL 4, the same as the Witcher Quick Loot mod). A hotkey ([qol] quick_loot_key in
--    witcher_enginefix.ini, default Space) presses "Take all" in the loot window and does nothing in shops and
--    storage. A key bound with bindkey never reaches the game, so with Space it also does Space's normal pause
--    everywhere else. When Witcher Quick Loot is installed as well, it handles the key and this one stays unbound.
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

-- The mod's own texts in each text language of the game (ids from Data\languages.2da), UTF-8 like the game's
-- talk tables (dialog_<id>.tlk), which is what the GUI expects of a Lua string. Words follow the game's own
-- ("Full screen" = StrRef 1045). Every character must exist in that language's .tlk, or its font may lack it:
-- python -I tools\check_texts.py <game folder> checks that. Other languages get English.
local TEXTS = {
    [3]  = { -- English
        exit = "EXIT GAME",
        borderless = "Borderless window",
        borderlessTip = "With full screen off, the game fills the screen without a border.",
    },
    [5]  = { -- Polish ("OPUŚĆ GRĘ" is the game's own text for both exit buttons)
        exit = "WYJDŹ DO PULPITU",
        borderless = "Okno bez ramki",
        borderlessTip = "Gdy pełny ekran jest wyłączony, gra wypełnia ekran bez ramki.",
    },
    [10] = { -- German ("SPIEL VERLASSEN" is the game's own text for both)
        exit = "ZUM DESKTOP",
        borderless = "Rahmenloses Fenster",
        borderlessTip = "Ohne Vollbild füllt das Spiel den Bildschirm ohne Rahmen aus.",
    },
    [11] = { -- French ("QUITTER" is the game's own text for both, so "Exit to main menu" gets mainMenu; all from Illuzio on Nexus)
        exit = "QUITTER LE JEU",
        mainMenu = "RETOUR AU MENU PRINCIPAL",
        borderless = "Fenêtre sans bordure",
        borderlessTip = "Lorsque le mode plein écran est désactivé, le jeu occupe tout l'écran sans bordure.",
    },
    [12] = { -- Spanish ("SALIR DEL JUEGO" is the game's own text for "Exit to main menu")
        exit = "SALIR AL ESCRITORIO",
        borderless = "Ventana sin bordes",
        borderlessTip = "Sin pantalla completa, el juego ocupa toda la pantalla sin bordes.",
    },
    [13] = { -- Italian
        exit = "ESCI DAL GIOCO",
        borderless = "Finestra senza bordi",
        borderlessTip = "Senza la modalità a tutto schermo, il gioco riempie lo schermo senza bordi.",
    },
    [14] = { -- Russian
        exit = "ВЫХОД ИЗ ИГРЫ",
        borderless = "Окно без рамки",
        borderlessTip = "Без полноэкранного режима игра занимает весь экран без рамки.",
    },
    [15] = { -- Czech
        exit = "UKONČIT HRU",
        borderless = "Okno bez okraje",
        borderlessTip = "Bez celé obrazovky hra vyplní obrazovku bez okraje.",
    },
    [16] = { -- Hungarian ("KILÉPÉS A JÁTÉKBÓL" is the game's own text for "Exit to main menu")
        exit = "KILÉPÉS A WINDOWSBA",
        borderless = "Keret nélküli ablak",
        borderlessTip = "Teljes képernyő nélkül a játék keret nélkül tölti ki a képernyőt.",
    },
    [21] = { -- Traditional Chinese
        exit = "離開遊戲",
        borderless = "無邊界視窗",
        borderlessTip = "關閉全螢幕時，遊戲會以無邊界視窗佔滿整個螢幕。",
    },
}

-- One of the TEXTS in the game's current text language.
local function Text(sKey)
    local ok, nLanguage = pcall(function() return g_pClientExoApp:GetClientTextLanguage() end)
    local tTexts = ok and TEXTS[nLanguage] or TEXTS[3]
    return tTexts[sKey] or TEXTS[3][sKey]
end

-- Reads one [section] of System\witcher_enginefix.ini (dinput8.dll's settings) as { key = "value" }, keys in
-- lower case, values up to the first space or ";".
local function ReadIniSection(sSection)
    local tValues = {}
    local file = io.open("witcher_enginefix.ini", "r")
    if not file then return tValues end
    local sCurrent
    for line in file:lines() do
        local _, _, sName = string.find(line, "^%s*%[(.-)%]")
        if sName then
            sCurrent = string.lower(sName)
        elseif sCurrent == sSection then
            local _, _, sKey, sValue = string.find(line, "^%s*([%w_]+)%s*=%s*([^%s;]*)")
            if sKey and tValues[string.lower(sKey)] == nil then tValues[string.lower(sKey)] = sValue end
        end
    end
    file:close()
    return tValues
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
        -- A literal label instead of a StrRef (defineGUIPanel passes String.String to InitTextLayer as is).
        tDef.String.StrRef = nil
        tDef.String.String = Text("exit")
        tDef.Scripts = { OnClick = g_GuiLayoutManager:PerformButtonAction(QUIT) }
        tControls[QUIT] = tDef
        -- Where the game's own "Exit to main menu" reads like quitting (French "QUITTER"), it gets a clearer label.
        local sMainMenu = Text("mainMenu")
        if sMainMenu then
            tControls["ExitButton"].String.StrRef = nil
            tControls["ExitButton"].String.String = sMainMenu
        end
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
        -- The game's "Full screen" setting, looked up the way CGammaSetting:IsEnabled does.
        local function FullScreenSetting()
            local pManager = CHardwareDependantSettingsManager and CHardwareDependantSettingsManager:Instance()
            return pManager and pManager:GetSetting("CFullScreenSetting")
        end

        CEngineFixBorderlessSetting = makeClass(CCheckBoxSetting)
        CEngineFixBorderlessSetting.new = function(self)
            return CEngineFixBorderlessSetting:create()
        end
        CEngineFixBorderlessSetting.GetClassName = function(self) return "EngineFixBorderless" end
        CEngineFixBorderlessSetting.GetCategory = function(self) return "Graphics" end
        CEngineFixBorderlessSetting.GetDefaultValue = function(self) return 0 end
        -- No talk-table entry for these: the texts come from TEXTS (the game's own settings return
        -- g_TalkTable:GetSimpleString, the raw UTF-8 of the .tlk, through the same path).
        CEngineFixBorderlessSetting.GetSettingName = function(self) return Text("borderless") end
        CEngineFixBorderlessSetting.GetSettingTooltip = function(self)
            if g_tSettingsDescriptions then
                g_tSettingsDescriptions["EngineFixBorderless"] = Text("borderlessTip")
            end
            return "EngineFixBorderless"
        end
        -- Greyed out while "Full screen" is ticked (the panel asks again after every checkbox click).
        CEngineFixBorderlessSetting.IsEnabled = function(self)
            local okFS, pFS = pcall(FullScreenSetting)
            return not (okFS and pFS and pFS:GetValue() == 1)
        end
        CEngineFixBorderlessSetting.ApplyChanges = function(self)
            self:ApplyChangesInternal()
        end
        -- The panel discards unsaved changes of greyed-out settings each time it updates (UpdateSettings), so ticking
        -- "Full screen" would undo an untick of this box made a moment before. Keep the choice there; Cancel
        -- (RevertSettings) still discards it.
        local baseDiscard = CLuaSettingBase and CLuaSettingBase.DiscardChanges
        local inPanelUpdate = false
        if baseDiscard and CGuiNewSystemSettingsPanel and CGuiNewSystemSettingsPanel.UpdateSettings then
            local origUpdate = CGuiNewSystemSettingsPanel.UpdateSettings
            CGuiNewSystemSettingsPanel.UpdateSettings = function(self, ...)
                inPanelUpdate = true
                local okU, errU = pcall(origUpdate, self, unpack(arg))
                inPanelUpdate = false
                if not okU then error(errU) end
            end
            CEngineFixBorderlessSetting.DiscardChanges = function(self)
                if inPanelUpdate then return end
                return baseDiscard(self)
            end
        end
        local pSetting = CEngineFixBorderlessSetting:new()
        RegisterLuaSetting(pSetting)
        pSetting:Read() -- the game read its settings before this script ran

        -- dinput8.dll reads the stored option when the game styles the window for windowed mode. When OK applies
        -- "Full screen" off and "Borderless window" on together, "Full screen" comes first, so store ours before it.
        if CFullScreenSetting and CFullScreenSetting.ApplyChanges and WriteSettingIniEntry then
            local origApply = CFullScreenSetting.ApplyChanges
            CFullScreenSetting.ApplyChanges = function(self)
                pcall(WriteSettingIniEntry, "EngineFixBorderless", tostring(pSetting:GetValue()))
                return origApply(self)
            end
        else
            Log("options: Borderless window: Full screen setting not found; a switch to windowed may show a border briefly")
        end
    end)
    if not ok then Log("options: Borderless window not added: " .. tostring(err)) end
end

if ModHooks_BindScripts then
    local tQol = ReadIniSection("qol")
    local sKey = string.upper(tQol.quick_loot_key or "")
    if sKey == "" then sKey = "SPACE" end
    local bPause = sKey == "SPACE" -- any other key does nothing outside the loot window

    -- Called by Data\Override\EngineFix\ef_lootall.ncs whenever the hotkey is pressed.
    function EngineFix_QuickLoot()
        local gui = g_GuiInGame
        if not gui then return end

        -- Inventory open: Take all in loot mode; in shops and storage do nothing.
        local inventory = gui.lm_pNewInventoryPanel
        if inventory and inventory.lm_bActive then
            if inventory:IsLootOpen() and inventory.lm_pTransferPanel then
                inventory.lm_pTransferPanel:TransferAll()
            end
            return
        end

        -- Otherwise the same pause toggle as the game's own Space key. Unpausing always works; pausing is skipped
        -- while another panel (map, journal, ...) is open, like the vanilla key.
        if bPause and g_pClientExoApp and (gui.lm_bUserPause or not gui:IsAnyPanelOpen()) then
            g_pClientExoApp:TogglePauseRequest()
        end
    end

    -- Witcher Quick Loot binds the same thing to its own script (lootall) with its own settings. When it is
    -- installed, it handles the key, whichever hook runs first. Otherwise a "runscript lootall" line left in
    -- keybinding.ini by a removed Quick Loot would keep its key without function (no pause), so it is unbound.
    local file = io.open("Scripts\\quickloot_hook.lua", "r")
    local bQuickLootMod = file ~= nil or QuickLoot_TakeAllOrPause ~= nil
    if file then file:close() end
    if bQuickLootMod then
        ModHooks_BindScripts({ ef_lootall = false })
    elseif tQol.quick_loot == nil or (tonumber(tQol.quick_loot) or 0) ~= 0 then -- as dinput8.dll reads it
        ModHooks_BindScripts({ ef_lootall = sKey, lootall = false })
    else
        ModHooks_BindScripts({ ef_lootall = false, lootall = false })
    end
end
