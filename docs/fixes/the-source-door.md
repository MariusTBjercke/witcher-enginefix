# Locked kidnappers' door in "The Source" (Chapter III)

## Symptom

Alvin is kidnapped from St. Lebioda's Hospital. Dandelion leads Geralt to the Salamandra hideout in Little Mahakam (opposite the dwarven smithy), tells him he will wait outside, and the quest marker points at the door, but the door is locked and the quest cannot continue. The fandom wiki blames using the teleporter in Triss's house; the Nexus "Kidnappers' Door Bugfix" (mods/706) works around it with a scroll and overrides of `def_arealoaded.ncs` and `custom_script.ncs`.

## Cause

Quest `q3043_trisshan` (resources from `main.key`, read with `tools/witcherres.py`):

| Phase | Name | Completes on |
|---|---|---|
| 5 | `Shani - znajdz dzieciaka!` | dialog `q3043_shani01`, node QP3 → `OnCompleted = q3043_unlock` |
| 6 | `Dandellion shows the way` | dialog `q3043_jaskier01`, node QP4 |
| 7 | `Walka z salamandrami` (the fight inside) | 4 × `q3043_sal01` killed |

`q3043_unlock.ncs` is the only thing that unlocks the hideout:

```
SetLocked(GetObjectByTag("q3043_sal_door1"), FALSE);
SetLocked(GetObjectByTag("q3043_sal_door2"), FALSE);
SetDialogFlagValue("q3043_jakierhospital", 1);
```

The two doors (`q3043_sal_door1`/`2`, `Locked = 1`, linked to `wp_l19_g01_05_from_l19`) belong to area `l19` (Temple Quarter, module `wyzimpoor_3`). `GetObjectByTag` only finds objects in loaded areas. If phase 5 completes while Geralt is somewhere else, for example in the Trade Quarter around Triss's house, the script finds no doors and the unlock is lost; nothing runs it again. Dandelion's walk (`q3043_tr01`–`tr03`, his `q3043_jaskier03` "wait outside" talk) does not depend on the doors, so the quest carries on to the locked door.

The owner's save `000584 - Temple Quarter` shows exactly this: phases 5 and 6 completed, current phase 7, both doors `Locked = 1` in the saved `l19.git`, all three Dandelion triggers already used.

## Fix

No game file is replaced.

- `Data\Override\EngineFix\ef_q3043_door.ncs` (source next to it, compiled by `tools/build_ef_ncs.py`): if the quest's current phase is "Dandellion shows the way" or "Walka z salamandrami" (looked up by name with `GetQuestPhaseIndex`), and the doors are loaded and either is still locked, it runs the game's own `q3043_unlock` and reports back with `RunClientLua("EngineFix_KidnapDoorUnlocked()")`.
- `enginefix_hook.lua` wraps `CGuiInGame:PerformGUIActionsOnGameLoadingEnd`, which the engine calls once a loading screen has finished (`FUN_00417e50`, when the loading flag at `+0x490` clears), and runs `console("runscript ef_q3043_door")` after the original. So the check runs after every area transition and every loaded save, and does nothing outside that quest step.
- `EngineFix_KidnapDoorUnlocked` writes `The Source: the kidnappers' hideout door was still locked; unlocked it` to `System\witcher_enginefix_lua.log`.

Switch: `local KIDNAP_DOOR_FIX = false` in `System\Scripts\enginefix_hook.lua`.

## Verification

- `tools/ncsdis.py` disassembly of the built script checked by hand (stack offsets, argument order as in the game's own scripts; routine numbers checked against `nwscriptdefn.nss`).
- In game (2026-10-19, Steam 1.5.0.1304): loading the stuck save `000584 - Temple Quarter` (phase 7, both doors locked) logged `The Source: the kidnappers' hideout door was still locked; unlocked it`, and the door opened to the hideout.
