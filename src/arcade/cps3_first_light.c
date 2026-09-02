#include "arcade/cps3_first_light.h"

#include "sf33rd/Source/Common/PPGFile.h"

#include <SDL3/SDL.h>
#include <minizip-ng/mz.h>
#include <minizip-ng/mz_strm.h>
#include <minizip-ng/mz_strm_os.h>
#include <minizip-ng/mz_zip.h>

#include <stdbool.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * 1. LOADER -- SIMM3-6 pair-interleave (doc §5B.3), dev-only, no digests.
 * ------------------------------------------------------------------- */

#define GFX_SIMM_FILE_SIZE (2u * 1024u * 1024u)  /* one sfiii3-simmN.M entry */
#define GFX_SIMM_FILE_COUNT 32u                  /* SIMM3.0-7 .. SIMM6.0-7 */
#define GFX_IMAGE_SIZE (64u * 1024u * 1024u)      /* 4 SIMMs x 16 MiB each */

/* CRC32 only -- explicitly a dev path (see the header). Values are MAME's
 * own for ROM_START(sfiii3n)'s simm3.0..simm6.7 lines, cps3.cpp @ master
 * 476fc5d6 (doc §5O's pin). Verified byte-for-byte identical to
 * ROM_START(sfiii3nr1)'s lines in the same file at the same commit (no
 * `// sldh` marker on any of the 32, unlike every SIMM1 line) -- i.e.
 * SIMM3-6 are revision-independent the same way §5I.2 already proved for
 * SIMM2, not merely assumed here. That identity was also confirmed
 * empirically: pair-interleaving these exact 32 files out of the
 * `sfiii3n/` (990608) folder of a merged sfiii3.zip reproduces
 * `/Volumes/KimchDrive/3sx-research-logs/cg-dma/rom/romuser.bin` (an
 * independently-built 64 MiB research artifact) byte-for-byte. */
typedef struct GfxSimmSpec {
    const char* name;
    Uint32 crc32;
} GfxSimmSpec;

static const GfxSimmSpec gfx_simm_specs[GFX_SIMM_FILE_COUNT] = {
    { "sfiii3-simm3.0", 0x7BAA1F79U }, { "sfiii3-simm3.1", 0x234BF8FEU },
    { "sfiii3-simm3.2", 0xD9EBC308U }, { "sfiii3-simm3.3", 0x293CBA77U },
    { "sfiii3-simm3.4", 0x6055E747U }, { "sfiii3-simm3.5", 0x499AA6FCU },
    { "sfiii3-simm3.6", 0x6C13879EU }, { "sfiii3-simm3.7", 0xCF4F8EDEU },
    { "sfiii3-simm4.0", 0x091FD5BAU }, { "sfiii3-simm4.1", 0x0BCA8917U },
    { "sfiii3-simm4.2", 0xA0FD578BU }, { "sfiii3-simm4.3", 0x4BF8C699U },
    { "sfiii3-simm4.4", 0x137B8785U }, { "sfiii3-simm4.5", 0x4FB70671U },
    { "sfiii3-simm4.6", 0x832374A4U }, { "sfiii3-simm4.7", 0x1C88576DU },
    { "sfiii3-simm5.0", 0xC67D9190U }, { "sfiii3-simm5.1", 0x6CB79868U },
    { "sfiii3-simm5.2", 0xDF69930EU }, { "sfiii3-simm5.3", 0x333754E0U },
    { "sfiii3-simm5.4", 0x78F6D417U }, { "sfiii3-simm5.5", 0x8CCAD9B1U },
    { "sfiii3-simm5.6", 0x85DE59E5U }, { "sfiii3-simm5.7", 0xEE7E29B3U },
    { "sfiii3-simm6.0", 0x8DA69042U }, { "sfiii3-simm6.1", 0x1C8C7AC4U },
    { "sfiii3-simm6.2", 0xA671341DU }, { "sfiii3-simm6.3", 0x1A990249U },
    { "sfiii3-simm6.4", 0x20CB39ACU }, { "sfiii3-simm6.5", 0x5F844B2FU },
    { "sfiii3-simm6.6", 0x450E8D28U }, { "sfiii3-simm6.7", 0xCC5F4187U },
};

