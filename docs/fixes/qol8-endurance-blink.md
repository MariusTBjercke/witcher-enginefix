# QoL 8: the Endurance bar blinks red when an attack fails for lack of Endurance

`[qol] endurance_blink=1` (default). Visual only: what the attack does is unchanged.

## Background

A charged attack (hold the attack button until Geralt takes the charged pose, animation 152) costs Endurance. Before an attack lands, `FUN_0085ece0` (called from `FUN_00864680` and `FUN_00861880`) builds the attacker's modifiers (`FUN_008d10c0`) and subtracts the cost, block `+0x4DC` times `+0x4E0` (the block's copy is at `esp+0x14`, read as `[esp+0x4F0]` and `[esp+0x4F4]`; the 12 charge sequences carry 25 and 1.0, so a charged attack costs 25 of 100 Endurance; in game every refusal on 2026-10-25 23:16-23:20 had 6.4 to 24.3 Endurance), from the current Endurance (stats `vfunc+0x2C`, `GetCurrentEndurancePoints`). If the result is below 0 it ends the attack sequence (`FUN_008a45f0(0)` at `0x0085EDD6`) and the attack is gone. Nothing is shown: the charge looks like it did nothing. On 2026-10-25, 6 of 9 full charges in the chapter 5 battle ended this way ([research.md](../research.md), "Combat lock").

The game has a red blink of the Endurance bar, `CGuiInGame:TriggerEnduranceRedBlinkBlink` (`gui.luc`, which forwards it to the status panel). `FUN_00466fe0` calls it through the engine's Lua call helper at `0x00C500C8`: `FUN_0051cd70(helper, gui, "CGuiInGame")` (thiscall, ret 8) selects the object, `FUN_0051ce10(helper, "TriggerEnduranceRedBlinkBlink")` (thiscall, ret 4) calls the method, with the GUI object at `[[app+0x10]+0x80]` (`FUN_00403890`, app = `[[0x00EE47C0]]`).

The Endurance the check uses is stats `vfunc+0x2C(0)` (`0x00772BE0`): current `+0x1AC` plus temporary `+0x1A8`. The HUD bar shows that same value against the maximum `FUN_00773cd0(stats, 0)` = (`+0x1A4` + `+0x2FC`) × `+0x300` (40 in the dumps of 2026-10-25), not against 100 (`+0x1B8` = 100 is something else). So with a maximum of 40 a charged attack needs more than 60 % of the bar. On 2026-10-26 11:20 the owner saw a full bar when the check read 19.1 and 22.0; the log line now records the maximum too.

## Change

The `call FUN_008a45f0` at `0x0085EDD6` in the "cannot pay" branch goes to a function that ends the sequence as before and then, if the creature is the player and was still in a sequence (the other way into that branch is "no longer in a sequence"), makes the same two calls with the game's own strings (`0x00B425C8`, `0x00B48CE4`). The branch's 15 bytes and both strings are checked first. Each blink is logged: `qol 8: attack failed for lack of Endurance, Endurance bar blinked` (at most 200 lines).

## Message (opt-in)

`[qol] endurance_message=1` (off by default) also writes, in the game's text language, "Not enough Endurance (have/needed)", e.g. "Not enough Endurance (19/25)", in the message line (the one Witcher Speed Toggle uses). The first version spelled it out ("... this attack needs 25, you have 19."); the owner found it too long. The cost is not always 25 (the talent "Endurance4 Upgrade2" multiplies it by 0.75), so the numbers come from the DLL: before the call it sets the Lua globals `EngineFix_EnduranceNeeded` (cost) and `EngineFix_EnduranceHave` with the game's Lua 5.0 API (`lua_pushstring` `0x00A5D350`, `lua_pushnumber` `0x00A5D2F0`, `lua_settable` `0x00A5D6E0` with `LUA_GLOBALSINDEX` -10001; all cdecl; `L` = `[[0x00C500C8+0x18]+4]`, the helper's script object), then calls `CGuiInGame:EngineFixLowEndurance()` through the same helper as the blink. `Scripts\enginefix_hook.lua` defines that method: it rounds the Endurance down and the cost up (the check failed only when the Endurance was below the cost, so they never read the same) and calls `self:ShowGameInfo("feedback", text)`. "Endurance" is each language's word from the game (StrRef 67: Energia, Ausdauer, Endurance, Aguante, Resistenza, Энергия, Energie, Állóképesség, 耐力); `tools\check_texts.py` passes. The first bytes of the three Lua functions are checked; if they differ, only the message is left out. Both settings can be used on their own.

## Verification

Harness (fixed-base test exe, the branch's original bytes and the strings at their addresses, the Lua helpers and the rest of `FUN_008a45f0` stood in for): the call is redirected; for the player in a sequence the sequence is ended and then the helper gets the GUI object with `"CGuiInGame"` and the method `"TriggerEnduranceRedBlinkBlink"`; no blink outside a sequence, for an NPC, or without a GUI object (no crash). With the message on (Lua API stood in for by recorders): blink, then `EngineFix_EnduranceNeeded` = 50 and `EngineFix_EnduranceHave` = 40 set in the globals table of the helper's `L`, then `EngineFixLowEndurance` called; nothing in the three cases above. The Lua method itself was not run outside the game (no Lua 5.0 interpreter here).

In game: the blink seen 2026-10-25 22:49 and after (the owner: "it blinks quite faithfully when there is not enough"). The message: installed 23:35, not yet seen.
