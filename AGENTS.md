# AGENTS.md

Notes for agents and maintainers. User docs are in `README.md`, one page per fix in `docs/fixes/`, open leads in `docs/research.md`.

**Start here:** if `docs/local/handoff.md` exists (git-ignored, local only), read it first: it is the latest session handoff. If the sibling folder `..\witcher-crash-lab` exists, this is the owner's machine. Read its `AGENTS.md` and `notes/status.md` first: they hold the current state (what is installed in the game, what is waiting for an in-game test, the game path) and the crash/profiling workflow. Keep personal and machine-specific notes there, not in this repo. When a lab finding becomes a fix, document it here in `docs/fixes/` and update `docs/research.md`.

**Working with the owner:** they play while you investigate. Installing needs the game closed (the DLL is locked), so build and harness-test first, then ask them to close the game. Effects that need gameplay (a crash, a stutter, a menu) are verified from the logs after they report back.

## Project at a glance

An in-memory engine fix for The Witcher: Enhanced Edition 1.5 (Aurora/Odyssey-derived engine, 32-bit, MSVC 2005 runtime msvcr80/msvcp80, Lua 5.0 UI). It is a `dinput8.dll` proxy: the game imports `DINPUT8.dll` (`DirectInput8Create` only), so a DLL with that name in `System\` loads with the game. It forwards `DirectInput8Create` to the system DLL and patches the engine in memory. `d3d9.dll` is not used as the proxy because DXVK usually occupies it.

| Path | What |
|---|---|
| `src/enginefix.c` | All fixes, the patch driver and the diagnostics. Header comment lists every fix |
| `src/dinput8.def` | Exports `DirectInput8Create` undecorated |
| `mod/` | Mirrors the game folder: `System/witcher_enginefix.ini`, `System/Scripts/enginefix_hook.lua`, `System/Scripts/testtools.lua`, `Data/Override/EngineFix/` (NWScript fixes) |
| `build.bat` | Builds `build/dinput8.dll` and `build/sampler.exe` (MSVC x86; `VCVARS` overrides the vcvars32 path) |
| `tools/install.ps1` | Copies the build and `mod/` into the game (`-GamePath` or `WITCHER_DIR`), keeps an existing ini |
| `tools/package-release.ps1` | Builds and writes `dist/WitcherEngineFix-<version>.zip` (`System/` + `docs/README.txt` + LICENSE); version from `CHANGELOG.md`, must match `ENGINEFIX_VERSION` in `src/enginefix.c` |
| `tools/profiler/` | Sampling profiler and its analysis scripts (below) |
| `tools/luac50.py` | Lua 5.0 `.luc` disassembler; names closures by the field they are stored in. `python -I tools\luac50.py <file.luc> [function filter]` |
| `tools/savetoc.py` | Lists a `.TheWitcherSave` table of contents |
| `tools/witcherres.py` | Game resources: `keys`/`get` (KEY V1.1 + BIFF V1.1 in `Data\`), `erf`/`erfget` (module `.mod`, also a gunzipped save `.sav`), `gff` (dump), `find` (search GFF values) |
| `tools/ncsdis.py` | NCS disassembler; engine routines named from `System\Scripts\nwscriptdefn.nss` (routine = prototype index) |
| `tools/build_ef_ncs.py` | Hand-assembles `mod/Data/Override/EngineFix/ef_q3043_door.ncs` from the program in the script (`--check` verifies) |

## Patch driver rules

- **SteamStub:** Steam's `witcher.exe` is wrapped; the code section is decrypted at runtime, after our DLL loads. `PatchThread` polls until fix 1's original bytes are visible (= code decrypted), then applies everything once. `DirectInput8Create` also calls `TryPatch` (code is certainly decrypted by input init). Never launch `System\witcher.exe` directly ("Application load error 5:0000065434"); start through Steam.
- **Every patch verifies the exact original bytes** and logs `skipped (bytes differ)` otherwise. Keep that contract for new fixes.
- Each fix has an ini switch in `[fixes]` (`FixEnabled("fixN")`, default 1); diagnostics live in `[debug]` (`IniFlag("debug", ..., 0)`, off unless the ini turns them on; the owner's installed ini has them on).
- Logs: `System\witcher_enginefix.log` (DLL) and `System\witcher_enginefix_lua.log` (Lua hook). The game's working directory is `System\`.
- Call-site hooks: redirect a `CALL rel32` to generated code (`ApplySaveListTiming`) or to a C function with the same calling convention (`ThumbWriteCached`, cdecl). When two features touch one call site, the second must accept the first's target (see `g_tTarget`).
- msvcp80 `std::string` is passed as an object; call `c_str` through the msvcp80 export (thiscall, used as `__fastcall` with a dummy EDX). CRT `FILE*` belongs to msvcr80, so use its exports (`_fileno`, `_get_osfhandle`), never the DLL's own CRT.

## Build, test, install

1. `build.bat`
2. **Harness test before installing.** Patches target absolute addresses, so test programs reproduce the original bytes at those addresses: link the test exe with `/FIXED /BASE:0x400000` and a large static array (e.g. 9 MB) so the image covers `0x420000-0xD00000`, copy the original bytes there, `VirtualProtect` it executable, `LoadLibrary("dinput8.dll")`, then check the patched bytes or run small hand-assembled stubs at the call sites. For anything using the game's CRT, add `#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.VC80.CRT' version='8.0.50727.9680' processorArchitecture='x86' publicKeyToken='1fc8b3b9a1e18e3b'\"")` and load `msvcr80.dll`/`msvcp80.dll`.
3. Close the game, `powershell -ExecutionPolicy Bypass -File tools\install.ps1 -GamePath <game>`.
4. Launch through Steam and check the log for `applied`.

