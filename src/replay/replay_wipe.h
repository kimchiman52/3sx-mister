#ifndef REPLAY_REPLAY_WIPE_H
#define REPLAY_REPLAY_WIPE_H

/* The replay viewer's PRIVATE screen cover — the 76-band diagonal wipe the
 * game already uses, redrawn by the viewer so the engine walk between two
 * replays (Title -> Menu -> Character Select -> Game) is HIDDEN rather than
 * watched.
 *
 * The walk is never skipped. PHASE_GAME ends on game_ended(), i.e.
 * `PL_Wins[0] == 2 || PL_Wins[1] == 2`, and PL_Wins is zeroed by
 * game.c -> Game01_Sub() — reached only through the full walk, NOT by
 * Soft_Reset_Sub(). Short-circuit the walk and replay N+1 terminates at
 * frame 0. So the walk runs exactly as before and this module just paints
 * over it.
 *
 * WHY A PRIVATE COVER AND NOT THE ENGINE'S OWN WIPE. Every alternative in
 * the engine mutates state the walk itself depends on:
 *
 *  - sc_sub.c -> WipeOut()/WipeIn() do `WipeLimit += 1` on the single global
 *    `u8 WipeLimit`, and the engine runs its OWN transitions underneath us
 *    for the whole walk (game.c -> Game0_2/Game12_2/Game01, sel_pl.c ->
 *    Sel_PL_Cont_1st/2nd/3rd, manage.c -> Game_Manage_2_1, all through
 *    Switch_Screen/Switch_Screen_Revival). A second caller incrementing
 *    WipeLimit every frame HALVES every engine wipe, which moves each
 *    G_No/S_No transition earlier — and replay_player.c's fixed waits
 *    (wait_timer = 60/20/45) were tuned against the real timings, so
 *    set_cursor/tap_button would land in a different S_No state and pick the
 *    wrong character, colour or stage. That presents as a frame-0 desync.
 *
 *  - sys_sub.c -> Switch_Screen_Init/Switch_Screen/Switch_Screen_Revival are
 *    worse still: they write Exec_Wipe, Active_Wipe_Type, Forbid_Break,
 *    Gap_Timer, Stop_SG, Escape_SS and Stop_Combo. Exec_Wipe gates gameplay
 *    and effect routines (spgauge.c, effd0/effd1/effe0/effl2/eff09, pause.c,
 *    menu.c), so touching it changes effect behaviour, hence RNG
 *    consumption, hence the .3sr checksums.
 *
 *  - No_Trans is not a hiding mechanism either: game.c -> Game_Task gates
 *    texture_cash_update() and the Mtrans_use_trans_mode refresh on
 *    !No_Trans, so suppressing draws starves the texture cache.
 *
 * What this module writes instead is njdp2d_w and nothing else: quads go in
 * through dc_ghost.c -> njDrawPolygon2D (attr 0x20, the only value that
 * appends) and out through njdp2d_draw -> Renderer_DrawSolidQuad. None of
 * that is engine state, so none of it can reach replay_player.c's
 * gather_live_fields (C_No[0..3], Game_timer, Random_ix16/32, both players'
 * positions, both input words). The module also never touches the engine
 * RNG.
 *
 * LIFECYCLE, one replay to the next:
 *
 *   ... battle ... [8-frame diagonal wipe-out] [full-screen black cover] ...
 *   ... freeze + "REPLAY COMPLETE" ... "NEXT REPLAY..." ...
 *   ... [cover still up] Title -> Menu -> Character Select ... [drop] ...
 *
 * The wipe-out has to START BEFORE THE FREEZE, because a held frame renders
 * BLACK: main.c's replay_frame_hold branch skips njUserMain(), and
 * SoftwareRenderer_RenderFrame's render_band -> clear_band fills opaque black
 * and then draws only the quads submitted this frame (arrsetlen(quads, 0) at
 * the end). There is no retained scene to wipe over once the freeze starts.
 * replay_player.c arms the wipe from a deterministic countdown (see
 * arm_exit_wipe there).
 *
 * The ENTRY needs no wipe-in of our own. sel_pl.c -> Sel_PL_Cont_2nd()
 * (S_No[0]==1) runs Switch_Screen(1) + Switch_Screen_Init(1), and
 * Sel_PL_Cont_3rd() (S_No[0]==2) runs Switch_Screen_Revival(0) = WipeIn(0),
 * whose WipeLimit==0 frame fully covers the screen. So we hold a solid black
 * cover until G_No[1]==1 && S_No[0]>=2 and simply DROP it: the engine's own
 * wipe-in does the reveal, with no seam and no viewer-side wipe-in code. */

#include <stdbool.h>

/* Raise the full-screen black cover immediately (no wipe). Called when a
 * replay is armed — the screen is already black at that point (the freeze
 * plus the inter-replay card), so there is nothing to wipe and a hard cover
 * is seamless. `why` is logged. */
void ReplayWipe_Cover(const char* why);

/* Start the 76-band diagonal wipe-out, to complete in `budget` live frames
 * (clamped to 1..REPLAY_WIPE_STEPS). Ignored when a cover is already up or a
 * wipe is already running, so the caller may arm it unconditionally. */
void ReplayWipe_BeginExit(int budget, const char* why);

/* True while a wipe-out is running or a cover is up. */
bool ReplayWipe_IsActive(void);

/* Drop the cover (the engine's own wipe-in takes over the reveal). */
void ReplayWipe_Reveal(const char* why);

/* Hard clear with no reveal log — teardown / abort paths. */
void ReplayWipe_Reset(void);

/* Advance the private step counter. `live_frame` is false on a held frame
 * (main.c's replay_frame_hold): those render black with no engine geometry,
 * so a wipe in progress snaps straight to the full cover. Call once per
 * frame from game_step_0, BEFORE njUserMain — the reveal is decided against
 * the S_No the engine left at the end of the previous frame, which is the
 * frame WipeIn(0) full-covers. */
void ReplayWipe_Tick(bool live_frame);

/* Submit the cover for this frame. Call from game_step_0's LIVE draw branch
 * only, next to ReplayOverlay_Draw/ReplayShuffle_Draw and before
 * njdp2d_draw(). Held frames are already black and still need their overlay
 * text readable, so they get no cover. */
void ReplayWipe_Draw(void);

#endif /* REPLAY_REPLAY_WIPE_H */
