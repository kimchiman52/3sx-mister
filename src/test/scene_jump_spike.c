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
 * Sources for each block are cited inline against the symbols they
 * replicate; the doc's SJ-15..SJ-19 findings record what was learned.
 *
 * NOT SHIPPED: #if DEBUG only, armed only by --test-instant-jump.
 */

#include "test/scene_jump_spike.h"

#include "configuration.h"
#include "constants.h"
#include "main.h"
#include "port/config/training_config.h"
#include "port/sdl/sdl_app.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/engine/grade.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/slowf.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/game.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/io/pulpul.h"
#include "sf33rd/Source/Game/opening/op_sub.h"
#include "sf33rd/Source/Game/opening/opening.h"
#include "sf33rd/Source/Game/rendering/color3rd.h"
#include "sf33rd/Source/Game/rendering/mmtmcnt.h"
#include "sf33rd/Source/Game/rendering/texgroup.h"
#include "sf33rd/Source/Game/sound/se.h"
#include "sf33rd/Source/Game/sound/sound3rd.h"
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

static s8 sjs_chars[2];
static s8 sjs_arts[2];
static s8 sjs_stage;

bool SceneJumpSpike_Active(void) {
    return configuration.test.instant_jump;
}

static void sjs_fail(const char* what) {
    SDL_Log("SCENE-JUMP FAIL: %s (state=%d frame=%u G_No=%d/%d/%d/%d C_No=%d/%d Mode_Type=%d "
            "Allow_a_battle_f=%d Game_pause=%d routine=%d/%d ldreq_clear=%d pl_load=%d)",
            what, (int)sjs_state, sjs_frame, G_No[0], G_No[1], G_No[2], G_No[3], C_No[0], C_No[1], (int)Mode_Type,
            Allow_a_battle_f, Game_pause, plw[0].wu.routine_no[0], plw[1].wu.routine_no[0], (int)Check_LDREQ_Clear(),
            (int)Check_PL_Load());
    exit(SJS_EXIT_CODE_FAILED);
}

/* The whole SJ-06 chain as one direct call. Runs in the prologue slot of a
 * frame whose Game_Task has not ticked yet; every step below therefore
 * executes before any task function sees the mutated state. */
