#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include "port/build_config.h"

#include <stdbool.h>

typedef struct NetplayConfiguration {
    int p2p_local_player;
    const char* p2p_remote_ip;
    /* Step 9 of docs/plan-stun-direct-p2p.md: direct-P2P handoff path.
     * When `direct_p2p_handoff_set` is true, main() reads the file at
     * `direct_p2p_handoff_path` after SDL init + DirectP2P_Init and
     * dispatches BeginHost/BeginJoin based on its contents. The fixed
     * 256-byte buffer holds the CLI-supplied path; argparse populates
     * it via a temporary `const char*` pointer copied in
     * `verify_configuration`. When `direct_p2p_handoff_set` is false,
     * main() still probes the config-default handoff path
     * (CFG_KEY_NETPLAY_DIRECT_P2P_HANDOFF_PATH) — if that file exists
     * the same dispatch runs, so the wrapper can either pass --direct-
     * p2p-handoff explicitly or drop the file at the default path. */
    bool direct_p2p_handoff_set;
    char direct_p2p_handoff_path[256];
} NetplayConfiguration;

typedef struct TestRunnerConfiguration {
    bool enabled;

    /* Task #108: which balance table a harness run must exercise, chosen
     * EXPLICITLY instead of falling out of `enabled` as a side effect.
     *
     * NULL (the default) keeps the historical behaviour every non-frame-data
     * harness relies on: a test-runner process pins PS2 so a corpus resolves
     * identically on every machine regardless of whether a CPS3 romset
     * happens to be installed (see ArcadeBalance_Init, src/arcade/
     * arcade_balance.c). "ps2" states that same intent out loud; "arcade"
     * asks for the shipping CPS3 balance and is a HARD REQUIREMENT - a run
     * that cannot reach 20/20-adapted arcade balance exits non-zero rather
     * than silently running the PS2 engine under an arcade label, which is
     * exactly the failure task #108 exists to close.
     *
     * Values: NULL | "ps2" | "arcade". Validated in args.c. */
    const char* balance;

    /* Task #108: extra frames the test runner idles on the CHARACTER-SELECT
     * screen before it starts driving cursors. 0 (default) is the historical
     * behaviour - the runner mashes through select in a handful of frames,
     * which is far short of UNIT_OF_TIMER_MAX (50, src/constants.h:6), the
     * period of one Select_Timer decrement. A select-timer observation needs
     * the screen to actually be inhabited; this is that dwell.
     *
     * Only meaningful in a #if DEBUG build (or -DENABLE_DEBUG_HOOKS=ON):
     * src/test/test_runner.c is wrapped in `#if defined(DEBUG)` end to end,
     * so the phase machine that reads this does not exist otherwise. Same
     * constraint as --test-pin-rng. `balance` above has NO such constraint -
     * ArcadeBalance_Init is ordinary shipped code. */
    int select_dwell_frames;

    const char* states_path;
    /* Phase 1 Step H1 (docs/plan-frame-data-harness.md section 1.3):
     * optional path to a line-oriented `.fdi` input script played back by
     * src/test/input_script.c once training-mode gameplay has started.
     * Mutually exclusive in practice with states_path (input_script takes
     * priority in TestRunner_Prologue's PHASE_GAME branch when set). */
    const char* input_script_path;
    const char* scene_preset;
    int characters[2];
    int super_arts[2];
    bool initial_super_full;
    /* EX/Supers program Step 1 (docs/plan-frame-data-completion.md's
     * exsuper plan): -1 (unset, the default) means "don't touch the
     * training S.A.GAUGE menu cell" — zero gameplay behavior. 0-3 mirror
     * the game's own training menu options (NORMAL/MAX START/INFINITY/
     * MAXIMUM, `effe3.c:95-123`'s switch on `Training[*].contents[0][1][0]`)
     * and are pinned every frame by apply_training_sa_gauge_overrides()
     * in test_runner.c, alongside a forced init_E3_flag re-latch. */
    int training_sa_gauge;
    int preserve_game_transition;
    int delay_gameplay_inputs_until_active;
    int stage;
    /* Phase 1 Step H3 (docs/plan-frame-data-harness.md section 1.5): when
     * true, the training-mode RNG reseed in game.c's Game01() zeroes
     * Random_ix16/32/_ex via Setup_Net_Random_ix() instead of seeding
     * Random_ix32/_ex from Interrupt_Timer, so two harness runs with the
     * same input script produce byte-identical FINAL annotation
     * sequences. Only takes effect in #if DEBUG builds. */
    bool pin_rng;

    /* Rollback-determinism harness (docs/rollback-determinism-harness.md,
     * src/test/rollback_determinism.c). All are inert unless
     * rbd_capture_path is set. Requires a #if DEBUG build with
     * ENABLE_NETPLAY=ON (the harness drives the production save_state/
     * load_state_from_event rollback path from game_state.c). */

    /* Output stream path: per-frame per-symbol hashes of the writable
     * data/bss image, consumed by
     * tools/rollback-determinism/check_rollback_determinism.py. */
    const char* rbd_capture_path;
    /* Symbol map (text: "hexaddr hexsize name" per line) generated by the
     * driver from `nm` on this exact binary. Required with capture. */
    const char* rbd_symmap_path;
    /* Stop after this many outer frames: flush, print a summary line and
     * exit 0 through the SDLApp_Exit() clean-quit path. Required > 0. */
    int rbd_frames;
    /* Every N outer frames while the test runner is in its in-game phase,
     * inject a save -> speculative-resimulate -> load rollback cycle
     * before the frame runs. 0 (default) = never: a baseline run. */
    int rbd_rollback_period;
    /* Speculative frames simulated inside each injected rollback cycle
     * (mirrors GekkoNet rolling_back advances with repeated-last-input
     * prediction). Default 3. */
    int rbd_rollback_depth;
    /* Character-select-phase cycle cadence. Select is covered separately
     * and at a GENTLE CADENCE (default period 8) because
     * every-frame cycles across select straddle one-shot ppg asset
     * setups and hit the crash-class arcade traps
     * (ppgSetupPalChunk hang / ppgSetupTexChunkSeqs NULL deref) — a
     * real, catalogued exposure the byte-diff harness scopes out; see
     * docs/rollback-determinism-harness.md "Known limits". 0 disables
     * select-phase cycles entirely. Only meaningful when
     * rbd_rollback_period > 0. */
    int rbd_select_rollback_period;

    /* Speculative depth for character-select-phase cycles. Defaults to 8,
     * matching production's input_prediction_window default
     * (netplay.c:903-905), so the shared gate probes select at the depth
     * GekkoNet actually predicts to rather than a quarter of it.
     *
     * INDEPENDENT of rbd_rollback_depth (task #63). It used to be
     * min(rbd_rollback_depth, this) — and before that a hard clamp to 2 —
     * which meant the in-game knob silently capped select coverage and the
     * only way to reach select depth 8 was to raise the IN-GAME depth to 8
     * too, changing what the in-game half measures and walking into the
     * crash class in docs/rollback-determinism-harness.md known limit 1.
     * The two knobs bound different risks, so they are now separate. */
    int rbd_select_rollback_depth;

    /* === Loader-timing invariance instrument (task #66) ===
     * See src/test/ldreq_timing_trace.h and
     * tools/ldreq-timing/check_ldreq_timing.py. */

    /* Write one CSV row per outer frame describing the saved state the
     * LDREQ loader feeds plus the loader's own observable surface. */
    const char* ldreq_trace_path;

    /* Row count, then flush + clean exit. Required with ldreq_trace_path. */
    int ldreq_trace_frames;

    /* === Per-slot LDREQ queue residue probe (task #69.2) ===
     * Side-channel companion to ldreq_trace_path: one CSV row per
     * (frame, q_ldreq slot) carrying every field of the REQ plus a
     * pointer-normalised raw byte image of the slot. Written to a
     * SEPARATE file on purpose — the barrier's timing-invariance gate
     * (check_ldreq_timing.py) compares whole rows of the main trace, and
     * folding 16 slots into it would change that gate's meaning. Requires
     * ldreq_trace_path (it shares its frame budget and its exit hook).
     * Analysed by tools/ldreq-timing/check_slot_residue.py. */
    const char* ldreq_slot_trace_path;

    /* Force Ldreq_BarrierActive() true without a live GekkoNet session,
     * so the barrier can be exercised from an offline test-runner scene.
     * Omitting it is the instrument's built-in neutralization: the same
     * binary then takes the unbarriered path and the comparison must go
     * red. */
    bool ldreq_barrier_force;

    /* Hold back the OBSERVED completion of every async AFS read by this
     * many milliseconds (AFS_SetInjectedLatencyMs, port/io/afs.h). This
     * is the independent variable: two runs differing only in this value
     * stand in for two peers whose disks differ. */
    int afs_inject_latency_ms;
} TestRunnerConfiguration;

