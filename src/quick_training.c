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
 * orchestration, netplay nav, or a --play-replay session is active, and the
 * tick aborts defensively if a session ever activates mid-sequence (netplay's
 * own session entry re-normalizes Exec_Wipe/Stop_SG/Gap_Timer,
 * netplay.c -> the `Exec_Wipe = 0` block).
 *
 * THE SHUFFLE VIEWER IS NOT IN THAT LIST. --watch-replays is the one session
 * this feature TERMINATES rather than defers to: the request stops the
 * playlist (ReplayShuffle_Stop) and qt_begin() tears the loaded replay down,
 * because "Quick Training" and "Watch Replays" are two rows of the same OSD
 * and picking the second one has to mean leaving the first. Everything else
 * in the list owns state this sequence must not yank out from under it.
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
#include "sf33rd/Source/Game/engine/cmb_win.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/sound/sound3rd.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/sysdir.h"
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
 * Bounded so a wedged transition cannot hold the request forever -- but the
 * bound EXPIRES INTO A START, not into a drop. The wrapper has already closed
 * the OSD by the time the game sees the signal, so discarding the request
 * leaves the user with a menu that shut and a game that did nothing: the exact
 * outcome the deferral exists to avoid, just four seconds later and with the
 * evidence only in the log.
 *
 * What IS measured about starting over the transition: it is recoverable.
 * Switch_Screen_Init rewrites every field it stomps (Forbid_Break, Gap_Timer,
 * Stop_SG, Escape_SS, WipeLimit via WipeInit), and the sequence normalizes
 * Forbid_Break/Stop_SG at QT_WIPE_IN -- as does qt_fail() on any watchdog.
 * The deadline is also measured UNREACHABLE on every path exercised so far:
 * 15 runs, N in {20..240}, PASS-frame minus N exactly 302 every time.
 *
 * NOT measured, and recorded here as UNPROVEN rather than as the reason: that
 * "a transition still holding Exec_Wipe after QT_DEFER_MAX_FRAMES is itself
 * broken". The engine's own wipes are tens of frames, but the training-pause
 * exit in menu.c case 2 sits under `if (Check_Pad_in_Pause(task_ptr) == 0)`
 * and was not chased to a conclusion, so a legitimate long hold has not been
 * ruled out. The deadline is a choice between two bad outcomes on a path
 * nothing has yet reached, not a claim about the engine. */
#define QT_DEFER_MAX_FRAMES 240

#define QT_TIMEOUT_GOTO_TITLE 1200
#define QT_TIMEOUT_DRAIN 600
#define QT_TIMEOUT_WAIT_LIVE 900

