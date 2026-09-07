#include "sf33rd/Source/PS2/mc/savesub.h"
#include "common.h"
#include "main.h"
#include "port/paths.h"
#include "port/utils.h"
#include "structs.h"
#include "sf33rd/Source/Game/menu/dir_data.h"
#include "sf33rd/Source/Game/menu/ex_data.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"

#include <SDL3/SDL.h>

#define SETTINGS_VERSION_V1 1
#define SETTINGS_VERSION_V2 2
#define SETTINGS_VERSION SETTINGS_VERSION_V2
// V1/V2 sizes match upstream's. V2 (upstream #371) appends the 1-byte
// Language field after Screen_Size; a V1 (367-byte) file still loads via
// the multi-format table below and gets Get_Default_Language() for the
// missing field. Historical note: this fork carried a +40-byte
// PL_Color[2][20] extension here (fork-local unlocked-color persistence
// predating upstream #296) until #296 "Unlock extra colors and Gill by
// default" was taken: extra colors and Gill are unlocked unconditionally
// now, PL_Color was dropped from struct _SAVE_W (include/structs.h), and
// this size shrank back to upstream's. A pre-existing settings save file
// written under the old (407-byte) size is rejected by the size gate in
// read_file_if_exists() below and falls back to defaults - nothing shipped
// with this fork carries a settings save yet, so that's an acceptable
// one-time reset.
#define SETTINGS_SIZE_V1 367
#define SETTINGS_SIZE_V2 368
#define SETTINGS_SIZE SETTINGS_SIZE_V2

#define SYSDIR_VERSION 1
#define SYSDIR_SIZE_V1 71
#define SYSDIR_SIZE SYSDIR_SIZE_V1

#define REPLAY_VERSION 1
#define REPLAY_SIZE_V1 0
#define REPLAY_SIZE REPLAY_SIZE_V1

/* === Layout claims above, made mechanical ===
 *
 * The two clamp loops in deserialize_settings / deserialize_sysdir iterate
 * over the dimensions of the SAVED struct (`extra_option.contents`,
 * `SystemDir::contents`) but subscript a DIFFERENT array with the same
 * indices — the per-item ceiling tables Ex_Menu_Max_Data / Dir_Menu_Max_Data.
 * Those loops are the validation barrier for an attacker-controlled file, so
 * if a ceiling table is ever smaller than the struct it bounds, the code that
 * exists to make a hostile save safe becomes an out-of-bounds read on that
 * hostile save. The two arrays are declared in different translation units
 * from the struct, nothing relates them, and the comments at the loops assert
 * they are "the same table" in prose only. */
_Static_assert(SDL_arraysize(Ex_Menu_Max_Data) ==
                   SDL_arraysize(((_EXTRA_OPTION*)0)->contents),
               "Ex_Menu_Max_Data page count must match _EXTRA_OPTION::contents — "
               "deserialize_settings() bounds the loop by contents and indexes "
               "Ex_Menu_Max_Data with it");
_Static_assert(SDL_arraysize(Ex_Menu_Max_Data[0]) ==
                   SDL_arraysize(((_EXTRA_OPTION*)0)->contents[0]),
               "Ex_Menu_Max_Data item count must match _EXTRA_OPTION::contents — "
               "deserialize_settings() bounds the loop by contents and indexes "
               "Ex_Menu_Max_Data with it");
_Static_assert(SDL_arraysize(Dir_Menu_Max_Data) ==
                   SDL_arraysize(((SystemDir*)0)->contents),
               "Dir_Menu_Max_Data page count must match SystemDir::contents — "
               "deserialize_sysdir() bounds the loop by contents and indexes "
               "Dir_Menu_Max_Data with it");
_Static_assert(SDL_arraysize(Dir_Menu_Max_Data[0]) ==
                   SDL_arraysize(((SystemDir*)0)->contents[0]),
               "Dir_Menu_Max_Data item count must match SystemDir::contents — "
               "deserialize_sysdir() bounds the loop by contents and indexes "
               "Dir_Menu_Max_Data with it");