#define GFX_READ_CHUNK (1024 * 10)

/* Reads the currently-open zip entry (exactly GFX_SIMM_FILE_SIZE bytes,
 * no content hash -- dev path) into `dst`. */
static bool read_gfx_entry(void* zip, Uint8* dst, void* read_buf) {
    if (mz_zip_entry_read_open(zip, false, NULL) != MZ_OK) {
        return false;
    }

    size_t total = 0;
    int32_t read = 0;

    while ((read = mz_zip_entry_read(zip, read_buf, GFX_READ_CHUNK)) > 0) {
        if (total + (size_t)read > GFX_SIMM_FILE_SIZE) {
            mz_zip_entry_close(zip);
            return false;
        }

        SDL_memcpy(dst + total, read_buf, (size_t)read);
        total += (size_t)read;
    }

    mz_zip_entry_close(zip);
    return total == GFX_SIMM_FILE_SIZE;
}

/* Interleaves one even/odd pair straight into `image` at the pair's own
 * offset: slot -> (simm*4 + pair) via slot/2 (see the call site), so `pos`
 * lands exactly where the original simm/pair nested loop would place it.
 * Factored out so a pair can be written as soon as both halves have been
 * read, rather than after all 32 slices are collected (see load_gfx_image's
 * peak-memory note). */
static void interleave_pair_into_image(Uint8* image, unsigned pair_index, const Uint8* even, const Uint8* odd) {
    Uint32 pos = pair_index * 2u * GFX_SIMM_FILE_SIZE;

    for (Uint32 i = 0; i < GFX_SIMM_FILE_SIZE; i++) {
        image[pos + 2 * i + 0] = even[i];
        image[pos + 2 * i + 1] = odd[i];
    }
}

/* Assembles the 64 MiB gfx image from 32 raw 2 MiB slices: SIMM3-6 in
 * order, each SIMM as four pairs (x.0/x.1, x.2/x.3, x.4/x.5, x.6/x.7),
 * even file -> even byte offsets, odd file -> odd byte offsets (doc
 * §5B.3). Returns NULL unless all 32 slices were found -- a partial gfx
 * image would decode to garbage silently, which is worse than refusing.
 *
 * PEAK MEMORY. The image (64 MiB) is allocated up front and each pair is
 * interleaved into it and freed as soon as both halves have been read,
 * rather than holding all 32 x 2 MiB slices (64 MiB) alongside the image
 * until the very end -- that earlier shape peaked at 128 MiB transient on
 * a device with ~445 MiB free. Zip entries for a pair (x.N/x.N+1) are
 * adjacent in every merged sfiii3.zip this loader has been run against
 * (verified against the same romuser.bin build cps3_first_light.c's own
 * CRC table cites), so in practice at most one slice is ever waiting for
 * its partner; `pending` below still handles out-of-order arrival
 * correctly (worst case all 16 partners arrive last), it just would not
 * hit the ~66 MiB bound this note describes in that worst case. */
