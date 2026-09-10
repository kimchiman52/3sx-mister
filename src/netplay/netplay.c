#include "netplay/netplay.h"
#include "arcade/arcade_balance.h"
#include "main.h"
#include "port/config/config.h"
#include "port/config/draw_players_above_hud.h"
#include "netplay/connect_fail.h"
#include "netplay/direct_p2p.h"
#include "netplay/late_punch.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "netplay/game_state.h"
#include "netplay/mist_handshake.h"
#include "netplay/net_tuning.h"
// #44: Rendezvous_WireVersion() for the report header — the wire version is
// a #define private to rendezvous.c, and duplicating the literal here is how
// a header field silently drifts away from what we actually speak.
#include "netplay/rendezvous.h"
#include "netplay/sdl_net_adapter.h"
#include "port/paths.h"
#include "port/sdl/sdl_app.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/grade.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/slowf.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/game.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/io/pulpul.h"
#include "sf33rd/Source/Game/menu/menu.h"
#include "sf33rd/Source/Game/rendering/color3rd.h"
#include "sf33rd/Source/Game/rendering/dc_ghost.h"
#include "sf33rd/Source/Game/rendering/mtrans.h"
#include "sf33rd/Source/Game/rendering/texcash.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/utils/djb2_hash.h"
#include "types.h"

#include <stdbool.h>

#include "gekkonet.h"
#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define INPUT_HISTORY_MAX 120
#define FRAME_SKIP_TIMER_MAX 60 // Allow skipping a frame roughly every second
#define STATS_UPDATE_TIMER_MAX 60
#define DELAY_FRAMES 1
#define PLAYER_COUNT 2

// Uncomment to enable packet drops
// #define LOSSY_ADAPTER

// S3: forward declaration — connect_abort_tick (defined with the S3 state
// above Netplay_Run's helpers) tears down via handle_disconnection.
static void handle_disconnection(void);

static GekkoSession* session = NULL;
static unsigned short local_port = 0;
static unsigned short remote_port = 0;
static char remote_ip_buf[64] = { 0 };
static const char* remote_ip = NULL;
static int player_number = 0;
static int player_handle = 0;
static NetplaySessionState session_state = NETPLAY_SESSION_IDLE;
static bool direct_p2p_pending = false;
static NET_DatagramSocket* p2p_sock = NULL;
// Step 6 (docs/plan-stun-direct-p2p.md): pre-punched STUN socket set by
// Netplay_SetStunSocket(). Mirrors upstream
// /tmp/3sxtra/src/netplay/netplay.c:79. Ownership sits with netplay.c
// after the setter is called; destroyed on session teardown.
static NET_DatagramSocket* stun_socket = NULL;
// Step 6 (plan P-2 #18): fired at the start of session teardown so the
// direct-P2P orchestrator can release its UPnP mapping while the STUN
// socket is still valid (the callback may inspect it via getsockname).
static void (*session_teardown_cb)(void) = NULL;
// Task #119: the S4a punch-auth token, handed over by do_handoff
// alongside the punched socket. While the pre-session window is open
// (TRANSITIONING + CONNECTING) it arms the late-punch layer
// (late_punch.h): the connected side keeps answering authenticated
// punches so a peer whose race ended one-sided — the residual
// split-brain bands — can still find us, instead of us waiting out
// CONNECT_TIMEOUT_CONNECTING_MS against a peer that already tore down
// and parking in FAILED_HANDSHAKE with the room code burned. Cleared
// with the rest of the session state on teardown.
static uint8_t s_punch_token[STUN_PUNCH_TOKEN_LEN];
static bool s_punch_token_valid = false;
// Task #119: a relearn arrived while a MIST pump attempt was mid-flight.
// s_hs_peer_addr is shared with the live attempt's io template, so it
// cannot be swapped under it; mist_pump_start honors this flag at the
// next attempt arm (<= one 500 ms attempt of latency, the pump's own
// budget) and re-resolves toward the updated endpoint.
static bool s_hs_peer_refetch = false;
static u16 input_history[2][INPUT_HISTORY_MAX] = { 0 };
static float frames_behind = 0;
static int frame_skip_timer = 0;
static int transition_ready_frames = 0;

static int stats_update_timer = 0;
static int frame_max_rollback = 0;
static NetworkStats network_stats = { 0 };
// Tracks the most recent non-rolling-back GekkoAdvanceEvent frame, used by
// the per-second heartbeat telemetry in update_network_stats().
static int s_last_advance_frame = 0;

// === Task #145: deferred menu-exit ===
// Netplay_HandleMenuExit is called from INSIDE the simulation (menu.c
// VS_Result case 7 runs under advance_game for predicted and replayed
// frames alike), but session_state lives outside GameState, so a
// rollback that erases the quit press cannot un-latch EXITING. The
// reachable failure is the rematch-confirm vs. quit-back-out race: the
// remote's rematch-confirm press (made while our rematch was still
// armed) is in flight when we disarm and quit inside the prediction
// window; the corrected timeline completes the rematch (VS_Result_Move_Sub
// r_no[2]=6) and the local quit press is never re-processed — but the
// speculative EXITING latch would tear the session down anyway, leaving
// the peer to rematch into GekkoNet's 5 s DISCONNECT_TIMEOUT.
//
// So a RUNNING-state exit is latched as a REQUEST stamped with the
// simulated frame, and Netplay_Run acts on it only once that frame can
// no longer be rolled back. A GekkoLoadEvent that rewinds past the
// request frame cancels it; if the corrected timeline still quits,
// VS_Result case 7 re-executes on the replay leg and re-latches.
//
// s_sim_frame is the frame advance_game is currently simulating (valid
// on replay legs too, unlike s_last_advance_frame which tracks only
// render frames). s_menu_exit_request_frame == -1 means no request.
// s_pred_window mirrors config.input_prediction_window for this session.
static int s_sim_frame = -1;
static int s_menu_exit_request_frame = -1;
static int s_pred_window = 8;

// Pure #145 predicates, extracted for unit testability (same shape as
// session_fail_code_for_event, task #144 review Item A).
//
// Confirmation bound, derived from GekkoNet @ 7be848c (the pinned ref,
// build-deps.sh GEKKONET_REF): predicted remote inputs are capped at
// input_prediction_window CONSECUTIVE frames (input.cpp ->
// HandleInputPrediction refuses once `window > diff` fails; event.cpp ->
// AddAdvanceEvent then returns false and the session stalls), and
// HandleRollback runs before every advance (game_session.cpp ->
// UpdateSession) and rewinds to min_incorrect - 1. A misprediction at
// frame <= R therefore cannot coexist with head >= R + window — the
// predicted run would have to span window + 1 frames — so once
// head - R >= window, frame R can never be re-simulated.
static bool menu_exit_request_confirmed(int head_frame, int request_frame, int pred_window) {
    return request_frame >= 0 && head_frame - request_frame >= pred_window;
}

// A load to frame L restores state AT L and re-advances from L + 1, so
// the request made while simulating frame R is erased iff L < R (L == R
// restores post-R state; R's own sim is not re-run).
static bool menu_exit_request_erased_by_load(int load_frame, int request_frame) {
    return request_frame >= 0 && load_frame < request_frame;
}

NetplayPostMatchAction Netplay_ResolvePostMatchAction(bool ready0, bool ready1,
                                                       bool rematch0, bool rematch1,
                                                       bool char_select0, bool char_select1,
                                                       bool exit0, bool exit1) {
    if (exit0 || exit1) {
        return NETPLAY_POST_MATCH_EXIT;
    }
    if (char_select0 || char_select1) {
        return NETPLAY_POST_MATCH_CHAR_SELECT;
    }
    if ((ready0 || rematch0) && (ready1 || rematch1)) {
        return NETPLAY_POST_MATCH_REMATCH;
    }
    return NETPLAY_POST_MATCH_NONE;
}

// === Netplay diagnostics state (gated by CFG_KEY_NETPLAY_DIAG_ENABLE) ===
// Captured at session start and used by:
//   - the per-second heartbeat (update_network_stats)
//   - the desync handler (process_session)
//   - the session-end log (Netplay_Run EXITING case)
//
// Refreshed inside configure_gekko() so two consecutive sessions in the
// same game process get distinct UUIDs and accurate UTCs.
static SDL_AtomicInt s_diag_enabled;
static uint32_t s_session_uuid = 0;
static uint64_t s_session_started_unix_ms = 0;
static uint64_t s_session_started_ticks_ms = 0;
// Reason buffer populated by callsites that initiate teardown (peer drop,
// desync, user cancel) and consumed by the EXITING-state session-end log.
static char s_session_end_reason[64] = { 0 };
static int s_session_end_frame = -1;

// === Tier-1 netplay diag — Item 3: previous-snapshot per-packet counters ===
// Item 3 keeps cumulative counters in sdl_net_adapter.c. The heartbeat
// reports deltas; we hold the previous snapshot here so update_network_stats
// can compute (cur - prev) once per second.
static uint64_t s_hb_prev_pkt_tx[8] = { 0 };
static uint64_t s_hb_prev_pkt_rx[8] = { 0 };

// === Tier-1 netplay diag — Item 5: GekkoAdvanceEvent watchdog ===
// Latched UTC ms of the most recent GekkoAdvanceEvent (Phase or rollback
// — both count). If it has been > 250ms since the last advance and we
// haven't yet reported this stall, emit a WATCHDOG line and latch the
// reported flag so we don't spam. Reset the latch on the next advance.
// 250ms ~= 15 frames at 60 Hz; healthy sessions never go that long
// without an advance even on a heavy rollback chain.
static uint64_t s_last_advance_utc_ms = 0;
static bool     s_watchdog_latched = false;

// === Tier-1 netplay diag — Item 6: rollback-depth bucket histogram ===
// Counts of advance batches by `frames_rolled_back` value, reset on each
// heartbeat snapshot. Buckets: 0, 1-2, 3-4, 5-7, 8+.
static uint32_t s_rb_buckets[5] = { 0 };

/* Gekko can return no AdvanceEvent when the prediction window is exhausted.
 * A batch containing only rollback advances is also intentionally non-drawing.
 * The renderer must retain its completed canvas for either case; rasterizing
 * the current queue would clear the canvas to opaque black first. */
static bool s_hold_last_frame = false;
static bool s_no_draw_episode = false;
static uint32_t s_hb_no_draw_holds = 0;
static uint32_t s_hb_catchups = 0;
static int s_last_no_draw_frame = -1;

typedef struct NetplayRunTrace {
    Uint64 session_ns;
    Uint64 update_ns;
    Uint64 load_ns;
    Uint64 advance_ns;
    Uint64 save_ns;
    int game_events;
    int advances;
    int rollback_advances;
    int drawable_advances;
    int loads;
    int saves;
} NetplayRunTrace;

static bool should_hold_last_frame(NetplaySessionState state, int drawable_advances) {
    return (state == NETPLAY_SESSION_CONNECTING || state == NETPLAY_SESSION_RUNNING) &&
           drawable_advances == 0;
}

// === Tier-1 netplay diag — Item 7: buffered netplay log file ===
// One FILE* opened at session start and closed in the EXITING branch of
// Netplay_Run. All netplay-tagged lines pass through netplay_logf() which
// tees to SDL_Log + the file.
static FILE* s_netplay_log = NULL;
// #125: the path netplay_log_open actually opened. A test that has to
// verify what this sink wrote used to locate the file by scanning the
// logs directory for the newest netplay-<ms>.log, which silently picks
// up ANOTHER process's session log whenever two builds run at once —
// see Netplay_TestHook_SessionLogPath below. Written on the main
// thread at open, read by tests on the main thread.
static char s_netplay_log_path[640] = { 0 };

// === #36: thread-safe connect-log sink ===
// Before #36 the FILE* above was documented MAIN THREAD ONLY and only
// Netplay_LogConnectEvent (main thread, from DirectP2P_Tick) ever wrote
// to it. Every cascade diagnostic in direct_p2p.c runs on a WORKER thread
// and used bare SDL_Log, so none of it reached the file a tester sends.
// Routing worker lines here needs one mutex; it is created once by
// Netplay_LogSinkInit on the main thread BEFORE any worker spawns, which
// is the happens-before that makes the pointer read below safe.
// NULL mutex => single-threaded era => lock/unlock are no-ops, so the
// pre-existing main-thread behaviour is unchanged even if Init never ran.
static SDL_Mutex* s_netplay_log_mu = NULL;

// Heartbeats are useful one-second health evidence, but a filesystem flush
// can block on the MiSTer's SD card. The game thread therefore hands each
// completed heartbeat to this one-slot, coalescing mailbox and never touches
// stderr or the session FILE for it. The logger thread owns that I/O. A
// heartbeat is diagnostic, so replacing an older pending row is preferable
// to letting gameplay wait for storage.
#define NETPLAY_HEARTBEAT_LINE_MAX 512
#define NETPLAY_HEARTBEAT_QUEUE_CAP 4
static SDL_Mutex* s_heartbeat_mu = NULL;
static SDL_Condition* s_heartbeat_cv = NULL;
static SDL_Thread* s_heartbeat_thread = NULL;
static bool s_heartbeat_accepting = false;
static unsigned s_heartbeat_head = 0;
static unsigned s_heartbeat_tail = 0;
static unsigned s_heartbeat_count = 0;
static bool s_heartbeat_inflight = false;
static bool s_heartbeat_stopping = false;
static char s_heartbeat_lines[NETPLAY_HEARTBEAT_QUEUE_CAP][NETPLAY_HEARTBEAT_LINE_MAX] = {{ 0 }};

// Bound ONE session file. A cascade that retries for minutes can emit a
// lot of lines and /media/fat is a shared SD card; past the budget the
// lines still go to SDL_Log, they just stop hitting the disk.
#define NETPLAY_LOG_MAX_BYTES (256u * 1024u)
static size_t s_netplay_log_bytes = 0;
static bool   s_netplay_log_truncated = false;

// === #44: session-log pruning + the ONE file a tester sends ===
//
// Nothing has ever removed a netplay-<utc_ms>.log. Measured on the
// development machine while writing this: 461 files / 87 MB in
// ~/Library/Application Support/CrowdedStreet/3S-ARM/logs/. On a MiSTer
// that directory lives on the SD card the whole system boots from
// (Paths_GetPrefPath() -> /media/fat/games/3s-arm/, src/port/paths.c:20-33),
// so unbounded growth is a device-health problem, not just clutter.
//
// Keep the newest NETPLAY_LOG_KEEP_FILES, ranked by the UTC-ms stamp the
// FILENAME carries — no stat(), so a directory copied between machines or
// restored from a backup still ranks correctly. Cap the scan so a hostile
// or merely enormous directory cannot make startup unbounded.
#define NETPLAY_LOG_KEEP_FILES 20
#define NETPLAY_LOG_PRUNE_SCAN_MAX 512

// The tester-facing report. <PrefPath>logs/netplay-report.txt carries ONLY
// the attributed "[netplay-connect]" one-liners, across launches, so the
// support request is "send me this one file" instead of "find the newest
// of 461 files". Exactly two generations, so the pair is hard-bounded at
// 2 * NETPLAY_REPORT_MAX_BYTES and stays pasteable into a chat message.
#define NETPLAY_REPORT_MAX_BYTES (64u * 1024u)
static FILE*  s_netplay_report = NULL;
static size_t s_netplay_report_bytes = 0;
// Defined below netplay_utc_ms (it needs the stamp); declared here because
// netplay_log_close, which is above that point, closes it.
static void netplay_report_close_locked(void);

static void netplay_log_lock(void) {
    if (s_netplay_log_mu != NULL) {
        SDL_LockMutex(s_netplay_log_mu);
    }
}

static void netplay_log_unlock(void) {
    if (s_netplay_log_mu != NULL) {
        SDL_UnlockMutex(s_netplay_log_mu);
    }
}

static void netplay_log_file_line(const char* line);
static void netplay_log_line(const char* line);
static bool heartbeat_enqueue(const char* line);

/* Optional frame-time diagnostics must never perform console or filesystem
 * I/O on the game thread.  The connection and session-end paths continue to
 * use netplay_log_lock() so their records remain lossless; frame diagnostics
 * are observational and may be dropped when the bounded logger queue fills. */
static bool netplay_log_line_try(const char* line) {
    if (line == NULL || line[0] == '\0') {
        return false;
    }
    return heartbeat_enqueue(line);
}

void Netplay_LogGameplayDiagnostic(const char* line) {
    (void)netplay_log_line_try(line);
}

void Netplay_LogGameplayDiagnosticf(const char* fmt, ...) {
    char line[512];
    va_list args;

    if (fmt == NULL) {
        return;
    }
    va_start(args, fmt);
    SDL_vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    Netplay_LogGameplayDiagnostic(line);
}

static int heartbeat_writer_thread(void* unused) {
    (void)unused;

    for (;;) {
        char line[NETPLAY_HEARTBEAT_LINE_MAX];

        SDL_LockMutex(s_heartbeat_mu);
        while (s_heartbeat_count == 0 && !s_heartbeat_stopping) {
            SDL_WaitCondition(s_heartbeat_cv, s_heartbeat_mu);
        }
        if (s_heartbeat_count == 0 && s_heartbeat_stopping) {
            SDL_UnlockMutex(s_heartbeat_mu);
            return 0;
        }

        SDL_strlcpy(line, s_heartbeat_lines[s_heartbeat_head], sizeof(line));
        s_heartbeat_head = (s_heartbeat_head + 1) % NETPLAY_HEARTBEAT_QUEUE_CAP;
        s_heartbeat_count--;
        s_heartbeat_inflight = true;
        SDL_UnlockMutex(s_heartbeat_mu);

        // This is deliberately the only live-session path that flushes a
        // heartbeat. It may wait on SD or a redirected stderr sink, but it
        // never runs on the game thread.
        fputs(line, stderr);
        fputc('\n', stderr);
        fflush(stderr);

        netplay_log_lock();
        netplay_log_file_line(line);
        if (s_netplay_log != NULL) {
            fflush(s_netplay_log);
        }
        netplay_log_unlock();

        SDL_LockMutex(s_heartbeat_mu);
        s_heartbeat_inflight = false;
        SDL_SignalCondition(s_heartbeat_cv);
        SDL_UnlockMutex(s_heartbeat_mu);
    }
}

// Main thread only: retire live heartbeat work before replacing or closing
// the FILE it targets. Waiting here is safe: session transitions and program
// shutdown are already non-realtime, unlike active gameplay.
static void heartbeat_disable_and_drain(void) {
    if (s_heartbeat_mu == NULL || s_heartbeat_thread == NULL) {
        return;
    }

    SDL_LockMutex(s_heartbeat_mu);
    s_heartbeat_accepting = false;
    SDL_SignalCondition(s_heartbeat_cv);
    while (s_heartbeat_count != 0 || s_heartbeat_inflight) {
        SDL_WaitCondition(s_heartbeat_cv, s_heartbeat_mu);
    }
    SDL_UnlockMutex(s_heartbeat_mu);
}