/* The on-disk sizes are the gate read_file_if_exists() accepts a file by, so
 * they are a property of what serialize_* actually writes, not free constants.
 * SYSDIR_SIZE_V1 is fully derivable; pin it rather than restate 71. */
_Static_assert(SYSDIR_SIZE_V1 == 1 + sizeof(((SystemDir*)0)->contents),
               "SYSDIR_SIZE_V1 must equal the version byte plus SystemDir::contents");
/* "V2 (upstream #371) appends the 1-byte Language field after Screen_Size;
 * a V1 (367-byte) file still loads" — the whole multi-format load path is
 * built on V2 being exactly one byte longer than V1. */
_Static_assert(SETTINGS_SIZE_V2 == SETTINGS_SIZE_V1 + 1,
               "SETTINGS_SIZE_V2 must be exactly one byte longer than V1 — V2 "
               "appends only the Language byte");

#define ROOT_DIR "saves"
#define PATH_LEN_MAX 128

typedef enum SaveState {
    SAVE_STATE_IDLE,
    SAVE_STATE_INIT,
    SAVE_STATE_WORKING,
    SAVE_STATE_ERROR,
} SaveState;

typedef struct SaveOperation {
    SaveState state;
    SaveFileType file_type;
    SaveMode mode;
    SDL_Storage* storage;
} SaveOperation;

typedef enum ReadResult {
    READ_SUCCESS,
    READ_NO_FILE,
    READ_SIZE_MISMATCH,
    READ_ERROR,
} ReadResult;

typedef void (*SerializeHandler)(SDL_IOStream* io);
typedef bool (*DeserializeHandler)(SDL_IOStream* io);

typedef struct SaveFileFormat {
    Uint8 version;
    Uint64 size;
} SaveFileFormat;

typedef struct SaveFileInfo {
    const char* name;
    const SaveFileFormat* formats;
    int format_count;
    SerializeHandler serialize_handler;
    DeserializeHandler deserialize_handler;
} SaveFileInfo;

static SaveOperation operation = { 0 };

static void serialize_settings(SDL_IOStream* io) {
    Save_Game_Data();

    const struct _SAVE_W* src = &save_w[1];
    SDL_WriteU8(io, SETTINGS_VERSION);

    for (int i = 0; i < SDL_arraysize(src->Pad_Infor); i++) {
        const _PAD_INFOR* pad_info = &src->Pad_Infor[i];

        for (int j = 0; j < SDL_arraysize(pad_info->Shot); j++) {
            SDL_WriteU8(io, pad_info->Shot[j]);
        }

        SDL_WriteU8(io, pad_info->Vibration);
    }

    SDL_WriteU8(io, src->Difficulty);
    SDL_WriteS8(io, src->Time_Limit);
    SDL_WriteU8(io, src->Battle_Number[0]);
    SDL_WriteU8(io, src->Battle_Number[1]);
    SDL_WriteU8(io, src->Damage_Level);
    SDL_WriteU8(io, src->Handicap);
    SDL_WriteU8(io, src->Partner_Type[0]);
    SDL_WriteU8(io, src->Partner_Type[1]);
    SDL_WriteS8(io, src->Adjust_X);
    SDL_WriteS8(io, src->Adjust_Y);
    SDL_WriteU8(io, src->Screen_Size);
    SDL_WriteU8(io, src->Language);
    SDL_WriteU8(io, src->GuardCheck);
    SDL_WriteU8(io, src->AnalogStick);
    SDL_WriteU8(io, src->BgmType);
    SDL_WriteU8(io, src->BGM_Level);
    SDL_WriteU8(io, src->SE_Level);
    SDL_WriteIO(io, &src->extra_option, sizeof(src->extra_option));

    for (int i = 0; i < SDL_arraysize(src->Ranking); i++) {
        const RANK_DATA* rank_data = &src->Ranking[i];

        SDL_WriteIO(io, rank_data->name, sizeof(rank_data->name));
        SDL_WriteU16BE(io, rank_data->player);
        SDL_WriteU32BE(io, rank_data->score);
        SDL_WriteS8(io, rank_data->cpu_grade);
        SDL_WriteS8(io, rank_data->grade);
        SDL_WriteU16BE(io, rank_data->wins);
        SDL_WriteU8(io, rank_data->player_color);
        SDL_WriteU8(io, rank_data->all_clear);
    }
}

