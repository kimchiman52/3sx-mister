// Self-contained protocol test for fcade-proxy.js's offline catalog mode
// (FCADE_CATALOG_FILE / option B — see README.md "Offline catalog (option
// B)"). Boots the proxy in-process on an ephemeral port with a real
// catalog file on disk and FCADE_PROXY_MOCK unset, and PROVES no upstream
// call ever happens by asserting the mock's call counter never moves even
// though FCADE_PROXY_MOCK is off (the real upstream path would need a
// cookie + network access, neither present here — if catalog mode ever
// regressed into falling through to the live path, this test would hang
// or error rather than pass).
//
// Wire encode/decode is duplicated here on purpose, same rationale as
// __test_protocol.js.

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const net = require('net');

// A real catalog file must exist BEFORE fcade-proxy.js is required, same
// spirit as __test_protocol.js setting FCADE_PROXY_MOCK before requiring —
// though loadCatalog() in fcade-proxy.js actually re-reads on every call,
// so this ordering isn't strictly required for correctness, just tidy.

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'fcade-catalog-test-'));
const catalogPath = path.join(tmpDir, 'catalog.json');

const GENERATED_AT = 1784600000000;

function writeCatalog(rows, generatedAt) {
    fs.writeFileSync(
        catalogPath,
        JSON.stringify({ generated_at: generatedAt !== undefined ? generatedAt : GENERATED_AT, gameid: 'sfiii3nr1', rows }),
        'utf8',
    );
}

// 9 rows: 8 for sfiii3nr1 (varied durations, 3 catalog_best:true, 5 distinct
// players, deliberately NOT sorted by date so paging/sorting is exercised),
// 1 for a different gameid (to prove gameid filtering).
const FIXTURE_ROWS = [
    {
        quarkid: 'q-2',
        date: 1784674900000,
        duration: 245,
        players: [
            { name: 'kimchiman', country: 'US', rank: 'A', score: 1450 },
            { name: 'shadowloo99', country: 'JP', rank: 'S', score: 1600 },
        ],
        ranked: true,
        num_matches: 5,
        emulator: 'fbneo',
        gameid: 'sfiii3nr1',
        catalog_best: true,
    },
    {
        quarkid: 'q-6',
        date: 1784674817000,
        duration: 132,
        players: [{ name: 'kimchiman', country: 'US', rank: 'A', score: 1420 }],
        ranked: true,
        num_matches: 2,
        emulator: 'fbneo',
        gameid: 'sfiii3nr1',
        catalog_best: false,
        // Deliberately-unexpected field: proves catalog rows also get the
        // strict-subset normalization treatment, not just live rows.
        internal_debug_flag: true,
    },
    {
        quarkid: 'q-5',
        date: 1784674652000,
        duration: 88,
        players: [{ name: 'necroBR', country: 'BR', rank: 'A', score: 1350 }],
        ranked: false,
        num_matches: 1,
        emulator: 'fbneo',
        gameid: 'sfiii3nr1',
        catalog_best: false,
    },
    {
        quarkid: 'q-4',
        date: 1784674314000,
        duration: 310,
        players: [{ name: 'gillmain_ac', country: 'MX', rank: 'B', score: 1220 }],
        ranked: true,
        num_matches: 6,
        emulator: 'fbneo',
        gameid: 'sfiii3nr1',
        catalog_best: true,
    },
    {
        quarkid: 'q-1',
        date: 1784674205000,
        duration: 57,
        players: [{ name: 'necroBR', country: 'BR', rank: 'A', score: 1330 }],
        ranked: false,
        num_matches: 1,
        emulator: 'fbneo',
        gameid: 'sfiii3nr1',
        catalog_best: false,
    },
    {
        quarkid: 'q-3',
        date: 1784673900000,
        duration: 401,
        players: [{ name: 'shadowloo99', country: 'JP', rank: 'S', score: 1610 }],
        ranked: true,
        num_matches: 7,
        emulator: 'fbneo',
        gameid: 'sfiii3nr1',
        catalog_best: true,
    },
    {
        quarkid: 'q-7',
        date: 1784673500000,
        duration: 19,
        players: [{ name: 'urienmain', country: 'FR', rank: 'B', score: 1160 }],
        ranked: false,
        num_matches: 1,
        emulator: 'fbneo',
        gameid: 'sfiii3nr1',
        catalog_best: false,
    },
    {
        quarkid: 'q-8',
        date: 1784673100000,
        duration: 176,
        players: [{ name: 'gillmain_ac', country: 'MX', rank: 'B', score: 1200 }],
        ranked: true,
        num_matches: 3,
        emulator: 'fbneo',
        gameid: 'sfiii3nr1',
        catalog_best: false,
    },
    {
        quarkid: 'q-other',
        date: 1784672000000,
        duration: 90,
        players: [{ name: 'unrelatedplayer', country: 'DE', rank: 'C', score: 900 }],
        ranked: true,
        num_matches: 1,
        emulator: 'fbneo',
        gameid: 'otherfightcadegame',
    },
];

