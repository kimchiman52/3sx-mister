// Self-contained protocol test for the fcade-proxy Mac-worker work-lease ops
// (docs/plan-preconvert-fleet.md Stage S4): worklease / workdone / workstats.
// Boots the proxy in-process on an ephemeral port and drives it with a raw
// length-framed TCP client (same harness as __test_protocol.js). Proves the
// security-critical properties:
//   - every new op is token-gated (missing/wrong token, AND no server token,
//     are all rejected 'unauthorized' — never a bypass);
//   - lease claim (priority order + row snapshot), renewal, expiry-reclaim;
//   - workdone: valid multi-game push → integrated + convertstatus 'ready';
//     bad magic / oversize / nameless → rejected + staging wiped + nothing in
//     3sr/; already-servable → no-op success; worker-reported failure → ledger;
//   - a device-shaped client (NO token) still gets full, unchanged behavior on
//     all six legacy ops (search/status/get3sr/convert/convertstatus/watchpoll).
//
// Runtime budget: a couple of seconds.

'use strict';

const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');

const TOKEN = 's3cr3t-work-token-abc';

// --- fixture dirs + env (BEFORE require) -------------------------------------
const ROOT = fs.mkdtempSync(path.join(os.tmpdir(), 'fcade-worklease-test-'));
const STORE = path.join(ROOT, '3sr');
const INCOMING = path.join(ROOT, '3sr-incoming');
const SCRATCH = path.join(ROOT, 'convert-scratch');
const RUNNER_DIR = path.join(ROOT, 'runner');
const CATALOG = path.join(ROOT, 'catalog.json');
const STATE_FILE = path.join(ROOT, 'preconvert-state.json');
fs.mkdirSync(STORE, { recursive: true });
fs.mkdirSync(INCOMING, { recursive: true });
fs.mkdirSync(SCRATCH, { recursive: true });
fs.mkdirSync(path.join(RUNNER_DIR, 'roms'), { recursive: true });

// Long-lived mock downloader/runner so a legacy `convert` op has something to
// spawn without ever touching ggpo.
const MOCK_DOWNLOADER = path.join(ROOT, 'mock-downloader.js');
fs.writeFileSync(
    MOCK_DOWNLOADER,
    `'use strict';
const fs = require('fs'); const path = require('path');
function arg(n){ const i = process.argv.indexOf(n); return i>=0?process.argv[i+1]:null; }
const outDir = arg('--out-dir');
fs.writeFileSync(path.join(outDir, 'savestate'), Buffer.alloc(4096, 0x5a));
const inputsPath = path.join(outDir, 'inputs');
fs.writeFileSync(inputsPath, Buffer.alloc(300 * 10, 1));
setInterval(() => { try { fs.appendFileSync(inputsPath, Buffer.alloc(10, 2)); } catch (e) {} }, 40);
setTimeout(() => process.exit(0), 60000);
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
const buf = Buffer.alloc(28); buf.write('3SR1', 0, 'ascii');
fs.writeFileSync(path.join(trackDir, 'game_0.3sr'), buf);
setInterval(() => {}, 1000);
`,
);
fs.chmodSync(MOCK_RUNNER, 0o755);

const CATALOG_QUARK = '1700000000000-catQ';
fs.writeFileSync(
    CATALOG,
    JSON.stringify({ generated_at: 1, rows: [{ quarkid: CATALOG_QUARK, gameid: 'sfiii3nr1', date: 1, duration: 100, players: [{ name: 'A' }, { name: 'B' }] }] }),
);

