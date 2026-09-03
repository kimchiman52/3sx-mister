#if defined(DEBUG)

/* === SPIKE — instant scene jump prototype ===
 *
 * Answers docs/savestates-and-instant-mode-jump.md §9 open question #1
 * ("does the SJ-06 chain run correctly outside its normal task dispatch
 * context?") empirically: performs the entire title -> live training match
 * transition by direct calls in ONE prologue, behind `No_Trans`, with the
 * multi-frame pieces (LDREQ drain, round intro) left to the ordinary frame
 * loop, and reports PASS/FAIL plus frame accounting.
 *
 * What "direct" means here: none of Menu_Task's states run, character
 * select never runs, no button is puppeted after the title screen. The
 * chain steps are invoked as plain calls in the test-runner prologue slot
 * (before njUserMain), i.e. outside any task[] dispatch.
 *
 * The chain itself was PROMOTED to shipped code for the OSD Quick Training
 * feature — src/scene_jump.c — and this spike now calls that shared
 * implementation, so a `--test-instant-jump` PASS keeps proving the exact
 * code the feature runs. What stays spike-local is the harness driver:
 * title mash, frame accounting, liveness verification, PASS/FAIL exit.
 *
 * NOT SHIPPED: #if DEBUG only, armed only by --test-instant-jump.
 */

#include "test/scene_jump_spike.h"

#include "configuration.h"
#include "constants.h"
#include "main.h"
#include "port/sdl/sdl_app.h"
#include "scene_jump.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "structs.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#define SJS_EXIT_CODE_FAILED 7

typedef enum SjsState {
    SJS_WAIT_TITLE, /* mash START until the post-coin title idles */
    SJS_DRAIN,      /* jump done; waiting for the LDREQ queue to drain */
    SJS_WAIT_LIVE,  /* battle scene entered; waiting for Allow_a_battle_f */
    SJS_VERIFY,     /* live; confirm both players reach control state */
    SJS_HOLD        /* PASS printed; idle until the exit event lands */
} SjsState;

static SjsState sjs_state = SJS_WAIT_TITLE;
static Uint32 sjs_frame = 0;       /* prologue ticks since launch */
static Uint32 sjs_jump_frame = 0;  /* frame the chain call ran */
static Uint32 sjs_enter_frame = 0; /* frame G_No flipped to the battle scene */
static Uint32 sjs_live_frame = 0;  /* frame Allow_a_battle_f went live */
static Uint32 sjs_state_started = 0;

static SceneJumpTrainingParams sjs_params;
static s16 sjs_stage;

bool SceneJumpSpike_Active(void) {
    return configuration.test.instant_jump;
}

static void sjs_fail(const char* what) {
    SDL_Log("SCENE-JUMP FAIL: %s (state=%d frame=%u G_No=%d/%d/%d/%d C_No=%d/%d Mode_Type=%d "
            "Allow_a_battle_f=%d Game_pause=%d routine=%d/%d ldreq_clear=%d pl_load=%d)",
            what,
            (int)sjs_state,
            sjs_frame,
            G_No[0],
            G_No[1],
            G_No[2],
            G_No[3],
            C_No[0],
            C_No[1],
            (int)Mode_Type,
            Allow_a_battle_f,
            Game_pause,
            plw[0].wu.routine_no[0],
            plw[1].wu.routine_no[0],
            (int)Check_LDREQ_Clear(),
            (int)Check_PL_Load());
    exit(SJS_EXIT_CODE_FAILED);
}

