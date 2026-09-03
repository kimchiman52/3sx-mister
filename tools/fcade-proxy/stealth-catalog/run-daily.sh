#!/usr/bin/env bash
# tools/fcade-proxy/stealth-catalog/run-daily.sh
#
# The wrapper the daily LaunchAgent (dev.sambae.fcade-catalog) invokes
# (plan-fcade-stealth-catalog.md, Stage S4). It:
#
#   1. resolves the persistent runtime provisioned by setup.sh (venv python,
#      Chrome-for-Testing binary, persistent profile, logs/state dirs);
#   2. runs the committed stealth-catalog runner HEADED but OFF-SCREEN, with a
#      push-safety row floor, into a private temp file;
#   3. ONLY on runner exit 0 (a clean, complete crawl that met the floor),
#      hands that file to the UNCHANGED refresh-catalog.sh --file tail
#      (validate -> push -> verify) with FCADE_NODE_BIN pointed at node;
#   4. on ANY failure (runner non-zero OR refresh-catalog.sh non-zero) logs
#      loudly, fires a macOS notification, records the failure, and exits
#      non-zero having PUSHED NOTHING -- the last-good catalog on the VPS is
#      preserved. This is the critical safety property (plan §6.2).
#
# It writes a timestamped append-only log and last-success / last-run state
# files so silent staleness is detectable (see check-staleness.sh).
#
# No secrets live here. The VPS target, node path, row floor and timeout are
# config vars below, overridable by env or by an optional $INSTALL_ROOT/config.sh.

set -uo pipefail   # NOTE: deliberately no -e; we handle every exit code by hand.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- Config (defaults; override via env or $INSTALL_ROOT/config.sh) ----------
INSTALL_ROOT="${FCADE_CATALOG_HOME:-${XDG_DATA_HOME:-$HOME/Library/Application Support}/fcade-stealth-catalog}"

# Source an optional per-machine config file for overrides. NOT in git; must
# never contain credentials -- only non-secret targets/paths/tunables.
if [ -f "$INSTALL_ROOT/config.sh" ]; then
  # shellcheck disable=SC1091
  . "$INSTALL_ROOT/config.sh"
fi

# rsync/ssh target (same shape refresh-catalog.sh/push-catalog.sh expect). This
# is a host alias + path, NOT a secret; the SSH key lives in ~/.ssh.
VPS_TARGET="${FCADE_VPS_TARGET:-hetzner-3s-arm:/opt/fcade-proxy}"
# node binary refresh-catalog.sh needs (installed separately; pass its path).
NODE_BIN="${FCADE_NODE_BIN:-node}"
# Push-safety floor: refuse to write/push a catalog with fewer rows than this.
#
# RE-SIZED for the weekly-best crawl (was 100, sized for the old Recent+Best
# merge whose live catalog ran ~278-295 rows). The crawl now pulls ONE feed --
# best-this-week -- capped at MAX_ROWS (150), so 100 was no longer a "clearly
# broke" guard: it sat two thirds of the way up the maximum possible yield, and
# a genuinely quiet week would trip it.
#
# The failure this floor causes when it trips wrongly is the nasty kind --
# nothing is written, the VPS keeps serving the last-good catalog, and the
# device plays an ever-staler set -- so it is deliberately LOW. 30 = two full
# pages of PAGE_SIZE 15. A crawl that terminates CLEANLY with under two full
# pages is a broken window or an upstream change, not a quiet week.
#
# Truncation is NOT this floor's job: collect_feed marks any dirty termination
# (eval timeout, non-200, a 403 surviving the re-solve) and run_full/amain then
# refuse to emit anything. This is only the second belt against "clean but
# absurdly small".
FLOOR="${FCADE_MIN_ROWS:-30}"
# Low-yield WARNING threshold (not a refusal). A crawl at or above FLOOR but
# below this still publishes -- a small week is still the real set -- but says
# so loudly and notifies, because a slow slide toward the floor is the early
# warning for the silent-staleness failure above. Also recorded in
# state/last-success as rows=, where check-staleness.sh re-checks it.
LOW_YIELD="${FCADE_LOW_YIELD_ROWS:-100}"
# Per-navigation / per-page-eval timeout handed to the runner (seconds).
TIMEOUT="${FCADE_TIMEOUT:-90}"
# Per-feed row cap handed to the runner (browser-catalog.js MAX_ROWS = 150).
MAX_ROWS="${FCADE_MAX_ROWS:-150}"

