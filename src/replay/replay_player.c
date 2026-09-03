/* Step C1 of docs/plan-fcade-replay-browser.md — runtime .3sr replay player.
 *
 * Phase machine cloned from src/test/statcheck_runner.c (the A4-proven
 * oracle harness), with these deliberate differences:
 * - Inputs come from the loaded .3sr word table instead of an SCRD archive;
 *   RNG seeds come from the .3sr header (seeded at the exact point
 *   statcheck runs Statcheck_SyncValues: PHASE_GAME_TRANSITION, once
 *   G_No[1] == 2 — statcheck_runner.c:334-348).
 * - No per-frame RAM compare. Divergence is detected via the .3sr's sparse
 *   13-field djb2 checksum table (docs/3sr-format.md §4), recomputed from
 *   LIVE engine state at the same tick boundary statcheck compares at.
 * - Normal frame pacing, rendering + audio untouched (plan §4.1) — this is
 *   the user-facing playback engine, not a headless verifier.
 *
 * LAYOUT INVARIANT (plan §2.3, docs/3sr-format.md §3): the .3sr input word
 * table stores ARCADE-RAM-layout words (bits 0-3 directions, 4-6 LP/MP/HP,
 * 7-9 LK/MK/HK, 12 start) exactly as archived at P1SW_0/P2SW_0. The engine
 * latch (p1sw_buff/p1sw_0) carries SWK-layout words (kicks 8-10, start 14 —
 * include/sf33rd/AcrSDK/common/pad.h). We convert arcade->SWK once at LOAD
 * time (same shifts as src/test/replay_game.c:12-26: LK 7->8, MK 8->9,
 * HK 9->10, start 12->14) and keep only SWK words in memory; the checksum
 * recomputation applies the exact inverse shift to recover the arcade word
 * the hash was computed over (docs/3sr-format.md §4.2 fields 12/13 +
 * §4.3 canonicalization). */

#include "replay/replay_player.h"

#include "main.h"
#include "netplay/netplay.h"
#include "port/config/config.h"
#include "port/sdl/sdl_app.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/utils/djb2_hash.h"

#include "cJSON.h"

#include <SDL3/SDL.h>

#include <stdbool.h>

/* ---------------------------------------------------------------------- */
/* Loaded replay data                                                     */
/* ---------------------------------------------------------------------- */

#define REPLAY_3SR_MAGIC "3SR1"
#define REPLAY_3SR_VERSION 1
#define REPLAY_3SR_HEADER_SIZE 28
/* All bits an arcade-RAM-layout input word may carry: 0-3 directions,
 * 4-6 punches, 7-9 kicks, 12 start (docs/3sr-format.md §3). */
#define ARCADE_WORD_MASK 0x13FFu

typedef struct ReplayChecksumEntry {
    Uint32 frame; /* .3sr frame index (k * checksum_interval) */
    Uint32 djb2;
} ReplayChecksumEntry;

typedef struct ReplayFile {
    Uint8 characters[2]; /* post-CHAR_ARCADE_TO_3SX ids — do NOT reconvert */
    Uint8 supers[2];
    Uint8 colors[2];
    Uint8 new_challenger;
    Uint16 random_ix16; /* bit pattern of the archived s16 (format §2) */
    Uint16 random_ix32;
    Uint32 frame_count;
    Uint16 checksum_interval;
    Uint16 checksum_count;
    Uint16 (*inputs)[2];            /* SWK-layout words (converted at load) */
    ReplayChecksumEntry* checksums; /* decoded {frame, djb2} entries */
} ReplayFile;

typedef enum Phase {
    PHASE_TITLE,
    PHASE_MENU,
    PHASE_CHARACTER_SELECT_TRANSITION,
    PHASE_CHARACTER_SELECT,
    PHASE_GAME_TRANSITION,
    PHASE_GAME,
    PHASE_POSTMATCH, /* KO landed; the engine plays the round-end tail out */
    PHASE_DONE,      /* complete/desynced/aborted — pads released, module idle */
} Phase;

static bool loaded = false;
static ReplayFile replay;
static char meta_json_path[1024];
static bool has_meta_json = false;

/* ---------------------------------------------------------------------- */
/* Step C2 — viewer metadata + hold-START-to-exit state                   */
/* ---------------------------------------------------------------------- */

/* Overlay metadata, filled at Init. Player names sanitized to printable
 * ASCII (SS glyph table is ascProData[128]; any byte outside 0x20-0x7E
 * would index out of range and render garbage). */
static char meta_p1_name[64];
static char meta_p2_name[64];
static char meta_date[16]; /* "YYYY-MM-DD\0" */
static char replay_label[256];
static bool have_p1_name = false;
static bool have_p2_name = false;
static bool have_date = false;
/* Fightcade rank 1..6 (letter grades E,D,C,B,A,S in Fightcade's own
 * convention — fightcade-api `Rank = ['Unranked','E','D','C','B','A','S']`);
 * 0 = unranked/unknown, drawn as name-only (never fabricated). Sourced from
 * the meta sidecar's players[].rank or the --live-replay-pN-rank handoff. */
static int meta_p1_rank = 0;
static int meta_p2_rank = 0;

/* Hold-START-to-exit: abort playback and return to title once the REAL user
 * holds START for this many consecutive frames (~2s @ 60fps). The hint is
 * shown for the first REPLAY_HINT_INTRO_FRAMES of the match, plus any time
 * START is being held. */
#define REPLAY_EXIT_HOLD_FRAMES 120
#define REPLAY_HINT_INTRO_FRAMES 300
static int exit_hold_frames = 0;

static Uint64 frame_index = 0; /* global tick counter (menu mash parity) */
static Phase phase = PHASE_TITLE;
static int char_select_phase = 0;
static int wait_timer = 0;
static Uint32 play_index = 0; /* .3sr frame index currently being played */
static Uint16 input_buffers[2] = { 0 };
/* This tick's injected SWK words with nothing added on top — the exact
 * conversion of .3sr word [play_index]. PHASE_GAME no longer adds taps, but
 * the pre-battle nav phases do, and live p1sw_0 also carries whatever the
 * local pads are doing; the checksum recomputation inverts THESE back to the
 * arcade words the hash covered (see hash_live_state). */
static Uint16 pure_words[2] = { 0 };

static ReplayPlayerStatus status = REPLAY_PLAYER_INACTIVE;
static Uint32 desync_frame = 0;
static Uint32 checksums_checked = 0;
static Uint32 checksums_passed = 0;
static Uint32 checksums_skipped_nonbattle = 0;
static Uint32 checksums_r16_resynced = 0;

/* THIS frame is held: the engine tick is skipped entirely (main.c reads
 * ReplayPlayer_IsStallingThisFrame and branches around njUserMain). Set by
 * finish() and tick_terminal() below — it is the mechanism the qix-trap
 * freeze is built out of, not a streaming leftover. Also read by Epilogue,
 * which must not advance the play cursor across a frame the engine never ran.
 *
 * A HELD FRAME RENDERS BLACK, not the last battle frame. There is no retained
 * scene: SDLApp_EndFrame calls SoftwareRenderer_RenderFrame() every frame,
 * and RenderFrame -> render_band -> clear_band fills the canvas with
 * 0xFF000000 before drawing this frame's `quads`, then arrsetlen(quads, 0)
 * empties the queue. The held branch in main.c's game_step_0 skips
 * njUserMain (Game_Task -> BG_Draw_System / reqPlayerDraw) and
 * seqsAfterProcess (-> Renderer_DrawSprites2Batch), so the ONLY quads a held
 * frame submits are the ones ReplayOverlay_Draw / ReplayShuffle_Draw put
 * there. Black plus overlay text is the whole picture — which is exactly why
 * PHASE_POSTMATCH below exists: the freeze is a cut to black, so the KO and
 * win pose have to be played out BEFORE it, not behind it. */
static bool s_stall_frame = false;

