/* replay_storage.c — Step F4/Step 3 of docs/plan-fcade-replay-browser.md.
 *
 * See replay_storage.h for the contract and the safety rationale. This file
 * implements the two operations (LRU eviction of raw stream files, and
 * whole-entry delete) that keep the replay-browser root from accumulating
 * unbounded raw Fightcade-fetch payloads.
 */

/* realpath() is gated behind extended feature-test levels on glibc (the
 * shipping ARM cross target); Apple libc declares it unconditionally, which
 * masks the gap in native macOS builds. Same convention as netplay/stun.c. */
#define _GNU_SOURCE

#include "replay/replay_storage.h"

#include <SDL3/SDL.h>

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#if defined(_WIN32)

/* Not implemented on Windows, deliberately.
 *
 * Every guard in this file rests on three POSIX primitives that MinGW does not
 * provide: realpath() to prove a path resolves strictly under the replay root,
 * and lstat()/S_ISLNK() to refuse a symlink without ever following it. Each
 * available substitute is strictly weaker in a way that matters here, because
 * these checks gate unlink():
 *
 *   - _fullpath() canonicalises but SUCCEEDS for paths that do not exist, so a
 *     failed resolve would stop meaning "not there".
 *   - there is no portable S_ISLNK on MinGW, so the symlink refusal would have
 *     to become a constant 0 -- i.e. silently disabled.
 *
 * A shimmed build would therefore relax delete guards rather than fail, which
 * is the wrong direction for code whose whole job is to bound what may be
 * removed. Nothing calls into this file on any platform today
 * (ReplayStorage_Evict / _DeleteEntry / _DeleteFetchDir have no callers in the
 * tree; it is compiled only because CMakeLists.txt globs src/*.c), so refusing
 * costs nothing and keeps the POSIX guarantees honest.
 *
 * If the replay browser is ever wired up on Windows, implement these against
 * the Win32 API -- GetFinalPathNameByHandleW for canonicalisation and
 * FILE_ATTRIBUTE_REPARSE_POINT for the link check -- rather than reintroducing
 * a POSIX-shaped shim. */

void ReplayStorage_Evict(const char* root, int max_mb) {
    (void)root;
    (void)max_mb;
}

bool ReplayStorage_DeleteEntry(const char* root, const char* path_3sr) {
    (void)root;
    (void)path_3sr;
    return false;
}

bool ReplayStorage_DeleteFetchDir(const char* root, const char* dir_path) {
    (void)root;
    (void)dir_path;
    return false;
}

#else

/* Raw-stream whitelist (E1a fetch outputs, fcade_stream.c:699-703). Fixed
 * basenames only — eviction/delete never unlink by pattern, glob, or
 * recursive walk; every candidate path is built from one of these four
 * literals joined to an already-validated directory. */
static const char* const RAW_BASENAMES[] = {
    "frames.bin",
    "inputs",
    "savestate",
    "summary.json",
};
#define RAW_BASENAME_COUNT ((int)(sizeof(RAW_BASENAMES) / sizeof(RAW_BASENAMES[0])))

/* Bound on how many child directories a single evict pass tracks. A
 * pathological number of fetch dirs degrades to "the first RS_MAX_CANDIDATE_
 * DIRS considered, sorted and evicted among themselves" rather than
 * unbounded stack/heap growth — the same defensive posture RB_MAX_ENTRIES
 * uses for the browser's own scan (replay_browser_scan.h:39). */
#define RS_MAX_CANDIDATE_DIRS 256

/* ---------------------------------------------------------------------- */
/* Path validation (AGENTS.md:10) — shared by evict and delete.           */
/* ---------------------------------------------------------------------- */

/* Refuse to touch `path` unless it (a) exists, (b) is NOT a symlink (lstat,
 * checked directly — a symlink is refused outright, regardless of where it
 * points; SDL_GetPathInfo always follows symlinks per SDL_filesystem.h and
 * so cannot make this distinction), (c) is a regular file, and (d)
 * realpath-resolves to a location strictly under `root_real` (itself already
 * realpath-resolved by the caller). Returns false silently for a path that
 * simply doesn't exist (the common no-op case for an already-evicted or
 * never-fetched raw file); logs a warning for an existing path that fails
 * (b)/(c)/(d). */
