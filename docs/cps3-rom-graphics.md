# CPS-3 ROM graphics — "first light" dev path

Dev-only scaffolding that decodes one sprite's tiles from a real CPS-3 arcade ROM
and renders them through the port's existing PS2 texture-upload path. It exists to
prove the pieces compose; it is not a shipping path and does nothing unless you
opt in with an environment variable.

Background and the full decode research live outside this repo in
`3sx-rom-only-research.md` (sections 5B.3, 5B.5, 5G, 5H, 5M, 5O, 5S 4.2).

## Running it

```
THIRDSARM_CPS3_ZIP=/path/to/sfiii3.zip <binary>
```

The knob is the pre-existing dev hook in `arcade_char_data.c`; first light piggybacks
on its success branch. With it unset, nothing loads and every hook is inert. On a
successful load you get:

```
Cps3FirstLight: CG 0x060A decoded -- 39 tiles (9984 bytes)
```

Alex's idle animation then draws 5 of its 15 chips (the wh=2 ones; CG 0x062A has no
wh=1 chips at all) from arcade tiles instead of PS2 ones -- the other 10 (wh=4, 89% of
this CG's own texture bytes) stay PS2 unconditionally; see "What this round actually
achieved" below for why that is not close to a correct picture. Decoder correctness is
covered by `--test-cps3-chardma`.

## Things that are not obvious from the code

**The upload path wants Dreamcast-twiddled tiles, not row-major.**
`ppgRenewDotDataSeqs()` reads its source through `dctex_linear[]` — a Morton table
built by `ppgMakeConvTableTexDC()` — and un-twiddles as it writes. The normal
producer (`lz_ext_p6_fx`) emits twiddled data, so anything substituted for it must be
twiddled too. The CPS-3 decoder emits row-major, so it is re-twiddled before upload.
Skip that step and every byte-level check still passes while the picture comes out in
Z-order. This cost a review cycle; it is the single easiest way to get this wrong.

**Omitting MAME's `dst[d ^ 3]` swizzle is deliberate and correct.**
MAME's `cps3_tiles16x16_layout` xoffset `{3*8,2*8,1*8,0*8,…}` is exactly that swizzle
inverted, so dropping both sides leaves unswizzled byte `d` = logical pixel `d`. It is
also cheaper on Cortex-A9 (the swizzle costs ~21.5% there and is free on x86/ARM64).

**Palette entries carry alpha in bit 15, and the rule is by INDEX not by value.**
*(Currently dormant: the palette hijack was removed after the crash below, so nothing
applies this rule today. It is recorded because `Cps3FirstLight_PaletteRaw()` still
decodes ROM palettes and any future patch that uploads them must get this right.)*
`dst[0] = 0x0000; dst[i] = src[i] | 0x8000` for `i = 1..63`. CPS-3 transparency is pen
*index* 0. A value-based "set bit 15 on non-zero entries" rule looks equivalent and is
not: the ROM bank holds thousands of index-≥1 entries whose value is `0x0000` (opaque
black), which such a rule would turn transparent. Applied at `ColorRAM` write time,
because `lz_ext_p6_cx()` reads `ColorRAM` directly.

**`ColorRAM[510]` is borrowed, not free** — kept here because it is a trap for anyone
who tries the same trick. `color_file[20]` (`type=2`, AFS file 0x9, 480 rows) bulk-loads
rows 32..511 at boot, so 510 holds real data; a `grep` for `ColorRAM[` cannot see this
because the index is data-driven. That turned out not to be why the hijack crashed (see
below) — but "I grepped and found nothing" is weak evidence against table-driven code.

**SIMM2-6 are revision-independent; only SIMM1 is not.** Verified against MAME's
`ROM_START(sfiii3n)` vs `ROM_START(sfiii3nr1)`: 32 files with identical CRC32/SHA1 and
no `// sldh` markers, which appear only on SIMM1 lines. Nothing here needs the
990512/990608 adjustment. (`rom_load.c` pins **990512 / nr1**; `sfiii3` is 990608, and
the two are related by `nr1 = e - 0x14C` — for SIMM1 only.)

## Seen on screen — 2026-09-02

**CORRECTION (2026-09-02, later the same day).** The paragraphs below were written
after watching only 5 of this CG's 15 chips (the wh=2 ones) carry arcade pixels; the
other 10 (wh=4, 89% of this CG's own texture bytes) were PS2 the entire time the human
was looking at the screen. "Coherent, recognisable arcade sprite geometry" describes
what was seen, not what was actually decoded pixels -- by texture-byte share, what was
on screen was overwhelmingly still the PS2 asset. The palette claim below was judged
against that same ~11%-arcade picture, on five chips, three of which (§ "Round-robin
and wh=1" below) are only half-drawn (`dw=8`). It is **downgraded to untested**: the
observation it was based on does not support it. The observation itself (loader,
decoder, twiddle and upload path compose end to end without crashing or scrambling
pixels, on the fraction of the sprite this round actually hijacks) stands and is kept
below, unedited except for this note.

**First light achieved** (on 5/15 chips; not the whole sprite -- see correction above).
Alex's idle animation in training mode draws coherent, recognisable arcade sprite
geometry decoded from the CPS-3 ROM, observed by a human. The loader, RLE+dictionary
decoder, twiddle and upload path all compose correctly on real hardware data.

**>>> Colours came out RIGHT, using the port's OWN PS2 palette. <<<** *(Untested beyond
this: see the correction above -- this was judged on five chips, three of them
half-drawn, out of fifteen.)* The hijack passes `palo` -- Alex's real colcd -- and
indexes the ROM-decoded pixels through it. It looked correct on the LP costume. That
was read as evidence the arcade and PS2 palette-*index* assignments agree for this CG,
which would follow from the 1:1 CG correspondence the research established, and as a
lead that sourcing arcade palettes for character sprites may be unnecessary work. The
evidence that inference was drawn from is too small and too unrepresentative (a minority
of one CG's chips) to carry it -- treat the palette-index-agreement idea as an
unverified hypothesis, not a lead this project should act on.

**Known cosmetic glitches, both expected:** a handful of small areas flicker during the
animation, because (a) only wh=2 (256 B) chips are hijacked, so wh=1 (64 B) chips still
draw PS2 pixels alongside arcade ones, and (b) `Cps3FirstLight_NextTile` hands the 39
tiles out round-robin, so the tile-to-chip correspondence is arbitrary. Neither is a
decode defect. **(a) as originally written is wrong for this CG specifically** -- CG
0x062A's own 15-chip trans_table has ZERO wh=1 entries (5 wh=2 + 10 wh=4; live-captured,
see below), so wh=1 chips could not have been the flicker's cause here. Whatever was
observed came from (b) and/or the cross-CG cache leak this round's own P-1.1 finding
describes (below) -- not from wh=1.

## Round-robin and wh=1 -- 2026-09-02

**(b) fixed for THIS draw's own tile-to-chip assignment -- not "flicker gone."**
`Cps3FirstLight_NextTile()` (a self-advancing global counter) is gone; the hijack site in
`mlt_obj_trans_ext()` (`rendering/mtrans.c`) now keys `Cps3FirstLight_TileForChip(chip_ordinal,
...)` off `cps3_chip_ordinal`, a counter local to that draw that advances once per
`TileMapEntry` in trans_table-visit order. Live capture (this CG's own trans_table,
`--test-scene-preset training-frame-data --test-p1-character 1 --test-balance arcade`)
confirmed the same 15-entry `(code, x, y, wh)` sequence repeats byte-identically on every
redraw of the same pose, so *which decoded tile a given chip is offered* is now
deterministic. **This is not the same claim as "flicker is eliminated."** The x16 texture
cache the hijack writes into (`get_mltbuf16_ext_2` -> `mts_hash_lookup(mt->hash16, code,
palt, mc)`, `mtrans.c` / `mts_hash.h`) is keyed by `(code, palt)` with **no `cg_number`
component** -- `code` here is `(texture-group-index << 16) | trsptr->code` (`PatternCode`,
`structs.h`), not unique to CG 0x062A. CG 0x062A shares texture group 2
(`obj_group_table[]`, `chren3rd.c`) with CGs 0x0623, 0x0624 and 0x062B, and this CG's own
wh=2 texture codes 54 (its chip @1) and 55 (its chip @11) are the exact same codes those
other CGs' chips reference (0x0623@1 / 0x0624@1 / 0x062B@1 = code 54; 0x0623@13 /
0x062B@12 = code 55 -- confirmed directly against `SF33RD.AFS`'s `trans_table` for all
four CGs). Whichever CG's chip populates that cache slot first wins it for every CG that
shares it, until eviction: if another CG uploads code 54/55 first, 0x062A's chip shows PS2
pixels; if 0x062A uploads first, our decoded tiles leak into those other sprites. This is a
real limitation of the scaffolding, not a hypothetical -- and this round did not attempt to
contain it (that would mean changing the cache key, which is shipping-path surgery and out
of scope for dev scaffolding).

**Not re-established: that the tile each chip now lands on is the geometrically correct
one.** CG 0x060A's ROM CgList was independently reconstructed this round (all 11 records'
SIMM offsets, lengths, `x_off`/`y_off`, tile shape -- byte-for-byte against every
worked-example value §5G quotes for k=0/5/10), and does not correlate cleanly against the
live-captured PS2 chip layout: the arcade side is 11 records/39 tiles, this CG's PS2
trans_table is 15 chips (5 wh=2 + 10 wh=4). That 15 does not divide evenly into 11 or 39
is not by itself meaningful (lots of unrelated numbers don't divide each other) -- the
actual evidence is geometric: walking this CG's own 15-chip trans_table and accumulating
(x, y) the way `mlt_obj_trans_ext()` does puts every chip on a 16 px grid on the x axis
except three (chips @0, @1, @2), and those same three chips are the ones whose PS2 draw
box is also not a round multiple of 16 (e.g. one wh=4 chip's box is 24 px wide) -- `dw`/
`dh` are the DRAW BOX `seqsStoreChip()` paints, not the source texture (every chip's
texture is still a full 16x16 or 32x32 tile regardless of dw/dh), so this reads as the PS2
asset pipeline cropping transparent margins off the same underlying tiles, not proof the
tile grid itself was re-cut. Proving per-chip correctness needs an offline content-match
audit (the 39 decoded tiles are in `g_tiles`, PS2 chip pixels come from the ordinary
`lz_ext_p6_fx()` path -- this is buildable from what already exists, nobody has built it
yet) or the PS2 asset pipeline's own tile-cut metadata. So `chip_ordinal % 39` is stable
but unverified past that. See `Cps3FirstLight_TileForChip()`'s declaration comment
(`cps3_first_light.h`) for the full derivation.

**(a) left as-is, now with a citation for why -- moot for this CG specifically.** CPS-3
CHARRAM DMA never writes fewer than 256 B: the reconstructed CG 0x060A CgList's own 11
records take lengths 512/256/256/1024/512/512/1024/4096/256/512/1024 -- all multiples of
256 -- matching §5G's length-code histogram (`0x0f..0xff` -> 256..4096 B, five values,
nothing smaller). There is no 64 B CPS-3-side unit for a wh=1 chip to be a fragment of, so
wh=1 stays on the PS2 path deliberately -- a correct argument in general, but it defends a
decision about an EMPTY class for CG 0x062A: this CG's own trans_table has zero wh=1
chips, so the branch it defends never fires for the sprite this round actually touches.

**New, undocumented-until-now finding: wh=4 chips.** The same live capture that confirmed
15 total chips per draw of this CG found 10 of them are wh=4 (1024 B, case 4 in
`mlt_obj_trans_ext()`) -- a separate switch arm with no CPS3 check at all, unconditionally
PS2: 10/15 chips, and **89% of this CG's own texture bytes** (10x1024 B vs. 5x256 B).
This task's brief named only wh=1 as the non-hijacked class; wh=4 is both a larger share
and was out of scope for this round. The blocker is **not** "a different texture-table/
cache indexing scheme" -- `case 4`'s `get_mltbuf32_ext_2()` -> `lz_ext_p6_fx()` ->
`njReLoadTexturePartNumG(mt->mltgidx32, ...)` is structurally identical to the wh=1/2 path
above it, just sized for 32x32/`mltgidx32` instead of 16x16/`mltgidx16`; the hijack could
be inserted there the same way. The real blocker is that a wh=4 chip's 1024 B texture is
FOUR 16x16 CPS-3 tiles assembled into one 32x32 twiddled upload (not a single tile), plus
that this CG's own wh=4 draw boxes are not uniformly 32x32 (chip @0 is `dw=24`, chip @12
is `dw=8`/`dh=24`, chips @7/@14 are `dh=16`) -- both unimplemented here, flagged, not
fixed.

