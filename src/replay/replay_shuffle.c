/*
 * replay_shuffle.c — the weekly-best shuffle viewer.
 *
 * Plays the cached `.3sr` set back to back, forever, in a random order that
 * is re-drawn from a fresh entropy seed on every boot. No persistence, no
 * end state, no empty-state UI: an empty cache simply has nothing to play.
 *
 * WHY THE CHAINING LOOP LOOKS LIKE THIS. Four properties of the C1 player
 * (src/replay/replay_player.c) dictate the shape of this file; each one is a
 * real bug if violated:
 *
 *  1. ReplayPlayer_LoadAndStart() calls ReplayPlayer_Destroy() — which clears
 *     the player's internal s_stall_frame freeze — BEFORE its
 *     ReplayPlayer_Init() success check, and only reaches Soft_Reset_Sub()
 *     AFTER it. So a FAILED load leaves the engine un-frozen in the PREVIOUS
 *     replay's post-match state: exactly the free-run into the "qix is out of
 *     range" trap (effect.c push_effect_work) that the freeze machinery
 *     exists to prevent. A truncated/corrupt `.3sr` in a downloaded set is
 *     entirely plausible, so start_entry() calls Soft_Reset_Sub() itself on
 *     every failed load before moving on.
 *
 *  2. EVERY replay, the first one included, is started through
 *     ReplayPlayer_LoadAndStart(). It is the only entry point that sets the
 *     player's s_browser_owned, which makes a terminal replay FREEZE and wait
 *     to be torn down by its owner. A replay started by the `--play-replay`
 *     boot path instead calls SDLApp_Exit() 180 frames after the match ends,
 *     and the playlist would die after one replay. This is why
 *     --watch-replays and --play-replay are mutually exclusive in args.c and
 *     why this module never leans on the boot path.
 *
 *  3. The player's title -> menu -> char-select -> game walk is never
 *     short-circuited. PHASE_GAME's first act is `if (game_ended()) finish()`
 *     where game_ended() is `PL_Wins[0] == 2 || PL_Wins[1] == 2`, and
 *     PL_Wins is zeroed by Game01_Sub() — NOT by Soft_Reset_Sub(). Only the
 *     full walk clears it; skip it and replay N+1 terminates at frame 0.
 *
 *  4. The transition delay is shorter than the overlay's own message hold.
 *     ReplayOverlay_Draw's complete_hold latches to RPL_OVL_COMPLETE_HOLD
 *     (180) on the first COMPLETE frame and counts down, so after ~3 s
 *     "REPLAY COMPLETE" vanishes while the freeze continues. 90 frames (the
 *     deleted browser's RB_RETURN_LINGER_FRAMES) keeps the screen alive
 *     across the whole transition.
 *
 * WHERE THE TICK RUNS. Before ReplayPlayer_Tick(), not after it (where
 * ReplayBrowser_Tick() used to sit). ReplayPlayer_Tick reads
 * (p1sw_buff | p2sw_buff) & SWK_START for its own hold-to-exit and then
 * OVERWRITES both buffers with the injected words — and zeroes them outright
 * once terminal. The hold-to-skip gesture needs the REAL pads, so it has to
 * sample first. Unlike the browser we do NOT consume the pads: while a
 * replay is navigating or playing the player overwrites them a few lines
 * later anyway, so nothing leaks into the game underneath.
 */

#include "replay/replay_shuffle.h"

#include "replay/replay_browser_scan.h"
#include "replay/replay_player.h"

#include "main.h"
#include "netplay/netplay.h"
#include "netplay/netplay_nav.h"
#include "port/config/config.h"
#include "port/paths.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"

#include <SDL3/SDL.h>

#include <stdbool.h>

/* ---------------------------------------------------------------------- */
/* Layout + tuning                                                        */
/* ---------------------------------------------------------------------- */

#define RS_CANVAS_W 384
#define RS_ATR 9
#define RS_COL 0xFFFFFFFFu
#define RS_PRIO 1

/* y=214 is the C1 viewer's own "HOLD START TO EXIT" hint (replay_overlay.c
 * RPL_OVL_HINT_Y); the skip hint sits one row above it. y=116 is just under
 * the terminal-message row (RPL_OVL_CENTER_Y 100) so the transition line
 * reads as a second line of the same message. */
