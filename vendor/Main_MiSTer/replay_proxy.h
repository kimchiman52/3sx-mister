#ifndef REPLAY_PROXY_H
#define REPLAY_PROXY_H

/*
 * replay_proxy — Stage S3a of docs/plan-osd-replay-browser.md.
 *
 * De-SDL'd port of src/replay/proxy_client.c for the HPS OSD wrapper. The
 * framing (u32be length prefix + JSON body), the JSON build/parse via cJSON,
 * the RFC-4648 base64 decoder, the get3sr manifest-then-per-game loop, and the
 * single-in-flight async worker pattern are all a faithful copy of the in-game
 * client — only the SDL shims are replaced:
 *
 *   SDL_Thread / SDL_CreateThread / SDL_WaitThread -> pthread_t / pthread_create / pthread_join
 *   SDL_AtomicInt / SDL_Set/GetAtomicInt           -> pthread_mutex-guarded int (RpAtomic below)
 *   SDL_malloc / SDL_free / SDL_calloc             -> malloc / free / calloc
 *   SDL_memcpy / SDL_memset / SDL_strlen / SDL_strcmp / SDL_snprintf -> libc
 *   SDL_strlcpy                                    -> rp_strlcpy (local, no glibc-2.38 dep)
 *   SDL_zero / SDL_zerop                           -> memset
 *   SDL_Log / SDL_LogError / SDL_LogWarn           -> fprintf(stderr, ...)
 *
 * Atomics choice: the async state int is guarded by a pthread_mutex (RpAtomic)
 * rather than a C11 `_Atomic` — a deliberate, toolchain-agnostic choice (the wrapper
 * builds .c with -std=gnu99, Makefile.full.3s-arm:81) and the plan's stated fallback.
 * The mutex doubles as the release/acquire barrier we need anyway. Correctness is identical to
 * the SDL version: the worker fully writes its results struct, THEN flips the state to
 * DONE; the poller reads the state under the mutex, and once it observes DONE it
 * pthread_join()s the worker (a full barrier) before reading the results.
 *
 * No SDL, no TLS, no cookie — only libc + POSIX sockets + pthread + cJSON, so it links
 * into the wrapper binary and a standalone host unit driver alike. The wrapper already
 * links -lpthread (Makefile.full.3s-arm:53) and vendored cJSON (S2).
 *
 * ARM safety: every wire integer is composed/decoded byte-wise (no htonl, no
 * host-endian assumption) — 32-bit ARM ships this code.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Typed outcome — mirrors proxy_client.h ProxyClientError exactly. The first
 * three are client-local (never sent by the proxy); the rest mirror the proxy's
 * typed error set. RpErrorHint() maps each to a short OSD line. */
typedef enum RpError {
    RP_ERR_NONE = 0,       /* success */
    RP_ERR_DISABLED,       /* host == "" — remote browsing disabled */
    RP_ERR_CONNECT,        /* DNS/connect/timeout — proxy unreachable (offline) */
    RP_ERR_PROTOCOL,       /* framing/parse error or oversize/short frame */
    RP_ERR_COOKIE_MISSING, /* proxy: cookie_missing */
    RP_ERR_CLOUDFLARE_403, /* proxy: cloudflare_403 — "catalog stale" */
    RP_ERR_UPSTREAM,       /* proxy: upstream_error — "try again later" */
    RP_ERR_BAD_REQUEST,    /* proxy: bad_request — client bug */
    RP_ERR_NOT_FOUND,      /* proxy: not_found — no converted/clean .3sr for this quark/game */
} RpError;

#define RP_QUARKID_MAX 64
#define RP_NAME_MAX 48
#define RP_GAMEID_MAX 64
#define RP_EMULATOR_MAX 32

/* One normalized search row (subset of the proxy's row shape — see fcade-proxy
 * README "Search success"). Only what the OSD needs to display a row + build a
 * fetch is kept. */
