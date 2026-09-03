#!/usr/bin/env bash
# tools/fcade-replays/auto-convert/preconvert-worker.sh
#
# The Mac-worker half of the pre-convert fleet (docs/plan-preconvert-fleet.md
# §2 Q2, Stage S5). Where convert-batch.sh walks the LOCAL catalog file and
# picks its own newest-unconverted quarks once a day, this script asks the
# VPS's own pre-convert queue (populated by its catalog-driven enqueuer,
# S2) what it would like converted RIGHT NOW, converts up to a few of those,
# and pushes them back -- coordinated via expiring leases so the VPS
# scheduler and this worker never step on the same quark.
#
# ONE TICK = one bounded unit of work, meant to be invoked every ~10 minutes
# by a LaunchAgent (StartInterval, no RunAtLoad -- a missed/offline tick is a
# no-op, not a failure):
#   preflight
#     -> worklease (claim <= FCADE_PRECONVERT_WORKER_COUNT items, default 3)
#     -> for each leased item:
#          synthesize a one-row catalog from the lease's row snapshot
#          -> publish_3sr.py --quark (from $RUNNER_DIR, same CWD gotcha as
#             convert-batch.sh) -- reuses $OUT_DIR as source of truth, so a
#             quark convert-batch.sh already converted locally is NOT
#             re-converted, just re-pushed
#          -> on success: push-3sr.sh --incoming + VPS perms fix -> workdone
#             {ok:true}
#          -> on failure: workdone {ok:false, reason} (frees the lease,
#             ledgers the failure server-side)
#          -> jittered inter-quark sleep (politeness pacing, §Q5)
#
# Politeness (docs/plan-preconvert-fleet.md §Q5): at most
# FCADE_PRECONVERT_WORKER_COUNT (<=3, server-clamped anyway) quarks per tick,
# a 60-120s jittered sleep between them, and publish_3sr.py itself pulls one
# quark at a time end-to-end (its own docstring/design) -- so this worker
# never drives more than ONE ggpo.fightcade.com connection at a time, same
# as convert-batch.sh.
#
# GOTCHA (same as convert-batch.sh): the FBNeo runner finds roms/ RELATIVE TO
# CWD, and publish_3sr.py spawns it inheriting publish's CWD -- publish MUST
# be launched from $RUNNER_DIR.
#
# TOKEN: FCADE_WORK_TOKEN is a SECRET (unlike everything else configured via
# the non-secret $INSTALL_ROOT/config.sh) -- it is never hardcoded here and
# never committed. See README.md "Token configuration": it lives in
# $INSTALL_ROOT/secrets.sh (chmod 600, gitignored, outside the repo by
# default install-root convention), sourced below IF PRESENT. In --dry-run,
# a missing token is a WARNING (a placeholder is substituted purely so the
# planned commands can be printed) -- a REAL run hard-fails without it.
#
# Usage:
#   preconvert-worker.sh                # run one tick (lease + convert + push)
#   preconvert-worker.sh --dry-run       # print the planned requests/commands; touch NOTHING (no network, no ssh, no conversion, no push)
#   FCADE_PRECONVERT_WORKER_DRY_RUN=1 preconvert-worker.sh   # same, via env

set -uo pipefail   # NOTE: deliberately no -e; exit codes are handled by hand (mirrors convert-batch.sh).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DRY_RUN=0
[ "${FCADE_PRECONVERT_WORKER_DRY_RUN:-0}" = "1" ] && DRY_RUN=1
if [ "${1:-}" = "--dry-run" ] || [ "${1:-}" = "-n" ]; then DRY_RUN=1; fi

# --- Config (defaults; override via env or $INSTALL_ROOT/config.sh) ----------
# SAME install root as convert-batch.sh -- this worker and the daily batch
# share $OUT_DIR (the "already converted" source of truth: a quark either
# job already has locally is never re-downloaded/re-run, only re-pushed) and
# the non-secret config.sh override file.
INSTALL_ROOT="${FCADE_CONVERT_HOME:-${XDG_DATA_HOME:-$HOME/Library/Application Support}/fcade-replay-convert}"

