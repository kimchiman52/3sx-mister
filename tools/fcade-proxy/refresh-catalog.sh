#!/usr/bin/env bash
# 3SX fcade-proxy: hands-off tail of the offline-catalog refresh (option B).
#
# See README.md "Offline catalog (option B)" for the full picture. The ONE
# irreducible human step is passing Cloudflare in a real, logged-in browser
# tab (browser-catalog.js / its bookmarklet) -- that cannot be automated
# (see browser-catalog.js's header for why). Everything AFTER the download
# -- detect it, validate it, push it, confirm the proxy went live -- is what
# this script automates, so the user's part of the refresh flow is just:
# click the bookmarklet, then run (or already have running) this script.
#
# Usage:
#   ./refresh-catalog.sh [--downloads DIR] [--target user@host:/path]
#                         [--timeout SECS] [--file PATH]
#
#   --downloads DIR   Directory to watch for the downloaded catalog.
#                      Default: $HOME/Downloads
#   --target SPEC      rsync target, same shape push-catalog.sh expects
#                      (user@host:/path). Default: hetzner-3s-arm:/opt/fcade-proxy
#   --timeout SECS     How long to wait for the download in watch mode.
#                      Default: 300
#   --file PATH        Skip watching entirely and push this exact file now
#                      (re-push / testing). Still validated before pushing.
#
# Flow (watch mode, the default):
#   1. Record the start time.
#   2. Print a call-to-action: go click the bookmarklet (or paste
#      browser-catalog.js into the console).
#   3. Poll DOWNLOADS_DIR every ~2s for the newest fcade-catalog*.json file
#      whose mtime is >= the recorded start time (so a stale file from a
#      previous run is never picked up), handling the browser's
#      "fcade-catalog (1).json" duplicate-name pattern.
#   4. Once found, wait for its size to stabilize across two polls (so a
#      still-downloading file is never pushed half-written).
#   5. Validate the JSON shape (see validate.js below). Reject and stop,
#      pushing nothing, on anything malformed.
#   6. Push via push-catalog.sh (same rsync, no --delete, single file).
#   7. Verify over SSH: connect to the VPS and speak the wire protocol's
#      `{"op":"status"}` directly against 127.0.0.1:3479 (the proxy is not
#      reachable from the Mac directly -- external TCP 3479 is firewalled
#      off pending a user firewall rule), and assert mode=="catalog" with
#      matching row count + generated_at.
#   8. Print a plain-language success summary.
#
# Robustness notes: written for macOS's stock /bin/bash (3.2) + BSD
# coreutils (no GNU-only stat/date/find flags), quotes every variable
# expansion, and cleans up its temp workdir on exit via trap.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DOWNLOADS_DIR="${HOME}/Downloads"
TARGET="hetzner-3s-arm:/opt/fcade-proxy"
TIMEOUT_SECS=300
EXPLICIT_FILE=""
NODE_BIN="${FCADE_NODE_BIN:-node}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [--downloads DIR] [--target user@host:/path] [--timeout SECS] [--file PATH]

  --downloads DIR   Directory to watch for the downloaded catalog.
                    Default: \$HOME/Downloads (currently: ${DOWNLOADS_DIR})
  --target SPEC     rsync target (user@host:/path), same shape push-catalog.sh
                    expects. Default: hetzner-3s-arm:/opt/fcade-proxy
  --timeout SECS    Seconds to wait for the download in watch mode. Default: 300
  --file PATH       Skip watching; validate and push this exact file now.
  -h, --help        Show this help.
EOF
}

require_arg() {
  # require_arg <flag-name> <remaining-arg-count>
  if [ "$2" -lt 2 ]; then
    echo "error: $1 requires a value" >&2
    usage >&2
    exit 1
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --downloads)
      require_arg "--downloads" "$#"
      DOWNLOADS_DIR="$2"
      shift 2
      ;;
    --target)
      require_arg "--target" "$#"
      TARGET="$2"
      shift 2
      ;;
    --timeout)
      require_arg "--timeout" "$#"
      case "$2" in
        ''|*[!0-9]*)
          echo "error: --timeout must be a positive integer (got: $2)" >&2
          exit 1
          ;;
      esac
      TIMEOUT_SECS="$2"
      shift 2
      ;;
    --file)
      require_arg "--file" "$#"
      EXPLICIT_FILE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if ! command -v "$NODE_BIN" >/dev/null 2>&1; then
  echo "error: node executable '$NODE_BIN' not found (set FCADE_NODE_BIN to override)" >&2
  exit 1
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/fcade-refresh.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT

