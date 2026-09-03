/*
 * replay_overlay.c — Step C2 of docs/plan-fcade-replay-browser.md.
 *
 * Minimal viewer UX for the C1 .3sr replay player. Draws the player-name HUD
 * labels, a hold-START-to-exit hint, and terminal-state messages through the engine's
 * native SSPutStrProP text path — the same mechanism the Direct-P2P overlay
 * uses (src/netplay/direct_p2p_overlay.c). Read-only over the player: it
 * never mutates replay state, so C1's checksum cadence is untouched.
 *
 * Called once per frame from game_step_0 (src/main.c), before njdp2d_draw()
 * flushes the 2D sprite list. A no-op when no replay is loaded.
 */

#include "replay/replay_player.h"

#include "sf33rd/Source/Common/PPGWork.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"

#include <SDL3/SDL.h>

/* Overlay layout on the 384x224 game canvas. SSPutStrProP(flag=1, width,...)
 * centers the string in [0, width]; passing the canvas width centers it.
 * The exit hint sits at the bottom, clear of the top HUD (health/timer).
 * Terminal-state messages sit mid-canvas where gameplay has stopped.
 * Priority 1 matches the Direct-P2P overlay: above title/attract frames and
 * the HUD default (PrioBase[2]), below full-screen transition wipes
 * (PrioBase[0]) — see direct_p2p_overlay.c:34-39. */
#define RPL_OVL_CANVAS_W 384
#define RPL_OVL_HINT_Y 214
#define RPL_OVL_CENTER_Y 100
/* Wrap budget for the centred terminal-state line: 8 px margin each side,
 * 12 px pitch, three rows (y=100..124) -- the DIVERGED line measures 455 px
 * with a one-digit frame number and 530 px with a ten-digit one, so it is
 * never a one-liner on this canvas. */
#define RPL_OVL_TEXT_W 368
#define RPL_OVL_LINE_H 12
#define RPL_OVL_CENTER_MAX_LINES 3
#define RPL_OVL_ATR 9
#define RPL_OVL_COL 0xFFFFFFFFu
#define RPL_OVL_PRIO 1

/* Frames to keep "REPLAY COMPLETE" on screen after completion (~3s @60). */
#define RPL_OVL_COMPLETE_HOLD 180

/* --- S4: in-battle P1/P2 name labels under the health bars ---------------
 *
 * Primary placement (Fightcade-style): each player's name sits just below the
 * top-HUD cluster, next to their health bar. The HUD strip occupancy (all on
 * the 384x224 game canvas, verified against sc_sub.c) is:
 *   y16..24  health/vitality bar        (vital_put, sc_sub.c:1087-1088)
 *   y24..32  stun bar                   (stun_put,  sc_sub.c:1221-1222)
 *   y24..48  face portraits + char-name plate (player_face/player_name,
 *            scfont_sqput_face/scfont_sqput, sc_sub.c:1481-1499,1691-1729)
 * y=48 is the first fully clear row directly under that cluster (8px-tall
 * glyphs occupy y48..56), so the label reads as belonging to the bar above it
 * without overwriting any HUD element. A jumping sprite / super-flash near the
 * top of the play-field can transiently overlap y=48 — that is a TV-only
 * judgment; if it proves distracting, compile RPL_OVL_HUD_NAMES to 0 (there is
 * no bottom-line fallback any more — see the label note in ReplayOverlay_Draw).
 *
 * Color: the engine's own char-name plate (player_name) draws through
 * scfont_sqput on palette bank 5 in a different (small) font. We draw through
 * SSPutStrProP (the ASCII-pro font) — already a distinct glyph set — and, to
 * avoid the white-on-busy-background blend the plain 0xFFFFFFFF status line can
 * suffer, tint it a bright arcade yellow via the vertex color. atr stays 9
 * because bank 9 is the only palette proven to hold the ASCII-pro font CLUT
 * (every text caller uses it, e.g. frame_data_overlay.c:65); distinctness/
 * legibility come from the non-white vtxcol, which modulates that CLUT (the
 * frame-data overlay tints the same bank green/red/orange the same way). */
