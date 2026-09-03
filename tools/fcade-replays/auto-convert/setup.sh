#!/usr/bin/env bash
# tools/fcade-replays/auto-convert/setup.sh
#
# Idempotent installer for the scheduled, INCREMENTAL replay-CONVERSION runtime
# that the daily LaunchAgent (dev.sambae.fcade-convert) drives. This is the
# companion to the stealth-catalog refresh job (tools/fcade-proxy/stealth-catalog):
# that job keeps last-catalog.json fresh; THIS job walks that catalog and turns
# newly-listed Fightcade replays into playable `.3sr` blobs, pushing them to the
# VPS so the MiSTer REMOTE tab fills in over time.
#
# WHAT IT PROVISIONS (all outside git, under a stable per-user install root):
#   - the conversion venv (Python >= 3.10 + `rich`) -- publish_3sr.py and its
#     statcheck_runner subprocess both import `rich` and need PEP-604 `X | None`
#     syntax, so a 3.10+ interpreter is mandatory. Default: the runner's own
#     venv (~/Developer/fbneo-replay-runner/venv), already known-good.
#   - a STABLE COPY of the statcheck executable. The source lives in the repo
#     `build/host-statcheck/...` tree, which a `build/` rebuild can wipe; we copy
#     it under the install root and the wrapper references that copy (re-copied
#     whenever the source is newer). NOTE: the exe resolves libSDL3/libav* via
#     absolute LC_RPATH entries into the repo's third_party/*/build/lib -- those
#     survive a statcheck rebuild, so copying the bare exe is sufficient.
#   - logs/ + state/ dirs and the persistent `.3sr` output store (the source of
#     truth for "already converted").
#   - the rendered LaunchAgent plist.
#
# It VERIFIES (fails loud, with guidance) that the FBNeo replay runner binary
# and its sfiii3 roms exist -- if not, points at docs/fcade-replay-notes.md §1
# (the build recipe). SAFE TO RE-RUN: every step checks first and skips if done.
#
# It does NOT install/load the LaunchAgent and does NOT run a conversion or a
# push -- it only provisions the runtime and renders the plist. See README.md
# for the launchctl install/load steps you run yourself.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# tools/fcade-replays/auto-convert -> repo root is three levels up.
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# --- Config (override via env) ----------------------------------------------
# Persistent install root. Default: the standard macOS per-user data dir.
INSTALL_ROOT="${FCADE_CONVERT_HOME:-${XDG_DATA_HOME:-$HOME/Library/Application Support}/fcade-replay-convert}"

# The FBNeo replay runner tree. The runner binary MUST be invoked with this as
# CWD (it finds roms/ relative to CWD) -- convert-batch.sh cd's here.
RUNNER_DIR="${FCADE_RUNNER_DIR:-$HOME/Developer/fbneo-replay-runner}"
RUNNER_BIN="${FCADE_RUNNER_BIN:-$RUNNER_DIR/build/debug/fbneosdldarm64}"

# The conversion venv. Default: the runner's own venv (already built w/ rich).
VENV_DIR="${FCADE_CONVERT_VENV:-$RUNNER_DIR/venv}"
VENV_PY="$VENV_DIR/bin/python"

# Source statcheck exe (repo build tree -- may be wiped by a rebuild).
STATCHECK_SRC="${FCADE_STATCHECK_SRC:-$REPO_ROOT/build/host-statcheck/3S-ARM.app/Contents/MacOS/3S-ARM}"

# --- Derived paths -----------------------------------------------------------
BIN_DIR="$INSTALL_ROOT/bin"
STATCHECK_DST="$BIN_DIR/3S-ARM-statcheck"   # stable copy the wrapper references
OUT_DIR="$INSTALL_ROOT/3sr-out"             # persistent .3sr store (source of truth)
LOGS_DIR="$INSTALL_ROOT/logs"
STATE_DIR="$INSTALL_ROOT/state"
DAILY_LOG="$LOGS_DIR/daily.log"

CONVERT_BATCH="$SCRIPT_DIR/convert-batch.sh"
PLIST_TEMPLATE="$SCRIPT_DIR/dev.sambae.fcade-convert.plist"
PLIST_RENDERED="$INSTALL_ROOT/dev.sambae.fcade-convert.plist"

# Mac-worker half of the pre-convert fleet (plan-preconvert-fleet.md Stage S5)
# -- REPLACES the daily batch's LaunchAgent (see "Migration" step below), but
# convert-batch.sh + its plist stay provisioned/tracked for manual use.
PRECONVERT_WORKER="$SCRIPT_DIR/preconvert-worker.sh"
PRECONVERT_WORKER_PLIST_TEMPLATE="$SCRIPT_DIR/dev.sambae.fcade-preconvert-worker.plist"
PRECONVERT_WORKER_PLIST_RENDERED="$INSTALL_ROOT/dev.sambae.fcade-preconvert-worker.plist"
PRECONVERT_WORKER_LOG="$INSTALL_ROOT/logs/preconvert-worker.log"