## What this round actually achieved

Read plainly, without the framing of "fixing" the sprite: this round took CG 0x062A's 5
wh=2 chips (3 of which draw at half-width, `dw=8`) from *flickering wrong tile* (the old
round-robin picked a different, arbitrary decoded tile on every redraw) to *static wrong
tile* (the same decoded tile, every redraw, still not proven to be the geometrically
correct one). The other 10 chips -- 89% of this CG's own texture bytes -- were PS2 before
this round and are PS2 after it; nothing here reaches them. **This is not a fix for the
sprite glitching, and does not make the picture correct.** A prior review established that
this approach cannot reach that goal as built: only 5 of 15 chips are hijackable at all
under the current wh=2-only scaffolding, so even a perfect chip-to-tile mapping for those
5 would leave the sprite mostly PS2. Attempting to extend the current hijack to wh=4
chips WITHOUT first establishing a real geometric or content-based mapping (the
still-open question above) would make MORE of the sprite wrong, not less: it would swap
four more (currently-correct, shipped) PS2 chips per wh=4 slot for arbitrarily-assigned
arcade tiles, at 4x the pixel footprint of a wh=2 chip.

**Visual check performed, not a full one.** A telemetry-flavour host build with both
fixes was run against the same repro and screenshotted repeatedly over several seconds on
a static training-mode idle pose; Alex's sprite renders coherently, and a frame-to-frame
pixel diff traces a single coherent whole-body silhouette (consistent with ordinary idle
motion), not scattered rectangular blocks. The same diff method run against the
pre-fix build over the same pose was visually indistinguishable at this granularity --
the flicker this round targets is described as "a handful of small areas" and this CG's
texture pattern is cached once built, so it only re-fires on a cache miss (pose change /
eviction), which a several-second static capture does not reliably exercise. A human
still needs to watch the live app through actual animation cycling (not a frozen pose) to
confirm the flicker is gone by eye.