# --- validate.js: strict shape check, mirrors README.md's documented
# catalog file format. Prints ROWS=/GENERATED_AT=/SAMPLE= lines on success
# (parsed by this script), a VALIDATE_FAIL: reason on stderr and exits 1 on
# anything malformed. Nothing here ever pushes -- this is read-only.
cat > "$WORKDIR/validate.js" <<'EOF'
'use strict';
const fs = require('fs');

const filePath = process.argv[2];
let raw;
try {
  raw = fs.readFileSync(filePath, 'utf8');
} catch (err) {
  console.error('VALIDATE_FAIL: cannot read file: ' + (err && err.message ? err.message : err));
  process.exit(1);
}

let json;
try {
  json = JSON.parse(raw);
} catch (err) {
  console.error('VALIDATE_FAIL: not valid JSON: ' + (err && err.message ? err.message : err));
  process.exit(1);
}

if (!json || typeof json !== 'object' || Array.isArray(json)) {
  console.error('VALIDATE_FAIL: top-level value must be a JSON object');
  process.exit(1);
}
if (!Array.isArray(json.rows)) {
  console.error('VALIDATE_FAIL: missing a "rows" array');
  process.exit(1);
}
if (json.rows.length === 0) {
  console.error('VALIDATE_FAIL: "rows" array is empty');
  process.exit(1);
}

for (let i = 0; i < json.rows.length; i++) {
  const r = json.rows[i];
  if (!r || typeof r !== 'object') {
    console.error('VALIDATE_FAIL: row ' + i + ' is not an object');
    process.exit(1);
  }
  if (typeof r.quarkid !== 'string' || r.quarkid.length === 0) {
    console.error('VALIDATE_FAIL: row ' + i + ' is missing a string "quarkid"');
    process.exit(1);
  }
  if (!Array.isArray(r.players)) {
    console.error('VALIDATE_FAIL: row ' + i + ' (quarkid ' + r.quarkid + ') is missing a "players" array');
    process.exit(1);
  }
}

const generatedAt = typeof json.generated_at === 'number' ? json.generated_at : null;
console.log('ROWS=' + json.rows.length);
console.log('GENERATED_AT=' + (generatedAt === null ? '' : generatedAt));

const sampleCount = Math.min(3, json.rows.length);
for (let i = 0; i < sampleCount; i++) {
  const r = json.rows[i];
  const names = r.players
    .map(function (p) {
      return p && typeof p.name === 'string' && p.name.length > 0 ? p.name : '?';
    })
    .join(' vs ');
  console.log('SAMPLE=' + r.quarkid + ': ' + (names.length > 0 ? names : '(no players)'));
}
EOF

# --- verify.js: speaks the exact wire protocol (u32be length prefix + JSON,
# see README.md "Wire protocol") to 127.0.0.1:3479 with a single
# {"op":"status"} request, then asserts mode=="catalog" and (if expected
# values were passed) that catalog_rows/catalog_generated_at match what was
# just pushed. Meant to run ON the VPS host (piped over ssh via `node -`),
# since the proxy only binds 127.0.0.1 there and external 3479 is
# firewalled off. Everything is printed to stdout so a single `2>&1`
# capture over ssh sees it all; exit code is the pass/fail signal.
cat > "$WORKDIR/verify.js" <<'EOF'
'use strict';
const net = require('net');

const port = Number(process.env.FCADE_VERIFY_PORT || '3479');
const expectedRowsArg = process.argv[2];
const expectedGeneratedAtArg = process.argv[3];
const expectedRows = expectedRowsArg !== undefined && expectedRowsArg !== '' ? Number(expectedRowsArg) : null;
const expectedGeneratedAt =
  expectedGeneratedAtArg !== undefined && expectedGeneratedAtArg !== '' ? Number(expectedGeneratedAtArg) : null;

function encodeFrame(obj) {
  const json = Buffer.from(JSON.stringify(obj), 'utf8');
  const out = Buffer.alloc(4 + json.length);
  out.writeUInt32BE(json.length, 0);
  json.copy(out, 4);
  return out;
}

let settled = false;
function finish(ok, msg) {
  if (settled) return;
  settled = true;
  console.log(msg);
  try {
    socket.destroy();
  } catch (_) {
    // ignore
  }
  process.exit(ok ? 0 : 1);
}

const socket = net.createConnection({ host: '127.0.0.1', port }, () => {
  socket.write(encodeFrame({ op: 'status' }));
});

socket.setTimeout(10000, () => {
  finish(false, 'VERIFY_FAIL: no response from proxy within 10s (is fcade-proxy running on 127.0.0.1:' + port + '?)');
});

