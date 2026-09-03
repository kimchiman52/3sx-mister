#ifndef REPLAY_SYNC_H
#define REPLAY_SYNC_H

/*
 * replay_sync — the wrapper-side fetcher for the weekly-best replay set.
 *
 * The game side (src/replay/replay_shuffle.c) plays whatever `.3sr` files are
 * cached under the replays root and never talks to the network. Filling that
 * cache is THIS module's job, and it lives in the HPS wrapper deliberately:
 *
 *   - the game runs a 60 fps budget on an 800 MHz ARM; sockets, JSON parsing
 *     and disk management have no business in that frame loop;
 *   - the refresh has to be able to happen when the game is not running (the
 *     wrapper's UI loop keeps ticking around the child either way).
 *
 * Everything network-facing is already built and audited in replay_proxy.h —
 * framing, JSON, base64, the manifest-then-per-game get3sr loop, and three
 * independent single-in-flight async slots. This module is only the policy on
 * top of it: WHICH rows make up the set, WHEN to refresh, and what gets
 * recorded on disk afterwards.
 *
 * Storage posture: this module only ever ADDS files and rewrites
 * <replay_root>/manifest.json. It NEVER deletes anything. Pruning the on-device
 * set down to the manifest belongs game-side, where
 * src/replay/replay_storage.c already has the audited, symlink-refusing,
 * realpath-confined deletion primitives; a second deletion implementation in
 * the wrapper is exactly what AGENTS.md warns against.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drive one step of the sync state machine. Cheap and NON-BLOCKING: designed to
 * be called from thirdsarm_wrapper.cpp's ~1 kHz wait_for_child() loop next to
 * poll_status_changes(). Internally rate-limited, so the vast majority of calls
 * return after one monotonic-clock read.
 *
 * Self-initializing — there is no Init(). The first call decides whether a
 * refresh is due and, if so, starts one; subsequent calls drain the async
 * search/fetch slots across successive iterations.
 *
 * Not thread-safe; call it from the wrapper's UI thread only. */
void ReplaySyncTick(void);

/* ---- Pure time arithmetic, exported for the host unit test ---------------
 * Both take and return ms since the Unix epoch (UTC). */

/* Lower bound of Fightcade's WEEKLY BEST window: today's UTC midnight minus
 * seven days. This mirrors the Fightcade client exactly — its WEEKLY BEST tab
 * is not a separate feed, it is the same `searchquarks` call with best:true and
 * `since = Date.now() - Date.now() % 864e5 - 6048e5`. */
long long ReplaySyncWeeklySinceMs(long long now_ms);

/* The most recent scheduled refresh instant at or before `now_ms`. See the
 * RS_DUE_HOUR_UTC_MS comment in replay_sync.c for why the schedule is pinned to
 * a UTC hour rather than a local-time one. */
long long ReplaySyncLastDueMs(long long now_ms);

#ifdef __cplusplus
}
#endif

#endif /* REPLAY_SYNC_H */
