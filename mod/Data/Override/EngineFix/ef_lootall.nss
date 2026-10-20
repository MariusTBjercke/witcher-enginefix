// Witcher Engine Fix: quick loot hotkey (QoL 4). Bound to the key in [qol] quick_loot_key (default SPACE) by
// enginefix_hook.lua with the engine's bindkey command. The logic is in Lua (EngineFix_QuickLoot in
// System\Scripts\enginefix_hook.lua): Take all in the loot window, otherwise Space's normal pause.
//
// Compiled by tools\build_ef_ncs.py (no game needed). The game's compiler gives the same bytes:
//   compile ..\Data\Override\EngineFix\ef_lootall.nss

void main()
{
    RunClientLua("EngineFix_QuickLoot()");
}
