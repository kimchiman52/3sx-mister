# 3SR format (v1 and v2) — device-facing replay format

Plan: `docs/plan-fcade-replay-browser.md` §4.2, Step B2. One `.3sr` file
represents one *game* (a Fightcade replay's `quark.json.num_matches` may be
> 1; each game gets its own `.3sr`). All integers are **little-endian**.

Every field in this format is exactly the set upstream statcheck seeds
before `PHASE_GAME` (plan §4.1) — no stage index, no derived state. Stage
selection flows from `new_challenger` + the seeded RNG, the same way the
original engine does it (plan B2 "What NOT to do"). **v2 exists because
that set grew by one**: `Statcheck_SyncValues` seeds `players_timer` as
well, and a `.3sr` that does not carry it cannot reproduce the recording's
`effect_G9_init()` spawn phase (§1.1).

## 1. Byte layout

Two versions exist. They share the first 28 bytes byte-for-byte; v2 appends
four.

```
offset  size  field
0x00    4     magic               = "3SR1" (ASCII, no NUL terminator) — SAME in every version
0x04    2     version             u16, = 1 or 2
0x06    2     header_size         u16, = 28 for version 1, 32 for version 2
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
------  ----  a VERSION 1 header ends here (header_size = 0x1C = 28 bytes)
0x1C    2     players_timer       u16  -- v2 ONLY. See §2 (free-running spawn-gate timer)
0x1E    2     reserved            u16  -- v2 ONLY. MUST be 0
------  ----  a VERSION 2 header ends here (header_size = 0x20 = 32 bytes)
<header_size>       frame_count * 4      input word table: frame_count * {u16 p1, u16 p2}, ARCADE-RAM layout (§3)
...                 checksum_count * 8   checksum table: checksum_count * {u32 frame, u32 djb2} (§4)
```

The `reserved` u16 at `0x1E` is not filler for its own sake: it keeps
`header_size` a multiple of 4, so the `{u32 frame, u32 djb2}` checksum table
starts on a 4-byte boundary relative to the file start exactly as it did in
v1.

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

### 1.1 Why a version bump, and why the magic did NOT change

The v1 header has exactly one spare byte (`pad` at `0x0F`) and
`players_timer` is a `u16`, so it cannot be carried without changing
`header_size` — and `header_size` is the offset of the input word table, so
any change to it is a layout change. Hence a new version.

**The magic stays `3SR1` in every version.** An earlier revision of this
document said a future layout would ship as `3SR2`; that is superseded. The
magic is the format FAMILY marker and `version` is what discriminates, for
one concrete reason: `fcade-proxy.js` gates servability on the magic
(`read3srGameData` → "game_N.3sr has a bad magic (not '3SR1')"), as do its
catalog and work-lease paths. Changing the magic would make the VPS refuse
to serve every newly converted replay until it was redeployed, while
changing only the version field costs nothing there.

### 1.2 Compatibility, in both directions

| reader | v1 file | v2 file |
|--------|---------|---------|
| pre-v2 viewer (`version != 1`) | plays | **refuses cleanly** — `ReplayPlayer_Init` rejects it at the version check ("unsupported version") and returns false; it never reaches a misparse |
| v2-aware viewer | **plays, `players_timer` treated as ABSENT** | plays, `players_timer` seeded |

"Absent" is not "zero". A v2-aware reader that meets a v1 file must leave
the engine's own `players_timer` alone rather than force it to 0 — that is
what makes v1 playback bit-identical to what it was before v2 existed, which
matters because tens of thousands of v1 files already exist — 507 on the
device and ~21,677 on the VPS at the time v2 was designed, plus 1,047 in the
Mac converter's own `3sr-out`. They are not
invalidated and are not re-converted on account of v2; they simply keep the
`effect_G9_init()` spawn-phase offset described in
`docs/research-arcade-balance-desyncs.md` §E2a, which surfaces as
`recover_random_ix16()` repairs at checkpoints rather than as a desync.

`replay_player.c` -> `ReplayPlayer_Init` holds both halves of this: the
version/`header_size` table, and `ReplayFile.has_players_timer`.

## 2. Setup block field sources

All setup fields are read from a single SCRD archive frame. Call its index
`S` (0-based, into the SCRD archive's own frame table — NOT the `.3sr`'s
frame index space, see §3). Finding `S` takes **two** frames and **two**
predicates, and a segment that fails either yields **no `.3sr` at all** —
see §2.1, which is the part of this section most easily got wrong.

The field reads themselves mirror `scrd_read_match_setup`
(`src/test/scrd_game.c`), and the producers mirror them in turn:
`find_match_start` / `_read_setup_block` in
`tools/fcade-replays/make_3sr.py`, and `Track3srBuildHeader` /
`Track3srOnFrame` in the FBNeo runner
(`tools/fcade-replays/runner-track-3sr.patch`).

