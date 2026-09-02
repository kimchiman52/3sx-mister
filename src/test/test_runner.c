#if defined(DEBUG)

#include "test/test_runner.h"
#include "arcade/arcade_constants.h"
#include "constants.h"
#include "main.h"
#include "port/config/config.h"
#include "port/sdl/sdl_app.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/debug/debug_config.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/spgauge.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/sysdir.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/count.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"
#include "test/input_script.h"
#include "test/replay_game.h"
#include "test/scene_jump_spike.h"
#include "test/test_runner_compare.h"
#include "test/test_runner_utils.h"

#include "stb/stb_ds.h"
#include <SDL3/SDL.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#define MY_CHAR_OFFSET 0x11387
#define SUPER_ARTS_OFFSET 0x1138B
#define GAME_ROUTINE_OFFSET 0x15438
#define P1SW_OFFSET 0x6AA8C
#define P2SW_OFFSET 0x6AA90

#define REPLAY_FRAMES_MAX 3 * 100 * 60

#define SWAP16(val) ((val << 8) | (val >> 8))

/* Character enum comes from constants.h (included above). The local
 * duplicate used to diverge slightly and was caught only when #if DEBUG
 * was actually compiled. */

typedef enum Phase {
    PHASE_INIT,
    PHASE_TITLE,
    PHASE_MENU,
    PHASE_CHARACTER_SELECT_TRANSITION,
    PHASE_CHARACTER_SELECT,
    PHASE_GAME_TRANSITION,
    PHASE_GAME,
} Phase;

typedef enum TestScenePreset {
    TEST_SCENE_PRESET_NONE,
    TEST_SCENE_PRESET_STAGE_HEAVY,
    TEST_SCENE_PRESET_EFFECT_HEAVY,
    TEST_SCENE_PRESET_SUPER_HEAVY,
    TEST_SCENE_PRESET_YUN_SA3_REPEAT,
    TEST_SCENE_PRESET_YUN_SA3_REPEAT_PRESSURE,
    TEST_SCENE_PRESET_Q_SA1_REPEAT,
    TEST_SCENE_PRESET_Q_SA1_REPEAT_PRESSURE,
    TEST_SCENE_PRESET_KEN_SA3_REPEAT,
    TEST_SCENE_PRESET_KEN_SA3_REPEAT_PRESSURE,
    TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT,
    TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT_PRESSURE,
    TEST_SCENE_PRESET_BASIC_EXCHANGE,
    TEST_SCENE_PRESET_PRESSURE_EXCHANGE,
    TEST_SCENE_PRESET_LEFT_CORNER_RYU_STAGE,
    TEST_SCENE_PRESET_TRAINING_YUN_RYU_RYU_STAGE,
    TEST_SCENE_PRESET_TRAINING_FRAME_DATA,
} TestScenePreset;

static const Uint8 character_to_cursor[20][2] = { { 7, 1 }, { 1, 0 }, { 5, 2 }, { 6, 1 }, { 3, 2 }, { 4, 0 }, { 1, 2 },
                                                  { 3, 0 }, { 2, 2 }, { 4, 2 }, { 0, 1 }, { 0, 2 }, { 2, 0 }, { 5, 0 },
                                                  { 6, 0 }, { 3, 1 }, { 2, 1 }, { 4, 1 }, { 1, 1 }, { 5, 1 } };
static const Sint8 scene_preset_basic_exchange_stage = 11;
static const Sint8 scene_preset_stage_heavy_stage = 19;
static const Sint8 scene_preset_training_yun_ryu_ryu_stage = 2;
static const int scene_preset_repeat_first_super_frame = 150;
static const int scene_preset_repeat_second_super_frame = 620;
static const int scene_preset_repeat_super_input_frames = 40;
static const int scene_preset_repeat_super_prep_frames = 32;
static const int scene_preset_repeat_second_super_refill_frame = 560;

static Uint64 frame = 0;
static Phase phase = PHASE_INIT;
static int char_select_phase = 0;
static int wait_timer = 0;
static int game_frame = 0;
static Sint8 characters[2] = { -1, -1 };
static Sint8 selected_super_arts[2] = { -1, -1 };
static Sint8 stage = -1;
static TestScenePreset scene_preset = TEST_SCENE_PRESET_NONE;
static int player_super_art_activation_starts[2] = { 0, 0 };
static bool player_super_art_was_active[2] = { false, false };
static bool scene_preset_repeat_second_super_ready = false;
/* Task #108, see PHASE_CHARACTER_SELECT. Seeded from
 * configuration.test.select_dwell_frames when the phase is first entered. */
static int select_dwell_remaining = -1;
static u16 inputs[REPLAY_FRAMES_MAX][2] = { 0 };
static int inputs_index = 0;
static int inputs_total = 0;

static const char* phase_name(Phase current_phase) {
    switch (current_phase) {
    case PHASE_INIT:
        return "init";
    case PHASE_TITLE:
        return "title";
    case PHASE_MENU:
        return "menu";
    case PHASE_CHARACTER_SELECT_TRANSITION:
        return "character-select-transition";
    case PHASE_CHARACTER_SELECT:
        return "character-select";
    case PHASE_GAME_TRANSITION:
        return "game-transition";
    case PHASE_GAME:
        return "game";
    }

    return "unknown";
}

static bool gameplay_input_active(void) {
    return phase == PHASE_GAME && plw[0].wu.routine_no[0] == 4 && plw[1].wu.routine_no[0] == 4;
}

static bool player_super_art_active(int player) {
    return phase == PHASE_GAME && player >= 0 && player < 2 && plw[player].sa != NULL && plw[player].sa->ok == -1;
}

static bool wipe_transition_type1_active(void) {
    return Exec_Wipe != 0 && Active_Wipe_Type == 1 && WipeLimit < 8;
}

static TestScenePreset resolve_scene_preset(const char* preset_name) {
    if (preset_name == NULL || preset_name[0] == '\0') {
        return TEST_SCENE_PRESET_NONE;
    }

    if (SDL_strcmp(preset_name, "stage-heavy") == 0) {
        return TEST_SCENE_PRESET_STAGE_HEAVY;
    }
    if (SDL_strcmp(preset_name, "effect-heavy") == 0) {
        return TEST_SCENE_PRESET_EFFECT_HEAVY;
    }
    if (SDL_strcmp(preset_name, "super-heavy") == 0) {
        return TEST_SCENE_PRESET_SUPER_HEAVY;
    }
    if (SDL_strcmp(preset_name, "yun-sa3-repeat") == 0) {
        return TEST_SCENE_PRESET_YUN_SA3_REPEAT;
    }
    if (SDL_strcmp(preset_name, "yun-sa3-repeat-pressure") == 0) {
        return TEST_SCENE_PRESET_YUN_SA3_REPEAT_PRESSURE;
    }
    if (SDL_strcmp(preset_name, "q-sa1-repeat") == 0) {
        return TEST_SCENE_PRESET_Q_SA1_REPEAT;
    }
    if (SDL_strcmp(preset_name, "q-sa1-repeat-pressure") == 0) {
        return TEST_SCENE_PRESET_Q_SA1_REPEAT_PRESSURE;
    }
    if (SDL_strcmp(preset_name, "ken-sa3-repeat") == 0) {
        return TEST_SCENE_PRESET_KEN_SA3_REPEAT;
    }
    if (SDL_strcmp(preset_name, "ken-sa3-repeat-pressure") == 0) {
        return TEST_SCENE_PRESET_KEN_SA3_REPEAT_PRESSURE;
    }
    if (SDL_strcmp(preset_name, "chunli-sa2-repeat") == 0) {
        return TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT;
    }
    if (SDL_strcmp(preset_name, "chunli-sa2-repeat-pressure") == 0) {
        return TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT_PRESSURE;
    }
    if (SDL_strcmp(preset_name, "basic-exchange") == 0) {
        return TEST_SCENE_PRESET_BASIC_EXCHANGE;
    }
    if (SDL_strcmp(preset_name, "pressure-exchange") == 0) {
        return TEST_SCENE_PRESET_PRESSURE_EXCHANGE;
    }
    if (SDL_strcmp(preset_name, "left-corner-ryu-stage") == 0) {
        return TEST_SCENE_PRESET_LEFT_CORNER_RYU_STAGE;
    }
    if (SDL_strcmp(preset_name, "training-yun-ryu-ryu-stage") == 0) {
        return TEST_SCENE_PRESET_TRAINING_YUN_RYU_RYU_STAGE;
    }
    if (SDL_strcmp(preset_name, "training-frame-data") == 0) {
        return TEST_SCENE_PRESET_TRAINING_FRAME_DATA;
    }

    return TEST_SCENE_PRESET_NONE;
}