static bool deserialize_settings(SDL_IOStream* io) {
    struct _SAVE_W* dst = &save_w[1];
    Uint8 version;

    SDL_ReadU8(io, &version);

    if (version != SETTINGS_VERSION_V1 && version != SETTINGS_VERSION_V2) {
        return false;
    }

    for (int i = 0; i < SDL_arraysize(dst->Pad_Infor); i++) {
        _PAD_INFOR* pad_info = &dst->Pad_Infor[i];

        for (int j = 0; j < SDL_arraysize(pad_info->Shot); j++) {
            SDL_ReadU8(io, &pad_info->Shot[j]);

            // Shot[] is a raw button-ID index consumed unchecked by
            // Convert_Data[Pad_Infor[PL_id].Shot[n]] (sys_sub.c ~:123-151).
            // Convert_Data has 12 entries (sys_sub.c:68) and the menu UI's own
            // increment/decrement wraps at 11 (menu.c ~:1904-1917, `max = 11`
            // for all Shot slots), so 0..11 is the authoritative range.
            if (pad_info->Shot[j] > 11) {
                pad_info->Shot[j] = 11;
            }
        }

        SDL_ReadU8(io, &pad_info->Vibration);
        pad_info->Vibration = pad_info->Vibration ? 1 : 0;
    }

    SDL_ReadU8(io, &dst->Difficulty);
    // Difficulty indexes Level_18_Data[9][17] (sys_sub.c:811) and Training's
    // Control_Time scaling (sys_sub.c:836). The menu's own Game Option screen
    // clamps it to Game_Option_Index_Data[0] == 7 (menu.c:1766), so 0..7 is
    // the authoritative range.
    if (dst->Difficulty > 7) {
        dst->Difficulty = 7;
    }
    SDL_ReadS8(io, &dst->Time_Limit);
    SDL_ReadU8(io, &dst->Battle_Number[0]);
    SDL_ReadU8(io, &dst->Battle_Number[1]);
    SDL_ReadU8(io, &dst->Damage_Level);
    // Damage_Level indexes Com_Vital_Unit_Data[20][4][12] (pls02.c:833); its
    // second dimension is 4, matching the menu's own clamp
    // (Game_Option_Index_Data[4] == 3, menu.c:1766), so 0..3 is authoritative.
    if (dst->Damage_Level > 3) {
        dst->Damage_Level = 3;
    }
    SDL_ReadU8(io, &dst->Handicap);
    SDL_ReadU8(io, &dst->Partner_Type[0]);
    SDL_ReadU8(io, &dst->Partner_Type[1]);
    SDL_ReadS8(io, &dst->Adjust_X);
    SDL_ReadS8(io, &dst->Adjust_Y);
    SDL_ReadU8(io, &dst->Screen_Size);

    if (version == SETTINGS_VERSION_V2) {
        SDL_ReadU8(io, &dst->Language);
        // Language flows into Convert_Buff[2][0][4] (sys_sub.c Copy_Save_w)
        // and mpp_w.language; the Convert_Buff copy is used unchecked as a
        // display index — eff64.c:197 indexes
        // Letter_Data_64[char_index][disp_index], whose language row only
        // populates entries 0..1 ("EN"/"JP", the rest are NULL), so an
        // out-of-range value from a corrupt/hostile save is a NULL deref or
        // OOB read — the same class as the system_dir/extra_option clamps
        // below. The menu UI itself only ever produces 0..1 (Language_Toggle,
        // menu.c), so 0..1 is the authoritative range.
        if (dst->Language > LANG_JAPANESE) {
            dst->Language = LANG_JAPANESE;
        }
    } else {
        dst->Language = Get_Default_Language();
    }

    SDL_ReadU8(io, &dst->GuardCheck);
    SDL_ReadU8(io, &dst->AnalogStick);
    SDL_ReadU8(io, &dst->BgmType);
    // BgmType indexes bgm_table[2]/bgm_exdata[2]/bgm_selector[2]/
    // Letter_Data_A8[2] (sound3rd.c, effa8.c) — always a 2-entry table
    // (BGM_ARRANGED/BGM_ORIGINAL, structs.h), so 0..1 is authoritative.
    if (dst->BgmType > 1) {
        dst->BgmType = 1;
    }
    SDL_ReadU8(io, &dst->BGM_Level);
    SDL_ReadU8(io, &dst->SE_Level);
    // BGM_Level/SE_Level are not array indices, but every consumer divides by
    // the fixed 15 (sound3rd.c ~:283/377/617) that the default/reset value
    // also uses (sound3rd.c:156-157, menu.c:2414-2415), so clamp to 0..15 to
    // keep them in the range the volume math was designed for.
    if (dst->BGM_Level > 15) {
        dst->BGM_Level = 15;
    }
    if (dst->SE_Level > 15) {
        dst->SE_Level = 15;
    }
    SDL_ReadIO(io, &dst->extra_option, sizeof(dst->extra_option));

    // extra_option.contents[page][item] is read raw with no per-field
    // validation and is used unchecked as an array index — the same shape
    // of bug as system_dir.contents fixed above: effc4.c:80 indexes
    // Ex_Letter_Data[page][char][extra_option.contents[page][item]].
    // Ex_Letter_Data is [4][7][17] (ex_data.c), and Ex_Menu_Max_Data[4][8]
    // (ex_data.c) is the same per-item upper-bound table the menu UI itself
    // clamps against (menu.c:1260 reads Ex_Page_Data for the page's item
    // count; the per-item value ceiling is Ex_Menu_Max_Data), so it is the
    // authoritative bound here too.
    for (int page = 0; page < SDL_arraysize(dst->extra_option.contents); page++) {
        for (int item = 0; item < SDL_arraysize(dst->extra_option.contents[page]); item++) {
            s8* value = &dst->extra_option.contents[page][item];

            if (*value < 0) {
                *value = 0;
            } else if (*value > (s8)Ex_Menu_Max_Data[page][item]) {
                *value = (s8)Ex_Menu_Max_Data[page][item];
            }
        }
    }

    for (int i = 0; i < SDL_arraysize(dst->Ranking); i++) {
        RANK_DATA* rank_data = &dst->Ranking[i];

        SDL_ReadIO(io, rank_data->name, sizeof(rank_data->name));
        SDL_ReadU16BE(io, &rank_data->player);
        SDL_ReadU32BE(io, &rank_data->score);
        SDL_ReadS8(io, &rank_data->cpu_grade);
        SDL_ReadS8(io, &rank_data->grade);
        SDL_ReadU16BE(io, &rank_data->wins);
        SDL_ReadU8(io, &rank_data->player_color);
        SDL_ReadU8(io, &rank_data->all_clear);
    }

    Copy_Save_w();
    Copy_Check_w();

    // SJ-30 (docs/savestates-and-instant-mode-jump.md Sec 10.9): the load above
    // writes save_w[1] and nothing else, but Convert_User_Setting() (sys_sub.c)
    // indexes save_w[Present_Mode], and training runs at Present_Mode 4/5. Until
    // this call the ONLY thing that ever carried the loaded mapping into those
    // slots was Save_Game_Data(), whose callers are the Game Option / Button
    // Config / Screen Adjust screens (menu.c) -- so every route into Training
    // that skipped those screens ran the DEFAULT pad mapping, not the user's.
    // Measured on the quick-training gate harness with a seeded non-identity
    // settings file: `PROBE: Present_Mode=4 sw1=5,4,3,11,2,1,0,11
    // sw4=0,1,2,11,3,4,5,11`.
    //
    // Same function Save_Game_Data() calls, so the two cannot disagree, and the
    // same restricted field set -- see Copy_Save_w_Training() for why the carry
    // covers Pad_Infor + GuardCheck and stops at slots 4/5.
    //
    // Placement: this runs from Init_Task_Aload (init3rd.c), which is reached
    // AFTER Init_Task_1st -> Game_Data_Init() -> Setup_Default_Game_Option()
    // has re-seeded all six slots from Game_Default_Data -- at boot and after
    // every Soft_Reset_Sub(). Carrying anywhere earlier in the walk would be
    // wiped by that re-seed.
    Copy_Save_w_Training();

    sys_w.bgm_type = save_w[1].BgmType;
    return true;
}