static Uint8* load_gfx_image(const char* path) {
    void* stream = mz_stream_os_create();

    if (mz_stream_open(stream, path, MZ_OPEN_MODE_READ) != MZ_OK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cps3FirstLight: could not open %s as a file", path);
        mz_stream_os_delete(&stream);
        return NULL;
    }

    void* zip = mz_zip_create();
    int32_t err = mz_zip_open(zip, stream, MZ_OPEN_MODE_READ);

    if (err != MZ_OK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cps3FirstLight: %s did not open as a zip archive (mz error %d)",
                    path, (int)err);
        mz_zip_close(zip);
        mz_zip_delete(&zip);
        mz_stream_os_delete(&stream);
        return NULL;
    }

    Uint8* image = SDL_malloc(GFX_IMAGE_SIZE);

    if (image == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cps3FirstLight: SDL_malloc(%u) failed for the gfx image",
                    (unsigned)GFX_IMAGE_SIZE);
        mz_zip_close(zip);
        mz_zip_delete(&zip);
        mz_stream_os_delete(&stream);
        return NULL;
    }

    err = mz_zip_goto_first_entry(zip);

    void* read_buf = SDL_malloc(GFX_READ_CHUNK);
    Uint8* pending[GFX_SIMM_FILE_COUNT] = { 0 }; /* one temp slice per slot, until its pair partner shows up */
    unsigned found = 0;

    if (read_buf == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cps3FirstLight: SDL_malloc(%u) failed for the zip read buffer",
                    (unsigned)GFX_READ_CHUNK);
    }

    while (err == MZ_OK && read_buf != NULL && found < GFX_SIMM_FILE_COUNT) {
        mz_zip_file* info = NULL;

        if (mz_zip_entry_get_info(zip, &info) == MZ_OK && info != NULL) {
            for (unsigned slot = 0; slot < GFX_SIMM_FILE_COUNT; slot++) {
                const GfxSimmSpec* spec = &gfx_simm_specs[slot];

                if (pending[slot] != NULL || info->crc != spec->crc32 ||
                    info->uncompressed_size != (int64_t)GFX_SIMM_FILE_SIZE) {
                    continue;
                }

                Uint8* data = SDL_malloc(GFX_SIMM_FILE_SIZE);

                if (data != NULL && read_gfx_entry(zip, data, read_buf)) {
                    found++;

                    const unsigned partner = slot ^ 1u; /* x.0<->x.1, x.2<->x.3, ... */

                    if (pending[partner] != NULL) {
                        const Uint8* even = (slot % 2 == 0) ? data : pending[partner];
                        const Uint8* odd = (slot % 2 == 0) ? pending[partner] : data;

                        interleave_pair_into_image(image, slot / 2, even, odd);

                        SDL_free(pending[partner]);
                        pending[partner] = NULL;
                        SDL_free(data);
                    } else {
                        pending[slot] = data;
                    }
                } else {
                    SDL_free(data);
                }

                break;
            }
        }

        err = mz_zip_goto_next_entry(zip);
    }

    SDL_free(read_buf);
    mz_zip_close(zip);
    mz_zip_delete(&zip);
    mz_stream_os_delete(&stream);

    if (found != GFX_SIMM_FILE_COUNT) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Cps3FirstLight: %s has SIMM1 (program) but only %u/%u SIMM3-6 (gfx) "
                    "entries -- probably the flat sfiii3nr1.zip rather than the merged "
                    "sfiii3.zip. First-light demo tile stays unavailable.",
                    path,
                    found,
                    GFX_SIMM_FILE_COUNT);
        SDL_free(image);
        image = NULL;
    }

    /* Any slice still in `pending` here belongs to a pair whose partner
     * never arrived (found < GFX_SIMM_FILE_COUNT) -- free the leftovers. */
    for (unsigned i = 0; i < GFX_SIMM_FILE_COUNT; i++) {
        SDL_free(pending[i]);
    }

    return image;
}

/* ---------------------------------------------------------------------
 * 2. DECODER -- ported from MAME src/mame/capcom/cps3.cpp (BSD-3-Clause,
 * `// license:BSD-3-Clause` at the top of that file), functions
 * cps3_state::process_byte() and cps3_state::do_char_dma(). Only the
 * command-2 (6bpp RLE+dictionary) path is ported -- command 3
 * (do_alt_char_dma, 8bpp) is "SFIII NG Sean's Stage ONLY" (doc §5B.5) and
 * CG 0x060A uses command 2 exclusively (verified against the oracle log,
 * see cps3_first_light_dma_log_check.txt in this task's notes).
 *
 * ONE deliberate deviation from MAME, per doc §5H's "ACTIONABLE": MAME
 * writes every output byte to dest[d^3] because its m_char_ram is a
 * host-endian u32[] container; that swizzle measured +21.5% on the
 * target's Cortex-A9 (13.77 vs 11.34 ns/output byte) for a container this
 * port doesn't have. This decoder writes dest[d] linearly instead --
 * cross-checked bit-exact against `romonly/chardma.py` (an independent
 * Python re-implementation the research session had already verified
 * against 1,524 real DMA records), which also writes linearly. */