process.env.FCADE_WORK_TOKEN = TOKEN;
process.env.FCADE_3SR_DIR = STORE;
process.env.FCADE_3SR_INCOMING_DIR = INCOMING;
process.env.FCADE_CATALOG_FILE = CATALOG;
process.env.FCADE_CONVERT_SCRATCH_DIR = SCRATCH;
process.env.FCADE_CONVERT_DOWNLOADER = MOCK_DOWNLOADER;
process.env.FCADE_CONVERT_PYTHON = process.execPath;
process.env.FCADE_CONVERT_RUNNER_BIN = MOCK_RUNNER;
process.env.FCADE_CONVERT_RUNNER_DIR = RUNNER_DIR;
process.env.FCADE_CONVERT_MAX_JOBS = '1';
process.env.FCADE_CONVERT_SWEEP_INTERVAL_MS = '600000';
process.env.FCADE_PRECONVERT_ENABLED = '0'; // scheduler dark; we seed the queue directly
process.env.FCADE_PRECONVERT_TICK_MS = '600000';
process.env.FCADE_PRECONVERT_STATE_FILE = STATE_FILE;
process.env.FCADE_STORE_MAX_QUARKS = '100000';
process.env.FCADE_STORE_MAX_BYTES = String(64 * 1024 * 1024 * 1024);
process.env.FCADE_DISK_FLOOR_BYTES = '0';

const { start } = require('./fcade-proxy.js');

// --- frame encode/decode + client (dup, same as __test_protocol.js) ----------
function encodeFrame(obj) {
    const json = Buffer.from(JSON.stringify(obj), 'utf8');
    const out = Buffer.alloc(4 + json.length);
    out.writeUInt32BE(json.length, 0);
    json.copy(out, 4);
    return out;
}
let failed = 0;
function assert(cond, msg) {
    if (!cond) { console.error(`ASSERT FAIL: ${msg}`); failed += 1; } else { console.log(`ok - ${msg}`); }
}
function assertEq(a, b, msg) { assert(a === b, `${msg} (actual=${JSON.stringify(a)} expected=${JSON.stringify(b)})`); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function makeClient(port) {
    return new Promise((resolve, reject) => {
        const sock = net.createConnection({ port, host: '127.0.0.1' }, () => resolve(client));
        sock.on('error', reject);
        let buf = Buffer.alloc(0);
        const waiters = [];
        const q = [];
        sock.on('data', (chunk) => {
            buf = Buffer.concat([buf, chunk]);
            for (;;) {
                if (buf.length < 4) return;
                const len = buf.readUInt32BE(0);
                if (buf.length < 4 + len) return;
                const obj = JSON.parse(buf.subarray(4, 4 + len).toString('utf8'));
                buf = buf.subarray(4 + len);
                if (waiters.length > 0) waiters.shift()(obj);
                else q.push(obj);
            }
        });
        const client = {
            sock,
            send(obj) { return new Promise((res, rej) => sock.write(encodeFrame(obj), (e) => (e ? rej(e) : res()))); },
            recv(timeoutMs) {
                return new Promise((res, rej) => {
                    if (q.length > 0) return res(q.shift());
                    const t = setTimeout(() => rej(new Error('recv timeout')), timeoutMs || 2000);
                    waiters.push((obj) => { clearTimeout(t); res(obj); });
                });
            },
            async rpc(obj) { await this.send(obj); return this.recv(); },
            close() { return new Promise((res) => sock.end(res)); },
        };
    });
}

// --- fixtures ----------------------------------------------------------------
function goodBytes() {
    return Buffer.concat([Buffer.from('3SR1', 'ascii'), Buffer.alloc(60, 0x11)]);
}
function writeStoreQuark(quarkid) {
    const dir = path.join(STORE, quarkid);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'game_0.3sr'), goodBytes());
    fs.writeFileSync(path.join(dir, 'game_0.meta.json'), JSON.stringify({ players: [{ name: 'P1' }, { name: 'P2' }] }));
}
function writeStaging(quarkid, games) {
    const dir = path.join(INCOMING, quarkid);
    fs.mkdirSync(dir, { recursive: true });
    for (const g of games) {
        fs.writeFileSync(path.join(dir, `game_${g.index}.3sr`), g.bytes);
        fs.writeFileSync(path.join(dir, `game_${g.index}.meta.json`), JSON.stringify(g.meta));
    }
    return dir;
}
function seedQueue(cm, items) {
    cm._preconvert.queue = items.map((it) => ({
        quarkid: it.quarkid,
        tier: it.tier,
        date: it.date,
        duration: 100,
        row: { quarkid: it.quarkid, gameid: 'sfiii3nr1', date: it.date, duration: 100, players: [{ name: 'P1' }, { name: 'P2' }] },
    }));
    cm._preconvert.leases = {};
    cm._preconvert.failed = {};
}