let buf = Buffer.alloc(0);
socket.on('data', (chunk) => {
  if (settled) return;
  buf = Buffer.concat([buf, chunk]);
  if (buf.length < 4) return;
  const len = buf.readUInt32BE(0);
  if (buf.length < 4 + len) return;

  const payload = buf.subarray(4, 4 + len);
  let resp;
  try {
    resp = JSON.parse(payload.toString('utf8'));
  } catch (err) {
    finish(false, 'VERIFY_FAIL: could not parse response JSON: ' + (err && err.message ? err.message : err));
    return;
  }

  console.log('STATUS_JSON=' + JSON.stringify(resp));

  if (!resp.ok) {
    finish(false, 'VERIFY_FAIL: status op returned ok:false');
    return;
  }
  if (resp.mode !== 'catalog') {
    finish(false, 'VERIFY_FAIL: mode is "' + resp.mode + '", expected "catalog"');
    return;
  }
  if (expectedRows !== null && resp.catalog_rows !== expectedRows) {
    finish(false, 'VERIFY_FAIL: catalog_rows is ' + resp.catalog_rows + ', expected ' + expectedRows);
    return;
  }
  if (expectedGeneratedAt !== null && resp.catalog_generated_at !== expectedGeneratedAt) {
    finish(false, 'VERIFY_FAIL: catalog_generated_at is ' + resp.catalog_generated_at + ', expected ' + expectedGeneratedAt);
    return;
  }
  finish(true, 'VERIFY_PASS');
});

socket.on('error', (err) => {
  finish(false, 'VERIFY_FAIL: connection error: ' + (err && err.message ? err.message : err));
});
EOF

# --- watch_for_download: polls DOWNLOADS_DIR for the newest
# fcade-catalog*.json whose mtime is >= START_EPOCH, then waits for its size
# to stabilize. Prints ONLY the winning path to stdout on success (all
# progress/status text goes to stderr) so callers can do
# `CATALOG_FILE=$(watch_for_download)`. Returns 1 (nothing on stdout) on
# timeout.
watch_for_download() {
  local start_epoch="$1"
  local deadline=$((start_epoch + TIMEOUT_SECS))
  local candidate="" candidate_mtime=-1 f m prev_size size

  echo "" >&2
  echo "In a logged-in fightcade.com tab, click your Fightcade Catalog" >&2
  echo "bookmarklet -- or paste tools/fcade-proxy/browser-catalog.js into the" >&2
  echo "console. Waiting up to ${TIMEOUT_SECS}s for the download in '${DOWNLOADS_DIR}'..." >&2
  echo "" >&2

  while [ "$(date +%s)" -lt "$deadline" ]; do
    candidate=""
    candidate_mtime=-1
    while IFS= read -r -d '' f; do
      m=$(stat -f %m "$f" 2>/dev/null || echo -1)
      if [ "$m" -ge "$start_epoch" ] && [ "$m" -gt "$candidate_mtime" ]; then
        candidate="$f"
        candidate_mtime="$m"
      fi
    done < <(find "$DOWNLOADS_DIR" -maxdepth 1 -type f -name 'fcade-catalog*.json' -print0 2>/dev/null)

    if [ -n "$candidate" ]; then
      prev_size=$(stat -f %z "$candidate" 2>/dev/null || echo -1)
      sleep 2
      if [ -f "$candidate" ]; then
        size=$(stat -f %z "$candidate" 2>/dev/null || echo -2)
        if [ "$prev_size" = "$size" ]; then
          echo "found: $candidate" >&2
          printf '%s\n' "$candidate"
          return 0
        fi
      fi
      continue
    fi

    sleep 2
  done

  return 1
}

# --- main flow ---------------------------------------------------------

if [ -n "$EXPLICIT_FILE" ]; then
  if [ ! -f "$EXPLICIT_FILE" ]; then
    echo "error: --file $EXPLICIT_FILE does not exist" >&2
    exit 1
  fi
  CATALOG_FILE="$EXPLICIT_FILE"
  echo "Using explicit file: $CATALOG_FILE"
else
  if [ ! -d "$DOWNLOADS_DIR" ]; then
    echo "error: --downloads dir '$DOWNLOADS_DIR' does not exist" >&2
    exit 1
  fi
  START_EPOCH="$(date +%s)"
  if ! CATALOG_FILE="$(watch_for_download "$START_EPOCH")"; then
    echo "" >&2
    echo "error: no fcade-catalog*.json appeared in '$DOWNLOADS_DIR' within ${TIMEOUT_SECS}s." >&2
    echo "  - Did you click the bookmarklet (or paste browser-catalog.js) in a" >&2
    echo "    logged-in fightcade.com browser tab?" >&2
    echo "  - Is your browser configured to download somewhere other than" >&2
    echo "    '$DOWNLOADS_DIR'? Re-run with --downloads DIR pointed at the" >&2
    echo "    right folder." >&2
    echo "  - Re-run with a longer --timeout if the browser is still paging" >&2
    echo "    through results (watch the console for [fcade-catalog] progress)." >&2
    exit 1
  fi
