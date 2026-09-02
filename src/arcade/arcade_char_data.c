#include "arcade/arcade_char_data.h"
#include "arcade/cps3_first_light.h"
#include "arcade/rom_load.h"
#include "constants.h"
#include "port/paths.h"
#include "structs.h"
#include "utils/sha256.h"

#include <SDL3/SDL.h>

#define BASE_OFFSET 0x6000000

// remap_cg_number's early-return threshold below which a raw cg_number is
// never remapped. Moved 0x800 -> 0x400 in ae309dc4; see the _Static_assert
// beside remy_cg_ranges, which depends on this staying <= 0x601.
#define CG_REMAP_CUTOFF 0x400

// Uncomment to enable parsed char data dumping to dump folder
// #define DUMP_CHAR_DATA

typedef struct Location {
    Uint32 offset;
    Uint32 size;
} Location;

typedef struct LocationData {
    Location nmca;
    Location dmca;
    Location btca;
    Location caca;
    Location cuca;
    Location atca;
    Location saca;
    Location exca;
    Location cbca;
    Location yuca;
    Location stxy;
    Location mvxy;
    Location sernd;
    Location ovct;
    Location ovix;
    Location rict;
    Location hiit;
    Location boda;
    Location hana;
    Location cata;
    Location caua;
    Location atta;
    Location hosa;
    Location atit;
    Location prot;
} LocationData;

_Static_assert(
    sizeof(LocationData) == CHAR_DATA_SECTION_COUNT * sizeof(Location), "LocationData must follow CharDataSection order"
);

typedef struct CgRemapRange {
    Uint16 first;
    Uint16 last;
    Sint32 delta;
} CgRemapRange;

typedef struct CharacterCgMap {
    Sint32 default_delta;
    const CgRemapRange* ranges;
    size_t range_count;
} CharacterCgMap;

/* cg_se sound-code exception (doc §8.Q / §21). `from`/`to` are the upper 12
 * bits of the cell's cg_se field -- the low nibble is flip/priority
 * (charset.c -> check_cgd_data does `wk->cg_se >>= 4;` before dispatch) and
 * is preserved by remap_cg_se. Keyed per character on purpose: 0x2FB is a
 * legitimate Urien voice and 0x3DF a legitimate Twelve voice, so a global
 * code->code table would silently break them (§21.8). */
typedef struct CgSeRemapPair {
    Uint16 from;
    Uint16 to;
} CgSeRemapPair;

typedef struct CharacterCgSeMap {
    const CgSeRemapPair* pairs;
    size_t pair_count;
} CharacterCgSeMap;

static CharDataImage data[NUM_CHARS] = { 0 };
static bool initialized = false;

// Forward decls
static const CharacterCgMap cg_maps[NUM_CHARS];
static const CharacterCgSeMap cg_se_maps[NUM_CHARS];
static const LocationData location_data[NUM_CHARS];
static const size_t section_element_sizes[CHAR_DATA_SECTION_COUNT];
#if defined(DEBUG)
static void validate_cg_ranges(void);
#endif

static int SDLCALL compare_u32(const void* lhs, const void* rhs) {
    const Uint32 a = *(Uint32*)lhs;
    const Uint32 b = *(Uint32*)rhs;

    if (a < b) {
        return -1;
    } else if (a > b) {
        return 1;
    } else {
        return 0;
    }
}

static Uint16 remap_cg_number(Uint16 value, Character character) {
    if (value < CG_REMAP_CUTOFF) {
        return value;
    }

    const CharacterCgMap* map = &cg_maps[character];
    Sint32 delta = map->default_delta;

    for (size_t i = 0; i < map->range_count; i++) {
        const CgRemapRange* range = &map->ranges[i];

        if (value >= range->first && value <= range->last) {
            delta = range->delta;
            break;
        }
    }

    const Sint32 adjusted = value + delta;

    if (adjusted < 0 || adjusted > UINT16_MAX) {
        return value;
    }

    return adjusted;
}

/* Translate a raw CPS3 cg_se into the PS2 sound namespace the port's audio
 * actually holds (the samples come from SF33RD.AFS, not the CPS3 romset --
 * doc §21.2). Exceptions only: outside a character's pair list the value is
 * byte-passed exactly as before. The low flip/priority nibble survives
 * untouched. Parse-time on purpose -- remaking the code later in
 * sound3rd.c -> remake_sound_code_for_DC has no character context, runs
 * after the per-player +uid*0x300 adjustment, and would leave the
 * sound_effect_request[] dispatch entry wrong (§21.12). */
static Uint16 remap_cg_se(Uint16 value, Character character) {
    const CharacterCgSeMap* map = &cg_se_maps[character];
    const Uint16 code = value >> 4;

    for (size_t i = 0; i < map->pair_count; i++) {
        if (map->pairs[i].from == code) {
            return (Uint16)((map->pairs[i].to << 4) | (value & 0xF));
        }
    }

    return value;
}

static const void* read_char_table(SDL_IOStream* rom, Location location, Character character) {
    void* result = SDL_malloc(location.size);
    SDL_memset(result, 0, location.size);

    // Read script offsets
    Uint32* offsets = result;
    int offset_count = 0;
    SDL_SeekIO(rom, location.offset, SDL_IO_SEEK_SET);

    for (;; offset_count++) {
        Uint32 value = 0;
        SDL_ReadU32BE(rom, &value);

        if (value == 0) {
            offsets[offset_count] = 0;
            break;
        }

        offsets[offset_count] = value - BASE_OFFSET - location.offset;
    }

    // Calculate script sizes
    const size_t script_offsets_size = offset_count * sizeof(Uint32);
    Uint32* script_offsets = SDL_malloc(script_offsets_size);
    SDL_memcpy(script_offsets, offsets, script_offsets_size);
    SDL_qsort(script_offsets, offset_count, sizeof(script_offsets[0]), compare_u32);

    // Parse data

    for (int i = 0; i < offset_count; i++) {
        const Uint32 start_offset = script_offsets[i] - 8;
        const Uint32 end_offset = (i == (offset_count - 1)) ? location.size : script_offsets[i + 1] - 8;

        SDL_SeekIO(rom, location.offset + start_offset, SDL_IO_SEEK_SET);
        Uint8* p = (Uint8*)result + start_offset;

        // Read script header
        Sint16 cgd_type = 0;
        SDL_ReadS16BE(rom, &cgd_type);
        SDL_assert(cgd_type == 1 || cgd_type == 2 || cgd_type == 4 || cgd_type == 6);
        *(Sint16*)p = cgd_type;
        p += 2;

        for (int i = 0; i < 6; i++) {
            SDL_ReadU8(rom, p); // pat_status ... sp_tech_id
            p += 1;
        }

        while (p < (Uint8*)result + end_offset) {
            Uint16 code = 0;
            SDL_ReadU16BE(rom, &code);

            if (code < 0x100) {
                *(Uint16*)p = code;
                p += 2;

                for (int i = 0; i < 3; i++) {
                    SDL_ReadS16BE(rom, p); // koc ... pat
                    p += 2;
                }

                const int left_to_move = SDL_max(cgd_type * 4 - 8, 0);
                p += left_to_move;
                SDL_SeekIO(rom, left_to_move, SDL_IO_SEEK_CUR);
            } else {
                const Uint8 cg_ctr = code >> 8;
                const Uint8 cg_type = code & 0xFF;
                *p++ = cg_type;
                *p++ = cg_ctr;

                Uint16 cg_se = 0;
                SDL_ReadU16BE(rom, &cg_se);
                cg_se = remap_cg_se(cg_se, character);
                *(Uint16*)p = cg_se;
                p += 2;

                SDL_ReadU16BE(rom, p); // cg_olc_ix
                p += 2;

                Uint16 cg_number = 0;
                SDL_ReadU16BE(rom, &cg_number);
                cg_number = remap_cg_number(cg_number, character);
                *(Uint16*)p = cg_number;
                p += 2;

                if (cgd_type >= 4) {
                    Sint16 cg_att_ix = 0;
                    Uint16 cg_hit_ix = 0;
                    SDL_ReadS16BE(rom, &cg_att_ix);
                    SDL_ReadU16BE(rom, &cg_hit_ix);

                    *(Uint16*)p = cg_hit_ix;
                    p += 2;
                    *(Sint16*)p = cg_att_ix;
                    p += 2;

                    for (int i = 0; i < 4; i++) {
                        SDL_ReadU8(rom, p); // cg_extdat ... cg_eftype
                        p += 1;
                    }
                }

                if (cgd_type == 6) {
                    for (int i = 0; i < 3; i++) {
                        SDL_ReadU16BE(rom, p); // cg_zoom ... cg_add_xy
                        p += 2;
                    }

                    for (int i = 0; i < 2; i++) {
                        SDL_ReadU8(rom, p); // cg_next_ix ... cg_status
                        p += 1;
                    }
                }
            }
        }
    }

    // Cleanup
    SDL_free(script_offsets);

    return result;
}

static const void* read_u8_array(SDL_IOStream* rom, Location location) {
    SDL_SeekIO(rom, location.offset, SDL_IO_SEEK_SET);
    void* result = SDL_malloc(location.size);
    SDL_ReadIO(rom, result, location.size);
    return result;
}

static const void* read_s16_array(SDL_IOStream* rom, Location location) {
    SDL_SeekIO(rom, location.offset, SDL_IO_SEEK_SET);
    void* result = SDL_malloc(location.size);
    Sint16* values = result;

    for (int i = 0; i < location.size / 2; i++) {
        SDL_ReadS16BE(rom, values);
        values++;
    }

    return result;
}

static const void* read_u16_array(SDL_IOStream* rom, Location location) {
    SDL_SeekIO(rom, location.offset, SDL_IO_SEEK_SET);
    void* result = SDL_malloc(location.size);
    Uint16* values = result;

    for (int i = 0; i < location.size / 2; i++) {
        SDL_ReadU16BE(rom, values);
        values++;
    }

    return result;
}

static void coalesce_adjacent_sections(CharDataImage* image, const LocationData* locations) {
    const Location* section_locations = (const Location*)locations;
    CharDataSection sections[CHAR_DATA_SECTION_COUNT];

    for (int i = 0; i < CHAR_DATA_SECTION_COUNT; i++) {
        sections[i] = i;
    }

    for (int i = 1; i < CHAR_DATA_SECTION_COUNT; i++) {
        const CharDataSection section = sections[i];
        int j = i;

        while (j > 0 && section_locations[sections[j - 1]].offset > section_locations[section].offset) {
            sections[j] = sections[j - 1];
            j--;
        }

        sections[j] = section;
    }

    for (int run_start = 0; run_start < CHAR_DATA_SECTION_COUNT;) {
        int run_end = run_start;

        while (run_end + 1 < CHAR_DATA_SECTION_COUNT) {
            const Location current = section_locations[sections[run_end]];
            const Location next = section_locations[sections[run_end + 1]];

            if (current.offset + current.size != next.offset) {
                break;
            }

            run_end++;
        }

        if (run_end > run_start) {
            const Uint32 base_offset = section_locations[sections[run_start]].offset;
            const Location last = section_locations[sections[run_end]];
            Uint8* allocation = SDL_malloc(last.offset + last.size - base_offset);

            for (int i = run_start; i <= run_end; i++) {
                const CharDataSection section = sections[i];
                CharDataSpan* span = &image->spans[section];
                Uint8* destination = allocation + section_locations[section].offset - base_offset;
                SDL_memcpy(destination, span->data, span->size);
                SDL_free(span->data);
                span->data = destination;
            }
        }

        run_start = run_end + 1;
    }
}

static void update_table_pointers(CharDataImage* image) {
    CharInitData* dst = &image->tables;

    dst->nmca = image->spans[CHAR_DATA_NMCA].data;
    dst->dmca = image->spans[CHAR_DATA_DMCA].data;
    dst->btca = image->spans[CHAR_DATA_BTCA].data;
    dst->caca = image->spans[CHAR_DATA_CACA].data;
    dst->cuca = image->spans[CHAR_DATA_CUCA].data;
    dst->atca = image->spans[CHAR_DATA_ATCA].data;
    dst->saca = image->spans[CHAR_DATA_SACA].data;
    dst->exca = image->spans[CHAR_DATA_EXCA].data;
    dst->cbca = image->spans[CHAR_DATA_CBCA].data;
    dst->yuca = image->spans[CHAR_DATA_YUCA].data;
    dst->stxy = image->spans[CHAR_DATA_STXY].data;
    dst->mvxy = image->spans[CHAR_DATA_MVXY].data;
    dst->sernd = image->spans[CHAR_DATA_SERND].data;
    dst->ovct = image->spans[CHAR_DATA_OVCT].data;
    dst->ovix = image->spans[CHAR_DATA_OVIX].data;
    dst->rict = image->spans[CHAR_DATA_RICT].data;
    dst->hiit = image->spans[CHAR_DATA_HIIT].data;
    dst->boda = image->spans[CHAR_DATA_BODA].data;
    dst->hana = image->spans[CHAR_DATA_HANA].data;
    dst->cata = image->spans[CHAR_DATA_CATA].data;
    dst->caua = image->spans[CHAR_DATA_CAUA].data;
    dst->atta = image->spans[CHAR_DATA_ATTA].data;
    dst->hosa = image->spans[CHAR_DATA_HOSA].data;
    dst->atit = image->spans[CHAR_DATA_ATIT].data;
    dst->prot = image->spans[CHAR_DATA_PROT].data;
}