#define RS_SKIP_HINT_Y 206
#define RS_TRANSITION_Y 116

/* Frames the terminal overlay is left on screen before the next replay is
 * loaded. Mirrors the deleted browser's RB_RETURN_LINGER_FRAMES, and stays
 * under RPL_OVL_COMPLETE_HOLD (180) so "REPLAY COMPLETE" is still up. */
#define RS_TRANSITION_FRAMES 90

/* Hold-to-skip. SWK_NORTH is MP: the C1 player never reads it (it reads only
 * SWK_START, for hold-to-exit) and the deleted browser had already
 * repurposed it as a state-scoped action button, so it is the one face
 * button with no competing meaning here. Hold it RS_SKIP_HOLD_FRAMES
 * consecutive frames to jump to the next replay; hold START keeps its
 * existing meaning (leave the viewer) and is handled by the player itself. */
#define RS_SKIP_BTN SWK_NORTH
#define RS_SKIP_HOLD_FRAMES 60

/* The skip hint is a hard on/off, exactly like replay_overlay.c's
 * draw_exit_hint: visible for the first RS_HINT_INTRO_FRAMES of each replay,
 * or any time the skip button is held. It does not fade. */
#define RS_HINT_INTRO_FRAMES 300
#define RS_HINT_PIPS 8

/* Divergence diagnostics sidecar. Written under the replays root because
 * tools/mister/package.sh rotates last-run.log to last-run-prev.log on each
 * launch — exactly one prior session survives there, so a SDL_Log-only
 * record is gone after two more launches. Bounded: once the file passes
 * RS_DIAG_MAX_BYTES it is restarted from scratch with a rotation note. */
#define RS_DIAG_BASENAME "shuffle-diagnostics.log"
#define RS_DIAG_MAX_BYTES 32768

/* ---------------------------------------------------------------------- */
/* State                                                                  */
/* ---------------------------------------------------------------------- */

typedef enum RsState {
    RS_UNINIT = 0,  /* first tick not yet run */
    RS_OFF,         /* disabled (or stood down) for this session — permanent */
    RS_WAIT_BOOT,   /* enabled; waiting for the attract/title screen */
    RS_PLAYING,     /* a replay is running (navigating or playing) */
    RS_TRANSITION,  /* terminal reached; holding the message, then advancing */
    RS_EMPTY,       /* nothing playable in the cache — idle forever, no UI */
} RsState;

static RsState s_state = RS_UNINIT;

static bool s_cli_enable = false;
static const char* s_cli_root = NULL;

static RbEntry s_entries[RB_MAX_ENTRIES];
static int s_count = 0;              /* playable (non-needs_conversion) entries */
static Uint16 s_order[RB_MAX_ENTRIES]; /* shuffled permutation of [0, s_count) */
static int s_cursor = 0;             /* next slot of s_order to play */
static int s_current = -1;           /* entry index of the replay on screen */
static Uint32 s_played = 0;          /* replays started this session (1-based counter) */
static Uint32 s_shuffles = 0;        /* how many times the set has been shuffled */

static int s_transition_frames = 0;
static int s_replay_frames = 0; /* frames since the current replay was started */
static int s_skip_hold = 0;
static bool s_hud_logged = false; /* per-replay "names are on screen" evidence line */

static Uint64 s_rng_state = 0;

/* ---------------------------------------------------------------------- */
/* Config plumbing                                                        */
/* ---------------------------------------------------------------------- */

void ReplayShuffle_Configure(bool enabled, const char* root_override) {
    s_cli_enable = enabled;
    s_cli_root = (root_override != NULL && root_override[0] != '\0') ? root_override : NULL;
}

bool ReplayShuffle_IsEnabled(void) {
#if defined(STATCHECK)
    /* A STATCHECK build drives its own SCRD replay through the same
     * p1sw_buff latch (StatcheckRunner_Prologue); the two injectors cannot
     * share a session. args.c rejects --watch-replays there as well. */
    return false;
#else
    return s_cli_enable;
#endif
}

