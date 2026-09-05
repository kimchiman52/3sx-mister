#if defined(STATCHECK)

#include "test/scrd_game.h"
#include "arcade/arcade_constants.h"
#include "constants.h"
#include "test/ram_archive.h"

#include <SDL3/SDL.h>

// Self-contained big-endian u16 reader. Upstream's replay_game.c pulls this
// from test/test_runner_utils.c, but the fork's copy of that file is gated
// `#if DEBUG` (never compiled in a STATCHECK build), so we inline it here to
// keep the STATCHECK translation unit independent of the DEBUG harness.
static Uint16 scrd_read_u16(SDL_IOStream* io, Sint64 offset) {
    Uint16 result = 0;
    SDL_SeekIO(io, offset, SDL_IO_SEEK_SET);
    SDL_ReadU16BE(io, &result);
    return result;
}

static void scrd_adjust_character_numbers(ScrdGame* game) {
    for (int i = 0; i < 2; i++) {
        game->characters[i] = CHAR_ARCADE_TO_3SX(game->characters[i]);
    }
}

static void scrd_read_match_setup(ScrdGame* game, SDL_IOStream* io) {
    SDL_SeekIO(io, MY_CHAR_OFFSET, SDL_IO_SEEK_SET);
    SDL_ReadIO(io, game->characters, 2);

    SDL_SeekIO(io, SUPER_ARTS_OFFSET, SDL_IO_SEEK_SET);
    SDL_ReadIO(io, game->supers, 2);

    SDL_SeekIO(io, NEW_CHALLENGER_OFFSET, SDL_IO_SEEK_SET);
    SDL_ReadU8(io, &game->new_challenger);

    SDL_SeekIO(io, PLAYER_COLOR_OFFSET, SDL_IO_SEEK_SET);
    SDL_ReadIO(io, game->colors, 2);

    /* Stage (H2, docs/research-arcade-balance-desyncs.md). It is NOT
     * reconstructible from the character select: on the arcade the stage
     * carries across matches, and `appear_data_init_set()` (`appear.c`) indexes
     * `app_type_tbl[own][opp][bg_w.stage]` to pick both `wu.routine_no[4]` and
     * `wu.xyz[0].disp.pos`, so getting it wrong moves a player at battle start.
     *
     * CHAR_ARCADE_TO_3SX is the right transform, not a coincidence: both sides
     * derive the home stage from a character id in their own index space.
     * `Setup_Battle_Country()` (`sel_pl.c`) returns `My_char[...]` verbatim, and
     * on the arcade side the byte at BG_W_STAGE_OFFSET equals one of the two
     * players' arcade character ids in all 11 measured segments. The port drops
     * arcade index 15 (CHAR_SHIN_AKUMA) from both spaces -- which is exactly why
     * `app_type_tbl` is [20][20][22] here against the arcade's [21][21][23]. */
    Uint8 stage = 0;
    SDL_SeekIO(io, BG_W_STAGE_OFFSET, SDL_IO_SEEK_SET);
    SDL_ReadU8(io, &stage);
    game->stage = (Uint8)CHAR_ARCADE_TO_3SX(stage);

    scrd_adjust_character_numbers(game);
}

/* Finding the match start (H1, docs/research-arcade-balance-desyncs.md).
 *
 * `G_No[1..3] == (2, 0, 0)` says only "the Game task is parked on the
 * Game2_0 slot" (`Game_Jmp_Tbl[G_No[1]]` -> `Game02` ->
 * `Game02_Jmp_Tbl[G_No[2]]`, game.c). It does NOT say the match started.
 * A segment cut right after a final KO can carry that triple frozen for its
 * whole length while the Game task is not being ticked at all -- measured on
 * two archives whose (G_No, C_No) pair never changes across 2,270 and 2,286
 * frames. Starting there put the archive mid-match against a fresh engine and
 * reported `Game_timer (0) != 6501` at archive frame 1: a harness artifact,
 * not a divergence.
 *
 * The signature of a match that actually started is the visible effect of
 * `Game2_0()` (game.c) having run: it writes, in one frame,
 *     `Game_timer = 0; C_No[0..3] = 0; G_No[2] = 3;`
 * So require the frame AFTER the triple to show `Game_timer == 0` and
 * `G_No[2] == 3`. Measured over the 16-segment corpus in
 * /Volumes/KimchDrive/3sarm-convert-tmp/rerun2: identical `start_index` (1)
 * on all 14 segments that contain a match, and no start found on the two that
 * do not -- which is the correct answer for those, not an error. */
ScrdGameInitResult ScrdGame_Init(ScrdGame* game, const char* ram_archive_path) {
    SDL_zerop(game);
    game->start_index = -1;

    if (!RamArchive_Init(&game->archive, ram_archive_path)) {
        SDL_Log("ScrdGame_Init: Failed to initialize RAM archive");
        return SCRD_GAME_INIT_ARCHIVE_ERROR;
    }

    /* True when the PREVIOUS frame carried the (2, 0, 0) triple and its
     * match setup has been latched into `game`. */
    bool armed = false;

    for (int frame_num = 0;; frame_num++) {
        SDL_IOStream* io = RamArchive_GetFrame(&game->archive, frame_num);

        if (io == NULL) {
            break;
        }

        const Uint16 g_no_1 = scrd_read_u16(io, G_NO_OFFSET + 2);
        const Uint16 g_no_2 = scrd_read_u16(io, G_NO_OFFSET + 4);
        const Uint16 g_no_3 = scrd_read_u16(io, G_NO_OFFSET + 6);
        const Uint16 game_timer = scrd_read_u16(io, GAME_TIMER_OFFSET);

        // Game2_0() ran between the armed frame and this one.
        if (armed && (game_timer == 0) && (g_no_2 == 3)) {
            game->start_index = frame_num;
            SDL_CloseIO(io);
            break;
        }

        armed = (g_no_1 == 2) && (g_no_2 == 0) && (g_no_3 == 0);

        if (armed) {
            scrd_read_match_setup(game, io);
        }

        SDL_CloseIO(io);
    }

    if (game->start_index == -1) {
        SDL_Log("ScrdGame_Init: no match start in '%s' -- the (2,0,0) G_No triple is never "
                "followed by Game2_0()'s Game_timer=0 / G_No[2]=3 (segment holds no match)",
                ram_archive_path);
        RamArchive_Destroy(&game->archive);
        return SCRD_GAME_INIT_NO_MATCH_START;
    }

    return SCRD_GAME_INIT_OK;
}

void ScrdGame_Destroy(ScrdGame* game) {
    RamArchive_Destroy(&game->archive);
    SDL_zerop(game);
}

#endif
