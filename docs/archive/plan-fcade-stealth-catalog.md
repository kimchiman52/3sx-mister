# Plan: Autonomous Stealth-Browser Fightcade Catalog Refresh

**Status:** PLAN phase (plan → implement → review → verify → fix). Nothing
implemented. Branch `feat/fcade-replay-browser`.

**Goal:** Replace the one irreducible human step in the catalog-refresh
workflow — a person pasting `browser-catalog.js` into a logged-in
`fightcade.com` tab to pass Cloudflare — with an unattended automated
stealth browser that (a) passes Cloudflare's managed challenge, (b) issues
the authenticated `searchquarks` JSON POST, (c) emits `catalog.json`, and
(d) hands it to the existing push/verify path with zero changes.

This plan presents OPTIONS with tradeoffs and a recommendation. The two big
architecture calls (which engine; where it runs) are the user's to make —
the plan's job is to make them cheap to decide via a first spike.

---

## 1. Problem statement + current-state map (cited)

### 1.1 Where the human step is, exactly

The refresh pipeline is four steps; only step 1 needs a human:

1. **Generate (HUMAN)** — a person opens a logged-in `fightcade.com` tab,
   pastes `tools/fcade-proxy/browser-catalog.js` (or clicks its bookmarklet),
   and it pages `searchquarks` via the tab's own `fetch('/api/', …)`
   (`browser-catalog.js:119-127`), producing `fcade-catalog.json` downloaded
   to `~/Downloads` and copied to the clipboard (`browser-catalog.js:214-247`).
2. **Detect + validate (automated)** — `refresh-catalog.sh` polls
   `~/Downloads` for the newest `fcade-catalog*.json` after start
   (`refresh-catalog.sh:297-337`), waits for the size to stabilize, and
   shape-validates it (`refresh-catalog.sh:134-198`, `:370-397`).
3. **Push (automated)** — `push-catalog.sh` rsyncs it to the VPS as
   `catalog.json` (`push-catalog.sh:11`; called from `refresh-catalog.sh:404`).
4. **Verify (automated)** — `refresh-catalog.sh` SSHes to the target and
   speaks the wire protocol's `{"op":"status"}` against `127.0.0.1:3479`,
   asserting `mode:"catalog"` with matching `catalog_rows`/`generated_at`
   (`refresh-catalog.sh:208-289`, `:414`).

The README states this plainly: "The **only** step that still requires a
human is passing Cloudflare in a real browser tab (step 1 itself)"
(`tools/fcade-proxy/README.md:366-368`, `:540-547`). This plan targets step 1.

### 1.2 The exact request the automation must reproduce

The `searchquarks` call has one shape, defined in three kept-in-sync places:

- Browser snippet: `POST /api/`, `credentials: 'same-origin'`, body
  `{req:'searchquarks', gameid, offset, limit[, best][, since]}`, headers
  `Accept`/`Content-Type: application/json;charset=UTF-8`
  (`browser-catalog.js:112-127`).
- VPS proxy live path: `POST https://www.fightcade.com/api/`
  (`fcade-proxy.js:42`), body `{req:'searchquarks',offset,limit,gameid[,best][,since][,username]}`
  plus `Origin`/`Referer: https://www.fightcade.com/game/<gameid>`/`User-Agent`/`Cookie`
  headers (`fcade-proxy.js:363-381`).
- Python reference: `tools/fcade-replays/fcade_replay_tool.py:117-168`.

Response shape the snippet reads: `body.results.results[]`
(`browser-catalog.js:143`). A 403 from this endpoint is Cloudflare
(`fcade-proxy.js:399-404`).

**Paging done today** (`browser-catalog.js:54-57,151-208`): gameid
`sfiii3nr1`; "Recent" (`best:false`, offset 0,15,30… up to `MAX_ROWS=150`) +
"Best this month" (`best:true`, `since` = UTC month start); merged and
de-duped by `quarkid`, `catalog_best:true` tag preserved; `REQUEST_DELAY_MS=400`
politeness delay between pages. Any automation must preserve this politeness
(the proxy independently rate-limits upstream at 2000 ms spacing —
`README.md:549-559`).

### 1.3 The catalog data shape (the drop-in contract)

Output must be exactly (`README.md:451-470`, `browser-catalog.js:83-105,211`):

