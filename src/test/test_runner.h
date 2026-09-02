#if defined(DEBUG) || ENABLE_PERF_TELEMETRY

#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <stdbool.h>

void TestRunner_Prologue();
void TestRunner_Epilogue();
/* Step B3 EXPERIMENT (docs/plan-fcade-replay-browser.md): pin
 * game-mode=arcade + arcade-balance=true for a raw fcade-stream playback
 * session. Real body only in #if DEBUG builds; main.c's call site is
 * #if DEBUG-gated too. */
void TestRunner_PinFcadeConfig(void);
const char* TestRunner_GetPhaseName(void);
bool TestRunner_IsSupportedPhaseName(const char* phase_name);
bool TestRunner_IsPhaseActive(const char* phase_name);

#endif

#endif
