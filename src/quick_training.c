/* === Quick Training — OSD -> live training match, no menus ===
 *
 * See quick_training.h for the feature contract and the context policy.
 * The scene work itself lives in src/scene_jump.c (the proven SJ-06
 * chain); this file owns the sequencing: wipe-out, teardown-to-title,
 * chain call, load drain, menu dismissal, wipe-in — plus the request
 * gating and the failure recovery.
 *
 * ROLLBACK / NETPLAY: Exec_Wipe (driven via Switch_Screen*) is in the
 * GS_SAVE set (src/netplay/game_state.c), so everything here is
 * rollback-visible state. That is fine offline and prohibited during a
 * session — qt_refusal() rejects requests while a session, direct-P2P
 * orchestration, netplay nav, or a replay session is active, and the tick
 * aborts defensively if a session ever activates mid-sequence (netplay's
 * own session entry re-normalizes Exec_Wipe/Stop_SG/Gap_Timer,
 * netplay.c -> the `Exec_Wipe = 0` block).
 */

#include "quick_training.h"

#include "configuration.h"
#include "constants.h"
#include "main.h"
#include "netplay/direct_p2p.h"
#include "netplay/netplay.h"
#include "netplay/netplay_nav.h"
#include "port/config/training_config.h"
#include "replay/replay_player.h"
#include "replay/replay_shuffle.h"
#include "scene_jump.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/sound/sound3rd.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "structs.h"

#include <SDL3/SDL.h>

#if defined(DEBUG)
#include "port/sdl/sdl_app.h"
#include <stdlib.h>
#endif

typedef enum QtPhase {
    QT_IDLE = 0,
    QT_WIPE_OUT,   /* diagonal Switch_Screen wipe over the current scene */
    QT_GOTO_TITLE, /* behind No_Trans: teardown if needed, drive to title idle */
    QT_DRAIN,      /* chain fired; LDREQ queue draining */
    QT_WAIT_LIVE,  /* battle scene entered; dismissing the training menu */
    QT_WIPE_IN,    /* cover lifted; wiping the live match in */
} QtPhase;

static QtPhase qt_phase = QT_IDLE;
static bool qt_request;       /* set by QuickTraining_Request, consumed by the tick */
static Uint32 qt_defer_frames; /* frames spent waiting out an engine wipe */
static Uint32 qt_frame;       /* frames since the running sequence started */
static Uint32 qt_phase_start; /* qt_frame at the current phase's entry */
static int qt_teardown;       /* 0 = not started, 1 = LDREQ break requested, 2 = reset done */
static SceneJumpTrainingParams qt_params;
static s16 qt_stage = -1;
static Uint32 qt_completions; /* finished sequences (read by the DEBUG test driver) */

/* Phase timeouts, in frames. GOTO_TITLE covers a full teardown + coin-in +
 * title dash (the spike's title watchdog was 1200); DRAIN and WAIT_LIVE
 * mirror the spike's 600/900. Blowing one is a bug, not an expected path —
 * qt_fail() recovers to the title so the game stays usable. */
/* An engine transition already in flight owns WipeLimit. Switch_Screen_Init
 * would call WipeInit() and reset that counter to 0 under it, restarting the
 * engine's wipe and stomping Forbid_Break/Gap_Timer/Stop_SG/Escape_SS with it
 * -- WipeLimit is shared state, and note WipeOut's `WipeLimit += 1` sits
 * OUTSIDE its `if (!No_Trans)` guard, so a cover suppresses the drawing but
 * never the counter. So hold the request until Exec_Wipe clears rather than
 * starting on top of it. Deferring beats refusing: the row is the first thing
 * in the OSD, and a press that silently did nothing would read as a bug.
 * Bounded so a wedged transition drops the request instead of arming forever. */
#define QT_DEFER_MAX_FRAMES 240

#define QT_TIMEOUT_GOTO_TITLE 1200
#define QT_TIMEOUT_DRAIN 600
#define QT_TIMEOUT_WAIT_LIVE 900

#if defined(DEBUG)
static void qt_test_tick(void);
static void qt_test_fail(const char* what);
#endif

void QuickTraining_Request(void) {
    qt_request = true;
}

/* NULL = allowed; otherwise the reason the request is ignored. */
static const char* qt_refusal(void) {
    if (Netplay_GetSessionState() != NETPLAY_SESSION_IDLE) {
        return "netplay session active";
    }
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) {
        return "direct-P2P orchestration active";
    }
    if (NetplayNav_IsActive()) {
        return "netplay menu navigation active";
    }
    if (ReplayPlayer_IsActive()) {
        return "replay playback active";
    }
    if (ReplayShuffle_IsEnabled()) {
        return "shuffle-viewer session";
    }
