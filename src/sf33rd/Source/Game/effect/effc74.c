/**
 * @file effc74.c
 * Club Metro palette animation — the CPS3 stage effect the PS2 re-authoring
 * dropped. Named `C74` ("C" for CPS3) because the port's id 74 is a different,
 * select-screen effect (`eff74.c`); see
 * docs/research-arcade-cg-data-accuracy.md §23.8.
 *
 * Only the RNG behaviour is reproduced. The arcade routine also drives a
 * palette-cycling animation; rendering it is out of scope (§23.10 item 5), so
 * this work is pulled with `disp_flag = 0` and its sole side effect is
 * `random_16()`. That side effect is the point: without it the port walks a
 * different path through `random_tbl_16` from frame 1 of every round on this
 * stage, in every mode (§23.9).
 *
 * State machine transcribed from the `sfiii3nr1` SH-2 program (CPS3
 * `0x060F1384`), documented in §23.6.
 */

#include "sf33rd/Source/Game/effect/effc74.h"
#include "common.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/pls02.h"
#include "sf33rd/Source/Game/engine/slowf.h"
#include "sf33rd/Source/Game/engine/workuser.h"

/**
 * Cadence table, CPS3 `0x061BF8BC`: four 12-byte entries `{rec*, loop_tbl*,
 * repeat}`. `effc74_step_tbl[type][i]` is `rec[i].t`; entries past
 * `effc74_repeat_tbl[type]` are never read.
 *
 * Frames between draws per type: 0 -> 80, 1 -> 4, 2 -> 2, 3 -> 10.
 */
const s16 effc74_step_tbl[4][5] = {
    { 2, 2, 2, 2, 2 },
    { 1, 1, 0, 0, 0 },
    { 1, 1, 0, 0, 0 },
    { 2, 2, 2, 2, 2 },
};

const s16 effc74_repeat_tbl[4] = { 5, 2, 2, 5 };

const s16 effc74_loops_tbl[4] = { 8, 2, 1, 1 };

/**
 * CPS3 `0x060F1516`: draw a type and reload the cadence from its table entry.
 * This is the only `random_16()` consumer in the effect.
 */
static void effc74_roll(WORK_Other* ewk) {
    s16 type = random_16() & 3;

    ewk->wu.old_rno[3] = type;
    ewk->wu.old_rno[1] = 0;
    ewk->wu.old_rno[2] = effc74_loops_tbl[type];
    ewk->wu.old_rno[0] = effc74_step_tbl[type][0];
}

void effect_C74_move(WORK_Other* ewk) {
    s16 type;

    switch (ewk->wu.routine_no[0]) {
    case 0:
        // Spawn frame. The arcade also block-copies the palette here.
        ewk->wu.routine_no[0]++;
        effc74_roll(ewk);
        break;

    case 1:
        // §23.6 records the gate on this routine only; the spawn frame runs
        // ungated. §23.11 notes the flags were assumed zero for the frames the
        // reproduction covers.
        if (!EXE_flag && !Game_pause) {
            ewk->wu.old_rno[0]--;

            if (ewk->wu.old_rno[0] <= 0) {
                type = ewk->wu.old_rno[3];
                ewk->wu.old_rno[1]++;

                if (ewk->wu.old_rno[1] < effc74_repeat_tbl[type]) {
                    ewk->wu.old_rno[0] = effc74_step_tbl[type][ewk->wu.old_rno[1]];
                } else {
                    ewk->wu.old_rno[1] = 0;
                    ewk->wu.old_rno[2]--;

                    if (ewk->wu.old_rno[2] > 0) {
                        ewk->wu.old_rno[0] = effc74_step_tbl[type][0];
                    } else {
                        effc74_roll(ewk);
                    }
                }
            }
        }

        break;

    default:
        push_effect_work(&ewk->wu);
        break;
    }
}

/// CPS3 `0x060F1652`. Class 4, `id = 74` there; 87 here (§23.8).
s32 effect_C74_init() {
    WORK_Other* ewk;
    s16 ix;

    if ((ix = pull_effect_work(4)) == -1) {
        return -1;
    }

    ewk = (WORK_Other*)frw[ix];
    ewk->wu.be_flag = 1;
    ewk->wu.id = 87;
    ewk->wu.work_id = 16;
    ewk->wu.cgromtype = 1;
    ewk->wu.rl_flag = 0;
    ewk->wu.disp_flag = 0;
    ewk->wu.routine_no[0] = 0;
    return 0;
}
