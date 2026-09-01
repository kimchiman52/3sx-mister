/*
 * netplay_launcher.c — task #157: desktop netplay launcher.
 *
 * A second CMake target (`3s-netplay-launcher`), NOT compiled into the
 * game — CMakeLists.txt filters src/launcher/ out of GAME_SRC the same
 * way it filters netplay_stub.c. Desktop only (Windows/Mac/Linux); the
 * MiSTer OSD already covers hosting/joining on device and is out of
 * scope here.
 *
 * What it does: hosts or joins a direct-P2P game without hand-writing a
 * handoff file. The handoff file is already a control API — the game
 * self-navigates from a cold launch when spawned with
 * `--direct-p2p-handoff <path>` (src/netplay/direct_p2p_handoff.h,
 * netplay_nav.h) — so this launcher only writes that file, spawns the
 * game, and (for hosting) reads the room code back from
 * <pref>/netplay.status, the machine-readable status file
 * direct_p2p.c's netplay_status_publish() maintains. The code in that
 * file is deliberately unredacted: it is local to the user's own pref
 * dir and exists precisely so this launcher does not screen-scrape.
 *
 * Zero new dependencies by construction: rendering is
 * SDL_RenderDebugText (SDL3's built-in 8x8 debug font — independent of
 * the game's proportional font atlas, so bug #156's corrupt '1' glyph
 * cannot reach us), spawning is SDL_CreateProcess (portable — no
 * per-platform fork/exec), clipboard is SDL_SetClipboardText, and the
 * only game code linked in is room_code.c (+ csprng.c it needs) and
 * paths.c (so the pref dir resolves IDENTICALLY to the game's,
 * THIRDSARM_HOME override included).
 *
 * The key-binding screen edits <pref>/keymap through keymap.c's own
 * editing API rather than a second copy of the defaults table, so the
 * launcher and the game can never disagree about what a default is.
 *
 * A pasted code is validated with RoomCode_Decode BEFORE spawning, so a
 * typo reports "invalid room code" here instead of costing a 15 s punch
 * at an uninvolved stranger's address — and the decoder's distinct
 * OLD_FORMAT / FUTURE_VERSION outcomes surface as distinct, actionable
 * messages, which is the whole point of the enum return (room_code.h).
 */

#include "netplay/room_code.h"
#include "port/config/keymap.h"
#include "port/paths.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Layout constants. Window is fixed 640x480; text is drawn through
 * draw_text() which scales SDL_RenderDebugText's 8px glyphs. */
#define WIN_W 640
#define WIN_H 480
#define GLYPH 8.0f

#define STATUS_POLL_MS 500

typedef enum {
    SCREEN_MENU,
    SCREEN_HOST,
    SCREEN_JOIN,
    SCREEN_LAN,
    SCREEN_KEYS,
} Screen;

static SDL_Window* g_win = NULL;
static SDL_Renderer* g_ren = NULL;
static Screen g_screen = SCREEN_MENU;
static bool g_quit = false;

/* Path to the game binary, resolved once at startup. */
static char g_game_path[1024] = { 0 };

/* Spawned game process (at most one). The handle is kept so we can
 * report "game exited"; we never kill it — the process IS the match. */
static SDL_Process* g_child = NULL;
static bool g_child_exited = false;
static int g_child_exit_code = 0;

/* Host screen state. */
static char g_host_code[ROOM_CODE_BUF_LEN] = { 0 }; /* validated, shown big */
static char g_host_status[64] = { 0 };              /* raw line 1 of the file */
static bool g_code_copied = false;
static Uint64 g_next_poll_ms = 0;

/* Join screen state. */
static char g_code_entry[32] = { 0 };

/* LAN screen state. */
static char g_ip_entry[64] = { 0 };
static int g_lan_player = 1;

/* Key screen state. -1 = not capturing; otherwise the g_key_rows index
 * whose next keypress becomes its binding. */
static int g_key_capture = -1;
static bool g_keymap_loaded = false;

/* One-line (plus optional second line) message area, per screen. */
static char g_msg[128] = { 0 };
static char g_msg2[128] = { 0 };
static SDL_Color g_msg_color = { 255, 255, 255, 255 };

static const SDL_Color COL_TEXT = { 230, 230, 235, 255 };
static const SDL_Color COL_DIM = { 150, 150, 160, 255 };
static const SDL_Color COL_ACCENT = { 255, 208, 64, 255 };
static const SDL_Color COL_OK = { 96, 220, 128, 255 };
static const SDL_Color COL_ERR = { 255, 96, 96, 255 };

/* ---------------------------------------------------------------------- */
/* Small drawing helpers. */

static void draw_text(float x, float y, float scale, SDL_Color c, const char* s) {
    SDL_SetRenderScale(g_ren, scale, scale);
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    SDL_RenderDebugText(g_ren, x / scale, y / scale, s);
    SDL_SetRenderScale(g_ren, 1.0f, 1.0f);
}

/* Widest a line of text may be before it starts leaving the window. */
#define TEXT_MAX_W ((float)WIN_W - 32.0f)

/* Fits `s` into `max_w`: steps the scale down towards 1.0 first, then
 * truncates with an ellipsis. Returns the scale to draw `out` at.
 *
 * Every text draw goes through this because the window is fixed at 640px
 * and a message only has to run a few words long to bleed off both edges
 * -- "Game starting - waiting for the room code..." is 43 chars, which at
 * the message area's scale 2 is 688px. Shortening the offending string is
 * not a fix; the next one just does it again. */
