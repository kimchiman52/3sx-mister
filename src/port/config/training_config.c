#include "port/config/training_config.h"
#include "constants.h"
#include "port/paths.h"
#include "structs.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/engine/workuser.h"

#if defined(DEBUG)
#include "main.h" /* configuration */
#include "quick_training.h"
#include "test/input_script.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#define TRAINING_CONFIG_MAGIC      0x54524E31  // "TRN1"
#define TRAINING_CONFIG_VERSION    2
#define TRAINING_CONFIG_VERSION_V1 1

typedef struct {
    u32 magic;
    u32 version;
    s8 contents[2][2][7];
    s8 cursor_x[2];
    s8 cursor_y[2];
    s8 super_arts[2];
    u8 my_char[2];
} TrainingConfigFile;

typedef struct {
    u32 magic;
    u32 version;
    s8 contents[2][2][6];
    s8 cursor_x[2];
    s8 cursor_y[2];
    s8 super_arts[2];
    u8 my_char[2];
} TrainingConfigFileV1;

_Static_assert(sizeof(TrainingConfigFile) == 44, "TrainingConfigFile must be 44 bytes");
_Static_assert(sizeof(TrainingConfigFileV1) == 40, "TrainingConfigFileV1 must be 40 bytes");

// Must match first 7 columns of Menu_Max_Data_Tr[2][2][9] in menu.c
static const s8 max_values[2][2][7] = {
    { { 4, 6, 2, 2, 0, 0, 0 }, { 3, 1, 3, 7, 1, 1, 1 } },
    { { 2, 3, 1, 3, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 } }
};

/* TrainingConfig_Load() memcpy's the whole `contents` block straight into the
 * live Training[] globals, sized by the FILE struct. The file struct and the
 * game struct are declared in different translation units and nothing tied
 * them together, so a change to TrainingData::contents alone would silently
 * make that copy over- or under-run the destination. */
_Static_assert(sizeof(((TrainingConfigFile*)0)->contents) == sizeof(Training[0].contents),
               "TrainingConfigFile::contents must match TrainingData::contents — "
               "TrainingConfig_Load memcpy's one into the other");

/* max_values is the clamp applied to every value read off disk before it
 * reaches Training[]. The clamp loop is bounded by the FILE struct's
 * dimensions and subscripts max_values with the same indices, so max_values
 * being smaller than what it bounds would turn the validation of an
 * untrusted file into an out-of-bounds read of the bounds table itself. */
_Static_assert(SDL_arraysize(max_values) ==
                   SDL_arraysize(((TrainingConfigFile*)0)->contents),
               "max_values player count must match TrainingConfigFile::contents");
_Static_assert(SDL_arraysize(max_values[0]) ==
                   SDL_arraysize(((TrainingConfigFile*)0)->contents[0]),
               "max_values type count must match TrainingConfigFile::contents");
_Static_assert(SDL_arraysize(max_values[0][0]) ==
                   SDL_arraysize(((TrainingConfigFile*)0)->contents[0][0]),
               "max_values slot count must match TrainingConfigFile::contents");

// Read either a V1 or V2 file from `f` into `out`. Returns false on error or
// unrecognized magic/version. V1 files migrate forward by zero-filling the
// new slot[6] columns.
static bool read_training_config(FILE* f, TrainingConfigFile* out) {
    u32 header[2];
    if (fread(header, sizeof(header), 1, f) != 1) {
        return false;
    }
    if (header[0] != TRAINING_CONFIG_MAGIC) {
        return false;
    }

    out->magic = header[0];
    out->version = TRAINING_CONFIG_VERSION;

    if (header[1] == TRAINING_CONFIG_VERSION) {
        // V2: read rest of struct
        u8* dst = (u8*)out + sizeof(header);
        size_t rest = sizeof(TrainingConfigFile) - sizeof(header);
        if (fread(dst, rest, 1, f) != 1) {
            return false;
        }
        return true;
    }

    if (header[1] == TRAINING_CONFIG_VERSION_V1) {
        TrainingConfigFileV1 v1;
        v1.magic = header[0];
        v1.version = header[1];
        u8* dst = (u8*)&v1 + sizeof(header);
        size_t rest = sizeof(TrainingConfigFileV1) - sizeof(header);
        if (fread(dst, rest, 1, f) != 1) {
            return false;
        }
        // Migrate: copy V1 contents into V2's first 6 slots, zero-fill slot 6.
        for (int id = 0; id < 2; id++) {
            for (int type = 0; type < 2; type++) {
                for (int slot = 0; slot < 6; slot++) {
                    out->contents[id][type][slot] = v1.contents[id][type][slot];
                }
                out->contents[id][type][6] = 0;
            }
        }
        memcpy(out->cursor_x, v1.cursor_x, sizeof(out->cursor_x));
        memcpy(out->cursor_y, v1.cursor_y, sizeof(out->cursor_y));
        memcpy(out->super_arts, v1.super_arts, sizeof(out->super_arts));
        memcpy(out->my_char, v1.my_char, sizeof(out->my_char));
        return true;
    }

    return false;
}

