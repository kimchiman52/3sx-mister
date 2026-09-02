#!/usr/bin/env node
// 3SX fcade-proxy: VPS-side search/catalog proxy for the Fightcade
// `searchquarks` API. See ../../docs/plan-fcade-replay-browser.md Step F1
// (§4.5) and §2.8 for the design rationale.
//
// The device (MiSTer core) never talks TLS or Cloudflare directly. This
// service terminates HTTPS+Cloudflare toward fightcade.com (cookie kept
// server-side) and exposes a dead-simple length-framed JSON protocol over
// plain TCP.
//
// Wire framing: u32be length prefix + UTF-8 JSON payload, one frame per
// request and one frame per response, on a persistent connection. This is
// deliberately the SAME framing the core's Fightcade *stream* client already
// speaks (src/replay/fcade_stream.c:253-268 `recv_frame`/`get_u32be`, itself a
// byte-for-byte port of tools/fcade-replays/fcade_replay_tool.py's
// `recv_frame`/`_u32be_from`, :73-75/:59-60) — big-endian, no host-endian
// assumption, so the same decode helper the device already has for the
// stream protocol works unmodified for this proxy protocol.
//
// Fixed op set only (search, status). Never proxies arbitrary URLs — the
// upstream endpoint is a compile-time constant, not client input.
//
// Offline catalog fallback (option B): when FCADE_CATALOG_FILE is set and
// points at a valid catalog file, `search` is served entirely from that
// static file — no cookie, no cache, no upstream call, ever. See
// README.md "Offline catalog (option B)" and loadCatalog()/searchCatalog()
// below.

'use strict';

const net = require('net');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { spawn } = require('child_process');

const pkg = require('./package.json');

// --- Tunables (all overridable via env for tests; defaults are the
// production values described in README.md) ------------------------------

const DEFAULT_PORT = Number(process.env.FCADE_PROXY_PORT) || 3479;

const UPSTREAM_API_URL = process.env.FCADE_PROXY_API_URL || 'https://www.fightcade.com/api/';
const UPSTREAM_TIMEOUT_MS = Number(process.env.FCADE_PROXY_UPSTREAM_TIMEOUT_MS) || 10_000;

// Politeness: minimum spacing between actual upstream calls. Requests that
// arrive faster than this are queued (delayed), never dropped, up to
// MAX_QUEUE_WAIT_MS; beyond that we give up and surface a typed error rather
// than let a connection hang indefinitely.
const MIN_UPSTREAM_INTERVAL_MS = Number(process.env.FCADE_PROXY_MIN_UPSTREAM_INTERVAL_MS) || 2000;
const MAX_QUEUE_WAIT_MS = Number(process.env.FCADE_PROXY_MAX_QUEUE_WAIT_MS) || 30_000;

// Cache TTLs. Plan (§4.5/Step F1): "15 min for page 0, longer for `best`".
// Policy adopted here (documented in README): offset===0 AND NOT best -> 15
// min; every other page (offset>0, or best regardless of offset) -> 60 min.
// Rationale: "best" pages change far less often than the live offset-0 feed,
// so they get the longer TTL even at offset 0.
const CACHE_TTL_FIRST_PAGE_MS = Number(process.env.FCADE_PROXY_CACHE_TTL_FIRST_PAGE_MS) || 15 * 60 * 1000;
const CACHE_TTL_OTHER_MS = Number(process.env.FCADE_PROXY_CACHE_TTL_OTHER_MS) || 60 * 60 * 1000;
const CACHE_MAX_ENTRIES = Number(process.env.FCADE_PROXY_CACHE_MAX_ENTRIES) || 500;

// Input hardening.
const MAX_FRAME_BYTES = Number(process.env.FCADE_PROXY_MAX_FRAME_BYTES) || 16 * 1024;
const CONN_IDLE_TIMEOUT_MS = Number(process.env.FCADE_PROXY_IDLE_TIMEOUT_MS) || 30_000;
const MAX_LIMIT = 50;
const DEFAULT_LIMIT = 15; // matches upstream tool's --page-size default

const COOKIE_ENV = 'FCADE_COOKIE';
const COOKIE_FILE_ENV = 'FCADE_COOKIE_FILE';
const DEFAULT_COOKIE_FILE = path.join(__dirname, 'fcade-cookie.txt');

// Offline catalog fallback (option B / plan §4.5 F1 fallback). See
// README.md "Offline catalog (option B)" for the full flow: Fightcade's
// /api/ sits behind a Cloudflare MANAGED challenge that no scripted client
// (curl, curl_cffi with a real Chrome TLS fingerprint, Playwright
// headless+headed) can pass -- only a real browser tab can. So the catalog
// is generated FROM a real browser (browser-catalog.js, pasted into the
// fightcade.com console) and pushed here (push-catalog.sh) as a static
// file; this proxy then serves `search` entirely from it, no upstream call
// ever attempted, when FCADE_CATALOG_FILE is set and points at a file that
// exists and parses.
const CATALOG_FILE_ENV = 'FCADE_CATALOG_FILE';

// Ready-only catalog serving (docs/plan-bounded-pool-replay.md §5/§9 Stage 2):
// when on, `searchCatalog` drops rows whose quarkid isn't store-servable
// before sort/paging -- but ONLY for username-less (tab-browse) searches; a
// BY PLAYER search (username present) stays unfiltered/best-effort, same as
// today. Read live (like workTokenOk's FCADE_WORK_TOKEN, :263-269) rather
// than cached at module load, so a flip needs no restart and tests can
// toggle it mid-process. Default OFF: turning this on for real is a
// deliberate VPS-config decision (systemd unit env line), never a code
// default -- see the plan's Stage 2 rollout note.
function searchReadyOnlyEnabled() {
    return process.env.FCADE_SEARCH_READY_ONLY === '1';
}

// get3sr (Step F1b / docs/plan-osd-replay-browser.md Stage S1): pre-converted
// `.3sr` + `.meta.json` blobs, one per-quark subdirectory, produced offline by
// `tools/fcade-replays/publish_3sr.py` and shipped here by `push-3sr.sh` --
// see README.md "get3sr". Mirrors FCADE_CATALOG_FILE's "a path, re-checked
// per request, no restart needed" posture, but this is a directory tree, not
// a single file.
const THREESX_3SR_DIR_ENV = 'FCADE_3SR_DIR';
const DEFAULT_3SR_DIR = path.join(__dirname, '3sr');

// A full match ≈29 KB `.3sr` (docs/3sr-format.md §5) + a small `.meta.json`.
// Cap generously so a corrupt/oversize file on disk is refused rather than
// ever base64-encoded and shipped.
const MAX_3SR_FILE_BYTES = Number(process.env.FCADE_PROXY_MAX_3SR_BYTES) || 1024 * 1024; // 1 MiB
const MAX_META_FILE_BYTES = Number(process.env.FCADE_PROXY_MAX_META_BYTES) || 64 * 1024; // 64 KiB
const MAX_3SR_GAMES_PER_QUARK = 64; // generous cap on game_N per quark directory
const HEADER_MIN_3SR_BYTES = 28; // v1 .3sr header size (docs/3sr-format.md §1)

// The device rejects any response frame whose payload exceeds this (its
// PROXY_MAX_FRAME_LEN, src/replay/proxy_client.c:46) — and rejects the WHOLE
// frame, so an over-cap response makes the replay unreachable even when each
// game is individually small. Two consequences enforced below:
//   - whole-quark browsing (game_index omitted) returns a metadata-only
//     MANIFEST (no base64 blobs at all), so its frame stays tiny no matter how
//     many games the quark carries (a real 6-game quark base64'd to 243 KB =
//     92.7% of the cap; 7+ overflow);
//   - a single-game (game_index >= N) response is hard-guarded against this
//     ceiling and returns a typed error rather than ever emit an over-cap
//     frame the device would silently drop.
const DEVICE_MAX_FRAME_BYTES = Number(process.env.FCADE_PROXY_DEVICE_MAX_FRAME_BYTES) || 256 * 1024;
// Margin below the device cap for the JSON envelope ({"ok":true,"quarkid":...})
// so the assembled frame's total byte length stays safely under it.
const SINGLE_GAME_FRAME_MARGIN = 4096;

// --- convert-on-select (Stage S2 / docs/plan-fcade-live-stream.md) ---------
// On-demand VOD conversion: pull the ggpo replay stream + run the FBNeo
// `-track-3sr` live tracker (the S1 runner, built on the VPS) to drop
// `game_N.{3sr,meta.json}` into the SAME `3sr/<quarkid>/` store that the
// existing get3sr op serves unchanged. This is Option B of the plan (§3.B):
// a catalog quark with no pre-converted `.3sr` becomes watchable in
// ≈ duration/6 (the wire is ~6.2x real-time, §1.3) with ZERO device-engine
// change. All paths default to the deployed VPS layout; every knob is an env
// override so tests can point them at fixtures.
//
// Concurrency is capped at 1 by default: the program has deliberately never
// held more than one live ggpo stream connection at a time (politeness,
// docs/fcade-replay-notes.md §4). NO per-frame `.ram` dumps are ever written
// (the -track-3sr tracker emits only the tiny incremental `.3sr` bytes) — this
// sidesteps the tens-of-GB ENOSPC failure mode of the -dump-ram-path path
// (notes §4 "Disk-space gotcha").
const CONVERT_ENABLED = process.env.FCADE_CONVERT_ENABLED !== '0';
// 1..2 concurrent jobs; default 1 (one ggpo connection at a time).
const CONVERT_MAX_JOBS = Math.max(1, Math.min(2, Number(process.env.FCADE_CONVERT_MAX_JOBS) || 1));
const CONVERT_SCRATCH_DIR = process.env.FCADE_CONVERT_SCRATCH_DIR || path.join(__dirname, 'convert-scratch');
const CONVERT_DOWNLOADER = process.env.FCADE_CONVERT_DOWNLOADER || path.join(__dirname, 'fcade_replay_tool.py');
const CONVERT_PYTHON = process.env.FCADE_CONVERT_PYTHON || 'python3';
const CONVERT_RUNNER_BIN = process.env.FCADE_CONVERT_RUNNER_BIN || '/opt/fcade-runner/build/debug/fbneosdldarm64';
// The runner finds roms/ relative to its CWD (docs/fcade-replay-notes.md §2:
// sfiii3nr1 is a same-dir clone that also needs the sfiii3.zip parent set).
const CONVERT_RUNNER_DIR = process.env.FCADE_CONVERT_RUNNER_DIR || '/opt/fcade-runner';
const CONVERT_GGPO_HOST = process.env.FCADE_CONVERT_GGPO_HOST || 'ggpo.fightcade.com';
const CONVERT_GGPO_PORT = Number(process.env.FCADE_CONVERT_GGPO_PORT) || 7100;
// Hard ceilings so a stuck download/runner can never wedge a job forever.
const CONVERT_DOWNLOAD_TIMEOUT_MS = Number(process.env.FCADE_CONVERT_DOWNLOAD_TIMEOUT_MS) || 30 * 60 * 1000;
const CONVERT_RUNNER_TIMEOUT_MS = Number(process.env.FCADE_CONVERT_RUNNER_TIMEOUT_MS) || 30 * 60 * 1000;
// Runner tail-follow idle timeout: if the growing inputs file stops growing
// for this long, the runner treats the stream as finished (patch default).
const CONVERT_FOLLOW_IDLE_MS = Number(process.env.FCADE_CONVERT_FOLLOW_IDLE_MS) || 60 * 1000;
// Retain a failed job's scratch for this long (debugging); a periodic sweep
// removes older scratch dirs. Successful jobs clean up immediately.
const CONVERT_SCRATCH_RETAIN_MS = Number(process.env.FCADE_CONVERT_SCRATCH_RETAIN_MS) || 30 * 60 * 1000;

// --- idle teardown (Stage S6 hardening) ------------------------------------
// A watch-initiated convert job pulls the ggpo stream + runs the FBNeo tracker.
// If the viewer(s) who triggered it all walk away — no `convert`/`convertstatus`/
// `watchpoll` touch for this quark within the idle window — the job must TEAR
// DOWN (kill the ggpo pull + the runner, wipe scratch) rather than keep
// downloading+converting an unwatched replay indefinitely. `lastAccess` is
// bumped on every op that touches a live job; the periodic sweeper tears down a
// job idle past this window IFF it is not yet `ready` AND has not yet produced
// servable output (see jobHasServableOutput). A job that HAS finalized a game
// (nearly done, becomes a cached VOD) is left to finish into the store; a job
// any viewer is still polling (fresh lastAccess) is never torn down. Default
// 45 s sits in the task's 30-60 s window.
const CONVERT_IDLE_TEARDOWN_MS = Number(process.env.FCADE_CONVERT_IDLE_TEARDOWN_MS) || 45 * 1000;
// Slot preemption (switch-replay fix, 2026-07-24): when a NEW convert/watch
// request is waiting for a slot and a slot-holding job's viewer is GONE (its
// lastAccess is stale beyond this threshold — the device stopped polling it,
// e.g. because picking another replay relaunches the game process), PREEMPT
// the abandoned job (kill pull+runner, wipe scratch, free the slot) even if
// it has already produced servable output. Rationale: a viewer-less job must
// never block a viewer-ful request; the "finish into the cached store"
// leniency (jobHasServableOutput in the idle sweep) only applies while NO
// other viewer needs the slot. An active viewer polls `watchpoll` every
// ~400 ms (src/replay/proxy_client.c WATCH_POLL_IDLE_DELAY_MS) and retries
// failures at 1 s spacing with a 4-failure terminal budget, so 8 s of silence
// means the viewer is truly gone, not merely retrying.
const CONVERT_PREEMPT_STALE_MS = Number(process.env.FCADE_CONVERT_PREEMPT_STALE_MS) || 8 * 1000;
// How often the maintenance sweeper runs (idle teardown + scratch reclamation +
// store eviction). Shorter than the old fixed 60 s so idle teardown lands within
// the idle window rather than up to a minute late.
const CONVERT_SWEEP_INTERVAL_MS = Number(process.env.FCADE_CONVERT_SWEEP_INTERVAL_MS) || 15 * 1000;

// --- store eviction (Stage S6 hardening) -----------------------------------
// The `3sr/<quarkid>/` store grows unbounded — every convert/watch of a new
// quark adds a directory. Cap it: when the store exceeds EITHER the quark-count
// cap OR the total-bytes cap, evict least-recently-SERVED quark directories
// until back under both caps. "Served" time = the max of the quark dir's file
// mtimes and an in-memory serve overlay bumped on every get3sr/watch-from-store
// (so an actively-watched pool stays warm and survives across a restart via the
// on-disk mtime). NEVER evicts a quark with a live (non-finished) convert job,
// and only ever touches quark-named SUBDIRECTORIES of the store — catalog.json,
// the cookie, and any other non-quark file are structurally out of scope.
const STORE_EVICTION_ENABLED = process.env.FCADE_STORE_EVICTION_ENABLED !== '0';
const STORE_MAX_QUARKS = Math.max(1, Number(process.env.FCADE_STORE_MAX_QUARKS) || 200);
const STORE_MAX_BYTES = Math.max(1, Number(process.env.FCADE_STORE_MAX_BYTES) || 200 * 1024 * 1024);
// --- store eviction trigger fraction (review amendment, plan-bounded-pool-
// replay.md §6.2 Stage 3) ---------------------------------------------------
// SAME fraction the pre-convert churn guard (`preconvertStoreAtChurnCap`,
// :2487) uses to stop enqueuing. `evictStoreIfNeeded` below now triggers
// at/above this same threshold -- NOT just strictly over the raw 100 % cap --
// so the guard can never latch (new enqueues stopped) while eviction sits
// dormant in the dead zone between the guard's trip point and the raw cap.
// Single source of truth: both call sites read this one constant so the two
// thresholds can never drift apart.
const STORE_EVICT_TRIGGER_FRACTION = 0.9;
// --- store eviction low-water (plan-bounded-pool-replay.md §6.2) -----------
// Once eviction triggers (at/above STORE_EVICT_TRIGGER_FRACTION of either
// cap -- review amendment, see above), stop exactly at the cap and the store
// sits pinned AT the enqueuer's churn guard forever -- the guard latches, new
// pre-convert enqueues stop, and the pipeline stalls (§3.5). Instead evict
// down to a LOW-WATER fraction of each cap, comfortably under the trigger.
// Env-tunable, clamped to a sane [0.5, 1.0] range (below 0.5 would evict away
// most of a warm store on every trigger; above 1.0 reduces to the old
// exactly-at-cap behavior or worse). An empty-string or non-numeric env value
// falls back to the 0.85 default (review fix: `Number('')` is 0, which used
// to silently pass Number.isFinite and clamp to the 0.5 floor instead of
// defaulting). If the effective value is >= STORE_EVICT_TRIGGER_FRACTION, the
// hysteresis band collapses -- the guard could re-latch the instant an
// eviction pass finishes -- so warn at startup.
const STORE_EVICT_LOW_WATER = (() => {
    const rawEnv = process.env.FCADE_STORE_EVICT_LOW_WATER;
    const hasEnv = typeof rawEnv === 'string' && rawEnv.trim().length > 0;
    const raw = hasEnv ? Number(rawEnv) : NaN;
    const v = hasEnv && Number.isFinite(raw) ? raw : 0.85;
    const clamped = Math.min(1.0, Math.max(0.5, v));
    if (clamped >= STORE_EVICT_TRIGGER_FRACTION) {
        logWarn(
            `FCADE_STORE_EVICT_LOW_WATER=${clamped} is >= the churn-guard trigger fraction ` +
                `(${STORE_EVICT_TRIGGER_FRACTION}) -- this defeats the hysteresis band that keeps the churn ` +
                `guard from re-latching right after an eviction pass; expected < ${STORE_EVICT_TRIGGER_FRACTION}`,
        );
    }
    return clamped;
})();
// --- store eviction cadence (plan-preconvert S3) ---------------------------
// scanStoreQuarks stats every file of every quark dir. At the raised S3 caps
// (10 k quarks) running that inside the 15 s maintenance sweep is ~60 k stats
// every 15 s for nothing. So eviction is DECOUPLED from that sweep: it runs
// on publish (tryFinalize) + on workdone integration (S4) + on this dedicated
// slow timer (default 10 min). The LRU policy itself is unchanged.
const STORE_EVICT_SWEEP_MS = Number(process.env.FCADE_STORE_EVICT_SWEEP_MS) || 10 * 60 * 1000;