static float fit_text(char* out, size_t out_len, const char* s, float desired, float max_w) {
    float scale = desired;
    while (scale > 1.0f && (float)strlen(s) * GLYPH * scale > max_w) {
        scale -= 0.5f;
    }
    if (scale < 1.0f) {
        scale = 1.0f;
    }

    size_t max_chars = (size_t)(max_w / (GLYPH * scale));
    if (max_chars > out_len - 1) {
        max_chars = out_len - 1;
    }

    if (strlen(s) <= max_chars) {
        SDL_strlcpy(out, s, out_len);
    } else if (max_chars > 3) {
        SDL_memcpy(out, s, max_chars - 3);
        SDL_strlcpy(out + max_chars - 3, "...", out_len - (max_chars - 3));
    } else {
        SDL_strlcpy(out, s, max_chars + 1);
    }

    return scale;
}

/* Returns the drawn width, so callers that place something after the
 * text (the entry caret) stay aligned with what was actually drawn. */
static float draw_text_fit(float x, float y, float desired, SDL_Color c, const char* s, float max_w) {
    char buf[192];
    const float scale = fit_text(buf, sizeof(buf), s, desired, max_w);
    draw_text(x, y, scale, c, buf);
    return (float)strlen(buf) * GLYPH * scale;
}

static void draw_text_centered(float cx, float y, float desired, SDL_Color c, const char* s,
                               float max_w) {
    char buf[192];
    const float scale = fit_text(buf, sizeof(buf), s, desired, max_w);
    const float w = (float)strlen(buf) * GLYPH * scale;
    draw_text(cx - w / 2.0f, y, scale, c, buf);
}

static float g_mouse_x = 0.0f;
static float g_mouse_y = 0.0f;
static bool g_mouse_clicked = false; /* set for one frame on button-up */

typedef struct {
    SDL_FRect rect;
    const char* label;
} Button;

/* Draws the button and returns true when it was clicked this frame. */
static bool button(const Button* b) {
    const bool hover = g_mouse_x >= b->rect.x && g_mouse_x < b->rect.x + b->rect.w &&
                       g_mouse_y >= b->rect.y && g_mouse_y < b->rect.y + b->rect.h;

    if (hover) {
        SDL_SetRenderDrawColor(g_ren, 82, 82, 118, 255);
    } else {
        SDL_SetRenderDrawColor(g_ren, 52, 52, 74, 255);
    }
    SDL_RenderFillRect(g_ren, &b->rect);
    SDL_SetRenderDrawColor(g_ren, 120, 120, 150, 255);
    SDL_RenderRect(g_ren, &b->rect);

    draw_text_centered(b->rect.x + b->rect.w / 2.0f,
                       b->rect.y + (b->rect.h - GLYPH * 2.0f) / 2.0f,
                       2.0f, COL_TEXT, b->label, b->rect.w - 8.0f);

    return hover && g_mouse_clicked;
}

static void set_msg(SDL_Color color, const char* line1, const char* line2) {
    SDL_strlcpy(g_msg, line1 != NULL ? line1 : "", sizeof(g_msg));
    SDL_strlcpy(g_msg2, line2 != NULL ? line2 : "", sizeof(g_msg2));
    g_msg_color = color;
}

/* ---------------------------------------------------------------------- */
/* Paths and files. */

static char* status_file_path(void) {
    char* path = NULL;
    SDL_asprintf(&path, "%snetplay.status", Paths_GetPrefPath());
    return path;
}

static char* handoff_file_path(void) {
    char* path = NULL;
    SDL_asprintf(&path, "%slauncher.handoff", Paths_GetPrefPath());
    return path;
}

static bool write_whole_file(const char* path, const char* content) {
    SDL_IOStream* io = SDL_IOFromFile(path, "w");
    if (io == NULL) {
        return false;
    }
    const size_t len = strlen(content);
    const bool ok = SDL_WriteIO(io, content, len) == len;
    SDL_CloseIO(io);
    return ok;
}

/* Overwrite netplay.status with IDLE before a host spawn, so anything
 * the poll loop later reads as HOSTING was necessarily written by the
 * child we just spawned — a code left behind by an earlier crashed run
 * (the one lifecycle hole the game's own teardown clearing cannot
 * cover) can never be re-read. The game clears it at DirectP2P_Init
 * too; either side alone closes the stale-read window, both together
 * make it belt-and-braces. */
static void clear_status_file(void) {
    char* path = status_file_path();
    if (path != NULL) {
        write_whole_file(path, "IDLE\n\n");
        SDL_free(path);
    }
}

/* Reads <pref>/netplay.status into (state, code). Missing file is not
 * an error — state comes back empty. */
static void read_status_file(char* state, size_t state_len, char* code, size_t code_len) {
    state[0] = '\0';
    code[0] = '\0';

    char* path = status_file_path();
    if (path == NULL) {
        return;
    }
    size_t size = 0;
    char* data = (char*)SDL_LoadFile(path, &size);
    SDL_free(path);
    if (data == NULL) {
        return;
    }

    /* Two newline-terminated lines; tolerate a torn/partial write (the
     * writer is not atomic) — a garbled read simply fails RoomCode
     * validation and the next poll re-reads. */
    char* nl = strchr(data, '\n');
    if (nl != NULL) {
        *nl = '\0';
        SDL_strlcpy(state, data, state_len);
        char* second = nl + 1;
        char* nl2 = strchr(second, '\n');
        if (nl2 != NULL) {
            *nl2 = '\0';
        }
        SDL_strlcpy(code, second, code_len);
    }
    SDL_free(data);
}

/* ---------------------------------------------------------------------- */
/* Game process. */

