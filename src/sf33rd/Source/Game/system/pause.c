/**
 * @file pause.c
 * Game Pause
 */

#include "sf33rd/Source/Game/system/pause.h"
#include "common.h"
#include "main.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/effect/eff66.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/io/pulpul.h"
#include "sf33rd/Source/Game/menu/menu.h"
#include "sf33rd/Source/Game/sound/sound3rd.h"
#include "sf33rd/Source/Game/system/reset.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"
#include "port/sdl/sdl_app.h"
#include "replay/replay_player.h"

#define PAUSE_HOLD_FRAMES 105

u8 PAUSE_X;
static u16 start_hold_counter[2];

void Pause_Task(struct _TASK* task_ptr);

void Pause_Check(struct _TASK* task_ptr);
void Pause_Move(struct _TASK* task_ptr);
void Pause_Sleep(struct _TASK* /* unused */);
void Pause_Die(struct _TASK* /* unused */);

void Flash_Pause(struct _TASK* task_ptr);

void Flash_Pause_Sleep(struct _TASK* /* unused */);
void Flash_Pause_1st(struct _TASK* task_ptr);
void Flash_Pause_2nd(struct _TASK* task_ptr);
void Flash_Pause_3rd(struct _TASK* /* unused */);
void Flash_Pause_4th(struct _TASK* task_ptr);

s32 Check_Pause_Term(u16 sw, u8 PL_id);
void Exit_Pause(struct _TASK* task_ptr);
void Setup_Pause(struct _TASK* task_ptr);
void Setup_Come_Out(struct _TASK* task_ptr);
s32 Check_Play_Status(s16 PL_id);

void Pause_Task(struct _TASK* task_ptr) {
    void (*Main_Jmp_Tbl[4])(struct _TASK*) = { Pause_Check, Pause_Move, Pause_Sleep, Pause_Die };

    if (!nowSoftReset() && Mode_Type != MODE_NETWORK && !Is_Training_Mode(Mode_Type)) {
        Main_Jmp_Tbl[task_ptr->r_no[0]](task_ptr);
        Flash_Pause(task_ptr);
    }
}

void Pause_Check(struct _TASK* task_ptr) {
    PAUSE_X = 0;

    if (SDLApp_IsHoldToPauseEnabled()) {
        for (int i = 0; i < 2; i++) {
            if (PLsw[i][0] & SWK_START) {
                if (start_hold_counter[i] <= PAUSE_HOLD_FRAMES)
                    start_hold_counter[i]++;
            } else {
                start_hold_counter[i] = 0;
            }

            if (start_hold_counter[i] == PAUSE_HOLD_FRAMES) {
                if (Check_Pause_Term(SWK_START, i) != 0)
                    break;
            }
        }
    } else {
        if (Check_Pause_Term(~PLsw[0][1] & PLsw[0][0], 0) == 0) {
            Check_Pause_Term(~PLsw[1][1] & PLsw[1][0], 1);
        }
    }

    switch (PAUSE_X) {
    case 1:
        Setup_Pause(task_ptr);
        break;

    case 2:
        Setup_Come_Out(task_ptr);
        break;
    }
}

void Pause_Move(struct _TASK* task_ptr) {
    if (Exit_Menu) {
        Exit_Pause(task_ptr);
    }
}

void Pause_Sleep(struct _TASK* /* unused */) {};

void Pause_Die(struct _TASK* /* unused */) {};

void Flash_Pause(struct _TASK* task_ptr) {
    void (*Flash_Jmp_Tbl[5])(
        struct _TASK*) = { Flash_Pause_Sleep, Flash_Pause_1st, Flash_Pause_2nd, Flash_Pause_3rd, Flash_Pause_4th };

    if (Pause_Down != 0) {
        Flash_Jmp_Tbl[task_ptr->r_no[2]](task_ptr);
    }
}

void Flash_Pause_Sleep(struct _TASK* /* unused */) {}

void Flash_Pause_1st(struct _TASK* task_ptr) {
    if (--task_ptr->free[0] == 0) {
        task_ptr->r_no[2] = 2;
        task_ptr->free[0] = 60;
    }
}

