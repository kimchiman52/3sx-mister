#if defined(STATCHECK)

#ifndef SCRD_GAME_H
#define SCRD_GAME_H

#include "test/ram_archive.h"

#include <SDL3/SDL.h>

typedef struct ScrdGame {
    int start_index;
    Uint8 characters[2];
    Uint8 supers[2];
    Uint8 colors[2];
    Uint8 new_challenger;
    /* Home stage, already mapped into the port's 3SX index space (H2). */
    Uint8 stage;
    /* Per-player `wu.wu_operator` as the archive holds it at `start_index`
     * (H4b): 1 = human, 0 = CPU AI. Latched for the rejection message only --
     * the harness cannot reproduce a CPU-driven side, see ScrdGame_Init. */
    Uint8 wu_operator[2];
    RamArchive archive;
} ScrdGame;

/* Outcome of ScrdGame_Init. Two of the four outcomes are HARNESS LIMITS, not
 * engine divergences, and callers must keep them distinct from
 * ARCHIVE_ERROR/divergence so a sweep never turns one into a worklist item.
 *
 * NO_MATCH_START: the runner's segmenter cuts a new game_N only when G_No[1]
 * stops being 2 (docs/research-arcade-balance-desyncs.md H4), which can hand
 * us a segment that is entirely the post-KO tail of the previous match and
 * contains no Game2_0() at all -- nothing to compare (H1).
 *
 * CPU_PLAYER: the recording had `wu.wu_operator == 0` on at least one side,
 * i.e. the cabinet ran that side from `cpu_algorithm()` under
 * `Play_Type == 0`. The harness cannot reproduce that (H4b); see the block
 * comment above the check in ScrdGame_Init for why. */
typedef enum ScrdGameInitResult {
    SCRD_GAME_INIT_OK,
    SCRD_GAME_INIT_ARCHIVE_ERROR,
    SCRD_GAME_INIT_NO_MATCH_START,
    SCRD_GAME_INIT_CPU_PLAYER,
} ScrdGameInitResult;

ScrdGameInitResult ScrdGame_Init(ScrdGame* game, const char* ram_archive_path);
void ScrdGame_Destroy(ScrdGame* game);

#endif

#endif
