#include "main.h"
#include "arcade/arcade_balance.h"
#include "args.h"
#include "common.h"
#include "configuration.h"
#include "netplay/direct_p2p.h"
#include "netplay/room_code.h"
#include "netplay/direct_p2p_handoff.h"
#include "netplay/netplay.h"
#include "netplay/netplay_nav.h"
#include "port/sdl/sdl_app.h"
#include "quick_training.h"
#include "sf33rd/AcrSDK/common/mlPAD.h"
#include "sf33rd/AcrSDK/ps2/flps2debug.h"
#include "sf33rd/AcrSDK/ps2/flps2etc.h"
#include "sf33rd/AcrSDK/ps2/flps2render.h"
#include "sf33rd/AcrSDK/ps2/foundaps2.h"
#include "sf33rd/Source/Common/MemMan.h"
#include "sf33rd/Source/Common/PPGFile.h"
#include "sf33rd/Source/Common/PPGWork.h"
#include "sf33rd/Source/Compress/zlibApp.h"
#include "sf33rd/Source/Game/debug/Debug.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/init3rd.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/io/ioconv.h"
#include "sf33rd/Source/Game/menu/menu.h"
#include "sf33rd/Source/Game/rendering/color3rd.h"
#include "sf33rd/Source/Game/rendering/dc_ghost.h"
#include "sf33rd/Source/Game/rendering/mtrans.h"
#include "sf33rd/Source/Game/rendering/texcash.h"
#include "sf33rd/Source/Game/sound/sound3rd.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/ramcnt.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/sys_sub2.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/frame_data_overlay.h"
#include "sf33rd/Source/Game/ui/frame_trace.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"
#include "replay/replay_player.h"
#include "replay/replay_shuffle.h"
#include "replay/replay_wipe.h"
#include "structs.h"
#include "test/ldreq_timing_trace.h"
#include "test/rollback_determinism.h"
#include "test/test_runner.h"

#if defined(STATCHECK)
#include "test/statcheck_runner.h"
#endif

#if defined(DEBUG)
#include "sf33rd/Source/Game/debug/debug_config.h"
#endif

#include "port/io/afs.h"
#include "port/linux/console_mode.h"
#include "port/resources.h"

#include <SDL3/SDL.h>

#if defined(_WIN32) && defined(DEBUG)
// Including windows.h causes conflicts with the Polygon struct, so I just included the header where
// AllocConsole is and the Windows-specific typedefs that it requires.
#include <windef.h>

#include <ConsoleApi.h>
#endif

#include <memory.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

typedef enum MainPhase {
    MAIN_PHASE_INIT,
    MAIN_PHASE_COPYING_RESOURCES,
    MAIN_PHASE_INITIALIZED,
} MainPhase;

s32 system_init_level;
MPP mpp_w;
Configuration configuration = {
    .test =
        {
            .balance = NULL,
            .select_dwell_frames = 0,
            .scene_preset = NULL,
            .characters = { -1, -1 },
            .super_arts = { -1, -1 },
            .initial_super_full = false,
            .training_sa_gauge = -1,
            .preserve_game_transition = false,
            .delay_gameplay_inputs_until_active = false,
            .stage = -1,
            .instant_jump = false,
            .quick_training_frame = -1,
            .quick_training_again_frame = -1,
            .rbd_capture_path = NULL,
            .rbd_symmap_path = NULL,
            .rbd_frames = 0,
            .rbd_rollback_period = 0,
            .rbd_rollback_depth = 3,
            .rbd_select_rollback_period = 8,
            .rbd_select_rollback_depth = 8,
            .ldreq_trace_path = NULL,
            .ldreq_trace_frames = 0,
            .ldreq_slot_trace_path = NULL,
            .ldreq_barrier_force = false,
            .afs_inject_latency_ms = 0,
            .fcade_inputs_path = NULL,
            .fcade_offset = 0,
            .fcade_anchor = 0,
            .fcade_p1_start_frame = -1,
            .fcade_p2_start_frame = -1,
            .fcade_max_frames = 0,
            .fcade_game_offset = -1,
            .fcade_p1_char = -1,
            .fcade_p2_char = -1,
            .fcade_p1_arts = -1,
            .fcade_p2_arts = -1,
            .fcade_p1_color = -1,
            .fcade_p2_color = -1,
            .fcade_new_challenger = -1,
            .fcade_seed_ix16 = -1,
            .fcade_seed_ix32 = -1,
            .fcade_seed_at_reanchor = false,
        },
};

static u8 dctex_linear_mem[0x800];
static u8 texcash_melt_buffer_mem[0x1000];
static u8 tpu_free_mem[0x2000];
static MainPhase phase = MAIN_PHASE_INIT;

static volatile sig_atomic_t shutdown_signal = 0;
static volatile sig_atomic_t fps_toggle_requested = 0;
static volatile sig_atomic_t arm_clock_cycle_requested = 0;
static volatile sig_atomic_t game_mode_cycle_requested = 0;
static volatile sig_atomic_t hold_to_pause_cycle_requested = 0;
static volatile sig_atomic_t quick_training_requested = 0;

static u8* mppMalloc(u32 size) {
    return flAllocMemory(size);
}

// Signal-safe: emit "[3sx] signal N\n" to stderr. Uses write() + manual
// itoa because fprintf/printf are not async-signal-safe. Logs only once
// per fatal-class signal so we can distinguish wrapper-kill (SIGTERM)
// from internal abort in post-mortem wrapper logs.
#if !defined(_WIN32)
static void log_shutdown_signal_safe(int signo) {
    char buf[32];
    static const char prefix[] = "[3sx] signal ";
    size_t pos = 0;
    memcpy(buf, prefix, sizeof(prefix) - 1);
    pos += sizeof(prefix) - 1;

    int n = signo;
    if (n < 0) { buf[pos++] = '-'; n = -n; }
    char digits[8];
    int d = 0;
    do { digits[d++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0 && d < (int)sizeof(digits));
    while (d > 0) { buf[pos++] = digits[--d]; }
    buf[pos++] = '\n';
    (void)!write(STDERR_FILENO, buf, pos);
}
#endif

static void on_shutdown_signal(int signo) {
#ifdef SIGUSR1
    if (signo == SIGUSR1) {
        fps_toggle_requested = 1;
        return;
    }
#endif

#ifdef SIGRTMIN
    if (signo == SIGRTMIN + 2) {
        arm_clock_cycle_requested = 1;
        return;
    }

    if (signo == SIGRTMIN + 3) {
        game_mode_cycle_requested = 1;
        return;
    }

    if (signo == SIGRTMIN + 4) {
        hold_to_pause_cycle_requested = 1;
        return;
    }

    /* OSD "Quick Training" row (menu.sv T[15]) -> thirdsarm_wrapper.cpp
     * quick_training_signal(). Live in-process jump; no restart. */
    if (signo == SIGRTMIN + 5) {
        quick_training_requested = 1;
        return;
    }
#endif

#if !defined(_WIN32)
    log_shutdown_signal_safe(signo);
#endif
    shutdown_signal = signo;
}

static void install_shutdown_signal_handlers() {
#if defined(_WIN32)
    /* MinGW has signal() and SIGINT/SIGTERM, but no sigaction, SIGHUP or
     * SIGUSR1. The absent ones are all MiSTer-side controls -- the wrapper
     * raises SIGUSR1 to toggle the FPS overlay and the SIGRTMIN range to
     * cycle settings -- and nothing on the desktop sends them. Ctrl+C and
     * a normal terminate are the two that still need to shut down cleanly,
     * and both exist here. */
    signal(SIGINT, on_shutdown_signal);
    signal(SIGTERM, on_shutdown_signal);
#else
    struct sigaction action;
    SDL_zero(action);
    action.sa_handler = on_shutdown_signal;
    sigemptyset(&action.sa_mask);

    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGUSR1, &action, NULL);
#ifdef SIGRTMIN
    sigaction(SIGRTMIN + 2, &action, NULL);
    sigaction(SIGRTMIN + 3, &action, NULL);
    sigaction(SIGRTMIN + 4, &action, NULL);
    sigaction(SIGRTMIN + 5, &action, NULL);
#endif
#endif
}

static void restore_shutdown_signal_handlers() {
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
#ifdef SIGHUP
    signal(SIGHUP, SIG_DFL);
#endif
#ifdef SIGUSR1
    signal(SIGUSR1, SIG_DFL);
#endif
#ifdef SIGRTMIN
    signal(SIGRTMIN + 2, SIG_DFL);
    signal(SIGRTMIN + 3, SIG_DFL);
    signal(SIGRTMIN + 4, SIG_DFL);
    signal(SIGRTMIN + 5, SIG_DFL);
#endif
}

