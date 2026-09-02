# Plan: Bounded-Pool Always-Ready Replay Strategy

**Status: DESIGN — uncommitted, for review.**
**Branch:** `feat/fcade-replay-browser` · **Written:** 2026-07-28 (all live
numbers sampled 2026-07-28 ~16:46–16:52 UTC over SSH to `hetzner-3s-arm`,
read-only).

Strategy anchor (settled, do not relitigate — memory
`project-device-direct-replay.md`): the cold-path wait is fixed by **bounding
the pool + full precompute so cold ceases to exist** — not by more conversion
engineering. Device-direct predict-search-correct is a proven-buildable future
option and is out of scope here.

---

## 0. The headline finding

**Right now, 11 of the 294 catalog-visible rows are READY (1 of 144 BEST,
10 of 150 RECENT)** — measured live 2026-07-28 16:51 UTC by intersecting
`/opt/fcade-proxy/catalog.json` rows with `/opt/fcade-proxy/3sr/` store dirs:

```
visible total: 294 ready: 11
best: 144 ready: 1
recent(non-best): 150 ready: 10
```

…despite the fleet having converted **522** quarks total (state counter
`converted_total`, same snapshot) and running flat-out at ~266/day. The fleet
is converting the *firehose*, not the *visible pool*:

1. New-quark inflow (~1,180/day observed, §3.2) exceeds conversion capacity
   (~282/day, §3.1) by ~4.4×, so the newest-first P1 race is unwinnable — a
   converted row is pushed out of the top-150 RECENT window within ~3 h.
2. The BEST tier (144 slow-churning rows — one-time cost ~12 h of capacity)
   is starved *behind* P1 by the current ordering
   (`preconvertPickEligible`, `tools/fcade-proxy/fcade-proxy.js:2493-2501`:
   lowest tier first, and `preconvertClassifyTier` at `:2389-2391` puts fresh
   at tier 1, best at tier 2). P1 never drains ⇒ P2 is never picked ⇒
   **143 of 144 BEST rows sit unconverted** (state snapshot: tier
   distribution `{1: 140, 2: 143, 3: 1717}` of queue length 2000).

The design below makes "every row the device can see is READY" a **structural
guarantee** (the catalog serves only READY rows) instead of a capacity race,
and points the conversion capacity at the highest-leverage targets (BEST
first, then fresh).

---

## 1. Verified current state (code + live)

### 1.1 Proxy scheduler (tools/fcade-proxy/fcade-proxy.js, 3026 lines)

| Fact | Citation |
|---|---|
| Single convert slot; `CONVERT_MAX_JOBS` clamped 1..2, default 1 | `fcade-proxy.js:136` |
| Concurrency >1 is ALSO blocked in code: downloader always spawns with fixed `--local-port 6004` — a second simultaneous job would fail the bind | `fcade-proxy.js:1769-1772`; plan-preconvert-fleet.md §Q1 ("load-bearing, not just polite") |
| Background pacing: `PRECONVERT_GAP_MS` 180 s + uniform 0–60 s jitter, measured from END of previous background job | `fcade-proxy.js:225-229`, `:2267-2273` |
| Scheduler tick every 30 s; starts ONE background job iff slot idle, gap elapsed, disk floor OK | `fcade-proxy.js:221`, `:2503-2520` |
| Pick order: **lowest tier, then newest date** | `fcade-proxy.js:2491-2501` |
| Tier assignment: `catalog_best === true` → tier 2 (P2); else tier 1 (P1). P3 only by demotion when a row leaves the catalog | `fcade-proxy.js:2386-2391`, demotion `:2441-2443` |
| Enqueuer runs on catalog mtime change; refreshes/demotes existing items, adds new rows unless store-servable / queued / leased / ledger-blocked / churn-guard | `fcade-proxy.js:2412-2464` |
| Churn guard: enqueuer adds nothing while store ≥ 90 % of either cap | `fcade-proxy.js:2393-2401` |
| Completion signal = store truth only (`convertStoreServableGames`) | `fcade-proxy.js:1237-1246`, reconcile `:2469-2481` |
| Live preemption of background jobs is instant (background = always stale; viewer-present jobs protected by 8 s staleness gate `CONVERT_PREEMPT_STALE_MS`) | `fcade-proxy.js:169-181`, `:1442-1445` |
| Failure ledger: `no_savestate` → 1 retry after 24 h then permanent; others backoff 1 h → 4 h → 24 h | `fcade-proxy.js:236-247` |