/* Post-match window: let the round-end animation PLAY before freezing.
 *
 * game_ended() (PL_Wins == 2) goes true on the winning hit, and finish()
 * freezes the whole frame from that tick on. Cutting there is correct for
 * safety but reads as a hard stop on the KO — no fall, no win pose, no
 * winner message — and worse, a held frame renders BLACK (see s_stall_frame),
 * so it is a cut to black ON the winning hit. What this window buys is
 * LIVE-RENDERED frames: the animation has to play before the freeze, because
 * nothing plays behind it. So instead of finishing on that frame we hand the
 * engine a BOUNDED number of further frames (PHASE_POSTMATCH), with NEUTRAL
 * pads and no checkpointing, and only then call finish() exactly as before.
 * The black "REPLAY COMPLETE" card still lands — after the round-end
 * presentation now, rather than in place of it.
 *
 * WHY IT IS STILL SAFE. The trap this defers (see the block comment below)
 * lives in Game_Manage_10th — Management_Jmp_Tbl[C_No[0]] index 9, so it runs
 * only at C_No[0] == 9. Reaching it from where the KO leaves us is a one-way
 * climb through C_No[0] == 7 and 8: every PL_Wins increment in manage.c fires
 * at C_No[0] <= 6 (Game_Manage_4th, Game_Manage_6_1's complete-judgement
 * tail), and the ONLY assignments that take C_No[0] above 6 from there are
 * `C_No[0] = 7` / `C_No[0] = 12` (Game_Manage_6th case 1, Game_Manage_7_3,
 * Game_Manage_7_6's C_No[0]++) — `C_No[0] = 10` lives inside Game_Manage_9th,
 * which is already past 6. So `C_No[0] > 6` is a complete "the round-end
 * presentation is over, the teardown flow is starting" tell, and it is where
 * the window ends — one full stage before Game_Manage_10th can run.
 *
 * HOW THE CAP WAS MEASURED (2026-09-02, host, three real .3sr files —
 * 3sr-out/{1783909831386-3826,1784866348472-2003,1784866353037-3646}/game_0):
 * a temporary probe logged G_No[]/C_No[]/Game_pause/Disp_Cockpit every frame
 * after game_ended() while letting the engine free-run. The three runs agreed
 * closely:
 *
 *     event                                   replay A / B / C   (frames
 *                                                                 after
 *                                                                 game_ended)
 *     C_No[0] > 6   (win pose + message done)     365 / 370 / 328
 *     Disp_Cockpit == 0  (HUD torn down)          495 / 500 / 458
 *     G_No[1] != 2  (left the in-game state)      579 / 584 / 542
 *
 * REPLAY_POSTMATCH_MAX_FRAMES is the backstop for the case where the C_No[0]
 * guard somehow never fires (e.g. a win pose that stalls). 420 sits above the
 * worst measured presentation length (370) and below the EARLIEST measured
 * screen teardown (458), so the cap alone stops the engine before it leaves
 * the battle screen even with the state guard removed. Re-derive by
 * reinstating the probe, not by guessing.
 *
 * HEADROOM, if a later change wants a longer window. From C_No[0] == 7 the
 * shortest possible path to Game_Manage_10th's Switch_Screen_Init(0) is about
 * 198 frames of fixed countdowns with neutral pads (Game_Manage_8_0 1 +
 * 81_0 20 + 81_1 + 81_2 20 + 81_3 + 8_2 50 + 8_3 30 + Game_Manage_9th 1 +
 * Game_Manage_10th's Button_Cut_EX(C_Timer = 75)), and the Scene_Cut
 * shortcuts in 8_3 / 7_6 need SWK_ATTACKS in p1sw_0/p2sw_0, which this window
 * never injects. So there is real room past where we stop; we stop early
 * because the score tally is not "the round ending", not because it is unsafe.
 *
 * WHAT THE PROBE COULD NOT CONFIRM. The s_browser_owned comment below
 * attributes the fatal "qix is out of range" to Game_Manage_10th. The probe
 * free-ran three replays 25,000+ frames past game_ended() — through
 * Game_Manage_10th, the continue flow, char select and into a whole further
 * match — and never tripped it. So that attribution is UNVERIFIED here. It is
 * not a reason to drop the freeze: the fatal is real (it is what ff626551 was
 * written for), the reproducer is simply not these three files. The safety
 * argument above deliberately does not lean on the free-run result.
 *
 * ONLY THE MATCH END IS AFFECTED. game_ended() is PL_Wins == 2, i.e. the
 * match; between rounds it stays false and PHASE_GAME keeps injecting the
 * recorded input words exactly as before.
 * The probe runs confirmed one PHASE_POSTMATCH entry per replay.
 *
 * The pads are held NEUTRAL here on purpose: the recording's post-KO tail is
 * not replayed (the play cursor stops at game_ended()), and feeding its
 * attack presses in would mash through Game_Manage_7_2's Button_Cut_EX and
 * skip the very animation this window exists to show. */
#define REPLAY_POSTMATCH_MAX_FRAMES 420
static int postmatch_frames = 0;

/* Post-terminal freeze + teardown (qix-trap fix). A .3sr is a VIEWER of a
 * recorded stream: the recording ends at match end, but the game's LIVE
 * post-match flow (win pose -> continue -> char select — Game_Manage_10th,
 * manage.c:1115) is NOT part of the recording. If it free-runs after the
 * replay reaches a terminal state it pushes a win-pose effect WORK whose
 * `myself` index is out of [0,128), tripping push_effect_work's bound check
 * (effect.c:229, fatal_error "qix is out of range"). So once terminal we HOLD
 * the whole frame (s_stall_frame -> main.c skips njUserMain) — realizing the
 * "freeze + message" UX the desync path already documents —
 * then tear the session down. Ownership decides the teardown: a browser launch
 * (ReplayPlayer_LoadAndStart) leaves it to the browser's tick_launching
 * (Soft_Reset_Sub + Destroy + reopen list, replay_browser.c:1000); a CLI
 * --play-replay launch has no browser, so after the
 * overlay linger it ends the session with a clean process exit (SDLApp_Exit),
 * which on device lets the wrapper's restart_to_menu return cleanly instead of
 * the qix crash-to-menu. */
#define REPLAY_TERMINAL_LINGER_FRAMES 180 /* == RPL_OVL_COMPLETE_HOLD: show the full message */
static bool s_browser_owned = false;      /* launched by the OSD browser (owns teardown) */
static int terminal_linger = 0;           /* frames held on the terminal overlay */
static bool terminal_exit_requested = false; /* SDLApp_Exit already pushed (CLI path) */

/* ---------------------------------------------------------------------- */
/* Input word layout conversion (plan §2.3)                               */
/* ---------------------------------------------------------------------- */

/* arcade -> SWK: bits 0-6 (directions + punches) coincide; kicks shift
 * 7->8/8->9/9->10, start 12->14. Same mapping as src/test/replay_game.c:12-26
 * (which reads P1SW_0-layout words like ours; statcheck_runner.c's
 * read_input_buff starts from the already-engine-layout WCP mirror instead —
 * plan §2.3 ERRATUM 2026-07-21 — but emits identical SWK words). */
static Uint16 arcade_to_swk(Uint16 arcade) {
    Uint16 swk = arcade & 0x7F;

    if (arcade & (1 << 7)) {
        swk |= SWK_SOUTH; /* LK: 7 -> 8 */
    }

    if (arcade & (1 << 8)) {
        swk |= SWK_EAST; /* MK: 8 -> 9 */
    }

    if (arcade & (1 << 9)) {
        swk |= SWK_RIGHT_TRIGGER; /* HK: 9 -> 10 */
    }

    if (arcade & (1 << 12)) {
        swk |= SWK_START; /* start: 12 -> 14 */
    }

    return swk;
}

/* SWK -> arcade: exact inverse of arcade_to_swk. Round-trips losslessly for
 * every word whose arcade source fits ARCADE_WORD_MASK (verified at load;
 * out-of-mask source bits would be unrepresentable and are rejected). */
static Uint16 swk_to_arcade(Uint16 swk) {
    Uint16 arcade = swk & 0x7F;

    if (swk & SWK_SOUTH) {
        arcade |= 1 << 7;
    }

    if (swk & SWK_EAST) {
        arcade |= 1 << 8;
    }

    if (swk & SWK_RIGHT_TRIGGER) {
        arcade |= 1 << 9;
    }

    if (swk & SWK_START) {
        arcade |= 1 << 12;
    }

    return arcade;
}

/* ---------------------------------------------------------------------- */
/* Loader (docs/3sr-format.md §1)                                         */
/* ---------------------------------------------------------------------- */

static Uint16 rd_u16le(const Uint8* p) {
    return (Uint16)(p[0] | ((Uint16)p[1] << 8));
}

static Uint32 rd_u32le(const Uint8* p) {
    return (Uint32)p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16) | ((Uint32)p[3] << 24);
}

static bool load_error(const char* path, const char* why) {
    SDL_Log("replay: refusing '%s': %s", path, why);
    return false;
}

