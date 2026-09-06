#ifndef CMD_DATA_H
#define CMD_DATA_H

#include "structs.h"
#include "types.h"

typedef const s16* const_s16_arr;
extern const_s16_arr* player_CMD[];
extern const_s16_arr* player_cmd[];
extern const s16 ukemi_time_tbl[17];
extern const s16 pl_cmd_num[20][7];
extern const s16 lever_gacha_tbl[16];
extern const u16 chk22_tbl[8];
extern void* pl_cmd[];
extern void* pl_CMD[];
extern s16 lvr_chk_tbl[2][4];

// MARK: - Serialized

extern WORK_CP wcp[2];
extern T_PL_LVR t_pl_lvr[2];
/* First `waza_work[]` entry that CPS3 does NOT clear at battle start: it clears
 * 0x540 bytes of each 0x620-byte (56 x 28) command-state block, so entries
 * 48..55 CARRY ACROSS THE MATCH BOUNDARY. `cmd_init()` (cmd_main.c) reproduces
 * that under `ArcadeBalance_IsEnabled()`, and the statcheck oracle and seed
 * audit key off this same boundary -- see Class C in
 * docs/research-arcade-balance-desyncs.md. */
#define WAZA_WORK_CARRIED_FIRST 48

extern WAZA_WORK waza_work[2][56];

// MARK: - Unhandled

extern s16 cmd_id;
extern s16* cmd_tbl_ptr;
extern u16 sw_work;
extern T_PL_LVR* chk_pl;
extern s16 waza_type[2];
extern WAZA_WORK* waza_ptr;
extern PLW* cmd_pl;

#endif