### 1.2 Store + eviction

| Fact | Citation |
|---|---|
| Store = one dir per quark under `/opt/fcade-proxy/3sr/`; scan stats every file, `served` = max(file mtimes, in-memory serve overlay) | `fcade-proxy.js:938-977` |
| Eviction: LRU by `served`, evicts until under BOTH caps **exactly** (no low-water), never a quark with a live job; **no notion of catalog membership** | `fcade-proxy.js:979-1021` |
| Caps (live systemd env): `FCADE_STORE_MAX_QUARKS=10000`, `FCADE_STORE_MAX_BYTES=2147483648` (2 GiB), eviction ON, disk floor 10 GiB | `systemctl cat fcade-proxy` output, 2026-07-28 |
| Eviction cadence: on publish + on workdone + dedicated 10-min timer | `fcade-proxy.js:200-206`, `:1650`, `:2644`, `:2727-2737` |
| `markQuarkServed` on get3sr/watch keeps watched quarks warm | `fcade-proxy.js:1185`, `:1213`, `:2004` |

### 1.3 Catalog rail

| Fact | Citation |
|---|---|
| Catalog is a static JSON pushed from the Mac stealth-browser runner; proxy serves `search` entirely from it when present (no upstream call) | `fcade-proxy.js:806-828` |
| Refresh cadence: **8×/day, every 3 h on the hour** (Mac LaunchAgent `StartCalendarInterval`) | `tools/fcade-proxy/stealth-catalog/dev.sambae.fcade-catalog.plist` (hours 0,3,…,21) |
| Per-feed row cap: `MAX_ROWS` **150**/feed (recent feed offset-paged 0,15,30… up to cap; best feed likewise) | `stealth-catalog/run-daily.sh:50-51`, `stealth-catalog/README.md:85-103` |
| `searchCatalog`: filters by gameid/username/since, `best:true` restricted to `catalog_best` rows, sorted by recency, paged | `fcade-proxy.js:495-524` |
| `normalizeRow` (the wire row shape) carries **no readiness field** — quarkid, date, duration, players(name/country/rank/score), ranked, num_matches, emulator, gameid | `fcade-proxy.js:404-419` |
| Live catalog: **294 rows, 144 `catalog_best`**, 154 rows dated 2026-07-28; file mtime 16:00 UTC (last scrape slot) | live read of `/opt/fcade-proxy/catalog.json` |

### 1.4 Device UI (the real one: wrapper OSD)

