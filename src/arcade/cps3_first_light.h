#ifndef ARCADE_CPS3_FIRST_LIGHT_H
#define ARCADE_CPS3_FIRST_LIGHT_H

/* DEV/TEST ONLY -- "first light" proof-of-pipeline scaffolding.
 *
 * Renders exactly ONE arcade sprite (CG 0x060A, the doc's own worked
 * example) through the port's EXISTING njReLoadTexturePartNumG ->
 * ppgRenewDotDataSeqs upload path, sourced from the user's own CPS3 SIMM3-6
 * gfx ROM instead of the shipped PS2 AFS. It exists to prove the loader,
 * the ported CPS-3 RLE+dictionary decoder, and the upload path compose --
 * nothing here is a general CG->pixels system. See
 * docs/research-arcade-cg-data-accuracy.md and
 * ~/Desktop/3s-arm/docs/3sx-rom-only-research.md §5S 4.2.
 *
 * Gated the same way as the rest of the CPS3 ROM path: only reachable when
 * $THIRDSARM_CPS3_ZIP resolved a ROM (see arcade_char_data.c). Never runs
 * on a normal install. No SHA-256 pinning here (unlike rom_load.c's SIMM1
 * path) -- this is explicitly a dev path, not the shipping loader. */

#include <stdbool.h>
#include <stdint.h>

/* PS2 CG number the demo hijacks. 0x060A is arcade Alex's CG; Alex's block
 * delta is +0x20 (cg_maps[CHAR_ALEX].default_delta, arcade_char_data.c,
 * matching §5G's "own delta" table), so remap_cg_number would send it to
 * 0x062A -- this constant IS that PS2 number, hardcoded rather than
 * computed, since the demo hooks a single already-working PS2 draw call
 * rather than reimplementing the remap. */
#define CPS3_FIRST_LIGHT_PS2_CG 0x062Au

/* One CPS-3 tile: 16x16 px, 8bpp packed (6bpp pixel data, one byte per
 * pixel), linear/row-major order. 256 bytes (doc §5B.4/§5G). */
#define CPS3_FIRST_LIGHT_TILE_BYTES 256u

/* CG 0x060A's 11 DMA records decode to this many 256-byte tiles
 * (2+1+4+2+1+16+4+2+1+4+2, doc §5G's per-record tile counts "1x2" .. "2x1").
 * NOT 11 -- that count is DMA records / metasprites, not physical tiles;
 * see the citation in cps3_first_light.c for where this correction was
 * made against the original task brief. */
#define CPS3_FIRST_LIGHT_TILE_COUNT 39u

/* ColorRAM row the demo's arcade-sourced palette lands in. 510 is NOT a
 * free/unused row -- it is deliberately overwritten by the boot-time PS2
 * asset load and its content is not incidental garbage.
 * color3rd.c's color_file[20] = { .data = 32, .type = 2, .apfn = 0x9 };
 * init_trans_color_ram() case 2 loads AFS file 0x9 (61,440 B = 480 rows)
 * starting at row `data` = 32, contiguously through row 32+480-1 = 511 --
 * i.e. it writes every row in [32, 511], 510 included. Only row 511 gets a
 * hand-written blanking palette immediately after (the `if (data == 32)`
 * branch, color3rd.c), which is itself evidence the load reaches all the
 * way to 511 and therefore also covers 510.
 *
 * This load is boot-once (Init_Task_1st() -> Init_load_on_memory_data() ->
 * load_any_color(0x14, 2), no ldreq_tbl row ever re-requests it), so this
 * demo's write at first-use permanently wins for the rest of the process;
 * nothing clobbers it back. No drawing consumer of row 510 was found
 * (mcs_sel_tbl uses 504/508; dmwk_kage.current_colcd is 0x1FF), but
 * absence could NOT be proven -- aboutspr.c computes
 * `current_colcd + conn[i].col` at runtime, so a reader cannot be ruled
 * out by grep alone. Flagged, not hidden: this is dev/test scaffolding
 * riding on a boot-once side effect, not a genuinely free row. */
#define CPS3_FIRST_LIGHT_PAL_ROW 510

