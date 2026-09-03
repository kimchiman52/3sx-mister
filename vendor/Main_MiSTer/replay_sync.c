/*
 * replay_sync.c — see replay_sync.h for the contract and the storage posture.
 *
 * One refresh cycle is:
 *
 *   1. paged `search` calls (offset 0/50/100, limit 50) with best=true,
 *      gameid=sfiii3nr1 and the weekly `since`, until RS_SET_MAX quarks are in
 *      hand — RP_MAX_ROWS is a hard server-side cap of 50, so a 90-quark set is
 *      two requests, not one;
 *   2. one `get3sr` fetch-all per quarkid that is not already on disk, paced
 *      apart so we never burst at the upstream API;
 *   3. one rewrite of <replay_root>/manifest.json.
 *
 * The set is the first RS_SET_MAX rows IN SERVER ORDER. No client-side sort and no
 * ranking key of our own: the viewer shuffles, so any ordering we imposed would
 * be thrown away immediately.
 *
 * Nothing here blocks. Every network call goes through replay_proxy's async
 * slots (search and get3sr are independent slots) and is drained across
 * successive ReplaySyncTick() calls.
 */

#include "replay_sync.h"

#include "cJSON.h"
#include "replay_proxy.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---- The set ------------------------------------------------------------ */

/* Fightcade's romset id for Street Fighter III: 3rd Strike (the "nr1" revision
 * every Fightcade match is played on). */
#define RS_GAMEID "sfiii3nr1"

/* How many quarks the weekly set downloads. The cap is ours, not the server's.
 *
 * SIZED AGAINST RB_MAX_ENTRIES (src/replay/replay_browser_scan.h) — CHANGE
 * BOTH. A quark is a whole session between two players, not one game: over 15
 * real quarks it holds 5.7 games on average (median 5), and each game lands as
 * its own game_N.3sr that the game side enumerates as one RbEntry. So the two
 * caps are one arithmetic:
 *
 *     RB_MAX_ENTRIES / 5.7 games-per-quark = quarks worth downloading
 *     512 / 5.7 = 89.8  ->  RS_SET_MAX = 90
 *
 * At the old 150 the wrapper downloaded ~60 quarks past what RbScan could
 * ever enumerate — bandwidth and SD writes for replays the viewer never saw. */
#define RS_SET_MAX 90

/* Search pages to walk at RP_MAX_ROWS rows each. rs_search_take() also stops
 * early on `s_quark_count >= RS_SET_MAX`, so this is the ceiling on requests,
 * not on the set: it only has to be able to SUPPLY RS_SET_MAX quarks once
 * duplicates across pages are deduped. */
#define RS_PAGES 3

/* Compile-time proof that RS_PAGES pages can actually supply RS_SET_MAX: if
 * either cap or replay_proxy's row cap moves such that the pages can no longer
 * fill the set, this fails to compile instead of silently fetching a short
 * set. */
typedef char rs_pagecount_static_assert[(RS_SET_MAX <= RS_PAGES * RP_MAX_ROWS) ? 1 : -1];

/* ---- Time ---------------------------------------------------------------- */

#define RS_DAY_MS 86400000LL
#define RS_WEEK_MS (7LL * RS_DAY_MS)

/* The daily refresh instant, as an offset into the UTC day: 09:00 UTC.
 *
 * WHY UTC AND NOT LOCAL TIME — do not "correct" this to a local-time schedule.
 * Fightcade's weekly window rolls at UTC midnight (see
 * ReplaySyncWeeklySinceMs), which is 8pm Eastern the previous evening. Firing
 * nine hours after that boundary — 09:00 UTC, i.e. 05:00 EDT / 04:00 EST — reads
 * a set that has been settled for nine hours, and the gap stays exactly nine
 * hours all year. A schedule expressed in Eastern local time would instead
 * drift an hour twice a year against a boundary that never moves, which is the
 * one thing the offset exists to hold constant. Nine hours of slack is also far
 * more than any plausible DST-adjacent confusion, so pinning the UTC number is
 * strictly safer than tracking a zone the device may not even have tzdata for.
 * (A MiSTer has no RTC; its clock comes from NTP, which is UTC.) */
#define RS_DUE_HOUR_UTC_MS (9LL * 3600000LL)

