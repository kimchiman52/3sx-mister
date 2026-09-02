# 3SX fcade-proxy

## Overview

VPS-side proxy for Fightcade's `searchquarks` API (replay search/browse).
Terminates HTTPS + Cloudflare toward `fightcade.com` (the cf_clearance
cookie lives here, never on the device or in this repo) and exposes a
dead-simple length-framed JSON protocol over plain TCP so the MiSTer core
never needs TLS. See `docs/plan-fcade-replay-browser.md` §4.5 and §2.8 for
the design rationale, and `docs/fcade-replay-notes.md` §3 for background on
the Cloudflare gate.

It does NOT do matchmaking, lobby, or netplay rendezvous (that's
`../rendezvous-server/`, a separate service on a separate port).

Two ways a `.3sr` reaches the store this proxy serves via `get3sr`:

1. **Offline, Mac-side** — `tools/fcade-replays/publish_3sr.py` converts a
   batch and `push-3sr.sh` ships the blobs here (see "get3sr" below). This
   was the only path originally.
2. **On demand, server-side (Stage S2 / `docs/plan-fcade-live-stream.md`)** —
   the `convert` op pulls the ggpo replay stream and runs the FBNeo
   `-track-3sr` live tracker *on the VPS*, dropping `game_N.{3sr,meta.json}`
   into the same store so the unchanged `get3sr` path serves it. This is the
   only mode in which the proxy host runs any emulation; it is gated on
   provisioning the runner + roms + downloader (see "convert-on-select"
   below) and is otherwise a no-op. Everything the tracker emits is
   byte-identical to what `publish_3sr.py` would have produced offline.

## Wire protocol

TCP, one connection may carry many request/response pairs. Every message —
request and response — is a single frame:

```
u32be length   (byte count of the JSON payload that follows; NOT including
                these 4 bytes)
<length> bytes of UTF-8 JSON
```

Big-endian, no host-endian assumption. This is the SAME framing the core's
Fightcade stream client already speaks
(`src/replay/fcade_stream.c:253-268` `recv_frame`/`get_u32be`, itself a
byte-for-byte port of `tools/fcade-replays/fcade_replay_tool.py`'s
`recv_frame`/`_u32be_from`) — a C client can reuse that exact decode helper
for this protocol unmodified.

There is no handshake and no magic number: the first frame on a connection
is a request, the reply is the next frame back, and so on for as many
request/response pairs as the client wants to send on that connection.
Requests are strictly lock-step from the client's point of view: send one
request, wait for its response frame, then send the next. (The server
additionally serializes responses in request order, so a pipelining client
would still get in-order replies — but responses carry no correlation IDs,
so lock-step is the supported usage.)
Connections idle for `FCADE_PROXY_IDLE_TIMEOUT_MS` (default 30000ms) are
closed by the server.

### Requests (client → server)

#### `{"op": "search", "gameid": "sfiii3nr1", "offset"?: 0, "limit"?: 15, "best"?: false, "since"?: 1700000000000, "username"?: "someuser"}`

- `gameid` (string, required, 1-64 chars) — Fightcade game id, e.g.
  `"sfiii3nr1"`.
- `offset` (integer, optional, default `0`, must be `>= 0`).
- `limit` (integer, optional, default `15`, must be `1..50`). 50 is a hard
  server-side cap regardless of what's requested.
- `best` (boolean, optional, default `false`) — Fightcade's "best replays"
  filter.
- `since` (integer, optional) — ms-epoch lower bound.
- `username` (string, optional, max 64 chars) — filter by player username.

Any other shape (missing/invalid `gameid`, non-integer `offset`/`limit`,
`limit` out of range, wrong types) is rejected as `bad_request` before any
upstream call is even considered.

#### `{"op": "status"}`

No fields.

#### `{"op": "get3sr", "quarkid": "1700000000000-0001", "game_index"?: 0}`

Fetch a pre-converted, statcheck-clean `.3sr` + its `.meta.json` name
sidecar for one quark. See "get3sr" below for the full contract — this is
the one op that does NOT talk to Fightcade at all (it reads a local `.3sr`
store this proxy was pushed via `push-3sr.sh`).

