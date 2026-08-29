// Self-contained protocol test for fcade-proxy.js.
// Boots the proxy in-process on an ephemeral port with FCADE_PROXY_MOCK=1 so
// no real network call to fightcade.com ever happens here, and drives it
// with a raw TCP client speaking the length-framed JSON protocol. Wire
// encode/decode is duplicated here on purpose so the test catches framing
// bugs in either side (same rationale as rendezvous-server's test).
//
// Runtime budget: a few seconds (bounded by the deliberately-shortened
// rate-limit interval below, not by wall-clock sleeps).

'use strict';

// Make the upstream call trivially fast+deterministic and force the
// rate-limit window down to something a test can afford, all BEFORE
// requiring the module under test (constants are read once at require time).
process.env.FCADE_PROXY_MOCK = '1';
process.env.FCADE_PROXY_MIN_UPSTREAM_INTERVAL_MS = process.env.FCADE_PROXY_MIN_UPSTREAM_INTERVAL_MS || '150';
process.env.FCADE_PROXY_MAX_QUEUE_WAIT_MS = process.env.FCADE_PROXY_MAX_QUEUE_WAIT_MS || '5000';
process.env.FCADE_PROXY_IDLE_TIMEOUT_MS = process.env.FCADE_PROXY_IDLE_TIMEOUT_MS || '400';
// No FCADE_COOKIE / FCADE_COOKIE_FILE set here on purpose for the
// cookie_missing test; testCookiePresentPath sets FCADE_COOKIE itself.

const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { start } = require('./fcade-proxy.js');

// --- get3sr fixture 3sr store (Step F1b / plan-osd-replay-browser.md Stage
// S1) -- built once, before start()/require() need not race it since
// FCADE_3SR_DIR is re-read on every get3sr request (no require-time
// caching, unlike FCADE_PROXY_MOCK etc. above). ------------------------------

const THREESX_3SR_FIXTURE_DIR = fs.mkdtempSync(path.join(os.tmpdir(), 'fcade-3sr-test-'));
process.env.FCADE_3SR_DIR = THREESX_3SR_FIXTURE_DIR;

function make3srBytes(tag) {
    // Only the 4-byte magic matters to the proxy (it does not parse the full
    // header) -- pad with a distinguishing tag so different fixtures produce
    // different byte sequences the base64 round-trip test can tell apart.
    return Buffer.concat([Buffer.from('3SR1', 'ascii'), Buffer.from(`fixture-${tag}-`.repeat(8), 'ascii')]);
}

function writeQuarkFixture(quarkid, games) {
    const dir = path.join(THREESX_3SR_FIXTURE_DIR, quarkid);
    fs.mkdirSync(dir, { recursive: true });
    for (const g of games) {
        fs.writeFileSync(path.join(dir, `game_${g.index}.3sr`), g.bytes);
        if (g.meta !== undefined) {
            fs.writeFileSync(path.join(dir, `game_${g.index}.meta.json`), JSON.stringify(g.meta));
        }
    }
}

// quark-happy: 2 clean games, real names.
const HAPPY_QUARK = 'test-happy-1700000000000-0001';
const HAPPY_GAME_0_BYTES = make3srBytes('happy-0');
const HAPPY_GAME_1_BYTES = make3srBytes('happy-1');
writeQuarkFixture(HAPPY_QUARK, [
    { index: 0, bytes: HAPPY_GAME_0_BYTES, meta: { quarkid: HAPPY_QUARK, players: [{ name: 'Alice' }, { name: 'Bob' }], date: 1700000000000, duration: 120 } },
    { index: 1, bytes: HAPPY_GAME_1_BYTES, meta: { quarkid: HAPPY_QUARK, players: [{ name: 'Alice' }, { name: 'Bob' }], date: 1700000000000, duration: 90 } },
]);

// quark-mixed: one clean game (index 0), one with no sidecar at all (index
// 1) -- the nameless one must be silently dropped from a whole-manifest
// request but must be a typed error if asked for by game_index directly.
const MIXED_QUARK = 'test-mixed-1700000000001-0002';
const MIXED_GAME_0_BYTES = make3srBytes('mixed-0');
writeQuarkFixture(MIXED_QUARK, [
    { index: 0, bytes: MIXED_GAME_0_BYTES, meta: { quarkid: MIXED_QUARK, players: [{ name: 'Carol' }], date: 1700000000001, duration: 60 } },
    { index: 1, bytes: make3srBytes('mixed-1-no-sidecar') /* no meta */ },
]);

