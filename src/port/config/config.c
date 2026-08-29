#include "port/config/config.h"
#include "port/config/config_helpers.h"
#include "port/paths.h"

#include <stdbool.h>
#include <stdio.h>

#include <SDL3/SDL.h>

#define CONFIG_ENTRIES_MAX 128

typedef enum ConfigType {
    CFG_BOOL,
    CFG_INT,
    CFG_STRING,
} ConfigType;

typedef union ConfigValue {
    bool b;
    int i;
    char* s;
} ConfigValue;

typedef struct ConfigEntry {
    const char* key;
    ConfigType type;
    ConfigValue value;
    bool coerced; /* true once find_entry has coerced this read entry to match
                   * the default's type; used to log once and to skip repeat
                   * coercion attempts on subsequent Config_Get* calls. */
} ConfigEntry;

#if defined(PORT_MISTER)
#define DEFAULT_VIDEO_DRIVER_ORDER "dummy"
#define DEFAULT_RENDER_DRIVER_ORDER "software"
#define DEFAULT_WINDOW_WIDTH 320
#define DEFAULT_WINDOW_HEIGHT 240
#define DEFAULT_SCALE_MODE "native"
#define DEFAULT_SOFTWARE_FRAME_MODE "on"
#else
#define DEFAULT_VIDEO_DRIVER_ORDER ""
#define DEFAULT_RENDER_DRIVER_ORDER ""
#define DEFAULT_WINDOW_WIDTH 640
#define DEFAULT_WINDOW_HEIGHT 480
#define DEFAULT_SCALE_MODE "soft-linear"
#define DEFAULT_SOFTWARE_FRAME_MODE "off"
#endif

/* DEFAULT_SUPER_EFFECT_QUALITY = "full" is the table-level default, but
 * init_super_effect_quality_mode() in src/port/sdl/sdl_app.c overrides it
 * to "cached-bg" on MiSTer when the key is absent on disk. The OSD knob
 * was removed; this key is dev-only — flip via config.ini for release
 * builds. See docs/config.md for the full chain. */
#define DEFAULT_SUPER_EFFECT_QUALITY "full"
#define DEFAULT_ARM_CLOCK "800"
#define DEFAULT_GAME_MODE "console"
#define DEFAULT_HOLD_TO_PAUSE "off"
/* Matches Init_sound_system()'s sys_w.bgm_type = BGM_ARRANGED default
 * (sound3rd.c) and savesub.c's BGM_ARRANGED == 0. */
#define DEFAULT_BGM_TYPE "arranged"
/* "auto" == no override: the settings save file (or, with no save yet,
 * Get_Default_Language()'s locale-derived pick) decides. Language_
 * ApplyBootOverride() materializes the resolved value over this sentinel on
 * the first boot so the MiSTer OSD's Language row has something concrete to
 * display. Mirrors CFG_KEY_BALANCE's "auto" sentinel below. */
#define DEFAULT_LANGUAGE "auto"

/* Root of the cached `.3sr` replay set (CFG_KEY_REPLAYS_ROOT). The common-case
 * literal per platform. See docs/config.md.
 *
 * The HPS wrapper reads the key from the config file this table seeds
 * (RpReplaysRootLoadFrom(), vendor/Main_MiSTer/replay_proxy.c) rather than
 * carrying its own copy of the path, so there is nothing to keep in step. */
#if defined(PORT_MISTER)
#define DEFAULT_REPLAYS_ROOT "/media/fat/games/3s-arm/replays"
#elif defined(PORT_MIYOO_MINI_PLUS)
#define DEFAULT_REPLAYS_ROOT "/mnt/SDCARD/Roms/PORTS/Games/3s-arm/replays"
#else
#define DEFAULT_REPLAYS_ROOT "./replays"
#endif

