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
    RamArchive archive;
} ScrdGame;

bool ScrdGame_Init(ScrdGame* game, const char* ram_archive_path);
void ScrdGame_Destroy(ScrdGame* game);

#endif

#endif
