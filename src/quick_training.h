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
 * netplay menu navigation, or a --play-replay boot session — those own the
 * engine and/or make wipe state rollback-visible, and --play-replay ends the
 * process with its one replay, so there is nothing behind it to return to.
 * From every OFFLINE state (attract, title, menus, character select, a live
 * match, even the pause menu) the request is honored: states past the title
 * are torn down through the same soft-reset flow the shipped START+BACK reset
 * uses. A request while a Quick Training sequence is already running is
 * ignored.
 *
 * The --watch-replays SHUFFLE VIEWER is honored, not refused, and the request
 * ENDS it: the playlist is stopped (ReplayShuffle_Stop) and the loaded replay
 * is torn down (ReplayPlayer_Destroy) before the wipe-out, so the sequence
 * runs from the replay's own scene exactly as it would from a live match.
 * "Quick Training" and "Watch Replays" are two rows of the same OSD; picking
 * one has to mean leaving the other. The viewer stays off for the rest of the
 * session — its only re-entry is the OSD row, which restarts the core.
 *
 * THE USER'S SETTINGS COME BACK, and the wrong-buttons defect was not where
 * it looked. A --watch-replays boot applies ReplayPlayer_PinConfig() for the
 * whole process; qt_begin() now calls ReplayPlayer_UnpinConfig() next to the
 * replay teardown, which restores the captured game mode and balance key —
 * the "Stage F2a" scoped pin the old note deferred. But the pin was never the
 * reason a training match ran on the default pad mapping: measured, its
 * save_w[] writes are overwritten by Setup_Default_Game_Option() on every
 * TASK_INIT walk, and the real cause is that nothing carries save_w[1] (where
 * the settings load puts the user's mapping) into the training slots
 * save_w[4]/[5] unless the player visits the option screens this sequence
 * exists to skip. qt_carry_user_pad_config() does that carry, one frame before
 * the chain fires, on EVERY Quick Training jump — replay boot or not.
 *
 * One thing genuinely does not come back: the RESOLVED arcade/PS2 balance.
 * ArcadeBalance_Init() reads the config key once at boot and latches the
 * answer for the process (arcade_balance.c), so a --watch-replays session
 * keeps the balance the pin let it select. The key itself is restored.
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
