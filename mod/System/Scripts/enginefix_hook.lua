-- Witcher Engine Fix: the Lua part of the fix.
--
-- 1. Fast load/save list. CListControl:AddListItem calls self:Refresh() after every item, and
--    Refresh re-attaches every item already in the list to the content panel. Building the save
--    list is therefore quadratic (531 saves took ~14 s, 51 saves ~0.25 s). While
--    CGuiNewSystemLoadPanel:SetType fills the list, items are added without the per-item Refresh,
--    and each touched list is refreshed once at the end. Every other list keeps the original
--    behaviour.
-- 2. Locked kidnappers' door in "The Source" (Chapter III). After every loading screen this runs
--    Data\Override\EngineFix\ef_q3043_door.ncs, which unlocks the door only when the quest is at
--    that step and the game failed to unlock it (see that script's .nss).
--
-- Run at startup by the shared loader testtools.lua.

local ENABLED = true           -- false turns the load/save list fix off
local LOG_TIMING = false       -- true appends the list build time to System\witcher_enginefix_lua.log
local KIDNAP_DOOR_FIX = true   -- false turns the "The Source" door fix off

-- Appends one line to System\witcher_enginefix_lua.log (the game's working directory).
local function Log(sText)
    local file = io.open("witcher_enginefix_lua.log", "a")
    if file then
        file:write((os.date and os.date("%H:%M:%S ") or "") .. sText .. "\n")
        file:close()
    end
end

if ENABLED and CListControl and CGuiNewSystemLoadPanel then
    local origAddListItem = CListControl.AddListItem
    local origSetType = CGuiNewSystemLoadPanel.SetType
    local batching = false
    local dirtyLists = {}

    CListControl.AddListItem = function(self, nIndex, sName, tAdditionalIcons, sTooltipText, bNoWrap)
        if not batching then
            return origAddListItem(self, nIndex, sName, tAdditionalIcons, sTooltipText, bNoWrap)
        end
        self:_AddListItem(nIndex, sName, tAdditionalIcons, sTooltipText, bNoWrap)
        dirtyLists[self] = true
    end

    local clock = os and os.clock

    CGuiNewSystemLoadPanel.SetType = function(self, sType)
        local t0 = clock and clock()
        batching = true
        local ok, err = pcall(origSetType, self, sType)
        batching = false
        local t1 = clock and clock()
        local lists = dirtyLists
        dirtyLists = {}
        for list in pairs(lists) do
            list:Refresh()
        end
        if LOG_TIMING and clock then
            Log(string.format("save list (%s): fill %.0f ms, final refresh %.0f ms",
                tostring(sType), (t1 - t0) * 1000, (clock() - t1) * 1000))
        end
        if not ok then
            error(err)
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
