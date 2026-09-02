// tools/fcade-proxy/browser-catalog.js
//
// Offline-catalog snippet ("option B" fallback, plan §4.5 F1 fallback).
// Run this INSIDE A REAL BROWSER TAB on fightcade.com -- not in Node, not
// with curl, not with Playwright. Do not try to automate this file.
//
// WHY THIS EXISTS
// Fightcade's /api/ endpoint sits behind a Cloudflare MANAGED challenge
// ("Just a moment...", __cf_chl). This was confirmed unbeatable by: plain
// node/curl with any headers, a real Chrome TLS fingerprint (curl_cffi),
// AND Playwright (both headless and headed -- automation is detected
// either way). The ONLY thing that reliably passes is a real, everyday
// browser tab that is already logged into fightcade.com. This script runs
// there and uses the tab's own same-origin fetch(), so Cloudflare sees a
// normal browser request, not a script. It produces a static catalog.json
// that the fcade-proxy VPS service can then serve `search` from with ZERO
// live upstream calls (see fcade-proxy.js's FCADE_CATALOG_FILE handling
// and README.md's "Offline catalog (option B)" section).
//
// HOW TO RUN
//   1. Open https://www.fightcade.com in your normal browser, logged in.
//   2. Open DevTools (Cmd+Opt+I on Mac, F12 elsewhere) -> Console tab.
//   3. Paste this entire file's contents into the console and press Enter.
//      (Or use the pre-built one-line form in
//      browser-catalog.bookmarklet.txt as a bookmarklet -- rebuild it with
//      `node build-bookmarklet.js` after editing this file.)
//   4. Watch the console for progress (`[fcade-catalog] ...` lines) while
//      it pages through the Best results.
//   5. When done, it downloads fcade-catalog.json AND copies the same JSON
//      to your clipboard, plus logs "COPIED N rows...". Either hand the
//      downloaded file (or pasted clipboard contents) to your assistant,
//      or feed it directly to push-catalog.sh:
//        ./push-catalog.sh ~/Downloads/fcade-catalog.json hetzner-3s-arm:/opt/fcade-proxy
//
// WHAT IT FETCHES (edit the constants below to adjust)
//   - "Best this week" ONLY: best:true, since = today's UTC midnight minus
//     7 days -- byte-for-byte the window fightcade.com's own WEEKLY BEST tab
//     uses (its `weeklyBest` computed is `e=Date.now(); e-e%864e5-6048e5`).
//     offset 0, 15, 30, ... up to MAX_ROWS rows, for GAMEID.
//   There is NO Recent (best:false) pass any more: the device plays the
//   weekly-best set and nothing else, so a fresh-feed crawl only added rows
//   that were then filtered out server-side. Dropping it is safe because the
//   two crawls were always independent paged fetches -- the Best call never
//   took any input from the Recent result -- so the best-tagged set this
//   emits is unchanged.
//
//   CAVEAT PRESERVED FROM THE OLD MERGE: when a quarkid appeared in BOTH
//   feeds, the merge that used to live below kept the RECENT copy's values
//   and only flipped its `catalog_best` flag to true. Measured overlap in a
//   real capture was zero, but it was never structurally zero, so rows that
//   used to come from the Recent copy now come from the Best copy instead.
//   Same quarkid, same normalize function, possibly different field values
//   if upstream ever reported a row differently between the two feeds.
//
//   All three Best tabs on fightcade.com POST the SAME `searchquarks`
//   request with `best: true` and differ ONLY in `since` -- weekly is not a
//   separate feed, it is this one value.
//
// ROW SHAPE
// Every row is normalized to mirror fcade-proxy.js's normalizeRow() /
// normalizePlayer() EXACTLY (tools/fcade-proxy/fcade-proxy.js:120-144) --
// same key set, same coercion rules -- plus one extra bookkeeping field,
// `catalog_best`, that the proxy strips (by re-running every catalog row
// through its own normalizeRow()) before any response ever reaches the
// device. There is no shared module between this browser snippet and the
// Node service, so keep the two normalize functions in sync by hand if
// normalizeRow() in fcade-proxy.js ever changes.