/* A MiSTer has no battery-backed clock: before NTP lands, time() can report
 * something absurd. Refuse to run a cycle then — a bogus `since` would ask the
 * upstream API for a nonsense window, and a bogus `fetched_at` written into the
 * manifest would suppress the next real refresh. 2025-01-01T00:00:00Z. */
#define RS_MIN_SANE_MS 1735689600000LL

long long ReplaySyncWeeklySinceMs(long long now_ms) {
    if (now_ms <= 0)
        return 0;
    /* Floor to the UTC day, then step back seven days. Identical arithmetic to
     * the Fightcade client's `Date.now() - Date.now() % 864e5 - 6048e5`. */
    long long since = now_ms - (now_ms % RS_DAY_MS) - RS_WEEK_MS;
    return (since < 0) ? 0 : since;
}

long long ReplaySyncLastDueMs(long long now_ms) {
    long long midnight = now_ms - (now_ms % RS_DAY_MS);
    long long due = midnight + RS_DUE_HOUR_UTC_MS;
    if (due > now_ms)
        due -= RS_DAY_MS; /* today's instant has not arrived yet */
    return due;
}

/* ---- Pacing -------------------------------------------------------------- */

/* How often an IDLE tick bothers to look at the clock/config/manifest at all.
 * ReplaySyncTick() runs at ~1 kHz; everything below this gate is one
 * clock_gettime() and a compare. */
#define RS_IDLE_POLL_MS 5000LL

/* Cadence of the async-slot polls once a cycle is running. */
#define RS_BUSY_POLL_MS 50LL

/* Gaps between upstream requests. The proxy fronts an API that rate-limits and
 * degrades to non-JSON under load; replay_proxy already reports those as typed
 * errors, and the correct response to them is to go slower, never to retry in a
 * tight loop. Nothing here is latency-sensitive — a full cold set takes a few
 * minutes of wall clock and that is fine. */
#define RS_PAGE_GAP_MS 750LL
#define RS_FETCH_GAP_MS 1500LL

/* After a cycle that could not even get its first page, wait this long before
 * trying again rather than re-attempting every RS_IDLE_POLL_MS. The daily due
 * check still governs: this only throttles retries within a day. */
#define RS_RETRY_BACKOFF_MS (30LL * 60LL * 1000LL)

/* How many already-cached quarks one tick may stat past before yielding. */
#define RS_SKIP_PER_TICK 8

/* ---- Paths --------------------------------------------------------------- */

#define RS_PATH_MAX 512
#define RS_MANIFEST_NAME "manifest.json"
/* Refuse to parse an absurd manifest — ours is a few KB. */
#define RS_MANIFEST_READ_CAP (256u * 1024u)

/* ---- State --------------------------------------------------------------- */

typedef enum RsState {
    RS_IDLE = 0,     /* nothing in flight; waiting for the next due check */
    RS_SEARCH_KICK,  /* a page is due to be sent (paced) */
    RS_SEARCH_WAIT,  /* a search is in replay_proxy's async slot */
    RS_FETCH_KICK,   /* the next quark fetch is due to be sent (paced) */
    RS_FETCH_WAIT,   /* a get3sr is in replay_proxy's async slot */
} RsState;

static RsState s_state = RS_IDLE;

static long long s_next_action_ms = 0; /* monotonic gate for the state above */

static RpProxyConfig s_cfg;
static char s_root[400];

/* The set under construction. s_ok[i] is true once quark i is confirmed present
 * on disk as a complete .3sr + .meta.json pair — only those go in the manifest. */
static char s_quarks[RS_SET_MAX][RP_QUARKID_MAX];
static bool s_ok[RS_SET_MAX];
static int s_quark_count = 0;

static int s_page = 0;
static int s_fetch_i = 0;
static long long s_cycle_since_ms = 0;
static int s_n_fetched = 0;
static int s_n_cached = 0;
static int s_n_failed = 0;
static bool s_search_complete = false;

/* One-shot log latches, so a permanently-disabled or clock-less device does not
 * print the same line every RS_IDLE_POLL_MS forever. */
static bool s_logged_disabled = false;
static bool s_logged_clock = false;

/* ---- Small helpers ------------------------------------------------------- */

static long long rs_mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static long long rs_wall_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static size_t rs_strlcpy(char* dst, const char* src, size_t dst_sz) {
    size_t i = 0;
    if (dst_sz == 0)
        return strlen(src);
    for (; src[i] != '\0' && i + 1 < dst_sz; i++)
        dst[i] = src[i];
    dst[i] = '\0';
    return strlen(src);
}

