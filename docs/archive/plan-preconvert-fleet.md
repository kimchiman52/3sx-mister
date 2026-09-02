# Plan: Pre-Convert Fleet — VPS background pre-converter + Mac worker

**Status: PLAN — nothing implemented. Written 2026-07-26 against branch
`feat/fcade-replay-browser`.**

Decision context (settled, do not relitigate): device-direct raw-stream
playback is CLOSED (CPS3 SH-2 slowdown skips records at cycle-load
instants); every watchable replay must pass through a whole-match FBNeo
conversion into an applied-word `.3sr`. Conversion is once-per-replay and
cacheable forever. This plan ships **Option B**: (1) a priority-queued
background pre-converter on the VPS's idle capacity, and (2) the user's
Mac as a second continuous converter worker that pushes finished
`.3sr` + meta to the VPS store. Goal: a MiSTer user opening the REMOTE
tab finds most rows already READY — instant playback instead of a
convert wait.

Hard constraints:

- **Zero new server cost.** Same VPS (Hetzner aarch64, 2 vCPU / 3.8 GB,
  verified 2026-07-26: `nproc` = 2, `free -m` total 3805). Disk bounded
  via the existing LRU eviction, with raised caps + a free-disk floor.
- **Do not break shipped paths**: `watchpoll` live streaming,
  `convert`/`convertstatus`, `get3sr`, store eviction, the ghost-job
  fix, the device OSD browser + wrapper. All are additive changes.
- **Politeness/stealth**: max ONE `ggpo.fightcade.com` connection per
  host at a time, humanly paced. Catalog fetching unchanged (stealth
  catalog, 8×/day).
- **Live requests always preempt background work** using the existing
  preemption machinery, made *instant* for background jobs.
- **Mac worker resumable/interruptible** (sleeps, moves networks) via
  expiring leases; the VPS never trusts pushed bytes blindly
  (server-side validation reusing `read3srGameData`).

---

## 1. Verified integration-point inventory

All citations verified 2026-07-26 by reading the file or running the
command shown. VPS facts observed over SSH (`hetzner-3s-arm`).

### 1.1 Proxy — `tools/fcade-proxy/fcade-proxy.js` (2249 lines)

| What | Where |
|---|---|
| Op dispatch: `search`, `status`, `get3sr`, `convert`, `convertstatus`, `watchpoll` — nothing else | `fcade-proxy.js:2088-2117` |
| Wire framing: u32be length + JSON over plain TCP; per-connection response chain | `fcade-proxy.js:2147-2183` |
| Request frame cap 16 KiB (`MAX_FRAME_BYTES`) | `fcade-proxy.js:63` |
| `CONVERT_MAX_JOBS` clamp 1..2, default **1** | `fcade-proxy.js:135` |
| Job machine: `jobs` Map + FIFO `queue`, `running` counter | `fcade-proxy.js:1159-1161` |
| Scratch wiped clean at boot; recreated | `fcade-proxy.js:1163-1173` |
| `touchJob` (lastAccess bump on convert/convertstatus/watchpoll) | `fcade-proxy.js:1178-1181` |
| `isQuarkActive` (eviction guard for live jobs) | `fcade-proxy.js:1186-1189` |
| **Ghost-job fix**: `dropGhostReadyJob` | `fcade-proxy.js:1200-1211` |
| `evictStore` (evict + purge finished jobs of evicted quarks) | `fcade-proxy.js:1217-1224` |
| `jobHasServableOutput` (finalized-game leniency) | `fcade-proxy.js:1233-1242` |
| `teardownAbandonedJob` (kill pull+runner, wipe scratch, delete job) | `fcade-proxy.js:1255-1283` |
| `catalogRowFor` — **`requestConvert` refuses quarks not in the current catalog** | `fcade-proxy.js:1285-1294`, `1708-1711` |
| **Preemption**: `findPreemptableJob` (stale > `CONVERT_PREEMPT_STALE_MS` = 8 s, `:180`); `pump()` preempts only while `queue.length > 0` | `fcade-proxy.js:1341-1373` |
| Idle teardown: `CONVERT_IDLE_TEARDOWN_MS` = 45 s (`:167`); sweep every 15 s (`:184`); idle victims = unfinished jobs with stale `lastAccess` and no servable output | `fcade-proxy.js:1968-1981` |
| Failed-scratch retention sweep + orphan cleanup | `fcade-proxy.js:1984-2005` |
| ggpo downloader spawn: fixed `--local-port 6004`, `--send-delay-ms 15` — **two concurrent pulls on one host are impossible (port bind clash); slot cap 1 must stay 1** | `fcade-proxy.js:1619-1642` |
| Runner spawn: `-track-3sr -replay-follow`, per-job CWD + roms symlink | `fcade-proxy.js:1546-1569`, `1601-1611` |
| Publish: meta sidecar written+renamed FIRST, then `.3sr` moved in — store-visible ⇒ complete | `fcade-proxy.js:1481-1492` |
| `tryFinalize`: `markQuarkServed` + `evictStore` post-publish | `fcade-proxy.js:1500-1522` |
| `requestConvert`: store short-circuit (`already:true`), idempotent per quark | `fcade-proxy.js:1692-1747` |
| `requestStatus`: jobs-map first, **store fallback → `state:'ready'`** (this is what makes pre-converted quarks instantly READY on-device with zero device change) | `fcade-proxy.js:1749-1761` |
| `requestWatch` starts a convert job if absent; failed job is terminal (no auto-retry) | `fcade-proxy.js:1863-1886` |
| Store validation: `read3srGameData` — size caps (`:954`, max 1 MiB `:96`), `3SR1` magic (`:980`), meta sidecar required + ≥1 named player (`:960-996`) | `fcade-proxy.js:944-1009` |
| Store LRU: `storeLastServed` overlay + dir-mtime touch (`markQuarkServed`) | `fcade-proxy.js:829-842` |
| `scanStoreQuarks` — **stats every file of every quark dir**; runs inside `evictStoreIfNeeded` on every 15 s sweep | `fcade-proxy.js:848-931`, `2009-2013` |
| Store caps: `STORE_MAX_QUARKS` 200, `STORE_MAX_BYTES` 200 MiB | `fcade-proxy.js:197-198` |
| `stats()` surfaced in `status` op | `fcade-proxy.js:2032-2049`, `777-791` |
| Quarkid shape gate `QUARKID_RE` (path-traversal proof) | `fcade-proxy.js:803` |
| Tests | `tools/fcade-proxy/__test_protocol.js`, `__test_catalog.js`, `__test_hardening.js`, `__test_watchpoll.js` |

