#if defined(STATCHECK)

/* Plan A3b (docs/plan-fcade-replay-browser.md): port of upstream
 * src/test/test_runner.c (upstream/main @ 3376518f) into fork idioms.
 *
 * Fork-shaped differences from upstream (each mirrors §4.3 of the plan):
 * - Input injection writes p1sw_buff/p2sw_buff directly (fork style — the
 *   same latch the DEBUG runner and netplay feed, src/main.c:595-603);
 *   upstream's apply_input_buffer/StatcheckInput_SetButtonState driver
 *   indirection is dropped entirely. The Prologue hook runs in game_step_0
 *   after keyConvert() and before the latch, so the injected words replace
 *   whatever the real pads produced this frame — equivalent to upstream's
 *   pre-Main_StepFrame injection point.
 * - ReplayGame is the fork's renamed ScrdGame (A3a), read helpers come from
 *   test/statcheck_utils.h, compare/sync from test/statcheck_compare.h.
 * - finish() prints a PASS verdict + frame count before exit(0) so the
 *   harness output is self-describing (upstream exits silently).
 *
 * LAYOUT INVARIANT (plan §2.3): read_input_buff below consumes the archived
 * WORK_CP sw_lvbt mirror (WCP_OFFSET / +0x406), which already carries the
 * engine's internal button layout — bits 4-6 punches, **8-10 kicks** (bit 7
 * unused) — NOT the raw arcade P1SW_0 register layout (kicks 7-9). The
 * bit-tested mapping is ported verbatim from upstream test_runner.c:82-128
 * and emits SWK-layout words; every word written to p1sw_buff/p2sw_buff goes
 * through it. Do not "simplify" it into a raw copy and do not feed it
 * P1SW_0-style words. */

#include "test/statcheck_runner.h"
#include "arcade/arcade_constants.h"
#include "constants.h"
#include "main.h"
#include "port/config/config.h"
#include "port/sdl/sdl_app.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/debug/debug_config.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"
#include "test/ram_archive.h"
#include "test/scrd_game.h"
#include "test/statcheck_compare.h"
#include "test/statcheck_seed_audit.h"
#include "test/statcheck_utils.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>

typedef enum Phase {
    PHASE_TITLE,
    PHASE_MENU,
    PHASE_CHARACTER_SELECT_TRANSITION,
    PHASE_CHARACTER_SELECT,
    PHASE_GAME_TRANSITION,
    PHASE_GAME,
} Phase;

static const Uint8 character_to_cursor[20][2] = { { 7, 1 }, { 1, 0 }, { 5, 2 }, { 6, 1 }, { 3, 2 }, { 4, 0 }, { 1, 2 },
                                                  { 3, 0 }, { 2, 2 }, { 4, 2 }, { 0, 1 }, { 0, 2 }, { 2, 0 }, { 5, 0 },
                                                  { 6, 0 }, { 3, 1 }, { 2, 1 }, { 4, 1 }, { 1, 1 }, { 5, 1 } };

/* Character-select color picker: which button (or Start+button) selects each
 * of the 13 colors. Upstream test_runner.c:35-49. */
static const SWKey color_to_keys[13] = {
    SWK_WEST,
    SWK_NORTH,
    SWK_RIGHT_SHOULDER,
    SWK_SOUTH,
    SWK_EAST,
    SWK_RIGHT_TRIGGER,
    SWK_WEST | SWK_RIGHT_SHOULDER | SWK_EAST,
    SWK_START | SWK_WEST,
    SWK_START | SWK_NORTH,
    SWK_START | SWK_RIGHT_SHOULDER,
    SWK_START | SWK_SOUTH,
    SWK_START | SWK_EAST,
    SWK_START | SWK_RIGHT_TRIGGER,
};

static Uint64 frame_index = 0;
static Phase phase = PHASE_TITLE;
static int char_select_phase = 0;
static int wait_timer = 0;
static int comparison_index = 0;
static ScrdGame game;
static Uint16 input_buffers[2] = { 0 };
static SDL_IOStream* frame_io = NULL;