static Uint8 process_byte(Uint8 real_byte, Uint8* dest, Uint32 dest_off, Uint32 max_length, Uint8* last_normal_byte,
                           Uint32* out_written) {
    if (real_byte & 0x40) {
        Uint32 rle_length = (Uint32)(real_byte & 0x3f) + 1;
        Uint32 transferred = 0;
        const Uint8 fill = (Uint8)(*last_normal_byte & 0x3f);

        while (rle_length > 0 && transferred < max_length) {
            dest[dest_off + transferred] = fill;
            transferred++;
            rle_length--;
        }

        *out_written = transferred;
    } else {
        dest[dest_off] = real_byte;
        *last_normal_byte = real_byte;
        *out_written = 1;
    }

    return real_byte;
}

/* Decodes one CgList dma[] record: `real_source` is already the SIMM byte
 * offset ((src<<1)-0x400000, doc §5G -- our hardcoded table stores this
 * form directly, see kCg060aRecords below), `dict_addr` is the dict_src
 * record's real_source the same way, `real_length` is the decompressed
 * byte count. Writes exactly `real_length` bytes to `dest` (linear, see
 * above) and returns false if the source ran past the end of `gfx`
 * (malformed input; never observed against the real ROM). */
static bool cps3_do_char_dma(const Uint8* gfx, Uint32 gfx_size, Uint32 real_source, Uint32 dict_addr, Uint8* dest,
                              Uint32 real_length) {
    Uint8 last_normal_byte = 0;
    Uint32 length_remaining = real_length;
    Uint32 dest_off = 0;

    while (length_remaining > 0) {
        if (real_source >= gfx_size) {
            return false;
        }

        Uint8 current_byte = gfx[real_source];
        real_source++;

        if (current_byte & 0x80) {
            current_byte &= 0x7f;

            if (dict_addr + (Uint32)current_byte * 2 + 1 >= gfx_size) {
                return false;
            }

            Uint32 written = 0;
            process_byte(gfx[dict_addr + (Uint32)current_byte * 2 + 0], dest, dest_off, length_remaining,
                         &last_normal_byte, &written);
            dest_off += written;
            length_remaining -= written;

            if (length_remaining == 0) {
                return true;
            }

            process_byte(gfx[dict_addr + (Uint32)current_byte * 2 + 1], dest, dest_off, length_remaining,
                         &last_normal_byte, &written);
            dest_off += written;
            length_remaining -= written;

            if (length_remaining == 0) {
                return true;
            }
        } else {
            Uint32 written = 0;
            process_byte(current_byte, dest, dest_off, length_remaining, &last_normal_byte, &written);
            dest_off += written;
            length_remaining -= written;

            if (length_remaining == 0) {
                return true;
            }
        }
    }

    return true;
}

