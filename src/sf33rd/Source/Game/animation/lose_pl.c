/**
 * @file lose_pl.c
 * Losing Character Animations
 */

#include "sf33rd/Source/Game/animation/lose_pl.h"
#include "arcade/arcade_balance.h"
#include "common.h"
#include "sf33rd/Source/Game/effect/effc1.h"
#include "sf33rd/Source/Game/engine/charset.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/pls02.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/stage/bg_data.h"
#include "sf33rd/Source/Game/system/work_sys.h"

/* ARCADE ADJUDICATION of every set_field_hosei_flag site in this file,
 * 2026-09-06. E7 left all 12 unadjudicated; they are adjudicated now.
 *
 * lose_player is 0x060C558C: it copies the 4-entry lose_jp_tbl at 0x061A3A6C
 * (one literal referrer, 0x060C56A8) onto its stack and indexes it by the
 * 21-entry loser_type_tbl at 0x061A3A3C (one referrer, 0x060C56B0); the
 * My_char mismatch arm is `bsr 0x60c5b2c` = meta_lose_pause. Lose_00000
 * (0x060C55DE) is the two-way branch this file has, giving Judge_normal_loser
 * (0x060C5A72) and Normal_normal_Loser (0x060C59BC). meta_lose_tbl is at
 * 0x061A3A7C, 21 entries = ours with 24 inserted at Shin Akuma's index 15.
 *
 *   Lose_00000 0x060C55DE   Lose_10000 0x060C55F4   Lose_20000 0x060C5742
 *   Lose_30000 0x060C5864   Normal_normal_Loser 0x060C59BC
 *   Judge_normal_loser 0x060C5A72   meta_lose_pause 0x060C5B2C
 *
 * ARCADE HAS THE CALL at all six sites -- verified negatives, do not re-check
 * and do not gate. The jsr pairs are 0x060C561A/0x060C563C,
 * 0x060C5768/0x060C578A, 0x060C588A/0x060C58AC, 0x060C59E2/0x060C5A04,
 * 0x060C5A98/0x060C5ABA and 0x060C5B58/0x060C5B7A.
 *
 * DIVERGENT IN SHAPE, NOT IN PRESENCE -- E9b, FIXED and GATED 2026-09-06 (see
 * docs/research-arcade-balance-desyncs.md). In all six the arcade makes the
 * correction the FIRST thing the routine does: nothing but the register
 * prologue (and, in meta_lose_pause, the bg_app_stop store at 0x060C5B3E)
 * precedes the first jsr, and the pcon_rno tests and the routine_no[3]
 * dispatch follow it. Ours was the LAST thing. Normal_normal_Loser is the
 * clearest reading -- 0x060C59E2/0x060C5A04 (the pair), then 0x060C5A08
 * `mov.l 0x60c5ae4,r4 ; =02068c5e` and the tst/cmp-eq-#4 pair, then
 * 0x060C5A18 `mov.w @(r0,r14),r0` with r0 = 42.
 *
 * Why it matters: the pair writes micchaku_flag / hos_fi_flag / hosei_amari
 * every call (zeros when inside the bound), check_damage_hosei() consumes
 * hosei_amari the same frame to push the OTHER player and zeroes it, and
 * bg_sub.c's scroll reads micchaku_flag. So on the `pcon_rno[1] == 0 || == 4`
 * early returns the arcade has corrected the loser and we had not; and on the
 * Lose_20000 -> Judge_normal_loser path the arcade corrects twice before the
 * dispatch (0x060C5768/0x060C578A then 0x060C5A98/0x060C5ABA), which leaves
 * hosei_amari = 0, where we corrected once, after.
 *
 * Shape of the gate: ArcadeBalance_IsEnabled() runs the pair at the head and
 * skips it at the tail; PS2 keeps the tail pair and no head pair, statement
 * for statement what this file has always done. The bound is scrr/scrl in
 * both arms: these routines are reached from Normal_41000 only, in versus,
 * where set_scrrrl() has just computed them from the same bgw[1].wxy[0] and
 * 192 the arcade reads inline (bg_w+0x104 and bg_w+40; the arcade writes 192
 * to the latter at 0x060BB6FE/0x060BB704 when the service byte 0x0206AC62 is
 * 0, which the corpus measures it to be). */
const s16 loser_type_tbl[20] = { 0, 0, 0, 0, 0, 2, 0, 0, 1, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0 };

const s16 meta_lose_tbl[20] = { 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 28, 24, 24, 24, 24, 24, 24 };

/* The screen-edge correction every routine in this file makes. Arcade
 * (0x0611DFB8) and port agree on the body; only WHERE it is called differs. */
static void loser_field_hosei(PLW* wk) {
    if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1) != 0) {
        set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
    }
}

void lose_player(PLW* wk) {
    void (*lose_jp_tbl[4])(PLW*) = { Lose_00000, Lose_10000, Lose_20000, Lose_30000 };

    if (My_char[wk->wu.id] != wk->player_number) {
        meta_lose_pause(wk);
        return;
    }

    lose_jp_tbl[loser_type_tbl[wk->player_number]](wk);
}