bool TestRunner_IsSupportedPhaseName(const char* phase_name_value) {
    if (phase_name_value == NULL || phase_name_value[0] == '\0') {
        return false;
    }

    return SDL_strcmp(phase_name_value, "init") == 0 || SDL_strcmp(phase_name_value, "title") == 0 ||
           SDL_strcmp(phase_name_value, "menu") == 0 ||
           SDL_strcmp(phase_name_value, "character-select-transition") == 0 ||
           SDL_strcmp(phase_name_value, "character-select") == 0 ||
           SDL_strcmp(phase_name_value, "game-transition") == 0 || SDL_strcmp(phase_name_value, "game") == 0 ||
           SDL_strcmp(phase_name_value, "game-input-active") == 0 ||
           SDL_strcmp(phase_name_value, "p1-super-art-active") == 0 ||
           SDL_strcmp(phase_name_value, "p1-super-art-active-2") == 0 ||
           SDL_strcmp(phase_name_value, "wipe-transition-type1") == 0;
}

const char* TestRunner_GetPhaseName(void) {
    return phase_name(phase);
}

bool TestRunner_IsPhaseActive(const char* phase_name_value) {
    if (!TestRunner_IsSupportedPhaseName(phase_name_value)) {
        return false;
    }

    if (SDL_strcmp(phase_name_value, "game") == 0) {
        return phase == PHASE_GAME;
    }

    if (SDL_strcmp(phase_name_value, "game-input-active") == 0) {
        return gameplay_input_active();
    }

    if (SDL_strcmp(phase_name_value, "p1-super-art-active") == 0) {
        return player_super_art_active(0);
    }

    if (SDL_strcmp(phase_name_value, "p1-super-art-active-2") == 0) {
        return player_super_art_active(0) && player_super_art_activation_starts[0] >= 2;
    }

    if (SDL_strcmp(phase_name_value, "wipe-transition-type1") == 0) {
        return wipe_transition_type1_active();
    }

    return SDL_strcmp(TestRunner_GetPhaseName(), phase_name_value) == 0;
}

static SWKey player_forward_button(int player) {
    return player ? SWK_LEFT : SWK_RIGHT;
}

static u16 projectile_script_input(int player, int local_frame) {
    const SWKey forward = player_forward_button(player);

    switch (local_frame) {
    case 0:
    case 1:
    case 2:
        return SWK_DOWN;
    case 3:
    case 4:
    case 5:
        return (u16)(SWK_DOWN | forward);
    case 6:
    case 7:
        return forward;
    case 8:
        return (u16)(forward | SWK_WEST);
    default:
        return 0;
    }
}

static u16 super_script_input_with_button(int player, int local_frame, SWKey attack_button) {
    const SWKey forward = player_forward_button(player);

    switch (local_frame) {
    case 0:
    case 1:
    case 2:
    case 3:
        return SWK_DOWN;
    case 4:
    case 5:
    case 6:
    case 7:
        return (u16)(SWK_DOWN | forward);
    case 8:
    case 9:
    case 10:
    case 11:
        return forward;
    case 16:
    case 17:
    case 18:
    case 19:
        return SWK_DOWN;
    case 20:
    case 21:
    case 22:
    case 23:
        return (u16)(SWK_DOWN | forward);
    case 24:
    case 25:
    case 26:
    case 27:
        return forward;
    case 30:
        return (u16)(forward | attack_button);
    default:
        return 0;
    }
}

static u16 super_script_input(int player, int local_frame) {
    return super_script_input_with_button(player, local_frame, SWK_WEST);
}

static u16 basic_exchange_script_input(int player, int local_frame) {
    const SWKey forward = player_forward_button(player);
    const SWKey backward = player ? SWK_RIGHT : SWK_LEFT;
    const int cycle_frame = local_frame % 96;

    switch (cycle_frame) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
        return forward;
    case 16:
    case 17:
    case 18:
    case 19:
        return (u16)(SWK_DOWN | backward);
    case 20:
    case 21:
        return SWK_SOUTH;
    case 28:
    case 29:
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
        return (u16)(SWK_UP | forward);
    case 36:
    case 37:
        return (u16)(SWK_UP | forward | SWK_SOUTH);
    case 42:
    case 43:
    case 44:
    case 45:
        return (u16)(SWK_DOWN | SWK_WEST);
    case 56:
    case 57:
    case 58:
    case 59:
    case 60:
    case 61:
    case 62:
    case 63:
        return backward;
    case 64:
    case 65:
    case 66:
    case 67:
    case 68:
    case 69:
    case 70:
    case 71:
        return forward;
    case 72:
    case 73:
        return SWK_WEST;
    default:
        return 0;
    }
}

static u16 pressure_exchange_attack_input(int player, int local_frame) {
    const SWKey forward = player_forward_button(player);

    switch (local_frame) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
        return forward;
    case 12:
    case 13:
        return SWK_SOUTH;
    case 20:
    case 21:
    case 22:
    case 23:
        return (u16)(SWK_DOWN | SWK_WEST);
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
        return (u16)(SWK_UP | forward);
    case 38:
    case 39:
        return (u16)(SWK_UP | forward | SWK_SOUTH);
    case 48:
    case 49:
        return SWK_WEST;
    case 58:
    case 59:
    case 60:
    case 61:
    case 62:
    case 63:
        return forward;
    case 64:
    case 65:
    case 66:
    case 67:
        return (u16)(SWK_DOWN | SWK_WEST);
    default:
        return 0;
    }
}

static u16 pressure_exchange_defend_input(int player, int local_frame) {
    const SWKey forward = player_forward_button(player);
    const SWKey backward = player ? SWK_RIGHT : SWK_LEFT;

    switch (local_frame) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
        return forward;
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
        return (u16)(SWK_DOWN | backward);
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
        return backward;
    case 38:
    case 39:
    case 40:
    case 41:
    case 42:
    case 43:
    case 44:
    case 45:
        return (u16)(SWK_UP | backward);
    case 52:
    case 53:
    case 54:
    case 55:
        return backward;
    case 60:
    case 61:
        return SWK_SOUTH;
    case 66:
    case 67:
    case 68:
    case 69:
        return (u16)(SWK_DOWN | backward);
    default:
        return 0;
    }
}

static u16 pressure_exchange_script_input(int player, int local_frame) {
    const int cycle_frame = local_frame % 144;
    const bool p1_pressure_turn = cycle_frame < 72;
    const int turn_frame = p1_pressure_turn ? cycle_frame : (cycle_frame - 72);
    const bool is_attacker = (player == 0) ? p1_pressure_turn : !p1_pressure_turn;

    if (is_attacker) {
        return pressure_exchange_attack_input(player, turn_frame);
    }

    return pressure_exchange_defend_input(player, turn_frame);
}

static bool scene_preset_uses_repeat_second_super(void) {
    switch (scene_preset) {
    case TEST_SCENE_PRESET_YUN_SA3_REPEAT:
    case TEST_SCENE_PRESET_YUN_SA3_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_Q_SA1_REPEAT:
    case TEST_SCENE_PRESET_Q_SA1_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_KEN_SA3_REPEAT:
    case TEST_SCENE_PRESET_KEN_SA3_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT:
    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT_PRESSURE:
        return true;
    default:
        return false;
    }
}

static bool scene_preset_uses_repeat_pressure(void) {
    switch (scene_preset) {
    case TEST_SCENE_PRESET_YUN_SA3_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_Q_SA1_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_KEN_SA3_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT_PRESSURE:
        return true;
    default:
        return false;
    }
}

static bool scene_preset_repeat_super_input_active(int start_frame) {
    return game_frame >= start_frame && game_frame < (start_frame + scene_preset_repeat_super_input_frames);
}

static bool scene_preset_repeat_super_prep_active(int start_frame) {
    const int prep_start = start_frame - scene_preset_repeat_super_prep_frames;

    return game_frame >= prep_start && game_frame < start_frame;
}

