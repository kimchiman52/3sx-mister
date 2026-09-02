/*
 * replay_browser_scan.c — Step F2a of docs/plan-fcade-replay-browser.md.
 *
 * Pure back-end for the in-game replay browser: enumerate a replays root for
 * `.3sr` files (flat + one level of subdirs), parse each `<name>.meta.json`
 * sidecar for display fields, sort newest-first, and paginate/format rows.
 * No engine dependency — see replay_browser_scan.h. The game-facing screen
 * (input, SSPutStrProP rendering, launch) lives in replay_browser.c.
 *
 * The sidecar parse deliberately mirrors replay_player.c's C2 logic
 * (players[0/1].name accepting bare-string or {name:} items, ms-epoch date
 * via Howard Hinnant's civil-from-days, printable-ASCII sanitization) so a
 * file lists in the browser exactly as it later titles in the viewer.
 */

#include "replay/replay_browser_scan.h"

#include "cJSON.h"

#include <stdbool.h>

/* ---------------------------------------------------------------------- */
/* Sidecar field helpers (parity with replay_player.c C2)                 */
/* ---------------------------------------------------------------------- */

/* Copy a JSON string into a fixed buffer, replacing any byte outside the
 * printable-ASCII range (0x20-0x7E) with '?'. The SS glyph table is indexed
 * by raw byte over 128 entries, so an unsanitized high byte would index out
 * of range once this string reaches the row renderer. */
static bool copy_sanitized_name(const char* src, char* dst, size_t dst_sz) {
    if (src == NULL || src[0] == '\0') {
        dst[0] = '\0';
        return false;
    }

    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < dst_sz; i++) {
        const unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    dst[i] = '\0';
    return i > 0;
}

/* players[k] may be a bare string ("name") or an object carrying a "name"
 * field, depending on the meta.json producer — accept both. */
static const char* player_name_from_item(const cJSON* item) {
    if (item == NULL) {
        return NULL;
    }

    if (cJSON_IsString(item)) {
        return item->valuestring;
    }

    if (cJSON_IsObject(item)) {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (cJSON_IsString(name)) {
            return name->valuestring;
        }
    }

    return NULL;
}

/* Portable civil-date-from-epoch (Howard Hinnant) — no gmtime/locale
 * dependency, identical on every build profile. `ms` is the sidecar's
 * ms-since-epoch `date`. */
static void format_date_ms(long long ms, char* out, size_t out_sz) {
    long long days = ms / 86400000LL;
    days += 719468; /* shift epoch from 1970-01-01 to 0000-03-01 */

    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    long long y = (long long)yoe + era * 400 + (m <= 2);

    SDL_snprintf(out, out_sz, "%04lld-%02u-%02u", y, m, d);
}

void RbFormatDate(long long ms, char* out, size_t out_sz) {
    if (out == NULL || out_sz == 0) {
        return;
    }
    format_date_ms(ms, out, out_sz);
}

/* Fill label from a full .3sr path: basename with the directory prefix and
 * the ".3sr" suffix stripped, sanitized to printable ASCII through the same
 * copy_sanitized_name() helper the sidecar player names use above — a
 * filename byte outside 0x20-0x7E is just as unsafe for SSPutStrProP's
 * 128-entry glyph table (sc_sub.c) as an unsanitized sidecar name is. */
static void label_from_path(const char* path_3sr, char* out, size_t out_sz) {
    const char* base = path_3sr;

    for (const char* p = path_3sr; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }

    copy_sanitized_name(base, out, out_sz);

    const size_t len = SDL_strlen(out);
    if (len >= 4 && SDL_strcmp(out + len - 4, ".3sr") == 0) {
        out[len - 4] = '\0';
    }
}

/* Parse the `<name>.meta.json` sidecar for entry `e`, whose path/label are
 * already filled. Best-effort: any failure leaves have_names/have_date false
 * and is never fatal. */
