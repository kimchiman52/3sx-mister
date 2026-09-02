// Self-contained test for the fcade-proxy pre-convert SCHEDULER
// (docs/plan-preconvert-fleet.md Stage S2): the persistent priority queue +
// catalog enqueuer + failure ledger + 30 s scheduler tick. Proves:
//   A. enqueue dedup (store-servable / already-queued / ledgered are skipped;
//      wrong gameid ignored) + tier ordering (P1 catalog_best, then P2 fresh
//      newest-first) + P3 demotion of a queued quark that left the catalog;
//   A2. priority-flip ordering (docs/plan-bounded-pool-replay.md Stage 1):
//      an older-dated best row is picked before a newer-dated fresh row, and
//      a P3 demoted row is picked last;
//   C. the pacing gap is honored (a fresh eligible item is NOT started until
//      nextBackgroundAllowedAt has passed);
//   F. the no_savestate ledger policy (1 retry after 24 h, then permanent) AND
//      the REAL downloader-exit path sets the typed failReason and ledgers it;
//   G. a live convert clears a prior ledger entry;
//   B. restart persistence (persist → re-require → queue + ledger reload);
//   D. the disk floor pauses background work (fresh module, huge floor);
//   H. P3 queue hygiene (docs/plan-bounded-pool-replay.md §6.1 Stage 4): a
//      demoted item past FCADE_PRECONVERT_P3_MAX_AGE_MS is dropped on the same
//      enqueue pass that demotes it, a fresh-dated one survives, and a leased
//      item is exempt from age-out; the FCADE_PRECONVERT_QUEUE_MAX hard cap
//      evicts oldest-date P3 first, never P1/P2 or a leased P3, and warns
//      (keeping everything) when P1+P2 alone exceed the cap.
//
// Same mock-process harness as __test_hardening.js: the convert manager is
// driven directly and the ggpo downloader + FBNeo runner are tiny real child
// processes keyed by the quarkid embedded in their path args.
//
// Runtime budget: a few seconds.

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

// --- scratch dirs + env (BEFORE require: constants read once at require time) -
const ROOT = fs.mkdtempSync(path.join(os.tmpdir(), 'fcade-preconv-test-'));
const SCRATCH = path.join(ROOT, 'convert-scratch');
const STORE = path.join(ROOT, '3sr');
const RUNNER_DIR = path.join(ROOT, 'runner');
const CATALOG = path.join(ROOT, 'catalog.json');
const STATE_FILE = path.join(ROOT, 'preconvert-state.json');
fs.mkdirSync(SCRATCH, { recursive: true });
fs.mkdirSync(STORE, { recursive: true });
fs.mkdirSync(path.join(RUNNER_DIR, 'roms'), { recursive: true });

const MOCK_DOWNLOADER = path.join(ROOT, 'mock-downloader.js');
fs.writeFileSync(
    MOCK_DOWNLOADER,
    `'use strict';
const fs = require('fs');
const path = require('path');
function arg(name){ const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i+1] : null; }
const outDir = arg('--out-dir');
if (outDir.indexOf('nosave') >= 0) {
    setTimeout(() => process.exit(0), 50); // NO savestate → job fails no_savestate
} else if (outDir.indexOf('complete') >= 0) {
    fs.writeFileSync(path.join(outDir, 'savestate'), Buffer.alloc(4096, 0x5a));
    fs.writeFileSync(path.join(outDir, 'inputs'), Buffer.alloc(300 * 10, 1));
    setTimeout(() => process.exit(0), 60); // let the job finalize into the store
} else {
    fs.writeFileSync(path.join(outDir, 'savestate'), Buffer.alloc(4096, 0x5a));
    const inputsPath = path.join(outDir, 'inputs');
    fs.writeFileSync(inputsPath, Buffer.alloc(300 * 10, 1));
    setInterval(() => { try { fs.appendFileSync(inputsPath, Buffer.alloc(10, 2)); } catch (e) {} }, 40);
    setTimeout(() => process.exit(0), 60000); // long-lived
}
`,
);