static SWKey scene_preset_repeat_super_attack_button(void) {
    switch (scene_preset) {
    case TEST_SCENE_PRESET_KEN_SA3_REPEAT:
    case TEST_SCENE_PRESET_KEN_SA3_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT:
    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT_PRESSURE:
        return SWK_SOUTH;
    default:
        return SWK_WEST;
    }
}

static u16 left_corner_ryu_stage_reanchor_input(int player) {
    return player == 0 ? SWK_LEFT : SWK_LEFT;
}

static bool left_corner_ryu_stage_needs_reanchor(void) {
    const int p1_x = plw[0].wu.position_x;
    const int p2_x = plw[1].wu.position_x;

    return p1_x > 144 || p2_x > 224 || (p2_x - p1_x) > 96;
}

static u16 left_corner_ryu_stage_script_input(int player, int local_frame) {
    const int cycle_frame = local_frame % 192;

    // Keep the fight biased into the left corner, then reuse the existing exchange script
    // so both players still trade attacks once they get there.
    if (cycle_frame < 48 || left_corner_ryu_stage_needs_reanchor()) {
        return left_corner_ryu_stage_reanchor_input(player);
    }

    if (cycle_frame < 64) {
        return (u16)(left_corner_ryu_stage_reanchor_input(player) | (player == 0 ? SWK_DOWN : 0));
    }

    return pressure_exchange_script_input(player, cycle_frame - 64);
}

static u16 training_yun_ryu_ryu_stage_script_input(int player, int local_frame) {
    if (player != 0) {
        return 0;
    }

    switch (local_frame % 192) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
        return SWK_RIGHT;
    case 36:
    case 37:
        return SWK_SOUTH;
    case 44:
    case 45:
        return SWK_WEST;
    case 56:
    case 57:
    case 58:
    case 59:
    case 60:
    case 61:
    case 62:
    case 63:
        return SWK_RIGHT;
    case 64:
    case 65:
        return (u16)(SWK_RIGHT | SWK_SOUTH);
    case 72:
    case 73:
    case 74:
    case 75:
    case 76:
    case 77:
        return (u16)(SWK_UP | SWK_RIGHT);
    case 78:
    case 79:
        return (u16)(SWK_UP | SWK_RIGHT | SWK_SOUTH);
    case 92:
    case 93:
        return SWK_WEST;
    case 108:
    case 109:
    case 110:
    case 111:
    case 112:
    case 113:
    case 114:
    case 115:
    case 116:
    case 117:
    case 118:
    case 119:
        return SWK_RIGHT;
    case 120:
    case 121:
        return (u16)(SWK_DOWN | SWK_WEST);
    case 130:
    case 131:
        return SWK_SOUTH;
    case 142:
    case 143:
    case 144:
    case 145:
    case 146:
    case 147:
    case 148:
    case 149:
        return SWK_LEFT;
    case 156:
    case 157:
    case 158:
    case 159:
    case 160:
    case 161:
    case 162:
    case 163:
        return SWK_RIGHT;
    case 164:
    case 165:
        return SWK_WEST;
    default:
        return 0;
    }
}

static bool scene_preset_uses_training_mode(void) {
    return scene_preset == TEST_SCENE_PRESET_TRAINING_YUN_RYU_RYU_STAGE ||
           scene_preset == TEST_SCENE_PRESET_TRAINING_FRAME_DATA;
}

static bool training_mode_gameplay_started(void) {
    return scene_preset_uses_training_mode() && Mode_Type == MODE_NORMAL_TRAINING && Allow_a_battle_f != 0 && Game_pause == 0 &&
           Pause_Down == 0;
}

static void maybe_force_training_scene_character_and_super_state(void) {
    if (!scene_preset_uses_training_mode()) {
        return;
    }

    for (int player = 0; player < 2; player++) {
        if (Sel_Arts_Complete[player] != 0) {
            continue;
        }

        if (My_char[player] != characters[player]) {
            My_char[player] = characters[player];
            Last_My_char2[player] = characters[player];
        }

        if (selected_super_arts[player] >= 0 && Arts_Y[player] != selected_super_arts[player]) {
            Arts_Y[player] = selected_super_arts[player];
            Super_Arts[player] = selected_super_arts[player];
            Last_Super_Arts[player] = selected_super_arts[player];
        }
    }
}

static void maybe_force_training_scene_super_confirm(void) {
    if (!scene_preset_uses_training_mode()) {
        return;
    }

    for (int player = 0; player < 2; player++) {
        if (Sel_Arts_Complete[player] != 0 || My_char[player] != characters[player]) {
            continue;
        }

        Slide_Type = player;
        Sel_Arts_Complete[player] = 1;
        Last_Super_Arts[player] = Arts_Y[player];
        Super_Arts[player] = Arts_Y[player];
        if (Used_char[player] != My_char[player]) {
            Last_Player_id = player;
        }
        Used_char[player] = My_char[player];
        Setup_ID();
    }
}

static void apply_repeat_super_preset_defaults(Character player1_character, Sint8 player1_super_art) {
    characters[0] = player1_character;
    characters[1] = CHAR_RYU;
    selected_super_arts[0] = player1_super_art;
    selected_super_arts[1] = 0;
    stage = scene_preset_stage_heavy_stage;
}

static void apply_scene_preset_defaults() {
    scene_preset = resolve_scene_preset(configuration.test.scene_preset);

    switch (scene_preset) {
    case TEST_SCENE_PRESET_STAGE_HEAVY:
        stage = scene_preset_stage_heavy_stage;
        break;

    case TEST_SCENE_PRESET_EFFECT_HEAVY:
        characters[0] = CHAR_RYU;
        characters[1] = CHAR_KEN;
        selected_super_arts[0] = 0;
        selected_super_arts[1] = 0;
        stage = scene_preset_stage_heavy_stage;
        break;

    case TEST_SCENE_PRESET_SUPER_HEAVY:
        characters[0] = CHAR_RYU;
        characters[1] = CHAR_RYU;
        selected_super_arts[0] = 0;
        selected_super_arts[1] = 0;
        stage = scene_preset_stage_heavy_stage;
        break;

    case TEST_SCENE_PRESET_YUN_SA3_REPEAT:
    case TEST_SCENE_PRESET_YUN_SA3_REPEAT_PRESSURE:
        apply_repeat_super_preset_defaults(CHAR_YUN, 2);
        break;

    case TEST_SCENE_PRESET_Q_SA1_REPEAT:
    case TEST_SCENE_PRESET_Q_SA1_REPEAT_PRESSURE:
        apply_repeat_super_preset_defaults(CHAR_Q, 0);
        break;

    case TEST_SCENE_PRESET_KEN_SA3_REPEAT:
    case TEST_SCENE_PRESET_KEN_SA3_REPEAT_PRESSURE:
        apply_repeat_super_preset_defaults(CHAR_KEN, 2);
        break;

    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT:
    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT_PRESSURE:
        apply_repeat_super_preset_defaults(CHAR_CHUNLI, 1);
        break;

    case TEST_SCENE_PRESET_BASIC_EXCHANGE:
        characters[0] = CHAR_RYU;
        characters[1] = CHAR_KEN;
        selected_super_arts[0] = 0;
        selected_super_arts[1] = 0;
        stage = scene_preset_basic_exchange_stage;
        break;

    case TEST_SCENE_PRESET_PRESSURE_EXCHANGE:
        characters[0] = CHAR_RYU;
        characters[1] = CHAR_KEN;
        selected_super_arts[0] = 0;
        selected_super_arts[1] = 0;
        stage = scene_preset_stage_heavy_stage;
        break;

    case TEST_SCENE_PRESET_LEFT_CORNER_RYU_STAGE:
        characters[0] = CHAR_RYU;
        characters[1] = CHAR_KEN;
        selected_super_arts[0] = 0;
        selected_super_arts[1] = 0;
        stage = scene_preset_basic_exchange_stage;
        break;

    case TEST_SCENE_PRESET_TRAINING_YUN_RYU_RYU_STAGE:
        characters[0] = CHAR_YUN;
        characters[1] = CHAR_RYU;
        selected_super_arts[0] = 2;
        selected_super_arts[1] = 0;
        stage = scene_preset_training_yun_ryu_ryu_stage;
        break;

    case TEST_SCENE_PRESET_TRAINING_FRAME_DATA:
        /* H2 (docs/plan-frame-data-harness.md section 1.4): reuses the
         * training-yun-ryu-ryu-stage boot machinery. Defaults P1=Q,
         * P2=Ken; --test-p1-character/--test-p2-character/--test-stage
         * override these below in initialize_default_data() (same
         * generic override path every other preset already goes
         * through). */
        characters[0] = CHAR_Q;
        characters[1] = CHAR_KEN;
        selected_super_arts[0] = 0;
        selected_super_arts[1] = 0;
        stage = scene_preset_training_yun_ryu_ryu_stage;
        break;

    case TEST_SCENE_PRESET_NONE:
        break;
    }
}

