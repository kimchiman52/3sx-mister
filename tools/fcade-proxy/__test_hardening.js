// Self-contained test for the fcade-proxy Stage S6 SERVER hardening
// (docs/plan-fcade-live-stream.md Stage S6):
//   1. Idle teardown of convert jobs whose viewers all walked away.
//   2. Bounded-size store eviction (LRU by last-served time).
//
// It drives the convert manager DIRECTLY (module export makeConvertManager) —
// no socket server needed — so it can inspect live child-process PIDs, force a
// single synchronous maintenance sweep (_sweepOnce), and stat the on-disk store.
// The ggpo downloader + FBNeo runner are replaced by tiny long-lived MOCK
// processes so "the pull + runner are gone after teardown" is a real
// process.kill(pid, 0) ESRCH check, not a stub.
//
// Runtime budget: a couple of seconds.

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

// --- scratch dirs + env (BEFORE require: convert/store constants are read once
//     at require time) ---------------------------------------------------------
const ROOT = fs.mkdtempSync(path.join(os.tmpdir(), 'fcade-harden-test-'));
const SCRATCH = path.join(ROOT, 'convert-scratch');
const STORE = path.join(ROOT, '3sr');
const RUNNER_DIR = path.join(ROOT, 'runner');
const CATALOG = path.join(ROOT, 'catalog.json');
fs.mkdirSync(SCRATCH, { recursive: true });
fs.mkdirSync(STORE, { recursive: true });
fs.mkdirSync(path.join(RUNNER_DIR, 'roms'), { recursive: true });

// --- Mock ggpo downloader: writes savestate + first inputs immediately, then
//     dribbles and STAYS ALIVE (so teardown has a live process to kill). ------
const MOCK_DOWNLOADER = path.join(ROOT, 'mock-downloader.js');
fs.writeFileSync(
    MOCK_DOWNLOADER,
    `'use strict';
const fs = require('fs');
const path = require('path');
function arg(name){ const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i+1] : null; }
const outDir = arg('--out-dir');
fs.writeFileSync(path.join(outDir, 'savestate'), Buffer.alloc(4096, 0x5a));
const inputsPath = path.join(outDir, 'inputs');
fs.writeFileSync(inputsPath, Buffer.alloc(300 * 10, 1)); // 300 records so the runner spawns
let n = 300;
setInterval(() => { try { fs.appendFileSync(inputsPath, Buffer.alloc(10, 2)); n++; } catch (e) {} }, 40);
// Stay alive ~60s (SIGKILL/SIGTERM ends it); never write inputs.done.
setTimeout(() => process.exit(0), 60000);
`,
);