const Q1 = '1700000000000-lease1';
const Q2 = '1700000000000-lease2';
const Q3 = '1700000000000-lease3';

// --- T1: token gate ----------------------------------------------------------
async function testTokenGate(port, cm) {
    seedQueue(cm, [{ quarkid: Q1, tier: 1, date: 300 }]);
    const c = await makeClient(port);
    try {
        let r = await c.rpc({ op: 'worklease', worker: 'mac' }); // no token
        assertEq(r.error, 'unauthorized', 'token: worklease with NO token rejected');
        r = await c.rpc({ op: 'worklease', token: 'wrong', worker: 'mac' });
        assertEq(r.error, 'unauthorized', 'token: worklease with WRONG token rejected');
        r = await c.rpc({ op: 'workdone', token: 'wrong', worker: 'mac', quarkid: Q1, ok: true });
        assertEq(r.error, 'unauthorized', 'token: workdone with WRONG token rejected');
        r = await c.rpc({ op: 'workstats', token: 'wrong' });
        assertEq(r.error, 'unauthorized', 'token: workstats with WRONG token rejected');
        // No lease was ever created by a rejected call.
        assertEq(Object.keys(cm._preconvert.leases).length, 0, 'token: a rejected worklease created no lease');

        // No token CONFIGURED on the server ⇒ ops disabled even with the "right" value.
        delete process.env.FCADE_WORK_TOKEN;
        r = await c.rpc({ op: 'worklease', token: TOKEN, worker: 'mac' });
        assertEq(r.error, 'unauthorized', 'token: with NO server token configured, even the right token is rejected (ops disabled)');
        process.env.FCADE_WORK_TOKEN = TOKEN;
    } finally {
        await c.close();
    }
}