static void set_cursor(Character character, int player) {
    Cursor_X[player] = character_to_cursor[character][0];
    Cursor_Y[player] = character_to_cursor[character][1];
}

/// Repeatedly press and release a button
static void mash_button(SWKey button, int player) {
    input_buffers[player] |= (frame_index & 1) ? button : 0;
}

static void tap_button(SWKey button, int player) {
    input_buffers[player] |= button;
}

static bool game_ended(void) {
    return (PL_Wins[0] == 2) || (PL_Wins[1] == 2);
}

static void finish(void) {
    /* Reaching here means every archived frame handed to
     * Statcheck_CompareValues matched (a mismatch would have exited 1 from
     * statcheck_compare.c's stop_if before we ran out of frames). */
    printf("statcheck: PASS — compared archive frames %d..%d of %u\n",
           game.start_index,
           comparison_index - 1,
           game.archive.entry_count);

    if (frame_io != NULL) {
        SDL_CloseIO(frame_io);
    }

    ScrdGame_Destroy(&game);
    exit(0);
}

/* Arcade->SWK input conversion — verbatim upstream test_runner.c:82-128.
 * See the layout-invariant comment at the top of this file. */
static Uint16 read_input_buff(SDL_IOStream* io, int player) {
    const Sint64 sw_lvbt_offset = (player == 0) ? WCP_OFFSET : WCP_OFFSET + 0x406;
    const Uint16 sw_lvbt_buff = read_u16(io, sw_lvbt_offset);
    Uint16 buff = 0;

    if (sw_lvbt_buff & (1 << 0)) {
        buff |= SWK_UP;
    }

    if (sw_lvbt_buff & (1 << 1)) {
        buff |= SWK_DOWN;
    }

    if (sw_lvbt_buff & (1 << 2)) {
        buff |= SWK_LEFT;
    }

    if (sw_lvbt_buff & (1 << 3)) {
        buff |= SWK_RIGHT;
    }

    if (sw_lvbt_buff & (1 << 4)) {
        buff |= SWK_WEST;
    }

    if (sw_lvbt_buff & (1 << 5)) {
        buff |= SWK_NORTH;
    }

    if (sw_lvbt_buff & (1 << 6)) {
        buff |= SWK_RIGHT_SHOULDER;
    }

    if (sw_lvbt_buff & (1 << 8)) {
        buff |= SWK_SOUTH;
    }

    if (sw_lvbt_buff & (1 << 9)) {
        buff |= SWK_EAST;
    }

    if (sw_lvbt_buff & (1 << 10)) {
        buff |= SWK_RIGHT_TRIGGER;
    }

    return buff;
}

/* Inter-round/inter-scene skip — upstream test_runner.c:153-158. The
 * archived session skipped win poses & continue screens with button taps;
 * mirror that whenever the archive's C_No/Scene_Cut says we're in one. */
static bool inter_round_skip_needed(void) {
    const Uint16 c_no_0_cps3 = read_u16(frame_io, C_NO_OFFSET + 0 * sizeof(u16));
    const Uint16 c_no_1_cps3 = read_u16(frame_io, C_NO_OFFSET + 1 * sizeof(u16));
    const Uint8 scene_cut_cps3 = read_u8(frame_io, SCENE_CUT_OFFSET);
    return ((c_no_0_cps3 == 6) && (c_no_1_cps3 == 3)) || (scene_cut_cps3 && (c_no_0_cps3 > 6));
}

/* Review round-1 finding P-1: default in-game button remap, ported
 * verbatim from netplay.c:458-467's identity table (same rationale —
 * make Convert_User_Setting, sys_sub.c:101, a no-op so the SWK-layout
 * words this harness writes into p1sw_buff/p2sw_buff pass through
 * Convert_Data (sys_sub.c:67) unchanged). Present_Mode can land on more
 * than one save_w[] slot depending how far the console-mode phase
 * machine's nav gets (0 = MODE_ARCADE pre-select, 1 = MODE_VERSUS once a
 * match starts — game.c:1662/1829), so every slot is pinned rather than
 * guessing which one is live at pin time. */
