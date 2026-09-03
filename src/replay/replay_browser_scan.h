#ifndef REPLAY_REPLAY_BROWSER_SCAN_H
#define REPLAY_REPLAY_BROWSER_SCAN_H

/* INTENTIONALLY CALLER-LESS (game side) AS OF THE REPLAY DESCOPE. The
 * in-game browser (src/replay/replay_browser.c) was the only caller of
 * RbScan/RbSort/RbFormatRow and the RbClampPage/RbPageStart/RbPageCount/
 * RbPageRows pagination helpers, and it has been deleted. The shuffle viewer
 * that replaces it enumerates the cached set through RbScan/RbSort. Keep
 * these exported and non-static. (The HPS OSD wrapper does NOT link this
 * file — it carries its own de-SDL'd port, vendor/Main_MiSTer/replay_scan.c.)
 *
 * Step F2a of docs/plan-fcade-replay-browser.md — pure scan back-end.
 *
 * The dir-scan / meta-parse / sort / pagination / row-format logic for the
 * in-game replay browser, deliberately factored out of the game-facing
 * replay_browser.c so it can be exercised by a standalone headless unit
 * driver (no engine / no SSPutStrProP dependency). The only externals here
 * are SDL3 (filesystem + string helpers) and cJSON (sidecar parse) — the
 * same two the C1/C2 player back-end already leans on.
 *
 * Nothing in this translation unit touches engine globals, so it links
 * cleanly into both the game binary and the tiny scratchpad ASan driver. */

#include <SDL3/SDL.h>

#include <stdbool.h>

/* Row width the list renders at. SSPutStrProP draws the SS glyph table at a
 * proportional ~8px/char pitch on the 384x224 canvas (sc_sub.c:635-657); a
 * left-aligned row starting near x=16 with a right margin fits comfortably
 * under ~44 glyphs. 40 keeps a safety margin and matches the C2 overlay's
 * "keep it short" convention. RbFormatRow truncates composed rows to this
 * bound (NUL included), so a row string is always <= RB_ROW_MAX_CHARS bytes
 * including the terminator. */
#define RB_ROW_MAX_CHARS 40

/* Per-field caps. Names are the Fightcade handles from the sidecar (already
 * sanitized to printable ASCII); label is the .3sr basename fallback. */
#define RB_NAME_MAX 48
#define RB_LABEL_MAX 96
#define RB_PATH_MAX 1024
#define RB_DATE_MAX 16 /* "YYYY-MM-DD\0" */

/* Hard cap on entries a single scan will collect; beyond this the scan stops
 * and logs, rather than growing unbounded.
 *
 * SIZED AGAINST RS_SET_MAX (vendor/Main_MiSTer/replay_sync.c) — CHANGE BOTH.
 * The wrapper downloads whole Fightcade *quarks*, and one quark is a whole
 * session between two players: measured over 15 real quarks it holds 5.7
 * games on average (median 5). So the two caps are one arithmetic:
 *
 *     RB_MAX_ENTRIES / 5.7 games-per-quark = quarks the game can enumerate
 *     512 / 5.7 = 89.8  ->  RS_SET_MAX = 90
 *
 * Raising this without raising RS_SET_MAX just leaves the array empty;
 * raising RS_SET_MAX without raising this makes the wrapper download quarks
 * the scan will never reach. An RbEntry is ~1.2 KB, so 512 costs ~600 KB of
 * .bss — nothing on the DE10-Nano's 1 GB (the headroom fight on this project
 * is CPU, not memory). */
#define RB_MAX_ENTRIES 512

typedef struct RbEntry {
    char path[RB_PATH_MAX];   /* full path to the .3sr file, OR (needs_conversion) the fetch dir */
    char label[RB_LABEL_MAX]; /* basename with dir prefix + ".3sr" stripped; the quarkid for fetch dirs */
    char p1[RB_NAME_MAX];     /* players[0].name, sanitized; "" if absent */
    char p2[RB_NAME_MAX];     /* players[1].name, sanitized; "" if absent */
    char date[RB_DATE_MAX];   /* "YYYY-MM-DD" or "" */
    long long date_ms;        /* sidecar ms-epoch date; 0 when absent */
    long long duration_secs;  /* sidecar duration in seconds; 0 when absent */
    bool have_names;          /* both p1 and p2 present */
    bool have_date;           /* date_ms > 0 and formatted */
    /* Step F2b: a raw Fightcade-fetch directory (inputs + savestate, no `.3sr`
     * yet). `path` is the directory, not a file; the row is display-only and
     * NOT selectable for playback — it must be converted off-device
     * (tools/replay_preprocessor.py + make_3sr) before it lists as a real
     * `.3sr`. RbScan recognizes these so a just-downloaded replay shows up in
     * the local list marked "NEEDS CONVERSION". */
    bool needs_conversion;
} RbEntry;

/* True iff `dir` is a raw Fightcade-fetch directory awaiting off-device
 * conversion: it contains both an `inputs` file and a `savestate` file
 * (fcade_stream.c's outputs) but no `*.3sr` yet. Exposed for the headless
 * unit driver. */
bool RbDirNeedsConversion(const char* dir);

/* Scan `root` for `*.3sr` files, flat plus one level of subdirectories, and
 * fill `entries[0..return)` (capped at `max`, itself capped at
 * RB_MAX_ENTRIES). Each entry's sidecar `<name>.meta.json` is parsed
 * best-effort for players[0/1].name + date; a missing or corrupt sidecar
 * leaves have_names/have_date false and the row falls back to the label.
 * Returns the number of entries found (0 for an empty/absent dir). Does not
 * sort — call RbSort next. */
int RbScan(const char* root, RbEntry* entries, int max);

/* Sort in place: newest sidecar date first; undated entries after all dated
 * ones, ordered alphabetically (ascending, case-sensitive) by label. Stable
 * enough for the browser's needs (qsort with a total-order comparator). */
void RbSort(RbEntry* entries, int count);

/* Pagination helpers over `count` entries at `page_size` rows/page. */
int RbPageCount(int count, int page_size);         /* >= 1 even when count==0 */
int RbClampPage(int page, int count, int page_size); /* clamp to [0, pages-1] */
int RbPageStart(int page, int page_size);          /* first entry index of page */
int RbPageRows(int page, int count, int page_size); /* rows visible on page */

/* Compose the display row for `e` into `out` (truncated to fit
 * RB_ROW_MAX_CHARS including the NUL). Format:
 *   "P1 vs P2  YYYY-MM-DD"  when names present (date appended if known)
 *   "LABEL  YYYY-MM-DD"     otherwise (date appended if known)
 *   "LABEL  NEEDS CONVERSION" for a needs_conversion fetch dir
 * Never writes past out_sz; always NUL-terminates. */
void RbFormatRow(const RbEntry* e, char* out, size_t out_sz);

/* Format an ms-epoch timestamp as "YYYY-MM-DD" into `out` (portable
 * civil-from-days, no locale). Exposed so the browser can format remote
 * (proxy) row dates with the exact same helper local rows use. */
void RbFormatDate(long long ms, char* out, size_t out_sz);

#endif /* REPLAY_REPLAY_BROWSER_SCAN_H */
