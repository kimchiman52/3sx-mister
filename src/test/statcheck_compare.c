#if defined(STATCHECK)

/* Plan A3b (docs/plan-fcade-replay-browser.md): port of upstream
 * test_runner_compare.c (upstream/main @ 3376518f) with renamed entry points
 * (Statcheck_CompareValues/Statcheck_SyncValues). The comparison set is
 * byte-for-byte upstream's — every field it reads exists in the fork's
 * engine structs (proof: the fork's own DEBUG src/test/test_runner_compare.c
 * is content-identical in its compare bodies and compiles against the same
 * headers), so nothing had to be dropped.
 *
 * Fork-shaped differences from upstream (each cited inline where relevant):
 * - `stop_if` here reads configuration.headless (fork has no get_args();
 *   upstream:src/test/test_runner_compare.c:29-45 reads
 *   get_args()->statcheck.headless) and reports the failing archive frame
 *   before halting, per the A3b success criterion ("first mismatch printed
 *   with frame number").
 * - read_u8/read_u16/read_s16/read_s32 come from test/statcheck_utils.h
 *   (the DEBUG test_runner_utils.c is `#if DEBUG`-gated — plan A3b item 5).
 * - assert_equals comes from the shared test/test_assert.h (A3b extraction).
 * - upstream's sync_values dead code (commented-out lvr/wcp/waza sync
 *   experiments, their now-unused static helpers sync_lvr/sync_wcp/
 *   sync_waza_work, and a loop whose only live effect was an unused
 *   `character` local — upstream:test_runner_compare.c:451-511) is dropped:
 *   it would trip -Werror (-Wunused-function/-Wunused-variable) and has no
 *   behavior.
 * - compare_service_values' `frame` parameter (upstream: only used by a
 *   commented-out printf) is dropped for the same -Werror reason; the
 *   current frame is tracked file-statically for the failure report instead.
 */

#include "test/statcheck_compare.h"
#include "test/statcheck_seed_audit.h"
#include "arcade/arcade_balance.h"
#include "arcade/arcade_cmd_data.h"
#include "arcade/arcade_constants.h"
#include "constants.h"
#include "main.h"
#include "sf33rd/Source/Game/engine/cmb_win.h"
#include "sf33rd/Source/Game/engine/cmd_data.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/pls02.h"
#include "sf33rd/Source/Game/engine/slowf.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/init3rd.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/ui/count.h"
#include "test/statcheck_utils.h"
#include "test/test_assert.h"
#include "types.h"

#include <SDL3/SDL.h>

#include <signal.h>
/* dladdr() resolves an RNG call site's return address to a symbol name. On
 * Apple platforms <dlfcn.h> hides it behind _DARWIN_C_SOURCE whenever
 * _POSIX_C_SOURCE is defined, and this build defines the latter -- so ask for
 * the Darwin extensions before the include, and only there. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Position {
    s16 x;
    s16 y;
} Position;

/* Archive frame currently being compared; set at Statcheck_CompareValues
 * entry so a mismatch anywhere in the compare tree can report it. */
static Uint64 current_compare_frame = 0;

/* File-local stop_if consumed by test_assert.h's assert_equals. Mirrors
 * upstream:src/test/test_runner_compare.c:29-45, with configuration.headless
 * standing in for get_args()->statcheck.headless and the platform fallback
 * simplified to the fork's port/utils.c:73-85 shape (plain #else raise —
 * upstream's `__APPLE__ || linux` check misfires under strict-C cross
 * builds where the `linux` macro is not defined). */
static void stop_if(bool condition) {
    if (!condition) {
        return;
    }

    fprintf(stderr,
            "statcheck: FAIL at archive frame %llu\n",
            (unsigned long long)current_compare_frame);

    if (configuration.headless) {
        /* Exit 4, not 1, when the seed audit already found the run's initial
         * conditions dirty (docs/research-arcade-balance-desyncs.md, "The seed
         * audit"). Same rule H1 (exit 2) and H4b (exit 3) established: a
         * segment the harness could not set up correctly must never be
         * reported as an engine divergence, because that is the report that
         * gets acted on. 1 keeps its meaning -- "the engine diverged from
         * CPS3, with a seed the audit says was correct" -- and gets STRONGER
         * for it. Publication gating is unaffected either way: publish_3sr.py's
         * statcheck_gate is `clean = proc.returncode == 0`. */
        if (StatcheckSeedAudit_Dirty()) {
            fprintf(stderr,
                    "statcheck: this run's SEED was dirty (%d field(s), reported above at the seed frame). "
                    "Exiting 4, not 1: the mismatch above is not attributable to the engine until the seed "
                    "gaps are closed.\n",
                    StatcheckSeedAudit_MismatchCount());
            fflush(stderr);
            exit(4);
        }

        exit(1);
    } else {
#if defined(_WIN32)
        __debugbreak();
#else
        raise(SIGSTOP);
#endif
    }
}

// Data reading

static Sint64 calc_plw_offset(int player) {
    return PLW_OFFSET + player * PLW_SIZE;
}

static Position read_position(SDL_IOStream* io, int player) {
    const Sint64 xyz_offset = calc_plw_offset(player) + WORK_XYZ_OFFSET;
    const Sint64 x_offset = xyz_offset;
    const Sint64 y_offset = x_offset + sizeof(XY);
    return (Position) { .x = read_s16(io, x_offset), .y = read_s16(io, y_offset) };
}

static Position get_position(int player) {
    const XY* xyz = plw[player].wu.xyz;
    return (Position) { .x = xyz[0].disp.pos, .y = xyz[1].disp.pos };
}

static u8 read_allow_a_battle_f(SDL_IOStream* io) {
    return read_u8(io, ALLOW_A_BATTLE_F_OFFSET);
}

static u16 read_game_timer(SDL_IOStream* io) {
    return read_u16(io, GAME_TIMER_OFFSET);
}

static void read_wcp(SDL_IOStream* io, WORK_CP dst[2]) {
    SDL_SeekIO(io, WCP_OFFSET, SDL_IO_SEEK_SET);
    SDL_ReadIO(io, dst, sizeof(wcp));

    for (int i = 0; i < 2; i++) {
        WORK_CP* w = &dst[i];

        w->sw_lvbt = SDL_Swap16BE(w->sw_lvbt);
        w->sw_new = SDL_Swap16BE(w->sw_new);
        w->sw_old = SDL_Swap16BE(w->sw_old);
        w->sw_now = SDL_Swap16BE(w->sw_now);
        w->sw_off = SDL_Swap16BE(w->sw_off);
        w->sw_chg = SDL_Swap16BE(w->sw_chg);
        w->old_now = SDL_Swap16BE(w->old_now);
        w->lgp = SDL_Swap16BE(w->lgp);

        for (int j = 0; j < 56; j++) {
            w->waza_flag[j] = SDL_Swap16BE(w->waza_flag[j]);
            w->reset[j] = SDL_Swap16BE(w->reset[j]);
            w->btix[j] = SDL_Swap16BE(w->btix[j]);

            for (int k = 0; k < 4; k++) {
                w->exdt[j][k] = SDL_Swap16BE(w->exdt[j][k]);
            }
        }
    }
}

