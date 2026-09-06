/**
 * @file win_pl.c
 * Winning Character Animation
 */

#include "sf33rd/Source/Game/animation/win_pl.h"
#include "arcade/arcade_balance.h"
#include "common.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/effect/eff30.h"
#include "sf33rd/Source/Game/effect/eff31.h"
#include "sf33rd/Source/Game/effect/eff32.h"
#include "sf33rd/Source/Game/effect/eff82.h"
#include "sf33rd/Source/Game/effect/eff83.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/effect/effl3.h"
#include "sf33rd/Source/Game/effect/effl6.h"
#include "sf33rd/Source/Game/effect/effm2.h"
#include "sf33rd/Source/Game/engine/charset.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/pls02.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/stage/bg_data.h"
#include "sf33rd/Source/Game/stage/ta_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"

void Win_00000(PLW* wk);
void Win_01000(PLW* wk);
void jijii_nebukuro(PLW* wk);
void jijii_jump(PLW* wk);
void jijii_full(PLW* wk);
void Win_02000(PLW* wk);
void Win_03000(PLW* wk);
void Win_04000(PLW* wk);
void Normal_normal_Winner(PLW* wk);
void Judge_normal_winner(PLW* wk);
void Win_05000(PLW* wk);
void Win_06000(PLW* wk);
void Win_07000(PLW* wk);
void Win_08000(PLW* wk);
void Win_09000(PLW* wk);
void Win_10000(PLW* wk);
void q_keeping_action(PLW* wk);
void q_leave_after_action(PLW* wk);
void Win_11000(PLW* wk);
void twelve_win_away(PLW* wk);
void twelve_win_backjump(PLW* wk);
void Win_12000(PLW* wk);
void Win_13000(PLW* wk);
void Win_14000(PLW* wk);
void urien_dash(PLW* wk);
void Win_15000(PLW* wk);
s16 win_select(PLW* /* unused */, s16 num);
void bonus_game_win_pause(PLW* wk);
void meta_win_pause(PLW* wk);

s16 win_rno[2];
s16 win_free[2];
s16 poison_flag[2];

/* ARCADE ADJUDICATION of every set_field_hosei_flag site in this file, 2026-09-06.
 * Recorded so nobody re-derives it. E7 read ONE routine (Oro's) out of the CPS3
 * program; this is the rest. Where the arcade HAS the call, that is a verified
 * negative -- do not re-check it, and do not gate it.
 *
 * The routine addresses are not guesses. win_player (0x060C2DDC) copies the
 * 16-entry win_jp_tbl at 0x061A38C0 -- one literal referrer, 0x060C2EBC -- onto
 * its stack and indexes it by the 21-entry winner_type_tbl at 0x061A3890, also
 * one referrer. Win_00000 (0x060C2E88) is `bra 0x60c37ba; nop`, which is how
 * Normal_normal_Winner is pinned; win_player's other three calls give
 * meta_win_pause (0x060C54B2), bonus_game_win_pause (0x060C5308) and
 * Judge_normal_winner (0x060C3898, a bsr) in source order.
 *
 *   Win_00000  0x060C2E88 (stub)  Win_08000  0x060C3E10
 *   Win_01000  0x060C2E8C         Win_09000  0x060C3F54
 *   Win_02000  0x060C33B2         Win_10000  0x060C414E
 *   Win_03000  0x060C34EA         Win_11000  0x060C47DC
 *   Win_04000  0x060C3654         Win_12000  0x060C4C22
 *   Win_05000  0x060C3950         Win_13000  0x060C4D22
 *   Win_06000  0x060C3B1C         Win_14000  0x060C4EAC
 *   Win_07000  0x060C3C3C         Win_15000  0x060C51AA
 *   Normal_normal_Winner 0x060C37BA   Judge_normal_winner  0x060C3898
 *   bonus_game_win_pause 0x060C5308   meta_win_pause       0x060C54B2
 *   twelve_win_away 0x060C499C   twelve_win_backjump 0x060C4A84
 *   Neither is in a table: Win_11000's default arm switches on win_rno[0]
 *   (0x060C490C, mov.w @r13,r0 with r13 = 0x020281AC) and tail-BRANCHES --
 *   case 0 jmp 0x060C37BA (Normal_normal_Winner), case 1 bra 0x060C499C,
 *   case 2 bra 0x060C4A84 -- which is this file's case 0/1/2 ladder exactly.
 *   Both then switch on win_rno[1] at 0x020281AC+2.
 *
 * ARCADE HAS THE CALL, in the same place, with the same arguments -- 16 of the
 * 24 pairs in this file (the remaining 8 are Win_01000's 1, which the arcade
 * does not have, and the 7 listed under DIVERGENT IN SHAPE below):
 *   Win_02000/06000/08000/10000/11000/12000/13000/15000  first statement of
 *     case 0, after the routine_no[3] dispatch, exactly as here.
 *   Win_03000/04000/05000/07000/09000/14000, Normal_normal_Winner,
 *     Judge_normal_winner  before the dispatch, exactly as here.
 * Normal_normal_Winner was the standing worry (its first ten lines are
 * byte-identical to Win_01000's) and it is settled: 0x060C37BA does
 * `mov.b r3,@r2` (bg_app_stop) at 0x060C37D2 and then `jsr` to 0x0611DFB8 at
 * 0x060C37E8 and 0x060C380A, before `mov #42,r0` at 0x060C380E.
 *
 * ARCADE DOES NOT HAVE IT: Win_01000 only. Gated below; see E7.
 *
 * DIVERGENT IN SHAPE, NOT IN PRESENCE -- established, deliberately NOT changed,
 * because they are an ordering/argument class and not E7's "call the arcade
 * never makes". Written up as E9 in docs/research-arcade-balance-desyncs.md:
 *   twelve_win_backjump (0x060C4A84)  arcade puts the pair at the HEAD of
 *     win_rno[1] cases 0 and 1 (0x060C4AEC/0x060C4B0C, 0x060C4B88/0x060C4BA8),
 *     before char_move; we put it at the tail, after.
 *   meta_win_pause  arcade 0x060C54B2 has ONE pair -- plw[wk->wu.id] with
 *     scrr/scrl -- BEFORE the dispatch, and no Bonus_Game_Flag test at all
 *     (0x02016B3A appears nowhere in 0x060C54B2..0x060C558C). We have three
 *     pairs after the dispatch behind a Bonus_Game_Flag if/else.
 *   bonus_game_win_pause  arcade 0x060C5308 has the plw[1]-then-plw[0] shape we
 *     have, in the same place, but passes the ordinary scrr/scrl pair -- both
 *     its pairs read the same *(0x02026CB0) +- *(0x02026BD4) every other routine
 *     reads -- where we pass bs_scrrrl[1][*] / bs_scrrrl[0][*]. */