// --- pre-convert scheduler (plan-preconvert-fleet.md S2) -------------------
// A background pre-converter that fills the VPS's idle convert capacity so a
// device opening the REMOTE tab finds most rows already READY. It owns a
// SEPARATE persistent priority queue + failure ledger (preconvert-state.json);
// a periodic tick starts exactly ONE background job — and only when the single
// convert slot is idle, the pacing gap has elapsed, and free disk is above the
// floor. Background jobs share the one slot with live work via S1's instant
// preemption, so a live viewer is never starved. DEFAULT ON in code, but the
// feature is shipped OFF (FCADE_PRECONVERT_ENABLED=0 in the systemd unit) until
// the burn-in stage flips it on — safe to deploy dark.
const PRECONVERT_ENABLED = process.env.FCADE_PRECONVERT_ENABLED !== '0';
// Only pre-convert this gameid's rows (the device browses sfiii3nr1).
const PRECONVERT_GAMEID = process.env.FCADE_PRECONVERT_GAMEID || 'sfiii3nr1';
// Only ADD catalog_best rows to the queue. The catalog is now the weekly-best
// set and nothing else (browser-catalog.js / stealth-catalog crawl `best:true`
// with since = today's UTC midnight - 7d, and no Recent pass), so every row is
// already tier 1 and tier 2 is empty by construction. This guard makes that
// STRUCTURAL rather than incidental: a future catalog that starts carrying
// untagged rows again cannot quietly reintroduce speculative fresh-tier
// background conversions -- work the device would never play, spent against
// ggpo.fightcade.com. Set FCADE_PRECONVERT_BEST_ONLY=0 to enqueue the whole
// catalog again (the pre-weekly behavior).
//
// Deliberately NOT inside preconvertClassifyTier(): that function also runs on
// every ALREADY-QUEUED item each enqueue pass (refresh/demote), where tier 2
// must stay a meaningful value -- a queued item must be able to hold P2 and be
// demoted to P3, not be silently reclassified or dropped mid-lease.
const PRECONVERT_BEST_ONLY = process.env.FCADE_PRECONVERT_BEST_ONLY !== '0';
const PRECONVERT_TICK_MS = Number(process.env.FCADE_PRECONVERT_TICK_MS) || 30 * 1000;
// Politeness spacing between background pull STARTS, measured from the END of
// the previous background job, plus a uniform 0..JITTER jitter (§Q5). 3 min +
// up to 60 s ⇒ a patient-human-binge profile toward ggpo.fightcade.com.
const PRECONVERT_GAP_MS = Number(process.env.FCADE_PRECONVERT_GAP_MS) || 180 * 1000;
const PRECONVERT_JITTER_MS = (() => {
    const v = Number(process.env.FCADE_PRECONVERT_JITTER_MS); // allow an explicit 0
    return Number.isFinite(v) && v >= 0 ? v : 60 * 1000;
})();
// Persisted state: atomic tmp+rename write, debounced ≥ this interval.
const PRECONVERT_SAVE_DEBOUNCE_MS = Number(process.env.FCADE_PRECONVERT_SAVE_DEBOUNCE_MS) || 5 * 1000;
const PRECONVERT_STATE_FILE = process.env.FCADE_PRECONVERT_STATE_FILE || path.join(__dirname, 'preconvert-state.json');
// Bound the failure ledger so a permanent-failure entry per expired quark can't
// grow without limit; oldest (by last_at) are dropped past this many.
const PRECONVERT_LEDGER_MAX = Math.max(1, Number(process.env.FCADE_PRECONVERT_LEDGER_MAX) || 20000);
// Failure retry policy (§Q4). no_savestate = the dominant expected failure
// (old quark expired server-side): 1 retry after 24 h, then permanent.
const PRECONVERT_NO_SAVESTATE_RETRY_MS = Number(process.env.FCADE_PRECONVERT_NO_SAVESTATE_RETRY_MS) || 24 * 60 * 60 * 1000;
// no_games / runner / publish errors: backoff 1 h → 4 h → 24 h, then permanent.
const PRECONVERT_BACKOFF_MS = (() => {
    const raw = process.env.FCADE_PRECONVERT_BACKOFF_MS;
    if (raw) {
        const parts = raw.split(',').map((s) => Number(s)).filter((n) => Number.isFinite(n) && n >= 0);
        if (parts.length > 0) return parts;
    }
    return [60 * 60 * 1000, 4 * 60 * 60 * 1000, 24 * 60 * 60 * 1000];
})();
// --- P3 queue hygiene (plan-bounded-pool-replay.md §6.1 Stage 4) -----------
// A demoted (tier 3) quark's only residual value is "best-lists resurface old
// quarks" -- and a resurfacing quark re-enters via the enqueuer's new-row path
// as long as it isn't ledger-blocked, so keeping it in the queue forever buys
// nothing under the ready-only-catalog flip while the firehose (§3.2) keeps
// demoting ~900/day. Age it out instead. `row.date`/`it.date` is the match's
// own ms-epoch timestamp (same units as the quarkid's numeric prefix, e.g.
// quarkid `1784908283814-3519` / README examples -- 13-digit ms-since-epoch,
// NOT unix seconds), so this compares directly against `Date.now()`. Explicit
// 0 is a legitimate value (age out on the very next catalog pass, i.e.
// equivalent to D5's "drop-on-demotion" alternative), so this uses the same
// hasEnv hygiene as STORE_EVICT_LOW_WATER: an empty-string env value falls
// back to the default rather than `Number('') === 0` silently becoming an
// aggressive age-out-immediately policy nobody asked for.
const PRECONVERT_P3_MAX_AGE_MS = (() => {
    const rawEnv = process.env.FCADE_PRECONVERT_P3_MAX_AGE_MS;
    const hasEnv = typeof rawEnv === 'string' && rawEnv.trim().length > 0;
    const raw = hasEnv ? Number(rawEnv) : NaN;
    return hasEnv && Number.isFinite(raw) && raw >= 0 ? raw : 48 * 60 * 60 * 1000;
})();
// Hard backstop on total queue length, independent of age (a demotion burst
// ahead of the age-out threshold). Same `Number(x) || default` idiom as
// PRECONVERT_LEDGER_MAX above -- a 0/'' env value is not a meaningful queue
// cap (P1/P2 alone can legitimately exceed it, §6.1), so it just falls back
// to the default rather than needing hasEnv treatment.
const PRECONVERT_QUEUE_MAX = Math.max(1, Number(process.env.FCADE_PRECONVERT_QUEUE_MAX) || 2500);

// --- Mac-worker work-lease ops (plan-preconvert S4) ------------------------
// Three ADDITIVE ops (worklease/workdone/workstats) let the user's Mac claim
// pre-convert queue items, convert them locally, and push finished .3sr+meta
// back. Port 3479 is open to the internet (§1.4), so every one of these
// mutating ops is gated by a shared secret FCADE_WORK_TOKEN compared in
// constant time. A missing/wrong token — OR no token configured on the server
// — is ALWAYS rejected; there is no bypass. The device never sends these ops.
const WORK_LEASE_TTL_MS = Number(process.env.FCADE_WORK_LEASE_TTL_MS) || 45 * 60 * 1000; // > the 30-min download/runner ceilings
const WORK_LEASE_MAX_COUNT = 3; // never hand out more than this per lease call
const WORKER_NAME_RE = /^[A-Za-z0-9_.-]{1,64}$/;

// Constant-time token check. Reads FCADE_WORK_TOKEN live so it can be rotated
// without a restart. Both sides are hashed to a fixed 32 bytes first so
// timingSafeEqual never throws on a length mismatch (and length is not leaked).
function workTokenOk(provided) {
    const expected = process.env.FCADE_WORK_TOKEN;
    if (typeof expected !== 'string' || expected.length === 0) return false; // no token configured ⇒ ops DISABLED, never a bypass
    if (typeof provided !== 'string' || provided.length === 0) return false;
    const a = crypto.createHash('sha256').update(provided, 'utf8').digest();
    const b = crypto.createHash('sha256').update(expected, 'utf8').digest();
    return crypto.timingSafeEqual(a, b);
}

// Staging root for pushed .3sr before validation. A push lands here (worker's
// SSH user, S5); `workdone` validates it and atomically renames into 3sr/.
// Nothing here is ever served — get3sr/convertstatus only look at 3sr/.
function resolveIncomingDir() {
    const configured = process.env.FCADE_3SR_INCOMING_DIR;
    if (configured && configured.trim().length > 0) return configured;
    return path.join(path.dirname(resolve3srDir()), '3sr-incoming');
}

// --- free-disk floor (plan-preconvert S2/S3/S4) ----------------------------
// Below this many free bytes on the store filesystem, the background scheduler
// PAUSES starting new work and (S4) `workdone` REFUSES to integrate a push.
// Live convert-on-select is NEVER gated by it (a viewer's ~2.5 MB scratch is
// noise and live behavior must not regress). Measured with fs.statfsSync
// (Node ≥ 18.15; VPS is v20). Default 10 GiB.
const DISK_FLOOR_BYTES = Math.max(0, Number(process.env.FCADE_DISK_FLOOR_BYTES) || 10 * 1024 * 1024 * 1024);

// --- watch (Stage S3 / docs/plan-fcade-live-stream.md) ---------------------
// The `watchpoll` op streams a convert job's incrementally-produced
// `game_N.3sr` bytes to the device AS THEY ARE PRODUCED, so watching starts
// within ~2s + prefix-time rather than after the whole conversion finishes.
//
// PROTOCOL SHAPE = (b) offset-based chunked polling, NOT (a) a long-lived
// framed stream. Justification (plan S3 "Failure/fallback" + task brief):
//   1. The device client is STRICTLY lock-step one-request-one-response
//      (src/replay/proxy_client.h:8-10; README "Wire protocol"): it sends one
//      request frame and waits for exactly one response frame. A long-lived
//      framed stream would need a new "many frames per request" reader on the
//      lock-step client; polling reuses the existing request/response client
//      verbatim (the same code path get3sr already uses).
//   2. S4's device player is "play a growing `.3sr` file". Offset-based polling
//      maps one-to-one onto that: each `watchpoll {from}` returns the appended
//      bytes from `from`, which the fetch worker writes into a local growing
//      file at that offset — identical in spirit to ProxyClient_Fetch3sr's
//      write-as-you-go loop (proxy_client.h:176-186).
//   3. The tracker patches the header's frame_count/checksum_count IN PLACE at
//      finalize (runner-track-3sr.patch: Track3srFinalizeGame fseek(20)/fseek(26))
//      and appends the checksum table only at finalize. Pure forward
//      concatenation of a growing file therefore CANNOT be byte-identical to
//      the final file (the final frame_count isn't known until the game ends).
//      Polling solves this cleanly: the 28-byte header (the only mutated region)
//      is re-sent on every poll in a separate `header_b64` field and overwritten
//      by the client at offset 0, while the body (offset >= 28: words then
//      checksums) is strictly append-only and streamed by offset. The
//      reconstructed file is byte-identical to the tracker's completed
//      `game_N.3sr` (verified by cmp/sha256 — see __test_watchpoll.js).
// The one cost vs (a) is one poll-interval of extra latency; at the wire's
// ~6x-real-time production rate the buffer only ever grows, so this is noise.
const WATCH_HEADER_BYTES = 28; // v1 .3sr header (docs/3sr-format.md §1); the only in-place-mutated region
// Raw bytes per body chunk. 48 KiB raw -> exactly 64 KiB base64 (well under the
// device's 256 KiB PROXY_MAX_FRAME_LEN cap, proxy_client.c:46), leaving ample
// room for the JSON envelope + the tiny header_b64 field.
const WATCH_CHUNK_BYTES = Number(process.env.FCADE_WATCH_CHUNK_BYTES) || 48 * 1024;
// Incremental-checksum side channel (live-RNG-resync fix, 2026-07-25): the
// patched tracker ALSO mirrors every checksum entry, as it is computed, into
// an append-only side file `game_N.cks` next to the growing `game_N.3sr`
// (runner-track-3sr.patch Track3srOnFrame). `watchpoll` relays those bytes
// under a SECOND monotonic cursor (`cks_from` -> `checksums_b64`/`cks_next`)
// so the device can populate its in-memory checksum table DURING playback and
// apply the same Random_ix16 resyncs the finished-file path applies — instead
// of hard-desyncing at the first checkpoint because the .3sr's own table only
// exists after finalize. The `.3sr` byte stream (and the finalized file) is
// UNCHANGED — this is purely an additional response field. 8 KiB raw per poll
// (~10.9 KiB base64) is >1000 entries — far more than any real game produces
// (one entry per 60 frames) — and keeps the frame well under the device cap.
const WATCH_CKS_CHUNK_BYTES = Number(process.env.FCADE_WATCH_CKS_CHUNK_BYTES) || 8 * 1024;

const USER_AGENT =
    process.env.FCADE_PROXY_USER_AGENT ||
    'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 ' +
    '(KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36';

// --- Logging -----------------------------------------------------------------

function ts() {
    return new Date().toISOString();
}

function logInfo(msg) {
    console.log(`[${ts()}] INFO ${msg}`);
}

function logWarn(msg) {
    console.warn(`[${ts()}] WARN ${msg}`);
}

// --- Framing -------------------------------------------------------------

function encodeFrame(obj) {
    const json = Buffer.from(JSON.stringify(obj), 'utf8');
    const out = Buffer.alloc(4 + json.length);
    out.writeUInt32BE(json.length, 0);
    json.copy(out, 4);
    return out;
}

// --- Cookie loading --------------------------------------------------------
// Re-read on every use so an admin can refresh the cookie file without
// restarting the service (README documents this as the manual refresh flow).

function loadCookie() {
    const envVal = process.env[COOKIE_ENV];
    if (envVal && envVal.trim().length > 0) {
        return envVal.trim();
    }
    const cookieFile = process.env[COOKIE_FILE_ENV] || DEFAULT_COOKIE_FILE;
    try {
        const contents = fs.readFileSync(cookieFile, 'utf8').trim();
        if (contents.length > 0) {
            return contents;
        }
    } catch (_) {
        // File missing or unreadable: no cookie.
    }
    return null;
}

// --- Row normalization -------------------------------------------------------
// STRICT subset of the upstream searchquarks row. Never echo unexpected
// fields (plan requirement + hardening: upstream response shape is not our
// contract, our client's is).

function normalizePlayer(p) {
    if (!p || typeof p !== 'object') return null;
    return {
        name: typeof p.name === 'string' ? p.name : '',
        country: typeof p.country === 'string' ? p.country : null,
        rank: typeof p.rank === 'string' || typeof p.rank === 'number' ? p.rank : null,
        score: typeof p.score === 'number' ? p.score : null,
    };
}

function normalizeRow(row, fallbackGameid) {
    if (!row || typeof row !== 'object') return null;
    if (typeof row.quarkid !== 'string' && typeof row.quarkid !== 'number') return null;
    const players = Array.isArray(row.players) ? row.players.map(normalizePlayer).filter((p) => p !== null) : [];
    return {
        quarkid: String(row.quarkid),
        date: typeof row.date === 'number' ? row.date : null,
        duration: typeof row.duration === 'number' ? row.duration : null,
        players,
        ranked: typeof row.ranked === 'boolean' ? row.ranked : null,
        num_matches: typeof row.num_matches === 'number' ? row.num_matches : null,
        emulator: typeof row.emulator === 'string' ? row.emulator : null,
        gameid: typeof row.gameid === 'string' ? row.gameid : fallbackGameid,
    };
}

// --- Catalog loading (offline fallback / option B) ---------------------------
// Re-checked (via mtime, like loadCookie() re-reads its file) on every
// `search`/`status` call so a push-catalog.sh refresh takes effect with no
// restart. Parsing is cached by mtime so a large catalog file isn't
// re-parsed on every single request -- only when it actually changes.
//
// Returns `null` (meaning "no catalog, use live path") when FCADE_CATALOG_FILE
// is unset, or the file is missing, unreadable, or fails to parse into the
// expected `{rows: [...]}` shape. This is a hard requirement: catalog mode
// must never regress into a hung/error state just because the file was
// mid-write or briefly absent -- the existing live/cookie path already
// handles "can't get an answer" gracefully, so falling back to it here is
// strictly safer than inventing a second error path.

const catalogState = { path: null, mtimeMs: -1, parsed: null };

function loadCatalog() {
    const catalogFile = process.env[CATALOG_FILE_ENV];
    if (!catalogFile || catalogFile.trim().length === 0) {
        return null;
    }

    let stat;
    try {
        stat = fs.statSync(catalogFile);
    } catch (_) {
        // Missing/unreadable: not an error, just "no catalog right now".
        catalogState.path = null;
        catalogState.mtimeMs = -1;
        catalogState.parsed = null;
        return null;
    }

    if (catalogState.path === catalogFile && catalogState.mtimeMs === stat.mtimeMs && catalogState.parsed !== null) {
        return catalogState.parsed;
    }

    let parsed;
    try {
        const raw = fs.readFileSync(catalogFile, 'utf8');
        const json = JSON.parse(raw);
        if (!json || typeof json !== 'object' || Array.isArray(json) || !Array.isArray(json.rows)) {
            throw new Error('catalog JSON must be an object with a "rows" array');
        }
        parsed = {
            generated_at: typeof json.generated_at === 'number' ? json.generated_at : null,
            rows: json.rows,
        };
    } catch (err) {
        logWarn(`catalog file ${catalogFile} present but invalid, falling back to live mode: ${err && err.message ? err.message : err}`);
        catalogState.path = null;
        catalogState.mtimeMs = -1;
        catalogState.parsed = null;
        return null;
    }

    catalogState.path = catalogFile;
    catalogState.mtimeMs = stat.mtimeMs;
    catalogState.parsed = parsed;
    return parsed;
}

