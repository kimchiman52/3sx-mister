# 3SR format (v1) — device-facing replay format

Plan: `docs/plan-fcade-replay-browser.md` §4.2, Step B2. One `.3sr` file
represents one *game* (a Fightcade replay's `quark.json.num_matches` may be
> 1; each game gets its own `.3sr`). All integers are **little-endian**.
Magic is versioned (`3SR1`) so a future incompatible layout can ship as
`3SR2` without breaking existing files.

Every field in this format is exactly the set upstream statcheck seeds
before `PHASE_GAME` (plan §4.1) — no stage index, no derived state. Stage
selection flows from `new_challenger` + the seeded RNG, the same way the
original engine does it (plan B2 "What NOT to do").

## 1. Byte layout

```
offset  size  field
0x00    4     magic               = "3SR1" (ASCII, no NUL terminator)
0x04    2     version             u16, = 1
0x06    2     header_size         u16, = 28 for version 1
0x08    1     characters[0]       u8   -- P1 3SX character id (post CHAR_ARCADE_TO_3SX)
0x09    1     characters[1]       u8   -- P2 3SX character id
0x0A    1     supers[0]           u8   -- P1 Super_Arts index (0/1/2), no conversion
0x0B    1     supers[1]           u8   -- P2 Super_Arts index
0x0C    1     colors[0]           u8   -- P1 Player_Color (0-12), cosmetic
0x0D    1     colors[1]           u8   -- P2 Player_Color (0-12), cosmetic
0x0E    1     new_challenger      u8   -- raw NEW_CHALLENGER_OFFSET byte
0x0F    1     pad                 u8   -- reserved, MUST be 0
0x10    2     random_ix16         u16  -- see §2 (Random_ix16 canonicalized)
0x12    2     random_ix32         u16  -- see §2 (Random_ix32 canonicalized)
0x14    4     frame_count         u32  -- number of {p1,p2} input words that follow
0x18    2     checksum_interval   u16  -- 0 = no checksum table; else sampling period (in-game frames)
0x1A    2     checksum_count      u16  -- number of checksum table entries
------  ----  header ends here (header_size = 0x1C = 28 bytes for v1)
0x1C    frame_count * 4   input word table: frame_count * {u16 p1, u16 p2}, ARCADE-RAM layout (§3)
...     checksum_count * 8   checksum table: checksum_count * {u32 frame, u32 djb2} (§4)
```

The checksum table is **not** length-prefixed with its own offset field —
its position is always computable as:

```
checksum_table_offset = header_size + frame_count * 4
```

A parser reads `header_size` bytes, then `frame_count * 4` bytes of input
words, then `checksum_count * 8` bytes of checksum entries. Total file
size must equal `header_size + frame_count * 4 + checksum_count * 8`
exactly; anything else is a corrupt/truncated file.

If `checksum_interval == 0`, `checksum_count` MUST be 0 and no checksum
bytes follow the input word table (the file simply ends after the input
words).

## 2. Setup block field sources (mirrors `src/test/scrd_game.c`)

All setup fields are read from the single SCRD archive frame that matches
the game-start signature `G_No[1]==2 && G_No[2]==0 && G_No[3]==0`
(`ScrdGame_Init`, `src/test/scrd_game.c:36-70`). Call that archive frame
index `S` (0-based, into the SCRD archive's own frame table — NOT the
`.3sr`'s frame index space, see §3).

| Field              | Archive offset (arcade_constants.h)     | Width | Notes |
|--------------------|------------------------------------------|-------|-------|
| `characters[0..1]` | `MY_CHAR_OFFSET` (0x11387), 2 bytes      | u8×2  | `CHAR_ARCADE_TO_3SX` applied (below) |
| `supers[0..1]`     | `SUPER_ARTS_OFFSET` (0x1138B), 2 bytes   | u8×2  | no conversion (scrd_game.c:52-53) |
| `new_challenger`   | `NEW_CHALLENGER_OFFSET` (0x113DA)        | u8    | raw byte (scrd_game.c:55-56) |
| `colors[0..1]`     | `PLAYER_COLOR_OFFSET` (0x15683), 2 bytes | u8×2  | no conversion (scrd_game.c:58-59) |
| `random_ix16`      | `RANDOM_IX_16_OFFSET` (0x155E8)          | s16 BE in archive | see canonicalization below |
| `random_ix32`      | `RANDOM_IX_32_OFFSET` (0x155EA)          | s16 BE in archive | see canonicalization below |

**`RANDOM_IX_16`/`RANDOM_IX_32` are both 16-bit fields, despite the "32"
in the name.** Verified at `src/sf33rd/Source/Game/engine/workuser.h:612-613`
(`extern s16 Random_ix16; extern s16 Random_ix32;`) and at the two archive
read sites that actually consume them: `statcheck_compare.c:456-457`
(`Statcheck_SyncValues`, both via `read_s16`) and `statcheck_compare.c:239,243`
(`compare_service_values`, same). The two offsets are 2 bytes apart
(0x155E8, 0x155EA), consistent with two adjacent 16-bit fields, not a
16-bit + 32-bit pair. `make_3sr.py` stores both as **u16** (16-bit width
confirmed; the sign bit is preserved bit-for-bit, just reinterpreted
unsigned — no value is lost since the consumer re-derives the signed
value with a cast, and the format is not required to distinguish
signed/unsigned storage for a fixed-width field).

**RNG sync frame:** `random_ix16`/`random_ix32` are read from the *same*
archive frame `S` where the game-start signature matched — this is
deliberate, not a shortcut. `ScrdGame_Init` sets `game->start_index = S + 1`
(`scrd_game.c:62`), and `StatcheckRunner_Init` sets
`comparison_index = game.start_index` (`statcheck_runner.c:242`). The
runtime's RNG sync (`statcheck_runner.c:341-346`,
`PHASE_GAME_TRANSITION`) fetches `RamArchive_GetFrame(&game.archive,
comparison_index - 1)` — i.e. frame `S` again — and calls
`Statcheck_SyncValues` on it. So "frame `start_index - 1`" in the plan and
in this doc is the exact same frame as the game-start signature frame;
there is only one frame read for the whole setup block, not two.