```json
{ "generated_at": <ms-epoch>, "gameid": "sfiii3nr1",
  "rows": [ { "quarkid": "...", "date": <ms|null>, "duration": <n|null>,
             "players": [{"name","country","rank","score"}], "ranked": <bool|null>,
             "num_matches": <n|null>, "emulator": <str|null>, "gameid": "...",
             "catalog_best": <bool> } ] }
```

Row normalization must mirror `fcade-proxy.js`'s `normalizeRow()`/
`normalizePlayer()` (`browser-catalog.js:69-105` mirrors
`fcade-proxy.js:120-144`). `refresh-catalog.sh`'s validator only hard-requires
non-empty `rows[]`, each with a string `quarkid` and a `players` array
(`refresh-catalog.sh:159-182`) — but the proxy re-normalizes every row anyway
(`README.md:476-480`), so extra fields are harmless. **The live catalog is
currently 295 rows** (`fcade-replay-notes.md:1166-1168`, "catalog_rows:295").

### 1.4 What the proxy expects; why nothing downstream needs to change

The proxy picks up a new `catalog.json` by mtime on the very next `search`
request, no restart (`README.md:484-491`). If the automation emits the exact
file shape above and hands it to `refresh-catalog.sh`/`push-catalog.sh`, the
entire push/verify/serve tail is a **drop-in** — zero changes required. The
only substitution is "human clicks bookmarklet" → "scheduled stealth browser
produces the same file."

**Flag (brief vs. repo):** The task brief says FlareSolverr was proven to
fail (clears CF but can't POST JSON). **A repo-wide grep for `flaresolverr`
returns zero hits** — FlareSolverr is not mentioned in any doc, script, or
code here. The FlareSolverr "cannot send an arbitrary in-page JSON POST"
limitation is a real, well-known property of FlareSolverr's API (it returns
solved cookies + rendered HTML, not an arbitrary authenticated XHR result),
but it was **not** recorded as tested against fightcade in this repo. Treat
the FlareSolverr claim as *plausible-but-unverified-here*. What the repo
**does** verify as failed is below (§1.5).

### 1.5 What is PROVEN to fail (cited, repo-verified)

- Plain `node`/`curl` with any headers (`README.md:331`).
- A real Chrome TLS fingerprint via `curl_cffi` (`README.md:332`,
  `browser-catalog.js:9-11`).
- **Playwright, both headless AND headed** — "automation is detected either
  way" (`README.md:333-336`, `browser-catalog.js:11-13`).
- Plain `curl` from the dev network got a bare 403 with no cookie
  (`fcade-replay-notes.md:90-93`).

The only thing proven to pass is "a real, everyday browser tab already logged
into fightcade.com" (`README.md:337-341`). Note the phrase **"already logged
into"** — see open question OQ-1 (§8): it is unverified whether `searchquarks`
needs an authenticated Fightcade *account* session in addition to
`cf_clearance`, or CF clearance alone.

### 1.6 The cf_clearance IP/UA-binding constraint (cited)

`cf_clearance` "can be IP-bound" and UA-bound; if it is bound to the browser's
IP rather than the consumer's, requests 403 (`README.md:291-296`,
`fcade-proxy.js:399-404`, `plan-fcade-replay-browser.md:546-553,1586-1590`).
A managed challenge "is not a one-time clearance cookie, it can re-trigger per
request/session" (`README.md:339-341`). This is *the* central constraint the
cookie/session strategy (§4) must design around.

---

## 2. Key insight: why a real-browser automation can do what FlareSolverr/curl can't

FlareSolverr and `curl_cffi` fail on the *POST* half even when they pass CF,
because the authenticated `searchquarks` call is a same-origin XHR issued from
page JS carrying the tab's cookies (`browser-catalog.js:119-127`). Any tool
that drives a **real browser** (nodriver → real Chrome; patchright → real
Chromium; camoufox → real Firefox) can execute *exactly* the manual snippet
inside the page via `evaluate()` after the challenge clears. So the automation
is literally "run `browser-catalog.js` programmatically instead of by human
paste." This is verified in principle — it is the same code path the snippet
already uses; what is unverified is whether the *automated* browser clears the
challenge at all (§3, and the spike in §7).

---

## 3. Options for the stealth-browser engine