// quark-empty-players: sidecar exists but players[] is empty -- must never
// be served (review P-1.1: a nameless replay must never reach the device).
const EMPTY_PLAYERS_QUARK = 'test-emptyplayers-1700000000002-0003';
writeQuarkFixture(EMPTY_PLAYERS_QUARK, [
    { index: 0, bytes: make3srBytes('emptyplayers-0'), meta: { quarkid: EMPTY_PLAYERS_QUARK, players: [], date: 1700000000002, duration: 60 } },
]);

// The device's PROXY_MAX_FRAME_LEN (src/replay/proxy_client.c:46) — the hard
// cap above which the device rejects a whole response frame. Every per-game
// frame this test observes must stay strictly under it; that's the whole point
// of the manifest/per-game split (Stage S1 P-1).
const DEVICE_MAX_FRAME_LEN = 256 * 1024;

// quark-manyz: 8 games (>= 7, the routine sf3 case that overflowed the old
// bundle-everything response). Each game carries a distinct payload + real
// names so the manifest lists all 8 and each per-game fetch round-trips. The
// .3sr blobs are deliberately sizeable (~24 KB each) so bundling all 8 would
// have blown past the device cap, but each ONE fits a frame comfortably.
const MANY_QUARK = 'test-many-1700000000003-0004';
const MANY_GAME_BYTES = [];
{
    const manyGames = [];
    for (let i = 0; i < 8; i++) {
        // ~24 KB of distinct bytes per game (magic + repeated tagged filler).
        const body = Buffer.concat([Buffer.from('3SR1', 'ascii'), Buffer.from(`many-${i}-`.repeat(3000), 'ascii')]);
        MANY_GAME_BYTES.push(body);
        manyGames.push({
            index: i,
            bytes: body,
            meta: { quarkid: MANY_QUARK, players: [{ name: `Player${i}A` }, { name: `Player${i}B` }], date: 1700000000003 + i, duration: 100 + i },
        });
    }
    writeQuarkFixture(MANY_QUARK, manyGames);
}

// quark-oversize: a single game whose .3sr is under MAX_3SR_FILE_BYTES (1 MiB)
// on disk but whose base64 blows past the device frame cap -- the per-game
// guard must refuse it with a typed error rather than emit an over-cap frame.
const OVERSIZE_QUARK = 'test-oversize-1700000000004-0005';
const OVERSIZE_GAME_BYTES = Buffer.concat([
    Buffer.from('3SR1', 'ascii'),
    // ~300 KB -> base64 ~400 KB, well over the 256 KiB device cap.
    Buffer.from('X'.repeat(300 * 1024)),
]);
writeQuarkFixture(OVERSIZE_QUARK, [
    { index: 0, bytes: OVERSIZE_GAME_BYTES, meta: { quarkid: OVERSIZE_QUARK, players: [{ name: 'Big' }, { name: 'Payload' }], date: 1700000000004, duration: 200 } },
]);

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
                // Record the exact wire payload byte length of the frame just
                // received so tests can assert it stays under the device frame
                // cap (Stage S1 P-1 framing guard).
                client.lastFrameLen = len;
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
            lastFrameLen: 0,
            sendRaw(buffer) {
                return new Promise((res, rej) => sock.write(buffer, (err) => (err ? rej(err) : res())));
            },
            send(obj) {
                return this.sendRaw(encodeFrame(obj));
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
    const addr = handle.server.address();
    return addr.port;
}

// --- Tests --------------------------------------------------------------------

async function testCookieMissing(port) {
    // No FCADE_COOKIE / file configured yet at this point in the run.
    const c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: 'sfiii3nr1' });
        const resp = await c.recv();
        assertEq(resp.ok, false, 'cookie_missing: ok=false');
        assertEq(resp.error, 'cookie_missing', 'cookie_missing: typed error');
    } finally {
        await c.close();
    }
}

async function testStatusBeforeCookie(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'status' });
        const resp = await c.recv();
        assertEq(resp.ok, true, 'status: ok=true');
        assertEq(resp.cookie_present, false, 'status: cookie_present=false pre-cookie');
        assert(typeof resp.version === 'string', 'status: version is a string');
        assert(resp.cache && typeof resp.cache.entries === 'number', 'status: cache.entries present');
        assert(typeof resp.uptime_s === 'number', 'status: uptime_s is a number');
    } finally {
        await c.close();
    }
}