static void serialize_sysdir(SDL_IOStream* io) {
    const SystemDir* src = &system_dir[1];

    SDL_WriteU8(io, SYSDIR_VERSION);

    for (int page = 0; page < SDL_arraysize(src->contents); page++) {
        for (int item = 0; item < SDL_arraysize(src->contents[page]); item++) {
            SDL_WriteS8(io, src->contents[page][item]);
        }
    }
}

static bool deserialize_sysdir(SDL_IOStream* io) {
    SystemDir loaded = { 0 };
    Uint8 version;

    SDL_ReadU8(io, &version);

    if (version != SYSDIR_VERSION) {
        return false;
    }

    for (int page = 0; page < SDL_arraysize(loaded.contents); page++) {
        for (int item = 0; item < SDL_arraysize(loaded.contents[page]); item++) {
            SDL_ReadS8(io, &loaded.contents[page][item]);

            // Clamp to the option's valid range. These values are used as array
            // indices with no further validation (e.g. eff51.c:64 indexes
            // Letter_Data_51[page][char][system_dir[1].contents[page][item]]);
            // Dir_Menu_Max_Data[page][item] (dir_data.c) is the same table the
            // menu UI itself clamps against when incrementing/decrementing an
            // option, so it is the authoritative per-item upper bound.
            if (loaded.contents[page][item] < 0) {
                loaded.contents[page][item] = 0;
            } else if (loaded.contents[page][item] > (s8)Dir_Menu_Max_Data[page][item]) {
                loaded.contents[page][item] = (s8)Dir_Menu_Max_Data[page][item];
            }
        }
    }

    system_dir[1] = loaded;
    return true;
}

