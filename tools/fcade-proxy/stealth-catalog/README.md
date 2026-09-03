# Stealth-browser Fightcade catalog runner (plan S1 + S2)

Automates the ONE irreducible human step in the offline-catalog refresh flow:
a person pasting `../browser-catalog.js` into a logged-in `fightcade.com` tab
to pass Cloudflare and page the `searchquarks` API. This runner does that with
a real, **visible** Chrome window driven over CDP by
[`nodriver`](https://github.com/ultrafunkamsterdam/nodriver), then emits a
drop-in `catalog.json` in the exact shape the proxy expects.

See `docs/plan-fcade-stealth-catalog.md` for the full design. This directory
implements Stages **S1** (reliable single-page fetch) and **S2** (full paging →
`catalog.json`). It is strictly **additive**: `../browser-catalog.js` and
`../refresh-catalog.sh` remain the permanent, untouched manual fallback.

## HEADED ONLY

nodriver **headless** is Cloudflare-detected (the POST comes back a bare 403).
A real **visible** Chrome window clears the challenge. This runner always
launches headed and there is deliberately no headless flag. On a display-less
host (e.g. a VPS) run it under a virtual display:

```
xvfb-run -a python fcade_stealth_catalog.py --out /path/catalog.json
```

## The Cloudflare recipe (verified working, S0 spike)

1. Navigate the top-level browser to `https://www.fightcade.com/api/` first —
   that CF-protected path mints an httpOnly `cf_clearance` cookie transparently
   (~5 s). It is httpOnly, so it is visible only via CDP
   `browser.cookies.get_all()`, never `document.cookie`.
2. Navigate to the homepage, then issue the same-origin `searchquarks` POST
   from page JS, so Cloudflare sees a normal in-page browser XHR.

No Fightcade login/account is needed — only `cf_clearance`.

## Verified working combo

| Component | Version |
|---|---|
| Python | **3.14.6** (CPython) |
| nodriver | **0.50.3** |
| Chrome | **Google Chrome for Testing 151.0.7922.47** (or any real system Chrome) |
| Live UA that cleared CF | `Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/151.0.0.0 Safari/537.36` |

Install gotchas (baked into the runner / documented so they don't surprise):

- **nodriver does not auto-download Chrome.** Point `--chrome` (or
  `$FCADE_CHROME`) at Chrome-for-Testing or a system Chrome. `resolve_chrome()`
  auto-detects common macOS/Linux install paths as a fallback.
- **nodriver 0.50.3's `cdp/network.py`** contains a UTF-8 `±` (`0xC2 0xB1`)
  byte. On CPython 3.14.6 it **imports cleanly** (verified) — the "non-UTF-8"
  concern from earlier notes does not manifest on this Python/nodriver build.
  If a future stricter source decoder ever fails on it, transcode that one byte
  or pin an already-imported install.

## Setup

```
python3.14 -m venv venv
venv/bin/pip install -r requirements.txt
# then point --chrome at a real Chrome binary (see above)
```

Keep the venv, any Chrome binary, browser profiles, and generated catalogs OUT
of git — `.gitignore` here already excludes them.

## Usage

**S1 — single-page smoke (no file written; exit code is the signal):**

```
venv/bin/python fcade_stealth_catalog.py --single-page \
  --chrome "/path/to/Google Chrome for Testing"
```

**S2 — full catalog → `--out`:**

```
venv/bin/python fcade_stealth_catalog.py \
  --chrome "/path/to/Google Chrome for Testing" \
  --out /tmp/fcade-catalog.json
```

Flags: `--gameid` (default `sfiii3nr1`), `--max-rows` (per-feed cap, default 150
= `browser-catalog.js` MAX_ROWS), `--timeout` (per-nav/eval seconds, default 60),
`--profile` (persisted Chrome user-data-dir), `--out` (default
`$TMPDIR/fcade-stealth-catalog.json`). The runner **never** writes to or pushes
a live catalog — it only writes `--out`.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | PASS (single page ok / catalog written) |
| 2 | Cloudflare not cleared (cf_clearance never minted / POST kept 403ing) |
| 3 | POST returned a non-200, non-403 status |
| 4 | Cleared + 200 but zero rows |
| 5 | Setup failure (no Chrome, launch/import error) |

## What it collects (mirrors `browser-catalog.js` exactly)

- **Best this week, and only that**: `best:true`, `since` = today's **UTC**
  midnight minus 7 days (`now_ms - now_ms % 86400000 - 604800000`, the
  arithmetic of fightcade.com's own `weeklyBest` computed), offset 0, 15,
  30 … up to `--max-rows` (150).
- **No Recent (`best:false`) pass.** The device plays the weekly-best set and
  nothing else; those rows were filtered back out server-side anyway. The two
  crawls were always independent — the Best call took no input from the Recent
  result — so the best-tagged set is unchanged. The one crumb: a `quarkid` that
  appeared in both feeds used to be emitted with the *Recent* copy's field
  values (flag flipped); it now carries the Best copy's. Measured overlap in a
  real capture was zero, but never structurally zero.
- De-duped by `quarkid` (the listing can shift under a multi-page crawl), every
  row tagged `catalog_best:true`. 400 ms politeness delay between page fetches:
  the API rate-limits with an `HTTP/3 503` whose body is **not JSON**, which is
  also why a JSON parse failure counts as a failed page instead of crashing.
  Neither protection may be "simplified" away.
- `results.count` is never treated as a row total — it is `limit + 1`, a
  has-more sentinel. No total exists in the API; exhaustion is only ever
  determined by paging until a short or empty page.
- On a mid-run 403 (Cloudflare re-challenge), the runner re-solves CF once and
  retries the same offset before stopping the feed early.

## Row shape — single JS source of truth

The per-page fetch **and** the `normalizePlayer()`/`normalizeRow()` coercion run
**in the page** as the exact JavaScript from `../browser-catalog.js:73-149`
(copied verbatim into `PAGE_FETCH_JS`, marked as such). Running the identical JS
in the identical engine — rather than re-implementing type coercion in Python —
guarantees the emitted rows match the human snippet field-for-field. Only the
paging loop, the `quarkid` de-dup, the delay, and the CF re-solve live
in Python (the merge is gone with the Recent pass). There are now three
hand-synced copies of the normalize logic
(`fcade-proxy.js:120-144`, `browser-catalog.js:73-105`, `PAGE_FETCH_JS` here);
if `normalizeRow()` changes, update all three.

The output is byte-shape-compatible with `../browser-catalog.js`'s
`fcade-catalog.json` and passes `../refresh-catalog.sh`'s validator, so the
existing push/verify tail consumes it unchanged (Stage S3, not implemented here).

---

## Stage S4 — Daily unattended refresh (macOS LaunchAgent)

S4 runs the whole flow on a schedule **with no human**: a per-user LaunchAgent
wakes **8× a day, every 3 hours** (00/03/06/09/12/15/18/21 local), runs the
committed runner **headed but off-screen**, and — only on a
clean, complete crawl — hands the result to the unchanged
`../refresh-catalog.sh --file` tail (validate → push → verify). Any failure
pushes **nothing**, so the last-good catalog on the VPS is preserved.

### The pieces (all additive, in this dir)

| File | Role |
|---|---|
| `setup.sh` | Idempotent installer: builds a persistent venv, downloads Chrome-for-Testing 151.0.7922.47, makes a persistent profile, renders the plist. |
| `run-daily.sh` | The wrapper the LaunchAgent runs: crawl off-screen → (on exit 0 only) push via `refresh-catalog.sh` → record state; loud fail + notify + exit non-zero + push nothing on any error. |
| `dev.sambae.fcade-catalog.plist` | LaunchAgent **template** (`StartCalendarInterval` array, 8×/day every 3h; `RunAtLoad` false). `setup.sh` renders the absolute paths into `<install-root>/`. |
| `check-staleness.sh` | Dependency-free freshness monitor. Reads `state/last-success` and keys on the **published catalog's `generated_at`** (falling back to the push epoch on a pre-`rows=` state file), not on any exit code: exits 1 + notifies if older than a threshold (default 26h — a loose "clearly broken" backstop; pass a smaller `MAX_AGE_HOURS` arg, e.g. `check-staleness.sh 4`, to track the 3h cadence). Also exits 1 if the published row count is at/below `FCADE_MIN_ROWS`, and reports `FRESH (LOW YIELD)` below `FCADE_LOW_YIELD_ROWS`. |

### The persistent install root

Everything stateful lives **outside the repo** (and outside any ephemeral
scratchpad) under:

```
${XDG_DATA_HOME:-$HOME/Library/Application Support}/fcade-stealth-catalog/
  venv/            Python venv (nodriver)
  chrome/          Chrome for Testing 151.0.7922.47 (mac-arm64)
  profile/         persistent Chrome user-data-dir  <-- holds cf_clearance
  logs/daily.log   append-only timestamped run log
  state/last-success, state/last-run, state/last-catalog.json
  config.sh        (optional) your non-secret overrides
  dev.sambae.fcade-catalog.plist   (rendered)
```

The **profile** dir is why the daily run stays fast/quiet: the httpOnly
`cf_clearance` cookie persists there between runs. Override the root with
`FCADE_CATALOG_HOME`.

### Off-screen behavior (why, and why not headless)

Cloudflare's managed challenge is **headless-detected** (bare 403), so the
window must be a real render. `run-daily.sh` passes the runner's new
`--offscreen` flag, which adds `--window-position=-32000,-32000
--window-size=1200,900` to Chrome: the window renders for real but is parked far
off any physical display, so a scheduled run never steals focus or flashes a
window mid-screen. `--offscreen` is **OFF by default** in the runner (existing S1/S2
behavior unchanged); only the wrapper turns it on. You can override placement
with `--window-position X,Y` / `--window-size W,H`.

### Config (no secrets)

`run-daily.sh` reads these from env or an optional `<install-root>/config.sh`
(sourced). All are non-secret host aliases / paths / tunables — the SSH key
lives in `~/.ssh`, never here:

| Var | Default | Meaning |
|---|---|---|
| `FCADE_VPS_TARGET` | `hetzner-3s-arm:/opt/fcade-proxy` | rsync/ssh target for the push. |
| `FCADE_NODE_BIN` | `node` | node binary `refresh-catalog.sh` needs (pass an absolute path if node is not on the LaunchAgent's PATH). |
| `FCADE_MIN_ROWS` | `30` | Push-safety floor: refuse to write below this. **Deliberately low.** See "Sizing the floor" below. |
| `FCADE_LOW_YIELD_ROWS` | `100` | Log a loud warning below this, but still publish. Deliberately does **not** notify (the job runs every 3 h and a quiet week is not a failure); `check-staleness.sh` is the low-yield surface. |
| `FCADE_TIMEOUT` | `90` | Per-nav/eval timeout (s). |
| `FCADE_MAX_ROWS` | `150` | Per-feed row cap. |

### Sizing the floor

The floor was `100`, sized when the crawl merged Recent + Best into a live
catalog of ~278–295 rows. The weekly-best crawl pulls **one** feed capped at
150, so 100 sat two thirds of the way up the maximum possible yield and a
genuinely quiet week would trip it.

Tripping it is the expensive direction. When the floor refuses, nothing is
written, the VPS keeps serving the **last-good** catalog, and the device plays
an ever-staler set. `run-daily.sh` does exit non-zero on that path (and
notifies) — but a non-zero exit from a background LaunchAgent nobody reads is
indistinguishable from success. So:

- the floor is `30` (two full `PAGE_SIZE = 15` pages) — a *clean* crawl that
  ends under two pages is a broken window or an upstream change, never a quiet
  week;
- truncation is not the floor's job: `collect_feed` flags any dirty
  termination (eval timeout, non-200, a 403 surviving the re-solve) and
  `run_full`/`amain` then emit nothing at all;
- a yield at/above the floor but under `FCADE_LOW_YIELD_ROWS` still publishes
  and logs loudly, because the slide toward the floor is the early warning;
- the row count is recorded in `state/last-success` as `rows=`, and
  **`check-staleness.sh` monitors the published `generated_at` — not the exit
  code** — plus that row count. That is the observable to watch.

Example `config.sh`:

```sh
FCADE_NODE_BIN=/opt/homebrew/bin/node
FCADE_VPS_TARGET=hetzner-3s-arm:/opt/fcade-proxy
```

### node / `FCADE_NODE_BIN`

`refresh-catalog.sh` shells out to **node** for its JSON validator and the SSH
wire-protocol verify step. Node is installed **separately**; point
`FCADE_NODE_BIN` at it (LaunchAgent env strips PATH, so an absolute path is
safest). `run-daily.sh` passes `FCADE_NODE_BIN=<node>` through to
`refresh-catalog.sh`.

### Install / test / load runbook

**1. Provision the runtime (idempotent):**

```sh
tools/fcade-proxy/stealth-catalog/setup.sh
```

Re-running is safe — it skips an existing venv, an already-downloaded Chrome,
and the existing profile. It prints every resolved path and renders the plist
to `<install-root>/dev.sambae.fcade-catalog.plist`.

**2. Test a single run by hand (does a real crawl + real push):**

```sh
FCADE_NODE_BIN=/opt/homebrew/bin/node \
  tools/fcade-proxy/stealth-catalog/run-daily.sh
tail -n 40 "$HOME/Library/Application Support/fcade-stealth-catalog/logs/daily.log"
```

The first run may be slower while the profile mints `cf_clearance`. To test the
crawl in isolation without pushing, run the runner directly with `--out
/tmp/x.json` (see the S1/S2 usage above).

**3. Install + load the LaunchAgent:**

```sh
cp "$HOME/Library/Application Support/fcade-stealth-catalog/dev.sambae.fcade-catalog.plist" \
   "$HOME/Library/LaunchAgents/dev.sambae.fcade-catalog.plist"
launchctl bootstrap gui/$(id -u) \
   "$HOME/Library/LaunchAgents/dev.sambae.fcade-catalog.plist"
```

**4. Verify it is scheduled:**

```sh
launchctl print gui/$(id -u)/dev.sambae.fcade-catalog | grep -iE 'state|runs|program'
# force one run now to smoke-test the scheduled path:
launchctl kickstart -k gui/$(id -u)/dev.sambae.fcade-catalog
```

**Unload / uninstall:**

```sh
launchctl bootout gui/$(id -u)/dev.sambae.fcade-catalog
rm "$HOME/Library/LaunchAgents/dev.sambae.fcade-catalog.plist"
```

### Staleness alarm

Catalog mode has **no** built-in "stale" signal — the proxy serves an
ever-older catalog with `ok:true`. So on every successful push `run-daily.sh`
writes `state/last-success` (epoch + `generated_at`). Check it any time:

```sh
tools/fcade-proxy/stealth-catalog/check-staleness.sh        # default 26h
tools/fcade-proxy/stealth-catalog/check-staleness.sh 48     # custom hours
```

Exit 0 = fresh, exit 1 (+ a macOS notification) = stale / never-succeeded.
`run-daily.sh` also fires a failure notification on any run that fails.

### The manual bookmarklet remains the permanent fallback

This automation is strictly additive. `../browser-catalog.js` (and its
bookmarklet) pasted into a logged-in `fightcade.com` tab, feeding
`../refresh-catalog.sh` in watch mode, is the **guaranteed** fallback whenever
the stealth browser breaks (Cloudflare arms race). Nothing here touches or
replaces it.

### Safety property

A failed or partial run pushes **nothing**. `refresh-catalog.sh` runs **only**
when the runner exits 0 (which itself requires a clean, complete crawl that met
`--min-rows`), and `refresh-catalog.sh` independently re-validates before
pushing. The prior catalog on the VPS is never overwritten by a bad pull.