static void parse_sidecar(RbEntry* e) {
    const size_t plen = SDL_strlen(e->path);
    const char* suffix = ".3sr";
    if (plen < 4 || SDL_strcmp(e->path + plen - 4, suffix) != 0) {
        return;
    }

    char meta_path[RB_PATH_MAX];
    if (plen - 4 + sizeof(".meta.json") > sizeof(meta_path)) {
        return;
    }
    SDL_memcpy(meta_path, e->path, plen - 4);
    SDL_strlcpy(meta_path + (plen - 4), ".meta.json", sizeof(meta_path) - (plen - 4));

    SDL_PathInfo info;
    if (!SDL_GetPathInfo(meta_path, &info) || info.type != SDL_PATHTYPE_FILE) {
        return; /* missing sidecar -> filename fallback */
    }

    size_t size = 0;
    char* text = SDL_LoadFile(meta_path, &size);
    if (text == NULL) {
        return;
    }

    cJSON* root = cJSON_ParseWithLength(text, size);
    SDL_free(text);
    if (root == NULL) {
        return; /* corrupt sidecar -> filename fallback */
    }

    const cJSON* players = cJSON_GetObjectItemCaseSensitive(root, "players");
    if (cJSON_IsArray(players)) {
        const bool p1 = copy_sanitized_name(player_name_from_item(cJSON_GetArrayItem(players, 0)), e->p1,
                                             sizeof(e->p1));
        const bool p2 = copy_sanitized_name(player_name_from_item(cJSON_GetArrayItem(players, 1)), e->p2,
                                             sizeof(e->p2));
        e->have_names = p1 && p2;
    }

    const cJSON* date = cJSON_GetObjectItemCaseSensitive(root, "date");
    if (cJSON_IsNumber(date) && date->valuedouble > 0.0) {
        e->date_ms = (long long)date->valuedouble;
        format_date_ms(e->date_ms, e->date, sizeof(e->date));
        e->have_date = true;
    }

    const cJSON* duration = cJSON_GetObjectItemCaseSensitive(root, "duration");
    if (cJSON_IsNumber(duration) && duration->valuedouble > 0.0) {
        e->duration_secs = (long long)duration->valuedouble;
    }

    cJSON_Delete(root);
}

/* ---------------------------------------------------------------------- */
/* Fetch-dir (needs-conversion) recognition                               */
/* ---------------------------------------------------------------------- */