/* CLI override > explicit config key > platform default resolved at runtime.
 * The runtime default honors THIRDSARM_HOME on device (Paths_GetPrefPath
 * ends with '/'), which the compile-time config-table literal cannot. */
const char* ReplayShuffle_GetRoot(void) {
    if (s_cli_root != NULL) {
        return s_cli_root;
    }
    if (Config_HasExplicitKey(CFG_KEY_REPLAYS_ROOT)) {
        return Config_GetString(CFG_KEY_REPLAYS_ROOT);
    }

    static char buf[RB_PATH_MAX];
    static bool built = false;
    if (!built) {
#if defined(PORT_MISTER) || defined(PORT_MIYOO_MINI_PLUS)
        SDL_snprintf(buf, sizeof(buf), "%sreplays", Paths_GetPrefPath());
#else
        SDL_strlcpy(buf, "./replays", sizeof(buf));
#endif
        built = true;
    }
    return buf;
}

/* ---------------------------------------------------------------------- */
/* Shuffle RNG                                                            */
/* ---------------------------------------------------------------------- */

/* DELIBERATELY NOT the engine's Random_ix16 / Random_ix32. Those two are
 * simulation state: the .3sr checksum window hashes them (replay_player.c
 * gather_live_fields), and the player even brute-force-recovers Random_ix16
 * from a checkpoint hash. Drawing shuffle numbers from them would perturb
 * the very fields playback is verified against. This is a private
 * xorshift64* with no connection to engine state. */
static Uint32 rs_rand(void) {
    Uint64 x = s_rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s_rng_state = x;
    return (Uint32)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

/* Seed from real entropy so the order differs across boots: the high-
 * resolution performance counter, the nanosecond clock, and the address of a
 * static (ASLR). Never zero — xorshift is stuck at 0. */
static void rs_seed(void) {
    Uint64 seed = SDL_GetPerformanceCounter();
    seed ^= (Uint64)SDL_GetTicksNS() * 0x9E3779B97F4A7C15ULL;
    seed ^= (Uint64)(size_t)(void*)&s_rng_state;
    if (seed == 0) {
        seed = 0x9E3779B97F4A7C15ULL;
    }
    s_rng_state = seed;

    SDL_Log("replay-shuffle: seeded from performance counter + ns clock (seed=%016" SDL_PRIx64 ")", seed);
}

/* Fisher-Yates over s_order. Logs the resulting head of the order so two
 * boots can be compared from the log alone. */
static void rs_shuffle(void) {
    for (int i = 0; i < s_count; i++) {
        s_order[i] = (Uint16)i;
    }

    for (int i = s_count - 1; i > 0; i--) {
        const int j = (int)(rs_rand() % (Uint32)(i + 1));
        const Uint16 t = s_order[i];
        s_order[i] = s_order[j];
        s_order[j] = t;
    }

    s_cursor = 0;
    s_shuffles += 1;

    /* One compact line: the permutation head plus the label of its first
     * entry. Bounded so a 256-entry set cannot spam the log. */
    char head[192];
    int n = 0;
    head[0] = '\0';
    for (int i = 0; i < s_count && i < 12; i++) {
        const int written = SDL_snprintf(head + n, sizeof(head) - (size_t)n, "%s%d", i ? "," : "", (int)s_order[i]);
        if (written <= 0 || (size_t)(n + written) >= sizeof(head)) {
            break;
        }
        n += written;
    }

    SDL_Log("replay-shuffle: shuffle #%u of %d replay(s) — order=[%s%s] first='%s'", s_shuffles, s_count, head,
            s_count > 12 ? ",..." : "", s_count > 0 ? s_entries[s_order[0]].label : "(none)");
}

/* ---------------------------------------------------------------------- */
/* Scan                                                                   */
/* ---------------------------------------------------------------------- */

/* Enumerate the cached set and drop everything that is not launchable.
 * needs_conversion entries are raw Fightcade fetch directories (inputs +
 * savestate, no `.3sr` yet — replay_browser_scan.h) whose `path` is a
 * directory, not a file: ReplayPlayer_LoadAndStart would fail on every one.
 * No RbSort: the order is about to be randomized anyway. */
static void rs_scan(void) {
    const char* root = ReplayShuffle_GetRoot();

    const int found = RbScan(root, s_entries, RB_MAX_ENTRIES);

    int kept = 0;
    for (int i = 0; i < found; i++) {
        if (s_entries[i].needs_conversion) {
            continue;
        }
        if (kept != i) {
            s_entries[kept] = s_entries[i];
        }
        kept += 1;
    }

    s_count = kept;

    SDL_Log("replay-shuffle: scan of '%s' found %d entr(ies), %d playable (%d awaiting conversion, skipped)", root,
            found, s_count, found - s_count);
}

/* ---------------------------------------------------------------------- */
/* Divergence diagnostics (append-only, bounded)                          */
/* ---------------------------------------------------------------------- */

static void rs_diag_path(char* out, size_t out_sz) {
    const char* root = ReplayShuffle_GetRoot();
    const size_t len = SDL_strlen(root);
    const bool trailing = (len > 0) && (root[len - 1] == '/');
    SDL_snprintf(out, out_sz, "%s%s%s", root, trailing ? "" : "/", RS_DIAG_BASENAME);
}

static void rs_now_string(char* out, size_t out_sz) {
    SDL_Time now = 0;
    SDL_DateTime dt;

    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &dt, false)) {
        SDL_snprintf(out, out_sz, "t+%ums", (unsigned)SDL_GetTicks());
        return;
    }

    SDL_snprintf(out, out_sz, "%04d-%02d-%02dT%02d:%02d:%02dZ", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                 dt.second);
}