/* A quarkid becomes a path component (replay_proxy builds
 * "<replay_root>/<quarkid>/game_N.3sr"), so it is validated HERE, at the point
 * it enters the wrapper from the network, rather than trusted downstream.
 *
 * The character set is exactly the proxy's own documented rule for a quarkid,
 * [A-Za-z0-9_-] (tools/fcade-proxy/README.md, the `get3sr` request section) —
 * no '.' and no '/', so no path component we build from one can traverse
 * anywhere, and a row the proxy would itself reject never costs a request. */
static bool rs_quark_valid(const char* q) {
    if (q == NULL || q[0] == '\0')
        return false;
    for (const char* p = q; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '_' && c != '-')
            return false;
    }
    return true;
}

/* True iff `path` is a regular file with a non-zero size. */
static bool rs_file_present(const char* path) {
    struct stat st;
    if (path == NULL || path[0] == '\0')
        return false;
    if (stat(path, &st) != 0)
        return false;
    return S_ISREG(st.st_mode) && st.st_size > 0;
}

/* Is this quark already fully on disk?
 *
 * "Fully" means game_0.3sr AND game_0.meta.json — the sidecar is checked
 * deliberately. The .meta.json is where the player names and ranks the viewer
 * overlays come from; a .3sr without one renders a blank overlay with NO error
 * anywhere. replay_proxy unlinks both halves if a write fails mid-pair, but it
 * cannot defend against the process dying between the two writes, so treating a
 * lone .3sr as "not cached" is what repairs that case: the next cycle re-fetches
 * the quark and overwrites both files. Add-only, no deletion.
 *
 * A quark with several in-session games whose game_0 landed but whose game_1 did
 * not stays "cached" and is not retried. That is deliberate: the viewer plays
 * what is there, and re-fetching whole quarks to chase a missing second game
 * would multiply upstream traffic for very little. */
static bool rs_quark_cached(const char* quarkid) {
    char p3[RS_PATH_MAX];
    char pm[RS_PATH_MAX];
    snprintf(p3, sizeof(p3), "%s/%s/game_0.3sr", s_root, quarkid);
    snprintf(pm, sizeof(pm), "%s/%s/game_0.meta.json", s_root, quarkid);
    return rs_file_present(p3) && rs_file_present(pm);
}

static void rs_manifest_path(char* out, size_t out_sz) {
    snprintf(out, out_sz, "%s/%s", s_root, RS_MANIFEST_NAME);
}

/* ---- Manifest ------------------------------------------------------------ */

/* Reads <root>/manifest.json and returns its `fetched_at` (ms epoch), or 0 when
 * the file is missing, unreadable, unparseable or has no usable timestamp — all
 * of which mean "never fetched", i.e. a refresh is due. */
static long long rs_manifest_fetched_at(void) {
    char path[RS_PATH_MAX];
    rs_manifest_path(path, sizeof(path));

    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return 0;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long len = ftell(f);
    if (len <= 0 || (unsigned long)len > RS_MANIFEST_READ_CAP) {
        fclose(f);
        return 0;
    }
    rewind(f);

    char* buf = (char*)malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return 0;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';

    cJSON* root = cJSON_ParseWithLength(buf, got);
    free(buf);
    if (root == NULL)
        return 0;

    long long fetched_at = 0;
    const cJSON* j = cJSON_GetObjectItemCaseSensitive(root, "fetched_at");
    if (cJSON_IsNumber(j) && j->valuedouble > 0.0) {
        /* ms-epoch values are ~1.8e12, far inside a double's exact-integer
         * range (2^53), so this round-trips exactly. */
        fetched_at = (long long)j->valuedouble;
    }
    cJSON_Delete(root);
    return fetched_at;
}

/* Rewrites <root>/manifest.json with the quarkids that are confirmed present on
 * disk. Written to a .tmp and rename()d into place so a reader never sees a
 * half-written file. rename() over the previous manifest is the only
 * destructive filesystem operation this module performs. */
