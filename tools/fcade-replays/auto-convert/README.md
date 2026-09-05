# auto-convert — Mac-side replay conversion (pre-convert fleet worker + legacy daily batch)

Two macOS LaunchAgents live here, converting Fightcade replays into playable
`.3sr` blobs and pushing them to the VPS so the MiSTer's **REMOTE** tab fills
in a rolling pool of watchable replays over time:

| Job | What it does | Cadence | Status |
|-----|--------------|---------|--------|
| `dev.sambae.fcade-preconvert-worker` (**current**) | Leases up to 3 items from the VPS's own pre-convert queue and converts+pushes them | ~every 10 min | **Stage S5** — the rail you should set up |
| `dev.sambae.fcade-convert` (legacy) | Converts a bounded batch of the newest still-unconverted catalog rows, chosen purely locally | 1×/day @ 05:00 | Superseded by the worker above; kept for manual/offline use |

Both are companions to the stealth-catalog refresh job
(`tools/fcade-proxy/stealth-catalog/`), which keeps `last-catalog.json` fresh
8×/day (headed Chrome required — unrelated to either job here).

## The pre-convert worker (`preconvert-worker.sh`) — current rail

Where the legacy batch decides what to convert entirely from a **local**
catalog file, the worker asks the **VPS's own pre-convert queue**
(`docs/plan-preconvert-fleet.md` §2, populated by its catalog-driven
scheduler) what it would like converted *right now*, via a `worklease`
request. Coordination — so the VPS's own background converter and this
worker never duplicate work on the same quark — is a lease with a TTL
(45 min): if the Mac sleeps, moves networks, or is offline, its leases just
expire and the VPS scheduler picks the quark back up on its own. No state on
the Mac has to be correct for that to work.

Each tick (one LaunchAgent firing, ~every 10 minutes):

1. **`worklease`** — claim up to `FCADE_PRECONVERT_WORKER_COUNT` (default
   **3**, server-clamped to 3 regardless) queue items. The response includes
   each item's full catalog **row snapshot**, so a quark that has since
   fallen out of this Mac's local catalog copy can still be converted.
2. For each leased quark:
   - write a synthetic one-row catalog (`{"rows": [<row snapshot>]}`),
   - if `$OUT_DIR/<quarkid>/game_*.3sr` doesn't already exist (the **shared**
     out-dir, same one `convert-batch.sh` uses, is still the local "already
     converted" source of truth), run `publish_3sr.py --quark` from
     `$RUNNER_DIR` (same CWD gotcha as the legacy batch, below),
   - if that produced `>=1` `game_N.3sr`, push it to the VPS's **staging**
     dir via `push-3sr.sh --incoming`, fix perms, then **`workdone
     {ok:true}`** — the proxy validates the staged files (magic/size/meta/
     named-players, the exact `get3sr`-serving check) and atomically renames
     them into the served store; a validation failure is rejected, never
     served,
   - if nothing was produced, **`workdone {ok:false, reason}`** — the proxy
     records it in its own failure ledger (retried later with backoff) and
     frees the lease immediately rather than waiting out the full 45 min,
   - sleep a jittered 60–120s before the next quark (politeness pacing,
     `docs/plan-preconvert-fleet.md` §Q5 — at most one Fightcade connection
     at a time, same as the legacy batch: `publish_3sr.py` is strictly serial).