/* Record one divergence. The .3sr carries ONE 32-bit djb2 per checkpoint over
 * a 13-field window (docs/3sr-format.md), not per-field values, so WHICH field
 * diverged is not recoverable and this record does not pretend otherwise —
 * replay_player.c's log_live_fields already dumps every live value it can see.
 * Two things are recorded that the player's own message cannot carry: the
 * identity of the replay (the player's DESYNC line was written when only one
 * replay was ever in play), and the positive fact that anything reaching
 * DESYNCED is proven NOT to be Random_ix16 drift — recover_random_ix16
 * brute-forces that single field and resyncs whenever it reconciles. */
static void rs_record_desync(const RbEntry* e) {
    char path[RB_PATH_MAX];
    rs_diag_path(path, sizeof(path));

    /* Bounded: restart the file once it grows past the cap. */
    bool rotated = false;
    SDL_PathInfo info;
    if (SDL_GetPathInfo(path, &info) && info.size >= (Sint64)RS_DIAG_MAX_BYTES) {
        rotated = true;
    }

    SDL_IOStream* io = SDL_IOFromFile(path, rotated ? "w" : "a");
    if (io == NULL) {
        SDL_Log("replay-shuffle: cannot append divergence record to '%s': %s (SDL_Log record only)", path,
                SDL_GetError());
        return;
    }

    char stamp[40];
    rs_now_string(stamp, sizeof(stamp));

    char line[1024];
    const int n = SDL_snprintf(line, sizeof(line),
                               "%s%s diverged frame=%u replay='%s' path='%s' p1='%s' p2='%s' date='%s' "
                               "(not Random_ix16 drift: recover_random_ix16 resyncs that field; the .3sr stores one "
                               "djb2 per checkpoint, so the diverging field is not recoverable)\n",
                               rotated ? "--- log restarted (size cap reached) ---\n" : "", stamp,
                               ReplayPlayer_GetDesyncFrame(), e->label, e->path, e->p1[0] ? e->p1 : "(unknown)",
                               e->p2[0] ? e->p2 : "(unknown)", e->date[0] ? e->date : "(unknown)");

    if (n > 0) {
        SDL_WriteIO(io, line, (size_t)n);
    }
    SDL_CloseIO(io);

    SDL_Log("replay-shuffle: divergence recorded to '%s' — replay='%s' frame=%u", path, e->label,
            ReplayPlayer_GetDesyncFrame());
}

/* ---------------------------------------------------------------------- */
/* Playlist advance                                                       */
/* ---------------------------------------------------------------------- */