### 1.2 Catalog rail (unchanged by this plan)

- Stealth refresh 8×/day at 00/03/06/09/12/15/18/21 local:
  `tools/fcade-proxy/stealth-catalog/dev.sambae.fcade-catalog.plist:37-46`;
  target `hetzner-3s-arm:/opt/fcade-proxy` (`run-daily.sh:41`); live
  catalog ≈ 278–295 rows (`run-daily.sh:44-45`); lands as
  `/opt/fcade-proxy/catalog.json` (`push-catalog.sh`), mtime-cached
  re-read per request (`fcade-proxy.js:345-391`).
- Mac keeps a local copy:
  `~/Library/Application Support/fcade-stealth-catalog/state/last-catalog.json`
  (`tools/fcade-replays/auto-convert/convert-batch.sh:65`).
- Row shape used by the proxy: `quarkid`, `date`, `duration`,
  `players[] {name,country,rank,score}`, `ranked`, `num_matches`,
  `emulator`, `gameid`, plus catalog-only `catalog_best`
  (`fcade-proxy.js:315-329`, `423-426`).

### 1.3 Mac-side conversion rail (exists today)

- `tools/fcade-replays/publish_3sr.py` — download → runner (RAM dumps)
  → per-game `.scrd` → **statcheck gate** → `make_3sr.py generate
  --quark-json` → `<out>/<quarkid>/game_N.{3sr,meta.json}`. Strictly
  one quark at a time end-to-end (docstring, `publish_3sr.py:24-40`);
  CLI takes `--catalog`, repeated `--quark` (`publish_3sr.py:44-49`).
- `tools/fcade-replays/auto-convert/convert-batch.sh` — the existing
  **daily** incremental batch (05:00, batch=10): out-dir is the
  "already converted" source of truth (`:10-17`), pushes via
  `push-3sr.sh` (`:79`, `:254`), fixes VPS perms with
  `sudo chown fcade-proxy:` over SSH (`:265`). LaunchAgent
  `dev.sambae.fcade-convert.plist` (05:00, `ThrottleInterval` 3600).
