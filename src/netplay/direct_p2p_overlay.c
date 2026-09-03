/*
 * direct_p2p_overlay.c — Step 8 of docs/plan-stun-direct-p2p.md.
 *
 * Game-side status overlay for the Direct-P2P orchestrator. Renders
 * through the existing SSPutStrPro native-text path into the 384x224
 * game canvas, so the overlay shows while the main menu is visible —
 * no RmlUi dependency, no separate SDL_Renderer target.
 *
 * Called once per frame from NetplayScreen_Render()
 * (src/port/sdl/netplay_screen.c). State is read non-blockingly via
 * DirectP2P_GetState / DirectP2P_GetHostCode / DirectP2P_GetStatusText.
 *
 * No-op when DirectP2P_GetState() == DIRECT_P2P_IDLE so the overlay
 * vanishes cleanly after the worker hands off to netplay.c.
 */

#include "netplay/direct_p2p.h"

#ifdef ENABLE_NETPLAY

#include "sf33rd/Source/Game/ui/sc_sub.h"

#include <SDL3/SDL.h>

// Overlay layout on the 384x224 canvas. SSPutStrProP(flag=1, width, ...)
// centers the string in [0, width] — passing the canvas width centers
// on the canvas. Uses the explicit-priority variant so the overlay
// stays above the title-sequence logo, attract-mode game frames, and
// "PRESS ANY BUTTON" which otherwise overdraw at lower priorities.
#define DP2P_OVL_CANVAS_W 384
#define DP2P_OVL_LINE1_Y 70
#define DP2P_OVL_LINE2_Y 100
#define DP2P_OVL_LINE3_Y 120
// Line 3 is the only line fed by composed text (DirectP2P_GetStatusText:
// ConnectFail_UserText strings, and the arm-time refusal reason that
// Netplay_RefuseArm builds from ArcadeBalance_GetReason). Centring an
// over-wide string spills it off BOTH edges symmetrically, which is what
// "...s arcade balance - CPS3 ROM not found or failed content..." on a
// user's screen was. So line 3 word-wraps: 368 px per line keeps an 8 px
// margin each side, 12 px pitch leaves 4 px between 8 px glyph rows, and
// 8 lines (y=120..204, glyph bottom 212) stay inside the 224-high canvas.
// Nothing else draws in that band while the orchestrator owns the screen
// (NetplayScreen_Render returns right after DirectP2P_DrawOverlay).
#define DP2P_OVL_TEXT_W 368
#define DP2P_OVL_LINE_H 12
#define DP2P_OVL_LINE3_MAX_LINES 8
#define DP2P_OVL_ATR 9
#define DP2P_OVL_COL 0xFFFFFFFFu
/* Engine convention: PrioBase index is a Z depth — LOWER index = closer
 * to camera = drawn in front. PrioBase[0] is used by full-screen wipes
 * (sc_sub.c -> WipeOut), PrioBase[2] by HUD / title text / "PRESS ANY
 * BUTTON" (entry.c -> Disp_00_0). Priority 1 sits above the title
 * sequence and attract frames while staying below transition wipes. */
#define DP2P_OVL_PRIO 1

// Mode label for line 1. Groups each orchestrator state into one of
// four user-facing categories so the player immediately knows whether
// we are hosting, joining, connected, or failing. The detailed message
// is on line 3 via DirectP2P_GetStatusText().
//
// The fallback states (FALLBACK_SIGNALING, FALLBACK_BILATERAL_PUNCH)
// can fire on either role's path, so the label branches on
// DirectP2P_GetRole() rather than mapping the state alone. The pre-
// fallback states (HOST_WAITING is host-only by construction,
// JOIN_PUNCHING is join-only) are also routed through the role check
// for symmetry; their behavior is unchanged from before 5a because the
// role is set once per BeginHost/BeginJoin to match.
static const char* dp2p_overlay_mode_label(DirectP2PState s) {
    Role role = DirectP2P_GetRole();
    switch (s) {
    case DIRECT_P2P_UPNP_PROBE:
    case DIRECT_P2P_STUN_DISCOVER:
        return (role == ROLE_JOIN) ? "CONNECTING" : "HOSTING";

    case DIRECT_P2P_HOST_WAITING:
    case DIRECT_P2P_FALLBACK_SIGNALING:
    case DIRECT_P2P_FALLBACK_BILATERAL_PUNCH:
        return (role == ROLE_HOST) ? "HOSTING" : "CONNECTING";

    case DIRECT_P2P_JOIN_PUNCHING:
        return "CONNECTING";

    case DIRECT_P2P_HANDOFF:
        return "CONNECTED";

    case DIRECT_P2P_FAILED_SYMMETRIC:
    case DIRECT_P2P_FAILED_STUN:
    case DIRECT_P2P_FAILED_PUNCH:
    case DIRECT_P2P_FAILED_BILATERAL:
    case DIRECT_P2P_FAILED_HANDSHAKE:
        return "ERROR";

    case DIRECT_P2P_IDLE:
    default:
        return "";
    }
}

