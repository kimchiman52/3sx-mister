// Self-contained test for the fcade-proxy `watchpoll` op (Stage S3 /
// docs/plan-fcade-live-stream.md): the chunked 3SR-stream relay.
//
// It boots fcade-proxy.js in-process on an ephemeral port and drives it with a
// raw length-framed TCP client (same harness style as __test_protocol.js), but
// with the ggpo downloader + FBNeo `-track-3sr` runner replaced by MOCK scripts
// that reproduce, byte-for-byte, the real tracker's on-disk behavior:
//   - grow game_0.3sr INCREMENTALLY (28-byte header with placeholder
//     frame_count/checksum_count, then one 4-byte word pair appended per
//     in-game frame), and
//   - FINALIZE in place exactly like runner-track-3sr.patch's
//     Track3srFinalizeGame: append the checksum table, then fseek(20)/fseek(26)
//     to patch frame_count/checksum_count, then write track3sr_manifest.json.
// Relaying THOSE bytes byte-identically is the whole point of Stage S3, so the
// test asserts the reconstruction of every streamed chunk == the completed file
// (sha256), plus first-byte latency, throughput, multi-viewer-shares-one-job,
// late-attach-to-store, and clean mid-watch failure.
//
// Runtime budget: a few seconds (no fixed sleeps beyond short poll intervals).

'use strict';

const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');

// --- scratch dirs + env (all BEFORE requiring the module: convert constants
//     are read once at require time) -----------------------------------------
const ROOT = fs.mkdtempSync(path.join(os.tmpdir(), 'fcade-watch-test-'));
const SCRATCH = path.join(ROOT, 'convert-scratch');
const STORE = path.join(ROOT, '3sr');
const RUNNER_DIR = path.join(ROOT, 'runner');
const CATALOG = path.join(ROOT, 'catalog.json');
fs.mkdirSync(SCRATCH, { recursive: true });
fs.mkdirSync(STORE, { recursive: true });
fs.mkdirSync(path.join(RUNNER_DIR, 'roms'), { recursive: true });

// A tiny per-run counter dir so the mocks can prove one-spawn-per-quark.
const COUNT_DIR = path.join(ROOT, 'counters');
fs.mkdirSync(COUNT_DIR, { recursive: true });

// Tracker params the mock runner mirrors from the real patch.
const PREFIX_FRAMES = 20; // pre-game frames (no words) before the game-start signature
const CHECKSUM_INTERVAL = 60;

// --- Mock ggpo downloader (stands in for fcade_replay_tool.py download) ------
// Spawned as `${CONVERT_PYTHON} ${CONVERT_DOWNLOADER} download --game .. --token
// <quark>.7 --out-dir <dl> ...`. Writes <dl>/savestate then appends 10-byte
// input records to <dl>/inputs in timed batches. A quark whose token contains
// "failnostate" writes NO savestate (exercises the clean-failure path).
const MOCK_DOWNLOADER = path.join(ROOT, 'mock-downloader.js');
fs.writeFileSync(
    MOCK_DOWNLOADER,
    `'use strict';
const fs = require('fs');
const path = require('path');
function arg(name){ const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i+1] : null; }
const outDir = arg('--out-dir');
const token = arg('--token') || '';
// The kill-mid/slow quarks deliberately dribble their input slowly so a viewer
// can be caught mid-stream and the job pulled from under it (kill-mid) or
// abandoned while still holding the convert slot (preemption test).
const slow = token.indexOf('killmid') >= 0 || token.indexOf('slow') >= 0;
const records = slow ? 12000 : Number(process.env.MOCK_DL_RECORDS || '6020');
const batch = slow ? 200 : Number(process.env.MOCK_DL_BATCH || '600');
const intervalMs = slow ? 60 : Number(process.env.MOCK_DL_INTERVAL_MS || '20');
// count spawns for the one-job-per-quark proof
try { fs.appendFileSync(path.join(${JSON.stringify(COUNT_DIR)}, 'dl-' + token.replace(/[^A-Za-z0-9_.-]/g,'_')), 'x'); } catch (e) {}
if (token.indexOf('failnostate') >= 0) { process.exit(0); } // no savestate -> job fails
fs.writeFileSync(path.join(outDir, 'savestate'), Buffer.alloc(4096, 0x5a));
const inputsPath = path.join(outDir, 'inputs');
fs.writeFileSync(inputsPath, Buffer.alloc(0));
let written = 0;
function step(){
  if (written >= records) { process.exit(0); return; }
  const n = Math.min(batch, records - written);
  const buf = Buffer.alloc(n * 10);
  for (let i = 0; i < n; i++) {
    const g = written + i; // deterministic per-record content
    buf.writeUInt16LE((g * 7 + 1) & 0xffff, i*10 + 0);
    buf.writeUInt16LE((g * 13 + 3) & 0xffff, i*10 + 2);
  }
  fs.appendFileSync(inputsPath, buf);
  written += n;
  setTimeout(step, intervalMs);
}
step();
`,
);

