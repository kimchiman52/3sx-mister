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

/* REMOVED: a CPS3_FIRST_LIGHT_PAL_ROW borrowing ColorRAM[510] used to exist
 * here, to point the hijacked chip's palette at the ROM-sourced colours
 * Cps3FirstLight_PaletteRaw() below decodes. It caused a deterministic
 * SIGSEGV -- see the palette-resolution comment in mlt_obj_trans_ext()
 * (rendering/mtrans.c) for the full mechanism. In short: whether a
 * ColorRAM row is free is the wrong question, because the chip that draws
 * CPS3_FIRST_LIGHT_PS2_CG never resolves its palette against ColorRAM at
 * all -- it resolves against a 16-entry directory (col3rd_w.palDC) that
 * row 510 cannot address. The hijack now draws with Alex's own real
 * palette instead (wrong colours, on purpose); Cps3FirstLight_PaletteRaw()
 * is kept for a future patch that routes the ROM palette through the
 * directory this CG's texture group actually uses. */

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

/* Writes ONE of the 39 decoded tiles into `out`, selected by
 * `chip_ordinal % CPS3_FIRST_LIGHT_TILE_COUNT` (in CHARRAM/`dst` order, see
 * kCg060aRecords in cps3_first_light.c) -- a pure function of its argument,
 * with NO carried state between calls. `chip_ordinal` is the caller's own
 * position counter (mtrans.c's `cps3_chip_ordinal`): the same physical
 * chip must always pass the same ordinal, every time it is drawn, so it
 * always gets the same tile.
 *
 * WHY THIS REPLACED THE OLD ROUND-ROBIN. The previous Cps3FirstLight_
 * NextTile() advanced a single global counter on every call, from every
 * chip, of every draw, of every frame this CG appeared in -- so which tile
 * a given physical chip got depended on how many OTHER chips had drawn
 * since boot, not on which chip it was. Two chips 39 (or 78, or ...) calls
 * apart got the same tile by coincidence; the same chip got a DIFFERENT
 * tile on its next redraw. That is the "known cosmetic glitches ...
 * arbitrary" flicker docs/cps3-rom-graphics.md records. Keying off a
 * caller-supplied per-chip ordinal instead makes the assignment
 * deterministic and stable per chip FOR THIS ONE FUNCTION, given that
 * trans_table traversal order is fixed per pose. That premise was
 * live-captured, not assumed: instrumenting mlt_obj_trans_ext() to log
 * each trsptr entry's (code, x, y, wh) while drawing this CG showed an
 * IDENTICAL 15-entry sequence on every redraw of the same pose (Alex,
 * training mode, `--test-scene-preset training-frame-data
 * --test-p1-character 1 --test-balance arcade`) -- trans_table is static
 * per-cg_number asset data, so the same trsptr index always yields the
 * same TileMapEntry, and cps3_chip_ordinal (mtrans.c) always counts up to
 * it the same way.
 *
 * THIS DOES NOT MAKE THE FLICKER GONE, ONLY THIS SOURCE OF IT. The x16
 * texture cache the hijack site writes into (get_mltbuf16_ext_2 ->
 * mts_hash_lookup(mt->hash16, code, palt, mc), mtrans.c / mts_hash.h) is
 * keyed by (code, palt) with NO cg_number component, and `code` here is
 * (texture-group-index << 16) | trsptr->code (PatternCode, structs.h), not
 * a value unique to this CG. CG 0x062A shares texture group 2 with CGs
 * 0x0623, 0x0624 and 0x062B, and this CG's own wh=2 texture codes 54
 * (chip @1) and 55 (chip @11) are the exact same codes those other CGs'
 * chips reference too (0x0623@1/0x0624@1/0x062B@1 = 54, 0x0623@13/
 * 0x062B@12 = 55 -- confirmed against SF33RD.AFS's trans_table for all
 * four CGs). Whichever CG's chip populates that cache slot first wins it
 * for every CG that shares it, until eviction; deterministic ordinal
 * assignment within 0x062A's own draw does nothing about that. See
 * docs/cps3-rom-graphics.md for the fuller accounting of what this
 * scaffolding does and does not fix.
 *
 * WHAT THIS DOES NOT ESTABLISH. `chip_ordinal` counts this draw's own
 * trans_table position (0, 1, 2, ...) -- it is NOT the arcade CgList's
 * per-CG record index `k` that doc §5G's invariant is stated in terms of.
 * Confirmed by directly reconstructing CG 0x060A's ROM CgList this task
 * (SIMM byte offsets/lengths/x_off/y_off/tile-shape for all 11 records,
 * cross-checked byte-for-byte against every worked-example value doc §5G
 * quotes for k=0, k=5, k=10): the arcade side has 11 records covering 39
 * tiles; the PS2 trans_table for CG 0x062A has 15 TileMapEntry (5 wh=2 +
 * 10 wh=4, live-captured). That the two counts (15 vs. 11 or 39) don't
 * divide evenly is not, by itself, meaningful -- lots of unrelated counts
 * don't divide each other. The stronger evidence is geometric: walking
 * this CG's own trans_table and accumulating (x, y) the way
 * mlt_obj_trans_ext() does (`x += trsptr->x`, `y += trsptr->y` under this
 * pose's flip attr) puts every chip's position on a 16 px grid on the x
 * axis (x in {-32,-16,0,16,32,48}) except three: chips @0 (x=51), @1
 * (x=55) and @2 (x=36, also off the loosely-32-px-spaced y values this
 * CG's other chips cluster around) -- i.e. the trimmed-looking chips are
 * the same three whose PS2 draw box also isn't a round multiple of 16 (one
 * wh=4 chip's box is 24 px wide). NOTE: `dw`/`dh` here are the DRAW BOX
 * seqsStoreChip() paints, not the underlying texture -- every chip's
 * source texture is still a full 16x16 (wh=2) or 32x32 (wh=4) tile
 * regardless of dw/dh, so a non-multiple-of-16 dw is evidence of edge
 * cropping in the PS2 asset pipeline (consistent with "same tiles, margins
 * trimmed"), not proof by itself that chip boundaries were re-cut from the
 * arcade tile grid. Net: `chip_ordinal % 39` is a stable BUT UNVERIFIED
 * assignment of tiles to chips -- it removes the round-robin flicker
 * covered above, not necessarily the wrong-tile-in-the-right-place case.
 * Proving per-chip correctness needs an offline content-match: this IS
 * buildable from what already exists here -- the 39 decoded tiles are in
 * `g_tiles` (cps3_first_light.c) and the PS2 chips' own pixels come from
 * the ordinary `lz_ext_p6_fx()` path (mtrans.c) -- nobody has built that
 * audit yet. A human still needs to confirm the picture by eye either way.
 *
 * NOT a copy: the decoder's row-major output is re-twiddled into the
 * Dreamcast/PowerVR Morton order njReLoadTexturePartNumG's consumer
 * (ppgRenewDotDataSeqs case 0x100, via dctex_linear) expects -- see the
 * comment on this function's definition. Only valid when
 * Cps3FirstLight_Ready(). */