static void stash_meta_sidecar_path(const char* path_3sr) {
    /* <name>.3sr -> <name>.meta.json. Path only — JSON parsing is Step C2
     * (cJSON); nothing here consumes the contents yet. */
    const size_t len = SDL_strlen(path_3sr);
    const char* suffix = ".3sr";
    const size_t suffix_len = 4;

    has_meta_json = false;

    if (len < suffix_len || SDL_strcmp(path_3sr + len - suffix_len, suffix) != 0) {
        return;
    }

    if (len - suffix_len + sizeof(".meta.json") > sizeof(meta_json_path)) {
        return;
    }

    SDL_memcpy(meta_json_path, path_3sr, len - suffix_len);
    SDL_strlcpy(meta_json_path + (len - suffix_len), ".meta.json", sizeof(meta_json_path) - (len - suffix_len));

    SDL_PathInfo info;
    if (SDL_GetPathInfo(meta_json_path, &info) && info.type == SDL_PATHTYPE_FILE) {
        has_meta_json = true;
        SDL_Log("replay: meta sidecar present: %s", meta_json_path);
    }
}

/* Copy a JSON string into a fixed buffer, replacing any byte outside the
 * printable-ASCII range (0x20-0x7E) with '?'. Fightcade names can carry
 * arbitrary UTF-8; the SS font only has a 128-entry glyph table indexed by
 * the raw byte, so an unsanitized high byte would index out of bounds. */
static bool copy_sanitized_name(const char* src, char* dst, size_t dst_sz) {
    if (src == NULL || src[0] == '\0') {
        return false;
    }

    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < dst_sz; i++) {
        const unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    dst[i] = '\0';
    return i > 0;
}

/* Fallback label shown when the sidecar carries no player names: the .3sr
 * basename with the directory prefix and ".3sr" suffix stripped, sanitized to
 * printable ASCII through copy_sanitized_name() above (same reasoning as
 * meta_p1_name/meta_p2_name: the SS glyph table is ascProData[128], and a
 * filename can carry arbitrary bytes just as a Fightcade meta.json name can).
 * Always set (empty string worst case) so the overlay never has to handle a
 * NULL. */
static void set_label_from_path(const char* path_3sr) {
    const char* base = path_3sr;

    for (const char* p = path_3sr; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }

    /* copy_sanitized_name() leaves dst untouched (not even NUL-terminated) on
     * a NULL/empty src instead of clearing it — fine for the sidecar name
     * fields (fresh stack/zeroed statics each parse), but replay_label is a
     * module-lifetime static this function does NOT reset per-launch
     * (reset_runtime_state() intentionally leaves it — see that function's
     * comment). Clear first so the pathological empty-basename case (path
     * ending in '/' ) can't leave a PRIOR replay's label on screen instead of
     * an honest empty string. */
    replay_label[0] = '\0';
    copy_sanitized_name(base, replay_label, sizeof(replay_label));

    const size_t len = SDL_strlen(replay_label);
    if (len >= 4 && SDL_strcmp(replay_label + len - 4, ".3sr") == 0) {
        replay_label[len - 4] = '\0';
    }
}

/* players[k].rank — Fightcade's numeric rank (1..6 = E..S; 0 = unranked).
 * Only present when the meta producer carried the catalog row's full player
 * objects (fcade-proxy publishTrackerOutput / make_3sr.py quark.json branch).
 * Accept a number or a numeric string; anything else -> 0 (unknown). */
static int player_rank_from_item(const cJSON* item) {
    if (!cJSON_IsObject(item)) {
        return 0;
    }
    const cJSON* rank = cJSON_GetObjectItemCaseSensitive(item, "rank");
    int v = 0;
    if (cJSON_IsNumber(rank)) {
        v = (int)rank->valuedouble;
    } else if (cJSON_IsString(rank) && rank->valuestring != NULL) {
        v = SDL_atoi(rank->valuestring);
    }
    return (v >= 1 && v <= 6) ? v : 0;
}

/* players[k] may be a bare string ("name") or an object carrying a "name"
 * field, depending on the meta.json producer — accept both. */
static const char* player_name_from_item(const cJSON* item) {
    if (item == NULL) {
        return NULL;
    }

    if (cJSON_IsString(item)) {
        return item->valuestring;
    }

    if (cJSON_IsObject(item)) {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (cJSON_IsString(name)) {
            return name->valuestring;
        }
    }

    return NULL;
}

/* Portable civil-date-from-epoch (Howard Hinnant's algorithm) — no
 * gmtime/localtime/locale dependency, identical result on every build
 * profile. `ms` is the sidecar's ms-since-epoch `date`. */
static void format_date_ms(long long ms, char* out, size_t out_sz) {
    long long days = ms / 86400000LL;
    days += 719468; /* shift epoch from 1970-01-01 to 0000-03-01 */

    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    long long y = (long long)yoe + era * 400 + (m <= 2);

    SDL_snprintf(out, out_sz, "%04lld-%02u-%02u", y, m, d);
}

/* Parse the .meta.json sidecar for players[0/1].name + date (Step C2).
 * Best-effort: any failure leaves have_p1_name/have_p2_name/have_date false
 * and the overlay falls back to the .3sr label. Never fatal. */
static void parse_meta_sidecar(void) {
    if (!has_meta_json) {
        return;
    }

    size_t size = 0;
    char* text = SDL_LoadFile(meta_json_path, &size);
    if (text == NULL) {
        SDL_Log("replay: meta sidecar '%s' present but unreadable: %s (using .3sr label)", meta_json_path,
                SDL_GetError());
        return;
    }

    cJSON* root = cJSON_ParseWithLength(text, size);
    SDL_free(text);

    if (root == NULL) {
        SDL_Log("replay: meta sidecar '%s' is not valid JSON (using .3sr label)", meta_json_path);
        return;
    }

    /* NON-CLOBBERING adoption: a live session may already hold names/ranks
     * from the wrapper handoff (ReplayPlayer_SetLiveMeta) before the sidecar
     * lands mid-playback — a sidecar that parses but carries no usable
     * players[] must never blank already-known values. A sidecar that DOES
     * carry them wins (same catalog-row source, freshest copy). */
    const cJSON* players = cJSON_GetObjectItemCaseSensitive(root, "players");
    if (cJSON_IsArray(players)) {
        const cJSON* it1 = cJSON_GetArrayItem(players, 0);
        const cJSON* it2 = cJSON_GetArrayItem(players, 1);
        if (copy_sanitized_name(player_name_from_item(it1), meta_p1_name, sizeof(meta_p1_name))) {
            have_p1_name = true;
            const int r = player_rank_from_item(it1);
            if (r != 0) {
                meta_p1_rank = r;
            }
        }
        if (copy_sanitized_name(player_name_from_item(it2), meta_p2_name, sizeof(meta_p2_name))) {
            have_p2_name = true;
            const int r = player_rank_from_item(it2);
            if (r != 0) {
                meta_p2_rank = r;
            }
        }
    }

    const cJSON* date = cJSON_GetObjectItemCaseSensitive(root, "date");
    if (cJSON_IsNumber(date) && date->valuedouble > 0.0) {
        format_date_ms((long long)date->valuedouble, meta_date, sizeof(meta_date));
        have_date = true;
    }

    cJSON_Delete(root);

    SDL_Log("replay: meta parsed — p1=%s[rank %d] p2=%s[rank %d] date=%s", have_p1_name ? meta_p1_name : "(none)",
            meta_p1_rank, have_p2_name ? meta_p2_name : "(none)", meta_p2_rank, have_date ? meta_date : "(none)");
}

/* Reset every playback-runtime static to its start-of-session value. Called
 * at the top of ReplayPlayer_Init so both the boot (--play-replay) path and
 * the browser's ReplayPlayer_LoadAndStart relaunch path begin from a clean
 * phase machine — the browser reuses this module across multiple launches in
 * one process, so stale phase/play_index/counter state from a prior replay
 * must not carry over. */
static void reset_runtime_state(void) {
    frame_index = 0;
    phase = PHASE_TITLE;
    char_select_phase = 0;
    wait_timer = 0;
    play_index = 0;
    SDL_zeroa(input_buffers);
    SDL_zeroa(pure_words);
    exit_hold_frames = 0;
    desync_frame = 0;
    checksums_checked = 0;
    checksums_passed = 0;
    checksums_skipped_nonbattle = 0;
    checksums_r16_resynced = 0;
    /* Terminal freeze/teardown bookkeeping. Default to CLI ownership; the
     * browser's ReplayPlayer_LoadAndStart flips s_browser_owned true after this
     * runs (it calls Init, which calls us). */
    s_browser_owned = false;
    terminal_linger = 0;
    terminal_exit_requested = false;
    postmatch_frames = 0;
}

/* True while the player is holding the whole engine frame (see s_stall_frame).
 * main.c skips njUserMain and draws only the overlay — the frame comes out
 * black with the overlay text on it, not a frozen battle scene (see
 * s_stall_frame). That hold is what keeps the game's post-match flow from
 * free-running into the qix effect trap once playback reaches a terminal
 * state. */
