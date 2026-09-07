#include "port/sdl/sdl_app.h"
#include "common.h"
#include "imgui/imgui_wrapper.h"
#include "main.h"
#include "port/config/config.h"
#include "port/config/config_helpers.h"
#include "port/config/keymap.h"
#include "port/paths.h"
#include "port/sdl/netplay_screen.h"
#include "port/sdl/netstats_renderer.h"
#if defined(ENABLE_NETPLAY)
#include "netplay/game_state.h"
#endif
#include "port/sdl/scanline_renderer.h"
#include "port/sdl/sdl_debug_text.h"
#include "port/sdl/fps_overlay_compositor.h"
#include "port/sdl/native_video_writer.h"
#include "platform/video/software/software_renderer.h"
#if defined(PORT_MIYOO_MINI_PLUS)
/* PORT_ARM_FBDEV also defines CRS_APP_DRIVER_ARM but Cut 1 deliberately
 * leaves the FBDEV/DRM Init/Shutdown wiring dormant — those paths exist
 * for a future port but their ArmDisplay_Present call is also gated on
 * PORT_MIYOO_MINI_PLUS only (see below). Gate Init/Shutdown here on the
 * same flag so the two stay in lock-step. */
#include "platform/app/arm/arm_display.h"
#endif
#include "port/sdl/sdl_message_renderer.h"
#include "port/sdl/sdl_pad.h"
#include "port/sound/adx.h"
#include "sf33rd/AcrSDK/ps2/foundaps2.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/pls01.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/opening/opening.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"
#include "sf33rd/Source/Game/ui/frame_trace.h"
#include "sf33rd/Source/Game/game.h"
#include "sf33rd/Source/Game/rendering/mtrans.h"
#include "sf33rd/Source/Game/rendering/dc_ghost.h"

#include <SDL3/SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(PORT_MISTER)
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
#endif

#if defined(PORT_MIYOO_MINI_PLUS)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#endif

typedef enum ScaleMode {
    SCALEMODE_NATIVE,
    SCALEMODE_NEAREST,
    SCALEMODE_LINEAR,
    SCALEMODE_SOFT_LINEAR,
    SCALEMODE_SQUARE_PIXELS,
    SCALEMODE_INTEGER,
} ScaleMode;

static const char* app_name = "Street Fighter III: 3rd Strike";
static const float display_target_ratio = 4.0 / 3.0;
static const int native_game_width = 384;
static const int native_game_height = 224;
#if defined(PORT_MISTER)
static const int window_min_width = 320;
static const int window_min_height = 240;
#else
static const int window_min_width = 384;
static const int window_min_height = (int)(window_min_width / display_target_ratio);
#endif
static Uint64 target_frame_time_ns = (Uint64)(1000000000.0 / TARGET_FPS);

SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_Texture* screen_texture = NULL;
/* Streaming SDL texture for desktop present of giblet's canvas. Format is
 * RGB565 under PORT_MISTER (CRS_SW_CANVAS_16BPP=1) to match the FPGA scanout
 * format, ARGB8888 elsewhere. Created lazily on first present, released in
 * SDLApp_Quit. */
static SDL_Texture* giblet_present_texture = NULL;
static ScaleMode scale_mode = SCALEMODE_SOFT_LINEAR;
static const char* mister_scale_mode_override_env = "THIRDSARM_SCALE_MODE_STARTUP_OVERRIDE";
static const char* wrapper_scale_mode_explicit_marker = "# thirdsarm-wrapper-scale-mode-explicit";
static const char* wrapper_scale_mode_auto_marker = "# thirdsarm-wrapper-scale-mode-auto";
static char scale_mode_startup_source[32] = "default";
static char scale_mode_requested_value[32] = "";
static SDL_FRect native_output_rect = { 0 };
static bool native_output_rect_dirty = true;
static bool native_output_has_bars = true;
static int native_output_width = 0;
static int native_output_height = 0;
static SDL_FRect screen_output_rect = { 0 };
static bool screen_output_rect_dirty = true;
static bool screen_output_has_letterbox = true;
static int screen_output_width = 0;
static int screen_output_height = 0;
typedef enum FpsOverlayMode {
    FPS_OVERLAY_OFF = 0,
    FPS_OVERLAY_FPS = 1,
    FPS_OVERLAY_DEBUG = 2,
} FpsOverlayMode;
static FpsOverlayMode fps_overlay_mode = FPS_OVERLAY_OFF;
static Uint64 fps_overlay_window_start_ns = 0;
static Uint32 fps_overlay_window_frames = 0;
static int fps_overlay_value = 0;
/* 256 bytes accommodates a two-line debug overlay (line 1 = legacy timing
 * breakdown, line 2 = perf-3 metrics — resolve/raster_pass split + new-path
 * hit rates). The two lines are separated by a single '\n' character which
 * the renderer splits on. */
static char fps_overlay_label[256] = "";

/* Rolling-average timing breakdown for the FPS overlay (accumulated over the
   same 250 ms measurement window used for the FPS counter). */
static Uint64 fps_overlay_update_ns_accum = 0;
static Uint64 fps_overlay_texrefresh_ns_accum = 0;
static Uint64 fps_overlay_gamelogic_ns_accum = 0;
static Uint64 fps_overlay_spritesubmit_ns_accum = 0;
static Uint64 fps_overlay_dispatch_ns_accum = 0;
static Uint64 fps_overlay_dtexrenew_ns_accum = 0;
static Uint64 fps_overlay_dsprsubmit_ns_accum = 0;
static Uint64 fps_overlay_render_ns_accum = 0;
static Uint64 fps_overlay_sort_ns_accum = 0;
static Uint64 fps_overlay_raster_ns_accum = 0;
/* perf-3 line-2 accumulators: resolve/raster_pass split + new-path hit
 * counters. Both are window totals over the same 250 ms window as the legacy
 * timing accumulators. */
static Uint64 fps_overlay_resolve_ns_accum = 0;
static Uint64 fps_overlay_raster_pass_ns_accum = 0;
static Uint64 fps_overlay_packed_8px_hits_accum = 0;
static Uint64 fps_overlay_neon_16px_direct_hits_accum = 0;
static Uint64 fps_overlay_qsort_invocations_accum = 0;
static Uint64 fps_overlay_present_ns_accum = 0;
static Uint64 fps_overlay_frame_ns_accum = 0;
static double fps_overlay_avg_update_ms = 0.0;
static double fps_overlay_avg_texrefresh_ms = 0.0;
static double fps_overlay_avg_gamelogic_ms = 0.0;
static double fps_overlay_avg_spritesubmit_ms = 0.0;
static double fps_overlay_avg_dispatch_ms = 0.0;
static double fps_overlay_avg_dtexrenew_ms = 0.0;
static double fps_overlay_avg_dsprsubmit_ms = 0.0;
static double fps_overlay_avg_render_ms = 0.0;
static double fps_overlay_avg_sort_ms = 0.0;
static double fps_overlay_avg_raster_ms = 0.0;
/* perf-3 line-2 averages. */
static double fps_overlay_avg_resolve_ms = 0.0;
static double fps_overlay_avg_raster_pass_ms = 0.0;
static double fps_overlay_avg_packed_8px_per_frame = 0.0;
static double fps_overlay_avg_neon_16px_per_frame = 0.0;
static double fps_overlay_avg_qsort_per_frame = 0.0;
/* Sticky max-active-effect-count peak watch — used to size GameState's
 * effect work pool ceiling for sparse-save (Option A). Sampled every frame
 * the overlay is in DEBUG mode; reset only when overlay mode toggles or
 * the user reboots. Displayed as the trailing `e<peak>` field of the DEBUG
 * overlay line built by publish_fps_overlay_label(). */
static int fps_overlay_peak_active_effects = 0;
static double fps_overlay_avg_present_ms = 0.0;
static double fps_overlay_avg_frame_ms = 0.0;

static Uint64 frame_deadline = 0;

static Uint64   pacer_max_jitter_ns = 0;
static Uint64   pacer_jitter_sum_ns = 0;
static Uint32   pacer_late_count = 0;
static Uint32   pacer_stats_frames = 0;
static Uint64   pacer_overlay_max_jitter_us = 0;
static Uint64   pacer_overlay_avg_jitter_us = 0;
static Uint32   pacer_overlay_late_pct = 0;
static Uint64   pacer_overlay_phase_us = 0;

/* Vsync feedback state (closed-loop phase lock) */
#if defined(PORT_MISTER)
static uint8_t  last_fpga_frame_cnt = 0;
static uint32_t last_feedback_seq = 0;
static Uint64   last_feedback_update_ns = 0;  /* for staleness detection */
static bool     vsync_feedback_disabled = false;
#endif
static Uint64   last_vsync_monotonic_ns = 0;  /* SDL_GetTicksNS at time of observation */
static bool     vsync_feedback_valid = false;
static Uint64   lead_time_ns = 2000000;       /* default 2ms */
static int64_t  pacer_phase_error_ns = 0;     /* for overlay display */

static FrameMetrics frame_metrics = { 0 };
static Uint64 last_frame_end_time = 0;

static Uint64 last_mouse_motion_time = 0;
static const int mouse_hide_delay_ms = 2000; // 2 seconds
static bool native_video_writer_enabled = false;
static bool use_native_render_path = false;
static bool software_frame_mode_enabled = false;
typedef enum SuperEffectQualityMode {
    SUPER_EFFECT_QUALITY_FULL = 0,
    SUPER_EFFECT_QUALITY_CACHED_BG = 1,
} SuperEffectQualityMode;
static SuperEffectQualityMode super_effect_quality_mode = SUPER_EFFECT_QUALITY_FULL;
/* arm_clock: ARM CPU clock mode (0=stock 800MHz, 1=1000MHz, 2=1200MHz).
   Applied via sysfs scaling_max_freq.  Reset to stock on exit. */
static int arm_clock_mode = 0;
static bool game_mode_arcade = false;
static bool hold_to_pause = false;
#if ENABLE_PERF_TELEMETRY
typedef struct PerfCaptureDemoLogoState {
    int effect_index;
    int routine2;
    int direction;
    int dir_timer;
} PerfCaptureDemoLogoState;

static bool perf_capture_enabled = false;
static bool perf_capture_completed = false;
static bool perf_capture_basic_mode = false;
static bool perf_capture_basic_first_window_family_snapshots_enabled = false;
static bool perf_capture_basic_first_window_render_subphases_enabled = false;
static bool perf_capture_basic_first_window_exact_hot_family_alpha_offpath_enabled = false;
static bool perf_capture_basic_first_window_onset_exact_hot_family_alpha_offpath_enabled = false;
static bool perf_capture_basic_first_window_onset_cluster_alpha_offpath_enabled = false;
static bool perf_capture_fast_non_integer_reuse_telemetry_enabled = true;
static bool perf_capture_fast_non_integer_subrect_alpha_telemetry_enabled = false;
static int perf_capture_target_frames = 0;
static int perf_capture_recorded_frames = 0;
static char* perf_capture_output_path = NULL;
static char* perf_capture_scene_name = NULL;
static Uint64 perf_frame_start_ns = 0;
/* Monotonic frame ordinal since process start, for correlating a logged
 * outlier with whatever else the same frame emitted (an AFS load failure,
 * say). Deliberately NOT reset by perf-capture start/stop -- a capture
 * window is not the timeline the log is read against. */
static Uint64 perf_frame_index = 0;
static Uint64 perf_update_start_ns = 0;
static Uint64 perf_update_ns_total = 0;
static Uint64 perf_render_ns_total = 0;
/* perf-3 piece-A: per-window totals for the resolve/raster_pass split inside
 * render_frame_to_software_surface(). Both reset alongside the other window
 * totals; emitted in the perf-capture JSON. */
static Uint64 perf_resolve_ns_total = 0;
static Uint64 perf_raster_pass_ns_total = 0;
static Uint64 perf_sort_qsort_invocations_total = 0;
static Uint64 perf_present_ns_total = 0;
static Uint64 perf_frame_work_ns_total = 0;
static Uint64 perf_dirty_tiles_total = 0;
static Uint64 perf_render_task_count_total = 0;
static Uint64 perf_rect_copy_tasks_total = 0;
static Uint64 perf_batch_runs_total = 0;
static Uint64 perf_batched_task_count_total = 0;
static Uint64 perf_rect_texture_runs_total = 0;
static Uint64 perf_rect_texture_multi_runs_total = 0;
static Uint64 perf_rect_texture_multi_run_tasks_total = 0;
static Uint64 perf_rect_texture_max_run_total = 0;
static Uint64 perf_rect_texture_hstrip_runs_total = 0;
static Uint64 perf_rect_texture_hstrip_tasks_total = 0;
static Uint64 perf_rect_texture_vstrip_runs_total = 0;
static Uint64 perf_rect_texture_vstrip_tasks_total = 0;
static Uint64 perf_rect_texture_run_links_total = 0;
static Uint64 perf_rect_texture_color_breaks_total = 0;
static Uint64 perf_rect_texture_flip_breaks_total = 0;
static Uint64 perf_rect_texture_flipped_tasks_total = 0;
static Uint64 perf_textured_geometry_tasks_total = 0;
static Uint64 perf_textured_geometry_rect_recovered_tasks_total = 0;
static Uint64 perf_textured_geometry_fallback_tasks_total = 0;
static Uint64 perf_set_texture_calls_total = 0;
static Uint64 perf_texture_binding_reuse_hits_total = 0;
static Uint64 perf_texture_cache_hits_total = 0;
static Uint64 perf_texture_cache_misses_total = 0;
static Uint64 perf_texture_cache_miss_dirty_texture_same_frame_total = 0;
static Uint64 perf_texture_cache_miss_dirty_texture_carried_total = 0;
static Uint64 perf_texture_cache_miss_dirty_palette_same_frame_total = 0;
static Uint64 perf_texture_cache_miss_dirty_palette_carried_total = 0;
static Uint64 perf_texture_cache_miss_cold_total = 0;
static Uint64 perf_texture_creates_total = 0;
static Uint64 perf_texture_unlock_calls_total = 0;
static Uint64 perf_palette_unlock_calls_total = 0;
static Uint64 perf_palette_unlock_changed_calls_total = 0;
static Uint64 perf_palette_unlock_unchanged_calls_total = 0;
static Uint64 perf_texture_unlock_dirty_surface_variants_total = 0;
static Uint64 perf_texture_unlock_dirty_surface_variants_max_total = 0;
static Uint64 perf_palette_unlock_dirty_surface_variants_total = 0;
static Uint64 perf_palette_unlock_dirty_surface_variants_max_total = 0;
static Uint64 perf_texture_unlock_locality_index8_tracked_total = 0;
static Uint64 perf_texture_unlock_locality_index8_baseline_skips_total = 0;
static Uint64 perf_texture_unlock_locality_index8_non_index8_skips_total = 0;
static Uint64 perf_texture_unlock_locality_index8_source_pixels_total = 0;
static Uint64 perf_texture_unlock_locality_index8_changed_pixels_total = 0;
static Uint64 perf_texture_unlock_locality_index8_changed_rows_total = 0;
static Uint64 perf_texture_unlock_locality_index8_changed_bbox_pixels_total = 0;
static Uint64 perf_texture_unlock_invalidation_ns_total = 0;
static Uint64 perf_palette_unlock_invalidation_ns_total = 0;
static Uint64 perf_texture_cache_evictions_total = 0;
static Uint64 perf_palette_cache_evictions_total = 0;
static Uint64 perf_software_surface_cache_hits_total = 0;
static Uint64 perf_software_surface_cache_creates_total = 0;
static Uint64 perf_software_surface_cache_refresh_attempts_total = 0;
static Uint64 perf_software_surface_cache_refresh_unique_bindings_total = 0;
static Uint64 perf_software_surface_cache_refresh_repeat_binding_attempts_total = 0;
static Uint64 perf_software_surface_cache_refresh_unique_texture_handles_total = 0;
static Uint64 perf_software_surface_cache_refresh_texture_handle_fanout_max_total = 0;
static Uint64 perf_software_surface_cache_refresh_failures_total = 0;
static Uint64 perf_software_surface_cache_refresh_ns_total = 0;
static Uint64 perf_software_surface_cache_refresh_palette_set_calls_total = 0;
static Uint64 perf_software_surface_cache_refresh_palette_set_ns_total = 0;
static Uint64 perf_software_surface_cache_refresh_blit_calls_total = 0;
static Uint64 perf_software_surface_cache_refresh_blit_ns_total = 0;
static Uint64 perf_software_surface_cache_create_dirty_texture_same_frame_total = 0;
static Uint64 perf_software_surface_cache_create_dirty_texture_carried_total = 0;
static Uint64 perf_software_surface_cache_create_dirty_palette_same_frame_total = 0;
static Uint64 perf_software_surface_cache_create_dirty_palette_carried_total = 0;
static Uint64 perf_software_surface_cache_create_cold_total = 0;
static Uint64 perf_software_surface_cache_texture_evictions_total = 0;
static Uint64 perf_software_surface_cache_palette_evictions_total = 0;
static Uint64 perf_textures_destroy_queued_total = 0;
static Uint64 perf_unknown_tasks_total = 0;
static Uint64 perf_ppg_tasks_total = 0;
static Uint64 perf_mtrans_tasks_total = 0;
static Uint64 perf_ui_direct_tasks_total = 0;
static Uint64 perf_solid_tasks_total = 0;
static Uint64 perf_hybrid_candidate_tasks_total = 0;
static Uint64 perf_hybrid_candidate_pixels_total = 0;
static Uint64 perf_hybrid_fallback_tasks_total = 0;
static Uint64 perf_hybrid_fallback_pixels_total = 0;
static Uint64 perf_hybrid_reason_clip_total = 0;
static Uint64 perf_hybrid_reason_alpha_total = 0;
static Uint64 perf_hybrid_reason_color_mod_total = 0;
static Uint64 perf_hybrid_reason_flip_total = 0;
static Uint64 perf_hybrid_reason_geometry_total = 0;
static Uint64 perf_hybrid_reason_solid_total = 0;
static Uint64 perf_super_art_stock_available_frames[2] = { 0 };
static int perf_super_art_stock_available_first_frame[2] = { -1, -1 };
static int perf_super_art_stock_max_reached[2] = { 0, 0 };
static int perf_super_art_stock_capacity[2] = { 0, 0 };
static int perf_super_art_gauge_max_value[2] = { 0, 0 };
static int perf_super_art_gauge_capacity[2] = { 0, 0 };
static Uint64 perf_super_art_ready_frames[2] = { 0 };
static int perf_super_art_ready_first_frame[2] = { -1, -1 };
enum {
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_0 = 0,
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_1 = 1,
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_2 = 2,
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_3 = 3,
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_4 = 4,
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_OTHER = 5,
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_COUNT = 6,
};
static Uint64 perf_super_art_ready_routine1_frames[2][PERF_SUPER_ART_READY_ROUTINE1_BUCKET_COUNT] = { { 0 } };
static int perf_super_art_ready_first_routine[2][3] = { { -1, -1, -1 }, { -1, -1, -1 } };
static int perf_super_art_ready_last_routine[2][3] = { { -1, -1, -1 }, { -1, -1, -1 } };
static Uint64 perf_super_art_active_frames[2] = { 0 };
static int perf_super_art_active_first_frame[2] = { -1, -1 };
static Uint64 perf_super_art_active_starts[2] = { 0 };
static bool perf_super_art_active_was_active[2] = { false, false };
static Uint64 perf_metamorphose_active_frames[2] = { 0 };
static int perf_metamorphose_active_first_frame[2] = { -1, -1 };
static int perf_capture_start_g_no[4] = { 0, 0, 0, 0 };
static int perf_capture_start_e_no[4] = { 0, 0, 0, 0 };
static int perf_capture_start_d_no[4] = { 0, 0, 0, 0 };
static int perf_capture_start_menu_task_condition = 0;
static int perf_capture_start_menu_task_r_no[4] = { 0, 0, 0, 0 };
static int perf_capture_start_break_into = 0;
static int perf_capture_start_hnc_num = 0;
static int perf_capture_start_exec_wipe = 0;
static int perf_capture_start_active_wipe_type = -1;
static int perf_capture_start_wipe_limit = 0;
static int perf_capture_start_sel_pl_complete[2] = { 0, 0 };
static int perf_capture_start_sel_arts_complete[2] = { 0, 0 };
static int perf_capture_start_select_arts[2] = { 0, 0 };
static int perf_capture_start_moving_plate[2] = { 0, 0 };
static int perf_capture_start_moving_plate_counter[2] = { 0, 0 };
static int perf_capture_start_command_name_visible[2] = { 0, 0 };
static int perf_capture_start_title_tex_flag = 0;
static int perf_capture_start_opening_r_no_0 = 0;
static int perf_capture_start_opening_r_no_1 = 0;
static int perf_capture_start_opening_r_no_2 = 0;
static int perf_capture_start_opening_free_work = 0;
static int perf_capture_start_demo_flag = 0;
static int perf_capture_start_demo_logo_effect_index = -1;
static int perf_capture_start_demo_logo_routine2 = -1;
static int perf_capture_start_demo_logo_direction = -1;
static int perf_capture_start_demo_logo_dir_timer = -1;
static Uint64 perf_break_into_frames_total = 0;
static int perf_break_into_first_frame = -1;
static Uint64 perf_hnc_active_frames_total = 0;
static int perf_hnc_active_first_frame = -1;
static int perf_hnc_max_num = 0;
static Uint64 perf_wipe_type1_active_frames_total = 0;
static int perf_wipe_type1_active_first_frame = -1;
static int perf_wipe_type1_max_limit = 0;
static Uint64 perf_title_logo_active_frames_total = 0;
static int perf_title_logo_active_first_frame = -1;
static Uint64 perf_demo_logo_active_frames_total = 0;
static int perf_demo_logo_active_first_frame = -1;
static int perf_demo_logo_max_direction = -1;
static Uint64 perf_software_frame_mode_enabled_frames = 0;
static Uint64 perf_software_frame_surface_ready_frames = 0;
static Uint64 perf_software_frame_owned_frames = 0;
static Uint64 perf_software_frame_direct_present_frames = 0;
static Uint64 perf_software_frame_uploaded_frames = 0;
static Uint64 perf_software_frame_fallback_frames = 0;
static Uint64 perf_software_frame_candidate_tasks_total = 0;
static Uint64 perf_software_frame_candidate_pixels_total = 0;
static Uint64 perf_software_frame_fallback_tasks_total = 0;
static Uint64 perf_software_frame_fallback_pixels_total = 0;
static Uint64 perf_software_frame_fast_exact_tasks_total = 0;
static Uint64 perf_software_frame_fast_exact_pixels_total = 0;
static Uint64 perf_software_frame_fast_exact_clipped_tasks_total = 0;
static Uint64 perf_software_frame_fast_exact_flipped_tasks_total = 0;
static Uint64 perf_software_frame_fast_exact_color_mod_tasks_total = 0;
static Uint64 perf_software_frame_fast_exact_color_mod_pixels_total = 0;
static Uint64 perf_software_frame_fast_scaled_tasks_total = 0;
static Uint64 perf_software_frame_fast_scaled_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_tasks_total = 0;
static Uint64 perf_software_frame_fast_non_integer_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_lookup_entries_total = 0;
static Uint64 perf_software_frame_fast_non_integer_source_alpha_opaque_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_source_alpha_transparent_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_source_alpha_blended_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_same_source_runs_total = 0;
static Uint64 perf_software_frame_fast_non_integer_same_source_reuse_runs_total = 0;
static Uint64 perf_software_frame_fast_non_integer_same_source_reused_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_same_source_opaque_reused_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_same_source_transparent_reused_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_same_source_blended_reused_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_same_source_max_run_length_total = 0;
static Uint64 perf_software_frame_fast_non_integer_alpha_only_tasks_total = 0;
static Uint64 perf_software_frame_fast_non_integer_alpha_only_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_rgb_mod_tasks_total = 0;
static Uint64 perf_software_frame_fast_non_integer_rgb_mod_pixels_total = 0;
static Uint64 perf_software_frame_generic_textured_tasks_total = 0;
static Uint64 perf_software_frame_generic_textured_pixels_total = 0;
static Uint64 perf_software_frame_generic_textured_alpha_only_tasks_total = 0;
static Uint64 perf_software_frame_generic_textured_alpha_only_pixels_total = 0;
static Uint64 perf_software_frame_generic_textured_rgb_mod_tasks_total = 0;
static Uint64 perf_software_frame_generic_textured_rgb_mod_pixels_total = 0;
static Uint64 perf_software_frame_fast_miss_color_mod_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_lookup_entries_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_ge_256_tasks_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_ge_256_pixels_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_ge_256_lookup_entries_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_ge_1024_tasks_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_ge_1024_pixels_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_max_pixels_total = 0;
static Uint64 perf_software_frame_fast_miss_scaled_total = 0;
static Uint64 perf_software_frame_fast_miss_unsupported_flip_total = 0;
static Uint64 perf_software_frame_fast_miss_source_bounds_total = 0;
static Uint64 perf_software_frame_reason_alpha_total = 0;
static Uint64 perf_software_frame_reason_color_mod_total = 0;
static Uint64 perf_software_frame_reason_geometry_total = 0;
static Uint64 perf_software_frame_reason_solid_total = 0;
enum {
    perf_capture_first_burst_frame_limit = 8,
    perf_capture_first_window_frame_limit = 60,
    perf_capture_first_window_fast_non_integer_family_capacity = 64,
    perf_capture_first_window_fast_non_integer_shared_shape_capacity = 32,
    perf_capture_first_window_fast_non_integer_lookup_profile_capacity = 32,
    perf_capture_first_window_generic_textured_family_capacity = 8,
};