`.3sr` is **not** a superset of what `ScrdGame_Init` reads, and does not try
to be. The oracle also reads `bg_w.stage` and the two `wu_operator` bytes;
neither is in this format, for opposite reasons — the stage is re-derived at
playback (§6), and `wu_operator` is a *filter*, not data: only recordings
with both operators set are ever converted, so the value is known to be
non-zero in every file that exists (§2.1).

| Field              | Archive offset (arcade_constants.h)     | Width | Notes |
|--------------------|------------------------------------------|-------|-------|
| `characters[0..1]` | `MY_CHAR_OFFSET` (0x11387), 2 bytes      | u8×2  | `CHAR_ARCADE_TO_3SX` applied (below) |
| `supers[0..1]`     | `SUPER_ARTS_OFFSET` (0x1138B), 2 bytes   | u8×2  | no conversion (scrd_game.c:52-53) |
| `new_challenger`   | `NEW_CHALLENGER_OFFSET` (0x113DA)        | u8    | raw byte (scrd_game.c:55-56) |
| `colors[0..1]`     | `PLAYER_COLOR_OFFSET` (0x15683), 2 bytes | u8×2  | no conversion (scrd_game.c:58-59) |
| `random_ix16`      | `RANDOM_IX_16_OFFSET` (0x155E8)          | s16 BE in archive | see canonicalization below |
| `random_ix32`      | `RANDOM_IX_32_OFFSET` (0x155EA)          | s16 BE in archive | see canonicalization below |
| `players_timer`    | `PLAYERS_TIMER_OFFSET` (0x157CE)         | u16 BE in archive | **v2 only** — see below |

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

**`players_timer` (v2 only).** A free-running `u16` — `players_timer++;
players_timer &= 0x7FFF` in `plcnt.c`, `plcnt2.c` and `plcnt3.c` (one site
per `Player_control` / `Player_control_bonus` / `Player_control_bonus2`).
A recording inherits it from a whole arcade session; a synthetic replay
start has it at 0. It is a live input to a spawn gate —
`src/sf33rd/Source/Game/effect/effg6.c` -> `effect_G6_move`:

```c
if (ewk->wu.now_koc & (players_timer + ewk->wu.blink_timing)) { break; }
```

— whose mask is 0..7, so a wrong value puts every `effect_G9_init()` spawn
(`effg6.c`, the line after the gate), and therefore the two `random_16()`
draws in `effect_G9_move`'s `case 0:`, on the wrong frame. Seeding it **once** at the sync frame is sufficient: both
engines then increment it on the same frames. This is exactly what
`src/test/statcheck_compare.c` -> `Statcheck_SyncValues` does for the
statcheck oracle; v2 carries the same value to the viewer.

The archive offset `0x157CE` (CPS3 `0x020157CE`) was established by
**disassembly, not by a value scan** — a scan for "a `u16` incrementing by 1
every frame" prunes it, because it stalls under `Game_pause || EXE_flag`.
`effg6_data` occurs once in the decrypted image; its only literal referrer is
inside CPS3's `effect_G6_move`; and `0x020157CE` has exactly four referrers —
the gate plus three `+1; & 0x7FFF` increment sites matching the port's three
`plcnt*.c` files. The full argument is in `statcheck_compare.c` ->
`Statcheck_SyncValues` and in `docs/research-arcade-balance-desyncs.md` §E2a.

**RNG sync frame:** `random_ix16`/`random_ix32`/`players_timer` are read from the *same*
archive frame `S` where the setup block was latched — this is
deliberate, not a shortcut. `ScrdGame_Init` (`src/test/scrd_game.c`) sets
`game->start_index` to the CONFIRMED frame, `S + 1`, and
`StatcheckRunner_Init` (`src/test/statcheck_runner.c`) sets
`comparison_index = game.start_index`. The runtime's RNG sync
(`statcheck_runner.c`, `PHASE_GAME_TRANSITION`) fetches
`RamArchive_GetFrame(&game.archive, comparison_index - 1)` — i.e. frame `S`
again — and calls `Statcheck_SyncValues` on it. So "frame
`start_index - 1`" in the plan and in this doc is the exact same frame as
the armed/signature frame; there is only one frame read for the whole setup
block. `players_timer` is read from that same frame, for the same reason: it
is the frame `Statcheck_SyncValues` is handed.

Measured over the 16-segment corpus in
`/Volumes/KimchDrive/3sarm-convert-tmp/rerun2`: `start_index == 1` (so
`S == 0`) on every one of the 14 segments that contains a match, and no start
at all on the two that do not.

### 2.1 Finding `S` — and the two segments that must NOT become a `.3sr`

