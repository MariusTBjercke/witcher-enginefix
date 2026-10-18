# Fix 1: death effect with a missing creator (crash)

**Symptom:** access violation in `witcher.exe` at offset `0x579DF8` (address `0x00979DF8`), typically in big fights. Windows Event Log: `0xC0000005`, faulting module `witcher.exe`.

## Cause

`FUN_00979d50` applies a death effect (it logs `"Applying Death Effect to %x"`). It looks up the effect's creator by object ID:

```
FUN_008530d0(creatorId) -> FUN_0092f2c0   ; object-ID hash lookup, returns 0 if the object is gone or is not type 5 (creature)
```

The result (`EBX`) is used without a NULL check:

```
00979df8  mov eax, [ebx+0x938]   ; EBX == 0 when the creator was killed/despawned -> crash
00979dfe  cmp eax, [edi+4]
00979e01  jnz 0x979e12           ; "deleted death" exit
```

## Patch

`jmp` at `0x979DF8` to a trampoline:

```
test ebx, ebx
jz   0x00979E12        ; existing "deleted death" exit: the effect is dropped
mov  eax, [ebx+0x938]  ; original instruction
jmp  0x00979DFE
```

The `0x979E12` exit is the path the game already takes when the creator does not match, so a missing creator is treated as "not the right creator". Original bytes checked: `8B 83 38 09 00 00 3B 47 04`.

## Verification

Harness-tested (simulated code at the real addresses), then verified in game: the log shows `applied` every session and this crash has not recurred.
