/**
 * @file effc08.c
 * Hong Kong (Shopping District) palette animation — the CPS3 stage effect the
 * PS2 re-authoring dropped. Named `C08` ("C" for CPS3) because the port's id 8
 * is a different effect (`eff08.c`, the round-message effect `manage.c`
 * spawns); see docs/research-arcade-cg-data-accuracy.md §23.8.
 *
 * Only the RNG behaviour is reproduced; rendering the palette animation is out
 * of scope (§23.10 item 5), so this work is pulled with `disp_flag = 0` and its
 * sole side effect is `random_16()`.
 *
 * State machine transcribed from the `sfiii3nr1` SH-2 program (CPS3
 * `0x060DD888`), documented in §23.6.
 */

#include "sf33rd/Source/Game/effect/effc08.h"
#include "common.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/pls02.h"
#include "sf33rd/Source/Game/engine/slowf.h"
#include "sf33rd/Source/Game/engine/workuser.h"

/**
 * The two six-byte timer tables the arcade routine indexes by its effect type,
 * CPS3 `0x061BA9C4` (routine 1 idle reload) and `0x061BA9F4` (step reload).
 * Both hold 1 for the type this stage spawns, which is what makes the draw
 * cadence one index every 8 frames.
 */
#define EFFC08_TIMER_A 1
#define EFFC08_TIMER_B 1

/// CPS3 `0x061BAA0C`, indexed by the `random_16()` return (0..15).
const s16 effc08_vt[16] = { 0, 0, 0, 1, 0, 0, 2, 0, 0, 1, 0, 0, 3, 0, 0, 0 };

void effect_C08_move(WORK_Other* ewk) {
    s16 v;

    switch (ewk->wu.routine_no[0]) {
    case 0:
        // Spawn frame. The arcade also block-copies the palette here. No RNG.
        ewk->wu.routine_no[0]++;
        ewk->wu.old_rno[1] = 0;
        ewk->wu.old_rno[2] = 0;
        ewk->wu.old_rno[0] = EFFC08_TIMER_A;
        break;

    case 1:
        // §23.6 records the gate on this routine only; §23.11 notes the flags
        // were assumed zero for the frames the reproduction covers.
        if (!EXE_flag && !Game_pause) {
            ewk->wu.old_rno[0]--;

            if (ewk->wu.old_rno[0] <= 0) {
                ewk->wu.old_rno[1] = (ewk->wu.old_rno[1] + 1) & 7;

                if (ewk->wu.old_rno[1] == 0) {
                    v = effc08_vt[random_16()];

                    if (v != 0) {
                        ewk->wu.old_rno[2] = v;
                        ewk->wu.routine_no[0] = 2;
                        ewk->wu.old_rno[0] = EFFC08_TIMER_B;
                    } else {
                        ewk->wu.old_rno[0] = EFFC08_TIMER_A;
                    }
                } else {
                    ewk->wu.old_rno[0] = EFFC08_TIMER_B;
                }
            }
        }

        break;

    case 2:
        // The 4 x v frame pause a non-zero draw buys. Returns to routine 1 with
        // the 8-step counter at 0, so the next draw is 8 steps later.
        ewk->wu.old_rno[0]--;

        if (ewk->wu.old_rno[0] <= 0) {
            ewk->wu.old_rno[1] = (ewk->wu.old_rno[1] + 1) & 3;

            if (ewk->wu.old_rno[1] != 0) {
                ewk->wu.old_rno[0] = EFFC08_TIMER_B;
            } else {
                ewk->wu.old_rno[2]--;

                if (ewk->wu.old_rno[2] > 0) {
                    ewk->wu.old_rno[0] = EFFC08_TIMER_B;
                } else {
                    ewk->wu.routine_no[0] = 1;
                    ewk->wu.old_rno[0] = EFFC08_TIMER_A;
                }
            }
        }

        break;

    default:
        push_effect_work(&ewk->wu);
        break;
    }
}

/// CPS3 `0x060DDC5E`. Class 4, `id = 8` there; 88 here (§23.8).
s32 effect_C08_init() {
    WORK_Other* ewk;
    s16 ix;

    if ((ix = pull_effect_work(4)) == -1) {
        return -1;
    }

    ewk = (WORK_Other*)frw[ix];
    ewk->wu.be_flag = 1;
    ewk->wu.id = 88;
    ewk->wu.work_id = 16;
    ewk->wu.cgromtype = 1;
    ewk->wu.rl_flag = 0;
    ewk->wu.disp_flag = 0;
    ewk->wu.routine_no[0] = 0;
    return 0;
}