if [ -f "$INSTALL_ROOT/config.sh" ]; then
  # shellcheck disable=SC1091
  . "$INSTALL_ROOT/config.sh"
fi

# The FBNeo replay runner tree (CWD the runner needs) + its binary.
RUNNER_DIR="${FCADE_RUNNER_DIR:-$HOME/Developer/fbneo-replay-runner}"
RUNNER_BIN="${FCADE_RUNNER_BIN:-$RUNNER_DIR/build/debug/fbneosdldarm64}"
# The conversion venv python (rich; 3.10+) -- also used to run proxy_ops.py
# (stdlib-only, but sharing one interpreter keeps the toolchain minimal).
VENV_PY="${FCADE_CONVERT_VENV:-$RUNNER_DIR/venv}/bin/python"
# The stable statcheck copy setup.sh made (survives a repo build/ rebuild).
STATCHECK_BIN="${FCADE_STATCHECK_BIN:-$INSTALL_ROOT/bin/3S-ARM-statcheck}"
# Persistent .3sr store -- SHARED with convert-batch.sh (source of truth for
# "already converted", locally).
OUT_DIR="${FCADE_CONVERT_OUT:-$INSTALL_ROOT/3sr-out}"
# rsync/ssh VPS target (host alias + path; NOT a secret -- key lives in ~/.ssh).
VPS_TARGET="${FCADE_VPS_TARGET:-hetzner-3s-arm:/opt/fcade-proxy}"
# statcheck per-game gate timeout handed to publish_3sr.py.
STATCHECK_TIMEOUT="${FCADE_STATCHECK_TIMEOUT:-30}"

# --- Worker-specific config ---------------------------------------------------
# Up to 3 leased items per tick (docs/plan-preconvert-fleet.md §Q5; the
# server clamps to WORK_LEASE_MAX_COUNT=3 regardless of what's requested).
LEASE_COUNT="${FCADE_PRECONVERT_WORKER_COUNT:-3}"
# Jittered inter-quark sleep, seconds (§Q5: "60-120s jittered").
SLEEP_MIN_S="${FCADE_PRECONVERT_WORKER_SLEEP_MIN:-60}"
SLEEP_MAX_S="${FCADE_PRECONVERT_WORKER_SLEEP_MAX:-120}"
# ssh host alias (derived from VPS_TARGET, same as convert-batch.sh) and the
# proxy's loopback-only port on the VPS (fcade-proxy.js default 3479).
SSH_HOST="${VPS_TARGET%%:*}"
VPS_PATH="${VPS_TARGET#*:}"
PROXY_PORT="${FCADE_PROXY_PORT:-3479}"
SSH_CONNECT_TIMEOUT="${FCADE_PROXY_SSH_TIMEOUT:-10}"
RESPONSE_TIMEOUT="${FCADE_PROXY_RESPONSE_TIMEOUT:-20}"
# Worker identity sent with every lease/workdone call (must match
# fcade-proxy.js's WORKER_NAME_RE: [A-Za-z0-9_.-]{1,64}).
WORKER_NAME_RAW="${FCADE_WORKER_NAME:-mac-$(scutil --get ComputerName 2>/dev/null || hostname -s 2>/dev/null || echo unknown)}"
WORKER_NAME="$(printf '%s' "$WORKER_NAME_RAW" | tr -c 'A-Za-z0-9_.-' '-')"
WORKER_NAME="${WORKER_NAME:0:64}"
[ -n "$WORKER_NAME" ] || WORKER_NAME="mac-worker"

# In-repo tools (source scripts, not build artifacts).
PUBLISH="$SCRIPT_DIR/../publish_3sr.py"
PUSH="$SCRIPT_DIR/../../fcade-proxy/push-3sr.sh"
PROXY_OPS="$SCRIPT_DIR/proxy_ops.py"

# --- Derived runtime paths ---------------------------------------------------
LOGS_DIR="$INSTALL_ROOT/logs"
STATE_DIR="$INSTALL_ROOT/state"
LOGFILE="$LOGS_DIR/preconvert-worker.log"
LAST_SUCCESS="$STATE_DIR/preconvert-worker-last-success"
LAST_RUN="$STATE_DIR/preconvert-worker-last-run"
LEASE_CATALOGS_DIR="$STATE_DIR/preconvert-lease-catalogs"
LOCK_DIR="$STATE_DIR/preconvert-worker.lock"
SECRETS_FILE="$INSTALL_ROOT/secrets.sh"