typedef struct PerfFrameSample {
    double frame_time_ms;
    double update_ms;
    double render_ms;
    double present_ms;
    int dirty_tiles;
    double dirty_ratio;
    int render_task_count;
    int rect_copy_tasks;
    int batch_runs;
    int batched_task_count;
    int rect_texture_runs;
    int rect_texture_multi_runs;
    int rect_texture_multi_run_tasks;
    int rect_texture_max_run;
    int rect_texture_hstrip_runs;
    int rect_texture_hstrip_tasks;
    int rect_texture_vstrip_runs;
    int rect_texture_vstrip_tasks;
    int rect_texture_run_links;
    int rect_texture_color_breaks;
    int rect_texture_flip_breaks;
    int rect_texture_flipped_tasks;
    int textured_geometry_tasks;
    int textured_geometry_rect_recovered_tasks;
    int textured_geometry_fallback_tasks;
    int set_texture_calls;
    int texture_binding_reuse_hits;
    int texture_cache_hits;
    int texture_cache_misses;
    int texture_cache_miss_dirty_texture_same_frame;
    int texture_cache_miss_dirty_texture_carried;
    int texture_cache_miss_dirty_palette_same_frame;
    int texture_cache_miss_dirty_palette_carried;
    int texture_cache_miss_cold;
    int texture_creates;
    int texture_unlock_calls;
    int palette_unlock_calls;
    int palette_unlock_changed_calls;
    int palette_unlock_unchanged_calls;
    int texture_unlock_dirty_surface_variants;
    int texture_unlock_dirty_surface_variants_max;
    int palette_unlock_dirty_surface_variants;
    int palette_unlock_dirty_surface_variants_max;
    int texture_unlock_locality_index8_tracked;
    int texture_unlock_locality_index8_baseline_skips;
    int texture_unlock_locality_index8_non_index8_skips;
    Uint64 texture_unlock_locality_index8_source_pixels;
    Uint64 texture_unlock_locality_index8_changed_pixels;
    Uint64 texture_unlock_locality_index8_changed_rows;
    Uint64 texture_unlock_locality_index8_changed_bbox_pixels;
    double texture_unlock_invalidation_ms;
    double palette_unlock_invalidation_ms;
    int texture_cache_evictions;
    int palette_cache_evictions;
    int software_surface_cache_hits;
    int software_surface_cache_creates;
    int software_surface_cache_refresh_attempts;
    int software_surface_cache_refresh_unique_bindings;
    int software_surface_cache_refresh_repeat_binding_attempts;
    int software_surface_cache_refresh_unique_texture_handles;
    int software_surface_cache_refresh_texture_handle_fanout_max;
    int software_surface_cache_refresh_failures;
    double software_surface_cache_refresh_ms;
    int software_surface_cache_refresh_palette_set_calls;
    double software_surface_cache_refresh_palette_set_ms;
    int software_surface_cache_refresh_blit_calls;
    double software_surface_cache_refresh_blit_ms;
    int software_surface_cache_create_dirty_texture_same_frame;
    int software_surface_cache_create_dirty_texture_carried;
    int software_surface_cache_create_dirty_palette_same_frame;
    int software_surface_cache_create_dirty_palette_carried;
    int software_surface_cache_create_cold;
    int software_surface_cache_texture_evictions;
    int software_surface_cache_palette_evictions;
    int textures_destroy_queued;
    int unknown_tasks;
    int ppg_tasks;
    int mtrans_tasks;
    int ui_direct_tasks;
    int solid_tasks;
    int software_frame_mode_enabled;
    int software_frame_surface_ready;
    int software_frame_owned;
    int software_frame_direct_present;
    int software_frame_uploaded;
    int software_frame_fallback;
    int software_frame_candidate_tasks;
    Uint64 software_frame_candidate_pixels;
    int software_frame_fallback_tasks;
    Uint64 software_frame_fallback_pixels;
    int software_frame_fast_exact_tasks;
    Uint64 software_frame_fast_exact_pixels;
    int software_frame_fast_exact_clipped_tasks;
    int software_frame_fast_exact_flipped_tasks;
    int software_frame_fast_exact_color_mod_tasks;
    Uint64 software_frame_fast_exact_color_mod_pixels;
    int software_frame_fast_scaled_tasks;
    Uint64 software_frame_fast_scaled_pixels;
    int software_frame_fast_non_integer_tasks;
    Uint64 software_frame_fast_non_integer_pixels;
    Uint64 software_frame_fast_non_integer_lookup_entries;
    Uint64 software_frame_fast_non_integer_source_alpha_opaque_pixels;
    Uint64 software_frame_fast_non_integer_source_alpha_transparent_pixels;
    Uint64 software_frame_fast_non_integer_source_alpha_blended_pixels;
    Uint64 software_frame_fast_non_integer_same_source_runs;
    Uint64 software_frame_fast_non_integer_same_source_reuse_runs;
    Uint64 software_frame_fast_non_integer_same_source_reused_pixels;
    Uint64 software_frame_fast_non_integer_same_source_opaque_reused_pixels;
    Uint64 software_frame_fast_non_integer_same_source_transparent_reused_pixels;
    Uint64 software_frame_fast_non_integer_same_source_blended_reused_pixels;
    int software_frame_fast_non_integer_same_source_max_run_length;
    int software_frame_fast_non_integer_alpha_only_tasks;
    Uint64 software_frame_fast_non_integer_alpha_only_pixels;
    int software_frame_fast_non_integer_rgb_mod_tasks;
    Uint64 software_frame_fast_non_integer_rgb_mod_pixels;
    int software_frame_generic_textured_tasks;
    Uint64 software_frame_generic_textured_pixels;
    int software_frame_generic_textured_alpha_only_tasks;
    Uint64 software_frame_generic_textured_alpha_only_pixels;
    int software_frame_generic_textured_rgb_mod_tasks;
    Uint64 software_frame_generic_textured_rgb_mod_pixels;
    int software_frame_fast_miss_color_mod;
    int software_frame_fast_miss_non_integer;
    Uint64 software_frame_fast_miss_non_integer_lookup_entries;
    int software_frame_fast_miss_non_integer_ge_256_tasks;
    Uint64 software_frame_fast_miss_non_integer_ge_256_pixels;
    Uint64 software_frame_fast_miss_non_integer_ge_256_lookup_entries;
    int software_frame_fast_miss_non_integer_ge_1024_tasks;
    Uint64 software_frame_fast_miss_non_integer_ge_1024_pixels;
    Uint64 software_frame_fast_miss_non_integer_max_pixels;
    int software_frame_fast_miss_scaled;
    int software_frame_fast_miss_unsupported_flip;
    int software_frame_fast_miss_source_bounds;
    int software_frame_reason_alpha;
    int software_frame_reason_color_mod;
    int software_frame_reason_geometry;
    int software_frame_reason_solid;
    int hybrid_candidate_tasks;
    Uint64 hybrid_candidate_pixels;
    int hybrid_fallback_tasks;
    Uint64 hybrid_fallback_pixels;
    int hybrid_reason_clip;
    int hybrid_reason_alpha;
    int hybrid_reason_color_mod;
    int hybrid_reason_flip;
    int hybrid_reason_geometry;
    int hybrid_reason_solid;
} PerfFrameSample;

static PerfFrameSample* perf_samples = NULL;

typedef struct PerfCaptureWindowSummary {
    int frame_count;
    double fps;
    double frame_time_ms;
    double update_ms;
    double render_ms;
    double present_ms;
    double software_frame_fast_non_integer_pixels;
    double software_frame_generic_textured_pixels;
} PerfCaptureWindowSummary;

/* Phase D: post-rip stub for the legacy first-window snapshot. The fields
 * are gone with the legacy renderer; callers only construct/zero the struct. */
typedef struct PerfCaptureFirstWindowFamilySnapshot {
    bool valid;
} PerfCaptureFirstWindowFamilySnapshot;

static PerfCaptureFirstWindowFamilySnapshot perf_capture_first_window_snapshot = { 0 };
static PerfCaptureFirstWindowFamilySnapshot perf_capture_first_burst_snapshot = { 0 };
#endif

#if defined(PORT_MISTER)
static const char* legacy_mister_video_driver_order = "kmsdrm,offscreen,dummy";
static const char* recommended_mister_video_driver_order = "evdev,dummy,offscreen";
static const char* recommended_mister_audio_driver = "alsa";
#endif

#if ENABLE_PERF_TELEMETRY
static void perf_capture_reset_storage(void);
static void perf_capture_write_summary(void);
static void perf_capture_reset_window_snapshot(PerfCaptureFirstWindowFamilySnapshot* snapshot) {
    if (snapshot != NULL) {
        SDL_zero(*snapshot);
    }
}
#endif
static const char* scale_mode_name(ScaleMode mode);
static const char* software_frame_mode_name(void);
static const char* super_effect_quality_mode_name(SuperEffectQualityMode mode);
static SuperEffectQualityMode current_renderer_super_effect_quality_mode(void);

#if ENABLE_PERF_TELEMETRY
#endif

static void append_backend_log_line(const char* line) {
    const char* pref_path = Paths_GetPrefPath();
    char* logs_dir = NULL;
    char* log_path = NULL;
    SDL_asprintf(&logs_dir, "%slogs", pref_path);
    SDL_CreateDirectory(logs_dir);
    SDL_asprintf(&log_path, "%s/backend.log", logs_dir);

    SDL_IOStream* io = SDL_IOFromFile(log_path, "a");

    if (io != NULL) {
        SDL_WriteIO(io, line, SDL_strlen(line));
        SDL_WriteIO(io, "\n", 1);
        SDL_CloseIO(io);
    }

    SDL_free(logs_dir);
    SDL_free(log_path);
}

static void backend_logf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char* buf = NULL;
    SDL_vasprintf(&buf, fmt, args);
    va_end(args);

    if (buf == NULL) {
        return;
    }

    SDL_Log("%s", buf);
    append_backend_log_line(buf);
    SDL_free(buf);
}

#if ENABLE_PERF_TELEMETRY
static int get_perf_capture_super_art_active(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || plw[player].sa == NULL) {
        return 0;
    }

    return plw[player].sa->ok == -1 ? 1 : 0;
}

static int get_perf_capture_super_art_stock(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || plw[player].sa == NULL) {
        return 0;
    }

    return plw[player].sa->store;
}

static int get_perf_capture_super_art_stock_capacity(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || plw[player].sa == NULL) {
        return 0;
    }

    return plw[player].sa->store_max;
}

static int get_perf_capture_super_art_ready(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || plw[player].sa == NULL) {
        return 0;
    }

    return plw[player].sa->ok == 1 ? 1 : 0;
}

static int get_perf_capture_super_art_gauge_value(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || plw[player].sa == NULL) {
        return 0;
    }

    return plw[player].sa->gauge.s.h;
}

static int get_perf_capture_super_art_gauge_capacity(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || plw[player].sa == NULL) {
        return 0;
    }

    return plw[player].sa->gauge_len;
}

static int get_perf_capture_metamorphose_active(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2) {
        return 0;
    }

    return plw[player].metamorphose != 0 ? 1 : 0;
}

static int get_perf_capture_player_routine_component(int player, int component) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || component < 1 || component > 3) {
        return -1;
    }

    return plw[player].wu.routine_no[component];
}

static int classify_perf_capture_ready_routine1_bucket(int routine1) {
    if (routine1 >= PERF_SUPER_ART_READY_ROUTINE1_BUCKET_0 && routine1 <= PERF_SUPER_ART_READY_ROUTINE1_BUCKET_4) {
        return routine1;
    }

    return PERF_SUPER_ART_READY_ROUTINE1_BUCKET_OTHER;
}

static bool get_perf_capture_demo_logo_state(PerfCaptureDemoLogoState* state) {
    if (state != NULL) {
        state->effect_index = -1;
        state->routine2 = -1;
        state->direction = -1;
        state->dir_timer = -1;
    }

    for (int i = 0; i < EFFECT_MAX; i++) {
        const WORK* work = (const WORK*)frw[i];
        if (work->be_flag == 0 || work->dead_f != 0) {
            continue;
        }
        if (work->id != 58 || work->work_id != 16 || work->routine_no[1] != 10) {
            continue;
        }

        const WORK_Other* effect = (const WORK_Other*)work;
        if (effect->wu.direction <= 0) {
            continue;
        }

        if (state != NULL) {
            state->effect_index = i;
            state->routine2 = effect->wu.routine_no[2];
            state->direction = effect->wu.direction;
            state->dir_timer = effect->wu.dir_timer;
        }
        return true;
    }

    return false;
}

static bool perf_capture_title_logo_active(void) {
    return title_tex_flag != 0;
}

static int get_perf_capture_command_name_visible(int player_index) {
    return (Disp_Command_Name[player_index][0] != 0) || (Disp_Command_Name[player_index][1] != 0) ||
                   (Disp_Command_Name[player_index][2] != 0)
               ? 1
               : 0;
}

static bool perf_capture_character_select_super_art_active(void) {
    for (int player = 0; player < 2; player++) {
        if (Sel_PL_Complete[player] != 1 || Sel_Arts_Complete[player] != 0 || Select_Arts[player] != 3 ||
            Moving_Plate[player] != 0 || Moving_Plate_Counter[player] != 0 ||
            !get_perf_capture_command_name_visible(player)) {
            return false;
        }
    }

    return true;
}

bool SDLApp_IsPerfRuntimeStateActive(const char* runtime_state_name) {
    if (runtime_state_name == NULL) {
        return false;
    }

    if (SDL_strcmp(runtime_state_name, "attract-demo-logo") == 0) {
        return get_perf_capture_demo_logo_state(NULL);
    }

    if (SDL_strcmp(runtime_state_name, "character-select-super-art") == 0) {
        return perf_capture_character_select_super_art_active();
    }

    return false;
}

static void snapshot_perf_capture_transition_start_state(void) {
    const struct _TASK* menu_task = &task[TASK_MENU];
    PerfCaptureDemoLogoState demo_logo_state;
    const bool demo_logo_active = get_perf_capture_demo_logo_state(&demo_logo_state);

    for (int i = 0; i < 4; i++) {
        perf_capture_start_g_no[i] = G_No[i];
        perf_capture_start_e_no[i] = E_No[i];
        perf_capture_start_d_no[i] = D_No[i];
        perf_capture_start_menu_task_r_no[i] = menu_task->r_no[i];
    }

    perf_capture_start_menu_task_condition = menu_task->condition;
    perf_capture_start_break_into = Break_Into;
    perf_capture_start_hnc_num = Hnc_Num;
    perf_capture_start_exec_wipe = Exec_Wipe;
    perf_capture_start_active_wipe_type = Active_Wipe_Type;
    perf_capture_start_wipe_limit = WipeLimit;
    for (int player = 0; player < 2; player++) {
        perf_capture_start_sel_pl_complete[player] = Sel_PL_Complete[player];
        perf_capture_start_sel_arts_complete[player] = Sel_Arts_Complete[player];
        perf_capture_start_select_arts[player] = Select_Arts[player];
        perf_capture_start_moving_plate[player] = Moving_Plate[player];
        perf_capture_start_moving_plate_counter[player] = Moving_Plate_Counter[player];
        perf_capture_start_command_name_visible[player] = get_perf_capture_command_name_visible(player);
    }
    perf_capture_start_title_tex_flag = title_tex_flag;
    perf_capture_start_opening_r_no_0 = op_w.r_no_0;
    perf_capture_start_opening_r_no_1 = op_w.r_no_1;
    perf_capture_start_opening_r_no_2 = op_w.r_no_2;
    perf_capture_start_opening_free_work = op_w.free_work;
    perf_capture_start_demo_flag = Demo_Flag;
    perf_capture_start_demo_logo_effect_index = demo_logo_active ? demo_logo_state.effect_index : -1;
    perf_capture_start_demo_logo_routine2 = demo_logo_active ? demo_logo_state.routine2 : -1;
    perf_capture_start_demo_logo_direction = demo_logo_active ? demo_logo_state.direction : -1;
    perf_capture_start_demo_logo_dir_timer = demo_logo_active ? demo_logo_state.dir_timer : -1;
}

static void note_perf_capture_transition_state(int frame_index) {
    if (Break_Into != 0) {
        perf_break_into_frames_total += 1;
        if (perf_break_into_first_frame < 0) {
            perf_break_into_first_frame = frame_index;
        }
    }

    if (Hnc_Num > 0) {
        perf_hnc_active_frames_total += 1;
        if (perf_hnc_active_first_frame < 0) {
            perf_hnc_active_first_frame = frame_index;
        }
    }

    if (Hnc_Num > perf_hnc_max_num) {
        perf_hnc_max_num = Hnc_Num;
    }

    if (Exec_Wipe != 0 && Active_Wipe_Type == 1 && WipeLimit < 8) {
        perf_wipe_type1_active_frames_total += 1;
        if (perf_wipe_type1_active_first_frame < 0) {
            perf_wipe_type1_active_first_frame = frame_index;
        }
        if (WipeLimit > perf_wipe_type1_max_limit) {
            perf_wipe_type1_max_limit = WipeLimit;
        }
    }
}

static void note_perf_capture_title_state(int frame_index) {
    if (perf_capture_title_logo_active()) {
        perf_title_logo_active_frames_total += 1;
        if (perf_title_logo_active_first_frame < 0) {
            perf_title_logo_active_first_frame = frame_index;
        }
    }
}

static void note_perf_capture_demo_logo_state(int frame_index) {
    PerfCaptureDemoLogoState demo_logo_state;
    if (!get_perf_capture_demo_logo_state(&demo_logo_state)) {
        return;
    }

    perf_demo_logo_active_frames_total += 1;
    if (perf_demo_logo_active_first_frame < 0) {
        perf_demo_logo_active_first_frame = frame_index;
    }
    if (demo_logo_state.direction > perf_demo_logo_max_direction) {
        perf_demo_logo_max_direction = demo_logo_state.direction;
    }
}