## Release

1. Bump `## x.y.z` in `CHANGELOG.md` and `ENGINEFIX_VERSION` in `src/enginefix.c`.
2. `powershell -ExecutionPolicy Bypass -File tools\package-release.ps1` (builds, then zips to `dist/`).
3. Tag `vx.y.z`, `gh release create` with the zip and notes in the style of the owner's other Witcher mods. Ask the owner before publishing.

## Lua side

- `startup.luc` runs `testtools` last (after all GUI classes, including `CListControl` and `gui_new_system_loadpanel`). Retail ships no `testtools`, so the owner's mods share a loader, `System\Scripts\testtools.lua`, which runs a fixed `HOOKS` list of `*_hook.lua` files and skips missing ones. This repo ships **loader version 2** (`mod/System/Scripts/testtools.lua`), which is version 1 with `enginefix_hook` added to `HOOKS`. witcher-speed-toggle (1.2.1) and witcher-quick-loot (1.0.2) ship the same byte-identical version 2; their older releases ship version 1, and installing one of those after this mod disables the Lua part. The DLL logs whether `testtools.lua` lists `enginefix_hook` (`CheckLuaLoader`). Lua 5.0 cannot list a directory, so a loader that discovers `*_hook.lua` by itself is not possible without the DLL.
- `CGuiInGame:PerformGUIActionsOnGameLoadingEnd` is called by the engine (`FUN_00417e50`) once each loading screen has finished, after area transitions and loaded saves. `enginefix_hook.lua` wraps it to run NWScript fixes with `console("runscript <name>")`; a script reports back with `RunClientLua`.
- Quest scripting: CDPR scripts compare `GetQuestCurrentPhaseIndex("<quest file>")` with `GetQuestPhaseIndex("<quest file>", "", "<phase Name>")`; phase names are the `Name` fields in the `.qst` (`witcherres.py gff`). `GetObjectByTag` only sees loaded areas, which is how `q3043_unlock` misses its doors.
- `makeClass` (in `global.luc`) uses an `__index` function that looks methods up in the class table at call time, so replacing class methods affects existing instances.
- Lua 5.0: varargs via `arg`, `pcall` returns status plus results, `os.clock`/`os.date` exist, the working directory is `System\`.

## Profiling and analysis

- `build\sampler.exe <pid> <seconds> <out.csv> [interval_ms]` suspends each recently active thread, records EIP plus an EBP-chain stack, and appends a module map (`#mod` lines). Build it 32-bit (a 32-bit sampler cannot read 64-bit thread contexts).
- `python -I tools\profiler\timeline.py <csv> <main_tid>`: busy % per 5 s and long busy stretches. The main thread is the process's first thread (earliest start time).
- `tools\profiler\report.py`: per-thread and per-function summary.
- `tools\profiler\dxsym.py <csv> <main_tid> <t0_ms>-<t1_ms> ...`: names d3dx9 time with Microsoft's public `d3dx9_35.pdb` through cdb (downloads it into `.\symbols` unless `--sym` points at a local copy; the owner's copy is in `..\witcher-crash-lab\symbols`).
- `functions.txt` is Ghidra's function list (name and address) for `witcher.exe` 1.5.0.1304; `*_exports.txt` are export tables (`dumpbin /exports`) for nearest-symbol lookups.
- **Pitfalls:**
  - `witcher.exe` is compiled with frame pointer omission, so stacks usually show only the leaf function, and samples in game code rarely have a caller.
  - **Nearest-export names lie for d3dx9/msvcr80**, because most of their code is not exported (this led to fix 3 and to blaming `_wopen`). Name D3DX time by the calling game function, and resolve system DLLs (`kernelbase`, `ntdll`, whose export tables are near complete) for the real API. Known: `ntdll+0x7879c` ZwCreateFile, `kernelbase+0x14e9f0` CreateFileW, `ntdll+0x7a08c` NtWaitForAlertByThreadId, `kernelbase+0x16a153` SleepConditionVariableSRW, `d3d9.dll(DXVK 2.7.1)+0x39d23` frame limiter wait.
  - Measure before fixing: `[debug] savelist_timing` showed the real split (90% Lua) where the profile alone pointed at file I/O.

## Ghidra

The owner's Ghidra project `test` has `/witcher1/witcher.exe.unpacked.exe` (SteamStub removed with Steamless). The ghidra-mcp plugin's HTTP API (`http://127.0.0.1:8089`, schema at `/mcp/schema`) can be called directly with curl when the MCP tools are not exposed, e.g. `/decompile_function?address=...`, `/disassemble_function?address=...`, `/get_xrefs_to?address=...`, `/get_function_by_address?address=...`, `/read_memory?address=...&length=...`, `/search_strings?search_term=...`, `/list_functions`.

## Environment pitfalls (Windows)

- In Git Bash, backslashes inside Python heredocs get mangled; write C or Python with backslashes through a file editor instead of `sed`/heredoc substitutions.
- Executables built in the temp folder run from PowerShell (`& path`), not reliably through `cmd /c`.
- `vcvars*.bat` may change the working directory; `cd /d %~dp0` after calling it.
