#if defined(STATCHECK)

#ifndef STATCHECK_SEED_AUDIT_H
#define STATCHECK_SEED_AUDIT_H

#include <SDL3/SDL_iostream.h>

#include <stdbool.h>

/* Seed audit (docs/research-arcade-balance-desyncs.md, "The seed audit").
 *
 * Runs ONCE, at the seed frame (`start_index - 1`) -- the frame
 * `Statcheck_SyncValues` imports from -- and immediately AFTER that import.
 * At that instant our engine has not yet executed any frame that will be
 * compared, so a field that already differs is an IMPORTED-STATE gap, never
 * engine behaviour. That is a categorically different diagnosis from a
 * divergence and this is the only place in the run where the two can be told
 * apart cheaply.
 *
 * Read-only: every function here reads engine globals and the archive stream
 * and prints. It writes no engine state (that is Statcheck_SyncValues' job)
 * and never exits. */
void StatcheckSeedAudit_Run(SDL_IOStream* io, int seed_frame);

/* True once StatcheckSeedAudit_Run has found at least one NON-allowlisted
 * mismatch. `statcheck_compare.c`'s stop_if consults this so a later
 * comparison failure exits 4 ("comparison failed with a dirty seed -- suspect
 * the import, not the engine") instead of 1 ("engine divergence"). */
bool StatcheckSeedAudit_Dirty(void);

/* Number of non-allowlisted seed mismatches; 0 before the audit runs. */
int StatcheckSeedAudit_MismatchCount(void);

#endif

#endif
