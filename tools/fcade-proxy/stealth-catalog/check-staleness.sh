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
# Exit 0 = fresh, exit 1 = STALE / never-succeeded. Prints a one-line verdict.
# Fires a macOS notification when stale (best effort). Read it by hand, from a
# LaunchAgent, or from any monitoring you wire up.
#
# Usage: ./check-staleness.sh [MAX_AGE_HOURS]   (default 26h -- a daily cadence
#        plus slack, so one missed 04:00 run trips it).

set -uo pipefail

INSTALL_ROOT="${FCADE_CATALOG_HOME:-${XDG_DATA_HOME:-$HOME/Library/Application Support}/fcade-stealth-catalog}"
LAST_SUCCESS="$INSTALL_ROOT/state/last-success"
MAX_AGE_HOURS="${1:-26}"

notify() {
  command -v osascript >/dev/null 2>&1 || return 0
  osascript -e "display notification \"$1\" with title \"Fightcade catalog STALE\"" >/dev/null 2>&1 || true
}

if [ ! -f "$LAST_SUCCESS" ]; then
  echo "STALE: no successful run recorded yet ($LAST_SUCCESS missing)."
  notify "No successful catalog refresh has ever run."
  exit 1
fi

# last-success format: "epoch=<sec> generated_at_ms=<ms> target=<...>"
LAST_EPOCH="$(sed -n 's/.*epoch=\([0-9][0-9]*\).*/\1/p' "$LAST_SUCCESS" | head -1)"
case "${LAST_EPOCH:-}" in
  ''|*[!0-9]*)
    echo "STALE: could not parse last-success epoch from $LAST_SUCCESS."
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
  echo "STALE: last successful refresh was $AGE_HOURS h ago ($LAST_HUMAN) > ${MAX_AGE_HOURS}h threshold."
  notify "Last refresh was ${AGE_HOURS}h ago -- run the manual bookmarklet."
  exit 1
fi

echo "FRESH: last successful refresh $AGE_HOURS h ago ($LAST_HUMAN), within ${MAX_AGE_HOURS}h."
exit 0