static void read_waza_work(SDL_IOStream* io, WAZA_WORK dst[2][56]) {
    SDL_SeekIO(io, WAZA_WORK_OFFSET, SDL_IO_SEEK_SET);

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 56; j++) {
            WAZA_WORK* wk = &dst[i][j];

            SDL_ReadS16BE(io, &wk->w_type);
            SDL_ReadS16BE(io, &wk->w_int);
            SDL_ReadS16BE(io, &wk->free1);
            SDL_ReadS16BE(io, &wk->w_lvr);

            u32 w_ptr;
            SDL_ReadU32BE(io, &w_ptr);
            wk->w_ptr = (s16*)(uintptr_t)w_ptr;

            SDL_ReadS16BE(io, &wk->free2);
            SDL_ReadS16BE(io, &wk->w_dead);
            SDL_ReadS16BE(io, &wk->w_dead2);
            SDL_ReadS16BE(io, &wk->uni0.tame.flag);
            SDL_ReadS16BE(io, &wk->uni0.tame.shot_flag);
            SDL_ReadS16BE(io, &wk->uni0.tame.shot_flag2);
            SDL_ReadS16BE(io, &wk->free3);
            SDL_ReadS16BE(io, &wk->shot_ok);
        }
    }
}

static void read_t_pl_lvr(SDL_IOStream* io, T_PL_LVR dst[2]) {
    SDL_SeekIO(io, T_PL_LVR_OFFSET, SDL_IO_SEEK_SET);

    u16* ptr = (u16*)dst;

    // T_PL_LVR consists of 16-bit ints. We need to read sizeof(T_PL_LVR) / 2 * 2 such ints
    for (size_t i = 0; i < sizeof(T_PL_LVR); i++) {
        SDL_ReadU16BE(io, ptr);
        ptr++;
    }
}

// Comparison

static void compare_main_values(SDL_IOStream* io) {
    const u8 allow_a_battle_f_cps3 = read_allow_a_battle_f(io);
    assert_equals(Allow_a_battle_f, allow_a_battle_f_cps3);

    const u8 round_timer_cps3 = read_u8(io, ROUND_TIMER_OFFSET);
    assert_equals(round_timer, round_timer_cps3);

    for (int i = 0; i < 2; i++) {
        const Sint64 plw_offset = calc_plw_offset(i);

        const s32 mvxy_a_x_3sx = plw[i].wu.mvxy.a[0].sp;
        const s32 mvxy_a_x_cps3 = read_s32(io, plw_offset + WORK_MVXY_OFFSET);
        assert_equals(mvxy_a_x_3sx, mvxy_a_x_cps3);

        const s32 mvxy_d_x_3sx = plw[i].wu.mvxy.d[0].sp;
        const s32 mvxy_d_x_cps3 = read_s32(io, plw_offset + WORK_MVXY_OFFSET + sizeof(Reg32SpReal) * 2);
        assert_equals(mvxy_d_x_3sx, mvxy_d_x_cps3);

        const s32 mvxy_a_y_3sx = plw[i].wu.mvxy.a[1].sp;
        const s32 mvxy_a_y_cps3 = read_s32(io, plw_offset + WORK_MVXY_OFFSET + sizeof(Reg32SpReal));
        assert_equals(mvxy_a_y_3sx, mvxy_a_y_cps3);

        const s32 mvxy_d_y_3sx = plw[i].wu.mvxy.d[1].sp;
        const s32 mvxy_d_y_cps3 = read_s32(io, plw_offset + WORK_MVXY_OFFSET + sizeof(Reg32SpReal) * 3);
        assert_equals(mvxy_d_y_3sx, mvxy_d_y_cps3);

        const Position pos_3sx = get_position(i);
        const Position pos_cps3 = read_position(io, i);
        assert_equals(pos_3sx.x, pos_cps3.x);
        assert_equals(pos_3sx.y, pos_cps3.y);

        const s16 vital_new_3sx = plw[i].wu.vital_new;
        const s16 vital_new_cps3 = read_s16(io, plw_offset + WORK_VITAL_NEW_OFFSET);
        assert_equals(vital_new_3sx, vital_new_cps3);

        /* wu.cg_ix -- WHICH SPRITE CELL IS DRAWN, as opposed to which reaction
         * state machine is running. `routine_no` above says a work is in
         * damage reaction 43; `cg_ix` says which cell of it. The two can
         * disagree: the Elena electric-shock defect was a wrong cell under a
         * right reaction, and nothing in this file could have seen it.
         *
         * It is an OFFSET IN WORDS INTO `set_char_ad`, not a pointer and not a
         * global sprite id -- `charset.c` writes it as
         * `(ip - 1) * cgd_type - cgd_type`, and the arcade's `comm_ixfw`, the
         * function whose `wk->cg_ix += (ctc->pat - 1) * wk->cgd_type` at the
         * literal 0x0204 is what identified WORK_CG_IX_OFFSET in the first
         * place (see arcade_constants.h on TEST_FLAG_OFFSET), advances it the
         * same way. So both sides express it in the SAME space -- cells of the
         * current character's own script, relative to that character's
         * `set_char_ad` -- rather than in any global cell numbering the port's
         * character remap could shift. That is WHY it turns out to be
         * comparable; it is not the evidence THAT it is. The evidence is the
         * measurement.
         *
         * MEASURED, all three corpora at once, --headless:
         *   - 447 of 447 eligible segments PASS with this assert in, and all
         *     463 result lines (rc, seed verdict, `PASS -- compared archive
         *     frames a..b of n`) are byte-identical to the sweep without it.
         *     2,683,431 archive frames inside those windows, x2 players.
         *   - LIVENESS CONTROL, because a clean sweep proves nothing about a
         *     dead assert (the standard 91ad2da1 set): change it to
         *     `assert_equals((s16)(cg_ix_3sx + 1), cg_ix_cps3)` and every one
         *     of the 447 fails, all at archive frame 11, all on that line.
         *   - NON-DEGENERATE, straight out of the .scrd frames: on the seven
         *     Elena-vs-Ryu segments the archive's own cg_ix takes 20-112
         *     DISTINCT values per player per segment (ranges 0..522), so
         *     agreement on every frame is not agreement on a constant.
         *   - The scenario that broke last time IS in the compared window:
         *     Ryu is SA3/Denjin in all seven, and `routine_no[2]` takes the
         *     special-reaction values 43/68 on Elena on 24 frames across five
         *     of them -- every one inside that segment's PASS range, with
         *     cg_ix 0/4/8/16/18/20/28 on those frames.
         *
         * This closes the shape that hid `waza_work` (H5): allowlisted in
         * `statcheck_seed_audit.c` as battle residue on a rewrite claim, with
         * no per-frame compare to catch the claim being wrong. Same treatment
         * as Game_pause's allowlist -- the audit still allowlists the seed
         * value, and this line checks the rewrite actually happened. */
        const s16 cg_ix_3sx = plw[i].wu.cg_ix;
        const s16 cg_ix_cps3 = read_s16(io, plw_offset + WORK_CG_IX_OFFSET);
        assert_equals(cg_ix_3sx, cg_ix_cps3);

        const s16 stun_3sx = piyori_type[i].now.quantity.h;
        const s16 stun_cps3 = read_s16(io, PIYORI_TYPE_OFFSET + i * sizeof(PiyoriType) + offsetof(PiyoriType, now));
        assert_equals(stun_3sx, stun_cps3);

        const s16 sa_gauge_3sx = super_arts[i].gauge.s.h;
        const s16 sa_gauge_cps3 = read_s16(io, SUPER_ARTS_WORK_OFFSET + i * sizeof(SA_WORK) + offsetof(SA_WORK, gauge));
        assert_equals(sa_gauge_3sx, sa_gauge_cps3);

        const s16 sa_store_3sx = super_arts[i].store;
        const s16 sa_store_cps3 = read_s16(io, SUPER_ARTS_WORK_OFFSET + i * sizeof(SA_WORK) + offsetof(SA_WORK, store));
        assert_equals(sa_store_3sx, sa_store_cps3);
    }
}

