# Fix 15: a charge attack released out of reach blocks every further attack

## Symptom

In a crowded fight (seen four times in the chapter 5 battle in Vizima, the Order against the Scoia'tael) Geralt suddenly cannot hit his enemy any more. Clicks do nothing or show the crossed-out sword, holding the button no longer charges an attack, and the enemy goes on fighting. Walking a step ends it.

## Cause

Holding the attack button runs the charge action, `aiactionattackpowerupload` (action 0x45, `FUN_008dd9f0`). Pressing attack during a fight ends the current combat round first (`calling ECR from CMessagePlayer_Input_Attack`), so while charging Geralt is in his attack sequence (`creature+0xC58` = 1) without a combat round.

When the button is released and the target is out of reach (the distance test at `0x008DDDC8`, typical when the enemy is pushed around by other fighters), the action cancels the attack (`FUN_008897b0`, `FUN_008d4c60`) and ends. It never ends the sequence. Geralt is then left in a sequence with no combat round, a state no other creature is in, and every further attack is cancelled at once: the client sends "attack failed" (message 0x28, `CMessagePlayer_Input_AttackFailed on server`) and the server cancels the attack again (`FUN_008d4c60` from `0x00970142`). Moving ends it because the move handler (`FUN_0096b480`) ends the sequence: `FUN_008a45f0(creature+0xC58, 1)` at `0x0096B655`.

Found with `[debug] combat_log` (2026-10-25 22:18): the player's attack was cancelled from `0x0088982F` with `0x008DDDD9` (this branch) on the stack, then he stayed in sequence 624 without a combat round for 15.4 s, every click cancelled, until `player's sequence ends (1) from 96B65A` (he moved) and `player out of the stale sequence`.

## Patch

The `call FUN_008d4c60` at `0x008DDDDB` (thiscall, no stack arguments) in that branch goes to a function that calls `FUN_008d4c60` and then, if the creature is the player and is still in a sequence without a combat round, ends the sequence the way the move handler does (`FUN_008a45f0(creature+0xC58, 1)`). The 19 bytes from `0x008DDDD2` are checked first. NPCs and every other path are unchanged. Each time it acts it logs `fix 15: charge attack released out of reach, ended the player's attack sequence N` (at most 200 lines).

### Second site: any other way into the stale state

The same state also came another way (2026-10-25 22:49): a full charge started its attack, then `AIActionAttackObject` (`FUN_00864ae0`) cancelled it over and over from `0x00865DD1` (its timer at `+0xDBC` ran out), and the sequence stayed; every click until 22:50:15 was refused. Wherever the state comes from, the clicks end in "attack failed" from the client, which the server handles in `FUN_0096fb70` (message 0x28) with `call FUN_008d4c60` at `0x0097013D` (ECX = the creature). That call now goes to a function that calls `FUN_008d4c60` and then ends the player's sequence the same way, if he is in one without a combat round and is not charging (no action 0x45 at the head of his queue, `creature+0xC4`/`+0xC8`). The click that finds the stale state fails as before; the next one works. Logged as `fix 15: attack refused while stuck in a stale attack sequence N, ended it (the next attack works)`. The 14 bytes from `0x0097013B` are checked first.

`[fixes] fix15=0` turns both off.

## Verification

Harness (fixed-base test exe, the branch's original bytes at their address, `FUN_008d4c60` and the rest of `FUN_008a45f0` stood in for): the call is redirected; the player in a sequence without a round has `FUN_008d4c60` run and the sequence ended with argument 1; with an active round, outside a sequence, or for an NPC only `FUN_008d4c60` runs. Second site: redirected; ended for the player in a stale sequence with an empty queue or another action at its head; not while charging (0x45 at the head), with an active round, or for an NPC.

In game: the first site installed 2026-10-25 22:27 and never fired in the sessions after it; the 22:49 lock came the second way. The second site installed 22:54, not yet seen in game.
