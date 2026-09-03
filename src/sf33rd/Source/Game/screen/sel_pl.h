#ifndef SEL_PL_H
#define SEL_PL_H

#include "types.h"

extern s16 Play_Type_1st;

s16 Select_Player();

/* The stock select-exit stage choice (called by Exit_2nd; also used by the
 * scene-jump chain in src/scene_jump.c to derive the stage a normal
 * training entry would have picked). Reads Mode_Type, My_char[],
 * New_Challenger and VS_Stage. */
u8 Setup_Battle_Country();

#endif