static void sjs_jump(void) {
    s16 ix;

    No_Trans = 1; /* SJ-09 black cover; cleared when the match is live */

    /* Steps 1-2 — title teardown. Game0_2 cases 3-4:
     * `TexRelease(601); title_tex_flag = 0;` and
     * `Purge_mmtm_area(2); Make_texcash_of_list(2);` (game.c -> Game0_2). */
    TexRelease(601);
    title_tex_flag = 0;
    Purge_mmtm_area(2);
    Make_texcash_of_list(2);

    /* Step 4's one non-cosmetic load: the 0x7F30 pattern block Menu_Init
     * pulls in (menu.c -> Menu_Init, `load_any_texture_patnum(0x7F30,
     * 0xC, 0)`). The training-menu overlay draws from it; skipping it
     * produced ppgCheckTextureNumber handle=0 failures at round start. */
    load_any_texture_patnum(0x7F30, 0xC, 0);

    /* Step 3 — menu BGM request, Game0_2 case 5 (`BGM_Request(65)`).
     * Behind black it is barely heard; round init's Check_Stage_BGM
     * (manage.c -> Game_Manage_1st) replaces it with the stage BGM. */
    BGM_Request(65);

    /* Steps 4-6 are SKIPPED ON PURPOSE: Menu_Init / Mode_Select case 0's
     * screen furniture (Setup_Virtual_BG, effect_57/04/61 headers and
     * cursor rows) exists to draw the mode-select menu, and step 9's
     * effect_work_init() below zeroes the whole effect pool anyway
     * (effect.c -> effect_work_init, SDL_zeroa(frw)). The non-cosmetic
     * remainder of Mode_Select case 0 is replicated here:
     * Clear_Personal_Data / Vital_Handicap / VS_Stage (menu.c ->
     * Mode_Select). */
    Clear_Personal_Data(0);
    Clear_Personal_Data(1);

    for (ix = 0; ix < 4; ix++) {
        Vital_Handicap[ix][0] = 7;
        Vital_Handicap[ix][1] = 7;
    }

    VS_Stage = 0x14;

    /* Mode set — the TRAINING confirm branch of Mode_Select case 3
     * (menu.c -> Mode_Select, `case 2:` under IO_Result 0x100). */
    mpp_w.initTrainingData = true;
    Mode_Type = MODE_NORMAL_TRAINING;
    Present_Mode = 4;
    Decide_ID = 0;
    Champion = 0;
    Pause_ID = 0;
    Training_ID = 0;
    New_Challenger = 1;
    TrainingConfig_RestoreCharSelect();

    /* Step 8 — Setup_VS_Mode (menu.c) minus its `task_ptr->r_no[0] = 5`:
     * that write parks Menu_Task in Suspend_Menu, which only matters while
     * TASK_MENU keeps being dispatched; here it is exited below instead. */
    plw[0].wu.wu_operator = 1;
    plw[1].wu.wu_operator = 1;
    Operator_Status[0] = 1;
    Operator_Status[1] = 1;
    grade_check_work_1st_init(0, 0);
    grade_check_work_1st_init(0, 1);
    grade_check_work_1st_init(1, 0);
    grade_check_work_1st_init(1, 1);
    Setup_Training_Difficulty();

    /* Task bookkeeping the menu path performs across its states:
     * TASK_SAVER exited on the training confirm, TASK_ENTRY exited right
     * after it, TASK_MENU exited at Game12_2. cpExitTask is SDL_zero
     * (main.c -> cpExitTask), safe on tasks that were never readied. */
    cpExitTask(TASK_SAVER);
    cpExitTask(TASK_ENTRY);
    cpExitTask(TASK_MENU);

    /* Step 7 — the two "async gates" are in fact SYNCHRONOUS on this port
     * (sound3rd.c -> checkAdxFileLoaded busy-loops load_it_use_any_key;
     * texgroup.c -> checkSelObjFileLoaded busy-loops load_it_use_this_key;
     * both bottom out in gd3rd.c -> fsFileReadSync) and idempotent
     * (adx_NowOnMemoryType / omSelObjNowOnMemoryType early-outs). */
    checkAdxFileLoaded();
    checkSelObjFileLoaded();

    /* Character-select outcome, hard-written — the same writes the
     * test-runner's select puppeting converges to (test_runner.c ->
     * maybe_force_training_scene_character_and_super_state /
     * maybe_force_training_scene_super_confirm) plus the operator lock
     * Game01's default case applies (`Sel_Arts_Complete[ix] = -1`). */
    for (ix = 0; ix < 2; ix++) {
        My_char[ix] = sjs_chars[ix];
        Last_My_char2[ix] = sjs_chars[ix];
        Arts_Y[ix] = sjs_arts[ix];
        Super_Arts[ix] = sjs_arts[ix];
        Last_Super_Arts[ix] = sjs_arts[ix];
        Sel_Arts_Complete[ix] = -1;
        Used_char[ix] = sjs_chars[ix];
        Player_Color[ix] = 0;
    }

    Play_Type = 1;
    Setup_ID();

    /* Game01 case 0 equivalents (game.c -> Game01): scene sub-state
     * clear, BGM level, RNG seed, level-B clear, rumble reset. */
    S_No[0] = 0;
    S_No[1] = 0;
    S_No[2] = 0;
    S_No[3] = 0;
    SsBgmHalfVolume(0);
    Break_Into = 0;
    Stop_Combo = 0;

    if (configuration.test.pin_rng) {
        Setup_Net_Random_ix();
    } else {
        Random_ix32 = Interrupt_Timer;
        Random_ix32_ex = Interrupt_Timer;
    }

    init_slow_flag();
    System_all_clear_Level_B();
    pulpul_stop();
    init_pulpul_work();

    /* Stage choice + asset loads — the shape Load_Replay_Sub case 3 and
     * the test-runner's force_stage_transition_override use (menu.c ->
     * Load_Replay_Sub; test_runner.c -> force_stage_transition_override). */
    Battle_Country = sjs_stage;
    bg_w.stage = sjs_stage;
    bg_w.area = 0;
    Push_LDREQ_Queue_Player(0, My_char[0]);
    Push_LDREQ_Queue_Player(1, My_char[1]);
    Push_LDREQ_Queue_BG((u16)sjs_stage);

    /* Character-select exit equivalents (game.c -> Game01 default case,
     * plus menu.c -> Load_Replay_Sub case 6). */
    Game01_Sub();
    Cover_Timer = 24;
    appear_type = APPEAR_TYPE_NON_ANIMATED; /* game.c -> Set_Appear_Type_For_Mode, training arm */
    set_hitmark_color();
    Purge_texcash_of_list(3);
    Make_texcash_of_list(3);
    Bonus_Game_Flag = 0;
    Demo_Time_Stop = 0;

    /* Step 9 — effect pool reset (game.c -> Game12_2 does this on the
     * stock path). */
    effect_work_init();

    /* Park the Game task in the menu-idle scene (Game12_0 is a no-op)
     * until the LDREQ queue drains; entering Game02 with a non-empty
     * queue is the Game2_0 fatal_error. G_No[0] is already 2 (Game). */
    G_No[1] = 12;
    G_No[2] = 0;
    G_No[3] = 0;
}

