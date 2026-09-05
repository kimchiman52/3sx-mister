#include "args.h"
#include "main.h"
#include "test/test_runner.h"

#include "argparse/argparse.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_CODE_RUNTIME_ERROR 1

static void error_out_with_code(const char* error, int code) {
    fprintf(stderr, "%s Exiting.\n", error);
    exit(code);
}

#if ENABLE_NETPLAY
static void error_out(const char* error) {
    error_out_with_code(error, EXIT_CODE_RUNTIME_ERROR);
}
#endif

static bool is_supported_test_stage(int stage) {
    return stage >= 0 && stage <= 19 && stage != 17;
}

/* Task #108. The three legal answers to "which balance table does this
 * harness run exercise?". NULL means "unstated", which is legal for every
 * harness EXCEPT the frame-data corpus suite (see below). */
static bool is_supported_test_balance(const char* balance) {
    return balance == NULL || SDL_strcmp(balance, "ps2") == 0 || SDL_strcmp(balance, "arcade") == 0;
}

static bool is_supported_test_scene_preset(const char* preset) {
    return preset == NULL || SDL_strcmp(preset, "stage-heavy") == 0 || SDL_strcmp(preset, "effect-heavy") == 0 ||
           SDL_strcmp(preset, "super-heavy") == 0 || SDL_strcmp(preset, "yun-sa3-repeat") == 0 ||
           SDL_strcmp(preset, "yun-sa3-repeat-pressure") == 0 || SDL_strcmp(preset, "q-sa1-repeat") == 0 ||
           SDL_strcmp(preset, "q-sa1-repeat-pressure") == 0 || SDL_strcmp(preset, "ken-sa3-repeat") == 0 ||
           SDL_strcmp(preset, "ken-sa3-repeat-pressure") == 0 ||
           SDL_strcmp(preset, "chunli-sa2-repeat") == 0 ||
           SDL_strcmp(preset, "chunli-sa2-repeat-pressure") == 0 ||
           SDL_strcmp(preset, "basic-exchange") == 0 || SDL_strcmp(preset, "pressure-exchange") == 0 ||
           SDL_strcmp(preset, "left-corner-ryu-stage") == 0 || SDL_strcmp(preset, "training-yun-ryu-ryu-stage") == 0 ||
           SDL_strcmp(preset, "training-frame-data") == 0;
}

#if ENABLE_PERF_TELEMETRY
static bool is_supported_perf_wait_test_phase(const char* phase_name) {
    return phase_name == NULL ||
           (TestRunner_IsSupportedPhaseName(phase_name) && SDL_strcmp(phase_name, "init") != 0);
}

static bool is_supported_perf_wait_runtime_state(const char* state_name) {
    return state_name == NULL || SDL_strcmp(state_name, "attract-demo-logo") == 0 ||
           SDL_strcmp(state_name, "character-select-super-art") == 0;
}
#endif

