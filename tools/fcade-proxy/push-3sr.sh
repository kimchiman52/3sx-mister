#!/usr/bin/env bash
# Rsync a local tree of converted replays (tools/fcade-replays/publish_3sr.py
# output: <quarkid>/game_N.3sr + game_N.meta.json) to the VPS `.3sr` store
# that fcade-proxy.js's `get3sr` op serves from -- OR, with `--incoming`
# (plan-preconvert-fleet.md §2 Q2 / Stage S5), to the STAGING dir that the
# Mac-worker rail pushes into ahead of server-side validation.
#
# docs/plan-osd-replay-browser.md Stage S1 item 3. This is a SEPARATE rail
# from push-catalog.sh: push-catalog.sh ships exactly one file and renames it
# to catalog.json; this script ships a whole per-quark directory tree and
# preserves every filename verbatim (get3sr reads <3sr-root>/<quarkid>/
# game_N.3sr + game_N.meta.json directly by name -- do not rename anything).
#
# Usage:
#   ./push-3sr.sh [--incoming] <local-3sr-dir> user@host:/opt/fcade-proxy [<quarkid> ...]
#
# --incoming   Ship into <target>/3sr-incoming/ (the Mac-worker staging root,
#              fcade-proxy.js `resolveIncomingDir()`) instead of <target>/3sr/
#              (the SERVED store). Files landing here are inert until a
#              `workdone {ok:true}` call validates and atomically renames
#              them into 3sr/ -- see auto-convert/preconvert-worker.sh and
#              auto-convert/proxy_ops.py. Without this flag, behavior is
#              UNCHANGED: the legacy direct-to-3sr/ push (still used by
#              convert-batch.sh's daily manual/batch rail).
#
# <local-3sr-dir> is a publish_3sr.py --out-dir (containing one subdirectory
# per quarkid, plus a published_manifest.json). With no quarkid arguments,
# the ENTIRE <local-3sr-dir> is pushed (published_manifest.json included, for
# operator reference only -- fcade-proxy.js's get3sr never reads it, it
# lists each quark directory directly). With one or more quarkid arguments,
# only those specific <local-3sr-dir>/<quarkid>/ subdirectories are pushed
# (useful for shipping a freshly-converted quark without re-syncing an
# entire large local output tree). The --incoming rail is meant to always be
# used with explicit quarkid arguments (one worker-converted quark at a
# time) -- pushing the whole local tree into 3sr-incoming/ would be unusual
# but is not specially guarded against.
#
# Like push-catalog.sh: no --delete (this rail only ever adds/overwrites
# files, never removes anything already on the VPS -- an operator who wants
# to retire a quark does so explicitly, not as a side effect of a push).

set -euo pipefail

INCOMING=0
if [ "${1:-}" = "--incoming" ]; then
  INCOMING=1
  shift
fi

LOCAL="${1:?Usage: ./push-3sr.sh [--incoming] <local-3sr-dir> user@host:/opt/fcade-proxy [<quarkid> ...]}"
TARGET="${2:?Usage: ./push-3sr.sh [--incoming] <local-3sr-dir> user@host:/opt/fcade-proxy [<quarkid> ...]}"
shift 2 || true
QUARKIDS=("$@")

if [ ! -d "$LOCAL" ]; then
  echo "error: $LOCAL is not a directory" >&2
  exit 1
fi

if [ "$INCOMING" -eq 1 ]; then
  DEST_SUBDIR="3sr-incoming"
else
  DEST_SUBDIR="3sr"
fi

# The remote dir (either the served store or the incoming staging root) must
# exist before rsync-ing into it.
ssh "${TARGET%%:*}" "mkdir -p '${TARGET#*:}/$DEST_SUBDIR'"

if [ "${#QUARKIDS[@]}" -eq 0 ]; then
  echo "Pushing entire local tree: $LOCAL -> $TARGET/$DEST_SUBDIR/"
  rsync -avz "$LOCAL"/ "$TARGET/$DEST_SUBDIR/"
else
  for quarkid in "${QUARKIDS[@]}"; do
    src="$LOCAL/$quarkid"
    if [ ! -d "$src" ]; then
      echo "error: $src does not exist -- skipping" >&2
      continue
    fi
    echo "Pushing quark: $src -> $TARGET/$DEST_SUBDIR/$quarkid/"
    rsync -avz "$src"/ "$TARGET/$DEST_SUBDIR/$quarkid/"
  done
fi

echo ""
if [ "$INCOMING" -eq 1 ]; then
  echo "NOTE: this landed in the STAGING dir ($DEST_SUBDIR/), which nothing"
  echo "serves. Fix perms (below), then call the 'workdone' op ({ok:true}) for"
  echo "each pushed quarkid so fcade-proxy validates (read3srGameData: magic /"
  echo "size / meta / named players) and atomically renames it into 3sr/ --"
  echo "see auto-convert/proxy_ops.py workdone and auto-convert/README.md."
else
  echo "NOTE: no service restart needed. fcade-proxy.js's get3sr reads"
  echo "<FCADE_3SR_DIR>/<quarkid>/game_N.3sr (+ game_N.meta.json) directly off"
  echo "disk on every request -- this rsync takes effect immediately."
fi
echo ""
echo "If this is the first push to a fresh host, fix ownership/perms so the"
echo "fcade-proxy service user can read it, e.g.:"
echo "  ssh <host> \"sudo chown -R fcade-proxy:fcade-proxy '${TARGET#*:}/$DEST_SUBDIR' && sudo find '${TARGET#*:}/$DEST_SUBDIR' -type d -exec chmod 755 {} \\; && sudo find '${TARGET#*:}/$DEST_SUBDIR' -type f -exec chmod 644 {} \\;\""