All benchmark figures below are from a **May 13–Jul 12 2026** public benchmark
(7 tools × 31 CF/anti-bot targets) and general 2026 scraping literature — see
Sources. **CRITICAL CAVEAT (zero-assumptions):** *none* of these benchmarks
tested `fightcade.com`. The repo already proves headed+headless Playwright and
`curl_cffi` fail on fightcade specifically (§1.5). Generic-benchmark success
therefore does **not** verify success against fightcade's particular managed
challenge. Every "clears CF" claim below is **VERIFIED only against generic CF
targets; UNVERIFIED against fightcade → requires the live spike (S0).**

| Engine | Lang | Generic CF result | Can POST after clear? | Maintenance | Footprint | Unattended fit |
|---|---|---|---|---|---|---|
| **nodriver** | Python | 28/31 OK, **0 blocked** — sole tool to pass the hardest gate | Yes (real Chrome, CDP `evaluate`) | Active, AGPL-3.0 | Chrome + deps | Good (headless Chrome) |
| **patchright** | Py/Node | 25/31 OK, 3 blocked | Yes (real Chromium) | Active, Apache-2.0, tracks Playwright | **peaks ~13GB RSS** in benchmark | OK but heavy |
| **camoufox** | Python | 25/31 OK, 3 blocked | Yes (real Firefox) | Active | 200MB+ RAM/instance; **~42s per CF bypass** | OK, slower |
| **puppeteer-extra-stealth** | Node | <20% vs managed/Turnstile | Yes | **Deprecated Feb 2025**, no meaningful update since Mar 2023 | Chromium | Poor — reject |
| **rebrowser-patches** | Py/Node | 24/31 (≈ vanilla in benchmark) | Yes | **Last commit Sep 2024, ~unmaintained** | Chromium | Poor — reject |
| **Persisted real-Chrome profile + pinned UA over CDP** | any | = whatever the profile achieves | Yes | You own it | Full Chrome | Depends (see below) |

### 3.1 Per-candidate notes

- **nodriver** (undetected-chromedriver successor). Drives real Chrome over
  CDP directly, no Playwright/Puppeteer middleware layer — which is precisely
  the layer whose `Runtime.enable`/CDP fingerprint Cloudflare gates on. Won
  the benchmark's hardest gate outright (the one all six patched
  Chromium/Firefox variants failed). Python. AGPL-3.0 (see OQ-6). **This is
  the strongest generic evidence of the candidates.**
- **patchright** — patched Playwright, cleanest drop-in if we ever want the
  Playwright API. Actively maintained, tracks upstream within days. But the
  benchmark measured a ~13GB RSS peak — a real concern for a small VPS (§5).
- **camoufox** — Firefox fork with C++-level anti-fingerprint patches, 0%
  headless-detection on standard tests; but 200MB+/instance and ~42s per CF
  clear. Different engine (Gecko) than the human's likely Chrome — a
  double-edged sword (novel fingerprint vs. UA/engine mismatch risk).
- **puppeteer-extra-stealth / rebrowser-patches** — **reject**: the first is
  deprecated and easily detected in 2026; the second is effectively
  unmaintained since Sep 2024. Listing them only to record they were
  evaluated and dropped.
- **Persisted real-Chrome profile + long-lived cf_clearance + UA pinning over
  CDP** — the lowest-novelty option: automate *exactly* what the human does —
  a real Chrome, a real logged-in profile, real cookies, driven by CDP only to
  trigger the fetch. Lowest arms-race exposure *if* it works, because it is
  closest to "a real everyday tab" (the one thing proven to pass, §1.5). Risk:
  the moment CDP attaches, the `Runtime.enable`/automation fingerprint may
  reappear — which is exactly what killed headed Playwright here already
  (§1.5). Whether a bare-CDP attach to an otherwise-pristine profile is
  detected is **unverified — needs the spike.**

### 3.2 Recommendation (user decides)

**Recommended first engine to spike: `nodriver`**, because (1) it has the
strongest generic evidence against the hardest CF gates, (2) it drives real
Chrome so it can run the exact same-origin fetch the snippet uses, and (3) its
footprint (one headless Chrome) is far lighter than patchright's 13GB benchmark
peak. **Fallback order if the spike fails: camoufox (different engine may
dodge a Chrome-specific gate) → persisted-real-profile-over-CDP.** Reject
puppeteer-extra-stealth and rebrowser-patches outright.

**But do not commit to any engine before S0.** The decisive fact — does *this*
tool clear *fightcade's* challenge — is unknown for every candidate and is
cheap to test (§7). The recommendation is a spike order, not a final pick.

---