// Initialization

#if defined(ENABLE_NETPLAY)
/*
 * Step 9 of docs/plan-stun-direct-p2p.md — Resolve the handoff source
 * path (CLI flag takes priority, config-default HANDOFF_PATH is the
 * fallback). Returns NULL when there's nothing to dispatch.
 */
static const char* resolve_direct_p2p_handoff_path(void) {
    if (configuration.netplay.direct_p2p_handoff_set) {
        return configuration.netplay.direct_p2p_handoff_path;
    }
    const char* cfg_path = DirectP2PHandoff_ConfigDefaultPath();
    if (cfg_path == NULL || cfg_path[0] == '\0') {
        return NULL;
    }
    if (!DirectP2PHandoff_FileExists(cfg_path)) {
        return NULL;
    }
    return cfg_path;
}

/*
 * Step 9 dispatch — read the handoff file, consume it (unlink), then
 * invoke DirectP2P_BeginHost / DirectP2P_BeginJoin based on the parsed
 * mode. main.c keeps both call sites visible so the dispatch wiring is
 * greppable; the parsing itself lives in src/netplay/direct_p2p_handoff.c.
 */
static void dispatch_direct_p2p_handoff(void) {
    const char* path = resolve_direct_p2p_handoff_path();
    if (path == NULL) {
        return;
    }

    DirectP2PHandoff handoff;
    if (!DirectP2PHandoff_ReadFile(path, &handoff)) {
        return;
    }

    /* One-shot: drop the file before we kick off a worker thread so a
     * mid-session SIGKILL can't leave a stale handoff on disk. The
     * wrapper's fork/execve ordering guarantees the file was fully
     * written before this child even started, so unlink-then-dispatch
     * is race-free. */
    DirectP2PHandoff_Consume(path);

    switch (handoff.mode) {
    case DIRECT_P2P_HANDOFF_MODE_HOST:
        fprintf(stderr, "[direct_p2p_handoff] dispatching Host (port=%d)\n", handoff.port);
        DirectP2P_BeginHost(handoff.port);
        break;
    case DIRECT_P2P_HANDOFF_MODE_JOIN:
        {
            /* S4-review MEDIUM-4 (re-scoped for v4, room_code.h): redacted
             * here the same way the host redacts its own — hygiene, not
             * confidentiality, as of v4. See RoomCode_Redact. */
            char peer_code_redacted[ROOM_CODE_BUF_LEN];
            RoomCode_Redact(handoff.peer_code, peer_code_redacted);
            fprintf(stderr,
                    "[direct_p2p_handoff] dispatching Join (peer_code=%s, redacted)\n",
                    peer_code_redacted);
        }
        DirectP2P_BeginJoin(handoff.peer_code);
        break;
    case DIRECT_P2P_HANDOFF_MODE_NONE:
    default:
        break;
    }
}
#endif

static void set_netplay_params() {
#if defined(ENABLE_NETPLAY)
    /* S3 (docs/plan-netplay-connection.md §5): initialize the direct-P2P
     * orchestrator for EVERY netplay entry path, not just the handoff
     * one. DirectP2P_Init registers the session-teardown callback that
     * converts a latched session failure (MIST reject, CONNECTING
     * deadline) into DIRECT_P2P_FAILED_HANDSHAKE + the on-screen reason;
     * pre-S3 it only ran on the handoff branch, so the LAN-CLI path
     * surfaced reject reasons on stdout ONLY and the player saw a
     * silent drop to attract. Init is idempotent and spawns no worker. */
    DirectP2P_Init();
    if (configuration.netplay.p2p_remote_ip != NULL) {
        Netplay_SetParams(configuration.netplay.p2p_local_player, configuration.netplay.p2p_remote_ip);
        /* Netplay_SetParams already wired remote_ip, so the nav state
         * machine's NAV_WAIT_ORCHESTRATOR state will see
         * Netplay_IsRemoteIpSet() true immediately and only gate on
         * the menu-nav frames above it. */
        NetplayNav_Arm();
    } else {
        /* Direct-P2P dispatch is deferred to the main game loop tick. The
         * orchestrator's worker thread publishes state transitions the
         * overlay renderer reads; if those happen before njUserInit()
         * completes ppg_Initialize the overlay's SSPutStrPro path hits
         * an uninitialized sprite bank and segfaults. DirectP2P_Init ran
         * above (no worker spawned) so DirectP2P_Tick has valid state
         * from frame 0; the actual BeginHost/BeginJoin fires once on
         * first tick. See defer_direct_p2p_handoff_tick(). */
        /* Arm nav ONLY when a handoff file actually exists — a normal
         * cold OSD launch with no handoff is indistinguishable from the
         * "netplay requested" case until we check the file system, and
         * arming nav in the plain-boot case makes "CONNECTING..."
         * appear and nav synthesize Start presses even though no peer
         * is coming. resolve_direct_p2p_handoff_path() returns NULL
         * when neither the --direct-p2p-handoff CLI flag was set nor
         * the config default path has a file on disk. */
        if (resolve_direct_p2p_handoff_path() != NULL) {
            NetplayNav_Arm();
        }
    }
#endif
}

#if defined(ENABLE_NETPLAY)
/* One-shot: on the first game-loop tick, read the handoff file and kick
 * off Host/Join. By this point njUserInit() has run and the sprite bank
 * / ppg list is ready, so any state transition the worker publishes can
 * be safely rendered by the overlay. */
static void defer_direct_p2p_handoff_tick(void) {
    static bool dispatched = false;
    if (dispatched) return;
    dispatched = true;
    /* Arm-time predicate: the handoff dispatch (BeginHost/BeginJoin) runs
     * independently of NetplayNav_Arm, so it needs its own gate — without
     * this the orchestrator would still host/join even though nav refused.
     * Netplay_RefuseArm keeps the overlay reason posted (idempotent with
     * the refusal NetplayNav_Arm already issued at boot). */
    if (!Netplay_ArmAllowed()) {
        if (resolve_direct_p2p_handoff_path() != NULL) {
            Netplay_RefuseArm();
        }
        return;
    }
    if (configuration.netplay.p2p_remote_ip != NULL) {
        // LAN/localhost direct-P2P path: Netplay_SetParams already wired
        // remote_ip/local_port/remote_port via set_netplay_params, and
        // set_netplay_params() also armed the nav state machine. The nav
        // module now owns the Netplay_BeginDirectP2P() call — it fires
        // only after Title -> Mode Select -> Versus have played out via
        // injected Start presses so char-select init side-effects run.
        return;
    }
    dispatch_direct_p2p_handoff();
}
#endif

void cpInitTask() {
    memset(&task, 0, sizeof(task));
}

Language Get_Default_Language() {
    int locale_count;
    SDL_Locale** locales = SDL_GetPreferredLocales(&locale_count);

    if (locales == NULL) {
        return LANG_ENGLISH;
    }

    Language language = LANG_ENGLISH;

    for (int i = 0; i < locale_count; i++) {
        if (SDL_strcmp(locales[i]->language, "ja") == 0) {
            language = LANG_JAPANESE;
            break;
        } else if (SDL_strcmp(locales[i]->language, "en") == 0) {
            language = LANG_ENGLISH;
            break;
        }
    }

    SDL_free(locales);
    return language;
}

static void njUserInit() {
    u32 size;

    sysFF = 1;
    mpp_w.sysStop = false;
    mpp_w.inGame = false;
    mpp_w.language = Get_Default_Language();
    mmSystemInitialize();
    flGetFrame(&mpp_w.fmsFrame);
    seqsInitialize(mppMalloc(seqsGetUseMemorySize()));
    ppg_Initialize(mppMalloc(0x60000), 0x60000);
    zlib_Initialize(mppMalloc(0x10000), 0x10000);
    size = flGetSpace();
    mpp_w.ramcntBuff = mppMalloc(size);
    Init_ram_control_work(mpp_w.ramcntBuff, size);

    Interrupt_Timer = 0;
    Disp_Size_H = 100;
    Disp_Size_V = 100;
    Country = 4;

    if (Country == 0) {
        while (1) {}
    }

    Init_sound_system();
    Init_bgm_work();
    sndInitialLoad();
    cpInitTask();
    cpReadyTask(TASK_INIT, Init_Task);
}

static void distributeScratchPadAddress() {
    dctex_linear = (s16*)dctex_linear_mem;
    texcash_melt_buffer = (u8*)texcash_melt_buffer_mem;
    tpu_free = (TexturePoolUsed*)tpu_free_mem;
}