mkdir -p "$LOGS_DIR" "$STATE_DIR" "$OUT_DIR" "$LEASE_CATALOGS_DIR"

# All output goes to the append-only logfile; ALSO to the terminal when
# interactive (mirrors convert-batch.sh; launchd points StandardOut/Err at
# the same file, so no dup there either).
if [ -t 1 ]; then
  exec > >(tee -a "$LOGFILE") 2>&1
else
  exec >> "$LOGFILE" 2>&1
fi

ts()  { date '+%Y-%m-%dT%H:%M:%S%z'; }
log() { printf '%s [preconvert-worker] %s\n' "$(ts)" "$*"; }

notify() {
  local msg="$1"
  command -v osascript >/dev/null 2>&1 || return 0
  osascript -e "display notification \"${msg//\"/\'}\" with title \"Fightcade pre-convert worker FAILED\"" >/dev/null 2>&1 || true
}

record_run() { printf '%s status=%s\n' "$(ts)" "$1" > "$LAST_RUN"; }

fail() {
  # fail <token> <human message> -- a HARD (missing-prereq / lease-request)
  # failure only. Per-quark failures are reported to the VPS via workdone
  # {ok:false} and do NOT abort the tick (mirrors convert-batch.sh's
  # per-quark posture).
  local token="$1"; shift
  log "FAILURE ($token): $*"
  record_run "$token"
  notify "$* (token=$token)"
  exit 1
}

# --- Lockfile: never let two ticks overlap ------------------------------------
# macOS ships no `flock(1)` (that's util-linux); `mkdir` is atomic on every
# POSIX filesystem this runs on, so it is the portable lock primitive here.
# A stale lock (owner process no longer alive) is reclaimed automatically.
lock_acquire() {
  local pid_file="$LOCK_DIR/pid"
  if mkdir "$LOCK_DIR" 2>/dev/null; then
    printf '%s\n' "$$" > "$pid_file"
    return 0
  fi
  if [ -f "$pid_file" ]; then
    local other_pid
    other_pid="$(cat "$pid_file" 2>/dev/null || echo '')"
    if [ -n "$other_pid" ] && kill -0 "$other_pid" 2>/dev/null; then
      return 1 # genuinely held by a live process
    fi
    log "reclaiming stale lock at $LOCK_DIR (pid $other_pid is not running)"
    rm -rf "$LOCK_DIR"
    if mkdir "$LOCK_DIR" 2>/dev/null; then
      printf '%s\n' "$$" > "$pid_file"
      return 0
    fi
  fi
  return 1
}
lock_release() { rm -rf "$LOCK_DIR" 2>/dev/null || true; }

log "==== preconvert-worker tick start ===="
log "install root  : $INSTALL_ROOT"
log "runner (CWD)  : $RUNNER_DIR"
log "out-dir       : $OUT_DIR (shared with convert-batch.sh)"
log "vps target    : $VPS_TARGET  (ssh host: $SSH_HOST, proxy port: $PROXY_PORT)"
log "worker name   : $WORKER_NAME"
log "lease count   : $LEASE_COUNT (server clamps to <= 3)"
[ "$DRY_RUN" = "1" ] && log "MODE          : DRY-RUN (zero network, zero ssh, zero conversion, zero push)"

# --- Preflight: hard failures if a prerequisite is missing (both modes) ------
[ -x "$RUNNER_BIN" ]    || fail preflight "FBNeo runner not found/executable at $RUNNER_BIN (run setup.sh / see docs/fcade-replay-notes.md §1)"
[ -x "$VENV_PY" ]       || fail preflight "conversion venv python not found at $VENV_PY (run setup.sh)"
[ -x "$STATCHECK_BIN" ] || fail preflight "statcheck copy not found at $STATCHECK_BIN (run setup.sh)"
[ -f "$PUBLISH" ]       || fail preflight "publish_3sr.py not found at $PUBLISH"
[ -x "$PUSH" ]          || fail preflight "push-3sr.sh not found/executable at $PUSH"
[ -f "$PROXY_OPS" ]     || fail preflight "proxy_ops.py not found at $PROXY_OPS"
"$VENV_PY" -c "import rich" 2>/dev/null || fail preflight "venv python at $VENV_PY cannot import rich (run setup.sh)"