- `tools/fcade-proxy/push-3sr.sh` — rsync-over-SSH (user's key), no
  `--delete`, whole-tree or per-quark push straight into
  `<target>/3sr/<quarkid>/`.
- Mac runner tree: `$HOME/Developer/fbneo-replay-runner`
  (`convert-batch.sh:58-59`).

### 1.4 VPS facts (observed over SSH, 2026-07-26)

- Disk: `df -h /` → 38 G size, 6.0 G used, **30 G available**.
- Store: `/opt/fcade-proxy/3sr` = **5.3 MiB, 61 entries** → avg quark
  dir ≈ **87 KiB**.
- `node --version` → **v20.20.2** (`fs.statfsSync` available; added in
  Node 18.15).
- Firewall: `iptables INPUT policy ACCEPT`, `ufw inactive`; node
  listening on `*:3479` → **port 3479 is open to the internet** (the
  device connects directly). Any new mutating op MUST be authenticated.
- `fcade-proxy.service` env: `FCADE_STORE_MAX_QUARKS=200`,
  `FCADE_STORE_MAX_BYTES=209715200`, no `FCADE_CONVERT_MAX_JOBS`
  (→ default 1), catalog + cookie + scratch paths as in §1.1.
- FBNeo tracker runner at `/opt/fcade-runner/build/debug/fbneosdldarm64`
  (service env), patched via `tools/fcade-replays/runner-track-3sr.patch`.

### 1.5 Device/wrapper (no changes planned)

- Wrapper polls `convertstatus` per REMOTE row and maps `"ready"` →
  playable: `vendor/Main_MiSTer/replay_proxy.c:984-995` (state-string
  map), `replay_proxy.h:179-182` (op contract). `get3sr`/`watchpoll`
  client in `src/replay/proxy_client.h:125-251`.

---

## 2. Design

Everything in this section is **design** (new) unless it carries a
citation. Server changes live in `tools/fcade-proxy/fcade-proxy.js` +
tests; Mac changes in `tools/fcade-replays/auto-convert/`.

### Q1. Queue + priority model

**Two queues, one slot.** The existing live FIFO (`fcade-proxy.js:1160`)
stays untouched and is always tier P0. A NEW background scheduler owns a
separate, persistent pre-convert queue and only ever injects work when
the live path is completely idle.

- **Slot policy: background never takes a second slot.**
  `CONVERT_MAX_JOBS` stays 1 — this is load-bearing, not just polite:
  both jobs would spawn the downloader with the same fixed
  `--local-port 6004` (`fcade-proxy.js:1638-1639`) and the second bind
  would fail; and one-ggpo-connection-per-host is a hard constraint
  anyway. Background and live share the single slot; a live request
  takes it by **instant preemption**:
  - New job flag `job.background = true` (set only by the scheduler's
    internal entry point, never from the wire).
  - `findPreemptableJob` (`:1341-1351`) treats a background job as
    *always* stale (skip the `CONVERT_PREEMPT_STALE_MS` check for
    `job.background`). `pump()` (`:1360-1365`) already runs on every
    live `convert`/`watchpoll` touch while queued (`:1704`, `:1860`),
    so a live request evicts the background job within one request.
  - The idle-teardown sweep (`:1971-1981`) SKIPS background jobs — they
    have no viewer polling by definition, so `lastAccess` staleness
    means nothing for them. (Without this exemption every background
    job would be killed 45 s in.)
  - A preempted background job is simply killed
    (`teardownAbandonedJob`, `:1255-1283`); its quark stays in the
    pre-convert queue (removal happens only on store-servable success,
    see below) and is retried at a later tick.
- **Priority tiers** within the pre-convert queue:
  - **P1 — catalog-visible fresh**: rows in the current catalog's fresh
    feed, newest `date` first (what the device browses right now).
  - **P2 — catalog-visible best/high-rank**: rows tagged
    `catalog_best === true` (`fcade-proxy.js:424`) or whose
    `players[].rank` is high; these change slowly and stay browsable
    longest.
  - **P3 — backfill**: rows seen in a previous catalog refresh but no
    longer visible (kept because best-lists resurface old quarks).
    Lowest priority; converted only when P1/P2 are drained.
- **Where the queue lives: a small persistent file**,
  `/opt/fcade-proxy/preconvert-state.json` — restart-proof, unlike the
  in-memory live queue (which is deliberately ephemeral: its jobs have
  live viewers who re-request). Shape:
  `{version, queue:[{quarkid, tier, date, duration, row}], leases:{quarkid:{worker, expires_at}}, failed:{quarkid:{reason, attempts, last_at, retry_after}}, counters:{…}}`.
  Written atomically (tmp + `renameSync`, same pattern as the meta
  sidecar publish, `fcade-proxy.js:1488-1490`), debounced (≥5 s).
  Corrupt/missing file → start empty; the next catalog enqueue
  repopulates (self-healing, mirrors `loadCatalog`'s tolerant posture,
  `:379-385`).
- **Row snapshot in the queue**: `requestConvert` refuses quarks absent
  from the *current* catalog (`:1708-1711`), but P3 backfill rows have
  left it. The queue therefore stores the full normalized row at
  enqueue time, and the scheduler passes it to a new internal variant
  (`requestConvert(quarkid, {background:true, row})` — row used only
  when `catalogRowFor` misses, and only for background jobs; the wire
  `convert` op is unchanged and still catalog-gated).
- **Scheduler tick** (every 30 s, `setInterval(...).unref()` like the
  sweeper `:2015-2016`): start the highest-priority eligible item iff
  ALL of: preconvert enabled (`FCADE_PRECONVERT_ENABLED !== '0'` — the
  kill switch); no unfinished job exists and the live queue is empty;
  pacing gap elapsed (§Q5); disk floor OK (§Q3); item not leased, not
  already store-servable (`convertStoreServableGames`, `:1147-1156`),
  not failed-and-unretryable. Completion is detected by store truth,
  not job state: item removed from the queue when
  `convertStoreServableGames(quarkid)` is non-null; a `failed` job
  state moves it to the failure ledger (§Q4).
- **Jobs-map hygiene**: today finished `'ready'` jobs linger in `jobs`
  until ghost-drop or eviction purge (`:1200-1224`). Thousands of
  background conversions would grow the map without bound, so the
  scheduler deletes a background job from `jobs` the moment its quark
  is store-servable — safe because `requestStatus` falls back to the
  store and still answers `'ready'` (`:1756-1759`).

### Q2. Mac worker protocol

**Claim/lease over the wire; bytes over SSH.** The Mac already has
authenticated SSH+rsync to the VPS (`push-3sr.sh`; the
speak-wire-protocol-over-SSH precedent is `refresh-catalog.sh`'s status
verification step). The push itself therefore needs NO new network
trust: rsync over the user's SSH key. What's new is coordination:

- **Three new proxy ops**, all requiring a shared secret token
  (`FCADE_WORK_TOKEN`, set in the systemd unit env; compared with
  `crypto.timingSafeEqual`). Required because 3479 is open to the
  internet (§1.4). A missing/wrong token → `{ok:false,
  error:'unauthorized'}`; ops are also excluded from any device code
  path (the wrapper never sends them).
  - `worklease {token, worker, count?}` → leases up to `count`
    (default 1, max 3) highest-priority unleased queue items to
    `worker`; response includes each item's **row snapshot** (so the
    Mac can convert quarks that have left its local catalog: the
    worker writes a synthetic one-row `{rows:[row]}` catalog file for
    `publish_3sr.py --catalog`). Lease TTL 45 min (> the 30 min
    download/runner ceilings, `fcade-proxy.js:146-147`). Re-leasing an
    item you already hold extends it (renewal). A sleeping Mac simply
    lets leases lapse; the VPS scheduler skips *leased* items and
    reclaims them on expiry — no stranded work.
  - `workdone {token, worker, quarkid, ok, reason?}` → on `ok:true`,
    the proxy **validates and integrates** the pushed files (below);
    on success removes the item from the queue. On `ok:false`, records
    the failure in the ledger (§Q4) and frees the lease.
  - `workstats {token}` → worker-facing view of the queue (optional
    convenience; also served to the operator via `status`, §Q7).