# --- Derived runtime paths ---------------------------------------------------
VENV_PY="$INSTALL_ROOT/venv/bin/python"
PROFILE_DIR="$INSTALL_ROOT/profile"
LOGS_DIR="$INSTALL_ROOT/logs"
STATE_DIR="$INSTALL_ROOT/state"
LOGFILE="$LOGS_DIR/daily.log"
LAST_SUCCESS="$STATE_DIR/last-success"
LAST_RUN="$STATE_DIR/last-run"
RUNNER="$SCRIPT_DIR/fcade_stealth_catalog.py"
REFRESH="$SCRIPT_DIR/../refresh-catalog.sh"
CHROME_DIR="$INSTALL_ROOT/chrome"
# Chrome-for-Testing binary (resolve the mac-arm64 layout; fall back to a glob).
CHROME_BIN="$CHROME_DIR/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing"

mkdir -p "$LOGS_DIR" "$STATE_DIR"

# All output (ours + subprocesses') goes to the append-only logfile. When run
# interactively we ALSO echo to the terminal; under launchd fd1 is not a TTY so
# it appends once (launchd points StandardOut/Err at the same file -- no dup).
if [ -t 1 ]; then
  exec > >(tee -a "$LOGFILE") 2>&1
else
  exec >> "$LOGFILE" 2>&1
fi

ts() { date '+%Y-%m-%dT%H:%M:%S%z'; }
log() { printf '%s [run-daily] %s\n' "$(ts)" "$*"; }

notify() {
  # Best-effort macOS notification; never fatal if osascript is unavailable.
  local msg="$1"
  command -v osascript >/dev/null 2>&1 || return 0
  osascript -e "display notification \"${msg//\"/\'}\" with title \"Fightcade catalog refresh FAILED\"" >/dev/null 2>&1 || true
}

record_run() {
  # record_run <status> ; status is "ok" or a short failure token.
  printf '%s status=%s\n' "$(ts)" "$1" > "$LAST_RUN"
}

fail() {
  # fail <token> <human message>
  local token="$1"; shift
  log "FAILURE ($token): $*"
  log "PUSHED NOTHING -- last-good catalog on $VPS_TARGET is preserved."
  record_run "$token"
  notify "$* (token=$token) -- last-good preserved."
  exit 1
}

log "==== daily run start ===="
log "install root : $INSTALL_ROOT"
log "target       : $VPS_TARGET"
log "node bin     : $NODE_BIN"
log "floor/max    : min-rows=$FLOOR low-yield-warn=$LOW_YIELD max-rows=$MAX_ROWS timeout=${TIMEOUT}s"

# --- Preflight: everything must exist before we touch the browser -----------
[ -x "$VENV_PY" ]  || fail preflight "venv python not found at $VENV_PY (run setup.sh)"
[ -f "$RUNNER" ]   || fail preflight "runner not found at $RUNNER"
[ -f "$REFRESH" ]  || fail preflight "refresh-catalog.sh not found at $REFRESH"
if [ ! -x "$CHROME_BIN" ]; then
  # Fall back to a glob in case the app dir name differs.
  ALT="$(/bin/ls -d "$CHROME_DIR"/*/*.app/Contents/MacOS/* 2>/dev/null | head -1 || true)"
  if [ -n "$ALT" ] && [ -x "$ALT" ]; then
    CHROME_BIN="$ALT"
  else
    fail preflight "Chrome-for-Testing binary not found under $CHROME_DIR (run setup.sh)"
  fi
fi
command -v "$NODE_BIN" >/dev/null 2>&1 || fail preflight "node '$NODE_BIN' not found (set FCADE_NODE_BIN)"
log "chrome       : $CHROME_BIN"

# --- 1. Run the stealth-catalog crawler (headed, off-screen) ----------------
TMPFILE="$(mktemp "$STATE_DIR/catalog.XXXXXX.json")"
cleanup() { rm -f "$TMPFILE"; }
trap cleanup EXIT