log "proxy_ops.py self-test (offline, no network) ..."
if ! "$VENV_PY" "$PROXY_OPS" --selftest; then
  fail preflight "proxy_ops.py --selftest FAILED -- wire-framing code is broken; refusing to lease/push anything"
fi

# --- Token: a SECRET, never in config.sh, never committed --------------------
if [ -z "${FCADE_WORK_TOKEN:-}" ] && [ -f "$SECRETS_FILE" ]; then
  # shellcheck disable=SC1090
  . "$SECRETS_FILE"
fi
if [ -f "$SECRETS_FILE" ]; then
  _perm="$(stat -f '%Lp' "$SECRETS_FILE" 2>/dev/null || echo '')"
  case "$_perm" in
    600|400) ;; # fine
    *) log "WARNING: $SECRETS_FILE has permissions $_perm, not 600 -- consider: chmod 600 '$SECRETS_FILE'" ;;
  esac
fi

if [ -z "${FCADE_WORK_TOKEN:-}" ]; then
  if [ "$DRY_RUN" = "1" ]; then
    log "WARN: FCADE_WORK_TOKEN is not configured. DRY-RUN will use a PLACEHOLDER token"
    log "      ONLY to demonstrate request construction below -- no network call is ever"
    log "      made in --dry-run regardless, so this has no security implication."
    export FCADE_WORK_TOKEN="dry-run-placeholder-token-not-real"
  else
    fail preflight "FCADE_WORK_TOKEN not set. Create $SECRETS_FILE (chmod 600) containing:
        export FCADE_WORK_TOKEN=<value configured in the VPS fcade-proxy service's FCADE_WORK_TOKEN env>
      See README.md 'Token configuration'."
  fi
fi

# --- Lockfile: never let two ticks overlap (both modes) ---------------------
if ! lock_acquire; then
  fail lock "another preconvert-worker tick is already running (lock: $LOCK_DIR) -- exiting"
fi
trap lock_release EXIT

