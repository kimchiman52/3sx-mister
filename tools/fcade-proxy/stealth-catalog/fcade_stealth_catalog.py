#!/usr/bin/env python3
# tools/fcade-proxy/stealth-catalog/fcade_stealth_catalog.py
#
# Autonomous stealth-browser Fightcade catalog refresh runner
# (docs/plan-fcade-stealth-catalog.md, Stages S1 + S2).
#
# WHAT THIS IS
# The one irreducible human step in the offline-catalog refresh flow is a
# person pasting tools/fcade-proxy/browser-catalog.js into a logged-in
# fightcade.com tab to pass Cloudflare's managed challenge and page the
# `searchquarks` API. This runner automates exactly that step with a real,
# VISIBLE Chrome window driven over CDP by `nodriver` -- it clears Cloudflare,
# pages the full `searchquarks` listing, and emits a byte-shape-compatible
# `catalog.json` that the existing push/verify tail
# (tools/fcade-proxy/refresh-catalog.sh / push-catalog.sh) consumes UNCHANGED.
#
# It is strictly ADDITIVE: browser-catalog.js and refresh-catalog.sh are the
# permanent, untouched manual fallback (plan §6.3). This runner never writes to
# or pushes a live catalog -- it only writes the file named by `--out`.
#
# HEADED ONLY. nodriver headless is Cloudflare-detected (bare 403 on the POST);
# a real visible Chrome window clears the challenge. Do not add a headless mode.
# On a headless host (e.g. the VPS) run it under Xvfb -- see README.md.
#
# THE CLOUDFLARE RECIPE (verified working, S0 spike)
#   1. Navigate the top-level browser to https://www.fightcade.com/api/ first.
#      That CF-protected path mints an httpOnly `cf_clearance` cookie
#      transparently (~5s). cf_clearance is httpOnly, so it is visible only via
#      CDP `browser.cookies.get_all()`, never document.cookie.
#   2. Navigate to the homepage, then issue the SAME-ORIGIN `searchquarks` POST
#      from page JS (so CF sees a normal in-page browser XHR, not a script).
#   No Fightcade login/account is needed (OQ-1 resolved by S0) -- only
#   cf_clearance.
#
# ROW SHAPE / NORMALIZATION -- SINGLE JS SOURCE
# The per-page fetch and the normalizePlayer()/normalizeRow() coercion are
# executed IN THE PAGE as the exact same JavaScript logic browser-catalog.js
# uses (see PAGE_FETCH_JS below -- copied verbatim from
# browser-catalog.js:73-149, marked as such). Running the identical JS in the
# identical JS engine -- rather than re-implementing type coercion in Python --
# guarantees the emitted rows match the human snippet's output field-for-field.
# The only logic kept in Python is the paging loop, the Recent/Best merge +
# quarkid de-dup + catalog_best tag (browser-catalog.js:181-210, pure map ops,
# no coercion), the politeness delay, and the Cloudflare re-solve/retry --
# precisely the parts that need to drive the browser from outside the page.
#
# There are now THREE hand-synced copies of the normalize logic
# (fcade-proxy.js:120-144, browser-catalog.js:73-105, and PAGE_FETCH_JS here).
# This mirrors the existing snippet<->proxy hand-sync convention; if
# normalizeRow() ever changes, update all three.

import argparse
import asyncio
import json
import os
import sys
import time
from datetime import datetime, timezone

# --- Constants mirrored from browser-catalog.js -----------------------------
GAMEID_DEFAULT = "sfiii3nr1"
MAX_ROWS_DEFAULT = 150       # browser-catalog.js:55
PAGE_SIZE = 15               # browser-catalog.js:56
REQUEST_DELAY_MS = 400       # browser-catalog.js:57 (politeness)

# The exact UA that cleared Cloudflare in the S0 spike: the stock
# Chrome-for-Testing 151 user agent (nodriver does not override it). We do NOT
# force a UA via --user-agent (a UA that disagrees with the browser's real
# sec-ch-ua client hints raises detection risk); instead we RECORD the live UA
# and warn if it drifts from this pinned expectation.
PINNED_UA_SUBSTR = "Chrome/151"

