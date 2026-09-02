// Self-contained test for the fcade-proxy `preconvert` observability block
// (docs/plan-preconvert-fleet.md Stage S6 / §Q7). Boots the proxy in-process,
// drives the counters through the convert manager, and asserts:
//   - the `preconvert` block's SHAPE (queue depths, ledger counts, conversions
//     total/today/by-source, store bytes + disk free, hit ready/absent, worker
//     last-seen) appears in the `status` wire response;
//   - hit counters increment on requestStatus (ready vs absent);
//   - converted.by_vps increments on a VPS conversion and by_worker on a
//     workdone integration; worker last-seen is recorded;
//   - the daily `converted_today` rolls over at the day boundary and the
//     counters persist across a restart.
//
// Runtime budget: a couple of seconds.

'use strict';

const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');

const ROOT = fs.mkdtempSync(path.join(os.tmpdir(), 'fcade-obs-test-'));
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

const CONVERT_Q = '1700000000000-obsConvert';
fs.writeFileSync(CATALOG, JSON.stringify({ generated_at: 1, rows: [{ quarkid: CONVERT_Q, gameid: 'sfiii3nr1', date: 1, duration: 100, players: [{ name: 'A' }, { name: 'B' }] }] }));

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
process.env.FCADE_PRECONVERT_ENABLED = '0';
process.env.FCADE_PRECONVERT_TICK_MS = '600000';
process.env.FCADE_PRECONVERT_STATE_FILE = STATE_FILE;
process.env.FCADE_STORE_MAX_QUARKS = '100000';
process.env.FCADE_STORE_MAX_BYTES = String(64 * 1024 * 1024 * 1024);
process.env.FCADE_DISK_FLOOR_BYTES = '0';

const proxy = require('./fcade-proxy.js');
const { start } = proxy;

function encodeFrame(obj) {
    const json = Buffer.from(JSON.stringify(obj), 'utf8');
    const out = Buffer.alloc(4 + json.length);
    out.writeUInt32BE(json.length, 0);
    json.copy(out, 4);
    return out;
}
let failed = 0;
function assert(cond, msg) { if (!cond) { console.error(`ASSERT FAIL: ${msg}`); failed += 1; } else { console.log(`ok - ${msg}`); } }
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
                if (waiters.length > 0) waiters.shift()(obj); else q.push(obj);
            }
        });
        const client = {
            async rpc(obj) {
                await new Promise((res, rej) => sock.write(encodeFrame(obj), (e) => (e ? rej(e) : res())));
                return new Promise((res, rej) => {
                    if (q.length > 0) return res(q.shift());
                    const t = setTimeout(() => rej(new Error('recv timeout')), 2000);
                    waiters.push((o) => { clearTimeout(t); res(o); });
                });
            },
            close() { return new Promise((res) => sock.end(res)); },
        };
    });
}

function writeStoreQuark(quarkid) {
    const dir = path.join(STORE, quarkid);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'game_0.3sr'), Buffer.concat([Buffer.from('3SR1'), Buffer.alloc(60, 1)]));
    fs.writeFileSync(path.join(dir, 'game_0.meta.json'), JSON.stringify({ players: [{ name: 'P1' }, { name: 'P2' }] }));
}
function writeStaging(quarkid) {
    const dir = path.join(INCOMING, quarkid);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'game_0.3sr'), Buffer.concat([Buffer.from('3SR1'), Buffer.alloc(60, 1)]));
    fs.writeFileSync(path.join(dir, 'game_0.meta.json'), JSON.stringify({ players: [{ name: 'W1' }, { name: 'W2' }] }));
}