static void pin_default_button_mapping(void) {
    static const u8 identity[8] = { 0, 1, 2, 11, 3, 4, 5, 11 };

    for (int mode = 0; mode < 6; mode++) {
        for (int p = 0; p < 2; p++) {
            for (int s = 0; s < 8; s++) {
                save_w[mode].Pad_Infor[p].Shot[s] = identity[s];
            }

            save_w[mode].Pad_Infor[p].Vibration = 0;
        }
    }
}

void StatcheckRunner_PinConfig(void) {
    const bool was_arcade_mode = SDLApp_IsArcadeGameMode();
    const char* was_balance = Config_GetString(CFG_KEY_BALANCE);

    /* game-mode=console: the arcade path (Loop_Demo branch, game.c) jumps
     * straight into Game12/MODE_ARCADE on the first Coin press and never
     * reaches the Menu_Task r_no sequence StatcheckRunner_Prologue's
     * PHASE_TITLE/PHASE_MENU watch for — an "arcade" game-mode in the
     * user's config would hang the phase machine forever. Session-only;
     * SDLApp_ForceConsoleGameMode (sdl_app.c) never touches the on-disk
     * config file. */
    SDLApp_ForceConsoleGameMode();

    /* balance=auto: ArcadeBalance_Init(), called right after this from
     * main.c's initialize_game(), decides whether to load sfiii3nr1.zip's
     * data tables. The archive was captured against those tables, so a
     * PS2-balance resolution would desync every replay from frame 1.
     *
     * Upstream reconcile: the old boolean "arcade-balance" key is gone;
     * balance now auto-selects at boot and CFG_KEY_BALANCE only ever forces
     * balance DOWN to PS2. Clearing it to "auto" is therefore the strongest
     * arcade-ward pin available, and it still delivers the hermeticity this
     * pin exists for (the user's on-disk "ps2" override cannot leak in).
     * Config_SetString only mutates the in-memory entries[] table
     * (config.c) — Config_Save() is a no-op stub, so the file is untouched.
     *
     * KNOWN LIMIT: ArcadeBalance_Init pins PS2 whenever
     * configuration.test.enabled is set (the frame-data corpora encode
     * PS2-balance expectations). A statcheck run must therefore NOT also
     * set --test-enable, or balance resolves PS2 regardless of this pin. */
    Config_SetString(CFG_KEY_BALANCE, "auto");

    pin_default_button_mapping();

    SDL_Log("statcheck: pinned hermetic config -- game-mode=console (was %s) "
            "balance=auto (was %s) button-mapping=default (identity, all save_w[] slots)",
            was_arcade_mode ? "arcade" : "console",
            was_balance != NULL ? was_balance : "auto");
}

ScrdGameInitResult StatcheckRunner_Init(const char* ram_archive_path) {
    const ScrdGameInitResult result = ScrdGame_Init(&game, ram_archive_path);

    if (result != SCRD_GAME_INIT_OK) {
        /* ScrdGame_Init has already logged the specific reason. Do not call
         * either harness limit a failure here: NO_MATCH_START (H1) and
         * CPU_PLAYER (H4b) are correct verdicts about the archive, and main.c
         * turns them into exit 2 / 3 rather than the divergence code. */
        SDL_Log("StatcheckRunner_Init: not comparing '%s' (ScrdGame_Init result %d)",
                ram_archive_path,
                (int)result);
        return result;
    }

    comparison_index = game.start_index;
    return SCRD_GAME_INIT_OK;
}

Uint8 StatcheckRunner_WuOperator(int player) {
    return game.wu_operator[player & 1];
}