static void note_perf_capture_test_state(int frame_index) {
    note_perf_capture_transition_state(frame_index);
    note_perf_capture_title_state(frame_index);
    note_perf_capture_demo_logo_state(frame_index);
    for (int player = 0; player < 2; player++) {
        const int super_art_stock = get_perf_capture_super_art_stock(player);
        const int super_art_stock_capacity = get_perf_capture_super_art_stock_capacity(player);
        const int super_art_gauge_value = get_perf_capture_super_art_gauge_value(player);
        const int super_art_gauge_capacity = get_perf_capture_super_art_gauge_capacity(player);

        if (super_art_stock > 0) {
            perf_super_art_stock_available_frames[player] += 1;
            if (perf_super_art_stock_available_first_frame[player] < 0) {
                perf_super_art_stock_available_first_frame[player] = frame_index;
            }
        }

        if (super_art_stock > perf_super_art_stock_max_reached[player]) {
            perf_super_art_stock_max_reached[player] = super_art_stock;
        }
        if (super_art_stock_capacity > perf_super_art_stock_capacity[player]) {
            perf_super_art_stock_capacity[player] = super_art_stock_capacity;
        }
        if (super_art_gauge_value > perf_super_art_gauge_max_value[player]) {
            perf_super_art_gauge_max_value[player] = super_art_gauge_value;
        }
        if (super_art_gauge_capacity > perf_super_art_gauge_capacity[player]) {
            perf_super_art_gauge_capacity[player] = super_art_gauge_capacity;
        }

        if (get_perf_capture_super_art_ready(player)) {
            const int routine1 = get_perf_capture_player_routine_component(player, 1);
            const int routine2 = get_perf_capture_player_routine_component(player, 2);
            const int routine3 = get_perf_capture_player_routine_component(player, 3);

            perf_super_art_ready_frames[player] += 1;
            perf_super_art_ready_routine1_frames[player][classify_perf_capture_ready_routine1_bucket(routine1)] += 1;
            if (perf_super_art_ready_first_frame[player] < 0) {
                perf_super_art_ready_first_frame[player] = frame_index;
                perf_super_art_ready_first_routine[player][0] = routine1;
                perf_super_art_ready_first_routine[player][1] = routine2;
                perf_super_art_ready_first_routine[player][2] = routine3;
            }
            perf_super_art_ready_last_routine[player][0] = routine1;
            perf_super_art_ready_last_routine[player][1] = routine2;
            perf_super_art_ready_last_routine[player][2] = routine3;
        }

        if (get_perf_capture_super_art_active(player)) {
            if (!perf_super_art_active_was_active[player]) {
                perf_super_art_active_starts[player] += 1;
            }
            perf_super_art_active_frames[player] += 1;
            if (perf_super_art_active_first_frame[player] < 0) {
                perf_super_art_active_first_frame[player] = frame_index;
            }
            perf_super_art_active_was_active[player] = true;
        } else {
            perf_super_art_active_was_active[player] = false;
        }

        if (get_perf_capture_metamorphose_active(player)) {
            perf_metamorphose_active_frames[player] += 1;
            if (perf_metamorphose_active_first_frame[player] < 0) {
                perf_metamorphose_active_first_frame[player] = frame_index;
            }
        }
    }
}

static void perf_capture_reset_storage(void) {
    if (perf_samples != NULL) {
        SDL_free(perf_samples);
        perf_samples = NULL;
    }

    if (perf_capture_output_path != NULL) {
        SDL_free(perf_capture_output_path);
        perf_capture_output_path = NULL;
    }

    if (perf_capture_scene_name != NULL) {
        SDL_free(perf_capture_scene_name);
        perf_capture_scene_name = NULL;
    }

    perf_capture_enabled = false;
    perf_capture_completed = false;
    perf_capture_basic_mode = false;
    perf_capture_basic_first_window_family_snapshots_enabled = false;
    perf_capture_basic_first_window_render_subphases_enabled = false;
    perf_capture_basic_first_window_exact_hot_family_alpha_offpath_enabled = false;
    perf_capture_basic_first_window_onset_exact_hot_family_alpha_offpath_enabled = false;
    perf_capture_basic_first_window_onset_cluster_alpha_offpath_enabled = false;
    perf_capture_fast_non_integer_reuse_telemetry_enabled = true;
    perf_capture_fast_non_integer_subrect_alpha_telemetry_enabled = false;
    perf_capture_target_frames = 0;
    perf_capture_recorded_frames = 0;
    perf_capture_reset_window_snapshot(&perf_capture_first_window_snapshot);
    perf_capture_reset_window_snapshot(&perf_capture_first_burst_snapshot);
    perf_frame_start_ns = 0;
    perf_update_start_ns = 0;
    perf_update_ns_total = 0;
    perf_render_ns_total = 0;
    perf_resolve_ns_total = 0;
    perf_raster_pass_ns_total = 0;
    perf_sort_qsort_invocations_total = 0;
    perf_present_ns_total = 0;
    perf_frame_work_ns_total = 0;
    perf_dirty_tiles_total = 0;
    perf_render_task_count_total = 0;
    perf_rect_copy_tasks_total = 0;
    perf_batch_runs_total = 0;
    perf_batched_task_count_total = 0;
    perf_rect_texture_runs_total = 0;
    perf_rect_texture_multi_runs_total = 0;
    perf_rect_texture_multi_run_tasks_total = 0;
    perf_rect_texture_max_run_total = 0;
    perf_rect_texture_hstrip_runs_total = 0;
    perf_rect_texture_hstrip_tasks_total = 0;
    perf_rect_texture_vstrip_runs_total = 0;
    perf_rect_texture_vstrip_tasks_total = 0;
    perf_rect_texture_run_links_total = 0;
    perf_rect_texture_color_breaks_total = 0;
    perf_rect_texture_flip_breaks_total = 0;
    perf_rect_texture_flipped_tasks_total = 0;
    perf_textured_geometry_tasks_total = 0;
    perf_textured_geometry_rect_recovered_tasks_total = 0;
    perf_textured_geometry_fallback_tasks_total = 0;
    perf_set_texture_calls_total = 0;
    perf_texture_binding_reuse_hits_total = 0;
    perf_texture_cache_hits_total = 0;
    perf_texture_cache_misses_total = 0;
    perf_texture_cache_miss_dirty_texture_same_frame_total = 0;
    perf_texture_cache_miss_dirty_texture_carried_total = 0;
    perf_texture_cache_miss_dirty_palette_same_frame_total = 0;
    perf_texture_cache_miss_dirty_palette_carried_total = 0;
    perf_texture_cache_miss_cold_total = 0;
    perf_texture_creates_total = 0;
    perf_texture_unlock_calls_total = 0;
    perf_palette_unlock_calls_total = 0;
    perf_palette_unlock_changed_calls_total = 0;
    perf_palette_unlock_unchanged_calls_total = 0;
    perf_texture_unlock_dirty_surface_variants_total = 0;
    perf_texture_unlock_dirty_surface_variants_max_total = 0;
    perf_palette_unlock_dirty_surface_variants_total = 0;
    perf_palette_unlock_dirty_surface_variants_max_total = 0;
    perf_texture_unlock_locality_index8_tracked_total = 0;
    perf_texture_unlock_locality_index8_baseline_skips_total = 0;
    perf_texture_unlock_locality_index8_non_index8_skips_total = 0;
    perf_texture_unlock_locality_index8_source_pixels_total = 0;
    perf_texture_unlock_locality_index8_changed_pixels_total = 0;
    perf_texture_unlock_locality_index8_changed_rows_total = 0;
    perf_texture_unlock_locality_index8_changed_bbox_pixels_total = 0;
    perf_texture_unlock_invalidation_ns_total = 0;
    perf_palette_unlock_invalidation_ns_total = 0;
    perf_texture_cache_evictions_total = 0;
    perf_palette_cache_evictions_total = 0;
    perf_software_surface_cache_hits_total = 0;
    perf_software_surface_cache_creates_total = 0;
    perf_software_surface_cache_refresh_attempts_total = 0;
    perf_software_surface_cache_refresh_unique_bindings_total = 0;
    perf_software_surface_cache_refresh_repeat_binding_attempts_total = 0;
    perf_software_surface_cache_refresh_unique_texture_handles_total = 0;
    perf_software_surface_cache_refresh_texture_handle_fanout_max_total = 0;
    perf_software_surface_cache_refresh_failures_total = 0;
    perf_software_surface_cache_refresh_ns_total = 0;
    perf_software_surface_cache_refresh_palette_set_calls_total = 0;
    perf_software_surface_cache_refresh_palette_set_ns_total = 0;
    perf_software_surface_cache_refresh_blit_calls_total = 0;
    perf_software_surface_cache_refresh_blit_ns_total = 0;
    perf_software_surface_cache_create_dirty_texture_same_frame_total = 0;
    perf_software_surface_cache_create_dirty_texture_carried_total = 0;
    perf_software_surface_cache_create_dirty_palette_same_frame_total = 0;
    perf_software_surface_cache_create_dirty_palette_carried_total = 0;
    perf_software_surface_cache_create_cold_total = 0;
    perf_software_surface_cache_texture_evictions_total = 0;
    perf_software_surface_cache_palette_evictions_total = 0;
    perf_textures_destroy_queued_total = 0;
    perf_unknown_tasks_total = 0;
    perf_ppg_tasks_total = 0;
    perf_mtrans_tasks_total = 0;
    perf_ui_direct_tasks_total = 0;
    perf_solid_tasks_total = 0;
    perf_hybrid_candidate_tasks_total = 0;
    perf_hybrid_candidate_pixels_total = 0;
    perf_hybrid_fallback_tasks_total = 0;
    perf_hybrid_fallback_pixels_total = 0;
    perf_hybrid_reason_clip_total = 0;
    perf_hybrid_reason_alpha_total = 0;
    perf_hybrid_reason_color_mod_total = 0;
    perf_hybrid_reason_flip_total = 0;
    perf_hybrid_reason_geometry_total = 0;
    perf_hybrid_reason_solid_total = 0;
    for (int player = 0; player < 2; player++) {
        perf_super_art_stock_available_frames[player] = 0;
        perf_super_art_stock_available_first_frame[player] = -1;
        perf_super_art_stock_max_reached[player] = 0;
        perf_super_art_stock_capacity[player] = 0;
        perf_super_art_gauge_max_value[player] = 0;
        perf_super_art_gauge_capacity[player] = 0;
        perf_super_art_ready_frames[player] = 0;
        perf_super_art_ready_first_frame[player] = -1;
        SDL_memset(perf_super_art_ready_routine1_frames[player], 0, sizeof(perf_super_art_ready_routine1_frames[player]));
        perf_super_art_ready_first_routine[player][0] = -1;
        perf_super_art_ready_first_routine[player][1] = -1;
        perf_super_art_ready_first_routine[player][2] = -1;
        perf_super_art_ready_last_routine[player][0] = -1;
        perf_super_art_ready_last_routine[player][1] = -1;
        perf_super_art_ready_last_routine[player][2] = -1;
        perf_super_art_active_frames[player] = 0;
        perf_super_art_active_first_frame[player] = -1;
        perf_super_art_active_starts[player] = 0;
        perf_super_art_active_was_active[player] = false;
        perf_metamorphose_active_frames[player] = 0;
        perf_metamorphose_active_first_frame[player] = -1;
    }
    SDL_zero(perf_capture_start_g_no);
    SDL_zero(perf_capture_start_e_no);
    SDL_zero(perf_capture_start_d_no);
    perf_capture_start_menu_task_condition = 0;
    SDL_zero(perf_capture_start_menu_task_r_no);
    perf_capture_start_break_into = 0;
    perf_capture_start_hnc_num = 0;
    perf_capture_start_exec_wipe = 0;
    perf_capture_start_active_wipe_type = -1;
    perf_capture_start_wipe_limit = 0;
    SDL_zero(perf_capture_start_sel_pl_complete);
    SDL_zero(perf_capture_start_sel_arts_complete);
    SDL_zero(perf_capture_start_select_arts);
    SDL_zero(perf_capture_start_moving_plate);
    SDL_zero(perf_capture_start_moving_plate_counter);
    SDL_zero(perf_capture_start_command_name_visible);
    perf_capture_start_title_tex_flag = 0;
    perf_capture_start_opening_r_no_0 = 0;
    perf_capture_start_opening_r_no_1 = 0;
    perf_capture_start_opening_r_no_2 = 0;
    perf_capture_start_opening_free_work = 0;
    perf_capture_start_demo_flag = 0;
    perf_capture_start_demo_logo_effect_index = -1;
    perf_capture_start_demo_logo_routine2 = -1;
    perf_capture_start_demo_logo_direction = -1;
    perf_capture_start_demo_logo_dir_timer = -1;
    perf_break_into_frames_total = 0;
    perf_break_into_first_frame = -1;
    perf_hnc_active_frames_total = 0;
    perf_hnc_active_first_frame = -1;
    perf_hnc_max_num = 0;
    perf_wipe_type1_active_frames_total = 0;
    perf_wipe_type1_active_first_frame = -1;
    perf_wipe_type1_max_limit = 0;
    perf_title_logo_active_frames_total = 0;
    perf_title_logo_active_first_frame = -1;
    perf_demo_logo_active_frames_total = 0;
    perf_demo_logo_active_first_frame = -1;
    perf_demo_logo_max_direction = -1;
    perf_software_frame_mode_enabled_frames = 0;
    perf_software_frame_surface_ready_frames = 0;
    perf_software_frame_owned_frames = 0;
    perf_software_frame_direct_present_frames = 0;
    perf_software_frame_uploaded_frames = 0;
    perf_software_frame_fallback_frames = 0;
    perf_software_frame_candidate_tasks_total = 0;
    perf_software_frame_candidate_pixels_total = 0;
    perf_software_frame_fallback_tasks_total = 0;
    perf_software_frame_fallback_pixels_total = 0;
    perf_software_frame_fast_exact_tasks_total = 0;
    perf_software_frame_fast_exact_pixels_total = 0;
    perf_software_frame_fast_exact_clipped_tasks_total = 0;
    perf_software_frame_fast_exact_flipped_tasks_total = 0;
    perf_software_frame_fast_exact_color_mod_tasks_total = 0;
    perf_software_frame_fast_exact_color_mod_pixels_total = 0;
    perf_software_frame_fast_scaled_tasks_total = 0;
    perf_software_frame_fast_scaled_pixels_total = 0;
    perf_software_frame_fast_non_integer_tasks_total = 0;
    perf_software_frame_fast_non_integer_pixels_total = 0;
    perf_software_frame_fast_non_integer_lookup_entries_total = 0;
    perf_software_frame_fast_non_integer_source_alpha_opaque_pixels_total = 0;
    perf_software_frame_fast_non_integer_source_alpha_transparent_pixels_total = 0;
    perf_software_frame_fast_non_integer_source_alpha_blended_pixels_total = 0;
    perf_software_frame_fast_non_integer_same_source_runs_total = 0;
    perf_software_frame_fast_non_integer_same_source_reuse_runs_total = 0;
    perf_software_frame_fast_non_integer_same_source_reused_pixels_total = 0;
    perf_software_frame_fast_non_integer_same_source_opaque_reused_pixels_total = 0;
    perf_software_frame_fast_non_integer_same_source_transparent_reused_pixels_total = 0;
    perf_software_frame_fast_non_integer_same_source_blended_reused_pixels_total = 0;
    perf_software_frame_fast_non_integer_same_source_max_run_length_total = 0;
    perf_software_frame_fast_non_integer_alpha_only_tasks_total = 0;
    perf_software_frame_fast_non_integer_alpha_only_pixels_total = 0;
    perf_software_frame_fast_non_integer_rgb_mod_tasks_total = 0;
    perf_software_frame_fast_non_integer_rgb_mod_pixels_total = 0;
    perf_software_frame_generic_textured_tasks_total = 0;
    perf_software_frame_generic_textured_pixels_total = 0;
    perf_software_frame_generic_textured_alpha_only_tasks_total = 0;
    perf_software_frame_generic_textured_alpha_only_pixels_total = 0;
    perf_software_frame_generic_textured_rgb_mod_tasks_total = 0;
    perf_software_frame_generic_textured_rgb_mod_pixels_total = 0;
    perf_software_frame_fast_miss_color_mod_total = 0;
    perf_software_frame_fast_miss_non_integer_total = 0;
    perf_software_frame_fast_miss_non_integer_lookup_entries_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_256_tasks_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_256_pixels_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_256_lookup_entries_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_1024_tasks_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_1024_pixels_total = 0;
    perf_software_frame_fast_miss_non_integer_max_pixels_total = 0;
    perf_software_frame_fast_miss_scaled_total = 0;
    perf_software_frame_fast_miss_unsupported_flip_total = 0;
    perf_software_frame_fast_miss_source_bounds_total = 0;
    perf_software_frame_reason_alpha_total = 0;
    perf_software_frame_reason_color_mod_total = 0;
    perf_software_frame_reason_geometry_total = 0;
    perf_software_frame_reason_solid_total = 0;
}

static bool perf_capture_collect_extended_stats(void) {
    return perf_capture_enabled && !perf_capture_completed && !perf_capture_basic_mode;
}

static const char* perf_capture_detail_mode_name(void) {
    if (!perf_capture_basic_mode) {
        return "full";
    }

    return perf_capture_basic_first_window_family_snapshots_enabled ? "basic-first-window-families" : "basic";
}