const s16 winner_type_tbl[20] = { 6, 0, 0, 6, 2, 7, 9, 3, 4, 1, 12, 0, 5, 14, 8, 13, 6, 10, 11, 15 };

void win_player(PLW* wk) {
    void (*win_jp_tbl[16])(PLW*) = { Win_00000, Win_01000, Win_02000, Win_03000, Win_04000, Win_05000,
                                     Win_06000, Win_07000, Win_08000, Win_09000, Win_10000, Win_11000,
                                     Win_12000, Win_13000, Win_14000, Win_15000 };

    if (My_char[wk->wu.id] != wk->player_number) {
        meta_win_pause(wk);
        return;
    }

    if (Bonus_Game_Flag) {
        bonus_game_win_pause(wk);
        return;
    }

    if (pcon_rno[0] == 2 && pcon_rno[1] == 3) {
        Judge_normal_winner(wk);
        return;
    }

    win_jp_tbl[winner_type_tbl[wk->player_number]](wk);
}

void Win_00000(PLW* wk) {
    Normal_normal_Winner(wk);
}

const s16 win_10000_tbl[2][8] = { { 32, 33, 34, 32, 36, 37, 38, 33 }, { 35, 39, 34, 35, 36, 37, 38, 39 } };

void Win_01000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    /* ARCADE: the CPS3 routine makes NO screen-edge correction here.
     *
     * Reached as win_jp_tbl[winner_type_tbl[player_number]]: win_player
     * (0x060C2DDC) copies the 16-entry table at 0x061A38C0 onto its stack and
     * indexes it by the s16 table at 0x061A3890, whose Oro entry is 1 ->
     * 0x060C2E8C, this function. Both tables are unique in the image, and
     * 0x061A3890 has exactly one literal referrer (0x060C2ED4, in win_player's
     * own pool).
     *
     * The arcade body goes from the bg_app_stop store STRAIGHT into the
     * dispatch, with nothing between them:
     *
     *   060c2ea2  mov.b r3,@r2       ; bg_app_stop = 1 (r3=1, r2=0x0202802A)
     *   060c2ea4  mov.l 0x60c2edc,r13
     *   060c2ea6  mov.w @(r0,r14),r0 ; r0=42 -> wk->wu.routine_no[3]
     *   060c2ea8  cmp/eq #0,r0 / bt  -> case 0
     *   060c2eae  cmp/eq #1,r0 / bt  -> 0x060C2F94  } case 1 and case 9 share
     *   060c2eb2  cmp/eq #9,r0 / bt  -> 0x060C2F94  } a target, as they do here
     *   060c2eb6  bra   0x060C3022                    (default -> tail)
     *
     * Corroborating the negative over the whole routine
     * (0x060C2E8C..0x060C33B2, 1318 bytes, with all three jijii_* inlined): no
     * 4-byte-aligned word in it equals &set_field_hosei_flag (0x0611DFB8), so
     * no `jsr @rn` can reach it, and neither can bsr -- the target is 0x5AC06
     * past the end against a +-0x1000 displacement reach. The pool does carry
     * the neighbouring 0x0611E0EE (random_16), which is the control: an
     * address 0x136 away was found by the same scan that found no
     * set_field_hosei_flag.
     *
     * The port's unconditional call pins Oro at scrr - satse[] (screen centre
     * + 164) for the whole win leap, so jijii_jump's exit test --
     * xyz[0].disp.pos > bg_w.bgw[1].xy[0].disp.pos + 320 -- is never reached
     * and the leap does not terminate. See E7 in
     * docs/research-arcade-balance-desyncs.md.
     *
     * PS2 keeps the clamp: this was proven against the CPS3 program and NOT
     * against the PS2 binary, so it is gated (the rule settled in 2d74225d,
     * applied again in 192291a4). */
    if (!ArcadeBalance_IsEnabled()) {
        if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
            set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
        }
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;
        work = win_select(wk, 7);

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            if (Round_Result & 0x800) {
                wk->wu.cmwk[0] = 0;
                set_char_move_init(&wk->wu, 9, 42);
                win_rno[0] = 3;
                break;
            }

            if (bg_w.stage == 9) {
                set_char_move_init(&wk->wu, 9, 41);
                win_rno[0] = 1;
                break;
            }

            set_char_move_init(&wk->wu, 9, win_10000_tbl[1][work]);

            if (work == 4) {
                win_rno[0] = 2;
            }

            break;
        }

        set_char_move_init(&wk->wu, 9, win_10000_tbl[0][work]);

        if (work == 4) {
            win_rno[0] = 2;
        }

        break;

    case 1:
    case 9:
        switch (win_rno[0]) {
        case 0:
            char_move(&wk->wu);
            break;

        case 1:
            jijii_nebukuro(wk);
            break;

        case 2:
            jijii_jump(wk);
            break;

        case 3:
            jijii_full(wk);
            break;
        }

        break;
    }
}