/* Resolve the game binary: --game <path> wins, then THIRDSARM_GAME, then
 * well-known names next to the launcher (SDL_GetBasePath). */
static bool find_game_binary(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--game") == 0) {
            SDL_strlcpy(g_game_path, argv[i + 1], sizeof(g_game_path));
            return true;
        }
    }

    const char* env = SDL_getenv("THIRDSARM_GAME");
    if (env != NULL && env[0] != '\0') {
        SDL_strlcpy(g_game_path, env, sizeof(g_game_path));
        return true;
    }

    const char* base = SDL_GetBasePath();
    if (base == NULL) {
        base = "";
    }
    /* On macOS the launcher IS 3S-ARM.app's CFBundleExecutable, so
     * double-clicking the app opens this window and the game binary is the
     * file sitting beside us in Contents/MacOS. SDL_GetBasePath() inside a
     * bundle is Contents/Resources (the default the game relies on to find
     * a romset and SF33RD.AFS dropped into the app), hence the hop up and
     * across rather than a plain sibling name. */
    static const char* const candidates[] = {
        "../MacOS/3S-ARM",                  /* macOS, inside our own bundle */
        "3S-ARM.app/Contents/MacOS/3S-ARM", /* macOS bundle next to us */
        "3s-arm",                           /* Linux */
        "3s-arm.exe",                       /* Windows */
        "3S-ARM",                           /* macOS non-bundle */
    };
    for (size_t i = 0; i < SDL_arraysize(candidates); i++) {
        char probe[1024];
        SDL_snprintf(probe, sizeof(probe), "%s%s", base, candidates[i]);
        SDL_PathInfo info;
        if (SDL_GetPathInfo(probe, &info) && info.type == SDL_PATHTYPE_FILE) {
            SDL_strlcpy(g_game_path, probe, sizeof(g_game_path));
            return true;
        }
    }
    return false;
}

static bool child_running(void) {
    if (g_child == NULL || g_child_exited) {
        return false;
    }
    int code = 0;
    if (SDL_WaitProcess(g_child, false, &code)) {
        g_child_exited = true;
        g_child_exit_code = code;
        return false;
    }
    return true;
}

/* Spawn the game with the given extra args (NULL-terminated tail is
 * built here). Returns false with a message set on failure. */
