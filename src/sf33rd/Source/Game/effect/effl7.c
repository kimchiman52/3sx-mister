/**
 * @file effl7.c
 * TODO: identify what this effect does
 */

#include "sf33rd/Source/Game/effect/effl7.h"
#include "arcade/arcade_balance.h"
#include "bin2obj/char_table.h"
#include "common.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/animation/win_pl.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/caldir.h"
#include "sf33rd/Source/Game/engine/charset.h"
#include "sf33rd/Source/Game/engine/pls02.h"
#include "sf33rd/Source/Game/engine/slowf.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/rendering/texcash.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/stage/bg_sub.h"
#include "sf33rd/Source/Game/stage/ta_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"

// forward declaration
const s16 effl7_data_tbl[16];

void effect_L7_move(WORK_Other* ewk) {
    WORK* oya_ptr = (WORK*)ewk->my_master;

    if (Suicide[0] || (ewk->wu.dead_f)) {
        ewk->wu.routine_no[0] = 1;
        ewk->wu.disp_flag = 0;
    }

    switch (ewk->wu.routine_no[0]) {
    case 0:
        if ((!EXE_flag) && (!Game_pause)) {
            effl7_move(ewk);
        }

        pl_eff_trans_entry(ewk);
        break;

    case 1:
        ewk->wu.routine_no[0] += 1;
        poison_flag[oya_ptr->id] = 0;
        /* fallthrough */

    default:
        push_effect_work(&ewk->wu);
        break;
    }
}

void effl7_move(WORK_Other* ewk) {
    switch (ewk->wu.routine_no[1]) {
    case 0:
        ewk->wu.routine_no[1] += 1;
        ewk->wu.disp_flag = 1;
        ewk->wu.kage_flag = 1;
        ewk->wu.kage_hx = 0;
        ewk->wu.kage_hy = -10;
        ewk->wu.kage_prio = 71;
        ewk->wu.kage_char = 16;
        set_char_move_init(&ewk->wu, 0, ewk->wu.char_index);
        ewk->wu.old_rno[0] = 80;
        cal_initial_speed(&ewk->wu, ewk->wu.old_rno[0], ewk->wu.old_rno[1], ewk->wu.xyz[1].disp.pos);
        break;

    case 1:
        char_move(&ewk->wu);
        add_x_sub(&ewk->wu);
        add_y_sub(&ewk->wu);
        ewk->wu.old_rno[0]--;

        if (ewk->wu.old_rno[0] <= 0) {
            ewk->wu.routine_no[1] += 1;
            set_char_move_init(&ewk->wu, 0, 1);
        }

        break;

    default:
        // Do nothing
        break;

    case 2:
        char_move(&ewk->wu);

        if (ewk->wu.cg_type == 0xFF) {
            ewk->wu.routine_no[1] += 1;
            set_char_move_init(&ewk->wu, 1, ewk->wu.old_rno[2]);
        }

        break;

    case 3:
        char_move(&ewk->wu);

        if (ewk->wu.cg_type == 9) {
            ewk->wu.routine_no[1] += 1;
            ewk->wu.rl_flag ^= 1;
        }

        break;

    case 4:
        char_move(&ewk->wu);

        if (ewk->wu.cg_type == 0xFF) {
            ewk->wu.routine_no[1] += 1;
            set_char_move_init2(&ewk->wu, 0, 0, 3, 1);

            if (ewk->wu.rl_flag) {
                ewk->wu.mvxy.a[0].sp = 0x20000;
            } else {
                ewk->wu.mvxy.a[0].sp = -0x20000;
            }

            ewk->wu.mvxy.a[1].sp = 0;
        }

        break;

    case 5:
        char_move(&ewk->wu);
        add_x_sub(&ewk->wu);

        if (range_x_check3(ewk, 64) == 0) {
            ewk->wu.routine_no[1] += 1;
            ewk->wu.disp_flag = 0;
            ewk->wu.kage_flag = 0;
        }

        break;

    case 6:
        ewk->wu.routine_no[1] += 1;
        ewk->wu.routine_no[0] += 1;
        break;
    }
}

