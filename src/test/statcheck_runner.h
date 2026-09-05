#if defined(STATCHECK)

#ifndef STATCHECK_RUNNER_H
#define STATCHECK_RUNNER_H

#include "test/scrd_game.h"

#include <SDL3/SDL.h>

#include <stdbool.h>

/* Plan A3b (docs/plan-fcade-replay-browser.md): the statcheck replay-fidelity
 * runner — upstream src/test/test_runner.c's phase machine ported into fork
 * idioms under renamed entry points (§4.3), so it can never collide with the
 * DEBUG harness's TestRunner_* symbols.
 *
 * Init parses the SCRD archive (game setup + start frame); Prologue runs in
 * game_step_0 before the p1sw_buff -> p1sw_0 latch and drives
 * Title -> Menu -> Character Select -> Game via injected SWK-layout inputs
 * written to p1sw_buff/p2sw_buff; Epilogue runs between game_step_0 and
 * game_step_1's Scrn_Renew (upstream Main_StepFrame/Main_FinishFrame
 * ordering) and compares engine state against the archived frame.
 * Exit code 0 = every archived frame matched; 1 = first mismatch (printed
 * with archive frame number by statcheck_compare.c); 2 = the archive holds no
 * match to check (H1, see ScrdGame_Init); 3 = the recording had a CPU player,
 * which this harness cannot reproduce (H4b, see ScrdGame_Init).
 *
 * Init returns ScrdGame_Init's result verbatim so main.c can keep the two
 * harness-limit outcomes (SCRD_GAME_INIT_NO_MATCH_START,
 * SCRD_GAME_INIT_CPU_PLAYER) out of the divergence exit code. */
ScrdGameInitResult StatcheckRunner_Init(const char* ram_archive_path);
void StatcheckRunner_Destroy(void);

/* `wu.wu_operator` the archive held at the match-start frame, for player 0/1.
 * Valid after StatcheckRunner_Init returns anything but ARCHIVE_ERROR; exists
 * so main.c can name the values in its SCRD_GAME_INIT_CPU_PLAYER message. */
Uint8 StatcheckRunner_WuOperator(int player);
void StatcheckRunner_Prologue(void);
void StatcheckRunner_Epilogue(void);

/* Review round-1 finding P-1 (docs/plan-fcade-replay-browser.md): the
 * archive's verdict depends on config/state the harness never declared —
 * game-mode, arcade-balance, and the in-game button remap all feed engine
 * behavior the archive was captured against, and any of them silently
 * corrupting comparisons is indistinguishable from a real engine bug.
 * Call once, after main.c's SDLApp_FullInit() (so Config_Init/
 * init_game_mode have already run their normal course) and before
 * ArcadeBalance_Init() (so the arcade-balance pin is in place before that
 * function reads it) — see main.c's initialize_game(). Forces the
 * in-memory/session state only; never touches the on-disk config file. */
void StatcheckRunner_PinConfig(void);

#endif

#endif