static bool validate_file_under_root(const char* root_real, const char* path) {
    struct stat lst;
    if (lstat(path, &lst) != 0) {
        return false; /* doesn't exist -- nothing to validate/delete */
    }
    if (S_ISLNK(lst.st_mode)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "replay-storage: refusing to touch symlink '%s' (never followed)", path);
        return false;
    }
    if (!S_ISREG(lst.st_mode)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "replay-storage: refusing to touch non-regular-file '%s'", path);
        return false;
    }

    char real[PATH_MAX];
    if (realpath(path, real) == NULL) {
        return false;
    }

    const size_t root_len = SDL_strlen(root_real);
    if (SDL_strncmp(real, root_real, root_len) != 0 || real[root_len] != '/') {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "replay-storage: refusing to touch '%s' (resolves to '%s', outside root '%s')", path, real,
                    root_real);
        return false;
    }

    return true;
}

/* Same shape as validate_file_under_root but for a directory: must exist,
 * must not itself be a symlink (a symlinked "subdir" pointing outside root
 * must never be traversed/rmdir'd), must be an actual directory, and must
 * realpath-resolve under root_real. */
static bool validate_dir_under_root(const char* root_real, const char* dir) {
    struct stat lst;
    if (lstat(dir, &lst) != 0) {
        return false;
    }
    if (S_ISLNK(lst.st_mode)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "replay-storage: refusing to traverse symlinked dir '%s' (never followed)", dir);
        return false;
    }
    if (!S_ISDIR(lst.st_mode)) {
        return false;
    }

    char real[PATH_MAX];
    if (realpath(dir, real) == NULL) {
        return false;
    }

    const size_t root_len = SDL_strlen(root_real);
    if (SDL_strncmp(real, root_real, root_len) != 0 || real[root_len] != '/') {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "replay-storage: refusing to traverse '%s' (resolves to '%s', outside root '%s')", dir, real,
                    root_real);
        return false;
    }

    return true;
}

/* True iff `dir` (a path string, not yet validated) realpath-resolves to
 * exactly `root_real` itself (the flat-layout case: raw files sitting
 * directly in the browser root rather than in a per-fetch subdirectory). */
static bool dir_is_root_itself(const char* root_real, const char* dir) {
    char real[PATH_MAX];
    if (realpath(dir, real) == NULL) {
        return false;
    }
    return SDL_strcmp(real, root_real) == 0;
}

/* True iff `dir` currently has zero entries (SDL_GlobDirectory's "*" pattern,
 * the same call RbScan/ReplayStorage use elsewhere for enumeration — dot
 * entries are not returned). */
static bool dir_is_empty(const char* dir) {
    int count = 0;
    char** entries = SDL_GlobDirectory(dir, "*", 0, &count);
    if (entries != NULL) {
        SDL_free(entries);
    }
    return count == 0;
}

/* Delete one raw-whitelist file if present and valid; returns its size (0 if
 * absent/invalid/failed) so the caller can keep a running byte total. */
