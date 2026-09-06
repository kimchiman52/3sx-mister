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

**Both `Random_ix16` masks were removed on 2026-09-05**, which is the newest
work here. The oracle no longer force-syncs the field (it asserts it) and the
viewer no longer repairs it on v2 files. See M1/M2 in the worklist, "The
instrumentation" for what the oracle change cost and bought, D1 for the viewer
half, and **E5** for the seven divergences that were hiding behind the oracle's
mask. Read any `Random_ix16` claim dated earlier in this document as a statement
made under the mask. **E5 is now fixed** — it was two port defects in the
screen-quake writers, not the unimported state it looked like — and **E4 is now
fixed** too, a single misassociated `/ 2` in `cal_move_dir_forecast`. With both
masks off the 143-segment corpus is **143 PASS / 0 FAIL**.

**Status (2026-09-05).** Fixed: **E2a** in the statcheck oracle (`f63507b7`)
and now in the shipped viewer too (`.3sr` v2 carries `players_timer`); **D2**
on the viewer (`c6a75572`, confirmed on hardware); **E1a** applied (the
`Play_Type` damage pin, gated on arcade balance) with **E1b RETRACTED** — see
E1. **H1, H2, H3 and H4b** have all landed, and the 16-segment corpus now
stands at **6 PASS / 0 divergence / 8 rejected as unreproducible (rc=3) /
2 no-match (rc=2)**. Every one of the eleven failures this document opened with
had a harness cause: **E2b** and **E3** are both RETRACTED — they were
human-vs-CPU recordings replayed under the wrong `Play_Type`, which H4b now
rejects by type rather than reporting as divergence. Open: E2a's viewer half is
untested on hardware; E3's actual `+32` write was never identified, only shown
to be unreachable in human-vs-human play; and the usable corpus is down to six
segments. Nothing here has had a Fable review yet. **E5 and E4 are both fixed
as of 2026-09-05** — E5 two port defects in the screen-quake writers, E4 one
misassociated `/ 2` in `cal_move_dir_forecast`; both host-only, neither device
tested. The 143-segment corpus is **143 PASS / 0 FAIL**, with no engine
divergence outstanding.

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
- **It asserts `Random_ix16` honestly, as of 2026-09-05.** For as long as the
  oracle had existed it instead force-synced the field every frame — the comment
  "This is dirty, but syncing Random_ix16 every frame helps avoid
  animation-related desyncs" in `compare_service_values()` — so `Random_ix16`
  divergence was invisible by construction and a clean statcheck said nothing
  about it. That line is now `assert_equals(Random_ix16, random_ix16_cps3)`,
  alongside the `Random_ix32` assert that was always honest. **Verdicts recorded
  in this document BEFORE that date carry the old, weaker meaning.** What the
  removal surfaced is E5.
- **The same force-sync still stands in `src/test/test_runner_compare.c`
  (`compare_service_values`, the DEBUG replay comparer), and it is harmless:**
  its `compare_values()` and `sync_values()` have **zero callers in `src/`** —
  `test_runner.c` includes the header and calls neither. A mask over code
  nothing runs hides nothing, so it was left alone rather than changed
  unverifiably; there is no corpus that exercises that path to re-grade against.

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

**Why the delta is exact.** Each frame begins synced, both sides advance one per
call, and the generator masks to six bits (`Random_ix16 &= 0x3F`). Therefore

    (our Random_ix16 - archive Random_ix16) & 0x3F

