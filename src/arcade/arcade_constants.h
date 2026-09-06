#ifndef ARCADE_CONSTANTS_H
#define ARCADE_CONSTANTS_H

/* test_flag (CPS3 0x02000094, u8). The cabinet's test/service flag: the
 * arcade's own globals are scattered through the low 0x100 bytes of work RAM
 * (`Init_Task_1st` zeroes 0x02000094 alongside 0x0200001D and Random_ix16),
 * so a low address here is not a red flag.
 *
 * Read off the sfiii3nr1 SH-2 program at four paired sites, all of the shape
 * `if (test_flag == 0 || ixbfw_cut == 0)`, which is a `||` and therefore
 * evaluates test_flag FIRST -- that is what tells the two apart:
 *   - `comm_ixfw` CPS3 0x0608A3(8A): `mov.l <0x02000094>,r?` / `mov.b @r?` /
 *     `tst` / `bt` to the body, then the same on 0x02025638, then
 *     `wk->cg_ix += (ctc->pat - 1) * wk->cgd_type` -- the += site indexes wk
 *     at the literal 0x0204, i.e. the already-established
 *     WORK_CG_IX_OFFSET, which is what identifies the function.
 *   - `comm_ixbw` CPS3 0x0608A3BA: identical, with `-=` and `pat + 1`.
 *   - the two `effk5.c` opcode cases, CPS3 0x0610FC4A and 0x0610FC78.
 * Corroborated by the writers: `Init_Task_1st` zeroes it (CPS3 0x06004F38)
 * and the service routine at CPS3 0x06005518 sets it to 1 -- reached only when
 * the switch word at 0x0206AA9C has bit 1 set, which is what a test flag is.
 *
 * Measured 0 on all 143 corpus segments, and our port's `Init_Task_1st` sets
 * 0 too. Note what that does NOT establish: a field that is 0 on both sides
 * everywhere cannot distinguish a correct offset from a wrong-but-zero one.
 * The address rests on the disassembly above, not on the corpus. */
#define TEST_FLAG_OFFSET 0x94
/* EXE_flag (CPS3 0x0200EECC, s16) and Game_pause (CPS3 0x0201136E, s16) -- the
 * two in-battle freeze flags. Every `if (!EXE_flag && !Game_pause)` in
 * `src/sf33rd/Source/Game/effect/` is one gate on this pair, and the arcade
 * reads them in that order, EXE_flag first.
 *
 * Both addresses were established by disassembly of the sfiii3nr1 SH-2 program
 * for E6 (docs/research-arcade-balance-desyncs.md); they are recorded in the
 * two port modules that were transcribed from it, at three independent sites:
 *   - `effect_C08_move` routine 1, CPS3 0x060DD918: `mov.w @r4,r0` / `tst` /
 *     `bra 0x060DDA4E`, r4 holding 0x0200EECC from the prologue
 *     (`mov.l 0x060DD974,r4` at 0x060DD890), then `mov.l 0x060DD98C,r3`
 *     (= 0x0201136E) / `mov.w @r3,r0` / `tst` / `bra 0x060DDA4E`.
 *   - `effect_C08_move` routine 2, CPS3 0x060DDA84: the byte-for-byte twin,
 *     with `bf 0x060DDB6C` and the second Game_pause pool slot 0x060DDBD8.
 *   - `effect_C74_move` routine 1, CPS3 0x060F13D4: the same pair, both
 *     branching past the whole body to 0x060F14D6.
 * See the comments in `effect/effc08.c` and `effect/effc74.c` for the full
 * read-out, including the literal-pool argument that assigns TWO 0x0201136E
 * slots to effect 8 and one to effect 74.
 *
 * Both are 16-bit: the arcade reads each with `mov.w`, and the surrounding
 * fields at 0x02011370, 0x0201137A (Round_Level) and 0x0201137E are 16-bit at
 * even addresses too. Our port narrowed Game_pause to `u8` (`workuser.c`);
 * EXE_flag is `s16` on both sides (`slowf.h`).
 *
 * ARCHIVE CORROBORATION -- EVERY frame of both corpora (183 segments /
 * 1,167,121 frames for 2026-09-06, 143 / 869,986 for 2026-09-05), read
 * straight out of the .scrd frames, not inferred:
 *   - EXE_flag holds only 0, 1, 2 and 3, which is exactly the range of our
 *     `set_EXE_flag()`'s `EXE_flag = Game_timer % (SLOW_flag + 1)`
 *     (`engine/slowf.c`) -- a slow-motion divider, not a boolean.
 *   - Game_pause holds only 0, 1 and -1, and NOTHING else -- in particular no
 *     0x81, which is the archive-side half of the argument that the PS2 START
 *     pause has no arcade counterpart. It is 0 on ~95% of frames; the non-zero
 *     bursts are of two kinds, and they are NOT the same value:
 *       * -1 (0xFFFF), in runs of exactly 90 frames -- 424 of 426 runs on the
 *         one corpus, 333 of 335 on the other, the exceptions truncated by the
 *         segment boundary, and present on every segment of both. 90 is
 *         `Time_Data[1]` in `effect/eff84.c`, the K.O. round-message window,
 *         and our `effect_84_move` writes 1 there.
 *       * 1, in runs of 1, 8 and ~163 frames -- the `Game_Manage_*` screen
 *         transitions, where `engine/manage.c` writes 1 on both sides.
 *     Across a whole segment, EXE_flag freezes at its last value for the whole
 *     of every non-zero Game_pause run (measured frame by frame on
 *     1788133423462-3110 game_0, frames 2462-2551), which is our own
 *     `set_EXE_flag()`'s `if (!Game_pause)` guard -- i.e. the arcade treats -1
 *     as "paused" exactly as it treats 1, and nothing reads the magnitude.
 *   - Game_timer (0x0201136C, immediately below) keeps incrementing through
 *     both kinds of run, which is our `Game2_1()`'s `Game_pause != 0x81` gate:
 *     only the PS2-only START pause stalls it, and the arcade has no 0x81.
 *
 * See `compare_service_values()` (`src/test/statcheck_compare.c`) for what is
 * asserted and the one normalization that -1 forces. */
