// Self-contained test for the fcade-proxy background-job semantics
// (docs/plan-preconvert-fleet.md Stage S1): a scheduler-injected pre-convert
// job (`job.background`) that
//   (a) is INSTANTLY preempted by a live request within one pump();
//   (b) is idle-teardown EXEMPT (survives an arbitrarily-stale lastAccess);
//   (c) does not change live jobs' idle/preempt behavior one bit (fresh live
//       job protected from preemption = no starvation; stale live job still
//       preempted; stale live job still idle-torn-down);
//   (d) is DELETED from `jobs` the moment its store dir is servable, yet
//       convertstatus still answers 'ready' via the store fallback;
//   (e) accepts a row SNAPSHOT for a quark absent from the current catalog
//       (P3 backfill), while the wire-shaped path (no opts) stays catalog-gated.
//
// Same mock-process harness style as __test_hardening.js: the convert manager
// is driven directly (makeConvertManager export) and the ggpo downloader +
// FBNeo runner are tiny real child processes, so "the pull+runner are gone
// after preemption" is a real process.kill(pid,0) ESRCH check.
//
// Runtime budget: a couple of seconds.

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

// --- scratch dirs + env (BEFORE require: convert/store constants read once) ---
const ROOT = fs.mkdtempSync(path.join(os.tmpdir(), 'fcade-bg-test-'));
const SCRATCH = path.join(ROOT, 'convert-scratch');
const STORE = path.join(ROOT, '3sr');
const RUNNER_DIR = path.join(ROOT, 'runner');
const CATALOG = path.join(ROOT, 'catalog.json');
fs.mkdirSync(SCRATCH, { recursive: true });
fs.mkdirSync(STORE, { recursive: true });
fs.mkdirSync(path.join(RUNNER_DIR, 'roms'), { recursive: true });

// --- Mock ggpo downloader ----------------------------------------------------
// Writes savestate + first inputs immediately. If its out-dir path names a
// "complete" quark it exits promptly (so the job can finalize into the store);
// otherwise it dribbles and STAYS ALIVE ~60s so preemption/idle tests have a
// live process to kill.
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
if (outDir.indexOf('complete') >= 0) {
    setTimeout(() => process.exit(0), 60); // let the job finalize
} else {
    let n = 300;
    setInterval(() => { try { fs.appendFileSync(inputsPath, Buffer.alloc(10, 2)); n++; } catch (e) {} }, 40);
    setTimeout(() => process.exit(0), 60000);
}
`,
);

// --- Mock FBNeo -track-3sr runner --------------------------------------------
// For a "complete" quark: write a valid 3SR1 game_0.3sr + a manifest with
// signature_found, then exit (→ tryFinalize publishes into the store). For any
// other quark: write a partial (non-finalized) header and STAY ALIVE.
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
    setInterval(() => {}, 1000); // stay alive until killed
}
`,
);
fs.chmodSync(MOCK_RUNNER, 0o755);

// --- catalog -----------------------------------------------------------------
const BG_A = '1700000000000-bgA'; // background, long-lived (preempt test)
const BG_B = '1700000000000-bgB'; // background, long-lived (idle-exempt test)
const COMPLETE = '1700000000000-complete'; // background, finalizes into the store
const LIVE1 = '1700000000000-live1'; // live, preempts BG_A
const LIVE_IDLE = '1700000000000-liveIdle'; // live, idle-teardown unchanged
const LIVE_HOLD = '1700000000000-liveHold'; // live, protected while fresh
const LIVE_WAIT = '1700000000000-liveWait'; // live, waits then preempts stale LIVE_HOLD
function row(quarkid) {
    return { quarkid, gameid: 'sfiii3nr1', date: 1, duration: 100, players: [{ name: 'P1' }, { name: 'P2' }] };
}
fs.writeFileSync(
    CATALOG,
    JSON.stringify({
        generated_at: 1700000000000,
        rows: [BG_A, BG_B, COMPLETE, LIVE1, LIVE_IDLE, LIVE_HOLD, LIVE_WAIT].map(row),
    }),
);

process.env.FCADE_CATALOG_FILE = CATALOG;
process.env.FCADE_3SR_DIR = STORE;
process.env.FCADE_CONVERT_SCRATCH_DIR = SCRATCH;
process.env.FCADE_CONVERT_DOWNLOADER = MOCK_DOWNLOADER;
process.env.FCADE_CONVERT_PYTHON = process.execPath;
process.env.FCADE_CONVERT_RUNNER_BIN = MOCK_RUNNER;
process.env.FCADE_CONVERT_RUNNER_DIR = RUNNER_DIR;
process.env.FCADE_CONVERT_MAX_JOBS = '1'; // production value — the one-slot invariant preemption relies on
process.env.FCADE_CONVERT_FOLLOW_IDLE_MS = '600000';
process.env.FCADE_CONVERT_IDLE_TEARDOWN_MS = '400'; // short window; staleness simulated by an old lastAccess
process.env.FCADE_CONVERT_PREEMPT_STALE_MS = '400'; // short stale gate for the live-preempt test
process.env.FCADE_CONVERT_SWEEP_INTERVAL_MS = '600000'; // sweeper timer off; we call _sweepOnce
process.env.FCADE_STORE_MAX_QUARKS = '1000';
process.env.FCADE_STORE_MAX_BYTES = String(1024 * 1024 * 1024);