void Cps3FirstLight_TileForChip(uint32_t chip_ordinal, uint8_t out[CPS3_FIRST_LIGHT_TILE_BYTES]);

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
 * row-major-to-Dreamcast-twiddled re-mapping Cps3FirstLight_TileForChip()
 * applies before handing a tile to njReLoadTexturePartNumG, so the
 * harness can round-trip a synthetic tile through it and PPGFile.c's own
 * `dctex_linear`-based un-twiddle and confirm the two invert each other.
 * Requires `dctex_linear` (sf33rd/Source/Common/PPGFile.h) to already
 * point at a buffer initialised by ppgMakeConvTableTexDC() -- the caller
 * sets that up. Test builds only. */
void Cps3FirstLight_TestTwiddleTile(const uint8_t tile[CPS3_FIRST_LIGHT_TILE_BYTES],
                                     uint8_t out[CPS3_FIRST_LIGHT_TILE_BYTES]);

/* Test seam for src/test/test_cps3_chardma.c: injects `tiles` as the
 * module's decoded-tile set and marks Cps3FirstLight_Ready() true,
 * without a ROM/zip load -- so the harness can exercise
 * Cps3FirstLight_TileForChip()'s statelessness and its
 * `chip_ordinal % CPS3_FIRST_LIGHT_TILE_COUNT` periodicity using tiles it
 * already decoded via Cps3FirstLight_TestDecodeRecord(). Test builds
 * only. */
void Cps3FirstLight_TestSetTiles(const uint8_t tiles[CPS3_FIRST_LIGHT_TILE_COUNT][CPS3_FIRST_LIGHT_TILE_BYTES]);
#endif

#endif