writeCatalog(FIXTURE_ROWS);

// --- Stage 2 ready-only fixture (docs/plan-bounded-pool-replay.md §5/§9) -----
// A store dir isolated to this test, same shape __test_store_evict.js uses:
// a quark subdir is "servable" iff it has a game_0.3sr with the 3SR1 magic
// plus a game_0.meta.json naming at least one player. Exactly 4 of the 8
// sfiii3nr1 fixture rows are made servable so the ready-only filter has a
// clear before/after signal and a real short-final-page case (see below).
const storeDir = path.join(tmpDir, '3sr');
fs.mkdirSync(storeDir, { recursive: true });
process.env.FCADE_3SR_DIR = storeDir;

function writeStoreQuark(quarkid) {
    const dir = path.join(storeDir, quarkid);
    fs.mkdirSync(dir, { recursive: true });
    const header = Buffer.alloc(28);
    header.write('3SR1', 0, 'ascii');
    fs.writeFileSync(path.join(dir, 'game_0.3sr'), header);
    fs.writeFileSync(path.join(dir, 'game_0.meta.json'), JSON.stringify({ players: [{ name: 'P1' }] }));
}

// By recency (desc) the full set is: q-2, q-6, q-5, q-4, q-1, q-3, q-7, q-8.
// Servable subset here (q-2, q-4, q-1, q-7) keeps that same relative order
// among themselves, so a limit:3 page-then-page exercises a full page
// followed by a short (1-row) final page.
const SERVABLE_QUARKIDS = ['q-2', 'q-4', 'q-1', 'q-7'];
for (const qid of SERVABLE_QUARKIDS) writeStoreQuark(qid);

process.env.FCADE_CATALOG_FILE = catalogPath;
delete process.env.FCADE_PROXY_MOCK; // explicit: prove live/mock path is never touched
delete process.env.FCADE_SEARCH_READY_ONLY; // explicit: gate starts OFF (today's default)
process.env.FCADE_PROXY_IDLE_TIMEOUT_MS = process.env.FCADE_PROXY_IDLE_TIMEOUT_MS || '2000';

const { start } = require('./fcade-proxy.js');

// --- Local frame encode/decode (duplicate of server) -------------------------

function encodeFrame(obj) {
    const json = Buffer.from(JSON.stringify(obj), 'utf8');
    const out = Buffer.alloc(4 + json.length);
    out.writeUInt32BE(json.length, 0);
    json.copy(out, 4);
    return out;
}

// --- Test plumbing ------------------------------------------------------------

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
    if (actual !== expected) {
        console.error(`ASSERT FAIL: ${msg} (actual=${JSON.stringify(actual)} expected=${JSON.stringify(expected)})`);
        failed += 1;
    } else {
        console.log(`ok - ${msg}`);
    }
}

function makeClient(port) {
    return new Promise((resolve, reject) => {
        const sock = net.createConnection({ port, host: '127.0.0.1' }, () => {
            resolve(client);
        });
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
                if (waiters.length > 0) {
                    waiters.shift()(obj);
                } else {
                    queue.push(obj);
                }
            }
        }

        sock.on('data', (chunk) => {
            buf = Buffer.concat([buf, chunk]);
            pump();
        });

        const client = {
            sock,
            send(obj) {
                return new Promise((res, rej) => sock.write(encodeFrame(obj), (err) => (err ? rej(err) : res())));
            },
            recv(timeoutMs) {
                return new Promise((res, rej) => {
                    if (queue.length > 0) {
                        res(queue.shift());
                        return;
                    }
                    const timer = setTimeout(() => rej(new Error('recv timeout')), timeoutMs || 2000);
                    waiters.push((obj) => {
                        clearTimeout(timer);
                        res(obj);
                    });
                });
            },
            close() {
                return new Promise((res) => sock.end(res));
            },
        };
    });
}

function getBoundPort(handle) {
    return handle.server.address().port;
}

const EXPECTED_ROW_KEYS = ['quarkid', 'date', 'duration', 'players', 'ranked', 'num_matches', 'emulator', 'gameid'];

// --- Tests --------------------------------------------------------------------

