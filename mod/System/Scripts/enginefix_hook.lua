-- Witcher engine fix: fast load/save list (Lua part of witcher-crash-lab\enginefix).
--
-- CListControl:AddListItem calls self:Refresh() after every item, and Refresh re-attaches every
-- item already in the list to the content panel. Building the save list is therefore quadratic
-- (531 saves took ~14 s, 51 saves ~0.25 s). While CGuiNewSystemLoadPanel:SetType fills the list,
-- items are added without the per-item Refresh, and each touched list is refreshed once at the end.
-- Every other list (journal, alchemy, ...) keeps the original behaviour.
--
-- Loaded by testtools.lua (add "enginefix_hook" to its HOOKS list). Set ENABLED = false to turn it off.

local ENABLED = true

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

    -- Appends one timing line to System\witcher_enginefix_lua.log (the game's working directory).
    local function LogTiming(sType, fFill, fRefresh)
        local file = io.open("witcher_enginefix_lua.log", "a")
        if file then
            file:write(string.format("%s save list (%s): fill %.0f ms, final refresh %.0f ms\n",
                os.date and os.date("%H:%M:%S") or "", tostring(sType), fFill * 1000, fRefresh * 1000))
            file:close()
        end
    end

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
        if clock then
            LogTiming(sType, t1 - t0, clock() - t1)
        end
        if not ok then
            error(err)
        end
    end
end
