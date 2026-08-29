// Self-contained test for the fcade-proxy store-eviction CADENCE decoupling
// (docs/plan-preconvert-fleet.md Stage S3): store eviction no longer rides the
// 15 s maintenance sweep. It proves:
//   1. an over-cap store is NOT trimmed by the 15 s maintenance sweep (_sweepOnce);
//   2. it IS trimmed by the dedicated slow-timer path (_evictStore);
//   3. a real publish (a completed convert) still triggers eviction (tryFinalize
//      → evictStore), so a fresh conversion stays bounded promptly.
// The existing LRU-by-last-served eviction tests (__test_hardening.js) still
// exercise the policy itself and pass unchanged (updated for low-water --
// see below).
//
// Also covers Stage 3 of docs/plan-bounded-pool-replay.md §6.2 (pinning +
// low-water):
//   4. catalog-pinned quarks are skipped by LRU eviction even when they are
//      the oldest-served (would otherwise be evicted first);
//   5. eviction goes down to the LOW-WATER target below cap, not exactly to
//      the cap (so the 90% churn guard, fcade-proxy.js ~:2429, never latches
//      permanently against an at-cap store);
//   6. when pinned mass alone exceeds a cap, nothing pinned is evicted and
//      the "still over cap" warn tripwire fires.
// Tests 1-3 above (`testSweepDecoupled` / `testPublishTriggersEviction`) were
// written when eviction stopped exactly at cap; their survivor-count
// assertions are updated in place for the low-water target (noted inline).
//
// Review-fix amendments to §6.2 Stage 3 (2026-07-28):
//   7. a catalog that is CONFIGURED but currently missing/unreadable/corrupt
//      makes the whole eviction pass fail OPEN (skip entirely, warn) rather
//      than fail closed to an empty pinned set;
//   8. eviction now triggers at/above the shared 0.9 churn-guard fraction
//      (not just strictly over the raw cap), closing the dead zone where the
//      guard could latch while eviction stayed dormant; also exercises the
//      FCADE_STORE_EVICT_LOW_WATER='' env-hygiene default.
//
// Runtime budget: a couple of seconds.

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

const ROOT = fs.mkdtempSync(path.join(os.tmpdir(), 'fcade-evict-test-'));
const SCRATCH = path.join(ROOT, 'convert-scratch');
const STORE = path.join(ROOT, '3sr');
const RUNNER_DIR = path.join(ROOT, 'runner');
const CATALOG = path.join(ROOT, 'catalog.json');
fs.mkdirSync(SCRATCH, { recursive: true });
fs.mkdirSync(STORE, { recursive: true });
fs.mkdirSync(path.join(RUNNER_DIR, 'roms'), { recursive: true });

// Mock downloader/runner that finalize a "complete" quark into the store.
const MOCK_DOWNLOADER = path.join(ROOT, 'mock-downloader.js');
fs.writeFileSync(
    MOCK_DOWNLOADER,
    `'use strict';
const fs = require('fs'); const path = require('path');
function arg(n){ const i = process.argv.indexOf(n); return i>=0?process.argv[i+1]:null; }
const outDir = arg('--out-dir');
fs.writeFileSync(path.join(outDir, 'savestate'), Buffer.alloc(4096, 0x5a));
fs.writeFileSync(path.join(outDir, 'inputs'), Buffer.alloc(300 * 10, 1));
setTimeout(() => process.exit(0), 60);
`,
);
const MOCK_RUNNER = path.join(ROOT, 'mock-runner.js');
fs.writeFileSync(
    MOCK_RUNNER,
    `#!${process.execPath}
'use strict';
const fs = require('fs'); const path = require('path');
function arg(n){ const i = process.argv.indexOf(n); return i>=0?process.argv[i+1]:null; }
const trackDir = arg('-track-3sr');
fs.mkdirSync(trackDir, { recursive: true });
const buf = Buffer.alloc(64); buf.write('3SR1', 0, 'ascii');
fs.writeFileSync(path.join(trackDir, 'game_0.3sr'), buf);
fs.writeFileSync(path.join(trackDir, 'track3sr_manifest.json'), JSON.stringify({ complete: true, games: [{ game_index: 0, signature_found: true, frame_count: 100, checksum_count: 2 }] }));
process.exit(0);
`,
);
fs.chmodSync(MOCK_RUNNER, 0o755);

const COMPLETE = '1700000000000-completeE';
fs.writeFileSync(CATALOG, JSON.stringify({ generated_at: 1, rows: [{ quarkid: COMPLETE, gameid: 'sfiii3nr1', date: 1, duration: 100, players: [{ name: 'P1' }, { name: 'P2' }] }] }));