const MOCK_RUNNER = path.join(ROOT, 'mock-runner.js');
fs.writeFileSync(
    MOCK_RUNNER,
    `#!${process.execPath}
'use strict';
const fs = require('fs');
const path = require('path');
function arg(name){ const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i+1] : null; }
const trackDir = arg('-track-3sr');
fs.mkdirSync(trackDir, { recursive: true });
const buf = Buffer.alloc(64); buf.write('3SR1', 0, 'ascii');
fs.writeFileSync(path.join(trackDir, 'game_0.3sr'), buf);
if (trackDir.indexOf('complete') >= 0) {
    fs.writeFileSync(
        path.join(trackDir, 'track3sr_manifest.json'),
        JSON.stringify({ complete: true, games: [{ game_index: 0, signature_found: true, frame_count: 100, checksum_count: 2 }] }),
    );
    process.exit(0);
} else {
    setInterval(() => {}, 1000);
}
`,
);
fs.chmodSync(MOCK_RUNNER, 0o755);

process.env.FCADE_CATALOG_FILE = CATALOG;
process.env.FCADE_3SR_DIR = STORE;
process.env.FCADE_CONVERT_SCRATCH_DIR = SCRATCH;
process.env.FCADE_CONVERT_DOWNLOADER = MOCK_DOWNLOADER;
process.env.FCADE_CONVERT_PYTHON = process.execPath;
process.env.FCADE_CONVERT_RUNNER_BIN = MOCK_RUNNER;
process.env.FCADE_CONVERT_RUNNER_DIR = RUNNER_DIR;
process.env.FCADE_CONVERT_MAX_JOBS = '1';
process.env.FCADE_CONVERT_FOLLOW_IDLE_MS = '600000';
process.env.FCADE_CONVERT_SWEEP_INTERVAL_MS = '600000'; // maintenance sweeper off
process.env.FCADE_PRECONVERT_TICK_MS = '600000'; // scheduler auto-tick off; we drive it
process.env.FCADE_PRECONVERT_STATE_FILE = STATE_FILE;
process.env.FCADE_PRECONVERT_GAP_MS = '10000'; // 10 s pacing gap (measurable without sleeping)
process.env.FCADE_PRECONVERT_JITTER_MS = '0'; // deterministic pacing
process.env.FCADE_PRECONVERT_SAVE_DEBOUNCE_MS = '50';
process.env.FCADE_DISK_FLOOR_BYTES = '0'; // never disk-blocked (except the dedicated floor test)
process.env.FCADE_STORE_MAX_QUARKS = '100000'; // never churn-guarded here
process.env.FCADE_STORE_MAX_BYTES = String(64 * 1024 * 1024 * 1024);

let mod = require('./fcade-proxy.js');