/* One line per diverging frame, then the callers that produced our side of it.
 * Bounded: after STATCHECK_RNG_DRIFT_MAX reports it goes quiet, because a run
 * that drifts every frame would otherwise bury the first divergence -- and the
 * FIRST one is the whole point. */
#define STATCHECK_RNG_DRIFT_MAX 40
static int s_rng_drift_reports = 0;

static void statcheck_report_rng_drift(int delta, s16 ours, s16 theirs) {
    if (s_rng_drift_reports >= STATCHECK_RNG_DRIFT_MAX) {
        return;
    }
    s_rng_drift_reports += 1;

    static const char* kWhich[4] = { "random_16", "random_32", "random_16_ex", "random_32_ex" };
    const int n = RngTrace_Count();
    fprintf(stderr, "statcheck-rng: frame %llu delta=%+d (ours ix16=%02x cps3=%02x) -- %d RNG call(s) this frame\n",
            (unsigned long long)current_compare_frame, delta, (unsigned)(ours & 0x3F), (unsigned)(theirs & 0x3F), n);

    for (int i = 0; i < n; i++) {
        const void* ra = RngTrace_Addr(i);
        const unsigned w = RngTrace_Which(i);
        Dl_info info;
        const char* sym = "?";
        if (ra != NULL && dladdr(ra, &info) != 0 && info.dli_sname != NULL) {
            sym = info.dli_sname;
        }
        fprintf(stderr, "statcheck-rng:   [%2d] %-12s <- %s (%p)\n", i, kWhich[w & 3], sym, ra);
    }
    fflush(stderr);
}