// Main thread only, after netplay_log_open() has installed the session FILE.
static void heartbeat_enable(void) {
    if (s_heartbeat_mu == NULL || s_heartbeat_thread == NULL) {
        return;
    }

    SDL_LockMutex(s_heartbeat_mu);
    s_heartbeat_head = 0;
    s_heartbeat_tail = 0;
    s_heartbeat_count = 0;
    s_heartbeat_accepting = true;
    SDL_UnlockMutex(s_heartbeat_mu);
}

// Nonblocking game-thread producer. If the mailbox is briefly held while
// the logger copies a row, skip this diagnostic sample rather than wait.
static bool heartbeat_enqueue(const char* line) {
    if (line == NULL || line[0] == '\0' || s_heartbeat_mu == NULL ||
        s_heartbeat_thread == NULL || !SDL_TryLockMutex(s_heartbeat_mu)) {
        return false;
    }

    bool accepted = false;
    if (s_heartbeat_accepting && !s_heartbeat_stopping &&
        s_heartbeat_count < NETPLAY_HEARTBEAT_QUEUE_CAP) {
        SDL_strlcpy(s_heartbeat_lines[s_heartbeat_tail], line,
                    sizeof(s_heartbeat_lines[s_heartbeat_tail]));
        s_heartbeat_tail = (s_heartbeat_tail + 1) % NETPLAY_HEARTBEAT_QUEUE_CAP;
        s_heartbeat_count++;
        SDL_SignalCondition(s_heartbeat_cv);
        accepted = true;
    }
    SDL_UnlockMutex(s_heartbeat_mu);
    return accepted;
}

void Netplay_LogSinkInit(void) {
    if (s_netplay_log_mu == NULL) {
        s_netplay_log_mu = SDL_CreateMutex();
    }
    if (s_heartbeat_mu == NULL) {
        s_heartbeat_mu = SDL_CreateMutex();
    }
    if (s_heartbeat_cv == NULL && s_heartbeat_mu != NULL) {
        s_heartbeat_cv = SDL_CreateCondition();
    }
    if (s_heartbeat_thread == NULL && s_heartbeat_mu != NULL && s_heartbeat_cv != NULL) {
        s_heartbeat_stopping = false;
        s_heartbeat_thread = SDL_CreateThread(heartbeat_writer_thread,
                                              "NetplayHeartbeatLog", NULL);
        if (s_heartbeat_thread == NULL) {
            SDL_DestroyCondition(s_heartbeat_cv);
            s_heartbeat_cv = NULL;
            SDL_DestroyMutex(s_heartbeat_mu);
            s_heartbeat_mu = NULL;
        }
    }
}

void Netplay_LogSinkShutdown(void) {
    heartbeat_disable_and_drain();
    if (s_heartbeat_mu == NULL || s_heartbeat_thread == NULL) {
        return;
    }

    SDL_LockMutex(s_heartbeat_mu);
    s_heartbeat_stopping = true;
    SDL_SignalCondition(s_heartbeat_cv);
    SDL_UnlockMutex(s_heartbeat_mu);
    SDL_WaitThread(s_heartbeat_thread, NULL);
    s_heartbeat_thread = NULL;
    SDL_DestroyCondition(s_heartbeat_cv);
    s_heartbeat_cv = NULL;
    SDL_DestroyMutex(s_heartbeat_mu);
    s_heartbeat_mu = NULL;
}

#ifdef NETPLAY_TEST_HOOKS
void Netplay_TestHook_HeartbeatEnqueue(const char* line) {
    Netplay_LogSinkInit();
    heartbeat_enable();
    heartbeat_enqueue(line);
}

void Netplay_TestHook_HeartbeatDrain(void) {
    heartbeat_disable_and_drain();
}
#endif

// === Tier-1 netplay diag — Item 9: /proc/net/snmp UDP-drop snapshots ===
// Captured at session start, on peer-disconnect, and at session end. The
// heartbeat doesn't sample this — it's relatively expensive (file read +
// parse) and only useful for before/after deltas.
typedef struct UdpSnmp {
    bool valid;
    uint64_t in_errors;
    uint64_t rcvbuf_errors;
    uint64_t no_ports;
} UdpSnmp;
static UdpSnmp s_udp_snmp_session_start = { 0 };

// Read /proc/net/snmp and pull the InErrors, RcvbufErrors, NoPorts fields
// out of the UDP row. Returns false (and leaves *out untouched apart from
// `valid=false`) on any parse failure or if the file doesn't exist (e.g.
// non-MiSTer host machines don't ship /proc/net/snmp). The format is two
// lines per protocol: a header with field names followed by a values line.
static bool read_proc_net_snmp_udp(UdpSnmp* out) {
    if (out == NULL) {
        return false;
    }
    out->valid = false;
    FILE* f = fopen("/proc/net/snmp", "r");
    if (f == NULL) {
        return false;
    }
    char header[1024];
    char values[1024];
    bool got = false;
    while (fgets(header, sizeof(header), f) != NULL) {
        if (SDL_strncmp(header, "Udp:", 4) != 0) {
            continue;
        }
        if (fgets(values, sizeof(values), f) == NULL) {
            break;
        }
        if (SDL_strncmp(values, "Udp:", 4) != 0) {
            // /proc/net/snmp pairs Udp: header / Udp: values lines.
            // If the second line isn't a values line, give up.
            break;
        }
        // Find indices of InErrors, RcvbufErrors, NoPorts in the header
        // tokens. Token 0 is the literal "Udp:" prefix.
        int idx_inerr = -1, idx_rcvbuf = -1, idx_noports = -1;
        int hdr_n = 0;
        char* save_h = NULL;
        for (char* tok = SDL_strtok_r(header, " \t\r\n", &save_h);
             tok != NULL && hdr_n < 64;
             tok = SDL_strtok_r(NULL, " \t\r\n", &save_h)) {
            if (SDL_strcmp(tok, "InErrors") == 0)      idx_inerr = hdr_n;
            else if (SDL_strcmp(tok, "RcvbufErrors") == 0) idx_rcvbuf = hdr_n;
            else if (SDL_strcmp(tok, "NoPorts") == 0)     idx_noports = hdr_n;
            hdr_n++;
        }
        // Tokenize values; pull by index.
        char* val_tokens[64];
        int val_n = 0;
        char* save_v = NULL;
        for (char* tok = SDL_strtok_r(values, " \t\r\n", &save_v);
             tok != NULL && val_n < 64;
             tok = SDL_strtok_r(NULL, " \t\r\n", &save_v)) {
            val_tokens[val_n++] = tok;
        }
        if (idx_inerr   >= 0 && idx_inerr   < val_n) out->in_errors      = (uint64_t)SDL_strtoull(val_tokens[idx_inerr],   NULL, 10);
        if (idx_rcvbuf  >= 0 && idx_rcvbuf  < val_n) out->rcvbuf_errors  = (uint64_t)SDL_strtoull(val_tokens[idx_rcvbuf],  NULL, 10);
        if (idx_noports >= 0 && idx_noports < val_n) out->no_ports       = (uint64_t)SDL_strtoull(val_tokens[idx_noports], NULL, 10);
        out->valid = (idx_inerr >= 0 || idx_rcvbuf >= 0 || idx_noports >= 0);
        got = true;
        break;
    }
    fclose(f);
    return got;
}

// Tee writer: the heartbeat and event lines go to both the system log
// (via SDL_Log so existing log surfaces still receive them) and the
// per-session netplay log file. Cheap — one fwrite per call once the
// file is open.
// CALLER HOLDS netplay_log_lock() when it can race (every #36 MT path).
// The SDL_Log tee is deliberately inside the lock too: SDL_Log itself is
// thread-safe, but keeping it inside keeps the two sinks in the same
// order, which is what makes a field log readable.
//
// F4 SPLIT: the FILE half is factored out so a caller that already owns
// its own console surface (the 1 Hz heartbeat writes to stderr by hand
// for tail -f workflows) can get the byte budget WITHOUT a second copy
// of every line appearing via SDL_Log. Before this split that caller
// wrote the FILE* raw and so escaped the budget entirely — it kept
// growing the session file after the TRUNCATED marker had been
// published, which made the marker a lie.
// CALLER HOLDS netplay_log_lock().
static void netplay_log_file_line(const char* line) {
    if (s_netplay_log == NULL) {
        return;
    }
    // #36 byte budget: bound ONE session file. `+1` is the newline.
    const size_t want = SDL_strlen(line) + 1u;
    if (s_netplay_log_bytes + want > (size_t)NETPLAY_LOG_MAX_BYTES) {
        if (!s_netplay_log_truncated) {
            s_netplay_log_truncated = true;
            char trunc[160];
            // %lu, not %zu: SDL_snprintf's own formatter is the one that
            // runs here and the codebase never relies on its 'z' support.
            SDL_snprintf(trunc, sizeof(trunc),
                         "[netplay-log] TRUNCATED at %lu bytes — further lines go to "
                         "SDL_Log only",
                         (unsigned long)s_netplay_log_bytes);
            fputs(trunc, s_netplay_log);
            fputc('\n', s_netplay_log);
            fflush(s_netplay_log);
        }
        return;
    }
    fputs(line, s_netplay_log);
    fputc('\n', s_netplay_log);
    s_netplay_log_bytes += want;
}

// The full tee: system log + the budgeted file write above.
// CALLER HOLDS netplay_log_lock() when it can race (every #36 MT path).
static void netplay_log_line(const char* line) {
    SDL_Log("%s", line);
    netplay_log_file_line(line);
}

// #44: parse "netplay-<digits>.log" -> the UTC-ms stamp the name carries.
// Returns 0 for ANY name that is not exactly that shape, and 0 is also the
// "not a candidate" sentinel — a real stamp is a wall-clock millisecond
// count, so it is never 0 on any machine whose clock is past 1970.
//
// This predicate is the entire safety argument for the prune below, so it
// is deliberately strict on all three axes:
//   - "netplay-report.txt"   fails the ".log" suffix test;
//   - "netplay-report.1.txt" fails it too, and "report.1" is not digits;
//   - "keepme.txt"           fails the "netplay-" prefix test.
// The >20-digit rejection keeps the name length bounded (8 + 20 + 4 = 32)
// AND rejects the only inputs that could overflow the unsigned long long
// the ranking compares.
static unsigned long long netplay_log_name_stamp(const char* name) {
    static const char k_prefix[] = "netplay-";
    static const char k_suffix[] = ".log";
    const size_t plen = sizeof(k_prefix) - 1u;
    const size_t slen = sizeof(k_suffix) - 1u;
    const size_t nlen = SDL_strlen(name);
    if (nlen <= plen + slen) {
        return 0;
    }
    if (SDL_strncmp(name, k_prefix, plen) != 0) {
        return 0;
    }
    if (SDL_strcmp(name + nlen - slen, k_suffix) != 0) {
        return 0;
    }
    const size_t digits = nlen - slen - plen;
    if (digits > 20u) {
        return 0;
    }
    for (size_t i = plen; i < nlen - slen; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return 0;
        }
    }
    return SDL_strtoull(name + plen, NULL, 10);
}

// #44: keep the NETPLAY_LOG_KEEP_FILES newest session logs in `logs_dir`,
// delete the rest. Called from netplay_log_open right after the directory
// is created, i.e. once per session at most.
//
// Deliberate properties:
//   - ONE opendir pass. Names are snapshotted into a heap array and every
//     remove() happens AFTER closedir, because POSIX leaves the effect of
//     unlinking during an active readdir loop unspecified.
//   - Never recurses, never removes a directory, never touches a name
//     netplay_log_name_stamp rejects. remove() on a directory fails with
//     EISDIR anyway, but the name filter means we never even try.
//   - Bounded: at most NETPLAY_LOG_PRUNE_SCAN_MAX entries are considered,
//     so the cost of a startup in a pathological directory is capped.
//   - Fails open. Any allocation or opendir failure means "prune nothing";
//     losing the prune is a disk-space problem, deleting the wrong thing
//     is a data-loss problem.
typedef struct NetplayLogEnt {
    unsigned long long ts;
    char               name[36]; // >= 32 + NUL, see netplay_log_name_stamp
} NetplayLogEnt;

static void netplay_log_prune(const char* logs_dir) {
    if (logs_dir == NULL || logs_dir[0] == '\0') {
        return;
    }
    DIR* d = opendir(logs_dir);
    if (d == NULL) {
        return;
    }
    NetplayLogEnt* ents =
        (NetplayLogEnt*)SDL_malloc(sizeof(NetplayLogEnt) * NETPLAY_LOG_PRUNE_SCAN_MAX);
    if (ents == NULL) {
        closedir(d);
        return;
    }

    // Top-N stamps, kept sorted descending, so the cutoff is available
    // without sorting the whole directory.
    unsigned long long keep[NETPLAY_LOG_KEEP_FILES];
    int nkeep = 0;
    int nents = 0;
    int examined = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && examined < NETPLAY_LOG_PRUNE_SCAN_MAX) {
        examined++;
        const unsigned long long ts = netplay_log_name_stamp(e->d_name);
        if (ts == 0) {
            continue;
        }
        if (nents < NETPLAY_LOG_PRUNE_SCAN_MAX) {
            ents[nents].ts = ts;
            SDL_strlcpy(ents[nents].name, e->d_name, sizeof(ents[nents].name));
            nents++;
        }
        if (nkeep < NETPLAY_LOG_KEEP_FILES) {
            int i = nkeep++;
            while (i > 0 && keep[i - 1] < ts) {
                keep[i] = keep[i - 1];
                i--;
            }
            keep[i] = ts;
        } else if (ts > keep[NETPLAY_LOG_KEEP_FILES - 1]) {
            int i = NETPLAY_LOG_KEEP_FILES - 1;
            while (i > 0 && keep[i - 1] < ts) {
                keep[i] = keep[i - 1];
                i--;
            }
            keep[i] = ts;
        }
    }
    closedir(d);

    if (nkeep < NETPLAY_LOG_KEEP_FILES) {
        SDL_free(ents);
        return; // fewer candidates than we keep — nothing to do
    }
    const unsigned long long cutoff = keep[NETPLAY_LOG_KEEP_FILES - 1];
    int removed = 0;
    for (int i = 0; i < nents; i++) {
        if (ents[i].ts >= cutoff) {
            continue;
        }
        char path[768];
        SDL_snprintf(path, sizeof(path), "%s/%s", logs_dir, ents[i].name);
        if (remove(path) == 0) {
            removed++;
        }
    }
    SDL_free(ents);
    if (removed > 0) {
        SDL_Log("[netplay-log] pruned %d old session log(s), kept the %d newest",
                removed, NETPLAY_LOG_KEEP_FILES);
    }
}

#ifdef NETPLAY_TEST_HOOKS
void Netplay_TestHook_LogPrune(const char* dir) {
    netplay_log_prune(dir);
}

/* #125. Hands back the path netplay_log_open opened for THIS process.
 *
 * The alternative a test has is to scan <PrefPath>logs for the newest
 * netplay-<ms>.log, and that is wrong the moment a second build is
 * running: the newest file in a shared directory belongs to whoever
 * opened it last, not to us. Measured with four concurrent
 * --test-connect-observability runs — three processes wrote to
 * netplay-...761.log and all three then read back netplay-...763.log,
 * reporting "800 of 800 MT lines missing or torn" about a file they had
 * never written a line to.
 *
 * Empty string when no session log is open. */
bool Netplay_TestHook_SessionLogPath(char* out, size_t cap) {
    if (out == NULL || cap == 0) {
        return false;
    }
    SDL_strlcpy(out, s_netplay_log_path, cap);
    return out[0] != '\0';
}
#endif

// Opens <pref>/logs/netplay-<utc_ms>.log for the active session. Block
// buffering keeps non-heartbeat event writes cheap; heartbeats are flushed by
// heartbeat_writer_thread(), away from the game thread. The filename is
// announced via SDL_Log at open time so post-mortem readers can find it
// quickly.
static void netplay_log_open(uint64_t utc_ms) {
    if (s_netplay_log != NULL) {
        return;
    }
    const char* pref = Paths_GetPrefPath();
    if (pref == NULL || pref[0] == '\0') {
        return;
    }
    char logs_dir[512];
    SDL_snprintf(logs_dir, sizeof(logs_dir), "%slogs", pref);
    SDL_CreateDirectory(logs_dir);
    // #44: prune BEFORE creating this session's file, so the file we are
    // about to open is never a prune candidate for its own call.
    netplay_log_prune(logs_dir);
    char path[640];
    SDL_snprintf(path, sizeof(path), "%s/netplay-%llu.log",
                 logs_dir, (unsigned long long)utc_ms);
    s_netplay_log = fopen(path, "w");
    // #36: a fresh file gets a fresh budget. Reset unconditionally so a
    // failed fopen cannot leave a stale "already truncated" latch behind.
    s_netplay_log_bytes = 0;
    s_netplay_log_truncated = false;
    SDL_strlcpy(s_netplay_log_path, (s_netplay_log != NULL) ? path : "",
                sizeof(s_netplay_log_path));
    if (s_netplay_log != NULL) {
        // Block-buffered; heartbeat_writer_thread() drives its flush.
        setvbuf(s_netplay_log, NULL, _IOFBF, 4096);
        SDL_Log("[netplay sess=%08x] netplay log file: %s",
                s_session_uuid, path);
    } else {
        SDL_Log("[netplay sess=%08x] failed to open netplay log: %s",
                s_session_uuid, path);
    }
}

static void netplay_log_close(void) {
    if (s_netplay_log != NULL) {
        fflush(s_netplay_log);
        fclose(s_netplay_log);
        s_netplay_log = NULL;
    }
    // #44: the tester report is opened and closed on exactly the same
    // paths as the session log, so there is no way to add a close for one
    // and forget the other.
    netplay_report_close_locked();
}

// UTC-ms helper used by the watchdog (Item 5), packet-ring (Item 4), and
// session-start/-end logs. P-1.1 fix: use clock_gettime(CLOCK_REALTIME)
// for full millisecond precision so timestamps cross-correlate exactly
// with sdl_net_adapter.c's now_utc_ms (same source). On Linux this is a
// vDSO-fast read with no syscall, identical perf profile to time(NULL).
static inline uint64_t netplay_utc_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

// ======================================================================
// #44 — <PrefPath>logs/netplay-report.txt, the ONE file a tester sends
// ======================================================================
//
// The support loop today is: "which of your 461 netplay-<ms>.log files is
// the one where it failed?" This file answers that by construction. It is
// append-mode across launches, it carries ONLY the attributed
// "[netplay-connect]" one-liners (the OK / FAIL / NOTE / ADVISORY /
// PORTMAP / DEADLINE / ABORT lines), and it is hard-bounded so it stays
// small enough to paste into a chat message.
//
// !! DEPLOY DEPENDENCY — RESOLVED, BUT KNOW WHY IT WAS HERE !!
// This warning used to say a redeploy DELETES the whole logs/ directory,
// this file included, because all three copies of the rsync preserve list
// in tools/mister/mister-common.sh omitted "logs". That was true when #44
// was written and it is no longer true: task #103 merged
// fix/tools-safety-93-90 (f8b29ded), which adds "logs" to the preserved
// set. `grep -n logs tools/mister/mister-common.sh` now answers
// mister-common.sh:648 ('logs' in the preserve list) and :1167 (the
// remote mkdir), so `rsync -av --delete` in mister_rsync_deploy leaves
// this file alone.
//
// Kept rather than deleted because the ordering it teaches is still the
// safe habit, and because if anyone ever drops "logs" from that list again
// the evidence this file exists to collect goes silently. If line 648 stops
// naming 'logs', this warning becomes true again.
//
// Every function below is "_locked": the caller holds netplay_log_lock().
// That is not decoration — the report is written from inside the same
// critical section as the session log (netplay_log_connect_event), which
// is what makes it thread-safe for free and what makes it impossible for
// the two files to disagree about what happened.