bool ReplayPlayer_IsStallingThisFrame(void) {
    return s_stall_frame;
}

/* Adopt caller-supplied metadata (names + Fightcade ranks + date) so the HUD
 * name labels and the bottom status line are populated from the FIRST frame,
 * without waiting on — or requiring — a .meta.json sidecar next to the .3sr.
 * Call AFTER ReplayPlayer_Init (Destroy inside it clears all meta state).
 * Fed from argv by main.c's --play-replay boot path. The sidecar parse stays
 * a late corroborating source and is deliberately non-clobbering, so whatever
 * is set here survives a sidecar that carries no usable players[]. */
void ReplayPlayer_SetLiveMeta(const char* p1, int p1_rank, const char* p2, int p2_rank, long long date_ms) {
    bool adopted = false;

    if (copy_sanitized_name(p1, meta_p1_name, sizeof(meta_p1_name))) {
        have_p1_name = true;
        adopted = true;
    }
    if (copy_sanitized_name(p2, meta_p2_name, sizeof(meta_p2_name))) {
        have_p2_name = true;
        adopted = true;
    }
    if (p1_rank >= 1 && p1_rank <= 6) {
        meta_p1_rank = p1_rank;
        adopted = true;
    }
    if (p2_rank >= 1 && p2_rank <= 6) {
        meta_p2_rank = p2_rank;
        adopted = true;
    }
    if (date_ms > 0) {
        format_date_ms(date_ms, meta_date, sizeof(meta_date));
        have_date = true;
        adopted = true;
    }

    /* Log only when argv actually supplied something. main.c calls this on
     * EVERY --play-replay boot, so an unconditional line claimed a handoff on
     * every ordinary playback and printed the post-sidecar state under a
     * "handoff" label — misleading in a shuffle session's log, where the line
     * would repeat once per replay and name values the sidecar supplied. */
    if (!adopted) {
        return;
    }

    SDL_Log("replay: argv meta adopted — p1=%s[rank %d] p2=%s[rank %d] date=%s",
            have_p1_name ? meta_p1_name : "(none)", meta_p1_rank, have_p2_name ? meta_p2_name : "(none)",
            meta_p2_rank, have_date ? meta_date : "(none)");
}

bool ReplayPlayer_Init(const char* path_3sr) {
    reset_runtime_state();

    size_t size = 0;
    Uint8* data = SDL_LoadFile(path_3sr, &size);

    if (data == NULL) {
        SDL_Log("replay: cannot read '%s': %s", path_3sr, SDL_GetError());
        return false;
    }

    bool ok = false;

    do {
        if (size < REPLAY_3SR_HEADER_SIZE) {
            load_error(path_3sr, "file shorter than the v1 header (28 bytes)");
            break;
        }

        if (SDL_memcmp(data, REPLAY_3SR_MAGIC, 4) != 0) {
            load_error(path_3sr, "bad magic (want \"3SR1\")");
            break;
        }

        const Uint16 version = rd_u16le(data + 0x04);
        const Uint16 header_size = rd_u16le(data + 0x06);

        if (version != REPLAY_3SR_VERSION) {
            load_error(path_3sr, "unsupported version (want 1)");
            break;
        }

        if (header_size != REPLAY_3SR_HEADER_SIZE) {
            load_error(path_3sr, "bad header_size (want 28 for v1)");
            break;
        }

        SDL_zero(replay);
        replay.characters[0] = data[0x08];
        replay.characters[1] = data[0x09];
        replay.supers[0] = data[0x0A];
        replay.supers[1] = data[0x0B];
        replay.colors[0] = data[0x0C];
        replay.colors[1] = data[0x0D];
        replay.new_challenger = data[0x0E];
        replay.random_ix16 = rd_u16le(data + 0x10);
        replay.random_ix32 = rd_u16le(data + 0x12);
        replay.frame_count = rd_u32le(data + 0x14);
        replay.checksum_interval = rd_u16le(data + 0x18);
        replay.checksum_count = rd_u16le(data + 0x1A);

        if (data[0x0F] != 0) {
            load_error(path_3sr, "reserved pad byte at 0x0F is nonzero");
            break;
        }

        /* Setup sanity: the ranges the phase machine's cursor/color tables
         * can actually express (character_to_cursor[20], color_to_keys[13],
         * Super_Arts 0-2). Format §2 stores post-conversion 3SX ids. */
        if (replay.characters[0] > 19 || replay.characters[1] > 19 || replay.supers[0] > 2 || replay.supers[1] > 2 ||
            replay.colors[0] > 12 || replay.colors[1] > 12) {
            load_error(path_3sr, "setup block out of range (characters 0-19, supers 0-2, colors 0-12)");
            break;
        }

        if (replay.frame_count == 0) {
            load_error(path_3sr, "frame_count is 0");
            break;
        }

        /* Format §1: total size must be exact; §4.1: count formula. */
        const Uint64 expected =
            (Uint64)REPLAY_3SR_HEADER_SIZE + (Uint64)replay.frame_count * 4 + (Uint64)replay.checksum_count * 8;

        if ((Uint64)size != expected) {
            load_error(path_3sr, "size != header + frame_count*4 + checksum_count*8 (corrupt/truncated)");
            break;
        }

        if (replay.checksum_interval == 0 && replay.checksum_count != 0) {
            load_error(path_3sr, "checksum_interval == 0 but checksum_count != 0");
            break;
        }

        if (replay.checksum_interval != 0) {
            const Uint32 want = (replay.frame_count + replay.checksum_interval - 1) / replay.checksum_interval;

            if (replay.checksum_count != want) {
                load_error(path_3sr, "checksum_count != ceil(frame_count / checksum_interval)");
                break;
            }
        }

        replay.inputs = SDL_malloc((size_t)replay.frame_count * sizeof(*replay.inputs));

        if (replay.inputs == NULL) {
            load_error(path_3sr, "out of memory for input table");
            break;
        }

        /* Convert arcade -> SWK at load time (plan §2.3; format §3). Reject
         * out-of-mask source bits: they would not survive the SWK round-trip
         * the checksum recomputation depends on, so a file carrying them
         * could never verify. */
        const Uint8* words = data + REPLAY_3SR_HEADER_SIZE;
        bool words_ok = true;

        for (Uint32 f = 0; f < replay.frame_count; f++) {
            const Uint16 p1 = rd_u16le(words + (size_t)f * 4);
            const Uint16 p2 = rd_u16le(words + (size_t)f * 4 + 2);

            if ((p1 & ~ARCADE_WORD_MASK) || (p2 & ~ARCADE_WORD_MASK)) {
                SDL_Log("replay: frame %u carries bits outside the arcade input mask 0x13FF (p1=%04x p2=%04x)",
                        f,
                        p1,
                        p2);
                words_ok = false;
                break;
            }

            replay.inputs[f][0] = arcade_to_swk(p1);
            replay.inputs[f][1] = arcade_to_swk(p2);
        }

        if (!words_ok) {
            load_error(path_3sr, "unconvertible input word (see previous line)");
            break;
        }

        if (replay.checksum_count > 0) {
            replay.checksums = SDL_malloc((size_t)replay.checksum_count * sizeof(*replay.checksums));

            if (replay.checksums == NULL) {
                load_error(path_3sr, "out of memory for checksum table");
                break;
            }

            const Uint8* table = words + (size_t)replay.frame_count * 4;
            bool table_ok = true;

            for (Uint32 k = 0; k < replay.checksum_count; k++) {
                replay.checksums[k].frame = rd_u32le(table + (size_t)k * 8);
                replay.checksums[k].djb2 = rd_u32le(table + (size_t)k * 8 + 4);

                /* Format §4.1/§4.4: entry k covers frame k*interval. */
                if (replay.checksums[k].frame != (Uint32)k * replay.checksum_interval) {
                    table_ok = false;
                    break;
                }
            }

            if (!table_ok) {
                load_error(path_3sr, "checksum table frame indices are not k * checksum_interval");
                break;
            }
        }

        ok = true;
    } while (false);

    SDL_free(data);

    if (!ok) {
        SDL_free(replay.inputs);
        SDL_free(replay.checksums);
        SDL_zero(replay);
        return false;
    }

    stash_meta_sidecar_path(path_3sr);
    set_label_from_path(path_3sr);
    parse_meta_sidecar();

    loaded = true;
    status = REPLAY_PLAYER_NAVIGATING;
    SDL_Log("replay: loaded '%s' — p1 char=%u sa=%u color=%u, p2 char=%u sa=%u color=%u, "
            "new_challenger=%u, rng ix16=%04x ix32=%04x, frames=%u, checksums=%u every %u frames",
            path_3sr,
            replay.characters[0],
            replay.supers[0],
            replay.colors[0],
            replay.characters[1],
            replay.supers[1],
            replay.colors[1],
            replay.new_challenger,
            replay.random_ix16,
            replay.random_ix32,
            replay.frame_count,
            replay.checksum_count,
            replay.checksum_interval);
    return true;
}