static bool file_exists(const char* dir, const char* name) {
    char path[RB_PATH_MAX];
    SDL_snprintf(path, sizeof(path), "%s/%s", dir, name);
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

static bool dir_has_3sr(const char* dir) {
    int n = 0;
    char** matches = SDL_GlobDirectory(dir, "*.3sr", 0, &n);
    if (matches == NULL) {
        return false;
    }
    SDL_free(matches);
    return n > 0;
}

bool RbDirNeedsConversion(const char* dir) {
    if (dir == NULL) {
        return false;
    }
    /* fcade_stream.c writes `inputs` + `savestate` (plus frames.bin/summary.json)
     * on a raw fetch; a converted replay additionally has a `*.3sr`. A dir with
     * the raw inputs but no `.3sr` is still awaiting off-device conversion. */
    return file_exists(dir, "inputs") && file_exists(dir, "savestate") && !dir_has_3sr(dir);
}

/* Best-effort: pull `downloaded_at` (unix seconds — fcade_stream.c
 * write_summary_json) from a fetch dir's summary.json so a just-downloaded
 * replay sorts near the top of the local list. Leaves have_date false on any
 * failure. */
static void parse_fetch_summary_date(const char* dir, RbEntry* e) {
    char meta_path[RB_PATH_MAX];
    SDL_snprintf(meta_path, sizeof(meta_path), "%s/summary.json", dir);

    size_t size = 0;
    char* text = SDL_LoadFile(meta_path, &size);
    if (text == NULL) {
        return;
    }
    cJSON* root = cJSON_ParseWithLength(text, size);
    SDL_free(text);
    if (root == NULL) {
        return;
    }
    const cJSON* dl = cJSON_GetObjectItemCaseSensitive(root, "downloaded_at");
    if (cJSON_IsNumber(dl) && dl->valuedouble > 0.0) {
        e->date_ms = (long long)dl->valuedouble * 1000LL; /* seconds -> ms */
        format_date_ms(e->date_ms, e->date, sizeof(e->date));
        e->have_date = true;
    }
    cJSON_Delete(root);
}

/* Append a needs-conversion fetch dir as a display-only entry. `dir` is the
 * fetch directory; the label is its basename (the quarkid). Returns false when
 * the cap is reached. */
static bool add_fetch_entry(const char* dir, RbEntry* entries, int* count, int max) {
    if (*count >= max) {
        return false;
    }
    RbEntry* e = &entries[*count];
    SDL_zerop(e);
    SDL_strlcpy(e->path, dir, sizeof(e->path));
    e->needs_conversion = true;

    /* label = trailing path component (the <quarkid> directory name). */
    const char* base = dir;
    for (const char* p = dir; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    copy_sanitized_name(base, e->label, sizeof(e->label));

    parse_fetch_summary_date(dir, e);
    *count += 1;
    return true;
}

/* ---------------------------------------------------------------------- */
/* Scan                                                                   */
/* ---------------------------------------------------------------------- */

/* Append one .3sr entry (given its full path) to entries[], parsing its
 * sidecar. Returns false when the cap is reached. */
static bool add_entry(const char* full_path, RbEntry* entries, int* count, int max) {
    if (*count >= max) {
        return false;
    }

    RbEntry* e = &entries[*count];
    SDL_zerop(e);
    SDL_strlcpy(e->path, full_path, sizeof(e->path));
    label_from_path(full_path, e->label, sizeof(e->label));
    parse_sidecar(e);
    *count += 1;
    return true;
}

/* Glob `dir` for `*.3sr` and append each as `<dir>/<name>`. */
static void scan_dir_flat(const char* dir, RbEntry* entries, int* count, int max) {
    int n = 0;
    char** matches = SDL_GlobDirectory(dir, "*.3sr", 0, &n);
    if (matches == NULL) {
        return;
    }

    for (int i = 0; i < n; i++) {
        char full[RB_PATH_MAX];
        SDL_snprintf(full, sizeof(full), "%s/%s", dir, matches[i]);
        if (!add_entry(full, entries, count, max)) {
            break;
        }
    }

    SDL_free(matches);
}

int RbScan(const char* root, RbEntry* entries, int max) {
    if (root == NULL || entries == NULL || max <= 0) {
        return 0;
    }
    if (max > RB_MAX_ENTRIES) {
        max = RB_MAX_ENTRIES;
    }

    int count = 0;

    /* Flat files directly under root. */
    scan_dir_flat(root, entries, &count, max);

    /* One level of subdirectories. Enumerate root's children, and for any
     * that is a directory, glob it for *.3sr too. */
    int n = 0;
    char** children = SDL_GlobDirectory(root, "*", 0, &n);
    if (children != NULL) {
        for (int i = 0; i < n && count < max; i++) {
            char child[RB_PATH_MAX];
            SDL_snprintf(child, sizeof(child), "%s/%s", root, children[i]);

            SDL_PathInfo info;
            if (SDL_GetPathInfo(child, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
                const int before = count;
                scan_dir_flat(child, entries, &count, max);
                /* No `.3sr` came out of this child but it holds a raw fetch
                 * (inputs + savestate): surface it as a needs-conversion row
                 * (Step F2b) rather than hiding a just-downloaded replay. */
                if (count == before && RbDirNeedsConversion(child)) {
                    add_fetch_entry(child, entries, &count, max);
                }
            }
        }
        SDL_free(children);
    }

    return count;
}

/* ---------------------------------------------------------------------- */
/* Sort                                                                   */
/* ---------------------------------------------------------------------- */

/* Newest date first; dated before undated; undated alphabetical by label. */
static int entry_cmp(const void* pa, const void* pb) {
    const RbEntry* a = (const RbEntry*)pa;
    const RbEntry* b = (const RbEntry*)pb;

    if (a->have_date && b->have_date) {
        if (a->date_ms > b->date_ms) {
            return -1;
        }
        if (a->date_ms < b->date_ms) {
            return 1;
        }
        /* Same timestamp — stable tiebreak on label so order is total. */
        return SDL_strcmp(a->label, b->label);
    }

    if (a->have_date != b->have_date) {
        return a->have_date ? -1 : 1; /* dated sorts before undated */
    }

    /* Neither dated: alphabetical ascending. */
    return SDL_strcmp(a->label, b->label);
}

void RbSort(RbEntry* entries, int count) {
    if (entries == NULL || count <= 1) {
        return;
    }
    SDL_qsort(entries, (size_t)count, sizeof(*entries), entry_cmp);
}

/* ---------------------------------------------------------------------- */
/* Pagination                                                             */
/* ---------------------------------------------------------------------- */

int RbPageCount(int count, int page_size) {
    if (page_size <= 0) {
        return 1;
    }
    if (count <= 0) {
        return 1;
    }
    return (count + page_size - 1) / page_size;
}

int RbClampPage(int page, int count, int page_size) {
    const int pages = RbPageCount(count, page_size);
    if (page < 0) {
        return 0;
    }
    if (page >= pages) {
        return pages - 1;
    }
    return page;
}

int RbPageStart(int page, int page_size) {
    if (page < 0 || page_size <= 0) {
        return 0;
    }
    return page * page_size;
}

int RbPageRows(int page, int count, int page_size) {
    if (page_size <= 0 || count <= 0) {
        return 0;
    }
    const int start = RbPageStart(RbClampPage(page, count, page_size), page_size);
    int rows = count - start;
    if (rows > page_size) {
        rows = page_size;
    }
    if (rows < 0) {
        rows = 0;
    }
    return rows;
}

/* ---------------------------------------------------------------------- */
/* Row formatting                                                         */
/* ---------------------------------------------------------------------- */

void RbFormatRow(const RbEntry* e, char* out, size_t out_sz) {
    if (out == NULL || out_sz == 0) {
        return;
    }
    if (e == NULL) {
        out[0] = '\0';
        return;
    }

    /* Compose into a generous scratch buffer, then hard-clamp to the row
     * width. RB_ROW_MAX_CHARS bounds the on-screen glyph budget; out_sz is
     * the caller's buffer. The effective limit is the smaller of the two. */
    char scratch[256];

    if (e->needs_conversion) {
        /* Display-only: a raw fetch dir awaiting off-device conversion. The
         * marker is what tells the player this row can't be launched yet. */
        SDL_snprintf(scratch, sizeof(scratch), "%s  NEEDS CONVERSION", e->label);
    } else if (e->have_names) {
        if (e->have_date) {
            SDL_snprintf(scratch, sizeof(scratch), "%s vs %s  %s", e->p1, e->p2, e->date);
        } else {
            SDL_snprintf(scratch, sizeof(scratch), "%s vs %s", e->p1, e->p2);
        }
    } else {
        if (e->have_date) {
            SDL_snprintf(scratch, sizeof(scratch), "%s  %s", e->label, e->date);
        } else {
            SDL_snprintf(scratch, sizeof(scratch), "%s", e->label);
        }
    }

    size_t limit = out_sz;
    if (limit > RB_ROW_MAX_CHARS) {
        limit = RB_ROW_MAX_CHARS;
    }

    /* SDL_strlcpy writes at most limit-1 chars + NUL — the truncation the
     * row width requires. */
    SDL_strlcpy(out, scratch, limit);
}
