# Load/save list: Lua batching + fix 4 (thumbnail rewrite)

Opening the load (or save) menu got slower the more saves there were: 51 saves took 0.26 s, 531 saves took 14.1 s.

## How the list is built

- `gui_new_system_loadpanel.luc` (`CGuiNewSystemLoadPanel:SetType`) calls `GetLoadSaveSystem():Lua_GetSavedGameList(...)`.
- Native wrapper `FUN_0063ef40` → `FUN_004c41a0` (CLoadSaveSystem::GetSavedGameList):
  - `FUN_00432990` → `FUN_00431420` enumerates the save folders (`"SAVES:%s"`).
  - Per save, `FUN_007824a0` → `FUN_008aaa40` opens the save with `_wfopen_s`, reads the trailer and table of contents (`FUN_008aa1e0`), finds the `.tga` entry and calls `FUN_008a8f40(FILE*, entry, "screens")`, which writes it to `%LOCALAPPDATA%\The Witcher\temp\saves\screens\<id>.tga`.
  - Per save, Lua `g_pGuiMan:AddSaveGame` → panel `AddSaveGame` → `CListControl:AddListItem`.

Save file format (`RGMH`): trailer = last 12 bytes `{toc_offset, ?, count}`; TOC entry = `u32 name_len, name, u32 size, u32 offset`. In memory a TOC entry is `{u32 size, u32 offset, std::string name}` (0x24 bytes, msvcp80). `tools/savetoc.py` lists a save's TOC.

## Measurements (531 saves, warm disk cache)

Timing hooks at the CALL sites (see AGENTS.md, `[debug] savelist_timing`):

| | total | Lua AddSaveGame | files |
|---|---|---|---|
| original | 14.1 s | 13.5 s | 0.23 s |
| + Lua batching | 2.5 s | 2.0 s | 0.23 s |
| + fix 4, later opens in a session | 0.84 s | 0.47 s | 0.12 s |

## Cause 1: every row re-lays out every row (Lua fix)

`CListControl.luc`: `AddListItem` = `_AddListItem(...)` followed by `self:Refresh()`. `Refresh` re-attaches every existing item's button, text and icons to the content panel (`GuiAurAttachToObject`). Each re-attach removes the object from a child array by shifting the rest (`0x529f80` virtual method on `[this+0x3a8]`, `FUN_005bd680`), so building the list is far worse than linear. Those two shift loops were 65% of the time.

`mod/System/Scripts/enginefix_hook.lua` wraps `CGuiNewSystemLoadPanel.SetType`: while it runs, `CListControl.AddListItem` calls `_AddListItem` only and marks the list dirty; afterwards each dirty list gets one `Refresh`. Other lists (journal, alchemy, main menu) are untouched. `startup.luc` loads `CListControl` and the load panel before `testtools`, and `makeClass` resolves methods through the class table at call time, so overriding the class methods also covers already created instances. Timings go to `System\witcher_enginefix_lua.log`.

## Cause 2: thumbnails rewritten, then read back (fix 4)

Hooking msvcr80's `CreateFileW` import during the list build showed 2651 opens taking 1.59 s: per save, the save itself, the thumbnail write, then the GUI opens the freshly written `.tga` twice at ~1.5 ms each. Unchanged files open in ~0.03 ms. (A PowerShell benchmark of write-then-open did not reproduce the cost, so it is specific to the game process; a likely cause is a security scanner rescanning just-modified files.)

Fix 4 redirects the call at `0x8AACCA` (`call FUN_008a8f40`) to `ThumbWriteCached`:

1. Read the thumbnail bytes from the save (positional `ReadFile` on the CRT handle via msvcr80 `_fileno`/`_get_osfhandle`; the OS file pointer is restored).
2. If `temp\saves\screens\<id>.tga` already holds exactly those bytes, skip the write.
3. The game wipes `%LOCALAPPDATA%\The Witcher\temp` between sessions, so thumbnails are also kept in `%LOCALAPPDATA%\The Witcher\enginefix_thumbs\screens`. On the first open of a session an identical cached file is hard-linked into temp instead of being rewritten.
4. Otherwise call the original writer, then hard-link the cache to the new file.

Autosaves share one thumbnail name (`000003.tga`), so they still get rewritten each time (4 of 531 in the measurements), exactly like the original.

Status: steps 1-2 verified in game (527 of 531 skipped on the second open). Step 3 (hard links on the first open of a session) is harness-tested; whether it avoids the slow first open in the game is not measured yet.

## Ideas not done

- The remaining ~0.9 ms per row is GUI object creation (`DefineControl` for a button and a text label per row). A virtualised list that only creates visible rows would remove most of it, but needs `CScrollView` scroll callbacks.
- `FUN_0084b320` (text glyph quads) takes ~11% while a long list is on screen: every row is drawn, visible or not.
