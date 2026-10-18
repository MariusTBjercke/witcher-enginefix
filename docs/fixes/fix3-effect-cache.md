# Fix 3: shader effect cache keep-alive (experimental, off)

Materials share D3DX effects through a name-keyed cache: `FUN_00525ae0` (get/load) and `FUN_00524db0` (release). When the last user releases an effect, the cache destroys it and the next use loads it again (from the shipped `.bfx` files in `System\Scripts`).

Patch (both or neither, otherwise effects leak):

- `0x524DD5` release: `jnz 0x524E91` → `jmp 0x524E91` (never destroy at refcount 0)
- `0x525C12` lookup: `jz` → `nop nop` (reuse an entry even at refcount 0)

## Why it is off

It was written from a misread profile. Time in `d3dx9_35.dll` was attributed to `D3DXCreateEffect` by nearest exported symbol, but `ID3DXEffect` methods are not exported. Attributed by caller instead, the steady d3dx9 time is `ID3DXEffect::BeginPass` / `CommitChanges` (vtable `+0x100` / `+0x104`, called from `FUN_0051fc00` / `FUN_0051fc50`), which is normal per-draw work. With the fix on, the share did not change. There is no evidence of effect churn, so the fix stays in the code but defaults to `fix3=0`.