# ==============================================================================
# DRY-RUN: print exactly what a real tick WOULD request/run, touching nothing.
# ==============================================================================
if [ "$DRY_RUN" = "1" ]; then
  log ""
  log "DRY-RUN: exact worklease request this tick would send (via proxy_ops.py --print-only -- constructs and prints the real request object; makes NO subprocess, NO ssh, NO network call):"
  "$VENV_PY" "$PROXY_OPS" worklease --worker "$WORKER_NAME" --count "$LEASE_COUNT" \
    --host "$SSH_HOST" --port "$PROXY_PORT" --ssh-timeout "$SSH_CONNECT_TIMEOUT" --response-timeout "$RESPONSE_TIMEOUT" \
    --print-only

  log ""
  log "DRY-RUN: since no network call was made, no REAL lease was claimed and no real"
  log "         quarkid is known yet. Below is the per-quark pipeline SHAPE using a"
  log "         PLACEHOLDER quarkid (illustrative only -- not a real leased item):"
  PLACEHOLDER_QID="EXAMPLE-DRYRUN-QUARK-0000000000001"
  PLACEHOLDER_CATALOG="$LEASE_CATALOGS_DIR/$PLACEHOLDER_QID.json"

  log ""
  log "  1. would write a synthetic one-row catalog (the lease response's row"
  log "     snapshot, so publish_3sr.py can convert a quark even if it has since"
  log "     left this Mac's local catalog copy) to:"
  log "       $PLACEHOLDER_CATALOG"
  log "     shape: {\"rows\": [<row snapshot from the worklease response>]}"

  log ""
  log "  2. would check \$OUT_DIR/$PLACEHOLDER_QID/game_*.3sr first (out-dir as"
  log "     source of truth, shared with convert-batch.sh) -- if absent, would run"
  log "     (from CWD=$RUNNER_DIR, the runner's roms/-relative-to-CWD gotcha):"
  log "       $VENV_PY $PUBLISH \\"
  log "         --catalog $PLACEHOLDER_CATALOG --runner $RUNNER_BIN \\"
  log "         --statcheck $STATCHECK_BIN --out-dir $OUT_DIR \\"
  log "         --quark $PLACEHOLDER_QID --statcheck-timeout $STATCHECK_TIMEOUT"

  log ""
  log "  3. if >=1 game_N.3sr resulted, would push to the VPS staging dir and fix perms:"
  log "       $PUSH --incoming $OUT_DIR $VPS_TARGET $PLACEHOLDER_QID"
  log "       ssh $SSH_HOST \"sudo chown -R fcade-proxy:fcade-proxy '$VPS_PATH/3sr-incoming/$PLACEHOLDER_QID' && ...chmod...\""

  log ""
  log "  4. would then report the outcome (exact request, via --print-only, no network):"
  "$VENV_PY" "$PROXY_OPS" workdone --worker "$WORKER_NAME" --quarkid "$PLACEHOLDER_QID" --ok true \
    --host "$SSH_HOST" --port "$PROXY_PORT" --ssh-timeout "$SSH_CONNECT_TIMEOUT" --response-timeout "$RESPONSE_TIMEOUT" \
    --print-only
  log "     (or, on a conversion failure, ok:false with a reason such as no_savestate/no_games -- e.g.:)"
  "$VENV_PY" "$PROXY_OPS" workdone --worker "$WORKER_NAME" --quarkid "$PLACEHOLDER_QID" --ok false --reason no_savestate \
    --host "$SSH_HOST" --port "$PROXY_PORT" --ssh-timeout "$SSH_CONNECT_TIMEOUT" --response-timeout "$RESPONSE_TIMEOUT" \
    --print-only

  log ""
  log "  5. would sleep a jittered ${SLEEP_MIN_S}-${SLEEP_MAX_S}s before the next leased quark"
  log "     (up to $LEASE_COUNT per tick -- politeness pacing, docs/plan-preconvert-fleet.md §Q5)."

  rm -f "$PLACEHOLDER_CATALOG" 2>/dev/null || true
  log ""
  log "DRY-RUN: zero network bytes sent, zero ssh connections opened, zero files pushed, zero conversions run."
  record_run ok-dryrun
  log "==== preconvert-worker tick end (dry-run) ===="
  exit 0
fi

# ==============================================================================
# REAL run
# ==============================================================================

classify_failure_reason() {
  # classify_failure_reason <out-dir> <quarkid>
  # Best-effort: publish_3sr.py's published_manifest.json (overwritten each
  # invocation -- we invoke it one quark at a time, so it reflects THIS
  # quark) records a per-quark "error" string on a hard failure (download/
  # runner) and empty published[]/non-empty divergent[] on an all-statcheck-
  # divergent run. publish_3sr.py does not emit a TYPED reason (unlike the
  # VPS's own downloader, which fcade-proxy.js S2 gave a typed job.failReason
  # for exactly this ambiguity) -- this is a heuristic substring classifier,
  # good enough to route into the server's no_savestate-vs-other retry policy
  # (fcade-proxy.js preconvertRecordFailure) without over-claiming precision.
  local out_dir="$1" quarkid="$2"
  "$VENV_PY" - "$out_dir" "$quarkid" <<'PY'
import json, sys
from pathlib import Path
out_dir, quarkid = Path(sys.argv[1]), sys.argv[2]
manifest_path = out_dir / "published_manifest.json"
reason = "no_games"
try:
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    entry = (data.get("quarks") or {}).get(quarkid)
    if entry:
        err = entry.get("error")
        if isinstance(err, str) and err:
            reason = "no_savestate" if "savestate" in err.lower() else "publish_error"
except Exception:
    pass
print(reason)
PY
}

sleep_jitter() {
  local span=$(( SLEEP_MAX_S - SLEEP_MIN_S + 1 ))
  [ "$span" -lt 1 ] && span=1
  local s=$(( (RANDOM % span) + SLEEP_MIN_S ))
  log "sleeping ${s}s (jittered politeness pacing) before the next quark ..."
  sleep "$s"
}