static void compare_service_values(SDL_IOStream* io, bool compare_characters) {
    const u16 game_timer_cps3 = read_game_timer(io);
    assert_equals(Game_timer, game_timer_cps3);

    const s16 counter_hi_cps3 = read_s16(io, COUNTER_HI_OFFSET);
    assert_equals(Counter_hi, counter_hi_cps3);

    const s16 counter_low_cps3 = read_s16(io, COUNTER_LOW_OFFSET);
    assert_equals(Counter_low, counter_low_cps3);

    const s16 random_ix16_cps3 = read_s16(io, RANDOM_IX_16_OFFSET);

    /* RNG call-count instrumentation (see the block comment in pls02.c).
     *
     * MUST run before the assert below, which ends the run on the very first
     * divergent frame. That ordering is what keeps the delta EXACT: every frame
     * that reaches here entered synced (the previous one asserted equal), and
     * both sides advance one step per call with `Random_ix16 &= 0x3F`. So this
     * delta IS (our calls - CPS3's calls) for this frame, for any true
     * difference under 64 -- and it is reported before the failure it explains.
     *
     * This used to be true for a different reason: the compare force-synced
     * `Random_ix16 = random_ix16_cps3` here on every frame, so the field could
     * never fail and every frame re-entered synced by fiat. That mask was
     * removed on 2026-09-05 (docs/research-arcade-balance-desyncs.md, "The
     * instrumentation"): it hid E2a for months and would have hidden the next
     * one the same way, because a repaired divergence reports as a pass.
     *
     * The trace then names which of OUR call sites ran. It cannot name the call
     * CPS3 made and we did not -- a negative delta means we are missing one, and
     * the answer to that is in the arcade disassembly, not here. */
    {
        const int d16 = (int)(((Random_ix16 - random_ix16_cps3) & 0x3F));
        const int delta = (d16 > 32) ? (d16 - 64) : d16; /* signed, shortest way round */
        if (delta != 0) {
            statcheck_report_rng_drift(delta, Random_ix16, random_ix16_cps3);
        }
    }

    assert_equals(Random_ix16, random_ix16_cps3);

    const s16 random_ix32_cps3 = read_s16(io, RANDOM_IX_32_OFFSET);
    assert_equals(Random_ix32, random_ix32_cps3);

    /* bg_w.quake_y_index is compared, never imported. It is not carried across
     * a match boundary -- both sides enter every corpus segment at 0 -- so
     * there is nothing for Statcheck_SyncValues to seed, and force-syncing it
     * per frame would be the Random_ix16 mask all over again. What it does is
     * gate the stage debris cohorts' random_16() draws, so an unasserted
     * divergence here surfaces as an unexplained Random_ix16 failure two lines
     * up. Asserting it names the cause instead: E5 was two port defects in the
     * quake writers (see the comments in eff02.c), and this is what keeps them
     * fixed. */
    const s16 quake_y_index_cps3 = read_s16(io, BG_W_QUAKE_Y_INDEX_OFFSET);
    assert_equals(bg_w.quake_y_index, quake_y_index_cps3);

    /* EXE_flag and Game_pause: the in-battle freeze pair every effect gates on.
     * `EXE_flag` and `Game_pause` appear TOGETHER on 210 lines of
     * `sf33rd/Source/Game/`, almost all of them the single conjunction
     * `if (!EXE_flag && !Game_pause)`, spread over 114 files -- effects,
     * `ui/count.c`, `engine/plcnt*.c`, `engine/vital.c`, `engine/stun.c`,
     * `engine/spgauge.c`, `stage/bg.c`. Offsets and their disassembly
     * provenance are on EXE_FLAG_OFFSET in arcade_constants.h.
     *
     * ASSERTED, NEVER SEEDED -- the same call E5 made for
     * bg_w.quake_y_index two lines up, and the opposite of players_timer.
     * Neither field is carried across a match boundary: `Game2_0()` (game.c)
     * writes `Game_pause = 0` on the frame after the seed frame, and
     * `set_EXE_flag()` (engine/slowf.c) recomputes EXE_flag from
     * `Game_timer % (SLOW_flag + 1)` every unpaused frame with Game_timer
     * itself zeroed by that same `Game2_0()`. So there is nothing for
     * Statcheck_SyncValues to import, both sides should already agree on every
     * compared frame, and a mismatch is an engine defect -- which is the point.
     * Force-syncing them per frame would be the Random_ix16 mask all over
     * again: it would repair the divergence and report it as a pass, and every
     * effect gated on the pair would go on diverging invisibly.
     *
     * What they catch that the RNG assert cannot: a freeze window that drifts
     * on a frame where no gated effect happened to draw. That frame passes the
     * Random_ix16 check and then bites hundreds of frames later, which is
     * exactly the shape E6 had. When a drift DOES move the RNG on the same
     * frame, Random_ix16 asserts first (it is above) and its call-site
     * backtrace is the more useful of the two reports -- that ordering is
     * deliberate and matches the quake assert's.
     *
     * Game_pause needs ONE normalization, and only one. Measured over EVERY
     * frame of both corpora -- 183 segments / 1,167,121 frames (2026-09-06)
     * and 143 / 869,986 (2026-09-05) -- the field takes exactly three values,
     * 0, 1 and -1, and NOTHING else (in particular, no 0x81). The arcade holds
     * -1 through the 90-frame K.O. round-message window where our
     * `effect_84_move` (effect/eff84.c) writes 1 -- 424 of 426 such runs are
     * exactly 90 frames on the one corpus and 333 of 335 on the other, and
     * every segment of both has at least one -- and holds 1 through the
     * `Game_Manage_*` transitions where `engine/manage.c` writes 1 on both
     * sides.
     *
     * WHY THE SUBSTITUTION IS SOUND, stated correctly. An earlier version of
     * this comment said "both engines consume the field only as a zero test".
     * That is TRUE of the arcade and FALSE of the port, and only the arcade
     * half is what the substitution needs -- the value being rewritten is the
     * ARCHIVE's, so what matters is what the arcade does with -1.
     *
     * The arcade side, MEASURED over the whole decrypted sfiii3nr1 image:
     * `0x0201136E` has **192** 4-aligned constant-pool entries, reached by
     * **237** `mov.l @(disp,PC),Rn` sites, giving **209** distinct
     * `mov.w @Rn,Rm` reads and **28** distinct `mov.w Rm,@Rn` writes. **208 of
     * the 209 reads are immediately followed by `tst Rm,Rm`**; the one
     * exception, `0x060F2A78`, is a `bt/s` delay slot whose consumer is
     * `exts.w r4,r4` + `tst r4,r4` at `0x060F2A92`/`0x060F2A94`. So on
     * hardware -1 and 1 are literally the same state -- no arcade site can
     * tell them apart -- and the archive corroborates it: EXE_flag freezes
     * across a -1 run exactly as across a 1 run.
     *
     * The port side is NOT a zero test, and that is a real difference this
     * assert does not adjudicate. `Game_pause` is `u8` here, so a -1 would be
     * 0xFF, and the port has mask and inequality readers that would separate
     * it from 1 -- several of them reachable in an arcade-mode battle:
     * `engine/cmb_win.c` (seven `Game_pause & 0x80`), `engine/spgauge.c`
     * (`(Game_pause & 0x80) || EXE_flag`), `stage/tate00.c`
     * (`if (Game_pause & 0x80) return;`), `effect/effa2.c` (six `& 0x80`),
     * `engine/plcnt.c` / `plcnt2.c` / `plcnt3.c` / `game.c` (`!= 0x81`),
     * `menu/menu.c` (`(Game_pause & 0x7F) != 0`), `ui/sc_sub.c` (`& 0x80`).
     * Our engine never writes -1, so none of them sees one today; the point is
     * that "the port is a zero test too" is not the reason this line is safe.
     * See the research doc's FP section for what that leaves open.
     *
     * Mapping that ONE value keeps the assert strict everywhere else: a window
     * that opens or closes on the wrong frame still fires, and so does any
     * value pair we have not seen.
     *
     * POSITIVE CONTROL, because a clean sweep proves nothing about a dead
     * assert. Remove the substitution and rebuild: the corpora go from
     * 182 PASS / 143 PASS to 182 FAIL / 143 FAIL, at archive frames 941-5,755
     * (median 2,270) and 921-4,255 (median 2,200) respectively, and EVERY one
     * of those 325 failures is the identical
     * line `game_pause_3sx (1) != game_pause_cps3_norm (-1)` -- no other value
     * pair occurs anywhere on either corpus. So (a) this assert is live, (b)
     * the compared window reaches a K.O. freeze on every single segment, which
     * is the coverage E6's 60-frame acceptance never had, and (c) the
     * substitution masks exactly one difference with nothing hiding behind
     * it. */
    const s16 exe_flag_cps3 = read_s16(io, EXE_FLAG_OFFSET);
    assert_equals(EXE_flag, exe_flag_cps3);

    const s16 game_pause_cps3 = read_s16(io, GAME_PAUSE_OFFSET);
    const s16 game_pause_cps3_norm = (game_pause_cps3 == -1) ? 1 : game_pause_cps3;
    const s16 game_pause_3sx = (s16)Game_pause;

    /* 0x81 is the PS2 START pause (`system/pause.c`, `menu/menu.c`); the
     * arcade cannot produce it, so if it ever reached here the assert below
     * would report a HARNESS fault as an engine divergence. It is unreachable
     * in a STATCHECK build and this is not a mask, it is a label on the one
     * report that would otherwise mislead. Every writer was traced:
     * `Setup_Pause` / `Setup_Come_Out` (pause.c) run only off `PAUSE_X`, which
     * only `Check_Pause_Term()` sets -- and that function returns 0
     * unconditionally under `#if defined(STATCHECK)`, above both the
     * SWK_START test and the controller-connection test (commit 3769c189);
     * `Check_SoftReset` (system/reset.c) needs `Reset_Status == 0x63`, which
     * needs `SWK_START | SWK_BACK` together and `read_input_buff`
     * (statcheck_runner.c) never emits SWK_BACK; the five `menu.c` writers
     * (`Setup_Tr_Pause`, `Reset_Training`, `Reset_Replay`, `Character_Change`,
     * `End_Replay_Menu`) are training/replay-menu paths and the harness runs
     * MODE_ARCADE; and `cpLoopTask`'s `Game_pause |= 0x80` (main.c) is
     * `#if defined(DEBUG)`, which cannot be co-compiled with STATCHECK. */
    if (Game_pause == GAME_PAUSE_TRAINING && game_pause_cps3 != GAME_PAUSE_TRAINING) {
        fprintf(stderr,
                "statcheck: Game_pause is 0x81 (the PS2 START pause) on our side at archive frame %llu. "
                "The arcade has no such value, so this is a HARNESS fault -- the STATCHECK carve-out in "
                "Check_Pause_Term() (system/pause.c) has regressed -- not an engine divergence.\n",
                (unsigned long long)current_compare_frame);
    }

    assert_equals(game_pause_3sx, game_pause_cps3_norm);

    const u8 cmb_stock_0_cps3 = read_u8(io, CMB_STOCK_OFFSET);
    const u8 cmb_stock_1_cps3 = read_u8(io, CMB_STOCK_OFFSET + 1);
    assert_equals(cmb_stock[0], cmb_stock_0_cps3);
    assert_equals(cmb_stock[1], cmb_stock_1_cps3);

    const u8 cmb_all_stock_cps3 = read_u8(io, CMB_ALL_STOCK_OFFSET);
    assert_equals(cmb_all_stock[0], cmb_all_stock_cps3);

    for (int i = 0; i < 4; i++) {
        const u16 c_no_cps3 = read_u16(io, C_NO_OFFSET + i * sizeof(u16));
        assert_equals(C_No[i], c_no_cps3);

        const u16 g_no_cps3 = read_u16(io, G_NO_OFFSET + i * sizeof(u16));

        if (i != 0) {
            assert_equals(G_No[i], g_no_cps3);
        }
    }

    if (!compare_characters) {
        return;
    }

    for (int i = 0; i < 2; i++) {
        const Sint64 plw_offset = calc_plw_offset(i);

        const u8 caution_flag_3sx = plw[i].caution_flag;
        const u8 caution_flag_cps3 = read_u8(io, plw_offset + PLW_CAUTION_FLAG_OFFSET);
        assert_equals(caution_flag_3sx, caution_flag_cps3);

        const s8 cat_break_ok_timer_3sx = plw[i].cat_break_ok_timer;
        const u8 cat_break_ok_timer_cps3 = read_u8(io, plw_offset + PLW_CAT_BREAK_OK_TIMER_OFFSET);
        assert_equals(cat_break_ok_timer_3sx, cat_break_ok_timer_cps3);

        const s8 cat_break_reserve_3sx = plw[i].cat_break_reserve;
        const u8 cat_break_reserve_cps3 = read_u8(io, plw_offset + PLW_CAT_BREAK_RESERVE_OFFSET);
        assert_equals(cat_break_reserve_3sx, cat_break_reserve_cps3);

        const s8 hazusenai_flag_3sx = plw[i].hazusenai_flag;
        const u8 hazusenai_flag_cps3 = read_u8(io, plw_offset + PLW_HAZUSENAI_FLAG_OFFSET);
        assert_equals(hazusenai_flag_3sx, hazusenai_flag_cps3);

        const u8 do_not_move_3sx = plw[i].do_not_move;
        const u8 do_not_move_cps3 = read_u8(io, plw_offset + PLW_DO_NOT_MOVE_OFFSET);
        assert_equals(do_not_move_3sx, do_not_move_cps3);

        for (int j = 0; j < 8; j++) {
            const s16 routine_no_3sx = plw[i].wu.routine_no[j];
            const s16 routine_no_cps3 = read_s16(io, plw_offset + WORK_ROUTINE_NO_OFFSET + j * 2);
            assert_equals(routine_no_3sx, routine_no_cps3);
        }

        const s16 dm_stop_3sx = plw[i].wu.dm_stop;
        const s16 dm_stop_cps3 = read_s16(io, plw_offset + WORK_DM_STOP_OFFSET);
        assert_equals(dm_stop_3sx, dm_stop_cps3);

        const s16 hit_stop_3sx = plw[i].wu.hit_stop;
        const s16 hit_stop_cps3 = read_s16(io, plw_offset + WORK_HIT_STOP_OFFSET);
        assert_equals(hit_stop_3sx, hit_stop_cps3);

        const u8 sa_stop_flag_3sx = plw[i].sa_stop_flag;
        const u8 sa_stop_flag_cps3 = read_u8(io, plw_offset + PLW_SA_STOP_FLAG_OFFSET);
        assert_equals(sa_stop_flag_3sx, sa_stop_flag_cps3);

        const u16 cg_add_xy_cps3 = read_u16(io, plw_offset + WORK_CG_ADD_XY_OFFSET);
        const u16 cg_add_xy_3sx = plw[i].wu.cg_add_xy;
        assert_equals(cg_add_xy_3sx, cg_add_xy_cps3);
    }
}