# --- Structured exit codes ---------------------------------------------------
EXIT_OK = 0
EXIT_CF_NOT_CLEARED = 2     # cf_clearance never minted / POST kept returning 403
EXIT_POST_FAILED = 3        # POST returned a non-200, non-403 status
EXIT_NO_ROWS = 4            # cleared + 200 but zero rows came back
EXIT_SETUP = 5              # launch / chrome / import failure

try:
    import nodriver as uc
except Exception as e:  # pragma: no cover - import guard
    sys.stderr.write(
        "FATAL: could not import nodriver (%r).\n"
        "Install it into a venv (see README.md): pip install -r requirements.txt\n" % e
    )
    sys.exit(EXIT_SETUP)


def log(msg):
    print("[stealth-catalog] " + msg, flush=True)


# --- PAGE_FETCH_JS: fetch + normalize, run IN THE PAGE -----------------------
# normalizePlayer() and normalizeRow() below are copied VERBATIM from
# browser-catalog.js:73-105 (which itself mirrors fcade-proxy.js:120-144). Keep
# all three in sync by hand. The fetch mirrors browser-catalog.js:112-149's
# fetchPage(), but returns NORMALIZED rows (map through normalizeRow, as the
# snippet does at :195-206) plus the HTTP status, so the Python driver can
# detect a Cloudflare 403 between pages and re-solve.
#
# Placeholders __OFFSET__ / __BEST__ / __SINCE__ / __GAMEID__ / __LIMIT__ are
# substituted with JSON literals before evaluate().
PAGE_FETCH_JS = r"""
(async () => {
  // --- VERBATIM from browser-catalog.js:73-105 (keep in sync) ---
  function normalizePlayer(p) {
    if (!p || typeof p !== 'object') return null;
    return {
      name: typeof p.name === 'string' ? p.name : '',
      country: typeof p.country === 'string' ? p.country : null,
      rank: typeof p.rank === 'string' || typeof p.rank === 'number' ? p.rank : null,
      score: typeof p.score === 'number' ? p.score : null,
    };
  }
  function normalizeRow(row, fallbackGameid, catalogBest) {
    if (!row || typeof row !== 'object') return null;
    if (typeof row.quarkid !== 'string' && typeof row.quarkid !== 'number') return null;
    var players = Array.isArray(row.players)
      ? row.players.map(normalizePlayer).filter(function (p) { return p !== null; })
      : [];
    return {
      quarkid: String(row.quarkid),
      date: typeof row.date === 'number' ? row.date : null,
      duration: typeof row.duration === 'number' ? row.duration : null,
      players: players,
      ranked: typeof row.ranked === 'boolean' ? row.ranked : null,
      num_matches: typeof row.num_matches === 'number' ? row.num_matches : null,
      emulator: typeof row.emulator === 'string' ? row.emulator : null,
      gameid: typeof row.gameid === 'string' ? row.gameid : fallbackGameid,
      catalog_best: catalogBest === true,
    };
  }
  // --- fetch (mirror of browser-catalog.js:112-149's fetchPage) ---
  var payload = { req: 'searchquarks', gameid: __GAMEID__, offset: __OFFSET__, limit: __LIMIT__ };
  if (__BEST__) payload.best = true;
  if (__SINCE__ !== null) payload.since = __SINCE__;
  var resp;
  try {
    resp = await fetch('/api/', {
      method: 'POST',
      credentials: 'same-origin',
      headers: {
        Accept: 'application/json, text/plain, */*',
        'Content-Type': 'application/json;charset=UTF-8',
      },
      body: JSON.stringify(payload),
    });
  } catch (err) {
    return JSON.stringify({ status: -1, error: String(err) });
  }
  if (!resp.ok) {
    var head = '';
    try { head = (await resp.text()).slice(0, 200); } catch (e) {}
    return JSON.stringify({ status: resp.status, bodyHead: head });
  }
  var body;
  try { body = await resp.json(); }
  catch (err) { return JSON.stringify({ status: resp.status, error: 'json: ' + String(err) }); }
  var raw = body && body.results && Array.isArray(body.results.results) ? body.results.results : null;
  if (raw === null) return JSON.stringify({ status: resp.status, error: 'unexpected response shape' });
  var rows = raw.map(function (r) { return normalizeRow(r, __GAMEID__, __BEST__); })
                .filter(function (r) { return r !== null; });
  return JSON.stringify({ status: resp.status, rawLen: raw.length, rows: rows });
})()
"""