/* Open <pref>/training, parse it (V1 migrated forward) and apply the
 * max_values clamp. Shared by TrainingConfig_Load() and, under DEBUG, by
 * TrainingConfig_ReadDiskContents(), so the harness compares against exactly
 * the bytes a load would install rather than against a second reader that can
 * drift from this one. Returns false when there is no readable config. */
static bool read_clamped_training_config(TrainingConfigFile* out) {
    const char* pref_path = Paths_GetPrefPath();
    if (pref_path == NULL) {
        return false;
    }

    char path[512];
    SDL_snprintf(path, sizeof(path), "%straining", pref_path);

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }

    if (!read_training_config(f, out)) {
        fclose(f);
        return false;
    }
    fclose(f);

    // Bounds-check loaded values; clamp anything out of range to 0
    for (int id = 0; id < SDL_arraysize(out->contents); id++) {
        for (int type = 0; type < SDL_arraysize(out->contents[id]); type++) {
            for (int slot = 0; slot < SDL_arraysize(out->contents[id][type]); slot++) {
                if (out->contents[id][type][slot] < 0 ||
                    out->contents[id][type][slot] > max_values[id][type][slot]) {
                    out->contents[id][type][slot] = 0;
                }
            }
        }
    }

    return true;
}

bool TrainingConfig_Load(void) {
#if defined(DEBUG)
    /* Frame-data test harness: the caller (Default_Training_Data(0),
     * menu.c) already zero-filled Training[0]/[2].contents before calling
     * us, which is exactly the deterministic, machine-independent default
     * the harness wants (ACTION=STAND, GUARD-derived slots=0/AUTO,
     * S.A.GAUGE=0, etc. - see docs/plan-frame-data-harness.md). Loading
     * the user's real on-disk training config here would leak it into
     * the harness run instead: a saved ACTION=CPU/HUMAN (contents[0][0][0]
     * == 3 or 4) leaves control_pl_rno at DUMMY_ACTION_UNFORCED at the
     * gameplay-entry latch (menu.c:4347-4353), so the P2 dummy runs
     * cpu_algorithm (or reads real human input) instead of being pinned
     * to the script - verified live: with a stale ACTION=CPU config, the
     * trace shows P2/Ken throwing unrelated, non-deterministic attacks
     * (atk=1, char=11) instead of the scripted P1/Q move (atk=0, char=17)
     * ever showing up. Slots like S.A.GAUGE are also latched once at
     * effect_E3 init - before the script gets a chance to poke anything -
     * so garbage there can't be recovered later either. Skip the load
     * entirely while a harness script is loaded. */
    if (InputScript_IsLoaded()) {
        return false;
    }
#endif

    TrainingConfigFile file;
    if (!read_clamped_training_config(&file)) {
        return false;
    }

    memcpy(Training[0].contents, file.contents, sizeof(file.contents));
    memcpy(Training[2].contents, file.contents, sizeof(file.contents));

    return true;
}

#if defined(DEBUG)
bool TrainingConfig_ReadDiskContents(s8 out[2][2][7]) {
    TrainingConfigFile file;

    if (!read_clamped_training_config(&file)) {
        return false;
    }

    memcpy(out, file.contents, sizeof(file.contents));
    return true;
}
#endif

#if defined(DEBUG)
/* True when a DEBUG test session owns the selection, and so must not persist
 * anything over the maintainer's real config.
 *
 * WHY IT IS A CLASS AND NOT A LIST OF FLAGS. Every `--test-enable` session
 * dictates its own characters, super arts, stage and training slots -- from
 * the command line, from a scene preset, or from a spike's hardcoded
 * fallbacks -- and any of them that reaches the training pause menu hits
 * menu.c -> Setup_NTr_Data(), whose FIRST statement is TrainingConfig_Save().
 * That save writes contents out of Training[2] and cursor/arts/char straight
 * out of the live Cursor_X/Cursor_Y/Super_Arts/My_char globals, i.e. out of
 * the harness's own selection. Measured 2026-09-05 against a seeded home:
 * `--test-instant-jump` turned super_arts `01 02` into `00 00` and my_char
 * `0b 00` into `03 02`; `--test-enable --test-scene-preset
 * training-yun-ryu-ryu-stage` turned cursor `00 00 00 00` into
 * `06 05 01 02` and arts into `02 00`. This destroyed a maintainer's real
 * training file.
 *
 * A per-harness opt-in was rejected for the reason the netplay-harness gate
 * discovery exists: it makes safety something the NEXT harness author has to
 * remember, and the discovery rule (an OPT_BOOLEAN whose help says it runs
 * and exits) structurally cannot see a session-owning flag like
 * --test-instant-jump or --test-quick-training. The default is now safe and
 * the exception is named here.
 *
 * THE ONE EXCEPTION is Quick Training. Its whole contract is that the SHIPPED
 * sequence -- which does legitimately save, because on device that is how a
 * setting persists -- leaves the file byte-identical. Suppressing the save
 * for it would make both halves of that assertion (the in-process compare and
 * the gate's cmp) pass on an empty write, which is the identity trap the rest
 * of this harness was rewritten to avoid. */
