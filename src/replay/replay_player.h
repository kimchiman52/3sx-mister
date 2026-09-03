#ifndef REPLAY_REPLAY_PLAYER_H
#define REPLAY_REPLAY_PLAYER_H

/* Step C1 of docs/plan-fcade-replay-browser.md — runtime .3sr replay player.
 *
 * ALWAYS-COMPILED release module (deliberately no #if DEBUG / #if STATCHECK
 * gate): loads a `.3sr` file (docs/3sr-format.md) once at startup, then
 * drives the real engine through Title -> Menu -> Character Select -> Game
 * with the archived setup (characters/SA/colors/New_Challenger/RNG seeds)
 * and feeds the archived per-frame input words through the
 * p1sw_buff/p2sw_buff latch in game_step_0 — rendering and audio fully on,
 * normal frame pacing (plan §4.1). The phase machine is a clone of the
 * STATCHECK harness's src/test/statcheck_runner.c (the proven oracle),
 * minus the SCRD archive compare; divergence is instead sampled via the
 * .3sr's sparse 13-field djb2 checksum table (docs/3sr-format.md §4).
 *
 * Inert unless --play-replay <file.3sr> was given: every entry point is a
 * cheap early-return when no replay is loaded. */

#include <SDL3/SDL.h>
#include <stdbool.h>

typedef enum ReplayPlayerStatus {
    REPLAY_PLAYER_INACTIVE = 0, /* no --play-replay / not loaded */
    REPLAY_PLAYER_NAVIGATING,   /* driving title/menu/char-select */
    REPLAY_PLAYER_PLAYING,      /* injecting in-game input words */
    REPLAY_PLAYER_COMPLETE,     /* game ended or inputs exhausted; pads released */
    REPLAY_PLAYER_DESYNCED,     /* checksum mismatch; pads released */
    REPLAY_PLAYER_ABORTED,      /* refused/stopped (e.g. netplay session became active) */
} ReplayPlayerStatus;

/* Parse + load the .3sr into memory (input words are converted arcade->SWK
 * here, at load time — docs/3sr-format.md §3). Returns false with an
 * SDL_Log diagnostic on any malformed/corrupt file. Call once from main()
 * right after read_args(); safe pre-SDL_Init (file IO only). */
bool ReplayPlayer_Init(const char* path_3sr);

/* True once Init succeeded (regardless of later completion/desync). */
bool ReplayPlayer_IsActive(void);

/* Runtime launch of a replay in an already-running process, no restart.
 *
 * INTENTIONALLY CALLER-LESS AS OF THE REPLAY DESCOPE. Its only caller was
 * the in-game replay browser (src/replay/replay_browser.c), now deleted; the
 * shuffle viewer that replaces it picks this up. Keep it exported and
 * non-static. It is the ONLY entry point that sets the player's file-scope
 * s_browser_owned, which makes a terminal replay FREEZE and wait to be torn
 * down by its owner instead of calling SDLApp_Exit() and ending the process.
 * A back-to-back playlist is only possible through this function.
 *
 * Frees any previously loaded replay, loads + validates `path_3sr` (same loader as
 * ReplayPlayer_Init), resets the phase machine to its start, then normalizes
 * the engine to a clean title via Soft_Reset_Sub() so the SAME title->menu->
 * char-select->game phase machine C1 drives replays through takes over.
 *
 * Returns false (logged) on a netplay session being active or a bad file.
 *
 * v1 PIN MODEL: playback correctness requires arcade-balance=true + console
 * mode + identity button mapping (A3b co-necessities). This call assumes the
 * pin is ALREADY in force and does not re-pin — the owning module must call
 * ReplayPlayer_PinConfig() once for the whole session before the first
 * LoadAndStart. (initialize_game() used to do that for the browser session;
 * with the browser gone, the shuffle viewer inherits the obligation.)
 * Scoped/dynamic per-launch pinning is future work (see the NOTE in
 * ReplayPlayer_PinConfig). */
bool ReplayPlayer_LoadAndStart(const char* path_3sr);

/* True when THIS frame must be held: the engine tick + input latch are
 * skipped and only the overlay is redrawn over the last rendered frame.
 * Computed by ReplayPlayer_Tick; game_step_0 reads it AFTER the Tick call and
 * before the p1sw_buff -> p1sw_0 latch. Terminal states (COMPLETE / DESYNCED
 * / ABORTED) hold every frame until teardown — that hold is what stops the
 * game's post-match flow free-running into the qix effect trap. */
bool ReplayPlayer_IsStallingThisFrame(void);