// --- Mock FBNeo -track-3sr runner: opens game_0.3sr, tail-follows, STAYS ALIVE
//     (never self-finalizes: no manifest unless the test writes one). ---------
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
const header = Buffer.alloc(28); header.write('3SR1', 0, 'ascii');
fs.writeFileSync(path.join(trackDir, 'game_0.3sr'), header); // partial, NOT finalized
setInterval(() => {}, 1000); // stay alive until killed
`,
);
fs.chmodSync(MOCK_RUNNER, 0o755);

// --- catalog with the slow test quarks ---------------------------------------
const SLOW_A = '1700000000000-slowA';
const SLOW_B = '1700000000000-slowB';
const SLOW_C = '1700000000000-slowC';
fs.writeFileSync(
    CATALOG,
    JSON.stringify({
        generated_at: 1700000000000,
        rows: [
            { quarkid: SLOW_A, gameid: 'sfiii3nr1', date: 1, duration: 100, players: [{ name: 'A1' }, { name: 'A2' }] },
            { quarkid: SLOW_B, gameid: 'sfiii3nr1', date: 2, duration: 100, players: [{ name: 'B1' }, { name: 'B2' }] },
            { quarkid: SLOW_C, gameid: 'sfiii3nr1', date: 3, duration: 100, players: [{ name: 'C1' }, { name: 'C2' }] },
        ],
    }),
);

process.env.FCADE_CATALOG_FILE = CATALOG;
process.env.FCADE_3SR_DIR = STORE;
process.env.FCADE_CONVERT_SCRATCH_DIR = SCRATCH;
process.env.FCADE_CONVERT_DOWNLOADER = MOCK_DOWNLOADER;
process.env.FCADE_CONVERT_PYTHON = process.execPath;
process.env.FCADE_CONVERT_RUNNER_BIN = MOCK_RUNNER;
process.env.FCADE_CONVERT_RUNNER_DIR = RUNNER_DIR;
process.env.FCADE_CONVERT_MAX_JOBS = '2'; // let two slow pulls run at once
process.env.FCADE_CONVERT_FOLLOW_IDLE_MS = '600000'; // runner never self-finalizes on idle
process.env.FCADE_CONVERT_IDLE_TEARDOWN_MS = '400'; // short idle window for the test
process.env.FCADE_CONVERT_SWEEP_INTERVAL_MS = '600000'; // sweeper timer effectively off; we call _sweepOnce
process.env.FCADE_STORE_MAX_QUARKS = '3';
process.env.FCADE_STORE_MAX_BYTES = String(1024 * 1024 * 1024); // huge; count cap drives phase-1 eviction

const { makeConvertManager, handleGet3sr } = require('./fcade-proxy.js');

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

function pidAlive(pid) {
    if (!pid) return false;
    try {
        process.kill(pid, 0);
        return true;
    } catch (err) {
        return err.code === 'EPERM'; // exists but not ours (won't happen here)
    }
}
async function waitPidDead(pid, budgetMs) {
    const start = Date.now();
    while (Date.now() - start < (budgetMs || 3000)) {
        if (!pidAlive(pid)) return true;
        await sleep(15);
    }
    return !pidAlive(pid);
}

// Wait until a job has spawned BOTH the downloader and the runner child procs.
async function waitJobProcs(cm, quarkid, budgetMs) {
    const start = Date.now();
    for (;;) {
        const job = cm._jobs.get(quarkid);
        if (job && job.dlProc && job.runnerProc) return job;
        if (Date.now() - start > (budgetMs || 4000)) throw new Error(`${quarkid}: procs never both spawned`);
        await sleep(20);
    }
}

// --- valid .3sr + meta writer for the eviction store fixtures ----------------
function writeStoreQuark(quarkid, servedMs) {
    const dir = path.join(STORE, quarkid);
    fs.mkdirSync(dir, { recursive: true });
    const header = Buffer.alloc(28);
    header.write('3SR1', 0, 'ascii');
    const p3sr = path.join(dir, 'game_0.3sr');
    const pMeta = path.join(dir, 'game_0.meta.json');
    fs.writeFileSync(p3sr, header);
    fs.writeFileSync(pMeta, JSON.stringify({ players: [{ name: 'P1' }, { name: 'P2' }] }));
    const t = servedMs / 1000;
    fs.utimesSync(p3sr, t, t);
    fs.utimesSync(pMeta, t, t);
    fs.utimesSync(dir, t, t); // dir last so file writes don't re-bump it
    return dir;
}

// --- TEST 1: idle teardown of an unwatched pull ------------------------------
async function testIdleTeardown(cm) {
    const r = cm.requestWatch(SLOW_A, 0, 0);
    assertEq(r.ok, true, 'idle-teardown: first watchpoll starts a job');
    const job = await waitJobProcs(cm, SLOW_A);
    const dlPid = job.dlProc.pid;
    const runPid = job.runnerProc.pid;
    assert(pidAlive(dlPid), 'idle-teardown: downloader process alive before teardown');
    assert(pidAlive(runPid), 'idle-teardown: runner process alive before teardown');
    assert(fs.existsSync(path.join(SCRATCH, SLOW_A)), 'idle-teardown: scratch dir exists before teardown');
    assert(!cm._isQuarkActive(SLOW_A) === false, 'idle-teardown: quark is active (protected) while a job runs');

    // No viewer for longer than the idle window → sweep must tear it down.
    job.lastAccess = Date.now() - 5000;
    cm._sweepOnce();

    assert(!cm._jobs.has(SLOW_A), 'idle-teardown: job removed from tracking after idle sweep');
    assert(await waitPidDead(dlPid), 'idle-teardown: downloader (ggpo pull) process is gone');
    assert(await waitPidDead(runPid), 'idle-teardown: runner (-track-3sr) process is gone');
    assert(!fs.existsSync(path.join(SCRATCH, SLOW_A)), 'idle-teardown: scratch wiped (no partial garbage)');
    assert(!fs.existsSync(path.join(STORE, SLOW_A)), 'idle-teardown: nothing left in the store for a torn-down pull');
    assert(!cm._isQuarkActive(SLOW_A), 'idle-teardown: quark no longer active after teardown');
}

// --- TEST 2: a job a viewer is still polling is NOT torn down -----------------
async function testActivelyWatchedSpared(cm) {
    const r = cm.requestWatch(SLOW_B, 0, 0);
    assertEq(r.ok, true, 'active-spared: watchpoll starts a job');
    const job = await waitJobProcs(cm, SLOW_B);
    const dlPid = job.dlProc.pid;

    // Simulate a viewer that keeps polling: fresh lastAccess across sweeps.
    for (let i = 0; i < 3; i++) {
        job.lastAccess = Date.now(); // what a real watchpoll/convertstatus touch does
        cm._sweepOnce();
        assert(cm._jobs.has(SLOW_B), `active-spared: still tracked across sweep ${i + 1} (viewer present)`);
        assert(pidAlive(dlPid), `active-spared: pull still alive across sweep ${i + 1}`);
        await sleep(20);
    }
    cm._killJob(SLOW_B, 'test cleanup'); // tidy up the live procs
}

// --- TEST 3: a job that produced servable output is spared even when idle -----
async function testServableSpared(cm) {
    const r = cm.requestWatch(SLOW_C, 0, 0);
    assertEq(r.ok, true, 'servable-spared: watchpoll starts a job');
    const job = await waitJobProcs(cm, SLOW_C);
    const dlPid = job.dlProc.pid;

    // The tracker finalized game_0 (manifest row with signature_found) — the
    // job is "nearly done" and must be left to finalize into the store even
    // though no viewer is polling anymore.
    fs.writeFileSync(
        path.join(job.trackDir, 'track3sr_manifest.json'),
        JSON.stringify({ complete: false, games: [{ game_index: 0, signature_found: true, frame_count: 100, checksum_count: 2 }] }),
    );
    job.lastAccess = Date.now() - 5000; // idle
    cm._sweepOnce();

    assert(cm._jobs.has(SLOW_C), 'servable-spared: idle job with a finalized game is NOT torn down');
    assert(pidAlive(dlPid), 'servable-spared: its pull/runner left running to finish into the store');
    cm._killJob(SLOW_C, 'test cleanup');
}

// --- TEST 4: store eviction by quark COUNT, LRU by last-served ----------------
async function testEvictionCountCap(cm) {
    // Decoys that eviction must NEVER touch: files in the store root + a
    // non-quark-named dir (dots fail QUARKID_RE).
    const decoyCatalog = path.join(STORE, 'catalog.json');
    const decoyCookie = path.join(STORE, 'fcade-cookie.txt');
    const decoyDir = path.join(STORE, 'weird.name.dir');
    fs.writeFileSync(decoyCatalog, '{"rows":[]}');
    fs.writeFileSync(decoyCookie, 'cf_clearance=xyz');
    fs.mkdirSync(decoyDir, { recursive: true });
    fs.writeFileSync(path.join(decoyDir, 'x'), 'x');

    // 5 quarks, q1 oldest-served ... q5 newest-served. Cap is 3.
    const base = Date.now() - 100000;
    const q = [];
    for (let i = 1; i <= 5; i++) {
        const id = `1700000000000-evict${i}`;
        writeStoreQuark(id, base + i * 1000);
        q.push(id);
    }

    // Cap is 3, but eviction now goes to the LOW-WATER target below cap
    // (default 0.85 -> floor(0.85*3) = 2 survivors), not exactly-at-cap, so
    // the churn guard at 90% of cap (fcade-proxy.js ~:2429) can never latch
    // permanently (plan-bounded-pool-replay.md §6.2). Updated 2026-07-28 for
    // Stage 3 of that plan: was `evicted.length === 2` / q3 kept.
    const evicted = cm._evictStoreIfNeeded();
    assertEq(evicted.length, 3, 'eviction(count): removed 3 to reach the low-water target of 2 (below cap of 3)');
    assert(!fs.existsSync(path.join(STORE, q[0])), 'eviction(count): oldest-served q1 evicted');
    assert(!fs.existsSync(path.join(STORE, q[1])), 'eviction(count): 2nd-oldest q2 evicted');
    assert(!fs.existsSync(path.join(STORE, q[2])), 'eviction(count): 3rd-oldest q3 evicted (low-water goes below cap)');
    assert(fs.existsSync(path.join(STORE, q[3])), 'eviction(count): q4 kept');
    assert(fs.existsSync(path.join(STORE, q[4])), 'eviction(count): newest q5 kept');

    // Decoys survived.
    assert(fs.existsSync(decoyCatalog), 'eviction: catalog.json in the store root untouched');
    assert(fs.existsSync(decoyCookie), 'eviction: fcade-cookie.txt in the store root untouched');
    assert(fs.existsSync(decoyDir), 'eviction: non-quark-named dir untouched');

    // A survivor still serves via get3sr.
    const g = handleGet3sr({ quarkid: q[4], game_index: 0 });
    assertEq(g.ok, true, 'eviction: get3sr still serves a survivor');
    const gGone = handleGet3sr({ quarkid: q[0] });
    assertEq(gGone.ok, false, 'eviction: get3sr reports an evicted quark not_found');

    // Clean the remaining fixtures for the next test.
    for (const id of q) {
        try {
            fs.rmSync(path.join(STORE, id), { recursive: true, force: true });
        } catch (_) {}
    }
    fs.rmSync(decoyDir, { recursive: true, force: true });
    fs.rmSync(decoyCatalog, { force: true });
    fs.rmSync(decoyCookie, { force: true });
}

// --- TEST 5: an actively-watched quark is spared even as the LRU -------------
async function testEvictionSparesActive(cm) {
    const base = Date.now() - 100000;
    const ids = [];
    for (let i = 1; i <= 4; i++) {
        const id = `1700000000000-active${i}`;
        writeStoreQuark(id, base + i * 1000); // active1 is the oldest-served (LRU)
        ids.push(id);
    }
    const lru = ids[0];
    // Register the LRU quark as having a live job (isQuarkActive → true).
    cm._jobs.set(lru, { quarkid: lru, finished: false, state: 'converting' });
    try {
        const evicted = cm._evictStoreIfNeeded();
        assert(!evicted.includes(lru), 'eviction(active): the LRU quark is SPARED because it has a live job');
        assert(fs.existsSync(path.join(STORE, lru)), 'eviction(active): the actively-watched LRU dir still on disk');
        assert(evicted.includes(ids[1]), 'eviction(active): the next-oldest inactive quark is evicted instead');
    } finally {
        cm._jobs.delete(lru);
        for (const id of ids) {
            try {
                fs.rmSync(path.join(STORE, id), { recursive: true, force: true });
            } catch (_) {}
        }
    }
}

// --- TEST 6: store eviction by BYTES (fresh manager, byte cap dominates) ------
async function testEvictionByteCap() {
    // Re-require the module with a low byte cap + effectively-off count cap so
    // the byte path drives eviction (the constants are read at require time).
    delete require.cache[require.resolve('./fcade-proxy.js')];
    process.env.FCADE_STORE_MAX_QUARKS = '1000';
    const perQuarkBytes = 28 + JSON.stringify({ players: [{ name: 'P1' }, { name: 'P2' }] }).length;
    // Cap that holds ~2 quarks; 5 quarks total → evict down to 2.
    process.env.FCADE_STORE_MAX_BYTES = String(perQuarkBytes * 2 + 1);
    const mod2 = require('./fcade-proxy.js');
    const cm2 = mod2.makeConvertManager();

    const base = Date.now() - 100000;
    const ids = [];
    for (let i = 1; i <= 5; i++) {
        const id = `1700000000000-byte${i}`;
        writeStoreQuark(id, base + i * 1000);
        ids.push(id);
    }
    // Low-water (default 0.85) applies to the byte target too: cap holds ~2
    // quarks (138 B <= 139 B cap), but floor(0.85*139) = 118 B is below 2
    // quarks' worth (138 B) and at-or-above 1 quark's worth (69 B), so
    // eviction goes one further, down to 1 survivor. Updated 2026-07-28 for
    // Stage 3 of plan-bounded-pool-replay.md §6.2: was survivors===2 /
    // evicted.length===3.
    const evicted = cm2._evictStoreIfNeeded();
    const survivors = ids.filter((id) => fs.existsSync(path.join(STORE, id)));
    assert(survivors.length === 1, `eviction(bytes): store trimmed to the low-water byte target (survivors=${survivors.length})`);
    assert(!fs.existsSync(path.join(STORE, ids[0])), 'eviction(bytes): oldest-served evicted first');
    assert(fs.existsSync(path.join(STORE, ids[4])), 'eviction(bytes): newest-served survives');
    assert(evicted.length === 4, `eviction(bytes): evicted 4 down to the low-water byte target (${evicted.length})`);
    for (const id of ids) {
        try {
            fs.rmSync(path.join(STORE, id), { recursive: true, force: true });
        } catch (_) {}
    }
}

// --- main --------------------------------------------------------------------
async function main() {
    console.warn = () => {};
    let exitCode = 0;
    const cm = makeConvertManager();
    try {
        await testIdleTeardown(cm);
        await testActivelyWatchedSpared(cm);
        await testServableSpared(cm);
        await testEvictionCountCap(cm);
        await testEvictionSparesActive(cm);
        await testEvictionByteCap();
    } catch (err) {
        console.error(`UNCAUGHT: ${err && err.stack ? err.stack : err}`);
        exitCode = 1;
    }
    if (failed > 0) {
        console.error(`${failed} assertion(s) failed`);
        exitCode = 1;
    }
    // Kill any residual mock procs still tracked.
    for (const job of cm._jobs.values()) {
        try {
            if (job.dlProc) job.dlProc.kill('SIGKILL');
            if (job.runnerProc) job.runnerProc.kill('SIGKILL');
        } catch (_) {}
    }
    try {
        fs.rmSync(ROOT, { recursive: true, force: true });
    } catch (_) {}
    if (exitCode === 0) console.log('hardening test passed');
    setTimeout(() => process.exit(exitCode), 50);
}

main();