async function testSearchHappyPathAndCache(port, mockState) {
    process.env.FCADE_COOKIE = 'cf_clearance=test-cookie-value';
    const c = await makeClient(port);
    try {
        const callsBefore = mockState.calls;
        await c.send({ op: 'search', gameid: 'sfiii3nr1', offset: 0, limit: 15 });
        const resp = await c.recv();
        assertEq(resp.ok, true, 'search happy path: ok=true');
        assertEq(resp.rows.length, 2, 'search happy path: 2 rows');
        assertEq(resp.count, 2, 'search happy path: count matches rows.length');
        const row = resp.rows[0];
        assertEq(row.quarkid, '1700000000000-0001', 'row: quarkid');
        assertEq(row.duration, 185, 'row: duration');
        assertEq(row.ranked, true, 'row: ranked');
        assertEq(row.num_matches, 3, 'row: num_matches');
        assertEq(row.emulator, 'fbneo', 'row: emulator');
        assertEq(row.gameid, 'sfiii3nr1', 'row: gameid');
        assertEq(row.players.length, 2, 'row: 2 players');
        assertEq(row.players[0].name, 'Alice', 'row: player[0].name');
        assertEq(row.players[0].country, 'US', 'row: player[0].country');
        assertEq(row.players[0].rank, 'S', 'row: player[0].rank');
        assertEq(row.players[0].score, 1200, 'row: player[0].score');
        assertEq(row.internal_debug_flag, undefined, 'row: unexpected field stripped (strict subset)');
        const expectedKeys = ['quarkid', 'date', 'duration', 'players', 'ranked', 'num_matches', 'emulator', 'gameid'];
        assertEq(Object.keys(row).sort().join(','), expectedKeys.sort().join(','), 'row: exact key set');
        assertEq(mockState.calls, callsBefore + 1, 'search happy path: exactly 1 upstream call');

        // Second identical request should be served from cache: no new upstream call.
        await c.send({ op: 'search', gameid: 'sfiii3nr1', offset: 0, limit: 15 });
        const resp2 = await c.recv();
        assertEq(resp2.ok, true, 'cache hit: ok=true');
        assertEq(resp2.rows.length, 2, 'cache hit: same row count');
        assertEq(mockState.calls, callsBefore + 1, 'cache hit: upstream NOT called again');

        // status should now reflect cookie_present + at least one cache entry + a hit.
        await c.send({ op: 'status' });
        const statusResp = await c.recv();
        assertEq(statusResp.cookie_present, true, 'status: cookie_present=true once configured');
        assert(statusResp.cache.entries >= 1, 'status: cache has at least 1 entry');
        assert(statusResp.cache.hits >= 1, 'status: cache has at least 1 hit');
    } finally {
        await c.close();
    }
}

async function testCloudflare403(port, mockState) {
    const c = await makeClient(port);
    try {
        const callsBefore = mockState.calls;
        await c.send({ op: 'search', gameid: '__mock_403__' });
        const resp = await c.recv();
        assertEq(resp.ok, false, 'cloudflare_403: ok=false');
        assertEq(resp.error, 'cloudflare_403', 'cloudflare_403: typed error');
        assert(typeof resp.detail === 'string' && resp.detail.length > 0, 'cloudflare_403: has detail');
        assertEq(mockState.calls, callsBefore + 1, 'cloudflare_403: exactly 1 upstream call (no silent retry storm)');
    } finally {
        await c.close();
    }
}

async function testNetworkError(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: '__mock_network_error__' });
        const resp = await c.recv();
        assertEq(resp.ok, false, 'network error: ok=false');
        assertEq(resp.error, 'upstream_error', 'network error: typed as upstream_error');
    } finally {
        await c.close();
    }
}

async function testBadShape(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'search', gameid: '__mock_bad_shape__' });
        const resp = await c.recv();
        assertEq(resp.ok, false, 'bad upstream shape: ok=false');
        assertEq(resp.error, 'upstream_error', 'bad upstream shape: typed as upstream_error');
    } finally {
        await c.close();
    }
}

async function testBadRequestGarbageFrame(port) {
    const c = await makeClient(port);
    try {
        const garbage = Buffer.from('not json at all');
        const frame = Buffer.alloc(4 + garbage.length);
        frame.writeUInt32BE(garbage.length, 0);
        garbage.copy(frame, 4);
        await c.sendRaw(frame);
        const resp = await c.recv();
        assertEq(resp.ok, false, 'garbage frame: ok=false');
        assertEq(resp.error, 'bad_request', 'garbage frame: typed as bad_request');
    } finally {
        await c.close();
    }
}