#define EXE_FLAG_OFFSET 0xEECC
#define GAME_TIMER_OFFSET 0x1136C
#define GAME_PAUSE_OFFSET 0x1136E
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
/* Country (CPS3 0x0201556F, u8). The cabinet REGION byte, and the root of two
 * further values -- our `Setup_Difficult_V()` derives CC_Value from it and
 * `Setup_Limit_Time()` derives Limit_Time from it, so one address resolves
 * three.
 *
 * Read off the sfiii3nr1 SH-2 program at three independent sites:
 *   - CPS3 0x06004EB2 (inside `Init_Task_1st`, just before its `bsr` to
 *     `Setup_Difficult_V` at 0x06005368): `mov.b @<0x0201556F>,r2` /
 *     `extu.b` / `add #-1` / `mov.b @(r0,r2),r2` off the byte table at
 *     0x0613D83D / `mov.b r2,@<0x0201584C>`. The arcade splits what our port
 *     inlines: it stores a country INDEX at 0x0201584C, and
 *     `Setup_Difficult_V` (CPS3 0x06005368) then reads that index, doubles it
 *     and copies two bytes out of `Difficult_V_Data` = 0x0613D845 into
 *     CC_Value. That table reads {0,0, 1,2, 2,4}, whose first two rows are our
 *     `Difficult_V_Data[2][2] = {{0,0},{1,2}}` verbatim, and the index table
 *     at 0x0613D83D reads {0,2,1,1,1,1,1,2,...} -- so index 0 (our
 *     `Country == 1` arm) is reached only by Country == 1.
 *   - CPS3 0x06092148: `switch (Country)` with `cmp/eq #1` .. `cmp/eq #8` in
 *     sequence, the 1..8 range our port's Country comparisons use.
 *   - CPS3 0x060F277C and 0x060F28A8: `cmp/eq #1` on the same byte.
 *
 * MEASURED: 1 on all 143 corpus segments (the ground truth is a Japanese
 * board). Our port hardcodes `Country = 4` in `njUserInit` (main.c), which is
 * a PS2-build constant -- see the seeding-gap section of
 * docs/research-arcade-balance-desyncs.md for what that difference gates. */