fi

echo ""
echo "Validating $CATALOG_FILE ..."
if ! VALIDATE_OUT="$("$NODE_BIN" "$WORKDIR/validate.js" "$CATALOG_FILE" 2>&1)"; then
  echo "error: catalog validation failed -- NOT pushing:" >&2
  printf '%s\n' "$VALIDATE_OUT" >&2
  exit 1
fi

# Take only the FIRST ROWS=/GENERATED_AT= line (validate.js prints them once,
# up front, before any SAMPLE= lines). A crafted quarkid/player name could
# contain an embedded newline followed by "ROWS=..." which sed would otherwise
# re-capture — and these values are interpolated into the remote ssh command
# below, so an un-clamped multi-line value would be a command-injection vector
# (review P-1). head -1 + a strict numeric assertion closes it: both are
# integers by construction, so anything else means tampered/hostile input.
ROWS="$(printf '%s\n' "$VALIDATE_OUT" | sed -n 's/^ROWS=//p' | head -1)"
GENERATED_AT="$(printf '%s\n' "$VALIDATE_OUT" | sed -n 's/^GENERATED_AT=//p' | head -1)"

case "$ROWS" in
  ''|*[!0-9]*)
    echo "error: internal — row count is not a plain integer ('$ROWS'); refusing to proceed" >&2
    exit 1
    ;;
esac
case "$GENERATED_AT" in
  *[!0-9]*)
    echo "error: internal — generated_at is not a plain integer ('$GENERATED_AT'); refusing to proceed" >&2
    exit 1
    ;;
esac

echo "  rows: $ROWS"
printf '%s\n' "$VALIDATE_OUT" | sed -n 's/^SAMPLE=/  sample: /p'

echo ""
echo "Pushing to $TARGET ..."
if ! "$SCRIPT_DIR/push-catalog.sh" "$CATALOG_FILE" "$TARGET"; then
  echo "error: push-catalog.sh failed" >&2
  exit 1
fi

SSH_HOST="${TARGET%%:*}"

echo ""
echo "Verifying over SSH ($SSH_HOST) that the proxy went live ..."
VERIFY_STATUS=0
VERIFY_OUT="$(ssh -o ConnectTimeout=10 -o BatchMode=yes "$SSH_HOST" "node - $ROWS ${GENERATED_AT:-}" < "$WORKDIR/verify.js" 2>&1)" || VERIFY_STATUS=$?

if [ "$VERIFY_STATUS" -eq 255 ]; then
  echo ""
  echo "Push succeeded, but the SSH verify step could not run (SSH to" >&2
  echo "'$SSH_HOST' failed/unreachable -- exit 255). This does NOT mean the" >&2
  echo "push failed. Verify manually once SSH is reachable:" >&2
  echo "  ssh $SSH_HOST 'node -e \"" >&2
  echo "    const net=require(\\\"net\\\");" >&2
  echo "    const s=net.createConnection(3479,\\\"127.0.0.1\\\",()=>{" >&2
  echo "      const j=Buffer.from(JSON.stringify({op:\\\"status\\\"}));" >&2
  echo "      const h=Buffer.alloc(4); h.writeUInt32BE(j.length,0);" >&2
  echo "      s.write(Buffer.concat([h,j]));" >&2
  echo "    });" >&2
  echo "    s.on(\\\"data\\\",d=>{console.log(d.toString(\\\"utf8\\\").slice(4));process.exit(0)});\"'" >&2
  echo "" >&2
  echo "SSH output was:" >&2
  printf '%s\n' "$VERIFY_OUT" >&2
  echo ""
  echo "PUSHED (unverified): $ROWS replays to $TARGET/catalog.json"
  exit 0
elif [ "$VERIFY_STATUS" -ne 0 ]; then
  echo "" >&2
  echo "error: push completed, but SSH verification FAILED -- the proxy does" >&2
  echo "not appear to be serving the catalog just pushed. Output:" >&2
  printf '%s\n' "$VERIFY_OUT" >&2
  exit 1
fi

STATUS_JSON="$(printf '%s\n' "$VERIFY_OUT" | sed -n 's/^STATUS_JSON=//p')"

HUMAN_DATE=""
if [ -n "${GENERATED_AT:-}" ]; then
  HUMAN_DATE="$(date -r "$((GENERATED_AT / 1000))" 2>/dev/null || echo "")"
fi

echo ""
echo "LIVE: $ROWS replays${HUMAN_DATE:+, generated $HUMAN_DATE}; the MiSTer REMOTE tab will now serve from this catalog."
echo "status: $STATUS_JSON"