// --- T2/T3/T4: claim + count + renewal + expiry-reclaim ----------------------
async function testLeaseLifecycle(port, cm) {
    const c = await makeClient(port);
    try {
        // Claim in priority order (tier asc, then date desc) with row snapshots.
        seedQueue(cm, [{ quarkid: Q1, tier: 1, date: 300 }, { quarkid: Q2, tier: 1, date: 200 }, { quarkid: Q3, tier: 2, date: 100 }]);
        let r = await c.rpc({ op: 'worklease', token: TOKEN, worker: 'mac', count: 3 });
        assertEq(r.ok, true, 'lease: worklease accepted with the right token');
        assertEq(r.leased.length, 3, 'lease: all three items leased');
        assertEq(r.leased[0].quarkid, Q1, 'lease: highest priority first (P1 newest date)');
        assertEq(r.leased[1].quarkid, Q2, 'lease: then the next P1');
        assertEq(r.leased[2].quarkid, Q3, 'lease: then P2');
        assert(r.leased[0].row && Array.isArray(r.leased[0].row.players) && r.leased[0].row.players.length === 2, 'lease: each item carries its row snapshot');

        // count clamps to the max of 3.
        seedQueue(cm, [{ quarkid: Q1, tier: 1, date: 300 }, { quarkid: Q2, tier: 1, date: 200 }, { quarkid: Q3, tier: 2, date: 100 }]);
        r = await c.rpc({ op: 'worklease', token: TOKEN, worker: 'mac', count: 99 });
        assertEq(r.leased.length, 3, 'lease: count is clamped to max 3');

        // default count = 1.
        seedQueue(cm, [{ quarkid: Q1, tier: 1, date: 300 }, { quarkid: Q2, tier: 1, date: 200 }]);
        r = await c.rpc({ op: 'worklease', token: TOKEN, worker: 'mac' });
        assertEq(r.leased.length, 1, 'lease: default count is 1');
        assertEq(r.leased[0].quarkid, Q1, 'lease: default lease takes the top item');

        // Renewal: the same worker re-leasing an item it holds extends the TTL.
        seedQueue(cm, [{ quarkid: Q1, tier: 1, date: 300 }]);
        await c.rpc({ op: 'worklease', token: TOKEN, worker: 'mac' });
        const exp1 = cm._preconvert.leases[Q1].expires_at;
        await sleep(20);
        r = await c.rpc({ op: 'worklease', token: TOKEN, worker: 'mac' });
        assertEq(r.leased[0].quarkid, Q1, 'renewal: same worker re-leases the held item');
        assert(cm._preconvert.leases[Q1].expires_at > exp1, 'renewal: the lease TTL was extended');

        // Expiry-reclaim: an expired lease is reclaimable by ANOTHER worker AND
        // by the VPS scheduler (pickEligible sees it as unleased).
        seedQueue(cm, [{ quarkid: Q1, tier: 1, date: 300 }]);
        await c.rpc({ op: 'worklease', token: TOKEN, worker: 'macA' });
        assertEq(cm._preconvert.leases[Q1].worker, 'macA', 'reclaim: initially leased to macA');
        cm._preconvert.leases[Q1].expires_at = Date.now() - 1; // force-expire
        assert(cm._preconvertPickEligible() && cm._preconvertPickEligible().quarkid === Q1, 'reclaim: the VPS scheduler treats an expired lease as unleased');
        r = await c.rpc({ op: 'worklease', token: TOKEN, worker: 'macB' });
        assertEq(r.leased[0].quarkid, Q1, 'reclaim: another worker re-leases the expired item');
        assertEq(cm._preconvert.leases[Q1].worker, 'macB', 'reclaim: the lease now belongs to macB');
    } finally {
        await c.close();
    }
}

// --- T5: workdone valid multi-game integration -------------------------------
async function testWorkdoneValid(port, cm) {
    const QI = '1700000000000-integrateQ';
    writeStaging(QI, [
        { index: 0, bytes: goodBytes(), meta: { players: [{ name: 'Alice' }, { name: 'Bob' }] } },
        { index: 1, bytes: goodBytes(), meta: { players: [{ name: 'Alice' }, { name: 'Bob' }] } },
    ]);
    seedQueue(cm, [{ quarkid: QI, tier: 1, date: 100 }]);
    cm._preconvert.leases[QI] = { worker: 'mac', expires_at: Date.now() + 100000 };
    const c = await makeClient(port);
    try {
        const r = await c.rpc({ op: 'workdone', token: TOKEN, worker: 'mac', quarkid: QI, ok: true });
        assertEq(r.ok, true, 'workdone-valid: accepted');
        assertEq(JSON.stringify(r.integrated), JSON.stringify([0, 1]), 'workdone-valid: both games integrated');
        assert(fs.existsSync(path.join(STORE, QI, 'game_0.3sr')) && fs.existsSync(path.join(STORE, QI, 'game_1.3sr')), 'workdone-valid: both .3sr moved into the store');
        assert(fs.existsSync(path.join(STORE, QI, 'game_0.meta.json')), 'workdone-valid: meta sidecars integrated too');
        assert(!fs.existsSync(path.join(INCOMING, QI)), 'workdone-valid: staging dir wiped after integration');
        assert(!cm._preconvert.queue.some((it) => it.quarkid === QI), 'workdone-valid: item removed from the queue');
        assert(!cm._preconvert.leases[QI], 'workdone-valid: lease freed');
        // A device-shaped convertstatus now reports 'ready'.
        const st = await c.rpc({ op: 'convertstatus', quarkid: QI });
        assertEq(st.state, 'ready', 'workdone-valid: convertstatus reports ready via the store');
        assertEq(JSON.stringify(st.games), JSON.stringify([0, 1]), 'workdone-valid: ready status lists both games');
    } finally {
        await c.close();
    }
}