(async function fcadeCatalogSnippet() {
  'use strict';

  const GAMEID = 'sfiii3nr1';
  const MAX_ROWS = 150;
  const PAGE_SIZE = 15;
  const REQUEST_DELAY_MS = 400; // politeness: pause between page fetches

  function log(msg) {
    console.log('[fcade-catalog] ' + msg);
  }

  function sleep(ms) {
    return new Promise(function (resolve) {
      setTimeout(resolve, ms);
    });
  }

  // --- normalizeRow/normalizePlayer: mirror of fcade-proxy.js's
  // normalizeRow()/normalizePlayer(). Keep in sync by hand -- see file
  // header above.

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
      ? row.players.map(normalizePlayer).filter(function (p) {
          return p !== null;
        })
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
      // Catalog-only bookkeeping field, stripped by the proxy's own
      // normalizeRow() before any device ever sees it (see
      // fcade-proxy.js's searchCatalog()).
      catalog_best: catalogBest === true,
    };
  }

  // --- Same-origin fetch of the exact request shape fcade_replay_tool.py's
  // search_quarks() sends (tools/fcade-replays/fcade_replay_tool.py:117-152)
  // -- but via the browser tab's own fetch, so it carries this tab's
  // cookies/TLS/JS environment and passes Cloudflare's managed challenge.

  async function fetchPage(offset, limit, best, since) {
    var payload = { req: 'searchquarks', gameid: GAMEID, offset: offset, limit: limit };
    if (best) payload.best = true;
    if (since !== undefined && since !== null) payload.since = since;

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
      log('page fetch threw (offset=' + offset + ' best=' + best + '): ' + (err && err.message ? err.message : err));
      return null;
    }
    if (!resp.ok) {
      log('page fetch failed: HTTP ' + resp.status + ' (offset=' + offset + ' best=' + best + ')');
      return null;
    }
    var body;
    try {
      body = await resp.json();
    } catch (err) {
      log('page fetch: could not parse JSON (offset=' + offset + ' best=' + best + '): ' + (err && err.message ? err.message : err));
      return null;
    }
    var rows = body && body.results && Array.isArray(body.results.results) ? body.results.results : null;
    if (rows === null) {
      log('page fetch: unexpected response shape (offset=' + offset + ' best=' + best + ')');
      return null;
    }
    return rows;
  }

  async function collectPages(best, since) {
    var collected = [];
    var offset = 0;
    for (;;) {
      if (collected.length >= MAX_ROWS) {
        log('stopping ' + (best ? 'best' : 'recent') + ' paging: hit MAX_ROWS (' + MAX_ROWS + ')');
        break;
      }
      var rawRows = await fetchPage(offset, PAGE_SIZE, best, since);
      if (rawRows === null) {
        log('stopping ' + (best ? 'best' : 'recent') + ' paging early at offset=' + offset + ' due to fetch failure');
        break;
      }
      if (rawRows.length === 0) {
        log('stopping ' + (best ? 'best' : 'recent') + ' paging at offset=' + offset + ': empty page');
        break;
      }
      for (var i = 0; i < rawRows.length; i++) collected.push(rawRows[i]);
      if (rawRows.length < PAGE_SIZE) {
        log('stopping ' + (best ? 'best' : 'recent') + ' paging at offset=' + offset + ': short page (' + rawRows.length + ' < ' + PAGE_SIZE + ')');
        break;
      }
      offset += PAGE_SIZE;
      await sleep(REQUEST_DELAY_MS);
    }
    return collected;
  }

  log('starting: gameid=' + GAMEID + ' max_rows=' + MAX_ROWS);

  // WEEKLY window, verbatim from fightcade.com's own `weeklyBest` computed:
  //   weeklyBest: function(){ var e=Date.now(); return e-e%864e5-6048e5 }
  // i.e. today's UTC midnight minus 7 days. `nowMs % 864e5` is ms elapsed
  // since UTC midnight, so subtracting it floors to midnight; 6048e5 is
  // 7 * 86 400 000. Deliberately NOT Date.UTC(y, m, 1): that was a calendar
  // month, which matches NEITHER of the site's tabs (its MONTHLY BEST is a
  // rolling 30 days, `e-e%864e5-2592e6`) and collapsed to a ~1-day window on
  // the 2nd of a month.
  var nowMs = Date.now();
  var weekStart = nowMs - (nowMs % 864e5) - 6048e5;

  log('fetching best (this week, since=' + weekStart + ')...');
  var bestRaw = await collectPages(true, weekStart);
  log('best: collected ' + bestRaw.length + ' raw rows');

  // De-dup by quarkid. Still needed with one feed: the listing can shift
  // under a multi-page crawl and repeat a row across page boundaries.
  var byId = new Map();
  for (var r2 = 0; r2 < bestRaw.length; r2++) {
    var normB = normalizeRow(bestRaw[r2], GAMEID, true);
    if (!normB) continue;
    if (!byId.has(normB.quarkid)) byId.set(normB.quarkid, normB);
  }

  var rows = Array.from(byId.values());
  var catalog = { generated_at: Date.now(), gameid: GAMEID, rows: rows };
  var json = JSON.stringify(catalog, null, 2);

  // Trigger a file download.
  try {
    var blob = new Blob([json], { type: 'application/json' });
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url;
    a.download = 'fcade-catalog.json';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(function () {
      URL.revokeObjectURL(url);
    }, 5000);
  } catch (err) {
    log('download trigger failed (you can still copy the JSON below manually): ' + (err && err.message ? err.message : err));
  }

  // Copy to clipboard.
  var copied = false;
  try {
    await navigator.clipboard.writeText(json);
    copied = true;
  } catch (err) {
    log('clipboard copy failed (' + (err && err.message ? err.message : err) + '); the JSON is logged below -- copy it manually.');
    console.log(json);
  }

  log(
    'COPIED ' +
      rows.length +
      ' rows' +
      (copied ? '' : ' (clipboard copy FAILED, see JSON logged above)') +
      ' — paste to your assistant or save the downloaded fcade-catalog.json.',
  );

  return catalog;
})();