static const ConfigEntry default_entries[] = {
    { .key = CFG_KEY_FULLSCREEN, .type = CFG_BOOL, .value.b = true },
    { .key = CFG_KEY_WINDOW_WIDTH, .type = CFG_INT, .value.i = DEFAULT_WINDOW_WIDTH },
    { .key = CFG_KEY_WINDOW_HEIGHT, .type = CFG_INT, .value.i = DEFAULT_WINDOW_HEIGHT },
    { .key = CFG_KEY_SCALEMODE, .type = CFG_STRING, .value.s = DEFAULT_SCALE_MODE },
    { .key = CFG_KEY_SCANLINES, .type = CFG_INT, .value.i = 0 },
    { .key = CFG_DRAW_PLAYERS_ABOVE_HUD, .type = CFG_BOOL, .value.b = false },
    { .key = CFG_KEY_BALANCE, .type = CFG_STRING, .value.s = "auto" },
    { .key = CFG_KEY_BGM_TYPE, .type = CFG_STRING, .value.s = DEFAULT_BGM_TYPE },
    { .key = CFG_KEY_LANGUAGE, .type = CFG_STRING, .value.s = DEFAULT_LANGUAGE },
    { .key = CFG_KEY_SOFTWARE_FRAME_MODE, .type = CFG_STRING, .value.s = DEFAULT_SOFTWARE_FRAME_MODE },
    { .key = CFG_KEY_SUPER_EFFECT_QUALITY, .type = CFG_STRING, .value.s = DEFAULT_SUPER_EFFECT_QUALITY },
    { .key = CFG_KEY_ARM_CLOCK, .type = CFG_STRING, .value.s = DEFAULT_ARM_CLOCK },
    { .key = CFG_KEY_GAME_MODE, .type = CFG_STRING, .value.s = DEFAULT_GAME_MODE },
    { .key = CFG_KEY_HOLD_TO_PAUSE, .type = CFG_STRING, .value.s = DEFAULT_HOLD_TO_PAUSE },
    { .key = CFG_KEY_REPLAYS_ROOT, .type = CFG_STRING, .value.s = DEFAULT_REPLAYS_ROOT },
    { .key = CFG_KEY_REPLAYS_MAX_MB, .type = CFG_INT, .value.i = 200 },
    /* Remote replay browse. Empty host = remote disabled (a no-config boot
     * stays local-only). Default port is the fcade-proxy's default
     * (tools/fcade-proxy README).
     *
     * These two rows are load-bearing even though nothing under src/ reads
     * them: write_defaults() below seeds the on-device config file from this
     * table, and the HPS wrapper's RpConfigLoadFrom()
     * (vendor/Main_MiSTer/replay_proxy.c) parses that file for exactly these
     * two literal key names. Do not delete them as "unused". */
    { .key = CFG_KEY_REPLAY_PROXY_HOST, .type = CFG_STRING, .value.s = "" },
    { .key = CFG_KEY_REPLAY_PROXY_PORT, .type = CFG_INT, .value.i = 3479 },
    { .key = CFG_KEY_SHOW_FPS, .type = CFG_STRING, .value.s = "off" },
    { .key = CFG_KEY_VIDEO_DRIVER_ORDER, .type = CFG_STRING, .value.s = DEFAULT_VIDEO_DRIVER_ORDER },
    { .key = CFG_KEY_RENDER_DRIVER_ORDER, .type = CFG_STRING, .value.s = DEFAULT_RENDER_DRIVER_ORDER },
    /* Direct-P2P defaults (docs/plan-stun-direct-p2p.md Step 5). The 3
     * runtime-populated keys (LAST_PEER_CODE + HOST_PORT at 0 + DISABLE_UPNP)
     * are not here — they live in the entries[] at runtime only. HANDOFF_PATH
     * default is the wrapper's canonical tmpfs path; STUN_TIMEOUT_MS budget
     * bumped from upstream's 2000ms to 4000ms for congested public STUN. */
    { .key = CFG_KEY_NETPLAY_DIRECT_P2P_HANDOFF_PATH, .type = CFG_STRING, .value.s = "/tmp/3s-arm-netplay.handoff" },
    { .key = CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS, .type = CFG_INT, .value.i = 4000 },
    /* Bilateral hole-punch fallback defaults (docs/plan-bilateral-hole-punch.md
     * Decision 6). SIGNAL_URL points at the live rendezvous server.
     * BILATERAL_PUNCH_MS raised 3000 -> 5000 (S2,
     * docs/plan-netplay-connection.md §4): the two sides enter their
     * punch windows skewed by DELIVER arrival — the host only learns of
     * the joiner on its next Tick after the server pushes the DELIVER,
     * while the joiner starts punching the moment its own DELIVER
     * parses — and both loops simply drop stray/early datagrams that
     * don't match the punch payload, so a longer overlap window costs
     * nothing but the failure-case wait. */
    { .key = CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL, .type = CFG_BOOL, .value.b = false },
    { .key = CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, .type = CFG_STRING, .value.s = "udp://46.62.244.55:3478" },
    { .key = CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_BUDGET_MS, .type = CFG_INT, .value.i = 8000 },
    { .key = CFG_KEY_NETPLAY_DIRECT_P2P_BILATERAL_PUNCH_MS, .type = CFG_INT, .value.i = 5000 },
    /* Host liveness (docs/plan-netplay-connection.md S1): persistent host
     * re-REGISTER cadence. ~0.2 pkt/s/host — far under the rendezvous
     * server's 10 pkt/s/IP limiter (rendezvous-server.js RATE_LIMIT_PER_
     * WINDOW=10 / RATE_WINDOW_MS=1000). */
    { .key = CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS, .type = CFG_INT, .value.i = 5000 },
    /* Host liveness (S1): STUN rebind keepalive cadence while
     * HOST_WAITING. Refreshes the advertised NAT mapping and detects
     * public-endpoint drift. <= 0 disables. Default 15 s (was 20 s,
     * review M2): common consumer NAT UDP idle timeouts bottom out
     * around 30 s (netfilter's unreplied default) with outliers near
     * 20 s — keeping the keepalive comfortably below those HOLDS the
     * mapping so drift never happens, which beats detecting it. Still
     * only 0.067 pkt/s toward the STUN server; floor clamp is 5000. */
    { .key = CFG_KEY_NETPLAY_DIRECT_P2P_STUN_KEEPALIVE_MS, .type = CFG_INT, .value.i = 15000 },
    /* Task #119: late-punch rescue layer kill switch (see config.h). */
    { .key = CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_LATE_PUNCH, .type = CFG_BOOL, .value.b = false },
    /* S6 candidate racing (docs/plan-netplay-connection.md §8). 8000 ms
     * bounds the whole post-STUN cascade now that its legs overlap:
     * a room-code punch leg and a DELIVER punch leg (each
     * BILATERAL_PUNCH_MS long) and the rendezvous leg (SIGNAL_BUDGET_MS)
     * all fit inside it. Before S6 the same work cost the SUM:
     * 2500 + 8000 + 5000 = 15500 ms. */
    { .key = CFG_KEY_NETPLAY_DIRECT_P2P_RACE_BUDGET_MS, .type = CFG_INT, .value.i = 8000 },
    { .key = CFG_KEY_NETPLAY_INPUT_PREDICTION_WINDOW, .type = CFG_INT, .value.i = 8 },
    { .key = CFG_KEY_NETPLAY_DIAG_ENABLE, .type = CFG_BOOL, .value.b = true },
    { .key = CFG_KEY_NETPLAY_SPARSE_EFFECT_SAVE_ENABLED, .type = CFG_BOOL, .value.b = true },
};