static void verify_configuration(Configuration* configuration) {
    const TestRunnerConfiguration* test = &configuration->test;

#if ENABLE_PERF_TELEMETRY
    if (configuration->perf.frame_count < 0) {
        error_out_with_code("--perf-capture must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }

    if (configuration->perf.basic_first_window_family_snapshots && !configuration->perf.basic_mode) {
        error_out_with_code("--perf-basic-first-window-families requires --perf-basic.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (configuration->perf.basic_first_window_render_subphases &&
        (!configuration->perf.basic_mode || !configuration->perf.basic_first_window_family_snapshots)) {
        error_out_with_code("--perf-basic-first-window-render-subphases requires --perf-basic-first-window-families.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (configuration->perf.basic_first_window_exact_hot_family_alpha_offpath &&
        (!configuration->perf.basic_mode || !configuration->perf.basic_first_window_family_snapshots)) {
        error_out_with_code("--perf-basic-first-window-exact-hot-family-alpha-offpath requires --perf-basic-first-window-families.",
                            EXIT_CODE_RUNTIME_ERROR);
    }
    if (configuration->perf.basic_first_window_onset_exact_hot_family_alpha_offpath &&
        (!configuration->perf.basic_mode || !configuration->perf.basic_first_window_family_snapshots)) {
        error_out_with_code("--perf-basic-first-window-onset-exact-hot-family-alpha-offpath requires --perf-basic-first-window-families.",
                            EXIT_CODE_RUNTIME_ERROR);
    }
    if (configuration->perf.basic_first_window_onset_cluster_alpha_offpath &&
        (!configuration->perf.basic_mode || !configuration->perf.basic_first_window_family_snapshots)) {
        error_out_with_code("--perf-basic-first-window-onset-cluster-alpha-offpath requires --perf-basic-first-window-families.",
                            EXIT_CODE_RUNTIME_ERROR);
    }
    if ((configuration->perf.basic_first_window_exact_hot_family_alpha_offpath ? 1 : 0) +
            (configuration->perf.basic_first_window_onset_exact_hot_family_alpha_offpath ? 1 : 0) +
            (configuration->perf.basic_first_window_onset_cluster_alpha_offpath ? 1 : 0) >
        1) {
        error_out_with_code("--perf-basic-first-window-exact-hot-family-alpha-offpath, --perf-basic-first-window-onset-exact-hot-family-alpha-offpath, and --perf-basic-first-window-onset-cluster-alpha-offpath cannot be used together.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (!is_supported_perf_wait_test_phase(configuration->perf.wait_for_test_phase)) {
        error_out_with_code("--perf-wait-test-phase must be one of title, menu, "
                            "character-select-transition, character-select, game-transition, game, "
                            "game-input-active, p1-super-art-active, p1-super-art-active-2, or "
                            "wipe-transition-type1.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (!is_supported_perf_wait_runtime_state(configuration->perf.wait_for_runtime_state)) {
        error_out_with_code("--perf-wait-runtime-state must be attract-demo-logo or character-select-super-art.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (configuration->perf.gameplay_warmup_frames < 0) {
        error_out_with_code("--perf-warmup must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }
#endif

    if (test->characters[0] != -1 && (test->characters[0] < 0 || test->characters[0] > 19)) {
        error_out_with_code("--test-p1-character must be between 0 and 19.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->characters[1] != -1 && (test->characters[1] < 0 || test->characters[1] > 19)) {
        error_out_with_code("--test-p2-character must be between 0 and 19.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->super_arts[0] != -1 && (test->super_arts[0] < 0 || test->super_arts[0] > 2)) {
        error_out_with_code("--test-p1-super-art must be between 0 and 2.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->super_arts[1] != -1 && (test->super_arts[1] < 0 || test->super_arts[1] > 2)) {
        error_out_with_code("--test-p2-super-art must be between 0 and 2.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->training_sa_gauge != -1 && (test->training_sa_gauge < 0 || test->training_sa_gauge > 3)) {
        error_out_with_code("--test-training-sa-gauge must be between 0 and 3.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->stage != -1 && !is_supported_test_stage(test->stage)) {
        error_out_with_code("--test-stage must be between 0 and 19 (excluding 17).", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->rbd_capture_path != NULL) {
#ifndef DEBUG
        error_out_with_code("--rbd-capture requires a Debug build (#if DEBUG).", EXIT_CODE_RUNTIME_ERROR);
#endif
#if !defined(ENABLE_NETPLAY)
        error_out_with_code("--rbd-capture requires a build with ENABLE_NETPLAY=ON.", EXIT_CODE_RUNTIME_ERROR);
#endif
        if (test->rbd_symmap_path == NULL || test->rbd_symmap_path[0] == '\0') {
            error_out_with_code("--rbd-capture requires --rbd-symmap.", EXIT_CODE_RUNTIME_ERROR);
        }
        if (test->rbd_frames <= 0) {
            error_out_with_code("--rbd-capture requires --rbd-frames > 0.", EXIT_CODE_RUNTIME_ERROR);
        }
    }
    if (test->rbd_rollback_period < 0) {
        error_out_with_code("--rbd-rollback-period must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->rbd_rollback_depth < 1) {
        error_out_with_code("--rbd-rollback-depth must be >= 1.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->rbd_select_rollback_period < 0) {
        error_out_with_code("--rbd-select-rollback-period must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->rbd_select_rollback_depth < 1) {
        error_out_with_code("--rbd-select-rollback-depth must be >= 1.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->ldreq_trace_path != NULL) {
#ifndef DEBUG
        error_out_with_code("--ldreq-trace requires a Debug build (#if DEBUG).", EXIT_CODE_RUNTIME_ERROR);
#endif
        if (test->ldreq_trace_frames <= 0) {
            error_out_with_code("--ldreq-trace requires --ldreq-trace-frames > 0.", EXIT_CODE_RUNTIME_ERROR);
        }
    }
    if (test->ldreq_slot_trace_path != NULL && test->ldreq_trace_path == NULL) {
        error_out_with_code("--ldreq-slot-trace requires --ldreq-trace (it shares its frame budget).",
                            EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->afs_inject_latency_ms < 0) {
        error_out_with_code("--afs-inject-latency-ms must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }

    if (test->fcade_inputs_path != NULL && !test->enabled) {
        error_out_with_code("--test-fcade-inputs requires --test-enable.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->fcade_offset < 0) {
        error_out_with_code("--test-fcade-offset must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->fcade_anchor < 0) {
        error_out_with_code("--test-fcade-anchor must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->fcade_max_frames < 0) {
        error_out_with_code("--test-fcade-max-frames must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (!is_supported_test_scene_preset(test->scene_preset)) {
        error_out_with_code("--test-scene-preset must be one of stage-heavy, effect-heavy, super-heavy, "
                            "yun-sa3-repeat, yun-sa3-repeat-pressure, q-sa1-repeat, q-sa1-repeat-pressure, "
                            "ken-sa3-repeat, ken-sa3-repeat-pressure, chunli-sa2-repeat, chunli-sa2-repeat-pressure, "
                            "basic-exchange, pressure-exchange, left-corner-ryu-stage, training-yun-ryu-ryu-stage, "
                            "or training-frame-data.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (test->select_dwell_frames < 0) {
        error_out_with_code("--test-select-dwell-frames must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }

    if (test->instant_jump) {
#ifndef DEBUG
        error_out_with_code("--test-instant-jump requires a Debug build (#if DEBUG).", EXIT_CODE_RUNTIME_ERROR);
#endif
        if (!test->enabled) {
            error_out_with_code("--test-instant-jump requires --test-enable.", EXIT_CODE_RUNTIME_ERROR);
        }
        if (test->scene_preset != NULL) {
            error_out_with_code("--test-instant-jump bypasses the scene-preset phase machine; drop "
                                "--test-scene-preset and use --test-p1-character/--test-stage instead.",
                                EXIT_CODE_RUNTIME_ERROR);
        }
    }

    if (test->quick_training_frame >= 0 || test->quick_training_again_frame >= 0) {
#ifndef DEBUG
        error_out_with_code("--test-quick-training requires a Debug build (#if DEBUG).", EXIT_CODE_RUNTIME_ERROR);
#endif
        if (!test->enabled) {
            error_out_with_code("--test-quick-training requires --test-enable.", EXIT_CODE_RUNTIME_ERROR);
        }
        if (test->quick_training_frame < 0) {
            error_out_with_code("--test-quick-training-again requires --test-quick-training.",
                                EXIT_CODE_RUNTIME_ERROR);
        }
        if (test->quick_training_again_frame >= 0 &&
            test->quick_training_again_frame <= test->quick_training_frame) {
            error_out_with_code("--test-quick-training-again must be a later frame than --test-quick-training.",
                                EXIT_CODE_RUNTIME_ERROR);
        }
        if (test->instant_jump) {
            error_out_with_code("--test-quick-training and --test-instant-jump both own the session; pick one.",
                                EXIT_CODE_RUNTIME_ERROR);
        }
        if (test->scene_preset != NULL) {
            error_out_with_code("--test-quick-training bypasses the scene-preset phase machine; drop "
                                "--test-scene-preset.",
                                EXIT_CODE_RUNTIME_ERROR);
        }
    }

    /* Task #108: balance is CHOSEN, never inherited from --test-enable. */
    if (!is_supported_test_balance(test->balance)) {
        error_out_with_code("--test-balance must be 'ps2' or 'arcade'.", EXIT_CODE_RUNTIME_ERROR);
    }

    if (test->balance != NULL && !test->enabled) {
        error_out_with_code("--test-balance requires --test-enable (it selects the balance table a "
                            "harness run exercises).",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    /* The frame-data corpus suite must name its engine. Before task #108 the
     * suite ran with the balance decided for it, as a side effect of
     * --test-enable, and every one of its corpora silently exercised PS2
     * while the shipping device path auto-selects arcade. Requiring the flag
     * here is what keeps that from being re-introduced by omission:
     * tools/frame-data/run.sh always passes it, from the corpus's own
     * `balance:` key. */
    if (test->scene_preset != NULL && SDL_strcmp(test->scene_preset, "training-frame-data") == 0 &&
        test->balance == NULL) {
        error_out_with_code("--test-scene-preset training-frame-data requires an explicit "
                            "--test-balance ps2|arcade (task #108).",
                            EXIT_CODE_RUNTIME_ERROR);
    }

#if defined(STATCHECK)
    /* A STATCHECK build exists to run the harness, so it always needs an
     * archive. (This used to carry an exemption for --fetch-replay, which
     * dispatched before StatcheckRunner_Init and exited without ever running
     * the harness; that flag is gone, and with it the only way to launch a
     * STATCHECK build that legitimately had no archive.) */
    if (configuration->statcheck.ram_archive_path == NULL) {
        error_out_with_code("You must specify --ram-archive.", EXIT_CODE_RUNTIME_ERROR);
    }

    /* C1: a STATCHECK build unconditionally drives its own SCRD replay
     * through the same p1sw_buff latch — the two injectors cannot share a
     * session. --play-replay works in every other flavor. */
    if (configuration->replay.play_replay_path != NULL) {
        error_out_with_code("--play-replay cannot be used in a THREESX_STATCHECK build (the statcheck "
                            "harness owns input injection).",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    /* Same reason for the shuffle viewer: it drives the same C1 player, whose
     * injection would collide with StatcheckRunner_Prologue's. */
    if (configuration->replay.watch_replays) {
        error_out_with_code("--watch-replays cannot be used in a THREESX_STATCHECK build (the statcheck "
                            "harness owns input injection).",
                            EXIT_CODE_RUNTIME_ERROR);
    }
#endif

    /* The meta handoff flags describe the replay being played, so they ride
     * ONLY with --play-replay. (Their "--live-replay-*" spelling is
     * historical, from the retired live-stream path.) */
    if (configuration->replay.play_replay_path == NULL &&
        (configuration->replay.live_p1_name != NULL || configuration->replay.live_p2_name != NULL ||
         configuration->replay.live_p1_rank != 0 || configuration->replay.live_p2_rank != 0 ||
         configuration->replay.live_date_ms != NULL)) {
        error_out_with_code("--live-replay-p1/p2/rank/date require --play-replay.", EXIT_CODE_RUNTIME_ERROR);
    }

    /* C1 (src/replay/replay_player.c): the replay player owns
     * p1sw_buff/p2sw_buff for the whole session — the DEBUG test runner
     * writes the same latch from the same game_step_0 slot. */
    if (configuration->replay.play_replay_path != NULL && configuration->test.enabled) {
        error_out_with_code("--play-replay cannot be combined with --test-enable.", EXIT_CODE_RUNTIME_ERROR);
    }

    /* The shuffle viewer and a single --play-replay boot are two different
     * owners of the same C1 player. Only ReplayPlayer_LoadAndStart (which the
     * viewer uses) marks a launch as owned, and only an owned launch FREEZES
     * at a terminal state instead of calling SDLApp_Exit(); a --play-replay
     * boot would therefore end the process after the first match and take the
     * playlist with it. Reject the combination outright. */
    if (configuration->replay.watch_replays && configuration->replay.play_replay_path != NULL) {
        error_out_with_code("--watch-replays cannot be combined with --play-replay.", EXIT_CODE_RUNTIME_ERROR);
    }

    /* The root override only means something to the viewer. */
    if (configuration->replay.watch_replays_root != NULL && !configuration->replay.watch_replays) {
        error_out_with_code("--watch-replays-root requires --watch-replays.", EXIT_CODE_RUNTIME_ERROR);
    }

    /* Same latch contention as --play-replay above. */
    if (configuration->replay.watch_replays && configuration->test.enabled) {
        error_out_with_code("--watch-replays cannot be combined with --test-enable.", EXIT_CODE_RUNTIME_ERROR);
    }

#if ENABLE_NETPLAY
    {
        const NetplayConfiguration* netplay = &configuration->netplay;
        const bool p2p_specified = netplay->p2p_local_player > 0 || netplay->p2p_remote_ip != NULL;
        const bool handoff_specified = netplay->direct_p2p_handoff_set;

        if (handoff_specified && p2p_specified) {
            error_out("--direct-p2p-handoff cannot be combined with --p2p-* flags.");
        }

        /* C1: replay playback and a netplay session would fight over the
         * p1sw_buff latch; ReplayPlayer_Tick additionally aborts at runtime
         * if a session appears later (e.g. default-path handoff probe). */
        if (configuration->replay.play_replay_path != NULL && (p2p_specified || handoff_specified)) {
            error_out("--play-replay cannot be combined with netplay flags.");
        }

        /* Same collision for the shuffle viewer: ReplayPlayer_LoadAndStart
         * refuses while a session is live and ReplayPlayer_Tick aborts a
         * running replay if one appears, so an armed netplay session would
         * leave the viewer with nothing to do. */
        if (configuration->replay.watch_replays && (p2p_specified || handoff_specified)) {
            error_out("--watch-replays cannot be combined with netplay flags.");
        }

        if (p2p_specified) {
            if (netplay->p2p_local_player != 1 && netplay->p2p_local_player != 2) {
                error_out("Local player must be 1 or 2.");
            }

            if (netplay->p2p_remote_ip == NULL) {
                error_out("You must specify --p2p-remote-ip.");
            }
        }
    }
#endif
}

void read_args(int argc, const char* argv[], Configuration* configuration) {
#if ENABLE_NETPLAY
    /* Argparse writes a string option through `const char**`; the fixed-
     * size `direct_p2p_handoff_path[256]` lives in NetplayConfiguration,
     * so we bounce the arg through a pointer and copy below. */
    const char* direct_p2p_handoff_arg = NULL;
#endif

    struct argparse_option options[] = {
        OPT_HELP(),

#if ENABLE_NETPLAY
        OPT_GROUP("Netplay"),
        OPT_INTEGER(0,
                    "p2p-local-player",
                    &configuration->netplay.p2p_local_player,
                    "Number of the local player (1 or 2).",
                    NULL,
                    0,
                    0),
        OPT_STRING(0, "p2p-remote-ip", &configuration->netplay.p2p_remote_ip, "Remote player IP.", NULL, 0, 0),
        OPT_STRING(0,
                   "direct-p2p-handoff",
                   &direct_p2p_handoff_arg,
                   "Read a one-shot direct-P2P handoff file (mode=host|join, port, peer_code) and dispatch DirectP2P_BeginHost/BeginJoin after init. The file is unlinked after a successful read.",
                   NULL,
                   0,
                   0),
#endif

        OPT_GROUP("Diagnostics"),
        OPT_BOOLEAN(0,
                    "probe-renderer-only",
                    &configuration->probe_renderer_only,
                    "Probe SDL video/render backends and exit.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0, "headless", &configuration->headless, "Run with non-interactive event handling.", NULL, 0, 0),
        OPT_BOOLEAN(0,
                    "test-mist-handshake",
                    &configuration->test_mist_handshake,
                    "Run the Layer 3 MIST arch-handshake test harness and exit. Requires ENABLE_NETPLAY=ON with -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-room-code",
                    &configuration->test_room_code,
                    "Run the STUN room-code codec test harness and exit. Requires ENABLE_NETPLAY=ON with -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-late-punch",
                    &configuration->test_late_punch,
                    "Run the late-punch rescue-layer unit harness and exit. Requires ENABLE_NETPLAY=ON with -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-stun-mock",
                    &configuration->test_stun_mock,
                    "Run the STUN mock-server test harness and exit. Requires ENABLE_NETPLAY=ON with -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-sparse-effect-save",
                    &configuration->test_sparse_effect_save,
                    "Run the sparse effect-pool save round-trip parity tests and exit. Requires ENABLE_NETPLAY=ON with -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-netplay-units",
                    &configuration->test_netplay_units,
                    "Run the fast netplay unit tests and exit. Requires ENABLE_NETPLAY=ON with -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-bilateral-punch",
                    &configuration->test_bilateral_punch,
                    "Run the bilateral hole-punch protocol unit tests and exit. Requires ENABLE_NETPLAY=ON with -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-connect-observability",
                    &configuration->test_connect_observability,
                    "Run the netplay connect-observability proof harness and exit. Requires ENABLE_NETPLAY=ON with -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-gs-coverage",
                    &configuration->test_gs_coverage,
                    "Run the GameState save/load field-coverage guard and exit. Requires ENABLE_NETPLAY=ON with -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-rendezvous-wire",
                    &configuration->test_rendezvous_wire,
                    "Run the rendezvous wire-codec + NACK verdict unit tests and exit. Requires ENABLE_NETPLAY=ON with -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-mist-compat-gate",
                    &configuration->test_mist_compat_gate,
                    "Run the MIST compat-gate parse/classify unit tests and exit. Requires ENABLE_NETPLAY=ON with -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-punch-predicates",
                    &configuration->test_punch_predicates,
                    "Run the STUN/punch predicate and late-punch decision unit tests and exit. Requires ENABLE_NETPLAY=ON with -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-texcash-bounds",
                    &configuration->test_texcash_bounds,
                    "Run the ext texture-cache brick-prevention harness (tasks #59/#61) and exit. Requires -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-cg-se-remap",
                    &configuration->test_cg_se_remap,
                    "Run the arcade cg_se sound-code remap unit harness (doc item Q) and exit. Requires -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-cps3-chardma",
                    &configuration->test_cps3_chardma,
                    "Run the ported CPS-3 char-DMA decoder unit harness (\"first light\" scaffolding) and exit. Requires -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-ui-text-units",
                    &configuration->test_ui_text_units,
                    "Run the proportional-font word-wrap unit harness (sc_sub.c -> SSWrapStrPro) and exit. Requires -DENABLE_NETPLAY_TESTS.",
                    NULL,
                    0,
                    0),
#if ENABLE_PERF_TELEMETRY
        OPT_GROUP("Performance"),
        OPT_INTEGER(0,
                    "perf-capture",
                    &configuration->perf.frame_count,
                    "Capture perf metrics for N frames, then exit.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic",
                    &configuration->perf.basic_mode,
                    "Capture low-overhead frame/update/render/present timings; lightweight test-state metadata may still be exported.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic-first-window-families",
                    &configuration->perf.basic_first_window_family_snapshots,
                    "When used with --perf-basic, also export first-window fast/generic family summaries without full shape/lookup telemetry.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic-first-window-render-subphases",
                    &configuration->perf.basic_first_window_render_subphases,
                    "When used with --perf-basic-first-window-families, also export first-window raster-bucket timing plus fast non-integer phase totals.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic-first-window-exact-hot-family-alpha-offpath",
                    &configuration->perf.basic_first_window_exact_hot_family_alpha_offpath,
                    "When used with --perf-basic-first-window-families, analyze the proven 57/58 + 391-394 hot-family alpha structure off the hot raster path after the first window snapshot.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic-first-window-onset-exact-hot-family-alpha-offpath",
                    &configuration->perf.basic_first_window_onset_exact_hot_family_alpha_offpath,
                    "When used with --perf-basic-first-window-families, analyze the exact proven first-visible Yun onset hot-family alpha structure off the hot raster path after the first window snapshot.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic-first-window-onset-cluster-alpha-offpath",
                    &configuration->perf.basic_first_window_onset_cluster_alpha_offpath,
                    "When used with --perf-basic-first-window-families, analyze the proven Loop 172 first-visible Yun onset cluster alpha structure off the hot raster path after the first window snapshot.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-fast-non-integer-no-reuse-telemetry",
                    &configuration->perf.fast_non_integer_disable_reuse_telemetry,
                    "Keep full perf capture enabled but skip fast non-integer row-reuse bookkeeping to reduce capture distortion.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-fast-non-integer-subrect-alpha-telemetry",
                    &configuration->perf.fast_non_integer_enable_subrect_alpha_telemetry,
                    "Keep full perf capture enabled and classify fast non-integer sampled subrect alpha rows/spans in-band during raster.",
                    NULL,
                    0,
                    0),
        OPT_STRING(0,
                   "perf-output",
                   &configuration->perf.output_path,
                   "Path to perf capture JSON output.",
                   NULL,
                   0,
                   0),
        OPT_STRING(
            0, "scene", &configuration->perf.scene, "Optional perf scene label for capture metadata.", NULL, 0, 0),
        OPT_BOOLEAN(0,
                    "perf-wait-in-game",
                    &configuration->perf.wait_for_gameplay,
                    "Delay perf capture until gameplay is active.",
                    NULL,
                    0,
                    0),
        OPT_STRING(0,
                   "perf-wait-test-phase",
                   &configuration->perf.wait_for_test_phase,
                   "Delay perf capture until the test runner reaches the named phase.",
                   NULL,
                   0,
                   0),
        OPT_STRING(0,
                   "perf-wait-runtime-state",
                   &configuration->perf.wait_for_runtime_state,
                   "Delay perf capture until the runtime reaches the named state.",
                   NULL,
                   0,
                   0),
        OPT_INTEGER(0,
                    "perf-warmup",
                    &configuration->perf.gameplay_warmup_frames,
                    "Warmup frames to skip after the selected perf wait condition becomes active before starting perf capture.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "software-frame-parity-check",
                    &configuration->perf.software_frame_parity_check,
                    "Run the offline software-frame parity self-check and exit.",
                    NULL,
                    0,
                    0),
#endif
        OPT_GROUP("Test runner"),
        OPT_BOOLEAN(0, "test-enable", &configuration->test.enabled, "Enable test runner.", NULL, 0, 0),
        OPT_INTEGER(0,
                    "test-select-dwell-frames",
                    &configuration->test.select_dwell_frames,
                    "Idle this many extra frames on the character-select screen before the test runner "
                    "starts driving cursors. Lets a run actually inhabit select long enough for the "
                    "select-timer countdown to advance (task #108). 0 (default) = historical behaviour. "
                    "Requires a #if DEBUG build (or -DENABLE_DEBUG_HOOKS=ON): the whole test runner is "
                    "compiled out otherwise.",
                    NULL,
                    0,
                    0),
        OPT_STRING(0,
                   "test-balance",
                   &configuration->test.balance,
                   "Balance table this harness run exercises: 'ps2' (the historical test-runner pin) "
                   "or 'arcade' (the shipping CPS3 tables; requires a verified romset -- the run exits "
                   "6 rather than silently falling back to PS2). Required with --test-scene-preset "
                   "training-frame-data. Requires --test-enable.",
                   NULL,
                   0,
                   0),
        OPT_STRING(0,
                   "test-states",
                   &configuration->test.states_path,
                   "Optional path to captured test states for scripted in-game inputs.",
                   NULL,
                   0,
                   0),
        OPT_STRING(0,
                   "test-input-script",
                   &configuration->test.input_script_path,
                   "Optional path to a line-oriented .fdi input script played back once training-mode "
                   "gameplay has started (see docs/plan-frame-data-harness.md section 1.3).",
                   NULL,
                   0,
                   0),
        OPT_STRING(0,
                   "test-balance",
                   &configuration->test.balance,
                   "Balance mode for the test runner: 'ps2' (default) pins PS2 balance so the frame-data "
                   "corpora resolve identically everywhere; 'arcade' lets the normal auto-selection run so "
                   "the arcade-only branches can be measured (needs a verified CPS3 ROM).",
                   NULL,
                   0,
                   0),
        OPT_STRING(0,
                   "test-scene-preset",
                   &configuration->test.scene_preset,
                   "Optional named scripted gameplay preset (stage-heavy, effect-heavy, super-heavy, yun-sa3-repeat, yun-sa3-repeat-pressure, q-sa1-repeat, q-sa1-repeat-pressure, ken-sa3-repeat, ken-sa3-repeat-pressure, chunli-sa2-repeat, chunli-sa2-repeat-pressure, basic-exchange, pressure-exchange, left-corner-ryu-stage, training-yun-ryu-ryu-stage, training-frame-data).",
                   NULL,
                   0,
                   0),
        OPT_INTEGER(0,
                    "test-p1-character",
                    &configuration->test.characters[0],
                    "Override player 1 character for the default test runner path (0-19).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-p2-character",
                    &configuration->test.characters[1],
                    "Override player 2 character for the default test runner path (0-19).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-p1-super-art",
                    &configuration->test.super_arts[0],
                    "Override player 1 super art for the default test runner path (0-2).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-p2-super-art",
                    &configuration->test.super_arts[1],
                    "Override player 2 super art for the default test runner path (0-2).",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-p1-super-full",
                    &configuration->test.initial_super_full,
                    "Start player 1 with a full super meter on the first gameplay frame of the test runner path.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-training-sa-gauge",
                    &configuration->test.training_sa_gauge,
                    "Pin the training-mode S.A.GAUGE menu option every frame (0=NORMAL, "
                    "1=MAX START, 2=INFINITY, 3=MAXIMUM), re-latching init_E3_flag so the "
                    "engine re-reads it. Unset (default) leaves the menu cell untouched. "
                    "Requires a #if DEBUG build.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-preserve-game-transition",
                    &configuration->test.preserve_game_transition,
                    "Preserve the full pre-game transition in the test runner instead of mashing attacks to skip it.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-delay-gameplay-inputs-until-active",
                    &configuration->test.delay_gameplay_inputs_until_active,
                    "Delay scripted gameplay inputs and first-frame super bootstrap until both players reach gameplay/input-active state.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-stage",
                    &configuration->test.stage,
                    "Override stage for the default test runner path (0-19, excluding 17).",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-instant-jump",
                    &configuration->test.instant_jump,
                    "SPIKE (docs/savestates-and-instant-mode-jump.md §9 Q1): jump from the title screen "
                    "straight into a live training match by direct chain calls behind No_Trans, bypassing "
                    "menu and character select, then verify liveness, print SCENE-JUMP PASS/FAIL and "
                    "terminate. Prototype only; requires a #if DEBUG build and --test-enable (so the "
                    "gate-runner harness discovery must NOT match it). Combine with --ldreq-barrier-force "
                    "for a same-frame load drain. Characters/super-arts/stage come from "
                    "--test-p1-character etc. (defaults: Yun vs Ryu, first super art, Ryu stage).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-quick-training",
                    &configuration->test.quick_training_frame,
                    "Quick Training harness (src/quick_training.c): fire the OSD feature's request at this "
                    "prologue frame — the SIGRTMIN+5 path minus the signal — exercising the full shipped "
                    "sequence (diagonal wipe-out, teardown to title if needed, scene-jump chain, wipe-in). "
                    "Then verify, in order: liveness; that the engine DIP tables reached plw[] (init_omop "
                    "ran before the round boot); that the persisted training settings are live AND that the "
                    "file still holds what it held at frame 0; and that the in-round SELECT reset fires and "
                    "preserves FIRST ATTACK. Prints QUICK-TRAINING TEST PASS/FAIL and terminates. Seed the "
                    "run's THIRDSARM_HOME with a non-zero training config or the settings assertions are "
                    "skipped. Characters/arts come from that config exactly as on device. Requires a #if "
                    "DEBUG build and --test-enable.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-quick-training-again",
                    &configuration->test.quick_training_again_frame,
                    "Second Quick Training fire at this (absolute) prologue frame — schedule it after the "
                    "first sequence completes to exercise the mid-match teardown + re-jump edge case. "
                    "Requires --test-quick-training.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-pin-rng",
                    &configuration->test.pin_rng,
                    "Pin the training-mode RNG seed to zero (like network mode) instead of seeding from "
                    "Interrupt_Timer, so scripted harness runs are deterministic. Requires a #if DEBUG build.",
                    NULL,
                    0,
                    0),
        OPT_STRING(0,
                   "test-fcade-inputs",
                   &configuration->test.fcade_inputs_path,
                   "Step B3 EXPERIMENT (docs/plan-fcade-replay-browser.md): play a raw decoded Fightcade "
                   "-13 input stream (decode_inputs.py --bin output, arcade-RAM layout) from cold boot, "
                   "bypassing the test runner's menu automation, and print per-frame state samples. "
                   "Forces game-mode=arcade + arcade-balance=true for the session. Requires a #if DEBUG "
                   "build and --test-enable.",
                   NULL,
                   0,
                   0),
        OPT_INTEGER(0,
                    "test-fcade-offset",
                    &configuration->test.fcade_offset,
                    "First stream frame of --test-fcade-inputs to feed (default 0).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-anchor",
                    &configuration->test.fcade_anchor,
                    "App frame at which --test-fcade-inputs feeding starts (default 0 = first frame).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-p1-start",
                    &configuration->test.fcade_p1_start_frame,
                    "App frame at which to inject a deterministic 2-frame P1 START tap during "
                    "--test-fcade-inputs playback (stand-in for the join/credit state the FBNeo "
                    "savestate carries but the -13 stream omits). -1 (default) = off.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-p2-start",
                    &configuration->test.fcade_p2_start_frame,
                    "Same as --test-fcade-p1-start, for P2. -1 (default) = off.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-max-frames",
                    &configuration->test.fcade_max_frames,
                    "Exit(0) after sampling this many app frames of --test-fcade-inputs playback "
                    "(default 0 = run until killed).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-game-offset",
                    &configuration->test.fcade_game_offset,
                    "Step B3 candidate rule: B1 per-game stream offset of the first game's archive "
                    "frame 0; on the first round-start init frame the stream cursor re-anchors so "
                    "the engine consumes stream frame offset+k on archive frame k. -1 (default) = off.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-p1-char",
                    &configuration->test.fcade_p1_char,
                    "Validation-gate setup injection: force P1's character (ENGINE 20-id numbering) onto "
                    "the arcade char-select outcome during --test-fcade-inputs playback. -1 (default) = off.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-p2-char",
                    &configuration->test.fcade_p2_char,
                    "Same as --test-fcade-p1-char, for P2. -1 (default) = off.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-p1-arts",
                    &configuration->test.fcade_p1_arts,
                    "Force P1's Super Arts (raw arcade byte) during --test-fcade-inputs setup injection. "
                    "-1 (default) = off.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-p2-arts",
                    &configuration->test.fcade_p2_arts,
                    "Same as --test-fcade-p1-arts, for P2. -1 (default) = off.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-p1-color",
                    &configuration->test.fcade_p1_color,
                    "Force P1's color (raw arcade Player_Color byte) during --test-fcade-inputs setup "
                    "injection. -1 (default) = off.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-p2-color",
                    &configuration->test.fcade_p2_color,
                    "Same as --test-fcade-p1-color, for P2. -1 (default) = off.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-new-challenger",
                    &configuration->test.fcade_new_challenger,
                    "Force New_Challenger (and Champion = New_Challenger ^ 1, the arcade invariant) once "
                    "the game phase is entered during --test-fcade-inputs playback. -1 (default) = off.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-seed-ix16",
                    &configuration->test.fcade_seed_ix16,
                    "Seed Random_ix16 ONCE to this value on the first G_No[1]==2 frame of "
                    "--test-fcade-inputs playback (the shipped player/statcheck seed point). "
                    "-1 (default) = off.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-fcade-seed-ix32",
                    &configuration->test.fcade_seed_ix32,
                    "Same as --test-fcade-seed-ix16, for Random_ix32. -1 (default) = off.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-fcade-seed-at-reanchor",
                    &configuration->test.fcade_seed_at_reanchor,
                    "Apply --test-fcade-seed-ix16/-ix32 at the --test-fcade-game-offset re-anchor frame "
                    "(round-start init; pass the SCRD frame-1 values) instead of the first G_No[1]==2 "
                    "frame. Probes in-game RNG-index tracking.",
                    NULL,
                    0,
                    0),

        OPT_GROUP("Replay"),
        OPT_STRING(0,
                   "play-replay",
                   &configuration->replay.play_replay_path,
                   "Play back a .3sr replay file (docs/3sr-format.md) through the engine with "
                   "rendering and audio at normal speed.",
                   NULL,
                   0,
                   0),
        OPT_STRING(0,
                   "live-replay-p1",
                   &configuration->replay.live_p1_name,
                   "Player 1 display name for the replay overlay. Optional; requires --play-replay. "
                   "(The \"live\" spelling is historical.)",
                   NULL,
                   0,
                   0),
        OPT_STRING(0,
                   "live-replay-p2",
                   &configuration->replay.live_p2_name,
                   "Player 2 display name for the replay overlay. Optional; requires --play-replay.",
                   NULL,
                   0,
                   0),
        OPT_INTEGER(0,
                    "live-replay-p1-rank",
                    &configuration->replay.live_p1_rank,
                    "Player 1 Fightcade rank (1-6 = E..S) for the replay overlay; 0 (default) = "
                    "unranked/unknown. Requires --play-replay.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "live-replay-p2-rank",
                    &configuration->replay.live_p2_rank,
                    "Player 2 Fightcade rank (1-6 = E..S); 0 (default) = unranked/unknown. "
                    "Requires --play-replay.",
                    NULL,
                    0,
                    0),
        OPT_STRING(0,
                   "live-replay-date",
                   &configuration->replay.live_date_ms,
                   "Match date as ms-since-epoch (string — the value overflows int) for the "
                   "replay overlay. Optional; requires --play-replay.",
                   NULL,
                   0,
                   0),
        OPT_BOOLEAN(0,
                    "watch-replays",
                    &configuration->replay.watch_replays,
                    "Weekly-best shuffle viewer: play every cached .3sr under the replays root "
                    "back to back in a random order, forever, reshuffling at the end of the set. "
                    "Hold MP to skip to the next replay; hold START to leave. Mutually exclusive "
                    "with --play-replay.",
                    NULL,
                    0,
                    0),
        OPT_STRING(0,
                   "watch-replays-root",
                   &configuration->replay.watch_replays_root,
                   "Directory the shuffle viewer scans for *.3sr files (flat + one level of "
                   "subdirs). Overrides the 'replays-root' config key / platform default. "
                   "Requires --watch-replays.",
                   NULL,
                   0,
                   0),

#if defined(STATCHECK)
        OPT_GROUP("Statcheck"),
        OPT_STRING(0,
                   "ram-archive",
                   &configuration->statcheck.ram_archive_path,
                   "Path to the SCRD RAM archive to replay-check (required in a THREESX_STATCHECK build).",
                   NULL,
                   0,
                   0),
#endif

        OPT_GROUP("Rollback determinism harness (docs/rollback-determinism-harness.md)"),
        OPT_STRING(0,
                   "rbd-capture",
                   &configuration->test.rbd_capture_path,
                   "Write per-frame per-symbol hashes of the writable data/bss image to this file. "
                   "Requires --rbd-symmap and --rbd-frames, a Debug build, and ENABLE_NETPLAY=ON. "
                   "Driven by tools/rollback-determinism/check_rollback_determinism.py.",
                   NULL,
                   0,
                   0),
        OPT_STRING(0,
                   "rbd-symmap",
                   &configuration->test.rbd_symmap_path,
                   "Symbol map for --rbd-capture (text lines: hexaddr hexsize name), generated by the "
                   "driver from nm on this exact binary.",
                   NULL,
                   0,
                   0),
        OPT_INTEGER(0,
                    "rbd-frames",
                    &configuration->test.rbd_frames,
                    "Capture this many frames, then flush and exit cleanly (exit code 0).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "rbd-rollback-period",
                    &configuration->test.rbd_rollback_period,
                    "Inject a save/resim/load rollback cycle every N frames once character select is "
                    "reached. 0 (default) = baseline run with no rollbacks.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "rbd-rollback-depth",
                    &configuration->test.rbd_rollback_depth,
                    "Speculative frames simulated inside each injected rollback cycle (default 3).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "rbd-select-rollback-period",
                    &configuration->test.rbd_select_rollback_period,
                    "Character-select-phase cycle period (default 8; 0 disables select-phase cycles). "
                    "Aggressive select cadence hits known crash-class ppg asset-setup traps — see "
                    "docs/rollback-determinism-harness.md.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "rbd-select-rollback-depth",
                    &configuration->test.rbd_select_rollback_depth,
                    "Speculative depth for character-select-phase cycles (default 8, matching "
                    "production's input_prediction_window, netplay.c:903-905). Independent of "
                    "--rbd-rollback-depth, which bounds the in-game phase only.",
                    NULL,
                    0,
                    0),

        OPT_GROUP("Loader-timing invariance instrument (src/test/ldreq_timing_trace.h)"),
        OPT_STRING(0,
                   "ldreq-trace",
                   &configuration->test.ldreq_trace_path,
                   "Write one CSV row per outer frame describing the saved state the LDREQ loader "
                   "feeds (Exit_No/Exit_Timer/G_No/G_Timer) plus the loader's observable surface. "
                   "Requires --ldreq-trace-frames and a Debug build. Driven by "
                   "tools/ldreq-timing/check_ldreq_timing.py.",
                   NULL,
                   0,
                   0),
        OPT_INTEGER(0,
                    "ldreq-trace-frames",
                    &configuration->test.ldreq_trace_frames,
                    "Capture this many frames for --ldreq-trace, then flush and exit cleanly (0).",
                    NULL,
                    0,
                    0),
        OPT_STRING(0,
                   "ldreq-slot-trace",
                   &configuration->test.ldreq_slot_trace_path,
                   "Write one CSV row per (frame, q_ldreq slot) carrying every REQ field plus a "
                   "pointer-normalised raw byte image of the slot, so a residue in a DRAINED slot "
                   "can be told apart from one in a live slot. Requires --ldreq-trace (shares its "
                   "frame budget). Analysed by tools/ldreq-timing/check_slot_residue.py.",
                   NULL,
                   0,
                   0),
        OPT_BOOLEAN(0,
                    "ldreq-barrier-force",
                    &configuration->test.ldreq_barrier_force,
                    "Force the netplay LDREQ frame barrier on without a live GekkoNet session, so it "
                    "can be exercised from an offline test-runner scene. Harness use only.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "afs-inject-latency-ms",
                    &configuration->test.afs_inject_latency_ms,
                    "Hold back the OBSERVED completion of every async AFS read by N ms. Models a peer "
                    "with a slower disk; the physical read is untouched. Harness use only.",
                    NULL,
                    0,
                    0),

        OPT_END(),
    };

    struct argparse argparse;
    argparse_init(&argparse, options, NULL, 0);
    argparse_parse(&argparse, argc, argv);

#if ENABLE_NETPLAY
    /* Copy the direct-P2P handoff argv pointer into the fixed buffer and
     * raise the flag. Longer paths get a hard error because the wrapper
     * writes into a canonical tmpfs path that's always short; a path
     * that doesn't fit is a bug in the caller, not silent truncation. */
    if (direct_p2p_handoff_arg != NULL && direct_p2p_handoff_arg[0] != '\0') {
        const size_t len = strlen(direct_p2p_handoff_arg);
        if (len + 1 > sizeof(configuration->netplay.direct_p2p_handoff_path)) {
            error_out("--direct-p2p-handoff path exceeds 255 bytes.");
        }
        memcpy(configuration->netplay.direct_p2p_handoff_path, direct_p2p_handoff_arg, len + 1);
        configuration->netplay.direct_p2p_handoff_set = true;
    }
#endif

    verify_configuration(configuration);
}