static void apply_scene_preset_inputs() {
    if (scene_preset == TEST_SCENE_PRESET_NONE || scene_preset == TEST_SCENE_PRESET_STAGE_HEAVY ||
        scene_preset == TEST_SCENE_PRESET_TRAINING_FRAME_DATA) {
        /* training-frame-data has no built-in scripted input pattern of
         * its own (unlike training-yun-ryu-ryu-stage below) - it exists
         * to be driven by --test-input-script. This branch only matters
         * if the preset is selected without a script, in which case a
         * no-op is the correct fallback (this function isn't even
         * reached when an input script is loaded - see PHASE_GAME in
         * TestRunner_Prologue). */
        p1sw_buff = 0;
        p2sw_buff = 0;
        return;
    }

    if (scene_preset == TEST_SCENE_PRESET_EFFECT_HEAVY) {
        const int cycle_length = 48;
        p1sw_buff = projectile_script_input(0, game_frame % cycle_length);
        p2sw_buff = projectile_script_input(1, (game_frame + 24) % cycle_length);
        return;
    }

    if (scene_preset_uses_repeat_second_super() && !scene_preset_uses_repeat_pressure()) {
        p1sw_buff = 0;
        p2sw_buff = 0;

        if (scene_preset_repeat_super_input_active(scene_preset_repeat_first_super_frame)) {
            p1sw_buff = super_script_input_with_button(
                0, game_frame - scene_preset_repeat_first_super_frame, scene_preset_repeat_super_attack_button());
        }

        if (scene_preset_repeat_super_input_active(scene_preset_repeat_second_super_frame)) {
            p1sw_buff = super_script_input_with_button(
                0, game_frame - scene_preset_repeat_second_super_frame, scene_preset_repeat_super_attack_button());
        }
        return;
    }

    if (scene_preset_uses_repeat_pressure()) {
        p1sw_buff = pressure_exchange_script_input(0, game_frame);
        p2sw_buff = pressure_exchange_script_input(1, game_frame);

        if (scene_preset_repeat_super_prep_active(scene_preset_repeat_first_super_frame)) {
            p1sw_buff = 0;
            p2sw_buff = pressure_exchange_defend_input(
                1, game_frame - (scene_preset_repeat_first_super_frame - scene_preset_repeat_super_prep_frames));
            return;
        }

        if (scene_preset_repeat_super_input_active(scene_preset_repeat_first_super_frame)) {
            p1sw_buff = super_script_input_with_button(
                0, game_frame - scene_preset_repeat_first_super_frame, scene_preset_repeat_super_attack_button());
            p2sw_buff = pressure_exchange_defend_input(
                1, game_frame - (scene_preset_repeat_first_super_frame - scene_preset_repeat_super_prep_frames));
            return;
        }

        if (scene_preset_repeat_super_prep_active(scene_preset_repeat_second_super_frame)) {
            p1sw_buff = 0;
            p2sw_buff = pressure_exchange_defend_input(
                1, game_frame - (scene_preset_repeat_second_super_frame - scene_preset_repeat_super_prep_frames));
            return;
        }

        if (scene_preset_repeat_super_input_active(scene_preset_repeat_second_super_frame)) {
            p1sw_buff = super_script_input_with_button(
                0, game_frame - scene_preset_repeat_second_super_frame, scene_preset_repeat_super_attack_button());
            p2sw_buff = pressure_exchange_defend_input(
                1, game_frame - (scene_preset_repeat_second_super_frame - scene_preset_repeat_super_prep_frames));
            return;
        }

        return;
    }

    if (scene_preset == TEST_SCENE_PRESET_BASIC_EXCHANGE) {
        p1sw_buff = basic_exchange_script_input(0, game_frame);
        p2sw_buff = basic_exchange_script_input(1, game_frame);
        return;
    }

    if (scene_preset == TEST_SCENE_PRESET_PRESSURE_EXCHANGE) {
        p1sw_buff = pressure_exchange_script_input(0, game_frame);
        p2sw_buff = pressure_exchange_script_input(1, game_frame);
        return;
    }

    if (scene_preset == TEST_SCENE_PRESET_LEFT_CORNER_RYU_STAGE) {
        p1sw_buff = left_corner_ryu_stage_script_input(0, game_frame);
        p2sw_buff = left_corner_ryu_stage_script_input(1, game_frame);
        return;
    }

    if (scene_preset == TEST_SCENE_PRESET_TRAINING_YUN_RYU_RYU_STAGE) {
        p1sw_buff = training_yun_ryu_ryu_stage_script_input(0, game_frame);
        p2sw_buff = training_yun_ryu_ryu_stage_script_input(1, game_frame);
        return;
    }

    p1sw_buff = projectile_script_input(0, (game_frame + 12) % 56);
    p2sw_buff = projectile_script_input(1, (game_frame + 36) % 56);

    if (game_frame >= 150 && game_frame < 190) {
        p1sw_buff = super_script_input(0, game_frame - 150);
    }

    if (game_frame >= 240 && game_frame < 280) {
        p2sw_buff = super_script_input(1, game_frame - 240);
    }
}

static void apply_initial_super_full_overrides() {
    if (game_frame > 2) {
        return;
    }

    if (!configuration.test.initial_super_full) {
        return;
    }

    if (plw[0].sa != NULL && plw[0].sa->store > 0) {
        return;
    }

    if (My_char[0] >= 0 && Super_Arts[0] >= 0) {
        tr_spgauge_cont_init2(0);
    }
}

/* EX/Supers program Step 1 (exsuper plan, G1/G2), revised per the STOP
 * report (<sp>/exsuper/step1-report.md): pins the training-mode S.A.GAUGE
 * menu option every frame, mirroring TestRunner_Epilogue's
 * Training[0/2].contents[0][1][6] "FRAME DATA" pin below. Two independent
 * hazards make a one-shot menu-cell write unsafe: (1) menu.c:4197
 * (`Training[0] = Training[2]`) copies over Training[0] on training-mode
 * menu entry, which can clobber a Training[0]-only poke landing before that
 * copy — writing both slots every frame survives it regardless of
 * ordering; (2) effect_E3_move's routine 0 (effe3.c:28-32) consumes the
 * init_E3_flag that effect_E3_init arms at round setup (plcnt.c:1326-1331),
 * on its own schedule before this override gets a turn on the very first
 * latch, so the flag needs at least one re-arm to land the poke at all.
 *
 * What changed from the first attempt: holding init_E3_flag = 1 EVERY
 * frame (forever, not just until the first correct latch) turned out to be
 * destructive. effect_E3_move is a 2-state loop (effe3.c:20-142): routine 0
 * consumes the flag and recomputes spmv_ng_flag2 from the pinned menu cell
 * (correct), falls through into routine 1 the same tick (flag now 0) — but
 * held-every-frame re-armed it again before the *next* tick, so routine 1's
 * own check (effe3.c:137-141: "if init_E3_flag==1, restore
 * spmv_ng_flag/2 from the E3-init-time backup, loop back to 0") fired on
 * that next tick and reverted the correct value it had just computed. This
 * repeated every 2 frames forever, and the diagnostic print below caught it
 * directly: spmv_ng_flag2 oscillated between the correct freshly-latched
 * pattern and the stale pre-latch backup pattern for the entire run,
 * suppressing super recognition regardless of stock level.
 *
 * Fix: make the re-arm EDGE-TRIGGERED. Each frame, compare the *observed*
 * spmv_ng_flag2 against the pattern effe3.c's own switch on
 * Training[0].contents[0][1][0] guarantees once latched (only the bits that
 * switch actually clears/sets for the pinned gauge value — see
 * training_sa_gauge_expected_flag2() below). Only set init_E3_flag = 1 when
 * that comparison mismatches. Once the first re-arm lands the correct
 * latch, the comparison matches on the very next frame and this stops
 * re-arming — init_E3_flag stays at the 0 that case 0 already left it at,
 * so routine 1's restore-from-backup branch never fires again, and the
 * latch holds stable. If something later actually changes the observed
 * state away from the expected pattern (e.g. the menu.c:4197 copy landing
 * with a stale Training[2] on some future entry), the mismatch reappears
 * and this re-arms again — bounded, not open-loop. Unset (-1, the default)
 * is still a hard no-op: zero gameplay behavior when the flag is not
 * passed. */