#if ENABLE_PERF_TELEMETRY
typedef struct PerfCaptureConfiguration {
    int frame_count;
    const char* output_path;
    const char* scene;
    bool basic_mode;
    bool basic_first_window_family_snapshots;
    bool basic_first_window_render_subphases;
    bool basic_first_window_exact_hot_family_alpha_offpath;
    bool basic_first_window_onset_exact_hot_family_alpha_offpath;
    bool basic_first_window_onset_cluster_alpha_offpath;
    bool fast_non_integer_disable_reuse_telemetry;
    bool fast_non_integer_enable_subrect_alpha_telemetry;
    bool wait_for_gameplay;
    const char* wait_for_test_phase;
    const char* wait_for_runtime_state;
    int gameplay_warmup_frames;
    bool software_frame_parity_check;
} PerfCaptureConfiguration;
#endif

typedef struct Configuration {
    NetplayConfiguration netplay;
    TestRunnerConfiguration test;
#if ENABLE_PERF_TELEMETRY
    PerfCaptureConfiguration perf;
#endif
    bool probe_renderer_only;
    bool headless;
    /* Phase 6 Step 8 (docs/archive/plan-netplay-phase6.md): when true, main() runs
     * the MIST handshake test harness and exits. Honors the CLI flag
     * --test-mist-handshake. Parsed unconditionally; the real test body
     * is only compiled in when ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS
     * is defined (otherwise the stub returns 2). The test uses a
     * localhost UDP socket pair; no external network dependency. */
    bool test_mist_handshake;
    /* Step 2 of docs/plan-stun-direct-p2p.md: when true, main() runs
     * the room-code codec test harness and exits. Honors the CLI flag
     * --test-room-code. Parsed unconditionally; the real test body is
     * only compiled in when ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS
     * is defined (otherwise the stub returns 2). The test is pure
     * in-process — no socket or network dependency. */
    bool test_room_code;
    /* Task #119: when true, main() runs the late-punch rescue-layer
     * unit harness (src/netplay/test_late_punch.c) and exits. Honors
     * the CLI flag --test-late-punch. Same gating pattern as the other
     * netplay harnesses (stub returns 2 without ENABLE_NETPLAY_TESTS).
     * In-process + loopback sockets only — no netns dependency; the
     * accept/reject/relearn decision is what it pins. */
    bool test_late_punch;
    /* Step 12 of docs/plan-stun-direct-p2p.md: when true, main() runs
     * the STUN mock-server test harness and exits. Honors the CLI flag
     * --test-stun-mock. Parsed unconditionally; the real test body is
     * only compiled in when ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS
     * is defined (otherwise the stub returns 2). The test spins up a
     * localhost UDP listener that speaks a canned STUN Binding
     * Response with XOR-MAPPED-ADDRESS; no external network dep. */
    bool test_stun_mock;
    /* Sparse effect-pool save Option A — round-trip parity tests for the
     * pack/unpack helpers in src/netplay/game_state.c. Honors the CLI flag
     * --test-sparse-effect-save. Parsed unconditionally; real body is
     * gated by ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS, otherwise the
     * stub returns 2. Pure in-process — no GekkoNet session required. */
    bool test_sparse_effect_save;
    /* Task #132 priority 3: the fast netplay unit harness
     * (src/netplay/test_netplay_units.c). Honors --test-netplay-units.
     * Everything it asserts used to live in test_bilateral_punch.c behind
     * scenario tests that open sockets and sleep; the blocks are pure, so
     * they were moved somewhere that finishes in milliseconds. Parsed
     * unconditionally; the real body is gated by ENABLE_NETPLAY=ON &&
     * ENABLE_NETPLAY_TESTS, otherwise the stub returns 2. */
    bool test_netplay_units;
    /* Step 6 of docs/plan-bilateral-hole-punch.md: when true, main() runs
     * the bilateral hole-punch test harness and exits. Honors the CLI flag
     * --test-bilateral-punch. Parsed unconditionally; the real test body
     * is only compiled in when ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS
     * is defined (otherwise the stub returns 2). The test exercises the
     * rendezvous wire codec, session-key derivation, LAN-bypass table,
     * and the kill-switch config gate; uses only localhost UDP sockets. */
    bool test_bilateral_punch;
    /* #36: when true, main() runs the connect-observability proof harness
     * (src/netplay/test_connect_observability.c) and exits. Honors the CLI
     * flag --test-connect-observability. Parsed unconditionally; the real
     * body is only compiled in when ENABLE_NETPLAY=ON &&
     * ENABLE_NETPLAY_TESTS is defined (otherwise the stub returns 2). It
     * induces a rendezvous version skew and a silent server on loopback
     * UDP sockets, drives one race per case, and reads the per-session
     * netplay log file back off disk to prove the evidence actually
     * reaches the file a tester sends us. */
    bool test_connect_observability;
    /* M-3 coverage guard: when true, main() runs the GameState
     * save/load field-coverage harness (randomized load->save round-trip
     * that fails loudly on any struct byte GS_SAVE/GS_LOAD misses) and
     * exits. Honors the CLI flag --test-gs-coverage. Parsed
     * unconditionally; real body gated by ENABLE_NETPLAY=ON &&
     * ENABLE_NETPLAY_TESTS, otherwise the stub returns 2. Pure
     * in-process — no session, no sockets. */
    bool test_gs_coverage;
    /* Task #122: when true, main() runs the rendezvous wire-codec unit
     * harness (src/netplay/test_rendezvous_wire.c) and exits. Honors the
     * CLI flag --test-rendezvous-wire. Parsed unconditionally; the real
     * body is gated by ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS,
     * otherwise the stub returns 2. Pure in-process — no session, no
     * sockets, and no NETPLAY_TEST_HOOKS: it exercises Rendezvous_*
     * (NACK parse, reason text, frame routing, signal-URL parse) and the
     * reason -> ConnectFailCode verdict mapping, all of which are in the
     * shipped build. */
    bool test_rendezvous_wire;
    /* Task #132: when true, main() runs the MIST compat-gate unit harness
     * (src/netplay/test_mist_compat_gate.c) and exits. Honors the CLI flag
     * --test-mist-compat-gate. Parsed unconditionally; the real body is
     * gated by ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS, otherwise the
     * stub returns 2. Pure in-process — no session, no sockets, and no
     * NETPLAY_TEST_HOOKS: it drives classify_peer_payload, parse_header
     * and the four bounds-checked payload readers directly through the
     * ENABLE_NETPLAY_TESTS trampolines in mist_handshake.c. */
    bool test_mist_compat_gate;
    /* Task #132: when true, main() runs the punch-predicate unit harness
     * (src/netplay/test_punch_predicates.c) and exits. Honors the CLI flag
     * --test-punch-predicates. Parsed unconditionally; the real body is
     * gated by ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS, otherwise the
     * stub returns 2. Pure in-process — no sockets and no
     * NETPLAY_TEST_HOOKS: it sweeps Stun_IsBindingResponse /
     * Stun_HasPunchPrefix / Stun_IsPunchPayload bit by bit, and drives the
     * three late-punch decisions (foreign IP, relearn cap, learned-target)
     * that are invisible from the socket harness. */
    bool test_punch_predicates;
    /* Tasks #59/#61: when true, main() runs the ext texture-cache
     * brick-prevention harness (src/test/test_texcash_bounds.c) and exits.
     * Honors --test-texcash-bounds. Parsed unconditionally; the real body is
     * gated on ENABLE_NETPLAY_TESTS only -- it touches no netplay code, so it
     * does not need ENABLE_NETPLAY. Pure in-process: no session, no sockets,
     * no SDL window. */
    bool test_texcash_bounds;
    /* Doc item Q (§8.Q/§21, docs/research-arcade-cg-data-accuracy.md): when
     * true, main() runs the per-character cg_se sound-code remap unit
     * harness (src/test/test_cg_se_remap.c) and exits. Honors
     * --test-cg-se-remap. Parsed unconditionally; the real body is gated on
     * ENABLE_NETPLAY_TESTS only -- it touches no netplay code, so it does
     * not need ENABLE_NETPLAY. Pure in-process: no ROM, no session, no SDL
     * window. */
    bool test_cg_se_remap;
} Configuration;

#endif