// --- Mock FBNeo -track-3sr runner (stands in for the patched runner) ---------
// Spawned directly as CONVERT_RUNNER_BIN, so it carries a node shebang + is
// chmod +x. Tail-follows -replay-inputs, and writes -track-3sr/game_0.3sr with
// the EXACT incremental-then-finalize byte layout of runner-track-3sr.patch.
const MOCK_RUNNER = path.join(ROOT, 'mock-runner.js');
fs.writeFileSync(
    MOCK_RUNNER,
    `#!${process.execPath}
'use strict';
const fs = require('fs');
const path = require('path');
function arg(name){ const i = process.argv.indexOf(name); return i >= 0 ? process.argv[i+1] : null; }
const trackDir = arg('-track-3sr');
const inputsPath = arg('-replay-inputs');
const idleMs = Number(arg('-replay-follow-idle-ms') || '2000');
const PREFIX = ${PREFIX_FRAMES};
const INTERVAL = ${CHECKSUM_INTERVAL};
try { fs.appendFileSync(path.join(${JSON.stringify(COUNT_DIR)}, 'run-' + path.basename(path.dirname(trackDir))), 'x'); } catch (e) {}
fs.mkdirSync(trackDir, { recursive: true });
const gamePath = path.join(trackDir, 'game_0.3sr');
const cksPath = path.join(trackDir, 'game_0.cks');
let fd = null;
let cksFd = null;         // incremental-checksum side file (live-RNG-resync fix)
let cksPos = 0;
let pos = 0;              // running write offset (Node positioned writes do NOT
                         // advance the fd offset, so we track it ourselves — the
                         // real runner uses sequential fwrite + fseek for patches)
let processed = 0;        // total 10-byte records consumed
let frameCount = 0;       // in-game word frames written
let signature = false;
let recordsAtSig = 0;
const checksums = [];     // {frame, hash}

function openGame(){
  const header = Buffer.alloc(28);
  header.write('3SR1', 0, 'ascii');
  header.writeUInt16LE(1, 4);   // version
  header.writeUInt16LE(28, 6);  // header size
  header[8] = 3; header[9] = 7; // characters (arbitrary but deterministic setup)
  header[10] = 1; header[11] = 2;
  header[12] = 4; header[13] = 5;
  header[14] = 0; header[15] = 0;
  header.writeUInt16LE(0x1234, 16); // random_ix16
  header.writeUInt16LE(0x5678, 18); // random_ix32
  header.writeUInt32LE(0, 20);      // frame_count placeholder
  header.writeUInt16LE(INTERVAL, 24);
  header.writeUInt16LE(0, 26);      // checksum_count placeholder
  fd = fs.openSync(gamePath, 'w');
  fs.writeSync(fd, header, 0, 28, 0);
  pos = 28;
  cksFd = fs.openSync(cksPath, 'w'); // side file opens with the game file
  cksPos = 0;
}
function processRecord(rec){
  if (!signature) {
    if (processed < PREFIX) { return; }
    // processed === PREFIX -> the signature frame (no words)
    openGame();
    signature = true;
    recordsAtSig = processed;
    return;
  }
  const words = Buffer.alloc(4);
  rec.copy(words, 0, 0, 4); // deterministic: first 4 bytes of the record
  fs.writeSync(fd, words, 0, 4, pos);
  pos += 4;
  if (INTERVAL !== 0 && (frameCount % INTERVAL) === 0) {
    checksums.push({ frame: frameCount, hash: (frameCount * 2654435761) >>> 0 });
    // Mirror the entry into the append-only side file AS COMPUTED (the real
    // patched tracker fwrite+fflushes here) — the watchpoll relay's source.
    const e = Buffer.alloc(8);
    e.writeUInt32LE(frameCount, 0);
    e.writeUInt32LE((frameCount * 2654435761) >>> 0, 4);
    fs.writeSync(cksFd, e, 0, 8, cksPos);
    cksPos += 8;
  }
  frameCount++;
}
function finalize(reason){
  if (fd !== null) {
    for (const c of checksums) {
      const e = Buffer.alloc(8);
      e.writeUInt32LE(c.frame, 0);
      e.writeUInt32LE(c.hash, 4);
      fs.writeSync(fd, e, 0, 8, pos);
      pos += 8;
    }
    const p = Buffer.alloc(4);
    p.writeUInt32LE(frameCount, 0);
    fs.writeSync(fd, p, 0, 4, 20);       // patch frame_count (does not move pos)
    const p2 = Buffer.alloc(2);
    p2.writeUInt16LE(checksums.length, 0);
    fs.writeSync(fd, p2, 0, 2, 26);      // patch checksum_count
    fs.closeSync(fd);
    fd = null;
  }
  if (cksFd !== null) { fs.closeSync(cksFd); cksFd = null; } // side file stays on disk
  const manifest = { complete: true, games: [] };
  if (signature) {
    manifest.games.push({ game_index: 0, file: 'game_0.3sr', signature_found: true,
      stream_records_consumed_at_signature: recordsAtSig, frame_count: frameCount,
      checksum_count: checksums.length, end_reason: reason });
  }
  fs.writeFileSync(path.join(trackDir, 'track3sr_manifest.json'), JSON.stringify(manifest));
}
const donePath = inputsPath + '.done';
let idle = 0;
function pump(){
  let data = Buffer.alloc(0);
  try { data = fs.readFileSync(inputsPath); } catch (e) {}
  let advanced = false;
  while ((processed + 1) * 10 <= data.length) {
    const rec = data.subarray(processed * 10, processed * 10 + 10);
    processRecord(rec);
    processed++;
    advanced = true;
  }
  if (advanced) idle = 0;
  const doneSeen = fs.existsSync(donePath);
  if (doneSeen && (processed + 1) * 10 > data.length) {
    finalize('session-end');
    process.exit(0);
    return;
  }
  if (!advanced) { idle += 25; if (idle >= idleMs) { finalize('idle'); process.exit(0); return; } }
  setTimeout(pump, 25);
}
pump();
`,
);
fs.chmodSync(MOCK_RUNNER, 0o755);