static bool spawn_game(const char* const* extra, int nextra) {
    if (child_running()) {
        set_msg(COL_ERR, "The game is already running.", "Close it before starting another.");
        return false;
    }
    if (g_child != NULL) {
        SDL_DestroyProcess(g_child);
        g_child = NULL;
    }
    g_child_exited = false;
    g_child_exit_code = 0;

    const char* args[16];
    int n = 0;
    args[n++] = g_game_path;
    for (int i = 0; i < nextra && n < (int)SDL_arraysize(args) - 1; i++) {
        args[n++] = extra[i];
    }
    args[n] = NULL;

    g_child = SDL_CreateProcess(args, false);
    if (g_child == NULL) {
        set_msg(COL_ERR, "Could not start the game:", SDL_GetError());
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* Mode wiring. */

/* Offline play: the double-click-the-game path verbatim. No handoff file
 * and no arguments, because there is no second player to agree with about
 * anything. It matters on macOS, where this launcher is 3S-ARM.app's
 * CFBundleExecutable and is therefore the only thing opening the app can
 * reach — without it there would be no way to just play. */
static bool start_play(void) {
    const bool ok = spawn_game(NULL, 0);
    if (ok) {
        set_msg(COL_TEXT, "Game starting...", NULL);
    }
    return ok;
}

static bool start_host(void) {
    /* #157 review P-2: the refusal must come BEFORE the clear. Clearing first
     * wiped the status file of a game that was genuinely still hosting, and
     * since the game only republishes on a state transition or a drift
     * re-encode, the launcher then waited forever for a code that had already
     * been published. */
    if (child_running()) {
        set_msg(COL_ERR, "The game is already running.", "Close it before starting another.");
        return false;
    }

    clear_status_file();
    g_host_code[0] = '\0';
    g_host_status[0] = '\0';
    g_code_copied = false;
    g_next_poll_ms = 0;

    char* handoff = handoff_file_path();
    if (handoff == NULL || !write_whole_file(handoff, "mode=host\nport=0\n")) {
        SDL_free(handoff);
        set_msg(COL_ERR, "Could not write the handoff file.", NULL);
        return false;
    }

    const char* extra[] = { "--direct-p2p-handoff", handoff };
    const bool ok = spawn_game(extra, 2);
    SDL_free(handoff);
    if (ok) {
        set_msg(COL_TEXT, "Game starting - waiting for the room code...", NULL);
    }
    return ok;
}

/* Validate + dispatch a join. The decode runs BEFORE any spawn: a bad
 * code must cost an error line here, not a 15 s hole-punch at whoever
 * owns the mistyped address. */
static void start_join(void) {
    if (g_code_entry[0] == '\0') {
        set_msg(COL_ERR, "Enter a room code first.", NULL);
        return;
    }

    uint32_t ip_be = 0;
    uint16_t port = 0;
    switch (RoomCode_Decode(g_code_entry, &ip_be, &port)) {
    case ROOM_CODE_OK:
        break;
    case ROOM_CODE_MALFORMED:
        SDL_Log("[launcher] join refused: ROOM_CODE_MALFORMED");
        set_msg(COL_ERR, "Invalid room code - check for typos.",
                "Codes are 12 letters/digits.");
        return;
    case ROOM_CODE_OLD_FORMAT:
        SDL_Log("[launcher] join refused: ROOM_CODE_OLD_FORMAT");
        set_msg(COL_ERR, "This code is from an OLDER game version.",
                "Ask the host to update, then use their new code.");
        return;
    case ROOM_CODE_FUTURE_VERSION:
        SDL_Log("[launcher] join refused: ROOM_CODE_FUTURE_VERSION");
        /* #157 review P-2: RoomCode_Decode gates the version char BEFORE the
         * checksum, so ANY 12-char string not starting with '4' lands here --
         * including a mis-heard first character, which is a typo, not a version
         * skew. Do not tell the user to update on that evidence alone. */
        set_msg(COL_ERR, "Code not recognised - it may be from a NEWER game version.",
                "Check the first character, or ask the host to confirm the code.");
        return;
    default:
        SDL_Log("[launcher] join refused: unrecognized decode result");
        set_msg(COL_ERR, "Could not read that room code.", NULL);
        return;
    }
    SDL_Log("[launcher] join code validated OK - spawning the game");

    char* handoff = handoff_file_path();
    char content[128];
    SDL_snprintf(content, sizeof(content), "mode=join\npeer_code=%s\n", g_code_entry);
    if (handoff == NULL || !write_whole_file(handoff, content)) {
        SDL_free(handoff);
        set_msg(COL_ERR, "Could not write the handoff file.", NULL);
        return;
    }

    const char* extra[] = { "--direct-p2p-handoff", handoff };
    const bool ok = spawn_game(extra, 2);
    SDL_free(handoff);
    if (ok) {
        set_msg(COL_OK, "Game launched - connecting to the host.",
                "Watch the game window for progress.");
    }
}

static void start_lan(void) {
    if (g_ip_entry[0] == '\0') {
        set_msg(COL_ERR, "Enter the other player's IP address first.", NULL);
        return;
    }
    char player_str[8];
    SDL_snprintf(player_str, sizeof(player_str), "%d", g_lan_player);
    const char* extra[] = { "--p2p-remote-ip", g_ip_entry, "--p2p-local-player", player_str };
    if (spawn_game(extra, 4)) {
        set_msg(COL_OK, "Game launched - connecting over LAN.",
                "The other player picks the other player number.");
    }
}

/* Host screen poll: read netplay.status on a cadence; when a valid code
 * appears (or a drift re-encode replaces it), show it and copy it to
 * the clipboard. */
static void host_poll(void) {
    const Uint64 now = SDL_GetTicks();
    if (now < g_next_poll_ms) {
        return;
    }
    g_next_poll_ms = now + STATUS_POLL_MS;

    char state[32];
    char code[64];
    read_status_file(state, sizeof(state), code, sizeof(code));
    SDL_strlcpy(g_host_status, state, sizeof(g_host_status));

    /* #157 review P-1: netplay.status is written by the game, and NOTHING on
     * the game's own shutdown path clears it -- a graceful quit (Cmd+Q) while
     * hosting leaves HOSTING+code on disk. Polling the file alone would keep
     * presenting a dead endpoint as live, and the user would hand that code to
     * someone who then punches a stranger's address for 15 s. The child's
     * liveness is the authority here, not the file: a code is live only while
     * the process that published it is still running. */
    if (!child_running()) {
        if (g_host_code[0] != '\0') {
            g_host_code[0] = '\0';
            g_code_copied = false;
            set_msg(COL_ERR, "The game exited - that room code is no longer valid.",
                    "Start hosting again to get a new one.");
        }
        SDL_strlcpy(g_host_status, "IDLE", sizeof(g_host_status));
        return;
    }

    if (strcmp(state, "HOSTING") == 0) {
        uint32_t ip_be = 0;
        uint16_t port = 0;
        if (RoomCode_Decode(code, &ip_be, &port) == ROOM_CODE_OK &&
            strcmp(code, g_host_code) != 0) {
            SDL_strlcpy(g_host_code, code, sizeof(g_host_code));
            g_code_copied = SDL_SetClipboardText(g_host_code);
            set_msg(COL_OK,
                    g_code_copied ? "Room code copied to the clipboard."
                                  : "Room code ready (clipboard copy failed).",
                    "Send it to the other player.");
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Text entry. */

static void entry_append_filtered(char* buf, size_t buflen, const char* text, bool code_chars) {
    for (const char* p = text; *p != '\0'; p++) {
        const char c = *p;
        bool ok;
        if (code_chars) {
            /* Room-code chars; the decoder tolerates dashes/spaces. */
            ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') || c == '-' || c == ' ';
        } else {
            /* IP / hostname chars. */
            ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') || c == '.' || c == ':' || c == '-';
        }
        const size_t len = strlen(buf);
        if (ok && len + 1 < buflen) {
            buf[len] = c;
            buf[len + 1] = '\0';
        }
    }
}

static char* active_entry_buf(void) {
    if (g_screen == SCREEN_JOIN) {
        return g_code_entry;
    }
    if (g_screen == SCREEN_LAN) {
        return g_ip_entry;
    }
    return NULL;
}

static size_t active_entry_cap(void) {
    return g_screen == SCREEN_JOIN ? sizeof(g_code_entry) : sizeof(g_ip_entry);
}

static void enter_screen(Screen s) {
    g_screen = s;
    g_key_capture = -1;
    set_msg(COL_TEXT, "", NULL);
    if (s == SCREEN_JOIN || s == SCREEN_LAN) {
        SDL_StartTextInput(g_win);
    } else {
        SDL_StopTextInput(g_win);
    }
}

/* ---------------------------------------------------------------------- */
/* Screens. */

static void render_child_state(float y) {
    if (g_child == NULL) {
        return;
    }
    if (child_running()) {
        draw_text_fit(24, y, 1.0f, COL_DIM, "game process: running", TEXT_MAX_W);
    } else {
        char line[64];
        SDL_snprintf(line, sizeof(line), "game process: exited (code %d)", g_child_exit_code);
        draw_text_fit(24, y, 1.0f, COL_DIM, line, TEXT_MAX_W);
    }
}

static void render_msg(float y) {
    if (g_msg[0] != '\0') {
        draw_text_centered(WIN_W / 2.0f, y, 2.0f, g_msg_color, g_msg, TEXT_MAX_W);
    }
    if (g_msg2[0] != '\0') {
        draw_text_centered(WIN_W / 2.0f, y + 24, 1.0f, COL_DIM, g_msg2, TEXT_MAX_W);
    }
}

static void screen_menu(void) {
    draw_text_centered(WIN_W / 2.0f, 36, 3.0f, COL_ACCENT, "3S NETPLAY", TEXT_MAX_W);
    draw_text_centered(WIN_W / 2.0f, 68, 1.0f, COL_DIM,
                       "play offline, or host and join a match without editing files", TEXT_MAX_W);

    /* PLAY leads because on macOS this window is what opening 3S-ARM.app
     * gets you, so the launcher has to serve the player who never wanted
     * netplay at all. It is the double-click-the-game path verbatim: no
     * handoff file, no extra arguments, nothing to display afterwards
     * beyond the process line render_child_state() already draws. */
    const Button b_play = { { 170, 92, 300, 52 }, "PLAY" };
    const Button b_host = { { 170, 156, 300, 52 }, "HOST A GAME" };
    const Button b_join = { { 170, 220, 300, 52 }, "JOIN WITH A CODE" };
    const Button b_lan = { { 170, 284, 300, 52 }, "LAN GAME" };

    if (button(&b_play)) {
        start_play();
    }
    if (button(&b_host)) {
        enter_screen(SCREEN_HOST);
        start_host();
    }
    if (button(&b_join)) {
        enter_screen(SCREEN_JOIN);
    }
    if (button(&b_lan)) {
        enter_screen(SCREEN_LAN);
    }

    render_msg(360);

    /* Corner button: key config is a rare, one-off errand next to the
     * three things this launcher exists to do. */
    const Button b_keys = { { 516, 398, 108, 34 }, "KEYS" };
    if (button(&b_keys)) {
        enter_screen(SCREEN_KEYS);
    }

    render_child_state(440);
    draw_text_centered(WIN_W / 2.0f, 456, 1.0f, COL_DIM, "esc quits", TEXT_MAX_W);
}

static void screen_host(void) {
    draw_text_centered(WIN_W / 2.0f, 40, 2.0f, COL_TEXT, "HOSTING", TEXT_MAX_W);

    host_poll();

    if (g_host_code[0] != '\0') {
        draw_text_centered(WIN_W / 2.0f, 96, 1.0f, COL_DIM, "give this code to the other player:", TEXT_MAX_W);
        draw_text_centered(WIN_W / 2.0f, 130, 4.0f, COL_ACCENT, g_host_code, TEXT_MAX_W);

        const Button b_copy = { { 220, 200, 200, 40 }, "COPY AGAIN" };
        if (button(&b_copy)) {
            if (SDL_SetClipboardText(g_host_code)) {
                set_msg(COL_OK, "Copied.", NULL);
            } else {
                set_msg(COL_ERR, "Clipboard copy failed.", SDL_GetError());
            }
        }
        if (strcmp(g_host_status, "HOSTING") != 0) {
            draw_text_centered(WIN_W / 2.0f, 260, 1.0f, COL_DIM,
                               "no longer advertising - peer connected, or hosting ended", TEXT_MAX_W);
        }
    } else {
        draw_text_centered(WIN_W / 2.0f, 140, 2.0f, COL_DIM, "waiting for the room code...", TEXT_MAX_W);
        draw_text_centered(WIN_W / 2.0f, 170, 1.0f, COL_DIM,
                           "the game is discovering its public address", TEXT_MAX_W);
    }

    render_msg(320);

    const Button b_back = { { 240, 390, 160, 40 }, "BACK" };
    if (button(&b_back)) {
        enter_screen(SCREEN_MENU);
    }
    render_child_state(440);
    draw_text_centered(WIN_W / 2.0f, 456, 1.0f, COL_DIM,
                       "back leaves the game running - quit it from the game window", TEXT_MAX_W);
}

static void render_entry_field(const char* value, const char* placeholder) {
    const SDL_FRect field = { 120, 150, 400, 44 };
    SDL_SetRenderDrawColor(g_ren, 34, 34, 46, 255);
    SDL_RenderFillRect(g_ren, &field);
    SDL_SetRenderDrawColor(g_ren, 120, 120, 150, 255);
    SDL_RenderRect(g_ren, &field);

    /* The caret follows the width actually drawn, not strlen(value), so a
     * long hostname that had to be shrunk to fit does not push it out of
     * the field. */
    float text_w = 0.0f;
    if (value[0] != '\0') {
        text_w = draw_text_fit(field.x + 12, field.y + 14, 2.0f, COL_TEXT, value, field.w - 24.0f);
    } else {
        draw_text_fit(field.x + 12, field.y + 14, 2.0f, COL_DIM, placeholder, field.w - 24.0f);
    }
    /* Blinking caret. */
    if ((SDL_GetTicks() / 500) % 2 == 0) {
        const float cx = field.x + 12 + text_w;
        const SDL_FRect caret = { cx, field.y + 12, 3, 20 };
        SDL_SetRenderDrawColor(g_ren, 230, 230, 235, 255);
        SDL_RenderFillRect(g_ren, &caret);
    }
}

static void screen_join(void) {
    draw_text_centered(WIN_W / 2.0f, 40, 2.0f, COL_TEXT, "JOIN WITH A CODE", TEXT_MAX_W);
    draw_text_centered(WIN_W / 2.0f, 110, 1.0f, COL_DIM,
                       "type or paste the host's room code (ctrl+v / cmd+v)", TEXT_MAX_W);

    render_entry_field(g_code_entry, "room code");

    const Button b_go = { { 240, 220, 160, 44 }, "JOIN" };
    if (button(&b_go)) {
        start_join();
    }

    render_msg(300);

    const Button b_back = { { 240, 390, 160, 40 }, "BACK" };
    if (button(&b_back)) {
        enter_screen(SCREEN_MENU);
    }
    render_child_state(440);
    draw_text_centered(WIN_W / 2.0f, 456, 1.0f, COL_DIM, "enter joins - esc goes back", TEXT_MAX_W);
}

static void screen_lan(void) {
    draw_text_centered(WIN_W / 2.0f, 40, 2.0f, COL_TEXT, "LAN GAME", TEXT_MAX_W);
    draw_text_centered(WIN_W / 2.0f, 110, 1.0f, COL_DIM,
                       "no room code needed - enter the other machine's IP", TEXT_MAX_W);

    render_entry_field(g_ip_entry, "192.168.1.50");

    char player_label[32];
    SDL_snprintf(player_label, sizeof(player_label), "PLAYER: %d", g_lan_player);
    const Button b_player = { { 150, 220, 160, 44 }, player_label };
    if (button(&b_player)) {
        g_lan_player = g_lan_player == 1 ? 2 : 1;
    }

    const Button b_go = { { 330, 220, 160, 44 }, "START" };
    if (button(&b_go)) {
        start_lan();
    }

    render_msg(300);

    const Button b_back = { { 240, 390, 160, 40 }, "BACK" };
    if (button(&b_back)) {
        enter_screen(SCREEN_MENU);
    }
    render_child_state(440);
    draw_text_centered(WIN_W / 2.0f, 456, 1.0f, COL_DIM,
                       "one machine is player 1, the other player 2", TEXT_MAX_W);
}

/* ---------------------------------------------------------------------- */
/* Key bindings.
 *
 * The pad-button names in the labels are what the game's own config
 * screens call these buttons; the attack names are what they do under the
 * default button config (ioConvInitData -> Convert_Data in sys_sub.c) and
 * under the identity config netplay forces (netplay.c, netplay_nav.c).
 * L1 and L2 are not listed because both configs leave them unassigned,
 * and left-stick/right-stick because the game does not use them -- all
 * four still round-trip through the file, because Keymap_Save() writes
 * the whole table. */

typedef struct {
    KeymapButton button;
    const char* label;
} KeyRow;

static const KeyRow g_key_rows[] = {
    { KEYMAP_BUTTON_UP, "UP" },
    { KEYMAP_BUTTON_DOWN, "DOWN" },
    { KEYMAP_BUTTON_LEFT, "LEFT" },
    { KEYMAP_BUTTON_RIGHT, "RIGHT" },
    { KEYMAP_BUTTON_WEST, "LP (Square)" },
    { KEYMAP_BUTTON_NORTH, "MP (Triangle)" },
    { KEYMAP_BUTTON_RIGHT_SHOULDER, "HP (R1)" },
    { KEYMAP_BUTTON_SOUTH, "LK (Cross)" },
    { KEYMAP_BUTTON_EAST, "MK (Circle)" },
    { KEYMAP_BUTTON_RIGHT_TRIGGER, "HK (R2)" },
    { KEYMAP_BUTTON_START, "START" },
    { KEYMAP_BUTTON_BACK, "SELECT" },
};

#define KEY_ROW_COUNT ((int)SDL_arraysize(g_key_rows))
#define KEY_ROWS_PER_COL 6

static void keys_load(void) {
    if (g_keymap_loaded) {
        return;
    }
    /* Keymap_Init() writes the defaults file when none exists. Doing that
     * from here is the same thing the game's first run would do, and it
     * means the screen always has a concrete binding to show. */
    Keymap_Init();
    g_keymap_loaded = true;
}

static void keys_binding_text(KeymapButton button, char* out, size_t out_len) {
    const SDL_Scancode* codes = Keymap_GetScancodes(button);

    out[0] = '\0';
    for (int i = 0; i < KEYMAP_CODES_PER_BUTTON; i++) {
        if (codes[i] == SDL_SCANCODE_UNKNOWN) {
            break;
        }
        if (out[0] != '\0') {
            SDL_strlcat(out, ", ", out_len);
        }
        const char* name = SDL_GetScancodeName(codes[i]);
        SDL_strlcat(out, name != NULL && name[0] != '\0' ? name : "?", out_len);
    }
    if (out[0] == '\0') {
        SDL_strlcpy(out, "(none)", out_len);
    }
}

/* Prefer the row label ("LP (Square)") over the raw keymap name ("west")
 * so a duplicate-binding warning names the button the way the screen
 * does; buttons with no row fall back to the file's own spelling. */
static const char* keys_button_label(KeymapButton button) {
    for (int i = 0; i < KEY_ROW_COUNT; i++) {
        if (g_key_rows[i].button == button) {
            return g_key_rows[i].label;
        }
    }
    return Keymap_GetButtonName(button);
}

static bool keys_save(void) {
    if (Keymap_Save()) {
        return true;
    }
    set_msg(COL_ERR, "Could not write the keymap file.", Paths_GetPrefPath());
    return false;
}

static void keys_reset_defaults(void) {
    for (int i = 0; i < KEYMAP_BUTTON_COUNT; i++) {
        const SDL_Scancode* defaults = Keymap_GetDefaultScancodes(i);
        int count = 0;
        while (count < KEYMAP_CODES_PER_BUTTON && defaults[count] != SDL_SCANCODE_UNKNOWN) {
            count++;
        }
        Keymap_SetScancodes(i, defaults, count);
    }
    if (keys_save()) {
        set_msg(COL_OK, "All keys restored to defaults.", NULL);
    }
}

/* Handles the one keypress that ends a capture. */
static void keys_capture(SDL_Scancode scancode, SDL_Keycode key) {
    const KeyRow* row = &g_key_rows[g_key_capture];
    g_key_capture = -1;

    if (key == SDLK_ESCAPE) {
        set_msg(COL_TEXT, "Rebind cancelled.", NULL);
        return;
    }

    if (key == SDLK_BACKSPACE || key == SDLK_DELETE) {
        Keymap_SetScancodes(row->button, NULL, 0);
        if (keys_save()) {
            char line[96];
            SDL_snprintf(line, sizeof(line), "%s unbound.", row->label);
            /* The keymap file has no way to spell "deliberately unbound":
             * a button with no key parses as absent, and an absent button
             * gets its default back at the next Keymap_Init(). Say so
             * rather than let the binding quietly reappear. */
            set_msg(COL_ACCENT, line, "an unbound button returns to its default the next time the game starts");
        }
        return;
    }

    if (scancode == SDL_SCANCODE_UNKNOWN) {
        set_msg(COL_ERR, "That key has no name SDL can store.", NULL);
        return;
    }

    int clash = -1;
    for (int i = 0; i < KEYMAP_BUTTON_COUNT; i++) {
        if (i == row->button) {
            continue;
        }
        const SDL_Scancode* codes = Keymap_GetScancodes(i);
        for (int j = 0; j < KEYMAP_CODES_PER_BUTTON; j++) {
            if (codes[j] == scancode) {
                clash = i;
            }
        }
    }

    Keymap_SetScancodes(row->button, &scancode, 1);
    if (!keys_save()) {
        return;
    }

    char line[96];
    SDL_snprintf(line, sizeof(line), "%s = %s", row->label, SDL_GetScancodeName(scancode));

    /* A key on two buttons is confusing but legal -- the game ORs both --
     * so warn and apply rather than silently stealing it from the other
     * button, which would lose a binding the user never asked to change. */
    if (clash >= 0) {
        char warn[128];
        SDL_snprintf(warn, sizeof(warn), "note: %s is also bound to %s",
                     SDL_GetScancodeName(scancode), keys_button_label((KeymapButton)clash));
        set_msg(COL_ACCENT, line, warn);
    } else {
        set_msg(COL_OK, line, NULL);
    }
}

static bool key_cell(const SDL_FRect* r, bool capturing) {
    const bool hover = g_mouse_x >= r->x && g_mouse_x < r->x + r->w && g_mouse_y >= r->y &&
                       g_mouse_y < r->y + r->h;

    if (capturing) {
        SDL_SetRenderDrawColor(g_ren, 96, 76, 34, 255);
    } else if (hover) {
        SDL_SetRenderDrawColor(g_ren, 62, 62, 88, 255);
    } else {
        SDL_SetRenderDrawColor(g_ren, 40, 40, 56, 255);
    }
    SDL_RenderFillRect(g_ren, r);
    SDL_SetRenderDrawColor(g_ren, capturing ? 255 : 100, capturing ? 208 : 100,
                           capturing ? 64 : 130, 255);
    SDL_RenderRect(g_ren, r);

    return hover && g_mouse_clicked;
}

static void screen_keys(void) {
    keys_load();

    draw_text_centered(WIN_W / 2.0f, 14, 2.0f, COL_ACCENT, "KEY BINDINGS", TEXT_MAX_W);

    for (int i = 0; i < KEY_ROW_COUNT; i++) {
        const SDL_FRect cell = { i < KEY_ROWS_PER_COL ? 20.0f : 328.0f,
                                 48.0f + 40.0f * (float)(i % KEY_ROWS_PER_COL), 292.0f, 36.0f };
        const bool capturing = g_key_capture == i;

        if (key_cell(&cell, capturing)) {
            g_key_capture = i;
            set_msg(COL_TEXT, "", NULL);
        }

        draw_text_fit(cell.x + 8, cell.y + 4, 1.5f, COL_ACCENT, g_key_rows[i].label, cell.w - 16.0f);

        char binding[160];
        if (capturing) {
            SDL_strlcpy(binding, "press a key...", sizeof(binding));
        } else {
            keys_binding_text(g_key_rows[i].button, binding, sizeof(binding));
        }
        draw_text_fit(cell.x + 8, cell.y + 20, 1.5f, capturing ? COL_ACCENT : COL_TEXT, binding,
                      cell.w - 16.0f);
    }

    draw_text_centered(WIN_W / 2.0f, 296, 1.0f, COL_DIM,
                       "click a row, then press a key - esc cancels, backspace unbinds",
                       TEXT_MAX_W);

    render_msg(316);

    const Button b_reset = { { 142, 386, 240, 36 }, "RESET TO DEFAULTS" };
    if (button(&b_reset)) {
        g_key_capture = -1;
        keys_reset_defaults();
    }

    const Button b_back = { { 398, 386, 100, 36 }, "BACK" };
    if (button(&b_back)) {
        enter_screen(SCREEN_MENU);
    }

    draw_text_centered(WIN_W / 2.0f, 440, 1.0f, COL_DIM,
                       "changes apply the next time the game starts", TEXT_MAX_W);
    draw_text_centered(WIN_W / 2.0f, 456, 1.0f, COL_DIM,
                       "gamepads use SDL's standard gamepad mapping - not configurable here",
                       TEXT_MAX_W);
}

/* ---------------------------------------------------------------------- */

static void handle_event(const SDL_Event* ev) {
    switch (ev->type) {
    case SDL_EVENT_QUIT:
        g_quit = true;
        break;

    case SDL_EVENT_MOUSE_MOTION:
        g_mouse_x = ev->motion.x;
        g_mouse_y = ev->motion.y;
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev->button.button == SDL_BUTTON_LEFT) {
            g_mouse_x = ev->button.x;
            g_mouse_y = ev->button.y;
            g_mouse_clicked = true;
        }
        break;

    case SDL_EVENT_TEXT_INPUT: {
        char* buf = active_entry_buf();
        if (buf != NULL) {
            entry_append_filtered(buf, active_entry_cap(), ev->text.text,
                                  g_screen == SCREEN_JOIN);
        }
        break;
    }

    case SDL_EVENT_KEY_DOWN: {
        /* A capture consumes the whole keypress -- including ESC, which
         * must cancel the capture without also leaving the screen. */
        if (g_screen == SCREEN_KEYS && g_key_capture >= 0) {
            keys_capture(ev->key.scancode, ev->key.key);
            break;
        }

        char* buf = active_entry_buf();
        if (ev->key.key == SDLK_ESCAPE) {
            if (g_screen == SCREEN_MENU) {
                g_quit = true;
            } else {
                enter_screen(SCREEN_MENU);
            }
        } else if (buf != NULL && ev->key.key == SDLK_BACKSPACE) {
            const size_t len = strlen(buf);
            if (len > 0) {
                buf[len - 1] = '\0';
            }
        } else if (buf != NULL && ev->key.key == SDLK_V &&
                   (ev->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0) {
            char* clip = SDL_GetClipboardText();
            if (clip != NULL) {
                entry_append_filtered(buf, active_entry_cap(), clip,
                                      g_screen == SCREEN_JOIN);
                SDL_free(clip);
            }
        } else if (ev->key.key == SDLK_RETURN || ev->key.key == SDLK_KP_ENTER) {
            if (g_screen == SCREEN_JOIN) {
                start_join();
            } else if (g_screen == SCREEN_LAN) {
                start_lan();
            }
        }
        break;
    }

    default:
        break;
    }
}

int main(int argc, char** argv) {
    SDL_SetAppMetadata("3S Netplay Launcher", NULL, "dev.crowdedstreet.3s-netplay-launcher");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (!SDL_CreateWindowAndRenderer("3S Netplay Launcher", WIN_W, WIN_H, 0, &g_win, &g_ren)) {
        fprintf(stderr, "window/renderer creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    if (!find_game_binary(argc, argv)) {
        set_msg(COL_ERR, "Game binary not found next to the launcher.",
                "Run with --game <path-to-game>, or set THIRDSARM_GAME.");
    } else {
        SDL_Log("[launcher] game binary: %s", g_game_path);
    }

    /* Optional auto-start flags — the button actions, triggerable from
     * the command line (scripting / testing; the UI stays up either
     * way): --play, --host, --join <code>, --lan <ip> <player 1|2>, --keys. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--play") == 0) {
            start_play();
        } else if (strcmp(argv[i], "--keys") == 0) {
            enter_screen(SCREEN_KEYS);
        } else if (strcmp(argv[i], "--host") == 0) {
            enter_screen(SCREEN_HOST);
            start_host();
        } else if (strcmp(argv[i], "--join") == 0 && i + 1 < argc) {
            enter_screen(SCREEN_JOIN);
            SDL_strlcpy(g_code_entry, argv[i + 1], sizeof(g_code_entry));
            start_join();
            i++;
        } else if (strcmp(argv[i], "--lan") == 0 && i + 2 < argc) {
            enter_screen(SCREEN_LAN);
            SDL_strlcpy(g_ip_entry, argv[i + 1], sizeof(g_ip_entry));
            g_lan_player = SDL_atoi(argv[i + 2]) == 2 ? 2 : 1;
            start_lan();
            i += 2;
        }
    }

    while (!g_quit) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 33)) {
            handle_event(&ev);
            while (SDL_PollEvent(&ev)) {
                handle_event(&ev);
            }
        }

        SDL_SetRenderDrawColor(g_ren, 24, 24, 32, 255);
        SDL_RenderClear(g_ren);

        switch (g_screen) {
        case SCREEN_MENU:
            screen_menu();
            break;
        case SCREEN_HOST:
            screen_host();
            break;
        case SCREEN_JOIN:
            screen_join();
            break;
        case SCREEN_LAN:
            screen_lan();
            break;
        case SCREEN_KEYS:
            screen_keys();
            break;
        }

        g_mouse_clicked = false;
        SDL_RenderPresent(g_ren);
    }

    /* Deliberately no SDL_KillProcess: the spawned game IS the match;
     * closing the launcher must not end it. DestroyProcess only frees
     * our handle. */
    if (g_child != NULL) {
        SDL_DestroyProcess(g_child);
    }
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
