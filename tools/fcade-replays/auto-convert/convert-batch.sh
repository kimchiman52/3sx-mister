#!/usr/bin/env bash
# tools/fcade-replays/auto-convert/convert-batch.sh
#
# The wrapper the daily LaunchAgent (dev.sambae.fcade-convert) invokes. It runs
# ONE bounded, INCREMENTAL batch of Fightcade-replay -> `.3sr` conversions and
# pushes the freshly-converted quarks to the VPS. Over many daily runs the VPS
# `.3sr` store (and thus the MiSTer REMOTE tab) fills in a rolling pool of the
# newest replays -- never the whole catalog at once.
#
# INCREMENTAL MODEL (the key idea):
#   - The persistent out-dir ($OUT_DIR) is the SOURCE OF TRUTH for "already
#     converted": a quark is considered done if $OUT_DIR/<quarkid>/ contains at
#     least one game_*.3sr.
#   - Each run: read the catalog's quarkids for the game, SUBTRACT the already-
#     converted ones, sort the remainder by date DESC, take the newest N
#     ($FCADE_CONVERT_BATCH, default 10), convert exactly those, push them.
#   - If nothing is left to convert, log it and exit 0.
#
# publish_3sr.py already processes each quark independently and continues past a
# per-quark failure (we deliberately do NOT pass --fail-fast), so one bad quark
# never aborts the batch -- we push whatever actually produced a .3sr. The only
# hard (exit-nonzero) failures are missing prerequisites (runner / statcheck /
# venv / catalog): everything else is logged and exits 0.
#
# GOTCHA (why we cd): the FBNeo runner finds its roms/ RELATIVE TO CWD, and
# publish_3sr.py spawns the runner inheriting publish's CWD -- so publish MUST
# be launched from $RUNNER_DIR. This script cd's there before invoking it.
#
# HEADLESS: the runner is -headless and statcheck uses dummy SDL video/audio, so
# no GUI session/display is needed -- this runs unattended even when logged out.
#
# No secrets live here. Targets/paths/tunables are config vars below, overridable
# via env or an optional $INSTALL_ROOT/config.sh.
#
# Usage:
#   convert-batch.sh                 # run one incremental batch
#   convert-batch.sh --dry-run       # print the selection + planned command; do nothing
#   FCADE_CONVERT_DRY_RUN=1 convert-batch.sh   # same, via env

set -uo pipefail   # NOTE: deliberately no -e; we handle exit codes by hand.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DRY_RUN=0
[ "${FCADE_CONVERT_DRY_RUN:-0}" = "1" ] && DRY_RUN=1
if [ "${1:-}" = "--dry-run" ] || [ "${1:-}" = "-n" ]; then DRY_RUN=1; fi

# --- Config (defaults; override via env or $INSTALL_ROOT/config.sh) ----------
INSTALL_ROOT="${FCADE_CONVERT_HOME:-${XDG_DATA_HOME:-$HOME/Library/Application Support}/fcade-replay-convert}"

# Optional per-machine, non-secret override file (targets/paths/tunables only).
if [ -f "$INSTALL_ROOT/config.sh" ]; then
  # shellcheck disable=SC1091
  . "$INSTALL_ROOT/config.sh"
fi