// MIST_BUILD_HASH is NOT a global compile definition: CMakeLists.txt:251-254
// attaches it with set_property(SOURCE ...) to a specific source list. This
// TU is on that list (see the same block), but the #ifdef stays so that a
// build system change downgrades the header field to "unknown" instead of
// failing to compile.
#ifdef MIST_BUILD_HASH
#define NETPLAY_REPORT_BUILD_HASH MIST_BUILD_HASH
#else
#define NETPLAY_REPORT_BUILD_HASH "unknown"
#endif

#ifdef NETPLAY_TEST_HOOKS
// Test-only redirect. Without it the rotation test would have to write
// 128 KB through — and then delete — the REAL user-facing report file in
// PrefPath, which is the one artifact this feature exists to preserve.
static char s_netplay_report_dir_override[512] = { 0 };
#endif

// Directory that holds netplay-report.txt. False when there is no usable
// PrefPath, in which case the report is simply not written (the session
// log makes the same call, netplay.c netplay_log_open).
static bool netplay_report_dir(char* out, size_t cap) {
#ifdef NETPLAY_TEST_HOOKS
    if (s_netplay_report_dir_override[0] != '\0') {
        SDL_strlcpy(out, s_netplay_report_dir_override, cap);
        return true;
    }
#endif
    const char* pref = Paths_GetPrefPath();
    if (pref == NULL || pref[0] == '\0') {
        return false;
    }
    SDL_snprintf(out, cap, "%slogs", pref);
    return true;
}

static void netplay_report_paths(const char* dir,
                                 char* live, size_t live_cap,
                                 char* prev, size_t prev_cap) {
    SDL_snprintf(live, live_cap, "%s/netplay-report.txt", dir);
    SDL_snprintf(prev, prev_cap, "%s/netplay-report.1.txt", dir);
}

static size_t netplay_report_file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    size_t n = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long end = ftell(f);
        if (end > 0) {
            n = (size_t)end;
        }
    }
    fclose(f);
    return n;
}

static void netplay_report_close_locked(void) {
    if (s_netplay_report != NULL) {
        fflush(s_netplay_report);
        fclose(s_netplay_report);
        s_netplay_report = NULL;
    }
    s_netplay_report_bytes = 0;
}

static void netplay_report_open_locked(void) {
    if (s_netplay_report != NULL) {
        return;
    }
    char dir[512];
    if (!netplay_report_dir(dir, sizeof(dir))) {
        return;
    }
    SDL_CreateDirectory(dir);
    char live[640];
    char prev[640];
    netplay_report_paths(dir, live, sizeof(live), prev, sizeof(prev));

    // Cross-launch rotation: a file that already reached the cap is
    // demoted now rather than being appended to forever. Exactly two
    // generations, so the pair can never exceed 2 * the cap.
    size_t existing = netplay_report_file_size(live);
    if (existing >= (size_t)NETPLAY_REPORT_MAX_BYTES) {
        remove(prev); // rename() replaces on POSIX; explicit is clearer
        if (rename(live, prev) == 0) {
            existing = 0;
        }
    }

    s_netplay_report = fopen(live, "a");
    s_netplay_report_bytes = existing;
    if (s_netplay_report == NULL) {
        s_netplay_report_bytes = 0;
        return;
    }
    setvbuf(s_netplay_report, NULL, _IOLBF, 512);

    // One header per open. build= identifies the binary, rend_ver= the
    // rendezvous wire version we speak — the single most useful field
    // when the symptom is "zero frames", because a version-mismatched
    // server drops our frame with no reply and looks exactly like an
    // unreachable one.
    char hdr[256];
    SDL_snprintf(hdr, sizeof(hdr),
                 "=== 3S-ARM netplay report build=%s rend_ver=%d utc_ms=%llu ===",
                 NETPLAY_REPORT_BUILD_HASH,
                 Rendezvous_WireVersion(),
                 (unsigned long long)netplay_utc_ms());
    fputs(hdr, s_netplay_report);
    fputc('\n', s_netplay_report);
    s_netplay_report_bytes += SDL_strlen(hdr) + 1u;
    fflush(s_netplay_report);
}

// Demote the live file and start a fresh one. Separate from the open-time
// check on purpose: mid-run we rotate when the NEXT line would cross the
// cap, so the live file is under the cap at the moment we rotate it and
// the open-time ">= cap" test would decline to act.
static void netplay_report_rotate_locked(void) {
    char dir[512];
    netplay_report_close_locked();
    if (!netplay_report_dir(dir, sizeof(dir))) {
        return;
    }
    char live[640];
    char prev[640];
    netplay_report_paths(dir, live, sizeof(live), prev, sizeof(prev));
    remove(prev);
    rename(live, prev);
    netplay_report_open_locked();
}

// The filter that keeps this file worth sending: ONLY "[netplay-connect]"
// lines. The heartbeat, the race spam and the kernel-UDP snapshots all
// pass through the same sink and all belong in the session log, not here.
static void netplay_report_line_locked(const char* line) {
    static const char k_tag[] = "[netplay-connect]";
    if (line == NULL || SDL_strncmp(line, k_tag, sizeof(k_tag) - 1u) != 0) {
        return;
    }
    netplay_report_open_locked();
    if (s_netplay_report == NULL) {
        return;
    }
    // Stamped, because this file spans launches and sessions: without an
    // absolute time there is no way to line a report line up against the
    // session log, the wrapper log, or the tester's description.
    char stamped[1024];
    SDL_snprintf(stamped, sizeof(stamped), "%llu %s",
                 (unsigned long long)netplay_utc_ms(), line);
    const size_t want = SDL_strlen(stamped) + 1u;
    if (s_netplay_report_bytes + want > (size_t)NETPLAY_REPORT_MAX_BYTES) {
        netplay_report_rotate_locked();
        if (s_netplay_report == NULL) {
            return;
        }
    }
    fputs(stamped, s_netplay_report);
    fputc('\n', s_netplay_report);
    s_netplay_report_bytes += want;
    fflush(s_netplay_report); // a crash right after a FAIL line must not lose it
}

#ifdef NETPLAY_TEST_HOOKS
void Netplay_TestHook_ReportDir(const char* dir) {
    netplay_log_lock();
    netplay_report_close_locked();
    if (dir == NULL || dir[0] == '\0') {
        s_netplay_report_dir_override[0] = '\0';
    } else {
        SDL_strlcpy(s_netplay_report_dir_override, dir,
                    sizeof(s_netplay_report_dir_override));
    }
    netplay_log_unlock();
}
#endif

// R-1: Layer 3 MIST handshake gate. Formerly conditioned on a
// s_lobby_session flag that no live code path ever set (the RmlUi lobby
// UI was removed), which made the handshake dead code on every real
// session — two peers with different GameState layouts would connect,
// sync, and desync mid-match with no explanation. The gate now runs
// unconditionally for every session (direct P2P, LAN CLI) on the same
// socket configure_gekko() hands to GekkoNet.
static bool s_mist_handshake_done = false;
static char s_mist_reject_reason[128] = { 0 };
// Consecutive silent-timeout attempts. Retry policy (peer-skew
// tolerance, 40 x 500 ms cap) lives with MIST_HANDSHAKE_MAX_ATTEMPTS /
// mist_handshake_gate_next in mist_handshake.h — extracted there so it
// is unit-testable (adv-review M-5).
static int s_mist_handshake_attempts = 0;
// R-1 adv-review H-1: session-scoped latch — "the session peer's hello
// passed the full compatibility check this session". Arms the runner's
// implicit-completion path (Gekko-shaped traffic from a classified-clean
// peer completes the handshake; see mist_handshake_run_attempt docs for
// the stranded-completion race and the bypass-safety argument). Must
// persist ACROSS the retry attempts of one session and reset wherever
// s_mist_handshake_attempts resets: session start (direct-P2P tick)
// and session EXITING.
static bool s_mist_peer_hello_ok = false;

// === S3 Part A (docs/plan-netplay-connection.md §5): CONNECTING deadline
// + user-reachable abort + live connect-status text ===
//
// NETPLAY_SESSION_CONNECTING had NO timeout: exit required a
// GekkoPlayerConnected/SessionStarted event, and GekkoNet's own
// DISCONNECT_TIMEOUT (5000 ms) applies only to actors already Connected —
// an actor stuck Initiating retries SendSyncRequest forever. The netplay
// watchdog (process_session) also only fires while RUNNING, so CONNECTING
// was entirely unwatched. We do NOT patch GekkoNet; instead:
//   - a wall-clock deadline (CONNECT_TIMEOUT_CONNECTING_MS = 15 s) bounds
//     the state and exits with an attributable reason
//     (P2P_FAIL_TIMEOUT_CONNECTING via DirectP2P_NotifySessionFailed);
//   - a 5 s progress log line makes the state log-visible (watchdog gap);
//   - holding START for CONNECT_ABORT_HOLD_FRAMES (~3 s) aborts BOTH the
//     TRANSITIONING (MIST handshake retry window) and CONNECTING states,
//     so nobody is stuck watching a frozen screen;
//   - s_connect_status carries honest progress text for
//     NetplayScreen_Render (replacing the perpetual "Match found!").
// All main-thread (Netplay_Run).
static uint64_t s_connecting_since_ms = 0;
static uint64_t s_connecting_last_log_ms = 0;
static int s_abort_hold_frames = 0;
static char s_connect_status[96] = { 0 };

// Shared abort handler for TRANSITIONING/CONNECTING: counts consecutive
// frames with START held on either pad; on the threshold, logs an
// attributed line and tears the session down as a user cancel (attract
// screen, no ERROR overlay). Returns true when the abort fired and the
// session is now EXITING.
static bool connect_abort_tick(const char* stage) {
    const bool start_down = ((p1sw_buff | p2sw_buff) & SWK_START) != 0;
    s_abort_hold_frames = ConnectFail_AbortHoldTick(s_abort_hold_frames, start_down);
    if (!ConnectFail_AbortHoldFired(s_abort_hold_frames)) {
        return false;
    }
    s_abort_hold_frames = 0;
    char line[192];
    SDL_snprintf(line, sizeof(line),
                 "[netplay-connect] ABORT code=%s stage=%s — user held START "
                 "through the %d-frame window",
                 ConnectFail_Code(CONNECT_FAIL_USER_ABORT), stage,
                 (int)CONNECT_ABORT_HOLD_FRAMES);
    Netplay_LogConnectEvent(line);
    if (s_session_end_reason[0] == '\0') {
        SDL_strlcpy(s_session_end_reason, "user-abort-connect", sizeof(s_session_end_reason));
        s_session_end_frame = s_last_advance_frame;
    }
    handle_disconnection();
    return true;
}

// First-to-X (number of game wins required to close a session).
// Placeholder for Track C / lobby FT negotiation; default 2 = FT2 sessions.
// TODO(track-c): wire from GekkoNet handshake or config once lobby lands.
static int s_negotiated_ft = 2;

#if defined(LOSSY_ADAPTER)
static GekkoNetAdapter* base_adapter = NULL;
static GekkoNetAdapter lossy_adapter = { 0 };

static float random_float() {
    /* RAND_MAX (0x7fffffff) is not representable as a float, so the implicit
     * int->float conversion trips -Wimplicit-const-int-float-conversion under
     * -Werror and this whole facility stopped compiling when the warning set
     * tightened. Cast explicitly: the value rounds to 2147483648.0f either
     * way, which is what the ratio wants. */
    return (float)rand() / (float)RAND_MAX;
}

static void LossyAdapter_SendData(GekkoNetAddress* addr, const char* data, int length) {
    const float number = random_float();

    // Adjust this number to change drop probability
    if (number <= 0.25) {
        return;
    }

    base_adapter->send_data(addr, data, length);
}
#endif

static void clean_input_buffers() {
    p1sw_0 = 0;
    p2sw_0 = 0;
    p1sw_1 = 0;
    p2sw_1 = 0;
    p1sw_buff = 0;
    p2sw_buff = 0;
    SDL_zeroa(PLsw);
    SDL_zeroa(plsw_00);
    SDL_zeroa(plsw_01);
}

/**
 * @netplay_sync THIS IS THE MOST IMPORTANT FUNCTION FOR INITIAL SYNC.
 *
 * Both peers enter netplay from slightly different local states (different
 * menus, timers, button configs, character select progress). This function
 * forces every divergent global to a known identical value so that the first
 * rollback frame starts from the same state on both sides.
 *
 * Ported from 3sxtra netplay.c:158-391 (Track A Phase 2). Substitutions vs
 * 3sxtra verbatim:
 *   - MenuTask_SetPhase(MTP_NETPLAY_IDLE) → task[TASK_MENU].r_no[0] = 5
 *     (we don't have 3sxtra's MenuTask phase enum; r_no routing is equivalent)
 *   - plw[].wu.pl_operator → plw[].wu.wu_operator (we keep the original name;
 *     3sxtra renamed to avoid C++ keyword conflict per research §9.5)
 *   - CFG_KEY_NETPLAY_FT / Config_GetInt → s_negotiated_ft static placeholder
 *     (FT negotiation is Track C lobby scope)
 */