static void compare_lvr(SDL_IOStream* io) {
    T_PL_LVR t_pl_lvr_cps3[2];
    read_t_pl_lvr(io, t_pl_lvr_cps3);

    for (int i = 0; i < 2; i++) {
        const T_PL_LVR* lvr_cps3 = &t_pl_lvr_cps3[i];
        const T_PL_LVR* lvr_3sx = &t_pl_lvr[i];

        assert_equals(lvr_3sx->sw_new, lvr_cps3->sw_new);
        assert_equals(lvr_3sx->sw_old, lvr_cps3->sw_old);
        assert_equals(lvr_3sx->sw_chg, lvr_cps3->sw_chg);
        assert_equals(lvr_3sx->sw_now, lvr_cps3->sw_now);
        assert_equals(lvr_3sx->old_now, lvr_cps3->old_now);
        assert_equals(lvr_3sx->now_lvbt, lvr_cps3->now_lvbt);
        assert_equals(lvr_3sx->old_lvbt, lvr_cps3->old_lvbt);
        assert_equals(lvr_3sx->new_lvbt, lvr_cps3->new_lvbt);
        assert_equals(lvr_3sx->sw_lever, lvr_cps3->sw_lever);
        assert_equals(lvr_3sx->shot_up, lvr_cps3->shot_up);
        assert_equals(lvr_3sx->shot_down, lvr_cps3->shot_down);
        assert_equals(lvr_3sx->shot_ud, lvr_cps3->shot_ud);
        assert_equals(lvr_3sx->lvr_status, lvr_cps3->lvr_status);
        assert_equals(lvr_3sx->jaku_cnt, lvr_cps3->jaku_cnt);
        assert_equals(lvr_3sx->chuu_cnt, lvr_cps3->chuu_cnt);
        assert_equals(lvr_3sx->kyou_cnt, lvr_cps3->kyou_cnt);
        assert_equals(lvr_3sx->up_cnt, lvr_cps3->up_cnt);
        assert_equals(lvr_3sx->down_cnt, lvr_cps3->down_cnt);
        assert_equals(lvr_3sx->left_cnt, lvr_cps3->left_cnt);
        assert_equals(lvr_3sx->right_cnt, lvr_cps3->right_cnt);
        assert_equals(lvr_3sx->s1_cnt, lvr_cps3->s1_cnt);
        assert_equals(lvr_3sx->s2_cnt, lvr_cps3->s2_cnt);
        assert_equals(lvr_3sx->s3_cnt, lvr_cps3->s3_cnt);
        assert_equals(lvr_3sx->s4_cnt, lvr_cps3->s4_cnt);
        assert_equals(lvr_3sx->s5_cnt, lvr_cps3->s5_cnt);
        assert_equals(lvr_3sx->s6_cnt, lvr_cps3->s6_cnt);
        assert_equals(lvr_3sx->lu_cnt, lvr_cps3->lu_cnt);
        assert_equals(lvr_3sx->ld_cnt, lvr_cps3->ld_cnt);
        assert_equals(lvr_3sx->ru_cnt, lvr_cps3->ru_cnt);
        assert_equals(lvr_3sx->rd_cnt, lvr_cps3->rd_cnt);
        assert_equals(lvr_3sx->waza_num, lvr_cps3->waza_num);
        assert_equals(lvr_3sx->wait_cnt, lvr_cps3->wait_cnt);
        assert_equals(lvr_3sx->cmd_r_no, lvr_cps3->cmd_r_no);
    }
}

