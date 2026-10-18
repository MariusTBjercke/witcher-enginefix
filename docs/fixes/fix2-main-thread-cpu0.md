# Fix 2: main thread pinned to CPU 0 (frame pacing)

## Cause

The engine timer constructor `FUN_007fdc80` (created via `FUN_007f6040`) runs on the main thread and does:

```
007fdc86  push 1
007fdc88  mov  [esi+0x2c], edi
007fdc8b  call [GetCurrentThread]
007fdc91  push eax
007fdc92  call [SetThreadAffinityMask]   ; SetThreadAffinityMask(GetCurrentThread(), 1)
```

The timer reads `RDTSC` (`FUN_007fd910`, calibrated against QPC in `FUN_007fdb50`). On 2007-era multi-core CPUs the TSC was not synchronised between cores, so pinning made timing stable. Modern CPUs have an invariant, synchronised TSC, so the pin only forces the whole main thread (game logic, AI and every D3D call) to share CPU 0 with the interrupt and DPC work Windows prefers to put there, and stops the scheduler from moving it to an idle fast core. On hybrid CPUs (P and E cores) it also takes the choice of core away from the OS.

Verified live before the fix: the initial thread (the main thread) had affinity `0x1`, every other thread the full mask.

## Patch

- `0x7FDC92`: `call [SetThreadAffinityMask]` → `add esp, 8` + 3 × `nop` (drops the two pushed arguments; the return value is unused).
- The main thread's affinity is also reset to the process mask once the code is patched, and again in `DirectInput8Create`, in case the timer was created before the patch landed.

Original bytes checked: `6A 01 89 7E 2C FF 15 5C F1 B3 00 50 FF 15 88 F2 B3 00`.

## Verification

Verified live: after the fix the main thread's affinity is the full mask (`fffff` on a 20-thread CPU). Expected benefit is smoother frame pacing, not higher average FPS; at a 60 FPS cap the main thread is idle 50-60% of the time in normal scenes.