const { makeConvertManager } = require('./fcade-proxy.js');

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
        return err.code === 'EPERM';
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
async function waitJobProcs(cm, quarkid, budgetMs) {
    const start = Date.now();
    for (;;) {
        const job = cm._jobs.get(quarkid);
        if (job && job.dlProc && job.runnerProc) return job;
        if (Date.now() - start > (budgetMs || 4000)) throw new Error(`${quarkid}: procs never both spawned`);
        await sleep(20);
    }
}

// --- (a) a live convert preempts a running background job within one pump() ---
async function testLivePreemptsBackground(cm) {
    const start = cm.requestConvert(BG_A, { background: true });
    assertEq(start.ok, true, 'preempt: background convert accepted');
    const bg = await waitJobProcs(cm, BG_A);
    assertEq(bg.background, true, 'preempt: job flagged background (internal, not from the wire)');
    const bgDl = bg.dlProc.pid;
    const bgRun = bg.runnerProc.pid;
    assert(pidAlive(bgDl) && pidAlive(bgRun), 'preempt: background pull+runner alive before the live request');

    // A live request for a DIFFERENT quark. With MAX_JOBS=1 it queues, and the
    // single requestConvert() → pump() call must evict the background slot-holder
    // (skipping the staleness gate) and start the live job — all synchronously.
    const live = cm.requestConvert(LIVE1); // no opts ⇒ live
    assertEq(live.ok, true, 'preempt: live convert accepted');
    assert(!cm._jobs.has(BG_A), 'preempt: background job removed from `jobs` within one pump() (instant, no 8s wait)');
    const liveJob = cm._jobs.get(LIVE1);
    assert(!!liveJob && liveJob.dlProc, 'preempt: live job took the freed slot and started pulling');
    assertEq(liveJob.background, false, 'preempt: the live job is NOT flagged background');

    assert(await waitPidDead(bgDl), 'preempt: background downloader (ggpo pull) killed');
    assert(await waitPidDead(bgRun), 'preempt: background runner killed');

    cm._killJob(LIVE1, 'test cleanup'); // free the slot for the next test
    await waitPidDead(liveJob.dlProc && liveJob.dlProc.pid, 1000);
}

// --- (b) a background job is idle-teardown EXEMPT ----------------------------
async function testBackgroundIdleExempt(cm) {
    const start = cm.requestConvert(BG_B, { background: true });
    assertEq(start.ok, true, 'idle-exempt: background convert accepted');
    const bg = await waitJobProcs(cm, BG_B);
    const dlPid = bg.dlProc.pid;

    // lastAccess arbitrarily old vs a 400 ms idle window — a *live* job would be
    // torn down; a background job must be spared (no viewer ⇒ staleness is
    // meaningless). This deterministically stands in for ">45s untouched".
    bg.lastAccess = Date.now() - 5000;
    cm._sweepOnce();
    assert(cm._jobs.has(BG_B), 'idle-exempt: stale background job NOT torn down by the idle sweep');
    assert(pidAlive(dlPid), 'idle-exempt: its pull left running');
    // A second sweep to be sure it is not a one-shot.
    cm._sweepOnce();
    assert(cm._jobs.has(BG_B), 'idle-exempt: still alive across a second sweep');

    cm._killJob(BG_B, 'test cleanup');
    await waitPidDead(dlPid, 1000);
}