/* Entries the command recogniser never reads are not compared -- the same rule
 * `compare_wcp()` below already applies to `reset`/`btix`/`waza_r`/`exdt`, for
 * the same reason and now for a second one.
 *
 * REACHABILITY. Every access to `waza_work[cmd_id][j]` in `cmd_main.c` is
 * gated on `wcp[cmd_id].waza_flag[j] != -1`: both loops in `cmd_move()`
 * (`if (wcp[cmd_id].waza_flag[j] != -1)`), `waza_compel_all_init2()`, and
 * `cmd_data_set()` which `waza_compel_all_init()` calls only for live indices.
 * `waza_compel_all_init()` sets `waza_flag[i] = -1` for every index outside the
 * character's six live ranges (`pl_cmd_num[player_number][0..6]`), so a dead
 * entry is unreachable state for the whole match.
 *
 * WHY IT MATTERS HERE (Class C, docs/research-arcade-balance-desyncs.md).
 * Under `ArcadeBalance_IsEnabled()` `cmd_init()` (`cmd_main.c`) clears only the
 * first 48 entries -- "CPS3 clears 0x540 bytes of each 0x620-byte command-state
 * block, leaving entries 48-55 intact" -- so entries 48..55 CARRY ACROSS THE
 * MATCH BOUNDARY on both sides. The archive enters a segment with the previous
 * match's residue there; a statcheck run enters it with zeros, because its
 * synthetic session has never played a match. For a character whose live range
 * stops below 48 (19 of the 20 -- only `pl_cmd_num[CHAR_TWELVE][6] == 50`
 * reaches 48 and 49) that residue is dead state, and comparing it reported an
 * imported-state gap as an engine divergence.
 *
 * NOTHING IS MASKED BY USING OUR OWN `waza_flag`. If the two sides disagreed
 * about which entries are live, `compare_wcp()` asserts `waza_flag[j]` for all
 * 56 indices on this same frame, immediately after this function returns. */
static void compare_waza_work(SDL_IOStream* io) {
    WAZA_WORK waza_work_cps3[2][56];
    read_waza_work(io, waza_work_cps3);

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 56; j++) {
            if (wcp[i].waza_flag[j] == -1) {
                continue;
            }

            const WAZA_WORK* w_3sx = &waza_work[i][j];
            const WAZA_WORK* w_cps3 = &waza_work_cps3[i][j];

            assert_equals(w_3sx->w_type, w_cps3->w_type);
            assert_equals(w_3sx->w_int, w_cps3->w_int);
            assert_equals(w_3sx->free1, w_cps3->free1);
            assert_equals(w_3sx->w_lvr, w_cps3->w_lvr);
            assert_equals(w_3sx->free2, w_cps3->free2);
            assert_equals(w_3sx->w_dead, w_cps3->w_dead);
            assert_equals(w_3sx->w_dead2, w_cps3->w_dead2);
            assert_equals(w_3sx->uni0.tame.flag, w_cps3->uni0.tame.flag);
            assert_equals(w_3sx->uni0.tame.shot_flag, w_cps3->uni0.tame.shot_flag);
            assert_equals(w_3sx->uni0.tame.shot_flag2, w_cps3->uni0.tame.shot_flag2);
            assert_equals(w_3sx->free3, w_cps3->free3);
            assert_equals(w_3sx->shot_ok, w_cps3->shot_ok);
        }
    }
}

static void compare_wcp(SDL_IOStream* io) {
    WORK_CP wcp_cps3[2];
    read_wcp(io, wcp_cps3);

    for (int i = 0; i < 2; i++) {
        const s16 waza_type_cps3 = read_s16(io, WAZA_TYPE_OFFSET + i * sizeof(s16));
        assert_equals(waza_type[i], waza_type_cps3);

        const WORK_CP* w_3sx = &wcp[i];
        const WORK_CP* w_cps3 = &wcp_cps3[i];

        assert_equals(w_3sx->sw_lvbt, w_cps3->sw_lvbt);
        assert_equals(w_3sx->sw_new, w_cps3->sw_new);
        assert_equals(w_3sx->sw_old, w_cps3->sw_old);
        assert_equals(w_3sx->sw_now, w_cps3->sw_now);
        assert_equals(w_3sx->sw_off, w_cps3->sw_off);
        assert_equals(w_3sx->sw_chg, w_cps3->sw_chg);
        assert_equals(w_3sx->old_now, w_cps3->old_now);
        assert_equals(w_3sx->lgp, w_cps3->lgp);
        assert_equals(w_3sx->ca14, w_cps3->ca14);
        assert_equals(w_3sx->ca25, w_cps3->ca25);
        assert_equals(w_3sx->ca36, w_cps3->ca36);
        assert_equals(w_3sx->calf, w_cps3->calf);
        assert_equals(w_3sx->calr, w_cps3->calr);
        assert_equals(w_3sx->lever_dir, w_cps3->lever_dir);

        for (int j = 0; j < 56; j++) {
            assert_equals(w_3sx->waza_flag[j], w_cps3->waza_flag[j]);

            if (w_3sx->waza_flag[j] == -1) {
                continue;
            }

            assert_equals(w_3sx->reset[j], w_cps3->reset[j]);
            assert_equals(w_3sx->btix[j], w_cps3->btix[j]);

            for (int k = 0; k < 4; k++) {
                assert_equals(w_3sx->waza_r[j][k], w_cps3->waza_r[j][k]);
                assert_equals(w_3sx->exdt[j][k], w_cps3->exdt[j][k]);
            }
        }
    }
}

static Uint64 start_frame = 0;

void Statcheck_CompareValues(SDL_IOStream* io, Uint64 frame) {
    current_compare_frame = frame;

    /* Arm once, then reset the ring at every frame boundary so a report only
     * ever lists calls made by the frame that actually drifted. */
    static bool rng_trace_armed = false;
    if (!rng_trace_armed) {
        RngTrace_Enable(1);
        rng_trace_armed = true;
    }

    if (start_frame == 0) {
        start_frame = frame;
    }

    // Wait a bit so that the game has time to clear garbage values
    if (frame - start_frame > 5) {
        compare_lvr(io);
        compare_waza_work(io);
        compare_wcp(io);
    }

    const bool compare_characters = G_No[1] == 2 && G_No[2] == 1;
    compare_service_values(io, compare_characters);

    if (compare_characters) {
        compare_main_values(io);
    }

    /* Frame boundary. This runs AFTER the game frame it is comparing, so the
     * ring currently holds that frame's calls -- which is what a drift report
     * above just consumed. Clear it here so the next report lists the next
     * frame's calls and not a running total. */
    RngTrace_FrameBegin();
}

// Syncing