// Filters/sorts/pages a loaded catalog for one canonicalized search request.
// Every row is piped through normalizeRow() before being returned, same as
// the live path -- this both strips catalog-only bookkeeping fields (e.g.
// `catalog_best`) AND guarantees the response row shape can never drift
// from the live path's contract even if the catalog file itself is stale
// or hand-edited.
//
// `best` semantics: if any row in the filtered set is tagged
// `catalog_best: true`, `best:true` requests are restricted to just those
// rows (mirrors upstream's "best replays" filter being a hard filter, not
// a sort order). If none are tagged (e.g. a catalog built without a
// best-replays pass), `best:true` falls back to the full filtered set --
// still sorted by recency -- rather than returning an empty page.
function searchCatalog(catalog, canon) {
    const wantUsername = canon.username ? canon.username.toLowerCase() : null;

    let rows = catalog.rows.filter((r) => r && typeof r === 'object' && r.gameid === canon.gameid);

    if (wantUsername) {
        rows = rows.filter(
            (r) =>
                Array.isArray(r.players) &&
                r.players.some((p) => p && typeof p.name === 'string' && p.name.toLowerCase().includes(wantUsername)),
        );
    }

    if (canon.since !== null) {
        rows = rows.filter((r) => typeof r.date === 'number' && r.date >= canon.since);
    }

    if (canon.best) {
        const bestRows = rows.filter((r) => r.catalog_best === true);
        rows = bestRows.length > 0 ? bestRows : rows;
    }

    // Stage 2 ready-only filter (docs/plan-bounded-pool-replay.md §5): a
    // username-less (tab-browse) search, with the gate on, is restricted to
    // quarkids that are store-servable RIGHT NOW -- the exact same store-truth
    // check convertstatus's ready-fallback uses (`convertStoreServableGames`,
    // :1913), so readiness is only ever derived from the store, never
    // invented here. This can only REMOVE rows from the page; a resulting
    // short/empty page is correct (the wrapper already handles a short raw
    // feed the same way, patch :349). Username searches are exempt -- they
    // stay best-effort so BY PLAYER never goes empty just because a row
    // hasn't converted yet.
    if (!wantUsername && searchReadyOnlyEnabled()) {
        rows = rows.filter((r) => {
            const quarkid = typeof r.quarkid === 'string' ? r.quarkid : String(r.quarkid);
            return QUARKID_RE.test(quarkid) && convertStoreServableGames(quarkid) !== null;
        });
    }

    // Recency order in both modes: it's the only ordering signal a
    // browser-derived catalog reliably carries (see browser-catalog.js).
    rows = rows.slice().sort((a, b) => (typeof b.date === 'number' ? b.date : 0) - (typeof a.date === 'number' ? a.date : 0));

    const page = rows.slice(canon.offset, canon.offset + canon.limit);
    const normalized = page.map((r) => normalizeRow(r, canon.gameid)).filter((r) => r !== null);
    return { rows: normalized, count: normalized.length };
}

// --- Request validation ------------------------------------------------------

function validateSearchRequest(req) {
    if (typeof req.gameid !== 'string' || req.gameid.length === 0 || req.gameid.length > 64) {
        return 'gameid must be a non-empty string (max 64 chars)';
    }
    if (req.offset !== undefined) {
        if (!Number.isInteger(req.offset) || req.offset < 0) {
            return 'offset must be a non-negative integer';
        }
    }
    if (req.limit !== undefined) {
        if (!Number.isInteger(req.limit) || req.limit < 1 || req.limit > MAX_LIMIT) {
            return `limit must be an integer between 1 and ${MAX_LIMIT}`;
        }
    }
    if (req.best !== undefined && typeof req.best !== 'boolean') {
        return 'best must be a boolean';
    }
    if (req.since !== undefined) {
        if (!Number.isInteger(req.since) || req.since < 0) {
            return 'since must be a non-negative integer (ms epoch)';
        }
    }
    if (req.username !== undefined) {
        if (typeof req.username !== 'string' || req.username.length > 64) {
            return 'username must be a string (max 64 chars)';
        }
    }
    return null;
}

function canonicalizeSearchRequest(req) {
    // Canonical, defaulted form used both as the cache key and as the
    // payload sent upstream.
    return {
        gameid: req.gameid,
        offset: req.offset !== undefined ? req.offset : 0,
        limit: req.limit !== undefined ? req.limit : DEFAULT_LIMIT,
        best: req.best === true,
        since: req.since !== undefined ? req.since : null,
        username: req.username !== undefined ? req.username : null,
    };
}

function cacheKeyFor(canon) {
    return JSON.stringify([canon.gameid, canon.offset, canon.limit, canon.best, canon.since, canon.username]);
}

function ttlForRequest(canon) {
    if (canon.offset === 0 && !canon.best) {
        return CACHE_TTL_FIRST_PAGE_MS;
    }
    return CACHE_TTL_OTHER_MS;
}

// --- Upstream call (real) ---------------------------------------------------
// Exact request shape ported from tools/fcade-replays/fcade_replay_tool.py
// `search_quarks` (:117-168): POST JSON body {req:"searchquarks", ...},
// Origin/Referer/User-Agent headers, Cookie header carrying cf_clearance.

async function upstreamSearchReal(canon, cookie) {
    const payload = {
        req: 'searchquarks',
        offset: canon.offset,
        limit: canon.limit,
        gameid: canon.gameid,
    };
    if (canon.best) payload.best = true;
    if (canon.since !== null) payload.since = canon.since;
    if (canon.username !== null) payload.username = canon.username;

    const headers = {
        Accept: 'application/json, text/plain, */*',
        'Content-Type': 'application/json;charset=UTF-8',
        Origin: 'https://www.fightcade.com',
        Referer: `https://www.fightcade.com/game/${canon.gameid}`,
        'User-Agent': USER_AGENT,
        Cookie: cookie,
    };

    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), UPSTREAM_TIMEOUT_MS);
    let resp;
    try {
        resp = await fetch(UPSTREAM_API_URL, {
            method: 'POST',
            headers,
            body: JSON.stringify(payload),
            signal: controller.signal,
        });
    } catch (err) {
        throw { type: 'upstream_error', detail: `request failed: ${err && err.message ? err.message : err}` };
    } finally {
        clearTimeout(timer);
    }

    if (resp.status === 403) {
        throw {
            type: 'cloudflare_403',
            detail: 'Fightcade API returned 403 Forbidden — the cf_clearance cookie is likely expired or IP-bound to a different host',
        };
    }
    if (!resp.ok) {
        throw { type: 'upstream_error', detail: `Fightcade API returned HTTP ${resp.status}` };
    }
    let body;
    try {
        body = await resp.json();
    } catch (err) {
        throw { type: 'upstream_error', detail: `could not parse upstream JSON: ${err && err.message ? err.message : err}` };
    }
    return body;
}

// --- Upstream call (mock, for __test_protocol.js) ---------------------------
// Activated by FCADE_PROXY_MOCK=1. Deterministic, driven entirely by fields
// already present on the canonical request — no external network access, no
// client-supplied URL. Sentinel gameids select behavior so the self-test can
// exercise every typed-error path without a real cookie.

const mockState = { calls: 0 };

function canned3Rows(gameid) {
    return [
        {
            quarkid: '1700000000000-0001',
            date: 1700000000000,
            duration: 185,
            players: [
                { name: 'Alice', country: 'US', rank: 'S', score: 1200 },
                { name: 'Bob', country: 'JP', rank: 'A', score: 1100 },
            ],
            ranked: true,
            num_matches: 3,
            emulator: 'fbneo',
            gameid,
            // Deliberately-unexpected field: proves strict-subset normalization.
            internal_debug_flag: true,
        },
        {
            quarkid: '1700000000000-0002',
            date: 1700000000500,
            duration: 92,
            players: [{ name: 'Carol', country: null, score: 900 }],
            ranked: false,
            num_matches: 1,
            emulator: 'fbneo',
            gameid,
        },
    ];
}

async function upstreamSearchMock(canon) {
    mockState.calls += 1;
    if (canon.gameid === '__mock_403__') {
        throw {
            type: 'cloudflare_403',
            detail: 'mock: Fightcade API returned 403 Forbidden',
        };
    }
    if (canon.gameid === '__mock_network_error__') {
        throw { type: 'upstream_error', detail: 'mock: simulated network failure' };
    }
    if (canon.gameid === '__mock_bad_shape__') {
        return { unexpected: true };
    }
    return { results: { results: canned3Rows(canon.gameid) } };
}

function upstreamSearch(canon, cookie) {
    if (process.env.FCADE_PROXY_MOCK === '1') {
        return upstreamSearchMock(canon, cookie);
    }
    return upstreamSearchReal(canon, cookie);
}

// --- Rate-limited upstream queue ---------------------------------------------
// Serializes upstream calls with a minimum spacing between them. Requests
// pile up in `queue` and are drained one at a time; a request that would
// have to wait past MAX_QUEUE_WAIT_MS gets a typed error instead of an
// indefinite hang.

function makeUpstreamQueue() {
    let lastCallAt = 0;
    let draining = false;
    const queue = [];

    function scheduleDrain() {
        if (draining) return;
        draining = true;
        const run = () => {
            if (queue.length === 0) {
                draining = false;
                return;
            }
            const now = Date.now();
            const waitMs = Math.max(0, lastCallAt + MIN_UPSTREAM_INTERVAL_MS - now);
            setTimeout(() => {
                const job = queue.shift();
                if (!job) {
                    run();
                    return;
                }
                lastCallAt = Date.now();
                job
                    .fn()
                    .then((v) => job.resolve(v))
                    .catch((e) => job.reject(e))
                    .finally(run);
            }, waitMs);
        };
        run();
    }

    return function enqueue(fn) {
        return new Promise((resolve, reject) => {
            const enqueuedAt = Date.now();
            const wrappedFn = () => {
                if (Date.now() - enqueuedAt > MAX_QUEUE_WAIT_MS) {
                    return Promise.reject({
                        type: 'upstream_error',
                        detail: `queued longer than ${MAX_QUEUE_WAIT_MS}ms waiting for upstream rate-limit slot`,
                    });
                }
                return fn();
            };
            queue.push({ fn: wrappedFn, resolve, reject });
            scheduleDrain();
        });
    };
}

// --- Cache -------------------------------------------------------------------

function makeCache() {
    const entries = new Map(); // key -> { rows, count, expiresAt }
    let hits = 0;
    let misses = 0;

    function get(key) {
        const e = entries.get(key);
        if (!e) {
            misses += 1;
            return null;
        }
        if (Date.now() > e.expiresAt) {
            entries.delete(key);
            misses += 1;
            return null;
        }
        hits += 1;
        return e;
    }

    function set(key, value, ttlMs) {
        if (entries.size >= CACHE_MAX_ENTRIES && !entries.has(key)) {
            // Evict the oldest entry (Map preserves insertion order).
            const oldestKey = entries.keys().next().value;
            if (oldestKey !== undefined) entries.delete(oldestKey);
        }
        entries.set(key, { ...value, expiresAt: Date.now() + ttlMs });
    }

    return {
        get,
        set,
        stats() {
            return { entries: entries.size, hits, misses };
        },
        _clear() {
            entries.clear();
            hits = 0;
            misses = 0;
        },
    };
}

// --- Request handling ---------------------------------------------------------

async function handleSearch(req, cache, enqueueUpstream) {
    const validationError = validateSearchRequest(req);
    if (validationError) {
        return { ok: false, error: 'bad_request', detail: validationError };
    }

    const canon = canonicalizeSearchRequest(req);

    // Catalog mode short-circuits everything below: no cache, no cookie, no
    // upstream call, ever. Checked fresh on every request (mtime-cached
    // inside loadCatalog) so a push-catalog.sh refresh needs no restart.
    const catalog = loadCatalog();
    if (catalog !== null) {
        const result = searchCatalog(catalog, canon);
        return {
            ok: true,
            rows: result.rows,
            count: result.count,
            source: 'catalog',
            generated_at: catalog.generated_at,
        };
    }

    const key = cacheKeyFor(canon);

    const cached = cache.get(key);
    if (cached) {
        return { ok: true, rows: cached.rows, count: cached.count };
    }

    const cookie = loadCookie();
    if (!cookie) {
        return {
            ok: false,
            error: 'cookie_missing',
            detail: `no Fightcade cookie configured (set ${COOKIE_ENV} or write ${COOKIE_FILE_ENV || DEFAULT_COOKIE_FILE})`,
        };
    }

    let body;
    try {
        body = await enqueueUpstream(() => upstreamSearch(canon, cookie));
    } catch (err) {
        if (err && err.type) {
            return { ok: false, error: err.type, detail: err.detail };
        }
        return { ok: false, error: 'upstream_error', detail: String(err) };
    }

    const rawRows = body && body.results && Array.isArray(body.results.results) ? body.results.results : null;
    if (rawRows === null) {
        return { ok: false, error: 'upstream_error', detail: 'unexpected upstream response shape (missing results.results)' };
    }

    const rows = rawRows.map((r) => normalizeRow(r, canon.gameid)).filter((r) => r !== null);
    const result = { rows, count: rows.length };
    cache.set(key, result, ttlForRequest(canon));
    return { ok: true, rows: result.rows, count: result.count };
}

function handleStatus(cache, startedAt, convert) {
    const catalog = loadCatalog();
    return {
        ok: true,
        version: pkg.version,
        cookie_present: loadCookie() !== null,
        cache: cache.stats(),
        uptime_s: Math.floor((Date.now() - startedAt) / 1000),
        catalog_present: catalog !== null,
        catalog_generated_at: catalog !== null ? catalog.generated_at : null,
        catalog_rows: catalog !== null ? catalog.rows.length : 0,
        mode: catalog !== null ? 'catalog' : 'live',
        convert: convert ? convert.stats() : null,
        preconvert: convert && convert.preconvertStats ? convert.preconvertStats() : null,
    };
}

// --- get3sr (Step F1b / plan-osd-replay-browser.md Stage S1) ---------------
// Serves pre-converted `.3sr` + `.meta.json` blobs from
// `<3sr-dir>/<quarkid>/game_N.3sr` (+ sibling `game_N.meta.json`), published
// offline by `tools/fcade-replays/publish_3sr.py` + `push-3sr.sh`. See
// README.md "get3sr" for the wire contract.

// Deliberately strict: real quarkids look like `<ms-epoch>-<n>` (catalog
// samples: "1700000000000-0001"). Reject anything else outright rather than
// build a filesystem path from client input -- this alone rules out path
// traversal ('/', '\', '..') without needing a second, separate check.
const QUARKID_RE = /^[A-Za-z0-9_-]{1,128}$/;
const GAME_FILE_RE = /^game_(\d+)\.3sr$/;

function validateGet3srRequest(req) {
    if (typeof req.quarkid !== 'string' || !QUARKID_RE.test(req.quarkid)) {
        return 'quarkid must be a non-empty string matching [A-Za-z0-9_-]{1,128}';
    }
    if (req.game_index !== undefined) {
        if (!Number.isInteger(req.game_index) || req.game_index < 0) {
            return 'game_index must be a non-negative integer';
        }
    }
    return null;
}

function resolve3srDir() {
    const configured = process.env[THREESX_3SR_DIR_ENV];
    return configured && configured.trim().length > 0 ? configured : DEFAULT_3SR_DIR;
}

// --- store eviction (Stage S6) --------------------------------------------
// In-memory "last served" overlay: bumped whenever a quark's `.3sr` is served
// (get3sr) or streamed from the finalized store copy (watchpoll). Combined with
// the quark dir's on-disk mtime so serve-recency survives a restart. markServed
// also best-effort touches the dir's mtime so the on-disk signal tracks serves,
// not just publishes.
const storeLastServed = new Map(); // quarkid -> ms epoch

function markQuarkServed(quarkid) {
    if (typeof quarkid !== 'string' || !QUARKID_RE.test(quarkid)) return;
    const now = Date.now();
    storeLastServed.set(quarkid, now);
    // Best-effort: encode the serve time on disk too, so LRU order survives a
    // proxy restart. Failure (e.g. read-only fs in a test) is harmless — the
    // in-memory overlay still orders serves within this process.
    try {
        const dir = path.join(resolve3srDir(), quarkid);
        fs.utimesSync(dir, now / 1000, now / 1000);
    } catch (_) {}
}

// Enumerate quark subdirectories of the store with their byte size and
// effective last-served time. Only entries that are directories AND match the
// strict quarkid shape are considered — catalog.json, the cookie file, and any
// other non-quark file/dir is skipped, so eviction can never touch them.
function scanStoreQuarks(dir3sr) {
    let ents;
    try {
        ents = fs.readdirSync(dir3sr, { withFileTypes: true });
    } catch (_) {
        return null; // store dir absent/unreadable -> nothing to evict
    }
    const quarks = [];
    let totalBytes = 0;
    for (const ent of ents) {
        if (!ent.isDirectory()) continue; // never a file (catalog.json/cookie/...)
        if (!QUARKID_RE.test(ent.name)) continue; // never a non-quark dir
        const qdir = path.join(dir3sr, ent.name);
        let bytes = 0;
        let mtimeMs = 0;
        let files;
        try {
            files = fs.readdirSync(qdir);
        } catch (_) {
            continue;
        }
        try {
            const dst = fs.statSync(qdir);
            mtimeMs = dst.mtimeMs;
        } catch (_) {}
        for (const f of files) {
            try {
                const st = fs.statSync(path.join(qdir, f));
                if (st.isFile()) {
                    bytes += st.size;
                    if (st.mtimeMs > mtimeMs) mtimeMs = st.mtimeMs;
                }
            } catch (_) {}
        }
        const served = Math.max(mtimeMs, storeLastServed.get(ent.name) || 0);
        quarks.push({ quarkid: ent.name, dir: qdir, bytes, served });
        totalBytes += bytes;
    }
    return { quarks, totalBytes };
}

// Quarks reachable from the CURRENT catalog are "pinned" -- never evicted by
// the LRU loop, exactly like an active convert/watch job (plan-bounded-pool-
// replay.md §6.2). `loadCatalog()` returns one flat `rows` array; since the
// catalog was retargeted to the weekly-best set that array IS the set the
// device shuffles (there is no Recent feed in it any more). Without this, at
// cap the LRU (`served` ≈ conversion-time mtime for a never-watched quark, see
// scanStoreQuarks above) evicts the *earliest-converted* quarks first -- after
// the Stage 1 tier flip that is precisely the BEST layer.
//
// Built ONCE per eviction pass by the caller (not per-candidate) -- see the
// single `buildPinnedQuarkSet()` call in `evictStoreIfNeeded` below.
//
// `attractSet` is the set the device is playing right now. `evictStoreIfNeeded`
// passes `buildAttractQuarkSet()` (below), so the shuffle set is pinned in its
// OWN right and not merely as a side effect of the broad catalog pin -- if the
// catalog ever carries rows outside the set again, or the catalog pin is ever
// narrowed, the set the device is mid-shuffle through stays protected. A
// curated attract list file (plan §7 / Stage 6, deferred, user decision D6)
// merges into this same argument when it ships.
function buildPinnedQuarkSet(attractSet) {
    const pinned = new Set();
    const catalog = loadCatalog();
    if (catalog && Array.isArray(catalog.rows)) {
        for (const r of catalog.rows) {
            if (r && r.quarkid !== undefined && r.quarkid !== null) {
                pinned.add(String(r.quarkid));
            }
        }
    }
    if (attractSet) {
        for (const id of attractSet) pinned.add(String(id));
    }
    return pinned;
}

