-- Shared startup loader (loader version 3) used by Witcher Speed Toggle, Witcher Quick Loot and
-- Witcher Engine Fix. Version 2 added enginefix_hook; hooks that are not installed are skipped, so it
-- also works with just one of the mods. Version 3 keeps bindkey hotkeys working after the video mode
-- changes (see ModHooks_ReplayKeybindings). Keep the newest version.
--
-- startup.luc always loads "testtools", but the retail game ships without it, so this file runs
-- on every launch. It runs the hook file of each installed mod (System\Scripts\<name>.lua).
-- Settings live in those hook files, not here.

local HOOKS = { "speedtoggle_hook", "quickloot_hook", "enginefix_hook" }

-- Binds each script in tScripts ({ scriptname = "KEY" or false }) to its key with the engine's
-- bindkey command. The engine saves every binding to System\keybinding.ini and reloads it on
-- startup, so keys these scripts were bound to before but no longer are get unbound first.
function ModHooks_BindScripts(tScripts)
    local file = io.open("keybinding.ini", "r")
    if file then
        local stale = {}
        for line in file:lines() do
            local _, _, key, script = string.find(line, "^(%S+)%s+runscript%s+(%S+)")
            local wanted = key and tScripts[script]
            if wanted ~= nil and (not wanted or string.upper(key) ~= string.upper(wanted)) then
                table.insert(stale, key)
            end
        end
        file:close()

        for _, key in ipairs(stale) do
            console("bindkey " .. key)
        end
    end

    for script, key in pairs(tScripts) do
        if key then
            console("bindkey " .. key .. " runscript " .. script)
        end
    end
end

-- Runs every line of System\keybinding.ini through bindkey again. bindkey registers its key with the
-- engine's input manager, and the engine replaces that manager whenever it changes the video mode
-- (full screen on/off, resolution), then re-registers only the game's own keys. Every bindkey hotkey
-- is dead from then on. OnReInitAuroraEnd (hwdepsettings) runs right after each such change, so the
-- manager is always new here. Losing focus in exclusive full screen (Alt+Tab) without DXVK replaces
-- the manager too, but calls no Lua; that case is not covered here.
function ModHooks_ReplayKeybindings()
    local file = io.open("keybinding.ini", "r")
    if not file then return end
    local tLines = {}
    for line in file:lines() do
        local _, _, sLine = string.find(line, "^%s*(.-)%s*$")
        if sLine and sLine ~= "" then
            table.insert(tLines, sLine)
        end
    end
    file:close()

    for _, sLine in ipairs(tLines) do
        console("bindkey " .. sLine)
    end
end

if OnReInitAuroraEnd and not ModHooks_OrigOnReInitAuroraEnd then
    ModHooks_OrigOnReInitAuroraEnd = OnReInitAuroraEnd
    OnReInitAuroraEnd = function(...)
        local result = ModHooks_OrigOnReInitAuroraEnd(unpack(arg))
        pcall(ModHooks_ReplayKeybindings)
        return result
    end
end

-- Enables the developer console (` character). Called by a hook whose ENABLE_CONSOLE is true.
function ModHooks_EnableConsole()
    g_cAuroraSettings.m_nDebugMode = 1
end

for _, name in ipairs(HOOKS) do
    local file = io.open("Scripts\\" .. name .. ".lua", "r")
    if file then
        file:close()
        g_Lua:PlayFile(name)
    end
end