void Lose_00000(PLW* wk) {
    if ((pcon_rno[0] == 2) && (pcon_rno[1] == 3)) {
        Judge_normal_loser(wk);
        return;
    }

    Normal_normal_Loser(wk);
}

void Lose_10000(PLW* wk) {
    if (ArcadeBalance_IsEnabled()) {
        loser_field_hosei(wk);
    }

    if ((pcon_rno[0] == 2) && (pcon_rno[1] == 3)) {
        switch (wk->wu.routine_no[3]) {
        case 0:
            wk->wu.routine_no[3]++;
            wk->wu.char_index = random_16();
            wk->wu.char_index &= 3;
            set_char_move_init(&wk->wu, 9, wk->wu.char_index + 0x38);
            break;

        default:
        case 1:
        case 9:
            char_move(&wk->wu);
            break;
        }
    } else if ((pcon_rno[1] == 0) || (pcon_rno[1] == 4)) {
        return;
    } else {
        switch (wk->wu.routine_no[3]) {
        case 0:
            wk->wu.routine_no[3]++;
            wk->wu.char_index = random_16();
            wk->wu.char_index &= 7;
            set_char_move_init(&wk->wu, 9, wk->wu.char_index + 0x18);
            break;

        case 1:
        case 9:
            char_move(&wk->wu);
            break;
        }
    }

    if (!ArcadeBalance_IsEnabled()) {
        loser_field_hosei(wk);
    }
}

void Lose_20000(PLW* wk) {
    s16 work;

    if (ArcadeBalance_IsEnabled()) {
        loser_field_hosei(wk);
    }

    if ((pcon_rno[0] == 2) && (pcon_rno[1] == 3)) {
        Judge_normal_loser(wk);
        return;
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        wk->wu.routine_no[3]++;

        if (!Extra_Break && ((Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type]) * 2) ||
                             (PL_Wins[Winner_id] >= (save_w[Present_Mode].Battle_Number[Play_Type]) + 1))) {
            effect_C1_init(&wk->wu);
        }

        if ((pcon_rno[1] != 0) && (pcon_rno[1] != 4)) {
            work = random_16();
            work &= 7;
            set_char_move_init(&wk->wu, 9, work + 0x18);
            break;
        }

        break;

    default:
        Normal_normal_Loser(wk);
        break;
    }

    if (!ArcadeBalance_IsEnabled()) {
        loser_field_hosei(wk);
    }
}

void Lose_30000(PLW* wk) {
    if (ArcadeBalance_IsEnabled()) {
        loser_field_hosei(wk);
    }

    if ((pcon_rno[0] == 2) && (pcon_rno[1] == 3)) {
        switch (wk->wu.routine_no[3]) {
        case 0:
            wk->wu.routine_no[3]++;
            set_char_move_init(&wk->wu, 9, 56);
            break;

        default:
        case 1:
        case 9:
            char_move(&wk->wu);
            break;
        }

    } else if ((pcon_rno[1] == 0) || (pcon_rno[1] == 4)) {
        return;
    } else {
        switch (wk->wu.routine_no[3]) {
        case 0:
            wk->wu.routine_no[3]++;
            set_char_move_init(&wk->wu, 9, 24);
            break;

        case 1:
        case 9:
            char_move(&wk->wu);
            break;
        }
    }

    if (!ArcadeBalance_IsEnabled()) {
        loser_field_hosei(wk);
    }
}

void Normal_normal_Loser(PLW* wk) {
    s16 work;

    if (ArcadeBalance_IsEnabled()) {
        loser_field_hosei(wk);
    }

    if ((pcon_rno[1] == 0) || (pcon_rno[1] == 4)) {
        return;
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        wk->wu.routine_no[3]++;
        work = random_16();
        work &= 7;
        set_char_move_init(&wk->wu, 9, work + 0x18);
        break;

    case 1:
    case 9:
        char_move(&wk->wu);
        break;
    }

    if (!ArcadeBalance_IsEnabled()) {
        loser_field_hosei(wk);
    }
}

void Judge_normal_loser(PLW* wk) {
    s16 work;

    if (ArcadeBalance_IsEnabled()) {
        loser_field_hosei(wk);
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        wk->wu.routine_no[3] += 1;
        work = random_16();
        work &= 3;
        set_char_move_init(&wk->wu, 9, work + 0x38);
        break;

    case 1:
    case 9:
    default:
        char_move(&wk->wu);
        break;
    }

    if (!ArcadeBalance_IsEnabled()) {
        loser_field_hosei(wk);
    }
}

void meta_lose_pause(PLW* wk) {
    bg_app_stop = 1;

    if (ArcadeBalance_IsEnabled()) {
        loser_field_hosei(wk);
    }

    if ((pcon_rno[1] == 0) || (pcon_rno[1] == 4)) {
        return;
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        wk->wu.routine_no[3] += 1;
        set_char_move_init(&wk->wu, 9, meta_lose_tbl[wk->player_number]);
        break;

    case 1:
    case 9:
        char_move(&wk->wu);
        break;
    }

    if (!ArcadeBalance_IsEnabled()) {
        loser_field_hosei(wk);
    }
}