- **Push path with server-side validation + atomicity.** The worker
  rsyncs each finished quark to a NEW staging root
  `/opt/fcade-proxy/3sr-incoming/<quarkid>/` (extend `push-3sr.sh`
  with an `--incoming` mode or a thin sibling script). `workdone` then
  makes the proxy: (1) check `QUARKID_RE` (`:803`); (2) run the exact
  serving validation `read3srGameData` (`:944-1009` — size caps, `3SR1`
  magic, meta sidecar, named players) against every `game_N.3sr` in the
  incoming dir; (3) if ≥1 game passes, `renameSync` the staged files
  into `3sr/<quarkid>/` meta-first (same ordering rule as
  `publishTrackerOutput`, `:1481-1492`), then delete the incoming dir;
  if none pass, reject with a typed error and wipe the staging dir. A
  half-push can never serve: `get3sr`/`convertstatus` only ever look at
  `3sr/`, and files appear there only via atomic rename after
  validation. (This also fixes a latent race in the legacy direct-rsync
  path, where a quark mid-push is briefly visible with `.3sr` but no
  meta.)
  - Perms: staging is created by the worker's SSH user; the `workdone`
    integration runs as the `fcade-proxy` service user, so the worker's
    push step keeps the existing `sudo chown -R fcade-proxy:` fix
    (`convert-batch.sh:265`) pointed at `3sr-incoming` before calling
    `workdone`.