void jijii_nebukuro(PLW* wk) {
    bg_app_stop = 1;

    switch (win_rno[1]) {
    case 0:
        char_move(&wk->wu);

        if (wk->wu.cg_type == 1) {
            win_rno[1]++;
            char_move_z(&wk->wu);
            wk->wu.mvxy.a[1].sp = 0xF0000;
            wk->wu.mvxy.d[1].sp = -0x600;
        }

        break;

    case 1:
        if (wk->wu.cg_type != 2) {
            char_move(&wk->wu);
        }

        add_y_sub((WORK_Other*)wk);

        if (wk->wu.xyz[1].disp.pos > 256) {
            win_rno[1]++;
            win_sp_flag = 2;
            set_char_move_init(&wk->wu, 9, 40);
            wk->wu.xyz[1].disp.pos = 200;
        }

        break;

    case 2:
        char_move(&wk->wu);
        break;
    }
}

void jijii_jump(PLW* wk) {
    s16 id_w;

    bg_app_stop = 1;
    id_w = wk->wu.id ^ 1;
    wk->wu.position_z = plw[id_w].wu.position_z - 1;
    wk->wu.my_priority = wk->wu.position_z;

    switch (win_rno[1]) {
    case 0:
        char_move(&wk->wu);
        if (wk->wu.cg_type == 9) {
            win_rno[1]++;

            if (wk->wu.rl_flag) {
                wk->wu.mvxy.a[0].sp = 0x60000;
                wk->wu.mvxy.d[0].sp = 0x1000;
            } else {
                wk->wu.mvxy.a[0].sp = -0x60000;
                wk->wu.mvxy.d[0].sp = -0x1000;
            }

            wk->wu.mvxy.a[1].sp = 0xA0000;
            wk->wu.mvxy.d[1].sp = -0x600;
        }

        break;

    case 1:
        if (wk->wu.cg_type != 99) {
            char_move(&wk->wu);
        }

        add_x_sub((WORK_Other*)wk);
        add_y_sub((WORK_Other*)wk);

        if (wk->wu.rl_flag) {
            if (wk->wu.xyz[0].disp.pos > bg_w.bgw[1].xy[0].disp.pos + 320) {
                win_rno[1]++;
                effect_work_kill(3, 13);
            }

            break;
        }

        if (wk->wu.xyz[0].disp.pos < bg_w.bgw[1].xy[0].disp.pos - 320) {
            win_rno[1]++;
            effect_work_kill(3, 13);
        }

        break;

    case 2:
        win_rno[1]++;
        set_char_move_init2(&wk->wu, 9, 36, 7, 0);
        win_free[wk->wu.id] = 48;
        break;

    case 3:
        win_free[wk->wu.id]--;

        if (win_free[wk->wu.id] > 0) {
            break;
        }

        win_rno[1]++;

        if (wk->wu.rl_flag) {
            wk->wu.xyz[0].disp.pos = bg_w.bgw[1].xy[0].disp.pos - 328;
            wk->wu.mvxy.a[0].sp = 0x18000;
        } else {
            wk->wu.xyz[0].disp.pos = bg_w.bgw[1].xy[0].disp.pos + 328;
            wk->wu.mvxy.a[0].sp = -0x18000;
        }

        wk->wu.mvxy.d[0].sp = 0;
        wk->wu.xyz[1].cal = 0;
        /* fallthrough */

    case 4:
        add_x_sub((WORK_Other*)wk);
        char_move(&wk->wu);

        break;
    }
}

void jijii_full(PLW* wk) {
    bg_app_stop = 1;

    switch (win_rno[1]) {
    case 0:
        char_move(&wk->wu);
        if (wk->wu.cmwk[0] == 1) {
            win_rno[1]++;
            break;
        }

        break;

    case 1:
        char_move(&wk->wu);
        wk->wu.xyz[1].cal += 0x10000;

        if (wk->wu.xyz[1].disp.pos >= 42) {
            win_rno[1]++;
            wk->wu.cmwk[0] = 2;
            set_char_move_init(&wk->wu, 9, 43);
            break;
        }

        break;

    case 2:
        char_move(&wk->wu);
        break;
    }
}

const s16 win_2000_tbl[18] = { 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1 };

void Win_02000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    switch (wk->wu.routine_no[3]) {
    case 0:
        if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
            set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
        }

        wk->wu.routine_no[3]++;

        win_rno[0] = win_rno[1] = 0;
        work = win_select(wk, 3);

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            if (win_2000_tbl[bg_w.bg_index]) {
                set_char_move_init(&wk->wu, 9, work + 36);
            } else if (work & 1) {
                set_char_move_init(&wk->wu, 9, 32);
            } else {
                set_char_move_init(&wk->wu, 9, 38);
            }
        } else {
            set_char_move_init(&wk->wu, 9, 32);
        }

        break;

    default:
        Normal_normal_Winner(wk);
        break;
    }
}

const s16 Win_3000_tbl[16] = { 42, 34, 33, 42, 32, 42, 32, 35, 42, 34, 33, 42, 32, 42, 32, 35 };

const s8 Win_3001_tbl[16] = { 36, 40, 41, 40, 41, 38, 40, 39, 36, 40, 41, 39, 41, 37, 39, 40 };

void Win_03000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
        set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        wk->wu.routine_no[3]++;
        win_rno[0] = win_rno[1] = 0;
        work = win_select(wk, 15);

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            if (bg_w.stage == 7) {
                set_char_move_init(&wk->wu, 9, 43);
                break;
            }

            set_char_move_init(&wk->wu, 9, Win_3001_tbl[work]);

            if (Win_3001_tbl[work] == 41) {
                win_rno[0] = 1;
            }

            break;
        }

        set_char_move_init(&wk->wu, 9, Win_3000_tbl[work]);
        break;

    default:
        if (win_rno[0]) {
            char_move(&wk->wu);

            if (wk->wu.cg_type == 0xFF) {
                wk->wu.disp_flag = 0;
                win_rno[0] = 0;
            }

            break;
        }

        char_move(&wk->wu);
        break;
    }
}