void StatcheckRunner_Destroy(void) {
    ScrdGame_Destroy(&game);
}

void StatcheckRunner_Prologue(void) {
    SDL_zeroa(input_buffers);

    switch (phase) {
    case PHASE_TITLE: {
        const struct _TASK* menu_task = &task[TASK_MENU];

        if (menu_task->r_no[0] == 0 && menu_task->r_no[1] == 1 && menu_task->r_no[2] == 3) {
            phase = PHASE_MENU;
            break;
        }

        mash_button(SWK_START, 0);
        break;
    }

    case PHASE_MENU:
        if (G_No[1] == 1 && G_No[2] == 2) {
            // Even though we move cursor manually later, setting Last_My_char2 is required
            // for Last_Super_Arts to take effect
            Last_My_char2[0] = game.characters[0];
            Last_My_char2[1] = game.characters[1];
            Last_Super_Arts[0] = game.supers[0];
            Last_Super_Arts[1] = game.supers[1];
            /* Stage pin (H2, docs/research-arcade-balance-desyncs.md). The
             * archive's stage is a carried-over session fact that the synthetic
             * char-select cannot reconstruct, and it feeds
             * `app_type_tbl[own][opp][bg_w.stage]` in `appear_data_init_set()`
             * (`appear.c`) -- which sets `wu.routine_no[4]` AND
             * `wu.xyz[0].disp.pos` at battle start. Route it through the
             * engine's own override rather than writing `bg_w.stage`: `Exit_2nd()`
             * (`sel_pl.c`) reads `Debug_w[31]` right after `Setup_Battle_Country()`
             * and does `Battle_Country = bg_w.stage = Debug_w[31] - 1;` before
             * `Push_LDREQ_Queue_BG(bg_w.stage)`, so the BG load request is issued
             * for the pinned stage too. Same override the DEBUG harness uses
             * (`test_runner.c` -> `apply_stage_override`). */
            Debug_w[DEBUG_STAGE_SELECT] = (s8)(game.stage + 1);
            phase = PHASE_CHARACTER_SELECT_TRANSITION;
            wait_timer = 60;
            break;
        }

        mash_button(SWK_SOUTH, 0);
        break;

    case PHASE_CHARACTER_SELECT_TRANSITION:
        wait_timer -= 1;

        if (wait_timer <= 0) {
            phase = PHASE_CHARACTER_SELECT;
        }

        break;

    case PHASE_CHARACTER_SELECT:
        switch (char_select_phase) {
        case 0:
            set_cursor(game.characters[0], 0);
            set_cursor(game.characters[1], 1);
            tap_button(SWK_START, 1);
            wait_timer = 20;
            char_select_phase = 1;
            break;

        case 1:
            wait_timer -= 1;

            if (wait_timer <= 0) {
                // We must set New_Challenger manually so that the game selects the correct stage.
                // If we set this var earlier it would be overwritten
                New_Challenger = game.new_challenger;
                // Restore the arcade invariant Champion == New_Challenger ^ 1 (entry.c:1326-1356).
                // Our synthetic char-select leaves Champion==0; without this, mirror matches with
                // recorded New_Challenger==0 desync at battle start via home_visitor_check()
                // (appear.c app_type_tbl vs app_type_tbl2). Paired with the same line in
                // replay_player.c — the gate and the player must agree. (upstream #289)
                Champion = New_Challenger ^ 1;
                char_select_phase = 2;
            }

            break;

        case 2:
            tap_button(color_to_keys[game.colors[0]], 0);
            tap_button(color_to_keys[game.colors[1]], 1);
            wait_timer = 45;
            char_select_phase = 3;
            break;

        case 3:
            wait_timer -= 1;

            if (wait_timer <= 0) {
                tap_button(SWK_SOUTH, 0);
                tap_button(SWK_SOUTH, 1);
                phase = PHASE_GAME_TRANSITION;
            }

            break;
        }

        break;

    case PHASE_GAME_TRANSITION: {
        if (G_No[1] != 2) {
            // This skips the VS animation
            mash_button(SWK_ATTACKS, 0);
            break;
        }

        /* RNG sync from the pre-game frame (start_index - 1) — upstream
         * test_runner.c:263-265. Syncing from start_index itself is the
         * known frame-1-desync trap (plan A3b "If it fails"). */
        SDL_IOStream* initial_frame = RamArchive_GetFrame(&game.archive, comparison_index - 1);
        Statcheck_SyncValues(initial_frame);

        /* Seed audit (docs/research-arcade-balance-desyncs.md, "The seed
         * audit"). Runs HERE and nowhere else: after the import, before the
         * engine has executed a single compared frame. Anything that differs
         * at this instant is an initial condition the harness failed to
         * reproduce, never engine behaviour -- and saying so here costs one
         * pass over the frame, where saying it 3,000 frames later has twice
         * cost a retracted engine-defect report. Read-only. */
        StatcheckSeedAudit_Run(initial_frame, comparison_index - 1);

        SDL_CloseIO(initial_frame);
        phase = PHASE_GAME;
    }
        /* fallthrough */

    case PHASE_GAME:
        frame_io = RamArchive_GetFrame(&game.archive, comparison_index);

        if ((frame_io == NULL) || game_ended()) {
            finish();
        }

        input_buffers[0] = read_input_buff(frame_io, 0);
        input_buffers[1] = read_input_buff(frame_io, 1);

        if (inter_round_skip_needed()) {
            tap_button(SWK_ATTACKS, 0);
            tap_button(SWK_ATTACKS, 1);
        }

        break;
    }

    /* Fork-style injection: write the SWK-layout words straight into the
     * pre-latch buffers (upstream instead routed them through its statcheck
     * input driver). game_step_0 latches p1sw_0 = p1sw_buff right after
     * this hook returns. */
    p1sw_buff = input_buffers[0];
    p2sw_buff = input_buffers[1];
}