## 4. Cookie / session strategy

### 4.1 The elegant property this design has that the live-cookie path lacks

In the **live-proxy** path, a browser-derived `cf_clearance` is copied to a
*different* machine (the VPS) and replayed from a *different* IP — which is
exactly where IP-binding bites (`README.md:291-296`). The **stealth-browser-
generates-catalog** design sidesteps this entirely: the browser clears CF
**and** issues the POST **and** emits the catalog all on one machine, so the
cookie never crosses an IP boundary. cf_clearance IP-binding is therefore a
non-issue *within* a run — the cookie is used only where it was minted.

### 4.2 What remains: datacenter-IP reputation and refresh cadence

- **IP reputation, not IP-binding, is the live risk.** CF composites IP
  reputation into its trust score; a Hetzner **datacenter** IP (46.62.244.55)
  typically draws harder challenges than a **residential** IP. So "run the
  browser on the VPS" may face a *stricter* challenge than the human's home
  connection ever did. **Unverified — the spike must be run from the VPS IP
  specifically, not just from the dev Mac**, or the S0 result won't generalize.
- **Refresh cadence.** Because each run mints and consumes its own clearance,
  there is no long-lived cookie to babysit. The browser solves the challenge
  fresh each scheduled run. If a run fails the challenge, it retries/backs off
  (§6). No cookie file to rotate — a strict simplification over the live path.
- **Login session (if required — OQ-1).** If `searchquarks` needs an
  authenticated Fightcade account (the manual flow says "logged in",
  `README.md:280-281,347`), the runner must persist a logged-in browser
  profile (username/password login once, reuse the session cookie across runs)
  — a secret-management concern (store Fightcade creds in the runner's
  environment/secret store, never in-repo, matching the cookie-never-in-repo
  posture at `README.md:6-8,262-263`). If CF clearance alone suffices, no
  credentials are needed. **This is the single biggest unknown and the spike
  must resolve it.**

---

## 5. Where it runs

| Option | Pros | Cons |
|---|---|---|
| **A. On the VPS (46.62.244.55), next to the proxy** | Unattended by default (already a 24/7 host); cookie+POST+catalog all co-located; `push-catalog.sh`/verify become a `localhost` cp instead of rsync+ssh | Needs Chromium + deps on the VPS (~400MB+ install; RAM — patchright's 13GB peak would be fatal, nodriver's single headless Chrome is far lighter); **datacenter-IP may face harder CF challenge (§4.2) — the core risk** |
| **B. On the dev Mac (scheduled)** | Residential IP → likely easier CF challenge; matches the human's proven-good environment | **Mac's Docker/disk is currently constrained** (`fcade-replay-notes.md:163-186` ENOSPC history; task brief); Mac not always on → not truly unattended; still needs rsync+ssh to VPS (already built, `refresh-catalog.sh`) |
| **C. A separate small always-on box / residential-egress runner** | Residential-ish IP + always-on + isolated from the constrained Mac and the VPS | New hardware/hosting to provision and maintain; more moving parts |

**Design constraint honored:** prefer a design that does **NOT** depend on the
dev Mac's Docker. All three options above run the stealth browser **natively**
(headless Chrome/Firefox), not in Docker — none require the Mac's Docker
daemon. (Docker on the Mac is only used today for the ARM game build,
`fcade-replay-notes.md:9.1` — unrelated to this workflow.)

**Recommendation (user decides):** Spike from **both** the VPS IP (Option A)
and a residential IP (Option B, ad-hoc, no Docker) in S0, because the *only*
thing that determines A-vs-B viability is whether the datacenter IP clears the
challenge — a fact we don't have. If the VPS IP clears it, Option A is the
clean unattended answer (co-located, no rsync). If only the residential IP
clears it, fall to Option C (a small always-on residential-egress runner) or
accept a Mac-scheduled job (Option B) with its "not always on" caveat.

---

## 6. Integration, failure modes, and fallback

### 6.1 Integration (zero-to-minimal change)

- **Ideal (zero change):** the runner writes `fcade-catalog.json` to a watched
  directory and invokes `refresh-catalog.sh` (or `--file` mode,
  `refresh-catalog.sh:105-109,341-347`) exactly as the human flow does. If the
  runner is **on the VPS**, `--file` + a local `push-catalog.sh` (rsync to a
  local path is a cp) + the existing `verify.js` over `localhost` all work
  unchanged.