log "running crawler (headed, off-screen) -> $TMPFILE"
# errexit is deliberately OFF for the whole script (set -uo pipefail above); we
# inspect every exit code by hand so a non-zero NEVER slips past unhandled.
"$VENV_PY" "$RUNNER" \
  --out "$TMPFILE" \
  --min-rows "$FLOOR" \
  --max-rows "$MAX_ROWS" \
  --timeout "$TIMEOUT" \
  --offscreen \
  --chrome "$CHROME_BIN" \
  --profile "$PROFILE_DIR"
RUNNER_RC=$?

if [ "$RUNNER_RC" -ne 0 ]; then
  fail runner "stealth-catalog runner exited $RUNNER_RC (CF-not-cleared / 0-rows / truncated / below floor)"
fi
if [ ! -s "$TMPFILE" ]; then
  # exit 0 but no file is a contract violation; refuse to push anyway.
  fail runner "runner exited 0 but produced no catalog file -- refusing to push"
fi
log "crawler OK -> $(wc -c <"$TMPFILE" | tr -d ' ') bytes"

# Row count, for the low-yield warning and for the freshness monitor. Counted
# with the venv python (already a hard preflight requirement) rather than jq,
# which is not a dependency of this script.
ROWS="$("$VENV_PY" - "$TMPFILE" <<'PYROWS' 2>/dev/null || echo 0
import json, sys
try:
    print(len(json.load(open(sys.argv[1])).get("rows", [])))
except Exception:
    print(0)
PYROWS
)"
case "${ROWS:-}" in ''|*[!0-9]*) ROWS=0 ;; esac
log "crawler rows : $ROWS (floor=$FLOOR warn-below=$LOW_YIELD cap=$MAX_ROWS)"
if [ "$ROWS" -lt "$LOW_YIELD" ]; then
  log "WARNING: low yield -- $ROWS rows is under the $LOW_YIELD warn threshold (floor $FLOOR)."
  log "         Publishing anyway (a small week is still the real set), but if this keeps"
  log "         falling the next stop is the floor, where the crawl writes NOTHING, this"
  log "         script exits non-zero, and -- if nobody reads that -- the device plays an"
  log "         ever-staler set. Monitor the PUBLISHED generated_at (check-staleness.sh)."
  log "         Deliberately NOT notifying here: this job runs every 3 h, notify() is"
  log "         titled \"refresh FAILED\", and a quiet week is not a failure. The"
  log "         low-yield surface is check-staleness.sh, which re-reads rows= below."
fi

# --- 2. Hand to the UNCHANGED validate/push/verify tail ----------------------
log "invoking refresh-catalog.sh --file (validate -> push -> verify) ..."
FCADE_NODE_BIN="$NODE_BIN" "$REFRESH" --file "$TMPFILE" --target "$VPS_TARGET"
REFRESH_RC=$?

if [ "$REFRESH_RC" -ne 0 ]; then
  fail push "refresh-catalog.sh exited $REFRESH_RC (validate/push/verify failed)"
fi

# --- 3. Success: record state for staleness detection -----------------------
# Extract generated_at from the pushed file for the freshness monitor.
GEN_AT="$("$VENV_PY" - "$TMPFILE" <<'PY' 2>/dev/null || true
import json, sys
try:
    print(int(json.load(open(sys.argv[1])).get("generated_at", 0)))
except Exception:
    print(0)
PY
)"
NOW_EPOCH="$(date +%s)"
# rows= is read back by check-staleness.sh -- see its LOW-YIELD check.
printf 'epoch=%s generated_at_ms=%s rows=%s target=%s\n' "$NOW_EPOCH" "${GEN_AT:-0}" "$ROWS" "$VPS_TARGET" > "$LAST_SUCCESS"
# Keep a local reference copy of the last catalog we successfully pushed.
cp -f "$TMPFILE" "$STATE_DIR/last-catalog.json" 2>/dev/null || true
record_run ok

log "SUCCESS: catalog pushed + verified; last-success recorded (generated_at_ms=${GEN_AT:-0} rows=$ROWS)."
log "==== daily run end ===="
exit 0