static ConfigEntry entries[CONFIG_ENTRIES_MAX] = { 0 };
static int entry_count = 0;

static bool is_int(const char* string) {
    for (int i = 0; i < SDL_strlen(string); i++) {
        if (SDL_isdigit(string[i]) || ((i == 0) && (string[i] == '-'))) {
            continue;
        } else {
            return false;
        }
    }

    return true;
}

static ConfigEntry* find_entry_in_array(const char* key, const ConfigEntry* array, size_t size) {
    for (int i = 0; i < size; i++) {
        const ConfigEntry* entry = &array[i];

        if (SDL_strcmp(key, entry->key) == 0) {
            return entry;
        }
    }

    return NULL;
}

static const char* type_name(ConfigType type) {
    switch (type) {
    case CFG_BOOL:
        return "bool";
    case CFG_INT:
        return "int";
    case CFG_STRING:
        return "string";
    }
    return "?";
}

/* Attempt to coerce a read entry's value into the expected default type,
 * mutating the entry in place. Returns true if coercion succeeded (or was a
 * no-op because types already match), false if the user's value cannot be
 * reasonably interpreted as the target type — caller should fall back to the
 * default in that case.
 *
 * Mutating in place (rather than duplicating) keeps the storage lifetime
 * tied to the existing entries[] table, which Config_Destroy already cleans
 * up. The .coerced flag suppresses repeat work and repeat logging. */