#define RPL_OVL_HUD_NAMES 1
#define RPL_OVL_NAME_Y 48
#define RPL_OVL_NAME_X_LEFT 8
#define RPL_OVL_NAME_X_RIGHT 376
/* Per-label width cap. The two labels share one 368 px row (LEFT..RIGHT);
 * half each minus an 8 px gap between them. Handles come from an external
 * sidecar and can be 63 glyphs (meta_p1_name[64] in replay_player.c) --
 * up to ~500 px -- so a label is cut to this with "..." (SSFitStrPro).
 * Truncation, not wrapping, because the row sits directly above the
 * play-field: there is no second line to wrap into. */
#define RPL_OVL_NAME_MAX_W 176
#define RPL_OVL_NAME_ATR 9
#define RPL_OVL_NAME_COL 0xFFF0E040u /* bright yellow (ARGB): distinct from the white HUD name plates */

/* Battle-state gate (read-only). The top-HUD health/stun/portrait/name cluster
 * is drawn only when `Disp_Cockpit && Game_pause != GAME_PAUSE_TRAINING`, so
 * Disp_Cockpit alone already excludes menus, char-select, KO and the attract
 * demo — everywhere the health bars are not on screen.
 *
 * We deliberately do NOT also require `Allow_a_battle_f`. That flag is 1 only
 * while a round is actively being fought: it goes true after the round-start
 * banner and is what gates the timer (count.c) and gameplay (game.c). Gating
 * on it held the names back through the entire "FIGHT!" intro, which is exactly
 * when a viewer wants to know who is playing. Names now appear with the health
 * bars. A plain global (workuser.h); we only READ it. */
extern u8 Disp_Cockpit;

/* sc_sub.c exports SSGetDrawSizePro (glyph-accurate string width in the
 * ASCII-pro font) but sc_sub.h only declares SSPutStrProP; forward-declare it
 * here to right-anchor the P2 label without editing the game header. */
extern s32 SSGetDrawSizePro(const s8* str);

#if RPL_OVL_HUD_NAMES
/* S4: draw the two player names at y=48, under their respective health bars —
 * P1 left-anchored, P2 right-anchored (width measured with SSGetDrawSizePro,
 * never flag=1 auto-center). Self-gates so it only fires while a round is being
 * shown (see the RPL_OVL_HUD_NAMES comment block): status must be PLAYING and
 * the HUD must be up (Disp_Cockpit).
 * Read-only over both player and engine state. A replay whose meta has no
 * players[] (both getters NULL) draws nothing here, and there is no bottom-line
 * fallback any more — a no-names replay simply shows no label. */
/* Compose "name [R]" where R is the player's Fightcade letter rank (rank
 * 1..6 -> E,D,C,B,A,S — Fightcade's own convention, fightcade-api
 * `Rank = ['Unranked','E','D','C','B','A','S']`). rank 0/out-of-range ->
 * name alone (never fabricate a rank). Kept compact ("[C]", 4 extra glyphs)
 * for the tight 384-wide HUD row. */
static void compose_name_label(const char* name, int rank, char* out, size_t out_sz) {
    static const char rank_letters[7] = { '?', 'E', 'D', 'C', 'B', 'A', 'S' };

    if (rank >= 1 && rank <= 6) {
        SDL_snprintf(out, out_sz, "%s [%c]", name, rank_letters[rank]);
    } else {
        SDL_strlcpy(out, name, out_sz);
    }
}