static uint32_t training_sa_gauge_expected_flag2(int gauge, uint32_t* mask_out) {
    /* Mirrors the clear-mask/set-mask pairs from effe3.c:95-123 for each
     * Training[0].contents[0][1][0] case. known_mask is the set of bits
     * that case's switch arm deterministically pins (cleared bits ->
     * expected 0, OR'd bits -> expected 1); bits the switch arm never
     * touches are left out of the mask (don't-care), matching each case's
     * actual clear+OR pattern rather than assuming a full overwrite. */
    switch (gauge) {
    case 0: /* NORMAL: |= 0xD0000, no clear */
        *mask_out = 0xD0000;
        return 0xD0000;
    case 1: /* MAX START: &= 0xFFFBFFFF, |= 0x90000 */
        *mask_out = 0xD0000;
        return 0x90000;
    case 2: /* INFINITY: &= 0xFFFEFFFF, |= 0xC0000 | DIP2_SA_GAUGE_NO_DEPLETE */
        *mask_out = 0xD0000 | DIP2_SA_GAUGE_NO_DEPLETE;
        return 0xC0000 | DIP2_SA_GAUGE_NO_DEPLETE;
    case 3: /* MAXIMUM: &= 0xFFF7FFFF, |= 0x50000 */
        *mask_out = 0xD0000;
        return 0x50000;
    default:
        *mask_out = 0;
        return 0;
    }
}

static void apply_training_sa_gauge_overrides() {
    if (configuration.test.training_sa_gauge < 0) {
        return;
    }

    Training[0].contents[0][1][0] = configuration.test.training_sa_gauge;
    Training[2].contents[0][1][0] = configuration.test.training_sa_gauge;

    uint32_t mask = 0;
    uint32_t expected = training_sa_gauge_expected_flag2(configuration.test.training_sa_gauge, &mask);

    if (mask != 0 && (plw[0].spmv_ng_flag2 & mask) != expected) {
        plw[0].init_E3_flag = 1;
    }
    if (mask != 0 && (plw[1].spmv_ng_flag2 & mask) != expected) {
        plw[1].init_E3_flag = 1;
    }
}

static void apply_scene_preset_super_refill_overrides() {
    if (!scene_preset_uses_repeat_second_super() || scene_preset_repeat_second_super_ready) {
        return;
    }

    if (game_frame < scene_preset_repeat_second_super_refill_frame || plw[0].sa == NULL || player_super_art_active(0)) {
        return;
    }

    if (plw[0].sa->store == 0 && My_char[0] >= 0 && Super_Arts[0] >= 0) {
        tr_spgauge_cont_init2(0);
    }
    scene_preset_repeat_second_super_ready = true;
}

static void initialize_default_data() {
    characters[0] = CHAR_RYU;
    characters[1] = CHAR_KEN;
    selected_super_arts[0] = 0;
    selected_super_arts[1] = 0;
    stage = -1;
    scene_preset = TEST_SCENE_PRESET_NONE;
    Debug_w[DEBUG_STAGE_SELECT] = 0;
    game_frame = 0;
    player_super_art_activation_starts[0] = 0;
    player_super_art_activation_starts[1] = 0;
    player_super_art_was_active[0] = false;
    player_super_art_was_active[1] = false;
    scene_preset_repeat_second_super_ready = false;

    apply_scene_preset_defaults();

    for (int player = 0; player < 2; player++) {
        if (configuration.test.characters[player] >= 0) {
            characters[player] = (Sint8)configuration.test.characters[player];
        }

        if (configuration.test.super_arts[player] >= 0) {
            selected_super_arts[player] = (Sint8)configuration.test.super_arts[player];
        }
    }

    if (configuration.test.stage >= 0) {
        stage = (Sint8)configuration.test.stage;
    }

    inputs_index = 0;
    inputs_total = 0;
}

static void apply_stage_override() {
    if (stage >= 0) {
        Debug_w[DEBUG_STAGE_SELECT] = stage + 1;
        VS_Stage = stage;
    }
}

static void force_stage_transition_override() {
    if (stage < 0) {
        return;
    }

    apply_stage_override();
    if (Battle_Country != stage || bg_w.stage != stage || bg_w.area != 0) {
        Battle_Country = stage;
        bg_w.stage = stage;
        bg_w.area = 0;
        Push_LDREQ_Queue_BG(stage);
    } else {
        Battle_Country = stage;
    }
}

static void set_cursor(Character character, int player) {
    Cursor_X[player] = character_to_cursor[character][0];
    Cursor_Y[player] = character_to_cursor[character][1];
}

/// Repeatedly press and release a button
static void mash_button(SWKey button, int player) {
    u16* dst = player ? &p2sw_buff : &p1sw_buff;
    *dst |= (frame & 1) ? button : 0;
}

static void tap_button(SWKey button, int player) {
    u16* dst = player ? &p2sw_buff : &p1sw_buff;
    *dst |= button;
}

/* read_u16 definition lives in test_runner_utils.c; included via the
 * header. The local duplicate used SDL_ReadIO+SWAP16, utils uses
 * SDL_ReadU16BE — semantically identical on our targets. */

static u16 read_input_buff(SDL_IOStream* io, Sint64 offset) {
    const u16 raw_buff = read_u16(io, offset);
    u16 buff = 0;

    buff |= raw_buff & 0xF;              // directions
    buff |= raw_buff & (1 << 4);         // LP
    buff |= raw_buff & (1 << 5);         // MP
    buff |= raw_buff & (1 << 6);         // HP
    buff |= (raw_buff & (1 << 7)) << 1;  // LK
    buff |= (raw_buff & (1 << 8)) << 1;  // MK
    buff |= (raw_buff & (1 << 9)) << 1;  // HK
    buff |= (raw_buff & (1 << 12)) << 2; // start

    return buff;
}

static void initialize_data() {
    initialize_default_data();

    const char* input_script_path = configuration.test.input_script_path;
    if (input_script_path != NULL && input_script_path[0] != '\0') {
        InputScript_Load(input_script_path);
        InputScript_RequireTrainingModePreset(scene_preset_uses_training_mode());
    }

    const char* base_path = configuration.test.states_path;

    if (base_path == NULL || base_path[0] == '\0') {
        return;
    }

    const size_t base_len = SDL_strlen(base_path);
    const size_t path_max_len = SDL_strlen(base_path) + 64;
    char* path = SDL_malloc(path_max_len);
    SDL_strlcpy(path, base_path, path_max_len);
    char filename[64];
    bool in_game_prev = false;
    bool did_set_char_data = false;

    for (int frame_num = 0;; frame_num++) {
        bool stop = false;
        path[base_len] = '\0';
        SDL_snprintf(filename, sizeof(filename), "/frame_%08d.ram", frame_num);
        SDL_strlcat(path, filename, path_max_len);
        SDL_IOStream* io = SDL_IOFromFile(path, "rb");

        if (io == NULL) {
            break;
        }

        const u16 routine = read_u16(io, GAME_ROUTINE_OFFSET);
        const bool in_game = (routine == 2);

        // Read character and SA indices until we get to game.
        // This ensures we read the latest data

        if (in_game && !did_set_char_data) {
            SDL_SeekIO(io, MY_CHAR_OFFSET, SDL_IO_SEEK_SET);
            SDL_ReadIO(io, characters, 2);

            SDL_SeekIO(io, SUPER_ARTS_OFFSET, SDL_IO_SEEK_SET);
            SDL_ReadIO(io, selected_super_arts, 2);

            did_set_char_data = true;
        }

        // Parse inputs

        if (in_game) {
            inputs[inputs_index][0] = read_input_buff(io, P1SW_OFFSET);
            inputs[inputs_index][1] = read_input_buff(io, P2SW_OFFSET);
            inputs_index += 1;
        } else if (in_game_prev) {
            stop = true;
        }

        in_game_prev = in_game;
        SDL_CloseIO(io);

        if (stop) {
            break;
        }
    }

    SDL_free(path);

    // There's no Shin Akuma in PS2 version, which is why we have to decrement character numbers after Akuma
    for (int i = 0; i < 2; i++) {
        if (characters[i] > CHAR_AKUMA) {
            characters[i] -= 1;
        }
    }

    inputs_total = inputs_index;
    inputs_index = 0;
}

