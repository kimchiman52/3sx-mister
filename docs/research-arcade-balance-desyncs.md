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
segments. Nothing here has had a Fable review yet.

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

### E3 — RETRACTED: `pos.x` +32 is not an engine divergence, and is now unobservable

**Symptom, as it was.** P0 `pos.x` read 424 where the archive held 392, at
archive frame 54. Two instances, `7733 game_5` and `1710 game_1`, both
**Alex vs Yang**.

**Disposition: retracted as a divergence claim; reclassified under H4b; not
observable in this corpus.** Both instances are human-vs-CPU segments, so with
H4b applied both now exit 3 and are never compared. That is *not* the same as
"fixed": what follows is the evidence for the reclassification, and the one
thing that stayed unproven.

**1. Both instances are CPU segments (measured).** `wu_operator` read straight
out of the archive at the match-start frame (`0x68C6F` / `0x69107`) is `(1, 0)`
in both, and `(1, 0)` for all 8,842 frames of `7733 game_5`. Both are rejected
by `ScrdGame_Init()`'s H4b check.

**2. The mechanism this entry proposed is REFUTED.** `Appear_24000()` and
`Appear_25000()` (`appear.c`) never run in either segment. `appear_player()`
dispatches `appear_jmp_tbl[wk->wu.routine_no[4]]`, and the archive holds
`routine_no[4] == 1` for P0 and `== 21` for P1 on **every** frame from 3 to 60
in both segments — `Appear_01000` and `Appear_21000`. Table indices 24 and 25
never occur. The `!wu_operator` X override was a plausible reading of the code
and is simply not what these recordings executed.

**3. It is a one-frame teleport, not a placement difference.** A one-off
position trace of `7733 game_5` (both players, ours vs archive, printed at the
head of `compare_main_values`) shows our P0 at 392 — the archive's value —
through frame 53 and 424 on frame 54, with P1 at the archive's 601 on both.
That it fails at 54 and not earlier is itself the proof it was right all along:
`Statcheck_CompareValues` asserts `pos` on every frame where
`G_No[1] == 2 && G_No[2] == 1`, and the archive holds that from frame 11.
Everything compared *ahead of* P0's `pos` on the failing frame agrees —
`Game_timer`, `C_No`, `G_No[1..3]`, `Random_ix32`, and for **both** players
`routine_no[0..7]`, `cg_add_xy`, `do_not_move`, `caution_flag`, `hit_stop`,
`dm_stop`, plus P0's `mvxy.a`/`mvxy.d`. So the +32 is a single direct write to
`wk->wu.xyz[0].disp.pos`, not accumulated motion — which rules out the `mvxy`
path as well as the two `appear.c` overrides.

**4. A (1,1) segment cannot exhibit it.** Every `wu_operator`-conditioned path
that can set round-start X is unreachable when both operators are 1:

- `Appear_24000()` / `Appear_25000()` are `if (!wk->wu.wu_operator)`;
- `home_visitor_check()`'s CPU arm is the `else` of `if (Play_Type)`, and
  `Play_Type == 1` exactly when both operators are set (`Setup_Play_Type()`,
  `sys_sub.c`);
- `move_player_work()` (`plcnt.c`) pins the two players' update order with
  `switch (plw[0].wu.wu_operator + (plw[1].wu.wu_operator * 2))` — `case 1`
  forces `move_P1_move_P2()` and `case 2` forces `move_P2_move_P1()` every
  frame, while `(1,1)` and `(0,0)` both fall to the `default` arm that
  alternates on `Game_timer & 1`.

A genuine human-vs-human recording has `(1, 1)` and so does the harness, so on
those segments the two sides take the same arm by construction. E3's symptom is
reachable only in the configuration the harness now refuses.

**5. What is NOT proven.** Which write produced the +32. Ruled out by
measurement: the two `appear.c` overrides (never dispatched) and `mvxy` drift
(compared equal on the failing frame). Still open as a candidate, unproven: the
screen-edge clamp `set_field_hosei_flag(&plw[0], scrr/scrl, …)` in
`move_P1_move_P2()` / `move_P2_move_P1()`, whose limits come from
`set_scrrrl()` -> `get_center_position()` = `bg_w.bgw[1].wxy[0].disp.pos`. That
camera value is neither imported nor compared by statcheck, and the order in
which the two clamps run is picked by the `wu_operator` switch above — so a
camera that is 32 off would snap exactly one player by exactly 32 on the frame
the clamp first arms (`bg_app_stop == 0 && bg_app == 0`). Confirming that needs
an arcade offset for `bg_w.bgw[1].wxy[0]`, which the H2 note's warning applies
to: it cannot be derived from the port struct (the arcade `BGW` has 4-byte
pointers and one extra byte ahead of `stage`), so it would have to come from
disassembly. Not done.

**6. Design (a) cannot settle it, measured.** Two runs with the archive's
`wu_operator` forced into `Operator_Status` / `plw[].wu.wu_operator`:

