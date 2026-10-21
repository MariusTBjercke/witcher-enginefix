# QoL 5: skip the intro

`[qol] skip_intro=1` in `witcher_enginefix.ini`, off by default. The game starts at the main menu, without the logos and the intro cinematic.

## Background

The startup movies are cutscenes listed in the Lua script `Movies.lua` (resource `movies`, type 2101, in `scripts00.bif`): `cs_publisher`, `cs_developer`, `cs_engine`, `cs_intro`, `cs_title`, with `CS_BLOCKEND` markers between them. Its global `PlayIntro` plays them one after the other with `playCutscene`.

The engine calls `PlayIntro` once, at startup (Steam 1.5.0.1304):

- `FUN_00401020` (the game's main routine) runs `FUN_00752af0`, the engine setup. That function also runs `g_Lua.PlayFile("startup")`, so `startup.luc`, the shared loader `testtools.lua` and `enginefix_hook.lua` have run by the time it returns.
- Next it calls `FUN_004026b0` → `FUN_004045c0`, which calls the Lua function `PlayIntro` only if the byte at `+0x290` of the app object is 0.
- That byte is the engine's "don't play intro" flag. Lua sets it with `g_pClientExoApp:SetDontPlayIntro(bool)` (binding `FUN_00613120` → `FUN_004026a0` → `FUN_004045b0`). No retail script calls it.

The game also ships `Movies.lua` loose in `Data\Scripts\`, and the game uses that copy. The usual manual skip edits it down to `local Intro = { "CS_BLOCKEND" }`. That keeps working alongside this option: with `skip_intro=1`, `PlayIntro` is not called at all, so its contents do not matter.

`g_cAuroraSettings.m_bDontPlayMovies` is not a substitute: it turns off every movie, including the ones in the story.

## Change

`enginefix_hook.lua` reads `[qol] skip_intro`. When it is 1, it wraps the global `playCutscene` (a C binding that exists before the startup scripts run) until `cs_title`, the last name `PlayIntro` queues. Until then:

- the five intro cutscenes are dropped;
- the first `CS_BLOCKEND` marker is passed on, and the second is dropped.

The result is the same as the manual `{ "CS_BLOCKEND" }` edit of `Movies.lua`. It works whichever `Movies.lua` is installed, because `PlayIntro` looks up `playCutscene` when it runs. Nothing is patched in the exe. The DLL's log line `qol 5` says whether the option is on and whether the Lua part is loaded.

First attempt, discarded: `g_pClientExoApp:SetDontPlayIntro(true)` from the hook. The engine then skipped `PlayIntro`, but the game stayed on a black screen at 72 FPS: nothing opened the main menu. That flag is meant for starts where something else follows, such as loading a save. The menu comes from the end of the cutscene sequence, which then never ran.

## Verification

2026-10-21: `SetDontPlayIntro` gave a black screen (above). The `playCutscene` wrapper, with the original `Movies.lua` installed, goes straight to the main menu. A black window remains for about 1-2 s while the main menu loads: the frame log shows the first frame 0.9 s after the window comes up and the first finished `Present` after 1.9 s. With the manual `Movies.lua` edit the same wait was 1.3 s / 2.3 s, so that wait is the menu loading, not the intro.