/* ====================================================================== *
 * Step B3 EXPERIMENT (docs/plan-fcade-replay-browser.md): raw decoded
 * Fightcade -13 stream playback from the engine's own cold boot.
 *
 * The stream file is decode_inputs.py's --bin output: frame_count x
 * {u16 p1, u16 p2} little-endian words in ARCADE-RAM layout (dirs bits
 * 0-3, punches 4-6, KICKS 7-9, START 12). The engine's pre-latch buffers
 * (p1sw_buff/p2sw_buff) consume SWK layout (kicks 8-10, start 14), so
 * every injected word goes through fcade_arcade_to_swk() below — the
 * same shift set as replay_game.c:12-26 / plan §2.3. Feeding the arcade
 * words verbatim would silently corrupt kicks and start.
 *
 * This mode bypasses the whole DEBUG phase machine: no menu mash, no
 * cursor forcing, no stage override — the stream (plus the optional
 * deterministic START taps, see configuration.h) is the only input
 * source. Per-frame state samples are printed from the epilogue for
 * offline comparison against SCRD ground truth.
 * ====================================================================== */

static u16 (*fcade_stream)[2] = NULL; /* [frame][player], SWK layout after load-convert */
static int fcade_stream_total = 0;
static int fcade_app_frame = 0;
static bool fcade_load_attempted = false;
static bool fcade_game_reanchored = false;
static int fcade_game_reanchor_frame = 0;
static bool fcade_rng_seeded = false;

static bool fcade_mode_active(void) {
    return configuration.test.fcade_inputs_path != NULL && configuration.test.fcade_inputs_path[0] != '\0';
}

/* Arcade-RAM layout -> engine SWK layout (plan §2.3; same shifts as
 * replay_game.c:12-26): dirs/punches pass through, LK 7->8, MK 8->9,
 * HK 9->10, start 12->14. */
static u16 fcade_arcade_to_swk(u16 arcade) {
    u16 buff = 0;

    buff |= arcade & 0xF;                // directions
    buff |= arcade & (1 << 4);           // LP
    buff |= arcade & (1 << 5);           // MP
    buff |= arcade & (1 << 6);           // HP
    buff |= (u16)((arcade & (1 << 7)) << 1);  // LK
    buff |= (u16)((arcade & (1 << 8)) << 1);  // MK
    buff |= (u16)((arcade & (1 << 9)) << 1);  // HK
    buff |= (u16)((arcade & (1 << 12)) << 2); // start

    return buff;
}

void TestRunner_PinFcadeConfig(void) {
    /* Same idea as StatcheckRunner_PinConfig (statcheck_runner.c), but for
     * the ARCADE flow the -13 stream was recorded against: the Fightcade
     * session ran the arcade program (attract -> char select), not the
     * console menu chain, and was captured against sfiii3nr1's
     * arcade-balance data tables. Both pins are session-only. */
    SDLApp_ForceArcadeGameMode();
    /* Upstream reconcile: the boolean "arcade-balance" key is gone; balance
     * auto-selects at boot and CFG_KEY_BALANCE only forces it DOWN to PS2.
     * Clearing to "auto" is the strongest arcade-ward pin now available.
     *
     * KNOWN LIMIT (B3 raw-stream path only): ArcadeBalance_Init pins PS2
     * whenever configuration.test.enabled is set, and this pin fires only on
     * a --test-enable launch — so under the current model the B3 experiment
     * always resolves PS2 balance and cannot reproduce an arcade-balance
     * capture. Recorded rather than "fixed" here because the test-runner PS2
     * pin is what keeps the 94 frame-data corpora machine-independent; any
     * change to it belongs in the descope discussion, not in this merge. */
    Config_SetString(CFG_KEY_BALANCE, "auto");
    SDL_Log("fcade-b3: pinned game-mode=arcade + balance=auto for raw stream playback");
}

static void fcade_load_stream(void) {
    fcade_load_attempted = true;

    size_t size = 0;
    void* data = SDL_LoadFile(configuration.test.fcade_inputs_path, &size);

    if (data == NULL || size == 0 || (size % 4) != 0) {
        SDL_Log("fcade-b3: failed to load stream '%s' (size=%zu, need nonzero multiple of 4): %s",
                configuration.test.fcade_inputs_path,
                size,
                SDL_GetError());
        SDL_free(data);
        exit(2);
    }

    fcade_stream_total = (int)(size / 4);
    fcade_stream = SDL_malloc((size_t)fcade_stream_total * sizeof(*fcade_stream));

    const Uint8* bytes = data;
    for (int i = 0; i < fcade_stream_total; i++) {
        const u16 p1_arcade = (u16)(bytes[i * 4 + 0] | (bytes[i * 4 + 1] << 8));
        const u16 p2_arcade = (u16)(bytes[i * 4 + 2] | (bytes[i * 4 + 3] << 8));
        fcade_stream[i][0] = fcade_arcade_to_swk(p1_arcade);
        fcade_stream[i][1] = fcade_arcade_to_swk(p2_arcade);
    }

    SDL_free(data);

    printf("fcade-b3: loaded %d stream frames from %s (offset=%d anchor=%d p1start=%d p2start=%d max=%d)\n",
           fcade_stream_total,
           configuration.test.fcade_inputs_path,
           configuration.test.fcade_offset,
           configuration.test.fcade_anchor,
           configuration.test.fcade_p1_start_frame,
           configuration.test.fcade_p2_start_frame,
           configuration.test.fcade_max_frames);
    fflush(stdout);
}

static bool fcade_start_tap_active(int tap_frame) {
    /* 2-frame press so the ~p*sw_1 & p*sw_0 rising-edge checks (Ck_Coin,
     * Entry_01) see a clean edge regardless of latch ordering. */
    return tap_frame >= 0 && fcade_app_frame >= tap_frame && fcade_app_frame < tap_frame + 2;
}

/* Stream cursor for the current app frame, honoring the optional
 * round-start re-anchor (candidate rule from plan Step B3). Returns -1
 * when no stream frame should be fed yet. */
static int fcade_stream_index(void) {
    if (fcade_game_reanchored) {
        /* The prologue that first observes the round-start init state
         * (G_No[1]==2 && G_No[2]==3, latched at the end of the archive's
         * frame-1-equivalent engine frame) runs at the engine frame
         * corresponding to archive frame 2, whose input word per the B1
         * alignment is stream[game_offset + 2]. */
        return configuration.test.fcade_game_offset + 2 + (fcade_app_frame - fcade_game_reanchor_frame);
    }

    if (fcade_app_frame < configuration.test.fcade_anchor) {
        return -1;
    }

    return configuration.test.fcade_offset + (fcade_app_frame - configuration.test.fcade_anchor);
}

/* Validation-gate setup injection (see configuration.h): force the
 * SCRD-recorded characters / super arts / colors onto the engine's own
 * arcade char-select outcome every pre-round frame, so the stream-driven
 * cursor nav's landing cell stops mattering (the 7287 Yang-instead-of-Ken
 * class of failure). New_Challenger/Champion and the one-shot RNG seed are
 * applied once the game phase (G_No[1]==2) is entered — the same timing the
 * shipped console player uses (replay_player.c PHASE_CHARACTER_SELECT
 * case 1 / PHASE_GAME_TRANSITION). Inert unless the flags are set. */
