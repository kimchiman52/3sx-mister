/**
 * @file pow_pow.c
 * Damage Calculation
 */

#include "sf33rd/Source/Game/engine/pow_pow.h"
#include "arcade/arcade_balance.h"
#include "common.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/pow_data.h"
#include "sf33rd/Source/Game/engine/workuser.h"

void cal_damage_vitality(PLW* as, PLW* ds) {
    u16 xx = as->wu.att.pow;
    s16 yy;
    s16 power = Power_Data[xx];

    /* E1a (docs/research-arcade-balance-desyncs.md). The `Play_Type == 1`
     * pin is a PS2-ism: CPS3 indexes the row with `Round_Level`
     * UNCONDITIONALLY and has no Play_Type test at all. Measured on the
     * decrypted SH-2 program of sfiii3nr1 -- the two arcade counterparts of
     * these functions are at CPS3 0x0609E36C and 0x0609E3FA, and both do
     *     mov.l  [0x0201137A],r2   ; &Round_Level
     *     mov.l  [0x06194A2C],r6   ; Pow_Control_Data_1
     *     mov.w  @r2,r7            ; r7 = Round_Level   (delay slot)
     *     mov r6,r2 ; add r7,r2 ; mov.b @r2,r2          ; row 0 [Round_Level]
     * with the one branch in the routine selecting the ROW (`mov.b @(8,r2)`
     * = row 1) off an attacker field at +0x3C0, not the index. An
     * exhaustive enumeration of every `mov.l @(disp,PC)` in
     * 0x0609E340..0x0609E490 yields exactly four literals -- Power_Data
     * 0x061946EC, Round_Level 0x0201137A, Pow_Control_Data_1 0x06194A2C and
     * the signed-divide helper 0x0612D428. Arcade `Play_Type` is
     * 0x020113B4 (pinned from arcade Setup_Play_Type at 0x060914D4, a
     * byte-for-byte match of sys_sub.c -> Setup_Play_Type) and is loaded by
     * NEITHER damage routine.
     *
     * `Pow_Control_Data_1[0]` (pow_data.c) is
     * `{90,95,98,100,103,106,109,112}` and index 3 is exactly 100, so the
     * pin means "neutral damage in VS". It coincides with the arcade
     * wherever `Round_Level == 3`, which is the common case: both
     * `Before_Select_Sub()` (game.c, and arcade 0x0609520C, whose
     * unguarded tail store at 0x0609531A is the `= 3`) and
     * `setup_vs_mode()` (netplay.c) leave it at 3, and neither
     * `Update_VS_Data()` nor `Loser_Sub()` (manage.c) moves it while
     * `Play_Type == 1` -- on hardware either (arcade Update_VS_Data tests
     * Play_Type at 0x0609C79A, arcade Loser_Sub at 0x0609C616, both exactly
     * as the port does). The pin therefore diverges only where `Round_Level`
     * is NOT 3 when `Play_Type` becomes 1 -- a second player breaking into a
     * 1P game that has already moved the cabinet's level.
     *
     * NOT demonstrated by the statcheck corpus, and it cannot be: all six
     * human-vs-human segments run at `Round_Level == 3`. This change is
     * therefore evidenced by the arcade program, not by a fixed archive.
     * Under PS2 balance the pin stays, because the PS2 tables were authored
     * against it. */
    if (Play_Type == 1 && !ArcadeBalance_IsEnabled()) {
        yy = Pow_Control_Data_1[0][3];
    } else {
        yy = Pow_Control_Data_1[0][Round_Level];
    }

    ds->wu.dm_vital = (power * yy) / 100;

    if (as->wu.work_id == 1) {
        ds->wu.dm_vital = (ds->wu.dm_vital * as->att_plus) / 8;
    }

    if (ds->wu.work_id == 1) {
        ds->wu.dm_vital = (ds->wu.dm_vital * ds->def_plus) / 8;
    }
}