IS (our calls - CPS3's calls) for that frame, for any true difference under 64.

**What guarantees "begins synced" changed on 2026-09-05, and the guarantee got
stronger.** It used to be the force-sync: every frame was re-synced by fiat, so
every frame began equal — and no frame could ever fail. Now it is the assert on
the very next line: a frame only reaches the compare because its predecessor
asserted equal, and the first frame that does not ends the run. So the delta is
still exactly one frame's call difference, it is reported immediately before the
failure it explains, and there is exactly one report per failing run. The
measurement kept its precision and stopped being a mask. (`STATCHECK_RNG_DRIFT_MAX`
is now effectively unreachable for `Random_ix16` drift; it still bounds nothing
else.)

**Limits.** It names only OUR call sites. A negative delta means CPS3 called
something we did not, and that answer is in the arcade disassembly. **And it is blind
to reordering**: if both engines make the same number of calls on a frame but in
a different order, the delta is 0 and nothing is reported, while the two sides
still consume different table entries. That is exactly how E3 hid — see E3. Reports are
capped (`STATCHECK_RNG_DRIFT_MAX`) so a run that drifts every frame cannot bury
the first divergence.

Usage:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  build/statcheck-verify/3S-ARM.app/Contents/MacOS/3S-ARM --ram-archive <x.scrd> 2>&1 | grep statcheck-rng
```

---

## Confirmed engine divergences

### E1 — the `Play_Type == 1` damage pin is a PS2-ism (E1a, REAL but narrow; E1b RETRACTED)

**Status, 2026-09-05.** This section previously reported two defects. After a
disassembly of the arcade program and a re-reading of the archives with the
segments correctly classified, **E1a is real but demonstrates on nothing in the
corpus, and E1b does not exist.** Both halves of the earlier write-up rested on
an assumption that turned out to be false — that all 16 corpus segments are
human-vs-human. They are not.

#### The mechanism

`cal_damage_vitality()` and `cal_damage_vitality_eff()` (`pow_pow.c`) select the
damage scale with:

```c
if (Play_Type == 1) { yy = Pow_Control_Data_1[0][3]; }
else                { yy = Pow_Control_Data_1[0][Round_Level]; }
```

`Pow_Control_Data_1[0]` (`pow_data.c`) is `{90,95,98,100,103,106,109,112}` — a
percentage scale whose **index 3 is exactly 100**. So the port pins neutral
damage for human-vs-human play. `Play_Type == 1` means both players are human
(`Setup_Play_Type()`, `sys_sub.c`).

#### E1a — the pin has no arcade counterpart (MEASURED, by disassembly)

The decisive evidence is the arcade program itself, not an archive. Working from
the decrypted 8 MB big-endian SH-2 image of `sfiii3nr1` (built exactly as
`docs/research-arcade-cg-data-accuracy.md` §23.4 describes; both of that
section's anchors re-verified — `random_16` at CPS3 `0x0611E0EE` loading
`0x020155E8`, and `random_tbl_16` unique at `0x065EB434`):

- `Pow_Control_Data_1` is at **CPS3 `0x06194A2C`** — the 16-byte pattern
  `5A5F6264676A6D70 64686C6E70727478` occurs **exactly once** in the image.
  `Power_Data` is at `0x061946EC`.
- Its only two consumers are the adjacent, structurally identical routines at
  **CPS3 `0x0609E36C`** and **`0x0609E3FA`** — `cal_damage_vitality` and
  `cal_damage_vitality_eff`. They index the row **unconditionally with
  `Round_Level`**:

  ```
  0609E410  mov.l 0x609e4a8,r6   ; 0x06194A2C = Pow_Control_Data_1
  0609E414  mov.w 0x609e49c,r0   ; 0x03C0 (attacker struct offset)
  0609E416  mov.w @(r0,r4),r0
  0609E418  cmp/eq #15,r0
  0609E41A  bf/s 0x609e428
  0609E41C  mov.w @r2,r7         ; r2 = 0x0201137A -> r7 = Round_Level (delay slot)
  0609E420  add r7,r2
  0609E422  mov.b @(8,r2),r0     ; row 1: Pow_Control_Data_1[1][Round_Level]
  0609E42C  mov.b @r2,r2         ; row 0: Pow_Control_Data_1[0][Round_Level]
  0609E432  muls.w r2,r1
  0609E436  jsr @r3              ; 0x0612D428 = signed divide
  0609E438  mov #100,r0          ; (power * yy) / 100
  ```

  The one branch selects the **row**, not the index — both arms add
  `Round_Level`.
- **Negative proof.** Every `mov.l @(disp,PC),Rn` in `0x0609E340..0x0609E490`
  was enumerated: exactly four literals — `Power_Data` `0x061946EC`,
  `Round_Level` `0x0201137A`, `Pow_Control_Data_1` `0x06194A2C`, and the
  signed-divide helper `0x0612D428`. **`Play_Type` is never loaded there.**
- `Play_Type` is **CPS3 `0x020113B4`** (`s16` in the arcade; the port declares
  `u8`), pinned from arcade `Setup_Play_Type` at **`0x060914D4`**, which is
  byte-for-byte the port's (`Operator_Status[0] & 0x7F && Operator_Status[1] &
  0x7F` -> 1 else 0, `Operator_Status` at `0x020156E2`).

So the pin is a PS2-ism and the port is unfaithful. **But it is a no-op almost
everywhere**, because it only differs from the arcade when `Round_Level != 3` at
a moment when `Play_Type == 1`:

- `Before_Select_Sub()` (`game.c`; arcade `0x0609520C`, unguarded tail store at
  `0x0609531A`) ends with `Round_Level = 3`, and `Pow_Control_Data_1[0][3] ==
  100` is exactly what the pin produces;
- neither `Update_VS_Data()` nor `Loser_Sub()` moves `Round_Level` while
  `Play_Type == 1` — on hardware either (see E1b below).

The reachable divergence is therefore **a second player breaking into a 1P game
that has already moved the cabinet's level**. That path was not traced, so how
often it is reached is **not established**.

**What the corpus says: nothing, and it cannot.** All six human-vs-human
segments run at `Round_Level == 3` for every frame (table below), where the pin
and the arcade agree to the value. **No segment in the corpus shows a
`vital_new` divergence at all** — the 8 divergences are 6 × `Random_ix32` (E2b)
and 2 × `pos.x` (E3). The "five `vital_new` failures" this section used to cite
as E1's symptom are not reproducible here and were, on the evidence below, a
harness artifact.

Applying the change is measurably inert on the corpus: a before/after sweep of
all 16 segments (baseline build vs. patched build, same archives, same binary
flags) produced **byte-identical** result lines — 6 PASS / 8 divergence /
2 no-match either way.

**Fix applied:** `if (Play_Type == 1 && !ArcadeBalance_IsEnabled())` at both call
sites, plus `setup_vs_mode()` (`netplay.c`) seeding `Round_Level = 3` rather than
`0` — mandatory in the same change, because `Pow_Control_Data_1[0][0] == 90`
would otherwise cut every netplay hit by 10%.

#### E1b — RETRACTED. The arcade does not move `Round_Level` in 2P play

The earlier claim was that the arcade ramps `Round_Level` down during a 2P
session while the port never does. **Both halves of the arcade's gating are
identical to the port's:**

| arcade site | action | guard |
|---|---|---|
| `Loser_Sub` `0x0609C5E4`, store `0x0609C61E` | `--Round_Level`, clamp 0 | `Play_Type == 0` (test at `0x0609C616`) |
| `Update_VS_Data` `0x0609C750`, store `0x0609C884` | `++Round_Level`, clamp 7 | `if (Play_Type != 0) return` at `0x0609C79A` |

A whole-image scan for the big-endian literal `0201137A` finds **6 aligned pool
entries, 7 code loads, 5 writers and 2 readers** — the two writers above,
`Before_Select_Sub` (`0x06095234` `= 7` under `Demo_Flag == 0`, `0x0609531A`
`= 3`) and the demo init (`0x06097654` `= 7`); the damage pair is the only
reader in the entire program. (The scan cannot exclude an `@(R0,Rn)` indexed
write or a bulk clear.) Every one of those matches the port's own writers
(`game.c` `Round_Level = 7` / `= 3` in `Before_Select_Sub()`, `demo02.c`
`Round_Level = 7`, `manage.c` `Update_VS_Data()` / `Loser_Sub()`).

**So the port is already faithful, and nothing is to be written for E1b.**

#### Why the archives said otherwise: the segments are not all human-vs-human

The retraction turns on a fact the earlier reading did not have.
`plw[i].wu.wu_operator` is at WORK offset 3 — archive `0x68C6F` and `0x69107`
(`PLW_OFFSET 0x68C6C`, `PLW_SIZE 0x498`). Read across all 16 corpus segments it
partitions them:

| quark | seg | dominant `wu_operator` | `Round_Level` seen | statcheck (this tree) |
|---|---|---|---|---|
| 3455 | g0 | (1,1) human-human | 3 | PASS |
| 3455 | g1 | (1,0) human-CPU | 3 | FAIL `Random_ix32` @359 |
| 3455 | g2 | (1,1) human-human | 3 | PASS |
| 5743 | g0 | (1,1) human-human | 3 | PASS |
| 5743 | g1 | (0,1) human-CPU | 3 -> 2 | FAIL `Random_ix32` @301 |
| 5743 | g2 | (0,0) attract | 2 | no-match (rc=2) |
| 5743 | g3 | (0,1) human-CPU | 3 | FAIL `Random_ix32` @234 |
| 7733 | g0 | (1,1) human-human | 3 | PASS |
| 7733 | g1 | (1,0) human-CPU | 3 -> 2 | FAIL `Random_ix32` @322 |
| 7733 | g2 | (0,0) attract | 2 | no-match (rc=2) |
| 7733 | g3 | (0,1) human-CPU | 3 -> 2 | FAIL `Random_ix32` @247 |
| 7733 | g4 | (1,0) human-CPU | 2 -> 1 | FAIL `Random_ix32` @247 |
| 7733 | g5 | (1,0) human-CPU | 3 | FAIL `pos.x` @54 |
| 7733 | g6 | (1,1) human-human | 3 | PASS |
| 1710 | g0 | (1,1) human-human | 3 | PASS |
| 1710 | g1 | (1,0) human-CPU | 3 -> 2 | FAIL `pos.x` @54 |

The partition is exact, and three things fall out of it that corroborate each
other:

1. **Only 6 of the 16 segments are human-vs-human.** Eight are human-vs-CPU and
   two are attract demo. The old "16 games of 4 human-vs-human sessions" framing
   was wrong, and everything E1 used to claim rested on it.
2. **`wu_operator` predicts the statcheck verdict perfectly.** All six (1,1)
   segments PASS; all eight human-vs-CPU segments diverge; both (0,0) segments
   are the two no-matches. That is the 6 / 8 / 2 tally, explained.
3. **Every `Round_Level` change happens in a non-(1,1) segment**, and every
   (1,1) segment holds `Round_Level` constant at 3 — exactly what the arcade
   `Play_Type == 0` gate predicts. The archives corroborate the disassembly
   rather than contradicting it.

The harness taps `SWK_START` for player 2 at `PHASE_CHARACTER_SELECT`
(`src/test/statcheck_runner.c`), and `ScrdGame_Init` never imports
`wu_operator`, so **every statcheck run has two human operators and therefore
`Play_Type == 1`**, including on the eight segments the cabinet ran at
`Play_Type == 0`. On those, hardware used `Pow_Control_Data_1[0][Round_Level]`
while the port used index 3 — a `vital_new` off-by-one with no engine defect
behind it. That is the harness artifact the old E1 symptom description was
measuring. (Fixing the harness properly means importing `Operator_Status` /
`wu_operator`; that is a separate item and is not done here.)

#### Where the decrements actually land (measured, kept because it was asked)

For the five segments that do step down, the transition frame is identical in
all five: `C_No` goes `[7,0] -> [7,3]` on the changing frame, with
`G_No == [1,2,1,1]`, one player at `vital_new == -1`, and it never recurs in
that segment. Mapping through `Management_Jmp_Tbl[C_No[0]]` (`manage.c`, index
`n` is `Game_Manage_(n+1)th`), `C_No[0] == 7` dispatches `Game_Manage_8th` ->
`Game_Manage_8_0()`, whose first statements are `Round_num++; Quick_Entry();`
and which then writes `C_No[1] = 3` — so the arcade's decrement is
`Quick_Entry()` -> `Loser_Sub()` on exactly that frame. **Not mid-fight, and not
at the internal rematch boundary** (5743 g1's round-1 end at f3677/f3678 shows
the same `[7,0] -> [7,3]` step with `Round_Level` unchanged, because
`Quick_Entry()` returns early until `PL_Wins[Winner_id] >=
save_w[Present_Mode].Battle_Number[Play_Type] + 1`). This is the port's
structure exactly; it is recorded here only to close the question, since the
port already has the same behaviour under the same guard.

#### Statcheck support (applied)

`ROUND_LEVEL_OFFSET 0x1137A` is now in `src/arcade/arcade_constants.h` and
imported once by `Statcheck_SyncValues()` (`statcheck_compare.c`) alongside
`players_timer` and `t_pl_lvr`. One seed is sufficient: the value is constant
across every frame in which damage is dealt, and the only in-segment change is
the match-end decrement above. The offset was previously taken on trust from an
external fork; it is now **confirmed by disassembly** (see the address table in
E1b).

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

**Not fixable by a FAILING archive, by construction — while the mask stood.** A
G9 phase error only moves `Random_ix16`, which the oracle overwrote every frame,
so it could never make statcheck FAIL. Its whole cost was downstream: the device
viewer's `recover_random_ix16()` recoveries (288 in 13 replays). Both masks were
removed on 2026-09-05 — the oracle now asserts `Random_ix16`, and the viewer
repairs it only on v1 files. See "The instrumentation" and D1.

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

### E2b — RETRACTED: the "consumer we never run" is the CPU player

**This was never an engine defect.** It is the same root cause as **H4b**.

**What it looked like.** Three archives failed identically on their first
RNG-drift frame: `delta=-2 … 0 RNG call(s) this frame`, immediately followed by
`Random_ix32 (N) != cps3 (N+1)`. CPS3 burned +2 `random_16` and +1 `random_32`
where we burned nothing for 40 frames either side.

**What it is.** On every one of those segments the recording had a **CPU
player**. The failing frame is the frame `plw[0..1].wu.routine_no[0]` goes
`3 → 4` — the "FIGHT!" frame, one after `Allow_a_battle_f` flips. `CPU_Sub()`
(`com_pl.c`) returns 0 while `Allow_a_battle_f == 0`, which is exactly why the
AI's first draw lands there and not earlier. On that frame `CP_No[id][0] == 0`,
so `Main_Program()` dispatches `Com_Initialize()`, which calls in order:

1. `Setup_Next_Stand_Timer()` → `random_16_com()` (`ck_pass.c`)
2. `Setup_Next_Squat_Timer()` → `random_16_com()` (`ck_pass.c`)
3. `Setup_Bullet_Counter()` → `random_32_com()` (`com_pl.c`)

With `Play_Mode == 0` those delegate to `random_16()`/`random_32()` (`pls02.c`).
**Exactly +2 / +1.** Our engine never runs them because `Player_move()`
(`plmain.c`) only reaches `cpu_algorithm()` when `wu.wu_operator == 0`, and the
harness makes both players operators.

**The discriminator, and it is perfect.** `plw[i].wu.wu_operator` (WORK offset 3;
archive `0x68C6F` / `0x69107`):

| group | `wu_operator` | segments | statcheck |
|---|---|---|---|
| human vs human | (1,1) | 6 | **all PASS** |
| human vs CPU | (1,0)/(0,1) | 8 | **all FAIL** |
| attract demo | (0,0) | 2 | NO-MATCH |

Independently confirmed on `5743 game_1`: the P1 lever word (`wcp`, `0x26318`)
disagrees with the P1 cabinet switch (`P1SW_0`, `0x6AA8C`) on **814/1201
(67.8%)** battle frames, against a 10–17% latch baseline in the both-human twin.
P1 is machine-driven.

**Supporting disassembly.** `random_32` = `0x0611E0D6`, 21 call sites image-wide;
18 sit in `0x06006xxx–0x060136xx`, matching the port's 18 `random_32_com()` sites
across `com_pl.c` (8), `com_sub.c` (9), `ck_pass.c` (1). `random_tbl_32_com` and
`random_tbl_16_com` do **not** occur anywhere in the CPS3 image — the `_com`
tables are a PS2 addition, and on hardware the AI calls `random_32`/`random_16`
directly, which is precisely what `random_*_com()` does when `Play_Mode == 0`.
`Random_ix32` (`0x020155EA`) has only three literal referrers and no writer
besides `random_32`, so the +1 had to be a real call.

**Ruled out along the way**, both real stubs, neither the cause:
`setup_effK4()` (`effk4.c`) is an empty stub and CPS3's `effect_K4_move` burns
1×`random_32` + 6×`random_16` per spawn — but K3/K4 are bonus-stage debris
(`_bonus_char_table`), unreachable in a VS match. Worth its own entry.
`pli_0002()` (`plcnt.c`) is a documented stub, but runs several frames before the
`3→4` transition and CPS3's effect M4 contains no RNG.

### E4 — a misassociated `/ 2` in `cal_move_dir_forecast` (FIXED and GATED, 2026-09-05)

**GATED on `ArcadeBalance_IsEnabled()`.** Everything below describes the arcade
form and the evidence for it. PS2 keeps `(d.sp * (tm * tm)) / 2`, the form this
decompilation has always emitted: the arcade association was proven against the
CPS3 program and never against the PS2 binary, and this port's baseline is the
PS2 decompilation. See "SETTLED: every CPS3-derived engine change is gated".


**The first genuine engine divergence found in a verified human-vs-human
segment since E2a**, and the reason the corpus was widened. It is one operator
precedence error in one line, and it is device-visible.

```
1788423525221-5107 game_0   Coccis77 vs Z3rog   Chun-Li (P1) vs Dudley (P2)
statcheck_compare.c:375: routine_no_3sx (23) != routine_no_cps3 (20)
statcheck: FAIL at archive frame 3090
```

#### The defect

`cal_move_dir_forecast()` (`engine/caldir.c`) predicts where a work will be
`tm` frames from now under constant acceleration and asks
`caldir_pos_032()` for the 32-way direction from here to there. The port had:

```c
ps[0].dp = (wk->mvxy.d[0].sp * (tm * tm)) / 2;
```

Every caller passes **`tm == 5`**, so `tm * tm` is the **odd** number 25, and
the two ways of associating the halving are not the same function:

| | acceleration term |
|---|---|
| port (was) | `trunc(d.sp * 25 / 2)` |
| arcade | `d.sp * 12` |

They differ by about `d.sp / 2` — half a unit of acceleration, per axis, per
forecast.

**The arcade halves the square once, before either multiply.**
`cal_move_dir_forecast` is CPS3 `0x06090E1C`. The only two sites in the
decrypted image that reach it — by constant-pool literal or by `bsr`, both
searched exhaustively — are `check_buttobi_type` (`0x0611E926`) and
`check_buttobi_type2` (`0x0611EA24`), whose `mov.l` literals are `dir32_skydm`
`0x065EB724` and `dir32_grddm` `0x065EB764`. With r13 = `tm` and r14 = `wk`:

```
06090e40  mul.l  r13,r13     ; macl = tm * tm
06090e4a  sts    macl,r5
06090e50  cmp/gt r5,r3       ; r3 = 0  ->  T = (r5 < 0)
06090e52  addc   r3,r5       ; r5 += (r5 < 0)
06090e54  shar   r5          ; r5 = (tm * tm) / 2, signed  -> 12
06090e58  mul.l  r2,r5       ; r2 = mvxy.d[0].sp, WORK +0x84
06090e78  mul.l  r3,r5       ; r3 = mvxy.d[1].sp, WORK +0x88, the SAME r5
```

`cmp/gt` + `addc` + `shar` is the compiler's signed divide-by-two, and it is
applied to `tm * tm` **on its own** before either `mul.l`. That is what pins the
association: the source halved the square, not the product. Both axes then reuse
the one halved square, so the fix computes it once as `half_tm_squared`.

The rest of the function is a faithful port and was checked instruction by
instruction against `0x06090E40..0x06090EA6`: the WORK offsets it indexes are
`+124`/`+128` (`mvxy.a[0]`/`a[1]`), `+0x84`/`+0x88` (`mvxy.d[0]`/`d[1]`) and
`+100`/`+104` (`xyz[0].cal`/`xyz[1].cal`), and it ends with
`caldir_pos_032(xyz[0].disp.pos, xyz[1].disp.pos, ps[0].rp.h, ps[1].rp.h)`.

#### The evidence chain, in the order it was walked

1. **The failing slot is a damage-reaction selection.** `routine_no[1] == 1`
   is the damage arm of `plmain_lv_02[]` (`plmain2.c`), and `routine_no[2] == 88`
   is the sky-knockdown request that `get_damage_reaction_data()` (`plpdm.c`)
   resolves in one frame:
   `routine_no[2] = check_buttobi_type(wk)`, then
   `wk->as = &dm_reaction_table[routine_no[2]]; routine_no[2] = wk->as->r_no;`.
   In the port's `dm_reaction_table`, `[97] = {20, 9, 0}` and `[98] = {23, 10, 0}`
   are **the only two of the 115 entries that produce 20 and 23** — so the
   archive took index 97 and we took 98. `dir32_skydm` (`pls02.c`) maps direction 11-14 and 18-21 to 97 and
   **15-17 to 98**, so the whole divergence is one direction bucket.
2. **Every input to that direction was byte-identical.** A trace in
   `check_buttobi_type` printed, at the failing frame, `dm_plnum=15`
   (Chun-Li, the attacker), `dm_butt_type=4`, `weight_level=1`,
   `xyz[0].cal=46596096`, `xyz[1].cal=618496`,
   `mvxy = a0 -163840, d0 0, a1 -393216, d1 -28672, kop 1/0`. The archive holds
   **exactly those values** for P2 at `PLW_OFFSET + PLW_SIZE`
   (`WORK_XYZ_OFFSET 0x64`, `WORK_MVXY_OFFSET 0x7C`) on frames 3089 and 3090.
   So `parabora_own_table`, `dm_butt_type`, `weight_level` and the position
   accumulators were all ruled out by measurement, not by argument.
3. **The tables were ruled out against the ROM.** `dir_sel_table[128][128]`
   (`caldir.c`) occurs exactly once in the decrypted image, at CPS3
   `0x0618F664`, and all **16,384 bytes are identical**; its only two literal
   referrers are `caldir_pos_256` (`0x0609016C`) and `caldir_pos_032`
   (`0x06090270`), both of which disassemble to the port's code exactly,
   including the two constants the port writes as `0x80` and `0xFF`
   (`mov.w` literals at `0x060902A4` / `0x060902A6`). `dir32_skydm` and
   `dir32_grddm` are byte-exact at `0x065EB724` / `0x065EB764`.
4. **That left only the arithmetic**, and the arithmetic is where it was.
   With the port's association, `ps[1] = trunc(-28672*25/2) + (-393216*5) +
   618496 = -1705984`, high word **-27**; with the arcade's,
   `-28672*12 + (-393216*5) + 618496 = -1691648`, high word **-26**. From
   `(711, 9)` that is `(dx, dy) = (-13, -36)` versus `(-13, -35)`, and
   `dir_sel_table[13][36] = 11` versus `dir_sel_table[13][35] = 12`:
   `tent` 139 versus 140, `(tent + 4) >> 3` = **17** versus **18**,
   `dir32_skydm` = **98** versus **97**, `r_no` = **23** versus **20**.
   One pixel of forecast, one bucket, one damage reaction.

#### It is not Dudley-specific, and it is not rare — the *consequence* is rare

Measured by running the fixed engine over all 143 segments with both
associations computed side by side and the disagreements logged:

| | count |
|---|---|
| `cal_move_dir_forecast` calls in the corpus | **764** |
| calls where the two associations give a different 32-direction | **11** (1.4%), across 10 segments |
| of those, calls where the different direction changes the table value | **1** |

The 11 bucket flips span four victims (Chun-Li, Urien, Necro, Dudley) and five
attackers (Yun, Yang, Dudley, Chun-Li, Necro) — **the defect is
character-agnostic**, as the arithmetic says it must be. What is rare is landing on a table edge:
`dir32_skydm` and `dir32_grddm` are piecewise constant with long runs, so ten of
the eleven flips (26->27, 29->30, 30->31) stay inside a run of 88 or 91 and
change nothing. Only the E4 event flipped 17->18, which is the boundary between
the `98` run (15-17) and the `97` run (18-21). That is the whole reason 142 of
143 segments passed with the defect in place.

The same measurement, keyed on the return address, shows all 764 calls came from
`check_buttobi_type` (755, carrying all 11 flips) and `check_buttobi_type2` (9),
and **none** from `remake_initial_speeds` (`plpdm.c`) — the corpus never
exercises the third call site or its `dir32_guard_air` table.

#### It is device-visible

`routine_no[2]` is outside the viewer's 13-field checkpoint window
(`gather_live_fields()`, `replay_player.c`), so the divergence itself is
oracle-only. It does not stay there. Re-running the **pre-fix** binary on this
archive with the oracle's `stop_if` made non-fatal, so the run continues past
the first mismatch:

```
plw[1] routine_no[2]        first diverges at archive frame 3090   ours 23      cps3 20
plw[1] mvxy.a[0].sp                                      3100      -81920      -40960
plw[1] xyz[0].disp.pos                                   3104      694         695
```

(all three are P2's, the victim: the archive holds `-40960` for
`plw[1].wu.mvxy.a[0].sp` at frame 3100 and `695` for `plw[1].wu.xyz[0].disp.pos`
at 3104, while P1's values still match ours.)

`plw[1].wu.xyz[0].disp.pos` is **field index 9 of the checkpoint window**, and
it stays divergent for hundreds of frames after (3539, 3540, 3820, ...). So this
was a real on-device desync, not an oracle curiosity: `r_no` 20 and 23 are
different damage reactions with different `char_ix` (9 versus 10), so the victim
plays a different knockdown and lands somewhere else.

#### What the fix cost and bought

143-segment sweep, `build/statcheck-verify`, same binary tree, `--headless`:

```
before   142 PASS / 1 FAIL   (E4)
after    143 PASS / 0 FAIL
```

Diffing the per-segment verdict line across the two sweeps, **the E4 segment is
the only line that changed** (`FAIL at archive frame 3090` ->
`PASS - compared archive frames 1..4538 of 5100`); the other 142 report a
byte-identical compared frame range and passed in both runs, so every field the
oracle compares is unchanged for them.

**Re-conversion consequence, same shape as E5's.** The fix changes
`plw[i].wu.xyz[0].disp.pos`, which *is* in the checkpoint window, on any replay
containing a bucket flip that changes the table value. Measured rate on this
corpus: **1 of 143 segments (0.7%)** — far smaller than E5's 7/143, and it needs
no `.3sr` format change, but a pre-fix `.3sr` for such a match will mismatch on
a post-fix build. Not decided here.

#### Still open

- **The device test.** Everything above is host-only.
- **The third call site is unlocated, not cleared.** The port's
  `remake_initial_speeds()` (`plpdm.c`) also calls `cal_move_dir_forecast`, and
  indexes `dir32_guard_air` with the result. No arcade caller of `0x06090E1C`
  other than the two named above exists by literal pool or `bsr`, and
  `dir32_guard_air` itself is not found in the image by exact search as `s16`
  or `u8` — but neither are `dm_reaction_table`, `ris_data_table` or
  `dm17_to_nm23_change`, three other `plpdm.c` tables, so **"not found" is weak
  evidence here and the honest word is unlocated, not different**. Nothing above
  depends on it: the return-address measurement shows `remake_initial_speeds`
  never ran on any of the 143 segments. It is named because a wrong row there
  would present exactly like E4 did, and because whatever makes `plpdm.c`'s
  tables unfindable is worth knowing before the next table argument is made.

### E3 — RETRACTED and now ROOT-CAUSED: a swapped RNG draw order

**The retraction stands, and is now proven rather than argued.** The earlier
evidence recorded here was wrong in kind and is corrected below.

**Mechanism.** An lldb hardware watchpoint on `plw[0].wu.xyz[0].disp.pos`
(condition `== 424`) gives:

```
comm_pa_x +44 <- char_move <- Appear_01000 <- Player_normal
              <- player_mv_2000 <- move_P1_move_P2 <- plcnt_init
              <- Player_control <- Game2_1
```

`comm_pa_x` (`charset.c`) is the walk-in script's "player add X":
`wk->xyz[0].cal += ctc->ix << 8`. So it is **accumulated script motion**, not a
direct write.

**Why the variant differs.** `Appear_01000` case 0 (`appear.c`) does
`work = random_16() & 3; set_char_move_init(&wk->wu, 9, work)` on
`Game_timer == 2`, and both players draw on that frame — so **draw ORDER decides
which player gets which index**. `move_player_work()` (`plcnt.c`) switches on
`plw[0].wu.wu_operator + (plw[1].wu.wu_operator * 2)`: `case 1`/`case 2` pin the
order, while `(1,1)` and `(0,0)` fall through to a `Game_timer & 1` arm. A
`(1,0)` recording therefore takes the pinned arm while our two-operator harness
took the parity arm, and Alex draws a different index:

| archive | Alex's index -> `&3` | arcade P0.x | ours |
|---|---|---|---|
| `7733 g5` (1,0) | `tbl[55]=3` -> **3** | 392 flat | `tbl[56]=12` -> **0**, 424 @54 |
| `1710 g1` (1,0) | `tbl[32]=5` -> **1** | 392 flat | `tbl[33]=4` -> **0**, 424 @54 |
| `5743 g0` (1,1) | `tbl[28]=1` -> **1** | 392 flat | same -> PASS |
| `7733 g6` (1,1) | `tbl[35]=2` -> **2** | **424** | same -> PASS |

Variants 1 and 3 hold at 392; variants 0 and 2 step +32. Note `7733 g6` is
**arcade-side 424 and passes** — the +32 is a legitimate walk-in variant, not a
defect in either engine.

**Corrections to what this document previously asserted:**
- "a single direct write to `xyz[0].disp.pos`, not accumulated motion" — **wrong**.
  It is accumulated script motion through `char_move`.
- the `set_field_hosei_flag` camera-clamp candidate — **wrong**, and dropped.
- "unreachable in (1,1) play" was right, but for the wrong reason: the
  `wu_operator`-conditioned *write paths* named earlier (`Appear_24000`/
  `Appear_25000`) never run — `routine_no[4]` is 1 and 21 on every frame 3-60.
  The real reason is the draw-order switch above.

**Limit this exposed in the instrument.** The RNG tracer is **blind to
equal-count reordering**: on the frame that matters, both engines make the same
number of `random_16()` calls, so the delta is 0 and nothing is reported. See the
caveat in the instrumentation section.

### E5 — two port defects in the screen-quake writers (FIXED and GATED, 2026-09-05)

**Both halves are GATED on `ArcadeBalance_IsEnabled()`.** Under PS2 the
`tad->hits == 0` branch still writes `bg_w.quake_y_index` before calling
`pp_screen_quake()` (E5a), and still reads `gqdt` with rows 7/8 at `{6, 0}`
(E5b). `pp_screen_quake()` is PS2 pad rumble, so E5a in particular may be
deliberate PS2 behaviour rather than a transcription slip — it was shown absent
from CPS3, never checked against the PS2 binary. E5b is implemented as a second
table plus a selector, `gqdt_active()`; see "SETTLED: every CPS3-derived engine
change is gated" for the shape and why. Everything below describes the arcade
form and the evidence for it.


**This is what removing the oracle's force-sync surfaced**, and it is the whole
of what it surfaced. Sweeping all 143 segments of
`/Volumes/KimchDrive/3sarm-corpus-2026-09-05/`:

| | segments | PASS | FAIL |
|---|---|---|---|
| `Random_ix16` force-synced | 143 | 142 | 1 (E4) |
| `Random_ix16` asserted (the mask off) | 143 | 135 | 8 (E4 + 7 E5) |
| **after the two fixes below** | 143 | **142** | **1 (E4)** |

E4 fails identically throughout — same archive, same archive frame 3090, same
`routine_no_3sx (23) != routine_no_cps3 (20)`. No previously-passing segment
started failing.

**The seven failures presented in two classes, and the stage was the
discriminator.** All were `Random_ix16 (n) != random_ix16_cps3 (m)` in
`compare_service_values()`, and the RNG tracer attributed every one to a single
call site:

| class | caller (`statcheck-rng:` trace) | delta | segments | 3SX stage | spawner |
|---|---|---|---|---|---|
| **E5a** | `eff19_quake_sub` (`effect/eff19.c`) | **+7** | 4 | 16 (Makoto's) | `bg1602_init00` -> `effect_19_init()` |
| **E5b** | `eff94_2000_1` (`effect/eff94.c`) | **+1** | 3 | 7 (Ibuki's) | `bg0702_init00` -> `effect_94_init(3)` |

The cohort sizes pinned the attribution: `effect_19_init()` spawns exactly seven
works (`for (i = 0; i < 7; i++)`) and BG070 calls `effect_94_init(3)` exactly
once. Both classes gate on **`bg_w.quake_y_index`** — `eff19_quake_sub` `case 0:`
returns early on `if (bg_w.quake_y_index <= 2)` and otherwise does
`work = random_16()`; `eff94_2000_0` advances `routine_no[2]` on
`if (bg_w.quake_y_index > 3)`, and the next tick `eff94_2000_1` draws.

#### The mechanism was NOT unimported state, and that was worth measuring

The obvious hypothesis — a seventh instance of "the arcade carries state across
a match boundary and our harness resets it", after E1 `Round_Level`, E2a
`players_timer`, H1, H2 `bg_w.stage`, H3 `t_pl_lvr` and H4b `wu_operator` — is
**wrong, and it is wrong in a way that would have made the oracle worse.**

Locating `bg_w.quake_y_index` in the archives (`BG_W_QUAKE_Y_INDEX_OFFSET`, see
below) and printing every frame where the two sides disagreed showed that on the
failing segments **the two agreed on every frame up to the failure**, both at 0,
and the port then wrote a quake the arcade never wrote. There was nothing to
import: the field enters every corpus segment at 0 on both sides. Seeding it in
`ScrdGame_Init` would have done nothing, and syncing it per frame in
`Statcheck_SyncValues` would have been the `Random_ix16` force-sync all over
again — a mask over a real divergence, hiding it in **48 of 143 segments**
instead of failing in 7.

#### Defect one (E5a): a quake write the arcade does not have

`effect_A7_move` (`effect/effa7.c`) and `effect_02_move` (`effect/eff02.c`)
both took a `tad->hits == 0` early-out in `case 0:` that read

```c
if (tad->quake != 0) {
    bg_w.quake_y_index = gqdt[tad->quake][1];
    pp_screen_quake(bg_w.quake_y_index);
}
```

**The arcade's version of that branch does the sound effect and nothing else.**
Read off the sfiii3nr1 SH-2 program instruction by instruction:

- `effect_A7_move` CPS3 `0x060F9194..0x060F94F8`. Its `hits == 0` branch runs
  `0x060F91EC..0x060F9220`: test `tad->se`, either call
  `sound_effect_request[se]` (via the literal `0x0618CCE4`) or store 0 to
  `Last_Called_SE`, then tail-jump to `push_effect_work` (`0x060DB8DC`). No
  store, no quake.
- `effect_02_move` CPS3 `0x060DC890..0x060DCC7E`. Same shape at
  `0x060DC918..0x060DC93E`, with `urian_guard_se_check` in place of the SE
  table, then the same tail-jump.
- Neither function's range contains a single reference to `&bg_w`
  (`0x02026BAC`). Each touches `bg_w.quake_y_index` exactly once, in the case-1
  `scr_mv_x` countdown, and does it through the **whole address** `0x02026BD8`
  as a literal — `effect_A7_move` at `0x060F9476`, `effect_02_move` at
  `0x060DCBE6`. That is the port's other quake write, and the port has it right.

`pp_screen_quake()` is PS2 pad rumble (`io/pulpul.c` -> `pulpul_request`), so
the port's block looks like a PS2 addition that reached for
`bg_w.quake_y_index` as a scratch variable to pass the rumble magnitude. The fix
keeps the rumble and drops the state write: `pp_screen_quake(gqdt[tad->quake][1])`.

Measured over the corpus before the fix: **62 spurious quakes across 43 of 143
segments**, every one peaking at 6 and decaying 6,5,4,3,2,1,0 while the archive
held 0 throughout. Only 7 of those 43 mattered to `Random_ix16`, because only a
stage with a quake-reactive debris cohort turns the extra quake into an extra
draw — which is exactly why the corpus reported 7 and not 43.

#### Defect two (E5b): two wrong rows in `gqdt`

The port's `gqdt` (`effect/eff02.c`) had `{6, 0}` at rows 7 and 8. The arcade's
table at CPS3 `0x061B941A` — the one `effect_A7_move` indexes at
`0x060F9364` with `mov.b @(7,r13),r0` / `shll2` / `mov.w @(r0,r4)` and
`mov.w @(2,r4)`, so the same `s16[2]` layout — has `{6, 4}` and `{6, 2}`. Every
other row of the nineteen matches exactly.

Corroborated in the archives independently of the disassembly: with `{6, 0}` the
port wrote `bg_w.quake_y_index = 0` on **12 events across 10 segments** where the
archive holds 2 on the next frame and 1 on the one after — one `gqdt[8][1] = 2`
quake decaying under `ta0_move()`. None of those 12 failed statcheck (no
quake-reactive cohort on those stages), so this defect was invisible to the
verdict and visible only to the field comparison.

#### What the fix cost and bought

143-segment sweep, `build/statcheck-verify`, same binary tree:

```
before   135 PASS / 8 FAIL     (E4 + 7 E5)
after    142 PASS / 1 FAIL     (E4 only)
```

and, across every compared frame of all 143 segments, **zero** remaining
`bg_w.quake_y_index` divergence — down from 48 segments. The field is now
asserted in `compare_service_values()`, next to `Random_ix32`, so it is compared
and never imported. That assert is the point: an unasserted quake divergence
surfaces as an unexplained `Random_ix16` failure with no named cause, which is
how E5 presented in the first place.

#### `BG_W_QUAKE_Y_INDEX_OFFSET` — and why `offsetof` is a trap here

`bg_w.quake_y_index` is at **CPS3 `0x02026BD8`**, archive offset `0x26BD8`, s16
big-endian. **Do not derive it** from `BG_W_STAGE_OFFSET` plus the port's
`offsetof`: the port's `BG` (`stage/bg.h`) puts `quake_y_index` 29 bytes past
`stage`, which lands on an odd address an SH-2 cannot hold an `s16` at, so the
arcade layout differs — the same trap H2 hit for `bg_w.stage`. Five independent
sites in the SH-2 program, three indexing `&bg_w = 0x02026BAC` at `+44` and two
loading the whole address:

| site | CPS3 | what it does | port line it implements |
|---|---|---|---|
| `eff19_quake_sub` | `0x060E4BF8` | `mov.l <&bg_w>,r12` / `mov #44,r0` / `mov.w @(r0,r12),r3` / `cmp/gt` 2, then 8, then 14 | `<= 2`, `< 8`, `> 14` selecting `eff19_s/m/l_tbl` |
| `eff94_2000_0` | `0x060F6D30` | same load, `cmp/gt` 3 then `cmp/ge` 24 | `> 3`, `>= 24` |
| `eff11_quake_sub` | `0x060E0A1A` | same load, `cmp/gt` 1, then `shll` + index `eff11_quake_index_tbl` | `> 1` |
| `effect_A7_move` | `0x060F9476` | `mov.l <0x02026BD8>,r1` / `mov.w @(98,r14),r2` / `mov.w r2,@r1` | `bg_w.quake_y_index = ewk->wu.scr_mv_y` |
| `effect_02_move` | `0x060DCBE6` | same | same |

The tables that anchor those functions each occur exactly once in the decrypted
image and have exactly one literal referrer, which is how the functions were
identified: `eff19_data_tbl`/`eff19_wait_tbl`/`effect_19_s/m/l_tbl` at
`0x061BD4C0`ff, `eff94_2000_tbl`/`eff94_2000_1_tbl` at `0x061BFAD0`/`0x061BFAF4`,
`eff11_quake_index_tbl` at `0x061BAE14`, and `explem` at `0x061B9466` (two
referrers — `effect_02_move` and `effect_A7_move`, which is what separated the
pair).

Archive corroboration, independent of all of that: the s16 at `0x26BD8` sits at
0, jumps to 6 or 10 on a hit, and decays by exactly 1 per frame to 0. That is
`ta0_move()`'s signature (`if (bg_w.quake_y_index > 0) bg_w.quake_y_index--`)
and nothing else's.

#### Still open

- **The `.3sr` files already published were produced by the pre-fix engine.**
  `bg_w.quake_y_index` is not itself in the viewer's 13-field checkpoint window
  (`gather_live_fields()`, `replay_player.c`), but `Random_ix16` is (index 5),
  and on a quake-reactive stage the fix changes `Random_ix16`. Pre-fix, 7 of 143
  corpus segments (4.9%) had a divergent `Random_ix16` stream; those are exactly
  the ones whose recorded checkpoints encode the wrong stream and will mismatch
  on a post-fix build. **No `.3sr` format change is needed** — nothing has to be
  imported, so there is no v3 field to add — but the ~22,682 v1 files on the VPS
  and the published v2 files were converted by the old engine and a share of
  them now need re-conversion. Not decided here.
- **The device test.** Everything above is host-only.

### E6 — `effect_C08_move`'s routine 2 ran ungated (FIXED and GATED, 2026-09-06)

**This is the 2026-09-06 corpus's divergence D2, and it is an incomplete fix we
landed ourselves.** Not to be confused with this document's own "D2" under
*Detection coverage* below — that namespace is the on-device detection list; the
corpus numbers its divergences separately in
`/Volumes/KimchDrive/3sarm-corpus-2026-09-06/divergences/`.

**26 of the widened corpus's 32 `rc=1` failures**, and **every stage-3 segment
in it — 26 of 26** — across 10 quarks, 10 player pairs and 8 character
pairings, at archive frames 1,325 to 6,473. All 26 carry the same line:

    statcheck-rng: frame N delta=+1 (ours ix16=XX cps3=XX)
    statcheck-rng:   [ 0] random_16    <- effect_C08_move (0x...)
    src/test/statcheck_compare.c:331: Random_ix16 (a) != random_ix16_cps3 (a-1)

`delta=+1` on every one: the port is one draw **ahead** of CPS3, and the RNG
backtrace names our own module.

#### The within-session control

Quark `1788133423462-3110` (Kaisark vs [SEBA], `num_matches` 8) is the whole
argument in one session. All eight segments are the **same two players as the
same two characters** (P1 Urien, P2 Yun), two human operators, `start_index` 1.
Only the stage varies:

| segment | frames | stage (`bg_w.stage`) | statcheck |
|---|---|---|---|
| game_0 | 4,770  | **Yun (3)**  | **rc=1** @ 2,581 |
| game_1 | 6,328  | Urien (13)   | rc=0 |
| game_2 | 8,266  | **Yun (3)**  | **rc=1** @ 3,755 |
| game_3 | 10,736 | **Yun (3)**  | **rc=1** @ 3,565 |
| game_4 | 10,937 | Urien (13)   | rc=0 |
| game_5 | 5,078  | Urien (13)   | rc=0 |
| game_6 | 6,139  | **Yun (3)**  | **rc=1** @ 2,443 |
| game_7 | 7,309  | Urien (13)   | rc=0 |

**4/4 stage-3 fail; 4/4 stage-13 pass.** Players, characters, engine build,
harness, ROM and balance mode are all held constant. The stage is the variable,
and Hong Kong (`bg030`) is where `effect_C08_init()` spawns.

#### The defect, measured from the arcade program

`effect/effc08.c` is our transcription of CPS3 `0x060DD888`, landed `afc16ad2`
for §23.10 of `research-arcade-cg-data-accuracy.md`. **The spawn is right and
the routine-1 cadence is right; the routine-2 gate was missing.** Our own
comment said the flags had been *assumed* zero, which over a full round they are
not — `EXE_flag` and `Game_pause` are set through hit-stop, super freezes and
pauses.

Read out of the `sfiii3nr1` SH-2 program (`CS_MODE_SH2 | CS_MODE_BIG_ENDIAN`,
decryption pipeline in §23.4), the arcade **gates both routines, identically**:

| | routine 1 | routine 2 |
|---|---|---|
| entry | `0x060DD918` | `0x060DDA84` |
| `EXE_flag` (`0x0200EECC`) | `mov.w @r4,r0` / `tst` / `bra 0x060DDA4E` | `mov.w @r4,r0` / `tst` / `bf 0x060DDB6C` |
| `Game_pause` (`0x0201136E`) | `mov.l 0x060DD98C,r3` / `mov.w @r3,r0` / `tst` / `bra 0x060DDA4E` | `mov.l 0x060DDBD8,r3` / `mov.w @r3,r0` / `tst` / `bf 0x060DDB6C` |
| first counter touch | `timer--` at `0x060DD92E` | `timer--` at `0x060DDA92` |

`r4` holds `0x0200EECC` from the prologue (`mov.l 0x060DD974,r4` at
`0x060DD890`) and stays live across the dispatch, which is why only *one*
`EXE_flag` literal appears in the function. `Game_pause` needs a pool slot per
use, and the function has **two** — `0x060DD98C` and `0x060DDBD8`, one per
routine. Both skip targets (`0x060DDA4E`, `0x060DDB6C`) are palette-request
tails: they draw nothing and touch no counter.

**Mechanism.** Ungated, our routine 2 counted down through frames on which the
arcade freezes, so the port left the `4 x v` pause early, re-entered routine 1
early, and reached its next every-8th-frame draw one cycle sooner. That is
`delta=+1`, ours ahead — the observed direction, on all 26.

#### `effc74.c` does NOT share it, and that was checked two ways

`effect/effc74.c` (Club Metro, CPS3 `0x060F1384`) was validated the same way and
was the obvious co-suspect. It is clean:

- **Disassembly.** The arcade dispatch at `0x060F1390` has only `routine_no` 0,
  1 and a default that tail-calls the release path — there is no routine 2 to
  gate. `0x060F1384..0x060F1652` references `0x0201136E` **exactly once**
  (`0x060F1488`), against effect 8's two.
- **Corpus.** The widened corpus carries **13 stage-19 (Remy) segments**, and
  all 13 pass both before and after this change. The old corpus has neither a
  stage-3 nor a stage-19 segment, so it is not evidence about either effect.

#### The fix, and where it is gated

Two changes, both arcade-side:

1. `effect/effc08.c` -> `effect_C08_move`, `case 2`: wrapped in
   `if (!EXE_flag && !Game_pause)`, matching `case 1` and CPS3 `0x060DDA84`.
2. `stage/bg030.c` -> `bg0301_init00` and `stage/bg190.c` -> `bg1902_init00`:
   the `effect_C08_init()` / `effect_C74_init()` calls now sit inside
   `if (ArcadeBalance_IsEnabled())`.

**Why (2) is part of this fix and not scope creep.** `afc16ad2` landed on
2026-09-03, two days before the gating rule was settled, and its two spawn calls
were unconditional — so PS2 mode was running two effects that have **no PS2
counterpart at all** (the PS2 re-authoring dropped both and reused ids 8 and 74
for different effects, §23.8), consuming `Random_ix16` indices the PS2 engine
never consumes, on those two stages, from frame 1 of every round. That is the
same condition `2d74225d` corrected for E4 and E5. Gating the **spawn** rather
than the body is the right shape here: `effc08.c` and `effc74.c` exist only to
reproduce CPS3, so there is no PS2 arm for their bodies to select — under PS2
balance the correct behaviour is for the work not to exist.

#### Result

| gate | before | after |
|---|---|---|
| corpus 2026-09-06 (185 segments) | 151 PASS / 32 FAIL / 2 cpu-player | **177 PASS / 6 FAIL / 2 cpu-player** |
| corpus 2026-09-05 (143 segments) | 143 PASS / 0 FAIL | **143 PASS / 0 FAIL**, zero verdict lines changed |
| frame-data suite (`--check-golden`) | 99 GREEN, zero drift | **99 GREEN, zero drift** |

Exactly 26 verdict lines changed, all of them stage 3, all of them
`divergent -> pass`. The 6 that remain are the corpus's D3-D6 and are separate
defects: 3 x `pos_3sx.x` (D4, one 19-match session), 1 x `Random_ix16` at
**delta=-1** with no caller (D5 — CPS3 drew where we did not, the opposite
direction and not this defect), 1 x `w_type` and 1 x `w_int`, both at archive
frame 7 (D6, D3). None of their frames or asserts moved.

#### Still open

- **Device test.** Host-only, as with E4 and E5.
- **The `4 x v` pause length itself is still only corroborated to frame 60.**
  What this pass proves is that routine 2 must not tick while the game is
  frozen. The `& 3` wrap and the `x98` decrement inside it are §23.6's reading
  and are unchanged; the corpus now exercises them for thousands of frames per
  segment and finds no residual drift, which is much stronger evidence than
  §23.10 had, but it is corpus evidence, not a second disassembly pass.
- **`EXE_obroll`.** CPS3 `effect_71_move` reads it alongside the other two
  (§23.6); neither effect 8 nor effect 74 does, and neither port module tests
  it. Nothing here suggests it should.

### E7 — `Win_01000()` clamps the winner to the screen edge; the arcade does not (FIXED and GATED, 2026-09-06)

**This is the 2026-09-06 corpus's divergence D4** — 3 of its 4 remaining
`rc=1` failures, all three in one 19-match session
(`1788572346231-3523`), all three the same assert:

    src/test/statcheck_compare.c:240: pos_3sx.x (520) != pos_cps3.x (526)

#### The defect

Our `Win_01000()` (`animation/win_pl.c`) calls a `set_field_hosei_flag` pair
between `bg_app_stop = 1` and `switch (wk->wu.routine_no[3])`. The arcade
routine has nothing between those two statements.

`Win_01000` is reached as `win_jp_tbl[winner_type_tbl[player_number]]`.
`win_player` (`0x060C2DDC`) copies the 16-entry table at `0x061A38C0` onto its
own stack — the arcade counterpart of the port's local
`void (*win_jp_tbl[16])(PLW*)` — and indexes it with the 21-entry `s16` table
at `0x061A3890`, whose Oro entry is `1`, giving `0x060C2E8C`. Both tables occur
**exactly once** in the image, and `0x061A3890` has **exactly one** literal
referrer, `0x060C2ED4`, inside `win_player`'s own pool.

The arcade body goes from the store straight into the dispatch:

    060c2ea2  mov.b r3,@r2       ; bg_app_stop = 1  (r3 = 1, r2 = 0x0202802A)
    060c2ea4  mov.l 0x60c2edc,r13
    060c2ea6  mov.w @(r0,r14),r0 ; r0 = 42 -> wk->wu.routine_no[3]
    060c2ea8  cmp/eq #0,r0 / bt  -> case 0
    060c2eae  cmp/eq #1,r0 / bt  -> 0x060C2F94  } case 1 and case 9 share a
    060c2eb2  cmp/eq #9,r0 / bt  -> 0x060C2F94  } target, exactly as the port's
    060c2eb6  bra   0x060C3022                    `case 1: case 9:` does

Two independent cross-checks fall out of that listing and neither was arranged:
`WORK_ROUTINE_NO_OFFSET 0x24` (`arcade_constants.h`) puts `routine_no[3]` at
`0x2A` = **42**, the arcade's own displacement; and `case 1` and `case 9`
sharing one target is a shape the port already had.

The negative is established over the whole routine — `0x060C2E8C..0x060C33B2`,
1,318 bytes, with all three `jijii_*` inlined into it:

- **No `jsr` can reach it.** No 4-byte-aligned word in that range equals
  `&set_field_hosei_flag` (`0x0611DFB8`), so the address is never loaded.
- **No `bsr` can reach it either.** The target sits `0x5AC06` past the end of
  the function, against a `bsr` displacement reach of `±0x1000`. The only two
  `bsr` targets in range are `0x060C260E` and `0x060C271E`.
- **The scan has a positive control.** The same pool *does* carry
  `0x0611E0EE` (`random_16`), which is `0x136` from `set_field_hosei_flag`.
  A scan that can find the neighbour and not the thing is not a scan that
  missed it.

#### The mechanism, and the archive agrees frame for frame

`set_field_hosei_flag(&plw[id], scrr, 1)` pins the winner at
`scrr - satse[]` — screen centre + 164. `Win_01000` case 9 dispatches
`win_rno[0] == 2` to `jijii_jump`, whose flight case leaves only on

    wk->wu.xyz[0].disp.pos > bg_w.bgw[1].xy[0].disp.pos + 320

Pinned at +164, that test can never be true, so the leap does not terminate.

Read straight out of the archive (`game_8`, XOR-accumulated, P1 is the winner),
`routine_no[3] == 9` and `win_rno == 2/1` — `jijii_jump`'s flight arm — hold
across the whole window, and the arcade's winner simply keeps going:

| archive frame | winner `xyz[0].disp.pos` | `win_rno` |
|---|---|---|
| 3,271 | **526** (ours: 520 — the assert) | 2/1 |
| 3,289 | 666 | 2/1 |
| **3,290** | **674** | **2/2** |
| 3,291 | 674 | 2/3 |
| 3,292+ | 674 (frozen) | 2/3 |

The camera (`bg_w.bgw[1].xy[0].disp.pos`) is at 348, so the threshold is 668.
At 3,289 the winner is at 666 and below it; at 3,290 he is at 674 and
`win_rno[1]` steps 1 -> 2, which is `jijii_jump`'s
`win_rno[1]++; effect_work_kill(3, 13);` firing. The next frame steps 2 -> 3
(case 2 sets `win_free[id] = 48`) and X freezes for the countdown. That is the
exit our clamp made unreachable, observed happening.

`win_rno` was not assumed: `0x020281AC` is the pool word `0x060C2EDC` loaded
into `r13` at the head of the arcade routine, and the two `s16` there run
`2/1 -> 2/2 -> 2/3` exactly as `jijii_jump`'s state machine does.

#### The fix, and where it is gated

`animation/win_pl.c` -> `Win_01000()`: the `set_field_hosei_flag` pair is now
inside `if (!ArcadeBalance_IsEnabled())`. PS2 keeps the clamp — this was proven
against the CPS3 program and **not** against the PS2 binary, which is the rule
settled in `2d74225d` and applied again in `192291a4`.

This also retires a candidate left open under **E3**, which guessed "the
`set_field_hosei_flag` clamp against an unimported camera" as the unidentified
`+32` writer. That guess was about the right function; E3 itself stays
retracted, and the write it was looking for is still unidentified.

#### Result

| gate | before | after |
|---|---|---|
| corpus 2026-09-06 (185 segments) | 178 PASS / 4 `rc=1` / 1 `rc=4` / 2 `rc=3` | **181 PASS / 1 `rc=1` / 1 `rc=4` / 2 `rc=3`** |
| corpus 2026-09-05 (143 segments) | 143 PASS / 0 FAIL | **143 PASS / 0 FAIL**, zero verdict lines changed |
| frame-data suite (`--check-golden`) | 99 GREEN, zero drift | **99 GREEN, zero drift** |

Exactly **3** verdict lines changed, all `divergent -> pass`, all in session
`1788572346231-3523`; every seed verdict and field count is unchanged.

#### Still open

- **Device test.** Host-only, as with E4, E5 and E6.
  `plw[1].wu.xyz[0].disp.pos` is field 9 of the viewer's checkpoint hash
  window, so unlike a purely internal fix this one *can* change what the
  on-device viewer sees.
- **The other 47 `set_field_hosei_flag` sites in `win_pl.c`, and 12 in
  `lose_pl.c`, are UNADJUDICATED.** This is the important caveat and it is not
  a formality: the port carries the clamp in two shapes — unguarded at the top
  of the function (`Win_01000`, `Win_03000`, `Win_04000`,
  `Normal_normal_Winner`, `Judge_normal_winner`, `Win_05000`, `Win_07000`,
  `Win_09000`, `Win_14000`) and guarded inside `case 0` of the
  `routine_no[3]` switch (`Win_02000`, `Win_06000`, `Win_08000`, `Win_10000`,
  `Win_11000`, `Win_12000`, `Win_13000`, `Win_15000`). Only Oro's routine was
  read out of the arcade program. The survey that was meant to generalise this
  indexed a function cluster that never contained Oro's routine at all, so it
  is not evidence about any of the others either. `Normal_normal_Winner` is
  the nearest neighbour — its first ten lines are byte-identical to
  `Win_01000`'s, which is how this edit first matched two sites — and it is
  reached by far more characters than Oro. Nothing here says the other sites
  are wrong; nothing here says they are right.
- **Whether any other character's win routine can strand the same way.** The
  `± 320` exit is `jijii_jump`'s, i.e. Oro's. Other win routines have other
  exits, and a clamp that is harmless in one may not be in another. Not
  examined.

### E8 — `effect_L7_init`'s input gate tests the wrong bit (FIXED and GATED, 2026-09-06)

**This is the 2026-09-06 corpus's divergence D5**, its last remaining `rc=1`:
segment `1785545046814-8666_game_0` (P1 Hugo / P2 Sean, stage Sean), archive
frame 2,809,

    statcheck-rng: frame 2809 delta=-1
    src/test/statcheck_compare.c:331: Random_ix16 (23) != random_ix16_cps3 (24)

`delta=-1` — CPS3 drew a `random_16()` where we did not, the **opposite**
direction from E6, and with no caller named on our side because we never ran
the code that draws.

#### The defect

`effect_L7_init` spawns Hugo's Poison taunt gag. Our port reaches the same
function on the same frame and passes the first two gates; the third rejects
it, and its mask is wrong.

The arcade function is CPS3 `0x06113FC8`, identified without reference to the
port's source:

- `effl7_data_tbl` — `{55, 56, 57, 55, ...}` as big-endian `s16` — occurs
  **exactly once** in the image, at `0x061CB064`, and has **exactly one**
  literal referrer, the pool word `0x061141A4`, loaded by the instruction at
  `0x0611416C`, which is inside this function.
- `effmovejptbl` (`0x061B883C`) entry **[217]** is `0x06113D54`,
  `effect_L7_move` — and 217 is the `wu.id` this very routine stores
  (`mov.w r0,@(8,r14)` at `0x0611404C`, `r0` = the word literal 217 at
  `0x061140C2`). Entry [214] is `0x061135B4`, the `effect_L4_move` used as a
  cross-check in `research-arcade-cg-data-accuracy.md` §23.5.

Gates 1 and 2 transcribe correctly. Gate 3 does not:

    06113ffc  mov.w 0x61140c0,r4   ; r4 = 0x1000
    06113ffe  mov.w @(8,r13),r0    ; wk->id
    06114000  tst r0,r0 / bt 0x6114016
    06114004  mov.l 0x61140dc,r2   ; 0x0206AA90  (P2SW_0)
    0611400a  tst r4,r3 / bf ...   ; continue iff P2SW_0 & 0x1000
    06114016  mov.l 0x61140e0,r2   ; 0x0206AA8C  (P1SW_0)
    0611401c  tst r4,r1 / bf ...   ; continue iff P1SW_0 & 0x1000

The arcade tests **bit 12** of the raw `P1SW_0`/`P2SW_0` register; the port
tested **bit 0**, `SWK_UP`. The neighbouring pool words corroborate the
addresses: `0x061140D8` is `poison_flag` (`0x020281A8`, the `s16` array gate 2
indexes) and `0x061140E4` is `pull_effect_work`, called immediately after with
`r4 = 4` exactly as the port does. `P1SW_0_OFFSET`/`P2SW_0_OFFSET`
(`arcade_constants.h`) are `0x6AA8C`/`0x6AA90`, the same two registers at RAM
base `0x02000000`.

**One draw per spawn, hence `delta=-1`.** Over the whole function
(`0x06113FC8`..`0x0611418A`, pool to `0x061141A8`) exactly one pool word equals
`&random_16` (`0x0611E0EE`): `0x061141A0`, `jsr`'d at `0x06114166`. No `bsr`
can supply another — `random_16` is `0x9F64` past the end against a `±0x1000`
reach, and the function's only `bsr` target is `0x06114FCA`. What the draw
feeds is visible in the next four instructions: `exts.w r0,r4`, then
`effl7_data_tbl[kind_w]` into `wu.old_rno[2]`, then `poison_flag[wk->id] = 1` —
the port's last three lines, in order.

#### Which bit that is, and what is NOT claimed

`SWK_START` is used as **"the bit this pipeline carries arcade `P1SW_0` bit 12
in"**. That is a conversion identity, not a claim about the cabinet. The port's
own raw-arcade -> SWK converter, `src/test/replay_game.c` ->
`read_input_buff()`, which converts whole raw `P1SW_0` words for `.3sr`
playback, already maps it and labels it:

    buff |= (raw_buff & (1 << 12)) << 2; // start

`1 << 12` shifted left 2 is `1 << 14` == `SWK_START`. So a `p*sw_0` word
produced by this port's conversion layer carries arcade bit 12 at `SWK_START`,
and testing `SWK_START` is the faithful transcription of `& 0x1000` regardless
of what the line is physically wired to.

**That bit 12 *is* START is likely but NOT proven, and nothing here depends on
it.** The supporting evidence is circumstantial and is recorded as such:
`0x3F0` (bits 4-9) is the image-wide six-button mask; the archive's `P1SW_0`
union is `0x13FF` with bits 10 and 11 never set; and `0x06001AE0` computes
`p1sw_0 & ~p1sw_1 & 0x1000`, the shape of `Ck_Coin()`'s
`~p1sw_1 & p1sw_0 & SWK_START`. `0x06001A72` was **not** proven to be
`Ck_Coin`.

#### The harness could not see this, and fixing that took two changes

`read_input_buff()` (`statcheck_runner.c`) feeds the engine from the archived
`sw_lvbt` mirror at `WCP_OFFSET`, which carries the lever and the six attack
buttons and **no start bit at all**. Every archived START press was therefore
invisible to statcheck, and no segment could reproduce a path gated on one.
The engine fix alone is inert here — it is correct, and it demonstrates on
nothing.

1. **`read_input_buff()` now also imports the raw START bit.** One extra
   bit-test bolted onto the existing translator, not a raw copy: the file's
   LAYOUT INVARIANT is about where the *kicks* sit (raw 7-9, engine 8-10), and
   bit 12 is outside that span in both encodings.

2. **`Check_Pause_Term()`'s `STATCHECK` carve-out moved above the `SWK_START`
   check** (`system/pause.c`). This is the same correction the runtime `.3sr`
   replay player already carries, three lines higher, and the comment there
   states the reason: *a `.3sr` is a recording of an ARCADE session, where
   START is not a pause button*, so recorded words contain ordinary in-match
   START presses and `Convert_User_Setting` passes `SWK_START` straight
   through to `Pause_Type = 1` / `Game_pause = 0x81`, stalling `Game_timer`.
   That comment also recorded why the STATCHECK block was safe below it —
   "`read_input_buff` never emits `SWK_START`" — which change (1) makes false.
   Leaving it there cost exactly what it cost the replay player, and it was
   measured, not predicted: two segments' `t_pl_lvr` counters fell one behind
   the archive (`left_cnt` 35 vs 36, `right_cnt` 53 vs 54) and one segment's
   `waza` `w_type` did, on three segments that had passed. A statcheck archive
   is the same arcade recording a `.3sr` is, so it now gets the same answer.
   `STATCHECK` is a harness-only build flavor; nothing in that block reaches
   the shipping binary.

#### The fix, and where it is gated

`effect/effl7.c` -> `effect_L7_init()` gate 3 now tests
`ArcadeBalance_IsEnabled() ? SWK_START : SWK_UP`. `SWK_UP` is `1 << 0`, so the
PS2 arm is bit-identical to what the function has always done — proven against
the CPS3 program and not against the PS2 binary, the rule from `2d74225d`.

`win_pl.c` -> `Win_13000()` carries the **identical `& 1` gate** (the alternate
final-win pose, the same easter egg) and is **deliberately left alone**: the
arcade counterpart of `Win_13000` was not read out. See *Still open*.

#### Result

| gate | before | after |
|---|---|---|
| corpus 2026-09-06 (185 segments) | 181 PASS / 1 `rc=1` / 1 `rc=4` / 2 `rc=3` | **182 PASS / 0 `rc=1` / 1 `rc=4` / 2 `rc=3`** |
| corpus 2026-09-05 (143 segments) | 143 PASS / 0 FAIL | **143 PASS / 0 FAIL**, zero verdict lines changed |
| frame-data suite (`--check-golden`) | 99 GREEN, zero drift | **99 GREEN, zero drift** |

Exactly **one** verdict line changed. With E7 and E8 landed, **no engine
divergence is outstanding on either corpus**: the 2026-09-06 corpus's three
remaining non-PASS results are 1 `rc=4` (H5's Twelve segment) and 2 `rc=3`
(H4b's CPU players), both harness verdicts by design. *(H5b since seeded that
`rc=4`: the corpus is 183 PASS / 2 `rc=3` as of 2026-09-06.)*

#### Still open

- **`Win_13000()`'s identical `& 1` gate is UNADJUDICATED.** Same easter egg,
  same shape, arcade counterpart never read. It was left alone on purpose
  rather than fixed by analogy.
- **Bit 12 == START is unproven**, as above. The fix does not rest on it, but
  any future claim that it *is* START needs its own evidence.
- **`work_id`: a real, unexplained difference.** The arcade routine makes **no
  store to `work_id`** — over `0x06113FC8`..`0x0611418A` there is no store to
  `@(6,r14)`, and `r0` never takes the value 6 in any of the indexed stores —
  and the archive's slot-35 work reads `work_id = 0`. Our port writes
  `ewk->wu.work_id = 16`. The consequences were not examined; the segment
  passes with the difference in place.
- **Device test.** Host-only, as with E4-E7.
- **One unreproduced `SIGABRT`.** During one 5-way-parallel sweep, segment
  `1784875995078-5749_game_2` exited on signal 6. It did not recur: 5 serial
  runs, 12 parallel runs, a full 185-segment re-sweep, and 12 parallel runs of
  the **pre-change** binary are all clean. Recorded because it happened, not
  because it is understood; it is not attributable to this change.

## FP — the freeze pair `EXE_flag` / `Game_pause` is now asserted (2026-09-06)

`EXE_flag` and `Game_pause` are the two flags every in-battle effect gates on.
The two names appear **together on 210 lines across 114 files** of
`sf33rd/Source/Game/` — almost all of them the one conjunction
`if (!EXE_flag && !Game_pause)`, in the effects, `ui/count.c`,
`engine/plcnt*.c`, `engine/vital.c`, `engine/stun.c`, `engine/spgauge.c` and
`stage/bg.c` — and until this change **neither was ever compared against
CPS3**. E6 established both arcade addresses by disassembly and then left them
in a source comment: the offsets never reached `arcade_constants.h`, and no
oracle read them.

That is the `Random_ix16` shape exactly. A freeze window that opens or closes
one frame off the arcade's changes what every gated effect does on that frame,
and the only thing the harness could see was the consequence — a
`Random_ix16` delta with a caller name, if the drift happened to land on a
frame where a gated effect drew, and nothing at all if it did not. E6's
acceptance window was 60 frames and could not have reached a K.O. or a super
freeze at all.

### The offsets, and the archive corroboration

| | CPS3 | archive offset | type |
|---|---|---|---|
| `EXE_flag` | `0x0200EECC` | `0xEECC` | s16 |
| `Game_pause` | `0x0201136E` | `0x1136E` | s16 arcade, `u8` in the port |

Both were read off the `sfiii3nr1` SH-2 program for E6 and are recorded in the
two modules transcribed from it, at three independent sites — `effect_C08_move`
routine 1 (`0x060DD918`), routine 2 (`0x060DDA84`) and `effect_C74_move`
routine 1 (`0x060F13D4`). Each reads `EXE_flag` first and `Game_pause` second,
both with `mov.w`, which is what makes them 16-bit. `GAME_TIMER_OFFSET` is
already `0x1136C`, so `Game_pause` is the adjacent halfword; the neighbours at
`0x02011370`, `0x0201137A` (`Round_Level`) and `0x0201137E` are 16-bit at even
addresses too, which is the layout a byte-level dump of the window confirms.

**Corroborated against the archives, not taken from the comment.** Read
straight out of the `.scrd` frames:

- `EXE_flag` holds only 0, 1, 2 and 3 — exactly the range of
  `set_EXE_flag()`'s `EXE_flag = Game_timer % (SLOW_flag + 1)`
  (`engine/slowf.c`). It is a slow-motion divider, not a boolean.
- `Game_pause` is 0 on ~95% of frames and non-zero in bursts, and **the bursts
  are two different values**:
  - **-1 (`0xFFFF`), in runs of exactly 90 frames.** 90 is `Time_Data[1]` in
    `effect/eff84.c` — the K.O. round-message window — and our
    `effect_84_move` writes **1** there.
  - **1, in runs of 1, 8 and ~163 frames** — the `Game_Manage_*` screen
    transitions, where `engine/manage.c` writes 1 on both sides.
- Frame by frame over one of the K.O. windows (`1788133423462-3110` game_0,
  frames 2,462-2,551): `Game_pause` goes to -1 and `EXE_flag` **freezes** at
  its last value for all 90 frames, then resumes cycling 0,1,2,3 the frame
  after. That is our own `set_EXE_flag()`'s `if (!Game_pause)` guard, seen from
  the outside — so the arcade treats -1 as "paused" exactly as it treats 1, and
  nothing reads the magnitude. `Random_ix16` is frozen for the whole window
  too, which says no gated effect draws inside it.
- `Game_timer` (`0x0201136C`, immediately below) keeps incrementing through
  both kinds of run. That is `Game2_1()`'s `Game_pause != 0x81` gate: only the
  PS2-only START pause stalls the timer, and the arcade has no `0x81`.

### Asserted, not seeded — and why that is the right call here

Both are **asserted every compared frame and never imported**, the call E5 made
for `bg_w.quake_y_index` (`BG_W_QUAKE_Y_INDEX_OFFSET`) and the opposite of the
one made for `players_timer`. The test is the one the seed audit section
states: *is this state carried across the match boundary, or rebuilt?*

- `Game2_0()` (`game.c`) writes `Game_pause = 0` two statements after
  `Game_timer = 0` — H1's predicate, the frame after the seed frame, on both
  sides.
- `set_EXE_flag()` (`engine/slowf.c`) recomputes `EXE_flag` from
  `Game_timer % (SLOW_flag + 1)` on every unpaused frame, and `Game2_0()` has
  just zeroed `Game_timer`.

So there is nothing for `Statcheck_SyncValues` to seed: both sides already
agree at the first compared frame, and a mismatch afterwards is behaviour, not
an initial condition. Force-syncing them per frame would be the `Random_ix16`
mask again — it would *repair* the divergence and report it as a pass, while
every effect gated on the pair went on diverging invisibly.

### The one normalization, and why it is not a mask on the field

`Game_pause` needs exactly one substitution, forced by the measurement above:
the arcade holds **-1** through the K.O. window where our `effect_84_move`
holds **1**. Both engines read the field only as a zero test, and the archive
shows `EXE_flag` freezing across a -1 run exactly as it does across a 1 run —
so -1 and 1 are the same state, differently spelled by a port that narrowed the
field to `u8`. `compare_service_values()` therefore maps that **one** value and
asserts equality on everything else:

```c
const s16 game_pause_cps3_norm = (game_pause_cps3 == -1) ? 1 : game_pause_cps3;
assert_equals((s16)Game_pause, game_pause_cps3_norm);
```

A window that opens or closes on the wrong frame still fires, and so does any
value pair not seen here. A blanket `!!ours == !!theirs` predicate would have
been weaker for no gain.

**Where the asserts sit, and what that costs.** Immediately after the
`bg_w.quake_y_index` assert, i.e. *after* `Random_ix16`. On a frame where a
freeze-window drift also moves the RNG, `Random_ix16` fails first and its
call-site backtrace — the thing that solved E6 — is the more useful of the two
reports. What these asserts add is the case the RNG cannot see at all: a window
that drifts on a frame where no gated effect happened to draw. That frame
passes the RNG check and bites hundreds of frames later, which is exactly E6's
shape.

### `Game_pause == 0x81` cannot reach the assert, and every writer was traced

`0x81` is the PS2 START pause. The arcade cannot produce it, so if it reached
the assert it would report a **harness** fault as an engine divergence. It
cannot, and this was traced writer by writer rather than assumed:

| writer | why it cannot fire under STATCHECK |
|---|---|
| `Setup_Pause`, `Setup_Come_Out` (`system/pause.c`) | reached only from `Pause_Check`'s `switch (PAUSE_X)`, and `PAUSE_X` is set only by `Check_Pause_Term()`, which returns 0 unconditionally under `#if defined(STATCHECK)` — above both the `SWK_START` test and the controller-connection test, where `3769c189` moved it |
| `Flash_Pause_4th` -> `Setup_Pause` | runs only while `Pause_Down != 0`, which only the two above set |
| `Check_SoftReset` (`system/reset.c`) | needs `Reset_Status == 0x63`, which `Check_Reset_IO` reaches only on `SWK_START \| SWK_BACK` **together**; `read_input_buff` (`statcheck_runner.c`) emits ten SWK bits and `SWK_START`, and **never** `SWK_BACK` |
| `Setup_Tr_Pause`, `Reset_Training`, `Reset_Replay`, `Character_Change`, `End_Replay_Menu` (`menu/menu.c`) | training- and replay-menu paths; the harness runs `MODE_ARCADE` (see "HARNESS FACT" below) and never readies `TASK_MENU` |
| `cpLoopTask`'s `Game_pause \|= 0x80` (`main.c`) | `#if defined(DEBUG)`, and the CMake guard makes DEBUG + STATCHECK a hard error |

The field is therefore **not masked** for `0x81`. What the compare does instead
is *label* it: if our side ever holds `0x81` while the archive does not, one
stderr line says the STATCHECK carve-out in `Check_Pause_Term()` has regressed
and that this is a harness fault, and then the assert fires normally. Masking
would have hidden a regression in the carve-out; labelling does not. **It never
fired on either corpus.**

### The seed audit checks them too, on opposite terms

`audit_service()` (`statcheck_seed_audit.c`) gains both, and they land on
opposite sides of the allowlist — which is the point of measuring rather than
assuming:

- **`EXE_flag` is STRICT.** 0 at the seed frame on **183 of 183** and **143 of
  143**, and 0 on our side too. Its one live input, `SLOW_flag`, *is* carried
  across the match boundary (`init_slow_flag()` is what clears it), so a stale
  `SLOW_flag` on either side would surface here first. It costs one comparison.
- **`Game_pause` is ALLOWLISTED**, and it had to be. The archive carries
  `Game_pause == 1` into the seed frame on **50 of 183** 2026-09-06 segments
  and **38 of 143** 2026-09-05 ones; our own synthetic session holds **1 on all
  of them**, because it is in its own `Game_Manage_*` transition. Both sides
  are 0 at `start_index` — the first compared frame — on **183 of 183** and
  **143 of 143**. Strict would have promoted an rc-1 run to rc 4 on 130 of 183
  segments while reporting nothing real.

Measured directly, not inferred, at `STATCHECK_SEED_AUDIT_VERBOSE=2`:

    1788133423462-3110 game_0 (stage 3):  Game_pause ours=1 cps3=1   EXE_flag ours=0 cps3=0
    1788133423462-3110 game_1 (stage 13): Game_pause ours=1 cps3=0   EXE_flag ours=0 cps3=0

The allowlist's own warning applies and is answered: an entry whose rewrite
claim is false does not go unnoticed, it goes *misattributed*. Here the claim
is checked by the oracle rather than trusted — `compare_service_values()`
asserts `Game_pause` on every compared frame, so a value that failed to reach 0
by `start_index` fails on the first compared frame instead of going quiet.

### Result

Two binaries, same machine, same CMake flags, differing only by this change:
the "before" is the pristine `3769c189` snapshot built at
`/Volumes/KimchDrive/statcheck-oracle/build-3769c189…`, the "after" is
`build/statcheck-pausegate`. `--headless` on every run.

| gate | before | after |
|---|---|---|
| corpus 2026-09-06 (185 segments) | 182 PASS / 0 `rc=1` / 1 `rc=4` / 2 `rc=3` | **182 PASS / 0 `rc=1` / 1 `rc=4` / 2 `rc=3`** |
| corpus 2026-09-05 (143 segments) | 143 PASS / 0 FAIL | **143 PASS / 0 FAIL** |
| `PASS — compared archive frames a..b of n` lines that changed | — | **0 of 182 and 0 of 143** |
| verdicts, fail frames or asserts that moved | — | **0** |
| `Game_pause == 0x81` harness notes | — | **0 of 328 runs** |
| frame-data suite (`--check-golden`) | 99 GREEN, zero drift | **99 GREEN, zero drift** |

**Zero new failures, and that is a real result rather than a null one.** Two
fields that gate 210 conjunction sites had never been compared; putting them
under assertion on 325 segments and **1,863,138** compared frames says the
freeze windows agree frame for frame.

The only thing that moved is the seed audit's allowlisted-difference count:
**+1 on 130 of 183** (2026-09-06) and **105 of 143** (2026-09-05), exactly the
segments where the archive's seed `Game_pause` is 0 and ours is 1. Per-segment
it is now 19-26 where it was 19-25.

### Positive control: the assert is live, and the mask hides exactly one thing

A clean sweep proves nothing on its own — a dead assert is also clean. So the
`-1 -> 1` substitution was **removed** and the corpus re-swept with an otherwise
identical binary (`build/statcheck-freezectrl`):

| | with the normalization | without it |
|---|---|---|
| 2026-09-06 (185) | 182 PASS / 0 `rc=1` | **0 PASS / 180 `rc=1` / 3 `rc=4` / 2 `rc=3`** |
| 2026-09-05 (143) | 143 PASS | **0 PASS / 143 `rc=1`** |

Three things follow, and the third is the one that justifies the design:

1. **The assert is live.** It fires on **182 of 182** comparable 2026-09-06
   segments (archive frames **941-5,755**, median 2,270) and **143 of 143**
   2026-09-05 ones (**921-4,255**, median 2,200). It is not dead code and it is
   not unreachable.
2. **The compared window reaches a K.O. freeze on every single segment.** That
   is the coverage question answered directly: §23.10's 60-frame acceptance
   could not reach one, and this oracle reaches one everywhere.
3. **Every one of those 325 failures is the same line** —
   `game_pause_3sx (1) != game_pause_cps3_norm (-1)` — with **no other value
   pair anywhere on either corpus**. So the `-1 -> 1` substitution masks
   exactly one difference and nothing is hiding behind it. That is the
   difference between a targeted normalization and a blanket predicate, and it
   is measured rather than argued.

### What the archives say, in full

Every `.scrd` frame of both corpora — 183 eligible 2026-09-06 segments
(1,167,121 frames) and all 143 2026-09-05 ones (869,986):

| field | values seen | 2026-09-06 counts | 2026-09-05 counts |
|---|---|---|---|
| `EXE_flag` | **0, 1, 2, 3 and nothing else** | 1,115,433 / 24,905 / 14,089 / 12,694 | 828,153 / 19,604 / 12,074 / 10,155 |
| `Game_pause` | **0, 1, -1 and nothing else** | 1,105,287 / 23,543 / 38,291 | 821,134 / 18,813 / 30,039 |

`Game_pause == -1` occurs on **183 of 183** and **143 of 143** segments, in
**426** and **335** runs, of which **424** and **333** are exactly 90 frames
(the four exceptions — 71, 60, 54 and 15 — are truncated by the segment
boundary). `Game_pause == 1` runs are 8 frames (262 / 195), 163 (82 / 53), 1
(50 / 38), 161-165 (49 / 52), plus a single 68-frame run on the 2026-09-05
corpus. **No value outside {0, 1, -1} appears
anywhere** — in particular no `0x81`, which is the archive-side half of the
argument that the PS2 pause has no arcade counterpart.

### Still open

- **The arcade's `-1` writer was not disassembled.** What is established is the
  window and the value: 90-frame runs coinciding with the K.O. message,
  matching `Time_Data[1]` in `effect/eff84.c`, on every segment. That our
  `effect_84_move`'s `Game_pause = 1` window is frame-for-frame co-extensive
  with the archive's -1 window is proven by the assert passing on all 182
  segments — but "the arcade's `effect_84` equivalent is the writer" is an
  inference from the duration, not a read-out of the program.
- **The value difference itself is a real, unadjudicated port/arcade
  difference.** Our `Game_pause` is `u8`; the arcade's is 16-bit and takes -1.
  Every arcade-relevant consumer is a zero test, so it is inert here, and the
  two port consumers that would notice — `sc_sub.c`'s `Game_pause & 0x80` and
  `menu.c`'s `(Game_pause & 0x7F)` — are training-mode paths. Changing the
  port's value would be an engine change (`ArcadeBalance_IsEnabled()` gate
  required) touching `netplay/game_state.h`'s serialized `u8`, and it would buy
  nothing measurable. It is recorded, not fixed.
- **`EXE_obroll` is the third member of this family and is still unassertable.**
  Its CPS3 address has never been established (E6's "Still open" says the same),
  so there is nothing to compare against. Worse, on our side it is
  **structurally dead**: the whole port has exactly ONE writer,
  `EXE_obroll = 0` in `engine/manage.c`, against 55 readers — so every
  `&& !EXE_obroll` is permanently true and `eff53.c`'s
  `if (EXE_flag || Game_pause || !EXE_obroll)` body is unreachable. Whether the
  arcade drives it is unknown and needs a disassembly pass, not more corpus.
- **Device test.** Host-only, as with E4-E8.

## Harness false positives — fix these before trusting a statcheck sweep

**All eleven** observed failures turned out not to be engine bugs. Each had a
concrete cause; until they were handled, a statcheck sweep over multi-game
sessions reported divergences that are not there. Three of the four causes are
repairs (H1, H2, H3); the fourth (H4b) is a limit the harness now declares
instead of hiding.

**H1, H2, H3 and H4b are now fixed. The corpus reports zero divergences.**
Over the same 16-segment corpus, on the same build tree
(`/Volumes/KimchDrive/3sarm-convert-tmp/rerun2`, `build/statcheck-verify`):

| | original | after H1-H3 | after H4b |
|---|---|---|---|
| PASS (rc=0) | 5 | 6 | **6** |
| divergence (rc=1) | 11 | 8 | **0** |
| no match in segment (rc=2) | — | 2 | **2** |
| CPU player, not reproducible (rc=3) | — | — | **8** |

Every one of the eleven original failures had a harness cause. The last eight —
6 × `Random_ix32` (E2b) and 2 × `pos.x` (E3) — were all human-vs-CPU segments
being replayed under the wrong `Play_Type`; H4b rejects them by type instead of
reporting them. Every previously-passing segment still passes with a
byte-identical compared-frame range (verified: all six `PASS — compared archive
frames a..b of n` lines are string-identical before and after).

**What this costs.** 8 of 16 segments are no longer checked at all, and 2 more
hold no match, so 6 of 16 are usable ground truth. That is the honest number:
the other ten were never being measured correctly.

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

### H2 — the stage is never imported, so the appearance table is wrong (FIXED)

`appear_data_init_set()` (`appear.c`) picks
`app_type_tbl[own][opp][bg_w.stage]`, and `appear_data_set()` writes both
`wu.routine_no[4]` and `wu.xyz[0].disp.pos` from that entry. `ScrdGame_Init()`
read characters, supers, new_challenger and colours — **never the stage**. On
the arcade the stage carries across matches, so the harness's synthetic
char-select cannot reconstruct it. It usually does not bite because
`app_type_tbl` is stage-flat for most matchups — which is why most games pass.

**The CPS3 offset, by disassembly.** An empirical scan was not used, and must
not be: a residue or delta test over a short window is blind to a constant
offset, which is exactly how the first `players_timer` candidate came out a
false positive. The method is §23.4 of
`docs/research-arcade-cg-data-accuracy.md` — find a table that occurs once in
the decrypted image, find its sole literal referrer, read the address off the
instruction that uses it.

- `app_type_tbl` @ CPS3 `0x0619E950`, `app_type_tbl2` @ `0x061A10EF`, each
  **exactly one** occurrence. The arcade tables are `[21][21][23]`; the port's
  `[20][20][22]` is byte-for-byte the arcade table with character index 15 and
  one stage column removed.
- Their literal-pool slots are unique and adjacent (`0x060C0220`,
  `0x060C021C`), both in one function: **`appear_data_init_set` @ CPS3
  `0x060C00E8`**. It loads `&bg_w = 0x02026BAC` (`mov.l 0x60c0214,r7`) and
  reads the index with **`mov.b @(4,r7),r0`**.
- Independent second site: `Appear_07000`'s `bg_w.stage == 12 && bg_w.area == 0`
  test loads the **whole address** as a literal — `mov.l 0x60c0b30,r3` where
  `[0x060C0B30] = 0x02026BB0` (and `0x02026BB1` for `area`).

So **`BG_W_STAGE_OFFSET = 0x26BB0`**. Note the trap this closes: the arcade
`BG` has one extra byte ahead of `stage` (arcade `stage` at `+4`, `area` at
`+5`; the port's `bg.h` has `+3`/`+4`), so the offset must be written
literally and never derived from a base plus the port's `offsetof`.

**Corroboration against the archives.** The byte at `0x26BB0` was read on every
frame of all 11 segments: constant across each whole segment, always in
`0..21`, always equal to one of the two players' arcade character ids (a home
stage), and different across segments (`11, 1, 1, 7, 5, 3, 3, 12, 12, 10, 2`).
Feeding those into `app_type_tbl` predicts the archives' `routine_no[4]` **six
for six**, and four of the six are *uniquely* solvable from the table alone —
stage 7, 3 and 12 respectively, each equal to the byte read. A constant-offset
neighbour cannot produce that, which is what makes this not the `players_timer`
failure mode.

**The index space is the character index space** — verified on both sides, not
assumed. `Setup_Battle_Country()` (`sel_pl.c`) returns `My_char[...]` verbatim,
so the port's `bg_w.stage` is a 3SX character id; the arcade's byte is an
arcade character id. `CHAR_ARCADE_TO_3SX` (`constants.h`) is therefore the
correct transform over the whole range, and it also names the stage column the
port dropped: index 15, `CHAR_SHIN_AKUMA`. That is why the port's table is
`[20][20][22]` against `[21][21][23]`.

**Fix (this commit).** `ScrdGame_Init()` imports the stage through
`CHAR_ARCADE_TO_3SX`, and the runner pins it at PHASE_MENU with
`Debug_w[DEBUG_STAGE_SELECT] = stage + 1` — the engine's own override, so
`Exit_2nd()` (`sel_pl.c`) applies it *and* issues `Push_LDREQ_Queue_BG()` for
the pinned stage. The same override the DEBUG harness uses
(`test_runner.c` -> `apply_stage_override`). No engine code changed.

**Measured, 16-segment corpus** (same corpus and build tree as H1/H3):

| segment | before | after |
|---|---|---|
| `5743 game_3` | `routine_no (16) != 15` @ frame 11 | gone; now `Random_ix32` @ 234 (E2b) |
| `7733 game_1` | `routine_no (3) != 1` @ frame 11 | gone; now `Random_ix32` @ 322 (E2b) |
| `7733 game_4` | `routine_no (3) != 1` @ frame 11 | gone; now `Random_ix32` @ 247 (E2b) |
| the 6 segments that passed | PASS | PASS, identical compared-frame ranges |
| the other 5 failures | (see table above) | unchanged: same file, line, values, frame |

Three reports that looked like distinct engine bugs collapse into the E2b
bucket, which is the point of fixing a false positive.

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

### H4b — the harness forced `Play_Type == 1` on every segment (FIXED, by rejection)

**Product note (user, 2026-09-05): the rejection is not a cost.** Human-vs-CPU
replays are not content anyone wants in the viewer — a weekly-best set exists to
show human matches. So dropping them is the desired behaviour independently of
whether the harness could reproduce them, and the "costs 57% of the corpus"
framing elsewhere in this document should be read as *of the ground-truth
corpus*, not of anything shippable. Do not spend effort trying to recover CPU
segments; the H4b investigation additionally showed they are not reproducible
from the archive (the AI reads cabinet state the archive does not carry, and
clearing `wu_operator` discards the archive's own pinned input word).

`statcheck_runner.c` taps `SWK_START` for player 2 at
`PHASE_CHARACTER_SELECT`, and `ScrdGame_Init()` never imported `wu_operator`.
So **every** statcheck run had two human operators: `set_base_data()`
(`plcnt.c`) copies `Operator_Status[ix]` into `plw[ix].wu.wu_operator`, and
`Setup_Play_Type()` (`sys_sub.c`) makes that `Play_Type == 1`. On the eight
corpus segments the cabinet recorded against the CPU, hardware ran
`Play_Type == 0` with `cpu_algorithm()` driving one side and the port ran
neither. That, not any engine defect, produced all eight remaining failures
(E2b's six `Random_ix32` and E3's two `pos.x`).

`plw[i].wu.wu_operator` is WORK offset 3 — archive `0x68C6F` and `0x69107` —
and reading it at the match-start frame partitions the corpus exactly:

| `wu_operator` at start | segments | verdict before H4b |
|---|---|---|
| (1,1) human vs human | 6 | all PASS |
| (1,0) / (0,1) human vs CPU | 8 | all divergence |
| (0,0) attract demo | 2 | no match (H1, rc=2) |

#### Why the fix is a rejection and not "reproduce the CPU player"

Importing `wu_operator` so `Player_move()` routes through `cpu_algorithm()`
would, on paper, make eight more segments usable. It does not work, for three
independent reasons — the first two read off the port's own sources, and the
first and third were then confirmed by experiment.

**1. It disables the oracle's input pinning on exactly those segments.**
`Player_move()` (`plmain.c`) is

    if (wk->wu.wu_operator) { wk->cp->sw_lvbt = lv_data; }
    else { wk->cp->sw_lvbt = processed_lvbt(cpu_algorithm(wk)); }

With the flag clear it **discards `lv_data`** — the archive's own button word,
which `read_input_buff()` (`statcheck_runner.c`) feeds it every frame. The test
stops being "same inputs, same state" and becomes "re-derive the AI", in which
a single wrong frame is unattributable. Measured: with the operators imported
and nothing else changed, both E3 segments fail at **frame 7** on
`w_lvr (-32760) != (-32764)` — the lever work itself, i.e. the pinned input.

**2. The AI reads cabinet state the import set does not carry.**
`Setup_Lv18(save_w[Present_Mode].Difficulty)` (`com_pl.c`, `com_sub.c`) and
`asagh_zuru[save_w[Present_Mode].Difficulty]` in `add_sp_arts_gauge_paring()` /
`_tokushu()` / `_ukemi()` / `_nagenuke()` (`pls02.c`, each guarded by
`wu_operator == 0`) all index the cabinet's service difficulty.
`Statcheck_SyncValues()` (`statcheck_compare.c`) imports `Random_ix16`,
`Random_ix32`, `players_timer`, `t_pl_lvr` and `Round_Level` — and nothing
else. `arcade_constants.h` has no offset for `Difficulty` and none was derived,
so the AI's difficulty scaling cannot be reconstructed from the archive.
(`com_pl.c` and `com_sub.c` between them reference 87 of the globals defined in
`workuser.c`; `Com_Initialize()` resets most of them at battle start, which is
why this entry rests on `Difficulty`, an input it demonstrably does not reset,
rather than on the size of that surface.)

**3. `wu_operator == 0` inside the harness's VERSUS match is a state the
cabinet never had.** The CPU recordings are arcade-mode matches;
`StatcheckRunner_PinConfig()` forces `game-mode=console` because the arcade
path never reaches the `Menu_Task` r_no sequence the phase machine watches, so
the harness always synthesises a console VERSUS match. Sites branch on the two
together — `cmb_win.c`'s
`(!ArcadeBalance_IsEnabled() && Mode_Type == MODE_VERSUS) || plw[PLS].wu.wu_operator`
and `sys_sub.c`'s
`(Mode_Type != MODE_VERSUS && Mode_Type != MODE_REPLAY) && plw[PL_id].wu.wu_operator == 0`
— so clearing the flag alone manufactures new divergences. Measured: with the
operators imported *and* the lever/wcp/waza comparisons suppressed so the
frame-7 failure above cannot mask it, both segments fail at **frame 11** on
`routine_no (3) != (1)` — the appear state machine, 43 frames before E3's
symptom.

Design (a) is therefore not "hard"; it is refuted on this corpus. The load-
bearing property is the one H1 already established: **a segment the harness
cannot reproduce must never be reported as an engine divergence.**

#### Fix (this commit)

`ScrdGame_Init()` (`scrd_game.c`) reads `wu_operator` for both players at
`start_index` — using `WORK_WU_OPERATOR_OFFSET` (3), new in
`arcade_constants.h` — and returns `SCRD_GAME_INIT_CPU_PLAYER` when either is
0. `main.c` turns that into **exit code 3**, kept distinct from 1 (engine
divergence) and from 2 (no match in segment, H1) so a sweep can tell "cannot
reproduce this recording" from "nothing here to reproduce". Publication gating
is unaffected: `publish_3sr.py`'s `statcheck_gate` is `clean =
proc.returncode == 0`.

**Why sample at `start_index` and not "the dominant value".** Measured across
the corpus, four of the six (1,1) segments carry a short tail of (1,0)/(0,1)
frames at the very end — 142, 124, 82 and 72 frames — because `manage.c`
clears the loser's operator after the final KO (`Operator_Status[LOSER] = 0`,
`plw[LOSER].wu.wu_operator = 0`). The match-start frame is the value
`set_base_data()` latches and `Setup_Play_Type()` reads, so it is the one that
decides how the whole segment should have been replayed. It gives the exact
6 / 8 / 2 partition above with no ambiguity.

**Measured, 16-segment corpus** (same corpus and build tree as H1/H3):

| segment | before | after |
|---|---|---|
| the 8 human-vs-CPU segments | rc=1, 6 × `Random_ix32` + 2 × `pos.x` | `CPU-PLAYER`, rc=3, `wu_operator` printed |
| the 6 (1,1) segments | PASS | PASS, **string-identical** compared-frame ranges |
| the 2 (0,0) segments | `NO-MATCH`, rc=2 | unchanged |

No segment that passed starts failing, and the sweep now reports **zero** rc=1.

### H5 — `waza_work[][48..55]` is carried state, and the seed audit allowlisted it wholesale (FIXED, 2026-09-06)

**This is the 2026-09-06 corpus's D3 and D6, and it is a false-negative class in
the seed audit itself** — the instrument the rest of this document now leans on.
Neither of the two segments is an engine divergence; both were graded `rc=1` on
a seed the audit called CLEAN.

    1785912751200-1008 game_2   Ryu / Twelve     statcheck_compare.c  w_int  (0) != (-1)   @ archive frame 7
    1784875995078-5749 game_1   Twelve / Elena   statcheck_compare.c  w_type (0) != (1)    @ archive frame 7

Two unrelated quarks, different players, characters and stages, failing in the
same loop at the **same** archive frame 7 — which is the second frame
`compare_waza_work()` runs at all (`Statcheck_CompareValues` gates the three
input-history groups on `frame - start_frame > 5`). A frame index that does not
depend on the gameplay is a property of the harness's warm-up window, not of a
match.

#### The mechanism

`cmd_init()` (`cmd_main.c`), reached from `set_base_data()` at match start,
clears the command-recogniser working set — but under
`ArcadeBalance_IsEnabled()` it clears only the first 48 of the 56 entries:

    // CPS3 clears 0x540 bytes of each 0x620-byte command-state block, leaving entries 48-55 intact.
    SDL_memset(waza_work[cmd_id], 0, sizeof(WAZA_WORK) * WAZA_WORK_CARRIED_FIRST);

0x540 is 48 × 28 and 0x620 is 56 × 28. **So `waza_work[][48..55]` carries across
the match boundary, deliberately, on both sides.** That is the exact shape of
every defect the seed audit was built for: the archive enters a segment with the
previous match's residue in those eight entries, and a statcheck run enters it
with zeros, because its synthetic session has never played a match.

Measured directly. Decoding the archive (`WAZA_WORK_OFFSET 0x256C4`, 28 B per
entry, big-endian) over `1785912751200-1008_game_2`, entries 0..47 show a clean
period-2 oscillation from archive frame 2 onward — `(w_type, w_int)` alternating
`(1,0)` / `(0,-1)`, which is `check_init` and `check_1` taking turns through
`chk_move_jp[]` — while P1's entries 48 and 49 sit frozen at `(0,-1)` from frame
0 to the end. A per-frame dump of our own table against the archive's, over
frames 7-12, gives **two** differing fields on either segment and nothing else:

| segment | player | entry | ours | CPS3 |
|---|---|---|---|---|
| `1785912751200-1008 g2` | P1 (Ryu) | 48, 49 | `(0, 0)` frozen | `(0, -1)` frozen |
| `1784875995078-5749 g1` | P1 (Twelve) | 49 | `(0,-1)`/`(1,0)` | `(1,0)`/`(0,-1)` — **antiphase** |

`(0, 0)` is the freshly-zeroed state; the arcade's `(0, -1)` is what `check_0`
leaves behind (`w_int--; if (w_int < 0) { w_type = 0; }`). The Twelve case is
not frozen but **one frame out of phase**, because the 2-cycle's phase is fixed
by the entry state and nothing resets it.

#### The two sub-cases, and why they get different verdicts

`waza_compel_all_init()` (`cmd_main.c`) sets `waza_flag[i] = -1` for every index
outside the character's six live ranges, the last of which ends at
`pl_cmd_num[player_number][6]` (`cmd_data.c`); `plcnt_init()` (`plcnt.c`) sets
`wk->player_number = My_char[wk->wu.id]`. Reading that column:

    46 46 47 46 46 46 46 47 46 48 46 47 46 46 48 46 47 46 50 46

**`CHAR_TWELVE` (18) is the only one of the twenty that reaches 48** — its live
range ends at 50, so entries 48 and 49 are real commands for Twelve and dead for
everyone else. Entries 50..55 are dead for every character.

- **Dead entries (19 of 20 characters).** `cmd_main.c` gates *every* access to
  `waza_work[cmd_id][j]` on `waza_flag[j] != -1`: both loops in `cmd_move()`,
  `waza_compel_all_init2()`, and `cmd_data_set()` (called only for live
  indices). The residue is unreachable state for the whole match. The oracle
  compared it anyway — while `compare_wcp()` right next to it already skips
  `reset`/`btix`/`waza_r`/`exdt` on exactly this test. **Fix: `compare_waza_work()`
  now skips entries with `waza_flag[j] == -1` too.** Nothing is masked by using
  our own `waza_flag`: if the two sides disagreed about which entries are live,
  `compare_wcp()` asserts `waza_flag[j]` for all 56 indices on the same frame,
  immediately after.
- **Live entries (Twelve's 48 and 49).** The residue *is* behavioural — it is
  what put `1784875995078-5749 g1` in antiphase. That is a genuine imported-state
  gap. **Fix: the seed audit now audits `waza_work[i][j]` strictly for
  `j >= 48 && j < pl_cmd_num[My_char[i]][6]`**, so such a segment exits **4**,
  not 1.

#### Why it is not seeded, which is the honest limit

Closing this the way H3 closed `t_pl_lvr` — copy the residue out of the archive
in `Statcheck_SyncValues` — **does not work, and the reason is concrete.**
`WAZA_WORK::w_ptr` is a CPS3 address into the character's command table; on
`1784875995078-5749 g1`'s seed frame P1's entries 46-49 hold `0x0619BDF8`,
`0x0619BE32`, `0x0619BE64`, `0x0619BE96`. There is no map to a host pointer
(the same reason `WORK_CURR_RCA_OFFSET` is excluded from the audit). And the
residual `w_type` there is **1**, i.e. `chk_move_jp[1] == check_1`, which
dereferences `*waza_ptr->w_ptr`. Importing `w_type` without `w_ptr` would make
the port follow a pointer it does not have. So `rc=4` is the correct verdict for
a Twelve segment with dirty carried state, not a repaired PASS.

**The cost, stated:** a Twelve segment can no longer report `rc=1`. Three of the
185 segments in the 2026-09-06 corpus are affected (five contain Twelve; three
of those five have differing residue). That is the price of the rule H1/H4b
established — a segment the harness could not set up correctly must never be
reported as an engine divergence.

#### A correction to the corpus's own D3 write-up

D3 argued from "29 allowlisted seed differences on the failing segment against
21 on a passing one". That inference does not hold: `s_expected` counts
`wcp[]` and `waza_work[]` as **one** each, whatever their field counts, so
`waza_work` contributes exactly 1 to both totals. Run at
`STATCHECK_SEED_AUDIT_VERBOSE=1` the two segments report `waza_work[] 188 of
1344` and `180 of 1344` differing fields respectively; the 29-vs-21 gap is
entirely the per-player WORK/PLW residue group plus `Scene_Cut` and `C_No[0]`.
The conclusion D3 reached was right; the number it reached it with was not.

#### Result

`--headless`, both corpora swept per-segment with the same binaries, before and
after:

| | 2026-09-06 (185) | 2026-09-05 (143) |
|---|---|---|
| before | 177 pass / 6 rc=1 / 2 rc=3 | 143 pass |
| after | **178 pass / 4 rc=1 / 1 rc=4 / 2 rc=3** | **143 pass** |
| segments whose rc, fail frame or assert changed | **2** | **0** |

`1785912751200-1008 g2` moves 1 -> 0 (its residue was on Ryu's dead entries);
`1784875995078-5749 g1` moves 1 -> 4 (Twelve's live entry 49). No other verdict,
frame or assert line moves anywhere in either corpus. Seed verdicts go DIRTY on
**3 of 185** and 0 of 143 — the audit stays quiet except where it now has
something true to say. Frame-data suite: 99 GREEN, zero drift.

**No engine behaviour changed.** `cmd_init()`'s literal 48 became
`WAZA_WORK_CARRIED_FIRST` (`cmd_data.h`) so the harness keys off the engine's own
boundary instead of duplicating a magic number. Proven a textual no-op rather
than argued: preprocessing `cmd_main.c` with the host build's own flags emits

    __builtin___memset_chk (waza_work[cmd_id], 0, sizeof(WAZA_WORK) * 48, ...)

— the macro is gone by the time the compiler sees the translation unit, and it
is the only engine edit. The two real changes are both in `src/test/`, which
needs no `ArcadeBalance_IsEnabled()` gate.

### H5b — the carried entries ARE seedable: `w_ptr` is `&tbl[16]`, not an arbitrary address (FIXED, 2026-09-06)

H5 closed the false negative and left a residue: a Twelve segment whose carried
`waza_work[][48..49]` differs could report only rc 0 or rc 4, never rc 1,
because the state was judged unimportable. Across the three corpora that came
to **five** `rc=4` segments — every non-PASS in 447 eligible segments, and every
one of them Twelve:

| segment | characters | Twelve's side |
|---|---|---|
| `1785200151999-8704` g1 | Makoto / Twelve | 1 |
| `1785200151999-8704` g2 | Makoto / Twelve | 1 |
| `1785803982634-6917` g1 | Twelve / Chun-Li | 0 |
| `1785823449983-1131` g10 | Twelve / Necro | 0 |
| `1784875995078-5749` g1 | Twelve / Elena | 0 |

All five `wu_operator = [1,1]`, all five failing `compare_waza_work()` at
archive frame 7, all five naming the **same seven fields with the same seven
values**. Values that repeat across five unrelated matches are not previous-
match residue; they are constants. That is the thread this section pulls.

#### The discriminator: one line of the audit's own output

Sixteen segments in the three corpora contain Twelve. Five carry no residue at
all (their side had not yet played Twelve in that session, and nothing but a
Twelve match ever writes entries 48/49 — they are dead, and so never written,
for the other nineteen characters). The remaining **eleven** all report a DIRTY
seed naming exactly seven fields, and those eleven split into two signatures
that differ **in one line**:

    waza_work[S][48].w_int    ours=0 cps3=-1        both
    waza_work[S][48].w_lvr    ours=0 cps3=-32766    both
    waza_work[S][48].w_dead   ours=0 cps3=1         both
    waza_work[S][49].w_type   ours=0 cps3=1         <-- the 5 that FAIL
    waza_work[S][49].w_int    ours=0 cps3=-1        <-- the 6 that PASS
    waza_work[S][49].w_lvr    ours=0 cps3=8         both
    waza_work[S][49].w_dead   ours=0 cps3=4         both
    waza_work[S][49].w_dead2  ours=0 cps3=2         both

Entry 49's `w_type` at the seed frame predicts the verdict on **16 of 16**
segments. Everything else about the residue is identical.

#### The mechanism: a period-2 idle cycle whose phase is all that carries

An idle recogniser entry runs a closed loop over the STATIC command table, and
`cmd_move()` hands `chk_move_jp[]` that table on every frame:

- `check_init()` (`w_type == 0`) does `cmd_tbl_ptr += 12` then four
  post-increments, reloading `w_type`/`w_int`/`free1`/`free2`/`w_lvr` from
  `tbl[12..15]`, setting `w_ptr = &tbl[16]`, zeroing `tame.*`/`free3`/`shot_ok`
  — and then **dispatches the loaded handler in the same frame**;
- that handler, with no matching lever, restores `free2 = free1`, decrements
  `w_int`, and drops `w_type` back to 0 once it goes negative (`check_0`, and
  both arms of `check_1`).

Twelve's entry 48 is `unk_cmd_186` (`arcade_cmd_data.c`), whose `tbl[12..15]`
are `{1, 0, 0, -32766}`: `check_init` loads `w_type = 1, w_int = 0`, `check_1`
decrements to `-1` and resets `w_type` to 0 — so entry 48 sits at
`(w_type 0, w_int -1)` at **every** frame boundary. Entry 49 is `unk_cmd_187`,
`tbl[12..15] = {1, 1, 0, 8}`: `w_int` starts one higher, so the entry alternates
`(1, 0)` and `(0, -1)` with period 2 and **never settles**.

Both sides run that cycle. Our side enters the match with the entries zeroed and
therefore always lands on `(0, -1)` at frame 7. The archive enters wherever the
previous Twelve match left it. When that is the even phase the two agree; when
it is the odd phase they stay one frame apart forever, and `compare_waza_work()`
says so at frame 7 — the second frame it runs at all. Nothing about the
gameplay is involved, which is why the fail frame was 7 on all five.

Every one of the seven differing values is now accounted for as a table
constant: `w_dead`/`w_dead2` are `tbl[1]`/`tbl[2]` (and are rewritten on both
sides by `waza_compel_all_init()` at match start, so they were never
load-bearing), `w_lvr` is `tbl[15]`, and `w_type`/`w_int` are the phase.

#### The correction: `w_ptr` in this state is `&tbl[16]`

H5 recorded the blocker as "`WAZA_WORK::w_ptr` is a CPS3 address into the
command table (measured: `0x0619BDF8`, `0x0619BE32`, `0x0619BE64`, `0x0619BE96`)
with no map to a host pointer". True of an arbitrary pointer. **Those four are
not arbitrary.** Their consecutive gaps are `0x3A`, `0x32`, `0x32` = 58, 50, 50
bytes — exactly `sizeof(unk_cmd_184[29])`, `sizeof(unk_cmd_185[25])`,
`sizeof(unk_cmd_186[25])` as our own transcription has them. So entries 46..49
sit at the same relative offset inside four consecutively laid-out tables, and
that offset is the one `check_init()` produces and nothing in the idle cycle
moves, because `check_next()` — the only other writer of `w_ptr` — is reachable
only through a `tame.flag` branch the cycle never takes.

And the reconstruction target is not the CPS3 pointer anyway. Everything that
will read `w_ptr` afterwards (`check_1`, `check_next`, `check_0`) is **our** code
reading **our** table, so the value we need is the one our own `check_init()`
would have written: `&tbl[16]`. That is available by construction.

#### The fix

`sync_waza_work_carried()` (`statcheck_compare.c`), called from
`Statcheck_SyncValues`, imports the twelve compared scalar fields for
`j` in `[WAZA_WORK_CARRIED_FIRST, pl_cmd_num[My_char[i]][6])` and
**reconstructs `w_ptr`** rather than importing it. The loop body is unreachable
for nineteen characters: `pl_cmd_num[c][6] <= 48` for every `c != CHAR_TWELVE`,
and Twelve's 50 admits exactly entries 48 and 49.

The guard is the safety argument, so it is narrow. An entry is seeded only when
its archived state is one the idle cycle can produce from `tbl` — `w_lvr`,
`free1`, `free2`, `w_dead`, `w_dead2` at their table values, `tame.*`, `free3`,
`shot_ok` zero, and `(w_type, w_int)` either post-init (`tbl[12]`, `0 <= w_int <
tbl[13]`) or expired (`0`, `-1`). Anything else — a mid-command state left by
`check_next()`, a charge in progress — means `w_ptr` is somewhere we cannot
name, so that entry is left alone, the audit still reports it DIRTY, and the run
still exits **4**. H5's protection is intact; what changed is that the states
this corpus actually contains are no longer among the ones we give up on. An
all-zero archive entry cannot pass the guard either: `expired` needs
`w_int == -1`, and the post-init arm needs `w_type == tbl[12]`, which is never 0
(0 is `check_init` itself).

Gated on `ArcadeBalance_IsEnabled()` as a **predicate, not a behaviour switch**:
it is the condition `cmd_init()` keys the partial clear off, so outside it
`SDL_zeroa(waza_work[cmd_id])` wipes all 56 entries at match start and there is
no carried state to seed. No engine file was touched.

#### Result — three corpora, before and after, `--headless` throughout

| | 2026-09-05 (143) | 2026-09-06 (185) | 2026-09-06b (135) |
|---|---|---|---|
| before | 143 PASS | 182 PASS / 1 `rc=4` / 2 `rc=3` | 117 PASS / 4 `rc=4` / 12 `rc=3` / 2 `rc=2` |
| after | **143 PASS** | **183 PASS / 0 `rc=4` / 2 `rc=3`** | **121 PASS / 0 `rc=4` / 12 `rc=3` / 2 `rc=2`** |

**447 of 447 eligible segments PASS.** Eleven verdict lines moved and no others:
the five `rc=4 -> rc=0`, and six segments that were already `rc=0` with a DIRTY
seed going CLEAN — the audit had been correctly reporting a gap on those too,
and closing it silenced them. The five that clear are not clearing on a
technicality; they compare **3,657 / 3,680 / 7,410 / 4,208 / 5,019** archive
frames apiece.

Coverage of "nothing else moved" is split, deliberately, because the two halves
warrant different evidence. For the **16 Twelve segments** the full
`PASS — compared archive frames a..b of n` line was captured on both binaries:
the only five that differ are the five that had no PASS line before, and the
five zero-residue Twelve segments are untouched down to their allowlisted-
difference counts. For the other **447 segments the loop is statically
unreachable** — it runs `j` from 48 to `pl_cmd_num[My_char[i]][6]`, and that
bound is at most 48 for all nineteen non-Twelve characters, so
`sync_waza_work_carried()` reads nothing and writes nothing on them; their
verdict lines (rc, fail frame, assert, seed verdict) are measured identical on
top of that.

#### Controls

- **The seed is live.** Force the guard false so nothing is ever seeded: all
  sixteen Twelve segments return **exactly** to the pre-fix line, the five back
  to `rc=4` with the same seven `MISMATCH` lines. The seed, not some incidental
  difference between the two binaries, is what moved the verdicts.
- **`w_ptr` is never dereferenced from the seeded state on this corpus — stated
  as the negative it is.** Skewing it one word (`&tbl[15]`) changes nothing on
  all sixteen. That is ambiguous between "never read" and "read but
  insensitive", so a second control replaces it with a pointer whose every word
  is `28`, the `command_ok()` sentinel — any dereference before `check_init()`
  rewrites it would fire the command and diverge loudly. **Still 0 of 16
  differ.** So the corpus does not test the reconstruction; `&tbl[16]` rests on
  `check_init()`'s own arithmetic and on the pointer-gap evidence above, not on
  a passing sweep. The flip side is that no wrong-but-green outcome can come
  from `w_ptr` here: the seed's entire effect on this corpus is the twelve
  scalar fields, and in practice `[49].w_type`.
- **Frame-data suite:** 99 GREEN, zero drift.

---

## The seed audit (2026-09-05)

Seven defects in this body of work shared one shape: **the arcade carries state
across a match boundary and the harness resets it.** `Round_Level` (E1a),
`bg_w.stage` (H2), `t_pl_lvr` (H3), `players_timer` (E2a), `wu_operator` (H4b),
the match-start predicate (H1) and `bg_w.quake_y_index` (E5). Each cost a
separate investigation, and **two were written up as engine defects and reported
before being retracted.**

The cost pattern never varied: the seed is wrong at frame 0, nothing notices,
and the mismatch surfaces at archive frame 300 or 3,090 looking exactly like an
engine bug. `Game_timer` at frame 1, `s1_cnt` at 7, `routine_no` at 11 — each
burned an investigation.

**The invariant that makes this cheap to catch: at the seed frame our engine has
not executed any compared frame yet.** Anything that differs there is an initial
condition, not behaviour. The audit therefore needs no model of the simulation.
It only has to notice.

### Where it runs

`StatcheckSeedAudit_Run` (`src/test/statcheck_seed_audit.c`), called from
`StatcheckRunner_Prologue`'s `PHASE_GAME_TRANSITION` arm (`statcheck_runner.c`)
**immediately after `Statcheck_SyncValues` and before `SDL_CloseIO`** — the same
`start_index - 1` frame the import reads. Running after the import is what makes
the five seeded fields a self-test of the import rather than a restatement of it.

It is read-only. Every engine reference in the file is on the right of a
comparison or an argument; the only assignments are to file-static counters and
locals. The behavioural proof is the sweep below: all 143 `PASS — compared
archive frames a..b of n` lines are **string-identical** before and after.

### What it audits — 170 fields — and what it cannot

`arcade_constants.h` carries ~46 offsets. Not all of them name a value that can
be soundly compared, and the audit does not invent a comparison it cannot
justify:

| excluded | why |
|---|---|
| `PLW_OFFSET`, `PLW_SIZE`, `T_PL_LVR_OFFSET`, `WAZA_WORK_OFFSET`, `WCP_OFFSET`, `SUPER_ARTS_WORK_OFFSET`, `PIYORI_TYPE_OFFSET` | struct bases and a stride, not fields — audited through the fields reached from them |
| `P1SW_0_OFFSET`, `P2SW_0_OFFSET` | raw arcade register layout (kicks at bits 7-9) against the port's SWK layout (kicks 8-10, bit 7 unused). `read_input_buff` (`statcheck_runner.c`) exists precisely because the two are different encodings; a bit-for-bit compare would be fabricated and a re-encoded one would only re-test `read_input_buff` |
| `WORK_CURR_RCA_OFFSET` | `CatchTable* curr_rca` (`include/structs.h`) — the archive holds a CPS3 address, our engine a host address, and there is no map between them |

Everything else is audited: the five `Statcheck_SyncValues` imports, the
`ScrdGame_Init` match setup (`My_char`, `Super_Arts`, `Player_Color`,
`New_Challenger`, `bg_w.stage`), the service globals, both players' WORK/PLW
scalars, and all 34 `T_PL_LVR` fields per player, plus the seven cabinet /
service globals and the two `save_w` service settings added when the seeding
gap was closed ("The seven, resolved"), and the freeze pair `EXE_flag` /
`Game_pause` (§FP). 170 comparisons per run — it was 150 when the audit landed,
158 by the time those ten were added, and 168 before the freeze pair.

**`t_pl_lvr[].waza_no` is new here.** `compare_lvr()` (`statcheck_compare.c`)
compares 33 of the struct's 34 fields; upstream's list omits `waza_no`. Measured
at the seed frame over the 143-segment corpus, **it is the only one of the 34
that is ever non-zero** — 71 of 80 sampled player-slots carry a value in 2..47,
every other field is 0 on both sides. Without it the H3 import has nothing to
check against on that corpus, which is exactly what the first retrodiction run
showed. `read_t_pl_lvr` already copies the whole struct, so auditing it is free.

### The allowlist

A field is audited **strictly** when the harness is responsible for reproducing
it at the seed frame. It is allowlisted when our own match-start path provably
rewrites it before the oracle ever compares it — the seed value is then not an
initial condition at all. Every entry carries its reason in the code.

**Everything on this list is compared by the oracle later**, which is what makes
the "provably rewrites it" half load-bearing rather than decorative: an
allowlist entry whose rewrite claim is false does not go unnoticed, it goes
*misattributed* — the oracle still catches the difference, hundreds or thousands
of frames on, and the audit's silence promotes it from a harness gap (rc 4) to
an engine divergence (rc 1). That is exactly what H5 was, and it is the failure
mode to check for whenever an entry is added here: not "will this be missed?"
but "if the rewrite claim is wrong, who gets the blame?".

| allowlisted | reason |
|---|---|
| `Game_timer`, `C_No[0..3]`, `G_No[2]`, `Allow_a_battle_f` | `Game2_0()` (`game.c`) writes `Game_timer = 0; C_No[0..3] = 0; G_No[2] = 3; Allow_a_battle_f = 0` in one frame, on both sides, on the frame **after** the seed frame — that is H1's predicate, so it is true by construction. The carried-in `Game_timer` here is H1's diagnostic law (it equals `len(previous segment) - 2`) |
| `G_No[0]` | `compare_service_values()` excludes it too (`if (i != 0)`); auditing it would report a difference the oracle itself declines to make |
| `Scene_Cut` | `Game02()` (`game.c`) recomputes it as its **first** statement every frame — `Scene_Cut = Cut_Cut_Cut();` (`sys_sub.c`), a pure function of the current buttons — before dispatching `Game02_Jmp_Tbl[G_No[2]]` |
| `waza_type[0..1]` | scratch, not state: its only writer is `waza_type[cmd_id] = j` inside `cmd_move()`'s 56-entry loop (`cmd_main.c`), every frame |
| `wcp[]`, `waza_work[][0..47]` (aggregate) | `Statcheck_CompareValues` itself skips them for 5 archive frames ("Wait a bit so that the game has time to clear garbage values"); they self-correct from the injected button word, and `cmd_init()` (`cmd_main.c`, called by `set_base_data()`) zeroes them at battle start. Reported as two counts, not per field. **`waza_work[][48..55]` is NOT in this group** — `cmd_init()` deliberately leaves those eight entries intact under `ArcadeBalance_IsEnabled()`, so they are carried state and the live ones are audited strictly. See H5 |
| `Game_pause` | `Game2_0()` writes `Game_pause = 0` two statements after `Game_timer = 0` — the same block, the same frame, on both sides. MEASURED and not merely argued: the archive carries `Game_pause == 1` into the seed frame on **50 of 183** eligible 2026-09-06 segments and **38 of 143** 2026-09-05 ones (the tail of a `Game_Manage_*` transition), while OUR side holds 1 on **all** of them — our synthetic session is in its own transition — and the archive holds 0 at `start_index`, the first compared frame, on **183 of 183** and **143 of 143**. Strict would have turned an rc-1 run into rc 4 on 130 of 183 segments for nothing. The rewrite claim is checked, not trusted: `compare_service_values()` asserts `Game_pause` on every compared frame, so a value that failed to reach 0 by `start_index` fails on the first compared frame (§FP) |
| the per-player WORK/PLW battle group | structural: at the seed frame the archive holds the **previous match's** players while our synthetic session has never played one (`plw` reads all-zero, measured on all 143). "Fresh vs residue" cannot say whether the harness reproduced anything — and cannot hide a defect either, because `set_base_data()` (`plcnt.c`), `plcnt_init()` and `appear_data_set()` (`appear.c`) rebuild all of it, and the oracle only reaches this group when `G_No[1] == 2 && G_No[2] == 1`, strictly later |

`wu.wu_operator` is the **exception inside that last group and stays strict**: it
is not previous-match residue, it comes from the harness's own synthetic
character select (`Entry_Mark_Set` -> `Operator_Status[]`, `entry.c` ->
`set_base_data()`), so it is the harness's responsibility — and it is the field
H4b's whole rejection rests on. Measured equal on all 143.

Allowlisted differences are **counted always, printed only under
`STATCHECK_SEED_AUDIT_VERBOSE=1`**. Twenty of them on every clean run is exactly
the noise that lets a real one go unread. `=2` dumps every audited field whether
it matches or not — that level is what turns the audit into a positive control,
below.

### Fail or warn: a fourth typed exit code, and only on an already-failing run

A seed mismatch is not an engine divergence, so exiting 1 would be wrong. But a
warning that can be ignored does not stop the failure mode this exists for. The
resolution keeps both properties:

- **rc 0 is untouched.** A clean run with a dirty seed still passes and still
  publishes. `publish_3sr.py`'s `statcheck_gate` is `clean = proc.returncode
  == 0`, so a hard failure here would reject segments that are fine in practice
  — 143 of 143 pass today, and several of them *would* be dirty if any import
  were removed.
- **A comparison failure with a dirty seed exits 4, not 1.** `stop_if`
  (`statcheck_compare.c`) consults `StatcheckSeedAudit_Dirty()` and prints why.
  This is the rule H1 (exit 2) and H4b (exit 3) already established: a segment
  the harness could not set up correctly must never be reported as an engine
  divergence, because that is the report that gets acted on.

The point is not that 4 is softer than 1. It is that **1 gets stronger**: after
this change, rc 1 means "the engine diverged from CPS3, with a seed the audit
says was correct". Both of the retracted engine-defect reports would have been
rc 4.

### Does it retrodict the defects it was built for?

Method: disable exactly one import, rebuild, run, and check the audit names that
field at the seed frame. **Two of the four historical defects reproduce
outright; the other two are not reproducible on either corpus and the audit's
detection path for them is demonstrated by a deliberate skew instead.**

| defect | experiment | result |
|---|---|---|
| **`players_timer` (E2a, `f63507b7`)** | drop `players_timer = read_u16(io, PLAYERS_TIMER_OFFSET)` from `Statcheck_SyncValues` | **RETRODICTED. 143/143** segments go DIRTY with `MISMATCH players_timer [seeded] ours=0 cps3=<archive value>` at the seed frame. 126 of them then fail, and every one fails on `Random_ix16` — E2a's exact signature — so all 126 exit **4** where before this change they would have exited 1 and read as an engine divergence. The remaining 17 pass anyway |
| **`t_pl_lvr` (H3, `cbbbcf25`)** | drop `read_t_pl_lvr(io, t_pl_lvr)` | **RETRODICTED twice over.** (1) On the segment the write-up names: `7733 game_6` (16-segment corpus) goes `rc=4` naming **ten** fields at seed frame 0, including `s1_cnt ours=0 cps3=16` — the exact 16-count head start H3 measured — and its downstream failure is `lvr_3sx->s1_cnt (6) != lvr_cps3->s1_cnt (22)` at archive frame 7, the H3 report verbatim. (2) On the 143-segment corpus, **143/143** go DIRTY naming `t_pl_lvr.waza_no` — and every one of those 143 still exits **0**, because the oracle does not compare `waza_no`. That is the audit catching an imported-state gap the oracle is completely blind to, which is the case it was built for |
| `Round_Level` (E1a, `1de4c7b5`) | drop the import | **not reproducible on this ground truth.** Measured with `=2`: `Round_Level` is **3 on both sides of all 143 segments** and of all 6 usable 16-segment ones. Our `setup_vs_mode()` seeds 3 and the arcade's `Before_Select_Sub` sets 3 for VS play; E1b established that the arcade's decrements are `Play_Type`-gated, so a human-vs-human corpus never moves it. The import is **inert on this ground truth**, and the audit's silence is a true negative, not a blind spot. Detection control: skew the import `+1` -> **6/6** usable segments `rc=4` naming `Round_Level ours=4 cps3=3`, downstream `vital_new` off by one — E1a's damage-scale mechanism, end to end |
| `bg_w.stage` (H2, `a3d7af69`) | remove the `Debug_w[DEBUG_STAGE_SELECT]` pin | **not reproducible either.** With the pin gone the stage still matches on 143/143 and 6/6: our synthetic character select already derives the same home stage, because `New_Challenger` **and** `Champion` are now pinned too (`statcheck_runner.c`, upstream #289) and `Setup_Battle_Country()` (`sel_pl.c`) returns `My_char[...]` verbatim. H2's pin is **redundant on today's corpora** — which is a finding, not a reason to remove it, since the stage varies across 13 values and the audit now proves the pin lands. Detection control: pin the **wrong** stage -> **6/6** `rc=4` naming `bg_w.stage`, downstream `pos.x`, `routine_no` and `Random_ix16` — H2's three original symptom classes |

So the honest scoreboard is **2 retrodicted (one of them on two independent
corpora), 2 shown inert on the available ground truth with the detection path
demonstrated separately by a deliberate skew**. The two inert
ones are a statement about the corpus, not about the audit: both need a
recording the corpus does not contain (a 2P break-in for `Round_Level`, a
mismatched home-stage for `bg_w.stage`).

### Clean-corpus result

`/Volumes/KimchDrive/3sarm-corpus-2026-09-05`, 143 segments, `--headless`, two
binaries built from the **same isolated worktree at HEAD**, differing only by
this change (a second Claude session was editing engine files in the shared
checkout at the time, so the sweep was moved off it deliberately):

| | before | after |
|---|---|---|
| rc 0 | 143 | **143** |
| rc 1/2/3/4 | 0 | **0** |
| `PASS — compared archive frames a..b of n` lines that changed | — | **0 of 143** |
| seed verdict | — | **CLEAN on 143 of 143** |

**No seed mismatch anywhere on the corpus.** That is the expected result — every
known gap is closed — and it is the reason the audit could be landed without
changing a single verdict.

Per-segment the audit records 19-24 allowlisted differences (the previous
match's player residue, plus `Scene_Cut`, `waza_type`, `Game_timer` and `C_No`).

### What the `=2` dump says about the seeds themselves

Running the audit at `STATCHECK_SEED_AUDIT_VERBOSE=2` over the corpus prints all
every audited field whether it matches or not, which answers a question a clean verdict
cannot: *is this field clean because the import works, or because both sides
happen to hold the same value anyway?*

| seeded field | distinct CPS3 values at the seed frame, 143 segments | verdict |
|---|---|---|
| `players_timer` | **109** (1857..32629) | import is load-bearing and verified |
| `Random_ix16` | 53 (0..63) | load-bearing, verified |
| `Random_ix32` | 37 (5..118) | load-bearing, verified |
| `bg_w.stage` | 13 (1..16) | pin verified — but redundant today (above) |
| `New_Challenger` | 2 | verified |
| `Round_Level` | **1** (always 3) | inert on this corpus |
| `t_pl_lvr` (33 compared fields) | **1** (always 0) | inert on this corpus; `waza_no`, the 34th, is the one that carries |

The freeze pair added in §FP splits the same way. `EXE_flag` is **0 at the seed
frame on 183 of 183 and 143 of 143** — constant and equal, kept for the price of
one comparison. `Game_pause` is the opposite and is the reason it is
allowlisted rather than strict: our side is **1** on every segment of both
corpora, the archive is 1 on 50 of 183 (38 of 143) and 0 on the rest, and both
sides are 0 by the first compared frame.

Also constant-and-equal on both sides across all 143, hence carrying no signal
today: `Counter_hi` (99), `Counter_low` (53), `round_timer` (99),
`bg_w.quake_y_index` (0), `cmb_stock`, `cmb_all_stock`, `piyori_type.now`,
`super_arts.gauge`/`.store`, `wu.hit_stop`, `wu.dm_stop`, `wu.cg_add_xy`, and
the five `PLW_*` flags, plus `Max_vitality` (160),
`No_Death` (0), `test_flag` (0), `ixbfw_cut` (0), `save_w.Difficulty` (2) and
`save_w.Damage_Level` (1) — see "The seven, resolved". They are kept because their cost is one comparison and
their absence is what the last seven defects were made of.

### Open

- The oracle still does not **compare** `t_pl_lvr[].waza_no` frame by frame; the
  audit only checks it at the seed frame. Adding it to `compare_lvr()` would
  change verdicts and was left out of this change deliberately.
- **The wholesale `waza_work[]` allowlist was a false-negative class, and it
  fired twice.** Closed 2026-09-06 — see H5. The mirror question this section
  posed for `waza_no` ("the audit checks it strictly, the oracle never compares
  it") turned out to have a twin running the other way: for `waza_work` the
  oracle compared what the audit had agreed not to check. Both halves of that
  pair are worth checking for any group added here in future.
- ~~A Twelve segment whose carried `waza_work[][48..49]` differs can now only
  report rc=0 or rc=4, never rc=1. Seeding it is blocked on `WAZA_WORK::w_ptr`
  being a CPS3 address (H5).~~ **CLOSED 2026-09-06 by H5b** — the blocking
  pointer is `&tbl[16]`, which our own command table supplies, so the state is
  seeded and the audit is now a self-test of that seed. A residue the idle
  cycle cannot produce is still left unseeded and still exits 4.
- `Round_Level` and `bg_w.stage` have no ground truth that exercises them. The
  fix is more corpus (a 2P break-in recording, and a session whose stage does
  not follow from the two characters), not more analysis.

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

So of what is still standing, only **E2a** is on-device detectable — E2b and
E3 are retracted (H4b), and E1 lives entirely outside the window.

Checkpoint interval is 60, verified three ways (`DEFAULT_CHECKSUM_INTERVAL`,
`kTrackChecksumInterval`, and all 507 device `.3sr` headers parsing as
`('3SR1', 1, 60, …)`). `check_checkpoint()` additionally skips any checkpoint
that is not a battle frame (`G_No[1]==2 && G_No[2]==1`) — logged, never failed —
and stops advancing once `game_ended()`, so a post-KO tail is never checked.

`probe_random_ix16()` (`replay_player.c`) is the only recovery, and it sweeps
field index 5 alone. It does not mask other fields: sweeping all 65,536 values
yields only 8,671 distinct hashes, so a hash differing for any other reason is
accepted with probability ≈2.0×10⁻⁶.

**It no longer repairs a v2 file (2026-09-05).** The repair existed to absorb
E2a, and E2a is a property of the FILE now: a v2 `.3sr` carries `players_timer`
and reproduces the recording's spawn phase, so on a v2 file the repair is inert
— and an inert repair is a trap, because a divergence that arrives later is
silently repaired and the checkpoint logged `ok`. That is how E2a itself hid for
months. `check_checkpoint()` therefore repairs only when
`!replay.has_players_timer`; on a v2 file the sweep runs purely as DIAGNOSIS and
the checkpoint fails like any other, with the desync line naming `Random_ix16`
as the sole divergent field (`ReplayPlayer_DesyncWasIx16Only`, also carried into
`rs_record_outcome`'s `diverged` line in `replay_shuffle.c`, whose old blanket
"not Random_ix16 drift" claim was true only while the repair was unconditional).

**Why v1 keeps the repair, measured rather than assumed.** Two `.3sr` files were
built from one corpus archive differing ONLY in the v2 header bytes
(`1787978900734-9309 game_0`, 10,760 frames, 179 battle checkpoints) and played
through the host viewer with this change in place:

| twin | outcome | `r16_resyncs` | first repair |
|---|---|---|---|
| v2 | `REPLAY COMPLETE … checksums=179/179 … reason=game-ended` | **0** | — |
| v1 | `REPLAY COMPLETE … checksums=179/179 … reason=game-ended` | **39** | checkpoint **18/189**, frame **1020** |

Without the gate that v1 file stops at frame 1020, 9.5% of the way in. The
shipped library is overwhelmingly v1 — ~22,682 on the VPS and ~620 on the device
— so failing v1 files would retire the library overnight in order to report a
divergence that is already root-caused, fixed and re-measured. Removing the
repair from v2 costs nothing and buys honesty; removing it from v1 costs the
library and buys a restatement of E2a.

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

## The failure this body of work nearly shipped

Worth stating on its own, because it is the one thing a careful reviewer caught
that six investigations did not.

**H1 and H4b were taught to the oracle and not to the producers.** Once
`ScrdGame_Init` learned to reject CPU-recorded and matchless segments, the sweep
reported zero divergences — and that reads as "the engine is clean". But both
`.3sr` writers kept converting exactly those segments, and the viewer forces two
operators the same way the harness did (`replay_player.c`,
`PHASE_CHARACTER_SELECT` -> `tap_button(SWK_START, 1)`), so every such file is a
**guaranteed on-device desync**. Demonstrated, not argued: a `.3sr` generated
from `7733 game_5` (CPU) and from `7733 game_2` (matchless) each produced
`REPLAY DESYNC at frame 60` in the shipped viewer.

The Mac lane hid this by accident — `publish_3sr.py` gated on `rc == 0`, so
ineligible segments were dropped as a side effect of the statcheck run. The VPS
lane has no statcheck at all (D3) and did ~97% of the 22,747 shipped
conversions.

**Fixed** in `3f04f0bb` (both producers gated), `0b8e05f6` (ineligible reported
as skipped, not divergent) and `abf81daa` (`RS_EMPTY` given an exit). Both lanes
now emit byte-identical output and agree file-for-file.

**The general lesson: a rejection added to the oracle is not a fix.** The oracle
only decides what we *measure*. Anything that also decides what we *ship* has to
learn the same rule, or the measurement gets cleaner while the product gets
worse. Check the producers whenever a harness predicate changes.

### Shipped-corpus exposure (bounded, not measured)

Roughly **1.5%-7.4% of 22,747 files, ~330-1,680**, point estimate ~1,280 (5.6%).

- **Matchless: measured exactly, 66 files (0.290%)** — identifiable from the
  `.3sr` alone because such a segment holds every checksummed field constant but
  `Game_timer` and the SW words, and djb2 over an odd field count is invertible
  enough to recover the difference. Validated 16/16 against ground truth. A lower
  bound, and proof that matchless files did ship.
- **CPU: not identifiable from a `.3sr` at all.** The obvious classifier — "the
  CPU side's input word looks idle" — is refuted: the stored word is a real
  human's, from the other seat, that `Player_move()` simply discarded. Best
  `.3sr`-visible feature reached AUC 0.725. The estimate instead comes from the
  Mac worker's own statcheck outcomes over an independent sample of the same
  catalog (69 FAIL / 1,117 games / 367 quarks = 6.18%, bootstrap 95% CI
  [4.65%, 7.79%]), which is an upper bound since statcheck-FAIL also catches
  real divergence.
- **Do not extrapolate the four local quarks** (62.5% bad). They are
  investigation samples, ~43x enriched against the corpus-wide matchless rate.

**Not cleaned up.** The 66 matchless files are identifiable and removable today;
the CPU files cannot be identified without re-converting. Both are the user's
call. The deployed VPS runner still carries the old tracker, so the VPS lane
keeps producing ungated output until it is redeployed.

## The seeding gap, sized (2026-09-05)

Six defects shared one shape — the arcade carries state our harness resets — so
the obvious question was how much is left. Answer, from `GS_SAVE` in
`src/netplay/game_state.c` (607 globals) against `arcade_constants.h`:

| category | count |
|---|---|
| SEEDED (harness already imports) | 11 |
| RESET (provably zeroed on our match-start path) | 384 |
| **CARRIED (survives our reset)** | **202** |
| UNKNOWN (pointer-aliased writes a name scan cannot see) | 10 |

**But only 7 are worth disassembling.** Of the 202 carried, 92 are read nowhere
in the simulation, 26 only in `effect/`, and **44 only in `com/` — inert, because
`ScrdGame_Init` now rejects CPU segments (H4b), so `cpu_algorithm()` never
runs.** H4b turned a whole risk class into a non-issue. Hand-verifying the
remaining 40 leaves seven with the shape of the known defects: boot- or
service-derived, single writer, never rewritten, read by the fighter sim.

**The seven:** `Max_vitality`, `No_Death`, `test_flag`, `ixbfw_cut`, `Country`,
`CC_Value`, `Limit_Time` — and `Country` is the root that feeds `CC_Value` and
`Limit_Time`, so one address resolves three.
**All seven are now resolved — see "The seven, resolved" below.**

Strongest is `Max_vitality`: `init3rd.c:Init_Task_1st` (`= 160`) is the only
writer, and it sets both starting HP and the damage divisor
`dmcal_d = (original_vitality << 5) / Max_vitality` (`pls02.c:setup_vitality`).
`pls02.c` carries a live `if (Max_vitality == 192)` branch — the engine expects a
second value on a cabinet.

A further 13 (`Winner_id`, `Loser_id`, `Conclusion_Type`, `EM_id`, …) are
genuinely carried but every read sits inside round-settle code that writes them
first in the same match. Take those only if a divergence is traced to a round
transition.

`save_w[].Damage_Level` is read unconditionally by `setup_vitality` and is NOT in
the whitelist, so it fell outside that count — but it is the same class of
cabinet service setting as `Round_Level` and belongs on the same trip.

**Caveat:** the scanner matches writes by name, so pointer-aliased writes
(`lvr->x = …`) are invisible — that is why `t_pl_lvr` shows zero writers. Read
202 as "roughly 200", not exact.

### The seven, resolved (2026-09-05)

All seven addresses were established by disassembly of the sfiii3nr1 SH-2
program and are recorded, with their evidence chains, in
`src/arcade/arcade_constants.h` — `TEST_FLAG_OFFSET`, `COUNTRY_OFFSET`,
`NO_DEATH_OFFSET`, `CC_VALUE_OFFSET`, `LIMIT_TIME_OFFSET`,
`MAX_VITALITY_OFFSET`, `IXBFW_CUT_OFFSET`. All seven then measured over the
whole 143-segment corpus, every frame of every archive:

| global | CPS3 address | arcade, 143/143 | ours | verdict |
|---|---|---|---|---|
| `Max_vitality` | `0x02016B30` | **160** | 160 | identical |
| `No_Death` | `0x02015761` | **0** | 0 | identical |
| `test_flag` | `0x02000094` | **0** | 0 | identical |
| `ixbfw_cut` | `0x02025638` | **0** | 0 | identical |
| `Country` | `0x0201556F` | **1** | **4** | **differs** |
| `CC_Value[0..1]` | `0x0201584D` | **{0, 0}** | **{1, 2}** | **differs** (via `Country`) |
| `Limit_Time` | `0x02016AD4` | **1241** | **1061** | **differs** (via `Country`) |

Not one of the seven varies *within* an archive either — measured across every
frame of all 143, `varying_within_archive = 0` for each. They are boot
constants on both sides, which is what makes a single seed sufficient and an
assert meaningful.

**Four are a negative result, and that is worth having.** `Max_vitality`,
`No_Death`, `test_flag` and `ixbfw_cut` are identical on both sides of every
segment, so none of them can be the cause of anything, and four of the forty
hand-verified carried globals are now closed rather than merely unexamined.
Note the limit of that: `test_flag` and `ixbfw_cut` are **0 on both sides
everywhere**, and a field that is zero on both sides cannot distinguish a
correct offset from a wrong-but-zero one. Their addresses rest on the
disassembly (four paired `test_flag == 0 || ixbfw_cut == 0` sites, anchored by
the already-established `WORK_CG_IX_OFFSET 0x204`), not on the corpus.

**`Max_vitality` came with a bonus.** The `if (Max_vitality == 192)` branch in
`cal_dm_vital_gauge_hosei` (`pls02.c`) really does have an arcade writer: one
routine at CPS3 `0x060053DC` branches on the service byte at `0x0206AC62` and
writes **192** on one arm (`0x06005450`) and **160** on the other
(`0x06005496`). The corpus reads that selector byte as 0 on all 143, i.e. the
archives take the 160 arm — the arm our `Init_Task_1st` hardcodes. So the
branch is live hardware behaviour, and this ground truth never exercises it.

### `Country` is the one real difference, and it is latent

Our port hardcodes `Country = 4` in `njUserInit` (`main.c`); the ground truth is
a Japanese board and reads **1**. The arcade splits what our
`Setup_Difficult_V()` inlines — CPS3 `0x06004EB2` stores
`tbl_0x0613D83D[Country - 1]` into a country *index* at `0x0201584C`, and
`Setup_Difficult_V` (CPS3 `0x06005368`) then copies two bytes out of
`Difficult_V_Data` = `0x0613D845` — but the two agree wherever our port has an
arm at all. The index byte reads 0 on all 143 segments, which is reachable only
from `Country == 1`: a second, independent address confirming the first.

What the difference gates, in descending order of how much it would matter:

- **`effb8_normal_or_senyou()` (`effb8.c`) is `if (Country != 1) return 0;
  return random_16() & 1;`** — an RNG draw the arcade takes and, before this
  change, we did not. Since the `Random_ix16` mask came off (M1) the oracle
  asserts that field honestly and the corpus is 143 PASS, so **this function is
  provably never called inside a compared frame on this corpus**. That is a
  statement about the corpus, not a reason the difference is safe.
- `efff9.c`'s `Country != 1 && Country != 8` clamp on `old_rno[5]`: draws
  nothing, and `old_rno` on an effect work is not compared.
- `Limit_Time` 1061 vs 1241 is the ceiling `Time_Control()` (`game.c`) clamps
  `Control_Time` to. `Control_Time` = `0x02011372` measures 481 at the start of
  all 143 archives — our `game.c`'s literal — and climbs by 1 per 60 frames, and
  in the longest archives it reaches **1093**. So the arcade genuinely runs
  `Control_Time` past our 1061 clamp. It does not surface in statcheck, because
  a statcheck run starts a synthetic match at 481 and never gets near either
  ceiling, and `Control_Time` only feeds CPU difficulty selection (`com/`),
  inert under H4b.
- `CC_Value[0]` is read only in `com/`; `CC_Value[1]` only on
  `setup_vitality`'s `wk->operator == 0` arm. Both inert under H4b.
- `old_my_char_check()` (`effect.c`) and `game.c`'s `Country == 3` are outside
  the battle path entirely (grade screen, `Rep_Game_Infor`).

### Seed vs assert, decided per global

**`Country` is SEEDED — one address resolves three.** `Statcheck_SyncValues`
now reads `Country` from the archive and re-runs the same two derivations
`Init_Task_1st` runs, `Setup_Difficult_V()` and `Setup_Limit_Time()`.
`CC_Value` and `Limit_Time` are deliberately **not** imported: they are
recomputed, and then asserted against the archive by the seed audit, which
makes the import a self-test of the derivation rather than a restatement of it.
Measured, that assert lands: `Country` 1 = 1, `CC_Value` {0,0} = {0,0},
`Limit_Time` 1241 = 1241 on every segment. The last of those is the interesting
one — our `Setup_Limit_Time()` folds the arcade's max-over-a-difficulty-table
(CPS3 `0x06012570`) into the literal `Country == 1 ? 1241 : 1061`, and the fold
is exactly right for this cabinet's settings.

This is the opposite call from `bg_w.quake_y_index` (E5), and for the opposite
reason. There, both sides entered every segment at 0, so a seed would have been
a per-frame mask over a real divergence. Here the two sides enter at *different*
constants, nothing in a replay can re-derive a region byte, and the seed is the
only thing that can close the gap.

**The seed is not gated on `ArcadeBalance_IsEnabled()`, and does not need to
be.** `statcheck_compare.c` is entirely `#if defined(STATCHECK)` and
`Statcheck_SyncValues` has exactly one caller, `StatcheckRunner_Prologue`. The
shipped engine never runs it, so no shipped behaviour changes. **Whether the
shipped build should stop hardcoding `Country = 4` is a separate question and
was deliberately left alone** — it is a *region* difference, not an
arcade-vs-PS2 balance difference. A US CPS3 cabinet would also read `Country !=
1` and also skip that `random_16()` draw, so "the arcade draws here" is only
true of the board this corpus came from.

**The other four are ASSERTED, strictly.** `Max_vitality`, `No_Death`,
`test_flag` and `ixbfw_cut` already agree; there is nothing to seed, and a
mismatch would be a real defect. Kept for the same reason the other
constant-and-equal fields are: one comparison each, and their absence is what
the last seven defects were made of.

**`save_w[Present_Mode].Difficulty` and `.Damage_Level` were on the same trip**
(`0x0206AC63` / `0x0206AC64`, read off `setup_vitality` CPS3 `0x0611E202`) and
are asserted too, not seeded: the archives read 2 and 1, which is
`Game_Default_Data` (`sys_sub.c`) verbatim, so both sides already agree. Note
the arcade `_SAVE_W` is **not** this port's — ours has `_PAD_INFOR
Pad_Infor[2]` ahead of `Difficulty` and `Time_Limit`/`Battle_Number[2]` between
the two fields, where the arcade has them at +1 and +2 — so these are absolute
addresses, never a base plus `offsetof`. That is the same trap
`bg_w.quake_y_index` set.

### Result

Ten comparisons added to `StatcheckSeedAudit_Run` (the `=2` dump goes from 158
named fields to **168**). Corpus sweep, two binaries built from the same source
tree differing only by this change, `--headless`, 143 segments:

| | before | after |
|---|---|---|
| rc 0 | 143 | **143** |
| rc 1/2/3/4 | 0 | **0** |
| `PASS — compared archive frames a..b of n` lines that changed | — | **0 of 143** |
| seed verdict | CLEAN 143/143 | **CLEAN 143/143** |

So seeding `Country` changes no verdict and no compared frame range on this
corpus — which is the expected result given that the one behavioural path it
opens (`effb8_normal_or_senyou`) is never reached in a compared frame here, and
is also the measurement that establishes that.

**Detection control, because a clean verdict on its own proves nothing.** A
third binary, identical except that the three seed lines are removed (the audit
left in), sweeps **143 of 143 DIRTY**, naming exactly the four fields with
exactly the predicted values:

    statcheck-seed: MISMATCH Country [seeded]        ours=4 cps3=1
    statcheck-seed: MISMATCH CC_Value[0] [derived]   ours=1 cps3=0
    statcheck-seed: MISMATCH CC_Value[1] [derived]   ours=2 cps3=0
    statcheck-seed: MISMATCH Limit_Time [derived]    ours=1061 cps3=1241

and **all 143 of those still exit 0**, because none of the four is compared
frame by frame. That is the `t_pl_lvr.waza_no` case again: the audit catching an
imported-state gap the oracle is completely blind to, which is the case it was
built for.

### Still carried, still unexamined

The 13 round-settle globals (`Winner_id`, `Loser_id`, `Conclusion_Type`,
`EM_id`, …) are unchanged: every read sits inside code that writes them first
in the same match, so they are only worth taking if a divergence is traced to a
round transition.

One new item came out of the measurement and is **not** closed: **`Break_Into` — CLOSED (2026-09-05).** Scanned across every frame of all 143 archives: 106 segments hold 1, but every run sits **entirely after the last compared frame** (min gap 264 frames, max 639; 0 of 106 inside the window). It is the next match's break-in, and `game.c` clears it before the compared window opens on both sides. Not a defect. Superseded text follows:

**`Break_Into`**
(CPS3 `0x02011386`, established at the same three `plcnt_*_move` sites as
`No_Death` — it is the very next guarded block in each). It is read exactly like
`No_Death` (`if (Break_Into) { plw[0].wu.dm_vital = plw[1].wu.dm_vital = 0; }`)
and it is **not** constant: it holds 1 at some point in **106 of the 143**
archives, and varies within those archives. Our port writes it too
(`entry.c` sets it to 1 at six sites, `game.c` and `entry.c` clear it), so this
may be reproduced rather than carried — but nothing has checked, and unlike the
seven it is not a boot constant. Sizing it needs the same treatment: when is it
1 relative to the compared window, and does our synthetic session set it on the
same frames.

### HARNESS FACT, and a trap: `Mode_Type` is `MODE_ARCADE`

`Menu_Init()` sets `Menu_Cursor_Y[0] = 0` and the harness only ever emits
`SWK_START`/`SWK_SOUTH`, never DOWN, so `Mode_Select()` takes `case 0` and the
statcheck harness runs **`MODE_ARCADE`**. Comments in `src/test/scrd_game.c` (the
H4b block) and `statcheck_runner.c:pin_default_button_mapping` claim
`MODE_VERSUS`. They are wrong.

**Do not "fix" the harness to actually select VERSUS.** `Statcheck_SyncValues`
and `Game2_0()` run on the same frame, and `Game2_0`'s MODE_VERSUS arm calls
`All_Clear_Random_ix()` / `All_Clear_Timer()` — which would wipe the
`Random_ix16`, `Random_ix32` and `players_timer` seeds before the first
comparison, destroying the oracle. Correct the comments, not the behaviour.

## SETTLED: every CPS3-derived engine change is gated (user, 2026-09-05)

**The governing rule, from the user, and it is not a per-fix judgement call.**
This project is a decompilation of the PS2 build. Arcade balance is an
*addition* layered on top, driven by decompiling CPS3. Therefore **any
CPS3-derived behavioural change belongs behind `ArcadeBalance_IsEnabled()`**
unless there is positive proof it corrects our own transcription error rather
than a genuine PS2/CPS3 difference. Every fix in this document was verified
against the CPS3 disassembly and **none against the PS2 binary**, which is
exactly the evidence that would supply such proof. So they are all gated.

E1a was gated when it landed. **E4 and E5 shipped ungated and are now gated
too** — they had been changing the simulation in every mode, so the port's
"PS2" engine no longer matched the original PS2 engine:

| fix | site | PS2 path | arcade path |
|---|---|---|---|
| **E4** | `cal_move_dir_forecast()` (`engine/caldir.c`) | `(d.sp * (tm * tm)) / 2` — the halving binds to the product, as decompiled | `d.sp * ((tm * tm) / 2)` — the halving binds to `tm * tm`, as CPS3 `0x06090E40`-`0x06090E58` |
| **E5a** | `effect_A7_move` (`effect/effa7.c`), `effect_02_move` (`effect/eff02.c`), the `tad->hits == 0` early-out | `bg_w.quake_y_index = gqdt_active()[tad->quake][1];` then `pp_screen_quake(bg_w.quake_y_index)` — the write restored | `pp_screen_quake(...)` only; no state write, as CPS3 `0x060F91EC` / `0x060DC918` |
| **E5b** | `gqdt` (`effect/eff02.c`) | `gqdt` rows 7/8 `{6, 0}` / `{6, 0}`, as decompiled | `gqdt_arcade` rows 7/8 `{6, 4}` / `{6, 2}`, as CPS3 `0x061B941A` |

**How E5b is gated.** `gqdt` is a `const s16[19][2]` read at five sites across
two files, so a branch at each read would have been five places to get wrong.
Instead there are two tables and one selector: `gqdt` keeps the PS2 values
unchanged, `gqdt_arcade` (static, adjacent to it so the two rows that differ
are diffable by eye) carries the CPS3 transcription, and
`gqdt_active()` — declared in `effect/eff02.h` — returns whichever the session
resolved. Every read goes through the selector. This follows
`src/arcade/arcade_char_data.c`, which is the tree's precedent for
arcade-vs-PS2 data selection: PS2 data stays where it is and the arcade variant
sits beside it, chosen at the read.

**Which balance each gate exercises, measured rather than assumed.** A
statcheck run prints `statcheck: pinned hermetic config -- ... balance=auto (was
auto)` and then, on a machine with the romset, `Arcade balance auto-selected:
CPS3 ROM verified, 20/20 characters adapted`. So the 143-segment sweep runs the
**arcade** path — it is the gated-ON branch it verifies, and it says so in its
own log. The PS2 path is covered by the frame-data suite instead: exactly 5 of
its 99 non-smoke corpora carry an explicit `balance: arcade` key and the other
94 carry no `balance:` key at all, so they resolve to `DEFAULT_BALANCE = "ps2"`
(`compile_corpus.py` -> `resolve_balance`); `run.sh` passes the resolved value
through as `--test-balance`.

**What remains open.** The gating makes both modes correct *by the rule*; it
does not answer which of the three is a decompilation error. Each would be
*un*gated only on positive evidence from the PS2 binary at the corresponding
site — read the PS2 build the way CPS3 was read here. Until someone does that,
they stay gated.

Note this is not academic: `caldir.c` feeds `dir_sel_table` -> `dir32_skydm` ->
`dm_reaction_table`, i.e. knockdown and juggle behaviour a player can feel — so
PS2 mode and arcade mode now genuinely differ there, deliberately.

## CORRECTION: pre-fix `.3sr` files do NOT need re-conversion (2026-09-05)

Earlier revisions of §E4 and §E5 — and commit `edf7b8c3`'s message — said a
`.3sr` recorded by the pre-fix engine encodes the wrong `Random_ix16` stream and
would mismatch on a post-fix build, implying a re-conversion campaign over
~22,682 v1 files. **That is backwards.**

`compute_checksum(frame)` (`tools/fcade-replays/make_3sr.py`) unpacks its 13
fields big-endian **from the CPS3 RAM archive frame**, via `CHECKSUM_FIELDS` read
in `extract_scrd_game`. It never hashes engine output. So a `.3sr`'s checkpoints
are arcade ground truth regardless of which engine converted the file, and a
post-fix engine matches them **better**, not worse.

Existing files are correct and need nothing. The genuinely open item is the
opposite one: segments the eligibility gate *rejected* before `3f04f0bb` were
never converted at all, which is a deploy question, not a re-conversion one.

## Worklist

| id | what | state |
|----|------|-------|
| E1a | `Play_Type == 1` damage pin has no arcade counterpart | **FIXED** — gated on `ArcadeBalance_IsEnabled()` at both `pow_pow.c` sites, `setup_vs_mode()` seeded to **3** (not 0), `ROUND_LEVEL_OFFSET 0x1137A` imported. Proven by disassembly (`0x0609E36C`/`0x0609E3FA` index `Round_Level` unconditionally); demonstrates on **no** corpus segment — reachable only via 2P break-in, which is not traced |
| E1b | port never updates `Round_Level` in VS | **RETRACTED, not a defect** — arcade `Loser_Sub` (`0x0609C616`) and `Update_VS_Data` (`0x0609C79A`) gate on `Play_Type` exactly as the port does. The archive decrements are all in human-vs-CPU segments |
| H4b | harness forces `Play_Type == 1` on every segment | **FIXED, by rejection** — `ScrdGame_Init` reads `wu_operator` at the match-start frame (`WORK_WU_OPERATOR_OFFSET`, archive `0x68C6F`/`0x69107`) and returns `SCRD_GAME_INIT_CPU_PLAYER`; `main.c` exits **3**, distinct from 1 and 2. Reproducing the CPU player was tried and refuted by measurement (see H4b) — it breaks input pinning at frame 7 and manufactures a new `routine_no` divergence at frame 11. Costs 8 of 16 segments; sweep now reports **0** divergences |
| E2a | `effect_G9_init()` spawn phase / `players_timer` | **oracle FIXED** `f63507b7` (drift 322/174/255/418 -> 0); **viewer FIXED** via `.3sr` v2 (host A/B: 31 -> 0 and 13 -> 0 `r16_resyncs`); NOT yet tested on the device. Both masks that hid it are now gone: the viewer repairs `Random_ix16` only on v1 files (D1) and the oracle asserts it (see "The instrumentation"). Existing v1 files keep the old behaviour, deliberately |
| E2b | ~~a `random_32` consumer the port never runs~~ | **RETRACTED** — it is the CPU player's `Com_Initialize()`; same cause as H4b, not an engine defect. All six instances now exit 3 |
| E4 | Dudley `routine_no[2]` 23 vs 20 @3090 | **FIXED and GATED, one misassociated `/ 2`** (arcade only; PS2 keeps the decompiled association) — `cal_move_dir_forecast()` (`engine/caldir.c`) wrote `(d.sp * (tm * tm)) / 2` where the arcade computes `d.sp * ((tm * tm) / 2)`; every caller passes `tm == 5`, so `tm * tm` is odd and the two differ by half a unit of acceleration. CPS3 `0x06090E40`-`0x06090E58` halves the square with `cmp/gt`/`addc`/`shar` **before** either `mul.l`. Ruled out first, by measurement: every input byte-identical to the archive, `dir_sel_table` all 16,384 bytes identical to `0x0618F664`, `dir32_skydm`/`dir32_grddm` byte-exact, `caldir_pos_256`/`_032` faithful. Corpus **142/1 -> 143/0**, and the E4 segment is the only verdict line that changed. Character-agnostic (11 bucket flips in 764 calls span 4 victims and 5 attackers); only 1 of the 11 lands on a table edge, which is why 142 segments passed with it in place. **Device-visible**: pre-fix, `plw[1].wu.xyz[0].disp.pos` — checkpoint field 9 — diverges 14 frames later, at archive frame 3104 |
| E5 | stage quake debris draws `random_16()` where CPS3 does not | **FIXED and GATED, two port defects in the quake writers** (arcade only; PS2 keeps the `bg_w.quake_y_index` write and `gqdt` rows 7/8 at `{6,0}`, via `gqdt_active()`) — (1) `effect_A7_move`/`effect_02_move`'s `tad->hits == 0` early-out wrote `bg_w.quake_y_index` where the arcade's branch only does the SE and tail-calls `push_effect_work` (CPS3 `0x060F91EC`/`0x060DC918`); the write is now `pp_screen_quake(gqdt[tad->quake][1])`, keeping the PS2 rumble and dropping the state. (2) `gqdt` rows 7 and 8 were `{6,0}`, arcade `0x061B941A` has `{6,4}`/`{6,2}`. Corpus 135/8 -> **142/1** and zero residual `bg_w.quake_y_index` divergence, down from 48 of 143 segments. **NOT** an unimported-state defect — both sides enter every segment at 0, so `BG_W_QUAKE_Y_INDEX_OFFSET 0x26BD8` is asserted, never seeded |
| E6 | `effect_C08_move` routine 2 ran ungated — the 2026-09-06 corpus's D2 | **FIXED and GATED** — two changes, both arcade-side. (1) `effect/effc08.c` `case 2` is now `if (!EXE_flag && !Game_pause)`, matching `case 1` and CPS3 `0x060DDA84`, whose gate is the byte-for-byte twin of routine 1's at `0x060DD918`; the function carries **two** `0x0201136E` pool slots (`0x060DD98C`, `0x060DDBD8`), one per routine. Ungated, the port ticked the `4 x v` pause through hit-stop and pause frames on which the arcade freezes, re-entered routine 1 early and drew one cycle sooner — `delta=+1`, ours ahead, on **26 of 26 stage-3 segments**, 10 quarks, frames 1,325-6,473, with a within-session control (`1788133423462-3110`: same two players and characters throughout, 4/4 stage-3 fail, 4/4 stage-13 pass). (2) `bg030.c`/`bg190.c` now spawn C08/C74 behind `ArcadeBalance_IsEnabled()` — `afc16ad2` predated the gating rule and had PS2 mode running two effects with no PS2 counterpart at all (§23.8). `effc74.c` does **NOT** share the defect: CPS3 `0x060F1390` dispatches only routines 0 and 1, one `0x0201136E` reference, and all **13** stage-19 corpus segments pass before and after. Corpus 151/32 -> **177 PASS / 6 FAIL** (the 6 are D3-D6, no frame or assert moved); old corpus **143/143 unchanged**; frame-data suite **99 GREEN, zero drift**. §23.10's acceptance was 60 frames and could not have seen this |
| E7 | `Win_01000()` clamps the winner where the arcade does not — the 2026-09-06 corpus's D4 | **FIXED and GATED** — `animation/win_pl.c` -> `Win_01000()` called a `set_field_hosei_flag` pair between `bg_app_stop = 1` and `switch (routine_no[3])`; the arcade routine has **nothing** between them (`060c2ea2 mov.b r3,@r2` -> `060c2ea6 mov.w @(r0,r14),r0`, r0 = 42 = `routine_no[3]`, then `cmp/eq #0/#1/#9` with 1 and 9 sharing a target as the port's `case 1: case 9:` does). Reached as `win_jp_tbl[winner_type_tbl[player_number]]` — `win_player` (`0x060C2DDC`) copies the 16-entry table at `0x061A38C0` to stack and indexes it by the 21-entry table at `0x061A3890` (Oro -> 1 -> `0x060C2E8C`); both tables unique in the image, `0x061A3890` has exactly one literal referrer. Negative established over the whole 1,318-byte routine with all three `jijii_*` inlined: no aligned word equals `&set_field_hosei_flag` (`0x0611DFB8`) so no `jsr` reaches it, and `bsr` cannot either (`0x5AC06` away vs `±0x1000` reach) — with `random_16` (`0x0611E0EE`, `0x136` distant) found by the same scan as the positive control. The clamp pins Oro at screen centre + 164, so `jijii_jump`'s `xyz[0].disp.pos > bgw[1].xy[0].disp.pos + 320` exit is unreachable and the win leap never ends. Archive agrees frame for frame: `routine_no[3] == 9`, `win_rno == 2/1` throughout, winner X climbing to **674 at f3,290** against a 668 threshold (camera 348), where `win_rno[1]` steps 1 -> 2 — the exit firing — then freezes. Corpus 178/4 -> **181 PASS / 1 `rc=1`**, exactly 3 verdicts moved, all one session; 143-corpus **143/143 unchanged**; frame-data suite **99 GREEN, zero drift**. **The other 47 `win_pl.c` sites and 12 in `lose_pl.c` are UNADJUDICATED** — only Oro's routine was read out of the arcade program, and `Normal_normal_Winner`'s first ten lines are byte-identical to `Win_01000`'s |
| E8 | `effect_L7_init()` gate 3 tests the wrong bit — the 2026-09-06 corpus's D5 | **FIXED and GATED** — the arcade (`0x06113FC8`) gates Hugo's Poison taunt gag on **bit 12** of the raw `P1SW_0`/`P2SW_0` (`mov.w 0x61140c0,r4` -> `r4 = 0x1000`; `0x0206AA8C`/`0x0206AA90` per branch); the port tested **bit 0**, `SWK_UP`, so it never spawned and never drew — `delta=-1`, CPS3 drawing where we did not, the opposite direction from E6. Function identified independently: `effl7_data_tbl` unique at `0x061CB064` with exactly one literal referrer (pool word `0x061141A4`, loaded at `0x0611416C`, in-function); `effmovejptbl[217] = 0x06113D54`, and 217 is the `wu.id` this routine stores. Exactly **one** `random_16` pool word in the function (`0x061141A0` -> `jsr` at `0x06114166`; `bsr` cannot reach, `0x9F64` vs `±0x1000`), so one draw per spawn. `SWK_START` is used as a **conversion identity** — the port's own raw-arcade converter `src/test/replay_game.c` -> `read_input_buff()` maps `(raw & (1 << 12)) << 2`, i.e. bit 12 -> `SWK_START`; that bit 12 **is** START is likely but **NOT proven**, and nothing rests on it. Two harness changes were needed because the `sw_lvbt` mirror carries no start bit at all: `statcheck_runner.c` -> `read_input_buff()` now imports it, and `pause.c` -> `Check_Pause_Term()`'s `STATCHECK` carve-out moved **above** the `SWK_START` check — the same correction the `.3sr` replay player already had, whose comment stated the now-false precondition "`read_input_buff` never emits `SWK_START`"; leaving it below cost 3 passing segments (`t_pl_lvr` `left_cnt` 35 vs 36, `right_cnt` 53 vs 54, and one `waza` `w_type`), measured not predicted. PS2 keeps `& 1` (`SWK_UP` is `1 << 0`, bit-identical). Corpus 181/1 -> **182 PASS / 0 `rc=1`**, exactly 1 verdict moved; 143-corpus **143/143 unchanged**; frame-data suite **99 GREEN, zero drift**. `win_pl.c` -> `Win_13000()` carries the **identical `& 1` gate** and is deliberately UNADJUDICATED — its arcade counterpart was never read |
| FP | `EXE_flag` / `Game_pause` were never compared against CPS3 — a detection blind spot, not a defect | **CLOSED, 2026-09-06** — both offsets added to `arcade_constants.h` (`EXE_FLAG_OFFSET 0xEECC` = CPS3 `0x0200EECC`, `GAME_PAUSE_OFFSET 0x1136E` = CPS3 `0x0201136E`; established by E6's disassembly at three sites — `effect_C08_move` routines 1 and 2 at `0x060DD918`/`0x060DDA84` and `effect_C74_move` at `0x060F13D4` — and corroborated in the archives, not taken from the comment). **ASSERTED every compared frame, never seeded**, the `bg_w.quake_y_index` call and the opposite of `players_timer`: `Game2_0()` zeroes `Game_pause` and `set_EXE_flag()` recomputes `EXE_flag` from a `Game_timer` that `Game2_0()` just zeroed, so there is nothing to import and a mismatch is behaviour. **One normalization, forced by measurement**: the arcade holds **-1** through the 90-frame K.O. window where our `effect_84_move` holds **1** (`Time_Data[1]`), and the archive shows `EXE_flag` freezing across a -1 run exactly as across a 1 run — so `compare_service_values()` maps that one value and stays strict on every other. **`0x81` is not masked**: every writer was traced unreachable under STATCHECK (`Check_Pause_Term()` returns 0 unconditionally above both the `SWK_START` and connection tests since `3769c189`; `Check_SoftReset` needs `SWK_BACK`, which `read_input_buff` never emits; the five `menu.c` writers are training/replay paths; `cpLoopTask`'s `|= 0x80` is DEBUG-only) and the compare merely *labels* the case — it never fired on any of the 328 corpus runs. Seed audit: `EXE_flag` **strict** (0 on 183/183 and 143/143), `Game_pause` **allowlisted** on `Game2_0()`'s next-frame zero (archive carries 1 into the seed frame on 50/183 and 38/143, ours is 1 on all, both 0 at `start_index` on 183/183 and 143/143). **Zero new failures**: 2026-09-06 **182 PASS / 0 `rc=1` / 1 `rc=4` / 2 `rc=3` unchanged**, 2026-09-05 **143/143 unchanged**, **0** `PASS — compared archive frames a..b of n` lines changed on either corpus, frame-data suite **99 GREEN, zero drift**. Archive census over every frame of both corpora (1,167,121 + 869,986): `EXE_flag` ∈ {0,1,2,3}, `Game_pause` ∈ {0,1,-1} and **nothing else** — in particular no `0x81`. **Positive control**: remove the `-1 -> 1` substitution and the corpus goes **182/182 and 143/143 FAIL**, at archive frames 941-5,755 and 921-4,255, every one of them the identical line `game_pause_3sx (1) != game_pause_cps3_norm (-1)` — so the assert is live, the compared window reaches a K.O. freeze on EVERY segment, and the mask hides exactly one value pair with nothing behind it. `EXE_obroll`, the third flag of the family, is still unassertable — no CPS3 address, and our port has ONE writer (`EXE_obroll = 0`, `manage.c`) against 55 readers, so it is structurally dead on our side |
| M1 | the oracle force-synced `Random_ix16` every frame | **REMOVED** — `compare_service_values()` now asserts it. Corpus 142/1 -> 135/8; the 7 new failures were E5, and fixing E5 took it back to 142/1 with the assert standing. Every `Random_ix16` verdict in this document dated before 2026-09-05 was made under the mask |
| M3 | the DEBUG comparer force-syncs `Random_ix16` too | **NO ACTION, and stated so** — `test_runner_compare.c` -> `compare_service_values` carries the identical line, but `compare_values`/`sync_values` have no caller anywhere in `src/` (`test_runner.c` includes the header and calls neither). It masks nothing because nothing runs it |
| M2 | the viewer repaired `Random_ix16` at every checkpoint | **GATED to v1** — `check_checkpoint()` repairs only when `!has_players_timer`; a v2 file fails an ix16-only mismatch and names the field (D1). v1 kept because an A/B twin desyncs at checkpoint 18/189 without it, against ~23,300 shipped v1 files |
| E3 | `pos.x` +32 at round start | **RETRACTED, not an engine divergence** — both instances are `(1,0)` CPU segments and now exit 3. The proposed `Appear_24000`/`Appear_25000` mechanism is refuted by measurement (`routine_no[4]` is 1 and 21 in both, never 24/25). Unreachable in (1,1) play: every `wu_operator`-conditioned round-start-X path needs an operator flag clear. **NOT proven**: which write produced the +32 (candidate: the `set_field_hosei_flag` clamp against an unimported camera, `bg_w.bgw[1].wxy[0]`) |
| H1 | `ScrdGame_Init` post-KO false positive | **FIXED** — require `Game2_0()`'s `Game_timer=0`/`G_No[2]=3`; matchless segments exit 2, not 1 |
| H2 | stage not imported | **FIXED** — `BG_W_STAGE_OFFSET 0x26BB0` from disassembly; pinned via `Debug_w[DEBUG_STAGE_SELECT]` |
| H3 | lever counters never cleared | **FIXED** — seed `t_pl_lvr` in `Statcheck_SyncValues` like `players_timer`; the warm-up was never the defect |
| H5 | seed audit allowlisted `waza_work[]` wholesale; carried entries 48-55 read as engine divergence | **FIXED, 2026-09-06** — the 2026-09-06 corpus's D3 and D6, both `rc=1` at archive frame 7 on a CLEAN seed. `cmd_init()` clears only entries 0..47 under `ArcadeBalance_IsEnabled()` ("CPS3 clears 0x540 bytes of each 0x620-byte command-state block"), so 48..55 carry across the match boundary — arcade residue vs a synthetic session that has never played a match. Two fixes, both in `src/test/`: `compare_waza_work()` now skips entries with `waza_flag[j] == -1`, the test `compare_wcp()` already applied and which `cmd_main.c` gates every `waza_work` access on; and the audit is now strict for `j >= 48 && j < pl_cmd_num[My_char[i]][6]`. `CHAR_TWELVE` is the only character whose live range reaches 48. Judged not seedable at the time — **that half is superseded by H5b**, which shows the blocking `WAZA_WORK::w_ptr` is always `&tbl[16]`. Corpus 177/6 -> **178 pass / 4 rc=1 / 1 rc=4**, exactly 2 verdicts moved; 143-segment corpus **143/143 unchanged**; frame-data suite 99 GREEN. Corrects D3's 29-vs-21 allowlist-count inference — `s_expected` counts `waza_work[]` as **one** |
| H5b | the carried `waza_work[][48..49]` residue was left unseeded, so a Twelve segment could never report rc=1 | **FIXED, 2026-09-06** — the last non-PASS class in the corpora: **5** `rc=4`, all Twelve, all `wu_operator = [1,1]`, all failing `compare_waza_work()` at archive frame 7 naming the **same seven fields with the same seven values**. Constants repeating across five unrelated matches are not residue: every one is a `arcade_cmd_data.c` table constant for Twelve's entries 48 (`unk_cmd_186`) / 49 (`unk_cmd_187`). An idle entry runs a closed cycle — `check_init()` reloads `w_type`/`w_int`/`free1`/`free2`/`w_lvr` from `tbl[12..15]`, sets `w_ptr = &tbl[16]` and dispatches in the same frame; the handler then decrements `w_int` and drops `w_type` to 0 when it goes negative. Entry 48's `tbl[13] == 0` makes it a fixed point at `(0,-1)`; entry 49's `tbl[13] == 1` gives it **period 2**, and the phase is the whole defect. **`waza_work[S][49].w_type` at the seed frame predicts the verdict on 16 of 16 Twelve segments** — the eleven populated ones split into two DIRTY signatures differing in exactly one line. **Seedable after all**: H5 read `w_ptr` as an unmappable CPS3 address, but the four measured values `0x0619BDF8`/`0x0619BE32`/`0x0619BE64`/`0x0619BE96` have gaps `0x3A`/`0x32`/`0x32` = the byte sizes of `unk_cmd_184`/`185`/`186` exactly, so all four sit at the one offset `check_init()` produces — and the value we need is what **our** `check_init()` would write, `&tbl[16]`, not the CPS3 address. `sync_waza_work_carried()` (`statcheck_compare.c`) imports the twelve scalar fields and RECONSTRUCTS `w_ptr`, behind a guard that admits only states the idle cycle can produce; anything else stays unseeded and still exits 4, so H5's protection is intact. Unreachable for 19 characters (`pl_cmd_num[c][6] <= 48`). Corpora **143/143 -> 143/143**, **182+1rc4 -> 183 PASS**, **117+4rc4 -> 121 PASS** — **447 of 447 eligible segments PASS**, 11 verdict lines moved (5 rc=4->0, 6 DIRTY->CLEAN); the five clear over 3,657-7,410 compared frames each. Nothing else moved, on split evidence: PASS ranges captured on both binaries for all 16 Twelve segments (none changed), and the loop is STATICALLY UNREACHABLE for the other 447 (`pl_cmd_num[c][6] <= 48` for all nineteen non-Twelve characters), with their verdict lines measured identical on top. Frame-data suite 99 GREEN, zero drift. Controls: forcing the guard false returns all 16 segments to the pre-fix line exactly; and `w_ptr` is **never dereferenced** from the seeded state on this corpus — replacing it with a pointer of all-`28` `command_ok()` sentinels changes **0 of 16**, so the reconstruction rests on `check_init()`'s arithmetic, not on a passing sweep |
| D1 | `vital_new` outside the hash window | E1 is undetectable on device by design — decide whether to widen |
| D2 | no rescan path | **FIXED** `c6a75572`; verified on device (13 -> 507 entries, desync detected) |
| CAB | **the seven cabinet / service globals** — `Max_vitality`, `No_Death`, `test_flag`, `ixbfw_cut`, `Country`, `CC_Value`, `Limit_Time` | **RESOLVED, 2026-09-05** — all seven addressed by disassembly and recorded with their evidence chains in `arcade_constants.h`, then measured over every frame of all 143 corpus segments. Four are **identical on both sides** (`Max_vitality` 160, `No_Death` 0, `test_flag` 0, `ixbfw_cut` 0) and are **asserted**, not seeded — a negative result that closes four of the forty hand-verified carried globals. `Country` is the one real difference (ours 4, arcade 1) and is **seeded**; `CC_Value` and `Limit_Time` are then **re-derived** by `Setup_Difficult_V()` / `Setup_Limit_Time()` and asserted, so one address resolves three and the import self-tests its own derivation. Latent, not fatal: the difference gates `effb8_normal_or_senyou()`'s `random_16()` draw, which the honest `Random_ix16` assert proves is never reached in a compared frame on this corpus. Sweep **143/143 -> 143/143**, zero PASS lines changed; detection control (seed removed) goes **143/143 DIRTY** naming all four. `save_w.Difficulty` / `.Damage_Level` came on the same trip and also agree |
| SA | **the seed audit** — announce imported-state gaps at the seed frame | **BUILT, 2026-09-05** — `src/test/statcheck_seed_audit.c`, run from `StatcheckRunner_Prologue` right after `Statcheck_SyncValues` and before the engine executes any compared frame; 168 fields, read-only. Corpus **CLEAN 143/143**, sweep unchanged at 143 PASS / 0 FAIL with **byte-identical** compared-frame ranges. A comparison failure with a dirty seed now exits **4**, not 1 — H1/H4b's rule, a fourth typed code, so rc 1 gets stronger. Retrodicts `players_timer` (143/143 named; 126 runs move from rc 1 to rc 4) and `t_pl_lvr` (the H3 segment's exact `s1_cnt` numbers, and 143/143 on the wide corpus via `waza_no` — a carried field the oracle never compares). `Round_Level` and `bg_w.stage` are **inert on both corpora** (`=2` dump: 3 and matching on every segment); their detection paths are proven by a deliberate skew instead |

**For a reviewer:** the 16-segment corpus reports **no engine divergence at
all** — 6 PASS, 8 rejected as unreproducible (H4b), 2 with no match (H1). Every
one of the eleven failures this document opened with had a harness cause. E1a
is applied; E1b, E2b and E3 are retracted; H1, H2, H3 and H4b have landed.
**That sentence was written while the oracle was still masking `Random_ix16`.**
On the 143-segment corpus with the mask removed the count was 135 PASS / 8 FAIL:
E4, plus the seven E5 segments. **E5 is now fixed** (two port defects in the
quake writers) and **E4 is now fixed** (one misassociated `/ 2` in
`cal_move_dir_forecast`), so the count is **143 PASS / 0 FAIL** and no engine
divergence is outstanding on this corpus.

That is a statement about *this* corpus, and its main consequence is that the
corpus is now too small to say much: 6 usable segments, all human-vs-human, all
from four sessions. The next useful move is more (1,1) ground truth, not more
analysis of these sixteen. Two things are known-open rather than closed —
E3's actual +32 write (never identified, only shown to be unreachable in (1,1)
play), and E2a's device test.
