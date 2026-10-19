# Open research

Leads that are not fixes yet. Addresses are for Steam `witcher.exe` 1.5.0.1304 (image base `0x400000`, so Event Log fault offset + `0x400000` = Ghidra address).

## Crashes seen (Event Log, before fix 1)

| Offset | Address | Where | Notes |
|---|---|---|---|
| `0x579DF8` | `0x979DF8` | death effect | fixed (fix 1) |
| `0x5FFFE0` | `0x9FFFE0` | `FUN_009fffc0`, red-black-tree insert | walks a corrupt node → heap corruption or use-after-free elsewhere. Callers `009f4830`, `009f7c40`, `009f77c0`, `009f3a80`. Needs a full crash dump. |
| `0x392702` | `0x792702` | `FUN_007926c0` | reads `[ESI]` of an iterator with checked-iterator calls around it: stale iterator / container |
| `0x66670E` | `0xA6670E` | `FUN_00a665c0` | `[ESI+0x10]` then `[ESI+5]`: stale pointer |
| `0x1410AE` | `0x5410AE` | `FUN_00541090` | object-ID table indexed with `id & 0x7fffffff`: invalid ID or bad table |
| MSVCR80 `0x14A5D`, `0x14A7F` | | CRT | typical of heap corruption surfacing in the allocator |

Several of these look like one root cause (objects used after deletion). Memory is **not** the problem: the exe is already Large Address Aware and a 5-hour session peaked at 2.85 GB of 4 GB with no growth.

## Exit hang

Black screen and hang when quitting from the main menu; the process must be killed. No dump yet. Plan: capture a hang dump (ProcDump `-h`), find what the main thread waits on, then either skip that cleanup or add a watchdog that terminates the process N seconds after shutdown starts (settings are saved during shutdown, so skipping blindly is risky).

## Stalls in heavy combat (swamp)

For ~54 s the main thread waited in DXVK (`d3d9.dll+0x11f50f`, `SleepConditionVariableSRW`) and the NVIDIA driver instead of in the frame limiter (`d3d9.dll+0x39d23`); FPS was unstable until the player died and reloaded, after which it was smooth. Game-side callers seen: `IDirect3DDevice9::CreateTexture` (vtable `+0x5c`) at `0x53F02A` and `0x6E7045`, MANAGED pool. Leading hypothesis: DXVK shader/pipeline compilation for first-seen effects. Check with `dxvk.hud = fps,frametimes,memory,gpuload,compiler` in `dxvk.conf`. Note: OBS's implicit Vulkan layer injects `graphics-hook32.dll` into DXVK games even when OBS is not running.

## Autosave on area transitions

Each autosave spends ~0.3-0.5 s in zlib: byte-at-a-time CRC32 `FUN_00800010` (table at `0xBC6610`), `inflate_fast`-like `FUN_00803860`, deflate window code `FUN_007feaf0`. The CRC part is fixed (fix 5). Deflate is zlib 1.1.4; a lower compression level or a newer deflate could cut the rest, but changes the save bytes, so it needs care (the game must still read them, and saves must stay compatible with the unpatched game).

## Sleep polling waits

A static pass over `witcher.exe` for 2007-era habits found the usual suspects already fine (LAA, LFH on the process heap, D3D9 device flags `0x142` without `MULTITHREADED`, effects begun with `D3DXFX_DONOTSAVESTATE`, resource files opened with `FILE_FLAG_SEQUENTIAL_SCAN`, texture loader thread blocking on semaphores). What stands out is waiting by Sleep polling:

- **Texture wait after loads:** `module.luc` `OnLoaded` calls `AurWaitForAllTexturesToLoad`, which sets `0x00CBAE38`; on the next frame `FUN_00588460` calls `FUN_006e8170`, which processes finished textures and, while the loader's pending queue (`[0x00C58C3C]+0x28/+0x2C`, 0x94-byte items) is not empty, does `Sleep(250)` (`0x006E81DB`). Each load can lose up to 250 ms per round.
- **Resource wait:** `FUN_0081a7a0` polls `FUN_007f5b40` with `Sleep(5)` (`0x0081A8A0`) when the main thread needs a resource that is being loaded asynchronously.
- **Spinlock:** `FUN_00456040` (and inlined copies around `0x009D8000-0x009D9D00`) does `Sleep(1)` while `InterlockedExchange` fails. Without a raised timer resolution `Sleep(1)` lasts ~15.6 ms (seen in the harness); DXVK may raise it in game.
- Minor: the main loop runs four console commands as text every frame (`"g_nGetGOCalls 0"` etc.); microseconds, not worth a patch.

`[debug] wait_timing=1` measures these: it swaps the exe's `Sleep` import (`0x00B3F22C`) for a hook that sums requested and actual sleep per caller (return address, main thread or other) and logs one line per burst, and it wraps the texture wait (`0x005884A2`) to log its total time, the number of `Sleep(250)` rounds and when the pending queue actually emptied. Harness-tested; not measured in game yet. If the texture wait regularly sleeps on an empty queue, shortening that `Sleep(250)` is a one-byte fix.