static Sint64 delete_raw_file_if_present(const char* root_real, const char* dir, const char* basename) {
    char file[PATH_MAX];
    SDL_snprintf(file, sizeof(file), "%s/%s", dir, basename);

    if (!validate_file_under_root(root_real, file)) {
        return 0;
    }

    SDL_PathInfo info;
    const Sint64 size = SDL_GetPathInfo(file, &info) ? (Sint64)info.size : 0;

    if (SDL_RemovePath(file)) {
        SDL_Log("replay-storage: evicted raw file '%s' (%lld bytes)", file, (long long)size);
        return size;
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "replay-storage: failed to remove raw file '%s': %s", file,
                SDL_GetError());
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Evict                                                                   */
/* ---------------------------------------------------------------------- */

typedef struct EvictCandidate {
    char dir[PATH_MAX];
    Sint64 total_bytes;
    Sint64 mtime; /* per-dir LRU key: max mtime among its present raw files */
} EvictCandidate;

void ReplayStorage_Evict(const char* root, int max_mb) {
    if (root == NULL || root[0] == '\0' || max_mb <= 0) {
        return;
    }

    char root_real[PATH_MAX];
    if (realpath(root, root_real) == NULL) {
        return; /* root doesn't exist yet -- nothing to evict */
    }

    int n = 0;
    char** children = SDL_GlobDirectory(root, "*", 0, &n);
    if (children == NULL) {
        return;
    }

    static EvictCandidate candidates[RS_MAX_CANDIDATE_DIRS];
    int cand_count = 0;
    Sint64 grand_total = 0;

    for (int i = 0; i < n && cand_count < RS_MAX_CANDIDATE_DIRS; i++) {
        char child[PATH_MAX];
        SDL_snprintf(child, sizeof(child), "%s/%s", root, children[i]);

        if (!validate_dir_under_root(root_real, child)) {
            continue; /* not a dir, a symlink, or resolves outside root */
        }

        Sint64 total = 0;
        Sint64 newest_mtime = 0;
        bool any_raw = false;

        for (int b = 0; b < RAW_BASENAME_COUNT; b++) {
            char file[PATH_MAX];
            SDL_snprintf(file, sizeof(file), "%s/%s", child, RAW_BASENAMES[b]);
            if (!validate_file_under_root(root_real, file)) {
                continue;
            }
            SDL_PathInfo info;
            if (!SDL_GetPathInfo(file, &info) || info.type != SDL_PATHTYPE_FILE) {
                continue;
            }
            any_raw = true;
            total += (Sint64)info.size;
            if (info.modify_time > newest_mtime) {
                newest_mtime = info.modify_time;
            }
        }

        if (!any_raw) {
            continue; /* nothing raw to evict in this dir */
        }

        SDL_strlcpy(candidates[cand_count].dir, child, sizeof(candidates[cand_count].dir));
        candidates[cand_count].total_bytes = total;
        candidates[cand_count].mtime = newest_mtime;
        cand_count += 1;
        grand_total += total;
    }
    SDL_free(children);

    const Sint64 cap_bytes = (Sint64)max_mb * 1024 * 1024;
    if (grand_total <= cap_bytes) {
        return;
    }

    /* Sort ascending by mtime (oldest/LRU first); ties broken by dir path
     * for a deterministic, testable order. Selection sort is fine at this
     * scale (RS_MAX_CANDIDATE_DIRS is a hard, small cap). */
    for (int i = 0; i < cand_count - 1; i++) {
        int min_idx = i;
        for (int j = i + 1; j < cand_count; j++) {
            const bool older = candidates[j].mtime < candidates[min_idx].mtime;
            const bool tie_earlier = candidates[j].mtime == candidates[min_idx].mtime &&
                                      SDL_strcmp(candidates[j].dir, candidates[min_idx].dir) < 0;
            if (older || tie_earlier) {
                min_idx = j;
            }
        }
        if (min_idx != i) {
            const EvictCandidate tmp = candidates[i];
            candidates[i] = candidates[min_idx];
            candidates[min_idx] = tmp;
        }
    }

    SDL_Log("replay-storage: evicting (root='%s' cap=%dMB total=%lldB over %d candidate dir(s))", root, max_mb,
            (long long)grand_total, cand_count);

    for (int i = 0; i < cand_count && grand_total > cap_bytes; i++) {
        const char* dir = candidates[i].dir;

        for (int b = 0; b < RAW_BASENAME_COUNT; b++) {
            grand_total -= delete_raw_file_if_present(root_real, dir, RAW_BASENAMES[b]);
        }

        /* rmdir only if the fetch dir is now completely empty -- a
         * remaining .3sr/.meta.json keeps it (never touched above). */
        if (dir_is_empty(dir)) {
            if (SDL_RemovePath(dir)) {
                SDL_Log("replay-storage: removed now-empty replay dir '%s'", dir);
            }
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Delete one entry                                                        */
/* ---------------------------------------------------------------------- */

bool ReplayStorage_DeleteEntry(const char* root, const char* path_3sr) {
    if (root == NULL || root[0] == '\0' || path_3sr == NULL || path_3sr[0] == '\0') {
        return false;
    }

    char root_real[PATH_MAX];
    if (realpath(root, root_real) == NULL) {
        return false;
    }

    const size_t plen = SDL_strlen(path_3sr);
    if (plen < 4 || SDL_strcmp(path_3sr + plen - 4, ".3sr") != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "replay-storage: refusing to delete '%s' (not a .3sr path)",
                    path_3sr);
        return false;
    }

    if (!validate_file_under_root(root_real, path_3sr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "replay-storage: refusing to delete '%s' (failed path validation)", path_3sr);
        return false;
    }

    const bool removed_3sr = SDL_RemovePath(path_3sr);
    if (removed_3sr) {
        SDL_Log("replay-storage: deleted '%s'", path_3sr);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "replay-storage: failed to remove '%s': %s", path_3sr,
                    SDL_GetError());
    }

    /* Meta sidecar: same suffix-swap replay_browser_scan.c:112-122 uses to
     * find it (".3sr" -> ".meta.json"). */
    char meta_path[PATH_MAX];
    if (plen - 4 + SDL_strlen(".meta.json") + 1 <= sizeof(meta_path)) {
        SDL_memcpy(meta_path, path_3sr, plen - 4);
        SDL_strlcpy(meta_path + (plen - 4), ".meta.json", sizeof(meta_path) - (plen - 4));
        if (validate_file_under_root(root_real, meta_path)) {
            if (SDL_RemovePath(meta_path)) {
                SDL_Log("replay-storage: deleted sidecar '%s'", meta_path);
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "replay-storage: failed to remove sidecar '%s': %s",
                            meta_path, SDL_GetError());
            }
        }
    }

    /* Raw whitelist basenames in the same directory -- only when that
     * directory is root itself (flat layout) or a validated direct child of
     * root (the E1b fetch-dir layout). */
    char dir[PATH_MAX];
    SDL_strlcpy(dir, path_3sr, sizeof(dir));
    char* slash = SDL_strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        SDL_strlcpy(dir, ".", sizeof(dir));
    }

    const bool dir_is_root = dir_is_root_itself(root_real, dir);
    const bool dir_ok = dir_is_root || validate_dir_under_root(root_real, dir);

    if (dir_ok) {
        for (int b = 0; b < RAW_BASENAME_COUNT; b++) {
            delete_raw_file_if_present(root_real, dir, RAW_BASENAMES[b]);
        }

        if (!dir_is_root && dir_is_empty(dir)) {
            if (SDL_RemovePath(dir)) {
                SDL_Log("replay-storage: removed now-empty replay dir '%s'", dir);
            }
        }
    }

    return removed_3sr;
}