// --- T6: workdone rejections (bad magic / oversize / nameless) ---------------
async function testWorkdoneRejections(port, cm) {
    const c = await makeClient(port);
    try {
        const cases = [
            { id: '1700000000000-badMagic', games: [{ index: 0, bytes: Buffer.concat([Buffer.from('XXXX'), Buffer.alloc(60)]), meta: { players: [{ name: 'A' }, { name: 'B' }] } }], why: 'bad magic' },
            { id: '1700000000000-oversize', games: [{ index: 0, bytes: Buffer.concat([Buffer.from('3SR1'), Buffer.alloc(1024 * 1024 + 16, 0x22)]), meta: { players: [{ name: 'A' }, { name: 'B' }] } }], why: 'oversize (>1 MiB)' },
            { id: '1700000000000-nameless', games: [{ index: 0, bytes: goodBytes(), meta: { players: [] } }], why: 'nameless meta' },
        ];
        for (const cc of cases) {
            writeStaging(cc.id, cc.games);
            const r = await c.rpc({ op: 'workdone', token: TOKEN, worker: 'mac', quarkid: cc.id, ok: true });
            assertEq(r.ok, false, `workdone-reject(${cc.why}): rejected`);
            assertEq(r.error, 'invalid_push', `workdone-reject(${cc.why}): typed invalid_push`);
            assert(!fs.existsSync(path.join(INCOMING, cc.id)), `workdone-reject(${cc.why}): staging wiped`);
            assert(!fs.existsSync(path.join(STORE, cc.id)), `workdone-reject(${cc.why}): nothing entered the store`);
        }
    } finally {
        await c.close();
    }
}

// --- T7: already-servable no-op + T-fail: worker-reported failure ------------
async function testWorkdoneIdempotentAndFail(port, cm) {
    const c = await makeClient(port);
    try {
        // Already-servable: workdone is a no-op success; redundant staging wiped.
        const QH = '1700000000000-haveQ';
        writeStoreQuark(QH); // already in the store
        writeStaging(QH, [{ index: 0, bytes: goodBytes(), meta: { players: [{ name: 'A' }, { name: 'B' }] } }]);
        seedQueue(cm, [{ quarkid: QH, tier: 1, date: 100 }]);
        cm._preconvert.leases[QH] = { worker: 'mac', expires_at: Date.now() + 100000 };
        let r = await c.rpc({ op: 'workdone', token: TOKEN, worker: 'mac', quarkid: QH, ok: true });
        assertEq(r.ok, true, 'idempotent: already-servable workdone is a success');
        assertEq(r.already, true, 'idempotent: flagged already');
        assert(!fs.existsSync(path.join(INCOMING, QH)), 'idempotent: redundant staging wiped');
        assert(fs.existsSync(path.join(STORE, QH, 'game_0.3sr')), 'idempotent: the servable store copy is untouched');

        // Worker-reported failure → ledger, queue/lease cleared, staging wiped.
        const QF = '1700000000000-failQ';
        writeStaging(QF, [{ index: 0, bytes: goodBytes(), meta: { players: [{ name: 'A' }, { name: 'B' }] } }]);
        seedQueue(cm, [{ quarkid: QF, tier: 1, date: 100 }]);
        cm._preconvert.leases[QF] = { worker: 'mac', expires_at: Date.now() + 100000 };
        r = await c.rpc({ op: 'workdone', token: TOKEN, worker: 'mac', quarkid: QF, ok: false, reason: 'no_savestate' });
        assertEq(r.ok, true, 'worker-fail: workdone ok:false acknowledged');
        assert(!!cm._preconvert.failed[QF], 'worker-fail: recorded in the ledger');
        assertEq(cm._preconvert.failed[QF].reason, 'no_savestate', 'worker-fail: the worker-supplied reason is recorded');
        assert(!fs.existsSync(path.join(INCOMING, QF)), 'worker-fail: staging wiped');
        assert(!cm._preconvert.leases[QF], 'worker-fail: lease freed');

        // workstats is a token-gated read.
        r = await c.rpc({ op: 'workstats', token: TOKEN });
        assertEq(r.ok, true, 'workstats: authorized read succeeds');
        assert(r.queue && typeof r.queue.p1 === 'number', 'workstats: returns queue depths');
    } finally {
        await c.close();
    }
}