/* waza_work[][48..55] -- the CARRIED command-recogniser entries (H5b,
 * docs/research-arcade-balance-desyncs.md).
 *
 * `cmd_init()` (`cmd_main.c`) clears only `waza_work[cmd_id][0..47]` under
 * `ArcadeBalance_IsEnabled()`, reproducing CPS3's 0x540-of-0x620 memset, so
 * entries 48..55 CARRY ACROSS THE MATCH BOUNDARY. `waza_compel_all_init()`
 * marks an entry live only below `pl_cmd_num[char][6]`, and that bound reaches
 * past 48 for exactly one character: `pl_cmd_num[CHAR_TWELVE][6] == 50`
 * (`cmd_data.c`), i.e. entries 48 and 49 on Twelve and nothing anywhere else.
 * An archive whose session already played Twelve on this side enters the
 * segment with those two entries populated; a statcheck run enters with zeros,
 * because its synthetic session has never played a match.
 *
 * WHAT THE RESIDUE ACTUALLY IS. An idle recogniser entry runs a closed
 * two-state cycle driven entirely by the STATIC command table `tbl` for that
 * entry (`ArcadeCommandData_Get(char)[j]`, the same pointer `cmd_move()`
 * hands `chk_move_jp[]`):
 *
 *   - `check_init()` (w_type 0) reloads w_type/w_int/free1/free2/w_lvr from
 *     `tbl[12..15]`, sets `w_ptr = &tbl[16]`, zeroes the tame/shot fields, and
 *     dispatches the loaded handler in the SAME frame;
 *   - that handler, with no matching lever, restores `free2 = free1`,
 *     decrements `w_int`, and drops `w_type` back to 0 once it goes negative
 *     (`check_0`, and both arms of `check_1`).
 *
 * So every frame-boundary state of an idle entry is `w_type == tbl[12]` with
 * `0 <= w_int < tbl[13]`, or the expired `w_type == 0, w_int == -1` -- with
 * w_lvr/free1/free2 at their table values, the tame/shot fields zero, and
 * `w_ptr` at `&tbl[16]` throughout, because nothing in the cycle calls
 * `check_next()`. Measured over all 16 Twelve segments in the three corpora:
 * every populated entry 48/49 is in exactly one of those two states, and the
 * archived `w_ptr` is 0x0619BE64 / 0x0619BE96 on every one of them -- constant,
 * and 32 bytes past the entry's table base (consecutive gaps 0x3A/0x32/0x32
 * match sizeof(unk_cmd_184/185/186) = 58/50/50 bytes exactly).
 *
 * THAT MAKES IT SEEDABLE, which corrects this file's earlier reading. The
 * blocker recorded against H5 was that `WAZA_WORK::w_ptr` is a CPS3 address
 * with no map to a host pointer, and that the residual `w_type == 1` is
 * `check_1`, which dereferences it. True of an arbitrary pointer -- but the
 * pointer here is never arbitrary: in the idle cycle it is `&tbl[16]`, and our
 * own copy of that table gives it to us. So the twelve scalar fields come from
 * the archive and `w_ptr` is RECONSTRUCTED, never imported.
 *
 * The guard is the whole safety argument, so it is deliberately narrow: an
 * entry is seeded only when its archived state is one the idle cycle can
 * actually produce from `tbl`. Anything else -- a mid-command state left by
 * `check_next()`, a held charge (tame.flag set, or free1 walked down below
 * `tbl[14]`) -- means `w_ptr` is somewhere we cannot name, so that entry is
 * left alone and the seed audit reports it DIRTY, which is still rc 4. An
 * all-zero archive entry cannot pass the guard either: `expired` needs
 * `w_int == -1`, and the post-init arm needs `w_type == tbl[12]`, which is
 * never 0 (0 is `check_init` itself, and a table dispatching back into
 * `check_init` would not terminate).
 *
 * GATED on ArcadeBalance_IsEnabled() as a PREDICATE, not as a behaviour
 * switch: it is the same condition `cmd_init()` keys the partial clear off, so
 * outside it `SDL_zeroa(waza_work[cmd_id])` wipes all 56 entries at match start
 * and there is no carried state to seed in the first place. It also selects the
 * command table, mirroring `get_commands()`'s arcade arm. */
enum { WAZA_WORK_ARCHIVE_STRIDE = 28 }; /* 12 x s16 + one 32-bit CPS3 w_ptr */

static void read_waza_work_entry(SDL_IOStream* io, int i, int j, WAZA_WORK* dst) {
    SDL_SeekIO(io, WAZA_WORK_OFFSET + (Sint64)((i * 56) + j) * WAZA_WORK_ARCHIVE_STRIDE, SDL_IO_SEEK_SET);

    SDL_zerop(dst);
    SDL_ReadS16BE(io, &dst->w_type);
    SDL_ReadS16BE(io, &dst->w_int);
    SDL_ReadS16BE(io, &dst->free1);
    SDL_ReadS16BE(io, &dst->w_lvr);

    u32 w_ptr;
    SDL_ReadU32BE(io, &w_ptr); /* CPS3 address. Reconstructed from our own table, never imported. */
    (void)w_ptr;

    SDL_ReadS16BE(io, &dst->free2);
    SDL_ReadS16BE(io, &dst->w_dead);
    SDL_ReadS16BE(io, &dst->w_dead2);
    SDL_ReadS16BE(io, &dst->uni0.tame.flag);
    SDL_ReadS16BE(io, &dst->uni0.tame.shot_flag);
    SDL_ReadS16BE(io, &dst->uni0.tame.shot_flag2);
    SDL_ReadS16BE(io, &dst->free3);
    SDL_ReadS16BE(io, &dst->shot_ok);
}

/* Is `b` a state the idle cycle above can produce from command table `tbl`?
 * Only then is `w_ptr == &tbl[16]` a fact rather than a hope. */
static bool waza_carried_is_idle(const s16* tbl, const WAZA_WORK* b) {
    const bool post_init = (b->w_type == tbl[12]) && (b->w_int >= 0) && (b->w_int < tbl[13]);
    const bool expired = (b->w_type == 0) && (b->w_int == -1);

    return (post_init || expired) && (b->w_lvr == tbl[15]) && (b->free1 == tbl[14]) && (b->free2 == tbl[14]) &&
           (b->w_dead == tbl[1]) && (b->w_dead2 == tbl[2]) && (b->uni0.tame.flag == 0) &&
           (b->uni0.tame.shot_flag == 0) && (b->uni0.tame.shot_flag2 == 0) && (b->free3 == 0) && (b->shot_ok == 0);
}

static void sync_waza_work_carried(SDL_IOStream* io) {
    if (!ArcadeBalance_IsEnabled()) {
        return;
    }

    for (int i = 0; i < 2; i++) {
        if (My_char[i] >= 20) {
            continue;
        }

        /* Same table `cmd_move()` indexes: get_commands() -> ArcadeCommandData_Get,
         * and plcnt_init() (plcnt.c) sets `wk->player_number = My_char[wk->wu.id]`. */
        const s16* const* adrs = (const s16* const*)ArcadeCommandData_Get(My_char[i]);
        const int live_end = (int)pl_cmd_num[My_char[i]][6];

        for (int j = WAZA_WORK_CARRIED_FIRST; j < live_end; j++) {
            WAZA_WORK b;
            read_waza_work_entry(io, i, j, &b);

            const s16* tbl = adrs[j];

            if (!waza_carried_is_idle(tbl, &b)) {
                continue;
            }

            WAZA_WORK* a = &waza_work[i][j];
            a->w_type = b.w_type;
            a->w_int = b.w_int;
            a->free1 = b.free1;
            a->w_lvr = b.w_lvr;
            a->free2 = b.free2;
            a->w_dead = b.w_dead;
            a->w_dead2 = b.w_dead2;
            a->uni0.tame.flag = b.uni0.tame.flag;
            a->uni0.tame.shot_flag = b.uni0.tame.shot_flag;
            a->uni0.tame.shot_flag2 = b.uni0.tame.shot_flag2;
            a->free3 = b.free3;
            a->shot_ok = b.shot_ok;

            /* RECONSTRUCTED, not imported: `check_init()` leaves w_ptr exactly
             * here, and the guard above just established the entry is in a
             * state only `check_init()` (plus in-place w_int decrements) can
             * have produced. */
            a->w_ptr = (s16*)(uintptr_t)&tbl[16];
        }
    }
}