static void sf3_init() {
#if defined(DEBUG)
    DebugConfig_Init();
#endif

    flInitialize();
    flSetRenderState(FLRENDER_BACKCOLOR, 0);
    system_init_level = 0;
    ppgWorkInitializeApprication();
    distributeScratchPadAddress();
    njdp2d_init();
    njUserInit();
    palCreateGhost();
    ppgMakeConvTableTexDC();
    appSetupBasePriority();
}

#if defined(_WIN32) && defined(DEBUG)
static void init_windows_console() {
    // attaches to an existing console for printouts. Works with windows CMD but not MSYS2
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
        // if fails, then allocate a new console
        AllocConsole();
    }
    freopen("CONIN$", "r", stdin);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
}
#endif

static void initialize_game() {
    SDLApp_FullInit();

#if defined(_WIN32) && defined(DEBUG)
    init_windows_console();
#endif

    /* --probe-renderer-only: SDLApp_FullInit() has already run
     * log_backend_diagnostics() (video/render/audio driver enumeration)
     * and init_window() (actual SDL_CreateWindowAndRenderer + the
     * "Selected video driver"/"Selected renderer" log lines) — that IS
     * the SDL video/render backend probe the flag's help text promises.
     * Stop here, before any ROM/AFS/netplay work, so the flag does what
     * it says instead of silently falling through to a normal run.
     * SDLApp_Quit() tears down the window/renderer/subsystems cleanly;
     * exit() (not return) so the atexit(ConsoleMode_Exit) registered by
     * ConsoleMode_Enter() on success still restores the console. */
    if (configuration.probe_renderer_only) {
        SDL_Log("--probe-renderer-only: SDL video/render backend probe complete, exiting.");
        SDLApp_Quit();
        exit(0);
    }

#if defined(STATCHECK)
    /* Review round-1 finding P-1 (docs/plan-fcade-replay-browser.md): pin
     * the oracle's ambient config dependencies (game-mode, arcade-balance,
     * default button mapping) so the harness verdict is hermetic against
     * the user's on-disk config. Placed before ArcadeBalance_Init() so the
     * arcade-balance pin is in place before that call reads it.
     *
     * NOTE (upstream reconcile): this used to sit AFTER set_netplay_params()
     * to keep netplay-nav config forcing from racing it. set_netplay_params
     * now runs LAST (see the ordering comment below), so the pins moved
     * ahead of it. Safe: set_netplay_params touches only DirectP2P/Netplay
     * params and nav arming, while the pins touch only CFG_ARCADE_BALANCE,
     * the game-mode flag and save_w[].Pad_Infor — disjoint sets. */
    StatcheckRunner_PinConfig();
#endif

#if defined(DEBUG)
    /* Step B3 EXPERIMENT (docs/plan-fcade-replay-browser.md): raw -13
     * stream playback needs the same ambient-config pinning idea as the
     * statcheck harness, but for the ARCADE flow: game-mode=arcade (the
     * Fightcade session ran the arcade program flow, not the console
     * menus) and arcade-balance=true (the session was captured against
     * sfiii3nr1's data tables). Session-only; placed before
     * ArcadeBalance_Init() below so the pin is visible to it. Inert
     * unless --test-enable + --test-fcade-inputs are both set. */
    if (configuration.test.enabled && configuration.test.fcade_inputs_path != NULL) {
        TestRunner_PinFcadeConfig();
    }
#endif

    /* Step C1 (docs/plan-fcade-replay-browser.md): same pin, same slot, for
     * .3sr playback — arcade-balance=true + game-mode=console are the
     * A3b-proven co-necessities for archived Fightcade sessions to
     * reproduce. Session-only; never writes the on-disk config. Placed
     * before ArcadeBalance_Init() so the arcade-balance pin is visible to
     * it. (A STATCHECK build rejects --play-replay in args.c, so this and
     * the statcheck pin can never both be live.) */
    if (ReplayPlayer_IsActive()) {
        ReplayPlayer_PinConfig();
        /* Adopt the caller-supplied row metadata (player names + Fightcade
         * ranks + date) passed alongside --play-replay, so the HUD name
         * labels and the bottom status line are populated from the first
         * frame even when the .3sr has no .meta.json sidecar beside it. All
         * fields optional; a sidecar that does turn up corroborates rather
         * than clobbers these. (The flags still spell "--live-replay-*" —
         * historical naming from the retired live-stream path, kept as-is.) */
        long long date_ms = 0;
        if (configuration.replay.live_date_ms != NULL) {
            date_ms = SDL_strtoll(configuration.replay.live_date_ms, NULL, 10);
        }
        ReplayPlayer_SetLiveMeta(configuration.replay.live_p1_name, configuration.replay.live_p1_rank,
                                 configuration.replay.live_p2_name, configuration.replay.live_p2_rank, date_ms);
    } else if (ReplayShuffle_IsEnabled()) {
        /* LOAD-BEARING. The shuffle viewer plays every replay through
         * ReplayPlayer_LoadAndStart, which deliberately does NOT re-pin the
         * config — the pin is a whole-session obligation the owning module
         * inherits (it used to be the browser's). Miss this and every replay
         * runs with the wrong balance, game mode and button mapping, which
         * presents as a desync rather than as a missing call. Applied here,
         * before ArcadeBalance_Init(), so the arcade-balance pin is visible to
         * it. (Mutually exclusive with the --play-replay branch above:
         * args.c rejects the combination.) */
        SDL_Log("replay-shuffle: enabled — applying the whole-session config pin "
                "(console + arcade-balance + identity buttons)");
        ReplayPlayer_PinConfig();
    }

    /* Ordering matters:
     *   AFS_Init          — the boot-time arcade adaptation reads each
     *                       character's PS2 char-data tail from the AFS;
     *   ArcadeBalance_Init— resolves arcade-vs-PS2 for the whole process
     *                       (ROM discovery + full 20-character adaptation);
     *   set_netplay_params— netplay arming consults the resolved balance
     *                       state (netplay requires verified arcade).
     * The replay/statcheck config pins above run before all three, exactly
     * as they did pre-reconcile, so ArcadeBalance_Init still observes them. */
    AFS_Init(Resources_GetAFSPath());
    ArcadeBalance_Init();
    set_netplay_params();
    sf3_init();
}

static void cleanup() {
    ReplayShuffle_Destroy();
    ReplayPlayer_Destroy();
    AFS_Finish();
    SDLApp_Quit();
}

// Iteration

static void cpLoopTask() {
#if defined(DEBUG)
    disp_ramcnt_free_area();

    if (sysSLOW) {
        if (--Slow_Timer == 0) {
            sysSLOW = 0;
            Game_pause &= 0x7F;
        } else {
            Game_pause |= 0x80;
        }
    }
#endif

    for (int i = 0; i < 11; i++) {
        struct _TASK* task_ptr = &task[i];

        switch (task_ptr->condition) {
        case 1:
            task_ptr->func_adrs(task_ptr);
            break;

        case 2:
            task_ptr->condition = 1;
            break;

        case 3:
            break;
        }
    }
}

static void appCopyKeyData() {
    // FIXME: Should PLsw be saved/restored too?
    PLsw[0][1] = PLsw[0][0];
    PLsw[1][1] = PLsw[1][0];
    PLsw[0][0] = p1sw_buff;
    PLsw[1][0] = p2sw_buff;
}

void njUserMain() {
    CPU_Time_Lag[0] = 0;
    CPU_Time_Lag[1] = 0;
    CPU_Rec[0] = 0;
    CPU_Rec[1] = 0;

    Check_Replay_Status(0, Replay_Status[0]);
    Check_Replay_Status(1, Replay_Status[1]);

    cpLoopTask();

    if ((Game_pause != 0x81) && (Mode_Type == MODE_VERSUS) && (Play_Mode == 1)) {
        if ((plw[0].wu.wu_operator == 0) && (CPU_Rec[0] == 0) && (Replay_Status[0] == 1)) {
            p1sw_0 = 0;

            Check_Replay_Status(0, 1);

            if (Debug_w[0x21]) {
                flPrintColor(0xFFFFFFFF);
                flPrintL(0x10, 0xA, "FAKE REC! PL1");
            }
        }

        if ((plw[1].wu.wu_operator == 0) && (CPU_Rec[1] == 0) && (Replay_Status[1] == 1)) {
            p2sw_0 = 0;

            Check_Replay_Status(1, 1);

            if (Debug_w[0x21]) {
                flPrintColor(0xFFFFFFFF);
                flPrintL(0x10, 0xA, "FAKE REC!     PL2");
            }
        }
    }
}

