#if defined(STATCHECK)

/* Seed audit -- see the header, and docs/research-arcade-balance-desyncs.md
 * ("The seed audit").
 *
 * WHY THIS EXISTS. Seven defects in one session shared a single shape: the
 * arcade carries state across a match boundary and the harness resets it.
 * `Round_Level` (E1a), `bg_w.stage` (H2), `t_pl_lvr` (H3), `players_timer`
 * (E2a), `wu_operator` (H4b), the match-start predicate (H1) and
 * `bg_w.quake_y_index` (E5). Each cost a separate investigation and two were
 * written up as ENGINE defects before being retracted. The cost is always the
 * same: the seed is wrong at the seed frame, nothing notices, and the mismatch
 * surfaces hundreds of frames later looking exactly like an engine bug --
 * `Game_timer` at frame 1, `s1_cnt` at 7, `routine_no` at 11.
 *
 * THE INVARIANT THAT MAKES IT CHEAP. At the seed frame our engine has not
 * executed any compared frame yet. Anything that differs there is an initial
 * condition, not behaviour. So the audit needs no model of the simulation: it
 * only has to notice.
 *
 * READ-ONLY. Nothing below assigns to an engine global. The audit runs after
 * Statcheck_SyncValues in StatcheckRunner_Prologue and must not perturb what
 * it measures.
 *
 * WHAT IS NOT AUDITED, and why. `arcade_constants.h` has ~46 offsets; not all
 * of them name a single engine value that can be soundly compared.
 *
 *  - PLW_OFFSET, PLW_SIZE, T_PL_LVR_OFFSET, WAZA_WORK_OFFSET, WCP_OFFSET,
 *    SUPER_ARTS_WORK_OFFSET, PIYORI_TYPE_OFFSET are struct BASES/strides, not
 *    fields. They are audited through the fields reached from them, below.
 *  - P1SW_0_OFFSET / P2SW_0_OFFSET hold the RAW ARCADE register layout
 *    (kicks at bits 7-9), while the port's p1sw_0/p2sw_0 hold the SWK layout
 *    (kicks at 8-10, bit 7 unused). See the LAYOUT INVARIANT comment at the
 *    top of statcheck_runner.c -- read_input_buff exists precisely because the
 *    two are different encodings. A bit-for-bit comparison would be
 *    fabricated, and a re-encoded one would only re-test read_input_buff.
 *  - WORK_CURR_RCA_OFFSET is `CatchTable* curr_rca` (include/structs.h): the
 *    archive holds a CPS3 address, our engine holds a host address. There is
 *    no address map between them, so no comparison is available.
 *  - WORK_CG_IX_OFFSET is audited; WORK_CG_ADD_XY_OFFSET is audited. The
 *    remaining WORK_* offsets each name one scalar and are all audited.
 */

#include "test/statcheck_seed_audit.h"
#include "arcade/arcade_constants.h"
#include "constants.h"
#include "sf33rd/Source/Game/engine/cmb_win.h"
#include "sf33rd/Source/Game/engine/cmd_data.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/slowf.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/count.h"
#include "test/statcheck_utils.h"

#include <SDL3/SDL.h>

#include <stdio.h>

/* Per-report cap. A whole-struct group (waza_work is 2 x 56 x 12 fields) can
 * mismatch wholesale; the COUNT is always exact, only the per-field lines are
 * bounded, because the first few names are what identifies the gap. */
#define SEED_AUDIT_MAX_LINES 40

static int s_mismatches = 0;   /* non-allowlisted */
static int s_expected = 0;     /* allowlisted, and actually differing */
static int s_lines = 0;
static bool s_ran = false;

/* STATCHECK_SEED_AUDIT_VERBOSE:
 *   unset/0 -- report only non-allowlisted mismatches (the default; an
 *              allowlisted difference is by construction one the harness has
 *              decided not to reproduce, and twenty of them on every clean run
 *              is exactly the noise that lets a real one go unread),
 *   1       -- also list the allowlisted differences,
 *   2       -- dump EVERY audited field, equal or not. This is the level that
 *              turns the audit into a positive control: it answers "is this
 *              field clean because the import works, or because both sides
 *              happen to hold the same value anyway?", which is a question a
 *              silence cannot answer. Used to establish that Round_Level is 3
 *              on both sides of all 143 corpus segments -- a true negative,
 *              not a blind spot. */
static int seed_verbose(void) {
    static int cached = -1;

    if (cached < 0) {
        const char* env = SDL_getenv("STATCHECK_SEED_AUDIT_VERBOSE");
        cached = (env != NULL && env[0] != '\0') ? SDL_atoi(env) : 0;
    }

    return cached;
}

static void seed_dump(const char* name, long long ours, long long theirs) {
    if (seed_verbose() < 2) {
        return;
    }

    fprintf(stderr, "statcheck-seed: value    %-34s ours=%lld cps3=%lld\n", name, ours, theirs);
}

static void seed_line(const char* tag, const char* name, long long ours, long long theirs, const char* reason) {
    if (s_lines >= SEED_AUDIT_MAX_LINES) {
        return;
    }

    s_lines += 1;
    fprintf(stderr,
            "statcheck-seed: %s %-34s ours=%lld cps3=%lld%s%s\n",
            tag,
            name,
            ours,
            theirs,
            (reason != NULL) ? "  -- " : "",
            (reason != NULL) ? reason : "");
}

/* A field that MUST agree at the seed frame. A difference here is an
 * unimported (or wrongly imported) initial condition. */
