#ifndef SCENE_JUMP_SPIKE_H
#define SCENE_JUMP_SPIKE_H

/* === SPIKE — instant scene jump prototype (docs/savestates-and-instant-mode-jump.md §9 Q1) ===
 *
 * Settles the design doc's open question #1: does the title->training-match
 * side-effect chain (SJ-06) run correctly when its steps are invoked
 * directly, outside their normal task-dispatch context, behind a `No_Trans`
 * black cover?
 *
 * This is a prototype. It is #if DEBUG only, armed only by
 * --test-instant-jump, and is NOT wired into any shipped path. Do not
 * promote it as-is; its value is the PASS/FAIL answer and the frame
 * accounting it prints.
 */

#include <stdbool.h>

/* True when --test-instant-jump was passed (DEBUG builds only). */
bool SceneJumpSpike_Active(void);

/* Per-frame tick, called from TestRunner_Prologue in place of the normal
 * phase machine. Drives: wait-for-title -> direct chain call (one frame)
 * -> load-drain wait -> battle entry -> liveness verification -> clean
 * exit. Prints a SCENE-JUMP PASS/FAIL line and exits the process. */
void SceneJumpSpike_Tick(void);

#endif /* SCENE_JUMP_SPIKE_H */