static void setup_vs_mode() {
    // ====================================================================
    // PHASE 0: Zero per-player and combat subsystem state.
    //
    // When connecting from the network lobby, the game engine may have been
    // running under an overlay (attract mode, demo, etc.), leaving stale data
    // in PLW[] and related player subsystems. We only zero player/combat
    // state — NOT engine globals (G_No, Country, task routing, etc.) because
    // step_game() runs during TRANSITIONING to advance the game state
    // machine (G_No[1]: 12→1).
    // ====================================================================
    SDL_zeroa(plw);
    SDL_zeroa(zanzou_table);
    SDL_zeroa(super_arts);

    // Task timers and scratch data evolve independently per peer during menus.
    // Zero them for deterministic start. DO NOT zero r_no or condition —
    // those are game state machine routing fields set by the engine.
    for (int i = 0; i < 11; i++) {
        task[i].timer = 0;
        SDL_zeroa(task[i].free);
    }

    // Jump to menu idle routine (equivalent of 3sxtra's MenuTask_SetPhase).
    task[TASK_MENU].r_no[0] = 5;
    cpExitTask(TASK_SAVER);
    cpExitTask(TASK_PAUSE);

    // Zero pause flags — if one peer was paused before entering netplay,
    // these would differ on the first synced frame.
    Pause = 0;
    Game_pause = 0;

    // Re-set after zeroing plw — both players must be active for 2P mode.
    plw[0].wu.wu_operator = 1;
    plw[1].wu.wu_operator = 1;
    Operator_Status[0] = 1;
    Operator_Status[1] = 1;
    Clear_Personal_Data(0);
    Clear_Personal_Data(1);
    grade_check_work_1st_init(0, 0);
    grade_check_work_1st_init(0, 1);
    grade_check_work_1st_init(1, 0);
    grade_check_work_1st_init(1, 1);
    Setup_Training_Difficulty();

    // Tear down stale backgrounds, reinitialize the effect pool, and stop
    // any lingering effects. Without this, attract/demo mode effects start
    // diverged between peers — and since we snapshot EffectState for
    // rollback, every restore to an early frame would replay garbage.
    System_all_clear_Level_B();

    G_No[0] = 2;
    E_No[0] = 1;
    Demo_Flag = 1;

    G_No[1] = 12;
    G_No[2] = 1;
    Mode_Type = MODE_NETWORK;
    // Present_Mode is a PresentMode, not a ModeType (upstream #296); NETWORK
    // and NETPLAY happen to share numeric value 2 in both enums, but assign
    // the correctly-typed constant here rather than leaning on that.
    Present_Mode = PRESENT_MODE_NETPLAY;
    Play_Mode = 0;
    Replay_Status[0] = 0;
    Replay_Status[1] = 0;
    cpExitTask(TASK_MENU);

    // Force standard game settings so both peers use identical values
    // regardless of each player's local DIP switch configuration. Without
    // this, save_w[MODE_NETWORK] retains per-player settings that cause
    // gameplay desyncs (different HP, timer, round count).
    save_w[MODE_NETWORK].Time_Limit = 99;
    save_w[MODE_NETWORK].Battle_Number[0] = 2; // Best of 3 (1P vs CPU)
    save_w[MODE_NETWORK].Battle_Number[1] = 2; // Best of 3 (1P vs 2P)
    /* Match the CPS3 default service setting.  Damage_Level 0 reduces
     * ordinary damage to roughly two-thirds of the arcade-default value. */
    save_w[MODE_NETWORK].Damage_Level = 1;
    save_w[MODE_NETWORK].Handicap = 0;
    save_w[MODE_NETWORK].GuardCheck = 0;

    // Balance is no longer forced off here: every netplay entry path is
    // gated at arm time by Netplay_ArmAllowed() (verified-arcade required)
    // and the MIST handshake digest-checks the adapted data, so both peers
    // simulate identical arcade balance. Belt-and-braces: a session
    // reaching this chokepoint without verified arcade means an entry
    // path missed its gate.
    if (!ArcadeBalance_IsEnabled()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "setup_vs_mode reached without verified arcade balance (%s) — "
                     "an entry path missed the Netplay_ArmAllowed gate",
                     ArcadeBalance_GetReason());
    }
    // CFG_DRAW_PLAYERS_ABOVE_HUD's gameplay-affecting reads (effect-table
    // population, scr_trans/scr_calc selection) are still a local config
    // peers never negotiate — suppress for the session at the common
    // session-setup chokepoint (covers the in-game network menu's direct
    // Netplay_BeginDirectP2P call, which skips NetplayNav_Arm). Released
    // when the session finishes tearing down.
    DrawPlayersAboveHud_SetNetplaySuppressed(true);

    E_Timer = 0; // E_Timer can have different values depending on when the session was initiated.

    Deley_Shot_No[0] = 0;
    Deley_Shot_No[1] = 0;
    Deley_Shot_Timer[0] = 15;
    Deley_Shot_Timer[1] = 15;
    Random_ix16 = 0;
    Round_num = 0;
    Game_timer = 0;
    Random_ix32 = 0;
    Clear_Flash_Init(4);

    // Ensure both peers start with identical timer state regardless of local
    // DIP switch settings. Without this, CurrentSave()->Time_Limit can differ
    // per player's config.
    Counter_hi = 99;
    Counter_low = 60;

    // Flash_Complete runs during the character select screen at slightly
    // different speeds per peer depending on when they connected. Zero to sync.
    Flash_Complete[0] = 0;
    Flash_Complete[1] = 0;

    // BG scroll positions and parameters evolve independently during the
    // transition phase before synced gameplay. Zero them so both peers start
    // identical. (These fields were added to GameState in Phase 1.)
    SDL_zeroa(bg_pos);
    SDL_zeroa(fm_pos);
    SDL_zeroa(bg_prm);
    Screen_Switch = 0;
    Screen_Switch_Buffer = 0;
    system_timer = 0;
    Interrupt_Timer = 0;

    // Order[] tracks rendering layer visibility for character select UI
    // elements. Weak_PL picks the weaker CPU during demo/attract mode via
    // random_16(). Both diverge per peer before battle; zero for a clean start.
    SDL_zeroa(Order);
    Weak_PL = 0;

    // Force identity button config for MODE_NETWORK so Convert_User_Setting()
    // is a no-op during simulation. Each player's actual config was already
    // baked into their input by get_inputs() via Remap_Buttons().
    {
        const u8 identity[8] = { 0, 1, 2, 11, 3, 4, 5, 11 };
        for (int p = 0; p < 2; p++) {
            for (int s = 0; s < 8; s++)
                save_w[MODE_NETWORK].Pad_Infor[p].Shot[s] = identity[s];
            save_w[MODE_NETWORK].Pad_Infor[p].Vibration = 0;
        }
    }

    // Apply first-to-X wins: FT controls how many GAME wins are needed for a
    // session (tracked server-side in future). Each individual game is always
    // best-of-3 rounds (Battle_Number = 1, meaning need 2 round wins per game).
    {
        int ft = s_negotiated_ft > 0 ? s_negotiated_ft : 2;
        if (ft < 1)
            ft = 2;
        if (ft > 10)
            ft = 10;
        s_negotiated_ft = ft; // Keep for future match reporting.
        // Rounds per game: always best-of-3 (need 2 round wins).
        save_w[MODE_NETWORK].Battle_Number[0] = 1;
        save_w[MODE_NETWORK].Battle_Number[1] = 1;
    }

    // Check_Buff and Convert_Buff hold per-player button remapping tables.
    // Each peer loads them from their local config, so they differ between
    // players. Zero them so the simulation uses identity mappings.
    SDL_zeroa(Check_Buff);
    SDL_zeroa(Convert_Buff);

    // Timers that evolved independently during menus/transition. Without
    // this, Game_timer and Control_Time diverge immediately.
    Game_timer = 0;
    Control_Time = 0;
    players_timer = 0;
    G_Timer = 0;

    // Peers spend different amounts of time in the menus, and nothing on the
    // way into a fight otherwise clears what that leaves behind.
    Next_Demo = 0;

    // bg_initialize() reinitialises only part of each layer, and l_limit/
    // r_limit are written by the opening alone. scno is one of those: the
    // attract sequence leaves it at 3 and the stage only assigns it on
    // frame 2, so peers that reached the menu at different times disagree
    // on the first two frames of every session.
    SDL_zeroa(bg_w.bgw);
    bg_w.scno = 0;
    bg_w.scrno = 0;

    // Per-player globals that can hold stale values from the previous game
    // session or differ based on who connected first.
    Champion = 0;
    Forbid_Break = 0;
    Connect_Status = 0;
    Stop_SG = 0;
    Exec_Wipe = 0;
    Gap_Timer = 0;
    SDL_zeroa(E_No);

    // State machine routing numbers evolve per-player during character select.
    // Each peer advances C_No/SC_No from its own perspective, causing them
    // to diverge before battle. Zero them so both peers start identical.
    SDL_zeroa(C_No);
    SDL_zeroa(SC_No);

    // ====================================================================
    // PHASE 3: Zero checksummed globals not covered above.
    //
    // save_state() checksums a whitelist of gameplay-critical fields
    // (ported in Phase 3). On LAN, the native menu system's fade-destroy-
    // reinit cycle leaves these at zero already. On INTERNET, the game
    // engine runs under an overlay (attract/demo mode), leaving stale
    // values that differ per peer and cause frame-0 desyncs.
    // ====================================================================

    // Extended RNG indices (attract mode advances these independently).
    Random_ix16_ex = 0;
    Random_ix32_ex = 0;
    Random_ix16_com = 0;
    Random_ix32_com = 0;
    Random_ix16_ex_com = 0;
    Random_ix32_ex_com = 0;

    /* Round/match state.
     *
     * Round_Level is seeded to 3, NOT 0. Three independent sources agree on
     * 3 and none on 0:
     *   - the port's own `Before_Select_Sub()` (game.c) ends with
     *     `Round_Level = 3;`, and so does its arcade counterpart (CPS3
     *     0x0609520C, store at 0x0609531A);
     *   - real CPS3 main RAM: `Round_Level` (CPS3 0x0201137A, confirmed by
     *     disassembly -- it is the sole index of `Pow_Control_Data_1` in
     *     the arcade damage routines at 0x0609E36C/0x0609E3FA) reads 3 at
     *     the start of every one of 16 games across 4 independent Fightcade
     *     sessions, and the only values ever observed are 3, 2 and 1 --
     *     never 0;
     *   - `game.c`'s other init is 7 (demo), so 0 is not a port value
     *     either.
     * It matters because `cal_damage_vitality()` (pow_pow.c) indexes
     * `Pow_Control_Data_1[0] = {90,95,98,100,103,106,109,112}` with it under
     * arcade balance: index 3 is exactly 100 (neutral), index 0 is 90 --
     * seeding 0 would silently cut every netplay hit by 10%. */
    Round_Level = 3;
    Round_Result = 0;
    SDL_zeroa(PL_Wins);
    Conclusion_Type = 0;
    SDL_zeroa(win_type);

    // Clean up stale attract/demo sequences that mutate start-of-match state.
    Combo_Demo_Flag = 0;
    Select_Demo_Index = 0;
    Demo_Stage_Index = 0;
    Demo_PL_Index = 0;

    // Player identity (set later by character select, but must start clean).
    SDL_zeroa(My_char);
    SDL_zeroa(Super_Arts);

    // Combat flags (stale from previous match or attract mode demo fights).
    SDL_zeroa(Attack_Flag);
    SDL_zeroa(Counter_Attack);
    SDL_zeroa(Guard_Flag);
    SDL_zeroa(Flip_Flag);
    SDL_zeroa(Lie_Flag);
    SDL_zeroa(Attack_Counter);
    SDL_zeroa(Bullet_No);
    SDL_zeroa(Bullet_Counter);
    SDL_zeroa(paring_counter);

    // Game flow.
    VS_Stage = 0;

    // Slow motion.
    SLOW_timer = 0;
    SLOW_flag = 0;
    EXE_flag = 0;

    // Stun gauge / vitality.
    SDL_zeroa(piyori_type);
    Max_vitality = 160; // MAX_VITALITY_DEFAULT — must not be 0 (setup_vitality divides by it).

    // Damage-scaling combo state (task #143). Written unconditionally by
    // combo_cont_init() (engine/cmb_win.c) at real battle init, which runs
    // AFTER setup_vs_mode — so zeroing here only clears a carry-over from a
    // PRIOR match/session, it never stomps a legitimate pre-save write.
    // Without this, a peer that played an earlier offline combo enters the
    // session with nonzero combo_type/remake_power while a fresh-boot peer
    // has zero, and the two disagree on the very first checksummed frame.
    SDL_zeroa(combo_type);
    SDL_zeroa(remake_power);

    // ca_check_flag (battle throw/counter-attack gate, engine/hitcheck.c),
    // Color7 (char-select color chord, screen/sel_pl.c), spmv_ng_save
    // (Makoto SA-buff PLW.spmv_ng_flag backup, effect/effl8.c) — none of
    // these three are declared in a header this TU already includes, so
    // (matching game_state.c's own local-extern style for this exact
    // field group) they're declared extern right here rather than pulling
    // in hitcheck.h/sel_pl.h/effl8.h for one symbol each.
    //
    // ca_check_flag: every write site found by grep (game.c, plcnt2.c,
    // plmain.c, plcnt.c — not claimed exhaustive) fires at real battle
    // init, after setup_vs_mode — same
    // "only clears a prior match's carry-over" argument as combo_type
    // above. Without a reset, a peer that played an offline match (any
    // write site sets it to 1) carries that 1 into the next netplay
    // session while a fresh-boot peer has 0.
    //
    // Color7 is a per-frame char-select accumulator with nothing that
    // zeroes it between matches/sessions — see the audit comment at
    // sel_pl.c:132-159. A peer that reached char-select before (this
    // session or a prior one) can carry a nonzero chord into the next
    // session's first save.
    //
    // spmv_ng_save: written only inside effect_L8_move's case 0
    // (effl8.c:50, "save the flag before the buff, so case 1 can restore
    // it") and never reset by anything else. Once Makoto's SA-buff effect
    // has fired once in this process, the value it stashed persists as a
    // stale process-global until the effect fires again — including into
    // a later netplay session. (2026-04-24 Candidate 0b wrongly
    // exonerated this field as dead via incorrect CPS3 character
    // numbering — see the "H-6" note at sel_pl.c:135 — it is live.)
    {
        extern u16 Color7[2];
        extern s8 ca_check_flag;
        extern u32 spmv_ng_save[2];
        SDL_zeroa(Color7);
        ca_check_flag = 0;
        SDL_zeroa(spmv_ng_save);
    }

    // chainex_check (EX-SA chain gating, system/sysdir.c) — deliberately
    // included for defense-in-depth, NOT because it is believed reachable
    // today. Every write site sits inside `if (!ArcadeBalance_IsEnabled())`
    // (see the audit comment at game_state.h above the chainex_check field),
    // and Netplay_ArmAllowed() == ArcadeBalance_IsEnabled(), which is a
    // fixed-at-boot value (ArcadeBalance_Init, called once from main.c) that
    // cannot change mid-process. So any process that ever arms netplay has
    // had arcade balance ON for its entire lifetime, meaning those writes
    // provably never fired in that process — chainex_check cannot be stale
    // entering a netplay session on a shipped build. This mirrors the
    // ArcadeBalance_IsEnabled() belt-and-braces check a few lines above in
    // this same function (setup_vs_mode already treats "an entry path
    // missed its gate" as a real possibility to defend, not just log): if
    // that invariant is ever violated, a stale chainex_check would be a
    // silent frame-0 desync exactly like the other five fields in this
    // block, so it gets the same defensive reset for uniformity.
    {
        extern u8 chainex_check[2][36];
        SDL_zeroa(chainex_check);
    }

    clean_input_buffers();
}

#if defined(LOSSY_ADAPTER)
static void configure_lossy_adapter(NET_DatagramSocket* sock) {
    base_adapter = SDLNetAdapter_Create(sock);
    lossy_adapter.send_data = LossyAdapter_SendData;
    lossy_adapter.receive_data = base_adapter->receive_data;
    lossy_adapter.free_data = base_adapter->free_data;
}
#endif

// R-1 — SDL3_net adapter for the extracted handshake runner core
// (mist_handshake_run_attempt). The core owns the loop/classification
// logic; these callbacks bind it to the live session socket. The most
// recently received datagram is kept alive in `last` (destroyed on the
// next recv or after the run) so send_reply_to_last can answer its
// source, exactly as the pre-extraction inline loop did.
typedef struct {
    NET_DatagramSocket* sock;
    NET_Address* peer;
    Uint16 peer_port;
    NET_Datagram* last;
} MistNetIo;

static void mist_netio_send_to_peer(void* vctx, const uint8_t* buf, size_t len) {
    MistNetIo* io = (MistNetIo*)vctx;
    NET_SendDatagram(io->sock, io->peer, io->peer_port, buf, (int)len);
}

static int mist_netio_recv(void* vctx, uint8_t* buf, size_t cap, bool* from_session_peer) {
    MistNetIo* io = (MistNetIo*)vctx;
    if (io->last != NULL) {
        NET_DestroyDatagram(io->last);
        io->last = NULL;
    }
    NET_Datagram* dgram = NULL;
    if (!NET_ReceiveDatagram(io->sock, &dgram) || dgram == NULL) {
        return 0;
    }
    // Task #119: during TRANSITIONING this drain is the socket's only
    // reader, so a retrying peer's authenticated punch lands HERE, not in
    // the GekkoNet adapter. Offer it to the late-punch layer first: a
    // consumed datagram is punch traffic (answered / relearned / dropped
    // by late_punch.c's rules) that the MIST magic gate would have
    // silently discarded — which is exactly the one-sided-handoff hang
    // this layer exists to close. Returning 0 just ends this slice; the
    // pump re-polls next slice. Disarmed, this is a no-op.
    if (dgram->buf != NULL &&
        LatePunch_HandleDatagram(dgram->buf, dgram->buflen,
                                 NET_GetAddressString(dgram->addr),
                                 dgram->port)) {
        NET_DestroyDatagram(dgram);
        return 0;
    }
    size_t n = (size_t)dgram->buflen;
    if (n > cap) {
        n = cap;
    }
    SDL_memcpy(buf, dgram->buf, n);
    *from_session_peer = (dgram->port == io->peer_port) &&
                         (NET_CompareAddresses(dgram->addr, io->peer) == 0);
    io->last = dgram;
    return (int)n;
}

static void mist_netio_send_reply_to_last(void* vctx, const uint8_t* buf, size_t len) {
    MistNetIo* io = (MistNetIo*)vctx;
    if (io->last == NULL) {
        return;
    }
    NET_SendDatagram(io->sock, io->last->addr, io->last->port, buf, (int)len);
}

static uint64_t mist_netio_now_ms(void* vctx) {
    (void)vctx;
    return (uint64_t)SDL_GetTicks();
}

static void mist_netio_delay_ms(void* vctx, uint32_t ms) {
    (void)vctx;
    SDL_Delay(ms);
}

// R-1 / S3 — the MIST handshake gate runs on the active session socket
// before gekko_create() takes it over (tier-2 §8.2.4). S3 (Part C)
// converted it from a BLOCKING per-attempt runner — which stalled the
// main thread for the full 500 ms budget once per frame, dropping the
// game to ~2 fps with ~2 Hz input sampling for up to ~20-24 s of
// retries — to an incremental per-tick pump over the same MistRunnerIo
// seam: one bounded slice (<= one hello send + a drain of the queued
// datagrams) per frame, terminal results folded through the unchanged
// mist_handshake_gate_next retry policy. The attempt state persists in
// the statics below; the resolved peer address is cached across the
// retry attempts of one session and released by mist_pump_stop().
static NET_DatagramSocket* acquire_active_socket(void); /* defined below */

static bool s_hs_pump_active = false;
static MistPumpState s_hs_pump;
static MistNetIo s_hs_net_io = { NULL, NULL, 0, NULL };
static NET_Address* s_hs_peer_addr = NULL;
static const MistRunnerIo s_hs_io_template = {
    &s_hs_net_io,
    mist_netio_send_to_peer,
    mist_netio_recv,
    mist_netio_send_reply_to_last,
    mist_netio_now_ms,
    mist_netio_delay_ms,
};

// Release everything the pump holds. Idempotent; called on PROCEED,
// hard failure, and session EXITING (covers a mid-handshake abort).
static void mist_pump_stop(void) {
    if (s_hs_net_io.last != NULL) {
        NET_DestroyDatagram(s_hs_net_io.last);
        s_hs_net_io.last = NULL;
    }
    if (s_hs_peer_addr != NULL) {
        NET_UnrefAddress(s_hs_peer_addr);
        s_hs_peer_addr = NULL;
    }
    s_hs_net_io.sock = NULL;
    s_hs_net_io.peer = NULL;
    s_hs_pump_active = false;
}

// Arm one pump attempt: bind the session socket, resolve (once per
// session, cached) the peer address, stamp the 500 ms budget. Returns
// false with the reason cached in s_mist_reject_reason on a local
// transport error — the caller folds that through the gate as a hard
// failure, exactly like the old blocking wrapper did.
static bool mist_pump_start(void) {
    NET_DatagramSocket* sock = acquire_active_socket();
    if (sock == NULL || remote_ip == NULL || remote_port == 0) {
        SDL_strlcpy(s_mist_reject_reason, "missing transport state", sizeof(s_mist_reject_reason));
        return false;
    }
    // Task #119: a relearn moved the peer while an attempt was mid-flight
    // (see s_hs_peer_refetch). This is the first point where no attempt
    // holds the cached address, so drop it and re-resolve below. The
    // relearn is same-IP by construction (late_punch.c), so today only
    // peer_port actually changes — the refetch still runs so the cache
    // discipline doesn't silently depend on that restriction.
    if (s_hs_peer_refetch) {
        s_hs_peer_refetch = false;
        if (s_hs_peer_addr != NULL) {
            NET_UnrefAddress(s_hs_peer_addr);
            s_hs_peer_addr = NULL;
        }
    }
    if (s_hs_peer_addr == NULL) {
        s_hs_peer_addr = NET_ResolveHostname(remote_ip);
        if (s_hs_peer_addr == NULL) {
            SDL_strlcpy(s_mist_reject_reason, "peer resolve failed", sizeof(s_mist_reject_reason));
            return false;
        }
        /* Wait briefly for resolution. One-time per session; remote_ip
         * is a numeric dotted-quad on the orchestrator path, so this
         * resolves immediately — the bounded poll only matters for a
         * hostname passed via the LAN CLI. */
        for (int spin = 0; spin < 50; spin++) {
            if (NET_GetAddressStatus(s_hs_peer_addr) != NET_WAITING) break;
            SDL_Delay(2);
        }
        if (NET_GetAddressStatus(s_hs_peer_addr) != NET_SUCCESS) {
            NET_UnrefAddress(s_hs_peer_addr);
            s_hs_peer_addr = NULL;
            SDL_strlcpy(s_mist_reject_reason, "peer resolve timed out", sizeof(s_mist_reject_reason));
            return false;
        }
    }
    s_hs_net_io.sock = sock;
    s_hs_net_io.peer = s_hs_peer_addr;
    s_hs_net_io.peer_port = remote_port;
    // MIST v2: advertise the adapted arcade-balance digest so peers with
    // differing adapted data (e.g. different CPS3 ROM revisions) reject
    // cleanly instead of desyncing mid-match. Balance is fixed at boot,
    // so this is idempotent; it must be set BEFORE pump_begin because
    // pump_begin builds the hello frame (mist_handshake_build_hello ->
    // build_frame) which serializes the digest, and before any slice
    // runs, because a slice's responder path (drain_and_answer_hellos /
    // mist_handshake_build_reply) serializes it into ack frames too.
    // Arming one attempt is the seam: it is the pre-S3 blocking runner's
    // call site translated into the pump's lifecycle, and every frame
    // this peer emits during the attempt is built after it.
    mist_handshake_set_balance_digest(ArcadeBalance_GetDigest());
    if (!mist_handshake_pump_begin(&s_hs_pump, &s_hs_io_template,
                                   s_mist_reject_reason, sizeof(s_mist_reject_reason))) {
        return false;
    }
    s_hs_pump_active = true;
    return true;
}