async function testBadRequestUnknownOp(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'delete_everything' });
        const resp = await c.recv();
        assertEq(resp.ok, false, 'unknown op: ok=false');
        assertEq(resp.error, 'bad_request', 'unknown op: typed as bad_request');
    } finally {
        await c.close();
    }
}

async function testBadRequestMissingGameid(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'search' });
        const resp = await c.recv();
        assertEq(resp.ok, false, 'missing gameid: ok=false');
        assertEq(resp.error, 'bad_request', 'missing gameid: typed as bad_request');
    } finally {
        await c.close();
    }
}

async function testOversizeFrameRejectedAndCloses(port) {
    const c = await makeClient(port);
    try {
        // Claim an oversized length in the header without sending that much
        // payload — the server must reject based on the declared length
        // alone (it must not wait for/allocate the full claimed size).
        const header = Buffer.alloc(4);
        header.writeUInt32BE(64 * 1024 * 1024, 0); // 64MiB claimed, way over MAX_FRAME_BYTES
        await c.sendRaw(header);
        await c.sendRaw(Buffer.from('a little bit of payload'));
        const resp = await c.recv();
        assertEq(resp.ok, false, 'oversize frame: ok=false');
        assertEq(resp.error, 'bad_request', 'oversize frame: typed as bad_request');
        // Server should also close the connection after this.
        await new Promise((resolve) => {
            c.sock.on('close', resolve);
            setTimeout(resolve, 500);
        });
        assert(c.sock.destroyed || c.sock.readyState === 'closed' || !c.sock.writable, 'oversize frame: connection closed by server');
    } finally {
        try {
            await c.close();
        } catch (_) {
            // Already closed by server — fine.
        }
    }
}

async function testRateLimitSpacing(port, mockState) {
    // Two distinct (non-cached) search requests fired back-to-back must be
    // spaced by at least FCADE_PROXY_MIN_UPSTREAM_INTERVAL_MS at the
    // upstream call layer. We can't see the mock's internal timestamps
    // directly, so we measure wall-clock time for two sequential distinct
    // requests and assert it's at least the configured interval (minus small
    // scheduling slack).
    const intervalMs = Number(process.env.FCADE_PROXY_MIN_UPSTREAM_INTERVAL_MS);
    const c = await makeClient(port);
    try {
        const callsBefore = mockState.calls;
        const t0 = Date.now();
        await c.send({ op: 'search', gameid: 'ratelimit-test-a' });
        await c.recv();
        await c.send({ op: 'search', gameid: 'ratelimit-test-b' });
        await c.recv();
        const elapsed = Date.now() - t0;
        assertEq(mockState.calls, callsBefore + 2, 'rate-limit: both distinct requests eventually hit upstream');
        assert(elapsed >= intervalMs - 30, `rate-limit: two distinct upstream calls spaced by >= ~${intervalMs}ms (elapsed=${elapsed}ms)`);
    } finally {
        await c.close();
    }
}

// --- get3sr tests (Step F1b / plan-osd-replay-browser.md Stage S1) --------

async function testGet3srManifest(port) {
    const c = await makeClient(port);
    try {
        // MANIFEST mode (game_index omitted): metadata ONLY, no base64 blobs.
        await c.send({ op: 'get3sr', quarkid: HAPPY_QUARK });
        const resp = await c.recv();
        assertEq(resp.ok, true, 'get3sr manifest: ok=true');
        assertEq(resp.quarkid, HAPPY_QUARK, 'get3sr manifest: quarkid echoed');
        assertEq(resp.games.length, 2, 'get3sr manifest: both games listed');

        const g0 = resp.games.find((g) => g.game_index === 0);
        const g1 = resp.games.find((g) => g.game_index === 1);
        assert(g0 !== undefined && g1 !== undefined, 'get3sr manifest: both game_index 0 and 1 present');

        // Metadata present, blobs ABSENT (the whole point of the manifest).
        assertEq(g0.size, HAPPY_GAME_0_BYTES.length, 'get3sr manifest: game_0 size = on-disk .3sr size');
        assert(typeof g0.meta_size === 'number' && g0.meta_size > 0, 'get3sr manifest: game_0 meta_size present');
        assertEq(g0.b64, undefined, 'get3sr manifest: game_0 has NO b64 blob');
        assertEq(g0.meta_b64, undefined, 'get3sr manifest: game_0 has NO meta_b64 blob');
        assert(Array.isArray(g0.players), 'get3sr manifest: game_0 players[] present');
        assertEq(g0.players.join(','), 'Alice,Bob', 'get3sr manifest: game_0 real player names listed');
        assertEq(g1.players.join(','), 'Alice,Bob', 'get3sr manifest: game_1 real player names listed');

        // A manifest frame must be tiny — nowhere near the device cap.
        assert(c.lastFrameLen < DEVICE_MAX_FRAME_LEN, `get3sr manifest: frame ${c.lastFrameLen} B << device cap`);
    } finally {
        await c.close();
    }
}