process.env.FCADE_CATALOG_FILE = CATALOG;
process.env.FCADE_3SR_DIR = STORE;
process.env.FCADE_CONVERT_SCRATCH_DIR = SCRATCH;
process.env.FCADE_CONVERT_DOWNLOADER = MOCK_DOWNLOADER;
process.env.FCADE_CONVERT_PYTHON = process.execPath;
process.env.FCADE_CONVERT_RUNNER_BIN = MOCK_RUNNER;
process.env.FCADE_CONVERT_RUNNER_DIR = RUNNER_DIR;
process.env.FCADE_CONVERT_MAX_JOBS = '1';
process.env.FCADE_CONVERT_FOLLOW_IDLE_MS = '600000';
process.env.FCADE_CONVERT_SWEEP_INTERVAL_MS = '600000';
process.env.FCADE_PRECONVERT_ENABLED = '0'; // scheduler dark; this test is store-only
process.env.FCADE_PRECONVERT_TICK_MS = '600000';
process.env.FCADE_STORE_MAX_QUARKS = '3';
process.env.FCADE_STORE_MAX_BYTES = String(64 * 1024 * 1024 * 1024); // count cap dominates
process.env.FCADE_STORE_EVICT_SWEEP_MS = '600000'; // slow timer off; we call _evictStore

const { makeConvertManager } = require('./fcade-proxy.js');