// Task #119: one service call per pre-session frame, from Netplay_Run's
// TRANSITIONING and CONNECTING cases — the window between do_handoff and
// GekkoSessionStarted in which a one-sided race can still be rescued.
// Drives the late-punch keepalive/answer stream and applies any relearn
// to every send path that targets the peer:
//   - remote_port      (configure_gekko's registration + MIST re-arm)
//   - the MIST pump's cached peer address (via s_hs_peer_refetch)
//   - the GekkoNet adapter's actual send target / inbound presentation
//     (SDLNetAdapter_RetargetPeer — a no-op passthrough before
//     configure_gekko has registered a canonical peer).
// The relearn is late_punch.c's verdict, token-authenticated and
// restricted to the established peer IP, so everything applied here is
// a PORT move within the peer the handoff already committed to.
static void late_punch_service(void) {
    if (!LatePunch_IsArmed()) {
        return;
    }
    LatePunch_Tick((uint32_t)SDL_GetTicks());
    char ip[64];
    uint16_t new_port = 0;
    if (LatePunch_TakeRelearn(ip, sizeof(ip), &new_port) && new_port != 0) {
        char line[160];
        SDL_snprintf(line, sizeof(line),
                     "[netplay sess=%08x] late-punch relearn: peer %s port "
                     "%u -> %u (state=%s)",
                     s_session_uuid, ip, (unsigned)remote_port,
                     (unsigned)new_port,
                     session_state == NETPLAY_SESSION_CONNECTING
                         ? "connecting" : "transitioning");
        Netplay_LogConnectEvent(line);
        remote_port = new_port;
        s_hs_peer_refetch = true;
        SDLNetAdapter_RetargetPeer(ip, new_port);
    }
}

// R-1: single source of truth for "which socket carries this session".
// Was inlined in configure_gekko(); hoisted so the MIST handshake gate in
// Netplay_Run() runs on the exact same socket GekkoNet will take over.
// Idempotent: the direct-P2P CLI/LAN socket is created once and cached in
// p2p_sock (destroyed + NET_Quit'd on session EXITING, unchanged).
static NET_DatagramSocket* acquire_active_socket(void) {
    if (stun_socket != NULL) {
        // Step 6: pre-punched STUN socket (internet play via orchestrator).
        // If the STUN socket is set, the orchestrator has already
        // hole-punched through NAT and the remote peer is expecting the
        // STUN-discovered mapping. Mirrors upstream
        // /tmp/3sxtra/src/netplay/netplay.c:440-444.
        // NetTuning_SetRecvBuf is a plain setsockopt — calling it again on
        // the configure_gekko() pass after the handshake pass is harmless.
        NetTuning_SetRecvBuf(stun_socket, 256 * 1024);
        return stun_socket;
    }
    // Direct P2P CLI/LAN path: create a dedicated UDP socket on local_port.
    if (p2p_sock == NULL) {
        /* Both results were previously discarded, so a bind failure here
         * surfaced only as the MIST pump's generic "missing transport
         * state" — indistinguishable from a missing remote_ip, and with no
         * hint of WHY. That cost a long debug session against a real
         * MiSTer: the device failed at transitioning frame 1 on every
         * attempt while both UDP directions were provably fine at the OS
         * level, and nothing in the log could tell us whether the port was
         * taken, the address was unusable, or SDL_net had not come up. */
        if (!NET_Init()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[netplay] NET_Init failed, cannot open the LAN/CLI "
                         "socket on port %d: %s",
                         local_port, SDL_GetError());
            return NULL;
        }
        p2p_sock = NET_CreateDatagramSocket(NULL, local_port);
        if (p2p_sock == NULL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[netplay] could not bind the LAN/CLI UDP socket on "
                         "port %d: %s (is another 3s-arm still holding it? a "
                         "previous session that failed its handshake keeps "
                         "running and keeps the port)",
                         local_port, SDL_GetError());
        }
    }
    return p2p_sock;
}

static void configure_gekko() {
    GekkoConfig config;

#if ENABLE_PERF_TELEMETRY
    /* Task #69.3 — session-start skew. This is the last moment before the
     * rollback engine exists (gekko_create() below), and the LDREQ barrier
     * is already ON here: #72 widened Ldreq_BarrierActive() (gd3rd.c:652)
     * from NETPLAY_SESSION_RUNNING-only to also cover TRANSITIONING and
     * CONNECTING, and this line runs while
     * session_state is still TRANSITIONING. If two peers can hold
     * different loader state at this instant, this is where it shows. */
    Ldreq_LogSessionProbe("configure-gekko", s_last_advance_frame);
#endif

    SDL_zero(config);

    config.num_players = PLAYER_COUNT;
    config.input_size = sizeof(u16);
    /* Sparse effect-pool save (Option A) — `state_size` is the per-slot
     * fixed allocation Gekko's StateStorage performs (storage.cpp:16,
     * `make_unique<u8[]>(state_size)`). It is the *ceiling*, NOT the
     * per-frame size; that's reported separately via GekkoSave.state_len.
     *
     * We keep this at sizeof(State) so the full-state safety net (kill
     * switch OFF or active>SPARSE_CEILING_SLOTS) always fits in a slot
     * without overrunning Gekko's buffer. The savings under Option A
     * come from the per-frame state_len being typically ~143KB (sparse
     * format with ~50 active) instead of ~247KB. That shrinks:
     *   - the user-side memcpy in load_state_from_event,
     *   - the checksum + transmission bytes Gekko walks on each save,
     *   - net bandwidth on rollback resync.
     * Ring-buffer footprint stays at ~4.9MB; Gekko's per-slot allocation
     * does not vary with state_len.
     *
     * If a future refactor makes the full-state fallback unnecessary
     * (e.g. by enforcing the ceiling via the active-slot pool design),
     * lower this to SPARSE_CEILING_BYTES and the ring shrinks to ~2.8MB.
     * The _Static_assert below enforces the correctness invariant. */
    config.state_size = (unsigned int)sizeof(State);
    _Static_assert(sizeof(State) >= SPARSE_CEILING_BYTES,
                   "SPARSE_CEILING_BYTES exceeds sizeof(State) — sparse buffer "
                   "would overflow if state_size were sizeof(State). Re-check "
                   "the SPARSE_CEILING_SLOTS / SPARSE_HEADER_BYTES math in "
                   "game_state.h.");
    config.max_spectators = 0;
    int cfg_pred_window = Config_GetInt(CFG_KEY_NETPLAY_INPUT_PREDICTION_WINDOW);
    if (cfg_pred_window < 1 || cfg_pred_window > NETPLAY_MAX_INPUT_PREDICTION_WINDOW) {
        cfg_pred_window = 8;
    }
    config.input_prediction_window = (unsigned char)cfg_pred_window;

    // #145: the deferred menu-exit bound must match THIS session's window,
    // and no request or sim-frame stamp may leak in from a prior session.
    s_pred_window = cfg_pred_window;
    s_menu_exit_request_frame = -1;
    s_sim_frame = -1;

    // Unconditional: the focused checksum runs in Release too (Phase 3).
    // Research §19 risk 5: shipping without detection means silent desyncs.
    config.desync_detection = true;

    if (gekko_create(&session, GekkoGameSession)) {
        gekko_start(session, &config);
    } else {
        printf("Session is already running! probably incorrect.\n");
    }

    // Diagnostics: refresh per-session identifiers and read the runtime gate
    // once. Captured here so two back-to-back sessions in the same process
    // get distinct UUIDs in the log stream.
    SDL_SetAtomicInt(&s_diag_enabled, Config_GetBool(CFG_KEY_NETPLAY_DIAG_ENABLE) ? 1 : 0);
    {
        // 32-bit tag derived from monotonic ticks XOR PID — not cryptographic,
        // just unique enough within a peer's log stream so log lines from
        // session A vs B don't blur. Two peers will (almost certainly) pick
        // different UUIDs; cross-peer correlation uses session_started_unix_ms
        // (within a few seconds of each other) plus the role/handle.
        uint64_t now_ns = SDL_GetTicksNS();
        s_session_uuid = (uint32_t)(now_ns ^ (now_ns >> 32) ^ (uint32_t)getpid());
        if (s_session_uuid == 0) {
            s_session_uuid = 1;  // 0 reserved as "unset" sentinel.
        }
    }
    s_session_started_unix_ms = netplay_utc_ms();
    s_session_started_ticks_ms = SDL_GetTicks();
    s_session_end_reason[0] = '\0';
    s_session_end_frame = -1;
    // Tier-1 netplay diag — Item 5 (watchdog): seed last-advance to "now" so
    // the watchdog only fires once we've actually seen at least one advance
    // followed by a real stall. Items 3/6: zero per-session counters.
    s_last_advance_utc_ms = netplay_utc_ms();
    s_watchdog_latched = false;
    SDL_zeroa(s_hb_prev_pkt_tx);
    SDL_zeroa(s_hb_prev_pkt_rx);
    SDL_zeroa(s_rb_buckets);
    s_hold_last_frame = false;
    s_no_draw_episode = false;
    s_hb_no_draw_holds = 0;
    s_hb_catchups = 0;
    s_last_no_draw_frame = -1;

    // Tier-1 netplay diag — Item 7: open the per-session netplay log file.
    // The heartbeat logger owns its asynchronous one-second flush. Filename
    // includes the session-start unix ms so cross-peer pairing is just
    // matching ms.
    //
    // S3-review L-2: a PRE-session connection failure opens the log lazily
    // via Netplay_LogConnectEvent, and nothing on that path ever closes it
    // (netplay_log_close only runs from the EXITING/flush paths of a live
    // session) — so without this close, a later session's netplay_log_open
    // would early-return on the already-open handle and append the entire
    // session log to the failure-timestamped file. Retire any such
    // leftover log first; this session gets its own file.
    // #36: close+open must be ONE critical section — a worker landing
    // between them would lazily reopen a file this session is about to
    // replace.
    Netplay_LogSinkInit();
    heartbeat_disable_and_drain();
    netplay_log_lock();
    netplay_log_close();
    netplay_log_open(s_session_started_unix_ms);
    netplay_log_unlock();
    heartbeat_enable();

    // Tier-1 netplay diag — Item 9: capture /proc/net/snmp UDP-row baseline
    // so peer-disconnect / session-end can emit deltas. Zeroed sample on
    // non-MiSTer hosts (read returns false; valid stays false).
    SDL_zero(s_udp_snmp_session_start);
    read_proc_net_snmp_udp(&s_udp_snmp_session_start);

    SDL_Log("[netplay sess=%08x] session-start utc_ms=%llu pred_window=%d state_size=%zu role=%s",
            s_session_uuid,
            (unsigned long long)s_session_started_unix_ms,
            (int)config.input_prediction_window,
            (size_t)sizeof(State),
            (player_number == 0) ? "host" : "joiner");

    NET_DatagramSocket* active_sock = acquire_active_socket();

#if defined(LOSSY_ADAPTER)
    configure_lossy_adapter(active_sock);
    gekko_net_adapter_set(session, &lossy_adapter);
#else
    gekko_net_adapter_set(session, SDLNetAdapter_Create(active_sock));
#endif

    printf("starting a session for player %d at port %hu (remote=%s:%hu)\n",
           player_number, local_port, remote_ip ? remote_ip : "(null)", remote_port);
    fflush(stdout);

    char remote_address_str[100];
    SDL_snprintf(remote_address_str, sizeof(remote_address_str), "%s:%hu", remote_ip, remote_port);
    GekkoNetAddress remote_address = { .data = remote_address_str, .size = strlen(remote_address_str) };

    /* Task #119: record the canonical peer registration with the adapter.
     * GekkoNet matches inbound packets against this exact string and has
     * no relearn API, so if the authenticated peer later moves (S2 retry
     * from a fresh socket), the adapter translates in both directions
     * around this fixed registration — see sdl_net_adapter.c. A relearn
     * that lands BEFORE this line already updated remote_port above, so
     * canonical == actual again here. */
    SDLNetAdapter_SetCanonicalPeer(remote_ip, remote_port);

    for (int i = 0; i < PLAYER_COUNT; i++) {
        const bool is_local_player = (i == player_number);

        if (is_local_player) {
            player_handle = gekko_add_actor(session, GekkoLocalPlayer, NULL);
            gekko_set_local_delay(session, player_handle, DELAY_FRAMES);
        } else {
            gekko_add_actor(session, GekkoRemotePlayer, &remote_address);
        }
    }
}

static u16 get_inputs() {
    // The game doesn't differentiate between controllers and players.
    // That's why we OR the inputs of both local controllers together to get
    // local inputs.
    u16 inputs = 0;
    inputs = p1sw_buff | p2sw_buff;
    return inputs;
}

static void note_input(u16 input, int player, int frame) {
    if (frame < 0) {
        return;
    }

    input_history[player][frame % INPUT_HISTORY_MAX] = input;
}

static u16 recall_input(int player, int frame) {
    if (frame < 0) {
        return 0;
    }

    return input_history[player][frame % INPUT_HISTORY_MAX];
}

// Dump the input_history ring buffer to states/desync_F<frame>_pid<pid>_inputs.txt
// alongside the game-state dump. Most desyncs that aren't engine determinism
// bugs trace to "we executed different inputs at frame X" — diffing two peers'
// input histories at the desync frame catches that whole failure axis even
// when the state checksums look identical until the divergent frame surfaces.
static void dump_input_history_on_desync(int frame) {
    SDL_CreateDirectory("states");
    char path[256];
    SDL_snprintf(path, sizeof(path), "states/desync_F%d_pid%d_inputs.txt", frame, (int)getpid());
    FILE* f = fopen(path, "w");
    if (!f) {
        SDL_Log("[desync] ERROR: could not open %s for writing", path);
        return;
    }
    fprintf(f, "=== INPUT HISTORY at desync frame %d ===\n", frame);
    fprintf(f, "session_uuid=0x%08x  pid=%d  ring_size=%d\n\n",
            s_session_uuid, (int)getpid(), INPUT_HISTORY_MAX);
    fprintf(f, "%8s  %6s  %6s%s\n", "frame", "p1", "p2", "");
    // Walk newest-back-to-oldest within the ring (size 120 = 2s @ 60Hz).
    for (int i = 0; i < INPUT_HISTORY_MAX; i++) {
        int f_idx = frame - INPUT_HISTORY_MAX + 1 + i;
        if (f_idx < 0) continue;
        int slot = f_idx % INPUT_HISTORY_MAX;
        const char* marker = (f_idx == frame) ? " <== DESYNC" : "";
        fprintf(f, "%8d  0x%04x  0x%04x%s\n",
                f_idx,
                (unsigned)input_history[0][slot],
                (unsigned)input_history[1][slot],
                marker);
    }
    fclose(f);
    SDL_Log("[desync] Wrote input history (%d frames) to %s", INPUT_HISTORY_MAX, path);
}

// The rollback save/load/checksum pipeline lives in netplay/game_state.c
// (Phase 3 move). See game_state.h for the public API.

static bool game_ready_to_run_character_select() {
    return G_No[1] == 1;
}

static bool need_to_catch_up() {
    return frames_behind >= 1;
}

static void step_game(bool render) {
    No_Trans = !render;

    njUserMain();
    seqsBeforeProcess();
    njdp2d_draw();
    seqsAfterProcess();
}

static void advance_game(GekkoGameEvent* event, bool render) {
    const u16* inputs = (u16*)event->data.adv.inputs;
    const int frame = event->data.adv.frame;

    // #145: the frame being simulated, replay legs included — read by
    // Netplay_HandleMenuExit to stamp a deferred exit request.
    s_sim_frame = frame;

    if (render) {
        s_last_advance_frame = frame;
    }

    p1sw_0 = PLsw[0][0] = inputs[0];
    p2sw_0 = PLsw[1][0] = inputs[1];
    p1sw_1 = PLsw[0][1] = recall_input(0, frame - 1);
    p2sw_1 = PLsw[1][1] = recall_input(1, frame - 1);

    note_input(inputs[0], 0, frame);
    note_input(inputs[1], 1, frame);

    step_game(render);
}

static void handle_disconnection() {
    if (session_state == NETPLAY_SESSION_EXITING || session_state == NETPLAY_SESSION_IDLE) {
        return;
    }

    // #145: a pending menu-exit request only ever reaches here still
    // latched when disconnect/desync teardown beats
    // menu_exit_request_confirmed() to it — the request's own frame
    // can no longer be rolled back at this point (RUNNING is the only
    // state that latches one, and this function isn't reached again
    // once EXITING), so it's superseded, not erased. s_session_end_reason
    // is already stamped by the caller (peer-disconnected/desync), which
    // is the truer cause and stands; this line exists only so a
    // confused player's .log also shows they'd asked to quit first.
    if (s_menu_exit_request_frame >= 0) {
        SDL_Log("[netplay sess=%08x] event=menu-exit-superseded request_frame=%d reason=%s",
                s_session_uuid, s_menu_exit_request_frame,
                s_session_end_reason[0] ? s_session_end_reason : "unknown");
    }

    clean_input_buffers();
    Soft_Reset_Sub();
    if (s_session_end_reason[0] == '\0') {
        SDL_strlcpy(s_session_end_reason, "disconnect", sizeof(s_session_end_reason));
        s_session_end_frame = s_last_advance_frame;
    }
    session_state = NETPLAY_SESSION_EXITING;
}

/* Task #144 review Item A: process_session() has no test seam — driving a
 * live GekkoPlayerDisconnected/GekkoDesyncDetected needs two real UDP
 * peers carried past the MIST handshake into a running GekkoNet session —
 * so nothing pins that the two RUNNING-phase handlers below actually CALL
 * DirectP2P_NotifySessionFailed at all; deleting both calls would leave
 * the suite green. This pure helper at least pins the narrower claim that
 * IS testable in isolation: the event->code MAPPING. See
 * Netplay_TestHook_SessionFailCodeForEvent below and test 43. */
static ConnectFailCode session_fail_code_for_event(GekkoSessionEventType type) {
    switch (type) {
    case GekkoPlayerDisconnected: return CONNECT_FAIL_PEER_DISCONNECTED;
    case GekkoDesyncDetected:     return CONNECT_FAIL_DESYNC_DETECTED;
    default:                      return CONNECT_FAIL_NONE;
    }
}

#ifdef NETPLAY_TEST_HOOKS
ConnectFailCode Netplay_TestHook_SessionFailCodeForEvent(GekkoSessionEventType type) {
    return session_fail_code_for_event(type);
}

/* #145: the deferred-menu-exit decision predicates. Driving the full
 * latch/cancel/fire cycle needs a live Gekko session (two peers past the
 * MIST handshake), so what IS pinned is the pure math both sites run on:
 * the confirmation bound (head - request >= window, derived from
 * GekkoNet 7be848c's prediction cap) and the load-cancel boundary
 * (load < request erases; load == request does not — a load to R
 * restores post-R state without re-running R's sim). */
bool Netplay_TestHook_MenuExitConfirmed(int head_frame, int request_frame, int pred_window) {
    return menu_exit_request_confirmed(head_frame, request_frame, pred_window);
}

bool Netplay_TestHook_MenuExitErasedByLoad(int load_frame, int request_frame) {
    return menu_exit_request_erased_by_load(load_frame, request_frame);
}

bool Netplay_TestHook_ShouldHoldLastFrame(NetplaySessionState state, int drawable_advances) {
    return should_hold_last_frame(state, drawable_advances);
}
#endif

