# QoL 4: quick loot (Space = Take all)

The same feature as the [Witcher Quick Loot](https://github.com/MariusTBjercke/witcher-quick-loot) mod (1.0.2), shipped with this mod. Its `AGENTS.md` holds the reverse-engineering notes; the short version is below.

## Behaviour

| Where | Space |
|---|---|
| Loot window | "Take all" (the button's own function) |
| Shop or storage | nothing, as before (it can never buy a shop's whole stock) |
| Anywhere else | pause / unpause, like the game's own Space; not while another panel (map, journal, ...) is open |

## How it works

- `enginefix_hook.lua` binds the key with the engine's `bindkey` console command to `runscript ef_lootall` (`ModHooks_BindScripts` from the shared loader). The engine saves macros to `System\keybinding.ini` at once and replays that file at startup.
- `Data\Override\EngineFix\ef_lootall.ncs` is one line, `RunClientLua("EngineFix_QuickLoot()")` (built by `tools\build_ef_ncs.py`, the same layout as Quick Loot's `lootall.ncs`).
- `EngineFix_QuickLoot()`: when `g_GuiInGame.lm_pNewInventoryPanel` is active and `IsLootOpen()`, it calls `lm_pTransferPanel:TransferAll()` (what the Take all button ends up calling); in another inventory mode it does nothing. Otherwise, with Space, `g_pClientExoApp:TogglePauseRequest()`, the toggle the game's Space reaches, when the game is user-paused or `not g_GuiInGame:IsAnyPanelOpen()`.
- A bound key is swallowed: the macro lookup (`FUN_0041ea20`, called first by the input handler `FUN_004262f0`) returns 1 and the key never reaches `FUN_00423bd0`, where Space's pause is hard-wired. That is why the hotkey performs the pause itself. Any other key gets only Take all, because its normal action is lost either way.

## Settings

`[qol]` in `witcher_enginefix.ini`, read by the Lua hook (the DLL only logs the state):

- `quick_loot=0` turns it off; the `ef_lootall` binding is removed on the next start.
- `quick_loot_key=<engine key name>` (default `SPACE`). A key that is changed here is unbound on the next start. Key names: Quick Loot's README.

## Together with Witcher Quick Loot

Both can be installed, in any order:

- If `System\Scripts\quickloot_hook.lua` exists (or `QuickLoot_TakeAllOrPause` is already defined), Quick Loot handles the key with its own script (`lootall`) and settings, and this hook only removes its own `ef_lootall` bindings. `ModHooks_BindScripts` reads `keybinding.ini` after Quick Loot's hook has rewritten it (loader order: Speed Toggle, Quick Loot, Engine Fix), so Quick Loot's Space binding is never touched. If the order were reversed, Quick Loot would simply rebind Space afterwards.
- Without Quick Loot, a `runscript lootall` line left in `keybinding.ini` by a removed Quick Loot is unbound (its script and Lua function are gone, so it would keep the key dead, without pause), and Space is bound to `ef_lootall`.
- The two scripts have different names, so no resource is duplicated under `Data\` (`OVERRIDE=..\Data` loads every loose file there).

The DLL log says which case applies: `qol 4 (quick loot hotkey): SPACE, bound by ...`, `... Witcher Quick Loot is installed ...` or `... disabled in ini`.

## Uninstall

The binding lives in `System\keybinding.ini` (`SPACE runscript ef_lootall`). Removing the mod without removing that line leaves Space without pause. Set `quick_loot=0` and start the game once before uninstalling, or delete the line (or the whole file, which only holds key macros).

## Verification

Stub-tested with Lua 5.1 and the real loader (bindkey emulated, 7 scenarios: alone, leftover Quick Loot binding, both installed with and without old bindings, off, another key) and a DLL load test of the log lines. In game: pending.