#if defined(DEBUG)
static void configure_slow_timer() {
    if (test_flag) {
        return;
    }

    if (mpp_w.sysStop) {
        sysSLOW = 1;

        switch (io_w.data[1].sw_new) {
        case SWK_LEFT_STICK:
            mpp_w.sysStop = false;
            // fallthrough

        case SWK_LEFT_SHOULDER:
            Slow_Timer = 1;
            break;

        default:
            switch (io_w.data[1].sw & (SWK_LEFT_SHOULDER | SWK_LEFT_TRIGGER)) {
            case SWK_LEFT_SHOULDER | SWK_LEFT_TRIGGER:
                if ((sysFF = Debug_w[1]) == 0) {
                    sysFF = 1;
                }

                sysSLOW = 1;
                Slow_Timer = 1;

                break;

            case SWK_LEFT_TRIGGER:
                if (Slow_Timer == 0) {
                    if ((Slow_Timer = Debug_w[0]) == 0) {
                        Slow_Timer = 1;
                    }

                    sysFF = 1;
                }

                break;

            default:
                Slow_Timer = 2;
                break;
            }

            break;
        }
    } else if (io_w.data[1].sw_new & SWK_LEFT_STICK) {
        mpp_w.sysStop = true;
    }
}
#endif

#if ENABLE_PERF_TELEMETRY
/* Task #141 -- boot-stall attribution inside game_step_0().
 *
 * sdl_app.c measures update_ns from SDLApp_BeginFrame() to SDLApp_EndFrame()
 * (src/port/sdl/sdl_app.c:3413), and the only thing between those two calls
 * is game_step_0() (below, called from MAIN_PHASE_INITIALIZED). So a
 * "FRAME OUTLIER: ... update=408.3" line means 408 ms was spent somewhere in
 * this function, and nothing in the existing telemetry says where. These
 * checkpoints say where.
 *
 * Cost on a normal frame: one SDL_GetTicksNS() per phase plus one compare at
 * the end. Nothing is reordered, skipped or made conditional -- every call
 * runs in exactly the order it ran before, so this cannot move the
 * simulation. The report fires only above the same 50 ms threshold
 * sdl_app.c's FRAME OUTLIER line uses, so a healthy frame logs nothing. */
enum Step0Phase {
    STEP0_PHASE_AFS = 0,
    STEP0_PHASE_INPUT,
    STEP0_PHASE_NAV,
    STEP0_PHASE_ENGINE,
    STEP0_PHASE_SEQS,
    STEP0_PHASE_NETPLAY,
    STEP0_PHASE_PROBES,
    STEP0_PHASE_TRACE,
    STEP0_PHASE_EFFECT,
    STEP0_PHASE_FLIP,
    STEP0_PHASE_COUNT
};

static Uint64 step0_phase_ns[STEP0_PHASE_COUNT];
static Uint64 step0_phase_mark_ns;
static Uint64 step0_start_ns;

static void step0_phase_begin(void) {
    SDL_memset(step0_phase_ns, 0, sizeof(step0_phase_ns));
    step0_start_ns = SDL_GetTicksNS();
    step0_phase_mark_ns = step0_start_ns;
    AFS_ResetSyncReadLedger();
}

static void step0_phase_end(enum Step0Phase phase) {
    const Uint64 now_ns = SDL_GetTicksNS();

    step0_phase_ns[phase] += now_ns - step0_phase_mark_ns;
    step0_phase_mark_ns = now_ns;
}

static void step0_phase_report(void) {
    const Uint64 total_ns = SDL_GetTicksNS() - step0_start_ns;

    if (total_ns <= 50 * SDL_NS_PER_MS) {
        return;
    }

    unsigned long long sync_read_ns = 0;
    unsigned long long sync_read_bytes = 0;
    unsigned int sync_read_count = 0;

    AFS_GetSyncReadLedger(&sync_read_ns, &sync_read_bytes, &sync_read_count);

    /* One line, printed immediately before sdl_app.c's own FRAME OUTLIER
     * line for the same frame, so the two land adjacent in a captured log
     * and the frame ordinal on the second labels the first. */
    SDL_Log("[step0] total=%.1fms afs=%.1f input=%.1f nav=%.1f engine=%.1f seqs=%.1f netplay=%.1f probes=%.1f "
            "trace=%.1f effect=%.1f flip=%.1f | syncread=%.1fms n=%u bytes=%llu | G_No=%d/%d/%d/%d "
            "E_No=%d/%d/%d/%d menu_cond=%d menu_r_no=%d/%d/%d/%d Play_Mode=%d Mode_Type=%d",
            (double)total_ns / 1e6,
            (double)step0_phase_ns[STEP0_PHASE_AFS] / 1e6,
            (double)step0_phase_ns[STEP0_PHASE_INPUT] / 1e6,
            (double)step0_phase_ns[STEP0_PHASE_NAV] / 1e6,
            (double)step0_phase_ns[STEP0_PHASE_ENGINE] / 1e6,
            (double)step0_phase_ns[STEP0_PHASE_SEQS] / 1e6,
            (double)step0_phase_ns[STEP0_PHASE_NETPLAY] / 1e6,
            (double)step0_phase_ns[STEP0_PHASE_PROBES] / 1e6,
            (double)step0_phase_ns[STEP0_PHASE_TRACE] / 1e6,
            (double)step0_phase_ns[STEP0_PHASE_EFFECT] / 1e6,
            (double)step0_phase_ns[STEP0_PHASE_FLIP] / 1e6,
            (double)sync_read_ns / 1e6,
            sync_read_count,
            sync_read_bytes,
            G_No[0],
            G_No[1],
            G_No[2],
            G_No[3],
            E_No[0],
            E_No[1],
            E_No[2],
            E_No[3],
            task[TASK_MENU].condition,
            task[TASK_MENU].r_no[0],
            task[TASK_MENU].r_no[1],
            task[TASK_MENU].r_no[2],
            task[TASK_MENU].r_no[3],
            (int)Play_Mode,
            (int)Mode_Type);
}
#else
#define step0_phase_begin() ((void)0)
#define step0_phase_end(phase) ((void)0)
#define step0_phase_report() ((void)0)
#endif