async function testStatusCatalogMode(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'status' });
        const resp = await c.recv();
        assertEq(resp.ok, true, 'status: ok=true');
        assertEq(resp.mode, 'catalog', 'status: mode=catalog');
        assertEq(resp.catalog_present, true, 'status: catalog_present=true');
        assertEq(resp.catalog_generated_at, GENERATED_AT, 'status: catalog_generated_at matches file');
        assertEq(resp.catalog_rows, FIXTURE_ROWS.length, 'status: catalog_rows matches fixture row count');
    } finally {
        await c.close();
    }
}

async function testRecentPagingAndShape(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: 'sfiii3nr1', offset: 0, limit: 3 });
        const page0 = await c.recv();
        assertEq(page0.ok, true, 'recent page0: ok=true');
        assertEq(page0.source, 'catalog', 'recent page0: source=catalog');
        assertEq(page0.generated_at, GENERATED_AT, 'recent page0: generated_at echoed');
        assertEq(page0.rows.length, 3, 'recent page0: 3 rows (limit)');
        assertEq(page0.count, 3, 'recent page0: count matches rows.length');
        // Recency order: q-2 (1784674900000) is the newest row for sfiii3nr1.
        assertEq(page0.rows[0].quarkid, 'q-2', 'recent page0: newest row first (recency sort)');
        assertEq(page0.rows[1].quarkid, 'q-6', 'recent page0: second-newest row second');
        assertEq(page0.rows[2].quarkid, 'q-5', 'recent page0: third-newest row third');

        const row = page0.rows[0];
        assertEq(Object.keys(row).sort().join(','), EXPECTED_ROW_KEYS.sort().join(','), 'recent page0: row key set matches normalizeRow exactly');
        assertEq(row.duration, 245, 'recent page0: duration present and correct');
        assertEq(row.players.length, 2, 'recent page0: players array present');
        assertEq(row.players[0].name, 'kimchiman', 'recent page0: player name correct');

        await c.send({ op: 'search', gameid: 'sfiii3nr1', offset: 3, limit: 3 });
        const page1 = await c.recv();
        assertEq(page1.ok, true, 'recent page1: ok=true');
        assertEq(page1.rows.length, 3, 'recent page1: 3 rows (offset paging works)');
        assertEq(page1.rows[0].quarkid, 'q-4', 'recent page1: continues where page0 left off');
        assertEq(page1.rows[1].quarkid, 'q-1', 'recent page1: order preserved across pages');
        assertEq(page1.rows[2].quarkid, 'q-3', 'recent page1: order preserved across pages');

        await c.send({ op: 'search', gameid: 'sfiii3nr1', offset: 6, limit: 3 });
        const page2 = await c.recv();
        assertEq(page2.ok, true, 'recent page2: ok=true');
        assertEq(page2.rows.length, 2, 'recent page2: last partial page has exactly 2 rows (8 total)');

        await c.send({ op: 'search', gameid: 'sfiii3nr1', offset: 8, limit: 3 });
        const page3 = await c.recv();
        assertEq(page3.ok, true, 'recent page3 (past end): ok=true, not an error');
        assertEq(page3.rows.length, 0, 'recent page3 (past end): 0 rows');
        assertEq(page3.count, 0, 'recent page3 (past end): count=0');
    } finally {
        await c.close();
    }
}

async function testUnexpectedFieldStripped(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: 'sfiii3nr1', offset: 1, limit: 1 });
        const resp = await c.recv();
        assertEq(resp.rows.length, 1, 'strip test: got the one row at offset 1');
        assertEq(resp.rows[0].quarkid, 'q-6', 'strip test: correct row (has the debug flag in the fixture)');
        assertEq(resp.rows[0].internal_debug_flag, undefined, 'strip test: catalog row unexpected field stripped by normalizeRow');
    } finally {
        await c.close();
    }
}

async function testBestFiltering(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: 'sfiii3nr1', best: true, limit: 10 });
        const resp = await c.recv();
        assertEq(resp.ok, true, 'best: ok=true');
        // Exactly 3 rows are catalog_best:true for sfiii3nr1: q-2, q-4, q-3.
        assertEq(resp.rows.length, 3, 'best: only catalog_best-tagged rows returned (hard filter)');
        const ids = resp.rows.map((r) => r.quarkid);
        assert(ids.includes('q-2') && ids.includes('q-4') && ids.includes('q-3'), 'best: exactly the 3 tagged rows present');
        assert(!ids.includes('q-1'), 'best: non-tagged row excluded');
        // Best-tagged first is trivially true since ALL returned rows are
        // best-tagged; assert recency order within that set too.
        assertEq(resp.rows[0].quarkid, 'q-2', 'best: recency order within best set (newest first)');
    } finally {
        await c.close();
    }
}