/* Steps 10-12's scene entry: flip to Game02/Game2_0, which performs the
 * doc's step-11 block itself (System_all_clear_Level_B, C_No[]=0,
 * clear_hit_queue, bg_work_clear, win_lose_work_clear, player_face_init,
 * G_Timer=10) and asserts the drained queue (step 10). */
static void sjs_enter_battle(void) {
    G_No[1] = 2;
    G_No[2] = 0;
    G_No[3] = 0;
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
         * real feature would fire from. */
        if (G_No[0] == 2 && G_No[1] == 0 && G_No[2] == 1) {
            sjs_chars[0] = (s8)(configuration.test.characters[0] >= 0 ? configuration.test.characters[0] : CHAR_YUN);
            sjs_chars[1] = (s8)(configuration.test.characters[1] >= 0 ? configuration.test.characters[1] : CHAR_RYU);
            sjs_arts[0] = (s8)(configuration.test.super_arts[0] >= 0 ? configuration.test.super_arts[0] : 0);
            sjs_arts[1] = (s8)(configuration.test.super_arts[1] >= 0 ? configuration.test.super_arts[1] : 0);
            sjs_stage = (s8)(configuration.test.stage >= 0 ? configuration.test.stage : 2);

            sjs_jump();
            sjs_jump_frame = sjs_frame;
            sjs_state = SJS_DRAIN;
            sjs_state_started = sjs_frame;
            SDL_Log("SCENE-JUMP: chain call fired at frame %u (chars=%d/%d arts=%d/%d stage=%d)", sjs_frame,
                    sjs_chars[0], sjs_chars[1], sjs_arts[0], sjs_arts[1], sjs_stage);
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
        if (Check_LDREQ_Clear() && Check_PL_Load() && Check_LDREQ_Queue_BG((u16)sjs_stage)) {
            sjs_enter_battle();
            sjs_enter_frame = sjs_frame;
            sjs_state = SJS_WAIT_LIVE;
            sjs_state_started = sjs_frame;
            SDL_Log("SCENE-JUMP: loads drained, battle scene entered at frame %u (+%u after jump)", sjs_frame,
                    sjs_frame - sjs_jump_frame);
            break;
        }

        if (sjs_frame - sjs_state_started > 600) {
            sjs_fail("LDREQ queue never drained");
        }

        break;

    case SJS_WAIT_LIVE:
        /* A training match STARTS ON its menu: Game_Manage_1st readies
         * TASK_MENU at r_no[0]=7 (manage.c) and Game_Manage_2_1 case 1
         * holds the round until the menu reaches Wait_Pause_in_Tr
         * (r_no[0] == 10). Dismissing it is in-scene UI, not scene
         * traversal — drive it exactly the way the test runner's
         * PHASE_GAME_TRANSITION does (test_runner.c). */
        if (task[TASK_MENU].r_no[0] == 7 && task[TASK_MENU].r_no[1] == 1 && task[TASK_MENU].r_no[2] == 1) {
            Menu_Cursor_Y[0] = 0;
            p1sw_buff |= (sjs_frame & 1) ? SWK_SOUTH : 0;
        } else if (Allow_a_battle_f == 0) {
            p1sw_buff |= (sjs_frame & 1) ? SWK_ATTACKS : 0;
        }

        /* training_mode_gameplay_started()'s predicate (test_runner.c). */
        if (Mode_Type == MODE_NORMAL_TRAINING && Allow_a_battle_f != 0 && Game_pause == 0 && Pause_Down == 0) {
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
                    sjs_jump_frame, sjs_enter_frame, sjs_live_frame, sjs_frame, sjs_enter_frame - sjs_jump_frame,
                    sjs_live_frame - sjs_jump_frame, (int)Mode_Type, Play_Type, plw[0].wu.wu_operator,
                    plw[1].wu.wu_operator, My_char[0], My_char[1], bg_w.stage);
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