// The quarkids the device's shuffle viewer can currently be playing: exactly
// what a `search` with `best:true` resolves to (searchCatalog's hard
// catalog_best filter), which after the weekly retarget is the whole catalog.
// It is therefore a SUBSET of the catalog pin above today -- deliberately so;
// it exists to state the dependency, not to add coverage, so that narrowing
// the catalog pin later cannot silently unpin the set the device is playing.
function buildAttractQuarkSet() {
    const set = new Set();
    const catalog = loadCatalog();
    if (!catalog || !Array.isArray(catalog.rows)) return set;
    for (const r of catalog.rows) {
        if (r && r.catalog_best === true && r.quarkid !== undefined && r.quarkid !== null) {
            set.add(String(r.quarkid));
        }
    }
    return set;
}

// Evict least-recently-served quark dirs until the store is back down to a
// LOW-WATER target below BOTH caps (not just back under them -- see
// STORE_EVICT_LOW_WATER above). `isQuarkActive(quarkid)` (supplied by the
// convert manager) protects any quark with a live convert/watch job, and
// `buildPinnedQuarkSet()` protects any quark still reachable from the current
// catalog -- both are skipped and never counted as evictable, but still count
// toward the caps (so we don't thrash trying to get under a cap that only
// active/pinned quarks exceed). Returns the list of evicted quarkids.
function evictStoreIfNeeded(isQuarkActive) {
    if (!STORE_EVICTION_ENABLED) return [];
    const dir3sr = resolve3srDir();
    const scan = scanStoreQuarks(dir3sr);
    if (!scan) return [];
    let { quarks, totalBytes } = scan;
    let count = quarks.length;
    // Trigger at/above the SAME fraction the churn guard trips at (review fix:
    // was "count > STORE_MAX_QUARKS" i.e. strictly over the raw cap, which left
    // a dead zone between the guard's trip point and the cap where the guard
    // could latch -- new pre-convert enqueues stop -- while eviction stayed
    // dormant). See STORE_EVICT_TRIGGER_FRACTION above.
    if (count < STORE_EVICT_TRIGGER_FRACTION * STORE_MAX_QUARKS && totalBytes < STORE_EVICT_TRIGGER_FRACTION * STORE_MAX_BYTES) {
        return [];
    }

    // Fail-open pins (review fix): a catalog that's CONFIGURED but currently
    // unreadable/corrupt must never be treated as "no pins" -- an empty pinned
    // set here would let this pass evict a quark that IS in the (temporarily
    // unreadable) catalog. Same "is it configured" signal loadCatalog() itself
    // uses (CATALOG_FILE_ENV set to a non-empty value). "No catalog configured
    // at all" legitimately means an empty pinned set, and the pass proceeds as
    // before. The slow timer (STORE_EVICT_SWEEP_MS, default 10 min) retries.
    const catalogFileEnv = process.env[CATALOG_FILE_ENV];
    const catalogConfigured = typeof catalogFileEnv === 'string' && catalogFileEnv.trim().length > 0;
    if (catalogConfigured && loadCatalog() === null) {
        logWarn(
            `store eviction: ${CATALOG_FILE_ENV} is configured but the catalog is missing/unreadable/corrupt -- ` +
                `skipping this eviction pass rather than risk evicting a catalog-pinned quark under a fail-open ` +
                `empty pinned set (retried by the ${STORE_EVICT_SWEEP_MS}ms slow timer)`,
        );
        return [];
    }

    // Low-water targets: how far under cap to evict, once eviction triggers.
    const quarkTarget = Math.floor(STORE_EVICT_LOW_WATER * STORE_MAX_QUARKS);
    const byteTarget = Math.floor(STORE_EVICT_LOW_WATER * STORE_MAX_BYTES);

    // Built once for this whole pass -- O(catalog rows), not O(candidates).
    // (loadCatalog() is called again inside here; cheap -- mtime-cached, see
    // loadCatalog() above -- and already proven non-null by the check above.)
    const pinned = buildPinnedQuarkSet(buildAttractQuarkSet());

    // Oldest-served first.
    quarks.sort((a, b) => a.served - b.served);
    const evicted = [];
    for (const q of quarks) {
        if (count <= quarkTarget && totalBytes <= byteTarget) break;
        if (isQuarkActive && isQuarkActive(q.quarkid)) continue; // never evict a watched/converting quark
        if (pinned.has(q.quarkid)) continue; // never evict a catalog-pinned quark
        try {
            fs.rmSync(q.dir, { recursive: true, force: true });
        } catch (err) {
            logWarn(`store eviction: failed to remove ${q.quarkid}: ${err && err.message ? err.message : err}`);
            continue;
        }
        storeLastServed.delete(q.quarkid);
        totalBytes -= q.bytes;
        count -= 1;
        evicted.push(q.quarkid);
        logInfo(
            `store eviction: removed ${q.quarkid} (${q.bytes} B, last-served ${new Date(q.served).toISOString()}) ` +
                `— now ${count} quarks / ${totalBytes} B (low-water target ${quarkTarget} / ${byteTarget} B, caps ${STORE_MAX_QUARKS} / ${STORE_MAX_BYTES})`,
        );
    }
    // Tripwire against the actual CAP (not the low-water target): pinned +
    // active mass alone can legitimately sit between low-water and cap (not a
    // problem), but if it still exceeds the hard cap after a full pass, that's
    // the "can't get under cap" case worth a warn.
    if (count > STORE_MAX_QUARKS || totalBytes > STORE_MAX_BYTES) {
        logWarn(
            `store eviction: still over cap after evicting ${evicted.length} (${count} quarks / ${totalBytes} B) — ` +
                `remaining are active convert/watch jobs or catalog-pinned, left in place`,
        );
    }
    return evicted;
}

// Reads+validates one game_N.3sr + its sibling game_N.meta.json. Returns
// `{ok:true, data}` or `{ok:false, reason}` (reason is a short, log-only
// string -- never surfaced verbatim to the client for a whole-quark request,
// where an invalid game is silently dropped from the list instead).
//
// `data` carries everything BOTH response shapes need: the raw buffers/sizes
// (for the per-game blobs) and the non-empty player names (for the manifest
// rows). The caller picks which subset to emit -- see makeManifestEntry() /
// makeGameEntry(). This is deliberately a single validate-once read: whether a
// game is servable (files present, correct size, `3SR1` magic, parseable meta,
// at least one non-empty player name) is the same question in both modes.
function read3srGameData(quarkDir, gameIndex) {
    const path3sr = path.join(quarkDir, `game_${gameIndex}.3sr`);
    const pathMeta = path.join(quarkDir, `game_${gameIndex}.meta.json`);

    let stat3sr;
    try {
        stat3sr = fs.statSync(path3sr);
    } catch (_) {
        return { ok: false, reason: `game_${gameIndex}.3sr missing` };
    }
    if (!stat3sr.isFile() || stat3sr.size === 0 || stat3sr.size > MAX_3SR_FILE_BYTES) {
        return { ok: false, reason: `game_${gameIndex}.3sr has an invalid size (${stat3sr.size})` };
    }

    let statMeta;
    try {
        statMeta = fs.statSync(pathMeta);
    } catch (_) {
        // The meta sidecar is the ONLY source of player names on-device
        // (docs/3sr-format.md; src/replay/replay_player.c:346-352/:651-656)
        // -- a `.3sr` with no sidecar must never be served (P-1.1).
        return { ok: false, reason: `game_${gameIndex}.meta.json missing (no name sidecar)` };
    }
    if (!statMeta.isFile() || statMeta.size === 0 || statMeta.size > MAX_META_FILE_BYTES) {
        return { ok: false, reason: `game_${gameIndex}.meta.json has an invalid size (${statMeta.size})` };
    }

    let buf3sr;
    let bufMeta;
    try {
        buf3sr = fs.readFileSync(path3sr);
        bufMeta = fs.readFileSync(pathMeta);
    } catch (err) {
        return { ok: false, reason: `read failed: ${err && err.message ? err.message : err}` };
    }

    if (buf3sr.length < 4 || buf3sr.toString('ascii', 0, 4) !== '3SR1') {
        return { ok: false, reason: `game_${gameIndex}.3sr has a bad magic (not '3SR1')` };
    }

    let meta;
    try {
        meta = JSON.parse(bufMeta.toString('utf8'));
    } catch (err) {
        return { ok: false, reason: `game_${gameIndex}.meta.json is not valid JSON: ${err.message}` };
    }
    const players = Array.isArray(meta.players)
        ? meta.players.filter((p) => p && typeof p.name === 'string' && p.name.trim().length > 0).map((p) => p.name)
        : [];
    if (players.length === 0) {
        // Never serve a nameless replay (plan Stage S1 item 4 / review P-1.1).
        return { ok: false, reason: `game_${gameIndex}.meta.json has no non-empty players[].name` };
    }

    return {
        ok: true,
        data: {
            game_index: gameIndex,
            size: buf3sr.length,
            meta_size: bufMeta.length,
            players,
            buf3sr,
            bufMeta,
        },
    };
}

// MANIFEST row: metadata ONLY (no base64 blobs). Stays tiny regardless of the
// .3sr size, so a whole-quark listing never approaches the device frame cap.
function makeManifestEntry(data) {
    return {
        game_index: data.game_index,
        size: data.size,
        meta_size: data.meta_size,
        players: data.players,
    };
}

// PER-GAME entry: the actual base64 blobs for exactly one game.
function makeGameEntry(data) {
    return {
        game_index: data.game_index,
        size: data.size,
        b64: data.buf3sr.toString('base64'),
        meta_size: data.meta_size,
        meta_b64: data.bufMeta.toString('base64'),
    };
}

function listQuarkGameIndices(quarkDir) {
    let names;
    try {
        names = fs.readdirSync(quarkDir);
    } catch (_) {
        return null; // quark directory does not exist / unreadable
    }
    const indices = [];
    for (const name of names) {
        const m = GAME_FILE_RE.exec(name);
        if (m) indices.push(Number(m[1]));
    }
    indices.sort((a, b) => a - b);
    return indices.slice(0, MAX_3SR_GAMES_PER_QUARK);
}

function handleGet3sr(req) {
    const validationError = validateGet3srRequest(req);
    if (validationError) {
        return { ok: false, error: 'bad_request', detail: validationError };
    }

    const dir3sr = resolve3srDir();
    const quarkDir = path.join(dir3sr, req.quarkid);
    const indices = listQuarkGameIndices(quarkDir);

    if (indices === null || indices.length === 0) {
        return {
            ok: false,
            error: 'not_found',
            detail: `no converted .3sr available for quarkid ${JSON.stringify(req.quarkid)}`,
        };
    }

    // PER-GAME mode (game_index >= 0): return exactly ONE game's base64 blobs,
    // hard-guarded against the device frame cap.
    if (req.game_index !== undefined) {
        if (!indices.includes(req.game_index)) {
            return { ok: false, error: 'not_found', detail: `quark has no game_${req.game_index}` };
        }
        const result = read3srGameData(quarkDir, req.game_index);
        if (!result.ok) {
            logWarn(`get3sr ${req.quarkid}/game_${req.game_index}: ${result.reason}`);
            return { ok: false, error: 'not_found', detail: result.reason };
        }
        const resp = { ok: true, quarkid: req.quarkid, games: [makeGameEntry(result.data)] };
        // A single game must fit one frame. A real .3sr is ~30-46 KB (well
        // clear), but MAX_3SR_FILE_BYTES permits up to 1 MiB on disk, whose
        // base64 (~1.33 MiB) would blow past the device cap -- refuse with a
        // typed error rather than emit a frame the device silently drops.
        const frameBytes = Buffer.byteLength(JSON.stringify(resp), 'utf8');
        const ceiling = DEVICE_MAX_FRAME_BYTES - SINGLE_GAME_FRAME_MARGIN;
        if (frameBytes > ceiling) {
            logWarn(
                `get3sr ${req.quarkid}/game_${req.game_index}: single-game frame ${frameBytes} B exceeds device ceiling ${ceiling} B`,
            );
            return {
                ok: false,
                error: 'not_found',
                detail: `game_${req.game_index} is too large to deliver in one frame (${frameBytes} > ${ceiling} bytes)`,
            };
        }
        markQuarkServed(req.quarkid); // keeps this quark warm against store eviction
        return resp;
    }

    // MANIFEST mode (game_index omitted): metadata ONLY -- no base64 blobs, so
    // the frame is tiny no matter how many games the quark carries. Silently
    // drop (log only) any game whose files are missing/oversize/nameless -- one
    // bad game in a multi-game session must not take down the others; a
    // nameless game is dropped exactly as before (P-1.1 still holds -- the
    // manifest only lists games that ARE servable per-game).
    const games = [];
    for (const idx of indices) {
        const result = read3srGameData(quarkDir, idx);
        if (result.ok) {
            games.push(makeManifestEntry(result.data));
        } else {
            logWarn(`get3sr ${req.quarkid}/game_${idx}: ${result.reason} (dropped from manifest)`);
        }
    }

    if (games.length === 0) {
        return {
            ok: false,
            error: 'not_found',
            detail: `quark ${JSON.stringify(req.quarkid)} has game_N.3sr file(s) but none pass validation`,
        };
    }

    markQuarkServed(req.quarkid); // keeps this quark warm against store eviction
    return { ok: true, quarkid: req.quarkid, games };
}

// --- convert-on-select job manager (Stage S2) ------------------------------
// A per-quark job state machine: queued -> pulling -> converting -> ready|failed.
//
// Each job: (a) spawns the repo's canonical ggpo downloader (fcade_replay_tool.py
// download) to pull savestate+inputs; (b) as soon as the savestate + first input
// records land, spawns the S1 `-track-3sr` runner in `-replay-follow` mode so
// conversion OVERLAPS the download (the tracker tails the growing inputs file) —
// this keeps total wall time at ≈ duration/6 rather than download+convert
// serialized; (c) on completion, writes a `game_N.meta.json` per game from the
// catalog row (mirroring make_3sr.py build_meta()'s quark.json branch — the
// sidecar is the ONLY source of player names on-device) and atomically moves
// each `game_N.{meta.json,3sr}` into the existing `3sr/<quarkid>/` store, so the
// unchanged get3sr op serves it. NO `.ram` dumps are ever written.

function convertQuarkStoreDir(quarkid) {
    return path.join(resolve3srDir(), quarkid);
}

// A quark is "already converted" iff its store dir has >=1 game that passes the
// exact same validation get3sr applies (present, sized, 3SR1 magic, named meta).
function convertStoreServableGames(quarkid) {
    const quarkDir = convertQuarkStoreDir(quarkid);
    const indices = listQuarkGameIndices(quarkDir);
    if (indices === null || indices.length === 0) return null;
    const servable = [];
    for (const idx of indices) {
        if (read3srGameData(quarkDir, idx).ok) servable.push(idx);
    }
    return servable.length > 0 ? servable : null;
}