## The palette crash, and why the hijack was removed

The first attempt pointed the chip's palette at a scratch `ColorRAM[510]` row and called
`palUpdateGhostCP3()`. That segfaulted deterministically ~21 s in, at
`sw_blit_indexed8_row_rev` with `KERN_INVALID_ADDRESS at 0x0`.

**Whether a ColorRAM row is free is the wrong question.** The chip drawing this CG never
resolves its palette against ColorRAM at all: `obj_group_table[0x062A]` -> group 2 ->
`mts_base[2].mode` = 4113, and `mode & 7 == 1` selects `palGetChunkGhostDC()`, a
16-entry directory (`col3rd_w.palDC`) that row 510 cannot address. The bounds guard in
`ppgGetUsingPaletteHandle()` returns the invalid sentinel 0, `Renderer_SetTexture()`
turns that into `palette_index = -1`, and `rasterize_textured()` then reads `pal->colors`
off a NULL `pal` -- `colors` is the first field, hence the fault at exactly 0x0.

The hijack was removed rather than repointed: `palDC`'s 16-slot pool has no
independently-proven-free row, so borrowing blind risked corrupting a real character's
palette. `Cps3FirstLight_PaletteRaw()` remains for a future patch that routes ROM
palettes through `palDC` properly -- whether that is actually needed for character
sprites is an open question (see the correction under "Seen on screen" above), not a
settled "probably not."