static void seed_cmp(const char* name, long long ours, long long theirs) {
    seed_dump(name, ours, theirs);

    if (ours == theirs) {
        return;
    }

    s_mismatches += 1;
    seed_line("MISMATCH", name, ours, theirs, NULL);
}

/* ALLOWLIST. A field the harness deliberately does not reproduce at the seed
 * frame. Still printed when it differs -- the value is diagnostic -- but it
 * does not make the seed dirty. Every call site carries its reason. */
static void seed_expect(const char* name, const char* reason, long long ours, long long theirs) {
    seed_dump(name, ours, theirs);

    if (ours == theirs) {
        return;
    }

    s_expected += 1;

    if (seed_verbose() >= 1) {
        seed_line("expected", name, ours, theirs, reason);
    }
}

static void seed_cmp_i(const char* base, int index, long long ours, long long theirs) {
    char name[64];
    SDL_snprintf(name, sizeof(name), "%s[%d]", base, index);
    seed_cmp(name, ours, theirs);
}

static void seed_expect_i(const char* base, int index, const char* reason, long long ours, long long theirs) {
    char name[64];
    SDL_snprintf(name, sizeof(name), "%s[%d]", base, index);
    seed_expect(name, reason, ours, theirs);
}

static void seed_expect_ij(const char* base, int i, int j, const char* reason, long long ours, long long theirs) {
    char name[64];
    SDL_snprintf(name, sizeof(name), "%s[%d][%d]", base, i, j);
    seed_expect(name, reason, ours, theirs);
}

/* ---- the audited set ---------------------------------------------------- */

/* Session/service globals the archive carries across the match boundary. This
 * is the class every known seeding defect fell into. */