## Area loading

3-5 s stretches on the main thread inside `d3dx9_35.dll` with no game frame on the stack. Which D3DX work it is cannot be told from export names (see the pitfall in AGENTS.md); Microsoft's public PDB for `d3dx9_35.dll` would settle it. Texture loads go through `FUN_0053f910` → `D3DXCreateTextureFromFileInMemoryEx` (MANAGED pool, all `D3DX_DEFAULT` sizes and filters). The AiO texture pack is all power-of-two DDS with full mip chains, so D3DX does no CPU filtering for it.

## Above 60 FPS

PCGamingWiki says high FPS causes "hair physics glitch and crashes at anytime, in addition to script issues" and recommends a 60 FPS cap. The crash and script claims trace back to an old 120 Hz report that we could not read; the hair reports (Abigail, the Reverend, Geralt; fixed by vsync or a ~58-60 cap) are consistent. Here the cap is DXVK's `d3d9.maxFrameRate = 60`. What the engine does with frame time (static analysis, not tested in game):

- **Timer:** 64-bit microsecond counter (`FUN_007fda90`/`FUN_007fdaf0`, RDTSC calibrated against QPC), so frame time is precise at any FPS. `g_cAuroraSettings.m_bStaticTimeFlow`/`m_nStaticTimeFlowFPS` (`0xC500E8`/`0xC500EC`) is a built-in fixed-time-per-frame mode (for cutscene recording), not a fix.
- **Rigid-body physics (Karma-style world):** frame-rate independent. `FUN_008377e0` adds dt (µs → s) to an accumulator and runs fixed steps of `1/m_fPhysicsStepsPerSecond` (`0xC50A30`; camera world `0xC50A34`, minigames `0xC50A38`), at most 15 per frame, with no interpolation. Above the step rate, physics objects move only on some frames. The default rate is set at runtime; read it from the live process.
- **Cloth (`MdlNodeCloth`, `m_bEnableClothySimulations`):** frame-rate independent. `FUN_005645d0` substeps at 100 Hz (dt clamped to 0.3 s), `FUN_00565980` at 200 Hz (dt clamped to 0.05 s). Above 100 FPS the 100 Hz cloth gets 0 steps on some frames (judder, not wrong motion).
- **Dangly nodes (`MdlNodeDanglyNode`, hair):** **frame-rate dependent damping.** The update `0x552B70` integrates force and position with dt (clamped to 0.05 s), but multiplies the velocity by the damping once per frame. An earlier note here said dt was never read; that was a misread of the stack offsets. **Fixed by fix 6** ([fix6-dangly-damping.md](fixes/fix6-dangly-damping.md)); still to be tested in game.
- **Game time (day/night, calendar):** frame-rate independent. The world timer `FUN_007556c0` adds the difference between absolute engine-clock readings (`FUN_007f60c0`, µs, 64-bit) each frame, so nothing is lost to rounding at any FPS. A time multiplier (`[timer+0x48]`, for slow motion) goes through a float conversion, at most ~1 µs per frame. Hours and minutes are derived from it (`FUN_00755880`: minutes-per-hour × 60000 ms).
- **Per-frame budgets:** `m_nPathFindingTimeSlicePerFrame`, `m_nPathFindingMaximumTimePerFrame`, `m_nPendingDecalsPerFrame`, `m_nNumTexturesToDownsamplePerFrame`. Higher FPS only means more of this work per second, not wrong behaviour.
- **Not yet checked:** camera smoothing, UI/Lua animations, NWScript timing. Combat timing is not traced in detail, but it is driven by the world timer and animation time (both time-based) and no per-frame counter turned up. Find these by playing with the cap raised (72 FPS on the owner's 75 Hz G-Sync screen).

Once fix 6 is tested in game, check whether the hair glitch reports are fully explained by the damping or whether more remains (e.g. jitter at very high FPS).

## Quest bugs

- **The Source, locked kidnappers' door:** fixed and verified in game (0.2.0, [the-source-door.md](fixes/the-source-door.md)). The same pattern, an unlock or spawn script that finds its objects with `GetObjectByTag` and runs while their area is not loaded, may explain other one-off "stuck quest" reports. `tools/witcherres.py find` over the extracted modules and `tools/ncsdis.py` on the quest's `OnCompleted` scripts are the way to check one.

## Other observations

- At the 60 FPS cap the main thread is busy 40-50% of the time in Vizima and the swamp; the game is not CPU-bound in normal play.
- The engine is effectively single-threaded (main thread: logic, AI, all D3D calls). DXVK already moves submission to its own thread.
- `.bfx` files in `System\Scripts` are precompiled effects; `FUN_00525ae0` only compiles `.fx` when a newer source exists (flags `DAT_00c50148`/`DAT_00c50150`, both 0 in retail).