void Flash_Pause_2nd(struct _TASK* task_ptr) {
    if (--task_ptr->free[0]) {
        if (Pause_ID == 0) {
            SSPutStr2(20, 9, 9, "1P PAUSE");
            return;
        }

        SSPutStr2(20, 9, 9, "2P PAUSE");
        return;
    }

    task_ptr->r_no[2] = 1;
    task_ptr->free[0] = 30;
}

void Flash_Pause_3rd(struct _TASK* /* unused */) {}

void Flash_Pause_4th(struct _TASK* task_ptr) {
    if (Interface_Type[Pause_ID] == 0) {
        dispControllerWasRemovedMessage(0x84, 0x52, 0x10);
        return;
    }

    Pause_Type = 1;
    Setup_Pause(task_ptr);
}

void dispControllerWasRemovedMessage(s32 x, s32 y, s32 step) {
    SSPutStrPro(0, x, y, 9, -1, "Please reconnect");
    SSPutStrPro(0, x, (y + step), 9, -1, "the controller to");

    if (Pause_ID) {
        SSPutStrPro(0, x, (y + (step * 2)), 9, -1, "controller port 2.");
    } else {
        SSPutStrPro(0, x, (y + (step * 2)), 9, -1, "controller port 1.");
    }
}

s32 Check_Pause_Term(u16 sw, u8 PL_id) {
    if (Demo_Flag == 0) {
        return 0;
    }

    if (Allow_a_battle_f == 0 || Extra_Break != 0) {
        return 0;
    }

    if (vm_w.Access != 0 || vm_w.Request != 0) {
        return PAUSE_X = 0;
    }

    if (Exec_Wipe) {
        return 0;
    }

    Pause_ID = PL_id;

    if (Check_Play_Status(PL_id) == 0) {
        return 0;
    }

    /* Step C1 (docs/plan-fcade-replay-browser.md): the runtime .3sr replay
     * player (release code, src/replay/replay_player.c) injects both pads
     * directly into p1sw_buff/p2sw_buff — no physical controller needs to
     * be connected, so the connection check below fires the Come_Out
     * ("controller unplugged") pause mid-replay: Game_pause = 0x81 freezes
     * Game_timer/gameplay while the replay keeps advancing its input
     * index, desyncing the next checksum checkpoint (observed on the first
     * C1 run: a 29-frame freeze starting ~frame 391). Same rationale as
     * the DEBUG/STATCHECK carve-outs around this one, but scoped at
     * runtime to an actively-injecting replay session — once playback
     * completes or desyncs the pads are released and stock pause behavior
     * returns.
     *
     * THIS MUST STAY ABOVE THE SWK_START CHECK. A .3sr is a recording of an
     * ARCADE session, where START is not a pause button; the recorded words
     * therefore contain ordinary in-match START presses, and
     * Convert_User_Setting (sys_sub.c) passes SWK_START straight through.
     * With the carve-out sitting below, such a press took the branch above
     * and set Game_pause = 0x81, which stops Game_timer from incrementing
     * (game.c -> Game2_1) while playback keeps advancing — 4 of the 22
     * failures in the 44-replay comparison corpus, e.g. 1784866358738-6178,
     * whose P2 START rising edge at .3sr frame 2142 froze Game_timer at its
     * 2141 value. Suppressing the pause makes those replays verify clean
     * against the CPS3 checksums for the rest of the match.
     *
     * NOT VERIFIED: whether START additionally drives a personal
     * action/taunt in this engine. The only evidence gathered is that every
     * post-press checkpoint passes once the pause is suppressed, which does
     * not distinguish "the taunt is reproduced" from "the taunt affects no
     * hashed field".
     *
     * The STATCHECK carve-out now sits ABOVE the SWK_START check too, for the
     * identical reason. It used to sit below, and the comment here recorded
     * why that was safe: statcheck_runner.c -> read_input_buff mapped only the
     * direction and attack bits of the archived sw_lvbt mirror and never
     * emitted SWK_START, so the START branch was unreachable from it. That
     * precondition is GONE -- read_input_buff now also imports the raw
     * P1SW_0/P2SW_0 START bit, because effect_L7_init's arcade gate tests it
     * and no segment could reproduce that spawn without it. Leaving the
     * carve-out below cost exactly what the replay player's did: measured on
     * the 2026-09-06 corpus, two segments' t_pl_lvr counters fell one behind
     * the archive's (left_cnt 35 vs 36, right_cnt 53 vs 54) and one segment's
     * waza w_type did, all of them the pause stalling a frame the arcade ran.
     * A statcheck archive is the same arcade recording a .3sr is, so it gets
     * the same answer. */
    if (ReplayPlayer_GetStatus() == REPLAY_PLAYER_PLAYING) {
        return 0;
    }

#if defined(STATCHECK)
    /* A3b (docs/plan-fcade-replay-browser.md): statcheck replays inject
     * inputs directly into p1sw_buff/p2sw_buff with no physical controller
     * attached, so the connection check below would fire the Come_Out
     * ("controller unplugged") pause on the first gameplay frame and freeze
     * Game_timer, desyncing every replay at round start. Same carve-out as
     * the DEBUG test-runner block below (upstream's statcheck input driver
     * never hits this because its virtual pads report as connected). DEBUG
     * and STATCHECK cannot be co-compiled, so exactly one gate exists per
     * build.
     *
     * Placement: this returns before the SWK_START check on purpose -- see
     * the block comment above. STATCHECK is a harness-only build flavor, so
     * nothing here reaches the shipping binary. */
    return 0;
#endif

    if (sw & SWK_START) {
        Pause_Type = 1;
        return PAUSE_X = 1;
    }

#if defined(DEBUG)
    // This skips checking controller connection status during gameplay testing
    if (configuration.test.enabled) {
        return 0;
    }
#endif

    if (Present_Mode == 3) {
        if (Interface_Type[Decide_ID] == 0) {
            Pause_ID = Decide_ID;
            Pause_Type = 2;
            return PAUSE_X = 2;
        }
    } else if (Interface_Type[PL_id] == 0 && plw[PL_id].wu.wu_operator) {
        Pause_Type = 2;
        return PAUSE_X = 2;
    }

    return 0;
}