/* Attempts to load SIMM3-6 (64 MiB gfx, pair-interleaved, doc §5B.3) from
 * the SAME zip $THIRDSARM_CPS3_ZIP already resolved for SIMM1/2, decode CG
 * 0x060A's 11 DMA records with the ported cps3.cpp decoder, and stage its
 * ROM-native palette. `zip_path` is the exact string ArcadeCharData_Init
 * read $THIRDSARM_CPS3_ZIP as. Safe to call when SIMM3-6 are absent (e.g.
 * the flat sfiii3nr1.zip, which carries only SIMM1) -- logs a line and
 * leaves Cps3FirstLight_Ready() false; nothing else in the ROM path is
 * affected either way. */
void Cps3FirstLight_TryLoad(const char* zip_path);

/* True once TryLoad decoded CG 0x060A successfully. */
bool Cps3FirstLight_Ready(void);

/* The 64-entry BGR555 palette read verbatim from ROM gfx 0x02F00000,
 * palette slot 8 -- CG 0x060A's own slot, confirmed against the oracle
 * capture's sprite log (gpal=008 for this CG's HDR block; see the
 * CG_060A_PALETTE_SLOT comment in cps3_first_light.c), not a placeholder.
 * Index 0 included. Caller applies the index-based transparency rule
 * (doc §5O/§5I.1: dst[0]=0, dst[i]=src[i]|0x8000 for i=1..63) at ColorRAM
 * write time; this function returns raw ROM bytes, no bit is set here.
 * Only valid when Cps3FirstLight_Ready(). */
const uint16_t* Cps3FirstLight_PaletteRaw(void);

/* Writes the next 256-byte tile out of the 39-tile decode into `out`,
 * round-robining across CPS3_FIRST_LIGHT_TILE_COUNT (in CHARRAM/`dst`
 * order, see kCg060aRecords in cps3_first_light.c) so repeated draws (one
 * chip per lz_ext_p6_fx call the demo intercepts) sweep CG 0x060A's own
 * tiles in their real order instead of freezing on tile 0. NOT a copy:
 * the decoder's row-major output is re-twiddled into the Dreamcast/
 * PowerVR Morton order njReLoadTexturePartNumG's consumer
 * (ppgRenewDotDataSeqs case 0x100, via dctex_linear) expects -- see the
 * comment on this function's definition. Only valid when
 * Cps3FirstLight_Ready(). */
void Cps3FirstLight_NextTile(uint8_t out[CPS3_FIRST_LIGHT_TILE_BYTES]);

#if defined(ENABLE_NETPLAY_TESTS)
/* Test seam for src/test/test_cps3_chardma.c: exposes the ported
 * MAME cps3.cpp decoder (process_byte/do_char_dma, command-2 6bpp
 * RLE+dictionary only) directly, so the harness can decode a
 * self-contained real-ROM record and byte-compare it against a golden
 * output computed offline by the independent Python reference
 * (romonly/chardma.py). Returns false on a malformed/out-of-range
 * source (mirrors cps3_do_char_dma's own contract). Test builds only. */
bool Cps3FirstLight_TestDecodeRecord(const uint8_t* gfx, uint32_t gfx_size, uint32_t real_source,
                                      uint32_t dict_addr, uint8_t* dest, uint32_t real_length);

/* Test seam for src/test/test_cps3_chardma.c: exposes the exact
 * row-major-to-Dreamcast-twiddled re-mapping Cps3FirstLight_NextTile()
 * applies before handing a tile to njReLoadTexturePartNumG, so the
 * harness can round-trip a synthetic tile through it and PPGFile.c's own
 * `dctex_linear`-based un-twiddle and confirm the two invert each other.
 * Requires `dctex_linear` (sf33rd/Source/Common/PPGFile.h) to already
 * point at a buffer initialised by ppgMakeConvTableTexDC() -- the caller
 * sets that up. Test builds only. */
void Cps3FirstLight_TestTwiddleTile(const uint8_t tile[CPS3_FIRST_LIGHT_TILE_BYTES],
                                     uint8_t out[CPS3_FIRST_LIGHT_TILE_BYTES]);
#endif

#endif