# The FBNeo replay runner tree (CWD the runner needs) + its binary.
RUNNER_DIR="${FCADE_RUNNER_DIR:-$HOME/Developer/fbneo-replay-runner}"
RUNNER_BIN="${FCADE_RUNNER_BIN:-$RUNNER_DIR/build/debug/fbneosdldarm64}"
# The conversion venv python (rich; 3.10+). Default: the runner's own venv.
VENV_PY="${FCADE_CONVERT_VENV:-$RUNNER_DIR/venv}/bin/python"
# The stable statcheck copy setup.sh made (survives a repo build/ rebuild).
STATCHECK_BIN="${FCADE_STATCHECK_BIN:-$INSTALL_ROOT/bin/3S-ARM-statcheck}"
# Catalog input: the file the stealth-catalog refresh job keeps fresh.
CATALOG="${FCADE_CATALOG:-${XDG_DATA_HOME:-$HOME/Library/Application Support}/fcade-stealth-catalog/state/last-catalog.json}"
# Persistent .3sr store (source of truth for "already converted").
OUT_DIR="${FCADE_CONVERT_OUT:-$INSTALL_ROOT/3sr-out}"
# Fightcade game id to convert.
GAMEID="${FCADE_CONVERT_GAMEID:-sfiii3nr1}"
# How many un-converted quarks to convert per run (rolling pool size per day).
BATCH="${FCADE_CONVERT_BATCH:-10}"
# rsync/ssh VPS target (host alias + path; NOT a secret -- key lives in ~/.ssh).
VPS_TARGET="${FCADE_VPS_TARGET:-hetzner-3s-arm:/opt/fcade-proxy}"
# statcheck per-game gate timeout handed to publish_3sr.py.
STATCHECK_TIMEOUT="${FCADE_STATCHECK_TIMEOUT:-30}"

# In-repo tools (source scripts, not build artifacts).
PUBLISH="$SCRIPT_DIR/../publish_3sr.py"
PUSH="$SCRIPT_DIR/../../fcade-proxy/push-3sr.sh"

# --- Derived runtime paths ---------------------------------------------------
LOGS_DIR="$INSTALL_ROOT/logs"
STATE_DIR="$INSTALL_ROOT/state"
LOGFILE="$LOGS_DIR/daily.log"
LAST_SUCCESS="$STATE_DIR/last-success"
LAST_RUN="$STATE_DIR/last-run"

mkdir -p "$LOGS_DIR" "$STATE_DIR" "$OUT_DIR"

# All output goes to the append-only logfile; ALSO to the terminal when
# interactive (launchd points StandardOut/Err at the same file -- no dup).
if [ -t 1 ]; then
  exec > >(tee -a "$LOGFILE") 2>&1
else
  exec >> "$LOGFILE" 2>&1
fi

ts()  { date '+%Y-%m-%dT%H:%M:%S%z'; }
log() { printf '%s [convert-batch] %s\n' "$(ts)" "$*"; }

notify() {
  local msg="$1"
  command -v osascript >/dev/null 2>&1 || return 0
  osascript -e "display notification \"${msg//\"/\'}\" with title \"Fightcade replay conversion FAILED\"" >/dev/null 2>&1 || true
}

record_run() { printf '%s status=%s\n' "$(ts)" "$1" > "$LAST_RUN"; }

fail() {
  # fail <token> <human message> -- a HARD (missing-prereq) failure only.
  local token="$1"; shift
  log "FAILURE ($token): $*"
  log "converted/pushed NOTHING."
  record_run "$token"
  notify "$* (token=$token)"
  exit 1
}

log "==== batch run start ===="
log "install root : $INSTALL_ROOT"
log "catalog      : $CATALOG"
log "out-dir      : $OUT_DIR"
log "gameid/batch : gameid=$GAMEID batch=$BATCH"
log "vps target   : $VPS_TARGET"
[ "$DRY_RUN" = "1" ] && log "MODE         : DRY-RUN (no conversion, no push)"

# --- Preflight: hard failures if a prerequisite is missing -------------------
[ -x "$RUNNER_BIN" ]    || fail preflight "FBNeo runner not found/executable at $RUNNER_BIN (run setup.sh / see docs/fcade-replay-notes.md §1)"
[ -x "$VENV_PY" ]       || fail preflight "conversion venv python not found at $VENV_PY (run setup.sh)"
[ -x "$STATCHECK_BIN" ] || fail preflight "statcheck copy not found at $STATCHECK_BIN (run setup.sh)"
[ -f "$PUBLISH" ]       || fail preflight "publish_3sr.py not found at $PUBLISH"
[ -x "$PUSH" ]          || fail preflight "push-3sr.sh not found/executable at $PUSH"
[ -f "$CATALOG" ]       || fail preflight "catalog not found at $CATALOG (is the stealth-catalog refresh job installed?)"
"$VENV_PY" -c "import rich" 2>/dev/null || fail preflight "venv python at $VENV_PY cannot import rich (run setup.sh)"

