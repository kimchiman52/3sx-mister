/*
 * netplay_nav: auto-drive the console-mode menu chain at cold launch.
 *
 * Injects SWK_START at Title, at Game0_1 title-screen, and at Mode_Select
 * case 3 so the game runs through Game0_2 cases 3-5 (texture teardown +
 * BGM switch) and Mode_Select's case-3 path (Setup_VS_Mode + char-select
 * init) on both peers, then fires Netplay_BeginDirectP2P() once
 * remote_ip has been wired.
 *
 * Natural console-mode flow (press counts needed):
 *   1. Loop_Demo attract (G_No[0]==1). Ck_Coin() consumes a Start edge,
 *      runs Next_Title_Sub() which flips G_No[0]=2, registers TASK_ENTRY
 *      (E_No[0]=1 -> Entry_01). Demo_Flag=1 but Title screen is now up.
 *   2. Game0_0 -> Game0_1 on the title screen. Entry_01 case 1 consumes
 *      a Start edge, calls Entry_01_Sub() which sets Request_G_No=1.
 *      Game0_1 sees Request_G_No and advances G_No[2]++. Game0_2 cycles
 *      its r_no[3] 0..5 over ~6 frames; case 5 does
 *      cpReadyTask(TASK_MENU, Menu_Task) and sets G_No[1]=12.
 *   3. Menu_Task runs After_Title -> Menu_Init -> Mode_Select. Mode_Select
 *      r_no[2] cycles 0..3 for the fade-in; case 3 accepts Start with
 *      cursor row 1 -> Setup_VS_Mode(), G_No[1]=12, cpExitTask(TASK_MENU),
 *      Mode_Type=MODE_VERSUS. G_No[1] was already 12; Game12 now owns the
 *      frame and runs Select_Player (char-select) with all textures and
 *      BGM primed by the path above.
 *
 * The nav state machine never stalls indefinitely: each state has a
 * safety timeout after which it falls through to the next state, and
 * the MODE_NETWORK override + DIP-switch sync are always applied before
 * NAV_WAIT_ORCHESTRATOR. Cold-launch still reaches NAV_START_NETPLAY
 * even if something upstream blocks the expected transition. A timeout
 * path means char select will render wrong (the bug this module exists
 * to fix), but the session still starts — better than deadlocking.
 * S3 closed the one exception: NAV_WAIT_ORCHESTRATOR historically had
 * NO deadline and hung forever when the orchestrator never populated
 * remote_ip; it now bails to NAV_DONE on terminal orchestrator failure,
 * on orchestrator-idle, or on an overall deadline (HOST_WAITING, which
 * is unbounded by design, re-arms the deadline instead).
 */

#include "netplay/netplay_nav.h"

#include "main.h"
#include "netplay/direct_p2p.h"
#include "netplay/netplay.h"
#include "port/config/draw_players_above_hud.h"
#include "port/sdl/sdl_app.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/system/work_sys.h"

#include <stdio.h>

typedef enum {
    NAV_IDLE = 0,
    NAV_WAIT_INIT,
    NAV_PRESS_COIN,
    NAV_PRESS_TITLE,
    NAV_WAIT_MENU,
    NAV_DRIVE_VS,
    NAV_WAIT_ORCHESTRATOR,
    NAV_START_NETPLAY,
    NAV_DONE,
} NavState;

static const char* state_name(NavState s) {
    switch (s) {
    case NAV_IDLE: return "NAV_IDLE";
    case NAV_WAIT_INIT: return "NAV_WAIT_INIT";
    case NAV_PRESS_COIN: return "NAV_PRESS_COIN";
    case NAV_PRESS_TITLE: return "NAV_PRESS_TITLE";
    case NAV_WAIT_MENU: return "NAV_WAIT_MENU";
    case NAV_DRIVE_VS: return "NAV_DRIVE_VS";
    case NAV_WAIT_ORCHESTRATOR: return "NAV_WAIT_ORCHESTRATOR";
    case NAV_START_NETPLAY: return "NAV_START_NETPLAY";
    case NAV_DONE: return "NAV_DONE";
    }
    return "NAV_?";
}