function assertBlockShape(pc) {
    assert(pc && typeof pc === 'object', 'shape: preconvert block present');
    assert(typeof pc.enabled === 'boolean', 'shape: enabled is a boolean');
    assert(pc.queue && ['p1', 'p2', 'p3', 'leased'].every((k) => typeof pc.queue[k] === 'number'), 'shape: queue depths (p1/p2/p3/leased)');
    assert(pc.failed && typeof pc.failed.total === 'number' && typeof pc.failed.permanent === 'number', 'shape: failed {total,permanent}');
    assert(pc.converted && ['total', 'today', 'by_vps', 'by_worker'].every((k) => typeof pc.converted[k] === 'number'), 'shape: converted {total,today,by_vps,by_worker}');
    assert(pc.store && typeof pc.store.quarks === 'number' && typeof pc.store.bytes === 'number' && typeof pc.store.disk_free_bytes === 'number', 'shape: store {quarks,bytes,disk_free_bytes}');
    assert(pc.hit && typeof pc.hit.status_ready === 'number' && typeof pc.hit.status_absent === 'number', 'shape: hit {status_ready,status_absent}');
    assert(pc.worker && 'last_seen' in pc.worker && 'last_worker' in pc.worker, 'shape: worker {last_seen,last_worker}');
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
        // Seed the queue for depth counts.
        cm._preconvert.queue = [
            { quarkid: '1700000000000-o1', tier: 1, date: 3, duration: 100, row: {} },
            { quarkid: '1700000000000-o2', tier: 1, date: 2, duration: 100, row: {} },
            { quarkid: '1700000000000-o3', tier: 2, date: 1, duration: 100, row: {} },
            { quarkid: '1700000000000-o4', tier: 3, date: 0, duration: 100, row: {} },
        ];

        // Block shape via the status WIRE response.
        const c = await makeClient(port);
        let status = await c.rpc({ op: 'status' });
        assertEq(status.ok, true, 'status: ok');
        assertBlockShape(status.preconvert);
        assertEq(status.preconvert.queue.p1, 2, 'queue: two P1');
        assertEq(status.preconvert.queue.p2, 1, 'queue: one P2');
        assertEq(status.preconvert.queue.p3, 1, 'queue: one P3');

        // hit counters: absent then ready.
        const ready0 = cm.preconvertStats().hit.status_ready;
        const absent0 = cm.preconvertStats().hit.status_absent;
        cm.requestStatus('1700000000000-notThere'); // absent
        writeStoreQuark('1700000000000-obsServable');
        cm.requestStatus('1700000000000-obsServable'); // ready
        assertEq(cm.preconvertStats().hit.status_absent, absent0 + 1, 'hit: absent incremented on a not-yet-converted browse');
        assertEq(cm.preconvertStats().hit.status_ready, ready0 + 1, 'hit: ready incremented on an instantly-playable browse');

        // converted.by_worker + worker last-seen via a workdone integration.
        writeStaging('1700000000000-obsWorker');
        cm._preconvert.queue.push({ quarkid: '1700000000000-obsWorker', tier: 1, date: 5, duration: 100, row: {} });
        cm._preconvert.leases['1700000000000-obsWorker'] = { worker: 'mac-obs', expires_at: Date.now() + 100000 };
        const wd = cm.requestWorkDone('mac-obs', '1700000000000-obsWorker', true);
        assertEq(wd.ok, true, 'workdone: integrated for the counter test');
        assertEq(cm.preconvertStats().converted.by_worker, 1, 'converted: by_worker incremented on a workdone integration');
        assertEq(cm.preconvertStats().worker.last_worker, 'mac-obs', 'worker: last_worker recorded');
        assert(cm.preconvertStats().worker.last_seen > 0, 'worker: last_seen recorded');

        // converted.by_vps via a real VPS conversion.
        const r = cm.requestConvert(CONVERT_Q);
        assertEq(r.ok, true, 'convert: accepted for the by_vps counter test');
        const deadline = Date.now() + 8000;
        while (Date.now() < deadline && cm.preconvertStats().converted.by_vps < 1) await sleep(30);
        assertEq(cm.preconvertStats().converted.by_vps, 1, 'converted: by_vps incremented on a VPS conversion');
        assertEq(cm.preconvertStats().converted.total, 2, 'converted: total = by_vps + by_worker');

        // store bytes + disk free are live numbers.
        const st = cm.preconvertStats();
        assert(st.store.quarks >= 1, 'store: quark count reflects the store');
        assert(st.store.bytes > 0, 'store: byte total > 0');
        assert(st.store.disk_free_bytes > 0, 'store: disk_free_bytes is a live measurement');

        await c.close();

        // Daily rollover: a stale day resets converted_today to 0 on next read.
        cm._preconvert.counters.today_day = Math.floor(Date.now() / (24 * 60 * 60 * 1000)) - 1;
        cm._preconvert.counters.converted_today = 9;
        const rolled = cm.preconvertStats();
        assertEq(rolled.converted.today, 0, 'rollover: converted_today resets at the day boundary');
        assert(rolled.converted.total >= 2, 'rollover: the cumulative total is NOT reset by the daily rollover');

        // Counters persist across a restart.
        cm._preconvertPersistNow();
        delete require.cache[require.resolve('./fcade-proxy.js')];
        const mod2 = require('./fcade-proxy.js');
        const cm2 = mod2.makeConvertManager();
        assertEq(cm2._preconvert.counters.converted_total, cm._preconvert.counters.converted_total, 'persist: converted_total reloaded across a restart');
        assertEq(cm2._preconvert.counters.converted_by_worker, 1, 'persist: by_worker reloaded');
        for (const q of [...cm2._jobs.keys()]) cm2._killJob(q, 'cleanup');
    } catch (err) {
        console.error(`UNCAUGHT: ${err && err.stack ? err.stack : err}`);
        exitCode = 1;
    }
    if (failed > 0) { console.error(`${failed} assertion(s) failed`); exitCode = 1; }
    for (const job of cm._jobs.values()) { try { if (job.dlProc) job.dlProc.kill('SIGKILL'); if (job.runnerProc) job.runnerProc.kill('SIGKILL'); } catch (_) {} }
    try { handle._shutdown && handle._shutdown('test-end'); } catch (_) {}
    try { fs.rmSync(ROOT, { recursive: true, force: true }); } catch (_) {}
    if (exitCode === 0) console.log('observability test passed');
    setTimeout(() => process.exit(exitCode), 50).unref();
}

main();