void Win_04000(PLW* wk) {
    s16 work;
    s16 work2;

    bg_app_stop = 1;

    if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
        set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        wk->wu.routine_no[3]++;

        win_rno[0] = win_rno[1] = 0;
        work = win_select(wk, 3);

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            set_char_move_init(&wk->wu, 9, work + 36);
            break;
        }

        switch (work) {
        case 1:
        case 3:
            if (wk->wu.now_koc == 0 && wk->wu.char_index == 0) {
                work2 = wk->wu.cg_ix / wk->wu.cgd_type;
                work2 += 2;
                set_char_move_init2(&wk->wu, 9, work + 32, work2, 0);
            } else {
                set_char_move_init(&wk->wu, 9, work + 32);
            }

            break;

        default:
            set_char_move_init(&wk->wu, 9, work + 32);
            break;
        }

        break;

    default:
    case 1:
        char_move(&wk->wu);
        break;
    }
}

void Normal_normal_Winner(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
        set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        wk->wu.routine_no[3]++;
        work = win_select(wk, 7);
        set_char_move_init(&wk->wu, 9, work + 32);
        break;

    case 1:
    case 9:
        char_move(&wk->wu);
        break;
    }
}

void Judge_normal_winner(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
        set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;
        work = win_select(wk, 3);
        set_char_move_init(&wk->wu, 9, work + 52);
        break;

    case 1:
    case 9:
        char_move(&wk->wu);
        break;
    }
}

void Win_05000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
        set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type]) {
            set_char_move_init(&wk->wu, 9, 36);

            if (wk->wu.rl_flag) {
                wk->wu.mvxy.a[0].sp = 0x20000;
            } else {
                wk->wu.mvxy.a[0].sp = -0x20000;
            }

            wk->wu.mvxy.d[0].sp = 0;
            wk->wu.mvxy.a[1].sp = 0x80000;
            wk->wu.mvxy.d[1].sp = -0x6000;
            win_rno[0] = 0;
            break;
        }

        work = win_select(wk, 3);
        set_char_move_init(&wk->wu, 9, work + 32);
        win_rno[0] = 1;
        break;

    default:
        if (win_rno[0]) {
            Normal_normal_Winner(wk);
            break;
        }

        switch (win_rno[1]) {
        case 0:
            char_move(&wk->wu);
            add_x_sub((WORK_Other*)wk);
            add_y_sub((WORK_Other*)wk);

            if (wk->wu.xyz[1].disp.pos < 0) {
                win_rno[1]++;
                wk->wu.xyz[1].cal = 0;
                char_move_z(&wk->wu);
            }

            break;

        case 1:
            char_move(&wk->wu);
            break;
        }
    }
}

void Win_06000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    switch (wk->wu.routine_no[3]) {
    case 0:
        if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
            set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
        }

        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            work = win_select(wk, 3);
            set_char_move_init(&wk->wu, 9, work + 36);
        } else {
            work = win_select(wk, 3);
            set_char_move_init(&wk->wu, 9, work + 32);
        }

        break;

    default:
        Normal_normal_Winner(wk);
        break;
    }
}

void Win_07000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
        set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            work = win_select(wk, 7);

            if (work < 4) {
                if (plw[0].player_number == 5 && plw[1].player_number == 5) {
                    win_rno[0] = 0;
                    set_char_move_init(&wk->wu, 9, work + 32);
                    break;
                }

                effect_82_init(&wk->wu);
                win_rno[0] = 1;
                set_char_move_init(&wk->wu, 9, 60);
                wk->wu.cmwk[1] = 0;
                break;
            }

            if (plw[0].player_number == 5 && plw[1].player_number == 5) {
                win_rno[0] = 0;
                set_char_move_init(&wk->wu, 9, work + 32);
                break;
            }

            effect_83_init(&wk->wu);
            win_rno[0] = 2;
            set_char_move_init(&wk->wu, 9, 60);
            wk->wu.cmwk[1] = 0;
            break;
        }

        win_rno[0] = 0;
        work = win_select(wk, 7);
        set_char_move_init(&wk->wu, 9, work + 32);
        break;

    default:
        switch (win_rno[0]) {
        case 0:
            char_move(&wk->wu);
            break;

        default:
            if (win_rno[1] == 0) {
                if (wk->wu.cmwk[1]) {
                    win_rno[1]++;

                    if (win_rno[0] == 1) {
                        set_char_move_init(&wk->wu, 9, 32);
                    } else {
                        set_char_move_init(&wk->wu, 9, 37);
                    }

                    break;
                }

                char_move(&wk->wu);
                break;
            }

            char_move(&wk->wu);
        }
    }
}

void Win_08000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    switch (wk->wu.routine_no[3]) {
    case 0:
        if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
            set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
        }

        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;

        if (Round_Result & 0x800) {
            set_char_move_init(&wk->wu, 9, 40);
        } else if (
            Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1
        ) {
            work = win_select(wk, 3);
            set_char_move_init(&wk->wu, 9, work + 36);
        } else {
            work = win_select(wk, 3);
            set_char_move_init(&wk->wu, 9, work + 32);
        }

        break;

    default:
        Normal_normal_Winner(wk);
        break;
    }
}