void Statcheck_SyncValues(SDL_IOStream* io) {
    Random_ix16 = read_s16(io, RANDOM_IX_16_OFFSET);
    Random_ix32 = read_s16(io, RANDOM_IX_32_OFFSET);

    /* players_timer is a free-running u16 (`players_timer++; players_timer &=
     * 0x7FFF` in plcnt.c / plcnt2.c / plcnt3.c) that the archive inherits from
     * a whole session, while a statcheck run starts a synthetic match with it
     * at 0. It is a live input to a spawn gate -- effg6.c -> effect_G6_move:
     *     `if (ewk->wu.now_koc & (players_timer + ewk->wu.blink_timing))`
     * -- whose mask is 0..7, so an unimported value puts every effect_G9 spawn
     * (and the two random_16() draws in effect_G9_move's case 0) on the wrong
     * frame. Seeding it once is enough: both engines then increment it on the
     * same frames.
     *
     * Offset from the arcade SH-2 program of sfiii3nr1: the gate at
     * CPS3 0x061085A0 loads &players_timer = 0x020157CE, and the three
     * increment sites (0x0611678C, 0x06118882, 0x06119266 -- one per
     * Player_control / Player_control_bonus / Player_control_bonus2) each do
     * `+1` then `& 0x7FFF` on that same address. */
    players_timer = read_u16(io, PLAYERS_TIMER_OFFSET);

    /* t_pl_lvr (H3, docs/research-arcade-balance-desyncs.md). The lever/button
     * command counters in `t_pl_lvr` (`cmd_data.c`) are plain globals that
     * accumulate across matches and that nothing clears at match start --
     * `System_all_clear_Level_B()` (`sys_sub.c`) does `Bg_Close()` +
     * `effect_work_init()` and does not touch them, and neither `Game2_0()`
     * nor `Game2_2()` (`game.c`) mentions them. So an archive can enter a
     * match with a counter already run up by a button held through the
     * preceding screen, while a statcheck run starts a synthetic match with
     * every counter at 0.
     *
     * Unlike `wcp`, these do NOT self-correct from the injected input:
     * `read_input_buff` feeds our engine the archive's own button word each
     * frame, so both sides then increment in lockstep -- and a constant offset
     * incremented in lockstep stays constant forever. Measured on
     * `7733 game_6`: the archive enters with p0 `s1_cnt = 16` (a punch+kick
     * hold, `sw_lvbt = 0x0170`, running +1/frame) and statcheck reported
     * `s1_cnt (6) != 22` at archive frame 7 -- exactly the 16-count head start,
     * six frames later. The 5-frame warm-up in Statcheck_CompareValues cannot
     * wash that out while the button stays held.
     *
     * Seeding once from the pre-game frame is enough, for the same reason the
     * `players_timer` seed above is: after it, both sides advance identically.
     * This is the same import the upstream compare had sketched and left
     * commented out (see the file header). */
    read_t_pl_lvr(io, t_pl_lvr);

    /* Round_Level (E1a, docs/research-arcade-balance-desyncs.md). Per-cabinet
     * session state a replay cannot re-derive: it is the index into
     * `Pow_Control_Data_1[0] = {90,95,98,100,103,106,109,112}` in
     * `cal_damage_vitality()` / `cal_damage_vitality_eff()` (pow_pow.c),
     * so an unimported value scales every hit by the wrong percentage.
     *
     * Nothing in a segment can reconstruct it: the only writers are
     * `Before_Select_Sub()` (game.c) at session start and the
     * `Play_Type == 0` branches of `Update_VS_Data()` / `Loser_Sub()`
     * (manage.c), all of which ran BEFORE the segment the archive starts at.
     * A statcheck run starts a synthetic match, so it inherits
     * `Before_Select_Sub()`'s 3 while the archive can be at any level the
     * preceding session left. Measured over the 16-segment corpus: 3, 2 and
     * 1 all occur, and 5 segments step down mid-segment (always at match
     * end, in `Game_Manage_8_0()` -> `Quick_Entry()` -> `Loser_Sub()`, never
     * during combat), so the ONE seed here is enough -- the value is
     * constant for every frame in which damage is dealt.
     *
     * Offset from the arcade SH-2 program of sfiii3nr1, see
     * ROUND_LEVEL_OFFSET in arcade_constants.h. */
    Round_Level = read_s16(io, ROUND_LEVEL_OFFSET);

    /* Country, and through it CC_Value and Limit_Time (the seeding-gap section
     * of docs/research-arcade-balance-desyncs.md).
     *
     * Country is a CABINET REGION byte. The harness cannot re-derive it from a
     * replay and cannot inherit it from a previous match either: our port
     * hardcodes `Country = 4` in `njUserInit` (main.c), which is the PS2
     * build's constant, while the ground truth is a Japanese board and reads
     * 1 on every one of the 143 corpus segments. That is a real difference in
     * an initial condition, not previous-match residue, so this seed is the
     * only thing that can close it.
     *
     * What the difference gates, and why it is latent rather than fatal today:
     *   - `effb8_normal_or_senyou()` (effb8.c) is
     *     `if (Country != 1) return 0; return random_16() & 1;` -- an RNG DRAW
     *     the arcade takes and, before this seed, we did not. Since 2026-09-05
     *     the oracle asserts Random_ix16 honestly and the corpus is 143 PASS,
     *     so that function is provably never called inside a compared frame on
     *     this corpus. The seed makes our engine draw where CPS3 draws, so a
     *     future corpus that does reach it will agree instead of desyncing.
     *   - `efff9.c`'s `Country != 1 && Country != 8` clamp on `old_rno[5]`,
     *     which draws nothing and is not compared.
     *   - `old_my_char_check()` (effect.c) and `game.c`'s `Country == 3` are
     *     both outside the battle path (grade screen, Rep_Game_Infor).
     *   - CC_Value[0] is read only in `com/` and CC_Value[1] only on
     *     `setup_vitality`'s CPU arm, so both are inert under H4b.
     *
     * ONE ADDRESS RESOLVES THREE. The arcade derives CC_Value and Limit_Time
     * from Country exactly as we do (see COUNTRY_OFFSET / LIMIT_TIME_OFFSET in
     * arcade_constants.h for the two arcade routines), so re-running the same
     * two derivations `Init_Task_1st` runs is enough -- there is no need to
     * import the derived values. The seed audit then asserts CC_Value[0..1]
     * and Limit_Time against the archive STRICTLY, which turns this into a
     * self-test: if our `Setup_Limit_Time()` folded the arcade's max-over-the-
     * difficulty-table into the wrong constant, the audit says so at the seed
     * frame rather than letting it surface a thousand frames later.
     *
     * NOT GATED on ArcadeBalance_IsEnabled(): this whole file is
     * `#if defined(STATCHECK)` and Statcheck_SyncValues has exactly one
     * caller, StatcheckRunner_Prologue. The shipped engine never runs it, so
     * no shipped behaviour changes and there is nothing to gate. Whether the
     * SHIPPED build should also stop hardcoding Country = 4 is a separate,
     * region-not-balance question and is deliberately left alone. */
    Country = read_u8(io, COUNTRY_OFFSET);
    Setup_Difficult_V();
    Setup_Limit_Time();

    sync_waza_work_carried(io);
}

#endif