static bool rs_manifest_write(long long fetched_at) {
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_CreateArray();
    if (root == NULL || arr == NULL) {
        if (root) cJSON_Delete(root);
        if (arr) cJSON_Delete(arr);
        return false;
    }

    int listed = 0;
    for (int i = 0; i < s_quark_count; i++) {
        if (!s_ok[i])
            continue;
        cJSON* s = cJSON_CreateString(s_quarks[i]);
        if (s == NULL)
            continue;
        cJSON_AddItemToArray(arr, s);
        listed++;
    }

    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddNumberToObject(root, "fetched_at", (double)fetched_at);
    cJSON_AddStringToObject(root, "gameid", RS_GAMEID);
    cJSON_AddBoolToObject(root, "best", 1);
    cJSON_AddNumberToObject(root, "since", (double)s_cycle_since_ms);
    cJSON_AddNumberToObject(root, "count", listed);
    cJSON_AddBoolToObject(root, "complete", (s_search_complete && s_n_failed == 0) ? 1 : 0);
    cJSON_AddItemToObject(root, "quarks", arr);

    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (text == NULL)
        return false;

    char path[RS_PATH_MAX];
    char tmp[RS_PATH_MAX];
    rs_manifest_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    bool ok = false;
    FILE* f = fopen(tmp, "wb");
    if (f != NULL) {
        size_t n = strlen(text);
        ok = (fwrite(text, 1, n, f) == n) && (fputc('\n', f) != EOF);
        if (fclose(f) != 0)
            ok = false;
    }
    free(text);

    if (ok && rename(tmp, path) != 0)
        ok = false;
    if (!ok)
        remove(tmp); /* our own temp file only — never a replay */

    if (ok)
        fprintf(stderr, "replay_sync: manifest written: %s (%d quark(s))\n", path, listed);
    else
        fprintf(stderr, "replay_sync: manifest WRITE FAILED: %s\n", path);
    return ok;
}

/* ---- Cycle ---------------------------------------------------------------- */

static void rs_cycle_end(const char* why, long long backoff_ms) {
    fprintf(stderr, "replay_sync: cycle end (%s): %d fetched, %d already cached, %d failed, set %d (cap %d)\n", why,
            s_n_fetched, s_n_cached, s_n_failed, s_quark_count, RS_SET_MAX);
    s_state = RS_IDLE;
    s_next_action_ms = rs_mono_ms() + backoff_ms;
}

static void rs_search_kick(void) {
    RpSearchParams p;
    memset(&p, 0, sizeof(p));
    rs_strlcpy(p.host, s_cfg.host, sizeof(p.host));
    p.port = s_cfg.port;
    rs_strlcpy(p.gameid, RS_GAMEID, sizeof(p.gameid));
    p.offset = s_page * RP_MAX_ROWS;
    p.limit = RP_MAX_ROWS;
    p.best = true;
    p.since = s_cycle_since_ms;

    if (!RpSearchAsync(&p)) {
        fprintf(stderr, "replay_sync: search slot busy, aborting cycle\n");
        rs_cycle_end("search slot busy", RS_RETRY_BACKOFF_MS);
        return;
    }
    fprintf(stderr, "replay_sync: search page %d (offset %d, limit %d, best, since %lld)\n", s_page, p.offset, p.limit,
            s_cycle_since_ms);
    s_state = RS_SEARCH_WAIT;
    s_next_action_ms = rs_mono_ms() + RS_BUSY_POLL_MS;
}

/* Move from the search phase to the fetch phase. */
static void rs_begin_fetch_phase(void) {
    fprintf(stderr, "replay_sync: set is %d quark(s); starting fetch\n", s_quark_count);
    s_fetch_i = 0;
    s_state = RS_FETCH_KICK;
    s_next_action_ms = rs_mono_ms(); /* first fetch goes immediately */
}