**Character id conversion** (`CHAR_ARCADE_TO_3SX`, `src/constants.h:63`):

```c
#define CHAR_ARCADE_TO_3SX(c) ((c) > CHAR_AKUMA ? (c) - 1 : (c))
```

with `CHAR_AKUMA == 14`. The arcade (CPS3) character space has 21 ids
including `CHAR_SHIN_AKUMA == 15`; the non-CPS3 3SX build this format
targets has 20 ids and no separate Shin Akuma slot, so every arcade id
above 14 shifts down by one. `.3sr` always stores the **already-converted
3SX id** — a consumer must NOT re-apply this conversion.

## 3. Input word table

`frame_count * {u16 p1, u16 p2}`, one pair per in-game frame, starting at
SCRD archive frame `S + 1` (`game.start_index`) through the last frame in
the archive (`entry_count - 1`), inclusive. So:

```
frame_count = entry_count - (S + 1)
```

Each word is read verbatim from `P1SW_0_OFFSET` (0x6AA8C) /
`P2SW_0_OFFSET` (0x6AA90) of the corresponding archive frame — genuine
**arcade-RAM layout**: bits 0-3 = up/down/left/right, 4-6 = LP/MP/HP,
7-9 = LK/MK/HK, 12 = start (plan §2.3, as corrected by the 2026-07-21
ERRATUM — this is *not* the WCP `sw_lvbt` mirror, which is a different
archive offset and already engine-layout).

**A `.3sr` consumer that injects these words into `p1sw_buff`/`p2sw_buff`/
`p1sw_0` MUST first apply the arcade→SWK shift-convert** (LK 7→8, MK 8→9,
HK 9→10, start 12→14 — `src/test/replay_game.c:12-26`,
`src/test/statcheck_runner.c:125-171`'s `read_input_buff`, which performs
the equivalent conversion starting from the WCP mirror instead of
`P1SW_0`/`P2SW_0` but produces the same SWK-layout output). Feeding the
raw arcade-layout words straight into the engine's SWK-layout input
buffers is silent input corruption (plan §2.3, §4.1).