void SDLApp_ConfigurePerfCapture(int frame_count,
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
    if (frame_count <= 0) {
        perf_capture_reset_storage();
        return;
    }

    if (perf_samples != NULL) {
        SDL_free(perf_samples);
        perf_samples = NULL;
    }

    if (perf_capture_output_path != NULL) {
        SDL_free(perf_capture_output_path);
        perf_capture_output_path = NULL;
    }

    if (perf_capture_scene_name != NULL) {
        SDL_free(perf_capture_scene_name);
        perf_capture_scene_name = NULL;
    }

    perf_capture_target_frames = frame_count;
    perf_capture_recorded_frames = 0;
    perf_capture_completed = false;
    perf_capture_enabled = true;
    perf_capture_basic_mode = basic_mode;
    perf_capture_basic_first_window_family_snapshots_enabled = basic_mode && basic_first_window_family_snapshots;
    perf_capture_basic_first_window_render_subphases_enabled =
        perf_capture_basic_first_window_family_snapshots_enabled && basic_first_window_render_subphases;
    perf_capture_basic_first_window_exact_hot_family_alpha_offpath_enabled =
        perf_capture_basic_first_window_family_snapshots_enabled &&
        basic_first_window_exact_hot_family_alpha_offpath;
    perf_capture_basic_first_window_onset_exact_hot_family_alpha_offpath_enabled =
        perf_capture_basic_first_window_family_snapshots_enabled &&
        basic_first_window_onset_exact_hot_family_alpha_offpath;
    perf_capture_basic_first_window_onset_cluster_alpha_offpath_enabled =
        perf_capture_basic_first_window_family_snapshots_enabled &&
        basic_first_window_onset_cluster_alpha_offpath;
    perf_capture_fast_non_integer_reuse_telemetry_enabled = !disable_reuse_telemetry;
    perf_capture_fast_non_integer_subrect_alpha_telemetry_enabled = !basic_mode && enable_subrect_alpha_telemetry;
    perf_capture_reset_window_snapshot(&perf_capture_first_window_snapshot);
    perf_capture_reset_window_snapshot(&perf_capture_first_burst_snapshot);
    perf_frame_start_ns = 0;
    perf_update_start_ns = 0;
    perf_update_ns_total = 0;
    perf_render_ns_total = 0;
    perf_resolve_ns_total = 0;
    perf_raster_pass_ns_total = 0;
    perf_sort_qsort_invocations_total = 0;
    perf_present_ns_total = 0;
    perf_frame_work_ns_total = 0;
    perf_dirty_tiles_total = 0;
    perf_render_task_count_total = 0;
    perf_rect_copy_tasks_total = 0;
    perf_batch_runs_total = 0;
    perf_batched_task_count_total = 0;
    perf_rect_texture_runs_total = 0;
    perf_rect_texture_multi_runs_total = 0;
    perf_rect_texture_multi_run_tasks_total = 0;
    perf_rect_texture_max_run_total = 0;
    perf_rect_texture_hstrip_runs_total = 0;
    perf_rect_texture_hstrip_tasks_total = 0;
    perf_rect_texture_vstrip_runs_total = 0;
    perf_rect_texture_vstrip_tasks_total = 0;
    perf_rect_texture_run_links_total = 0;
    perf_rect_texture_color_breaks_total = 0;
    perf_rect_texture_flip_breaks_total = 0;
    perf_rect_texture_flipped_tasks_total = 0;
    perf_textured_geometry_tasks_total = 0;
    perf_textured_geometry_rect_recovered_tasks_total = 0;
    perf_textured_geometry_fallback_tasks_total = 0;
    perf_set_texture_calls_total = 0;
    perf_texture_binding_reuse_hits_total = 0;
    perf_texture_cache_hits_total = 0;
    perf_texture_cache_misses_total = 0;
    perf_texture_cache_miss_dirty_texture_same_frame_total = 0;
    perf_texture_cache_miss_dirty_texture_carried_total = 0;
    perf_texture_cache_miss_dirty_palette_same_frame_total = 0;
    perf_texture_cache_miss_dirty_palette_carried_total = 0;
    perf_texture_cache_miss_cold_total = 0;
    perf_texture_creates_total = 0;
    perf_texture_unlock_calls_total = 0;
    perf_palette_unlock_calls_total = 0;
    perf_palette_unlock_changed_calls_total = 0;
    perf_palette_unlock_unchanged_calls_total = 0;
    perf_texture_unlock_dirty_surface_variants_total = 0;
    perf_texture_unlock_dirty_surface_variants_max_total = 0;
    perf_palette_unlock_dirty_surface_variants_total = 0;
    perf_palette_unlock_dirty_surface_variants_max_total = 0;
    perf_texture_unlock_locality_index8_tracked_total = 0;
    perf_texture_unlock_locality_index8_baseline_skips_total = 0;
    perf_texture_unlock_locality_index8_non_index8_skips_total = 0;
    perf_texture_unlock_locality_index8_source_pixels_total = 0;
    perf_texture_unlock_locality_index8_changed_pixels_total = 0;
    perf_texture_unlock_locality_index8_changed_rows_total = 0;
    perf_texture_unlock_locality_index8_changed_bbox_pixels_total = 0;
    perf_texture_unlock_invalidation_ns_total = 0;
    perf_palette_unlock_invalidation_ns_total = 0;
    perf_texture_cache_evictions_total = 0;
    perf_palette_cache_evictions_total = 0;
    perf_software_surface_cache_hits_total = 0;
    perf_software_surface_cache_creates_total = 0;
    perf_software_surface_cache_refresh_attempts_total = 0;
    perf_software_surface_cache_refresh_unique_bindings_total = 0;
    perf_software_surface_cache_refresh_repeat_binding_attempts_total = 0;
    perf_software_surface_cache_refresh_unique_texture_handles_total = 0;
    perf_software_surface_cache_refresh_texture_handle_fanout_max_total = 0;
    perf_software_surface_cache_refresh_failures_total = 0;
    perf_software_surface_cache_refresh_ns_total = 0;
    perf_software_surface_cache_refresh_palette_set_calls_total = 0;
    perf_software_surface_cache_refresh_palette_set_ns_total = 0;
    perf_software_surface_cache_refresh_blit_calls_total = 0;
    perf_software_surface_cache_refresh_blit_ns_total = 0;
    perf_software_surface_cache_create_dirty_texture_same_frame_total = 0;
    perf_software_surface_cache_create_dirty_texture_carried_total = 0;
    perf_software_surface_cache_create_dirty_palette_same_frame_total = 0;
    perf_software_surface_cache_create_dirty_palette_carried_total = 0;
    perf_software_surface_cache_create_cold_total = 0;
    perf_software_surface_cache_texture_evictions_total = 0;
    perf_software_surface_cache_palette_evictions_total = 0;
    perf_textures_destroy_queued_total = 0;
    perf_unknown_tasks_total = 0;
    perf_ppg_tasks_total = 0;
    perf_mtrans_tasks_total = 0;
    perf_ui_direct_tasks_total = 0;
    perf_solid_tasks_total = 0;
    perf_hybrid_candidate_tasks_total = 0;
    perf_hybrid_candidate_pixels_total = 0;
    perf_hybrid_fallback_tasks_total = 0;
    perf_hybrid_fallback_pixels_total = 0;
    perf_hybrid_reason_clip_total = 0;
    perf_hybrid_reason_alpha_total = 0;
    perf_hybrid_reason_color_mod_total = 0;
    perf_hybrid_reason_flip_total = 0;
    perf_hybrid_reason_geometry_total = 0;
    perf_hybrid_reason_solid_total = 0;
    for (int player = 0; player < 2; player++) {
        perf_super_art_ready_frames[player] = 0;
        perf_super_art_ready_first_frame[player] = -1;
        SDL_memset(perf_super_art_ready_routine1_frames[player], 0, sizeof(perf_super_art_ready_routine1_frames[player]));
        perf_super_art_ready_first_routine[player][0] = -1;
        perf_super_art_ready_first_routine[player][1] = -1;
        perf_super_art_ready_first_routine[player][2] = -1;
        perf_super_art_ready_last_routine[player][0] = -1;
        perf_super_art_ready_last_routine[player][1] = -1;
        perf_super_art_ready_last_routine[player][2] = -1;
        perf_super_art_active_frames[player] = 0;
        perf_super_art_active_first_frame[player] = -1;
        perf_super_art_active_starts[player] = 0;
        perf_super_art_active_was_active[player] = false;
        perf_metamorphose_active_frames[player] = 0;
        perf_metamorphose_active_first_frame[player] = -1;
    }
    perf_software_frame_mode_enabled_frames = 0;
    perf_software_frame_surface_ready_frames = 0;
    perf_software_frame_owned_frames = 0;
    perf_software_frame_direct_present_frames = 0;
    perf_software_frame_uploaded_frames = 0;
    perf_software_frame_fallback_frames = 0;
    perf_software_frame_candidate_tasks_total = 0;
    perf_software_frame_candidate_pixels_total = 0;
    perf_software_frame_fallback_tasks_total = 0;
    perf_software_frame_fallback_pixels_total = 0;
    perf_software_frame_fast_exact_tasks_total = 0;
    perf_software_frame_fast_exact_pixels_total = 0;
    perf_software_frame_fast_exact_clipped_tasks_total = 0;
    perf_software_frame_fast_exact_flipped_tasks_total = 0;
    perf_software_frame_fast_exact_color_mod_tasks_total = 0;
    perf_software_frame_fast_exact_color_mod_pixels_total = 0;
    perf_software_frame_fast_scaled_tasks_total = 0;
    perf_software_frame_fast_scaled_pixels_total = 0;
    perf_software_frame_fast_non_integer_tasks_total = 0;
    perf_software_frame_fast_non_integer_pixels_total = 0;
    perf_software_frame_fast_non_integer_lookup_entries_total = 0;
    perf_software_frame_fast_non_integer_source_alpha_opaque_pixels_total = 0;
    perf_software_frame_fast_non_integer_source_alpha_transparent_pixels_total = 0;
    perf_software_frame_fast_non_integer_source_alpha_blended_pixels_total = 0;
    perf_software_frame_fast_non_integer_same_source_runs_total = 0;
    perf_software_frame_fast_non_integer_same_source_reuse_runs_total = 0;
    perf_software_frame_fast_non_integer_same_source_reused_pixels_total = 0;
    perf_software_frame_fast_non_integer_same_source_opaque_reused_pixels_total = 0;
    perf_software_frame_fast_non_integer_same_source_transparent_reused_pixels_total = 0;
    perf_software_frame_fast_non_integer_same_source_blended_reused_pixels_total = 0;
    perf_software_frame_fast_non_integer_same_source_max_run_length_total = 0;
    perf_software_frame_fast_non_integer_alpha_only_tasks_total = 0;
    perf_software_frame_fast_non_integer_alpha_only_pixels_total = 0;
    perf_software_frame_fast_non_integer_rgb_mod_tasks_total = 0;
    perf_software_frame_fast_non_integer_rgb_mod_pixels_total = 0;
    perf_software_frame_generic_textured_tasks_total = 0;
    perf_software_frame_generic_textured_pixels_total = 0;
    perf_software_frame_generic_textured_alpha_only_tasks_total = 0;
    perf_software_frame_generic_textured_alpha_only_pixels_total = 0;
    perf_software_frame_generic_textured_rgb_mod_tasks_total = 0;
    perf_software_frame_generic_textured_rgb_mod_pixels_total = 0;
    perf_software_frame_fast_miss_color_mod_total = 0;
    perf_software_frame_fast_miss_non_integer_total = 0;
    perf_software_frame_fast_miss_non_integer_lookup_entries_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_256_tasks_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_256_pixels_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_256_lookup_entries_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_1024_tasks_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_1024_pixels_total = 0;
    perf_software_frame_fast_miss_non_integer_max_pixels_total = 0;
    perf_software_frame_fast_miss_scaled_total = 0;
    perf_software_frame_fast_miss_unsupported_flip_total = 0;
    perf_software_frame_fast_miss_source_bounds_total = 0;
    perf_software_frame_reason_alpha_total = 0;
    perf_software_frame_reason_color_mod_total = 0;
    perf_software_frame_reason_geometry_total = 0;
    perf_software_frame_reason_solid_total = 0;
    perf_capture_output_path = output_path != NULL ? SDL_strdup(output_path) : NULL;
    perf_capture_scene_name = scene_name != NULL ? SDL_strdup(scene_name) : NULL;
    perf_samples = (PerfFrameSample*)SDL_calloc((size_t)frame_count, sizeof(PerfFrameSample));

    if (perf_samples == NULL) {
        backend_logf("PERF capture disabled: failed to allocate sample buffer for %d frames.", frame_count);
        perf_capture_reset_storage();
        return;
    }

    snapshot_perf_capture_transition_start_state();
    backend_logf("PERF capture enabled: frames=%d output=%s scene=%s detail_mode=%s scale_mode=%s software_frame_mode=%s fast_non_integer_reuse_telemetry=%s basic_first_window_family_snapshots=%s basic_first_window_render_subphases=%s basic_first_window_exact_hot_family_alpha_offpath=%s basic_first_window_onset_exact_hot_family_alpha_offpath=%s basic_first_window_onset_cluster_alpha_offpath=%s fast_non_integer_subrect_alpha_telemetry=%s",
                 perf_capture_target_frames,
                 perf_capture_output_path != NULL ? perf_capture_output_path : "(auto)",
                 perf_capture_scene_name != NULL ? perf_capture_scene_name : "(none)",
                 perf_capture_detail_mode_name(),
                 scale_mode_name(scale_mode),
                 software_frame_mode_name(),
                 perf_capture_fast_non_integer_reuse_telemetry_enabled ? "on" : "off",
                 perf_capture_basic_first_window_family_snapshots_enabled ? "on" : "off",
                 perf_capture_basic_first_window_render_subphases_enabled ? "on" : "off",
                 perf_capture_basic_first_window_exact_hot_family_alpha_offpath_enabled ? "on" : "off",
                 perf_capture_basic_first_window_onset_exact_hot_family_alpha_offpath_enabled ? "on" : "off",
                 perf_capture_basic_first_window_onset_cluster_alpha_offpath_enabled ? "on" : "off",
                 perf_capture_fast_non_integer_subrect_alpha_telemetry_enabled ? "on" : "off");
}

static void perf_capture_write_summary(void) {
    if (!perf_capture_enabled || (perf_capture_recorded_frames <= 0)) {
        return;
    }

    char* output_path = NULL;
    if (perf_capture_output_path != NULL && perf_capture_output_path[0] != '\0') {
        output_path = SDL_strdup(perf_capture_output_path);
    } else {
        const char* pref_path = Paths_GetPrefPath();
        char* logs_dir = NULL;
        SDL_asprintf(&logs_dir, "%slogs", pref_path);
        SDL_CreateDirectory(logs_dir);
        SDL_asprintf(&output_path, "%s/perf-capture.json", logs_dir);
        SDL_free(logs_dir);
    }

    if (output_path == NULL) {
        backend_logf("PERF capture: failed to resolve output path.");
        return;
    }

    SDL_IOStream* io = SDL_IOFromFile(output_path, "w");
    if (io == NULL) {
        backend_logf("PERF capture: failed to open output file '%s': %s", output_path, SDL_GetError());
        SDL_free(output_path);
        return;
    }

    /* Phase D rip: the legacy renderer perf-capture surface is gone.
     * Emit a minimal frame-time summary; downstream Ralph-loop scripts that
     * grep for the deleted keys will fail loudly (intentional). */
    const int frame_count = perf_capture_recorded_frames;
    double total_frame_ms = 0.0;
    for (int i = 0; i < frame_count; i++) {
        total_frame_ms += perf_samples[i].frame_time_ms;
    }
    const double avg_frame_ms = frame_count > 0 ? total_frame_ms / frame_count : 0.0;
    const double fps = avg_frame_ms > 0.0 ? 1000.0 / avg_frame_ms : 0.0;
    io_printf(io, "{\n");
    io_printf(io, "  \"schema_version\": 65,\n");
    io_printf(io, "  \"frames\": %d,\n", frame_count);
    io_printf(io, "  \"avg_frame_ms\": %.4f,\n", avg_frame_ms);
    io_printf(io, "  \"fps\": %.4f\n", fps);
    io_printf(io, "}\n");
    SDL_CloseIO(io);
    backend_logf("PERF capture complete: frames=%d avg_frame_ms=%.3f fps=%.2f output=%s",
                 frame_count, avg_frame_ms, fps, output_path);
    SDL_free(output_path);
}
#else
static bool perf_capture_collect_extended_stats(void) {
    return false;
}
#endif

static const char* get_effective_video_driver_order(const char* configured_order) {
#if defined(PORT_MISTER)
    if (configured_order != NULL && SDL_strcmp(configured_order, legacy_mister_video_driver_order) == 0) {
        return recommended_mister_video_driver_order;
    }
#endif
    return configured_order;
}

static const char* get_effective_audio_driver(void) {
    const char* configured_driver = SDL_getenv("SDL_AUDIO_DRIVER");
    if (configured_driver != NULL && configured_driver[0] != '\0') {
        return configured_driver;
    }

    configured_driver = SDL_getenv("SDL_AUDIODRIVER");
    if (configured_driver != NULL && configured_driver[0] != '\0') {
        return configured_driver;
    }

    configured_driver = SDL_GetHint(SDL_HINT_AUDIO_DRIVER);
    if (configured_driver != NULL && configured_driver[0] != '\0') {
        return configured_driver;
    }

#if defined(PORT_MISTER)
    return recommended_mister_audio_driver;
#else
    return NULL;
#endif
}

static void apply_backend_hints() {
    const char* configured_video_driver_order = Config_GetString(CFG_KEY_VIDEO_DRIVER_ORDER);
    const char* video_driver_order = get_effective_video_driver_order(configured_video_driver_order);
    const char* render_driver_order = Config_GetString(CFG_KEY_RENDER_DRIVER_ORDER);
    const char* audio_driver = get_effective_audio_driver();

    if (video_driver_order != NULL && video_driver_order[0] != '\0') {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, video_driver_order);
    }

    if (render_driver_order != NULL && render_driver_order[0] != '\0') {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, render_driver_order);
    }

    if (audio_driver != NULL && audio_driver[0] != '\0') {
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, audio_driver);
    }
}

static void log_backend_diagnostics() {
    const char* configured_video_order = Config_GetString(CFG_KEY_VIDEO_DRIVER_ORDER);
    const char* effective_video_order = get_effective_video_driver_order(configured_video_order);
    const char* render_order = Config_GetString(CFG_KEY_RENDER_DRIVER_ORDER);
    const char* requested_audio_driver = get_effective_audio_driver();
    const char* current_audio_driver = SDL_GetCurrentAudioDriver();

    backend_logf("===== SDL backend probe start =====");
    backend_logf("Platform: %s", SDL_GetPlatform());
    backend_logf("Config video-driver-order: %s", configured_video_order != NULL ? configured_video_order : "(null)");
    backend_logf("Effective video-driver-order: %s", effective_video_order != NULL ? effective_video_order : "(null)");
    backend_logf("Config render-driver-order: %s", render_order != NULL ? render_order : "(null)");
    backend_logf("Requested audio-driver: %s", requested_audio_driver != NULL ? requested_audio_driver : "(auto)");
    backend_logf("Current audio-driver: %s", current_audio_driver != NULL ? current_audio_driver : "(null)");

    const int video_driver_count = SDL_GetNumVideoDrivers();
    backend_logf("Available video drivers (%d):", video_driver_count);
    for (int i = 0; i < video_driver_count; i++) {
        const char* name = SDL_GetVideoDriver(i);
        backend_logf("  video[%d]=%s", i, name != NULL ? name : "(null)");
    }

    const int render_driver_count = SDL_GetNumRenderDrivers();
    backend_logf("Available render drivers (%d):", render_driver_count);
    for (int i = 0; i < render_driver_count; i++) {
        const char* name = SDL_GetRenderDriver(i);
        backend_logf("  render[%d]=%s", i, name != NULL ? name : "(null)");
    }

    const int audio_driver_count = SDL_GetNumAudioDrivers();
    backend_logf("Available audio drivers (%d):", audio_driver_count);
    for (int i = 0; i < audio_driver_count; i++) {
        const char* name = SDL_GetAudioDriver(i);
        backend_logf("  audio[%d]=%s", i, name != NULL ? name : "(null)");
    }

    const char* current_video = SDL_GetCurrentVideoDriver();
    backend_logf("Current video driver: %s", current_video != NULL ? current_video : "(null)");

    int kbd_count = 0;
    SDL_KeyboardID* kbds = SDL_GetKeyboards(&kbd_count);
    backend_logf("Keyboards seen by SDL (%d):", kbd_count);
    for (int i = 0; i < kbd_count; i++) {
        const char* name = SDL_GetKeyboardNameForID(kbds[i]);
        backend_logf("  kbd[%d] id=%u name=%s", i, (unsigned)kbds[i], name != NULL ? name : "(null)");
    }
    SDL_free(kbds);

    int joy_count = 0;
    SDL_JoystickID* joys = SDL_GetJoysticks(&joy_count);
    backend_logf("Joysticks seen by SDL (%d):", joy_count);
    for (int i = 0; i < joy_count; i++) {
        const char* name = SDL_GetJoystickNameForID(joys[i]);
        backend_logf("  joy[%d] id=%u name=%s isGamepad=%d", i, (unsigned)joys[i],
                     name != NULL ? name : "(null)", SDL_IsGamepad(joys[i]) ? 1 : 0);
    }
    SDL_free(joys);

    int pad_count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&pad_count);
    backend_logf("Gamepads seen by SDL (%d):", pad_count);
    for (int i = 0; i < pad_count; i++) {
        const char* name = SDL_GetGamepadNameForID(pads[i]);
        backend_logf("  pad[%d] id=%u name=%s", i, (unsigned)pads[i], name != NULL ? name : "(null)");
    }
    SDL_free(pads);

#if defined(PORT_MIYOO_MINI_PLUS)
    {
        DIR* dir = opendir("/dev/input");
        if (dir == NULL) {
            backend_logf("/dev/input: opendir failed: %s", strerror(errno));
        } else {
            backend_logf("/dev/input contents:");
            struct dirent* ent;
            while ((ent = readdir(dir)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char path[256];
                snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
                struct stat st;
                int stat_rc = stat(path, &st);
                int r_ok = access(path, R_OK);
                int rw_ok = access(path, R_OK | W_OK);
                backend_logf("  %s mode=%o stat_rc=%d r_ok=%d rw_ok=%d",
                             path,
                             stat_rc == 0 ? (unsigned)(st.st_mode & 0777) : 0,
                             stat_rc, r_ok, rw_ok);
            }
            closedir(dir);
        }
        backend_logf("euid=%u egid=%u", (unsigned)geteuid(), (unsigned)getegid());

        FILE* pf = fopen("/proc/bus/input/devices", "r");
        if (pf == NULL) {
            backend_logf("/proc/bus/input/devices: open failed: %s", strerror(errno));
        } else {
            backend_logf("---- /proc/bus/input/devices ----");
            char line[512];
            while (fgets(line, sizeof(line), pf) != NULL) {
                size_t len = strlen(line);
                if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
                backend_logf("  %s", line);
            }
            backend_logf("---- end /proc/bus/input/devices ----");
            fclose(pf);
        }
    }
#endif
}

static const char* scale_mode_name(ScaleMode mode) {
    switch (mode) {
    case SCALEMODE_NATIVE:
        return "native";
    case SCALEMODE_NEAREST:
        return "nearest";
    case SCALEMODE_LINEAR:
        return "linear";
    case SCALEMODE_SOFT_LINEAR:
        return "soft-linear";
    case SCALEMODE_SQUARE_PIXELS:
        return "square-pixels";
    case SCALEMODE_INTEGER:
        return "integer";
    }

    return "nearest";
}

static bool parse_scale_mode(const char* raw_scalemode, ScaleMode* out_mode) {
    if (raw_scalemode == NULL || raw_scalemode[0] == '\0' || out_mode == NULL) {
        return false;
    }

    if (SDL_strcmp(raw_scalemode, "nearest") == 0) {
        *out_mode = SCALEMODE_NEAREST;
    } else if (SDL_strcmp(raw_scalemode, "native") == 0) {
        *out_mode = SCALEMODE_NATIVE;
    } else if (SDL_strcmp(raw_scalemode, "linear") == 0) {
        *out_mode = SCALEMODE_LINEAR;
    } else if (SDL_strcmp(raw_scalemode, "soft-linear") == 0) {
        *out_mode = SCALEMODE_SOFT_LINEAR;
    } else if (SDL_strcmp(raw_scalemode, "square-pixels") == 0) {
        *out_mode = SCALEMODE_SQUARE_PIXELS;
    } else if (SDL_strcmp(raw_scalemode, "integer") == 0) {
        *out_mode = SCALEMODE_INTEGER;
    } else {
        return false;
    }

    return true;
}

static SDL_ScaleMode screen_texture_scale_mode() {
    switch (scale_mode) {
    case SCALEMODE_LINEAR:
    case SCALEMODE_SOFT_LINEAR:
        return SDL_SCALEMODE_LINEAR;

    case SCALEMODE_NATIVE:
    case SCALEMODE_NEAREST:
    case SCALEMODE_SQUARE_PIXELS:
    case SCALEMODE_INTEGER:
        return SDL_SCALEMODE_NEAREST;
    default:
        return SDL_SCALEMODE_INVALID;
    }

    return SDL_SCALEMODE_NEAREST;
}

static bool get_native_output_size(int* out_w, int* out_h);

typedef struct StartupConfigDefaultEntry {
    const char* key;
    const char* value;
} StartupConfigDefaultEntry;

static const StartupConfigDefaultEntry startup_generated_defaults[] = {
    { CFG_KEY_FULLSCREEN, "true" },
    { CFG_KEY_WINDOW_WIDTH, "320" },
    { CFG_KEY_WINDOW_HEIGHT, "240" },
    { CFG_KEY_SCALEMODE, "native" },
    { CFG_KEY_SOFTWARE_FRAME_MODE, "on" },
    { CFG_KEY_SHOW_FPS, "off" },
    { CFG_KEY_VIDEO_DRIVER_ORDER, "dummy" },
    { CFG_KEY_RENDER_DRIVER_ORDER, "software" },
};

static bool runtime_scale_mode_has_explicit_wrapper_marker(void) {
    const char* pref_path = Paths_GetPrefPath();
    char* config_path = NULL;
    SDL_asprintf(&config_path, "%sconfig", pref_path);

    FILE* file = fopen(config_path, "r");
    SDL_free(config_path);
    if (file == NULL) {
        return false;
    }

    char line[256] = { 0 };
    while (fgets(line, sizeof(line), file) != NULL) {
        char* cursor = line;
        while ((*cursor != '\0') && SDL_isspace(*cursor)) {
            cursor++;
        }

        size_t len = SDL_strlen(cursor);
        while ((len > 0) && SDL_isspace(cursor[len - 1])) {
            cursor[--len] = '\0';
        }

        if (SDL_strcmp(cursor, wrapper_scale_mode_explicit_marker) == 0) {
            fclose(file);
            return true;
        }
    }

    fclose(file);
    return false;
}

static bool runtime_scale_mode_has_auto_wrapper_marker(void) {
    const char* pref_path = Paths_GetPrefPath();
    char* config_path = NULL;
    SDL_asprintf(&config_path, "%sconfig", pref_path);

    FILE* file = fopen(config_path, "r");
    SDL_free(config_path);
    if (file == NULL) {
        return false;
    }

    char line[256] = { 0 };
    while (fgets(line, sizeof(line), file) != NULL) {
        char* cursor = line;
        while ((*cursor != '\0') && SDL_isspace(*cursor)) {
            cursor++;
        }

        size_t len = SDL_strlen(cursor);
        while ((len > 0) && SDL_isspace(cursor[len - 1])) {
            cursor[--len] = '\0';
        }

        if (SDL_strcmp(cursor, wrapper_scale_mode_auto_marker) == 0) {
            fclose(file);
            return true;
        }
    }

    fclose(file);
    return false;
}