async function testUsernameFilter(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: 'sfiii3nr1', username: 'kimchi', limit: 10 });
        const resp = await c.recv();
        assertEq(resp.ok, true, 'username filter: ok=true');
        // kimchiman appears in q-2 and q-6.
        assertEq(resp.rows.length, 2, 'username filter: narrows to rows containing a matching player');
        const ids = resp.rows.map((r) => r.quarkid).sort();
        assertEq(ids.join(','), 'q-2,q-6', 'username filter: exact matching row set');

        await c.send({ op: 'search', gameid: 'sfiii3nr1', username: 'no_such_player_xyz', limit: 10 });
        const resp2 = await c.recv();
        assertEq(resp2.ok, true, 'username filter (no match): ok=true, not an error');
        assertEq(resp2.rows.length, 0, 'username filter (no match): 0 rows');
    } finally {
        await c.close();
    }
}

// Stage 2 (docs/plan-bounded-pool-replay.md §5/§9): FCADE_SEARCH_READY_ONLY
// filters username-less searches down to store-servable quarkids, before
// sort/paging, while a username search stays unfiltered/best-effort. The gate
// is read live (fcade-proxy.js searchReadyOnlyEnabled()), so it's flipped
// mid-process here rather than via a second require.
async function testReadyOnlyGate(port) {
    // (a) gate OFF (today's default, explicit here even though every test
    // above already ran with it off): unfiltered — all 8 sfiii3nr1 rows,
    // servable and not, come back.
    delete process.env.FCADE_SEARCH_READY_ONLY;
    let c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: 'sfiii3nr1', limit: 20 });
        const resp = await c.recv();
        assertEq(resp.rows.length, 8, 'ready-only gate off: unfiltered — all 8 rows returned');
        const ids = resp.rows.map((r) => r.quarkid).sort();
        assert(ids.includes('q-6') && ids.includes('q-5') && ids.includes('q-8'), 'ready-only gate off: non-servable rows still present');
    } finally {
        await c.close();
    }

    // (b) gate ON, no username: only the 4 store-servable quarkids come back,
    // still recency-sorted, and the shorter feed pages correctly — a
    // limit:3 page1 ends early with exactly 1 row (real short-final-page).
    process.env.FCADE_SEARCH_READY_ONLY = '1';
    c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: 'sfiii3nr1', offset: 0, limit: 3 });
        const page0 = await c.recv();
        assertEq(page0.ok, true, 'ready-only gate on: page0 ok=true');
        assertEq(page0.rows.length, 3, 'ready-only gate on: page0 has 3 rows (full page from the 4 servable)');
        assertEq(
            page0.rows.map((r) => r.quarkid).join(','),
            'q-2,q-4,q-1',
            'ready-only gate on: page0 is the 3 newest servable rows, recency order',
        );

        await c.send({ op: 'search', gameid: 'sfiii3nr1', offset: 3, limit: 3 });
        const page1 = await c.recv();
        assertEq(page1.ok, true, 'ready-only gate on: page1 ok=true');
        assertEq(page1.rows.length, 1, 'ready-only gate on: page1 short final page (only 4 servable total)');
        assertEq(page1.rows[0].quarkid, 'q-7', 'ready-only gate on: page1 is the last servable row');

        // Non-servable rows never appear at any offset.
        await c.send({ op: 'search', gameid: 'sfiii3nr1', offset: 0, limit: 20 });
        const all = await c.recv();
        const allIds = all.rows.map((r) => r.quarkid).sort();
        assertEq(allIds.join(','), 'q-1,q-2,q-4,q-7', 'ready-only gate on: exact servable set, non-servable rows dropped');
    } finally {
        await c.close();
    }

    // (c) gate ON + username: unconverted rows are still returned — the
    // filter is exempt for BY PLAYER searches (same 'kimchi' query as
    // testUsernameFilter: q-2 is servable, q-6 is not).
    c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: 'sfiii3nr1', username: 'kimchi', limit: 10 });
        const resp = await c.recv();
        assertEq(resp.ok, true, 'ready-only gate on + username: ok=true');
        const ids = resp.rows.map((r) => r.quarkid).sort();
        assertEq(ids.join(','), 'q-2,q-6', 'ready-only gate on + username: unconverted row (q-6) still returned, unfiltered');
    } finally {
        await c.close();
    }

    // Restore the gate to off so every later test in this file keeps seeing
    // today's unfiltered baseline (in particular testCatalogRefreshByMtime's
    // fresh single-row catalog, which has no store dir of its own).
    delete process.env.FCADE_SEARCH_READY_ONLY;
}