static void audit_service(SDL_IOStream* io) {
    /* ALLOWLIST: Game_timer, C_No[0..3], G_No[2]. The seed frame is by
     * construction the frame BEFORE Game2_0() runs (ScrdGame_Init's H1
     * predicate: the (2,0,0) triple, with the NEXT frame showing
     * `Game_timer == 0 && G_No[2] == 3`). Game2_0() (game.c) writes
     * `Game_timer = 0; C_No[0..3] = 0; G_No[2] = 3` in one frame on BOTH
     * sides, so whatever these hold at the seed frame is overwritten before
     * the first compared frame. The carried-in Game_timer here is exactly
     * H1's diagnostic law (it equals len(previous segment) - 2). */
    seed_expect("Game_timer",
                "Game2_0() zeroes it on the next frame, on both sides (H1)",
                Game_timer,
                read_u16(io, GAME_TIMER_OFFSET));

    /* ALLOWLIST: Game_pause. Same proof as Game_timer above, from the same
     * statement block -- `Game2_0()` (game.c) writes `Game_pause = 0` two
     * lines after `Game_timer = 0`, on both sides, on the frame after the seed
     * frame. MEASURED, and the measurement is why this entry is here at all:
     * the archive carries `Game_pause == 1` INTO the seed frame on 50 of the
     * 183 eligible 2026-09-06 segments and 38 of the 143 2026-09-05 ones (the
     * tail of a `Game_Manage_*` screen transition), and holds 0 at
     * `start_index` -- the first compared frame -- on 183 of 183 and 143 of
     * 143. So the difference is real, frequent, and provably gone before the
     * oracle looks. Strict would have made an rc-1 run report rc 4 on a third
     * of the corpus for nothing.
     *
     * The rewrite claim is the load-bearing half (see "The allowlist" in
     * docs/research-arcade-balance-desyncs.md): if it were false the oracle
     * would still catch the difference, but the audit's silence would promote
     * a harness gap to an engine divergence. Here it is checked directly --
     * `compare_service_values()` asserts Game_pause on every compared frame,
     * so a Game_pause that failed to reach 0 by `start_index` fails loudly on
     * the first compared frame rather than going quiet. */
    seed_expect("Game_pause",
                "Game2_0() zeroes it on the next frame, on both sides (game.c)",
                (s16)Game_pause,
                read_s16(io, GAME_PAUSE_OFFSET));
    /* STRICT: EXE_flag. Not carried state -- `set_EXE_flag()` (engine/slowf.c)
     * recomputes it as `Game_timer % (SLOW_flag + 1)` on every unpaused frame,
     * and `Game2_0()` zeroes Game_timer -- but it costs one comparison and the
     * archive reads 0 at the seed frame on 183 of 183 and 143 of 143, so there
     * is no allowlist argument to make and nothing to lose by checking. Its
     * one live input, SLOW_flag, IS carried across the match boundary
     * (`init_slow_flag()` is what clears it), and a stale SLOW_flag on either
     * side would show up here first. */
    seed_cmp("EXE_flag", EXE_flag, read_s16(io, EXE_FLAG_OFFSET));
    seed_cmp("Counter_hi", Counter_hi, read_s16(io, COUNTER_HI_OFFSET));
    seed_cmp("Counter_low", Counter_low, read_s16(io, COUNTER_LOW_OFFSET));
    /* ALLOWLIST: Allow_a_battle_f -- `Game2_0()` writes `Allow_a_battle_f = 0`
     * in the same block as Game_timer/C_No/G_No[2] below, on both sides, on
     * the frame after the seed frame. (Clean on all 143 corpus segments as it
     * happens; allowlisted on the proof, not on the measurement.) */
    seed_expect("Allow_a_battle_f",
                "Game2_0() zeroes it on the next frame, on both sides (game.c)",
                Allow_a_battle_f,
                read_u8(io, ALLOW_A_BATTLE_F_OFFSET));
    seed_cmp("New_Challenger", New_Challenger, read_u8(io, NEW_CHALLENGER_OFFSET));
    /* ALLOWLIST: Scene_Cut. `Game02()` (game.c) recomputes it unconditionally
     * as its FIRST statement on every frame -- `Scene_Cut = Cut_Cut_Cut();`
     * (sys_sub.c), a pure function of the current button state -- before it
     * dispatches Game02_Jmp_Tbl[G_No[2]]. Both sides are fed the same button
     * word by read_input_buff from the first compared frame, so the seed value
     * cannot survive into anything. Measured differing on 111 of 143 corpus
     * segments, every one of which passes. */
    seed_expect("Scene_Cut",
                "Game02() recomputes it from the current input every frame (game.c)",
                Scene_Cut ? 1 : 0,
                read_u8(io, SCENE_CUT_OFFSET) ? 1 : 0);
    seed_cmp("round_timer", round_timer, read_u8(io, ROUND_TIMER_OFFSET));

    /* Imported by Statcheck_SyncValues. A mismatch on any of these five means
     * the IMPORT ITSELF is broken (wrong offset, wrong frame, wrong order) --
     * they are the audit's own self-test. */
    seed_cmp("Random_ix16 [seeded]", Random_ix16, read_s16(io, RANDOM_IX_16_OFFSET));
    seed_cmp("Random_ix32 [seeded]", Random_ix32, read_s16(io, RANDOM_IX_32_OFFSET));
    seed_cmp("players_timer [seeded]", players_timer, read_u16(io, PLAYERS_TIMER_OFFSET));
    seed_cmp("Round_Level [seeded]", Round_Level, read_s16(io, ROUND_LEVEL_OFFSET));

    /* bg_w.stage is imported through ScrdGame_Init + Debug_w[DEBUG_STAGE_SELECT]
     * (H2), NOT by Statcheck_SyncValues, and it is never compared -- so before
     * this audit nothing at all checked that the pin took effect. The archive
     * byte is an ARCADE character id; the port's bg_w.stage is a 3SX id, hence
     * the same CHAR_ARCADE_TO_3SX transform ScrdGame_Init applies. */
    seed_cmp("bg_w.stage [seeded]", bg_w.stage, CHAR_ARCADE_TO_3SX(read_u8(io, BG_W_STAGE_OFFSET)));

    seed_cmp("bg_w.quake_y_index", bg_w.quake_y_index, read_s16(io, BG_W_QUAKE_Y_INDEX_OFFSET));

    /* Cabinet / service globals (the seeding-gap section of
     * docs/research-arcade-balance-desyncs.md). Every one of these is
     * boot- or service-derived, has a single writer that runs long before any
     * segment starts, is never rewritten during a match, and is read by the
     * fighter sim -- the exact shape of the seven defects this audit exists
     * for. None of them is COMPARED frame by frame, so before this block
     * nothing in the harness would ever have noticed one being wrong.
     *
     * All are STRICT. Four of them were measured identical on both sides of
     * all 143 corpus segments (Max_vitality 160, No_Death 0, test_flag 0,
     * ixbfw_cut 0) and are kept for the same reason the other constant-and-
     * equal fields are: the cost is one comparison, and their absence is what
     * the last seven defects were made of.
     *
     * The other three -- Country, CC_Value and Limit_Time -- were NOT equal:
     * our port hardcodes `Country = 4` (main.c) where the ground truth reads
     * 1. Statcheck_SyncValues now seeds Country and re-runs
     * `Setup_Difficult_V()` / `Setup_Limit_Time()`, so these three assert the
     * DERIVATION as much as the import: CC_Value and Limit_Time are never
     * read from the archive, only recomputed from the seeded Country, and a
     * mismatch here would mean our two derivations disagree with the arcade's
     * (which compute the same values by different routes -- see
     * COUNTRY_OFFSET and LIMIT_TIME_OFFSET in arcade_constants.h). */
    seed_cmp("Country [seeded]", Country, read_u8(io, COUNTRY_OFFSET));
    seed_cmp("CC_Value[0] [derived]", CC_Value[0], read_u8(io, CC_VALUE_OFFSET));
    seed_cmp("CC_Value[1] [derived]", CC_Value[1], read_u8(io, CC_VALUE_OFFSET + 1));
    seed_cmp("Limit_Time [derived]", Limit_Time, read_s16(io, LIMIT_TIME_OFFSET));
    seed_cmp("Max_vitality", Max_vitality, read_s16(io, MAX_VITALITY_OFFSET));
    seed_cmp("No_Death", No_Death, (s8)read_u8(io, NO_DEATH_OFFSET));
    seed_cmp("test_flag", test_flag, read_u8(io, TEST_FLAG_OFFSET));
    seed_cmp("ixbfw_cut", ixbfw_cut, read_u8(io, IXBFW_CUT_OFFSET));

    /* save_w[Present_Mode]: the two cabinet service settings `setup_vitality`
     * (pls02.c) reads unconditionally. `save_w` is not in the GS_SAVE
     * whitelist, so it fell outside the 607-global carried-state count -- but
     * it is the same class of setting as Round_Level and belongs here.
     * Neither is seeded: both sides start from `Game_Default_Data` (sys_sub.c)
     * and the archives measure at its values (Difficulty 2, Damage_Level 1) on
     * all 143 segments, so a mismatch would be a genuine defect. */
    seed_cmp("save_w.Difficulty", save_w[Present_Mode].Difficulty, read_u8(io, SAVE_W_DIFFICULTY_OFFSET));
    seed_cmp("save_w.Damage_Level", save_w[Present_Mode].Damage_Level, read_u8(io, SAVE_W_DAMAGE_LEVEL_OFFSET));

    seed_cmp("cmb_all_stock[0]", cmb_all_stock[0], read_u8(io, CMB_ALL_STOCK_OFFSET));

    for (int i = 0; i < 2; i++) {
        seed_cmp_i("cmb_stock", i, cmb_stock[i], read_u8(io, CMB_STOCK_OFFSET + i));
        /* ALLOWLIST: waza_type -- scratch, not state. Its only writer is
         * `waza_type[cmd_id] = j` inside `cmd_move()`'s 56-entry loop
         * (cmd_main.c), which runs every frame, so it holds "the command slot
         * examined last". The oracle compares it inside `compare_wcp()`, under
         * the same 5-frame warm-up as wcp and waza_work, for the same reason. */
        seed_expect_i("waza_type",
                      i,
                      "cmd_move() overwrites it every frame (cmd_main.c); oracle-compared only after "
                      "the 5-frame warm-up",
                      waza_type[i],
                      read_s16(io, WAZA_TYPE_OFFSET + i * (int)sizeof(s16)));

        /* Match setup ScrdGame_Init reads and the runner drives through the
         * synthetic character select. My_char is compared in the port's own
         * (3SX) index space, like bg_w.stage above. */
        seed_cmp_i("My_char [seeded]", i, My_char[i], CHAR_ARCADE_TO_3SX(read_u8(io, MY_CHAR_OFFSET + i)));
        seed_cmp_i("Super_Arts [seeded]", i, Super_Arts[i], read_u8(io, SUPER_ARTS_OFFSET + i));
        seed_cmp_i("Player_Color [seeded]", i, Player_Color[i], read_u8(io, PLAYER_COLOR_OFFSET + i));
    }

    for (int i = 0; i < 4; i++) {
        const u16 c_no_cps3 = read_u16(io, C_NO_OFFSET + i * (int)sizeof(u16));
        seed_expect_i("C_No", i, "Game2_0() zeroes C_No[0..3] on the next frame, both sides (H1)", C_No[i], c_no_cps3);

        const u16 g_no_cps3 = read_u16(io, G_NO_OFFSET + i * (int)sizeof(u16));

        if (i == 0) {
            /* ALLOWLIST: G_No[0] is excluded from Statcheck_CompareValues too
             * (`if (i != 0)`), because the port's task dispatch does not track
             * the arcade's outer Game task index. Auditing it would report a
             * difference the comparison itself declines to make. */
            seed_expect_i("G_No", i, "not compared by the oracle either -- port task dispatch differs", G_No[i],
                          g_no_cps3);
        } else if (i == 2) {
            seed_expect_i("G_No", i, "Game2_0() sets G_No[2] = 3 on the next frame, both sides (H1)", G_No[i],
                          g_no_cps3);
        } else {
            seed_cmp_i("G_No", i, G_No[i], g_no_cps3);
        }
    }
}