static void process_session() {
    frames_behind = -gekko_frames_ahead(session);

    gekko_network_poll(session);

    u16 local_inputs = get_inputs();
    gekko_add_local_input(session, player_handle, &local_inputs);

    // Tier-1 netplay diag — Item 5: GekkoAdvanceEvent watchdog. Fire once
    // per stall episode if no advance has happened in 250 ms. This is the
    // exact signal we lacked for the 17.6-min disconnect: friend's side
    // stopped advancing, peer-disconnect waited 5s for the timeout, no
    // logging in between. 250 ms = ~15 frames at 60 Hz; even a heavy
    // rollback chain returns to advance well inside that window.
    if (session_state == NETPLAY_SESSION_RUNNING && !s_watchdog_latched) {
        const uint64_t now_ms = netplay_utc_ms();
        if (now_ms > s_last_advance_utc_ms + 250) {
            const uint64_t stall_ms = now_ms - s_last_advance_utc_ms;
            char line[256];
            SDL_snprintf(line, sizeof(line),
                         "[netplay sess=%08x] WATCHDOG no-advance for %llu ms "
                         "(last_frame=%d frames_behind=%.1f recent_rb=%d)",
                         s_session_uuid,
                         (unsigned long long)stall_ms,
                         s_last_advance_frame,
                         (double)frames_behind,
                         (int)network_stats.rollback);
            (void)netplay_log_line_try(line);
            s_watchdog_latched = true;
        }
    }

    int session_event_count = 0;
    GekkoSessionEvent** session_events = gekko_session_events(session, &session_event_count);

    for (int i = 0; i < session_event_count; i++) {
        const GekkoSessionEvent* event = session_events[i];

        switch (event->type) {
        case GekkoPlayerSyncing:
            printf("🔴 player syncing\n"); fflush(stdout);
            SDL_Log("[netplay sess=%08x] event=syncing", s_session_uuid);
            // FIXME: Show status to the player
            break;

        case GekkoPlayerConnected:
            printf("🔴 player connected\n"); fflush(stdout);
            SDL_Log("[netplay sess=%08x] event=connected", s_session_uuid);
            break;

        case GekkoPlayerDisconnected:
            printf("🔴 player disconnected\n"); fflush(stdout);
            SDL_Log("[netplay sess=%08x] event=peer-disconnected frame=%d",
                    s_session_uuid, s_last_advance_frame);
            SDL_strlcpy(s_session_end_reason, "peer-disconnected", sizeof(s_session_end_reason));
            s_session_end_frame = s_last_advance_frame;

            // Task #144: this is a post-handoff (RUNNING-phase) session
            // failure exactly like the MIST reject / CONNECTING-deadline
            // cases below already route through DirectP2P_NotifySessionFailed
            // — latch BEFORE handle_disconnection so the teardown
            // callback that handle_disconnection's EXITING pass eventually
            // fires (direct_p2p_on_teardown) sees the latch and parks
            // FAILED_HANDSHAKE with a distinct "Opponent disconnected."
            // overlay instead of falling to IDLE (silent drop to attract).
            DirectP2P_NotifySessionFailed(session_fail_code_for_event(event->type), NULL);

            // Tier-1 netplay diag — Item 4: dump packet ring on disconnect
            // so we capture the most recent ~512 packets when the 5-second
            // GekkoNet timeout fires. Also Item 9: emit UDP-row deltas so
            // we can tell whether the kernel was dropping packets in the
            // silence window.
            {
                UdpSnmp now_snmp;
                SDL_zero(now_snmp);
                read_proc_net_snmp_udp(&now_snmp);
                if (now_snmp.valid && s_udp_snmp_session_start.valid) {
                    char line[256];
                    SDL_snprintf(line, sizeof(line),
                                 "[netplay sess=%08x] kernel-udp-stats: "
                                 "in_errors=%llu rcvbuf_errs=%llu noport_errs=%llu",
                                 s_session_uuid,
                                 (unsigned long long)(now_snmp.in_errors      - s_udp_snmp_session_start.in_errors),
                                 (unsigned long long)(now_snmp.rcvbuf_errors  - s_udp_snmp_session_start.rcvbuf_errors),
                                 (unsigned long long)(now_snmp.no_ports       - s_udp_snmp_session_start.no_ports));
                    netplay_log_lock();   // #36
                    netplay_log_line(line);
                    netplay_log_unlock();
                }
                SDLNetAdapter_DumpPacketRing(
                    netplay_utc_ms(),
                    (player_number == 0) ? "host" : "joiner",
                    s_session_uuid);
            }

            handle_disconnection();
            break;

        case GekkoSessionStarted:
            printf("🔴 session started\n"); fflush(stdout);
            SDL_Log("[netplay sess=%08x] event=session-started frame=%d",
                    s_session_uuid, s_last_advance_frame);
            session_state = NETPLAY_SESSION_RUNNING;
            // Task #119: the rescue window closes the moment the session
            // is real — from here GekkoNet's own traffic keeps the pair
            // alive and a punch payload has nothing left to rescue. The
            // running path is exactly as wide as it was pre-#119.
            LatePunch_Disarm();
#if ENABLE_PERF_TELEMETRY
            /* Task #69.3 — probed after session_state becomes RUNNING, so
             * the logged barrier= column reads 1 and the rest of the row
             * is the state the barrier inherits rather than one it
             * produced. This is no longer the line the barrier turns on
             * at: #72 widened Ldreq_BarrierActive() (gd3rd.c:652) to also
             * cover TRANSITIONING and CONNECTING, so the barrier has been
             * active since session_state first became TRANSITIONING
             * (netplay.c:2168 / :2114). */
            Ldreq_LogSessionProbe("session-running", s_last_advance_frame);
#endif
            // P-2.1 fix: re-seed last-advance now so the watchdog clock
            // starts from the RUNNING transition, not from CONNECTING-entry
            // in configure_gekko(). Multi-second handshakes would otherwise
            // make the first watchdog check after RUNNING fire spuriously.
            s_last_advance_utc_ms = netplay_utc_ms();
            s_watchdog_latched = false;
            break;

        case GekkoDesyncDetected:
        {
            const int frame = event->data.desynced.frame;
            const uint32_t local_cs = event->data.desynced.local_checksum;
            const uint32_t remote_cs = event->data.desynced.remote_checksum;
            printf("⚠️ desync detected at frame %d  local=0x%08x remote=0x%08x\n",
                   frame, local_cs, remote_cs);
            SDL_Log("[netplay sess=%08x] DESYNC frame=%d local=0x%08x remote=0x%08x",
                    s_session_uuid, frame, local_cs, remote_cs);
            // Dump pre-desync ring buffers + binary State to states/ for
            // offline triage. The dump function is post-event so it has no
            // per-frame perf cost; gating on s_diag_enabled lets a user
            // suppress the file writes if the SD card is constrained.
            if (SDL_GetAtomicInt(&s_diag_enabled)) {
                dump_desync_state(frame, local_cs, remote_cs);
                dump_input_history_on_desync(frame);
                // Tier-1 netplay diag — Item 4: also dump the packet ring
                // alongside the state/input dumps so all three artifacts
                // come out together. Useful for "did we miss an Inputs
                // packet right before the divergence?" investigations.
                SDLNetAdapter_DumpPacketRing(
                    netplay_utc_ms(),
                    (player_number == 0) ? "host-desync" : "joiner-desync",
                    s_session_uuid);
            }
            SDL_strlcpy(s_session_end_reason, "desync", sizeof(s_session_end_reason));
            s_session_end_frame = frame;
            // F8 — netplay Track A review finding. Port of 3sxtra's
            // GekkoDesyncDetected handler (/tmp/3sxtra/src/netplay/netplay.c:633-651):
            // treat a detected desync like a disconnect — clean input buffers
            // and soft-reset via handle_disconnection().
            //
            // Task #144: also latch a taxonomy code, same rationale as
            // GekkoPlayerDisconnected above — without this, teardown parks
            // IDLE and a desynced player sees the same silent drop as a
            // clean exit. CONNECT_FAIL_DESYNC_DETECTED gets its own overlay
            // text ("Session desynced...") so "opponent left" and "we
            // desynced" no longer read identically on screen.
            DirectP2P_NotifySessionFailed(session_fail_code_for_event(event->type), NULL);
            printf("[netplay] desync at frame %d — terminating session\n", frame);
            handle_disconnection();
            break;
        }

        case GekkoEmptySessionEvent:
        case GekkoSpectatorPaused:
        case GekkoSpectatorUnpaused:
            // Do nothing
            break;
        }
    }
}

static void process_events(bool drawing_allowed, NetplayRunTrace* trace) {
    const int advances_before = trace->advances;
    int game_event_count = 0;
#if ENABLE_PERF_TELEMETRY
    const Uint64 update_start_ns = SDL_GetTicksNS();
#endif
    GekkoGameEvent** game_events = gekko_update_session(session, &game_event_count);
#if ENABLE_PERF_TELEMETRY
    trace->update_ns += SDL_GetTicksNS() - update_start_ns;
#endif
    trace->game_events += game_event_count;
    int frames_rolled_back = 0;

    for (int i = 0; i < game_event_count; i++) {
        const GekkoGameEvent* event = game_events[i];

        switch (event->type) {
        case GekkoLoadEvent:
#if ENABLE_PERF_TELEMETRY
            const Uint64 load_start_ns = SDL_GetTicksNS();
#endif
#if defined(DEBUG)
            /* Black-BG investigation 2026-04-24 — Experiment 5a.
             * Count GekkoLoadEvents (rollback restores). Emit every 10th. */
            {
                static int s_load_count = 0;
                s_load_count++;
                if (s_load_count <= 40 || (s_load_count % 10) == 0) {
                    fprintf(stderr, "[netplay] GekkoLoadEvent #%d (rollback restore)\n", s_load_count);
                }
            }
#endif
            // #145: a rollback past the exit-request frame re-simulates
            // it — the request only stands if the corrected timeline
            // re-executes VS_Result case 7 (which re-latches it).
            if (menu_exit_request_erased_by_load(event->data.load.frame, s_menu_exit_request_frame)) {
                s_menu_exit_request_frame = -1;
            }
            load_state_from_event(event);
#if ENABLE_PERF_TELEMETRY
            trace->load_ns += SDL_GetTicksNS() - load_start_ns;
#endif
            trace->loads++;
            break;

        case GekkoAdvanceEvent:
            const bool rolling_back = event->data.adv.rolling_back;
            const bool drawable = drawing_allowed && !rolling_back;
#if ENABLE_PERF_TELEMETRY
            const Uint64 advance_start_ns = SDL_GetTicksNS();
#endif
            advance_game(event, drawable);
#if ENABLE_PERF_TELEMETRY
            trace->advance_ns += SDL_GetTicksNS() - advance_start_ns;
#endif
            trace->advances++;
            trace->drawable_advances += drawable ? 1 : 0;
            trace->rollback_advances += rolling_back ? 1 : 0;
            frames_rolled_back += rolling_back ? 1 : 0;
            // Tier-1 netplay diag — Item 5: stamp UTC ms of the most recent
            // advance regardless of whether it was the rolling-back leg.
            // Rollback advances are still progress; an absence of any advance
            // for >250 ms is what we want to flag.
            s_last_advance_utc_ms = netplay_utc_ms();
            if (s_watchdog_latched) {
                // Reset the latch so the next stall episode emits.
                s_watchdog_latched = false;
            }
            break;

        case GekkoSaveEvent:
#if ENABLE_PERF_TELEMETRY
            const Uint64 save_start_ns = SDL_GetTicksNS();
#endif
            save_state(event);
#if ENABLE_PERF_TELEMETRY
            trace->save_ns += SDL_GetTicksNS() - save_start_ns;
#endif
            trace->saves++;
            break;

        case GekkoEmptyGameEvent:
            // Do nothing
            break;
        }
    }

    frame_max_rollback = SDL_max(frame_max_rollback, frames_rolled_back);

    // Tier-1 netplay diag — Item 6: bucket per-batch rollback depth. Done
    // here (not inside the per-event switch) so a single update_session()
    // call producing N rolling-back advances is counted as one rollback of
    // depth N, matching what users observe.
    if (frames_rolled_back > 0) {
        // P-2.5 fix: bucket 0 is filled by the else branch below; the chain
        // here only runs when frames_rolled_back >= 1, so a final `else b=0`
        // would be unreachable.
        int b;
        if (frames_rolled_back >= 8)      b = 4;
        else if (frames_rolled_back >= 5) b = 3;
        else if (frames_rolled_back >= 3) b = 2;
        else                              b = 1;  // 1..2
        s_rb_buckets[b]++;
    } else if (trace->advances > advances_before) {
        // Empty updates have their own no-draw counter; do not disguise them
        // as ordinary zero-rollback advance batches.
        s_rb_buckets[0]++;
    }
}

static void step_logic(bool drawing_allowed, NetplayRunTrace* trace) {
#if ENABLE_PERF_TELEMETRY
    const Uint64 session_start_ns = SDL_GetTicksNS();
#endif
    process_session();
#if ENABLE_PERF_TELEMETRY
    trace->session_ns += SDL_GetTicksNS() - session_start_ns;
#endif
    process_events(drawing_allowed, trace);
}

static void update_network_stats() {
    if (stats_update_timer == 0) {
        GekkoNetworkStats net_stats;
        gekko_network_stats(session, player_handle ^ 1, &net_stats);

        network_stats.ping = net_stats.avg_ping;
        network_stats.delay = DELAY_FRAMES;

        if (frame_max_rollback < network_stats.rollback) {
            // Don't decrease the reading by more than a frame to account for
            // the opponent not pressing buttons for 1-2 seconds
            network_stats.rollback -= 1;
        } else {
            network_stats.rollback = frame_max_rollback;
        }

        // Lightweight telemetry: one heartbeat line per second so wrapper
        // logs show where the session was when a drop happened. Only emit
        // while actually in an active session to avoid pre-match noise.
        // The diag gate suppresses the noisy extras (kbps/jitter) but the
        // baseline stats still land — they cost nothing.
        if (session_state == NETPLAY_SESSION_RUNNING) {
            const bool diag = SDL_GetAtomicInt(&s_diag_enabled) != 0;

            // Tier-1 netplay diag — Item 2: kb_sent / kb_received are PER-SECOND
            // rates (gekkonet net.h:136-137 — kb_sent_per_sec / kb_received_per_sec
            // exposed via the GekkoNetworkStats accessor). Emit them as kbps_tx /
            // kbps_rx directly; the previous code subtracted from cached "last"
            // values, but the cached values were already rates, so the subtraction
            // produced spurious zero-or-negative numbers. Drop the s_hb_last_kb_*
            // statics entirely.
            const float kbps_tx = net_stats.kb_sent;
            const float kbps_rx = net_stats.kb_received;

            // Tier-1 netplay diag — Item 3: per-packet-type delta counters.
            uint64_t cur_tx[8] = { 0 }, cur_rx[8] = { 0 };
            SDLNetAdapter_SnapshotCounters(cur_tx, cur_rx);
            uint64_t d_tx[8], d_rx[8];
            for (int k = 0; k < 8; k++) {
                d_tx[k] = cur_tx[k] - s_hb_prev_pkt_tx[k];
                d_rx[k] = cur_rx[k] - s_hb_prev_pkt_rx[k];
            }
            SDL_memcpy(s_hb_prev_pkt_tx, cur_tx, sizeof(s_hb_prev_pkt_tx));
            SDL_memcpy(s_hb_prev_pkt_rx, cur_rx, sizeof(s_hb_prev_pkt_rx));

            // Item 6: snapshot + reset rb_buckets so the heartbeat carries the
            // delta since the last hb (matching the kbps semantics). Buckets
            // 0..4 = rb depth 0, 1-2, 3-4, 5-7, 8+.
            uint32_t rb_snap[5];
            SDL_memcpy(rb_snap, s_rb_buckets, sizeof(rb_snap));
            SDL_zeroa(s_rb_buckets);
            const uint32_t hold_snap = s_hb_no_draw_holds;
            const uint32_t catchup_snap = s_hb_catchups;
            s_hb_no_draw_holds = 0;
            s_hb_catchups = 0;

            char line[512];
            if (diag) {
                SDL_snprintf(line, sizeof(line),
                        "[netplay sess=%08x] hb f=%d ping=%d jitter=%d "
                        "kbps_tx=%.1f kbps_rx=%.1f rb=%d behind=%.1f "
                        "tx=I:%llu,A:%llu,SH:%llu,NH:%llu "
                        "rx=I:%llu,A:%llu,SH:%llu,NH:%llu "
                        "rb_hist=0:%u,1:%u,2:%u,3:%u,4:%u "
                        "holds=%u catchups=%u last_hold_f=%d",
                        s_session_uuid,
                        s_last_advance_frame,
                        (int)network_stats.ping,
                        (int)net_stats.jitter,
                        (double)kbps_tx,
                        (double)kbps_rx,
                        (int)network_stats.rollback,
                        (double)frames_behind,
                        (unsigned long long)d_tx[1],   // Inputs
                        (unsigned long long)d_tx[3],   // InputAck
                        (unsigned long long)d_tx[6],   // SessionHealth
                        (unsigned long long)d_tx[7],   // NetworkHealth
                        (unsigned long long)d_rx[1],
                        (unsigned long long)d_rx[3],
                        (unsigned long long)d_rx[6],
                        (unsigned long long)d_rx[7],
                        rb_snap[0], rb_snap[1], rb_snap[2], rb_snap[3], rb_snap[4],
                        hold_snap, catchup_snap, s_last_no_draw_frame);
            } else {
                SDL_snprintf(line, sizeof(line),
                        "[netplay hb] f=%d ping=%d rb=%d behind=%.1f",
                        s_last_advance_frame,
                        (int)network_stats.ping,
                        (int)network_stats.rollback,
                        (double)frames_behind);
            }
            // The one-second row retains its stderr and session-log surfaces,
            // but heartbeat_enqueue never waits for either sink. The logger
            // thread applies the same file budget and flushes after gameplay
            // has handed off the row.
            heartbeat_enqueue(line);
        }

        frame_max_rollback = 0;
        stats_update_timer = STATS_UPDATE_TIMER_MAX;
    }

    stats_update_timer -= 1;
    stats_update_timer = SDL_max(stats_update_timer, 0);
}