// SSH-verifiable evidence of the wrap (the display is not observable from
// a headless run or a field log): once per distinct status text, log how
// line 3 was laid out -- each line's glyphs and measured width against
// the budget. Bounded by the number of distinct statuses a session posts.
static void dp2p_overlay_log_layout(const char* status) {
    static char last[256];
    SSProLine lines[SS_PRO_WRAP_MAX_LINES];
    s32 needed;
    s32 shown;
    s32 i;

    if (SDL_strcmp(last, status) == 0) {
        return;
    }

    SDL_strlcpy(last, status, sizeof(last));
    needed = SSWrapStrPro(status, DP2P_OVL_TEXT_W, lines, DP2P_OVL_LINE3_MAX_LINES);
    shown = needed < DP2P_OVL_LINE3_MAX_LINES ? needed : DP2P_OVL_LINE3_MAX_LINES;
    SDL_Log("[direct_p2p] overlay line 3: %d line(s) needed, %d shown, budget %d px x %d lines (total %d px): \"%s\"",
            needed, shown, DP2P_OVL_TEXT_W, DP2P_OVL_LINE3_MAX_LINES, SSGetDrawSizePro((const s8*)status), status);

    for (i = 0; i < shown; i++) {
        SDL_Log("[direct_p2p]   line %d: %d px \"%.*s\"", i + 1, lines[i].width, (int)lines[i].len,
                status + lines[i].off);
    }
}

void DirectP2P_DrawOverlay(void) {
    const DirectP2PState state = DirectP2P_GetState();
    if (state == DIRECT_P2P_IDLE) {
        return;
    }

    const char* label = dp2p_overlay_mode_label(state);
    if (label && label[0] != '\0') {
        SSPutStrProP(1, DP2P_OVL_CANVAS_W, DP2P_OVL_LINE1_Y, DP2P_OVL_ATR,
                     DP2P_OVL_COL, label, DP2P_OVL_PRIO);
    }

    // Line 2: room code, standalone and prominent. DirectP2P_GetHostCode()
    // returns the code only during HOST_WAITING and "" in every other state,
    // so this draws exactly when a shareable code exists.
    const char* code = DirectP2P_GetHostCode();
    if (code && code[0] != '\0') {
        SSPutStrProP(1, DP2P_OVL_CANVAS_W, DP2P_OVL_LINE2_Y, DP2P_OVL_ATR,
                     DP2P_OVL_COL, code, DP2P_OVL_PRIO);
    }

    // Line 3: per-state status detail (no room code — that's line 2).
    // Always non-NULL, possibly empty — skip the draw when empty so
    // SSPutStrProP doesn't try to center a zero-width string. Word-wrapped
    // (see DP2P_OVL_TEXT_W): the refusal reasons alone measure 314-1126 px
    // against a 384 px canvas, so this line can never assume it fits.
    const char* status = DirectP2P_GetStatusText();
    if (status && status[0] != '\0') {
        SSPutStrProWrapP(1, DP2P_OVL_CANVAS_W, DP2P_OVL_LINE3_Y, DP2P_OVL_LINE_H, DP2P_OVL_TEXT_W,
                         DP2P_OVL_LINE3_MAX_LINES, DP2P_OVL_ATR, DP2P_OVL_COL, status, DP2P_OVL_PRIO);
        dp2p_overlay_log_layout(status);
    }
}

#endif /* ENABLE_NETPLAY */
