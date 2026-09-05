/* === Scene jump — the shared title -> live-training-match chain ===
 *
 * The SJ-06 chain (docs/savestates-and-instant-mode-jump.md §4) as ONE
 * direct call, verbatim from the spike that proved it (SJ-15;
 * src/test/scene_jump_spike.c at a8250882) so the proof transfers. Each
 * block cites the stock-path symbol it replicates; the doc's SJ-15..SJ-21
 * findings record why the shape is what it is.
 *
 * See scene_jump.h for the contract (title-idle precondition, netplay
 * prohibition, caller split).
 */

#include "scene_jump.h"

#include "constants.h"
#include "main.h"
#include "port/config/training_config.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/grade.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/slowf.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/game.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/io/pulpul.h"
#include "sf33rd/Source/Game/menu/menu.h"
#include "sf33rd/Source/Game/opening/op_sub.h"
#include "sf33rd/Source/Game/opening/opening.h"
#include "sf33rd/Source/Game/rendering/color3rd.h"
#include "sf33rd/Source/Game/rendering/mmtmcnt.h"
#include "sf33rd/Source/Game/rendering/texgroup.h"
#include "sf33rd/Source/Game/screen/sel_pl.h"
#include "sf33rd/Source/Game/sound/se.h"
#include "sf33rd/Source/Game/sound/sound3rd.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/sysdir.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "structs.h"

/* The whole SJ-06 chain as one direct call. Runs in the prologue slot of a
 * frame whose Game_Task has not ticked yet; every step below therefore
 * executes before any task function sees the mutated state. */
s16 SceneJump_ExecuteTrainingChain(const SceneJumpTrainingParams* params) {
    s16 stage;
    s16 ix;

    No_Trans = 1; /* SJ-09 black cover; the caller lifts it once live */

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
     * produced ppgCheckTextureNumber handle=0 failures at round start
     * (SJ-21). */
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

    /* The CONSUMER of the initTrainingData flag set two lines above, and the
     * only thing that ever loads the persisted training settings: character
     * select's own `Default_Training_Data(0)` (sel_pl.c -> Switch_Work case 1,
     * next to `Training_Auto_Start = 1`), whose menu.c body early-outs unless
     * the flag is set, zero-fills Training[0]/[2], then overlays the on-disk
     * config via TrainingConfig_Load().
     *
     * Skipping it did not merely lose the settings, it DESTROYED them: the
     * training-menu dismissal this chain drives reaches menu.c ->
     * Setup_NTr_Data(), whose first statement is TrainingConfig_Save(), which
     * flushes Training[2] -- zeroed, because nothing had loaded it -- straight
     * over the user's file. Measured on a seeded scratch home: `02 05 01 02`
     * came back `00 00 00 00` after one Quick Training run, and the match ran
     * on those zeros. Present_Mode must already be 4 (set above): the body
     * reads save_w->Damage_Level/Difficulty into save_w[Present_Mode]. */
    Default_Training_Data(0);

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
     * (adx_NowOnMemoryType / omSelObjNowOnMemoryType early-outs). SJ-16. */
    checkAdxFileLoaded();
    checkSelObjFileLoaded();

    /* Character-select outcome, hard-written — the same writes the
     * test-runner's select puppeting converges to (test_runner.c ->
     * maybe_force_training_scene_character_and_super_state /
     * maybe_force_training_scene_super_confirm) plus the operator lock
     * Game01's default case applies (`Sel_Arts_Complete[ix] = -1`). */
    for (ix = 0; ix < 2; ix++) {
        My_char[ix] = params->chars[ix];
        Last_My_char2[ix] = params->chars[ix];
        Arts_Y[ix] = params->arts[ix];
        Super_Arts[ix] = params->arts[ix];
        Last_Super_Arts[ix] = params->arts[ix];
        Sel_Arts_Complete[ix] = -1;
        Used_char[ix] = params->chars[ix];
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

    if (params->pin_rng) {
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
     * Load_Replay_Sub; test_runner.c -> force_stage_transition_override).
     * A negative stage derives the stock select-exit choice: sel_pl.c ->
     * Exit_2nd does `Battle_Country = Setup_Battle_Country()`, which for
     * training returns the challenger's character id (SJ-19 records what
     * select otherwise contributes). My_char/New_Challenger/Mode_Type are
     * already written above, which is all Setup_Battle_Country reads. */
    stage = (params->stage >= 0) ? params->stage : (s16)Setup_Battle_Country();
    Battle_Country = stage;
    bg_w.stage = stage;
    bg_w.area = 0;
    Push_LDREQ_Queue_Player(0, My_char[0]);
    Push_LDREQ_Queue_Player(1, My_char[1]);
    Push_LDREQ_Queue_BG((u16)stage);

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

    return stage;
}