static const void* read_sernd(SDL_IOStream* rom, Location location) {
    SDL_SeekIO(rom, location.offset, SDL_IO_SEEK_SET);

    void* result = SDL_malloc(location.size);
    const int offset_count = location.size / 0x24;
    Uint32* offsets = result;

    for (int i = 0; i < offset_count; i++) {
        SDL_ReadU32BE(rom, offsets);
        *offsets -= (BASE_OFFSET + location.offset);
        offsets++;
    }

    Uint16* values = offsets;
    const int value_count = offset_count * 16;

    for (int i = 0; i < value_count; i++) {
        SDL_ReadU16BE(rom, values);
        values++;
    }

    return result;
}

static const void* read_ovct(SDL_IOStream* rom, Location location) {
    SDL_SeekIO(rom, location.offset, SDL_IO_SEEK_SET);

    OverlapPart* result = SDL_malloc(location.size);
    const int elem_count = location.size / sizeof(OverlapPart);

    for (int i = 0; i < elem_count; i++) {
        OverlapPart* element = &result[i];
        SDL_ReadS16BE(rom, &element->parts_hos_x);
        SDL_ReadS16BE(rom, &element->parts_hos_y);
        SDL_ReadU8(rom, &element->parts_colmd);
        SDL_ReadU8(rom, &element->parts_colcd);
        SDL_ReadU8(rom, &element->parts_prio);
        SDL_ReadU8(rom, &element->parts_flip);
        SDL_ReadU8(rom, &element->parts_timer);
        SDL_ReadU8(rom, &element->parts_disp);
        SDL_ReadS16BE(rom, &element->parts_mts);
        SDL_ReadU16BE(rom, &element->parts_nix);
        SDL_ReadU16BE(rom, &element->parts_char);
    }

    return result;
}

static const void* read_catch_table(SDL_IOStream* rom, Location location) {
    SDL_SeekIO(rom, location.offset, SDL_IO_SEEK_SET);

    CatchTable* result = SDL_malloc(location.size);
    const int elem_count = location.size / sizeof(CatchTable);

    for (int i = 0; i < elem_count; i++) {
        CatchTable* element = &result[i];
        SDL_ReadS16BE(rom, &element->catch_hos_x);
        SDL_ReadS16BE(rom, &element->catch_hos_y);
        SDL_ReadU8(rom, &element->catch_prio);
        SDL_ReadU8(rom, &element->catch_flip);
        SDL_ReadS16BE(rom, &element->catch_nix);
    }

    return result;
}

#if defined(DEBUG) && defined(DUMP_CHAR_DATA)
static void dump(const void* buf, const char* name, size_t length, Character character) {
    char* path;
    SDL_asprintf(&path, "dump/%s_pl%02d", name, character);

    SDL_IOStream* io = SDL_IOFromFile(path, "wb");
    SDL_WriteIO(io, buf, length);

    SDL_CloseIO(io);
    SDL_free(path);
}

static void dump_data(CharInitData* data, Character character) {
    const LocationData* locations = &location_data[character];
    dump(data->nmca, "nmca", locations->nmca.size, character);
    dump(data->dmca, "dmca", locations->dmca.size, character);
    dump(data->btca, "btca", locations->btca.size, character);
    dump(data->caca, "caca", locations->caca.size, character);
    dump(data->cuca, "cuca", locations->cuca.size, character);
    dump(data->atca, "atca", locations->atca.size, character);
    dump(data->saca, "saca", locations->saca.size, character);
    dump(data->exca, "exca", locations->exca.size, character);
    dump(data->cbca, "cbca", locations->cbca.size, character);
    dump(data->yuca, "yuca", locations->yuca.size, character);
    dump(data->stxy, "stxy", locations->stxy.size, character);
    dump(data->mvxy, "mvxy", locations->mvxy.size, character);
    dump(data->sernd, "sernd", locations->sernd.size, character);
    dump(data->ovct, "ovct", locations->ovct.size, character);
    dump(data->ovix, "ovix", locations->ovix.size, character);
    dump(data->rict, "rict", locations->rict.size, character);
    dump(data->hiit, "hiit", locations->hiit.size, character);
    dump(data->boda, "boda", locations->boda.size, character);
    dump(data->hana, "hana", locations->hana.size, character);
    dump(data->cata, "cata", locations->cata.size, character);
    dump(data->caua, "caua", locations->caua.size, character);
    dump(data->atta, "atta", locations->atta.size, character);
    dump(data->hosa, "hosa", locations->hosa.size, character);
    dump(data->atit, "atit", locations->atit.size, character);
    dump(data->prot, "prot", locations->prot.size, character);
}
#endif

/* The romset directories we probe -- a deliberately SHORT subset of the
 * ones a MiSTer arcade core reads, in MiSTer Main's own resolution order.
 *
 * Main resolves the arcade ROM directory ONCE per MRA, via
 * findGamesDir("mame") -> findPrefixDir(GAMES_DIR, no_prefix_check=true,
 * "mame") -- Main_MiSTer file_io.cpp:1135-1138 and :1046-1133 @ 915ca339.
 * The in-source order comment there (file_io.cpp:1048-1056) and the code
 * below it agree: USB first, then network, then CIFS, then the SD card,
 * with the bare `<root>/mame` checked before `<root>/games/mame` at every
 * root. Relative paths are resolved against the storage root by
 * make_fullpath (file_io.cpp:215-227), so "../usb0/mame" is
 * /media/usb0/mame. That walk spans 19 directories, and this list used to
 * mirror all 19.
 *
 * WHY IT NO LONGER DOES. Exactly one of those 19 has ever been exercised
 * anywhere -- /media/fat/games/mame, on the target device. The other 18
 * were string literals no test and no run had ever reached, so a typo in
 * one of them was indistinguishable from the directory merely being
 * absent: the search failed the same silent way either case. Untested
 * paths bought no coverage, only the appearance of it.
 *
 * The trim is conservative, because the cost of dropping a path someone
 * really uses is a silent fall back to PS2 balance -- worse than a wasted
 * open(). Kept:
 *
 *   /media/fat/games/mame     update_all's default arcade ROM directory,
 *                             and the only path verified on real hardware
 *   /media/fat/mame           where a hand-assembled set commonly lands
 *   /media/fat/_Arcade/mame   Main's own last-resort fallback
 *   /media/usb0/{,games/}mame a 79 MB romset on external storage is a
 *                             real install, and usb0 is the first (often
 *                             only) USB storage slot Main numbers
 *
 * Dropped: usb1..usb5, /media/network, /media/fat/cifs. On the target
 * device /media/network and /media/fat/cifs do not exist at all, not even
 * as empty mount points, whereas /media/usb0../media/usb7 all do -- so a
 * second USB slot is the only dropped case with a mount point behind it,
 * and reaching it needs a player with two storage devices attached who
 * put the romset on the second. That player is no longer dropped into PS2
 * balance without a word: the miss summary in ArcadeCharData_Init names
 * every directory it looked in, which is the whole point of trimming to a
 * list short enough to print.
 *
 * (Main itself only searches usb0..usb5 -- file_io.cpp:1049-1050 -- even
 * though the rootfs provisions usb0..usb7, so usb6/usb7 were never in
 * scope for either of us.)
 *
 * The last entry is Main's fallback rather than part of that list: when
 * findGamesDir finds no `mame` directory anywhere, set_arcade_root falls
 * back to the MRA's own top-level folder (mra_loader.cpp:176-178), i.e.
 * /media/fat/_Arcade/mame for an MRA under _Arcade/. Main only reaches it
 * when every directory above is absent, which is why it is probed last.
 *
 * /media/fat/mame is listed for a different reason. Main does check it
 * (file_io.cpp:1118-1122), but that branch returns with `dir` still the
 * bare "mame", so the strrchr in set_arcade_root finds no '/' and leaves
 * mame_root empty (mra_loader.cpp:171-174); the part path is then built as
 * "%s/mame/%s/%s" (mra_loader.cpp:895) -> the absolute "/mame/<zip>/<part>",
 * so Main itself misreads that case. We still look, because it is where
 * such a user actually put the romset and one failed open costs nothing.
 *
 * Deliberate difference from Main: Main commits to the FIRST directory that
 * exists and never looks past it, so a `mame` folder on usb0 hides the SD
 * card's. We probe every entry instead. That is safe precisely because
 * acceptance is by content digest, not by presence -- a decoy or
 * wrong-revision zip in an earlier directory cannot mask a good one later. */
/* Length of `s` with one trailing '/' or '\\' removed, if present. */
static size_t trim_trailing_slash_len(const char* s) {
    size_t n = SDL_strlen(s);

    if (n > 0 && (s[n - 1] == '/' || s[n - 1] == '\\')) {
        n--;
    }

    return n;
}

static const char* const cps3_rom_dirs[] = {
    "/media/usb0/mame", "/media/usb0/games/mame",
    "/media/fat/mame",  "/media/fat/games/mame",
    "/media/fat/_Arcade/mame",
};

/* The zip basenames the shipped jtcps3 3rd Strike MRA declares, in its own
 * order: `<rom index="0" zip="sfiii3nr1.zip|sfiii3.zip" ...>` -- verified
 * both in jotego/jtbin's published MRA and in the copy installed on the
 * target device at /media/fat/_Arcade/Street Fighter III 3rd Strike Fight
 * for the Future (Japan 990512, NO CD).mra. Main walks that `|` list with
 * strsep and takes the first that satisfies the part (mra_loader.cpp:889-915),
 * so either basename is a legitimate install: update_all ships the merged
 * sfiii3.zip, while a set assembled to run the JOTEGO public-repo core --
 * the route users take because the jtcps3 MRAs are jtbeta-gated in
 * update_all -- is commonly the small sfiii3nr1.zip.
 *
 * These names decide only which file we OPEN. What we accept out of it is
 * decided purely by content hash in rom_load.c. */
static const char* const cps3_rom_zip_names[] = {
    "sfiii3nr1.zip",
    "sfiii3.zip",
};

#define CPS3_ROM_DIR_COUNT (sizeof(cps3_rom_dirs) / sizeof(cps3_rom_dirs[0]))
#define CPS3_ROM_ZIP_NAME_COUNT (sizeof(cps3_rom_zip_names) / sizeof(cps3_rom_zip_names[0]))

/* Is this path a directory / a regular file right now? Used ONLY to
 * classify a miss for the log — never to decide what to open, so a
 * filesystem that answers oddly costs a vaguer message and never a ROM
 * that would otherwise have loaded. */
static bool path_is(const char* path, SDL_PathType want) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == want;
}

/* Joins the probed directory names into `out` for the miss summary:
 * every one of them, or only the ones that are really there. Runs on the
 * failure path only. Truncation would be harmless, and CPS3_ROM_DIR_COUNT
 * is small enough (see the trim rationale above) that it cannot happen at
 * the 256-byte call site. */
static void join_rom_dirs(char* out, size_t out_size, bool only_existing) {
    size_t used = 0;
    out[0] = '\0';

    for (size_t d = 0; d < CPS3_ROM_DIR_COUNT && used + 1 < out_size; d++) {
        if (only_existing && !path_is(cps3_rom_dirs[d], SDL_PATHTYPE_DIRECTORY)) {
            continue;
        }

        /* SDL_snprintf returns what it WOULD have written, so clamp
         * before it is used as an offset again. */
        const int written = SDL_snprintf(out + used, out_size - used, "%s%s",
                                         (used > 0) ? ", " : "", cps3_rom_dirs[d]);

        if (written <= 0) {
            break;
        }

        used += (size_t)written;

        if (used >= out_size) {
            used = out_size - 1;
            break;
        }
    }
}

