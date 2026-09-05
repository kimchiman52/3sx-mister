#ifndef SCENE_JUMP_H
#define SCENE_JUMP_H

/* === Scene jump — the shared title -> live-training-match chain ===
 *
 * The SJ-06 side-effect chain (docs/savestates-and-instant-mode-jump.md §4),
 * promoted out of the DEBUG-only spike into shipped code. It performs the
 * whole title -> training-match transition as direct calls in ONE frame's
 * prologue, behind the `No_Trans` cover, leaving only the genuinely
 * multi-frame pieces (LDREQ drain, round intro, training-menu dismissal) to
 * the ordinary frame loop.
 *
 * Two consumers:
 *   - src/quick_training.c — the OSD "Quick Training" feature.
 *   - src/test/scene_jump_spike.c — the --test-instant-jump harness
 *     (#if DEBUG), which keeps proving this exact chain end to end.
 *
 * PRECONDITION for SceneJump_ExecuteTrainingChain: the engine must be
 * sitting on the post-coin title idle scene (G_No == {2,0,1}, `game.c` ->
 * `Game0_1`) with the LDREQ queue drained. That is the state the chain was
 * proven from (SJ-15); calling it from anywhere else is undefined.
 *
 * NETPLAY: never call any of this during an active netplay session. The
 * chain and the wipe machinery write rollback-visible state (`Exec_Wipe`
 * is in the GS_SAVE set). Callers gate on
 * Netplay_GetSessionState() == NETPLAY_SESSION_IDLE.
 */

#include "types.h"

#include <stdbool.h>

typedef struct SceneJumpTrainingParams {
    s8 chars[2]; /* engine (3SX) character ids for P1/P2 */
    s8 arts[2];  /* super arts 0-2 */
    /* Stage id, or < 0 to derive it the way the stock select-exit does
     * (sel_pl.c -> Setup_Battle_Country: the challenger's character id,
     * with the Q / double-Q random-stage special cases). */
    s8 stage;
    /* true: zero-seed the RNG via Setup_Net_Random_ix() (deterministic
     * harness runs). false: stock offline seeding from Interrupt_Timer. */
    bool pin_rng;
} SceneJumpTrainingParams;

/* Execute the whole chain (sets No_Trans = 1 as its first action) and park
 * the scene in Game12_0 awaiting the load drain. Returns the stage id
 * actually used (== params->stage unless it was < 0). */
s16 SceneJump_ExecuteTrainingChain(const SceneJumpTrainingParams* params);

/* True once every load the chain pushed has landed: LDREQ queue drained,
 * both players' character data loaded, stage BG loaded. `stage` is the
 * value SceneJump_ExecuteTrainingChain returned. */
bool SceneJump_TrainingLoadsDrained(s16 stage);

/* Flip the parked scene into the battle scene (Game02/Game2_0, which runs
 * the SJ-06 step-11 block itself and asserts the drained queue). Call once,
 * after SceneJump_TrainingLoadsDrained() reports true.
 *
 * Also runs init_omop() first -- the character-select exit's last act
 * (sel_pl.c -> Exit_6th), in the same "loads drained, about to flip" slot,
 * and the thing that makes the engine DIP tables non-zero before the round
 * boot copies them into plw[]. See the call site for the measurement. */
void SceneJump_EnterBattleScene(void);

/* Per-frame while waiting for the match to go live: injects the presses
 * that dismiss the training menu the round blocks on (SJ-21) into
 * p1sw_buff, alternating on `frame_parity` (pass frame & 1). Returns true
 * once the match is live (the training_mode_gameplay_started predicate:
 * Mode_Type == MODE_NORMAL_TRAINING, Allow_a_battle_f set, no pause). */
bool SceneJump_TrainingMenuDismissTick(u32 frame_parity);

#endif /* SCENE_JUMP_H */