/* Start the next entry in shuffle order. Reshuffles and continues from the
 * top when the cursor runs off the end — the set never exhausts.
 *
 * A failed ReplayPlayer_LoadAndStart is not fatal and never stops the
 * viewer: it leaves the engine un-frozen in the previous replay's post-match
 * state (see the file header, point 1), so we Soft_Reset_Sub() ourselves and
 * try the next entry. Bounded at one full pass over the set so a directory of
 * entirely corrupt files terminates instead of spinning. */
static void rs_start_next(void) {
    if (s_count <= 0) {
        s_state = RS_EMPTY;
        return;
    }

    for (int attempt = 0; attempt < s_count; attempt++) {
        if (s_cursor >= s_count) {
            SDL_Log("replay-shuffle: reached the end of the set (%d replay(s)) — reshuffling and continuing",
                    s_count);
            rs_shuffle();
        }

        const int idx = (int)s_order[s_cursor];
        s_cursor += 1;

        const RbEntry* e = &s_entries[idx];

        if (ReplayPlayer_LoadAndStart(e->path)) {
            s_current = idx;
            s_played += 1;
            s_replay_frames = 0;
            s_skip_hold = 0;
            s_transition_frames = 0;
            s_hud_logged = false;
            s_state = RS_PLAYING;
            SDL_Log("replay-shuffle: now playing #%u (entry %d, slot %d/%d) '%s' — %s vs %s — %s", s_played, idx,
                    s_cursor, s_count, e->label, e->p1[0] ? e->p1 : "(unknown)", e->p2[0] ? e->p2 : "(unknown)",
                    e->path);
            return;
        }

        /* LoadAndStart already logged why. It ran ReplayPlayer_Destroy()
         * (clearing the freeze) before failing its Init check and never
         * reached its own Soft_Reset_Sub(), so the engine is loose in the
         * previous replay's post-match flow right now. Normalize it here
         * before touching anything else. */
        Soft_Reset_Sub();
        SDL_Log("replay-shuffle: entry %d '%s' failed to load — soft-reset and skipping to the next one", idx,
                e->path);
    }

    SDL_Log("replay-shuffle: no playable replay in the set of %d — idling (nothing to show)", s_count);
    s_current = -1;
    s_state = RS_EMPTY;
}

static bool rs_is_terminal(ReplayPlayerStatus st) {
    /* COMPLETE / DESYNCED / ABORTED are the player's three terminal states.
     * INACTIVE is treated as terminal defensively, exactly as the deleted
     * browser's tick_launching did, in case the player was torn down
     * underneath us. */
    return st == REPLAY_PLAYER_COMPLETE || st == REPLAY_PLAYER_DESYNCED || st == REPLAY_PLAYER_ABORTED ||
           st == REPLAY_PLAYER_INACTIVE;
}

/* ---------------------------------------------------------------------- */
/* Tick                                                                   */
/* ---------------------------------------------------------------------- */

/* Per-replay evidence that the name HUD is live. replay_overlay.c's
 * draw_name_labels() logs only ONCE PER PROCESS (its `static bool logged`),
 * which cannot show that replay N+1 also got real names — and in an
 * unattended viewer that is exactly the thing worth being able to check from
 * a log. This mirrors draw_name_labels' own gate (PLAYING + the HUD's
 * Disp_Cockpit/Allow_a_battle_f battle gate) and reports the very strings
 * that function reads, once per replay. Read-only. */
static void rs_log_hud_names(void) {
    if (s_hud_logged) {
        return;
    }
    if (ReplayPlayer_GetStatus() != REPLAY_PLAYER_PLAYING || Disp_Cockpit == 0 || Allow_a_battle_f == 0) {
        return;
    }

    const char* p1 = ReplayPlayer_GetP1Name();
    const char* p2 = ReplayPlayer_GetP2Name();

    s_hud_logged = true;
    SDL_Log("replay-shuffle: #%u in battle — HUD names on screen: '%s' vs '%s'", s_played,
            p1 != NULL ? p1 : "(none)", p2 != NULL ? p2 : "(none)");
}