static void game_step_0() {
    step0_phase_begin();
    AFS_RunServer();
    step0_phase_end(STEP0_PHASE_AFS);

    /* Reset the engine's per-frame "active hitbox" capture flag before
     * the engine tick runs. set_jugde_area() will set it during the
     * tick if cg_ja.atix != 0 for either player. */
    fd_engine_hitbox_active[0] = 0;
    fd_engine_hitbox_active[1] = 0;
    /* CONTACT-2 Step 1 diagnostics (design.md §1.3 G4): same reset contract
     * as fd_engine_hitbox_active above — zero before njUserMain() runs so
     * check_leap_attack() (pls03.c) can set it fresh this frame only. */
    fd_engine_move_is_uoh[0] = 0;
    fd_engine_move_is_uoh[1] = 0;

#if ENABLE_PERF_TELEMETRY
    /* Per-frame reset for the perf-overlay diagnostic counters (report §4)
       whose producers accumulate within this frame: njdp2d prim peak/drops
       (drained twice per frame) and the training-overlay submit timer (0 on
       frames where the training task doesn't run). */
    Njdp2d_ResetPerf();
    Training_SetPerfDispNs(0);
#endif

    flSetRenderState(FLRENDER_BACKCOLOR, 0xFF000000);

#if defined(DEBUG)
    if (Debug_w[0x43]) {
        flSetRenderState(FLRENDER_BACKCOLOR, 0xFF0000FF);
    }
#endif

    appSetupTempPriority();
    flPADGetALL();
    keyConvert();
    step0_phase_end(STEP0_PHASE_INPUT);

#if defined(DEBUG)
    if (configuration.test.enabled) {
        TestRunner_Prologue();
    }

    configure_slow_timer();
#endif

#if defined(STATCHECK)
    /* A3b: statcheck replay injection. Runs after keyConvert() (so it
     * replaces whatever the real pads wrote to p*sw_buff this frame) and
     * before the p1sw_buff -> p1sw_0 latch below — the same slot the DEBUG
     * TestRunner_Prologue occupies. DEBUG and STATCHECK cannot be
     * co-compiled (CMake hard-errors on Debug + THREESX_STATCHECK), so the
     * two hooks can never both run. */
    StatcheckRunner_Prologue();
#endif

    /* Quick Training (OSD T[15] -> SIGRTMIN+5 -> QuickTraining_Request):
     * drives the wipe-out / teardown / scene-jump / wipe-in sequence.
     * Runs BEFORE the p*sw_buff latch below because it owns the pads
     * while a sequence is active (suppresses real presses and injects
     * its own — the same slot contract as NetplayNav_Tick underneath).
     * Mutually exclusive with nav/replay/shuffle by its request gating
     * (src/quick_training.c -> qt_refusal). No-op when idle. */
    QuickTraining_Tick();

    /* Drive cold-launch menu navigation for netplay BEFORE p1sw_buff is
     * latched. The nav state machine may inject SWK_START on this tick;
     * if it does the rising-edge comparison ~p*sw_1 & p*sw_0 & SWK_START
     * in Ck_Coin() / Entry_01() / Mode_Select() needs our injected bit
     * to be present in p*sw_0 (the "current" snapshot). */
    NetplayNav_Tick();

    /* Step C1 (docs/plan-fcade-replay-browser.md): .3sr replay playback.
     * Must run BEFORE the latch below so the injected SWK-layout words land
     * in p*sw_0 this frame; placed after NetplayNav_Tick so a (mutually
     * exclusive — args.c + runtime session guard) replay session owns the
     * final word on the buffers. Inert without --play-replay. */
    /* The weekly-best shuffle viewer. Runs BEFORE ReplayPlayer_Tick, not
     * after it where ReplayBrowser_Tick used to sit: its hold-to-skip gesture
     * has to read the REAL pads keyConvert() wrote this frame, and
     * ReplayPlayer_Tick overwrites p1sw_buff/p2sw_buff with the injected
     * words (and zeroes them once terminal). Unlike the browser it does not
     * consume the pads — the player overwrites them a few lines later anyway.
     * Inert without --watch-replays. */
    ReplayShuffle_Tick();

    ReplayPlayer_Tick();

    /* When the replay player is holding the frame, skip the input latch and
     * the engine tick below, keeping p*sw_0/p*sw_1 and all engine state
     * exactly as the last injected tick left them. Rendering still runs (the
     * netplay-stall precedent: game_step_1's Scrn_Renew etc. run on frames
     * whose engine tick was skipped). The player holds every frame once
     * playback reaches a terminal state, which is what stops the game's
     * post-match flow free-running into the qix effect trap. Always false
     * without --play-replay. */
    const bool replay_frame_hold = ReplayPlayer_IsStallingThisFrame();

    /* Advance the replay viewer's private screen cover (the 76-band diagonal
     * wipe that hides the title/menu/character-select walk between replays).
     * Here, not in a draw branch: it must see whether the frame is HELD, and
     * it must run BEFORE njUserMain so its character-select reveal is decided
     * against the S_No the previous frame left — which is the frame the
     * engine's own WipeIn(0) fully covers. Inert without a loaded replay. */
    ReplayWipe_Tick(!replay_frame_hold);

    if (!replay_frame_hold && ((Play_Mode != 3 && Play_Mode != 1) || (Game_pause != 0x81))) {
        p1sw_1 = p1sw_0;
        p2sw_1 = p2sw_0;
        p3sw_1 = p3sw_0;
        p4sw_1 = p4sw_0;
        p1sw_0 = p1sw_buff;
        p2sw_0 = p2sw_buff;
        p3sw_0 = p3sw_buff;
        p4sw_0 = p4sw_buff;

        if ((task[TASK_MENU].condition == 1) && Is_Training_Mode(Mode_Type) && (Play_Mode == 1)) {
            const u16 sw_buff = p2sw_0;
            p2sw_0 = p1sw_0;
            p1sw_0 = sw_buff;
        }
    }

    appCopyKeyData();
    step0_phase_end(STEP0_PHASE_NAV);

    mpp_w.inGame = false;

    if (Netplay_GetSessionState() != NETPLAY_SESSION_IDLE) {
        Netplay_Run();
        step0_phase_end(STEP0_PHASE_ENGINE);
        // Flush the 2D polygon buffer each frame when the game's normal render
        // loop isn't running, preventing the NJDP2D_PRIM_MAX limit from overflowing.
        njdp2d_draw();
        step0_phase_end(STEP0_PHASE_SEQS);
        /* S1 host liveness (docs/plan-netplay-connection.md): keep the
         * orchestrator ticking during the active session so the UPnP
         * lease renewal (half of the 1-hour lease) fires mid-session —
         * the mapping is what carries the peer's traffic. In HANDOFF
         * state this is one atomic state read + one bool check per
         * frame; renewal itself runs on a side thread. */
        DirectP2P_Tick();
        step0_phase_end(STEP0_PHASE_NETPLAY);
    } else if (replay_frame_hold) {
        /* Engine tick held. Draw the replay overlay and flush the 2D buffer,
         * mirroring the netplay-stall branch above. Skipping njUserMain here
         * means the frame carries NO game geometry: SoftwareRenderer_RenderFrame
         * clears the canvas to opaque black every frame and draws only the
         * quads submitted since the last one, so a held frame is black plus
         * whatever these two Draw calls put on it. That is the intended
         * "freeze + message" card, not a retained freeze-frame — see
         * replay_player.c's s_stall_frame comment.
         *
         * The three phase_end calls are not decoration: step0_phase_end()
         * accumulates (now - mark) into a phase and advances the mark, so a
         * branch that closes nothing donates its whole elapsed time to
         * whichever phase closes next. Upstream instruments every other
         * branch of this chain; matching it here keeps the telemetry
         * flavor's per-phase breakdown honest on held frames. ENGINE and
         * NETPLAY close at ~0 because neither runs. */
        step0_phase_end(STEP0_PHASE_ENGINE);
        ReplayOverlay_Draw();
        /* MUST be here as well as in the normal branch below: every
         * inter-replay transition happens on HELD frames, so anything the
         * shuffle viewer wants on screen between replays is only ever drawn
         * from this branch. */
        ReplayShuffle_Draw();
        njdp2d_draw();
        step0_phase_end(STEP0_PHASE_SEQS);
        step0_phase_end(STEP0_PHASE_NETPLAY);
    } else {
        njUserMain();
        step0_phase_end(STEP0_PHASE_ENGINE);
        seqsBeforeProcess();
        /* Step C2 (docs/plan-fcade-replay-browser.md): draw the .3sr replay
         * viewer overlay (status line / hold-START-to-exit hint / terminal
         * message) into the 2D sprite list before njdp2d_draw() flushes it.
         * Read-only over the player state — inert without --play-replay. */
        ReplayOverlay_Draw();
        /* Shuffle-viewer chrome (skip hint). Inert without --watch-replays. */
        ReplayShuffle_Draw();
        /* The viewer's private cover, LAST and in front of everything (z =
         * 0.0f, just ahead of PrioBase[0]). Live frames only: a held frame is
         * already black and still has to show its "REPLAY COMPLETE" /
         * "NEXT REPLAY..." card, which an opaque cover would hide. */
        ReplayWipe_Draw();
        njdp2d_draw();
        seqsAfterProcess();
        step0_phase_end(STEP0_PHASE_SEQS);
        Netplay_TickDirectP2P();
#if defined(ENABLE_NETPLAY)
        defer_direct_p2p_handoff_tick();
#endif
        DirectP2P_Tick();
        step0_phase_end(STEP0_PHASE_NETPLAY);
    }

    /* Freeze-boundary probe (fit.md §5), env-gated on FD_SPAWN_PROBE.
     * MUST run here — after the engine tick (njUserMain, which runs
     * effect_13_init and sets fd_engine_proj_spawned) and before
     * frame_data_overlay_tick(), whose consume sites clear the flag. See
     * frame_spawn_probe_tick()'s definition comment for why pre-consume
     * sampling is required to place the 0->1 transition unambiguously. */
    frame_spawn_probe_tick();

    frame_data_overlay_tick();

    /* FD_IDLE_PROBE (diagnostic, env-gated): per-tick idle ledger. MUST run
     * AFTER frame_data_overlay_tick() so each line reports post-engine,
     * post-overlay-latch state. Observation only; inert unless FD_IDLE_PROBE
     * is set (and, like the trace, only in training + overlay-enabled). */
    fd_idle_probe_tick();
    step0_phase_end(STEP0_PHASE_PROBES);
#if ENABLE_PERF_TELEMETRY
    {
        const Uint64 _ft0 = SDL_GetTicksNS();
        frame_trace_tick();
        FrameTrace_SetPerfTickNs(SDL_GetTicksNS() - _ft0);
    }
#else
    frame_trace_tick();
#endif

    step0_phase_end(STEP0_PHASE_TRACE);

    disp_effect_work();
    step0_phase_end(STEP0_PHASE_EFFECT);
    flFlip(0);
    step0_phase_end(STEP0_PHASE_FLIP);
    step0_phase_report();
}