**Frame index** in this format always means an index into this input
word array (0-based, `0 .. frame_count - 1`). Frame 0 of a `.3sr`
corresponds one-to-one to SCRD archive frame `S + 1`
(`comparison_index`'s initial value in `statcheck_runner.c:242`) — the
first frame the statcheck harness actually injects and compares in
`PHASE_GAME`. This is the same alignment Step B1's cross-check measures
against (`comparison_index` semantics, `docs/plan-fcade-replay-browser.md`
Step B1).

## 4. Checksum table

Purpose: a cheap, sparse, self-contained sanity signal a future runtime
replay player (Stage C) can use to detect divergence without needing the
full SCRD archive on-device. **It is not a substitute for statcheck** —
it samples 13 small fixed fields, not full engine state.

### 4.1 Sampling

Entry `k` (`k = 0 .. checksum_count - 1`) covers `.3sr` frame index
`k * checksum_interval` (0-based into the input word array from §3, i.e.
the *same* frame-index space as the input word table — NOT the SCRD
archive's absolute frame number). Sampling starts at frame 0 and continues
while `frame_index < frame_count`:

```
checksum_count = ceil(frame_count / checksum_interval)   (checksum_interval > 0)
```

`make_3sr.py`'s default `checksum_interval` is **60** (one second of
in-game frames at 60 fps).

### 4.2 The 13-field window

The checksum is computed over a fixed, ordered set of 13 fields read from
the *same archived RAM frame* that produced the input words at that frame
index (i.e. SCRD archive frame `S + 1 + k * checksum_interval`). Every
field here already has an established archive-offset ↔ live-engine-variable
mapping in `src/test/statcheck_compare.c`, chosen specifically so a future
Stage C runtime checker can recompute the same hash from live engine state
using the exact same read expressions statcheck already uses — no new
mapping needs inventing.

| # | Field         | Archive offset expression                                    | Archive width | Cites |
|---|---------------|---------------------------------------------------------------|----------------|-------|
| 1 | `C_No[0]`     | `C_NO_OFFSET + 0`  (0x154A6)                                   | u16 BE | `statcheck_compare.c:255` |
| 2 | `C_No[1]`     | `C_NO_OFFSET + 2`                                              | u16 BE | ″ |
| 3 | `C_No[2]`     | `C_NO_OFFSET + 4`                                              | u16 BE | ″ |
| 4 | `C_No[3]`     | `C_NO_OFFSET + 6`                                              | u16 BE | ″ |
| 5 | `Game_timer`  | `GAME_TIMER_OFFSET` (0x1136C)                                  | u16 BE | `statcheck_compare.c:106-108,230` |
| 6 | `Random_ix16` | `RANDOM_IX_16_OFFSET` (0x155E8)                                 | s16 BE | `statcheck_compare.c:239,456` |
| 7 | `Random_ix32` | `RANDOM_IX_32_OFFSET` (0x155EA)                                 | s16 BE | `statcheck_compare.c:243,457` |
| 8 | P1 position X | `PLW_OFFSET + 0*PLW_SIZE + WORK_XYZ_OFFSET` (0x68C6C+0x64)     | s16 BE | `statcheck_compare.c:90-95` (`read_position`) |
| 9 | P1 position Y | same + `sizeof(XY)` (+4)                                       | s16 BE | ″ |
|10 | P2 position X | `PLW_OFFSET + 1*PLW_SIZE + WORK_XYZ_OFFSET`                    | s16 BE | ″ |
|11 | P2 position Y | same + 4                                                       | s16 BE | ″ |
|12 | `P1SW_0`      | `P1SW_0_OFFSET` (0x6AA8C)                                      | u16 BE | arcade_constants.h:27 (== this frame's input word) |
|13 | `P2SW_0`      | `P2SW_0_OFFSET` (0x6AA90)                                      | u16 BE | arcade_constants.h:28 |

`PLW_SIZE` is `0x498` (arcade_constants.h:30). `sizeof(XY)` is 4 bytes
(`include/structs.h:164-170`, a `union { s32 cal; struct { s16 low; s16
pos; } disp; }`).

### 4.3 Canonicalization (byte-exact, endian-neutral)

Each field's value is read from the archive using the *archive's* native
big-endian encoding (matching `read_u16`/`read_s16` in
`src/test/statcheck_utils.h` — every fixed-offset read out of a
decompressed SCRD RAM frame in this codebase is big-endian). The resulting
16-bit integer (signed fields keep their two's-complement bit pattern,
reinterpreted unsigned) is then re-encoded as **2 bytes little-endian**
before hashing. This canonicalization step exists so the *same* hash can
be recomputed from a live (host-native-endian) engine variable without
requiring the live side to know or care about the archive's on-disk
endianness — only "take this field's integer value, write it as 2 bytes
LE" is required on both sides.

The 13 fields' canonicalized 2-byte-LE encodings are concatenated **in
the table order above** (26 bytes total) and hashed with djb2:

```c
// src/sf33rd/utils/djb2_hash.h — the ADDITIVE djb2 variant already used
// by this codebase's netplay desync checksums (src/netplay/game_state.c
// :1921-1935, djb2_update_mem), reused verbatim here rather than
// inventing a second hash function:
uint32_t hash = 5381;
for each byte b in the 26-byte canonicalized buffer (in order):
    hash = hash * 33 + b;   // mod 2^32 (unsigned 32-bit wraparound)
```

**Deviation note:** an XOR variant (`hash = hash*33 ^ byte`) is sometimes
called "djb2" in casual descriptions. This codebase's actual
`djb2_hash.h` implementation is the **additive** variant
(`hash = hash*33 + byte`), and it is already wired through netplay's
desync detector. `make_3sr.py` matches that real implementation exactly
(verified by reading `src/sf33rd/utils/djb2_hash.h`) rather than the
XOR shorthand, specifically so a future Stage C checker can call
`djb2_init()`/`djb2_update_mem()` unmodified instead of shipping a second,
divergent hash routine.

### 4.4 Table entry format

Each entry is `{u32 frame, u32 djb2}` LE, where `frame` is the `.3sr`
frame index (§3's frame-index space, i.e. `k * checksum_interval`, NOT
the SCRD archive's absolute frame number) and `djb2` is the 32-bit hash
from §4.3.

## 5. Sizes

A 2-minute game at 60 fps is ≈ 7,200 frames: `28 + 7200*4 + 120*8 = 29788`
bytes ≈ 29 KB (checksum table with the default 60-frame interval adds
only ~1 KB). This matches the plan's "≈ 7,200 frames ≈ 29 KB" estimate
(plan §4.2).

## 6. What this format deliberately excludes

- **No stage index.** Stage selection flows from `new_challenger` + the
  seeded RNG exactly as the original engine derives it — adding a stage
  override is a Stage C playback *option*, not format truth (plan B2
  "What NOT to do").
- **No savestate / full RAM snapshot.** `.3sr` is SCRD-derived compact
  input+setup data only; it is not a substitute for the SCRD archive it
  was extracted from, and cannot alone reproduce byte-exact engine RAM —
  only the checksum table's 13-field samples are self-verifiable.