- **Minimal change (if runner is on the VPS):** `refresh-catalog.sh`'s
  SSH-verify step (`:414`) could be simplified to a direct local
  `node verify.js` since the proxy is on the same host — optional, not required.

### 6.2 Failure detection — a real gap to design for

**Catalog mode has no "stale" signal.** When a catalog is present, the proxy
serves from it and **never attempts the live path**, so it can never return
`cloudflare_403` for a stale catalog (`README.md:513-517,529-536`). A catalog
that silently stops refreshing therefore just serves increasingly old data
with `ok:true` — there is **no** existing "catalog went stale" alarm. So
breakage detection must be built into the *runner*, not inferred from the proxy:

- The runner must treat "challenge not cleared / 0 rows / POST 403" as a hard
  failure, alert (log + notify), and **leave the last-good `catalog.json` in
  place** (never overwrite good data with an empty/failed pull —
  `refresh-catalog.sh` already refuses malformed/empty files,
  `:159-166,370-374`).
- Add a freshness check: compare the served `generated_at`
  (`{"op":"status"}` → `catalog_generated_at`) against now; page if it exceeds
  a threshold. This is new monitoring, not present today.

### 6.3 Fallback — the manual snippet MUST remain a working path

The arms race is real: "Cloudflare's detection has added signals since 2023…
per-iframe target ID auditing… JA4 TLS fingerprint" (2026 literature). When
(not if) the stealth browser breaks:

- **`browser-catalog.js` + `refresh-catalog.sh` stay exactly as they are** —
  the human paste path is the permanent fallback and must not be removed or
  modified by this work (`README.md:337-341` is the ground truth: only a real
  everyday tab is *proven* to pass). The automation is strictly additive.
- Breakage → alert (§6.2) → human runs the existing bookmarklet → pipeline
  recovers with zero code changes.

---

## 7. Staged implementation breakdown (S0…S5)

Sizing/house-style per `docs/plan-osd-replay-browser.md` (each stage ≤ ~2h
agent work, explicit success criteria, dependencies, fallback). **No stage
here requires the dev Mac's Docker** (unlike the ARM game build). Disk needs
are modest (a browser install + a <1MB JSON), not the tens-of-GB the replay
extraction hit (`fcade-replay-notes.md:163-186`).

### Stage S0 — SPIKE: prove/disprove one engine clears fightcade CF + POSTs (do this FIRST, cheapest)

**Title:** Throwaway spike — can `nodriver` (recommended first) clear
fightcade's managed challenge and run the same-origin `searchquarks` POST,
from a datacenter IP and from a residential IP?

**Why:** This is the entire go/no-go. Every later stage is wasted if no engine
clears *fightcade's* challenge (all benchmark evidence is generic; repo proves
Playwright+curl_cffi already fail here, §1.5). Cheapest possible test before
committing to automation.

**Do:** In a scratchpad (not the repo tree), install nodriver, launch headless
Chrome, navigate to `https://www.fightcade.com`, wait for the challenge to
resolve, then `evaluate()` the exact `fetchPage()` body from
`browser-catalog.js:112-149` for one page (offset 0, limit 15). Record:
challenge cleared? POST returns 200 with `body.results.results[]`? Rows parse?
**Run from two egress points: the VPS IP (46.62.244.55) and a residential IP.**
If nodriver fails, repeat with camoufox, then persisted-real-profile-over-CDP.
Also record whether login was required (OQ-1) — retry with and without a
logged-in profile.

**Success criteria:**
- At least one engine, from at least one IP, clears CF and returns ≥1 real
  `searchquarks` row via the in-page POST. Record engine + IP + login-needed.
- Explicit written verdict per engine/IP (pass/fail), so §3/§5 recommendations
  become *verified* instead of *unverified*.

**Dependencies:** none. **Docker/disk:** none (native browser, scratchpad).

**What NOT to do:** don't touch the repo tree; don't overwrite the live
`catalog.json`; keep to the 400ms+ politeness delay (`browser-catalog.js:57`);
one engine/IP at a time.

**Failure/fallback:** if *no* engine clears fightcade from *any* practical IP,
STOP and report — the honest outcome is "autonomous browser is not viable
today; manual snippet remains the only path" and this whole effort should not
proceed past S0. That is a valid, valuable spike result.

### Stage S1 — Harden the spike into a repeatable single-page fetch module

