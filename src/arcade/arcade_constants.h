#ifndef ARCADE_CONSTANTS_H
#define ARCADE_CONSTANTS_H

#define GAME_TIMER_OFFSET 0x1136C
#define COUNTER_HI_OFFSET 0x11376
#define COUNTER_LOW_OFFSET 0x11378
#define MY_CHAR_OFFSET 0x11387
#define ALLOW_A_BATTLE_F_OFFSET 0x11389
#define SUPER_ARTS_OFFSET 0x1138B // Super_Arts
/* Round_Level (CPS3 0x0201137A, s16). Confirmed by disassembly of the
 * sfiii3nr1 SH-2 program: it is the sole index of Pow_Control_Data_1
 * (0x06194A2C) in the two arcade damage routines at 0x0609E36C /
 * 0x0609E3FA, and the only address the five arcade writers touch --
 * Before_Select_Sub (0x06095234 `= 7`, 0x0609531A `= 3`), the demo init
 * (0x06097654 `= 7`), Loser_Sub (0x0609C61E `--`) and Update_VS_Data
 * (0x0609C884 `++`). */
#define ROUND_LEVEL_OFFSET 0x1137A
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
/* bg_w.quake_y_index (E5, docs/research-arcade-balance-desyncs.md). The s16
 * screen-quake countdown: a hit sets it, `ta0_move()` (`stage/tate00.c`)
 * decrements it once per frame, and the stage debris cohorts gate their
 * `random_16()` draws on it (`eff19_quake_sub`, `eff94_2000_0`,
 * `eff11_quake_sub`, ...).
 *
 * DO NOT derive this from BG_W_STAGE_OFFSET plus the port struct's offsetof.
 * The port's `BG` (`stage/bg.h`) puts `quake_y_index` 29 bytes past `stage`,
 * which is an odd address an SH-2 cannot hold an s16 at -- the arcade layout
 * differs. Read off the sfiii3nr1 SH-2 program at five independent sites,
 * three loading &bg_w = 0x02026BAC and indexing +44, two loading the whole
 * address as a literal:
 *   - `eff19_quake_sub`  CPS3 0x060E4BF8: `mov.l <&bg_w>,r12` / `mov #44,r0` /
 *     `mov.w @(r0,r12),r3` / `cmp/gt` against 2, then 8 and 14 -- the port's
 *     `<= 2` / `< 8` / `> 14` ladder, selecting eff19_s/m/l_tbl.
 *   - `eff94_2000_0`     CPS3 0x060F6D30: same load, `cmp/gt` 3 then
 *     `cmp/ge` 24 -- the port's `> 3` / `>= 24`.
 *   - `eff11_quake_sub`  CPS3 0x060E0A1A: same load, `cmp/gt` 1, then
 *     `shll` + index of eff11_quake_index_tbl -- the port's `> 1`.
 *   - `effect_A7_move`   CPS3 0x060F9476 and `effect_02_move` CPS3 0x060DCBE6:
 *     the scr_mv countdown stores `wu.scr_mv_y` straight to the literal
 *     0x02026BD8 (`mov.l <0x02026BD8>,r1` / `mov.w @(98,r14),r2` /
 *     `mov.w r2,@r1`).
 *
 * Corroborated in the archives: the s16 there sits at 0, jumps to 6 or 10 on a
 * hit and decays by exactly 1 per frame back to 0, which is `ta0_move()`'s
 * signature (`if (bg_w.quake_y_index > 0) bg_w.quake_y_index--`) and nothing
 * else's. */
#define BG_W_QUAKE_Y_INDEX_OFFSET 0x26BD8 // bg_w.quake_y_index (CPS3 0x02026BD8)
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

/* WORK.wu_operator (H4b, docs/research-arcade-balance-desyncs.md): 1 = a human
 * is on that side, 0 = the CPU AI drives it. WORK offset 3, so the two players
 * sit at PLW_OFFSET + 3 = 0x68C6F and PLW_OFFSET + PLW_SIZE + 3 = 0x69107.
 *
 * The port's own struct agrees -- `set_base_data()` (`plcnt.c`) writes
 * `wk->wu.wu_operator = Operator_Status[ix]` and `Setup_Play_Type()`
 * (`sys_sub.c`) derives `Play_Type` from the pair -- and the value read at
 * these two addresses partitions the 16-segment corpus exactly along the
 * PASS/FAIL/no-match line (see the E2b table in the research doc), which is
 * what confirms the offset rather than the struct arithmetic alone. */
#define WORK_WU_OPERATOR_OFFSET 3
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