typedef struct RpRow {
    char quarkid[RP_QUARKID_MAX];
    long long date_ms;        /* ms-epoch; 0 when absent */
    long long duration_secs;  /* seconds; 0 when absent */
    char p1[RP_NAME_MAX];     /* players[0].name, "" if absent */
    char p2[RP_NAME_MAX];     /* players[1].name, "" if absent */
    int p1_rank;              /* players[0].rank: Fightcade 1..6 = E..S; 0 = unranked/absent */
    int p2_rank;              /* players[1].rank: same convention */
    char emulator[RP_EMULATOR_MAX]; /* e.g. "fbneo" */
    char gameid[RP_GAMEID_MAX];     /* e.g. "sfiii3nr1" */
    bool ranked;
    int num_matches;
} RpRow;

/* 50 is the proxy's hard server-side cap on `limit`. */
#define RP_MAX_ROWS 50

typedef struct RpSearchResults {
    RpRow rows[RP_MAX_ROWS];
    int count;
    RpError error;    /* RP_ERR_NONE iff the search succeeded */
    char detail[256]; /* free-text (proxy `detail` or a local message) — logs only */
} RpSearchResults;

typedef struct RpSearchParams {
    char host[256]; /* proxy host; "" disables remote browsing */
    int port;       /* proxy port (default 3479) */
    char gameid[RP_GAMEID_MAX];
    int offset;     /* >= 0 */
    int limit;      /* 1..50 */
    bool best;      /* Fightcade "best replays" filter */
    long long since;   /* ms-epoch lower bound; 0 = omit */
    char username[65]; /* proxy cap 64 chars + NUL; "" = omit */
} RpSearchParams;

/* Synchronous: connect to params->host:port, send one `search` request, read
 * one response frame, parse it. Blocking (bounded by internal timeouts).
 * Returns 0 on success (out->error == RP_ERR_NONE, rows/count filled), non-zero
 * on any failure (out->error set, count == 0). `out` is always fully written. */
int RpSearch(const RpSearchParams* params, RpSearchResults* out);

/* ---- Async wrapper (single in-flight, polled state) --------------------- */

typedef enum RpAsyncState {
    RP_ASYNC_IDLE = 0, /* no op pending; results (if any) already taken */
    RP_ASYNC_RUNNING,  /* worker thread in flight */
    RP_ASYNC_DONE,     /* worker finished — call the matching Take */
} RpAsyncState;

/* Start a search on a worker thread. A copy of *params is made. Returns false
 * (and does nothing) if a search is already RUNNING or a DONE result was not
 * yet taken. */
bool RpSearchAsync(const RpSearchParams* params);
/* Current async state (mutex-guarded read). */
RpAsyncState RpSearchPoll(void);
/* When DONE, joins the worker, copies results into *out, resets to IDLE, returns
 * true. Returns false (leaves *out untouched) in any other state. */
bool RpSearchTake(RpSearchResults* out);
/* Join any in-flight/finished worker and reset to IDLE (blocks until the
 * worker's current search returns). Safe to call when already IDLE. */
void RpSearchCancel(void);

/* Short (<= ~24 char) display line for an error. RP_ERR_NONE returns "".
 * Never NULL. */
const char* RpErrorHint(RpError err);

/* ---- get3sr: fetch a pre-converted .3sr + its .meta.json name sidecar ----
 * One quark can carry multiple in-session games; RpFetch3sr() writes BOTH files
 * per game to <replay_root>/<quarkid>/game_N.{3sr,meta.json}. The .meta.json is
 * NOT optional — names live only there (docs/3sr-format.md). A fetch-all
 * (game_index < 0) is a metadata-only MANIFEST request followed by one bounded
 * per-game request per listed game (so each frame stays under the frame cap).
 * A game_index >= 0 fetch is a single per-game request. */

#define RP_3SR_MAX_GAMES 32

typedef struct Rp3srGame {
    int game_index;
    char path_3sr[512];  /* on-disk path just written */
    char path_meta[512]; /* on-disk path just written */
    size_t size_3sr;
    size_t size_meta;
} Rp3srGame;

typedef struct Rp3srResults {
    Rp3srGame games[RP_3SR_MAX_GAMES];
    int count;
    RpError error;
    char detail[256];
} Rp3srResults;

typedef struct Rp3srParams {
    char host[256];
    int port;
    char quarkid[RP_QUARKID_MAX];
    char replay_root[400]; /* writes <replay_root>/<quarkid>/game_N.{3sr,meta.json} */
    int game_index;        /* -1 = every game the quark has; >= 0 = just that one */
} Rp3srParams;

