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

## Area loading

3-5 s stretches on the main thread inside `d3dx9_35.dll` with no game frame on the stack. Which D3DX work it is cannot be told from export names (see the pitfall in AGENTS.md); Microsoft's public PDB for `d3dx9_35.dll` would settle it. Texture loads go through `FUN_0053f910` → `D3DXCreateTextureFromFileInMemoryEx` (MANAGED pool, all `D3DX_DEFAULT` sizes and filters). The AiO texture pack is all power-of-two DDS with full mip chains, so D3DX does no CPU filtering for it.

## Other observations

- At the 60 FPS cap the main thread is busy 40-50% of the time in Vizima and the swamp; the game is not CPU-bound in normal play.
- The engine is effectively single-threaded (main thread: logic, AI, all D3D calls). DXVK already moves submission to its own thread.
- `.bfx` files in `System\Scripts` are precompiled effects; `FUN_00525ae0` only compiles `.fx` when a newer source exists (flags `DAT_00c50148`/`DAT_00c50150`, both 0 in retail).