void Win_09000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
        set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;
        work = win_select(wk, 7);

        if (work == 7) {
            set_char_move_init(&wk->wu, 9, 32);
        } else {
            set_char_move_init(&wk->wu, 9, (work) + 32);
        }

        if (Round_num < (save_w[Present_Mode].Battle_Number[Play_Type] * 2) &&
            PL_Wins[wk->wu.id] < save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            break;
        }

        if (poison_flag[wk->wu.id]) {
            break;
        }

        switch (work) {
        case 0:
            effect_L6_init(&wk->wu, 0);
            break;

        case 3:
            effect_30_init(&wk->wu);
            break;

        case 4:
            effect_31_init(&wk->wu);
            break;

        case 5:
            effect_32_init(&wk->wu);
            break;

        case 7:
            wk->wu.cmwk[0] = 0;
            effect_L6_init(&wk->wu, 1);
            set_char_move_init(&wk->wu, 0, 0);
            win_rno[0] = 1;
            break;
        }

        break;

    default:
        if (win_rno[0]) {
            switch (win_rno[1]) {
            case 0:
                char_move(&wk->wu);

                if (wk->wu.cmwk[0]) {
                    win_rno[1]++;
                    set_char_move_init(&wk->wu, 9, 39);
                }

                break;

            case 1:
                char_move(&wk->wu);
                break;
            }

            break;
        }

        Normal_normal_Winner(wk);
        break;
    }
}

void Win_10000(PLW* wk) {
    s16 work;
    s16 work2;
    s16 id_w;

    bg_app_stop = 1;

    id_w = wk->wu.id ^ 1;
    wk->wu.position_z = wk->wu.next_z = plw[id_w].wu.position_z + 1;

    switch (wk->wu.routine_no[3]) {
    case 0:
        if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
            set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
        }

        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;
        work = win_select(wk, 3);

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            work2 = wk->wu.xyz[0].disp.pos - plw[id_w].wu.xyz[0].disp.pos;

            if (work2 < 0) {
                work2 = -work2;
            }

            if (work2 > 224) {
                if (work & 1) {
                    win_rno[0] = 1;
                } else {
                    win_rno[0] = 2;
                }
            } else if (work > 1) {
                if (work & 1) {
                    if (plw[id_w].wu.char_index != 67) {
                        win_rno[0] = 1;
                    } else {
                        win_rno[0] = 3;
                    }
                } else if (plw[id_w].wu.char_index != 67) {
                    win_rno[0] = 2;
                } else {
                    win_rno[0] = 4;
                }
            } else if (work & 1) {
                win_rno[0] = 1;
            } else {
                win_rno[0] = 2;
            }
        } else {
            set_char_move_init(&wk->wu, 9, work + 32);
        }

        break;

    default:
        switch (win_rno[0]) {
        case 0:
            Normal_normal_Winner(wk);
            break;

        case 1:
        case 3:
            q_keeping_action(wk);
            break;

        case 2:
        case 4:
            q_leave_after_action(wk);
            break;
        }
    }
}

const s16 q_em_distance_tbl[20][2] = { { -96, -16 }, { -104, 0 },  { -90, -16 }, { -100, -8 }, { -100, 0 },
                                       { -106, 0 },  { 12, -117 }, { -84, -21 }, { -112, 0 },  { -106, 4 },
                                       { -100, 0 },  { -90, -16 }, { -90, -16 }, { -96, -16 }, { -90, -16 },
                                       { -90, -16 }, { 0, -96 },   { -2, -112 }, { -112, 4 },  { -96, -6 } };

s16 q_em_distance_chk(PLW* wk) {
    s16 work;
    s16 id_w = wk->wu.id ^ 1;
    s16 rl_w = wk->wu.rl_flag ^ plw[id_w].wu.rl_flag;

    if (wk->wu.rl_flag) {
        work = wk->wu.xyz[0].disp.pos - plw[id_w].wu.xyz[0].disp.pos;

        if (work >= q_em_distance_tbl[plw[id_w].player_number][rl_w]) {
            return 1;
        }
    } else {
        work = plw[id_w].wu.xyz[0].disp.pos - wk->wu.xyz[0].disp.pos;

        if (work >= q_em_distance_tbl[plw[id_w].player_number][rl_w]) {
            return 1;
        }
    }

    return 0;
}

s32 q_em_dir(PLW* wk) {
    s16 work;
    s16 pos_w;
    s16 id_w = wk->wu.id ^ 1;

    work = wk->wu.xyz[0].disp.pos - plw[id_w].wu.xyz[0].disp.pos;

    if (work < 0) {
        wk->wu.direction = 1;
    } else {
        wk->wu.direction = 0;
    }

    pos_w = wk->wu.xyz[0].disp.pos;

    if (q_em_distance_chk(wk)) {
        if (win_rno[0] == 3) {
            win_rno[0] = 1;
            set_char_move_init(&wk->wu, 9, 36);
        } else if (win_rno[0] == 4) {
            win_rno[0] = 2;
            set_char_move_init(&wk->wu, 9, 36);
        }

        wk->wu.direction = wk->wu.rl_flag;
        win_rno[1] = 4;
        wk->wu.xyz[0].disp.pos = pos_w;
        return 0;
    }

    wk->wu.xyz[0].disp.pos = pos_w;
    return 1;
}

void q_keeping_action(PLW* wk) {
    switch (win_rno[1]) {
    case 0:
        if (!q_em_dir(wk)) {
            break;
        }

        if (wk->wu.direction == wk->wu.rl_flag) {
            win_rno[1] = 2;
            break;
        }

        win_rno[1] = 1;
        set_char_move_init(&wk->wu, 9, 40);
        wk->wu.rl_flag ^= 1;
        break;

    case 1:
        char_move(&wk->wu);

        if (wk->wu.cg_type == 0xFF) {
            win_rno[1]++;
            break;
        }

        break;

    case 2:
        win_rno[1]++;
        set_char_move_init(&wk->wu, 9, 41);
        wk->wu.mvxy.d[0].sp = 0;

        if (wk->wu.rl_flag) {
            wk->wu.mvxy.a[0].sp = 0x1C000;
            break;
        }

        wk->wu.mvxy.a[0].sp = -0x1C000;
        break;

    case 3:
        char_move(&wk->wu);
        add_x_sub((WORK_Other*)wk);

        if (!q_em_distance_chk(wk)) {
            break;
        }

        win_rno[1]++;

        if (win_rno[0] == 1) {
            set_char_move_init(&wk->wu, 9, 36);
            break;
        }

        set_char_move_init(&wk->wu, 9, 37);
        break;

    case 4:
        char_move(&wk->wu);
        break;
    }
}

