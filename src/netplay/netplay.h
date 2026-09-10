#ifndef NETPLAY_H
#define NETPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration so netplay.h does not pull in <SDL3_net/SDL_net.h>.
 * Mirrors upstream /tmp/3sxtra/src/netplay/netplay.h typedef hygiene. */
struct NET_DatagramSocket;

typedef struct NetworkStats {
    int delay;
    int ping;
    int rollback;
} NetworkStats;

typedef enum NetplaySessionState {
    NETPLAY_SESSION_IDLE,
    NETPLAY_SESSION_TRANSITIONING,
    NETPLAY_SESSION_CONNECTING,
    NETPLAY_SESSION_RUNNING,
    NETPLAY_SESSION_EXITING,
} NetplaySessionState;

void Netplay_SetParams(int player, const char* ip);
// Override the remote UDP port chosen by Netplay_SetParams. Needed on the
// direct-P2P orchestrator path where the STUN-translated peer port is only
// known after hole-punch, not at SetParams time. See do_handoff() in
// src/netplay/direct_p2p.c.
void Netplay_SetRemotePort(unsigned short port);
// True once remote_ip has been wired via Netplay_SetParams (LAN/localhost
// path) OR via do_handoff() after the direct-P2P orchestrator completes
// its UPnP+STUN hole-punch. Used by netplay_nav to gate NAV_START_NETPLAY
// so the nav state machine doesn't call Netplay_BeginDirectP2P before
// remote_ip is populated.
bool Netplay_IsRemoteIpSet(void);
void Netplay_BeginDirectP2P();
void Netplay_TickDirectP2P();
// Step 6 (docs/plan-stun-direct-p2p.md): pre-punched STUN socket handoff.
// Mirrors upstream /tmp/3sxtra/src/netplay/netplay.c:808-814. Ownership
// transfers to netplay.c; destroyed on session teardown. Calling twice
// without an intervening teardown destroys the previously held socket
// before replacing it. Pass NULL to clear.
void Netplay_SetStunSocket(struct NET_DatagramSocket* socket);
// Task #119: hand over the S4a punch-auth token (STUN_PUNCH_TOKEN_LEN =
// 8 bytes, stun.h) alongside the punched socket, from do_handoff. It
// arms the late-punch rescue layer (late_punch.h) for the pre-session
// window so a peer whose race ended one-sided can still connect late.
// Pass (NULL, false) to clear; paths that never call this (LAN CLI) never
// arm the layer.
void Netplay_SetPunchToken(const uint8_t* token, bool valid);
// Step 6 (docs/plan-stun-direct-p2p.md, P-2 #18): register a single
// callback fired at the top of NETPLAY_SESSION_EXITING teardown, before
// the STUN socket is destroyed. Used by the direct-P2P orchestrator
// (Step 7) to release its UPnP mapping. Pass NULL to clear. The slot is
// not cleared automatically — the callback persists across sessions.
void Netplay_SetSessionTeardownCallback(void (*cb)(void));
// S3 (docs/plan-netplay-connection.md §5): append one line to the
// per-session netplay log (<pref>/logs/netplay-<utc_ms>.log), opening it
// lazily when no session has opened it yet — connection failures happen
// before configure_gekko() normally opens the file, and the attributed
// failure line + stage timings must survive to disk for field reports.
// Also tees to SDL_Log. MAIN THREAD ONLY.
void Netplay_LogConnectEvent(const char* line);
/* #36 — Arm the connect-log sink. Main thread, before any orchestrator
 * worker thread is spawned — that is the happens-before that lets worker
 * threads use Netplay_LogConnectEventMT without a race on the mutex
 * pointer. Idempotent; safe to call on every host/join attempt. */
void Netplay_LogSinkInit(void);
/* Stop and join the asynchronous heartbeat logger before SDL teardown. */
void Netplay_LogSinkShutdown(void);
/* #36 — Same as Netplay_LogConnectEvent but callable from ANY thread.
 * Every cascade diagnostic worth keeping lives on a direct_p2p worker
 * thread, and those used bare SDL_Log, so they never reached the
 * per-session file a tester actually sends us. */