/* Synchronous get3sr. Returns 0 on success (out->error == RP_ERR_NONE,
 * games/count filled with the paths just written), non-zero on failure. In
 * fetch-all mode a game that fails mid-loop is skipped (logged), not fatal — the
 * call only fails if EVERY game fails. `out` is always fully written. */
int RpFetch3sr(const Rp3srParams* params, Rp3srResults* out);

/* ---- Async wrapper — same pattern, a SEPARATE in-flight slot from search:
 * a fetch and a search can be in flight at the same time. -------------------- */
bool RpFetch3srAsync(const Rp3srParams* params);
RpAsyncState RpFetch3srPoll(void);
bool RpFetch3srTake(Rp3srResults* out);
void RpFetch3srCancel(void);

/* ---- convertstatus: poll a VPS convert job's state for the OSD row --------
 * Stage S5 (docs/plan-fcade-live-stream.md). Mirrors the proxy `convertstatus`
 * op (tools/fcade-proxy README "convert / convertstatus success"): request
 * {"op":"convertstatus","quarkid":…}; response {ok,state,progress,[games]}.
 * The convert job is auto-started by the game's first watchpoll (Stage S4), so
 * this is a read-only status poll — it never starts a job. Used to render
 * WATCH / CONVERTING n% / READY / FAILED on the REMOTE row-action page. */

typedef enum RpConvertState {
    RP_CONVERT_ABSENT = 0,  /* no job + nothing in store — not converted yet */
    RP_CONVERT_QUEUED,      /* accepted, waiting for a concurrency slot */
    RP_CONVERT_PULLING,     /* downloading the ggpo stream */
    RP_CONVERT_CONVERTING,  /* the -track-3sr runner is tracking the stream */
    RP_CONVERT_READY,       /* done — blobs are in the store, get3sr-able */
    RP_CONVERT_FAILED,      /* the job stopped; see detail */
    RP_CONVERT_UNKNOWN,     /* ok:true but an unrecognized state string */
} RpConvertState;

typedef struct RpConvertStatusResult {
    RpConvertState state;
    int progress;     /* 0..100 */
    RpError error;    /* RP_ERR_NONE iff the status call succeeded */
    char detail[256]; /* free-text (proxy `detail` or a local message) — logs */
} RpConvertStatusResult;

typedef struct RpConvertStatusParams {
    char host[256];
    int port;
    char quarkid[RP_QUARKID_MAX];
} RpConvertStatusParams;

/* Synchronous convertstatus. Returns 0 on success (out->error == RP_ERR_NONE,
 * state/progress filled), non-zero on failure (out->error set). `out` is always
 * fully written. */
int RpConvertStatus(const RpConvertStatusParams* params, RpConvertStatusResult* out);

/* ---- Async wrapper — same pattern, a SEPARATE in-flight slot from search and
 * get3sr: all three can be in flight at once. ------------------------------- */
bool RpConvertStatusAsync(const RpConvertStatusParams* params);
RpAsyncState RpConvertStatusPoll(void);
bool RpConvertStatusTake(RpConvertStatusResult* out);
void RpConvertStatusCancel(void);

/* ---- Wrapper-side config accessor ----------------------------------------
 * The game reads replay-proxy-host/replay-proxy-port from its config file; the
 * wrapper needs the same values to populate params->host/port. These read the
 * game config file (INI `key = value`, same parser shape as
 * thirdsarm_wrapper.cpp read_runtime_config_value) so S3b can decide whether
 * REMOTE is enabled (host "" => disabled). */

typedef struct RpProxyConfig {
    char host[256]; /* "" when unset / file missing => remote disabled */
    int port;       /* defaults to 3479 */
} RpProxyConfig;

/* Load host/port from `config_path`. Always fully writes *out (host "", port
 * 3479 defaults first). Returns true if the file was opened (even if the keys
 * are absent — the defaults then stand), false if the file could not be read. */
bool RpConfigLoadFrom(const char* config_path, RpProxyConfig* out);

/* Convenience: RpConfigLoadFrom() against the canonical on-device game config
 * path (/media/fat/games/3s-arm/config). */
bool RpConfigLoad(RpProxyConfig* out);

#ifdef __cplusplus
}
#endif

#endif /* REPLAY_PROXY_H */