void q_leave_after_action(PLW* wk) {
    s16 work;

    switch (win_rno[1]) {
    case 0:
        if (q_em_dir(wk) == 0) {
            break;
        }

        if (wk->wu.direction == wk->wu.rl_flag) {
            win_rno[1] = 2;
        } else {
            win_rno[1] = 1;
            set_char_move_init(&wk->wu, 9, 40);
            wk->wu.rl_flag ^= 1;
        }

        break;

    case 1:
        char_move(&wk->wu);

        if (wk->wu.cg_type == 0xFF) {
            win_rno[1]++;
        }

        break;

    case 2:
        win_rno[1]++;
        set_char_move_init(&wk->wu, 9, 41);
        wk->wu.mvxy.d[0].sp = 0;

        if (wk->wu.rl_flag) {
            wk->wu.mvxy.a[0].sp = 0x1C000;
        } else {
            wk->wu.mvxy.a[0].sp = -0x1C000;
        }

        break;

    case 3:
        char_move(&wk->wu);
        add_x_sub((WORK_Other*)wk);

        if (q_em_distance_chk(wk)) {
            win_rno[1]++;

            if (win_rno[0] == 2) {
                set_char_move_init(&wk->wu, 9, 36);
            } else {
                set_char_move_init(&wk->wu, 9, 39);
            }
        }

        break;

    case 4:
        char_move(&wk->wu);

        if (wk->wu.cg_type == 0xFF) {
            win_rno[1]++;
            set_char_move_init(&wk->wu, 9, 41);
            wk->wu.mvxy.d[0].sp = 0;

            if (wk->wu.rl_flag) {
                wk->wu.mvxy.a[0].sp = 0x1C000;
            } else {
                wk->wu.mvxy.a[0].sp = -0x1C000;
            }
        }

        break;

    case 5:
        char_move(&wk->wu);
        add_x_sub((WORK_Other*)wk);

        if (wk->wu.rl_flag) {
            work = bg_w.bgw[1].wxy[0].disp.pos + bg_w.pos_offset;
            work += 64;

            if (work < wk->wu.xyz[0].disp.pos) {
                win_rno[1]++;
            }

            break;
        }

        work = bg_w.bgw[1].wxy[0].disp.pos - bg_w.pos_offset;
        work -= 64;

        if (work > wk->wu.xyz[0].disp.pos) {
            win_rno[1]++;
        }

        break;
    }
}

void Win_11000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    switch (wk->wu.routine_no[3]) {
    case 0:
        if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
            set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
        }

        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;
        work = win_select(wk, 3);

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            if (Perfect_Flag) {
                win_rno[0] = 1;
                set_char_move_init(&wk->wu, 9, 38);
                effect_L3_init(wk);
            } else {
                set_char_move_init(&wk->wu, 9, work + 36);
                switch (work) {
                case 0:
                    win_rno[0] = 2;
                    break;

                case 1:
                    break;

                default:
                    effect_L3_init(wk);
                    win_rno[0] = 1;
                    break;
                }
            }
        } else {
            win_rno[0] = 0;
            set_char_move_init(&wk->wu, 9, work + 32);
        }

        break;

    default:
        switch (win_rno[0]) {
        case 0:
            Normal_normal_Winner(wk);
            break;

        case 1:
            twelve_win_away(wk);
            break;

        case 2:
            twelve_win_backjump(wk);
            break;
        }
    }
}

void twelve_win_away(PLW* wk) {
    switch (win_rno[1]) {
    case 0:
        char_move(&wk->wu);

        if (wk->wu.cg_type == 1) {
            win_rno[1]++;
            wk->wu.mvxy.a[0].sp = 0;
            wk->wu.mvxy.d[0].sp = 0;
            wk->wu.mvxy.a[1].sp = 0x78000;
            wk->wu.mvxy.d[1].sp = -0x6000;
        }

        break;

    case 1:
        add_y_sub((WORK_Other*)wk);
        char_move(&wk->wu);

        if (wk->wu.cg_type != 2) {
            break;
        }

        win_rno[1]++;
        wk->wu.mvxy.d[0].sp = 0;

        if (wk->wu.rl_flag) {
            wk->wu.mvxy.a[0].sp = 0x80000;
        } else {
            wk->wu.mvxy.a[0].sp = -0x80000;
        }

        wk->wu.mvxy.a[1].sp = -0x8000;
        wk->wu.mvxy.d[1].sp = 0x4000;
        break;

    case 2:
        add_x_sub((WORK_Other*)wk);
        add_y_sub((WORK_Other*)wk);

        if (!range_x_check3((WORK_Other*)wk, 208)) {
            win_rno[1]++;
        }

        break;

    case 3:
        break;
    }
}

void twelve_win_backjump(PLW* wk) {
    switch (win_rno[1]) {
    case 0:
        char_move(&wk->wu);

        if (wk->wu.cg_type == 1) {
            win_rno[1]++;
            wk->wu.mvxy.a[0].sp = 0x30000;
            wk->wu.mvxy.d[0].sp = 0;
            wk->wu.mvxy.a[1].sp = 0x78000;
            wk->wu.mvxy.d[1].sp = -0x5000;

            if (wk->wu.rl_flag) {
                wk->wu.mvxy.a[0].sp = -wk->wu.mvxy.a[0].sp;
            }
        }

        if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
            set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
        }

        break;

    case 1:
        add_y_sub((WORK_Other*)wk);
        add_x_sub((WORK_Other*)wk);
        char_move(&wk->wu);

        if (wk->wu.cg_type == 2) {
            win_rno[1]++;
            char_move_z(&wk->wu);
            wk->wu.xyz[1].cal = 0;
        }

        if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
            set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
        }

        break;

    case 2:
        char_move(&wk->wu);

        if (wk->wu.cg_type == 9) {
            win_rno[1]++;
        }

        break;

    case 3:
        char_move(&wk->wu);
        wk->wu.xyz[1].cal += 0x20000;

        if (wk->wu.xyz[1].disp.pos > 256) {
            win_rno[1]++;
        }

        break;

    case 4:
        break;
    }
}