static void draw_name_labels(void) {
    /* Gate on the HUD being up, NOT on the round being live. Disp_Cockpit is
     * set with the health bars (manage.c), while Allow_a_battle_f only goes
     * true once the round-start banner finishes and the timer starts running
     * (it also gates the timer in count.c and gameplay in game.c). Gating on
     * both used to hold the names back through the whole "FIGHT!" intro, which
     * is precisely when a viewer is looking for who is playing. Names now
     * appear the moment the health bars do. Still excludes menus,
     * char-select, KO and the attract demo, because Disp_Cockpit is 0 there. */
    if (ReplayPlayer_GetStatus() != REPLAY_PLAYER_PLAYING || Disp_Cockpit == 0) {
        return;
    }

    const char* p1 = ReplayPlayer_GetP1Name();
    const char* p2 = ReplayPlayer_GetP2Name();

    /* No-names replay: leave the HUD clear, rely on the bottom fallback line. */
    if (p1 == NULL && p2 == NULL) {
        return;
    }

    char label1[80];
    char label2[80];

    if (p1 != NULL) {
        compose_name_label(p1, ReplayPlayer_GetP1Rank(), label1, sizeof(label1));
        (void)SSFitStrPro(label1, RPL_OVL_NAME_MAX_W);
        SSPutStrProP(0, RPL_OVL_NAME_X_LEFT, RPL_OVL_NAME_Y, RPL_OVL_NAME_ATR, RPL_OVL_NAME_COL, label1,
                     RPL_OVL_PRIO);
    }

    if (p2 != NULL) {
        compose_name_label(p2, ReplayPlayer_GetP2Rank(), label2, sizeof(label2));
        /* Right-anchor: fit to the cap (returns the real glyph width) and
         * subtract from the right edge. The clamp cannot fire now that the
         * width is capped, but it costs nothing and keeps the u16 x safe. */
        const s32 w = SSFitStrPro(label2, RPL_OVL_NAME_MAX_W);
        s32 x = RPL_OVL_NAME_X_RIGHT - w;
        if (x < 0) {
            x = 0;
        }
        SSPutStrProP(0, (u16)x, RPL_OVL_NAME_Y, RPL_OVL_NAME_ATR, RPL_OVL_NAME_COL, label2, RPL_OVL_PRIO);
    }

    /* Headless/SSH-verifiable evidence: the display is occlusion-throttled, so
     * log the first frame the HUD name overlay actually draws, with the
     * resolved names+ranks and the chosen y. Once per process — no per-frame
     * spam. */
    static bool logged = false;
    if (!logged) {
        logged = true;
        SDL_Log("replay-overlay: HUD name labels drawn (y=%d) p1='%s' p2='%s'", RPL_OVL_NAME_Y,
                p1 != NULL ? label1 : "(none)", p2 != NULL ? label2 : "(none)");
    }
}
#endif /* RPL_OVL_HUD_NAMES */

/* "Hold START to exit" plus a trivial [====    ] progress bar that fills as
 * START is held toward the abort threshold. */
static void draw_exit_hint(void) {
    if (!ReplayPlayer_ShouldShowExitHint()) {
        return;
    }

    const int held = ReplayPlayer_GetExitHoldFrames();
    const int threshold = ReplayPlayer_GetExitHoldThreshold();

    if (held > 0 && threshold > 0) {
        char hint[48];
        const int pips = 8;
        int filled = (held * pips) / threshold;

        if (filled < 0) {
            filled = 0;
        }
        if (filled > pips) {
            filled = pips;
        }

        char bar[16];
        int b = 0;
        bar[b++] = '[';
        for (int i = 0; i < pips; i++) {
            bar[b++] = (i < filled) ? '=' : '.';
        }
        bar[b++] = ']';
        bar[b] = '\0';

        SDL_snprintf(hint, sizeof(hint), "HOLD START TO EXIT  %s", bar);
        SSPutStrProP(1, RPL_OVL_CANVAS_W, RPL_OVL_HINT_Y, RPL_OVL_ATR, RPL_OVL_COL, hint, RPL_OVL_PRIO);
    } else {
        SSPutStrProP(1, RPL_OVL_CANVAS_W, RPL_OVL_HINT_Y, RPL_OVL_ATR, RPL_OVL_COL, "HOLD START TO EXIT", RPL_OVL_PRIO);
    }
}