static bool coerce_entry(ConfigEntry* read_entry, ConfigType target_type) {
    if (read_entry->type == target_type) {
        return true;
    }
    if (read_entry->coerced) {
        /* Already attempted; type either matches now or we left it alone
         * because coercion was impossible. The caller's type check on the
         * returned entry will sort it out. */
        return read_entry->type == target_type;
    }

    const ConfigType from_type = read_entry->type;
    bool ok = false;

    switch (target_type) {
    case CFG_STRING: {
        char buf[32];
        const char* coerced_str = NULL;
        if (from_type == CFG_INT) {
            SDL_snprintf(buf, sizeof(buf), "%d", read_entry->value.i);
            coerced_str = buf;
        } else if (from_type == CFG_BOOL) {
            coerced_str = read_entry->value.b ? "true" : "false";
        }
        if (coerced_str != NULL) {
            char* dup = SDL_strdup(coerced_str);
            if (dup != NULL) {
                read_entry->type = CFG_STRING;
                read_entry->value.s = dup;
                ok = true;
            }
        }
        break;
    }

    case CFG_INT: {
        if (from_type == CFG_BOOL) {
            read_entry->value.i = read_entry->value.b ? 1 : 0;
            read_entry->type = CFG_INT;
            ok = true;
        } else if (from_type == CFG_STRING) {
            const char* s = read_entry->value.s;
            if (s != NULL && is_int(s)) {
                int parsed = SDL_atoi(s);
                SDL_free(read_entry->value.s);
                read_entry->value.i = parsed;
                read_entry->type = CFG_INT;
                ok = true;
            }
            /* If the string isn't a clean integer (e.g. "banana"), leave the
             * entry untouched and report failure so caller uses default. */
        }
        break;
    }

    case CFG_BOOL: {
        if (from_type == CFG_INT) {
            read_entry->value.b = read_entry->value.i != 0;
            read_entry->type = CFG_BOOL;
            ok = true;
        } else if (from_type == CFG_STRING) {
            const char* s = read_entry->value.s;
            if (s != NULL) {
                bool parsed = false;
                bool matched = false;
                if (SDL_strcasecmp(s, "true") == 0 || SDL_strcasecmp(s, "yes") == 0 ||
                    SDL_strcmp(s, "1") == 0) {
                    parsed = true;
                    matched = true;
                } else if (SDL_strcasecmp(s, "false") == 0 || SDL_strcasecmp(s, "no") == 0 ||
                           SDL_strcmp(s, "0") == 0) {
                    parsed = false;
                    matched = true;
                }
                if (matched) {
                    SDL_free(read_entry->value.s);
                    read_entry->value.b = parsed;
                    read_entry->type = CFG_BOOL;
                    ok = true;
                }
            }
        }
        break;
    }
    }

    /* Mark coerced regardless of success: a failed coercion (e.g. garbage
     * string for an int key) should not be retried on every Config_Get* and
     * should not re-log every call. */
    read_entry->coerced = true;

    if (ok) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Config: coerced %s value for key '%s' to default type %s",
                    type_name(from_type),
                    read_entry->key,
                    type_name(target_type));
    }

    return ok;
}

static ConfigEntry* find_entry(const char* key) {
    ConfigEntry* default_entry = find_entry_in_array(key, default_entries, SDL_arraysize(default_entries));
    ConfigEntry* read_entry = find_entry_in_array(key, entries, entry_count);

    if (read_entry != NULL) {
        if (default_entry != NULL && read_entry->type != default_entry->type) {
            // Type mismatch: try to coerce the user's value into the default's
            // type so their override isn't silently swallowed (e.g. arm-clock
            // = 1200 parsed as CFG_INT vs CFG_STRING default "800"). Fall
            // back to the default only if coercion is genuinely impossible.
            if (coerce_entry(read_entry, default_entry->type)) {
                return read_entry;
            }
            return default_entry;
        } else {
            return read_entry;
        }
    } else if (default_entry != NULL) {
        return default_entry;
    } else {
        /* Unknown key: warn and fall through to the getters' documented
         * defaults (false / 0 / NULL). This used to SDL_assert(false),
         * which in a default Debug build invokes SDL's INTERACTIVE
         * assertion prompt — a guaranteed hang for any non-interactive
         * run (test harnesses, CI) that reaches it. A typo'd key is a
         * programmer error worth logging, not worth an interactive trap. */
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Config: unknown key '%s' — returning the type default", key);
        return NULL;
    }
}

bool Config_HasExplicitKey(const char* key) {
    return find_entry_in_array(key, entries, entry_count) != NULL;
}

static void print_config_entry_to_io(SDL_IOStream* io, const ConfigEntry* entry) {
    io_printf(io, "%s = ", entry->key);

    switch (entry->type) {
    case CFG_BOOL:
        io_printf(io, entry->value.b ? "true" : "false");
        break;

    case CFG_INT:
        io_printf(io, "%d", entry->value.i);
        break;

    case CFG_STRING:
        io_printf(io, entry->value.s);
        break;
    }
}

static void write_defaults(const char* dst_path) {
    SDL_IOStream* io = SDL_IOFromFile(dst_path, "w");
    io_printf(io,
              "# For the full list of settings see https://github.com/kimchiman52/3s-mister-arm/blob/main/docs/config.md\n\n");

    for (int i = 0; i < SDL_arraysize(default_entries); i++) {
        print_config_entry_to_io(io, &default_entries[i]);
        io_printf(io, "\n");
    }

    SDL_CloseIO(io);
}