# Probe for a Python >= 3.10 to build the venv with, if one must be created.
if [ -n "${FCADE_PYTHON_BIN:-}" ]; then
  PYTHON_BIN="$FCADE_PYTHON_BIN"
else
  PYTHON_BIN=""
  for _cand in python3.14 python3.13 python3.12 python3.11 python3.10 python3; do
    _p="$(command -v "$_cand" 2>/dev/null)" || continue
    if "$_p" -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' 2>/dev/null; then
      PYTHON_BIN="$_p"; break
    fi
  done
fi

say() { printf '[setup] %s\n' "$*"; }
die() { printf '[setup] ERROR: %s\n' "$*" >&2; exit 1; }

say "install root: $INSTALL_ROOT"
mkdir -p "$INSTALL_ROOT" "$BIN_DIR" "$OUT_DIR" "$LOGS_DIR" "$STATE_DIR"

# --- 1. FBNeo runner + roms (verify only; built out-of-band) -----------------
# The runner is built per docs/fcade-replay-notes.md §1 ("Runner build"). We do
# not build it here -- just prove it and its roms are present, with a clear
# pointer if not.
if [ ! -x "$RUNNER_BIN" ]; then
  die "FBNeo replay runner not found/executable at:
        $RUNNER_BIN
      Build it per docs/fcade-replay-notes.md §1 (clone crowded-street/fbneo-replay-runner
      @ ccf96ab, 'make sdl BUILD_X86_ASM= CPUTYPE=arm64 -j1'), or set FCADE_RUNNER_BIN."
fi
say "runner: $RUNNER_BIN"
_missing_roms=()
for _rom in sfiii3.zip sfiii3nr1.zip; do
  [ -f "$RUNNER_DIR/roms/$_rom" ] || _missing_roms+=("$_rom")