#if defined(DEBUG)
    if (configuration.test.enabled && !QuickTraining_TestActive()) {
        return "test harness owns the session";
    }
#endif
    return NULL;
}

/* Everything past the title needs the teardown: mode-select and the other
 * menus, character select, a live match, win/continue/ranking screens —
 * i.e. G_No[0] == 2 (game.c -> Main_Jmp_Tbl slot 2, `Game`) outside the
 * title scene (`Game00` at G_No[2] <= 1: dash or idle). Pre-coin states
 * (boot G_No[0] == 0, attract G_No[0] == 1) coin in via START instead. */
static bool qt_needs_teardown(void) {
    return G_No[0] == 2 && !(G_No[1] == 0 && G_No[2] <= 1);
}

static void qt_begin(void) {
    qt_frame = 0;
    qt_phase_start = 0;
    qt_teardown = 0;
    qt_stage = -1;
    qt_defer_frames = 0;

    /* Last-used characters/arts from the persisted training config
     * (SJ-10: both training entry paths already restore from it; this is
     * the same file). Defaults when it is missing/invalid match the
     * spike's: Yun vs Ryu, first super art. Stage < 0 = derive the stock
     * select-exit choice (scene_jump.c). */
    qt_params.chars[0] = CHAR_YUN;
    qt_params.chars[1] = CHAR_RYU;
    qt_params.arts[0] = 0;
    qt_params.arts[1] = 0;
    qt_params.stage = -1;
    qt_params.pin_rng = false;
    TrainingConfig_GetLastUsed(qt_params.chars, qt_params.arts);

    /* Wipe-out over whatever is on screen, in the game's own transition
     * language: Switch_Screen_Init then Switch_Screen(1) per frame until
     * done (the Disp_Ranking / Game0_2 pattern, game.c). Type 1 is the
     * diagonal wipe. The wipe draws through njDrawPolygon2D into the 2D
     * prim buffer, which game_step_0 flushes AFTER this tick each frame
     * (njdp2d_draw drains and resets it, dc_ghost.c), so prologue-time
     * draws land on this same frame. */
    Switch_Screen_Init(0);
    qt_phase = QT_WIPE_OUT;

    SDL_Log("quick-training: start (chars=%d/%d arts=%d/%d) from G_No=%d/%d/%d/%d",
            qt_params.chars[0],
            qt_params.chars[1],
            qt_params.arts[0],
            qt_params.arts[1],
            G_No[0],
            G_No[1],
            G_No[2],
            G_No[3]);
}

/* Recover from a blown watchdog: back to a clean title via the shipped
 * soft-reset flow (the replay_player.c / netplay-disconnect precedent),
 * cover lifted, wipe bookkeeping cleared. */
static void qt_fail(const char* what) {
    SDL_Log("quick-training: FAIL - %s (phase=%d frame=%u G_No=%d/%d/%d/%d ldreq_clear=%d pl_load=%d)",
            what,
            (int)qt_phase,
            qt_frame,
            G_No[0],
            G_No[1],
            G_No[2],
            G_No[3],
            (int)Check_LDREQ_Clear(),
            (int)Check_PL_Load());
    Soft_Reset_Sub();
    No_Trans = 0;
    Forbid_Break = 0;
    Stop_SG = 0;
    qt_phase = QT_IDLE;
#if defined(DEBUG)
    qt_test_fail(what);
#endif
}