def build_page_js(gameid, offset, limit, best, since):
    return (
        PAGE_FETCH_JS
        .replace("__GAMEID__", json.dumps(gameid))
        .replace("__OFFSET__", json.dumps(offset))
        .replace("__LIMIT__", json.dumps(limit))
        .replace("__BEST__", "true" if best else "false")
        .replace("__SINCE__", json.dumps(since) if since is not None else "null")
    )


def month_start_utc_ms():
    """Start of the current UTC month in ms-epoch -- matches
    browser-catalog.js:181-182's Date.UTC(year, month, 1)."""
    now = datetime.now(timezone.utc)
    start = datetime(now.year, now.month, 1, tzinfo=timezone.utc)
    return int(start.timestamp() * 1000)


async def has_cf_clearance(browser):
    try:
        cookies = await browser.cookies.get_all()
    except Exception:
        return False
    return any(getattr(c, "name", None) == "cf_clearance" for c in cookies)


async def ensure_cf(browser, timeout_s):
    """The verified CF recipe: navigate to /api/ to mint httpOnly cf_clearance,
    poll for it, then land on the homepage and return that page handle. Returns
    (page, cleared_bool)."""
    log("clearing Cloudflare: navigating to /api/ to mint cf_clearance ...")
    try:
        await asyncio.wait_for(
            browser.get("https://www.fightcade.com/api/"), timeout=timeout_s
        )
    except Exception as e:
        log("  navigate to /api/ failed: %r" % e)
    cleared = False
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        await asyncio.sleep(5)
        if await has_cf_clearance(browser):
            cleared = True
            break
    log("  cf_clearance minted? %s" % cleared)
    log("landing on homepage ...")
    page = await asyncio.wait_for(
        browser.get("https://www.fightcade.com"), timeout=timeout_s
    )
    await asyncio.sleep(4)
    return page, cleared


async def record_ua(page):
    try:
        ua = await page.evaluate("navigator.userAgent", await_promise=False)
    except Exception:
        ua = None
    if isinstance(ua, str):
        log("live UA: %s" % ua)
        if PINNED_UA_SUBSTR not in ua:
            log("  WARNING: UA does not contain pinned '%s' -- CF behavior may "
                "differ from the S0-verified environment." % PINNED_UA_SUBSTR)
    return ua


async def fetch_one_page(page, gameid, offset, limit, best, since, timeout_s):
    """Evaluate the in-page fetch+normalize for one page. Returns the parsed
    dict {status, rows?, rawLen?, bodyHead?, error?}."""
    js = build_page_js(gameid, offset, limit, best, since)
    raw = await asyncio.wait_for(page.evaluate(js, await_promise=True), timeout=timeout_s)
    if not isinstance(raw, str):
        # nodriver occasionally returns already-decoded values; be tolerant.
        return raw if isinstance(raw, dict) else {"status": -1, "error": "non-str eval result: %r" % (raw,)}
    return json.loads(raw)


