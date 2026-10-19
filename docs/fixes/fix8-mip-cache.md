# Fix 8: cache the mip levels D3DX builds on the CPU

## Cause

Area loads spent 3-5 s on the main thread inside `d3dx9_35.dll`. With Microsoft's public PDB (`tools/profiler/dxsym.py`) that time is 99% `D3DXCreateTextureFromFileInMemoryEx` → `D3DXFilterTexture` → `CBlt::BltBox2D` with the DXT codecs (`EncodeDXT5`/`EncodeBC5S`, `DecodeDXT1/5`): mip levels built on the CPU by decoding each level, box-filtering it and encoding it again.

The engine creates textures in `FUN_0053f910` (call at `0x0053FAC3`) from the resource's DDS with MipLevels `D3DX_DEFAULT` (full chain), Format `D3DFMT_FROM_FILE`, MANAGED. When the DDS has no mips, D3DX builds the whole chain. `[debug] texture_timing` on a Temple Quarter ↔ Trade Quarter transition:

```
textures: 1147 created in 2824.8 ms | 450 with levels generated: 1539.0 ms | 697 with all levels in the data: 1285.7 ms | ...
     72.6 ms  2048x1024 DXT5, header mips 0, 1 in data -> 12 levels 2048x1024 DXT5  ui_background_03:1224
     69.4 ms  2048x1024 DXT5, header mips 0, 1 in data -> 12 levels 2048x1024 DXT5  ui_main_back:1334
     43.6 ms  1024x1024 DXT5, header mips 0, 1 in data -> 11 levels 1024x1024 DXT5  foggl19:1165
```

Mostly GUI and fog-of-war textures, and the GUI is recreated on every load, so the same ~70 ms textures are rebuilt again and again.

## Patch

The call at `0x0053FAC3` (`E8` → thunk `0x00A750FE`) goes to `TexCreateHook`, which calls D3DX with the same arguments. For a DXT1/3/5 DDS that holds only its top level, power-of-two, with the game's usual arguments:

- **Key:** CRC32C (SSE4.2 instruction) and CRC32 (slicing-by-8) of the whole DDS, plus its size. Files: `%LOCALAPPDATA%\The Witcher\enginefix_mips\<d3dx9_35 PE timestamp>\<crc32c><crc32>_<size>.mip`, with a header that repeats the key, size, format and level count, and a CRC32C of the level data.
- **Miss:** D3DX builds the levels as before; afterwards levels 1..k-1 are read back with `LockRect(D3DLOCK_READONLY)` and written to a temp file, then renamed into place (no half-written cache files).
- **Hit:** the DDS is handed to D3DX with those levels appended (`dwMipMapCount = k`, `DDSD_MIPMAPCOUNT`, `DDSCAPS_MIPMAP | DDSCAPS_COMPLEX`), so D3DX copies them instead of filtering. Any mismatch (header, checksum, short read) is a miss.
- **k** is the last level with both sides ≥ 4. D3DX re-processes the partial DXT blocks of smaller levels when it loads them from a file (a first version that cached all levels differed in a few bytes of the 8x2..1x1 levels), so it still builds those itself from the last cached level, as it did the first time.

The cache lives on disk, not in the game's (32-bit) address space; Windows' file cache keeps repeats fast. A different `d3dx9_35.dll` build uses a different folder. Deleting the folder is always safe.

## Verification

Harness with the real `d3dx9_35.dll` and a D3D9 HAL device: four textures (`ui_scr_up` from `textures00.bif`, synthetic 2048x1024 DXT5, 512x256 DXT1, 256x1024 DXT3), each created directly, through the hook on a miss and through the hook on a hit; all levels read back and compared: **byte-identical** in all three ways. 2048x1024 DXT5: 140 ms direct, 3 ms from the warm cache (8 ms right after the file was written, when the antivirus scans it). A truncated cache file and one with a flipped byte in the level data were both rejected and rebuilt.

Still to verify in game: the `textures:` line on a second visit should show the generated textures as `levels from fix 8's cache`.