# --- 1. Incremental selection (newest-N unconverted) -------------------------
# The venv python reads the catalog + out-dir and prints the selected quarkids
# (one per line) to STDOUT; all diagnostics go to STDERR (-> logfile). We write
# stdout to a temp file (not a pipe) so we can capture python's own exit code
# AND read the result into an array on stock macOS bash 3.2 (no `mapfile`).
SEL_TMP="$(mktemp "$STATE_DIR/select.XXXXXX")"
"$VENV_PY" - "$CATALOG" "$OUT_DIR" "$GAMEID" "$BATCH" > "$SEL_TMP" <<'PY'
import json, sys
from pathlib import Path

catalog_path, out_dir, gameid, batch = sys.argv[1], Path(sys.argv[2]), sys.argv[3], int(sys.argv[4])

def err(m): print(m, file=sys.stderr)

data = json.loads(Path(catalog_path).read_text(encoding="utf-8"))
rows = data.get("rows") if isinstance(data, dict) else None
if not isinstance(rows, list):
    err(f"select: catalog {catalog_path} has no 'rows' array"); sys.exit(2)

# Catalog rows for this game, keyed by quarkid with their date.
by_id = {}
for r in rows:
    if isinstance(r, dict) and r.get("gameid") == gameid and r.get("quarkid") is not None:
        by_id[str(r["quarkid"])] = r.get("date") or 0

def converted(qid: str) -> bool:
    d = out_dir / qid
    return d.is_dir() and any(d.glob("game_*.3sr"))

done = [q for q in by_id if converted(q)]
remaining = [q for q in by_id if q not in set(done)]
remaining.sort(key=lambda q: by_id[q], reverse=True)   # newest first
selected = remaining[:batch]

err(f"select: catalog rows for {gameid}: {len(by_id)}")
err(f"select: already converted (>=1 game_*.3sr in out-dir): {len(done)}")
err(f"select: remaining unconverted: {len(remaining)}; taking newest {len(selected)} (batch={batch})")
for q in selected:
    err(f"select:   -> {q} (date={by_id[q]})")

for q in selected:
    print(q)
PY
SELECT_RC=$?
if [ "$SELECT_RC" -ne 0 ]; then
  rm -f "$SEL_TMP"
  fail select "incremental selection failed (rc=$SELECT_RC) -- catalog unreadable/malformed?"
fi

# Read the selected quarkids into an array (bash 3.2: no mapfile/readarray).
SELECTED=()
while IFS= read -r _q; do
  [ -n "$_q" ] && SELECTED+=("$_q")
done < "$SEL_TMP"
rm -f "$SEL_TMP"

if [ "${#SELECTED[@]}" -eq 0 ]; then
  log "nothing to convert (every catalog quark for $GAMEID already has a .3sr, or catalog empty)."
  record_run ok-nothing
  log "==== batch run end ===="
  exit 0
fi

log "selected ${#SELECTED[@]} quark(s) to convert: ${SELECTED[*]}"

# Build the repeated --quark args once (reused by dry-run print + real run).
QUARK_ARGS=()
for q in "${SELECTED[@]}"; do QUARK_ARGS+=(--quark "$q"); done