/* Per-player WORK/PLW battle state.
 *
 * ALLOWLISTED AS A GROUP, and the reason is structural rather than
 * field-by-field: at the seed frame the two sides are not in comparable
 * situations. The archive's seed frame is the tail of the PREVIOUS match
 * (ScrdGame_Init's H1 predicate puts start_index on the frame Game2_0() ran,
 * so seed_frame = start_index - 1 is the frame before it), and it still holds
 * that match's players -- their positions, health, animation indices. Our
 * synthetic session has never played a match at all: `plw` reads all-zero
 * here, measured on every one of the 143 corpus segments. Comparing "fresh"
 * against "residue" cannot say whether the harness reproduced anything.
 *
 * It also cannot hide a defect, because the battle-start path rewrites every
 * one of these before the oracle looks at any of them:
 *   - `set_base_data()` (plcnt.c) rebuilds the WORK header and calls
 *     `cmd_init()` (cmd_main.c), which zeroes waza_work and wcp's waza_flag /
 *     waza_r for that player;
 *   - `plcnt_init()` (plcnt.c) and `appear_data_set()` (appear.c) set
 *     `wu.routine_no[4]` and `wu.xyz[0].disp.pos` from
 *     `app_type_tbl[own][opp][bg_w.stage]` (that is H2's mechanism);
 *   - `setup_vitality()` (pls02.c) sets `vital_new`.
 * and `Statcheck_CompareValues` only reaches this group when
 * `G_No[1] == 2 && G_No[2] == 1` -- the battle proper, strictly after all of
 * the above. At the seed frame G_No[2] is 0 on our side and 3 one frame later,
 * so the group is never compared with its seed value live.
 *
 * MEASURED, 143 segments: routine_no, cg_ix, pos.x, pos.y, vital_new and the
 * four mvxy components differ on 4-368 segment-halves each; every one of those
 * segments passes end to end.
 *
 * AND THE REWRITE CLAIM IS NOW CHECKED, NOT TRUSTED, for every scalar in this
 * group that the oracle also compares live -- `routine_no`, `pos.x`, `pos.y`,
 * `vital_new`, the four `mvxy` components and, since 2026-09-06, `wu.cg_ix`
 * (`compare_main_values()`, statcheck_compare.c). That matters because an
 * allowlist entry whose rewrite claim is FALSE is not missed, it is
 * misattributed: the oracle catches it hundreds of frames later and the
 * audit's silence promotes a harness gap (rc 4) into an engine divergence
 * (rc 1). That is exactly what H5 was. `cg_ix` was the last member of this
 * group that was allowlisted here with no live compare anywhere.
 *
 * `wu_operator` is the exception and stays STRICT. It is not previous-match
 * residue: our side gets it from the harness's own synthetic character select
 * (`Entry_Mark_Set` -> `Operator_Status[]`, entry.c -> `set_base_data()`), so
 * it IS the harness's responsibility at the seed frame, and it is the field
 * H4b's whole rejection rests on. Measured equal on all 143 segments. */
