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
    RamArchive archive;
} ScrdGame;

/* Outcome of ScrdGame_Init. NO_MATCH_START is NOT an error and NOT a
 * divergence: the runner's segmenter cuts a new game_N only when G_No[1]
 * stops being 2 (docs/research-arcade-balance-desyncs.md H4), which can hand
 * us a segment that is entirely the post-KO tail of the previous match and
 * contains no Game2_0() at all. Callers must keep that outcome distinct from
 * ARCHIVE_ERROR so a sweep does not read "no match here" as "engine
 * divergence" (H1). */
typedef enum ScrdGameInitResult {
    SCRD_GAME_INIT_OK,
    SCRD_GAME_INIT_ARCHIVE_ERROR,
    SCRD_GAME_INIT_NO_MATCH_START,
} ScrdGameInitResult;

ScrdGameInitResult ScrdGame_Init(ScrdGame* game, const char* ram_archive_path);
void ScrdGame_Destroy(ScrdGame* game);

#endif

#endif