static void serialize_stub(SDL_IOStream* io) {
    fatal_error("Not implemented");
}

static bool deserialize_stub(SDL_IOStream* io) {
    fatal_error("Not implemented");
}

static const SaveFileFormat settings_formats[] = {
    { SETTINGS_VERSION_V1, SETTINGS_SIZE_V1 },
    { SETTINGS_VERSION_V2, SETTINGS_SIZE_V2 },
};

static const SaveFileFormat sysdir_formats[] = {
    { SYSDIR_VERSION, SYSDIR_SIZE },
};

static const SaveFileFormat replay_formats[] = {
    { REPLAY_VERSION, REPLAY_SIZE },
};

static const SaveFileInfo file_info[] = {
    [SAVE_FILE_SETTINGS] = { .name = "settings",
                             .formats = settings_formats,
                             .format_count = SDL_arraysize(settings_formats),
                             .serialize_handler = serialize_settings,
                             .deserialize_handler = deserialize_settings },
    [SAVE_FILE_SYSTEM_DIRECTION] = { .name = "sysdir",
                                     .formats = sysdir_formats,
                                     .format_count = SDL_arraysize(sysdir_formats),
                                     .serialize_handler = serialize_sysdir,
                                     .deserialize_handler = deserialize_sysdir },
    [SAVE_FILE_REPLAY] = { .name = "replay",
                           .formats = replay_formats,
                           .format_count = SDL_arraysize(replay_formats),
                           .serialize_handler = serialize_stub,
                           .deserialize_handler = deserialize_stub },
};