#define COUNTRY_OFFSET 0x1556F
#define PLAYER_COLOR_OFFSET 0x15683
/* No_Death (CPS3 0x02015761, s8). Nonzero means no damage lands:
 * `if (No_Death) { plw[0].wu.dm_vital = plw[1].wu.dm_vital = 0; }`.
 *
 * Read off the sfiii3nr1 SH-2 program at three independent sites -- the three
 * `plcnt_*_move` entry points our port has as `plcnt_move` (plcnt.c),
 * `plcnt_b_move` (plcnt2.c) and `plcnt_b2_move` (plcnt3.c), each of which
 * opens with the SAME pair of guarded double stores:
 *   CPS3 0x06116E0A, 0x06118C04, 0x06119470:
 *     `mov.l <0x02015761>,r3` / `mov.b @r3,r0` / `tst` / `bt` past the body,
 *     the body being `mov.w #0,@(0xA2, plw[1])` + `mov.w #0,@(0xA2, plw[0])`
 *     with the plw bases the established PLW_OFFSET (0x02068C6C) and
 *     PLW_OFFSET + PLW_SIZE (0x02069104), and 0xA2 the `wu.dm_vital` slot
 *     that `setup_vitality` zeroes at CPS3 0x0611E27A.
 *   Each is immediately followed by the identical block guarded on
 *   0x02011386, which is `Break_Into` -- the same source order our three
 *   functions have.
 * Corroborated by the writer: `Init_Task_1st` zeroes 0x02015761 (CPS3
 * 0x06004F74) in the same run of stores that zeroes Random_ix16 (0x020155E8)
 * and Random_ix32 (0x020155EA) at their already-established addresses.
 *
 * MEASURED: 0 on all 143 corpus segments; our port's `Init_Task_1st` sets 0. */
#define NO_DEATH_OFFSET 0x15761
#define PLAYERS_TIMER_OFFSET 0x157CE // players_timer (CPS3 0x020157CE)
/* CC_Value[2] (CPS3 0x0201584D, u8[2]). Derived from Country -- see
 * COUNTRY_OFFSET for the `Setup_Difficult_V` disassembly that establishes the
 * base. CC_Value[1] is confirmed a second time, independently, by
 * `setup_vitality` (CPS3 0x0611E20C): `mov.l <0x0201584E>,r6` / `mov.b @r6,r6`
 * / `extu.b` / `add r0,r6` where r0 is `save_w[..].Difficulty` -- our
 * `ix = CC_Value[1] + save_w[Present_Mode].Difficulty`, with the `extu.b` on
 * CC_Value and the sign-extending `mov.b` on Difficulty matching their u8 /
 * s8 types.
 *
 * MEASURED: {0, 0} on all 143 corpus segments (Country == 1 there). Both
 * entries are CPU-only reads -- CC_Value[0] appears only in `com/`, and
 * CC_Value[1] only on `setup_vitality`'s `wk->operator == 0` arm -- so on a
 * human-vs-human corpus they are inert. */
#define CC_VALUE_OFFSET 0x1584D
/* Limit_Time (CPS3 0x02016AD4, s16). The ceiling `Time_Control()` clamps
 * `Control_Time` to. Derived from Country -- see COUNTRY_OFFSET.
 *
 * Read off the sfiii3nr1 SH-2 program at four independent sites, three
 * readers and one writer:
 *   - `Time_Control` CPS3 0x06096D08: `mov.l <0x02011372>,r4` (Control_Time)
 *     / `mov.l <0x02016AD4>,r2` / `cmp/ge` / `mov.w r5,@r4` on the taken arm,
 *     then the `--Time_in_Time == 0 -> Time_in_Time = 60; Control_Time += 1`
 *     tail on 0x02011374 -- our `Time_Control()` (game.c) statement for
 *     statement.
 *   - `Update_Level_Control` CPS3 0x0609C24C: `Control_Time += 40` then
 *     `cmp/gt` against the same address and `Control_Time = Limit_Time` --
 *     our `Update_Level_Control()` (manage.c).
 *   - CPS3 0x060A0C1E: `Control_Time = Limit_Time` verbatim.
 *   - the writer, CPS3 0x06012570: `mov.w r4,@<0x02016AD4>` at the end of a
 *     run of `max(entry + 20, ...)` steps over a difficulty table -- the
 *     arcade's `Setup_Limit_Time`. It is NOT the port's: ours short-circuits
 *     the same computation to the literal `Country == 1 ? 1241 : 1061`, and
 *     neither 1241 nor a matching 1061 appears as a code literal in the
 *     arcade image. They agree on the value anyway (below), which is what
 *     makes the port's constant readable as a folded form of the arcade's max.
 *   `Control_Time` = 0x02011372 is itself established by the two sites that
 *   copy it to and from `SC_Personal_Time[PL_id]` = 0x0201583A (CPS3
 *   0x060993EA and 0x0609C7BE), our `Correct_Control_Time()` (sel_pl.c).
 *
 * MEASURED: 1241 on all 143 corpus segments -- exactly what our
 * `Setup_Limit_Time()` produces for Country == 1 and Difficulty == 2. With
 * our hardcoded Country == 4 it produces 1061 instead. */