void ArcadeCharData_Init() {
#if defined(DEBUG)
    /* Merged from fix/arcade-cg-mapping: the CG-range overlap check runs
     * at the top of Init, before the ROM is located, so it also covers
     * PS2-balance Debug launches. Compiled out of every shipped build. */
    validate_cg_ranges();
#endif

    /* CPS3 ROM search path.
     *
     * We ship no ROM of our own and we do not look for one in our own
     * install dir. The only ROM this program loads is the romset the
     * user's own CPS3 arcade core already reads; a player without that
     * core gets PS2 balance rather than a copy we supplied.
     *
     * Within each zip the required slices are matched by CONTENT --
     * stored-CRC32 pre-filter then SHA-256 (rom_load.c) -- never by entry
     * name or path, so a flat minimal set, update_all's merged set and
     * subdirectory-variant packagings all load identically.
     *
     * MAKING A MISS LEGIBLE. Rom_Load answers NULL to three quite
     * different questions: "the file is not there" (rom_load.c:123-126,
     * silently), "it is there but will not open as a zip"
     * (rom_load.c:131-136, silently) and "the zip opened and nothing
     * inside it matched the pinned digests" (rom_load.c:230-239, one line
     * per unmatched SIMM). The third is a player holding the WRONG
     * REVISION of the romset, and it used to end in exactly the same
     * final message as a player holding no romset at all -- followed by a
     * silently different game, because total failure falls back to PS2
     * balance without further comment.
     *
     * So the walk below stats each directory and each candidate file
     * itself, purely to classify the outcome. Rom_Load is still called
     * for every candidate exactly as before and no stat result gates it,
     * so what gets FOUND is unchanged; only the logging is new.
     *
     * Bounded on purpose. A normal "no romset installed" boot prints ONE
     * line, because nothing existed to report on. Only a zip we actually
     * found and then refused earns a line of its own. */
    size_t rom_size = 0;
    const void* rom = NULL;
    int probed = 0;
    int dirs_present = 0;
    int zips_found = 0;

    /* DEV/TEST ONLY. Not a supported player knob and never set on a real
     * install: it is how CI-style and cross-arch runs point at a romset
     * that lives outside a MiSTer arcade install at all. */
    const char* env_path = SDL_getenv("THIRDSARM_CPS3_ZIP");
    const bool env_set = env_path != NULL && env_path[0] != '\0';

    if (env_set) {
        const bool env_found = path_is(env_path, SDL_PATHTYPE_FILE);

        probed++;
        rom = Rom_Load(env_path, &rom_size);

        if (rom != NULL) {
            SDL_Log("ArcadeCharData: CPS3 ROM load satisfied by %s ($THIRDSARM_CPS3_ZIP)", env_path);

            /* DEV/TEST ONLY -- "first light" (docs/research-arcade-cg-data-accuracy.md,
             * 3sx-rom-only-research.md §5S 4.2). Attempts SIMM3-6 (gfx) from the
             * same zip; a no-op when it only carries SIMM1 (e.g. the flat
             * sfiii3nr1.zip). Never reached outside this $THIRDSARM_CPS3_ZIP
             * branch -- the production directory search above/below never
             * touches gfx data. */
            Cps3FirstLight_TryLoad(env_path);
        } else if (env_found) {
            zips_found++;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ArcadeCharData: $THIRDSARM_CPS3_ZIP=%s exists but FAILED CONTENT "
                        "VERIFICATION -- wrong romset revision, not a missing file",
                        env_path);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ArcadeCharData: $THIRDSARM_CPS3_ZIP=%s does not exist", env_path);
        }
    }

    /* Every directory in cps3_rom_dirs is a MiSTer absolute path, so on a
     * desktop install none of them exist and there is nowhere a player can
     * put the romset -- leaving $THIRDSARM_CPS3_ZIP, which is a dev hook,
     * as the only route. That is not a cosmetic gap: netplay arms ONLY in
     * verified-arcade balance (netplay.c's arm-time predicate), so a
     * desktop build with no discoverable romset cannot host or join at all.
     *
     * So also probe the executable's own directory and the pref directory,
     * each with an optional roms/ subdirectory. The executable's directory
     * comes first: a downloaded desktop build is a single folder the player
     * moves around, and a romset dropped in beside the .exe should win over
     * whatever an older install left behind. On MiSTer these resolve to
     * /media/fat/games/3s-arm/bin/ and /media/fat/games/3s-arm/, both
     * harmless to probe and arguably useful. Content verification is unchanged:
     * this only widens WHERE a romset may be found, never what is
     * accepted. */
    const char* pref = Paths_GetPrefPath();
    const char* base = SDL_GetBasePath();
    char* extra_dirs[4] = { NULL, NULL, NULL, NULL };

    /* Portable layout first, so a self-contained folder wins over whatever
     * an older install left in the pref directory. */
    /* Paths_GetPrefPath and SDL_GetBasePath both end in a separator, and the
     * candidate below joins with one of its own, so trim it here rather than
     * emit "dir//sfiii3.zip" into every log line this search prints. */
    if (base != NULL) {
        SDL_asprintf(&extra_dirs[0], "%sroms", base);
        SDL_asprintf(&extra_dirs[1], "%.*s", (int)trim_trailing_slash_len(base), base);
    }

    if (pref != NULL) {
        SDL_asprintf(&extra_dirs[2], "%sroms", pref);
        SDL_asprintf(&extra_dirs[3], "%.*s", (int)trim_trailing_slash_len(pref), pref);
    }

    for (size_t d = 0; rom == NULL && d < SDL_arraysize(extra_dirs); d++) {
        if (extra_dirs[d] == NULL) {
            continue;
        }

        if (path_is(extra_dirs[d], SDL_PATHTYPE_DIRECTORY)) {
            dirs_present++;
        }

        for (size_t n = 0; rom == NULL && n < CPS3_ROM_ZIP_NAME_COUNT; n++) {
            char* path = NULL;
            SDL_asprintf(&path, "%s/%s", extra_dirs[d], cps3_rom_zip_names[n]);

            if (path == NULL) {
                continue;
            }

            const bool zip_found = path_is(path, SDL_PATHTYPE_FILE);

            probed++;
            rom = Rom_Load(path, &rom_size);

            if (rom != NULL) {
                SDL_Log("ArcadeCharData: CPS3 ROM load satisfied by %s", path);
            } else if (zip_found) {
                zips_found++;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "ArcadeCharData: %s exists but FAILED CONTENT VERIFICATION "
                            "-- wrong romset revision, not a missing ROM",
                            path);
            }

            SDL_free(path);
        }
    }

    for (size_t d = 0; d < SDL_arraysize(extra_dirs); d++) {
        SDL_free(extra_dirs[d]);
    }

    /* dirs x names, outer loop over directories so the directory stat is
     * paid once per directory instead of once per candidate. Both loops
     * carry the `rom == NULL` guard, so the walk still has a single exit
     * and the per-candidate string is freed on every path. */
    for (size_t d = 0; rom == NULL && d < CPS3_ROM_DIR_COUNT; d++) {
        if (path_is(cps3_rom_dirs[d], SDL_PATHTYPE_DIRECTORY)) {
            dirs_present++;
        }

        for (size_t n = 0; rom == NULL && n < CPS3_ROM_ZIP_NAME_COUNT; n++) {
            char* path = NULL;
            SDL_asprintf(&path, "%s/%s", cps3_rom_dirs[d], cps3_rom_zip_names[n]);

            if (path == NULL) {
                continue;
            }

            const bool zip_found = path_is(path, SDL_PATHTYPE_FILE);

            probed++;
            rom = Rom_Load(path, &rom_size);

            if (rom != NULL) {
                SDL_Log("ArcadeCharData: CPS3 ROM load satisfied by %s", path);
            } else if (zip_found) {
                /* The one miss worth a line of its own: a file with a
                 * romset's name, in a directory romsets really live in,
                 * that we nevertheless refused. Rom_Load's own "no entry
                 * matched" lines sit directly above this one and name the
                 * slices that were missing. */
                zips_found++;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "ArcadeCharData: %s exists but FAILED CONTENT VERIFICATION "
                            "-- wrong romset revision, not a missing ROM",
                            path);
            }

            SDL_free(path);
        }
    }

    if (rom == NULL) {
        char dir_list[256];

        /* Three misses, three answers. They used to share one message, so
         * "you have the wrong romset" and "you have no romset" arrived in
         * a bug report indistinguishable from each other. */
        if (zips_found > 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ArcadeCharData: no CPS3 ROM -- %d candidate zip(s) were FOUND and "
                        "REJECTED by content verification (each named above), out of %d "
                        "probed location(s). A romset IS installed; it is the WRONG "
                        "REVISION. Arcade balance needs the sfiii3nr1 SIMM1 slices, which "
                        "in update_all's merged set live under sfiii3nar1/. Falling back "
                        "to PS2 balance.",
                        zips_found, probed);
        } else if (dirs_present > 0) {
            join_rom_dirs(dir_list, sizeof(dir_list), true);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ArcadeCharData: no CPS3 ROM -- %d of the %d arcade ROM directories "
                        "searched exist (%s), but none of them holds %s or %s. Falling back "
                        "to PS2 balance.",
                        dirs_present, (int)CPS3_ROM_DIR_COUNT, dir_list,
                        cps3_rom_zip_names[0], cps3_rom_zip_names[1]);
        } else {
            join_rom_dirs(dir_list, sizeof(dir_list), false);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ArcadeCharData: no CPS3 ROM -- none of the %d arcade ROM directories "
                        "searched exist on this system (%s), so no CPS3 arcade core romset is "
                        "installed anywhere we look. Install one (%s or %s) and retry. "
                        "Falling back to PS2 balance.",
                        (int)CPS3_ROM_DIR_COUNT, dir_list,
                        cps3_rom_zip_names[0], cps3_rom_zip_names[1]);
        }

        if (env_set) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ArcadeCharData: $THIRDSARM_CPS3_ZIP was set and is counted in the "
                        "%d probed location(s) above",
                        probed);
        }

        return;
    }

    SDL_IOStream* io = SDL_IOFromConstMem(rom, rom_size);

    for (int character = 0; character < NUM_CHARS; character++) {
        const LocationData* locations = &location_data[character];
        CharDataImage* image = &data[character];
        CharInitData* dst = &image->tables;
        dst->nmca = read_char_table(io, locations->nmca, character);
        dst->dmca = read_char_table(io, locations->dmca, character);
        dst->btca = read_char_table(io, locations->btca, character);
        dst->caca = read_char_table(io, locations->caca, character);
        dst->cuca = read_char_table(io, locations->cuca, character);
        dst->atca = read_char_table(io, locations->atca, character);
        dst->saca = read_char_table(io, locations->saca, character);
        dst->exca = read_char_table(io, locations->exca, character);
        dst->cbca = read_char_table(io, locations->cbca, character);
        dst->yuca = read_char_table(io, locations->yuca, character);
        dst->stxy = read_s16_array(io, locations->stxy);
        dst->mvxy = read_s16_array(io, locations->mvxy);
        dst->sernd = read_sernd(io, locations->sernd);
        dst->ovct = read_ovct(io, locations->ovct);
        dst->ovix = read_s16_array(io, locations->ovix);
        dst->rict = read_catch_table(io, locations->rict);
        dst->hiit = read_u16_array(io, locations->hiit);
        dst->boda = read_s16_array(io, locations->boda);
        dst->hana = read_s16_array(io, locations->hana);
        dst->cata = read_s16_array(io, locations->cata);
        dst->caua = read_s16_array(io, locations->caua);
        dst->atta = read_s16_array(io, locations->atta);
        dst->hosa = read_s16_array(io, locations->hosa);
        dst->atit = read_u8_array(io, locations->atit);
        dst->prot = read_s16_array(io, locations->prot);

        void* const section_data[CHAR_DATA_SECTION_COUNT] = {
            dst->nmca, dst->dmca, dst->btca, dst->caca,  dst->cuca, dst->atca, dst->saca, dst->exca, dst->cbca,
            dst->yuca, dst->stxy, dst->mvxy, dst->sernd, dst->ovct, dst->ovix, dst->rict, dst->hiit, dst->boda,
            dst->hana, dst->cata, dst->caua, dst->atta,  dst->hosa, dst->atit, dst->prot,
        };

        const Location* section_locations = (const Location*)locations;

        for (size_t i = 0; i < CHAR_DATA_SECTION_COUNT; i++) {
            image->spans[i] = (CharDataSpan) {
                .data = section_data[i],
                .size = section_locations[i].size,
                .element_size = section_element_sizes[i],
            };
        }

        // CPS3 data sometimes indexes across named section boundaries. Preserve
        // every directly adjacent ROM run so those accesses remain well-defined.
        coalesce_adjacent_sections(image, locations);
        update_table_pointers(image);

#if defined(DEBUG) && defined(DUMP_CHAR_DATA)
        dump_data(dst, character);
#endif
    }

    initialized = true;
    SDL_CloseIO(io);
    SDL_free(rom);
}

bool ArcadeCharData_IsInitialized() {
    return initialized;
}

uint64_t ArcadeCharData_ComputeDigest() {
    if (!initialized) {
        return 0;
    }

    sha256 sha;

    if (!sha256_init(&sha)) {
        return 0;
    }

    /* Hash every parsed section span of every character, in fixed
     * (character, section) order. Run AFTER the full 20-character
     * adaptation: Apply3SXRenderingConventions mutates the OVCT spans, so
     * the digest covers exactly the arcade data the simulation will read.
     * Two peers agree iff their adapted tables are byte-identical — a
     * different ROM revision (or a future parser/adaptation change)
     * yields a different digest. */
    for (int character = 0; character < NUM_CHARS; character++) {
        for (int section = 0; section < CHAR_DATA_SECTION_COUNT; section++) {
            const CharDataSpan* span = &data[character].spans[section];
            sha256_append(&sha, span->data, span->size);
        }
    }

    Uint8 bytes[SHA256_BYTES_SIZE];

    if (!sha256_finalize_bytes(&sha, bytes)) {
        return 0;
    }

    uint64_t digest = 0;

    for (int i = 0; i < 8; i++) {
        digest = (digest << 8) | bytes[i];
    }

    /* 0 is reserved for "no digest" — never emit it for real data. */
    return digest != 0 ? digest : 1;
}