static void game_step_1() {
#if defined(STATCHECK)
    /* A3b: compare engine state against the archived frame. Placed at the
     * TOP of game_step_1 — not beside the DEBUG epilogue at the bottom —
     * because upstream runs TestRunner_Epilogue between Main_StepFrame and
     * Main_FinishFrame (upstream sdl_headless_app.c:59-72), i.e. after
     * njUserMain but before Interrupt_Timer/Scrn_Renew/Irl_Family/Irl_Scrn/
     * BGM_Server mutate more state. The archived CPS3 dumps were taken at
     * that same boundary; comparing after Scrn_Renew would misalign the
     * oracle. */
    StatcheckRunner_Epilogue();
#endif

    /* Step C1: sample the .3sr divergence checksums at the same boundary
     * the statcheck oracle compares at (see the STATCHECK comment above —
     * the SCRD frames the .3sr checksums derive from were captured after
     * njUserMain and before Interrupt_Timer/Scrn_Renew). Inert without
     * --play-replay. */
    ReplayPlayer_Epilogue();

    Interrupt_Timer += 1;
    Record_Timer += 1;

    Scrn_Renew();
    Irl_Family();
    Irl_Scrn();
    BGM_Server();

#if defined(DEBUG)
    if (configuration.test.enabled) {
        TestRunner_Epilogue();
    }
#endif
}

static bool sdl_poll_helper() {
    SDL_Event event;
    bool continue_running = true;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            continue_running = false;
        }
    }

    return continue_running;
}

static void handle_signal_requests() {
    if (fps_toggle_requested != 0) {
        fps_toggle_requested = 0;
        SDLApp_ToggleFPSOverlay();
    }
    if (arm_clock_cycle_requested != 0) {
        arm_clock_cycle_requested = 0;
        SDLApp_CycleArmClock();
    }
    if (game_mode_cycle_requested != 0) {
        game_mode_cycle_requested = 0;
        SDLApp_CycleGameMode();
    }
    if (hold_to_pause_cycle_requested != 0) {
        hold_to_pause_cycle_requested = 0;
        SDLApp_CycleHoldToPause();
    }
    if (quick_training_requested != 0) {
        quick_training_requested = 0;
        QuickTraining_Request();
    }
}

static int loop() {
    bool is_running = true;
    bool console_mode_entered = false;
    bool shutdown_handlers_installed = false;
#if ENABLE_PERF_TELEMETRY
    bool perf_capture_started = false;
    int perf_wait_warmup_remaining = configuration.perf.gameplay_warmup_frames;
#endif
    int exit_code = 0;

    shutdown_signal = 0;
    fps_toggle_requested = 0;
    arm_clock_cycle_requested = 0;
    game_mode_cycle_requested = 0;
    quick_training_requested = 0;

    while (is_running && shutdown_signal == 0) {
        switch (phase) {
        case MAIN_PHASE_INIT:
            if (Resources_Check()) {
                /* Resources verified: enter console mode and initialize.
                   ConsoleMode switches the VT to KD_GRAPHICS (black screen).
                   Doing this AFTER Resources_Check means the SHA256 hash runs
                   while the screen is still visible, avoiding a long unexplained
                   black screen before the game starts. */
                console_mode_entered = ConsoleMode_Enter();
#if defined(PORT_MISTER) && defined(__linux__)
                if (!console_mode_entered) {
                    fprintf(stderr,
                            "Failed to acquire Linux console (KD_GRAPHICS). "
                            "Run from MiSTer OSD/local console, not over SSH.\n");
                    exit(1);
                }
#endif
                install_shutdown_signal_handlers();
                shutdown_handlers_installed = true;
                SDLApp_PreInit();
                initialize_game();
                phase = MAIN_PHASE_INITIALIZED;

#if ENABLE_PERF_TELEMETRY
                if (configuration.perf.frame_count > 0 && !configuration.perf.wait_for_gameplay &&
                    configuration.perf.wait_for_test_phase == NULL &&
                    configuration.perf.wait_for_runtime_state == NULL) {
                    SDLApp_ConfigurePerfCapture(configuration.perf.frame_count,
                                                configuration.perf.output_path,
                                                configuration.perf.scene,
                                                configuration.perf.basic_mode,
                                                configuration.perf.basic_first_window_family_snapshots,
                                                configuration.perf.basic_first_window_render_subphases,
                                                configuration.perf.basic_first_window_exact_hot_family_alpha_offpath,
                                                configuration.perf.basic_first_window_onset_exact_hot_family_alpha_offpath,
                                                configuration.perf.basic_first_window_onset_cluster_alpha_offpath,
                                                configuration.perf.fast_non_integer_disable_reuse_telemetry,
                                                configuration.perf.fast_non_integer_enable_subrect_alpha_telemetry);
                    perf_capture_started = true;
                }
#endif
            } else {
                phase = MAIN_PHASE_COPYING_RESOURCES;
            }

            break;

        case MAIN_PHASE_COPYING_RESOURCES:
            is_running = sdl_poll_helper();

            if (!is_running) {
                break;
            }

            SDL_Delay(16);

            const bool resource_flow_ended = Resources_RunResourceCopyingFlow();

            if (resource_flow_ended) {
                initialize_game();
                phase = MAIN_PHASE_INITIALIZED;
            }

            break;

        case MAIN_PHASE_INITIALIZED:
            handle_signal_requests();

            is_running = SDLApp_PollEvents();

            if (!is_running) {
                break;
            }

            /* Rollback-determinism harness (inert without --rbd-capture):
             * PreFrame may inject a save/resim/load rollback cycle before
             * the frame; FrameEnd hashes the writable image after it. Both
             * are stubs outside DEBUG+ENABLE_NETPLAY builds. */
            RollbackDeterminism_PreFrame();

            SDLApp_BeginFrame();
            game_step_0();
            SDLApp_EndFrame();
            game_step_1();

            RollbackDeterminism_FrameEnd();
            LdreqTimingTrace_FrameEnd();

#if ENABLE_PERF_TELEMETRY
            if (!perf_capture_started && configuration.perf.frame_count > 0 &&
                (configuration.perf.wait_for_gameplay || configuration.perf.wait_for_test_phase != NULL ||
                 configuration.perf.wait_for_runtime_state != NULL)) {
                const bool wait_condition_met =
                    configuration.perf.wait_for_gameplay
                        ? mpp_w.inGame
                        : ((configuration.perf.wait_for_test_phase != NULL)
                               ? TestRunner_IsPhaseActive(configuration.perf.wait_for_test_phase)
                               : SDLApp_IsPerfRuntimeStateActive(configuration.perf.wait_for_runtime_state));

                if (wait_condition_met) {
                    if (perf_wait_warmup_remaining > 0) {
                        perf_wait_warmup_remaining -= 1;
                    } else {
                        const int perf_stage_id = mpp_w.inGame ? bg_w.stage : -1;
                        const int perf_p1_character = mpp_w.inGame ? My_char[0] : -1;
                        const int perf_p2_character = mpp_w.inGame ? My_char[1] : -1;
                        const int perf_p1_super_art = mpp_w.inGame ? Super_Arts[0] : -1;
                        const int perf_p2_super_art = mpp_w.inGame ? Super_Arts[1] : -1;
                        SDL_Log("PERF capture start: in_game=%d warmup_frames=%d scene=%s detail_mode=%s stage_id=%d "
                                "test_stage_override=%d test_scene_preset=%s p1_character=%d p2_character=%d "
                                "p1_super_art=%d p2_super_art=%d test_phase=%s wait_test_phase=%s wait_runtime_state=%s "
                                "fast_non_integer_reuse_telemetry=%s basic_first_window_family_snapshots=%s "
                                "basic_first_window_render_subphases=%s "
                                "basic_first_window_exact_hot_family_alpha_offpath=%s "
                                "basic_first_window_onset_exact_hot_family_alpha_offpath=%s "
                                "basic_first_window_onset_cluster_alpha_offpath=%s "
                                "fast_non_integer_subrect_alpha_telemetry=%s "
                                "g_no=%d/%d/%d/%d e_no=%d/%d/%d/%d menu_task_condition=%d menu_r_no=%d/%d/%d/%d "
                                "break_into=%d hnc_num=%d exec_wipe=%d active_wipe_type=%d wipe_limit=%d",
                                mpp_w.inGame ? 1 : 0,
                                configuration.perf.gameplay_warmup_frames,
                                configuration.perf.scene != NULL ? configuration.perf.scene : "(none)",
                                configuration.perf.basic_mode
                                    ? (configuration.perf.basic_first_window_family_snapshots
                                           ? "basic-first-window-families"
                                           : "basic")
                                    : "full",
                                perf_stage_id,
                                configuration.test.stage,
                                configuration.test.scene_preset != NULL ? configuration.test.scene_preset : "(none)",
                                perf_p1_character,
                                perf_p2_character,
                                perf_p1_super_art,
                                perf_p2_super_art,
                                TestRunner_GetPhaseName(),
                                configuration.perf.wait_for_test_phase != NULL ? configuration.perf.wait_for_test_phase
                                                                               : "(none)",
                                configuration.perf.wait_for_runtime_state != NULL ? configuration.perf.wait_for_runtime_state
                                                                                 : "(none)",
                                !configuration.perf.fast_non_integer_disable_reuse_telemetry
                                    ? "on"
                                    : "off",
                                (configuration.perf.basic_mode &&
                                 configuration.perf.basic_first_window_family_snapshots)
                                    ? "on"
                                    : "off",
                                (configuration.perf.basic_mode &&
                                 configuration.perf.basic_first_window_render_subphases)
                                    ? "on"
                                    : "off",
                                (configuration.perf.basic_mode &&
                                 configuration.perf.basic_first_window_exact_hot_family_alpha_offpath)
                                    ? "on"
                                    : "off",
                                (configuration.perf.basic_mode &&
                                 configuration.perf.basic_first_window_onset_exact_hot_family_alpha_offpath)
                                    ? "on"
                                    : "off",
                                (configuration.perf.basic_mode &&
                                 configuration.perf.basic_first_window_onset_cluster_alpha_offpath)
                                    ? "on"
                                    : "off",
                                (!configuration.perf.basic_mode &&
                                 configuration.perf.fast_non_integer_enable_subrect_alpha_telemetry)
                                    ? "on"
                                    : "off",
                                G_No[0],
                                G_No[1],
                                G_No[2],
                                G_No[3],
                                E_No[0],
                                E_No[1],
                                E_No[2],
                                E_No[3],
                                task[TASK_MENU].condition,
                                task[TASK_MENU].r_no[0],
                                task[TASK_MENU].r_no[1],
                                task[TASK_MENU].r_no[2],
                                task[TASK_MENU].r_no[3],
                                Break_Into,
                                Hnc_Num,
                                Exec_Wipe,
                                Active_Wipe_Type,
                                WipeLimit);
                        SDLApp_ConfigurePerfCapture(configuration.perf.frame_count,
                                                    configuration.perf.output_path,
                                                    configuration.perf.scene,
                                                    configuration.perf.basic_mode,
                                                    configuration.perf.basic_first_window_family_snapshots,
                                                    configuration.perf.basic_first_window_render_subphases,
                                                    configuration.perf.basic_first_window_exact_hot_family_alpha_offpath,
                                                    configuration.perf.basic_first_window_onset_exact_hot_family_alpha_offpath,
                                                    configuration.perf.basic_first_window_onset_cluster_alpha_offpath,
                                                    configuration.perf.fast_non_integer_disable_reuse_telemetry,
                                                    configuration.perf.fast_non_integer_enable_subrect_alpha_telemetry);
                        perf_capture_started = true;
                    }
                } else {
                    perf_wait_warmup_remaining = configuration.perf.gameplay_warmup_frames;
                }
            }
#endif
            break;
        }
    }

    // Tier-1 netplay diag — Item 10: SIGTERM flush hook. Runs after the
    // main loop exits but before cleanup() (which tears SDL down). If a
    // netplay session was active we dump the packet ring, capture a final
    // /proc/net/snmp UDP-row delta, and close the per-session log file.
    // No-op when no session was active.
#if defined(ENABLE_NETPLAY)
    Netplay_FlushDiagnostics();
#endif

    cleanup();

    if (shutdown_handlers_installed) {
        restore_shutdown_signal_handlers();
    }
    if (console_mode_entered) {
        ConsoleMode_Exit();
    }

    if (shutdown_signal != 0) {
        exit_code = 128 + shutdown_signal;
    }

    return exit_code;
}