async function testGameidFilter(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: 'otherfightcadegame', limit: 10 });
        const resp = await c.recv();
        assertEq(resp.ok, true, 'gameid filter: ok=true');
        assertEq(resp.rows.length, 1, 'gameid filter: only the other-game row returned');
        assertEq(resp.rows[0].quarkid, 'q-other', 'gameid filter: correct row');
    } finally {
        await c.close();
    }
}

async function testEmptyResultForUnknownGameid(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: 'totally-unknown-gameid', limit: 10 });
        const resp = await c.recv();
        assertEq(resp.ok, true, 'unknown gameid: ok=true (empty state, not an error)');
        assertEq(resp.rows.length, 0, 'unknown gameid: 0 rows');
        assertEq(resp.count, 0, 'unknown gameid: count=0');
        assertEq(resp.source, 'catalog', 'unknown gameid: still reports source=catalog');
    } finally {
        await c.close();
    }
}

async function testBadRequestStillValidatedInCatalogMode(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'search' }); // missing gameid
        const resp = await c.recv();
        assertEq(resp.ok, false, 'catalog mode: missing gameid still bad_request');
        assertEq(resp.error, 'bad_request', 'catalog mode: typed error preserved');
    } finally {
        await c.close();
    }
}

async function testCatalogRefreshByMtime(port) {
    // Overwrite the catalog file with a different row set + generated_at,
    // and confirm the NEXT request picks it up with no server restart —
    // this is the "push-catalog.sh needs no restart" contract.
    const newGeneratedAt = GENERATED_AT + 999;
    writeCatalog(
        [
            {
                quarkid: 'refreshed-1',
                date: 1784680000000,
                duration: 42,
                players: [{ name: 'freshplayer', country: 'CA', rank: 'A', score: 1000 }],
                ranked: true,
                num_matches: 1,
                emulator: 'fbneo',
                gameid: 'sfiii3nr1',
                catalog_best: false,
            },
        ],
        newGeneratedAt,
    );
    // Ensure mtime actually differs (some filesystems have 1s mtime
    // resolution) — bump the mtime explicitly rather than sleeping blindly.
    const bumped = new Date(Date.now() + 2000);
    fs.utimesSync(catalogPath, bumped, bumped);

    const c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: 'sfiii3nr1', limit: 10 });
        const resp = await c.recv();
        assertEq(resp.ok, true, 'refresh: ok=true');
        assertEq(resp.rows.length, 1, 'refresh: sees the new (smaller) row set, not the old cached-in-memory one');
        assertEq(resp.rows[0].quarkid, 'refreshed-1', 'refresh: sees the new row');
        assertEq(resp.generated_at, newGeneratedAt, 'refresh: generated_at reflects the new file');

        await c.send({ op: 'status' });
        const status = await c.recv();
        assertEq(status.catalog_rows, 1, 'refresh: status reflects new row count too');
        assertEq(status.catalog_generated_at, newGeneratedAt, 'refresh: status reflects new generated_at');
    } finally {
        await c.close();
    }
}

// --- Main ---------------------------------------------------------------------

async function main() {
    const origLog = console.log;
    console.log = () => {};

    const handle = start(0);
    const port = getBoundPort(handle);
    const mockState = handle._mockState;
    const callsBefore = mockState.calls;

    let exitCode = 0;
    try {
        console.log = origLog;
        await testStatusCatalogMode(port);
        await testRecentPagingAndShape(port);
        await testUnexpectedFieldStripped(port);
        await testBestFiltering(port);
        await testUsernameFilter(port);
        await testReadyOnlyGate(port);
        await testGameidFilter(port);
        await testEmptyResultForUnknownGameid(port);
        await testBadRequestStillValidatedInCatalogMode(port);
        await testCatalogRefreshByMtime(port);

        // The single most important assertion in this whole file: no test
        // above ever caused an upstream (mock or otherwise) call. Catalog
        // mode must NEVER fall through to the live path.
        assertEq(mockState.calls, callsBefore, 'catalog mode: zero upstream/mock calls across the entire suite');
    } catch (err) {
        console.error(`UNCAUGHT: ${err && err.stack ? err.stack : err}`);
        exitCode = 1;
    }

    if (failed > 0) {
        console.error(`${failed} assertion(s) failed`);
        exitCode = 1;
    }

    try {
        handle._shutdown && handle._shutdown('test-end');
    } catch (_) {
        // ignore
    }
    try {
        fs.rmSync(tmpDir, { recursive: true, force: true });
    } catch (_) {
        // ignore
    }

    if (exitCode === 0) {
        console.log('catalog test passed');
    }
    setTimeout(() => process.exit(exitCode), 50).unref();
}

main();