static NavState s_state = NAV_IDLE;
/* Frame counter per state — used for debounce and safety timeouts. */
static int s_frames_in_state = 0;
/* Tracks whether the current-state Start-press has been injected this
 * cycle so we inject for exactly one frame (rising edge in Ck_Coin /
 * Entry_01 / Mode_Select is ~p*sw_1 & p*sw_0 & SWK_START). */
static bool s_press_injected = false;

static void transition_to(NavState next) {
    fprintf(stderr, "[netplay_nav] state %s -> %s\n", state_name(s_state), state_name(next));
    fflush(stderr);
    s_state = next;
    s_frames_in_state = 0;
    s_press_injected = false;
}

static void inject_start_press(void) {
    p1sw_buff |= SWK_START;
    p2sw_buff |= SWK_START;
    s_press_injected = true;
}

/* Periodic re-injection helper: fire SWK_START for one frame, skip a
 * debounce window, then re-arm. This lets us re-try if the receiving
 * state wasn't ready on the first attempt (e.g. Entry_Task hadn't
 * transitioned into Entry_01 yet when we pressed during NAV_PRESS_COIN).
 * The debounce is important — pressing every frame would trigger the
 * rising-edge check every frame and cause unwanted repeated activation.
 * 8 frames (~0.13s) is a conservative lower bound: the game polls input
 * every frame and Menu_Task's r_no machine advances at most once per
 * frame, so a missed press is never more than a handful of frames from
 * the next retry window. */
#define NAV_PRESS_DEBOUNCE_FRAMES 8

/* S3 Part A: NAV_WAIT_ORCHESTRATOR overall deadline (see that case for
 * the exit taxonomy). The counter re-arms while the orchestrator sits in
 * HOST_WAITING, which is unbounded by design.
 *
 * Task #76: this used to be a flat `150 * 60` frames — 150 SECONDS. Two
 * problems with that. First, its comment derived 150 s from the PRE-S6
 * SERIAL cascade (STUN + punch + signalling + bilateral, summed); since
 * S6 those legs race inside ONE race budget, so the stated derivation
 * had silently stopped being true. Second, and the reason it is a
 * product bug: when the orchestrator wedges without publishing a
 * terminal state, the player watches a static "Connecting..." overlay
 * for two and a half minutes. That reads as a HANG, not a timeout — they
 * power-cycle long before the attributed failure ever appears. S3 built
 * the failure taxonomy precisely so every failure has a fast,
 * attributable cause; a 150 s backstop throws that away.
 *
 * A smaller FLAT constant is not the fix, for two reasons. The shallow
 * one: at the config ceiling (race-budget-ms 30000, stun-timeout-ms
 * 15000) the joiner's attempts legitimately total 92.8 s and the host's
 * port-map + STUN retry ladder 87.05 s, so any constant short enough to
 * be good UX at the shipped defaults would abort a legal maximal config
 * mid-attempt — a spurious failure, which is worse than a slow one.
 * (The host figure read 120.2 s until #96 latched the port-map verdict
 * and stopped re-probing on every rung; #131 then added back the 600 ms
 * of per-rung worker startup the ladder macro had been undercounting.
 * Both numbers are DERIVED in direct_p2p.c and pinned by _Static_assert
 * there — this sentence is prose about them, so it is the half that can
 * go stale, and twice now has.) The
 * deep one: a constant is what rotted. Replacing one constant whose
 * derivation went stale with another constant whose derivation can go
 * stale fixes today's number and not the failure mode.
 *
 * So the bound is DERIVED, in direct_p2p.c, from the very symbols the
 * orchestrator's own enforcement sites use — including
 * RACE_HARD_CAP_MS(), which is shared with the race deadline check, so
 * nav's deadline can never cut inside a confirmed punch's H-1 tails.
 * direct_p2p.c carries _Static_asserts that fail the BUILD if a cascade
 * change pushes the shipped-defaults deadline past what this product
 * treats as an acceptable wait, or drops the coverage the ceiling
 * configs need.
 *
 * At the shipped defaults that collapses 150 s to 31.8 s; a maximal
 * config still gets everything it is entitled to. */

