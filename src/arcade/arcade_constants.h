#ifndef ARCADE_CONSTANTS_H
#define ARCADE_CONSTANTS_H

#define GAME_TIMER_OFFSET 0x1136C
#define COUNTER_HI_OFFSET 0x11376
#define COUNTER_LOW_OFFSET 0x11378
#define MY_CHAR_OFFSET 0x11387
#define ALLOW_A_BATTLE_F_OFFSET 0x11389
#define SUPER_ARTS_OFFSET 0x1138B // Super_Arts
#define NEW_CHALLENGER_OFFSET 0x113DA
#define G_NO_OFFSET 0x15436
#define C_NO_OFFSET 0x154A6
#define RANDOM_IX_16_OFFSET 0x155E8
#define RANDOM_IX_32_OFFSET 0x155EA
#define PLAYER_COLOR_OFFSET 0x15683
#define PLAYERS_TIMER_OFFSET 0x157CE // players_timer (CPS3 0x020157CE)
#define SCENE_CUT_OFFSET 0x16D30
#define T_PL_LVR_OFFSET 0x2563C
#define WAZA_WORK_OFFSET 0x256C4
#define WAZA_TYPE_OFFSET 0x2630C
#define WCP_OFFSET 0x26318
/* bg_w.stage. Read off the arcade SH-2 program of sfiii3nr1 (H2,
 * docs/research-arcade-balance-desyncs.md): `appear_data_init_set` at
 * CPS3 0x060C00E8 loads &bg_w = 0x02026BAC and reads the stage with
 * `mov.b @(4,r7),r0`, and `Appear_07000`'s stage==12 test at CPS3
 * 0x060C0AA6 loads the whole address 0x02026BB0 as a literal.
 *
 * DO NOT derive this from a BG_W base plus the port struct's offsetof.
 * The arcade BG has one extra byte ahead of `stage`, so arcade `stage` is
 * at +4 / `area` at +5 where the port's bg.h has them at +3 / +4. */
#define BG_W_STAGE_OFFSET 0x26BB0 // bg_w.stage (CPS3 0x02026BB0)
#define ROUND_TIMER_OFFSET 0x28679
#define CMB_STOCK_OFFSET 0x2883C
#define CMB_ALL_STOCK_OFFSET 0x288A4
#define PLW_OFFSET 0x68C6C
#define SUPER_ARTS_WORK_OFFSET 0x6959C // super_arts
#define PIYORI_TYPE_OFFSET 0x695F4
#define P1SW_0_OFFSET 0x6AA8C
#define P2SW_0_OFFSET 0x6AA90

#define PLW_SIZE 0x498

#define PLW_SA_STOP_FLAG_OFFSET 0x41C
#define PLW_CAUTION_FLAG_OFFSET 0x428
#define PLW_CAT_BREAK_OK_TIMER_OFFSET 0x434
#define PLW_CAT_BREAK_RESERVE_OFFSET 0x435
#define PLW_HAZUSENAI_FLAG_OFFSET 0x436
#define PLW_DO_NOT_MOVE_OFFSET 0x455

#define WORK_ROUTINE_NO_OFFSET 0x24
#define WORK_HIT_STOP_OFFSET 0x44
#define WORK_XYZ_OFFSET 0x64
#define WORK_MVXY_OFFSET 0x7C
#define WORK_VITAL_NEW_OFFSET 0x9E
#define WORK_CURR_RCA_OFFSET 0x1FC
#define WORK_CG_IX_OFFSET 0x204
#define WORK_CG_ADD_XY_OFFSET 0x228
#define WORK_DM_STOP_OFFSET 0x32E

#endif