void ReplayOverlay_Draw(void) {
    if (!ReplayPlayer_IsActive()) {
        return;
    }

    /* Boot-order guard: with --play-replay the player is ACTIVE from the very
     * first game frame, but SSPutStrProP renders through ppgScrList, whose
     * texture group is only bound by Scrscreen_Init() (sc_sub.c:431) inside
     * Init_Task_1st — and cpReadyTask arms TASK_INIT at condition=2, which
     * cpLoopTask only flips to "run" on the SECOND frame. Drawing on frame 1
     * sends njDrawSprite -> ppgWriteQuadWithST_B2 through ppgScrList.tex ==
     * NULL (PPGFile.c:463) — a guaranteed segfault (reproduced on MiSTer and
     * on desktop under the dummy/offscreen video drivers). Skip drawing until
     * the SCR font texture group actually exists; the browser's list draw is
     * naturally safe because it only opens after Init_Task completes
     * (replay_browser.c RB_WAIT_BOOT gate). */
    if (ppgScrList.tex == NULL) {
        return;
    }

    /* Countdown that keeps "REPLAY COMPLETE" up briefly after the match ends
     * rather than flashing for a single frame. -1 = not yet started; latched
     * to the hold length on the first COMPLETE frame, then counts down to 0
     * and stays there (never re-latches). The overlay owns it — no player
     * state is touched. */
    static int complete_hold = -1;

    const ReplayPlayerStatus st = ReplayPlayer_GetStatus();

    switch (st) {
    case REPLAY_PLAYER_NAVIGATING:
    case REPLAY_PLAYER_PLAYING: {
        /* No bottom status line: the player names under the health bars (top
         * HUD, draw_name_labels) are the label now — the redundant bottom
         * "REPLAY  P1 vs P2  date" line was removed at user request. */
#if RPL_OVL_HUD_NAMES
        draw_name_labels(); /* self-gates: only draws in-battle while PLAYING */
#endif
        draw_exit_hint(); /* self-gates: only draws while PLAYING */

        complete_hold = -1;
        break;
    }

    case REPLAY_PLAYER_DESYNCED: {
        /* Stage S6: a genuine checkpoint divergence — the live/local engine no
         * longer matches the recording's checksum. Honest + player-facing copy
         * (hard rule: never hide or soften a real divergence). Deliberately
         * DISTINCT from a network abort ("LIVE REPLAY ABORTED: STREAM LOST ..."
         * below, REPLAY_PLAYER_ABORTED): a divergence is a fidelity fact about
         * the recording, not a lost connection. The detector that set this
         * status (check_checkpoint) is unchanged. */
        char line[80];
        SDL_snprintf(line, sizeof(line), "REPLAY DIVERGED (frame %u) - can't reproduce this recording exactly",
                     ReplayPlayer_GetDesyncFrame());
        SSPutStrProWrapP(1, RPL_OVL_CANVAS_W, RPL_OVL_CENTER_Y, RPL_OVL_LINE_H, RPL_OVL_TEXT_W,
                         RPL_OVL_CENTER_MAX_LINES, RPL_OVL_ATR, RPL_OVL_COL, line, RPL_OVL_PRIO);
        break;
    }

    case REPLAY_PLAYER_COMPLETE:
        if (complete_hold == -1) {
            complete_hold = RPL_OVL_COMPLETE_HOLD;
        }

        if (complete_hold > 0) {
            complete_hold -= 1;
            SSPutStrProP(1, RPL_OVL_CANVAS_W, RPL_OVL_CENTER_Y, RPL_OVL_ATR, RPL_OVL_COL, "REPLAY COMPLETE",
                         RPL_OVL_PRIO);
        }
        break;

    case REPLAY_PLAYER_ABORTED:
        /* The hold-START abort and the browser-return path both leave the
         * screen silent, exactly as before. */
        break;

    case REPLAY_PLAYER_INACTIVE:
    default:
        break;
    }
}