// Only defined when ENABLE_NETPLAY is on; otherwise the CLI flag prints a
// diagnostic and exits.
#ifdef ENABLE_NETPLAY
// Phase 6 Step 8: forward-decl of the MIST handshake test harness
// (src/netplay/test_mist_handshake.c). Same gating as above.
int Netplay_Test_MistHandshake(void);
// STUN direct P2P Step 2 (docs/plan-stun-direct-p2p.md): forward-decl
// of the room-code codec test harness (src/netplay/test_room_code.c).
// Same gating pattern as the other Phase 6 tests — ENABLE_NETPLAY gates
// TU inclusion, ENABLE_NETPLAY_TESTS inside the TU gates the real body.
int Netplay_Test_RoomCode(void);
// Task #119: forward-decl of the late-punch rescue-layer unit harness
// (src/netplay/test_late_punch.c). Pins the accept/reject/relearn
// decision that the netns proof cannot see. Same gating pattern.
int Netplay_Test_LatePunch(void);
// STUN direct P2P Step 12 (docs/plan-stun-direct-p2p.md): forward-decl
// of the STUN mock-server test harness (src/netplay/test_stun_mock.c).
// Spawns a localhost UDP listener that echoes a crafted Binding Response
// with XOR-MAPPED-ADDRESS, then verifies the client parses it correctly.
// Also round-trips Stun_EncodeEndpoint / Stun_DecodeEndpoint.
int Netplay_Test_StunMock(void);
// perf(netplay) Option A: forward-decl of the sparse effect-pool save
// round-trip parity test harness (src/netplay/test_sparse_effect_save.c).
// Same gating pattern as the other Phase 6 tests.
int Netplay_Test_SparseEffectSave(void);
// Step 6 of docs/plan-bilateral-hole-punch.md: forward-decl of the
// bilateral hole-punch protocol test harness
// (src/netplay/test_bilateral_punch.c). Same gating pattern as the
// other Phase 6 tests. Spawns a localhost UDP rendezvous mock and
// exercises the REGISTER/POLL/DELIVER round-trip plus the LAN-bypass
// table; no external network dep.
int Netplay_Test_BilateralPunch(void);
// Task #132 P3: forward-decl of the fast netplay unit harness
// (src/netplay/test_netplay_units.c). Same gating pattern. No sockets, no
// threads, no sleeps — it is the half of test_bilateral_punch.c that
// never needed any of those, moved somewhere it can be run constantly.
int Netplay_Test_NetplayUnits(void);
// #36: forward-decl of the connect-observability proof harness
// (src/netplay/test_connect_observability.c). Same gating pattern as the
// other Phase 6 tests. Induces a rendezvous protocol-version skew and a
// silent rendezvous server on loopback UDP, then reads the per-session
// netplay log file back off disk to prove the attribution evidence
// reaches it; also hammers the thread-safe log sink from 4 threads.
int Netplay_Test_ConnectObservability(void);
// M-3 coverage guard: forward-decl of the GameState save/load
// field-coverage harness (src/netplay/test_gs_coverage.c). Randomized
// load->save round-trip over the whole GameState; any struct byte that
// GS_SAVE/GS_LOAD misses fails loudly with its exact offset. Same
// gating pattern as the other harnesses.
int Netplay_Test_GsCoverage(void);
// Task #122: forward-decl of the rendezvous wire-codec unit harness
// (src/netplay/test_rendezvous_wire.c). Same gating pattern as the other
// Phase 6 tests. Pure: no sockets, no threads, no NETPLAY_TEST_HOOKS —
// it pins Rendezvous_ParseNack (which shipped with zero callers) and the
// reason -> ConnectFailCode verdict mapping.
int Netplay_Test_RendezvousWire(void);
// Task #132: forward-decl of the MIST compat-gate unit harness
// (src/netplay/test_mist_compat_gate.c). Same gating pattern as the other
// Phase 6 tests. Pure: no sockets, no threads, no NETPLAY_TEST_HOOKS — it
// drives classify_peer_payload/parse_header/read_* directly, which is the
// decision that says whether a peer is allowed to play with us.
int Netplay_Test_MistCompatGate(void);
// Task #132: forward-decl of the punch-predicate unit harness
// (src/netplay/test_punch_predicates.c). Pure: no sockets, no threads, no
// NETPLAY_TEST_HOOKS — the STUN/punch classifiers swept bit by bit, plus
// the three late-punch decisions the socket harness cannot observe.
int Netplay_Test_PunchPredicates(void);
#endif

/* Tasks #59/#61: forward-decl of the ext texture-cache brick-prevention
 * harness (src/test/test_texcash_bounds.c). Outside the ENABLE_NETPLAY block
 * on purpose -- it exercises mtrans.c/texcash.c, not netplay, so the TU is
 * always compiled and gates its own body on ENABLE_NETPLAY_TESTS. */