- **Mac worker loop** (new
  `tools/fcade-replays/auto-convert/preconvert-worker.sh` +
  a tiny framing client `proxy_ops.py` — u32be+JSON, port of
  `fcade_replay_tool.py`'s `recv_frame`, run against `127.0.0.1:3479`
  through `ssh hetzner-3s-arm` exactly like `refresh-catalog.sh` step 7
  does): every run = preflight (same checks as `convert-batch.sh:128-134`)
  → `worklease` → for each item: synthesize row-catalog →
  `publish_3sr.py --quark` from `$RUNNER_DIR` (CWD gotcha,
  `convert-batch.sh:25-27`) → rsync to incoming → `workdone` → next.
  Runs as a LaunchAgent with `StartInterval` ~600 s, no `RunAtLoad`,
  `ThrottleInterval` guard (mirroring `dev.sambae.fcade-convert.plist`)
  — a finished run exits; a sleeping/offline Mac just misses ticks.
  Single-instance lock via a state-dir lockfile. **This worker replaces
  the daily 05:00 batch agent** (bootout `dev.sambae.fcade-convert`);
  `convert-batch.sh` stays for manual runs.

### Q3. Store growth policy

- **Observed baseline**: 61 quarks = 5.3 MiB (avg ≈ 87 KiB/quark,
  §1.4); a single full match ≈ 29 KiB `.3sr` (`fcade-proxy.js:93`).
  Live catalog ≈ 290 rows ⇒ converting the *entire* visible catalog is
  ≈ 25 MiB. 30 G is free.
- **New caps** (env only — mechanism unchanged):
  `FCADE_STORE_MAX_QUARKS=10000`, `FCADE_STORE_MAX_BYTES=2147483648`
  (2 GiB). At the observed average, 10 k quarks ≈ 0.9 GiB — the count
  cap binds first; 2 GiB is headroom for long FT10-style quarks. 10 k
  quarks ≈ months of full rolling catalog history, so every row the
  device can browse stays READY for its whole browsable life —
  hit-rate is limited by conversion throughput and quark expiry, not
  cache size. Scratch stays trivial (<~2.5 MB/job, `:1162-1165`; boot
  wipe `:1166-1168`; retention sweep `:1984-2005`).
- **Disk floor** (new): `FCADE_DISK_FLOOR_BYTES` default 10 GiB. Checked
  with `fs.statfsSync` (Node 20, §1.4) (a) by the background scheduler
  before starting a job, (b) by `workdone` before integrating a push.
  Below the floor: background conversion pauses and pushes are refused
  with a typed error; live convert-on-select is *not* gated (a viewer's
  ~2.5 MB scratch is noise, and live behavior must not regress).
- **Eviction cadence decoupled from the 15 s sweep**: `scanStoreQuarks`
  stats every file in every quark dir (`:848-887`) and currently runs
  in every sweep (`:2009-2013`). At 10 k quarks that is ~60 k stats
  every 15 s for nothing. Change: run `evictStore` on publish (already
  the case, `:1517-1521`), on `workdone` integration, and on a separate
  timer `FCADE_STORE_EVICT_SWEEP_MS` default 10 min. Eviction policy
  itself (LRU by `served` = max(dir mtimes, in-memory overlay),
  active-job guard) is untouched, so `markQuarkServed` (`:829-842`)
  keeps favoring actually-watched quarks over merely-pre-converted
  ones.
- **Churn guard**: the enqueuer never enqueues while the store is at
  ≥ 90 % of either cap (prevents an enqueue→convert→evict→re-enqueue
  loop if caps are ever lowered). With caps ≫ catalog size this should
  never trigger; it is a safety, not a mechanism.

### Q4. Catalog-driven enqueue + failure policy

- **Trigger**: the scheduler tick detects a catalog change by mtime
  (same signal `loadCatalog` caches on, `:364`) and runs the enqueuer:
  for every current row with `gameid === 'sfiii3nr1'`
  (`FCADE_PRECONVERT_GAMEID` env), classify tier (P2 if
  `catalog_best`/high rank, else P1), and enqueue unless the quark is
  (a) store-servable (`convertStoreServableGames`), (b) already
  queued/leased, (c) in the failure ledger and not yet past
  `retry_after`, or (d) blocked by the churn guard. Rows that dropped
  out of the current catalog but sit un-converted in the queue demote
  to P3 (their stored row snapshot keeps them convertible, §Q1).