typedef enum PathKind {
    PATH_KIND_MAIN,
    PATH_KIND_BACKUP,
    PATH_KIND_TMP,
} PathKind;

static void make_path(char* text, size_t maxlen, const char* name, PathKind kind) {
    switch (kind) {
    case PATH_KIND_BACKUP:
        SDL_snprintf(text, maxlen, "%s/%s.bak", ROOT_DIR, name);
        break;
    case PATH_KIND_TMP:
        SDL_snprintf(text, maxlen, "%s/%s.tmp", ROOT_DIR, name);
        break;
    case PATH_KIND_MAIN:
    default:
        SDL_snprintf(text, maxlen, "%s/%s", ROOT_DIR, name);
        break;
    }
}

static const SaveFileFormat* current_format(const SaveFileInfo* file) {
    return &file->formats[file->format_count - 1];
}

static ReadResult read_file_if_exists(SDL_Storage* storage, const char* path, void* buffer, const SaveFileInfo* file) {
    SDL_PathInfo info;
    const SaveFileFormat* format = NULL;

    if (!SDL_GetStoragePathInfo(storage, path, &info)) {
        return READ_ERROR;
    }

    if (info.type != SDL_PATHTYPE_FILE) {
        return READ_NO_FILE;
    }

    for (int i = 0; i < file->format_count; i++) {
        if (info.size == file->formats[i].size) {
            format = &file->formats[i];
            break;
        }
    }

    if (format == NULL) {
        return READ_SIZE_MISMATCH;
    }

    if (!SDL_ReadStorageFile(storage, path, buffer, format->size)) {
        return READ_ERROR;
    }

    if (*(const Uint8*)buffer != format->version) {
        return READ_ERROR;
    }

    return READ_SUCCESS;
}

static bool create_saves_dir(SDL_Storage* storage) {
    const char* dir = ROOT_DIR;
    bool dir_exists = SDL_GetStoragePathInfo(storage, dir, NULL);

    if (!dir_exists) {
        dir_exists = SDL_CreateStorageDirectory(storage, dir);
    }

    return dir_exists;
}

void SaveInit(SaveFileType file_type, SaveMode save_mode) {
    operation.state = SAVE_STATE_INIT;
    operation.file_type = file_type;
    operation.mode = save_mode;
}