static void fcade_force_setup(void) {
    if (fcade_game_reanchored) {
        return; /* fight is live; setup vars are engine-owned from here */
    }

    /* G_No[0]==2 gates the whole game program (char select G=[2,1,2],
     * VS/loading G=[2,2,x]); the attract TITLE also has G_No[1]==2 but
     * with G_No[0]==1 (b3-runA logs: G=[1,2,0,0] from f=454), so gating on
     * G_No[1] alone would force setup during attract — which breaks the
     * join path when New_Challenger=0 is forced (observed: 2133 g1/g2
     * P1 never joins, SIGSEGV at re-anchor into a 1P game). */
    const bool char_select_phase = G_No[0] == 2 && G_No[1] == 1;
    const bool game_phase = G_No[0] == 2 && G_No[1] == 2;

    if (!char_select_phase && !game_phase) {
        return;
    }

    if (configuration.test.fcade_p1_char >= 0) {
        My_char[0] = (u8)configuration.test.fcade_p1_char;
    }
    if (configuration.test.fcade_p2_char >= 0) {
        My_char[1] = (u8)configuration.test.fcade_p2_char;
    }
    if (configuration.test.fcade_p1_arts >= 0) {
        Super_Arts[0] = (s8)configuration.test.fcade_p1_arts;
    }
    if (configuration.test.fcade_p2_arts >= 0) {
        Super_Arts[1] = (s8)configuration.test.fcade_p2_arts;
    }
    if (configuration.test.fcade_p1_color >= 0) {
        Player_Color[0] = (s8)configuration.test.fcade_p1_color;
    }
    if (configuration.test.fcade_p2_color >= 0) {
        Player_Color[1] = (s8)configuration.test.fcade_p2_color;
    }

    if (game_phase) {
        if (configuration.test.fcade_new_challenger >= 0) {
            /* Same pairing the shipped player restores (replay_player.c:
             * "Restore the arcade invariant Champion == New_Challenger ^ 1"). */
            New_Challenger = (s8)configuration.test.fcade_new_challenger;
            Champion = New_Challenger ^ 1;
        }

        if (!fcade_rng_seeded && !configuration.test.fcade_seed_at_reanchor) {
            fcade_rng_seeded = true;

            if (configuration.test.fcade_seed_ix16 >= 0) {
                Random_ix16 = (s16)configuration.test.fcade_seed_ix16;
            }
            if (configuration.test.fcade_seed_ix32 >= 0) {
                Random_ix32 = (s16)configuration.test.fcade_seed_ix32;
            }

            if (configuration.test.fcade_seed_ix16 >= 0 || configuration.test.fcade_seed_ix32 >= 0) {
                printf("fcade-b3: RNG seeded at f=%d -> r16=%d r32=%d\n",
                       fcade_app_frame,
                       (int)Random_ix16,
                       (int)Random_ix32);
                fflush(stdout);
            }
        }
    }
}

static void fcade_prologue(void) {
    if (!fcade_load_attempted) {
        fcade_load_stream();
    }

    fcade_force_setup();

    if (configuration.test.fcade_game_offset >= 0 && !fcade_game_reanchored && G_No[1] == 2 && G_No[2] == 3) {
        fcade_game_reanchored = true;
        fcade_game_reanchor_frame = fcade_app_frame;
        printf("fcade-b3: round-start re-anchor at f=%d -> stream idx %d\n",
               fcade_app_frame,
               configuration.test.fcade_game_offset + 2);

        if (configuration.test.fcade_seed_at_reanchor && !fcade_rng_seeded) {
            fcade_rng_seeded = true;

            if (configuration.test.fcade_seed_ix16 >= 0) {
                Random_ix16 = (s16)configuration.test.fcade_seed_ix16;
            }
            if (configuration.test.fcade_seed_ix32 >= 0) {
                Random_ix32 = (s16)configuration.test.fcade_seed_ix32;
            }

            printf("fcade-b3: RNG seeded at re-anchor f=%d -> r16=%d r32=%d\n",
                   fcade_app_frame,
                   (int)Random_ix16,
                   (int)Random_ix32);
        }
        fflush(stdout);
    }

    u16 p1 = 0;
    u16 p2 = 0;

    {
        const int idx = fcade_stream_index();

        if (idx >= 0 && idx < fcade_stream_total) {
            p1 = fcade_stream[idx][0];
            p2 = fcade_stream[idx][1];
        }
    }

    if (fcade_start_tap_active(configuration.test.fcade_p1_start_frame)) {
        p1 |= SWK_START;
    }

    if (fcade_start_tap_active(configuration.test.fcade_p2_start_frame)) {
        p2 |= SWK_START;
    }

    p1sw_buff = p1;
    p2sw_buff = p2;
}

static void fcade_epilogue(void) {
    const int idx = fcade_stream_index();

    /* Dizzy (kizetsu/piyori) observability for the validation gate: py->flag
     * flips to 1 when the stun gauge maxes (plpdm.c:1490/1566) and
     * Damage_25000 then rolls the random dizzy duration into py->time
     * (plpdm.c:893, the confirmed gameplay random_16() consumer). Printing
     * both per frame lets the offline compare detect whether a game
     * contained a dizzy at all and how long it lasted. */
    printf("fcade-b3 f=%d idx=%d inj=[%04x,%04x] G=[%u,%u,%u,%u] C=[%u,%u,%u,%u] mode=%d play=%u demo=%d "
           "op=[%d,%d] gtimer=%u rtimer=%d allow=%u mychar=[%d,%d] arts=[%d,%d] cur=[%d,%d|%d,%d] "
           "sel=[%d,%d] r16=%d r32=%d pos=[%d,%d] vit=[%d,%d] piyo=[%d/%d,%d/%d]\n",
           fcade_app_frame,
           (idx >= 0 && idx < fcade_stream_total) ? idx : -1,
           p1sw_0,
           p2sw_0,
           G_No[0],
           G_No[1],
           G_No[2],
           G_No[3],
           C_No[0],
           C_No[1],
           C_No[2],
           C_No[3],
           (int)Mode_Type,
           Play_Mode,
           (int)Demo_Flag,
           (int)Operator_Status[0],
           (int)Operator_Status[1],
           Game_timer,
           (int)round_timer,
           Allow_a_battle_f,
           (int)My_char[0],
           (int)My_char[1],
           (int)Super_Arts[0],
           (int)Super_Arts[1],
           (int)Cursor_X[0],
           (int)Cursor_Y[0],
           (int)Cursor_X[1],
           (int)Cursor_Y[1],
           (int)Sel_Arts_Complete[0],
           (int)Sel_Arts_Complete[1],
           (int)Random_ix16,
           (int)Random_ix32,
           (int)plw[0].wu.xyz[0].disp.pos,
           (int)plw[1].wu.xyz[0].disp.pos,
           (int)plw[0].wu.vital_new,
           (int)plw[1].wu.vital_new,
           plw[0].py != NULL ? (int)plw[0].py->flag : -1,
           plw[0].py != NULL ? (int)plw[0].py->time : -1,
           plw[1].py != NULL ? (int)plw[1].py->flag : -1,
           plw[1].py != NULL ? (int)plw[1].py->time : -1);
    fflush(stdout);

    fcade_app_frame += 1;

    if (configuration.test.fcade_max_frames > 0 && fcade_app_frame >= configuration.test.fcade_max_frames) {
        printf("fcade-b3: reached max frames (%d), exiting\n", configuration.test.fcade_max_frames);
        fflush(stdout);
        exit(0);
    }
}

static void update_player_super_art_activation_state(void) {
    for (int player = 0; player < 2; player++) {
        const bool is_active = player_super_art_active(player);
        if (is_active && !player_super_art_was_active[player]) {
            player_super_art_activation_starts[player] += 1;
        }
        player_super_art_was_active[player] = is_active;
    }
}