void Win_12000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    switch (wk->wu.routine_no[3]) {
    case 0:
        if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
            set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
        }

        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;
        work = win_select(wk, 7);
        set_char_move_init(&wk->wu, 9, work + 32);

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            effect_M2_init(&wk->wu, 1);
        }

        break;

    default:
        Normal_normal_Winner(wk);
        break;
    }
}

void Win_13000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    switch (wk->wu.routine_no[3]) {
    case 0:
        if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
            set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
        }

        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            /* ARCADE: bit 12, not bit 0 -- the same defect as E8, in the routine
             * E8 named and could not read.
             *
             * This is Chun-Li's win routine and hers alone: winner_type_tbl
             * holds 13 at exactly one index, 15 == CHAR_CHUNLI (arcade index 16,
             * Shin Akuma inserted at 15), and win_jp_tbl[13] == 0x060C4D22.
             *
             * The arcade routine tests a mask held in a pool word, not the
             * immediate 1:
             *
             *   060c4df4  mov.w 0x60c4e74,r4   ; r4 = 0x1000  (bit 12)
             *   060c4df6  mov.w @(8,r14),r0    ; r0 = wu.id
             *   060c4df8  tst   r0,r0
             *   060c4dfa  bt    0x60c4e0a      ; id == 0 -> the P1 arm
             *   060c4dfc  mov.l 0x60c4e9c,r2   ; = 0x0206AA90  (P2SW_0)
             *   060c4e02  tst   r4,r3
             *   060c4e04  bt    0x60c4e20      ; not held -> win_select
             *   060c4e0a  mov.l 0x60c4ea0,r2   ; = 0x0206AA8C  (P1SW_0)
             *   060c4e10  tst   r4,r3
             *   060c4e14  mov   #40,r6 / mov #9,r5 / jsr set_char_move_init
             *
             * Same two raw switch words E8 established (0x0206AA8C / 0x0206AA90)
             * and the same 0x1000. The routine is Win_13000 and not a lookalike:
             * win_player (0x060C2DDC) copies win_jp_tbl from 0x061A38C0, whose
             * entry [13] is 0x060C4D22, and that function's case 0 matches this
             * one statement for statement -- the set_field_hosei_flag pair, the
             * win_rno[0] = win_rno[1] = 0 pair at 0x020281AC, routine_no[3]++,
             * the Round_num / PL_Wins test, and the default arm tail-calling
             * Normal_normal_Winner (0x060C37BA).
             *
             * WHY THE BIT MATTERS BEYOND THE POSE. The held branch returns
             * WITHOUT calling win_select(), so it draws no random_16. Under
             * `& 1` a Chun-Li player merely holding UP on the deciding win skips
             * a draw the arcade takes -- an RNG divergence, not just a different
             * animation.
             *
             * SWK_START is a conversion identity here, exactly as in effl7.c:
             * src/test/replay_game.c -> read_input_buff() maps arcade bit 12 to
             * (raw & (1 << 12)) << 2 == SWK_START, so a p*sw_0 word produced by
             * this port's conversion layer carries arcade bit 12 at SWK_START.
             *
             * PS2 keeps `& 1`: proven against the CPS3 program and not against
             * the PS2 binary (2d74225d, 192291a4). SWK_UP is 1 << 0, so the PS2
             * arm is bit-identical to what this function has always done.
             *
             * See Class B / E8 and E7's adjudication table in
             * docs/research-arcade-balance-desyncs.md. */
            const u16 pose_sw = ArcadeBalance_IsEnabled() ? (u16)SWK_START : (u16)SWK_UP;

            if (wk->wu.id) {
                if (p2sw_0 & pose_sw) {
                    set_char_move_init(&wk->wu, 9, 40);
                    return;
                }
            } else if (p1sw_0 & pose_sw) {
                set_char_move_init(&wk->wu, 9, 40);
                return;
            }

            work = win_select(wk, 3);
            set_char_move_init(&wk->wu, 9, work + 36);
        } else {
            work = win_select(wk, 3);
            set_char_move_init(&wk->wu, 9, work + 32);
        }

        break;

    default:
        Normal_normal_Winner(wk);
        break;
    }
}

void Win_14000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
        set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            work = win_select(wk, 3);

            if (!(work & 1)) {
                win_rno[0] = 1;
            } else {
                set_char_move_init(&wk->wu, 9, work + 36);
            }
        } else {
            work = win_select(wk, 3);
            set_char_move_init(&wk->wu, 9, work + 32);
        }

        break;

    default:
        if (win_rno[0]) {
            urien_dash(wk);
        } else {
            Normal_normal_Winner(wk);
        }

        break;
    }
}

s32 urien_dash_chk(PLW* wk) {
    s16 id_w = wk->wu.id ^ 1;
    s16 pos_w = wk->wu.xyz[0].disp.pos - plw[id_w].wu.xyz[0].disp.pos;

    if (pos_w < 0) {
        pos_w = -pos_w;

        if (!wk->wu.rl_waza) {
            wk->wu.rl_waza = 1;
        }
    } else if (wk->wu.rl_waza) {
        wk->wu.rl_waza = 0;
    }

    if (pos_w < 88) {
        return 1;
    }

    return 0;
}

