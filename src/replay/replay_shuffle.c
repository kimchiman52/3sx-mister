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
 *     short-circuited. PHASE_GAME ends on `game_ended()`, i.e.
 *     `PL_Wins[0] == 2 || PL_Wins[1] == 2` (via the bounded PHASE_POSTMATCH
 *     window that plays the round-end animation out first), and PL_Wins is
 *     zeroed by Game01_Sub() — NOT by Soft_Reset_Sub(). Only the full walk
 *     clears it; skip it and replay N+1 terminates at frame 0.
 *
 *  4. The transition delay is shorter than the overlay's own message hold.
 *     ReplayOverlay_Draw's complete_hold latches to RPL_OVL_COMPLETE_HOLD
 *     (180) on the first COMPLETE frame and counts down, so after ~3 s
 *     "REPLAY COMPLETE" vanishes while the freeze continues. 90 frames (the
 *     deleted browser's RB_RETURN_LINGER_FRAMES) keeps the screen alive
 *     across the whole transition.
 *
 * WHAT "SHUFFLE" MEANS HERE. The unit of shuffling is the QUARK, not the
 * file. A Fightcade quark is a whole session between two players, downloaded
 * whole by the wrapper as <replays_root>/<quarkid>/game_N.3sr (replay_sync.c
 * -> rs_fetch_kick, replay_proxy.c -> fetch3sr_write_game); over 15 real
 * quarks it holds 5.7 games on average. Shuffling the flat file list
 * interleaved the same pairing at random, which both watches badly and made
 * the natural repetition look like a shuffle bug. So: group by quark, shuffle
 * the QUARK order, and play each quark's games in game_N index order before
 * moving to the next quark. Show the whole set.
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
#include "replay/replay_wipe.h"

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
 * under RPL_OVL_COMPLETE_HOLD (180) so "REPLAY COMPLETE" is still up.
 *
 * This does NOT compose with the player's post-match window
 * (REPLAY_POSTMATCH_MAX_FRAMES, replay_player.c) into a doubled dead gap. That
 * window runs BEFORE the player is terminal — the engine is still animating
 * the KO and win pose, status is still PLAYING, and the overlay's complete_hold
 * has not latched (it latches on the first COMPLETE frame). The frozen part of
 * the gap is still these 90 frames. */
#define RS_TRANSITION_FRAMES 90

/* RS_EMPTY manifest poll period, in frames (~5 s at 60 Hz). */
#define RS_EMPTY_POLL_FRAMES 300

/* Hold-to-skip. SWK_NORTH is MP: the C1 player never reads it (it reads only
 * SWK_START, for hold-to-exit) and the deleted browser had already
 * repurposed it as a state-scoped action button, so it is the one face
 * button with no competing meaning here. Hold it RS_SKIP_HOLD_FRAMES
 * consecutive frames to jump to the next replay; hold START keeps its
 * existing meaning (leave the viewer) and is handled by the player itself. */
#define RS_SKIP_BTN SWK_NORTH
#define RS_SKIP_HOLD_FRAMES 60

/* Live frames of diagonal wipe-out run at the tail of the skip hold, so a
 * skipped replay leaves the screen the same way a finished one does. Matches
 * replay_player.c's REPLAY_EXIT_WIPE_FRAMES (the engine's own 8-step
 * WipeOut cadence). */
#define RS_SKIP_WIPE_FRAMES 8

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
/* 256 KiB. Was 32 KiB when only divergences were recorded -- perhaps a dozen
 * lines a week. Now every replay writes an outcome line (~150 B), so an
 * unattended box doing ~500 replays/day would rotate through 32 KiB twice a
 * day and silently zero the clean-run count this file exists to provide. */
#define RS_DIAG_MAX_BYTES 262144

/* ---------------------------------------------------------------------- */
/* State                                                                  */
/* ---------------------------------------------------------------------- */

typedef enum RsState {
    RS_UNINIT = 0,  /* first tick not yet run */
    RS_OFF,         /* disabled (or stood down) for this session — permanent */
    RS_WAIT_BOOT,   /* enabled; waiting for the attract/title screen */
    RS_PLAYING,     /* a replay is running (navigating or playing) */
    RS_TRANSITION,  /* terminal reached; holding the message, then advancing */
    RS_EMPTY,       /* nothing playable in the cache — idle, watching the manifest */
} RsState;

static RsState s_state = RS_UNINIT;

static bool s_cli_enable = false;
static const char* s_cli_root = NULL;

static RbEntry s_entries[RB_MAX_ENTRIES];
static int s_count = 0; /* playable (non-needs_conversion) entries */

/* The quark grouping. rs_group() sorts s_entries so that one quark's games are
 * CONTIGUOUS and in game_N index order, then records each run here:
 * s_group_start[g] is its first entry index, s_group_len[g] its game count.
 * s_group_order is the shuffled permutation of [0, s_group_count) — the only
 * thing randomized. s_order is the flattened play order it expands to, so
 * rs_start_next() still just walks entry indices one slot at a time. */
static Uint16 s_group_start[RB_MAX_ENTRIES];
static Uint16 s_group_len[RB_MAX_ENTRIES];
static Uint16 s_group_order[RB_MAX_ENTRIES];
static int s_group_count = 0;

static Uint16 s_order[RB_MAX_ENTRIES]; /* quark-grouped permutation of [0, s_count) */
static int s_cursor = 0;             /* next slot of s_order to play */
static int s_current = -1;           /* entry index of the replay on screen */
static Uint32 s_played = 0;          /* replays started this session (1-based counter) */
static Uint32 s_shuffles = 0;        /* how many times the set has been shuffled */

static int s_transition_frames = 0;

/* Frames since RS_EMPTY last stat()ed the manifest. Polling is throttled to
 * RS_EMPTY_POLL_FRAMES because RS_EMPTY is entered for the whole remaining
 * session on a card with no replays yet, and a stat() per frame at 60 Hz on
 * the MiSTer's SD card is pure waste for a file that changes once a day. */
static int s_empty_poll_frames = 0;
static int s_replay_frames = 0; /* frames since the current replay was started */
static int s_skip_hold = 0;
static bool s_hud_logged = false; /* per-replay "names are on screen" evidence line */

/* Session outcome tallies. Written into the rotation note so a rotation does
 * not throw away the totals -- the point of the file is the DENOMINATOR (how
 * many played cleanly), and that is exactly what discarding old lines loses. */
/* Modify-time of the replay root's manifest.json as of the last rs_scan().
 * The WRAPPER process (vendor/Main_MiSTer/replay_sync.c, rs_manifest_write)
 * rewrites that file at the end of every fetch cycle, so its mtime changing is
 * exactly the signal that new replays landed underneath us. 0 = not yet
 * known, which is also what a missing/unreadable manifest reports -- both mean
 * "no rescan trigger", never "rescan now". */
static Sint64 s_manifest_mtime = 0;
static Sint64 rs_manifest_mtime(void); /* defined below, next to rs_diag_path */

static unsigned s_n_complete = 0;
static unsigned s_n_desync = 0;
static unsigned s_n_aborted = 0;

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
/* Quark grouping                                                         */
/* ---------------------------------------------------------------------- */

/* Lines of per-quark detail rs_group() will log. A weekly set is ~90 quarks;
 * dumping all of them would bury everything else, and the first two dozen are
 * enough to read the grouping back out of a log. */
#define RS_GROUP_LOG_MAX 24

/* The grouping key for an entry: the <quarkid> directory component of its
 * path.
 *
 * RbScan walks the root flat plus EXACTLY ONE level of subdirectories
 * (replay_browser_scan.c -> RbScan), and that one level is precisely the
 * directory the fetcher writes a quark into —
 * <replays_root>/<quarkid>/game_N.{3sr,meta.json} (replay_proxy.c ->
 * fetch3sr_write_game). So the first path component below the root IS the
 * quark id; no sidecar field and no extra stat() is needed.
 *
 * FILES AT THE ROOT have no quark. They are hand-dropped .3sr files rather
 * than anything the fetcher produced, and nothing says two of them belong to
 * the same session. DECISION: each root-level file is its own single-game
 * group. That falls out of keying it on its own full path — unique per entry —
 * so the run detection below gives it a group of one and the quark shuffle
 * then scatters root-level files individually, exactly as the old per-file
 * shuffle did. */
static void rs_quark_key(const RbEntry* e, char* out, size_t out_sz) {
    const char* root = ReplayShuffle_GetRoot();
    size_t rlen = SDL_strlen(root);
    /* RbScan composes children as "<root>/<child>"; tolerate a root that was
     * configured with a trailing slash. */
    while (rlen > 0 && root[rlen - 1] == '/') {
        rlen -= 1;
    }

    if (rlen > 0 && SDL_strncmp(e->path, root, rlen) == 0 && e->path[rlen] == '/') {
        const char* rel = e->path + rlen + 1;
        const char* slash = SDL_strchr(rel, '/');
        if (slash != NULL) {
            size_t n = (size_t)(slash - rel);
            if (n >= out_sz) {
                n = out_sz - 1;
            }
            SDL_memcpy(out, rel, n);
            out[n] = '\0';
            return;
        }
    }

    SDL_strlcpy(out, e->path, out_sz);
}

/* The key's job is uniqueness, not readability: a real quark keys on its
 * quarkid, a root-level loner on its whole path. Log the loner as just its
 * filename so an absolute path does not swamp the quark-order line. */
static const char* rs_key_display(const char* key) {
    const char* base = key;
    for (const char* p = key; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

/* `game_N` -> N. The label is the basename with ".3sr" stripped
 * (replay_browser_scan.c -> label_from_path), so the index is the digit run at
 * its end. Returns false when the name is not of that shape.
 *
 * PARSED, NOT COMPARED LEXICALLY, on purpose: "game_10" sorts BEFORE "game_2".
 * Quarks that long are uncommon but the fetcher does write them (game_index
 * comes straight from the server), and a lexical order would silently show
 * game 10 second. */
static bool rs_game_index(const RbEntry* e, long* out) {
    const size_t len = SDL_strlen(e->label);
    size_t i = len;
    while (i > 0 && e->label[i - 1] >= '0' && e->label[i - 1] <= '9') {
        i -= 1;
    }
    if (i == len || i == 0 || e->label[i - 1] != '_') {
        return false;
    }
    *out = SDL_strtol(e->label + i, NULL, 10);
    return true;
}

/* Total order: quark key, then game index, then label. Entries whose name is
 * not `*_<digits>` sort after the numbered ones inside their quark rather than
 * being dropped — the viewer plays everything it scanned. */
static int rs_entry_cmp(const void* pa, const void* pb) {
    const RbEntry* a = (const RbEntry*)pa;
    const RbEntry* b = (const RbEntry*)pb;

    char ka[RB_PATH_MAX];
    char kb[RB_PATH_MAX];
    rs_quark_key(a, ka, sizeof(ka));
    rs_quark_key(b, kb, sizeof(kb));

    const int q = SDL_strcmp(ka, kb);
    if (q != 0) {
        return q;
    }

    long ia = 0;
    long ib = 0;
    const bool ha = rs_game_index(a, &ia);
    const bool hb = rs_game_index(b, &ib);
    if (ha && hb) {
        if (ia < ib) {
            return -1;
        }
        if (ia > ib) {
            return 1;
        }
    } else if (ha != hb) {
        return ha ? -1 : 1;
    }

    return SDL_strcmp(a->label, b->label);
}

/* Sort s_entries into quark-contiguous, game-index order and record the runs.
 * Called once per scan; the shuffle then permutes only the run order. */
static void rs_group(void) {
    s_group_count = 0;
    if (s_count <= 0) {
        return;
    }

    SDL_qsort(s_entries, (size_t)s_count, sizeof(s_entries[0]), rs_entry_cmp);

    char prev[RB_PATH_MAX];
    prev[0] = '\0';

    for (int i = 0; i < s_count; i++) {
        char key[RB_PATH_MAX];
        rs_quark_key(&s_entries[i], key, sizeof(key));

        if (s_group_count == 0 || SDL_strcmp(key, prev) != 0) {
            s_group_start[s_group_count] = (Uint16)i;
            s_group_len[s_group_count] = 0;
            s_group_count += 1;
            SDL_strlcpy(prev, key, sizeof(prev));
        }
        s_group_len[s_group_count - 1] += 1;
    }

    /* Tenths without floating point, so the line reads the same on every
     * libc: 57 -> "5.7 games/quark". */
    const int tenths = (s_count * 10 + s_group_count / 2) / s_group_count;
    SDL_Log("replay-shuffle: %d playable replay(s) group into %d quark(s) — %d.%d games/quark", s_count,
            s_group_count, tenths / 10, tenths % 10);

    const int shown = s_group_count < RS_GROUP_LOG_MAX ? s_group_count : RS_GROUP_LOG_MAX;
    for (int g = 0; g < shown; g++) {
        const int start = (int)s_group_start[g];
        char key[RB_PATH_MAX];
        rs_quark_key(&s_entries[start], key, sizeof(key));
        SDL_Log("replay-shuffle:   quark %d '%s' — %d game(s), entries %d..%d (first '%s')", g, rs_key_display(key),
                (int)s_group_len[g], start, start + (int)s_group_len[g] - 1, s_entries[start].label);
    }
    if (shown < s_group_count) {
        SDL_Log("replay-shuffle:   ... and %d more quark(s) not listed", s_group_count - shown);
    }
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

/* Fisher-Yates over the QUARK order (s_group_order), then flatten it into the
 * entry play order (s_order). Only the quark order is randomized: inside a
 * quark the games stay in the game_N order rs_group() sorted them into, so a
 * session is always watched start to finish.
 *
 * Logs the head of the quark order by quarkid, so two boots can be compared
 * from the log alone — and so it is visible that what moved is the quark
 * order, not the order within one. */
static void rs_shuffle(void) {
    for (int g = 0; g < s_group_count; g++) {
        s_group_order[g] = (Uint16)g;
    }

    for (int g = s_group_count - 1; g > 0; g--) {
        const int j = (int)(rs_rand() % (Uint32)(g + 1));
        const Uint16 t = s_group_order[g];
        s_group_order[g] = s_group_order[j];
        s_group_order[j] = t;
    }

    int flat = 0;
    for (int i = 0; i < s_group_count; i++) {
        const int g = (int)s_group_order[i];
        for (int k = 0; k < (int)s_group_len[g]; k++) {
            s_order[flat] = (Uint16)((int)s_group_start[g] + k);
            flat += 1;
        }
    }

    s_cursor = 0;
    s_shuffles += 1;

    /* One compact line: the head of the quark order plus the first game it
     * will play. Bounded so a 90-quark set cannot spam the log. `flat` is
     * logged rather than asserted — every entry belongs to exactly one group,
     * so it must equal s_count, and a mismatch would be visible here in a
     * release build too. */
    char head[320];
    int n = 0;
    head[0] = '\0';
    for (int i = 0; i < s_group_count && i < 8; i++) {
        char key[RB_PATH_MAX];
        rs_quark_key(&s_entries[s_group_start[s_group_order[i]]], key, sizeof(key));
        const int written =
            SDL_snprintf(head + n, sizeof(head) - (size_t)n, "%s%s", i ? "," : "", rs_key_display(key));
        if (written <= 0 || (size_t)(n + written) >= sizeof(head)) {
            break;
        }
        n += written;
    }

    SDL_Log("replay-shuffle: shuffle #%u of %d quark(s) / %d replay(s) — quark order=[%s%s] first='%s'", s_shuffles,
            s_group_count, flat, head, s_group_count > 8 ? ",..." : "",
            s_group_count > 0 ? s_entries[s_order[0]].label : "(none)");
}

/* ---------------------------------------------------------------------- */
/* Scan                                                                   */
/* ---------------------------------------------------------------------- */

/* Enumerate the cached set and drop everything that is not launchable.
 * needs_conversion entries are raw Fightcade fetch directories (inputs +
 * savestate, no `.3sr` yet — replay_browser_scan.h) whose `path` is a
 * directory, not a file: ReplayPlayer_LoadAndStart would fail on every one.
 *
 * No RbSort — its newest-date-first order is not the one the viewer wants.
 * rs_group() re-sorts into quark/game_N order instead, which is the order
 * playback actually uses, and the quark order on top of it is randomized. */
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

    /* Record the manifest this scan reflects, AFTER the scan: if the wrapper
     * rewrites it mid-scan we want the next tick to rescan, not to record a
     * newer mtime than the entries we actually read. */
    s_manifest_mtime = rs_manifest_mtime();

    rs_group();
}

/* ---------------------------------------------------------------------- */
/* Divergence diagnostics (append-only, bounded)                          */
/* ---------------------------------------------------------------------- */

/* mtime of <root>/manifest.json, or 0 when it cannot be read. */
static Sint64 rs_manifest_mtime(void) {
    char path[RB_PATH_MAX];
    SDL_snprintf(path, sizeof(path), "%s/manifest.json", ReplayShuffle_GetRoot());
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) {
        return 0;
    }
    return (Sint64)info.modify_time;
}

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
 * replay was ever in play), and — for the one field the hash CAN be inverted
 * for — whether Random_ix16 was the sole divergence (probe_random_ix16,
 * replay_player.c). That used to be recorded as a blanket "not Random_ix16
 * drift", because the probe also REPAIRED that field and no ix16-only
 * mismatch could reach DESYNCED. It repairs only v1 files now, so on a v2
 * file an ix16-only divergence does reach here, and the record has to say
 * which of the two it was. */
/* Record one replay outcome. `st` is the player's terminal status.
 *
 * A divergence-only log cannot answer the question it is usually asked: a
 * quiet file looks identical whether 400 replays played cleanly or the box
 * sat at a menu all night. Recording COMPLETE and ABORTED alongside DESYNCED
 * makes the denominator explicit. The `diverged` line keeps every field it
 * always had, in order, so anything already grepping this file keeps working;
 * only its trailing parenthetical changed, when the Random_ix16 repair became
 * v1-only and the blanket "not Random_ix16 drift" stopped being true. */
static void rs_record_outcome(const RbEntry* e, ReplayPlayerStatus st) {
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

    char rot[176];
    rot[0] = '\0';
    if (rotated) {
        SDL_snprintf(rot, sizeof(rot),
                     "--- log restarted (size cap reached); session so far: %u complete, %u diverged, %u skipped ---\n",
                     s_n_complete, s_n_desync, s_n_aborted);
    }

    char line[1024];
    int n;
    if (st == REPLAY_PLAYER_DESYNCED) {
        n = SDL_snprintf(line, sizeof(line),
                         "%s%s diverged frame=%u replay='%s' path='%s' p1='%s' p2='%s' date='%s' %s\n",
                         rot, stamp, ReplayPlayer_GetDesyncFrame(), e->label, e->path,
                         e->p1[0] ? e->p1 : "(unknown)", e->p2[0] ? e->p2 : "(unknown)",
                         e->date[0] ? e->date : "(unknown)",
                         ReplayPlayer_DesyncWasIx16Only()
                             ? "(Random_ix16 was the ONLY divergent field)"
                             : "(not Random_ix16 drift; the .3sr stores one djb2 per checkpoint, so "
                               "the diverging field is not recoverable)");
    } else {
        n = SDL_snprintf(line, sizeof(line), "%s%s %s frames=%d replay='%s' p1='%s' p2='%s' date='%s'\n", rot, stamp,
                         (st == REPLAY_PLAYER_COMPLETE) ? "completed" : "skipped", s_replay_frames, e->label,
                         e->p1[0] ? e->p1 : "(unknown)", e->p2[0] ? e->p2 : "(unknown)",
                         e->date[0] ? e->date : "(unknown)");
    }

    if (n > 0) {
        SDL_WriteIO(io, line, (size_t)n);
    }
    SDL_CloseIO(io);

    if (st == REPLAY_PLAYER_DESYNCED) {
        SDL_Log("replay-shuffle: divergence recorded to '%s' — replay='%s' frame=%u", path, e->label,
                ReplayPlayer_GetDesyncFrame());
    }
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

            /* The quark is named on every line on purpose: it is what makes
             * "these five played back to back" readable straight out of a
             * log, without cross-referencing paths. */
            char key[RB_PATH_MAX];
            rs_quark_key(e, key, sizeof(key));
            SDL_Log("replay-shuffle: now playing #%u (quark '%s' game '%s', entry %d, slot %d/%d) — %s vs %s — %s",
                    s_played, rs_key_display(key), e->label, idx, s_cursor, s_count, e->p1[0] ? e->p1 : "(unknown)",
                    e->p2[0] ? e->p2 : "(unknown)", e->path);
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

    /* Cover the cut. The skip jumps straight to ReplayPlayer_LoadAndStart,
     * which raises the viewer's black cover in the same frame — so without
     * this a battle in progress would hard-cut to black. The hold threshold
     * is a fixed frame count, so its last few frames are a deterministic tell
     * exactly like the round-end countdown: start the diagonal wipe-out
     * RS_SKIP_WIPE_FRAMES frames before the jump and the cut lands under a
     * finished cover.
     *
     * RS_PLAYING only. A skip during RS_TRANSITION is already running on held
     * frames — black, with the "NEXT REPLAY..." card on it — so there is
     * nothing to wipe over and the cover is raised by the relaunch anyway. */
    if (s_state == RS_PLAYING && s_skip_hold == RS_SKIP_HOLD_FRAMES - RS_SKIP_WIPE_FRAMES) {
        ReplayWipe_BeginExit(RS_SKIP_WIPE_FRAMES, "skip gesture about to fire");
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
    /* RS_OFF is the only permanent state. RS_EMPTY is NOT: it means "nothing
     * playable was on the card the last time we looked", and the wrapper's
     * daily fetch changes that without restarting the core. See the RS_EMPTY
     * case below. */
    if (s_state == RS_OFF) {
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

        if (st == REPLAY_PLAYER_COMPLETE) {
            s_n_complete++;
        } else if (st == REPLAY_PLAYER_DESYNCED) {
            s_n_desync++;
        } else if (st == REPLAY_PLAYER_ABORTED) {
            s_n_aborted++;
        }

        if (s_current >= 0 && s_current < s_count &&
            (st == REPLAY_PLAYER_COMPLETE || st == REPLAY_PLAYER_DESYNCED || st == REPLAY_PLAYER_ABORTED)) {
            /* Decision: record and auto-advance. This runs unattended; it
             * must never stop and never wait for input. INACTIVE is deliberately
             * not recorded -- it means the player was torn down underneath us,
             * which is not an outcome for this replay. */
            rs_record_outcome(&s_entries[s_current], st);
        }

        SDL_Log("replay-shuffle: replay #%u finished (status=%d) — holding the message %d frames, then advancing",
                s_played, (int)st, RS_TRANSITION_FRAMES);
        s_transition_frames = 0;
        s_state = RS_TRANSITION;
        break;
    }

    case RS_TRANSITION:
        /* The player freezes every frame while terminal (s_browser_owned).
         * Those held frames render BLACK with only the overlay text on them —
         * nothing of the last battle frame is retained (replay_player.c's
         * s_stall_frame comment has the render-path proof). So this hold is a
         * black "REPLAY COMPLETE" card between replays, not a freeze-frame;
         * the round-end animation the viewer sees is played live BEFORE it, in
         * the player's PHASE_POSTMATCH window. A skip during the hold just
         * shortens it. */
        if (rs_handle_skip()) {
            break;
        }

        s_transition_frames += 1;
        if (s_transition_frames >= RS_TRANSITION_FRAMES) {
            /* Pick up replays that landed after boot.
             *
             * rs_scan() used to run exactly once, from RS_WAIT_BOOT, and never
             * again -- so the daily fetch (ReplaySyncTick in the wrapper) could
             * put 500 new replays on the card and this viewer would keep looping
             * whatever it saw at boot until someone restarted the core. Measured
             * 2026-09-04: a 24-hour session scanned 13 entries, then played 734
             * replays -- 56 passes over those same 13 -- while 507 sat unread.
             *
             * Rescanning HERE, and only here, is what makes it safe: the player
             * is terminal, no .3sr is loaded, and s_current is about to be
             * reassigned by rs_start_next(), so rebuilding s_entries/s_order
             * cannot invalidate anything in use. Doing it mid-replay could not
             * be made safe that cheaply.
             *
             * Gated on the manifest mtime rather than a timer or a dirent count:
             * the wrapper rewrites manifest.json once per completed fetch cycle,
             * so it changes exactly when the set does -- and a partially-written
             * set does not trigger a scan, because the manifest is renamed into
             * place last. */
            const Sint64 mtime = rs_manifest_mtime();
            if (mtime != 0 && mtime != s_manifest_mtime) {
                SDL_Log("replay-shuffle: manifest changed (%lld -> %lld) -- rescanning", (long long)s_manifest_mtime,
                        (long long)mtime);
                const int before = s_count;
                rs_scan();
                if (s_count <= 0) {
                    /* Never let a rescan strand a working set. Only reachable if
                     * the replays root was emptied underneath us. */
                    SDL_Log("replay-shuffle: rescan found nothing playable (was %d) -- idling", before);
                    s_state = RS_EMPTY;
                    break;
                }
                SDL_Log("replay-shuffle: rescan %d -> %d playable; reshuffling", before, s_count);
                rs_shuffle();
            }
            rs_start_next();
        }
        break;

    case RS_EMPTY:
        /* Not a terminal state. Every path into RS_EMPTY is "the set was empty
         * when we scanned" -- at boot (RS_WAIT_BOOT), after a rescan found
         * nothing, or when every entry failed to load (rs_start_next). The
         * wrapper's fetch (vendor/Main_MiSTer/replay_sync.c) can land hundreds
         * of replays hours later, and before this poll existed they were
         * invisible until someone restarted the core -- the same defect
         * c6a75572 fixed for the normal RS_TRANSITION path, one state over.
         *
         * Safe for the same reason the RS_TRANSITION rescan is: no .3sr is
         * loaded, the player is inactive and s_current is -1, so rebuilding
         * s_entries/s_order cannot invalidate anything in use.
         *
         * Gated on the manifest mtime, not a dirent count, for the same reason
         * as the RS_TRANSITION rescan: the wrapper renames manifest.json into
         * place LAST, so a half-fetched set never triggers a scan. */
        s_empty_poll_frames += 1;
        if (s_empty_poll_frames >= RS_EMPTY_POLL_FRAMES) {
            s_empty_poll_frames = 0;
            const Sint64 mtime = rs_manifest_mtime();
            if (mtime != 0 && mtime != s_manifest_mtime) {
                SDL_Log("replay-shuffle: manifest changed while idle (%lld -> %lld) -- rescanning",
                        (long long)s_manifest_mtime, (long long)mtime);
                rs_scan();
                if (s_count > 0) {
                    SDL_Log("replay-shuffle: %d playable replay(s) appeared -- resuming", s_count);
                    rs_shuffle();
                    rs_start_next();
                }
            }
        }
        break;

    case RS_UNINIT:
    case RS_OFF:
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