/* ---------------------------------------------------------------------
 * 3. CG 0x060A -- hardcoded record table, dev-only "smallest first light"
 * (doc §5S 4.2: "no SIMM2 pinning decision"). These are NOT re-derived
 * from the CG directory at runtime -- SIMM2 (the directory) is not loaded
 * anywhere in this tree yet. Instead they are the exact 11 DMA records
 * MAME itself emitted for CG 0x060A / frame 1500, read from the oracle
 * capture (`/Volumes/KimchDrive/3sx-research-logs/cg-dma/run1.dma.log`,
 * lines "F 1500 DMA list=000400 i=3.. i=33.. cmd=2"), which is also this
 * task's pass-criterion source. `src` here is already real_source (the
 * log's own `src=` field, i.e. (dat3<<1)-0x400000 already applied -- see
 * cps3.cpp process_character_dma) so no further shift is needed before
 * indexing the gfx image. `dict_src` is the i=0 cmd=4 record's `src`
 * field. Revision: SIMM2/3-6 are revision-independent (proven above, not
 * merely the doc's SIMM2-only claim), so these ROM-content-derived values
 * need no 990512/990608 adjustment -- unlike a raw SIMM1 code offset.
 *
 * CORRECTION to the task brief: it describes this as "CG 0x060A's 11
 * tiles". That conflates DMA records with tiles -- n_dma=11 is the record
 * count (doc §5G), but records are multi-tile (a record's length is
 * (len_code+1)*16 bytes = up to 16 tiles); summing the 11 lengths below
 * gives 9,984 bytes = 39 tiles of 256 B each. §5S 4.2's own aside ("gives
 * W1-b its first datum... a CPS-3 tile = 16x16, wh=0, size=256") is about
 * per-tile geometry, not a claim that this CG is 11 tiles.
 *
 * ORDERING: this table is sorted by `dst` (the oracle log's own
 * `dst=00xxxxxx` field, i.e. CHARRAM placement), NOT by DMA-record index
 * `i=`. The two do not agree -- e.g. i=12's dst (0028d800) precedes i=15's
 * dst (0028d300) in record order despite landing later in CHARRAM -- so
 * decoding in `i=` order and laying tiles out by `flat_off += real_length`
 * (as TryLoad does below) would NOT reproduce CHARRAM tile order. Sorting
 * here by `dst` instead makes g_tiles[k] the tile CHARRAM would hold at
 * offset dst_base + k*256, so Cps3FirstLight_NextTile()'s round-robin
 * sweep visits CG 0x060A's own tiles in their real order. This is still
 * only ONE (arbitrarily-selected-by-draw-order) chip's worth of tile per
 * frame, round-robining across all 39 -- not "CG 0x060A rendered in full"
 * on any single frame. */
typedef struct Cg060aRecord {
    Uint32 real_source;
    Uint32 real_length;
} Cg060aRecord;

#define CG_060A_DICT_REAL_SOURCE 0x025a0000U
#define CG_060A_RECORD_COUNT 11u

static const Cg060aRecord kCg060aRecords[CG_060A_RECORD_COUNT] = {
    /* dst=0028d000 (i=3)  */ { 0x025a7cecU, 512 },
    /* dst=0028d200 (i=6)  */ { 0x025a28dcU, 256 },
    /* dst=0028d300 (i=15) */ { 0x025a7ed8U, 256 },
    /* dst=0028d400 (i=9)  */ { 0x025a7a08U, 1024 },
    /* dst=0028d800 (i=12) */ { 0x025a7d06U, 512 },
    /* dst=0028da00 (i=24) */ { 0x025a7daaU, 512 },
    /* dst=0028dc00 (i=21) */ { 0x025a7bdaU, 1024 },
    /* dst=0028e000 (i=18) */ { 0x025a7320U, 4096 },
    /* dst=0028f000 (i=27) */ { 0x025a2912U, 256 },
    /* dst=0028f200 (i=33) */ { 0x025a7e46U, 512 },
    /* dst=0028f400 (i=30) */ { 0x025a7b04U, 1024 },
};

/* Palette bank base in gfx address space (doc §5I.1: located by scanning
 * for "bit15 density < 2%", 128-byte granules = 64 x u16 BE). Slot 8 IS
 * determinable, and is CG 0x060A's own slot: the oracle capture's sprite
 * log (/Volumes/KimchDrive/3sx-research-logs/cg-dma/run1.spr.log, "F 1500
 * HDR blk=9 gs=2 len=44 start=0b780 gbpp=1 gpal=008") shows this exact
 * block -- its dst range 0028d000-0028f400 in run1.dma.log matches CG
 * 0x060A's 11 DMA records' dst fields one-for-one -- drawing with
 * colour-RAM palette 8, supplied by the block header (doc §5G) rather
 * than by any per-SPR-entry field (those all carry pal=000 here). What
 * remains genuinely unknown is which SIMM bank granule palette DMA had
 * loaded into palette slot 8 at capture time; that provenance question is
 * NOT resolved by this offset. Pen index 0 is transparent on CPS-3
 * regardless (doc §5B.4), so the demo tile is visible either way, but
 * colour fidelity beyond "the right slot" is not tested. */