void QuickTraining_Tick(void) {
#if defined(DEBUG)
    qt_test_tick();
#endif

    if (qt_phase == QT_IDLE && !qt_request) {
        return;
    }

    /* Defensive: a netplay session activating mid-sequence. Unreachable by
     * the request gating (orchestration must already be non-idle before a
     * session can start, and that refuses the request), but the wipe/cover
     * state must never leak into a session, so abort rather than assume.
     * Session entry re-normalizes Exec_Wipe/Stop_SG/Gap_Timer itself
     * (netplay.c) and manages No_Trans per frame. */
    if (qt_phase != QT_IDLE && Netplay_GetSessionState() != NETPLAY_SESSION_IDLE) {
        SDL_Log("quick-training: ABORT - netplay session became active mid-sequence (phase=%d)", (int)qt_phase);
        No_Trans = 0;
        qt_phase = QT_IDLE;
        qt_request = false;
        return;
    }

    /* Hold, do not start, while an engine transition owns WipeLimit
     * (see QT_DEFER_MAX_FRAMES). Checked before the request is consumed so the
     * press survives the wait. */
    if (qt_request && qt_phase == QT_IDLE && Exec_Wipe != 0) {
        qt_defer_frames += 1;

        if (qt_defer_frames <= QT_DEFER_MAX_FRAMES) {
            return;
        }

        SDL_Log("quick-training: request dropped - engine wipe still active after %u frames", qt_defer_frames);
        qt_request = false;
        qt_defer_frames = 0;
        return;
    }

    if (qt_request) {
        qt_request = false;
        qt_defer_frames = 0;

        if (qt_phase != QT_IDLE) {
            SDL_Log("quick-training: request ignored - sequence already running (phase=%d)", (int)qt_phase);
        } else {
            const char* why = qt_refusal();

            if (why != NULL) {
                SDL_Log("quick-training: request ignored - %s", why);
            } else {
                qt_begin();
            }
        }
    }

    if (qt_phase == QT_IDLE) {
        return;
    }

    qt_frame += 1;

    /* Own the pads for the covered part of the sequence: real presses are
     * suppressed (this runs before the p*sw_buff -> p*sw_0 latch in
     * game_step_0, the NetplayNav_Tick precedent) and the phases below OR
     * their own injected presses back in. During QT_WIPE_IN the match is
     * already live, so the player gets the pads back. */
    if (qt_phase != QT_WIPE_IN) {
        p1sw_buff = 0;
        p2sw_buff = 0;
    }

    switch (qt_phase) {
    case QT_WIPE_OUT:
        if (Switch_Screen(1) != 0) {
            /* Fully covered (this frame drew the final full-cover bands).
             * Drop the SJ-09 cover for the whole hidden stretch. */
            No_Trans = 1;
            qt_phase = QT_GOTO_TITLE;
            qt_phase_start = qt_frame;
        }
        break;

    case QT_GOTO_TITLE:
        /* Fire the chain the moment the post-coin title idles (G_No
         * {2,0,1}, game.c -> Game0_1) with the load queue quiet — the
         * exact state the spike proved the chain from (SJ-15). */
        if (G_No[0] == 2 && G_No[1] == 0 && G_No[2] == 1 && Check_LDREQ_Clear()) {
            qt_stage = SceneJump_ExecuteTrainingChain(&qt_params);
            qt_phase = QT_DRAIN;
            qt_phase_start = qt_frame;
            SDL_Log("quick-training: chain fired at frame %u (chars=%d/%d arts=%d/%d stage=%d)",
                    qt_frame,
                    qt_params.chars[0],
                    qt_params.chars[1],
                    qt_params.arts[0],
                    qt_params.arts[1],
                    qt_stage);
            break;
        }

        if (qt_needs_teardown()) {
            /* Mirror the shipped soft-reset sequence (reset.c ->
             * Check_SoftReset freezes with Game_pause = 0x81, Reset_Move
             * breaks the load queue and clears the effect pool, Reset_Wait
             * waits for the break to land, then Soft_Reset_Sub). The break
             * is serviced by Game_Task's tail (Check_LDREQ_Queue cancels
             * the in-flight read and reinits the queue, gd3rd.c), which
             * still runs behind No_Trans. */
            if (qt_teardown == 0) {
                Game_pause = 0x81;
                Pause_ID = 0;
                sound_all_off();
                Request_LDREQ_Break();
                effect_work_init();
                qt_teardown = 1;
            } else if (qt_teardown == 1 && Check_LDREQ_Break() == 0) {
                Soft_Reset_Sub();
                qt_teardown = 2;
            }
        } else {
            /* Pre-coin attract/boot, or the post-reset/post-coin title
             * dash: one START press coins in (Loop_Demo -> Ck_Coin ->
             * Next_Title_Sub), further presses fast-forward the dash —
             * exactly the spike's drive. */
            p1sw_buff |= (qt_frame & 1) ? SWK_START : 0;
        }

        if (qt_frame - qt_phase_start > QT_TIMEOUT_GOTO_TITLE) {
            qt_fail("title screen never reached");
        }
        break;

    case QT_DRAIN:
        if (SceneJump_TrainingLoadsDrained(qt_stage)) {
            const Uint32 drained_in = qt_frame - qt_phase_start;
            SceneJump_EnterBattleScene();
            qt_phase = QT_WAIT_LIVE;
            qt_phase_start = qt_frame;
            SDL_Log("quick-training: loads drained in %u frames, battle scene entered", drained_in);
            break;
        }

        if (qt_frame - qt_phase_start > QT_TIMEOUT_DRAIN) {
            qt_fail("LDREQ queue never drained");
        }
        break;

    case QT_WAIT_LIVE:
        if (SceneJump_TrainingMenuDismissTick(qt_frame & 1)) {
            /* Live. Lift the cover and wipe the match in. The wipe and the
             * cover are mutually exclusive (WipeIn's draw early-outs while
             * No_Trans is set, sc_sub.c -> WipeIn), so this is strictly
             * sequential: cover OFF first, then the wipe draws its own
             * full-cover first frame (WipeLimit=0 fully covers — the
             * comment inside WipeIn) on this very frame. */
            No_Trans = 0;
            Switch_Screen_Init(0);
            Switch_Screen_Revival(1);
            qt_phase = QT_WIPE_IN;
            qt_phase_start = qt_frame;
            break;
        }

        if (qt_frame - qt_phase_start > QT_TIMEOUT_WAIT_LIVE) {
            qt_fail("battle never went live (Allow_a_battle_f)");
        }
        break;

    case QT_WIPE_IN:
        if (Switch_Screen_Revival(1) != 0) {
            /* Post-revival bookkeeping, per the stock pattern:
             * Forbid_Break = 0 after the revival completes (game.c ->
             * Disp_Ranking case 3). Stop_SG was raised by
             * Switch_Screen_Init and the round-start clears (manage.c)
             * have already run behind the cover, so clear it here too —
             * the netplay session entry does the same direct clear
             * (netplay.c). */
            Forbid_Break = 0;
            Stop_SG = 0;
            qt_phase = QT_IDLE;
            qt_completions += 1;
            SDL_Log("quick-training: complete - live match in %u frames (Mode_Type=%d chars=%d/%d arts=%d/%d "
                    "stage=%d)",
                    qt_frame,
                    (int)Mode_Type,
                    My_char[0],
                    My_char[1],
                    Super_Arts[0],
                    Super_Arts[1],
                    bg_w.stage);
        }
        break;

    case QT_IDLE:
        break;
    }
}