static void run_netplay() {
    NetplayRunTrace trace;
    SDL_zero(trace);
    s_hold_last_frame = false;
#if ENABLE_PERF_TELEMETRY
    const Uint64 run_start_ns = SDL_GetTicksNS();
#endif
    const bool catch_up = need_to_catch_up() && (frame_skip_timer == 0);
    step_logic(!catch_up, &trace);

    if (catch_up) {
        step_logic(true, &trace);
        frame_skip_timer = FRAME_SKIP_TIMER_MAX;
        s_hb_catchups++;
    }

    s_hold_last_frame = should_hold_last_frame(session_state, trace.drawable_advances);
    if (s_hold_last_frame) {
        s_hb_no_draw_holds++;
        s_last_no_draw_frame = s_last_advance_frame;
#if ENABLE_PERF_TELEMETRY
        if (!s_no_draw_episode) {
            char line[320];
            SDL_snprintf(line, sizeof(line),
                         "[netplay sess=%08x] no-draw hold f=%d behind=%.1f "
                         "catchup=%d events=%d advances=%d rollback_adv=%d "
                         "loads=%d saves=%d",
                         s_session_uuid, s_last_advance_frame, (double)frames_behind,
                         catch_up ? 1 : 0, trace.game_events, trace.advances,
                         trace.rollback_advances, trace.loads, trace.saves);
            (void)netplay_log_line_try(line);
        }
#endif
        s_no_draw_episode = true;
    } else {
        s_no_draw_episode = false;
    }

    frame_skip_timer -= 1;
    frame_skip_timer = SDL_max(frame_skip_timer, 0);

    // Update stats

    update_network_stats();

#if ENABLE_PERF_TELEMETRY
    const Uint64 run_ns = SDL_GetTicksNS() - run_start_ns;
    if (run_ns > 50 * SDL_NS_PER_MS) {
        char line[448];
        SDL_snprintf(line, sizeof(line),
                     "[netplay-perf sess=%08x] f=%d total=%.1fms session=%.1f "
                     "gekko_update=%.1f load=%.1f advance=%.1f save=%.1f "
                     "events=%d advances=%d rollback_adv=%d drawable_adv=%d "
                     "loads=%d saves=%d catchup=%d hold=%d",
                     s_session_uuid, s_last_advance_frame, (double)run_ns / 1e6,
                     (double)trace.session_ns / 1e6, (double)trace.update_ns / 1e6,
                     (double)trace.load_ns / 1e6, (double)trace.advance_ns / 1e6,
                     (double)trace.save_ns / 1e6, trace.game_events, trace.advances,
                     trace.rollback_advances, trace.drawable_advances, trace.loads,
                     trace.saves, catch_up ? 1 : 0, s_hold_last_frame ? 1 : 0);
        (void)netplay_log_line_try(line);
    }
#endif
}

bool Netplay_ShouldHoldLastFrame(void) {
    return s_hold_last_frame;
}

void Netplay_SetParams(int player, const char* ip) {
    SDL_assert(player == 1 || player == 2);
    player_number = player - 1;

    // Callers may pass a stack-local buffer (e.g. direct_p2p's host path
    // copies NET_GetAddressString into `char src_ip[64]` before calling
    // do_handoff). Storing the raw pointer dangles by the time
    // configure_gekko() stringifies remote_address_str on a later frame.
    // Copy the string into module-local storage so remote_ip stays valid
    // for the lifetime of the netplay session.
    if (ip != NULL) {
        SDL_strlcpy(remote_ip_buf, ip, sizeof(remote_ip_buf));
        remote_ip = remote_ip_buf;
    } else {
        remote_ip_buf[0] = '\0';
        remote_ip = NULL;
    }

    if (ip != NULL && SDL_strcmp(ip, "127.0.0.1") == 0) {
        switch (player_number) {
        case 0:
            local_port = 50000;
            remote_port = 50001;
            break;

        case 1:
            local_port = 50001;
            remote_port = 50000;
            break;
        }
    } else {
        local_port = 50000;
        remote_port = 50000;
    }
}

void Netplay_SetRemotePort(unsigned short port) {
    remote_port = port;
}

bool Netplay_IsRemoteIpSet(void) {
    return remote_ip != NULL;
}

void Netplay_BeginDirectP2P() {
    if (remote_ip == NULL) {
        return;
    }
    direct_p2p_pending = true;
}

void Netplay_TickDirectP2P() {
    if (!direct_p2p_pending) {
        return;
    }

    // Cold-launch race: the wrapper-OSD "Host Game" path re-execs the
    // process with --direct-p2p-handoff, and the orchestrator can complete
    // (UPnP + STUN + hole-punch) while Init_Task is still in its
    // Aload/Warning/2nd phase. If setup_vs_mode() fires before
    // Init_Task_End runs, Init_Task_End clobbers G_No[0]=1 and registers
    // TASK_GAME late, dropping the game into Loop_Demo with G_No[1]=12 —
    // which falls through Loop_Demo's default case to Next_Demo_Loop,
    // permanently locking attract mode with session_state=TRANSITIONING
    // and the game engine unable to advance through Game12 → char select.
    // Defer until init finalizes so Game_Task is ready and G_No[0]=2
    // survives setup_vs_mode.
    if (task[TASK_INIT].condition != 0) {
        return;
    }

    direct_p2p_pending = false;
    setup_vs_mode();

    SDL_zeroa(input_history);
    frames_behind = 0;
    frame_skip_timer = 0;
    transition_ready_frames = 0;
    s_mist_handshake_done = false;
    s_mist_handshake_attempts = 0;
    s_mist_peer_hello_ok = false;

    session_state = NETPLAY_SESSION_TRANSITIONING;

    // Task #119: the pre-session rescue window opens here — socket, peer
    // endpoint and token are all committed (do_handoff ran before nav
    // called Netplay_BeginDirectP2P). Arm the late-punch layer so a peer
    // whose race ended one-sided can still confirm against us and, via
    // the S2 retry's fresh socket, be relearned. Kill switch first: the
    // pre-#119 behaviour is one config flip away, so any regression in
    // the field is attributable in minutes.
    if (s_punch_token_valid && remote_ip != NULL && remote_port != 0 &&
        !Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_LATE_PUNCH)) {
        LatePunch_Arm(acquire_active_socket(), s_punch_token,
                      remote_ip, remote_port);
    }
}

void Netplay_Run() {
    /* Per-outer-frame output decision. run_netplay may set it again for the
     * CONNECTING/RUNNING branches below. */
    s_hold_last_frame = false;
#if ENABLE_PERF_TELEMETRY
    /* Task #69.3 — the whole session-start window as a timeline, not two
     * point samples. TRANSITIONING and CONNECTING both step the engine
     * with the barrier ON now: #72 widened Ldreq_BarrierActive()
     * (gd3rd.c:652) from RUNNING-only to also cover TRANSITIONING and
     * CONNECTING, so this whole pre-RUNNING window is barrier-gated, not
     * barrier-free. This probe still times the
     * window end-to-end so any residual skew between the two peers still
     * shows up in the log.
     *
     * Both arms are capped because this is a telemetry build that ships:
     * TRANSITIONING has NO wall-clock deadline (it waits on the peer, and
     * the comment on that branch says so), so an uncapped per-frame log
     * would be unbounded on a slow-peer session. 120 pre-RUNNING frames
     * covers a nominal start with room to spare — measured 66 and 12 on
     * the two peers of the 2026-08-25 run — and the 8-frame RUNNING tail
     * is only there to show what the barrier inherited. Steady-state
     * RUNNING is not this probe's job; the per-slot residue probe and
     * tools/ldreq-timing cover it.
     *
     * `s_last_advance_frame` is 0 until GekkoNet starts advancing, so the
     * pre-RUNNING rows carry their own ordinal instead — otherwise the
     * whole start window logs as frame=0 and cannot be read as a
     * timeline. */
    {
        static int pre_running_probes = 0;
        static int running_probes = 0;

        if (session_state == NETPLAY_SESSION_TRANSITIONING || session_state == NETPLAY_SESSION_CONNECTING) {
            if (pre_running_probes < 120) {
                pre_running_probes += 1;
                Ldreq_LogSessionProbe(session_state == NETPLAY_SESSION_TRANSITIONING ? "transitioning" : "connecting",
                                      pre_running_probes);
            }
        } else if (session_state == NETPLAY_SESSION_RUNNING && running_probes < 8) {
            running_probes += 1;
            Ldreq_LogSessionProbe("running", s_last_advance_frame);
        }
    }
#endif

    switch (session_state) {
    case NETPLAY_SESSION_TRANSITIONING:
        /* Task #119: keep speaking the authenticated punch protocol for
         * the whole pre-session window — see late_punch_service. During
         * this phase the MIST drain (mist_netio_recv) is the receive-side
         * hook. */
        late_punch_service();
        /* S3: user-reachable abort — the MIST retry window below can
         * legitimately spend up to ~20 s waiting on a slow-booting peer,
         * and pre-S3 there was no way out but killing the process. Check
         * BEFORE clean_input_buffers() so the pad state keyConvert()
         * captured this frame is still visible. */
        if (connect_abort_tick("transitioning")) {
            break;
        }
        if (game_ready_to_run_character_select()) {
            transition_ready_frames += 1;
        } else {
            transition_ready_frames = 0;
            /* Review L-5: the hold-START abort armed above is live during
             * this loading phase too — say so, like the verifying/syncing
             * texts do. (This phase is bounded ONLY by that abort: it
             * waits on game_ready_to_run_character_select with no
             * wall-clock deadline.) */
            SDL_snprintf(s_connect_status, sizeof(s_connect_status),
                         "Match found! Loading... START quits");
            clean_input_buffers();
            step_game(true);
        }

        if (transition_ready_frames >= 2) {
            // R-1 — Layer 3 MIST handshake, unconditional for every live
            // session path (direct-P2P punch, LAN CLI). Runs
            // AFTER the punch/handoff produced the session socket and
            // BEFORE gekko_create() takes it over, on the exact socket
            // configure_gekko() will hand to GekkoNet. Non-MIST frames
            // (stray punch keepalives, early GekkoNet packets from a
            // legacy peer) are dropped inside the runner by the "MIST"
            // magic gate. See mist_handshake.h for wire format.
            if (!s_mist_handshake_done) {
                SDL_snprintf(s_connect_status, sizeof(s_connect_status),
                             "Verifying opponent (%ds)... START quits",
                             s_mist_handshake_attempts / 2);
                // S3: incremental pump — one bounded slice per frame
                // instead of blocking the whole 500 ms attempt budget
                // (see the s_hs_pump block comment). The game keeps
                // rendering + sampling input at full frame rate for the
                // entire retry window.
                MistHandshakeResult hs = MIST_HS_FAIL;
                bool slice_pending = false;
                if (!s_hs_pump_active && !mist_pump_start()) {
                    // Local transport error — reason already cached;
                    // fold through the gate as a hard failure.
                    mist_pump_stop();
                } else {
                    const MistPumpStatus ps = mist_handshake_pump(
                        &s_hs_pump, &s_hs_io_template, &s_mist_peer_hello_ok,
                        s_mist_reject_reason, sizeof(s_mist_reject_reason));
                    if (ps == MIST_PUMP_PENDING) {
                        slice_pending = true;
                    } else {
                        s_hs_pump_active = false; // this attempt concluded
                        hs = (ps == MIST_PUMP_OK)        ? MIST_HS_OK
                             : (ps == MIST_PUMP_TIMEOUT) ? MIST_HS_TIMEOUT
                                                         : MIST_HS_FAIL;
                    }
                }
                if (slice_pending) {
                    break; // slice done; stay TRANSITIONING at full rate
                }
                // Retry policy (peer-skew tolerance, attempt cap, the
                // exhaustion message) lives in mist_handshake_gate_next
                // — extracted for unit testability (adv-review M-5).
                const MistGateAction act = mist_handshake_gate_next(
                    hs, &s_mist_handshake_attempts, MIST_HANDSHAKE_MAX_ATTEMPTS,
                    s_mist_reject_reason, sizeof(s_mist_reject_reason));
                if (act == MIST_GATE_PROCEED) {
                    s_mist_handshake_done = true;
                    mist_pump_stop();
                } else if (act == MIST_GATE_RETRY) {
                    // Silent peer — likely still booting toward its own
                    // gate (cold-launch skew). Stay in TRANSITIONING; the
                    // next tick re-arms the pump (the resolved peer
                    // address stays cached in s_hs_peer_addr). Each
                    // attempt keeps the 500 ms budget. An explicit
                    // reject never lands here.
                    break;
                } else {
                    mist_pump_stop();
                    printf("[netplay] MIST handshake failed: %s\n",
                           s_mist_reject_reason);
                    // Surface the reason on screen: latch it into the
                    // direct-P2P overlay BEFORE entering EXITING — the
                    // teardown callback that runs there converts the
                    // latch into DIRECT_P2P_FAILED_HANDSHAKE so the
                    // overlay shows ERROR + reason instead of a silent
                    // drop back to attract mode.
                    DirectP2P_NotifySessionRejected(s_mist_reject_reason);
                    session_state = NETPLAY_SESSION_EXITING;
                    clean_input_buffers();
                    Soft_Reset_Sub();
                    break;
                }
            }
            configure_gekko();
            session_state = NETPLAY_SESSION_CONNECTING;
            /* S3: arm the CONNECTING deadline + progress clock. */
            s_connecting_since_ms = SDL_GetTicks();
            s_connecting_last_log_ms = s_connecting_since_ms;
            s_abort_hold_frames = 0;
        }

        break;

    case NETPLAY_SESSION_CONNECTING: {
        /* Task #119: still inside the rescue window — GekkoNet owns the
         * socket now, so the receive-side hook has moved to the adapter's
         * receive_data; the send/relearn side stays here. */
        late_punch_service();
        /* S3 Part A: CONNECTING is now (a) user-abortable, (b) bounded
         * by a wall-clock deadline with an attributable reason, and
         * (c) log-visible via a 5 s progress line — see the S3 comment
         * block at the top of the file for why GekkoNet cannot be
         * trusted to time this state out itself. */
        if (connect_abort_tick("connecting")) {
            break;
        }
        const uint64_t now = SDL_GetTicks();
        if (s_connecting_since_ms == 0) { /* safety: entered without arming */
            s_connecting_since_ms = now;
            s_connecting_last_log_ms = now;
        }
        if (ConnectFail_DeadlineExpired(now, s_connecting_since_ms,
                                        CONNECT_TIMEOUT_CONNECTING_MS)) {
            // S3-review HIGH-2: tagged DEADLINE, not FAIL — the ONE
            // attributed "[netplay-connect] FAIL code=..." line for this
            // outcome is emitted by the orchestrator's FAILED_HANDSHAKE
            // report after the NotifySessionFailed latch below parks it
            // there at teardown. A second FAIL-tagged line here would
            // double-report the same terminal outcome.
            char line[192];
            SDL_snprintf(line, sizeof(line),
                         "[netplay-connect] DEADLINE code=%s stage=connecting — no "
                         "GekkoSessionStarted within %u ms (peer never synced); "
                         "attributed FAIL line follows from the teardown path",
                         ConnectFail_Code(CONNECT_FAIL_TIMEOUT_CONNECTING),
                         (unsigned)CONNECT_TIMEOUT_CONNECTING_MS);
            Netplay_LogConnectEvent(line);
            if (s_session_end_reason[0] == '\0') {
                SDL_strlcpy(s_session_end_reason, "connect-timeout",
                            sizeof(s_session_end_reason));
                s_session_end_frame = s_last_advance_frame;
            }
            /* Surface through the taxonomy path: the latch parks the
             * orchestrator in FAILED_HANDSHAKE at teardown so the
             * overlay shows ERROR + the reason on every entry path. */
            DirectP2P_NotifySessionFailed(CONNECT_FAIL_TIMEOUT_CONNECTING, NULL);
            handle_disconnection();
            break;
        }
        const unsigned elapsed_s = (unsigned)((now - s_connecting_since_ms) / 1000u);
        if (now - s_connecting_last_log_ms >= 5000) {
            s_connecting_last_log_ms = now;
            SDL_Log("[netplay sess=%08x] still CONNECTING after %u s — waiting for "
                    "GekkoNet sync (deadline %u s)",
                    s_session_uuid, elapsed_s,
                    (unsigned)(CONNECT_TIMEOUT_CONNECTING_MS / 1000u));
        }
        SDL_snprintf(s_connect_status, sizeof(s_connect_status),
                     "Syncing with opponent (%us)... START quits", elapsed_s);
        run_netplay();
        break;
    }

    case NETPLAY_SESSION_RUNNING:
        /* S3: leaving CONNECTING — retire its bookkeeping/status. */
        if (s_connecting_since_ms != 0) {
            s_connecting_since_ms = 0;
            s_abort_hold_frames = 0;
            s_connect_status[0] = '\0';
        }
        run_netplay();
        // #145: fire a simulation-latched menu exit only once its frame
        // can no longer be rolled back (<= pred_window frames, ~133 ms at
        // the default 8 — the exit fade covers it). Re-check RUNNING:
        // run_netplay may have torn down via disconnect/desync, which
        // owns its own reason.
        if (session_state == NETPLAY_SESSION_RUNNING &&
            menu_exit_request_confirmed(s_sim_frame, s_menu_exit_request_frame, s_pred_window)) {
            s_menu_exit_request_frame = -1;
            if (s_session_end_reason[0] == '\0') {
                SDL_strlcpy(s_session_end_reason, "user-canceled", sizeof(s_session_end_reason));
                s_session_end_frame = s_last_advance_frame;
            }
            session_state = NETPLAY_SESSION_EXITING;
        }
        break;

    case NETPLAY_SESSION_EXITING:
        // S3: retire the CONNECTING bookkeeping + status text so the next
        // session (and the idle screen) starts clean.
        s_connecting_since_ms = 0;
        s_connecting_last_log_ms = 0;
        s_abort_hold_frames = 0;
        s_connect_status[0] = '\0';
        // Final session-end summary. Logged once per session; gives offline
        // log readers a clean correlation point — pairing two friends' logs
        // is just matching session_started_unix_ms + the role-distinguished
        // UUID. Reason is set by whichever callsite initiated teardown
        // (peer-disconnected / desync / user-canceled / unknown if none).
        if (s_session_uuid != 0) {
            uint64_t dur_ms = (uint64_t)SDL_GetTicks() - s_session_started_ticks_ms;
            GekkoNetworkStats final_stats;
            SDL_zero(final_stats);
            if (session != NULL) {
                gekko_network_stats(session, player_handle ^ 1, &final_stats);
            }
            // Tier-1 netplay diag — Item 2: kb_sent / kb_received are PER-SECOND
            // rates (gekkonet net.h:136-137), not session totals. Label the
            // session-end fields explicitly as last-window rates so log
            // consumers don't confuse them with cumulative bytes.
            char line[512];
            SDL_snprintf(line, sizeof(line),
                    "[netplay sess=%08x] session-end reason=%s frame=%d duration_ms=%llu "
                    "final_ping=%d final_jitter=%d "
                    "last_window_kbps_tx=%.1f last_window_kbps_rx=%.1f",
                    s_session_uuid,
                    s_session_end_reason[0] ? s_session_end_reason : "unknown",
                    s_session_end_frame,
                    (unsigned long long)dur_ms,
                    (int)final_stats.avg_ping,
                    (int)final_stats.jitter,
                    (double)final_stats.kb_sent,
                    (double)final_stats.kb_received);
            heartbeat_disable_and_drain();
            netplay_log_lock();   // #36: held across session-end + close
            netplay_log_line(line);

            // Tier-1 netplay diag — Item 9: final UDP-row delta on session-end.
            // P-1.2 fix: the peer-disconnect handler (process_session) already
            // emits this exact SNMP delta line just before transitioning to
            // EXITING. Skip it here to avoid double-logging the same numbers.
            // For all other end reasons (desync, user-canceled, unknown) the
            // disconnect handler did not run, so we still emit on this path.
            if (SDL_strcmp(s_session_end_reason, "peer-disconnected") != 0) {
                UdpSnmp end_snmp;
                SDL_zero(end_snmp);
                if (read_proc_net_snmp_udp(&end_snmp) && end_snmp.valid && s_udp_snmp_session_start.valid) {
                    char snmp_line[256];
                    SDL_snprintf(snmp_line, sizeof(snmp_line),
                                 "[netplay sess=%08x] kernel-udp-stats: "
                                 "in_errors=%llu rcvbuf_errs=%llu noport_errs=%llu",
                                 s_session_uuid,
                                 (unsigned long long)(end_snmp.in_errors      - s_udp_snmp_session_start.in_errors),
                                 (unsigned long long)(end_snmp.rcvbuf_errors  - s_udp_snmp_session_start.rcvbuf_errors),
                                 (unsigned long long)(end_snmp.no_ports       - s_udp_snmp_session_start.no_ports));
                    netplay_log_line(snmp_line);
                }
            }

            // Tier-1 netplay diag — Item 7: close the netplay log file once
            // we've emitted session-end + the final SNMP delta.
            netplay_log_close();
            netplay_log_unlock();

            s_session_uuid = 0;  // Mark closed so a stray re-entry doesn't double-log.
        }

        // Task #119: stop answering punches BEFORE the socket dies, and
        // retire the token with the session that owned it — the next
        // session derives its own. Idempotent (SessionStarted usually
        // already disarmed).
        LatePunch_Disarm();
        s_punch_token_valid = false;

        // Step 6 (plan P-2 #18): fire the orchestrator-registered teardown
        // callback before we touch the socket. The callback needs the STUN
        // socket (or at least its port via getsockname) valid to release
        // the UPnP mapping cleanly.
        if (session_teardown_cb != NULL) {
            session_teardown_cb();
        }

        if (session != NULL) {
            gekko_destroy(&session);
            SDLNetAdapter_Destroy();
        }

        // Step 6: destroy the pre-punched STUN socket (if any) now that
        // GekkoNet and the adapter have released their references. Mirrors
        // upstream /tmp/3sxtra/src/netplay/netplay.c:1009-1012.
        if (stun_socket != NULL) {
            NET_DestroyDatagramSocket(stun_socket);
            stun_socket = NULL;
        }

        if (p2p_sock != NULL) {
            NET_DestroyDatagramSocket(p2p_sock);
            p2p_sock = NULL;
            NET_Quit();
        }

        // R-1: clear handshake state so the next session (direct P2P,
        // LAN, etc.) starts without carrying stale flags.
        // S3: also release anything the incremental pump still holds
        // (cached peer address / last datagram) — a hold-START abort or
        // menu exit can land here mid-handshake.
        mist_pump_stop();
        s_mist_handshake_done = false;
        s_mist_handshake_attempts = 0;
        s_mist_peer_hello_ok = false;

        // C1 fix: setup_vs_mode() (above) zeroes Convert_Buff/Check_Buff for
        // the duration of the session so the netplay simulation runs with
        // identity button mappings. Since Save_Game_Data() (sys_sub.c) reads
        // settings FROM Convert_Buff, any save that happens while those
        // buffers are still zeroed would persist zeroed settings
        // (Difficulty/pad mappings/volumes) to disk.
        //
        // The clean-exit path here has no reload-from-disk of its own
        // (unlike disconnect, which forces a hard Soft_Reset_Sub ->
        // TASK_INIT -> settings reload). NOTE (corrected 2026-08-23,
        // review C1 addendum): game.c Game06's post-game auto-save
        // (SaveInit(SAVE_FILE_SETTINGS, SAVE_MODE_SAVE), case 5) is NOT
        // reachable for a netplay session and so was never actually at risk
        // here — proven via manage.c: Game_Manage_1st (C_No[0]==0, called
        // every round from Game2_1 -> Game_Management()) sets
        // Round_Operator[p] = (plw[p].wu.wu_operator != 0) every round
        // (manage.c:194-203); netplay.c:371-372 unconditionally forces both
        // plw[].wu.wu_operator = 1, so Round_Operator[WINNER] is always true
        // in netplay; Game_Manage_10th's routing gate
        // (manage.c:1143, `Mode_Type == MODE_VERSUS || Mode_Type == 5 ||
        // Round_Operator[WINNER]`) is therefore always true for netplay
        // regardless of Mode_Type, so it always takes G_No[1]=3 (Game03,
        // whose own switch(Mode_Type) explicitly handles MODE_NETWORK via
        // the VS_Result menu screen, game.c ~774-798) and never
        // G_No[1]=4 (Game04, the arcade-only continue-screen flow that is
        // the only route to Game07/Game08, the sole two writers of
        // `G_No[1] = 6`, game.c:1272 and :1317). Game_Manage_10th's
        // Check_Ending() call is unreachable for netplay, same reason
        // (Play_Type==1, forced by both wu_operator being set, short-
        // circuits it at manage.c:1202 before it can route to Game08
        // either).
        //
        // The real, still-current motivation for this restore: leaving
        // Convert_Buff/Check_Buff zeroed here persists forward to the NEXT
        // real settings save after the player returns to the main menu —
        // e.g. Option_Select (menu.c:778), or Game06's auto-save the next
        // time the player plays Arcade/Versus locally (Game06 *is*
        // reachable then). Restore both buffers from the current save_w[1]
        // here, mirroring the same restore done at the other safe call
        // sites (boot: sys_sub.c Setup_IO_ConvData -> Copy_Save_w();
        // settings load: savesub.c deserialize_settings; menu defaults:
        // menu.c).
        Copy_Save_w();
        Copy_Check_w();

        // Release the netplay-session suppression of the draw-players-
        // above-HUD gameplay reads: the session is fully torn down, so
        // subsequent LOCAL play gets the user's configured value back.
        // (The old ForceDisable latch was never cleared — a menu-initiated
        // netplay session left the suppression on until relaunch.)
        DrawPlayersAboveHud_SetNetplaySuppressed(false);

        session_state = NETPLAY_SESSION_IDLE;
        break;

    case NETPLAY_SESSION_IDLE:
        break;
    }
}