## What this proves, and what it does not

Proven: the loader's pair-interleave, the decoder, the palette rule, and the upload
handoff compose end to end on a real ROM, for the 5 of 15 chips (wh=2) this scaffolding
actually hijacks, without crashing or scrambling pixels. This is a pipeline-composition
result, not a claim that the resulting picture is correct -- see "What this round
actually achieved" above for why 89% of this CG's own texture bytes (the wh=4 chips)
are untouched, and the "Seen on screen" correction above for why the earlier "visually
correct" framing overstated what was actually observed.

**Not** proven:

- **Colour fidelity beyond one costume, on a minority of one CG's chips.** Colours looked
  right on Alex's LP costume with the port's own palette (above), but this is one costume
  of one character, judged by eye, on 5 of 15 chips (3 of those half-drawn at `dw=8`). The
  capture shows CG 0x060A draws with colour-RAM palette 8
  (`run1.spr.log`: `HDR blk=9 … gpal=008`), which is what is used — but *which SIMM
  granule palette DMA had loaded into palette 8* is unknown, because no palette-DMA log
  exists. The sprite renders visible; its colours are not verified against hardware.
- **A full CG.** `chip_ordinal % 39` is now a deterministic, stable assignment (not a
  rotating round-robin -- see "Round-robin and wh=1" above), but it is unverified against
  the chip's actual geometric position, and it only reaches 5 of this CG's 15 chips in the
  first place; the other 10 (wh=4, 89% of texture bytes) are unconditionally PS2 and
  untouched by any tile assignment at all. Records are ordered by `dst` (the oracle's
  `dst` fields are non-monotonic, so record order ≠ CHARRAM order), but no per-chip
  placement mapping is implemented or proven correct.