#define SEED_BATTLE_RESIDUE                                                                                            \
    "previous-match residue vs a session that has not played one; rebuilt by set_base_data()/appear_data_set() "        \
    "before the oracle compares it"

static void audit_players(SDL_IOStream* io) {
    for (int i = 0; i < 2; i++) {
        const Sint64 base = PLW_OFFSET + (Sint64)i * PLW_SIZE;

        seed_cmp_i("wu.wu_operator", i, plw[i].wu.wu_operator, read_u8(io, base + WORK_WU_OPERATOR_OFFSET));

        seed_expect_i("wu.hit_stop", i, SEED_BATTLE_RESIDUE, plw[i].wu.hit_stop, read_s16(io, base + WORK_HIT_STOP_OFFSET));
        seed_expect_i("wu.dm_stop", i, SEED_BATTLE_RESIDUE, plw[i].wu.dm_stop, read_s16(io, base + WORK_DM_STOP_OFFSET));
        seed_expect_i("wu.vital_new", i, SEED_BATTLE_RESIDUE, plw[i].wu.vital_new,
                      read_s16(io, base + WORK_VITAL_NEW_OFFSET));
        seed_expect_i("wu.cg_ix", i, SEED_BATTLE_RESIDUE, plw[i].wu.cg_ix, read_s16(io, base + WORK_CG_IX_OFFSET));
        seed_expect_i("wu.cg_add_xy", i, SEED_BATTLE_RESIDUE, plw[i].wu.cg_add_xy,
                      read_u16(io, base + WORK_CG_ADD_XY_OFFSET));

        seed_expect_i("pos.x", i, SEED_BATTLE_RESIDUE, plw[i].wu.xyz[0].disp.pos, read_s16(io, base + WORK_XYZ_OFFSET));
        seed_expect_i("pos.y", i, SEED_BATTLE_RESIDUE, plw[i].wu.xyz[1].disp.pos,
                      read_s16(io, base + WORK_XYZ_OFFSET + (int)sizeof(XY)));

        seed_expect_i("mvxy.a[0].sp", i, SEED_BATTLE_RESIDUE, plw[i].wu.mvxy.a[0].sp,
                      read_s32(io, base + WORK_MVXY_OFFSET));
        seed_expect_i("mvxy.a[1].sp", i, SEED_BATTLE_RESIDUE, plw[i].wu.mvxy.a[1].sp,
                      read_s32(io, base + WORK_MVXY_OFFSET + (int)sizeof(Reg32SpReal)));
        seed_expect_i("mvxy.d[0].sp", i, SEED_BATTLE_RESIDUE, plw[i].wu.mvxy.d[0].sp,
                      read_s32(io, base + WORK_MVXY_OFFSET + (int)sizeof(Reg32SpReal) * 2));
        seed_expect_i("mvxy.d[1].sp", i, SEED_BATTLE_RESIDUE, plw[i].wu.mvxy.d[1].sp,
                      read_s32(io, base + WORK_MVXY_OFFSET + (int)sizeof(Reg32SpReal) * 3));

        seed_expect_i("sa_stop_flag", i, SEED_BATTLE_RESIDUE, plw[i].sa_stop_flag,
                      read_u8(io, base + PLW_SA_STOP_FLAG_OFFSET));
        seed_expect_i("caution_flag", i, SEED_BATTLE_RESIDUE, plw[i].caution_flag,
                      read_u8(io, base + PLW_CAUTION_FLAG_OFFSET));
        seed_expect_i("cat_break_ok_timer", i, SEED_BATTLE_RESIDUE, plw[i].cat_break_ok_timer,
                      read_u8(io, base + PLW_CAT_BREAK_OK_TIMER_OFFSET));
        seed_expect_i("cat_break_reserve", i, SEED_BATTLE_RESIDUE, plw[i].cat_break_reserve,
                      read_u8(io, base + PLW_CAT_BREAK_RESERVE_OFFSET));
        seed_expect_i("hazusenai_flag", i, SEED_BATTLE_RESIDUE, plw[i].hazusenai_flag,
                      read_u8(io, base + PLW_HAZUSENAI_FLAG_OFFSET));
        seed_expect_i("do_not_move", i, SEED_BATTLE_RESIDUE, plw[i].do_not_move,
                      read_u8(io, base + PLW_DO_NOT_MOVE_OFFSET));

        for (int j = 0; j < 8; j++) {
            seed_expect_ij("wu.routine_no", i, j, SEED_BATTLE_RESIDUE, plw[i].wu.routine_no[j],
                           read_s16(io, base + WORK_ROUTINE_NO_OFFSET + j * 2));
        }

        seed_expect_i("piyori_type.now.h", i, SEED_BATTLE_RESIDUE, piyori_type[i].now.quantity.h,
                      read_s16(io, PIYORI_TYPE_OFFSET + i * (int)sizeof(PiyoriType) + (int)offsetof(PiyoriType, now)));
        seed_expect_i("super_arts.gauge.h", i, SEED_BATTLE_RESIDUE, super_arts[i].gauge.s.h,
                      read_s16(io, SUPER_ARTS_WORK_OFFSET + i * (int)sizeof(SA_WORK) + (int)offsetof(SA_WORK, gauge)));
        seed_expect_i("super_arts.store", i, SEED_BATTLE_RESIDUE, super_arts[i].store,
                      read_s16(io, SUPER_ARTS_WORK_OFFSET + i * (int)sizeof(SA_WORK) + (int)offsetof(SA_WORK, store)));
    }
}