- **Failure ledger** (persisted in `preconvert-state.json`):
  - `no_savestate` — the expired/handshake-only signature. Today it is
    only a failure *string* (`:1677`); add a typed `job.failReason`
    set in the downloader exit handler so the ledger never
    string-matches. Policy: 1 retry after 24 h, then permanent (entry
    kept so re-appearing catalog rows don't re-enqueue it). This is
    the dominant expected failure: old quarks expire server-side.
  - `no_games` / runner or publish errors (`:1508-1509`, `:1683`):
    retry with backoff 1 h → 4 h → 24 h, max 3 attempts, then
    permanent.
  - A **live** `convert` op for a ledgered quark is unaffected — the
    explicit-retry contract (`:1701-1707`, comment `:1863-1869`)
    stands; a live success clears the ledger entry.
- Failed-forever entries are pruned when they age out of the union of
  seen catalogs (bounded ledger).

### Q5. Politeness pacing

- **One ggpo connection per host**: VPS — structurally enforced (slot
  cap 1 + fixed local port 6004, §Q1). Mac — `publish_3sr.py`
  processes quarks strictly serially (`publish_3sr.py:36-40`), one
  download at a time.
- **VPS background spacing**: new `FCADE_PRECONVERT_GAP_MS` default
  180 000 (3 min) + uniform jitter 0–60 s between background pull
  *starts*; measured from the end of the previous background job. Live
  convert-on-select is exempt (unchanged behavior). No day/night
  schedule initially — at ~10–14 pulls/hour worst case the profile is
  a patient human binge, and the backlog (~230 unconverted visible
  rows) drains in about a day; steady state (tens of new quarks/day) is
  a few pulls/hour. Downloader pacing itself is unchanged
  (`--send-delay-ms 15`, `:1640-1641`).
- **Mac spacing**: the worker sleeps 60–120 s (jittered) between
  quarks within a run, ≤3 quarks per 10-min tick.
- **Never convert the same quark twice**: leases (§Q2) are the
  cross-host mutex — the VPS scheduler skips leased items; the Mac
  only converts leased items. On lease expiry both sides may race in
  theory; the store is idempotent (same quark → same bytes; `workdone`
  integration of an already-servable quark is a no-op success), so the
  race wastes at most one conversion, never corrupts.

### Q6. Device UX

**No device, core, or wrapper change.** Verified chain: pre-converted
quarks answer `convertstatus` with `state:'ready'` via the store
fallback (`fcade-proxy.js:1756-1759`); the wrapper maps `"ready"` to
the playable row state (`vendor/Main_MiSTer/replay_proxy.c:984-995`);
playback uses the unchanged `get3sr`/`watchpoll` ops. The only visible
effect is that most REMOTE rows show READY immediately.

### Q7. Observability

Extend the existing `status` op (`handleStatus`, `:777-791`) with a
`preconvert` block — read-only, no auth needed (status already leaks
nothing sensitive):

```json
"preconvert": {
  "enabled": true,
  "queue": {"p1": 12, "p2": 30, "p3": 180, "leased": 2},
  "failed": {"total": 14, "permanent": 9},
  "converted": {"total": 213, "today": 41, "by_vps": 25, "by_worker": 16},
  "store": {"quarks": 240, "bytes": 21400000, "disk_free_bytes": 31000000000},
  "hit": {"status_ready": 1041, "status_absent": 88},
  "worker": {"last_seen": 1784900000000, "last_worker": "mac-sb"}
}
```

`hit` counters increment in `requestStatus` (ready-vs-absent) — the
direct measure of "rows instantly playable when browsed". `evicted`
count rides in the existing store stats. Daily rollover persisted in
`preconvert-state.json.counters`. Operator check =
`{"op":"status"}` against `127.0.0.1:3479` over SSH, the
`refresh-catalog.sh` pattern.

### Q8. Failure / edge cases

| Case | Behavior |
|---|---|
| Proxy restart | Pre-convert queue/leases/ledger reload from `preconvert-state.json`; in-flight job's scratch is boot-wiped (`:1166-1168`) and its quark is still queued (removal is store-truth only) → reconverted. Live jobs were viewer-driven and re-request naturally. |
| Mac offline a week | Leases expire (45 min); VPS-only mode converts the whole visible catalog by itself (slower, same result). No coordination state on the Mac matters. |
| Duplicate push / lease-expiry race | `workdone` on an already-servable quark = no-op success; staged files wiped. Store content idempotent. |
| Multi-game quarks | Both converters emit per-game `game_N.*`; validation/integration loop all indices (`listQuarkGameIndices`, cap 64, `:98`, `:1033-1047`). Unchanged. |
| Disk full mid-convert | Scratch cleanup exists (boot wipe `:1166-1168`, failed-retain sweep `:1984-2005`); the new disk floor stops *starting* background work well before ENOSPC; live convert unaffected. |
| Ghost jobs | Background finished jobs are deleted from `jobs` at completion (§Q1) so they can never ghost; the shipped `dropGhostReadyJob` (`:1200-1211`) + eviction purge (`:1217-1224`) still cover live jobs. Scheduler uses the store, not `jobs`, as truth. |
| Catalog push mid-enqueue | `loadCatalog` already tolerates mid-write/missing files (`:353-385`); the enqueuer runs off the same loaded snapshot. |
| Bad/hostile push to staging | Only reachable via the user's SSH key; `workdone` validation (magic/size/meta/quarkid regex) rejects garbage; nothing unvalidated ever enters `3sr/`. |
| Wire abuse of new ops | Token-gated (`timingSafeEqual`); wrong token → typed error; work ops never spawn processes directly (leases mutate queue metadata only; `workdone` touches only `3sr-incoming/<validated-quarkid>`). |

---

## 3. Staged implementation plan

Each stage is independently shippable and leaves every shipped path
green. Existing tests (`__test_protocol.js`, `__test_catalog.js`,
`__test_hardening.js`, `__test_watchpoll.js`) must pass at every stage.
"Deploy" = rsync `fcade-proxy.js` + systemd env edit + restart, the
same rail as today (`tools/fcade-proxy/deploy.sh`).

### S1 — Background-job semantics in the job machine
**Side: server JS (fcade-proxy.js + new test). Device: none.**

- Add `job.background` (internal only; never settable via the wire).
  `requestConvert` gains an internal options arg `{background, row}`
  (row used only when `catalogRowFor` misses; wire `convert` op
  behavior unchanged).
- `findPreemptableJob`: background ⇒ instantly preemptable (skip the
  8 s staleness gate).
- Idle-teardown sweep: skip `job.background`.
- On background job completion with a servable store dir: delete the
  job from `jobs` (map hygiene).
- **Gate**: new `__test_background.js` (mock-process style like
  `__test_hardening.js`): (a) a live `convert` preempts a running
  background job within one `pump()`; (b) background job survives >45 s
  with zero touches; (c) live jobs' idle/preempt behavior byte-for-byte
  unchanged; (d) finished background job absent from `jobs`, its
  `convertstatus` still `'ready'` from the store. All existing tests
  pass.

### S2 — Persistent pre-convert queue, scheduler, catalog enqueuer, failure ledger
**Side: server JS. Device: none.**

- `preconvert-state.json` load/save (atomic, debounced, tolerant).
- Scheduler tick (30 s): eligibility checks (idle live path, pacing
  gap+jitter, disk floor via `fs.statfsSync`, lease/store/ledger
  dedup, churn guard) → start background job; completion by
  store-truth; failure → ledger (typed `job.failReason` added to the
  downloader/runner exit paths).
- Catalog-mtime-driven enqueuer with P1/P2/P3 tiers + row snapshots +
  P3 demotion.
- Kill switch `FCADE_PRECONVERT_ENABLED` (default on in code, but
  **deployed OFF first**; flipped on in S7).
- **Gate**: unit tests with fixture catalogs + mock convert: enqueue
  dedup (store/queue/ledger), tier ordering, restart persistence
  (kill+reload keeps queue and ledger), pacing gap honored, disk-floor
  pause, `no_savestate` → 1×24 h retry then permanent, live-convert
  clears ledger entry. Existing tests pass.

### S3 — Store scale-up + eviction cadence
**Side: server JS + VPS env. Device: none.**

- Decouple `evictStore` from the 15 s sweep: publish/workdone hooks +
  `FCADE_STORE_EVICT_SWEEP_MS` (10 min) timer.
- Deploy env: `FCADE_STORE_MAX_QUARKS=10000`,
  `FCADE_STORE_MAX_BYTES=2147483648`, `FCADE_DISK_FLOOR_BYTES`
  (10 GiB).
- **Gate**: existing eviction tests still pass (they set their own env
  caps); new test that eviction still triggers post-publish and on the
  slow timer but not on the 15 s sweep; on-VPS spot check: `status`
  shows new caps, sweep CPU unchanged (`journalctl` quiet).

### S4 — Work-lease ops + validated push integration
**Side: server JS. Device: none.**

- `worklease`/`workdone`/`workstats` ops, `FCADE_WORK_TOKEN` gate
  (`timingSafeEqual`), lease TTL/renewal/expiry.
- `3sr-incoming/` staging + `workdone` validation
  (`read3srGameData`-reuse) + meta-first atomic rename into `3sr/` +
  staging cleanup; disk-floor refusal.
- **Gate**: protocol tests: no/wrong token rejected; lease claim,
  renewal, expiry-reclaim; `workdone` with (a) valid multi-game push →
  integrated + `convertstatus` ready, (b) bad magic/oversize/nameless →
  rejected + staging wiped + nothing in `3sr/`, (c) already-servable
  quark → no-op success; a device-shaped client (no token) still gets
  full unchanged behavior on all six legacy ops.

### S5 — Mac worker
**Side: Mac tooling (tools/fcade-replays/auto-convert/ + tools/fcade-proxy/push-3sr.sh). Device: none.**

- `proxy_ops.py` (u32be framing client over `ssh hetzner-3s-arm`,
  ported from `fcade_replay_tool.py` framing).
- `preconvert-worker.sh`: preflight → lease (≤3) → per quark:
  synthetic row-catalog → `publish_3sr.py` from `$RUNNER_DIR` →
  rsync to `3sr-incoming/` (push-3sr.sh `--incoming` mode) + perms fix
  → `workdone`; jittered inter-quark sleep; lockfile.
- LaunchAgent plist (`StartInterval` 600, no `RunAtLoad`) + `setup.sh`
  extension; migration step: `launchctl bootout` of
  `dev.sambae.fcade-convert` (daily batch retired; script kept for
  manual use).
- **Gate**: `--dry-run` prints leases + planned commands without
  converting; one attended end-to-end cycle: lease → convert one real
  quark → push → `workdone` → `convertstatus` ready on the VPS →
  `status.preconvert.converted.by_worker` incremented; then a
  simulated sleep (kill worker mid-lease) → lease expires → VPS
  scheduler picks the quark up.

### S6 — Observability
**Side: server JS. Device: none.**

- `preconvert` block in `status` (§Q7): queue depths, ledger counts,
  conversions total/today/by-source, store bytes + disk free, `hit`
  ready/absent counters in `requestStatus`, worker last-seen; daily
  rollover persisted.
- **Gate**: test asserts the block's shape and counter increments;
  manual `{"op":"status"}` over SSH shows live numbers.

### S7 — Turn-on + burn-in
**Side: ops only (VPS env + Mac launchctl). Device: none.**

- Set `FCADE_WORK_TOKEN`, flip `FCADE_PRECONVERT_ENABLED=1`, load the
  Mac worker agent.
- **Gate** (48 h burn-in, all via `status` + journal): backlog
  draining; zero live-viewer regressions (a `watchpoll` on a
  non-converted quark still starts within one poll — background job
  preempted); ggpo pull spacing in the journal ≥ the configured gap;
  store under caps; `hit.status_ready` fraction climbing toward ~all
  non-expired rows; then a device TV check: REMOTE tab rows
  near-universally READY, WATCH NOW instant.

Suggested commit granularity: one commit per stage, tests included,
message tagged `[plan-preconvert S<n>]` (matches the existing
`[plan-osd S…]` convention in `git log`).

---

## 4. Risk register

| # | Risk | Mitigation |
|---|---|---|
| R1 | New mutating ops on an internet-open port (3479, §1.4) | Token gate (`timingSafeEqual`); ops mutate only queue metadata / validated staging; pushes ride SSH, not the TCP port; legacy ops untouched. |
| R2 | Background pulls raise the Fightcade footprint | 1 conn/host (structural), 3 min+jitter gap, bounded backlog drain, kill switch `FCADE_PRECONVERT_ENABLED=0`, catalog fetch cadence untouched. |
| R3 | Background job starves a live viewer | Instant preemption for `background` jobs (S1 gate a); live queue untouched; watch flow identical to today when a quark is unconverted. |
| R4 | Eviction scan cost at 10 k quarks | Eviction decoupled from the 15 s sweep (S3); scan runs ~6×/h + on publish. |
| R5 | Enqueue/evict churn | Caps ≫ catalog size; 90 % churn guard; queue removal only on store-servable success. |
| R6 | Mac/VPS double-convert on lease expiry | Idempotent store; `workdone` no-op on servable quark; wasted work bounded to one conversion. |
| R7 | `preconvert-state.json` corruption | Atomic tmp+rename writes; tolerant load → empty + catalog re-enqueue (self-healing). |
| R8 | VPS tracker output lacks the Mac path's statcheck gate | Pre-existing property of shipped convert-on-select (S2 of plan-fcade-live-stream); background VPS conversions are exactly as trustworthy as today's live ones; Mac-converted quarks are statcheck-gated. Not a regression; noted for a future statcheck-on-VPS follow-up. |
| R9 | Someone raises `CONVERT_MAX_JOBS` to 2 | Document in code next to the clamp (`:135`): fixed `--local-port 6004` (`:1638`) + politeness make 2 unusable; keep 1. |
| R10 | Legacy direct-rsync push (`convert-batch.sh`) races the meta-first rule | Worker path uses staging+validated rename (S4); daily batch retired in S5; `push-3sr.sh` non-incoming mode kept for manual whole-tree seeding only. |

---

## 5. Non-goals

- **No device/core/wrapper changes** (verified unnecessary, §Q6).
- **No new server spend** — same VPS, disk bounded (caps + floor).
- **No catalog-fetch changes** — stealth catalog stays 8×/day,
  browser-driven, unchanged shape.
- **No revisiting raw-stream device playback** (closed with
  mechanism-level proof).
- **No public/third-party worker support** — exactly one worker (the
  user's Mac) with SSH access; the token is a fence, not a
  multi-tenant auth system.
- **No rsync `--delete` anywhere** (standing rule).
