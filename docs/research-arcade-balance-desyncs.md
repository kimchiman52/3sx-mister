# Arcade-Balance Desyncs: Engine Divergences from CPS3

**Scope:** behavioural divergences between this port and CPS3 arcade hardware
that show up when `ArcadeBalance_IsEnabled()`. Also the *measurement* problems
that hid them — the statcheck harness's false positives and the on-device
detector's coverage.

**Why this is not in `research-arcade-cg-data-accuracy.md`.** That document
audits whether our copies of ROM-backed data match the ROM: CG tables, sound
codes, `cg_zoom`, `cg_effect`, stage effects. Every finding here is a *control
flow* or *engine state* difference, not a wrong table value. The distinction is
not bookkeeping — it is exactly why a thorough seven-pass ROM-data audit could
not have found finding **E1** below: `Pow_Control_Data_1` is a compiled-in
constant in `pow_data.c`, its values are correct, and the defect is an `if`.
Filing engine behaviour into a data-fidelity document reproduces that blind
spot. Keep them separate.

**Status (2026-09-05).** Fixed: **E2a** in the statcheck oracle (`f63507b7`)
and now in the shipped viewer too (`.3sr` v2 carries `players_timer`); **D2**
on the viewer (`c6a75572`, confirmed on hardware). Open: **E1** has an
external patch, not merged, blocked on a netplay question; **E2b** and **E3**
have no patch and E2b is unidentified; **H1-H3** are unfixed, so a broad
statcheck sweep would still produce false positives. Nothing here has had a
Fable review yet.

**E2a's viewer half is untested on hardware.** The v2 fix was measured on the
host build only (below); nothing has been deployed to the MiSTer, and the
existing corpus (507 device files, ~21,677 VPS files) is v1 and keeps the old
behaviour until re-converted.

---

## How this was measured, and what that costs

`publish_3sr.py` runs the FBNeo runner to produce ground-truth CPS3 RAM per
frame, then replays our engine against it with `statcheck_compare.c` asserting
field-by-field, every frame. A failure prints the asserting line and the archive
frame index.

Two properties of the harness matter for reading anything below:

- **It is deterministic.** Four quarks re-converted from scratch reproduced
  every segment count, every PASS/FAIL and every failure value identically.
  Verdicts here are reproducible, not sampled.
- **It force-syncs `Random_ix16` every frame**, by design — see the comment
  "This is dirty, but syncing Random_ix16 every frame helps avoid
  animation-related desyncs" in `statcheck_compare.c`. So `Random_ix16`
  divergence is invisible to statcheck by construction. `Random_ix32` is
  compared honestly. Do not read a clean statcheck as evidence about
  `Random_ix16`.

**Reproduction** (the CWD and interpreter both matter):

```sh
PY=~/Developer/fbneo-replay-runner/venv/bin/python   # 3.14; system python3 is 3.9 and cannot parse this code
cd ~/Developer/fbneo-replay-runner                   # the runner is spawned inheriting this CWD
TMPDIR=/Volumes/KimchDrive/3sarm-convert-tmp "$PY" tools/fcade-replays/publish_3sr.py \
  --catalog <cat.json> --runner <fbneosdldarm64> --statcheck <3S-ARM with THREESX_STATCHECK=ON> \
  --out-dir <out> --quark <quarkid> --statcheck-timeout 60 --runner-timeout 1800
```

Raw dumps are 524,288 B/frame and peak ~26 GB on a long session — keep `TMPDIR`
off the boot volume. `publish_3sr.py` has returned **exit code 0 while printing
a fatal traceback**; read its output, never its status.

---

## The instrumentation (commit `90ccb151`)

`RngTrace_*` in `pls02.c` records `__builtin_return_address(0)` for every RNG
call into a 256-entry ring, reset each frame. `statcheck_compare.c` computes the
per-frame delta BEFORE the force-sync and, when non-zero, prints the callers
resolved through `dladdr()`.