static bool runtime_config_matches_generated_defaults(void) {
    const char* pref_path = Paths_GetPrefPath();
    char* config_path = NULL;
    SDL_asprintf(&config_path, "%sconfig", pref_path);

    FILE* file = fopen(config_path, "r");
    SDL_free(config_path);
    if (file == NULL) {
        return false;
    }

    bool seen[SDL_arraysize(startup_generated_defaults)] = { false };
    char line[256] = { 0 };
    while (fgets(line, sizeof(line), file) != NULL) {
        char* cursor = line;
        while ((*cursor != '\0') && SDL_isspace(*cursor)) {
            cursor++;
        }
        if ((*cursor == '\0') || (*cursor == '#')) {
            continue;
        }

        char* equals = SDL_strchr(cursor, '=');
        if (equals == NULL) {
            fclose(file);
            return false;
        }

        *equals = '\0';
        char* key = cursor;
        char* raw_value = equals + 1;

        while ((*key != '\0') && SDL_isspace(*key)) {
            key++;
        }
        size_t key_len = SDL_strlen(key);
        while ((key_len > 0) && SDL_isspace(key[key_len - 1])) {
            key[--key_len] = '\0';
        }

        while ((*raw_value != '\0') && SDL_isspace(*raw_value)) {
            raw_value++;
        }
        size_t value_len = SDL_strlen(raw_value);
        while ((value_len > 0) && SDL_isspace(raw_value[value_len - 1])) {
            raw_value[--value_len] = '\0';
        }

        bool matched = false;
        for (int i = 0; i < SDL_arraysize(startup_generated_defaults); i++) {
            if (SDL_strcasecmp(key, startup_generated_defaults[i].key) != 0) {
                continue;
            }
            if (SDL_strcasecmp(raw_value, startup_generated_defaults[i].value) != 0) {
                fclose(file);
                return false;
            }

            seen[i] = true;
            matched = true;
            break;
        }

        if (!matched) {
            fclose(file);
            return false;
        }
    }

    fclose(file);

    for (int i = 0; i < SDL_arraysize(startup_generated_defaults); i++) {
        if (!seen[i]) {
            return false;
        }
    }

    return true;
}

static SDL_Point screen_texture_size() {
    SDL_Point size;
    SDL_GetRenderOutputSize(renderer, &size.x, &size.y);

    if (scale_mode == SCALEMODE_SOFT_LINEAR) {
        size.x *= 2;
        size.y *= 2;
    }

    return size;
}

static void create_screen_texture() {
    if (use_native_render_path) {
        if (screen_texture != NULL) {
            SDL_DestroyTexture(screen_texture);
            screen_texture = NULL;
        }
        native_output_rect_dirty = true;
        native_output_has_bars = true;
        native_output_width = 0;
        native_output_height = 0;
        screen_output_rect_dirty = true;
        screen_output_has_letterbox = true;
        screen_output_width = 0;
        screen_output_height = 0;
        return;
    }

    if (screen_texture != NULL) {
        SDL_DestroyTexture(screen_texture);
    }

    const SDL_Point size = screen_texture_size();
    screen_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB32, SDL_TEXTUREACCESS_TARGET, size.x, size.y);
    SDL_SetTextureScaleMode(screen_texture, screen_texture_scale_mode());
    native_output_rect_dirty = true;
    native_output_has_bars = true;
    native_output_width = 0;
    native_output_height = 0;
    screen_output_rect_dirty = true;
    screen_output_has_letterbox = true;
    screen_output_width = 0;
    screen_output_height = 0;
}

static void init_scalemode() {
    const bool has_explicit_scale_mode = Config_HasExplicitKey(CFG_KEY_SCALEMODE);
    const char* configured_scale_mode = Config_GetString(CFG_KEY_SCALEMODE);
    const char* env_scale_mode = SDL_getenv(mister_scale_mode_override_env);
    const char* requested_scale_mode = configured_scale_mode;
    const char* startup_source = has_explicit_scale_mode ? "config" : "default";
    const bool generated_default_native = has_explicit_scale_mode && (configured_scale_mode != NULL) &&
                                          (SDL_strcasecmp(configured_scale_mode, "native") == 0) &&
                                          (env_scale_mode != NULL) && (env_scale_mode[0] != '\0') &&
                                          !runtime_scale_mode_has_explicit_wrapper_marker() &&
                                          (runtime_scale_mode_has_auto_wrapper_marker() ||
                                           runtime_config_matches_generated_defaults());

    if ((!has_explicit_scale_mode || generated_default_native) &&
        env_scale_mode != NULL && env_scale_mode[0] != '\0') {
        requested_scale_mode = env_scale_mode;
        startup_source = "env";
    }

    ScaleMode parsed_scale_mode = scale_mode;
    if (parse_scale_mode(requested_scale_mode, &parsed_scale_mode)) {
        scale_mode = parsed_scale_mode;
    }

#if defined(PORT_MISTER) || defined(PORT_MIYOO_MINI_PLUS)
    // Soft-linear doubles internal target size and is too expensive on
    // MiSTer (Cortex-A9) and Miyoo Mini Plus (Cortex-A7) CPU rendering paths.
    if (scale_mode == SCALEMODE_SOFT_LINEAR) {
        scale_mode = SCALEMODE_NEAREST;
    }
#endif

    SDL_snprintf(scale_mode_startup_source, sizeof(scale_mode_startup_source), "%s", startup_source);
    SDL_snprintf(scale_mode_requested_value,
                 sizeof(scale_mode_requested_value),
                 "%s",
                 requested_scale_mode != NULL ? requested_scale_mode : "");
}

static const char* software_frame_mode_name(void) {
    return software_frame_mode_enabled ? "on" : "off";
}

static const char* super_effect_quality_mode_name(SuperEffectQualityMode mode) {
    switch (mode) {
    case SUPER_EFFECT_QUALITY_CACHED_BG:
        return "cached-bg";
    case SUPER_EFFECT_QUALITY_FULL:
    default:
        return "full";
    }
}

static bool scale_mode_uses_native_render_path(void) {
    return (scale_mode == SCALEMODE_NATIVE) || (scale_mode == SCALEMODE_SQUARE_PIXELS);
}

static void init_software_frame_mode(void) {
    const char* raw_mode = Config_GetString(CFG_KEY_SOFTWARE_FRAME_MODE);
    software_frame_mode_enabled = false;

    if ((raw_mode == NULL) || (raw_mode[0] == '\0')) {
        return;
    }

    if ((SDL_strcasecmp(raw_mode, "on") == 0) || (SDL_strcasecmp(raw_mode, "true") == 0)) {
        software_frame_mode_enabled = true;
    }
}

static SuperEffectQualityMode parse_super_effect_quality_mode(const char* raw_mode) {
    if (raw_mode == NULL) {
        return SUPER_EFFECT_QUALITY_FULL;
    }
    if (SDL_strcasecmp(raw_mode, "cached-bg") == 0) {
        return SUPER_EFFECT_QUALITY_CACHED_BG;
    }
    return SUPER_EFFECT_QUALITY_FULL;
}

static void init_super_effect_quality_mode(void) {
    const char* raw_mode = Config_GetString(CFG_KEY_SUPER_EFFECT_QUALITY);
    if (raw_mode == NULL || raw_mode[0] == '\0') {
#if defined(PORT_MISTER) || defined(PORT_MIYOO_MINI_PLUS)
        super_effect_quality_mode = SUPER_EFFECT_QUALITY_CACHED_BG;
#else
        super_effect_quality_mode = SUPER_EFFECT_QUALITY_FULL;
#endif
    } else {
        super_effect_quality_mode = parse_super_effect_quality_mode(raw_mode);
    }
}

static SuperEffectQualityMode current_renderer_super_effect_quality_mode(void) {
    return super_effect_quality_mode;
}

static FpsOverlayMode parse_fps_overlay_mode(const char* value) {
    if (value == NULL) return FPS_OVERLAY_OFF;
    if (SDL_strcasecmp(value, "fps") == 0) return FPS_OVERLAY_FPS;
    if (SDL_strcasecmp(value, "debug") == 0) return FPS_OVERLAY_DEBUG;
    return FPS_OVERLAY_OFF;
}

static void init_show_fps_overlay(void) {
    fps_overlay_mode = parse_fps_overlay_mode(Config_GetString(CFG_KEY_SHOW_FPS));
    fps_overlay_window_start_ns = 0;
    fps_overlay_window_frames = 0;
    fps_overlay_value = 0;
    fps_overlay_label[0] = '\0';
    fps_overlay_update_ns_accum = 0;
    fps_overlay_texrefresh_ns_accum = 0;
    fps_overlay_gamelogic_ns_accum = 0;
    fps_overlay_spritesubmit_ns_accum = 0;
    fps_overlay_dispatch_ns_accum = 0;
    fps_overlay_dtexrenew_ns_accum = 0;
    fps_overlay_dsprsubmit_ns_accum = 0;
    fps_overlay_render_ns_accum = 0;
    fps_overlay_sort_ns_accum = 0;
    fps_overlay_raster_ns_accum = 0;
    fps_overlay_resolve_ns_accum = 0;
    fps_overlay_raster_pass_ns_accum = 0;
    fps_overlay_packed_8px_hits_accum = 0;
    fps_overlay_neon_16px_direct_hits_accum = 0;
    fps_overlay_qsort_invocations_accum = 0;
    fps_overlay_present_ns_accum = 0;
    fps_overlay_frame_ns_accum = 0;
    fps_overlay_avg_resolve_ms = 0.0;
    fps_overlay_avg_raster_pass_ms = 0.0;
    fps_overlay_avg_packed_8px_per_frame = 0.0;
    fps_overlay_avg_neon_16px_per_frame = 0.0;
    fps_overlay_avg_qsort_per_frame = 0.0;
    fps_overlay_peak_active_effects = 0;
    fps_overlay_avg_update_ms = 0.0;
    fps_overlay_avg_texrefresh_ms = 0.0;
    fps_overlay_avg_gamelogic_ms = 0.0;
    fps_overlay_avg_spritesubmit_ms = 0.0;
    fps_overlay_avg_dispatch_ms = 0.0;
    fps_overlay_avg_dtexrenew_ms = 0.0;
    fps_overlay_avg_dsprsubmit_ms = 0.0;
    fps_overlay_avg_render_ms = 0.0;
    fps_overlay_avg_sort_ms = 0.0;
    fps_overlay_avg_raster_ms = 0.0;
    fps_overlay_avg_present_ms = 0.0;
    fps_overlay_avg_frame_ms = 0.0;
}

static void publish_fps_overlay_label(void) {
    if (fps_overlay_mode == FPS_OVERLAY_OFF) {
        fps_overlay_label[0] = '\0';
        FPSOverlay_SetText(NULL);
        return;
    }

    if (fps_overlay_mode == FPS_OVERLAY_FPS) {
        SDL_snprintf(fps_overlay_label, sizeof(fps_overlay_label), "%d", fps_overlay_value);
    }
    else if (fps_overlay_avg_frame_ms > 0.0) {
        /* Giblet renderer DEBUG line — compact u/r/p/t breakdown, plus the
         * sticky active-effect-slot peak as `e<peak>`. The peak is the one
         * consumer of fps_overlay_peak_active_effects; without this field it
         * is a write-only counter. */
        SDL_snprintf(fps_overlay_label, sizeof(fps_overlay_label),
                     "fps %d u %4.1f r %4.1f p %4.1f t %4.1f e%d",
                     fps_overlay_value,
                     fps_overlay_avg_update_ms,
                     fps_overlay_avg_render_ms,
                     fps_overlay_avg_present_ms,
                     fps_overlay_avg_frame_ms,
                     fps_overlay_peak_active_effects);
    } else {
        SDL_snprintf(fps_overlay_label, sizeof(fps_overlay_label), "%d FPS", fps_overlay_value);
    }
    FPSOverlay_SetText(fps_overlay_label);
}

#if ENABLE_PERF_TELEMETRY
static void fps_overlay_accumulate_timing(Uint64 update_ns, Uint64 texrefresh_ns,
                                          Uint64 gamelogic_ns, Uint64 spritesubmit_ns, Uint64 dispatch_ns,
                                          Uint64 dtexrenew_ns, Uint64 dsprsubmit_ns,
                                          Uint64 render_ns, Uint64 sort_ns, Uint64 raster_ns,
                                          Uint64 resolve_ns, Uint64 raster_pass_ns,
                                          Uint64 packed_8px_hits, Uint64 neon_16px_direct_hits,
                                          Uint64 qsort_invocations,
                                          Uint64 present_ns, Uint64 frame_work_ns) {
    fps_overlay_update_ns_accum += update_ns;
    fps_overlay_texrefresh_ns_accum += texrefresh_ns;
    fps_overlay_gamelogic_ns_accum += gamelogic_ns;
    fps_overlay_spritesubmit_ns_accum += spritesubmit_ns;
    fps_overlay_dispatch_ns_accum += dispatch_ns;
    fps_overlay_dtexrenew_ns_accum += dtexrenew_ns;
    fps_overlay_dsprsubmit_ns_accum += dsprsubmit_ns;
    fps_overlay_render_ns_accum += render_ns;
    fps_overlay_sort_ns_accum += sort_ns;
    fps_overlay_raster_ns_accum += raster_ns;
    fps_overlay_resolve_ns_accum += resolve_ns;
    fps_overlay_raster_pass_ns_accum += raster_pass_ns;
    fps_overlay_packed_8px_hits_accum += packed_8px_hits;
    fps_overlay_neon_16px_direct_hits_accum += neon_16px_direct_hits;
    fps_overlay_qsort_invocations_accum += qsort_invocations;
    fps_overlay_present_ns_accum += present_ns;
    fps_overlay_frame_ns_accum += frame_work_ns;
}
#endif /* ENABLE_PERF_TELEMETRY */

static void update_fps_overlay(Uint64 frame_end_ns) {
    if (fps_overlay_mode == FPS_OVERLAY_OFF) {
        return;
    }

    if (fps_overlay_window_start_ns == 0) {
        fps_overlay_window_start_ns = frame_end_ns;
        fps_overlay_window_frames = 1;
        return;
    }

    fps_overlay_window_frames += 1;
    const Uint64 elapsed_ns = frame_end_ns - fps_overlay_window_start_ns;
    if (elapsed_ns < 250000000ULL) {
        return;
    }

    /* When the pacer adjusts phase (blending toward ideal vsync alignment),
       frame delivery times shift without any actual frames being dropped.
       The pacer's jitter tracker counts genuinely late frames (>500 us past
       pre-blend deadline).  If none were late, all dips are from phase
       correction — report the target rate instead of the measured rate. */
    const int measured_fps = (native_video_writer_enabled && pacer_late_count == 0 && fps_overlay_window_frames > 0)
        ? (int)(1000000000.0 / (double)target_frame_time_ns + 0.5)
        : (int)((((double)fps_overlay_window_frames * 1000000000.0) / (double)elapsed_ns) + 0.5);

    /* Snapshot timing averages from the measurement window. */
    if (fps_overlay_window_frames > 0) {
        const double n = (double)fps_overlay_window_frames;
        fps_overlay_avg_update_ms = ((double)fps_overlay_update_ns_accum / n) / 1e6;
        fps_overlay_avg_texrefresh_ms = ((double)fps_overlay_texrefresh_ns_accum / n) / 1e6;
        fps_overlay_avg_gamelogic_ms = ((double)fps_overlay_gamelogic_ns_accum / n) / 1e6;
        fps_overlay_avg_spritesubmit_ms = ((double)fps_overlay_spritesubmit_ns_accum / n) / 1e6;
        fps_overlay_avg_dispatch_ms = ((double)fps_overlay_dispatch_ns_accum / n) / 1e6;
        fps_overlay_avg_dtexrenew_ms = ((double)fps_overlay_dtexrenew_ns_accum / n) / 1e6;
        fps_overlay_avg_dsprsubmit_ms = ((double)fps_overlay_dsprsubmit_ns_accum / n) / 1e6;
        fps_overlay_avg_render_ms = ((double)fps_overlay_render_ns_accum / n) / 1e6;
        fps_overlay_avg_sort_ms = ((double)fps_overlay_sort_ns_accum / n) / 1e6;
        fps_overlay_avg_raster_ms = ((double)fps_overlay_raster_ns_accum / n) / 1e6;
        fps_overlay_avg_resolve_ms = ((double)fps_overlay_resolve_ns_accum / n) / 1e6;
        fps_overlay_avg_raster_pass_ms = ((double)fps_overlay_raster_pass_ns_accum / n) / 1e6;
        fps_overlay_avg_packed_8px_per_frame = (double)fps_overlay_packed_8px_hits_accum / n;
        fps_overlay_avg_neon_16px_per_frame = (double)fps_overlay_neon_16px_direct_hits_accum / n;
        fps_overlay_avg_qsort_per_frame = (double)fps_overlay_qsort_invocations_accum / n;
        fps_overlay_avg_present_ms = ((double)fps_overlay_present_ns_accum / n) / 1e6;
        fps_overlay_avg_frame_ms = ((double)fps_overlay_frame_ns_accum / n) / 1e6;
    }
    fps_overlay_update_ns_accum = 0;
    fps_overlay_texrefresh_ns_accum = 0;
    fps_overlay_gamelogic_ns_accum = 0;
    fps_overlay_spritesubmit_ns_accum = 0;
    fps_overlay_dispatch_ns_accum = 0;
    fps_overlay_dtexrenew_ns_accum = 0;
    fps_overlay_dsprsubmit_ns_accum = 0;
    fps_overlay_render_ns_accum = 0;
    fps_overlay_sort_ns_accum = 0;
    fps_overlay_raster_ns_accum = 0;
    fps_overlay_resolve_ns_accum = 0;
    fps_overlay_raster_pass_ns_accum = 0;
    fps_overlay_packed_8px_hits_accum = 0;
    fps_overlay_neon_16px_direct_hits_accum = 0;
    fps_overlay_qsort_invocations_accum = 0;
    fps_overlay_present_ns_accum = 0;
    fps_overlay_frame_ns_accum = 0;

    /* Snapshot pacer stats for overlay */
    if (pacer_stats_frames > 0) {
        pacer_overlay_max_jitter_us = pacer_max_jitter_ns / 1000;
        pacer_overlay_avg_jitter_us = (pacer_jitter_sum_ns / pacer_stats_frames) / 1000;
        pacer_overlay_late_pct = (pacer_late_count * 100) / pacer_stats_frames;
        pacer_overlay_phase_us = (Uint64)((pacer_phase_error_ns < 0 ? -pacer_phase_error_ns : pacer_phase_error_ns) / 1000);
    } else {
        pacer_overlay_max_jitter_us = 0;
        pacer_overlay_avg_jitter_us = 0;
        pacer_overlay_late_pct = 0;
        pacer_overlay_phase_us = 0;
    }
    pacer_max_jitter_ns = 0;
    pacer_jitter_sum_ns = 0;
    pacer_late_count = 0;
    pacer_stats_frames = 0;

    fps_overlay_window_start_ns = frame_end_ns;
    fps_overlay_window_frames = 0;

    fps_overlay_value = measured_fps;
    publish_fps_overlay_label();
}

static void reset_fps_overlay_state(void) {
    fps_overlay_window_start_ns = 0;
    fps_overlay_window_frames = 0;
    fps_overlay_value = 0;
    fps_overlay_label[0] = '\0';
    fps_overlay_update_ns_accum = 0;
    fps_overlay_texrefresh_ns_accum = 0;
    fps_overlay_gamelogic_ns_accum = 0;
    fps_overlay_spritesubmit_ns_accum = 0;
    fps_overlay_dispatch_ns_accum = 0;
    fps_overlay_dtexrenew_ns_accum = 0;
    fps_overlay_dsprsubmit_ns_accum = 0;
    fps_overlay_render_ns_accum = 0;
    fps_overlay_sort_ns_accum = 0;
    fps_overlay_raster_ns_accum = 0;
    fps_overlay_resolve_ns_accum = 0;
    fps_overlay_raster_pass_ns_accum = 0;
    fps_overlay_packed_8px_hits_accum = 0;
    fps_overlay_neon_16px_direct_hits_accum = 0;
    fps_overlay_qsort_invocations_accum = 0;
    fps_overlay_present_ns_accum = 0;
    fps_overlay_frame_ns_accum = 0;
    fps_overlay_avg_resolve_ms = 0.0;
    fps_overlay_avg_raster_pass_ms = 0.0;
    fps_overlay_avg_packed_8px_per_frame = 0.0;
    fps_overlay_avg_neon_16px_per_frame = 0.0;
    fps_overlay_avg_qsort_per_frame = 0.0;
    fps_overlay_peak_active_effects = 0;
    fps_overlay_avg_update_ms = 0.0;
    fps_overlay_avg_texrefresh_ms = 0.0;
    fps_overlay_avg_gamelogic_ms = 0.0;
    fps_overlay_avg_spritesubmit_ms = 0.0;
    fps_overlay_avg_dispatch_ms = 0.0;
    fps_overlay_avg_dtexrenew_ms = 0.0;
    fps_overlay_avg_dsprsubmit_ms = 0.0;
    fps_overlay_avg_render_ms = 0.0;
    fps_overlay_avg_sort_ms = 0.0;
    fps_overlay_avg_raster_ms = 0.0;
    fps_overlay_avg_present_ms = 0.0;
    fps_overlay_avg_frame_ms = 0.0;

    FPSOverlay_SetText(NULL);
}

void SDLApp_ToggleFPSOverlay(void) {
    /* Re-read from config file on disk — the wrapper writes it before
       sending the signal, so reading it back stays in sync. */
    const char* pref_path = Paths_GetPrefPath();
    char* config_path = NULL;
    SDL_asprintf(&config_path, "%sconfig", pref_path);
    if (config_path != NULL) {
        FILE* f = fopen(config_path, "r");
        if (f != NULL) {
            char line[256];
            while (fgets(line, sizeof(line), f) != NULL) {
                char* eq = SDL_strchr(line, '=');
                if (eq == NULL) continue;
                *eq = '\0';
                char* key = line;
                while (*key == ' ' || *key == '\t') key++;
                char* key_end = key + SDL_strlen(key);
                while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
                *key_end = '\0';
                if (SDL_strcmp(key, "show-fps") != 0) continue;
                char* val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;
                char* val_end = val + SDL_strlen(val);
                while (val_end > val && (val_end[-1] == ' ' || val_end[-1] == '\t' ||
                       val_end[-1] == '\n' || val_end[-1] == '\r')) val_end--;
                *val_end = '\0';
                fps_overlay_mode = parse_fps_overlay_mode(val);
                break;
            }
            fclose(f);
        }
        SDL_free(config_path);
    }
    reset_fps_overlay_state();

    FPSOverlay_SetMode(fps_overlay_mode);

    static const char* mode_names[] = { "off", "fps", "debug" };
    backend_logf("FPS overlay: %s", mode_names[fps_overlay_mode]);
}