**Title:** A small, tested runner module that clears CF and fetches one page
deterministically.
**Why:** Turn the throwaway into a reliable primitive before adding paging.
**Do:** Wrap the winning engine in a script with timeouts, one retry+backoff on
challenge-not-cleared, and structured pass/fail output. Pin the UA to match
what cleared in S0.
**Success criteria:** 5/5 consecutive runs return a valid single page; a forced
failure (bad URL) exits non-zero without emitting a file.
**Dependencies:** S0 pass. **Docker/disk:** none.
**Fallback:** if flaky (<80% clear rate), revisit engine choice (S0 fallback
order) before proceeding.

### Stage S2 — Full paging → emit exact `catalog.json` shape

**Title:** Port `browser-catalog.js`'s Recent+Best paging + de-dup +
normalization into the runner; emit the drop-in file.
**Why:** Produce a byte-shape-compatible `catalog.json` (§1.3).
**Do:** Reproduce `browser-catalog.js:151-211` (Recent offsets 0..MAX_ROWS,
Best-this-month, merge/de-dup by quarkid, `catalog_best` tag,
`normalizeRow`/`normalizePlayer` mirror) inside the page via `evaluate()` —
ideally by injecting the existing snippet verbatim so there's a single source
of truth, not a third copy of `normalizeRow`.
**Success criteria:** output passes `refresh-catalog.sh`'s validator
(`:134-198`) and has a row count in the same order of magnitude as the live 295
(`fcade-replay-notes.md:1166`); diff of row *shape* against a
human-generated catalog is clean.
**Dependencies:** S1. **Docker/disk:** none.
**Fallback:** if paging trips CF mid-run (challenge re-triggers per request,
`README.md:339-341`), reduce page rate / re-solve on 403 / cap rows.

### Stage S3 — Wire into the existing push/verify tail (zero-change integration)

**Title:** Feed the emitted file to `refresh-catalog.sh`/`push-catalog.sh`
unchanged.
**Why:** Prove the drop-in claim (§1.4, §6.1) end-to-end against a **staging**
target (never clobber the live 295-row catalog until S4).
**Do:** Runner writes `fcade-catalog.json`; invoke `refresh-catalog.sh --file`
pointed at a staging path/host. Confirm `{"op":"status"}` reports
`mode:"catalog"` with the pushed count/`generated_at`.
**Success criteria:** a full unattended run produces a file that pushes and
verifies with **zero** changes to the push/verify scripts.
**Dependencies:** S2. **Docker/disk:** none (rsync/ssh only).
**Fallback:** if the runner is on the VPS, use `--file` + local push (§6.1);
no script edits needed either way.

### Stage S4 — Schedule it unattended + breakage detection + last-good safety