void SceneJumpSpike_Tick(void) {
    sjs_frame += 1;

    switch (sjs_state) {
    case SJS_WAIT_TITLE:
        /* Cold boot -> attract loop. One START press coins in
         * (Loop_Demo -> Ck_Coin -> Next_Title_Sub establishes
         * Demo_Flag = 1, Present_Mode = 1, TASK_ENTRY, G_No[0] = 2);
         * further presses fast-forward the title dash. Fire the jump
         * once the title idles at Game0_1 (G_No {2,0,1}), the state the
         * real feature fires from. */
        if (G_No[0] == 2 && G_No[1] == 0 && G_No[2] == 1) {
            sjs_params.chars[0] =
                (s8)(configuration.test.characters[0] >= 0 ? configuration.test.characters[0] : CHAR_YUN);
            sjs_params.chars[1] =
                (s8)(configuration.test.characters[1] >= 0 ? configuration.test.characters[1] : CHAR_RYU);
            sjs_params.arts[0] = (s8)(configuration.test.super_arts[0] >= 0 ? configuration.test.super_arts[0] : 0);
            sjs_params.arts[1] = (s8)(configuration.test.super_arts[1] >= 0 ? configuration.test.super_arts[1] : 0);
            sjs_params.stage = (s8)(configuration.test.stage >= 0 ? configuration.test.stage : 2);
            sjs_params.pin_rng = configuration.test.pin_rng;

            /* The whole SJ-06 chain as one direct call — the SHARED,
             * shipped implementation (src/scene_jump.c). */
            sjs_stage = SceneJump_ExecuteTrainingChain(&sjs_params);
            sjs_jump_frame = sjs_frame;
            sjs_state = SJS_DRAIN;
            sjs_state_started = sjs_frame;
            SDL_Log("SCENE-JUMP: chain call fired at frame %u (chars=%d/%d arts=%d/%d stage=%d)",
                    sjs_frame,
                    sjs_params.chars[0],
                    sjs_params.chars[1],
                    sjs_params.arts[0],
                    sjs_params.arts[1],
                    sjs_stage);
            break;
        }

        if (sjs_frame > 1200) {
            sjs_fail("title screen never reached");
        }

        p1sw_buff |= (sjs_frame & 1) ? SWK_START : 0;
        break;

    case SJS_DRAIN:
        /* The one genuinely multi-frame dependency: the LDREQ queue
         * drains one pump step per frame on the stock path
         * (game.c -> Game_Task tail calls Check_LDREQ_Queue once per
         * tick), or entirely within the push frame when
         * --ldreq-barrier-force is also passed (gd3rd.c ->
         * Check_LDREQ_Queue barrier loop). */
        if (SceneJump_TrainingLoadsDrained(sjs_stage)) {
            SceneJump_EnterBattleScene();
            sjs_enter_frame = sjs_frame;
            sjs_state = SJS_WAIT_LIVE;
            sjs_state_started = sjs_frame;
            SDL_Log("SCENE-JUMP: loads drained, battle scene entered at frame %u (+%u after jump)",
                    sjs_frame,
                    sjs_frame - sjs_jump_frame);
            break;
        }

        if (sjs_frame - sjs_state_started > 600) {
            sjs_fail("LDREQ queue never drained");
        }

        break;

    case SJS_WAIT_LIVE:
        /* Training-menu dismissal + liveness predicate, shared with the
         * feature (SJ-21; src/scene_jump.c). */
        if (SceneJump_TrainingMenuDismissTick(sjs_frame & 1)) {
            No_Trans = 0; /* lift the black cover */
            sjs_live_frame = sjs_frame;
            sjs_state = SJS_VERIFY;
            sjs_state_started = sjs_frame;
            SDL_Log("SCENE-JUMP: match live at frame %u (+%u after jump)", sjs_frame, sjs_frame - sjs_jump_frame);
            break;
        }

        if (sjs_frame - sjs_state_started > 900) {
            sjs_fail("battle never went live (Allow_a_battle_f)");
        }

        break;

    case SJS_VERIFY:
        /* Both players must reach the in-control routine state the
         * test-runner's gameplay_input_active() checks
         * (plw[].wu.routine_no[0] == 4), and stay healthy for a window. */
        if (plw[0].wu.routine_no[0] == 4 && plw[1].wu.routine_no[0] == 4 && sjs_frame - sjs_state_started >= 180) {
            SDL_Log("SCENE-JUMP PASS: jump@%u entered@%u live@%u verified@%u — drain=%u frames, "
                    "jump->live=%u frames (Mode_Type=%d Play_Type=%d operators=%d/%d chars=%d/%d stage=%d)",
                    sjs_jump_frame,
                    sjs_enter_frame,
                    sjs_live_frame,
                    sjs_frame,
                    sjs_enter_frame - sjs_jump_frame,
                    sjs_live_frame - sjs_jump_frame,
                    (int)Mode_Type,
                    Play_Type,
                    plw[0].wu.wu_operator,
                    plw[1].wu.wu_operator,
                    My_char[0],
                    My_char[1],
                    bg_w.stage);
            sjs_state = SJS_HOLD;
            SDLApp_Exit();
            break;
        }

        if (sjs_frame - sjs_state_started > 900) {
            sjs_fail("players never reached control state (routine_no)");
        }

        break;

    case SJS_HOLD:
        break;
    }
}

#else /* !DEBUG — inert stubs (args.c rejects --test-instant-jump here) */

#include "test/scene_jump_spike.h"

bool SceneJumpSpike_Active(void) {
    return false;
}

void SceneJumpSpike_Tick(void) {}

#endif