#if defined(PORT_MISTER)
static bool sysfs_write(const char* path, const char* value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        backend_logf("ARM clock: open(%s) failed: %s", path, strerror(errno));
        return false;
    }
    size_t len = strlen(value);
    ssize_t written = write(fd, value, len);
    close(fd);
    if (written < 0 || (size_t)written != len) {
        backend_logf("ARM clock: write(%s, \"%s\") failed: %s", path, value, strerror(errno));
        return false;
    }
    return true;
}

static bool sysfs_read(const char* path, char* buf, size_t buf_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n <= 0) return false;
    /* strip trailing whitespace */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) n--;
    buf[n] = '\0';
    return true;
}
#endif

#if defined(PORT_MISTER)
static bool pacer_rt_sched_active = false;
static bool pacer_mlock_active = false;
static Uint64 busywait_threshold_ns = 200000; /* default 0.2ms */

static void setup_realtime_scheduling(void) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
        pacer_mlock_active = true;
    } else {
        backend_logf("Frame pacer: mlockall failed (errno %d), continuing without", errno);
    }

    /* SCHED_FIFO at priority 49 for the game thread.  The SPU audio callback
       (SPU_SDL_CB in spu.c) boosts its own thread to priority 50 on first
       invocation, so the audio thread can always preempt the game thread to
       fill audio buffers and release soundLock.  This avoids the priority
       inversion that would otherwise cause audio stutter in heavy scenes. */
    struct sched_param sp;
    sp.sched_priority = 49;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0) {
        pacer_rt_sched_active = true;
    } else {
        backend_logf("Frame pacer: SCHED_FIFO failed (errno %d), continuing with normal scheduling", errno);
    }
}

static void teardown_realtime_scheduling(void) {
    if (pacer_rt_sched_active) {
        struct sched_param sp;
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);
        pacer_rt_sched_active = false;
    }
    if (pacer_mlock_active) {
        munlockall();
        pacer_mlock_active = false;
    }
}

static void precise_delay_ns(Uint64 target_wakeup_ns) {
    Uint64 now = SDL_GetTicksNS();
    if (now >= target_wakeup_ns) return;
    Uint64 remaining = target_wakeup_ns - now;
    if (remaining > busywait_threshold_ns) {
        SDL_DelayNS(remaining - busywait_threshold_ns);
    }
    while (SDL_GetTicksNS() < target_wakeup_ns) {
#if defined(__arm__) || defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }
}

static void poll_vsync_feedback(void) {
    if (!native_video_writer_enabled || vsync_feedback_disabled) return;

    uint32_t seq1 = NativeVideoWriter_ReadFeedbackSeq();
    uint32_t word = NativeVideoWriter_ReadFeedback();
    uint32_t seq2 = NativeVideoWriter_ReadFeedbackSeq();

    if (seq1 != seq2 || seq1 == 0) return;  /* torn read or no data yet */
    if (seq1 == last_feedback_seq) return;   /* no new data */

    last_feedback_seq = seq1;

    uint8_t cnt = NV_FeedbackFrameCounter(word);
    uint32_t ts_us = NV_FeedbackTimestampUs(word);

    if (cnt == last_fpga_frame_cnt) return;  /* same counter, skip */

    /* Compute how long ago the wrapper observed this vsync.
       The wrapper wrote a 24-bit CLOCK_MONOTONIC microsecond timestamp.
       We must compare against the same clock (not SDL_GetTicksNS which
       counts from SDL init, not boot). */
    struct timespec mono_ts;
    clock_gettime(CLOCK_MONOTONIC, &mono_ts);
    uint32_t mono_us = (uint32_t)(mono_ts.tv_sec * 1000000ULL + mono_ts.tv_nsec / 1000) & 0x00FFFFFF;
    int32_t delta_us = (int32_t)((mono_us - ts_us) & 0x00FFFFFF);
    if (delta_us > 0x00800000) delta_us -= 0x01000000;  /* handle wrap */

    if (delta_us < 0 || delta_us >= 200000) {
        return;  /* stale or bogus, skip (200ms is far beyond the ~1ms normal) */
    }

    /* Convert to SDL time domain for the frame pacer (which uses SDL_GetTicksNS) */
    Uint64 now_ns = SDL_GetTicksNS();
    if ((Uint64)delta_us * 1000 > now_ns) {
        return;  /* feedback predates SDL init — skip to avoid unsigned underflow */
    }
    Uint64 vsync_time_ns = now_ns - (Uint64)delta_us * 1000;

    last_fpga_frame_cnt = cnt;
    last_vsync_monotonic_ns = vsync_time_ns;
    last_feedback_update_ns = now_ns;
    if (!vsync_feedback_valid) {
        backend_logf("Frame pacer: closed-loop vsync feedback engaged (lead_time=%llu us)",
                     (unsigned long long)(lead_time_ns / 1000));
    }
    vsync_feedback_valid = true;
}
#endif /* PORT_MISTER */

static void apply_arm_clock(int mode) {
#if defined(PORT_MISTER)
    const char* freq = "800000";
    if (mode == 1) freq = "1000000";
    else if (mode == 2) freq = "1200000";

    /* Pin governor to performance so it doesn't downclock during frame sleep */
    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "performance");

    /* Determine direction: kernel rejects min > max and max < min, so we must
       write in the right order depending on whether we're raising or lowering. */
    char cur_max[32] = {};
    bool have_cur = sysfs_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
                               cur_max, sizeof(cur_max));
    long target = strtol(freq, NULL, 10);
    long current = have_cur ? strtol(cur_max, NULL, 10) : 0;

    if (target >= current) {
        /* Raising: max first (so min write doesn't exceed old max) */
        if (!sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", freq)) {
            backend_logf("ARM clock: failed to set scaling_max_freq (driver may not be loaded)");
            return;
        }
        sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", freq);
    } else {
        /* Lowering: min first (so max write doesn't go below old min) */
        sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", freq);
        if (!sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", freq)) {
            backend_logf("ARM clock: failed to set scaling_max_freq");
            return;
        }
    }

    /* Read back and verify */
    char readback[32] = {};
    if (sysfs_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", readback, sizeof(readback))) {
        if (strcmp(readback, freq) != 0) {
            backend_logf("ARM clock: WARNING scaling_max_freq readback mismatch: wanted %s, got %s", freq, readback);
        }
    }
#else
    (void)mode;
#endif
}

static const char* arm_clock_mode_label(int mode) {
    if (mode == 1) return "1000MHz";
    if (mode == 2) return "1200MHz";
    return "stock (800MHz)";
}

static int parse_arm_clock_mode(const char* raw_value) {
    if (raw_value == NULL) return 0;
    if (SDL_strcmp(raw_value, "1000") == 0) return 1;
    if (SDL_strcmp(raw_value, "1200") == 0) return 2;
    return 0;
}

static void init_arm_clock(void) {
    const char* raw_value = Config_GetString(CFG_KEY_ARM_CLOCK);
    if (raw_value == NULL || raw_value[0] == '\0') {
        arm_clock_mode = 0; /* default to stock 800MHz */
    } else {
        arm_clock_mode = parse_arm_clock_mode(raw_value);
    }
    apply_arm_clock(arm_clock_mode);
}

int SDLApp_GetArmClock(void) {
    return arm_clock_mode;
}

void SDLApp_CycleArmClock(void) {
    /* Re-read arm-clock from the config file on disk.  The wrapper writes
       the file on every OSD click before sending the signal, so reading it
       back is always in sync with what the OSD displays.  This avoids
       desync caused by signal key-repeat (multiple signals per click). */
    const char* pref_path = Paths_GetPrefPath();
    char* config_path = NULL;
    SDL_asprintf(&config_path, "%sconfig", pref_path);
    if (config_path != NULL) {
        FILE* f = fopen(config_path, "r");
        if (f != NULL) {
            char line[256];
            while (fgets(line, sizeof(line), f) != NULL) {
                /* Look for "arm-clock = <value>" */
                char* eq = SDL_strchr(line, '=');
                if (eq == NULL) continue;
                *eq = '\0';
                /* Trim key */
                char* key = line;
                while (*key == ' ' || *key == '\t') key++;
                char* key_end = key + SDL_strlen(key);
                while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
                *key_end = '\0';
                if (SDL_strcmp(key, "arm-clock") != 0) continue;
                /* Trim value */
                char* val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;
                char* val_end = val + SDL_strlen(val);
                while (val_end > val && (val_end[-1] == ' ' || val_end[-1] == '\t' ||
                       val_end[-1] == '\n' || val_end[-1] == '\r')) val_end--;
                *val_end = '\0';
                arm_clock_mode = parse_arm_clock_mode(val);
                break;
            }
            fclose(f);
        }
        SDL_free(config_path);
    }
    /* Don't apply the clock change live — changing frequency via sysfs while
       the game is running causes freezes on some DE10-Nano boards.  The new
       setting will take effect on the next game restart via init_arm_clock(). */
    backend_logf("ARM clock: %s (applies on restart)", arm_clock_mode_label(arm_clock_mode));
}

static void init_game_mode(void) {
    const char* raw_value = Config_GetString(CFG_KEY_GAME_MODE);
    if (raw_value != NULL && SDL_strcasecmp(raw_value, "arcade") == 0) {
        game_mode_arcade = true;
    } else {
        game_mode_arcade = false;
    }
}

void SDLApp_CycleGameMode(void) {
    /* Re-read game-mode from the config file on disk.  The wrapper writes
       the file on every OSD click before sending the signal, so reading it
       back is always in sync with what the OSD displays. */
    const char* pref_path = Paths_GetPrefPath();
    char* config_path = NULL;
    SDL_asprintf(&config_path, "%sconfig", pref_path);
    if (config_path != NULL) {
        FILE* f = fopen(config_path, "r");
        if (f != NULL) {
            char line[256];
            while (fgets(line, sizeof(line), f) != NULL) {
                /* Look for "game-mode = <value>" */
                char* eq = SDL_strchr(line, '=');
                if (eq == NULL) continue;
                *eq = '\0';
                /* Trim key */
                char* key = line;
                while (*key == ' ' || *key == '\t') key++;
                char* key_end = key + SDL_strlen(key);
                while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
                *key_end = '\0';
                if (SDL_strcmp(key, "game-mode") != 0) continue;
                /* Trim value */
                char* val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;
                char* val_end = val + SDL_strlen(val);
                while (val_end > val && (val_end[-1] == ' ' || val_end[-1] == '\t' ||
                       val_end[-1] == '\n' || val_end[-1] == '\r')) val_end--;
                *val_end = '\0';
                game_mode_arcade = (SDL_strcasecmp(val, "arcade") == 0);
                break;
            }
            fclose(f);
        }
        SDL_free(config_path);
    }
    backend_logf("Game mode: %s", game_mode_arcade ? "arcade" : "console");
}

bool SDLApp_IsArcadeGameMode(void) {
    return game_mode_arcade;
}

void SDLApp_ForceConsoleGameMode(void) {
    /* Session-only override — does not rewrite the on-disk config. The
     * next launch will re-read whatever the user has saved. */
    game_mode_arcade = false;
}

/* The restore half of the force above; see the header. Shipped in every
 * flavor because the replay viewer's scoped unpin (replay_player.c ->
 * ReplayPlayer_UnpinConfig) is release code, not a DEBUG experiment. */
void SDLApp_SetArcadeGameMode(bool arcade) {
    game_mode_arcade = arcade;
}

#if defined(DEBUG)
void SDLApp_ForceArcadeGameMode(void) {
    /* Step B3 EXPERIMENT (docs/plan-fcade-replay-browser.md): session-only
     * override, same contract as SDLApp_ForceConsoleGameMode above. */
    game_mode_arcade = true;
}
#endif

static void init_hold_to_pause(void) {
    const char* raw_value = Config_GetString(CFG_KEY_HOLD_TO_PAUSE);
    if (raw_value != NULL && SDL_strcasecmp(raw_value, "on") == 0) {
        hold_to_pause = true;
    } else {
        hold_to_pause = false;
    }
}

void SDLApp_CycleHoldToPause(void) {
    const char* pref_path = Paths_GetPrefPath();
    char* config_path = NULL;
    SDL_asprintf(&config_path, "%sconfig", pref_path);
    if (config_path != NULL) {
        FILE* f = fopen(config_path, "r");
        if (f != NULL) {
            char line[256];
            while (fgets(line, sizeof(line), f) != NULL) {
                char* eq = SDL_strchr(line, '=');
                if (eq == NULL) continue;
                *eq = '\0';
                char* key = line;
                while (*key == ' ' || *key == '\t') key++;
                char* key_end = key + SDL_strlen(key);
                while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
                *key_end = '\0';
                if (SDL_strcmp(key, "hold-to-pause") != 0) continue;
                char* val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;
                char* val_end = val + SDL_strlen(val);
                while (val_end > val && (val_end[-1] == ' ' || val_end[-1] == '\t' ||
                       val_end[-1] == '\n' || val_end[-1] == '\r')) val_end--;
                *val_end = '\0';
                hold_to_pause = (SDL_strcasecmp(val, "on") == 0);
                break;
            }
            fclose(f);
        }
        SDL_free(config_path);
    }
    backend_logf("Hold to pause: %s", hold_to_pause ? "on" : "off");
}

bool SDLApp_IsHoldToPauseEnabled(void) {
    return hold_to_pause;
}

static bool init_window() {
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    if (Config_GetBool(CFG_KEY_FULLSCREEN)) {
        window_flags |= SDL_WINDOW_FULLSCREEN;
    }

    int window_width = Config_GetInt(CFG_KEY_WINDOW_WIDTH);

    if (window_width < window_min_width) {
        window_width = window_min_width;
    }

    int window_height = Config_GetInt(CFG_KEY_WINDOW_HEIGHT);

    if (window_height < window_min_height) {
        window_height = window_min_height;
    }

    if (!SDL_CreateWindowAndRenderer(app_name, window_width, window_height, window_flags, &window, &renderer)) {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        backend_logf("SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        return false;
    }

    const char* selected_video_driver = SDL_GetCurrentVideoDriver();
    const char* selected_renderer = SDL_GetRendererName(renderer);
    backend_logf("Selected video driver: %s", selected_video_driver != NULL ? selected_video_driver : "(null)");
    backend_logf("Selected renderer: %s", selected_renderer != NULL ? selected_renderer : "(null)");
    backend_logf("Scale mode startup: source=%s requested=%s resolved=%s",
                 scale_mode_startup_source,
                 scale_mode_requested_value[0] != '\0' ? scale_mode_requested_value : "(null)",
                 scale_mode_name(scale_mode));

#if defined(PORT_MISTER)
    FPSOverlay_SetMode(fps_overlay_mode);
    {
        static const char* mode_names[] = { "off", "fps", "debug" };
        backend_logf("FPS overlay: %s", mode_names[fps_overlay_mode]);
    }

    /* Native video: write frames directly to DDR3 for the FPGA's native video
       reader instead of going through the Linux framebuffer scaler path.
       Enabled by default; set THIRDSARM_NATIVE_VIDEO=0 to disable. */
    {
        const char* native_video_env = SDL_getenv("THIRDSARM_NATIVE_VIDEO");
        if (!native_video_env || SDL_strcmp(native_video_env, "0") != 0) {
            native_video_writer_enabled = NativeVideoWriter_Init();
            backend_logf("Native video writer: %s", native_video_writer_enabled ? "enabled" : "disabled");

            /* Match ARM frame pacing to the FPGA's pixel-clock-derived refresh
               rate.  With the dedicated video PLL (31.1538 MHz, 495x264),
               NV_TARGET_FPS = TARGET_FPS = 59.59949 Hz (1.4 uHz error).
               H-freq = 15,734 Hz (NTSC standard). No compensation needed. */
            if (native_video_writer_enabled) {
                target_frame_time_ns = (Uint64)(1000000000.0 / NV_TARGET_FPS);
                backend_logf("Native video: frame pacing at %.4f Hz (FPGA PLL rate)", NV_TARGET_FPS);

                const char* bw_env = SDL_getenv("THIRDSARM_BUSYWAIT_US");
                if (bw_env) {
                    int val = SDL_atoi(bw_env);
                    if (val >= 0 && val <= 5000) {
                        busywait_threshold_ns = (Uint64)val * 1000;
                    }
                }

                /* Vsync feedback kill switch */
                const char* vf_env = SDL_getenv("THIRDSARM_VSYNC_FEEDBACK");
                if (vf_env && SDL_strcmp(vf_env, "0") == 0) {
                    vsync_feedback_disabled = true;
                    backend_logf("Frame pacer: vsync feedback disabled by THIRDSARM_VSYNC_FEEDBACK=0");
                }

                /* Lead time tuning */
                const char* lt_env = SDL_getenv("THIRDSARM_LEAD_TIME_US");
                if (lt_env) {
                    int val = SDL_atoi(lt_env);
                    if (val >= 100 && val <= 10000) {
                        lead_time_ns = (Uint64)val * 1000;
                        backend_logf("Frame pacer: lead time set to %d us", val);
                    }
                }
            }

            /* Native video requires software frame mode to get the ARGB8888
               surface.  Force it on regardless of the user config setting. */
            if (native_video_writer_enabled && !software_frame_mode_enabled) {
                software_frame_mode_enabled = true;
                backend_logf("Native video: auto-enabled software frame mode");
            }
        }
    }

    /* Post-Phase-C note: previously, when the dummy SDL video driver was
     * selected (the default on MiSTer per startup_generated_defaults at
     * `CFG_KEY_VIDEO_DRIVER_ORDER = "dummy"`), the fbdev_presenter init block
     * also called `SDL_SetWindowSize(window, FBDevPresenter_GetWidth(),
     * FBDevPresenter_GetHeight())` to size the dummy window to the Linux
     * framebuffer. With fbdev_presenter ripped out, the present path on
     * MiSTer is `native_video_writer` (DDR3 direct write at fixed 384x224)
     * and the dummy SDL window's size has no consumer that affects output —
     * the only `SDL_GetWindowSize` consumers in the tree are imgui (which
     * cannot render under the dummy driver). Leaving the dummy window at
     * its initial create-time dims (CFG_KEY_WINDOW_WIDTH/HEIGHT) is fine. */

#endif

    if (scale_mode_uses_native_render_path()) {
        use_native_render_path = true;
        backend_logf("Native render path: enabled (scale-mode=%s)", scale_mode_name(scale_mode));
    }
#if defined(PORT_MIYOO_MINI_PLUS)
    /* Force native render path on Miyoo regardless of scale mode. The
     * dummy/evdev SDL3 video driver has no real on-screen surface, and
     * ArmDisplay_Present consumes giblet's RGB565 canvas directly via
     * MI_GFX. The non-native render path uploads the canvas to an SDL3
     * internal screen_texture via SDL_UpdateTexture+SDL_RenderTexture
     * (~27ms/frame at 1600 MHz) that is never read by anyone — pure
     * waste. Native path skips that work and just does a couple of
     * cheap SDL renderer calls that no-op on the dummy backend. */
    if (!use_native_render_path) {
        use_native_render_path = true;
        backend_logf("Native render path: forced ON for Miyoo Mini Plus");
    }
#endif

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    return true;
}

int SDLApp_PreInit() {
    SDL_SetAppMetadata(app_name, "0.1", NULL);
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_PREFER_LIBDECOR, "1");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#if defined(PORT_MISTER) || defined(PORT_MIYOO_MINI_PLUS)
    // On MiSTer / Miyoo we run without a focused desktop window; keep controller input active.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Ensure console key events do not bleed into the underlying Linux VT while the game is running.
    SDL_SetHint(SDL_HINT_MUTE_CONSOLE_KEYBOARD, "1");
#endif
#if defined(PORT_MISTER)
    // MiSTer wrapper exposes the gamepad as /dev/input/js0; prefer that.
    SDL_SetHint(SDL_HINT_JOYSTICK_LINUX_CLASSIC, "1");
#endif
    // On Miyoo we deliberately do NOT set JOYSTICK_LINUX_CLASSIC: Onion's
    // BSP exposes the gamepad as a /dev/input/event* (keyboard-emulating
    // evdev device emitted by the keymon daemon), not as /dev/input/js*.
    // SDL3's evdev gamepad backend is what we want here.
    apply_backend_hints();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return 1;
    }

    // Tier-1 netplay diag (Item 8): ALSA underrun spam. SDL3's audio backend
    // forwards libasound under-run prints into the AUDIO log category at INFO
    // priority. On a tight session we accumulate ~100 of these per session
    // and they crowd out the [netplay …] heartbeat lines we actually want.
    // CRITICAL still surfaces real failures (SDL_InitSubSystem audio failure
    // paths use SDL_Log directly above, not the AUDIO category logger).
    SDL_SetLogPriority(SDL_LOG_CATEGORY_AUDIO, SDL_LOG_PRIORITY_CRITICAL);

    return 0;
}

int SDLApp_FullInit() {
    Config_Init();
    Keymap_Init();
    init_scalemode();
    init_software_frame_mode();
    init_super_effect_quality_mode();
    init_arm_clock();
    init_game_mode();
    init_hold_to_pause();
    init_show_fps_overlay();

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        SDL_Log("Couldn't initialize SDL gamepad: %s", SDL_GetError());
        return 1;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        const char* error_with_audio = SDL_GetError();
        SDL_Log("Couldn't initialize SDL audio: %s", error_with_audio);
        backend_logf("SDL_InitSubSystem audio failed: %s", error_with_audio);
        backend_logf("Continuing without SDL audio subsystem");
    }

    log_backend_diagnostics();

    if (!init_window()) {
        SDL_Log("Couldn't initialize SDL window: %s", SDL_GetError());
        return 1;
    }

    // Initialize rendering subsystems
    SDLMessageRenderer_Initialize(renderer);
#if defined(PORT_MIYOO_MINI_PLUS)
    if (!ArmDisplay_Init()) {
        SDL_Log("ArmDisplay_Init failed");
        return 1;
    }
#endif
    if (!SoftwareRenderer_Init(true /* nearest_filter */, 1 /* scale */)) {
        SDL_Log("SoftwareRenderer_Init failed");
        return 1;
    }
    (void)software_frame_mode_enabled;
    (void)current_renderer_super_effect_quality_mode();
#if defined(ENABLE_NETPLAY)
    /* Sparse effect-pool save (Option A) kill switch. Default true; explicit
     * override only. Off forces the legacy full-state save path for A/B
     * parity testing without rebuilding. */
    if (Config_HasExplicitKey(CFG_KEY_NETPLAY_SPARSE_EFFECT_SAVE_ENABLED)) {
        const bool enabled = Config_GetBool(CFG_KEY_NETPLAY_SPARSE_EFFECT_SAVE_ENABLED);
        Netplay_SetSparseEffectSaveEnabled(enabled);
        backend_logf("netplay-sparse-effect-save-enabled = %s",
                     enabled ? "true" : "false");
    }