/* Pure ms -> frames conversion, exported (netplay_nav.h) so the deadline
 * this file actually enforces is observable without standing up a session
 * or driving the nav machine. Production composes it with
 * DirectP2P_OrchWorstCaseMs() one call site below; a checker composes the
 * same two functions with a pinned role. */
int NetplayNav_OrchTimeoutFrames(int orch_worst_case_ms) {
    if (orch_worst_case_ms < 0) orch_worst_case_ms = 0;
    const long long ms = (long long)orch_worst_case_ms + NAV_ORCH_TIMEOUT_MARGIN_MS;
    return (int)((ms * NAV_FPS) / 1000);
}

static int nav_orch_timeout_frames(void) {
    return NetplayNav_OrchTimeoutFrames(DirectP2P_OrchWorstCaseMs());
}

static void drive_start_press(void) {
    if (!s_press_injected) {
        inject_start_press();
    } else if ((s_frames_in_state % NAV_PRESS_DEBOUNCE_FRAMES) == 0) {
        s_press_injected = false;
    }
}

static void apply_network_mode_override(void) {
    /* Switch to MODE_NETWORK and force both peers to use identical
     * DIP-switch settings. save_w[MODE_NETWORK] is read by Game01's
     * init path via mode-indexed accessors; applying here (before
     * Game12 advances into character select) ensures both peers
     * hash the same state on frame 0. */
    Mode_Type = MODE_NETWORK;
    // Present_Mode is a PresentMode, not a ModeType (upstream #296); NETWORK
    // and NETPLAY happen to share numeric value 2 in both enums, but assign
    // the correctly-typed constant here rather than leaning on that.
    Present_Mode = PRESENT_MODE_NETPLAY;
    save_w[MODE_NETWORK].Time_Limit = 99;
    save_w[MODE_NETWORK].Battle_Number[0] = 1;
    save_w[MODE_NETWORK].Battle_Number[1] = 1;
    /* CPS3's default service setting is Damage_Level 1.  The arcade
     * archives measured 1 on all 143 corpus segments; level 0 is a
     * materially lower-damage setting, not an alias for normal damage. */
    save_w[MODE_NETWORK].Damage_Level = 1;
    save_w[MODE_NETWORK].Handicap = 0;
    save_w[MODE_NETWORK].GuardCheck = 0;
    const u8 identity[8] = { 0, 1, 2, 11, 3, 4, 5, 11 };
    for (int p = 0; p < 2; p++) {
        for (int s = 0; s < 8; s++) {
            save_w[MODE_NETWORK].Pad_Infor[p].Shot[s] = identity[s];
        }
        save_w[MODE_NETWORK].Pad_Infor[p].Vibration = 0;
    }
}

void NetplayNav_Arm(void) {
    if (s_state != NAV_IDLE) {
        /* Idempotent — already armed or driving. */
        return;
    }
    /* Netplay arms ONLY in verified-arcade balance state (arm-time
     * predicate; see Netplay_ArmAllowed in netplay.c). Balance is fixed
     * at boot and digest-checked in the MIST handshake, so gated peers
     * always simulate identical arcade data. On refusal the reason is
     * routed to the direct-P2P overlay (ERROR + text). */
    if (!Netplay_ArmAllowed()) {
        Netplay_RefuseArm();
        return;
    }
    /* Force console game mode for the session. The arcade path
     * (Loop_Demo branch in game.c) inline-jumps to Game12 / MODE_ARCADE
     * on the first Coin press, skipping Mode_Select registration
     * entirely. Nav would then wait in NAV_WAIT_MENU until its safety
     * timeout without accomplishing anything, and peers would also
     * diverge on the arcade-specific DIP-switch block that we never
     * synchronize. */
    SDLApp_ForceConsoleGameMode();
    /* CFG_DRAW_PLAYERS_ABOVE_HUD's gameplay-affecting reads (effect-table
     * population, scr_trans/scr_calc selection) are a local config the
     * peers never negotiate — suppress for the netplay session (released
     * when the session finishes tearing down; see draw_players_above_hud.h). */
    DrawPlayersAboveHud_SetNetplaySuppressed(true);
    fprintf(stderr, "[netplay_nav] armed\n");
    fflush(stderr);
    transition_to(NAV_WAIT_INIT);
}