# --- 2. Dry-run: show exactly what WOULD run, then stop -----------------------
if [ "$DRY_RUN" = "1" ]; then
  log "DRY-RUN: would (cd '$RUNNER_DIR') and run:"
  log "  $VENV_PY $PUBLISH \\"
  log "    --catalog $CATALOG --runner $RUNNER_BIN \\"
  log "    --statcheck $STATCHECK_BIN --out-dir $OUT_DIR \\"
  log "    --gameid $GAMEID --statcheck-timeout $STATCHECK_TIMEOUT ${QUARK_ARGS[*]}"
  log "DRY-RUN: would then push newly-converted quarks via $PUSH to $VPS_TARGET (+ perms)."
  record_run ok-dryrun
  log "==== batch run end (dry-run) ===="
  exit 0
fi

# --- 3. Convert (from the runner's CWD, with the venv python) ----------------
log "running publish_3sr.py from CWD=$RUNNER_DIR (runner needs roms/ relative to CWD) ..."
(
  cd "$RUNNER_DIR" || exit 97
  exec "$VENV_PY" "$PUBLISH" \
    --catalog "$CATALOG" \
    --runner "$RUNNER_BIN" \
    --statcheck "$STATCHECK_BIN" \
    --out-dir "$OUT_DIR" \
    --gameid "$GAMEID" \
    --statcheck-timeout "$STATCHECK_TIMEOUT" \
    "${QUARK_ARGS[@]}"
)
PUBLISH_RC=$?
log "publish_3sr.py exited $PUBLISH_RC (0=at least one game published, 1=zero published; per-quark errors are non-fatal)"

# --- 4. Determine which selected quarks actually produced a .3sr -------------
NEWLY=()
for q in "${SELECTED[@]}"; do
  if compgen -G "$OUT_DIR/$q/game_*.3sr" >/dev/null 2>&1; then
    NEWLY+=("$q")
  fi
done

if [ "${#NEWLY[@]}" -eq 0 ]; then
  # Not a hard failure: quarks may be expired/statcheck-divergent. Log + exit 0.
  log "no quark produced a .3sr this run (expired / statcheck-divergent?) -- nothing to push."
  record_run ok-noconvert
  log "==== batch run end ===="
  exit 0
fi

log "newly-converted quark(s) (${#NEWLY[@]}): ${NEWLY[*]}"

# --- 5. Push only the newly-converted quarks + fix VPS perms -----------------
log "pushing newly-converted quarks to $VPS_TARGET ..."
"$PUSH" "$OUT_DIR" "$VPS_TARGET" "${NEWLY[@]}"
PUSH_RC=$?
if [ "$PUSH_RC" -ne 0 ]; then
  log "WARNING: push-3sr.sh exited $PUSH_RC -- some/all quarks may not have shipped."
fi

# Fix ownership/perms so the fcade-proxy service user can read the new files.
# (get3sr reads <FCADE_3SR_DIR>/<quarkid>/game_N.3sr directly off disk.)
VPS_HOST="${VPS_TARGET%%:*}"
VPS_PATH="${VPS_TARGET#*:}"
log "fixing VPS perms on $VPS_HOST:$VPS_PATH/3sr ..."
ssh "$VPS_HOST" "sudo chown -R fcade-proxy:fcade-proxy '$VPS_PATH/3sr' && sudo find '$VPS_PATH/3sr' -type d -exec chmod 755 {} \\; && sudo find '$VPS_PATH/3sr' -type f -exec chmod 644 {} \\;" \
  && log "VPS perms fixed." \
  || log "WARNING: VPS perm fix failed (ssh/sudo) -- new files may be unreadable to the service user."

# --- 6. Record success state -------------------------------------------------
NOW_EPOCH="$(date +%s)"
printf 'epoch=%s converted=%s pushed=%s target=%s\n' \
  "$NOW_EPOCH" "${#NEWLY[@]}" "${NEWLY[*]}" "$VPS_TARGET" > "$LAST_SUCCESS"
record_run ok
log "SUCCESS: converted ${#NEWLY[@]} quark(s), pushed to $VPS_TARGET."
log "==== batch run end ===="
exit 0