void cal_damage_vitality_eff(WORK_Other* as, PLW* ds) {
    u16 xx = as->wu.att.pow;
    s16 yy;
    s16 power = Power_Data[xx];

    /* E1a (docs/research-arcade-balance-desyncs.md). The `Play_Type == 1`
     * pin is a PS2-ism: CPS3 indexes the row with `Round_Level`
     * UNCONDITIONALLY and has no Play_Type test at all. Measured on the
     * decrypted SH-2 program of sfiii3nr1 -- the two arcade counterparts of
     * these functions are at CPS3 0x0609E36C and 0x0609E3FA, and both do
     *     mov.l  [0x0201137A],r2   ; &Round_Level
     *     mov.l  [0x06194A2C],r6   ; Pow_Control_Data_1
     *     mov.w  @r2,r7            ; r7 = Round_Level   (delay slot)
     *     mov r6,r2 ; add r7,r2 ; mov.b @r2,r2          ; row 0 [Round_Level]
     * with the one branch in the routine selecting the ROW (`mov.b @(8,r2)`
     * = row 1) off an attacker field at +0x3C0, not the index. An
     * exhaustive enumeration of every `mov.l @(disp,PC)` in
     * 0x0609E340..0x0609E490 yields exactly four literals -- Power_Data
     * 0x061946EC, Round_Level 0x0201137A, Pow_Control_Data_1 0x06194A2C and
     * the signed-divide helper 0x0612D428. Arcade `Play_Type` is
     * 0x020113B4 (pinned from arcade Setup_Play_Type at 0x060914D4, a
     * byte-for-byte match of sys_sub.c -> Setup_Play_Type) and is loaded by
     * NEITHER damage routine.
     *
     * `Pow_Control_Data_1[0]` (pow_data.c) is
     * `{90,95,98,100,103,106,109,112}` and index 3 is exactly 100, so the
     * pin means "neutral damage in VS". It coincides with the arcade
     * wherever `Round_Level == 3`, which is the common case: both
     * `Before_Select_Sub()` (game.c, and arcade 0x0609520C, whose
     * unguarded tail store at 0x0609531A is the `= 3`) and
     * `setup_vs_mode()` (netplay.c) leave it at 3, and neither
     * `Update_VS_Data()` nor `Loser_Sub()` (manage.c) moves it while
     * `Play_Type == 1` -- on hardware either (arcade Update_VS_Data tests
     * Play_Type at 0x0609C79A, arcade Loser_Sub at 0x0609C616, both exactly
     * as the port does). The pin therefore diverges only where `Round_Level`
     * is NOT 3 when `Play_Type` becomes 1 -- a second player breaking into a
     * 1P game that has already moved the cabinet's level.
     *
     * NOT demonstrated by the statcheck corpus, and it cannot be: all six
     * human-vs-human segments run at `Round_Level == 3`. This change is
     * therefore evidenced by the arcade program, not by a fixed archive.
     * Under PS2 balance the pin stays, because the PS2 tables were authored
     * against it. */
    if (Play_Type == 1 && !ArcadeBalance_IsEnabled()) {
        yy = Pow_Control_Data_1[0][3];
    } else {
        yy = Pow_Control_Data_1[0][Round_Level];
    }

    ds->wu.dm_vital = (power * yy) / 100;

    if (as->wu.work_id == 1) {
        ds->wu.dm_vital = (ds->wu.dm_vital * ((PLW*)as)->att_plus) / 8;
    }

    if (ds->wu.work_id == 1) {
        ds->wu.dm_vital = (ds->wu.dm_vital * ds->def_plus) / 8;
    }
}

void Additinal_Score_DM(WORK_Other* wk, u16 ix) {
    s16 id;

    if (wk->wu.work_id == 1) {
        id = wk->wu.id;
    } else {
        if (((WORK*)wk->my_master)->work_id != 1) {
            return;
        }

        id = wk->master_id;
    }

    Score[id][2] += Score_Data[ix];

    if (Score[id][2] >= 99999900) {
        Score[id][2] = 99999900;
    }

    if ((Mode_Type != MODE_VERSUS) && (Mode_Type != MODE_REPLAY)) {
        if (!plw[id].wu.wu_operator) {
            return;
        }

        Score[id][Play_Type] += Score_Data[ix];

        if (Score[id][Play_Type] >= 99999900) {
            Score[id][Play_Type] = 99999900;
        }
    } else {
        Score[id][Play_Type] += Score_Data[ix];

        if (Score[id][Play_Type] >= 99999900) {
            Score[id][Play_Type] = 99999900;
        }
    }
}
