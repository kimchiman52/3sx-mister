/*
 * direct_p2p.c — Step 7 of docs/plan-stun-direct-p2p.md.
 *
 * Direct-P2P orchestrator. See direct_p2p.h for the public contract and
 * the state-machine rationale.
 *
 * Upstream reference (pattern, not line-by-line):
 *   /tmp/3sxtra/src/port/sdl/netplay/sdl_netplay_ui.cpp:696-749 (thread fns),
 *   :1141-1209 (punch-done handler + socket ownership transfer),
 *   :1213-1240 (UPnP-done handler). Our ordering is inverted per user
 *   locked decision #2: UPnP first, STUN fallback.
 *
 * Key invariants:
 *   - All SDL3_net / miniupnpc blocking calls run on the worker thread.
 *   - State transitions published via SDL_AtomicInt; main thread only
 *     reads that atomic and inspects s_work (a struct populated by the
 *     worker before it publishes DIRECT_P2P_HOST_WAITING or a terminal
 *     state). The worker never touches s_work after it publishes a
 *     terminal state.
 *   - DirectP2P_Tick never blocks. HOST_WAITING receive polling is a
 *     single non-blocking NET_ReceiveDatagram probe per call.
 *   - The STUN socket lives in s_work.stun.socket until handoff; after
 *     Netplay_SetStunSocket(sock) we null s_work.stun.socket so cancel
 *     and session teardown don't double-close it.
 *   - The UPnP mapping lives in s_upnp_mapping and is released by
 *     direct_p2p_on_teardown() fired from the netplay.c session exit
 *     path (registered via Netplay_SetSessionTeardownCallback in
 *     DirectP2P_Init). Cancel before handoff releases directly.
 */

#include "netplay/direct_p2p.h"
#include "netplay/rendezvous.h"

/* This whole translation unit is netplay-only. The CMake glob excludes
 * direct_p2p.c from NETPLAY=OFF builds, and direct_p2p.h provides
 * header-local no-op inlines for that config. We still wrap the body
 * in #ifdef ENABLE_NETPLAY so a misconfigured include from a
 * non-netplay toolchain compiles cleanly to an empty TU rather than
 * bleeding symbol references. */
#ifdef ENABLE_NETPLAY

#include "netplay/connect_fail.h"
#include "netplay/natpmp.h"
#include "netplay/netplay.h"
/* Task #76: for NAV_ORCH_TIMEOUT_MARGIN_MS. The orchestrator cascade's
 * _Static_asserts live HERE — next to the legs, so editing a leg is what
 * breaks the build — but the ceiling they check is nav's, so nav's margin
 * has to be readable from here. netplay_nav.h pulls in nothing but
 * <stdbool.h>, so this is not a dependency cycle. */
#include "netplay/netplay_nav.h"
#include "netplay/room_code.h"
#include "netplay/stun.h"
#include "netplay/upnp.h"
#include "port/config/config.h"
#include "port/paths.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

/* Default UDP port used when neither the --direct-p2p-handoff file nor
 * CFG_KEY_NETPLAY_DIRECT_P2P_HOST_PORT specifies one. Chosen to sit
 * adjacent to RetroArch's netplay port (55435) in the IANA dynamic
 * range: shared "retro netplay" neighborhood for firewall/docs, no
 * collision with registered services, +3 offset as a Third-Strike
 * mnemonic. A concrete port (rather than 0 = OS-ephemeral) is required
 * for UPnP port-forwarding to work — router error 716
 * (WildCardNotPermittedInExtPort) means most routers reject wildcard
 * external-port requests. */
#define NETPLAY_DIRECT_P2P_DEFAULT_PORT 55438

/* --- module state ------------------------------------------------------ */

/* Role enum lives in direct_p2p.h so direct_p2p_overlay.c can branch on
 * DirectP2P_GetRole without depending on the file-local Work struct. */

typedef struct {
    Role role;

    /* Host-side preferred local port (0 => OS-assigned). */
    int preferred_port;

    /* Join-side decoded peer tuple. peer_local_port was removed when
     * the v1 room code shrank to 11 chars (see room_code.h; the code is
     * 12 chars as of v4 but has never carried local_port again). Hairpin
     * fallback now relies on the router's NAT-loopback behavior
     * rather than rewriting to 127.0.0.1:peer_local_port. */
    char peer_code[64];
    char peer_ip[64];
    uint16_t peer_public_port;

    /* Populated by worker on STUN success. The socket here is the one
     * that must eventually hand off to netplay.c via
     * Netplay_SetStunSocket. */
    StunResult stun;

    /* Host-side published room code — valid while state ==
     * DIRECT_P2P_HOST_WAITING. */
    char host_code[ROOM_CODE_BUF_LEN];

    /* Host-side: the public port actually encoded in host_code (UPnP
     * external port when the mapping succeeded, else the STUN-observed
     * port). The rendezvous session key MUST be derived from this
     * advertised tuple — the joiner derives its key from the decoded
     * room code, so a host deriving from the raw STUN port instead
     * would land in a different rendezvous slot whenever the UPnP
     * external port differs from the STUN-observed port (non-port-
     * preserving NAT). Set by host_thread_fn before HOST_WAITING is
     * published; only rewritten on the main thread by the STUN-rebind
     * drift handler (which joins the rendezvous thread first). */
    uint16_t advertised_port;

    /* S4a punch-auth token (Rendezvous_DerivePunchToken over the same
     * payload the room code encodes). HOST: derived by host_thread_fn
     * from its advertised tuple right after the room code is built
     * (re-derived by the drift handler when the code is re-encoded);
     * consumed by host_tick_receive's datagram gate and the bilateral
     * punch worker. JOINER: derived per join_attempt from the decoded
     * code into a worker-stack copy, and — task #119 — mirrored into
     * this field so do_handoff can hand it to netplay.c's late-punch
     * rescue layer (both roles keep answering authenticated punches
     * until the session starts; late_punch.h). token_valid gates the
     * host's punch gate fail-closed: with no valid token NO datagram is
     * ever accepted as the peer. */
    uint8_t punch_token[STUN_PUNCH_TOKEN_LEN];
    bool punch_token_valid;

    /* Nonce fed to the session-key and punch-token derivations on both
     * roles (Rendezvous_DeriveSessionKey / Rendezvous_DerivePunchToken).
     * Through v3 this was a per-session CSPRNG draw carried in the room
     * code. As of v4 (task #155) the room code no longer carries a
     * nonce, so this is always ROOM_CODE_V4_FIXED_NONCE (0) — set once
     * by host_thread_fn / BeginJoin and never touched again, INCLUDING
     * by a drift re-encode. See room_code.h's "nonce leaves the code"
     * section for the accepted-risk writeup this implies: both
     * derivations are now a pure function of the public (ip, port)
     * tuple. The field is kept as a real uint32_t (not inlined as a
     * literal at each derive call) so the two roles visibly agree on
     * one value and so a future room-list feature that DOES deliver a
     * real nonce out-of-band has one place to plug it into. */
    uint32_t nonce;

    /* Parsed rendezvous endpoint cache; populated in BeginHost/BeginJoin
     * once 5b/5c land. Holds the result of parsing the signal-url config
     * value (host:port). Both fields zero in 5a — no callers yet. */
    char     signal_host[64];
    uint16_t signal_port;

    /* --- S3 failure attribution (docs/plan-netplay-connection.md §5) ---
     * Written by the worker BEFORE it publishes a terminal state (same
     * discipline as every other s_work field); read by the main thread's
     * Tick reporting path after it observes that state. */
    ConnectFailCode fail_code;   /* taxonomy code for the surfaced failure */
    bool ev_deliver_any;         /* joiner: >=1 DELIVER frame (incl. sentinel) */
    bool ev_deliver_real;        /* joiner: >=1 DELIVER with a real endpoint */
    bool ev_challenge_any;       /* S4c joiner: >=1 CHALLENGE frame received */
    bool ev_cookie_rechallenged; /* #105 joiner: challenged AGAIN after we
                                    echoed a cookie — the echo never verified */
    int  join_attempts;          /* attempts consumed (S2 auto-retry) */
    /* Stage timings (ms) for the report line — 0 = stage not reached.
     *
     * S6 WARNING for anyone reading a report line: punch / signal /
     * bilateral are the LEG lifetimes and they now OVERLAP, so they no
     * longer sum to the elapsed time. `t_race_ms` is the wall clock of
     * the whole post-STUN cascade and is the number to compare across
     * builds. */
    uint32_t t_upnp_ms;
    uint32_t t_stun_ms;
    uint32_t t_punch_ms;
    uint32_t t_signal_ms;
    uint32_t t_bilateral_ms;
    uint32_t t_race_ms;

    /* #36 attribution evidence, copied out of the race / portmap probe.
     * Appended at the END on purpose: nothing reads this struct
     * positionally, so appending cannot shift anything.
     *
     * LIFETIME, and read this before adding a seventh field. s_work is
     * memset-zeroed once per attempt SET (DirectP2P_BeginHost /
     * BeginJoin / Cancel / Reset — the memsets near the bottom of this
     * file), NOT once per attempt. The S2 join retry runs join_attempt()
     * a second time on the SAME struct, so every per-attempt field must
     * ALSO be cleared by hand in join_attempt()'s reset block or the
     * retry's report inherits attempt 1's evidence. All six race fields
     * below are in that block; portmap_* are host-only and the host has
     * no in-set retry of this shape. */
    uint8_t  portmap_backend;      /* PortMapBackend; 0 = none */
    bool     portmap_active;
    bool     race_confirm_seen;
    uint32_t race_confirm_ms;
    uint16_t ev_deliver_n;
    uint16_t ev_challenge_n;
    uint16_t ev_badver_n;
    /* Task #122: the typed refusal the race ended on. Per-attempt like
     * every other ev_* field — cleared in join_reset_attempt_evidence()
     * — so the S2 retry can never report attempt 1's refusal. */
    bool     ev_nack_any;
    uint8_t  ev_nack_reason;
    uint16_t ev_nack_n;
    /* Largest gap between two DELIVERs inside ONE race. The joiner
     * re-REGISTERs every 500 ms while the signal leg is live (section 5
     * of p2p_race) and the server pushes one DELIVER per REGISTER, so
     * ~500 ms is the expected baseline and a much larger value is
     * DELIVER loss on the path. Do NOT compare this against the 5000 ms
     * CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS default: that is
     * the HOST-ADVERTISING loop's cadence (host_rendezvous_thread_fn),
     * a different loop that this counter never observes. */
    uint32_t ev_deliver_gap_max_ms;

    /* --- #104 HOST LADDER RUNGS ------------------------------------
     *
     * WHY THESE EXIST. The S2 host ladder runs host_thread_fn up to
     * 1 + HOST_STUN_MAX_RETRIES times per hosting session, and every
     * rung OVERWRITES t_upnp_ms / t_stun_ms above. #36's report line
     * therefore showed only the LAST rung, and its `attempts=` field is
     * s_work.join_attempts — joiner-only, never incremented on the host
     * path. So a host that burned all four rungs (and, per
     * ORCH_HOST_WORST_CASE_MS, legitimately consumed the derived nav
     * deadline doing it) reported as ONE fast failure. That is the
     * misattribution class #36 exists to prevent — the H-1 shape, where
     * evidence of slow-but-real work is discarded and the user's network
     * gets the blame.
     *
     * These are CUMULATIVE across the ladder and sit alongside the
     * per-rung t_*_ms fields rather than replacing them: the last rung's
     * cost is still the interesting one when the ladder is one rung
     * long, and the totals are what distinguish four slow failures from
     * one fast one.
     *
     * BOUNDED, per #36's constraints: the two counts are structurally
     * capped at 1 + HOST_STUN_MAX_RETRIES (and saturate anyway), and the
     * three ms totals are bounded by ORCH_HOST_WORST_CASE_MS. Nothing
     * here is per-datagram and nothing here is a secret.
     *
     * LIFETIME: zeroed with the rest of s_work at BeginHost / Cancel /
     * Reset, i.e. once per attempt SET, which is exactly the scope of
     * one ladder. Deliberately NOT cleared per rung — accumulating is
     * the whole point.
     *
     * THREADING: host_rungs / host_portmap_probes / t_upnp_total_ms /
     * t_stun_total_ms are written by host_thread_fn (worker);
     * t_host_backoff_ms is written by DirectP2P_Tick (main). Those two
     * writers are strictly serialised by the ladder itself — the worker
     * publishes FAILED_STUN as its LAST act and has exited before Tick's
     * FAILED_STUN case observes that state and arms the backoff, and
     * Tick joins the dead worker before spawning the next rung. No two
     * of these fields are ever written concurrently, and they are read
     * only by report_connect_outcome on the main thread after a terminal
     * state. */
    uint8_t  host_rungs;           /* host_thread_fn entries this session */
    uint8_t  host_portmap_probes;  /* rungs that actually ran try_portmap */
    uint32_t t_upnp_total_ms;      /* sum of every rung's port-map leg */
    uint32_t t_stun_total_ms;      /* sum of every rung's STUN leg */
    uint32_t t_host_backoff_ms;    /* sum of the ladder backoffs waited */
} Work;

static SDL_AtomicInt s_state = { DIRECT_P2P_IDLE };
static SDL_AtomicInt s_cancel = { 0 };

/* s_work is touched by the worker while !terminal_state and only read
 * by the main thread once it observes a state transition that implies
 * the worker has finished producing the relevant fields (HOST_WAITING
 * implies stun / host_code are populated; HANDOFF / FAILED_* implies
 * the worker has exited the relevant phase). */
static Work s_work;

static SDL_Thread* s_thread = NULL;

/* Bilateral hole-punch fallback: separate cancel atomics per phase so
 * cancelling one phase doesn't accidentally signal another. Thread
 * handles are mutually exclusive on the user-action axis (host vs.
 * join) but coexist in time on the host path during the
 * FALLBACK_BILATERAL_PUNCH phase. Both stay NULL in 5a — no spawns
 * happen until 5b/5c. Cancel and teardown still join all three. */
static SDL_AtomicInt s_rendezvous_cancel = { 0 };
static SDL_AtomicInt s_bilateral_punch_cancel = { 0 };
static SDL_Thread* s_rendezvous_thread = NULL;
static SDL_Thread* s_bilateral_punch_thread = NULL;

/* Bilateral-punch worker -> Tick handoff signal. The worker thread cannot
 * call do_handoff (which invokes Netplay_SetParams / Netplay_SetRemotePort
 * / Netplay_SetStunSocket — main-thread-only). Instead, on punch SUCCESS
 * the worker writes peer_ip/peer_public_port back to s_work, sets this
 * flag, and exits. Tick's FALLBACK_BILATERAL_PUNCH case observes the flag
 * on the next frame, joins the worker, and runs do_handoff inline. */
static SDL_AtomicInt s_bilateral_handoff_pending = { 0 };

/* Review M1: host-side bilateral-punch FAILURE signal, mirror of the
 * handoff-pending flag above. The worker must not park the host in a
 * terminal state — a single stale or hostile REGISTER on our session
 * key (anyone who saw the room code) would then kill the room for the
 * entire hosting period. Instead the worker raises this flag and
 * exits; Tick joins it and returns the host to HOST_WAITING (respawning
 * the rendezvous loop) up to HOST_BILATERAL_MAX_FAILURES times per
 * hosting session, after which it parks FAILED_BILATERAL so a genuinely
 * unreachable pairing still surfaces. The count is main-thread only.
 * The JOINER's failure disposition is deliberately unchanged. */
static SDL_AtomicInt s_bilateral_failed = { 0 };
static int s_bilateral_fail_count = 0;
#define HOST_BILATERAL_MAX_FAILURES 5

/* Set by the worker right before it publishes a FAILED_* or
 * HOST_WAITING state so the status-text reader sees the fresh message
 * without a race. Writes from worker -> readers from main. Char buffer
 * is bounded; a torn read shows at worst a truncated but NUL-terminated
 * string because we always write the NUL first if we shorten. */
/* 256, not 128: the arm-time refusal text from Netplay_RefuseArm (prefix +
 * arcade_balance.c ps2_reason[192]) can run to ~150 glyphs, and the overlay
 * wraps rather than clips, so the buffer must not be the thing that cuts
 * the diagnostic. report_connect_outcome's line[] grew by the same amount. */
static char s_status[256] = { 0 };

/* UPnP mapping owned by the orchestrator until session teardown fires
 * direct_p2p_on_teardown(). "active == true" gates the RemoveMapping
 * call so re-entering Init doesn't double-release. */
static UpnpMapping s_upnp_mapping = { 0 };

/* #96: the OTHER half of the re-probe decision — the port whose probe
 * came back with NO mapping, or 0 for "no verdict yet this session".
 *
 * s_upnp_mapping above records a SUCCESS and lets host_thread_fn skip a
 * pointless re-probe on an S2 retry. That skip was one-sided, and the
 * side it optimised is the side that did not need it: a host that GOT a
 * mapping is not the host having a bad time. A host whose probe FAILS
 * re-paid PORTMAP_PROBE_BUDGET_MS on every one of the ladder's
 * 1 + HOST_STUN_MAX_RETRIES rungs — 45,000 of the 76,800 ms host bound
 * at shipped defaults, 59% of it, spent re-asking a question that was
 * answered the first time.
 *
 * And that budget is REACHED, not merely provisioned. Measured against
 * the shipping backends, on a host with a working default route and no
 * IGD:
 *   - the UPnP half: upnpDiscover(UPNP_DISCOVER_TIMEOUT_MS=2000) costs
 *     8033 ms with miniupnpc 2.2.1 (MINIUPNPC_API_VERSION 17 — the
 *     libminiupnpc.so.17 that the MINIUPNPC_LIBRARY_DIRS install block,
 *     CMakeLists.txt:752, ships to the MiSTer), and 8008 ms with 2.3.3.
 *     It is NOT one 2000 ms wait; miniupnpc walks a device-type list and
 *     waits the timeout per search target. Note this already exceeds
 *     UPNP_PROBE_BUDGET_MS, a nominal share not enforced anywhere.
 *   - the NAT-PMP half: a wholly silent gateway costs 3510 ms, exactly
 *     the two phases natpmp.h documents.
 * 8033 + 3510 = 11543 > PORTMAP_PROBE_BUDGET_MS (11250), so the outer
 * deadline fires and the probe costs its FULL budget every time.
 *
 * The 1-38 ms figure the netns rig reports for try_portmap does NOT
 * contradict this and must not be read as field evidence: a build with
 * NETPLAY_TEST_HOOKS *and* ENABLE_NETPLAY_TESTS refuses to reach the
 * network at all — upnp.c's guard above upnpDiscover and natpmp.c's
 * guard in discover_gateway both return false without sending anything.
 * The rig measures the refusal, not a probe.
 *
 * SCOPE, and it is deliberately NARROWER than the success latch.
 * s_upnp_mapping survives across BeginHost (the router-side lease
 * outlives the attempt); this does not. It is cleared by BeginHost, so
 * the automatic ladder trusts the verdict while any USER action — back
 * out, fix the router, press Host again — re-probes from scratch. The
 * ladder exists to ride out a transient STUN failure, and skipping a
 * re-probe costs at most "no port mapping this session", which is
 * already the non-fatal path host_thread_fn falls through to.
 *
 * Keyed by port for the same reason the success side is: a verdict is
 * about a specific (internal port) probe, not about the session. */
static uint16_t s_portmap_failed_port = 0;

/* #121: the JOINER's port-map probe — spawned, never waited on.
 *
 * WHY THE JOINER PROBES AT ALL. Until now try_portmap had exactly one
 * call site, host_thread_fn. The S7 machinery underneath it is already
 * role-agnostic (upnp_worker_fn, portmap_remove, the CGNAT gate, the S1
 * renewal all key off s_upnp_mapping, not off a role), so the joiner was
 * the only thing missing. A symmetric joiner that HOLDS an explicit
 * mapping stops being symmetric for that one port: the gateway forwards
 * the mapped external port to a fixed internal port regardless of who
 * sends, which is endpoint-independent by construction. That converts
 * the symmetric-joiner x port-dependent-host cell — the only cell the
 * netns matrix still fails — with no protocol change and no server cost.
 *
 * WHY IT IS FIRE-AND-FORGET, AND WHY THAT IS NOT LAZINESS. A FAILING
 * probe costs its full PORTMAP_PROBE_BUDGET_MS — 11,250 ms, measured,
 * see s_portmap_failed_port above for the two backend numbers. The
 * joiner's whole derived deadline is 31,800 ms at shipped defaults
 * against a 35,000 ms UX ceiling (ORCH_JOIN_WORST_CASE_MS and assert [A]
 * below), so a SERIAL joiner probe does not fit — not even a one-time
 * latched one: 200 + 11250 + 2*13300 + 5000 = 43,050 ms. Assert [E]
 * below pins exactly that arithmetic so this reasoning is checked by the
 * compiler rather than trusted from this comment.
 *
 * So the probe is never waited on. join_attempt SPAWNS it after its STUN
 * discover (which is what first reveals the socket's bound port) and
 * immediately proceeds into the race. The verdict is collected at the
 * top of the NEXT attempt with a ZERO-WAIT poll: complete means take the
 * mapping, still running means detach and forget. The serial cost added
 * to the joiner cascade is therefore 0 ms in every case, and
 * ORCH_JOIN_ATTEMPT_MS is unchanged.
 *
 * That trade loses nothing real. The window the probe gets for free is
 * attempt 1's race — RACE_HARD_CAP_MS, 9,200 ms at shipped defaults. A
 * gateway that HAS a backend answers inside it (NAT-PMP replies in one
 * LAN round trip; miniupnpc's SSDP discover is ~2 s), and a gateway that
 * does not is precisely the case whose 11,250 ms answer is worthless.
 * The expensive outcome and the useless outcome are the same outcome.
 *
 * SCOPE. One spawn per join session, gated on the STUN port_disagreement
 * signal (stun.h:26-31) so only a joiner that STUN has already caught
 * translating per-destination pays anything at all. The failure verdict
 * latches into #96's s_portmap_failed_port — the same variable, the same
 * session scope, the same reset sites — so the automatic retry does not
 * re-ask and a user action does. */
struct UpnpJob; /* defined with upnp_worker_fn below */
static SDL_Thread*     s_join_portmap_thread = NULL;
static struct UpnpJob* s_join_portmap_job = NULL;
static bool            s_join_portmap_spawned = false;
/* The internal port THIS join session probed for, or 0. Distinct from
 * s_join_portmap_spawned: "we started a probe" is not "this mapping is
 * ours". s_upnp_mapping deliberately outlives a HOSTING session, so
 * without this a joiner could pin and reuse a mapping left behind by an
 * earlier host attempt that no probe of this session ever asked for. */
static uint16_t        s_join_portmap_port = 0;

/* Set by the JOIN WORKER while it owns s_upnp_mapping; read by the MAIN
 * thread in upnp_renew_tick.
 *
 * DirectP2P_Tick gates the renewal on state, which is enough for the
 * HOST because the host transitions INTO a renewal-permitted state only
 * after it has finished writing the mapping. The joiner has the opposite
 * shape: it writes the mapping while transitioning OUT of a permitted
 * state, and a main thread that already sampled FALLBACK_SIGNALING for
 * this frame is not evicted by a later store. A bare state store cannot
 * express mutual exclusion; this flag can. */
static SDL_AtomicInt   s_join_portmap_busy;

/* #147: the ONE port-map worker that overran its budget and was left
 * running — parked here JOINABLE instead of SDL_DetachThread'ed.
 *
 * Why parking instead of detaching: a worker that overruns is still
 * executing inside miniupnpc's cached-IGD statics (upnp.c's
 * s_cached_urls / s_cached_data / s_cache_valid, with FreeUPNPUrls on
 * its error path) or natpmp.c's nonce/epoch statics. The REMOVAL and
 * RENEWAL sides were already gated on quiescence
 * (upnp_renew_join_and_discard, join_portmap_reset), but SPAWNING was
 * not: DirectP2P_Cancel -> DirectP2P_BeginHost clears
 * s_portmap_failed_port and runs a fresh upnp_worker_fn into the same
 * statics — two threads populating and freeing one cache, the
 * data-race / double-free class natpmp.c's L-3 comment records. A
 * detached thread's completion is unobservable (SDL_DetachThread), so
 * the only way a later session can KNOW the backends are quiescent is
 * to keep the handle. portmap_backends_quiescent below reaps it for
 * free once it exits; until then every upnp_worker_fn spawn site
 * refuses to start a second worker.
 *
 * AT MOST ONE straggler can exist, by construction: all three spawn
 * sites (try_portmap, join_portmap_spawn, upnp_renew_tick's renewal —
 * the last needs no explicit gate, see the note there) are closed while
 * this slot is occupied, so there is never a second running worker to
 * park. portmap_straggler_park asserts that loudly rather than assuming
 * it.
 *
 * Threading: same hand-off convention as s_join_portmap_thread above.
 * Worker-thread touches (try_portmap's park, join_portmap_spawn's gate,
 * join_portmap_collect's park) happen while that worker runs; every
 * main-thread touch (join_portmap_reset, upnp_renew_join_and_discard —
 * both called only from Cancel, teardown, BeginHost/BeginJoin and Init)
 * happens after the orchestrator worker was SDL_WaitThread'ed, so no
 * two threads ever access the slot concurrently. */
static SDL_Thread*     s_portmap_straggler_thread = NULL;
static struct UpnpJob* s_portmap_straggler_job = NULL;

/* R-1: latched by DirectP2P_NotifySessionRejected (game thread) when the
 * post-handoff MIST handshake rejects the peer. Consumed by
 * direct_p2p_on_teardown, which then parks in FAILED_HANDSHAKE instead
 * of IDLE so the overlay keeps showing the reject reason. Main-thread
 * only (notify, teardown, and Cancel all run on the game thread). */
static bool s_handshake_reject_latched = false;

/* S1 host liveness: STUN rebind keepalive bookkeeping. Main-thread only
 * (written/read exclusively from Tick's HOST_WAITING branch and the
 * BeginHost/BeginJoin/Cancel/teardown resets, all on the game thread).
 * last_ms == 0
 * means "not armed yet" — the first keepalive fires one interval after
 * HOST_WAITING is first ticked. txid_valid gates response parsing so a
 * stale or spoofed binding response without a matching in-flight
 * transaction is ignored. */
static uint64_t s_stun_keepalive_last_ms = 0;
static uint8_t s_rebind_txid[12] = { 0 };
static bool s_rebind_txid_valid = false;

/* Review M2: drift debounce. A single differing STUN Binding Response
 * must not rewrite the on-screen room code — the user may be reading it
 * aloud. A drift candidate is only COMMITTED when a second consecutive
 * keepalive confirms the same new endpoint; a response matching the
 * current endpoint clears the candidate. Deliberate side effect: a NAT
 * that rebinds on every keepalive (UDP idle timeout shorter than the
 * keepalive interval) produces a DIFFERENT port each time, so the
 * candidate keeps being replaced and never commits — the code stays
 * stable instead of churning every interval with "Network changed!".
 * Main-thread only, same lifecycle as the txid state above. */
static char s_drift_pending_ip[64] = { 0 };
static uint16_t s_drift_pending_port = 0;
static bool s_drift_pending_valid = false;

/* S2 host auto-retry (docs/plan-netplay-connection.md §4): a host
 * parking terminal in FAILED_STUN because one discovery attempt raced
 * a DHCP renew / DNS hiccup is pure loss — the user just stares at
 * ERROR. Tick's FAILED_STUN case re-spawns host_thread_fn after a 5 s
 * backoff, up to HOST_STUN_MAX_RETRIES per hosting session (BeginHost
 * resets the count). Main-thread only, like the other Tick-side
 * bookkeeping. The JOINER's FAILED_STUN is handled inside its own
 * worker (join_thread_fn single fresh-socket retry) and stays terminal
 * here. */
static int s_host_stun_retry_count = 0;
static uint64_t s_host_stun_retry_at_ms = 0;
#define HOST_STUN_MAX_RETRIES 3
#define HOST_STUN_RETRY_BACKOFF_MS 5000

/* #104: the ladder LENGTH and BACKOFF the retry path actually uses.
 * Identical to HOST_STUN_MAX_RETRIES / HOST_STUN_RETRY_BACKOFF_MS in
 * every shipped build — the overrides are NETPLAY_TEST_HOOKS-only and
 * exist so the #104 harness can drive the REAL state machine (real
 * worker spawns, real STUN failures, real FAILED_STUN transitions, real
 * re-probes) at a chosen ladder length, and without spending
 * 3 x 5000 ms of suite wall-clock asleep. Only the length and the sleep
 * are dialled; every rung that runs is genuine.
 *
 * DELIBERATELY NOT used by ORCH_HOST_WORST_CASE_MS, which must keep
 * summing the shipped constants: the nav deadline is a property of the
 * shipped build, not of whatever a harness dialled in. */
#ifdef NETPLAY_TEST_HOOKS
static int s_host_retry_backoff_override_ms = 0;
static int s_host_max_retries_override = -1;
static int host_stun_retry_backoff_ms(void) {
    return s_host_retry_backoff_override_ms > 0 ? s_host_retry_backoff_override_ms
                                                : HOST_STUN_RETRY_BACKOFF_MS;
}
static int host_stun_max_retries(void) {
    return s_host_max_retries_override >= 0 ? s_host_max_retries_override
                                            : HOST_STUN_MAX_RETRIES;
}
#else
#define host_stun_retry_backoff_ms() (HOST_STUN_RETRY_BACKOFF_MS)
#define host_stun_max_retries() (HOST_STUN_MAX_RETRIES)
#endif

/* Pre-NET_Init settle delay both workers take before touching SDL_net
 * (see host_thread_fn for the segfault rationale). Named for task #76 so
 * the orchestrator worst case can sum it. */
#define WORKER_STARTUP_DELAY_MS 200

/* S2 joiner auto-retry: join_thread_fn runs join_attempt() up to this
 * many times, each with a freshly bound local socket (see the loop in
 * join_thread_fn for the rationale). Named for task #76 because it is
 * the MULTIPLIER on the joiner's whole per-attempt cascade — an
 * anonymous second call could be turned into a third without the
 * derived nav deadline noticing. */
#define JOIN_MAX_ATTEMPTS 2

/* Bounded blocking DNS poll (resolve_with_short_poll) that a signalling
 * leg runs before it can arm. Named for task #76 because the cascade sum
 * has to account for it: an anonymous `< 100` in the loop body cannot be
 * summed into a derived deadline without the deadline silently going
 * stale the moment the loop changes.
 *
 * NOT inside #ifdef NETPLAY_TEST_HOOKS: resolve_with_short_poll and the
 * worst-case derivation are both unconditional, so a production build
 * (ENABLE_NETPLAY on, NETPLAY_TEST_HOOKS off) needs these too. */
#define RESOLVE_POLL_ATTEMPTS 100
#define RESOLVE_POLL_STEP_MS  1
#define RESOLVE_POLL_MAX_MS   (RESOLVE_POLL_ATTEMPTS * RESOLVE_POLL_STEP_MS)

/* Init guard so DirectP2P_Init is idempotent. */
static bool s_init_done = false;

/* --- internal helpers -------------------------------------------------- */

/* Task #157: machine-readable hosting status for the desktop launcher.
 *   <pref>/netplay.status
 *     line 1: HOSTING | IDLE
 *     line 2: room code, or empty when not hosting
 * Same pref-path location, line-oriented shape and SDL_IOStream idiom as
 * arcade_balance.c's write_status_file(). The code here is deliberately
 * UNREDACTED — unlike the log lines (RoomCode_Redact), this file exists
 * precisely so a local launcher can read the live code instead of
 * screen-scraping, and it never leaves the user's own pref dir.
 *
 * Write/clear sites: set_state() below rewrites it on every transition
 * into or out of DIRECT_P2P_HOST_WAITING, host_commit_endpoint()
 * refreshes it when a drift re-encode replaces the code WITHOUT a state
 * transition, and DirectP2P_Init() clears it at boot so a code left by
 * a crashed run cannot be read back by a later launcher run. */
static void netplay_status_publish(bool hosting, const char* code) {
    char* path = NULL;
    SDL_asprintf(&path, "%snetplay.status", Paths_GetPrefPath());

    if (path == NULL) {
        return;
    }

    SDL_IOStream* io = SDL_IOFromFile(path, "w");

    if (io != NULL) {
        SDL_IOprintf(io, "%s\n%s\n", hosting ? "HOSTING" : "IDLE",
                     (hosting && code != NULL) ? code : "");
        SDL_CloseIO(io);
    }

    SDL_free(path);
}

static void set_state(DirectP2PState s) {
    DirectP2PState prev = (DirectP2PState)SDL_SetAtomicInt(&s_state, (int)s);

    /* Task #157: netplay.status tracks HOST_WAITING exactly. Every
     * state transition goes through this function — the only other
     * writer of s_state is DirectP2P_Init's once-per-process IDLE reset,
     * which runs before any hosting session can exist — so hooking the
     * transition here is what proves no path out of HOST_WAITING
     * (handoff, Cancel, teardown, bilateral failure, drift, quit-to-
     * FAILED_*) can leave a live code behind. Both HOST_WAITING entry
     * sites (host_thread_fn's publish and the M1 bilateral-failure
     * return in DirectP2P_Tick) have s_work.host_code fully written by
     * the calling thread before they publish, so the read below is
     * race-free. */
    if (s == DIRECT_P2P_HOST_WAITING) {
        netplay_status_publish(true, s_work.host_code);
    } else if (prev == DIRECT_P2P_HOST_WAITING) {
        netplay_status_publish(false, NULL);
    }
}

static DirectP2PState get_state(void) {
    return (DirectP2PState)SDL_GetAtomicInt(&s_state);
}

static void set_status(const char* msg) {
    if (msg == NULL) {
        s_status[0] = '\0';
        return;
    }
    /* Written from worker threads while the main thread reads s_status IN
     * PLACE — DirectP2P_GetStatusText returns the buffer itself (the
     * overlay renders straight out of it), it does not copy. #150: an
     * earlier version of this comment claimed the reader "copies out of
     * the buffer eagerly", which the API has never done — do not design
     * against that. What this write pattern actually guarantees is only:
     *   (a) the buffer is never un-NUL-terminated (len is clamped below
     *       sizeof and the memset clears the tail first), so a racing
     *       reader cannot run off the end;
     *   (b) a reader that races the write may see an empty or torn
     *       status for the frame the write straddles. Accepted: this is
     *       a human-readable line that settles by the next frame.
     * A worker must never put anything here whose momentary tearing
     * matters. */
    size_t len = strlen(msg);
    if (len >= sizeof(s_status)) len = sizeof(s_status) - 1;
    memset(s_status, 0, sizeof(s_status));
    memcpy(s_status, msg, len);
}

static bool cancel_requested(void) {
    return SDL_GetAtomicInt(&s_cancel) != 0;
}

/* --- S3: failure attribution helpers ----------------------------------- */

/* One attributed report line per Begin* attempt-set (reset alongside the
 * other per-session bookkeeping). Main-thread only — written from Tick's
 * reporting path and the BeginHost / BeginJoin / Cancel resets. */
static bool s_outcome_reported = false;

/* S3: has the host received ANY rendezvous DELIVER (incl. the
 * zero-sentinel) this hosting session? The server answers every REGISTER
 * with a DELIVER, so this doubles as "the rendezvous path is alive" for
 * the cause-8 host advisory. Main-thread only (try_handle_deliver runs
 * from Tick). */
static bool s_host_deliver_seen = false;

/* S3: set by host_rendezvous_thread_fn when it actually ENTERS its
 * REGISTER resend loop. The cause-8 advisory reads "zero DELIVERs" as
 * evidence of a dead rendezvous path — which is only evidence when
 * REGISTERs are in fact being sent. With the bilateral kill switch on,
 * a missing/malformed signal URL, a failed resolve, or a failed thread
 * spawn, no REGISTER ever leaves the box and silence is EXPECTED, so
 * the advisory must stay quiet (those paths already log their own
 * cause). Worker-write / main-read. */
static SDL_AtomicInt s_host_registering = { 0 };

/* S3 Part A(3): HOST_WAITING is legitimately unbounded by design — a
 * host advertises until a joiner arrives or the user cancels. The S3
 * requirement is that it be INFORMATIVE, not silent: elapsed time on the
 * status line (so the user can see the room is alive), a minute-cadence
 * log line, and the cause-8 advisory when the evidence says the room is
 * likely unjoinable. Main-thread only (Tick's HOST_WAITING branch).
 * NOT reset on the M1 bilateral-failure return to HOST_WAITING — that is
 * the same hosting session. */
static uint64_t s_host_waiting_since_ms = 0;
#ifdef NETPLAY_TEST_HOOKS
/* Multiplier applied to the host-waiting elapsed clock (test seam; 1 =
 * off). See host_waiting_tick. */
static int s_host_advisory_scale = 1;
#endif
static uint64_t s_host_waiting_last_note_ms = 0;
static ConnectFailCode s_host_advisory_code = CONNECT_FAIL_NONE;

/* S4a: count of unauthenticated datagrams the host's gate has ignored
 * this hosting session (rate-limited advisory logging in
 * host_tick_receive). Main-thread only. */
static int s_host_unauth_drops = 0;

/* --- S4-review HIGH-1b: host punch-gate throttle -----------------------
 *
 * s_host_unauth_drops above is a LOG COUNTER ONLY. Before this block
 * there was no cap, no per-source throttle and no backoff on the host's
 * punch gate: the host answered every guess (a correct token is
 * accepted and handed off, a wrong one is silently dropped and the host
 * keeps waiting — deliberately with NO wall-clock budget). host_tick_
 * receive drains up to HOST_TICK_RECV_BATCH datagrams per frame
 * (~960/second since #150; ~60/second when this block was written).
 *
 * PER SOURCE IP — after HOST_PUNCH_SRC_MAX_BAD bad punches from one IP
 * that source is MUTED: its punches are dropped WITHOUT the accept/
 * echo/handoff. Sizing: a joiner's Stun_HolePunch emits ~10 datagrams in
 * its first 500 ms then 5/s, so 24 covers the opening burst plus ~2.8 s
 * of steady state before muting — a peer that merely raced a drift
 * re-encode gets plenty of room. The mute EXPIRES after
 * HOST_PUNCH_MUTE_MS rather than lasting the session, so a friend who
 * flooded 24 bad punches on a typo'd code and then re-typed it correctly
 * is not locked out forever with no diagnosis.
 *
 * Accounting rule — ONLY punch-SHAPED datagrams that FAIL the token
 * check are charged. A legitimate joiner holding the right code is
 * accepted on its first punch and is never charged at all.
 *
 * v4 (task #155) — what this throttle defends against CHANGED, and the
 * old "brute-force oracle" framing above is now only half true. Through
 * v3, guessing the token meant guessing a CSPRNG nonce nobody but a code
 * holder had, and this throttle (plus the nonce width) was what made
 * that grind infeasible. As of v4 the token is `f(ip, port)` with a
 * FIXED nonce (room_code.h's accepted risk): anyone who knows or scans
 * the host's public endpoint computes the CORRECT token directly, no
 * guessing required, and their very first punch is accepted like any
 * real joiner's — this throttle does nothing for that attacker, because
 * they are never charged a single bad punch. What it still defends
 * against: a source that keeps sending WRONG-TOKEN traffic — punch-
 * SHAPED datagrams that fail the token check (see the accounting rule
 * above and host_ignore_is_chargeable) — from a stale/mismatched build,
 * a corrupted client, or unsophisticated flooding that never computed
 * the real token. Non-punch-shaped garbage (port scans, stray traffic)
 * is deliberately NOT charged either way — it teaches an attacker
 * nothing and charging it would let unrelated noise trip the gate. A
 * real but narrower threat than the one this block was written for.
 *
 * The SECOND layer this block originally described — a per-session
 * "re-roll" that drew a fresh nonce once HOST_PUNCH_TOTAL_REROLL bad
 * punches accumulated, invalidating whatever an attacker had been
 * grinding for — is REMOVED as of v4. It has no v4 analog: with a fixed
 * nonce the token cannot be rotated without changing the advertised
 * (ip, port) itself, which nothing here does on demand, so "re-roll"
 * would relabel the SAME code as new and accomplish nothing. The
 * per-source mute above is the sole backstop from here — see
 * room_code.h's accepted-risk section for what this implies.
 *
 * HOST_PUNCH_TOTAL_REROLL / HOST_PUNCH_REROLL_MAX and the functions that
 * used them (host_reroll_room_code, host_punch_gate_clear_mutes) stay
 * DORMANT-BUT-COMPILED below rather than deleted outright — see the
 * tombstone comment on host_reroll_room_code for why unwiring them (not
 * removing them) is the correct call under v4, and direct_p2p.h for why
 * they are declared non-static instead of parked behind #if.
 *
 * All state here is MAIN-THREAD ONLY (host_tick_receive and the Tick
 * reset paths). No locking.
 */
#define HOST_PUNCH_SRC_TABLE      8      /* tracked source IPs           */
#define HOST_PUNCH_SRC_MAX_BAD    24     /* bad punches before mute      */
#define HOST_PUNCH_MUTE_MS        60000u /* mute lifetime                */
/* DORMANT as of v4 — read only by the unwired host_reroll_room_code
 * below (no production caller reaches it; see its tombstone comment). */
#define HOST_PUNCH_TOTAL_REROLL   64     /* session bad punches -> re-roll */
#define HOST_PUNCH_REROLL_MAX     3      /* re-rolls per hosting session */

typedef struct {
    char     ip[64];       /* "" = free slot                            */
    int      bad;          /* bad punch-shaped datagrams charged        */
    uint32_t mute_until;   /* SDL_GetTicks() deadline; 0 = not muted    */
} HostPunchSrc;

static HostPunchSrc s_host_punch_src[HOST_PUNCH_SRC_TABLE];
static int  s_host_punch_bad_total = 0;   /* this hosting session        */
/* DORMANT as of v4 — only host_reroll_room_code increments this, and
 * nothing calls that function (see its tombstone comment). Kept so the
 * function's own accounting still compiles and reads sensibly. */
static int  s_host_punch_rerolls   = 0;   /* code re-rolls performed     */
static int  s_host_punch_muted_drops = 0; /* accepts suppressed by mute  */

static void host_punch_gate_reset(void) {
    memset(s_host_punch_src, 0, sizeof(s_host_punch_src));
    s_host_punch_bad_total = 0;
    s_host_punch_rerolls = 0;
    s_host_punch_muted_drops = 0;
}

/*
 * v4 (task #155) — DORMANT-BUT-COMPILED, NOT WIRED. Clears every mute and
 * per-source tally, keeping the session totals. Through v3 this ran as
 * the tail of a punch-gate "re-roll": the code the muted sources were
 * failing against no longer existed, so holding their strikes against
 * them would have been exactly the lockout this design avoids.
 *
 * Under v4 that reasoning does NOT carry over, and this function must
 * stay UNCALLED rather than being re-wired as-is. host_reroll_room_code
 * below can only ever re-encode the SAME (ip, port) into a
 * byte-identical code (the nonce is fixed — see room_code.h), so a
 * hypothetical caller that invoked this once every HOST_PUNCH_TOTAL_
 * REROLL charged bad punches would clear every source's mute against an
 * UNCHANGED code — an attacker grinding wrong tokens would launder their
 * own mute every 64 datagrams for free. That is a mute bypass, not a
 * defense, which is why the old call site in host_tick_receive_one is
 * gone.
 *
 * Declared non-static (see direct_p2p.h) rather than parked behind #if —
 * #if is what let LOSSY_ADAPTER silently stop compiling once the warning
 * set tightened (4ca4c0de) — and kept for the day a room-list feature
 * restores an out-of-band nonce (room_code.h's "nonce leaves the code"
 * section): host_reroll_room_code and this function become correct again
 * together, and are re-wired together, never one without the other.
 */
void host_punch_gate_clear_mutes(void) {
    memset(s_host_punch_src, 0, sizeof(s_host_punch_src));
}

/* Find `ip`'s slot, or claim one. Eviction picks the entry with the
 * FEWEST strikes that is not currently muted — a muted attacker cannot
 * push itself out of the table by rotating source addresses, while a
 * one-off stray packet ages out immediately. Returns NULL only when
 * every slot is muted (in which case the caller just charges the
 * session total, which is the level that actually matters). */
static HostPunchSrc* host_punch_src_slot(const char* ip, uint32_t now_ms) {
    HostPunchSrc* victim = NULL;
    for (int i = 0; i < HOST_PUNCH_SRC_TABLE; i++) {
        HostPunchSrc* e = &s_host_punch_src[i];
        if (e->ip[0] != '\0' && strcmp(e->ip, ip) == 0) {
            /* Expire a stale mute in place. */
            if (e->mute_until != 0 &&
                (int32_t)(now_ms - e->mute_until) >= 0) {
                e->mute_until = 0;
                e->bad = 0;
            }
            return e;
        }
        if (e->ip[0] == '\0') {
            victim = e;
            break;
        }
        const bool muted = e->mute_until != 0 &&
                           (int32_t)(now_ms - e->mute_until) < 0;
        if (muted) continue;
        if (victim == NULL || e->bad < victim->bad) victim = e;
    }
    if (victim == NULL) return NULL;
    SDL_strlcpy(victim->ip, ip, sizeof(victim->ip));
    victim->bad = 0;
    victim->mute_until = 0;
    return victim;
}

/* Is `ip` currently muted? Cheap read-only probe used on the ACCEPT
 * path — the token compare still runs, we simply refuse to answer, so
 * the source learns nothing from a correct guess. */
static bool host_punch_gate_is_muted(const char* ip, uint32_t now_ms) {
    for (int i = 0; i < HOST_PUNCH_SRC_TABLE; i++) {
        const HostPunchSrc* e = &s_host_punch_src[i];
        if (e->ip[0] == '\0' || strcmp(e->ip, ip) != 0) continue;
        return e->mute_until != 0 && (int32_t)(now_ms - e->mute_until) < 0;
    }
    return false;
}

/* Charge one bad punch-shaped datagram to `ip` (this function stays
 * free of s_work and thread lifecycle concerns so it can be
 * unit-tested). v4 (task #155): no return value — the session-total
 * "re-roll owed" signal this used to report is gone along with the
 * re-roll escalation itself; see the punch-gate throttle comment
 * above. */
static void host_punch_gate_note_bad(const char* ip, uint32_t now_ms) {
    s_host_punch_bad_total++;

    HostPunchSrc* e = host_punch_src_slot(ip, now_ms);
    if (e != NULL && e->mute_until == 0) {
        e->bad++;
        if (e->bad >= HOST_PUNCH_SRC_MAX_BAD) {
            e->mute_until = now_ms + HOST_PUNCH_MUTE_MS;
            if (e->mute_until == 0) e->mute_until = 1; /* 0 means "not muted" */
            SDL_Log("[direct_p2p] %s punch-gate MUTE %s for %u ms after %d "
                    "bad-token punches — that source can no longer probe the "
                    "gate (session total %d)",
                    ConnectFail_Code(CONNECT_FAIL_PUNCH_AUTH), ip,
                    (unsigned)HOST_PUNCH_MUTE_MS, e->bad,
                    s_host_punch_bad_total);
        }
    }
}

/* --- S4c: rendezvous return-routability cookie (host side) ------------- */

/* The v2 rendezvous server answers an uncookied REGISTER with a
 * CHALLENGE and only binds a slot once the client echoes the cookie
 * back (proving it receives at its claimed source). On the HOST the
 * writer and reader live on different threads: the CHALLENGE arrives
 * on the MAIN thread (host_tick_receive -> host_handle_challenge)
 * while the periodic REGISTER resends are built on the rendezvous
 * WORKER thread. A seqlock publishes the 8-byte cookie across:
 * seq == 0 means "no cookie yet"; the writer bumps to odd, writes the
 * bytes, bumps to even; the reader snapshots seq, copies, re-checks.
 * The JOINER needs none of this — its signaling loop is inline in the
 * join worker, the socket's sole actor. */
static uint8_t s_signal_cookie[REND_COOKIE_LEN];
static SDL_AtomicInt s_signal_cookie_seq = { 0 };

/* Main-thread writer (host_handle_challenge). */
static void signal_cookie_publish(const uint8_t cookie[REND_COOKIE_LEN]) {
    int seq = SDL_GetAtomicInt(&s_signal_cookie_seq);
    if (seq & 1) seq++; /* defensive: never leave an odd value behind */
    SDL_SetAtomicInt(&s_signal_cookie_seq, seq + 1);       /* odd: writing */
    memcpy(s_signal_cookie, cookie, REND_COOKIE_LEN);
    SDL_SetAtomicInt(&s_signal_cookie_seq, seq + 2);       /* even: stable */
}

static void signal_cookie_reset(void) {
    SDL_SetAtomicInt(&s_signal_cookie_seq, 0);
    memset(s_signal_cookie, 0, sizeof(s_signal_cookie));
}

/* Worker-thread reader. Returns true and fills `out` when a stable
 * cookie is available; false -> caller sends the uncookied form. */
static bool signal_cookie_snapshot(uint8_t out[REND_COOKIE_LEN]) {
    for (int attempt = 0; attempt < 4; attempt++) {
        const int s1 = SDL_GetAtomicInt(&s_signal_cookie_seq);
        if (s1 == 0 || (s1 & 1)) return false; /* none, or mid-write */
        memcpy(out, s_signal_cookie, REND_COOKIE_LEN);
        if (SDL_GetAtomicInt(&s_signal_cookie_seq) == s1) return true;
    }
    return false; /* writer kept racing us — next tick will get it */
}

/* S4c: has the host seen a CHALLENGE this hosting session? "Server is
 * alive" evidence for the host-waiting advisory (a challenge with no
 * subsequent DELIVER points at cookie/auth trouble, not a dead
 * server). Main-thread only. */
static bool s_host_challenge_seen = false;

/* Record the taxonomy code and put its user string on the overlay status
 * line. Callable from worker threads (same rules as set_status — s_work
 * writes happen-before the terminal state publish). */
static void set_fail(ConnectFailCode code) {
    s_work.fail_code = code;
    set_status(ConnectFail_UserText(code));
}

/* Like set_fail but with a custom status string (cases where the generic
 * user text would mislead, e.g. kill-switch bypass). */
static void set_fail_msg(ConnectFailCode code, const char* msg) {
    s_work.fail_code = code;
    set_status(msg);
}

/* Returns true when ip is in a private/loopback/link-local IPv4 range.
 * Used by the bilateral-punch fallback (5b/5c) to short-circuit
 * rendezvous when the peer turns out to be on the same LAN — direct
 * STUN-discovered punch is the right path there, not the public
 * rendezvous server. */
static bool direct_p2p_is_lan_peer(const char* ip) {
    if (!ip || !*ip) return false;
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) return false;
    uint32_t a = ntohl(addr.s_addr);
    /* 127.0.0.0/8 */
    if ((a & 0xFF000000u) == 0x7F000000u) return true;
    /* 10.0.0.0/8 */
    if ((a & 0xFF000000u) == 0x0A000000u) return true;
    /* 172.16.0.0/12 */
    if ((a & 0xFFF00000u) == 0xAC100000u) return true;
    /* 192.168.0.0/16 */
    if ((a & 0xFFFF0000u) == 0xC0A80000u) return true;
    /* 169.254.0.0/16 (link-local) */
    if ((a & 0xFFFF0000u) == 0xA9FE0000u) return true;
    return false;
}

/* Classify a router-reported external IP for the CGNAT gate (review
 * M3). Returns true — "the mapping is provably useless" — only when
 * the string PARSES as IPv4 and is a non-public address: RFC1918,
 * RFC 6598 CGN shared space (100.64.0.0/10), loopback, or link-local.
 * A public-but-different IP (ISP 1:1 NAT / DMZ, where the router's WAN
 * IP differs from the STUN IP but the mapping forwards perfectly) and
 * an unparseable string (garbage, IPv6 form) must NOT poison a working
 * mapping — callers keep it and log instead. */
static bool direct_p2p_ip_is_nonpublic(const char* ip) {
    /* S7 review M-5.4: the gate used to fail OPEN on an EMPTY string.
     *
     * An empty external_ip does not mean "an address we could not
     * classify", it means "no address was ever learned" — natpmp.c's
     * fill_mapping leaves the buffer empty when the gateway reported
     * 0.0.0.0, and upnp.c leaves it empty when GetExternalIPAddress
     * failed. The gate's whole job is to compare the router's idea of
     * our external address against STUN's; with nothing to compare it
     * cannot clear the mapping, and "cannot clear" must not read as
     * "cleared". Falling through to STUN costs one port mapping;
     * advertising a port on an address we never learned costs the
     * connection.
     *
     * Note this is checked BEFORE direct_p2p_is_lan_peer, which parses
     * and would classify "" as simply unparseable. */
    if (ip == NULL || ip[0] == '\0') {
        return true; /* fail CLOSED: unknown external address */
    }
    if (direct_p2p_is_lan_peer(ip)) {
        return true; /* 127/8, 10/8, 172.16/12, 192.168/16, 169.254/16 */
    }
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        return false; /* unparseable — cannot prove anything, keep mapping */
    }
    uint32_t a = ntohl(addr.s_addr);
    /* 100.64.0.0/10 — RFC 6598 carrier-grade NAT shared address space */
    if ((a & 0xFFC00000u) == 0x64400000u) return true;
    return false;
}

#ifdef NETPLAY_TEST_HOOKS
/* S7 review M-5.4. Thin passthrough to the very predicate the CGNAT gate
 * calls — not a re-derivation of it — so a test can pin the fail-closed
 * behaviour on an empty external IP without standing up a whole host
 * state machine for a case (UPnP's GetExternalIPAddress failing) that
 * has no mockable seam. */
bool DirectP2P_TestHook_IpIsNonPublic(const char* ip) {
    return direct_p2p_ip_is_nonpublic(ip);
}
#endif

/* Equality check that normalizes both inputs through inet_pton so two
 * dotted-quad strings that differ only in formatting (leading zeros,
 * embedded whitespace handling, IPv4-mapped-v6 like "::ffff:1.2.3.4")
 * still compare equal. Returns false if either input fails to parse as
 * IPv4. Used by the hairpin gate in 5b/5c. */
static bool direct_p2p_ip_eq_normalized(const char* a, const char* b) {
    if (!a || !b) return false;
    struct in_addr aa, bb;
    if (inet_pton(AF_INET, a, &aa) != 1) return false;
    if (inet_pton(AF_INET, b, &bb) != 1) return false;
    return aa.s_addr == bb.s_addr;
}

/*
 * Production send wrapper. Lives in direct_p2p.c (not rendezvous.c) so
 * rendezvous.c stays SDL_net-pure. Step 6 wraps this in a function-pointer
 * seam so the bilateral-punch unit tests can interpose without touching
 * the network. See NETPLAY_TEST_HOOKS block below.
 */
static bool Rendezvous_Send(NET_DatagramSocket* sock, NET_Address* target,
                            uint16_t target_port, const uint8_t* pkt,
                            size_t pkt_len) {
    if (!sock || !target || !pkt || pkt_len == 0 || pkt_len > INT_MAX) return false;
    return NET_SendDatagram(sock, target, target_port, pkt, (int)pkt_len);
}

/*
 * NETPLAY_TEST_HOOKS — function-pointer seam (Step 6 of
 * docs/plan-bilateral-hole-punch.md).
 *
 * Production builds (without -DNETPLAY_TEST_HOOKS) keep the call sites as
 * direct calls, so the resulting object code is byte-identical to
 * pre-Step-6. Test builds (-DNETPLAY_TEST_HOOKS) replace the direct calls
 * with indirect calls through s_stun_hole_punch_impl /
 * s_rendezvous_send_impl, which test_bilateral_punch.c can override via
 * the DirectP2P_TestHook_Set* setters.
 *
 * Signatures must match the productions exactly:
 *   bool Stun_HolePunch(StunResult*, char*, uint16_t*, const uint8_t[8],
 *                       int, SDL_AtomicInt*);
 *   bool Rendezvous_Send(NET_DatagramSocket*, NET_Address*, uint16_t,
 *                        const uint8_t*, size_t);
 */
#ifdef NETPLAY_TEST_HOOKS
/* Public typedefs are declared in direct_p2p.h. The local function
 * pointers default to the production functions; test_bilateral_punch.c
 * overrides them via DirectP2P_TestHook_Set*. */
static DirectP2P_RendezvousSend_fn s_rendezvous_send_impl  = Rendezvous_Send;
/* S2: Stun_Discover seam so the joiner auto-retry test can drive
 * BeginJoin end-to-end without touching real STUN servers. */
static DirectP2P_StunDiscover_fn   s_stun_discover_impl    = Stun_Discover;
/* S3-review HIGH-1: signaling-budget override so the self-DELIVER
 * regression test doesn't spend 2 x 8 s (both attempts' full default
 * budget) waiting out a loop whose early-exit the fix deliberately
 * removes. 0 = use the config value. */
static int s_test_signal_budget_ms = 0;
/* S6: override for the OVERALL race budget (0 = use the config value). */
static int s_test_race_budget_ms = 0;

#define RENDEZVOUS_SEND(sock, target, target_port, pkt, pkt_len) \
    s_rendezvous_send_impl((sock), (target), (target_port), (pkt), (pkt_len))
#define STUN_DISCOVER(result, local_port, timeout_ms) \
    s_stun_discover_impl((result), (local_port), (timeout_ms))

void DirectP2P_TestHook_SetRaceBudgetMs(int ms) {
    s_test_race_budget_ms = (ms > 0) ? ms : 0;
}
void DirectP2P_TestHook_SetRendezvousSend(DirectP2P_RendezvousSend_fn fn) {
    s_rendezvous_send_impl = (fn != NULL) ? fn : Rendezvous_Send;
}
void DirectP2P_TestHook_SetStunDiscover(DirectP2P_StunDiscover_fn fn) {
    s_stun_discover_impl = (fn != NULL) ? fn : Stun_Discover;
}
void DirectP2P_TestHook_SetSignalBudgetMs(int ms) {
    s_test_signal_budget_ms = (ms > 0) ? ms : 0;
}
bool DirectP2P_TestHook_IsLanPeer(const char* ip) {
    return direct_p2p_is_lan_peer(ip);
}
#else
#define RENDEZVOUS_SEND(sock, target, target_port, pkt, pkt_len) \
    Rendezvous_Send((sock), (target), (target_port), (pkt), (pkt_len))
#define STUN_DISCOVER(result, local_port, timeout_ms) \
    Stun_Discover((result), (local_port), (timeout_ms))
#endif /* NETPLAY_TEST_HOOKS */

/* S6: the overall post-STUN race budget. The per-leg keys
 * (SIGNAL_BUDGET_MS, BILATERAL_PUNCH_MS) still bound
 * their own legs INSIDE this. */
/* Named (task #76) so the compile-time cascade mirror in
 * DirectP2P_OrchWorstCaseMsForRole's block below evaluates the SAME
 * symbols this clamp enforces, instead of a hand-copied literal that can
 * drift away from it. */
#define RACE_BUDGET_DEFAULT_MS 8000  /* keep in sync with the config.c default */
#define RACE_BUDGET_MIN_MS     2000  /* below this no leg completes a distant round trip */
#define RACE_BUDGET_MAX_MS     30000 /* past this the S3 orchestrator deadline is the bound */

static int race_budget_ms(void) {
    int ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_RACE_BUDGET_MS);
    if (ms <= 0) ms = RACE_BUDGET_DEFAULT_MS;
    if (ms < RACE_BUDGET_MIN_MS) ms = RACE_BUDGET_MIN_MS;
    if (ms > RACE_BUDGET_MAX_MS) ms = RACE_BUDGET_MAX_MS;
#ifdef NETPLAY_TEST_HOOKS
    if (s_test_race_budget_ms > 0) ms = s_test_race_budget_ms;
#endif
    return ms;
}

/* S6: wall clock of the last completed race — the number this stage
 * exists to shrink, exposed so the timing regression test measures the
 * shipped path rather than a re-implementation of it. */
static SDL_AtomicInt s_last_race_ms;

#ifdef NETPLAY_TEST_HOOKS
uint32_t DirectP2P_TestHook_LastRaceMs(void) {
    return (uint32_t)SDL_GetAtomicInt(&s_last_race_ms);
}
#endif

/* --- S4a: host-waiting datagram classifier ----------------------------- */

/* One routing decision per inbound datagram on the host's waiting
 * socket. Order matters: rendezvous frames and STUN responses are
 * infrastructure traffic recognized by structure; ONLY a datagram
 * carrying the exact authenticated punch payload may be accepted as
 * the peer. Everything else — port scans, stray packets, legacy
 * unauthenticated punches, spoofed garbage — is IGNORE: the host drops
 * it and KEEPS WAITING. (Pre-S4a the final arm was "anything else IS
 * the peer": one stray datagram consumed the host's only peer slot,
 * the real joiner then failed, and after the MIST silence window the
 * user got the misleading "opponent build may be too old".) */
static DirectP2PHostDgramClass classify_host_datagram(
        const uint8_t* buf, int len,
        const uint8_t token[STUN_PUNCH_TOKEN_LEN], bool token_valid) {
    if (buf == NULL || len <= 0) {
        return DP2P_HOST_DGRAM_IGNORE;
    }
    /* Rendezvous DELIVER/CHALLENGE: 32+ bytes, '3SXR' magic
     * (0x33535852 BE). Hosts only ever receive server->client frames;
     * REGISTER and POLL are client->server only. Gate BEFORE the punch
     * check so a server frame is never mistaken for a peer probe. */
    if (len >= 32 && buf[0] == 0x33 && buf[1] == 0x53 &&
        buf[2] == 0x58 && buf[3] == 0x52) {
        return DP2P_HOST_DGRAM_RENDEZVOUS;
    }
    /* S1: STUN Binding Responses (keepalive replies, or a late
     * duplicate from a slower Stun_Discover server). */
    if (Stun_IsBindingResponse(buf, len)) {
        return DP2P_HOST_DGRAM_STUN;
    }
    /* S4a: the peer slot requires the exact 17-byte authenticated
     * punch payload. Fail closed when no token exists. */
    if (token_valid && Stun_IsPunchPayload(buf, len, token)) {
        return DP2P_HOST_DGRAM_PEER_PUNCH;
    }
    return DP2P_HOST_DGRAM_IGNORE;
}

/* Of the datagrams classify_host_datagram sends to IGNORE, which ones is
 * the punch gate entitled to charge? Punch-SHAPED ones only (arbitrary
 * garbage teaches an attacker nothing) -- EXCEPT a late-punch probe that
 * parses under this session's token. Such a probe is 26 bytes, so it is
 * not the exact 17-byte punch and is correctly IGNOREd for ROUTING, but
 * it proved the token rather than guessing at it and is not a gate probe.
 * Splitting this out of host_tick_receive keeps the distinction testable
 * without a socket; routing is decided solely by classify_host_datagram
 * and is not affected by anything here. */
static bool host_ignore_is_chargeable(
        const uint8_t* buf, int len,
        const uint8_t token[STUN_PUNCH_TOKEN_LEN], bool token_valid) {
    if (!Stun_HasPunchPrefix(buf, len)) {
        return false;
    }
    if (token_valid && Stun_ParseProbePayload(buf, len, token, NULL, NULL)) {
        return false;
    }
    return true;
}

#ifdef NETPLAY_TEST_HOOKS
DirectP2PHostDgramClass DirectP2P_TestHook_ClassifyHostDatagram(
        const uint8_t* buf, int len,
        const uint8_t token[STUN_PUNCH_TOKEN_LEN], bool token_valid) {
    return classify_host_datagram(buf, len, token, token_valid);
}

bool DirectP2P_TestHook_HostIgnoreIsChargeable(
        const uint8_t* buf, int len,
        const uint8_t token[STUN_PUNCH_TOKEN_LEN], bool token_valid) {
    return host_ignore_is_chargeable(buf, len, token, token_valid);
}
#endif /* NETPLAY_TEST_HOOKS */

/* --- rendezvous send queue (SPSC ring) --------------------------------- */

/* Per plan §Decision 3: the rendezvous worker thread does not call
 * NET_SendDatagram directly. It enqueues (NET_RefAddress'd target, payload)
 * tuples here and the main thread drains the queue from DirectP2P_Tick's
 * HOST_WAITING branch, calling Rendezvous_Send (and NET_UnrefAddress).
 * This keeps the STUN socket main-thread-owned during HOST_WAITING — the
 * same socket the magic-byte gate in host_tick_receive reads from. The
 * single-producer / single-consumer invariant is enforced by the
 * lifecycle: only host_rendezvous_thread_fn produces, only Tick consumes.
 *
 * Ring math: 8 slots; head moves on consume, tail moves on produce; a
 * full ring is (tail - head) == capacity; an empty ring is
 * (tail - head) == 0. We rely on uint32_t wraparound for the difference
 * — distance is always < capacity by construction.
 */
#define REND_Q_CAPACITY 8u

typedef struct {
    NET_Address* target;       /* NET_RefAddress'd by producer; consumer Unrefs after send */
    uint16_t     target_port;  /* host order, matches NET_SendDatagram */
    uint8_t      payload[REND_REGISTER_PKT_LEN]; /* v2 REGISTER or POLL packet */
    uint8_t      payload_len;  /* always 36 in practice; future-proof */
} RendezvousSendSlot;

static RendezvousSendSlot s_rendezvous_send_q[REND_Q_CAPACITY];
static SDL_AtomicInt s_q_head = { 0 };  /* consumer-write (drain) */
static SDL_AtomicInt s_q_tail = { 0 };  /* producer-write (enqueue) */
static SDL_AtomicInt s_q_drops = { 0 }; /* producer-incremented; logged once per session */
static SDL_AtomicInt s_q_drops_logged = { 0 };

/* Producer: returns false on full-ring (drop). Caller should already have
 * NET_RefAddress'd `target`; on overflow, the caller is responsible for
 * NET_UnrefAddress'ing it (we do NOT auto-unref here, so producer stays
 * trivially side-effect-free on its own Refcount). */
static bool rend_q_enqueue(NET_Address* target, uint16_t target_port,
                           const uint8_t* payload, uint8_t payload_len) {
    int head = SDL_GetAtomicInt(&s_q_head);
    int tail = SDL_GetAtomicInt(&s_q_tail);
    /* SPSC invariant guarantees |tail-head| <= REND_Q_CAPACITY, so a plain
     * unsigned subtraction (with natural wraparound) yields the depth
     * without needing a mask. */
    int depth = (int)((unsigned)(tail - head));
    if (depth >= (int)REND_Q_CAPACITY) {
        int drops = SDL_AddAtomicInt(&s_q_drops, 1) + 1;
        if (SDL_CompareAndSwapAtomicInt(&s_q_drops_logged, 0, 1)) {
            SDL_Log("[direct_p2p] WARNING: rendezvous send queue overflowed "
                    "(drops=%d). Main-thread drain may be stalled.", drops);
        }
        return false;
    }
    RendezvousSendSlot* slot = &s_rendezvous_send_q[(unsigned)tail % REND_Q_CAPACITY];
    slot->target = target;
    slot->target_port = target_port;
    if (payload_len > sizeof(slot->payload)) payload_len = (uint8_t)sizeof(slot->payload);
    memcpy(slot->payload, payload, payload_len);
    slot->payload_len = payload_len;
    /* Publish: bump tail AFTER slot is fully written. */
    SDL_SetAtomicInt(&s_q_tail, tail + 1);
    return true;
}

/* Consumer: drain up to `max_slots` items. Sends each via Rendezvous_Send
 * and Unrefs the target. Called from DirectP2P_Tick (main thread). */
static void rend_q_drain(NET_DatagramSocket* sock, int max_slots) {
    if (sock == NULL) {
        /* No socket — but slots may have been enqueued. Still drain to
         * release the NET_Address refs; just skip the send. */
    }
    for (int i = 0; i < max_slots; ++i) {
        int head = SDL_GetAtomicInt(&s_q_head);
        int tail = SDL_GetAtomicInt(&s_q_tail);
        if (head == tail) return;  /* empty */
        RendezvousSendSlot* slot = &s_rendezvous_send_q[(unsigned)head % REND_Q_CAPACITY];
        NET_Address* target = slot->target;
        uint16_t target_port = slot->target_port;
        uint8_t payload[REND_REGISTER_PKT_LEN];
        memcpy(payload, slot->payload, sizeof(payload));
        uint8_t payload_len = slot->payload_len;
        /* Consume: bump head BEFORE we use the slot's pointers (post-bump
         * the producer may overwrite the slot — that's fine, we already
         * copied the payload and saved target/target_port locally). */
        SDL_SetAtomicInt(&s_q_head, head + 1);
        if (sock != NULL && target != NULL) {
            (void)RENDEZVOUS_SEND(sock, target, target_port, payload, payload_len);
        }
        if (target != NULL) {
            NET_UnrefAddress(target);
        }
    }
}

/* Drain the queue and Unref any pending targets without sending. Used at
 * teardown / cancel so we don't leak NET_Address refs. */
static void rend_q_purge(void) {
    rend_q_drain(NULL, (int)REND_Q_CAPACITY);
}

/* S2: CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS is the overall
 * wall-clock budget for Stun_Discover (all servers probed in parallel;
 * see stun.c). Read at each discovery site via stun_budget_ms().
 *
 * Ceiling rationale (review L-4): Stun_Discover takes no cancel flag —
 * it runs to its budget when no server answers — and DirectP2P_Cancel
 * joins the worker on the GAME thread, so this budget is a direct
 * bound on how long Cancel can block the game. The retransmit ladder
 * ends 1500 ms after the last server arms; 15 s already covers any
 * network where discovery can succeed at all. An uncapped user value
 * would turn a mid-discovery Cancel into an arbitrarily long UI
 * freeze. */
/* Named for the same reason as the race-budget clamp above (task #76). */
#define STUN_BUDGET_DEFAULT_MS 4000  /* keep in sync with the config.c default */
#define STUN_BUDGET_MIN_MS     1000  /* below one RTO the retransmit ladder is meaningless */
#define STUN_BUDGET_MAX_MS     15000 /* ceiling — bounds DirectP2P_Cancel's worst-case block */

static int stun_budget_ms(void) {
    int ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS);
    if (ms <= 0) ms = STUN_BUDGET_DEFAULT_MS;
    if (ms < STUN_BUDGET_MIN_MS) ms = STUN_BUDGET_MIN_MS;
    if (ms > STUN_BUDGET_MAX_MS) ms = STUN_BUDGET_MAX_MS;
    return ms;
}

/* ======================================================================
 * S6 — candidate racing (docs/plan-netplay-connection.md §8)
 * ======================================================================
 *
 * Before S6 the joiner ran its establishment phases STRICTLY SERIALLY —
 * direct punch, then the rendezvous REGISTER/DELIVER loop, then the
 * bilateral punch — so a join that ended in failure cost the SUM of
 * every budget (2500 + 8000 + 5000 = 15500 ms on top of STUN).
 *
 * We do NOT build ICE. The justification is on file: the MiSTer has ONE
 * interface, the kernel is built without CONFIG_IPV6 so there is no v6
 * escape from NAT, there are at most three candidate routes, and a
 * rendezvous signalling channel already exists. Full ICE would buy
 * nothing those three facts do not already decide.
 *
 * Instead: ONE interleaved loop, on the SAME worker thread that already
 * owned the socket exclusively, driving up to three legs concurrently:
 *
 *   punch[0]  the endpoint we already know — the joiner's room-code
 *             endpoint, or (host) the joiner endpoint the DELIVER gave us
 *   punch[1]  the endpoint a rendezvous DELIVER teaches us mid-race
 *             (joiner only; this is the old "bilateral" punch, except it
 *             now starts the instant the DELIVER parses instead of after
 *             the direct punch has burned its whole window)
 *   signal    the rendezvous REGISTER/CHALLENGE/DELIVER conversation
 *             (joiner only — the host is already paired by the time it
 *             punches, and its rendezvous worker has exited)
 *
 * NO NEW THREADS, NO NEW LOCKS, NO NEW RACES: every leg is a state
 * machine pumped from the one loop, and the socket keeps exactly one
 * reader/writer, exactly as it had before.
 *
 * A FOURTH leg used to run here: the S5 relay rung (RELAY_REQ -> GRANT
 * -> PIN -> ACK). It was REMOVED as a product decision, and everything
 * that existed only to ORDER the relay against the punches went with
 * it — the arm delay, the per-candidate arm deferral, and the relay's
 * commit grace.
 *
 * ONE thing that was scoped to the relay did NOT go with it, because it
 * was never about the relay: the receive-only hold that keeps a
 * candidate listening past its send window. It is what makes two peers
 * starting at different times reach the SAME outcome, and PUNCHED on
 * one side with EXHAUSTED on the other is a split brain whether or not
 * a relay exists. It is now unconditional. See RACE_PUNCH_SETTLE_MS and
 * section 2 below.
 */

/* Loop period. The legs' own cadences are 50-500 ms; 5 ms keeps the
 * cancel check responsive and the send timings honest without spinning
 * (the receive call below is non-blocking, so this is the only sleep). */
#define RACE_POLL_MS 5

/*
 * The overall race deadline, as a pure function of the clock.
 *
 * WRAP SAFETY (S6-review M-1 — this comment replaces a WRONG one).
 * `SDL_GetTicks` is stored here in a uint32_t, which wraps after ~49.7
 * days of uptime. The form this replaced was
 *
 *     now >= t0 + budget            // uint32_t arithmetic
 *
 * and the recorded reasoning for deleting it ("the race would run until
 * every leg finished with no overall bound") had the symptom BACKWARDS.
 * `t0 + budget` overflows to a SMALL number whenever `t0` is within
 * `budget` of the wrap, and `now` — still just below the wrap — is then
 * already >= it. The real symptom is the opposite of unbounded: the
 * deadline fires on the FIRST iteration, so every join attempted in the
 * ~8 s before a wrap fails instantly. Subtract-then-cast never forms the
 * overflowing sum, which is why it is the form used throughout this file.
 *
 * H-1 EXEMPTION. A leg that has CONFIRMED still owes its peer the
 * STUN_PUNCH_CONFIRM_MS tail (stun.c Stun_PunchSettled) before the
 * endpoint may be handed off. Breaking on the raw budget threw that
 * connection away and reported a NAT failure — with the shipped
 * defaults, for every punch confirming in the last 600 ms of the 8 000 ms
 * budget. The deadline is therefore extended by exactly one tail, and no
 * more: the loop stays hard-bounded at budget + STUN_PUNCH_CONFIRM_MS
 * whatever the legs do.
 */
static bool race_budget_expired(uint32_t now, uint32_t t0, int budget_ms,
                                bool tail_outstanding) {
    const int elapsed = (int)(now - t0);
    if (elapsed < budget_ms) {
        return false;
    }
    if (!tail_outstanding) {
        return true;
    }
    return elapsed >= budget_ms + STUN_PUNCH_CONFIRM_MS;
}

#define RACE_PUNCH_LEGS 2

typedef enum {
    RACE_PUNCHED = 0,  /* a punch leg confirmed and finished its tail */
    RACE_CANCELLED,    /* caller-initiated abort                      */
    RACE_EXHAUSTED     /* every leg finished without a link           */
} RaceOutcome;

typedef struct {
    NET_DatagramSocket* sock;
    StunResult* stun;                 /* diag sink (punch bad-token); may be NULL */
    const uint8_t* punch_token;       /* STUN_PUNCH_TOKEN_LEN bytes */

    /* Seed punch candidate — the endpoint we already hold. */
    const char* seed_ip;
    uint16_t seed_port;

    /* Rendezvous endpoint. `signal_addr` is BORROWED: the caller owns
     * the ref for the whole race and after it. */
    NET_Address* signal_addr;
    uint16_t signal_port;
    const uint8_t* session_key;       /* 16 bytes */
    uint16_t my_public_port;
    const uint8_t* cookie_in;         /* NULL when we hold no cookie yet */

    bool signal_leg;                  /* run the REGISTER/DELIVER conversation */
    int  signal_budget_ms;
    const char* my_public_ip;         /* self-DELIVER gate; may be NULL */

    int  punch_leg_ms;                /* per-leg punch window */
    int  race_budget_ms;              /* overall wall clock */

    SDL_AtomicInt* cancel;            /* extra cancel flag, beside s_cancel */
} RaceCfg;

typedef struct {
    RaceOutcome outcome;
    char     peer_ip[64];
    uint16_t peer_port;

    /* S3 evidence, carried out for the classifier. */
    bool deliver_any;
    bool deliver_real;
    bool challenge_any;
    /* Task #105: we were challenged again while already holding a cookie,
     * i.e. our echo did not verify. Distinguishes a real cookie refusal
     * from the healthy one-challenge opening every v2 session has. */
    bool cookie_rechallenged;

    /* S4c cookie learned mid-race, so the caller can reuse it. */
    bool    have_cookie;
    uint8_t cookie[REND_COOKIE_LEN];

    /* Stage timings. NOTE: these now OVERLAP — that is the whole point of
     * S6 — so they no longer sum to the elapsed time. `t_race_ms` is the
     * wall clock the caller should reason about. */
    uint32_t t_punch_ms;      /* punch leg 0 (room code / host's peer) alive */
    uint32_t t_bilateral_ms;  /* punch leg 1 (DELIVER endpoint) alive        */
    uint32_t t_signal_ms;     /* signal leg alive                            */
    uint32_t t_race_ms;       /* the whole race                              */

    /* #36 attribution evidence. */
    bool     confirm_seen;        /* a punch leg was CONFIRMED at some point */
    uint32_t confirm_ms;          /* t+ms of the first confirm; 0 if none    */
    uint16_t deliver_n;           /* well-formed DELIVER frames             */
    uint16_t challenge_n;         /* CHALLENGE frames answered              */
    uint16_t badver_n;            /* '3SXR' magic, version byte != ours,
                                     FROM THE RENDEZVOUS ENDPOINT ONLY     */
    /* Task #122: typed server refusals. `nack_reason` is the LATEST one
     * accepted, as a raw RendezvousNackReason wire byte — see the
     * ConnectJoinEvidence comment in connect_fail.h for why "latest" and
     * why it is not range-checked into the enum. Only meaningful when
     * nack_any is set; nack_n is the count for the report line. */
    bool     nack_any;
    uint16_t nack_n;
    uint8_t  nack_reason;
    /* Largest observed inter-DELIVER gap. Baseline ~500 ms — that is the
     * joiner's in-race REGISTER cadence in section 5 below, and the
     * server pushes one DELIVER per REGISTER. NOT 5000 ms: that is the
     * host-advertising loop's interval in host_rendezvous_thread_fn,
     * which is a different loop on a different thread. */
    uint32_t deliver_gap_max_ms;
} RaceResult;

#ifdef NETPLAY_TEST_HOOKS
/* S6 test seam. Replaces the pre-S6 DirectP2P_TestHook_SetStunHolePunch:
 * with the punch no longer owning the socket for a whole window, there is
 * no blocking call left to substitute, so the seam moved to the decision
 * the tests were actually making — "does THIS candidate ever confirm?".
 * A leg under an oracle override sends NOTHING on the wire, which is what
 * keeps the offline harnesses offline. */
static DirectP2P_PunchOracle_fn s_punch_oracle = NULL;

void DirectP2P_TestHook_SetPunchOracle(DirectP2P_PunchOracle_fn fn) {
    s_punch_oracle = fn;
}
#define PUNCH_ORACLE(ip, port) \
    (s_punch_oracle != NULL ? s_punch_oracle((ip), (port)) : DP2P_PUNCH_REAL)

/*
 * H-C test seam: make Stun_PunchBegin FAIL for one nominated endpoint.
 *
 * WHY A SEAM IS NEEDED AT ALL. race_arm_punch has exactly one failure mode
 * downstream of its up-front ip/port guard: Stun_PunchBegin returning
 * false, which happens when NET_ResolveHostname fails or the address never
 * reaches NET_SUCCESS (stun.c:781-798). The only endpoint the race ever
 * RE-arms is slot 1, and slot 1's endpoint comes from a DELIVER, whose IP
 * string is produced by inet_ntop(AF_INET, ...) in
 * Rendezvous_ParseDeliverEx (rendezvous.c:300-302) — i.e. ALWAYS a
 * well-formed dotted quad that always resolves. So the wire physically
 * cannot deliver an endpoint that reaches the validate/memset decision and
 * fails there. The seam supplies what the wire cannot.
 *
 * It is deliberately as thin as possible: it swaps ONLY the hostname
 * STRING handed to Stun_PunchBegin. Stun_PunchBegin itself is the real
 * one and fails down its real NET_ResolveHostname path; nothing about the
 * validate-then-memset ordering under test is mocked, short-circuited or
 * bypassed. `c->ip`/`c->port` still record the endpoint the DELIVER named.
 */
static char     s_arm_fail_ip[64] = { 0 };
static uint16_t s_arm_fail_port = 0;

void DirectP2P_TestHook_SetArmFailEndpoint(const char* peer_ip, uint16_t peer_port) {
    if (peer_ip == NULL || peer_ip[0] == '\0' || peer_port == 0) {
        s_arm_fail_ip[0] = '\0';
        s_arm_fail_port = 0;
        return;
    }
    SDL_strlcpy(s_arm_fail_ip, peer_ip, sizeof(s_arm_fail_ip));
    s_arm_fail_port = peer_port;
}

/* A single 144-character DNS label. The DNS limit is 63 bytes per label,
 * so getaddrinfo rejects this locally without emitting a query — no
 * network, no resolver, no wildcard-DNS zone can turn it into a success.
 * Measured on this host: NET_GetAddressStatus() == -1 (NET_FAILURE) after
 * a single 10 ms poll, repeatably. */
static const char k_arm_fail_host[] =
    "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"
    "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";

static const char* arm_punch_target_ip(const char* ip, uint16_t port) {
    if (s_arm_fail_port != 0 && port == s_arm_fail_port &&
        strcmp(ip, s_arm_fail_ip) == 0) {
        return k_arm_fail_host;
    }
    return ip;
}
#define ARM_PUNCH_TARGET_IP(ip, port) arm_punch_target_ip((ip), (port))
#else
#define PUNCH_ORACLE(ip, port) ((void)(ip), (void)(port), DP2P_PUNCH_REAL)
#define ARM_PUNCH_TARGET_IP(ip, port) ((void)(port), (ip))
#endif

/*
 * How long a punch candidate stays on the RECEIVE path after it has
 * stopped SENDING, before it is torn down.
 *
 * WHAT THIS IS FOR. Two peers start their races an arbitrary skew `s`
 * apart. If a candidate stops listening at the same instant it stops
 * sending, there is a band of skews in which exactly ONE side confirms:
 * the late peer arms just inside the early peer's send window, is
 * confirmed by the early peer's last punches, and its own first punch
 * reaches the early peer one one-way delay `d` LATER — after the early
 * peer has already torn the leg down. The late peer hands off to
 * GekkoNet; the early peer reports failure. GekkoNet registers the
 * remote ONCE by source address at configure time (netplay.c) with no
 * relearn path, so the connected side then hangs for the whole
 * CONNECT_TIMEOUT_CONNECTING_MS and gives up. That is the split brain.
 *
 * DERIVATION — this is a NETWORK quantity, not a harness constant.
 * Write `L` for `punch_leg_ms`, `d` for the one-way delay, `s` for the
 * start skew (peer B at 0, peer A at s >= 0), and `G` for this window.
 * Each side sends over [own_arm, own_arm+L] and listens over
 * [own_arm, own_arm+L+G]. A confirmation on either side resumes THAT
 * side's tail immediately (Stun_PunchOffer clears `sent_any`, so the
 * pump in section 5 sends on the very next iteration), and that tail is
 * what confirms the other side. So:
 *
 *   - A is confirmed directly by B's stream iff  s <= L + d.
 *     A then answers, and B must still be listening at s + d:
 *     s + d <= L + G. Worst case s = L + d gives  G >= 2d.
 *   - Otherwise (s > L + d) A's own stream reaches B at s + d, so B is
 *     confirmed directly iff s <= L + G - d. B then answers, and A is
 *     still listening at s + 2d because s + 2d <= s + L + G always.
 *   - For s > L + G - d neither side hears the other and BOTH exhaust.
 *
 * The two direct thresholds are L + d and L + G - d. They coincide, and
 * the one-sided band between them vanishes, exactly when
 *
 *     G >= 2 * one-way delay   (i.e. G >= one peer-to-peer ROUND TRIP)
 *
 * and where they do not, the residual band is (2d - G) wide and sits
 * just past the send window. This is the same condition the S5-era
 * review derived and measured for the relay's commit grace, over five
 * (owd, grace) configurations, at 9240aa50:src/netplay/direct_p2p.c:
 * 1153-1176 — the mechanism is unchanged, only its trigger is: it used
 * to be scoped to "a relay leg is still deciding" and is now
 * unconditional, because the relay is gone and the property it was
 * protecting never belonged to the relay in the first place.
 *
 * SIZING. 600 ms covers every pair up to a 600 ms ROUND TRIP. Real
 * one-way delays: transatlantic 35-90 ms (RTT 70-180), intercontinental
 * with a mobile last hop 100-200 ms (RTT 200-400), geostationary
 * satellite ~250 ms (RTT ~500). A pair above 600 ms RTT cannot play a
 * rollback fighting game at all. It is also exactly one
 * STUN_PUNCH_CONFIRM_MS — the same quantity the punch itself spends
 * making sure its peer heard it — which is not a coincidence: both are
 * "one round trip plus slack" measured against the same network.
 *
 * The derivation uses `L` for the send window because that is the
 * common case; it only ever needs the two windows to be one `G` shorter
 * than the two listen windows, so it holds unchanged when the race
 * budget — not `punch_leg_ms` — is what ends a send (section 2), and it
 * holds when the two peers' send windows differ in LENGTH, which is
 * what happens when one side's candidate was armed by a DELIVER later
 * than the other's.
 *
 * COST. On the failure path a race now ends `G` later than the send
 * window, which is what the shipped pre-removal build already did in
 * production (a relay leg was always possible there, so the deferral
 * always applied). It is bounded twice: by G itself, and by the race
 * deadline, which grants the settle the same single-tail extension the
 * H-1 exemption grants a confirmed punch. A confirmation landing inside
 * the settle owes a tail from there, so the loop's hard bound becomes
 * `race_budget_ms + 2 * STUN_PUNCH_CONFIRM_MS` — reached only by a race
 * that CONFIRMED, never by a failing one. See section 2 and the
 * deadline check in section 8.
 */
#define RACE_PUNCH_SETTLE_MS 600u

/* The settle window is allowed to run past `race_budget_ms` on the same
 * one-tail extension the H-1 exemption uses (race_budget_expired, and
 * section 8 of p2p_race), which extends the deadline by exactly one
 * STUN_PUNCH_CONFIRM_MS. A settle window LONGER than that extension
 * would be silently truncated by the budget in precisely the
 * configurations it exists to protect, so the coupling is asserted
 * rather than left to a comment. Raise the extension, not this alone. */
_Static_assert(RACE_PUNCH_SETTLE_MS <= STUN_PUNCH_CONFIRM_MS,
               "RACE_PUNCH_SETTLE_MS must fit in the one-tail deadline extension "
               "race_budget_expired() grants, or the race budget truncates it");

/* THE hard bound on one p2p_race, as a function of its configured budget.
 *
 * Derived in section 8 of p2p_race: a leg listening through its settle
 * window can be CONFIRMED as late as `budget + one tail` (because
 * send_end can itself be the budget, and RACE_PUNCH_SETTLE_MS ==
 * STUN_PUNCH_CONFIRM_MS), and it then owes its peer a FULL tail from
 * there. Two tails, never three.
 *
 * A MACRO, and the single definition of the cap, because this is exactly
 * the leg that rotted once already: task #76's first pass summed
 * `race_budget_ms + 1 * STUN_PUNCH_CONFIRM_MS` into the nav deadline,
 * which was correct when it was written and became wrong the moment the
 * second tail landed. Both the enforcement site (section 8) and the
 * orchestrator worst case now evaluate THIS symbol, so a future change
 * to the cap moves both together or moves neither. */
#define RACE_HARD_CAP_MS(budget_ms) ((budget_ms) + 2 * STUN_PUNCH_CONFIRM_MS)

/*
 * ================= RULE: THE PROMISED LISTENING INTERVAL =================
 *
 * The moment a punch leg is armed, this race has PROMISED its peer an
 * interval during which we are listening. The promise is always LONGER
 * than the sending that created it:
 *
 *   - an armed leg listens for `punch_leg_ms + RACE_PUNCH_SETTLE_MS` from
 *     its own arm time (section 2), and
 *   - a leg that CONFIRMS owes a further STUN_PUNCH_CONFIRM_MS tail from
 *     the confirm, because the peer is confirmed by that tail and not
 *     before (stun.c, Stun_PunchSettled).
 *
 * THE RULE: no other bound in this file may end a leg before its promised
 * interval runs out. Not the race budget, not the deadline extension, not
 * a re-arm, not anything added later.
 *
 * WHY A RULE AND NOT THREE ONE-LINE FIXES. Every split-brain this race has
 * shipped has had the SAME shape: some second bound, correct in
 * isolation, truncated a promised interval, and the peer — already
 * confirmed by the partial tail it did receive — reached the opposite
 * verdict. PUNCHED on one side with EXHAUSTED on the other is a split
 * brain regardless of which bound did the cutting, so the defect is not
 * "the budget was wrong" or "the re-arm was wrong"; it is that the
 * promise had no name and therefore nothing to be checked against.
 *
 * ENFORCED AT FOUR SITES, all of them this shape:
 *   1. race_budget_expired()'s H-1 exemption — the budget does not cut a
 *      CONFIRMED leg's tail.
 *   2. p2p_race section 8's `settle_outstanding` — the budget does not
 *      cut an unconfirmed leg's settle window either.
 *   3. the _Static_assert on RACE_PUNCH_SETTLE_MS above — the one-tail
 *      extension that (1) and (2) grant is large enough to contain a
 *      whole settle window, so the grant cannot truncate the very thing
 *      it exists to protect.
 *   4. race_arm_punch()'s confirmed-slot guard — a DELIVER carrying a
 *      DIFFERENT endpoint does not re-arm over a CONFIRMED leg. Re-arming
 *      calls race_finish_punch() on the incumbent, which Stun_PunchEnds it
 *      mid-tail: it destroys a punch that provably reached us AND stops
 *      the tail owed to a peer our partial tail may already have
 *      confirmed. Section 1 only ends the race once a confirmed leg has
 *      SETTLED, so between confirm and settle the leg is live, valuable,
 *      and — before the guard — replaceable.
 *
 * ADDING A FIFTH SITE. Any new code that can end, replace, or re-arm a
 * leg must first ask whether that leg is inside a promised interval.
 * race_punch_confirmed() and race_punch_settled() are the two predicates
 * that answer it; declining is the only correct response when it is.
 * ========================================================================
 */

/* One punch candidate. `oracle` is DP2P_PUNCH_REAL for every production
 * leg; the other two values make the leg a pure clock with no wire
 * traffic, for the offline harnesses. */
typedef struct {
    bool         armed;
    bool         finished;
    StunPunchLeg leg;
    uint32_t     armed_ms;
    uint32_t     alive_ms;
    char         ip[64];
    uint16_t     port;
    DirectP2PPunchOracleResult oracle;
    bool         oracle_confirmed;
    /* `punch_leg_ms` is the window in which we SEND. A candidate that
     * reaches the end of it stops sending but is NOT torn down: it stays
     * on the receive path for RACE_PUNCH_SETTLE_MS so a peer that
     * started late can still confirm it. `send_end_ms` is when the
     * sending stopped, and it is the anchor the settle window is
     * measured from. */
    bool         send_expired;
    uint32_t     send_end_ms;
} RacePunchCandidate;

static bool race_cancelled(const RaceCfg* cfg) {
    return cancel_requested() ||
           (cfg->cancel != NULL && SDL_GetAtomicInt(cfg->cancel) != 0);
}

static void race_finish_punch(RacePunchCandidate* c, uint32_t now);
static bool race_punch_confirmed(const RacePunchCandidate* c);

/* Arm a punch candidate. Returns false if the endpoint is unusable (or a
 * duplicate of one we are already punching — punching the same endpoint
 * twice from one socket doubles the traffic and teaches us nothing, and
 * see THE PROMISED LISTENING INTERVAL above for why a CONFIRMED
 * incumbent is refused outright). */
static bool race_arm_punch(RacePunchCandidate* cands, int n_cands, int slot,
                           const RaceCfg* cfg, const char* ip, uint16_t port,
                           uint32_t now) {
    if (ip == NULL || ip[0] == '\0' || port == 0) {
        return false;
    }
    for (int i = 0; i < n_cands; i++) {
        if (cands[i].armed && !cands[i].finished &&
            cands[i].port == port && strcmp(cands[i].ip, ip) == 0) {
            return false;
        }
    }
    /* THE PROMISED LISTENING INTERVAL, site 4. The slot we are about to
     * take is re-armed by race_finish_punch() below; if its incumbent has
     * CONFIRMED, that call ends a live punch mid-tail. Decline instead —
     * a confirmed leg is worth more than any endpoint a later DELIVER can
     * carry, and section 1 is already committed to ending the race on it
     * as soon as it settles. */
    if (race_punch_confirmed(&cands[slot])) {
        SDL_Log("[direct_p2p] S6 race: refusing to re-arm slot %d over a "
                "CONFIRMED leg (%s:%u) for %s:%u",
                slot, cands[slot].ip, (unsigned)cands[slot].port,
                ip, (unsigned)port);
        return false;
    }
    /* S6-review M-2: VALIDATE, THEN memset. The pre-fix order wiped the
     * slot before Stun_PunchBegin could fail, and a slot being re-armed
     * (slot 1 is re-armed whenever a DELIVER carries a DIFFERENT endpoint)
     * holds a ref'd NET_Address in leg.target — wiping it leaked the ref
     * with no way left to release it. The new leg is built on the stack
     * and only committed once it is known good. */
    StunPunchLeg leg;
    memset(&leg, 0, sizeof(leg));
    const DirectP2PPunchOracleResult oracle = PUNCH_ORACLE(ip, port);
    if (oracle == DP2P_PUNCH_REAL) {
        if (!Stun_PunchBegin(&leg, ARM_PUNCH_TARGET_IP(ip, port), port,
                             cfg->punch_token, now)) {
            return false;
        }
    }
    RacePunchCandidate* c = &cands[slot];
    /* Releasing the ref the outgoing leg still holds is the whole point of
     * doing this after the validation, not before it. */
    race_finish_punch(c, now);
    memset(c, 0, sizeof(*c));
    c->leg = leg;
    SDL_strlcpy(c->ip, ip, sizeof(c->ip));
    c->port = port;
    c->armed_ms = now;
    c->oracle = oracle;
    c->armed = true;
    SDL_Log("[direct_p2p] S6 race: punching candidate %s:%u", ip, (unsigned)port);
    return true;
}

static void race_finish_punch(RacePunchCandidate* c, uint32_t now) {
    if (!c->armed || c->finished) {
        return;
    }
    /* `punch=` in a report line has always meant "how long this candidate
     * spent punching". A candidate being held on the receive path past
     * its send window is no longer punching, so the reported number stops
     * at `send_end_ms` and the report keeps its established meaning. */
    c->alive_ms = (c->send_expired && c->send_end_ms != 0)
                      ? (c->send_end_ms - c->armed_ms)
                      : (now - c->armed_ms);
    if (c->oracle == DP2P_PUNCH_REAL) {
        Stun_PunchEnd(&c->leg);
    }
    c->finished = true;
}

static bool race_punch_confirmed(const RacePunchCandidate* c) {
    if (!c->armed || c->finished) {
        return false;
    }
    return (c->oracle == DP2P_PUNCH_REAL) ? Stun_PunchConfirmed(&c->leg)
                                          : c->oracle_confirmed;
}

static bool race_punch_settled(const RacePunchCandidate* c, uint32_t now) {
    if (!race_punch_confirmed(c)) {
        return false;
    }
    /* An oracle-driven leg has no wire traffic to flush, so it settles at
     * once; a real leg owes the peer its ~600 ms confirmation tail. */
    return (c->oracle == DP2P_PUNCH_REAL) ? Stun_PunchSettled(&c->leg, now) : true;
}

/*
 * Run the race. Returns a RaceResult; the caller owns every decision that
 * follows from it (classification, status text, handoff).
 *
 * Preconditions the caller must guarantee — the same ones the pre-S6
 * serial phases relied on: this thread is the SOLE reader/writer of
 * `cfg->sock` for the whole call (the join worker, or the host's
 * bilateral-punch worker during FALLBACK_BILATERAL_PUNCH, plan
 * §Decision 3), and `cfg->signal_addr` stays ref'd by the caller.
 */
static void p2p_race(const RaceCfg* cfg, RaceResult* out) {
    memset(out, 0, sizeof(*out));
    out->outcome = RACE_EXHAUSTED;

    const uint32_t t0 = SDL_GetTicks();

    RacePunchCandidate cands[RACE_PUNCH_LEGS];
    memset(cands, 0, sizeof(cands));

    /* --- leg: punch[0], the endpoint we already hold ------------------ */
    (void)race_arm_punch(cands, RACE_PUNCH_LEGS, 0, cfg,
                         cfg->seed_ip, cfg->seed_port, t0);

    /* --- leg: rendezvous signalling ----------------------------------- */
    uint8_t register_pkt[REND_REGISTER_PKT_LEN];
    uint8_t cookie[REND_COOKIE_LEN];
    bool have_cookie = false;
    bool signal_active = false;
    uint32_t signal_last_send = 0;
    bool signal_sent_any = false;
    uint32_t signal_end_ms = 0;

    if (cfg->cookie_in != NULL) {
        memcpy(cookie, cfg->cookie_in, sizeof(cookie));
        have_cookie = true;
    }
    if (cfg->signal_leg && cfg->signal_addr != NULL && cfg->session_key != NULL) {
        if (Rendezvous_BuildRegister(cfg->my_public_port, cfg->session_key,
                                     have_cookie ? cookie : NULL, register_pkt)) {
            signal_active = true;
        } else {
            SDL_Log("[direct_p2p] S6 race: failed to build REGISTER — signal leg off");
        }
    }

    /* H-1: one line per race when the confirmation-tail exemption fires. */
    bool logged_tail_hold = false;

    /* #36: DELIVER cadence. The rendezvous server pushes ONE unacknowledged
     * DELIVER per REGISTER (tools/rendezvous-server/rendezvous-server.js
     * handleRegister), and the cadence that applies HERE is the joiner's
     * IN-RACE one: section 5 below re-REGISTERs every 500 ms for as long
     * as the signal leg is live. So the expected baseline for
     * deliver_gap_max_ms is ~500 ms, and a gap much larger than that is
     * DELIVER loss on the path, not a slow server.
     *
     * The 5000 ms figure that shows up elsewhere in this file is the
     * HOST-ADVERTISING cadence — CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_
     * INTERVAL_MS, defaulted in host_rendezvous_thread_fn — and it is a
     * different loop that never feeds this counter. Comparing against it
     * would make every healthy race look like 90% DELIVER loss.
     * 0 = none seen yet. */
    uint32_t last_deliver_ms = 0;
    /* #36: one line per race for the version-skew arm below. */
    bool logged_badver = false;
    /* Task #122: one line per DISTINCT reason for the NACK arm below.
     * Not one line per race: the reasons progress (RATE_PREGATE while we
     * are uncookied, then RATE_IP/RATE_KEY once we hold a cookie), and
     * collapsing that to the first one would hide the transition a
     * triager needs. Bounded by the nine reasons, so it is still O(1)
     * lines per race whatever the server sends. */
    uint8_t logged_nack_reason = 0xFFu;
    /* #36 (F5): the rendezvous endpoint as a string, resolved ONCE. The
     * skew arm below compares an inbound datagram's source against it,
     * so an off-path host cannot fabricate "server skew" evidence. NULL
     * signal_addr (no signal leg) leaves it empty, which matches
     * nothing. */
    char signal_ip_str[64];
    signal_ip_str[0] = '\0';
    if (cfg->signal_addr != NULL) {
        const char* s = NET_GetAddressString(cfg->signal_addr);
        if (s != NULL) {
            SDL_strlcpy(signal_ip_str, s, sizeof(signal_ip_str));
        }
    }

    while (true) {
        const uint32_t now = SDL_GetTicks();

        if (race_cancelled(cfg)) {
            out->outcome = RACE_CANCELLED;
            break;
        }

        /* --- 1) a confirmed punch ends the race ----------------------- */
        {
            for (int i = 0; i < RACE_PUNCH_LEGS; i++) {
                if (race_punch_confirmed(&cands[i])) {
                    /* #36: record the confirm BEFORE the settle test, on
                     * purpose. A confirm that never settles is exactly the
                     * evidence the race throws away — and it is the one
                     * piece of evidence that PROVES the peer's datagram
                     * reached us, i.e. that a subsequent NAT_BLOCKED
                     * verdict is a misattribution (the H-1 defect). */
                    if (!out->confirm_seen) {
                        out->confirm_seen = true;
                        out->confirm_ms = now - t0;
                    }
                    if (race_punch_settled(&cands[i], now)) {
                        if (cands[i].oracle == DP2P_PUNCH_REAL) {
                            Stun_PunchEndpoint(&cands[i].leg, out->peer_ip,
                                               (int)sizeof(out->peer_ip),
                                               &out->peer_port);
                        } else {
                            SDL_strlcpy(out->peer_ip, cands[i].ip, sizeof(out->peer_ip));
                            out->peer_port = cands[i].port;
                        }
                        out->outcome = RACE_PUNCHED;
                        goto done;
                    }
                }
            }
        }

        /* --- 2) leg lifetimes ---------------------------------------- */
        /*
         * SENDING and LISTENING end at different times.
         *
         * A candidate SENDS for `punch_leg_ms` from its OWN arm time. At
         * the end of that window it stops sending but stays on the
         * receive path for RACE_PUNCH_SETTLE_MS — see the derivation at
         * that constant for why a candidate that stops listening when it
         * stops sending puts a whole band of start skews into split
         * brain, and why one peer-to-peer round trip of listening closes
         * it. A leg that is still listening can be confirmed by a late
         * peer's punch; Stun_PunchOffer then sets `confirmed` and clears
         * `sent_any`, so section 5 resumes pumping on this very
         * iteration and the confirmation tail goes out — which is what
         * confirms the OTHER peer in turn. That mutual tail is why this
         * converges instead of merely moving the problem.
         *
         * This is UNCONDITIONAL. Between S6 and the relay's removal the
         * same hold existed but was scoped to "a relay leg exists and
         * has not finished", on the reasoning that with no relay there
         * was nothing for the peers to disagree ABOUT. That reasoning
         * was wrong: PUNCHED-vs-EXHAUSTED is a disagreement all by
         * itself, and it is the one that hangs GekkoNet. The relay's
         * removal deleted the mechanism along with its trigger, which is
         * what test 34 (test_bilateral_punch.c) measured and reddened.
         *
         * THE SETTLE WINDOW MUST FIT INSIDE THE RACE, or it is not a
         * settle window. If the race ends partway through it, the early
         * peer stops listening inside a window it had promised to spend
         * listening and the band comes straight back — measured on the
         * S5-era rig with the budget squeezed to 5 200 ms against a
         * 5 000 ms send window and a 600 ms grace, which split at skew
         * 5 050 ms.
         *
         * There are two ways to make it fit, and only one of them is
         * free. The S5-era code SHORTENED THE SEND WINDOW so that
         * send_end + grace landed before `race_budget_ms`. That is not
         * free: it deletes punch traffic the budget would otherwise have
         * allowed, and a punch that would have confirmed in the last
         * `settle` ms of the budget is then never sent at all — which is
         * exactly the class of race the H-1 confirmation-tail exemption
         * exists to rescue (see race_budget_expired, and test 23).
         *
         * So the send window is NOT shortened. It ends where it would
         * have ended anyway — at `punch_leg_ms`, or at the budget, since
         * the loop stops pumping there regardless — and the SETTLE is
         * allowed to run past the raw budget, held open by the very same
         * one-tail extension H-1 already uses. `RACE_PUNCH_SETTLE_MS`
         * and `STUN_PUNCH_CONFIRM_MS` are equal (asserted above), so a
         * settle window always fits inside that one extension. A
         * confirmation that lands INSIDE the settle then owes a tail of
         * its own from there, which is the second tail granted in
         * section 8: the loop stays hard-bounded, at
         * `race_budget_ms + 2 * STUN_PUNCH_CONFIRM_MS`, whatever the
         * legs do. See the deadline check in section 8.
         */
        for (int i = 0; i < RACE_PUNCH_LEGS; i++) {
            if (!cands[i].armed || cands[i].finished ||
                race_punch_confirmed(&cands[i])) {
                continue;
            }
            /* An oracle-driven leg puts NOTHING on the wire and can never
             * be confirmed by a peer, so there is no late confirmation
             * for it to wait for and its settle window is zero. Same
             * reasoning as race_punch_settled() above. In a production
             * build PUNCH_ORACLE is DP2P_PUNCH_REAL unconditionally, so
             * every shipped candidate takes the full window. */
            const int settle_ms = (cands[i].oracle == DP2P_PUNCH_REAL)
                                      ? (int)RACE_PUNCH_SETTLE_MS
                                      : 0;
            /* The budget already stops the pump, so recognising it here
             * costs no wire traffic: it only moves the instant the leg
             * is DECLARED done sending, which is what the settle window
             * is measured from. Without this the budget would cut the
             * leg mid-send with no settle at all whenever
             * `race_budget_ms < punch_leg_ms`, and the band would be
             * open in exactly that configuration. Never negative. */
            int send_window_ms = cfg->punch_leg_ms;
            {
                const int to_budget = cfg->race_budget_ms -
                                      (int)(cands[i].armed_ms - t0);
                if (to_budget < send_window_ms) {
                    send_window_ms = (to_budget > 0) ? to_budget : 0;
                }
            }
            if ((int)(now - cands[i].armed_ms) < send_window_ms) {
                continue;
            }
            if (!cands[i].send_expired) {
                cands[i].send_expired = true;
                cands[i].send_end_ms = (now != 0) ? now : 1u;
                SDL_Log("[direct_p2p] S6 race: candidate %s:%u stopped punching after "
                        "%d ms%s — listening %d ms more",
                        cands[i].ip, (unsigned)cands[i].port, send_window_ms,
                        (send_window_ms < cfg->punch_leg_ms)
                            ? " (the race budget, not the punch leg, ended the send)"
                            : "",
                        settle_ms);
            }
            if ((int)(now - cands[i].send_end_ms) < settle_ms) {
                continue;
            }
            SDL_Log("[direct_p2p] S6 race: candidate %s:%u timed out after %d ms",
                    cands[i].ip, (unsigned)cands[i].port, send_window_ms);
            race_finish_punch(&cands[i], now);
        }
        if (signal_active && (int)(now - t0) >= cfg->signal_budget_ms) {
            signal_active = false;
            signal_end_ms = now;
        }

        /* --- 5) pump the legs ---------------------------------------- */
        for (int i = 0; i < RACE_PUNCH_LEGS; i++) {
            if (!cands[i].armed || cands[i].finished) continue;
            /* Past its send window a candidate is receive-only — UNLESS
             * it has just been confirmed by a late peer, in which case it
             * owes that peer the confirmation tail and pumping resumes
             * (Stun_PunchOffer cleared `sent_any`, so the first tail
             * datagram leaves on this very iteration). */
            if (cands[i].send_expired && !race_punch_confirmed(&cands[i])) continue;
            if (cands[i].oracle == DP2P_PUNCH_REAL) {
                Stun_PunchPump(&cands[i].leg, cfg->sock, now);
            } else if (cands[i].oracle == DP2P_PUNCH_CONFIRM) {
                cands[i].oracle_confirmed = true;
            }
        }
        if (signal_active &&
            (!signal_sent_any || (now - signal_last_send) >= 500u)) {
            (void)RENDEZVOUS_SEND(cfg->sock, cfg->signal_addr, cfg->signal_port,
                                  register_pkt, sizeof(register_pkt));
            signal_last_send = now;
            signal_sent_any = true;
        }

        /* --- 6) ONE shared receive path ------------------------------ */
        NET_Datagram* dgram = NULL;
        bool got_dgram = false;
        if (NET_ReceiveDatagram(cfg->sock, &dgram) && dgram != NULL) {
            got_dgram = true;
            char src_ip[64];
            SDL_strlcpy(src_ip, NET_GetAddressString(dgram->addr), sizeof(src_ip));
            const uint16_t src_port = dgram->port;
            const int ft = Rendezvous_HasMagic(dgram->buf, dgram->buflen)
                               ? Rendezvous_FrameType(dgram->buf, dgram->buflen)
                               : -1;

            if (ft == REND_FRAME_CHALLENGE) {
                /* S4c: answer inline — one RTT to bind instead of waiting
                 * out a resend cadence — and carry the cookie on every
                 * later REGISTER. A challenge is ALSO
                 * liveness evidence: the server answered us, so a budget
                 * expiry with challenges but no DELIVERs is an auth
                 * problem, not a dead server. */
                if (Rendezvous_ParseChallenge(dgram->buf, dgram->buflen,
                                              cfg->session_key, cookie)) {
                    /* Task #105: record the RE-challenge before we
                     * overwrite have_cookie. Being challenged while we
                     * already hold a cookie is the only on-wire proof that
                     * the echo failed to verify — without it, the normal
                     * first challenge of a perfectly healthy session was
                     * being reported to the user as "Matchmaking auth
                     * failed. Update the game." */
                    if (have_cookie) {
                        out->cookie_rechallenged = true;
                    }
                    have_cookie = true;
                    out->have_cookie = true;
                    memcpy(out->cookie, cookie, sizeof(cookie));
                    out->challenge_any = true;
                    if (out->challenge_n < UINT16_MAX) out->challenge_n++; /* #36 */
                    if (signal_active &&
                        Rendezvous_BuildRegister(cfg->my_public_port, cfg->session_key,
                                                 cookie, register_pkt)) {
                        (void)RENDEZVOUS_SEND(cfg->sock, cfg->signal_addr,
                                              cfg->signal_port, register_pkt,
                                              sizeof(register_pkt));
                        signal_last_send = now;
                        signal_sent_any = true;
                    }
                    SDL_Log("[direct_p2p] S6 race answered a rendezvous CHALLENGE");
                }
            } else if (ft == REND_FRAME_DELIVER) {
                char parsed_ip[64] = { 0 };
                uint16_t parsed_port = 0;
                /* S3 tri-state parse: a zero-sentinel DELIVER is MEANINGFUL
                 * evidence — it proves the server is alive (it answers
                 * every REGISTER), which separates "server down" from
                 * "host offline" when the budget expires. */
                const RendezvousDeliverResult dr = Rendezvous_ParseDeliverEx(
                    dgram->buf, dgram->buflen, cfg->session_key, parsed_ip, &parsed_port);
                if (dr != REND_DELIVER_MALFORMED) {
                    out->deliver_any = true;
                    /* #36: count them and measure the cadence. The gap is
                     * only meaningful BETWEEN two DELIVERs, so the first
                     * one only seeds the clock. `now` is monotonic within
                     * a race and `last_deliver_ms` is stamped from it, so
                     * the subtraction cannot underflow. */
                    if (out->deliver_n < UINT16_MAX) out->deliver_n++;
                    if (last_deliver_ms != 0) {
                        const uint32_t gap = now - last_deliver_ms;
                        if (gap > out->deliver_gap_max_ms) {
                            out->deliver_gap_max_ms = gap;
                        }
                    }
                    /* 0 is the "unset" sentinel; a DELIVER landing on tick
                     * 0 of the race would otherwise re-seed forever. */
                    last_deliver_ms = (now != 0) ? now : 1u;
                }
                if (dr == REND_DELIVER_PEER && parsed_ip[0] != '\0' && parsed_port != 0) {
                    /* S3-review HIGH-1 self-DELIVER gate: a DELIVER whose
                     * endpoint IP equals OUR OWN public IP is not the host;
                     * it is this client's own registration echoed back
                     * after the S2 fresh-socket retry re-REGISTERed from a
                     * new source port. A legitimate same-IP host is
                     * impossible: the hairpin bypass fails any same-IP room
                     * code before this leg ever REGISTERs. Treat it as
                     * EMPTY evidence and keep polling. */
                    if (cfg->my_public_ip != NULL && cfg->my_public_ip[0] != '\0' &&
                        direct_p2p_ip_eq_normalized(parsed_ip, cfg->my_public_ip)) {
                        SDL_Log("[direct_p2p] S6 race: DELIVER carries our own public IP "
                                "(%s:%u) — our stale registration, not the host",
                                parsed_ip, (unsigned)parsed_port);
                    } else {
                        if (!out->deliver_real) {
                            SDL_Log("[direct_p2p] S6 race: DELIVER peer=%s:%u at t+%u ms",
                                    parsed_ip, (unsigned)parsed_port,
                                    (unsigned)(now - t0));
                        }
                        out->deliver_real = true;
                        if (signal_end_ms == 0) {
                            signal_end_ms = now;
                        }
                        /* The DELIVER endpoint becomes a punch candidate
                         * IMMEDIATELY. Pre-S6 this punch could not start
                         * until the direct punch had burned its full
                         * window AND the signalling loop had broken out —
                         * the single biggest serial cost in the cascade. */
                        (void)race_arm_punch(cands, RACE_PUNCH_LEGS, 1, cfg,
                                             parsed_ip, parsed_port, now);
                        /* We have what we came for; stop re-REGISTERing. */
                        signal_active = false;
                    }
                }
            } else if (ft == REND_FRAME_NACK) {
                /* TASK #122 — THE CLIENT HALF.
                 *
                 * Before this arm existed the server's typed refusal
                 * matched no branch and fell straight through to
                 * NET_DestroyDatagram below, so SESSION_FULL, RATE_KEY,
                 * RATE_IP, KEY_QUOTA, TABLE_FULL, BAD_LENGTH, BAD_TYPE
                 * and RATE_PREGATE all still collapsed into
                 * CONNECT_FAIL_RENDEZVOUS_DOWN — the pre-#122 behaviour,
                 * with the wire change deployed and unread.
                 *
                 * TWO GATES, and both are load-bearing because a NACK
                 * DECIDES A VERDICT. This is the badver_n argument (F5)
                 * one step further along: badver_n only counts, while a
                 * reason byte names a cause and puts a sentence on the
                 * user's screen, so a NACK an attacker can choose is a
                 * DIAGNOSIS an attacker can choose — strictly worse than
                 * the silence it replaced (H-1).
                 *
                 *   1. Rendezvous_ParseNack requires the frame to echo
                 *      OUR session key, exactly as ParseChallenge and
                 *      ParseDeliverEx do. The key embeds the S4b nonce
                 *      and is derived, never transmitted by us in the
                 *      clear beyond the REGISTER itself.
                 *   2. SOURCE GATE, the same normalizing comparison the
                 *      self-DELIVER and skew arms use: the datagram must
                 *      come from the very endpoint this signal leg is
                 *      REGISTERing to. Gate 1 alone stops an OFF-PATH
                 *      sender; it does not stop one that has SEEN a
                 *      REGISTER, since the key sits in plaintext at
                 *      bytes [8..24] of every one we send. Gate 2 raises
                 *      that to "and spoof the server's address".
                 *
                 * A frame that fails either gate falls out of this arm
                 * uncounted — the same silent drop as before, which is
                 * the correct failure mode for evidence we cannot trust.
                 *
                 * The reason byte is stored RAW. A server newer than us
                 * may name a reason we have no word for, and
                 * Rendezvous_NackReasonText renders those as "unknown"
                 * while ConnectFail_ClassifyNackReason maps them to the
                 * honest catch-all; coercing an unknown number onto the
                 * nearest name we do have is the one thing that must not
                 * happen here. */
                uint8_t reason = REND_NACK_NONE;
                if (src_port == cfg->signal_port &&
                    signal_ip_str[0] != '\0' &&
                    direct_p2p_ip_eq_normalized(src_ip, signal_ip_str) &&
                    Rendezvous_ParseNack(dgram->buf, dgram->buflen,
                                         cfg->session_key, &reason)) {
                    out->nack_any = true;
                    out->nack_reason = reason;
                    if (out->nack_n < UINT16_MAX) out->nack_n++;
                    /* A NACK is proof of life as strong as a CHALLENGE:
                     * the server parsed enough of our frame to echo our
                     * own session key back at us. It is NOT a DELIVER,
                     * so nothing here touches deliver_any/deliver_real —
                     * those two still mean exactly what they meant, and
                     * the taxonomy reads the refusal from nack_any
                     * instead. */
                    if (logged_nack_reason != reason) {
                        logged_nack_reason = reason;
                        SDL_Log("[direct_p2p] S6 race: rendezvous NACK from %s:%u "
                                "reason=%u (%s) at t+%u ms",
                                src_ip, (unsigned)src_port, (unsigned)reason,
                                Rendezvous_NackReasonText(reason),
                                (unsigned)(now - t0));
                    }
                }
            } else if (ft == 0 && dgram->buflen >= 6 &&
                       dgram->buf[4] != (uint8_t)Rendezvous_WireVersion() &&
                       src_port == cfg->signal_port &&
                       signal_ip_str[0] != '\0' &&
                       direct_p2p_ip_eq_normalized(src_ip, signal_ip_str)) {
                /* #36 — PROTOCOL SKEW, previously a silent drop.
                 *
                 * SOURCE GATE (F5), and it is not decoration. badver_n is
                 * LOAD-BEARING: ConnectFail_Attribute turns badver_n > 0
                 * plus RENDEZVOUS_DOWN into a DEFINITE "version-skew"
                 * verdict that tells the user to update the game. This
                 * socket is unconnected and accepts datagrams from
                 * anywhere, so without the gate any off-path host that
                 * learns our port could spray four '3SXR' bytes and a
                 * junk version byte and manufacture that verdict. The
                 * frame is only counted when it came from the very
                 * endpoint the signal leg is REGISTERing to — the only
                 * source whose protocol version we are actually making a
                 * claim about. Comparison is the same normalizing one the
                 * self-DELIVER gate above uses, so a formatting
                 * difference between the resolved address string and the
                 * datagram's source string cannot cause a false miss.
                 * Frames from any other source fall through uncounted,
                 * exactly as they did before #36.
                 *
                 * `ft` is computed above as
                 *   Rendezvous_HasMagic(...) ? Rendezvous_FrameType(...) : -1
                 * so ft == 0 means the datagram DID carry the '3SXR' magic
                 * but Rendezvous_FrameType refused it. FrameType returns 0
                 * for THREE reasons (rendezvous.c:340-350): len < 6, wrong
                 * magic, and pkt[4] != REND_VERSION. The magic is already
                 * proven by Rendezvous_HasMagic — but HasMagic only needs
                 * len >= 4 (rendezvous.c:331-337), so a 4- or 5-byte runt
                 * reaches here too. Counting a runt as a version skew would
                 * be an attribution error of exactly the kind this whole
                 * change exists to prevent, so the version byte is tested
                 * EXPLICITLY, against the same constant FrameType uses.
                 * Runts and well-formed v2 frames carrying an unknown type
                 * byte fall through to the arms below, unchanged.
                 *
                 * DIRECTIONALITY, and it is the important part: this counter
                 * detects a server NEWER than us, and in THAT direction the
                 * verdict is DEFINITE — ConnectFail_Attribute turns
                 * (RENDEZVOUS_DOWN && badver_n > 0) into
                 * CONNECT_ATTRIB_VERSION_SKEW, no hedging, because the
                 * server demonstrably answered and we demonstrably could
                 * not parse the answer.
                 *
                 * It stays 0 in the #87 case — the deployed April v1 server
                 * DROPS a version-mismatched frame with no reply at all, so
                 * a v2 client sees zero frames, not a v1 frame. THAT
                 * reverse skew is genuinely indistinguishable from an
                 * unreachable server on the wire, and only that one is
                 * reported as CONNECT_ATTRIB_AMBIG_VERSION. */
                if (out->badver_n < UINT16_MAX) out->badver_n++;
                if (!logged_badver) {
                    logged_badver = true;
                    SDL_Log("[direct_p2p] S6 race: '3SXR' frame with an unsupported "
                            "version byte from %s:%u — protocol skew (we speak v%d)",
                            src_ip, (unsigned)src_port, Rendezvous_WireVersion());
                }
            } else if (ft < 0) {
                /* Not a '3SXR' frame. STUN stragglers from a slower
                 * discovery server are dropped (S1); everything else is
                 * offered to the punch legs, which accept ONLY the exact
                 * authenticated payload from a candidate's IP. */
                if (!Stun_IsBindingResponse(dgram->buf, dgram->buflen)) {
                    for (int i = 0; i < RACE_PUNCH_LEGS; i++) {
                        if (!cands[i].armed || cands[i].finished) continue;
                        if (cands[i].oracle != DP2P_PUNCH_REAL) continue;
                        if (Stun_PunchOffer(&cands[i].leg, cfg->stun, dgram->buf,
                                            dgram->buflen, src_ip, src_port, now)) {
                            break;
                        }
                    }
                }
            }
            NET_DestroyDatagram(dgram);
        }

        /* --- 8) termination ------------------------------------------ */
        {
            bool punch_live = false;
            for (int i = 0; i < RACE_PUNCH_LEGS; i++) {
                if (cands[i].armed && !cands[i].finished) punch_live = true;
            }
            if (!punch_live && !signal_active) {
                break;
            }
        }
        /* Overall deadline. See race_budget_expired() for the wrap-safety
         * argument and for the H-1 confirmation-tail exemption: a leg that
         * has already confirmed is not thrown away just because the budget
         * edge landed inside its 600 ms tail. */
        {
            bool tail_outstanding = false;
            for (int i = 0; i < RACE_PUNCH_LEGS; i++) {
                if (race_punch_confirmed(&cands[i]) &&
                    !race_punch_settled(&cands[i], now)) {
                    tail_outstanding = true;
                }
            }
            /* The settle window gets the SAME one-tail extension, for the
             * same reason: a leg still inside it has been PROMISED to the
             * peer as listening time, and truncating that promise is what
             * reopens the split-brain band (section 2). Because
             * RACE_PUNCH_SETTLE_MS == STUN_PUNCH_CONFIRM_MS that extends
             * the deadline to exactly one tail past the budget, and
             * race_budget_expired() itself is untouched: only the flag
             * handed to it widens. The H-1 LOG below stays gated on
             * `tail_outstanding` alone, so it keeps meaning "a CONFIRMED
             * punch was rescued" and nothing else. */
            bool settle_outstanding = false;
            for (int i = 0; i < RACE_PUNCH_LEGS; i++) {
                if (cands[i].armed && !cands[i].finished &&
                    cands[i].send_expired && !race_punch_confirmed(&cands[i])) {
                    settle_outstanding = true;
                }
            }
            bool expired = race_budget_expired(now, t0, cfg->race_budget_ms,
                                               tail_outstanding || settle_outstanding);
            /*
             * SECOND TAIL — and it is H-1's own argument, one tail later.
             *
             * A leg listening through its settle window can be confirmed
             * as late as `send_end + RACE_PUNCH_SETTLE_MS`, and `send_end`
             * can itself be the budget (section 2), so a confirmation can
             * legitimately land at `budget + one tail`. It then owes its
             * peer a FULL tail from THERE. Cutting it at `budget + one
             * tail` discards a punch that provably reached us and reports
             * a NAT failure — exactly the defect H-1 exists to prevent —
             * and, worse, the peer that sent that punch has already been
             * confirmed by our partial tail, so the two sides disagree.
             *
             * MEASURED, on the two-peer rig with the budget squeezed to
             * 2 700 ms against a 2 500 ms send window (owd 150, skew
             * 2 650): the early peer confirmed at t+2 810 ms, needed to
             * settle at t+3 410 ms, and was cut at the t+3 300 ms cap —
             * `A=PUNCHED B=EXHAUSTED`. The same probe at
             * `S6_SPLIT_BUDGET_MS=2000` split for the same reason.
             *
             * So a CONFIRMED leg gets one more tail, and no more: the
             * loop stays hard-bounded, now at
             * `race_budget_ms + 2 * STUN_PUNCH_CONFIRM_MS`. That is
             * sufficient AND tight — the latest reachable confirmation is
             * `budget + one tail` and a tail is one tail long, so nothing
             * needs a third. It costs nothing on the failure path, which
             * has no confirmed leg by definition.
             */
            if (expired && tail_outstanding &&
                (int)(now - t0) < RACE_HARD_CAP_MS(cfg->race_budget_ms)) {
                expired = false;
            }
            if (expired) {
                SDL_Log("[direct_p2p] S6 race: overall budget (%d ms) expired%s",
                        cfg->race_budget_ms,
                        tail_outstanding ? " (confirmation tail also exhausted)" : "");
                break;
            }
            if (tail_outstanding && (int)(now - t0) >= cfg->race_budget_ms) {
                /* Logged ONCE per race: this is the H-1 rescue happening,
                 * and a field log that shows it is how we know the window
                 * is real on the wire rather than only in the harness. */
                if (!logged_tail_hold) {
                    logged_tail_hold = true;
                    /* The cap printed is the one that actually applies to a
                     * CONFIRMED leg: the settle window can carry a
                     * confirmation as late as budget + one tail, and that
                     * confirmation owes a full tail from there.
                     *
                     * #36: through the MT sink (which tees to SDL_Log) so
                     * the rescue is visible in the tester's FILE, not only
                     * on a console nobody has. Already once per race.
                     *
                     * #76/#103: the cap is RACE_HARD_CAP_MS, not a second
                     * hand-typed copy of `budget + 2 * confirm`. #36 wrote
                     * the expression out because the macro did not exist on
                     * its base; it does now, and the deadline check 20 lines
                     * up already uses it. Two copies of one bound is exactly
                     * what #76 removed — keep the single source of truth. */
                    char hold_line[256];
                    SDL_snprintf(hold_line, sizeof(hold_line),
                                 "[direct_p2p] S6 race: budget expired with a CONFIRMED "
                                 "punch still in its %d ms tail — holding the race open "
                                 "(hard cap %d ms)",
                                 STUN_PUNCH_CONFIRM_MS,
                                 RACE_HARD_CAP_MS(cfg->race_budget_ms));
                    Netplay_LogConnectEventMT(hold_line);
                }
            }
        }

        if (!got_dgram) {
            SDL_Delay(RACE_POLL_MS);
        }
    }

done:;
    const uint32_t t_end = SDL_GetTicks();
    for (int i = 0; i < RACE_PUNCH_LEGS; i++) {
        race_finish_punch(&cands[i], t_end);
    }
    out->t_punch_ms = cands[0].alive_ms;
    out->t_bilateral_ms = cands[1].alive_ms;
    /* A leg that never ran reports 0, not "the whole race": `signal=` in a
     * report line has always meant "time spent in the signalling phase",
     * and the host has no signalling leg at all. */
    out->t_signal_ms = !cfg->signal_leg ? 0u
                       : (signal_end_ms != 0) ? (signal_end_ms - t0)
                                              : (t_end - t0);
    out->t_race_ms = t_end - t0;
    SDL_SetAtomicInt(&s_last_race_ms, (int)out->t_race_ms);
    if (have_cookie) {
        out->have_cookie = true;
        memcpy(out->cookie, cookie, sizeof(cookie));
    }
    /* The outcome is logged by NAME, not by ordinal: RACE_RELAYED used to
     * sit at 1 and its removal shifts CANCELLED/EXHAUSTED down, which would
     * have made a bare `outcome=%d` mean two different things across
     * builds. */
    static const char* const k_outcome_name[] = {
        "PUNCHED", "CANCELLED", "EXHAUSTED"
    };
    /* #36: this is THE line a field report is triaged from, and until now
     * it only reached SDL_Log — i.e. never the per-session file a tester
     * sends us (netplay.c: only Netplay_LogConnectEvent* writes there).
     * Route it through the MT sink, which tees to SDL_Log itself, so this
     * is a re-route and not a second copy. Once per race: bounded. */
    char line[320];
    SDL_snprintf(line, sizeof(line),
                 "[direct_p2p] S6 race done in %u ms: outcome=%s punch=%u bilateral=%u "
                 "signal=%u deliver=any:%d,real:%d "
                 "confirm=%d@%ums badver=%u dgap=%u nack=%u:%u(%s)",
                 (unsigned)out->t_race_ms,
                 ((int)out->outcome >= 0 &&
                  (size_t)out->outcome < SDL_arraysize(k_outcome_name))
                     ? k_outcome_name[(int)out->outcome] : "?",
                 out->t_punch_ms, out->t_bilateral_ms, out->t_signal_ms,
                 (int)out->deliver_any, (int)out->deliver_real,
                 (int)out->confirm_seen, (unsigned)out->confirm_ms,
                 (unsigned)out->badver_n, (unsigned)out->deliver_gap_max_ms,
                 (unsigned)out->nack_n, (unsigned)out->nack_reason,
                 out->nack_any ? Rendezvous_NackReasonText(out->nack_reason)
                               : "none");
    Netplay_LogConnectEventMT(line);
}

#ifdef NETPLAY_TEST_HOOKS
static NET_Address* resolve_with_short_poll(const char* host);

/* S6-review H-3 / H-2: run ONE race against caller-supplied endpoints.
 * See the contract in direct_p2p.h — this exists so a harness can stand
 * up TWO concurrent peers in one process (the split-brain property is
 * not observable from one side) and so at least one harness leg punches
 * for real instead of through the oracle. */
void DirectP2P_TestHook_RunRace(const DirectP2PRaceProbeCfg* pcfg,
                                DirectP2PRaceProbeOut* pout) {
    if (pout == NULL) {
        return;
    }
    memset(pout, 0, sizeof(*pout));
    /* The probe is not part of a session, and s_cancel is sticky: a
     * DirectP2P_Cancel from an EARLIER test in the same process leaves it
     * raised (only Init/BeginHost/BeginJoin clear it), which would make
     * every probe race return CANCELLED on its first iteration. */
    SDL_SetAtomicInt(&s_cancel, 0);
    if (pcfg == NULL || pcfg->sock == NULL) {
        pout->outcome = DP2P_RACE_PROBE_EXHAUSTED;
        return;
    }

    NET_Address* signal_addr = NULL;
    if (pcfg->signal_ip != NULL && pcfg->signal_ip[0] != '\0') {
        signal_addr = resolve_with_short_poll(pcfg->signal_ip);
    }

    RaceCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sock = pcfg->sock;
    cfg.stun = NULL;
    cfg.punch_token = pcfg->punch_token;
    cfg.seed_ip = pcfg->seed_ip;
    cfg.seed_port = pcfg->seed_port;
    cfg.signal_addr = signal_addr;
    cfg.signal_port = pcfg->signal_port;
    cfg.session_key = pcfg->session_key;
    cfg.my_public_port = pcfg->my_public_port;
    cfg.cookie_in = NULL;
    cfg.signal_leg = pcfg->signal_leg && signal_addr != NULL;
    cfg.signal_budget_ms = pcfg->signal_budget_ms;
    cfg.my_public_ip = NULL;
    cfg.punch_leg_ms = pcfg->punch_leg_ms;
    cfg.race_budget_ms = pcfg->race_budget_ms;
    cfg.cancel = NULL;

    RaceResult res;
    p2p_race(&cfg, &res);

    if (signal_addr != NULL) {
        NET_UnrefAddress(signal_addr);
    }

    switch (res.outcome) {
    case RACE_PUNCHED:   pout->outcome = DP2P_RACE_PROBE_PUNCHED;   break;
    case RACE_CANCELLED: pout->outcome = DP2P_RACE_PROBE_CANCELLED; break;
    default:             pout->outcome = DP2P_RACE_PROBE_EXHAUSTED; break;
    }
    SDL_strlcpy(pout->peer_ip, res.peer_ip, sizeof(pout->peer_ip));
    pout->peer_port = res.peer_port;
    pout->t_race_ms = res.t_race_ms;
    /* #36 evidence — same fields the production copies carry into s_work. */
    pout->confirm_seen = res.confirm_seen;
    pout->confirm_ms = res.confirm_ms;
    pout->deliver_n = res.deliver_n;
    pout->challenge_n = res.challenge_n;
    pout->badver_n = res.badver_n;
    pout->deliver_gap_max_ms = res.deliver_gap_max_ms;
    pout->nack_any = res.nack_any;      /* #122 */
    pout->nack_n = res.nack_n;
    pout->nack_reason = res.nack_reason;
}

bool DirectP2P_TestHook_RaceBudgetExpired(uint32_t now, uint32_t t0,
                                          int budget_ms, bool tail_outstanding) {
    return race_budget_expired(now, t0, budget_ms, tail_outstanding);
}

#endif /* NETPLAY_TEST_HOOKS */

/* --- worker thread ----------------------------------------------------- */

/* --- UPnP worker thread (wraps Upnp_AddMapping with a wall-clock
 * timeout so a slow/broken router doesn't stall the orchestrator).
 *
 * miniupnpc's SSDP discover has its own ~2s cap but a misbehaving
 * router can still keep us blocked for longer than we want to wait on
 * a game's "Host Game" button. Budget the whole UPnP attempt at
 * ~3 seconds and fall through to STUN-only if the router takes too
 * long. On timeout we detach the side thread; if its Upnp_AddMapping
 * later succeeds the mapping will still be registered on the router
 * but we never publish it — it expires naturally when its lease runs
 * out (miniupnpc requests a 1-hour lease by default) or when the
 * router reboots. Accepting that small leak is cheaper than forcing
 * a clean kill on a blocking socket call. */

typedef struct UpnpJob {
    /* Inputs populated before SDL_CreateThread; never mutated after. */
    uint16_t internal_port;
    uint16_t preferred_external;
    /* S7: which backend to run. PORTMAP_BACKEND_NONE = "first probe":
     * try UPnP, then NAT-PMP/PCP. Anything else = "renew THIS backend",
     * which is what keeps a renewal from re-running discovery on the
     * wrong protocol. */
    PortMapBackend backend;
    /* S7: wall-clock budget handed to the NAT-PMP/PCP client. The UPnP
     * half has no such parameter (miniupnpc blocks), which is why
     * try_portmap still needs its own outer deadline+detach. */
    int natpmp_budget_ms;

    /* Output populated by the side thread on completion. Consumer reads
     * these two fields only if SDL_GetThreadState returned
     * SDL_THREAD_COMPLETE within the budget. */
    UpnpMapping result;
    bool ok;
} UpnpJob;

/* Human name for a backend, for logs and the failure report. */
static const char* portmap_backend_name(PortMapBackend b) {
    switch (b) {
    case PORTMAP_BACKEND_UPNP:   return "UPnP";
    case PORTMAP_BACKEND_NATPMP: return "NAT-PMP";
    case PORTMAP_BACKEND_PCP:    return "PCP";
    default:                     return "none";
    }
}

/* S7 teardown dispatch. Three backends now share UpnpMapping, and the
 * removal call is protocol-specific: Upnp_RemoveMapping drives miniupnpc
 * HTTP at an IGD, Natpmp_RemoveMapping sends a lifetime-0 datagram to
 * the gateway. Calling the wrong one is not a no-op on a router that
 * speaks both — it would delete an unrelated IGD entry that happens to
 * sit on the same external port. Every removal site goes through here. */
static void portmap_remove(UpnpMapping* mapping) {
    if (mapping == NULL || !mapping->active) {
        return;
    }
    switch (mapping->backend) {
    case PORTMAP_BACKEND_NATPMP:
    case PORTMAP_BACKEND_PCP:
        Natpmp_RemoveMapping(mapping);
        break;
    case PORTMAP_BACKEND_UPNP:
    case PORTMAP_BACKEND_NONE:
    default:
        Upnp_RemoveMapping(mapping);
        break;
    }
    mapping->active = false;
}

static int SDLCALL upnp_worker_fn(void* data) {
    UpnpJob* job = (UpnpJob*)data;
    memset(&job->result, 0, sizeof(job->result));
    job->ok = false;

    const uint16_t want_external =
        job->preferred_external != 0 ? job->preferred_external : job->internal_port;

    /* UPnP first — the existing ordering, and the backend the shipped
     * MiSTer routers have been validated against. */
    if ((job->backend == PORTMAP_BACKEND_NONE || job->backend == PORTMAP_BACKEND_UPNP) &&
        !Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP)) {
        job->ok = Upnp_AddMapping(&job->result, job->internal_port, want_external, "UDP");
    }

    /* S7 (plan §9): NAT-PMP/PCP is tried when IGD discovery fails.
     * DELIBERATELY NOT gated on DISABLE_UPNP — that kill switch exists
     * for one specific miniupnpc defect (the 2.2.1 upnpDiscover segfault
     * documented in try_portmap), and a user who sets it to dodge that
     * crash should still get a port mapping from a router that speaks
     * NAT-PMP. It has its own switch. */
    if (!job->ok &&
        (job->backend == PORTMAP_BACKEND_NONE || job->backend == PORTMAP_BACKEND_NATPMP ||
         job->backend == PORTMAP_BACKEND_PCP) &&
        !Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP)) {
        job->ok = Natpmp_AddMapping(&job->result, job->internal_port, want_external,
                                    job->backend,
                                    job->natpmp_budget_ms > 0 ? job->natpmp_budget_ms
                                                              : NATPMP_PROBE_BUDGET_MS);
    }
    return 0;
}

/* Wall-clock ceiling for the UPnP half of a probe. Was an inline 6000
 * before S7; named now because the outer deadline is the SUM of the two
 * backends' budgets. */
#define UPNP_PROBE_BUDGET_MS 6000u

/* try_portmap's OUTER deadline: the two backends run serially inside one
 * worker, so the ceiling is their sum. Named for task #76 so the host
 * cascade sums the same symbol the deadline below enforces. */
#define PORTMAP_PROBE_BUDGET_MS (UPNP_PROBE_BUDGET_MS + NATPMP_PROBE_BUDGET_MS)

/* --- #147: the straggler slot ------------------------------------------ */

/* TRUE when no overrun port-map worker is still executing — i.e. the
 * miniupnpc / natpmp process globals have a single user again. Reaps a
 * parked straggler that has since finished (SDL_WaitThread on a COMPLETE
 * thread returns without waiting, and the job heap block is reclaimed —
 * the old detach path leaked it by design). NON-BLOCKING in every case.
 *
 * Every upnp_worker_fn spawn decision must consult this; the removal
 * gates (join_portmap_reset / upnp_renew_join_and_discard verdicts) stay
 * as they are and cover the release side. */
static bool portmap_backends_quiescent(void) {
    if (s_portmap_straggler_thread == NULL) {
        return true;
    }
    if (SDL_GetThreadState(s_portmap_straggler_thread) != SDL_THREAD_COMPLETE) {
        return false; /* still inside miniupnpc / natpmp */
    }
    SDL_WaitThread(s_portmap_straggler_thread, NULL);
    s_portmap_straggler_thread = NULL;
    if (s_portmap_straggler_job != NULL) {
        SDL_free(s_portmap_straggler_job);
        s_portmap_straggler_job = NULL;
    }
    return true;
}

/* Park an overrun worker joinable. Replaces every SDL_DetachThread of an
 * upnp_worker_fn thread; `job` ownership moves to the slot (freed on
 * reap). The slot being occupied by a RUNNING straggler here is
 * unreachable by construction (see the slot's comment) — if it ever
 * happens the new thread is detached exactly as pre-#147, with a loud
 * log, rather than losing track of the old one. */
static void portmap_straggler_park(SDL_Thread* t, struct UpnpJob* job) {
    if (t == NULL) {
        return;
    }
    if (!portmap_backends_quiescent()) {
        SDL_Log("[direct_p2p] BUG: two overrun port-map workers exist at once — "
                "the #147 spawn gate should make this impossible; detaching the "
                "newer one (its job block leaks until it exits)");
        SDL_DetachThread(t);
        return;
    }
    s_portmap_straggler_thread = t;
    s_portmap_straggler_job = job;
}

/* Attempt a port mapping (UPnP, then NAT-PMP/PCP). Returns true on
 * success (s_upnp_mapping.active is set and s_upnp_mapping.backend says
 * which protocol won). Returns false on user-disabled, no backend
 * available, router rejection, or wall-clock timeout. See UpnpJob's
 * comment for the timeout rationale. */
static bool try_portmap(uint16_t internal_port, uint16_t preferred_external) {
    /* Known caveat: libminiupnpc 2.2.1 upnpDiscover() segfaults on MiSTer
     * (Buildroot 2021.02.4, glibc 2.31) when a Realtek 8821cu USB WiFi
     * adapter is connected alongside eth0 — validated 2026-04-22.
     * eth0-only is safe. Users running WiFi can set
     * CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP=1 to skip this path. */
    /* S7 review H-7.4: there used to be a second copy of the two kill
     * switches HERE, short-circuiting before the worker ever ran. That
     * made the worker's own gates untestable — with both switches set,
     * this early return kept the gateway silent no matter what the worker
     * did, so DELETING the worker's `disable-natpmp` check left the whole
     * suite green. The switches are now enforced in exactly ONE place,
     * upnp_worker_fn, which is the place a test can observe. The cost of
     * dropping the shortcut is one thread that starts and immediately
     * returns ok=false when both backends are off. */
    set_status("Preparing...");

    /* Wrap Upnp_AddMapping with a wall-clock budget. miniupnpc's internal
     * SSDP discovery alone can consume up to UPNP_DISCOVER_TIMEOUT_MS
     * (see upnp.c); GetValidIGD + GetExternalIP + AddPortMapping add
     * another 1–3 s on slow routers. 6 s is comfortable headroom without
     * making the Host Game click feel stuck. If it times out we detach
     * the thread (may register the mapping belatedly — harmless, expires
     * on its own lease) and fall back to STUN.
     *
     * Heap-allocated job struct: the detached thread must not reference
     * our stack, and the leak on timeout is bounded by host-game attempts
     * per session.
     *
     * S7: the worker may now run TWO backends serially, so the outer
     * deadline is UPNP_PROBE_BUDGET_MS + NATPMP_PROBE_BUDGET_MS. That
     * 11.25 s ceiling (6000 + 3 x 1750, the value the loop below actually
     * enforces via PORTMAP_PROBE_BUDGET_MS) is reached only when BOTH
     * protocols are dead silent;
     * the case S7 exists for — a NAT-PMP router with no IGD — costs
     * ~2 s (miniupnpc's own UPNP_DISCOVER_TIMEOUT_MS) plus one LAN
     * round-trip, because a router that speaks NAT-PMP answers
     * immediately. */
    UpnpJob* job = (UpnpJob*)SDL_calloc(1, sizeof(UpnpJob));
    if (job == NULL) {
        return false;
    }
    job->internal_port = internal_port;
    job->preferred_external = preferred_external;
    job->backend = PORTMAP_BACKEND_NONE; /* first probe: try both */
    job->natpmp_budget_ms = NATPMP_PROBE_BUDGET_MS;

    SDL_Thread* t = SDL_CreateThread(upnp_worker_fn, "PortMapProbe", job);
    if (t == NULL) {
        SDL_free(job);
        return false;
    }

    const uint64_t deadline_ms = SDL_GetTicks() + (uint64_t)PORTMAP_PROBE_BUDGET_MS;
    while (SDL_GetThreadState(t) != SDL_THREAD_COMPLETE) {
        if (SDL_GetTicks() >= deadline_ms) {
            SDL_Log("[direct_p2p] WARNING: port-mapping attempt timed out after %u ms; "
                    "falling back to STUN.",
                    (unsigned)PORTMAP_PROBE_BUDGET_MS);
            /* #147: park the overrun worker joinable instead of detaching
             * it — it is still inside miniupnpc / natpmp process globals,
             * and the spawn gates key off this slot. Any mapping it
             * eventually registers (UPnP or NAT-PMP/PCP — both request a
             * 3600 s lease) will expire on its own. */
            portmap_straggler_park(t, job);
            return false;
        }
        SDL_Delay(20);
    }

    /* Thread completed within budget. Join to reclaim resources. */
    SDL_WaitThread(t, NULL);
    bool ok = job->ok;
    if (ok) {
        s_upnp_mapping = job->result;
    }
    SDL_free(job);
    return ok;
}

/* --- #121: the joiner's non-blocking port-map probe --------------------- */

/* Spawn the probe for `internal_port` and RETURN IMMEDIATELY. Runs on the
 * join worker thread, from join_attempt, right after a successful STUN
 * discover — which is the first moment the socket's OS-assigned port is
 * known (StunResult.local_port, stun.h:16).
 *
 * Deliberately NOT try_portmap: try_portmap's contract is "block up to
 * PORTMAP_PROBE_BUDGET_MS and return a verdict", and blocking is the one
 * thing the joiner cannot afford (see s_join_portmap_thread). This shares
 * the WORKER — upnp_worker_fn, the same code the host's probe runs, both
 * backends, both kill switches enforced in that one place per S7 review
 * H-7.4 — and differs only in that nobody waits for it.
 *
 * Every gate here is a reason NOT to spend a thread:
 *   - already spawned: one probe per join session, full stop. This is the
 *     structural half of #96's "pay it at most once"; the latch below is
 *     the other half and covers the case where the verdict came back.
 *   - a live mapping on this port: nothing to ask for.
 *   - #96's failure latch already says no for this port.
 *   - no port_disagreement: STUN saw a consistent mapped port across
 *     servers, so this joiner is not symmetric and a mapping would buy
 *     nothing the existing race does not already win. Cells other than
 *     the symmetric row never reach this code at all, which is what keeps
 *     #121 from being able to trade one quadrant for another. */
static void join_portmap_spawn(uint16_t internal_port) {
    if (s_join_portmap_spawned) return;
    if (internal_port == 0) {
        /* #150: Stun_Discover could not learn the OS-bound local port
         * (getsockname failed on every handle) and now reports 0 instead
         * of fabricating public_port. Asking the gateway to map a port
         * the socket is not bound to could never rescue anything, so the
         * skip is correct — but on the symmetric NAT this rescue exists
         * for, it must be SAID, not silent. Not latched: attempt 2
         * re-discovers on a fresh socket and may learn a real port. */
        if (s_work.stun.port_disagreement) {
            Netplay_LogConnectEventMT(
                "[netplay-connect] PORTMAP joiner rescue skipped: local port "
                "unknown (getsockname failed) — refusing to request a mapping "
                "for a fabricated port; this attempt advertises the "
                "STUN-observed endpoint");
        }
        return;
    }
    if (s_upnp_mapping.active && s_upnp_mapping.internal_port == internal_port) return;
    if (s_portmap_failed_port == internal_port) return;
    if (!s_work.stun.port_disagreement) return;
    if (!portmap_backends_quiescent()) {
        /* #147: an overrun worker from an earlier session is still inside
         * the backend libraries' process globals — same gate as
         * host_thread_fn's, same reason. Not latched, and
         * s_join_portmap_spawned stays false: attempt 2 re-checks a slot
         * that reaps itself when the straggler exits. */
        Netplay_LogConnectEventMT(
            "[netplay-connect] PORTMAP joiner rescue skipped: a previous port-map "
            "attempt is still running inside the backend libraries (#147 spawn gate)");
        return;
    }

    struct UpnpJob* job = (struct UpnpJob*)SDL_calloc(1, sizeof(struct UpnpJob));
    if (job == NULL) return;
    job->internal_port = internal_port;
    job->preferred_external = internal_port;
    job->backend = PORTMAP_BACKEND_NONE; /* first probe: try both */
    job->natpmp_budget_ms = NATPMP_PROBE_BUDGET_MS;

    SDL_Thread* t = SDL_CreateThread(upnp_worker_fn, "JoinPortMapProbe", job);
    if (t == NULL) {
        SDL_free(job);
        return;
    }
    s_join_portmap_thread = t;
    s_join_portmap_job = job;
    s_join_portmap_spawned = true;
    s_join_portmap_port = internal_port;
    SDL_Log("[direct_p2p] joiner: STUN reported per-destination port translation "
            "(symmetric NAT) — asking the LAN for a mapping on internal port %u in the "
            "background; the race is NOT waiting for it",
            (unsigned)internal_port);
}

/* Zero-wait collection. Returns the internal port of a LIVE mapping, or 0.
 *
 * This function must never block, and the two branches below are the whole
 * reason it cannot: a COMPLETE thread is joined (SDL_WaitThread on an
 * already-complete thread returns without waiting), and anything else is
 * DETACHED and forgotten — try_portmap's own timeout disposition, with
 * the same accepted costs it documents: the job struct leaks until the
 * thread exits, and a mapping the gateway grants after we stopped caring
 * expires on its own 3600 s lease.
 *
 * A still-running probe does NOT latch a failure. #96's latch means "the
 * LAN was asked and said no"; "we did not wait for the answer" is a
 * different statement and recording it as the first one would suppress a
 * later legitimate probe on evidence nobody gathered. */
static uint16_t join_portmap_collect(void) {
    /* Only ever report a mapping THIS join session asked for.
     *
     * Without this gate the function would hand back any live
     * s_upnp_mapping, and s_upnp_mapping deliberately OUTLIVES a hosting
     * session — the router-side lease does, so #96 keeps the success
     * latch wider than the failure latch. A user who hosts (mapping on
     * port 7000), backs out, and then JOINS would therefore have had
     * attempt 2 pin port 7000 and advertise the host mapping's external
     * port, with no probe having run and port_disagreement never
     * consulted. That mapping is live and the path would probably even
     * work, which is what makes it dangerous: it is a silent behaviour
     * change on the retry of a joiner #121 has no business touching, and
     * the netns matrix could never catch it because every cell there is
     * a fresh process that has never hosted. */
    if (!s_join_portmap_spawned) return 0;

    if (s_join_portmap_thread == NULL) {
        /* Already collected or already detached. Report a mapping only if
         * it is the one THIS session probed for — `spawned` alone would
         * also be true after the detach branch below, and at a bumped
         * JOIN_MAX_ATTEMPTS a third attempt would then pin whatever
         * s_upnp_mapping happened to hold. */
        return (s_upnp_mapping.active && s_join_portmap_port != 0 &&
                s_upnp_mapping.internal_port == s_join_portmap_port)
                   ? s_upnp_mapping.internal_port
                   : 0;
    }

    if (SDL_GetThreadState(s_join_portmap_thread) != SDL_THREAD_COMPLETE) {
        SDL_Log("[direct_p2p] joiner: port-map probe still in flight at the retry "
                "boundary — parking it rather than paying up to %u ms to find out; "
                "this attempt advertises the STUN-observed endpoint",
                (unsigned)PORTMAP_PROBE_BUDGET_MS);
        /* #147: parked joinable, not detached — see s_portmap_straggler_thread. */
        portmap_straggler_park(s_join_portmap_thread, s_join_portmap_job);
        s_join_portmap_thread = NULL;
        s_join_portmap_job = NULL; /* owned by the straggler slot now */
        return 0;
    }

    SDL_WaitThread(s_join_portmap_thread, NULL);
    s_join_portmap_thread = NULL;

    struct UpnpJob* job = s_join_portmap_job;
    s_join_portmap_job = NULL;
    if (job == NULL) return 0;

    uint16_t mapped_internal = 0;
    if (job->ok) {
        /* Exclude upnp_renew_tick for the duration of the struct copy.
         * See s_join_portmap_busy: the state gate DirectP2P_Tick applies
         * is not sufficient on the joiner's transition shape, and a
         * renewal that read a torn (internal, external, backend) triple
         * would renew a port nobody asked for and then overwrite this
         * mapping with its own result. */
        SDL_SetAtomicInt(&s_join_portmap_busy, 1);
        s_upnp_mapping = job->result;
        SDL_SetAtomicInt(&s_join_portmap_busy, 0);
        mapped_internal = s_upnp_mapping.internal_port;
        SDL_Log("[direct_p2p] joiner: %s mapping OK (external %s:%u -> internal %u) — "
                "this retry binds internal port %u and advertises the mapped port",
                portmap_backend_name(s_upnp_mapping.backend),
                s_upnp_mapping.external_ip[0] ? s_upnp_mapping.external_ip : "(unknown)",
                (unsigned)s_upnp_mapping.external_port,
                (unsigned)s_upnp_mapping.internal_port,
                (unsigned)s_upnp_mapping.internal_port);
    } else {
        /* #96's latch, reused verbatim: a real verdict, session-scoped. */
        s_portmap_failed_port = job->internal_port;
        SDL_Log("[direct_p2p] joiner: no port mapping (UPnP and NAT-PMP/PCP both "
                "unavailable or refused) — latched for this join session");
    }
    SDL_free(job);
    return mapped_internal;
}

/* Teardown/reset disposition. Same zero-wait rule: a probe still running
 * when the session ends is parked (#147: joinable, in the straggler
 * slot), never joined here. Callers that hold the main thread
 * (DirectP2P_Cancel, direct_p2p_on_teardown) have already joined the
 * join worker, so this cannot race a spawn.
 *
 * RETURNS TRUE WHEN THE PORT-MAP BACKENDS ARE QUIESCENT — i.e. no probe
 * was left running. This mirrors upnp_renew_join_and_discard's contract
 * exactly, and for the identical reason: a DETACHED probe is still inside
 * miniupnpc, which caches the IGD in process globals (upnp.c's
 * s_cached_urls / s_cached_data / s_cache_valid), and NAT-PMP likewise
 * keeps a PCP nonce and epoch in file statics. Calling portmap_remove
 * while one of those is in flight has two threads walking the same cached
 * controlURL, one of which may FreeUPNPUrls it.
 *
 * So a caller that is about to release the mapping MUST call this FIRST
 * and skip the release when it returns false. The mapping then expires on
 * its own 3600 s lease — the same trade the renewal path already makes,
 * and strictly better than a use-after-free on a process global. */
static bool join_portmap_reset(void) {
    bool quiescent = true;
    SDL_SetAtomicInt(&s_join_portmap_busy, 0);
    if (s_join_portmap_thread != NULL) {
        if (SDL_GetThreadState(s_join_portmap_thread) == SDL_THREAD_COMPLETE) {
            /* Finished on its own: joinable for free, and then the
             * backends really are idle. */
            SDL_WaitThread(s_join_portmap_thread, NULL);
            if (s_join_portmap_job != NULL) {
                SDL_free(s_join_portmap_job);
            }
        } else {
            /* #147: parked joinable so a later spawn can observe when the
             * backends actually go quiet, instead of detached-and-lost. */
            portmap_straggler_park(s_join_portmap_thread, s_join_portmap_job);
            quiescent = false; /* still inside miniupnpc / natpmp */
        }
        s_join_portmap_thread = NULL;
        s_join_portmap_job = NULL;
    }
    s_join_portmap_spawned = false;
    s_join_portmap_port = 0;
    return quiescent;
}

/* --- S1 host liveness: UPnP lease renewal ------------------------------ */

/* The UPnP mapping is requested with a 1-hour lease (UPNP_LEASE_DURATION
 * "3600" in upnp.c) and was never renewed — a host relying on UPnP lost
 * its mapping exactly one hour in, potentially MID-SESSION (the mapping
 * is what carries the peer's traffic to us). Renew at half-lease while
 * the mapping is in use, including into the active netplay session:
 * main.c now calls DirectP2P_Tick from the session branch too, and the
 * renewal runs on a short-lived side thread (miniupnpc HTTP to the
 * router), so the netplay hot path and the GekkoNet-owned socket are
 * never touched — UPnP renewal is router-side HTTP, not socket I/O.
 *
 * Threading: the renewal thread reuses upnp_worker_fn/UpnpJob. It only
 * runs in states where host_thread_fn has finished writing
 * s_upnp_mapping (see upnp_renew_tick's caller gate), and teardown /
 * Cancel join it BEFORE calling Upnp_RemoveMapping, so miniupnpc's
 * cached-IGD statics are never used concurrently. */
#define UPNP_RENEW_INTERVAL_MS (30u * 60u * 1000u) /* half the 3600 s lease */
#define UPNP_RENEW_RETRY_MS (5u * 60u * 1000u)     /* failed renew: retry sooner */

/* S7 floor on the renewal cadence. RFC 6887 §11.2.1: "renewal requests
 * MUST NOT be sent less than four seconds apart (a PCP client MUST NOT
 * send a flood of ever-closer-together requests in the last few seconds
 * before a mapping expires)." */
#define PORTMAP_RENEW_FLOOR_MS 4000u

static SDL_Thread* s_upnp_renew_thread = NULL;
static UpnpJob* s_upnp_renew_job = NULL;
static uint64_t s_upnp_next_renew_ms = 0;

/* S7 review H-5 / M-5.5: when the CURRENT lease runs out, in SDL ticks.
 * 0 = "no lease deadline known" (no mapping, or a UPnP mapping, whose
 * granted lifetime miniupnpc never reports).
 *
 * Without this, a mapping that could not be renewed stayed active=true
 * forever: the renewal retried on a fixed timer, the room code kept
 * advertising the mapped external port, and the drift re-encode kept
 * PINNING that port — long after the router had forgotten it. The user
 * saw a code that had worked and then silently stopped working. */
static uint64_t s_portmap_lease_expiry_ms = 0;

/* Defined with the drift handler (both are main-thread endpoint
 * re-publish paths); declared here because upnp_renew_tick calls it when
 * a lease is lost. */
static void host_commit_endpoint(const char* ip, uint16_t port, const char* why);

/* S7: half of the lease the gateway actually GRANTED, when it told us.
 *
 * UPNP_RENEW_INTERVAL_MS is half of upnp.c's requested 3600 s, and
 * miniupnpc's AddPortMapping reports no granted lease, so UPnP keeps
 * using it verbatim. NAT-PMP and PCP DO report one, and both allow the
 * gateway to shorten it — RFC 6886 §3.3 "The NAT gateway MAY reduce the
 * lifetime from what the client requested". A router that grants 120 s
 * against our 3600 s request would silently lose the mapping 28 minutes
 * before a fixed half-hour timer fired.
 *
 * Half-life is what both specs ask for: RFC 6886 §3.3 "The client SHOULD
 * begin trying to renew the mapping halfway to expiry time, like DHCP",
 * and RFC 6887 §11.2.1's recommended window is 1/2 to 5/8 of expiry, so
 * 1/2 sits inside it. */
static uint64_t portmap_renew_interval_for(uint32_t lifetime_s) {
    if (lifetime_s == 0) {
        return UPNP_RENEW_INTERVAL_MS;
    }
    uint64_t half = ((uint64_t)lifetime_s * 1000u) / 2u;
    if (half < PORTMAP_RENEW_FLOOR_MS) {
        half = PORTMAP_RENEW_FLOOR_MS;
    }
    if (half > UPNP_RENEW_INTERVAL_MS) {
        half = UPNP_RENEW_INTERVAL_MS;
    }
    return half;
}

static uint64_t portmap_renew_interval_ms(void) {
    return portmap_renew_interval_for(s_upnp_mapping.lifetime_s);
}

/* S7 review M-5.2: how long to wait after a FAILED renewal.
 *
 * This used to be a flat 5 minutes regardless of the lease. A gateway
 * that granted 120 s — which RFC 6886 §3.3 explicitly permits, "The NAT
 * gateway MAY reduce the lifetime from what the client requested" — put
 * the retry four and a half minutes AFTER the mapping had already
 * expired, so the one retry that mattered never happened. The retry now
 * scales with the lease: half the renewal interval, i.e. a quarter of
 * the lease, floored by RFC 6887 §11.2.1's four seconds and capped by
 * the old five minutes so a long UPnP lease behaves as before. */
static uint64_t portmap_renew_retry_for(uint32_t lifetime_s) {
    if (lifetime_s == 0) {
        return UPNP_RENEW_RETRY_MS; /* UPnP: no granted lease to scale to */
    }
    uint64_t retry = portmap_renew_interval_for(lifetime_s) / 2u;
    if (retry < PORTMAP_RENEW_FLOOR_MS) {
        retry = PORTMAP_RENEW_FLOOR_MS;
    }
    if (retry > UPNP_RENEW_RETRY_MS) {
        retry = UPNP_RENEW_RETRY_MS;
    }
    return retry;
}

/* Is the mapping something we may still ADVERTISE?
 *
 * `active` alone is not enough (review H-5 / M-5.5): it stays true after
 * a refused renewal, and the lease keeps running down underneath it. A
 * mapping past its known expiry forwards nothing, so the room code must
 * fall back to the STUN-observed endpoint rather than pin a dead port.
 * A zero expiry means "no lease deadline known" — the UPnP case — and
 * keeps the pre-S7 behaviour of trusting `active`. */
static bool portmap_mapping_usable(void) {
    if (!s_upnp_mapping.active) {
        return false;
    }
    if (s_portmap_lease_expiry_ms == 0) {
        return true;
    }
    return SDL_GetTicks() < s_portmap_lease_expiry_ms;
}

#ifdef NETPLAY_TEST_HOOKS
uint64_t DirectP2P_TestHook_PortmapRenewIntervalMs(uint32_t granted_lifetime_s) {
    return portmap_renew_interval_for(granted_lifetime_s);
}

uint64_t DirectP2P_TestHook_PortmapRenewRetryMs(uint32_t granted_lifetime_s) {
    return portmap_renew_retry_for(granted_lifetime_s);
}
#endif

/* Reap the renewal thread with a BOUNDED wait (review H3 — mirrors
 * try_upnp's deadline+detach pattern). An unbounded SDL_WaitThread here
 * was a main-thread hang: the worker runs Upnp_AddMapping = two blocking
 * router HTTP transactions, and if the router is dead/rebooting —
 * exactly when a renewal is likely to be in flight — a TCP connect can
 * block for the OS SYN timeout (~75 s macOS, ~2 min Linux), freezing
 * the game thread inside teardown/Cancel.
 *
 * Returns true when the thread was joined (or none was running), i.e.
 * miniupnpc is quiescent and the caller may safely make further
 * miniupnpc calls (Upnp_RemoveMapping). Returns false when the thread
 * had to be DETACHED: the caller must then SKIP Upnp_RemoveMapping —
 * (a) it would race the still-running worker on miniupnpc's cached-IGD
 * statics, and (b) it would block the main thread on the same dead
 * router anyway. The un-removed mapping expires on its own 1-hour
 * lease (same accepted leak as try_upnp's timeout path). On detach the
 * heap job struct is intentionally leaked to the worker — it is the
 * thread's sole owner from that point, so the detached thread can
 * never touch freed state (it references only the job and miniupnpc
 * statics, never s_work). */
#define UPNP_RENEW_JOIN_BUDGET_MS 2000
static bool upnp_renew_join_and_discard(void) {
    bool joined = true;
    if (s_upnp_renew_thread != NULL) {
        const uint64_t deadline_ms = SDL_GetTicks() + UPNP_RENEW_JOIN_BUDGET_MS;
        while (SDL_GetThreadState(s_upnp_renew_thread) != SDL_THREAD_COMPLETE &&
               SDL_GetTicks() < deadline_ms) {
            SDL_Delay(10);
        }
        if (SDL_GetThreadState(s_upnp_renew_thread) == SDL_THREAD_COMPLETE) {
            SDL_WaitThread(s_upnp_renew_thread, NULL);
        } else {
            SDL_Log("[direct_p2p] WARNING: UPnP renewal thread unresponsive after %u ms "
                    "(router down?) — parking it; router-side mapping removal skipped, "
                    "the lease expires on its own.",
                    (unsigned)UPNP_RENEW_JOIN_BUDGET_MS);
            /* #147: parked joinable (straggler slot), not detached — the
             * next session's spawn gate observes it. Job ownership moves
             * to the slot. */
            portmap_straggler_park(s_upnp_renew_thread, s_upnp_renew_job);
            s_upnp_renew_job = NULL;
            joined = false;
        }
        s_upnp_renew_thread = NULL;
    }
    if (s_upnp_renew_job != NULL) {
        SDL_free(s_upnp_renew_job);
        s_upnp_renew_job = NULL;
    }
    s_upnp_next_renew_ms = 0;
    /* Kept in lockstep with the renewal deadline: both are re-armed
     * together on the next first-sighting of an active mapping, so
     * leaving a stale expiry behind would mean judging a NEW mapping
     * against an OLD lease. */
    s_portmap_lease_expiry_ms = 0;
    return joined;
}

/* Main-thread, non-blocking. Called once per frame from DirectP2P_Tick
 * while the mapping is in use. Polls a completed renewal thread, or
 * spawns one when the half-lease deadline passes. */
static void upnp_renew_tick(void) {
    /* #121: the join worker owns s_upnp_mapping right now. Skipping a
     * frame costs nothing — this is a half-lease timer with hours of
     * slack — whereas reading the struct mid-copy does not. */
    if (SDL_GetAtomicInt(&s_join_portmap_busy) != 0) {
        return;
    }
    if (s_upnp_renew_thread != NULL) {
        if (SDL_GetThreadState(s_upnp_renew_thread) != SDL_THREAD_COMPLETE) {
            return; /* renewal in flight */
        }
        SDL_WaitThread(s_upnp_renew_thread, NULL);
        s_upnp_renew_thread = NULL;
        uint64_t now = SDL_GetTicks();
        if (s_upnp_renew_job != NULL && s_upnp_renew_job->ok) {
            s_upnp_mapping = s_upnp_renew_job->result;
            const uint64_t iv = portmap_renew_interval_ms();
            s_upnp_next_renew_ms = now + iv;
            /* A granted lifetime restarts the lease clock; UPnP reports
             * none, so it stays on the "no deadline known" path. */
            s_portmap_lease_expiry_ms =
                s_upnp_mapping.lifetime_s != 0
                    ? now + (uint64_t)s_upnp_mapping.lifetime_s * 1000u
                    : 0u;
            SDL_Log("[direct_p2p] %s lease renewed (external %u -> internal %u); next "
                    "renewal in %u s",
                    portmap_backend_name(s_upnp_mapping.backend),
                    s_upnp_mapping.external_port, s_upnp_mapping.internal_port,
                    (unsigned)(iv / 1000u));
        } else if (s_portmap_lease_expiry_ms != 0 && now >= s_portmap_lease_expiry_ms) {
            /* S7 review H-5 / M-5.5: THE LEASE IS GONE.
             *
             * A refused or unanswered renewal is not itself fatal — the
             * mapping lives until its lease runs out and the retry below
             * may still catch it. But once the lease HAS run out, the
             * router forwards nothing and continuing to advertise the
             * mapped port is worse than never having had one: the room
             * code the user is reading aloud points at a closed port.
             *
             * Drop it here rather than calling portmap_remove: there is
             * nothing left on the router to remove, and on PCP the delete
             * would be refused anyway (RFC 6887 §11.3 — the mapping the
             * nonce belonged to no longer exists). Clearing the struct
             * makes both the room-code encode and the drift re-encode
             * fall back to the STUN endpoint, which is the endpoint the
             * keepalive loop is actively maintaining.
             */
            SDL_Log("[direct_p2p] WARNING: %s mapping LOST — the lease expired and "
                    "renewal did not succeed; dropping it and advertising the "
                    "STUN-observed endpoint instead. Share the NEW code.",
                    portmap_backend_name(s_upnp_mapping.backend));
            memset(&s_upnp_mapping, 0, sizeof(s_upnp_mapping));
            s_portmap_lease_expiry_ms = 0;
            s_upnp_next_renew_ms = 0;
            /* Same STUN endpoint, different advertised PORT: with the
             * mapping gone the code must carry the STUN-observed port
             * again. host_commit_endpoint re-encodes, re-derives the
             * punch token and restarts the rendezvous loop. */
            host_commit_endpoint(s_work.stun.public_ip, s_work.stun.public_port,
                                 "after port-mapping loss");
        } else {
            const uint64_t retry = portmap_renew_retry_for(s_upnp_mapping.lifetime_s);
            s_upnp_next_renew_ms = now + retry;
            SDL_Log("[direct_p2p] WARNING: %s lease renewal failed; retrying in %u s "
                    "(mapping expires at the end of its current lease)",
                    portmap_backend_name(s_upnp_mapping.backend),
                    (unsigned)(retry / 1000u));
        }
        if (s_upnp_renew_job != NULL) {
            SDL_free(s_upnp_renew_job);
            s_upnp_renew_job = NULL;
        }
        return;
    }

    if (!s_upnp_mapping.active) {
        return;
    }
    uint64_t now = SDL_GetTicks();
    if (s_upnp_next_renew_ms == 0) {
        /* First sighting of an active mapping — arm the half-lease timer.
         * (Lazily armed here rather than in the worker so the deadline
         * bookkeeping stays main-thread-only.) */
        s_upnp_next_renew_ms = now + portmap_renew_interval_ms();
        /* S7: and the lease clock. Arming it from the first main-thread
         * sighting rather than from the grant instant puts the deadline a
         * frame or two LATE, which is the safe direction — it can only
         * ever make us keep a mapping marginally longer than the router
         * does, never drop a live one. */
        s_portmap_lease_expiry_ms =
            s_upnp_mapping.lifetime_s != 0
                ? now + (uint64_t)s_upnp_mapping.lifetime_s * 1000u
                : 0u;
        return;
    }
    /* RFC 6886 §3.6 (review M-5.1): the gateway's epoch went backwards,
     * so it rebooted and its mapping table is empty. §3.6 is a MUST —
     * "the client MUST immediately renew all its active port mapping
     * leases" — and §3.7 attaches a mandatory 0-to-5-second random delay
     * before the first request so a whole LAN does not stampede a router
     * that has just finished booting. Pull the renewal deadline in to
     * now + that jitter; the existing spawn path below does the rest,
     * and because the renewal is a MAP for the same internal port the
     * rebooted gateway simply treats it as a creation (§3.7: "from the
     * point of view of the freshly rebooted NAT gateway, it appears as a
     * new mapping request"). */
    {
        uint32_t jitter_ms = 0;
        if (Natpmp_TakeEpochReset(&jitter_ms)) {
            const uint64_t due = now + (uint64_t)jitter_ms;
            if (due < s_upnp_next_renew_ms) {
                s_upnp_next_renew_ms = due;
                SDL_Log("[direct_p2p] %s gateway reboot detected (RFC 6886 §3.6 epoch "
                        "went backwards) — recreating the mapping in %u ms",
                        portmap_backend_name(s_upnp_mapping.backend),
                        (unsigned)jitter_ms);
            }
        }
    }
    if (now < s_upnp_next_renew_ms) {
        return;
    }

    UpnpJob* job = (UpnpJob*)SDL_calloc(1, sizeof(UpnpJob));
    if (job == NULL) {
        s_upnp_next_renew_ms = now + UPNP_RENEW_RETRY_MS;
        return;
    }
    job->internal_port = s_upnp_mapping.internal_port;
    /* RFC 6886 §3.3 / RFC 6887 §11.2.1 both say a renewal SHOULD carry
     * the currently ASSIGNED external port, not the originally wanted
     * one, so a rebooted gateway can recreate the same mapping. That is
     * already what this line has always passed. */
    job->preferred_external = s_upnp_mapping.external_port;
    /* S7: renew the backend that HOLDS the mapping. Re-probing from
     * scratch would run UPnP discovery against a NAT-PMP-only router
     * every half-lease, and — worse — could land the renewal on a
     * different protocol than the teardown path will later try to
     * remove. One ladder only, which is what keeps the worker inside
     * upnp_renew_join_and_discard's 2 s join budget. */
    job->backend = s_upnp_mapping.backend;
    job->natpmp_budget_ms = NATPMP_RENEW_BUDGET_MS;
    /* #147: this spawn needs NO portmap_backends_quiescent() gate, and
     * deliberately does not call it (the call itself would touch the
     * straggler slot from the main thread while a join worker may be
     * touching it). Reaching here requires s_upnp_mapping.active, and a
     * mapping can only be installed by a probe that the #147 spawn gates
     * let run — i.e. while the straggler slot was empty. Every site that
     * parks a straggler (try_portmap timeout, join_portmap_collect,
     * join_portmap_reset, upnp_renew_join_and_discard) either precedes
     * the probe that would install a mapping or is followed by the
     * mapping being memset (Cancel / teardown), so "straggler running"
     * and "mapping active" are mutually exclusive. */
    s_upnp_renew_thread = SDL_CreateThread(upnp_worker_fn, "PortMapRenew", job);
    if (s_upnp_renew_thread == NULL) {
        SDL_free(job);
        s_upnp_next_renew_ms = now + UPNP_RENEW_RETRY_MS;
        SDL_Log("[direct_p2p] WARNING: failed to spawn port-mapping renewal thread");
        return;
    }
    s_upnp_renew_job = job;
    SDL_Log("[direct_p2p] %s lease renewal started (external port %u)",
            portmap_backend_name(s_upnp_mapping.backend),
            s_upnp_mapping.external_port);
}

/* Convert public_ip string to network-byte-order uint32_t for room-code
 * encoding. Returns 0 on failure (still a valid IP, but 0.0.0.0 is not a
 * routable public addr so we also bail the caller). */
static uint32_t ipv4_str_to_be(const char* ip) {
    struct in_addr in = { 0 };
    if (inet_pton(AF_INET, ip, &in) != 1) {
        return 0;
    }
    /* inet_pton writes network byte order into in.s_addr. */
    return (uint32_t)in.s_addr;
}

/* --- bilateral fallback: host-side threads ----------------------------- */

/* Resolve `host` to a NET_Address with a 100ms-bounded poll, mirroring
 * stun.c:272-275. Caller must NET_UnrefAddress on success. Returns NULL
 * on resolve failure. */
static NET_Address* resolve_with_short_poll(const char* host) {
    NET_Address* addr = NET_ResolveHostname(host);
    if (!addr) return NULL;
    int wait_attempts = 0;
    while (NET_GetAddressStatus(addr) == NET_WAITING &&
           wait_attempts < RESOLVE_POLL_ATTEMPTS) {
        SDL_Delay(RESOLVE_POLL_STEP_MS);
        wait_attempts++;
    }
    if (NET_GetAddressStatus(addr) != NET_SUCCESS) {
        NET_UnrefAddress(addr);
        return NULL;
    }
    return addr;
}

/* Host rendezvous worker: parses signal-url, resolves once, then loops
 * sending REGISTER via the s_rendezvous_send_q for the ENTIRE duration
 * of HOST_WAITING (docs/plan-netplay-connection.md S1 "host liveness").
 *
 * The pre-S1 version resent for an 8 s budget and then exited
 * permanently, so (a) the rendezvous server forgot the session at its
 * TTL and (b) the host's NAT mapping decayed at the router's UDP idle
 * timeout — while the overlay kept displaying the room code. Real
 * usage shares the code out-of-band over minutes, so the host must
 * stay registered for as long as it is advertising.
 *
 * Cadence: CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS (default
 * 5000 ms, floor 1000 ms — the server rate-limits at 10 pkts/s/IP).
 * Each REGISTER also refreshes the host->server NAT mapping.
 *
 * Exit conditions (all prompt, <= ~50 ms latency via the inner sleep):
 *   - s_rendezvous_cancel set (DELIVER arrived / Cancel / teardown);
 *   - state leaves HOST_WAITING (direct-punch handoff sets HANDOFF
 *     without raising s_rendezvous_cancel — the state check covers it,
 *     so the loop cannot keep REGISTERing into an active session).
 * Spawn stays gated behind CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL
 * (see host_thread_fn step 5), which remains the kill switch for this
 * whole path. */
static int SDLCALL host_rendezvous_thread_fn(void* data) {
    (void)data;

    /* 1) Parse signal URL from config. */
    const char* signal_url = Config_GetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL);
    if (signal_url == NULL || signal_url[0] == '\0') {
        SDL_Log("[direct_p2p] rendezvous: no signal URL configured; fallback disabled");
        return 0;
    }
    char host[64] = { 0 };
    uint16_t port = 0;
    if (!Rendezvous_ParseSignalUrl(signal_url, host, &port)) {
        SDL_Log("[direct_p2p] rendezvous: malformed signal URL '%s'; fallback disabled",
                signal_url);
        return 0;
    }
    SDL_strlcpy(s_work.signal_host, host, sizeof(s_work.signal_host));
    s_work.signal_port = port;

    /* 2) Resolve hostname once at thread start (100ms-bounded poll —
     * mirror stun.c:272-275). Per the plan, resolve failure exits
     * immediately; the 8-second budget is for peer-pairing, not DNS. */
    NET_Address* signal_addr = resolve_with_short_poll(host);
    if (!signal_addr) {
        SDL_Log("[direct_p2p] rendezvous: failed to resolve %s; fallback disabled", host);
        return 0;
    }

    /* 3) Derive session key from OUR ADVERTISED endpoint — the exact
     * tuple the room code encodes (advertised_port is the UPnP external
     * port when the mapping succeeded, else the STUN port). The joiner
     * derives its key from the decoded room code, so both sides only
     * land in the same rendezvous slot when the host hashes the same
     * pair. Pre-S1 this hashed the raw STUN port, which diverges from
     * the room code whenever UPnP maps a different external port. */
    uint8_t session_key[16];
    uint32_t ip_be = ipv4_str_to_be(s_work.stun.public_ip);
    if (!Rendezvous_DeriveSessionKey(ip_be, s_work.advertised_port, s_work.nonce,
                                     session_key)) {
        SDL_Log("[direct_p2p] rendezvous: failed to derive session key");
        NET_UnrefAddress(signal_addr);
        return 0;
    }

    /* 4) REGISTER packets are (re)built per send since S4c: the cookie
     * tail changes when the main thread answers a server CHALLENGE
     * (signal_cookie_publish) or when the server rotates its cookie
     * slot. Validate buildability once up front so a hard failure
     * still exits early. */
    uint8_t register_pkt[REND_REGISTER_PKT_LEN];
    if (!Rendezvous_BuildRegister(s_work.stun.public_port, session_key, NULL,
                                  register_pkt)) {
        SDL_Log("[direct_p2p] rendezvous: failed to build REGISTER packet");
        NET_UnrefAddress(signal_addr);
        return 0;
    }

    /* 5) Persistent resend loop (S1). First REGISTER goes out
     * immediately, then one every interval_ms until cancel or the state
     * leaves HOST_WAITING. There is deliberately NO wall-clock budget:
     * the host must stay registered (and keep its NAT mapping warm) for
     * as long as the room code is on screen. Cancel-flag and state
     * checks happen every 50 ms inner tick, so exit latency on
     * handoff / cancel / teardown is ~50 ms. */
    int interval_ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS);
    if (interval_ms <= 0) interval_ms = 5000;
    if (interval_ms < 1000) interval_ms = 1000; /* server limits 10 pkts/s/IP */
    /* S3: REGISTERs are now actually flowing — arm the cause-8 advisory
     * (see s_host_registering). */
    SDL_SetAtomicInt(&s_host_registering, 1);
    uint32_t last_send = 0;
    for (;;) {
        if (SDL_GetAtomicInt(&s_rendezvous_cancel)) {
            /* Main thread received DELIVER (or shutdown). Exit cleanly. */
            break;
        }
        if (get_state() != DIRECT_P2P_HOST_WAITING) {
            /* Direct-punch handoff / failure / cancel — we are no longer
             * advertising, so stop re-REGISTERing. This is the exit path
             * for the direct-punch success case, which never raises
             * s_rendezvous_cancel. */
            break;
        }
        uint32_t now = SDL_GetTicks();
        if (last_send == 0 || (now - last_send) >= (uint32_t)interval_ms) {
            /* S4c: rebuild with the latest cookie (zeros until the main
             * thread has answered a CHALLENGE; the server treats an
             * uncookied REGISTER as a challenge request, not a bind). */
            uint8_t cookie[REND_COOKIE_LEN];
            const bool have_cookie = signal_cookie_snapshot(cookie);
            (void)Rendezvous_BuildRegister(s_work.stun.public_port, session_key,
                                           have_cookie ? cookie : NULL,
                                           register_pkt);
            /* Producer must Ref before enqueue; consumer Unrefs after
             * send. On enqueue failure (queue full) we Unref ourselves. */
            NET_Address* ref = NET_RefAddress(signal_addr);
            if (ref) {
                if (!rend_q_enqueue(ref, port, register_pkt, sizeof(register_pkt))) {
                    NET_UnrefAddress(ref);
                }
            }
            last_send = now;
        }
        SDL_Delay(50);
    }

    NET_UnrefAddress(signal_addr);
    return 0;
}

/* Host bilateral-punch worker.
 *
 * The host's only establishment leg is its punch, run through
 * p2p_race() on this same thread (the host has no signalling leg — it is
 * already paired by the time it punches, and its rendezvous worker has
 * exited).
 *
 * §Decision 3 is unchanged and still load-bearing: while this thread
 * runs, the STUN socket is exclusively read/written HERE. The main-thread
 * Tick MUST NOT call host_tick_receive or drain s_rendezvous_send_q
 * during FALLBACK_BILATERAL_PUNCH — see DirectP2P_Tick's case for that
 * state. p2p_race introduces no thread of its own, so that invariant
 * carries over verbatim.
 *
 * On success the endpoint is written BACK to s_work before
 * s_bilateral_handoff_pending is raised, because the main thread reads
 * s_work.peer_ip / peer_public_port in do_handoff. */
static int SDLCALL host_bilateral_punch_thread_fn(void* data) {
    (void)data;

    /* Stack-local copy: the race writes the post-NAT-translation endpoint
     * into its own result, and the main thread reads s_work.peer_ip in
     * do_handoff, so nothing hands &s_work.* to the race. */
    char peer_ip[64];
    SDL_strlcpy(peer_ip, s_work.peer_ip, sizeof(peer_ip));
    const uint16_t peer_port = s_work.peer_public_port;

    int punch_leg_ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_BILATERAL_PUNCH_MS);
    if (punch_leg_ms <= 0) punch_leg_ms = 5000; /* S2: keep in sync with config.c default */

    set_status("Connecting...");
    SDL_Log("[direct_p2p] entering FALLBACK_BILATERAL_PUNCH peer=%s:%u "
            "(punch leg=%dms, race budget=%dms)",
            peer_ip, (unsigned)peer_port, punch_leg_ms, race_budget_ms());

    RaceCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sock = s_work.stun.socket;
    cfg.stun = &s_work.stun;
    /* S4a: the host punches with the token derived from its own advertised
     * tuple (host_thread_fn refuses to advertise without one, so
     * punch_token_valid is guaranteed here). */
    cfg.punch_token = s_work.punch_token;
    cfg.seed_ip = peer_ip;
    cfg.seed_port = peer_port;
    /* The host runs NO rendezvous leg: it is already paired (the DELIVER
     * that started this thread proves it) and its rendezvous worker has
     * exited. With the relay rung gone nothing else on this path needs a
     * signal endpoint or a session key, so the host no longer resolves
     * one — one fewer blocking DNS lookup before the punch starts. */
    cfg.signal_addr = NULL;
    cfg.signal_port = 0;
    cfg.session_key = NULL;
    cfg.my_public_port = s_work.stun.public_port;
    cfg.cookie_in = NULL;
    cfg.signal_leg = false;
    cfg.my_public_ip = s_work.stun.public_ip;
    cfg.punch_leg_ms = punch_leg_ms;
    cfg.race_budget_ms = race_budget_ms();
    cfg.cancel = &s_bilateral_punch_cancel;

    RaceResult res;
    p2p_race(&cfg, &res);

    s_work.t_bilateral_ms = res.t_punch_ms; /* the host's only punch leg */
    s_work.t_race_ms = res.t_race_ms;
    /* #36: carry the attribution evidence out with the timings. */
    s_work.race_confirm_seen = res.confirm_seen;
    s_work.race_confirm_ms = res.confirm_ms;
    s_work.ev_deliver_n = res.deliver_n;
    s_work.ev_challenge_n = res.challenge_n;
    s_work.ev_badver_n = res.badver_n;
    s_work.ev_deliver_gap_max_ms = res.deliver_gap_max_ms;
    /* #122: copied for uniformity, and it is structurally always zero
     * here. This race runs with cfg.signal_addr == NULL (the host is
     * already paired), so signal_ip_str stays empty and the NACK arm's
     * source gate matches nothing. Copying anyway means the report line
     * cannot go stale if the host ever grows a signal leg again. */
    s_work.ev_nack_any = res.nack_any;
    s_work.ev_nack_reason = res.nack_reason;
    s_work.ev_nack_n = res.nack_n;

    if (res.outcome == RACE_CANCELLED ||
        SDL_GetAtomicInt(&s_bilateral_punch_cancel) || cancel_requested()) {
        /* Cancelled: leave state untouched if a terminal state has already
         * been published by another path; otherwise drop to IDLE-ish via
         * Cancel's own teardown. The DirectP2P_Cancel path joins this
         * thread before tearing down s_work, so we just exit. */
        return 0;
    }

    if (res.outcome == RACE_PUNCHED) {
        /* Writeback BEFORE signaling — main-thread do_handoff reads
         * s_work.peer_ip/peer_public_port and we want the punched
         * endpoint. */
        SDL_strlcpy(s_work.peer_ip, res.peer_ip, sizeof(s_work.peer_ip));
        s_work.peer_public_port = res.peer_port;
        set_status("Connecting...");
        /* Cannot set_state(DIRECT_P2P_HANDOFF) here: do_handoff calls
         * Netplay_SetParams / Netplay_SetRemotePort / Netplay_SetStunSocket
         * which are main-thread-only. Stay in FALLBACK_BILATERAL_PUNCH and
         * raise s_bilateral_handoff_pending so the next Tick observes us,
         * joins this thread, and runs do_handoff on the main thread. */
        SDL_SetAtomicInt(&s_bilateral_handoff_pending, 1);
        SDL_Log("[direct_p2p] host race SUCCESS (punch) - handoff pending via %s:%u",
                s_work.peer_ip, (unsigned)s_work.peer_public_port);
        return 0;
    }

    /* Review M1: do NOT park terminal from here. Raise the failure flag
     * and let Tick (main thread) decide: back to HOST_WAITING with the
     * rendezvous loop respawned, or FAILED_BILATERAL once the per-session
     * retry budget is spent. State stays FALLBACK_BILATERAL_PUNCH until
     * Tick acts. */
    SDL_SetAtomicInt(&s_bilateral_failed, 1);
    SDL_Log("[direct_p2p] host race FAILED after %u ms — deferring disposition to Tick",
            (unsigned)res.t_race_ms);
    return 0;
}

/* Host worker: UPnP (optional) -> STUN discover -> publish room code ->
 * transition to HOST_WAITING. The main-thread Tick then owns the wait
 * for the first inbound datagram. */
static int SDLCALL host_thread_fn(void* data) {
    (void)data;

    /* Let the main thread finish its first render frame before hitting
     * SDL_net. Calling NET_Init / NET_ResolveHostname on a worker thread
     * while the main thread is still completing game init (ppg, sound,
     * memcard) races and segfaults inside NET_ResolveHostname on MiSTer
     * (glibc 2.31, ARM). 200ms is empirically enough to get past all
     * the initialize_game() work that follows defer_direct_p2p_handoff. */
    SDL_Delay(WORKER_STARTUP_DELAY_MS);

    const uint16_t local_port = (uint16_t)s_work.preferred_port;

    /* #104: one more rung of the S2 ladder. Counted HERE — past the
     * startup delay, before any leg — so the count is "rungs entered",
     * which is what the report's t_*_total_ms fields are the sum over.
     * Saturating: structurally this can only reach
     * 1 + HOST_STUN_MAX_RETRIES, but a counter that can wrap is a
     * counter that can lie. */
    if (s_work.host_rungs < 255u) {
        s_work.host_rungs++;
    }

    /* 1) Port-mapping probe: UPnP IGD first, then NAT-PMP/PCP (S7).
     * Non-fatal if it fails — we fall through to STUN. Request external
     * == internal so the router doesn't have to invent an external port
     * (some firmware rejects wildcards, error 716); NAT-PMP's §3.3 and
     * PCP's §11.1 Suggested External Port fields carry the same request,
     * and neither gateway is obliged to honour it.
     *
     * S2-retry hygiene (review L-5): on a FAILED_STUN auto-retry, a
     * PREVIOUS attempt in this hosting session may already hold a live
     * mapping (s_upnp_mapping is only reset in DirectP2P_Init /
     * teardown, deliberately — the router-side lease survives the
     * retry). Re-probing could only refresh that same (internal,
     * external) pair, but a TRANSIENT re-probe failure would leave
     * upnp_ok=false while s_upnp_mapping.active stays true — the
     * advertised port becomes the STUN port while a later room-code
     * drift re-encode (which keys off s_upnp_mapping.active) would
     * switch to the stale mapping's port. Skip the re-probe and trust
     * the live mapping instead: consistent with S1's renewal policy,
     * which likewise KEEPS the mapping when a renew attempt fails
     * (upnp_renew_tick) rather than tearing it down on a blip. */
    set_state(DIRECT_P2P_UPNP_PROBE);
    uint32_t stage_t0 = SDL_GetTicks();
    bool upnp_ok;
    bool probe_skipped_busy = false; /* #147: gate skip, for honest attribution */
    if (s_upnp_mapping.active && s_upnp_mapping.internal_port == local_port) {
        SDL_Log("[direct_p2p] retry: reusing the live %s mapping from the previous "
                "attempt (external %u -> internal %u) — skipping re-probe",
                portmap_backend_name(s_upnp_mapping.backend),
                s_upnp_mapping.external_port, s_upnp_mapping.internal_port);
        upnp_ok = true;
    } else if (s_portmap_failed_port == local_port) {
        /* #96: the SAME skip, for the other verdict. An earlier rung of
         * THIS ladder already asked the LAN for a mapping on this port
         * and was told no — by a full PORTMAP_PROBE_BUDGET_MS of
         * silence, which is measured, not provisioned (see
         * s_portmap_failed_port). Nothing between two rungs of an
         * automatic retry changes the answer: the ladder's own trigger
         * is a STUN failure, which says nothing about whether the LAN
         * has an IGD or a NAT-PMP gateway. Asking again costs the user
         * 11,250 ms and cannot inform anything.
         *
         * upnp_ok stays false, which is the path host_thread_fn already
         * takes on a fresh failure — the room code carries the
         * STUN-observed endpoint. No new outcome is introduced here;
         * only the repetition is removed. */
        SDL_Log("[direct_p2p] retry: the port-map probe for internal port %u already "
                "failed this hosting session — skipping the re-probe (it would cost "
                "another %u ms and cannot change its answer); advertising the "
                "STUN-observed endpoint",
                local_port, (unsigned)PORTMAP_PROBE_BUDGET_MS);
        upnp_ok = false;
    } else if (!portmap_backends_quiescent()) {
        /* #147: an overrun probe/renewal worker from an EARLIER session
         * (Cancel -> BeginHost re-arms this path; the intra-session
         * ladder is closed by the #96 latch above) is still executing
         * inside miniupnpc / natpmp process globals. Spawning a second
         * worker into those statics is the data-race / double-free
         * natpmp.c's L-3 comment documents. Skip THIS session's probe —
         * attributed below, NOT latched into s_portmap_failed_port: "we
         * did not ask" is not a verdict, and the user's next Host press
         * re-checks a slot that reaps itself the moment the straggler
         * exits. */
        SDL_Log("[direct_p2p] a previous port-map attempt is still running inside "
                "the backend libraries — skipping this session's probe rather than "
                "racing it; advertising the STUN-observed endpoint");
        probe_skipped_busy = true;
        upnp_ok = false;
    } else {
        /* #104: the rung that actually PAYS the probe. Since #96 this is
         * at most ONE rung per hosting session — a report showing
         * host_portmap_probes=1 alongside host_rungs=4 is the fix
         * working; host_portmap_probes==host_rungs would mean a verdict
         * is not being latched. */
        if (s_work.host_portmap_probes < 255u) {
            s_work.host_portmap_probes++;
        }
        upnp_ok = try_portmap(local_port, local_port);
        if (!upnp_ok) {
            s_portmap_failed_port = local_port;
        }
    }
    s_work.t_upnp_ms = SDL_GetTicks() - stage_t0;
    s_work.t_upnp_total_ms += s_work.t_upnp_ms;
    /* #96 / #36 — portmap leg visibility. Recorded at the CALL SITE, not
     * inside try_portmap: the probe's cost and its verdict are a property
     * of this stage. Worker thread => the MT sink, which tees to SDL_Log.
     *
     * The trailing note used to read "this whole cost is paid again on
     * every host STUN retry". Since #96 it is not: the verdict — either
     * one — is latched for the hosting session, so `probes=1` next to a
     * four-rung ladder is the expected shape and the note says so. */
    s_work.portmap_backend = (uint8_t)s_upnp_mapping.backend;
    s_work.portmap_active = s_upnp_mapping.active;
    {
        char pm_line[256];
        SDL_snprintf(pm_line, sizeof(pm_line),
                     "[netplay-connect] PORTMAP backend=%s active=%d ms=%u probes=%u "
                     "rung=%u — %s",
                     portmap_backend_name(s_upnp_mapping.backend),
                     (int)s_upnp_mapping.active,
                     (unsigned)s_work.t_upnp_ms,
                     (unsigned)s_work.host_portmap_probes,
                     (unsigned)s_work.host_rungs,
                     s_upnp_mapping.active
                         ? "mapped"
                         : probe_skipped_busy
                             ? "probe SKIPPED: an overrun probe from an earlier attempt "
                               "is still running (#147 spawn gate); no verdict latched — "
                               "this session advertises the STUN endpoint"
                             : "probe found no IGD and no NAT-PMP/PCP gateway; the verdict "
                               "is latched for this hosting session, so the ladder's "
                               "remaining rungs skip it");
        Netplay_LogConnectEventMT(pm_line);
    }
    if (cancel_requested()) {
        set_status("Cancelled.");
        set_state(DIRECT_P2P_IDLE);
        return 0;
    }
    if (upnp_ok) {
        SDL_Log("[direct_p2p] %s mapping OK (external %s:%u -> internal %u)",
                portmap_backend_name(s_upnp_mapping.backend),
                s_upnp_mapping.external_ip[0] ? s_upnp_mapping.external_ip : "(unknown)",
                s_upnp_mapping.external_port,
                s_upnp_mapping.internal_port);
    } else {
        SDL_Log("[direct_p2p] no port mapping (UPnP and NAT-PMP/PCP both unavailable or "
                "refused); falling back to STUN.");
    }

    /* 2) STUN discover. We always need a public IP + port for the room
     * code, even if UPnP succeeded (the room code encodes the public
     * tuple; the peer needs to know where to send). Stun_Discover
     * itself assumes NET_Init has already run — ensure it here on the
     * worker thread because this may be the first netplay path any
     * session takes. NET_Init is idempotent. */
    NET_Init();
    set_state(DIRECT_P2P_STUN_DISCOVER);
    set_status("Preparing...");
    stage_t0 = SDL_GetTicks();
    bool stun_ok = STUN_DISCOVER(&s_work.stun, local_port, stun_budget_ms());
    s_work.t_stun_ms = SDL_GetTicks() - stage_t0;
    s_work.t_stun_total_ms += s_work.t_stun_ms; /* #104: across the ladder */
    if (cancel_requested()) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Cancelled.");
        set_state(DIRECT_P2P_IDLE);
        return 0;
    }
    if (!stun_ok) {
        /* S3 causes 1-2 (host side): same classification as the joiner.
         * Review M-2: a local socket-creation failure is not a network
         * condition — classify INTERNAL, not "no internet". S4-review
         * L-3: a CSPRNG failure is the same shape — nothing was ever
         * sent, so a connectivity diagnosis would be a lie. */
        set_fail((s_work.stun.diag_socket_fail || s_work.stun.diag_csprng_fail)
                     ? CONNECT_FAIL_INTERNAL
                     : ConnectFail_ClassifyStunDiscover(
                           s_work.stun.diag_servers_probed,
                           s_work.stun.diag_servers_answered,
                           s_work.stun.diag_sends_ok,
                           s_work.stun.diag_dns_all_failed));
        set_state(DIRECT_P2P_FAILED_STUN);
        return 0;
    }

    /* S1 CGNAT gate: on carrier-grade NAT (or any double NAT) the inner
     * router happily grants a mapping whose external IP is a private /
     * CGN (100.64/10) address, while STUN reports the true public IP.
     * The room code is built from the STUN IP + the chosen port, so
     * advertising the UPnP port would pair the STUN IP with a port that
     * only exists on the INNER router — a wrong (ip, port) pair that
     * silently kills the direct path.
     *
     * Review M3: the mapping is dropped only when the router's external
     * IP PARSES and is provably non-public (RFC1918 / CGN 100.64.0.0/10 /
     * loopback / link-local) — that is the double-NAT signature. A
     * public-but-different external IP is ISP 1:1 NAT / DMZ territory,
     * where the mapping forwards perfectly and dropping it would
     * downgrade a host with a good direct path; an unparseable string
     * (garbage, IPv6) proves nothing. Both keep the mapping and log.
     *
     * S7: this gate is backend-agnostic BY DESIGN and NOT duplicated per
     * protocol. It keys off s_upnp_mapping.external_ip, which all three
     * backends fill — UPnP from UPNP_GetExternalIPAddress (upnp.c),
     * PCP from the MAP response's Assigned External IP Address (RFC 6887
     * §11.2), NAT-PMP from a Public Address Request (RFC 6886 §3.2),
     * which natpmp.c issues before the mapping request precisely so this
     * comparison has something to judge. The only S7 change here is that
     * the release dispatches on the backend. */
    if (upnp_ok &&
        !direct_p2p_ip_eq_normalized(s_upnp_mapping.external_ip, s_work.stun.public_ip)) {
        if (direct_p2p_ip_is_nonpublic(s_upnp_mapping.external_ip)) {
            SDL_Log("[direct_p2p] CGNAT detected: %s external IP %s is private/CGN and "
                    "!= STUN public IP %s — ignoring the mapping and advertising "
                    "the STUN endpoint instead",
                    portmap_backend_name(s_upnp_mapping.backend),
                    s_upnp_mapping.external_ip, s_work.stun.public_ip);
            /* Release the useless mapping now (worker thread — same thread
             * class try_portmap used; no concurrent miniupnpc user exists
             * in UPNP_PROBE/STUN_DISCOVER states). Also stops the S1 lease
             * renewal from ever arming for it. */
            portmap_remove(&s_upnp_mapping);
            memset(&s_upnp_mapping, 0, sizeof(s_upnp_mapping));
            s_portmap_lease_expiry_ms = 0;
            upnp_ok = false;
        } else {
            SDL_Log("[direct_p2p] %s external IP %s differs from STUN public IP %s but "
                    "is not a private/CGN address (1:1 NAT / DMZ, or unparseable) — "
                    "keeping the mapping",
                    portmap_backend_name(s_upnp_mapping.backend),
                    s_upnp_mapping.external_ip[0] ? s_upnp_mapping.external_ip : "(empty)",
                    s_work.stun.public_ip);
        }
    }

    /* 3) Build room code from discovered endpoint. If UPnP succeeded we
     * prefer its external port over the STUN-observed port — symmetric
     * NATs translate per-destination, so the STUN port is only valid
     * for STUN servers; the UPnP mapping is stable for any peer. The
     * room code no longer carries the host's LAN local_port; see
     * room_code.h for the rationale. */
    uint16_t pub_port = upnp_ok ? s_upnp_mapping.external_port : s_work.stun.public_port;
    uint32_t ip_be = ipv4_str_to_be(s_work.stun.public_ip);
    /* v4 (task #155): the room code no longer carries a nonce — see
     * room_code.h's "nonce leaves the code" section for the accepted-
     * risk writeup. Every derivation below uses the fixed value, so
     * the session key and punch token are a pure function of the
     * advertised (ip, port) tuple. */
    s_work.nonce = ROOM_CODE_V4_FIXED_NONCE;
    if (ip_be == 0 ||
        !RoomCode_Encode(ip_be, pub_port, s_work.host_code)) {
        Stun_CloseSocket(&s_work.stun);
        set_fail(CONNECT_FAIL_INTERNAL);
        set_state(DIRECT_P2P_FAILED_STUN);
        return 0;
    }
    /* Record the advertised tuple's port — the rendezvous session key is
     * derived from (public_ip, advertised_port) on both roles (S1). Must
     * be visible before HOST_WAITING publishes (the rendezvous thread
     * spawns after and reads it). */
    s_work.advertised_port = pub_port;

    /* S4a: derive the punch-auth token from the SAME advertised tuple
     * the room code encodes — the joiner derives the identical token
     * from the decoded code. Fail closed: without a token the host's
     * datagram gate would ignore every punch, so an unjoinable room
     * must not be advertised at all. */
    s_work.punch_token_valid =
        Rendezvous_DerivePunchToken(ip_be, pub_port, s_work.nonce, s_work.punch_token);
    if (!s_work.punch_token_valid) {
        Stun_CloseSocket(&s_work.stun);
        set_fail(CONNECT_FAIL_INTERNAL);
        set_state(DIRECT_P2P_FAILED_STUN);
        return 0;
    }

    /* 4) Publish — Tick() will now drain the STUN socket non-blockingly
     * until a peer punches through. The room code renders on overlay
     * line 2 via DirectP2P_GetHostCode(); this status goes on line 3.
     *
     * Cancel-flag race fix: clear s_rendezvous_cancel and
     * s_bilateral_punch_cancel BEFORE publishing HOST_WAITING. Once Tick
     * sees HOST_WAITING it can call host_tick_receive -> try_handle_deliver
     * which raises s_rendezvous_cancel as its DELIVER signal. If we
     * cleared after publishing, that signal could be clobbered by us
     * here. Clear-before-publish ensures any subsequent set-by-DELIVER
     * survives. */
    SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 0);

    set_status("Waiting for player 2...");
    set_state(DIRECT_P2P_HOST_WAITING);
    {
        /* MEDIUM-4 (re-scoped for v4, room_code.h): the redaction here is
         * hygiene, not confidentiality — the ip:port on the same line
         * already IS the whole payload. */
        char code_redacted[ROOM_CODE_BUF_LEN];
        RoomCode_Redact(s_work.host_code, code_redacted);
        SDL_Log("[direct_p2p] HOST_WAITING published. Code=%s (redacted) "
                "public=%s:%u (via %s)",
                code_redacted, s_work.stun.public_ip, (unsigned)pub_port,
                upnp_ok ? portmap_backend_name(s_upnp_mapping.backend) : "STUN");
    }

    /* 5) Bilateral fallback: spawn the rendezvous-resender thread unless
     * the kill switch is set. The thread enqueues REGISTER packets onto
     * s_rendezvous_send_q; the main thread drains them in Tick's
     * HOST_WAITING branch. Per §Decision 5 the host stays in
     * HOST_WAITING during this phase — there's no host-side
     * FALLBACK_SIGNALING transition.
     *
     * Note: s_rendezvous_cancel was cleared above (before HOST_WAITING
     * publish) to avoid clobbering a try_handle_deliver-set value. */
    if (!Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL)) {
        s_rendezvous_thread = SDL_CreateThread(host_rendezvous_thread_fn,
                                               "DirectP2PRendezvous", NULL);
        if (s_rendezvous_thread == NULL) {
            SDL_Log("[direct_p2p] WARNING: failed to spawn rendezvous thread; "
                    "fallback disabled this session");
        }
    }
    return 0;
}

/* Join attempt: STUN discover -> hairpin-detect -> Stun_HolePunch ->
 * bilateral fallback. One complete pass of the joiner flow. RETURNS the
 * terminal state instead of publishing it — the join_thread_fn wrapper
 * below owns the terminal set_state so it can interpose the S2
 * auto-retry (docs/plan-netplay-connection.md §4). Intermediate states
 * (STUN_DISCOVER / JOIN_PUNCHING / FALLBACK_*) are still published
 * here; every failure return has already closed the attempt's socket
 * and set the status text; DIRECT_P2P_HANDOFF returns with s_work
 * fully written back. */
/* S3: fresh evidence per attempt — the retry's classification and report
 * must not inherit the first attempt's DELIVER counters or the timings
 * of stages the retry never reached.
 *
 * THIS IS THE ONLY PLACE the per-attempt fields are cleared. s_work's
 * whole-struct memsets run once per attempt SET (BeginHost / BeginJoin /
 * Cancel / Reset), never between attempt 1 and attempt 2 of the S2
 * retry, so a field that is written by an attempt and read by
 * report_connect_outcome MUST be listed here. Factored into its own
 * function so the NETPLAY_TEST_HOOKS seam below can exercise the real
 * block rather than a copy of it that could drift. */
static void join_reset_attempt_evidence(void) {
    s_work.fail_code = CONNECT_FAIL_NONE;
    s_work.ev_deliver_any = false;
    s_work.ev_deliver_real = false;
    s_work.ev_challenge_any = false; /* S4c */
    s_work.ev_cookie_rechallenged = false; /* #105 — per-attempt, see LIFETIME */
    s_work.t_stun_ms = 0;
    s_work.t_punch_ms = 0;
    s_work.t_signal_ms = 0;
    s_work.t_bilateral_ms = 0;
    s_work.t_race_ms = 0;           /* S6 */
    /* #36: the attribution evidence is per-attempt for exactly the same
     * reason. Leaving it stale produces a report line that cannot be
     * true of a single attempt — e.g. attempt 1 races and exhausts,
     * attempt 2's STUN never comes back, and the FAIL line then reads
     * `code=P2P_FAIL_STUN_ALLDOWN ... race_deliver_n=5 confirm=1@7412ms`:
     * punch evidence that cannot coexist with "UDP is dead". */
    s_work.race_confirm_seen = false;
    s_work.race_confirm_ms = 0;
    s_work.ev_deliver_n = 0;
    s_work.ev_challenge_n = 0;
    s_work.ev_badver_n = 0;
    s_work.ev_deliver_gap_max_ms = 0;
    /* #122: a refusal is per-attempt evidence like every field above it.
     * The S2 retry binds a FRESH source port, which is precisely what
     * changes the answer for the reclaim family — attempt 2 may be
     * accepted where attempt 1 drew SESSION_FULL — so carrying attempt
     * 1's reason forward would report a refusal that is no longer true. */
    s_work.ev_nack_any = false;
    s_work.ev_nack_reason = 0;
    s_work.ev_nack_n = 0;
}

#ifdef NETPLAY_TEST_HOOKS
void DirectP2P_TestHook_JoinAttemptEvidenceReset(const DirectP2PAttemptEvidence* in,
                                                 DirectP2PAttemptEvidence* out) {
    if (in != NULL) {
        s_work.fail_code = (ConnectFailCode)in->fail_code;
        s_work.ev_deliver_any = in->deliver_any;
        s_work.ev_deliver_real = in->deliver_real;
        s_work.ev_challenge_any = in->challenge_any;
        s_work.ev_cookie_rechallenged = in->cookie_rechallenged; /* #105 */
        s_work.t_race_ms = in->t_race_ms;
        s_work.race_confirm_seen = in->confirm_seen;
        s_work.race_confirm_ms = in->confirm_ms;
        s_work.ev_deliver_n = in->deliver_n;
        s_work.ev_challenge_n = in->challenge_n;
        s_work.ev_badver_n = in->badver_n;
        s_work.ev_deliver_gap_max_ms = in->deliver_gap_max_ms;
        s_work.ev_nack_any = in->nack_any;   /* #122 */
        s_work.ev_nack_reason = in->nack_reason;
        s_work.ev_nack_n = in->nack_n;
    }
    join_reset_attempt_evidence();
    if (out != NULL) {
        out->fail_code = (int)s_work.fail_code;
        out->deliver_any = s_work.ev_deliver_any;
        out->deliver_real = s_work.ev_deliver_real;
        out->challenge_any = s_work.ev_challenge_any;
        out->cookie_rechallenged = s_work.ev_cookie_rechallenged; /* #105 */
        out->t_race_ms = s_work.t_race_ms;
        out->confirm_seen = s_work.race_confirm_seen;
        out->confirm_ms = s_work.race_confirm_ms;
        out->deliver_n = s_work.ev_deliver_n;
        out->challenge_n = s_work.ev_challenge_n;
        out->badver_n = s_work.ev_badver_n;
        out->deliver_gap_max_ms = s_work.ev_deliver_gap_max_ms;
        out->nack_any = s_work.ev_nack_any;  /* #122 */
        out->nack_reason = s_work.ev_nack_reason;
        out->nack_n = s_work.ev_nack_n;
    }
}
#endif

/* #121: `bind_port` is 0 for "let the OS choose", which is what every
 * attempt did before this task and what attempt 1 still does. It is
 * non-zero only when the previous attempt's background port-map probe
 * came back with a live mapping, in which case this attempt MUST bind
 * that mapping's internal port or the mapping addresses nothing.
 *
 * This is the one place #121 touches an attempt's socket, and the reason
 * it is safe to do so is that it cannot happen to a joiner the matrix
 * currently passes: join_portmap_spawn refuses to probe unless STUN
 * reported port_disagreement, so a non-symmetric joiner never obtains a
 * mapping and never reaches a non-zero bind_port. The retry's documented
 * "freshly bound socket" property (see join_thread_fn) is preserved
 * verbatim on that path.
 *
 * Residual, stated rather than hidden: binding a specific port can fail
 * where port 0 could not. The port was ours microseconds earlier —
 * join_attempt closes its socket on every failure path and UDP has no
 * TIME_WAIT — so this is unlikely, and if it does happen the cost is the
 * retry of a symmetric joiner that was failing anyway. It is not a cost
 * any currently-passing cell can pay. */
static DirectP2PState join_attempt(uint16_t bind_port) {
    join_reset_attempt_evidence();
    s_work.join_attempts++;

    set_state(DIRECT_P2P_STUN_DISCOVER);
    set_status("Preparing...");
    uint32_t stage_t0 = SDL_GetTicks();
    bool stun_ok = STUN_DISCOVER(&s_work.stun, bind_port, stun_budget_ms());
    /* #121: a PINNED port can fail to bind where port 0 could not, and
     * before this task attempt 2 could not fail this way at all. Left
     * alone it is a bad trade: Stun_Discover reports a local bind failure
     * as diag_socket_fail, which classifies INTERNAL, so a symmetric
     * joiner whose mapped port was taken in the interval would surface a
     * spurious internal error INSTEAD of the NAT diagnosis it earned —
     * and attempt 2 is the last attempt, so the race never runs.
     *
     * Retry once without the pin. The mapping is worth trying for; it is
     * not worth losing the attempt over.
     *
     * The retry is charged to the SAME budget, deliberately. A second
     * full stun_budget_ms() here would add a leg to the cascade and
     * ORCH_JOIN_ATTEMPT_MS would have to grow to cover it — which assert
     * [A] would then reject at shipped defaults (39,800 > 35,000). Paying
     * it out of the remaining budget keeps the attempt's STUN leg bounded
     * by exactly one stun_budget_ms(), so the derivation below is
     * untouched and stays honest. In practice there is nearly the whole
     * budget left: diag_socket_fail is a verdict reached BEFORE anything
     * is sent, so the failed call consumed no network time. */
    if (!stun_ok && bind_port != 0 && s_work.stun.diag_socket_fail) {
        const int spent = (int)(SDL_GetTicks() - stage_t0);
        const int left = stun_budget_ms() - spent;
        SDL_Log("[direct_p2p] joiner: could not bind the mapped internal port %u "
                "(%d ms spent); retrying this attempt on an OS-assigned port with "
                "the remaining %d ms of the STUN budget",
                (unsigned)bind_port, spent, left > 0 ? left : 0);
        if (left > 0) {
            stun_ok = STUN_DISCOVER(&s_work.stun, 0, left);
        }
    }
    s_work.t_stun_ms = SDL_GetTicks() - stage_t0;
    if (cancel_requested()) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Cancelled.");
        return DIRECT_P2P_IDLE;
    }
    if (!stun_ok) {
        /* S3 causes 1-2: distinguish "no network / DNS dead" from
         * "outbound UDP filtered" using the discovery evidence.
         * Review M-2: a local socket-creation failure is neither —
         * classify INTERNAL, not "no internet". */
        set_fail((s_work.stun.diag_socket_fail || s_work.stun.diag_csprng_fail)
                     ? CONNECT_FAIL_INTERNAL
                     : ConnectFail_ClassifyStunDiscover(
                           s_work.stun.diag_servers_probed,
                           s_work.stun.diag_servers_answered,
                           s_work.stun.diag_sends_ok,
                           s_work.stun.diag_dns_all_failed));
        return DIRECT_P2P_FAILED_STUN;
    }

    /* #121: STUN has just told us two things this is the first moment to
     * know — the socket's bound port, and whether this NAT translates
     * per-destination. Spawn the port-map probe on both and DO NOT WAIT;
     * the free window is the race that starts a few lines below. See
     * s_join_portmap_thread for why waiting is the one thing the joiner
     * budget cannot afford. */
    join_portmap_spawn(s_work.stun.local_port);

    /* Hairpin detect: peer public IP == our public IP => same LAN.
     * The room code used to carry the peer's LAN local_port so we
     * could rewrite the target to 127.0.0.1:peer_local_port; since
     * that field was dropped (see room_code.h) we now rely on the
     * router's NAT-loopback support. If the router forwards the UDP
     * mapping back to itself on a same-LAN send, the hole-punch
     * completes normally using the STUN-discovered public address. */
    if (s_work.peer_ip[0] != '\0' && s_work.stun.public_ip[0] != '\0' &&
        strcmp(s_work.peer_ip, s_work.stun.public_ip) == 0) {
        SDL_Log("[direct_p2p] NAT hairpin detected — router must support "
                "NAT loopback. Proceeding with public address %s:%u.",
                s_work.peer_ip, (unsigned)s_work.peer_public_port);
    }

    /* S4a: derive the punch-auth token from the DECODED room-code tuple
     * (the host derives the identical token from its advertised tuple).
     * Derived once per attempt; used for EVERY punch leg of the race.
     * Never overwritten by retargeting — the token is bound to the
     * advertised payload, not to whatever translated endpoint a leg later
     * observes. */
    uint8_t punch_token[STUN_PUNCH_TOKEN_LEN];
    {
        uint32_t token_host_ip_be = ipv4_str_to_be(s_work.peer_ip);
        if (!Rendezvous_DerivePunchToken(token_host_ip_be, s_work.peer_public_port,
                                         s_work.nonce, punch_token)) {
            Stun_CloseSocket(&s_work.stun);
            set_fail(CONNECT_FAIL_INTERNAL);
            return DIRECT_P2P_FAILED_STUN;
        }
        /* Task #119: mirror the token into s_work so do_handoff can arm
         * netplay's late-punch rescue layer on the JOINER too (the field
         * was host-only before this; see its declaration comment).
         * Worker-thread write, but by the time the main thread reads it
         * in do_handoff this worker has published HANDOFF and been
         * joined — the same ordering every other s_work field relies
         * on. The race below keeps using the stack copy, unchanged. */
        memcpy(s_work.punch_token, punch_token, sizeof(s_work.punch_token));
        s_work.punch_token_valid = true;
    }

    /* ---- S6: the bypasses move UP FRONT --------------------------------
     *
     * Pre-S6 these three were evaluated AFTER the direct punch had already
     * failed, because that was the only point at which the code decided
     * whether to enter the fallback. In a race there is no such point: the
     * signalling leg starts at t=0 alongside the punch. So the question
     * they answer — "is the rendezvous fallback worth running AT ALL for
     * this peer?" — has to be answered before the race is configured.
     *
     * Answering earlier does not change any verdict, because none of the
     * three depends on the punch outcome:
     *   (a) kill switch  — pure configuration;
     *   (b) LAN peer     — a private-subnet peer must be reached directly;
     *                      public rendezvous is the wrong path for it;
     *   (c) hairpin      — peer's public IP equals ours, so the bilateral
     *                      punch would loop back through the same router
     *                      that has already proven it lacks NAT loopback.
     * Each still yields FAILED_SYMMETRIC with the same status text; the
     * only change is that the punch leg now runs WITH that decision
     * already made, instead of before it.
     *
     * The FOURTH pre-S6 bypass — diag_punch_bad_token — genuinely depends
     * on the punch, so it stays where it always was: after the race. */
    bool fallback_ok = true;
    if (Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL)) {
        fallback_ok = false;
    } else if (direct_p2p_is_lan_peer(s_work.peer_ip)) {
        fallback_ok = false;
    } else if (s_work.stun.public_ip[0] != '\0' &&
               direct_p2p_ip_eq_normalized(s_work.peer_ip, s_work.stun.public_ip)) {
        fallback_ok = false;
    }

    /* Resolve the rendezvous endpoint and derive the session key BEFORE
     * the race — the race must never do blocking DNS from inside its
     * loop. Failures here are not fatal to the DIRECT punch, which is a
     * behaviour change worth stating: pre-S6 a malformed signal URL
     * returned FAILED_BILATERAL without the direct punch ever having been
     * given its full window in the presence of the fallback. Now the
     * punch leg still runs and can still win; only the fallback legs are
     * dropped, and the failure is reported if nothing connects. */
    NET_Address* signal_addr = NULL;
    uint16_t signal_port = 0;
    uint8_t session_key[16];
    bool have_session_key = false;
    ConnectFailCode signal_setup_fail = CONNECT_FAIL_NONE;

    if (fallback_ok) {
        const char* signal_url = Config_GetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL);
        char signal_host[64] = { 0 };
        if (signal_url == NULL || signal_url[0] == '\0' ||
            !Rendezvous_ParseSignalUrl(signal_url, signal_host, &signal_port)) {
            SDL_Log("[direct_p2p] joiner: missing/malformed signal URL — fallback legs off");
            signal_setup_fail = CONNECT_FAIL_INTERNAL;
        } else {
            /* Session key from the HOST's public endpoint, decoded from the
             * room code into peer_ip / peer_public_port. The host
             * registered with the same hash and the server pairs entries by
             * literal session_key equality, so both sides MUST hash the
             * host's tuple to land in the same slot. */
            const uint32_t host_ip_be = ipv4_str_to_be(s_work.peer_ip);
            if (!Rendezvous_DeriveSessionKey(host_ip_be, s_work.peer_public_port,
                                             s_work.nonce, session_key)) {
                SDL_Log("[direct_p2p] joiner: failed to derive session key");
                signal_setup_fail = CONNECT_FAIL_INTERNAL;
            } else {
                have_session_key = true;
                signal_addr = resolve_with_short_poll(signal_host);
                if (signal_addr == NULL) {
                    SDL_Log("[direct_p2p] joiner: failed to resolve %s", signal_host);
                    /* DNS worked for STUN moments ago; a dead resolve HERE
                     * most likely means the rendezvous hostname is gone. */
                    signal_setup_fail = CONNECT_FAIL_RENDEZVOUS_DOWN;
                }
            }
        }
    }

    /* ---- S6: run the race ---------------------------------------------- */
    set_state(DIRECT_P2P_JOIN_PUNCHING);
    set_status("Connecting...");

    int punch_leg_ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_BILATERAL_PUNCH_MS);
    if (punch_leg_ms <= 0) punch_leg_ms = 5000; /* S2: keep in sync with config.c */
    int signal_budget_ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_BUDGET_MS);
    if (signal_budget_ms <= 0) signal_budget_ms = 8000;
#ifdef NETPLAY_TEST_HOOKS
    if (s_test_signal_budget_ms > 0) signal_budget_ms = s_test_signal_budget_ms;
#endif

    RaceCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sock = s_work.stun.socket;
    cfg.stun = &s_work.stun;
    cfg.punch_token = punch_token;
    cfg.seed_ip = s_work.peer_ip;
    cfg.seed_port = s_work.peer_public_port;
    cfg.signal_addr = signal_addr;
    cfg.signal_port = signal_port;
    cfg.session_key = have_session_key ? session_key : NULL;
    /* #121: this stays s_work.stun.public_port, and the mapped external
     * port is DELIBERATELY not substituted here.
     *
     * A first cut of #121 did substitute it, on the reasoning that the
     * mapped port is the endpoint-independent one and therefore the
     * better thing to advertise. That reasoning is wrong about how the
     * joiner's endpoint actually reaches the host, and the rule is
     * already written down one screen away — see the host's cookied-echo
     * REGISTER: `my_public_port` "must carry s_work.stun.public_port (the
     * source port the server checks against rinfo.port) and NOT
     * s_work.advertised_port, which differs whenever UPnP maps an
     * external port the STUN probe never saw."
     *
     * The rendezvous server does not route on this field at all. It reads it,
     * compares it to the observed source port, WARNS on a mismatch (the
     * `my_public_port` warn in handleRegister, rendezvous-server.js:1250), and
     * stores the OBSERVED tuple as the slot endpoint. So substituting the mapped port
     * changes nothing the host aims at, and instead makes every REGISTER
     * of a mapped joiner trip an unthrottled server-side warning — at the
     * race's REGISTER cadence, a steady log stream on a shared server,
     * bought for no connectivity at all.
     *
     * #121 works through the OBSERVED port instead, which is the honest
     * mechanism: pinning the socket to the mapping's internal port
     * (bind_port) is what makes the gateway translate our outbound to the
     * mapped external port, so rinfo.port BECOMES the mapped port and the
     * server records it without anyone having to claim it. The mapping
     * earns the endpoint; it does not assert it.
     *
     * That is also why there is no joiner CGNAT gate here. The host needs
     * one because it PUBLISHES a (STUN ip, mapped port) tuple in the room
     * code and a double NAT makes that pair incoherent. The joiner
     * publishes nothing — the server observes it — so there is no
     * mismatched pair to construct. */
    cfg.my_public_port = s_work.stun.public_port;
    cfg.cookie_in = NULL; /* the joiner starts uncookied; the race binds one */
    cfg.signal_leg = (signal_addr != NULL && have_session_key);
    cfg.signal_budget_ms = signal_budget_ms;
    cfg.my_public_ip = s_work.stun.public_ip;
    cfg.punch_leg_ms = punch_leg_ms;
    cfg.race_budget_ms = race_budget_ms();
    cfg.cancel = &s_cancel;

    /* The state line is informational only; the race owns the socket
     * either way. Publishing FALLBACK_SIGNALING when a signalling leg is
     * running keeps the overlay/state machine honest about what is on the
     * wire, and the S3 nav deadlines key off terminal states, not this. */
    if (cfg.signal_leg) {
        set_state(DIRECT_P2P_FALLBACK_SIGNALING);
    }

    RaceResult res;
    p2p_race(&cfg, &res);

    if (signal_addr != NULL) {
        NET_UnrefAddress(signal_addr);
    }

    s_work.ev_deliver_any = res.deliver_any;
    s_work.ev_deliver_real = res.deliver_real;
    s_work.ev_challenge_any = res.challenge_any;
    s_work.ev_cookie_rechallenged = res.cookie_rechallenged; /* #105 */
    s_work.t_punch_ms = res.t_punch_ms;
    s_work.t_bilateral_ms = res.t_bilateral_ms;
    s_work.t_signal_ms = res.t_signal_ms;
    s_work.t_race_ms = res.t_race_ms;
    /* #36: carry the attribution evidence out with the timings. */
    s_work.race_confirm_seen = res.confirm_seen;
    s_work.race_confirm_ms = res.confirm_ms;
    s_work.ev_deliver_n = res.deliver_n;
    s_work.ev_challenge_n = res.challenge_n;
    s_work.ev_badver_n = res.badver_n;
    s_work.ev_deliver_gap_max_ms = res.deliver_gap_max_ms;
    s_work.ev_nack_any = res.nack_any;   /* #122 */
    s_work.ev_nack_reason = res.nack_reason;
    s_work.ev_nack_n = res.nack_n;

    if (res.outcome == RACE_CANCELLED || cancel_requested()) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Cancelled.");
        return DIRECT_P2P_IDLE;
    }

    if (res.outcome == RACE_PUNCHED) {
        /* Writeback BEFORE the HANDOFF publish (done by the join_thread_fn
         * wrapper on our return) — Tick reads s_work.peer_ip /
         * peer_public_port in join_tick_handoff, so the punched endpoint
         * must be visible by the time HANDOFF is observed. */
        SDL_strlcpy(s_work.peer_ip, res.peer_ip, sizeof(s_work.peer_ip));
        s_work.peer_public_port = res.peer_port;
        if (s_work.peer_code[0] != '\0') {
            Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_LAST_PEER_CODE, s_work.peer_code);
        }
        set_status("Connected!");
        return DIRECT_P2P_HANDOFF;
    }

    /* ---- nothing connected: classify -----------------------------------
     *
     * Precedence, unchanged from S3/S4 but now decided in one place
     * instead of at several different points in a serial cascade:
     *   1. PUNCH_AUTH — the peer's datagrams REACHED us and failed
     *      authentication. The peer was reached, so blaming NAT would send
     *      the user to their router for a version/payload mismatch.
     *   2. A fallback that could not be SET UP at all (bad URL, dead
     *      resolve) reports that.
     *   3. Otherwise the S3 evidence classifier.
     */
    Stun_CloseSocket(&s_work.stun);

    if (s_work.stun.diag_punch_bad_token) {
        set_fail(CONNECT_FAIL_PUNCH_AUTH);
        return DIRECT_P2P_FAILED_SYMMETRIC;
    }

    if (!fallback_ok) {
        /* The three up-front bypasses keep their pre-S6 verdicts and
         * their pre-S6 terminal state. */
        if (Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL)) {
            set_fail_msg(CONNECT_FAIL_NAT_BLOCKED,
                         "Direct punch failed (fallback disabled).");
        } else if (direct_p2p_is_lan_peer(s_work.peer_ip)) {
            set_fail_msg(CONNECT_FAIL_NAT_BLOCKED,
                         "LAN peer unreachable (check firewall).");
        } else {
            /* S3 cause 7: peer public IP == our public IP — same router,
             * and the punch just proved it lacks NAT loopback. */
            set_fail(CONNECT_FAIL_HAIRPIN);
        }
        return DIRECT_P2P_FAILED_SYMMETRIC;
    }

    if (signal_setup_fail != CONNECT_FAIL_NONE) {
        set_fail(signal_setup_fail);
        return DIRECT_P2P_FAILED_BILATERAL;
    }

    {
        /* S3 causes 3-6: silence from the server (which answers every
         * REGISTER) means the server/path is down; only-sentinel replies
         * mean the HOST never registered — offline or a stale code; a real
         * endpoint plus a failed punch is the NAT pair, upgraded to the
         * symmetric-both class by our own S2 port_disagreement signal. */
        ConnectJoinEvidence ev = { 0 };
        ev.deliver_any = res.deliver_any;
        ev.deliver_real = res.deliver_real;
        ev.challenge_any = res.challenge_any; /* S4c */
        ev.cookie_rechallenged = res.cookie_rechallenged; /* #105 */
        ev.bilateral_punched = false;
        ev.port_disagreement = s_work.stun.port_disagreement;
        ev.punch_bad_token = s_work.stun.diag_punch_bad_token; /* S4a */
        ev.nack_any = res.nack_any;       /* #122 */
        ev.nack_reason = res.nack_reason;
        const ConnectFailCode jc = ConnectFail_ClassifyJoin(&ev);
        /* #105: rechal is in the line because it is the bit that decides
         * between COOKIE_REJECTED and RENDEZVOUS_NOPAIR — a triager
         * reading a tester log must be able to see which branch was
         * taken, not just the verdict. */
        SDL_Log("[direct_p2p] joiner race exhausted (deliver_any=%d real=%d "
                "challenge_any=%d rechal=%d portdis=%d nack=%d:%s) -> %s",
                (int)res.deliver_any, (int)res.deliver_real,
                (int)res.challenge_any, (int)res.cookie_rechallenged,
                (int)s_work.stun.port_disagreement,
                (int)res.nack_any,
                res.nack_any ? Rendezvous_NackReasonText(res.nack_reason) : "none",
                ConnectFail_Code(jc));
        set_fail(jc);
        return DIRECT_P2P_FAILED_BILATERAL;
    }
}


/* Join worker: one join_attempt pass, plus the S2 auto-retry — on a
 * TERMINAL failure the joiner gets exactly ONE automatic full retry
 * with a FRESHLY BOUND local socket before the error is surfaced.
 * Rationale (docs/plan-netplay-connection.md §4): the first attempt can
 * die to transient causes a second pass fixes — the host was still in
 * its UPnP probe when our 2.5 s direct punch ran (start-skew), a first
 * packet lost while NAT mappings opened, or stuck conntrack/NAT state
 * pinned to the previous local port. join_attempt's failure paths all
 * close the attempt's socket, and each attempt re-runs STUN_DISCOVER
 * with local_port 0, so the retry naturally binds a fresh OS-assigned
 * port (dodging stale per-port NAT state).
 *
 * Non-retryable outcomes: DIRECT_P2P_IDLE (user cancel) and HANDOFF
 * (success). Everything terminal (FAILED_STUN / FAILED_SYMMETRIC /
 * FAILED_BILATERAL) retries once. */
static int SDLCALL join_thread_fn(void* data) {
    (void)data;

    /* See host_thread_fn for the rationale on this pre-NET_Init delay. */
    SDL_Delay(WORKER_STARTUP_DELAY_MS);

    /* Idempotent NET_Init — Stun_Discover assumes we've called it. */
    NET_Init();

    /* Task #76: a LOOP over JOIN_MAX_ATTEMPTS rather than an open-coded
     * second call. Behaviourally identical at JOIN_MAX_ATTEMPTS == 2, but
     * the attempt count is now a symbol the orchestrator worst case
     * multiplies by — adding a third attempt here can no longer leave the
     * nav deadline sized for two. */
    DirectP2PState outcome = DIRECT_P2P_IDLE;
    for (int attempt = 1; attempt <= JOIN_MAX_ATTEMPTS; attempt++) {
        /* #121: the port-map probe attempt 1 spawned has had attempt 1's
         * whole race to answer. Collect it with a ZERO-WAIT poll — this
         * call cannot block, by construction (join_portmap_collect) — and
         * bind its internal port if a mapping came back.
         *
         * Attempt 1 always passes 0, so its SOCKET is bound exactly as it
         * was before this task. That is the property the twelve passing
         * cells depend on: every one of them connects on attempt 1, and
         * attempt 1 still discovers, punches and races on an
         * OS-assigned ephemeral port. What attempt 1 does gain is a
         * thread spawn (join_portmap_spawn, and only when STUN reported
         * port_disagreement) — which touches no socket the race uses and
         * costs no wall-clock the race waits on. Stated precisely rather
         * than as "attempt 1 is unchanged", because it is not: it is
         * unchanged in the two respects that matter and different in one
         * that does not.
         *
         * This is the ONLY place the joiner cascade touches the probe on
         * its critical path, and it adds 0 ms to the derived deadline.
         * ORCH_JOIN_ATTEMPT_MS is unchanged and assert [E] below pins why
         * it had to be. */
        /* Publish the state attempt 2 is about to enter anyway. This is
         * tidiness, NOT the thing that makes the collect safe: DirectP2P_Tick
         * samples the state once per frame and a main thread that already
         * sampled FALLBACK_SIGNALING is not evicted by a store here. The
         * actual exclusion is s_join_portmap_busy, which
         * join_portmap_collect raises around the struct copy. */
        set_state(DIRECT_P2P_STUN_DISCOVER);
        const uint16_t bind_port = (attempt > 1) ? join_portmap_collect() : 0;

        outcome = join_attempt(bind_port);
        if (outcome == DIRECT_P2P_HANDOFF || outcome == DIRECT_P2P_IDLE) break;
        if (attempt < JOIN_MAX_ATTEMPTS) {
            SDL_Log("[direct_p2p] join attempt %d/%d failed (state %d) — one automatic "
                    "retry with a freshly bound socket",
                    attempt, JOIN_MAX_ATTEMPTS, (int)outcome);
            set_status("Retrying...");
        } else {
            SDL_Log("[direct_p2p] join retry failed too (state %d) — surfacing",
                    (int)outcome);
        }
    }
    /* #121: collect one last time, zero-wait, whatever the outcome.
     *
     * The loop above only collects at the TOP of attempt 2, so a probe
     * spawned by attempt 1 that succeeds after attempt 1 HANDOFFs is
     * never read — and by this design's own timing argument (a gateway
     * with a backend answers inside attempt 1's race) that is the LIKELY
     * case on the success path, not a corner. The gateway would be
     * holding a real mapping while s_upnp_mapping.active stayed false, so
     * direct_p2p_on_teardown would remove nothing and the user's router
     * would keep a one-hour hole for every successful symmetric join.
     *
     * Installing it here costs nothing and hands teardown something it
     * can actually clean up. Still zero-wait: a probe that has not
     * finished is detached and forgotten exactly as before, which is the
     * bounded leak the design already accepts. */
    (void)join_portmap_collect();

    set_state(outcome);
    return 0;
}

/* --- main-thread handoff ----------------------------------------------- */

/* Called from session teardown (via Netplay_SetSessionTeardownCallback).
 * Releases the UPnP mapping while the STUN socket is still bound so
 * miniupnpc can cleanly deregister. Safe to call when no mapping is
 * active. */
static void direct_p2p_on_teardown(void) {
    /* Signal any still-running worker(s) to exit, then join. The natural-
     * success exit path returns from the worker without nulling the
     * handle (5a switched the spawn sites away from SDL_DetachThread),
     * so teardown owns the join responsibility for both the orchestrator
     * worker and the new (5b/5c) rendezvous / bilateral-punch threads. */
    SDL_SetAtomicInt(&s_cancel, 1);
    SDL_SetAtomicInt(&s_rendezvous_cancel, 1);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 1);
    if (s_thread)                  { SDL_WaitThread(s_thread,                  NULL); s_thread                  = NULL; }
    if (s_rendezvous_thread)       { SDL_WaitThread(s_rendezvous_thread,       NULL); s_rendezvous_thread       = NULL; }
    if (s_bilateral_punch_thread)  { SDL_WaitThread(s_bilateral_punch_thread,  NULL); s_bilateral_punch_thread  = NULL; }
    /* #121: the join worker is joined above, so no spawn can race this.
     * Zero-wait, per join_portmap_collect's rule — teardown must not
     * inherit the 11,250 ms wait the joiner path exists to avoid. The
     * verdict feeds the removal decision below: a DETACHED probe is still
     * inside miniupnpc's cached-IGD globals, so removing the mapping
     * underneath it is a use-after-free, not merely untidy. */
    const bool join_portmap_quiescent = join_portmap_reset();

    /* Producer threads are joined; release any pending NET_Address refs
     * still in the rendezvous send queue (consumer never gets to drain
     * after teardown). */
    rend_q_purge();

    /* S1: reap any in-flight UPnP lease renewal BEFORE RemoveMapping so
     * miniupnpc's cached-IGD statics are never used concurrently. Bounded
     * (review H3); on detach-timeout the router-side removal is skipped —
     * see upnp_renew_join_and_discard. */
    bool upnp_quiescent = upnp_renew_join_and_discard() && join_portmap_quiescent;

    if (s_upnp_mapping.active) {
        if (upnp_quiescent) {
            portmap_remove(&s_upnp_mapping);
        }
        memset(&s_upnp_mapping, 0, sizeof(s_upnp_mapping));
    }
    /* Note: we do NOT destroy s_work.stun.socket here — ownership has
     * been transferred to netplay.c via Netplay_SetStunSocket and
     * netplay.c will destroy it immediately after this callback
     * returns. */
    s_work.stun.socket = NULL;
    /* S1: the ref'd STUN server address is still ours (do_handoff already
     * released it on the handoff path; this covers non-handoff teardowns). */
    Stun_ReleaseServerAddr(&s_work.stun);
    s_stun_keepalive_last_ms = 0;
    s_rebind_txid_valid = false;
    s_drift_pending_valid = false; /* M2: drop any half-confirmed drift */
    s_host_deliver_seen = false;   /* S3 */
    s_host_waiting_since_ms = 0;   /* S3 */
    s_host_waiting_last_note_ms = 0;
    s_host_advisory_code = CONNECT_FAIL_NONE;
    s_host_unauth_drops = 0; /* S4a */
    s_host_challenge_seen = false; /* S4c */
    host_punch_gate_reset(); /* S4-review HIGH-1b */
    signal_cookie_reset();         /* S4c */
    SDL_SetAtomicInt(&s_host_registering, 0);
    /* Reset state so the next BeginHost/BeginJoin starts clean.
     * R-1 exception: when the MIST handshake rejected the session, park
     * in FAILED_HANDSHAKE instead so the overlay keeps ERROR + the
     * reason (set_status by NotifySessionRejected) on screen after the
     * soft reset — an IDLE state would hide the overlay and the player
     * would see a silent drop with no explanation. Terminal like the
     * other FAILED_* states: cleared by DirectP2P_Cancel or process
     * restart (the MiSTer OSD retry path re-execs anyway). */
    if (s_handshake_reject_latched) {
        s_handshake_reject_latched = false;
        set_state(DIRECT_P2P_FAILED_HANDSHAKE);
    } else {
        set_state(DIRECT_P2P_IDLE);
    }
}

/* Finish the handoff on the main thread. Called from Tick when the
 * worker publishes DIRECT_P2P_HANDOFF (Join path) or when Tick itself
 * receives the first inbound datagram (Host path). */
#ifdef NETPLAY_TEST_HOOKS
/* Handoff observation seam. The only honest way to assert what an
 * establishment path produced is to observe do_handoff's own arguments.
 * Recording them here (rather than reading s_work) also catches a path
 * that writes s_work correctly but never reaches the handoff.
 * Main-thread only, like do_handoff itself. */
static char     s_test_handoff_ip[64] = { 0 };
static uint16_t s_test_handoff_port = 0;
static int      s_test_handoff_player = 0;
static int      s_test_handoff_count = 0;
#endif

static void do_handoff(int player, const char* peer_ip, uint16_t peer_port) {
    SDL_Log("[direct_p2p] Handoff to netplay: player=%d peer=%s:%u", player, peer_ip,
            (unsigned)peer_port);
#ifdef NETPLAY_TEST_HOOKS
    SDL_strlcpy(s_test_handoff_ip, peer_ip ? peer_ip : "", sizeof(s_test_handoff_ip));
    s_test_handoff_port = peer_port;
    s_test_handoff_player = player;
    s_test_handoff_count++;
#endif
    /* Netplay_SetParams wires remote_ip, local_port and the default
     * remote_port from (player, ip). The STUN socket we hand off below is a
     * plain datagram socket with no connected-peer state, so GekkoNet sends
     * go to whatever remote_ip:remote_port configure_gekko stringifies
     * (netplay.c:1556). For direct-P2P over the internet the real peer
     * endpoint is the STUN-translated peer_port from the hole-punch, not
     * SetParams' hardcoded 50000. Override remote_port here so outbound
     * Gekko frames reach the actual punched endpoint instead of oblivion. */
    Netplay_SetParams(player, peer_ip);
    Netplay_SetRemotePort(peer_port);
    Netplay_SetStunSocket(s_work.stun.socket);
    /* Task #119: hand the S4a punch token over with the socket so the
     * pre-session window keeps answering authenticated punches (the
     * late-punch rescue layer, late_punch.h). NOT a fifth site under THE
     * PROMISED LISTENING INTERVAL: that layer arms only after this
     * handoff, i.e. after the race returned and finished every leg by
     * its own rules — it can only ADD listening time, never end a leg. */
    Netplay_SetPunchToken(s_work.punch_token, s_work.punch_token_valid);
    /* Ownership transferred — prevent direct_p2p_on_teardown or a
     * subsequent Cancel from double-closing. */
    s_work.stun.socket = NULL;
    /* S1: keepalives stop at handoff (GekkoNet traffic keeps the mapping
     * warm from here) — drop the ref'd STUN server address now. */
    Stun_ReleaseServerAddr(&s_work.stun);
    s_rebind_txid_valid = false;
    s_drift_pending_valid = false; /* M2: drop any half-confirmed drift */
    /* netplay_nav owns the Netplay_BeginDirectP2P() call now. Netplay_
     * SetParams just populated remote_ip; nav's NAV_WAIT_ORCHESTRATOR
     * was gating on Netplay_IsRemoteIpSet() and will advance to
     * NAV_START_NETPLAY on its next tick. If menu-nav completes first
     * (fast path on host, orchestrator still hole-punching on joiner)
     * nav simply stays in NAV_WAIT_ORCHESTRATOR until we land here. */
    SDL_Log("[direct_p2p] handoff complete — nav state machine will start netplay session");
}

/* --- S1 host liveness: STUN rebind keepalive + drift detection --------- */

/* Called from Tick's HOST_WAITING branch (main thread — the socket's
 * sole I/O actor in that phase). Every CFG_KEY_NETPLAY_DIRECT_P2P_STUN_
 * KEEPALIVE_MS (default 20 s; <= 0 disables) re-issues a STUN Binding
 * Request on the shared socket toward the server that answered
 * Stun_Discover. Two effects:
 *   (a) the outbound datagram refreshes the host's NAT mapping — for a
 *       non-UPnP host this is the very mapping the room code
 *       advertises, which otherwise decays at the router's UDP idle
 *       timeout while the host sits waiting;
 *   (b) the response (handled in host_handle_stun_rebind via
 *       host_tick_receive's STUN gate) reveals whether the NAT rebound
 *       the mapping — i.e. whether the displayed room code went stale. */
static void host_stun_keepalive_tick(void) {
    int interval_ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_STUN_KEEPALIVE_MS);
    if (interval_ms <= 0) return; /* disabled */
    if (interval_ms < 5000) interval_ms = 5000; /* don't spam public STUN */
    uint64_t now = SDL_GetTicks();
    if (s_stun_keepalive_last_ms == 0) {
        /* Arm on first HOST_WAITING tick; Stun_Discover just probed, so
         * the first keepalive is due one full interval from now. */
        s_stun_keepalive_last_ms = now;
        return;
    }
    if (now - s_stun_keepalive_last_ms < (uint64_t)interval_ms) return;
    s_stun_keepalive_last_ms = now;
    if (Stun_SendKeepalive(&s_work.stun, s_rebind_txid)) {
        s_rebind_txid_valid = true;
    }
}

/* Cancel and JOIN the rendezvous re-REGISTER worker. Safe to call when
 * no worker is running. Callers MUST do this before mutating any s_work
 * field the worker's session-key derivation reads (public_ip /
 * advertised_port / nonce). */
static void host_rendezvous_stop(void) {
    if (s_rendezvous_thread != NULL) {
        SDL_SetAtomicInt(&s_rendezvous_cancel, 1);
        SDL_WaitThread(s_rendezvous_thread, NULL);
        s_rendezvous_thread = NULL;
    }
}

/* Respawn the rendezvous worker under whatever session key s_work now
 * implies, honoring the same kill switch as the original spawn in
 * host_thread_fn step 5. `why` names the caller in the failure log. */
static void host_rendezvous_restart(const char* why) {
    if (Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL)) {
        return;
    }
    SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
    s_rendezvous_thread = SDL_CreateThread(host_rendezvous_thread_fn,
                                           "DirectP2PRendezvous", NULL);
    if (s_rendezvous_thread == NULL) {
        SDL_Log("[direct_p2p] WARNING: failed to respawn rendezvous thread "
                "%s; fallback disabled this session", why);
    }
}

/* Handle a STUN Binding Success Response received on the host socket
 * during HOST_WAITING (main thread). If the mapped endpoint matches the
 * last known public endpoint the NAT mapping is stable — nothing to do.
 * If it DIFFERS, the NAT rebound and the displayed room code is stale.
 *
 * Chosen behavior (documented in docs/plan-netplay-connection.md S1):
 * re-encode and display the NEW code, plus an explicit status line
 * ("Network changed! Share the NEW code."). Rationale: silently
 * continuing would leave a code on screen that can never connect; the
 * least surprising outcome for a user staring at a code they already
 * shared is a visible, explained code change — the old code was already
 * dead the moment the NAT rebound, so there is nothing to preserve.
 *
 * Ordering: the rendezvous thread derives its session key from
 * (public_ip, advertised_port), so it is cancelled and JOINED before
 * s_work is mutated, then respawned under the new key. Join latency is
 * ~50 ms (the thread's inner tick) — acceptable for a rare event on a
 * menu screen.
 *
 * Review M2: commits are DEBOUNCED — a drift must be confirmed by two
 * consecutive keepalives reporting the same new endpoint before the
 * code is rewritten (see s_drift_pending_* for the rationale and the
 * rebind-every-interval no-churn property). */
static void host_handle_stun_rebind(const uint8_t* buf, int len) {
    if (!s_rebind_txid_valid) {
        return; /* no keepalive in flight — stale/foreign response */
    }
    char ip[64] = { 0 };
    uint16_t port = 0;
    if (!Stun_ParseBindingResponse(buf, len, s_rebind_txid, ip, sizeof(ip), &port)) {
        return; /* wrong transaction / malformed — ignore */
    }
    s_rebind_txid_valid = false;

    if (port == s_work.stun.public_port &&
        direct_p2p_ip_eq_normalized(ip, s_work.stun.public_ip)) {
        s_drift_pending_valid = false; /* stable again — drop any candidate */
        return; /* mapping stable — keepalive did its NAT-refresh job */
    }

    /* Review M2 debounce: require a second consecutive keepalive to
     * confirm the SAME new endpoint before rewriting the displayed
     * code. First sighting (or a candidate that keeps changing — the
     * rebind-every-interval NAT) only records/replaces the candidate. */
    if (!s_drift_pending_valid || port != s_drift_pending_port ||
        !direct_p2p_ip_eq_normalized(ip, s_drift_pending_ip)) {
        SDL_strlcpy(s_drift_pending_ip, ip, sizeof(s_drift_pending_ip));
        s_drift_pending_port = port;
        s_drift_pending_valid = true;
        SDL_Log("[direct_p2p] STUN rebind drift CANDIDATE %s:%u (current %s:%u) — "
                "awaiting confirmation on the next keepalive",
                ip, (unsigned)port,
                s_work.stun.public_ip, (unsigned)s_work.stun.public_port);
        return;
    }
    s_drift_pending_valid = false;

    SDL_Log("[direct_p2p] STUN rebind drift CONFIRMED: public endpoint changed "
            "%s:%u -> %s:%u — displayed room code is stale",
            s_work.stun.public_ip, (unsigned)s_work.stun.public_port,
            ip, (unsigned)port);

    host_commit_endpoint(ip, port, "after endpoint drift");
}

/*
 * Re-publish the advertised tuple: recompute the advertised port from
 * (STUN endpoint, live port mapping), re-encode the room code, re-derive
 * the punch token, and restart the rendezvous loop under the new key.
 *
 * Extracted from the drift handler in S7 so the OTHER event that
 * invalidates the advertised port can use it too: a port mapping whose
 * lease expired without a successful renewal (review H-5 / M-5.5). Both
 * callers are the main thread in a HOST_WAITING-family state.
 */
static void host_commit_endpoint(const char* ip, uint16_t port, const char* why) {
    /* Stop the rendezvous re-REGISTER loop before mutating the fields it
     * reads (public_ip / advertised_port). */
    host_rendezvous_stop();

    SDL_strlcpy(s_work.stun.public_ip, ip, sizeof(s_work.stun.public_ip));
    s_work.stun.public_port = port;

    /* Recompute the advertised tuple. A live port mapping pins the
     * advertised PORT (the router forwards it regardless of the STUN
     * mapping), so only the IP component can drift there; without one
     * both components track the STUN-observed endpoint.
     *
     * S7 review H-5: the test is portmap_mapping_usable(), not
     * `active` — a mapping whose lease has run out under a failing
     * renewal is still flagged active but forwards nothing, and pinning
     * its port here is precisely how a stale room code outlives the
     * mapping it describes. */
    uint16_t new_adv_port = portmap_mapping_usable() ? s_upnp_mapping.external_port : port;
    uint32_t ip_be = ipv4_str_to_be(ip);
    char new_code[ROOM_CODE_BUF_LEN];
    /* v4 (task #155): s_work.nonce is always ROOM_CODE_V4_FIXED_NONCE —
     * kept only so the derivation calls below have one place both roles
     * agree on. */
    if (ip_be != 0 && RoomCode_Encode(ip_be, new_adv_port, new_code) &&
        strcmp(new_code, s_work.host_code) != 0) {
        char old_redacted[ROOM_CODE_BUF_LEN];
        char new_redacted[ROOM_CODE_BUF_LEN];
        RoomCode_Redact(s_work.host_code, old_redacted);
        RoomCode_Redact(new_code, new_redacted);
        SDL_Log("[direct_p2p] room code re-encoded: %s -> %s",
                old_redacted, new_redacted);
        SDL_strlcpy(s_work.host_code, new_code, sizeof(s_work.host_code));
        set_status("Network changed! Share the NEW code.");
        /* Task #157: a drift re-encode replaces the code without a state
         * transition, so set_state's netplay.status hook never sees it —
         * refresh the file here. Only while HOST_WAITING: in the
         * FALLBACK_* / HANDOFF states the file already says IDLE and a
         * launcher must not read a code nobody is advertising (the
         * bilateral-failure return to HOST_WAITING republishes the fresh
         * s_work.host_code through set_state). */
        if (get_state() == DIRECT_P2P_HOST_WAITING) {
            netplay_status_publish(true, s_work.host_code);
        }
    }
    s_work.advertised_port = new_adv_port;

    /* S4a: the punch token is derived from the advertised tuple, so a
     * drift-committed endpoint means a NEW token (the re-encoded code
     * the user shares carries the new payload; the joiner derives from
     * that). Fail closed on derivation failure — better to ignore
     * punches than to accept unauthenticated ones. */
    s_work.punch_token_valid =
        Rendezvous_DerivePunchToken(ip_be, new_adv_port, s_work.nonce, s_work.punch_token);
    if (!s_work.punch_token_valid) {
        SDL_Log("[direct_p2p] WARNING: punch-token re-derivation failed %s — punches "
                "will be ignored until re-host", why);
    }

    /* Respawn the rendezvous loop under the new session key (same kill
     * switch as the original spawn in host_thread_fn step 5). */
    host_rendezvous_restart(why);
}

/*
 * S4-review HIGH-1b's "room-code re-roll" — v4 (task #155):
 * DORMANT-BUT-COMPILED, NOT WIRED. Through v3 this ran once the punch
 * gate absorbed HOST_PUNCH_TOTAL_REROLL session-total bad-token
 * punches: draw a fresh nonce, re-encode the room code, re-derive the
 * punch token, clear every source's mute (host_punch_gate_clear_mutes)
 * and tell the user — invalidating whatever an attacker had been
 * grinding for.
 *
 * It has no v4 analog, and — this is the part that matters — restoring
 * it AS IT WAS would be actively harmful, not merely useless. The token
 * is now `f(ip, port)` with a FIXED nonce (room_code.h's "nonce leaves
 * the code" section), so re-encoding the SAME (ip, port) below can only
 * ever reproduce a BYTE-IDENTICAL code, key and token — there is
 * nothing left to roll. If this were still invoked from
 * host_punch_gate_note_bad's old threshold, the mute-clear at the end
 * would fire on every HOST_PUNCH_TOTAL_REROLL charged bad punches
 * against that same unchanged code: an attacker grinding wrong tokens
 * would launder their own mute every 64 datagrams for free. That is a
 * mute bypass, not a defense, which is why the call site that used to
 * live in host_tick_receive_one is gone — the per-source mute
 * (host_punch_gate_note_bad / host_punch_gate_is_muted above) is the
 * sole backstop from here.
 *
 * Declared non-static (see direct_p2p.h) rather than parked behind #if
 * — #if is what let LOSSY_ADAPTER silently stop compiling once the
 * warning set tightened (4ca4c0de), and dormant-but-built is the
 * requirement here too. Kept for the day a room-list feature restores
 * an out-of-band nonce (room_code.h): re-wiring this and
 * host_punch_gate_clear_mutes becomes correct again at that point, and
 * they are re-wired TOGETHER — never one without the other, for exactly
 * the mute-bypass reason above.
 *
 * Adapted (not verbatim) from the pre-v4 body: RoomCode_Encode dropped
 * its nonce parameter when the code stopped carrying one, so the call
 * below passes 2 args instead of 3. Rendezvous_DerivePunchToken kept
 * its nonce parameter (rendezvous.h) for the same future-room-list
 * reason as host_punch_gate_clear_mutes above, so that call is
 * unchanged.
 */
void host_reroll_room_code(void) {
    const uint32_t ip_be = ipv4_str_to_be(s_work.stun.public_ip);
    if (ip_be == 0) {
        return;
    }

    uint32_t new_nonce = 0;
    if (!RoomCode_GenerateNonce(&new_nonce)) {
        SDL_Log("[direct_p2p] punch-gate re-roll SKIPPED: CSPRNG unavailable — "
                "keeping the current room code (a predictable nonce would be "
                "worse than the flood)");
        /* Do not retry on every datagram: park the counter just under the
         * threshold so the next bad punch re-arms rather than spinning. */
        s_host_punch_bad_total = HOST_PUNCH_TOTAL_REROLL - 1;
        return;
    }

    char new_code[ROOM_CODE_BUF_LEN];
    uint8_t new_token[STUN_PUNCH_TOKEN_LEN];
    if (!RoomCode_Encode(ip_be, s_work.advertised_port, new_code) ||
        !Rendezvous_DerivePunchToken(ip_be, s_work.advertised_port, new_nonce,
                                     new_token)) {
        SDL_Log("[direct_p2p] punch-gate re-roll SKIPPED: re-encode/derive "
                "failed — keeping the current room code");
        s_host_punch_bad_total = HOST_PUNCH_TOTAL_REROLL - 1;
        return;
    }

    host_rendezvous_stop();

    s_work.nonce = new_nonce;
    SDL_strlcpy(s_work.host_code, new_code, sizeof(s_work.host_code));
    memcpy(s_work.punch_token, new_token, sizeof(s_work.punch_token));
    s_work.punch_token_valid = true;

    s_host_punch_rerolls++;
    s_host_punch_bad_total = 0;
    host_punch_gate_clear_mutes();

    SDL_Log("[direct_p2p] %s punch-gate RE-ROLL #%d/%d after %d bad-token "
            "punches — room code regenerated (every source mute cleared)",
            ConnectFail_Code(CONNECT_FAIL_PUNCH_AUTH),
            s_host_punch_rerolls, HOST_PUNCH_REROLL_MAX,
            HOST_PUNCH_TOTAL_REROLL);

    /* The user is staring at a code they may already have read out. Say
     * so explicitly on the overlay status line — same mechanism and the
     * same wording shape as the STUN-drift re-encode above. */
    set_status("Code was being probed. Share the NEW code.");

    host_rendezvous_restart("after a punch-gate re-roll");
}

/*
 * DELIVER handler — invoked from host_tick_receive after the magic-byte
 * gate. Parses the packet against our session key (derived from our own
 * public endpoint, same as the rendezvous thread). On a valid DELIVER
 * with a non-zero peer endpoint, transitions out of HOST_WAITING per
 * the bilateral fallback flow:
 *   - Self-DELIVER (peer public IP == our public IP) => ignore and stay
 *     HOST_WAITING with the resender alive (review H1 — it is our own
 *     stale registration or a spoof; a legitimate joiner hairpin-bails
 *     client-side before ever REGISTERing).
 *   - LAN peer => stay HOST_WAITING (we should have seen a direct punch
 *     already; rendezvous via public infra is the wrong path).
 *   - Otherwise => stash peer endpoint, signal rendezvous-cancel,
 *     transition to FALLBACK_BILATERAL_PUNCH and spawn the
 *     bilateral-punch worker thread.
 *
 * Returns true iff the packet was a recognized DELIVER (whether or not
 * we acted on it) so the caller knows it consumed the datagram.
 */
static bool try_handle_deliver(const uint8_t* pkt, int len) {
    /* Only honor DELIVER while we're still in HOST_WAITING. After we've
     * transitioned to FALLBACK_BILATERAL_PUNCH (or any terminal) a stale
     * DELIVER from the server is a no-op. */
    if (get_state() != DIRECT_P2P_HOST_WAITING) {
        return true;
    }

    /* Derive the session key from our own ADVERTISED endpoint — the same
     * derivation the rendezvous thread does and the joiner does (from the
     * decoded room code). See the advertised_port field comment. */
    uint32_t ip_be = ipv4_str_to_be(s_work.stun.public_ip);
    uint8_t session_key[16];
    if (!Rendezvous_DeriveSessionKey(ip_be, s_work.advertised_port, s_work.nonce,
                                     session_key)) {
        SDL_Log("[direct_p2p] DELIVER drop: failed to derive session key");
        return true;
    }

    char peer_ip[64] = { 0 };
    uint16_t peer_port = 0;
    const RendezvousDeliverResult dr =
        Rendezvous_ParseDeliverEx(pkt, len, session_key, peer_ip, &peer_port);
    if (dr != REND_DELIVER_MALFORMED) {
        /* S3 cause-8 evidence: the server answers EVERY REGISTER with a
         * DELIVER (zero-sentinel while unpaired), so seeing ANY valid
         * DELIVER proves the rendezvous path is alive. Consumed by the
         * host-waiting advisory in Tick. Main thread — no atomics. */
        s_host_deliver_seen = true;
    }
    if (dr != REND_DELIVER_PEER) {
        /* Malformed, wrong session, or "peer not yet registered" sentinel —
         * any of which the rendezvous thread will retry past. No action. */
        return true;
    }

    SDL_Log("[direct_p2p] DELIVER received peer=%s:%u", peer_ip, (unsigned)peer_port);

    /* Self-DELIVER gate (review H1): a DELIVER whose peer IP equals our
     * OWN public IP is our own stale registration echoed back, not a
     * peer. Scenario: host presses Host, cancels, re-hosts within the
     * server TTL. With UPnP the external port is pinned so the session
     * key is unchanged; if the new socket's NAT mapping toward the
     * server picked a different source port, the server sees stale slot
     * A + empty slot B, makes us B, and its reply DELIVER carries our
     * own old endpoint. Pre-fix this hit the terminal hairpin gate
     * below and killed the re-hosted room with FAILED_SYMMETRIC for the
     * whole 10-minute TTL.
     *
     * A LEGITIMATE joiner can never produce a same-IP DELIVER: the
     * joiner's own hairpin bypass (join_thread_fn, the
     * ip_eq_normalized(peer_ip, own public_ip) check before
     * FALLBACK_SIGNALING) fails it with FAILED_SYMMETRIC before it ever
     * REGISTERs with the rendezvous server. So every same-IP DELIVER is
     * either our own stale slot (any old NAT port — we cannot know
     * which, so we ignore ALL same-IP ports, not just advertised_port)
     * or a spoofed REGISTER; in both cases the right move is to keep
     * waiting, and to keep the rendezvous resender ALIVE so our
     * re-REGISTERs eventually refresh/reclaim the slot server-side.
     * This subsumes the old terminal hairpin gate, whose only reachable
     * firing case was exactly this self-DELIVER poison. */
    if (direct_p2p_ip_eq_normalized(peer_ip, s_work.stun.public_ip)) {
        SDL_Log("[direct_p2p] DELIVER carries our own IP (%s:%u, advertised port %u) "
                "— stale self-registration or spoof; ignoring and staying HOST_WAITING.",
                peer_ip, (unsigned)peer_port, (unsigned)s_work.advertised_port);
        return true;
    }

    /* Stop the rendezvous resender — we have what we need from the server. */
    SDL_SetAtomicInt(&s_rendezvous_cancel, 1);

    /* LAN bypass: if the rendezvous-delivered peer is on a private
     * subnet, we should have seen a direct punch already. Something is
     * weird (firewall blocking the LAN path? mismatched configs?). Stay
     * in HOST_WAITING and let the user retry / the direct path catch up. */
    if (direct_p2p_is_lan_peer(peer_ip)) {
        SDL_Log("[direct_p2p] DELIVER peer is LAN (%s); staying HOST_WAITING — "
                "direct path should already have completed.", peer_ip);
        return true;
    }

    /* Stash the peer endpoint so the bilateral-punch thread can copy
     * stack-locals from it. The thread overwrites these on success
     * before transitioning to HANDOFF. */
    SDL_strlcpy(s_work.peer_ip, peer_ip, sizeof(s_work.peer_ip));
    s_work.peer_public_port = peer_port;

    /* Transition + spawn the bilateral-punch worker. The thread itself
     * publishes HANDOFF / FAILED_BILATERAL when it completes. */
    set_state(DIRECT_P2P_FALLBACK_BILATERAL_PUNCH);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 0);
    /* Natural-success guard: if a previous bilateral-punch worker exited
     * without being detached, join it now before clobbering the handle. */
    if (s_bilateral_punch_thread != NULL) {
        SDL_WaitThread(s_bilateral_punch_thread, NULL);
        s_bilateral_punch_thread = NULL;
    }
    s_bilateral_punch_thread = SDL_CreateThread(host_bilateral_punch_thread_fn,
                                                "DirectP2PBilateral", NULL);
    if (s_bilateral_punch_thread == NULL) {
        SDL_Log("[direct_p2p] failed to spawn bilateral-punch thread");
        set_fail_msg(CONNECT_FAIL_INTERNAL, "Connection failed. Try again.");
        set_state(DIRECT_P2P_FAILED_BILATERAL);
    }
    return true;
}

/* S4c: server CHALLENGE handler — invoked from host_tick_receive's
 * rendezvous arm (main thread) when the '3SXR' frame's type byte is
 * CHALLENGE. Parses the cookie against our session key, publishes it
 * for the rendezvous worker's periodic resends, and IMMEDIATELY sends
 * one cookie'd REGISTER back to the frame's source (the signal server)
 * so slot binding costs one RTT instead of waiting out the worker's
 * next 5 s cadence. Main thread owns the socket in HOST_WAITING, so
 * the direct send is race-free. */
static void host_handle_challenge(const uint8_t* pkt, int len,
                                  NET_Address* src_addr, uint16_t src_port) {
    if (get_state() != DIRECT_P2P_HOST_WAITING) {
        return; /* stale challenge after a transition — ignore */
    }
    uint32_t ip_be = ipv4_str_to_be(s_work.stun.public_ip);
    uint8_t session_key[16];
    if (!Rendezvous_DeriveSessionKey(ip_be, s_work.advertised_port, s_work.nonce,
                                     session_key)) {
        return;
    }
    uint8_t cookie[REND_COOKIE_LEN];
    if (!Rendezvous_ParseChallenge(pkt, len, session_key, cookie)) {
        SDL_Log("[direct_p2p] CHALLENGE drop: bad frame or wrong session key");
        return;
    }
    s_host_challenge_seen = true;
    signal_cookie_publish(cookie);
    uint8_t register_pkt[REND_REGISTER_PKT_LEN];
    if (Rendezvous_BuildRegister(s_work.stun.public_port, session_key, cookie,
                                 register_pkt) &&
        s_work.stun.socket != NULL && src_addr != NULL) {
        /* Routed through the RENDEZVOUS_SEND seam (not a direct
         * Rendezvous_Send) so test_bilateral_punch.c can OBSERVE the
         * cookied echo: its `my_public_port` field must carry
         * s_work.stun.public_port (the source port the server checks
         * against rinfo.port) and NOT s_work.advertised_port, which
         * differs whenever UPnP maps an external port the STUN probe
         * never saw. Production (no -DNETPLAY_TEST_HOOKS) expands to the
         * identical direct Rendezvous_Send call. */
        (void)RENDEZVOUS_SEND(s_work.stun.socket, src_addr, src_port,
                              register_pkt, sizeof(register_pkt));
    }
    SDL_Log("[direct_p2p] rendezvous CHALLENGE answered (cookie echoed to %s:%u)",
            src_addr != NULL ? NET_GetAddressString(src_addr) : "?",
            (unsigned)src_port);
}

/* #150: how many datagrams one HOST_WAITING frame may drain. The old
 * shape was ONE per frame (~60/s at the frame rate), and the punch gate
 * mutes only punch-SHAPED traffic — so arbitrary garbage aimed at the
 * advertised endpoint (stable and knowable to a past opponent whenever
 * UPnP mapped it) consumed the frame's single receive and could park a
 * legitimate joiner's punch behind up to 256 KB of queue
 * (NetTuning_SetRecvBuf) for minutes. Pre-pairing only. 16/frame is
 * ~960/s: a full 256 KB queue of minimal datagrams (~15 k) drains in
 * ~16 s instead of ~250 s, the legitimate joiner's ~10-punch opening
 * burst fits in one frame, and the per-frame cost stays bounded at 16
 * classify calls (memcmp-sized) plus rate-limited logging — the same
 * order of work as the rendezvous queue's 4-slot drain beside it. This
 * is a drain-rate bound, not a flood-proof: a sender sustaining more
 * than ~960 datagrams/s can still delay the joiner, which is the
 * accepted residual (the race loop's greedy drain is not available
 * here — HOST_WAITING is unbounded by design and must not spin). */
#define HOST_TICK_RECV_BATCH 16

/* Host-side receive routing (the per-frame drain, host_tick_receive
 * below, loops over this). The first ACCEPTED
 * inbound packet on a direct-P2P Host socket is the joiner's
 * authenticated Stun_HolePunch probe ("3SX_PUNCH" + the 8-byte
 * code-derived token, S4a). We echo the validated payload back to the
 * source so the joiner's Stun_HolePunch loop sees an authenticated
 * response and returns success — otherwise the joiner would time out
 * and flag FAILED_SYMMETRIC even though connectivity is established.
 * Anything that fails the datagram gate is dropped WITHOUT consuming
 * the peer slot (see classify_host_datagram).
 *
 * #150: host_tick_receive drains up to HOST_TICK_RECV_BATCH datagrams
 * per call instead of one; this helper handles exactly ONE. The batch
 * loop stops the moment the state leaves HOST_WAITING (a DELIVER can
 * transition it) or a peer is captured — later datagrams in the queue
 * belong to the session and must be left for the socket's next owner. */
typedef enum {
    HOST_RECV_EMPTY = 0,  /* nothing queued — the frame's drain is done   */
    HOST_RECV_CONSUMED,   /* one non-peer datagram handled; may drain on  */
    HOST_RECV_STOP,       /* peer captured or state transitioned — stop   */
} HostRecvOne;

static HostRecvOne host_tick_receive_one(void) {
    NET_Datagram* dgram = NULL;
    if (!NET_ReceiveDatagram(s_work.stun.socket, &dgram) || dgram == NULL) {
        return HOST_RECV_EMPTY;
    }
    /* S4a: one routing decision per datagram (classify_host_datagram).
     * Only an AUTHENTICATED punch — "3SX_PUNCH" + the 8-byte token both
     * sides derive from the room-code payload — may be accepted as the
     * peer. Pre-S4a, ANY non-'3SXR'/non-STUN datagram was captured as
     * the peer and echoed: session hijack for anyone who saw/guessed
     * the advertised endpoint, and permanent slot loss to a single
     * stray packet (the real joiner then failed with a misleading
     * "opponent build may be too old" after the MIST silence window). */
    switch (classify_host_datagram(dgram->buf, dgram->buflen,
                                   s_work.punch_token, s_work.punch_token_valid)) {
    case DP2P_HOST_DGRAM_RENDEZVOUS:
        /* Server->host frame ('3SXR'). Hosts only ever receive DELIVER
         * or (S4c) CHALLENGE; REGISTER/POLL are client->server.
         * Dispatch on the type byte — DELIVER pairs, CHALLENGE arms the
         * return-routability cookie. */
        if (dgram->buflen >= 6 && dgram->buf[5] == 4 /* REND_TYPE_CHALLENGE */) {
            host_handle_challenge(dgram->buf, dgram->buflen,
                                  dgram->addr, dgram->port);
        } else {
            try_handle_deliver(dgram->buf, dgram->buflen);
        }
        NET_DestroyDatagram(dgram);
        /* #150: a DELIVER can have paired us and left HOST_WAITING; any
         * further queued datagrams belong to the session then. A
         * CHALLENGE keeps waiting, and the batch may drain on. */
        return get_state() == DIRECT_P2P_HOST_WAITING ? HOST_RECV_CONSUMED
                                                      : HOST_RECV_STOP;

    case DP2P_HOST_DGRAM_STUN:
        /* S1: keepalive replies / late Stun_Discover duplicates route
         * to the drift detector, never to the peer capture. */
        host_handle_stun_rebind(dgram->buf, dgram->buflen);
        NET_DestroyDatagram(dgram);
        return HOST_RECV_CONSUMED; /* not a peer — keep waiting */

    case DP2P_HOST_DGRAM_IGNORE: {
        /* Unauthenticated / unrecognized datagram: drop it, do NOT
         * echo, and KEEP WAITING — the peer slot is not consumed.
         * Rate-limited advisory so a scan/legacy-build burst is visible
         * in the log without flooding it. A punch-shaped payload with a
         * wrong token is called out as probable version mismatch. */
        const bool punch_shaped =
            Stun_HasPunchPrefix(dgram->buf, dgram->buflen);
        const bool chargeable = host_ignore_is_chargeable(
            dgram->buf, dgram->buflen, s_work.punch_token,
            s_work.punch_token_valid);
        const bool authed_probe = punch_shaped && !chargeable;
        char src_ip[64];
        SDL_strlcpy(src_ip, NET_GetAddressString(dgram->addr), sizeof(src_ip));

        s_host_unauth_drops++;
        if (s_host_unauth_drops == 1 || (s_host_unauth_drops % 50) == 0) {
            SDL_Log("[direct_p2p] ADVISORY code=%s ignored unauthenticated datagram "
                    "#%d from %s:%u (len=%d%s) — still waiting for an authenticated "
                    "punch",
                    ConnectFail_Code(CONNECT_FAIL_PUNCH_AUTH),
                    s_host_unauth_drops, src_ip, (unsigned)dgram->port,
                    dgram->buflen,
                    !punch_shaped ? ""
                        : authed_probe
                            ? ", authenticated late-punch probe: not charged"
                            : ", punch-shaped: peer build too old or wrong code");
        }

        /* S4-review HIGH-1b: only a punch-SHAPED datagram that failed
         * the token check is a gate probe. Arbitrary garbage (port
         * scans, stray traffic) is logged above but deliberately NOT
         * charged — it teaches an attacker nothing. An authenticated
         * late-punch probe is exempt for the same reason: it proves the
         * token rather than guessing at it (host_ignore_is_chargeable). */
        if (chargeable) {
            host_punch_gate_note_bad(src_ip, SDL_GetTicks());
        }
        NET_DestroyDatagram(dgram);
        return HOST_RECV_CONSUMED; /* keep waiting */
    }

    case DP2P_HOST_DGRAM_PEER_PUNCH:
        /* S4-review HIGH-1b: the token compare has ALREADY run and
         * passed, but if this source has been muted for grinding the
         * gate we refuse to ANSWER. Dropping here (rather than skipping
         * the classify) is what closes the oracle: the sender gets no
         * echo and no handoff, so a correct guess is indistinguishable
         * from a wrong one. Infrastructure arms ('3SXR', STUN) are
         * untouched by the mute. */
        if (host_punch_gate_is_muted(NET_GetAddressString(dgram->addr),
                                     SDL_GetTicks())) {
            s_host_punch_muted_drops++;
            if (s_host_punch_muted_drops == 1 ||
                (s_host_punch_muted_drops % 50) == 0) {
                SDL_Log("[direct_p2p] %s punch-gate suppressed an AUTHENTICATED "
                        "punch from muted source %s:%u (#%d) — that address "
                        "burned its budget guessing; it recovers when the mute "
                        "expires",
                        ConnectFail_Code(CONNECT_FAIL_PUNCH_AUTH),
                        NET_GetAddressString(dgram->addr),
                        (unsigned)dgram->port, s_host_punch_muted_drops);
            }
            NET_DestroyDatagram(dgram);
            return HOST_RECV_CONSUMED; /* keep waiting */
        }
        break; /* authenticated peer — fall through to the capture below */
    }

    /* Capture the source endpoint — this is who we'll talk to. The
     * socket's internal "connected peer" state gets set when
     * configure_gekko wraps it into SDLNetAdapter; for the orchestrator
     * the source-IP:port from the first packet is the peer's public
     * endpoint (post-NAT translation, which is what we need for sends). */
    char src_ip[64];
    SDL_strlcpy(src_ip, NET_GetAddressString(dgram->addr), sizeof(src_ip));
    uint16_t src_port = dgram->port;

    /* Echo the (already-validated) punch payload back — Stun_HolePunch
     * on the joiner requires the byte-identical authenticated payload
     * from the expected peer, so echoing what we just validated
     * authenticates US to the joiner in the same exchange. */
    (void)NET_SendDatagram(s_work.stun.socket, dgram->addr, src_port,
                           dgram->buf, dgram->buflen);
    NET_DestroyDatagram(dgram);

    /* Stash for debug-logging; the handoff itself only needs ip. */
    SDL_strlcpy(s_work.peer_ip, src_ip, sizeof(s_work.peer_ip));
    s_work.peer_public_port = src_port;

    SDL_Log("[direct_p2p] Host received first inbound from %s:%u", src_ip, src_port);
    set_status("Connected!");
    set_state(DIRECT_P2P_HANDOFF);

    /* Host is player 1 (player_number = 0). */
    do_handoff(1, src_ip, src_port);
    return HOST_RECV_STOP;
}

/* The per-frame drain (see host_tick_receive_one above for the routing
 * and HOST_TICK_RECV_BATCH for the bound's sizing). Returns true when a
 * datagram ended the wait (peer captured / state transitioned). */
static bool host_tick_receive(void) {
    if (s_work.stun.socket == NULL) {
        return false;
    }
    for (int drained = 0; drained < HOST_TICK_RECV_BATCH; drained++) {
        switch (host_tick_receive_one()) {
        case HOST_RECV_EMPTY:
            return false;
        case HOST_RECV_STOP:
            return true;
        case HOST_RECV_CONSUMED:
        default:
            break; /* drain on */
        }
    }
    return false; /* batch bound reached — the rest waits for next frame */
}

/* Join-side tick: the worker already completed punch and published
 * HANDOFF; Tick executes the main-thread handoff. */
static void join_tick_handoff(void) {
    /* Join is player 2. */
    do_handoff(2, s_work.peer_ip, s_work.peer_public_port);
}

/* --- public API -------------------------------------------------------- */

void DirectP2P_Init(void) {
    if (s_init_done) return;
    SDL_SetAtomicInt(&s_state, (int)DIRECT_P2P_IDLE);
    SDL_SetAtomicInt(&s_cancel, 0);
    SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_handoff_pending, 0);
    SDL_SetAtomicInt(&s_bilateral_failed, 0);
    s_bilateral_fail_count = 0; /* M1: fresh retry budget per hosting session */
    s_host_stun_retry_count = 0; /* S2: fresh host STUN-retry budget */
    s_host_stun_retry_at_ms = 0;
    /* #96: the port-map FAILURE verdict has exactly the ladder's own
     * lifetime — same reset sites as the retry counter above. Narrower
     * than s_upnp_mapping on purpose: a user action re-probes. */
    s_portmap_failed_port = 0;
    /* #121: the joiner probe's one-spawn-per-session guard has exactly
     * the same lifetime as the verdict above, and for the same reason —
     * a user action is allowed to re-ask, the automatic retry is not.
     * Detaches (never joins) any probe still in flight. */
    join_portmap_reset();
    SDL_SetAtomicInt(&s_q_head, 0);
    SDL_SetAtomicInt(&s_q_tail, 0);
    SDL_SetAtomicInt(&s_q_drops, 0);
    SDL_SetAtomicInt(&s_q_drops_logged, 0);
    s_outcome_reported = false;  /* S3 */
    s_host_deliver_seen = false; /* S3 */
    s_host_waiting_since_ms = 0; /* S3 */
    s_host_waiting_last_note_ms = 0;
    s_host_advisory_code = CONNECT_FAIL_NONE;
    s_host_unauth_drops = 0; /* S4a */
    s_host_challenge_seen = false; /* S4c */
    host_punch_gate_reset(); /* S4-review HIGH-1b */
    signal_cookie_reset();         /* S4c */
    SDL_SetAtomicInt(&s_host_registering, 0);
    memset(s_rendezvous_send_q, 0, sizeof(s_rendezvous_send_q));
    memset(&s_work, 0, sizeof(s_work));
    memset(&s_upnp_mapping, 0, sizeof(s_upnp_mapping));
    s_portmap_lease_expiry_ms = 0;
    s_upnp_next_renew_ms = 0;
    s_status[0] = '\0';
    Netplay_SetSessionTeardownCallback(direct_p2p_on_teardown);
    /* Task #157: clear netplay.status at boot. Covers the one lifecycle
     * hole the set_state hook cannot: a process that died while
     * HOST_WAITING (crash, SIGKILL) leaves HOSTING + a dead code on
     * disk, and a launcher reading it would send a joiner to a dead
     * endpoint. The launcher also rewrites the file to IDLE before it
     * spawns the game, so between the two no stale code survives to be
     * read. */
    netplay_status_publish(false, NULL);
    s_init_done = true;
}

void DirectP2P_BeginHost(int preferred_port) {
    DirectP2P_Init();
    if (get_state() != DIRECT_P2P_IDLE) {
        return;
    }
    /* #36: arm the connect-log mutex on the MAIN thread, before any
     * worker below can spawn. That ordering is the happens-before that
     * lets host_thread_fn / the rendezvous + bilateral workers call
     * Netplay_LogConnectEventMT without racing the mutex pointer itself.
     * Idempotent. */
    Netplay_LogSinkInit();
    /* Natural-success exit guard: if a previous worker returned without
     * being detached (5a switched from detach to wait), join its handle
     * before clobbering it. Safe when s_thread is NULL. */
    if (s_thread != NULL) {
        SDL_WaitThread(s_thread, NULL);
        s_thread = NULL;
    }

    SDL_SetAtomicInt(&s_cancel, 0);
    SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_handoff_pending, 0);
    SDL_SetAtomicInt(&s_bilateral_failed, 0);
    s_bilateral_fail_count = 0; /* M1: fresh retry budget per hosting session */
    s_host_stun_retry_count = 0; /* S2: fresh host STUN-retry budget */
    s_host_stun_retry_at_ms = 0;
    /* #96: the port-map FAILURE verdict has exactly the ladder's own
     * lifetime — same reset sites as the retry counter above. Narrower
     * than s_upnp_mapping on purpose: a user action re-probes. */
    s_portmap_failed_port = 0;
    /* #121: the joiner probe's one-spawn-per-session guard has exactly
     * the same lifetime as the verdict above, and for the same reason —
     * a user action is allowed to re-ask, the automatic retry is not.
     * Detaches (never joins) any probe still in flight. */
    join_portmap_reset();
    SDL_SetAtomicInt(&s_q_drops, 0);
    SDL_SetAtomicInt(&s_q_drops_logged, 0);
    /* R-1: a reject latched during a session whose teardown never ran the
     * callback (e.g. LAN CLI session with no orchestrator) must not leak
     * into this fresh session's teardown. */
    s_handshake_reject_latched = false;
    s_outcome_reported = false;  /* S3: fresh report per hosting session */
    s_host_deliver_seen = false; /* S3 */
    s_host_waiting_since_ms = 0; /* S3 */
    s_host_waiting_last_note_ms = 0;
    s_host_advisory_code = CONNECT_FAIL_NONE;
    s_host_unauth_drops = 0; /* S4a */
    s_host_challenge_seen = false; /* S4c */
    host_punch_gate_reset(); /* S4-review HIGH-1b */
    signal_cookie_reset();         /* S4c */
    SDL_SetAtomicInt(&s_host_registering, 0);
    Stun_ReleaseServerAddr(&s_work.stun); /* belt-and-braces before memset */
    s_stun_keepalive_last_ms = 0;
    s_rebind_txid_valid = false;
    s_drift_pending_valid = false; /* M2: drop any half-confirmed drift */
    memset(&s_work, 0, sizeof(s_work));
    s_work.role = ROLE_HOST;
    s_work.preferred_port = preferred_port;
    /* HOST_PORT config key semantics: >0 wins over the parameter when
     * the parameter is 0 (wrapper passes the config-derived value in
     * directly; this is a belt-and-braces fallback for callers that
     * forgot). If neither the parameter nor the config key specifies a
     * port, fall back to NETPLAY_DIRECT_P2P_DEFAULT_PORT — UPnP needs a
     * concrete external port to map, so 0 (OS-ephemeral) isn't viable. */
    if (preferred_port == 0) {
        int cfg_port = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_HOST_PORT);
        if (cfg_port > 0 && cfg_port <= 65535) {
            s_work.preferred_port = cfg_port;
        } else {
            s_work.preferred_port = NETPLAY_DIRECT_P2P_DEFAULT_PORT;
        }
    }
    s_thread = SDL_CreateThread(host_thread_fn, "DirectP2PHost", NULL);
    if (s_thread == NULL) {
        set_fail_msg(CONNECT_FAIL_INTERNAL, "Connection failed. Try again.");
        set_state(DIRECT_P2P_FAILED_STUN);
        return;
    }
    /* Worker handle retained — DirectP2P_Cancel and direct_p2p_on_teardown
     * SDL_WaitThread it. Natural-success exit is joined on next
     * BeginHost/BeginJoin via the guard above, or on teardown. */
}

void DirectP2P_BeginJoin(const char* peer_code) {
    DirectP2P_Init();
    if (peer_code == NULL || peer_code[0] == '\0') return;
    if (get_state() != DIRECT_P2P_IDLE) return;
    /* #36: see DirectP2P_BeginHost — main thread, before join_thread_fn. */
    Netplay_LogSinkInit();

    /* Decode before spawning the thread: user-input errors should
     * surface immediately, not after a STUN round-trip. (S3: safe to
     * memset s_work here — state == IDLE guarantees any previous worker
     * has published its terminal state, which happens-after its last
     * s_work write.)
     *
     * S4b: the decode result is a tri-state-plus — a legacy-checksum-
     * valid 11/14/18-char v1/v2/v3 code, and an unknown-version 12-char
     * code, each get their own explanation
     * (CONNECT_FAIL_CODE_VERSION_OLDER / _NEWER — L-4 split so log
     * triage can tell WHICH side is stale) instead of the
     * mysterious generic "Invalid room code.". */
    uint32_t ip_be = 0;
    uint16_t pub_port = 0;
    const RoomCodeDecodeResult dec =
        RoomCode_Decode(peer_code, &ip_be, &pub_port);
    if (dec != ROOM_CODE_OK) {
        /* Review L-1: same belt-and-braces release the normal path does
         * before ITS memset — a ref'd STUN server address left in
         * s_work.stun by an earlier session must not be leaked by the
         * zeroing below. */
        Stun_ReleaseServerAddr(&s_work.stun);
        memset(&s_work, 0, sizeof(s_work));
        s_work.role = ROLE_JOIN;
        s_outcome_reported = false;
        switch (dec) {
        case ROOM_CODE_OLD_FORMAT:
            SDL_Log("[direct_p2p] room code is a valid pre-v4 (v1/v2/v3) code — "
                    "the host runs an older build");
            set_fail_msg(CONNECT_FAIL_CODE_VERSION_OLDER,
                         "Code is from an older game version.");
            break;
        case ROOM_CODE_FUTURE_VERSION:
            SDL_Log("[direct_p2p] room code carries an unknown format-version "
                    "char — created by a newer build?");
            set_fail_msg(CONNECT_FAIL_CODE_VERSION_NEWER,
                         "Code is from a newer game version.");
            break;
        default:
            set_fail(CONNECT_FAIL_INVALID_CODE);
            break;
        }
        set_state(DIRECT_P2P_FAILED_PUNCH);
        return;
    }

    struct in_addr in;
    in.s_addr = ip_be;
    char peer_ip[64] = { 0 };
    if (inet_ntop(AF_INET, &in, peer_ip, sizeof(peer_ip)) == NULL) {
        Stun_ReleaseServerAddr(&s_work.stun); /* L-1: see decode path above */
        memset(&s_work, 0, sizeof(s_work));
        s_work.role = ROLE_JOIN;
        s_outcome_reported = false;
        set_fail(CONNECT_FAIL_INVALID_CODE);
        set_state(DIRECT_P2P_FAILED_PUNCH);
        return;
    }

    /* Natural-success exit guard: see BeginHost for the rationale. */
    if (s_thread != NULL) {
        SDL_WaitThread(s_thread, NULL);
        s_thread = NULL;
    }

    SDL_SetAtomicInt(&s_cancel, 0);
    SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_handoff_pending, 0);
    SDL_SetAtomicInt(&s_bilateral_failed, 0);
    s_bilateral_fail_count = 0; /* M1: fresh retry budget per hosting session */
    s_host_stun_retry_count = 0; /* S2: fresh host STUN-retry budget */
    s_host_stun_retry_at_ms = 0;
    /* #96: the port-map FAILURE verdict has exactly the ladder's own
     * lifetime — same reset sites as the retry counter above. Narrower
     * than s_upnp_mapping on purpose: a user action re-probes. */
    s_portmap_failed_port = 0;
    /* #121: the joiner probe's one-spawn-per-session guard has exactly
     * the same lifetime as the verdict above, and for the same reason —
     * a user action is allowed to re-ask, the automatic retry is not.
     * Detaches (never joins) any probe still in flight. */
    join_portmap_reset();
    SDL_SetAtomicInt(&s_q_drops, 0);
    SDL_SetAtomicInt(&s_q_drops_logged, 0);
    s_handshake_reject_latched = false; /* R-1: see BeginHost */
    s_outcome_reported = false;  /* S3: fresh report per join session */
    s_host_deliver_seen = false; /* S3 */
    s_host_waiting_since_ms = 0; /* S3 */
    s_host_waiting_last_note_ms = 0;
    s_host_advisory_code = CONNECT_FAIL_NONE;
    s_host_unauth_drops = 0; /* S4a */
    s_host_challenge_seen = false; /* S4c */
    host_punch_gate_reset(); /* S4-review HIGH-1b */
    signal_cookie_reset();         /* S4c */
    SDL_SetAtomicInt(&s_host_registering, 0);
    Stun_ReleaseServerAddr(&s_work.stun); /* belt-and-braces before memset */
    s_stun_keepalive_last_ms = 0;
    s_rebind_txid_valid = false;
    s_drift_pending_valid = false; /* M2: drop any half-confirmed drift */
    memset(&s_work, 0, sizeof(s_work));
    s_work.role = ROLE_JOIN;
    SDL_strlcpy(s_work.peer_code, peer_code, sizeof(s_work.peer_code));
    SDL_strlcpy(s_work.peer_ip, peer_ip, sizeof(s_work.peer_ip));
    s_work.peer_public_port = pub_port;
    /* v4 (task #155): the room code no longer carries a nonce; every
     * caller of the session-key / punch-token derivations uses this
     * fixed value (see room_code.h's accepted-risk section). */
    s_work.nonce = ROOM_CODE_V4_FIXED_NONCE;

    s_thread = SDL_CreateThread(join_thread_fn, "DirectP2PJoin", NULL);
    if (s_thread == NULL) {
        set_fail_msg(CONNECT_FAIL_INTERNAL, "Connection failed. Try again.");
        set_state(DIRECT_P2P_FAILED_STUN);
        return;
    }
    /* Worker handle retained — see BeginHost. */
}

void DirectP2P_Cancel(void) {
    DirectP2PState st = get_state();
    if (st == DIRECT_P2P_IDLE || st == DIRECT_P2P_HANDOFF) return;

    SDL_SetAtomicInt(&s_cancel, 1);
    SDL_SetAtomicInt(&s_rendezvous_cancel, 1);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 1);

    /* Wait for any in-flight worker(s) to observe the cancel flag and
     * exit. Replaces the prior spin-on-state loop — joining the actual
     * thread handle eliminates the race where the worker writes s_work
     * after the memset below. Each worker honors its cancel flag at the
     * next loop iteration; worst-case blocking is bounded by the
     * longest cancel-uninterruptible span among the active workers:
     * Stun_Discover takes no cancel flag, so a worker inside it blocks
     * us for up to stun_budget_ms() — the user-configurable STUN
     * timeout, clamped to [1000, 15000] ms (default 4000) precisely so
     * this join stays bounded (review L-4). Stun_HolePunch checks its
     * cancel flag every ~10-50 ms. */
    if (s_thread)                  { SDL_WaitThread(s_thread,                  NULL); s_thread                  = NULL; }
    if (s_rendezvous_thread)       { SDL_WaitThread(s_rendezvous_thread,       NULL); s_rendezvous_thread       = NULL; }
    if (s_bilateral_punch_thread)  { SDL_WaitThread(s_bilateral_punch_thread,  NULL); s_bilateral_punch_thread  = NULL; }

    /* Producer threads are joined; release any pending NET_Address refs
     * still in the rendezvous send queue. */
    rend_q_purge();

    /* Tear down any retained resources: the worker may already have
     * populated s_work.stun and then observed cancel after publishing
     * HOST_WAITING. */
    if (s_work.stun.socket != NULL) {
        NET_DestroyDatagramSocket(s_work.stun.socket);
        s_work.stun.socket = NULL;
    }
    Stun_ReleaseServerAddr(&s_work.stun); /* S1: drop keepalive target ref */
    s_stun_keepalive_last_ms = 0;
    s_rebind_txid_valid = false;
    s_drift_pending_valid = false; /* M2: drop any half-confirmed drift */
    /* S1: reap any in-flight UPnP lease renewal before RemoveMapping —
     * bounded, and RemoveMapping is skipped when the worker had to be
     * detached (review H3; see upnp_renew_join_and_discard). */
    /* #121: this MUST run before the removal below, not with the other
     * per-session resets further down. join_portmap_reset detaches a
     * probe that is still executing inside miniupnpc, and portmap_remove
     * walks the same cached controlURL and FreeUPNPUrls()es it. Ordered
     * after the worker joins above (so no spawn can race) and before the
     * release (so no removal can race a live probe). Its verdict gates
     * the removal for the same reason upnp_renew_join_and_discard's
     * does. */
    const bool join_portmap_quiescent = join_portmap_reset();
    if (upnp_renew_join_and_discard() && join_portmap_quiescent) {
        if (s_upnp_mapping.active) {
            portmap_remove(&s_upnp_mapping);
        }
    }
    memset(&s_upnp_mapping, 0, sizeof(s_upnp_mapping));
    memset(&s_work, 0, sizeof(s_work));
    set_status("");
    s_handshake_reject_latched = false; /* R-1: drop any pending reject latch */
    s_host_stun_retry_count = 0; /* S2: drop any pending host STUN retry */
    s_host_stun_retry_at_ms = 0;
    /* #96: the port-map FAILURE verdict has exactly the ladder's own
     * lifetime — same reset sites as the retry counter above. Narrower
     * than s_upnp_mapping on purpose: a user action re-probes. */
    s_portmap_failed_port = 0;
    /* #121: already reset above, BEFORE the mapping release — see there
     * for why the ordering is not free to move. */
    s_outcome_reported = false;  /* S3 */
    s_host_deliver_seen = false; /* S3 */
    s_host_waiting_since_ms = 0; /* S3 */
    s_host_waiting_last_note_ms = 0;
    s_host_advisory_code = CONNECT_FAIL_NONE;
    s_host_unauth_drops = 0; /* S4a */
    s_host_challenge_seen = false; /* S4c */
    host_punch_gate_reset(); /* S4-review HIGH-1b */
    signal_cookie_reset();         /* S4c */
    SDL_SetAtomicInt(&s_host_registering, 0);
    set_state(DIRECT_P2P_IDLE);
}

/* S3 — see direct_p2p.h. Latch a post-handoff session failure with its
 * taxonomy code: status text for the overlay now, FAILED_HANDSHAKE park
 * at teardown, and one machine-coded report line from Tick's reporting
 * path once the terminal state is visible.
 *
 * Task #144 review Item B: FIRST-WINS. netplay.c's process_session() can
 * see both a disconnect and a desync GekkoSessionEvent in the SAME batch
 * (handle_disconnection's EXITING guard stops the second TEARDOWN, but
 * this function ran before that guard, once per event, so without this
 * check the second call's text would silently overwrite the first's —
 * misattributing the overlay to whichever event happened to sort last).
 * Skipping a later call cannot swallow a MORE informative reason: the
 * three call families that reach here — the MIST reject in
 * NETPLAY_SESSION_TRANSITIONING, the CONNECTING-deadline timeout, and the
 * RUNNING-phase disconnect/desync pair in process_session() — are
 * mutually exclusive PER SESSION. The first two set session_state =
 * NETPLAY_SESSION_EXITING and `break` out of the SAME switch statement
 * (netplay.c) that gates process_session() behind `case
 * NETPLAY_SESSION_RUNNING:`, so a session that took either of those exits
 * never reaches RUNNING and process_session()'s event loop — the only
 * other caller of this function — never runs for it. The sole real
 * collision is therefore disconnect-vs-desync within one RUNNING-phase
 * event batch, and connect_fail.c's user-text comment already treats
 * both as equally truthful ("a player dropped by their opponent should
 * not read the same message as one whose session desynced" — neither is
 * ranked more informative than the other). The latch this checks is
 * cleared once at teardown consumption and again at every BeginHost /
 * BeginJoin / Cancel call (see direct_p2p_on_teardown and the R-1 comment on
 * s_handshake_reject_latched), so first-wins never leaks across
 * sessions. */
void DirectP2P_NotifySessionFailed(ConnectFailCode code, const char* reason) {
    if (s_handshake_reject_latched) {
        SDL_Log("[direct_p2p] session failure (%s) dropped — a reason is "
                "already latched for this session: %s",
                ConnectFail_Code(code), reason ? reason : "(no reason)");
        return;
    }
    set_status((reason != NULL && reason[0] != '\0')
                   ? reason
                   : ConnectFail_UserText(code));
    s_work.fail_code = code;
    s_handshake_reject_latched = true;
    /* The pre-handoff outcome (OK) may already have been reported; this
     * is a NEW outcome for the same attempt-set — re-arm the reporter. */
    s_outcome_reported = false;
    SDL_Log("[direct_p2p] session failed post-handoff (%s): %s",
            ConnectFail_Code(code), reason ? reason : "(no reason)");
}

/* See direct_p2p.h — arm-time refusal with no session to tear down:
 * publish the reason and park in FAILED_HANDSHAKE immediately so the
 * overlay renders ERROR + reason on the very next frame.
 *
 * Rebase integration note: this predates the S3 taxonomy, which made
 * Tick's terminal FAILED_HANDSHAKE case emit one machine-coded
 * "[netplay-connect] FAIL code=..." line via report_connect_outcome.
 * Parking the state WITHOUT a fail_code would emit that line with the
 * zero code (P2P_FAIL_NONE) and role=none — a dishonest log entry for a
 * fully-attributable cause. Stamp the taxonomy explicitly, and re-arm
 * the reporter for the same reason NotifySessionFailed does. */
void DirectP2P_RefuseSession(const char* reason) {
    set_status((reason != NULL && reason[0] != '\0') ? reason : "Netplay unavailable.");
    s_work.fail_code = CONNECT_FAIL_BALANCE_UNAVAILABLE;
    s_outcome_reported = false;
    SDL_Log("[direct_p2p] refusing session: %s", reason ? reason : "(no reason)");
    set_state(DIRECT_P2P_FAILED_HANDSHAKE);
}

/* R-1 — see direct_p2p.h. Records the reject reason for the overlay and
 * latches the teardown redirect to FAILED_HANDSHAKE. */
void DirectP2P_NotifySessionRejected(const char* reason) {
    DirectP2P_NotifySessionFailed(CONNECT_FAIL_PEER_REJECTED, reason);
}

/* S3 Part A(3): per-frame informative pass for HOST_WAITING (see the
 * s_host_waiting_* comment). Called from Tick's HOST_WAITING branch —
 * main thread, like the keepalive tick beside it. */
static void host_waiting_tick(void) {
    const uint64_t now = SDL_GetTicks();
    if (s_host_waiting_since_ms == 0) {
        s_host_waiting_since_ms = now;
        s_host_waiting_last_note_ms = now;
        return;
    }
    uint32_t waited_ms = (uint32_t)(now - s_host_waiting_since_ms);
#ifdef NETPLAY_TEST_HOOKS
    /* S4-review MEDIUM-2 seam: CONNECT_HOST_ADVISORY_MS is a 30 s
     * compile-time constant, so the COOKIE_REJECTED host-advisory test
     * would otherwise have to sleep 30 s of real time. Scale the elapsed
     * clock instead of the threshold: the classifier under test still
     * sees the real CONNECT_HOST_ADVISORY_MS boundary, so the test
     * exercises the shipped constant rather than a test-only one. */
    if (s_host_advisory_scale > 1) {
        waited_ms *= (uint32_t)s_host_advisory_scale;
    }
#endif

    /* Cause-8 advisory (once per hosting session): the server answers
     * every REGISTER with a DELIVER, so zero DELIVERs after the
     * threshold means the rendezvous path is dead — and with no UPnP
     * mapping either, this room is likely unjoinable. Tell the host NOW
     * instead of letting a friend fail minutes later. */
    if (s_host_advisory_code == CONNECT_FAIL_NONE &&
        SDL_GetAtomicInt(&s_host_registering) != 0) {
        const ConnectFailCode adv = ConnectFail_ClassifyHostWaiting(
            s_upnp_mapping.active, s_host_deliver_seen,
            s_host_challenge_seen, waited_ms);
        if (adv != CONNECT_FAIL_NONE) {
            s_host_advisory_code = adv;
            char line[256];
            SDL_snprintf(line, sizeof(line),
                         "[netplay-connect] ADVISORY code=%s role=host waited_ms=%u "
                         "upnp=%d deliver_seen=0 challenge_seen=%d — %s",
                         ConnectFail_Code(adv), (unsigned)waited_ms,
                         (int)s_upnp_mapping.active,
                         (int)s_host_challenge_seen,
                         adv == CONNECT_FAIL_HOST_UNMAPPABLE
                             ? "no UPnP mapping and no rendezvous contact: joiners "
                               "will likely fail; ask the other side to host"
                         : adv == CONNECT_FAIL_COOKIE_REJECTED
                             ? "server challenges us but never accepts the cookie "
                               "echo: auth/version trouble, not a dead server"
                             : "rendezvous server unreachable: the bilateral "
                               "fallback is unavailable, direct joins still work");
            Netplay_LogConnectEvent(line);
            if (adv == CONNECT_FAIL_HOST_UNMAPPABLE ||
                adv == CONNECT_FAIL_COOKIE_REJECTED) {
                /* Overlay line 3 — the room code stays displayed (a
                 * direct cone-NAT join could still land), but the host
                 * is no longer silently unaware. */
                set_status(ConnectFail_UserText(adv));
            }
        }
    }

    /* Minute-cadence liveness note: log + on-screen elapsed counter (the
     * unmappable / cookie advisories own the status line when present). */
    if (now - s_host_waiting_last_note_ms >= 60000u) {
        s_host_waiting_last_note_ms = now;
        const unsigned min = waited_ms / 60000u;
        SDL_Log("[direct_p2p] host still advertising (%u min): rendezvous=%s upnp=%s",
                min, s_host_deliver_seen ? "alive" : "SILENT",
                s_upnp_mapping.active ? "mapped" : "none");
        if (s_host_advisory_code != CONNECT_FAIL_HOST_UNMAPPABLE &&
            s_host_advisory_code != CONNECT_FAIL_COOKIE_REJECTED) {
            char st[64];
            SDL_snprintf(st, sizeof(st), "Waiting for player 2... (%u min)", min);
            set_status(st);
        }
    }
}

/* S3 — one attributed outcome line per attempt-set, written to the
 * per-session netplay log (lazily opened by Netplay_LogConnectEvent for
 * pre-session failures) + SDL_Log. Main-thread only: called exclusively
 * from Tick after it observes a terminal state, so every s_work field
 * the worker wrote before publishing that state is visible. */
static void report_connect_outcome(DirectP2PState st, bool success) {
    if (s_outcome_reported) {
        return;
    }
    s_outcome_reported = true;
    /* The msg field can already be a full sizeof(s_status)-1 chars.
     * SDL_snprintf truncates safely, but a truncated report line is a
     * silently degraded diagnostic, so the buffer keeps real headroom
     * rather than sitting just under. (The relay's `via_relay=` /
     * `relay_fail=` / `relay=` fields were dropped with the rung.) */
    /* #36 widened this from 640: the FAIL branch now carries the portmap
     * leg, the confirm witness and the four frame counters. #104 widened
     * it again to 1024 for the five host-ladder fields — the ARM build
     * treats -Wformat-truncation as an error (see the netplay_nav.c
     * nav-deadline buffer widening), so headroom here is a build gate,
     * not a style preference. s_status then grew 128 -> 256 for the
     * wrapped refusal overlay; this grew by the same 128 to keep the
     * headroom it had. */
    char line[1152];
    if (success) {
        /* #104: `attempts=` is s_work.join_attempts and is JOINER-ONLY —
         * nothing on the host path ever increments it, so a host line has
         * always read attempts=0. `host_rungs=` is the host's equivalent,
         * and printing both (rather than overloading `attempts=`) keeps
         * every existing parser and every archived report line valid. */
        SDL_snprintf(line, sizeof(line),
                     "[netplay-connect] OK role=%s attempts=%d host_rungs=%u/%d "
                     "t_ms upnp=%u stun=%u race=%u punch=%u signal=%u bilateral=%u "
                     "stun=%d/%d portdis=%d",
                     s_work.role == ROLE_HOST ? "host" : "join",
                     s_work.join_attempts,
                     (unsigned)s_work.host_rungs, 1 + host_stun_max_retries(),
                     s_work.t_upnp_ms, s_work.t_stun_ms, s_work.t_race_ms, s_work.t_punch_ms,
                     s_work.t_signal_ms, s_work.t_bilateral_ms,
                     s_work.stun.diag_servers_answered,
                     s_work.stun.diag_servers_probed,
                     (int)s_work.stun.port_disagreement);
    } else {
        /* #36: the attribution verdict rides ALONGSIDE the code, never
         * instead of it. ConnectFail_Attribute is a pure function so the
         * rules are unit-testable without standing up a race. */
        const ConnectAttribution attrib =
            ConnectFail_Attribute(s_work.fail_code, s_work.race_confirm_seen,
                                  s_work.ev_badver_n);
        /* F6 — SCOPE IS IN THE FIELD NAME. The four frame counters are
         * populated ONLY by p2p_race, so they describe the race and
         * nothing else. The host's own DELIVER arrives on
         * host_rendezvous_thread_fn, OUTSIDE any race, which is why a
         * perfectly healthy host FAIL line can read
         * `deliver=any:1,real:1 ... race_deliver_n=0 race_dgap=0`. With
         * the bare names a triager reads that pair as "the server never
         * delivered"; with the race_ prefix the two fields visibly
         * measure different things. The struct fields keep their names —
         * this is a log-format decision, not a data-model one. */
        /* #104 — THE HOST LADDER, WHICH THIS LINE COULD NOT SHOW.
         *
         * Every t_ms field above is the LAST rung: host_thread_fn
         * overwrites t_upnp_ms / t_stun_ms on each of its 1 +
         * HOST_STUN_MAX_RETRIES entries, and `attempts=` counts joiner
         * attempts only. So four slow host failures and one fast host
         * failure printed the SAME line — and the four-rung case can
         * legitimately consume the whole derived nav deadline
         * (ORCH_HOST_WORST_CASE_MS sums 4 port-map probes, 4 STUN
         * budgets and 3 x HOST_STUN_RETRY_BACKOFF_MS). Reading that as
         * one fast failure is the H-1 misattribution shape: real, slow,
         * bounded work discarded and the tester's network blamed.
         *
         * host_rungs= disambiguates the two. The three *_total= sums say
         * where the time went. host_portmap_probes= is the #96 evidence
         * field: it equals host_rungs exactly when the re-probe skip did
         * NOT fire, i.e. when the probe was failing — the case that pays
         * PORTMAP_PROBE_BUDGET_MS every rung. All four are zero on a
         * joiner line, which is correct: the joiner has no ladder. */
        SDL_snprintf(line, sizeof(line),
                     "[netplay-connect] FAIL code=%s state=%d role=%s msg=\"%s\" "
                     "attempts=%d host_rungs=%u/%d host_portmap_probes=%u "
                     "t_ms upnp=%u stun=%u race=%u punch=%u signal=%u bilateral=%u "
                     "t_ms_total upnp=%u stun=%u backoff=%u "
                     "stun=%d/%d sends_ok=%d dns_all_failed=%d portdis=%d "
                     "deliver=any:%d,real:%d "
                     "portmap=%s/%d confirm=%d@%ums race_deliver_n=%u "
                     "race_challenge_n=%u race_badver_n=%u race_dgap=%u "
                     "race_nack_n=%u race_nack=%s attrib=%s",
                     ConnectFail_Code(s_work.fail_code),
                     (int)st,
                     s_work.role == ROLE_HOST ? "host"
                     : s_work.role == ROLE_JOIN ? "join" : "none",
                     s_status,
                     s_work.join_attempts,
                     (unsigned)s_work.host_rungs, 1 + host_stun_max_retries(),
                     (unsigned)s_work.host_portmap_probes,
                     s_work.t_upnp_ms, s_work.t_stun_ms, s_work.t_race_ms, s_work.t_punch_ms,
                     s_work.t_signal_ms, s_work.t_bilateral_ms,
                     s_work.t_upnp_total_ms, s_work.t_stun_total_ms,
                     s_work.t_host_backoff_ms,
                     s_work.stun.diag_servers_answered,
                     s_work.stun.diag_servers_probed,
                     s_work.stun.diag_sends_ok,
                     (int)s_work.stun.diag_dns_all_failed,
                     (int)s_work.stun.port_disagreement,
                     (int)s_work.ev_deliver_any, (int)s_work.ev_deliver_real,
                     portmap_backend_name((PortMapBackend)s_work.portmap_backend),
                     (int)s_work.portmap_active,
                     (int)s_work.race_confirm_seen, (unsigned)s_work.race_confirm_ms,
                     (unsigned)s_work.ev_deliver_n, (unsigned)s_work.ev_challenge_n,
                     (unsigned)s_work.ev_badver_n,
                     (unsigned)s_work.ev_deliver_gap_max_ms,
                     /* #122: the reason NAME, not the number. This is the
                      * field that separates "the matchmaking server was
                      * unreachable" from "the matchmaking server told us
                      * why", and it is the whole reason the collapse into
                      * four BUSY verdicts costs triage nothing. */
                     (unsigned)s_work.ev_nack_n,
                     s_work.ev_nack_any
                         ? Rendezvous_NackReasonText(s_work.ev_nack_reason)
                         : "none",
                     ConnectFail_AttributionText(attrib));
        Netplay_LogConnectEvent(line);
        /* Exactly ONE extra line per failed attempt, and only when the
         * `code=` field alone would mislead: the evidence contradicts it
         * (AMBIG_CONFIRM), cannot support it (AMBIG_VERSION), or names a
         * cause the code does not (VERSION_SKEW — where the note is the
         * OPPOSITE of a hedge and carries the actual remedy). A
         * SUPPORTED verdict prints no note, because there is nothing the
         * code does not already say. */
        const char* note = ConnectFail_AttributionNote(attrib);
        if (note[0] != '\0') {
            char note_line[512];
            SDL_snprintf(note_line, sizeof(note_line),
                         "[netplay-connect] NOTE %s", note);
            Netplay_LogConnectEvent(note_line);
        }
        return;
    }
    Netplay_LogConnectEvent(line);
}

void DirectP2P_Tick(void) {
    DirectP2PState st = get_state();

    /* S1: UPnP lease renewal — only in states where host_thread_fn has
     * finished writing s_upnp_mapping (it publishes HOST_WAITING after)
     * and the mapping is potentially carrying traffic. HANDOFF covers
     * the active netplay session: main.c ticks us from the session
     * branch too, so a mapping that outlives the 1-hour lease keeps
     * getting renewed mid-session. Never runs during UPNP_PROBE /
     * STUN_DISCOVER, where the worker thread still owns the mapping. */
    if (st == DIRECT_P2P_HOST_WAITING || st == DIRECT_P2P_FALLBACK_SIGNALING ||
        st == DIRECT_P2P_FALLBACK_BILATERAL_PUNCH || st == DIRECT_P2P_HANDOFF) {
        upnp_renew_tick();
    }

    switch (st) {
    case DIRECT_P2P_IDLE:
    case DIRECT_P2P_UPNP_PROBE:
    case DIRECT_P2P_STUN_DISCOVER:
    case DIRECT_P2P_JOIN_PUNCHING:
        /* Worker is active; nothing for main thread to do. */
        return;

    case DIRECT_P2P_HOST_WAITING:
        /* Drain the rendezvous send queue first (up to 4 slots per tick
         * per §Decision 3) so REGISTER/POLL packets enqueued by the
         * rendezvous worker reach the wire from the main thread (the
         * STUN socket's sole I/O actor during this phase). Then poll for
         * inbound — peer punch OR rendezvous DELIVER. */
        rend_q_drain(s_work.stun.socket, 4);
        /* S1: periodic STUN rebind keepalive (mapping refresh + drift
         * probe). Send-only and non-blocking; the response comes back
         * through host_tick_receive's STUN gate. */
        host_stun_keepalive_tick();
        /* S3: elapsed-time status + cause-8 advisory (see host_waiting_tick). */
        host_waiting_tick();
        host_tick_receive();
        return;

    case DIRECT_P2P_HANDOFF:
        /* Three entry points land us here:
         *   - Host direct path: host_tick_receive() already executed
         *     the handoff inline (main thread) and left state at
         *     HANDOFF. The netplay session is now running; nothing
         *     more to do. Leave state at HANDOFF; teardown resets it
         *     via direct_p2p_on_teardown.
         *   - Host bilateral fallback: the bilateral worker writes
         *     peer endpoint back to s_work and raises
         *     s_bilateral_handoff_pending while staying in
         *     FALLBACK_BILATERAL_PUNCH. Tick's
         *     FALLBACK_BILATERAL_PUNCH case (below) runs do_handoff
         *     and publishes HANDOFF — by the time we land here the
         *     handoff has already executed.
         *   - Join path: the worker published HANDOFF but the actual
         *     Netplay_BeginDirectP2P call has to happen on the main
         *     thread (Netplay internals are not thread-safe). Detect
         *     the join side by role and drive the handoff once.
         */
        if (s_work.role == ROLE_JOIN && s_work.stun.socket != NULL) {
            join_tick_handoff();
        }
        /* S3: one success line with the stage timings — the "how long
         * did each phase take in the field" data reports need.
         *
         * S3-review HIGH-2: once a post-handoff failure has been latched
         * (DirectP2P_NotifySessionFailed re-arms the reporter while the
         * orchestrator is still parked here — Netplay_Run runs BEFORE
         * DirectP2P_Tick in the same frame, and the netplay teardown that
         * publishes FAILED_HANDSHAKE only runs on the NEXT frame's
         * EXITING pass), this success report MUST NOT re-fire: it would
         * consume the re-arm as a second, spurious OK line and thereby
         * suppress the real FAIL line the FAILED_HANDSHAKE case emits.
         * The latch is main-thread, like every reader here. */
        if (!s_handshake_reject_latched) {
            report_connect_outcome(st, true);
        }
        return;

    case DIRECT_P2P_FAILED_STUN:
        /* S2: HOST auto-retry with backoff. FAILED_STUN on the host is
         * frequently transient (DHCP renew mid-discovery, DNS hiccup,
         * momentary uplink loss) and the host has nothing else to do —
         * parking terminal turns a 5-second blip into a dead room.
         * Bounded per hosting session; each retry re-runs the full
         * host_thread_fn. The UPnP step reuses a live mapping from the
         * previous attempt rather than re-probing (review L-5 — a
         * transient re-probe failure must not desync upnp_ok from
         * s_upnp_mapping.active); with no live mapping it probes
         * fresh. The JOINER already retried inside join_thread_fn and
         * stays terminal here. */
        if (s_work.role == ROLE_HOST && s_host_stun_retry_count < host_stun_max_retries()) {
            uint64_t now = SDL_GetTicks();
            const int backoff_ms = host_stun_retry_backoff_ms();
            if (s_host_stun_retry_at_ms == 0) {
                s_host_stun_retry_at_ms = now + (uint64_t)backoff_ms;
                SDL_Log("[direct_p2p] host STUN discovery failed — auto-retry %d/%d in %u ms",
                        s_host_stun_retry_count + 1, host_stun_max_retries(),
                        (unsigned)backoff_ms);
                set_status("Connection failed. Retrying...");
                return;
            }
            if (now < s_host_stun_retry_at_ms) {
                return; /* backoff in progress */
            }
            /* #104: charge the backoff we ACTUALLY slept, not the nominal
             * constant. The arm instant is recoverable from the deadline
             * (it was set to arm + backoff_ms one branch up), so this
             * needs no extra state, and measuring rather than assuming
             * means a Tick starved by a long frame shows up as the larger
             * number it really was. This is the 15,000 ms term
             * ORCH_HOST_WORST_CASE_MS sums and #36 had no field for. */
            {
                const uint64_t armed_at = s_host_stun_retry_at_ms - (uint64_t)backoff_ms;
                s_work.t_host_backoff_ms += (uint32_t)(now - armed_at);
            }
            s_host_stun_retry_at_ms = 0;
            s_host_stun_retry_count++;
            /* Reap the failed worker before re-spawning (it has exited —
             * it published FAILED_STUN as its last act). */
            if (s_thread != NULL) {
                SDL_WaitThread(s_thread, NULL);
                s_thread = NULL;
            }
            SDL_Log("[direct_p2p] host STUN auto-retry %d/%d starting",
                    s_host_stun_retry_count, host_stun_max_retries());
            set_status("Preparing...");
            /* Publish the intermediate state BEFORE spawning so a Tick
             * racing the worker's own set_state never re-enters this
             * case mid-spawn. host_thread_fn re-publishes UPNP_PROBE
             * itself after its startup delay. */
            set_state(DIRECT_P2P_UPNP_PROBE);
            s_thread = SDL_CreateThread(host_thread_fn, "DirectP2PHost", NULL);
            if (s_thread == NULL) {
                SDL_Log("[direct_p2p] host STUN auto-retry: thread spawn failed");
                set_fail_msg(CONNECT_FAIL_INTERNAL, "Connection failed. Try again.");
                set_state(DIRECT_P2P_FAILED_STUN);
            }
            return;
        }
        /* joiner, or retry budget exhausted — terminal */
        report_connect_outcome(st, false);
        return;

    case DIRECT_P2P_FAILED_SYMMETRIC:
    case DIRECT_P2P_FAILED_PUNCH:
        /* Terminal failure; no auto-retry. The status-rendering
         * overlay (Step 8) is responsible for showing the reason. The
         * caller (menu) will eventually issue DirectP2P_Cancel to
         * return to IDLE. */
        report_connect_outcome(st, false);
        return;

    case DIRECT_P2P_FALLBACK_SIGNALING:
        /* Joiner inlines the rendezvous loop into its worker thread, so
         * there's nothing for Tick to drive on the joiner side. The
         * host's REGISTER/POLL phase happens while state is still
         * HOST_WAITING; this case is unreachable in 5a (no transitions
         * to it yet) and remains a no-op skeleton until 5b/5c. */
        return;

    case DIRECT_P2P_FALLBACK_BILATERAL_PUNCH:
        /* The bilateral-punch worker thread (host_bilateral_punch_thread_fn)
         * exclusively owns s_work.stun.socket for reads/writes via
         * Stun_HolePunch during this phase. Per §Decision 3, the main
         * thread MUST NOT call host_tick_receive (concurrent
         * NET_ReceiveDatagram on the same socket is a race) and MUST NOT
         * drain the rendezvous queue (rendezvous resends are stopped by
         * the s_rendezvous_cancel signal raised in try_handle_deliver).
         *
         * On punch SUCCESS the worker raises s_bilateral_handoff_pending
         * (after writing peer_ip/peer_public_port back to s_work) and
         * exits without changing state — do_handoff invokes
         * Netplay_SetParams / Netplay_SetRemotePort / Netplay_SetStunSocket
         * which are main-thread-only. We pick up that signal here, join
         * the worker so its handle is reclaimed and s_work reads are
         * race-free, then run do_handoff inline. do_handoff itself does
         * NOT publish HANDOFF (mirroring the direct-path convention in
         * host_tick_receive, where set_state(DIRECT_P2P_HANDOFF)
         * precedes the do_handoff call), so we set_state HANDOFF after.
         *
         * On punch FAILURE the worker publishes FAILED_BILATERAL itself
         * and we never observe the pending flag — that path is handled
         * by the FAILED_BILATERAL terminal case. */
        if (SDL_GetAtomicInt(&s_bilateral_handoff_pending) != 0) {
            SDL_SetAtomicInt(&s_bilateral_handoff_pending, 0);
            if (s_bilateral_punch_thread != NULL) {
                SDL_WaitThread(s_bilateral_punch_thread, NULL);
                s_bilateral_punch_thread = NULL;
            }
            set_state(DIRECT_P2P_HANDOFF);
            /* Host is player 1 (player_number = 0). */
            do_handoff(1, s_work.peer_ip, s_work.peer_public_port);
            return;
        }
        /* Review M1: host-side punch FAILURE. Pre-fix the worker parked
         * terminal FAILED_BILATERAL and the host stopped advertising —
         * so one stale/hostile REGISTER against our key (anyone who saw
         * the room code, or an aborted joiner's leftover slot) killed
         * the room for the whole hosting period. Return to HOST_WAITING
         * and respawn the rendezvous loop (bounded retries so a
         * repeated attacker cannot spin the host in 3 s punch cycles
         * forever); the joiner's own failure handling is untouched. */
        if (SDL_GetAtomicInt(&s_bilateral_failed) != 0 &&
            s_work.role == ROLE_HOST) {
            SDL_SetAtomicInt(&s_bilateral_failed, 0);
            if (s_bilateral_punch_thread != NULL) {
                SDL_WaitThread(s_bilateral_punch_thread, NULL);
                s_bilateral_punch_thread = NULL;
            }
            s_bilateral_fail_count++;
            if (s_bilateral_fail_count >= HOST_BILATERAL_MAX_FAILURES) {
                SDL_Log("[direct_p2p] bilateral punch failed %d times this session — "
                        "parking FAILED_BILATERAL", s_bilateral_fail_count);
                /* S3 causes 5-6 (host side): every punch ran against a
                 * real DELIVER'd endpoint; the NAT pair is the blocker. */
                {
                    ConnectJoinEvidence ev = { 0 };
                    ev.deliver_any = true;
                    ev.deliver_real = true;
                    ev.bilateral_punched = false;
                    ev.port_disagreement = s_work.stun.port_disagreement;
                    ev.punch_bad_token = s_work.stun.diag_punch_bad_token; /* S4a */
                    set_fail(ConnectFail_ClassifyJoin(&ev));
                }
                set_state(DIRECT_P2P_FAILED_BILATERAL);
                return;
            }
            SDL_Log("[direct_p2p] bilateral punch failure %d/%d — returning to "
                    "HOST_WAITING and resuming rendezvous advertising",
                    s_bilateral_fail_count, HOST_BILATERAL_MAX_FAILURES);
            /* The rendezvous thread exited when try_handle_deliver raised
             * s_rendezvous_cancel (and the state left HOST_WAITING); its
             * handle is still ours to reclaim before respawning. */
            if (s_rendezvous_thread != NULL) {
                SDL_WaitThread(s_rendezvous_thread, NULL);
                s_rendezvous_thread = NULL;
            }
            set_status("Waiting for player 2...");
            /* Clear the cancel flag BEFORE publishing HOST_WAITING so a
             * DELIVER processed on a later tick can re-raise it without
             * being clobbered (same ordering as host_thread_fn step 4). */
            SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
            SDL_SetAtomicInt(&s_bilateral_punch_cancel, 0);
            set_state(DIRECT_P2P_HOST_WAITING);
            if (!Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL)) {
                s_rendezvous_thread = SDL_CreateThread(host_rendezvous_thread_fn,
                                                       "DirectP2PRendezvous", NULL);
                if (s_rendezvous_thread == NULL) {
                    SDL_Log("[direct_p2p] WARNING: failed to respawn rendezvous thread "
                            "after bilateral failure; direct punch still possible");
                }
            }
        }
        return;

    case DIRECT_P2P_FAILED_BILATERAL:
        /* Terminal — bilateral fallback exhausted. No work; the menu
         * will issue DirectP2P_Cancel to return to IDLE. */
        report_connect_outcome(st, false);
        return;

    case DIRECT_P2P_FAILED_HANDSHAKE:
        /* Terminal — post-handoff session failure (MIST reject or an S3
         * deadline). Overlay shows ERROR + the reason; DirectP2P_Cancel
         * (or the OSD retry's process re-exec) returns to IDLE. */
        report_connect_outcome(st, false);
        return;
    }
}

DirectP2PState DirectP2P_GetState(void) {
    return get_state();
}

Role DirectP2P_GetRole(void) {
    /* Snapshot value — set once per BeginHost/BeginJoin and not mutated
     * thereafter. No atomic needed; a stale read across the BeginHost ->
     * worker transition is at worst one frame of overlay miscategorization. */
    return s_work.role;
}

bool DirectP2P_HostStunRetryPending(void) {
    /* S3-review M-3: true while Tick's FAILED_STUN case still owns the
     * outcome — a HOST parked in FAILED_STUN with S2 auto-retries left
     * (backoff pending or about to respawn). Once the retry budget is
     * spent (or for a joiner, whose retry ran inside its own worker)
     * FAILED_STUN is TERMINAL: Tick has emitted the attributed FAIL
     * report, and nav must take its exit (a) instead of waiting out the
     * orchestrator deadline and logging a second, contradictory
     * P2P_FAIL_TIMEOUT_ORCHESTRATOR for the same failure. Main-thread
     * only, like the retry bookkeeping it reads. */
    return get_state() == DIRECT_P2P_FAILED_STUN &&
           s_work.role == ROLE_HOST &&
           s_host_stun_retry_count < host_stun_max_retries();
}

/* ======================================================================
 * Task #76 — THE ORCHESTRATOR CASCADE, as arithmetic the compiler checks
 * ======================================================================
 *
 * Worst-case wall clock the orchestrator can legitimately spend between
 * BeginHost/BeginJoin and publishing a terminal state. netplay_nav's
 * NAV_WAIT_ORCHESTRATOR backstop is this plus a scheduling margin, so
 * this is the thing that decides how long a stuck join freezes.
 *
 * WHY IT IS SHAPED LIKE THIS. The constant it replaced was a flat
 * `150 * 60` frames whose comment derived 150 s from the PRE-S6 SERIAL
 * cascade (STUN, then punch, then signalling, then bilateral, summed).
 * S6 made those legs RACE inside one budget; the constant did not move
 * and its derivation quietly stopped being true. So the lesson is not
 * "pick a better number" — 150 s was a plausible number once — it is
 * that the number must be COMPUTED from the legs the code enforces, and
 * that the computation must be checked by something that fails loudly.
 * Hence: macros shared with the enforcement sites, and _Static_asserts.
 *
 * THE LEGS. Every term is the enforcement site's own bound, by symbol,
 * never a copied literal:
 *
 *   WORKER_STARTUP_DELAY_MS   host_thread_fn / join_thread_fn pre-NET_Init
 *                             settle delay (SDL_Delay at both sites).
 *                             PER SPAWN, not per session: it is the
 *                             worker's FIRST statement, so the S2 ladder
 *                             — which RESPAWNS host_thread_fn on every
 *                             rung — pays it 1 + HOST_STUN_MAX_RETRIES
 *                             times. The joiner pays it once, because
 *                             its retry is a loop INSIDE one worker.
 *   JOIN_MAX_ATTEMPTS         the join_thread_fn attempt loop.
 *   stun_budget_ms()          the STUN_DISCOVER argument at both sites,
 *                             clamped [STUN_BUDGET_MIN_MS, _MAX_MS].
 *   RESOLVE_POLL_MAX_MS       resolve_with_short_poll's bounded DNS poll,
 *                             which join_attempt runs once before the race.
 *   RACE_HARD_CAP_MS(race_budget_ms())
 *                             p2p_race's TRUE ceiling — the configured
 *                             budget plus the TWO confirmation tails
 *                             section 8 grants a confirmed leg. Shared
 *                             with the deadline check itself, which is
 *                             the whole point: the first pass at this
 *                             task summed ONE tail, was right when
 *                             written, and went stale the moment the
 *                             second tail landed.
 *   PORTMAP_PROBE_BUDGET_MS   try_portmap's outer deadline (UPnP then
 *                             NAT-PMP/PCP, serially, in one worker).
 *   HOST_STUN_MAX_RETRIES /
 *   HOST_STUN_RETRY_BACKOFF_MS  the S2 host FAILED_STUN ladder.
 *
 * WHAT IS NOT IN HERE, and why that is correct:
 *
 *   - The RELAY. Removed entirely (client and server) — the cascade is
 *     STUN -> direct punch -> rendezvous signalling -> bilateral punch,
 *     full stop. There is no relay budget left to count.
 *   - The per-leg SIGNAL_BUDGET_MS / BILATERAL_PUNCH_MS. Bounded INSIDE
 *     the race, so RACE_HARD_CAP_MS already contains them. Summing them
 *     on top would re-create the pre-S6 serial error in a new place.
 *   - HOST_WAITING. Unbounded BY DESIGN (a host advertises until a joiner
 *     arrives), so nav re-arms its counter every frame while the
 *     orchestrator sits there; it is excluded rather than estimated.
 *   - The host's post-HOST_WAITING race. It DOES have a term of its own
 *     now (ORCH_HOST_POSTWAIT_MS) — but as the other arm of a MAX, not
 *     as an addend, because HOST_WAITING sits between the two phases and
 *     re-arms nav. Until task #96 the ladder dominated it at every legal
 *     config and assert [C] pinned that, so the term could be omitted;
 *     #96 removed 3 of the ladder's 4 port-map probes and inverted the
 *     comparison at one corner. See ORCH_HOST_WORST_CASE_MS.
 *   - The host's bilateral RETRY loop (HOST_BILATERAL_MAX_FAILURES).
 *     Each failure returns to HOST_WAITING, which re-arms nav, so the
 *     deadline only ever has to cover ONE race — not the loop.
 *
 * Thread-safety: reads Config plus DirectP2P_GetRole(), both of which the
 * existing main-thread callers already touch. Main thread only. */

/* The joiner's per-attempt cascade, and the whole joiner path. Macro
 * rather than a function so the _Static_asserts below evaluate LITERALLY
 * the same expression the runtime path evaluates — a second, hand-copied
 * expression in an assert would just be a comment that happens to
 * compile. */
/* #121: the joiner's port-map probe, as a term in the cascade.
 *
 * It is ZERO, and the zero is the design, not an omission — which is
 * exactly why it is written down as an addend instead of left out. A leg
 * that is absent from this sum because nobody thought of it and a leg
 * that is absent because it provably costs nothing look identical in a
 * cascade you can only read; they do not look identical here.
 *
 * join_portmap_spawn is an SDL_CreateThread and returns. The collection
 * point, join_portmap_collect, polls SDL_GetThreadState and takes one of
 * two non-blocking branches: SDL_WaitThread on a thread already reported
 * COMPLETE, or SDL_DetachThread. Neither waits on the network. The probe
 * therefore runs entirely inside wall-clock the cascade was already
 * spending (attempt 1's race) and contributes nothing to the deadline.
 *
 * Make the joiner probe BLOCKING and this constant becomes
 * PORTMAP_PROBE_BUDGET_MS, at which point assert [A] fails with
 * 200 + 2*(4000 + 100 + 11250 + 9200) + 5000 = 54,300 ms against a
 * 35,000 ms ceiling. Assert [E] below pins the one-time variant too, so
 * neither shape can land without re-arguing the ceiling. */
#define ORCH_JOIN_PORTMAP_SERIAL_MS 0

#define ORCH_JOIN_ATTEMPT_MS(stun_ms, race_ms)                       \
    ((stun_ms) + RESOLVE_POLL_MAX_MS + ORCH_JOIN_PORTMAP_SERIAL_MS + \
     RACE_HARD_CAP_MS(race_ms))

#define ORCH_JOIN_WORST_CASE_MS(stun_ms, race_ms) \
    (WORKER_STARTUP_DELAY_MS +                    \
     JOIN_MAX_ATTEMPTS * ORCH_JOIN_ATTEMPT_MS(stun_ms, race_ms))

/* The host's ladder: STUN once per attempt across 1 initial attempt +
 * HOST_STUN_MAX_RETRIES retries with a backoff between each, plus the
 * port-map probe — which task #96 made a ONE-TIME cost.
 *
 * #96 MOVED PORTMAP_PROBE_BUDGET_MS OUT OF THE MULTIPLIER, and that is
 * the whole of the arithmetic change. host_thread_fn now latches the
 * probe's verdict for the hosting session in BOTH directions: success
 * into s_upnp_mapping (which it always did) and failure into
 * s_portmap_failed_port (which it did not). local_port is constant
 * across a ladder, so at most ONE rung can reach try_portmap. Four
 * probes was not a bound that had gone slack — it was work the code
 * genuinely did, 45,000 ms of it at shipped defaults, and the code no
 * longer does it.
 *
 * Shipped defaults: 4 * 200 + 11250 + 4 * 4000 + 3 * 5000 = 43,050 ms,
 * down from 76,800.
 *
 * AND THE HOST BOUND IS NOW A MAX, NOT JUST THE LADDER. The two host
 * phases — the ladder, and the post-HOST_WAITING race — are separated by
 * HOST_WAITING, which re-arms nav's counter every frame. So nav needs
 * the LARGER of them, never the sum. Before #96 the ladder was larger at
 * every legal config and assert [C] pinned exactly that, which is why
 * the bound could be written as the ladder alone. #96 shortens the
 * ladder enough to invert that at one corner (STUN at its floor, race at
 * its ceiling: 31,050 vs 31,300), so the max is now load-bearing rather
 * than decorative. Writing it out is strictly more correct than the
 * previous form, which was only correct because [C] happened to hold —
 * and [C] below now guards the max itself. */
#define ORCH_HOST_LADDER_MS(stun_ms)                                 \
    ((1 + HOST_STUN_MAX_RETRIES) * WORKER_STARTUP_DELAY_MS +         \
     PORTMAP_PROBE_BUDGET_MS +                                       \
     (1 + HOST_STUN_MAX_RETRIES) * (stun_ms) +                       \
     HOST_STUN_MAX_RETRIES * HOST_STUN_RETRY_BACKOFF_MS)

/* The host's ONE post-HOST_WAITING race: the same resolve poll + whole
 * race (both H-1 confirmation tails) the joiner's per-attempt term uses.
 * No worker startup delay — that worker has long since run. */
#define ORCH_HOST_POSTWAIT_MS(race_ms) \
    (RESOLVE_POLL_MAX_MS + RACE_HARD_CAP_MS(race_ms))

#define ORCH_HOST_WORST_CASE_MS(stun_ms, race_ms)          \
    (ORCH_HOST_LADDER_MS(stun_ms) >= ORCH_HOST_POSTWAIT_MS(race_ms) \
         ? ORCH_HOST_LADDER_MS(stun_ms)                    \
         : ORCH_HOST_POSTWAIT_MS(race_ms))

/* ---- the compile-time checks -----------------------------------------
 *
 * [A] THE PRODUCT BUG, as a build failure.
 *
 * Task #76 is a UX bug: a join that cannot succeed froze for 150 s, which
 * a player reads as a hang and power-cycles through, so the attributed
 * failure S3 built the taxonomy for is never seen. The fix is only real
 * if the SHIPPED-DEFAULTS deadline stays human-scale, and it only stays
 * that way if growing the cascade past it breaks the build.
 *
 * The ceiling is not a taste call. The same overlay already makes the
 * player wait CONNECT_TIMEOUT_CONNECTING_MS post-handoff (connect_fail.h)
 * and that is the longest wait this product has ever asked for and
 * considered acceptable. The joiner runs JOIN_MAX_ATTEMPTS attempts, so
 * the pre-handoff phase is allowed the same acceptable wait once per
 * attempt, plus nav's scheduling margin — and no more:
 *
 *     JOIN_MAX_ATTEMPTS * CONNECT_TIMEOUT_CONNECTING_MS
 *         + NAV_ORCH_TIMEOUT_MARGIN_MS
 *     = 2 * 15000 + 5000 = 35000 ms
 *
 * against a shipped-defaults derivation of
 *
 *     200 + 2 * (4000 + 100 + (8000 + 2*600)) + 5000 = 31800 ms.
 *
 * 3.2 s of headroom, deliberately tight: the next leg added to the joiner
 * cascade should have to come here and re-argue the number, which is
 * exactly what did NOT happen when S6 reshaped the cascade under the old
 * constant. */
#define NAV_ORCH_UX_CEILING_MS \
    (JOIN_MAX_ATTEMPTS * (int)CONNECT_TIMEOUT_CONNECTING_MS + NAV_ORCH_TIMEOUT_MARGIN_MS)

_Static_assert(ORCH_JOIN_WORST_CASE_MS(STUN_BUDGET_DEFAULT_MS,
                                       RACE_BUDGET_DEFAULT_MS) +
                       NAV_ORCH_TIMEOUT_MARGIN_MS <=
                   NAV_ORCH_UX_CEILING_MS,
               "task #76: at SHIPPED DEFAULTS the derived NAV_WAIT_ORCHESTRATOR "
               "deadline now exceeds what this product considers an acceptable "
               "wait (JOIN_MAX_ATTEMPTS x CONNECT_TIMEOUT_CONNECTING_MS + nav "
               "margin). A cascade leg grew. Re-derive the deadline and re-argue "
               "the ceiling here — do NOT just raise the ceiling.");

/* [B] The joiner bound must contain a WHOLE race including BOTH
 * confirmation tails, at every legal config. If it does not, nav can cut
 * inside the tail of a punch that provably reached us — which is exactly
 * the misattribution the S6/S7 H-1 exemption exists to prevent, resurrected
 * one layer up. Checked at the clamp extremes, which bound the interior
 * because every term is monotone in stun and race. */
_Static_assert(ORCH_JOIN_ATTEMPT_MS(STUN_BUDGET_MIN_MS, RACE_BUDGET_MAX_MS) >=
                   RACE_HARD_CAP_MS(RACE_BUDGET_MAX_MS),
               "task #76: the joiner bound no longer contains one full race "
               "including both H-1 confirmation tails");

/* [C] The host bound COVERS one whole post-HOST_WAITING race, at every
 * legal config.
 *
 * This is the same property task #76 pinned, re-pointed at what now
 * delivers it. #76 could assert the stronger form — "the LADDER alone
 * dominates a race at its longest" — and did, because that was true then
 * and it justified writing the host bound as the ladder term only. Task
 * #96 removed 3 of the ladder's 4 PORTMAP_PROBE_BUDGET_MS legs (they
 * were repeated work; see ORCH_HOST_LADDER_MS) and the stronger form
 * stopped holding at one corner:
 *
 *     STUN at its floor, race at its ceiling
 *     ladder   = 4*200 + 11250 + 4*1000 + 3*5000 = 31,050 ms
 *     postwait = 100 + 30000 + 2*600           = 31,300 ms
 *
 * The response is NOT to weaken the guarantee — it is to stop relying on
 * a coincidence for it. ORCH_HOST_WORST_CASE_MS is now the max of the
 * two phases (they are separated by HOST_WAITING, which re-arms nav
 * every frame, so nav needs the larger and never the sum), and this
 * assert checks the property the deadline actually has to have. It is
 * not a tautology: delete the max and revert the bound to the bare
 * ladder, and this fails at exactly the corner above — which is the
 * regression it exists to catch.
 *
 * Checked at both race extremes; every term is monotone in stun and
 * race, so the interior is bounded by them. */
_Static_assert(ORCH_HOST_WORST_CASE_MS(STUN_BUDGET_MIN_MS, RACE_BUDGET_MAX_MS) >=
                   RESOLVE_POLL_MAX_MS + RACE_HARD_CAP_MS(RACE_BUDGET_MAX_MS),
               "task #76/#96: the host bound no longer covers one full "
               "post-HOST_WAITING race at the STUN floor / race ceiling corner — "
               "nav would cut inside a punch the host had already started");
_Static_assert(ORCH_HOST_WORST_CASE_MS(STUN_BUDGET_MIN_MS, RACE_BUDGET_MIN_MS) >=
                   RESOLVE_POLL_MAX_MS + RACE_HARD_CAP_MS(RACE_BUDGET_MIN_MS),
               "task #76/#96: the host bound no longer covers one full "
               "post-HOST_WAITING race at the minimum-config corner");

/* [D] Both role bounds must still COVER the measured worst-case cascades
 * this task was sized against, at the CONFIG CEILING — cutting below them
 * turns a slow-but-successful join into a spurious failure, which is a
 * worse bug than the one being fixed. Recomputed at current tip:
 *   joiner ceiling 200 + 2*(15000 + 100 + 30000 + 1200) = 92800 ms
 *   host   ceiling 4*200 + 11250 + 4*15000 + 3*5000     = 87050 ms
 * These are lower bounds on the derivation, so they fail if a leg is
 * DROPPED from the sum as well as if the sum is capped.
 *
 * THE HOST NUMBER MOVED, 120800 -> 87050, AND THAT IS NOT A CAP.
 * [D] guards against shortening the bound below work the code still
 * does. Task #96 did the opposite: it deleted the work. 120,800 was
 * 4 x PORTMAP_PROBE_BUDGET_MS because host_thread_fn really did re-run
 * the probe on all four rungs of the ladder — 45,000 ms of it, measured
 * (a failing probe reaches its full 11,250 ms budget in the field; see
 * s_portmap_failed_port for the numbers and for why the netns rig's
 * 1-38 ms says nothing about it). With the failure verdict latched, at
 * most one rung can reach try_portmap, so no legal maximal config can
 * spend the missing 33,750 ms and nothing slow-but-successful gets cut.
 * Re-derived here rather than re-asserted, per this block's own rule:
 * the number must be COMPUTED from the legs the code enforces. */
_Static_assert(ORCH_JOIN_WORST_CASE_MS(STUN_BUDGET_MAX_MS, RACE_BUDGET_MAX_MS) >= 92800,
               "task #76: the joiner bound fell below the measured 92800 ms "
               "config-ceiling cascade — a legal maximal config would now be "
               "aborted mid-attempt");
_Static_assert(ORCH_HOST_WORST_CASE_MS(STUN_BUDGET_MAX_MS, RACE_BUDGET_MAX_MS) >= 87050,
               "task #76/#96: the host bound fell below the 87050 ms "
               "config-ceiling cascade — a legal maximal config would now be "
               "aborted mid-ladder");
/* And the #96 saving is itself pinned: if a future change puts the
 * port-map probe back inside the ladder's multiplier (or adds a second
 * one-time leg of that size), the host bound climbs back over the old
 * 120,800 ms and this fails. That is the direction #96 exists to
 * prevent, so it gets a check of its own rather than a comment. */
_Static_assert(ORCH_HOST_WORST_CASE_MS(STUN_BUDGET_MAX_MS, RACE_BUDGET_MAX_MS) < 120800,
               "task #96: the host bound is back at its pre-#96 size — the "
               "port-map probe is being paid more than once per hosting session "
               "again. Latch the verdict, do not re-derive the ceiling.");

/* [E] THE JOINER'S PORT-MAP PROBE MUST NOT BECOME SERIAL. Task #121.
 *
 * #121 gave the joiner a port-map probe. Every assert above still holds
 * with its original numbers, which is a claim that deserves more than
 * "trust me": ORCH_JOIN_PORTMAP_SERIAL_MS is 0, so ORCH_JOIN_ATTEMPT_MS
 * is arithmetically what it was, and the shipped-defaults derivation is
 * still 200 + 2*(4000 + 100 + 0 + 9200) + 5000 = 31,800 ms against the
 * 35,000 ms ceiling, with the same 3,200 ms of headroom [A] was written
 * to protect. Nothing was weakened and no ceiling was raised.
 *
 * The reason the probe HAD to be non-blocking is the thing worth pinning,
 * because it is the constraint a future change will unknowingly violate.
 * Even the cheapest serial shape — ONE probe per join session, latched
 * exactly as #96 latches the host's, placed outside the attempt
 * multiplier — does not fit:
 *
 *     WORKER_STARTUP_DELAY_MS + PORTMAP_PROBE_BUDGET_MS
 *         + JOIN_MAX_ATTEMPTS * ORCH_JOIN_ATTEMPT_MS(defaults)
 *         + NAV_ORCH_TIMEOUT_MARGIN_MS
 *     = 200 + 11250 + 2*13300 + 5000 = 43,050 ms  >  35,000 ms
 *
 * 8,050 ms over. That is the measured cost of a FAILING probe
 * (s_portmap_failed_port: 8033 ms of UPnP plus 3510 ms of NAT-PMP, so
 * the outer deadline fires and the full budget is spent), and a failing
 * probe is the case a symmetric joiner with no IGD and no NAT-PMP
 * gateway actually hits — the common case, not the corner.
 *
 * So this assert is deliberately the NEGATIVE form: it asserts that the
 * serial shape does NOT fit. It fires in exactly two situations, and
 * both are ones somebody needs to look at rather than a build that
 * should quietly proceed:
 *   - PORTMAP_PROBE_BUDGET_MS shrinks (or the ceiling legitimately
 *     rises) far enough that a serial one-time probe WOULD fit. Then
 *     #121's fire-and-forget machinery — the spawn/collect pair, the
 *     detach disposition, the bind_port pinning on attempt 2 — is no
 *     longer buying anything, and the honest move is to delete it and
 *     call try_portmap from the joiner like the host does. This assert
 *     is how that becomes visible instead of staying complexity nobody
 *     revisits.
 *   - somebody edits the arithmetic above without understanding it.
 *
 * It constrains no RUNTIME behaviour — but it is not free of the rest of
 * the file, and saying otherwise would be the kind of overclaim this
 * block exists to prevent. Both sides move: raising
 * CONNECT_TIMEOUT_CONNECTING_MS past ~19,025 ms, or JOIN_MAX_ATTEMPTS to
 * 7, lifts NAV_ORCH_UX_CEILING_MS far enough that a serial probe WOULD
 * fit, and this fires even though [A] gained headroom. That is the
 * intended reading, not a false positive: the condition #121's
 * indirection exists to work around would have stopped being true, and
 * somebody should decide whether to keep the indirection rather than
 * inherit it. But the failure text points at #121 for a change made
 * elsewhere, so read it as "re-decide", not "you broke something". */
_Static_assert(WORKER_STARTUP_DELAY_MS + PORTMAP_PROBE_BUDGET_MS +
                       JOIN_MAX_ATTEMPTS * ORCH_JOIN_ATTEMPT_MS(STUN_BUDGET_DEFAULT_MS,
                                                                RACE_BUDGET_DEFAULT_MS) +
                       NAV_ORCH_TIMEOUT_MARGIN_MS >
                   NAV_ORCH_UX_CEILING_MS,
               "task #121: a SERIAL one-time joiner port-map probe would now fit "
               "under the UX ceiling. #121's fire-and-forget probe exists only "
               "because it did not. Either re-check this derivation, or delete "
               "join_portmap_spawn/join_portmap_collect and call try_portmap from "
               "join_attempt the way host_thread_fn does — but do not leave the "
               "indirection in place with its reason gone.");

/* And the zero itself, so the two halves cannot drift: if the joiner
 * probe is ever made to block, ORCH_JOIN_PORTMAP_SERIAL_MS must be set
 * to what it really costs, and [A] must then be re-argued rather than
 * silently satisfied by a stale 0. */
_Static_assert(ORCH_JOIN_PORTMAP_SERIAL_MS == 0,
               "task #121: the joiner port-map probe is no longer free. It was "
               "fire-and-forget (spawn + zero-wait collect) precisely so the "
               "joiner cascade did not grow. Re-derive assert [A] with the real "
               "cost — do NOT raise NAV_ORCH_UX_CEILING_MS.");

#ifdef NETPLAY_TEST_HOOKS
/* Task #132 P2: evaluate the ORCH_* cascade at CALLER-SUPPLIED budgets.
 *
 * The macros are file-local to this TU on purpose, and
 * DirectP2P_OrchWorstCaseMsForRole reads its budgets from live config, so
 * the only way to pin the arithmetic used to be to write config and read
 * a role bound back — which pins the SLOPE but never the ladder's actual
 * value. That is how #131 shipped: ORCH_HOST_LADDER_MS paid
 * WORKER_STARTUP_DELAY_MS once instead of once per rung, every derived
 * number was self-consistent with the wrong formula ([D] 86450, shipped
 * 42450, the corner 30450, #96's guard 120200), and nothing was red.
 *
 * This hook evaluates the real macros, with no config and no clock, so a
 * test can list the primitive constants INDEPENDENTLY, compute the ladder
 * itself, and compare. A test that derived its expectation from these
 * macros would pass through any error in them; the whole point is that
 * the expectation comes from somewhere else.
 *
 * Any out pointer may be NULL. */
void DirectP2P_TestHook_OrchCascade(int stun_ms, int race_ms,
                                    int* out_race_hard_cap_ms,
                                    int* out_host_ladder_ms,
                                    int* out_host_postwait_ms,
                                    int* out_host_worst_ms,
                                    int* out_join_attempt_ms,
                                    int* out_join_worst_ms) {
    if (out_race_hard_cap_ms != NULL) {
        *out_race_hard_cap_ms = RACE_HARD_CAP_MS(race_ms);
    }
    if (out_host_ladder_ms != NULL) {
        *out_host_ladder_ms = ORCH_HOST_LADDER_MS(stun_ms);
    }
    if (out_host_postwait_ms != NULL) {
        *out_host_postwait_ms = ORCH_HOST_POSTWAIT_MS(race_ms);
    }
    if (out_host_worst_ms != NULL) {
        *out_host_worst_ms = ORCH_HOST_WORST_CASE_MS(stun_ms, race_ms);
    }
    if (out_join_attempt_ms != NULL) {
        *out_join_attempt_ms = ORCH_JOIN_ATTEMPT_MS(stun_ms, race_ms);
    }
    if (out_join_worst_ms != NULL) {
        *out_join_worst_ms = ORCH_JOIN_WORST_CASE_MS(stun_ms, race_ms);
    }
}
#endif /* NETPLAY_TEST_HOOKS */

int DirectP2P_OrchWorstCaseMsForRole(Role role) {
    const int stun = stun_budget_ms();
    const int race = race_budget_ms();

    const int join_ms = ORCH_JOIN_WORST_CASE_MS(stun, race);
    const int host_ms = ORCH_HOST_WORST_CASE_MS(stun, race);

    switch (role) {
    case ROLE_JOIN: return join_ms;
    case ROLE_HOST: return host_ms;
    default:        break;
    }
    /* ROLE_NONE — role not yet published, or the orchestrator is idle.
     * Bound by whichever path is longer so the deadline is never short. */
    return (join_ms > host_ms) ? join_ms : host_ms;
}

int DirectP2P_OrchWorstCaseMs(void) {
    return DirectP2P_OrchWorstCaseMsForRole(DirectP2P_GetRole());
}

const char* DirectP2P_GetHostCode(void) {
    if (get_state() != DIRECT_P2P_HOST_WAITING) return "";
    return s_work.host_code;
}

const char* DirectP2P_GetStatusText(void) {
    return s_status;
}

#ifdef NETPLAY_TEST_HOOKS
/* S3-review HIGH-2: run the session-teardown callback exactly as
 * netplay.c's EXITING pass would (it is registered via
 * Netplay_SetSessionTeardownCallback in DirectP2P_Init). Lets the
 * harness drive the notify -> teardown -> FAILED_HANDSHAKE -> report
 * sequence without standing up a full GekkoNet session. */
void DirectP2P_TestHook_RunTeardown(void) {
    direct_p2p_on_teardown();
}

/* #147: straggler-slot seams (see direct_p2p.h). The simulated straggler
 * is a thread that parks in the real slot and spins until released — a
 * stand-in for an upnp_worker_fn stuck inside miniupnpc, which no test
 * can produce for real (there is no mock IGD; upnp_ensure_cached refuses
 * SSDP in harness builds by design). */
static SDL_AtomicInt s_test_straggler_release = { 0 };

static int SDLCALL test_straggler_thread_fn(void* data) {
    (void)data;
    while (!SDL_GetAtomicInt(&s_test_straggler_release)) {
        SDL_Delay(5);
    }
    return 0;
}

bool DirectP2P_TestHook_PortmapStragglerSim(void) {
    if (!portmap_backends_quiescent()) {
        return false; /* slot already occupied by a RUNNING straggler */
    }
    SDL_SetAtomicInt(&s_test_straggler_release, 0);
    SDL_Thread* t = SDL_CreateThread(test_straggler_thread_fn,
                                     "TestPortmapStraggler", NULL);
    if (t == NULL) {
        return false;
    }
    portmap_straggler_park(t, NULL);
    return true;
}

void DirectP2P_TestHook_PortmapStragglerRelease(void) {
    SDL_SetAtomicInt(&s_test_straggler_release, 1);
}

bool DirectP2P_TestHook_PortmapQuiescent(void) {
    return portmap_backends_quiescent();
}

/* S4-review HIGH-1b: punch-gate throttle seams (see direct_p2p.h). */
void DirectP2P_TestHook_SetHostAdvisoryScale(int scale) {
    s_host_advisory_scale = (scale > 1) ? scale : 1;
}

void DirectP2P_TestHook_SetHostLadder(int max_retries, int backoff_ms) {
    s_host_max_retries_override = (max_retries >= 0) ? max_retries : -1;
    s_host_retry_backoff_override_ms = (backoff_ms > 0) ? backoff_ms : 0;
}

void DirectP2P_TestHook_HostLadderCounters(int* out_rungs, int* out_portmap_probes,
                                           uint32_t* out_upnp_total_ms,
                                           uint32_t* out_stun_total_ms,
                                           uint32_t* out_backoff_ms) {
    if (out_rungs != NULL) *out_rungs = (int)s_work.host_rungs;
    if (out_portmap_probes != NULL) *out_portmap_probes = (int)s_work.host_portmap_probes;
    if (out_upnp_total_ms != NULL) *out_upnp_total_ms = s_work.t_upnp_total_ms;
    if (out_stun_total_ms != NULL) *out_stun_total_ms = s_work.t_stun_total_ms;
    if (out_backoff_ms != NULL) *out_backoff_ms = s_work.t_host_backoff_ms;
}

void DirectP2P_TestHook_HostLadderLimits(int* out_max_retries, int* out_backoff_ms) {
    if (out_max_retries != NULL) *out_max_retries = HOST_STUN_MAX_RETRIES;
    if (out_backoff_ms != NULL) *out_backoff_ms = HOST_STUN_RETRY_BACKOFF_MS;
}

void DirectP2P_TestHook_PunchGateReset(void) {
    host_punch_gate_reset();
}

void DirectP2P_TestHook_PunchGateNoteBad(const char* src_ip, uint32_t now_ms) {
    host_punch_gate_note_bad(src_ip, now_ms);
}

bool DirectP2P_TestHook_PunchGateIsMuted(const char* src_ip, uint32_t now_ms) {
    return host_punch_gate_is_muted(src_ip, now_ms);
}

void DirectP2P_TestHook_PunchGateCounters(int* bad_total) {
    if (bad_total) *bad_total = s_host_punch_bad_total;
}

/* S5: last do_handoff arguments (see s_test_handoff_*). Any pointer may
 * be NULL. `count` distinguishes "no handoff happened" from "a handoff
 * happened to 0.0.0.0:0". */
void DirectP2P_TestHook_LastHandoff(char* out_ip, int ip_cap, uint16_t* out_port,
                                    int* out_player, int* out_count) {
    if (out_ip != NULL && ip_cap > 0) {
        SDL_strlcpy(out_ip, s_test_handoff_ip, (size_t)ip_cap);
    }
    if (out_port) *out_port = s_test_handoff_port;
    if (out_player) *out_player = s_test_handoff_player;
    if (out_count) *out_count = s_test_handoff_count;
}

void DirectP2P_TestHook_ResetHandoff(void) {
    s_test_handoff_ip[0] = '\0';
    s_test_handoff_port = 0;
    s_test_handoff_player = 0;
    s_test_handoff_count = 0;
}

void DirectP2P_TestHook_PunchGateLimits(int* src_max_bad, uint32_t* mute_ms,
                                        int* src_table) {
    if (src_max_bad) *src_max_bad = HOST_PUNCH_SRC_MAX_BAD;
    if (mute_ms) *mute_ms = HOST_PUNCH_MUTE_MS;
    if (src_table) *src_table = HOST_PUNCH_SRC_TABLE;
}
#endif /* NETPLAY_TEST_HOOKS */

#endif /* ENABLE_NETPLAY */