void ReplayPlayer_Destroy(void) {
    s_stall_frame = false;

    SDL_free(replay.inputs);
    SDL_free(replay.checksums);
    SDL_zero(replay);
    loaded = false;

    has_meta_json = false;
    have_p1_name = false;
    have_p2_name = false;
    have_date = false;
    meta_p1_rank = 0;
    meta_p2_rank = 0;
    exit_hold_frames = 0;

    if (status != REPLAY_PLAYER_INACTIVE) {
        status = REPLAY_PLAYER_INACTIVE;
    }
}

bool ReplayPlayer_IsActive(void) {
    return loaded;
}

/* Step F2a — browser-driven runtime launch (see header). */
bool ReplayPlayer_LoadAndStart(const char* path_3sr) {
    if (Netplay_GetSessionState() != NETPLAY_SESSION_IDLE) {
        SDL_Log("replay: refusing browser launch of '%s' — netplay session active", path_3sr);
        return false;
    }

    /* Free any replay from a previous launch, then load + validate the new
     * one. Init resets the phase machine (reset_runtime_state) and, on
     * success, sets status = REPLAY_PLAYER_NAVIGATING. */
    ReplayPlayer_Destroy();

    if (!ReplayPlayer_Init(path_3sr)) {
        /* Init already logged the specific failure reason. */
        return false;
    }

    /* Normalize the engine to a clean title. The browser sits over the
     * attract loop (possibly mid-demo); Soft_Reset_Sub() is the same
     * back-to-title flow the hold-START abort and netplay disconnect paths
     * use (netplay.c:911-923), and it leaves the engine where the phase
     * machine's PHASE_TITLE watch (task[TASK_MENU].r_no) drives forward. The
     * session config pin (console + arcade-balance + identity buttons) was
     * applied once at boot for the whole browser session and is untouched
     * here (v1 whole-session pin — see header + ReplayPlayer_PinConfig). */
    Soft_Reset_Sub();

    /* This launch is owned by the OSD browser: on a terminal state the browser's
     * tick_launching performs the teardown (Soft_Reset_Sub + Destroy + reopen
     * list), so the player must NOT self-exit — it only freezes until Destroy.
     * Set AFTER Init (which reset it to false via reset_runtime_state). */
    s_browser_owned = true;

    SDL_Log("replay: browser launched '%s' — phase machine armed "
            "(browser-session config pin already in force: console + arcade-balance + identity)",
            path_3sr);
    return true;
}

ReplayPlayerStatus ReplayPlayer_GetStatus(void) {
    return status;
}

Uint32 ReplayPlayer_GetDesyncFrame(void) {
    return desync_frame;
}

const char* ReplayPlayer_GetMetaJsonPath(void) {
    return has_meta_json ? meta_json_path : NULL;
}

/* ---------------------------------------------------------------------- */
/* Step C2 — overlay metadata + hold-exit accessors                       */
/* ---------------------------------------------------------------------- */

const char* ReplayPlayer_GetP1Name(void) {
    return have_p1_name ? meta_p1_name : NULL;
}

const char* ReplayPlayer_GetP2Name(void) {
    return have_p2_name ? meta_p2_name : NULL;
}

int ReplayPlayer_GetP1Rank(void) {
    return meta_p1_rank;
}

int ReplayPlayer_GetP2Rank(void) {
    return meta_p2_rank;
}

const char* ReplayPlayer_GetDateString(void) {
    return have_date ? meta_date : NULL;
}

const char* ReplayPlayer_GetLabel(void) {
    return replay_label;
}

bool ReplayPlayer_ShouldShowExitHint(void) {
    if (status != REPLAY_PLAYER_PLAYING) {
        return false;
    }

    return exit_hold_frames > 0 || play_index < REPLAY_HINT_INTRO_FRAMES;
}

int ReplayPlayer_GetExitHoldFrames(void) {
    return exit_hold_frames;
}

int ReplayPlayer_GetExitHoldThreshold(void) {
    return REPLAY_EXIT_HOLD_FRAMES;
}

/* ---------------------------------------------------------------------- */
/* Session-only config pin (statcheck_runner.c:192-234)                   */
/* ---------------------------------------------------------------------- */

/* Replicated from StatcheckRunner_PinConfig / pin_default_button_mapping
 * (statcheck_runner.c review round-1 finding P-1) rather than shared,
 * because that TU is #if STATCHECK and this module must compile in every
 * flavor. Same A3b-proven co-necessities:
 * - game-mode=console: the arcade Loop_Demo path never reaches the
 *   Menu_Task r_no sequence PHASE_TITLE/PHASE_MENU watch for.
 * - arcade-balance=true: the Fightcade sessions the .3sr files derive from
 *   were captured against sfiii3nr1 arcade data tables; console tables
 *   desync from frame 1. Config_SetString only mutates the in-memory
 *   entries[] table; Config_Save() is a no-op stub — the user's on-disk
 *   config is never written.
 * - default (identity) button mapping so Convert_User_Setting passes our
 *   SWK words through Convert_Data unchanged; every save_w[] slot is
 *   pinned because Present_Mode migrates across slots (statcheck P-1).
 *
 * NOTE for Stage F2a: browser-launched playback (no process restart) will
 * need SCOPED handling — save/restore of these pins around a session —
 * instead of this process-lifetime pin. */
void ReplayPlayer_PinConfig(void) {
    static const u8 identity[8] = { 0, 1, 2, 11, 3, 4, 5, 11 };

    const bool was_arcade_mode = SDLApp_IsArcadeGameMode();
    const char* was_balance = Config_GetString(CFG_KEY_BALANCE);

    SDLApp_ForceConsoleGameMode();
    /* Upstream reconcile: the old boolean "arcade-balance" key this pin used
     * to force to "true" no longer exists. Balance now AUTO-SELECTS at boot
     * (CPS3 ROM present + full 20-character adaptation succeeds -> arcade,
     * else PS2) and CFG_KEY_BALANCE is a config-file-only override that can
     * only ever force balance DOWN to PS2 ("ps2") or leave auto-select alone
     * ("auto"); see src/port/config/config.h CFG_KEY_BALANCE and
     * ArcadeBalance_Init in src/arcade/arcade_balance.c.
     *
     * So the faithful port of "pin arcade balance on for this session" is to
     * clear any on-disk "ps2" override back to "auto" — that is the strongest
     * arcade-ward pin the current model permits, and it preserves the pin's
     * real purpose: the user's config must not change replay determinism.
     * When the CPS3 ROM is absent, arcade balance is genuinely unavailable
     * and the replay will not reproduce; that is a data problem the old
     * boolean silently mislabelled rather than solved. */
    Config_SetString(CFG_KEY_BALANCE, "auto");

    for (int mode = 0; mode < 6; mode++) {
        for (int p = 0; p < 2; p++) {
            for (int s = 0; s < 8; s++) {
                save_w[mode].Pad_Infor[p].Shot[s] = identity[s];
            }

            save_w[mode].Pad_Infor[p].Vibration = 0;
        }
    }

    SDL_Log("replay: pinned session config -- game-mode=console (was %s) "
            "balance=auto (was %s) button-mapping=default (identity, all save_w[] slots); "
            "on-disk config untouched",
            was_arcade_mode ? "arcade" : "console",
            was_balance != NULL ? was_balance : "auto");
}

/* ---------------------------------------------------------------------- */
/* Phase machine (cloned from statcheck_runner.c)                         */
/* ---------------------------------------------------------------------- */

/* Character-select cursor map — statcheck_runner.c:57-59. */
static const Uint8 character_to_cursor[20][2] = { { 7, 1 }, { 1, 0 }, { 5, 2 }, { 6, 1 }, { 3, 2 }, { 4, 0 }, { 1, 2 },
                                                  { 3, 0 }, { 2, 2 }, { 4, 2 }, { 0, 1 }, { 0, 2 }, { 2, 0 }, { 5, 0 },
                                                  { 6, 0 }, { 3, 1 }, { 2, 1 }, { 4, 1 }, { 1, 1 }, { 5, 1 } };