function makeConvertManager() {
    const jobs = new Map(); // quarkid -> job
    const queue = []; // quarkids awaiting a concurrency slot (FIFO)
    let running = 0;

    // Scratch holds nothing durable (savestate + inputs + tiny game_N.3sr, all
    // <~2.5 MB/quark — NO .ram frames), so a clean slate on boot is safe and
    // avoids leaking a crashed run's residue.
    try {
        fs.rmSync(CONVERT_SCRATCH_DIR, { recursive: true, force: true });
    } catch (_) {}
    try {
        fs.mkdirSync(CONVERT_SCRATCH_DIR, { recursive: true });
    } catch (err) {
        logWarn(`convert: could not create scratch dir ${CONVERT_SCRATCH_DIR}: ${err && err.message ? err.message : err}`);
    }

    // Bump a live job's idle-teardown clock. Called on every op that touches a
    // quark's job (convert / convertstatus / watchpoll) so that as long as ANY
    // viewer keeps polling, the job is never considered idle.
    function touchJob(quarkid) {
        const job = jobs.get(quarkid);
        if (job && !job.finished) job.lastAccess = Date.now();
    }

    // Is a quark protected from store eviction? True while it has a live
    // (non-finished) convert/watch job — never evict what is being produced or
    // actively watched.
    function isQuarkActive(quarkid) {
        const job = jobs.get(quarkid);
        return !!(job && !job.finished);
    }

    // A finished 'ready' job whose backing store bytes are gone — LRU-evicted
    // (the eviction guard only protects NON-finished jobs, so a finalized VOD's
    // bytes are evictable) or deleted out-of-band — is a ghost. Left in `jobs`
    // it blocks re-conversion: requestConvert/requestWatch see the object and
    // never re-enter the pull path, so watchpoll/convertstatus answer
    // state:'ready' with zero bytes until the proxy restarts. Detect and drop
    // it so the next request re-converts cleanly. Only 'ready' — a 'failed' job
    // stays terminal by design (no silent auto-retry of a ggpo pull). Returns
    // true if a ghost was dropped. Safe to call at every request entrypoint.
    function dropGhostReadyJob(quarkid) {
        const job = jobs.get(quarkid);
        if (job && job.finished && job.state === 'ready' && !convertStoreServableGames(quarkid)) {
            logInfo(
                `convert ${quarkid}: 'ready' job has no store bytes (evicted/deleted) — ` +
                    `dropping ghost job so the next request re-converts`,
            );
            jobs.delete(quarkid);
            return true;
        }
        return false;
    }

    // Evict per S6 LRU caps, then purge any finished job object left pointing at
    // a store dir we just removed (an evicted quark is never active — the guard
    // skips non-finished jobs — so its lingering job is finished and now a
    // ghost). Keeps the in-memory job map consistent with the on-disk store.
    function evictStore() {
        const evicted = evictStoreIfNeeded(isQuarkActive);
        for (const quarkid of evicted) {
            const job = jobs.get(quarkid);
            if (job && job.finished) jobs.delete(quarkid);
        }
        return evicted;
    }

    // Has the tracker FINALIZED at least one game (manifest row with
    // signature_found)? A finalized game is servable — its header is patched and
    // checksum table appended on disk — so a job that has produced one is "nearly
    // done"; idle teardown leaves it to finish into the store (a cached VOD)
    // rather than discarding real work. A still-converting single game (no
    // manifest yet, or only signature_found:false rows) is NOT servable and is
    // eligible for teardown when idle.
    function jobHasServableOutput(job) {
        let manifest = null;
        try {
            manifest = JSON.parse(fs.readFileSync(path.join(job.trackDir, 'track3sr_manifest.json'), 'utf8'));
        } catch (_) {
            return false;
        }
        if (!manifest || !Array.isArray(manifest.games)) return false;
        return manifest.games.some((g) => g && g.signature_found === true);
    }

    // Tear down an abandoned job: kill the ggpo pull + the runner, free its
    // concurrency slot, wipe scratch, and drop it from tracking so a viewer
    // who later returns starts a fresh pull rather than resuming a job nobody
    // was watching. Two callers, same mechanics, different policy:
    //   - the idle sweep (`why` = 'idle_teardown'): a not-yet-servable job with
    //     no viewer for CONVERT_IDLE_TEARDOWN_MS;
    //   - slot preemption from pump() (`why` = 'preempted'): a slot-holding job
    //     with no viewer for CONVERT_PREEMPT_STALE_MS while a NEW request waits
    //     for a slot — servable output does NOT protect it here.
    // Distinct from failJob(): this is not a failure — no failedAt/retained
    // scratch, and the job is removed outright.
    function teardownAbandonedJob(job, why) {
        if (job.finished) return;
        const idleMs = Date.now() - job.lastAccess;
        logInfo(`convert ${job.quarkid}: ${why} — no viewer for ${idleMs} ms; killing pull+runner, wiping scratch`);
        job.finished = true;
        job.state = why;
        if (job.poller) {
            clearInterval(job.poller);
            job.poller = null;
        }
        if (job.dlProc) {
            try {
                job.dlProc.kill('SIGKILL');
            } catch (_) {}
            job.dlProc = null;
        }
        if (job.runnerProc) {
            try {
                job.runnerProc.kill('SIGKILL');
            } catch (_) {}
            job.runnerProc = null;
        }
        running = Math.max(0, running - 1);
        try {
            fs.rmSync(job.scratch, { recursive: true, force: true });
        } catch (_) {}
        // Fable review LOW-2: a torn-down background job just made (and killed) a
        // ggpo pull. Advance the pacing anchor so the scheduler can't immediately
        // start another pull once the preempting live job finishes — preserves the
        // 1-pull-per-gap politeness posture across preemption. Only the anchor;
        // the quark is NOT failed or de-queued (preemption is not a failure — it
        // stays eligible for a later background attempt).
        if (job.background) {
            preconvert.nextBackgroundAllowedAt =
                Date.now() + PRECONVERT_GAP_MS + Math.floor(Math.random() * (PRECONVERT_JITTER_MS + 1));
            preconvertSchedulePersist();
        }
        jobs.delete(job.quarkid);
        pump();
    }

    function catalogRowFor(quarkid) {
        const catalog = loadCatalog();
        if (!catalog || !Array.isArray(catalog.rows)) return null;
        for (const r of catalog.rows) {
            if (r && (typeof r.quarkid === 'string' || typeof r.quarkid === 'number') && String(r.quarkid) === quarkid) {
                return r;
            }
        }
        return null;
    }

    function computeProgress(job) {
        if (job.state === 'ready') return 100;
        if (job.state === 'queued') return 0;
        if (job.state === 'failed') return job.progress || 0;
        // pulling / converting: estimate from downloaded input bytes vs the
        // expected total (duration_s * 60 fps * 10 B/record). The download is
        // the rate-limiter (the runner follows it), so this tracks overall %.
        let bytes = 0;
        try {
            bytes = fs.statSync(job.inputsPath).size;
        } catch (_) {}
        if (job.expectedInputBytes > 0) {
            return Math.max(1, Math.min(99, Math.floor((100 * bytes) / job.expectedInputBytes)));
        }
        let haveState = false;
        try {
            haveState = fs.statSync(job.savestatePath).size > 0;
        } catch (_) {}
        return haveState ? 50 : 5;
    }

    function publicState(job) {
        const out = { state: job.state, progress: computeProgress(job) };
        if (job.error) out.detail = job.error;
        if (job.state === 'ready' && Array.isArray(job.games)) out.games = job.games;
        return out;
    }

    function moveInto(src, dst) {
        try {
            fs.renameSync(src, dst);
        } catch (err) {
            if (err && err.code === 'EXDEV') {
                fs.copyFileSync(src, dst);
                try {
                    fs.unlinkSync(src);
                } catch (_) {}
            } else {
                throw err;
            }
        }
    }

    // A slot-holding (started, unfinished) job whose viewer is gone: stale
    // lastAccess beyond the preemption threshold. Oldest-abandoned first.
    // A BACKGROUND job (plan-preconvert S1) has no viewer BY DEFINITION, so it
    // is ALWAYS preemptable — the CONVERT_PREEMPT_STALE_MS staleness gate is
    // skipped for it: a live request injects itself into `queue` and pump()
    // evicts the background slot-holder within that one request. For live jobs
    // the eligibility test is byte-for-byte the original (fresh viewer ⇒
    // protected), so a live viewer is never starved by this change.
    function findPreemptableJob() {
        const now = Date.now();
        let victim = null;
        for (const job of jobs.values()) {
            if (job.finished) continue;
            if (job.state === 'queued') continue; // holds no slot
            if (!job.background && now - job.lastAccess <= CONVERT_PREEMPT_STALE_MS) continue; // live viewer still polling
            if (!victim || job.lastAccess < victim.lastAccess) victim = job;
        }
        return victim;
    }

    function pump() {
        // Slot preemption: a queued request with a live viewer (it just got
        // queued / is being polled — see requestWatch/requestConvert, which
        // re-pump while their job sits queued) must not wait behind a job
        // whose viewer walked away. Servable output does NOT protect the
        // victim here — an abandoned job's "finish into the cached store"
        // leniency only holds while no viewer-ful request needs the slot.
        while (running >= CONVERT_MAX_JOBS && queue.length > 0) {
            const victim = findPreemptableJob();
            if (!victim) break;
            logInfo(`convert: preempting abandoned job ${victim.quarkid} — ${queue.length} viewer request(s) waiting for a slot`);
            teardownAbandonedJob(victim, 'preempted'); // frees the slot + re-pumps
        }
        while (running < CONVERT_MAX_JOBS && queue.length > 0) {
            const quarkid = queue.shift();
            const job = jobs.get(quarkid);
            if (!job || job.state !== 'queued') continue;
            running += 1;
            startJob(job);
        }
    }

    function finishJob(job, state) {
        if (job.finished) return;
        job.finished = true;
        job.state = state;
        job.progress = state === 'ready' ? 100 : job.progress || 0;
        if (job.poller) {
            clearInterval(job.poller);
            job.poller = null;
        }
        if (state === 'ready') {
            try {
                fs.rmSync(job.scratch, { recursive: true, force: true });
            } catch (_) {}
        } else {
            job.failedAt = Date.now(); // retained briefly for debugging, then swept
        }
        running = Math.max(0, running - 1);
        pump();
    }

    function failJob(job, msg) {
        if (job.finished) return;
        logWarn(`convert ${job.quarkid}: FAILED — ${msg}`);
        job.error = msg;
        if (job.dlProc) {
            try {
                job.dlProc.kill('SIGKILL');
            } catch (_) {}
            job.dlProc = null;
        }
        if (job.runnerProc) {
            try {
                job.runnerProc.kill('SIGKILL');
            } catch (_) {}
            job.runnerProc = null;
        }
        finishJob(job, 'failed');
        // Background failure ledger (plan-preconvert S2/§Q4): record the typed
        // reason, drop from the queue, free the lease, anchor pacing. A job that
        // a live viewer adopted (job.background cleared) is a live failure and
        // is deliberately NOT ledgered — the explicit-retry contract stands.
        if (job.background) preconvertOnJobEnded(job, true);
    }

    // Move the tracker's completed game_N.3sr files + freshly-written meta
    // sidecars into the store. Returns the published game indices.
    function publishTrackerOutput(job) {
        let manifest = null;
        try {
            manifest = JSON.parse(fs.readFileSync(path.join(job.trackDir, 'track3sr_manifest.json'), 'utf8'));
        } catch (_) {}
        const signatureFound = new Set();
        if (manifest && Array.isArray(manifest.games)) {
            for (const g of manifest.games) {
                if (g && typeof g.game_index === 'number' && g.signature_found === true) {
                    signatureFound.add(g.game_index);
                }
            }
        }

        let names = [];
        try {
            names = fs.readdirSync(job.trackDir);
        } catch (_) {}
        const candidates = [];
        for (const name of names) {
            const m = /^game_(\d+)\.3sr$/.exec(name);
            if (m) candidates.push(Number(m[1]));
        }
        candidates.sort((a, b) => a - b);

        const players = Array.isArray(job.row.players) ? job.row.players : [];
        const hasName = players.some((p) => p && typeof p.name === 'string' && p.name.trim().length > 0);

        const published = [];
        for (const idx of candidates) {
            if (signatureFound.size > 0 && !signatureFound.has(idx)) continue;
            const src3sr = path.join(job.trackDir, `game_${idx}.3sr`);
            let st;
            try {
                st = fs.statSync(src3sr);
            } catch (_) {
                continue;
            }
            if (!st.isFile() || st.size < HEADER_MIN_3SR_BYTES) continue;
            let magicOk = false;
            try {
                const fd = fs.openSync(src3sr, 'r');
                const b = Buffer.alloc(4);
                fs.readSync(fd, b, 0, 4, 0);
                fs.closeSync(fd);
                magicOk = b.toString('ascii') === '3SR1';
            } catch (_) {}
            if (!magicOk) continue;
            if (!hasName) {
                // never publish a nameless game — get3sr would refuse it anyway
                // (the catalog row carried no player names).
                logWarn(`convert ${job.quarkid}: game_${idx} — catalog row has no player names, skipping`);
                continue;
            }

            const meta = {
                quarkid: job.quarkid,
                players,
                date: typeof job.row.date === 'number' ? job.row.date : null,
                duration: typeof job.row.duration === 'number' ? job.row.duration : null,
                game_index: idx,
                source: 'quark.json',
            };

            const storeDir = convertQuarkStoreDir(job.quarkid);
            try {
                fs.mkdirSync(storeDir, { recursive: true });
                // Write the meta sidecar FIRST (atomic rename), then move the
                // .3sr in LAST: get3sr keys off the .3sr file's presence and
                // then requires the sibling meta, so the meta must already be in
                // place the instant the .3sr becomes visible.
                const metaFinal = path.join(storeDir, `game_${idx}.meta.json`);
                const metaTmp = `${metaFinal}.tmp-${process.pid}-${idx}`;
                fs.writeFileSync(metaTmp, JSON.stringify(meta, null, 2) + '\n');
                fs.renameSync(metaTmp, metaFinal);
                moveInto(src3sr, path.join(storeDir, `game_${idx}.3sr`));
                published.push(idx);
            } catch (err) {
                logWarn(`convert ${job.quarkid}: failed to move game_${idx} into store: ${err && err.message ? err.message : err}`);
            }
        }
        return published;
    }

    function tryFinalize(job) {
        if (job.finished || !job.dlDone || !job.runnerDone) return;
        let published = [];
        try {
            published = publishTrackerOutput(job);
        } catch (err) {
            job.failReason = 'no_games';
            return failJob(job, `publish failed: ${err && err.message ? err.message : err}`);
        }
        if (published.length === 0) {
            job.failReason = 'no_games';
            return failJob(job, 'no playable games produced (no game-start signature reached, or catalog row unnamed)');
        }
        job.games = published;
        // The just-published quark is the most-recently-served (fresh mtime) so
        // eviction will never pick it; run now so its growth is bounded promptly.
        markQuarkServed(job.quarkid);
        finishJob(job, 'ready');
        logInfo(`convert ${job.quarkid}: ready — game(s) ${published.join(',')} in ${Date.now() - job.createdAt} ms`);
        // A successful conversion of ANY kind (live or background) clears a
        // prior failure-ledger entry for this quark (plan-preconvert §Q4:
        // "a live success clears the ledger entry").
        preconvertClearFailure(job.quarkid);
        preconvertBumpConverted('vps'); // plan-preconvert S6: a VPS-side conversion (live or background)
        // Background pacing + queue bookkeeping (plan-preconvert S2): anchor the
        // pacing gap to this job's end and drop the item from the pre-convert
        // queue (reconcile against the store would also catch it next tick).
        if (job.background) preconvertOnJobEnded(job, false);
        // Background map hygiene (plan-preconvert S1): a finished background job
        // has no viewer polling it, and requestStatus falls back to the store
        // and still answers 'ready' with zero device change — so drop it from
        // `jobs` the moment its quark is store-servable. Thousands of background
        // conversions would otherwise grow the map without bound (live 'ready'
        // jobs are still handled by dropGhostReadyJob + eviction purge).
        if (job.background && convertStoreServableGames(job.quarkid)) {
            jobs.delete(job.quarkid);
        }
        try {
            evictStore();
        } catch (err) {
            logWarn(`store eviction (post-publish) error: ${err && err.message ? err.message : err}`);
        }
    }

    function maybeStartRunner(job) {
        if (job.runnerStarted || job.finished) return;
        let stateSize = 0;
        let inputsSize = 0;
        try {
            stateSize = fs.statSync(job.savestatePath).size;
        } catch (_) {}
        try {
            inputsSize = fs.statSync(job.inputsPath).size;
        } catch (_) {}
        // Need the (single, complete) savestate and >=1 input record (10 B).
        if (stateSize <= 0 || inputsSize < 10) return;

        job.runnerStarted = true;
        if (job.poller) {
            clearInterval(job.poller);
            job.poller = null;
        }
        job.state = 'converting';
        logInfo(`convert ${job.quarkid}: converting (savestate=${stateSize}B, inputs=${inputsSize}B, tail-follow)`);

        const runLog = fs.openSync(path.join(job.scratch, 'runner.log'), 'a');
        const runArgs = [
            job.gameid,
            '-replay-state',
            job.savestatePath,
            '-replay-inputs',
            job.inputsPath,
            '-headless',
            '-track-3sr',
            job.trackDir,
            '-replay-follow',
            '-replay-follow-idle-ms',
            String(CONVERT_FOLLOW_IDLE_MS),
        ];
        let r;
        try {
            // CWD is a per-job dir with a `roms` symlink into the runner tree
            // (the runner finds roms relative to CWD); HOME points at that dir
            // too so FBNeo's config writes land in writable scratch, not a
            // read-only system path.
            r = spawn(CONVERT_RUNNER_BIN, runArgs, {
                cwd: job.runCwd,
                env: { ...process.env, HOME: job.runCwd },
                stdio: ['ignore', runLog, runLog],
            });
        } catch (err) {
            try {
                fs.closeSync(runLog);
            } catch (_) {}
            return failJob(job, `failed to spawn runner: ${err && err.message ? err.message : err}`);
        }
        job.runnerProc = r;
        const runTimer = setTimeout(() => {
            logWarn(`convert ${job.quarkid}: runner timeout after ${CONVERT_RUNNER_TIMEOUT_MS} ms, killing`);
            try {
                r.kill('SIGKILL');
            } catch (_) {}
        }, CONVERT_RUNNER_TIMEOUT_MS);
        r.on('exit', (code, signal) => {
            clearTimeout(runTimer);
            try {
                fs.closeSync(runLog);
            } catch (_) {}
            job.runnerProc = null;
            job.runnerDone = true;
            // If the download somehow outlived the runner (runner idle-timeout),
            // stop it — we have all the tracker output we're going to get.
            if (job.dlProc) {
                try {
                    job.dlProc.kill('SIGTERM');
                } catch (_) {}
            }
            tryFinalize(job);
        });
    }

    function startJob(job) {
        job.state = 'pulling';
        try {
            fs.mkdirSync(job.dlDir, { recursive: true });
            fs.mkdirSync(job.trackDir, { recursive: true });
            fs.mkdirSync(job.runCwd, { recursive: true });
            const romsLink = path.join(job.runCwd, 'roms');
            try {
                fs.symlinkSync(path.join(CONVERT_RUNNER_DIR, 'roms'), romsLink);
            } catch (e) {
                if (!e || e.code !== 'EEXIST') throw e;
            }
        } catch (err) {
            return failJob(job, `scratch setup failed: ${err && err.message ? err.message : err}`);
        }
        logInfo(`convert ${job.quarkid}: pulling (gameid=${job.gameid}, duration=${job.row.duration}s)`);

        const dlLog = fs.openSync(path.join(job.scratch, 'download.log'), 'a');
        const dlArgs = [
            CONVERT_DOWNLOADER,
            'download',
            '--game',
            job.gameid,
            '--token',
            `${job.quarkid}.7`,
            '--host',
            CONVERT_GGPO_HOST,
            '--port',
            String(CONVERT_GGPO_PORT),
            '--out-dir',
            job.dlDir,
            '--idle-timeout',
            '2',
            '--max-idle-timeouts',
            '20',
            '--max-frames',
            '200000',
            '--local-port',
            '6004',
            '--send-delay-ms',
            '15',
        ];
        let dl;
        try {
            dl = spawn(CONVERT_PYTHON, dlArgs, { cwd: job.scratch, stdio: ['ignore', dlLog, dlLog] });
        } catch (err) {
            try {
                fs.closeSync(dlLog);
            } catch (_) {}
            return failJob(job, `failed to spawn downloader: ${err && err.message ? err.message : err}`);
        }
        job.dlProc = dl;
        const dlTimer = setTimeout(() => {
            logWarn(`convert ${job.quarkid}: download timeout after ${CONVERT_DOWNLOAD_TIMEOUT_MS} ms, killing`);
            try {
                dl.kill('SIGKILL');
            } catch (_) {}
        }, CONVERT_DOWNLOAD_TIMEOUT_MS);

        dl.on('exit', (code, signal) => {
            clearTimeout(dlTimer);
            try {
                fs.closeSync(dlLog);
            } catch (_) {}
            job.dlProc = null;
            job.dlDone = true;
            // Sentinel so the tail-following runner stops waiting for more input.
            try {
                fs.writeFileSync(`${job.inputsPath}.done`, '');
            } catch (_) {}
            if (job.finished) return;
            let haveState = false;
            try {
                haveState = fs.statSync(job.savestatePath).size > 0;
            } catch (_) {}
            if (!haveState) {
                // Typed reason for the failure ledger (plan-preconvert S2/§Q4):
                // the expired/handshake-only signature — the dominant expected
                // background failure. Never string-matched downstream.
                job.failReason = 'no_savestate';
                return failJob(job, `download produced no savestate (expired/handshake-only quark? exit=${code} sig=${signal || 'none'})`);
            }
            // Start the runner if the poller never got the chance (very short
            // sessions), then finalize once it too has exited.
            maybeStartRunner(job);
            if (!job.runnerStarted) {
                job.failReason = 'no_games';
                return failJob(job, 'download produced a savestate but no input records (nothing to convert)');
            }
            tryFinalize(job);
        });

        // Poll for savestate + first inputs, then overlap the runner.
        job.poller = setInterval(() => maybeStartRunner(job), 250);
    }

    // `opts` is INTERNAL ONLY — never populated from the wire (`convert`
    // dispatch calls this with the quarkid alone). The background scheduler
    // (plan-preconvert S2) passes `{background:true, row}`:
    //   - background:true  → job.background = true (instantly preemptable,
    //     idle-teardown-exempt, deleted from `jobs` at store-servable finish).
    //   - row              → a row SNAPSHOT used ONLY when catalogRowFor misses
    //     (a P3 backfill quark that has aged out of the current catalog) AND
    //     ONLY for background jobs; the wire `convert` op stays catalog-gated.
    function requestConvert(quarkid, opts) {
        const background = !!(opts && opts.background);
        const snapshotRow = opts && opts.row && typeof opts.row === 'object' ? opts.row : null;
        if (!CONVERT_ENABLED) {
            return { ok: false, error: 'convert_unavailable', detail: 'on-demand conversion is disabled on this proxy' };
        }
        const have = convertStoreServableGames(quarkid);
        if (have) {
            return { ok: true, quarkid, state: 'ready', progress: 100, games: have, already: true };
        }
        dropGhostReadyJob(quarkid); // no store bytes: a lingering 'ready' job is a ghost — clear it before re-convert
        let job = jobs.get(quarkid);
        if (job && job.state !== 'failed') {
            // A LIVE (non-background) request that lands on a running background
            // job ADOPTS it: clear the background flag so it gains the standard
            // viewer protections (8s preempt gate + idle teardown) and can no
            // longer be instantly preempted out from under the now-present
            // viewer. A background touch never re-flags a live job.
            if (!background && job.background) job.background = false;
            job.lastAccess = Date.now(); // a fresh convert touch keeps this job from idle teardown
            if (job.state === 'queued') pump(); // preemption chance for a viewer stuck in the queue
            return { ok: true, quarkid, ...publicState(job) };
        }
        // No job yet, or retrying a previously-failed one.
        let row = catalogRowFor(quarkid);
        // Background jobs may target a quark that has aged out of the CURRENT
        // catalog (P3 backfill); fall back to the scheduler-supplied snapshot.
        if (!row && background && snapshotRow) row = snapshotRow;
        if (!row) {
            return { ok: false, error: 'not_found', detail: `quark ${JSON.stringify(quarkid)} is not in the catalog` };
        }
        const gameid = typeof row.gameid === 'string' && row.gameid ? row.gameid : 'sfiii3nr1';
        const duration = typeof row.duration === 'number' && row.duration > 0 ? row.duration : 0;
        const scratch = path.join(CONVERT_SCRATCH_DIR, quarkid);
        try {
            fs.rmSync(scratch, { recursive: true, force: true });
        } catch (_) {}
        job = {
            quarkid,
            row,
            gameid,
            state: 'queued',
            progress: 0,
            error: null,
            games: null,
            createdAt: Date.now(),
            lastAccess: Date.now(), // bumped on every convert/convertstatus/watchpoll touch; drives idle teardown
            scratch,
            dlDir: path.join(scratch, 'dl'),
            trackDir: path.join(scratch, 'track'),
            runCwd: path.join(scratch, 'run'),
            inputsPath: path.join(scratch, 'dl', 'inputs'),
            savestatePath: path.join(scratch, 'dl', 'savestate'),
            expectedInputBytes: duration > 0 ? Math.round(duration * 600) : 0,
            dlProc: null,
            runnerProc: null,
            runnerStarted: false,
            dlDone: false,
            runnerDone: false,
            poller: null,
            finished: false,
            background, // plan-preconvert S1: scheduler-injected pre-convert (no viewer)
        };
        jobs.set(quarkid, job);
        queue.push(quarkid);
        pump();
        return { ok: true, quarkid, ...publicState(job) };
    }

    function requestStatus(quarkid) {
        dropGhostReadyJob(quarkid); // a 'ready' job with no store bytes must not report ready
        const job = jobs.get(quarkid);
        if (job) {
            if (!job.finished) job.lastAccess = Date.now(); // convertstatus poll = a viewer is present
            return { ok: true, quarkid, ...publicState(job) };
        }
        const have = convertStoreServableGames(quarkid);
        if (have) {
            preconvertBumpHit('ready'); // plan-preconvert S6: a browsed row was instantly playable
            return { ok: true, quarkid, state: 'ready', progress: 100, games: have };
        }
        preconvertBumpHit('absent'); // browsed row not yet converted
        return { ok: true, quarkid, state: 'absent', progress: 0 };
    }

    // --- watch (Stage S3) --------------------------------------------------
    // Is game_<gameIndex> already finalized (header patched, checksum table
    // appended) in the live job's trackDir? The tracker writes the manifest
    // (track3sr_manifest.json) only AFTER it fcloses a finalized game_N.3sr
    // (runner-track-3sr.patch: Track3srFinalizeGame writes the file fully, then
    // Track3srWriteManifest). So a game listed in the manifest is complete on
    // disk — the safe `done` signal for a still-running job. Store files are
    // ALWAYS final (publishTrackerOutput moved them post-finalize).
    function watchGameFinalized(job, gameIndex) {
        let manifest = null;
        try {
            manifest = JSON.parse(fs.readFileSync(path.join(job.trackDir, 'track3sr_manifest.json'), 'utf8'));
        } catch (_) {
            return false;
        }
        if (!manifest || !Array.isArray(manifest.games)) return false;
        return manifest.games.some((g) => g && typeof g.game_index === 'number' && g.game_index === gameIndex);
    }

    function nonEmptyFile(p) {
        try {
            return fs.statSync(p).size > 0;
        } catch (_) {
            return false;
        }
    }

    // Incremental-checksum side channel: read the tracker's append-only
    // `game_N.cks` from `cksFrom`, whole 8-byte entries only (both the start
    // offset and the length are floored to entry boundaries, so the client's
    // cursor is always entry-aligned). Missing side file (pre-fix runner, or
    // a finalized store copy whose scratch was swept) => empty relay — the
    // client then simply behaves exactly as before this fix (table at
    // finalize only). Never touches the `.3sr` byte stream.
    function readWatchChecksums(cksPath, cksFromArg) {
        const cksFromReq = Number.isInteger(cksFromArg) && cksFromArg >= 0 ? cksFromArg : 0;
        let size = 0;
        try {
            const st = fs.statSync(cksPath);
            if (st.isFile()) size = st.size;
        } catch (_) {}
        const alignedEnd = size - (size % 8);
        let cksFrom = Math.min(cksFromReq, alignedEnd);
        cksFrom -= cksFrom % 8;
        let buf = Buffer.alloc(0);
        if (alignedEnd > cksFrom) {
            let want = Math.min(WATCH_CKS_CHUNK_BYTES, alignedEnd - cksFrom);
            want -= want % 8;
            try {
                const fd = fs.openSync(cksPath, 'r');
                try {
                    buf = Buffer.alloc(want);
                    const got = fs.readSync(fd, buf, 0, want, cksFrom);
                    buf = buf.subarray(0, got - (got % 8));
                } finally {
                    fs.closeSync(fd);
                }
            } catch (_) {
                buf = Buffer.alloc(0);
            }
        }
        return { cks_from: cksFrom, cks_next: cksFrom + buf.length, checksums_b64: buf.toString('base64') };
    }

    // watchpoll: return the game_<gameIndex>.3sr bytes appended since `from`,
    // plus the (always-current) 28-byte header, starting or attaching to a
    // shared convert job as needed. One-request-one-response; never blocks.
    function requestWatch(quarkid, fromArg, gameIndexArg, cksFromArg) {
        const gameIndex = Number.isInteger(gameIndexArg) && gameIndexArg >= 0 ? gameIndexArg : 0;
        const from = Number.isInteger(fromArg) && fromArg >= 0 ? fromArg : 0;
        const cksFromEcho = Number.isInteger(cksFromArg) && cksFromArg >= 0 ? cksFromArg : 0;

        // 1. Resolve the file: the published store (final) first, then a live
        //    job's trackDir (possibly still growing). These never overlap — a
        //    game is in trackDir while converting and moves to the store at
        //    finalize (publishTrackerOutput), so store-present ⇒ complete.
        let filePath = null;
        let finalized = false;
        const storePath = path.join(convertQuarkStoreDir(quarkid), `game_${gameIndex}.3sr`);
        if (nonEmptyFile(storePath)) {
            filePath = storePath;
            finalized = true;
            markQuarkServed(quarkid); // watching from the finalized store keeps it warm vs. eviction
        }

        // If the store file is gone and the only job is a finished 'ready'
        // ghost, drop it now so the `!job` branch below re-enters requestConvert
        // instead of streaming zero bytes off a 'ready' state forever.
        if (!filePath) dropGhostReadyJob(quarkid);
        let job = jobs.get(quarkid);
        if (job && !job.finished) {
            if (job.background) job.background = false; // plan-preconvert S1: a watch viewer adopted a background job → promote to live (viewer protections apply)
            job.lastAccess = Date.now(); // a viewer is polling — never idle-tear-down under it
            // A polling viewer whose job is stuck QUEUED is exactly the case
            // slot preemption exists for: every poll re-runs pump() so a
            // slot-holder whose own viewer has gone stale (device relaunched
            // onto this quark) is preempted within CONVERT_PREEMPT_STALE_MS
            // instead of blocking this viewer until the abandoned job ends.
            if (job.state === 'queued') pump();
        }

        if (!filePath) {
            // No store file yet. Start a live job iff none exists — do NOT
            // auto-retry a FAILED job here (that is surfaced terminally in
            // step 2 below): a client polling a permanently-failing quark must
            // never respawn the ggpo pull on every poll. An explicit `convert`
            // op is the retry path. A live job is shared (requestConvert is
            // idempotent — it never forks a second ggpo pull for one quark).
            if (!job) {
                const started = requestConvert(quarkid);
                if (!started.ok) return started; // not_found / convert_unavailable
                if (started.already && started.state === 'ready' && nonEmptyFile(storePath)) {
                    filePath = storePath;
                    finalized = true;
                }
                job = jobs.get(quarkid);
            }
            if (!filePath && job && job.state !== 'failed') {
                const trackPath = path.join(job.trackDir, `game_${gameIndex}.3sr`);
                if (nonEmptyFile(trackPath)) {
                    filePath = trackPath;
                    finalized = watchGameFinalized(job, gameIndex);
                }
            }
        }

        // 2. Job died mid-watch → clean terminal `failed`, never a hang.
        if (!filePath && job && job.state === 'failed') {
            return {
                ok: true, quarkid, game_index: gameIndex, state: 'failed',
                detail: job.error || 'conversion failed',
                size: 0, from, next: from, header_b64: '', b64: '', eof: true, done: true,
                cks_from: cksFromEcho, cks_next: cksFromEcho, checksums_b64: '',
            };
        }

        // 3. No bytes yet (still pulling, or no game-start signature reached) →
        //    tell the client to keep polling for setup. `progress` (0..100, the
        //    same estimate convertstatus serves) lets the device render an
        //    honest "CONVERTING n%" instead of a dead "connecting" line.
        if (!filePath) {
            const state = job ? job.state : 'absent';
            const progress = job && !job.finished ? computeProgress(job) : 0;
            return {
                ok: true, quarkid, game_index: gameIndex, state, progress,
                size: 0, from, next: from, header_b64: '', b64: '', eof: true, done: false,
                cks_from: cksFromEcho, cks_next: cksFromEcho, checksums_b64: '',
            };
        }

        // 4. Stream a byte range: the current header (always) + body[from, +chunk).
        let size = 0;
        try {
            size = fs.statSync(filePath).size;
        } catch (_) {}
        const bodyStart = Math.max(from, WATCH_HEADER_BYTES);
        let headerBuf = Buffer.alloc(0);
        let bodyBuf = Buffer.alloc(0);
        let next = bodyStart;
        try {
            const fd = fs.openSync(filePath, 'r');
            try {
                const headerLen = Math.min(WATCH_HEADER_BYTES, size);
                if (headerLen > 0) {
                    headerBuf = Buffer.alloc(headerLen);
                    const gotH = fs.readSync(fd, headerBuf, 0, headerLen, 0);
                    if (gotH < headerLen) headerBuf = headerBuf.subarray(0, gotH);
                }
                if (size > bodyStart) {
                    const want = Math.min(WATCH_CHUNK_BYTES, size - bodyStart);
                    bodyBuf = Buffer.alloc(want);
                    const gotB = fs.readSync(fd, bodyBuf, 0, want, bodyStart);
                    if (gotB < want) bodyBuf = bodyBuf.subarray(0, gotB);
                    next = bodyStart + bodyBuf.length;
                }
            } finally {
                fs.closeSync(fd);
            }
        } catch (err) {
            return { ok: false, error: 'not_found', detail: `watch read failed: ${err && err.message ? err.message : err}` };
        }

        const eof = next >= size;
        // done: the game is finalized AND the client has now been sent every
        // byte (header re-sent this poll carries the finalize patch).
        const done = finalized && eof && size >= WATCH_HEADER_BYTES;
        const state = job && !job.finished ? job.state : 'ready';
        const progress = job && !job.finished ? computeProgress(job) : 100;
        // Incremental checksums ride along on the second cursor. The side
        // file sits next to whichever game_N.3sr we resolved (only a live
        // job's trackDir ever has one; a store path yields an empty relay).
        const cks = readWatchChecksums(filePath.replace(/\.3sr$/, '.cks'), cksFromArg);
        return {
            ok: true, quarkid, game_index: gameIndex, state, progress,
            size, from: bodyStart, next,
            header_b64: headerBuf.toString('base64'),
            b64: bodyBuf.toString('base64'),
            eof, done,
            cks_from: cks.cks_from, cks_next: cks.cks_next, checksums_b64: cks.checksums_b64,
        };
    }

    // --- pre-convert scheduler (plan-preconvert-fleet.md S2) ----------------
    // Persistent priority queue + lease table + failure ledger, all in
    // preconvert-state.json. The tick starts ONE background job when idle,
    // paced, and above the disk floor; completion is detected by STORE TRUTH
    // (convertStoreServableGames), never by job state. All state mutates only
    // these plain objects — no process is ever spawned here except via the
    // existing requestConvert({background:true}) entry point.
    const preconvert = {
        version: 1,
        queue: [], // [{quarkid, tier, date, duration, row}]  tier: 1=P1 best, 2=P2 fresh, 3=P3 backfill
        leases: {}, // quarkid -> {worker, expires_at}
        failed: {}, // quarkid -> {reason, attempts, last_at, retry_after, permanent}
        counters: {}, // S6 observability rollovers
        nextBackgroundAllowedAt: 0, // pacing anchor (ms epoch); 0 ⇒ start immediately
        seenCatalogMtime: -1, // enqueuer runs when catalog mtime differs from this
    };

    // --- persistence: tolerant load, atomic + debounced save ---------------
    function preconvertLoad() {
        let j = null;
        try {
            j = JSON.parse(fs.readFileSync(PRECONVERT_STATE_FILE, 'utf8'));
        } catch (_) {
            j = null; // missing/corrupt → start empty; catalog enqueue self-heals
        }
        if (!j || typeof j !== 'object' || Array.isArray(j)) return;
        if (Array.isArray(j.queue)) {
            preconvert.queue = j.queue.filter(
                (it) => it && typeof it === 'object' && typeof it.quarkid === 'string' && QUARKID_RE.test(it.quarkid),
            );
        }
        if (j.leases && typeof j.leases === 'object' && !Array.isArray(j.leases)) preconvert.leases = j.leases;
        if (j.failed && typeof j.failed === 'object' && !Array.isArray(j.failed)) preconvert.failed = j.failed;
        if (j.counters && typeof j.counters === 'object' && !Array.isArray(j.counters)) preconvert.counters = j.counters;
        if (typeof j.nextBackgroundAllowedAt === 'number') preconvert.nextBackgroundAllowedAt = j.nextBackgroundAllowedAt;
        if (typeof j.seenCatalogMtime === 'number') preconvert.seenCatalogMtime = j.seenCatalogMtime;
    }

    let preconvertSaveTimer = null;
    let preconvertSavePending = false;
    function preconvertPersistNow() {
        preconvertSavePending = false;
        if (preconvertSaveTimer) {
            clearTimeout(preconvertSaveTimer);
            preconvertSaveTimer = null;
        }
        const snapshot = {
            version: preconvert.version,
            queue: preconvert.queue,
            leases: preconvert.leases,
            failed: preconvert.failed,
            counters: preconvert.counters,
            nextBackgroundAllowedAt: preconvert.nextBackgroundAllowedAt,
            seenCatalogMtime: preconvert.seenCatalogMtime,
        };
        try {
            const tmp = `${PRECONVERT_STATE_FILE}.tmp-${process.pid}`;
            fs.writeFileSync(tmp, JSON.stringify(snapshot));
            fs.renameSync(tmp, PRECONVERT_STATE_FILE); // atomic, same pattern as the meta sidecar publish
        } catch (err) {
            logWarn(`preconvert: failed to persist state: ${err && err.message ? err.message : err}`);
        }
    }
    function preconvertSchedulePersist() {
        preconvertSavePending = true;
        if (preconvertSaveTimer) return; // debounce: coalesce a burst of mutations
        preconvertSaveTimer = setTimeout(() => {
            preconvertSaveTimer = null;
            if (preconvertSavePending) preconvertPersistNow();
        }, PRECONVERT_SAVE_DEBOUNCE_MS);
        preconvertSaveTimer.unref();
    }

    // --- small helpers -----------------------------------------------------
    function preconvertLeaseActive(quarkid, now) {
        const l = preconvert.leases[quarkid];
        return !!(l && typeof l.expires_at === 'number' && l.expires_at > now);
    }
    function preconvertFreeLease(quarkid) {
        if (preconvert.leases[quarkid]) delete preconvert.leases[quarkid];
    }
    function preconvertPruneExpiredLeases(now) {
        for (const [q, l] of Object.entries(preconvert.leases)) {
            if (!l || typeof l.expires_at !== 'number' || l.expires_at <= now) delete preconvert.leases[q];
        }
    }
    function preconvertClearFailure(quarkid) {
        if (preconvert.failed[quarkid]) {
            delete preconvert.failed[quarkid];
            preconvertSchedulePersist();
        }
    }
    // Is this quark blocked from (re-)conversion by the failure ledger right now?
    function preconvertLedgerBlocks(quarkid, now) {
        const e = preconvert.failed[quarkid];
        if (!e) return false;
        if (e.permanent) return true;
        return typeof e.retry_after === 'number' && now < e.retry_after;
    }
    function preconvertRemoveFromQueue(quarkid) {
        const before = preconvert.queue.length;
        preconvert.queue = preconvert.queue.filter((it) => it.quarkid !== quarkid);
        return preconvert.queue.length !== before;
    }
    // Record a typed failure with the §Q4 retry policy.
    function preconvertRecordFailure(quarkid, reason) {
        const now = Date.now();
        const e = preconvert.failed[quarkid] || { reason, attempts: 0, last_at: 0, retry_after: 0, permanent: false };
        e.attempts += 1;
        e.reason = reason;
        e.last_at = now;
        if (reason === 'no_savestate') {
            // 1 retry after 24 h, then permanent (entry KEPT so a re-appearing
            // catalog row does not re-enqueue an expired quark).
            if (e.attempts >= 2) {
                e.permanent = true;
                e.retry_after = 0;
            } else {
                e.retry_after = now + PRECONVERT_NO_SAVESTATE_RETRY_MS;
            }
        } else {
            // backoff 1 h → 4 h → 24 h across the retries, then permanent once
            // all backoffs are exhausted.
            if (e.attempts > PRECONVERT_BACKOFF_MS.length) {
                e.permanent = true;
                e.retry_after = 0;
            } else {
                e.retry_after = now + PRECONVERT_BACKOFF_MS[e.attempts - 1];
            }
        }
        preconvert.failed[quarkid] = e;
        // Bound the ledger: drop the oldest (by last_at) beyond the cap.
        const ids = Object.keys(preconvert.failed);
        if (ids.length > PRECONVERT_LEDGER_MAX) {
            ids.sort((a, b) => (preconvert.failed[a].last_at || 0) - (preconvert.failed[b].last_at || 0));
            for (const id of ids.slice(0, ids.length - PRECONVERT_LEDGER_MAX)) delete preconvert.failed[id];
        }
        preconvertSchedulePersist();
    }

    // Called from failJob/tryFinalize when a BACKGROUND job ends. Anchors the
    // pacing gap to the job's end (+ jitter), removes it from the queue, frees
    // its lease, and — on failure — records the typed reason in the ledger.
    function preconvertOnJobEnded(job, failedOutcome) {
        const now = Date.now();
        preconvert.nextBackgroundAllowedAt = now + PRECONVERT_GAP_MS + Math.floor(Math.random() * (PRECONVERT_JITTER_MS + 1));
        if (failedOutcome) preconvertRecordFailure(job.quarkid, job.failReason || 'error');
        preconvertRemoveFromQueue(job.quarkid);
        preconvertFreeLease(job.quarkid);
        preconvertSchedulePersist();
    }

    // --- observability counters (plan-preconvert S6 / §Q7) -----------------
    // All live in preconvert.counters so they persist (debounced) across a
    // restart. `converted_today` rolls over at the UTC-day boundary.
    function preconvertRolloverToday() {
        const day = Math.floor(Date.now() / (24 * 60 * 60 * 1000));
        if (preconvert.counters.today_day !== day) {
            preconvert.counters.today_day = day;
            preconvert.counters.converted_today = 0;
        }
    }
    function preconvertBumpConverted(source) {
        preconvertRolloverToday();
        const c = preconvert.counters;
        c.converted_total = (c.converted_total || 0) + 1;
        c.converted_today = (c.converted_today || 0) + 1;
        if (source === 'worker') c.converted_by_worker = (c.converted_by_worker || 0) + 1;
        else c.converted_by_vps = (c.converted_by_vps || 0) + 1;
        preconvertSchedulePersist();
    }
    // The direct measure of "rows instantly playable when browsed": every
    // requestStatus store-fallback outcome (§Q7). Persist is debounced, so a
    // high poll rate coalesces into at most one write per debounce interval.
    function preconvertBumpHit(kind) {
        // Fable review MED-1: convertstatus is a shipped, unauthenticated,
        // device-polled hot path. The counters here are pure cache-hit
        // observability, but the original code also called
        // preconvertSchedulePersist() — adding a debounced preconvert-state.json
        // disk write to every browse poll, including while the feature is
        // deployed dark (a low-grade write-amplification an internet client
        // could sustain). Fix: count in MEMORY only, never schedule a persist
        // from this path. The counters are still surfaced in `status` and still
        // ride along whenever the state file is written for a real reason (an
        // active scheduler/workdone); when dark, nothing persists them and they
        // reset on restart — acceptable for observability, and convertstatus
        // regains its zero-disk-side-effect shape.
        const c = preconvert.counters;
        if (kind === 'ready') c.hit_status_ready = (c.hit_status_ready || 0) + 1;
        else c.hit_status_absent = (c.hit_status_absent || 0) + 1;
    }
    function preconvertMarkWorker(worker) {
        preconvert.counters.worker_last_seen = Date.now();
        preconvert.counters.worker_last_name = typeof worker === 'string' ? worker : null;
        preconvertSchedulePersist();
    }

    // The `preconvert` block surfaced in the `status` op (§Q7). Read-only.
    // NOTE: this does a full scanStoreQuarks — acceptable because `status` is
    // an infrequent operator/boot call, not the per-row convertstatus poll.
    function preconvertStats() {
        const now = Date.now();
        preconvertPruneExpiredLeases(now);
        preconvertRolloverToday();
        let p1 = 0;
        let p2 = 0;
        let p3 = 0;
        let leased = 0;
        for (const it of preconvert.queue) {
            if (it.tier === 1) p1 += 1;
            else if (it.tier === 2) p2 += 1;
            else p3 += 1;
            if (preconvertLeaseActive(it.quarkid, now)) leased += 1;
        }
        let permanent = 0;
        for (const e of Object.values(preconvert.failed)) if (e && e.permanent) permanent += 1;
        const scan = scanStoreQuarks(resolve3srDir());
        let diskFree = null;
        try {
            const st = fs.statfsSync(resolve3srDir());
            diskFree = st.bavail * st.bsize;
        } catch (_) {}
        const c = preconvert.counters;
        return {
            enabled: PRECONVERT_ENABLED,
            best_only: PRECONVERT_BEST_ONLY,
            queue: { p1, p2, p3, leased },
            failed: { total: Object.keys(preconvert.failed).length, permanent },
            converted: {
                total: c.converted_total || 0,
                today: c.converted_today || 0,
                by_vps: c.converted_by_vps || 0,
                by_worker: c.converted_by_worker || 0,
            },
            store: {
                quarks: scan ? scan.quarks.length : 0,
                bytes: scan ? scan.totalBytes : 0,
                disk_free_bytes: diskFree,
            },
            hit: { status_ready: c.hit_status_ready || 0, status_absent: c.hit_status_absent || 0 },
            worker: { last_seen: c.worker_last_seen || 0, last_worker: c.worker_last_name || null },
        };
    }

    // Trim a catalog row to the fields the converter + publisher need, so the
    // persisted snapshot stays small (row used only when the quark has aged out
    // of the current catalog — §Q1 row snapshot).
    function preconvertSnapshotRow(r) {
        return {
            quarkid: String(r.quarkid),
            gameid: typeof r.gameid === 'string' && r.gameid ? r.gameid : PRECONVERT_GAMEID,
            date: typeof r.date === 'number' ? r.date : null,
            duration: typeof r.duration === 'number' ? r.duration : null,
            players: Array.isArray(r.players)
                ? r.players.map((p) => ({
                      name: p && typeof p.name === 'string' ? p.name : '',
                      country: p && p.country != null ? p.country : null,
                      rank: p && p.rank != null ? p.rank : null,
                      score: p && typeof p.score === 'number' ? p.score : null,
                  }))
                : [],
        };
    }
    // P1 = catalog_best (best-lists change slowly; a one-time backfill buys a
    // permanently-ready BEST tab); else P2 (fresh feed the device browses now,
    // but the firehose inflow means P2 never fully drains). P3 is assigned
    // only by demotion when a queued quark has left the current catalog.
    function preconvertClassifyTier(r) {
        return r && r.catalog_best === true ? 1 : 2;
    }

    // Store at/above STORE_EVICT_TRIGGER_FRACTION (90 %) of either cap? The
    // enqueuer never adds while true, to avoid an enqueue→convert→evict→
    // re-enqueue loop (§Q3 churn guard). Shares the single-source-of-truth
    // fraction with evictStoreIfNeeded's trigger (review amendment,
    // plan-bounded-pool-replay.md §6.2 Stage 3) so the guard can never latch
    // while eviction is still dormant.
    function preconvertStoreAtChurnCap() {
        const scan = scanStoreQuarks(resolve3srDir());
        if (!scan) return false;
        if (scan.quarks.length >= STORE_EVICT_TRIGGER_FRACTION * STORE_MAX_QUARKS) return true;
        if (scan.totalBytes >= STORE_EVICT_TRIGGER_FRACTION * STORE_MAX_BYTES) return true;
        return false;
    }

    function preconvertBelowDiskFloor() {
        try {
            const st = fs.statfsSync(resolve3srDir());
            return st.bavail * st.bsize < DISK_FLOOR_BYTES;
        } catch (_) {
            return true; // can't measure ⇒ fail-closed: don't start speculative work
        }
    }

    // Catalog-mtime-driven enqueuer (§Q4). Refreshes tiers of existing queue
    // items (demoting any that left the catalog to P3), then adds new eligible
    // rows. Runs only when the catalog mtime changed (or force=true).
    function preconvertEnqueueFromCatalog(force) {
        const catalog = loadCatalog();
        if (!catalog || !Array.isArray(catalog.rows)) return;
        const mtime = catalogState.mtimeMs; // set by loadCatalog() just above
        if (!force && mtime === preconvert.seenCatalogMtime) return;
        preconvert.seenCatalogMtime = mtime;

        const now = Date.now();
        const rowById = new Map();
        for (const r of catalog.rows) {
            if (!r || typeof r !== 'object') continue;
            if (r.gameid !== PRECONVERT_GAMEID) continue;
            if (typeof r.quarkid !== 'string' && typeof r.quarkid !== 'number') continue;
            const qid = String(r.quarkid);
            if (!QUARKID_RE.test(qid)) continue;
            rowById.set(qid, r);
        }

        // (1) refresh/demote existing queue items in place.
        for (const it of preconvert.queue) {
            const r = rowById.get(it.quarkid);
            if (r) {
                it.tier = preconvertClassifyTier(r);
                it.row = preconvertSnapshotRow(r);
                if (typeof r.date === 'number') it.date = r.date;
                if (typeof r.duration === 'number') it.duration = r.duration;
            } else {
                it.tier = 3; // P3 demotion; keep the stored row snapshot
            }
        }

        // (2) add new eligible rows (skip if the store is near its cap).
        if (!preconvertStoreAtChurnCap()) {
            const queued = new Set(preconvert.queue.map((it) => it.quarkid));
            for (const [qid, r] of rowById) {
                if (queued.has(qid)) continue;
                // Best-tier only (see PRECONVERT_BEST_ONLY): never ADD a row
                // the device's weekly-best set would not contain.
                if (PRECONVERT_BEST_ONLY && preconvertClassifyTier(r) !== 1) continue;
                if (preconvertLeaseActive(qid, now)) continue;
                if (convertStoreServableGames(qid)) continue;
                if (preconvertLedgerBlocks(qid, now)) continue;
                preconvert.queue.push({
                    quarkid: qid,
                    tier: preconvertClassifyTier(r),
                    date: typeof r.date === 'number' ? r.date : null,
                    duration: typeof r.duration === 'number' ? r.duration : null,
                    row: preconvertSnapshotRow(r),
                });
            }
        }

        // (3) P3 queue hygiene (plan-bounded-pool-replay.md §6.1 Stage 4): age
        // out stale demoted rows, then a hard cap as a backstop. Both steps
        // only ever touch tier-3 items and never a leased one (a Mac worker
        // mid-convert on a P3 row must not have its item vanish out from
        // under the lease — the job would finish, then find nothing to
        // remove/free against on completion). Dropping here never orphans a
        // lease (skipped by construction) or the failure ledger (`preconvert.
        // failed` is keyed by quarkid independent of queue membership, and is
        // consulted by quarkid at re-enqueue time regardless of whether a
        // queue row currently exists for it).
        preconvert.queue = preconvert.queue.filter((it) => {
            if (it.tier !== 3) return true;
            if (preconvertLeaseActive(it.quarkid, now)) return true;
            const age = now - (typeof it.date === 'number' ? it.date : 0);
            return age <= PRECONVERT_P3_MAX_AGE_MS;
        });
        if (preconvert.queue.length > PRECONVERT_QUEUE_MAX) {
            const overflow = preconvert.queue.length - PRECONVERT_QUEUE_MAX;
            // Oldest-date P3 first; P1/P2 and leased P3 are never candidates.
            const evictable = preconvert.queue
                .filter((it) => it.tier === 3 && !preconvertLeaseActive(it.quarkid, now))
                .sort((a, b) => (a.date || 0) - (b.date || 0));
            if (evictable.length === 0) {
                logWarn(
                    `preconvert: queue at ${preconvert.queue.length} exceeds FCADE_PRECONVERT_QUEUE_MAX ` +
                        `(${PRECONVERT_QUEUE_MAX}) with no evictable P3 items — P1/P2 (+ leased P3) alone ` +
                        `exceed the cap; keeping all of them rather than dropping priority work`,
                );
            } else {
                const toDrop = new Set(evictable.slice(0, Math.min(overflow, evictable.length)).map((it) => it.quarkid));
                preconvert.queue = preconvert.queue.filter((it) => !toDrop.has(it.quarkid));
                if (toDrop.size < overflow) {
                    logWarn(
                        `preconvert: queue still at ${preconvert.queue.length}, over FCADE_PRECONVERT_QUEUE_MAX ` +
                            `(${PRECONVERT_QUEUE_MAX}) after evicting every eligible P3 item — P1/P2 (+ leased P3) ` +
                            `alone exceed the cap; keeping all of them rather than dropping priority work`,
                    );
                }
            }
        }

        preconvertSchedulePersist();
    }

    // Remove queue items that have become store-servable (a background job
    // finished, a live convert produced them, or the Mac worker pushed them).
    // Store truth is the ONLY completion signal (§Q1).
    function preconvertReconcile() {
        let changed = false;
        preconvert.queue = preconvert.queue.filter((it) => {
            if (convertStoreServableGames(it.quarkid)) {
                preconvertFreeLease(it.quarkid);
                preconvertClearFailure(it.quarkid);
                changed = true;
                return false;
            }
            return true;
        });
        if (changed) preconvertSchedulePersist();
    }

    // The single convert slot is busy (live OR background) or a live request is
    // queued: the scheduler must not inject background work.
    function preconvertSlotBusy() {
        if (queue.length > 0) return true;
        for (const job of jobs.values()) if (!job.finished) return true;
        return false;
    }

    // Highest-priority eligible queue item: lowest tier, then newest date.
    // reconcile ran first, so remaining items are not store-servable this tick.
    function preconvertPickEligible(now) {
        let best = null;
        for (const it of preconvert.queue) {
            if (preconvertLeaseActive(it.quarkid, now)) continue;
            if (preconvertLedgerBlocks(it.quarkid, now)) continue;
            if (!best || it.tier < best.tier || (it.tier === best.tier && (it.date || 0) > (best.date || 0))) best = it;
        }
        return best;
    }

    function preconvertTick() {
        if (!PRECONVERT_ENABLED) return;
        try {
            preconvertEnqueueFromCatalog(false);
            preconvertReconcile();
            const now = Date.now();
            preconvertPruneExpiredLeases(now);
            if (preconvertSlotBusy()) return; // live path not idle
            if (now < preconvert.nextBackgroundAllowedAt) return; // pacing gap
            if (preconvertBelowDiskFloor()) return; // disk floor
            const item = preconvertPickEligible(now);
            if (!item) return;
            const res = requestConvert(item.quarkid, { background: true, row: item.row });
            if (!res.ok) {
                // The row is gone / convert disabled — don't spin on it.
                preconvertRecordFailure(item.quarkid, 'not_found');
                preconvertRemoveFromQueue(item.quarkid);
                preconvertSchedulePersist();
            }
        } catch (err) {
            logWarn(`preconvert scheduler tick error: ${err && err.stack ? err.stack : err}`);
        }
    }

    // --- Mac-worker work-lease ops (plan-preconvert S4) --------------------
    // Token-gated in dispatch(); these methods assume the caller is authorized.
    // They mutate ONLY the pre-convert queue/lease/ledger metadata and the
    // validated 3sr-incoming/<quarkid> staging dir — never spawn a process.

    function wipeStaging(dir) {
        try {
            fs.rmSync(dir, { recursive: true, force: true });
        } catch (_) {}
    }

    // worklease: hand out up to `count` (default 1, max 3) highest-priority
    // items NOT held by another worker to `worker`. Re-leasing an item this
    // worker already holds renews (extends) it. Store-servable / ledger-blocked
    // items are never leased.
    function requestWorkLease(worker, count) {
        const now = Date.now();
        preconvertMarkWorker(worker); // plan-preconvert S6: worker last-seen
        preconvertPruneExpiredLeases(now);
        preconvertReconcile(); // drop anything already store-servable first
        const n = Math.max(1, Math.min(WORK_LEASE_MAX_COUNT, Number.isInteger(count) ? count : 1));
        const candidates = preconvert.queue
            .filter((it) => {
                const l = preconvert.leases[it.quarkid];
                if (l && typeof l.expires_at === 'number' && l.expires_at > now && l.worker !== worker) return false; // held by another worker
                if (preconvertLedgerBlocks(it.quarkid, now)) return false;
                if (convertStoreServableGames(it.quarkid)) return false;
                return true;
            })
            .sort((a, b) => a.tier - b.tier || (b.date || 0) - (a.date || 0))
            .slice(0, n);
        const leased = [];
        for (const it of candidates) {
            preconvert.leases[it.quarkid] = { worker, expires_at: now + WORK_LEASE_TTL_MS };
            leased.push({ quarkid: it.quarkid, tier: it.tier, date: it.date, duration: it.duration, row: it.row });
        }
        if (leased.length > 0) preconvertSchedulePersist();
        return { ok: true, leased, lease_ttl_ms: WORK_LEASE_TTL_MS };
    }

    // workdone: integrate (or reject) a pushed conversion. quarkid is validated
    // in dispatch. ok:false → the worker reports its own failure (ledger it).
    // ok:true → validate the staged files with the EXACT serving validation
    // (read3srGameData) and atomically, meta-first, rename them into 3sr/.
    function requestWorkDone(worker, quarkid, ok, reason) {
        preconvertMarkWorker(worker); // plan-preconvert S6: worker last-seen
        const incomingDir = path.join(resolveIncomingDir(), quarkid);

        if (ok === false) {
            preconvertRecordFailure(quarkid, typeof reason === 'string' && reason ? reason.slice(0, 64) : 'worker_failed');
            preconvertRemoveFromQueue(quarkid);
            preconvertFreeLease(quarkid);
            wipeStaging(incomingDir);
            preconvertSchedulePersist();
            return { ok: true, quarkid, integrated: [], recorded: 'failed' };
        }

        // Idempotent: an already-servable quark is a no-op success (a duplicate
        // push or a lease-expiry race — §Q8). Wipe the redundant staging.
        const alreadyServable = convertStoreServableGames(quarkid);
        if (alreadyServable) {
            wipeStaging(incomingDir);
            preconvertRemoveFromQueue(quarkid);
            preconvertFreeLease(quarkid);
            preconvertClearFailure(quarkid);
            preconvertSchedulePersist();
            return { ok: true, quarkid, integrated: alreadyServable, already: true };
        }

        // Disk floor: refuse the integration (and free the just-pushed staging,
        // which also reclaims space). The worker retries later.
        if (preconvertBelowDiskFloor()) {
            wipeStaging(incomingDir);
            preconvertFreeLease(quarkid);
            return { ok: false, error: 'disk_floor', detail: 'free disk below the floor; push refused' };
        }

        const indices = listQuarkGameIndices(incomingDir);
        if (indices === null || indices.length === 0) {
            wipeStaging(incomingDir);
            preconvertFreeLease(quarkid);
            return { ok: false, error: 'invalid_push', detail: 'no game_N.3sr in staging' };
        }
        const valid = [];
        for (const idx of indices) {
            if (read3srGameData(incomingDir, idx).ok) valid.push(idx);
        }
        if (valid.length === 0) {
            wipeStaging(incomingDir);
            preconvertFreeLease(quarkid);
            return { ok: false, error: 'invalid_push', detail: 'no staged game passed validation (magic/size/meta/name)' };
        }

        const storeDir = convertQuarkStoreDir(quarkid);
        try {
            fs.mkdirSync(storeDir, { recursive: true });
            for (const idx of valid) {
                // Meta FIRST, then the .3sr — get3sr keys off the .3sr's presence
                // and requires the sibling meta, so the meta must be in place the
                // instant the .3sr becomes visible (same rule as publishTrackerOutput).
                moveInto(path.join(incomingDir, `game_${idx}.meta.json`), path.join(storeDir, `game_${idx}.meta.json`));
                moveInto(path.join(incomingDir, `game_${idx}.3sr`), path.join(storeDir, `game_${idx}.3sr`));
            }
        } catch (err) {
            wipeStaging(incomingDir);
            preconvertFreeLease(quarkid);
            return { ok: false, error: 'integrate_failed', detail: err && err.message ? err.message : String(err) };
        }
        wipeStaging(incomingDir);
        markQuarkServed(quarkid); // freshly integrated ⇒ warm against eviction
        preconvertRemoveFromQueue(quarkid);
        preconvertFreeLease(quarkid);
        preconvertClearFailure(quarkid);
        preconvertBumpConverted('worker'); // plan-preconvert S6: a Mac-worker conversion
        preconvertSchedulePersist();
        logInfo(`workdone ${quarkid}: integrated game(s) ${valid.join(',')} from worker ${worker}`);
        try {
            evictStore(); // bound the store on the workdone hook (plan-preconvert S3)
        } catch (err) {
            logWarn(`store eviction (post-workdone) error: ${err && err.message ? err.message : err}`);
        }
        return { ok: true, quarkid, integrated: valid };
    }

    // workstats: worker-facing queue snapshot.
    function requestWorkStats() {
        const now = Date.now();
        preconvertPruneExpiredLeases(now);
        let p1 = 0;
        let p2 = 0;
        let p3 = 0;
        let leased = 0;
        for (const it of preconvert.queue) {
            if (it.tier === 1) p1 += 1;
            else if (it.tier === 2) p2 += 1;
            else p3 += 1;
            if (preconvertLeaseActive(it.quarkid, now)) leased += 1;
        }
        let permanent = 0;
        for (const e of Object.values(preconvert.failed)) if (e && e.permanent) permanent += 1;
        return {
            ok: true,
            queue: { p1, p2, p3, leased },
            failed: { total: Object.keys(preconvert.failed).length, permanent },
            lease_ttl_ms: WORK_LEASE_TTL_MS,
        };
    }

    // Periodic sweep (Stage S6): (a) idle teardown of watch-initiated jobs whose
    // viewers all walked away; (b) reclaim a failed job's scratch after the
    // retain window + remove orphan scratch dirs (crash residue); (c) bound the
    // .3sr store size via LRU eviction. Runs every CONVERT_SWEEP_INTERVAL_MS.
    function runSweep() {
        const now = Date.now();

        // (a) Idle teardown: kill a job nobody is watching anymore, UNLESS it has
        // already produced servable output (leave that to finalize into the store
        // as a cached VOD). Snapshot first — teardownAbandonedJob mutates `jobs`.
        const idleVictims = [];
        for (const [, job] of jobs) {
            if (job.finished) continue; // ready/failed handled below / already terminal
            if (job.background) continue; // plan-preconvert S1: background jobs have no viewer — lastAccess staleness is meaningless; only live preemption (findPreemptableJob) may reclaim their slot
            if (now - job.lastAccess <= CONVERT_IDLE_TEARDOWN_MS) continue; // a viewer is still polling
            if (jobHasServableOutput(job)) continue; // nearly done — let it finish into the store
            idleVictims.push(job);
        }
        for (const job of idleVictims) teardownAbandonedJob(job, 'idle_teardown');

        // (b) Failed-job scratch reclamation + orphan scratch cleanup.
        for (const [quarkid, job] of jobs) {
            if (job.state === 'failed' && job.failedAt && now - job.failedAt > CONVERT_SCRATCH_RETAIN_MS) {
                try {
                    fs.rmSync(job.scratch, { recursive: true, force: true });
                } catch (_) {}
                jobs.delete(quarkid);
            }
        }
        let entries = [];
        try {
            entries = fs.readdirSync(CONVERT_SCRATCH_DIR);
        } catch (_) {}
        for (const name of entries) {
            const job = jobs.get(name);
            if (job && !job.finished) continue;
            const p = path.join(CONVERT_SCRATCH_DIR, name);
            try {
                if (now - fs.statSync(p).mtimeMs > CONVERT_SCRATCH_RETAIN_MS) {
                    fs.rmSync(p, { recursive: true, force: true });
                }
            } catch (_) {}
        }

        // NOTE (plan-preconvert S3): store eviction is NO LONGER done here. At
        // the raised S3 caps a full scanStoreQuarks every 15 s is pure waste;
        // eviction now runs on publish (tryFinalize), on workdone integration
        // (S4), and on the dedicated STORE_EVICT_SWEEP_MS timer below.
    }
    const sweeper = setInterval(runSweep, CONVERT_SWEEP_INTERVAL_MS);
    sweeper.unref();

    // Dedicated store-eviction timer (plan-preconvert S3): the ONLY periodic
    // store bound, at a much slower cadence than the 15 s maintenance sweep so
    // the expensive full-store scan runs ~6×/h instead of 240×/h. Also catches
    // growth from out-of-band push-3sr.sh publishes between conversions.
    const storeEvictTimer = setInterval(() => {
        try {
            evictStore();
        } catch (err) {
            logWarn(`store eviction (slow timer) error: ${err && err.message ? err.message : err}`);
        }
    }, STORE_EVICT_SWEEP_MS);
    storeEvictTimer.unref();

    // Evict once at startup too, so lowering a cap (or an already-oversized store)
    // takes effect immediately rather than on the first sweep tick.
    try {
        evictStore();
    } catch (_) {}

    // Load the persistent pre-convert queue/leases/ledger (tolerant: corrupt or
    // missing → empty, the next catalog enqueue self-heals) and start the
    // scheduler tick. The tick is a no-op while FCADE_PRECONVERT_ENABLED=0, so
    // this is safe to run even when the feature is deployed dark.
    preconvertLoad();
    const preconvertTimer = setInterval(preconvertTick, PRECONVERT_TICK_MS);
    preconvertTimer.unref();

    return {
        requestConvert,
        requestStatus,
        requestWatch,
        requestWorkLease,
        requestWorkDone,
        requestWorkStats,
        preconvertStats,
        _evictStoreIfNeeded: () => evictStoreIfNeeded(isQuarkActive),
        _evictStore: evictStore, // test hook: the slow-timer eviction path (evict + purge finished jobs of evicted quarks)
        _markQuarkServed: markQuarkServed,
        _sweepOnce: runSweep, // test hook: run one maintenance sweep synchronously
        _isQuarkActive: isQuarkActive,
        stats() {
            let active = 0;
            for (const job of jobs.values()) {
                if (!job.finished) active += 1;
            }
            return {
                enabled: CONVERT_ENABLED,
                max_jobs: CONVERT_MAX_JOBS,
                active,
                tracked: jobs.size,
                queued: queue.length,
                idle_teardown_ms: CONVERT_IDLE_TEARDOWN_MS,
                preempt_stale_ms: CONVERT_PREEMPT_STALE_MS,
                store_eviction: STORE_EVICTION_ENABLED,
                store_max_quarks: STORE_MAX_QUARKS,
                store_max_bytes: STORE_MAX_BYTES,
            };
        },
        _jobs: jobs,
        // --- pre-convert scheduler test hooks (plan-preconvert S2) ---------
        // Not reachable over the wire; the scheduler runs on its own unref'd
        // interval in production. Tests drive it synchronously.
        _preconvert: preconvert,
        _preconvertTick: preconvertTick,
        _preconvertEnqueueFromCatalog: () => preconvertEnqueueFromCatalog(true),
        _preconvertReconcile: preconvertReconcile,
        _preconvertPersistNow: preconvertPersistNow,
        _preconvertRecordFailure: preconvertRecordFailure,
        _preconvertLedgerBlocks: (quarkid) => preconvertLedgerBlocks(quarkid, Date.now()),
        _preconvertPickEligible: () => preconvertPickEligible(Date.now()),
        // Test-only: force a tracked job through the real kill path (kills the
        // ggpo pull + runner, marks the job failed) so a mid-watch VPS/job death
        // can be exercised deterministically. Not reachable over the wire.
        _killJob(quarkid, msg) {
            const job = jobs.get(quarkid);
            if (job && !job.finished) {
                failJob(job, msg || 'killed (test)');
                return true;
            }
            return false;
        },
    };
}

