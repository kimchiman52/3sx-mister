#if defined(STATCHECK)

#ifndef STATCHECK_COMPARE_H
#define STATCHECK_COMPARE_H

#include <SDL3/SDL_iostream.h>

/* Plan A3b (docs/plan-fcade-replay-browser.md): STATCHECK ports of upstream
 * test_runner_compare.c's compare_values/sync_values, renamed so they can
 * never collide with the DEBUG harness's same-purpose symbols (§4.3).
 * `frame` is the archive frame index being compared — it is echoed in the
 * failure report when a comparison mismatches. */
void Statcheck_CompareValues(SDL_IOStream* io, Uint64 frame);
void Statcheck_SyncValues(SDL_IOStream* io);

#endif

#endif