/* Color picker buttons — statcheck_runner.c:63-77 (upstream :35-49). */
static const SWKey color_to_keys[13] = {
    SWK_WEST,
    SWK_NORTH,
    SWK_RIGHT_SHOULDER,
    SWK_SOUTH,
    SWK_EAST,
    SWK_RIGHT_TRIGGER,
    SWK_WEST | SWK_RIGHT_SHOULDER | SWK_EAST,
    SWK_START | SWK_WEST,
    SWK_START | SWK_NORTH,
    SWK_START | SWK_RIGHT_SHOULDER,
    SWK_START | SWK_SOUTH,
    SWK_START | SWK_EAST,
    SWK_START | SWK_RIGHT_TRIGGER,
};

static void set_cursor(Uint8 character, int player) {
    Cursor_X[player] = (s8)character_to_cursor[character][0];
    Cursor_Y[player] = (s8)character_to_cursor[character][1];
}

/// Repeatedly press and release a button
static void mash_button(SWKey button, int player) {
    input_buffers[player] |= (frame_index & 1) ? button : 0;
}

static void tap_button(SWKey button, int player) {
    input_buffers[player] |= button;
}

static bool game_ended(void) {
    return (PL_Wins[0] == 2) || (PL_Wins[1] == 2);
}

/* NO synthetic inter-round skip taps here — deliberately, and do not
 * re-add them from statcheck_runner.c.
 *
 * statcheck needs its inter_round_skip_needed() because it replays a RAM
 * ARCHIVE and not an input stream: it reads the recorded C_No/Scene_Cut out
 * of the archived frame and re-creates, as SWK_ATTACKS taps, the skip the
 * recorded session performed. A .3sr viewer has no such gap to fill. It
 * injects the recorded input WORDS, and those words are exactly what made
 * the original session skip — sys_sub.c -> Cut_Cut_Cut() is nothing more
 * than "an operator is holding SWK_ATTACKS". Replaying them reproduces
 * every Scene_Cut of the recording on the frame it really happened, and on
 * no other frame.
 *
 * NEGATIVE RESULT: tapping on top of that was a feedback loop, not a skip.
 * The removed condition was `(C_No[0] == 6 && C_No[1] == 3) || (Scene_Cut
 * && C_No[0] > 6)` read off LIVE engine state, i.e. one frame BEHIND the
 * archived value statcheck tests. By the time it saw C_No == [6,3],
 * Game_Manage_7_2's Button_Cut_EX had already run and the tap could no
 * longer skip anything; all it still did was set Scene_Cut itself (game.c
 * -> Game02() re-derives Scene_Cut = Cut_Cut_Cut() from p1sw_0/p2sw_0 on
 * every in-match frame). That re-satisfied the condition's own second clause, so the taps
 * latched on through the whole round-end flow. Game_Manage_9th's `case 1`
 * (manage.c) then took its `if (Scene_Cut) C_Timer = 1;` branch and
 * collapsed a 60-frame score tally the recorded players never cut —
 * desyncing C_No immediately and Game_timer a few frames later. That was 16
 * of the 22 failures in the 44-replay comparison corpus (e.g.
 * 1784785136322-3894 at frame 3120: the CPS3 archive holds C_No == [8,1,0,0]
 * with Scene_Cut == 0 from .3sr frame 3066 to past 3123 and carries no
 * attack bits in that range, while we left [8,1] after a single frame).
 *
 * statcheck cannot hit this: it reads the ARCHIVE's SCENE_CUT_OFFSET, which
 * is 0 whenever nobody pressed, so its own taps can never feed its own
 * condition. Anything derived from LIVE Scene_Cut can, which is why the
 * skip must come from the recording and nowhere else. */

static void finish(const char* reason) {
    phase = PHASE_DONE;
    status = REPLAY_PLAYER_COMPLETE;
    /* Freeze from this very frame. finish() fires at match end (inputs
     * exhausted / game_ended: a player just hit 2 wins) — the LAST recorded
     * frame was already simulated on the prior tick, and the game's live
     * post-match flow (Game_Manage_10th) would keep running if njUserMain did.
     * Holding now (main.c skips njUserMain when we stall) stops that flow
     * before it can push the out-of-range win-pose effect WORK that trips the
     * qix trap (effect.c:229). tick_terminal keeps the hold on later ticks.
     *
     * From this frame on the screen is BLACK plus the overlay message — a held
     * frame retains nothing (see s_stall_frame). The KO and win pose have
     * already played by the time we get here: PHASE_POSTMATCH runs them live
     * first, and calls us on the tick it sees C_No[0] > 6. */
    s_stall_frame = true;
    SDL_Log("REPLAY COMPLETE frames=%u checksums=%u/%u r16_resyncs=%u%s reason=%s",
            play_index,
            checksums_passed,
            checksums_checked,
            checksums_r16_resynced,
            checksums_skipped_nonbattle ? " (plus non-battle-frame checkpoints skipped)" : "",
            reason);

    if (checksums_skipped_nonbattle) {
        SDL_Log("replay: %u checkpoint(s) fell on non-battle frames and were not verified "
                "(statcheck itself never validates position fields outside G_No[1]==2 && G_No[2]==1)",
                checksums_skipped_nonbattle);
    }

    /* v1 end-of-replay behavior: stop injecting and release the pads — the
     * post-match flow (win pose -> char select in console VERSUS mode)
     * continues naturally under player control. Multi-game replays are out
     * of scope for v1: each .3sr holds exactly one game (docs/3sr-format.md
     * preamble); a corpus game N+1 lives in its own file. */
}

/* Terminal state reached (phase == PHASE_DONE, status COMPLETE/DESYNCED/
 * ABORTED). HOLD the whole frame so the game's live post-match flow can't
 * free-run into the qix effect trap (see the s_browser_owned block comment),
 * then tear the session down per ownership. Runs every tick while terminal. */
static void tick_terminal(void) {
    /* Freeze: main.c reads ReplayPlayer_IsStallingThisFrame() and skips
     * njUserMain (and thus Game_Management -> Game_Manage_10th) this frame.
     * The screen goes BLACK with just the terminal overlay on it — nothing is
     * retained from the last battle frame (see s_stall_frame). Pads stay
     * released so nothing the frozen engine might still read is pressed. */
    s_stall_frame = true;
    p1sw_buff = 0;
    p2sw_buff = 0;

    /* Browser-owned launch: the browser's tick_launching (replay_browser.c)
     * owns teardown. We only keep freezing until it Soft_Reset_Sub + Destroys
     * us (Destroy clears s_stall_frame). Never self-exit here — that would kill
     * the whole process and drop the user out of the browser. */
    if (s_browser_owned) {
        return;
    }

    /* CLI --play-replay: no browser owns teardown. Linger
     * so the terminal overlay ("REPLAY COMPLETE" / "REPLAY DIVERGED") is fully
     * visible, then end the session with a clean process exit. SDLApp_Exit
     * pushes SDL_EVENT_QUIT; the main loop leaves next tick with exit code 0
     * (same clean-shutdown path the perf-capture + input-script completion use,
     * sdl_app.c:4015). On device the wrapper's restart_to_menu then returns to
     * the MiSTer menu cleanly — the qix crash-to-menu is gone. */
    if (terminal_linger < REPLAY_TERMINAL_LINGER_FRAMES) {
        terminal_linger += 1;
        return;
    }
    if (!terminal_exit_requested) {
        terminal_exit_requested = true;
        SDL_Log("replay: terminal state reached (status=%d) after %d-frame overlay linger — "
                "clean session exit (no post-match free-run)",
                (int)status, terminal_linger);
        SDLApp_Exit();
    }
}