/* ---------------------------------------------------------------------- */
/* Delete one needs-conversion fetch directory                            */
/* ---------------------------------------------------------------------- */

bool ReplayStorage_DeleteFetchDir(const char* root, const char* dir_path) {
    if (root == NULL || root[0] == '\0' || dir_path == NULL || dir_path[0] == '\0') {
        return false;
    }

    char root_real[PATH_MAX];
    if (realpath(root, root_real) == NULL) {
        return false;
    }

    /* Never the root itself -- a fetch dir is always a per-quark child. */
    if (dir_is_root_itself(root_real, dir_path)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "replay-storage: refusing to delete fetch dir '%s' (resolves to the browser root itself)",
                    dir_path);
        return false;
    }

    if (!validate_dir_under_root(root_real, dir_path)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "replay-storage: refusing to delete fetch dir '%s' (failed path validation)", dir_path);
        return false;
    }

    /* Direct-child check: dirname(realpath(dir_path)) must equal root_real
     * exactly -- validate_dir_under_root above only proves "somewhere under
     * root", which would also accept a path nested two-or-more levels deep. */
    char real[PATH_MAX];
    if (realpath(dir_path, real) == NULL) {
        return false; /* raced out from under us between the two checks above */
    }

    char parent[PATH_MAX];
    SDL_strlcpy(parent, real, sizeof(parent));
    char* slash = SDL_strrchr(parent, '/');
    if (slash == parent) {
        /* real == "/something" with no other slash -- parent is the
         * filesystem root "/". root_real is a realpath and so never equals
         * "" (the truncate-to-nothing case), only ever "/" in this shape. */
        SDL_strlcpy(parent, "/", sizeof(parent));
    } else if (slash != NULL) {
        *slash = '\0';
    }
    if (SDL_strcmp(parent, root_real) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "replay-storage: refusing to delete fetch dir '%s' (parent '%s' is not root '%s' -- not a "
                    "direct child)",
                    dir_path, parent, root_real);
        return false;
    }

    for (int b = 0; b < RAW_BASENAME_COUNT; b++) {
        delete_raw_file_if_present(root_real, dir_path, RAW_BASENAMES[b]);
    }

    /* A stray `<quarkid>.meta.json` inside the fetch dir itself -- not part
     * of fcade_stream.c's normal raw-fetch output (fcade_stream.c:699-703),
     * but defensively removed in case an off-device conversion pass left one
     * behind before the `.3sr` itself landed. `real`'s trailing path
     * component is the quarkid (dir basename), matching add_fetch_entry's
     * label (replay_browser_scan.c) and make_3sr.py's default --meta-out
     * naming. */
    const char* base = real;
    for (const char* p = real; *p != '\0'; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    char meta_path[PATH_MAX];
    if (SDL_snprintf(meta_path, sizeof(meta_path), "%s/%s.meta.json", dir_path, base) < (int)sizeof(meta_path)) {
        if (validate_file_under_root(root_real, meta_path)) {
            if (SDL_RemovePath(meta_path)) {
                SDL_Log("replay-storage: deleted fetch-dir sidecar '%s'", meta_path);
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "replay-storage: failed to remove fetch-dir sidecar '%s': %s", meta_path,
                            SDL_GetError());
            }
        }
    }

    if (!dir_is_empty(dir_path)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "replay-storage: fetch dir '%s' not empty after whitelist removal -- leaving directory in place",
                    dir_path);
        return false;
    }

    if (SDL_RemovePath(dir_path)) {
        SDL_Log("replay-storage: removed fetch dir '%s'", dir_path);
        return true;
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "replay-storage: failed to remove fetch dir '%s': %s", dir_path,
                SDL_GetError());
    return false;
}

#endif /* !_WIN32 */