const CharInitData* ArcadeCharData_Get(Character character) {
    if (!initialized || character >= NUM_CHARS) {
        return NULL;
    }

    return &data[character].tables;
}

static bool overlap_behavior_matches(const OverlapPart* arcade, const OverlapPart* ps2) {
    return arcade->parts_hos_x == ps2->parts_hos_x && arcade->parts_hos_y == ps2->parts_hos_y &&
           arcade->parts_colmd == ps2->parts_colmd && arcade->parts_prio == ps2->parts_prio &&
           arcade->parts_flip == ps2->parts_flip && arcade->parts_timer == ps2->parts_timer &&
           arcade->parts_disp == ps2->parts_disp && arcade->parts_nix == ps2->parts_nix;
}

static bool get_ps2_section_span(const void* ps2_data, size_t ps2_size, CharDataSection section, CharDataSpan* span) {
    const size_t header_size = CHAR_DATA_SECTION_COUNT * sizeof(Uint32);

    if (ps2_data == NULL || ps2_size < header_size) {
        return false;
    }

    Uint32 offsets[CHAR_DATA_SECTION_COUNT];
    SDL_memcpy(offsets, ps2_data, sizeof(offsets));

    const Uint32 start = offsets[section];
    size_t end = ps2_size;

    if (start < header_size || start >= ps2_size) {
        return false;
    }

    for (size_t i = 0; i < CHAR_DATA_SECTION_COUNT; i++) {
        if (offsets[i] > start && offsets[i] < end) {
            end = offsets[i];
        }
    }

    span->data = (Uint8*)ps2_data + start;
    span->size = end - start;
    span->element_size = section_element_sizes[section];
    return span->size % span->element_size == 0;
}

bool ArcadeCharData_Apply3SXRenderingConventions(Character character, const void* ps2_data, size_t ps2_size) {
    static bool adapted[NUM_CHARS] = { false };

    if (!initialized || character >= NUM_CHARS) {
        return false;
    }

    if (adapted[character]) {
        return true;
    }

    CharDataSpan ps2_span;

    if (!get_ps2_section_span(ps2_data, ps2_size, CHAR_DATA_OVCT, &ps2_span)) {
        SDL_Log("Invalid PS2 overlap table for character %u", character);
        return false;
    }

    CharDataSpan* arcade_span = &data[character].spans[CHAR_DATA_OVCT];
    OverlapPart* arcade_parts = arcade_span->data;
    const OverlapPart* ps2_parts = ps2_span.data;
    const size_t arcade_count = arcade_span->size / sizeof(*arcade_parts);
    const size_t ps2_count = ps2_span.size / sizeof(*ps2_parts);
    const size_t common_count = SDL_min(arcade_count, ps2_count);

    for (size_t i = 0; i < common_count; i++) {
        if (!overlap_behavior_matches(&arcade_parts[i], &ps2_parts[i])) {
            SDL_Log("Arcade/PS2 overlap tables diverge structurally for character %u at part %zu", character, i);
            return false;
        }
    }

    for (size_t i = 0; i < common_count; i++) {
        // The arcade table supplies animation behavior. These three fields bind
        // that behavior to 3SX's palette, texture-mode, and CG namespaces.
        arcade_parts[i].parts_colcd = ps2_parts[i].parts_colcd;
        arcade_parts[i].parts_mts = ps2_parts[i].parts_mts;
        arcade_parts[i].parts_char = ps2_parts[i].parts_char;
    }

    adapted[character] = true;
    return true;
}

static const CgRemapRange gill_cg_ranges[] = {
    { .first = 0x7070, .last = 0x707A, .delta = -0x6AE0 },
};

static const CgRemapRange alex_cg_ranges[] = {
    { .first = 0x707B, .last = 0x7085, .delta = -0x667D },
};

static const CgRemapRange ryu_cg_ranges[] = {
    { .first = 0x7082, .last = 0x7090, .delta = -0x62C6 },
};

static const CgRemapRange yun_cg_ranges[] = {
    { .first = 0x7091, .last = 0x709B, .delta = -0x5D20 },
};

// yuca[68..75] script cell 0x0CB4 is a cross-bank reference (§8.E): one raw
// CG shared by 12 characters' own scripts (Ryu included), each needing that
// *other* character's delta rather than its own -- except Ryu, whose own
// default_delta already is -0x1E0, so only the other 11 need a row here.
// PS2 resolves it via Ryu's own -0x1E0 for Dudley/Necro/Hugo/Ibuki/Elena/Oro/
// Yang/Urien; Ken/Sean/Akuma measure their own distinct deltas (see their
// range tables below).
static const CgRemapRange dudley_cg_ranges[] = {
    { .first = 0x709C, .last = 0x70A6, .delta = -0x58C3 },
    { .first = 0x0CB4, .last = 0x0CB4, .delta = -0x1E0 },
};

static const CgRemapRange necro_cg_ranges[] = {
    { .first = 0x70A7, .last = 0x70B1, .delta = -0x53E4 },
    { .first = 0x0CB4, .last = 0x0CB4, .delta = -0x1E0 },
};

static const CgRemapRange hugo_cg_ranges[] = {
    { .first = 0x70B2, .last = 0x70BC, .delta = -0x5005 },
    { .first = 0x0CB4, .last = 0x0CB4, .delta = -0x1E0 },
};

static const CgRemapRange ibuki_cg_ranges[] = {
    { .first = 0x70BD, .last = 0x70C7, .delta = -0x4904 },
    // Off-by-one (doc §8.D): PS2's measured delta for this whole span is
    // -0x74CF, one less (in magnitude) than the -0x74D0 below.
    { .first = 0x9BA8, .last = 0x9C6F, .delta = -0x74CF },
    { .first = 0x0CB4, .last = 0x0CB4, .delta = -0x1E0 },
};

static const CgRemapRange elena_cg_ranges[] = {
    { .first = 0x70C8, .last = 0x70D2, .delta = -0x42E5 },
    { .first = 0x9C88, .last = 0x9CC1, .delta = -0x6F08 },
    /* Fixes upstream #363 (Ryu's Denjin shocks Elena) and its btca[15]/exca
       siblings. 0x9CC1 (end of #290's range) maps to 11705 under -0x6F08;
       0x9CFC (this range's start) maps to 11706 under -0x6F42 -- the delta
       step (0x3A = 58) equals the size of the intervening raw gap
       0x9CC2-0x9CFB, which backs 0 cells in any of Elena's ten tables. So
       0x9C88..0x9D24 maps onto one unbroken PS2 run 11648..11746, with 58
       arcade-only sprites elided, and all 41 raw values in this range land
       in group 9. See docs/research-arcade-cg-data-accuracy.md §8.A. */
    { .first = 0x9CFC, .last = 0x9D24, .delta = -0x6F42 },
    { .first = 0x0CB4, .last = 0x0CB4, .delta = -0x1E0 },
};

static const CgRemapRange oro_cg_ranges[] = {
    { .first = 0x70D3, .last = 0x70DD, .delta = -0x3D5A },
    { .first = 0x0CB4, .last = 0x0CB4, .delta = -0x1E0 },
};

static const CgRemapRange yang_cg_ranges[] = {
    { .first = 0x70DE, .last = 0x70E8, .delta = -0x37F2 },
    { .first = 0x0CB4, .last = 0x0CB4, .delta = -0x1E0 },
};

static const CgRemapRange ken_cg_ranges[] = {
    { .first = 0x70E9, .last = 0x70F3, .delta = -0x3399 },
    // caca[18..20] cross-bank cells (doc §8.E / §7.3(i)).
    { .first = 0x1201, .last = 0x1201, .delta = -0x420 },
    // Ken's own resolved delta for the yuca[68..75] slot -- not Ryu's.
    { .first = 0x0CB4, .last = 0x0CB4, .delta = 0x2D40 },
};

static const CgRemapRange sean_cg_ranges[] = {
    { .first = 0x70F4, .last = 0x70FF, .delta = -0x2F74 },
    // cuca[64] and siblings, all measuring the same +0x3160 (doc §8.E /
    // §7.3(i)); discrete rows because the raw values aren't contiguous.
    { .first = 0x0C92, .last = 0x0C92, .delta = 0x3160 },
    { .first = 0x0C95, .last = 0x0C95, .delta = 0x3160 },
    { .first = 0x0C97, .last = 0x0C97, .delta = 0x3160 },
    { .first = 0x0C9C, .last = 0x0C9C, .delta = 0x3160 },
    { .first = 0x0CAB, .last = 0x0CAB, .delta = 0x3160 },
    { .first = 0x0CB4, .last = 0x0CB4, .delta = 0x3160 },
    { .first = 0x0CBB, .last = 0x0CBB, .delta = 0x3160 },
    { .first = 0x0CC5, .last = 0x0CC5, .delta = 0x3160 },
};

static const CgRemapRange urien_cg_ranges[] = {
    { .first = 0x70FF, .last = 0x7109, .delta = -0x29C3 },
    { .first = 0x0CB4, .last = 0x0CB4, .delta = -0x1E0 },
    // yuca[8..15] (doc §8.D): ten distinct measured deltas against the flat
    // -0xC60 default. 0x52DA-0x52E2 share -0xC78; 0x52E3-0x52EC step by one
    // as raw increases, so each needs its own row. 0x52D9 is deliberately
    // NOT covered here: yuca[0..7] also uses raw 0x52D9 and needs the
    // default -0xC60, so a range would fix yuca[8..15]'s cell but regress
    // yuca[0..7]'s -- the per-raw-value model can't express both; see
    // docs/research-arcade-cg-data-accuracy.md §8.D remainder note.
    { .first = 0x52DA, .last = 0x52E2, .delta = -0xC78 },
    { .first = 0x52E3, .last = 0x52E3, .delta = -0xC6F },
    { .first = 0x52E4, .last = 0x52E4, .delta = -0xC70 },
    { .first = 0x52E5, .last = 0x52E5, .delta = -0xC71 },
    { .first = 0x52E6, .last = 0x52E6, .delta = -0xC72 },
    { .first = 0x52E7, .last = 0x52E7, .delta = -0xC73 },
    { .first = 0x52E8, .last = 0x52E8, .delta = -0xC74 },
    { .first = 0x52E9, .last = 0x52E9, .delta = -0xC75 },
    { .first = 0x52EA, .last = 0x52EA, .delta = -0xC76 },
    { .first = 0x52EB, .last = 0x52EB, .delta = -0xC77 },
    { .first = 0x52EC, .last = 0x52EC, .delta = -0xC78 },
};

static const CgRemapRange akuma_cg_ranges[] = {
    { .first = 0x710A, .last = 0x7114, .delta = -0x2526 },
    // Akuma's own resolved delta for the yuca[68..75] slot -- not Ryu's.
    { .first = 0x0CB4, .last = 0x0CB4, .delta = 0x3B60 },
};

static const CgRemapRange chunli_cg_ranges[] = {
    { .first = 0x7115, .last = 0x711F, .delta = -0x1EB4 },
};

// atca/exca cross-bank cells (doc §8.E / §7.3(i)): 521 cells scattered over
// a wide raw span, all resolving via Ryu's -0x1E0. Rows are the raw-value
// runs that are actually referenced, per cg_audit.json -- coalescing across
// the gaps between them would sweep in unmeasured values (see doc §8.N's
// caution, same hazard).
static const CgRemapRange makoto_cg_ranges[] = {
    { .first = 0x7120, .last = 0x712A, .delta = -0x1760 },
    { .first = 0xA000, .last = UINT16_MAX, .delta = -0x5378 },
    { .first = 0x0C0D, .last = 0x0C0D, .delta = -0x1E0 },
    { .first = 0x0C28, .last = 0x0C2C, .delta = -0x1E0 },
    { .first = 0x0C2F, .last = 0x0C2F, .delta = -0x1E0 },
    { .first = 0x0C44, .last = 0x0C4B, .delta = -0x1E0 },
    { .first = 0x0C56, .last = 0x0C57, .delta = -0x1E0 },
    { .first = 0x0C6B, .last = 0x0C6B, .delta = -0x1E0 },
    { .first = 0x0C83, .last = 0x0C88, .delta = -0x1E0 },
    { .first = 0x0C8C, .last = 0x0C8C, .delta = -0x1E0 },
    { .first = 0x0C93, .last = 0x0C93, .delta = -0x1E0 },
    { .first = 0x0CE3, .last = 0x0CE7, .delta = -0x1E0 },
    { .first = 0x0DE6, .last = 0x0DE7, .delta = -0x1E0 },
    { .first = 0x0E65, .last = 0x0E6C, .delta = -0x1E0 },
    { .first = 0x0EBB, .last = 0x0EBB, .delta = -0x1E0 },
};