/* t_pl_lvr -- imported by Statcheck_SyncValues (H3) and compared from the 6th
 * frame on. Strict: this is the defect H3 fixed, and the audit is the thing
 * that would have named it at the seed frame instead of at archive frame 7. */
static void audit_lvr(SDL_IOStream* io) {
    T_PL_LVR cps3[2];
    SDL_SeekIO(io, T_PL_LVR_OFFSET, SDL_IO_SEEK_SET);

    u16* ptr = (u16*)cps3;

    for (size_t i = 0; i < sizeof(T_PL_LVR); i++) {
        SDL_ReadU16BE(io, ptr);
        ptr++;
    }

    for (int i = 0; i < 2; i++) {
        const T_PL_LVR* a = &t_pl_lvr[i];
        const T_PL_LVR* b = &cps3[i];

        seed_cmp_i("t_pl_lvr.sw_new", i, a->sw_new, b->sw_new);
        seed_cmp_i("t_pl_lvr.sw_old", i, a->sw_old, b->sw_old);
        seed_cmp_i("t_pl_lvr.sw_chg", i, a->sw_chg, b->sw_chg);
        seed_cmp_i("t_pl_lvr.sw_now", i, a->sw_now, b->sw_now);
        seed_cmp_i("t_pl_lvr.old_now", i, a->old_now, b->old_now);
        seed_cmp_i("t_pl_lvr.now_lvbt", i, a->now_lvbt, b->now_lvbt);
        seed_cmp_i("t_pl_lvr.old_lvbt", i, a->old_lvbt, b->old_lvbt);
        seed_cmp_i("t_pl_lvr.new_lvbt", i, a->new_lvbt, b->new_lvbt);
        seed_cmp_i("t_pl_lvr.sw_lever", i, a->sw_lever, b->sw_lever);
        seed_cmp_i("t_pl_lvr.shot_up", i, a->shot_up, b->shot_up);
        seed_cmp_i("t_pl_lvr.shot_down", i, a->shot_down, b->shot_down);
        seed_cmp_i("t_pl_lvr.shot_ud", i, a->shot_ud, b->shot_ud);
        seed_cmp_i("t_pl_lvr.lvr_status", i, a->lvr_status, b->lvr_status);
        seed_cmp_i("t_pl_lvr.jaku_cnt", i, a->jaku_cnt, b->jaku_cnt);
        seed_cmp_i("t_pl_lvr.chuu_cnt", i, a->chuu_cnt, b->chuu_cnt);
        seed_cmp_i("t_pl_lvr.kyou_cnt", i, a->kyou_cnt, b->kyou_cnt);
        seed_cmp_i("t_pl_lvr.up_cnt", i, a->up_cnt, b->up_cnt);
        seed_cmp_i("t_pl_lvr.down_cnt", i, a->down_cnt, b->down_cnt);
        seed_cmp_i("t_pl_lvr.left_cnt", i, a->left_cnt, b->left_cnt);
        seed_cmp_i("t_pl_lvr.right_cnt", i, a->right_cnt, b->right_cnt);
        seed_cmp_i("t_pl_lvr.s1_cnt", i, a->s1_cnt, b->s1_cnt);
        seed_cmp_i("t_pl_lvr.s2_cnt", i, a->s2_cnt, b->s2_cnt);
        seed_cmp_i("t_pl_lvr.s3_cnt", i, a->s3_cnt, b->s3_cnt);
        seed_cmp_i("t_pl_lvr.s4_cnt", i, a->s4_cnt, b->s4_cnt);
        seed_cmp_i("t_pl_lvr.s5_cnt", i, a->s5_cnt, b->s5_cnt);
        seed_cmp_i("t_pl_lvr.s6_cnt", i, a->s6_cnt, b->s6_cnt);
        seed_cmp_i("t_pl_lvr.lu_cnt", i, a->lu_cnt, b->lu_cnt);
        seed_cmp_i("t_pl_lvr.ld_cnt", i, a->ld_cnt, b->ld_cnt);
        seed_cmp_i("t_pl_lvr.ru_cnt", i, a->ru_cnt, b->ru_cnt);
        seed_cmp_i("t_pl_lvr.rd_cnt", i, a->rd_cnt, b->rd_cnt);
        seed_cmp_i("t_pl_lvr.waza_num", i, a->waza_num, b->waza_num);

        /* waza_no is the one T_PL_LVR field `compare_lvr()` does not compare
         * (upstream's list omits it) -- and MEASURED over the 143-segment
         * corpus at the seed frame it is the ONLY one of the 34 that is ever
         * non-zero: 71 of 80 sampled player-slots carry a value in 2..47, and
         * every other field is 0 on both sides. So without this line the H3
         * import has nothing to check against on that corpus, which is exactly
         * what the retrodiction run showed. `read_t_pl_lvr` already copies the
         * whole struct, so this costs nothing and is a real test of the seed. */
        seed_cmp_i("t_pl_lvr.waza_no", i, a->waza_no, b->waza_no);
        seed_cmp_i("t_pl_lvr.wait_cnt", i, a->wait_cnt, b->wait_cnt);
        seed_cmp_i("t_pl_lvr.cmd_r_no", i, a->cmd_r_no, b->cmd_r_no);
    }
}