bool NetplayNav_IsActive(void) {
    return s_state != NAV_IDLE && s_state != NAV_DONE;
}

void NetplayNav_Reset(void) {
    s_state = NAV_IDLE;
    s_frames_in_state = 0;
    s_press_injected = false;
}

void NetplayNav_Tick(void) {
    if (s_state == NAV_IDLE || s_state == NAV_DONE) {
        return;
    }

    s_frames_in_state += 1;

    switch (s_state) {
    case NAV_WAIT_INIT:
        /* Wait for Init_Task to finish (condition=0 => exited) and the
         * boot sequence to land in Loop_Demo (G_No[0]==1). If someone
         * somehow advanced past Loop_Demo before we got here, pick up
         * at a later state. */
        if (G_No[0] == 2) {
            transition_to(NAV_PRESS_TITLE);
            break;
        }
        if (task[TASK_INIT].condition == 0 && G_No[0] == 1) {
            transition_to(NAV_PRESS_COIN);
        }
        break;

    case NAV_PRESS_COIN:
        /* Attract -> Title: inject Start (debounced) while G_No[0]==1.
         * Ck_Coin() rising-edge check wires this into Next_Title_Sub(),
         * which flips G_No[0]=2 and registers TASK_ENTRY. Safety
         * timeout ~10s. */
        if (G_No[0] == 2) {
            transition_to(NAV_PRESS_TITLE);
            break;
        }
        if (G_No[0] == 1) {
            drive_start_press();
        }
        if (s_frames_in_state > 600) {
            /* Timeout — pick the most-sensible fallthrough based on
             * where the engine actually is. If still in attract
             * (G_No[0]==1) the coin press never landed; give
             * NAV_PRESS_TITLE another 10s window in case the rising
             * edge fires there. If we somehow advanced (G_No[0]>1)
             * without this state seeing it, skip ahead to
             * NAV_WAIT_MENU. */
            if (G_No[0] == 1) {
                transition_to(NAV_PRESS_TITLE);
            } else {
                transition_to(NAV_WAIT_MENU);
            }
        }
        break;

    case NAV_PRESS_TITLE:
        /* Title -> Menu: inject Start (debounced) while G_No[0]==2 and
         * Menu_Task hasn't been registered yet. Entry_01 case 1 sees
         * the rising edge via ~p*sw_1 & p*sw_0 & SWK_START and calls
         * Entry_01_Sub(), setting Request_G_No=1. Game0_1 sees
         * Request_G_No and advances to Game0_2; Game0_2 case 5 does
         * cpReadyTask(TASK_MENU, Menu_Task) and sets G_No[1]=12.
         *
         * Advance to NAV_WAIT_MENU once TASK_MENU is registered
         * (condition 1 or 2 — cpReadyTask sets 2, the next main
         * loop iteration flips it to 1). Safety timeout ~10s. */
        if (task[TASK_MENU].condition == 1 || task[TASK_MENU].condition == 2) {
            transition_to(NAV_WAIT_MENU);
            break;
        }
        if (G_No[0] == 2) {
            drive_start_press();
        }
        if (s_frames_in_state > 600) {
            transition_to(NAV_WAIT_MENU);
        }
        break;

    case NAV_WAIT_MENU:
        /* Advance to NAV_DRIVE_VS as soon as Menu_Task is live. Don't
         * wait for Mode_Select's fade-in to hit r_no[2]==3 — that just
         * adds perceived latency on-screen. NAV_DRIVE_VS will keep
         * re-pressing Start on the NAV_PRESS_DEBOUNCE_FRAMES cadence;
         * Mode_Select ignores presses during case 0/1/2 (fade-in) and
         * accepts the first press after case 3 fires. Safety timeout
         * ~10s in case Menu_Task never registers. */
        if (task[TASK_MENU].condition == 1) {
            transition_to(NAV_DRIVE_VS);
            break;
        }
        if (s_frames_in_state > 600) {
            /* Safety fallthrough: apply the MODE_NETWORK override so
             * the orchestrator/session path still runs on sane state
             * even if Mode_Select never reached case 3. */
            apply_network_mode_override();
            transition_to(NAV_WAIT_ORCHESTRATOR);
        }
        break;

    case NAV_DRIVE_VS:
        /* Force cursor to row 1 (Versus) and inject Start. Mode_Select
         * case 3 sees the rising edge, runs Setup_VS_Mode(),
         * cpExitTask(TASK_MENU), sets G_No[1]=12.
         *
         * Connect_Status fixup: when only P1 has an Interface_Type
         * bound (the common case on desktop/MiSTer single-pad setups),
         * Menu_Task's header at menu.c:201 sets Connect_Status=0 and
         * Mode_Select case 3 then forces Menu_Cursor_Y[0] from 1 to 2
         * (Training) before the Start press latches. Override both
         * Interface_Type slots before Menu_Task reads them this frame
         * (nav tick runs after keyConvert() but before the task
         * dispatch that invokes Menu_Task) so Connect_Status=1 and the
         * cursor-coercion in case 3 is skipped. The override is
         * transient — ioconv restores pad->kind-derived values on the
         * next frame after we leave NAV_DRIVE_VS. */
        Interface_Type[0] = 2;
        Interface_Type[1] = 2;
        Menu_Cursor_Y[0] = 1;
        if (task[TASK_MENU].condition == 0) {
            /* Menu exited this frame — Mode_Select set Mode_Type=
             * MODE_VERSUS on the way out. Apply the MODE_NETWORK
             * override in the same tick so Game12's very first tick
             * already sees MODE_NETWORK and both peers hash identical
             * state from frame 0. */
            apply_network_mode_override();
            transition_to(NAV_WAIT_ORCHESTRATOR);
            break;
        }
        drive_start_press();
        if (s_frames_in_state > 600) {
            /* Safety fallthrough: apply the override anyway so the
             * orchestrator/session path still runs on sane state. */
            apply_network_mode_override();
            transition_to(NAV_WAIT_ORCHESTRATOR);
        }
        break;

    case NAV_WAIT_ORCHESTRATOR: {
        /* For --p2p-remote-ip (LAN/localhost) Netplay_SetParams wired
         * remote_ip at set_netplay_params() time, so this flips true
         * immediately. For the direct-P2P orchestrator path the
         * UPnP+STUN hole-punch takes several seconds and remote_ip is
         * populated by do_handoff() after that. Wait here so
         * NAV_START_NETPLAY doesn't hit the early-return in
         * Netplay_BeginDirectP2P() when remote_ip is NULL. */
        if (Netplay_IsRemoteIpSet()) {
            transition_to(NAV_START_NETPLAY);
            break;
        }

        /* S3 Part A (docs/plan-netplay-connection.md §5): pre-S3 this
         * was the ONLY state in the machine with no deadline — if the
         * orchestrator never set remote_ip, nav waited forever. Three
         * bounded exits, all to NAV_DONE (there is no session to start;
         * the direct-P2P overlay owns the ERROR display):
         *
         * (a) The orchestrator parked in a terminal FAILED_* state.
         *     Exception (review M-3: only while genuinely non-terminal):
         *     a HOST in FAILED_STUN with S2 auto-retries LEFT — Tick
         *     re-spawns the worker after backoff, so nav keeps waiting.
         *     Once the retry budget is exhausted FAILED_STUN is as
         *     terminal as any other failure and exit (a) fires; the
         *     pre-fix unconditional exception made nav wait out the full
         *     orchestrator deadline and then log a second, contradictory
         *     TIMEOUT_ORCHESTRATOR line for an already-reported failure.
         * (b) The orchestrator is IDLE with no remote ip — cancelled or
         *     never started. 5 s debounce covers the deferred-dispatch
         *     window at cold launch.
         * (c) Overall deadline. HOST_WAITING is legitimately unbounded
         *     by design (the host advertises until a joiner arrives),
         *     so the frame counter re-arms while the orchestrator sits
         *     there; every OTHER orchestrator state is internally
         *     bounded, and nav_orch_timeout_frames() is derived from
         *     exactly those bounds (task #76) so it covers the worst-case
         *     joiner (2 attempts) and host retry ladder at whatever the
         *     budgets are currently configured to, with margin. */
        const DirectP2PState dps = DirectP2P_GetState();
        if (dps == DIRECT_P2P_HOST_WAITING) {
            s_frames_in_state = 0; /* unbounded-by-design; re-arm deadline */
            break;
        }
        const bool orch_failed =
            dps == DIRECT_P2P_FAILED_SYMMETRIC || dps == DIRECT_P2P_FAILED_STUN ||
            dps == DIRECT_P2P_FAILED_PUNCH || dps == DIRECT_P2P_FAILED_BILATERAL ||
            dps == DIRECT_P2P_FAILED_HANDSHAKE;
        if (orch_failed && !DirectP2P_HostStunRetryPending()) {
            fprintf(stderr,
                    "[netplay_nav] orchestrator failed terminally (state %d) — "
                    "abandoning nav; overlay shows the reason\n", (int)dps);
            fflush(stderr);
            transition_to(NAV_DONE);
            break;
        }
        if (dps == DIRECT_P2P_IDLE && s_frames_in_state > 300) {
            fprintf(stderr, "[netplay_nav] orchestrator idle with no remote ip "
                            "after %d frames — abandoning nav\n", s_frames_in_state);
            fflush(stderr);
            transition_to(NAV_DONE);
            break;
        }
        const int deadline_frames = nav_orch_timeout_frames();
        if (s_frames_in_state > deadline_frames) {
            /* Report the derived deadline: with a config-dependent bound,
             * a bare "the nav deadline" line is unreadable in the field —
             * you cannot tell a wedge from a legitimately long maximal
             * config without the number that was actually enforced.
             *
             * The buffer must hold the whole line: this is the ONE line that
             * explains a nav-deadline abandonment, and a truncated tail drops
             * exactly the three numbers the comment above says are the point.
             * The literal alone is 227 bytes (clang-20 -Wformat-truncation on
             * the arm-linux-gnueabihf cross build measured it and, with
             * -Werror, refused the 192-byte buffer this started with), plus up
             * to three int conversions. 320 covers the literal and INT_MIN in
             * all three slots (227 - 6 for the "%d"s + 3 * 11 = 254) with room
             * to spare.
             *
             * This widening was derived independently THREE times (task #103,
             * task #67, and 86e812c0) against the same clang-20 diagnostic,
             * and all three landed on char[320]. The two surviving derivations
             * of the lower bound differ slightly in bookkeeping -- 227 - 6 +
             * 3*11 = 254 here, 227 + 3*10 + 1 = 258 in the #67 branch -- and
             * both sit well under 320, so the buffer is not sensitive to which
             * one you accept. Do not "simplify" 320 down to either figure:
             * the headroom is what lets the wording change without
             * reintroducing the truncation. */
            char line[320];
            snprintf(line, sizeof(line),
                     "[netplay-connect] FAIL code=P2P_FAIL_TIMEOUT_ORCHESTRATOR "
                     "stage=NAV_WAIT_ORCHESTRATOR — orchestrator produced neither a "
                     "session nor a terminal state within the nav deadline "
                     "(%d ms = orchestrator worst case %d ms + %d ms margin)",
                     (deadline_frames * 1000) / NAV_FPS,
                     DirectP2P_OrchWorstCaseMs(), NAV_ORCH_TIMEOUT_MARGIN_MS);
            Netplay_LogConnectEvent(line);
            transition_to(NAV_DONE);
        }
        break;
    }

    case NAV_START_NETPLAY:
        Netplay_BeginDirectP2P();
        transition_to(NAV_DONE);
        break;

    case NAV_IDLE:
    case NAV_DONE:
        /* Unreachable given the early-return above. */
        break;
    }
}