s32 effect_L7_init(WORK* wk, s32 /* unused */) {
    WORK_Other* ewk;
    s16 ix;
    s16 kind_w;

    if ((wk->work_id == 1) && (((PLW*)wk)->player_number != My_char[wk->id])) {
        return 0;
    }

    if (poison_flag[wk->id]) {
        return 0;
    }

    /* ARCADE: the CPS3 gate tests a different bit of the same word.
     *
     * effect_L7_init is CPS3 0x06113FC8, identified without reference to this
     * file's contents: effl7_data_tbl {55,56,57,...} as big-endian s16 occurs
     * exactly ONCE in the image, at 0x061CB064, and has exactly ONE literal
     * referrer, 0x0611416C, which lies inside 0x06113FC8. effmovejptbl
     * (0x061B883C) entry [217] -- this work's wu.id, set below -- is
     * 0x06113D54, effect_L7_move, the same function pair.
     *
     * The first two gates transcribe correctly; the third does not:
     *
     *   06113ffc  mov.w 0x61140c0,r4   ; r4 = 0x1000
     *   06113ffe  mov.w @(8,r13),r0    ; wk->id
     *   06114000  tst r0,r0 / bt 0x6114016
     *   06114004  mov.l 0x61140dc,r2   ; r2 = 0x0206AA90  (P2SW_0)
     *   0611400a  tst r4,r3 / bf ...   ; continue iff P2SW_0 & 0x1000
     *   06114016  mov.l 0x61140e0,r2   ; r2 = 0x0206AA8C  (P1SW_0)
     *   0611401c  tst r4,r1 / bf ...   ; continue iff P1SW_0 & 0x1000
     *
     * so the arcade tests bit 12 of the raw P1SW_0/P2SW_0 register, and the
     * port tested bit 0 -- SWK_UP. The surrounding anchors corroborate the
     * addresses: 0x061140D8 is poison_flag (0x020281A8, matching gate 2's
     * s16 index) and 0x061140E4 is pull_effect_work, called immediately after
     * with r4 = 4 exactly as below.
     *
     * WHICH BIT THAT IS, IN PORT COORDINATES. SWK_START is used here as "the
     * bit this pipeline carries arcade P1SW_0 bit 12 in", which is a
     * conversion identity, not a claim about what the arcade button IS. The
     * port's own raw-arcade -> SWK converter, src/test/replay_game.c ->
     * read_input_buff(), maps it that way and says so:
     *
     *     buff |= (raw_buff & (1 << 12)) << 2; // start
     *
     * 1 << 12 shifted left 2 is 1 << 14 == SWK_START. So whatever the cabinet
     * called that line, a p*sw_0 word produced by this port's conversion layer
     * carries arcade bit 12 at SWK_START, and testing SWK_START here is the
     * faithful transcription of `& 0x1000`. That bit 12 is physically START is
     * likely but NOT proven, and nothing here depends on it.
     *
     * PS2 keeps `& 1`: proven against the CPS3 program and not against the PS2
     * binary, so it is gated (2d74225d, 192291a4). SWK_UP is 1 << 0, so the
     * PS2 arm is bit-identical to what this function has always done.
     *
     * See Class B / E8 in docs/research-arcade-balance-desyncs.md. */
    const u16 gag_sw = ArcadeBalance_IsEnabled() ? (u16)SWK_START : (u16)SWK_UP;

    if (wk->id) {
        if (!(p2sw_0 & gag_sw)) {
            return 0;
        }
    } else if (!(p1sw_0 & gag_sw)) {
        return 0;
    }

    if ((ix = pull_effect_work(4)) == -1) {
        return -1;
    }

    ewk = (WORK_Other*)frw[ix];
    ewk->wu.be_flag = 1;
    ewk->wu.id = 217;
    ewk->wu.work_id = 16;
    ewk->master_id = wk->id;
    ewk->wu.cgromtype = 1;
    ewk->wu.my_col_mode = wk->my_col_mode;
    ewk->wu.my_col_code = wk->my_col_code + 1;
    ewk->wu.my_family = wk->my_family;
    ewk->my_master = wk;
    ewk->wu.rl_flag = wk->rl_flag;

    if (wk->rl_flag) {
        if (wk->xyz[0].disp.pos < bg_w.bgw[1].wxy[0].disp.pos) {
            ewk->wu.xyz[0].disp.pos = wk->xyz[0].disp.pos - 256;
        } else {
            ewk->wu.xyz[0].disp.pos = bg_w.bgw[1].wxy[0].disp.pos - (bg_w.pos_offset + 32);
        }

        ewk->wu.old_rno[1] = wk->xyz[0].disp.pos - 32;
    } else {
        if (wk->xyz[0].disp.pos > bg_w.bgw[1].wxy[0].disp.pos) {
            ewk->wu.xyz[0].disp.pos = wk->xyz[0].disp.pos + 256;
        } else {
            ewk->wu.xyz[0].disp.pos = bg_w.bgw[1].wxy[0].disp.pos + (bg_w.pos_offset + 32);
        }

        ewk->wu.old_rno[1] = wk->xyz[0].disp.pos + 32;
    }

    ewk->wu.xyz[1].disp.pos = wk->xyz[1].disp.pos - 12;
    ewk->wu.my_priority = 28;
    ewk->wu.position_z = 28;
    ewk->wu.char_table[0] = _etc3_char_table;
    ewk->wu.char_table[1] = _etc_char_table;
    ewk->wu.char_index = 0;
    ewk->wu.sync_suzi = 0;
    suzi_offset_set(ewk);
    kind_w = random_16();
    ewk->wu.old_rno[2] = effl7_data_tbl[kind_w];
    poison_flag[wk->id] = 1;
    ewk->wu.my_mts = 14;
    ewk->wu.my_trans_mode = get_my_trans_mode(ewk->wu.my_mts);
    return 0;
}

const s16 effl7_data_tbl[16] = { 55, 56, 57, 55, 56, 57, 55, 57, 55, 56, 57, 55, 56, 57, 56, 57 };