/* Hold-to-skip. Reads the REAL pads (this runs before ReplayPlayer_Tick
 * overwrites them) and, on reaching the threshold, jumps straight to the next
 * replay via rs_start_next() -> ReplayPlayer_LoadAndStart.
 *
 * It deliberately does NOT route through the player's own hold-START abort:
 * that path sets ABORTED and calls Soft_Reset_Sub() WITHOUT setting the
 * freeze flag on that frame, so one njUserMain() runs mid-fade and the engine
 * freezes wherever the fade happened to reach. LoadAndStart resets cleanly.
 *
 * Returns true when a skip was performed (the caller must not also run the
 * terminal check this frame). */
static bool rs_handle_skip(void) {
    const Uint16 pad = (Uint16)(p1sw_buff | p2sw_buff);

    if ((pad & RS_SKIP_BTN) != 0) {
        s_skip_hold += 1;
    } else {
        s_skip_hold = 0;
        return false;
    }

    if (s_skip_hold < RS_SKIP_HOLD_FRAMES) {
        return false;
    }

    SDL_Log("replay-shuffle: user held the skip button %d frames — advancing to the next replay", s_skip_hold);
    s_skip_hold = 0;
    rs_start_next();
    return true;
}

void ReplayShuffle_Tick(void) {
    if (s_state == RS_OFF || s_state == RS_EMPTY) {
        return;
    }

    if (s_state == RS_UNINIT) {
        if (!ReplayShuffle_IsEnabled()) {
            s_state = RS_OFF;
            return;
        }
        if (ReplayPlayer_IsActive()) {
            /* A --play-replay boot owns the injection latch AND is not
             * browser-owned, so it would SDLApp_Exit() at the end of its one
             * replay. args.c rejects the combination; this is the belt to
             * that braces. */
            SDL_Log("replay-shuffle: a --play-replay session is already loaded — shuffle viewer stays off");
            s_state = RS_OFF;
            return;
        }
        rs_seed();
        s_state = RS_WAIT_BOOT;
        SDL_Log("replay-shuffle: enabled — waiting for the title screen (root '%s')", ReplayShuffle_GetRoot());
    }

    /* Stand down for netplay, permanently. ReplayPlayer_LoadAndStart REFUSES
     * while a session is live and ReplayPlayer_Tick ABORTS a running replay
     * when one appears, so a module that just retried on terminal status
     * would spin: refuse, abort, retry, forever. NetplayNav_IsActive() covers
     * the cold-launch window before the session state leaves IDLE. */
    if (Netplay_GetSessionState() != NETPLAY_SESSION_IDLE || NetplayNav_IsActive()) {
        SDL_Log("replay-shuffle: netplay session active — shutting the shuffle viewer down for this session");
        s_state = RS_OFF;
        return;
    }

    switch (s_state) {
    case RS_WAIT_BOOT:
        /* Same boot signal NetplayNav's NAV_WAIT_INIT and the deleted browser
         * both watched: Init_Task finished (condition 0) and the boot
         * sequence reached Loop_Demo (G_No[0] == 1). */
        if (task[TASK_INIT].condition == 0 && G_No[0] == 1) {
            rs_scan();
            if (s_count <= 0) {
                /* Decision: the OSD row is unconditional, so an empty cache is
                 * an ordinary outcome. Nothing to play, no empty-state screen
                 * — the attract loop just keeps running. */
                SDL_Log("replay-shuffle: no playable replays under '%s' — nothing to show; attract continues",
                        ReplayShuffle_GetRoot());
                s_state = RS_EMPTY;
                break;
            }
            rs_shuffle();
            rs_start_next();
        }
        break;

    case RS_PLAYING: {
        s_replay_frames += 1;
        rs_log_hud_names();

        if (rs_handle_skip()) {
            break;
        }

        const ReplayPlayerStatus st = ReplayPlayer_GetStatus();
        if (!rs_is_terminal(st)) {
            break;
        }

        if (st == REPLAY_PLAYER_DESYNCED && s_current >= 0 && s_current < s_count) {
            /* Decision: record and auto-advance. This runs unattended; it
             * must never stop and never wait for input. */
            rs_record_desync(&s_entries[s_current]);
        }

        SDL_Log("replay-shuffle: replay #%u finished (status=%d) — holding the message %d frames, then advancing",
                s_played, (int)st, RS_TRANSITION_FRAMES);
        s_transition_frames = 0;
        s_state = RS_TRANSITION;
        break;
    }

    case RS_TRANSITION:
        /* The player freezes every frame while terminal (s_browser_owned), so
         * the last rendered frame plus the overlay stay on screen throughout.
         * A skip during the hold just shortens it. */
        if (rs_handle_skip()) {
            break;
        }

        s_transition_frames += 1;
        if (s_transition_frames >= RS_TRANSITION_FRAMES) {
            rs_start_next();
        }
        break;

    case RS_UNINIT:
    case RS_OFF:
    case RS_EMPTY:
    default:
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* Draw                                                                   */
/* ---------------------------------------------------------------------- */

/* Hard on/off, no fade — the same shape as replay_overlay.c's draw_exit_hint:
 * visible for the first RS_HINT_INTRO_FRAMES of a replay, or whenever the
 * skip button is held, with an 8-pip progress bar. */
static void draw_skip_hint(void) {
    if (s_state != RS_PLAYING) {
        return;
    }

    const ReplayPlayerStatus st = ReplayPlayer_GetStatus();
    if (st != REPLAY_PLAYER_PLAYING && st != REPLAY_PLAYER_NAVIGATING) {
        return;
    }

    if (s_skip_hold <= 0 && s_replay_frames >= RS_HINT_INTRO_FRAMES) {
        return;
    }

    if (s_skip_hold > 0) {
        int filled = (s_skip_hold * RS_HINT_PIPS) / RS_SKIP_HOLD_FRAMES;
        if (filled < 0) {
            filled = 0;
        }
        if (filled > RS_HINT_PIPS) {
            filled = RS_HINT_PIPS;
        }

        char bar[RS_HINT_PIPS + 3];
        int b = 0;
        bar[b++] = '[';
        for (int i = 0; i < RS_HINT_PIPS; i++) {
            bar[b++] = (i < filled) ? '=' : '.';
        }
        bar[b++] = ']';
        bar[b] = '\0';

        char hint[48];
        SDL_snprintf(hint, sizeof(hint), "HOLD MP TO SKIP  %s", bar);
        SSPutStrProP(1, RS_CANVAS_W, RS_SKIP_HINT_Y, RS_ATR, RS_COL, hint, RS_PRIO);
    } else {
        SSPutStrProP(1, RS_CANVAS_W, RS_SKIP_HINT_Y, RS_ATR, RS_COL, "HOLD MP TO SKIP", RS_PRIO);
    }
}

void ReplayShuffle_Draw(void) {
    if (s_state == RS_OFF || s_state == RS_UNINIT || s_state == RS_EMPTY) {
        return;
    }

    /* Same boot-order guard the C1 overlay carries: SSPutStrProP renders
     * through ppgScrList, whose texture group is only bound by
     * Scrscreen_Init() inside Init_Task_1st. Drawing before that is a
     * guaranteed segfault. ppgScrList is declared in PPGWork.h, which
     * replay_overlay.c includes for exactly this reason; ReplayShuffle_Draw
     * only ever runs from RS_PLAYING / RS_TRANSITION, both of which are
     * reached long after Init_Task completes (RS_WAIT_BOOT gates on
     * task[TASK_INIT].condition == 0), so no extra guard is needed here. */

    if (s_state == RS_TRANSITION) {
        /* MUST be drawn from game_step_0's HELD-frame branch as well as the
         * normal one: while the player is frozen on a terminal state the
         * engine tick is skipped and only ReplayOverlay_Draw() would
         * otherwise run — which is to say the entire inter-replay transition
         * would be invisible. */
        SSPutStrProP(1, RS_CANVAS_W, RS_TRANSITION_Y, RS_ATR, RS_COL, "NEXT REPLAY...", RS_PRIO);
        return;
    }

    draw_skip_hint();
}

void ReplayShuffle_Destroy(void) {
    if (s_state == RS_UNINIT || s_state == RS_OFF) {
        return;
    }

    /* The viewer owns every replay it started (s_browser_owned), so it owns
     * the teardown too. Nothing here is heap-allocated — the entry table is
     * static — so this only releases the player. */
    ReplayPlayer_Destroy();
    s_state = RS_OFF;
}
