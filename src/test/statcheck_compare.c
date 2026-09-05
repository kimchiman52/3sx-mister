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
#include "arcade/arcade_constants.h"
#include "constants.h"
#include "main.h"
#include "sf33rd/Source/Game/engine/cmb_win.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/pls02.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/stage/bg.h"
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

static void compare_waza_work(SDL_IOStream* io) {
    WAZA_WORK waza_work_cps3[2][56];
    read_waza_work(io, waza_work_cps3);

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 56; j++) {
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
}

#endif