#endif
    backend_logf("Software frame mode: %s", software_frame_mode_name());
    backend_logf("Super effect quality: %s", super_effect_quality_mode_name(super_effect_quality_mode));
    backend_logf("ARM clock: %s", arm_clock_mode_label(arm_clock_mode));
    ScanlineRenderer_Init(renderer);

#if defined(DEBUG)
    SDLDebugText_Initialize(renderer);
#endif

    // Initialize screen texture
    create_screen_texture();

    // Initialize pads
    SDLPad_Init();

#if defined(PORT_MISTER)
    if (native_video_writer_enabled) {
        setup_realtime_scheduling();
        backend_logf("Frame pacer: software PLL, busy-wait %lluus, SCHED_FIFO=%s, mlock=%s, vsync_feedback=%s, lead_time=%lluus",
                     (unsigned long long)(busywait_threshold_ns / 1000),
                     pacer_rt_sched_active ? "yes" : "no",
                     pacer_mlock_active ? "yes" : "no",
                     vsync_feedback_disabled ? "disabled" : "enabled",
                     (unsigned long long)(lead_time_ns / 1000));
    }
#endif

#if defined(DEBUG)
    ImGuiW_Init(window, renderer);
#endif

    return 0;
}

void SDLApp_Quit() {
#if defined(PORT_MISTER)
    teardown_realtime_scheduling();
#endif
    apply_arm_clock(0);
    Config_Destroy();
    SoftwareRenderer_Quit();
#if defined(PORT_MIYOO_MINI_PLUS)
    /* ArmDisplay_Shutdown must run AFTER SoftwareRenderer_Quit so the
     * MMA-allocated canvas (Miyoo zero-copy path) is freed via
     * mi_gfx_free_canvas while MI_SYS is still initialized. Gated on
     * PORT_MIYOO_MINI_PLUS to mirror the Init gate above. */
    ArmDisplay_Shutdown();
#endif
    if (giblet_present_texture != NULL) {
        SDL_DestroyTexture(giblet_present_texture);
        giblet_present_texture = NULL;
    }
    NativeVideoWriter_Shutdown();
    SDL_DestroyTexture(screen_texture);
    ScanlineRenderer_Destroy();

#if defined(DEBUG)
    ImGuiW_Finish();
#endif

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
#if ENABLE_PERF_TELEMETRY
    perf_capture_reset_storage();
#endif
    SDL_Quit();
}

#if defined(DEBUG)
static void toggle_debug_window_visibility(SDL_KeyboardEvent* event) {
    if ((event->key == SDLK_GRAVE) && event->down && !event->repeat) {
        ImGuiW_ToggleVisivility();
    }
}
#endif

static void handle_fullscreen_toggle(SDL_KeyboardEvent* event) {
    const bool is_alt_enter = (event->key == SDLK_RETURN) && (event->mod & SDL_KMOD_ALT);
    const bool is_f11 = (event->key == SDLK_F11);
    const bool correct_key = (is_alt_enter || is_f11);

    if (!correct_key || !event->down || event->repeat) {
        return;
    }

    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);

    if (flags & SDL_WINDOW_FULLSCREEN) {
        SDL_SetWindowFullscreen(window, false);
    } else {
        SDL_SetWindowFullscreen(window, true);
    }

    native_output_rect_dirty = true;
    screen_output_rect_dirty = true;
}

static void handle_mouse_motion() {
    last_mouse_motion_time = SDL_GetTicks();
    SDL_ShowCursor();
}

static void hide_cursor_if_needed() {
    const Uint64 now = SDL_GetTicks();

    if ((last_mouse_motion_time > 0) && ((now - last_mouse_motion_time) > mouse_hide_delay_ms)) {
        SDL_HideCursor();
    }
}

bool SDLApp_PollEvents() {
    SDL_Event event;
    bool continue_running = true;

    while (SDL_PollEvent(&event)) {
#if defined(PORT_MIYOO_MINI_PLUS)
        static int dbg_event_count = 0;
        if (dbg_event_count < 500) {
            switch (event.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                fprintf(stderr, "[ev] KEY_%s scancode=%d (%s) key=%d kbd_id=%u\n",
                        event.type == SDL_EVENT_KEY_DOWN ? "DOWN" : "UP",
                        (int)event.key.scancode, SDL_GetScancodeName(event.key.scancode),
                        (int)event.key.key, (unsigned)event.key.which);
                ++dbg_event_count;
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                fprintf(stderr, "[ev] PAD_%s button=%d which=%u\n",
                        event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? "DOWN" : "UP",
                        (int)event.gbutton.button, (unsigned)event.gbutton.which);
                ++dbg_event_count;
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                fprintf(stderr, "[ev] PAD_AXIS axis=%d value=%d which=%u\n",
                        (int)event.gaxis.axis, (int)event.gaxis.value, (unsigned)event.gaxis.which);
                ++dbg_event_count;
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
                fprintf(stderr, "[ev] JOY_%s button=%d which=%u\n",
                        event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ? "DOWN" : "UP",
                        (int)event.jbutton.button, (unsigned)event.jbutton.which);
                ++dbg_event_count;
                break;
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                fprintf(stderr, "[ev] JOY_HAT hat=%d value=%d which=%u\n",
                        (int)event.jhat.hat, (int)event.jhat.value, (unsigned)event.jhat.which);
                ++dbg_event_count;
                break;
            case SDL_EVENT_JOYSTICK_AXIS_MOTION:
                fprintf(stderr, "[ev] JOY_AXIS axis=%d value=%d which=%u\n",
                        (int)event.jaxis.axis, (int)event.jaxis.value, (unsigned)event.jaxis.which);
                ++dbg_event_count;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
            case SDL_EVENT_JOYSTICK_ADDED:
            case SDL_EVENT_JOYSTICK_REMOVED:
            case SDL_EVENT_KEYBOARD_ADDED:
            case SDL_EVENT_KEYBOARD_REMOVED:
                fprintf(stderr, "[ev] device-event type=%u which=%u\n",
                        (unsigned)event.type, (unsigned)event.gdevice.which);
                ++dbg_event_count;
                break;
            }
        }
#endif

#if defined(DEBUG)
        ImGuiW_ProcessEvent(&event);
#endif

        switch (event.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            SDLPad_HandleGamepadDeviceEvent(&event.gdevice);
            break;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            // set_screenshot_flag_if_needed(&event.key);

#if defined(DEBUG)
            toggle_debug_window_visibility(&event.key);
#endif

            handle_fullscreen_toggle(&event.key);
            break;

        case SDL_EVENT_MOUSE_MOTION:
            handle_mouse_motion();
            break;

        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            create_screen_texture();
            break;

        case SDL_EVENT_QUIT:
            continue_running = false;
            break;
        }
    }

    return continue_running;
}

void SDLApp_BeginFrame() {
#if ENABLE_PERF_TELEMETRY
    perf_frame_start_ns = SDL_GetTicksNS();
    perf_update_start_ns = perf_frame_start_ns;
    perf_frame_index++;
#endif

    SDLMessageRenderer_BeginFrame();
    /* Giblet renderer has no BeginFrame hook — its state machine is
     * implicit; SoftwareRenderer_RenderFrame() does setup+raster+commit
     * in one shot. */
    (void)perf_capture_collect_extended_stats;

#if defined(DEBUG)
    ImGuiW_BeginFrame();
#endif
}

static void center_rect(SDL_FRect* rect, int win_w, int win_h) {
    rect->x = (win_w - rect->w) / 2;
    rect->y = (win_h - rect->h) / 2;
}

static SDL_FRect fit_4_by_3_rect(int win_w, int win_h) {
    SDL_FRect rect;
    rect.w = win_w;
    rect.h = win_w / display_target_ratio;

    if (rect.h > win_h) {
        rect.h = win_h;
        rect.w = win_h * display_target_ratio;
    }

    center_rect(&rect, win_w, win_h);
    return rect;
}

static SDL_FRect fit_native_rect(int win_w, int win_h) {
    SDL_FRect rect;
    rect.w = native_game_width;
    rect.h = native_game_height;
    center_rect(&rect, win_w, win_h);
    return rect;
}

static SDL_FRect fit_integer_rect(int win_w, int win_h, int pixel_w, int pixel_h) {
    const int virtual_w = win_w / pixel_w;
    const int virtual_h = win_h / pixel_h;
    const int scale_w = virtual_w / native_game_width;
    const int scale_h = virtual_h / native_game_height;
    int scale = (scale_h < scale_w) ? scale_h : scale_w;

    // Better to show a cropped image than nothing at all
    if (scale < 1) {
        scale = 1;
    }

    SDL_FRect rect;
    rect.w = scale * native_game_width * pixel_w;
    rect.h = scale * native_game_height * pixel_h;
    center_rect(&rect, win_w, win_h);
    return rect;
}

static SDL_FRect get_letterbox_rect(int win_w, int win_h) {
    switch (scale_mode) {
    case SCALEMODE_NATIVE:
        return fit_native_rect(win_w, win_h);

    case SCALEMODE_NEAREST:
    case SCALEMODE_LINEAR:
    case SCALEMODE_SOFT_LINEAR:
        return fit_4_by_3_rect(win_w, win_h);

    case SCALEMODE_INTEGER:
        // In order to scale a 384x224 buffer to 4:3 we need to stretch the image vertically by 9 / 7
        return fit_integer_rect(win_w, win_h, 7, 9);

    case SCALEMODE_SQUARE_PIXELS:
        return fit_integer_rect(win_w, win_h, 1, 1);

    default:
        return fit_4_by_3_rect(win_w, win_h);
    }

    return fit_4_by_3_rect(win_w, win_h);
}

static bool rect_has_letterbox(const SDL_FRect* rect, int target_w, int target_h) {
    return (rect->x > 0.0f) || (rect->y > 0.0f) || ((rect->x + rect->w) < (float)target_w) ||
           ((rect->y + rect->h) < (float)target_h);
}

static void render_renderer_fps_overlay(const SDL_FRect* content_rect) {
    if (fps_overlay_mode == FPS_OVERLAY_OFF || native_video_writer_enabled || (fps_overlay_label[0] == '\0')) {
        return;
    }

    int output_w = 0;
    int output_h = 0;
    if (!SDL_GetRenderOutputSize(renderer, &output_w, &output_h)) {
        return;
    }

    SDL_FRect draw_rect = { 0.0f, 0.0f, (float)output_w, (float)output_h };
    if ((content_rect != NULL) && (content_rect->w > 0.0f) && (content_rect->h > 0.0f)) {
        draw_rect = *content_rect;
    }

    const int scale = draw_rect.h >= 420.0f ? 2 : 1;
    /* Split fps_overlay_label on '\n' so each line renders independently —
     * SDL_RenderDebugText draws a single line, embedded newlines render as
     * garbage glyphs without this. Maximum 4 lines (more than enough for the
     * current two-line debug overlay). */
    const char* line_starts[4] = { fps_overlay_label, NULL, NULL, NULL };
    int line_lengths[4] = { 0, 0, 0, 0 };
    int line_count = 1;
    {
        const int label_len = (int)SDL_strlen(fps_overlay_label);
        int line_start = 0;
        for (int i = 0; i < label_len && line_count < 4; i++) {
            if (fps_overlay_label[i] == '\n') {
                line_lengths[line_count - 1] = i - line_start;
                line_start = i + 1;
                line_starts[line_count] = fps_overlay_label + line_start;
                line_count++;
            }
        }
        line_lengths[line_count - 1] = label_len - line_start;
    }
    int max_line_len = 0;
    for (int i = 0; i < line_count; i++) {
        if (line_lengths[i] > max_line_len) {
            max_line_len = line_lengths[i];
        }
    }
    const int text_w = max_line_len * 8 * scale;
    const int line_h = 8 * scale;
    const int line_gap = scale; /* small gap between stacked lines */
    const int text_h = line_count * line_h + (line_count - 1) * line_gap;
    const int margin = SDL_max(10, scale * 4);
    float draw_x, draw_y;
    if (fps_overlay_mode == FPS_OVERLAY_FPS) {
        draw_x = draw_rect.x + (float)margin;
        draw_y = draw_rect.y + (float)margin;
    } else {
        draw_x = draw_rect.x + ((draw_rect.w - (float)text_w) * 0.5f);
        draw_y = (draw_rect.y + draw_rect.h) - (float)text_h - (float)margin;
    }

    SDL_SetRenderScale(renderer, (float)scale, (float)scale);
    /* SDL_RenderDebugText takes a NUL-terminated string; we copy each split
     * line into a small stack buffer, NUL-terminating at the embedded '\n'
     * position for that line, then render. */
    char line_buf[128];
    for (int i = 0; i < line_count; i++) {
        const int copy_len = line_lengths[i] < (int)sizeof(line_buf) - 1
                                 ? line_lengths[i]
                                 : (int)sizeof(line_buf) - 1;
        SDL_memcpy(line_buf, line_starts[i], (size_t)copy_len);
        line_buf[copy_len] = '\0';
        const float line_y = draw_y + (float)(i * (line_h + line_gap));
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDebugText(renderer, (draw_x + 1.0f) / (float)scale, (line_y + 1.0f) / (float)scale, line_buf);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDebugText(renderer, draw_x / (float)scale, line_y / (float)scale, line_buf);
    }
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

static bool get_native_output_size(int* out_w, int* out_h) {
    return SDL_GetRenderOutputSize(renderer, out_w, out_h);
}

static void refresh_native_output_rect() {
    if (!use_native_render_path) {
        return;
    }

    int render_w = 0;
    int render_h = 0;
    if (!get_native_output_size(&render_w, &render_h)) {
        // Safe fallback: keep clearing until output size can be queried again.
        native_output_has_bars = true;
        return;
    }

    const bool output_size_changed = (render_w != native_output_width) || (render_h != native_output_height);
    if (!native_output_rect_dirty && !output_size_changed) {
        return;
    }

    native_output_width = render_w;
    native_output_height = render_h;
    native_output_rect = get_letterbox_rect(render_w, render_h);
    native_output_has_bars = rect_has_letterbox(&native_output_rect, render_w, render_h);
    native_output_rect_dirty = false;
}

static void refresh_screen_output_rect() {
    if (use_native_render_path || (screen_texture == NULL)) {
        return;
    }

    const bool output_size_changed =
        (screen_texture->w != screen_output_width) || (screen_texture->h != screen_output_height);
    if (!screen_output_rect_dirty && !output_size_changed) {
        return;
    }

    screen_output_width = screen_texture->w;
    screen_output_height = screen_texture->h;
    screen_output_rect = get_letterbox_rect(screen_output_width, screen_output_height);
    screen_output_has_letterbox = rect_has_letterbox(&screen_output_rect, screen_output_width, screen_output_height);
    screen_output_rect_dirty = false;
}

static void render_native_output_to_present_target(bool has_message_content, bool clear_bars) {
    SDL_SetRenderTarget(renderer, NULL);
    if (clear_bars) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); // black bars
        SDL_RenderClear(renderer);
    }

    if (has_message_content) {
        SDL_RenderTexture(renderer, message_canvas, NULL, &native_output_rect);
    }
}

static void update_metrics(Uint64 sleep_time) {
    const Uint64 new_frame_end_time = SDL_GetTicksNS();
    const Uint64 frame_time = new_frame_end_time - last_frame_end_time;
    const float frame_time_ms = (float)frame_time / 1e6;

    frame_metrics.frame_time[frame_metrics.head] = frame_time_ms;
    frame_metrics.idle_time[frame_metrics.head] = (float)sleep_time / 1e6;
    frame_metrics.fps[frame_metrics.head] = 1000 / frame_time_ms;

    frame_metrics.head = (frame_metrics.head + 1) % SDL_arraysize(frame_metrics.frame_time);
    last_frame_end_time = new_frame_end_time;
}

void SDLApp_EndFrame() {
#if defined(STATCHECK)
    /* A3b (docs/plan-fcade-replay-browser.md §4.3): uncapped headless
     * stepping for statcheck runs. `--headless` is parsed in every build but
     * consumed only here, only under STATCHECK — non-STATCHECK builds keep
     * the flag exactly as inert as before. Skips window presentation and the
     * frame-pacing wait below entirely (the fork-shaped equivalent of
     * upstream sdl_headless_app.c:59-72's render-free while(true) loop), so
     * frames step as fast as the engine can tick. Two calls are kept:
     * - ADX_ProcessTracks(): drains the sound decode queue. It is bounded by
     *   SDL_GetAudioStreamQueued (src/port/sound/adx.c:136), so uncapped
     *   stepping cannot balloon it; keeping it avoids starving any
     *   engine-visible track state.
     * - SoftwareRenderer_RenderFrame(): the giblet quad queue is an stb_ds
     *   array drained only by this call (software_renderer.c:1836
     *   arrsetlen(quads, 0)); skipping it would grow the queue without bound
     *   over a multi-thousand-frame replay. Rasterizing 384x224 is cheap
     *   relative to the engine tick. */
    if (configuration.headless) {
        ADX_ProcessTracks();
        SoftwareRenderer_RenderFrame();
        return;
    }
#endif

#if ENABLE_PERF_TELEMETRY
    const Uint64 render_start_ns = SDL_GetTicksNS();
    const Uint64 update_ns = render_start_ns > perf_update_start_ns ? (render_start_ns - perf_update_start_ns) : 0;
#endif
    /* Per-frame giblet timing locals for the FPS overlay (u/r/p/t). Set at
     * the SoftwareRenderer_RenderFrame() and present call-sites below; read
     * at the bottom of this function for the overlay accumulator. Function-
     * scoped so they reset to 0 every frame. Marked unused so the compile
     * with ENABLE_PERF_TELEMETRY=OFF (Miyoo) doesn't trip -Werror. */
    __attribute__((unused)) Uint64 gib_render_ns_this_frame = 0;
    __attribute__((unused)) Uint64 gib_present_ns_this_frame = 0;

    // Run sound processing
    ADX_ProcessTracks();

    // Render

    // This should come before SoftwareRenderer_RenderFrame,
    // because NetstatsRenderer uses the existing SFIII rendering pipeline.
#if defined(ENABLE_NETPLAY)
    NetplayScreen_Render();
    NetstatsRenderer_Render();
#endif
    const bool has_message_content = SDLMessageRenderer_HasContent();
    {
        const Uint64 gib_render_start_ns = SDL_GetTicksNS();
        SoftwareRenderer_RenderFrame();
        const Uint64 gib_render_end_ns = SDL_GetTicksNS();
        gib_render_ns_this_frame = (gib_render_end_ns > gib_render_start_ns)
                                       ? (gib_render_end_ns - gib_render_start_ns)
                                       : 0;
    }
    bool current_frame_surface_valid = false;
    bool current_frame_canvas_valid = true;

    const SDL_FRect* onscreen_content_rect = NULL;

    if (use_native_render_path) {
        refresh_native_output_rect();
        onscreen_content_rect = &native_output_rect;

        render_native_output_to_present_target(
            has_message_content, native_output_has_bars);
        current_frame_canvas_valid = true;
    } else {
        SDL_SetRenderTarget(renderer, screen_texture);
        refresh_screen_output_rect();

        const SDL_FRect* dst_rect = &screen_output_rect;
        onscreen_content_rect = dst_rect;
        const bool has_letterbox = screen_output_has_letterbox;

        if (has_letterbox) {
            // Render window background only when bars are actually visible.
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); // black bars
            SDL_RenderClear(renderer);
        }

        // Render content
        {
            int gw = 0, gh = 0, gpitch_bytes = 0;
            const SWCanvasPixel* gcanvas = SoftwareRenderer_GetCanvas(&gw, &gh, &gpitch_bytes);
            if (gcanvas && gw > 0 && gh > 0) {
                if (giblet_present_texture == NULL) {
                    giblet_present_texture = SDL_CreateTexture(
                        renderer,
#if defined(CRS_SW_CANVAS_16BPP)
                        SDL_PIXELFORMAT_RGB565,
#else
                        SDL_PIXELFORMAT_ARGB8888,
#endif
                        SDL_TEXTUREACCESS_STREAMING, gw, gh);
                    if (giblet_present_texture != NULL) {
                        SDL_SetTextureScaleMode(giblet_present_texture, SDL_SCALEMODE_NEAREST);
                    }
                }
                if (giblet_present_texture != NULL) {
                    /* Host-build "p" phase: the equivalent of
                     * NativeVideoWriter_WriteFrame on MiSTer is the canvas
                     * pixels → GPU texture upload. SDL_RenderTexture queues a
                     * draw command; the actual GPU submit happens in
                     * SDL_RenderPresent later, but that step is shared with
                     * other compositing work and not specific to the giblet
                     * canvas, so we measure only the upload here. */
                    const Uint64 gib_present_start_ns = SDL_GetTicksNS();
                    SDL_UpdateTexture(giblet_present_texture, NULL, gcanvas, gpitch_bytes);
                    SDL_RenderTexture(renderer, giblet_present_texture, NULL, dst_rect);
                    const Uint64 gib_present_end_ns = SDL_GetTicksNS();
                    gib_present_ns_this_frame = (gib_present_end_ns > gib_present_start_ns)
                                                    ? (gib_present_end_ns - gib_present_start_ns)
                                                    : 0;
                }
            }
        }
        if (has_message_content) {
            SDL_RenderTexture(renderer, message_canvas, NULL, dst_rect);
        }
        current_frame_canvas_valid = true;

        // Render screen texture to screen
        SDL_SetRenderTarget(renderer, NULL);
        SDL_RenderTexture(renderer, screen_texture, NULL, NULL);

        // Apply scanlines using a cached overlay texture.
        int win_w, win_h;
        SDL_GetRenderOutputSize(renderer, &win_w, &win_h);
        const SDL_FRect game_rect = get_letterbox_rect(win_w, win_h);
        ScanlineRenderer_Render(&game_rect);
    }

#if defined(DEBUG)
    // Render debug text
    SDLDebugText_Render();
    ImGuiW_EndFrame(renderer);
#endif
    render_renderer_fps_overlay(onscreen_content_rect);

#if ENABLE_PERF_TELEMETRY
    const Uint64 present_start_ns = SDL_GetTicksNS();
#endif

