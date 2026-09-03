#!/usr/bin/env bash
# tools/fcade-proxy/stealth-catalog/setup.sh
#
# Idempotent installer for the PERSISTENT stealth-catalog runtime that the
# daily LaunchAgent (plan-fcade-stealth-catalog.md, Stage S4) drives.
#
# WHY A PERSISTENT INSTALL ROOT (not the repo, not a scratchpad)
#  - The venv, the Chrome-for-Testing binary (~400MB) and the Chrome profile
#    (which holds the httpOnly cf_clearance cookie that must survive between
#    daily runs) are all large / stateful / machine-local and MUST NOT live in
#    git (see .gitignore here) or in an ephemeral $TMPDIR/scratchpad that gets
#    wiped. They go under a stable per-user data dir instead.
#
# This script is SAFE TO RE-RUN: every step checks for the artifact first and
# skips it if already present. It never downloads Chrome twice, never rebuilds
# an existing venv, never clobbers the profile.
#
# It does NOT install/load the LaunchAgent and does NOT run the crawler or any
# push -- it only provisions the runtime and renders the plist. (See README.md
# for the launchctl install/load steps you run yourself.)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- Config (override via env) ----------------------------------------------
# Persistent install root. Default: the standard macOS per-user data dir.
INSTALL_ROOT="${FCADE_CATALOG_HOME:-${XDG_DATA_HOME:-$HOME/Library/Application Support}/fcade-stealth-catalog}"

# Chrome for Testing pin -- the exact build verified to clear Fightcade's CF
# challenge (README.md "Verified working combo"). mac-arm64 only.
CFT_VERSION="${FCADE_CFT_VERSION:-151.0.7922.47}"
CFT_PLATFORM="${FCADE_CFT_PLATFORM:-mac-arm64}"
# Official Chrome-for-Testing known-good-versions endpoint (JSON, with download
# URLs). We resolve the exact zip URL from here rather than hardcoding a
# storage.googleapis.com path, per the plan.
KGV_URL="https://googlechromelabs.github.io/chrome-for-testing/known-good-versions-with-downloads.json"

# nodriver needs Python >= 3.10 (it uses PEP-604 `str | pathlib.Path` union
# syntax). macOS's system python3 is 3.9, so probe for a modern interpreter
# unless the caller pinned one via FCADE_PYTHON_BIN.
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

# --- Derived paths -----------------------------------------------------------
VENV_DIR="$INSTALL_ROOT/venv"
VENV_PY="$VENV_DIR/bin/python"
CHROME_DIR="$INSTALL_ROOT/chrome"
PROFILE_DIR="$INSTALL_ROOT/profile"
LOGS_DIR="$INSTALL_ROOT/logs"
STATE_DIR="$INSTALL_ROOT/state"
# Chrome-for-Testing zip extracts to chrome-<platform>/Google Chrome for Testing.app
CHROME_APP_SUBDIR="chrome-${CFT_PLATFORM}"
CHROME_BIN="$CHROME_DIR/$CHROME_APP_SUBDIR/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing"

REQUIREMENTS="$SCRIPT_DIR/requirements.txt"
RUN_DAILY="$SCRIPT_DIR/run-daily.sh"
PLIST_TEMPLATE="$SCRIPT_DIR/dev.sambae.fcade-catalog.plist"
PLIST_RENDERED="$INSTALL_ROOT/dev.sambae.fcade-catalog.plist"
DAILY_LOG="$LOGS_DIR/daily.log"

say() { printf '[setup] %s\n' "$*"; }
die() { printf '[setup] ERROR: %s\n' "$*" >&2; exit 1; }

say "install root: $INSTALL_ROOT"
mkdir -p "$INSTALL_ROOT" "$CHROME_DIR" "$PROFILE_DIR" "$LOGS_DIR" "$STATE_DIR"