// --- assertion plumbing ------------------------------------------------------
let failed = 0;
function assert(cond, msg) {
    if (!cond) {
        console.error(`ASSERT FAIL: ${msg}`);
        failed += 1;
    } else {
        console.log(`ok - ${msg}`);
    }
}
function assertEq(a, b, msg) {
    assert(a === b, `${msg} (actual=${JSON.stringify(a)} expected=${JSON.stringify(b)})`);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// --- fixtures ----------------------------------------------------------------
function row(quarkid, extra) {
    return Object.assign(
        { quarkid, gameid: 'sfiii3nr1', date: 100, duration: 100, players: [{ name: 'P1' }, { name: 'P2' }] },
        extra || {},
    );
}
function rewriteCatalog(rows) {
    fs.writeFileSync(CATALOG, JSON.stringify({ generated_at: Date.now(), rows }));
}
function writeStoreQuark(quarkid) {
    const dir = path.join(STORE, quarkid);
    fs.mkdirSync(dir, { recursive: true });
    const header = Buffer.alloc(28);
    header.write('3SR1', 0, 'ascii');
    fs.writeFileSync(path.join(dir, 'game_0.3sr'), header);
    fs.writeFileSync(path.join(dir, 'game_0.meta.json'), JSON.stringify({ players: [{ name: 'P1' }, { name: 'P2' }] }));
}
function cleanStore() {
    for (const ent of fs.readdirSync(STORE)) {
        try {
            fs.rmSync(path.join(STORE, ent), { recursive: true, force: true });
        } catch (_) {}
    }
}
function killAllJobs(cm) {
    for (const q of [...cm._jobs.keys()]) cm._killJob(q, 'test reset');
    cm._jobs.clear();
}
function resetPreconvert(cm) {
    cm._preconvert.queue = [];
    cm._preconvert.leases = {};
    cm._preconvert.failed = {};
    cm._preconvert.counters = {};
    cm._preconvert.nextBackgroundAllowedAt = 0;
    cm._preconvert.seenCatalogMtime = -1;
}
function freshTest(cm) {
    killAllJobs(cm);
    resetPreconvert(cm);
    cleanStore();
}
function queuedIds(cm) {
    return cm._preconvert.queue.map((it) => it.quarkid);
}
function futureLease(cm, quarkid) {
    cm._preconvert.leases[quarkid] = { worker: 'test', expires_at: Date.now() + 10 * 60 * 1000 };
}

// --- A: enqueue dedup + tier ordering + P3 demotion --------------------------
async function testEnqueueDedupAndTiers(cm) {
    freshTest(cm);
    const SERVABLE = '1700000000000-servable';
    const LEDGERED = '1700000000000-ledgered';
    const FRESH_A = '1700000000000-freshA';
    const FRESH_B = '1700000000000-freshB';
    const BEST_C = '1700000000000-bestC';
    const WRONG = '1700000000000-wrongGame';

    writeStoreQuark(SERVABLE); // already store-servable ⇒ must NOT enqueue
    cm._preconvert.failed[LEDGERED] = { reason: 'no_savestate', attempts: 1, last_at: Date.now(), retry_after: Date.now() + 10 * 60 * 1000, permanent: false };

    rewriteCatalog([
        row(SERVABLE),
        row(LEDGERED),
        row(FRESH_A, { date: 100 }),
        row(FRESH_B, { date: 200 }),
        row(BEST_C, { date: 150, catalog_best: true }),
        row(WRONG, { gameid: 'sfiii3other' }), // wrong gameid ⇒ ignored
    ]);

    cm._preconvertEnqueueFromCatalog();
    const ids = queuedIds(cm).sort();
    assertEq(JSON.stringify(ids), JSON.stringify([BEST_C, FRESH_A, FRESH_B].sort()), 'enqueue: only fresh non-servable non-ledgered rows queued (servable/ledgered/wrong-game skipped)');

    // Dedup: a second enqueue adds nothing.
    cm._preconvertEnqueueFromCatalog();
    assertEq(cm._preconvert.queue.length, 3, 'enqueue: re-enqueue is idempotent (no duplicates)');

    // Tiers (post-flip, docs/plan-bounded-pool-replay.md §4.2): P1 = catalog_best,
    // P2 = fresh newest-first. Lease the picked one to advance.
    let pick = cm._preconvertPickEligible();
    assertEq(pick && pick.quarkid, BEST_C, 'tier: P1 catalog_best first (bestC)');
    futureLease(cm, BEST_C);
    pick = cm._preconvertPickEligible();
    assertEq(pick && pick.quarkid, FRESH_B, 'tier: next P2 newest date first (freshB, date 200)');
    futureLease(cm, FRESH_B);
    pick = cm._preconvertPickEligible();
    assertEq(pick && pick.quarkid, FRESH_A, 'tier: P2 fresh drains newest-first (freshA, date 100)');
    futureLease(cm, FRESH_A);
    pick = cm._preconvertPickEligible();
    assertEq(pick, null, 'tier: nothing eligible once all are leased');

    // P3 demotion: a queued quark that has left the catalog is demoted to tier 3.
    // date is set to "just now" (not the old literal 50, i.e. 1970) so this stays a
    // pure demotion check and isn't also, incidentally, an age-out drop (§6.1 Stage 4,
    // covered on its own in testP3AgeOut below).
    cm._preconvert.queue.push({ quarkid: '1700000000000-gone', tier: 1, date: Date.now() - 1000, duration: 100, row: row('1700000000000-gone') });
    cm._preconvertEnqueueFromCatalog(); // catalog unchanged content ⇒ force re-run
    const goneItem = cm._preconvert.queue.find((it) => it.quarkid === '1700000000000-gone');
    assertEq(goneItem && goneItem.tier, 3, 'P3 demotion: a queued quark absent from the catalog is demoted to tier 3 (snapshot kept)');
    assert(goneItem && goneItem.row, 'P3 demotion: the row snapshot is retained for the demoted item');
}

// --- A2: priority-flip ordering (docs/plan-bounded-pool-replay.md Stage 1) ---
// best (older date) must be picked before fresh (newer date); P3 stays last;
// newest-first ordering still holds within a tier.
async function testPriorityFlipOrdering(cm) {
    freshTest(cm);
    const BEST_OLD = '1700000000000-bestOld';
    const FRESH_NEW = '1700000000000-freshNew';
    const DEMOTED = '1700000000000-demoted';

    rewriteCatalog([row(BEST_OLD, { date: 50, catalog_best: true }), row(FRESH_NEW, { date: 500 })]);
    cm._preconvertEnqueueFromCatalog();
    // Demote a queued quark that has left the catalog (P3), directly, as the
    // demotion path itself is already covered by testEnqueueDedupAndTiers.
    cm._preconvert.queue.push({ quarkid: DEMOTED, tier: 3, date: 999, duration: 100, row: row(DEMOTED) });

    let pick = cm._preconvertPickEligible();
    assertEq(pick && pick.quarkid, BEST_OLD, 'priority-flip: catalog_best (older date) picked before fresh (newer date)');
    futureLease(cm, BEST_OLD);

    pick = cm._preconvertPickEligible();
    assertEq(pick && pick.quarkid, FRESH_NEW, 'priority-flip: fresh (P2) picked once best (P1) is drained');
    futureLease(cm, FRESH_NEW);

    pick = cm._preconvertPickEligible();
    assertEq(pick && pick.quarkid, DEMOTED, 'priority-flip: P3 demoted item picked last, once P1+P2 are drained');
}

// --- H: P3 age-out (docs/plan-bounded-pool-replay.md §6.1 Stage 4) -----------
// `it.date` is the match's own ms-epoch timestamp (same units as the
// quarkid's numeric prefix, e.g. real quarkids `1784908283814-3519` /
// `1700000000000-0001` in the mock rows above -- 13-digit ms-since-epoch), so
// FCADE_PRECONVERT_P3_MAX_AGE_MS (48 h default here, unset ⇒ default) compares
// directly against Date.now(). Both quarks below are absent from the catalog
// throughout, so the same enqueue pass that demotes them (tier → 3) also runs
// the age-out check on them.
async function testP3AgeOut(cm) {
    freshTest(cm);
    rewriteCatalog([]);
    const STALE = '1700000000000-staleP3';
    const RECENT = '1700000000000-recentP3';
    const staleDate = Date.now() - 49 * 60 * 60 * 1000; // 49h old, past the 48h default
    const recentDate = Date.now() - 1000; // 1s old, well inside the 48h window
    cm._preconvert.queue.push({ quarkid: STALE, tier: 1, date: staleDate, duration: 100, row: row(STALE) });
    cm._preconvert.queue.push({ quarkid: RECENT, tier: 1, date: recentDate, duration: 100, row: row(RECENT) });

    cm._preconvertEnqueueFromCatalog();

    assert(!cm._preconvert.queue.some((it) => it.quarkid === STALE), 'P3 age-out: demoted item past FCADE_PRECONVERT_P3_MAX_AGE_MS (48h default) is dropped on the same enqueue pass');
    const survivor = cm._preconvert.queue.find((it) => it.quarkid === RECENT);
    assert(!!survivor, 'P3 age-out: demoted item within the 48h window survives');
    assertEq(survivor && survivor.tier, 3, 'P3 age-out: the survivor was still correctly demoted to tier 3');
}

// A leased item is never age-out-dropped, even when its date is far past the
// max age -- a Mac worker mid-convert on it must not have the item vanish
// out from under the lease (workdone/failJob would then have nothing to
// remove/free against on completion).
async function testP3LeasedNotAgedOut(cm) {
    freshTest(cm);
    rewriteCatalog([]);
    const LEASED_STALE = '1700000000000-leasedStaleP3';
    const staleDate = Date.now() - 72 * 60 * 60 * 1000; // 72h old, well past the 48h default
    cm._preconvert.queue.push({ quarkid: LEASED_STALE, tier: 1, date: staleDate, duration: 100, row: row(LEASED_STALE) });
    futureLease(cm, LEASED_STALE);

    cm._preconvertEnqueueFromCatalog();

    assert(cm._preconvert.queue.some((it) => it.quarkid === LEASED_STALE), 'P3 age-out: a leased item is never dropped, even past the max age');
}

// --- H: P3 hard queue cap (docs/plan-bounded-pool-replay.md §6.1 Stage 4) ----
// Constants are read once at require time, so a small FCADE_PRECONVERT_QUEUE_MAX
// needs its own fresh module instance (same pattern as testDiskFloorPause).
async function testQueueHardCap() {
    delete require.cache[require.resolve('./fcade-proxy.js')];
    process.env.FCADE_PRECONVERT_QUEUE_MAX = '3';
    const modCap = require('./fcade-proxy.js');
    const cmCap = modCap.makeConvertManager();

    // (c) over-cap queue evicts oldest-date P3 only; P1/P2 untouched.
    freshTest(cmCap);
    const BEST = '1700000000000-capBest';
    const FRESH = '1700000000000-capFresh';
    rewriteCatalog([row(BEST, { catalog_best: true, date: 10 }), row(FRESH, { date: 20 })]); // 1 P1 + 1 P2
    const p3ids = [];
    for (let i = 0; i < 4; i++) {
        const id = `1700000000000-capP3-${i}`;
        p3ids.push(id);
        // ascending dates: capP3-0 oldest ... capP3-3 newest
        cmCap._preconvert.queue.push({ quarkid: id, tier: 3, date: Date.now() - (4 - i) * 1000, duration: 100, row: row(id) });
    }
    cmCap._preconvertEnqueueFromCatalog(); // adds BEST+FRESH -> 2 + 4 = 6 items, over the cap of 3
    assertEq(cmCap._preconvert.queue.length, 3, 'queue-cap: queue trimmed down to FCADE_PRECONVERT_QUEUE_MAX (3)');
    assert(cmCap._preconvert.queue.some((it) => it.quarkid === BEST), 'queue-cap: P1 item never dropped');
    assert(cmCap._preconvert.queue.some((it) => it.quarkid === FRESH), 'queue-cap: P2 item never dropped');
    assert(!cmCap._preconvert.queue.some((it) => it.quarkid === p3ids[0]), 'queue-cap: oldest-date P3 evicted first');
    assert(!cmCap._preconvert.queue.some((it) => it.quarkid === p3ids[1]), 'queue-cap: 2nd-oldest-date P3 evicted next');
    assert(cmCap._preconvert.queue.some((it) => it.quarkid === p3ids[3]), 'queue-cap: newest P3 item survives');

    // (d) P1/P2-only queue over cap -> nothing dropped, warn instead.
    freshTest(cmCap);
    const B1 = '1700000000000-warnBest1';
    const B2 = '1700000000000-warnBest2';
    const F1 = '1700000000000-warnFresh1';
    const F2 = '1700000000000-warnFresh2';
    rewriteCatalog([
        row(B1, { catalog_best: true, date: 10 }),
        row(B2, { catalog_best: true, date: 20 }),
        row(F1, { date: 30 }),
        row(F2, { date: 40 }),
    ]); // 4 P1/P2 rows, cap is 3 -> no P3 candidates to evict
    let warned = '';
    const prevWarn = console.warn;
    console.warn = (msg) => {
        warned += String(msg);
    };
    cmCap._preconvertEnqueueFromCatalog();
    console.warn = prevWarn;
    assertEq(cmCap._preconvert.queue.length, 4, 'queue-cap: P1+P2 alone over the cap (3) -- nothing dropped');
    for (const id of [B1, B2, F1, F2]) {
        assert(cmCap._preconvert.queue.some((it) => it.quarkid === id), `queue-cap: ${id} kept despite the queue exceeding the cap`);
    }
    assert(warned.includes('no evictable P3'), 'queue-cap: a warn fires when P1/P2 alone exceed the cap');

    // (e) a leased P3 item is never evicted under cap pressure either, even
    // when it is the globally-oldest (and therefore first-picked by a naive
    // oldest-first sort) candidate.
    freshTest(cmCap);
    const LBEST = '1700000000000-leaseCapBest';
    rewriteCatalog([row(LBEST, { catalog_best: true, date: 10 })]); // 1 P1 row
    const LEASED = '1700000000000-leaseCapLeased';
    const UNLEASED = '1700000000000-leaseCapUnleased';
    cmCap._preconvert.queue.push({ quarkid: LEASED, tier: 3, date: Date.now() - 10000, duration: 100, row: row(LEASED) }); // oldest
    cmCap._preconvert.queue.push({ quarkid: UNLEASED, tier: 3, date: Date.now() - 5000, duration: 100, row: row(UNLEASED) }); // newer
    futureLease(cmCap, LEASED);
    cmCap._preconvertEnqueueFromCatalog(); // adds LBEST -> 1 + 2 = 3 items, at the cap of 3... need one more to force eviction
    // Bump the cap pressure: push one more unleased P3 so the queue is over cap.
    const UNLEASED2 = '1700000000000-leaseCapUnleased2';
    cmCap._preconvert.queue.push({ quarkid: UNLEASED2, tier: 3, date: Date.now() - 7000, duration: 100, row: row(UNLEASED2) });
    cmCap._preconvertEnqueueFromCatalog();
    assertEq(cmCap._preconvert.queue.length, 3, 'queue-cap: trimmed back to cap (3) even with a leased P3 present');
    assert(cmCap._preconvert.queue.some((it) => it.quarkid === LBEST), 'queue-cap: P1 kept');
    assert(cmCap._preconvert.queue.some((it) => it.quarkid === LEASED), 'queue-cap: leased P3 never dropped, even though it is the oldest');
    assert(!cmCap._preconvert.queue.some((it) => it.quarkid === UNLEASED2), 'queue-cap: oldest UNLEASED (of the two unleased) dropped instead');
    assert(cmCap._preconvert.queue.some((it) => it.quarkid === UNLEASED), 'queue-cap: newer unleased P3 survives (only 1 needed to be dropped to reach cap)');

    killAllJobs(cmCap);
    delete process.env.FCADE_PRECONVERT_QUEUE_MAX; // restore: unset -> default (2500) for any later fresh-module test
}

// --- C: pacing gap honored ---------------------------------------------------
async function testPacingGap(cm) {
    freshTest(cm);
    const PACE = '1700000000000-pace1';
    rewriteCatalog([row(PACE)]);
    cm._preconvertEnqueueFromCatalog();
    assertEq(cm._preconvert.queue.length, 1, 'pacing: one eligible item queued');

    // Gap not yet elapsed ⇒ tick must NOT start background work.
    cm._preconvert.nextBackgroundAllowedAt = Date.now() + 100000;
    cm._preconvertTick();
    assert(!cm._jobs.has(PACE), 'pacing: item NOT started while the gap has not elapsed');
    assert(cm._preconvert.queue.some((it) => it.quarkid === PACE), 'pacing: the item stays queued (paced, not dropped)');

    // Gap elapsed ⇒ tick starts exactly one background job.
    cm._preconvert.nextBackgroundAllowedAt = 0;
    cm._preconvertTick();
    const job = cm._jobs.get(PACE);
    assert(!!job, 'pacing: item started once the gap has elapsed');
    assertEq(job && job.background, true, 'pacing: the started job is flagged background');
    killAllJobs(cm);
}

// --- F: no_savestate ledger policy + real downloader-exit path ---------------
async function testNoSavestateLedger(cm) {
    freshTest(cm);
    // Policy via the hook: 1 retry after ~24 h, then permanent.
    const POLICY = '1700000000000-policyP';
    cm._preconvertRecordFailure(POLICY, 'no_savestate');
    let e = cm._preconvert.failed[POLICY];
    assertEq(e.attempts, 1, 'no_savestate: first failure recorded (attempt 1)');
    assertEq(e.permanent, false, 'no_savestate: not permanent after the first failure');
    assert(e.retry_after > Date.now() + 20 * 60 * 60 * 1000, 'no_savestate: retry_after ~24 h out');
    assertEq(cm._preconvertLedgerBlocks(POLICY), true, 'no_savestate: ledger blocks (retry_after in the future)');
    cm._preconvertRecordFailure(POLICY, 'no_savestate');
    e = cm._preconvert.failed[POLICY];
    assertEq(e.attempts, 2, 'no_savestate: second failure recorded (attempt 2)');
    assertEq(e.permanent, true, 'no_savestate: PERMANENT after the second failure');
    assertEq(cm._preconvertLedgerBlocks(POLICY), true, 'no_savestate: ledger blocks permanently');

    // Real path: a background job whose download yields no savestate must set
    // job.failReason = 'no_savestate' in the downloader exit handler and ledger it.
    freshTest(cm);
    const NOSAVE = '1700000000000-nosaveQ';
    rewriteCatalog([row(NOSAVE)]);
    cm._preconvertEnqueueFromCatalog();
    cm._preconvert.nextBackgroundAllowedAt = 0;
    cm._preconvertTick();
    assert(cm._jobs.has(NOSAVE), 'no_savestate(real): background job started');
    const deadline = Date.now() + 6000;
    while (Date.now() < deadline && !cm._preconvert.failed[NOSAVE]) await sleep(30);
    e = cm._preconvert.failed[NOSAVE];
    assert(!!e, 'no_savestate(real): the failed download was recorded in the ledger');
    assertEq(e.reason, 'no_savestate', 'no_savestate(real): typed reason set by the downloader-exit path (not string-matched)');
    assert(!cm._preconvert.queue.some((it) => it.quarkid === NOSAVE), 'no_savestate(real): item removed from the queue on failure');
    // A re-enqueue must not re-add it (ledger blocks it while retry_after is future).
    cm._preconvertEnqueueFromCatalog();
    assert(!cm._preconvert.queue.some((it) => it.quarkid === NOSAVE), 'no_savestate(real): a re-appearing row is not re-enqueued while ledgered');
    killAllJobs(cm);
}

// --- G: a live convert clears a prior ledger entry ---------------------------
async function testLiveConvertClearsLedger(cm) {
    freshTest(cm);
    const CL = '1700000000000-completeL';
    cm._preconvert.failed[CL] = { reason: 'no_games', attempts: 1, last_at: Date.now(), retry_after: Date.now() + 10 * 60 * 1000, permanent: false };
    rewriteCatalog([row(CL)]);

    const r = cm.requestConvert(CL); // LIVE convert (no opts)
    assertEq(r.ok, true, 'live-clears-ledger: live convert accepted');
    const deadline = Date.now() + 6000;
    while (Date.now() < deadline && cm._preconvert.failed[CL]) await sleep(30);
    assert(!cm._preconvert.failed[CL], 'live-clears-ledger: a successful live convert cleared the ledger entry');
    assert(fs.existsSync(path.join(STORE, CL, 'game_0.3sr')), 'live-clears-ledger: the quark was actually converted into the store');
    killAllJobs(cm);
}

// --- B: restart persistence --------------------------------------------------
async function testRestartPersistence(cm) {
    freshTest(cm);
    const R1 = '1700000000000-restart1';
    const R2 = '1700000000000-restart2';
    const RL = '1700000000000-restartLedger';
    rewriteCatalog([row(R1), row(R2)]);
    cm._preconvertEnqueueFromCatalog();
    cm._preconvertRecordFailure(RL, 'no_savestate');
    const before = queuedIds(cm).sort();
    cm._preconvertPersistNow();
    assert(fs.existsSync(STATE_FILE), 'restart: state file written');

    // Re-require the module fresh so a brand-new manager loads from disk.
    delete require.cache[require.resolve('./fcade-proxy.js')];
    const mod2 = require('./fcade-proxy.js');
    const cm2 = mod2.makeConvertManager();
    const after = cm2._preconvert.queue.map((it) => it.quarkid).sort();
    assertEq(JSON.stringify(after), JSON.stringify(before), 'restart: pre-convert queue reloaded across a restart');
    assert(!!cm2._preconvert.failed[RL], 'restart: failure ledger reloaded across a restart');
    killAllJobs(cm2);
}

// --- D: disk floor pauses background work ------------------------------------
async function testDiskFloorPause() {
    // Fresh module with a floor above any real free space (constants read once).
    delete require.cache[require.resolve('./fcade-proxy.js')];
    process.env.FCADE_DISK_FLOOR_BYTES = String(2 ** 60); // ~1.15e18 bytes
    const mod3 = require('./fcade-proxy.js');
    const cm3 = mod3.makeConvertManager();
    freshTest(cm3);
    const DISK = '1700000000000-disk1';
    rewriteCatalog([row(DISK)]);
    cm3._preconvertEnqueueFromCatalog();
    cm3._preconvert.nextBackgroundAllowedAt = 0;
    assertEq(cm3._preconvert.queue.length, 1, 'disk-floor: item enqueued');
    cm3._preconvertTick();
    assert(!cm3._jobs.has(DISK), 'disk-floor: below the floor, the scheduler did NOT start background work');
    assert(cm3._preconvert.queue.some((it) => it.quarkid === DISK), 'disk-floor: the item remains queued (paused, not dropped)');
    killAllJobs(cm3);
    process.env.FCADE_DISK_FLOOR_BYTES = '0';
}

// --- main --------------------------------------------------------------------
async function main() {
    console.warn = () => {};
    let exitCode = 0;
    const cm = mod.makeConvertManager();
    try {
        await testEnqueueDedupAndTiers(cm);
        await testPriorityFlipOrdering(cm);
        await testP3AgeOut(cm);
        await testP3LeasedNotAgedOut(cm);
        await testQueueHardCap();
        await testPacingGap(cm);
        await testNoSavestateLedger(cm);
        await testLiveConvertClearsLedger(cm);
        await testRestartPersistence(cm);
        await testDiskFloorPause();
        killAllJobs(cm);
    } catch (err) {
        console.error(`UNCAUGHT: ${err && err.stack ? err.stack : err}`);
        exitCode = 1;
    }
    if (failed > 0) {
        console.error(`${failed} assertion(s) failed`);
        exitCode = 1;
    }
    try {
        fs.rmSync(ROOT, { recursive: true, force: true });
    } catch (_) {}
    if (exitCode === 0) console.log('preconvert test passed');
    setTimeout(() => process.exit(exitCode), 50).unref();
}

main();
