#ifndef QUICK_TRAINING_H
#define QUICK_TRAINING_H

/* === Quick Training — OSD -> live training match, no menus ===
 *
 * The MiSTer OSD "Quick Training" row (menu.sv `"T[15],Quick Training;"`)
 * signals the running game with SIGRTMIN+5 (thirdsarm_wrapper.cpp ->
 * quick_training_signal). The game-side handler (main.c ->
 * handle_signal_requests) calls QuickTraining_Request(), and the per-frame
 * QuickTraining_Tick() then drives, inside the game's own transition
 * language:
 *
 *   1. diagonal wipe-out (Switch_Screen_Init + Switch_Screen(1)) over
 *      whatever is on screen,
 *   2. behind the `No_Trans` cover: teardown to the title if needed
 *      (the shipped soft-reset flow), then the proven SJ-06 chain
 *      (src/scene_jump.c) with the last-used characters/arts from the
 *      persisted training config,
 *   3. wipe-in (Switch_Screen_Revival(1)) over the live match.
 *
 * Context policy (the edge cases, decided): a request is IGNORED (with a
 * log line) during a netplay session, active direct-P2P orchestration,
 * netplay menu navigation, or a replay/shuffle-viewer session — those own
 * the engine and/or make wipe state rollback-visible. From every OFFLINE
 * state (attract, title, menus, character select, a live match, even the
 * pause menu) the request is honored: states past the title are torn down
 * through the same soft-reset flow the shipped START+BACK reset uses.
 * A request while a Quick Training sequence is already running is ignored.
 */

#include <stdbool.h>

/* Queue a quick-training jump. Called from the SIGRTMIN+5 drain in
 * main.c (same thread as the tick). Validation happens in the tick. */
void QuickTraining_Request(void);

/* Per-frame driver. Called from game_step_0() BEFORE the p*sw_buff ->
 * p*sw_0 latch (it suppresses/injects pad input while a sequence runs,
 * the NetplayNav_Tick precedent). No-op while idle with no request. */
void QuickTraining_Tick(void);

/* True when the DEBUG-only --test-quick-training harness is armed (always
 * false in non-DEBUG builds). TestRunner_Prologue/Epilogue early-out on
 * this so the phase machine leaves the session to this module. */
bool QuickTraining_TestActive(void);

#endif /* QUICK_TRAINING_H */