- **Anything about backgrounds, stages, or audio.** Stage tiles take a completely
  disjoint path.
- **That the cross-CG texture-cache leak is contained.** See "Round-robin and wh=1"
  above: the x16 cache this hijack writes into is keyed by `(code, palt)` with no
  `cg_number` component, and CG 0x062A shares texture group 2 -- and specific texture
  codes 54/55 -- with CGs 0x0623, 0x0624 and 0x062B. Nothing here prevents this CG's
  decoded tiles from leaking into those other sprites, or their PS2 tiles from leaking
  into this one, depending on upload/eviction order.

## Pass criterion — and why the obvious one does not work

`run1.dma.log` carries DMA command headers only (`src`/`dst`/`len`) — **never pixel
payloads** — so "byte-compare the rendered tiles against the log" is not possible, and
any plan that says so needs correcting. What is checked instead:

1. The 11 hardcoded records match the log's frame-1500 CG-0x060A burst byte-for-byte.
2. The decoder reproduces `chardma.py`'s independently-computed golden output
   (9,984 B / 39 tiles) — `--test-cps3-chardma`.
3. The interleave arithmetic reproduces a separately-built 64 MiB reference image
   byte-for-byte.
4. The twiddle round-trips against `PPGFile.c`'s own un-twiddle math.

None of these was a substitute for looking at the screen — which has now been done, and
which is what caught the remaining glitches. See "Seen on screen" above.