void Netplay_LogConnectEventMT(const char* line);
#ifdef NETPLAY_TEST_HOOKS
#include "netplay/connect_fail.h" /* ConnectFailCode, for SessionFailCodeForEvent */
#include "gekkonet.h"             /* GekkoSessionEventType, for SessionFailCodeForEvent */
/* #44 test seams. Both are compiled out of the shipped build.
 *
 * LogPrune runs the production session-log prune against an arbitrary
 * directory, so the "what does it refuse to delete" contract can be tested
 * on a scratch directory instead of the user's real logs/.
 *
 * ReportDir redirects <PrefPath>logs/netplay-report.txt at a scratch
 * directory. Pass NULL or "" to restore the default. Without it the
 * rotation test would have to push 128 KB through, and then delete, the
 * one real artifact this feature exists to hand a tester. Closes any open
 * report first, so the next line reopens under the new directory. */
void Netplay_TestHook_LogPrune(const char* dir);
void Netplay_TestHook_ReportDir(const char* dir);
/* #125: the session log path THIS process opened, so a test never has
 * to guess it from the newest name in a shared directory. False (and
 * an empty string) when no session log is open. */
bool Netplay_TestHook_SessionLogPath(char* out, size_t cap);
/* Exercise the asynchronous heartbeat mailbox without a live Gekko session. */
void Netplay_TestHook_HeartbeatEnqueue(const char* line);
void Netplay_TestHook_HeartbeatDrain(void);
/* Task #144 review Item A: exposes process_session()'s pure event->code
 * mapping (session_fail_code_for_event, netplay.c) for the two RUNNING-
 * phase failure taxonomy codes. process_session() itself has no test seam
 * — driving a live GekkoPlayerDisconnected/GekkoDesyncDetected needs two
 * real UDP peers past the MIST handshake — so this pins the narrower,
 * testable claim: disconnect maps to CONNECT_FAIL_PEER_DISCONNECTED,
 * desync maps to CONNECT_FAIL_DESYNC_DETECTED. It does NOT pin that
 * process_session()'s handlers call DirectP2P_NotifySessionFailed at all
 * — see test 42/43's own comments for that residual. */
ConnectFailCode Netplay_TestHook_SessionFailCodeForEvent(GekkoSessionEventType type);
/* #145: pure predicates behind the deferred menu-exit (VS_Result case 7
 * runs inside the rollback simulation; the EXITING latch is deferred
 * until the requesting frame can no longer be rolled back, and canceled
 * by a load that rewinds past it). See netplay.c for the GekkoNet
 * 7be848c derivation of the confirmation bound. */
bool Netplay_TestHook_MenuExitConfirmed(int head_frame, int request_frame, int pred_window);
bool Netplay_TestHook_MenuExitErasedByLoad(int load_frame, int request_frame);
bool Netplay_TestHook_ShouldHoldLastFrame(NetplaySessionState state, int drawable_advances);
#endif
void Netplay_Run();
/* Set for the current outer frame when Gekko produced no drawable,
 * non-rollback AdvanceEvent. */
bool Netplay_ShouldHoldLastFrame(void);
NetplaySessionState Netplay_GetSessionState();
// S3: honest connect-phase progress text ("Verifying opponent (3s)...",
// "Syncing with opponent (7s)... START quits") for the netplay screen.
// Returns "" outside TRANSITIONING/CONNECTING. Never NULL.
const char* Netplay_GetConnectStatusText(void);
void Netplay_HandleMenuExit();

// Arm-time predicate: netplay arms ONLY in verified-arcade balance state
// (balance auto-selects at boot and is fixed for the process). Every
// session entry path — NetplayNav_Arm, the direct-P2P handoff dispatch,
// and the in-game network menu — must consult this before starting
// anything, and call Netplay_RefuseArm() on false, which
// logs and routes the human-readable reason to the direct-P2P overlay
// (the same surfacing mechanism the MIST handshake reject path uses).
bool Netplay_ArmAllowed(void);
void Netplay_RefuseArm(void);
void Netplay_GetNetworkStats(NetworkStats* stats);

// === Tier-1 netplay diag — Item 10: SIGTERM flush hook ===
// Called from src/main.c after the main loop exits but before SDL teardown
// so we capture diagnostics for wrapper-initiated SIGTERM scenarios. If
// no session was active, this is a no-op. If one was active, it:
//   - emits a final heartbeat-shaped line (best-effort, no per-second wait)
//   - dumps the per-packet ring buffer to <pref>/states/
//   - captures /proc/net/snmp UDP-row deltas
//   - flushes and closes the per-session netplay log file
// All file writes are guarded so a partial pref-path setup can't crash.
void Netplay_FlushDiagnostics(void);

#endif