async function testGet3srSingleGameIndex(port) {
    const c = await makeClient(port);
    try {
        // PER-GAME mode (game_index >= 0): exactly one game's base64 blobs.
        await c.send({ op: 'get3sr', quarkid: HAPPY_QUARK, game_index: 0 });
        const resp = await c.recv();
        assertEq(resp.ok, true, 'get3sr per-game: ok=true');
        assertEq(resp.games.length, 1, 'get3sr per-game: exactly 1 game returned');
        const g0 = resp.games[0];
        assertEq(g0.game_index, 0, 'get3sr per-game: correct game_index echoed');
        assertEq(g0.size, HAPPY_GAME_0_BYTES.length, 'get3sr per-game: game_0 size matches on-disk .3sr');
        assert(typeof g0.b64 === 'string' && g0.b64.length > 0, 'get3sr per-game: b64 present');
        assert(typeof g0.meta_b64 === 'string' && g0.meta_b64.length > 0, 'get3sr per-game: meta_b64 present');
        assertEq(g0.players, undefined, 'get3sr per-game: no manifest-only players field');

        // Base64 round-trip: must decode to the EXACT bytes on disk (P-1.1 --
        // both the .3sr payload and its meta sidecar).
        const decoded3sr = Buffer.from(g0.b64, 'base64');
        assert(decoded3sr.equals(HAPPY_GAME_0_BYTES), 'get3sr per-game: b64 decodes to exact pushed .3sr bytes');
        const decodedMeta = Buffer.from(g0.meta_b64, 'base64');
        const parsedMeta = JSON.parse(decodedMeta.toString('utf8'));
        assertEq(parsedMeta.players[0].name, 'Alice', 'get3sr per-game: meta_b64 decodes to real player name');
        assertEq(parsedMeta.players[1].name, 'Bob', 'get3sr per-game: meta_b64 decodes to real player name (p2)');

        // A different game_index returns that game's bytes.
        await c.send({ op: 'get3sr', quarkid: HAPPY_QUARK, game_index: 1 });
        const resp2 = await c.recv();
        assertEq(resp2.games[0].game_index, 1, 'get3sr per-game: game_1 requested returns game_1');
        assert(Buffer.from(resp2.games[0].b64, 'base64').equals(HAPPY_GAME_1_BYTES), 'get3sr per-game: game_1 b64 round-trips');
    } finally {
        await c.close();
    }
}

// The core P-1 regression: a >= 7-game quark must be fully retrievable via
// manifest + per-game, and NO single frame may exceed the device cap (the old
// bundle-everything response overflowed at 7+ games).
async function testGet3srManyGamesFraming(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'get3sr', quarkid: MANY_QUARK });
        const manifest = await c.recv();
        assertEq(manifest.ok, true, 'get3sr many: manifest ok=true');
        assertEq(manifest.games.length, 8, 'get3sr many: all 8 games listed in manifest');
        assert(manifest.games.every((g) => g.b64 === undefined && g.meta_b64 === undefined), 'get3sr many: manifest carries NO blobs');
        assert(c.lastFrameLen < DEVICE_MAX_FRAME_LEN, `get3sr many: manifest frame ${c.lastFrameLen} B < device cap`);

        // Fetch each game individually; every frame must stay under the cap and
        // round-trip to the exact on-disk bytes.
        const indices = manifest.games.map((g) => g.game_index).sort((a, b) => a - b);
        assertEq(indices.join(','), '0,1,2,3,4,5,6,7', 'get3sr many: manifest lists indices 0..7');
        let maxFrame = 0;
        for (const idx of indices) {
            await c.send({ op: 'get3sr', quarkid: MANY_QUARK, game_index: idx });
            const resp = await c.recv();
            assertEq(resp.ok, true, `get3sr many: game_${idx} ok=true`);
            assertEq(resp.games.length, 1, `get3sr many: game_${idx} single entry`);
            maxFrame = Math.max(maxFrame, c.lastFrameLen);
            assert(c.lastFrameLen < DEVICE_MAX_FRAME_LEN, `get3sr many: game_${idx} frame ${c.lastFrameLen} B < device cap`);
            assert(Buffer.from(resp.games[0].b64, 'base64').equals(MANY_GAME_BYTES[idx]), `get3sr many: game_${idx} b64 round-trips exactly`);
        }
        console.log(`ok - get3sr many: 8 games retrieved, largest per-game frame ${maxFrame} B (cap ${DEVICE_MAX_FRAME_LEN} B)`);
    } finally {
        await c.close();
    }
}