// cuca[37] cross-bank cells (doc §8.E / §7.3(i)); resolves via Ryu's -0x1E0.
static const CgRemapRange q_cg_ranges[] = {
    { .first = 0x712B, .last = 0x7135, .delta = -0x123A },
    { .first = 0x0C01, .last = 0x0C01, .delta = -0x1E0 },
    { .first = 0x0C94, .last = 0x0C94, .delta = -0x1E0 },
    { .first = 0x0C97, .last = 0x0C98, .delta = -0x1E0 },
    { .first = 0x0CB3, .last = 0x0CB3, .delta = -0x1E0 },
    { .first = 0x0CB5, .last = 0x0CB7, .delta = -0x1E0 },
    { .first = 0x0CBB, .last = 0x0CC0, .delta = -0x1E0 },
    { .first = 0x0D02, .last = 0x0D02, .delta = -0x1E0 },
};

// nmca/cuca/exca cross-bank cells, the X.C.O.P.Y. family (doc §8.E /
// §7.3(i)); resolves via Necro's -0x600.
static const CgRemapRange twelve_cg_ranges[] = {
    { .first = 0x7136, .last = 0x7140, .delta = -0x0C46 },
    { .first = 0x1E01, .last = 0x1E01, .delta = -0x600 },
    { .first = 0x1E21, .last = 0x1E24, .delta = -0x600 },
    { .first = 0x1E26, .last = 0x1E27, .delta = -0x600 },
    { .first = 0x1E3B, .last = 0x1E3D, .delta = -0x600 },
    { .first = 0x1E40, .last = 0x1E43, .delta = -0x600 },
    { .first = 0x1E68, .last = 0x1E69, .delta = -0x600 },
    { .first = 0x1E71, .last = 0x1E72, .delta = -0x600 },
    { .first = 0x1E76, .last = 0x1E7D, .delta = -0x600 },
    { .first = 0x1FCC, .last = 0x1FCD, .delta = -0x600 },
    { .first = 0x1FD7, .last = 0x1FD8, .delta = -0x600 },
    { .first = 0x1FDA, .last = 0x1FDC, .delta = -0x600 },
    { .first = 0x2033, .last = 0x2033, .delta = -0x600 },
    { .first = 0x2035, .last = 0x2036, .delta = -0x600 },
    { .first = 0x203C, .last = 0x203C, .delta = -0x600 },
    { .first = 0x2074, .last = 0x2074, .delta = -0x600 },
    { .first = 0x2090, .last = 0x2090, .delta = -0x600 },
    { .first = 0x2095, .last = 0x2095, .delta = -0x600 },
};

static const CgRemapRange remy_cg_ranges[] = {
    { .first = 0x7141, .last = 0x714B, .delta = -0x07C1 },
    /* Raw CG 1537 (0x0601) is an Alex sprite placed in Remy's nmca[48] /
       exca[30,37,38] slots. Remy's default_delta underflows it (§7.4's
       negative clamp), so it passes through raw into Gill's group with an
       out-of-range residual (§17.3). PS2 stores 1569 for all six counterpart
       cells -- Alex's own delta, +0x20. See
       docs/research-arcade-cg-data-accuracy.md §8.K. */
    { .first = 0x0601, .last = 0x0601, .delta = 0x20 },
    // A further 19 raw CGs (38 cells), all in ordinary nmca/cuca animations,
    // measure the same Alex-bank +0x20 (doc §8.N). Discrete rows only: the
    // gaps between them hold dmca[3]/[90]/[91] cells whose scripts decode a
    // different shape (cgd) than their PS2 counterparts, so cg_audit.py has
    // no oracle for those raw values -- sweeping them in would be unverified.
    { .first = 0x0655, .last = 0x065C, .delta = 0x20 },
    { .first = 0x0669, .last = 0x0669, .delta = 0x20 },
    { .first = 0x0676, .last = 0x0676, .delta = 0x20 },
    { .first = 0x0678, .last = 0x0678, .delta = 0x20 },
    { .first = 0x067C, .last = 0x067D, .delta = 0x20 },
    { .first = 0x0683, .last = 0x0684, .delta = 0x20 },
    { .first = 0x0690, .last = 0x0692, .delta = 0x20 },
    { .first = 0x0744, .last = 0x0744, .delta = 0x20 },
    // nmca[49]: measures Ryu's -0x1E0, not the Alex-bank +0x20 above --
    // must stay a separate row (doc §8.N's warning).
    { .first = 0x0C01, .last = 0x0C01, .delta = -0x1E0 },
};

// See docs/research-arcade-cg-data-accuracy.md §8.K -- this binds a cutoff
// that has already moved once (ae309dc4).
_Static_assert(
    CG_REMAP_CUTOFF <= 0x601, "remy_cg_ranges' 0x0601 row (doc §8.K) requires CG_REMAP_CUTOFF <= 0x601"
);

static const CharacterCgMap cg_maps[NUM_CHARS] = {
    [CHAR_GILL] = { .default_delta = 0x0000, .ranges = gill_cg_ranges, .range_count = SDL_arraysize(gill_cg_ranges) },
    [CHAR_ALEX] = { .default_delta = 0x0020, .ranges = alex_cg_ranges, .range_count = SDL_arraysize(alex_cg_ranges) },
    [CHAR_RYU] = { .default_delta = -0x01E0, .ranges = ryu_cg_ranges, .range_count = SDL_arraysize(ryu_cg_ranges) },
    [CHAR_YUN] = { .default_delta = -0x0420, .ranges = yun_cg_ranges, .range_count = SDL_arraysize(yun_cg_ranges) },
    [CHAR_DUDLEY] = { .default_delta = -0x0480,
                      .ranges = dudley_cg_ranges,
                      .range_count = SDL_arraysize(dudley_cg_ranges) },
    [CHAR_NECRO] = { .default_delta = -0x0600,
                     .ranges = necro_cg_ranges,
                     .range_count = SDL_arraysize(necro_cg_ranges) },
    [CHAR_HUGO] = { .default_delta = -0x0720, .ranges = hugo_cg_ranges, .range_count = SDL_arraysize(hugo_cg_ranges) },
    [CHAR_IBUKI] = { .default_delta = -0x0940,
                     .ranges = ibuki_cg_ranges,
                     .range_count = SDL_arraysize(ibuki_cg_ranges) },
    [CHAR_ELENA] = { .default_delta = -0x0820,
                     .ranges = elena_cg_ranges,
                     .range_count = SDL_arraysize(elena_cg_ranges) },
    [CHAR_ORO] = { .default_delta = -0x0800, .ranges = oro_cg_ranges, .range_count = SDL_arraysize(oro_cg_ranges) },
    [CHAR_YANG] = { .default_delta = -0x0820, .ranges = yang_cg_ranges, .range_count = SDL_arraysize(yang_cg_ranges) },
    [CHAR_KEN] = { .default_delta = -0x08C0, .ranges = ken_cg_ranges, .range_count = SDL_arraysize(ken_cg_ranges) },
    [CHAR_SEAN] = { .default_delta = -0x0AA0, .ranges = sean_cg_ranges, .range_count = SDL_arraysize(sean_cg_ranges) },
    [CHAR_URIEN] = { .default_delta = -0x0C60,
                     .ranges = urien_cg_ranges,
                     .range_count = SDL_arraysize(urien_cg_ranges) },
    [CHAR_AKUMA] = { .default_delta = -0x0CA0,
                     .ranges = akuma_cg_ranges,
                     .range_count = SDL_arraysize(akuma_cg_ranges) },
    [CHAR_CHUNLI] = { .default_delta = -0x0E00,
                      .ranges = chunli_cg_ranges,
                      .range_count = SDL_arraysize(chunli_cg_ranges) },
    [CHAR_MAKOTO] = { .default_delta = -0x0D80,
                      .ranges = makoto_cg_ranges,
                      .range_count = SDL_arraysize(makoto_cg_ranges) },
    [CHAR_Q] = { .default_delta = -0x0C20, .ranges = q_cg_ranges, .range_count = SDL_arraysize(q_cg_ranges) },
    [CHAR_TWELVE] = { .default_delta = -0x0B80,
                      .ranges = twelve_cg_ranges,
                      .range_count = SDL_arraysize(twelve_cg_ranges) },
    [CHAR_REMY] = { .default_delta = -0x0D00, .ranges = remy_cg_ranges, .range_count = SDL_arraysize(remy_cg_ranges) },
};

/* The six cg_se divergences (doc §8.Q; full derivation, method and negative
 * results in §21 -- read it before touching these). Six pairs, 29 script
 * cells (17 distinct parsed-buffer positions -- duplicate script offsets
 * share cell bodies). Two of the six resolve to empty TSB slots today and
 * are SILENT, not merely wrong (Alex 0x2FB -> TSB_PL01[27] cmd=0, Oro
 * 0x2F8 -> TSB_PL09[24] cmd=0).
 *
 * Deliberate non-actions, each a recorded decision (§21.9) -- do not "fix":
 *   Yang 0x27F, Yun 0x268   mod-32 equivalent to their PS2 codes (no-ops);
 *   Yang 0x269              PS2 silenced a voice CPS3 plays, and Yang uses
 *                           0x269 correctly elsewhere on both sides;
 *   Q caca[4..7] 0x000      authentic arcade silence (§21.10);
 *   shoto dmca[3] 0x10A     a valid common SE PS2 removed.
 * The deltas across the six pairs share no structure (§21.8): no general
 * arcade->PS2 sound translation exists, exceptions are the only shape. */
static const CgSeRemapPair alex_cg_se_pairs[] = {
    { .from = 0x2FB, .to = 0x3BF },
};

static const CgSeRemapPair oro_cg_se_pairs[] = {
    { .from = 0x2F8, .to = 0x25C },
    { .from = 0x3DF, .to = 0x25D },
};

static const CgSeRemapPair yang_cg_se_pairs[] = {
    { .from = 0x1DF, .to = 0x29E },
};

static const CgSeRemapPair akuma_cg_se_pairs[] = {
    { .from = 0x37E, .to = 0x130 },
};

static const CgSeRemapPair makoto_cg_se_pairs[] = {
    { .from = 0x27E, .to = 0x1DF },
};

static const CharacterCgSeMap cg_se_maps[NUM_CHARS] = {
    [CHAR_ALEX] = { .pairs = alex_cg_se_pairs, .pair_count = SDL_arraysize(alex_cg_se_pairs) },
    [CHAR_ORO] = { .pairs = oro_cg_se_pairs, .pair_count = SDL_arraysize(oro_cg_se_pairs) },
    [CHAR_YANG] = { .pairs = yang_cg_se_pairs, .pair_count = SDL_arraysize(yang_cg_se_pairs) },
    [CHAR_AKUMA] = { .pairs = akuma_cg_se_pairs, .pair_count = SDL_arraysize(akuma_cg_se_pairs) },
    [CHAR_MAKOTO] = { .pairs = makoto_cg_se_pairs, .pair_count = SDL_arraysize(makoto_cg_se_pairs) },
};

#if defined(ENABLE_NETPLAY_TESTS)
/* Test seam for src/test/test_cg_se_remap.c -- the harness sweeps the whole
 * (character, code) domain to prove exactly the six mappings above exist and
 * the flip/priority nibble survives. Compiled only into test builds. */
uint16_t ArcadeCharData_TestRemapCgSe(uint16_t value, Character character) {
    return remap_cg_se(value, character);
}
#endif

#if defined(DEBUG)
// doc §8.C (range-overlap guard): remap_cg_number takes the first matching
// row and stops (see above), so a future row that shadows an earlier one in
// the same character's table would silently remap to the wrong delta with
// no diagnostic. Tables are hand-authored and unsorted, so nothing else
// catches that.
//
// DEVELOPER CONVENIENCE ONLY -- this is compiled out entirely in every
// shipped build. `DEBUG` is defined only for CMAKE_BUILD_TYPE=Debug
// (CMakeLists.txt's target_compile_definitions), and every shipping
// pipeline (tools/mister/build-game.sh) configures Release, so this
// SDL_assert never runs where a user could hit it. It also runs
// unconditionally at the top of ArcadeCharData_Init(), before the ROM is
// even located, so it is not "arcade-only": it runs on PS2-balance Debug
// launches too (ROM missing, or full-cast adaptation failing) -- it is
// gated on build config, not on arcade balance being enabled. The check
// that actually runs in every build, release included, is
// tools/arcade-audit/cg_audit.py's check_range_overlaps().
static void validate_cg_ranges(void) {
    for (int character = 0; character < NUM_CHARS; character++) {
        const CharacterCgMap* map = &cg_maps[character];

        for (size_t i = 0; i < map->range_count; i++) {
            const CgRemapRange* a = &map->ranges[i];
            SDL_assert(a->first <= a->last);

            for (size_t j = i + 1; j < map->range_count; j++) {
                const CgRemapRange* b = &map->ranges[j];
                const bool overlap = a->first <= b->last && b->first <= a->last;
                SDL_assert(!overlap);
            }
        }
    }

    /* Same guard for the cg_se pair tables: remap_cg_se takes the first
     * matching `from` and stops, so a duplicate `from` in one character's
     * table would silently win by position. Codes are the UPPER 12 BITS of
     * the cell field (the nibble is reattached by remap_cg_se), so anything
     * >= 0x1000 is a shifted cell value pasted in by mistake. */
    for (int character = 0; character < NUM_CHARS; character++) {
        const CharacterCgSeMap* map = &cg_se_maps[character];

        for (size_t i = 0; i < map->pair_count; i++) {
            const CgSeRemapPair* a = &map->pairs[i];
            SDL_assert(a->from < 0x1000 && a->to < 0x1000);
            SDL_assert(a->from != a->to);

            for (size_t j = i + 1; j < map->pair_count; j++) {
                SDL_assert(map->pairs[j].from != a->from);
            }
        }
    }
}
#endif