- `quarkid` (string, required, 1-128 chars, `[A-Za-z0-9_-]` only) — the
  Fightcade quark id. Anything else (including any `/`, `\`, or `.`) is
  rejected as `bad_request` before touching the filesystem.
- `game_index` (integer, optional, `>= 0`) — selects the response **mode**:
  - **omitted** → **MANIFEST** mode: a metadata-only listing of every
    published game (no blobs). Tiny frame regardless of game count.
  - **`>= 0`** → **PER-GAME** mode: exactly that one game's base64 blobs.

  See "get3sr framing" under the response section for why this is split.

#### `{"op": "convert", "quarkid": "1784908283814-3519"}`

Request that this quark be converted **on the VPS** into `.3sr` blobs and
dropped into the `get3sr` store. Idempotent and non-blocking: it starts (or
attaches to) a background job and returns the job's current state
immediately — the client polls `convertstatus` for progress. See
"convert-on-select" below.

- `quarkid` (string, required, same `[A-Za-z0-9_-]{1,128}` rule as `get3sr`).

If the quark is already in the store, returns `state: "ready"` with no work.

#### `{"op": "convertstatus", "quarkid": "1784908283814-3519"}`

Poll a convert job's state + progress for the OSD "CONVERTING… n%" row.

- `quarkid` (string, required, same rule).

#### `{"op": "watchpoll", "quarkid": "1784908283814-3519", "from"?: 0, "game_index"?: 0, "cks_from"?: 0}`

Stream one in-session game's `.3sr` bytes AS THEY ARE PRODUCED by a live
convert job — the "watch it start in seconds" path (Stage S3 /
`docs/plan-fcade-live-stream.md`). Starts or ATTACHES to the same shared
convert job `convert`/`convertstatus` use (never a second ggpo pull for one
quark), then returns the file bytes appended since `from`. One request →
one response (lock-step, exactly like every other op); the client loops,
advancing `from`, until the response says `done`. See "watch (live stream
relay)" below for the full flow.

- `quarkid` (string, required, same `[A-Za-z0-9_-]{1,128}` rule as `get3sr`).
- `from` (integer, optional, default `0`, `>= 0`) — the file offset the client
  wants the next body bytes from. Pass back the previous response's `next`.
- `game_index` (integer, optional, default `0`, `>= 0`) — which in-session game
  to stream (v1 device player is one game per file).
- `cks_from` (integer, optional, default `0`, `>= 0`) — SECOND monotonic
  cursor: byte offset into the tracker's incremental-checksum side file
  (`game_N.cks`, live-RNG-resync fix). Pass back the previous response's
  `cks_next`. Server-side the offset is floored to whole 8-byte entries.

There is no other op (fixed set: `search`, `status`, `get3sr`, `convert`,
`convertstatus`, `watchpoll`). Unknown `op` values, and anything that isn't
valid JSON, are `bad_request`. This is a fixed op set talking to a fixed
upstream endpoint — the proxy never accepts a URL from the client.

### Responses (server → client)

#### Search success

```json
{
  "ok": true,
  "rows": [
    {
      "quarkid": "1700000000000-0001",
      "date": 1700000000000,
      "duration": 185,
      "players": [
        {"name": "Alice", "country": "US", "rank": "S", "score": 1200},
        {"name": "Bob", "country": "JP", "rank": "A", "score": 1100}
      ],
      "ranked": true,
      "num_matches": 3,
      "emulator": "fbneo",
      "gameid": "sfiii3nr1"
    }
  ],
  "count": 1
}
```

`rows[]` is a STRICT subset/normalization of the upstream row — exactly the
keys shown above, always in that shape, regardless of what extra fields the
upstream API adds later. Missing optional upstream fields are normalized to
`null` (player `country`/`rank`/`score`) rather than omitted, so client code
can rely on every key always being present. `count` is always
`rows.length` (kept as a separate field for client-side clarity/future
pagination metadata, not because it can ever currently disagree). This
per-row shape is IDENTICAL whether the proxy is in live mode or catalog
mode (see "Offline catalog (option B)" below) — the device never needs to
know or care which one answered it.

When the search was answered from the offline catalog (see below) rather
than a live upstream call, the response carries two additional top-level
fields the device can ignore: `"source": "catalog"` and `"generated_at":
<ms-epoch of when the catalog snippet ran>`. Live-mode responses omit both.

#### `status` success

```json
{
  "ok": true,
  "version": "0.1.0",
  "cookie_present": true,
  "cache": {"entries": 3, "hits": 5, "misses": 2},
  "uptime_s": 412,
  "catalog_present": false,
  "catalog_generated_at": null,
  "catalog_rows": 0,
  "mode": "live"
}
```

`catalog_present`/`catalog_generated_at`/`catalog_rows`/`mode` describe the
offline catalog fallback (see "Offline catalog (option B)" below).
`mode` is `"catalog"` whenever `FCADE_CATALOG_FILE` is set and currently
points at a file that exists and parses (regardless of how many rows it
has — an empty catalog is still `mode: "catalog"`), `"live"` otherwise.
When `mode` is `"catalog"`, `cookie_present`/`cache` are still reported
(for operator visibility) but are irrelevant to how `search` actually
behaves — no cookie or cache is consulted in catalog mode.

#### `get3sr` framing (why manifest + per-game)

The base64 blobs are large. Bundling **every** game of a quark into one
response frame can exceed the device's frame cap
(`PROXY_MAX_FRAME_LEN` = 256 KiB, `src/replay/proxy_client.c:46`), and the
device rejects the **whole** frame (`proxy_client.c:406-411`) — so an over-cap
response makes the replay unreachable even though each game is individually
small (~30-46 KB). A real 6-game quark base64'd to 243 KB (92.7% of the cap);
7+ games (routine for sf3) overflow.

So `get3sr` has two response shapes, selected by `game_index`:

- **MANIFEST** (`game_index` omitted) — metadata only, **no blobs**. Stays
  tiny no matter how many games the quark has. Used to enumerate a quark's
  games for browsing.
- **PER-GAME** (`game_index >= 0`) — exactly one game's blobs, hard-guarded
  against the device frame cap (see below).

The device's fetch-all path (`ProxyClient_Fetch3sr` with `game_index < 0`)
requests the MANIFEST first, then loops one PER-GAME request per listed game —
so every frame it ever receives is bounded.

##### MANIFEST success (`game_index` omitted)

```json
{
  "ok": true,
  "quarkid": "1700000000000-0001",
  "games": [
    {"game_index": 0, "size": 29788, "meta_size": 181, "players": ["Alice", "Bob"]},
    {"game_index": 1, "size": 31204, "meta_size": 181, "players": ["Alice", "Bob"]}
  ]
}
```

- One entry per published game (`game_index` ascending). NO `b64`/`meta_b64` —
  a manifest never carries blobs, so its frame is small even for a many-game
  quark.
- `players[]` is the list of non-empty player names from that game's sidecar,
  so the UI can show/enumerate rows without fetching the blobs.
- A game whose sidecar is missing, oversize, unparseable, or has no non-empty
  `players[].name` is **dropped** from the manifest (it is not servable
  per-game either — see "get3sr" validation below).

##### PER-GAME success (`game_index >= 0`)

```json
{
  "ok": true,
  "quarkid": "1700000000000-0001",
  "games": [
    {
      "game_index": 0,
      "size": 29788,
      "b64": "M1NSMQEAHAAA...",
      "meta_size": 181,
      "meta_b64": "eyJxdWFya2lkIjogIjE3MDAw..."
    }
  ]
}
```

- Always exactly one entry (the requested `game_index`).
- `b64` is base64 of the raw `.3sr` bytes read from
  `<3sr-dir>/<quarkid>/game_N.3sr` — decoding it reproduces the file
  byte-for-byte (first 4 bytes are the `3SR1` magic, `docs/3sr-format.md`).
- `meta_b64` is base64 of the raw sibling `game_N.meta.json` bytes, verbatim
  — NOT re-serialized. This is the ONLY source of player names on-device
  (`src/replay/replay_player.c:346-352`/`:651-656` parse `players[].name`
  from exactly this sidecar); a per-game response never omits it, and a game
  whose sidecar is missing or has no non-empty `players[].name` is never
  served at all (see "get3sr" below).
- **Frame guard**: if a single game's assembled frame would exceed the device
  cap (its `.3sr` is under the 1 MiB on-disk limit but its base64 still
  overflows 256 KiB), the proxy returns a typed `not_found` error rather than
  emit an over-cap frame the device would silently drop. Real `.3sr` files
  (~30-46 KB) are far under this; the guard only trips on a pathologically
  large file.

#### `convert` / `convertstatus` success

Both return the job's current state (same shape):

```json
{"ok": true, "quarkid": "1784908283814-3519", "state": "converting", "progress": 41}
```

- `state` — one of:
  - `queued` — accepted, waiting for a concurrency slot (progress 0).
  - `pulling` — downloading the ggpo stream (savestate not fully in yet).
  - `converting` — the `-track-3sr` runner is tracking the (still-downloading,
    tail-followed) stream.
  - `ready` — done; `games` lists the published `game_index`es and the blobs
    are in the store, fetchable via `get3sr`. `progress` is 100.
  - `failed` — the job stopped; `detail` says why (expired/handshake-only
    quark, no game-start signature reached, etc.). A subsequent `convert`
    retries.
  - `absent` — (`convertstatus` only) no job exists and nothing is in the
    store for this quark; issue `convert` to start one.
- `progress` — 0..100 integer estimate. During `pulling`/`converting` it
  tracks downloaded input bytes against the expected total
  (`duration × 60 fps × 10 B`); it can plateau below 100 for the runner's
  post-download tail, then jumps to 100 at `ready`.
- `games` — present only on `state: "ready"`: the published `game_index`es.
- A `convert` on an already-converted quark returns
  `{state: "ready", already: true, games: [...]}` with no work.

Job state machine: `queued → pulling → converting → ready | failed`.

#### `watchpoll` success

```json
{
  "ok": true,
  "quarkid": "1784908283814-3519",
  "game_index": 0,
  "state": "converting",
  "progress": 41,
  "size": 24796,
  "from": 12316,
  "next": 20508,
  "header_b64": "M1NSMQEAHAAD...",
  "b64": "AQIDBAUGBwgJ...",
  "eof": false,
  "done": false,
  "cks_from": 0,
  "cks_next": 64,
  "checksums_b64": "AAAAAO2W3hs..."
}
```

- `state` — the underlying convert job's state (`queued`/`pulling`/
  `converting`/`ready`/`failed`/`absent`), for a progress/status line.
- `progress` — 0..100 integer, the same estimate `convertstatus` serves (100
  when serving a finished store file). Lets the device render an honest
  "CONVERTING… n%" while it waits for the stream's first bytes instead of a
  dead "connecting" line.
- `size` — current total byte size of the game's `.3sr` (grows as the tracker
  produces it; final once the game is finalized).
- `from` — the body offset these `b64` bytes start at (always `>= 28`; the
  server clamps a `from` below the header region up to 28).
- `next` — the offset to pass as `from` on the next poll. `next == from` means
  no new body bytes were available this poll (keep polling).
- `header_b64` — base64 of the **current** 28-byte `.3sr` header. It is re-sent
  on **every** poll because the tracker patches the header's `frame_count`/
  `checksum_count` IN PLACE at finalize (`docs/3sr-format.md` §1); the client
  overwrites its local file's first 28 bytes with this each poll, so the final
  reconstruction carries the patched header, not the streamed placeholder.
- `b64` — base64 of the body bytes `[from, next)` (words, then — after finalize
  — the checksum table). Empty when no new bytes are available. The raw chunk
  is capped at `FCADE_WATCH_CHUNK_BYTES` (default 48 KiB → 64 KiB base64, well
  under the device's 256 KiB `PROXY_MAX_FRAME_LEN`, `proxy_client.c:46`).
- `cks_from`/`cks_next`/`checksums_b64` — the incremental-checksum side
  channel (live-RNG-resync fix, 2026-07-25). While a game is CONVERTING, the
  patched tracker mirrors every checksum entry it computes into an
  append-only `game_N.cks` side file (8 B LE `{frame, djb2}` per entry — the
  exact encoding of the `.3sr` table it appends at finalize). Each poll
  relays the side-file bytes from `cks_from` (whole entries only, raw chunk
  capped at `FCADE_WATCH_CKS_CHUNK_BYTES`, default 8 KiB) so the device can
  populate its in-memory checksum table DURING playback and apply the same
  `Random_ix16` resyncs finished-file playback applies — instead of
  hard-desyncing because the `.3sr`'s own table only exists after finalize.
  A store-served (finalized) watch has no side file → `checksums_b64` is
  always empty there (the table arrives in the body bytes instead); the
  client may therefore end a watch having received only a PREFIX of the
  table via this channel — that is expected, and the device cross-checks
  that prefix against the finalized table. The `.3sr` byte stream itself
  (and the finalized file) is completely unchanged by this channel.
- `eof` — `next >= size`: the client has every byte currently on disk.
- `done` — the game is **finalized** AND `eof`: the file is complete and the
  client now holds every byte. The reconstruction (header at offset 0 +
  body chunks at their offsets) is byte-identical to the completed
  `game_N.3sr` — and therefore to the offline `publish_3sr.py` `.3sr`
  (transitively via the Stage S1 tracker). A `state: "failed"` response also
  carries `done: true` (terminal) with a `detail` string.

Before the first bytes exist (job still `pulling`, or no game-start signature
reached yet) the response is `{state, size: 0, next: from, header_b64: "",
b64: "", eof: true, done: false}` — keep polling. `watchpoll` NEVER blocks; a
job that dies mid-watch surfaces `state: "failed", done: true` on the next
poll, never a hang.

#### Typed errors (any op)

```json
{"ok": false, "error": "cookie_missing", "detail": "..."}
```

| `error` | Meaning | Client guidance |
|---|---|---|
| `cookie_missing` | No cookie configured on this proxy at all (neither `FCADE_COOKIE` nor a readable cookie file). | Same as `cloudflare_403` from the player's perspective — nothing to page through. Admin action needed on the VPS. |
| `cloudflare_403` | Cloudflare/Fightcade rejected the request with HTTP 403 — the cookie exists but is expired or IP-bound elsewhere. | Render "catalog stale" (per the plan) rather than a generic error. |
| `upstream_error` | Anything else that stopped a real answer from coming back: network failure, timeout, non-200/403 HTTP status, unparseable/unexpected upstream JSON shape, or the rate-limit queue timing out (see below). | Generic "try again later." |
| `bad_request` | The request itself was malformed (bad JSON, unknown op, invalid field, oversize frame). | Client bug — fix the request. |
| `not_found` | `get3sr` only: the quark has no converted `.3sr` at all, the requested `game_index` doesn't exist, or the only game(s) present fail validation (missing/oversize/nameless — see "get3sr" below). `convert`: the quark is not in the catalog, so there is no row to drive conversion. | Render "not available for remote play" / "try again later" — this is data-availability, not a client bug. |
| `convert_unavailable` | `convert` only: on-demand conversion is disabled on this proxy (`FCADE_CONVERT_ENABLED=0`, or the runner was never provisioned). | Fall back to browsing the pre-converted pool; `get3sr` still works. |

`detail` is a free-text human-readable string for logs/debugging. Client
code must switch on `error`, never parse `detail`.

## Cookie

The Fightcade `searchquarks` API requires a browser-derived `cf_clearance`
Cloudflare cookie (see `docs/fcade-replay-notes.md` §3 — this exact cookie
cannot be obtained by an automated client). This proxy never stores or
ships a cookie in the repo. Configure it one of two ways:

1. **Env var** `FCADE_COOKIE` — the exact `Cookie:` header value to send,
   e.g. `FCADE_COOKIE='cf_clearance=AbCdEf...'`. Requires a service restart
   to change.
2. **File** (preferred for hands-on refresh) — path from `FCADE_COOKIE_FILE`
   env var, default `fcade-cookie.txt` next to `fcade-proxy.js`. Contents
   are the same `Cookie:` header value, trimmed of surrounding whitespace.
   **Re-read on every search request that needs to hit upstream** — no
   restart needed, just overwrite the file.

If neither is present/readable, every `search` request that isn't already
cached returns `{"ok": false, "error": "cookie_missing"}` immediately —
the proxy never attempts the upstream call without a cookie.

### Manual refresh flow

1. Open `fightcade.com` in a real browser, log in, load any replay search
   page so Cloudflare issues a fresh `cf_clearance`.
2. Open devtools → Application/Storage → Cookies → copy the full cookie
   string (or at minimum `cf_clearance=...`; include any other cookies the
   session sent if the plain `cf_clearance` alone starts getting 403s again).
3. On the VPS: `echo 'cf_clearance=...' > /opt/fcade-proxy/fcade-cookie.txt`
   (no restart needed — the next `search` request that misses cache picks
   it up).
4. Confirm with `{"op": "status"}` → `cookie_present: true`, then a real
   `search` request.

`cf_clearance` can be IP-bound (§3 open question 5 /
`docs/plan-fcade-replay-browser.md` §3 item 5) — if searches keep coming
back `cloudflare_403` even right after a fresh copy-paste, the cookie may
be bound to the browser's IP rather than the VPS's; the plan's documented
fallback is generating the catalog on the machine the browser cookie came
from and pushing it to the VPS as static data instead.

## Caching

In-memory only (no external cache service, no disk persistence — restart
clears it), keyed by the canonicalized request (`gameid`, `offset`, `limit`,
`best`, `since`, `username` with defaults applied). Size-capped at
`FCADE_PROXY_CACHE_MAX_ENTRIES` (default 500) with oldest-first eviction.

TTL policy (plan: "15 min for page 0, longer for `best`"):

- `offset === 0 && best !== true` → 15 minutes (`FCADE_PROXY_CACHE_TTL_FIRST_PAGE_MS`).
- Everything else (`offset > 0`, OR `best === true` at any offset) →
  60 minutes (`FCADE_PROXY_CACHE_TTL_OTHER_MS`).

Rationale for folding "best at offset 0" into the long TTL bucket rather
than a third tier: the plan's ask was specifically that `best` pages get a
*longer* TTL than the live offset-0 feed, and `best` result sets change far
less often than the live feed regardless of which page you're on — a
three-way split would add complexity with no behavioral difference for the
one case (`best` + offset 0) that's ambiguous between the two stated rules.

Cache hits never touch the upstream rate limiter or the upstream call at
all — the entire point is that most device queries never reach Fightcade.

**None of the above (cookie, cache, rate limiter) applies when the proxy is
running in catalog mode** — see "Offline catalog (option B)" below.

## Offline catalog (option B)

### Why this exists

Fightcade's `/api/` endpoint sits behind a Cloudflare **managed challenge**
("Just a moment...", `__cf_chl`). This was confirmed, in this session,
unbeatable by every scripted approach tried:

- plain `node`/`curl` with any headers,
- a real Chrome TLS fingerprint (`curl_cffi`),
- Playwright, both headless **and** headed (automation is detected either
  way).

The only thing that reliably passes is a real, everyday browser tab
already logged into `fightcade.com`. There is no cookie-refresh trick that
fixes this the way the live/cookie path (above) assumes — a managed
challenge is not a one-time clearance cookie, it can re-trigger per
request/session. So rather than fight Cloudflare, option B moves the
*generation* step into the one place that already passes: the user's own
browser.

### The flow

1. **Generate**: open `fightcade.com` in a real browser, logged in, open
   DevTools console, paste in `browser-catalog.js` (or click the
   equivalent bookmarklet built from it — see below) and run it. It pages
   through Fightcade's `searchquarks` API using the tab's own same-origin
   `fetch()` (so Cloudflare sees a normal browser request), normalizes
   every row to the exact shape below, and produces a `catalog.json`
   file — downloaded automatically and copied to the clipboard.
2. **Push**: `./push-catalog.sh <local-catalog.json> user@host:/opt/fcade-proxy`
   rsyncs it to the VPS as `catalog.json`, next to `fcade-proxy.js`.
3. **Serve**: `fcade-proxy.js` (already running, per the deployed
   `fcade-proxy.service`, which sets `FCADE_CATALOG_FILE=/opt/fcade-proxy/catalog.json`)
   picks the new file up on the **very next `search` request** — no
   restart. `search` is then answered entirely from that static file: no
   cookie is read, no cache is consulted, no upstream HTTP call to
   Fightcade is ever attempted.

### Automated refresh (`refresh-catalog.sh`)

Steps 2 and 3 above (push + confirm-live) — and even watching for step 1's
download — are automated by `refresh-catalog.sh`. The **only** step that
still requires a human is passing Cloudflare in a real browser tab (step 1
itself); everything after that is hands-off:

```
./refresh-catalog.sh
```

With no arguments this watches `$HOME/Downloads` for up to 300s. Run it,
then go click your Fightcade Catalog bookmarklet (or paste
`browser-catalog.js` into the console) in a logged-in `fightcade.com` tab.
The script:

1. polls for the newest `fcade-catalog*.json` that appears after it
   started (handling the browser's `fcade-catalog (1).json`
   duplicate-name pattern, and waiting for the download to finish writing
   before touching it),
2. validates the file's shape (non-empty `rows[]`, each row with a string
   `quarkid` and a `players` array) and refuses to push anything malformed,
3. pushes it via `push-catalog.sh`,
4. SSHes to the target host and speaks the wire protocol directly to
   `127.0.0.1:3479` to confirm `status` now reports `mode: "catalog"` with
   the row count and `generated_at` that were just pushed (the proxy only
   binds `127.0.0.1` on the VPS, so this check must run over SSH, not from
   your Mac directly),
5. prints a plain-language "LIVE: N replays, generated ..." summary.

Useful flags:

- `--downloads DIR` — where to watch (default `$HOME/Downloads`).
- `--target user@host:/path` — rsync target (default
  `hetzner-3s-arm:/opt/fcade-proxy`).
- `--timeout SECS` — how long to wait for the download (default 300).
- `--file PATH` — skip watching and validate+push an exact file right now
  (re-pushing a file you already have, or testing).

The SSH live-verify step requires SSH access to the target host (an SSH
config alias like `hetzner-3s-arm`, or any host reachable via the
`--target` spec's `user@host` — the target's `user@host` portion is what
gets SSHed to). If that SSH check can't run (host unreachable, no config
entry), the script still reports the push as done and prints the manual
`{"op":"status"}` check to run once you can reach it — a failed *reachability*
check is never treated as a failed push.

### `browser-catalog.js` — what it collects

Run from the browser console on `fightcade.com`. Fetches, for `gameid`
`sfiii3nr1` (edit the constant at the top of the file to change):

- **Best this week**, and nothing else: `best: true`, `since` = today's UTC
  midnight minus 7 days, `offset` 0, 15, 30, ... up to `MAX_ROWS`
  (default 150).

`since` is the *entire* difference between Fightcade's three Best tabs —
they all POST the same `searchquarks` request with `best: true`, and the
site's own computeds are

```js
weeklyBest:  function(){ var e=Date.now(); return e-e%864e5-6048e5 }   // midnight − 7d
monthlyBest: function(){ var e=Date.now(); return e-e%864e5-2592e6 }   // midnight − 30d
```

so the snippet uses `nowMs - (nowMs % 864e5) - 6048e5` verbatim. It used to
use `Date.UTC(y, m, 1)` — a **calendar** month, which matched *neither* tab
(the site's monthly is a rolling 30 days) and collapsed to a ~1-day window on
the 2nd of a month.

There is **no Recent (`best: false`) pass**. The device plays the weekly-best
set and nothing else, so those rows were only ever filtered back out
server-side. Dropping it could not change the best-tagged set: Recent and Best
were always two independent paged crawls and the Best call took no input from
the Recent result. One behavioural crumb survives only as a comment: when a
`quarkid` appeared in **both** feeds the old merge kept the *Recent* copy's
field values and merely flipped its `catalog_best` flag, so such a row is now
emitted with the Best copy's values instead. Measured overlap in a real
capture was zero; it was never structurally zero.

`MAX_ROWS` stays at 150, and the first 150 rows **in server order** are the
set — there is no client-side re-ranking, and the device shuffles anyway.

Paging stops early on an empty or short (`< PAGE_SIZE`) page, or a fetch
failure — whichever it collected so far is still used, with a console message
reporting how many rows it got. `results.count` is **never** read as a row
total: upstream returns `limit + 1` there as a has-more sentinel, no total
exists anywhere in the API, and exhaustion is determined only by paging until
a short or empty page. There's a small delay (`REQUEST_DELAY_MS`, default
400ms) between page fetches so as not to hammer Fightcade even from a real
browser tab — the API rate-limits with an `HTTP/3 503` carrying a **non-JSON**
body, which is why a JSON parse failure is treated as a failed page rather
than a crash. Do not remove either protection.

Rows are de-duplicated by `quarkid` (still needed with one feed: the listing
can shift under a multi-page crawl and repeat a row across a page boundary)
and every row is tagged `catalog_best: true`. Each row is
normalized to mirror `fcade-proxy.js`'s `normalizeRow()`/`normalizePlayer()`
**exactly** (same key set, same coercion rules) — there is no shared module
between a VPS-side Node service and a pasted browser snippet, so the two
implementations are kept in sync by hand; a code comment in each file
points at the other.

When it finishes, it:

- downloads `fcade-catalog.json` via the browser's normal download flow,
- copies the same JSON to the clipboard,
- logs `[fcade-catalog] COPIED N rows — paste to your assistant or save the
  downloaded fcade-catalog.json.`

Either the downloaded file or the clipboard contents is the `catalog.json`
that `push-catalog.sh` expects.

A pre-built one-line form (for use as a bookmarklet, i.e. a browser
bookmark whose URL is `javascript:...`) lives in
`browser-catalog.bookmarklet.txt`. It's mechanically generated —
`node build-bookmarklet.js` rebuilds it from `browser-catalog.js` after any
edit; don't hand-edit the `.txt`.

### Catalog file format

```json
{
  "generated_at": 1721692540000,
  "gameid": "sfiii3nr1",
  "rows": [
    {
      "quarkid": "1700000000000-0001",
      "date": 1700000000000,
      "duration": 185,
      "players": [{"name": "Alice", "country": "US", "rank": "S", "score": 1200}],
      "ranked": true,
      "num_matches": 3,
      "emulator": "fbneo",
      "gameid": "sfiii3nr1",
      "catalog_best": true
    }
  ]
}
```

The top-level `gameid` field is informational only (written by the
snippet, ignored by the proxy — a catalog file can mix rows from multiple
`gameid`s, since every row already carries its own `gameid`). Only `rows`
(an array) is required; `generated_at` is optional and surfaced via `status`
and each `search` response for operator/client visibility. Each row is
free to carry extra bookkeeping fields (like `catalog_best`) — the proxy
re-runs every catalog row through its own `normalizeRow()` before ever
returning it, so anything outside that function's known key set is
silently dropped, exactly like it is for live-mode rows.

### How the proxy serves from it (`fcade-proxy.js`)

Set `FCADE_CATALOG_FILE` to the catalog's path (the deployed
`fcade-proxy.service` sets it to `/opt/fcade-proxy/catalog.json`). On every
`search` request, the proxy:

1. Re-checks the file's mtime (re-parses only if it changed since the last
   check — a large catalog isn't re-read on every single request, but a
   fresh push takes effect on the very next request after the rsync
   completes).
2. If the file is unset, missing, or fails to parse into `{rows: [...]}`
   — falls through to the existing **live** path unchanged (cookie/cache/
   upstream, exactly as documented above). This is deliberate: a catalog
   mid-write, briefly absent, or never pushed yet must never turn into an
   error state or a hang; live mode (if a cookie happens to be configured)
   or the existing typed errors (if not) already cover "no answer right
   now" gracefully.
3. Otherwise, filters the catalog's rows by `gameid` (exact match), then by
   `username` (case-insensitive substring match against any player's
   `name`) if given, then by `since` (row `date >= since`) if given.
4. If `best: true`, restricts to rows tagged `catalog_best: true` **if
   any exist in the filtered set**; if none are tagged, falls back to the
   full filtered set rather than returning empty (a catalog built without
   ever running the best pass still serves *something* for a `best:true`
   query). This mirrors Fightcade's own "best replays" filter being a hard
   filter, not a re-ranking. Since the weekly retarget every crawled row is
   tagged, so this step is what makes `search` double as the device's
   **set manifest** — see "The weekly-best set" below.
5. Sorts by `date` descending (recency) — the only ordering signal a
   browser-derived catalog reliably carries, in both `best` and non-`best`
   modes.
6. Applies `offset`/`limit` paging to that sorted, filtered list, exactly
   like the live path's `offset`/`limit` semantics.
7. Returns `{"ok": true, "rows": [...], "count": rows.length, "source":
   "catalog", "generated_at": <catalog's generated_at or null>}`. A query
   that matches nothing (unknown `gameid`, no `username` match, etc.) still
   returns `{"ok": true, "rows": [], "count": 0, ...}` — never an error;
   catalog mode has no notion of "stale"/"403", only "matched" or "didn't."

Request validation (`bad_request` for a malformed request — missing
`gameid`, out-of-range `limit`, etc.) happens identically in both modes,
**before** the catalog-vs-live branch — a malformed request is a client
bug regardless of which backend would have answered it.

`duration`-based filtering is intentionally NOT done server-side in either
mode; the device already filters by duration client-side, so catalog rows
just need to carry a real `duration` value (they do, when the source
replay had one) for that client-side filter to work.

### Interaction with live mode

Catalog mode and live mode are mutually exclusive per request, decided
fresh every time by whether `FCADE_CATALOG_FILE` currently resolves to a
valid file: if a catalog is pushed, it's authoritative and **live mode is
never attempted**, even if a cookie also happens to be configured. To go
back to live mode, remove or rename the catalog file (or unset
`FCADE_CATALOG_FILE` and restart) — there's no separate toggle.

### Refresh cadence

Nothing here schedules a refresh for you — run it whenever the
currently-deployed catalog feels stale (new replays not showing up), same
as the cookie refresh flow it replaces. `refresh-catalog.sh` (see
"Automated refresh" above) automates everything after the browser step,
but the browser step itself — clicking the bookmarklet in a logged-in
`fightcade.com` tab — is still a one-shot, manually-triggered action; there
is no automation that clicks it for you (see `browser-catalog.js`'s header
for why that step specifically can't be scripted).

## Rate limiting toward Fightcade

Upstream calls are serialized with a minimum spacing of
`FCADE_PROXY_MIN_UPSTREAM_INTERVAL_MS` (default 2000ms) between them,
regardless of how many concurrent client connections are asking. Requests
that need an upstream call are queued, not dropped; a request queued for
longer than `FCADE_PROXY_MAX_QUEUE_WAIT_MS` (default 30000ms) gives up and
returns `upstream_error` rather than holding a connection open
indefinitely. This is a hard requirement, not a nice-to-have — treat
Fightcade's API politely or risk the cookie/IP getting blocked harder than
Cloudflare's normal challenge.

## Input hardening

- Max frame size `FCADE_PROXY_MAX_FRAME_BYTES` (default 16 KiB) — a
  declared length over this is rejected as `bad_request` and the
  connection is closed **based on the declared length alone**, before
  waiting for that much data to arrive.
- Invalid JSON in a frame → `bad_request` (connection stays open for the
  next frame).
- Unknown `op` → `bad_request`.
- Per-connection idle timeout `FCADE_PROXY_IDLE_TIMEOUT_MS` (default
  30000ms).
- Fixed op set (`search`, `status`, `get3sr`, `convert`, `convertstatus`,
  `watchpoll`) talking to a fixed, compile-time upstream URL
  (`FCADE_PROXY_API_URL`, only overridable via env for testing/ops, never from
  a client request) — this proxy can never be used to fetch an arbitrary URL.

## Run locally

```
node fcade-proxy.js
```

Binds `FCADE_PROXY_PORT` (default 3479). All tunables above are env vars
with the defaults documented next to them in `fcade-proxy.js`.

## Test

```
node __test_protocol.js
```

Boots the proxy in-process on an ephemeral port with `FCADE_PROXY_MOCK=1`
(no real network call to fightcade.com happens in this test — the mock
upstream is entirely in-process, keyed off sentinel `gameid` values:
`__mock_403__`, `__mock_network_error__`, `__mock_bad_shape__`, anything
else returns two canned rows). Exercises: cookie_missing, `status`, search
happy path + row normalization (including that an unexpected upstream field
is stripped), cache hit (asserted via mock call count staying flat),
cloudflare_403, upstream_error (network failure + malformed upstream
shape), bad_request (garbage frame, oversize frame, unknown op, missing
`gameid`), rate-limit spacing between two distinct upstream calls, and
idle-connection timeout. Exits 0 with `protocol test passed` on success.
`FCADE_CATALOG_FILE` is never set in this test, so it exercises live mode
end to end (this is also the regression check that catalog mode above
doesn't change live-mode behavior).

This file also covers `get3sr` (Stage S1): it builds a temp `.3sr` store
fixture (`FCADE_3SR_DIR` pointed at a `mkdtemp()` dir, cleaned up at the end
of the run) with a happy-path quark (2 games, real names), a mixed quark
(one clean game + one with no sidecar at all), a quark whose only game has an
empty `players[]`, an **8-game quark** (>= 7, the routine sf3 case that
overflowed the old bundle-everything frame), and an **oversize** quark whose
single `.3sr` is under the 1 MiB on-disk limit but whose base64 overflows the
device frame cap. Exercises: MANIFEST mode (metadata-only — asserts NO
`b64`/`meta_b64` blobs, all games listed with real `players[]`, frame tiny);
PER-GAME mode (exactly one game's `b64`+`meta_b64`, asserting the base64
decodes back to the EXACT bytes written to the fixture — the round-trip
guarantee); the 8-game quark fully retrieved via manifest + per-game with
**every frame asserted `< PROXY_MAX_FRAME_LEN` (256 KiB)**; the per-game frame
guard returning a typed `not_found` for the oversize game rather than an
over-cap frame; a missing-quark `not_found`; that a nameless game is silently
dropped from a manifest but is a typed `not_found` when asked for by
`game_index` directly; that a quark whose only game has empty `players[]` is
refused entirely; and `bad_request` for a missing `quarkid`, a
path-traversal-shaped `quarkid`, and a negative `game_index`.

```
node __test_watchpoll.js
```

Covers the `watchpoll` live-stream relay (Stage S3). Boots the proxy
in-process and replaces the ggpo downloader + FBNeo `-track-3sr` runner with
MOCK scripts that reproduce the real tracker's on-disk behavior byte-for-byte:
grow `game_0.3sr` incrementally (placeholder header + one 4-byte word pair per
frame), then finalize IN PLACE exactly like `runner-track-3sr.patch`
(`Track3srFinalizeGame`: append the checksum table, `fseek(20)`/`fseek(26)` to
patch `frame_count`/`checksum_count`, write `track3sr_manifest.json`). A raw
TCP client then watches to completion and asserts: **the reconstruction of
every streamed chunk is sha256/cmp-identical to the completed `game_0.3sr`**
(including the patched header, proving the header-re-send handles the in-place
patch); first bytes arrive in seconds; every response frame stays under the
256 KiB device cap; two concurrent viewers of one quark share ONE job (exactly
one downloader + one runner spawn); a late attach after conversion finished
still streams the whole file from the store; a no-savestate pull and a genuine
mid-stream kill each surface a clean terminal `state: "failed", done: true`
(never a hang), and a failed job is not respawned on repeat polls. Exits 0
with `watchpoll test passed`.

```
node __test_catalog.js
```

Same in-process-server-plus-raw-TCP-client approach, but for the offline
catalog path: writes a real fixture `catalog.json` to a temp dir, points
`FCADE_CATALOG_FILE` at it, and — critically — leaves `FCADE_PROXY_MOCK`
**unset** and asserts the mock's call counter never moves across the whole
suite, proving catalog mode never falls through to any upstream call.
Exercises: `status` reporting `mode: "catalog"` + row/`generated_at`
counts, `search` paging (`offset`/`limit` across multiple pages, including
past-the-end returning `ok:true, rows:[]`), row shape (exact key-set match
with `normalizeRow()`, including that a catalog row's own unexpected field
gets stripped the same as a live row's would), `best:true` filtering to
`catalog_best`-tagged rows, `username` substring filtering (including a
no-match case returning empty, not an error), `gameid` filtering, an
unknown-`gameid` query returning the empty state, that `bad_request`
validation still applies in catalog mode, and that overwriting the catalog
file on disk (bumping its mtime) is picked up by the very next request with
no server restart. Exits 0 with `catalog test passed` on success.

```
node __test_watchpoll.js
```

Covers the `watchpoll` live-stream relay (Stage S3) — see "watch (live stream
relay, Stage S3)" below. Exits 0 with `watchpoll test passed`.

```
node __test_hardening.js
```

Covers the Stage S6 server hardening by driving the convert manager directly
(`makeConvertManager`) with long-lived MOCK downloader/runner processes.
**Idle teardown:** an unwatched pull is torn down on the next sweep — asserted
via real `process.kill(pid, 0)` ESRCH checks that both the ggpo pull and the
`-track-3sr` runner are gone, the scratch is wiped, and nothing partial leaks
into the store; a job a viewer keeps polling (fresh `lastAccess`) survives every
sweep; and a job that has finalized a game (servable output) is spared even when
idle. **Store eviction:** LRU-by-last-served eviction to a quark-count cap and,
in a fresh re-required manager, to a byte cap; an actively-watched (live-job)
quark is spared even as the LRU; and `catalog.json`/cookie/non-quark entries in
the store root are never touched. Exits 0 with `hardening test passed`.

## Deploy

```
./deploy.sh user@host:/opt/fcade-proxy
```

Ships `fcade-proxy.js`, `fcade-proxy.service`, `package.json`, `README.md`
— **never** `fcade-cookie.txt`, which is excluded from the rsync so a
redeploy can never wipe a cookie already on the remote (the cookie is
refreshed independently per "Manual refresh flow" above, not part of a code
deploy). Then on the remote:

```
sudo useradd -r -s /usr/sbin/nologin fcade-proxy   # if it does not exist
sudo touch /opt/fcade-proxy/fcade-cookie.txt
sudo chown fcade-proxy:fcade-proxy /opt/fcade-proxy/fcade-cookie.txt
sudo chmod 600 /opt/fcade-proxy/fcade-cookie.txt
sudo cp /opt/fcade-proxy/fcade-proxy.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now fcade-proxy
```

This is server-side only — nothing here pushes to the VPS automatically;
the commands above are for the operator (this repo/agent does not have
VPS credentials and does not run them).

`deploy.sh` ships **code only** — it never touches `catalog.json` (not in
its rsync file list at all, deliberately, same spirit as excluding
`fcade-cookie.txt`). To push a generated catalog, use `push-catalog.sh`
instead — see "Offline catalog (option B)" above. The two scripts are
independent: redeploying code never wipes a pushed catalog, and pushing a
catalog never touches the running code.

## Operator notes

- Runs on port 3479 by default — does **not** touch `rendezvous-server`'s
  port 3478 or its process; they are independent systemd units that can run
  side by side on the same VPS.
- Logs: stdout (info) and stderr (warn). Use `journalctl -u fcade-proxy` on
  the deployed host.
- No persistent state beyond the cookie file. Restarting the service drops
  the in-memory cache; clients transparently re-fetch (subject to the rate
  limiter, so a cold restart under load will be slower to warm up — this is
  intentional, not a bug).
- Bind is plain TCP on all interfaces by default (`net.createServer` with
  no explicit host) — same posture as `rendezvous-server`'s `udp4` bind;
  put this behind whatever firewall rules the VPS already uses for the
  rendezvous service.

## get3sr (pre-converted `.3sr` store)

`docs/plan-osd-replay-browser.md` Stage S1: this is how a remote pick
becomes a directly-playable file instead of "NEEDS CONVERSION"
(`docs/fcade-replay-notes.md:1074,1092`). Conversion is Mac-side only, done
OFFLINE by `tools/fcade-replays/publish_3sr.py` (download → FBNeo replay
runner → `.scrd` → statcheck-gate → `make_3sr.py generate --quark-json` with
the catalog row's real `players[]`/`date`/`duration`) — this proxy never
runs any emulation itself, it only serves whatever `.3sr` + `.meta.json`
pairs were pushed to it.

### Layout

```
<FCADE_3SR_DIR>/<quarkid>/game_0.3sr
<FCADE_3SR_DIR>/<quarkid>/game_0.meta.json
<FCADE_3SR_DIR>/<quarkid>/game_1.3sr
<FCADE_3SR_DIR>/<quarkid>/game_1.meta.json
...
```

`FCADE_3SR_DIR` defaults to `<this-directory>/3sr` (i.e.
`/opt/fcade-proxy/3sr` on the deployed host), re-read fresh on every
`get3sr` request — same "no restart needed" posture as `FCADE_CATALOG_FILE`.
`game_N.meta.json` is always the sibling of `game_N.3sr` with the exact same
basename (`make_3sr.py generate`'s default `--meta-out`) — the proxy never
guesses or synthesizes a sidecar path.

### Publishing a converted quark

```bash
# 1. Convert (Mac-side, offline; needs the FBNeo runner + sfiii3nr1.zip +
#    parent sfiii3.zip ROMs — see tools/fcade-replays/publish_3sr.py header):
venv/bin/python tools/fcade-replays/publish_3sr.py \
  --catalog catalog.json --runner <fbneo-replay-runner-exe> \
  --statcheck <build-statcheck 3S-ARM binary> \
  --out-dir converted/ --quark 1700000000000-0001

# 2. Push (separate rail from push-catalog.sh -- does NOT rename anything):
./push-3sr.sh converted/ user@host:/opt/fcade-proxy
```

### Validation (why a `get3sr` response can never carry a nameless replay)

For every `game_N.3sr` found under `<3sr-dir>/<quarkid>/`, the proxy also
requires a valid sibling `game_N.meta.json` whose `players[]` has at least
one entry with a non-empty `name` — the `.3sr` binary format has NO player
name field at all (`docs/3sr-format.md`; names live only in the sidecar,
`src/replay/replay_player.c:346-352`/`:651-656`). A game that fails this
(missing/oversize `.3sr` or `.meta.json`, bad `3SR1` magic, unparsable JSON,
or empty `players[]`) is:

- silently dropped from the `games[]` array when the request asked for the
  whole quark (`game_index` omitted) — one bad game must not take down the
  others in a multi-game session, and
- a typed `not_found` error when the request asked for that exact
  `game_index` directly, or when EVERY game for the quark fails validation
  (nothing left to serve).

Blob sizes are also capped generously (`FCADE_PROXY_MAX_3SR_BYTES` default
1 MiB, `FCADE_PROXY_MAX_META_BYTES` default 64 KiB — a real match is
≈29 KB `.3sr` + a small `.meta.json`, `docs/3sr-format.md` §5) so a
corrupt/oversize file on disk is refused rather than ever base64-encoded.

### Device fetch

`src/replay/proxy_client.c`'s `ProxyClient_Fetch3sr()` writes BOTH
`<replay-root>/<quarkid>/game_N.3sr` and `game_N.meta.json` to disk — never
just the `.3sr` alone, since a sidecar-less file plays back but shows no
names (exactly the gap this stage exists to close). To keep every frame under
the device cap it uses the two-mode framing above: a single-game fetch
(`game_index >= 0`) is one PER-GAME request; a fetch-all (`game_index < 0`)
first pulls the MANIFEST, then loops one PER-GAME request per listed game,
writing each pair as it arrives. A game that errors mid-loop is skipped and
logged rather than aborting the whole set; the fetch only fails if every game
fails. On a per-game write failure the device unlinks any partial
`.3sr`/`.meta.json` it created for that game so no truncated/orphan file
lingers.

## convert-on-select (server-side conversion, Stage S2)

`docs/plan-fcade-live-stream.md` Stage S2 (ships Option B). The `convert` op
turns a catalog quark that has **no** pre-converted `.3sr` into a playable one
on demand, entirely on the VPS, so a REMOTE-tab pick becomes watchable in
≈ a fraction of the match duration with **zero device-engine change** — the
shipped player + `get3sr` fetch path are used unchanged.

### The job

Per quark, a state machine `queued → pulling → converting → ready | failed`:

1. **pulling** — spawn the repo's canonical downloader
   (`tools/fcade-replays/fcade_replay_tool.py download`) to pull the ggpo
   stream to `savestate` + `inputs` in a scratch dir.
2. **converting** — the moment the savestate + first input records land,
   spawn the FBNeo `-track-3sr` runner (the Stage S1 tracker) in
   `-replay-follow` mode so it *tail-follows the still-growing `inputs`
   file* — conversion overlaps the download rather than waiting for it. The
   tracker emits, per in-session game, exactly the bytes `make_3sr.py generate`
   would produce (setup header + per-frame input words + sparse checksums) —
   **no per-frame `.ram` dumps** (this is what sidesteps the tens-of-GB ENOSPC
   trap of the offline `-dump-ram-path` path, `docs/fcade-replay-notes.md` §4).
3. **ready** — write a `game_N.meta.json` per game from the catalog row
   (mirroring `make_3sr.py` `build_meta()`'s `--quark-json` branch — the
   sidecar is the only source of player names on-device) and **atomically**
   move each `game_N.{meta.json,3sr}` into `3sr/<quarkid>/` (meta first, then
   the `.3sr`, so `get3sr` never sees a `.3sr` without its sidecar).

**Byte-identity**: because the tracker reads the same RAM fields the offline
pipeline does, a converted game is byte-for-byte identical to what
`publish_3sr.py` produces for the same game. Verified end-to-end this way:
a VPS `convert` of `1784908283814-3519` produced a `game_0.3sr` with the same
SHA-256 as a fully independent Mac `publish_3sr.py` run of the same quark
(different machine, different download, `-track-3sr` vs `-dump-ram`→`make_3sr`).
The trailing (session-end) game of a session can differ by a frame or two —
that is pure download-tail variance (independent ggpo pulls capture slightly
different trailing frame counts on the last game), not a conversion
difference: its setup block, its input words over every shared frame, and its
checksums all match.

### Concurrency + cleanup

- **One job at a time by default** (`FCADE_CONVERT_MAX_JOBS`, clamped 1..2).
  The program has deliberately never held more than one live ggpo stream
  connection at once (politeness, notes §4); further `convert`s queue.
- Scratch is wiped on success immediately; a failed job's scratch is retained
  briefly for debugging then reclaimed by a periodic sweep, which also removes
  any orphan scratch from a crash. Scratch is tiny (savestate + inputs +
  `game_N.3sr`, ~2.5 MB/quark — never `.ram` frames).
- **Idle teardown (Stage S6).** A watch-initiated job whose viewers all walk
  away must not keep downloading + converting an unwatched replay forever. Every
  `convert`/`convertstatus`/`watchpoll` touch bumps the job's `lastAccess`; the
  same periodic sweep tears down (SIGKILLs the ggpo pull + the runner, wipes
  scratch, drops the job) any job that has been untouched for longer than
  `FCADE_CONVERT_IDLE_TEARDOWN_MS` (default 45 s) **and** is not yet `ready`
  **and** has not yet produced servable output. A job that has already finalized
  a game (manifest row with `signature_found` — "nearly done, becomes a cached
  VOD") is left to finish into the store; a job any viewer is still polling
  (fresh `lastAccess`) is never torn down. A viewer who returns after a teardown
  simply starts a fresh pull. Teardown is *not* a failure — no `failed` state,
  no retained scratch. The sweep interval is `FCADE_CONVERT_SWEEP_INTERVAL_MS`
  (default 15 s), short enough that teardown lands inside the idle window.
- **Slot preemption (switch-replay fix, 2026-07-24).** When a NEW
  `convert`/`watchpoll` request is waiting for a slot and a slot-holding job's
  viewer is GONE — its `lastAccess` is stale beyond
  `FCADE_CONVERT_PREEMPT_STALE_MS` (default 8 s; an active viewer polls every
  ~400 ms) — the abandoned job is preempted: killed, scratch wiped, slot
  freed, **even if it has already produced servable output**. A viewer-less
  job must never block a viewer-ful request; the idle-teardown "finish into
  the cached store" leniency only holds while nobody is waiting for the slot.
  Preemption is checked in the job pump, re-run on every poll of a queued
  job, so a waiting viewer starts within roughly the stale threshold. This is
  exactly the on-device flow "watch A → pick B from the OSD (game relaunches,
  A's device worker dies) → B must start in seconds, not after A's whole
  session".
- `status` reports a `convert` block: `{enabled, max_jobs, active, tracked,
  queued, idle_teardown_ms, preempt_stale_ms, store_eviction,
  store_max_quarks, store_max_bytes}`.

### The weekly-best set (what the device plays)

The catalog is no longer "a browsable feed"; it **is** the set the device's
shuffle viewer plays. There is no separate manifest op — `search` with the
best filter already answers "what is the current set?", so the device asks
that and nothing new was added to the protocol:

```json
{"op":"search","gameid":"sfiii3nr1","best":true,"since":<midnight-7d>,"offset":0,"limit":50}
```

paged at `offset` 0 / 50 / 100 (`limit` 50 is `MAX_LIMIT`, and matches the
device's `RP_MAX_ROWS` row buffer in `replay_proxy.h`), which covers the whole
`MAX_ROWS = 150` set in three requests. `searchCatalog()`'s hard
`catalog_best === true` filter is what makes the answer the set rather than a
feed slice, and `count` in the reply is the length of the page returned —
never a total.

Two things to know about the reply:

- **`since` is applied to the catalog too.** `searchCatalog()` also filters
  rows by `date >= since`. Both sides floor to UTC midnight, so they agree
  except across a midnight boundary: a device asking after 00:00 UTC against a
  catalog crawled before it computes a window one day newer than the one that
  was crawled, and that oldest day's rows drop out of the answer until the next
  crawl. The crawl LaunchAgent runs every 3 h
  (`stealth-catalog/dev.sambae.fcade-catalog.plist`), so the exposure is at
  most one 3 h slot. It shrinks the set slightly; it never invents rows or
  errors.
- **`FCADE_SEARCH_READY_ONLY`**, when on, further restricts a username-less
  search to quarks that are store-servable right now, so early in a
  pre-convert backfill the set can be short. That is intended: a short page is
  correct, not a failure.

### Store eviction (Stage S6)

The `3sr/<quarkid>/` store grows unbounded — every `convert`/`watch` of a
new quark adds a directory. Eviction bounds it:

- **Caps (OR'd):** `FCADE_STORE_MAX_QUARKS` (default 200; the shipped
  systemd unit sets **180**) and `FCADE_STORE_MAX_BYTES` (default 200 MiB).
  When the store exceeds *either*, least-recently-**served** quark directories
  are removed until back under *both*.
- **Sizing the quark cap against the pinned set.** Every row of the
  weekly-best catalog is pinned (below), so at most 150 quarks are
  unevictable. Both the eviction trigger (0.90 × cap) and the low-water target
  (0.85 × cap) must sit *above* that pinned mass or an eviction pass can never
  reach its target: the "still over cap" tripwire would warn every pass and
  the pre-convert churn guard would latch permanently. 180 gives trigger 162
  and low-water 153, both > 150, and leaves room for ~30 quarks that have aged
  out of the window to stay cached. Lower the cap below ~177 and that property
  breaks. The byte cap must not bind first either — a real match is ≈29 KB per
  `game_N.3sr`, so 150 quarks sit far under 200 MiB.
- **LRU by last-served time**, not last-published: a quark's serve time is the
  max of its files' on-disk mtimes and an in-memory overlay bumped on every
  `get3sr` / watch-from-store. `get3sr`/watch also best-effort `utimes` the
  quark dir so serve-recency survives a proxy restart. An actively-watched pool
  therefore stays warm.
- **Never evicts** a quark with a live (non-finished) convert/watch job (it is
  skipped and left in place even if it is the LRU), and **only ever touches
  quark-named subdirectories** of the store — `catalog.json`, the cookie file,
  and any other non-quark file/dir in or around the store are structurally out
  of scope (skipped by an is-directory + strict-quarkid-shape filter).
- **Runs** at startup (so a lowered cap or an already-oversized store is trimmed
  immediately), after every successful publish (store grew), and on the periodic
  sweep (also catches out-of-band growth from `push-3sr.sh`).
- Disable entirely with `FCADE_STORE_EVICTION_ENABLED=0`.

### VPS provisioning (out-of-band, one-time)

The proxy runs no emulation unless these are present; convert is otherwise a
graceful no-op (`convert_unavailable`).

1. **Build the runner** — clone `crowded-street/fbneo-replay-runner` @
   `ccf96ab`, `git apply tools/fcade-replays/runner-track-3sr.patch`, then on
   Linux aarch64: `apt install build-essential pkg-config libsdl2-dev
   libgl1-mesa-dev libglu1-mesa-dev zlib1g-dev libpng-dev`; patch
   `makefile.sdl`'s Linux branch to link SDL2 via `pkg-config --libs sdl2`
   (not `-lSDL`) and add `pkg-config --cflags sdl2` +
   `-DBOOL=int -DTRUE=1 -DFALSE=0 -DPNG_ARM_NEON=0 -DPNG_ARM_NEON_OPT=0`;
   `make sdl 'BUILD_X86_ASM=' 'CPUTYPE=arm64' -j1` (stub the missing
   `cps3_debug_harness.d` after the first pass fails, then re-run — see
   `docs/fcade-replay-notes.md` §1). Install to `/opt/fcade-runner/`.
2. **Provision roms** — copy `sfiii3nr1.zip` **and** its `sfiii3.zip` parent
   set into `/opt/fcade-runner/roms/` (both required — notes §2). Never commit
   roms.
3. **Copy the downloader** — `deploy.sh` ships
   `tools/fcade-replays/fcade_replay_tool.py` to `/opt/fcade-proxy/`.
4. **systemd** — `fcade-proxy.service` sets `FCADE_CONVERT_*` env for the
   runner/roms/downloader paths and, crucially, adds
   `ReadWritePaths=/opt/fcade-proxy/3sr /opt/fcade-proxy/convert-scratch`
   (`ProtectSystem=strict` otherwise makes the store + scratch read-only).
   Create `/opt/fcade-proxy/convert-scratch` owned by the `fcade-proxy` user.

All paths are env-overridable (`FCADE_CONVERT_RUNNER_BIN`,
`FCADE_CONVERT_RUNNER_DIR`, `FCADE_CONVERT_DOWNLOADER`,
`FCADE_CONVERT_SCRATCH_DIR`, `FCADE_CONVERT_PYTHON`, `FCADE_CONVERT_MAX_JOBS`,
`FCADE_CONVERT_ENABLED`); defaults match the deployed VPS layout. Stage S6
hardening adds `FCADE_CONVERT_IDLE_TEARDOWN_MS` (default 45 000),
`FCADE_CONVERT_SWEEP_INTERVAL_MS` (default 15 000),
`FCADE_STORE_EVICTION_ENABLED` (default on),
`FCADE_STORE_MAX_QUARKS` (default 200), and
`FCADE_STORE_MAX_BYTES` (default 209 715 200 = 200 MiB).

## watch (live stream relay, Stage S3)

`docs/plan-fcade-live-stream.md` Stage S3. Where `convert` is "wait for the
whole conversion, then `get3sr`", `watchpoll` streams a game's `.3sr` bytes
**as the tracker produces them**, so watching can begin within
~2 s + the pre-game prefix time rather than after `≈ duration/6`. It shares
the SAME convert job machinery (one ggpo pull per quark; multiple viewers of
one quark attach to that one job — never a second connection).

### Protocol shape: offset-based chunked polling, NOT a long-lived stream

The choice was (a) a long-lived framed stream (one `watch` request → many
frames until the game ends) vs. (b) offset-based chunked polling (each
`watchpoll {from}` → one request/response with the bytes from `from`).
**We chose (b)**, for three reasons:

1. **The device client is strictly lock-step one-request-one-response**
   (`src/replay/proxy_client.h:8-10`; "Wire protocol" above): it sends one
   request frame and reads exactly one response frame. A long-lived stream
   would need a new multi-frame reader on that client; polling reuses the
   existing request/response client verbatim — the very code path `get3sr`
   already uses (`ProxyClient_Fetch3sr`).
2. **The Stage S4 device player is "play a growing `.3sr` file."** Offset
   polling maps one-to-one onto that: each response's `b64` is appended to a
   local growing file at offset `from`, identical in spirit to `get3sr`'s
   write-as-you-go loop.
3. **Byte-identity survives the tracker's in-place header patch.** The tracker
   patches the header's `frame_count`/`checksum_count` IN PLACE at finalize
   (`runner-track-3sr.patch`: `Track3srFinalizeGame` `fseek(20)`/`fseek(26)`)
   and appends the checksum table only at finalize — so the final
   `frame_count` isn't known until the game ends, and pure forward
   concatenation of a growing file could never equal the final file. Polling
   solves this cleanly: the 28-byte header (the only mutated region) rides in
   a separate `header_b64` field re-sent every poll and overwritten by the
   client at offset 0, while the body (offset ≥ 28: words then checksums) is
   strictly append-only and streamed by offset. The reconstruction is
   **byte-identical** to the completed `game_N.3sr` (verified by sha256/cmp in
   `__test_watchpoll.js`), and therefore to the offline `publish_3sr.py` file
   transitively via the Stage S1 tracker.

The one cost vs. (a) is one poll-interval of extra latency. At the wire's
~6× real-time production rate the local buffer only ever grows after playback
starts, so that interval is noise. **Fallback** (per the plan's S3
"Failure/fallback"): (b) IS the robust degrade of (a) — there is no lower
fallback needed; if latency ever matters more than lock-step simplicity, (a)
can be added later without changing the byte contract.

### The flow

1. Client sends `watchpoll {quarkid, from: 0}`. The proxy starts (or attaches
   to) the shared convert job and returns whatever bytes exist so far:
   `header_b64` + `b64` (body `[from, next)`), plus `size`/`next`/`eof`/`done`.
2. Client writes `header_b64` at offset 0 and `b64` at offset `from` of a local
   growing file, sets `from = next`, and polls again (immediately if `!eof`,
   after a short wait if `eof && !done`).
3. When `done` is `true`, the local file is byte-identical to the tracker's
   completed `game_N.3sr` (and to what `get3sr` will serve once the job
   finalizes and publishes it into the `3sr/<quarkid>/` store).

A watcher that starts AFTER a quark is already converted attaches to the ready
store copy and still streams the whole file to `done`. A job that dies
mid-watch surfaces `state: "failed", done: true` on the next poll (never a
hang). A failed job is NOT auto-retried by `watchpoll` — an explicit `convert`
op is the retry path.

## Known limitations / what was NOT verified

No real `cf_clearance` cookie was available when this was built (expected
— the cookie is browser-derived per `docs/fcade-replay-notes.md` §3, and
none was on hand at build time). Everything above the wire protocol itself
was validated only against the mock upstream in `__test_protocol.js`.

Two live checks were run against the real internet (no `FCADE_PROXY_MOCK`,
no real cookie):

1. `FCADE_COOKIE` unset entirely → `search` returned `cookie_missing`
   immediately, **zero** upstream HTTP calls (the proxy's own
   short-circuit, checked before any request is built).
2. `FCADE_COOKIE='cf_clearance=placeholder-invalid-shape-test-only'` (a
   syntactically-cookie-shaped but not-actually-valid value) → `search`
   made **exactly one** real POST to `https://www.fightcade.com/api/` and
   got back a real HTTP 403 from Cloudflare, which this proxy correctly
   classified as `{"ok": false, "error": "cloudflare_403", "detail":
   "Fightcade API returned 403 Forbidden — the cf_clearance cookie is
   likely expired or IP-bound to a different host"}`. This confirms the
   request shape (method, URL, headers, JSON body — ported verbatim from
   `tools/fcade-replays/fcade_replay_tool.py`'s `search_quarks`) is
   well-formed enough to reach Cloudflare and get a real 403 back, rather
   than e.g. a connection failure, and that the 403 → `cloudflare_403`
   mapping works end-to-end against the real service, not just the mock.

What remains unverified: the search **happy path** against real data (a
genuinely valid, non-expired `cf_clearance`) — row shape from a live
response has not been diffed against the mock's assumed shape. If a real
cookie later reveals the live API returns a subtly different row shape
than assumed here (e.g. a field this build assumes is a `number` actually
comes back as a numeric string), `normalizeRow`/`normalizePlayer` in
`fcade-proxy.js` will silently coerce it to `null` rather than crash, but
that's a correctness gap worth re-checking once a real cookie is
available. This is expected per the plan's own framing (§ Step F1 "If it
fails" / open question 5) and is not a defect in this build.
