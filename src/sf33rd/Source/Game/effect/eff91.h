#ifndef EFF91_H
#define EFF91_H

#include "structs.h"
#include "types.h"

void effect_91_move(WORK_Other* ewk);
s32 effect_91_init(s16 master_id, s16 type, s16 target_bg, s16 char_ix, s16 char_ix2, s16 master_player);
/* World-space anchors for the three VS-result labels. */
extern const s16 EFF91_Pos_Data[2][3][2];

#endif