#define CG_060A_PALETTE_BANK_REAL_SOURCE 0x02F00000U
#define CG_060A_PALETTE_SLOT 8u
#define CG_060A_PALETTE_ENTRIES 64u
#define CG_060A_PALETTE_SLOT_BYTES (CG_060A_PALETTE_ENTRIES * 2u)

static Uint8 g_tiles[CPS3_FIRST_LIGHT_TILE_COUNT][CPS3_FIRST_LIGHT_TILE_BYTES];
static Uint16 g_palette[CG_060A_PALETTE_ENTRIES];
static bool g_ready = false;
static Uint32 g_next_tile = 0;

static Uint16 read_be16(const Uint8* p) {
    return (Uint16)(((Uint16)p[0] << 8) | (Uint16)p[1]);
}

void Cps3FirstLight_TryLoad(const char* zip_path) {
    if (g_ready || zip_path == NULL) {
        return;
    }

    Uint8* gfx = load_gfx_image(zip_path);

    if (gfx == NULL) {
        return;
    }

    /* Decode all 11 records up front into one flat 39-tile buffer. Real
     * hardware decodes on cache miss into a rolling CHARRAM window (doc
     * §5G); this demo has no cache to miss into (task constraint: "no
     * cache integration"), so it just decodes the whole CG once. */
    Uint8 flat[CPS3_FIRST_LIGHT_TILE_COUNT * CPS3_FIRST_LIGHT_TILE_BYTES];
    Uint32 flat_off = 0;
    bool ok = true;

    for (Uint32 i = 0; i < CG_060A_RECORD_COUNT && ok; i++) {
        const Cg060aRecord* rec = &kCg060aRecords[i];
        ok = cps3_do_char_dma(gfx, GFX_IMAGE_SIZE, rec->real_source, CG_060A_DICT_REAL_SOURCE, flat + flat_off,
                               rec->real_length);
        flat_off += rec->real_length;
    }

    if (!ok || flat_off != CPS3_FIRST_LIGHT_TILE_COUNT * CPS3_FIRST_LIGHT_TILE_BYTES) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Cps3FirstLight: CG 0x060A decode failed or produced %u bytes, expected %u",
                    flat_off,
                    (unsigned)(CPS3_FIRST_LIGHT_TILE_COUNT * CPS3_FIRST_LIGHT_TILE_BYTES));
        SDL_free(gfx);
        return;
    }

    SDL_memcpy(g_tiles, flat, sizeof(g_tiles));

    const Uint32 palette_offset = CG_060A_PALETTE_BANK_REAL_SOURCE + CG_060A_PALETTE_SLOT * CG_060A_PALETTE_SLOT_BYTES;

    if (palette_offset + CG_060A_PALETTE_ENTRIES * 2 <= GFX_IMAGE_SIZE) {
        for (Uint32 i = 0; i < CG_060A_PALETTE_ENTRIES; i++) {
            g_palette[i] = read_be16(gfx + palette_offset + i * 2);
        }
    } else {
        SDL_memset(g_palette, 0, sizeof(g_palette));
    }

    /* Tiles and palette are both copied out above; the 64 MiB image itself
     * is scratch (unlike SIMM1's decrypted image, nothing else in the
     * process reads it), so it does not need to stay resident. */
    SDL_free(gfx);

    g_ready = true;
    g_next_tile = 0;

    SDL_Log("Cps3FirstLight: CG 0x060A decoded -- %u tiles (%u bytes) from %s, ready for PS2 CG 0x%04X",
            CPS3_FIRST_LIGHT_TILE_COUNT,
            (unsigned)sizeof(g_tiles),
            zip_path,
            CPS3_FIRST_LIGHT_PS2_CG);
}

bool Cps3FirstLight_Ready(void) {
    return g_ready;
}

const Uint16* Cps3FirstLight_PaletteRaw(void) {
    return g_palette;
}