void Exit_Pause(struct _TASK* task_ptr) {
    u8 ix;

    if (Present_Mode != 3 && Check_Pause_Term(0, Pause_ID ^ 1)) {
        Exit_Menu = 0;
        return;
    }

    SE_selected();
    Game_pause = 0;
    Pause = 0;
    Pause_Down = 0;

    for (ix = 0; ix < 4; ix++) {
        task_ptr->r_no[ix] = 0;
        task_ptr->free[ix] = 0;
    }

    Menu_Suicide[0] = 1;
    Menu_Suicide[1] = 1;
    Menu_Suicide[2] = 1;
    Menu_Suicide[3] = 1;
    pulpul_request_again();
    cpExitTask(TASK_SAVER);
    cpExitTask(TASK_MENU);
    SsBgmHalfVolume(0);
}

void Setup_Pause(struct _TASK* task_ptr) {
    s16 ix;

    SE_selected();
    Pause_Down = 1;
    Game_pause = 0x81;
    task_ptr->r_no[0] = 1;
    task_ptr->r_no[2] = 1;
    task_ptr->free[0] = 1;
    cpReadyTask(TASK_MENU, Menu_Task);
    task[TASK_MENU].r_no[0] = 1;
    Exit_Menu = 0;

    for (ix = 0; ix < 4; ix++) {
        Menu_Suicide[ix] = 0;
    }

    Order[0x8A] = 3;
    Order_Timer[0x8A] = 1;
    effect_66_init(0x8A, 9, 2, 7, -1, -1, -0x3FFC);
    SsBgmHalfVolume(1);
    spu_all_off();
}

void Setup_Come_Out(struct _TASK* task_ptr) {
    s16 ix;

    SE_selected();
    Pause_Down = 1;
    Game_pause = 0x81;
    task_ptr->r_no[0] = 1;
    task_ptr->r_no[2] = 4;
    task_ptr->free[0] = 1;
    cpReadyTask(TASK_MENU, Menu_Task);
    task[TASK_MENU].r_no[0] = 1;
    Exit_Menu = 0;

    for (ix = 0; ix < 4; ix++) {
        Menu_Suicide[(ix)] = 0;
    }

    Order[0x8A] = 3;
    Order_Timer[0x8A] = 1;
    effect_66_init(0x8A, 9, 2, 7, -1, -1, -0x3FFC);
    SsBgmHalfVolume(1);
    spu_all_off();
}

s32 Check_Play_Status(s16 PL_id) {
    if (Mode_Type != MODE_VERSUS) {
        return Round_Operator[PL_id];
    }

    return 1;
}