void ReplayPlayer_Tick(void) {
    /* Recomputed every frame; the terminal + stall paths below set it. Cleared
     * first so a non-terminal frame never inherits a stale hold on main.c. */
    s_stall_frame = false;

    if (!loaded) {
        return;
    }

    /* Terminal: hold the frame (no post-match free-run into the qix trap) and
     * run the teardown. Replaces the old bare `return` that let the live
     * post-match state machine free-run. */
    if (phase == PHASE_DONE) {
        tick_terminal();
        return;
    }

    /* Refuse to fight a live netplay session for the input latch (same
     * session guard game_step_0 branches on, main.c:612). Covers sessions
     * started via the default-path direct-P2P handoff probe, which needs no
     * CLI flag (args.c already rejects --play-replay + netplay flags). */
    if (Netplay_GetSessionState() != NETPLAY_SESSION_IDLE) {
        phase = PHASE_DONE;
        status = REPLAY_PLAYER_ABORTED;
        SDL_Log("replay: netplay session active — aborting replay playback (pads released)");
        return;
    }

    /* Hold-START-to-exit (Step C2). p1sw_buff/p2sw_buff still carry the REAL
     * user pads written by keyConvert() this frame (main.c:604) — we read
     * them HERE, before overwriting the buffers below. Armed only while a
     * match is actually PLAYING; either player's START counts. Holding it
     * REPLAY_EXIT_HOLD_FRAMES consecutive frames aborts cleanly: release the
     * pads and Soft_Reset_Sub() back to title (netplay.c:911-923 precedent).
     * A released START resets the streak, so brief presses do nothing. */
    if (status == REPLAY_PLAYER_PLAYING) {
        const bool start_held = ((p1sw_buff | p2sw_buff) & SWK_START) != 0;

        if (start_held) {
            exit_hold_frames += 1;

            if (exit_hold_frames >= REPLAY_EXIT_HOLD_FRAMES) {
                phase = PHASE_DONE;
                status = REPLAY_PLAYER_ABORTED;
                SDL_zeroa(input_buffers);
                p1sw_buff = 0;
                p2sw_buff = 0;
                Soft_Reset_Sub();
                SDL_Log("replay: user held START %d frames at play_index %u — aborting playback, "
                        "returning to title (pads released)",
                        exit_hold_frames, play_index);
                return;
            }
        } else {
            exit_hold_frames = 0;
        }
    } else {
        exit_hold_frames = 0;
    }

    SDL_zeroa(input_buffers);

    switch (phase) {
    case PHASE_TITLE: {
        const struct _TASK* menu_task = &task[TASK_MENU];

        if (menu_task->r_no[0] == 0 && menu_task->r_no[1] == 1 && menu_task->r_no[2] == 3) {
            phase = PHASE_MENU;
            break;
        }

        mash_button(SWK_START, 0);
        break;
    }

    case PHASE_MENU:
        if (G_No[1] == 1 && G_No[2] == 2) {
            // Even though we move cursor manually later, setting Last_My_char2 is required
            // for Last_Super_Arts to take effect
            Last_My_char2[0] = (s8)replay.characters[0];
            Last_My_char2[1] = (s8)replay.characters[1];
            Last_Super_Arts[0] = (s8)replay.supers[0];
            Last_Super_Arts[1] = (s8)replay.supers[1];
            phase = PHASE_CHARACTER_SELECT_TRANSITION;
            wait_timer = 60;
            break;
        }

        mash_button(SWK_SOUTH, 0);
        break;

    case PHASE_CHARACTER_SELECT_TRANSITION:
        wait_timer -= 1;

        if (wait_timer <= 0) {
            phase = PHASE_CHARACTER_SELECT;
        }

        break;

    case PHASE_CHARACTER_SELECT:
        switch (char_select_phase) {
        case 0:
            set_cursor(replay.characters[0], 0);
            set_cursor(replay.characters[1], 1);
            tap_button(SWK_START, 1);
            wait_timer = 20;
            char_select_phase = 1;
            break;

        case 1:
            wait_timer -= 1;

            if (wait_timer <= 0) {
                // We must set New_Challenger manually so that the game selects the correct stage.
                // If we set this var earlier it would be overwritten
                New_Challenger = (s8)replay.new_challenger;
                // Restore the arcade invariant Champion == New_Challenger ^ 1 (entry.c:1326-1356).
                // Our synthetic char-select leaves Champion==0; without this, mirror matches with
                // recorded New_Challenger==0 desync at battle start via home_visitor_check()
                // (appear.c app_type_tbl vs app_type_tbl2). Paired with the same line in the
                // statcheck gate — the player and the gate must agree. (upstream #289)
                Champion = New_Challenger ^ 1;
                char_select_phase = 2;
            }

            break;

        case 2:
            tap_button(color_to_keys[replay.colors[0]], 0);
            tap_button(color_to_keys[replay.colors[1]], 1);
            wait_timer = 45;
            char_select_phase = 3;
            break;

        case 3:
            wait_timer -= 1;

            if (wait_timer <= 0) {
                tap_button(SWK_SOUTH, 0);
                tap_button(SWK_SOUTH, 1);
                phase = PHASE_GAME_TRANSITION;
            }

            break;
        }

        break;

    case PHASE_GAME_TRANSITION:
        if (G_No[1] != 2) {
            // This skips the VS animation
            mash_button(SWK_ATTACKS, 0);
            break;
        }

        /* RNG seed at the exact point statcheck syncs (statcheck_runner.c:
         * 341-346): once G_No[1] == 2, from the pre-game values — the .3sr
         * header carries the archive's frame `start_index - 1` values
         * (docs/3sr-format.md §2 "RNG sync frame"). Seeding earlier is the
         * known frame-1-desync trap (plan A3b "If it fails"). */
        Random_ix16 = (s16)replay.random_ix16;
        Random_ix32 = (s16)replay.random_ix32;
        status = REPLAY_PLAYER_PLAYING;
        phase = PHASE_GAME;
        /* fallthrough */

    case PHASE_GAME:
        if (play_index >= replay.frame_count) {
            finish("inputs-exhausted");
            return;
        }

        if (game_ended()) {
            /* A .3sr holds exactly one match (docs/3sr-format.md preamble):
             * once local PL_Wins reaches 2 the recorded match is over by
             * definition. Any checkpoints beyond this point are the tracker's
             * post-KO win-pose tail (RAM game-state G_No[1] stays == 2 through
             * the whole win pose, so it keeps emitting checkpoints there —
             * see docs/plan-fix-diverged-falsepositive.md), not further
             * battle — so they never indicate a real divergence here.
             *
             * Rather than finish() on this very frame — which froze the engine
             * on the winning hit — hand it a bounded post-match window so the
             * KO and win pose actually play (REPLAY_POSTMATCH_MAX_FRAMES block
             * comment). finish() still runs, just later and unchanged. */
            phase = PHASE_POSTMATCH;
            postmatch_frames = 0;
            /* Neutral pads this frame (input_buffers were zeroed above) and
             * the engine ticks normally; the window is counted from the next
             * tick, in case PHASE_POSTMATCH below. */
            break;
        }

        pure_words[0] = replay.inputs[play_index][0];
        pure_words[1] = replay.inputs[play_index][1];
        input_buffers[0] = pure_words[0];
        input_buffers[1] = pure_words[1];

        /* Nothing is added on top of the recorded words — see the
         * "NO synthetic inter-round skip taps" block comment near
         * game_ended() for why. */

        break;

    case PHASE_POSTMATCH:
        /* Pads stay neutral (input_buffers were zeroed above) and the play
         * cursor stays put — Epilogue only checkpoints/advances in PHASE_GAME,
         * so the recording's post-KO tail is never compared against the win
         * pose we are letting run. The KO and win pose are engine-driven, not
         * input-driven, so neutral is all they need. */
        postmatch_frames += 1;
        if (C_No[0] > 6 || postmatch_frames >= REPLAY_POSTMATCH_MAX_FRAMES) {
            SDL_Log("replay: round-end tail played for %d frame(s) (C_No[0]=%u, cap %d) — freezing now",
                    postmatch_frames, C_No[0], REPLAY_POSTMATCH_MAX_FRAMES);
            finish("game-ended");
            return;
        }

        break;

    case PHASE_DONE:
        return;
    }

    /* Fork-style injection (statcheck_runner.c:369-374): overwrite the
     * pre-latch buffers; game_step_0 latches p1sw_0 = p1sw_buff right after
     * this hook returns. */
    p1sw_buff = input_buffers[0];
    p2sw_buff = input_buffers[1];
}

/* ---------------------------------------------------------------------- */
/* Divergence detector (docs/3sr-format.md §4)                            */
/* ---------------------------------------------------------------------- */

/* Recompute the 13-field window from live engine state. Field sources are
 * the live counterparts statcheck_compare.c reads for the same archive
 * offsets (format §4.2 table):
 *   1-4  C_No[0..3]                (compare_service_values)
 *   5    Game_timer
 *   6-7  Random_ix16 / Random_ix32
 *   8-11 plw[i].wu.xyz[0/1].disp.pos  (get_position, statcheck_compare.c:98-101)
 *   12-13 this frame's P1SW_0/P2SW_0 — recovered by inverting the load-time
 *        arcade->SWK conversion on the words injected this tick (the archived
 *        P1SW_0 word IS the .3sr input word, format §4.2 note; live p1sw_0
 *        would additionally carry the local pads, which the original archive
 *        never saw).
 * Canonicalization (format §4.3): each 16-bit value as 2 bytes LE,
 * concatenated in table order (26 bytes), hashed with the codebase's
 * additive djb2 (djb2_hash.h, verbatim — same fn netplay's desync detector
 * uses). */