static void rs_search_take(void) {
    RpSearchResults res;
    if (!RpSearchTake(&res))
        return;

    if (res.error != RP_ERR_NONE) {
        fprintf(stderr, "replay_sync: search page %d failed: %s (%s)\n", s_page, RpErrorHint(res.error), res.detail);
        if (s_quark_count == 0) {
            /* Nothing at all — no set, so nothing to fetch and no manifest to
             * write. Back off; the daily due check still stands. */
            rs_cycle_end("search failed", RS_RETRY_BACKOFF_MS);
            return;
        }
        /* A later page failed: keep the rows we do have. A partial set is fine
         * — the viewer plays what is there. */
        rs_begin_fetch_phase();
        return;
    }

    int added = 0;
    for (int i = 0; i < res.count && s_quark_count < RS_SET_MAX; i++) {
        const char* q = res.rows[i].quarkid;
        if (!rs_quark_valid(q)) {
            fprintf(stderr, "replay_sync: refusing suspicious quarkid, skipped\n");
            continue;
        }
        bool dup = false;
        for (int j = 0; j < s_quark_count; j++) {
            if (strcmp(s_quarks[j], q) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        rs_strlcpy(s_quarks[s_quark_count], q, RP_QUARKID_MAX);
        s_ok[s_quark_count] = false;
        s_quark_count++;
        added++;
    }
    fprintf(stderr, "replay_sync: search page %d returned %d row(s), %d new (set now %d)\n", s_page, res.count, added,
            s_quark_count);

    /* A short page means the server has no more rows in this window. */
    const bool short_page = (res.count < RP_MAX_ROWS);
    s_page++;
    if (short_page || s_page >= RS_PAGES || s_quark_count >= RS_SET_MAX) {
        /* Every page we asked for came back without an error, so the set is the
         * full intended one (however many rows the window actually held). */
        s_search_complete = true;
        if (s_quark_count == 0) {
            /* The window legitimately held nothing, or the upstream returned an
             * empty ok:true page. Either way, back off — re-searching every
             * RS_IDLE_POLL_MS would be exactly the retry storm this module is
             * supposed to avoid. */
            rs_cycle_end("empty set", RS_RETRY_BACKOFF_MS);
            return;
        }
        rs_begin_fetch_phase();
        return;
    }

    s_state = RS_SEARCH_KICK;
    s_next_action_ms = rs_mono_ms() + RS_PAGE_GAP_MS;
}

static void rs_fetch_kick(void) {
    /* Skip quarks already complete on disk. The weekly window slides by one day
     * at a time, so on a warm device most of the set is already here and this
     * loop is what keeps the daily refresh cheap.
     *
     * Bounded per tick: each skip costs two stat()s, and on a fully-warm set the
     * whole RS_SET_MAX would otherwise be walked inside ONE iteration of the
     * wrapper's ~1 kHz loop. Spreading them over successive ticks keeps every
     * call short. */
    int skipped_here = 0;
    while (s_fetch_i < s_quark_count && skipped_here < RS_SKIP_PER_TICK && rs_quark_cached(s_quarks[s_fetch_i])) {
        s_ok[s_fetch_i] = true;
        s_n_cached++;
        s_fetch_i++;
        skipped_here++;
    }
    if (skipped_here == RS_SKIP_PER_TICK && s_fetch_i < s_quark_count) {
        s_next_action_ms = rs_mono_ms(); /* resume the walk on the next tick */
        return;
    }
    if (s_fetch_i >= s_quark_count) {
        /* A failed manifest write (read-only or full SD) must NOT re-run the
         * whole cycle every RS_IDLE_POLL_MS: without a fetched_at on disk the
         * due check stays true forever, so back off explicitly. */
        const bool wrote = rs_manifest_write(rs_wall_ms());
        rs_cycle_end(wrote ? "done" : "manifest write failed", wrote ? RS_IDLE_POLL_MS : RS_RETRY_BACKOFF_MS);
        return;
    }

    Rp3srParams p;
    memset(&p, 0, sizeof(p));
    rs_strlcpy(p.host, s_cfg.host, sizeof(p.host));
    p.port = s_cfg.port;
    rs_strlcpy(p.quarkid, s_quarks[s_fetch_i], sizeof(p.quarkid));
    rs_strlcpy(p.replay_root, s_root, sizeof(p.replay_root));
    p.game_index = -1; /* every game in the quark, .3sr + .meta.json for each */

    if (!RpFetch3srAsync(&p)) {
        fprintf(stderr, "replay_sync: get3sr slot busy, retrying shortly\n");
        s_next_action_ms = rs_mono_ms() + RS_FETCH_GAP_MS;
        return;
    }
    s_state = RS_FETCH_WAIT;
    s_next_action_ms = rs_mono_ms() + RS_BUSY_POLL_MS;
}

static void rs_fetch_take(void) {
    Rp3srResults res;
    if (!RpFetch3srTake(&res))
        return;

    const char* q = s_quarks[s_fetch_i];
    if (res.error != RP_ERR_NONE) {
        fprintf(stderr, "replay_sync: fetch %s failed: %s (%s)\n", q, RpErrorHint(res.error), res.detail);
        s_n_failed++;
    } else {
        /* POSTCONDITION: BOTH files of every returned game must actually be on
         * disk with a non-zero size. replay_proxy reports the two paths and the
         * two sizes precisely so this can be checked, and it is checked because
         * the failure it guards against is silent: a .3sr whose .meta.json is
         * missing plays fine and renders a blank overlay, with no error
         * anywhere. A quark that fails this is left out of the manifest and
         * retried on the next cycle. */
        int good = 0;
        for (int i = 0; i < res.count; i++) {
            const Rp3srGame* g = &res.games[i];
            if (g->size_3sr > 0 && g->size_meta > 0 && rs_file_present(g->path_3sr) && rs_file_present(g->path_meta)) {
                good++;
            } else {
                fprintf(stderr, "replay_sync: %s game_%d INCOMPLETE PAIR (3sr=%s %zu B, meta=%s %zu B)\n", q,
                        g->game_index, g->path_3sr, g->size_3sr, g->path_meta, g->size_meta);
            }
        }
        if (good > 0 && good == res.count) {
            s_ok[s_fetch_i] = true;
            s_n_fetched++;
            fprintf(stderr, "replay_sync: fetched %s (%d game(s))\n", q, good);
        } else {
            s_n_failed++;
            fprintf(stderr, "replay_sync: fetch %s incomplete (%d/%d games verified), not listed\n", q, good,
                    res.count);
        }
    }

    s_fetch_i++;
    s_state = RS_FETCH_KICK;
    s_next_action_ms = rs_mono_ms() + RS_FETCH_GAP_MS;
}

/* Decide whether a refresh is due, and if so set the cycle up. */
static void rs_idle_check(void) {
    const long long now_ms = rs_wall_ms();
    if (now_ms < RS_MIN_SANE_MS) {
        if (!s_logged_clock) {
            fprintf(stderr, "replay_sync: system clock not set yet (%lld), waiting for NTP\n", now_ms);
            s_logged_clock = true;
        }
        return;
    }
    s_logged_clock = false;

    memset(&s_cfg, 0, sizeof(s_cfg));
    if (!RpConfigLoad(&s_cfg) || s_cfg.host[0] == '\0') {
        if (!s_logged_disabled) {
            fprintf(stderr, "replay_sync: no replay-proxy-host configured, remote refresh disabled\n");
            s_logged_disabled = true;
        }
        return;
    }
    s_logged_disabled = false;

    /* The replays root comes from the SAME game config file, never from a
     * wrapper-side literal: a hardcoded copy is what previously had to be "kept
     * in step" with the game's `replays-root` key by hand. */
    RpReplaysRoot(s_root, sizeof(s_root));

    const long long due = ReplaySyncLastDueMs(now_ms);
    const long long fetched_at = rs_manifest_fetched_at();
    if (fetched_at >= due)
        return; /* already refreshed since the most recent scheduled instant */

    s_quark_count = 0;
    s_page = 0;
    s_fetch_i = 0;
    s_n_fetched = 0;
    s_n_cached = 0;
    s_n_failed = 0;
    s_search_complete = false;
    memset(s_ok, 0, sizeof(s_ok));
    s_cycle_since_ms = ReplaySyncWeeklySinceMs(now_ms);

    fprintf(stderr,
            "replay_sync: refresh due (now %lld, due %lld, manifest %lld); root=%s proxy=%s:%d since=%lld\n", now_ms,
            due, fetched_at, s_root, s_cfg.host, s_cfg.port, s_cycle_since_ms);

    s_state = RS_SEARCH_KICK;
    s_next_action_ms = rs_mono_ms();
}

/* ---- Tick ---------------------------------------------------------------- */

void ReplaySyncTick(void) {
    const long long now = rs_mono_ms();
    if (now < s_next_action_ms)
        return;

    switch (s_state) {
    case RS_IDLE:
        s_next_action_ms = now + RS_IDLE_POLL_MS;
        rs_idle_check();
        break;

    case RS_SEARCH_KICK:
        rs_search_kick();
        break;

    case RS_SEARCH_WAIT:
        if (RpSearchPoll() == RP_ASYNC_DONE)
            rs_search_take();
        else
            s_next_action_ms = now + RS_BUSY_POLL_MS;
        break;

    case RS_FETCH_KICK:
        rs_fetch_kick();
        break;

    case RS_FETCH_WAIT:
        if (RpFetch3srPoll() == RP_ASYNC_DONE)
            rs_fetch_take();
        else
            s_next_action_ms = now + RS_BUSY_POLL_MS;
        break;
    }
}