static bool test_session_owns_training_config(void) {
    if (QuickTraining_TestActive()) {
        return false;
    }

    return configuration.test.enabled;
}
#endif

void TrainingConfig_Save(void) {
#if defined(DEBUG)
    /* Mirror of the TrainingConfig_Load() harness gate above: a script
     * that presses START (pause resume - Setup_NTr_Data, menu.c) or
     * triggers a character change / soft reset would otherwise persist
     * its pokes (e.g. the G directive's guard/stance slots) into the
     * user's real on-disk training config. */
    if (InputScript_IsLoaded()) {
        return;
    }

    if (test_session_owns_training_config()) {
        return;
    }
#endif

    const char* pref_path = Paths_GetPrefPath();
    if (pref_path == NULL) {
        return;
    }

    char path[512];
    SDL_snprintf(path, sizeof(path), "%straining", pref_path);

    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }

    TrainingConfigFile file;
    file.magic = TRAINING_CONFIG_MAGIC;
    file.version = TRAINING_CONFIG_VERSION;
    memcpy(file.contents, Training[2].contents, sizeof(file.contents));
    memcpy(file.cursor_x, Cursor_X, sizeof(file.cursor_x));
    memcpy(file.cursor_y, Cursor_Y, sizeof(file.cursor_y));
    memcpy(file.super_arts, Super_Arts, sizeof(file.super_arts));
    memcpy(file.my_char, My_char, sizeof(file.my_char));

    if (fwrite(&file, sizeof(file), 1, f) != 1) {
        fclose(f);
        remove(path);
        return;
    }
    fclose(f);
}

bool TrainingConfig_GetLastUsed(s8 chars_out[2], s8 arts_out[2]) {
    const char* pref_path = Paths_GetPrefPath();
    if (pref_path == NULL) {
        return false;
    }

    char path[512];
    SDL_snprintf(path, sizeof(path), "%straining", pref_path);

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }

    TrainingConfigFile file;
    if (!read_training_config(f, &file)) {
        fclose(f);
        return false;
    }
    fclose(f);

    /* Per-slot validation, mirroring TrainingConfig_RestoreCharSelect's
     * clamps: out-of-range stored values leave the caller's defaults in
     * place. my_char is stored unvalidated (u8 straight off My_char[]),
     * so the range check here is load-bearing. */
    for (int i = 0; i < 2; i++) {
        if (file.my_char[i] < NUM_CHARS) {
            chars_out[i] = (s8)file.my_char[i];
        }
        if (file.super_arts[i] >= 0 && file.super_arts[i] < 3) {
            arts_out[i] = file.super_arts[i];
        }
    }

    return true;
}

void TrainingConfig_RestoreCharSelect(void) {
    const char* pref_path = Paths_GetPrefPath();
    if (pref_path == NULL) {
        return;
    }

    char path[512];
    SDL_snprintf(path, sizeof(path), "%straining", pref_path);

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }

    TrainingConfigFile file;
    if (!read_training_config(f, &file)) {
        fclose(f);
        return;
    }
    fclose(f);

    // Bounds-check cursor positions (grid is 8 columns x 3 rows)
    for (int i = 0; i < 2; i++) {
        if (file.cursor_x[i] >= 0 && file.cursor_x[i] < 8) {
            Cursor_X[i] = file.cursor_x[i];
        }
        if (file.cursor_y[i] >= 0 && file.cursor_y[i] < 3) {
            Cursor_Y[i] = file.cursor_y[i];
        }
        if (file.super_arts[i] >= 0 && file.super_arts[i] < 3) {
            Arts_Y[i] = file.super_arts[i];
            Last_Super_Arts[i] = file.super_arts[i];
        }
        // Set Last_My_char2 so sel_pl.c sees "same character" and doesn't reset Arts_Y
        Last_My_char2[i] = file.my_char[i];
    }
}