static const size_t section_element_sizes[CHAR_DATA_SECTION_COUNT] = {
    [CHAR_DATA_NMCA] = 1,
    [CHAR_DATA_DMCA] = 1,
    [CHAR_DATA_BTCA] = 1,
    [CHAR_DATA_CACA] = 1,
    [CHAR_DATA_CUCA] = 1,
    [CHAR_DATA_ATCA] = 1,
    [CHAR_DATA_SACA] = 1,
    [CHAR_DATA_EXCA] = 1,
    [CHAR_DATA_CBCA] = 1,
    [CHAR_DATA_YUCA] = 1,
    [CHAR_DATA_STXY] = sizeof(s16),
    [CHAR_DATA_MVXY] = sizeof(s16),
    [CHAR_DATA_SERND] = sizeof(u32),
    [CHAR_DATA_OVCT] = sizeof(OverlapPart),
    [CHAR_DATA_OVIX] = sizeof(OverlapSelection),
    [CHAR_DATA_RICT] = sizeof(CatchTable),
    [CHAR_DATA_HIIT] = sizeof(UNK_0),
    [CHAR_DATA_BODA] = sizeof(UNK_1),
    [CHAR_DATA_HANA] = sizeof(UNK_2),
    [CHAR_DATA_CATA] = sizeof(UNK_3),
    [CHAR_DATA_CAUA] = sizeof(UNK_4),
    [CHAR_DATA_ATTA] = sizeof(UNK_5),
    [CHAR_DATA_HOSA] = sizeof(UNK_6),
    [CHAR_DATA_ATIT] = sizeof(UNK_7),
    [CHAR_DATA_PROT] = sizeof(UNK_Data),
};