// --- (c) live jobs' idle + preempt behavior is byte-for-byte unchanged -------
async function testLiveBehaviorUnchanged(cm) {
    // (c1) a live job idle past the window with no servable output IS torn down.
    let r = cm.requestConvert(LIVE_IDLE); // live
    assertEq(r.ok, true, 'live-unchanged: live convert accepted');
    let job = await waitJobProcs(cm, LIVE_IDLE);
    const idleDl = job.dlProc.pid;
    job.lastAccess = Date.now() - 5000; // stale, no manifest ⇒ no servable output
    cm._sweepOnce();
    assert(!cm._jobs.has(LIVE_IDLE), 'live-unchanged: stale live job WITH no viewer IS idle-torn-down (unchanged)');
    assert(await waitPidDead(idleDl), 'live-unchanged: its pull killed by idle teardown');

    // (c2) a FRESH live slot-holder is NOT preempted by a waiting live request
    // (the anti-starvation guarantee), but the SAME holder once stale IS.
    r = cm.requestConvert(LIVE_HOLD); // live, holds the single slot
    assertEq(r.ok, true, 'live-unchanged: LIVE_HOLD accepted');
    const hold = await waitJobProcs(cm, LIVE_HOLD);
    const holdDl = hold.dlProc.pid;
    hold.lastAccess = Date.now(); // fresh viewer

    const wait = cm.requestConvert(LIVE_WAIT); // live, must queue behind the fresh holder
    assertEq(wait.state, 'queued', 'live-unchanged: LIVE_WAIT queued behind the busy slot');
    assert(cm._jobs.has(LIVE_HOLD) && pidAlive(holdDl), 'live-unchanged: FRESH live holder NOT preempted (no starvation)');
    assertEq(cm._jobs.get(LIVE_WAIT).state, 'queued', 'live-unchanged: waiter still queued while the holder is fresh');

    // Now the holder's viewer goes stale — a re-pump (via re-requesting the
    // already-queued waiter) must preempt it, exactly as before this change.
    hold.lastAccess = Date.now() - 5000;
    cm.requestConvert(LIVE_WAIT); // re-touch the queued waiter ⇒ pump() ⇒ preempt stale holder
    assert(!cm._jobs.has(LIVE_HOLD), 'live-unchanged: STALE live holder IS preempted (staleness gate unchanged)');
    assert(await waitPidDead(holdDl), 'live-unchanged: preempted holder pull killed');
    const waitJob = cm._jobs.get(LIVE_WAIT);
    assert(!!waitJob && waitJob.dlProc, 'live-unchanged: the waiter took the freed slot');

    cm._killJob(LIVE_WAIT, 'test cleanup');
    await waitPidDead(waitJob.dlProc && waitJob.dlProc.pid, 1000);
}

// --- (d) finished background job absent from `jobs`, still 'ready' from store -
async function testBackgroundCompletionHygiene(cm) {
    const start = cm.requestConvert(COMPLETE, { background: true });
    assertEq(start.ok, true, 'hygiene: background convert accepted');

    // Wait for the job to finalize into the store and be dropped from `jobs`.
    const deadline = Date.now() + 8000;
    while (Date.now() < deadline) {
        if (!cm._jobs.has(COMPLETE) && fs.existsSync(path.join(STORE, COMPLETE, 'game_0.3sr'))) break;
        await sleep(30);
    }
    assert(fs.existsSync(path.join(STORE, COMPLETE, 'game_0.3sr')), 'hygiene: background conversion published into the store');
    assert(!cm._jobs.has(COMPLETE), 'hygiene: finished background job DELETED from `jobs` (map bounded)');

    const st = cm.requestStatus(COMPLETE);
    assertEq(st.state, 'ready', "hygiene: convertstatus still 'ready' via the store fallback (no device change)");
    assert(Array.isArray(st.games) && st.games.includes(0), 'hygiene: ready status carries the servable game index');
}

// --- (e) background row-snapshot vs. catalog-gated wire path ------------------
async function testRowSnapshot(cm) {
    const ABSENT = '1700000000000-absentBackfill'; // NOT in the catalog

    // Wire-shaped path (no opts): a quark absent from the catalog is refused.
    const live = cm.requestConvert(ABSENT);
    assertEq(live.ok, false, 'row-snapshot: wire convert of an absent quark is refused (catalog-gated, unchanged)');
    assertEq(live.error, 'not_found', 'row-snapshot: refusal is not_found');
    assert(!cm._jobs.has(ABSENT), 'row-snapshot: no job created for the refused wire request');

    // Background path with a row snapshot: accepted even though catalogRowFor misses.
    const bg = cm.requestConvert(ABSENT, { background: true, row: row(ABSENT) });
    assertEq(bg.ok, true, 'row-snapshot: background convert with a row snapshot is accepted for an absent quark');
    const job = await waitJobProcs(cm, ABSENT);
    assertEq(job.background, true, 'row-snapshot: the snapshot-driven job is flagged background');

    cm._killJob(ABSENT, 'test cleanup');
    await waitPidDead(job.dlProc && job.dlProc.pid, 1000);
}

// --- main --------------------------------------------------------------------
async function main() {
    console.warn = () => {};
    let exitCode = 0;
    const cm = makeConvertManager();
    try {
        await testLivePreemptsBackground(cm);
        await testBackgroundIdleExempt(cm);
        await testLiveBehaviorUnchanged(cm);
        await testBackgroundCompletionHygiene(cm);
        await testRowSnapshot(cm);
    } catch (err) {
        console.error(`UNCAUGHT: ${err && err.stack ? err.stack : err}`);
        exitCode = 1;
    }
    if (failed > 0) {
        console.error(`${failed} assertion(s) failed`);
        exitCode = 1;
    }
    for (const job of cm._jobs.values()) {
        try {
            if (job.dlProc) job.dlProc.kill('SIGKILL');
            if (job.runnerProc) job.runnerProc.kill('SIGKILL');
        } catch (_) {}
    }
    try {
        fs.rmSync(ROOT, { recursive: true, force: true });
    } catch (_) {}
    if (exitCode === 0) console.log('background test passed');
    setTimeout(() => process.exit(exitCode), 50).unref();
}

main();