The game-side browser (`src/replay/replay_browser.c`) is **permanently
inert** — Option-A hide, `replay_browser.c:276-283` ("the native OSD …
is now the one and only replay browser UI"). The real UI lives in the
wrapper menu patch `tools/mister-wrapper/main-mister-full-menu.patch`
(applied onto upstream Main_MiSTer by `tools/mister-wrapper/build-hps.sh:6-10`;
`vendor/Main_MiSTer/` overlays `replay_proxy.c` / `thirdsarm_wrapper.cpp`).

| Fact | Citation |
|---|---|
| Sub-tabs: RECENT (newest), BEST (`best=true` + `since=` start-of-current-UTC-month), BY PLAYER (username) | patch `:236-237`, `:428` (`p.best = (g_remote_tab == REMOTE_TAB_BEST)`) |
| Page size 10 rows (`REMOTE_PAGE_LIMIT`), offset-paged; "full page ⇒ maybe next page" | patch `:247`, `:349`, `:1195-1203` |
| Row list shows quarkid/players/date only — **no per-row ready badge**; readiness appears only after picking a row: a one-shot `convertstatus` poll drives a status line `CHECKING / not converted yet / QUEUED / CONVERTING n% / READY / CONVERT FAILED` | patch `:1597-1599`, `:1633-1641` |
| "Watch now (live)" is **always offered** on the row-action chooser (the ~15–30 s live-WATCH path) | patch `:1650`, `:1589-1599` |
| Wrapper maps proxy `state:"ready"` → playable | `vendor/Main_MiSTer/replay_proxy.c:995-996` |
| Proxy answers `convertstatus` for store-servable quarks with `ready` via store fallback and bumps hit counters | `fcade-proxy.js:1906-1920` |

**Consequence:** a proxy-side change to *which rows `search` returns* needs
**zero device change**. Only per-row badges or an attract mode would need
device code.

### 1.5 Mac worker

`tools/fcade-replays/auto-convert/preconvert-worker.sh` (header, lines 1-45):
one tick ≈ every 10 min via LaunchAgent, leases ≤ 3 quarks
(`WORK_LEASE_MAX_COUNT = 3`, `fcade-proxy.js:257`), 60–120 s jittered sleep
between quarks, one ggpo connection at a time, pushes via SSH + `workdone`.

### 1.6 Live state snapshot (2026-07-28 16:46–16:52 UTC)

All from read-only SSH to `hetzner-3s-arm`:

- Store: **584 quark dirs, 63 MB** (`ls | wc -l`, `du -sh`); disk 18 % used,
  30 G avail (`df -h /opt/fcade-proxy`).
- `preconvert-state.json` (614,770 B, ≈ 307 B/queue item): queue **2000**
  (tiers `{P1:140, P2:143, P3:1717}` same-snapshot), leases 0, failed **34**
  (all `publish_error`, 0 permanent).
- Counters: `converted_total 522` (`by_vps 484`, `by_worker 38`),
  `converted_today 200` at 16:46 UTC; `hit_status_ready 1`,
  `hit_status_absent 2` (device has barely browsed since the S6 counters
  landed — too sparse to be a metric yet).
- Journal, last 24 h: **266** `ready — game(s)` completions, **0** preempts,
  0 failed-convert lines, 16 `workdone` log lines.
- Completion spacing sample (journal): 16:26:00 → 16:31:11 → 16:35:50 →
  16:40:41 → 16:45:33 ⇒ deltas 311/279/291/292 s, **mean ≈ 293 s/job**
  (job 56–77 s + gap 180 s + jitter 0–60 s).

---

## 2. Pool definition

**The pool = the set of quarks the device can reach by browsing, and every
pool member is READY before it is browsable.** Three layers:

- **B — BEST layer**: all `catalog_best` rows (144 today; device BEST tab
  shows the month-scoped subset, §1.4). Slow churn ⇒ convert once, maintain
  by trickle. Target: **100 % READY at all times.**
- **F — FRESH layer**: the RECENT tab. Redefined (this is the flip): the tab
  shows **the newest 150 READY rows**, not the newest 150 known rows. The
  guarantee becomes structural — visible ⇔ READY — at *any* conversion rate;
  conversion rate buys *freshness*, not the guarantee (§3.4).
- **A — ATTRACT layer (optional, user decision)**: a small curated pinned
  set (suggest 50–200) for a pseudo attract-mode (random autoplay). Sketch in
  §7; separate stage, ships only if wanted.

Pool size: **≈ 294 catalog rows + attract layer** ≈ 300–500 quarks ≈ 33–55 MB
at the observed ~108 KB/quark average (63 MB / 584). Trivially inside the
10 k / 2 GiB caps. **`MAX_ROWS` does not need to change** for the guarantee
(raising it is a stealth/observability decision, §8 D4).

What is *not* in the pool: username-search results and out-of-catalog quarks.
Those stay reachable as explicit best-effort paths (honest status line +
"Watch now" ≈ 15–30 s live stream — the existing WATCH path, §1.4), so
"cold" as a surprise state ceases to exist: browsing is always instant, and
the only non-instant paths are ones the user explicitly asked for.

---

## 3. Capacity math (derived from live data)

### 3.1 Conversion capacity

- VPS background cycle = job + gap + jitter ≈ **293 s** (measured, §1.6) ⇒
  ceiling ≈ 86400/293 ≈ **295/day**; observed **266/day** (journal 24 h count;
  the difference is jitter spread + catalog-enqueue pauses).
- Mac worker: ≤ 16 `workdone` lines/24 h observed; 38 total conversions ever.
  Real but minor and Mac-awake-hours-bound. Call it **~15/day** as-is.
- **Total ≈ 282/day at current knobs.** Levers exist (gap, concurrency,
  worker cadence) but every one is a user stealth decision (§8).

### 3.2 Inflow

Observed new-quark inflow (Mon 2026-07-27, the one full weekday of fleet
operation): queue items still pending with that quark-date (**913**) + store
quarks with that quark-date (**266**) ≈ **1,179/day** — essentially at the
observation ceiling of 8 refreshes × 150 recent rows = **1,200/day**, so the
feed is *saturated*: true sfiii3nr1 inflow is **≥ ~1,180/day** and partially
unobserved. (Tue 2026-07-28 by 16:46 UTC: 705 pending + 189 converted = 894,
on pace ~1,280/day.)

### 3.3 The raw race is unwinnable — and that's fine

All-ready for the *raw* newest-150 window would need conversion ≥ inflow ≈
1,180/day = **4.4×** current capacity, and rising with the game's popularity.
This is exactly why the strategic decision is to bound the pool: under the
ready-filter (§2 F), the guarantee costs **zero** additional capacity.

### 3.4 What capacity buys under the flip: freshness

- BEST backfill (one-time): 143 unconverted rows × 293 s ≈ **11.6 h** of
  exclusive background capacity. After that, maintenance ≈ **~5 best
  entrants/day** (estimate: 144 best rows span quark-dates 2026-07-01→07-28 ⇒
  144/28 ≈ 5.1/day; labeled estimate — single catalog snapshot, no history).
- RECENT freshness: newest-150-READY at 266/day ⇒ the tab spans the last
  150/266 × 24 h ≈ **13.5 h** of matches. The newest row is ≈ one scrape lag
  (0–3 h, median ~1.5 h) + one convert cycle (~5 min) old. Every visible row:
  instant play.
- Steady state after backfill: ~5/day best + 261/day fresh ⇒ **~98 % of
  capacity still flows to fresh rows.**

### 3.5 Timelines that force the hygiene work

- Store grows ~282/day ⇒ hits the **90 % churn guard** (9,000 quarks) in
  (9000−584)/282 ≈ **30 days**. Because eviction stops *exactly at* the cap
  (`fcade-proxy.js:991-997`) and the guard trips at 90 % of it
  (`:2395-2401`), the store will oscillate at ~cap while the guard stays
  **permanently latched** — the enqueuer freezes and the always-ready pipeline
  silently stalls. Must fix before ~2026-08-27 (§6.2).
- P3 backlog grows ≈ inflow − conversions ≈ **~900/day** ⇒ queue/state-file
  growth ~275 KB/day (307 B/item measured). Unbounded today (§6.1).

---

## 4. Priority policy (the flip)

### 4.1 Today

`preconvertPickEligible` (`fcade-proxy.js:2493-2501`): min tier, then max
date, over non-leased, non-ledger-blocked items. Tiers
(`preconvertClassifyTier`, `:2389-2391`): fresh = 1, `catalog_best` = 2,
demoted = 3. Result (proven live, §0): best starves forever behind an
undrainable fresh tier.

### 4.2 Proposed ordering function

Priority is a tuple compared lexicographically; **lower sorts first**:

```
priority(item) = (tier, -date)
  tier 1: catalog_best row, not store-servable        # was tier 2
  tier 2: catalog fresh (non-best) row                 # was tier 1
  tier 3: demoted / out-of-catalog backlog (unchanged)
  date  : row date (newest first within a tier, unchanged)
```

Implementation = swap the two numbers in `preconvertClassifyTier` (best → 1,
fresh → 2) plus a tier-refresh pass over the existing persisted queue at load
(the enqueuer's in-place refresh at `:2434-2444` already re-tiers on the next
catalog change, so strictly the load-time pass is optional; the next scrape ≤
3 h away re-tiers everything). `preconvertPickEligible` itself is unchanged.

Rationale: best-first buys a *permanently* 100 %-ready BEST tab for a one-time
~12 h and ~2 % steady-state capacity (§3.4); fresh newest-first then maximizes
RECENT freshness. During the one-time backfill the RECENT tab (already
ready-filtered by Stage 2) keeps working — its freshness temporarily degrades
(worst case ~+12 h of span), a bounded transient. Option (not recommended, or
only if the transient bothers): interleave 1 fresh per N best during backfill.

### 4.3 What does NOT change

- **Real user requests always win**: live convert/watch preempts a background
  job instantly (`job.background` ⇒ always-stale in `findPreemptableJob`,
  §Q1 of plan-preconvert-fleet.md; verified live: 0 preempts needed in 24 h
  because the device wasn't browsing). Untouched.
- Gap pacing (180 s + jitter), tick cadence, disk floor, failure-ledger
  policy, lease semantics: untouched.
- Wire `convert` op stays catalog-gated (`fcade-proxy.js:1861-1867`).

---

## 5. UX contract

**Browse = READY.** Enforced server-side in `searchCatalog`
(`fcade-proxy.js:495-524`): when the new env gate `FCADE_SEARCH_READY_ONLY=1`
is set **and** the request has no `username`, drop rows whose quarkid is not
store-servable before sorting/paging.

- RECENT and BEST tabs ⇒ every row instant-play. **Zero device change**
  (verified chain in §1.4: rows are shape-identical; the wrapper's one-shot
  `convertstatus` will simply always answer `ready`, `fcade-proxy.js:1913-1916` →
  `replay_proxy.c:995-996`).
- BY PLAYER (username present) ⇒ **unfiltered, explicitly best-effort**: the
  row-action status line stays honest (`not converted yet` / `CONVERTING n%`,
  patch `:1633-1641`) and "Watch now (live)" is always offered (patch
  `:1650`) — first frame ≈ 15–30 s. A username-initiated convert/watch also
  *warms the pool* (store truth removes it from the queue, `:2469-2481`).
- Cost note: readiness check = `convertStoreServableGames` per row (one
  readdir + header reads, `:1237-1246`) × ≤294 rows per search. Acceptable
  at OSD browsing rates; if it ever shows up, cache the ready-set keyed on
  (catalog mtime, store generation) — invalidation hooks already exist at
  publish/workdone/evict. Not in scope for Stage 2 unless measured slow.
- Honesty invariant: the filter must never *add* readiness — `ready` is still
  only ever derived from store truth. The `hit_status_ready/absent` counters
  (`:1906-1920`) become the acceptance metric: after the flip, absent hits
  from tab browsing should go to ~0 (only BY PLAYER can produce them).

Pagination: unchanged. The wrapper pages by 10 with "full page ⇒ maybe more"
(patch `:349`); a filtered feed just ends a page early, which the wrapper
already handles (same as a short raw feed).

---

## 6. Queue + store hygiene

### 6.1 Queue (P3 backlog)

P3 = 1,717 of 2,000 items and grows ~900/day (§3.5). Under the flip, P3 is
never converted (tier 2 never drains) — it is dead weight whose only residual
value, "best-lists resurface old quarks" (plan-preconvert-fleet.md §Q1), does
NOT require keeping it: a resurfacing quark re-enters via the enqueuer's
new-row path (`fcade-proxy.js:2447-2461`) as long as it isn't
ledger-blocked. The stored row snapshot is only needed to convert a quark
*while* out-of-catalog — a thing the flip deliberately stops doing.

**Policy: age out P3.** In `preconvertEnqueueFromCatalog`, after the
demotion pass: drop items with `tier === 3` whose `date` is older than
`FCADE_PRECONVERT_P3_MAX_AGE_MS` (default 48 h), plus a hard queue cap
`FCADE_PRECONVERT_QUEUE_MAX` (default 2,500; evict oldest-date P3 first,
never P1/P2). Steady state: queue ≈ 284 visible-pending + ≤ ~1,800 recent P3
⇒ state file bounded ≈ 650 KB. Alternative presented (§8 D5): drop-on-demotion
(no P3 at all) — simplest, loses nothing under the flip; 48 h default kept
only as a hedge for a quark that briefly leaves and re-enters the recent
window between scrapes.

Failure ledger: unchanged (already bounded, `:235`, `:2256-2259`). The 34
`publish_error` entries retry on the existing backoff. Separate follow-up
(not this plan): root-cause `publish_error` — 34/522 ≈ 6.5 % of jobs.

### 6.2 Store eviction: pinning + low-water

Two changes to `evictStoreIfNeeded` (`fcade-proxy.js:984-1021`):

1. **Pin the pool.** New predicate `isQuarkPinned(quarkid)` — true iff the
   quarkid is in the *current* catalog (both feeds) or on the attract list
   (if any). Pinned quarks are skipped exactly like active-job quarks (still
   counted toward caps, same anti-thrash reasoning as `:980-983`). Without
   this, at cap the LRU (`served` ≈ conversion-time mtime for never-watched
   quarks, `:972`) evicts the *earliest-converted* quarks first — which after
   Stage 1 is precisely the BEST layer. Pool size (~300–500) is ≤ 5 % of the
   cap, so pinning can never wedge eviction; keep the existing
   "still over cap" warn (`:1014-1019`) as the tripwire.
2. **Low-water.** Evict down to `FCADE_STORE_EVICT_LOW_WATER` (default 0.85 ×
   each cap) instead of exactly-at-cap, so the 90 % churn guard
   (`:2395-2401`) can never latch permanently (§3.5 stall). Guard threshold
   itself unchanged.

Caps stay 10,000 / 2 GiB (systemd): at ~108 KB/quark the byte cap is not
binding, and 10 k ≈ 35 days of rolling history at current throughput —
enough that anything still catalog-reachable is comfortably retained.

**Review amendments (2026-07-28, applied in the FIX phase, not a re-plan):**

1. **Catalog-fail-open.** `evictStoreIfNeeded` reads the same "is a catalog
   configured" signal `loadCatalog()` uses (`FCADE_CATALOG_FILE` non-empty).
   If configured but `loadCatalog()` returns `null` (missing/unreadable/
   corrupt file), the WHOLE pass is skipped with a `logWarn` — an empty
   pinned set is never substituted, because that would let this pass evict a
   quark that IS in the (temporarily unreadable) catalog. The 10-min slow
   timer (`STORE_EVICT_SWEEP_MS`) retries. No catalog configured at all is
   unaffected — an empty pinned set is correct there, and the pass proceeds.
2. **Trigger fraction = the shared churn-guard fraction.** The original
   design (above) evicted only when strictly OVER the raw cap, while the
   churn guard trips at 90 % of cap — a dead zone existed between 90 % and
   100 % where the guard had already latched (new pre-convert enqueues
   stopped) but eviction was still dormant. `evictStoreIfNeeded`'s trigger is
   now "at/above `STORE_EVICT_TRIGGER_FRACTION`" (extracted as a single
   constant, `0.9`, shared with `preconvertStoreAtChurnCap`, `fcade-proxy.js:
   2500` area) instead of "over the raw cap". The low-water target (0.85)
   sits below the 0.9 trigger, giving a hysteresis band so the guard
   unlatches and stays unlatched after an eviction pass. The pre-existing
   "still over cap" tripwire warn is unchanged — it still compares against
   the RAW caps, not the trigger fraction.
3. **Env hygiene.** `FCADE_STORE_EVICT_LOW_WATER=''` (or any non-numeric
   value) now falls back to the 0.85 default instead of silently evaluating
   `Number('') === 0` and clamping to the 0.5 floor. A startup `logWarn`
   fires if the effective low-water value is `>= STORE_EVICT_TRIGGER_FRACTION`
   (0.9), since that would collapse the hysteresis band from point 2.

---

## 7. Attract layer (optional, sketch only — separate stage, user decision)

- Server: a curated list file `/opt/fcade-proxy/attract.json`
  (`[quarkid, …]`, hand-edited or "top-N by rank from recent best feeds"),
  merged into the eviction pin-set; a trivial `attract` op returns a random
  member's row (or reuse `search` with a new flag). All members are, by
  pin + pool membership, always READY.
- Device (wrapper patch): an "Attract" entry on the REMOTE tab root that
  requests a random pool row and enters the existing play flow
  (`replay_play_live_handoff` / get3sr path, patch `:1589-1599`) in a loop —
  on `REPLAY COMPLETE`, pick the next random row. No new wire ops strictly
  required if `search`+random-pick client-side is acceptable.
- Not designed further here; ships only if D6 (§8) is a yes.

---

## 8. User decision points

| # | Decision | Default in this plan | Notes |
|---|---|---|---|
| D1 | `CONVERT_MAX_JOBS` = 2 | **Stays 1.** | Proven liftable network-side (5 concurrent same-IP different-source-port pulls verified), but it is a stealth posture change AND needs code (fixed `--local-port 6004`, `fcade-proxy.js:1769-1772`, would bind-fail; per-slot ports required). Not needed for the guarantee — only buys freshness (≈ halves the RECENT span). |
| D2 | Lower `PRECONVERT_GAP_MS` below 180 s | **No.** | Stealth (patient-human-binge profile, `:222-229`). Same class as D1: freshness only. |
| D3 | Mac-worker cadence up (more ticks / >3 per lease) | **No change.** | Server clamps 3/lease (`:257`). Worker adds ~15/day today. |
| D4 | Raise scrape `MAX_ROWS` (150/feed) or scrape frequency | **No.** | The feed is saturated (§3.2) — raising it widens *observation*, not the guarantee; it is a scrape-volume/stealth decision. The ready-filter works at any cap. |
| D5 | P3 policy: 48 h age-out (default) vs drop-on-demotion | **48 h age-out.** | Drop-on-demotion is simpler and loses nothing under the flip; kept as the leaner alternative. |
| D6 | Attract layer (curated pinned pool + device autoplay) | **Deferred**, sketched §7. | Only stage with device code. |
| D7 | Best-backfill interleave (1 fresh per N best during the ~12 h backfill) | **No interleave** (strict best-first). | Transient is bounded; interleave adds scheduler complexity. |

Zero new server cost throughout: everything runs on the existing VPS
(disk 18 % used; pool ≈ 55 MB worst case) and existing Mac agents.

---

## 9. Staged implementation plan

Conventions: every stage is independently shippable and gated; tests in
`tools/fcade-proxy/__test_*.js` (7 suites) must stay green at every stage
(`node __test_preconvert.js` etc.); deploy via the existing
`tools/fcade-proxy/deploy.sh` + systemd restart; rollback per stage listed.
No pushes/deploys without explicit user go, per standing rules.

### Stage 1 — Priority flip (proxy-code)
- Change: `preconvertClassifyTier` best→1, fresh→2 (`fcade-proxy.js:2389-2391`);
  update `preconvertStats` tier labels if needed (`:2328-2336`); extend
  `__test_preconvert.js` with an ordering test (best row picked before newer
  fresh row; P3 still last; newest-first within tier).
- Gate: all 7 suites green locally. Post-deploy: within 24 h,
  `python3` catalog∩store probe (§0 one-liner) shows BEST ready ≥ 100/144
  (from 1/144); journal shows best-dated quarks converting.
- Rollback: redeploy previous `fcade-proxy.js` (no state migration — tiers
  re-derive from the catalog on the next enqueue pass).

### Stage 2 — Ready-only catalog serving (proxy-code + VPS-config flip)
- Change: `FCADE_SEARCH_READY_ONLY` env gate; in `searchCatalog`, when set
  and `canon.username` is empty, filter rows through
  `convertStoreServableGames` before sort/page. New tests in
  `__test_catalog.js` (filtered vs unfiltered, username exemption, empty-page
  end). Code default **off**; turning on = adding the env line to the systemd
  unit (user go).
- Gate: suites green. Post-flip probe over SSH: `search` (no username)
  returns only store-servable quarkids; `search` with username still returns
  unconverted rows; device RECENT/BEST tabs show rows that all answer READY
  on pick. `hit_status_absent` stops incrementing during tab browsing.
- Rollback: remove the env line + restart (config-only, instant).

### Stage 3 — Eviction pinning + low-water (proxy-code)
- Change: catalog/attract pin-set predicate threaded into
  `evictStoreIfNeeded`; low-water eviction target (§6.2). Extend
  `__test_store_evict.js`: over-cap store evicts only unpinned; evicts to
  low-water; pinned-over-cap emits the warn.
- Gate: suites green. Live: no functional change until the cap approaches
  (~30 days out, §3.5) — gate is test-level + a one-off forced check via the
  existing test hook `_evictStoreIfNeeded` (`fcade-proxy.js:2762`) against a
  scratch store dir. Deploy well before ~2026-08-27.
- Rollback: redeploy previous file (behavior reverts to exactly-at-cap LRU).
- **Review amendments (2026-07-28, FIX phase — see §6.2 for detail):**
  catalog-configured-but-unreadable now fails the whole pass open (skip +
  warn, not empty-pins); the eviction trigger moved from "over the raw cap"
  to "at/above the shared 0.9 churn-guard fraction" (`STORE_EVICT_TRIGGER_
  FRACTION`, one constant shared with `preconvertStoreAtChurnCap`) so the
  guard can never latch with eviction dormant; `FCADE_STORE_EVICT_LOW_WATER`
  env parsing now defaults empty/non-numeric to 0.85 (was silently clamping
  to 0.5) and warns at startup if the effective value collapses the
  hysteresis band against the trigger fraction. `__test_store_evict.js`
  gained two tests (catalog-fail-open; trigger-at-90%-not-over-cap dead
  zone); existing eviction tests in `__test_store_evict.js` /
  `__test_hardening.js` were checked for "at-cap-or-below never evicts"
  assumptions — none existed (every fixture was already seeded strictly over
  the raw cap), so no other test needed recomputed expectations.

### Stage 4 — Queue hygiene (proxy-code)
- Change: P3 age-out + queue hard cap in `preconvertEnqueueFromCatalog`
  (§6.1), env-tunable. Tests: demoted item dropped past age; cap evicts
  oldest-date P3 only; P1/P2 never dropped.
- Gate: suites green. Post-deploy: `preconvert-state.json` queue length falls
  from ~2,000 to ≈ (visible-pending + ≤48 h P3) within one catalog-refresh
  cycle (≤ 3 h); state file size drops accordingly.
- Rollback: redeploy previous (dropped P3 items are re-addable only if they
  resurface in a catalog — acceptable by design; note this makes rollback
  forward-safe but not data-restoring).

### Stage 5 — Optional throughput levers (user-decision; VPS-config and/or proxy-code)
- Only on explicit user go per §8 D1–D4. D1 additionally requires the
  per-slot local-port change (proxy-code) before `FCADE_CONVERT_MAX_JOBS=2`
  is honored safely.
- Gate (if taken): journal completion cadence reflects the new rate; zero
  `bind` errors; stealth posture re-reviewed by user.
- Rollback: env revert (D2–D4) / env revert + optional code revert (D1).

### Stage 6 — Attract mode (device-code + proxy-code; optional, §7, D6)
- Separate plan if greenlit; the only stage touching the wrapper patch.

---

## 10. Non-goals

- Device-direct predict-search-correct replay (separate closed investigation,
  memory `project-device-direct-replay.md`).
- Any change to the live convert/watch path, preemption, lease protocol,
  wire ops, or the `.3sr` format.
- Raising scrape volume or conversion aggressiveness without an explicit
  user decision (§8).
