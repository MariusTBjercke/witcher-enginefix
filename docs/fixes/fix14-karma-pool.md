# Fix 14: physics objects freed twice (crash at `0x9FFFE0`)

Status: installed 2026-10-25, harness-tested, **not verified in game yet**. The crash is rare (hours apart), so it takes a few sessions with the log to know.

## Cause

The game's physics is MathEngine Karma. Its fixed-size pools (`MePoolx`) are called through the function table `MePoolAPI` at `0x00C31308` (in `.data`): create `FUN_009fd810`, destroy `FUN_009fd8a0`, reset `FUN_009fd7e0`, get `FUN_009fd8e0`, put `FUN_009fd900`, used count, free count, all stdcall. A pool is `+4` blocks, `+8` pointer stack, `+0xC` used count, `+0x10` block size, `+0x14` capacity; get returns `stack[used++]`, put does `stack[--used] = block`. Nothing checks that a block being put is in use, so a block freed twice sits twice on the free part of the stack and is later handed to two owners.

The crash dump of 2026-10-25 02:28 (combat in Vizima, chapter 5) shows exactly that state in the world's constraint pool (`world+0x12C`, blocks of `0x210` bytes for joints, contact groups and contacts): 14 live joints and contact groups, still linked into the world's constraint dictionary (`world+0x34`), were on the free part of the stack. The crash itself was a new contact group (`FUN_00a0ec50` → `FUN_009f85a0`) handed the block of a live joint, the root of that dictionary; `FUN_009f77c0` zeroed it and the dictionary insert (`FUN_009fffc0`) walked into the zeroed child pointer. The same crash also happened on 2026-10-16, before this mod existed. Background in [research.md](../research.md), "Karma constraint pool".

Which caller frees twice is not in the dump; the fix logs it.

## Patch

The table's destroy, reset, get and put entries point to wrappers that keep a free bit per block (one bitmap per pool, set up from the pool's own stack the first time the pool is used):

- **Put of a block that is already free:** skipped and logged. The second copy never reaches the stack.
- **Queue before reuse:** a freed block waits in a queue (up to 256 blocks, at most an eighth of the pool, none for pools under 64 blocks) before it goes back on the stack. A block freed a second time a little later is then still free and caught; without the queue it might already belong to a new owner, and that second free would look like a normal one. If the pool runs empty, the queue is handed back first.
- **Destroy of an object that is already free:** `MdtConstraintDestroy` (`0x009F7BC0`) and `MdtContactDestroy` (`0x009F7540`) write into the object (dictionary and contact list unlinking) before they free it, so their entry (`push esi; mov esi,[esp+8]`, 5 bytes) jumps to a check that returns at once when the block is free in its world's pool (constraint: world at `+0x40`; contact: group at `+0x68`, whose world is at `+0x40`). A contact without a group has been destroyed already (the original would read `[0x40]`) and is skipped too.
- **Get returning a block in use:** a stale copy from before the tracking started; the next block is taken.
- **Destroy and reset of a pool:** its tracking is dropped (queued blocks go with the pool's memory) and set up again on the next use.

The pool's stack and counts remain Karma's own, so `[fixes] fix14=0` leaves the game exactly as before. The original table values and both entries are checked before anything is written.

## Log

- `fix 14: tracking Karma pool <addr>: N blocks of S bytes, U in use` for the first 12 pools of a session (and any pool that already had stale entries).
- `fix 14: block freed twice, skipped: ...`, `constraint destroyed twice`, `contact destroyed twice`, `free list handed out a block in use`: the block, its pool, the value at `+0xB0` (the constraint type: 0, 1, 9 joints, 5 contact group, -1 destroyed), the caller and further code addresses found on the stack (likely, not certain, return addresses: `witcher.exe` has no frame pointers), and a running count. The first 40 events are logged in full, then every 100th.

Lines like these mean the fix stopped a corruption that would have crashed the game later. If the game still crashes at `0x9FFFE0` with none of them in the log, the second free came after the block had already been reused (longer than the queue), and the callers in the dump are the next lead.

## Verification

Harness (fixed-base test exe with the original Karma bytes from the dump at their addresses: the pool functions `0x009FD7E0-0x009FD940`, both destroy functions, `FUN_009f7d40`; the unlink calls stubbed):

- Without the DLL, a double put makes the pool hand out the same block twice (the bug).
- With it: a constraint destroyed twice and a block put twice are skipped and logged; a contact destroyed twice and a contact without a group are skipped; queued and live blocks are never handed out; an empty pool hands back its queue and every block exactly once; a stale copy on the free list at tracking start is passed over; a destroyed pool struct set up again is tracked from scratch.
- 200 000 random gets and puts on a 2000-block pool, with second frees of recent frees mixed in: no block handed out while in use.

In game: not yet.