async def collect_feed(state, gameid, best, since, max_rows, timeout_s):
    """Page one feed (Recent or Best), mirroring browser-catalog.js:151-177's
    collectPages: stop on empty/short page, MAX_ROWS, or fetch failure. On a
    403 (Cloudflare re-challenge mid-run), re-solve CF once and retry the same
    offset before giving up on the feed.

    Returns (rows, clean). `clean` is True ONLY when the feed terminated
    naturally -- an empty page, a short page, or the MAX_ROWS cap -- i.e. we
    reached the true end of the data. It is False on ANY dirty termination:
    an eval exception/timeout, a non-200 status, or a 403 that survived the one
    re-solve. A dirty feed means `rows` is TRUNCATED, not complete, and the
    caller MUST refuse to emit a catalog from it -- otherwise a partial pull
    would blank the live catalog when pushed (the plan's data-fix regression)."""
    label = "best" if best else "recent"
    collected = []
    raw_collected = 0  # raw rows seen; browser-catalog.js:155 caps on this, not the normalized len
    offset = 0
    resolved_once = False
    while True:
        if raw_collected >= max_rows:
            log("  %s: hit MAX_ROWS (%d), stopping" % (label, max_rows))
            break
        try:
            res = await fetch_one_page(
                state["page"], gameid, offset, PAGE_SIZE, best, since, timeout_s)
        except Exception as e:
            log("  %s: page eval failed at offset=%d: %r -- stopping feed (DIRTY)" % (label, offset, e))
            return collected, False
        status = res.get("status")
        rows = res.get("rows")

        if status == 403:
            if not resolved_once:
                log("  %s: HTTP 403 at offset=%d -- Cloudflare re-challenge; re-solving once ..."
                    % (label, offset))
                page, cleared = await ensure_cf(state["browser"], timeout_s)
                state["page"] = page
                resolved_once = True
                if cleared:
                    await asyncio.sleep(REQUEST_DELAY_MS / 1000.0)
                    continue  # retry same offset
            log("  %s: still 403 after re-solve (offset=%d) -- stopping feed early (DIRTY)" % (label, offset))
            state["hit_403"] = True
            return collected, False

        if status != 200 or not isinstance(rows, list):
            log("  %s: unexpected result at offset=%d: status=%s err=%s -- stopping feed (DIRTY)"
                % (label, offset, status, res.get("error") or res.get("bodyHead")))
            return collected, False

        raw_len = res.get("rawLen", len(rows))
        collected.extend(rows)
        raw_collected += raw_len
        if raw_len == 0:
            log("  %s: empty page at offset=%d -- stopping feed" % (label, offset))
            break
        if raw_len < PAGE_SIZE:
            log("  %s: short page (%d < %d) at offset=%d -- stopping feed"
                % (label, raw_len, PAGE_SIZE, offset))
            break
        offset += PAGE_SIZE
        await asyncio.sleep(REQUEST_DELAY_MS / 1000.0)
    log("  %s: collected %d normalized rows (clean)" % (label, len(collected)))
    return collected, True


async def run_full(browser, gameid, max_rows, timeout_s):
    """S2: full Recent + Best-this-month paging -> merged/deduped rows in the
    exact browser-catalog.js order + catalog_best semantics."""
    page, cleared = await ensure_cf(browser, timeout_s)
    await record_ua(page)
    state = {"browser": browser, "page": page, "hit_403": False}

    if not cleared:
        # ensure_cf couldn't confirm the cookie; try one full re-solve before
        # committing to a crawl that would 403 on page 1.
        log("cf_clearance not confirmed on first solve -- retrying the CF recipe once ...")
        page, cleared = await ensure_cf(browser, timeout_s)
        state["page"] = page

    log("fetching recent (best=false) ...")
    recent, recent_clean = await collect_feed(state, gameid, False, None, max_rows, timeout_s)
    await asyncio.sleep(REQUEST_DELAY_MS / 1000.0)

    since = month_start_utc_ms()
    log("fetching best (best=true, since=%d = UTC month start) ..." % since)
    best_rows, best_clean = await collect_feed(state, gameid, True, since, max_rows, timeout_s)

    # Merge + de-dup by quarkid, verbatim semantics of browser-catalog.js:194-210:
    # recent first (catalog_best=false), then best (catalog_best=true); a quark
    # in both keeps catalog_best=true. dict preserves insertion order.
    by_id = {}
    for r in recent:
        by_id[r["quarkid"]] = r
    for b in best_rows:
        existing = by_id.get(b["quarkid"])
        if existing is not None:
            existing["catalog_best"] = True
        else:
            by_id[b["quarkid"]] = b

    rows = list(by_id.values())
    clean = recent_clean and best_clean
    return rows, clean, state["hit_403"], (len(recent) + len(best_rows) > 0)