static void gather_live_fields(Uint16 fields[13]) {
    const Uint16 live[13] = {
        (Uint16)C_No[0],
        (Uint16)C_No[1],
        (Uint16)C_No[2],
        (Uint16)C_No[3],
        Game_timer,
        (Uint16)Random_ix16,
        (Uint16)Random_ix32,
        (Uint16)plw[0].wu.xyz[0].disp.pos,
        (Uint16)plw[0].wu.xyz[1].disp.pos,
        (Uint16)plw[1].wu.xyz[0].disp.pos,
        (Uint16)plw[1].wu.xyz[1].disp.pos,
        swk_to_arcade(pure_words[0]),
        swk_to_arcade(pure_words[1]),
    };

    SDL_memcpy(fields, live, sizeof(live));
}

static Uint32 hash_fields(const Uint16 fields[13]) {
    Uint8 canon[26];

    for (int i = 0; i < 13; i++) {
        canon[i * 2] = (Uint8)(fields[i] & 0xFF);
        canon[i * 2 + 1] = (Uint8)(fields[i] >> 8);
    }

    return djb2_update_mem(djb2_init(), canon, sizeof(canon));
}

/* Random_ix16 recovery (field 6 of the checksum window, 0-based index 5).
 *
 * The statcheck oracle only matches the archives on Random_ix16 because it
 * force-syncs the live variable from the archive EVERY frame
 * (statcheck_compare.c:239 — "This is dirty, but syncing Random_ix16 every
 * frame helps avoid animation-related desyncs"): random_16() is consumed
 * mostly by visual-effect modules (src/sf33rd/Source/Game/effect) whose
 * call counts do not track CPS3 exactly, so the live index drifts by a few
 * steps over hundreds of frames. NOTE it is NOT gameplay-free: at least
 * the dizzy/stun duration (kizetsu_timer_table lookup, plpdm.c:893) and
 * some AI pattern picks (plpat09.c) consume random_16(). A drift that
 * bites one of those changes recovery frames -> positions/timer, which
 * the NEXT battle checkpoint's 12-field check catches as a hard desync —
 * i.e. gameplay-affecting ix16 drift is DETECTED, never silently rendered.
 * (Random_ix32 — asserted, never synced — stays matched; so do
 * timer/C_No/positions in the benign visual-only case.)
 * Observed on the first C1 runs: 15/15 checkpoints clean, then live
 * ix16=0004 vs archive 0002 at f=960 with the other 12 fields identical.
 *
 * We cannot replicate the per-frame sync (the .3sr carries only sparse
 * hashes, not per-frame values) — but the archive's Random_ix16 at a
 * checkpoint is RECOVERABLE from the hash: the other 12 fields are known
 * live, so sweep the single unknown 16-bit field and accept the FIRST
 * hash-matching candidate. djb2's ix16 contribution is many-to-one
 * (~8-15 candidates collide onto each achievable value), so recovery is
 * best-effort: the low bits of the applied value may differ from the true
 * archive value, and a resulting between-checkpoint divergence is again
 * caught at the next checkpoint. The sweep still verifies the 12 known
 * fields essentially as strictly as an exact compare (any second
 * divergent field -> no candidate matches -> hard desync, up to ~2^-16
 * residual collision odds against the 32-bit hash) and re-applies the
 * oracle's dirty sync at checkpoint granularity.
 * Cost: one 64K x 26-byte djb2 sweep, only on mismatch. */
static bool recover_random_ix16(Uint16 fields[13], Uint32 want, Uint16* recovered) {
    for (Uint32 cand = 0; cand <= 0xFFFF; cand++) {
        fields[5] = (Uint16)cand;

        if (hash_fields(fields) == want) {
            *recovered = (Uint16)cand;
            return true;
        }
    }

    return false;
}

static void log_live_fields(const char* tag) {
    SDL_Log("replay: %s C=[%u,%u,%u,%u] timer=%u ix16=%04x ix32=%04x "
            "p1=(%d,%d) p2=(%d,%d) sw=[%04x,%04x] G=[%u,%u,%u,%u]",
            tag,
            C_No[0],
            C_No[1],
            C_No[2],
            C_No[3],
            Game_timer,
            (Uint16)Random_ix16,
            (Uint16)Random_ix32,
            plw[0].wu.xyz[0].disp.pos,
            plw[0].wu.xyz[1].disp.pos,
            plw[1].wu.xyz[0].disp.pos,
            plw[1].wu.xyz[1].disp.pos,
            swk_to_arcade(pure_words[0]),
            swk_to_arcade(pure_words[1]),
            G_No[0],
            G_No[1],
            G_No[2],
            G_No[3]);
}

static void check_checkpoint(void) {
    if (replay.checksum_interval == 0) {
        return;
    }

    if (play_index % replay.checksum_interval != 0) {
        return;
    }

    const Uint32 k = play_index / replay.checksum_interval;

    if (k >= replay.checksum_count) {
        /* Just the final partial window — silent, as ever. */
        return;
    }

    /* Battle-lock gate: the checksum window includes the player position
     * fields, which statcheck itself only ever validates while
     * G_No[1]==2 && G_No[2]==1 (compare_characters, statcheck_compare.c:
     * 445-450) — outside battle (pre-round intro, win poses, continue
     * scenes) the archived plw bytes are unvalidated stale/scene state the
     * live engine has no contract to reproduce. Checkpoints landing on such
     * frames are computed and logged but not failed. */
    const bool battle_frame = (G_No[1] == 2) && (G_No[2] == 1);

    Uint16 fields[13];
    gather_live_fields(fields);
    const Uint32 live = hash_fields(fields);
    const Uint32 want = replay.checksums[k].djb2;

    if (live == want) {
        checksums_checked += 1;
        checksums_passed += 1;
        SDL_Log("replay: checksum %u/%u ok at frame %u", k + 1, replay.checksum_count, play_index);
        return;
    }

    if (battle_frame) {
        Uint16 recovered = 0;

        if (recover_random_ix16(fields, want, &recovered)) {
            checksums_checked += 1;
            checksums_passed += 1;
            checksums_r16_resynced += 1;
            SDL_Log("replay: checksum %u/%u ok at frame %u (Random_ix16-only divergence: live=%04x "
                    "archive=%04x, other 12 fields verified; resynced — the statcheck oracle "
                    "dirty-syncs this field every frame, statcheck_compare.c:239)",
                    k + 1,
                    replay.checksum_count,
                    play_index,
                    (Uint16)Random_ix16,
                    recovered);
            Random_ix16 = (s16)recovered;
            return;
        }
    }

    if (!battle_frame) {
        checksums_skipped_nonbattle += 1;
        SDL_Log("replay: checksum %u/%u at frame %u skipped (non-battle frame, G=[%u,%u,%u,%u]; "
                "position fields unvalidated there — informational, live=%08x want=%08x)",
                k + 1,
                replay.checksum_count,
                play_index,
                G_No[0],
                G_No[1],
                G_No[2],
                G_No[3],
                live,
                want);
        log_live_fields("non-battle checkpoint state");
        return;
    }

    checksums_checked += 1;
    desync_frame = play_index;
    phase = PHASE_DONE;
    status = REPLAY_PLAYER_DESYNCED;
    /* The replay's own identity is part of the record: this message was
     * written when exactly one replay was ever in play, so it named the
     * checkpoint and both hashes but not WHICH recording diverged. In a
     * shuffle session that is the one thing the line was missing. (Which
     * FIELD diverged is still not knowable here — the .3sr stores a single
     * 32-bit djb2 per checkpoint over the 13-field window, not per-field
     * values; log_live_fields below dumps every live value instead.) */
    SDL_Log("REPLAY DESYNC at frame %u (checkpoint %u/%u: live=%08x want=%08x) in replay '%s' (%s vs %s) — "
            "stopping input injection",
            play_index,
            k + 1,
            replay.checksum_count,
            live,
            want,
            replay_label,
            have_p1_name ? meta_p1_name : "(unknown)",
            have_p2_name ? meta_p2_name : "(unknown)");
    log_live_fields("desync live state");
}

void ReplayPlayer_Epilogue(void) {
    if (!loaded) {
        return;
    }

    if (phase == PHASE_GAME && !s_stall_frame) {
        /* Same tick boundary as StatcheckRunner_Epilogue: after njUserMain
         * (game_step_0) and before Interrupt_Timer/Scrn_Renew — the archived
         * frames the checksums were computed from were captured there.
         * A STALLED frame injected nothing and skipped the engine tick
         * entirely (main.c holds the frame), so neither the checkpoint nor
         * the play cursor may advance. */
        check_checkpoint();
        play_index += 1;
    }

    frame_index += 1;
}