# --- 1. Python venv + deps ---------------------------------------------------
venv_ok() { [ -x "$VENV_PY" ] && "$VENV_PY" -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' 2>/dev/null; }
if venv_ok; then
  say "venv already present (Python >= 3.10) -> $VENV_DIR (skipping create)"
else
  if [ -x "$VENV_PY" ]; then
    say "existing venv is Python < 3.10 ($("$VENV_PY" --version 2>&1)); recreating"
    rm -rf "$VENV_DIR"
  fi
  [ -n "$PYTHON_BIN" ] || die "no Python >= 3.10 found (nodriver needs 3.10+; macOS system python3 is 3.9). Install one (e.g. 'brew install python@3.14') or set FCADE_PYTHON_BIN."
  command -v "$PYTHON_BIN" >/dev/null 2>&1 || die "python interpreter '$PYTHON_BIN' not found (set FCADE_PYTHON_BIN)"
  "$PYTHON_BIN" -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' 2>/dev/null \
    || die "$PYTHON_BIN is $("$PYTHON_BIN" --version 2>&1) but nodriver needs Python >= 3.10"
  say "creating venv with $PYTHON_BIN ($("$PYTHON_BIN" --version 2>&1)) ..."
  "$PYTHON_BIN" -m venv "$VENV_DIR"
fi
[ -f "$REQUIREMENTS" ] || die "requirements.txt missing at $REQUIREMENTS"
say "installing/verifying pip deps (nodriver) ..."
"$VENV_PY" -m pip install --quiet --upgrade pip >/dev/null
"$VENV_PY" -m pip install --quiet -r "$REQUIREMENTS"

# nodriver 0.50.3 ships cdp/network.py with a raw latin-1 '±' (0xB1) byte and
# NO PEP-263 encoding declaration, so `import nodriver` dies with SyntaxError:
# "Non-UTF-8 code starting with '\xb1'". Re-encode any non-UTF-8 source file in
# the installed package from latin-1 to UTF-8. Idempotent: a file that already
# decodes as UTF-8 is left untouched.
say "patching nodriver source encoding (0.50.3 cdp/network.py non-UTF-8 byte) ..."
"$VENV_PY" - <<'PY'
import pathlib, sysconfig
pkg = pathlib.Path(sysconfig.get_paths()["purelib"]) / "nodriver"
patched = 0
for f in pkg.rglob("*.py"):
    data = f.read_bytes()
    try:
        data.decode("utf-8")
    except UnicodeDecodeError:
        f.write_bytes(data.decode("latin-1").encode("utf-8"))
        patched += 1
        print("  re-encoded latin-1 -> utf-8:", f.relative_to(pkg))
print("  encoding patch: %d file(s) fixed" % patched)
PY

# Prove nodriver imports on this interpreter.
if "$VENV_PY" -c "import nodriver" 2>/dev/null; then
  say "nodriver imports OK"
else
  die "nodriver failed to import in the venv -- see README.md install gotchas"
fi

# --- 2. Chrome for Testing ---------------------------------------------------
if [ -x "$CHROME_BIN" ]; then
  say "Chrome for Testing already present -> $CHROME_BIN (skipping download)"
else
  say "resolving Chrome for Testing $CFT_VERSION ($CFT_PLATFORM) from known-good-versions JSON ..."
  KGV_JSON="$(mktemp "${TMPDIR:-/tmp}/fcade-kgv.XXXXXX.json")"
  trap 'rm -f "$KGV_JSON"' EXIT
  curl -fsSL "$KGV_URL" -o "$KGV_JSON" || die "could not fetch known-good-versions JSON ($KGV_URL)"
  # Parse the exact chrome download URL for our version+platform with python
  # (always available; avoids a jq dependency).
  DL_URL="$("$PYTHON_BIN" - "$KGV_JSON" "$CFT_VERSION" "$CFT_PLATFORM" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
want_ver, want_plat = sys.argv[2], sys.argv[3]
for v in data.get("versions", []):
    if v.get("version") == want_ver:
        for d in v.get("downloads", {}).get("chrome", []):
            if d.get("platform") == want_plat:
                print(d["url"]); sys.exit(0)
sys.exit(1)
PY
)" || die "version $CFT_VERSION / platform $CFT_PLATFORM not found in known-good-versions JSON"
  say "download URL: $DL_URL"
  ZIP="$CHROME_DIR/chrome-${CFT_PLATFORM}.zip"
  say "downloading Chrome for Testing (~200-400MB) ..."
  curl -fSL "$DL_URL" -o "$ZIP" || die "download failed"
  say "unzipping into $CHROME_DIR ..."
  ( cd "$CHROME_DIR" && unzip -q -o "$ZIP" )
  rm -f "$ZIP"
  # Strip the macOS quarantine attr so a headless/agent launch is not blocked.
  xattr -dr com.apple.quarantine "$CHROME_DIR/$CHROME_APP_SUBDIR" 2>/dev/null || true
  [ -x "$CHROME_BIN" ] || die "post-extract: expected Chrome binary not found at $CHROME_BIN"
fi

say "verifying Chrome launches --version ..."
if CHROME_VER="$("$CHROME_BIN" --version 2>/dev/null)"; then
  say "chrome: $CHROME_VER"
else
  die "Chrome binary failed to run --version at $CHROME_BIN"
fi

# --- 3. Profile (persist cf_clearance across daily runs) ---------------------
# Just ensure the dir exists; nodriver populates it on first run and the
# httpOnly cf_clearance cookie then persists here between daily invocations.
say "chrome profile dir: $PROFILE_DIR (persists cf_clearance)"

# --- 4. Render the LaunchAgent plist into the install root -------------------
if [ -f "$PLIST_TEMPLATE" ]; then
  say "rendering LaunchAgent plist -> $PLIST_RENDERED"
  # Fill the __PLACEHOLDER__ tokens with resolved absolute paths.
  sed \
    -e "s#__RUN_DAILY_SH__#${RUN_DAILY}#g" \
    -e "s#__INSTALL_ROOT__#${INSTALL_ROOT}#g" \
    -e "s#__DAILY_LOG__#${DAILY_LOG}#g" \
    "$PLIST_TEMPLATE" > "$PLIST_RENDERED"
  if command -v xmllint >/dev/null 2>&1; then
    xmllint --noout "$PLIST_RENDERED" && say "rendered plist is well-formed XML"
  fi
else
  say "WARNING: plist template not found at $PLIST_TEMPLATE (skipping render)"
fi

# --- Done: print resolved paths ----------------------------------------------
cat <<EOF

[setup] DONE. Resolved runtime:
  install root : $INSTALL_ROOT
  venv python  : $VENV_PY
  chrome       : $CHROME_BIN
  profile      : $PROFILE_DIR
  logs         : $LOGS_DIR  (daily log: $DAILY_LOG)
  state        : $STATE_DIR
  run-daily.sh : $RUN_DAILY
  plist        : $PLIST_RENDERED

Next: create $INSTALL_ROOT/config.sh (see README.md) if you need to override
the VPS target / node path / row floor, then load the LaunchAgent per README.md.
EOF