/* wcp / waza_work -- the command-recogniser working set.
 *
 * `wcp[]` and `waza_work[][0..47]` are AGGREGATE ONLY and allowlisted, because
 * our own match-start path provably rewrites them before the oracle ever
 * compares them: `cmd_init()` (`cmd_main.c`, reached from `set_base_data()`)
 * zeroes `wcp[cmd_id].waza_flag`, `wcp[cmd_id].waza_r` and -- under
 * `ArcadeBalance_IsEnabled()` -- `waza_work[cmd_id][0..47]`, and
 * `waza_compel_all_init()` then rebuilds `reset`/`btix`/`exdt`/`w_dead`/
 * `w_dead2` for every live index from the command table. What is left
 * (`sw_new`, `sw_now`, `sw_chg`, ...) self-corrects once `read_input_buff` is
 * feeding both sides the same button word, which is why
 * `Statcheck_CompareValues` skips all three groups for 5 archive frames
 * ("Wait a bit so that the game has time to clear garbage values"). A per-field
 * report for that set would be noise the oracle has already decided to ignore;
 * the two counts are still printed, because a swing in them is a cheap signal
 * that something about the entry state changed.
 *
 * `waza_work[][48..55]` IS AUDITED STRICTLY, and that is a correction. The
 * allowlist used to cover all 56 entries with the self-correction argument
 * above, and for entries 48..55 that argument is simply false: `cmd_init()`
 * clears 0x540 of the 0x620-byte block -- "leaving entries 48-55 intact",
 * deliberately, to match CPS3 -- so those eight entries CARRY ACROSS THE MATCH
 * BOUNDARY on both sides. The archive enters a segment with the previous
 * match's residue in them; a statcheck run enters it with zeros, because its
 * synthetic session has never played a match. That is the imported-state shape
 * this whole audit exists to catch, and the wholesale allowlist was hiding it:
 * two 2026-09-06 corpus segments failed `compare_waza_work` at archive frame 7
 * -- the second frame that loop runs at all -- and were graded rc=1, an engine
 * divergence, on a seed the audit called CLEAN. See Class C in
 * docs/research-arcade-balance-desyncs.md.
 *
 * ONLY THE LIVE ENTRIES ARE STRICT. `waza_compel_all_init()` sets
 * `waza_flag[i] = -1` for every index at or above `pl_cmd_num[char][6]`, and
 * `cmd_main.c` gates every read and write of `waza_work[cmd_id][j]` on
 * `waza_flag[j] != -1`, so a dead entry's residue is unreachable for the whole
 * match -- it cannot be an initial condition, and `compare_waza_work()` no
 * longer compares it either. `pl_cmd_num[][6]` reaches 48 for exactly one of
 * the twenty characters (`CHAR_TWELVE`, 50), so on nineteen characters this
 * adds zero comparisons and on Twelve it adds entries 48 and 49. The character
 * is `My_char[i]` -- `plcnt_init()` (`plcnt.c`) assigns
 * `wk->player_number = My_char[wk->wu.id]` -- and `My_char` is itself audited
 * strictly a few lines above, so this index is not taken on trust.
 *
 * SEEDED SINCE H5b, so this audit is now a self-test of that seed rather than
 * a standing report of a gap. `sync_waza_work_carried()`
 * (`statcheck_compare.c`) imports the twelve scalar fields below and
 * RECONSTRUCTS `w_ptr` as `&tbl[16]` from our own copy of the command table.
 * The earlier reading here -- that `WAZA_WORK::w_ptr` is a CPS3 address
 * (measured: 0x0619BDF8, 0x0619BE32, 0x0619BE64, 0x0619BE96 on the seed frame
 * of `1784875995078-5749_game_1`) with no map to a host pointer, so the
 * residual `w_type == 1` (`chk_move_jp[1] == check_1`) would make the port
 * follow a pointer it does not have -- was true of an ARBITRARY pointer and
 * false of this one. Those four addresses are 32 bytes past their entries'
 * table bases and are the same on every Twelve segment in all three corpora,
 * because an idle recogniser entry never leaves the two-state cycle
 * `check_init()` reloads it into. `sync_waza_work_carried()` seeds only states
 * that cycle can produce and leaves anything else untouched -- so a residue we
 * genuinely cannot place still lands here, still reports DIRTY, and still
 * exits 4. */