/* ====================================================================== */
/* DEBUG test driver — --test-quick-training / --test-quick-training-again */
/* ====================================================================== */

#if defined(DEBUG)

bool QuickTraining_TestActive(void) {
    return configuration.test.quick_training_frame >= 0;
}

static Uint32 qtt_frame;        /* prologue frames since launch */
static Uint32 qtt_expected;     /* requests scheduled so far */
static Uint32 qtt_verify_start; /* qtt_frame when verification began */
static bool qtt_verifying;
static bool qtt_done;

static void qt_test_fail(const char* what) {
    if (!QuickTraining_TestActive()) {
        return;
    }
    SDL_Log("QUICK-TRAINING TEST FAIL: %s (frame=%u completions=%u expected=%u)",
            what,
            qtt_frame,
            qt_completions,
            qtt_expected);
    exit(7);
}

static void qt_test_tick(void) {
    if (!QuickTraining_TestActive() || qtt_done) {
        return;
    }

    qtt_frame += 1;

    if ((int)qtt_frame == configuration.test.quick_training_frame) {
        qt_request = true;
        qtt_expected += 1;
    }

    if (configuration.test.quick_training_again_frame >= 0 &&
        (int)qtt_frame == configuration.test.quick_training_again_frame) {
        qt_request = true;
        qtt_expected += 1;
    }

    const int last_scheduled = (configuration.test.quick_training_again_frame >= 0)
                                   ? configuration.test.quick_training_again_frame
                                   : configuration.test.quick_training_frame;

    if (qtt_expected > 0 && (int)qtt_frame > last_scheduled && qt_completions == qtt_expected && qt_phase == QT_IDLE) {
        if (!qtt_verifying) {
            qtt_verifying = true;
            qtt_verify_start = qtt_frame;
        }

        /* The spike's liveness bar: both players in the in-control routine
         * state, held for a window. */
        if (plw[0].wu.routine_no[0] == 4 && plw[1].wu.routine_no[0] == 4 && qtt_frame - qtt_verify_start >= 180) {
            SDL_Log("QUICK-TRAINING TEST PASS: %u sequence(s) verified at frame %u (Mode_Type=%d chars=%d/%d "
                    "stage=%d)",
                    qt_completions,
                    qtt_frame,
                    (int)Mode_Type,
                    My_char[0],
                    My_char[1],
                    bg_w.stage);
            qtt_done = true;
            SDLApp_Exit();
            return;
        }

        if (qtt_frame - qtt_verify_start > 900) {
            qt_test_fail("players never reached control state (routine_no)");
        }
    }

    /* Overall watchdog: a scheduled request that never completes (e.g. it
     * was refused, or a sequence wedged in a way the phase watchdogs
     * recovered from without completing). */
    if (qtt_expected > 0 && qt_completions < qtt_expected && (int)qtt_frame > last_scheduled + 3600) {
        qt_test_fail("scheduled request(s) never completed");
    }
}

#else /* !DEBUG */

bool QuickTraining_TestActive(void) {
    return false;
}

#endif
