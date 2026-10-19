# Load/save list: Lua batching + fix 4 (thumbnail rewrite)

Opening the load (or save) menu got slower the more saves there were: 51 saves took 0.26 s, 531 saves took 14.1 s.

## How the list is built

- `gui_new_system_loadpanel.luc` (`CGuiNewSystemLoadPanel:SetType`) calls `GetLoadSaveSystem():Lua_GetSavedGameList(...)`.
- Native wrapper `FUN_0063ef40` → `FUN_004c41a0` (CLoadSaveSystem::GetSavedGameList):
  - `FUN_00432990` → `FUN_00431420` enumerates the save folders (`"SAVES:%s"`).
  - Per save, `FUN_007824a0` → `FUN_008aaa40` opens the save with `_wfopen_s`, reads the trailer and table of contents (`FUN_008aa1e0`), finds the `.tga` entry and calls `FUN_008a8f40(FILE*, entry, "screens")`, which writes it to `%LOCALAPPDATA%\The Witcher\temp\saves\screens\<id>.tga`.
  - Per save, Lua `g_pGuiMan:AddSaveGame` → panel `AddSaveGame` → `CListControl:AddListItem`.

Save file format (`RGMH`): trailer = last 12 bytes `{?, toc_offset, count}` (in current saves the TOC is at the second field; `tools/savetoc.py` tries both); TOC entry = `u32 name_len, name, u32 size, u32 offset`. In memory a TOC entry is `{u32 size, u32 offset, std::string name}` (0x24 bytes, msvcp80). `tools/savetoc.py` lists a save's TOC.

## Measurements (531 saves, warm disk cache)

Timing hooks at the CALL sites (see AGENTS.md, `[debug] savelist_timing`):

| | total | Lua AddSaveGame | files |
|---|---|---|---|
| original | 14.1 s | 13.5 s | 0.23 s |
| + Lua batching | 2.5 s | 2.0 s | 0.23 s |
| + fix 4, later opens in a session | 0.84 s | 0.47 s | 0.12 s |
| + fix 4 cache, first open after a clean restart (533 saves) | 0.96 s | 0.53 s | 0.28 s |
| first open after a PC restart (575 saves), before fix 7 | 3.5-6.1 s | 0.64-0.76 s | 2.6-5.1 s |

## Cause 1: every row re-lays out every row (Lua fix)

`CListControl.luc`: `AddListItem` = `_AddListItem(...)` followed by `self:Refresh()`. `Refresh` re-attaches every existing item's button, text and icons to the content panel (`GuiAurAttachToObject`). Each re-attach removes the object from a child array by shifting the rest (`0x529f80` virtual method on `[this+0x3a8]`, `FUN_005bd680`), so building the list is far worse than linear. Those two shift loops were 65% of the time.

`mod/System/Scripts/enginefix_hook.lua` wraps `CGuiNewSystemLoadPanel.SetType`: while it runs, `CListControl.AddListItem` calls `_AddListItem` only and marks the list dirty; afterwards each dirty list gets one `Refresh`. Other lists (journal, alchemy, main menu) are untouched. `startup.luc` loads `CListControl` and the load panel before `testtools`, and `makeClass` resolves methods through the class table at call time, so overriding the class methods also covers already created instances. With `LOG_TIMING = true` at the top of the hook, timings go to `System\witcher_enginefix_lua.log`.

## Cause 3: cold files after a PC restart (fix 7)

The first list of a session after a PC restart (or, it seems, after an antivirus signature update) took 3.5-6 s with 575 saves, the later ones ~1 s. The drives are NVMe, so it is not the disk: every first open of a save cost ~4-8 ms (CreateFileW alone ~4 ms; Defender real-time protection scans the file again), and fix 4's reads of the cached thumbnails were cold too (write thumbnail step 2.4 s instead of 0.2 s).

Fix 7 starts a below-normal-priority thread at startup that does the same work once: for every `*.TheWitcherSave` in `Documents\The Witcher\saves` it opens the file, reads the trailer, the TOC and the thumbnail, and reads the cached thumbnail in `enginefix_thumbs\screens`. It only reads (share mode read/write/delete, so it never blocks the game), and it stops as soon as a save list starts building. Warm, 575 saves take ~70 ms; the log line `fix 7: warmed N saves ... slowest X ms` shows the cold cost on the first start after a restart. Harness-tested; to be verified on the first game start after a PC restart (expect the first list at ~1 s).