int Texcash_Test_Bounds(void);

/* Doc item Q (§8.Q/§21): forward-decl of the cg_se sound-code remap unit
 * harness (src/test/test_cg_se_remap.c). Outside the ENABLE_NETPLAY block on
 * purpose -- it exercises src/arcade/arcade_char_data.c, not netplay, so the
 * TU is always compiled and gates its own body on ENABLE_NETPLAY_TESTS. */
int CgSe_Test_Remap(void);

/* "First light" scaffolding (docs/research-arcade-cg-data-accuracy.md,
 * 3sx-rom-only-research.md §5S 4.2): forward-decl of the ported CPS-3
 * char-DMA decoder unit harness (src/test/test_cps3_chardma.c). Outside
 * the ENABLE_NETPLAY block on purpose -- it exercises
 * src/arcade/cps3_first_light.c, not netplay, so the TU is always
 * compiled and gates its own body on ENABLE_NETPLAY_TESTS. */
int Cps3Chardma_Test_Decode(void);

/* Test harnesses run unattended (scripts, CI). SDL's DEFAULT assertion
 * handler shows an interactive Retry/Break/Abort/Ignore prompt in Debug
 * builds, which never returns in a non-interactive session — a tripped
 * SDL_assert anywhere under a harness becomes an infinite hang that a
 * wrapper losing the exit code can mistake for a pass. Install an
 * abort-on-assert handler before dispatching ANY harness so every
 * assert terminates loudly (SIGABRT) with the condition on stderr, with
 * no SDL_ASSERT environment override needed. Interactive game runs keep
 * SDL's default handler. */
static SDL_AssertState SDLCALL test_harness_assert_handler(const SDL_AssertData* data, void* userdata) {
    (void)userdata;
    fprintf(stderr,
            "[test-harness] SDL assertion failed: '%s' at %s:%d (%s) — aborting "
            "(non-interactive harness assert policy, src/main.c)\n",
            data->condition, data->filename, data->linenum, data->function);
    return SDL_ASSERTION_ABORT;
}

int main(int argc, const char* argv[]) {
    read_args(argc, argv, &configuration);

    /* Loader-timing invariance instrument (task #66, src/test/
     * ldreq_timing_trace.h). Both are no-ops at their default values, so
     * a normal launch leaves the barrier gated on the live GekkoNet
     * session state and the AFS path untouched. */
    Ldreq_SetBarrierForced(configuration.test.ldreq_barrier_force);
    AFS_SetInjectedLatencyMs(configuration.test.afs_inject_latency_ms);

    if (configuration.test_mist_handshake ||
        configuration.test_room_code || configuration.test_late_punch ||
        configuration.test_stun_mock ||
        configuration.test_sparse_effect_save || configuration.test_bilateral_punch ||
        configuration.test_connect_observability ||
        configuration.test_gs_coverage || configuration.test_rendezvous_wire) {
        SDL_SetAssertionHandler(test_harness_assert_handler, NULL);
    }

    /* Stash the shuffle viewer's CLI state before any tick (the slot
     * ReplayBrowser_Configure used to occupy). The `replays-root` config key
     * is read later, after Config_Init in SDLApp_FullInit. */
    ReplayShuffle_Configure(configuration.replay.watch_replays, configuration.replay.watch_replays_root);

#if defined(STATCHECK)
    /* Stage A3b of docs/plan-fcade-replay-browser.md — statcheck harness
     * init (replaces A3a's parse-and-exit smoke). Open + parse the SCRD
     * archive up front so an invalid archive dies cleanly before any game
     * init; the runner then drives the whole session from the
     * StatcheckRunner_Prologue/Epilogue hooks in game_step_0/game_step_1
     * and exits with the verdict (0 = full-game RAM match, 1 = first
     * mismatch). SDL IO does not require SDL_Init, so this runs safely
     * pre-init. */
    {
        const ScrdGameInitResult init_result = StatcheckRunner_Init(configuration.statcheck.ram_archive_path);

        /* Exit 2, not 1, when the segment simply holds no match (H1,
         * docs/research-arcade-balance-desyncs.md). 1 means "the engine
         * diverged from CPS3" and is what a sweep turns into a worklist item;
         * a segment the runner cut entirely out of a post-KO tail has nothing
         * to diverge from, and reporting it as 1 was the H1 false positive.
         * Callers that gate publication on rc == 0 are unaffected. */
        if (init_result == SCRD_GAME_INIT_NO_MATCH_START) {
            printf("statcheck: NO-MATCH — archive '%s' contains no match start; nothing to compare\n",
                   configuration.statcheck.ram_archive_path);
            return 2;
        }

        /* Exit 3, not 1, when the recording had a CPU player (H4b,
         * docs/research-arcade-balance-desyncs.md). Same rule as the H1 exit 2
         * above: 1 means "the engine diverged from CPS3", and the harness
         * forces two human operators, so a segment the cabinet ran at
         * `Play_Type == 0` is not comparable at all. A distinct code (rather
         * than reusing 2) keeps "cannot reproduce this recording" separable
         * from "nothing in this segment to reproduce" in a sweep. Callers that
         * gate publication on rc == 0 -- publish_3sr.py's statcheck_gate is
         * `clean = proc.returncode == 0` -- are unaffected. */
        if (init_result == SCRD_GAME_INIT_CPU_PLAYER) {
            printf("statcheck: CPU-PLAYER — archive '%s' was recorded against the CPU "
                   "(wu_operator = (%u, %u)); the harness forces two operators and cannot "
                   "reproduce it\n",
                   configuration.statcheck.ram_archive_path,
                   StatcheckRunner_WuOperator(0),
                   StatcheckRunner_WuOperator(1));
            return 3;
        }

        if (init_result != SCRD_GAME_INIT_OK) {
            SDL_Log("statcheck: failed to open/parse RAM archive '%s'",
                    configuration.statcheck.ram_archive_path);
            return 1;
        }
    }
#endif

    /* Step C1 (docs/plan-fcade-replay-browser.md): load + validate the .3sr
     * up front so a corrupt file dies cleanly before any game init. File IO
     * only — safe pre-SDL_Init (same rationale as the statcheck init
     * above). */
    if (configuration.replay.play_replay_path != NULL) {
        if (!ReplayPlayer_Init(configuration.replay.play_replay_path)) {
            SDL_Log("replay: failed to load '%s'", configuration.replay.play_replay_path);
            return 1;
        }
    }

    if (configuration.test_mist_handshake) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_MistHandshake();
#else
        fprintf(stderr,
                "--test-mist-handshake requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_room_code) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_RoomCode();
#else
        fprintf(stderr,
                "--test-room-code requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_late_punch) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_LatePunch();
#else
        fprintf(stderr,
                "--test-late-punch requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_stun_mock) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_StunMock();
#else
        fprintf(stderr,
                "--test-stun-mock requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_sparse_effect_save) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_SparseEffectSave();
#else
        fprintf(stderr,
                "--test-sparse-effect-save requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_netplay_units) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_NetplayUnits();
#else
        fprintf(stderr,
                "--test-netplay-units requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_bilateral_punch) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_BilateralPunch();
#else
        fprintf(stderr,
                "--test-bilateral-punch requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_connect_observability) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_ConnectObservability();
#else
        fprintf(stderr,
                "--test-connect-observability requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_gs_coverage) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_GsCoverage();
#else
        fprintf(stderr,
                "--test-gs-coverage requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_rendezvous_wire) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_RendezvousWire();
#else
        fprintf(stderr,
                "--test-rendezvous-wire requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_mist_compat_gate) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_MistCompatGate();
#else
        fprintf(stderr,
                "--test-mist-compat-gate requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_punch_predicates) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_PunchPredicates();
#else
        fprintf(stderr,
                "--test-punch-predicates requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_texcash_bounds) {
        return Texcash_Test_Bounds();
    }

    if (configuration.test_cg_se_remap) {
        return CgSe_Test_Remap();
    }

    if (configuration.test_cps3_chardma) {
        return Cps3Chardma_Test_Decode();
    }

    return loop();
}

// Tasks

void cpReadyTask(TaskID num, void (*func_adrs)(struct _TASK* task_ptr)) {
    struct _TASK* task_ptr = &task[num];

    memset(task_ptr, 0, sizeof(struct _TASK));

    task_ptr->func_adrs = func_adrs;
    task_ptr->condition = 2;
}

void cpExitTask(TaskID num) {
    SDL_zero(task[num]);
}