#if defined(PORT_MISTER)
    /* Giblet renderer present path on MiSTer: feed giblet's RGB565 canvas
     * directly to NativeVideoWriter. SoftwareRenderer_GetCanvas pitch is
     * tightly packed (pitch_bytes == w * 2) per software_renderer.h:34. */
    if (native_video_writer_enabled) {
        int gw = 0, gh = 0, gpitch_bytes = 0;
        const SWCanvasPixel* gcanvas = SoftwareRenderer_GetCanvas(&gw, &gh, &gpitch_bytes);
        if (gcanvas && gw == 384 && gh == 224) {
            if (fps_overlay_mode != FPS_OVERLAY_OFF) {
                /* Overlay writes into the canvas in place; SoftwareRenderer_GetCanvas
                 * returns const, but giblet's canvas is module-local and writable. */
                FPSOverlay_ApplyToRGB565Buffer((Uint16*)(uintptr_t)gcanvas, gw, gh);
            }
            const Uint64 gib_present_start_ns = SDL_GetTicksNS();
            NativeVideoWriter_WriteFrame((const uint16_t*)gcanvas, gw, gh, gpitch_bytes);
            const Uint64 gib_present_end_ns = SDL_GetTicksNS();
            gib_present_ns_this_frame = (gib_present_end_ns > gib_present_start_ns)
                                            ? (gib_present_end_ns - gib_present_start_ns)
                                            : 0;
        } else {
            static bool nv_giblet_warned = false;
            if (!nv_giblet_warned) {
                backend_logf("Native video (giblet): canvas unavailable or unexpected size (%dx%d)", gw, gh);
                nv_giblet_warned = true;
            }
        }
    }
#endif

    /* NOTE: RmlUi render + composite moved upstream of the native video
     * writer (see the ENABLE_RMLUI block above the native-video path). The
     * old post-writer rmlui_wrapper_render() call targeted the window
     * backbuffer, which is not on the MiSTer display pipeline — see
     * feedback-rmlui-render-target.md for the architectural rationale. */

#if defined(PORT_MIYOO_MINI_PLUS)
#if ENABLE_PERF_TELEMETRY
    Uint64 miyoo_overlay_ns = 0;
    Uint64 miyoo_armpresent_ns = 0;
#endif
    /* Giblet renderer present on Miyoo Mini Plus: feed giblet's RGB565
     * canvas directly to MI_GFX via ArmDisplay_Present. The SDL3
     * dummy video driver has no on-screen window; the SDL_RenderPresent
     * call below is a no-op for this profile. */
    {
        int gw = 0, gh = 0, gpitch_bytes = 0;
        const SWCanvasPixel* gcanvas = SoftwareRenderer_GetCanvas(&gw, &gh, &gpitch_bytes);
        if (gcanvas && gw > 0 && gh > 0) {
            if (fps_overlay_mode != FPS_OVERLAY_OFF) {
#if ENABLE_PERF_TELEMETRY
                const Uint64 ov_start = SDL_GetTicksNS();
#endif
                FPSOverlay_ApplyToRGB565Buffer((Uint16*)(uintptr_t)gcanvas, gw, gh);
#if ENABLE_PERF_TELEMETRY
                const Uint64 ov_end = SDL_GetTicksNS();
                miyoo_overlay_ns = ov_end > ov_start ? (ov_end - ov_start) : 0;
#endif
            }
#if ENABLE_PERF_TELEMETRY
            const Uint64 ap_start = SDL_GetTicksNS();
#endif
            ArmDisplay_Present((const uint32_t*)gcanvas, gw, gh);
#if ENABLE_PERF_TELEMETRY
            const Uint64 ap_end = SDL_GetTicksNS();
            miyoo_armpresent_ns = ap_end > ap_start ? (ap_end - ap_start) : 0;
#endif
        }
    }
#endif

#if defined(PORT_MIYOO_MINI_PLUS) && ENABLE_PERF_TELEMETRY
    const Uint64 sdl_present_start_ns = SDL_GetTicksNS();
#endif
#if !defined(PORT_MIYOO_MINI_PLUS)
    SDL_RenderPresent(renderer);
#else
    /* Skip SDL_RenderPresent on Miyoo: SDL3's dummy/evdev video driver
     * has no on-screen window — the canvas is already on the panel via
     * MI_GFX in ArmDisplay_Present above. SDL_RenderPresent costs ~27ms
     * per frame on this stack (measured 2026-05-05 on Miyoo Mini Plus
     * @1600MHz), apparently due to internal sync work in the software
     * renderer with no real backbuffer to flip. Skipping it drops the
     * frame budget from ~40ms to ~14ms in steady state. */
    (void)renderer;
#endif
#if defined(PORT_MIYOO_MINI_PLUS) && ENABLE_PERF_TELEMETRY
    const Uint64 sdl_present_end_ns = SDL_GetTicksNS();
    const Uint64 miyoo_sdlpresent_ns = sdl_present_end_ns > sdl_present_start_ns
                                           ? (sdl_present_end_ns - sdl_present_start_ns)
                                           : 0;
#endif
    (void)current_frame_surface_valid;
    (void)current_frame_canvas_valid;
#if ENABLE_PERF_TELEMETRY
    const Uint64 present_end_ns = SDL_GetTicksNS();
    const Uint64 render_ns = present_start_ns > render_start_ns ? (present_start_ns - render_start_ns) : 0;
    const Uint64 present_ns = present_end_ns > present_start_ns ? (present_end_ns - present_start_ns) : 0;
    const Uint64 frame_work_ns = present_end_ns > perf_frame_start_ns ? (present_end_ns - perf_frame_start_ns) : 0;
    const int dirty_tiles = 0;
    const double dirty_ratio = 0.0;

    /* Issue #16 freeze diagnostics — log frames that exceed 50 ms of work.
     * The frame ordinal is what makes an outlier attributable: flLogOut() and
     * backend_logf() both reach stderr, so a captured run log interleaves this
     * line with engine messages, and frame= says whether a neighbouring
     * message belongs to this frame or merely landed next to it. */
    if (frame_work_ns > 50000000ULL) {
        backend_logf("FRAME OUTLIER: frame=%llu total=%.1fms update=%.1f render=%.1f present=%.1f",
                     (unsigned long long)perf_frame_index,
                     (double)frame_work_ns / 1e6,
                     (double)update_ns / 1e6,
                     (double)render_ns / 1e6,
                     (double)present_ns / 1e6);
    }

    /* Steady-state perf summary: 120-frame moving averages of u/r/p/t plus
     * Miyoo per-call breakdown (overlay / ArmDisplay_Present / SDL_RenderPresent).
     * Cadence matches mi_gfx_perf so the two summaries can be cross-referenced. */
    {
        static int      perf_avg_frames        = 0;
        static Uint64   perf_avg_update_ns_sum = 0;
        static Uint64   perf_avg_render_ns_sum = 0;
        static Uint64   perf_avg_present_ns_sum = 0;
        static Uint64   perf_avg_total_ns_sum  = 0;
#if defined(PORT_MIYOO_MINI_PLUS)
        static Uint64   perf_avg_overlay_ns_sum    = 0;
        static Uint64   perf_avg_armpresent_ns_sum = 0;
        static Uint64   perf_avg_sdlpresent_ns_sum = 0;
#endif
        perf_avg_frames++;
        perf_avg_update_ns_sum  += update_ns;
        perf_avg_render_ns_sum  += render_ns;
        perf_avg_present_ns_sum += present_ns;
        perf_avg_total_ns_sum   += frame_work_ns;
#if defined(PORT_MIYOO_MINI_PLUS)
        perf_avg_overlay_ns_sum    += miyoo_overlay_ns;
        perf_avg_armpresent_ns_sum += miyoo_armpresent_ns;
        perf_avg_sdlpresent_ns_sum += miyoo_sdlpresent_ns;
#endif
        if (perf_avg_frames >= 120) {
            const double inv = 1.0 / (perf_avg_frames * 1e6);
#if defined(PORT_MIYOO_MINI_PLUS)
            backend_logf("[perf_avg] frames=%d  update=%.2f render=%.2f present=%.2f total=%.2f  ov=%.2f arm=%.2f sdlp=%.2f",
                         perf_avg_frames,
                         perf_avg_update_ns_sum * inv,
                         perf_avg_render_ns_sum * inv,
                         perf_avg_present_ns_sum * inv,
                         perf_avg_total_ns_sum * inv,
                         perf_avg_overlay_ns_sum * inv,
                         perf_avg_armpresent_ns_sum * inv,
                         perf_avg_sdlpresent_ns_sum * inv);
            perf_avg_overlay_ns_sum    = 0;
            perf_avg_armpresent_ns_sum = 0;
            perf_avg_sdlpresent_ns_sum = 0;
#else
            backend_logf("[perf_avg] frames=%d  update=%.2f render=%.2f present=%.2f total=%.2f",
                         perf_avg_frames,
                         perf_avg_update_ns_sum * inv,
                         perf_avg_render_ns_sum * inv,
                         perf_avg_present_ns_sum * inv,
                         perf_avg_total_ns_sum * inv);
#endif
            perf_avg_frames        = 0;
            perf_avg_update_ns_sum = 0;
            perf_avg_render_ns_sum = 0;
            perf_avg_present_ns_sum = 0;
            perf_avg_total_ns_sum  = 0;
        }
    }

    /* Feed per-frame timing into the FPS overlay rolling accumulator so the
       overlay can display averaged component-breakdown values. */
    if (fps_overlay_mode == FPS_OVERLAY_DEBUG) {
        const Uint64 texrefresh_ns = 0;
        const Uint64 gamelogic_ns = Game_GetPerfGameLogicNs();
        const Uint64 spritesubmit_ns = Game_GetPerfSpriteSubmitNs();
        const Uint64 dispatch_ns = Game_GetPerfDispatchNs();
        const Uint64 dtexrenew_ns = Mtrans_GetPerfTexRenewNs();
        const Uint64 dsprsubmit_ns = Mtrans_GetPerfSprSubmitNs();
        const Uint64 sort_ns = 0;
        const Uint64 raster_ns = 0;
        const Uint64 resolve_ns = 0;
        const Uint64 raster_pass_ns = 0;
        /* perf-3 hit/qsort counters are cumulative across all frames since
         * the renderer was last reset (only the perf-sampler resets them).
         * The overlay needs the per-frame delta, so track previous values
         * and subtract. A non-monotonic reset (e.g. perf-sampler firing)
         * shows up as a single-frame negative delta which we clamp to 0. */
        static Uint64 prev_packed_8px_hits = 0;
        static Uint64 prev_neon_16px_direct_hits = 0;
        static Uint64 prev_qsort_invocations = 0;
        const Uint64 cur_packed = 0;
        const Uint64 cur_neon = 0;
        const Uint64 cur_qsort = 0;
        const Uint64 packed_8px_hits = (cur_packed >= prev_packed_8px_hits) ? (cur_packed - prev_packed_8px_hits) : 0;
        const Uint64 neon_16px_direct_hits = (cur_neon >= prev_neon_16px_direct_hits) ? (cur_neon - prev_neon_16px_direct_hits) : 0;
        const Uint64 qsort_invocations = (cur_qsort >= prev_qsort_invocations) ? (cur_qsort - prev_qsort_invocations) : 0;
        prev_packed_8px_hits = cur_packed;
        prev_neon_16px_direct_hits = cur_neon;
        prev_qsort_invocations = cur_qsort;
        /* Sticky peak of EFFECT_MAX - frwctr (active effect-pool slot count).
         * Updated by seqsBeforeProcess every frame. Used to size the sparse
         * GameState ceiling for Option A.
         *
         * Bug guard: charsel_active_effect_count is computed as
         * EFFECT_MAX - frwctr, but frwctr is a static s16 that defaults to 0
         * pre-init. seqsBeforeProcess runs before effect_work_init in some
         * paths, so the formula yields a bogus 128 (= EFFECT_MAX) until init
         * runs and sets frwctr = EFFECT_MAX. Without gating, the peak gets
         * pinned at 128 forever from a frame zero or two of bogus reads.
         *
         * Gate: track whether we've seen frwctr == EFFECT_MAX (the post-init
         * idle state) at least once. Until that's observed, ignore samples.
         * After that, trust the reading (including a genuine 128 if the pool
         * is fully allocated). */
        {
            extern int charsel_active_effect_count;
            extern s16 frwctr;
            static bool effect_pool_init_seen = false;
            if (frwctr == 128 /* EFFECT_MAX */) {
                effect_pool_init_seen = true;
            }
            /* Range-validate before peak-tracking. Observed in the wild:
             * values up to ~1200 mid-rollback, which mathematically requires
             * frwctr to be a negative s16 (~-1072), produced either by a
             * pull_effect_work() overflow path or by a transient read during
             * rollback re-sim mid-state-restore. Either way, anything outside
             * [0, EFFECT_MAX] is not a real active-slot count, so don't let
             * it pin the peak. */
            const int sample = charsel_active_effect_count;
            if (effect_pool_init_seen
                && sample >= 0 && sample <= 128 /* EFFECT_MAX */
                && sample > fps_overlay_peak_active_effects) {
                fps_overlay_peak_active_effects = sample;
            }
        }
        fps_overlay_accumulate_timing(update_ns, texrefresh_ns,
                                      gamelogic_ns, spritesubmit_ns, dispatch_ns,
                                      dtexrenew_ns, dsprsubmit_ns,
                                      render_ns, sort_ns, raster_ns,
                                      resolve_ns, raster_pass_ns,
                                      packed_8px_hits, neon_16px_direct_hits,
                                      qsort_invocations,
                                      present_ns, frame_work_ns);

        /* perf-P3 diagnostic counters (perf-overlay report §4): frame_trace and
           overlay-submit ns timers, njdp2d prim peak/drops, giblet quad peak.
           Reported on the same 120-frame cadence as [perf_avg] so the on-device
           5-condition protocol needs no further code changes. */
        {
            static int    p3_frames = 0;
            static Uint64 p3_frametrace_ns_sum = 0;
            static Uint64 p3_trainingdisp_ns_sum = 0;
            static int    p3_njdp2d_peak = 0;
            static int    p3_njdp2d_drops_sum = 0;
            static int    p3_quads_peak = 0;
            p3_frames++;
            p3_frametrace_ns_sum   += FrameTrace_GetPerfTickNs();
            p3_trainingdisp_ns_sum += Training_GetPerfDispNs();
            {
                const int njp = Njdp2d_GetPerfPeakTotal();
                if (njp > p3_njdp2d_peak) p3_njdp2d_peak = njp;
            }
            p3_njdp2d_drops_sum += Njdp2d_GetPerfDrops();
            {
                const int qp = SoftwareRenderer_GetPerfPeakQuads();
                if (qp > p3_quads_peak) p3_quads_peak = qp;
            }
            if (p3_frames >= 120) {
                const double inv = 1.0 / (p3_frames * 1e6);
                backend_logf("[perf_p3] frames=%d  trace=%.3f trdisp=%.3f  njdp2d_peak=%d/512 drops=%d  quads_peak=%d/512",
                             p3_frames,
                             (double)p3_frametrace_ns_sum * inv,
                             (double)p3_trainingdisp_ns_sum * inv,
                             p3_njdp2d_peak, p3_njdp2d_drops_sum,
                             p3_quads_peak);
                p3_frames = 0;
                p3_frametrace_ns_sum = 0;
                p3_trainingdisp_ns_sum = 0;
                p3_njdp2d_peak = 0;
                p3_njdp2d_drops_sum = 0;
                p3_quads_peak = 0;
            }
        }
    }
    /* Giblet renderer u/r/p/t timing for the FPS overlay debug line.
     * Substitute the per-call-site giblet measurements for the frame's
     * render/present/frame slots. */
    if (fps_overlay_mode == FPS_OVERLAY_DEBUG) {
        fps_overlay_render_ns_accum  -= render_ns;
        fps_overlay_present_ns_accum -= present_ns;
        fps_overlay_frame_ns_accum   -= frame_work_ns;
        fps_overlay_render_ns_accum  += gib_render_ns_this_frame;
        fps_overlay_present_ns_accum += gib_present_ns_this_frame;
        /* t = u + r + p (per the giblet overlay spec). */
        const Uint64 gib_total_ns = update_ns
                                  + gib_render_ns_this_frame
                                  + gib_present_ns_this_frame;
        fps_overlay_frame_ns_accum   += gib_total_ns;
    }
#endif

    // Handle cursor hiding
    hide_cursor_if_needed();

    // Do frame pacing
    //
    // Software PLL: the FPGA PLL produces 59.5995 Hz, matching TARGET_FPS to
    // within 1.4 uHz.  No frequency tracking needed.  We use SCHED_FIFO +
    // hybrid sleep/busy-wait for sub-ms precision.
    //
    // When vsync feedback is available from the wrapper (closed-loop), the
    // pacer aligns frame delivery to arrive lead_time_ns before the next
    // FPGA vsync.  When feedback is unavailable (open-loop fallback), the
    // pacer runs on its own deadline with no phase correction.

#if defined(PORT_MISTER)
    poll_vsync_feedback();

    /* Staleness check: if feedback hasn't been updated in 100ms, disengage */
    if (vsync_feedback_valid) {
        Uint64 stale_check_ns = SDL_GetTicksNS();
        if (last_feedback_update_ns > 0 && (stale_check_ns - last_feedback_update_ns) > 100000000ULL) {
            vsync_feedback_valid = false;
            backend_logf("Frame pacer: vsync feedback stale, falling back to open-loop");
        }
    }
#endif

    Uint64 now = SDL_GetTicksNS();
    Uint64 sleep_time = 0;

    if (native_video_writer_enabled) {
        if (frame_deadline == 0) {
            frame_deadline = now + target_frame_time_ns;
        }

        /* --- Closed-loop phase correction (when feedback is available) --- */
        if (vsync_feedback_valid) {
            /* The wrapper observed a vsync at last_vsync_monotonic_ns.
               Compute where the next vsync is, accounting for possibly multiple
               vsyncs having passed since last_vsync_monotonic_ns. */
            if (now < last_vsync_monotonic_ns) {
                goto skip_closed_loop;  /* clock ordering anomaly — run open-loop this frame */
            }
            Uint64 elapsed_since_vsync = now - last_vsync_monotonic_ns;
            Uint64 frames_since = elapsed_since_vsync / target_frame_time_ns;
            Uint64 next_vsync = last_vsync_monotonic_ns + (frames_since + 1) * target_frame_time_ns;

            /* Our ideal deadline: lead_time_ns before the next vsync. */
            Uint64 ideal_deadline = next_vsync - lead_time_ns;

            /* If ideal_deadline is in the past (we're late), target the one after. */
            if (ideal_deadline <= now) {
                ideal_deadline += target_frame_time_ns;
            }

            /* Blend toward ideal: don't jump instantly (causes visible stutter).
               Move frame_deadline 25% toward ideal each frame. */
            int64_t error = (int64_t)(ideal_deadline - frame_deadline);
            pacer_phase_error_ns = error;  /* for overlay */
            if (error > (int64_t)target_frame_time_ns || error < -(int64_t)target_frame_time_ns) {
                frame_deadline = ideal_deadline;    /* too far off, snap */
            } else {
                frame_deadline += error / 4;        /* smooth convergence (~4 frames) */
            }
        } else {
            skip_closed_loop:
            pacer_phase_error_ns = 0;
        }

        /* Sleep until deadline */
        if (now < frame_deadline) {
            sleep_time = frame_deadline - now;
#if defined(PORT_MISTER)
            // native_video_writer_enabled is only true on PORT_MISTER builds,
            // and precise_delay_ns is only compiled there.
            precise_delay_ns(frame_deadline);
#else
            SDL_DelayNS(frame_deadline - now);
#endif
            now = SDL_GetTicksNS();
        }

        /* Measure jitter against the post-blend deadline (the actual sleep
           target) so that phase corrections — both blend-earlier and
           blend-later — don't register as dropped frames.  Only real
           oversleep (game logic overrun or OS scheduling delay) counts. */
        Uint64 jitter_ns = (now > frame_deadline) ? (now - frame_deadline) : 0;

        // Jitter stats for FPS overlay
        pacer_stats_frames++;
        if (jitter_ns > pacer_max_jitter_ns)
            pacer_max_jitter_ns = jitter_ns;
        pacer_jitter_sum_ns += jitter_ns;
        if (jitter_ns > 500000)
            pacer_late_count++;

        // Advance deadline
        frame_deadline += target_frame_time_ns;

        // Guard: if >1 frame behind, reset
        if (now > frame_deadline + target_frame_time_ns)
            frame_deadline = now + target_frame_time_ns;
    } else {
        // --- Open-loop pacing for non-native-video paths ---
        if (frame_deadline == 0)
            frame_deadline = now + target_frame_time_ns;
        if (now < frame_deadline) {
            sleep_time = frame_deadline - now;
            SDL_DelayNS(sleep_time);
            now = SDL_GetTicksNS();
        }
        frame_deadline += target_frame_time_ns;
        if (now > frame_deadline + target_frame_time_ns)
            frame_deadline = now + target_frame_time_ns;
    }

    // Measure
    update_fps_overlay(now);

#if ENABLE_PERF_TELEMETRY
    if (perf_capture_enabled && !perf_capture_completed && perf_capture_recorded_frames < perf_capture_target_frames) {
        PerfFrameSample* sample = &perf_samples[perf_capture_recorded_frames];
        note_perf_capture_test_state(perf_capture_recorded_frames);
        sample->frame_time_ms = (double)frame_work_ns / 1e6;
        sample->update_ms = (double)update_ns / 1e6;
        sample->render_ms = (double)render_ns / 1e6;
        sample->present_ms = (double)present_ns / 1e6;
        sample->dirty_tiles = dirty_tiles;
        sample->dirty_ratio = dirty_ratio;

        perf_update_ns_total += update_ns;
        perf_render_ns_total += render_ns;
        perf_present_ns_total += present_ns;
        perf_frame_work_ns_total += frame_work_ns;
        perf_dirty_tiles_total += (Uint64)dirty_tiles;
        perf_capture_recorded_frames += 1;

        if (perf_capture_recorded_frames >= perf_capture_target_frames) {
            perf_capture_write_summary();
            perf_capture_completed = true;
            SDLApp_Exit();
        }
    }
#endif

    update_metrics(sleep_time);
}

void SDLApp_Exit() {
    SDL_Event quit_event;
    quit_event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit_event);
}

const FrameMetrics* SDLApp_GetFrameMetrics() {
    return &frame_metrics;
}
