#ifndef TEST_ASSERT_H
#define TEST_ASSERT_H

#include <stdio.h>

/* assert_equals — ported from upstream #268 (52a395bd) with the tests that
 * use it (fork commit a9a4c11c); the rest of that statcheck tooling commit
 * was deliberately not pulled in at the time.
 *
 * Extracted into this shared header for plan A3b
 * (docs/plan-fcade-replay-browser.md): the macro used to live inside
 * `#if DEBUG` in src/test/test_runner_compare.c, which made it invisible to
 * a Release STATCHECK build. Both the DEBUG compare
 * (src/test/test_runner_compare.c) and the STATCHECK compare
 * (src/test/statcheck_compare.c) include this header now; the macro body
 * exists exactly once. Deliberately no DEBUG/STATCHECK gate here — the
 * header defines only a macro, so including it from any TU is harmless.
 *
 * The including TU must provide a `stop_if(bool)` that halts execution when
 * passed true (the DEBUG compare uses port/utils.h's; the STATCHECK compare
 * defines its own file-local one that reports the failing frame and honors
 * --headless). */
#define assert_equals(actual, expected)                                                                                \
    do {                                                                                                               \
        if ((actual) != (expected)) {                                                                                  \
            fprintf(                                                                                                   \
                stderr,                                                                                                \
                "%s:%d: %s (%lld) != %s (%lld)\n",                                                                     \
                __FILE__,                                                                                              \
                __LINE__,                                                                                              \
                #actual,                                                                                               \
                (long long)(actual),                                                                                   \
                #expected,                                                                                             \
                (long long)(expected)                                                                                  \
            );                                                                                                         \
            stop_if(true);                                                                                             \
        }                                                                                                              \
    } while (0)

#endif
