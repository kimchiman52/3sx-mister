#!/usr/bin/env bash
# tools/fcade-proxy/stealth-catalog/check-staleness.sh
#
# Dependency-free staleness monitor for the daily stealth-catalog refresh
# (plan-fcade-stealth-catalog.md §6.2). Catalog mode has NO built-in "stale"
# signal -- the proxy happily serves an ever-older catalog with ok:true -- so
# staleness must be checked from the runner side. run-daily.sh writes
# <install-root>/state/last-success on every successful push; this reads that
# timestamp and reports whether it is older than a threshold.
#
# WHAT IT MEASURES: the PUBLISHED catalog's `generated_at`, not run-daily.sh's
# exit code. That distinction is the whole point. run-daily.sh does exit
# non-zero on every refusal path (below-floor, dirty crawl, CF failure, failed
# push) -- but a non-zero exit from a background LaunchAgent that nobody reads
# is indistinguishable from success, and the proxy serves a months-old catalog
# with ok:true forever. The only observable that cannot lie is the timestamp
# baked into the catalog the device is actually being served.
#
# It also re-checks the LOW-YIELD signal run-daily.sh records (rows=), because
# the weekly-best crawl is a bounded ~150-row set: a yield sliding toward the
# push-safety floor is the early warning that the next run will refuse to write
# and staleness will start accumulating silently.
#
# Exit 0 = fresh, exit 1 = STALE / never-succeeded / below the row floor.
# Prints a one-line verdict. Fires a macOS notification when stale (best
# effort). Read it by hand, from a LaunchAgent, or from any monitoring you
# wire up.
#
# Usage: ./check-staleness.sh [MAX_AGE_HOURS]   (default 26h -- a daily cadence
#        plus slack, so one missed 04:00 run trips it).

set -uo pipefail

INSTALL_ROOT="${FCADE_CATALOG_HOME:-${XDG_DATA_HOME:-$HOME/Library/Application Support}/fcade-stealth-catalog}"
LAST_SUCCESS="$INSTALL_ROOT/state/last-success"
MAX_AGE_HOURS="${1:-26}"
# Below this many rows in the last published catalog, warn. Keep in step with
# run-daily.sh's FCADE_LOW_YIELD_ROWS (same default, same meaning).
LOW_YIELD="${FCADE_LOW_YIELD_ROWS:-100}"
# Hard floor run-daily.sh refuses to publish under. If a published catalog is
# somehow at/below it, that is a fault, not a quiet week.
FLOOR="${FCADE_MIN_ROWS:-30}"

notify() {
  command -v osascript >/dev/null 2>&1 || return 0
  osascript -e "display notification \"$1\" with title \"Fightcade catalog STALE\"" >/dev/null 2>&1 || true
}

if [ ! -f "$LAST_SUCCESS" ]; then
  echo "STALE: no successful run recorded yet ($LAST_SUCCESS missing)."
  notify "No successful catalog refresh has ever run."
  exit 1
fi

# last-success format:
#   "epoch=<sec> generated_at_ms=<ms> rows=<n> target=<...>"
# `generated_at_ms` is the timestamp INSIDE the catalog that was pushed -- what
# the device is being served -- so it is the value this monitor keys on.
# `epoch` (when the push happened) is the fallback for a last-success file
# written before rows=/generated_at_ms existed. The two differ only by the push
# duration in practice, but generated_at_ms is the one with device meaning.
GEN_AT_MS="$(sed -n 's/.*generated_at_ms=\([0-9][0-9]*\).*/\1/p' "$LAST_SUCCESS" | head -1)"
LAST_EPOCH="$(sed -n 's/.*[^_]epoch=\([0-9][0-9]*\).*/\1/p' "$LAST_SUCCESS" | head -1)"
if [ -z "${LAST_EPOCH:-}" ]; then
  LAST_EPOCH="$(sed -n 's/^epoch=\([0-9][0-9]*\).*/\1/p' "$LAST_SUCCESS" | head -1)"
fi
ROWS="$(sed -n 's/.*rows=\([0-9][0-9]*\).*/\1/p' "$LAST_SUCCESS" | head -1)"

# Prefer the published catalog's own generated_at; fall back to the push epoch.
SOURCE="published generated_at"
case "${GEN_AT_MS:-}" in
  ''|*[!0-9]*|0) GEN_AT_MS="" ;;
esac
if [ -n "$GEN_AT_MS" ]; then
  LAST_EPOCH=$(( GEN_AT_MS / 1000 ))
else
  SOURCE="push epoch (no generated_at_ms recorded)"
fi

case "${LAST_EPOCH:-}" in
  ''|*[!0-9]*)
    echo "STALE: could not parse a timestamp from $LAST_SUCCESS."
    notify "last-success file is unreadable."
    exit 1
    ;;
esac

NOW="$(date +%s)"
AGE_SEC=$(( NOW - LAST_EPOCH ))
AGE_HOURS=$(( AGE_SEC / 3600 ))
MAX_SEC=$(( MAX_AGE_HOURS * 3600 ))
LAST_HUMAN="$(date -r "$LAST_EPOCH" 2>/dev/null || echo "epoch $LAST_EPOCH")"

if [ "$AGE_SEC" -gt "$MAX_SEC" ]; then
  echo "STALE: catalog the device is served is $AGE_HOURS h old ($LAST_HUMAN, from $SOURCE) > ${MAX_AGE_HOURS}h threshold."
  notify "Served catalog is ${AGE_HOURS}h old -- run the manual bookmarklet."
  exit 1
fi

# Fresh by timestamp -- now the yield of the weekly-best set it carries.
case "${ROWS:-}" in
  ''|*[!0-9]*)
    echo "FRESH: catalog is $AGE_HOURS h old ($LAST_HUMAN, from $SOURCE), within ${MAX_AGE_HOURS}h. (row count not recorded)"
    exit 0
    ;;
esac
if [ "$ROWS" -le "$FLOOR" ]; then
  echo "STALE-RISK: catalog is fresh ($AGE_HOURS h) but carries only $ROWS rows, at/below the publish floor ($FLOOR) -- the next crawl may refuse to write."
  notify "Catalog has only $ROWS rows (floor $FLOOR)."
  exit 1
fi
if [ "$ROWS" -lt "$LOW_YIELD" ]; then
  echo "FRESH (LOW YIELD): catalog is $AGE_HOURS h old ($LAST_HUMAN, from $SOURCE) but carries $ROWS rows, under the $LOW_YIELD warn threshold (floor $FLOOR)."
  exit 0
fi

echo "FRESH: catalog is $AGE_HOURS h old ($LAST_HUMAN, from $SOURCE) with $ROWS rows, within ${MAX_AGE_HOURS}h."
exit 0