static void audit_wcp_waza(SDL_IOStream* io) {
    WORK_CP wcp_cps3[2];
    SDL_SeekIO(io, WCP_OFFSET, SDL_IO_SEEK_SET);
    SDL_ReadIO(io, wcp_cps3, sizeof(wcp_cps3));

    int wcp_diff = 0;
    int wcp_total = 0;

    for (int i = 0; i < 2; i++) {
        WORK_CP* b = &wcp_cps3[i];

        b->sw_lvbt = SDL_Swap16BE(b->sw_lvbt);
        b->sw_new = SDL_Swap16BE(b->sw_new);
        b->sw_old = SDL_Swap16BE(b->sw_old);
        b->sw_now = SDL_Swap16BE(b->sw_now);
        b->sw_off = SDL_Swap16BE(b->sw_off);
        b->sw_chg = SDL_Swap16BE(b->sw_chg);
        b->old_now = SDL_Swap16BE(b->old_now);
        b->lgp = SDL_Swap16BE(b->lgp);

        const WORK_CP* a = &wcp[i];
        const s16 ours[14] = { a->sw_lvbt, a->sw_new, a->sw_old, a->sw_now,   a->sw_off, a->sw_chg, a->old_now,
                               a->lgp,     a->ca14,   a->ca25,   a->ca36,     a->calf,   a->calr,   a->lever_dir };
        const s16 theirs[14] = { b->sw_lvbt, b->sw_new, b->sw_old, b->sw_now,   b->sw_off, b->sw_chg, b->old_now,
                                 b->lgp,     b->ca14,   b->ca25,   b->ca36,     b->calf,   b->calr,   b->lever_dir };

        for (int k = 0; k < 14; k++) {
            wcp_total += 1;
            wcp_diff += (ours[k] != theirs[k]) ? 1 : 0;
        }

        for (int j = 0; j < 56; j++) {
            wcp_total += 1;
            wcp_diff += (a->waza_flag[j] != (s16)SDL_Swap16BE(b->waza_flag[j])) ? 1 : 0;
        }
    }

    int waza_diff = 0;
    int waza_total = 0;
    SDL_SeekIO(io, WAZA_WORK_OFFSET, SDL_IO_SEEK_SET);

    for (int i = 0; i < 2; i++) {
        /* Live index bound for this player's character: waza_compel_all_init()
         * (cmd_main.c) sets waza_flag[i] = -1 for every i >= pl_cmd_num[c][6]. */
        const int live_end = (My_char[i] < 20) ? (int)pl_cmd_num[My_char[i]][6] : 0;

        for (int j = 0; j < 56; j++) {
            WAZA_WORK b;
            SDL_zero(b);
            SDL_ReadS16BE(io, &b.w_type);
            SDL_ReadS16BE(io, &b.w_int);
            SDL_ReadS16BE(io, &b.free1);
            SDL_ReadS16BE(io, &b.w_lvr);

            u32 w_ptr;
            SDL_ReadU32BE(io, &w_ptr); /* w_ptr is a CPS3 address; never compared. */
            (void)w_ptr;

            SDL_ReadS16BE(io, &b.free2);
            SDL_ReadS16BE(io, &b.w_dead);
            SDL_ReadS16BE(io, &b.w_dead2);
            SDL_ReadS16BE(io, &b.uni0.tame.flag);
            SDL_ReadS16BE(io, &b.uni0.tame.shot_flag);
            SDL_ReadS16BE(io, &b.uni0.tame.shot_flag2);
            SDL_ReadS16BE(io, &b.free3);
            SDL_ReadS16BE(io, &b.shot_ok);

            const WAZA_WORK* a = &waza_work[i][j];
            const s16 ours[12] = { a->w_type,          a->w_int,         a->free1,
                                   a->w_lvr,           a->free2,         a->w_dead,
                                   a->w_dead2,         a->uni0.tame.flag, a->uni0.tame.shot_flag,
                                   a->uni0.tame.shot_flag2, a->free3,    a->shot_ok };
            const s16 theirs[12] = { b.w_type,          b.w_int,         b.free1,
                                     b.w_lvr,           b.free2,         b.w_dead,
                                     b.w_dead2,         b.uni0.tame.flag, b.uni0.tame.shot_flag,
                                     b.uni0.tame.shot_flag2, b.free3,    b.shot_ok };

            /* Entries 48..55 that this character actually uses are carried
             * state, not warm-up scratch -- audit them strictly. */
            if ((j >= WAZA_WORK_CARRIED_FIRST) && (j < live_end)) {
                static const char* const kFieldNames[12] = {
                    "w_type", "w_int",     "free1",     "w_lvr",     "free2", "w_dead",
                    "w_dead2", "tame.flag", "tame.shot_flag", "tame.shot_flag2", "free3", "shot_ok"
                };

                for (int k = 0; k < 12; k++) {
                    char name[64];
                    SDL_snprintf(name, sizeof(name), "waza_work[%d][%d].%s", i, j, kFieldNames[k]);
                    seed_cmp(name, ours[k], theirs[k]);
                }

                continue;
            }

            for (int k = 0; k < 12; k++) {
                waza_total += 1;
                waza_diff += (ours[k] != theirs[k]) ? 1 : 0;
            }
        }
    }

    if (wcp_diff != 0) {
        s_expected += 1;
    }

    if ((wcp_diff != 0) && (seed_verbose() >= 1)) {
        fprintf(stderr,
                "statcheck-seed: expected wcp[]        %d of %d field(s) differ  -- the oracle skips wcp for "
                "5 frames; it self-corrects from the injected input word\n",
                wcp_diff,
                wcp_total);
    }

    if (waza_diff != 0) {
        s_expected += 1;
    }

    if ((waza_diff != 0) && (seed_verbose() >= 1)) {
        fprintf(stderr,
                "statcheck-seed: expected waza_work[]  %d of %d field(s) differ  -- same 5-frame warm-up "
                "window as wcp\n",
                waza_diff,
                waza_total);
    }
}

void StatcheckSeedAudit_Run(SDL_IOStream* io, int seed_frame) {
    if (s_ran || (io == NULL)) {
        return;
    }

    s_ran = true;

    audit_service(io);
    audit_players(io);
    audit_lvr(io);
    audit_wcp_waza(io);

    if (s_mismatches == 0) {
        fprintf(stderr,
                "statcheck-seed: CLEAN at seed frame %d (%d allowlisted difference(s))\n",
                seed_frame,
                s_expected);
    } else {
        if (s_lines >= SEED_AUDIT_MAX_LINES) {
            fprintf(stderr, "statcheck-seed: ... report capped at %d lines\n", SEED_AUDIT_MAX_LINES);
        }

        fprintf(stderr,
                "statcheck-seed: DIRTY at seed frame %d -- %d field(s) differ BEFORE our engine has run a "
                "single compared frame. These are IMPORTED-STATE gaps, not engine divergences; any failure "
                "later in this run is suspect until they are explained.\n",
                seed_frame,
                s_mismatches);
    }

    fflush(stderr);
}

bool StatcheckSeedAudit_Dirty(void) {
    return s_mismatches > 0;
}

int StatcheckSeedAudit_MismatchCount(void) {
    return s_mismatches;
}

#endif