let failed = 0;
function assert(cond, msg) {
    if (!cond) { console.error(`ASSERT FAIL: ${msg}`); failed += 1; } else { console.log(`ok - ${msg}`); }
}
function assertEq(a, b, msg) { assert(a === b, `${msg} (actual=${JSON.stringify(a)} expected=${JSON.stringify(b)})`); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function writeStoreQuark(quarkid, servedMs) {
    const dir = path.join(STORE, quarkid);
    fs.mkdirSync(dir, { recursive: true });
    const header = Buffer.alloc(28); header.write('3SR1', 0, 'ascii');
    fs.writeFileSync(path.join(dir, 'game_0.3sr'), header);
    fs.writeFileSync(path.join(dir, 'game_0.meta.json'), JSON.stringify({ players: [{ name: 'P1' }, { name: 'P2' }] }));
    const t = servedMs / 1000;
    fs.utimesSync(path.join(dir, 'game_0.3sr'), t, t);
    fs.utimesSync(path.join(dir, 'game_0.meta.json'), t, t);
    fs.utimesSync(dir, t, t);
}
function storeDirs() {
    return fs.readdirSync(STORE).filter((n) => fs.statSync(path.join(STORE, n)).isDirectory());
}
function cleanStore() {
    for (const ent of fs.readdirSync(STORE)) { try { fs.rmSync(path.join(STORE, ent), { recursive: true, force: true }); } catch (_) {} }
}

// --- TEST 1+2: the 15 s sweep does NOT evict; the slow-timer path DOES --------
async function testSweepDecoupled(cm) {
    cleanStore();
    const base = Date.now() - 100000;
    const q = [];
    for (let i = 1; i <= 5; i++) { const id = `1700000000000-old${i}`; writeStoreQuark(id, base + i * 1000); q.push(id); }
    assertEq(storeDirs().length, 5, 'decoupled: 5 quarks in an over-cap (3) store');

    // The maintenance sweep must NOT touch the store anymore.
    cm._sweepOnce();
    assertEq(storeDirs().length, 5, 'decoupled: the 15 s maintenance sweep does NOT evict (eviction removed from it)');

    // The dedicated slow-timer path trims down to the LOW-WATER target below
    // cap (default 0.85 -> floor(0.85*3) = 2 survivors), LRU by last-served.
    // None of these quarks are catalog-pinned (CATALOG only lists COMPLETE),
    // so this is pure low-water behavior. Updated for
    // docs/plan-bounded-pool-replay.md §6.2 Stage 3: was evicted.length===2 /
    // storeDirs().length===3 (exactly-at-cap).
    const evicted = cm._evictStore();
    assertEq(evicted.length, 3, 'decoupled: the slow-timer eviction path trims to the low-water target (evicted 3)');
    assert(!fs.existsSync(path.join(STORE, q[0])), 'decoupled: oldest-served evicted by the slow-timer path');
    assert(!fs.existsSync(path.join(STORE, q[1])), 'decoupled: 2nd-oldest-served also evicted (low-water goes below cap)');
    assert(!fs.existsSync(path.join(STORE, q[2])), 'decoupled: 3rd-oldest-served also evicted (low-water target is 2 survivors)');
    assert(fs.existsSync(path.join(STORE, q[3])), 'decoupled: 2nd-newest survives');
    assert(fs.existsSync(path.join(STORE, q[4])), 'decoupled: newest-served survives');
    assertEq(storeDirs().length, 2, 'decoupled: store trimmed to the low-water target (below the cap of 3)');
}

// --- TEST 3: a real publish still triggers eviction --------------------------
async function testPublishTriggersEviction(cm) {
    cleanStore();
    const base = Date.now() - 100000;
    const olds = [];
    for (let i = 1; i <= 3; i++) { const id = `1700000000000-pub${i}`; writeStoreQuark(id, base + i * 1000); olds.push(id); }
    assertEq(storeDirs().length, 3, 'publish-evict: store starts full at the cap (3)');

    // A live convert that finalizes into the store → count 4 → tryFinalize's
    // post-publish evictStore trims down to the low-water target (2 survivors,
    // same floor(0.85*3) as above), evicting the two oldest old quarks. COMPLETE
    // is catalog-pinned (it's the CATALOG row from module setup, line ~59) but
    // that's moot here -- it's also the newest-served, so it would survive
    // either way. Updated for docs/plan-bounded-pool-replay.md §6.2 Stage 3:
    // was storeDirs().length===3 (exactly-at-cap, only olds[0] evicted).
    const r = cm.requestConvert(COMPLETE);
    assertEq(r.ok, true, 'publish-evict: convert accepted');
    const deadline = Date.now() + 8000;
    while (Date.now() < deadline && !fs.existsSync(path.join(STORE, COMPLETE, 'game_0.3sr'))) await sleep(30);
    assert(fs.existsSync(path.join(STORE, COMPLETE, 'game_0.3sr')), 'publish-evict: the new quark was published');
    // Give the synchronous post-publish evictStore a moment to have run.
    await sleep(50);
    assertEq(storeDirs().length, 2, 'publish-evict: publish triggered eviction down to the low-water target (no slow timer, no sweep)');
    assert(!fs.existsSync(path.join(STORE, olds[0])), 'publish-evict: the oldest-served pre-existing quark was evicted on publish');
    assert(!fs.existsSync(path.join(STORE, olds[1])), 'publish-evict: the 2nd-oldest pre-existing quark also evicted (low-water goes below cap)');
    assert(fs.existsSync(path.join(STORE, olds[2])), 'publish-evict: the newest pre-existing quark survives');
    assert(fs.existsSync(path.join(STORE, COMPLETE)), 'publish-evict: the freshly-published quark survives');
    for (const q of [...cm._jobs.keys()]) cm._killJob(q, 'cleanup');
}

// --- TEST 4: catalog-pinned quarks are skipped by LRU eviction, even when
//     they are the oldest-served (plain LRU would evict them first) ---------
async function testPinnedSurvivesLru(cm) {
    cleanStore();
    const base = Date.now() - 100000;
    const PINNED = '1700000000000-pinnedOld';
    writeStoreQuark(PINNED, base + 1000); // oldest-served of the batch
    const unpinned = [];
    for (let i = 2; i <= 5; i++) {
        const id = `1700000000000-unpinned${i}`;
        writeStoreQuark(id, base + i * 1000);
        unpinned.push(id);
    }
    assertEq(storeDirs().length, 5, 'pinned: 5 quarks in an over-cap (3) store, 1 pinned + 4 unpinned');

    // Only PINNED is in the catalog -- everything else is a store-only quark
    // that never surfaced in (or has aged out of) the current catalog.
    fs.writeFileSync(
        CATALOG,
        JSON.stringify({
            generated_at: Date.now(),
            rows: [{ quarkid: PINNED, gameid: 'sfiii3nr1', date: 1, duration: 100, players: [{ name: 'P1' }, { name: 'P2' }] }],
        }),
    );

    const evicted = cm._evictStoreIfNeeded();

    // Cap 3 -> low-water target floor(0.85*3) = 2 survivors. PINNED is
    // skipped even though it is the oldest-served (plain LRU would evict it
    // first); eviction instead removes the 3 oldest UNPINNED quarks, leaving
    // PINNED + the newest unpinned quark as the 2 survivors.
    assertEq(evicted.length, 3, 'pinned: 3 unpinned quarks evicted to reach the low-water target (PINNED never counted as evictable)');
    assert(fs.existsSync(path.join(STORE, PINNED)), 'pinned: catalog-member quark survives despite being the oldest-served');
    assert(!fs.existsSync(path.join(STORE, unpinned[0])), 'pinned: oldest unpinned quark evicted');
    assert(!fs.existsSync(path.join(STORE, unpinned[1])), 'pinned: 2nd-oldest unpinned quark evicted');
    assert(!fs.existsSync(path.join(STORE, unpinned[2])), 'pinned: 3rd-oldest unpinned quark evicted');
    assert(fs.existsSync(path.join(STORE, unpinned[3])), 'pinned: newest unpinned quark survives (low-water target reached)');
    assertEq(storeDirs().length, 2, 'pinned: store at the low-water target (1 pinned survivor + 1 unpinned survivor)');
}

// --- TEST 5: eviction goes down to the LOW-WATER target, not exactly-at-cap -
async function testLowWaterTarget(cm) {
    cleanStore();
    // No pins in play here -- isolate the low-water behavior on its own.
    fs.writeFileSync(CATALOG, JSON.stringify({ generated_at: Date.now(), rows: [] }));
    const base = Date.now() - 100000;
    const ids = [];
    for (let i = 1; i <= 5; i++) {
        const id = `1700000000000-lw${i}`;
        writeStoreQuark(id, base + i * 1000);
        ids.push(id);
    }
    const evicted = cm._evictStoreIfNeeded();
    const survivors = ids.filter((id) => fs.existsSync(path.join(STORE, id)));
    // Cap 3, low-water floor(0.85*3) = 2: eviction must NOT stop the instant
    // it's back at 3 survivors (exactly-at-cap) -- it continues down to 2.
    assertEq(survivors.length, 2, 'low-water: eviction stops at the low-water target (2 survivors), not exactly-at-cap (3)');
    assertEq(evicted.length, 3, 'low-water: 3 quarks evicted -- one more than the 2 needed to merely reach the cap');
    assert(survivors.includes(ids[3]) && survivors.includes(ids[4]), 'low-water: the 2 newest-served quarks survive');
}

// --- TEST 6: pinned mass alone exceeding a cap emits the warn tripwire and --
//     evicts nothing pinned ---------------------------------------------------
async function testPinnedOverCapWarn(cm) {
    cleanStore();
    const base = Date.now() - 100000;
    const ids = [];
    for (let i = 1; i <= 4; i++) {
        const id = `1700000000000-pinover${i}`;
        writeStoreQuark(id, base + i * 1000);
        ids.push(id);
    }
    // All 4 are catalog members -- pinned mass (4) alone exceeds the cap (3),
    // same as the "still over cap" case the pre-existing warn already covered
    // for active convert/watch jobs.
    fs.writeFileSync(
        CATALOG,
        JSON.stringify({
            generated_at: Date.now(),
            rows: ids.map((id, idx) => ({ quarkid: id, gameid: 'sfiii3nr1', date: idx, duration: 100, players: [{ name: 'P1' }, { name: 'P2' }] })),
        }),
    );

    let warned = '';
    const prevWarn = console.warn;
    console.warn = (msg) => { warned += String(msg); };
    const evicted = cm._evictStoreIfNeeded();
    console.warn = prevWarn;

    assertEq(evicted.length, 0, 'pinned-over-cap: nothing evicted -- all 4 quarks are catalog-pinned');
    assertEq(storeDirs().length, 4, 'pinned-over-cap: store still over the cap of 3 (pinned mass alone exceeds it)');
    assert(warned.includes('still over cap'), 'pinned-over-cap: the still-over-cap warn tripwire fired');
    for (const id of ids) assert(fs.existsSync(path.join(STORE, id)), `pinned-over-cap: ${id} survives (pinned)`);
}

// --- TEST 7 (review fix): catalog CONFIGURED but missing/corrupt -> fail
//     open. `loadCatalog()` returning null must NEVER be read as "no pins" --
//     that would let this pass evict a quark that IS in the (temporarily
//     unreadable) catalog. The whole eviction pass is skipped instead, with a
//     warn; the 10-min slow timer retries. -------------------------------
async function testCatalogConfiguredButMissingFailsOpen(cm) {
    cleanStore();
    const base = Date.now() - 100000;
    const ids = [];
    for (let i = 1; i <= 5; i++) {
        const id = `1700000000000-failopen${i}`;
        writeStoreQuark(id, base + i * 1000);
        ids.push(id);
    }
    assertEq(storeDirs().length, 5, 'fail-open: 5 quarks seeded in an over-cap (3) store');

    const prevCatalogFile = process.env.FCADE_CATALOG_FILE;
    process.env.FCADE_CATALOG_FILE = path.join(ROOT, 'catalog-does-not-exist.json'); // configured, but missing

    let warned = '';
    const prevWarn = console.warn;
    console.warn = (msg) => {
        warned += String(msg);
    };
    const evicted = cm._evictStoreIfNeeded();
    console.warn = prevWarn;
    process.env.FCADE_CATALOG_FILE = prevCatalogFile;

    assertEq(evicted.length, 0, 'fail-open: nothing evicted while the configured catalog is unreadable');
    assertEq(storeDirs().length, 5, 'fail-open: store untouched -- the whole pass was skipped, not just pin-empty');
    assert(warned.length > 0, 'fail-open: a warn was emitted for the skipped pass');
    for (const id of ids) {
        assert(fs.existsSync(path.join(STORE, id)), `fail-open: ${id} survives (pass skipped)`);
    }
}

// --- TEST 8 (review fix): eviction now triggers at/above the SHARED
//     churn-guard fraction (0.9), not just strictly over the raw cap -- closes
//     the "dead zone" where the guard (>= 90 % of cap) could latch (new
//     enqueues stop) while eviction stayed dormant (old code required
//     count/bytes to be > 100 % of cap). Re-requires the module with a bigger
//     cap (10) so 90 % (9) is a distinct integer strictly below the cap,
//     rather than only exercising the exactly-at-cap boundary. Also exercises
//     the Fix-3 env-hygiene default: FCADE_STORE_EVICT_LOW_WATER='' must fall
//     back to 0.85, not the old Number('')===0 -> clamp-to-0.5 bug. ---------
async function testTriggerAtChurnFractionDeadZone() {
    delete require.cache[require.resolve('./fcade-proxy.js')];
    process.env.FCADE_STORE_MAX_QUARKS = '10';
    process.env.FCADE_STORE_MAX_BYTES = String(64 * 1024 * 1024 * 1024); // bytes never binds here
    process.env.FCADE_STORE_EVICT_LOW_WATER = ''; // Fix 3: empty string -> default 0.85, not 0.5
    fs.writeFileSync(CATALOG, JSON.stringify({ generated_at: Date.now(), rows: [] })); // isolate: no pins
    const mod3 = require('./fcade-proxy.js');
    const cm3 = mod3.makeConvertManager();

    cleanStore();
    const base = Date.now() - 100000;
    const ids = [];
    // 9 quarks against a cap of 10: 9 < 10 (NOT over the raw cap -- old code
    // returned [] here) but 9 >= 0.9*10 = 9 (AT the churn-guard trigger
    // fraction -- new code must evict).
    for (let i = 1; i <= 9; i++) {
        const id = `1700000000000-triga${i}`;
        writeStoreQuark(id, base + i * 1000);
        ids.push(id);
    }
    assertEq(storeDirs().length, 9, 'dead-zone: 9 quarks seeded, cap is 10 (not over the raw cap)');

    const evicted = cm3._evictStoreIfNeeded();
    // Low-water default 0.85 (env was '' -> the Fix-3 default, NOT the old
    // 0.5-floor bug) -> floor(0.85*10) = 8 survivors. 9 in, evict 1 down to 8.
    assertEq(evicted.length, 1, 'dead-zone: evicted 1 to reach the low-water target of 8 (from 9, cap 10)');
    assertEq(storeDirs().length, 8, 'dead-zone: store now at the low-water target, below the 90% trigger point');
    assert(!fs.existsSync(path.join(STORE, ids[0])), 'dead-zone: oldest-served quark evicted');
    for (let i = 1; i < 9; i++) {
        assert(fs.existsSync(path.join(STORE, ids[i])), `dead-zone: ids[${i}] survives`);
    }
}

async function main() {
    console.warn = () => {};
    let exitCode = 0;
    const cm = makeConvertManager();
    try {
        await testSweepDecoupled(cm);
        await testPublishTriggersEviction(cm);
        await testPinnedSurvivesLru(cm);
        await testLowWaterTarget(cm);
        await testPinnedOverCapWarn(cm);
        await testCatalogConfiguredButMissingFailsOpen(cm);
        await testTriggerAtChurnFractionDeadZone();
    } catch (err) {
        console.error(`UNCAUGHT: ${err && err.stack ? err.stack : err}`);
        exitCode = 1;
    }
    if (failed > 0) { console.error(`${failed} assertion(s) failed`); exitCode = 1; }
    for (const job of cm._jobs.values()) { try { if (job.dlProc) job.dlProc.kill('SIGKILL'); if (job.runnerProc) job.runnerProc.kill('SIGKILL'); } catch (_) {} }
    try { fs.rmSync(ROOT, { recursive: true, force: true }); } catch (_) {}
    if (exitCode === 0) console.log('store-evict test passed');
    setTimeout(() => process.exit(exitCode), 50);
}

main();
