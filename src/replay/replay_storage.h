#ifndef REPLAY_REPLAY_STORAGE_H
#define REPLAY_REPLAY_STORAGE_H

#include <stdbool.h>

/* INTENTIONALLY CALLER-LESS (game side) AS OF THE REPLAY DESCOPE.
 * The in-game pad-driven replay browser (src/replay/replay_browser.c) was
 * this module's only caller and has been deleted. Every function below is
 * kept, exported and non-static on purpose: the shuffle viewer that replaces
 * the browser adopts the whole API — it prunes the cached set through
 * ReplayStorage_DeleteEntry()/ReplayStorage_DeleteFetchDir() and sweeps it
 * with ReplayStorage_Evict(). Do not delete or `static`-ify this module in
 * the interim; there is no game-side caller until that module lands.
 *
 * Storage lifecycle for the cached `.3sr` replay set (Step F4/Step 3 of
 * docs/plan-fcade-replay-browser.md).
 *
 * Fightcade replay fetches (E1a, src/replay/fcade_stream.c:699-703) place
 * four raw stream files alongside the tiny `.3sr` + `.meta.json` a fetch
 * eventually produces: `frames.bin`, `inputs`, `savestate`, `summary.json`.
 * The raw files are bulky (savestate alone decompresses to ~1.9 MB,
 * fcade_stream.c:58) and dwarf the `.3sr`/`.meta.json` the browser actually
 * needs to list and play a replay. This module keeps the replays root
 * tidy:
 *
 *   - ReplayStorage_Evict(): LRU-evicts raw stream files (oldest-mtime-first
 *     across the root's direct child directories) once the total raw byte
 *     count exceeds a configured cap, WITHOUT ever touching `.3sr` or
 *     `.meta.json`.
 *   - ReplayStorage_DeleteEntry(): removes one replay's full file set (the
 *     `.3sr`, its `.meta.json` sidecar, and any raw files in the same
 *     directory) on an explicit user delete action.
 *   - ReplayStorage_DeleteFetchDir(): removes one NEEDS-CONVERSION raw fetch
 *     directory in its entirety (raw files + directory) on an explicit user
 *     delete action, for rows that have no `.3sr` yet and so cannot go
 *     through ReplayStorage_DeleteEntry.
 *
 * SAFETY (AGENTS.md:5-10). Every unlink/rmdir target is validated before
 * touching the filesystem:
 *   (a) lstat'd directly — a symlink is refused outright, never followed;
 *   (b) realpath-resolved and required to sit strictly under the
 *       realpath-resolved browser root.
 * Deletions are by fixed basename whitelist only: no recursive delete, no
 * wildcard unlink, no path escaping the root is ever possible even if a
 * caller passes a manipulated/malicious path. Every actual deletion is
 * logged (SDL_Log) so on-device behavior is SSH-verifiable after the fact.
 *
 * No engine dependency (same "pure back-end" posture as
 * replay_browser_scan.h) — SDL3 filesystem/string helpers plus POSIX
 * lstat/realpath (the two primitives SDL3 does not expose: it always
 * follows symlinks and has no canonicalization call, see
 * SDL_filesystem.h's SDL_PathType doc). Desktop/MiSTer/Miyoo (Linux-family)
 * targets only — matches the existing POSIX-only footprint of the E1a
 * fetch code this module cleans up after (fcade_stream.c's mkdir_p is
 * likewise unguarded for non-POSIX hosts). */

/* Evict raw stream files (frames.bin/inputs/savestate/summary.json) from
 * `root`'s direct child directories until the total raw byte count is <=
 * `max_mb` megabytes, or every child directory has been swept. Eviction
 * order is oldest-first, where a directory's LRU key is the max mtime among
 * its present raw files (the most recent write to that fetch's raw set —
 * i.e. how recently that replay's raw payload was last touched). A child
 * directory whose raw files are fully evicted is rmdir'd only if it becomes
 * completely empty (a remaining `.3sr`/`.meta.json` keeps it). `max_mb <= 0`
 * disables eviction (no-op) — the caller (config key `replays-max-mb`)
 * treats 0 as "eviction disabled, delete-only". Safe to call with a `root`
 * that does not exist yet (no-op). Intended call sites: browser-open, and
 * after any future replay-fetch/download completes. */
void ReplayStorage_Evict(const char* root, int max_mb);

/* Delete one replay under `root`: `path_3sr` itself, its `<name>.meta.json`
 * sidecar (same suffix-swap replay_browser_scan.c uses to find it), and the
 * raw stream whitelist basenames in the same directory (only when that
 * directory is `root` itself or a validated direct child of it) — then
 * rmdir the containing directory if it is a non-root child of `root` and is
 * now completely empty. `root` is required (not implied global state) so
 * every candidate can be validated against it the same way
 * ReplayStorage_Evict validates its candidates. Returns true iff the `.3sr`
 * itself was removed (the caller's signal to rescan); false if `path_3sr`
 * failed validation (wrong suffix, outside root, a symlink, missing, ...) or
 * the underlying removal failed. */
bool ReplayStorage_DeleteEntry(const char* root, const char* path_3sr);

/* Delete one needs-conversion raw-fetch directory under `root` in its
 * entirety: the raw stream whitelist basenames (frames.bin/inputs/savestate/
 * summary.json), a stray `<basename-of-dir_path>.meta.json` sidecar if one is
 * present (a partially-run off-device conversion, tools/fcade-replays/
 * make_3sr.py, can in principle leave one behind before the `.3sr` itself
 * lands), and finally the directory itself. This is the delete counterpart
 * for the rows replay_browser_scan.h's RbEntry.needs_conversion marks NEEDS
 * CONVERSION -- those rows have no `.3sr` yet (fcade_stream.c's raw fetch
 * output only, no conversion run), so ReplayStorage_DeleteEntry's `.3sr`
 * suffix check refuses them by design. `dir_path` is validated exactly like
 * ReplayStorage_Evict/DeleteEntry validate their candidates (lstat'd
 * directly -- a symlink is refused outright; realpath-resolved and required
 * to sit strictly under root_real) with one added constraint: `dir_path`
 * MUST be a direct child of `root` -- never `root` itself, never a path
 * nested more than one level under it. The directory is only rmdir'd once
 * whitelist removal leaves it completely empty (an unexpected leftover file
 * keeps it, same conservative posture as Evict/DeleteEntry). Returns true
 * iff the directory itself was removed (the caller's signal to rescan);
 * false on any validation failure or if the directory was not empty
 * afterward. */
bool ReplayStorage_DeleteFetchDir(const char* root, const char* dir_path);

#endif /* REPLAY_REPLAY_STORAGE_H */