async def run_single(browser, gameid, timeout_s):
    """S1: clear CF, fetch exactly one page (offset 0, best=false), assert
    200 + >=1 row, with one retry+backoff on a 403 / challenge-not-cleared."""
    for attempt in (1, 2):
        page, cleared = await ensure_cf(browser, timeout_s)
        await record_ua(page)
        try:
            res = await fetch_one_page(page, gameid, 0, PAGE_SIZE, False, None, timeout_s)
        except Exception as e:
            log("attempt %d: page eval failed: %r" % (attempt, e))
            res = {"status": -1, "error": str(e)}
        status = res.get("status")
        rows = res.get("rows")
        log("attempt %d: POST /api/ searchquarks status=%s" % (attempt, status))
        if status == 200 and isinstance(rows, list) and len(rows) >= 1:
            log("attempt %d: OK, %d rows (rawLen=%s)" % (attempt, len(rows), res.get("rawLen")))
            for r in rows[:3]:
                log("  quarkid=%s players=%s date=%s"
                    % (r["quarkid"], [p["name"] for p in r["players"]], r["date"]))
            return EXIT_OK, rows
        if status == 200 and isinstance(rows, list):
            log("attempt %d: cleared + 200 but 0 rows" % attempt)
            if attempt == 2:
                return EXIT_NO_ROWS, None
        elif status == 403 or not cleared:
            log("attempt %d: 403 / CF-not-cleared (bodyHead=%r)" % (attempt, res.get("bodyHead")))
            if attempt == 2:
                return EXIT_CF_NOT_CLEARED, None
        else:
            log("attempt %d: POST failed status=%s err=%s"
                % (attempt, status, res.get("error") or res.get("bodyHead")))
            if attempt == 2:
                return EXIT_POST_FAILED, None
        backoff = 8 * attempt
        log("backing off %ds before retry ..." % backoff)
        await asyncio.sleep(backoff)
    return EXIT_POST_FAILED, None


def build_browser_args(args):
    """Assemble extra Chrome CLI args for window placement. Returns [] when
    neither --offscreen nor an explicit --window-position/--window-size was
    given, so the default (unchanged) launch behavior is preserved exactly.

    --offscreen renders a REAL, headed window (Cloudflare needs a real render;
    headless is CF-detected) but parks it far off any physical display so the
    daily LaunchAgent run does not steal focus or flash a visible window
    mid-screen. It is a convenience shorthand for
    --window-position=-32000,-32000 --window-size=1200,900; either component
    can still be overridden explicitly.
    """
    pos = args.window_position
    size = args.window_size
    if args.offscreen:
        pos = pos or "-32000,-32000"
        size = size or "1200,900"
    extra = []
    if pos:
        extra.append("--window-position=%s" % pos)
    if size:
        extra.append("--window-size=%s" % size)
    return extra


def resolve_chrome(explicit):
    """Locate a real Chrome binary. Priority: --chrome, $FCADE_CHROME, a
    Chrome-for-Testing under a sibling scratchpad, then common system installs.
    nodriver does NOT auto-download Chrome, so this must resolve to a real
    binary or we exit EXIT_SETUP."""
    candidates = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("FCADE_CHROME")
    if env:
        candidates.append(env)
    candidates += [
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing",
        "/usr/bin/google-chrome",
        "/usr/bin/chromium",
        "/usr/bin/chromium-browser",
    ]
    for c in candidates:
        if c and os.path.exists(c):
            return c
    return None


