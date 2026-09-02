#ifndef SDL_APP_H
#define SDL_APP_H

#include "port/build_config.h"
#include <SDL3/SDL.h>

#define TARGET_FPS 59.59949
#define FRAME_METRICS_COUNT 240

typedef struct FrameMetrics {
    size_t head;
    float fps[FRAME_METRICS_COUNT];
    float frame_time[FRAME_METRICS_COUNT];
    float idle_time[FRAME_METRICS_COUNT];
} FrameMetrics;

// FPGA native video refresh rate: dedicated video PLL (50 MHz * 81/5/26 = 31.1538 MHz)
// / 4 CE_PIXEL / (495 * 264) total pixels = 7,788,462 / 130,680 = 59.5995 Hz.
// H-freq = 15,734 Hz (NTSC standard, exact). PLL error vs. TARGET_FPS: 1.4 microhertz.
// ARM frame pacing uses TARGET_FPS directly (no compensation needed).
#define NV_TARGET_FPS TARGET_FPS

int SDLApp_PreInit();
int SDLApp_FullInit();
void SDLApp_Quit();
void SDLApp_ToggleFPSOverlay(void);
int SDLApp_GetArmClock(void);
void SDLApp_CycleArmClock(void);
void SDLApp_CycleGameMode(void);
bool SDLApp_IsArcadeGameMode(void);
/* Force console-mode for the current session without touching the config
 * file on disk. Used by netplay: the arcade path skips the menu and uses
 * per-peer DIP switches that would desync, so cold-launch netplay must
 * run through the console menu chain regardless of the user's saved
 * preference. Does not persist across restarts. */
void SDLApp_ForceConsoleGameMode(void);
#if defined(DEBUG)
/* Step B3 EXPERIMENT (docs/plan-fcade-replay-browser.md): session-only
 * arcade-mode force for raw fcade-stream playback under the DEBUG test
 * runner — the Fightcade session ran the arcade program flow, so the
 * stream's inputs are only meaningful against the arcade attract/char-
 * select path. Mirrors SDLApp_ForceConsoleGameMode; never touches the
 * on-disk config. DEBUG builds only. */
void SDLApp_ForceArcadeGameMode(void);
#endif
void SDLApp_CycleHoldToPause(void);
bool SDLApp_IsHoldToPauseEnabled(void);

static inline bool SDLApp_HasPerfTelemetry(void) {
    return ENABLE_PERF_TELEMETRY != 0;
}

#if ENABLE_PERF_TELEMETRY
/// Configure optional frame-stage perf capture.
/// `frame_count` <= 0 disables capture.
void SDLApp_ConfigurePerfCapture(
    int frame_count,
    const char* output_path,
    const char* scene_name,
    bool basic_mode,
    bool basic_first_window_family_snapshots,
    bool basic_first_window_render_subphases,
    bool basic_first_window_exact_hot_family_alpha_offpath,
    bool basic_first_window_onset_exact_hot_family_alpha_offpath,
    bool basic_first_window_onset_cluster_alpha_offpath,
    bool disable_reuse_telemetry,
    bool enable_subrect_alpha_telemetry);
bool SDLApp_IsPerfRuntimeStateActive(const char* runtime_state_name);
#else
static inline void
SDLApp_ConfigurePerfCapture(int frame_count,
                            const char* output_path,
                            const char* scene_name,
                            bool basic_mode,
                            bool basic_first_window_family_snapshots,
                            bool basic_first_window_render_subphases,
                            bool basic_first_window_exact_hot_family_alpha_offpath,
                            bool basic_first_window_onset_exact_hot_family_alpha_offpath,
                            bool basic_first_window_onset_cluster_alpha_offpath,
                            bool disable_reuse_telemetry,
                            bool enable_subrect_alpha_telemetry) {
    (void)frame_count;
    (void)output_path;
    (void)scene_name;
    (void)basic_mode;
    (void)basic_first_window_family_snapshots;
    (void)basic_first_window_render_subphases;
    (void)basic_first_window_exact_hot_family_alpha_offpath;
    (void)basic_first_window_onset_exact_hot_family_alpha_offpath;
    (void)basic_first_window_onset_cluster_alpha_offpath;
    (void)disable_reuse_telemetry;
    (void)enable_subrect_alpha_telemetry;
}

static inline bool SDLApp_IsPerfRuntimeStateActive(const char* runtime_state_name) {
    (void)runtime_state_name;
    return false;
}
#endif

/// @brief Poll SDL events.
/// @return `true` if the main loop should continue running, `false` otherwise.
bool SDLApp_PollEvents();

void SDLApp_BeginFrame();
void SDLApp_EndFrame();
void SDLApp_Exit();

const FrameMetrics* SDLApp_GetFrameMetrics();

#endif