A recorded session is cut into segments on the in-game -> not-in-game
transition, and **not every segment is a game two people played**. Two classes
are unconvertible. Both are enforced by `ScrdGame_Init`
(`src/test/scrd_game.c`) for the statcheck oracle and by both producers —
`find_match_start` (`tools/fcade-replays/make_3sr.py`) and `Track3srOnFrame`
(`tools/fcade-replays/runner-track-3sr.patch`) — for the files themselves.

**(a) The `G_No` triple is armed, not confirmed.**
`G_No[1..3] == (2, 0, 0)` says only "the Game task is parked on the `Game2_0`
slot" (`Game_Jmp_Tbl[G_No[1]]` -> `Game02` -> `Game02_Jmp_Tbl[G_No[2]]`,
`game.c`). It does **not** say a match started: a segment cut right after a
final KO can carry that triple frozen for its entire length while the Game task
is not being ticked at all — measured on two archives whose `(G_No, C_No)` pair
never changes across 2,270 and 2,286 frames.

The signature of a match that *did* start is the visible effect of `Game2_0()`
(`game.c`) having run — it writes `Game_timer = 0; C_No[0..3] = 0; G_No[2] = 3;`
in one frame. So the frame **after** the triple must show
`Game_timer == 0 && G_No[2] == 3`. The triple frame is `S` (setup block, RNG
pair, `players_timer`); the confirming frame is `S + 1`, and it is also the
first frame of the input table (§3). If no triple is ever confirmed the segment
holds no match: `make_3sr.py generate` exits **2** and the runner's tracker
records `"skip_reason": "no-match-start"`. No file is written.

**(b) The cabinet was playing against the CPU.**
At the confirmed frame `S + 1`, both `plw[0].wu.wu_operator` and
`plw[1].wu.wu_operator` (`PLW_OFFSET + 3` = `0x68C6F`,
`PLW_OFFSET + PLW_SIZE + 3` = `0x69107`) must be non-zero. A zero means the
cabinet ran `Play_Type == 0` with `cpu_algorithm()` driving that side, and
`Player_move()` (`src/sf33rd/Source/Game/engine/plmain.c`) therefore **discarded** the
hardware button words this format stores:

```c
if (wk->wu.wu_operator) { wk->cp->sw_lvbt = lv_data; }
else { wk->cp->sw_lvbt = processed_lvbt(cpu_algorithm(wk)); }
```

The device viewer cannot reproduce that. `ReplayPlayer_Tick`
(`src/replay/replay_player.c`) taps `SWK_START` for player 2 at
`PHASE_CHARACTER_SELECT`, exactly as the statcheck harness does, so **both**
sides always come up with `wu_operator != 0` and are fed the stored words —
words that never moved the CPU-driven character. Measured: a `.3sr` built from
a `(1, 0)` segment reports `REPLAY DESYNC at frame 60`. `make_3sr.py generate`
exits **3**; the runner's tracker records `"skip_reason": "cpu-player"`. No file
is written.

This is why a quark yields fewer games than `quark.json.num_matches`. On the
16-segment corpus the split is 6 convertible, 2 no-match, 8 CPU. Fightcade's
winner-plays-the-CPU-until-a-challenger-arrives flow means CPU segments
interleave with human ones inside one recording, so the ineligible ones are not
a suffix that could be trimmed by index.

**Exit-code table**, shared by the oracle (`src/main.c`) and the producer
(`make_3sr.py` -> `cmd_generate`), on purpose:

| code | meaning | is it a defect? |
|------|---------|-----------------|
| 0 | converted / compared clean | no |
| 1 | engine divergence (oracle) or converter failure (producer) | yes |
| 2 | segment holds no match | no — skip |
| 3 | recorded against the CPU | no — skip |

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

A 2-minute game at 60 fps is ≈ 7,200 frames: `32 + 7200*4 + 120*8 = 29792`
bytes ≈ 29 KB (checksum table with the default 60-frame interval adds
only ~1 KB; the v2 header adds 4 bytes over v1's `28 + … = 29788`). This
matches the plan's "≈ 7,200 frames ≈ 29 KB" estimate (plan §4.2).

## 6. What this format deliberately excludes

- **No stage index.** Stage selection flows from `new_challenger` + the
  seeded RNG exactly as the original engine derives it — adding a stage
  override is a Stage C playback *option*, not format truth (plan B2
  "What NOT to do").
- **No per-frame `players_timer`.** It is seeded once, at the sync frame,
  and both engines then increment it in lockstep — the same argument
  `Statcheck_SyncValues` makes. Sampling it per frame would be a second,
  redundant clock.
- **No savestate / full RAM snapshot.** `.3sr` is SCRD-derived compact
  input+setup data only; it is not a substitute for the SCRD archive it
  was extracted from, and cannot alone reproduce byte-exact engine RAM —
  only the checksum table's 13-field samples are self-verifiable.
