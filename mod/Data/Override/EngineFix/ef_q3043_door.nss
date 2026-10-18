// Witcher Engine Fix: unlocks the kidnappers' hideout door in "The Source" (q3043_trisshan) when the
// game failed to. The game unlocks it with q3043_unlock when quest phase 5 (Shani at the hospital)
// completes. That script finds the doors by tag, so if the phase completes while the doors' area
// (the Temple Quarter, l19) is not loaded, nothing is unlocked and the door stays locked for good.
//
// Run by enginefix_hook.lua after every loading screen (runscript ef_q3043_door). It does nothing
// unless the quest is at "Dandellion shows the way" or "Walka z salamandrami" (the fight inside) and
// the doors are loaded and still locked; then it runs the game's own q3043_unlock.
//
// Compiled by tools\build_ef_ncs.py (no game needed). The game's compiler gives the same behaviour:
//   compile ..\Data\Override\EngineFix\ef_q3043_door.nss

void main()
{
    int nCur = GetQuestCurrentPhaseIndex("q3043_trisshan");
    int nWay = GetQuestPhaseIndex("q3043_trisshan", "", "Dandellion shows the way");
    int nFight = GetQuestPhaseIndex("q3043_trisshan", "", "Walka z salamandrami");
    if (nCur == nWay || nCur == nFight) {
        object oDoor1 = GetObjectByTag("q3043_sal_door1");
        object oDoor2 = GetObjectByTag("q3043_sal_door2");
        if ((GetLocked(oDoor1) || GetLocked(oDoor2)) && GetIsObjectValid(oDoor1)) {
            ExecuteScript("q3043_unlock", OBJECT_SELF);
            RunClientLua("EngineFix_KidnapDoorUnlocked()");
        }
    }
}