async def amain(args):
    chrome = resolve_chrome(args.chrome)
    if not chrome:
        log("FATAL: could not locate a Chrome binary. Pass --chrome PATH or set "
            "$FCADE_CHROME (nodriver does NOT auto-download Chrome).")
        return EXIT_SETUP, None
    log("chrome: %s" % chrome)
    log("mode: %s  gameid=%s  headed=True" % ("single-page" if args.single_page else "full", args.gameid))

    profile = args.profile or os.path.join(
        os.environ.get("TMPDIR", "/tmp"), "fcade-stealth-profile")

    extra_args = build_browser_args(args)
    if extra_args:
        log("window args: %s" % " ".join(extra_args))

    try:
        browser = await uc.start(
            browser_executable_path=chrome,
            headless=False,          # HEADED ONLY -- headless is CF-detected.
            user_data_dir=profile,
            browser_args=extra_args or None,
        )
    except Exception as e:
        log("FATAL: browser launch failed: %r" % e)
        return EXIT_SETUP, None

    try:
        if args.single_page:
            code, _rows = await run_single(browser, args.gameid, args.timeout)
            return code, None
        rows, clean, hit_403, got_any = await run_full(browser, args.gameid, args.max_rows, args.timeout)
        if not got_any:
            log("no rows collected from either feed")
            # Distinguish CF failure from a genuinely empty result.
            return (EXIT_CF_NOT_CLEARED if hit_403 else EXIT_NO_ROWS), None
        if not clean:
            # A feed terminated dirty (timeout / non-200 / persistent-403), so
            # `rows` is a TRUNCATED slice of the real listing. Refuse to emit it:
            # writing a short catalog here would, once pushed, shrink the live
            # browser to whatever partial count we happened to reach. Fail loudly
            # and write nothing so the last-good catalog is preserved.
            log("crawl ended DIRTY with only %d partial rows -- refusing to emit a "
                "truncated catalog (last-good preserved)" % len(rows))
            return (EXIT_CF_NOT_CLEARED if hit_403 else EXIT_POST_FAILED), None
        catalog = {
            "generated_at": int(time.time() * 1000),
            "gameid": args.gameid,
            "rows": rows,
        }
        return EXIT_OK, catalog
    finally:
        try:
            browser.stop()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser(
        description="Stealth-browser Fightcade catalog refresh runner (plan S1+S2).")
    ap.add_argument("--out", default=None,
                    help="Path to write catalog.json (full mode). "
                         "Default: $TMPDIR/fcade-stealth-catalog.json")
    ap.add_argument("--single-page", action="store_true",
                    help="S1: clear CF + fetch ONE page, assert 200 + >=1 row; "
                         "write no file. Exit code is the pass/fail signal.")
    ap.add_argument("--gameid", default=GAMEID_DEFAULT)
    ap.add_argument("--max-rows", type=int, default=MAX_ROWS_DEFAULT,
                    help="Per-feed row cap (browser-catalog.js MAX_ROWS, default 150).")
    ap.add_argument("--min-rows", type=int, default=0,
                    help="Push-safety floor: refuse to write a catalog with fewer "
                         "than N merged rows (guards S4's daily push against a "
                         "suspiciously small pull). 0 = disabled (default).")
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="Per-navigation / per-page-eval timeout (seconds).")
    ap.add_argument("--chrome", default=None,
                    help="Path to a real Chrome binary (nodriver does not download one).")
    ap.add_argument("--profile", default=None,
                    help="Chrome user-data-dir (persisted profile). "
                         "Default: $TMPDIR/fcade-stealth-profile")
    ap.add_argument("--offscreen", action="store_true",
                    help="Render the (still headed) Chrome window far off-screen "
                         "so an unattended/daily run does not steal focus or flash "
                         "a visible window. Shorthand for "
                         "--window-position=-32000,-32000 --window-size=1200,900. "
                         "OFF by default (behavior unchanged).")
    ap.add_argument("--window-position", default=None,
                    help="Explicit Chrome --window-position 'X,Y' (e.g. -32000,-32000). "
                         "Overrides the --offscreen default position.")
    ap.add_argument("--window-size", default=None,
                    help="Explicit Chrome --window-size 'W,H' (e.g. 1200,900). "
                         "Overrides the --offscreen default size.")
    args = ap.parse_args()

    out = args.out or os.path.join(
        os.environ.get("TMPDIR", "/tmp"), "fcade-stealth-catalog.json")

    code, catalog = uc.loop().run_until_complete(amain(args))

    if code == EXIT_OK and catalog is not None:
        n = len(catalog["rows"])
        if args.min_rows and n < args.min_rows:
            log("catalog has %d rows < --min-rows %d -- refusing to write "
                "(preserving last-good)" % (n, args.min_rows))
            log("VERDICT: FAIL(%d)" % EXIT_NO_ROWS)
            sys.exit(EXIT_NO_ROWS)
        tmp = out + ".tmp"
        with open(tmp, "w") as f:
            json.dump(catalog, f, indent=2)
        os.replace(tmp, out)  # atomic: never leave a half-written catalog
        log("WROTE %d rows -> %s (generated_at=%d)"
            % (len(catalog["rows"]), out, catalog["generated_at"]))

    verdict = "PASS" if code == EXIT_OK else "FAIL(%d)" % code
    log("VERDICT: %s" % verdict)
    sys.exit(code)


if __name__ == "__main__":
    main()