#define LIMIT_TIME_OFFSET 0x16AD4
/* Max_vitality (CPS3 0x02016B30, s16). Starting HP AND the damage divisor.
 *
 * Read off the sfiii3nr1 SH-2 program at four independent sites, two readers
 * and two writers:
 *   - `setup_vitality` CPS3 0x0611E220: `mov.l <0x02016B30>,r5`, then
 *     `mov.w @r5,r0` as the DIVISOR of `original_vitality << 5` (our
 *     `wk->dmcal_d = (wk->original_vitality << 5) / Max_vitality`), and then
 *     `mov.w @r5,r3` stored to wk+0xA0, wk+0x9E and wk+0x9C -- our
 *     `wk->vitality = wk->vital_new = wk->vital_old = Max_vitality`, where
 *     0x9E is the already-established WORK_VITAL_NEW_OFFSET. The same
 *     function is anchored by `Com_Vital_Unit_Data` = 0x0616E1B8, whose row 0
 *     {1593,1687,...,2625} occurs exactly once in the image and is referenced
 *     by exactly one literal (0x0611E2DC, loaded at 0x0611E218).
 *   - `cal_dm_vital_gauge_hosei` CPS3 0x0611E288: `mov.w @r5,r1` then
 *     `r1 * 6 / 10` compared against wk+0x9E (our
 *     `if (wk->wu.vital_new < (Max_vitality * 6) / 10)`), and at 0x0611E2A2
 *     `mov.w @r5,r2` / `cmp/eq <0x00C0>` -- our live `if (Max_vitality ==
 *     192)` branch.
 *   - the two writers, in one arcade routine at CPS3 0x060053DC that branches
 *     on the byte at 0x0206AC62: `mov.w <0x00C0>,r2` / `mov.w r2,@r1` (192)
 *     on the nonzero arm at 0x06005450, and `mov.w <0x00A0>,r3` /
 *     `mov.w r3,@r2` (160) on the zero arm at 0x06005496. So the "the engine
 *     expects a second value" reading of the ==192 branch is correct, and the
 *     selector is a service byte.
 *
 * MEASURED: 160 on all 143 corpus segments, with the selector byte at
 * 0x0206AC62 reading 0 there -- i.e. the archives take the 160 arm, which is
 * the arm our port's `Init_Task_1st` hardcodes. */
#define MAX_VITALITY_OFFSET 0x16B30
#define SCENE_CUT_OFFSET 0x16D30
/* ixbfw_cut (CPS3 0x02025638, u8). Read only in the `||` pair with test_flag
 * -- see TEST_FLAG_OFFSET for the four sites and why the `||` ordering is
 * what assigns the two addresses. The arcade has extra writers our port does
 * not (e.g. CPS3 0x060B22C8 sets it from a switch word); the port keeps only
 * `Init_Task_1st`'s zero, which is why our copy has a single writer.
 *
 * MEASURED: 0 on all 143 corpus segments; our port sets 0. The same caveat as
 * test_flag applies -- 0 on both sides everywhere is not evidence for the
 * offset, only consistency with it. */
#define IXBFW_CUT_OFFSET 0x25638
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
/* save_w[Present_Mode].Difficulty / .Damage_Level (CPS3 0x0206AC63 /
 * 0x0206AC64, u8). The two cabinet service settings the fighter sim reads
 * unconditionally, from `setup_vitality` (pls02.c):
 *   `Com_Vital_Unit_Data[pno][save_w[Present_Mode].Damage_Level][ix]`
 * with `ix = CC_Value[1] + save_w[Present_Mode].Difficulty` on the CPU arm,
 * and from `Setup_Limit_Time()` (sys_sub.c). `save_w` is not in the GS_SAVE
 * whitelist so it fell outside the 607-global carried-state count, but it is
 * the same class of setting as Round_Level.
 *
 * Read off `setup_vitality` CPS3 0x0611E202: `mov.l <0x0206AC62>,r7`, then
 * `mov.b @(1,r7),r0` added to CC_Value[1] (Difficulty) and `mov.b @(2,r7),r0`
 * scaled by 24 = 12 s16 (Damage_Level) before indexing Com_Vital_Unit_Data --
 * the 96/24 strides being exactly this port's `s16[20][4][12]`. The ARCADE
 * struct is not this port's `_SAVE_W` (ours has `_PAD_INFOR Pad_Infor[2]`
 * ahead of Difficulty and `Time_Limit`/`Battle_Number[2]` between the two
 * fields), so these are absolute addresses, not base + offsetof.
 *
 * MEASURED: Difficulty 2 and Damage_Level 1 on all 143 corpus segments --
 * which is `Game_Default_Data` (sys_sub.c) verbatim, so our port agrees. */
#define SAVE_W_DIFFICULTY_OFFSET 0x6AC63
#define SAVE_W_DAMAGE_LEVEL_OFFSET 0x6AC64

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