/* PPGFile.c's ppgRenewDotDataSeqs() case 0x100 (the 16x16-8bpp chip path
 * njReLoadTexturePartNumG feeds into) reads
 * `srcRam8[dctex_linear[j + (i << 5)]]` for a destination pixel at row i,
 * col j -- i.e. it UN-TWIDDLES: the source buffer must already hold each
 * pixel at its Dreamcast-twiddled (Morton) address, not at its raster
 * address. `dctex_linear` is built by ppgMakeConvTableTexDC()
 * (src/sf33rd/Source/Common/PPGFile.c) before any frame renders (main.c's
 * sf3_init() -> distributeScratchPadAddress() + ppgMakeConvTableTexDC(),
 * both ahead of the game loop that first calls Cps3FirstLight_NextTile()),
 * so it is guaranteed initialised here.
 *
 * The decoder above deliberately emits ROW-MAJOR bytes (see the "ONE
 * deliberate deviation from MAME" note): that is correct CPS-3 pixel
 * data, but it is not yet in the form this consumer expects. So NextTile
 * re-twiddles at hand-off time: `dctex_linear[x + (y << 5)]` is, for
 * x,y in [0,16), exactly the Morton code of (x,y) (verified: it
 * bit-interleaves x's 4 bits into the odd bit positions and y's 4 bits
 * into the even ones, landing in [0,255] -- a bijection onto the 256-byte
 * tile), so writing `out[dctex_linear[x + (y << 5)]] = tile[y*16 + x]`
 * places every row-major pixel at the address the consumer's un-twiddle
 * will read it back from. Without this, the tile renders with its 16x16
 * pixels scrambled into their Z-order permutation.
 *
 * Factored out of Cps3FirstLight_NextTile() so the test harness
 * (src/test/test_cps3_chardma.c) can exercise this exact math without a
 * full ROM/tile load -- see Cps3FirstLight_TestTwiddleTile(). */
static void cps3_first_light_twiddle_tile(const Uint8 tile[CPS3_FIRST_LIGHT_TILE_BYTES],
                                           Uint8 out[CPS3_FIRST_LIGHT_TILE_BYTES]) {
    for (Uint32 y = 0; y < 16; y++) {
        for (Uint32 x = 0; x < 16; x++) {
            out[dctex_linear[x + (y << 5)]] = tile[y * 16 + x];
        }
    }
}

void Cps3FirstLight_NextTile(Uint8 out[CPS3_FIRST_LIGHT_TILE_BYTES]) {
    if (!g_ready) {
        SDL_memset(out, 0, CPS3_FIRST_LIGHT_TILE_BYTES);
        return;
    }

    cps3_first_light_twiddle_tile(g_tiles[g_next_tile], out);
    g_next_tile = (g_next_tile + 1) % CPS3_FIRST_LIGHT_TILE_COUNT;
}

#if defined(ENABLE_NETPLAY_TESTS)
/* Test seam for src/test/test_cps3_chardma.c -- see cps3_first_light.h. */
bool Cps3FirstLight_TestDecodeRecord(const Uint8* gfx, Uint32 gfx_size, Uint32 real_source, Uint32 dict_addr,
                                      Uint8* dest, Uint32 real_length) {
    return cps3_do_char_dma(gfx, gfx_size, real_source, dict_addr, dest, real_length);
}

/* Test seam for src/test/test_cps3_chardma.c -- see cps3_first_light.h.
 * Exposes cps3_first_light_twiddle_tile() so the harness can round-trip a
 * synthetic tile through it and PPGFile.c's own dctex_linear consumer
 * math, without a ROM load. `dctex_linear` must already point at a
 * ppgMakeConvTableTexDC()-initialised buffer -- the harness sets that up
 * itself, since main()'s normal sf3_init() path does not run before a
 * --test-cps3-chardma dispatch. */
void Cps3FirstLight_TestTwiddleTile(const Uint8 tile[CPS3_FIRST_LIGHT_TILE_BYTES],
                                     Uint8 out[CPS3_FIRST_LIGHT_TILE_BYTES]) {
    cps3_first_light_twiddle_tile(tile, out);
}
#endif