function validateConvertRequest(req) {
    if (typeof req.quarkid !== 'string' || !QUARKID_RE.test(req.quarkid)) {
        return 'quarkid must be a non-empty string matching [A-Za-z0-9_-]{1,128}';
    }
    return null;
}

function validateWatchRequest(req) {
    if (typeof req.quarkid !== 'string' || !QUARKID_RE.test(req.quarkid)) {
        return 'quarkid must be a non-empty string matching [A-Za-z0-9_-]{1,128}';
    }
    if (req.from !== undefined && (!Number.isInteger(req.from) || req.from < 0)) {
        return 'from must be a non-negative integer';
    }
    if (req.game_index !== undefined && (!Number.isInteger(req.game_index) || req.game_index < 0)) {
        return 'game_index must be a non-negative integer';
    }
    if (req.cks_from !== undefined && (!Number.isInteger(req.cks_from) || req.cks_from < 0)) {
        return 'cks_from must be a non-negative integer';
    }
    return null;
}

async function dispatch(req, ctx) {
    if (typeof req !== 'object' || req === null || Array.isArray(req)) {
        return { ok: false, error: 'bad_request', detail: 'request must be a JSON object' };
    }
    if (req.op === 'search') {
        return handleSearch(req, ctx.cache, ctx.enqueueUpstream);
    }
    if (req.op === 'status') {
        return handleStatus(ctx.cache, ctx.startedAt, ctx.convert);
    }
    if (req.op === 'get3sr') {
        return handleGet3sr(req);
    }
    if (req.op === 'convert') {
        const err = validateConvertRequest(req);
        if (err) return { ok: false, error: 'bad_request', detail: err };
        return ctx.convert.requestConvert(req.quarkid);
    }
    if (req.op === 'convertstatus') {
        const err = validateConvertRequest(req);
        if (err) return { ok: false, error: 'bad_request', detail: err };
        return ctx.convert.requestStatus(req.quarkid);
    }
    if (req.op === 'watchpoll') {
        const err = validateWatchRequest(req);
        if (err) return { ok: false, error: 'bad_request', detail: err };
        return ctx.convert.requestWatch(req.quarkid, req.from, req.game_index, req.cks_from);
    }
    // --- Mac-worker ops (plan-preconvert S4): token-gated, mutating -----------
    // The token check comes FIRST for every one of these — before any argument
    // validation — so an unauthorized caller learns nothing about the op's
    // shape, and a missing/wrong token (or no server token) is always rejected.
    if (req.op === 'worklease') {
        if (!workTokenOk(req.token)) return { ok: false, error: 'unauthorized' };
        if (typeof req.worker !== 'string' || !WORKER_NAME_RE.test(req.worker)) {
            return { ok: false, error: 'bad_request', detail: 'worker must match [A-Za-z0-9_.-]{1,64}' };
        }
        if (req.count !== undefined && (!Number.isInteger(req.count) || req.count < 1)) {
            return { ok: false, error: 'bad_request', detail: 'count must be a positive integer' };
        }
        return ctx.convert.requestWorkLease(req.worker, req.count);
    }
    if (req.op === 'workdone') {
        if (!workTokenOk(req.token)) return { ok: false, error: 'unauthorized' };
        if (typeof req.worker !== 'string' || !WORKER_NAME_RE.test(req.worker)) {
            return { ok: false, error: 'bad_request', detail: 'worker must match [A-Za-z0-9_.-]{1,64}' };
        }
        if (typeof req.quarkid !== 'string' || !QUARKID_RE.test(req.quarkid)) {
            return { ok: false, error: 'bad_request', detail: 'quarkid must match [A-Za-z0-9_-]{1,128}' };
        }
        if (typeof req.ok !== 'boolean') {
            return { ok: false, error: 'bad_request', detail: 'ok must be a boolean' };
        }
        return ctx.convert.requestWorkDone(req.worker, req.quarkid, req.ok, req.reason);
    }
    if (req.op === 'workstats') {
        if (!workTokenOk(req.token)) return { ok: false, error: 'unauthorized' };
        return ctx.convert.requestWorkStats();
    }
    return { ok: false, error: 'bad_request', detail: `unknown op: ${JSON.stringify(req.op)}` };
}