static bool dict_iterator(const char* key, const char* value) {
    if (entry_count == CONFIG_ENTRIES_MAX) {
        printf("⚠️ Reached max config entry count (%d), skipping the rest\n", CONFIG_ENTRIES_MAX);
        return false;
    }

    ConfigEntry* entry = &entries[entry_count];
    entry->key = SDL_strdup(key);
    entry->coerced = false;

    const bool is_true = SDL_strcmp(value, "true") == 0;
    const bool is_false = SDL_strcmp(value, "false") == 0;

    if (is_true || is_false) {
        entry->type = CFG_BOOL;
        entry->value.b = is_true;
    } else if (is_int(value)) {
        entry->type = CFG_INT;
        entry->value.i = SDL_atoi(value);
    } else {
        entry->type = CFG_STRING;
        entry->value.s = SDL_strdup(value);
    }

    entry_count += 1;
    return true;
}

void Config_Init() {
    const char* pref_path = Paths_GetPrefPath();
    char* config_path;
    SDL_asprintf(&config_path, "%sconfig", pref_path);

    FILE* f = fopen(config_path, "r");

    if (f == NULL) {
        // Config doesn't exist. Write defaults
        write_defaults(config_path);
        SDL_free(config_path);
        return;
    }

    SDL_free(config_path);
    dict_read(f, dict_iterator);
    fclose(f);
}

void Config_Destroy() {
    for (int i = 0; i < entry_count; i++) {
        ConfigEntry* entry = &entries[i];
        SDL_free(entry->key);

        if (entry->type == CFG_STRING) {
            SDL_free(entry->value.s);
        }
    }

    SDL_zeroa(entries);
    entry_count = 0;
}

bool Config_GetBool(const char* key) {
    const ConfigEntry* entry = find_entry(key);

    if (entry == NULL || entry->type != CFG_BOOL) {
        return false;
    }

    return entry->value.b;
}

int Config_GetInt(const char* key) {
    const ConfigEntry* entry = find_entry(key);

    if (entry == NULL || entry->type != CFG_INT) {
        return 0;
    }

    return entry->value.i;
}

const char* Config_GetString(const char* key) {
    const ConfigEntry* entry = find_entry(key);

    if (entry == NULL || entry->type != CFG_STRING) {
        return NULL;
    }

    return entry->value.s;
}

/* Config_SetString updates the in-memory entries table so subsequent
 * Config_GetString calls within the same session see the new value.
 * Config_Save is currently a no-op with a warn-once log so callers have a
 * stable symbol; real on-disk persistence is deferred until a caller
 * needs it. Config_Init's write_defaults() still bootstraps an on-disk
 * file so users can edit it manually as before. */

void Config_SetString(const char* key, const char* value) {
    if (key == NULL || value == NULL) {
        return;
    }

    ConfigEntry* existing = find_entry_in_array(key, entries, entry_count);
    if (existing != NULL) {
        if (existing->type == CFG_STRING) {
            char* dup = SDL_strdup(value);
            if (dup == NULL) {
                return;
            }
            SDL_free(existing->value.s);
            existing->value.s = dup;
        } else {
            existing->type = CFG_STRING;
            existing->value.s = SDL_strdup(value);
        }
        existing->coerced = false; /* fresh user value; allow re-coercion if needed later */
        return;
    }

    if (entry_count >= CONFIG_ENTRIES_MAX) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Config_SetString: entries table full (%d); dropping key '%s'",
                    CONFIG_ENTRIES_MAX,
                    key);
        return;
    }

    ConfigEntry* entry = &entries[entry_count++];
    entry->key = SDL_strdup(key);
    entry->type = CFG_STRING;
    entry->value.s = SDL_strdup(value);
}

void Config_SetBool(const char* key, bool value) {
    if (key == NULL) {
        return;
    }

    ConfigEntry* existing = find_entry_in_array(key, entries, entry_count);
    if (existing != NULL) {
        if (existing->type == CFG_STRING) {
            SDL_free(existing->value.s);
            existing->value.s = NULL;
        }
        existing->type = CFG_BOOL;
        existing->value.b = value;
        existing->coerced = false;
        return;
    }

    if (entry_count >= CONFIG_ENTRIES_MAX) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Config_SetBool: entries table full (%d); dropping key '%s'",
                    CONFIG_ENTRIES_MAX,
                    key);
        return;
    }

    ConfigEntry* entry = &entries[entry_count++];
    entry->key = SDL_strdup(key);
    entry->type = CFG_BOOL;
    entry->value.b = value;
}

void Config_Save(void) {
    static bool warned = false;
    if (!warned) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Config_Save: in-memory only (stub); on-disk persistence not yet wired.");
        warned = true;
    }
}