`proxy_ops.py` is the wire client: it speaks the proxy's
u32be-length-prefixed-JSON framing (`tools/fcade-proxy/README.md` "Wire
protocol") over `ssh -W 127.0.0.1:3479 <host>` — OpenSSH's own stdio-
forwarding primitive, piping this process's stdin/stdout straight to the TCP
connection sshd makes, on the VPS, to its own loopback-only proxy port. No
`nc`, no remote script, nothing new to provision server-side. It never talks
to the device-facing ops (`search`/`status`/`get3sr`/`convert`/
`convertstatus`/`watchpoll`) — only `worklease`/`workdone`/`workstats`.

### Token configuration

`FCADE_WORK_TOKEN` is a **secret**, unlike everything else the worker reads
(which lives in the non-secret, still-not-committed `config.sh`). It must
match the value configured in the VPS `fcade-proxy` service's own
`FCADE_WORK_TOKEN` env (checked with `crypto.timingSafeEqual` server-side;
`docs/plan-preconvert-fleet.md` §2 Q2 / Stage S4). Configure it one of two
ways (mirrors the proxy's own `FCADE_COOKIE`/`FCADE_COOKIE_FILE` convention,
`tools/fcade-proxy/README.md` "Cookie"):

1. **File (preferred)** — create `$INSTALL_ROOT/secrets.sh`, **outside the
   repo** (default install root already is), containing:

   ```sh
   # $INSTALL_ROOT/secrets.sh -- chmod 600, NEVER commit
   export FCADE_WORK_TOKEN=<the value configured on the VPS>
   ```

   ```sh
   chmod 600 "$INSTALL_ROOT/secrets.sh"
   ```

   `preconvert-worker.sh` sources this file automatically if present (and
   warns if its permissions are looser than `600`). This file is `.gitignore`d
   in this directory as a defense-in-depth measure even though the install
   root already lives outside the repo by default.
2. **Env var** — export `FCADE_WORK_TOKEN` directly in the LaunchAgent's
   `EnvironmentVariables` dict (edit the *rendered* plist under the install
   root, not the tracked template) or your shell, if you'd rather not use a
   file.

Without a token configured, a **real** run hard-fails at preflight with a
pointer back here; `--dry-run` only ever needs a placeholder (see below) and
never fails on a missing token, since it makes no network call at all.

`proxy_ops.py` itself also accepts `--token` / `--token-file` directly (for
standalone testing) and always masks the token in any `--print-only` output.

## Dry-run first (zero network, zero side effects)

```sh
bash tools/fcade-replays/auto-convert/preconvert-worker.sh --dry-run
# or: FCADE_PRECONVERT_WORKER_DRY_RUN=1 bash tools/fcade-replays/auto-convert/preconvert-worker.sh
```

Prints, and does **nothing** else:

- the exact `worklease` request it would send (via `proxy_ops.py
  --print-only` — the real argument-parsing/request-construction code path,
  just never opens a socket or spawns `ssh`),
- since no network call is made, no *real* lease exists yet to show — the
  per-quark pipeline shape (synthetic catalog → `publish_3sr.py` command →
  `push-3sr.sh --incoming` → perms fix → `workdone`) is demonstrated against
  a clearly-labeled placeholder quarkid (`EXAMPLE-DRYRUN-QUARK-...`), not a
  real leased item.

Offline framing self-test (no args, no network, no subprocess at all):

```sh
"$HOME/Developer/fbneo-replay-runner/venv/bin/python" tools/fcade-replays/auto-convert/proxy_ops.py --selftest
```

## Failure modes found the hard way (2026-09-03/04)

Three defects that were live for weeks. All fixed; recorded because each one
*mis-reported itself*, and the next person will otherwise re-derive them.

**`publish_error` is a catch-all, not a push failure.**
`classify_failure_reason()` buckets any error string without "savestate" in it
as `publish_error` — runner timeouts, ENOSPC, and genuine push failures all land
there together. 179 of the ledger's 190 failures carried that label, which sent
one investigation at rsync permissions and `3sr-incoming` ownership while the
real causes were elsewhere. **Read the log's actual error text; do not trust the
ledger reason.** Timeouts now classify as `runner_timeout` (`30fc820f`).

**The Mac lane had half the VPS's runner budget.** This script passed
`--statcheck-timeout` but never `--runner-timeout`, so it silently took
`publish_3sr.py`'s 900 s default against the VPS's 30 min
(`CONVERT_RUNNER_TIMEOUT_MS`). Measured over 72 conversions: **42 %** failed,
every one a "runner timed out after 900s", on quarks the VPS converts fine —
28 of that day's 147 catalog rows are longer than 900 s, and a successful
conversion already runs a median 1479 s. Now `RUNNER_TIMEOUT=1800`
(`FCADE_RUNNER_TIMEOUT`).

**Scratch peaks ~26 GB per long session, and TMPDIR was unset.** The runner
writes the whole CPS3 work RAM every frame (524,288 B/frame); `publish_3sr.py`
compresses and deletes each `game_N/` at the boundary, which bounds it, but a
10-game / ~100k-frame session still peaked at 26 GB. With `TMPDIR` unset that
landed on the boot volume (37 GB free at the time) and did exhaust it during
the investigation. Now pointed at `SCRATCH_DIR` / `FCADE_SCRATCH_DIR`, guarded
so a missing external volume warns and falls back rather than pointing `TMPDIR`
at a nonexistent path (`1b5684cd`).

**Two more things worth knowing before trusting any conversion result:**

- **The VPS runs no statcheck at all** — 0 mentions in the deployed
  `fcade-proxy.js`, no binary, no `publish_3sr.py`. It did 4,935 of 5,051
  conversions, so **~97.6 % of the shipped corpus is unvalidated** against our
  engine. Only this Mac worker gates on statcheck.
- **`publish_3sr.py` has returned exit code 0 while printing a fatal traceback.**
  Read its output, never its status. Its interpreter is the 3.14 venv at
  `$RUNNER_DIR/venv`; the system `python3` is 3.9 and cannot parse it.

## Layout

```
tools/fcade-replays/auto-convert/
  setup.sh                                    # idempotent installer (venv, statcheck copy, dirs, BOTH plists)
  proxy_ops.py                                # Stage S5: worklease/workdone/workstats wire client
  preconvert-worker.sh                        # Stage S5: the worker's LaunchAgent wrapper (current rail)
  dev.sambae.fcade-preconvert-worker.plist    # Stage S5: LaunchAgent TEMPLATE for the worker
  convert-batch.sh                            # legacy daily-batch wrapper (manual use)
  dev.sambae.fcade-convert.plist              # legacy LaunchAgent TEMPLATE
  README.md / .gitignore
```

Everything the installer generates lives under the **install root**
(default `~/Library/Application Support/fcade-replay-convert`, outside the
repo) — **shared** between the worker and the legacy batch:

```
$INSTALL_ROOT/
  bin/3S-ARM-statcheck                        # stable copy of the statcheck exe (rebuild-proof)
  3sr-out/                                     # persistent .3sr store -- SHARED "already converted" truth
  logs/daily.log                               # legacy batch's append-only run log
  logs/preconvert-worker.log                   # worker's append-only run log (separate: different cadence)
  state/{last-run,last-success}                # legacy batch staleness monitoring
  state/preconvert-worker-{last-run,last-success}  # worker staleness monitoring
  state/preconvert-lease-catalogs/             # worker's transient synthetic per-lease catalogs
  state/preconvert-worker.lock/                # worker's mkdir-based overlap lock (no flock(1) on macOS)
  dev.sambae.fcade-convert.plist               # rendered legacy plist (absolute paths)
  dev.sambae.fcade-preconvert-worker.plist     # rendered worker plist (absolute paths)
  config.sh                                    # optional, non-secret overrides (you create it) -- SHARED
  secrets.sh                                   # FCADE_WORK_TOKEN -- SECRET, you create it, chmod 600, never committed
```

## Prerequisites (built out-of-band, verified by setup.sh)

- **FBNeo replay runner** at `~/Developer/fbneo-replay-runner/build/debug/fbneosdldarm64`
  with `roms/sfiii3.zip` + `roms/sfiii3nr1.zip` — build recipe in
  `docs/fcade-replay-notes.md` §1 (runner) and §2 (roms).
- **Conversion venv** (Python ≥ 3.10 + `rich`) — default is the runner's own
  venv (`~/Developer/fbneo-replay-runner/venv`); setup ensures `rich` is
  present. `proxy_ops.py` is stdlib-only but runs under this same
  interpreter for a single toolchain.
- **statcheck exe** at `build/host-statcheck/3S-ARM.app/Contents/MacOS/3S-ARM`
  (THREESX_STATCHECK host build) — setup copies it to a stable path so a repo
  `build/` rebuild can't wipe the copy the job depends on.

> **The "runner must run from its own dir" gotcha.** The FBNeo runner locates
> its `roms/` **relative to the current working directory**, and
> `publish_3sr.py` spawns the runner inheriting its own CWD.
> `preconvert-worker.sh` / `convert-batch.sh` therefore `cd` into
> `$RUNNER_DIR` before invoking publish. If you ever run publish by hand, do
> the same or it will fail to load the ROM.

## Setup

```sh
bash tools/fcade-replays/auto-convert/setup.sh
```

Idempotent: verifies the runner + roms, ensures the venv has `rich`, copies
the statcheck exe (re-copies only if the source is newer), creates the
logs/state/output dirs, renders **both** plists into the install root, and
runs `proxy_ops.py --selftest` (offline) as a sanity check. Re-run it any
time (e.g. after rebuilding statcheck) to refresh the stable copy. It does
**not** touch `secrets.sh` — the token is configured by hand (above).

Override anything via env, e.g.:

```sh
FCADE_CONVERT_HOME="$HOME/Library/Application Support/fcade-replay-convert" \
FCADE_RUNNER_DIR="$HOME/Developer/fbneo-replay-runner" \
bash tools/fcade-replays/auto-convert/setup.sh
```

### Optional `config.sh` (non-secret, shared by both jobs)

```sh
# $INSTALL_ROOT/config.sh
FCADE_CONVERT_BATCH=10                          # legacy batch: quarks per day
FCADE_PRECONVERT_WORKER_COUNT=3                  # worker: quarks leased per tick (server clamps to 3 anyway)
FCADE_VPS_TARGET="hetzner-3s-arm:/opt/fcade-proxy"
FCADE_CATALOG="$HOME/Library/Application Support/fcade-stealth-catalog/state/last-catalog.json"  # legacy batch only
```

No credentials belong here — the SSH key lives in `~/.ssh` (alias
`hetzner-3s-arm` authenticates passwordlessly); the work token lives in
`secrets.sh` (above).

## Run one tick by hand

```sh
# Dry-run (zero network/side effects -- see "Dry-run first" above):
bash tools/fcade-replays/auto-convert/preconvert-worker.sh --dry-run

# Real tick (requires FCADE_WORK_TOKEN configured -- see "Token configuration"):
bash tools/fcade-replays/auto-convert/preconvert-worker.sh
```

Watch it: `tail -f "$HOME/Library/Application Support/fcade-replay-convert/logs/preconvert-worker.log"`.

## Install / load / unload the worker's LaunchAgent

```sh
INSTALL_ROOT="$HOME/Library/Application Support/fcade-replay-convert"
PLIST="$INSTALL_ROOT/dev.sambae.fcade-preconvert-worker.plist"

mkdir -p "$HOME/Library/LaunchAgents"
cp -f "$PLIST" "$HOME/Library/LaunchAgents/dev.sambae.fcade-preconvert-worker.plist"
launchctl bootstrap gui/$(id -u) "$HOME/Library/LaunchAgents/dev.sambae.fcade-preconvert-worker.plist"

# Verify it's registered and see its schedule.
launchctl print gui/$(id -u)/dev.sambae.fcade-preconvert-worker | grep -Ei 'state|runatload|next'

# Trigger a tick right now (out of schedule) to smoke-test end-to-end:
launchctl kickstart -k gui/$(id -u)/dev.sambae.fcade-preconvert-worker

# Unload:
launchctl bootout gui/$(id -u)/dev.sambae.fcade-preconvert-worker
```

The agent has `RunAtLoad=false` — loading it will **not** immediately tick;
it fires every ~10 minutes on `StartInterval`, or when you `kickstart` it.

## Migration from the daily batch agent

The worker **supersedes** `dev.sambae.fcade-convert` (docs/plan-preconvert-
fleet.md Stage S5). If the daily batch agent is currently loaded, retire it
once the worker is loaded and confirmed ticking:

```sh
launchctl bootout gui/$(id -u)/dev.sambae.fcade-convert
```

`convert-batch.sh` and its plist are **kept** (not deleted) for manual/
offline use — e.g. seeding a fresh Mac's local `3sr-out` cache in one big
batch before the incremental worker takes over, or a one-off manual push.
They are simply no longer auto-scheduled by default.

## Monitoring / troubleshooting

- **Worker log**: `$INSTALL_ROOT/logs/preconvert-worker.log`.
- **Worker state**: `state/preconvert-worker-last-run` (every tick) and
  `state/preconvert-worker-last-success` (`epoch=... leased=... converted=...
  pushed=... failed=... worker=... target=...`).
- **"worklease returned zero leasable items"** means the VPS queue was empty
  or everything in it was already leased (by this worker or another) —
  expected once the backlog is drained; the VPS's own background converter
  keeps filling in regardless.
- A per-quark conversion failure (`workdone {ok:false}`) never aborts the
  tick — it's ledgered server-side (retried with backoff) and the worker
  moves on to its next leased quark.
- **Legacy batch log**: `$INSTALL_ROOT/logs/daily.log`; **"nothing to
  convert"** means every catalog quark already has a `.3sr`.
- Two ticks (of either job) never overlap: the worker uses a `mkdir`-based
  lockfile (macOS ships no `flock(1)`); a stale lock (owner process no
  longer running) is reclaimed automatically.
- If you rebuild the repo `build/` tree, re-run `setup.sh` to refresh the
  stable statcheck copy.