/* Adopt caller-supplied metadata (player names, Fightcade ranks 1..6,
 * ms-epoch date) for the loaded session. Call right after a successful
 * ReplayPlayer_Init; every field is optional (NULL/0 = leave unknown). Names
 * are sanitized to printable ASCII. Fed from argv by main.c's --play-replay
 * boot path; a .meta.json sidecar, if one turns up, corroborates rather than
 * clobbers what is set here. */
void ReplayPlayer_SetLiveMeta(const char* p1, int p1_rank, const char* p2, int p2_rank, long long date_ms);

/* Session-only config pin (arcade-balance=true + game-mode=console +
 * default button mapping) — the A3b-proven co-necessities for archived
 * sessions to reproduce. Mirrors StatcheckRunner_PinConfig; never writes
 * the on-disk config file. Call from initialize_game() before
 * ArcadeBalance_Init(), only when ReplayPlayer_IsActive(). */
void ReplayPlayer_PinConfig(void);

/* Per-frame injection hook. Call from game_step_0 after NetplayNav_Tick()
 * and BEFORE the p1sw_buff -> p1sw_0 latch. Writes p1sw_buff/p2sw_buff
 * while navigating/playing; a no-op (pads released) once complete,
 * desynced, aborted, or inactive. */
void ReplayPlayer_Tick(void);

/* Per-frame divergence sampling. Call at the TOP of game_step_1 (same
 * boundary as StatcheckRunner_Epilogue — after njUserMain, before
 * Interrupt_Timer/Scrn_Renew mutate more state; the .3sr checksums were
 * computed from SCRD frames captured at that boundary). */
void ReplayPlayer_Epilogue(void);

/* Free the loaded replay (safe to call when inactive). */
void ReplayPlayer_Destroy(void);

/* State for the future playback overlay (Stage C/D). */
ReplayPlayerStatus ReplayPlayer_GetStatus(void);
/* .3sr frame index of the failed checksum checkpoint; only meaningful when
 * status == REPLAY_PLAYER_DESYNCED. */
Uint32 ReplayPlayer_GetDesyncFrame(void);
/* Path of the optional <name>.meta.json sidecar if one exists next to the
 * loaded .3sr, else NULL. Parsed at Init (Step C2 wires cJSON). */
const char* ReplayPlayer_GetMetaJsonPath(void);

/* ---------------------------------------------------------------------- */
/* Step C2 — viewer overlay metadata + hold-START-to-exit UX              */
/* ---------------------------------------------------------------------- */

/* Player names parsed from the .meta.json sidecar (players[0/1].name).
 * NULL when the sidecar is missing/corrupt or carries no player entry — the
 * overlay falls back to ReplayPlayer_GetLabel() in that case. The returned
 * strings are sanitized to printable ASCII (safe for the SS glyph table). */
const char* ReplayPlayer_GetP1Name(void);
const char* ReplayPlayer_GetP2Name(void);

/* Fightcade rank 1..6 (letter grades E,D,C,B,A,S per Fightcade's own
 * convention); 0 = unranked/unknown (draw the name alone — never fabricate).
 * Sourced from the meta sidecar's players[].rank or the live handoff. */
int ReplayPlayer_GetP1Rank(void);
int ReplayPlayer_GetP2Rank(void);

/* "YYYY-MM-DD" derived from the sidecar's ms-epoch `date`, or NULL when
 * absent/null. */
const char* ReplayPlayer_GetDateString(void);

/* Fallback label: the loaded .3sr basename with the extension stripped.
 * Never NULL once a replay is loaded (empty string worst case). */
const char* ReplayPlayer_GetLabel(void);

/* Hold-START-to-exit hint gating. True while the overlay should draw the
 * "Hold START to exit" hint: during the first few seconds of a match, or
 * any time START is currently being held. Only ever true while PLAYING. */
bool ReplayPlayer_ShouldShowExitHint(void);

/* Consecutive frames the real user has held START this episode (0 when not
 * held), and the threshold at which playback aborts to title. Together they
 * drive the hint's progress pip. */
int ReplayPlayer_GetExitHoldFrames(void);
int ReplayPlayer_GetExitHoldThreshold(void);

/* Draw the playback status/hint/terminal-state overlay for the current
 * frame. Call once per frame from the game render path (main.c game_step_0,
 * before njdp2d_draw). A no-op when no replay is loaded. Reads player state
 * only — never mutates it, so C1's checksum cadence is untouched. */
void ReplayOverlay_Draw(void);

#endif /* REPLAY_REPLAY_PLAYER_H */