done
if [ "${#_missing_roms[@]}" -ne 0 ]; then
  die "runner roms missing under $RUNNER_DIR/roms/: ${_missing_roms[*]}
      Provision them per docs/fcade-replay-notes.md §2 (copy sfiii3nr1.zip +
      sfiii3.zip parent set into the runner's roms/ dir)."
fi
say "roms: sfiii3.zip + sfiii3nr1.zip present under $RUNNER_DIR/roms/"

# --- 2. Conversion venv (Python >= 3.10 + rich) ------------------------------
venv_ok() { [ -x "$VENV_PY" ] && "$VENV_PY" -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' 2>/dev/null; }
if venv_ok; then
  say "venv already present (Python >= 3.10) -> $VENV_DIR"
else
  if [ -x "$VENV_PY" ]; then
    say "existing venv is Python < 3.10 ($("$VENV_PY" --version 2>&1)); recreating"
    rm -rf "$VENV_DIR"
  fi
  [ -n "$PYTHON_BIN" ] || die "no Python >= 3.10 found (the tooling needs 3.10+ 'X | None' syntax; macOS system python3 is 3.9). Install one (e.g. 'brew install python@3.14') or set FCADE_PYTHON_BIN."
  say "creating venv with $PYTHON_BIN ($("$PYTHON_BIN" --version 2>&1)) -> $VENV_DIR"
  "$PYTHON_BIN" -m venv "$VENV_DIR"
fi
say "installing/verifying pip deps (rich) ..."
"$VENV_PY" -m pip install --quiet --upgrade pip >/dev/null
"$VENV_PY" -m pip install --quiet rich
if "$VENV_PY" -c "import rich" 2>/dev/null; then
  say "rich imports OK on the conversion venv"
else
  die "rich failed to import in the venv ($VENV_PY) -- statcheck_runner needs it"
fi

# --- 3. Stable statcheck copy ------------------------------------------------
[ -x "$STATCHECK_SRC" ] || die "statcheck exe not found/executable at:
        $STATCHECK_SRC
      Build it (THREESX_STATCHECK host build) or set FCADE_STATCHECK_SRC."
# Copy if the destination is missing or the source is strictly newer.
if [ ! -x "$STATCHECK_DST" ] || [ "$STATCHECK_SRC" -nt "$STATCHECK_DST" ]; then
  say "copying statcheck exe -> $STATCHECK_DST"
  cp -f "$STATCHECK_SRC" "$STATCHECK_DST"
  chmod +x "$STATCHECK_DST"
else
  say "statcheck copy already current -> $STATCHECK_DST"
fi
# Prove the copy loads (its @rpath resolves to the repo's third_party dylibs).
if SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$STATCHECK_DST" --help >/dev/null 2>&1; then
  say "statcheck copy runs --help OK (dummy SDL)"
else
  die "statcheck copy failed to run --help at $STATCHECK_DST
      (its @rpath libSDL3/libav* may be missing -- check the repo's
      third_party/{sdl3,ffmpeg}/build/lib dylibs still exist)."
fi

# --- 4. Render the LaunchAgent plist into the install root -------------------
if [ -f "$PLIST_TEMPLATE" ]; then
  say "rendering LaunchAgent plist -> $PLIST_RENDERED"
  sed \
    -e "s#__CONVERT_BATCH_SH__#${CONVERT_BATCH}#g" \
    -e "s#__INSTALL_ROOT__#${INSTALL_ROOT}#g" \
    -e "s#__DAILY_LOG__#${DAILY_LOG}#g" \
    "$PLIST_TEMPLATE" > "$PLIST_RENDERED"
  if command -v plutil >/dev/null 2>&1; then
    plutil -lint "$PLIST_RENDERED" >/dev/null && say "rendered plist passes plutil -lint"
  fi
else
  say "WARNING: plist template not found at $PLIST_TEMPLATE (skipping render)"
fi

# --- 4b. Render the pre-convert WORKER's LaunchAgent plist (Stage S5) --------
# Same idempotent render, separate label/schedule/log. Does NOT install/load
# it (same posture as step 4) and does NOT touch FCADE_WORK_TOKEN -- that
# secret is configured by hand in $INSTALL_ROOT/secrets.sh, never by this
# installer (see README.md "Token configuration").
if [ -f "$PRECONVERT_WORKER_PLIST_TEMPLATE" ]; then
  say "rendering pre-convert worker LaunchAgent plist -> $PRECONVERT_WORKER_PLIST_RENDERED"
  sed \
    -e "s#__PRECONVERT_WORKER_SH__#${PRECONVERT_WORKER}#g" \
    -e "s#__INSTALL_ROOT__#${INSTALL_ROOT}#g" \
    -e "s#__PRECONVERT_WORKER_LOG__#${PRECONVERT_WORKER_LOG}#g" \
    "$PRECONVERT_WORKER_PLIST_TEMPLATE" > "$PRECONVERT_WORKER_PLIST_RENDERED"
  if command -v plutil >/dev/null 2>&1; then
    plutil -lint "$PRECONVERT_WORKER_PLIST_RENDERED" >/dev/null && say "rendered pre-convert worker plist passes plutil -lint"
  fi
else
  say "WARNING: plist template not found at $PRECONVERT_WORKER_PLIST_TEMPLATE (skipping render)"
fi

# --- 4c. Prove the worker's offline framing self-test passes ----------------
# No network, no ssh, no token needed -- just proves proxy_ops.py's wire
# framing is intact in this venv before anyone wires up the real token.
if [ -f "$SCRIPT_DIR/proxy_ops.py" ]; then
  if "$VENV_PY" "$SCRIPT_DIR/proxy_ops.py" --selftest >/dev/null; then
    say "proxy_ops.py --selftest passes (offline wire-framing check)"
  else
    die "proxy_ops.py --selftest FAILED -- something is wrong with the venv python or the script itself"
  fi
fi

# --- Done: print resolved paths ----------------------------------------------
cat <<EOF

[setup] DONE. Resolved runtime:
  install root   : $INSTALL_ROOT
  venv python    : $VENV_PY
  runner (CWD)   : $RUNNER_DIR   (binary: $RUNNER_BIN)
  statcheck copy : $STATCHECK_DST
  .3sr store     : $OUT_DIR   (persistent -- "already converted" truth)
  logs           : $LOGS_DIR  (daily log: $DAILY_LOG)
  state          : $STATE_DIR
  convert-batch  : $CONVERT_BATCH   (manual/legacy daily batch -- kept, not auto-loaded by this step)
  plist          : $PLIST_RENDERED   (dev.sambae.fcade-convert -- legacy daily batch)

  preconvert-worker : $PRECONVERT_WORKER   (Stage S5 -- the CURRENT rail)
  worker plist      : $PRECONVERT_WORKER_PLIST_RENDERED   (dev.sambae.fcade-preconvert-worker)
  worker log        : $PRECONVERT_WORKER_LOG

Next:
  - Optionally create $INSTALL_ROOT/config.sh to override batch size / VPS
    target / catalog path (see README.md; no secrets).
  - Configure the pre-convert worker's SECRET token (never in config.sh):
      create $INSTALL_ROOT/secrets.sh, chmod 600, containing:
        export FCADE_WORK_TOKEN=<value configured in the VPS's FCADE_WORK_TOKEN>
      See README.md "Token configuration".
  - Dry-run the pre-convert worker first (zero network, zero side effects):
      bash $PRECONVERT_WORKER --dry-run
  - Dry-run the legacy incremental selection (manual use only):
      FCADE_CONVERT_DRY_RUN=1 bash $CONVERT_BATCH
  - Load the pre-convert worker's LaunchAgent per README.md "Install / load /
    unload the LaunchAgent" (launchctl bootstrap gui/\$UID ...). If the daily
    batch agent (dev.sambae.fcade-convert) is currently loaded, retire it per
    README.md "Migration from the daily batch agent" (launchctl bootout) --
    the worker supersedes it; convert-batch.sh itself is kept for manual runs.
EOF