bool SceneJump_TrainingLoadsDrained(s16 stage) {
    /* The one genuinely multi-frame dependency: the LDREQ queue drains one
     * pump step per frame on the stock path (game.c -> Game_Task tail ->
     * Check_LDREQ_Queue). SJ-17 measured it at 24-25 frames for two
     * players + one stage on host. */
    return Check_LDREQ_Clear() && Check_PL_Load() && Check_LDREQ_Queue_BG((u16)stage);
}

/* Steps 10-12's scene entry: flip to Game02/Game2_0, which performs the
 * doc's step-11 block itself (System_all_clear_Level_B, C_No[]=0,
 * clear_hit_queue, bg_work_clear, win_lose_work_clear, player_face_init,
 * G_Timer=10) and asserts the drained queue (step 10). */
void SceneJump_EnterBattleScene(void) {
    /* The character-select exit's LAST act, and the one the chain used to
     * omit: sel_pl.c -> Exit_6th calls init_omop() once the player and stage
     * loads have drained and immediately before the scene flips to the battle.
     * This is that slot exactly -- the caller only reaches here through
     * SceneJump_TrainingLoadsDrained().
     *
     * It has to be HERE, ahead of the flip, and not later. init_omop()
     * (sysdir.c) is what builds omop_spmv_ng_table[]/table2[] out of
     * system_dir/extra_option, and plcnt.c -> set_base_data() -- reached from
     * setup_base_and_other_data() during Game02's round boot -- seeds
     * plw[].spmv_ng_flag/flag2 by copying that table. Without this call the
     * table was still all-zero at that copy, so both players entered the match
     * with every engine DIP restriction cleared. Measured on the jump path
     * before the fix: plw[0].spmv_ng_flag 0x00000000 against 0x0B2CE8E0 on the
     * stock path, and spmv_ng_flag2 0x000D0000 against 0x03FF002E -- i.e. no
     * air-guard / auto-guard / auto-parry / anti-air-parry / absolute-guard /
     * chip-damage / wall-jump / air-jump / air-recovery / air-knockdown
     * restrictions, and special-to-special cancel, all-normals-cancellable,
     * SA-to-SA cancel and chain combos all switched ON. The dummy's visible
     * symptom was random parrying: pls00.c -> process_damage() opens with
     * `if (wk->wu.routine_no[3] == 0)` and nests inside it an
     * `if (!(wk->spmv_ng_flag & DIP_SEMI_AUTO_PARRY_DISABLED))` that rewrites
     * routine_no[2] 4->31 / 5->32 / 6->33 / 7->34, turning each of the guard
     * states hitcheck.c -> defense_ground_cps3() picks into a parry.
     *
     * menu.c -> Normal_Training's own init_omop() runs on this path too, but a
     * scene later: set_base_data() has already copied the zero table by then,
     * and effe3.c -> effect_E3_move()'s tail
     * (`omop_spmv_ng_table[id] = mwk->spmv_ng_flag`) writes those stale player
     * flags back over the freshly-correct table -- measured omop0 going
     * 0x0B2CE8E0 -> 0x00000000. With the table correct before the copy, that
     * write-back carries the correct value plus the training settings, which
     * is what it does on the stock path; nothing else is needed to defuse it.
     *
     * Reads My_char[], Mode_Type, Demo_Flag and Present_Mode, all written by
     * SceneJump_ExecuteTrainingChain() before the drain wait began. */
    init_omop();

    G_No[1] = 2;
    G_No[2] = 0;
    G_No[3] = 0;
}

bool SceneJump_TrainingMenuDismissTick(u32 frame_parity) {
    /* A training match STARTS ON its menu: Game_Manage_1st readies
     * TASK_MENU at r_no[0]=7 (manage.c) and Game_Manage_2_1 case 1 holds
     * the round until the menu reaches Wait_Pause_in_Tr (r_no[0] == 10).
     * Dismissing it is in-scene UI, not scene traversal — drive it exactly
     * the way the test runner's PHASE_GAME_TRANSITION does (test_runner.c).
     * SJ-21. */
    if (task[TASK_MENU].r_no[0] == 7 && task[TASK_MENU].r_no[1] == 1 && task[TASK_MENU].r_no[2] == 1) {
        Menu_Cursor_Y[0] = 0;
        p1sw_buff |= frame_parity ? SWK_SOUTH : 0;
    } else if (Allow_a_battle_f == 0) {
        p1sw_buff |= frame_parity ? SWK_ATTACKS : 0;
    }

    /* training_mode_gameplay_started()'s predicate (test_runner.c). */
    return Mode_Type == MODE_NORMAL_TRAINING && Allow_a_battle_f != 0 && Game_pause == 0 && Pause_Down == 0;
}
