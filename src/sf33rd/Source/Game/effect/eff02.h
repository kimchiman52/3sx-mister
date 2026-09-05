#ifndef EFF02_H
#define EFF02_H

#include "structs.h"
#include "types.h"

typedef struct {
    u16 se;
    u8 hits;
    u8 deff;
    u8 kezu;
    u8 fsin;
    u8 status;
    u8 quake;
    u8 dir;
    u8 col;
    u8 myhix;
    u8 emhix;
} HMDT;

typedef struct {
    u16 chix;
    s16 hx;
    s16 hy;
} EXPLEM;

extern const s16 hit_mark_dir_table[16];
extern const HMDT hmdt[];
extern const s16 hcct[];
extern const s16 gqdt[][2];

/* The screen-quake table the current balance reads: `gqdt` (the PS2
 * decompilation's, unchanged) under PS2, the CPS3 0x061B941A transcription
 * under arcade balance. Rows 7 and 8 are the only two that differ. See E5b in
 * docs/research-arcade-balance-desyncs.md and the tables in eff02.c. */
const s16 (*gqdt_active(void))[2];

extern const EXPLEM explem[];
extern const EXPLEM explem2[][20];
extern const s16 hit_mark_hosei_table[][2];

void effect_02_move(WORK_Other* ewk);
s32 effect_02_init(WORK* wk, s8 dmgp, s8 mkst, s8 dmrl);

#endif