void TestRunner_Prologue() {
    p1sw_buff = 0;
    p2sw_buff = 0;

    /* SPIKE (--test-instant-jump): the direct scene-jump prototype owns
     * the whole session in place of the phase machine. */
    if (SceneJumpSpike_Active()) {
        SceneJumpSpike_Tick();
        return;
    }

    /* Step B3 EXPERIMENT: raw fcade-stream playback replaces the whole
     * phase machine — the stream is the only input source. */
    if (fcade_mode_active()) {
        fcade_prologue();
        return;
    }

    switch (phase) {
    case PHASE_INIT:
        initialize_data();
        phase = PHASE_TITLE;
        // fallthrough

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
        apply_stage_override();

        if (G_No[1] == 1 && G_No[2] == 2 && (!scene_preset_uses_training_mode() || Mode_Type == MODE_NORMAL_TRAINING)) {
            Last_My_char2[0] = characters[0];
            Last_My_char2[1] = characters[1];
            Last_Super_Arts[0] = selected_super_arts[0];
            Last_Super_Arts[1] = selected_super_arts[1];
            phase = PHASE_CHARACTER_SELECT_TRANSITION;
            wait_timer = 60;
            break;
        }

        if (scene_preset_uses_training_mode()) {
            const struct _TASK* menu_task = &task[TASK_MENU];

            if (menu_task->r_no[1] == 1 && menu_task->r_no[2] == 3) {
                Menu_Cursor_Y[0] = 2;
                mash_button(SWK_SOUTH, 0);
                break;
            }

            if (menu_task->r_no[1] == 4 && menu_task->r_no[2] == 3) {
                Menu_Cursor_Y[0] = 0;
                mash_button(SWK_SOUTH, 0);
                break;
            }
        }

        mash_button(SWK_SOUTH, 0);
        break;

    case PHASE_CHARACTER_SELECT_TRANSITION:
        apply_stage_override();
        wait_timer -= 1;

        if (wait_timer <= 0) {
            phase = PHASE_CHARACTER_SELECT;
            /* Task #108: seed the select dwell exactly once, on entry. */
            if (select_dwell_remaining < 0) {
                select_dwell_remaining = configuration.test.select_dwell_frames;
            }
        }

        break;

    case PHASE_CHARACTER_SELECT:
        apply_stage_override();
        /* Task #108: dwell on select before touching a cursor. The runner
         * otherwise clears this screen in a handful of frames, which is short
         * of UNIT_OF_TIMER_MAX (50, src/constants.h:6) - one Select_Timer
         * decrement - so effect_A5_move never reaches its tick even on a
         * non-training preset where Present_Mode does not early-return it
         * (effa5.c:49-51). Inputs are left at neutral while dwelling, so the
         * screen simply sits there and the countdown runs. 0 (default) skips
         * this entirely and the phase behaves exactly as before. */
        if (select_dwell_remaining > 0) {
            select_dwell_remaining -= 1;
            break;
        }

        maybe_force_training_scene_character_and_super_state();
        switch (char_select_phase) {
        case 0:
            set_cursor(characters[0], 0);
            set_cursor(characters[1], 1);
            tap_button(SWK_START, 1);
            char_select_phase = 1;
            break;

        case 1:
            set_cursor(characters[0], 0);
            set_cursor(characters[1], 1);
            if (Select_Start[1] >= 2) {
                char_select_phase = 2;
            }

            break;

        case 2:
            set_cursor(characters[0], 0);
            set_cursor(characters[1], 1);

            if (My_char[0] != characters[0] || Sel_Arts_Complete[0] < 0) {
                mash_button(SWK_SOUTH, 0);
            }

            if (My_char[1] != characters[1] || Sel_Arts_Complete[1] < 0) {
                mash_button(SWK_SOUTH, 1);
            }

            if (My_char[0] == characters[0] && My_char[1] == characters[1] && Sel_Arts_Complete[0] >= 0 &&
                Sel_Arts_Complete[1] >= 0) {
                char_select_phase = 3;
            }
            break;

        case 3:
            maybe_force_training_scene_super_confirm();
            if (Sel_Arts_Complete[0] == 0) {
                mash_button(SWK_SOUTH, 0);
            }

            if (Sel_Arts_Complete[1] == 0) {
                mash_button(SWK_SOUTH, 1);
            }

            if (Sel_Arts_Complete[0] > 0 && Sel_Arts_Complete[1] > 0) {
                phase = PHASE_GAME_TRANSITION;
            }

            break;
        }

        break;

    case PHASE_GAME_TRANSITION:
        force_stage_transition_override();
        if (scene_preset_uses_training_mode()) {
            const struct _TASK* menu_task = &task[TASK_MENU];

            if (menu_task->r_no[0] == 7 && menu_task->r_no[1] == 1 && menu_task->r_no[2] == 1) {
                Menu_Cursor_Y[0] = 0;
                mash_button(SWK_SOUTH, 0);
                break;
            }

            if (training_mode_gameplay_started()) {
                phase = PHASE_GAME;
                game_frame = 0;
                break;
            }

            mash_button(SWK_ATTACKS, 0);
            break;
        } else if (G_No[1] == 2) {
            phase = PHASE_GAME;
            game_frame = 0;
        } else {
            if (!configuration.test.preserve_game_transition) {
                // This skips the VS animation on the default automation path.
                mash_button(SWK_ATTACKS, 0);
            }
            break;
        }

        // fallthrough

    case PHASE_GAME:
        if (configuration.test.delay_gameplay_inputs_until_active && !gameplay_input_active()) {
            p1sw_buff = 0;
            p2sw_buff = 0;
            break;
        }

        apply_initial_super_full_overrides();
        apply_scene_preset_super_refill_overrides();
        if (InputScript_IsLoaded()) {
            if (training_mode_gameplay_started()) {
                InputScript_Tick(&p1sw_buff, &p2sw_buff);
            } else {
                p1sw_buff = 0;
                p2sw_buff = 0;
            }
        } else if (inputs_index < inputs_total) {
            p1sw_buff = inputs[inputs_index][0];
            p2sw_buff = inputs[inputs_index][1];
            inputs_index += 1;
        } else if (scene_preset != TEST_SCENE_PRESET_NONE) {
            apply_scene_preset_inputs();
        } else {
            p1sw_buff = 0;
            p2sw_buff = 0;
        }
        break;
    }
}

void TestRunner_Epilogue() {
    /* SPIKE: the scene-jump prototype needs none of the pins below. */
    if (SceneJumpSpike_Active()) {
        return;
    }

    /* Step B3 EXPERIMENT: sample-and-count only; none of the training-mode
     * pins below apply to raw fcade-stream playback. */
    if (fcade_mode_active()) {
        fcade_epilogue();
        return;
    }

    frame += 1;
    update_player_super_art_activation_state();

    /* Phase 5 hygiene item 2/3 (docs/plan-frame-data-harness.md):
     * frame_trace_tick()/frame_data_overlay_tick() are now additionally
     * gated on Disp_Frame_Data, so the harness needs the training-menu
     * "FRAME DATA" option to actually be on for this run. Rather than
     * writing Disp_Frame_Data directly (it's a one-shot latch, not
     * re-derived per frame), pin the persisted config cell it's latched
     * from — Training[0/2].contents[0][1][6] — every frame, the same
     * pattern input_script_apply_guard_mode() already uses for the
     * dummy's guard/stance slots. Wait_Pause_in_Tr (menu.c) reads this
     * cell exactly once, at the menu->gameplay transition
     * (Allow_a_battle_f: 0->1), and assigns it to Disp_Frame_Data for
     * real, so this rides the production code path instead of fighting
     * it. Runs after njUserMain() (main.c's game_step_1, which calls
     * this) so it survives Default_Training_Data(0)'s one-time
     * contents-zeroing on the same frame training mode is entered, and
     * is in place well before Wait_Pause_in_Tr's one-shot read at round
     * start. */
    if (scene_preset == TEST_SCENE_PRESET_TRAINING_FRAME_DATA) {
        Training[0].contents[0][1][6] = 1;
        Training[2].contents[0][1][6] = 1;
    }

    apply_training_sa_gauge_overrides();

    if (phase == PHASE_GAME) {
        if (configuration.test.delay_gameplay_inputs_until_active && !gameplay_input_active()) {
            return;
        }
        game_frame += 1;
    }
}

#else /* !DEBUG — provide stubs for perf-telemetry references */

/* port/build_config.h must come FIRST. test_runner.h opens with
 * `#if DEBUG || ENABLE_PERF_TELEMETRY`; in this branch DEBUG is 0, and without
 * build_config.h ENABLE_PERF_TELEMETRY is undefined and also evaluates to 0
 * (no -Wundef in C_FLAGS), so the header body — the prototypes for the three
 * stubs below — silently disappears and the definitions compile unchecked
 * against their declarations. This is the RELEASE/telemetry branch, i.e. the
 * configuration this project actually ships. Same class as mtrans.c/pls03.c. */
#include "port/build_config.h"
#include "test/test_runner.h"
#include <stdbool.h>

bool TestRunner_IsSupportedPhaseName(const char* phase_name) {
    (void)phase_name;
    return false;
}

const char* TestRunner_GetPhaseName(void) {
    return "none";
}

bool TestRunner_IsPhaseActive(const char* phase_name) {
    (void)phase_name;
    return false;
}

#endif