#if defined(DEBUG)
static void qt_test_tick(void);
static void qt_test_fail(const char* what);
static bool qt_test_verify_engine_state(void);
static bool qt_test_select_reset_tick(void);
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
    /* The --play-replay BOOT path, and only that one.
     *
     * `ReplayPlayer_IsActive()` alone would be wrong, because during a
     * --watch-replays session a replay is essentially always loaded — the
     * plain check refused every press in the very mode this escape exists
     * for. `!ReplayShuffle_IsEnabled()` narrows it to the boot path: the CLI
     * flag is process-lifetime, so it separates the two modes for the whole
     * session and keeps doing so after the viewer has been stopped.
     *
     * Refused rather than terminated, for a reason that is about the mode
     * and not about difficulty. --play-replay is a one-file viewer whose
     * terminal state is SDLApp_Exit() (replay_player.c -> tick_terminal):
     * the process is the session, and there is nothing behind it to return
     * to. It is also not reachable from the device OSD at all — the wrapper's
     * replay_play_handoff() has no caller, and the only replay row wired to a
     * menu bit is "Watch Replays" -> replay_shuffle_handoff()
     * (thirdsarm_wrapper.cpp). So this refusal can only ever be hit by
     * someone who typed the flag. */
    if (!ReplayShuffle_IsEnabled() && ReplayPlayer_IsActive()) {
        return "--play-replay session (the process ends with the replay; nothing behind it)";
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

    /* Tear down the shuffle viewer's loaded replay, if there is one. The
     * PLAYLIST was already stopped when the request was accepted (see
     * ReplayShuffle_Stop in QuickTraining_Tick), so nothing can start
     * another; what is left here is the .3sr and, crucially, the frame
     * freeze.
     *
     * WHY THE FREEZE MATTERS. ReplayPlayer_Destroy() clears s_stall_frame, so
     * from this frame on main.c stops holding the engine and njUserMain()
     * runs again. That hold is deliberate: it keeps a terminal replay's LIVE
     * post-match flow from reaching Game_Manage_10th and tripping
     * push_effect_work's bound check (effect.c, "qix is out of range").
     * Releasing it costs a bounded free-run — the 8 frames of WipeOut
     * (sc_sub.c, WipeLimit 0..7) plus the single frame QT_GOTO_TITLE takes to
     * raise Game_pause = 0x81 / Request_LDREQ_Break() / effect_work_init().
     * The player freezes at C_No[0] > 6 (replay_player.c -> PHASE_POSTMATCH),
     * and the shortest measured path from C_No[0] == 7 to Game_Manage_10th's
     * Switch_Screen_Init(0) is ~198 frames of fixed countdowns under NEUTRAL
     * pads — which this sequence holds, because every phase but QT_WIPE_IN
     * zeroes p1sw_buff/p2sw_buff. Nine frames against ~198.
     *
     * WHY THIS IS NOT THE ABORT PATH'S BUG. replay_player.c's hold-START
     * abort calls Soft_Reset_Sub() — whose first statement is
     * FadeOut(1, 0xFF, 8) — while leaving the freeze to RE-ASSERT on the next
     * tick, so exactly one njUserMain() runs, the 8-step fade advances by one
     * and never finishes, and TASK_INIT never walks. Here the freeze is gone
     * for good (ReplayPlayer_Tick early-returns on !loaded with s_stall_frame
     * already false), so every frame of the fade QT_GOTO_TITLE's own
     * Soft_Reset_Sub() starts actually runs. */
    if (ReplayPlayer_IsActive()) {
        SDL_Log("quick-training: tearing down the loaded replay (status=%d) — the shuffle viewer is stopped",
                (int)ReplayPlayer_GetStatus());
        ReplayPlayer_Destroy();
    }

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

    /* Stop the shuffle viewer the moment the press is ACCEPTED, not when the
     * sequence finally starts. The deferral below can hold the request for up
     * to QT_DEFER_MAX_FRAMES, and ReplayShuffle_Tick() — which runs AFTER this
     * one (main.c -> game_step_0) — would happily reach the end of the current
     * replay and start the next one inside that window, so the user would see
     * the viewer visibly move on from a press that was meant to end it.
     *
     * Same gate as the consumption below (idle + no refusal), so every request
     * that reaches qt_begin() has passed through here first. Idempotent, and
     * it stops only the PLAYLIST: the loaded replay may be freezing the engine
     * frame, and that freeze is torn down in qt_begin() instead, one call
     * before the engine goes back under a cover. */
    if (qt_request && qt_phase == QT_IDLE && qt_refusal() == NULL) {
        ReplayShuffle_Stop("quick training requested");
    }

    /* Hold, do not start, while an engine transition owns WipeLimit
     * (see QT_DEFER_MAX_FRAMES). Checked before the request is consumed so the
     * press survives the wait. */
    if (qt_request && qt_phase == QT_IDLE && Exec_Wipe != 0) {
        qt_defer_frames += 1;

        if (qt_defer_frames <= QT_DEFER_MAX_FRAMES) {
            return;
        }

        /* Deadline reached. Fall through and start on top of the stuck
         * transition rather than dropping the press -- see the comment on
         * QT_DEFER_MAX_FRAMES for why a drop is the worse of the two. */
        SDL_Log("quick-training: engine wipe still active after %u frames - starting over it", qt_defer_frames);
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
static bool qtt_engine_verified;
static bool qtt_done;

/* The on-disk training contents as they were BEFORE the first request fired.
 * Snapshotted rather than re-read at the end, because the failure this exists
 * to catch destroys the thing a late read would compare against: a run that
 * never loads the config still SAVES it (menu.c -> Setup_NTr_Data opens with
 * TrainingConfig_Save), so the file ends up zeroed, the live globals are
 * zeroed, and a compare-at-the-end agrees with itself. Measured 2026-09-05 --
 * that exact check passed against a deliberately broken build. */
static s8 qtt_disk_contents[2][2][7];
static bool qtt_have_disk;

/* Same snapshot discipline for the last-used characters/arts, and for the same
 * measured reason: TrainingConfig_Save() writes my_char/super_arts straight
 * out of the LIVE My_char[]/Super_Arts[], so a run that used the wrong
 * characters persists them and a late read agrees with the wrong answer.
 * Measured 2026-09-05: deleting TrainingConfig_GetLastUsed() from qt_begin()
 * left a compare-at-the-end check green with chars=3/2 against a file that
 * had said 11/0 when the run started. */
static s8 qtt_disk_chars[2];
static s8 qtt_disk_arts[2];
static bool qtt_have_disk_chars;

/* Second verification stage: the training-mode SELECT reset (menu.c ->
 * Tr_Reset_Check / Tr_Reset_Apply, docs/training-select-reset.md). Nothing
 * else in the tree exercises it -- `grep -rl "Tr_Reset\|select-reset" tools
 * src/test` found nothing before this -- so its FIRST ATTACK preservation, the
 * one thing in Tr_Reset_Apply that is a deliberate deviation from
 * combo_cont_init()'s round-start semantics, was unverified.
 *
 * It rides on the Quick Training harness rather than getting a flag of its own
 * because this harness has already done the expensive part: it is sitting in a
 * live, unpaused, Play_Mode == PLAY_MODE_NORMAL training round with
 * plw[0].wu.operator set, which is exactly Tr_Reset_Check's precondition set.
 * A flag would also have to be remembered by whatever runs the gate; a stage
 * cannot be forgotten. */
typedef enum QttSelReset {
    QTT_SR_ARM = 0,  /* plant the FIRST ATTACK sentinel; no press yet */
    QTT_SR_PRESS,    /* one frame of SELECT -- Tr_Reset_Read_Input wants an EDGE */
    QTT_SR_AWAIT,    /* watch for the teardown; inject nothing (a second edge re-fires) */
    QTT_SR_RECOVER,  /* teardown seen and asserted; wait for the round to hand control back */
    QTT_SR_DONE,
} QttSelReset;

static QttSelReset qtt_sr_phase;
static Uint32 qtt_sr_started;
static s8 qtt_sr_first_attack;

/* Any non-zero value combo_cont_init() would clear; 3 is the both-players
 * value cmb_win.c itself writes. */
#define QTT_SR_FIRST_ATTACK_SENTINEL 3

#define QTT_SR_TIMEOUT 240

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

/* Bits that MUST be set on both players' spmv_ng_flag once a training match
 * is live, and are all zero when the engine DIP tables were never built.
 *
 * Chosen so effe3.c -> effect_E3_move() cannot reach them, which is what lets
 * the DUMMY be held to the same mask as the player. The set it can reach was
 * ENUMERATED, not assumed to be a range -- the earlier "every write is inside
 * bits 4..11" was wrong, because DIP_AIR_GUARD_DISABLED is 1 << 5 and would
 * have been inside it. Every bit-level write effect_E3_move() makes to
 * spmv_ng_flag (`&0xFFFFFFEF`, `|0xC0`, `|0x80`, `&0xFFFFFFBF`, `|0x40`,
 * `&0xFFFFFF7F`, `&0xFFFFF0FF`, and the named DIP_GUARD_DISABLED /
 * DIP_AUTO_GUARD_DISABLED / DIP_AUTO_PARRY_DISABLED / `~(DIP_UNKNOWN_8 |
 * DIP_UNKNOWN_9 | DIP_AIR_PARRY_DISABLED | DIP_ANTI_AIR_PARRY_DISABLED)`
 * writes) lands in bits {4, 6, 7, 8, 9, 10, 11}. Bit 5 is never written.
 * Its one whole-word write, `mwk->spmv_ng_flag = ewk->master_ng_flag`, only
 * reinstates the snapshot effect_E3_init() took from the same field.
 * Corroborated live: plw[1] came back 0B2CE070 in one run and 0B2CE0F0 in
 * another -- bit 7 moving, the required bits not.
 *
 * That is the whole point -- the defect these catch showed up only on the
 * dummy, as apparently random parrying, because pls00.c -> process_damage()
 * nests, under its opening `if (wk->wu.routine_no[3] == 0)`, an
 * `if (!(wk->spmv_ng_flag & DIP_SEMI_AUTO_PARRY_DISABLED))` that rewrites
 * routine_no[2] 4->31 / 5->32 / 6->33 / 7->34, turning each ground/air guard
 * state into a parry.
 *
 * Measured (Debug, --test-quick-training=60, seeded scratch home):
 *   jump path BEFORE the fix  plw0 00000000/000D0000  plw1 000000C0/000D0200
 *   jump path AFTER  the fix  plw0 0B2CE8E0/03FF002E  plw1 0B2CE070/03FF002E
 *   stock character select    plw0 0B2CE8E0/03FF002E  plw1 0B2CE070/03FF002E
 * i.e. the jump path now lands byte-identical to the shipping path. */
#define QT_DIP1_REQUIRED                                                                                               \
    ((u32)(DIP_AIR_GUARD_DISABLED | DIP_ABSOLUTE_GUARD_DISABLED | DIP_SEMI_AUTO_PARRY_DISABLED |                       \
           DIP_AIR_KNOCKDOWNS_DISABLED))

/* Same idea on flag2. These five are the cancels the pre-fix jump path left
 * switched ON -- special-to-special, all-normals-cancellable, SA-to-SA, and
 * both chain-combo bits, i.e. bits 1, 2, 3, 20 and 21. Enumerated the same
 * way, and for the same reason: "bits 16..19, the S.A.GAUGE group" was also
 * wrong. effect_E3_move()'s flag2 writes (`&0xFFFBFFFF`, `|0x90000`,
 * `&0xFFF7FFFF`, `|0x50000`, `&0xFFFEFFFF`, `|0xC0000 |
 * DIP2_SA_GAUGE_NO_DEPLETE`, `|0xD0000`, and the QUICK STAND pair
 * `|DIP2_QUICK_STAND_DISABLED` / `&~DIP2_QUICK_STAND_DISABLED`) touch bits
 * {9, 16, 18, 19, 26}, plus the same whole-word master_ng_flag2 restore.
 * None of 1/2/3/20/21, so these are dummy-safe too. */
#define QT_DIP2_REQUIRED                                                                                               \
    ((u32)(DIP2_SPECIAL_TO_SPECIAL_CANCEL_DISABLED | DIP2_ALL_NORMALS_CANCELLABLE_DISABLED |                           \
           DIP2_SA_TO_SA_CANCEL_DISABLED | DIP2_GROUND_CHAIN_COMBO_DISABLED | DIP2_AIR_CHAIN_COMBO_DISABLED))

/* Everything a liveness check cannot see. Returns false (after reporting)
 * when the match is live but running the wrong engine or the wrong settings.
 *
 * Three independent claims, because each is falsifiable on its own:
 *   1. the engine DIP tables were built BEFORE the round boot copied them
 *      into plw[] (scene_jump.c -> SceneJump_EnterBattleScene -> init_omop);
 *   2. the persisted training settings were LOADED and are still what is on
 *      disk (scene_jump.c's Default_Training_Data(0)); and
 *   3. the characters and super arts came off that same file rather than from
 *      qt_begin()'s hardcoded fallbacks (TrainingConfig_GetLastUsed).
 *
 * THE LIMIT OF CLAIM 1, stated precisely because the mask reads like a parity
 * check and is not one. It pins a CONFIGURATION. Every bit in QT_DIP1/2_
 * REQUIRED is conditional inside sysdir.c -> get_system_direction_parameter()
 * on a `system_dir[N].contents[p][i] == 0` test (air guard `[7][0]`, air
 * knockdowns `[7][2]`, absolute guard `[1][1]`; DIP2 ground/air chain `[8][0]`
 * / `[8][1]`, all-normals `[8][2]`, special-to-special `[8][5]`, SA-to-SA
 * `[9][1]`), or -- for DIP_SEMI_AUTO_PARRY_DISABLED -- on
 * `omop_guard_type[extra_option.contents[0][3]]`, whose index-2 entry does not
 * contain that bit at all. Both `system_dir[1]` and `save_w[]` are PERSISTED
 * (savesub.c -> serialize_sysdir / serialize_settings), so a home whose SYSTEM
 * DIRECTION or EXTRA OPTION settings differ from Dir_Default_Data fails this
 * spuriously. Two things it therefore does NOT catch: init_omop() running with
 * the wrong inputs (it branches on Mode_Type, Demo_Flag, Present_Mode and
 * Direction_Working[] to pick which system_dir/save_w slot to read, and a
 * wrong-slot read that happens to agree with the defaults still passes); and
 * omop_spmv_ng_table[] itself, which is never asserted here -- the harness
 * reads plw[], so the claim in docs §10.6 that the early init_omop() "defuses
 * effect_E3_move()'s write-back" is reasoning, not something under test. */
static bool qt_test_verify_engine_state(void) {
    s8 want_chars[2];
    s8 want_arts[2];
    int i;

    for (i = 0; i < 2; i++) {
        const u32 dip1 = (u32)plw[i].spmv_ng_flag;
        const u32 dip2 = (u32)plw[i].spmv_ng_flag2;

        if ((dip1 & QT_DIP1_REQUIRED) != QT_DIP1_REQUIRED) {
            SDL_Log("QUICK-TRAINING TEST: plw[%d].spmv_ng_flag = %08X, missing %08X - init_omop() did not run "
                    "before set_base_data() copied the DIP table",
                    i,
                    (unsigned)dip1,
                    (unsigned)(QT_DIP1_REQUIRED & ~dip1));
            qt_test_fail("engine DIP table (spmv_ng_flag) not seeded");
            return false;
        }

        if ((dip2 & QT_DIP2_REQUIRED) != QT_DIP2_REQUIRED) {
            SDL_Log("QUICK-TRAINING TEST: plw[%d].spmv_ng_flag2 = %08X, missing %08X - cancels/chain combos are "
                    "enabled; this is not the shipping engine configuration",
                    i,
                    (unsigned)dip2,
                    (unsigned)(QT_DIP2_REQUIRED & ~dip2));
            qt_test_fail("engine DIP table (spmv_ng_flag2) not seeded");
            return false;
        }
    }

    if (qtt_have_disk) {
        s8 now[2][2][7];

        /* (a) THE FILE IS INTACT. The data-loss half: nothing in the sequence
         * may write over the user's settings. */
        if (!TrainingConfig_ReadDiskContents(now)) {
            qt_test_fail("the persisted training config is gone or unreadable after the run");
            return false;
        }

        if (SDL_memcmp(now, qtt_disk_contents, sizeof(now)) != 0) {
            SDL_Log("QUICK-TRAINING TEST: on-disk training contents changed - was "
                    "action=%d guard=%d quick-stand=%d stun=%d, now %d/%d/%d/%d",
                    qtt_disk_contents[0][0][0],
                    qtt_disk_contents[0][0][1],
                    qtt_disk_contents[0][0][2],
                    qtt_disk_contents[0][0][3],
                    now[0][0][0],
                    now[0][0][1],
                    now[0][0][2],
                    now[0][0][3]);
            qt_test_fail("the run rewrote the persisted training config");
            return false;
        }

        /* (b) THE SETTINGS ARE LIVE. The other half: they must actually have
         * been loaded into the match, not merely left undamaged on disk. */
        if (SDL_memcmp(Training[0].contents, qtt_disk_contents, sizeof(now)) != 0) {
            SDL_Log("QUICK-TRAINING TEST: live Training[0] contents are "
                    "action=%d guard=%d quick-stand=%d stun=%d, the stored config says %d/%d/%d/%d",
                    Training[0].contents[0][0][0],
                    Training[0].contents[0][0][1],
                    Training[0].contents[0][0][2],
                    Training[0].contents[0][0][3],
                    qtt_disk_contents[0][0][0],
                    qtt_disk_contents[0][0][1],
                    qtt_disk_contents[0][0][2],
                    qtt_disk_contents[0][0][3]);
            qt_test_fail("training settings were not loaded from the persisted config");
            return false;
        }
    }

    if (qtt_have_disk_chars) {
        /* Pre-filled with qt_begin()'s own fallbacks, exactly as the snapshot
         * at frame 0 was, so the two are comparable. */
        want_chars[0] = CHAR_YUN;
        want_chars[1] = CHAR_RYU;
        want_arts[0] = 0;
        want_arts[1] = 0;
        (void)TrainingConfig_GetLastUsed(want_chars, want_arts);

        for (i = 0; i < 2; i++) {
            /* (a) the file's characters/arts are still what they were. */
            if (want_chars[i] != qtt_disk_chars[i] || want_arts[i] != qtt_disk_arts[i]) {
                SDL_Log("QUICK-TRAINING TEST: stored character/art for player %d changed - was char=%d art=%d, "
                        "now char=%d art=%d",
                        i,
                        qtt_disk_chars[i],
                        qtt_disk_arts[i],
                        want_chars[i],
                        want_arts[i]);
                qt_test_fail("the run rewrote the persisted character/art selection");
                return false;
            }

            /* (b) the match is actually using them. */
            if (My_char[i] != qtt_disk_chars[i] || Super_Arts[i] != qtt_disk_arts[i]) {
                SDL_Log("QUICK-TRAINING TEST: player %d is char=%d art=%d, the config said char=%d art=%d when "
                        "the run started",
                        i,
                        My_char[i],
                        Super_Arts[i],
                        qtt_disk_chars[i],
                        qtt_disk_arts[i]);
                qt_test_fail("match did not use the last-used characters/arts");
                return false;
            }
        }
    }

    return true;
}

/* Drives the SELECT-reset stage one frame at a time. Returns true only when
 * the stage has completed successfully; every other return is "not yet" (or a
 * qt_test_fail() that has already exited).
 *
 * What it proves, and why each half is needed:
 *   - the reset FIRED. The marker is the Suicide[0] PULSE, not a routine_no
 *     change. `plw[].wu.routine_no[0] != 4 on either player` was the first
 *     marker and is unsound: it is the exact negation of test_runner.c's
 *     gameplay_input_active(), i.e. "somebody is not in control", which a
 *     hit, a knockdown or a CPU-controlled dummy produces just as well --
 *     and this harness runs against whatever training config is on disk, so
 *     a stored ACTION=CPU would let the first exchange stand in for the
 *     teardown. Tr_Reset_Apply writes `Suicide[0] = 1` and
 *     Tr_Reset_Finish_Teardown clears it on the very next frame, so in a
 *     live round it is a one-frame pulse with exactly one producer. ARM
 *     asserts it is 0 before the press so the pulse cannot be a leftover.
 *     Without a positive marker "first_attack was preserved" is satisfied by
 *     a reset that never happened at all.
 *   - FIRST ATTACK SURVIVED. Tr_Reset_Apply saves and restores it around
 *     combo_cont_init(), which zeroes it (cmb_win.c). That restore is the
 *     deviation from round-start semantics -- a SELECT reset repositions the
 *     players inside a running round, so re-arming the banner would fire it
 *     again on the next hit, every reset.
 *   - the round RECOVERED. The reset re-enters the appear sequence at
 *     pcon_rno[1] = 2 and hands input back shortly after -- measured 2 frames
 *     from the Suicide[0] pulse; a teardown that never comes back out is a
 *     wedge, not a reset. */
static bool qt_test_select_reset_tick(void) {
    switch (qtt_sr_phase) {
    case QTT_SR_ARM:
        /* The marker must be clean before the press, or the pulse this stage
         * waits for could be somebody else's. */
        if (Suicide[0] != 0) {
            SDL_Log("QUICK-TRAINING TEST: Suicide[0] = %d before the SELECT press - the reset marker is "
                    "already set, so the teardown watch below would be meaningless",
                    Suicide[0]);
            qt_test_fail("SELECT-reset stage armed on a dirty Suicide[0]");
            return false;
        }

        qtt_sr_first_attack = first_attack;
        first_attack = QTT_SR_FIRST_ATTACK_SENTINEL;
        qtt_sr_phase = QTT_SR_PRESS;
        qtt_sr_started = qtt_frame;
        return false;

    case QTT_SR_PRESS:
        /* Exactly one frame. Tr_Reset_Read_Input takes `~PLsw[i][1] & PLsw[i][0]`
         * on SWK_BACK, and main.c latches PLsw from p*sw_buff AFTER this
         * prologue, so one write here is one edge. Holding it would also be
         * harmless (an edge is an edge) but a second edge later would fire a
         * second reset, which is why QTT_SR_AWAIT injects nothing. */
        p1sw_buff |= SWK_BACK;
        qtt_sr_phase = QTT_SR_AWAIT;
        qtt_sr_started = qtt_frame;
        return false;

    case QTT_SR_AWAIT:
        if (Suicide[0] != 0) {
            if (first_attack != QTT_SR_FIRST_ATTACK_SENTINEL) {
                SDL_Log("QUICK-TRAINING TEST: first_attack = %d after the SELECT reset, expected %d - "
                        "Tr_Reset_Apply's combo_cont_init() cleared it and the save/restore did not put it back",
                        first_attack,
                        QTT_SR_FIRST_ATTACK_SENTINEL);
                qt_test_fail("SELECT reset did not preserve FIRST ATTACK");
                return false;
            }

            qtt_sr_phase = QTT_SR_RECOVER;
            qtt_sr_started = qtt_frame;
            return false;
        }

        if (qtt_frame - qtt_sr_started > QTT_SR_TIMEOUT) {
            SDL_Log("QUICK-TRAINING TEST: no Suicide[0] pulse %u frames after SELECT (Play_Mode=%d "
                    "Allow_a_battle_f=%d Game_pause=%d Extra_Break=%d operator=%d/%d routine_no=%d/%d)",
                    QTT_SR_TIMEOUT,
                    Play_Mode,
                    Allow_a_battle_f,
                    Game_pause,
                    Extra_Break,
                    plw[0].wu.operator,
                    plw[1].wu.operator,
                    plw[0].wu.routine_no[0],
                    plw[1].wu.routine_no[0]);
            qt_test_fail("SELECT reset never fired");
            return false;
        }

        return false;

    case QTT_SR_RECOVER:
        if (plw[0].wu.routine_no[0] == 4 && plw[1].wu.routine_no[0] == 4) {
            if (first_attack != QTT_SR_FIRST_ATTACK_SENTINEL) {
                SDL_Log("QUICK-TRAINING TEST: first_attack = %d once the round recovered, expected %d",
                        first_attack,
                        QTT_SR_FIRST_ATTACK_SENTINEL);
                qt_test_fail("SELECT reset did not preserve FIRST ATTACK");
                return false;
            }

            SDL_Log("QUICK-TRAINING TEST: SELECT reset fired and recovered in %u frames, FIRST ATTACK preserved (%d)",
                    qtt_frame - qtt_sr_started,
                    first_attack);
            first_attack = qtt_sr_first_attack;
            qtt_sr_phase = QTT_SR_DONE;
            return true;
        }

        if (qtt_frame - qtt_sr_started > QTT_SR_TIMEOUT) {
            SDL_Log("QUICK-TRAINING TEST: players stuck at routine_no %d/%d %u frames after the SELECT reset",
                    plw[0].wu.routine_no[0],
                    plw[1].wu.routine_no[0],
                    QTT_SR_TIMEOUT);
            qt_test_fail("SELECT reset never handed control back");
            return false;
        }

        return false;

    case QTT_SR_DONE:
    default:
        return true;
    }
}

static void qt_test_tick(void) {
    if (!QuickTraining_TestActive() || qtt_done) {
        return;
    }

    if (qtt_frame == 0) {
        /* Before anything can touch it. */
        qtt_have_disk = TrainingConfig_ReadDiskContents(qtt_disk_contents);

        qtt_disk_chars[0] = CHAR_YUN;
        qtt_disk_chars[1] = CHAR_RYU;
        qtt_disk_arts[0] = 0;
        qtt_disk_arts[1] = 0;
        qtt_have_disk_chars = TrainingConfig_GetLastUsed(qtt_disk_chars, qtt_disk_arts);

        /* Both snapshots can come up empty, and they fail independently:
         * ReadDiskContents needs a parseable file, GetLastUsed needs the same
         * file but validates my_char/super_arts separately. Say so for each,
         * and say it again on the PASS line -- a PASS that quietly ran three
         * of seven assertions is the shape this whole stage exists to stop. */
        if (!qtt_have_disk) {
            SDL_Log("QUICK-TRAINING TEST: no readable training contents in this home - the "
                    "settings-loaded and settings-preserved assertions are SKIPPED for this run");
        }

        if (!qtt_have_disk_chars) {
            SDL_Log("QUICK-TRAINING TEST: no readable last-used characters/arts in this home - the "
                    "chars-loaded and chars-preserved assertions are SKIPPED for this run");
        }
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
        if (!qtt_engine_verified) {
            if (plw[0].wu.routine_no[0] == 4 && plw[1].wu.routine_no[0] == 4 && qtt_frame - qtt_verify_start >= 180) {
                if (!qt_test_verify_engine_state()) {
                    return;
                }

                qtt_engine_verified = true;
            } else if (qtt_frame - qtt_verify_start > 900) {
                qt_test_fail("players never reached control state (routine_no)");
            }

            return;
        }

        /* Second stage, in the live round the first one built. Driven OUTSIDE
         * the liveness gate above on purpose: Tr_Reset_Apply zeroes
         * routine_no[0..7], so a stage nested inside "both players are at
         * routine_no 4" would stop being ticked across the teardown -- which
         * is exactly the window in which its Suicide[0] marker appears. */
        if (!qt_test_select_reset_tick()) {
            return;
        }

        {
            /* The PASS line names what it did NOT check. A green line that
             * silently excludes four of seven assertions is the same defect
             * class as not running them. */
            const char* skipped = "none";

            if (!qtt_have_disk && !qtt_have_disk_chars) {
                skipped = "settings-loaded, settings-preserved, chars-loaded, chars-preserved";
            } else if (!qtt_have_disk) {
                skipped = "settings-loaded, settings-preserved";
            } else if (!qtt_have_disk_chars) {
                skipped = "chars-loaded, chars-preserved";
            }

            SDL_Log("QUICK-TRAINING TEST PASS: %u sequence(s) verified at frame %u (Mode_Type=%d chars=%d/%d "
                    "arts=%d/%d stage=%d dip=%08X/%08X %08X/%08X) skipped-assertions=[%s]",
                    qt_completions,
                    qtt_frame,
                    (int)Mode_Type,
                    My_char[0],
                    My_char[1],
                    Super_Arts[0],
                    Super_Arts[1],
                    bg_w.stage,
                    (unsigned)plw[0].spmv_ng_flag,
                    (unsigned)plw[0].spmv_ng_flag2,
                    (unsigned)plw[1].spmv_ng_flag,
                    (unsigned)plw[1].spmv_ng_flag2,
                    skipped);
            qtt_done = true;
            SDLApp_Exit();
            return;
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