log "requesting worklease (worker=$WORKER_NAME, count=$LEASE_COUNT) ..."
LEASE_RESP_TMP="$(mktemp "$STATE_DIR/worklease-response.XXXXXX")"
"$VENV_PY" "$PROXY_OPS" worklease --worker "$WORKER_NAME" --count "$LEASE_COUNT" \
  --host "$SSH_HOST" --port "$PROXY_PORT" --ssh-timeout "$SSH_CONNECT_TIMEOUT" --response-timeout "$RESPONSE_TIMEOUT" \
  > "$LEASE_RESP_TMP"
LEASE_RC=$?
if [ "$LEASE_RC" -ne 0 ]; then
  cat "$LEASE_RESP_TMP"
  rm -f "$LEASE_RESP_TMP"
  fail worklease "worklease request failed (rc=$LEASE_RC) -- see output above (ssh/network/token problem?)"
fi
log "worklease response: $(cat "$LEASE_RESP_TMP")"

# Parse the response: write one synthetic single-row catalog per leased item
# under $LEASE_CATALOGS_DIR, print one quarkid per line to stdout (consumed
# below the same bash-3.2-safe way convert-batch.sh reads its selection).
SEL_TMP="$(mktemp "$STATE_DIR/lease-select.XXXXXX")"
"$VENV_PY" - "$LEASE_RESP_TMP" "$LEASE_CATALOGS_DIR" > "$SEL_TMP" 2>>"$LOGFILE" <<'PY'
import json, sys
from pathlib import Path

resp_path, catalogs_dir = sys.argv[1], Path(sys.argv[2])
catalogs_dir.mkdir(parents=True, exist_ok=True)
data = json.loads(Path(resp_path).read_text(encoding="utf-8"))
if not data.get("ok"):
    print(f"worklease response not ok: {data}", file=sys.stderr)
    sys.exit(2)
leased = data.get("leased") or []
for item in leased:
    qid = item.get("quarkid")
    row = item.get("row")
    if not qid or not isinstance(row, dict):
        print(f"leased item missing quarkid/row, skipping: {item}", file=sys.stderr)
        continue
    cat_path = catalogs_dir / f"{qid}.json"
    cat_path.write_text(json.dumps({"rows": [row]}), encoding="utf-8")
    print(qid)
PY
SELECT_RC=$?
rm -f "$LEASE_RESP_TMP"
if [ "$SELECT_RC" -ne 0 ]; then
  rm -f "$SEL_TMP"
  fail worklease "parsing the worklease response failed (rc=$SELECT_RC) -- see $LOGFILE"
fi

SELECTED=()
while IFS= read -r _q; do
  [ -n "$_q" ] && SELECTED+=("$_q")
done < "$SEL_TMP"
rm -f "$SEL_TMP"

if [ "${#SELECTED[@]}" -eq 0 ]; then
  log "worklease returned zero leasable items (queue empty, or everything already leased elsewhere) -- nothing to do this tick."
  record_run ok-nothing
  log "==== preconvert-worker tick end ===="
  exit 0
fi

log "leased ${#SELECTED[@]} quark(s): ${SELECTED[*]}"