NetplaySessionState Netplay_GetSessionState() {
    return session_state;
}

// S3 — honest connect-phase progress text for NetplayScreen_Render.
// Non-empty only while TRANSITIONING/CONNECTING have something to say;
// replaces the old perpetual "Match found!" (which stayed on screen for
// the entire MIST retry window and CONNECTING phase, lying about what
// was happening).
const char* Netplay_GetConnectStatusText(void) {
    if (session_state != NETPLAY_SESSION_TRANSITIONING &&
        session_state != NETPLAY_SESSION_CONNECTING) {
        return "";
    }
    return s_connect_status;
}

// Arm-time predicate: netplay arms ONLY in verified-arcade balance state.
// Balance auto-selects once at boot (ArcadeBalance_Init) and is fixed for
// the process, so this answer cannot change under a live session. Peers
// that both pass the gate still cross-check the adapted data via the MIST
// handshake balance digest.
bool Netplay_ArmAllowed(void) {
    return ArcadeBalance_IsEnabled();
}

// Refusal path shared by every entry site (nav arm, direct-P2P handoff
// dispatch, in-game network menu): log, then surface the reason on
// screen through the existing direct-P2P overlay mechanism —
// the same route the MIST handshake reject path uses (ERROR + status
// text, rendered by NetplayScreen_Render -> DirectP2P_DrawOverlay).
void Netplay_RefuseArm(void) {
    /* Sized to hold the whole reason: ArcadeBalance_GetReason() is
     * arcade_balance.c's ps2_reason[192], and the two adapt_all_characters
     * failures fill ~120 of it -- with the 31-glyph prefix that is ~150,
     * which the previous msg[120] cut off before it ever reached the
     * overlay. The overlay word-wraps (direct_p2p_overlay.c), so length is
     * no longer a reason to shorten a diagnostic here. */
    char msg[256];
    SDL_snprintf(msg, sizeof(msg), "Netplay needs arcade balance - %s", ArcadeBalance_GetReason());
    // SDL_Log, not a bare printf: this fires during set_netplay_params()
    // at boot, long before any of the fflush(stdout) pairings elsewhere
    // in this file run, so a block-buffered stdout (any redirected or
    // piped run — i.e. every field log we actually receive) would swallow
    // the only record of WHY netplay refused. SDL_Log is unbuffered to
    // stderr and matches DirectP2P_RefuseSession's own line below it.
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[netplay] refusing to arm: %s", msg);
    DirectP2P_Init();
    DirectP2P_RefuseSession(msg);
}

void Netplay_HandleMenuExit() {
    // A refused arm (or a post-session MIST reject) parks the direct-P2P
    // overlay in the terminal FAILED_HANDSHAKE state so the reason stays
    // readable. Leaving the network menu is the user acknowledging it —
    // clear the overlay so it does not sit over subsequent local play.
    // Only when no session exists: live-session teardown owns its own path.
    if (session_state == NETPLAY_SESSION_IDLE && DirectP2P_GetState() == DIRECT_P2P_FAILED_HANDSHAKE) {
        DirectP2P_Cancel();
    }

    switch (session_state) {
    case NETPLAY_SESSION_IDLE:
    case NETPLAY_SESSION_EXITING:
        break;

    case NETPLAY_SESSION_TRANSITIONING:
    case NETPLAY_SESSION_CONNECTING:
        // Pre-match states are driven by real local UI, not by the Gekko
        // simulation — no rollback can erase the intent, act immediately.
        if (s_session_end_reason[0] == '\0') {
            SDL_strlcpy(s_session_end_reason, "user-canceled", sizeof(s_session_end_reason));
            s_session_end_frame = s_last_advance_frame;
        }
        session_state = NETPLAY_SESSION_EXITING;
        break;

    case NETPLAY_SESSION_RUNNING:
        // #145: RUNNING-state calls come only from inside the simulation
        // (menu.c VS_Result case 7, the sole caller, runs under
        // advance_game), so this frame may still be rolled back. Latch a
        // request stamped with the frame being simulated; Netplay_Run
        // fires it once menu_exit_request_confirmed() proves the frame
        // final. Keep the FIRST frame — case 7 re-calls every frame
        // until the exit fade completes, and the confirmation clock must
        // run from the earliest occurrence.
        if (s_menu_exit_request_frame < 0) {
            s_menu_exit_request_frame = (s_sim_frame >= 0) ? s_sim_frame : 0;
        }
        break;
    }
}

void Netplay_GetNetworkStats(NetworkStats* stats) {
    SDL_copyp(stats, &network_stats);
}

// Step 6 (docs/plan-stun-direct-p2p.md) — pre-punched STUN socket handoff.
// Mirrors /tmp/3sxtra/src/netplay/netplay.c:808-814 bodily. Ownership
// transfers from the caller (Step 7's orchestrator) into netplay.c. If we
// already hold a STUN socket and the caller is installing a different
// one, destroy the prior socket first to avoid a leak.
void Netplay_SetStunSocket(struct NET_DatagramSocket* socket) {
    NET_DatagramSocket* sock = (NET_DatagramSocket*)socket;
    if (stun_socket != NULL && stun_socket != sock) {
        // Task #131: the late-punch layer BORROWS this socket
        // (late_punch.c:26 — it never closes it), so destroying it out
        // from under an armed LatePunch_Tick would be a use-after-free
        // on the next tick's NET_SendDatagram.
        //
        // NO CALLER DOES THIS TODAY, and the attempt to build one failed
        // on a real structural guard rather than on luck: the only
        // production caller is do_handoff (direct_p2p.c:4522), which
        // nulls s_work.stun.socket immediately after
        // (direct_p2p.c:4532), and Tick's HANDOFF case re-enters
        // join_tick_handoff only while that field is non-NULL
        // (direct_p2p.c:5783). So a second handoff inside one session --
        // the one ordering that would install a DIFFERENT socket while
        // late-punch is armed -- cannot be reached. Both terminal paths
        // are correct for the same reason and by explicit ordering:
        // EXITING disarms (:2543) before the destroy below runs (:2562),
        // and SessionStarted disarms at :1798.
        //
        // This is therefore a guard on the INVARIANT, not a fix for a
        // live bug. It is here because the invariant is currently held
        // only by call-ordering discipline in another translation unit:
        // nothing stops a future second call site, and the failure mode
        // it would produce is a UAF in a thread that never named this
        // function. Disarming costs one branch on a once-per-session
        // path and cannot regress the handoff, which arms only AFTER
        // this call returns (netplay.c:2179, from Netplay_BeginDirectP2P).
        LatePunch_Disarm();
        NET_DestroyDatagramSocket(stun_socket);
    }
    stun_socket = sock;
}

// Task #119 — do_handoff hands over the S4a punch token alongside the
// punched socket, so the pre-session window can keep answering
// authenticated punches (late_punch.h). NULL/false clears it, which is
// what the non-orchestrator LAN CLI session path leaves in place: with
// no token the late-punch layer never arms and that path is bitwise
// unchanged.
void Netplay_SetPunchToken(const uint8_t* token, bool valid) {
    if (token != NULL && valid) {
        SDL_memcpy(s_punch_token, token, sizeof(s_punch_token));
        s_punch_token_valid = true;
    } else {
        SDL_memset(s_punch_token, 0, sizeof(s_punch_token));
        s_punch_token_valid = false;
    }
}

// Step 6 (plan P-2 #18) — single-slot teardown callback.
void Netplay_SetSessionTeardownCallback(void (*cb)(void)) {
    session_teardown_cb = cb;
}

// S3 (docs/plan-netplay-connection.md §5) — connection-establishment
// event logger. Most connection failures happen BEFORE configure_gekko()
// opens the per-session netplay log, so this opens it lazily: a failed
// attempt still leaves a netplay-<utc_ms>.log on disk carrying the
// attributed failure line + stage timings, which is exactly what a field
// report needs. Tees to SDL_Log via netplay_log_line and flushes
// immediately (failure paths have no heartbeat to drive the 1 Hz flush).
// #36: NO LONGER main-thread-only. The single implementation below runs
// under s_netplay_log_mu, so the lazy open + write + flush sequence is
// atomic with respect to any other caller on any thread. The observable
// behaviour of the main-thread path is byte-identical to the pre-#36
// version: same lazy open, same tee, same immediate flush.
static void netplay_log_connect_event(const char* line) {
    if (line == NULL || line[0] == '\0') {
        return;
    }
    netplay_log_lock();
    if (s_netplay_log == NULL) {
        netplay_log_open(netplay_utc_ms());
    }
    netplay_log_line(line);
    if (s_netplay_log != NULL) {
        fflush(s_netplay_log);
    }
    // #44: the same line, inside the same critical section, into the
    // tester-facing report — filtered to "[netplay-connect]" only. Wiring
    // it HERE rather than in netplay_log_line is what keeps heartbeats and
    // race spam out of it: those reach the session log by other paths.
    netplay_report_line_locked(line);
    netplay_log_unlock();
}

void Netplay_LogConnectEvent(const char* line) {
    netplay_log_connect_event(line);
}

// #36: the worker-thread door. Same function, named separately so a
// reader of direct_p2p.c can see at the call site which thread it is on
// (and so the main-thread contract of the original name is not silently
// widened out from under existing callers).
void Netplay_LogConnectEventMT(const char* line) {
    netplay_log_connect_event(line);
}

// === Tier-1 netplay diag — Item 10: SIGTERM flush hook ===
// main.c calls this after its main loop exits but before SDL teardown so
// the diagnostics survive wrapper-SIGTERM. Idempotent: a guard on
// s_session_uuid keeps it from double-running if the regular EXITING path
// already cleaned up.
void Netplay_FlushDiagnostics(void) {
    // A queued heartbeat may otherwise race the direct final writes and FILE
    // close below. Draining is intentionally allowed to wait on shutdown.
    heartbeat_disable_and_drain();
    // No active session — nothing to flush. The s_session_uuid==0 sentinel
    // is set in the EXITING branch after final-summary emission.
    //
    // F7: s_netplay_log is read under the lock. Since #36 a worker thread
    // can install that pointer (Netplay_LogConnectEventMT -> lazy open),
    // so an unsynchronized read here is a genuine data race even though
    // every caller today is the main thread on a shutdown path. Only the
    // read is inside the critical section; the work below re-takes the
    // lock around its own writes, which is fine — this early-out is a
    // "nothing to do at all" test, not a guard whose result the rest of
    // the function relies on staying true.
    netplay_log_lock();
    const bool have_log_file = (s_netplay_log != NULL);
    netplay_log_unlock();
    if (s_session_uuid == 0 && !have_log_file) {
        return;
    }

    if (s_session_uuid != 0) {
        // Best-effort final heartbeat. We don't have fresh GekkoNetworkStats
        // (the session may already be torn down or partially gone), so just
        // record the last observed advance frame + frames_behind.
        char line[256];
        SDL_snprintf(line, sizeof(line),
                     "[netplay sess=%08x] sigterm-flush last_frame=%d "
                     "frames_behind=%.1f reason=%s",
                     s_session_uuid,
                     s_last_advance_frame,
                     (double)frames_behind,
                     s_session_end_reason[0] ? s_session_end_reason : "sigterm");
        netplay_log_lock();   // #36
        netplay_log_line(line);

        // Item 9: SNMP UDP-row delta on shutdown.
        UdpSnmp end_snmp;
        SDL_zero(end_snmp);
        if (read_proc_net_snmp_udp(&end_snmp) && end_snmp.valid && s_udp_snmp_session_start.valid) {
            char snmp_line[256];
            SDL_snprintf(snmp_line, sizeof(snmp_line),
                         "[netplay sess=%08x] kernel-udp-stats: "
                         "in_errors=%llu rcvbuf_errs=%llu noport_errs=%llu",
                         s_session_uuid,
                         (unsigned long long)(end_snmp.in_errors      - s_udp_snmp_session_start.in_errors),
                         (unsigned long long)(end_snmp.rcvbuf_errors  - s_udp_snmp_session_start.rcvbuf_errors),
                         (unsigned long long)(end_snmp.no_ports       - s_udp_snmp_session_start.no_ports));
            netplay_log_line(snmp_line);
        }
        netplay_log_unlock();

        // Item 4: dump packet ring with a "sigterm" role tag so post-mortem
        // readers can distinguish the dump source from peer-disconnect /
        // desync dumps.
        SDLNetAdapter_DumpPacketRing(
            netplay_utc_ms(),
            (player_number == 0) ? "host-sigterm" : "joiner-sigterm",
            s_session_uuid);
    }

    // Item 7: flush + close the netplay log file.
    netplay_log_lock();   // #36
    netplay_log_close();
    netplay_log_unlock();

    // P-2.2 fix: clear the session-uuid sentinel so a second call to this
    // function is a no-op. Mirrors the EXITING branch's post-flush zeroing.
    s_session_uuid = 0;
}

#ifdef ENABLE_NETPLAY_TESTS
// Task #143 (queue.md #143): expose setup_vs_mode() itself so
// test_gs_coverage.c can probe whether the full equalizer is safe to call
// from the --test-* dispatch position (immediately after read_args(),
// before any SDL/engine bootstrap — see main.c). Not declared in
// netplay.h; test TU forward-declares it.
void Netplay_Test_RunSetupVsMode(void) {
    setup_vs_mode();
}
#endif