// The per-game frame guard: a single game too large to fit one frame must be a
// typed error, never an emitted over-cap frame.
async function testGet3srSingleGameFrameGuard(port) {
    const c = await makeClient(port);
    try {
        // It IS listed in the manifest (it's a valid, named, on-disk-sized game)...
        await c.send({ op: 'get3sr', quarkid: OVERSIZE_QUARK });
        const manifest = await c.recv();
        assertEq(manifest.ok, true, 'get3sr oversize: manifest still ok (metadata-only stays tiny)');
        assertEq(manifest.games.length, 1, 'get3sr oversize: manifest lists the game');
        assert(c.lastFrameLen < DEVICE_MAX_FRAME_LEN, 'get3sr oversize: manifest frame tiny despite huge .3sr');

        // ...but fetching its blobs would overflow the device frame -> typed error.
        await c.send({ op: 'get3sr', quarkid: OVERSIZE_QUARK, game_index: 0 });
        const resp = await c.recv();
        assertEq(resp.ok, false, 'get3sr oversize: per-game over-cap -> ok=false');
        assertEq(resp.error, 'not_found', 'get3sr oversize: over-cap game typed as not_found');
        assert(c.lastFrameLen < DEVICE_MAX_FRAME_LEN, 'get3sr oversize: the ERROR frame itself is under the cap');
    } finally {
        await c.close();
    }
}

async function testGet3srMissingQuark(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'get3sr', quarkid: 'no-such-quark-ever-published-0000' });
        const resp = await c.recv();
        assertEq(resp.ok, false, 'get3sr missing quark: ok=false');
        assertEq(resp.error, 'not_found', 'get3sr missing quark: typed as not_found');
    } finally {
        await c.close();
    }
}

async function testGet3srMixedDropsNameless(port) {
    const c = await makeClient(port);
    try {
        // Whole-manifest request: game_1 (no sidecar) must be silently
        // dropped, game_0 (has a name) must still be served.
        await c.send({ op: 'get3sr', quarkid: MIXED_QUARK });
        const resp = await c.recv();
        assertEq(resp.ok, true, 'get3sr mixed: ok=true (one good game still serves)');
        assertEq(resp.games.length, 1, 'get3sr mixed: nameless game_1 dropped, only game_0 served');
        assertEq(resp.games[0].game_index, 0, 'get3sr mixed: surviving game is game_0');

        // Asking for the nameless one BY game_index directly must be a typed
        // error, not a silent empty success (review P-1.1: never let a
        // nameless replay reach the device).
        await c.send({ op: 'get3sr', quarkid: MIXED_QUARK, game_index: 1 });
        const resp2 = await c.recv();
        assertEq(resp2.ok, false, 'get3sr mixed: explicit request for nameless game_index fails');
        assertEq(resp2.error, 'not_found', 'get3sr mixed: nameless game_index typed as not_found');
    } finally {
        await c.close();
    }
}

async function testGet3srEmptyPlayersNeverServed(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'get3sr', quarkid: EMPTY_PLAYERS_QUARK });
        const resp = await c.recv();
        assertEq(resp.ok, false, 'get3sr empty players[]: whole quark refused (its only game has no name)');
        assertEq(resp.error, 'not_found', 'get3sr empty players[]: typed as not_found');
    } finally {
        await c.close();
    }
}