TICK_CONVERTED=0
TICK_PUSHED=0
TICK_FAILED=0
LAST_INDEX=$(( ${#SELECTED[@]} - 1 ))

for i in "${!SELECTED[@]}"; do
  QID="${SELECTED[$i]}"
  CAT_FILE="$LEASE_CATALOGS_DIR/$QID.json"
  log "--- quark $QID ($((i + 1))/${#SELECTED[@]}) ---"

  if compgen -G "$OUT_DIR/$QID/game_*.3sr" >/dev/null 2>&1; then
    log "already present in \$OUT_DIR (shared out-dir source of truth) -- skipping conversion, pushing existing files."
  else
    log "converting via publish_3sr.py (CWD=$RUNNER_DIR) ..."
    (
      cd "$RUNNER_DIR" || exit 97
      exec "$VENV_PY" "$PUBLISH" \
        --catalog "$CAT_FILE" \
        --runner "$RUNNER_BIN" \
        --statcheck "$STATCHECK_BIN" \
        --out-dir "$OUT_DIR" \
        --quark "$QID" \
        --statcheck-timeout "$STATCHECK_TIMEOUT"
    )
    PUB_RC=$?
    log "publish_3sr.py exited $PUB_RC (0=published, 1=nothing published for this quark)"
  fi

  if ! compgen -G "$OUT_DIR/$QID/game_*.3sr" >/dev/null 2>&1; then
    REASON="$(classify_failure_reason "$OUT_DIR" "$QID")"
    log "no .3sr produced for $QID -- reporting workdone ok:false reason=$REASON"
    "$VENV_PY" "$PROXY_OPS" workdone --worker "$WORKER_NAME" --quarkid "$QID" --ok false --reason "$REASON" \
      --host "$SSH_HOST" --port "$PROXY_PORT" --ssh-timeout "$SSH_CONNECT_TIMEOUT" --response-timeout "$RESPONSE_TIMEOUT"
    WD_RC=$?
    [ "$WD_RC" -ne 0 ] && log "WARNING: workdone(ok:false) itself failed (rc=$WD_RC) for $QID -- lease will simply expire server-side instead."
    TICK_FAILED=$((TICK_FAILED + 1))
    rm -f "$CAT_FILE"
    [ "$i" -lt "$LAST_INDEX" ] && sleep_jitter
    continue
  fi

  TICK_CONVERTED=$((TICK_CONVERTED + 1))
  log "pushing $QID to $VPS_TARGET/3sr-incoming ..."
  "$PUSH" --incoming "$OUT_DIR" "$VPS_TARGET" "$QID"
  PUSH_RC=$?
  if [ "$PUSH_RC" -ne 0 ]; then
    log "WARNING: push-3sr.sh --incoming exited $PUSH_RC for $QID -- NOT calling workdone (a partial/failed push must not be validated as if complete). Lease will expire server-side and it will be retried."
    rm -f "$CAT_FILE"
    [ "$i" -lt "$LAST_INDEX" ] && sleep_jitter
    continue
  fi

  log "fixing VPS perms on $SSH_HOST:$VPS_PATH/3sr-incoming/$QID ..."
  ssh "$SSH_HOST" "sudo chown -R fcade-proxy:fcade-proxy '$VPS_PATH/3sr-incoming/$QID' && sudo find '$VPS_PATH/3sr-incoming/$QID' -type d -exec chmod 755 {} \\; && sudo find '$VPS_PATH/3sr-incoming/$QID' -type f -exec chmod 644 {} \\;" \
    && log "VPS perms fixed." \
    || log "WARNING: VPS perm fix failed (ssh/sudo) -- workdone validation may fail to read the staged files."

  log "calling workdone (ok=true) for $QID ..."
  "$VENV_PY" "$PROXY_OPS" workdone --worker "$WORKER_NAME" --quarkid "$QID" --ok true \
    --host "$SSH_HOST" --port "$PROXY_PORT" --ssh-timeout "$SSH_CONNECT_TIMEOUT" --response-timeout "$RESPONSE_TIMEOUT"
  WD_RC=$?
  if [ "$WD_RC" -eq 0 ]; then
    log "workdone: integrated OK."
    TICK_PUSHED=$((TICK_PUSHED + 1))
  else
    log "WARNING: workdone(ok:true) failed/rejected (rc=$WD_RC) for $QID -- staged files may have been wiped server-side (validation failure) or the disk floor was hit; will be retried on a future tick."
  fi

  rm -f "$CAT_FILE"
  [ "$i" -lt "$LAST_INDEX" ] && sleep_jitter
done

NOW_EPOCH="$(date +%s)"
printf 'epoch=%s leased=%s converted=%s pushed=%s failed=%s worker=%s target=%s\n' \
  "$NOW_EPOCH" "${#SELECTED[@]}" "$TICK_CONVERTED" "$TICK_PUSHED" "$TICK_FAILED" "$WORKER_NAME" "$VPS_TARGET" > "$LAST_SUCCESS"
record_run ok
log "SUCCESS: leased ${#SELECTED[@]}, converted $TICK_CONVERTED, pushed $TICK_PUSHED, failed $TICK_FAILED."
log "==== preconvert-worker tick end ===="
exit 0
