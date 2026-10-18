# Fix 5: faster CRC32 for saves

## Cause

The engine links zlib **1.1.4** (`" deflate 1.1.4 Copyright 1995-2002 Jean-loup Gailly "`). Its `crc32()` is `FUN_00800010` (cdecl `uLong crc32(uLong crc, const Bytef *buf, uInt len)`, table at `0x00BC6610`, identical to the standard zlib table), which does one table lookup per byte. Saving runs it over the whole save; profiles of area transitions (autosave) showed it as the top function in those stretches, next to the deflate code (`FUN_007feaf0`, `FUN_00803860`). Callers are in the engine's zlib stream wrappers (`FUN_007fb3d0`, `FUN_007fb940`, `FUN_007fb570`, `FUN_007fb6b0`).

## Patch

A `jmp` at the function entry to `Crc32Hook`, which runs a slicing-by-8 CRC32 (8 KB of tables built at startup) with the same polynomial (`0xEDB88320`), the same pre/post inversion and the same `NULL` buffer behaviour (returns 0). Original bytes checked: `8B 4C 24 08 85 C9 75 03 33 C0 C3`.

A trampoline (the original's first 6 bytes, then `jmp 0x00800016`) keeps the original callable, so `fix5=0` together with `[debug] crc_timing=1` times the original crc32 for comparison.

`[debug] crc_timing=1` logs one line per burst of calls (one save), once the calls have stopped for a second:

```
crc32 (slicing-by-8): 5313 calls, 34.4 MB, 12.5 ms
```

## Verification

Harness with the game's own `crc32` machine code and table copied to their real addresses:

- 4800 cases (lengths 0-199, 8 alignments, 3 seeds), chained calls, and the `NULL` buffer: identical results.
- 32 MB: original 46-48 ms, slicing-by-8 11.2-11.5 ms (**4.1-4.3x**) on a Core Ultra 7 265KF.

In-game timing per autosave: pending (see the log after the next area transition).