**Title:** systemd timer / cron on the chosen host (§5), with the §6.2 alarms.
**Why:** Eliminate the human on a cadence; make silent staleness impossible.
**Do:** Schedule the runner (recommended: on the VPS if S0 proved the
datacenter IP clears CF; else a residential-egress runner). Add: never
overwrite last-good on failure; log+notify on challenge-fail/0-rows/403; a
freshness monitor comparing `catalog_generated_at` to now.
**Success criteria:** N consecutive scheduled runs refresh the catalog with no
human touch; an injected failure leaves the prior catalog intact and fires an
alert; staleness beyond threshold pages.
**Dependencies:** S3, and the §5 where-it-runs decision. **Docker/disk:** a
browser install on the chosen host (~400MB); **on the VPS this is the only
resource ask — size RAM to the chosen engine (nodriver: one headless Chrome;
patchright's 13GB peak would be disqualifying on a small VPS).**
**Fallback:** if unattended clear-rate degrades over time (arms race), the
manual snippet (§6.3) is the standing fallback; escalate to re-spike (S0).

### Stage S5 — Document + keep the manual path as first-class fallback

**Title:** README/notes update; explicit "manual snippet is the permanent
fallback" statement; runbook for re-spiking when CF changes.
**Why:** The arms race guarantees future breakage; the recovery path must be
written down.
**Do:** Update `tools/fcade-proxy/README.md` "Offline catalog" and "Refresh
cadence" sections to describe the automated path *and* reaffirm the manual
snippet as the guaranteed fallback; add a "when the stealth browser breaks"
runbook.
**Success criteria:** a fresh operator can (a) run the automated refresh and
(b) fall back to the manual snippet, from docs alone.
**Dependencies:** S4. **Docker/disk:** none.

### Stage dependency map

```
S0  SPIKE: does an engine clear fightcade CF + POST? (VPS IP + residential)  [BLOCKING]
S1  repeatable single-page fetch module        (needs S0)
S2  full paging → exact catalog.json shape      (needs S1)
S3  wire into push/verify tail (staging)        (needs S2)
S4  schedule unattended + breakage alarms       (needs S3 + §5 decision)
S5  docs + manual-fallback runbook              (needs S4)
```

**Docker/disk-blocked stages:** none are blocked by the constrained dev-Mac
Docker/disk. S4 needs a native browser install (~400MB) + RAM on whatever host
is chosen (§5) — that is the only infra cost, and it is deliberately *not* on
the Mac's Docker.

---

## 8. Open questions (only the user or a live test can resolve)

- **OQ-1 (biggest): Does `searchquarks` require an authenticated Fightcade
  *account* session, or just `cf_clearance`?** The manual flow says "logged
  in" (`README.md:280-281,347`); the proxy live path sends only `cf_clearance`
  and has only ever seen 403 (with an invalid placeholder cookie —
  `README.md:783-800`), never a confirmed valid-cookie 200. If login is
  required, the runner must persist credentials/session (§4.2). **Resolved
  only by S0.**
- **OQ-2: Does *any* engine clear fightcade's specific managed challenge?** All
  benchmark evidence is generic; repo proves Playwright+curl_cffi fail here
  (§1.5). **Resolved only by S0.**
- **OQ-3: Does the VPS *datacenter* IP (46.62.244.55) clear the challenge, or
  only a residential IP?** Determines the entire §5 where-it-runs decision.
  **Resolved only by S0's two-IP test.**
- **OQ-4: Does the challenge re-trigger mid-paging** (`README.md:339-341`)
  frequently enough to break a 10–20-page crawl at 400ms spacing? **Resolved
  by S2.**
- **OQ-5: Cadence** — how often should the catalog refresh? Not specified today
  ("run whenever it feels stale", `README.md:539-547`). User decision for S4.
- **OQ-6: nodriver's AGPL-3.0 license** — acceptable for an internal, non-
  distributed ops tool? Likely yes (no distribution), but a user/legal call
  before standardizing on it. patchright (Apache-2.0) and camoufox are
  permissive alternatives.
- **OQ-7: Secret management** — if OQ-1 needs login, where do Fightcade
  credentials live on the runner (env/secret store, never in-repo, matching
  `README.md:6-8,262-263`)?

---

## 9. Recommended path in one paragraph

Do **S0 first and only S0** until it answers OQ-1/OQ-2/OQ-3: install
`nodriver`, from both the VPS IP and a residential IP, and try to clear
fightcade's challenge + run the in-page `searchquarks` POST (with and without a
logged-in profile). If that succeeds, the rest (S1–S5) is mechanical — reuse
`browser-catalog.js`'s exact paging/normalization inside the page, emit the
identical `catalog.json`, and feed the *unchanged* `refresh-catalog.sh` tail;
prefer running on the VPS (Option A) if its datacenter IP clears CF, else a
residential-egress runner (Option C). Keep the manual snippet as the permanent,
untouched fallback and add the staleness alarm the proxy cannot provide itself.
If S0 fails for every engine from every practical IP, the honest answer is that
autonomous browsing is not viable today and the human snippet stays — a valid
spike outcome worth the cheap test.

## Sources

- [Anti-detect browser benchmark 2026 (7 tools, 31 targets)](https://ianlpaterson.com/blog/anti-detect-browser-benchmark-patchright-nodriver-curl-cffi/)
- [Best Playwright Stealth 2026: Patchright vs Camoufox vs noDriver](https://scrapewise.ai/blogs/playwright-stealth-2026)
- [rebrowser/rebrowser-patches (GitHub)](https://github.com/rebrowser/rebrowser-patches)
- [How to Bypass Cloudflare When Web Scraping in 2026 (Scrapfly)](https://scrapfly.io/blog/posts/how-to-bypass-cloudflare-anti-scraping)
- [Best Stealth Browsers for Web Scraping in 2026 (Scrapfly)](https://scrapfly.io/blog/posts/best-stealth-browsers)
- [Playwright Anti-Fingerprinting Alternatives 2026 (BotCloud)](https://botcloud.dev/blog/playwright-anti-fingerprinting-alternatives-2026/)
