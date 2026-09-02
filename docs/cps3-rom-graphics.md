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

Alex's idle animation then draws its `wh=1/2` chips from arcade tiles instead of PS2
ones. Decoder correctness is covered by `--test-cps3-chardma`.

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
`dst[0] = 0x0000; dst[i] = src[i] | 0x8000` for `i = 1..63`. CPS-3 transparency is pen
*index* 0. A value-based "set bit 15 on non-zero entries" rule looks equivalent and is
not: the ROM bank holds thousands of index-≥1 entries whose value is `0x0000` (opaque
black), which such a rule would turn transparent. Applied at `ColorRAM` write time,
because `lz_ext_p6_cx()` reads `ColorRAM` directly.

**`ColorRAM[510]` is borrowed, not free.** `color_file[20]` (`type=2`, AFS file 0x9,
480 rows) bulk-loads rows 32..511 at boot, so 510 holds real data — a `grep` for
`ColorRAM[` cannot see this because the index is data-driven. The load is boot-once and
no drawing consumer of row 510 was identified, so the hijack wins and is never clobbered
back, but absence of a reader was **not** proven.

**SIMM2-6 are revision-independent; only SIMM1 is not.** Verified against MAME's
`ROM_START(sfiii3n)` vs `ROM_START(sfiii3nr1)`: 32 files with identical CRC32/SHA1 and
no `// sldh` markers, which appear only on SIMM1 lines. Nothing here needs the
990512/990608 adjustment. (`rom_load.c` pins **990512 / nr1**; `sfiii3` is 990608, and
the two are related by `nr1 = e - 0x14C` — for SIMM1 only.)

## What this proves, and what it does not

Proven: the loader's pair-interleave, the decoder, the palette rule, and the upload
handoff compose end to end on a real ROM.

**Not** proven:

- **Colour fidelity.** The capture shows CG 0x060A draws with colour-RAM palette 8
  (`run1.spr.log`: `HDR blk=9 … gpal=008`), which is what is used — but *which SIMM
  granule palette DMA had loaded into palette 8* is unknown, because no palette-DMA log
  exists. The sprite renders visible; its colours are not verified against hardware.
- **A full CG.** Tiles are handed out one per chip, so a single frame shows a rotating
  subset of the 39 tiles, not a correct assembled sprite. Records are ordered by `dst`
  (the oracle's `dst` fields are non-monotonic, so record order ≠ CHARRAM order), but
  no per-chip placement mapping is implemented.
- **Anything about backgrounds, stages, or audio.** Stage tiles take a completely
  disjoint path.

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

None of these is a substitute for looking at the screen, which has not been done.