/* Optional per-frame engine trace for divergence triage (A4 will want this
 * too): set STATCHECK_TRACE=1 to print the engine-side counterparts of the
 * fields tools/../scrd_dump-style archive dumps show, so the two timelines
 * can be diffed frame-by-frame. Off by default; zero cost when unset. */
static bool trace_enabled(void) {
    static int cached = -1;

    if (cached < 0) {
        const char* env = SDL_getenv("STATCHECK_TRACE");
        cached = (env != NULL && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }

    return cached == 1;
}

static void trace_frame(void) {
    printf("statcheck-trace f=%d C=[%u,%u,%u,%u] G=[%u,%u,%u,%u] timer=%u "
           "wipe=%u gap=%u cover=%d demo=%d play=%u break=%d pause=%02x allow=%u p1=%04x p2=%04x\n",
           comparison_index,
           C_No[0],
           C_No[1],
           C_No[2],
           C_No[3],
           G_No[0],
           G_No[1],
           G_No[2],
           G_No[3],
           Game_timer,
           WipeLimit,
           Gap_Timer,
           Cover_Timer,
           Demo_Flag,
           Play_Mode,
           Break_Into,
           Game_pause,
           Allow_a_battle_f,
           input_buffers[0],
           input_buffers[1]);
}

void StatcheckRunner_Epilogue(void) {
    switch (phase) {
    case PHASE_GAME:
        if (trace_enabled()) {
            trace_frame();
        }
        /* Pass the archive frame index (not upstream's global frame_index)
         * so mismatch reports name the frame you can seek to in the SCRD;
         * Statcheck_CompareValues only uses it for its warm-up window and
         * error reporting, so the substitution is behavior-neutral. */
        Statcheck_CompareValues(frame_io, (Uint64)comparison_index);
        SDL_CloseIO(frame_io);
        frame_io = NULL;
        comparison_index += 1;
        break;

    default:
        // Do nothing
        break;
    }

    frame_index += 1;
}

#endif