async function testGet3srBadRequest(port) {
    const c = await makeClient(port);
    try {
        await c.send({ op: 'get3sr' }); // missing quarkid
        const resp = await c.recv();
        assertEq(resp.ok, false, 'get3sr bad request: missing quarkid -> ok=false');
        assertEq(resp.error, 'bad_request', 'get3sr bad request: missing quarkid typed as bad_request');

        await c.send({ op: 'get3sr', quarkid: '../../etc/passwd' });
        const resp2 = await c.recv();
        assertEq(resp2.ok, false, 'get3sr bad request: path-traversal-shaped quarkid -> ok=false');
        assertEq(resp2.error, 'bad_request', 'get3sr bad request: path-traversal-shaped quarkid typed as bad_request');

        await c.send({ op: 'get3sr', quarkid: HAPPY_QUARK, game_index: -1 });
        const resp3 = await c.recv();
        assertEq(resp3.ok, false, 'get3sr bad request: negative game_index -> ok=false');
        assertEq(resp3.error, 'bad_request', 'get3sr bad request: negative game_index typed as bad_request');
    } finally {
        await c.close();
    }
}

async function testIdleConnectionTimeout(port) {
    const idleMs = Number(process.env.FCADE_PROXY_IDLE_TIMEOUT_MS);
    const c = await makeClient(port);
    try {
        // Send nothing; the server should close us after CONN_IDLE_TIMEOUT_MS.
        await new Promise((resolve) => {
            c.sock.on('close', resolve);
            setTimeout(resolve, idleMs + 500);
        });
        assert(c.sock.destroyed, 'idle timeout: connection closed by server after inactivity');
    } finally {
        try {
            await c.close();
        } catch (_) {
            // Already closed — fine.
        }
    }
}

// --- Main ---------------------------------------------------------------------

async function main() {
    const origLog = console.log;
    const origWarn = console.warn;
    const origErr = console.error;
    // Silence info/warn logs from the server during tests; keep our own
    // ok/ASSERT FAIL lines by routing through the saved originals explicitly
    // where needed. Simplest: suppress only the server's [ts] INFO/WARN
    // pattern by intercepting console.log/warn (server uses those exclusively).
    console.log = () => {};
    console.warn = () => {};

    const handle = start(0);
    const port = getBoundPort(handle);
    const mockState = handle._mockState;

    let exitCode = 0;
    try {
        console.log = origLog; // restore so our own test output is visible
        await testCookieMissing(port);
        await testStatusBeforeCookie(port);
        await testSearchHappyPathAndCache(port, mockState);
        await testCloudflare403(port, mockState);
        await testNetworkError(port);
        await testBadShape(port);
        await testBadRequestGarbageFrame(port);
        await testBadRequestUnknownOp(port);
        await testBadRequestMissingGameid(port);
        await testOversizeFrameRejectedAndCloses(port);
        await testRateLimitSpacing(port, mockState);
        await testIdleConnectionTimeout(port);
        await testGet3srManifest(port);
        await testGet3srSingleGameIndex(port);
        await testGet3srManyGamesFraming(port);
        await testGet3srSingleGameFrameGuard(port);
        await testGet3srMissingQuark(port);
        await testGet3srMixedDropsNameless(port);
        await testGet3srEmptyPlayersNeverServed(port);
        await testGet3srBadRequest(port);
    } catch (err) {
        origErr(`UNCAUGHT: ${err && err.stack ? err.stack : err}`);
        exitCode = 1;
    } finally {
        console.log = origLog;
        console.warn = origWarn;
    }

    if (failed > 0) {
        console.error(`${failed} assertion(s) failed`);
        exitCode = 1;
    }

    try {
        // [task #97] Pass the real exitCode through: shutdown()'s
        // server.close() callback used to call process.exit(0)
        // unconditionally and won the race against the fallback exit below,
        // silently discarding a failing exitCode (exit-0 masking). See
        // fcade-proxy.js's shutdown() for the mechanism.
        handle._shutdown && handle._shutdown('test-end', exitCode);
    } catch (_) {
        // ignore
    }

    try {
        fs.rmSync(THREESX_3SR_FIXTURE_DIR, { recursive: true, force: true });
    } catch (_) {
        // best-effort cleanup only
    }

    if (exitCode === 0) {
        console.log('protocol test passed');
    }
    // Not unref()'d: this is the last-resort guarantee that the process
    // exits with the real code even if shutdown() above never calls back
    // (e.g. server.close() hangs). Bounded at 50 ms, test-only.
    setTimeout(() => process.exit(exitCode), 50);
}

main();