**Why the delta is exact, and why the force-sync is what makes it so.** Each
frame begins synced, both sides advance one per call, and the generator masks to
six bits (`Random_ix16 &= 0x3F`). Therefore

    (our Random_ix16 - archive Random_ix16) & 0x3F

read before the next sync IS (our calls - CPS3's calls) for that frame, for any
true difference under 64. The "dirty" hack everyone works around is the thing
that makes the measurement precise.

**Limits.** It names only OUR call sites. A negative delta means CPS3 called
something we did not, and that answer is in the arcade disassembly. Reports are
capped (`STATCHECK_RNG_DRIFT_MAX`) so a run that drifts every frame cannot bury
the first divergence.

Usage:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  build/statcheck-verify/3S-ARM.app/Contents/MacOS/3S-ARM --ram-archive <x.scrd> 2>&1 | grep statcheck-rng
```

---

## Confirmed engine divergences

### E1 — `Round_Level` is ignored in VS play, so damage is off by one step

**Symptom.** `vital_new` differs by exactly 1, ours always *higher* (we deal
less damage). Five independent instances across one session, at unrelated
frames, all off by 1 in the same direction.

**Mechanism.** `cal_damage_vitality()` and `cal_damage_vitality_eff()`
(`pow_pow.c`) select the damage scale with:

```c
if (Play_Type == 1) { yy = Pow_Control_Data_1[0][3]; }
else                { yy = Pow_Control_Data_1[0][Round_Level]; }
```

`Pow_Control_Data_1[0]` (`pow_data.c`) is `{90,95,98,100,103,106,109,112}` — a
percentage scale whose **index 3 is exactly 100**. So the port pins neutral
damage for human-vs-human play, while the arcade scales by the cabinet's
`Round_Level`. `Play_Type == 1` means both players are human
(`Setup_Play_Type()`, `sys_sub.c`).

**Why it is invisible early in a session.** `Round_Level` initialises to 3
(`game.c`), which *is* the hardcoded index — so the first games of a session
match, and divergence begins only once the cabinet's level moves off 3. On the
measured session, games 0–2 PASS and 3–9 FAIL. That clean-prefix-then-permanent
signature is the tell for this defect.

**`Round_Level` is per-cabinet session state that cannot be re-derived.**
`Update_VS_Data()` (`manage.c`) returns early when `Play_Type != 0`, so our port
never updates it during VS play at all; `Loser_Sub()` decrements only when
`Play_Type == 0`. A replay resumes mid-session, so the value it ran under is not
recoverable from the replay — it has to be read from the archive. That means a
statcheck for this defect needs a `ROUND_LEVEL_OFFSET` added to
`arcade_constants.h` and imported in `sync_values()`, which does **not** exist
on `new-stuff` today.

**Proposed patch** — external, from `gibletto/3sx` branch
`round-level-damage-parity` (commit `2d1edca6`), NOT merged:

```c
if (Play_Type == 1 && !ArcadeBalance_IsEnabled()) { ... }   // both call sites
```

plus `ROUND_LEVEL_OFFSET 0x1137A` and the archive import.

**ANSWERED from the archives (2026-09-05).** This was previously written up as a
decision to be taken. It is not a decision — it is an empirical question, and the
archives contain real CPS3 RAM for whole human-vs-human sessions. Reading
`Round_Level` (offset `0x1137A`) across every game of four sessions:

```
7733  g0 [3]  g1 3->2  g2 [2]  g3 3->2  g4 2->1  g5 [3]  g6 [3]
5743  g0 [3]  g1 3->2  g2 [2]  g3 [3]
1710  g0 [3]  g1 3->2
3455  g0 [3]  g1 [3]   g2 [3]
```

Three facts, all consistent across 16 games in 4 independent sessions:

1. **The arcade baseline is 3.** Every session starts there. The values ever
   observed are 1, 2 and 3 — **never 0**.
2. **`netplay.c`'s `Round_Level = 0` is therefore wrong**, not merely
   questionable. Seeding 3 matches both the hardware and the port's own
   `game.c` init; seeding 0 would index `Pow_Control_Data_1[0][0]` = 90 and cut
   netplay damage 10%, which is what made the external patch look dangerous.
   With 3, that danger disappears.
3. **The arcade CHANGES it during a 2P session**, always downward. Our port
   never does: `Update_VS_Data()` returns early when `Play_Type != 0`, and
   `Loser_Sub()` decrements only when `Play_Type == 0`.

**So E1 is two defects, and the external patch fixes only the first.**

- **E1a** — `pow_pow.c` ignores `Round_Level` in VS play. Gibletto's patch, plus
  seeding netplay to 3 rather than 0.
- **E1b** — our port never *updates* `Round_Level` during VS play while the
  arcade ramps it down. With E1a alone we would pin whatever we seed while the
  hardware decays away from it, so the damage scale drifts apart over a session
  exactly as it does today, just from a different starting point.

**Not established:** whether the observed decrements happen mid-fight or at the
internal rematch boundary inside a merged segment (see H4 — `Game2_2()` merges a
rematch into one `game_N`). That distinction decides where E1b's fix belongs, and
it is answerable from the same archives by correlating the change frame against
`C_No`/`PL_Wins`.

### E2a — `effect_G9` spawn phase (ROOT-CAUSED; oracle FIXED `f63507b7`)

**Symptom.** Our engine spawned `effect_G9` a few frames later than CPS3. Each
spawn burns exactly two `random_16()` (`effect_G9_move` `case 0:`, `effg9.c`), so
the drift alternated -2/+2 at a fixed lag on archives that otherwise PASS. 42 of
42 traced RNG calls came from that one function.

**Cause.** `effect_G6_move` (`effg6.c`) gates the spawn on
`now_koc & (players_timer + blink_timing)`. `players_timer` is a free-running
`u16` the harness never imported, so our synthetic match start had it at 0 while
the archive was mid-session in the tens of thousands.

**Address, proven by disassembly: CPS3 `0x020157CE`** (archive offset
`0x157CE`). `effg6_data` occurs once in the decrypted image at `0x061C6B38`; its
only literal referrer is inside CPS3's `effect_G6_move` at `0x061083E4`; the gate
at `0x061085A0` reads `0x020157CE`, adds `blink_timing` (WORK +2), and tests
against `now_koc` (WORK +0x0206). That address has exactly four referrers in the
image — the gate plus three `+1; & 0x7FFF` increment sites, matching the port's
`plcnt.c` / `plcnt2.c` / `plcnt3.c`.

**Why the empirical scan could not settle it — worth remembering.** A scan for "a
`u16` incrementing by 1 every frame" *prunes* `players_timer`, because it stalls
~312 frames per game under `Game_pause || EXE_flag`. It also produced a
false positive, `0x07F02`, which sat a constant 7,254 ahead over the sampled
window and so gave identical residues mod 4; the two only diverge across a whole
game. **A residue test on a short window is blind to a constant offset.** Only
the disassembly discriminated.

**Result:** drift 322 → **0**, 174 → **0**, 255 → **0**, 418 → **0** across four
archives. Every other archive fails at exactly the same frame with the same drift
count — no regression, no improvement.

**Not fixable by a FAILING archive, by construction.** A G9 phase error only
moves `Random_ix16`, which the oracle overwrites every frame, so it can never
make statcheck FAIL. Its whole cost is downstream: the device viewer's
`recover_random_ix16()` recoveries (288 in 13 replays).

**CLOSED for the viewer too — `.3sr` v2.** The v1 header (`header_size = 28`)
carried `random_ix16`/`random_ix32` but not `players_timer`, so the viewer
started every replay with whatever the engine had (0) and kept the full phase
offset. v2 (`header_size = 32`) appends `players_timer` at `0x1C`;
`ReplayPlayer_Init` reads it and `ReplayPlayer_Tick`'s `PHASE_GAME_TRANSITION`
seeds it at the same point it seeds the RNG pair — the point
`Statcheck_SyncValues` syncs at. See `docs/3sr-format.md` §1.1/§1.2 for the
version-vs-magic decision and for what a v1 file does now (nothing changes:
`players_timer` is treated as ABSENT, not as 0).

**Measured on the host build, A/B on the same archive.** Two `.3sr` files were
built from one SCRD differing ONLY in the v2 header bytes (identical input word
table and checksum table, verified byte-for-byte), then played through
`--play-replay --headless`:

| archive | frames played | checkpoints | v1 twin `r16_resyncs` | v2 twin `r16_resyncs` |
|---------|---------------|-------------|------------------------|------------------------|
| `sfiii3nr1-1676027217605-8232.7/game_2` | 12,124 | 202/202 | **31** | **0** |
| `sfiii3nr1-1675032176612-9791.7/game_0` |  3,507 |   58/58 | **13** | **0** |

Both twins of each pair reached `REPLAY COMPLETE … reason=game-ended` with the
same frame count and the same `checksums=N/N`, so the counts are over identical
checkpoint sets. The
divergences were ±2 on `Random_ix16` — the two `random_16()` draws per
`effect_G9_move` `case 0:`, exactly the predicted signature.

### E2b — a `random_32` consumer the port never executes (OPEN)

**Correction.** An earlier revision of this document unified E2a and E2b, calling
the `Random_ix32` off-by-one a second face of the G9 phase offset. **That was
wrong**, and the fix above disproves it: `players_timer` is now imported, all G9
drift is zero, and these three archives fail at exactly the same frame as before.

**Symptom.** `3455 game_1` @359, `5743 game_1` @301, `7733 game_3` @247 all fail
identically: in a single frame CPS3 consumes **+2 `random_16` and +1
`random_32`**, while our engine consumes **zero RNG calls for 40 frames either
side** (windowed trace over archive frames 260–302).

**Ruled out.** A BFS over the resolved CPS3 call graph finds no path from
`effect_G9_move` (`0x06108AC8`) to `random_32` (`0x0611E0D6`) at depth 5, nor
from `char_move` (`0x06089848`). So this is a consumer the port does not execute
at all — not a mistimed one.

**Not identified.** The two CPS3 functions carrying both ≥2 `random_16` and a
`random_32` are `0x0610F278` (3×16, 1×32) and `0x0610F5C4` (4×16, 1×32); neither
matches +2/+1 alone. Could not verify what runs there — searched the call graph
and the port's function-level RNG census.

This is the one that actually fails statcheck, so it is the higher-value target.

### E3 — `pos.x` jumps +32 at round start, cause unknown

**Symptom.** P0 `pos.x` reads 424 where the archive holds 392, at archive frame
54, in two independent sessions of the **same matchup** (char 1 vs char 10).
392 is the archive-side round-start default; 424 is ours.

**Ruled out, each by measurement:** segmentation (well-formed match start);
input alignment (our `C_No`, `G_No[1..3]`, `Game_timer` and injected input words
match the archive frame-for-frame for 53 frames); stage
(`app_type_tbl[1][10]` is flat across all stages and our engine demonstrably
used that entry — `routine_no[4]` matched); Champion/home-visitor
(`app_type_tbl2[1][10]` gives a different `rno` that would have failed earlier);
lever warm-up.

**Open.** No mechanism established. Reproduces reliably, so it is tractable.

---

## Harness false positives — fix these before trusting a statcheck sweep

Six of eleven observed failures were **not** engine bugs. Each has a concrete
cause; until they are fixed, a statcheck sweep over multi-game sessions will
report divergences that are not there.

### H1 — `ScrdGame_Init()` false-positives on the post-KO state (FIXED)

`ScrdGame_Init()` (`scrd_game.c`) scanned for `G_No[1..3] == (2,0,0)` to find
a match start. That triple says only "the Game task is parked on the `Game2_0`
slot" (`Game_Jmp_Tbl[G_No[1]]` -> `Game02` -> `Game02_Jmp_Tbl[G_No[2]]`,
`game.c`) — not that a match started. The archive then began mid-match while
our engine started fresh, giving `Game_timer (0) != <large>` at archive
frame 1.

Diagnostic law, exact in every case checked: the carried-in `Game_timer` at
frame 0 equals `len(previous segment) − 2`.

**What the two affected segments actually contain — measured.** Neither holds
a match at all. Dumping every frame of `5743 game_2` (2,270 frames) and
`7733 game_2` (2,286 frames), the pair `(G_No, C_No)` **never changes** from
`((0,2,0,0), (9,1,0,0))` across the whole segment. `Game2_0()` therefore never
runs in them. They are pure post-KO tail — H4's segmenter behaviour, seen from
the other side. So the right verdict for these is "nothing to check", not
"start later".

**Fix (this commit).** `Game2_0()` (`game.c`) writes, in one frame,
`Game_timer = 0; C_No[0..3] = 0; G_No[2] = 3;`. `ScrdGame_Init()` now requires
the frame *after* the `(2,0,0)` triple to show `Game_timer == 0` **and**
`G_No[2] == 3` — i.e. that `Game2_0()` demonstrably ran — and reports
`SCRD_GAME_INIT_NO_MATCH_START` when no such pair exists. `main.c` turns that
into **exit code 2**, kept distinct from 1 (= engine divergence) so a sweep
never reads a segmentation artifact as a worklist item. Publication gates that
test `rc == 0` are unaffected.

**Measured, 16-segment corpus** (`/Volumes/KimchDrive/3sarm-convert-tmp/rerun2`,
`build/statcheck-verify`, before/after on the same binary tree):

| segment | before | after |
|---|---|---|
| `5743 game_2` | `Game_timer (0) != 6501` @ frame 1, rc=1 | `NO-MATCH`, rc=2 |
| `7733 game_2` | `Game_timer (0) != 6535` @ frame 1, rc=1 | `NO-MATCH`, rc=2 |
| the 5 segments that passed | PASS | PASS, **identical** compared-frame ranges |
| the other 9 failures | (see table above) | unchanged: same file, same line, same values, same frame |

The `start_index` the new predicate picks is **1 on all 14 segments that
contain a match** — byte-identical to the old scan — and undefined (correctly)
on the two that do not.

### H2 — the stage is never imported, so the appearance table is wrong

`appear_data_init_set()` (`appear.c`) picks
`app_type_tbl[own][opp][bg_w.stage]`, and `appear_data_set()` writes both
`wu.routine_no[4]` and `wu.xyz[0].disp.pos` from that entry. `ScrdGame_Init()`
reads characters, supers, new_challenger and colours — **never the stage**. On
the arcade the stage carries across matches, so the harness's synthetic
char-select cannot reconstruct it.

Solved uniquely against the shipped tables: one failing archive's `rno` occurs
at stage 7 only, another's at stage 12 only. `routine_no[3]` 3-vs-1 is the
downstream symptom via `Appear_01000()`'s `Appear_flag` branch. It usually does
not bite because `app_type_tbl` is stage-flat for most matchups — which is why
most games pass. **Fix:** import `bg_w.stage`; there is no CPS3 offset for it in
`arcade_constants.h` today.

### H3 — lever counters are never cleared (FIXED)

`t_pl_lvr` (`cmd_data.c`) is a plain global that nothing clears at match start —
`System_all_clear_Level_B()` does `Bg_Close()` + `effect_work_init()` only, and
neither `Game2_0()` nor `Game2_2()` (`game.c`) mentions it. An archive can open
with `s1_cnt` already at 16 from a pre-match button hold; ours starts at 0.

**Why the warm-up could never have fixed it.** `read_input_buff()`
(`statcheck_runner.c`) feeds our engine the archive's own button word every
frame, so once both sides are holding the same button they increment in
lockstep — and a constant offset incremented in lockstep stays constant
forever. Lengthening the 5-frame warm-up in `Statcheck_CompareValues()` would
only have moved the failure later. Measured on `7733 game_6`: the archive
enters with p0 `sw_lvbt = 0x0170` and `s1_cnt/s2_cnt/s3_cnt/s5_cnt =
16/16/17/15`, all running +1/frame; the report was `s1_cnt (6) != 22` at
archive frame 7 — the 16-count head start, six frames on. (This corrects the
earlier framing of this item: the warm-up length was never the defect.)

**Fix (this commit).** `Statcheck_SyncValues()` now seeds `t_pl_lvr` from the
pre-game frame with the existing `read_t_pl_lvr()` reader, exactly as it
already seeds `players_timer` and for the same reason — one import, after
which both sides advance identically. No new CPS3 offset was needed:
`T_PL_LVR_OFFSET` was already in `arcade_constants.h` because the *compare*
side reads it. This is the import upstream had sketched and left commented
out (see this file's header comment).

**Measured, 16-segment corpus** (same corpus and build tree as H1):

| segment | before | after |
|---|---|---|
| `7733 game_6` | `s1_cnt (6) != 22` @ frame 7, rc=1 | **PASS**, compared frames 1..14874 of 14875 |
| the 5 segments that passed | PASS | PASS, identical compared-frame ranges |
| the other 8 failures | (see table above) | unchanged: same file, same line, same values, same frame |

A 14,875-frame segment going clean end to end — not merely past frame 7 — is
what says the seed is the right value rather than a papered-over symptom.

### H4 — segment count below `num_matches` is correct, not a bug

The runner cuts a new `game_N` only when `G_No[1]` stops being 2, and writes no
frames while out of game. But `Game2_2()` (`game.c`) re-initialises a whole new
match — `Game_timer = 0; Round_num = 0; C_No[0..3] = 0` — **without leaving**
`G_No[1] == 2`. A rematch therefore merges two matches into one `game_N`.
Verified: a 2,270-frame segment whose successor carries `Game_timer` 2,226.

---

## Detection coverage

### D1 — what the on-device viewer can and cannot see

The `.3sr` carries one 32-bit djb2 per checkpoint over a 13-field window, not
per-field values (`docs/3sr-format.md`). The window is built by
`gather_live_fields()` (`replay_player.c`); producer side is `CHECKSUM_FIELDS`
in `make_3sr.py` and `kTrackChecksumOffsets` in `runner-track-3sr.patch`.

**In the window:** `C_No[0..3]`, `Game_timer`, `Random_ix16`, `Random_ix32`,
both players' `pos.x`/`pos.y`, `P1SW_0`/`P2SW_0`.

**Outside it** — and therefore undetectable on device unless it propagates in:
`vital_new` (so **E1 is invisible to the shipped viewer**), `routine_no`,
`T_PL_LVR` including `s1_cnt`, `G_No[1..3]`, `Counter_hi/low`, `cmb_stock`,
`mvxy`, `super_arts.*`, `hit_stop`, `dm_stop`, `waza_work`, `wcp`.

So of the three confirmed defects, only **E2 and E3 are on-device detectable**.

Checkpoint interval is 60, verified three ways (`DEFAULT_CHECKSUM_INTERVAL`,
`kTrackChecksumInterval`, and all 507 device `.3sr` headers parsing as
`('3SR1', 1, 60, …)`). `check_checkpoint()` additionally skips any checkpoint
that is not a battle frame (`G_No[1]==2 && G_No[2]==1`) — logged, never failed —
and stops advancing once `game_ended()`, so a post-KO tail is never checked.

`recover_random_ix16()` (`replay_player.c`) is the only recovery, and it sweeps
field index 5 alone. It does not mask other fields: sweeping all 65,536 values
yields only 8,671 distinct hashes, so a hash differing for any other reason is
accepted with probability ≈2.0×10⁻⁶.

`REPLAY_PLAYER_DESYNCED` is assigned in exactly one place — the tail of
`check_checkpoint()`. No retry, no swallow, no build flag disables checking.

### D2 — the viewer never rescanned (FIXED, `c6a75572`)

`rs_scan()` (`replay_shuffle.c`) has exactly one call site:
`ReplayShuffle_Tick()`, `case RS_WAIT_BOOT`, which transitions straight to
playing and never returns. **There is no rescan path.**

Observed consequence: a 24-hour session scanned 13 files from 4 quarks at boot,
then played 734 replays — 56 loops of the same 13 — while 507 `.3sr` sat on the
card, having arrived 12.5 hours after the scan. Zero divergences logged, and
that number said nothing about the 494 files never opened.

**Fixed in `c6a75572`:** `rs_scan()` now re-runs when `manifest.json`'s mtime
changes, at the RS_TRANSITION -> `rs_start_next()` boundary — the player is
terminal, no `.3sr` is loaded, and `s_current` is about to be reassigned, so
rebuilding the entry arrays cannot invalidate anything in use. Gated on the
manifest because the wrapper renames it into place once per completed fetch, so
a half-fetched set cannot trigger a scan.

**Confirmed by experiment 2026-09-04.** After a manual restart the scan reported
`found 507 entr(ies), 507 playable` and the detector logged a real desync within
13 replays — `1788331393445-5699/game_4` at frame 1980, checkpoint 34/111,
`live=6618483e want=ffcaf3a8`. The checkpoint immediately before it read
`Random_ix16-only divergence: live=002d archive=002c; resynced`, i.e. E2 drifting
and being repaired, then failing 60 frames later. **The detector works.**

**Still true regardless: any "N hours clean" claim must be qualified by what the
boot scan found.** Check the `replay-shuffle: scan of … found N entr(ies)` line
in `logs/last-run.log` before quoting a clean run as evidence.

### D3 — the VPS does not validate conversions at all

The deployed `fcade-proxy.js` contains zero occurrences of "statcheck", there is
no statcheck binary on the box, and no `publish_3sr.py`. Only the Mac worker
gates on statcheck — 137 of 5,051 conversions (2.7%). **The shipped corpus is
~97.6% unvalidated**, so on-device play is the only thing exercising those files
against our engine.

Corollary: per-game statcheck drops are not why games go missing from a shipped
quark. Missing games have some other cause (H4 accounts for the count
mismatch, not for gaps).

---

## Worklist

| id | what | state |
|----|------|-------|
| E1a | `pow_pow.c` ignores `Round_Level` in VS | external patch + seed netplay to **3** (not 0) — netplay question ANSWERED from archives |
| E1b | port never updates `Round_Level` in VS while the arcade ramps it down | OPEN, no patch; E1a alone is insufficient |
| E2a | `effect_G9_init()` spawn phase / `players_timer` | **oracle FIXED** `f63507b7` (drift 322/174/255/418 -> 0); **viewer FIXED** via `.3sr` v2 (host A/B: 31 -> 0 and 13 -> 0 `r16_resyncs`); NOT yet tested on the device, and existing v1 files keep the old behaviour |
| E2b | a `random_32` consumer the port never runs | OPEN, unidentified; this is the one that fails statcheck |
| E3 | `pos.x` +32 at round start | no patch; mechanism unknown, reproduces reliably |
| H1 | `ScrdGame_Init` post-KO false positive | **FIXED** — require `Game2_0()`'s `Game_timer=0`/`G_No[2]=3`; matchless segments exit 2, not 1 |
| H2 | stage not imported | add a CPS3 offset for `bg_w.stage` |
| H3 | lever counters never cleared | **FIXED** — seed `t_pl_lvr` in `Statcheck_SyncValues` like `players_timer`; the warm-up was never the defect |
| D1 | `vital_new` outside the hash window | E1 is undetectable on device by design — decide whether to widen |
| D2 | no rescan path | **FIXED** `c6a75572`; verified on device (13 -> 507 entries, desync detected) |

**For a reviewer:** E2b is now the highest-value target — it is on-device
detectable, reproduces deterministically, and RNG divergence compounds. E1 is
the best understood but needs the netplay decision first. H1–H3 should land
before any broad statcheck sweep, or the sweep's output cannot be trusted.