| experiment | first divergence |
|---|---|
| operators imported, harness otherwise unchanged | `w_lvr (-32760) != (-32764)` @ frame **7** |
| same, with `compare_lvr`/`compare_wcp`/`compare_waza_work` suppressed | `routine_no (3) != (1)` @ frame **11** |

Both segments, both experiments, identical results. Neither run reaches frame
54, so E3 is not re-measurable that way. The first failure is argument 1 of the
H4b design note in action (`Player_move()` discards the pinned lever word); the
second is argument 3 (a CPU operator inside the harness's synthetic VERSUS
match manufactures new divergences — here in the appear state machine —
*earlier* than the one it was meant to explain).

Previously excluded by measurement, all still valid: segmentation, input
alignment (our `C_No`, `G_No[1..3]`, `Game_timer` and injected input words match
frame-for-frame for 53 frames), stage (`app_type_tbl[1][10]` is flat across
stages and `routine_no[4]` matched), lever warm-up.

---

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
| E1a | `Play_Type == 1` damage pin has no arcade counterpart | **FIXED** — gated on `ArcadeBalance_IsEnabled()` at both `pow_pow.c` sites, `setup_vs_mode()` seeded to **3** (not 0), `ROUND_LEVEL_OFFSET 0x1137A` imported. Proven by disassembly (`0x0609E36C`/`0x0609E3FA` index `Round_Level` unconditionally); demonstrates on **no** corpus segment — reachable only via 2P break-in, which is not traced |
| E1b | port never updates `Round_Level` in VS | **RETRACTED, not a defect** — arcade `Loser_Sub` (`0x0609C616`) and `Update_VS_Data` (`0x0609C79A`) gate on `Play_Type` exactly as the port does. The archive decrements are all in human-vs-CPU segments |
| H4b | harness forces `Play_Type == 1` on every segment | **FIXED, by rejection** — `ScrdGame_Init` reads `wu_operator` at the match-start frame (`WORK_WU_OPERATOR_OFFSET`, archive `0x68C6F`/`0x69107`) and returns `SCRD_GAME_INIT_CPU_PLAYER`; `main.c` exits **3**, distinct from 1 and 2. Reproducing the CPU player was tried and refuted by measurement (see H4b) — it breaks input pinning at frame 7 and manufactures a new `routine_no` divergence at frame 11. Costs 8 of 16 segments; sweep now reports **0** divergences |
| E2a | `effect_G9_init()` spawn phase / `players_timer` | **oracle FIXED** `f63507b7` (drift 322/174/255/418 -> 0); **viewer FIXED** via `.3sr` v2 (host A/B: 31 -> 0 and 13 -> 0 `r16_resyncs`); NOT yet tested on the device, and existing v1 files keep the old behaviour |
| E2b | ~~a `random_32` consumer the port never runs~~ | **RETRACTED** — it is the CPU player's `Com_Initialize()`; same cause as H4b, not an engine defect. All six instances now exit 3 |
| E3 | `pos.x` +32 at round start | **RETRACTED, not an engine divergence** — both instances are `(1,0)` CPU segments and now exit 3. The proposed `Appear_24000`/`Appear_25000` mechanism is refuted by measurement (`routine_no[4]` is 1 and 21 in both, never 24/25). Unreachable in (1,1) play: every `wu_operator`-conditioned round-start-X path needs an operator flag clear. **NOT proven**: which write produced the +32 (candidate: the `set_field_hosei_flag` clamp against an unimported camera, `bg_w.bgw[1].wxy[0]`) |
| H1 | `ScrdGame_Init` post-KO false positive | **FIXED** — require `Game2_0()`'s `Game_timer=0`/`G_No[2]=3`; matchless segments exit 2, not 1 |
| H2 | stage not imported | **FIXED** — `BG_W_STAGE_OFFSET 0x26BB0` from disassembly; pinned via `Debug_w[DEBUG_STAGE_SELECT]` |
| H3 | lever counters never cleared | **FIXED** — seed `t_pl_lvr` in `Statcheck_SyncValues` like `players_timer`; the warm-up was never the defect |
| D1 | `vital_new` outside the hash window | E1 is undetectable on device by design — decide whether to widen |
| D2 | no rescan path | **FIXED** `c6a75572`; verified on device (13 -> 507 entries, desync detected) |

**For a reviewer:** the 16-segment corpus now reports **no engine divergence at
all** — 6 PASS, 8 rejected as unreproducible (H4b), 2 with no match (H1). Every
one of the eleven failures this document opened with had a harness cause. E1a
is applied; E1b, E2b and E3 are retracted; H1, H2, H3 and H4b have landed.

That is a statement about *this* corpus, and its main consequence is that the
corpus is now too small to say much: 6 usable segments, all human-vs-human, all
from four sessions. The next useful move is more (1,1) ground truth, not more
analysis of these sixteen. Two things are known-open rather than closed —
E3's actual +32 write (never identified, only shown to be unreachable in (1,1)
play), and E2a's device test.