static const LocationData location_data[NUM_CHARS] = {
    {
        .nmca = { .offset = 0x1CB230, .size = 0x2660 },
        .dmca = { .offset = 0x1CD890, .size = 0x25B4 },
        .btca = { .offset = 0x1CFE44, .size = 0x1554 },
        .caca = { .offset = 0x1D1398, .size = 0x578 },
        .cuca = { .offset = 0x1D1910, .size = 0x2D54 },
        .atca = { .offset = 0x1D4664, .size = 0x2358 },
        .saca = { .offset = 0x1DBC98, .size = 0x25B4 },
        .exca = { .offset = 0x1D9ECC, .size = 0x1DCC },
        .cbca = { .offset = 0x1DE24C, .size = 0x2F0 },
        .yuca = { .offset = 0x36068C, .size = 0x1388 },
        .stxy = { .offset = 0x45D664, .size = 0x1C4 },
        .mvxy = { .offset = 0x45D828, .size = 0x270 },
        .sernd = { .offset = 0x453FB0, .size = 0x24 },
        .ovct = { .offset = 0x1D6E4C, .size = 0x1880 },
        .ovix = { .offset = 0x1D69BC, .size = 0x490 },
        .rict = { .offset = 0x1D86CC, .size = 0x1800 },
        .hiit = { .offset = 0x3EE998, .size = 0xD80 },
        .boda = { .offset = 0x3EF718, .size = 0x1600 },
        .hana = { .offset = 0x3F0D18, .size = 0x320 },
        .cata = { .offset = 0x443D58, .size = 0x10 },
        .caua = { .offset = 0x443D68, .size = 0x40 },
        .atta = { .offset = 0x443798, .size = 0x5C0 },
        .hosa = { .offset = 0x3F1038, .size = 0x60 },
        .atit = { .offset = 0x453D80, .size = 0x230 },
        .prot = { .offset = 0x45DA98, .size = 0x420 },
    },
    {
        .nmca = { .offset = 0x1DE53C, .size = 0x1C58 },
        .dmca = { .offset = 0x1E0194, .size = 0x27EC },
        .btca = { .offset = 0x1E2980, .size = 0x1694 },
        .caca = { .offset = 0x1E4014, .size = 0x33E8 },
        .cuca = { .offset = 0x1E73FC, .size = 0x2D1C },
        .atca = { .offset = 0x1EA118, .size = 0x2994 },
        .saca = { .offset = 0x1F4EC4, .size = 0x3EE8 },
        .exca = { .offset = 0x1F33A4, .size = 0x1B20 },
        .cbca = { .offset = 0x1F8DAC, .size = 0x4BC },
        .yuca = { .offset = 0x361A14, .size = 0x1150 },
        .stxy = { .offset = 0x45DEB8, .size = 0x3D0 },
        .mvxy = { .offset = 0x45E288, .size = 0x45C },
        .sernd = { .offset = 0x454624, .size = 0x24 },
        .ovct = { .offset = 0x1ECC54, .size = 0x390 },
        .ovix = { .offset = 0x1ECAAC, .size = 0x1A8 },
        .rict = { .offset = 0x1ECFE4, .size = 0x63C0 },
        .hiit = { .offset = 0x3F1098, .size = 0x13F0 },
        .boda = { .offset = 0x3F2488, .size = 0x1F00 },
        .hana = { .offset = 0x3F4388, .size = 0x800 },
        .cata = { .offset = 0x444768, .size = 0xC8 },
        .caua = { .offset = 0x444830, .size = 0x90 },
        .atta = { .offset = 0x443DA8, .size = 0x9C0 },
        .hosa = { .offset = 0x3F4B88, .size = 0xE8 },
        .atit = { .offset = 0x453FD4, .size = 0x650 },
        .prot = { .offset = 0x45E6E4, .size = 0x570 },
    },
    {
        .nmca = { .offset = 0x1F9268, .size = 0x16D0 },
        .dmca = { .offset = 0x1FA938, .size = 0x217C },
        .btca = { .offset = 0x1FCAB4, .size = 0x1584 },
        .caca = { .offset = 0x1FE038, .size = 0x724 },
        .cuca = { .offset = 0x1FE75C, .size = 0x2D24 },
        .atca = { .offset = 0x201480, .size = 0x2114 },
        .saca = { .offset = 0x20669C, .size = 0x2748 },
        .exca = { .offset = 0x20476C, .size = 0x1F30 },
        .cbca = { .offset = 0x208DE4, .size = 0x9D0 },
        .yuca = { .offset = 0x362B64, .size = 0x6E8 },
        .stxy = { .offset = 0x45EC54, .size = 0x1B0 },
        .mvxy = { .offset = 0x45EE04, .size = 0x324 },
        .sernd = { .offset = 0x454C08, .size = 0x24 },
        .ovct = { .offset = 0x2035CC, .size = 0x360 },
        .ovix = { .offset = 0x203594, .size = 0x38 },
        .rict = { .offset = 0x20392C, .size = 0xE40 },
        .hiit = { .offset = 0x3F4C70, .size = 0x11B0 },
        .boda = { .offset = 0x3F5E20, .size = 0x1580 },
        .hana = { .offset = 0x3F73A0, .size = 0x4A0 },
        .cata = { .offset = 0x445120, .size = 0x10 },
        .caua = { .offset = 0x445130, .size = 0x30 },
        .atta = { .offset = 0x4448C0, .size = 0x860 },
        .hosa = { .offset = 0x3F7840, .size = 0x30 },
        .atit = { .offset = 0x454648, .size = 0x5C0 },
        .prot = { .offset = 0x45F128, .size = 0x840 },
    },
    {
        .nmca = { .offset = 0x2097B4, .size = 0x22A4 },
        .dmca = { .offset = 0x20BA58, .size = 0x275C },
        .btca = { .offset = 0x20E1B4, .size = 0x14EC },
        .caca = { .offset = 0x20F6A0, .size = 0x184C },
        .cuca = { .offset = 0x210EEC, .size = 0x2D14 },
        .atca = { .offset = 0x213C00, .size = 0x6AC4 },
        .saca = { .offset = 0x2218E0, .size = 0x7D40 },
        .exca = { .offset = 0x21E604, .size = 0x32DC },
        .cbca = { .offset = 0x229620, .size = 0xE34 },
        .yuca = { .offset = 0x36324C, .size = 0x15F8 },
        .stxy = { .offset = 0x45F968, .size = 0x3A8 },
        .mvxy = { .offset = 0x45FD10, .size = 0x42C },
        .sernd = { .offset = 0x45570C, .size = 0x90 },
        .ovct = { .offset = 0x21A764, .size = 0x60 },
        .ovix = { .offset = 0x21A6C4, .size = 0xA0 },
        .rict = { .offset = 0x21A7C4, .size = 0x3E40 },
        .hiit = { .offset = 0x3F7870, .size = 0x18B0 },
        .boda = { .offset = 0x3F9120, .size = 0x2020 },
        .hana = { .offset = 0x3FB140, .size = 0x980 },
        .cata = { .offset = 0x446060, .size = 0x18 },
        .caua = { .offset = 0x446078, .size = 0x98 },
        .atta = { .offset = 0x445160, .size = 0xF00 },
        .hosa = { .offset = 0x3FBAC0, .size = 0xB0 },
        .atit = { .offset = 0x454C2C, .size = 0xAE0 },
        .prot = { .offset = 0x46013C, .size = 0x7E0 },
    },
    {
        .nmca = { .offset = 0x22A454, .size = 0x1C0C },
        .dmca = { .offset = 0x22C060, .size = 0x2954 },
        .btca = { .offset = 0x22E9B4, .size = 0x160C },
        .caca = { .offset = 0x22FFC0, .size = 0x7B8 },
        .cuca = { .offset = 0x230778, .size = 0x2CA4 },
        .atca = { .offset = 0x23341C, .size = 0x4200 },
        .saca = { .offset = 0x23A644, .size = 0x6AE4 },
        .exca = { .offset = 0x238DC4, .size = 0x1880 },
        .cbca = { .offset = 0x241128, .size = 0xF10 },
        .yuca = { .offset = 0x364844, .size = 0xD58 },
        .stxy = { .offset = 0x46091C, .size = 0x1A0 },
        .mvxy = { .offset = 0x460ABC, .size = 0x3FC },
        .sernd = { .offset = 0x455EFC, .size = 0x24 },
        .ovct = { .offset = 0x237764, .size = 0xB20 },
        .ovix = { .offset = 0x23761C, .size = 0x148 },
        .rict = { .offset = 0x238284, .size = 0xB40 },
        .hiit = { .offset = 0x3FBB70, .size = 0x1050 },
        .boda = { .offset = 0x3FCBC0, .size = 0x17A0 },
        .hana = { .offset = 0x3FE360, .size = 0x460 },
        .cata = { .offset = 0x446A50, .size = 0x10 },
        .caua = { .offset = 0x446A60, .size = 0xB8 },
        .atta = { .offset = 0x446110, .size = 0x940 },
        .hosa = { .offset = 0x3FE7C0, .size = 0xF0 },
        .atit = { .offset = 0x45579C, .size = 0x760 },
        .prot = { .offset = 0x460EB8, .size = 0x7B0 },
    },
    {
        .nmca = { .offset = 0x242038, .size = 0x1D78 },
        .dmca = { .offset = 0x243DB0, .size = 0x278C },
        .btca = { .offset = 0x24653C, .size = 0x132C },
        .caca = { .offset = 0x247868, .size = 0x2124 },
        .cuca = { .offset = 0x24998C, .size = 0x2D04 },
        .atca = { .offset = 0x24C690, .size = 0x3A04 },
        .saca = { .offset = 0x254AE4, .size = 0x3FF8 },
        .exca = { .offset = 0x2536C4, .size = 0x1420 },
        .cbca = { .offset = 0x258ADC, .size = 0x534 },
        .yuca = { .offset = 0x36559C, .size = 0xE20 },
        .stxy = { .offset = 0x461668, .size = 0x21C },
        .mvxy = { .offset = 0x461884, .size = 0x264 },
        .sernd = { .offset = 0x4564E0, .size = 0x24 },
        .ovct = { .offset = 0x2500E4, .size = 0x2E0 },
        .ovix = { .offset = 0x250094, .size = 0x50 },
        .rict = { .offset = 0x2503C4, .size = 0x3300 },
        .hiit = { .offset = 0x3FE8B0, .size = 0x1410 },
        .boda = { .offset = 0x3FFCC0, .size = 0x1C80 },
        .hana = { .offset = 0x401940, .size = 0x940 },
        .cata = { .offset = 0x447278, .size = 0x30 },
        .caua = { .offset = 0x4472A8, .size = 0x88 },
        .atta = { .offset = 0x446B18, .size = 0x760 },
        .hosa = { .offset = 0x402280, .size = 0xD8 },
        .atit = { .offset = 0x455F20, .size = 0x5C0 },
        .prot = { .offset = 0x461AE8, .size = 0x450 },
    },
    {
        .nmca = { .offset = 0x259010, .size = 0x1A48 },
        .dmca = { .offset = 0x25AA58, .size = 0x241C },
        .btca = { .offset = 0x25CE74, .size = 0x156C },
        .caca = { .offset = 0x25E3E0, .size = 0x2C40 },
        .cuca = { .offset = 0x261020, .size = 0x2D44 },
        .atca = { .offset = 0x263D64, .size = 0x211C },
        .saca = { .offset = 0x26ECC0, .size = 0x4164 },
        .exca = { .offset = 0x26C360, .size = 0x2960 },
        .cbca = { .offset = 0x272E24, .size = 0x920 },
        .yuca = { .offset = 0x3663BC, .size = 0xD88 },
        .stxy = { .offset = 0x461F38, .size = 0xF4 },
        .mvxy = { .offset = 0x46202C, .size = 0x474 },
        .sernd = { .offset = 0x456984, .size = 0x24 },
        .ovct = { .offset = 0x266070, .size = 0x470 },
        .ovix = { .offset = 0x265E80, .size = 0x1F0 },
        .rict = { .offset = 0x2664E0, .size = 0x5E80 },
        .hiit = { .offset = 0x402358, .size = 0x1020 },
        .boda = { .offset = 0x403378, .size = 0x11C0 },
        .hana = { .offset = 0x404538, .size = 0x660 },
        .cata = { .offset = 0x447B50, .size = 0x60 },
        .caua = { .offset = 0x447BB0, .size = 0x60 },
        .atta = { .offset = 0x447330, .size = 0x820 },
        .hosa = { .offset = 0x404B98, .size = 0x80 },
        .atit = { .offset = 0x456504, .size = 0x480 },
        .prot = { .offset = 0x4624A0, .size = 0x630 },
    },
    {
        .nmca = { .offset = 0x273744, .size = 0x2458 },
        .dmca = { .offset = 0x275B9C, .size = 0x302C },
        .btca = { .offset = 0x278BC8, .size = 0x1674 },
        .caca = { .offset = 0x27A23C, .size = 0x2144 },
        .cuca = { .offset = 0x27C380, .size = 0x2CC4 },
        .atca = { .offset = 0x27F044, .size = 0x54C4 },
        .saca = { .offset = 0x296D50, .size = 0x9158 },
        .exca = { .offset = 0x293C18, .size = 0x3138 },
        .cbca = { .offset = 0x29FEA8, .size = 0xCF4 },
        .yuca = { .offset = 0x367144, .size = 0x1820 },
        .stxy = { .offset = 0x462AD0, .size = 0x354 },
        .mvxy = { .offset = 0x462E24, .size = 0x6D8 },
        .sernd = { .offset = 0x457518, .size = 0x24 },
        .ovct = { .offset = 0x288AB8, .size = 0x8EE0 },
        .ovix = { .offset = 0x284508, .size = 0x45B0 },
        .rict = { .offset = 0x291998, .size = 0x2280 },
        .hiit = { .offset = 0x404C18, .size = 0x1970 },
        .boda = { .offset = 0x406588, .size = 0x2220 },
        .hana = { .offset = 0x4087A8, .size = 0xB00 },
        .cata = { .offset = 0x448C30, .size = 0x60 },
        .caua = { .offset = 0x448C90, .size = 0x68 },
        .atta = { .offset = 0x447C10, .size = 0x1020 },
        .hosa = { .offset = 0x4092A8, .size = 0x90 },
        .atit = { .offset = 0x4569A8, .size = 0xB70 },
        .prot = { .offset = 0x4634FC, .size = 0x720 },
    },
    {
        .nmca = { .offset = 0x2A0B9C, .size = 0x2BB0 },
        .dmca = { .offset = 0x2A374C, .size = 0x2754 },
        .btca = { .offset = 0x2A5EA0, .size = 0x148C },
        .caca = { .offset = 0x2A732C, .size = 0x1DC },
        .cuca = { .offset = 0x2A7508, .size = 0x2D8C },
        .atca = { .offset = 0x2AA294, .size = 0x45EC },
        .saca = { .offset = 0x2B2F04, .size = 0x6638 },
        .exca = { .offset = 0x2AF948, .size = 0x35BC },
        .cbca = { .offset = 0x2B953C, .size = 0xA80 },
        .yuca = { .offset = 0x368964, .size = 0x2DA8 },
        .stxy = { .offset = 0x463C1C, .size = 0x2C0 },
        .mvxy = { .offset = 0x463EDC, .size = 0x39C },
        .sernd = { .offset = 0x457C8C, .size = 0x48 },
        .ovct = { .offset = 0x2AEB58, .size = 0x5B0 },
        .ovix = { .offset = 0x2AE880, .size = 0x2D8 },
        .rict = { .offset = 0x2AF108, .size = 0x840 },
        .hiit = { .offset = 0x409338, .size = 0x1D10 },
        .boda = { .offset = 0x40B048, .size = 0x3180 },
        .hana = { .offset = 0x40E1C8, .size = 0x10A0 },
        .cata = { .offset = 0x4498B8, .size = 0x10 },
        .caua = { .offset = 0x4498C8, .size = 0x50 },
        .atta = { .offset = 0x448CF8, .size = 0xBC0 },
        .hosa = { .offset = 0x40F268, .size = 0x60 },
        .atit = { .offset = 0x45753C, .size = 0x750 },
        .prot = { .offset = 0x464278, .size = 0x5A0 },
    },
    {
        .nmca = { .offset = 0x2B9FBC, .size = 0x1D88 },
        .dmca = { .offset = 0x2BBD44, .size = 0x22CC },
        .btca = { .offset = 0x2BE010, .size = 0x1474 },
        .caca = { .offset = 0x2BF484, .size = 0x24D0 },
        .cuca = { .offset = 0x2C1954, .size = 0x2D0C },
        .atca = { .offset = 0x2C4660, .size = 0x3540 },
        .saca = { .offset = 0x2CD3F8, .size = 0x4054 },
        .exca = { .offset = 0x2CB658, .size = 0x1DA0 },
        .cbca = { .offset = 0x2D144C, .size = 0xB30 },
        .yuca = { .offset = 0x36B70C, .size = 0x1880 },
        .stxy = { .offset = 0x464818, .size = 0x17C },
        .mvxy = { .offset = 0x464994, .size = 0x384 },
        .sernd = { .offset = 0x458274, .size = 0x24 },
        .ovct = { .offset = 0x2C7CE8, .size = 0x430 },
        .ovix = { .offset = 0x2C7BA0, .size = 0x148 },
        .rict = { .offset = 0x2C8118, .size = 0x3540 },
        .hiit = { .offset = 0x40F2C8, .size = 0x11F0 },
        .boda = { .offset = 0x4104B8, .size = 0x1B40 },
        .hana = { .offset = 0x411FF8, .size = 0x840 },
        .cata = { .offset = 0x44A0F8, .size = 0x50 },
        .caua = { .offset = 0x44A148, .size = 0x68 },
        .atta = { .offset = 0x449918, .size = 0x7E0 },
        .hosa = { .offset = 0x412838, .size = 0xB0 },
        .atit = { .offset = 0x457CD4, .size = 0x5A0 },
        .prot = { .offset = 0x464D18, .size = 0x570 },
    },
    {
        .nmca = { .offset = 0x2D1F7C, .size = 0x1DEC },
        .dmca = { .offset = 0x2D3D68, .size = 0x274C },
        .btca = { .offset = 0x2D64B4, .size = 0x150C },
        .caca = { .offset = 0x2D79C0, .size = 0x161C },
        .cuca = { .offset = 0x2D8FDC, .size = 0x2D14 },
        .atca = { .offset = 0x2DBCF0, .size = 0x603C },
        .saca = { .offset = 0x2E8F64, .size = 0x7054 },
        .exca = { .offset = 0x2E5ECC, .size = 0x3098 },
        .cbca = { .offset = 0x2EFFB8, .size = 0xF50 },
        .yuca = { .offset = 0x36CF8C, .size = 0x11E8 },
        .stxy = { .offset = 0x465288, .size = 0x3F4 },
        .mvxy = { .offset = 0x46567C, .size = 0x4EC },
        .sernd = { .offset = 0x458CF8, .size = 0x48 },
        .ovct = { .offset = 0x2E1DCC, .size = 0x140 },
        .ovix = { .offset = 0x2E1D2C, .size = 0xA0 },
        .rict = { .offset = 0x2E1F0C, .size = 0x3FC0 },
        .hiit = { .offset = 0x4128E8, .size = 0x1840 },
        .boda = { .offset = 0x414128, .size = 0x1EA0 },
        .hana = { .offset = 0x415FC8, .size = 0x940 },
        .cata = { .offset = 0x44B030, .size = 0x18 },
        .caua = { .offset = 0x44B048, .size = 0x98 },
        .atta = { .offset = 0x44A1B0, .size = 0xE80 },
        .hosa = { .offset = 0x416908, .size = 0xB8 },
        .atit = { .offset = 0x458298, .size = 0xA60 },
        .prot = { .offset = 0x465B68, .size = 0x630 },
    },
    {
        .nmca = { .offset = 0x2F0F08, .size = 0x16D0 },
        .dmca = { .offset = 0x2F25D8, .size = 0x217C },
        .btca = { .offset = 0x2F4754, .size = 0x1584 },
        .caca = { .offset = 0x2F5CD8, .size = 0xE38 },
        .cuca = { .offset = 0x2F6B10, .size = 0x2D24 },
        .atca = { .offset = 0x2F9834, .size = 0x2CEC },
        .saca = { .offset = 0x3001EC, .size = 0x26A8 },
        .exca = { .offset = 0x2FEA40, .size = 0x17AC },
        .cbca = { .offset = 0x302894, .size = 0x5D0 },
        .yuca = { .offset = 0x36E174, .size = 0x938 },
        .stxy = { .offset = 0x466198, .size = 0x250 },
        .mvxy = { .offset = 0x4663E8, .size = 0x360 },
        .sernd = { .offset = 0x4594B0, .size = 0x24 },
        .ovct = { .offset = 0x2FC5E0, .size = 0x2A0 },
        .ovix = { .offset = 0x2FC520, .size = 0xC0 },
        .rict = { .offset = 0x2FC880, .size = 0x21C0 },
        .hiit = { .offset = 0x4169C0, .size = 0x1040 },
        .boda = { .offset = 0x417A00, .size = 0x12C0 },
        .hana = { .offset = 0x418CC0, .size = 0x540 },
        .cata = { .offset = 0x44B9E0, .size = 0x10 },
        .caua = { .offset = 0x44B9F0, .size = 0x30 },
        .atta = { .offset = 0x44B0E0, .size = 0x900 },
        .hosa = { .offset = 0x419200, .size = 0x30 },
        .atit = { .offset = 0x458D40, .size = 0x770 },
        .prot = { .offset = 0x466748, .size = 0x750 },
    },
    {
        .nmca = { .offset = 0x302E64, .size = 0x16D0 },
        .dmca = { .offset = 0x304534, .size = 0x215C },
        .btca = { .offset = 0x306690, .size = 0x1584 },
        .caca = { .offset = 0x307C14, .size = 0x12C0 },
        .cuca = { .offset = 0x308ED4, .size = 0x2D24 },
        .atca = { .offset = 0x30BBF8, .size = 0x21AC },
        .saca = { .offset = 0x311BD0, .size = 0x2A88 },
        .exca = { .offset = 0x3101BC, .size = 0x1A14 },
        .cbca = { .offset = 0x314658, .size = 0x73C },
        .yuca = { .offset = 0x36EAAC, .size = 0xDA8 },
        .stxy = { .offset = 0x466E98, .size = 0x350 },
        .mvxy = { .offset = 0x4671E8, .size = 0x564 },
        .sernd = { .offset = 0x459B74, .size = 0x24 },
        .ovct = { .offset = 0x30DE6C, .size = 0x190 },
        .ovix = { .offset = 0x30DDA4, .size = 0xC8 },
        .rict = { .offset = 0x30DFFC, .size = 0x21C0 },
        .hiit = { .offset = 0x419230, .size = 0x1100 },
        .boda = { .offset = 0x41A330, .size = 0x1480 },
        .hana = { .offset = 0x41B7B0, .size = 0x620 },
        .cata = { .offset = 0x44CA28, .size = 0x18 },
        .caua = { .offset = 0x44CA40, .size = 0xD0 },
        .atta = { .offset = 0x44BA20, .size = 0x1008 },
        .hosa = { .offset = 0x41BDD0, .size = 0x38 },
        .atit = { .offset = 0x4594D4, .size = 0x6A0 },
        .prot = { .offset = 0x46774C, .size = 0x750 },
    },
    {
        .nmca = { .offset = 0x314D94, .size = 0x1A78 },
        .dmca = { .offset = 0x31680C, .size = 0x2594 },
        .btca = { .offset = 0x318DA0, .size = 0x148C },
        .caca = { .offset = 0x31A22C, .size = 0x568 },
        .cuca = { .offset = 0x31A794, .size = 0x2D54 },
        .atca = { .offset = 0x31D4E8, .size = 0x290C },
        .saca = { .offset = 0x324858, .size = 0x3A54 },
        .exca = { .offset = 0x32305C, .size = 0x17FC },
        .cbca = { .offset = 0x3282AC, .size = 0x47C },
        .yuca = { .offset = 0x36F854, .size = 0x1058 },
        .stxy = { .offset = 0x467E9C, .size = 0x25C },
        .mvxy = { .offset = 0x4680F8, .size = 0x330 },
        .sernd = { .offset = 0x459F28, .size = 0x24 },
        .ovct = { .offset = 0x3203CC, .size = 0x1490 },
        .ovix = { .offset = 0x31FDF4, .size = 0x5D8 },
        .rict = { .offset = 0x32185C, .size = 0x1800 },
        .hiit = { .offset = 0x41BE08, .size = 0xF30 },
        .boda = { .offset = 0x41CD38, .size = 0x16E0 },
        .hana = { .offset = 0x41E418, .size = 0x580 },
        .cata = { .offset = 0x44D1F0, .size = 0x10 },
        .caua = { .offset = 0x44D200, .size = 0x48 },
        .atta = { .offset = 0x44CB10, .size = 0x6E0 },
        .hosa = { .offset = 0x41E998, .size = 0x78 },
        .atit = { .offset = 0x459B98, .size = 0x390 },
        .prot = { .offset = 0x468428, .size = 0x480 },
    },
    {
        .nmca = { .offset = 0x328728, .size = 0x16A0 },
        .dmca = { .offset = 0x329DC8, .size = 0x217C },
        .btca = { .offset = 0x32BF44, .size = 0x1584 },
        .caca = { .offset = 0x32D4C8, .size = 0xF3C },
        .cuca = { .offset = 0x32E404, .size = 0x2D24 },
        .atca = { .offset = 0x331128, .size = 0x2810 },
        .saca = { .offset = 0x337EBC, .size = 0x3544 },
        .exca = { .offset = 0x335978, .size = 0x2544 },
        .cbca = { .offset = 0x33B400, .size = 0x788 },
        .yuca = { .offset = 0x3708AC, .size = 0xD84 },
        .stxy = { .offset = 0x4688A8, .size = 0x240 },
        .mvxy = { .offset = 0x468AE8, .size = 0x4A4 },
        .sernd = { .offset = 0x45A54C, .size = 0x24 },
        .ovct = { .offset = 0x333A48, .size = 0x730 },
        .ovix = { .offset = 0x333938, .size = 0x110 },
        .rict = { .offset = 0x334178, .size = 0x1800 },
        .hiit = { .offset = 0x41EA10, .size = 0x1190 },
        .boda = { .offset = 0x41FBA0, .size = 0x1540 },
        .hana = { .offset = 0x4210E0, .size = 0x620 },
        .cata = { .offset = 0x44DC28, .size = 0x98 },
        .caua = { .offset = 0x44DCC0, .size = 0x30 },
        .atta = { .offset = 0x44D248, .size = 0x9E0 },
        .hosa = { .offset = 0x421700, .size = 0x30 },
        .atit = { .offset = 0x459F4C, .size = 0x600 },
        .prot = { .offset = 0x468F8C, .size = 0x930 },
    },
    {
        .nmca = { .offset = 0x3723B4, .size = 0x21F8 },
        .dmca = { .offset = 0x3745AC, .size = 0x2574 },
        .btca = { .offset = 0x376B20, .size = 0x151C },
        .caca = { .offset = 0x37803C, .size = 0x684 },
        .cuca = { .offset = 0x3786C0, .size = 0x2CF4 },
        .atca = { .offset = 0x37B3B4, .size = 0x3710 },
        .saca = { .offset = 0x382940, .size = 0x57CC },
        .exca = { .offset = 0x38024C, .size = 0x26F4 },
        .cbca = { .offset = 0x38810C, .size = 0x660 },
        .yuca = { .offset = 0x3E2E00, .size = 0x1000 },
        .stxy = { .offset = 0x46A544, .size = 0x28C },
        .mvxy = { .offset = 0x46A7D0, .size = 0x234 },
        .sernd = { .offset = 0x45B294, .size = 0x24 },
        .ovct = { .offset = 0x37ED1C, .size = 0x4B0 },
        .ovix = { .offset = 0x37EAC4, .size = 0x258 },
        .rict = { .offset = 0x37F1CC, .size = 0x1080 },
        .hiit = { .offset = 0x423D50, .size = 0x1FF0 },
        .boda = { .offset = 0x425D40, .size = 0x3240 },
        .hana = { .offset = 0x428F80, .size = 0x1B00 },
        .cata = { .offset = 0x44F310, .size = 0x18 },
        .caua = { .offset = 0x44F328, .size = 0xA8 },
        .atta = { .offset = 0x44E670, .size = 0xCA0 },
        .hosa = { .offset = 0x42AA80, .size = 0x48 },
        .atit = { .offset = 0x45AAD4, .size = 0x7C0 },
        .prot = { .offset = 0x46AA04, .size = 0x780 },
    },
    {
        .nmca = { .offset = 0x38876C, .size = 0x2350 },
        .dmca = { .offset = 0x38AABC, .size = 0x230C },
        .btca = { .offset = 0x38CDC8, .size = 0x152C },
        .caca = { .offset = 0x38E2F4, .size = 0xA8C },
        .cuca = { .offset = 0x38ED80, .size = 0x2D04 },
        .atca = { .offset = 0x391A84, .size = 0x31BC },
        .saca = { .offset = 0x39D7DC, .size = 0x4E3C },
        .exca = { .offset = 0x399898, .size = 0x3F44 },
        .cbca = { .offset = 0x3A2618, .size = 0x594 },
        .yuca = { .offset = 0x3E3E00, .size = 0x1260 },
        .stxy = { .offset = 0x46B184, .size = 0x344 },
        .mvxy = { .offset = 0x46B4C8, .size = 0x2B8 },
        .sernd = { .offset = 0x45B798, .size = 0x24 },
        .ovct = { .offset = 0x395098, .size = 0xFC0 },
        .ovix = { .offset = 0x394C40, .size = 0x458 },
        .rict = { .offset = 0x396058, .size = 0x3840 },
        .hiit = { .offset = 0x42AAC8, .size = 0x1680 },
        .boda = { .offset = 0x42C148, .size = 0x2240 },
        .hana = { .offset = 0x42E388, .size = 0xBA0 },
        .cata = { .offset = 0x44FD30, .size = 0x30 },
        .caua = { .offset = 0x44FD60, .size = 0x88 },
        .atta = { .offset = 0x44F3D0, .size = 0x960 },
        .hosa = { .offset = 0x42EF28, .size = 0xA0 },
        .atit = { .offset = 0x45B2B8, .size = 0x4E0 },
        .prot = { .offset = 0x46B780, .size = 0x660 },
    },
    {
        .nmca = { .offset = 0x3A2BAC, .size = 0x2358 },
        .dmca = { .offset = 0x3A4F04, .size = 0x293C },
        .btca = { .offset = 0x3A7840, .size = 0x148C },
        .caca = { .offset = 0x3A8CCC, .size = 0x13A8 },
        .cuca = { .offset = 0x3AA074, .size = 0x2DAC },
        .atca = { .offset = 0x3ACE20, .size = 0x2F5C },
        .saca = { .offset = 0x3B41D0, .size = 0x52DC },
        .exca = { .offset = 0x3B262C, .size = 0x1BA4 },
        .cbca = { .offset = 0x3B94AC, .size = 0x5B8 },
        .yuca = { .offset = 0x3E5060, .size = 0x13B0 },
        .stxy = { .offset = 0x46BDE0, .size = 0x3FC },
        .mvxy = { .offset = 0x46C1DC, .size = 0x270 },
        .sernd = { .offset = 0x45BC8C, .size = 0x24 },
        .ovct = { .offset = 0x3AFE0C, .size = 0x120 },
        .ovix = { .offset = 0x3AFD7C, .size = 0x90 },
        .rict = { .offset = 0x3AFF2C, .size = 0x2700 },
        .hiit = { .offset = 0x42EFC8, .size = 0x1E90 },
        .boda = { .offset = 0x430E58, .size = 0x3140 },
        .hana = { .offset = 0x433F98, .size = 0x1420 },
        .cata = { .offset = 0x450848, .size = 0x30 },
        .caua = { .offset = 0x450878, .size = 0x30 },
        .atta = { .offset = 0x44FDE8, .size = 0xA60 },
        .hosa = { .offset = 0x4353B8, .size = 0x40 },
        .atit = { .offset = 0x45B7BC, .size = 0x4D0 },
        .prot = { .offset = 0x46C44C, .size = 0x870 },
    },
    {
        .nmca = { .offset = 0x3B9A64, .size = 0x1E38 },
        .dmca = { .offset = 0x3BB89C, .size = 0x297C },
        .btca = { .offset = 0x3BE218, .size = 0x134C },
        .caca = { .offset = 0x3BF564, .size = 0xDF8 },
        .cuca = { .offset = 0x3C035C, .size = 0x2CD4 },
        .atca = { .offset = 0x3C3030, .size = 0x2F94 },
        .saca = { .offset = 0x3CA60C, .size = 0x5E50 },
        .exca = { .offset = 0x3C8AFC, .size = 0x1B10 },
        .cbca = { .offset = 0x3D045C, .size = 0x89C },
        .yuca = { .offset = 0x3E6410, .size = 0x11A8 },
        .stxy = { .offset = 0x46CCBC, .size = 0x22C },
        .mvxy = { .offset = 0x46CEE8, .size = 0x2E8 },
        .sernd = { .offset = 0x45C330, .size = 0x24 },
        .ovct = { .offset = 0x3C63EC, .size = 0x850 },
        .ovix = { .offset = 0x3C5FC4, .size = 0x428 },
        .rict = { .offset = 0x3C6C3C, .size = 0x1EC0 },
        .hiit = { .offset = 0x4353F8, .size = 0x1F70 },
        .boda = { .offset = 0x437368, .size = 0x2EE0 },
        .hana = { .offset = 0x43A248, .size = 0x2640 },
        .cata = { .offset = 0x451908, .size = 0x1A8 },
        .caua = { .offset = 0x451AB0, .size = 0x98 },
        .atta = { .offset = 0x4508A8, .size = 0x1060 },
        .hosa = { .offset = 0x43C888, .size = 0xF8 },
        .atit = { .offset = 0x45BCB0, .size = 0x680 },
        .prot = { .offset = 0x46D1D0, .size = 0x4B0 },
    },
    {
        .nmca = { .offset = 0x3D0CF8, .size = 0x23A0 },
        .dmca = { .offset = 0x3D3098, .size = 0x2504 },
        .btca = { .offset = 0x3D559C, .size = 0x1394 },
        .caca = { .offset = 0x3D6930, .size = 0x3E8 },
        .cuca = { .offset = 0x3D6D18, .size = 0x2D64 },
        .atca = { .offset = 0x3D9A7C, .size = 0x227C },
        .saca = { .offset = 0x3DF2B4, .size = 0x36B4 },
        .exca = { .offset = 0x3DCB10, .size = 0x27A4 },
        .cbca = { .offset = 0x3E2968, .size = 0x498 },
        .yuca = { .offset = 0x3E75B8, .size = 0x73E0 },
        .stxy = { .offset = 0x46D680, .size = 0x400 },
        .mvxy = { .offset = 0x46DA80, .size = 0x2F4 },
        .sernd = { .offset = 0x45C844, .size = 0x24 },
        .ovct = { .offset = 0x3DBDF0, .size = 0x2A0 },
        .ovix = { .offset = 0x3DBCF8, .size = 0xF8 },
        .rict = { .offset = 0x3DC090, .size = 0xA80 },
        .hiit = { .offset = 0x43C980, .size = 0x1910 },
        .boda = { .offset = 0x43E290, .size = 0x2720 },
        .hana = { .offset = 0x4409B0, .size = 0x1080 },
        .cata = { .offset = 0x452528, .size = 0x10 },
        .caua = { .offset = 0x452538, .size = 0x1848 },
        .atta = { .offset = 0x451B48, .size = 0x9E0 },
        .hosa = { .offset = 0x441A30, .size = 0x1D68 },
        .atit = { .offset = 0x45C354, .size = 0x4F0 },
        .prot = { .offset = 0x46DD74, .size = 0x570 },
    },
};
