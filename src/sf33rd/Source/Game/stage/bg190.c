/**
 * @file bg_190.c
 * Club Metro, France
 */

#include "sf33rd/Source/Game/stage/bg190.h"
#include "arcade/arcade_balance.h"
#include "common.h"
#include "sf33rd/Source/Game/effect/eff05.h"
#include "sf33rd/Source/Game/effect/eff06.h"
#include "sf33rd/Source/Game/effect/eff12.h"
#include "sf33rd/Source/Game/effect/eff44.h"
#include "sf33rd/Source/Game/effect/effc74.h"
#include "sf33rd/Source/Game/effect/effl4.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/stage/bg_data.h"
#include "sf33rd/Source/Game/stage/bg_sub.h"
#include "sf33rd/Source/Game/stage/ta_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"

void BG190() {
    bgw_ptr = &bg_w.bgw[1];
    bg1902();
    bgw_ptr = &bg_w.bgw[0];
    bg1901();
    bgw_ptr = &bg_w.bgw[2];
    sync_bg14_common();
    zoom_ud_check();
    bg_pos_hosei2();
    Bg_Family_Set();
}

void bg1901() {
    void (*bg1901_jmp[2])() = { bg1901_init00, bg_move_common };
    bg1901_jmp[bgw_ptr->r_no_0]();
}

void bg1901_init00() {
    bgw_ptr->r_no_0++;
    bgw_ptr->old_pos_x = bgw_ptr->xy[0].disp.pos = bgw_ptr->pos_x_work = 0x1D0;
    bgw_ptr->hos_xy[0].cal = bgw_ptr->wxy[0].cal = bgw_ptr->xy[0].cal;
    bgw_ptr->zuubun = 0;
}

void bg1902() {
    void (*bg1902_jmp[2])() = { bg1902_init00, bg_base_move_common };
    bg1902_jmp[bgw_ptr->r_no_0]();
}

void bg1902_init00() {
    bgw_ptr->r_no_0++;
    bgw_ptr->old_pos_x = bgw_ptr->xy[0].disp.pos = bgw_ptr->pos_x_work = 0x1D0;
    bgw_ptr->hos_xy[0].cal = bgw_ptr->wxy[0].cal = bgw_ptr->xy[0].cal;
    bgw_ptr->zuubun = 0;
    effect_05_init();
    effect_06_init();
    // CPS3 spawns its effect 74 here (arcade order 5, 6, 74, 14, 14, L4, 44,
    // 12). Ordering inside class 4 is load-bearing: it fixes the frame on
    // which each work's move routine draws. See
    // docs/research-arcade-cg-data-accuracy.md §23.10.
    //
    // GATED, for the reason spelled out at the `effect_C08_init()` call in
    // `bg030.c`: `effc74.c` has no PS2 counterpart (the PS2 build gave id 74
    // to a select-screen effect, §23.8), so under PS2 balance this was an RNG
    // consumer the PS2 engine does not have.
    if (ArcadeBalance_IsEnabled()) {
        effect_C74_init();
    }
    effect_L4_init();
    effect_44_init(6);
    effect_12_init(3);
}

void sync_bg14_common() {
    switch (bgw_ptr->r_no_0) {
    case 0:
        bgw_ptr->r_no_0++;
        bgw_ptr->old_pos_x = bgw_ptr->xy[0].disp.pos = bgw_ptr->pos_x_work = 0x1D0;
        bgw_ptr->hos_xy[0].cal = bgw_ptr->wxy[0].cal = bgw_ptr->xy[0].cal;
        bgw_ptr->xy[1].disp.pos = bgw_ptr->pos_y_work = 0;
        bgw_ptr->fam_no = 2;
        bgw_ptr->xy[0].disp.low = bgw_ptr->xy[1].disp.low = 0;
        bgw_ptr->y_limit = bgw_ptr->y_limit2 = 0xF0;
        bgw_ptr->speed_x = 0xD000;
        bgw_ptr->speed_y = 0xE000;
        sync_fam_set3(2);
        break;

    case 1:
        bg_x_move_check();
        bg_y_move_check();
        sync_fam_set3(2);
        break;
    }
}