s32 SaveMove() {
    // SAVE_FILE_REPLAY has no real serialize/deserialize handler yet
    // (file_info[SAVE_FILE_REPLAY] points at serialize_stub/deserialize_stub,
    // which both call fatal_error() and abort). Three live menu paths still
    // call SaveInit(SAVE_FILE_REPLAY, ...) (menu.c:1435, 3532, 5279). Rather
    // than crash on every one of them, make REPLAY a graceful no-op here:
    // complete immediately without touching storage. This is a deliberate
    // deviation from upstream (which has a real replay save/load
    // implementation this fork has not ported) — logged once so the gap
    // stays visible without spamming the log on repeat menu visits.
    if (operation.state == SAVE_STATE_INIT && operation.file_type == SAVE_FILE_REPLAY) {
        static bool warned = false;

        if (!warned) {
            SDL_Log("[savesub] SAVE_FILE_REPLAY is a no-op stub (no replay serializer ported); skipping.");
            warned = true;
        }

        SDL_zero(operation);
        return 0;
    }

    switch (operation.state) {
    case SAVE_STATE_IDLE:
        return 0;

    case SAVE_STATE_INIT:
        operation.storage = Paths_OpenUserStorage(0);

        if (operation.storage == NULL) {
            operation.state = SAVE_STATE_ERROR;
            return -1;
        }

        operation.state = SAVE_STATE_WORKING;
        /* fallthrough */

    case SAVE_STATE_WORKING:
        if (!SDL_StorageReady(operation.storage)) {
            return 1;
        }

        const SaveFileInfo* info = &file_info[operation.file_type];
        char path[PATH_LEN_MAX];
        char backup_path[PATH_LEN_MAX];
        char tmp_path[PATH_LEN_MAX];
        make_path(path, sizeof(path), info->name, PATH_KIND_MAIN);
        make_path(backup_path, sizeof(backup_path), info->name, PATH_KIND_BACKUP);
        make_path(tmp_path, sizeof(tmp_path), info->name, PATH_KIND_TMP);
        const SaveFileFormat* format = current_format(info);
        void* buffer = SDL_malloc(format->size);

        if (buffer == NULL) {
            // Unlike the SAVE_STATE_INIT->ERROR transition above (where
            // operation.storage is still NULL), we're holding an open
            // storage handle here — close it ourselves since the
            // SAVE_STATE_ERROR case below does not.
            SDL_CloseStorage(operation.storage);
            SDL_zero(operation);
            operation.state = SAVE_STATE_ERROR;
            return -1;
        }

        SDL_IOStream* io = NULL;
        bool success = false;

        switch (operation.mode) {
        case SAVE_MODE_LOAD:
            io = SDL_IOFromConstMem(buffer, format->size);
            const char* paths[] = { path, backup_path };

            for (int i = 0; i < SDL_arraysize(paths); i++) {
                const char* _path = paths[i];
                SDL_SeekIO(io, 0, SDL_IO_SEEK_SET);

                if (read_file_if_exists(operation.storage, _path, buffer, info) == READ_SUCCESS) {
                    success = info->deserialize_handler(io);
                }

                if (success) {
                    break;
                }
            }

            break;

        case SAVE_MODE_SAVE:
            io = SDL_IOFromMem(buffer, format->size);
            info->serialize_handler(io);

            const bool saves_dir_exists = create_saves_dir(operation.storage);

            if (saves_dir_exists) {
                bool backup_success = false;
                const bool save_file_exists = SDL_GetStoragePathInfo(operation.storage, path, NULL);

                // .bak is taken first, before any write to the tmp/main file, so that
                // whichever moment a power cut lands in, at least one of {backup_path,
                // path} is a complete, intact prior copy.
                if (save_file_exists) {
                    backup_success = SDL_CopyStorageFile(operation.storage, path, backup_path);
                } else {
                    backup_success = true; // Nothing to backup
                }

                if (backup_success) {
                    // Atomic write: stage the new content at tmp_path, then rename it
                    // over path. A truncate-and-rewrite of path directly leaves a
                    // window where the file is empty/partial; a power cut on vfat in
                    // that window can corrupt both the main file and (mid-copy) the
                    // .bak above. SDL_RenameStoragePath -> SDL_RenamePath -> POSIX
                    // rename() atomically replaces an existing destination on the
                    // generic storage backend's Linux target (verified against SDL
                    // release-3.4.4 src/filesystem/posix/SDL_sysfsops.c).
                    success = SDL_WriteStorageFile(operation.storage, tmp_path, buffer, format->size);

                    if (success) {
                        success = SDL_RenameStoragePath(operation.storage, tmp_path, path);

                        if (!success) {
                            // Fallback for a backend/filesystem where rename does not
                            // replace an existing destination: remove path first (the
                            // .bak taken above is still intact at this point) then
                            // retry the rename.
                            SDL_RemoveStoragePath(operation.storage, path);
                            success = SDL_RenameStoragePath(operation.storage, tmp_path, path);
                        }
                    }
                }
            }

            break;
        }

        SDL_CloseIO(io);
        SDL_free(buffer);
        SDL_CloseStorage(operation.storage);
        SDL_zero(operation);
        return success ? 0 : -1;

    case SAVE_STATE_ERROR:
        return -1;
    }

    return -1;
}