// --- T8: all six legacy ops unchanged for a device-shaped (no-token) client --
async function testLegacyOpsUnchanged(port, cm) {
    const STORE_Q = '1700000000000-legacyStore';
    writeStoreQuark(STORE_Q);
    const c = await makeClient(port);
    try {
        let r = await c.rpc({ op: 'search', gameid: 'sfiii3nr1' });
        assertEq(r.ok, true, 'legacy: search works with no token (catalog mode)');
        r = await c.rpc({ op: 'status' });
        assertEq(r.ok, true, 'legacy: status works with no token');
        r = await c.rpc({ op: 'get3sr', quarkid: STORE_Q, game_index: 0 });
        assertEq(r.ok, true, 'legacy: get3sr works with no token');
        r = await c.rpc({ op: 'convert', quarkid: CATALOG_QUARK });
        assertEq(r.ok, true, 'legacy: convert works with no token');
        assert(typeof r.state === 'string', 'legacy: convert returns a normal job state');
        r = await c.rpc({ op: 'convertstatus', quarkid: CATALOG_QUARK });
        assertEq(r.ok, true, 'legacy: convertstatus works with no token');
        r = await c.rpc({ op: 'watchpoll', quarkid: CATALOG_QUARK });
        assertEq(r.ok, true, 'legacy: watchpoll works with no token');
    } finally {
        await c.close();
    }
}

async function main() {
    const origLog = console.log;
    console.log = () => {};
    console.warn = () => {};
    const handle = start(0);
    const port = handle.server.address().port;
    const cm = handle._convert;
    console.log = origLog;

    let exitCode = 0;
    try {
        await testTokenGate(port, cm);
        await testLeaseLifecycle(port, cm);
        await testWorkdoneValid(port, cm);
        await testWorkdoneRejections(port, cm);
        await testWorkdoneIdempotentAndFail(port, cm);
        await testLegacyOpsUnchanged(port, cm);
    } catch (err) {
        console.error(`UNCAUGHT: ${err && err.stack ? err.stack : err}`);
        exitCode = 1;
    }
    if (failed > 0) { console.error(`${failed} assertion(s) failed`); exitCode = 1; }
    // Kill any residual mock convert procs.
    for (const job of cm._jobs.values()) { try { if (job.dlProc) job.dlProc.kill('SIGKILL'); if (job.runnerProc) job.runnerProc.kill('SIGKILL'); } catch (_) {} }
    // [task #97] Pass the real exitCode through: shutdown()'s server.close()
    // callback used to call process.exit(0) unconditionally and won the race
    // against the fallback exit below, silently discarding a failing exitCode
    // (exit-0 masking). See fcade-proxy.js's shutdown() for the mechanism.
    try { handle._shutdown && handle._shutdown('test-end', exitCode); } catch (_) {}
    try { fs.rmSync(ROOT, { recursive: true, force: true }); } catch (_) {}
    if (exitCode === 0) console.log('worklease test passed');
    // Not unref()'d: this is the last-resort guarantee that the process
    // exits with the real code even if shutdown() above never calls back
    // (e.g. server.close() hangs). Bounded at 50 ms, test-only.
    setTimeout(() => process.exit(exitCode), 50);
}

main();
