#ifndef REPLAY_REPLAY_SHUFFLE_H
#define REPLAY_REPLAY_SHUFFLE_H

/* The weekly-best shuffle viewer — the replacement for the deleted in-game
 * replay browser (src/replay/replay_browser.c, removed in the descope).
 *
 * Selecting "Watch Replays" in the MiSTer OSD relaunches the game with
 * `--watch-replays`; this module then plays the cached `.3sr` set back to
 * back, forever, reshuffling when it runs off the end. There is no list, no
 * cursor, no end state and no persistence: shuffle order is re-drawn from a
 * fresh entropy seed on every boot, and nothing about it survives a reboot.
 *
 * ONE BINDING: hold START for ~1 s to skip to the next replay. There is no
 * hold-to-exit on the pad — the MiSTer OSD is how you leave the viewer, either
 * by picking another core or by picking "Quick Training", which stops the
 * viewer through ReplayShuffle_Stop() and lands in a live training match
 * (src/quick_training.c). The C1 player's own hold-START-to-exit is suppressed
 * for every replay this module launches (replay_player.c -> ReplayPlayer_Tick,
 * gated on !s_browser_owned), so neither that abort nor its
 * "HOLD START TO EXIT" hint can appear here. One button, one hint.
 *
 * ALWAYS-COMPILED release module. Every entry point is a cheap early-return
 * when the viewer is disabled, so a normal boot pays nothing.
 *
 * Lifecycle (mirrors the C1 player's tick/draw split):
 *   - ReplayShuffle_Configure() once in main() right after read_args().
 *   - ReplayShuffle_IsEnabled() in initialize_game() to fold into the
 *     boot-time config pin — the viewer inherits the browser's obligation to
 *     call ReplayPlayer_PinConfig() once for the whole session, because
 *     ReplayPlayer_LoadAndStart deliberately does NOT re-pin.
 *   - ReplayShuffle_Tick() once per frame in game_step_0, BEFORE
 *     ReplayPlayer_Tick(): the hold-START skip gesture has to read the REAL
 *     pads, and ReplayPlayer_Tick overwrites p1sw_buff/p2sw_buff with the
 *     injected words (and zeroes them once terminal), so a tick placed after
 *     it would only ever see injected input.
 *   - ReplayShuffle_Draw() once per frame from BOTH draw branches of
 *     game_step_0 — the held-frame branch as well as the normal one. Every
 *     inter-replay transition happens on held frames, where only
 *     ReplayOverlay_Draw() would otherwise run.
 *   - ReplayShuffle_Destroy() from cleanup().
 */

#include <stdbool.h>

/* Stash the CLI state before any tick. `enabled` is --watch-replays;
 * `root_override` is --watch-replays-root (NULL/"" = use the
 * `replays-root` config key / platform default). Call once in main() right
 * after read_args(); the config-key path is read later (after Config_Init in
 * SDLApp_FullInit), so this must not touch Config_*. */
void ReplayShuffle_Configure(bool enabled, const char* root_override);

/* True when the shuffle viewer owns this session. Safe any time after
 * Configure. Used at boot to decide whether to apply the session config pin
 * (arcade-balance + console mode + identity buttons). */
bool ReplayShuffle_IsEnabled(void);

/* Per-frame state machine: waits for the title screen, starts the first
 * replay, chains to the next one when the current one reaches a terminal
 * state, reshuffles at the end of the set, and handles the hold-START-to-skip
 * gesture. No-op when disabled. MUST run before ReplayPlayer_Tick(). */
void ReplayShuffle_Tick(void);

/* Per-frame render of the skip hint and the inter-replay transition line,
 * through SSPutStrProP. Read-only over engine and player state. */
void ReplayShuffle_Draw(void);

/* Stop the viewer for the rest of the session WITHOUT touching the loaded
 * replay. `reason` is logged (never NULL-checked away: pass a literal).
 *
 * What this actually buys, and why it is the stop rather than one of the
 * alternatives: RS_OFF is the state machine's ONLY permanent state, and
 * ReplayShuffle_Tick() returns on its first line while in it — so no path
 * back to rs_start_next() survives. Every other state has one. Destroying
 * the player instead would be worse than useless: ReplayShuffle_Tick() reads
 * REPLAY_PLAYER_INACTIVE as terminal, runs the inter-replay transition and
 * starts the NEXT replay. Clearing the CLI flag would not help either — the
 * flag is only read in RS_UNINIT (and by the boot-time config pin).
 *
 * Deliberately does NOT tear the loaded replay down. A terminal replay is
 * FREEZING the whole engine frame (replay_player.c -> s_stall_frame), and
 * that freeze is what keeps the live post-match flow away from
 * Game_Manage_10th and the "qix is out of range" fatal. Un-freezing is the
 * caller's business and has to be paired with putting the engine somewhere
 * safe; ReplayPlayer_Destroy() is the call that does it.
 *
 * Idempotent. Safe when disabled / never started. */
void ReplayShuffle_Stop(const char* reason);

/* Release the viewer's hold on the player (tears the loaded replay down).
 * Safe when disabled / never started. */
void ReplayShuffle_Destroy(void);

/* Resolved replay root (CLI override > explicit config key > platform
 * default honoring THIRDSARM_HOME). Never NULL. Safe after Config_Init. */
const char* ReplayShuffle_GetRoot(void);

#endif /* REPLAY_REPLAY_SHUFFLE_H */