void urien_dash(PLW* wk) {
    switch (win_rno[1]) {
    case 0:
        win_rno[1]++;

        if (urien_dash_chk(wk)) {
            win_rno[1] = 5;
        } else {
            wk->wu.rl_flag = wk->wu.rl_waza;
            set_char_move_init(&wk->wu, 0, 4);
            setup_mvxy_data(&wk->wu, 2);
        }

        /* fallthrough */

    case 1:
        if (wk->wu.cg_type == 1) {
            win_rno[1]++;
            add_mvxy_speed(&wk->wu);
        } else {
            char_move(&wk->wu);
        }

        break;

    case 2:
        add_mvxy_speed(&wk->wu);
        cal_mvxy_speed(&wk->wu);
        char_move(&wk->wu);

        if (wk->wu.xyz[1].disp.pos + wk->wu.cg_jphos >= 1) {
            break;
        }

        win_rno[1]++;
        wk->wu.position_y = 0;
        wk->wu.xyz[1].cal = 0;
        wk->wu.mvxy.a[1].sp = 0;
        char_move_cmja(&wk->wu);
        break;

    case 3:
        char_move(&wk->wu);

        if (wk->wu.cg_type != 64) {
            break;
        }

        if (urien_dash_chk(wk)) {
            win_rno[1]++;
        } else {
            win_rno[1] = 0;
        }

        break;

    case 4:
        char_move(&wk->wu);

        if (wk->wu.cg_type != 0xFF) {
            break;
        }

        win_rno[1]++;
        /* fallthrough */

    case 5:
        win_rno[1]++;
        set_char_move_init(&wk->wu, 9, 36);
        break;

    case 6:
        char_move(&wk->wu);
        break;
    }
}

const s16 Win_15000_tbl[8] = { 38, 37, 40, 39, 38, 40, 39, 36 };

void Win_15000(PLW* wk) {
    s16 work;

    bg_app_stop = 1;

    switch (wk->wu.routine_no[3]) {
    case 0:
        if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
            set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
        }

        win_rno[0] = win_rno[1] = 0;
        wk->wu.routine_no[3]++;

        if (Round_num >= (save_w[Present_Mode].Battle_Number[Play_Type] * 2) ||
            PL_Wins[wk->wu.id] >= save_w[Present_Mode].Battle_Number[Play_Type] + 1) {
            work = win_select(wk, 7);
            set_char_move_init(&wk->wu, 9, Win_15000_tbl[work]);
        } else {
            work = win_select(wk, 3);
            set_char_move_init(&wk->wu, 9, work + 32);
        }

        break;

    default:
        Normal_normal_Winner(wk);
        break;
    }
}

s16 win_select(PLW* /* unused */, s16 num) {
    s16 work = random_16();
    work &= num;
    return work;
}

void bonus_game_win_pause(PLW* wk) {
    bg_app_stop = 1;

    if (set_field_hosei_flag(&plw[1], bs_scrrrl[1][0], 1)) {
        set_field_hosei_flag(&plw[1], bs_scrrrl[1][1], 0);
    }

    if (set_field_hosei_flag(&plw[0], bs_scrrrl[0][0], 1)) {
        set_field_hosei_flag(&plw[0], bs_scrrrl[0][1], 0);
    }

    switch (wk->wu.routine_no[3]) {
    case 0:
        wk->wu.routine_no[3]++;
        win_rno[0] = win_rno[1] = 0;

        if (Bonus_Game_Flag == 20) {
            if (wk->wu.wu_operator) {
                if (Time_Over) {
                    set_char_move_init(&wk->wu, 9, 67);
                } else {
                    set_char_move_init(&wk->wu, 9, 65);
                }

                break;
            }

            wk->wu.routine_no[3] = 99;
            break;
        }

        if (wk->wu.wu_operator) {
            if (Bonus_Game_result == 20 || Bonus_Game_ex_result == 20) {
                set_char_move_init(&wk->wu, 9, 65);
                break;
            }

            if (Bonus_Game_result > 10) {
                set_char_move_init(&wk->wu, 9, 66);
                break;
            }

            set_char_move_init(&wk->wu, 9, 67);
            break;
        }

        if (Bonus_Game_result == 20 || Bonus_Game_ex_result == 20) {
            win_rno[0] = 1;

            if (wk->wu.rl_flag) {
                wk->wu.mvxy.a[0].sp = 0x20000;
            } else {
                wk->wu.mvxy.a[0].sp = -0x20000;
            }

            wk->wu.mvxy.d[0].sp = 0;
            wk->wu.mvxy.a[1].sp = 0x80000;
            wk->wu.mvxy.d[1].sp = -0x6000;
            win_rno[0] = 0;
            set_char_move_init(&wk->wu, 9, 66);
            break;
        }

        set_char_move_init(&wk->wu, 9, 52);
        break;

    case 1:
    case 9:
        char_move(&wk->wu);
        break;
    }
}

const s16 meta_win_tbl[20] = { 33, 32, 32, 32, 32, 32, 33, 32, 32, 37, 32, 32, 32, 32, 34, 32, 32, 32, 32, 32 };

void meta_win_pause(PLW* wk) {
    bg_app_stop = 1;

    switch (wk->wu.routine_no[3]) {
    case 0:
        wk->wu.routine_no[3]++;
        set_char_move_init(&wk->wu, 9, meta_win_tbl[wk->player_number]);
        break;

    case 1:
    case 9:
        char_move(&wk->wu);
        break;
    }

    if (Bonus_Game_Flag) {
        if (set_field_hosei_flag(&plw[1], bs_scrrrl[1][0], 1)) {
            set_field_hosei_flag(&plw[1], bs_scrrrl[1][1], 0);
        }

        if (set_field_hosei_flag(&plw[0], bs_scrrrl[0][0], 1)) {
            set_field_hosei_flag(&plw[0], bs_scrrrl[0][1], 0);
        }
    } else {
        if (set_field_hosei_flag(&plw[wk->wu.id], scrr, 1)) {
            set_field_hosei_flag(&plw[wk->wu.id], scrl, 0);
        }
    }
}