// --- catalog with the test quarks (requestConvert needs a catalog row) -------
const GOOD_QUARK = '1700000000000-watch';
const FAIL_QUARK = '1700000000000-failnostate';
const SHARED_QUARK = '1700000000000-shared';
const KILL_QUARK = '1700000000000-killmid';
const PREEMPT_A = '1700000000000-slowpreempta'; // abandoned mid-convert ("slow" drip)
const PREEMPT_B = '1700000000000-preemptb'; // the viewer-ful request that must win the slot
fs.writeFileSync(
    CATALOG,
    JSON.stringify({
        generated_at: 1700000000000,
        rows: [
            { quarkid: GOOD_QUARK, gameid: 'sfiii3nr1', date: 1700000000000, duration: 100, players: [{ name: 'Alice' }, { name: 'Bob' }] },
            { quarkid: FAIL_QUARK, gameid: 'sfiii3nr1', date: 1700000000001, duration: 100, players: [{ name: 'Carol' }, { name: 'Dave' }] },
            { quarkid: SHARED_QUARK, gameid: 'sfiii3nr1', date: 1700000000002, duration: 100, players: [{ name: 'E' }, { name: 'F' }] },
            { quarkid: KILL_QUARK, gameid: 'sfiii3nr1', date: 1700000000003, duration: 100, players: [{ name: 'G' }, { name: 'H' }] },
            { quarkid: PREEMPT_A, gameid: 'sfiii3nr1', date: 1700000000004, duration: 100, players: [{ name: 'I' }, { name: 'J' }] },
            { quarkid: PREEMPT_B, gameid: 'sfiii3nr1', date: 1700000000005, duration: 100, players: [{ name: 'K' }, { name: 'L' }] },
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
process.env.FCADE_CONVERT_MAX_JOBS = '1';
process.env.FCADE_CONVERT_FOLLOW_IDLE_MS = '1500';
// Preemption threshold, scaled down for the test (production default 8 s).
// Must be comfortably ABOVE every active watcher's poll cadence in this file
// (watchToDone sleeps <= 25 ms; other loops <= 50 ms) so only a genuinely
// abandoned viewer's job ever crosses it.
process.env.FCADE_CONVERT_PREEMPT_STALE_MS = '400';
// Small body chunk so a modest replay spans several chunks quickly.
process.env.FCADE_WATCH_CHUNK_BYTES = '8192';
// Downloader pacing: ~6000 game frames + 20 prefix, in ~10 batches.
process.env.MOCK_DL_RECORDS = '6020';
process.env.MOCK_DL_BATCH = '650';
process.env.MOCK_DL_INTERVAL_MS = '20';

const { start } = require('./fcade-proxy.js');

const DEVICE_MAX_FRAME_LEN = 256 * 1024; // src/replay/proxy_client.c:46

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
function assertEq(actual, expected, msg) {
    assert(actual === expected, `${msg} (actual=${JSON.stringify(actual)} expected=${JSON.stringify(expected)})`);
}

function encodeFrame(obj) {
    const json = Buffer.from(JSON.stringify(obj), 'utf8');
    const out = Buffer.alloc(4 + json.length);
    out.writeUInt32BE(json.length, 0);
    json.copy(out, 4);
    return out;
}

function makeClient(port) {
    return new Promise((resolve, reject) => {
        const sock = net.createConnection({ port, host: '127.0.0.1' }, () => resolve(client));
        sock.on('error', reject);
        let buf = Buffer.alloc(0);
        const waiters = [];
        const queue = [];
        function pump() {
            for (;;) {
                if (buf.length < 4) return;
                const len = buf.readUInt32BE(0);
                if (buf.length < 4 + len) return;
                const payload = buf.subarray(4, 4 + len);
                buf = buf.subarray(4 + len);
                const obj = JSON.parse(payload.toString('utf8'));
                client.lastFrameLen = len;
                if (waiters.length > 0) waiters.shift()({ obj, len });
                else queue.push({ obj, len });
            }
        }
        sock.on('data', (chunk) => {
            buf = Buffer.concat([buf, chunk]);
            pump();
        });
        const client = {
            sock,
            lastFrameLen: 0,
            async req(obj, timeoutMs) {
                await new Promise((res, rej) => sock.write(encodeFrame(obj), (e) => (e ? rej(e) : res())));
                return new Promise((res, rej) => {
                    if (queue.length > 0) return res(queue.shift());
                    const timer = setTimeout(() => rej(new Error('recv timeout')), timeoutMs || 4000);
                    waiters.push((v) => {
                        clearTimeout(timer);
                        res(v);
                    });
                });
            },
            close() {
                return new Promise((res) => sock.end(res));
            },
        };
    });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const sha256 = (buf) => crypto.createHash('sha256').update(buf).digest('hex');

// The stream reports `done` the instant the tracker finalizes game_0 in its
// trackDir; the job then moves it to the get3sr store a tick later. Wait for
// that published copy so the byte-identity comparison targets the canonical
// served file.
async function waitForStoreFile(quarkid, gameIndex, budgetMs) {
    const p = path.join(STORE, quarkid, `game_${gameIndex}.3sr`);
    const start = Date.now();
    for (;;) {
        try {
            if (fs.statSync(p).size > 0) return p;
        } catch (_) {}
        if (Date.now() - start > (budgetMs || 4000)) throw new Error(`store file never published: ${p}`);
        await sleep(20);
    }
}

// A device-side reconstruction of the growing .3sr from the stream: overwrite
// [0,28) with the latest header each poll, splice body chunks at their offset.
// This is exactly the S4 "growing on-disk file" model.
function makeReconstructor() {
    let file = Buffer.alloc(0);
    function put(offset, bytes) {
        if (offset + bytes.length > file.length) {
            const grown = Buffer.alloc(offset + bytes.length);
            file.copy(grown, 0);
            file = grown;
        }
        bytes.copy(file, offset);
    }
    return {
        apply(resp) {
            if (resp.header_b64) put(0, Buffer.from(resp.header_b64, 'base64'));
            if (resp.b64) put(resp.from, Buffer.from(resp.b64, 'base64'));
        },
        get: () => file,
    };
}

// Drive a full watch of one quark to completion via chunked polling.
async function watchToDone(client, quarkid, opts = {}) {
    const rec = makeReconstructor();
    let from = 0;
    let cksFrom = 0; // second monotonic cursor (incremental-checksum side channel)
    let cksBuf = Buffer.alloc(0);
    let cksBeforeDone = false; // any checksum entries relayed on a non-done poll?
    let polls = 0;
    let firstByteAt = null;
    let firstBodyAt = null;
    const started = Date.now();
    let last = null;
    for (;;) {
        const { obj, len } = await client.req({ op: 'watchpoll', quarkid, from, cks_from: cksFrom });
        polls += 1;
        last = obj;
        assertEq(obj.ok, true, `${quarkid}: watchpoll ok`);
        assert(len < DEVICE_MAX_FRAME_LEN, `${quarkid}: response frame ${len} < device cap ${DEVICE_MAX_FRAME_LEN}`);
        if (obj.state === 'failed') return { obj, rec, polls, firstByteAt, firstBodyAt, cksBuf, cksBeforeDone, failed: true };
        rec.apply(obj);
        if (typeof obj.checksums_b64 === 'string' && obj.checksums_b64.length > 0) {
            if (obj.cks_from !== cksFrom) throw new Error(`${quarkid}: non-contiguous cks chunk (${obj.cks_from} != ${cksFrom})`);
            const chunk = Buffer.from(obj.checksums_b64, 'base64');
            if (chunk.length % 8 !== 0) throw new Error(`${quarkid}: cks chunk not whole entries (${chunk.length} B)`);
            cksBuf = Buffer.concat([cksBuf, chunk]);
            cksFrom = obj.cks_next;
            if (!obj.done) cksBeforeDone = true;
        }
        if (obj.header_b64 && firstByteAt === null) firstByteAt = Date.now() - started;
        if (obj.b64 && obj.b64.length > 0 && firstBodyAt === null) firstBodyAt = Date.now() - started;
        from = obj.next;
        if (obj.done) return { obj, rec, polls, firstByteAt, firstBodyAt, cksBuf, cksBeforeDone, failed: false };
        if (Date.now() - started > (opts.budgetMs || 15000)) throw new Error(`${quarkid}: watch did not finish in budget`);
        // Poll faster than the producer so we observe incremental growth.
        await sleep(obj.eof ? 25 : 1);
    }
}

// --- tests -------------------------------------------------------------------

async function testLiveWatchByteIdentity(port) {
    const c = await makeClient(port);
    try {
        const r = await watchToDone(c, GOOD_QUARK);
        assert(!r.failed, 'live watch: reached done, not failed');
        assert(r.polls >= 2, `live watch: multi-poll stream (${r.polls} polls)`);
        assert(r.firstByteAt !== null && r.firstByteAt < 4000, `live watch: first bytes at ${r.firstByteAt} ms (< 4000)`);

        // The completed store file is what get3sr would serve. Byte-identity is
        // the whole point of S3: the reconstruction of every streamed chunk must
        // equal it exactly.
        const storeFile = await waitForStoreFile(GOOD_QUARK, 0);
        assert(fs.existsSync(storeFile), 'live watch: completed game_0.3sr published to the store');
        const onDisk = fs.readFileSync(storeFile);
        const streamed = r.rec.get();
        assertEq(sha256(streamed), sha256(onDisk), 'live watch: sha256(streamed) == sha256(completed game_0.3sr)');
        assertEq(Buffer.compare(streamed, onDisk), 0, 'live watch: streamed bytes cmp-identical to file');
        assertEq(streamed.toString('ascii', 0, 4), '3SR1', 'live watch: reconstructed file has 3SR1 magic');

        // The final header must carry the PATCHED frame_count (not the streamed
        // placeholder 0) — proves the header-re-send handles the in-place patch.
        const frameCount = onDisk.readUInt32LE(20);
        assert(frameCount > 0, `live watch: final header frame_count patched (${frameCount})`);
        assertEq(streamed.readUInt32LE(20), frameCount, 'live watch: reconstructed frame_count == final (patched, not placeholder)');

        // Throughput sanity: the wire/tracker produce well ahead of 1x playback.
        // frameCount frames == frameCount/60 s of real-time playback; producing
        // + streaming them in the observed wall time must beat real-time.
        const realtimeS = frameCount / 60;
        console.log(`     (live watch: ${frameCount} frames = ${realtimeS.toFixed(1)}s real-time playback)`);

        // Incremental-checksum side channel (live-RNG-resync fix): the entries
        // relayed under the cks cursor must (a) start flowing BEFORE done —
        // that is the entire point: the device can apply Random_ix16 resyncs
        // mid-stream — and (b) be a byte-identical PREFIX of the finalized
        // file's appended checksum table (a partial prefix is expected: once
        // the game finalizes into the store the remaining entries arrive via
        // the body bytes, and the device cross-checks the streamed prefix
        // against that authoritative table at done).
        const cksCount = onDisk.readUInt16LE(26);
        assert(cksCount > 0, `live watch: finalized checksum_count > 0 (${cksCount})`);
        const tableOnDisk = onDisk.subarray(28 + frameCount * 4, 28 + frameCount * 4 + cksCount * 8);
        assert(r.cksBeforeDone, 'live watch: checksum entries streamed BEFORE done (mid-conversion)');
        assert(r.cksBuf.length > 0 && r.cksBuf.length % 8 === 0,
            `live watch: streamed a nonzero whole-entry cks prefix (${r.cksBuf.length} B of ${cksCount * 8})`);
        assert(r.cksBuf.length <= cksCount * 8, 'live watch: streamed cks prefix never exceeds the finalized table');
        assertEq(
            Buffer.compare(r.cksBuf, tableOnDisk.subarray(0, r.cksBuf.length)), 0,
            'live watch: streamed checksum entries byte-identical to the finalized table prefix',
        );
        console.log(`     (live watch: ${r.cksBuf.length / 8}/${cksCount} checksum entries arrived via the side channel pre-done)`);
    } finally {
        await c.close();
    }
}

async function testLateAttachToStore(port) {
    // GOOD_QUARK is now in the store (previous test finished it). A brand-new
    // watcher must still get the WHOLE file, attaching to the ready store, with
    // no new job/ggpo pull.
    const c = await makeClient(port);
    try {
        const r = await watchToDone(c, GOOD_QUARK, { budgetMs: 5000 });
        assert(!r.failed, 'late-attach: reached done');
        const onDisk = fs.readFileSync(await waitForStoreFile(GOOD_QUARK, 0));
        assertEq(sha256(r.rec.get()), sha256(onDisk), 'late-attach: full file byte-identical from store');
        // No second ggpo pull was spawned for the already-converted quark.
        const dlCount = countMock('dl-', GOOD_QUARK);
        assertEq(dlCount, 1, `late-attach: still exactly ONE downloader spawn total (${dlCount})`);
    } finally {
        await c.close();
    }
}

async function testMultiViewerSharesOneJob(port) {
    // Two viewers race to watch the SAME fresh quark. They must share ONE job:
    // exactly one downloader + one runner spawn, one ggpo pull.
    const QUARK = SHARED_QUARK;
    const a = await makeClient(port);
    const b = await makeClient(port);
    try {
        // Kick both off before either can finish (interleave first polls).
        const [ra, rb] = await Promise.all([a.req({ op: 'watchpoll', quarkid: QUARK, from: 0 }), b.req({ op: 'watchpoll', quarkid: QUARK, from: 0 })]);
        assertEq(ra.obj.ok, true, 'multi-viewer: viewer A first poll ok');
        assertEq(rb.obj.ok, true, 'multi-viewer: viewer B first poll ok');
        // Now let both stream to completion concurrently.
        const [wa, wb] = await Promise.all([watchToDone(a, QUARK), watchToDone(b, QUARK)]);
        assert(!wa.failed && !wb.failed, 'multi-viewer: both viewers reached done');
        const onDisk = fs.readFileSync(await waitForStoreFile(QUARK, 0));
        assertEq(sha256(wa.rec.get()), sha256(onDisk), 'multi-viewer: viewer A byte-identical');
        assertEq(sha256(wb.rec.get()), sha256(onDisk), 'multi-viewer: viewer B byte-identical');
        const dlCount = countMock('dl-', QUARK);
        const runCount = countMock('run-', QUARK);
        assertEq(dlCount, 1, `multi-viewer: exactly ONE downloader / ggpo pull for the shared quark (${dlCount})`);
        assertEq(runCount, 1, `multi-viewer: exactly ONE tracker runner for the shared quark (${runCount})`);
    } finally {
        await a.close();
        await b.close();
    }
}

async function testFailedPullCleanFail(port) {
    // A quark whose ggpo pull yields no savestate: the job fails, and a watcher
    // polling it must surface a clean terminal `failed` — never a hang, and
    // (crucially) without respawning the pull on every poll.
    const c = await makeClient(port);
    try {
        let obj = null;
        const started = Date.now();
        for (;;) {
            const r = await c.req({ op: 'watchpoll', quarkid: FAIL_QUARK, from: 0 });
            obj = r.obj;
            assertEq(obj.ok, true, 'failed-pull: watchpoll always answers (never hangs)');
            if (obj.state === 'failed') break;
            if (Date.now() - started > 8000) throw new Error('failed-pull: job never surfaced failed');
            await sleep(50);
        }
        assertEq(obj.state, 'failed', 'failed-pull: state == failed');
        assertEq(obj.done, true, 'failed-pull: done == true (terminal)');
        assert(typeof obj.detail === 'string' && obj.detail.length > 0, 'failed-pull: carries a human detail string');
        // Poll a few more times: a failed job must NOT be auto-retried (no new
        // ggpo pull respawned) and must keep reporting the terminal failure.
        const spawnsBefore = countMock('dl-', FAIL_QUARK);
        for (let i = 0; i < 5; i++) {
            const r = await c.req({ op: 'watchpoll', quarkid: FAIL_QUARK, from: 0 });
            assertEq(r.obj.state, 'failed', 'failed-pull: still terminal on repeat polls');
        }
        assertEq(countMock('dl-', FAIL_QUARK), spawnsBefore, 'failed-pull: no ggpo pull respawn on repeat polls of a failed job');
    } finally {
        await c.close();
    }
}

async function testKillMidWatch(port, convert) {
    // Genuine mid-watch kill: start watching, get some body bytes flowing, then
    // kill the job through the real failJob path (kills the ggpo pull + runner).
    // The next poll must surface a clean `failed` — never a hang or corrupt done.
    const c = await makeClient(port);
    try {
        let from = 0;
        let killed = false;
        let sawBody = false;
        let obj = null;
        const started = Date.now();
        for (;;) {
            const r = await c.req({ op: 'watchpoll', quarkid: KILL_QUARK, from });
            obj = r.obj;
            assertEq(obj.ok, true, 'kill-mid: watchpoll always answers (never hangs)');
            if (obj.b64 && obj.b64.length > 0) sawBody = true;
            from = obj.next;
            // Once the stream is genuinely mid-flight (body bytes seen, not yet
            // done), pull the plug on the job.
            if (sawBody && !obj.done && !killed) {
                const did = convert._killJob(KILL_QUARK, 'simulated mid-watch VPS death');
                assert(did, 'kill-mid: _killJob found and killed the in-flight job');
                killed = true;
            }
            if (killed && obj.state === 'failed') break;
            if (obj.done) throw new Error('kill-mid: job completed before it could be killed (tighten timing)');
            if (Date.now() - started > 10000) throw new Error('kill-mid: never surfaced failed after kill');
            await sleep(obj.eof ? 5 : 1);
        }
        assertEq(obj.state, 'failed', 'kill-mid: state == failed after kill');
        assertEq(obj.done, true, 'kill-mid: done == true (terminal)');
        assert(typeof obj.detail === 'string' && obj.detail.length > 0, 'kill-mid: carries a human detail string');
    } finally {
        await c.close();
    }
}

async function testAbandonedJobPreemption(port) {
    // The switch-replay bug (2026-07-24 TV test): viewer watches A, A's job is
    // mid-convert holding the ONLY slot, the device relaunches onto B (A's
    // viewer is gone — its process died). Pre-fix, B sat `queued` until A's
    // whole job ended (idle teardown spares a job with servable output, so a
    // long/live session blocked B indefinitely). Post-fix, B's polls preempt
    // the abandoned A within CONVERT_PREEMPT_STALE_MS and B starts in seconds.
    const a = await makeClient(port);
    const b = await makeClient(port);
    try {
        // Viewer 1: watch A (slow-drip quark) until its job holds the slot with
        // body bytes flowing, then ABANDON it (stop polling; process "died").
        let from = 0;
        const t0 = Date.now();
        for (;;) {
            const { obj } = await a.req({ op: 'watchpoll', quarkid: PREEMPT_A, from });
            assertEq(obj.ok, true, 'preempt: A watchpoll ok');
            from = obj.next;
            if (obj.b64 && obj.b64.length > 0 && !obj.done) break; // mid-convert, slot held
            if (obj.done) throw new Error('preempt: A finished before it could be abandoned (tighten timing)');
            if (Date.now() - t0 > 8000) throw new Error('preempt: A never started producing');
            await sleep(20);
        }

        // Viewer 2: request B. Its polls must (a) always answer, (b) preempt the
        // abandoned A once A's lastAccess crosses the 400 ms test threshold, and
        // (c) deliver B's first body bytes within a few seconds.
        const started = Date.now();
        let sawQueued = false;
        let sawProgressField = false;
        let bFrom = 0;
        let bObj = null;
        for (;;) {
            const { obj } = await b.req({ op: 'watchpoll', quarkid: PREEMPT_B, from: bFrom });
            bObj = obj;
            assertEq(obj.ok, true, 'preempt: B watchpoll ok (never hangs)');
            if (obj.state === 'queued') sawQueued = true;
            if (typeof obj.progress === 'number') sawProgressField = true;
            bFrom = obj.next;
            if (obj.b64 && obj.b64.length > 0) break;
            if (Date.now() - started > 6000) throw new Error(`preempt: B never got bytes (last state=${obj.state})`);
            await sleep(50);
        }
        const waitedMs = Date.now() - started;
        assert(waitedMs < 5000, `preempt: B got first bytes ${waitedMs} ms after its first poll (< 5000)`);
        assert(sawQueued, 'preempt: B was genuinely queued behind the abandoned job first');
        assert(sawProgressField, 'preempt: watchpoll responses carry a numeric progress field');
        assert(bObj.state === 'pulling' || bObj.state === 'converting' || bObj.state === 'ready',
               `preempt: B is live (state=${bObj.state})`);

        // The abandoned A job is GONE (removed outright, not failed): a status
        // probe reports `absent` (nothing in store, no tracked job), and its
        // ggpo pull was never respawned.
        const st = await b.req({ op: 'convertstatus', quarkid: PREEMPT_A });
        assertEq(st.obj.state, 'absent', 'preempt: abandoned A job removed (absent, not failed/retained)');
        assertEq(countMock('dl-', PREEMPT_A), 1, 'preempt: A ggpo pull spawned exactly once (killed, never respawned)');

        // Let B finish so it does not leak into later tests' slot accounting.
        const wb = await watchToDone(b, PREEMPT_B, { budgetMs: 20000 });
        assert(!wb.failed, 'preempt: B streamed to done after preemption');
        const onDisk = fs.readFileSync(await waitForStoreFile(PREEMPT_B, 0));
        assertEq(sha256(wb.rec.get()), sha256(onDisk), 'preempt: B byte-identical to its completed store file');
    } finally {
        await a.close();
        await b.close();
    }
}

async function testGhostReadyJobRecovers(port) {
    // Residual fix: a finished 'ready' job whose store bytes vanish — LRU
    // eviction of a finalized VOD (the eviction guard only shields NON-finished
    // jobs), or an out-of-band delete — must NOT leave watchpoll/convertstatus
    // answering 'ready' with zero bytes until the proxy restarts. The ghost job
    // has to be dropped so the next request re-converts.
    const c = await makeClient(port);
    try {
        // GOOD_QUARK is ready in the store with a lingering finished 'ready'
        // job (earlier tests finalized it; finishJob keeps the job object).
        let st = await c.req({ op: 'convertstatus', quarkid: GOOD_QUARK });
        assertEq(st.obj.state, 'ready', 'ghost: precondition — GOOD_QUARK ready in store');

        // Its bytes disappear out-of-band (LRU eviction / manual rm).
        fs.rmSync(path.join(STORE, GOOD_QUARK), { recursive: true, force: true });

        // The core regression: convertstatus must NOT still report ready — the
        // ghost is detected (no store bytes) and dropped, so state is absent.
        st = await c.req({ op: 'convertstatus', quarkid: GOOD_QUARK });
        assertEq(st.obj.state, 'absent', 'ghost: ready job with no store bytes dropped (absent, not a zero-byte ready)');

        // And a fresh watch fully RECOVERS: re-converts, streams to done,
        // byte-identical to the freshly re-published store file.
        const dlBefore = countMock('dl-', GOOD_QUARK);
        const r = await watchToDone(c, GOOD_QUARK, { budgetMs: 15000 });
        assert(!r.failed, 'ghost: fresh watch re-converts to done after the ghost was dropped');
        const onDisk = fs.readFileSync(await waitForStoreFile(GOOD_QUARK, 0));
        assertEq(sha256(r.rec.get()), sha256(onDisk), 'ghost: recovered stream byte-identical to the re-published file');
        assertEq(countMock('dl-', GOOD_QUARK), dlBefore + 1, 'ghost: exactly ONE new ggpo pull re-spawned (genuine re-convert, not stale serve)');
    } finally {
        await c.close();
    }
}

async function testBadRequests(port) {
    const c = await makeClient(port);
    try {
        let r = await c.req({ op: 'watchpoll' });
        assertEq(r.obj.error, 'bad_request', 'bad_request: missing quarkid');
        r = await c.req({ op: 'watchpoll', quarkid: '../etc/passwd' });
        assertEq(r.obj.error, 'bad_request', 'bad_request: path-traversal quarkid');
        r = await c.req({ op: 'watchpoll', quarkid: GOOD_QUARK, from: -1 });
        assertEq(r.obj.error, 'bad_request', 'bad_request: negative from');
        r = await c.req({ op: 'watchpoll', quarkid: GOOD_QUARK, game_index: -2 });
        assertEq(r.obj.error, 'bad_request', 'bad_request: negative game_index');
    } finally {
        await c.close();
    }
}

function countMock(prefix, quarkid) {
    // token used in mock filenames = `${quarkid}.7` sanitized (dl) / quarkid (run)
    let total = 0;
    for (const name of fs.readdirSync(COUNT_DIR)) {
        if (!name.startsWith(prefix)) continue;
        if (name.indexOf(quarkid) < 0) continue;
        total += fs.readFileSync(path.join(COUNT_DIR, name), 'utf8').length;
    }
    return total;
}

// --- main --------------------------------------------------------------------
async function main() {
    const origLog = console.log;
    console.log = () => {};
    console.warn = () => {};
    const handle = start(0);
    const port = handle.server.address().port;
    let exitCode = 0;
    try {
        console.log = origLog;
        await testLiveWatchByteIdentity(port);
        await testLateAttachToStore(port);
        await testGhostReadyJobRecovers(port);
        await testMultiViewerSharesOneJob(port);
        await testFailedPullCleanFail(port);
        await testKillMidWatch(port, handle._convert);
        await testAbandonedJobPreemption(port);
        await testBadRequests(port);
    } catch (err) {
        console.error(`UNCAUGHT: ${err && err.stack ? err.stack : err}`);
        exitCode = 1;
    } finally {
        console.log = origLog;
    }
    if (failed > 0) {
        console.error(`${failed} assertion(s) failed`);
        exitCode = 1;
    }
    try {
        // [task #18] Pass the real exitCode through: shutdown()'s
        // server.close() callback used to call process.exit(0)
        // unconditionally and won the race against the fallback exit below,
        // silently discarding a failing exitCode (exit-0 masking). See
        // fcade-proxy.js's shutdown() for the mechanism.
        handle._shutdown && handle._shutdown('test-end', exitCode);
    } catch (_) {}
    try {
        fs.rmSync(ROOT, { recursive: true, force: true });
    } catch (_) {}
    if (exitCode === 0) console.log('watchpoll test passed');
    // Not unref()'d: this is the last-resort guarantee that the process
    // exits with the real code even if shutdown() above never calls back
    // (e.g. server.close() hangs). Bounded at 50 ms, test-only.
    setTimeout(() => process.exit(exitCode), 50);
}

main();
