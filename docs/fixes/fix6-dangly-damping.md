# Fix 6: frame-rate dependent hair damping

## Cause

Hair and other spring-hung parts are dangly nodes (`MdlNodeDanglyNode`). Their update `0x552B70` (vtable `0xB59170` slot `0x9C`; the node tree passes dt down every frame through `FUN_0052abd0`) runs one spring step per frame:

```
dt = min(dt, 0.05)
force = -min(|offset|, 0.1) * stiffness * offset      ; stiffness = [data+0xC0]
force.z += gravity_scale * -0.981                      ; [data+0xDC]
v += force * dt / mass                                 ; [data+0xC4]
v *= damping                                           ; [data+0xC8], once per frame
pos += v * dt
pos clamped to the node's limits, then pulled back to its rest length
```

Everything is scaled by dt except the damping, which is applied once per frame. With a damping of 0.9, the velocity keeps 0.9^60 of itself per second at 60 FPS and 0.9^72 at 72 FPS, so above 60 FPS hair is damped more and behaves differently. Below 60 FPS it is damped less. PCGamingWiki and forum threads report hair glitches above 60 FPS and recommend a 60 FPS cap. Whether this damping is the whole story behind those reports is not confirmed yet.

## Patch

At `0x5534FE`, `movss xmm0,[edi+0xC8]` (load the damping, 8 bytes) becomes `call stub` + 3 × `nop`. The stub saves xmm1-xmm7 and eax/ecx/edx, calls `DanglyDamping(damping, dt)` (dt is in xmm1 at that point), and returns the result in xmm0:

- `damping^(dt*60)` for 0 < damping < 1, which is the factor a 60 FPS frame of length dt would get;
- the original value otherwise (0, 1 or above, negative).

At 60 FPS the factor equals the original damping, so hair at 60 FPS is unchanged. Flags are not preserved; the next flag-dependent jump (`0x55361C`) follows a `comiss`. Original bytes checked: `F3 0F 10 87 C8 00 00 00 F3 0F 10 96 04 01 00 00`.

`[debug] dangly_log=1` logs the first five calls (damping, dt, result) to confirm that the hook runs in game.

## Verification

Harness (the original bytes at `0x5534FE`, then `ret`; dinput8.dll loaded; the site called with known edi/esi/xmm1):

| damping | dt | result | expected |
|---|---|---|---|
| 0.9 | 1/60 | 0.900000 | 0.9 |
| 0.9 | 1/120 | 0.948683 | √0.9 |
| 0.9 | 1/72 | 0.915944 | 0.9^(60/72) |
| 0.9 | 0.05 | 0.729000 | 0.9³ |
| 0.9 | 0 | 1.000000 | 1 |
| 1.0 / 0 / 1.2 / -0.5 | 1/120 | unchanged | unchanged |

xmm1-xmm7, eax/ecx/edx and esp were preserved in every case. Not yet tested in game.