// --- Server lifecycle ----------------------------------------------------

function start(port) {
    const startedAt = Date.now();
    const cache = makeCache();
    const enqueueUpstream = makeUpstreamQueue();
    const convert = makeConvertManager();
    const ctx = { cache, enqueueUpstream, startedAt, convert };

    const server = net.createServer((socket) => {
        socket.setTimeout(CONN_IDLE_TIMEOUT_MS);
        let buf = Buffer.alloc(0);
        let destroyed = false;

        function safeWrite(obj) {
            if (destroyed) return;
            try {
                socket.write(encodeFrame(obj));
            } catch (err) {
                logWarn(`write failed: ${err && err.message ? err.message : err}`);
            }
        }

        socket.on('timeout', () => {
            logInfo(`connection idle timeout, closing (${socket.remoteAddress}:${socket.remotePort})`);
            socket.destroy();
        });

        socket.on('data', (chunk) => {
            buf = Buffer.concat([buf, chunk]);
            // Process as many complete frames as are buffered.
            for (;;) {
                if (buf.length < 4) return;
                const len = buf.readUInt32BE(0);
                if (len > MAX_FRAME_BYTES) {
                    safeWrite({ ok: false, error: 'bad_request', detail: `frame length ${len} exceeds max ${MAX_FRAME_BYTES}` });
                    destroyed = true;
                    socket.destroy();
                    return;
                }
                if (buf.length < 4 + len) return; // wait for more data
                const payload = buf.subarray(4, 4 + len);
                buf = buf.subarray(4 + len);

                let req;
                try {
                    req = JSON.parse(payload.toString('utf8'));
                } catch (err) {
                    safeWrite({ ok: false, error: 'bad_request', detail: 'invalid JSON frame' });
                    continue;
                }
                // Serialize responses per connection: chain each dispatch off
                // the previous one so replies always go out in request order,
                // even if a fast op (status) completes while a slow one
                // (search, rate-limit-queued) is still pending. Without this,
                // pipelined clients would receive reordered, uncorrelatable
                // replies (review finding, 2026-07-22).
                ctx.responseChain = (ctx.responseChain || Promise.resolve())
                    .then(() => dispatch(req, ctx))
                    .then((resp) => safeWrite(resp))
                    .catch((err) => {
                        logWarn(`dispatch error: ${err && err.stack ? err.stack : err}`);
                        safeWrite({ ok: false, error: 'upstream_error', detail: 'internal error' });
                    });
            }
        });

        socket.on('error', (err) => {
            logWarn(`connection error: ${err.message}`);
        });

        socket.on('close', () => {
            destroyed = true;
        });
    });

    server.on('error', (err) => {
        logWarn(`server error: ${err.message}`);
    });

    server.listen(port, () => {
        const addr = server.address();
        logInfo(`fcade-proxy listening on ${typeof addr === 'string' ? addr : `${addr.address}:${addr.port}`}`);
    });

    let shuttingDown = false;
    function shutdown(reason) {
        if (shuttingDown) return;
        shuttingDown = true;
        logInfo(`shutting down (${reason})`);
        try {
            server.close(() => process.exit(0));
        } catch (_) {
            process.exit(0);
        }
        setTimeout(() => process.exit(0), 1000).unref();
    }

    process.on('SIGTERM', () => shutdown('SIGTERM'));
    process.on('SIGINT', () => shutdown('SIGINT'));

    return {
        server,
        _cache: cache,
        _mockState: mockState,
        _convert: convert,
        _shutdown: shutdown,
    };
}

// --- CLI entrypoint ------------------------------------------------------

if (require.main === module) {
    start(DEFAULT_PORT);
}

module.exports = {
    start,
    normalizeRow,
    canonicalizeSearchRequest,
    cacheKeyFor,
    ttlForRequest,
    validateSearchRequest,
    loadCatalog,
    searchCatalog,
    validateGet3srRequest,
    handleGet3sr,
    validateConvertRequest,
    validateWatchRequest,
    makeConvertManager,
};
