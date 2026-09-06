# 3SX — Arcade (CPS3) ROM Data Accuracy: Research, Root Cause & Worklist

**Date:** 2026-08-29 (first pass) · **2026-08-30** (second pass — §15, §16, §11.4)
· **2026-08-30** (third pass — §17, §18, §19)
· **2026-09-02** (fifth pass — §21, item Q: **sound codes**, a second
byte-passed namespace, found from a player bug report)
· **2026-09-02** (sixth pass — §22: **`cg_zoom` and `cg_effect`/`cg_eftype`
value-level diff** — both clean, no item-Q-class defect; the rest of the
byte-pass list swept as negative results)
· **2026-09-03** (seventh pass — §23: **`Random_ix16` stage-init divergence**
— CPS3 spawns two stage effects (ids 74 and 8) on Club Metro and Hong Kong
that the port never does; named from the arcade disassembly, reproduced
exactly on 4/4 affected replays; fix proposed, not applied)
**Repo:** `/Users/sb/Developer/3sx-mister`
**Branch examined:** `upstream-engine-fixes`; second pass verified in the
worktree `/Users/sb/Developer/3sx-mister-arcade`, branch `fix/arcade-cg-mapping`
@ `a5bc6a5b` (items A and K are landed — see §3, §8.A, §8.K)
**Upstream compared:** `crowded-street/3sx` @ `513380f9` ("Refactor texgroup (#372)")
**Tooling:** `tools/arcade-audit/` (this repo, same branch)

This is a **handoff document**. It is written so the work can resume cold, with
no prior conversation context. Every factual claim is traced to a `file:line`, a
command and its observed output, or a named primary source. Things that were
*not* verified are called out in §12 rather than smoothed over.

---

## 1. How to use this document

- **Just want the state of play?** §2 and §3.
- **Fixing the crash?** §5 (root cause) then §8 (worklist item A).
- **Fixing wrong sprites?** §7 (audit results) then §8 (items B-E).
- **Need to re-run the analysis?** §9 (tooling) — it works today, verified.
- **Need to reproduce the crash live?** §10 (exact build + run recipe).
- **Wondering what we *can't* find statically?** §11 — and §11.4 for the
  ground-truth oracle that now exists.
- **Think the crash class is closed?** Read §17 first — there is a *second*
  way the same render path faults, and it is not the one §6.1 audits.
- **About to trust "latent but unreachable"?** §18 — one such claim was wrong.
- **Chasing a wrong or missing *sound*?** §21 — `cg_se` is a second
  byte-passed namespace with six live divergences (item Q). Two characters are
  currently **silent**, not just wrong. Read §21.3 before diffing any sound
  code: codes are equivalent iff equal **mod 32**, so a raw diff over-reports.
- **About to audit another byte-passed field?** §21.4 — cell-index alignment
  misses 478 scripts cast-wide; §21.6 — 781 cells look divergent but are PS2
  converter artifacts (dead data). Both traps cost a pass to find.
- **Suspecting `cg_zoom` (super camera zoom) or `cg_effect`/`cg_eftype`
  (spawned effects)?** §22 — both were value-diffed arcade-vs-PS2 and are
  **clean**: no camera-zoom-level divergence exists anywhere in the 20
  characters, and the effect namespace is shared (no item-Q-class error). Do
  not re-diff them raw; §22.3's grid caveat explains why a raw diff lies.
- **Chasing a replay that desyncs at the first checkpoint, or wondering why
  intros/AI/dizzy on Remy's and Yun's stages never match arcade?** §23 —
  `Random_ix16` walks a different path from frame 1 on those two stages
  because CPS3 spawns two stage effects the port has no code for. The
  statcheck oracle has always hidden this (it overwrites `Random_ix16` every
  frame). Read §23.10 before adding a bare `random_16()` anywhere.
- **About to trust a frame-count argument for an OVCT walk?** §25 — the
  hold is bounded by enumerating every writer of the master's `hit_stop` and
  every freeze that is asymmetric between player and overlay, and the bound
  is re-derived by `cg_audit.py` on every run.
- **Wondering whether Twelve's X.C.O.P.Y. can hand one character's part
  index to another character's OVCT?** §26 — the reverse swap (`effk7.c`
  case 4) is armed only by a path that ends on the rebirth script's own
  marker three frames later, and every way out of that window is enumerated
  and closed; `cg_audit.py` re-derives it (`xcopy:` column) and also computes
  what a swap *would* consume, so a gate it cannot prove is reported, not
  assumed.
- **Reading an out-of-range-index count in the audit, or about to call a cell
  dead?** §28 — every `*_oob` column is split `live+dead` and the dead half is
  an entry-point closure over the six intra-script writers of `cg_ix`, not a
  scan; whole cast, every OOB-index class is 100% dead and the wrong-sprite
  class is 100% live. Do not use §19's "after the first terminator" as a
  reachability test — it is not one (§26.10.2), and neither is a
  forward-only walk (§28.2).
- **Worried the parse itself is truncating data?** §19.
- **Worried about hitboxes / throw ranges / attack properties?** §15 — the other
  13 sections (the ones a CG audit cannot see). This is upstream issue **#325**.
- **Worried about command inputs?** §16. **That question is closed** — do not
  re-open it.

---

## 2. TL;DR

1. **The reported bug is fixed-shaped and understood.** Upstream issue #363
   ("Ryu's Denjin Hadoken crashes the game") was reproduced under
   AddressSanitizer and root-caused. It is **not** a Ryu bug: the out-of-range
   sprite index lives in the **victim's** damage-reaction table. In the crash
   replay the victim is **Elena**.
2. **The defect class is now exhaustively enumerated.** An audit of all 20
   characters × 10 script tables (**133,901 cells**) against both the decrypted
   CPS3 ROM and the PS2 AFS found the crash class is **Elena-only: 66 cells at
   3 sites**. Nobody else in the cast can produce an out-of-range sprite index.
3. **A second, larger class exists: 1,694 wrong-sprite cells** (in-range but
   mismatched against PS2). These are cosmetic, not crashes, and are
   concentrated in Makoto (521), Ibuki (408), Urien (256), Twelve (88) and a
   13-character cross-bank cluster.
4. **The root mechanism is a modelling limitation, not a typo.** `remap_cg_number`
   models CG translation as *one delta per character* plus a few patch ranges.
   The data contains cross-bank references and per-script deltas that a single
   delta cannot express, so every new crash has historically bought one more
   patch range (upstream #290, #359, #360 — see §4.4).
5. **Whack-a-mole is over for the crash class.** `cg_audit.py` re-derives the
   whole finding in ~40 s and is checked against source constants at run time.
6. **The simulation is not the problem.** Statcheck reported **zero state
   divergences** through the faulting frame — this is purely a rendering/CG
   defect. The engine stays CPS3-accurate right up to the fault.

*Added by the second pass (2026-08-30):*

7. **The other 13 sections — where gameplay accuracy lives — carry no
   adaptation defect** (§15). Hitboxes (BODA/HANA/CATA/CAUA/ATTA/HOSA), movement
   (STXY/MVXY/PROT) and sound (SERND) are byte-identical arcade-vs-PS2 on every
   common element for all 20 characters. The differences that exist are **the
   balance change itself**, and they are now enumerated: **122 ATIT attack-property
   entries** (115 of them a single flag bit), **9 HIIT**, **4 BODA** and **55 RICT**
   elements. **Zero arcade-only out-of-bounds hazards** were found.
8. **One structural discovery.** RICT is `[group][opponent]`, with **24 opponent
   slots in the arcade table and 20 in PS2's** — so a naive element-wise diff
   reports 20,477 false differences where the real count is 55 (§15.4).
9. **Command data is settled** (§16): `arcade_cmd_data.c` is a byte-exact
   extraction of the CPS3 ROM's command tables, and arcade-vs-PS2 `pl_cmd`
   differs in **0 of 1120 slots**. A previous investigation concluded the
   opposite; it was disproved. Recorded so it is not re-opened.

*Added by the third pass (2026-08-30):*

10. **The crash class had a second door, and it was not empty.** The audit only
    ever checked `cg_number >= 37664`. The renderer then computes an unchecked
    residual `n -= texgrpdat[i].num_of_1st` and indexes a variable-length
    offset table with it. That table's length is now derivable statically for
    all 71 groups (§17.2). Bounds-checking every cell found **6 violations —
    all Remy, all landing in Gill's group, all pre-terminator, none
    pre-existing in PS2**, each producing a pointer **5.8 MB past the end of a
    3.0 MB allocation** (§17.3). They were reachable whenever **Remy and Gill
    are in the same match** (§17.4). §7.4's "the clamp produces wrong sprites
    rather than a fault" was wrong. **Fixed and landed (`a5bc6a5b`) 2026-08-30 —
    see §8.K; the current tree measures 0 (§17.3, §17.5).**
11. **A reachability model now exists.** Which texture groups can be loaded, and
    by whom, is derived from `ldreq_tbl[]`/`ldreq_ix[]` rather than guessed
    (§17.4). Applied to class (c): all 949 wrong-group cells land in
    *character* groups, **541 of them (57%) in Gill's**.
12. **No declared span truncates its data.** All 500 `location_data[]` spans
    were checked from the other direction: **0 truncated, 500 COVERED** (§19).
    The 500 spans tile the ROM in 8 contiguous runs with **zero overlaps**, no
    last script is cut off mid-body (0 of 200), and the arcade bytes past each
    short section's end do **not** continue that section. But "exhaustive" is
    not literally true: **`IBUKI atca` contains a complete 376-byte script that
    no pointer references** (PS2 has it too) and that no audit run has ever
    decoded — checked here, all its CGs are in Ibuki's own group and in bounds.
13. **§7.6's over-declaration list was understated and partly wrong** — the real
    count is **106 script spans, not 7**, and `ELENA exca` is not one of them
    (§19.7).
14. **"Latent but unreachable" was too strong for Elena's OVCT tail.** Her OVIX
    is the identity map over 91 entries and **does** name parts 85-90; what
    keeps them cold is two properties of the shipped data, not any code
    invariant (§18). The correct word is **undefended**. One genuine OVIX
    overrun does exist — Ibuki's, index 2277 against 2,230 entries — and it is
    **pre-existing in PS2**, from a byte-identical cell (§18.6(i)).
    **Superseded 2026-09-06 (§24): CLOSED, unreachable.** §18's "two data
    properties" were one property and one error: the timer walk is
    **stationary** (every one of Elena's 91 `parts_nix[i] == i`, so
    `eff01.c`'s `cg_ix++` branch never runs for her), and the only other
    writer is the cell's `olc >> 4`, whose maximum over all 7,769 cells is
    16. Parts **17-90**, not just 85-90, are unreachable, by exhaustive
    enumeration of every writer — no timing argument. `cg_audit.py` now
    computes the reachable part set per character (`ovct_reachability()`)
    and `residual_audit.py` reports OVCT violations on reachable parts (0),
    so the tail is **defended by the audit**. The same sweep found an
    **arcade-only dangling next-index on Dudley** (entry 177 → 178, past a
    178-entry table; §24.6(i), item R) — **closed 2026-09-06 (§25)**: the walk
    needs 297 frames of one unchanged `olc`; no writer can hold it past 179.

---

## 3. Current status

| Item | Status |
|---|---|
| Denjin crash root cause | **CLOSED** — identified, ASan-reproduced (§5) |
| Full-cast crash-class audit, door 1 (`obj_group_table` OOB) | **CLOSED** — 66 cells, Elena only (§7.2) |
| Full-cast crash-class audit, door 2 (residual) | **CLOSED** — 6 cells, Remy only (§17.3) |
| Elena crash fix | **LANDED** `23326679` — range applied in `src/arcade/arcade_char_data.c`; gate `cg_audit.py` class (a) 66 → 0 (§8.A) |
| Remy crash fix | **LANDED** `a5bc6a5b` — range applied in `src/arcade/arcade_char_data.c`; gate `residual_audit.py` residual-OOB 6 → 0 (§8.K) |
| Elena OVCT unpatched tail | **CLOSED 2026-09-06 — unreachable, defended by the audit** (§24). Parts 17-90 are indexed by no writer: the OVIX is the identity and no cell emits `olc >> 4` above 16, and the `eff01.c` timer walk is stationary (`parts_nix[i] == i` for all 91). No code change; `cg_audit.py` -> `ovct_reachability()` and `residual_audit.py` R2b `part_reachable` enforce it. Corrects §18's "undefended" |
| Dudley dangling OVCT next-index (arcade entry 177 → 178) | **CLOSED 2026-09-06 — unreachable, defended by the audit** (§25). The walk needs 297 (seed 130) / 594 (seed 82) consecutive frames of one `olc`; the master can hold those for ≤ 179 / 148 — the run's script frames plus one positive `hit_stop` per renewal cell, bounded at 23 by the largest value any writer hands an attacker. No code change; `cg_audit.py` -> `ovct_dangling_hold()` re-derives the bound; row `walk>end-unreached[178:hold<=179/297]` |
| 1,694 wrong-sprite cells (measured against the audit's oracle reach — §11.2 notes 162 more scripts, 2,441 cells, with no oracle at all) | **MOSTLY LANDED** items D, E, N (§8.D, §8.E, §8.N) — class (c) 1688 (post-§8.K baseline) → 89; item F (Chun-Li, 72 of the 89) investigated, deliberately left as-is (§8.F); remaining 17 enumerated with reasons (§8.D's Urien 0x52D9 ambiguity, and 7 of Necro/Hugo/Yun/Akuma's 9 smaller own-group cells — the same per-raw-value ambiguity; Akuma's other 2, `0x546B`, are a no-oracle block on a unanimous delta, not an ambiguity — §8.P) |
| Shape-divergent scripts (316) | **CLOSED 2026-09-06 — adjudicated, defended by the audit, and one real divergence found** (§29). A shape mismatch only means `audit()` skipped class (c) there; the question is decidable without the cell pairing, because `remap()` is a pure function of the raw `cg_number`, so a raw appearing in any shape-ok script is pinned by that script's PS2 counterpart. All 316: **225 direct + 60 bracketed + 20 no-live-cells + 4 unresolved + 7 DIVERGENT**; their 3,320 live L-cells: 2,710 + 465 + 94 bracket-disagree + 0 unbracketed + 51 divergent. Of the 51, **44 are new — Twelve `dmca[3]/[90]/[91]`, a hole in `twelve_cg_ranges` (§8.S, reported not fixed)**; the other 7 are §8.N's and §8.P's already-enumerated cells, independently rediscovered. The 4 unresolved are named with exactly what is unread (§29.5). No code change; `cg_audit.py` -> `manu_delta_gate()` re-derives it every run. §11.4's hardware oracle was assessed and **could not** answer this predicate — CPS3 RAM reports arcade numbering, not the PS2-side index the remap targets (§29.6) |
| Upstream issue #363 | **OPEN** upstream; our findings not yet reported (§13) |
| **The other 13 sections** (issue **#325**) | **AUDITED, no defect** — differences enumerated and classified (§15) |
| **Arcade command tables** (input recognition) | **CLOSED — no bug** (§16) |
| **`location_data[]` over-declared spans** | **CLOSED 2026-09-06 — no code change, defended by the audit** (§27). Both consequences adjudicated: (1) no path the engine can take forms a cell index inside any over-declared tail — every writer of `cg_ix` is modelled in `cg_audit.py` -> `span_closure()`, and the only cells reachable past a first terminator are 18 in-bounds script cells of Dudley `caca[6]`/`saca[87]` and Elena `atca[159]`; Remy's CAUA/HOSA tails are never indexed (`max cuix` 6 of 7, `max hoix` 11 of 12). (2) `ArcadeCharData_ComputeDigest` does hash 49,288 B (1.90% of its 2,590,884 B input) of decoded ROM past the real data, but the ROM is SHA-256-pinned to one revision (`rom_load.c`) and the decode is a pure function of it, so every peer hashes the same bytes: harmless, and deliberately NOT tightened (a digest move forces a lockstep client update). Real ends re-derived by reach, not by §19's terminator scan; §19.7's `Real end` column corrected in 14 spans |
| **Residual (second-door) bounds** | **AUDITED, FIXED** `a5bc6a5b` — pre-fix baseline was 6 violations, all Remy → Gill's group; current tree measures 0 (§17.3, §17.5); tooling `residual_audit.py` (§8.K) |
| **Under-declared (truncating) spans** | **CLOSED — none exist.** 500/500 spans COVERED (§19) |
| Unreferenced script in `IBUKI atca` | **OPEN, benign** — 376 B, real data, outside the 133,901-cell census; all CGs in bounds (§19.6) |
| **`cg_se` sound divergences** (item Q, §21) | **LANDED 2026-09-02** — `remap_cg_se()` in `src/arcade/arcade_char_data.c`; gates `--test-cg-se-remap` + `cg_se_audit.py` (29/29 cells); digest changes by design (§8.O) |
| **Texture-group offset-table lengths** | **DERIVED** — all 71 groups, statically, from `SF33RD.AFS` (§17.2) |
| **Group load reachability model** | **DERIVED** — from `ldreq_tbl[]`/`ldreq_ix[]` (§17.4) |
| Any *committed* code change | **YES, as of 2026-08-30/31** — see the note below; committed to `fix/arcade-cg-mapping`, not merged to `main`/`mister`, not on-device verified |

**Note on "committed" vs. "landed" (historical; corrects an earlier
self-contradiction).** The first pass modified nothing. Starting 2026-08-30
the `fix/arcade-cg-mapping` branch carries **committed** edits to
`src/arcade/arcade_char_data.c`: `23326679` (item A, the Elena range,
§8.A) and `a5bc6a5b` (item K, the Remy range, §8.K). An earlier draft of
this doc called these edits "landed" in one sentence and "proposed and
unlanded... not committed, not on any branch" in the next — that was wrong;
both are real commits, on this branch (`git branch -a --contains
23326679` / `a5bc6a5b` show only `fix/arcade-cg-mapping`), just not merged
upstream of it and not yet verified on-device. Every measurement in §15,
§16 and §19 is independent of them (those sections do not touch
`cg_maps[]`). §17 and §18 were run **both ways** for the Elena range: the
Elena OVCT findings are identical with and without it; the class-(a) count
moves (0 with it, 66 without), and the residual-OOB count moves independently
with the Remy range (0 with it, 6 without) — see §17.3.

**Update 2026-08-31.** Items **D**, **E**, **N** are landed (`CgRemapRange`
additions, commit `86a4d948`) and item **F** is investigated with no code
change, all on `fix/arcade-cg-mapping`, all still **committed to this
branch, unmerged to `main`/`mister`, and not on-device
verified** — see their status blocks in §8. `cg_audit.py`'s class (c) total
(the post-§8.K baseline, 1688) is now **89**, of which 72 are item F's
deliberately-unfixed Chun-Li cells and 17 are enumerated, reasoned remainders
(§8.D's Urien `0x52D9` ambiguity; 7 of Necro/Hugo/Yun/Akuma's 9 smaller
own-group cells — the same per-raw-value ambiguity, not a scope decision;
Akuma's other 2, `0x546B`, are a no-oracle block on a unanimous delta, a
different reason — §8.P).
Class (a) and `manu` (316) are unchanged.
`residual_audit.py` and `data_audit.py` both still hold their invariants.

---

## 4. Background: how arcade balance works

### 4.1 The two data universes

3SX is a decompiled PS2 port. "Arcade balance" replaces the PS2 character data
with the original CPS3 arcade data parsed out of the ROM, so the game plays with
arcade balance/frame data instead of the PS2 revisions.

The two datasets describe the *same* animations but index sprites in **different
namespaces**. That mismatch is the entire subject of this document. Upstream's
own framing, from Artem in Discord (2026-08-22):

> "whenever the game wants to render a different sprite for a character/entity
> it requests it using its index. and indices don't fully line up between PS2 and
> CPS3. I don't know why they had to re-map indices though."

### 4.2 Boot pipeline (our fork)

1. `src/main.c:474` → `ArcadeBalance_Init()`.
2. `src/arcade/arcade_balance.c:136` → `ArcadeCharData_Init()`.
3. ROM resolved from three candidate paths, loaded by `Rom_Load`
   (`src/arcade/rom_load.c:479`), which content-matches four 2 MiB SIMM slices by
   CRC32 pre-filter + pinned SHA-256 (`rom_load.c:41-46`, `:161-215`) and
   XOR-decrypts them into one 8 MiB big-endian image (`rom_load.c:94-118`,
   `cps3_decrypt.c:33-37`).
4. For each of `NUM_CHARS` = 20 characters (`src/constants.h:36`), 25 sections
   are parsed at hard-coded ROM offsets from `location_data[]`
   (`arcade_char_data.c` -> `location_data[]`) into `CharDataImage.spans[]`.
5. `coalesce_adjacent_sections` (`arcade_char_data.c:263-314`) merges adjacent
   ROM runs "because CPS3 data sometimes indexes across named section
   boundaries" (`:543-544`).
6. `arcade_balance.c:43-83` reads the **PS2** char-data tail per character out
   of the AFS (via `texgrpdat[character+1].apfn` / `.to_chd`) and calls
   `ArcadeCharData_Apply3SXRenderingConventions` (`:73`).
   **All-or-nothing:** any single character failing drops the whole session to
   PS2 balance (`:76-79`, `:143-149`).
7. On success, `digest = ArcadeCharData_ComputeDigest()` (`:151-152`), carried in
   the MIST netplay handshake so peers with differing adapted data reject rather
   than desync.

### 4.3 Runtime install and render

- `src/sf33rd/Source/Game/rendering/texgroup.c:417-437`: when arcade balance is
  on, the arcade `CharInitData` **replaces the PS2 one wholesale** via
  `SDL_copyp(dst, arcade_data)` (`:437`). Note the **texture/trans tables stay
  PS2** (`:395-397`).
- `charid.c:87-96` fans it into the live `WORK`: `char_table[0..9]` ←
  nmca/dmca/btca/caca/cuca/atca/saca/exca/cbca/yuca, plus `overlap_char_tbl`←ovct,
  `olc_ix_table`←ovix, `hit_ix_table`←hiit, `att_ix_table`←atit.
- Per-frame, script execution produces `wk->cg_number`, which reaches the
  renderer as:

```c
n = wk->cg_number;                 /* u16, 0..65535, straight from char data */
i = obj_group_table[n];            /* (1) NO BOUNDS CHECK — table is [37664]  */
if (i == 0) return;                /*     gap -> clean skip                    */
if (texgrplds[i].ok == 0) return;  /*     group not loaded -> clean skip       */
n -= texgrpdat[i].num_of_1st;      /* (2) can go negative                      */
trsbas = (u16*)(texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[n]);
count = *trsbas;                   /* (4) DEREFERENCE of a wild address        */
```

`obj_group_table` is `const u8 [37664]`
(`src/sf33rd/Source/Game/rendering/chren3rd.c:8`, decl `chren3rd.h:6`).

> **Step (2) is a second, independent fault door, and §6.1 does not audit it.**
> The offset table indexed at step (3) has a *finite length*, computed at run
> time as `*(u32*)trans_table / 4` (`mtrans.c:2533`). That length is now known
> statically for every group — see **§17**, which bounds-checks the residual
> for all 133,901 cells and finds six real violations.
The idiom above repeats at **nine sites** in our fork's
`src/sf33rd/Source/Game/rendering/mtrans.c`: `:174-187`, `:273-284`, `:362-379`
(`getObjectHeight`), `:415-427`, `:684-696`, `:807-819`, `:1088-1100`,
`:1216-1228`, `:1475-1487`. On upstream @ 513380f9 the faulting site is
`mtrans.c:400` in `mlt_obj_trans_ext`.

### 4.4 The remap — the single point of translation

**Only one value in the entire pipeline is ever translated: `cg_number`.**
(*True when written; since 2026-09-02 a second, far narrower translation
exists: `remap_cg_se()` rewrites six per-character `cg_se` exceptions at the
same parse site — item Q, §8.Q/§21. Everything below about `cg_number`'s
range-based remap is unchanged.*)

`remap_cg_number()` — `src/arcade/arcade_char_data.c:85-109`, applied at `:188`
and nowhere else:

```c
if (value < 0x400) return value;             /* :86-88  low CGs pass through   */
delta = cg_maps[character].default_delta;    /* :90     one delta per character*/
for each range: if (first <= value <= last) { delta = range->delta; break; }
adjusted = value + delta;
if (adjusted < 0 || adjusted > UINT16_MAX) return value;   /* :104-107 CLAMP   */
return adjusted;
```

Three properties matter, and all three are load-bearing defects:

1. **The only guard is `UINT16_MAX`.** Nothing ties the result to
   `obj_group_table`'s 37,664 entries. 37664 = 0x9320, so any result in
   [0x9320, 0xFFFF] is an out-of-bounds read.
2. **Failure to remap is silent and returns the raw CPS3 value** (`:104-107`).
   No log, no adaptation failure. Remy hits this path (§7.4).
3. **One delta per character cannot express cross-bank references.** When a
   script points at *another* character's sprite bank, the character's own delta
   scatters it somewhere wrong (§7.3).

Applied to the 10 script sections only (nmca, dmca, btca, caca, cuca, atca,
saca, exca, cbca, yuca — `arcade_char_data.c:501-510`). Within each cell, only
`cg_number` is remapped; `cg_se`, `cg_olc_ix`, `cg_hit_ix`, `cg_att_ix`,
`cg_extdat..cg_eftype`, `cg_zoom`, `cg_next_ix`, `cg_status` are byte-passed.
The other 15 sections are installed **raw**.

> **`cg_se` was byte-passed into a namespace that was authored against the PS2
> data, and that was a live defect — see §21 and worklist item Q (LANDED
> 2026-09-02: `remap_cg_se()` now translates the six diverging codes; all
> other `cg_se` values still byte-pass).** The byte-pass list above is a
> statement of *what the code does*, not a statement that byte-passing is
> *safe* for every field in it. Six arcade sound codes landed on the wrong TSB
> note, two of them on empty slots (silent). Treat the rest of this list as
> unaudited in the same way: `cg_zoom` and the `cg_eff`/`cg_eftype` pair have
> had no value-level arcade-vs-PS2 diff either.

### 4.5 The one other adaptation: OVCT

`ArcadeCharData_Apply3SXRenderingConventions` (`arcade_char_data.c:647-689`)
touches exactly **one** section, OVCT (overlap parts), and only three of its
fields:

- `:672-677` verification pass: every common element must match on
  `parts_hos_x/y, colmd, prio, flip, timer, disp, nix`
  (`overlap_behavior_matches`, `:611-616`). Any divergence fails the whole
  adaptation. **`parts_char` is deliberately NOT compared.**
- `:679-685` the entire remap: for `i < common_count`, three fields are
  overwritten from PS2 — `parts_colcd`, `parts_mts`, `parts_char`.
- `common_count = SDL_min(arcade_count, ps2_count)` (`:670`). **Anything past
  that keeps raw CPS3 values** (see §8.B).
- `:648, 654-656` one-shot latch per character: a second call returns `true`
  without inspecting anything.

### 4.6 Upstream fix history for this exact class

All of these are single-range patches added after a crash was reported:

| Commit | PR | What it added |
|---|---|---|
| `da493399` | #181 | Arcade char data (initial reader) |
| `ae309dc4` | #185 | Adjusted the remap cutoff `0x800` → `0x400` |
| `1a2d354e` | #196 | Fixed Ibuki's cg numbers |
| `1f64b621` | #283 | Missing-palette crash; introduced `Apply3SXRenderingConventions`, **deleted** the richer `remap_ovct_parts_char` |
| `3b38d29d` | #290 | **Fix Elena's CG range mapping** — added `0x9C88-0x9CC1 → -0x6F08` |
| `0ef92066` | #350 | Preserve arcade data adjacency (`coalesce_adjacent_sections`) |
| `fee66bf8` | #359 | **Fix X.C.O.P.Y. crash against Sean** — added `0x70F4-0x70FF → -0x2F74` |
| `bcd7b892` | #360 | Fix X.C.O.P.Y. transition animations — added **17** ranges at once |

**Read #290 and #359 carefully: they are the same bug as #363.** A single move
crashed because a flat `default_delta` mis-mapped one CG sub-range. Elena's
Denjin-shock cells sit *immediately above* the range #290 added.

### 4.7 Why nothing catches this automatically

`arcade_balance.c:114-119` pins the test runner to PS2 balance:

> "The frame-data suite's corpora encode PS2-balance expectations; the harness
> must resolve identically on every machine regardless of ROM presence."

So the 1,349-entry frame-data suite **never exercises the arcade path**. Also
note `SDL_assert` is compiled out in Release (`-DNDEBUG`), including the
`SDL_assert(adapted && arcade_data != NULL)` at `texgroup.c:424`.

---

## 5. Root cause: the Denjin crash (upstream #363)

### 5.1 The report

Upstream issue **#363**, "Ryu's Denjin Hadoken crashes the game", filed by Artem
(apstygo) 2026-08-19, milestone 1.0, still OPEN. Body in full:

> This looks like a CG mapping issue
> Replay ID: 1787095232817-6231.7 — Game index: 2

Attachment `game_2.scrd.zip` → `game_2.scrd` (23,052,544 bytes), downloadable
from `https://github.com/user-attachments/files/31238480/game_2.scrd.zip`.

### 5.2 The reproduction (verified 2026-08-29)

Built upstream @ 513380f9 with statcheck + ASan and ran the attached replay:

```
==96666==ERROR: AddressSanitizer: global-buffer-overflow
READ of size 1 at 0x000104dba0c2 thread T0
    #0 mlt_obj_trans_ext      mtrans.c:400
    #1 mlt_obj_trans          mtrans.c:638
    #2 Mtrans_use_trans_mode  aboutspr.c:290
    #3 sort_push_request      aboutspr.c:389
    #4 reqPlayerDraw          plcnt.c:479
    #5 Game2_1                game.c:523
0x000104dba0c2 is located 482 bytes after global variable 'obj_group_table'
  (0x000104db0bc0) of size 37664
```

From lldb at the fault: `wk->cg_number = 38146` (0x9502). Array max valid index
is 37,663 — **482 past the end, exactly matching ASan's offset**.
`wk == &plw[0]` and the caller is `plcnt.c:479` → **Player 1**.

**Match context:** `My_char[0] = 8` (Elena, P1), `My_char[1] = 2` (Ryu, P2),
`Super_Arts[1] = 2` (SA-III = Denjin Hadouken). Statcheck `frame_index = 3913`.

### 5.3 Where the bad value comes from

A watchpoint on `plw[0].wu.cg_number` with condition `== 38146` caught the write:

```
frame #0: setupCharTableData   charset.c:121   (dst[i] = src[i])
frame #1: check_cgd_patdat     charset.c:2404
frame #2: check_cm_extended_code charset.c:434
frame #3: Damage_12000         plpdm.c:496
frame #4: Player_damage        plpdm.c:201
frame #5: player_mv_4000       plmain.c:326
frame #6: move_P1_move_P2      plcnt.c:982
```

At that stop `plw[0].wu.now_koc = 1` → `char_table[1] = cdat->dmca`
(`charid.c:80`), and `char_init_data[10].dmca ==
ArcadeCharData_Get(CHAR_ELENA)->dmca` (identical pointers).

**So: the out-of-range CG is read out of Elena's arcade-ROM damage-reaction
table while she is being shocked by Ryu's Denjin projectile.** It fires **19
times** across the replay with three consecutive CGs — 38146 (×9), 38147 (×6),
38148 (×4) — i.e. one 3-frame shock animation.

### 5.4 Why the naive hypothesis was wrong (recorded so it isn't re-run)

The obvious theory — that Ryu's Denjin data is broken — was **disproven by
measurement**. Ryu's entire Denjin chain (`saca[40..42]`, `cbca[13..18]`) is
byte-identical between CPS3 and PS2 after remap; all 676 of his distinct CG
numbers land inside his own group 3 (`0x0A20..0x0DCA`, `texgrpdat[3].num_of_1st
= 2592`); his projectile rows (`tama_data` 8/9/10/11/52) match. His OVCT theory
also died: PS2 Ryu OVCT = 56 entries ≥ arcade's 54, so every `parts_char` is
patched.

**Do not re-investigate Ryu's tables. The victim's table is the carrier.**

### 5.5 The symptom is memory-layout-dependent (important)

The **non-ASan** build of the same commit **completes the replay with exit 0**.
From lldb on that binary: `obj_group_table[38146]` is `const`, so it lands in
`__TEXT.__const`; index 38146 reads a **zero byte inside the neighbouring
`color_file`**, so `mtrans.c:402` (`if (i == 0) return;`) silently drops the
sprite.

Consequences:

- On macOS the bug is a missing sprite, not a crash.
- On a target where that neighbouring byte is **non-zero**, `i` becomes an
  arbitrary value indexing `texgrplds[100]`, and the code then either spins
  forever in upstream's `while (1) {}` or dereferences a garbage `trans_table`.
- **On our fork**, the 2026-04-29 trap sweep converted those hangs into
  skip+log, so our worst case is a garbage-pointer deref → **SIGSEGV**. No
  SIGSEGV handler is installed (`src/main.c:176-191` registers only
  SIGINT/HUP/TERM/USR1/RTMIN+2..4), so on the MiSTer this appears as
  **`exit=139`** in `/media/fat/games/3s-arm/logs/last-run.log` with **no
  backtrace**. (`exit=134` would instead mean `fatal_error()`/`abort()`.)

**Therefore Artem's ~95.5% daily replay pass rate very likely undercounts this
class** — a non-ASan runner can pass a replay that is performing OOB reads.

### 5.6 Control run

Same binary, `arcade-balance = false`: statcheck stops at
`test_runner_compare.c:318: lvr_3sx->sw_new (6) != lvr_cps3->sw_new (2)`,
exit 1, at `frame_index = 720`. Frame 3913 is unreachable without arcade
balance — confirming the crash path is arcade-balance-only.

---

## 6. The audit: method

`cg_audit.py` (preserved, §9) decodes **every** arcade script cell for all 20
characters and attempts to pair it against a PS2 counterpart — **162 of those
scripts (2,441 cells) have no PS2 counterpart to pair against at all** and are
counted separately (§11.2), not silently treated as passing.

- **Constants are parsed from source at run time, not hand-copied** — so the
  audit stays honest as the code changes. Verified header line:
  `obj_group_table=37664 effinitjptbl=59 decode_chcmd=125
  sound_effect_request=1024 tama_data=243 sa_sign_data=69`.
- Arcade side: `rom.bin`, produced by `decrypt.py` from `sfiii3nr1.zip`; the four
  SIMM SHA-256s were confirmed equal to the digests pinned at `rom_load.c:41-45`.
- PS2 side: `SF33RD.AFS`, entry `texgrpdat[char+1].apfn`, tail at `.to_chd`, then
  `get_ps2_section_span` logic mirrored from `arcade_char_data.c:618-645`.
- **133,901 cells audited.** Runtime ~40 s.

### 6.1 Violation classes

| Class | Meaning | Severity |
|---|---|---|
| **(a)** `a_ogt_oob` | remapped CG ≥ 37664 | **crash class** — OOB read |
| **(b)** gap | remapped CG hits an `obj_group_table` zero | silent sprite drop |
| **(c)** mismatch | in-range but ≠ the PS2 counterpart's CG | wrong sprite drawn |
| — | `c_own_group` / `c_other_group` | sub-split by whether it stays in the character's own texture group |
| manu | script shapes differ; no cell-aligned oracle | needs human review |

Two discriminators had to be built in to avoid false positives, and are worth
knowing if the script is ever modified:

1. `cg_se` bit `0x800` selects the random-SE table rather than indexing
   `sound_effect_request[]` (`charset.c:2723-2727`). Checking `>= 1024` naively
   produced **1,848 false positives**.
2. **Any value identical on both sides is a pre-existing property of shipping PS2
   data, not an adaptation defect.** This filter cleared, among others, Makoto's
   `atca[108] → jsr(cbca,38)` against a 30-entry `cbca` — PS2's Makoto `cbca` is
   *also* 30 entries and PS2's `atca[108]` issues the same `jsr`, so it is a
   latent hazard in the shipped PS2 build too, and out of scope here.

---

## 7. The audit: results

```
char    cells |  (a)  (b) (c)wg (c)og  manu | ovct a/p    ovix a/p
GILL     6634 |    0    0     0     0     8 | 392/396 ok  146/148 short
ALEX     6546 |    0    0     0     0    21 | 57/59   ok  53/55   short
RYU      5099 |    0    0     0     0    14 | 54/56   ok  7/9     short
YUN      8859 |    0    0     0     1    31 | 6/8     ok  20/22   short
DUDLEY   7051 |    0    0    23     0    16 | 178/180 ok  41/43   short
NECRO    6484 |    0    0    23     1    16 | 46/48   ok  10/12   short
HUGO     5952 |    0    0    23     1    15 | 71/73   ok  62/64   short
IBUKI    8222 |    0    0    23   408    21 | 2286/2296 ok 2230/2235 short
ELENA    7769 |   66    0    23     0    13 | 91/85 UNPATCHED-TAIL! 91/85
ORO      6191 |    0    0    23     0    14 | 67/69   ok  41/43   short
YANG     8613 |    0    0    23     0    30 | 20/22   ok  20/22   short
KEN      5056 |    0    0    26     0     9 | 42/44   ok  24/26   short
SEAN     5547 |    0    0    31     0     9 | 25/27   ok  25/27   short
URIEN    6268 |    0    0    23   256    21 | 329/352 ok  187/189 short
AKUMA    5881 |    0    0    23     6    15 | 115/117 ok  34/36   short
CHUNLI   6726 |    0    0     0    72     9 | 75/78   ok  75/77   short
MAKOTO   7192 |    0    0   521     0    15 | 252/254 ok  139/141 short
Q        7178 |    0    0    29     0    15 | 18/20   ok  18/20   short
TWELVE   7268 |    0    0    88     0    11 | 133/135 ok  133/135 short
REMY     5365 |    0    0    47     0    13 | 42/44   ok  31/33   short
TOTAL         |   66    0   949   745   316
cells audited: 133901
```

### 7.1 Validation — the audit rediscovers all three historical fixes

Counterfactual runs with each shipped fix reverted (`counterfactual.py`,
`cg_counterfactual.json`):

| Reverted | class (a) | class (c) | Where it reappears |
|---|---|---|---|
| baseline (HEAD) | 66 | 1694 | — |
| **#290** (Elena `0x9C88-0x9CC1`) | **304** | 1694 | Elena, as **class (a)** — the crash class |
| **#359** (Sean `0x70F4-0x70FF`) | 66 | **1740** | Sean `saca[0,1,6,7]`, as class (c) |
| **#360** (all `0x7070-0x714B`) | 66 | **2530** | `saca[0,1,6,7]` of **all 20** characters (882 violations) |
| all three | 304 | 2530 | — |

This is the evidence that the audit would have caught each historical crash
*before* a user hit it.

### 7.2 Class (a) — the crash class: Elena only, 66 cells, 3 sites

| Table | Raw CG | Remapped | PS2 counterpart | Cells | Scripts |
|---|---|---|---|---|---|
| `dmca` | 0x9D22-0x9D24 | 38146-38148 | 0x2DE0-0x2DE2 | 24 | 82-89 |
| `btca` | 0x9D22-0x9D24 | 38146-38148 | 0x2DE0-0x2DE2 | 4 | 15 |
| `exca` | 0x9CFC-0x9D21 | 38108-38145 | (shape differs) | 38 | 58-65 |

- `dmca[82..89]` is the electric-shock damage animation — **the #363 crash**.
- `btca[15]` (a knockdown) uses the **same three sprites** and was not in the
  bug report.
- `exca[58..65]` is a 38-cell contiguous run, also unreported.

All sit immediately above the #290 range's `last = 0x9CC1`, take the default
delta **−0x0820**, and land past `obj_group_table[37664]`.

**Measured correct delta for the adjacent block: −0x6F42** (from the 16
cell-aligned pairs; #290's range uses −0x6F08).

Confidence note: for class (a) the OOB is a property of the arcade value plus
the remap alone, so **all 66 are certain**. The high/low split in the JSON
(16 high, 50 low) refers only to whether a cell-aligned PS2 counterpart could be
annotated, not to whether the cell is OOB.

**Fixing only what the bug report describes would fix 24 of 66 cells.**

### 7.3 Class (c) — wrong sprites: 1,694 cells, three sub-families

**(i) Cross-bank references — 949 cells.** The clearest case: raw `0x0CB4`
appears in `yuca[68..75]` of **13 characters**. Each character's own delta
scatters it to a different wrong group (Dudley→grp 2, Hugo→grp 1, Urien→grp 1,
Ken→grp 12, Sean→grp 13, Akuma→grp 15…), while **PS2 resolves it with Ryu's
−0x1E0 to group 3**. Same shape: Makoto `atca`/`exca` (521 cells, PS2 wants
−0x1E0), Twelve `nmca`/`cuca`/`exca` (88, PS2 wants Necro's −0x600 — the
X.C.O.P.Y. family), Q `cuca[37]` (29), Sean `cuca[64]` (8), Ken `caca[18..20]` (3).

**This sub-family is unfixable by adding more per-character ranges** without
either (a) ranges fine enough to isolate every cross-bank reference, or (b) a
model change (§8.E).

**(ii) Off-by-N deltas inside the character's own group — 745 cells.**
- **Ibuki**: `saca[56..59]` + `yuca` — 408 cells where arcade delta −0x74D0 is
  **exactly one less** than PS2's −0x74CF. Draws the neighbouring sprite.
- **Urien**: `yuca[8..15]` — 256 cells spanning **ten distinct correct deltas**
  (−0xC6F … −0xC78) against the flat −0xC60.
- Smaller: Akuma `nmca[21,46]`, Yun `nmca[26]`, Necro `nmca[28]`, Hugo `btca[15]`.

**(iii) Probably-intentional 3SX content edits — not remap bugs.**
- **Chun-Li `saca[44..47]`, 72 cells**: PS2's CG is literally `0x0000` (blank).
  3SX appears to have removed those sprites deliberately. **Do not "fix" these
  without checking intent.**

### 7.4 The negative-clamp pass-through

Remy (and part of Makoto) hit `arcade_char_data.c:104-107`: `raw + delta < 0`
returns the value **unchanged**, silently passing e.g. Alex-range values
straight into Alex's group. Remy `nmca` (37+3 cells), `exca`, `cuca`. This is
the clamp behaving as written, producing wrong sprites rather than a fault.

> **CORRECTION (third pass, §17.3): it can fault.** Six of those clamped Remy
> cells (`nmca[48]`, `exca[30]`, `exca[37]`, `exca[38]`) pass raw CG **1537** —
> an *Alex* value — straight through. `obj_group_table[1537]` is **Gill's**
> group 1, whose offset table has only 1,435 entries (valid indices 0..1434),
> so the residual 1537 is **103 past the last valid index** and `((u32*)trans_table)[1537]` reads
> `0x00870035`, i.e. a pointer 5.8 MB past the end of a 3.0 MB allocation.

### 7.5 OVCT / OVIX structural findings

- **Elena is the only character with `arcade_count > ps2_count`: 91 vs 85.**
  Parts **85-90 keep raw CPS3 `parts_char` 0x9CF6-0x9CFB (40182-40187)** — every
  one ≥ 37664. `read_ovct` (`arcade_char_data.c:370-393`) never remaps
  `parts_char`, and the patch loop only covers `i < common_count`.
  **Status corrected by the third pass — see §18.** The "no selected `ovix`
  entry reaches a part ≥ 85" reason is **wrong**: Elena's OVIX is the identity
  map over 91 entries and names parts 85-90 outright. What is true is that no
  Elena *cell* emits an effective `cg_olc_ix` ≥ 85 (max 16 over 7,769 cells) —
  a property of the data, not a guard. **Undefended, not unreachable.**
  **Closed by §24 (2026-09-06): unreachable, and defended by the audit.**
  §18's residual worry — the timer walk marching up to 85 — cannot happen:
  every one of Elena's 91 entries has `parts_nix[i] == i`, so the walk is
  stationary. Parts 17-90 are indexed by no writer. `cg_audit.py` computes
  the reachable set (`r<=16`) and prints `tail-unreached(6)`.
- **Every other character's OVIX is 2-5 entries SHORTER than PS2's.**
  `wk->cg_olc = wk->olc_ix_table[wk->cg_olc_ix]` (`charset.c:2739`, `:2904`) is
  unbounded. **Corrected by the third pass (§18.6(i)): one character does
  overrun** — Ibuki's `cuca[37]` cell 28 emits effective index 2277 against a
  2,230-entry arcade OVIX. Its PS2 counterpart cell is byte-identical and PS2's
  table is 2,235, so it overruns there too — pre-existing, not an adaptation
  defect. The other 19 characters are in range. (Confirmed PS2's own `dmca[82..89]` *does* use
  `olc = 112/128` → `cg_olc_ix` 7/8, where arcade uses 0.)

### 7.6 Over-declared `location_data` sizes (new finding, low priority)

Seven sections declare a size far past their real script data, so
`read_char_table`'s last script (`end_offset = location.size`,
`arcade_char_data.c:135`) decodes unrelated ROM as cells — and
`ArcadeCharData_ComputeDigest` **hashes that slack**, which matters because the
digest gates netplay compatibility.

```
HUGO   saca  declared=0x4164  real=0x34C4  slack=0x0CA0
ELENA  saca  declared=0x6638  real=0x6078  slack=0x05C0
ELENA  exca  declared=0x35BC  real=0x2E1C  slack=0x07A0
SEAN   caca  declared=0x12C0  real=0x0CD8  slack=0x05E8
URIEN  saca  declared=0x3A54  real=0x3184  slack=0x08D0
TWELVE saca  declared=0x5E50  real=0x5530  slack=0x0920
REMY   yuca  declared=0x73E0  real=0x11B0  slack=0x6230  (24 KB!)
```

Parsed-but-unreachable at run time (execution is bounded by each script's own
terminator), so not a live fault.

> **CORRECTED by the third pass — see §19.7.** The `real=` column above is
> `size − max(pointer)`, which is not the real end. Measured from each last
> script's terminator read-end: **`ELENA exca`'s slack is 0** (it is not
> over-declared at all), the other six figures shrink, and the real list is
> **106 script spans, not seven**, totalling 36,680 bytes — all hashed into the
> digest.

> **CLOSED 2026-09-06 — see §27.** The real end is now derived from what the
> executor can reach, not from the first terminator (which the §19 convention
> is now known to undercount — a `jmp`/`jsr` `pat` is a 1-based cell index and
> can land past it). Nothing executes any tail; the digest hashes the tails but
> deterministically; no code change.

---

## 8. Worklist

Ordered by severity. Item lettering is historical (A-J from the first two
passes, K-M added by the third, N-O added by the fourth, P added by the fourth,
**Q added by the fifth — the sound-code pass, §21**); the two crash-class
items are **A** and **K**, and they are independent of each other. **As of
2026-09-02, A, K, D, E, N and Q are LANDED** (see their status blocks below and
§3) and **F is CLOSED as investigated-not-a-defect** (§8.F); everything else
in this worklist remains unimplemented.

**Keep this paragraph and §3 in step with the status blocks.** They are the
only two places a reader checks before trusting an item, and they are the first
things to go stale. Any pass that lands an item updates all three — the item's
own status block, this list, and the §3 row — in the same commit as the code.

### Standing requirement for every remaining item: balance gating

**Maintainer instruction, 2026-08-30.** No fix in this worklist may change PS2
behaviour. Arcade-only is the contract, the same one upstream's #290 / #359 /
#360 hold to.

For the CG range tables (items D, E, F, N), **the guarantee that actually holds
is "no PS2-session behaviour change," not "a PS2 session cannot reach any of
this"** — those are different claims, and the second one is false. Verified in
`arcade_balance.c` -> `ArcadeBalance_Init()`: `ArcadeCharData_Init()` is called
unconditionally, *before* `is_enabled = true` is set, inside the same `do { ...
} while (0)` block that later checks `adapt_all_characters()`. So on any launch
where the CPS3 ROM is present but `adapt_all_characters()` subsequently fails,
`ArcadeCharData_Init()` -> `read_char_table` -> `remap_cg_number` has already
run for all 20 characters in a session that goes on to run PS2 balance
(`is_enabled` stays `false`). `validate_cg_ranges()` (§8.C above) runs even
earlier and even more broadly — unconditionally at the top of
`ArcadeCharData_Init()`, in Debug builds, on every launch regardless of
ROM presence.

**What actually holds:** every *consumer* of the parsed arcade tables —
every render/OGT/mtrans site, every place `ArcadeCharData_ComputeDigest()` or
the parsed buffers are read — is behind `ArcadeBalance_IsEnabled()`, which is
`false` whenever the session isn't arcade. So a range-table edit changes
*execution* on some PS2-balance launches (the CG remap runs, its result is
computed and discarded) but never changes *observable PS2-session behaviour*,
because nothing downstream reads that result when `is_enabled` is false. That
is the contract this worklist actually verifies — narrower than "a PS2 session
never enters that code."

The exposure is item **E**. If the cross-bank cluster is solved by changing the
remap *model* — letting a range name a target bank rather than a bare delta —
and that change reaches shared engine code rather than staying inside
`src/arcade/`, it needs an explicit `ArcadeBalance_IsEnabled()` gate. Anything
touching `texgroup.c`, `charset.c`, `mtrans.c` or the `eff*` files is in that
category by default.

Verification, not assertion: run the audits and a PS2-balance regression pass
and show PS2-side behaviour unchanged. `configuration.test.enabled` pins PS2
(`arcade_balance.c`), so the frame-data suite is already a PS2-side control.

The same requirement applies to items **C** and **L** (the bounds guards): a
guard placed in the render path runs under both balances and on a 60 fps
budget, which is why §8.C prefers adaptation-time validation — it runs once,
under arcade only.

### A. Elena's crash-class cells — 66 cells, 3 sites (DO FIRST)

The measured correct delta for the adjacent block is **−0x6F42**. The naive fix
(extend #290's range) is wrong: #290 uses −0x6F08, and the three sites span
`0x9CFC..0x9D24`, which is a *different* block from `0x9C88..0x9CC1`.

Suggested shape: add a new `CgRemapRange` to `elena_cg_ranges` covering
`0x9CFC-0x9D24` with delta **−0x6F42**, then re-run `cg_audit.py` and confirm
class (a) drops to **0**. Verify against the 16 cell-aligned PS2 counterparts
(`dmca[82,83,86,87]`, `btca[15]` → PS2 `0x2DE0-0x2DE2`) rather than trusting the
arithmetic alone. The `exca[58..65]` run has no cell-aligned oracle, so confirm
visually or via a replay that exercises it.

> #### Status 2026-08-30: LANDED
>
> Committed to `fix/arcade-cg-mapping` as an edit to
> `src/arcade/arcade_char_data.c` adding exactly that range to
> `elena_cg_ranges` — `{ .first = 0x9CFC, .last = 0x9D24, .delta = -0x6F42 }` —
> with a comment recording the derivation. Observed with the edit in the tree
> (`cg_audit.audit()` called in-memory so `cg_audit.json` was not rewritten):
>
> ```
> TOTAL (a)=0 (b)=0 (c)wg=949 (c)og=745 manu=316 cells=133901
> distinct raw CGs in 0x9CFC-0x9D24 across Elena's scripts: 41
> their obj_group_table groups: {9: 41}
> ELENA class-(a) count now: 0
> ```
>
> So: class (a) **66 → 0**, class (c) and `manu` unchanged, and all 41 distinct
> raw CG values in the block land in group **9**, Elena's own texture group
> (`own_group = character + 1`, `cg_audit.py`).
>
> **Committed, unmerged (corrects an earlier self-contradiction in this
> block).** This edit is commit `23326679` on `fix/arcade-cg-mapping` — it is
> committed and on that branch, contrary to an earlier draft's "not
> committed, not on any branch" in this same spot. It is not merged to
> `main`/`mister` and not verified on-device. The `exca[58..65]` run still
> has no cell-aligned PS2 oracle (§12), so "lands in group 9" is a necessary
> condition, not proof the sprites are the intended ones. §11.4 now describes
> the tool that could settle it.

> #### Severity, from the trigger analysis (§20)
>
> The 66 cells are not equally live:
>
> - **`dmca[82..85]` — CONDITIONAL, routine.** The electric ground damage
>   reaction. Reached by **Ryu's Denjin projectile, Necro and Urien** — the only
>   three electric attackers in the game (§20.2). #363 is one of three.
> - **`dmca[86..89]` — CONDITIONAL, narrower.** The same hit against a
>   knocked-down Elena (`get_kagami_damage`, `hitcheck.c:513`).
> - **`btca[15]` — ROUTINE, and *easier* to reach than #363's site.** Every
>   electric hit on an airborne Elena, and every electric **KO**
>   (`dd_convert[43] = 104`), lands here. It carries the same three CGs
>   (38146-38148) as `dmca[82..89]`, so the 19 faults in the §5.2 replay cannot
>   be attributed to the `dmca` site alone.
> - **`exca[58..65]` — APPARENTLY UNREACHABLE (38 of the 66 cells).** No control
>   cell in Elena's ten tables names `(koc = 7, ix = 58..65)`, and no C call site
>   passes `koc = 7` at all (§20.3).
>
> This does not change the fix — one `CgRemapRange` row covers all 66 — but it
> does mean **fixing only `dmca[82..89]` would leave the more common door
> (`btca[15]`) open**, and that a replay-based verification should target an
> electric KO, not just a Denjin connect.

### B. Elena's unpatched OVCT tail — parts 85-90

**Undefended, not unreachable** — §18 corrects the earlier framing and gives the
delta: the PS2 patch loop's trailing band is **−29360**, which maps the six tail
values to 10822-10827, in Elena's own group 9 at residual 614-619. Options: extend the patch loop past `common_count` with an
explicit remap for `parts_char`, or add a bounds check where `parts_char`
becomes `cg_number` (`eff01.c:169`). Note upstream **deleted** a richer
per-character OVCT remap (`remap_ovct_parts_char`) in #283 — its Ibuki/Urien
bands have no equivalent today.

> #### Status 2026-09-06: CLOSED — unreachable, no code change, defended by the audit (§24)
>
> Neither option above was taken, on purpose. Parts 85-90 — and parts 17-90
> with them — are indexed by **no writer** of the part index: the OVIX is the
> identity and the maximum `olc >> 4` over all 7,769 of Elena's cells is 16;
> `plcnt_init` writes 0; `exdm_ix_data[*][8][3]` is 0; and the `eff01.c` timer
> walk is **stationary** because every one of her 91 entries has
> `parts_nix[i] == i` (§18.3's "monotone and unbounded march" was wrong —
> §24.4). The X.C.O.P.Y. table-swap window is closed by data too (§24.3).
> Applying −29360 would have shown six sprites (10822-10827) nobody can prove
> are the right ones, on a path nobody can reach, and moved the netplay
> digest for it. Instead the reachability model is now code:
> `tools/arcade-audit/cg_audit.py` -> `ovct_reachability()` (table column
> `ovct a/p reach`, ELENA row `91/85 r<=16 tail-unreached(6)`), and
> `residual_audit.py` R2b annotates each OVCT violation with
> `part_reachable` and prints the invariant `on a REACHABLE part : 0`. A
> future cell, table or writer that makes a tail part reachable turns the
> row into `TAIL-REACHED(n)!`.

### C. A bounds guard (cheap, high value, defensive)

Neither `remap_cg_number` nor any of the nine `mtrans.c` sites checks against
37,664. A guard turns every *future* instance of this class from a
layout-dependent SIGSEGV into a dropped sprite plus a log line — which is
exactly what the 2026-04-29 trap sweep did elsewhere in the tree. Consider both:
a clamp/reject in `remap_cg_number` (with a log) and a bounds check at the
`obj_group_table[n]` sites.

**A related but distinct guard: `CgRemapRange` row overlaps.**
`arcade_char_data.c`'s `validate_cg_ranges()` checks a different hazard —
`remap_cg_number` takes the *first* matching row in a character's table and
stops, so a later row that shadows an earlier one would silently apply the
wrong delta with no diagnostic. It is `#if DEBUG`, and `DEBUG` is defined only
for `CMAKE_BUILD_TYPE=Debug` (`CMakeLists.txt`); every shipping pipeline
(`tools/mister/build-game.sh`) configures Release, so **this guard protects
nothing in a build a user runs.** Decision, 2026-08-31 cleanup pass: do
**not** promote it to `SDL_assert_always` — turning a wrong sprite into a
crash for the user inverts the point of this whole branch. Instead the check
now runs where it actually gates something: `tools/arcade-audit/cg_audit.py`'s
`check_range_overlaps()` re-derives the same check from source on every audit
run (any build config) and exits non-zero if it ever finds one. The `#if
DEBUG` assert stays, as a developer convenience only, with its comment
corrected to say so.

### D. The off-by-N deltas — Ibuki (408) and Urien (256)

Mechanical: the PS2 counterparts give the exact per-script deltas. Ibuki needs
−0x74CF for `saca[56..59]` and parts of `yuca`; Urien needs up to ten distinct
deltas for `yuca[8..15]`. Both are range-table work, no model change needed.

> #### Status 2026-08-31: LANDED
>
> Committed to `fix/arcade-cg-mapping` as edits to `src/arcade/arcade_char_data.c`.
>
> **Ibuki**: the existing `0x9BA8-0x9C6F` range's delta was corrected from
> −0x74D0 to −0x74CF — a single-constant fix, not a refactor. The span (200
> raw slots, `0x9BA8-0x9C6F`, mapping 1:1 onto the dense contiguous PS2 run
> `9945..10144`) is referenced by 85 distinct raw CGs across Ibuki's scripts.
> 65 of those are directly measurable (`cg_audit.json`) and unanimous at
> −0x74CF. The other 20 sit in five shape-divergent `yuca` scripts
> (`[14]`, `[15]`, `[37]`, `[38]`, `[43]`) where PS2 carries exactly one extra
> control cell per script that the naive shape check can't see past; dropping
> that one cell aligns every remaining control cell (`code`/`koc`/`ix`) and
> every remaining L-cell exactly, and every one of the 20 resolves to the
> same −0x74CF. So: 65 measured directly, 20 more resolved by that one-cell
> alignment, all 85 unanimous, and all land in Ibuki's own group (8). Class
> (c)-own-group for Ibuki: 408 → 0.
>
> **Urien**: added `{0x52DA-0x52E2, -0xC78}` plus ten discrete single-value
> rows for `0x52E3-0x52EC` (deltas −0xC6F..−0xC78, one per raw value, since
> the delta steps by one as the raw value increases and cannot be expressed
> as one range). Class (c)-own-group for Urien: 256 → 8.
>
> **Deliberately left unfixed: raw `0x52D9`, 8 cells (`yuca[8..15]` script
> index 8-15, cell 0).** This raw value is used in *two* different script
> contexts with two different correct deltas: `yuca[0..7]` (single-cell
> scripts) already resolves it correctly via the default delta −0xC60;
> `yuca[8..15]`'s first cell needs −0xC78. The per-raw-value remap model has
> no way to distinguish the two call sites — a range covering `0x52D9` was
> tried and confirmed (by re-running `cg_audit.py`) to fix the 8 `yuca[8..15]`
> cells while breaking the 8 `yuca[0..7]` cells that were already correct, a
> net wash. Left at the pre-existing baseline (8 cells broken, matching
> upstream/PS2-adjacent behavior before this change) rather than trade one
> set of broken cells for another. Not fixable without a context-aware model
> (see item E's option 2).

### E. The cross-bank cluster — 949 cells (needs a model decision)

The 13-character `yuca[68..75]` cluster and Makoto/Twelve/Q/Sean/Ken cannot be
expressed as "one delta per character" — the correct answer is *another
character's* delta. Two ways out:

1. **More ranges** — keep the current model, add narrow ranges per cross-bank
   block. Works, grows the table, stays whack-a-mole-shaped but is now
   *audit-driven* rather than crash-driven.
2. **Change the model** — allow a range to name a target group/bank rather than a
   raw delta, so a cross-bank reference is expressed as intent. Cleaner, larger
   change, and would need upstream buy-in to avoid divergence.

Recommend deciding this **with Artem** before writing code (§13).

> #### Status 2026-08-31: LANDED (option 1 — more ranges)
>
> Committed to `fix/arcade-cg-mapping` as `CgRemapRange` additions to
> `src/arcade/arcade_char_data.c`, derived programmatically from
> `cg_audit.json`'s measured `(raw, ps2)` pairs rather than the doc's
> hand-summarized "13 characters" figure. The measured data refines that
> summary: the literal `yuca[68..75]` raw `0x0CB4` slot is referenced by
> **12** characters (not 13) — Ryu included, 53 cells — of which **11** need a
> row: **8** (Dudley, Necro, Hugo, Ibuki, Elena, Oro, Yang, Urien) resolve via
> Ryu's own −0x1E0, and **3** (Ken, Sean, Akuma) resolve via their own
> distinct, character-specific deltas (0x2D40, 0x3160, 0x3B60) — still a
> single measured constant each, added as single-value rows. Ryu himself
> needs no row: his own `default_delta` is already −0x1E0. Makoto (521 cells,
> uniformly −0x1E0), Twelve (88, uniformly −0x600, the X.C.O.P.Y./Necro
> family), Q (29, uniformly −0x1E0), Sean's other 8 named cells (folded into
> one 8-row, uniform-+0x3160 family together with its `0xCB4` cell — same
> delta, same audit oracle), and Ken's named 3 cells (`0x1201`, −0x420) are
> all included. Every family's rows are maximal contiguous-raw runs sharing
> one measured delta — never a range that would sweep an unmeasured raw value
> (see doc §8.N's warning, applied identically here for Makoto/Twelve/Q).
>
> Class (c)-wrong-group: **902 → 0** for item E's own share (all of it — this
> closes item E entirely, using more-ranges rather than a model change; no
> target-bank concept was added, no code outside `src/arcade/` was touched).
> The commonly-quoted "949" figure is the *pre-item-K* count (`23326679`,
> before Remy's residual fix moved 6 cells out of this class); the correct
> post-K baseline (`a5bc6a5b`, the tree this item was measured against) is
> **943**, of which **41 are Remy's** and are item **N**'s to claim (§8.N: "41
> → 0"), not double-counted here. 943 − 41 = **902**, item E's actual share,
> confirmed by re-running `cg_audit.py` against both commits.
>
> This does not change `remap_cg_number`'s model or reach non-arcade code, so
> the model-change question in option 2 above is now moot for the measured
> cast; leaving this note for the record in case a future crash surfaces a
> cross-bank case this audit's oracle can't see (§11).

### F. Chun-Li's 72 blank-CG cells — verify intent, probably no-op

Confirm whether 3SX intentionally blanked those sprites. If intentional, mark
them excluded in the audit so they stop appearing as findings.

> #### Status 2026-08-31: INVESTIGATED, NO CHANGE
>
> All 72 cells are one raw CG (`0x5FEE`) across `saca[44..47]` (SA-I
> activation, `asstbl_lv_9900_g[0..3]`). Decoded both sides cell-by-cell:
> every field *except* `num` (`ctr`, `se`, `olc`, `att`, `hit`, `eff`,
> `eftype`) is byte-identical between the arcade ROM and the PS2 AFS at each
> of the 18 repeating cells per script (an `olc`-stepped hold sequence). Only
> `num` differs: arcade carries a real, in-bounds Chun-Li sprite
> (`0x5FEE` remaps to her own group 16); PS2's shipped data has `0` there
> (blank) at every one of those cells, on all four near-identical scripts.
>
> **This is not something 3SX added.** The arcade side is unmodified ROM
> bytes; `0x5FEE` reads straight out of the decrypted CPS3 image. The `0` is
> what PS2's own shipped `SF33RD.AFS` carries at that position — a property
> of the PS2 port's data, not of 3SX's adaptation. Since arcade balance's
> entire purpose is to draw arcade-accurate sprites where they differ from
> PS2 (§4.1), drawing Chun-Li's real ROM sprite here is the *correct* arcade
> behavior, not a defect — remapping it to `0` would be actively wrong
> (it would suppress a real, in-bounds, arcade-accurate sprite to match a
> PS2-only omission). **No code change.** Left as a documented, expected
> arcade-vs-PS2 divergence; `cg_audit.py` was not modified to exclude it
> (out of the constraints for this task), so it will keep appearing in future
> `cg_audit.py` runs as a `c_mismatch_own_group` finding for CHUNLI — that is
> expected, not a regression.

### G. Over-declared section sizes (§7.6, **corrected by §19.7**, **CLOSED by §27**)

> **Status 2026-09-06: CLOSED, no code change.** §27 adjudicates both halves:
> no engine path forms a cell index in any tail (measured over every writer of
> `cg_ix`, both `cg_audit.py` closures), and the hashed tails are a pure
> function of the one SHA-pinned ROM, hence identical for every peer. The
> "tighten the sizes" change below is deliberately **not** made: it would move
> the digest for no behavioural gain. `cg_audit.py` re-derives the verdict on
> every run (`slack:` column, `span_reach` records).

Tighten the declared sizes to real extents. **There are 106 over-declared script
spans, not seven** (§19.7 has the measured table; 12 exceed 0x100, and `ELENA
exca` — listed in §7.6 — is not one of them). Note this **changes
`ArcadeCharData_ComputeDigest`**, which is carried in the MIST handshake — so it
is a peer-compatibility-breaking change and must ship on both sides together.

**Extended by the second pass:** the same class exists outside the script
tables. Remy's `caua` is declared `0x1848` (6,216 B) against a real 56 B, and
his `hosa` `0x1D68` (7,528 B) against a real 96 B — 13.6 KB of unrelated ROM
hashed into the digest and installed as two arrays 111× and 78× longer than the
data (§15.6). Ibuki's `stxy` is 4 B *shorter* than PS2's. Fold these into the
same change.

### H. RICT's four dead opponent slots per group — decide, then document (§15.4)

The arcade rival-catch table has **24** opponent slots per group; 3SX can only
select 20 of them (`catch_table_offset`, `charset.c:2658-2664`, with
`CHAR_3SX_TO_ARCADE` capping at arcade id 20). Arcade slots **15 (Shin Akuma),
21, 22 and 23 are unreachable** in this port — 7,244 of the 27,584 parsed RICT
elements. They are not a bug; they are dead weight in the digest and a trap for
anyone who diffs the section naively. At minimum add a comment at
`section_element_sizes[CHAR_DATA_RICT]` / `read_catch_table` recording the
`[group][24]` layout. **Do not "fix" the size mismatch by trimming** — `cg_rival`
is stored as a multiple of 24 and the stride is load-bearing.

### I. The 122 ATIT / 9 HIIT / 4 BODA / 55 RICT balance deltas — verify intent (§15.3-§15.5)

These are not adaptation defects, they are the arcade-vs-PS2 balance difference,
and they are exactly what upstream issue **#325** is about. But nobody has
confirmed the arcade side is the *intended* side for each. The highest-value
subset: **115 of the 122 ATIT differences are a single bit** — `0x40` in
`att.level`, i.e. `jump_att_flag` (`charset.c:2946`). One bit, 115 attacks,
17 characters, and it changes how each attack is classified. Worth a targeted
check against a frame-data source or §11.4 before assuming it is right.

### J. Publish the audits alongside each other

`tools/arcade-audit/data_audit.py` is the §15 tooling. It should run in whatever
CI or pre-release check `cg_audit.py` ends up in, with the assertion "zero
`ARCADE_ONLY` bounds verdicts" — that is the invariant it protects.
`tools/arcade-audit/residual_audit.py` (§17) belongs in the same gate, with two
assertions: **`residual < 0` == 0** and **`residual >= offset-table length` ==
0**. The second was **6** at the pre-fix baseline (§17.3) and required item
**K** to land first; with K's range applied it reads **0** (landed `a5bc6a5b`, as of
2026-08-30). **This gate covers the script-cell residual only.**
`residual_audit.py` runs a separate check over the OVCT `parts_char` path
(§17.3's "OVCT path" paragraph) that is **not** part of either assertion above
and currently reports **6** (Elena parts 85-90, item **B**, §18) — still open.
A gate on "zero script-cell residual violations" would pass today without
that door being shut.

---

### K. Remy's six residual-OOB cells — the second-door crash (§17.3) — DO FIRST

Six cells (`nmca[48]` ×3, `exca[30]`, `exca[37]`, `exca[38]`) pass raw CG
**1537** through the negative clamp into **Gill's** group, 103 past the last
valid index of his 1,435-entry offset table, yielding a pointer 5.8 MB past the end of
the allocation. Reachable whenever Remy and Gill are in the same match — the
arcade-ladder final fight, or a local-versus/training pairing
(`sel_pl.c:314-321`, `sys_sub.c:1710-1735`, `next_cpu.c:1116`, `:1490`).

The measured correct translation is **Alex's `+32`** (`1537 + 32 = 1569`, which
is exactly what PS2 stores for all six counterpart cells). Shape: a
`CgRemapRange` on `remy_cg_ranges` covering 1537 (`0x0601`) with delta `+0x20`,
then re-run `residual_audit.py` and require `residual >= offset-table length`
to drop to **0**. Note this is a **cross-bank** reference (§7.3(i) / §8.E), so
if the model decision in §8.E lands first, express it that way instead of as a
bare delta.

**`0x0601` is not the whole Alex-bank story.** 19 further raw CGs (38 cells)
measure the same `+0x20` delta and are left unfixed, deliberately — see item
**N**.

> #### Status 2026-08-30: LANDED
>
> Committed to `fix/arcade-cg-mapping` as an edit to
> `src/arcade/arcade_char_data.c` adding exactly that range to
> `remy_cg_ranges` — `{ .first = 0x0601, .last = 0x0601, .delta = 0x20 }` —
> with a comment recording the derivation, plus a `_Static_assert` binding
> `remap_cg_number`'s `CG_REMAP_CUTOFF` to stay `<= 0x601` — this is the only
> range in the file below `0x7070`, and the cutoff has already moved once
> (`0x800` → `0x400`, `ae309dc4`), so a second move could silently disable the
> row without either audit script catching it at run time. Observed with the
> edit in the tree:
>
> ```
> cells walked      : 133901
> in bounds         : 133610
> residual < 0                    : 0
> residual >= offset-table length : 0
> ```
>
> So: the six residual-OOB violations (§17.3) → 0, and `in bounds` gains
> exactly those six cells (133604 → 133610).
>
> **Committed, unmerged (corrects an earlier self-contradiction in this
> block).** This edit is commit `a5bc6a5b` on `fix/arcade-cg-mapping` — it is
> committed and on that branch, contrary to an earlier draft's "not
> committed, not on any branch" in this same spot. It is not merged to
> `main`/`mister` and not run in-game; it has been compiled for the host target
> (`cmake --build build/host`) but not for a device target. §20's
> "APPARENTLY UNREACHABLE" verdict (below) is unchanged by the fix — it bears
> on priority/severity, not on whether the fix is correct.

> #### Reachability, from the trigger analysis (§20)
>
> All six cells are **APPARENTLY UNREACHABLE** — which lowers the *observed*
> risk of this item, not its priority (the fix is one table row, and the verdict
> is a static argument, not a bound — §20.5).
>
> - **`nmca[48]`** is the wall-jump ("sankaku tobi") kick-off, written only at
>   `plpnm.c:1057` and gated by `DIP_WALL_JUMP_DISABLED` (`pls01.c:243`), which
>   `sysdir_base_move[]` sets for **every character except Chun-Li**
>   (`sysdir.c:38-47`) and which nothing ever clears. Twelve's X.C.O.P.Y.
>   inherits the *opponent's* flag (`effk7.c:71-72`), so it opens no door.
>   Corroborated by the data: the slot is a 3-cell stub in all 20 characters and
>   only Chun-Li's holds two distinct sprites.
> - **`exca[30]`, `[37]`, `[38]`** are entered only by script operands, and no
>   control cell in Remy's ten tables names them. 17 of the other 19 characters
>   *do* jump there from `cbca`; Remy, Gill and Makoto do not.
>
> So §8.J's "residual >= offset-table length must be 0" gate is still worth
> having, and item **L**'s guard is what actually makes the class safe.

### L. Guard the residual, not just `obj_group_table` (extends item C)

Item **C** proposes a bounds check at the nine `obj_group_table[n]` sites. §17
shows that is only half the guard: `n -= texgrpdat[i].num_of_1st` followed by
`((u32*)trans_table)[n]` needs its own check against
`*(u32*)trans_table / 4` — the renderer already computes that value in
`mlt_obj_melt2` (`mtrans.c:2533`) and simply never uses it as a bound anywhere
else. Adding it converts every future instance of this class into a logged
sprite drop, and it costs one load the code is already doing.

### M. Gill's 114 unbacked CG slots (§17.2)

`obj_group_table` assigns cg **1435..1548** to group 1, but Gill's texture file
carries only 1,435 offset entries. No cell in the cast — Gill's included —
references that window except item **K**'s six. This is a property of two
shipped PS2 tables, not of arcade balance, so **do not "fix" it**; record it,
and let the guard in item **L** cover it. Worth reporting upstream alongside
#363 since it is upstream's data too.

### N. Remy's other 38 Alex-bank cells — not fixed, deliberately out of scope (extends §8.K)

Item **K** fixes exactly one raw CG (1537 / `0x0601`, six cells, all
**APPARENTLY UNREACHABLE** per §20.4). A wider sweep of Remy's own ten tables
for cells that measure the same **+0x20** (Alex's) delta against their PS2
counterpart finds **19 further distinct raw CGs, 38 cells, spanning
`0x0655`-`0x0744`** — all in **ordinary, reachable normal-move animations**,
not placeholder stubs:

| Raw CG | Cells | Table / script |
|---|---|---|
| `0x0655`-`0x065C` | 1 each (8) | `nmca[46]` |
| `0x0669` | 5 | `nmca[33..37]` |
| `0x0676`, `0x0678` | 3 each (6) | `nmca[38]`, `nmca[39]`, `nmca[50]` |
| `0x067C`, `0x067D` | 1 each (2) | `nmca[40]` |
| `0x0683`, `0x0684` | 2 each (4) | `nmca[41]`, `nmca[42]` |
| `0x0690`-`0x0692` | 4 each (12) | `nmca[38]`, `nmca[39]`, `nmca[40]`, `nmca[50]` |
| `0x0744` | 1 | `cuca[35]` |

All 38 measure PS2 = raw + 32 (verified against `cg_audit.json`'s `REMY`
violations, `cls == c_mismatch_other_group`). **`0x0C01` (`nmca[49]`, 3 cells)
measures `-0x1E0` instead** — a different, non-Alex delta — and must **not**
be swept into the same range.

**This is correct but deliberately out of scope.** This work was scoped to
crash-and-desync items only (§8.A and §8.K). These 38 cells are class (c) —
wrong sprite, not a crash — and unlike item K's six cells, ordinary play very
likely reaches them (they sit in `nmca`, not in an operand-only-entered
`exca`/`cbca` slot with zero script references — see §20.3's method).
Widening `remy_cg_ranges` here would be exactly the scope creep already
pushed back on; it is recorded so it is not silently lost, not implemented.

**Caution for whoever picks this up: a blind range widen is unsafe.** A
further 20 raw values (4 singles plus the two runs below) sit in or near this
span with **no cell-aligned PS2 counterpart** to measure a delta from —
`dmca[3]`, `dmca[90]` and `dmca[91]` decode a different `cgd` (6 vs PS2's 4)
and a different cell count (12 vs 11/14) from their PS2 counterparts, so
`cg_audit.py`'s shape check fails and no comparison is made for any cell in
those three scripts:

| Raw CG (arcade) | Script |
|---|---|
| `0x0636` | `dmca[91]` cell 9 |
| `0x0679`, `0x067A` | `dmca[3]` cells 6-8 |
| `0x0685` | `dmca[3]` cell 4 |
| `0x0827`-`0x0834` | `dmca[90]` cells 3-9, `dmca[91]` cells 2-8 |
| `0x08D6`-`0x08D7` | `dmca[90]` cells 1-2, `dmca[91]` cell 1 |

(Verified: `dmca[3]` is `cgd=6`/12 cells vs PS2's `cgd=4`/11; `dmca[90]` is
`cgd=4`/12 vs PS2's 11; `dmca[91]` is `cgd=4`/12 vs PS2's 14.) The safe forms
are **discrete rows over the 19 measured values above**, or **one row with the
interpolation stated explicitly** (and verified against a wider cell-aligned
sample first) — not a bare `{first, last}` spanning `0x0636`-`0x0744`, which
would silently remap those six unmeasured values too.

> #### Status 2026-08-31: LANDED
>
> The scoping decision above predates this task; a later, wider pass (doc
> §8, standing balance-gating requirement) explicitly brought item N back
> in scope alongside D, E and F. Committed to `fix/arcade-cg-mapping` as
> **discrete rows** on `remy_cg_ranges` — exactly the "safe forms" this
> section called for, not the unsafe interpolated span: eight rows covering
> the 19 measured raw values (`0x0655-0x065C`, `0x0669`, `0x0676`, `0x0678`,
> `0x067C-0x067D`, `0x0683-0x0684`, `0x0690-0x0692`, `0x0744`), all
> `delta = +0x20`, plus a separate single-value row for `0x0C01` at
> `delta = -0x1E0` — kept as its own row per this section's explicit warning
> not to merge it with the Alex-bank family. The 20 unmeasured raw values
> listed above (`dmca[3]/[90]/[91]`'s shape-divergent cells) were left
> untouched — no range covers them, by construction (the rows above are
> discrete, not a span). Class (c)-wrong-group for Remy: 41 → 0.

### O. Items A, K, D, E and N's fixes change the netplay balance digest — release-note this

All five items' ranges change `cg_maps[]` (not just A and K — every
`CgRemapRange` addition or edit does, D/E/N included), and `remap_cg_number`'s
output is written straight into the parsed script buffer
(`arcade_char_data.c` -> `read_char_table`'s
`cg_number = remap_cg_number(...)` assignment) that
`ArcadeCharData_ComputeDigest()` (`arcade_char_data.c` -> `ArcadeCharData_ComputeDigest`)
later hashes span-by-span →
`ArcadeBalance_GetDigest()` (`arcade_balance.c:152`) →
`mist_handshake_set_balance_digest()` (`netplay.c:1305`) → compared against
the peer's digest at `mist_handshake.c:364`, which rejects the pairing with
`MIST_REJECT_BALANCE_MISMATCH` on any mismatch (`mist_handshake.c:372`, error
string at `:395`). This is the same mechanism item **G** already flagged for
the (not yet applied) over-declared-span fix — "a peer-compatibility-breaking
change [that] must ship on both sides together" — and it applies here too, now
that A, K, D, E and N are all landed: **every peer on a build without these
five items' ranges becomes unpairable with every peer on a build with them**,
silently, at handshake time, with no other symptom. No pinned digest constant
or test vector exists in the tree (checked: no reference to a specific digest
value anywhere in `src/netplay/` or `src/test/`), so nothing catches this at
build time. Worth a release note when any of A/K/D/E/N ship, and worth
considering whether item **G**'s and these five fixes' digest changes should
all be bundled into one compatibility bump rather than landing separately —
they already are one uncommitted change as of this pass, so in practice they
will ship together.

**Item Q joins this set (landed 2026-09-02):** the `cg_se` remap is applied in
the same `read_char_table()` parse the digest hashes, so it too changes the
digest — observed `e294f59fb707e518` → `de7a005eef378cab` (§8.Q's status
block). Rollback-safe per §21.13 (no `0x800` code in the set, RNG consumption
unchanged); include it in the same compatibility release note.

### P. Necro/Hugo/Yun/Akuma's remaining 9 own-group cells — 7 are the same ambiguity as Urien's `0x52D9`; Akuma's other 2 are not

§3 previously called these "explicitly out of scope"; that is wrong. Measured
against `cg_audit.json` (all cells referencing the raw, not just the currently
mismatching ones), each of these raw CGs is required by *different cells* to
resolve to *two different deltas*, exactly the failure mode item **D**
documents for Urien's `0x52D9`:

```
YUN   0x129B {-1052:1, -1056:3}    NECRO 0x1E5F {-1536:2, -1560:1}
HUGO  0x26C9 {-1824:5, -1822:1}    AKUMA 0x5440 {-3232:3, -3231:1}
AKUMA 0x5441 {-3232:4, -3233:1}    AKUMA 0x546A {-3274:1, -3273:1}
```

`AKUMA 0x546B` (`{-3266:2}` plus 2 more cells with no cell-aligned PS2
counterpart) is a different problem — a **single, unanimous** measured delta
(`-3266`, over the 2 cell-aligned cells) blocked only by the no-oracle rule
(§11), not an ambiguity — and is not counted in the six-raw table above.
**But it is counted in this section's own "9" title-count**: the two
`0x546B` cells are own-group-mismatched (`cg_audit.json`'s AKUMA `(c)og`
column is 6, decomposing as `0x5440`(1) + `0x5441`(1) + `0x546A`(2) +
`0x546B`(2)), so of this section's 9 cells, **7 are genuinely ambiguous
and 2 (`0x546B`) are not** — verified against `cg_audit.py`'s own output.
Do not read this section's title or the sentence above as meaning all 9 are
the same failure mode as Urien's `0x52D9`.

**Policy inconsistency, flagged rather than resolved.** The no-oracle rule
(§11) is applied strictly here: `0x546B`'s unanimous `-3266` delta is
*measured*, on 2 cell-aligned cells, and is still left unfixed because 2
more of its cells have no cell-aligned PS2 counterpart to confirm the
pattern holds for them too. Item **A** (§8.A) does not apply that rule the
same way: of the 41 distinct raw CG values in Elena's `0x9CFC-0x9D24` block,
only a subset have a cell-aligned PS2 counterpart (`dmca[82,83,86,87]`,
`btca[15]`) — the `exca[58..65]` run has **no** cell-aligned oracle at all
(§8.A's own text) — yet the whole 41-value contiguous range was applied on
the strength of "all 41 land in group 9," not a per-value measurement. Both
decisions are recorded as intentional (item A: committed, `23326679`; item
P: deliberately left unfixed); this section states the inconsistency between
them rather than hiding it. Neither decision is changed here.

The blocker is structural, not a missing range: `remap_cg_number` maps one
raw CG to one delta regardless of which script cell asked, so it cannot
express "this raw needs delta X from script A and delta Y from script B." A
range covering these values would necessarily break whichever side it didn't
target — the same trade item D found and declined for `0x52D9`.

In 5 of these 6 ambiguous raws the character's current `default_delta`
already happens to match the majority-measured delta, so the majority of
cells are already correct and only the minority are wrong (`YUN`: 3 correct /
1 wrong; `NECRO`: 2/1; `HUGO`: 5/1; `AKUMA 0x5440`: 3/1; `AKUMA 0x5441`: 4/1).
`AKUMA 0x546A` is the exception — it splits 1-1 with no majority, so both of
its cells are wrong under the current mapping and neither delta can be
preferred over the other by range-table means.

**Not fixable without a context-aware remap model** (the same option item
**E** declined to build, since the per-raw-value model covered the measured
cast without it). Recorded here so a future pass doesn't spend time writing a
range row for these raws — it cannot work by construction. If the model ever
gains a context-aware mode (script/cell-qualified rather than raw-CG-only),
these 9 cells plus Urien's 8 (§8.D) are exactly its test cases.

### Q. The six `cg_se` sound divergences — 29 cells, 6 per-character pairs (§21)

**The second byte-passed namespace.** `cg_se` is listed as byte-passed in §4.4
and had never been diffed by any audit. Six arcade codes resolve to the wrong
TSB note under arcade balance; **two of them (Alex, Oro) resolve to empty slots
and are currently silent.** Full derivation, method and negative results in §21.

**Fix:** a per-character exception remap in `arcade_char_data.c` ->
`read_char_table()`, beside `remap_cg_number()`, on the **upper 12 bits** of
`cg_se` (low nibble is flip/priority — preserve it):

| Char | arcade → PS2 | cells |
|---|---|---|
| MAKOTO | `0x27E` → `0x1DF` | 4 |
| ALEX | `0x2FB` → `0x3BF` | 4 |
| ORO | `0x2F8` → `0x25C` | 13 |
| ORO | `0x3DF` → `0x25D` | 2 |
| YANG | `0x1DF` → `0x29E` | 5 |
| AKUMA | `0x37E` → `0x130` | 1 |

**Must be per-character.** `0x2FB` is a legitimate Urien voice and `0x3DF` a
legitimate Twelve voice; a global code→code table breaks them (§21.8).

**Do not "simplify" this by reducing mod 32 and dropping pairs** — the two
mod-32-equivalent pairs (Yang `0x27F`, Yun `0x268`) are *already* excluded, and
Yang `0x269`, Q's `0x108` and the shoto `dmca[3]` SE are deliberate
non-actions with recorded reasons (§21.9, §21.10). Re-deriving the set without
reading §21 will produce a different, wrong list.

**Digest:** changes the balance digest. Rollback-safe (no `0x800` in the set) —
release-note under §8.O. Rationale in §21.13.

> #### Status 2026-09-02: LANDED
>
> Exactly the shape specified: `CgSeRemapPair` / `cg_se_maps[]` tables in
> `src/arcade/arcade_char_data.c`, applied by `remap_cg_se()` inside
> `read_char_table()` beside `remap_cg_number()`, on the upper 12 bits only
> (the flip/priority nibble is reattached). Two assertions hold it:
>
> - `src/test/test_cg_se_remap.c` (`--test-cg-se-remap`, auto-discovered by
>   `tools/gates/run-gates.sh`): the six pairs across all 16 nibble values, a
>   full 20×0x1000 (character, code) domain sweep proving exactly six
>   mappings exist (so Urien `0x2FB` / Twelve `0x3DF` and every §21.9
>   non-action pass through), and no chaining (Makoto's target `0x1DF` is
>   Yang's source). Verified red-able: corrupting one pair turned it red.
> - `tools/arcade-audit/cg_se_audit.py`: parses `cg_se_maps[]` from source
>   and counts the touched cells against the regenerated `rom.bin`. Observed:
>
>   ```
>   MAKOTO 0x27E->0x1DF cells  4/4   ALEX 0x2FB->0x3BF cells  4/4
>   ORO    0x2F8->0x25C cells 13/13  ORO  0x3DF->0x25D cells  2/2
>   YANG   0x1DF->0x29E cells  5/5   AKUMA 0x37E->0x130 cells 1/1
>   TOTAL script cells 29 (expected 29), distinct buffer positions 17 (expected 17)
>   ```
>
>   The 29 script cells dedupe to **17 distinct u16 slots** in the parsed
>   buffers because duplicate script offsets share cell bodies (Alex's
>   `yuca[8]/[10]/[12]/[14]` are four table entries onto one body) — both
>   denominators are asserted.
>
> Digest observed to change, as §21.13 requires: `e294f59fb707e518`
> (pre-change build at `90bc598d`) → `de7a005eef378cab`, both captured from
> the boot log's "Arcade balance auto-selected" line against the same
> verified romset. Not merged to `mister`, not on-device verified.

### R. Dudley's dangling OVCT next-index — arcade entry 177 → 178, past a 178-entry table (§24.6(i)) — CLOSED 2026-09-06 (§25)

Found by the §24 reachability sweep, **arcade-only** (PS2 control: none).
Dudley's arcade OVCT has 178 entries; entry 177 is `{timer 250, parts_char 0,
parts_nix 178}` — byte-identical to PS2's entry 177 — but the PS2 table has
two more entries, `178 = {sprite 5058, nix 178}` and `179 = {sprite 5059, nix
179}` (both group 5, Dudley's own, `colcd 13, mts 1`, self-looping). On the
arcade table `nix 178` points one element past the `SDL_malloc(location.size)`
buffer `read_ovct` returns, so `get_new_parts_data` would build the part from
**heap bytes** and hand `parts_char` to the renderer unremapped. Reachable in
principle: the walk seeds are 82 (`saca[28..31]`, olc 39) and 130
(`saca[48..51]`, olc 40), the chain from 130 is 47 one-frame steps to entry
177, then 250 more frames there. The scripts hold those `olc` values for 33
and 18 frames respectively (hit-stop extends the hold; the SA freeze does not,
`sa_stop_flag`), so it is **not observed and timing-gated** — the same class
§18 put Elena's tail in, and this time the timing argument is all there is.

Options, each a decision: (1) carry PS2's trailing entries when
`arcade_count < ps2_count` — needs a larger buffer than `location.size` and
moves the digest (§8.O); (2) clamp `parts_nix >= arcade_count` to `i` in
`Apply3SXRenderingConventions` — stationary at part 177, `parts_char 0`, never
drawn, whereas PS2 would show sprite 5058; also moves the digest; (3) a bounds
check in `eff01.c` -> `get_new_parts_data`, which touches a PS2 engine file
and must be gated. `cg_audit.py` prints the row as `walk>end[178](arcade-only)`
until one of them lands.

> #### Status 2026-09-06: CLOSED — unreachable, no code change, defended by the audit (§25)
>
> None of the three options was taken, on purpose. The walk from seed 130
> needs **297** consecutive effect frames of `olc 40`, from seed 82 **594** of
> `olc 39`. The master decodes cells only from `char_move`, which
> `check_hit_stop` withholds only while `hit_stop > 0`; every other freeze
> is either symmetric (`Game_pause`, `EXE_flag`), freezes the effect harder
> (`sa_stop_flag`, the opponent's flash) or installs a new script. Every
> writer of an attacker's positive `hit_stop` was enumerated (§25.3): own
> `hs_me` (Dudley max 12), parry (20), and an atemi's `hs_you` (Dudley 21,
> Remy 23 — the only two `comm_atmf` users in the cast); each needs a
> renewal cell, of which the `olc 40` run has 7 and the `olc 39` run 5.
> Holds are therefore ≤ 18 + 7 × 23 = **179** and ≤ 33 + 5 × 23 = **148**.
> The runs have no C cell and are bounded by `olc 0` cells on both sides,
> and both SA handlers call `char_move` every frame (§25.4). The "SA freeze"
> sentence above is corrected in §25.7. `cg_audit.py` ->
> `ovct_dangling_hold()` now re-derives the bound on every run; the row
> reads `walk>end-unreached[178:hold<=179/297]`.

### S. Twelve's 44 shape-divergent cells fall through a hole in `twelve_cg_ranges` — REPORTED, NOT FIXED (§29)

**The finding.** 44 live cells in Twelve's `dmca[3]`, `dmca[90]` and
`dmca[91]` carry raw `cg_number`s that our remap sends to a **different
texture group than the PS2 oracle measures for the same band**. Found by
§29's per-raw-value oracle; it is the only new divergence among the 316.

**The mechanism — a fitted table with unfitted gaps.** Twelve's raw CG space
splits into two clean, non-overlapping regions, measured over the 1,021
distinct raws his shape-ok scripts pin (zero of which measure two deltas):

| raw band | observations | delta | resulting group |
|---|---|---|---|
| `0x1E01`-`0x2095` | 40 | `-0x600` | **6**, all 40 |
| `0x6C01`-`0x706F` | 969 | `-0xB80` (his `default_delta`) | **19** (own), all 969 |

**No default-delta observation falls anywhere inside `0x1E01..0x2095`** — the
bands do not interleave. `twelve_cg_ranges` (`arcade_char_data.c`) carries 17
`-0x600` rows, and those rows cover **exactly the 40 measured values and
nothing else**: `0x1E01`, `0x1E21-24`, `0x1E26-27`, `0x1E3B-3D`, `0x1E40-43`,
`0x1E68-69`, `0x1E71-72`, `0x1E76-7D`, `0x1FCC-CD`, `0x1FD7-D8`, `0x1FDA-DC`,
`0x2033`, `0x2035-36`, `0x203C`, `0x2074`, `0x2090`, `0x2095`. The table was
fitted to the values the cell-index diff could see — which is precisely the
"discrete rows over measured values" method item **N** landed for Remy — so
every in-band raw that appears **only** in a shape-mismatched script was never
measured, never covered, and falls through to the `-0xB80` default. That is
the other band's delta.

**The affected raws and where they land:**

| script | cells | raw | ours | oracle |
|---|---|---|---|---|
| `dmca[3]` | 4 | `0x1E67`, `0x1E6B`, `0x1E6C` | `-0xB80` -> group 4 | `-0x600` -> group 6 |
| `dmca[90]` | 21 | `0x1E25`, `0x1E28`, `0x2010`-`0x2019` | `-0xB80` -> group 4/5 | `-0x600` -> group 6 |
| `dmca[91]` | 19 | `0x1E25`, `0x1E28`, `0x2005`-`0x201A` | `-0xB80` -> group 4/5 | `-0x600` -> group 6 |

The brackets are tight where it matters: `0x1E25` sits between `0x1E24` and
`0x1E26`, **both directly observed at `-0x600`**, and `0x1E28` between
`0x1E27` and `0x1E3B`, likewise. `dmca[3]` is the clearest single view — its
cells alternate group 6 (raws covered by the `0x1E76-7D` row) and group 4
(raws `0x1E67`/`0x1E6B`/`0x1E6C`, which fell through), frame by frame, inside
one animation. A damage animation does not change texture group between
consecutive frames.

**Adaptation defect, not §6.1.** The PS2 counterparts of these three scripts
draw `0x60F8`-`0x61EE` / `0x6290`-`0x629A` / `0x60A6`-`0x60AE`, all group 19.
The two sides are **not** identical, so §6.1's "pre-existing property of
shipping PS2 data" exemption does not apply. The wrong value is produced by
`remap()`, which is ours.

**Why every existing check missed it.** Class (a) needs the remapped index out
of `obj_group_table`'s bounds — it is in bounds. Class (b) needs group 0 —
groups 4, 5 and 6 are real. Class (c) is the check that would have caught it,
and it is exactly the one `shape_ok` skips for these three scripts. Twelve's
`(c)wg`/`(c)og` columns read 0 for that reason, not because the cells are
clean.

**Not fixed here, deliberately.** This item is reported so a fix can be scoped
on its own; §29 was an audit pass, and changing `cg_maps[]` moves the netplay
balance digest (§8.O). Two things a fix must settle first, neither answered
here:

1. **Range shape.** The safe form is the one §8.N argued for — discrete rows
   over bracketed values, or one row `0x1E01`-`0x2095` with the interpolation
   stated. The band is measured uniform end to end, which is the stronger case
   §8.N did not have, but the 16 still-unmeasured Remy raws are the standing
   reminder that a bare span is the unsafe default.
2. **Reachability in play.** Nothing in Twelve's own ten tables jumps to
   `(koc=1, ix=90/91)`; `dmca[90]`/`[91]` are entered by the damage-selection
   chain (§20.1), so they are live under the fail-open convention but were not
   shown to be reached in a real match.

**Gate.** `cg_audit.py` -> `manu_delta_gate()` emits these 44 as
`manu_cg_delta_divergent` rows and the report reads
`manu:8/11(d6+b2+z0) DIVERGENT(3 scripts,44 cells)!` on Twelve's line. A fix
that widens `twelve_cg_ranges` correctly drives that to zero with no edit to
this section.

---

## 9. Tooling (in-repo and verified working)

**Location:** `tools/arcade-audit/` on branch `fix/arcade-cg-mapping`. See the
README there for the short version.

Re-verified after the move: `python3 tools/arcade-audit/cg_audit.py` reproduces
`TOTAL | 66 0 949 745 316` and `cells audited: 133901` identically.

| File | Purpose |
|---|---|
| `cg_audit.py` | **The main deliverable.** Full 20-character audit of the 10 script tables' CG numbers. Parses constants from repo source at run time. Writes `cg_audit.json`. ~40 s. |
| `data_audit.py` | **The §15 deliverable.** The other 13 sections (STXY MVXY SERND RICT HIIT BODA HANA CATA CAUA ATTA HOSA ATIT PROT), all 20 characters: content diff + bounds analysis. Imports `cg_audit.py` for the shared constants (and does not modify it); derives element sizes by **compiling a `sizeof` probe against `include/structs.h`**, so a struct change moves the audit automatically. Writes `data_audit.json`. **~1 s** (no `obj_group_table` decode of its own beyond the import). |
| `data_audit.json` | Every §15 finding, machine-readable (256 KB) |
| `residual_audit.py` | **The §17 deliverable.** Derives every texture group's offset-table length from `SF33RD.AFS`, bounds-checks the residual `n -= texgrpdat[i].num_of_1st` for all 133,901 cells plus the OVCT `parts_char` path, and derives the group-load reachability model from `ldreq_tbl[]`/`ldreq_ix[]`. Imports `cg_audit.py` and `data_audit.py`; modifies neither. Writes `residual_audit.json`. **~1 s** (`/usr/bin/time -p`: real 1.10). |
| `residual_audit.json` | Every §17 finding, machine-readable (group table, violations, reachability census) |
| `counterfactual.py` | Re-runs the audit with historical fixes reverted → `cg_counterfactual.json` |
| `decrypt.py` | Rebuilds `rom.bin` from `sfiii3nr1.zip` (prints SIMM SHA-256s for verification) |
| `afs.py` | AFS container parser (magic `AFS\0`, 1535 entries) |
| `parse.py`, `scan.py`, `cgscan.py` | Arcade-side script decoders / early sweeps |
| `ps2scan.py`, `cmpovct.py`, `fulldiff.py` | PS2-side decode, OVCT compare, full arcade-vs-PS2 script diff |
| `rom.bin` | Decrypted 8 MiB CPS3 image (sha256 starts `c15743e350011f6a…`). **Gitignored** — rebuild with `decrypt.py` |
| `cg_audit.json` | Every violation, machine-readable (493 KB) |
| `audit_summary.txt`, `audit_run.txt` | Human-readable audit output |
| `denjin_oobcount.log` | The 19 OOB hits from the ASan run |

**Paths resolve automatically.** The repo root is derived from the script's own
location, so the audit reads the source tree of whichever worktree it sits in —
important, since the whole point is auditing *this* branch's tables. Override
with env vars when needed:

| Var | Default |
|---|---|
| `ARCADE_AUDIT_REPO` | repo root, derived from `tools/arcade-audit/../..` |
| `ARCADE_AUDIT_AFS` | `~/Library/Application Support/CrowdedStreet/3S-ARM/resources/SF33RD.AFS` |
| `ARCADE_AUDIT_ROM` | `rom.bin` beside the scripts |
| `ARCADE_AUDIT_ROMZIP` | `~/Library/Application Support/CrowdedStreet/3S-ARM/resources/sfiii3nr1.zip` |

**Asset locations (verified):**
- PS2 data: `~/Library/Application Support/CrowdedStreet/3S-ARM/resources/SF33RD.AFS`
  — 642,492,416 bytes, md5 `cc788f2ba398c7e464736f4b6d00bc82`.
  **Note the `3SX/` copy is a DANGLING SYMLINK** to a non-existent
  `/Users/sb/Developer/3sx-ios/SF33RD.AFS` — do not use it.
- CPS3 ROM: same `resources/` dir as `sfiii3nr1.zip`; also
  `/Users/sb/Developer/fbneo-replay-runner/roms/sfiii3nr1.zip` (what `decrypt.py`
  points at).
- Crash replay: re-downloadable from the issue-#363 attachment URL (§5.1).

Also in-repo and related: `tools/compare_char_data.py` — upstream's own
"compare CharInitData between the arcade version and the port" tool.

---

## 10. Reproducing the crash (exact recipe)

Two corrections to assumptions that cost time the first round:

- `THREESX_STATCHECK` is a **cmake option, not a build type**
  (`CMakeLists.txt:22`, `docs/building.md:57-70`).
- `CMAKE_BUILD_TYPE=Debug` **does not build** against the local prebuilt
  GekkoNet — Debug defines `NETPLAY_ENABLED` and fails with
  `error: use of undeclared identifier 'GekkoReplayFinished'`. Use
  `RelWithDebInfo`.

```bash
# 1. upstream source without network
git -C /Users/sb/Developer/3sx-mister archive upstream/main | tar -x -C <workdir>/src-upstream
ln -sfn /Users/sb/Developer/3sx-mister/third_party <workdir>/src-upstream/third_party

# 2. build statcheck + ASan
CC=clang CXX=clang++ cmake -S src-upstream -B src-upstream/build-statcheck-asan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTHREESX_STATCHECK=ON \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O1 -g -DNDEBUG -fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g -DNDEBUG -fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build src-upstream/build-statcheck-asan --parallel 10

# 3. run (pref dir ISOLATED — see below)
CFFIXED_USER_HOME=<workdir>/fakehome \
  src-upstream/build-statcheck-asan/3SX.app/Contents/MacOS/3SX \
  --ram-archive <path>/game_2.scrd --headless
```

**Pref-dir isolation matters.** `Paths_GetPrefPath()` is
`SDL_GetPrefPath("CrowdedStreet","3SX")` (`src/port/paths.c:5-16`). `HOME=` does
**not** move it; **`CFFIXED_USER_HOME=` does**. Put a config containing
`arcade-balance = true` in
`<fakehome>/Library/Application Support/CrowdedStreet/3SX/config`, and symlink
`resources/SF33RD.AFS` + `resources/sfiii3nr1.zip` into that tree. This keeps the
real `~/Library/Application Support/CrowdedStreet/3SX/` untouched.

Invocation contract from `tools/statcheck_runner.py:157`:
`[exe, "--ram-archive", <game_N.scrd>, "--headless"]`. **There is no game-index
argument** — `game_2.scrd` *is* the single game; `ReplayGame_Init`
(`src/test/replay_game.c:26-69`) finds the game-start frame itself.

To prove arcade balance was actually on, breakpoint `ArcadeCharData_Init` and
read `target variable is_enabled` (upstream: the static at
`arcade_balance.c:8`, set from `Config_GetBool(CFG_ARCADE_BALANCE)` at `:12`).

---

## 11. Blind spots — what static auditing cannot find

### 11.1 The three tiers

1. **Crash class (a)** — fully statically discoverable from the arcade ROM +
   `cg_maps[]` alone. **Closed** by `cg_audit.py`. **Its second door — the
   residual index — needs the PS2 AFS as well** (the offset-table lengths live
   there), and is closed by `residual_audit.py` (§17).
2. **Wrong-sprite class (c)** — statically discoverable *given the PS2 AFS as
   oracle*, wherever scripts are cell-aligned. **Enumerated** (1,694).
3. **State/shape divergence** — **not** statically discoverable. No table-bounds
   oracle exists.

### 11.2 The tier-3 residue

- **316 shape-mismatched scripts** (arcade vs PS2 cell counts differ) have no
  automatic verdict. They are enumerated in `cg_audit.json` (`manu` column) and
  are exactly where tier-3 bugs live.

  > **Superseded for the `cg_number` question by §29 (2026-09-06).** "No
  > automatic verdict" was true only of the *cell-index* diff. The adaptation
  > is a pure function of the raw `cg_number`, so the verdict does not need the
  > pairing, and `cg_audit.py` -> `manu_delta_gate()` now produces one for all
  > 316 on every run: 285 confirmed clean, 20 with nothing to adjudicate, 4
  > unresolved-and-named, **7 carrying a real divergence** — 44 of whose cells
  > (Twelve, §8.S) were never recorded anywhere. The sentence was right that
  > this is where tier-3 bugs live. It was wrong that they could not be found.
  > The claim still stands for `cg_se`/`cg_zoom`/`cg_effect` ordering and for
  > state divergence, which §29 does not address.
- **162 more scripts, 2,441 L-cells, have no oracle at all — a class the
  audit didn't even count until this pass.** `cg_audit.py`'s `audit()` sets
  `pcells = None` when the arcade table has more scripts than the PS2 table
  (`si >= pn`), and the pre-existing `needs_manual` counter only increments
  when `pcells is not None` — so a script index past the end of the PS2
  offset table was **neither compared nor counted anywhere**, not even in
  `manu`. The PS2 tables genuinely terminate early here (real
  `0x00000000` terminator words, not truncation — see §19 on truncation).
  Measured (`cg_audit.py`'s new `extra` column): **162 scripts, 2,441
  L-cells** — YANG 1,524, GILL 570, ELENA 200, YUN 93, HUGO 33, ORO 21.
  **This is an accounting gap, not crash exposure**: the crash-door checks
  (class (a), the residual door) still cover these cells — they are inside
  the 133,901-cell census and `residual_audit.py` walks all of it — so no
  OOB door was silently open. What was silently open is the count: the
  wrong-sprite class was never "closed" against these 2,441 cells, because
  nothing ever looked at them. **So the accurate framing of the wrong-sprite
  class is: closed relative to the oracle's reach** (89 remaining `(c)og`
  cells, §3), **with 89 remainders + 316 `manu` + 162 extra-script outside
  that reach** — not "closed" without qualification.
- **A concrete example found on the Denjin path itself:** `cbca[19..23]` — the
  five scripts `uja7` lands on after the projectile is released — differ:

  | | arcade | PS2 |
  |---|---|---|
  | cell 0 | `ctr=16, cancel=0, cg_effect=0` | `ctr=4, cancel=64, cg_effect=31, cg_eftype=1` |
  | cell 1 | `back` | `ctr=12, cg_effect=0` |
  | cell 2 | — | `back` |

  PS2 splits the 16-frame release freeze into 4+12 and fires
  `effinitjptbl[31] = setup_meoshi_hit_flag`; the arcade script fires no effect.
  Both `cg_effect` values are in range, so this is a **behavioural** divergence,
  not an OOB. Unresolved whether it is a port bug or a real CPS3-vs-PS2
  difference.
- **Two Denjin-unique engine mechanisms** that no data sweep can validate:
  `Att_DENJINHADOUKEN` calls `char_move()` up to **5 extra times per tick**
  (`plpat02.c:40-49` — no other move does this), and it is the only Ryu move
  using the `cmj7` reserved-jump slot (`comm_rja7`/`comm_uja7`,
  `charset.c:943-955`), which is consumed with no validation of the stored
  koc/ix/pat.
- **Hard-coded C constants that mirror data layout** are a related hazard and are
  grep-able: `now_koc == 8 && char_index == 13` (`plpat02.c:37`,
  `com_sub.c:300`) hard-codes Denjin's `cbca` slot in C.

### 11.3 The systematic net for tier 3

Run the ASan statcheck build over a **large replay corpus** (`tools/fcade-replays`
bulk download → `fbneo-replay-runner` → SCRD → ASan statcheck). Per-frame RAM
equality is the oracle; ASan removes the layout-luck masking documented in §5.5.

**A coverage counter would convert unknown-unknowns into a number:** the audit
already enumerates all 133,901 cells, so counting executed (character, table,
script, cell) tuples at the `setupCharTableData`/`char_move` choke point and
diffing against that universe yields exactly which cells have never been
exercised. That set *is* the residual risk, and it also tells you which
characters/moves to go fetch replays for.

### 11.4 NEW CAPABILITY (2026-08-30): a real-hardware ground-truth oracle exists

§11.1-§11.3 were written assuming the only oracle for tier 3 was "our engine vs
our engine, plus the PS2 AFS". **That is no longer true.** A separate
investigation built **frame-exact input injection into real FBNeo plus per-frame
CPS3 main-RAM diffing**, and used it to settle a question §11.3 would have
called unanswerable. Primary source:
`~/Desktop/3sx-makoto-1f-link-2026-08-29.md` **§18** ("THE REAL ARCADE,
MEASURED"); tooling and captured dumps at `/Volumes/KimchDrive/makoto-t-a/`
(`tools/`, `d2/game_0/`, `in_*.bin`, `sweep.sh`, `sweep2.sh`, `sweep3.sh`).

**What it does.** `fbneo-replay-runner`
(`/Users/sb/Developer/fbneo-replay-runner`, prebuilt
`build/release/fbneosdlarm64`) replays a Fightcade savestate plus a
10-bytes-per-frame input stream, and `-dump-ram-path` dumps CPS3 main RAM every
frame. A replay's `inputs` file is a flat per-frame record array, so the first N
records (menus + character select) are kept and everything after is replaced
with a scripted sequence — giving **frame-exact input control on the real ROM**.
Record layout verified in the runner's source: p1 = LE u16 at bytes 0-1, p2 at
bytes 5-6 (`src/burner/sdl/run.cpp:862-864`); bit 1 start, 2 up, 3 down, 4 left,
5 right, 6-11 = `fire 1..6` (`run.cpp:729-748`), with `fire 1..6` = LP/MP/HP/
LK/MK/HK (`src/burn/drv/cps3/d_cps3.cpp:40-45`). The dumped region is `RamMain`,
0x80000 bytes, SH-2-mapped at 0x02000000
(`src/burn/drv/cps3/cps3run.cpp:1245`), byte-normalised to big-endian by the
runner. A full 700-frame run takes ~12 s. Known CPS3 field offsets are published
in-repo at `src/arcade/arcade_constants.h` (`PLW_OFFSET 0x68C6C`,
`PLW_SIZE 0x498`, …).

**Why it matters here.** This is a general oracle for arcade accuracy, and it
directly dissolves part of the §11.2 residue:

- ~~The **316 shape-mismatched scripts** can be adjudicated by running the move
  on real hardware and diffing per-frame RAM against our engine~~ — the
  technique already produced "zero differing cells from the hit frame onward"
  over a whole move (`makoto-1f-link` §18.4).

  > **WITHDRAWN for the `cg_number` question (§29.6).** CPS3 RAM reports
  > `cg_number` in **arcade** numbering, which the ROM already gives us exactly.
  > The open question was what our *remap* should turn that into, and its target
  > namespace is the PS2's — which real hardware cannot pin. The oracle could
  > not have answered this even in perfect working order, and separately it no
  > longer runs as recorded: the cited primary source
  > `~/Desktop/3sx-makoto-1f-link-2026-08-29.md` is gone (no `.md` file remains
  > on `~/Desktop`; the drive's `-BACKUP-before-s18.md` predates §18 and
  > contains zero occurrences of "REAL ARCADE"), and the three `sweep*.sh`
  > scripts resolve their input generators through a scratchpad path from a
  > dead session. The dumps, the `in_*.bin` streams and the prebuilt runner do
  > all still exist. §29 settled the item statically instead.
- **Class-(c) intent** becomes answerable: whether Chun-Li's 72 blank-CG cells
  (§7.3(iii)) are a deliberate 3SX edit or a defect is a question about what the
  arcade draws, and the arcade can now be asked.
- The same applies to §8.A's `exca[58..65]` run, which has no cell-aligned PS2
  oracle, and to §11.2's `cbca[19..23]` divergence.

> #### ⚠ THE TRAP THAT COMES WITH IT: `offsetof` on the decomp ≠ hardware
>
> **Do not read a CPS3 RAM dump using `offsetof` on our `WORK`/`PLW` structs.**
> Measured (`makoto-1f-link` §18.2): compiled 32-bit
> (`clang -target armv7-none-eabi`), the leading anchors all agree —
> `routine_no` 0x24, `hit_stop` 0x44, `xyz` 0x64, `mvxy` 0x7C, `vital_new` 0x9E,
> `curr_rca` 0x1FC, `cg_ix` 0x204, `cg_add_xy` 0x228, eight for eight — but from
> `dm_stop` onward the decomp runs **short**: `dm_stop` 0x306 vs 0x32E (−0x28),
> `sa_stop_flag` 0x3E4 vs 0x41C (−0x38), `do_not_move` 0x415 vs 0x455 (−0x40),
> and **`sizeof(PLW)` 0x444 in the decomp vs 0x498 on real CPS3**. The port
> dropped or reshaped fields inside `WORK`.
>
> Fields past the published anchors must be located **empirically, by
> behavioural fingerprint**. Worked example: `guard_flag` was found by scanning
> every byte offset in `PLW` for one that is 3 on the attacker mid-attack and 0
> after, and 0→3→0 on the defender across the hit — exactly two adjacent
> candidates, disambiguated by the fact that `old_gdflag` is a one-frame-delayed
> copy (`plmain.c:88`, `plmain2.c:75`) and so must transition later. Result:
> `PLW.guard_flag` at `PLW + 0x3D2`. Note the method used only the decomp's
> *field order*, never its *values*, so it is not circular.
>
> For a **CG/sprite** question the relevant field is `cg_ix` (0x204), which
> **is** in the agreeing prefix — so §7's questions are cheaper to ask than
> §15's would be.

---

## 12. Not verified (stated so nothing is mistaken for a finding)

- **Elena's pre-remap raw values for the class-(a) cells were not read directly**
  from the running process — the variable is optimized out at
  `arcade_char_data.c:87`. The raw values come from the ROM parse and the delta
  from arithmetic (`0x9502 = 0x9D22 − 0x820`). The OOB itself *was* observed.
- **Whether the 66 Elena cells all crash on the MiSTer.** Only the `dmca` shock
  path was observed faulting, on macOS, under ASan. `btca[15]` and `exca[58..65]`
  are OOB by measurement but were not executed in a run.
- **No on-device (MiSTer/ARM) reproduction was attempted.** All runtime evidence
  is macOS + ASan on upstream @ 513380f9.
- **Whether the audit's 1,694 class-(c) findings are all real defects.** Chun-Li's
  72 look intentional; the rest were not individually eyeballed.
- **Whether other characters have unreported class-(a) equivalents in data the
  audit cannot align.** The audit says no OOB is *producible*, which is stronger
  than "none observed" — but it rests on the parse being complete.
- **The `cbca[19..23]` divergence** (§11.2) — port bug or genuine version
  difference: unresolved.
- **Our fork's behaviour under the OOB** was reasoned from the trap sweep and the
  absent SIGSEGV handler, not observed. No `exit=139` was captured on device.

*Added by the second pass (2026-08-30), for §15/§16:*

- **Whether the arcade side is the *intended* side for any of the 190 balance
  differences in §15.** The audit proves what differs and that nothing is out of
  bounds. It does **not** prove the arcade values are the ones a player should
  get; that is a design question, and §11.4 is how to answer it empirically.
- **What `att.level` bit `0x40` (`jump_att_flag`) actually changes in play.**
  Verified: it is set from `att.level & 0x40` (`charset.c:2946`) and differs on
  115 attacks across 17 characters. Its downstream effect was **not** traced.
- **Whether the 300 `hit_ix_table` reads past the end of HIIT are ever
  executed.** They are produced by `saca[1]` and `saca[7]` cells whose
  `cg_att_ix`/`cg_hit_ix` are **byte-identical in PS2**, so they are a hazard the
  shipped PS2 build carries too — but reachability was not established for
  either build (§15.7).
- **The 92 bounds hits that sit after a script terminator** (§15.7) are read as
  decoder artefacts because 100% of them are after one and 0% of pre-terminator
  cells produce an out-of-range index. That is strong, but it is inference from
  a distribution, not a proof that the region is unreachable: a jump targeting a
  later `pat` could enter it. Unresolved.
- ~~**Whether Remy's `caua`/`hosa` over-declared spans (§15.6) overlap another
  character's real data.**~~ **CLOSED by §19.1**: the 500 declared spans tile the
  ROM with **zero overlaps**, so they sit inside gaps. (No provenance was traced
  for the excess bytes themselves — that part remains unknown.)
- **`sizeof(PLW)` on real CPS3 (0x498)** and the `guard_flag` offset in §11.4
  are quoted from `~/Desktop/3sx-makoto-1f-link-2026-08-29.md` §18.2. They were
  **not independently re-measured** during this pass.
- **Whether upstream would accept the RICT 24-slot framing.** §15.4 is our
  reading of `catch_table_offset`; upstream has never documented it.

*Added by the third pass (2026-08-30), for §17-§19:*

- **Whether Remy's `nmca[48]` / `exca[30,37,38]` are ever executed** (§17.5).
  No `jmp`/`jpss`/`jsr` cell inside Remy's own ten script tables targets them
  (checked, 0 hits). Script entry is `set_char_move_init2(wk, koc, index, ip,
  scf)` (`charset.c:153`), whose `index` also arrives from C call sites; **no
  exhaustive enumeration of callers that could pass `(0, 48)` or
  `(7, 30|37|38)` was done.** The placeholder shape — `nmca[48]` is the same
  3-cell `(8, 0, 255)` stub in all 20 characters, and 18 of them point it at
  their own first sprite — is suggestive, not decisive.
- **Why `obj_group_table` assigns 114 more CG slots to group 1 than Gill's
  texture file backs** (§17.2). Measured, not explained.
- **Whether the six residual violations actually SIGSEGV on the MiSTer.** Same
  gap as §12's existing entry for Elena's 66: the OOB is proven by measurement,
  the fault was not observed in a run on any target.
- **Whether Elena's OVCT drift walk is truly unreachable** (§18.3). The
  argument is a quantitative margin — 255 accumulated frames per step against a
  5-frame longest run — not a structural bound. Hit-stop frames do accumulate.
  Not proven impossible.
- **The `old_cgnum + *ptr` arithmetic** (`eff61.c:275`, `effa8.c:275`) is not
  covered by any audit (§17.6). `old_cgnum` is set from a live `cg_number` at
  `effe8.c:110` and `effj0.c:50`, and from a local at `eff61.c:245` /
  `effa8.c:225`. Only the latter two feed the two arithmetic sites *as written*;
  whether any control flow can reach them with a propagated value was **not**
  traced.
- **What `IBUKI atca`'s unreferenced 376-byte script is for** (§19.6). It is
  well-formed, terminated, present in PS2 identically, and every CG it emits is
  in Ibuki's own group and in bounds — but no pointer references it, and nothing
  explains why it is there.
- **Two spans rest on indirect evidence** (§19.5): HIIT has no exact-fit index
  witness of its own, and Ken's OVCT does not match PS2 at any shift. Both are
  pinned by their neighbours at gap 0; neither is pinned by its own content.
- **`mvxy` and `prot` have no index-consumer bound.** Their extents rest on
  neighbour pinning plus exact length parity with PS2 (20/20 for both); their
  consumers were not traced to a maximum index.
- **Whether `exdm_ix_data`'s second subscript is meant to be the character
  rather than `player_number`** (§18.6(iii)). The 1:1 mapping of its 20 rows'
  `cg_number` values onto groups 1..20 is measured; that the subscript is a
  port bug is *inference*, and was not traced to the CPS3 original.

*Added by the trigger analysis (2026-08-30), for §20:*

- **The "apparently unreachable" verdicts for `ELENA exca[58..65]`,
  `REMY nmca[48]` and `REMY exca[30,37,38]` are static, not proofs.** They rest
  on (a) an exhaustive scan of every control-cell operand in all 20 characters ×
  10 script tables, reading all three operand slots as `koc`/`ix`/`pat`
  regardless of opcode, and (b) an exhaustive enumeration of the 830
  `set_char_move_init`/`_init2` call sites. A `char_index` corrupted or
  mis-restored by rollback, or a `cm*` register reused across an unexpected
  path, would bypass both. **None of the three was executed in a run.**
- **How long `pat_status >= 32` persists on Elena.** The `dmca[86..89]` door
  needs it. Her only producers are two `comm_sps` cells (`btca[18]`, `btca[33]`);
  the restore at `charset.c:201-203` only fires for `scf != 0` entries, and what
  `set_char_move_init` (`charset.c:74`) leaves `pat_status` at on the CPS3 path
  was **not** traced. So `dmca[86..89]` is "conditional" on an unmeasured window.
- **`dm17_to_nm23_change[]` (`plpdm.c:78`) is indexed into `char_table[now_koc]`,
  not into a fixed table** (`plpdm.c:655`, air-recovery). Its per-character values
  exceed several tables' entry counts — e.g. Yun 103 and Yang 100 against 98
  `dmca` entries; Remy 51 against 51 `nmca` entries. Whether `now_koc` can be 0
  or 1 at that point was **not** determined. This is engine source common to both
  builds, so it is not an adaptation defect, but it is unaudited.
- **Which `dm_attlv` values are actually producible.** Every electric ATT record
  in the shipped data carries `att.level & 7` of **1 or 2**, so only `dmca[83]`,
  `[84]` (and `[87]`, `[88]`) have a measured producer; `dmca[82]`, `[85]`,
  `[86]`, `[89]` have none. But `dm_attlv` is also copied around by effects
  (`effk2.c:678`, `effk3.c:137`, `effc2.c:760`) and that was not traced, so this
  is **not** a reachability claim for those four scripts.
- **`_ef13_char_table` script 224 and `tama_data[86]`** both carry electric ATT
  records but were not traced to a spawner (§20.2 lists the other nine). They may
  be reached from inside another `eff13` script; not checked.

---

## 13. Upstream coordination

- Issue **#363** is open, milestone 1.0, assigned to nobody, zero comments.
- Artem said in Discord (2026-08-22): *"I don't have the tools to properly fix
  denjin right now so I'm taking some time to better understand the engine."*
  **The tooling in §9 is precisely those tools** — it decodes and diffs both
  datasets and enumerates every violation in the cast.
- Our fork carries the **identical defect** (same `remap_cg_number`, same Elena
  ranges) on `upstream-engine-fixes`. Any fix should be shaped so it can go
  upstream rather than diverge — especially the §8.E model decision.
- Worth telling upstream regardless of who fixes it: the bug is in the **victim's
  `dmca`**, not Ryu's data; there are **three** affected sites, not one; and the
  crash is **layout-dependent**, so a non-ASan replay runner can pass a replay
  that is reading out of bounds (§5.5).

---

## 14. Suggested next steps

1. **Fix Elena's 66 cells** (§8.A) — one range, delta −0x6F42, then re-run
   `cg_audit.py` and require class (a) = 0. **And fix Remy's 6** (§8.K) — one
   range, delta +0x20, then re-run `residual_audit.py` and require the residual
   violation count = 0. They are the same defect class through two different
   doors, and neither is fixed by the other. **Status, updated 2026-08-31:
   both ranges are committed to `fix/arcade-cg-mapping` (`23326679`,
   `a5bc6a5b`) and audit-verified — see the status blocks in §8.A and
   §8.K — but neither is merged to `main`/`mister` or verified on-device.**
   The remaining step is merging them and testing on-device (and reading
   item **O**'s netplay-digest note first).
2. **Add the bounds guard** (§8.C **and §8.L**) — converts the whole future
   class from layout-dependent crashes into logged sprite drops. The guard must
   cover **both** `obj_group_table[n]` and the residual index; guarding only the
   first would have caught Elena and missed Remy.
3. **Report findings on #363** (§13) — cheap, and prevents duplicate work
   upstream.
4. **Decide the model question** (§8.E) with Artem before touching the 949
   cross-bank cells.
5. **Land Ibuki + Urien** (§8.D) — mechanical, 664 cells, visible quality win.
6. **Wire the coverage counter** (§11.3) — turns the tier-3 blind spot into a
   measured number instead of an unknown.
7. ~~**Use the real-hardware oracle** (§11.4) on the 316 shape-mismatched
   scripts~~ and on §8.A's `exca[58..65]`. It exists, it is fast (~12 s per
   run), and it answers questions no amount of static diffing can. **Amended
   2026-09-06 (§29.6):** not for the 316 — that predicate is about the PS2-side
   index our remap targets, which CPS3 RAM cannot pin; §29 settled it
   statically instead. The `exca[58..65]` half stands, but budget for repair
   first: the rig's `sweep*.sh` no longer resolve their input generators, and
   the write-up they were derived from is gone.
8. **Take the `jump_att_flag` question to upstream #325** (§8.I) — one bit, 115
   attacks, and the single largest balance delta this repo has measured.
9. **Scope a fix for §8.S** — Twelve's 44 cells in `dmca[3]/[90]/[91]`, the one
   real divergence §29 turned up. Reported, deliberately not fixed in the audit
   pass; it moves the netplay balance digest (§8.O).

---

## 15. The other 13 sections (second pass, 2026-08-30) — upstream issue #325

§7 audited sprite indices in the 10 script tables. This section audits the
**13 sections a CG audit cannot see**, where a wrong hitbox, throw position or
attack property would neither crash nor corrupt a sprite:

`STXY MVXY SERND RICT HIIT BODA HANA CATA CAUA ATTA HOSA ATIT PROT`

(OVCT and OVIX were already covered in §7.5; the other 10 are the scripts.)

### 15.1 The framing correction that governs everything below

**PS2 is not an oracle for these sections.** `remap_cg_number` translates exactly
one value in the entire pipeline (§4.4), and `Apply3SXRenderingConventions`
touches exactly one section, OVCT (§4.5). All 13 sections here are installed
**raw** (`arcade_char_data.c:515-525`). So an arcade-vs-PS2 content difference
here **is the balance change** — it is what "arcade balance" means — not a port
defect.

What *would* be a defect, and what this audit therefore looks for:

| Class | Meaning |
|---|---|
| **structural misread** | wrong element size, or a layout the parser models wrongly |
| **bounds hazard** | an index the arcade data can produce that exceeds the arcade section's own length |
| **over-declared span** | `location_data[]` size past the real data — installed and **hashed into `ArcadeCharData_ComputeDigest`** |

The §6.1 discriminator still applies and is enforced in code: **a hazard the PS2
build also has, from a byte-identical cell, is not an adaptation defect.**

### 15.2 Section semantics — established from the consumers, not the names

Every row below was traced to the code that reads it. Element sizes come from
compiling `sizeof()` against `include/structs.h`, not from reading the struct by
eye. Bindings are `set_char_base_data`, `charid.c:97-110`.

| Section | `WORK` field | Element type / size | Indexed by | What it controls |
|---|---|---|---|---|
| **STXY** | `step_xy_table` (`charid.c:97`) | `s16`, 2 B; **used as pairs** | `cg_add_xy` (`charset.c:2676-2688`); `exec_char_asxy` uses `data*2` (`effect.c:439-451`, = `effinitjptbl[32]`, `effxx.c:298`) | per-frame position deltas — each s16 is `<<8` and added to `xyz[0]/[1].cal` |
| **MVXY** | `move_xy_table` (`:98`) | `s16`, 2 B; **engine stride 6 s16 = 12 B** | `add_to_mvxy_data(wk, ix)` at `ix*6` (`pls02.c:113-134`), `setup_mvxy_data` (`:149-152`) | velocity/acceleration: `a[0].sp, d[0].sp, kop[0], a[1].sp, d[1].sp, kop[1]` |
| **SERND** | `se_random_table` (`:99`) | record **0x24 B** = one `u32` offset + 16 `u16` (`read_sernd`, `arcade_char_data.c:346-370`) | `cg_se & 0x7FF`, only when bit `0x800` is set (`charset.c:2723-2727`, `:2891-2894`) | random sound-effect pick: `seAdrs[random_16()]` |
| **RICT** | `rival_catch_tbl` (`:102`) | `CatchTable`, 8 B | `cg_rival + catch_table_offset(tsukami_num)` (`charset.c:2736`, `:2900`; offset fn `:2658-2664`) | **held-character placement during throws** — `catch_nix` → `char_move_index` (`plpcu.c:108-112`), `catch_flip` (`:115`), `catch_hos_x/y` → the victim's position (`:118-123`), `catch_prio == 2` → victim drawn **behind** the holder, else in front (`:125-129`, `:170-174`) |
| **HIIT** | `hit_ix_table` (`:103`) | `UNK_0`, 16 B = 8 × `u16` | the packed `(cg_att_ix:cg_hit_ix)` word (`charset.c:2698-2704`), read at `:2743`, `:2905`, `:2999` | **the indirection hub.** Loads `cg_ja`, whose 8 fields are indices into the six box tables below |
| **BODA** | `body_adrs` (`:104`) | `UNK_1` = `s16 body_dm[4][4]`, 32 B | `cg_ja.boix` (`charset.c:2976`) | body / collision boxes (`h_bod`) |
| **HANA** | `hand_adrs` (`:105`) | `UNK_2` = `s16 hand_dm[4][4]`, 32 B | `cg_ja.bhix + cg_ja.haix` (`:2981`) | "hand" boxes (`h_han`) |
| **CATA** | `catch_adrs` (`:106`) | `UNK_3` = `s16 cat_box[4]`, 8 B | `cg_ja.caix` (`:2977`) | catch / grab box (`h_cat`) |
| **CAUA** | `caught_adrs` (`:107`) | `UNK_4` = `s16 cau_box[4]`, 8 B | `cg_ja.cuix` (`:2978`) | caught box (`h_cau`) |
| **ATTA** | `attack_adrs` (`:108`) | `UNK_5` = `s16 att_box[4][4]`, 32 B | `cg_ja.atix` (`:2979`) | **attack boxes** (`h_att`) |
| **HOSA** | `hosei_adrs` (`:109`) | `UNK_6` = `s16 hos_box[4]`, 8 B | `cg_ja.hoix` (`:2980`); also `[hoix+1]` (`effc2.c:879`) and a fixed `[1]` (`pls01.c:814`) | push / correction box |
| **ATIT** | `att_ix_table` (`:110`) | `UNK_7`, 16 B of `u8`/`s8` | `cg_att_ix >> 6` (`charset.c:2702`), read at `:2944` | **attack properties**: `reaction, level, mkh_ix, but_ix, dipsw, guard, dir, free, pow, impact, piyo, ng_type, hs_me, hs_you, hit_mark, dmg_mark` (`structs.h:105-122`) |
| **PROT** | *not bound in `set_char_base_data`* | `UNK_Data` = `s16 data[4][6]`, 48 B | published as `parabora_own_table[character_id] = dst->prot` (`texgroup.c:488`; decl `charid.c:12`), read **only** by `setup_butt_own_data` as `[dm_butt_type].data[weight_level]` (`pls02.c:164-170`) | **throw trajectory** per button type × weight class — fed to `read_adrs_store_mvxy` (`pls02.c:172-183`), i.e. the same 6-s16 record MVXY uses |

**Two decodes worth writing down, because both are non-obvious:**

1. **The `(att:hit)` word is a bitfield.** `charset.c:2698-2704` does
   ```c
   wk->cg_meoshi = wk->cg_hit_ix & 0x1FFF;
   st.w.h = wk->cg_att_ix;  st.w.l = wk->cg_hit_ix;   /* LoHi16 = {s16 l; s16 h;} */
   wk->cg_att_ix >>= 6;
   st.l *= 8;
   wk->cg_hit_ix = st.w.h & 0x1FF;
   ```
   With `combined = (cg_att_ix << 16) | cg_hit_ix`: bits **0-12** are
   `cg_meoshi`, bits **13-21** are the **HIIT index** (0-511), bits **22-31** are
   the **ATIT index** (signed; `set_new_attnum` negates a negative one before
   indexing, `charset.c:2927-2944`). A naive "cg_hit_ix indexes HIIT" reading is
   wrong and would mis-audit every cell.
2. **`att.level` is packed too** (`charset.c:2945-2949`): bit `0x80` →
   `zu_flag`, bit **`0x40` → `jump_att_flag`**, bits `0x30` → `at_attribute`,
   bit `0x08` → `no_death_attack`, bits `0x07` → the actual level. Likewise
   `att.guard` bits `0xC0` → `kezuri_pow` index, `0x3F` → guard value (`:2950-2951`).

### 15.3 Result — per-section verdict, all 20 characters

`python3 tools/arcade-audit/data_audit.py` (~1 s). Observed:

```
sect   identical differing chars_dif  chars_sz   verdict
stxy        3413         0         0         1   CONTENT-IDENTICAL, SPAN DIFFERS
mvxy        1571         0         0         0   IDENTICAL
sernd         25         0         0         0   IDENTICAL
rict       20885        55         3        20   DIFFERS
hiit        6911         9         5         8   DIFFERS
boda        4965         4         4         7   DIFFERS
hana        1815         0         0         0   IDENTICAL
cata         175         0         0         0   IDENTICAL
caua         275         0         0         1   CONTENT-IDENTICAL, SPAN DIFFERS
atta        1714         0         0         0   IDENTICAL
hosa         331         0         0         1   CONTENT-IDENTICAL, SPAN DIFFERS
atit        1958       122        17         0   DIFFERS
prot         688         0         0         0   IDENTICAL
```

| Section | Verdict | Detail |
|---|---|---|
| **STXY** | identical content | every common element equal; Ibuki's arcade span is **4 B shorter** than PS2's (852 vs 856) |
| **MVXY** | **identical** | 1,571/1,571 records, all 20 characters, byte for byte |
| **SERND** | **identical** | 25/25 records (after `read_sernd`'s rebasing of the leading `u32`s) |
| **RICT** | differs — **55 elements, 3 characters** | see §15.4; the naive count is 20,477 and is wrong |
| **HIIT** | differs — **9 entries, 5 characters** | see §15.5 |
| **BODA** | differs — **4 entries, 4 characters** | see §15.5; the counterpart of the HIIT diffs |
| **HANA** | **identical** | 1,815/1,815 |
| **CATA** | **identical** | 175/175 |
| **CAUA** | identical content | Remy's span is 111× over-declared (§15.6) |
| **ATTA** | **identical** | 1,714/1,714 — **no attack box differs anywhere in the cast** |
| **HOSA** | identical content | Remy's span is 78× over-declared (§15.6) |
| **ATIT** | differs — **122 entries, 17 characters** | see §15.5; the substantive finding |
| **PROT** | **identical** | 688/688 throw-trajectory records |

**Gameplay-visible difference count: 190 elements** (122 ATIT + 55 RICT + 9 HIIT
+ 4 BODA), spread over 18 of 20 characters. **Zero are adaptation defects** —
every one is a genuine arcade-vs-PS2 data difference, correctly carried through.
Severity is bounded: no attack box (ATTA), no hand box (HANA), no catch or
caught box (CATA/CAUA), no push box (HOSA), no movement table (STXY/MVXY/PROT)
and no sound table (SERND) differs at all.

### 15.4 RICT — a structural finding, and why a naive diff is a trap

A plain element-wise diff reports **every character differing**, arcade element
count exactly **1.2×** PS2's, and 20,477 mismatched elements. All of that is an
artefact.

`catch_table_offset` (`charset.c:2658-2664`) is the tell:

```c
if (ArcadeBalance_IsEnabled()) { return CHAR_3SX_TO_ARCADE(thrown_character) - 24; }
else                          { return thrown_character - 20; }
```

The subtracted constant **is the number of opponent slots per group**. RICT is
`[group][opponent]`, with **24 slots per group in the arcade table and 20 in
PS2's**, and `cg_rival` is stored as `(group+1) * slots`. Verified three ways:

1. `arcade_elems * 20 == ps2_elems * 24` for **all 20 characters**, and
   `arcade_elems / 24 == ps2_elems / 20` is an integer group count every time
   (Gill 32, Alex 133, Ryu 19, … Remy 14).
2. Mapping arcade slot `CHAR_3SX_TO_ARCADE(j)` ↔ PS2 slot `j` makes the sections
   agree on **20,885 of 20,940** mapped elements. Under the wrong model they
   disagree from byte 120 — element 15, exactly where `CHAR_3SX_TO_ARCADE`
   starts skipping (arcade 15 = Shin Akuma).
3. Every one of the **3,568** non-zero `cg_rival` values in the cast is a
   multiple of 24 and in range, except 31 cells that are all decoder artefacts
   (§15.7).

**The real difference is 55 elements in 3 characters**, classified as geometry
and draw-order:

| Character | Elements | Field hits | What |
|---|---|---|---|
| GILL | 48 | `catch_prio` 39, `catch_hos_x` 11, `catch_hos_y` 9 | groups 23 and 24: `catch_prio` 1 (arcade) vs 2 (PS2) against **all 20 opponents** — a uniform, deliberate-looking change of whether the held character draws in front of or behind Gill (`plpcu.c:125-129`). Plus groups 20-30 vs **Gill himself**: hold-position offsets differ (e.g. `hos_x` −41 vs −46) |
| NECRO | 6 | `catch_hos_y` 6 | group 9 vs Ryu/Dudley/Hugo/Ken/Sean/Akuma: `hos_y` 1 or −3 (arcade) vs 0 (PS2) |
| SEAN | 1 | `catch_hos_x` 1, `catch_hos_y` 1 | group 18 vs Oro: (91,122) arcade vs (96,58) PS2 |

`catch_nix` — the only field that indexes anything (`char_move_index`,
`plpcu.c:111`) — **never differs**. So none of these 55 can reach a wrong script.

**Four opponent slots per group are unreachable in this port.** Under arcade
balance the offset is `CHAR_3SX_TO_ARCADE(thrown) - 24` with `thrown ∈ 0..19`,
so arcade ids 0..20 are selectable and slots **15 (Shin Akuma), 21, 22, 23**
never are. Across the cast that is 1,047 groups × 4 slots = **4,188 of the
25,128 parsed RICT elements** (PS2's total is 20,940) that this port can never
reach, yet which are installed and hashed into the netplay digest. All of them
are non-zero, i.e. real data, not padding. See §8.H.

### 15.5 The content differences, classified

**(i) ATIT — 122 entries, 17 characters. The substantive finding.**

Field frequency across all 122: `level` **115**, `guard` 5, `dir` 1, `mkh_ix` 1.

> **All 115 `level` differences are exactly one bit: `0x40`.**
> `charset.c:2946` — `wk->jump_att_flag = wk->att.level & 0x40;`

That is 115 attacks across **15** characters where arcade and PS2 disagree on
whether the attack counts as a jump attack. It is by a wide margin the largest
balance delta measured in this repo. Distribution: Chun-Li 30, Ken 12, Yang 12,
Akuma 11, Dudley 8, Remy 8, Elena 6, Sean 6, Ryu 5, Urien 5, Oro 4, Alex 3,
Ibuki 3, Gill 1, Hugo 1. (Necro, Yun, Makoto, Q and Twelve have none; Makoto and
Twelve reach the 17-character count only through the non-`level` rows below.)

The other 7, spread over 5 characters:

| Character | Entry | Field | arcade → PS2 | Consequence |
|---|---|---|---|---|
| ALEX | 61 | `guard` | 198 → 246 | `kezuri_pow` index unchanged (both `>>6 == 3`), guard value `0x06` vs `0x36` (`charset.c:2950-2951`) |
| HUGO | 50 | `guard` | 56 → 24 | same shape |
| AKUMA | 56, 57 | `guard` | 120 → 88 | same shape |
| TWELVE | 94 | `guard` | 191 → 190 | `0x3F` vs `0x3E` |
| GILL | 23 | `dir` | 6 → 14 | knockback direction (`att.dir &= 0xF`, `charset.c:2953`) |
| MAKOTO | 60 | `mkh_ix` | 99 → 0 | index field; **not** traced to a consumer this pass — see §12 |

**(ii) HIIT + BODA — 13 entries, 5 characters. One coherent shape.**

| Character | HIIT entry | arcade → PS2 | Paired BODA entry |
|---|---|---|---|
| RYU | 49 | `boix` 0→44, `cuix` 0→3, `hoix` 0→3 | BODA[44] differs (a different box, not a null one) |
| KEN | 76 | `boix` 0→21, `cuix` 0→3, `hoix` 0→3 | BODA[21] all-zero in arcade, a full box in PS2 |
| SEAN | 137 | `boix` 0→19, `cuix` 0→3, `hoix` 0→3 | BODA[19] all-zero in arcade, a full box in PS2 |
| AKUMA | 131 | `boix` 0→82, `cuix` 0→3, `hoix` 0→3 | BODA[82] all-zero in arcade, a full box in PS2 |
| AKUMA | 90, 91, 92, 93 | `cuix` 0→1 | — |
| Q | 453 | `cuix` 3→1 | — |

The Ryu/Ken/Sean/Akuma cluster is the **shoto family**, and the PS2 body box is
literally the same 16 values in all four
(`{-14,22,98,16,-32,56,84,22,-34,62,58,24,-28,54,44,12}`). Read plainly: **on
one animation frame PS2 gives these four characters a body box and a
caught/push box where the arcade gives them none.** These are index fields
pointing into other sections — the dangerous kind — but the targets are in
bounds on both sides (§15.7), so the difference is behavioural, not a hazard.

**(iii) Nothing that looks like a 3SX content edit.** Unlike §7.3(iii)'s
Chun-Li blank-CG cells, none of the 190 differences here has the signature of a
port-side edit: no zeroed-out records, no wholesale table replacements, no
values outside their field's normal range. Every difference is a plausible
Capcom balance revision.

### 15.6 Over-declared spans (extends §7.6 beyond the script tables)

| Character | Section | Declared (arcade) | PS2 span | Excess |
|---|---|---|---|---|
| REMY | `caua` | `0x1848` = 6,216 B (777 elems) | 56 B (7 elems) | **6,160 B**, 111× |
| REMY | `hosa` | `0x1D68` = 7,528 B (941 elems) | 96 B (12 elems) | **7,432 B**, 78× |
| IBUKI | `stxy` | 852 B | 856 B | arcade **4 B short** |

In both Remy cases the **common prefix is byte-identical to PS2** — the real
data is right, the declared extent is not. Same consequences as §7.6:
unreachable at run time (indices never approach it), but `read_s16_array`
allocates and byte-swaps it, and **`ArcadeCharData_ComputeDigest` hashes it**
(`arcade_char_data.c:568-581`), so it is part of the netplay compatibility key.
Remy's layout in ROM (`caua` at `0x452538`, `hosa` at `0x441A30`) leaves the
declared extents inside gaps rather than overlapping a neighbouring section, so
`coalesce_adjacent_sections` is unaffected. Whether those extents run into
*another character's* data was not checked — see §12.

Every other span is exactly PS2's length. The characters whose HIIT is
**shorter** than PS2's are Yun, Dudley, Ibuki, Elena, Oro, Yang, Q and Remy
(8 characters; Ibuki by 3 entries, the rest by 1); whose BODA is shorter is the
same list **minus Dudley** (7 characters; Ibuki by 3, the rest by 1). In every
case the common prefix is byte-identical — the arcade genuinely has fewer
entries.

### 15.7 Bounds — no arcade-only hazard exists

Every index the arcade data can produce was checked against the arcade section's
own length, with the §6.1 discriminator applied per cell.

| Check | Result |
|---|---|
| `cg_att_ix >> 6` → **ATIT** | **0 violations.** The max index used is exactly `entries - 1` for all 20 characters (Gill 34/35, Alex 100/101, … Remy 78/79) — a perfect fit, and independent confirmation that the `>> 6` decode is right |
| `cg_ja.boix / bhix+haix / caix / cuix / atix / hoix` → **BODA/HANA/CATA/CAUA/ATTA/HOSA** | **0 violations across all 20 characters.** Every HIIT entry's six indices are ≤ `entries - 1` in the arcade tables. This is the §7.5 "unbounded `olc_ix_table`" hazard's analogue, and it does **not** reproduce here |
| `hosei_adrs[hoix + 1]` (`effc2.c:879`) | Reaches one past the max for 17 characters — but **PS2's tables are the same length**, so it is pre-existing, not an adaptation defect |
| packed word → **HIIT** | 300 hits over 16 characters, **all `pre_existing_in_ps2`** |
| `cg_add_xy` / `exec_char_asxy` → **STXY** | 24 hits: 2 pre-existing (Oro `nmca[4]`), 22 decoder artefacts |
| `cg_se & 0x7FF` → **SERND** | 70 hits, **all** decoder artefacts |
| `cg_rival` → **RICT** | 31 off-model values, **all** decoder artefacts |

**The 300 HIIT hits.** Every one comes from `saca[1]` and `saca[7]` — the same
two scripts in every character — carrying `cg_att_ix = 0x0038`,
`cg_hit_ix = 0x8000`, which decodes to HIIT index **452** while e.g. Gill has
216 entries. The PS2 cells are **byte-identical** (verified for Gill, Ryu,
Chun-Li and Remy: arcade `att=0x0038 hit=0x8000` vs PS2 `att=0x0038 hit=0x8000`,
differing only in `cg_number`, which is the remapped field). So this is a hazard
the shipped PS2 build carries too, exactly like §7.5's OVIX case: latent in both
datasets, not created by the adaptation. Characters with large HIIT tables
(Elena 465, Chun-Li 511, Q 489, Twelve 503) are in range and never flag.

**The 92 decoder artefacts.** These were nearly reported as findings.
`cg_audit.py`'s script walk stops at an unconditional control-transfer command
only for the *last* script of a table (`TERMINATORS`, `cg_audit.py`), so for
every other script it keeps decoding to the next script's start. Adding an
`after_terminator` flag to the walk resolves it completely:

```
    hiit_oob                 before-terminator  300 | after-terminator    0
    sernd_oob                before-terminator    0 | after-terminator   70
    stxy_oob                 before-terminator    2 | after-terminator   22
```

**100% of the SERND, RICT and out-of-range STXY hits are after a terminator;
0% of pre-terminator cells produce an out-of-range index into any section.** The
values themselves confirm it: Remy `saca[63]` yields `cg_rival` 0x8000/0xA000/
0xC000 and `cg_add_xy` 32768/35328 after the terminator, whereas **every**
pre-terminator `cg_add_xy` in the entire cast lies in 2..544 (544 being Oro's
single pre-existing case). `data_audit.py`
reports the split rather than hiding it — see §12 for why this is inference and
not proof.

### 15.8 Stretch: the second ROM revision (`sfiii3`, 990608)

> **Label corrected 2026-09-02.** This section previously called `sfiii3` the
> "990512" set. It is the other way round: **`sfiii3nr1` IS 990512** (the literal
> `"990512"` string appears in its decrypted SIMM1, and its CRCs match FBNeo's
> `SFIII3_990512_FLASH`), and **`sfiii3` is 990608**. The `+0x14C` shift measured
> below is correct and unaffected — only the two revision labels were swapped.
> Note also that the `sfiii3.zip` in `fbneo-replay-runner/roms/` carries MAME
> `sfiii3n` CRCs, i.e. the no-CD 990608 set, not a CD set.

All prior work used `sfiii3nr1` (the revision pinned by `rom_load.c:41-45`).
`/Users/sb/Developer/fbneo-replay-runner/roms/sfiii3.zip` is the merged
990608 set. Decrypting its SIMM1 with `decrypt.py` gives a valid image
(`strings -n 8` yields 6,670 runs against 6,672 for nr1, with the same leading
patterns — so the key is right for both).

**The char data is the same data, shifted by `+0x14C`.**

| Check | Result |
|---|---|
| non-script sections at `rev2_offset = rev1_offset + 0x14C` | **237 of 260 byte-identical** |
| script offset-table heads shifted by exactly `+0x14C` | **200 of 200** |
| script bodies (past the pointer table) at `+0x14C` | **193 of 200** |
| SERND after `read_sernd`'s rebasing | **20 of 20 identical** — the 20 apparent mismatches were only the embedded absolute pointers moving with the shift |

Real cross-revision content differences, after accounting for the shift:

- **NECRO `rict`** — 8 differing bytes of 13,056, from element 218 (`catch_hos_y`
  and `catch_prio` values 1/−3 in nr1 → 0 in 990512).
- **NECRO `atit`** — **1 byte**: entry 90, `hit_mark` 14 → 30.
- Seven script bodies: Ibuki `atca` (18 B), Elena `atca` (26 B), Akuma `caca`
  (1 B), Akuma `saca` (3 B), Makoto `cuca` (1 B), Remy `cuca` (1 B), Remy `yuca`
  (290 B).

**Consequence, and the safety net.** `location_data[]` is revision-specific:
with the current offsets, **not one section's offset table decodes on the 990512
image** (200 of 200 first-script pointers land outside their own section). But
`Rom_Load` matches by pinned SHA-256 (`rom_load.c:41-45`), and this flat 990512
zip's SIMM1 digests (`2cc58dcf…`, `01c72b44…`, `0f912ccc…`, `fe01877e…`) do not
match the pinned `0ddcfaa9…` / `7c039558…` / `30b5e727…` / `d9597fdc…`, so it is
rejected and the session falls back to PS2 balance. The comment at
`rom_load.c:33-34` — "the same bytes appear in the update_all merged set under
`sfiii3nar1/`" — is about a *merged* set that contains the nr1 slices in a
subdirectory; this particular zip is 990512-only and does not. **Report, don't
fix**, per the brief. Reproduce with:

```sh
ARCADE_AUDIT_ROMZIP=/Users/sb/Developer/fbneo-replay-runner/roms/sfiii3.zip \
ARCADE_AUDIT_ROM=/tmp/rom_sfiii3.bin python3 tools/arcade-audit/decrypt.py
```

---

## 16. Command data — CLOSED, and recorded so it is not re-opened

**This section exists to stop a specific piece of work from being redone.** A
previous investigation concluded that "arcade balance ships PS2 inputs wearing an
arcade label" and wrote an alarming brief on that basis. **It was wrong, and it
was disproved with direct evidence from the ROM.** Primary source, including all
the counting: `~/Desktop/3sx-arcade-command-tables-HANDOFF-2026-08-30.md`
(verified in this same worktree at `ebbd8645`).

**`src/arcade/arcade_cmd_data.c` is a byte-exact extraction of the CPS3 ROM's
command tables.** Structure in the decrypted image (CPS3 address =
`0x6000000 + offset`):

| Structure | ROM offset | Size |
|---|---|---|
| 21 × 56 pointer tables | `0x61381C`–`0x614A7C` | 4,704 B, **stride `0xE0`** |
| the 194 command `s16` arrays | `0x1997A0`–`0x19C026` | 10,374 B |
| `pl_cmd_num` group boundaries | `0x199650` | 336 B = 24 rows × 7 × `s16` |

Per-character table start is `0x61381C + char * 0xE0` in **arcade** numbering;
there is no master pointer array in the ROM, the SH-2 code computes the base
inline. Three independent proofs:

1. **Structural decode** — pointer-chasing all 21×56 slots and comparing
   element-wise against the compiled C tables: **1,176 slots identical, 0
   differing**.
2. **Perfect bijection** — 194 ROM addresses ↔ 194 C array names, one-to-one,
   zero ROM pointers mapping to more than one name. Decisive, because several
   `unk_cmd_NNN` arrays are *value-duplicates of each other* and the extractor
   still emitted them separately, exactly as the ROM stores them at distinct
   addresses — which a PS2-sourced or value-deduplicating extraction could not
   reproduce.
3. **Blind big-endian substring search** — all 194 arrays searched in `rom.bin`
   with no structural assumptions: **194 hits, 0 misses** (little-endian control:
   16/194). The 194 pointer targets tile `[0x1997A0, 0x19C026)` with zero gaps
   and zero overlap.

**Arcade vs PS2: `pl_cmd` differs in 0 of 1120 slots.** Input recognition under
arcade balance is arcade-accurate; every `reset[]` value, motion window and
button mask matches the ROM. `pl_cmd_num` likewise: the ROM's 24-row table maps
onto the C `pl_cmd_num[20][7]` under `CHAR_3SX_TO_ARCADE` with **0 mismatches**
(the C table is the ROM's table with the arcade-index-15 Gill row removed).

**`pl_CMD` — the `cmd_sel` "all super arts" alternate set — has no ROM
counterpart at all.** Two exhaustive sweeps found no second table set: zero BE
u32 pointers into the array region exist outside the `0x61381C` block, and no
other run of ≥20 pointer tables at stride `0xE0` exists in the 8 MiB image.
⇒ **`get_commands()` ignoring `cmd_sel` under arcade balance is correct**, not a
bug (`cmd_main.c:94-102`).

**The one real residual.** `cmd_data_set()` (`cmd_main.c:60-75`) adds
`blok_b_omake[omop_b_block_ix[cmd_id]]` to `reset[3,4,5,6,12]`.
`blok_b_omake[4] = {-2,0,2,4}` (`sysdir.c:61`) is a **PS2 System Direction
feature with no ROM counterpart**. At default settings it is a no-op
(`omop_b_block_ix[0]` resolves through `Dir_Default_Data.contents[0]`,
`dir_data.c:14`, to index 1 → `blok_b_omake[1] = 0`), but **if a user changes the
System Direction blocking option, arcade balance applies a PS2-only modifier to
five `reset[]` slots.** Small, real, and the only genuine fidelity gap in this
area. Decide whether to gate it under `ArcadeBalance_IsEnabled()`.

**Two traps recorded, because both have already bitten someone:**

- **`reset[]` is not the input-buffer duration.** It is the value latched into
  `waza_flag[i]` once a command is *already recognized* (`command_ok()`,
  `cmd_main.c:1529-1536`) — the post-recognition window. Per-step input leniency
  is each motion step's `w_int`. Tuning leniency off `reset[]` turns the wrong
  knob.
- **The arcade command table is 21 entries and needs `CHAR_3SX_TO_ARCADE`**
  (`constants.h:62`); PS2's 20-entry tables are indexed directly. Getting it
  wrong silently reads another character's commands. This is the same 21-vs-20
  asymmetry that §15.4 hits in RICT and §7 hits in the SA tables.

**Do not re-open:** "Q has no HCB+K" (it is slot 31 in both tables,
`arcade_cmd_data.c:502-503` / `:731-739`), and "the 647 `dm_cmd_xx` placeholder
slots mean missing moves" (the C table is byte-identical to the ROM, so those
are the arcade's own empty slots).

---

## 17. The second door: residual bounds (third pass, 2026-08-30)

§4.3 wrote the render path out as four steps and §6.1 audited only the first.
This section audits the second. It is the work behind
`tools/arcade-audit/residual_audit.py`.

### 17.1 Why a second door exists

```c
n = wk->cg_number;
i = obj_group_table[n];            /* (1) n >= 37664 -> OOB  — cg_audit.py checks this */
if (i == 0) return;                /*     gap -> clean skip                            */
if (texgrplds[i].ok == 0) return;  /*     group not loaded -> clean skip                */
n -= texgrpdat[i].num_of_1st;      /* (2) THE RESIDUAL — nothing checks it              */
trsbas = (u16*)(texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[n]);
count = *trsbas;                   /* (3) DEREFERENCE of whatever that produced         */
```

`cg_audit.py:370-379` flags step (1) only (`rm >= OGT_N`). A `cg_number` that
lands in a **wrong but valid** group therefore files as class (c) "wrong sprite
drawn" — but if the residual `n` is negative or past the end of *that* group's
offset table, step (3) dereferences a wild pointer. **Same fault as upstream
#363, different door.** The nine sites are the same nine listed in §4.3
(`mtrans.c:185`, `:284`, `:377`, `:426`, `:695`, `:818`, `:1099`, `:1227`,
`:1486`); `:377` (`getObjectHeight`) differs in that its `cgnum` is a `u16`, so
a *negative* residual there wraps to ≈65,500 rather than going negative.

Bounding this needs one number the audit never had: **the length of each
group's offset table.**

### 17.2 R1 — the missing bound, derived (method and result)

`mtrans.c:2533` computes it at run time:

```c
n = *(u32*)grplds->trans_table / 4;      /* mlt_obj_melt2 */
```

and `trans_table` is the base of the loaded file, set identically at all three
load sites — `texgroup.c:405` (`q_ldreq_texture_group`), `:567`
(`checkSelObjFileLoaded`), `:634` (`load_any_texture_grpnum`) — each
`lds->trans_table = ldadr`, where `ldadr` is the RAM address of AFS entry
`texgrpdat[grp].apfn`. So:

> **`offset_table_len(g) = LE_u32(AFS_entry[texgrpdat[g].apfn][0:4]) / 4`**

readable statically out of `SF33RD.AFS`. Three facts make the indexing
unambiguous, and all three are re-checked on every run:

1. **`texgrpdat` is indexed by group number on the render path.**
   `load_any_texture_grpnum` does `lds = &texgrplds[grp]; bsd = &texgrpdat[grp];`
   (`texgroup.c:626-627`), and `q_ldreq_texture_group` publishes a row into
   `texgrplds[obj_group_table[bsd->num_of_1st]]` (`texgroup.c:185-190`, with the
   `num_of_1st == 0` special case at `:184-185`). Resolving all 100 rows that
   way: **71 distinct groups**, and for every one of them
   `obj_group_table[num_of_1st] == the row index` (group 1 is the special case —
   its run starts at cg 1 because `obj_group_table[0]` is the universal gap).
2. **Aliased rows agree.** Seven groups are published by more than one
   `texgrpdat` row (group 27 by ten rows, group 23 by two — the language variant
   `checkSelObjFileLoaded` picks at `texgroup.c:550-555`). **Every alias set
   agrees on `table_len`**, so the bound is well-defined per group.
3. **The decoded array validates structurally.** For all 71 groups the array is
   strictly increasing, `offs[0] == 4 * len`, and every entry lies in
   `[4*len, texgrpdat[g].to_tex]` — i.e. between the end of the array and the
   start of the texture table. **PASS, 71/71.**

**Cross-check against `obj_group_table`.** Each group's run in
`obj_group_table` is contiguous (checked: 71/71) and starts at its own
`num_of_1st`. Comparing run length against `table_len`:

| | groups |
|---|---|
| `table_len == extent` (exact) | **68** |
| `table_len > extent` (spare entries) | 2 — group 23 (+2), group 52 (+1) |
| **`table_len < extent` (unbacked CG slots)** | **1 — group 1 (GILL)** |

> **Group 1 is short by 114.** `obj_group_table` assigns cg **1..1548** to
> Gill's group, but Gill's file (AFS entry 1460, 3,040,072 B) carries only
> **1,435** offset entries. **cg 1435..1548 are unbacked**: any of them reaches
> step (3) with `n` past the end of the array. This is a property of the two
> shipped PS2 tables — `obj_group_table` (`chren3rd.c:8`) and Gill's own texture
> file — not of arcade balance. It is only *dangerous* because arcade balance
> can now steer a `cg_number` into that window (§17.3).

Full 71-row table (group, apfn, num_of_1st, cg_hi, extent, table_len, owner) is
printed by the tool and stored in `residual_audit.json` under `groups`.

**A corollary worth stating, because it closes half the question outright:**
no group's `obj_group_table` run starts below its own `num_of_1st` (checked,
0/71). Therefore, for any value that survives the `i == 0` early-out,
`n = rm - texgrpdat[i].num_of_1st >= 0` — **a negative residual is
unreachable through `obj_group_table`.** The only way to get one is to go
through door (1) first, where `obj_group_table[rm]` is already an OOB read and
`i` is garbage. The `getObjectHeight` `u16`-wrap hazard is therefore also
unreachable except downstream of class (a).

### 17.3 R2 — the residual, bounds-checked for every cell

`residual_audit.py` re-walks all 20 characters × 10 script tables (the same
133,901 cells `cg_audit.py` counts, decoded through `data_audit._walk` so the
`cgd == 6` tail is included), and for each cell computes `rm = remap(raw)`,
`g = obj_group_table[rm]`, `n = rm - texgrpdat[g].num_of_1st`, and compares `n`
against `offset_table_len(g)`.

**Pre-fix baseline** (working tree at the time of writing — i.e. **with** the
Elena range of §8.A applied but **without** the Remy range of
§8.K, which had not yet been derived; class (a) reads 0 here and 66 without
the Elena range; the tool was run both ways and the six findings below are
identical either way):

```
  cells walked      : 133901
  in bounds         : 133604
  blank / table gap : 291        (all rm == 0; matches cg_audit's (b) = 0)
  obj_group_table OOB (class (a)) : 0     [66 with the Elena range reverted]
  residual < 0                    : 0
  residual >= offset-table length : 6
```

**Six violations. All Remy. All pre-terminator. None pre-existing in PS2.**
This is the finding that produced §8.K's fix. **Current status: FIXED,
the pre-fix baseline.** With the §8.K range also applied, `residual >= offset-table
length` reads **0** and `in bounds` reads 133610 (133604 + the 6 cells above);
every other figure in this baseline is unchanged. Re-run `residual_audit.py`
in the working tree to reproduce.

| Char | Table | Script | Cells | Raw CG | Remapped | Group | Residual `n` | Table len |
|---|---|---|---|---|---|---|---|---|
| REMY | `nmca` | 48 | 0,1,2 | 1537 | 1537 | **1 (Gill)** | **1537** | 1435 |
| REMY | `exca` | 30 | 0 | 1537 | 1537 | **1 (Gill)** | **1537** | 1435 |
| REMY | `exca` | 37 | 0 | 1537 | 1537 | **1 (Gill)** | **1537** | 1435 |
| REMY | `exca` | 38 | 0 | 1537 | 1537 | **1 (Gill)** | **1537** | 1435 |

**What the machine would actually do.** `((u32*)trans_table)[1537]` reads
`0x00870035` = 8,847,413 out of Gill's file — **5.8 MB past the end of the
3,040,072-byte allocation.** `trsbas` becomes `trans_table + 8847413` and
`count = *trsbas` dereferences it. That is the §5.5 shape exactly: on a target
where the address is unmapped it is a **SIGSEGV** (`exit=139`, no backtrace);
where it is mapped it is a garbage sprite count driving a garbage tile walk.

**Mechanism — this is §7.4's negative clamp, promoted to a crash.** Remy's
`default_delta` is **−0x0D00** (−3328) (`arcade_char_data.c`, `remy_cg_ranges`
/ `cg_maps[CHAR_REMY]`). `1537 + (−3328) = −1791 < 0`, so
`remap_cg_number` returns the value **unchanged** (`arcade_char_data.c:104-107`)
and 1537 goes to the renderer raw. `obj_group_table[1537] = 1` → Gill.
`1537 − 0 = 1537 ≥ 1435`. The doc has always described the negative clamp as
"producing wrong sprites rather than a fault" (§7.4). **That was wrong: it can
fault.**

**What the value actually is.** Raw 1537 (`0x0601`) is an **Alex** CG number.
PS2's counterpart cell holds **1569** for all six cells — `obj_group_table[1569]
= 2` (Alex), residual 1, in bounds. Alex's own `default_delta` is **+32**
(`cg_maps[CHAR_ALEX]`), and `1537 + 32 = 1569`. So the correct translation for
these six cells is *Alex's* delta, not Remy's — the §7.3(i) cross-bank family,
with a crash rather than a cosmetic consequence.

**These four scripts look like placeholder slots, in both datasets.**
`nmca[48]` decodes to the same 3-cell shape — cell types `8`, `0`, `255`, one
repeated CG — for **all 20 characters**, and in 18 of them that CG is the
character's own **first** sprite (residual 1 in their own group). Gill's is raw
1 → group 1 residual 1; Chun-Li's is the only other outlier (raw 24384/24385 →
own group 16, residual 1344/1345, in bounds). Remy's `nmca[47]` holds *his*
first sprite (raw 29185 → 25857 → group 20 residual 1) and his `nmca[48]` holds
**Alex's**. For `exca[30]`, `[37]` and `[38]` every other character has a real
3-to-33-cell animation; **Remy alone has a 1-cell stub**, again carrying Alex's
value. That is consistent with unused/placeholder slots seeded from Alex's
table, but it is **not proof they are never executed** — see §17.5 and §12.

**PS2 control.** The same walk over the shipped PS2 scripts produces **34**
hits, **0 of them pre-terminator** — all 34 are in Remy `saca[63]` past a
terminator, i.e. the decoder artefacts already characterised in §15.7. So by the
§6.1 discriminator the six arcade findings are **adaptation defects, not
pre-existing PS2 hazards.**

**OVCT path.** `residual_audit.py` also applies the same check to
`parts_char → cg_number` (`get_new_parts_data`, `eff01.c:169`), for the
post-adaptation table (PS2 values for `i < common_count`, raw CPS3 past it —
`arcade_char_data.c:670`, `:679-685`). Result: **6 violations, all Elena parts
85-90, all class (a) (`≥ 37664`), zero residual violations anywhere in the
cast**, and **0** on the PS2 control. That is the already-known §7.5 / §8.B
tail; the OVCT path introduces no *new* door.

### 17.4 R3 — the reachability model, derived from the loader tables

A residual violation only faults if `texgrplds[i].ok != 0`, i.e. if that group
is loaded. Group ownership is not guesswork — it is in `gd3rd.c`:

- `ldreq_tbl[294]` (`gd3rd.c:1024`) is `{type, ix, frre, kokey}`; **type 1 is
  `q_ldreq_texture_group`** (`ldreq_process[6]`, `gd3rd.c:1006`), and `ix` is a
  `texgrpdat` row.
- `ldreq_ix[43][2]` (`gd3rd.c:2791`) is `{start, count}` into it.
- `Push_LDREQ_Queue_Player(id, ix)` (`gd3rd.c:405-431`) walks
  `ldreq_ix[ix]`, called as `Push_LDREQ_Queue_Player(COM_id, My_char[COM_id])`
  (`next_cpu.c:182`, `:691`, `:1490`; also `win.c:178`, `ranking.c:330`), so
  **rows 0..19 are the 20 characters**.
- `Push_LDREQ_Queue_BG(ix)` (`gd3rd.c:434-437`) calls
  `Push_LDREQ_Queue_Union(ix + 20)`, so **rows 20..42 are the stage unions**.

Resolving every type-1 entry through `obj_group_table[texgrpdat[ix].num_of_1st]`
gives a complete ownership map (printed in full by the tool):

| Groups | Loaded when |
|---|---|
| 1..20 | the corresponding character is in the match (group = character + 1) |
| 27 | Gill, Ryu, Necro, Ibuki, Oro, Ken, Sean, Urien, Akuma, Chun-Li or Remy is in the match (own row plus alias rows 89-97) |
| 35 | **Gill only** |
| 33, 34, 42-59, 61, 83, 84 | per stage union (rows 20-42) |
| 21, 23, 25, 26, 30, 38, 62-82 | **no type-1 `ldreq_tbl` entry** — loaded, if at all, by other paths (e.g. `load_any_texture_patnum(0x7F30, …)` → group 38, `menu.c:280`, `win.c:80`, `effe6.c:1662-1665`; `checkSelObjFileLoaded` → group 23) |

**Verdict on the six findings: group 1 is Gill's, and Gill is reachable.**

- `Check_Use_Gill()` (`sel_pl.c:314-321`) sets
  `permission_player[PRESENT_MODE_LOCAL].ok[CHAR_GILL] = 1` and the same for
  both training modes — so **Gill is directly selectable in local versus and in
  training**. (It returns early for `MODE_NETWORK`, `sel_pl.c:315-317`, so Gill
  is *not* selectable in netplay.)
- `Initialize_EM_Candidate` (`sys_sub.c:1710-1735`) sets
  `EM_Candidate[PL_id][*][9] = 0` whenever `My_char[PL_id] != 0` — i.e.
  **the tenth arcade-ladder opponent is character 0, Gill**, for every player
  who is not Gill; `Setup_Next_Fighter` then does `My_char[COM_id] = EM_id`
  (`next_cpu.c:1116`) and `next_cpu.c:1490` issues the load.

So the condition is **"Remy in the match with Gill"** — the arcade-mode final
fight, or a local-versus / training pairing. It is not an exotic state.

**Ranking the class-(c) population by the same model.** The tool also censuses
where all 949 `c_mismatch_other_group` cells land:

| Landing group | Cells | Loaded when | Characters |
|---|---|---|---|
| **1 (Gill)** | **541** | Gill in the match | Makoto 293, Sean 31, Q 27, Hugo/Ibuki/Elena/Oro/Yang/Ken/Urien/Akuma 23 each, **Remy 6** |
| 3 (Ryu) | 233 | Ryu in the match | Makoto 228, Remy 3, Q 2 |
| 2 (Alex) | 87 | Alex in the match | Remy 38, Dudley 23, Necro 23, Ken 3 |
| 4 (Yun) | 60 | Yun in the match | Twelve 60 |
| 5 (Dudley) | 28 | Dudley in the match | Twelve 28 |

Every one of the 949 lands in a **character** group, never a stage or menu
group — so every one of them is gated on a specific opponent being present, and
**Gill's group is by far the largest target (541 of 949, 57%)**. That reorders
§8's worklist: the Gill-landing cluster is both the biggest cosmetic population
*and* the only one that contains a crash.

### 17.5 What this does and does not close

**Closed by measurement:**

- The residual can never be negative via `obj_group_table` (§17.2 corollary).
- Every group's offset-table length is now known statically, and 68 of 71 match
  `obj_group_table` exactly; the tool re-derives all of it on each run and will
  flag any future divergence.
- Across all 133,901 script cells, the pre-fix baseline measured the residual
  out of bounds in exactly **6** places, all Remy → Gill's group, all
  arcade-adaptation-only (§17.3). **With the §8.K range applied (landed
  `a5bc6a5b`, 2026-08-30), the current tree measures 0.**

**Not closed:**

- **The OVCT `parts_char` path — a separate, still-open 6.** §17.3's "OVCT
  path" paragraph runs the same bounds check over `parts_char → cg_number`
  (`eff01.c:169`) for all 20 characters and finds **6 violations, all Elena
  parts 85-90** — unrelated to Remy's six and untouched by §8.K. This is
  worklist item **B** (§18), and it is still **OPEN**. Do not read the bullet
  above as covering "all 20 OVCT tables"; it covers the script-cell residual
  only.

- **Whether Remy's `nmca[48]` / `exca[30,37,38]` are ever executed.** No jump
  inside Remy's own ten script tables targets them (checked: 0 `jmp`/`jpss`/`jsr`
  cells with `(koc, ix)` matching any of the four). Script entry is
  `set_char_move_init2(wk, koc, index, ip, scf)` (`charset.c:153`), whose
  `index` comes from move tables and from C call sites; **no exhaustive
  enumeration of the callers that can pass `(0, 48)` / `(7, 30|37|38)` was
  done.** The placeholder shape (§17.3) is suggestive, not decisive.
- **The 114 unbacked cg slots in Gill's group (1435..1548).** No arcade or PS2
  cell in the cast references cg 1435..1548 other than the six above — Gill's
  own data never does. Why `obj_group_table` over-assigns 114 slots to group 1
  was not established.

### 17.6 The boundary of the claim — every writer of `cg_number`

"The crash class is closed" is only meaningful against a stated scope. Grepping
every assignment to a `cg_number` field in `src/` (excluding `src/test/` and
`netplay/game_state.c`'s save/load) gives **34 sites**, in four families:

| Family | Sites | Arcade balance can influence it? |
|---|---|---|
| **script cell** — `setupCharTableData` copies the parsed cell (`charset.c:129-150`, `dst[i] = src[i]` at `:148`; reached via `check_cgd_patdat`, `charset.c:2666`) | 1 | **YES** — this is `remap_cg_number`'s only output. **Audited: §6.1 + §17.3.** |
| **OVCT `parts_char`** — `eff01.c:169` | 1 | **YES** — `Apply3SXRenderingConventions` rewrites it for `i < common_count`. **Audited: §17.3 (OVCT), §18.** |
| **propagation** — an effect copies the master's live value (`efff0.c:23`, `effi9.c:104`, `effe8.c:110`, `effe7.c:45`, `effj0.c:50`, `:78`; `plcnt.c:1031` records it into `zanzou_table[i]->cg_num`, which `effe8.c:114`/`effe7.c:102` read back) | 8 | inherits, introduces **no new value** |
| **compiled C constants** — literals and PS2-namespace tables (`effc2.c:97`, `:282`, `:307` = 9/9/18; `effh6.c:314-344` = 0x7949-0x794D; `effc3.c:1143`, `:1212`; `aboutspr.c:131`, `:137`, `:723`; `effm3.c:125`/`aboutspr.c:253` = `conn[].chr`; `plpdm.c:1051` = `exdm_ix_data`) | 24 | **NO** — arcade balance never touches them |

Spot-checked the constant family against both doors: literals 9, 18 → group 1
residual 9/18; `0x7949-0x794D` → group 30 residuals 153-157 of 176;
`effC3_nsc` (6 values) and `effk8k9_pattern` (18 values) → **0 violations**;
`exdm_ix_data`'s reachable `cg_number`s (359, 1801) → groups 1 and 2, residuals
359 and 233. All in bounds.

Two sites that look alarming and are not: `effG0_trans` (`effg0.c:99-102`) and
`effL1_trans` (`effl1.c:209-212`) both do
`cg_number = (cg_number + 1) & 0x7FFF` every frame — an unbounded incrementing
sprite index. Both then call `sort_push_request3` (`aboutspr.c:432-460`), which
**never reads `wk->cg_number`**: it delegates to `set_conn_sprite`
(`aboutspr.c:230-258`), which renders from `wk->conn[i].chr`. So the counter has
no rendering consumer on that path.

**One path arcade balance CAN influence is not covered by either audit.**
`eff61.c:275` and `effa8.c:275` build `conn[ix].chr = ewk->wu.old_cgnum + *ptr`
— an ASCII string offset added to a `cg_number` — and `old_cgnum` is set from
the master's live `cg_number` at `effe8.c:110` and `effj0.c:50`. In the two
sites above `old_cgnum` is instead set from a local (`eff61.c:245`
`letter_type`, `effa8.c:225` `char_ix`), so those particular uses are
constant-sourced — but the *field* is shared, and the arithmetic
`cg_number + arbitrary byte` has no bound. **Not audited. See §12.**

> ### So: is the crash class closed?
>
> **Both doors are now enumerated for both arcade-influenced paths, and the
> residual door is not empty.** With §8.A applied, class (a) is 0; the residual
> class is **6** and needs §8.K. When both land, the statement becomes:
>
> *"Across all 133,901 arcade script cells, no `cg_number` that arcade balance
> can produce is out of range for `obj_group_table`, and none produces a
> residual outside its group's offset table."*
>
> **This does NOT extend to the OVCT `parts_char` path.** An earlier draft of
> this boxed conclusion also claimed "and all 20 OVCT tables" here — that is
> false and conflicts with §18: Elena's unpatched OVCT tail (parts 85-90,
> `parts_char` 40182-40187, all ≥ 37664) is exactly an `a_ogt_oob` violation
> through the OVCT path, and `residual_audit.py`'s R2b check reports it as
> **6 open violations**, unchanged by either §8.A or §8.K (see §17.3's "OVCT
> path" paragraph and §18). That is worklist item **B**, and it is still
> **OPEN** — see §3.
>
> **That rests on five things**, each of which the tooling re-checks per run:
> 1. the parse being complete — **§19 shows no span truncates**, but §19.6 found
>    one unreferenced script the census never covered;
> 2. `obj_group_table`'s runs being contiguous and starting at `num_of_1st`
>    (71/71) — this is what makes a negative residual impossible;
> 3. the offset-table length being `first u32 / 4` (structurally validated
>    71/71, and aliased rows agreeing);
> 4. the four families above being the complete set of `cg_number` writers, with
>    only the first two arcade-influenced — **and the `old_cgnum + *ptr`
>    arithmetic being out of scope**;
> 5. `remap_cg_number` and `Apply3SXRenderingConventions` remaining the only
>    translations (`arcade_char_data.c:188`, `:679-685`).
>
> It does **not** rest on any code guard, because there is none — which is the
> whole argument for worklist items **C** and **L**.

---

## 18. Elena's OVCT tail: the "unreachable" claim was wrong

> **Read with §24 (2026-09-06).** This section's verdict — "not-observed, not
> provably unreachable" — is itself superseded: §24 enumerates every writer
> of the part index and shows the tail unreachable with no timing argument.
> Three statements below are wrong and are corrected there: §18.3(2)'s
> "monotone and unbounded" walk (every `parts_nix[i] == i`, so it is
> stationary), §18.3(2)'s "+1 for P1" (`player_number` is the *character*;
> the +1 is Gill's), and §18.6(iii)'s reading of `exdm_ix_data`'s subscript
> as a player slot (it is the character, and rows 2..19 are live). The rest
> — §18.1, §18.2, §18.5, §18.6(i)-(ii) — stands.

§7.5 and §8.B call Elena's unpatched OVCT parts 85-90 "currently unreachable"
because *"no selected `ovix` entry reaches a part ≥ 85"*. **That reason is
factually wrong.** The tail is still not-observed, but for a different and much
weaker reason.

### 18.1 The selection path, re-verified

| Step | Code |
|---|---|
| tables bound | `wk->overlap_char_tbl = cdat->ovct; wk->olc_ix_table = cdat->ovix;` — `charid.c:100-101` |
| **the cell byte is shifted first** | `wk->cg_jphos = jphos_table[wk->cg_olc_ix & 0xF]; wk->cg_olc_ix >>= 4;` — `charset.c:2717-2718`, and again `:2885-2886` |
| select | `wk->cg_olc = wk->olc_ix_table[wk->cg_olc_ix];` — `charset.c:2739` (guarded by `work_id == 1`) and `charset.c:2904` (unguarded) |
| entry shape | `OverlapSelection` is `s16 olc_ix[4]` (`structs.h:138-140`) — one OVIX entry supplies four part indices, one per overlap `type` |
| consume | `ewk->wu.cg_ix = mwk->cg_olc.olc_ix[type]` (`eff01.c:48`) → `ewk->wu.overlap_char_tbl = mwk->wu.overlap_char_tbl + ewk->wu.now_koc` (`eff01.c:141`) → `ewk->wu.cg_number = ewk->wu.overlap_char_tbl->parts_char` (`eff01.c:169`) |

**The `>> 4` is the piece §7.5 missed.** The effective OVIX index is the cell's
`olc` word shifted right by four, not the word itself.

### 18.2 Elena's OVIX names parts 85-90 outright

Decoded from the ROM (`LOC[ELENA]['ovix']`, `s16[4]` big-endian, 8 B/entry):
**91 entries, and it is the identity map** — `ovix[i] == {i, 0, 0, 0}` for every
`i` in 0..90, verified for all 91. So `ovix[85] = {85,0,0,0}` … `ovix[90] =
{90,0,0,0}`: **entries 85-90 select parts 85-90 directly.** Slots 1-3 are zero
throughout, so only overlap `type == 0` is ever live for Elena.

Elena's OVCT entries 85-90 carry `parts_char` 0x9CF6-0x9CFB (40182-40187),
`parts_nix[i] == i`, `parts_timer = 255`. All six are ≥ 37,664.

**So the tail is exactly one cell datum away.** A cell whose `olc` word is
≥ `0x550` (85 << 4) puts `cg_olc_ix` at 85, and `cg_number` becomes 40182 —
the identical fault as the #363 crash. `cg_olc_ix` is a `u16` (`structs.h:322`),
so `0x550` is perfectly representable; nothing in the data or the code forbids
it.

### 18.3 What actually holds it closed — two data facts, not a code invariant

1. **No Elena cell emits an effective `cg_olc_ix` ≥ 85.** Over all 7,769 of her
   cells in all ten script tables, the pre-terminator distribution of
   `olc >> 4` is `{0: 7596, 1..14: 3 each, 15: 5, 16: 5}` — **max 16**; after a
   terminator, `{0: 121}` — nothing but zero. Every nonzero value lives in one
   script, `saca[48]`, and every such cell has `ctr = 1`.
2. **The forward walk cannot get there in practice.** `eff01.c:56-65` advances
   the part index on timer expiry — `cg_ix = parts_nix` if nonzero, else
   `cg_ix++` — and `get_new_parts_data` re-applies a `+1` for P1/type-0/`rl_flag`
   (`eff01.c:50-52`, `:136-139`). With `parts_nix[i] == i` that combination is
   monotone and unbounded, so it *would* march through 85-90 and off the end.
   But `cg_ctr` reloads from `parts_timer` on every advance (`eff01.c:142`), and
   **every one of Elena's 91 entries has `parts_timer = 255`**, so one step costs
   255 accumulated frames on a single held `olc` value, while her longest
   constant-nonzero run is 5 cells. Reaching part 85 from the anchor at 16 needs
   69 steps.

Both are properties of the shipped data. Neither is enforced anywhere.

### 18.4 Verdict

> **NOT-OBSERVED, not provably unreachable.** The correct framing for §8.B is
> **undefended**, not "latent but unreachable". Three named things would open it:
> a script cell carrying `olc >= 0x550`; anything that holds one nonzero
> `olc_ix[0]` for 255 accumulated frames (note `move_effect_work`,
> `effect.c:32-51`, is unconditional, so `--cg_ctr` also ticks during hit-stop,
> when the master's `char_move` does not — `plmain.c:322`); or a change to the
> `exdm_ix_data` subscript in §18.6.

This strengthens worklist item **C** (a bounds guard at the
`obj_group_table[n]` sites) independently of any OVCT remap.

### 18.5 The fix, mechanically derived

The PS2 patch loop's own deltas, measured over Elena's 85 common entries
(`ps2_parts_char[i] − arcade_parts_char[i]`):

| entries | delta |
|---|---|
| 0 | both zero |
| 1-32 | **−28482** (−0x6F42) |
| 33-34 | −28385 |
| 35-84 | **−29360** (−0x72B0) |

Applying the trailing band's **−29360** to the six tail values gives
**10822-10827**, which `obj_group_table` places in **group 9 — Elena's own** —
at residual 614-619 against a 1,550-entry table. **In bounds, own group.** That
is a necessary condition, not proof the sprites are the intended ones (same
caveat as §8.A's `exca[58..65]`).

### 18.6 Three adjacent findings from the same sweep

**(i) One live OVIX overrun exists, and it is pre-existing in PS2.**
`wk->cg_olc = wk->olc_ix_table[wk->cg_olc_ix]` is unbounded (`charset.c:2739`,
`:2904`). Comparing each character's maximum pre-terminator effective
`cg_olc_ix` against their arcade OVIX entry count: **19 of 20 are in range**
(and are in fact exactly `entries − 1` for most). **IBUKI is not: max 2277
against a 2230-entry arcade OVIX.** The single emitting cell is
`cuca[37]` cell 28, `olc` word **36432**. Its PS2 counterpart cell is
**byte-identical** (36432) and PS2's OVIX is 2,235 entries — **also short**. So
by the §6.1 discriminator this is a hazard the shipped PS2 build carries too,
**not an adaptation defect**, in the same family as §7.5 and §15.7's 300 HIIT
hits. It is still an unbounded read of 43-48 entries past the end of a live
table, and worth a guard.

**(ii) The arcade OVCT/OVIX tables really are two entries shorter**, not
under-declared. Elena is the only character with `arcade > ps2` (91 vs 85, for
*both* sections); every other character is `arcade == ps2 − 2`. Elena's `ovix`
(`0x2AE880`, size `0x2D8`) ends exactly where her `ovct` (`0x2AEB58`, size
`0x5B0`) begins, which ends exactly at her `rict` (`0x2AF108`) — adjacent with
no gap, so 91/91 is the real extent. Spot-checked ALEX: the bytes immediately
past his declared 57-entry OVCT decode as `rict` data (`nix=513, char=1,
disp=178`), not as OVCT, while PS2's entries 57-58 are genuine parts.

**(iii) A dormant unremapped `cg_number`/`cg_olc_ix` source.**
`plpdm.c:1049-1051`:

```c
datadrs = exdm_ix_data[wk->wu.dm_exdm_ix][wk->player_number];   /* :1039 */
wk->wu.cg_olc_ix = datadrs[3];                 /* NOT shifted, NOT bounded */
wk->wu.cg_olc = wk->wu.olc_ix_table[wk->wu.cg_olc_ix];
wk->wu.cg_number = datadrs[4];                 /* NOT remapped              */
```

`exdm_ix_data` is `const u16 [2][20][5]` (`plpdm.c:167`). Measured over all 40
inner rows: `datadrs[3]` is **0 in 38 of them and 964 in the other two** — block
0 row 7 and block 1 row 7 — and the 20 rows' `datadrs[4]` values are
`{359, 1801, 2744, 3865, …, 26103}`, which `obj_group_table` maps **one-to-one
onto groups 1..20 in character order**. So the second subscript is semantically
the **character**, while the code indexes it with `player_number` ∈ {0,1}:
rows 2..19 are unreachable, and every character reads Gill's or Alex's row.
Consequences today: `cg_olc_ix` is always 0 (safe), and `cg_number` is always
359 (group 1, residual 359) or 1801 (group 2, residual 233) — both **in bounds**,
so no fault, but a wrong sprite whenever Gill or Alex happens to be loaded.
Row 7 is Ibuki's, and `964` is in range only for Ibuki's 2,230-entry OVIX —
**if that subscript is ever "fixed" to the character, `olc_ix_table[964]` is an
out-of-bounds read for 19 of the 20 characters.** (That the intended subscript
is the character is an *inference* from the 1:1 group mapping; it was not traced
to the CPS3 original.)

---

## 19. Are any declared spans TOO SMALL? (the direction nobody had checked)

§7.6 and §15.6 record spans whose declared `location_data[]` size is **larger**
than the real data — harmless at run time, digest-polluting. The opposite error
would **truncate parsing and hide cells from every audit run so far**, which
would undermine "133,901 cells, exhaustively audited". All 25 sections × 20
characters = **500 spans** were checked.

> ### Verdict: **0 truncated. 0 possible-truncation. 0 unboundable. 500/500 COVERED.**

### 19.1 The structural fact that governs everything: the 500 spans tile the ROM

Sorting every declared `[offset, offset+size)` by offset
(`arcade_char_data.c` -> `location_data[]`):

- **overlaps: 0**
- **negative gaps: 0**
- **zero gaps: 492 of 499 adjacencies** — the 500 spans form exactly **8
  perfectly contiguous runs**

so a section's declared end **is** the next section's declared start. Truncating
section X therefore requires section Y's declared *offset* to be wrong. The
whole question reduces to: **is every declared offset independently pinned?**

Every character uses the identical section order:
`nmca dmca btca caca cuca atca | ovix ovct rict | exca saca cbca yuca |
hiit boda hana hosa atta cata caua | atit sernd | stxy mvxy prot`.

### 19.2 Every offset is independently pinned

| pin method | spans | strength |
|---|---|---|
| script pointer-table abutment | 200 | exact |
| PS2 element identity (shift 0 beats ±1 element) | 259 | strong |
| RICT `[group][24 vs 20]` model identity (§15.4) | 20 | strong |
| OVCT alignment argmax + `ovix → ovct` exact fit | 20 | strong (1 weak — §19.5) |

**Script sections (200/200).** The table at `offset` is a NUL-terminated list of
**absolute** big-endian CPS3 pointers (`0x6000000 + offset + rel`), so
`rel = ptr − 0x6000000 − declared_offset` is only self-consistent at the true
offset. For **199 of 200**, `min(rel) − 8 == 4 × (n + 1)` exactly — the first
script abuts the pointer table with zero padding. In all 200, every pointer
lands in `[8, size]`.

**Non-script sections.** Element-exact match against the PS2 counterpart at
shift 0 versus ±1 element. The cases that settle the doc's own open questions:
**Ibuki's `mvxy` is byte-for-byte identical to PS2's (1752/1752 B)** at its
declared offset, which pins the end of his `stxy` — so §15.6's "Ibuki `stxy` is
4 B shorter than PS2's" is a **real** difference, not truncation. **Yun's
`boda`** matches 257/257 elements at the declared offset and 8/257 shifted one
element earlier, pinning the end of his `hiit`.

### 19.3 No script is cut off mid-body

Walking the last script of each of the 200 script tables to its first
`TERMINATORS` command, **unbounded** (note `read_char_table` reads only **8
bytes** of a command cell and then `SDL_SeekIO`s the rest without reading —
`arcade_char_data.c:161-175` — so the read-end after a terminator is
`terminator_offset + 8`):

```
last-script terminator read-end, relative to the declared size (200 spans):
  exactly 0            : 94 spans     <- the terminator's last read byte IS the declared end
  short of it (slack)  : 106 spans
  PAST the declared end:   0 spans
```

**The truncation signature — a last script still running when the declared end
cuts it off — does not occur once.** The only overruns are of the *stride*, not
the *read*.

**Forward jumps past the terminator** were checked as the one hole in that
argument: **2 of 200** last scripts contain an intra-script forward jump whose
target sits past the terminator — `DUDLEY saca[87]` (cell 4, `comm_wcne`, word
`0x4007`) and `ELENA atca[159]` (cell 22, `comm_wcne`, `charset.c:1641-1646`,
word `0x4002`). **Both targets land inside the declared span**, and both were
decoded here: Dudley's cell 11 is a sprite cell with raw CG 6774 → remapped 5622
→ group 5, residual 630 of 1124 — **in bounds**; Elena's cells 24-25 are
commands (`comm_jmp`, `comm_end`), no sprite cell at all. **0 of 200** reach
past the declared size.

### 19.4 Fixed-element sections: the extents are confirmed positively

- **`size % element_size == 0` for all 300 fixed-element spans.**
- **The decisive test — do the arcade bytes past a declared end match PS2's
  *next* elements? — is NO in all 54 arcade-shorter spans.** Not one extra
  element matches. For HIIT/BODA the bytes past the end are all zero (they are
  the neighbour's element 0, zero in PS2 too); for OVCT/OVIX they are unrelated
  non-zero bytes. So §15.6's "the arcade genuinely has fewer entries" **holds**.
- **Exact-fit index witnesses confirm the extents, not merely the offsets:**

| section | index source | result |
|---|---|---|
| ATIT | `cg_att_ix >> 6` | max == `entries − 1` for **19/20** (Yun 172/174) |
| BODA | `HIIT.boix` | max == `entries − 1` for **20/20** |
| HANA | `HIIT.bhix + haix` | max == `entries − 1` for **20/20** |
| STXY | `cg_add_xy` / `exec_char_asxy`, pre-terminator | max == `entries − 1` for **17/20** |
| RICT | `cg_rival` | highest touched == `entries − 1` for **19/20** |
| OVCT | `max(ovix)` (`eff01.c:141`) | == `entries − 1` for 13/20, in bounds for the other 6 (Yun — §19.5) |

**Ibuki's `stxy` — the doc's "4 B short" — is refuted positively:** his own
maximum pre-terminator STXY index is **425 = 426 − 1**. The data uses exactly
the 426 slots declared.

### 19.5 `coalesce_adjacent_sections` **masks** truncation rather than revealing it

`arcade_char_data.c:263-314` mallocs one buffer per contiguous run and copies
each span to `allocation + offset − base_offset`, so the result is a positional
byte image of the ROM run. An over-read past a truncated section's end would
therefore land on the *correct ROM position* — plausible-looking data, no fault.
Two caveats worth recording: the neighbour's bytes carry the *neighbour's* byte
order transform, not the reader's; and the six run-terminal spans per character
(`cbca`, `yuca`, `hosa`, `caua`, `sernd`, `prot` — 120 spans) sit at the end of
their allocation, so an over-read *there* would leave the heap block.
`ArcadeCharData_ComputeDigest` (`:583`) hashes `span->size` bytes, so coalescing
does not affect the digest.

**Two spans rest on weaker evidence** and are recorded rather than smoothed
over: **HIIT has no exact-fit witness of its own** (its max index is dominated
by the pre-existing `saca[1]`/`saca[7]` artefact of §15.7), so it rests on
BODA's pinned offset; and **Ken's OVCT is 0% identical to PS2's at every shift
−4..+4**, so it is pinned only by being sandwiched between a pinned OVIX and a
pinned RICT at gap 0, plus `max(ovix) = 41 = 42 − 1`.

### 19.6 Does "133,901 cells is exhaustive" survive?

**With respect to truncation, yes — no cell is hidden by a short declared span.**
But "exhaustive" is not literally true, and two gaps were found while checking:

**(a) An entire unreferenced script exists in Ibuki's `atca`.** 36,288 of
1,791,768 script-span bytes (2.03%) are never visited by the audit walk; 35,912
of those are post-terminator slack in a last script. The remaining **376 B is
real, well-formed script data that no pointer-table entry references**:

> `IBUKI atca` (`0x27F044`), rel **0x2A4-0x42C** — a complete `cgd = 6` script:
> 14 sprite cells plus 2 commands, terminated by `comm_end`, whose read-end
> lands exactly at `min(rel) − 8`. It is the **only** non-zero header gap in all
> 400 arcade + PS2 script tables, and **PS2 carries it too, identically**.
> Decoded here: raw CGs 11160-11197 → remapped **8792-8829**, all in
> `obj_group_table` group **8 — Ibuki's own** — at residuals 408-445 against a
> 1,796-entry offset table. **No class (a), no class (b), no residual
> violation.** But that is a result that had to be *computed*; the 133,901-cell
> census never covered it.

**(b) Two reachable post-terminator cells** (§19.3) that `cg_audit.py`'s walk
skips. Both checked; both in bounds.

### 19.7 Correction to §7.6, and the real over-declaration list

**§7.6's "real=" column is `size − max(pointer)`, which is not the real end.**
Measured properly (last script's terminator read-end):

| Character | Section | Declared | Real end | **Slack** | §7.6 said |
|---|---|---|---|---|---|
| REMY | `yuca` | 0x73E0 | 0x11C0 | **0x6220** | 0x6230 |
| HUGO | `saca` | 0x4164 | 0x374C | **0x0A18** | 0x0CA0 |
| TWELVE | `saca` | 0x5E50 | 0x55B0 | **0x08A0** | 0x0920 |
| URIEN | `saca` | 0x3A54 | 0x36E4 | **0x0370** | 0x08D0 |
| NECRO | `saca` | 0x3FF8 | 0x3CC8 | **0x0330** | *(not listed)* |
| ELENA | `saca` | 0x6638 | 0x63B0 | **0x0288** | 0x05C0 |
| SEAN | `caca` | 0x12C0 | 0x1040 | **0x0280** | 0x05E8 |
| TWELVE | `cbca` | 0x089C | 0x06BC | **0x01E0** | *(not listed)* |
| DUDLEY | `saca` | 0x6AE4 | 0x6944 | **0x01A0** | *(not listed)* |
| URIEN | `atca` | 0x290C | 0x2794 | **0x0178** | *(not listed)* |
| NECRO | `caca` | 0x2124 | 0x1FF4 | **0x0130** | *(not listed)* |
| DUDLEY | `caca` | 0x07B8 | 0x06B0 | **0x0108** | *(not listed)* |

**`ELENA exca` is not over-declared at all** — declared `0x35BC`, terminator
read-end `0x35BC`, **slack 0**. §7.6's `0x7A0` was the pointer-table artefact.
Likewise `GILL yuca`, whose last script terminates exactly at `0x1388`.

Across all 200 script spans: **94 have slack exactly 0, 106 have slack > 0,
none is negative, total slack 36,680 bytes** — all of it hashed by
`ArcadeCharData_ComputeDigest`. So §8.G's list of "seven sections" is really
**106**, of which 12 exceed 0x100. Fold the corrected table into that item.

> **CORRECTED by §27 (2026-09-06).** "Real end = first terminator's read-end"
> undercounts: a `jmp`/`jpss`/`jsr` `pat` is a 1-based cell index and can enter
> a script past its first terminator (so can `comm_end`, `cg_next_ix`,
> `cg_wca_ix`, `cg_extdat`, the relative jumps, and a script that simply runs
> into the next one). Measured by reach, 14 of the 106 spans end later than
> this table says — `DUDLEY caca` is not over-declared at all (its `caca[6]`
> continues past a cell-1 `roa` and ends in a `comm_ret` exactly at the
> declared size), `DUDLEY saca` is 0x138 not 0x1A0, and the others shrink by
> 8-16 bytes. The total is **35,696 bytes**, not 36,680. §27.2 has the table.

### 19.8 One §12 unknown now closed

§12 asked whether Remy's over-declared `caua`/`hosa` extents (§15.6) run into
**another character's** data. The 500-span tiling has **zero overlaps**, so they
do not: they sit inside gaps, exactly as §15.6 stated.

---

## 20. What plays the seven bad scripts (trigger analysis, 2026-08-30)

§7.2 and §17.3 said *which* cells are wrong. This section says *what makes the
engine run them*. Method: read the selection code, then enumerate the shipped
data that can drive it — no move was classified from memory or from its name.

### 20.1 The selection chain for a damage reaction

The victim never picks its own damage script. The attacker's `ATT` record does:

1. `wk->att = *(wk->att_ix_table + wk->cg_att_ix)` (`charset.c:2944`), where
   `att_ix_table` is that work's ATIT (`charid.c:110`) and the index is the
   cell's `cg_att_ix >> 6` (§15, `data_audit.decoded_indices`).
2. `set_new_attnum` then splits `att.level`: **`wk->at_attribute = (wk->att.level >> 4) & 3`**
   (`charset.c:2947`) and `wk->att.level &= 7` (`charset.c:2949`). So
   *"electric"* is not a move name — it is **bits 4-5 of one ATIT byte**, value
   **2**. (1 = flame, 3 = freeze.)
3. On a hit, `dm_reaction_init_set` sets `ds->wu.routine_no[2] = as->wu.att.reaction`
   (`hitcheck.c:576`) and immediately rewrites it:
   `change_damage_attribute(as, as->wu.at_attribute, routine_no[2])`
   (`hitcheck.c:590`), which for `atr == 2` is `ix = attr_thunder_tbl[ix - 32]`
   (`hitcheck.c:2193`, table at `hitcheck.c:2261`). Measured mapping:
   `32..44 → 43`, `64..70 → 68`, `87..114 → 104`, everything else → 0.
4. Secondary conversions, in the same two callers:
   - victim airborne → `get_sky_sp_damage` / `get_sky_nm_damage`
     (`hitcheck.c:492`, `:494`), which map **43 → 104** and **68 → 104**;
   - victim grounded with `zu_flag == 0` and `pat_status >= 32` →
     `get_kagami_damage` (`hitcheck.c:513`), which maps **43 → 68**;
   - `hddm_damage_tbl` / `trdm_damage_tbl` are **0** at 43, 68 and 104, so the
     head/trunk redirects leave all three alone (`hitcheck.c:2297`, `:2301`).
   - victim dying → `dd_convert[routine_no[2]][dm_attlv]` (`plpdm.c:1603`), whose
     rows 43, 68 and 104 are all `{104,104,104,104}` (`plpdm.c:123`).
5. `wk->as = &dm_reaction_table[routine_no[2]]` (`plpdm.c:1618`), then
   `plpdm_lv_00[routine_no[2]](wk)` (`plpdm.c:201`, table `plpdm.c:157`).
6. The three landing entries (`dm_reaction_table`, `plpdm.c:103`):

   | `routine_no[2]` | entry `{r_no, char_ix, data_ix}` | handler | script chosen |
   |---|---|---|---|
   | **43** | `{12, 82, 0}` | `Damage_12000` (`plpdm.c:488`) | `dmca[82 + dm_attlv]` (`plpdm.c:495-496`) |
   | **68** | `{13, 86, 0}` | `Damage_12000` (`plpdm_lv_00[13]`) | `dmca[86 + dm_attlv]` |
   | **104** | `{18, 15, 0}` | `Damage_18000` (`plpdm.c:668`) | `btca[15]` (`plpdm.c:673`, koc 6 = `btca`, `charid.c:89`) |

   `dm_attlv` is `att.level & 7` copied at `hitcheck.c:1589`, but every table it
   indexes is `[...][4]` (`_damage_pause_table`, `src/bin2obj/etc.c:5`;
   `dd_convert[115][4]`, `plpdm.c:123`), so its usable range is **0..3** and the
   two `dmca` entries span exactly **82..85** and **86..89** — the §7.2 run.

   The flame and freeze siblings confirm the reading: `dm_reaction_table[42] =
   {12, 92}` and `[44] = {12, 74}` (flame/freeze ground `dmca`), `[103] = {18, 8}`
   and `[105] = {18, 19}` (flame/freeze `btca`). **`btca[15]` is the electric
   knockdown, `dmca[82..89]` the electric damage reaction** — for every
   character, not just Elena. (Hugo's `btca[15]` is separately listed as an
   off-by-one delta in §7.3(ii); same slot.)

`dm_impact` (16 in the §5.2 repro) only picks `dm_step_tbl` via
`_select_hit_dsd` (`plpdm.c:497`); it plays **no part** in choosing the script.

**No reaction code reaches 43, 68 or 104 without the attribute rewrite.** A scan
of all 20 arcade ATITs finds reaction values `{0,32..40,64,70,88..114}` and
**zero** occurrences of 42, 43, 44, 67, 68, 69, 103, 104 or 105; the only direct
occurrences anywhere are `_ef13_catt_table[35] = 42` and `[36] = 103`, both
flame and both idempotent under `attr_flame_tbl`. The throw path
(`set_caught_status`, `hitcheck.c:274`, `:289`) and `get_catch_off_data`
(`plpdm.c:1688`) assign `att.reaction` **without** the rewrite, so they cannot
select these scripts either.

### 20.2 The electric census — every ATT record in the game with `at_attribute == 2`

Two ATT universes exist: the 20 per-character ATITs, and `_ef13_catt_table`
(`src/bin2obj/char_table.c:3994`), which **every** projectile uses — `eff13`
sets `charset_id = 11` and calls `set_char_base_data` before overwriting
`charset_id` with `tama->kind_of_tama` (`eff13.c:51-52`, `:63`), and
`char_init_data[11]` is `char_init_data_ex[0]` (`charid.c:118-121`), whose
`atit` slot is `_ef13_catt_table` (`charid.c:48`).

**Per-character ATIT — 14 electric records in the whole cast:**

| Char | ATIT indices (reaction, `attlv`) | cells that use them |
|---|---|---|
| NECRO | 42 (37,1) 43 (37,1) 44 (92,2) 45 (37,2) 56 (37,2) 67 (91,2) 73 (37,2) 78 (92,2) | `saca[32..35]`, `saca[36..39]` |
| URIEN | 45 (93,2) 56 (93,2) | `saca[44]` |
| TWELVE | 42 (37,1) 67 (91,2) 73 (37,2) 78 (92,2) | **no cell references them** |

No other character has a single ATIT byte with bits 4-5 == 2. Twelve's four are
byte-identical to Necro's four of the same index and are unreferenced — dead
rows in a table that was cloned.

**`_ef13_catt_table` — 10 electric records**, reached through these projectiles
(cell `effect == 2` / control cell `comm_exec` with `eff == 2`, data = `tama_data`
index; `eff13.c:1921`):

| `_ef13_catt_table` idx | reaction, `attlv` | ef13 script | `tama_data` rows | spawned by |
|---|---|---|---|---|
| 4, 5 | 37, 1 | 6 | 8, 9, 10, 11, 52 | **RYU `cbca[14..18]` — Denjin Hadouken** (§5.4's chain) |
| 6 | 37, 1 | 10 | 20, 22, 23 | NECRO `saca[24..27]` |
| 34 | 91, 2 | 71 | 21 | NECRO `saca[24..27]` |
| 38 | 37, 1 | 86, 91 | 83, 84, 85, 240, 241, 242 | URIEN `saca[33..35]` |
| 41, 56 | 91, 1 | 102 | 104 | URIEN `saca[66..69]` |
| 44 (r 32), 45 | 32/37, 1 | 112, 143 | 123, 126 | URIEN `saca[62]`, `saca[65]` |
| 105 | 37, 1 | 144 | 86, 132 | URIEN `saca[36]` (row 132) |

**So the complete set of electric attackers in the game is Ryu, Necro and
Urien**, all through Super Arts (Ryu's is the SA-only Denjin chain). Twelve
carries the rows but no move that uses them.

**PS2 discriminator.** All eight Necro and all four Twelve electric ATIT records
are **byte-identical** arcade-vs-PS2; Urien's 45 and 56 differ only in
`att.level` bit `0x40` (`jump_att_flag`, the §8.I finding) — the attribute bits
match. `_ef13_catt_table` is shipped C, common to both builds. **The triggers are
therefore pre-existing shipped behaviour, not an adaptation defect.** The PS2
build runs exactly these scripts, with the correct sprites; only the arcade CG
values are wrong.

### 20.3 How `exca` and `nmca` are entered (the other four sites)

Enumerating all **830** `set_char_move_init` / `set_char_move_init2` call sites
in the tree: the literal `koc` arguments are 0 (×516), 9, 5, 1, 6, 4 and 3 —
**no C site passes 7 (`exca`), 8 (`cbca`) or a bare 2**. The variable-`koc`
sites are `set_char_move_init_ca` (always 2, `plpca.c:528` from
`plpca.c:140-393`), `exset_char_move_init(&wk->wu, wk->wu.now_koc, …)`
(`plpdm.c:655`, which reuses the current table), and the script-driven forms in
`charset.c` — `comm_jmp` / `comm_jpss` / `comm_jsr` (`charset.c:736`, `:742`,
`:750`), `comm_rapp` / `comm_rapk` / `comm_rapp2` / `comm_rapk2`
(`:1319`, `:1339`, `:1706`, `:1726`) and the `cm*` registers loaded by the
`comm_rja…rja7` / `comm_rmja` family (`comm_rja`, `charset.c:865`). All of those
take `koc`/`ix` **from the script data**.

So `exca[n]` is reachable **iff** some control cell in that character's own ten
tables names `(koc = 7, ix = n)`. A deliberately over-broad scan — every control
cell of all 20 characters × 10 tables, every opcode, reading the three operand
slots as `koc`/`ix`/`pat` — gives:

- **ELENA `exca[58..65]`: 0 references.** Her `exca` has 68 entries; the highest
  index any operand names is **56**. Seven other characters (Yun, Hugo, Ibuki,
  Yang, Akuma, Chun-Li, Makoto) *do* name `exca[58..65]` from their `cbca`
  scripts, so the slot is real — Elena simply has no path into hers.
- **REMY `exca[30]`, `[37]`, `[38]`: 0 references.** 17 of the other 19
  characters name all three from `cbca` (e.g. `ALEX cbca[8]`/`cbca[15]`,
  `ELENA cbca[41]`/`[45]`/`[46]`); only Gill, Makoto and Remy do not.
- **REMY `nmca[48]`: 0 references from any character's script data.**

`nmca[48]` has exactly one producer in the whole tree: `Normal_52000`
(`plpnm.c:1045`), `set_char_move_init(&wk->wu, 0, 48)` at **`plpnm.c:1057`**,
immediately after `remake_sankaku_tobi_mvxy` — the **wall jump** (sankaku tobi)
kick-off, which then chains to `nmca[14]` (`plpnm.c:1067`). `routine_no[2] = 52`
is written in exactly one place, `check_sankaku_tobi` (`pls01.c:265`), whose
first gate is `if (wk->spmv_ng_flag & DIP_WALL_JUMP_DISABLED) return 0;`
(`pls01.c:243`, bit 18, `sysdir.h:26`). `spmv_ng_flag` comes from
`omop_spmv_ng_table` (`plcnt.c:1373`), into which `init_omop` **ORs**
`sysdir_base_move[My_char[i]]` (`sysdir.c:115-116`) *after* the option parse —
and nothing in the tree ever clears bit 18. `sysdir_base_move[20]`
(`sysdir.c:38-47`) sets `DIP_WALL_JUMP_DISABLED` for **every character except
index 15 = CHAR_CHUNLI** (`constants.h:54`); index 9 = CHAR_ORO is the only one
without `DIP_AIR_JUMP_DISABLED`. **Chun-Li is the only character who can run
`nmca[48]`.** Twelve's X.C.O.P.Y. does not open a door: it takes the target's
`charset_id` *and* the **opponent's** `spmv_ng_flag`
(`effk7.c:71-72`, `plcnt.c:1384-1394`), so Twelve-as-Remy inherits Remy's
wall-jump ban. The other literal `(0, 48)` call, `eff94.c:127`, is a stage prop
whose `char_table[0]` is `char_add[bg_w.bg_index]` (`eff94.c:298`), not a
character table.

The data agrees with the code: `nmca[48]` is a 3-cell stub
(`cg/t8, cg/t0, cg/t255, comm_roa`) in **all 20** characters, repeating one CG in
18 of them — **except Chun-Li**, the only wall-jumper, whose slot holds two
*distinct* sprites (24384 then 24385). Remy's holds Alex's 1537 twice, i.e. the
§17.3 crash cell.

Fall-through was checked and does not apply: every script involved
(`ELENA exca[56..65]`, `REMY nmca[46..49]`, `REMY exca[29..31]`, `[36..39]`)
ends in `comm_roa` (opcode 1), an unconditional transfer.

### 20.4 Trigger table and reachability verdicts

| Site | Selected by | Triggering attacks / conditions | Verdict |
|---|---|---|---|
| **ELENA `dmca[82..85]`** | `routine_no[2] = 43` → `dmca[82 + dm_attlv]` (`plpdm.c:495`) | Electric hit on a **grounded, surviving** Elena whose `pat_status < 32`. Producers: Ryu's Denjin projectile (`cbca[14..18]`), Necro `saca[24..27]`/`[32..39]`, Urien `saca[33..36]`, `[62]`, `[65..69]`, `saca[44]`. Measured `attlv` on electric ground records is 1 or 2 → `dmca[83]`, `dmca[84]` | **CONDITIONAL, routine in any Ryu/Necro/Urien match.** This is #363. |
| **ELENA `dmca[86..89]`** | `routine_no[2] = 43` then `get_kagami_damage` → 68 → `dmca[86 + dm_attlv]` (`hitcheck.c:513`, `kagami_damage_tbl[11] = 68`) | Same attacks, but the victim's `pat_status >= 32`. `pat_status` is written only by a `cgd == 6` cell with `cg_status & 0x80` (`charset.c:2693`, `:2865`), or by `comm_sps` (`charset.c:759-761`), and is re-instated verbatim on `scf != 0` script entries (`charset.c:201-203`). Elena's **only** sources of a value >= 32 are two `comm_sps` cells, in **`btca[18]` and `btca[33]`** — knocked-down scripts. (Her `cgd == 6` cells only ever set 20.) | **CONDITIONAL — narrower than `[82..85]`, not unreachable.** Needs an electric hit landing while she is grounded in/after a knockdown. |
| **ELENA `btca[15]`** | `routine_no[2] = 104` → `Damage_18000` → `btca[15]` (`plpdm.c:673`) | Three independent producers: (a) any electric hit on an **airborne** Elena (`sky_nm/sp_damage_tbl[11] = 104`); (b) any electric hit that **kills** her (`dd_convert[43] = 104`); (c) electric records whose reaction is 91/92/93 — Necro ATIT 44/67/78, Urien ATIT 45/56, `_ef13` 34/41/56 | **ROUTINE.** Strictly *easier* to reach than #363's `dmca` site: every electric KO goes through it. The §5.2 replay very likely already hit it — `btca[15]` carries the **same three CGs** (38146-38148), so the 19 observed faults cannot be attributed to `dmca` alone. |
| **ELENA `exca[58..65]`** | `koc = 7` is only ever supplied by script operands (§20.3) | Nothing. 0 references in Elena's 10 tables; no C caller passes `koc = 7`; predecessors terminate with `comm_roa` | **APPARENTLY UNREACHABLE** (38 of the 66 cells). |
| **REMY `nmca[48]`** | `Normal_52000` wall-jump kick-off (`plpnm.c:1057`) | Wall jump, gated by `DIP_WALL_JUMP_DISABLED`, which `sysdir_base_move[]` sets for everyone but Chun-Li; X.C.O.P.Y. inherits the opponent's ban | **APPARENTLY UNREACHABLE.** Corroborated by the data: Chun-Li's slot is the only one with real content. |
| **REMY `exca[30]`, `[37]`, `[38]`** | Script operands only (§20.3) | Nothing in Remy's tables. 17 of the other 19 characters *do* jump there from `cbca`; Remy does not | **APPARENTLY UNREACHABLE.** |

**Answer to "is Denjin the only way to crash Elena?" — no.** Denjin is one of
**three** electric attackers; **Necro and Urien** drive the identical selection
chain and reach the identical scripts. And no attacker is confined to the
`dmca` site: **any** electric hit that catches Elena airborne, and **any**
electric KO, is redirected to `btca[15]`, which carries the same three
out-of-range CGs (38146-38148). Fixing only what #363 describes would leave both
the other two attackers and the more common `btca` door open.

**Consequence for the worklist.** Elena's 66 cells split into **28 that ordinary
play reaches** (`dmca[82..89]` 24 + `btca[15]` 4) and **38 that nothing
observed can reach** (`exca[58..65]`). Remy's six are all in the apparently
unreachable class. That does not demote either fix — the §8.A range is one table
row and covers all 66 at once, and "apparently unreachable" is a static
argument, not a bound (§20.5) — but it does say where the severity is.

### 20.5 What this analysis does not establish

The unreachability verdicts rest on an exhaustive scan of the *shipped script
operands* plus an exhaustive enumeration of the *C call sites*. They are not a
proof: a corrupted or rolled-back `char_index`, an index arriving from a table
not scanned here, or an emergent path through `cm*` register reuse would all
bypass the argument. They are stated as "apparently unreachable", and §8.L's
guard remains the thing that makes the class safe regardless.

---

## 21. Sound codes — the second byte-passed namespace (fifth pass, 2026-09-02)

**Citation style for this section.** This document is *not* in
`tools/doc-citations/baselines.txt`, so its line numbers are not enforced and
must not be hand-maintained. Everything below cites a **symbol** (`file` ->
`function`/`table`) or the **exact text** of a line. Where a line number appears
it is a hint qualified by the commit it was read at, never an address.

### 21.1 Why this pass happened

A player bug report, 2026-09-02: *"Makoto's Hayate has the wrong sound — it
should be Chesuto."* The report is correct, the cause is a port defect, and it
is **not** a sound-system bug. It is the same class of defect this entire
document is about — an un-translated CPS3 value byte-passed into a namespace
that was authored against the PS2 data — reached through a field §4.4 lists as
byte-passed and which no audit had ever diffed.

§4.4 names `cg_se` in its byte-pass list. `cg_audit.py` audits **sprite
indices**; `data_audit.py` audits the **13 non-script sections**. Neither ever
compared `cg_se` *values* arcade-vs-PS2. That gap is the bug.

### 21.2 The dispatch chain

Every link read in code, at `90bc598d`:

1. `arcade_char_data.c` -> `read_char_table()` copies the field unmodified. The
   line reads, in full:
   `SDL_ReadU16BE(rom, p); // cg_se ... cg_olc_ix`
   Contrast the next statement, `cg_number = remap_cg_number(cg_number, character);`
   — `cg_number` is the *only* translated value (§4.4).
2. `charset.c` -> `check_cgd_data` does `wk->cg_se >>= 4;` then dispatches
   `sound_effect_request[wk->cg_se](wk, check_xcopy_filter_se_req(wk))`. The low
   nibble is flip/priority, not part of the code.
3. `se_data.c` -> `sound_effect_request[1024]`. For the codes at issue the entry
   is `Se_Myself` (`se.c`), which adds `uid * 0x300` for P2 and calls
   `SsRequestPan`.
4. `sound3rd.c` -> `remake_sound_code_for_DC`:
   `rmcode->code = (cd = sdcode_conv[code]) & 0xFFF;`
5. Case `0x0` of `cd & 0xF000` issues
   `cseTsbRequest(rmc->ptix, rmc->code, ...)` — a **note** in the requesting
   player's own per-character bank, `TSB_PLxx[65]`
   (`src/sf33rd/Source/PS2/cseDataFiles/`), bound by `color3rd.c` ->
   `q_ldreq_color_data` case 5 via `cseTSBDataTable`.

**The audio samples are PS2 assets.** They come from `SF33RD.AFS`, not from the
CPS3 romset — this port takes char/command data from CPS3 and *all* audio from
the PS2 data. So there is no "arcade sound bank" for an arcade code to be
correct against; the PS2 same-cell value is the only available oracle.

### 21.3 The decisive structure: two codes are equivalent iff equal mod 32

`sdcode_conv[1024]` (`se_data.c`), parsed in full:

| range | structure |
|---|---|
| `[0, 0x160)` | ad-hoc common-SE map — `0x2xxx`/`0x3xxx` SE banks, `0x8001`–`0x8043` BGM, `0x7000` = driver-nop |
| `[0x160, 0x3E0)` | **exactly `(c - 0x160) % 32`** |
| `[0x3E0, 0x3E5)` | `0x3058`–`0x305C` |
| rest | `0x7000` |

**Re-verified independently of the audit tooling** by parsing `se_data.c` and
asserting the closed form across the whole range: *0 exceptions*.

The consequence is load-bearing and non-obvious: for voice codes the
**32-block index in the code is discarded**. Only `(c - 0x160) % 32` survives,
as a note 0–31 in the character's own TSB bank (`+32` when metamorphosed, which
matches `charset.c` -> `check_xcopy_filter_se_req`, whose guard reads
`if ((voif = wk->cg_se) < 0x160)`).

**Therefore an arcade/PS2 code pair that differs numerically is a no-op if the
two are equal mod 32.** Two of the apparent divergences are exactly that, and
collapse to nothing. Any future pass that diffs raw codes without reducing mod
32 will over-report.

### 21.4 Method: cell alignment is not sufficient, and three oracles were needed

The first-pass result (37 diverging cells) came from aligning arcade and PS2
cells **by index**, which is only valid when a script has the same shape on both
sides. That method cannot see divergence in any script whose shape differs, and
cannot see a script that has no PS2 counterpart at all.

Three oracles were therefore used, in increasing order of independence:

- **(a) per-script sound-event multiset** — compare the *bag* of events a script
  emits (L-cell `se >> 4` plus `comm_sse` args) irrespective of position. A
  script whose multiset matches is clean even if cells moved.
- **(b) alignment on non-sound invariants** — duration, `cg_olc_ix`, flags.
- **(c) per-character cast-wide code-set diff** — the decisive one. A code the
  arcade data uses that the character's PS2 data uses *anywhere* is
  namespace-valid even if content moved between scripts. Verified pattern: Alex
  `0x3A7` is arcade-only in `nmca[27]/[28]` and PS2-only in `dmca[90]/[91]` —
  the same content re-laid-out, not a namespace error.

Tooling: the repo's own `tools/arcade-audit/cg_audit.py` parsers, imported
**unmodified**, against a `rom.bin` regenerated by the repo's `decrypt.py` and
against `SF33RD.AFS`. The first pass's 37-cell result reproduces exactly under
the new scripts, which is what validates the shared tooling.

### 21.5 The oracle gap was 478 scripts, not 15

Cast-wide, all 10 script tables × 20 characters:

| | count |
|---|---|
| Total arcade scripts | 14,334 |
| Cell-diffable (shape matches) | 13,856 |
| **Shape-mismatched** | **316** |
| **Arcade script with no PS2 counterpart** | **162** (101 contain sound events) |
| PS2-extra scripts | 0 |

Per character, `shape-mismatch + no-PS2-counterpart`:

GILL 8+43 · ALEX 21+0 · RYU 14+0 · YUN 31+7 · DUDLEY 16+0 · NECRO 16+0 ·
HUGO 15+2 · IBUKI 21+0 · ELENA 13+10 · ORO 14+1 · **YANG 30+99** · KEN 9+0 ·
SEAN 9+0 · URIEN 21+0 · AKUMA 15+0 · CHUNLI 9+0 · MAKOTO 15+0 · Q 15+0 ·
TWELVE 11+0 · REMY 13+0.

Yang's 99 no-counterpart scripts are all in `atca` and **all carry sound
events** — the single largest blind spot, and invisible to cell-index diffing.

**Verdicts for all 478.** 214 of the 316 mismatched are provably clean by
oracle (a) (most have zero sound events). The remaining 102, plus the 101
no-oracle-with-sound, are all resolved by oracle (c). Yang's 99 `atca` scripts
use only `{0x284–0x288, 0x10C–0x10E}`, every one of which PS2 Yang also uses →
namespace-valid, no action. **Zero scripts remain undecidable.**

Makoto's 15 specifically: 11 provably clean (`dmca` 3/84/85/88/89 have zero
sound events; `atca` 150–155 have one event each, multiset-equal). 4 divergent
but namespace-safe: `nmca[27]/[28]` emit `0x1C8`, which PS2 Makoto uses
elsewhere; `dmca[90]/[91]` — PS2 emits `0x1C4` in cells the 2-cell arcade script
does not have (a genuine data difference; nothing to fix under arcade fidelity).
**No namespace miss beyond `saca` 28–31.**

### 21.6 Negative result: 781 cells are converter artifacts, not divergences

This is the finding that explains most "shape mismatch" and every phantom code,
and it is recorded here so no future pass re-opens it.

> **The *conclusion* is confirmed; the *number* is not. See §22.10.** The fifth
> pass's successor (§22) independently confirmed that these regions are
> converter artifacts and must not be diffed — it hit the same trap and had to
> solve it — but reproduced only **258** of the 781 under a stricter
> byte-aligned predicate. §22 also supersedes the *method*: a multiset compare
> over these regions **fabricates values**, because the two sides read them on
> different cell grids, so §22 uses a grid-independent u16-stream compare
> instead. Treat "781" as resting on this section's derivation alone, and
> prefer §22's method for any new audit.

781 cells cast-wide (Yang 337, Dudley 100, Urien 77, Gill 65, …) have the
property that the **arcade BE u32 equals the PS2 LE u32 with its u16 halves
crossed**. Capcom's PS2 converter byte-swapped these 4-byte blocks as **u32s**,
whereas genuine cells got per-u16 swaps plus the att/hit reorder. Such blocks
read as C-vs-L cell-kind flips — hence "shape mismatch" — and manufacture
phantom codes on *both* sides (arcade `0xA00`/`0xC00`/`0xE00`; PS2
`0xD9D`/`0xF05`/`0xF9C`/…).

Three independent arguments that they are dead data:

1. Every such cell sits **past its script's first terminator command** —
   verified for all 26 affected scripts.
2. **No script-command jump reaches any of those regions** — scan of codes
   3, 4, 5, 16–31, 46, 47, 69, 102 per character. The one interior entry found,
   Yang `cbca[47]` cell 3 (`code=22` -> `saca[44]` pat 23), lands on cell 22,
   which is `comm_jmp saca[75]` and immediately redirects.
3. The converter's own blanket-u32 treatment is evidence that **Capcom's script
   walker considered them non-cell data**.

Same verdict for all 31 arcade cells carrying a non-random `se >= 0x400`
(`0x400`, `0x4C8`, `0x600` — the only three values that occur).

> **The verdict stands; the proof recorded here does not, and has been
> replaced.** What this paragraph originally argued was "100% of them sit
> inside these dead regions, zero occur in a shape-ok script" — i.e. it
> inherited the 781-cell census above *and* argument 1's criterion, "past the
> script's first terminator". Both supports have since gone: §22.10 reproduced
> only **258** of the 781, and §26.10.2 showed the terminator criterion is a
> LINEAR scan of a format whose jumps carry a cell index, so it is not a
> reachability test at all. A verdict resting on two withdrawn supports is not
> a verdict, however right it turns out to be. It was re-derived from scratch
> (§28), and the sentence above is what survives.

**Re-derived, and now re-derived on every run.** `cg_audit.py` computes the
`dead` flag on each `a_se_oob` row from `k7_entry_walk` — an entry-point
closure over the successor graph, not a linear scan — and splits the `se`
column into live and dead. The column reads `0+31` for the whole cast: **31
violations, 0 of them on a cell any entry point can reach.** The closure was
first run this way at `db9c8b02` and is strictly stronger now that §28 has
added the six intra-script writers of `cg_ix`, which can revive a cell from
behind; the 31 stay dead under both.

Three facts about the same 31 rows, each measured rather than inherited:

- They occupy **15 distinct ROM byte locations** — 8 in `saca` and 7 in
  `atca`, so this is not the all-`atca` set the original write-up implies.
  The 31 rows exceed the 15 locations because several scripts share one body
  (Yun `atca[118]`/`[119]`/`[130]`/`[131]`/`[142]`/`[143]` are six entries onto
  one cell).
- **PS2 mirror audit: 0 cells.** Running the identical predicate over the PS2
  side of all 20 characters finds nothing, so this is *not* the §6.1
  "identical on both sides, therefore out of scope" case.
- The asymmetry is **grid, not content.** Aligning by `(table, script, cell)`
  — the PS2 script's own entry offset plus the same cell index and stride —
  and comparing the arcade **BE** u32 against the PS2 **LE** u32: **7 of the 15
  are bit-identical**, e.g. Gill `atca[15]` c30 is `02 C4 60 00` on the arcade
  side and `00 60 C4 02` on the PS2 side, the same four bytes reversed, which
  is exactly the blanket-u32 converter signature this section is about. The
  **8 that differ are all Yun's**, whose PS2 spans do not line up.

So the verdict no longer depends on the 781 census or on the terminator
convention, and it is checkable from `cg_audit.json` (`"cls": "a_se_oob"`,
`"dead": true`) rather than from prose.

### 21.7 Results: six live divergences, 29 cells

TSB slots read from `src/sf33rd/Source/PS2/cseDataFiles/TSB_PLxx.c`. The
"notes" column is `(code - 0x160) % 32` on each side — the only part that
reaches the driver (§21.3).

| Char | arcade → PS2 | notes | cells | current symptom |
|---|---|---|---|---|
| MAKOTO | `0x27E` → `0x1DF` | 30 → 31 (`TSB_PL16` note 90 vs 91) | 4 | wrong voice line |
| ALEX | `0x2FB` → `0x3BF` | 27 → 31 (`TSB_PL01[27]` is `cmd=0`) | 4 | **silent** |
| ORO | `0x2F8` → `0x25C` | 24 → 28 (`TSB_PL09[24]` is `cmd=0`) | 13 | **silent** |
| ORO | `0x3DF` → `0x25D` | 31 → 29 (note 79 vs 80) | 2 | wrong voice |
| YANG | `0x1DF` → `0x29E` | 31 → 30 (note 88 vs 87) | 5 | wrong voice |
| AKUMA | `0x37E` → `0x130` | voice note 30 → common SE (`Se_Let`) | 1 | wrong class |

**Independently re-verified** (parsed straight from the C tables, not via the
audit scripts): `sdcode_conv[0x27E]=0x1E` / `[0x1DF]=0x1F`; `TSB_PL16[0x1E]`
note 90 / `[0x1F]` note 91; `TSB_PL01[27]` `cmd=0` and `[31]` note 79;
`TSB_PL09[24]` `cmd=0`, `[28]` note 78, `[31]` note 79, `[29]` note 80;
`TSB_PL06[31]` note 88 and `[30]` note 87.

**Alex's and Oro's cells are currently silent, not merely wrong** — their arcade
codes resolve to TSB slots with `cmd = 0`. That is a more visible defect than
Makoto's and was not in the original report.

### 21.8 The offset is not systematic — no general translation exists

Deltas across the six pairs: **−0x9F, +0xC4, −0x9C, −0x182, +0xBF, −0x24E.**
No constant, no piecewise-linear structure, and one pair (Akuma) crosses out of
the voice range into the common-SE map entirely. **A general arcade→PS2 sound
translation function cannot be derived from this data**, and could only come
from CPS3 sound-driver ground truth, which this port does not have (§21.13).

Two hard disqualifiers for any *global* code→code table:

- `0x2FB` is a **legitimate Urien voice**, used by Urien on both sides.
- `0x3DF` is a **legitimate Twelve voice**, likewise.

A global remap of either breaks those characters. The correct shape is a
**per-character exception table**, matching `remap_cg_number`'s existing
`CgRemapRange` model.

### 21.9 Deliberate non-actions (each one a decision, not an oversight)

- **YANG `0x27F` → `0x29F`** — 31 → 31. Mod-32 equivalent. **No-op.**
- **YUN `0x268` → `0x288`** — 8 → 8. Mod-32 equivalent. **No-op.**
- **YANG `0x269` → `0x29C`** — 9 → 28, and `TSB_PL06[28]` is an **empty slot**:
  PS2 *silenced* a voice CPS3 plays. Yang also uses `0x269` correctly elsewhere
  on both sides, so even a per-character per-code remap would break those cells.
  Arcade fidelity favours leaving it. **No remap.**
- **Q `caca[4..7]`** — authentic silence, see §21.10.
- **Shoto `dmca[3]` SE `0x10A`** — a valid common SE (`0x200E`) that arcade
  plays and PS2 removed. Arcade-faithful as-is. **No action.**

### 21.10 Negative result: Q's silent cells are authentic arcade data

`caca[4..7]` are Q's 34-cell caught/throw-reaction scripts, entered by the
engine's catch flow (zero script jumps reference them). Arcade and PS2 are
**identical in every field** — commands, durations, `olc`, and `cg_number`
modulo Q's constant remap delta — **except cell 1's `se`: arcade `0x0000`, PS2
`0x1080`**. The code is not hiding in another field or another cell.

Cross-cast, **16 of 20 characters carry `0x1080` in that same cell on _both_
sides** (Ryu, Ken, Sean, Akuma, Ibuki, Yang, …) — it is the common
caught-reaction sound (`0x108` -> `Se_Let`). Q is the only character where
arcade is silent and PS2 added it.

Under this document's arcade-fidelity contract: **arcade-silent is the
authentic CPS3 data. No action.** (Whether CPS3's *audible* result was silence
cannot be proven statically — §21.13.)

### 21.11 Ingress paths: exactly three, two verified clean

Every field in the arcade char data that indexes a sound namespace:

1. **L-cell `cg_se`** — byte-passed (§21.2). **The bug.** 29 live cells.
2. **`comm_sse` args** — `charset.c`, `decode_chcmd[116] = comm_sse`, which does
   `wk->cg_se = ctc->koc;` with **no `>> 4`**. Cast-wide: **73 cells, 0
   divergences.** Clean. (This extends the first pass's Makoto-only check to the
   whole cast.) Note `comm_scmd` (`decode_chcmd[112]`) writes `wk->cmd_request`,
   which **has no reader anywhere in `src/`** — not a sound path.
3. **SERND** — `read_sernd()`, byte-passed, consumed as `wk->se_random_table`
   (`charid.c`). **Re-verified directly rather than trusted from §15.3**: all 20
   characters' named spans identical (25/25 entries; rebased offsets and all 16
   u16 candidates each). The legitimate random codes `0x800`–`0x803` resolve
   through absolute CPS3 pointers into the named span with byte-identical
   candidate tables arcade-vs-PS2 for every character. Clean.

No other section feeds sound: the rest are movement/hitbox/catch data per
§15.2, and L-cell `cg_eff`/`cg_eftype` spawn effects whose sounds are code
constants, not data indices.

**One residual door (new, unrelated to the six pairs).** `se_random_table` is
`u32*` and its first-level index is `cg_se & 0x7FF` (`charset.c`), **unbounded**.
The phantom dead-region codes of §21.6 (`0xA00`/`0xC00`/`0xE00` → idx
0x200/0x400/0x600) would read reinterpreted other-section data and then dispatch
`sound_effect_request[<unbounded u16>]`. Every known occurrence is dead data
(§21.6), and the audit's `se_oob` check deliberately skips the random path — but
this is an unguarded index of exactly the kind §8.C and §8.L exist for. Folded
into those items rather than given its own.

### 21.12 Fix site: parse-time, and why the alternative is disqualified

**Site: `arcade_char_data.c` -> `read_char_table()`, beside
`remap_cg_number()`**, applied to the **upper 12 bits** of `cg_se` (the low
nibble is flip/priority — §21.2 — and must be preserved).

The post-digest alternative, remapping inside `sound3rd.c` ->
`remake_sound_code_for_DC`, is disqualified three independent ways:

1. **No character context exists there** — and `0x2FB`/`0x3DF` collide with
   legitimate Urien/Twelve voices (§21.8). The remap would be unconditional and
   would break them.
2. It runs **after** the `+ uid * 0x300` / `+ 0x600` player adjustments and is
   shared by menus and the announcer.
3. It would leave the **dispatch entry** wrong. `sound_effect_request[0x25C /
   0x25D / 0x130]` is `Se_Let`, while the arcade sources sit on `Se_Myself` —
   different guards. Only fixing `cg_se` itself reproduces PS2 dispatch exactly.

### 21.13 Netplay: the digest bump is correct behaviour, and the remap is rollback-safe

`ArcadeCharData_ComputeDigest()` hashes the **parsed, post-adaptation** spans
(its own comment: "Run AFTER the full 20-character adaptation"). A parse-time
`cg_se` remap therefore changes the digest.

**That is the mechanism working as designed, not a cost.** Sound is *not*
purely presentational upstream of the driver: `wk->cg_se` lives in `WORK`/`PLW`,
which is rollback state (`game_state.c` -> `GS_SAVE(plw)`), and the `0x800`
random path consumes `random_16()`, whose index is saved *and hashed*. So
mixed-build sessions genuinely would desync — and the digest, carried in the
MIST handshake (`arcade_balance.h`: "Carried in the MIST netplay handshake so
peers with differing adapted data reject instead of desync"), makes them
**reject with `MIST_REJECT_BALANCE_MISMATCH` instead**.

Everything from `SsRequestPan` down *is* presentational: `game_state.c` saves no
sound-driver state, and `Store_Sound_Code` writes only the `sdeb[8]` debug ring.

**None of the six source or target codes carries `0x800`**, so RNG consumption
is unchanged by the remap — it is rollback-safe. Release-note this under §8.O
alongside the other digest-changing fixes.

### 21.14 What this analysis does not establish

- **CPS3 sound-driver ground truth.** What each arcade code *audibly* played on
  real hardware is unknown; the audio SIMMs are not part of this port's romset
  usage. The PS2 same-cell value is used as the oracle throughout, and it is a
  proxy, not a primary source. No sample was decoded — the maintainer explicitly
  scoped audio decoding out.
- **Exhaustive runtime reachability of the 781 converter-mangled cells**
  (§21.6). The verdict rests on linear-prefix position + a jump-target scan +
  the converter's own treatment. It is not full control-flow simulation, and
  `rja`/`uja` conditional-jump argument semantics are not fully modelled. Same
  epistemic status as §20.5's "apparently unreachable".
- **Whether CPS3 Q's throw-reaction silence was intentional authoring** or an
  omission Capcom corrected for the PS2 release (§21.10).

---

## 22. `cg_zoom` and `cg_effect`/`cg_eftype` — the byte-pass list audited to the end (sixth pass, 2026-09-02)

**Citation style for this section.** As in §21: this document is *not* in
`tools/doc-citations/baselines.txt`, so line numbers are not enforced and are
not hand-maintained here. Everything below cites a **symbol** (`file` ->
`function`/`table`) or the **exact text** of a line, read at `f7f63055`
(item Q's commit; none of the files cited below were touched by it, verified
via its `--stat`).

**Headline: both fields are clean.** There is no item-Q-class defect in
`cg_zoom` or in `cg_effect`/`cg_eftype`. No cell anywhere in the 20 characters
gives the camera a different zoom *level* under arcade balance than under PS2
balance, and every effect index the arcade data dispatches is an index the
same character's PS2 data also dispatches (three argument-value exceptions,
each verified harmless, §22.5). The rest of §4.4's byte-pass list was swept in
the same run; every remaining field is either clean, self-consistent by
construction, or an already-documented balance difference (§22.7). **No new
worklist item is needed.** The value of this pass is the recorded negative
result — and one new structural fact (§22.6) plus one artifact variant §21.6's
criteria do not cover (§22.9).

### 22.1 Why this pass happened

§4.4's callout (added with §21) says it directly: after `cg_se` turned out to
be a live defect, "byte-passed" could no longer be read as "safe" for any
field, and it names `cg_zoom` and the `cg_eff`/`cg_eftype` pair as never
having had a value-level arcade-vs-PS2 diff. `cg_zoom` was additionally timely:
a separate investigation had just traced super-move screen zoom end-to-end
(`charset.c` -> `setupCharTableData` bulk-copies the frame record into WORK;
`bg_sub.c` -> `check_cg_zoom` reads `plw[0].wu.cg_zoom` / `plw[1].wu.cg_zoom`
and sets `zoom_request_flag` / `zoom_request_level`; `bg_sub.c` ->
`zoom_ud_check` steps `bg_w.bg_f_x` toward `bg_w.frame_deff = 64 -
zoom_request_level`; `bg.c` -> `Zoom_Value_Set` does `scr_sc = 64.0f / zadd`),
so a wrong `cg_zoom` would mis-zoom the camera during supers. It does not
(§22.4). The user-visible SEAMS during supers are therefore confirmed to be
**not data** — consistent with their separate root cause (per-chip integer
snapping under zoom).

### 22.2 The consumers — what survives of each field

Every link read in code:

- **`cg_effect`** (u8) has exactly one reader: `charset.c` ->
  `check_cgd_patdat`, `if (wk->cg_effect) { effinitjptbl[wk->cg_effect](wk,
  wk->cg_eftype); }`. The **full u8 survives** as a direct index into
  `effxx.c` -> `effinitjptbl[59]`, a fixed engine function table
  (`effect_03_init` … `effect_F0_init`). The index is **unbounded in code**;
  entry 0 is `NULL` but shielded by the nonzero test.
- **`cg_eftype`** (u8) survives whole, twice: as the second argument to the
  dispatched init function above, and in `pls03.c` -> `check_renda_cancel`,
  `wk->wu.cg_ix = wk->wu.cg_eftype * wk->wu.cgd_type - (wk->wu.cgd_type * 2);`
  — the renda-cancel restart cell.
- **A second ingress into the same table** (the analogue of `comm_sse` for
  sound, §21.11): C-cell opcode 43, `charset.c` -> `comm_exec`,
  `effinitjptbl[ctc->koc](wk, (u8)ctc->ix);` — also unbounded, and with **no
  zero guard**: `koc = 0` would call the `NULL` entry directly. (Shipped
  arcade data never does — live `koc` min is 1, §22.5.)
- **`cg_zoom`** (u16) is written into WORK only by `charset.c` ->
  `setupCharTableData` (so only cgd-6 cells ever set it) and read only by
  `bg_sub.c` -> `check_cg_zoom`. What survives: the X-request switch masks
  with `0xE200`, the Y-request switch with `0xD100`, the frozen-camera merge
  reads `>> 8 & 3`, and the low byte is the zoom level (`zoom_request_level =
  p1zoom & 0xFF`, max of the two players). **Bits `0x0C00` are never read.**
  Crucially the level is *arithmetic*, not an index (`64 -
  zoom_request_level`, then `Zoom_Value_Set`'s division) — **no OOB class
  exists for `cg_zoom` at all**, unlike every other field this document has
  audited. A wrong value could only mis-frame the camera, never fault.

### 22.3 Method: §21.4's oracles, plus a fourth that §21 did not need

The three §21.4 oracles were reused (cell-aligned diff for shape-ok scripts;
per-script live multisets for shape-mismatched; cast-wide per-character
value-set diff as the decisive namespace test), with parsers modeled on
`tools/arcade-audit/cg_audit.py`'s `arc_parse`/`ps2_parse` extended to decode
the cgd-6 tail (`cg_zoom`, `cg_rival`, `cg_add_xy`, `cg_next_ix`,
`cg_status`) that `cg_audit.py` skips, against the same regenerated `rom.bin`
(sha256 prefix `c15743e350011f6a` — matches §9) and `SF33RD.AFS`.

**Harness validation** (all reproduced exactly before any new number was
trusted): §21.5's structural counts — 14,334 arcade scripts, 13,856
cell-diffable, 316 shape-mismatched, 162 with no PS2 counterpart, 101 of
those with sound; §21.4's first-pass raw `cg_se` count of 37; and §21.7's six
namespace pairs plus every §21.9 exclusion, re-derived independently by the
cast-wide set diff (the six pairs fall out as exactly the arcade-only live
voice codes, with `0x10A`, Yang `0x269`/`0x27F` appearing and being excluded
for §21.9's recorded reasons). §21.6's 781-cell artifact census was **not**
reproduced (a cruder byte-aligned predicate found 258 of them, Alex + Yang
only); it did not need to be, because of the fourth oracle:

**The grid caveat (new).** §21.6's converter-mangled regions do not merely
*look* divergent under a cell diff — the two sides' parsers read them on
**different cell grids**, so one side reports C-cells where the other reports
L-cells with entirely fictitious field values. A per-script multiset compare
is *also* polluted by this (it was how a first pass of this run briefly
"found" PS2-only zoom levels on Oro, §22.9). The decisive tool is a
**grid-independent u16-stream compare**: read the whole script span as BE u16s
(arcade) vs LE u16s (PS2) and accept a position as equal iff (a) the values
match, (b) the raw bytes match (u8 sub-fields read as u16 flip per-endian),
(c) the u32's two u16 halves are crossed (§21.6's converter signature), or
(d) PS2 == `remap_cg_number(arcade)`. Every equal-length flagged script
resolved to **zero unexplained positions** under this compare (16 scripts:
Dudley `saca[36..39]`, Oro `saca[28..31]`, Remy `saca[63]`, Urien
`atca[24..26]`, Yang `saca[44..47]`) — their apparent eff/zoom differences
are pure grid phantoms. The 99 flagged scripts with *different byte lengths*
are genuine Capcom re-authoring, and for those the cast-wide set oracle is
the namespace verdict.

### 22.4 `cg_zoom`: clean — zero camera-level divergences anywhere

29,887 cgd-6 cell pairs compared cell-aligned. Raw `cg_zoom` divergences: 927.
They decompose completely:

- **924** are the §22.6 cgd-type-mismatch class: the arcade script is cgd 6
  and the PS2 script is cgd 4, so the PS2 cell has no zoom field at all — and
  in **every one of the 924 the arcade value is `0x0000`** (explicit zero vs
  absent field; the only behavioral residue is that arcade data actively
  clears WORK's copy each frame where PS2 data leaves it stale, and no
  nonzero value is ever at stake).
- **3** are real value differences, all one script: **Yun `caca[0]` cells
  23-25, arcade `0x4000` vs PS2 `0x0000`** — the opt-out flag (`case 0x4000`
  contributes no zoom request; it also gates the frozen-camera merge, the
  line `if (bg_stop != 0 && !((p1zoom | p2zoom) & 0x4000))`). The level byte
  is 0 on both sides. Being-thrown framing nuance, not a camera zoom.
- **0** have a differing level byte. Not one, in any script, for any
  character.

Cast-wide (the decisive oracle): **no character has any live arcade zoom
value, under the consumed-bit mask `0xF3FF`, that its PS2 data does not also
use** — the arcade-only set is empty for all 20. The flagged shape-mismatched
scripts all resolved as either §22.3 grid phantoms (including every "PS2-only
zoom level" candidate — §22.9) or genuine flag-duration re-authoring with
level byte 0 on both sides (Yun/Yang `caca` hold `0x4000` for 24 cells vs
PS2's 21; Oro `caca[3]` holds `0x100` five cells vs six; Akuma `saca[48..51]`
hold `0x1000` ten cells vs eleven; Dudley `saca[36..39]`'s extra PS2 `0x2000`
events are phantoms per the stream compare). The two arcade-only scripts with
zoom content and no PS2 counterpart (Gill `saca[61]` all-`0x100`, Oro
`caca[14]` `0x308`/`0x300`) use only values PS2 Gill/Oro use elsewhere.

Census, for the record: 277 arcade scripts carry a nonzero live zoom value.
The corresponding PS2-side count of 279 is inflated by the four Oro phantom
scripts; the shared population is 275, plus arcade's two no-counterpart
scripts above. **Conclusion: byte-passing `cg_zoom` is correct. Super-move
camera zoom under arcade balance is the arcade's own, and it is the same
zoom the PS2 data specifies wherever both specify one.**

### 22.5 `cg_effect`/`cg_eftype`: shared namespace, no `cg_se`-class defect

The namespace question was the point: `cg_se` indexes PS2-authored *data*
(TSB banks), and arcade codes named different notes. `cg_effect` indexes
engine *code* (`effinitjptbl`), and the question was whether CPS3 authored
its indices against a different table order. It did not. Of 93,947 cgd>=4
cell pairs, `cg_effect` differs in 245 (242 live), `cg_eftype` in 207 (206
live) — 0.26% — and **every single divergence is an event added, removed, or
moved by one cell between the two releases; there is no case of the same
event carrying different codes**, which is what a namespace error looks like.
The bulk, by value pair: Q arcade-0/PS2-32 ×104 and Hugo ×34 (that is the
§22.6 mechanism swap, not a lost effect); one-cell moves of 21
(`clear_caution_flag`) on Ibuki ×39; PS2-added 31 (`setup_meoshi_hit_flag`)
across eight characters ×30; PS2-added 22/21/16/58 event additions in
re-authored scripts (`dmca[90]/[91]` cast-wide, Urien/Akuma SA scripts,
Ibuki `yuca`).

The cast-wide arcade-only test leaves exactly **four** items, each resolved:

- **Hugo `(19,2)` ×8, Akuma `(19,6)` ×1 and `(19,10)` ×3** (the only
  arcade-only `(eff, eftype)` pairs in the cast): eff 19 is `effe5.c` ->
  `erase_after_images(PLW*, u8 who)`, whose switch sends `who == 0` to own,
  `1` to target, and **anything else through `default:` to both** — 2/6/10
  are defined behavior, no hazard. PS2 dropped these calls; arcade keeps
  them. Genuine data difference, arcade-faithful as-is.
- **Ibuki `caca[10]`: arcade `comm_exec (koc=20, ix=1)`** — the one dispatch
  in the whole cast to an entry that is a **no-op in this engine**
  (`effinitjptbl[20]` is `effect_dummy_init`). The PS2 re-authoring of the
  same script instead carries an L-cell `(18,5)` — `setup_after_images`.
  Whatever CPS3's entry 20 did is unknowable from this tree (§22.10); under
  arcade balance the port calls a no-op where PS2 balance sets up
  after-images, so one Ibuki caught-reaction may lack an after-image visual.
  Visual-only (effect sounds are code constants, §21.11), not mechanically
  fixable without CPS3 ground truth, and not a byte-pass error — the index
  is faithfully in-namespace. **No action.**
- **Yang `atca[0..2]`: `(21,5)` vs PS2 `(21,4)`** — `effect.c` ->
  `clear_caution_flag(PLW*, u8 /* unused */)` ignores its argument. Inert.

**Bounds, live data:** max live L-cell `cg_effect` cast-wide is **44**; live
`comm_exec` `koc` spans **1..57** over 827 live cells (so the `NULL` entry 0
is never dispatched). Both under the table's 59. Every one of the 53 cells
`cg_audit.py` flags as `a_effinit_oob` (Yang `saca[44..47]` eff 64, Urien
`atca[24..26]` eff 78, Remy `saca[63]` eff 116/117) was re-verified to sit
**past its script's first terminator** (cells 28+/34+/82+ vs terminators at
cells 22/31/20 respectively) — dead-region phantoms, §21.6's class. And the
904 cell-aligned `comm_exec` cells diverge in **zero** operands; `comm_sse`
re-verified at 0 divergences over its 61 cell-aligned cells (§21.11's 73 was
the cast-wide count including shape-mismatched scripts).

### 22.6 New structural fact: 85 scripts have a different `cgd_type` per release

Not previously recorded anywhere in this document: the same script index can
be authored with a different **cell width** in the two releases. 85 scripts
mismatch (79 are arcade-6-vs-PS2-4; plus 2 each of 4-vs-2, 2-vs-6, 2-vs-4),
concentrated in **Hugo (41)** and **Q (32)**. Verifying example, raw headers:
Hugo `nmca[4]` reads `0006` big-endian in `rom.bin` and `0400`
little-endian in the AFS tail — cgd 6 vs cgd 4, same script.

The interesting consequence is a **mechanism swap**: a cgd-4 cell cannot
carry `cg_add_xy`, so where the arcade authored cgd-6 cells with per-cell
step offsets (138 nonzero `cg_add_xy` values across 35 of these scripts —
Hugo `saca`/`cbca`, Q `nmca`/`atca`/`saca`), the PS2 re-authoring uses cgd-4
cells plus eff-32 `exec_char_asxy` events — and **both consumers read the
same STXY table**: `charset.c` -> `check_cgd_patdat` does `from_rom2 =
wk->step_xy_table + wk->cg_add_xy` and `effect.c` -> `exec_char_asxy` does
`from_rom2 = &wk->step_xy_table[ix]` with `ix = data * 2`. This is what the
Q ×104 / Hugo ×34 "arcade 0 vs PS2 32" eff rows in §22.5 are: not a missing
effect but the same movement expressed through the other door. Each side is
self-consistent (arcade `cg_add_xy` indexes the raw-installed arcade STXY).
Whether the two encodings produce identical trajectories was not verified
(§22.10).

### 22.7 The rest of the byte-pass list — negative results, one line each

Swept in the same run, cell-aligned live cells, so the next pass does not
re-do them:

- **`cg_extdat`**: 0 divergences. **`cg_status`**: 0 divergences.
- **`cg_next_ix`**: exactly **one** cell cast-wide — Alex `caca[19]` cell 8,
  arcade 9 vs PS2 0 (byte-verified in both containers; consumer is
  `charset.c` -> `check_cm_extended_code`, `wk->cg_ix = (wk->cg_next_ix - 1)
  * wk->cgd_type`). A genuine one-byte Capcom difference in a caught-script
  loop-back; arcade-faithful as-is.
- **`cg_rival`**: 3,023 divergences, and **100% of them satisfy `arcade * 5
  == ps2 * 6`** — the RICT 24-vs-20 row stride already established in §15.4
  (four dead opponent slots per group, §8.H). Zero anomalies outside the
  stride law. Self-consistent: arcade `cg_rival` indexes the raw-installed
  arcade RICT, with `charset.c` -> `catch_table_offset` already adapting the
  character base under arcade balance.
- **`cg_olc_ix`**: 296 divergences — genuine overlap-selection data
  differences; the namespace is the character's own raw-installed OVIX, so
  no cross-universe indexing exists to be wrong.
- **`cg_hit_ix` / `cg_att_ix`**: 9 / 12 value divergences — arcade-vs-PS2
  frame-data balance, §15's territory; namespaces are the raw-installed
  HIIT/ATTA-ATIT.
- **`cg_cancel`**: 114 divergences — cancel-window balance differences
  between the releases; expected and arcade-faithful.
- **`cg_add_xy`**: beyond the §22.6 mechanism-swap class, no anomaly.

### 22.8 Residual unguarded indexes — folded into §8.C / §8.L, like §21.11's

Two more indexes of exactly the class §8.C and §8.L exist for, recorded here
rather than given a worklist item: `effinitjptbl[wk->cg_effect]` and
`comm_exec`'s `effinitjptbl[ctc->koc]` are both unbounded, and `comm_exec`
additionally lacks the zero guard that protects the L-cell path from the
`NULL` entry. Live shipped data never exceeds 57 or reaches 0 (§22.5); the
dead-region phantoms carry 64/78/116/117 and would OOB-read a function
pointer **if** anything ever entered those regions — the same static
"apparently unreachable" status as §21.11's `se_random_table` door.

### 22.9 Negative result with a twist: Oro `saca[28..31]` are a §21.6 variant the terminator criterion misses

§21.6's artifact criterion says mangled cells sit *past their script's first
terminator*. Oro `saca[28..31]` break the letter of that rule while
confirming its spirit, and are recorded so the next pass does not re-fight
them. The script is: 4 L-cells, then `comm_for` (opcode 12, **not** a
terminator), then bytes that the engine's uniform 24-byte cgd-6 grid reads —
per-endianness — as two different things: the arcade/BE side sees an endless
run of `comm_dummy` cells (`decode_chcmd[0]`, returns 1), the PS2/LE side
sees blank L-cells carrying zoom `0x300` and a level-13 cell. Those "PS2-only
zoom levels" were the loudest false positive of this whole pass. The
grid-independent stream compare (§22.3) proves the two containers are
**value-identical mod the CG remap and the §21.6 crossed-halves signature**
— same bytes, two grids. The authored content (visible off-grid: an animation
loop between `comm_for` and a `comm_nex` that sits 8 bytes off the engine's
cell boundary) is unreachable by either engine's actual walk. Reachability
was checked both ways: **no SA-table slot selects Oro `saca[28..31]`**
(`sa_labels` over `asstbl.c`'s `asstbl_lv_9900_g/a_arcade` rows — slots 24-27
and 32-33 are named; 28-31 are not), and an over-broad §20.3-style operand
scan over all of Oro's ten tables finds **zero references** to `(koc=5, ix
28..31)`. Same epistemic status as §20.5. The practical lesson survives:
**"past the first terminator" is a sufficient but not necessary artifact
signature; the stream compare is the reliable oracle.**

### 22.10 What this analysis does not establish

- **CPS3 ground truth for `effinitjptbl` entry 20.** Whether CPS3 rendered
  something (an after-image setup, per the PS2 re-authoring's substitute) at
  Ibuki `caca[10]`'s `comm_exec (20,1)` cannot be known from this tree; the
  PS2 engine's entry is a deliberate dummy. The port faithfully reproduces
  the PS2-engine reading of the arcade data, which is the only defined
  behavior available.
- **Trajectory equivalence of the §22.6 mechanism swap.** That `cg_add_xy`
  (arcade cgd-6) and `exec_char_asxy` (PS2 cgd-4) read the same STXY table
  is verified; that the 35 affected scripts produce identical motion was
  not simulated. Under arcade balance the arcade encoding runs, which is the
  fidelity contract, so nothing hinges on it.
- **Execution behavior if a mis-gridded region were ever entered** (§22.9):
  the `comm_dummy` march past script end was reasoned about, not executed.
  Reachability rests on the same static-scan grounds as §20.5.
- **§21.6's 781-cell census was not independently reproduced** (258 of the
  781 were, under a stricter byte-aligned predicate). The stream compare
  supersedes the census for every script this pass needed to decide, but the
  number 781 itself still rests on §21.6's derivation alone.
- **The frozen-camera merge path** (`bg_stop != 0` in `check_cg_zoom`) was
  traced for which bits it reads, not exercised; the Yun/Yang `0x4000`
  three-cell differences were classified by mechanism, and their on-screen
  visibility was not confirmed on device.

---

## 23. `Random_ix16` stage-init divergence — CPS3 spawns two stage effects the port never does (seventh pass, 2026-09-03)

**Citation style for this section.** As in §21/§22: this document is not in
`tools/doc-citations/baselines.txt`; everything below cites a **symbol**
(`file` -> `function`/`table`) or the exact text of a line. Port code was read
in the worktree `/Users/sb/Developer/3sx-mister-wt-descope`, branch
`feat/replay-descope` @ `2ab572e1`. CPS3 ground truth is the **SH-2 program
of the `sfiii3nr1` romset itself** (§23.4 says how it was obtained); CPS3 code
is cited by its ROM virtual address (`CPS3 0x0611E0EE`), which is stable for
that romset and is the only durable anchor a stripped binary has. Everything
marked **measured** was run; everything marked **inferred** was not, and says
what it rests on.

**Headline.** The port consumes `random_16()` differently from the arcade on
exactly two stages — **Club Metro (`bg190`, Remy's) and the Shopping District
(`bg030`, Yun's)** — because the CPS3 stage initialisers for those two stages
spawn one extra stage effect each (**effect id 74** on Club Metro, **effect id
8** on Hong Kong) whose move routines call the arcade `random_16`. The port's
`bg1902_init00` / `bg0301_init00` do not spawn them, and *cannot*: in the
PS2-derived engine, ids 8 and 74 dispatch entirely different effects
(§23.8). Everything downstream of `Random_ix16` on those stages — both
players' intro variants, effect picks, the AI pattern picks in `plpat09.c`, the
dizzy duration lookup in `plpdm.c` — sees an index the arcade never had, on
every round played there, in every mode. The mechanism was named from the
arcade disassembly (§23.5–§23.6) and then **reproduced exactly** for all four
affected replays that have traces: a frame-level model of the two arcade
effects predicts the archive's `Random_ix16` at frame 60 to the value, and
Remy's intro X position to the pixel, in 4/4 cases (§23.7). The fix is
bounded and is proposed, not applied (§23.10).

### 23.1 Why this pass happened

The replay viewer (`src/replay/replay_player.c`) plays FBNeo-recorded `.3sr`
files and compares 13 live engine fields against per-checkpoint djb2 hashes
that were computed **from real CPS3 main RAM** (format: `docs/3sr-format.md`
§4). After the viewer's own desync fixes (`2ab572e1`), the previous pass
reported **42 of 44 real replays clean**. Two fail — both at **frame 60, the
first battle checkpoint**, both with **P2 = Remy** on Remy's stage:

| replay | players (chars) | header `ix16` | live @f60 | archive @f60 (hash-solved) | live P2 X | archive P2 X |
|---|---|---|---|---|---|---|
| `1784868362963-5027` | Ken (11) / Remy (19) | `0x1A` | `0x1D` | **`0x1F`** (+2) | 552 | **577** (+25) |
| `1784868387508-5268` | Makoto (16) / Remy (19) | `0x16` | `0x19` | **`0x1A`** (+1) | 577 | **552** (−25) |

The 44-replay tally (42/2) and the "only Remy's and Yun's stages drift by
checkpoint 2" observation are the previous pass's; this pass re-parsed the 16
replays whose run logs survive in its scratchpad and found the same partition
(the 4 replays on stages 19 and 3 drift; the 12 on seven other stages do not). The hash solves in
the table were **re-derived here** (§23.3), not copied.

### 23.2 Two facts about the measurement chain that this section depends on

**(a) The publish gate has never been able to see this class of defect.**
*(Superseded 2026-09-05: `statcheck_compare.c` now asserts `Random_ix16`, and
what that unmasked is `research-arcade-balance-desyncs.md` §E5. The paragraph is
kept as written because everything §23 measured was measured under the mask.)*
`src/test/statcheck_compare.c` (and `src/test/test_runner_compare.c`,
identically) does not *assert* `Random_ix16` — it **overwrites** it from the
archive every frame:

> `// This is dirty, but syncing Random_ix16 every frame helps avoid animation-related desyncs`
> `Random_ix16 = random_ix16_cps3;`

`Random_ix32` is asserted (`assert_equals(Random_ix32, random_ix32_cps3)`), and
the corpus passes it at ~93% (previous pass's figure), so the animation cell-walk that consumes
`random_32` is faithful. But any port-vs-arcade difference in *how many times*
`random_16()` is called has been silently repaired every frame by the oracle
for as long as the oracle has existed. The replay viewer is the first tool
that runs the engine against arcade truth **without** that sync — which is why
a defect that is present in every Club Metro round ever played on the port
surfaced only now, as a replay bug.

**(b) The viewer's `Random_ix16` recovery is exact, so the drift numbers are
data.** `replay_player.c` -> `probe_random_ix16` (named `recover_random_ix16`
when this pass ran) sweeps the 16-bit field
ascending and accepts the first hash match. Its own comment calls this
"best-effort" and says "the low bits of the applied value may differ" — that
is too pessimistic. The 13 fields are hashed as 26 little-endian bytes with
`djb2_hash.h`'s `h = h*33 + byte`; `Random_ix16` occupies bytes 10–11, so two
candidates collide iff `33^14 * (33*Δlo + Δhi) ≡ 0 (mod 2^32)`; `33^14` is odd
hence invertible, so `33*Δlo + Δhi = 0` exactly, i.e. `Δhi = −33*Δlo` with
`|Δlo| ≤ 7`. Colliders of a value `v` are therefore exactly `v − 8447*k`.
**Measured** (a full 65,536-candidate sweep against both failing checkpoints):
colliders at `[26, 8473, 16920, …]` and `[31, 8478, 16925, …]`, spacing 8447
throughout. Because the true index is `≤ 0x3F` (`random_16` masks with `&=
0x3F`; CPS3's does the same, §23.4) the smallest collider *is* the archive
value, and the ascending sweep's first match is exact. (The stale comment
lives in `replay_player.c`, which this pass was not permitted to edit.)

### 23.3 The port side: what the failing checkpoint actually says (measured)

Solving each failing checkpoint over `Random_ix16 ∈ [0, 63]` × `P2 X ± 400`
against the archive hash gives **exactly one** solution each, and solving over
`P1 X` instead gives none — so the archive differs from the live engine in
precisely two fields, `Random_ix16` and P2's X, and in nothing else:

- 5268: live `(ix16=0x19, P2X=577)` — archive `(0x1A, 552)`;
- 5027: live `(ix16=0x1D, P2X=552)` — archive `(0x1F, 577)`.

25 px is Remy's intro. `animation/appear.c` -> `Appear_34000` (Remy:
`appear_data[45].rno == 34`) does `work = random_16(); work &= 7;` and, for
`work ∈ {0, 2, 6, 7}`, places P2 at `bg_w.bgw[1].pos_x_work + 0x71` (113);
otherwise the table's `hx = 88` stands. `pos_x_work` on Club Metro is `0x1D0`
(`stage/bg190.c` -> `bg1901_init00`/`bg1902_init00`): 464 + 113 = **577**,
464 + 88 = **552**. So "P2 X off by 25" is "Remy drew a different `work`", and
the only way that happens with identical inputs is a different `Random_ix16`
when `Appear_34000` runs.

The live trace for 5268 (`scratchpad/trace-1784868387508-5268.log`,
per-frame `random_16` counters with call sites) shows the port's whole
`Random_ix16` history up to the checkpoint: **f=0: 1 call (`ta0_init00`); f=1:
0 calls; f=2: 2 calls (`Appear_34000`, `Appear_01000`); f=3..60: 0 calls.**
With `random_tbl_16` (`engine/pls02.c`) that is: header `0x16` → f0 `0x17` →
Remy draws index `0x18` → `tbl[24] = 0` → `work = 0` → 577 (live, correct).
One extra call anywhere in f0..f2 before Remy's draw shifts Remy to `0x19` →
`tbl[25] = 3` → 552 — the archive's value. For 5027 the same arithmetic gives
live `0x1C → tbl[28] = 1 → 552` and, with one extra call, `0x1D → tbl[29] = 7
→ 577` — again the archive's. In both replays, therefore, **CPS3 consumed
exactly one `random_16` more than the port between frame 0 and Remy's intro
draw** (and, in 5027, one more between f=2 and f=60). That is the entire
defect; everything after this is finding the consumer.

### 23.4 CPS3 ground truth: the pipeline worked, and here is the RNG (measured)

Upstream carries a decrypt-and-split pipeline: `crowded-street/3sx` @
`12eaa105` ("Add cps3 decryption and splitter to dump asm") adds
`tools/combine-and-decrypt.py`, `config/cps3/sfiii3n.yaml` and a
`tools/saturn-splitter` submodule. The local `/Users/sb/Developer/3sx`
checkout is on `flatpak-workflow2` and does **not** contain those files; they
were recovered with `git show 12eaa105:<path>`. The splitter (Rust) was not
needed: the decrypt script is pure Python (the `cps3_mask` XOR with keys
`0xA55432B4`/`0x0C129981` — the same `KEY_1` the port's own
`src/arcade/cps3_decrypt.c` uses), takes the four `sfiii3-simm1.[0-3]` files
from `~/Library/Application Support/CrowdedStreet/3S-ARM/roms/sfiii3nr1.zip`,
and produces an 8 MB big-endian image in 3 s. Its first words are an SH-2
vector table (`PC=0x06000EA0, SP=0x02008F94`), and disassembly used capstone
5.0.7 in `CS_MODE_SH2 | CS_MODE_BIG_ENDIAN` (scripts in the pass's scratchpad:
`cps3/sh2.py`, `callers.py`, `fn.py`, `spawn.py`, `sim.py`).

Locating the RNG needed no symbols. `random_tbl_16` (the 64 `s16` values in
`pls02.c`) occurs exactly once in the image, at **`0x065EB434`**;
`random_tbl_32_ex` once, at `0x065EB4B4`. The CPS3 address of `Random_ix16` is
**`0x020155E8`** — which is `RANDOM_IX_16_OFFSET 0x155E8` from
`src/arcade/arcade_constants.h` plus the CPS3 work-RAM base, an independent
cross-check of the statcheck offsets. The one literal pool holding *both* that
address and the table's address belongs to:

```
CPS3 0x0611E0EE  random_16:
  mov.l  [0x020155E8],r4      ; &Random_ix16
  mov.l  [0x065EB434],r1      ; random_tbl_16
  mov.w  @r4,r3 ; add #1,r3 ; mov.w r3,@r4
  mov.w  @r4,r0 ; and #63,r0 ; mov.w r0,@r4
  shll r0 ; rts ; mov.w @(r0,r1),r0
```

i.e. `Random_ix16++; Random_ix16 &= 0x3F; return random_tbl_16[Random_ix16];`.
The port's `Debug_w[0x3B] == -32` reset (`pls02.c` -> `random_16`) has no
arcade counterpart; it is inert unless that debug word is set. The three
siblings at `0x0611E0D6` (`&0x7F`, `Random_ix32` at `0x020155EA`), `0x0611E106`
(`&0x1F`) and `0x0611E11E` (`&0x0F`) are `random_32` and the `_ex` variants.

A whole-image scan for `jsr @rN` preceded by `mov.l lit,rN` (plus `bsr`)
finds **157 call sites** of `0x0611E0EE`; the port has **114** `random_16()`
sites across 49 files (`grep`, this tree). The gap is expected — CPS3 has
effects the PS2 re-authoring dropped — and this section names the two that
matter for stage init.

### 23.5 The stage-init comparison: two CPS3 initialisers, each with one extra spawn (measured)

CPS3 functions were identified from *data they reference*, not guessed:

- `effl4_data_tbl` (`effect/effl4.c`, 24 `s16`) occurs once, at `0x061CB034`;
  its single referrer is **`CPS3 0x06113626`**, a 6-iteration loop that pulls
  from effect class 3 and stores `id = 214` — `effect_L4_init` (`214` is
  "L4": `effl4.c` sets `ewk->wu.id = 214`). Its only caller is
  **`CPS3 0x060BEBD4`**, which sets `pos_x_work = 0x1D0`, then calls, in order:
  `0x06087024(0xDD60, 1)`, `0x060DD486` (id 5), `0x060DD612` (id 6),
  **`0x060F1652` (id 74)**, **`0x060E3A00(8)` and `0x060E3A00(9)` (id 14)**,
  `0x06113626` (L4), `0x060EB982(6)` (id 44), and tail-jumps to `0x060E10AE(3)`
  (id 12). That is `stage/bg190.c` -> `bg1902_init00` — `effect_05_init();
  effect_06_init(); effect_L4_init(); effect_44_init(6); effect_12_init(3);`
  — **plus four calls the port does not make.**
- `eff71_time_tbl` (`effect/eff71.c`, `{2,8,12,9,4,6,50,3}`) occurs once, at
  `0x061BF684`, referenced from the CPS3 `effect_71_move` (`0x060F0DA8`,
  structurally identical to the port's: `obr_no_disp_check`, then
  `EXE_flag`/`Game_pause`/`EXE_obroll` gates, `random_16() & 7` into the time
  table). `effect_71_init` is `0x060F0E6E`; its only caller is
  **`CPS3 0x060BC678`**, which sets `pos_x_work = 0x200` and calls
  `0x06087024(?, 1)`, id 5, id 6, **`0x060DDC5E` (id 8)**, id 71, and
  tail-jumps to id 212 (`effect_L2_init`; "L2" = 212). That is
  `stage/bg030.c` -> `bg0301_init00` — `effect_05_init(); effect_06_init();
  effect_71_init(); effect_L2_init();` — **plus two calls the port does not
  make.**

The effect ids were read from each init's `mov.w r0,@(8,r4)` store (offset 8
is `id`; offset 6 is `work_id = 16`, exactly as in the port's `WORK`). The
id → move-routine table was found from `effect_L4_move` (`0x061135B4`, the
function immediately preceding `_init`): its only referrer is
`0x061B8B94 = 0x061B883C + 214*4`, so **`0x061B883C` is CPS3's
`effmovejptbl`**, and it agrees with the port's `effect/effxx.c` ->
`effmovejptbl` at every id checked that both engines share (5, 6, 12, 44, 71,
212, 214). At **[8] and [74] it does not** (§23.8).

Static reachability of `0x0611E0EE` from each spawned routine (BFS over the
resolved call graph, depth 5):

| spawned by CPS3 | id | `_init` reaches `random_16`? | move routine reaches `random_16`? | in port's stage init? |
|---|---|---|---|---|
| both stages | 5, 6 | 6: only via `char_move` (`0x06089848` → `0x0608BD6A`, the cell-walk) | same | yes |
| both stages | `0x06087024(const, 1)` | no (depth 6) | — | **no** (unidentified; every CPS3 stage init calls it) |
| bg190 | **74** | no | **yes — directly, `0x060F1522`, in routine 0** | **no** |
| bg190 | 14 (×2) | no | only via `char_move` | **no** |
| bg030 | **8** | no | **yes — directly, `0x060DD95A`, in routine 1** | **no** |
| bg030 | 71 | no | yes (`0x060F0E3C`, = port's) | yes |

The `char_move` path (`CPS3 0x0608BF0C`, a `random_16` inside the cell
walker) is the same path the port has (`engine/charset.c` has three
`random_16()` sites) and fires only on scripts that use the random-cell
opcode; the frame-exact reproduction in §23.7 shows it contributed nothing in
the first 60 frames on either stage.

### 23.6 The two consumers, read out (measured from the disassembly)

**CPS3 effect 74** — spawned only by Club Metro's `bg1902_init00`
(`0x060BEC22` is its sole call site in the image). `0x060F1652` pulls from
class 4, stores `id = 74`, allocates two palette handles (`0x061377F0(1)` /
`0x061376C6`). The move routine `0x060F1384`:

- **routine 0** (the spawn frame): `routine++`; `0x060F15E4` (a 3×16 block
  copy into the palette buffer via `0x060BAC3C`); then **`0x060F1516`:
  `random_16()`, `type = r & 3`**, `counter = 0`, `loops = ptr_b[type]`,
  `timer = rec[0].t`, `repeat = entry.repeat`; then a palette-request write
  (`0x0612E340(1, ptr)`).
- **routine 1** (every frame while `EXE_flag == 0 && Game_pause == 0`; the
  addresses `0x0200EECC` / `0x0201136E` are those globals — proved by the
  CPS3 `effect_71_move` reading them in the exact order the port's reads
  `EXE_flag`, `Game_pause`, `EXE_obroll`): `timer--`; on expiry `counter++`;
  if `counter < repeat` → `timer = rec[counter].t`; else `counter = 0`,
  `loops--`; if `loops > 0` → `timer = rec[0].t`; **else → `0x060F1516`
  again (a new `random_16`)**.
- The table at `0x061BF8BC` (four 12-byte entries `{rec*, loop_tbl*, repeat}`)
  and its records give the cadence per drawn type:

| type (`r & 3`) | records (timer each) | loops (`ptr_b[type]`) | frames until the next `random_16` |
|---|---|---|---|
| 0 | 5 × 2 | 8 | 80 |
| 1 | 2 × 1 | 2 | 4 |
| 2 | 2 × 1 | 1 | 2 |
| 3 | 5 × 2 | 1 | 10 |

So effect 74 consumes **exactly one index on the spawn frame** and then
re-rolls at a data-driven cadence between 2 and 80 frames.

**CPS3 effect 8** — spawned only by Hong Kong's `bg0301_init00` (`0x060BC6C6`
is its sole call site in the image). `0x060DDC5E` pulls from class 4, `id = 8`,
two palette handles. Move routine `0x060DD888`:

- **routine 0** (spawn frame): `count = 0`, `x98 = 0`, `timer =
  tblA[type].t` (= 1; `tblA` at `0x061BA9C4`, six-byte records), a palette
  block copy (`0x060DDBF0`) and a palette request (`0x0612E2FE`). No RNG.
- **routine 1** (`EXE_flag == 0 && Game_pause == 0`): `timer--`; on expiry
  `count = (count + 1) & 7`; **if `count == 0` → `random_16()`**, `v =
  vt[r]` with `vt = {0,0,0,1,0,0,2,0,0,1,0,0,3,0,0,0}` at `0x061BAA0C`; if `v
  != 0` → `x98 = v`, routine 2, `timer = tblB[type].t` (= 1; `tblB` at
  `0x061BA9F4`); else `timer = tblA[type].t` (= 1). Non-zero counts reload
  `timer = tblB[type].t` (= 1).
- **routine 2**: `timer--`; on expiry `count = (count + 1) & 3`; when it
  wraps, `x98--`; back to routine 1 (`count` is 0 there) when `x98` reaches 0.

With both timers 1, effect 8 consumes **one index every 8 frames** while in
routine 1, and pauses `4 × v` frames whenever a draw lands on a non-zero `vt`
entry.

**Spawn-frame timing.** Both stage initialisers run from `TATE00` ->
`ta0_init01` (`stage/tate00.c`), i.e. on the second `TATE00` call — frame 1
of the round (`game.c` -> `Game2_0` calls `TATE00()` once at round init, which
is the trace's f=0 `ta0_init00` call; `Game2_1` calls `TATE00()` before
`Basic_Sub_Ex()`). `effect/effect.c` -> `pull_effect_work` sets
`tadr->timing = exec_tm[index]` at pull time, and `move_effect_work` increments
`exec_tm[index]` *before* comparing, so an effect pulled inside `TATE00` is
moved by `system/sys_sub.c` -> `Basic_Sub_Ex` -> `move_effect_work(4)` **in
the same frame** — its routine 0 runs on frame 1. That is the port's
mechanism; that CPS3 orders the frame the same way is **inferred** from the
port being a decompilation of the same game loop, and is then confirmed by
§23.7 (a one-frame slip would break 7092's reproduction).

### 23.7 Reproduction: the model predicts every affected archive value exactly (measured)

`scratchpad/cps3/sim.py` steps `Random_ix16` frame by frame from each
replay's header value through the port's own consumers (as the live traces
show them: `ta0_init00` at f=0; the two `Appear_*` draws at f=2, Remy's first;
`effect_71_move` on Hong Kong, modelled from `eff71.c`) and then adds the CPS3
effect from §23.6 for that stage. Effects are stepped in spawn order inside
class 4, after `Player_control`'s intro draws. The port half of the model is
validated against the traces first; then the CPS3 half is compared with the
hash-recovered archive value:

| replay | stage | port model @f60 | trace live @f60 | CPS3 model @f60 | archive @f60 | Remy X: port / CPS3 model | Remy X: live / archive |
|---|---|---|---|---|---|---|---|
| 5268 | bg190 | `0x19` | `0x19` ✓ | **`0x1A`** | `0x1A` ✓ | 577 / 552 | 577 / 552 ✓ |
| 5027 | bg190 | `0x1D` | `0x1D` ✓ | **`0x1F`** | `0x1F` ✓ | 552 / 577 | 552 / 577 ✓ |
| 7092 (Urien/Remy) | bg190 | `0x21` | `0x21` ✓ | **`0x2A`** | `0x2A` ✓ | 552 / 552 | 552 / 552 ✓ (passes: same `& 7` class) |
| 6292 (p1=7 / Yun) | bg030 | `0x23` | `0x23` ✓ | **`0x28`** | `0x28` ✓ | — | — |

The 7092 row is the sharp one: effect 74 draws type 1 on frame 1, then
re-rolls at frames 5, 7, 17, 21, 31, 33, 37 and 39 (types 2, 3, 1, 3, 2, 1,
2, 0 — each duration read from the table) and finally lands on type 0, whose
80-frame span outlasts the checkpoint: **nine** extra indices, `0x21 → 0x2A`,
which is precisely what the archive holds. On Hong Kong the model also
reproduces the port's own `effect_71` draw frames from the trace (2, 11, 15,
25, 30, 37, 42, 49 — each `eff71_time_tbl[w] + 1` apart) before adding effect
8's every-8th-frame draws (9, 17, 25, 33, 41, 53 — the 49 slot is displaced by
a 4-frame routine-2 pause after the f=41 draw hit `vt = 1`). Four replays,
four different extra-call counts (1, 2, 9, 5), all exact. That is the
strongest form of evidence available short of instrumenting the arcade: the
consumer, its trigger frame, and its cadence are all pinned by data that had
no way of fitting by accident.

### 23.8 Why the port cannot simply "spawn effect 74 and 8" (measured)

In the PS2-derived engine those ids are taken. `effect/eff74.c` ->
`effect_74_move` is a **select-screen** effect (it reads `Menu_Suicide`,
`Menu_Cursor_Y`, `Order`, `Order_Timer`, has `EFF74_WAIT`/`EFF74_SUDDENLY`
states, and there is **no `effect_74_init` anywhere in the tree**).
`effect/eff08.c` -> `effect_08_init(s8 sc_num, s8 x, s8 y, u16 atr, s16
color_type)` is the round-message effect `engine/manage.c` spawns
(`effect_08_init(7, 0, 1, 15, 0)` etc.). The CPS3 `effmovejptbl` at
`0x061B883C` dispatches id 8 to `0x060DD888` and id 74 to `0x060F1384`, both
stage palette animations — so the PS2 re-authoring **re-used those two ids**
for different effects and dropped the arcade stage effects (both are
palette-cycling: they write palette pointers into a request table at
`0x0206A17C` via `0x0612E340`/`0x0612E2FE`; what they look like on a real
board was not determined — a Club Metro light show and a Hong Kong neon
flicker are the obvious readings, and are **inferred**). This is a genuine
engine-content gap, not a missing call: the port has *no code* for either
effect. It also means §22.5's "the effect namespace is shared" holds only for
the character-data-dispatched `effinitjptbl` indices it examined; the
stage-spawned move table disagrees at (at least) two ids.

### 23.9 What this means for play, not just replays

`Random_ix16` is one global sequence. On Club Metro and Hong Kong the port's
sequence is a *different walk* of the same table from frame 1 of **every
round** (`bg_routine` restarts per round; `Game2_0` -> `TATE00` ->
`ta0_init00`/`ta0_init01`), in **every mode**, under both balance settings.
Consumers that then diverge from what the arcade would have done with the
same inputs include: both players' intro variants (`Appear_34000` and every
other `Appear_*` that draws — the 25 px Remy shift is just the visible one),
every effect that picks with `random_16` (`effect/*.c`, 73 sites), CPU
pattern selection (`engine/plpat09.c`), and the dizzy-duration lookup
(`engine/plpdm.c` -> `kizetsu_timer_table`). The **distribution** of outcomes
is unchanged — the table is the same — so no player is advantaged and
port-vs-port netplay is unaffected; what is lost is *sequence fidelity to the
arcade*: an arcade-recorded match cannot be reproduced on these two stages,
and any future arcade-truth oracle that does not dirty-sync `Random_ix16`
will fail there. (That oracle now exists — the statcheck force-sync came out on
2026-09-05 — and the 143-segment corpus it was swept over contains no stage-3
and no stage-19 segment, so it is not evidence about §23.10 either way. It did
surface seven segments on two further stages, but those turned out NOT to be a
CG-data defect of this class: `research-arcade-balance-desyncs.md` §E5 is two
defects in the screen-quake writers -- a `bg_w.quake_y_index` write the arcade
does not have, and two wrong rows in `gqdt` -- and it is fixed.) For every other stage in the corpus the sequence matched to
frame 60 in 37/37 replays (previous pass) — there is no evidence of a third
affected stage, but see §23.11.

### 23.10 The fix — APPLIED 2026-09-03, CORRECTED 2026-09-06

**CORRECTION FIRST, because the acceptance below is the part that failed.**
This fix shipped with a real defect in `effc08.c` and the acceptance criterion
could not have caught it. **The validation window was 60 frames. That was too
short, and it is the reusable lesson of this section.**

`effect_C08_move`'s routine 2 — the `4 x v` frame pause a non-zero draw buys —
was transcribed **without** the `!EXE_flag && !Game_pause` gate that item 1
below correctly asks for; only routine 1 got it. The arcade gates both, at CPS3
`0x060DDA84` exactly as at `0x060DD918` (the function carries two `0x0201136E`
pool slots, `0x060DD98C` and `0x060DDBD8`, one per routine). `EXE_flag` and
`Game_pause` are zero for essentially all of the first 60 frames of a round —
that is *why* §23.11's "assumed zero" bullet was harmless for the model — so
every one of the four `sim.py` rows, and all 44 replays at checkpoint 2, were
blind to it by construction. A round only starts freezing once players connect
hits, super-freeze and pause; the drift then accumulates until it crosses a
whole 8-frame draw cycle.

Measured on a widened 185-segment corpus a fortnight of engine work later
(`docs/research-arcade-balance-desyncs.md` §E6, the corpus's divergence D2):
**every stage-3 segment failed — 26 of 26**, over 10 quarks, 10 player pairs
and 8 character pairings, at archive frames **1,325 to 6,473**, all with
`delta=+1` and `effect_C08_move` named in the RNG backtrace. The nearest frame
to the old acceptance window was more than twenty times past it. With the gate
added the corpus goes 151 PASS / 32 FAIL -> **177 PASS / 6 FAIL**, and the 26
that changed are exactly the stage-3 set.

**`effc74.c` was checked for the same defect and does not have it**: CPS3
`0x060F1390` dispatches only routines 0 and 1 plus a release default, so its one
gate is the only one there is, and the corpus's **13 stage-19 segments** pass
before and after.

**What to take from it.** A cadence fix cannot be accepted on a window shorter
than the period of the state it is supposed to reproduce. Effect 8's routine 2
is only *entered* on a non-zero `vt` draw and only *matters* once the game
freezes; 60 frames of round intro contains neither reliably. Where a full-length
oracle exists, spend it.

**Also corrected 2026-09-06: the spawns were not gated.** `bg0301_init00` and
`bg1902_init00` called `effect_C08_init()` / `effect_C74_init()`
unconditionally, so PS2 mode ran two effects that have **no PS2 counterpart at
all** (§23.8) and consumed `Random_ix16` indices the PS2 engine never consumes.
Both calls now sit inside `if (ArcadeBalance_IsEnabled())`, the correction
`2d74225d` applied to E4 and E5. The gate is on the spawn, not in the effect
bodies: these two modules exist only to reproduce CPS3, so under PS2 balance the
correct behaviour is for the work not to exist.

**Status: implemented and green.** `effect/effc74.c` (`effect_C74_init/move`,
id 87) and `effect/effc08.c` (`effect_C08_init/move`, id 88) carry the §23.6
state machines; `bg190.c -> bg1902_init00` and `bg030.c -> bg0301_init00` spawn
them at the arcade positions. The 44-replay corpus went **44/44 with zero
`Random_ix16-only divergence` at checkpoint 2 on stages 3 and 19**, on the first
run, with no constant tuned. `7092` — which had been passing by luck at +0 — now
matches at frame 60, which is what confirms the table-driven re-roll cadence
rather than only the spawn-frame draw. Exactly seven logs changed, all Remy or
Yun stages. The text below is the original proposal, kept as written.

**What is *not* the fix.** Adding a bare `random_16();` to `bg1902_init00`
(precedent: `ta0_init00`'s `// Calling this function is necessary for
Random_ix16 to be in sync with the arcade version`) would make 5268 pass and
5027 fail, because effect 74 keeps drawing at its table cadence (§23.6);
5027 needs the second draw at frame 5 and 7092 needs nine. A single call is
wrong, and a wrong fix would silently shift every other consumer on that
stage for the whole round. Do not do it.

**The fix.** Give the port the two arcade effects' **RNG behaviour**, as
effect works spawned where CPS3 spawns them, with the visual part optional:

1. New modules (suggested `effect/effc74.c` -> `effect_C74_init/move` and
   `effect/effc08.c` -> `effect_C08_init/move`, "C" for CPS3; any free id in
   `effmovejptbl` — the port table has 229 entries and `effect_dummy_move`
   slots — since 8 and 74 are taken). Each is a `WORK_Other` pulled from
   class 4 (`pull_effect_work(4)`, as the arcade does) with `disp_flag = 0`,
   carrying the state machine of §23.6 verbatim: for C74 `type, counter,
   loops, timer` plus the four-entry table above; for C08 `count, x98,
   timer, routine`. Gate both on `!EXE_flag && !Game_pause` exactly as the
   arcade routines do. Their only side effect is `random_16()`.
2. `stage/bg190.c` -> `bg1902_init00`: spawn C74 **between `effect_06_init()`
   and `effect_L4_init()`** (arcade order: 5, 6, 74, 14, 14, L4, 44, 12).
   `stage/bg030.c` -> `bg0301_init00`: spawn C08 **between `effect_06_init()`
   and `effect_71_init()`** (arcade order: 5, 6, 8, 71, L2). Order matters
   within class 4 — §23.7's Hong Kong row is exact only with 8 ahead of 71.
3. Acceptance is already built: run the viewer over the 44-replay corpus with
   `Random_ix16` recovery logging on; the expected result is **44/44 with zero
   "Random_ix16-only divergence" lines at checkpoint 2 on stages 3 and 19**.
   `sim.py` predicts the exact archive values, so a mismatch is a bug in the
   port of the state machine, not in the theory.
   **INSUFFICIENT, established 2026-09-06 — checkpoint 2 is frame 60.** It was
   met on the first run and it still missed a missing gate whose first
   consequence lands around frame 1,300. See the correction at the head of this
   section.
4. Rollback safety: effect works live in `frw[]`, which `netplay/game_state.c`
   saves and restores (`SDL_copya(es->frw, frw)`), so the stubs' state is
   rolled back with everything else. Both peers run the same code, so the
   netplay digest bump is the usual one for an engine change and nothing
   more.
5. Rendering the actual palette effects is a separate, larger job (the CPS3
   routines drive palette hardware through `0x0612E340`/`0x0612E4E0` with
   tables at `0x061BF6E4`/`0x061BA8C4`/`0x064DCBD4`/`0x064C93D4`); the RNG
   stubs make the engine arcade-faithful without it.

**Confidence.** High on the mechanism (the consumer is named from the arcade
binary, the trigger is its documented spawn site, and the cadence reproduces
four independent archive values exactly, one of them nine draws deep). Medium
on completeness — this pass diffed the three stage initialisers it had reason
to open (`bg190`, `bg030`, and the one at `0x060BCC68` that spawns id 14 twice
plus id 22 and 44(4), i.e. `bg180`), not all twenty. A full initialiser diff
is the natural next experiment (§23.11).

### 23.11 Not verified — stated so nothing is mistaken for a finding

- **CPS3's `TATE00`/`ta0_init00` were not located.** The CPS3 `ta_move_tbl`
  (22 `BGxxx` pointers with `[1] == [11]`) is at `0x06614C94`, but no literal
  in the image references it, so its dispatcher uses an addressing pattern
  the scan does not follow. Consequently the arcade code that upstream's bare
  `random_16()` in `ta0_init00` stands in for is **still unidentified**. What
  *is* measured is that CPS3 consumes exactly one index before frame 1's
  spawns on all four replays (every §23.7 row requires it), so the stand-in
  is count-correct.
- **`0x06087024(const, 1)`**, called by every CPS3 stage initialiser (71
  call sites in the image), was not identified. It has no path to
  `random_16` to depth 6; it reads tables at `0x0200EBBC`/`0x0200E3BC` and
  calls into the `0x0613xxxx` system region — a CG/palette loader is the
  obvious reading, and is inferred.
- **CPS3 effect 14 (×2 on Club Metro, ×2 on `bg180`)** reaches `random_16`
  only through `char_move`'s random-cell opcode. §23.7 shows it drew nothing
  in the first 60 frames; whether its script ever does later was not checked.
  The port's `effect_14_init(id, x, y, atr)` (`effect/eff14.c`) is, like 8
  and 74, a different effect.
- **The gate flags** (`EXE_flag`, `Game_pause`, `EXE_obroll`) were assumed
  zero for frames 1–60 in the model. The port traces support it (the
  `effect_71` cadence is table-exact), and a wrong assumption would have
  broken the 7092 and 6292 rows; but a round that opens with a pause or a
  hit-stop before frame 60 was not modelled.
  **RESOLVED 2026-09-06, and it cost a defect.** The assumption held for the
  model — but it was carried into the *implementation* as a comment in
  `effc08.c` and `effc74.c` saying the flags "were assumed zero for the frames
  the reproduction covers", and `effect_C08_move`'s routine 2 shipped ungated
  behind it. The arcade gates both of effect 8's routines (CPS3 `0x060DDA84`
  mirrors `0x060DD918`; two `0x0201136E` pool slots in the one function) and
  gates effect 74's only routine (one pool slot at `0x060F1488`). Both modules
  now say so with the addresses instead of recording an assumption, and the
  fix is §E6 of `research-arcade-balance-desyncs.md`. `EXE_obroll` remains the
  genuinely open third of the trio: CPS3 `effect_71_move` reads it, neither
  effect 8 nor effect 74 does, and neither port module tests it.
- **What effects 74 and 8 look like** on hardware (§23.8) is inferred from
  their palette-request writes, not observed. MAME/FBNeo with a watchpoint on
  `0x020155E8` would settle both this and the previous two bullets in one
  session; no emulator with a debugger is installed on this machine (`mame`
  not found), so it was not done.
- **The 42/44 and 37/37 figures** are the previous pass's; this pass
  re-parsed 16 of the 44 run logs (12 replays on seven other stages clean,
  the 4 on stages 3 and 19 drifting) and independently re-solved the two failing checkpoints.
- **Character ids.** Remy = 19 and Yun = 3 are read from the replays'
  headers cross-checked against the stage each loaded (`bg_index_tbl[19]` /
  `[3]`) and against this tree's Q = 17 (`screen/sel_pl.c` ->
  `Setup_Battle_Country`'s comment) and Ken = 11 / Makoto = 16 / Urien = 13
  from the replays' player names; no enum naming the characters was found in
  the tree.
- **Only three of twenty stage initialisers were diffed against CPS3.** The
  corpus shows no drift on the other stages it covers by frame 60, but a
  CPS3-only spawn whose first draw comes later than frame 60, or on a stage
  the corpus lacks, would not have been seen. The next experiment is
  mechanical: walk the CPS3 `ta_move_tbl` (its entries are known even though
  its referrer is not), decode each `bgXX0N_init00`'s spawn list with
  `spawn.py`, and diff against `stage/bg*.c`.

---

## 24. Elena's OVCT tail: CLOSED — unreachable by every writer, and the audit now checks it (eighth pass, 2026-09-06)

**Citation style for this section.** As in §21-§23: this document is not in
`tools/doc-citations/baselines.txt`, so everything below cites a **symbol**
(`file` -> `function`/`table`) or the exact text of a line. Code was read at
`new-stuff` @ `a3172c7f`. Every number marked **measured** was produced by
`tools/arcade-audit/cg_audit.py` (`ovct_reachability()`, added in this pass)
against the same `rom.bin` (md5 `909f5abec4b6b21bf7d2a452a03fdfcc`) and
`SF33RD.AFS` the rest of this document uses, or by the scratch sweep that
preceded it and that the function reproduces.

**Headline.** §18 was right that "no OVIX entry reaches a part ≥ 85" was a
false defence, and wrong about what replaces it. The part index has exactly
five writers in the engine (§24.2). Enumerating them over the shipped data
gives a reachable set for Elena of **parts 1-16, exactly** — not because a
timer never runs out, but because **every one of her 91 OVCT entries has
`parts_nix[i] == i`**, which makes `eff01.c`'s timer walk a self-loop, and
because the identity OVIX turns the cell's `olc >> 4` (max **16** over all
7,769 cells, pre- and post-terminator) directly into the part index. Parts
**17-90** are cold, the six unpatched ones among them. No timing argument is
needed and none is used. §8.B is closed with **no code change**; the model
is now code in `cg_audit.py` and `residual_audit.py`, so the claim is
re-derived on every audit run instead of resting in prose.

### 24.1 What the six entries are

Decoded from `LOC[ELENA]['ovct']` (`0x2AEB58`, 91 × 16 B, big-endian, field
order as `read_ovct`) — **measured**:

| part | hos_x | hos_y | colmd | colcd | prio | flip | timer | disp | mts | nix | parts_char |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 85 | 0 | 0 | 0 | 0 | 2 | 0 | 255 | 0 | 0 | 85 | 0x9CF6 (40182) |
| 86 | 0 | 0 | 0 | 0 | 2 | 0 | 255 | 0 | 0 | 86 | 0x9CF7 (40183) |
| 87 | 0 | 0 | 0 | 0 | 2 | 0 | 255 | 0 | 0 | 87 | 0x9CF8 (40184) |
| 88 | 0 | 0 | 0 | 0 | 2 | 0 | 255 | 0 | 0 | 88 | 0x9CF9 (40185) |
| 89 | 0 | 0 | 0 | 0 | 2 | 0 | 255 | 0 | 0 | 89 | 0x9CFA (40186) |
| 90 | 0 | 0 | 0 | 0 | 2 | 0 | 255 | 0 | 0 | 90 | 0x9CFB (40187) |

They are the last six of a **58-value run**: parts 33-90 carry `parts_char`
`0x9CC2..0x9CFB` contiguously (33 → `0x9CC2`, 90 → `0x9CFB`). That run is the
exact "raw gap 0x9CC2-0x9CFB" the `elena_cg_ranges` comment (§8.A) calls
"58 arcade-only sprites elided": it is not sprite-less and not elided — it is
the OVCT's namespace, and **no script cell of any of the 20 characters
references any value in it** (measured: 0 cells). `Apply3SXRenderingConventions`
gives parts 33-84 their PS2 sprites (33-34 → 11745-11746, 35-84 →
10772-10821, the −28385 / −29360 bands of §18.5); parts 85-90 are past
`common_count = 85` and keep the raw values, all ≥ 37,664. The comment is
corrected in this pass.

**What the PS2 uses parts 33-84 for, and the arcade does not (measured).**
Every PS2 part 33-84 has `parts_char` equal to the `cg_number` of the PS2 cell
that selects it, with `colcd 8` (13 for 33-34) and `mts 1` — `eff01.c` ->
`effect_01_move` sets `my_mts = 14` for `parts_mts != 0` — i.e. a second draw
of the body sprite in another texture mode. The PS2 cells that select them:
`btca[15]` and `dmca[82..89]` (parts 33-34: the electric-shock scripts of
§5/§8.A) and `saca[48]` (parts 35-84, one per cell, the SA slot
`9900_g[8..11]`). The arcade versions of those same scripts select **`olc 0`**
(btca/dmca) or **parts 1-16** (`saca[48]`, cells 3-55). Arcade parts 17-90 are
selected by nothing in the arcade data; the PS2 port re-purposed 33-84 for its
own rendering convention and dropped 85-90 (its OVIX and OVCT both end at 85).
PS2 OVIX entries 35-78 additionally carry a slot-1 part 1-16 (`ovix[35] = {35,
1, 0, 0}` … `{78, 16, 0, 0}`); the arcade OVIX is the identity throughout
(`{i, 0, 0, 0}`, all 91 — confirms §18.2).

### 24.2 Every writer of the part index

`get_new_parts_data` (`eff01.c`) reads the part at
`mwk->wu.overlap_char_tbl + ewk->wu.now_koc`. `now_koc` is `cg_ix`, plus one
when `type == 0 && mwk->player_number == 0 && rl_flag`. Everything that can
put a value into `cg_ix`, and everything that feeds it:

| # | Writer | Value | Elena, measured |
|---|---|---|---|
| 1 | `charset.c` -> `check_cgd_data` (both copies): `wk->cg_olc_ix >>= 4; wk->cg_olc = wk->olc_ix_table[wk->cg_olc_ix];` | the cell's `olc` word `>> 4` selects an OVIX entry; its four `s16` slots are the part indices, one per overlap `type` | `olc >> 4` over all 7,769 cells, all ten tables: pre-terminator `{0: 7596, 1..14: 3 each, 15: 5, 16: 5}`, post-terminator `{0: 121}`; **max 16**; all nonzero in `saca[48]` |
| 2 | `plcnt.c` -> `plcnt_init`: `wk->wu.cg_olc_ix = wk->wu.cg_hit_ix = 0;` | OVIX[0] | `{0, 0, 0, 0}` |
| 3 | `plpdm.c` -> `Player_damage`: `wk->wu.cg_olc_ix = datadrs[3];` with `datadrs = exdm_ix_data[wk->wu.dm_exdm_ix][wk->player_number]` — **unshifted**, and `player_number` is the **character** (`plcnt.c` -> `plcnt_init`: `wk->player_number = My_char[ix]`; `constants.h`: `CHAR_GILL = 0`) | `exdm_ix_data[b][8][3]` | rows `{ 30, 21, 1, 0, 10394 }` and `{ 40, 21, 1, 0, 10394 }` → **0**; the `cg_number` 10394 it also writes is group 9 |
| 4 | `eff01.c` -> `effect_01_move` restart: `ewk->wu.cg_olc.olc_ix[type] = ewk->wu.cg_ix = mwk->cg_olc.olc_ix[type];` | the master's current selection, i.e. #1-#3 through the OVIX | slot 0 of `ovix[e]` for `e ∈ {0..16}` = `{0..16}`; slots 1-3 are 0 for all 91 entries, so overlap types 1-3 never leave `if (mwk->cg_olc.olc_ix[type] == 0) return;` |
| 5 | `eff01.c` -> `effect_01_move` timer walk: `if (--cg_ctr == 0) { cg_ix = parts_nix ? parts_nix : cg_ix + 1; }` | the current part's `parts_nix`, else the next index | **`parts_nix[i] == i` for i = 1..90; `parts_nix[0] == 0`** |

Not writers: `netplay/game_state.c` restores `cg_ix`/`cg_olc`/`overlap_char_tbl`
from saved copies of the same values; `cg_type` 20/30 (the X.C.O.P.Y. markers,
§24.3) are only ever compared, never assigned, outside a cell decode (grep
`cg_type = 20` / `= 30`: no hits in `src/sf33rd`). `effect_01_init` sets no
part index; `pull_effect_work` does not zero the work, but the stale `cg_ix`
is unreachable: the walk branch requires the effect's cached selection to
equal the master's nonzero one, and the cache is written 0 in routine 0 and
by every dormant frame, so the first live frame is always a restart (#4).

### 24.3 The proof

1. **Seeds.** By #1-#3, the only OVIX indices Elena's engine state can hold
   are `{0..16}`. With the identity OVIX, slot 0 gives part indices `{1..16}`
   (0 is the dormant sentinel, #4) and slots 1-3 give nothing. Seeds =
   **{1..16}**.
2. **Closure.** From any seed p, #5 moves to `parts_nix[p]` if nonzero, else
   `p + 1`. For p ∈ 1..90, `parts_nix[p] == p`: the walk stays put, for any
   number of timer expiries. The `cg_ix + 1` branch requires `parts_nix == 0`,
   which only entry 0 has, and entry 0 is never a walk position (never a
   seed; no entry's `nix` is 0 except its own). So the closure of {1..16} is
   **{1..16}**, and `cg_ix + 1` **never executes for Elena**. Reachable parts
   = **1-16**; unreachable = **17-90**, including all six unpatched ones.
3. **Gill's +1 does not apply.** `mwk->player_number == 0` is `CHAR_GILL`;
   Elena is 8. (For Gill it is a left-facing part offset — his `parts_nix`
   pairs make that consistent; not audited here.)
4. **The X.C.O.P.Y. window is closed by data.** `effk7.c` -> `K7_move_type_0`
   rebinds `overlap_char_tbl`/`olc_ix_table` to the target's tables
   (`set_base_data_metamorphose` -> `set_char_base_data`) when Twelve's
   current cell has `cg_type == 20`, while `mwk->cg_olc` still holds the
   selection decoded from Twelve's cell against Twelve's 133-entry OVIX. Had
   that selection been a part index in 85-90, `eff01.c` would have applied it
   to Elena's table. Measured over both data universes: Twelve has **40**
   cells with `cg_type == 20`; every one has `olc >> 4 == 0`, so
   `cg_olc = ovix[0] = {0, 0, 0, 0}` — all four overlays dormant at the swap
   — and the cell before each marker also has `olc >> 4 == 0`. After the
   swap the cells come from Elena's tables (item 1). The reverse marker
   (`cg_type == 30`, Elena's cells) rebinds to Twelve's *larger* tables and is
   irrelevant to her tail.
5. **Model completeness.** The cell census is the same one every other
   section uses (§19: the 500 spans tile the ROM; the only un-decoded script
   is Ibuki's `atca` orphan). Post-terminator cells are included in the
   `olc` sweep even though they never execute; they contribute only 0.

That is the whole argument. It rests on the shipped ROM (sha-pinned by
`rom_load.c`) and on the five writers; it does not rest on `parts_timer`,
hit-stop, or how long a script holds an `olc`. §18.4's three "openers"
reduce to: a different ROM, a code change to `eff01.c`/`charset.c`, or a
data change to `exdm_ix_data` — each a change to the thing being audited,
and the first two would move the `cg_audit.py` row (§24.5).

### 24.4 Corrections to §18 (recorded, not silently edited)

- **§18.3(2)** says the walk "is monotone and unbounded" and would "march
  through 85-90 and off the end", held off only by `parts_timer = 255`. Wrong:
  `parts_nix[i] == i` makes it a self-loop. The 255-frame arithmetic and the
  "69 steps from the anchor at 16" were computed for a walk that does not
  exist.
- **§18.3(2)** says the +1 is applied "for P1/type-0/`rl_flag`". `player_number`
  is not the player slot; it is `My_char[ix]`, the character id, and the test
  is `== CHAR_GILL`. (`effk7.c` copies the *target's* `player_number` into
  Twelve on morph, which only makes sense for a character id.)
- **§18.6(iii)** reads `exdm_ix_data[dm_exdm_ix][player_number]` as indexed by
  a player slot ∈ {0, 1}, concludes rows 2..19 are unreachable and that
  "every character reads Gill's or Alex's row", and warns that "fixing" the
  subscript would overrun 19 OVIXes. The subscript **is** the character; the
  20 rows are live, one per character (which is why their `cg_number`s map
  1:1 onto groups 1..20 — the "inference" there is the mechanism); the 964 in
  row 7 is Ibuki's own and in range for her 2,230-entry OVIX. There is no
  dormant defect and nothing to fix. `statcheck_compare.c` records the same
  reading of `player_number`.
- **§8.A's `elena_cg_ranges` comment** ("58 arcade-only sprites elided") — see
  §24.1; corrected in `src/arcade/arcade_char_data.c` in this pass.
- §18.6(i) (Ibuki's pre-terminator OVIX overrun at 2277) and §18.6(ii) (the
  tables are genuinely two entries shorter; bytes past every OVCT decode as
  RICT) are **re-confirmed** for all 20 characters (measured; the RICT
  pattern `nix 513/257, timer 255, char 1` appears past every arcade OVCT).
  The seven characters whose post-terminator cells carry `olc` words like
  `0x8000` (Gill, Alex, Yun, Yang, Urien, Remy; Ibuki's is pre-terminator)
  are decoder artefacts of the same kind as §15.7's, and are reported
  separately from the pre-terminator set.

### 24.5 The defence, in code

- `tools/arcade-audit/cg_audit.py` -> `ovct_reachability(ci)`: parses the
  arcade OVIX/`parts_nix` (and the PS2 ones as the §6.1 control), sweeps
  every cell's `olc >> 4` (pre and post terminator), adds `plcnt_init`'s 0
  and `exdm_ix_data[*][ci][3]` (parsed from `plpdm.c`), and takes the closure
  of #4/#5 with **no timing constraint** — an upper bound on what the C can
  index. Per character the JSON gains `ovct_reachability` and the stats
  `ovct_reach_max`, `ovct_reach_unpatched` (reachable parts ≥ `common_count`),
  `ovct_walk_past_end` / `_ps2` (walk indices outside the table),
  `ovix_oob_pre_terminator`. **No pre-existing JSON value changed**
  (measured: field-by-field diff against `1d7aa3cb`'s file, 0 differences).
- The table's last column is now `ovct a/p reach`, and the flag is a
  reachability verdict, not a count difference: `TAIL-REACHED(n)!` when a
  reachable part is unpatched, `walk>end[...]` when the walk leaves the
  table, `tail-unreached(n)` when a tail exists but nothing indexes it, else
  `ok`.
- `tools/arcade-audit/residual_audit.py` R2b annotates every OVCT violation
  with `part_reachable` and prints `on a REACHABLE part : 0` — that line is
  the invariant. R2b itself stays reachability-blind on purpose (every slot
  is checked), so the six Elena rows still print, now with
  `part_reachable=False`.

ELENA row, `cg_audit.py`, before (committed `1d7aa3cb`, re-run 2026-09-06 —
byte-identical JSON) and after:

```
ELENA    7769 |    0    0     0     0    13    10 |     0     0     0     0     0     0     0 | 91/85 UNPATCHED-TAIL!  91/85 ok
ELENA    7769 |    0    0     0     0    13    10 |     0     0     0     0     0     0     0 | 91/85 r<=16 tail-unreached(6)  91/85 ok
```

`residual_audit.py` invariants after: `residual < 0 : 0`,
`residual >= offset-table length : 0`, and R2b `on a REACHABLE part : 0`
(6 slot violations, all Elena 85-90, all `part_reachable=False`).

### 24.6 Adjacent findings from the same sweep

**(i) DUDLEY — an arcade-only dangling `parts_nix`, timing-gated, OPEN (item
R).** Arcade entry 177 of 178 is `{timer 250, char 0, nix 178}`,
byte-identical to PS2's entry 177, but PS2's table continues with `178 =
{sprite 5058, nix 178}` and `179 = {sprite 5059, nix 179}` (group 5, `colcd
13`, `mts 1`). The arcade walk from seed 130 (`saca[48..51]`, `olc` 40) is 47
one-frame steps to 177 then 250 frames to 178, one element past the
`SDL_malloc(location.size)` buffer; from seed 82 (`saca[28..31]`, `olc` 39) it
is 344 + 250 frames. Those scripts hold the `olc` for 18 and 33 script
frames. `cg_audit.py` prints `178/180 r<=177 walk>end[178](arcade-only)`.
This is the only character whose arcade walk leaves its table. See §8.R.
**CLOSED 2026-09-06 (§25):** the hold is bounded at 179 / 148 frames against
a need of 297 / 594.

**(ii) RYU — a pre-existing PS2 quirk, not ours.** PS2 entries 54 and 55 (the
two entries the port added) carry `nix 57` and `58` against a 56-entry table,
and PS2 cells select 54 and 55 directly, so the PS2 walk leaves its own table
after 255 frames. The arcade table (54 entries) has no dangling `nix`.
Reported as `ovct_walk_past_end_ps2 = [57, 58]`; by §6.1 not an adaptation
defect.

**(iii) For 16 of the other 19 characters the reachable maximum is
`entries − 1`** (`r<=` column): the arcade OVCTs are used to their ends,
which is what makes Elena's 17-90 and the PS2's "+2 entries per character"
stand out as port-side data. The three exceptions — Yang (5 of 20), Akuma
(69 of 115), Chun-Li (63 of 75) — also carry cold parts, but every one of
them is below `common_count`, so it holds a PS2-patched `parts_char`;
`ovct_reach_unpatched` is 0 for all 20.

### 24.7 What this does not establish

- Nothing about what parts 85-90 (or 17-32, 33-90) *would* look like — no
  sprite identity was checked, because no path draws them. §18.5's −29360
  band remains an unverified arithmetic guess and is not applied.
- The CPS3's own overlay engine was not disassembled; the port runs the PS2
  engine (`eff01.c`) on arcade data, and that is the only engine whose
  reachability matters here.
- Dudley's window (§24.6(i)) was measured in script frames from cell `ctr`
  sums; hit-stop accumulation across a multi-hit SA was not modelled. The
  finding is "arcade-only dangling index, ≥ 297 frames from an 18-frame
  hold", not "reachable in play". **Modelled and closed in §25.**
- The reachability model is per-character: a part index decoded against one
  character's OVIX and consumed against another's table (the X.C.O.P.Y.
  window) was checked only at the 40 `cg_type == 20` markers and their
  predecessors, which is where the swap happens; `cg_audit.py` does not
  model the swap generically. **§25.5 extends the check to every cell after
  every marker: all `olc 0`. §26 closes the reverse swap (`cg_type 30`) for
  every character and puts both in `cg_audit.py`.**

---

## 25. Dudley's dangling OVCT next-index: CLOSED — the hold is bounded by data, and the audit now checks it (ninth pass, 2026-09-06)

**Citation style for this section.** As in §21-§24: this document is not in
`tools/doc-citations/baselines.txt`, so everything below cites a **symbol**
(`file` -> `function`/`table`) or the exact text of a line. Code was read at
`new-stuff` @ `54fd349d`. Every number marked **measured** was produced by
`tools/arcade-audit/cg_audit.py` (`ovct_dangling_hold()`, added in this pass)
against the same `rom.bin` (md5 `909f5abec4b6b21bf7d2a452a03fdfcc`) the rest
of this document uses, or by the scratch sweeps that preceded it and that the
function reproduces.

**Headline.** §8.R and §24.6(i) left Dudley's arcade entry 177
(`{timer 250, parts_char 0, parts_nix 178}`, one past his 178-entry table) as
"not observed and timing-gated", and §24.7 named the hole: hit-stop was not
modelled. It is now. The walk from the only two seeds that reach entry 177
needs **297** (seed 130) and **594** (seed 82) consecutive effect frames of
one unchanged `olc`; the master can hold those `olc` values for at most
**179** and **148** frames respectively — the run's own script frames plus
one positive `hit_stop` per attack-renewal cell, with the per-renewal value
bounded by the largest hit-stop any writer can hand an attacking player
(**23**, Remy's Blue Nocturne absorbing the hit). The gap is **118** and
**446** frames. This is not an observation: every writer of the master's
`hit_stop` was enumerated (§25.3), every mechanism that freezes the master
without freezing the effect was enumerated (§25.2), and the numbers come from
the ROM. §8.R is closed with **no code change**; the model is code in
`cg_audit.py`, so the DUDLEY row now reads
`walk>end-unreached[178:hold<=179/297]` and would flip back to
`walk>end[178](arcade-only)` if a cell, a table or an ATIT record moved the
bound past the need. The X.C.O.P.Y. gap in §24.7 is closed generically for all
40 markers (§25.5).

### 25.1 What has to happen for the walk to leave the table

`eff01.c` -> `effect_01_move` walks `cg_ix` only inside
`if (!Game_pause && !EXE_flag)`, only on the `else if (((PLW*)mwk)->sa_stop_flag == 0)`
branch, and only while its cached selection equals the master's
(`ewk->wu.cg_olc.olc_ix[type] != mwk->cg_olc.olc_ix[type]` restarts at the
seed; a master value of 0 goes dormant). Each such frame is one `--cg_ctr`;
the walk moves when it hits 0, and `get_new_parts_data` reloads `cg_ctr` from
the new part's `parts_timer` (both `u8`, `structs.h`, so a 0 timer is 256
frames). **Measured** on the arcade OVCT: exactly **96** entries (82-177) walk
to index 178; every other entry's walk stays inside the table. The OVIX
entries that can install one of them are **39** (`{82, 0, 0, 0}`) and **40**
(`{130, 0, 0, 0}`) — slots 1-3 are 0, so overlay types 1-3 never take part
(§24.2 #4). `plcnt_init`'s 0 and `exdm_ix_data[*][4][3]` (= 0, measured) select
nothing on the chain. From seed 130 the walk is 47 one-frame entries then 250
frames on entry 177: **297** frames. From seed 82 it is 47 one-frame entries,
250 on an entry whose `parts_nix` is 130, then the same: **594**.

So the question is exactly: can the master's `cg_olc.olc_ix[0]` stay **40**
(or **39**) for 297 (594) consecutive effect-walking frames?

### 25.2 What can keep the master's `olc` unchanged while the effect walks

The master's `cg_olc` is written only by a cell decode (§24.2 #1-#3), and a
player decodes cells only from `charset.c` -> `char_move` (`--cg_ctr == 0` ->
`check_cm_extended_code`). Where `char_move` is reached from, for a player in
an attack, and what withholds it:

| Path | Calls `char_move`? | Effect walks meanwhile? |
|---|---|---|
| `plmain.c` -> `player_mv_4000` -> `check_hit_stop` returns 0 -> `plmain_lv_02[4]` = `Player_attack` -> `plxx_extra_attack_table[player_number]` | yes, every frame in every nonzero `routine_no[3]` state of both Dudley SA handlers (§25.4) | yes — this is the run's own script frames |
| `check_hit_stop` with `hit_stop > 0`: `wk->wu.hit_stop--` and `num = 1` | **no** | **yes** — `eff01.c` never reads the master's `hit_stop`, and the attacker's `sa_stop_flag` is 0 (`charset.c` -> `comm_stop`: `wk->sa_stop_flag = 0` for the caster) |
| `check_hit_stop` with `hit_stop < 0`: `wk->wu.hit_stop++; char_move(&wk->wu);` | yes, from `check_hit_stop` itself | yes, but the master advances at the same rate — holds nothing |
| `Game_pause` or `EXE_flag` (`plcnt.c` -> `Player_control`: `if (Game_pause \|\| EXE_flag) goto end;`) | no | **no** — `eff01.c` tests the same two flags; symmetric, drops out |
| opponent's SA flash (`comm_stop`: `wk2->wu.hit_stop = ctc->ix; wk2->sa_stop_flag = 2;`, later 1 in `check_hit_stop`) | no while the stop lasts | **no** — `sa_stop_flag != 0` is the effect's own freeze; it returns to 0 only in state-entry code (`plpat.c` -> `Player_attack`, `plpnm.c`, `plpca.c`, `plpcu.c`, `plpdm.c` -> `setup_damage_process_flags`, `plmain2.c`), each of which installs a new script |
| round settle (`plcnt.c` -> `plcnt_die` -> `settle_process[]`, then `move_player_work()`) | yes — the players keep moving; `footwork_check`/`nekorobi_check` wait for them | yes, but at the same rate |
| a damage, catch, caught or normal state | new script, new `olc` | restart |

The only asymmetric freeze is the second row: a **positive `hit_stop` on the
attacker**. `Game_timer`-based slow motion (`slowf.c` -> `set_EXE_flag`)
and pause hit both sides; the opponent's SA stop freezes the effect harder
than the master; everything else installs a script. Netplay rollback
restores the effect pool wholesale (`game_state.c`: `SDL_copya(es->frw, frw)`),
so re-simulation cannot add walk frames either.

### 25.3 Every writer of an attacking player's `hit_stop`

Positive assignments to a `PLW`'s `wu.hit_stop` (grep `hit_stop =` over
`src/sf33rd`, effect-own `ewk->wu.hit_stop` excluded), classified by whether
the player is still on its attack script afterwards:

| Writer | Value | Leaves the attacker on its script? |
|---|---|---|
| `hitcheck.c` -> `dm_status_copy`: `as->hit_stop = as->att.hs_me;` (called on hit **and** on guard, `set_guard_status`) | the attacker's current ATIT record's `hs_me` | yes |
| `hitcheck.c` -> `set_paring_status`: `as->wu.hit_stop = sel_hs_add_tbl[hsadix] + 16;` / `= 16` | 16-20 (`sel_hs_add_tbl[6] = { 4, 3, 2, 1, 0, 0 }`) | yes |
| `plpdm.c` -> `damage_atemi_setup`: `ek->wu.hit_stop = wk->wu.att.hs_you;` (`ek` is the attacker; reached from `get_damage_reaction_data` when the *defender's* `atemi_flag` is 1 or 2) | the **defender's** current ATIT record's `hs_you` | yes — the defender jumps to its `cmms` script, the attacker continues |
| `plcnt.c` (and `plcnt2.c`) aiuchi KO: `plw[0].wu.hit_stop = plw[1].wu.hit_stop = 2` / `4` | 2 / 4 | no — both already in damage (`check_result_extra` requires `routine_no[1] == 1` on both) |
| `hitcheck.c` -> `check_result_extra` aiuchi: `plw[0].wu.hit_stop = plw[1].wu.hit_stop = hs1` / `hs2` | max `\|dm_stop\|` | no — same precondition |
| `plpdm.c` -> `Player_damage` -> `pls02.c` -> `set_hit_stop_hit_quake`: `wk->hit_stop = wk->dm_stop;`; `plpdm.c` `wk->wu.hit_stop = 3;` | | no — damage state, new script |
| `plpca.c`: `tk->wu.hit_stop = 1;` | 1 | no — the throw target is in `caught`, new script |
| `charset.c` -> `comm_stop`: `wk->wu.hit_stop = ctc->koc;` | the script's own value | only if a C cell sits inside the run (none do — §25.4) |
| `effect.c` -> `setup_shell_hit_stop`, `effc2.c`, `effi3.c`, `effe2.c` | | write an **effect's** `hit_stop`, not the player's |

Being hit does not extend the hold either: `hitplpl.c` ->
`plef_at_vs_player_damage_union` writes `ds->wu.routine_no[1] = 1;
ds->wu.routine_no[3] = 0;` at contact, so `check_hit_stop`'s
`(dm_stop != 0) && (hit_stop != 0)` case takes the `routine_no[3] == 0` arm
(`dm_stop = select_hit_stop(...); hit_stop = 0; return 0;`) and
`Player_damage` installs the damage script the next frame. The
`routine_no[3] != 0` arm (which would merge `dm_stop` into `hit_stop` and drop
the damage) is unreachable for a player that has just been hit.

**How many times per run.** A contact requires `att_hit_ok`, which only
`charset.c` -> `set_new_attnum` sets — on a cell whose `att` word is negative
(`if (wk->cg_att_ix < 0) { ... wk->att_hit_ok = 1; ...}`, the renewal) — and
which `hitcheck.c` clears on every registered contact (`mad->att_hit_ok = 0`
in both hit loops; `hissatsu_setup_union` also clears it at SA start). A
renewal needs a decode, a decode needs `char_move`, and `char_move` needs
`hit_stop == 0`. So the positive hit-stops during a run are sequential, one
per renewal cell at most, and the hold is

> **hold ≤ Σ ctr over the run + (renewal cells in the run) × HS\***, with
> HS\* = max(own `hs_me`, parry 20, atemi `hs_you`).

**Measured** from the arcade ATIT (`structs.h` `UNK_7`, 16 bytes, `hs_me` at
+12, `hs_you` at +13, both `s8`): Dudley's largest positive `hs_me` over all
118 records is **12** (the run's own records 62/92/36 carry 4, and 38-42
carry 2, 2, 4, 6, 10); the only scripts in the cast that issue `comm_atmf`
(`decode_chcmd[100]`) with a nonzero `koc` are **Dudley `saca[65..68]`**
(Cross Counter, `atemi_flag 1`, record 79, `hs_you` **21**) and **Remy
`saca[50..53]`** (Blue Nocturne, `atemi_flag 2`, record 47, `hs_you` **23**);
their whole ATITs bound the value at 21 and 23 whatever record happens to be
loaded. **HS\* = 23.**

### 25.4 The runs, and the bound

**Measured** — every cell in Dudley's ten arcade tables with `olc >> 4 == 40`
or `39`, pre- and post-terminator:

| `olc` | Scripts | Cells | Σ ctr | Renewal cells (`att < 0`) | Bounded by | Hold ≤ | Need | Gap |
|---|---|---|---|---|---|---|---|---|
| 40 (seed 130) | `saca[48..51]` (SA1, `9900_g[0..3]`; 51 is selected by no slot) | 43-60, eighteen 1-frame cells | 18 | 7 (`att` -3954 ×6, -2290) | L cell 42 (`olc 0`) and L cell 61 (`olc 0`, `cg_type 30`) | 18 + 7 × 23 = **179** | **297** | 118 |
| 39 (seed 82) | `saca[28..31]` (SA3, `9900_g[8..11]`; 31 is selected by no slot) | 13-25 | 33 | 5 (`att` -2420 … -2676) | L cell 12 (`olc 0`) and L cell 26 (`olc 0`, `cg_type 255`) | 33 + 5 × 23 = **148** | **594** | 446 |

Why the runs cannot be re-entered without a restart: a run is bounded on both
sides by an L cell with a different `olc`, and contains **no C cell**, so
leaving it in script order decodes an `olc 0` cell first (the effect goes
dormant and restarts at the seed on the next 40), and no command inside it can
loop or jump. The handlers are `plpatuni.c` -> `Att_SHOURYUUREPPA`
(`waza_r` 19 for command slot 20, `arcade_cmd_data.c` `unk_cmd_51`) and
`Att_CHOUCHUURENGEKI` (`waza_r` 21, `unk_cmd_53`): both call `char_move` — directly
or through `pls01.c` -> `jumping_union_process` — in every nonzero
`routine_no[3]` state, so the master never parks on a run cell. The `cg_type
20` on cell 43 sends `Att_SHOURYUUREPPA` to its airborne state, where landing
(`jumping_union_process`: `xyz[1].disp.pos + cg_jphos <= 0` ->
`char_move_cmja`) jumps to the address `comm_rja` saved at cell 36
(`cbca[0]`, `olc 0`) — an exit, not a re-entry. The own SA flash is
`comm_stop koc=-50 ix=50` at cell 1: a **negative** `hit_stop` for the caster,
so §25.2's third row — the caster animates through its own flash.

Nothing else in the frame budget is asymmetric, so the walk from seed 130
stops at or before entry 130 + 179 - 1 (well short of 177's 250-frame hold),
and from seed 82 at or before 82 + 148 - 1. Entry 177 is never occupied for
250 frames; index 178 is never formed; `get_new_parts_data` never reads past
`read_ovct`'s buffer.

### 25.5 The X.C.O.P.Y. swap, closed generically

§24.7 recorded that the swap was checked only at the 40 `cg_type == 20`
markers and their predecessors. **Measured** this pass over Twelve's arcade
tables: for every one of the **40** markers, **every** L cell from the marker
to the end of its script has `olc >> 4 == 0`. `effk7.c` -> `K7_move_type_0`
case 0 rebinds the tables at the marker, and the X.C.O.P.Y. script keeps
running until `plpatuni.c` -> `Att_METAMOR_WAIT` leaves it; those are the
cells decoded against the *target's* OVIX, and none selects anything. So no
Twelve cell can seed any target's walk, Dudley's chain included. The reverse
swap (case 4, on a `cg_type == 30` cell) is reached through `Att_METAMOR_REBIRTH`'s
`set_char_move_init(&wk->wu, 5, 1)`: Dudley's `saca[1]` carries its marker at
cell 1 with `olc 0` and every other cell `olc 0` (measured), so the effect is
dormant when Twelve's tables come back. Twelve-as-Dudley otherwise runs
Dudley's scripts through Dudley's handlers (`plxx_extra_attack_table[player_number]`
after `mwk->player_number = target->player_number`), so §25.4 covers him.
Dudley is **not exposed** through the swap.

### 25.6 The defence, in code

- `tools/arcade-audit/cg_audit.py` -> `ovct_dangling_hold(ci, rr)`: for every
  arcade walk exit (`past_end`), the seeds that reach it, the frames the walk
  needs (`parts_timer`, `u8`, 0 = 256), the OVIX indices that install each
  seed, every `olc` run in the ten tables that selects such an index
  (`olc_runs`), and the bound `Σ ctr + renewals × HS*` with HS* rebuilt from
  the sources each run: `parse_parry_hit_stop()` reads `sel_hs_add_tbl` and the
  `+ 16` from `hitcheck.c`; `arc_atit_hs()` reads the character's own ATIT;
  `atemi_hit_stop_max()` finds every `comm_atmf` script in the cast and takes
  its character's ATIT maximum. A run that contains a C cell, starts at a
  script's first cell or reaches its last is reported `unmodelled` and keeps
  the exit **reachable** — the model refuses to bound what it has not read.
  Per character the JSON gains `ovct_dangling_hold` and the stats
  `ovct_walk_past_end_reachable` (exits the model cannot rule out) and
  `ovct_walk_hold` (`[exit, hold_max, need]` for the ones it can). **No
  pre-existing JSON value changed** (measured: field-by-field diff against
  `54fd349d`'s file, 0 changed, 0 removed, 60 added — three keys × 20
  characters).
- The flag is now `walk>end[...]` only for a reachable-or-unmodelled exit;
  a bounded one prints `walk>end-unreached[exit:hold<=H/N]`. `TAIL-REACHED`
  still takes precedence.

DUDLEY row, `cg_audit.py`, before (`54fd349d`) and after:

```
DUDLEY   7051 |    0    0     0     0    16     0 |     0     0     0     0     0    33     0 | 178/180 r<=177 walk>end[178](arcade-only)  41/43 short
DUDLEY   7051 |    0    0     0     0    16     0 |     0     0     0     0     0    33     0 | 178/180 r<=177 walk>end-unreached[178:hold<=179/297]  41/43 short
```

`residual_audit.py` after: output byte-identical to before; `residual < 0 :
0`, `residual >= offset-table length : 0`, R2b `on a REACHABLE part : 0`.

### 25.7 Corrections to §8.R and §24 (recorded, not silently edited)

- **§8.R** says "hit-stop extends the hold; the SA freeze does not,
  `sa_stop_flag`". The conclusion is right, the reason is not: the caster's
  own flash is a **negative** `hit_stop` (`comm_stop`'s `koc`, -50 here) and
  `check_hit_stop`'s negative arm calls `char_move` itself; `sa_stop_flag`
  is set to 0 for the caster and to 2 for the *opponent*, where it freezes
  the **effect** (`eff01.c` reads the master's flag), not the master.
- **§8.R's three options** were all declined: (1) and (2) move
  `ArcadeCharData_ComputeDigest`; (3) would be a gated `eff01.c` guard for a
  read that cannot happen. The hazard was real data and stays reported
  (`ovct_walk_past_end = [178]`); what changed is that the audit now knows the
  frame budget, so the row is no longer an open item.
- **§24.6(i)**'s "scripts hold the `olc` for 33 and 18 script frames" and
  **§24.7**'s "hit-stop accumulation across a multi-hit SA was not modelled"
  are both superseded by §25.3-§25.4.

### 25.8 What this does not establish

- Nothing about what entry 178 *would* draw. No path forms the index.
- The bound is over the shipped ROM and the engine as read. A new writer of a
  player's positive `hit_stop`, a new `comm_atmf` script, a larger `hs_you`
  in either atemi character's ATIT, or a C cell inside either run moves
  `ovct_dangling_hold()`'s numbers and the row with them.
- The PS2 side was not modelled: Ryu's PS2-only `nix 57/58` (§24.6(ii)) is
  reported, not bounded, because it is not an adaptation defect (§6.1).
- The reverse X.C.O.P.Y. swap was checked for the script `Att_METAMOR_REBIRTH`
  actually installs (`saca[1]`). Dudley also carries `cg_type 30` cells with
  `olc >> 4 == 26` in `saca[71]`, `saca[72]` (measured); whether `K7_move_type_0`
  case 4 can be armed while one of those plays (an interrupted rebirth) was
  not analysed. If it can, the effect restarts on Twelve's 133-entry table
  with Dudley's OVIX[26] part index — in range, and a Twelve-table question,
  not this exit.
  **CLOSED 2026-09-06 (§26):** it cannot be armed there — case 4 is consumed
  on `saca[1]` cell 1, three frames after arming, and no writer can move the
  master off `saca[1]` first; had it fired, part 26 on Twelve's table
  self-loops in range on both data sides.
- The `unmodelled` verdict is deliberately blunt: a C cell inside a run is
  reported, not decoded. No run in the cast currently needs it (Dudley's two
  are the only dangling-exit runs).

---

## 26. The X.C.O.P.Y. reverse swap: CLOSED — case 4 fires only on the rebirth script's own marker, and the audit now checks it (tenth pass, 2026-09-06)

**Citation style for this section.** As in §21-§25: this document is not in
`tools/doc-citations/baselines.txt`, so everything below cites a **symbol**
(`file` -> `function`/`table`) or the exact text of a line. Code was read at
`new-stuff` @ `34a54e83`. Every number marked **measured** was produced by
`tools/arcade-audit/cg_audit.py` (`k7_swap_gate()` and `k7_foreign_cells()`,
added in this pass) against the same `rom.bin` (md5
`909f5abec4b6b21bf7d2a452a03fdfcc`) and `SF33RD.AFS` the rest of this document
uses, or by the scratch sweeps that preceded them and that the functions
reproduce.

**Headline.** §25.8 asked whether `effk7.c` -> `K7_move_type_0` case 4 — the
rebind of a morphed Twelve's tables back to his own — can be armed while
Dudley's `saca[71]`/`saca[72]` play, because those scripts carry `cg_type 30`
cells that select `olc 26`, and case 4 fires on the first `cg_type 30` cell it
sees with whatever selection that cell decoded against *Dudley's* OVIX. It
cannot. Case 4 is written only by case 3, and case 3 forces the master into
`Att_METAMOR_REBIRTH`, whose script (`saca[1]`) reaches its own `cg_type 30`
cell **three frames** later with `olc 0`; every writer that could move the
master off `saca[1]` in those three frames was enumerated (§26.3) and each is
closed by the code or by the shipped data. The same argument holds for **all
20** characters as X.C.O.P.Y. targets, and for five of them (Gill, Dudley,
Hugo, Ibuki, Remy) it is the only thing standing between a live `cg_type 30`
cell and a foreign part index; for Gill and Ibuki that index would leave
Twelve's 133-entry table. Two characters (Ken, Yang) have a gate the model
declines to close (§26.6) — Ken's foreign parts are in range on both data
sides, Yang's cells are post-terminator data. Had Dudley's case fired, the
consumed index is **part 26 on Twelve's OVCT**, `parts_nix[26] == 26`, timer
255: a self-loop, in range, and byte-for-byte the same structure on the PS2
side (§26.5) — by §6.1 not an adaptation defect either way. **No code
change**; the model is code in `cg_audit.py` (new `xcopy:` column, §26.7) and
fails toward *open* wherever it has not read the data.

### 26.1 The mechanism, and the frame order it depends on

`effk7.c` -> `K7_move_type_0` is a five-state walk on `ewk->wu.routine_no[1]`:

| case | waits for | does |
|---|---|---|
| 0 | `mwk->wu.cg_type == 20` on Twelve's X.C.O.P.Y. cell | `player_number`/`charset_id` := the target's; `set_base_data_metamorphose` -> `plcnt.c` -> `set_char_base_data` rebinds every table to the target's (`charid.c`: `char_table[]`, `overlap_char_tbl = cdat->ovct`, `olc_ix_table = cdat->ovix`, `hit_ix_table = cdat->hiit`, …) |
| 1 | `cg_type == 30` | `metamorphose = 1`, colours |
| 2 | `mwk->sa->ok != -1` | `metamor_over = 1` (`plmain.c` -> `Player_move`: `if (wk->metamor_over) sw_lvbt = 0` — input dead from here) |
| 3 | `K7_mt0_rebirth_check`: `routine_no[1] == 0`, `guard_flag != 3`, `!hit_stop` (demo arm: `routine_no[2] == 1 && routine_no[3] != 0`), then `pcon_rno[0] == 1` (else state 9, never case 4) | **`mwk->wu.routine_no[1] = 4; [2] = 33; [3] = 0; cg_type = 0; cg_hit_ix = 0; cg_ja = hit_ix_table[0]; set_jugde_area`** — and `ewk->wu.routine_no[1] = 4` |
| 4 | `cg_type == 30` | `player_number`/`charset_id` := Twelve's; `set_base_data_metamorphose` rebinds everything back |
| 5 | — | `routine_no[0] = 2` -> restore colours, `push_effect_work` (K7 freed) |

The order inside one frame is what makes the swap a cross-table read at all
(`game.c` -> `Game2_1`): `Player_control` (both players decode their cells;
`charset.c` -> `check_cgd_patdat` writes `cg_olc = olc_ix_table[olc >> 4]`
against the *current* OVIX), then `reqPlayerDraw` -> `move_effect_work(6)`
(K7 lives on list 6: `effect_K7_init` -> `pull_effect_work(6)`, id 207), then
`Basic_Sub_Ex` -> `move_effect_work(0..5)` (the overlays are list 1:
`eff01.c` -> `pull_effect_work(1)`), then `hit_check_main_process`. So on the
frame case 4 fires, the master's `cg_olc` was decoded against the *target's*
OVIX, K7 rebinds `overlap_char_tbl` to Twelve's, and `effect_01_move`'s
restart (`cg_ix = mwk->cg_olc.olc_ix[type]`, `get_new_parts_data`:
`overlap_char_tbl = mwk->wu.overlap_char_tbl + now_koc`) indexes Twelve's
table with the target's part index. That is the hazard §25.8 named. It needs
a `cg_type 30` cell whose `olc >> 4` is nonzero **and** K7 sitting in case 4
when that cell is decoded.

### 26.2 How case 4 is armed, and what the master is doing when it is

1. `ewk->wu.routine_no[1] = 4` has exactly one writer: case 3 (grep
   `routine_no[1] = 4` in `effk7.c`).
2. Case 3 writes the master's routine **(4, 33, 0)**. `plmain.c` ->
   `plmain_lv_02[4]` = `plpat.c` -> `Player_attack`; `routine_no[2] > 15` ->
   `plxx_extra_attack_table[wk->player_number]` -> `plNN_extra_attack` ->
   `plNN_exatt_table[33 - 16]`. **Measured** from the source: all **20**
   `plNN_exatt_table[18]` tables carry `Att_METAMOR_REBIRTH` at index 17, so
   the dispatch does not depend on which character the master is bound to.
3. `plpatuni.c` -> `Att_METAMOR_REBIRTH` case 0: `set_char_move_init(&wk->wu,
   5, 1)` — `char_table[5]` is `saca` (`charid.c`) — and **`metamor_over =
   0`**. `charset.c` -> `set_char_move_init` ends with `char_move(wk)`, so
   cell 0 is decoded on the install frame.
4. The rebirth script, all 20 characters — **measured**: cell 0 is `type 0,
   ctr 2, att 0, hit 0, canc 0, olc 0`; cell 1 is `type 30, ctr 1, olc 0`;
   cells 2-11 are `olc 0` (hit index 452); cell 12 is `comm_jmp` (`5, 53`).
   Twelve's own `saca[1]` is the same shape with five `ctr 2` cells before
   the marker (22 cells).

So, with case 3 firing at frame **N** (after `Player_control`): N+1 installs
`saca[1]` (cell 0, `cg_ctr` 2); N+2 counts down; N+3 decodes cell 1 (`cg_type
30`) in `Player_control`, K7 sees it in `reqPlayerDraw` the same frame and
rebinds, and N+4 frees K7. **Case 4 exists for four frames**, and the master
is on `saca[1]` for three of them. The only question is whether anything can
take the master off `saca[1]` (or keep it from getting there) inside that
window.

### 26.3 Every way off the rebirth script, enumerated and closed

For a player whose routine is being driven by `plmain.c` -> `player_mv_4000`
(`check_lever_data` -> `pls00.c` -> `process_attack`; then `check_hit_stop`;
then `Player_attack`), the writers of `routine_no[1..3]` and of the current
script, over the four frames:

| # | Path | Window | Why it cannot fire here |
|---|---|---|---|
| 1 | Contact — `hitcheck.c` -> `plef_at_vs_player_damage_union` (`ds->wu.routine_no[1] = 1`), `catch_hit_check` | N+1..N+3 | `hitcheck.c` -> `hit_push_request`: `if (hpq_in < 31 && hpr_wk->cg_hit_ix != 0)` — the master is **not in the queue**. Cells 0 and 1 decode to hit index 0: `check_cgd_patdat` case 4 computes `cg_hit_ix = ((att << 16 \| hit) * 8) >> 16 & 0x1FF` from `att 0, hit 0` (**measured**, 20/20). Both checks iterate `q_hit_push[]` only. |
| 2 | Contact at frame N itself (the master was queued during `Player_control`, before K7 ran) | N | Case 3 set `cg_ja = hit_ix_table[0]` and called `set_jugde_area`, which re-points `h_bod`, `h_han`, `h_att`, `h_hos`, `h_cau` — the pointers `attack_hit_check` (`sh = sad->h_bod->body_dm[0]`, `dmdat_adrs[0..10]`, `if (dmdat_adrs[lp2][1] == 0) continue`) and `catch_hit_check` (`sh = &sad->h_cau->cau_box[0]; if (sh[1] == 0) continue`) read at check time. **Measured**: `hiit[0]` is `{0,0,0,0,0,0,0,0}` for all 20 characters and every row it selects (`boda[0]` ×4, `hana[0]` ×4, `atta[0]` rows 2-3, `hosa[0]`, `caua[0]`) has `[1] == 0`. |
| 3 | `process_attack` cancel block (`check_full_gauge_attack`, `check_super_arts_attack`, `check_special_attack`, `check_chouhatsu`, `check_catch_attack`, `check_leap_attack`) | N+2, N+3 | Gated on `wk->cancel_timer`. Its only nonzero writer is `plpat.c` -> `get_cancel_timer` (8 call sites, all `Attack_0x000` handlers); `plpnm.c` -> `setup_normal_process_flags` zeroes it on **every** normal-state frame (`Player_normal` calls it first), case 3 requires normal state at N, and neither `Player_attack` nor `Att_METAMOR_REBIRTH` sets it. |
| 4 | `process_attack` -> `check_ashimoto_ex` (`routine_no[1] = 0; [2] = 55`) | N+2, N+3 | Needs `pls01.c` -> `check_floor_2`: `if (wk->bs2_on_car == 0) return 0`. `bs2_on_car = 1` has one writer, `jumping_union_process` under `Bonus_Game_Flag == 20`; `plpat19.c` -> `Att_METAMORPHOSE` creates K7 only `if ((Bonus_Game_Flag != 20) && (effect_K7_init(wk) != -1))`. |
| 5 | `process_attack` -> `routine_no[2] < 16 && check_full_gauge_attack(wk, 1)`; `routine_no[2] == 3 && check_sankaku_tobi` | N+2, N+3 | `routine_no[2]` is 33. |
| 6 | `process_attack` -> `check_cg_cancel_data` | N+2, N+3 | `if (wk->wu.cg_cancel == 0) return 0;` — `cg_cancel` is the cell's `canc` byte for a cgd-4 script (`setupCharTableData` copies `cgd_type` words from `cg_type`; `structs.h` puts `cg_cancel` in the fourth), and cell 0's is **0** (measured, 20/20). |
| 7 | `process_attack` -> `jumping_cg_type_check` | N+2, N+3 | Acts on `cg_type` 0xFF, 64, 2, 3, 7; cell 0 is type 0 (measured, 20/20). `Att_METAMOR_REBIRTH` case 1's own branches need 31 or 40, also absent before the marker. |
| 8 | The N+1 pre-empt: `process_attack` runs at N+1 **before** `Att_METAMOR_REBIRTH` installs `saca[1]`, on whatever cell was current when case 3 fired, with `routine_no[1] == 4`, `routine_no[3] == 0` | N+1 | Rows 3-5 and 7 are off (`routine_no[3] == 0` skips the block; `jumping_cg_type_check` is behind it). Row 6 remains. Input is dead this frame: `metamor_over` is still 1 when `Player_move` forces `sw_lvbt = 0`, and `cmd_main.c` -> `pl_lvr_set` builds `sw_0` from `sw_lvbt` (the only bits it can add are the release-derived 0x80/0x800, which `pls03.c` `shot_prio` does not list), so `check_nm_attack` (`shot_data_convert(sw_now) < 0 -> return 0`) is off and `check_renda_cancel` at most rewinds the current script without a routine write. What is left are the paths that fire from a **buffered** command and a **stale** `meoshi_hit_flag`: bits 0x40 (`check_super_arts_attack`), 0x20 (`check_special_attack`/`check_chouhatsu`), 0x08 (`check_meoshi_cancel`). They need the current cell's `canc` to carry one of them. Which cell can be current at N: case 3 needs `routine_no[1] == 0` — normal state runs `nmca` (all 43 `set_char_move_init` sites in `plpnm.c`/`pls00-02.c` use table 0); a transition *into* normal written inside a state handler leaves that state's script current for the frame, which from attack is possible only on a `cg_type` 0xFF/64/2/3/7 cell (`Player_attack` sets `guard_flag = 3` first and `pls00.c` -> `jumping_guard_type_check` is the only clear inside it — case 3 needs `!= 3`), never from catch/caught (`plpca.c`/`plpcu.c` set 3), and from damage on any `dmca`/`btca` cell (`Damage_04000` and others set 0). A transition written outside `Player_control` (`hitcheck.c` parry, `routine_no[1] = 0`) gets its `nmca` install in the next `Player_control`, before K7. **Measured for Dudley**: no `nmca`/`dmca`/`btca` cell carries any of 0x40/0x20/0x08 (the OR of every `canc` in those three tables is 0x00), and no attack-table cell of type 0xFF/64/2/3/7 does either. |
| 9 | Round settle — `plcnt.c` -> `settle_type_00000/20000/40000` (`routine_no[1] = 0; [2] = 40/41; [3] = 0`) | any | Each write sits behind `footwork_check` (`routine_no[1] == 0 && routine_no[2] == 1` — normal **and standing**) or `nekorobi_check` (damage state, lying down); `settle_type_30000` and `init_app_30000` start from a fresh player init (`K7_muriyari_metamor_rebirth`). Meanwhile `plcnt_die` -> `move_player_work` keeps `Player_move` running, so the rebirth completes first; `time_over_check`'s early return skips one `Player_move` and nothing else. If the round has already ended when rebirth-check passes, `pcon_rno[0] != 1` sends K7 to state 9, not case 4. |
| 10 | `Game_pause`, `EXE_flag`, the opponent's SA flash | any | Pause/slow freeze `effect_K7_move` on the same two flags. `comm_stop` gives the master a positive `hit_stop`; `check_hit_stop` withholds `Player_attack`, K7 keeps polling `cg_type`: the cells are delayed, not reordered. |
| 11 | Round init, training reset, rollback | any | `plcnt.c` -> `erase_extra_plef_work` -> `effect_work_list_init(6, -1)` frees list 6 (K7) and `setup_any_data` -> `set_base_data_tiny` restores `My_char`; both run from `game.c` (round transition) and `menu.c` (`Tr_Reset`). `game_state.c` restores the effect pool wholesale. K7 and the morph end together; no case 4 survives. |
| 12 | K7 itself aborting (`effect_K7_move` case 1: `dead_f`, or `metamor_index != myself`) | any | `dead_f` has no writer for K7 (every `dead_f = 1` is an effect writing its own); `metamor_index` is written only by `effect_K7_init`, reachable only through Twelve's `pl19_exatt_table` — not dispatched while `player_number` is the target's. An abort would free K7 *without* rebinding, i.e. no case 4 at all. |

Nothing else writes a live player's routine or script between `Player_control`
and the next one. Therefore the master decodes `saca[1]` cell 1 at N+3, case 4
fires on it, and **the selection it consumes is `olc 0`** — `ovix[0] = {0, 0,
0, 0}` for every character (§24.2 #2), so all four overlays go dormant at the
rebind. The cells after the marker, which are decoded against *Twelve's* OVIX
once the tables are rebound, are `olc 0` up to the `comm_jmp` (measured, 20/20;
the jump then goes through the rebound `char_table` into Twelve's own
`saca[53]`, whose cells §24's per-character sweep already covers).

### 26.4 So when do `saca[71]`/`saca[72]` play, and what are they

`saca[65..68]` are Dudley's Cross Counter (§25.3: the only `comm_atmf`
scripts besides Remy's). **Measured**: each opens with `comm_rmja (5, 69..72,
1)`, which `charset.c` -> `comm_rmja` stores in `cmms`; when the absorbed hit
lands, `plpdm.c` -> `damage_atemi_setup` -> `char_move_cmms` installs it. So
`saca[69..72]` are the four counter-punch follow-ups (`saca[75..77]`,
`9900_g[36..38]`, also `comm_rmja` into 69-71), reachable only through a
Cross Counter that absorbs a hit. Their `cg_type 30` cells — `saca[71]` cell
14, `saca[72]` cells 14 and 33, all `olc 26` — are the engine's generic
phase marker: `cg_type == 30` is read by some thirty attack handlers in
`plpat*.c`/`plpatuni.c` for their own state changes, and by
`K7_move_type_0` in cases 1 (sets `metamorphose`, nothing else) and 4. So a
`cg_type 30` cell is harmless to the tables unless K7 is in case 4, and K7 is
in case 4 only during §26.2's window, during which the master is on
`saca[1]`. `saca[72]` cell 33 sits after
an unconditional `comm_jpss` (`5, 72, 37`) at cell 18 and is classed
post-terminator by the audit's §19 convention; the gate covers it regardless.

### 26.5 Had it fired: part 26 on Twelve's table, in range, and pre-existing

The question §25.8 deferred, answered for completeness and as the §6.1
control — **measured**:

| | arcade | PS2 |
|---|---|---|
| Dudley `saca[71]` c14 / `saca[72]` c14, c33 | `type 30, olc 26` | `type 30, olc 26` (same cells) |
| Dudley `ovix[26]` | `{26, 0, 0, 0}` | `{26, 0, 0, 0}` |
| Twelve OVCT / OVIX entries | 133 / 133 | 135 / 135 |
| Twelve `ovct[26]` | `timer 255, nix 26` (self-loop) | in range |
| cell after the marker | `olc 0` | `olc 0` |

A swap there would restart overlay type 0 at part 26 of Twelve's table — the
wrong sprite, not an out-of-bounds read — and the PS2 data carries the same
cell, the same OVIX entry and a larger Twelve table. Nothing the port
adapted is on that path.

### 26.6 The same gate for the other nineteen targets

`k7_foreign_cells()` lists every `cg_type 30` cell outside `saca[1]` that
selects a live `olc` — the cells the gate protects — and what a swap there
would consume; `k7_swap_gate()` applies §26.2-§26.3 per character.
**Measured**:

| target | foreign cells (live + dead) | gate | if case 4 fired there |
|---|---|---|---|
| Gill | 5 (`saca[29..32]` c3 `olc 105` -> part 206; `saca[59]` c6 `olc 142` -> parts 291, 292) | closed | **past** Twelve's 133 entries; PS2 identical |
| Dudley | 2 + 1 (§26.4) | closed | part 26, in range; PS2 identical |
| Hugo | 2 (`caca[33]`/`[42]` c19 `olc 50` -> part 59) | closed | in range; `caca[42]` has no PS2 twin |
| Ibuki | 23 + 1 (`saca[24..31]`, `[44..47]`, `[60..63]`, `caca[10]`; `olc` 1030-1225 -> parts 1089-1284) | closed | **past** Twelve's OVIX and OVCT; 22 of 23 PS2 identical, `caca[10]` c31 arcade-only (counts corrected in §26.10.2 — four cells this table called dead are live) |
| Remy | 5 + 2 (`saca[28..31]`, `olc` 26-28 -> parts 28, 29, 34) | closed | in range; PS2 identical |
| Ken | 6 (`saca[30]` c11, `saca[36..39]` c32, `saca[64]` c12; `olc` 19-21 -> parts 37-39) | **unmodelled** — `nmca[4]` cells 6-7 (`canc 0x21`) and `dmca[64]` cell 17 (`canc 0x60`) can be current at arming (§26.10.1: the input-side half of this row is now *refuted*, not merely unread) | in range; PS2 identical |
| Yang | 0 + 4 (`saca[44..47]` c41 `olc 1264`) | **unmodelled** — nine `atca` type-64 cells with `canc 0x24` | the four cells are unreachable by any entry point (§26.10.2, a closure stronger than the §19 convention this row originally used), and `olc 1264` is already past Yang's own 20-entry OVIX (`ovix_oob_post_terminator`, §24.4's decoder-artefact class) |
| Alex, Necro, Elena, Oro, Urien, Twelve | 0 | closed | — |
| Ryu, Yun, Sean, Akuma, Chun-Li, Makoto, Q | 0 | unmodelled (`nmca[4]`/`dmca[64]`-class cells, Yun's `atca` type-64 cells) | nothing to protect |

Two things follow. First, the gate is **load-bearing** for Gill and Ibuki:
their foreign selections would leave Twelve's tables on both data sides, so
a future data or code change that opens the window — a `canc` byte on a
normal-state cell, a hit box on `saca[1]` cell 0, a new writer of the routine
inside the window — is a real hazard there, and the audit will say so
(`xcopy:FOREIGN-OOB(...)!`). Second, the `unmodelled` verdicts are the honest
edge of the model: they say a cell with a buffered-command cancel bit *can*
be current when case 3 fires, not that the cancel then succeeds (it still
needs the buffered command, `meoshi_hit_flag` from a previous hit, and for
0x40 an SA stock). Ken's consequence is in range either way; Yang's cells are
dead data.

### 26.7 The defence, in code

- `tools/arcade-audit/cg_audit.py` -> `parse_k7_rebirth()`: reads the case-3
  routine and the case-4 marker from `effk7.c`, the install from
  `Att_METAMOR_REBIRTH`, and asserts all 20 `plNN_exatt_table[18]` dispatch
  that routine to `Att_METAMOR_REBIRTH`. `k7_swap_gate(ci)`: the rebirth
  script's marker index, the cells before it (type, hit index via
  `check_cgd_patdat`'s arithmetic, `canc`), `hiit[0]` and every box row it
  selects (`hiit0_boxes`), the marker's `olc`, the post-marker cells against
  Twelve's OVIX/OVCT, and the §26.3 row-8 census (`K7_ENTRY_TABLES` =
  nmca/dmca/btca on any cell; `K7_ATTACK_TABLES` on `K7_END_TYPES` cells;
  bits `K7_CANCEL_BITS` = 0x68). Anything it cannot close is a reason in
  `unmodelled`, which keeps the gate **open**. `k7_foreign_cells(ci)`:
  every protected cell with its target-OVIX parts, the following cells'
  `olc`, and both consequences on Twelve's tables (`_k7_consequence`),
  arcade and PS2, with post-terminator cells marked `dead`.
- Per character the JSON gains `xcopy_case4` (`gate`, `foreign_cells`) and
  the stats `k7_foreign_cells`, `k7_foreign_dead`, `k7_gate`,
  `k7_foreign_oob`, `k7_foreign_oob_ps2`, `k7_foreign_ps2_differs`. The
  table gains a trailing `xcopy:` column: `none`, `gated(n)`,
  `unmodelled(n,in-range)`, or `FOREIGN-OOB(k/n)!` — the last only when the
  gate is open **and** a live foreign cell's consequence leaves Twelve's
  tables. **No pre-existing JSON value changed** (measured: field-by-field
  diff against `34a54e83`'s file, 0 changed, 0 removed, 460 added — six
  stats keys × 20 plus the `xcopy_case4` records). `arc_parse`/`ps2_parse`
  now keep the cell's `ext`/`canc` bytes; nothing that was emitted before
  reads them.

DUDLEY row, `cg_audit.py`, before (`34a54e83`) and after:

```
DUDLEY   7051 |    0    0     0     0    16     0 |     0     0     0     0     0    33     0 | 178/180 r<=177 walk>end-unreached[178:hold<=179/297]  41/43 short
DUDLEY   7051 |    0    0     0     0    16     0 |     0     0     0     0     0    33     0 | 178/180 r<=177 walk>end-unreached[178:hold<=179/297]  41/43 short  xcopy:gated(3)(1 dead)
```

`residual_audit.py` after: output and `residual_audit.json` byte-identical to
before; R2b `on a REACHABLE part : 0`.

### 26.8 Corrections to §24 and §25 (recorded, not silently edited)

- **§25.5** says the reverse swap "is reached through `Att_METAMOR_REBIRTH`'s
  `set_char_move_init(&wk->wu, 5, 1)`" and that Dudley's `saca[1]` marker is
  at cell 1 with `olc 0`. Both right; what it did not say is *why* no other
  `cg_type 30` cell can be the one case 4 sees — §26.3 is that argument, and
  it does not rest on `saca[1]` alone but on the master being unable to
  leave it.
- **§25.8**'s "in range, and a Twelve-table question" was a guess; §26.5
  measures it (in range, self-loop, PS2-identical).
- **§24.7** said `cg_audit.py` "does not model the swap generically". It now
  models both directions; the forward one (§25.5's all-`olc 0` sweep) is
  still a per-marker data fact rather than a function, and is listed below.

### 26.9 What this does not establish

> **Resolved in §26.10 (eleventh pass, 2026-09-06), except where noted.**
> Bullets 1, 2 and 4 are closed and are now code in the audit; bullet 3 (Ken,
> Yang, and the seven with nothing to protect) is *partly* closed — Yang is
> closed outright, and Ken's input-side leg turns out to be **refutable, not
> merely unread**, which leaves the gate open for a better-founded reason than
> this section gives. Bullet 5 stands unchanged.

- The **forward** swap (case 0) is closed by §25.5's sweep, not by code in
  the audit; a Twelve cell after a `cg_type 20` marker acquiring a nonzero
  `olc` would not move a row. Same class of gap as §25.8's last bullet.
- Tables other than OVIX/OVCT after a rebind — the post-marker cells'
  **hit indices** (452 for all 20) are read against Twelve's 503-entry
  `hiit`, and the `comm_jmp (5, 53)` lands in Twelve's 75-entry `saca` —
  were checked only by inspection here, not by the audit.
- The `unmodelled` verdicts for Ken and Yang (and the seven characters with
  nothing to protect) are not closed: the model reports that a cell with a
  buffered-command cancel bit can be current at arming, and stops. Closing
  them means tracing `check_cg_cancel_data`'s 0x40/0x20/0x08 paths at N+1
  through `meoshi_hit_flag` (set by `hitcheck.c` on the master's own hit,
  cleared only by an attack setup) and the command buffer — or showing those
  cells cannot be current with `guard_flag != 3` and `hit_stop == 0`.
- `effect.c` -> `pull_effect_work` does not clear the work it hands out and
  `effect_K7_init` does not write `dead_f`; a slot whose previous occupant
  left `dead_f = 1` would make K7 abort on its first frame (case 1, no
  rebind, morph fizzles). Engine-side and identical on both arms; noted, not
  analysed.
- The CPS3's own overlay engine was not disassembled (§24.7); the port runs
  `eff01.c`/`effk7.c` on arcade data, and those are the only engines whose
  arming order matters here.

## 26.10 The §26.9 residuals: Yang and the method gaps closed, Ken's gate open for a better reason (eleventh pass, 2026-09-06)

**Citation style for this section.** As in §21-§26: this document is not in
`tools/doc-citations/baselines.txt`, so everything below cites a **symbol**
(`file` -> `function`/`table`) or the exact text of a line. Code was read at
`new-stuff` @ `56f4f52a`. Every number marked **measured** was produced by
`tools/arcade-audit/cg_audit.py` against the same `rom.bin` (md5
`909f5abec4b6b21bf7d2a452a03fdfcc`) the rest of this document uses.

**Headline.** §26.9 left five residuals. Three are now closed and are code in
the audit, not prose: the **forward** swap (`k7_forward_gate()`), the
post-rebind hit indices and `comm_jmp` target (folded into `k7_swap_gate()`),
and `pull_effect_work`'s `dead_f` — which turns out never to have been a
hazard at all (§26.10.4). **Yang is closed** by an entry-point argument that
is strictly stronger than the §19 terminator convention §26.6 leaned on — and
that convention is itself **unsound**, which reclassifies four Ibuki cells
from dead to live (§26.10.2). **Ken stays `unmodelled`**, but the reason has
changed shape: the input-side leg of §26.3 row 8 is now *refuted* rather than
unread, because the only input word the shipped command data ever reads is
`sw_chg`, and `sw_chg` at N+1 carries frame N's live buttons (§26.10.1). The
consequence is still in range on both arms and PS2-identical, so §6.1 puts it
out of scope: **no code change**, and the model still fails toward *open*
wherever it has not read the data.

### 26.10.1 Ken: "input is dead at N+1" is false for the only word the cancel reads

§26.3 row 8 conceded that the 0x40/0x20 cancel paths could fire "from a
**buffered** command and a **stale** `meoshi_hit_flag`", but rested part of
its confidence on `plmain.c` -> `Player_move` forcing `sw_lvbt = 0` while
`metamor_over` is set. Reading the input chain end to end shows that force
does not reach the word those paths actually read.

1. Both paths sit inside one guard. `pls00.c` -> `check_cg_cancel_data`
   opens `if (wk->wu.cg_cancel == 0) return 0;` and then puts the whole
   `0x40` block (`check_full_gauge_attack`, `check_super_arts_attack`) and
   the whole `0x20` block (`check_special_attack`, `check_chouhatsu`) inside
   `if (wk->wu.meoshi_hit_flag != 0) { ... }`.
2. Every scan reads its button word through the same window. `pls03.c` sets
   `conpane = &wk->cp->sw_lvbt` at **all eight** of its scan sites and then
   reads `cusw = conpane[wk->cp->btix[i] & 0xFF]`. `structs.h` -> `WORK_CP`
   orders that window `sw_lvbt, sw_new, sw_old, sw_now, sw_off, sw_chg,
   old_now` — indices 0 to 6.
3. **Measured** (`k7_input_words()`, over `src/arcade/arcade_cmd_data.c`,
   whose records `cmd_main.c` -> `cmd_data_set` consumes as `reset, w_dead,
   w_dead2, waza_r[0..3], btix, exdt[0..3]`, i.e. `btix` is word 7): across
   all **21** arcade command tables, every ground entry (28-37) and every air
   entry (46-55) has `btix & 0xFF` equal to **5** — `sw_chg` — or the `0x80`
   no-button sentinel `pls03.c` tests for explicitly. Word 5 is the only live
   index in the shipped data; `sw_lvbt`, `sw_new` and `sw_now` are never read
   by a special-move entry.
4. `sw_chg` is not a function of this frame's input. `cmd_main.c` ->
   `pl_lvr_set` computes `chk_pl->sw_chg = (chk_pl->sw_now) | (chk_pl->sw_old
   & ~(sw_0))`, and `cmd_main.c` copies it into `wcp[cmd_id].sw_chg`
   alongside `sw_new`, `sw_old`, `sw_now` and `sw_off`. `sw_old` is the
   previous frame's `sw_new`. So with this frame's `sw_0` forced to 0, every
   button held on the previous frame appears in `sw_chg` as a release.
5. Which frames are forced. `Player_move` zeroes `sw_lvbt` at the top of the
   frame when `metamor_over` is set, so at N+1 `sw_lvbt`, `sw_new` and
   `sw_now` are indeed dead — row 8's argument for the `0x04` path
   (`check_nm_attack`, which reads `sw_now` via `shot_data_convert`) survives
   untouched. `sw_old`, `sw_off`, `sw_chg` and `old_now` do not.
6. Frame N itself had live input. `effk7.c` -> `K7_move_type_0` case 2 sets
   `mwk->metamor_over = 1` and then **falls through** to case 3 in the same
   invocation — there is no `break`. `metamor_over` is read at the top of
   `Player_move`, and K7 runs later in the frame (`game.c` -> `Game2_1`:
   `Player_control`, then `reqPlayerDraw` -> `move_effect_work(6)`), so on
   the frame case 2 fires the master's input was still live.
7. And that frame can be N. `K7_mt0_rebirth_check` requires `routine_no[1]
   == 0`; case 2 waits on `mwk->sa->ok != -1`; and `plmain.c` ->
   `sag_union_0`/`sag_union_1` case 2 restores `ok = 0` on the condition
   `(wk->sa->saeff_ok != 1) || (wk->wu.routine_no[1] != 4)` — the moment the
   master leaves attack state. `about_gauge_process`, which calls
   `sag_union`, is the **last** call in `plmain.c` -> `player_mv_4000`, so it
   runs after the state handler. On the first frame the master is back in
   normal state, `sa->ok` leaves -1, case 2 releases, `metamor_over` becomes
   1, and case 3's `routine_no[1] == 0` is already true. That is the main
   line, not an edge case, and its `Player_move` saw live input.

So `sw_chg` at N+1 carries the buttons the master held at frame N. The gate
stays open, now for a reason the model can state positively.

The `guard_flag` sub-argument does not close Ken either. `nmca[4]` has exactly
two installers — `plpnm.c` -> `nm_05_0000` and `nm_05_0100`, both under
`Normal_05000`, which sets `wk->guard_flag = 3` before dispatching. But
`Normal_05000` ends by calling `jumping_guard_type_check`, and `pls00.c` ->
`jumping_guard_type_check` clears `guard_flag` on `cg_type` 0xFF, 64, 2, 3
and 7 — and Ken's two flagged cells are types **7 and 64**. So
`K7_mt0_rebirth_check`'s `guard_flag != 3` is satisfiable on precisely the two
cells that carry the cancel bit.

**What is still unread** — exactly two links, and this is the whole of it:

- `meoshi_hit_flag != 0` at N+1. Writers enumerated: set by `hitcheck.c`
  (the attacker's own landed hit — and for a non-player attacker the branch
  writes the *effect's* flag, not the master's), by `charset.c` ->
  `check_cgd_patdat` when `(kow & 0x60) && (cg_cancel & 0x40)` and
  `DIP2_SA_TO_SA_CANCEL_DISABLED` is clear, by `charset.c` -> `comm_smhf`,
  and by `plpat18.c`; cleared by `charset.c` -> `set_new_attnum` (on
  `cg_att_ix < 0`) and by `pls03.c` -> `hissatsu_setup_union`,
  `check_nm_attack`, `check_chouhatsu`, `check_catch_attack`,
  `check_renda_cancel`, `check_meoshi_cancel`. `plpnm.c` ->
  `setup_normal_process_flags` clears `cancel_timer` but **not**
  `meoshi_hit_flag`, so it can be stale across every normal-state frame.
  Whether it is nonzero at N+1 was not traced.
- `waza_flag[i] != 0` at N+1 — whether a recognised motion survives the
  metamorphose window. `cmd_main.c` -> `cmd_init` clears `waza_flag[]` at the
  forward swap, and `cmd_move` re-runs the recogniser every frame, but the
  decay was not traced.

**Consequence, and why there is no fix.** Measured, unchanged from §26.5's
control: all six Ken cells are `ps2_same_cell`, and the part indices they
would feed Twelve's OVCT are in range on **both** data sides
(`k7_foreign_oob` 0, `k7_foreign_oob_ps2` 0). By §6.1 a value identical on
both sides is a property of shipping PS2 data and out of scope. Even if the
two unread links were shown reachable, the visible effect is a wrong overlay
sprite that the PS2 shows too. A change here would have to be gated behind
`ArcadeBalance_IsEnabled()`, would introduce an arcade-only divergence where
none exists today, and would be fixing the PS2's behaviour rather than an
adaptation defect. **Not warranted.**

### 26.10.2 Yang: the four cells are dead — and the convention that said so is not sound

§26.6 called Yang's `saca[44..47]` c41 dead using §19's convention: everything
after the first terminating C command never executes. The convention is a
**linear scan**, and the script format's jumps carry a cell index —
`charset.c` -> `comm_jmp`, `comm_jpss` and `comm_jsr` all call
`set_char_move_init2(wk, ctc->koc, ctc->ix, ctc->pat, ...)`, whose
`wk->cg_ix = (ip - 1) * wk->cgd_type - wk->cgd_type` makes `pat` a 1-based
cell index. A jump can therefore land *after* a terminator and revive the
cells behind it, which a linear scan cannot see.

`k7_entry_walk(ci)` replaces the scan with a reachability walk: the entry
points of a script are cell 0 plus every landing (`pat - 1`) that any C cell
anywhere in that character's own tables names, and each walk runs forward
until a terminator. It fails open in both directions — a landing outside the
script, or a `koc` the model does not map, marks the whole script live.

**Measured**, comparing the two methods over every `cg_type 30` cell with a
live `olc` in all 20 characters:

| | verdict |
|---|---|
| Yang `saca[44]` c41 | dead — exactly one reference to the script exists (`cbca[47]` c3, `pat` 23), landing on c22, which is itself the terminating `comm_jmp (5, 75, 1)`, so that walk stops on the cell it lands on |
| Yang `saca[45..47]` c41 | dead — **zero** references to those scripts anywhere in Yang's tables |
| Dudley `saca[72]` c33 | dead — the `comm_jpss (5, 72, 37)` at c18 that §26.4 names lands at 0-based cell 36, *past* c33, so it does not revive it |
| Ibuki `saca[27]` c33, `saca[60..62]` c17 | **live** — reached by a `comm_rja7`/`comm_jmp` landing past the terminator; the linear scan called all four dead |

So Yang's verdict is sound and now rests on an entry-point closure rather than
on a convention. The convention itself is not sound, and the audit no longer
uses it for this gate. Ibuki's gate is `closed`, so the four reclassified
cells change counts but raise no hazard: `xcopy:gated(24)(5 dead)` becomes
`xcopy:gated(24)(1 dead)`, and `k7_foreign_cells` 19 -> 23,
`k7_foreign_dead` 5 -> 1, `k7_foreign_oob` 19 -> 23, `k7_foreign_oob_ps2`
18 -> 22. Those are the only computed values in `cg_audit.json` this pass
changes.

Yang's remaining `unmodelled` is the nine `atca` type-64 cells with
`canc 0x24`, i.e. the same §26.10.1 pre-empt question as Ken's — but with
`k7_foreign_cells` 0 there is nothing behind the gate to protect.

### 26.10.3 The seven characters with nothing to protect: judged, not closed

Ryu, Yun, Sean, Akuma, Chun-Li, Makoto and Q report `xcopy:none` in the table
(zero `cg_type 30` cells outside the rebirth script that select a live `olc`)
while their `k7_gate` field reads `unmodelled`. Closing those gates would mean
answering exactly the §26.10.1 question — `meoshi_hit_flag` and `waza_flag` at
N+1 — for a set of characters where the answer protects nothing: with no
foreign cell, case 4 firing late has no wrong part index to consume.

**Judgement: not worth closing, and the split field is the right shape.** The
gate and the consequence are deliberately independent (§26.6), so that a
future data change which *adds* a foreign cell to one of these seven is caught
by the existing machinery rather than by a gate that was closed on today's
data. `xcopy_flag()` already reports `none` ahead of the gate for them, so the
open gate costs no false alarm. The census that drives it is honest: the
`dmca` type-0 cells (Chun-Li, Makoto and Q's only reason) and the `nmca`
type-2/64/7 cells are real cells with real cancel bits.

One narrowing was examined and rejected. `plpdm.c` has exactly two writers of
`routine_no[1] = 0` — the guard-recovery exit (`routine_no[2] = 38`, which
sets `cg_type = 0; cg_next_ix = 0` and calls `char_move_wca`) and
`Damage_17000`'s auto air recovery (`routine_no[2] = 23`, which calls
`exset_char_move_init(&wk->wu, wk->wu.now_koc, dm17_to_nm23_change[...])`) —
so "any `dmca`/`btca` cell can be current at arming" is looser than the code
requires. But normal state runs `nmca` continuously, so **any** `nmca` cell
can be current at frame N regardless, and Ken's two flagged cells are `nmca`.
Narrowing the damage half would close Chun-Li, Makoto and Q — three characters
with nothing to protect — and would not move Ken, Yang, Ryu, Sean or Akuma.
Not worth the added model surface; recorded here so the next pass does not
re-derive it.

### 26.10.4 The three method gaps, closed

**The forward swap is now a function.** `k7_forward_gate()` reads case 0's
marker from `effk7.c` the same way `parse_k7_rebirth()` reads case 3's and
case 4's, then sweeps Twelve's own tables. Case 0 fires on a `cg_type 20` cell
while the master is still Twelve — so that cell's `cg_olc` was decoded against
Twelve's OVIX — and then rebinds every table to the target's, with
`eff01.c` -> `effect_01_move` restarting the overlay the same frame. A marker
(or a following cell, decoded against the target's OVIX once rebound) with a
nonzero `olc` would carry an index across. **Measured**: Twelve has **40**
`cg_type 20` cells, 32 live and 8 dead by `k7_entry_walk`; every live one
selects `olc 0`, and every cell between it and the next C command selects
`olc 0`. Nothing is carried, so the forward swap is closed for all 20 targets
at once with no per-target table comparison — the same conclusion §25.5
reached by hand, now re-derived on every run. `k7_fwd_gate` is `closed` on
every row.

**The post-rebind tables are now checked, not inspected.** `k7_swap_gate()`
gained two checks for the state after case 4 rebinds to Twelve's tables:

- the post-marker cells' hit indices, read against Twelve's `hiit`
  (`charset.c` -> `check_cgd_patdat`: `cg_ja = hit_ix_table[cg_hit_ix]`).
  **Measured**: every post-marker cell of all 20 rebirth scripts decodes to
  hit index **452**, and Twelve's `hiit` holds **503** entries — in range,
  20/20, confirming §26.9's inspected figure.
- the C command that ends the script, which jumps through the rebound
  `char_table` (`charset.c` -> `set_char_move_init2`:
  `char_table[koc][index]`). **Measured**: all 20 exit with the identical
  `comm_jmp` (code 3) to `koc 5` (`saca`), `ix 53`, `pat 1`, and Twelve's
  `saca` holds **75** entries — in range, 20/20, confirming §26.9's
  inspected `comm_jmp (5, 53)`.

Either check failing appends to `unmodelled` and keeps the gate open.

**`pull_effect_work` was never a hazard.** §26.9 worried that a slot whose
previous occupant left `dead_f = 1` would make K7 abort on its first frame.
It cannot. `effect.c` -> `push_effect_work` — the only way a slot returns to
the free queue — calls `SDL_zeroa(frw[qix])` before relinking it, and
`effect.h` declares `uintptr_t frw[EFFECT_MAX][448]`, so `frw[qix]` is a
448-word array and `SDL_zeroa(x)` (`SDL_memset((x), 0, sizeof((x)))`) clears
the entire WORK, `dead_f` included. `effect.c` -> `effect_work_init` likewise
zeroes all of `frw` at startup, so the first use of a slot is clean too. The
clear is on the *free* side rather than the *pull* side, which is why reading
`pull_effect_work` alone suggested otherwise. `effect.c` contains **no**
`ArcadeBalance_IsEnabled()` call, so this is identical on both arms — but the
point is moot, because the hazard does not exist on either.

### 26.10.5 Corrections to §26 (recorded, not silently edited)

- **§26.3 row 8** says of frame N+1 "Input is dead this frame". True for
  `sw_lvbt`, `sw_new` and `sw_now` — which is what its `check_nm_attack`
  argument needs, so that argument stands — but **false** for `sw_old`,
  `sw_off`, `sw_chg` and `old_now`, and `sw_chg` is the only word the shipped
  command data ever reads for a special (§26.10.1). Row 8's conclusion
  (Ken and Yang stay open) is unchanged; its stated reason was too strong in
  one clause and is now replaced by a positive refutation.
- **§26.6**'s Ibuki row read "19 + 5" and "18 of 19 PS2 identical". Corrected
  to "23 + 1" and "22 of 23": four cells the §19 convention called dead are
  reachable by a jump landing past the terminator (§26.10.2). The row's
  verdict — gate closed, consequence past Twelve's tables — is unchanged.
- **§26.6**'s Yang row justified "dead" by "post-terminator". Corrected to an
  entry-point closure; the verdict is unchanged and now rests on something
  sound.
- **§26.9** bullet 4 called `pull_effect_work`'s uncleared `dead_f` a real
  (if out-of-scope) trap. It is not a trap at all — the slot is zeroed on
  free (§26.10.4).
- **§19**'s terminator convention is not sound as a reachability test
  wherever a script can be entered by a jump. This section changes it only
  for the X.C.O.P.Y. gate (`k7_entry_walk`); every other consumer of
  `TERMINATORS` in `cg_audit.py` is untouched, and whether they need the same
  treatment was not examined.

### 26.10.6 What this does not establish

- Ken's gate is **not** closed. The two unread links are named exactly in
  §26.10.1: `meoshi_hit_flag != 0` and `waza_flag[i] != 0` at N+1. Closing
  them means tracing the command recogniser's decay across the metamorphose
  window and the flag's lifetime from the master's last landed hit — or
  showing the flagged cells cannot be current with `routine_no[1] == 0`,
  `guard_flag != 3` and `hit_stop == 0`. Neither was done.
- No claim is made that Ken's case is **reachable** in play. What is
  established is that the input-side defence does not exclude it. The
  remaining links may or may not hold; the model reports open because it has
  not read them, per the fail-open rule.
- Yang's and Ibuki's dead/live reclassification rests on `k7_entry_walk`
  seeing every entry point. It sweeps C cells in the character's **own**
  tables only; a jump from another character's script, or an entry written by
  C code rather than by a script command, would not be seen. No such entry is
  known, and none was searched for.
- The §19 convention was corrected only where this gate uses it. Its other
  uses in `cg_audit.py` (§21.6's converter-artefact classification among them)
  were not re-derived, and their counts are unchanged by this pass.
- The CPS3's own overlay engine was still not disassembled (§24.7/§26.9).

---

## 27. Over-declared `location_data[]` spans: CLOSED — nothing executes a tail, and the hashed tails are deterministic (eleventh pass, 2026-09-06)

**Citation style for this section.** As in §21-§26: this document is not in
`tools/doc-citations/baselines.txt`, so everything below cites a **symbol**
(`file` -> `function`/`table`) or the exact text of a line. Code was read at
`new-stuff` @ `56f4f52a`. Every number marked **measured** was produced by
`tools/arcade-audit/cg_audit.py` (`span_closure()`, `span_results()`,
`caua_hosa_fit()`, added in this pass) against the same `rom.bin` (md5
`909f5abec4b6b21bf7d2a452a03fdfcc`) and `SF33RD.AFS` the rest of this document
uses.

**Headline.** §7.6/§19.7 left two consequences of the 106 over-declared script
spans (and Remy's CAUA/HOSA, §15.6) unseparated. Separated and adjudicated:

1. **Execution.** No path the engine can take forms a cell index inside an
   over-declared tail. Every writer of the cell index `cg_ix` was enumerated
   (§27.1) and the closure taken from every entry the C can form (§27.3). The
   only cells reachable past a *first* terminator are **18**, in three spans —
   Dudley `caca[6]` cells 2-12, Dudley `saca[87]` cells 11-16, Elena
   `atca[159]` cell 24 — and all of them are script data (entered by a `jsr`
   or a forward `comm_wcne` that PS2 carries byte-identically), every sprite
   among them in bounds (Dudley's are CG 5537-5543, group 5, his own). Remy's
   CAUA/HOSA tails are indexed by nothing: `max(HIIT.cuix) = 6 = 7 − 1`,
   `max(HIIT.hoix) = 11 = 12 − 1` — exact fits (§27.4).
2. **Digest.** `ArcadeCharData_ComputeDigest` hashes `span->data, span->size`
   for all 500 spans — the *decoded* image, tails included: **49,288 B of
   2,590,884 B (1.90%)** is decoded ROM past the real data (35,696 B in the
   106 script spans, 13,592 B in Remy's CAUA/HOSA). But `rom_load.c` accepts
   exactly one ROM (four SIMM slices, each SHA-256-pinned — `rom_load.c` ->
   `expected_entries[]`, "Digests pinned 2026-08-23 from a known-good
   sfiii3nr1 set"), and `read_char_table`'s decode of a tail is a pure
   function of those bytes (`SDL_memset(result, 0, location.size)`, then
   `remap_cg_se`/`remap_cg_number` on every cell, no other input). Every peer
   therefore hashes the same 49,288 bytes. **The slack is hashed, junk, and
   harmless.** Tightening the sizes would move the digest and force every
   netplay client to update in lockstep for no behavioural change; this pass
   makes **no code change**, like the two sibling lanes that declined
   digest-moving fixes this week.

The §19 terminator convention is **not** load-bearing here: the real end of
every span is derived from reach (§27.2), and §19.7's column is corrected by it.

### 27.1 Every writer of `cg_ix`, and what each can form

`cg_ix` is a `u32` word index into the current script (`charset.c` ->
`check_cm_extended_code`: `cpc = (UNK11*)(wk->set_char_ad + wk->cg_ix)`), with
stride `cgd_type` words. The C indexes with no bound: a cell index may be
negative (the header), or run past the script into the next one, or past the
span. `grep 'cg_ix *=' src` — the complete list:

| Writer | Forms | Modelled as |
|---|---|---|
| `check_cm_extended_code` `cg_ix += cgd_type` | k+1 | sequential edge |
| same, `if (wk->cg_next_ix) cg_ix = (cg_next_ix - 1) * cgd_type` | the cgd-6 cell's byte | absolute edge |
| `check_cgd_patdat` `cg_wca_ix = cg_type & 0x7F` → `char_move_wca`/`comm_wca` | `(type & 0x7F) − 1` | absolute edge |
| `hitcheck.c` `((as->cg_extdat & 0x3F) - 1) * as->cgd_type` (cases 0x81/0x41/0x01) | `(ext & 0x3F) − 1` | absolute edge |
| `pls03.c` -> `check_renda_cancel` `cg_eftype * cgd_type - cgd_type * 2`, gated by `cg_cancel & 16` (`pls00.c`) | `eftype − 1` | absolute edge on cells with `canc & 0x10` |
| `comm_end` `(ctc->pat - 2) * cgd_type` | `pat − 1` | absolute edge |
| `comm_ixfw` / `comm_ixbw` | k ± `pat` | relative edge |
| `decord_if_jump` (23 conditional commands): `0x4000` fwd, `0x8000` back, `0x2000` `decode_if_lever[]`, else `(ix - 2) * cgd_type` | k ± (w & 0xFF), or `w − 1` | every operand a target, condition ignored |
| `set_char_move_init2(koc, index, ip)` from `comm_jmp`/`jpss`/`jsr`/`rapp*`/`rapk*` and the register jumps `comm_uja*`/`umja`/`uhsja` after `comm_rja*`/`rmja`/`rhsja` | `(table koc, script ix, cell ip − 1)`; `ip <= 0` → 1, `index < 0` → 0 (the `#if !defined(CPS3)` clamps: `CPS3` is not defined, `CMakeLists.txt`) | seed; every stored triple is a global seed (the register persists across script switches) |
| C literal entries: `appear.c` (nine `set_char_move_init2(&wk->wu, 9, ix, ip, 0)`), `win_pl.c` `(9, 36, 7)`, `plpat00.c` `(5, 60, 8, 1)` | `yuca`/`saca` at `ip − 1` | seeds |
| `plpcu.c` `char_move_index(emwk->wu.curr_rca->catch_nix)` on `cuca[cmyd.ix]` | thrower's RICT row `cg_rival + CHAR_3SX_TO_ARCADE(caught) − 24` (`catch_table_offset`) | throw census, §27.3 |
| `exset_char_move_init` (keeps `cg_ix`, `cg_next_ix`, `cg_wca_ix`): `pls00.c` Elena `nmca[36]` → `nmca[0]`, `plpdm.c` `Damage_17000` → `dmca[dm17_to_nm23_change[ci]]` | the source cell's index | one-step carry |
| `appear.c` -> `Appear_14000` `work = cg_ix / cgd_type; set_char_move_init2(0, 0, work + 1)` (own index, read while running `yuca[0x3C]`, when the *opponent's* `Appear_free` is 1); `win_pl.c` `work2 = cg_ix / cgd_type + 2` into `yuca[work + 32]` from `nmca[0]` | the source cell's index (+1) | one-step carry from the fixed source script |
| `set_char_move_init(koc, index)` from C, any state | cell 0 of any script in a table the C names by literal koc: `nmca dmca caca cuca atca saca btca yuca` (measured over `engine/` + `animation/`; `exca`, `cbca` are entered only by script triples) | seeds |
| `comm_retmj` `cg_ix = cmb2.pat` (raw), `comm_back`/`ret`/`abbak`/`nex` | a position saved from a reachable cell (+1) | already an edge |
| `effk5.c` -> `get_okuri_time` | a look-ahead over the master's script that follows only `comm_end`/`ixfw`/`ixbw` and stops at every `k5_exc_check == 2` code, which includes every terminator | a subset of the closure; not a writer |

Not writers: effects bind their own `char_table` (`eff*.c` `*ewk->wu.char_table
= _..._char_table`); the two that call `set_char_base_data` and then
`set_char_move_init2` with a non-zero `ip` (`eff13.c` charset 11, `effc3.c`
charset 17) bind slots that `charid.c` -> `copy_char_base_data` overwrites with
effect tables; `eff50`/`eff09`/`effc8`/`effk5` mirror the master's `cg_ix` on
their own scripts; `netplay/game_state.c` restores saved values; nothing in
`src/training/` or `src/port/config/` writes it.

**Two engine facts worth recording.** (i) `cgd_type 1` scripts (the `yuca`
intro/win tables) stride **4 bytes** in the executor (`cg_ix += cgd_type`,
`set_char_ad` is `u32*`; `setupCharTableData` copies `cgd_type` words) while
`read_char_table` lays their cells out 8 bytes wide (`8 + max(cgd_type*4 − 8,
0)`); the executor therefore runs each sprite's `olc`/`num` word and each
command's `ix`/`pat` word as a command of its own. The data is built for it —
those words decode as `comm_dummy` (0), `rja6`/`rja7` register stores — and
the PS2 data is byte-identical, so this is the engine's contract, not a port
defect; it is why the model reads cells at the executor's stride. (ii)
`comm_ydat` (code 33) appears in **no** script of any character on either
release (measured: 0 cells arcade, 0 cells PS2), so `cmyd.ix` only ever holds
its initial 0 and a caught player always runs `cuca[0]`.

### 27.2 The true extent of every span (measured)

A span's **real end** is the last byte any reachable cell touches, or the first
terminator's read-end if that is later. The §19.7 column undercounts wherever a
script continues past a mid-script terminator; 14 of the 106 spans do:

| Character | Section | Declared | §19.7 real end | Real end by reach | Junk | §19.7 slack |
|---|---|---|---|---|---|---|
| REMY | `yuca` | 0x73E0 | 0x11C0 | 0x11C0 | **0x6220** | 0x6220 |
| HUGO | `saca` | 0x4164 | 0x374C | 0x3754 | **0x0A10** | 0x0A18 |
| TWELVE | `saca` | 0x5E50 | 0x55B0 | 0x55B0 | **0x08A0** | 0x08A0 |
| URIEN | `saca` | 0x3A54 | 0x36E4 | 0x36F4 | **0x0360** | 0x0370 |
| NECRO | `saca` | 0x3FF8 | 0x3CC8 | 0x3CD0 | **0x0328** | 0x0330 |
| ELENA | `saca` | 0x6638 | 0x63B0 | 0x63C0 | **0x0278** | 0x0288 |
| SEAN | `caca` | 0x12C0 | 0x1040 | 0x1050 | **0x0270** | 0x0280 |
| TWELVE | `cbca` | 0x089C | 0x06BC | 0x06BC | **0x01E0** | 0x01E0 |
| URIEN | `atca` | 0x290C | 0x2794 | 0x279C | **0x0170** | 0x0178 |
| DUDLEY | `saca` | 0x6AE4 | 0x6944 | 0x69AC | **0x0138** | 0x01A0 |
| NECRO | `caca` | 0x2124 | 0x1FF4 | 0x2004 | **0x0120** | 0x0130 |
| DUDLEY | `caca` | 0x07B8 | 0x06B0 | 0x07B8 | **0** | 0x0108 |

`DUDLEY caca[6]` is the instructive one: cell 0 is a 250-frame hold, cell 1 is
`comm_roa` — where §19 stopped — and `caca[1]`/`caca[2]` both `jsr` to
`(2, 6, 3)`, i.e. `caca[6]` **cell 2**, which is `comm_wset` followed by nine
sprite cells (raw CG → remapped 5537-5543, `obj_group_table` group 5, Dudley's
own) and a `comm_ret` whose 8 read bytes end exactly at the declared size. The
span is exactly right; only the metric was wrong. `DUDLEY saca[87]` (cells
11-16 via the `comm_wcne` `0x4007` of §19.3, ending in `jmp saca[86]`) and
`ELENA atca[159]` (cell 24, `jmp nmca[34]`, via `0x4002`) are the other two.

Totals: **106 spans, 35,696 B of junk** (§19.7 said 36,680), the largest
Remy's `yuca` at 25,120 B. Remy's `caua` (777 elements declared, 7 real) and
`hosa` (941 declared, 12 real) add 13,592 B.

### 27.3 The closure, and what keeps its gate open

`cg_audit.py` -> `span_closure(ci)` takes the closure over positions — a frame
is `(table, script)` with that script's stride; a node is a cell index in it,
negative or past the script allowed, as the C allows — from every seed in
§27.1, with every operand of every conditional a target. Two cross-character
couplings are iterated to a fixpoint in `span_results()`: the **throw census**
(a thrower's reachable `cg_rival` cells select the caught player's RICT row,
`ydat = {0}` per §27.1(ii)), and the **X.C.O.P.Y. morph** (`effk7.c` ->
`K7_move_type_0` case 0 rebinds the tables and touches no register, so Twelve's
stored `rja*`/`rmja`/`rhsja` triples resolve against the target's tables at the
target's `uja*` consumers, and the target's against Twelve's on the reverse
marker). A donor triple is applied only where the recipient has a consumer
reachable from a C-side entry without passing a setter of that register
(`span_stale()`, `SPAN_REG`).

**Result (measured, both closures):** the reachable cells past a first
terminator are the 18 of §27.2 — **0 out of bounds**, 0 in a tail. The gate is
nonetheless reported **`unmodelled`** for every character, for three reasons
the model refuses to guess at, all of them **byte-identical on PS2** (§6.1):

- **Registered jumps whose script index is beyond the pointer table** (81
  reasons): Gill's cgd-1 `yuca[68..]` cell 9 is the `num` word `0x0400`
  executing as `rja7 (0, 1024, 0)` → `nmca[1024]` of a 63-entry table; Elena's
  one-cell `saca[10..31]` run off their end into the next script's header,
  which executes as `jpss (9, 3072, 273)` → `yuca[3072]`; Makoto's
  `atca[108..]` cell 0 is `jsr (8, 38, 1)` → `cbca[38]` of a 30-entry table
  (PS2: same cell, same 30 entries). What `char_table[koc][index]` reads past
  the table is a decoded cell interpreted as an offset — computable, not
  computed here.
- **Ken's `rja4 (5, 44, 55)`** (`cbca[0]` cell 3, PS2-identical) enters
  `saca[44]` at cell 54 of 53 and runs misaligned through the next script; the
  garbage cells carry `cg_rival` 4320/16384/32768, whose RICT rows fall
  outside his 1,080-row table — so every caught character's census inherits
  three unplaceable rows (60 reasons, 3 per character). Hugo's `rja (4, 89,
  12)` (`cbca[16]` cell 0) is the same shape: `atca[89]` has 2 cells on both
  releases, cell 11 is a later script's header.
- **Two jumps with `koc` outside 0..9** (Ken `saca[44]` cell 81, Twelve
  `yuca[16]` cell 2 — both in the overruns above).

These are pre-existing properties of data the PS2 engine runs identically; they
keep the gate open because their landing position is not emulated, not because
any of them reaches a tail (none does: the overruns stay inside the spans they
start in, and every span's junk is measured **after** them). The X.C.O.P.Y.
closure adds only donor triples beyond the recipient's tables (Twelve: 124)
and the positional registers it does not carry (`cmbk`, `cmsw`, `cmlp`,
`cml2`, `cmb2` — listed per recipient in `donor_notes`).

### 27.4 Remy's CAUA and HOSA

`caught_adrs` is indexed only by `HIIT.cuix` (`charset.c` `wk->h_cau =
wk->caught_adrs + wk->cg_ja.cuix`) and `hosei_adrs` by `HIIT.hoix`
(`wk->h_hos = wk->hosei_adrs + wk->cg_ja.hoix`) plus the literal
`hosei_adrs[1]` (`pls01.c`, `pls02.c`, `pls03.c`); `effc2.c`'s `hoix + 1` is
the bonus-stage charset 17, not a player. Over **every** HIIT row, reachable or
not (measured, `caua_hosa_fit()`): Remy `max cuix = 6` against 7 real
elements, `max hoix = 11` against 12 — both exact fits, so the 770 and 929
over-declared elements are never indexed. For the other 19 characters the same
witness holds at the declared length (Sean `caua` and Chun-Li `caua` and Ibuki
`hosa` are under-used but in bounds).

### 27.5 `read_char_table` itself

The decoder writes the last script until `p < end_offset = location.size`,
8 bytes for a command and the stride for a sprite. For all 200 last scripts the
final write ends at or before the malloc (measured: `decode_overrun` 0 B for
every span) — no heap overrun from the slack. The executor's copy of a sprite
cell (`cgd_type` words) at a terminator that sits at the very end of a span
reads only the command's 8 bytes (`decode_chcmd[code](wk, cpc)` reads `UNK11`),
so the §19.5 caveat does not bite either.

### 27.6 The defence, in code

- `tools/arcade-audit/cg_audit.py` -> `span_closure()`, `span_results()`,
  `span_stale()`, `span_throw_census()`/`span_throw_seeds()`, `span_donor()`,
  `caua_hosa_fit()`, `c_start_tables()` (the literal-koc census). Per character
  the JSON gains `span_reach` (per table: `declared`, `term_end`,
  `slack_by_terminator`, `reach_end`, `real_end`, `junk`,
  `past_terminator_cells` with each cell's class, `decode_overrun`; the
  `unmodelled` reasons, `throw_notes`, the X.C.O.P.Y. reasons and stale
  consumer counts; `caua_hosa`) and the stats `span_over_declared`,
  `span_slack_bytes`, `span_junk_bytes`, `span_past_terminator_cells`,
  `span_past_terminator_bad`, `span_gate`, `span_gate_xcopy`,
  `caua_hosa_over_declared`, `caua_hosa_tail_reached`. **No pre-existing JSON
  value changed** (measured: field-by-field diff against `56f4f52a`, 0
  changed, 0 removed, 3,650 added).
- The table's trailing column: `slack:dead(n,B)` — n over-declared tables, B
  junk bytes, nothing reaches them; `+past(k,in-bounds)` when k cells past a
  first terminator are reachable and all in bounds; `REACHED-OOB(k)!` if any
  such cell is out of bounds; then the gate state of both closures. The
  summary block prints the totals, the digest arithmetic and the past-terminator
  spans.
- Kept: `over_declared_sections` (the §7.6 metric) and `arc_parse`'s
  first-terminator stop, so every earlier number reproduces.

DUDLEY row, `cg_audit.py`, before (`56f4f52a`) and after:

```
DUDLEY   7051 |    0    0     0     0    16     0 |     0     0     0     0     0    33     0 | 178/180 r<=177 walk>end-unreached[178:hold<=179/297]  41/43 short  xcopy:gated(3)(1 dead)
DUDLEY   7051 |    0    0     0     0    16     0 |     0     0     0     0     0    33     0 | 178/180 r<=177 walk>end-unreached[178:hold<=179/297]  41/43 short  xcopy:gated(3)(1 dead)  slack:dead(6,352B)+past(17,in-bounds) unmodelled(3)+xc:unmodelled(5)
```

`residual_audit.py` after: output and JSON unchanged, R2b `on a REACHABLE part
: 0`.

### 27.7 Corrections to earlier sections (recorded, not silently edited)

- **§7.6 / §8.G** "Parsed-but-unreachable at run time (execution is bounded by
  each script's own terminator)" — the conclusion stands, the reason does not:
  execution is bounded by the reachable set, which crosses first terminators in
  three spans. **§8.G's fix ("tighten the declared sizes") is withdrawn.**
- **§19.7** `Real end` column: 14 spans later than stated (§27.2 table);
  `DUDLEY caca` is not over-declared; total junk 35,696 B, not 36,680.
- **§19.3** "2 of 200 last scripts contain an intra-script forward jump whose
  target sits past the terminator" — there is a third entry past a terminator,
  by `jsr` from another script (`DUDLEY caca[6]` cell 2), and the cells it
  reaches were **outside the 133,901-cell census**; they are now classified
  (11 cells, all in bounds). §19.6(b)'s "two" is nine sprite cells more.
- **§19.6(a)** says 35,912 of the 36,288 unvisited script-span bytes are
  post-terminator slack; 248 of those bytes are Dudley's live `caca[6]` tail.
- **§15.6** "indices never approach it" for Remy's CAUA/HOSA is now measured
  rather than asserted (§27.4).

### 27.8 What this does not establish

- The landing position of the pointer-table-overrunning jumps of §27.3 is not
  emulated (it would need a byte-exact model of `read_char_table`'s decoded
  image, including the `hit`/`att` field swap and the remaps); those jumps are
  PS2-identical and are the reason the gate reports `unmodelled`, not a
  finding about the tails.
- The X.C.O.P.Y. closure carries the stored-triple registers only; the
  positional registers a C-side setter can hold (`cmbk`, `cmb2`, `cmb3`, and
  `cmsw`/`cmlp`/`cml2` from script) are listed, not applied — applying "any
  reachable cell of Twelve" to every target's same-numbered script is sound but
  does not converge, and says nothing.
- No timing is modelled anywhere: every conditional's every operand is taken.
  That is the direction that keeps the gate open, never the one that closes it.
- Whether the arcade engine's own reader (not disassembled here) treats cgd-1
  cells the way `charset.c` does was not checked; the port runs `charset.c`,
  and that is the reader whose reach matters.

---

## 28. The `*_oob` classes adjudicate themselves — and `check_renda_cancel` cannot fire in a Super Art (twelfth pass, 2026-09-06)

§21.6's last paragraph ruled 31 out-of-range sound codes harmless on the
strength of two things that have since been withdrawn (§22.10's 258-of-781,
§26.10.2's linear-scan convention). Re-deriving that verdict by hand would
have left the *next* one resting on prose again, so the reachability test was
moved into the audit instead: every out-of-range-index violation now carries
the answer with it.

### 28.1 The split, and what it shows

`cg_audit.py` computes `dead = k7_entry_walk(ci)[(sec, si)]` once per script
and writes `"dead": true|false` onto every per-cell violation record. Each of
the seven OOB-index counters became two, and the summary table's columns read
`live+dead`. **Measured**, whole cast:

| class | live | dead |
|---|---|---|
| `a_se_oob` (`sound_effect_request[1024]`) | **0** | 31 |
| `a_effinit_oob` (`effinitjptbl[59]`) | **0** | 53 |
| `a_koc_oob` / `a_koc_unset` | **0** | 52 |
| `a_tama_oob`, `a_sasign_oob`, `a_code_oob`, `a_script_idx_oob` | 0 | 0 |
| `c_mismatch_own_group` (wrong sprite) | **89** | **0** |

The separation is total and it runs the right way: **every class that is an
out-of-range *index* is 100% dead, and the class that is a wrong *value* on a
cell that plays is 100% live.** That is the shape you would predict — an index
nobody can form is data nobody decodes — but it had never been measured, and
it is now a property the audit re-derives rather than a sentence someone wrote.

**Dead rows are counted, not suppressed.** A future data change that revives
one has to show up as a live row appearing, and it cannot appear if the row was
never emitted. The `+31` half of a column is as load-bearing as the `0+`.

### 28.2 `k7_entry_walk` under-approximated liveness — six writers, now modelled

§26.10.2 replaced §19's linear terminator scan with an entry-point closure, but
the closure it built still walked each script *forwards in a straight line*
from its entry points. A script is not a straight line either: six writers of
`cg_ix` besides the dispatch loop's `+= cgd_type` carry an index that stays
inside the same `(table, script)` frame, and each can move the cursor backwards
over a terminator. §27.1 had already enumerated them for the slack closure;
`k7_entry_walk` did not use them, so it could call a cell dead that a
same-script index revives. All six are now edges in `_k7_succ`:

| writer | site | edge from cell `k` |
|---|---|---|
| `comm_end` (code 2) | `charset.c` -> `comm_end`: `cg_ix = (pat - 2) * cgd_type`, then the dispatch loop's `+= cgd_type` | `pat - 1` |
| `comm_ixfw` / `comm_ixbw` (49/50) | `charset.c`: `+= (pat - 1) * cgd_type` / `-= (pat + 1) * cgd_type`, then `+=` | `k ± pat` |
| `decord_if_jump` | 32 `decode_chcmd` slots (§28.5) | `k + (w & 0xFF)` for `0x4000`, `k - (w & 0xFF)` for `0x8000`, `w - 1` otherwise |
| `cg_wca_ix` | `charset.c` -> `check_cgd_patdat` sets it from `cg_type & 0x80`; `char_move_wca` / `decode_if_lever[13]` rewind | `(cg_type & 0x7F) - 1` |
| `cg_extdat` | `hitcheck.c` cases `0x1`/`0x41`/`0x81`: `((cg_extdat & 0x3F) - 1) * cgd_type - cgd_type` | `(ext & 0x3F) - 1` |
| `cg_eftype` | `pls03.c` -> `check_renda_cancel`: `cg_eftype * cgd_type - cgd_type * 2`, gated by `pls00.c` -> `check_cg_cancel_data`'s `cg_cancel & 16` | `eftype - 1`, **only where the cell's `canc` carries `0x10`** |

The last three read `cg_extdat`, `cg_cancel` and `cg_eftype`, which share word
3 of the cell. `setupCharTableData` copies `cgd_type` u32s from `&wk->cg_type`,
so word 3 exists only for `cgd_type >= 4`; below that the fields hold the zero
`set_char_move_init` wrote through `setupCharTableData(wk, 1, 1)` on entry, and
no edge is added.

Nothing among the six is left unmodelled. What still fails **open** — marking
the whole script live rather than risking a wrong `dead` — is: a command code
past `decode_chcmd[125]`; a `decode_if_lever` sub-index out of range; an entry
landing or a same-frame edge outside the parsed cells; a `koc` the model does
not map; and every `cgd_type 1` script (23 cast-wide, all `yuca`), where the
executor's 4-byte grid is finer than the 8-byte one `arc_parse` decodes on so
no index in the script is even expressible.

**Effect on the counts**: three cells the previous walk called dead are live
under the six-writer model, all revived by the `cg_extdat` rewind jumping over
a terminator — Dudley `saca[72]` c33 (from c14, `ext 145` -> `&0x3F` 17 -> cell
16, then the `comm_hjmp` at c17, then sequentially), Remy `saca[28]` c19 and
Remy `saca[29]` c18 (both from their c5, `ext 145`/`144`). All three sit behind
a `closed` X.C.O.P.Y. gate, so they raise no hazard; they move
`k7_foreign_dead` 1 -> 0 for Dudley and 2 -> 0 for Remy, and
`k7_foreign_cells` 2 -> 3 and 5 -> 7. Twelve's forward-swap census moves 32
live / 8 dead to 33 / 7 and `k7_fwd_gate` stays `closed` — the newly live
`cg_type 20` marker still selects `olc 0`. **Yang's `saca[44..47]` c41 stay
dead**, so §26.10.2's verdict survives a strictly stronger model.

### 28.3 `check_renda_cancel` in a Super Art script: closed by enumerating every writer of `cg_cancel`

The renda edge is the one that could have turned `a_effinit_oob` live, so its
gate was read out rather than assumed. `check_renda_cancel` has exactly one
call site — `pls00.c` -> `check_cg_cancel_data`:
`if ((wk->wu.cg_cancel & 16) && check_renda_cancel(wk))` — so the question is
what can make bit `0x10` of `cg_cancel` true.

**Every writer of `cg_cancel` in `src/sf33rd/`** (grep `cg_cancel *(=|\|=|&=)`
outside `Game/ui/`, which only snapshots it — 45 statements, plus the two
structural ones):

- the per-cell copy — `charset.c` -> `setupCharTableData(wk, 0, 0)`, `cgd_type`
  u32s from the cell into `&wk->cg_type`, which refreshes `cg_cancel` and
  `cg_eftype` **together, from the same cell**;
- the clear on script entry — `setupCharTableData(wk, 1, 1)` zeroes six words
  from `&wk->cg_type`, so a freshly entered script starts at `cg_cancel == 0`;
- nine plain assignments, all of them `wk->wu.cg_cancel = 0` (`pls03.c`);
- thirty-six read-modify-writes, whose complete operator set is masks
  (`&= 0`, `&= 0x40`, `&= 0x60`, `&= 0x9F`, `&= 0xBF`, `&= 0xE0`, `&= 0xF7`,
  `&= 0xF8`) and ORs of bits `0x01`, `0x02`, `0x08`, `0x40` and `0x60` — in
  `charset.c`, `hitcheck.c`, `pls00.c` and `pls03.c`.

**No writer anywhere sets bit `0x10`.** So `cg_cancel & 16` is true if and only
if the cell currently being decoded carries `canc & 0x10` in its data — a pure
data property — and because `canc` and `eftype` are refreshed by the same word
of the same copy, the bit and the index it forms always come from one cell.

**The census.** 334 cells cast-wide carry `canc & 0x10`:

| table | cells |
|---|---|
| `atca` (KOC 4) | 329 |
| `saca` (KOC 5) | **5** |
| every other table | 0 |

All five `saca` cells are Yun's, in `saca[31]` — a Super Art script, named by
row 43 of `asstbl_lv_9900_g_arcade`, the table `pls03.c` ->
`check_full_gauge_attack` and `check_super_arts_attack` dispatch from — at
cells 92, 94, 177, 178 and 179. **All five are dead** by entry-point closure,
and all five carry `cg_eftype = 0`, so the index they would form is cell `-1` —
outside the script even if one of them ran.

**Verdict: `check_renda_cancel` cannot fire on any live Super Art cell in the
shipped arcade data.** Yang's `saca[44..47]` c37 stay dead, `a_effinit_oob`
stays 0-live, and `effinitjptbl[59]`-OOB is not a reachable finding.

A second, independent argument was available and is recorded but not relied on:
`wk->current_attack`, which `check_renda_cancel`'s third condition compares
against `sw_now & 0x770`, is written at exactly one site in the whole tree —
`pls03.c` -> `check_nm_attack`: `wk->current_attack = shot_data_refresh(kos)` —
so a Super Art never sets it. The data gate closes first, which is why this is
a footnote.

### 28.4 The reported Yang revival was a too-weak model, not a property of the data

The report this pass was asked to settle was that enabling all six writers flips Yang
`saca[44..47]` c37 (`eff 64`, past `effinitjptbl[59]`) from dead to live, via
`check_renda_cancel` on a live c5 with `eftype 37`. It does not reproduce.
Yang `saca[44]` c5 reads `type 20, ctr 1, ext 151, canc 0, eff 30, eftype 37` —
**`canc` is 0**, so the cell carries no renda-cancel bit and the edge does not
exist there.

Dropping the `cg_cancel & 0x10` guard — treating every cell's `eftype - 1` as
an edge — reproduces the reported result exactly: c37 goes live in all four
scripts and Yang's dead-cell count falls from 1,463 to 868. With the guard the
six-writer model leaves c37 dead in all four. The flip was an artefact of an
unguarded edge; it is recorded here because the guard is one `&`-test away from
being dropped again, and because "the model got stronger and a finding
appeared" is exactly the shape a real finding also has.

### 28.5 §27.1's `decord_if_jump` slot list was 25 of 32 — now derived from source

`SPAN_DECORD`, the hand-written map of `decode_chcmd` slots that reach
`decord_if_jump`, named **25** slots. `charset.c` has **32**. Missing:
`comm_rngc` (44), `comm_mpcy` (88), `comm_epcy` (89), `comm_myhp` (96),
`comm_emhp` (97), `comm_s_chg` (117), `comm_schg2` (118) — and `comm_mpcy`,
`comm_epcy`, `comm_myhp`, `comm_emhp` were also missing from
`SPAN_DECORD_FALL`. A missing jump edge *removes* reach, which is the direction
that fails toward "closed", so this was the unsafe kind of gap.

Both tables are now derived by `parse_decord_slots()`, which reads the
`decode_chcmd[125]` initialiser and each handler's body out of `charset.c` and
asserts it finds 32 — so adding a `decord_if_jump` caller updates the model
instead of silently escaping it.

**Effect on the audit**: reach grows and nothing closes. `span_reach` gains
cells for five characters (Elena +58, Yang +62, Twelve +27, Makoto +6, Akuma
+1), six characters gain one X.C.O.P.Y. donor-jump `unmodelled` reason each,
and Elena's and Yang's `cmb2` stale-consumer counts rise (2 -> 4 and 10 -> 14).
No gate changed state, `span_past_terminator_bad` is still 0 for all 20, and
the violation totals are untouched.

### 28.6 Corrections to earlier sections (recorded, not silently edited)

- **§21.6**'s closing verdict on the 31 `se >= 0x400` cells is unchanged; its
  stated proof is replaced in place, because both supports (the 781 census, the
  terminator criterion) had been withdrawn by §22.10 and §26.10.2. Its example
  list `(0x600, 0x4C8, …)` is now complete at the three values that occur:
  `0x400`, `0x4C8`, `0x600`. Where the 31 live is new information, not a
  correction: 15 distinct ROM byte locations, 8 of them `saca` and 7 `atca`.
- **§26.10.2**'s table row "Dudley `saca[72]` c33 — dead" is **wrong** under
  the six-writer model: c33 is live, reached over the terminator by the
  `cg_extdat` rewind at c14 (§28.2). Its neighbouring conclusion — that
  §26.4's `comm_jpss (5, 72, 37)` lands at c36 and does not revive c33 — is
  still true; it simply was not the only way in. Dudley's gate is `closed`, so
  the correction changes a label and no verdict.
- **§26.10.2**'s Yang rows are confirmed, not corrected: `saca[44]` c41 and
  `saca[45..47]` c41 stay dead under a strictly stronger closure.
- **§27.1**'s writer list is right; the `decord_if_jump` slot table that
  implemented it was not (§28.5).
- **§19**'s terminator convention now has a second consumer that no longer uses
  it. Its remaining uses in `cg_audit.py` (`arc_parse`'s last-script cut, the
  `term_end` scan in `span_closure`) are unchanged and were not re-derived.

### 28.7 What this does not establish

- **No condition is ever evaluated.** All six writers are modelled as edges
  with every branch of every conditional taken and no timing anywhere. That is
  an over-approximation of liveness, which is the direction a `dead` verdict
  needs; it says nothing about whether a given cell is reached *in play*.
- **The entry sweep is still own-tables-only** (§26.10.6): `k7_entry_walk`
  collects landings from C cells in the character's own ten tables. An entry
  written by C code rather than by a script command, or a jump from another
  character's script, would not be seen. `span_closure` models the C-side
  entries; the two models were not unified, and their node spaces differ for
  `cgd_type 1` (4-byte vs 8-byte grid).
- **The renda verdict is about the shipped data, not the engine.** No live
  `saca` cell carries `canc & 0x10` today. Nothing in `charset.c` or `pls00.c`
  forbids one — which is precisely why the edge is in `_k7_succ` rather than
  the verdict being in prose: a data change that sets the bit on a live `saca`
  cell turns the affected `a_effinit_oob` rows live by itself, on the next run.
- `check_renda_cancel`'s other two conditions (`rl_flag == rl_waza`, and
  `pat_status == renda_status_table[sw_new & 3]`) were read but not closed;
  they were not needed.
- **`cgd_type 1` scripts are unmodelled and therefore fully live.** Whether the
  arcade's own reader strides 4 or 8 bytes there is unchecked — the same gap
  §27.8 records — and no violation of any class falls in one, so nothing turns
  on it either way.

---

## 29. The 316 shape-divergent scripts: CLOSED — adjudicated per raw `cg_number`, and the audit now checks it (thirteenth pass, 2026-09-06)

§3 carried "Shape-divergent scripts (316) — **OPEN** — enumerated; §11.4 now
offers an oracle" as the last substantive open item. This pass closes it, and
finds **one real divergence that was never recorded anywhere: Twelve, 44
cells, three scripts** (§8.S).

### 29.1 What the item actually was

The `manu` column counts scripts where the arcade and PS2 cell sequences have
different shapes, so `audit()` cannot pair arcade cell *i* with PS2 cell *i*.
`shape_ok` gates `ps2num`, so for every cell of those 316 scripts **the
class-(c) wrong-sprite check is skipped entirely**. The item was never a
finding; it was the absence of one. Classes (a) and (b) do still run there —
they need only the arcade side plus `obj_group_table` — which is why no
crash-class door was ever open here.

§21.5 adjudicated the same 478 scripts (316 + 162) for the **sound-namespace**
question and concluded "Zero scripts remain undecidable". That is a different
predicate and nothing below disturbs it.

### 29.2 The method, and why it is not circular

`remap()` is a pure function of the raw value — a piecewise range shift
(`arcade_char_data.c`) — so "is the adaptation right for this cell" is a
question about a **raw `cg_number`**, not about a cell position. A raw value
that appears anywhere in a **shape-ok** script of the same character is pinned
there by that script's PS2 counterpart, whichever script later asks for it. So
the pairing the shape mismatch destroyed is not needed: build the oracle
`raw -> delta` from the shape-ok scripts alone, then look each shape-mismatched
cell's raw value up in it.

Per live cell, the verdict is one of:

| verdict | meaning |
|---|---|
| `direct` | the raw is itself observed, and our delta equals the observed one |
| `bracketed` | unobserved, but the nearest observed raw **below and above** both measure the same delta, and ours equals it |
| `bracket_disagree` | the two bracketing observations measure **different** deltas — the band is not uniform across the gap, so nothing is confirmed |
| `unbracketed` | no observation below, or none above |
| `divergent` | an observation — direct, or a bracketing agreement — **contradicts** our delta |

**It fails toward divergence everywhere**, per the house rule that nothing
unmodelled may land on "benign": `bracket_disagree` and `unbracketed` are not
benign verdicts, and a raw observed with two *different* deltas is **dropped**
from the oracle rather than settled by a majority — so an ambiguous raw can
never confirm anything, only fail to. Reachability is `k7_entry_walk` alone:
§26.10.2 withdrew "past the first terminator" as a reachability test, so this
gate does not use it and adjudicates every cell the entry-point closure leaves
live, post-terminator ones included. That is the stricter choice, and it costs
one extra divergent script (Twelve `dmca[3]`) over the terminator-cut variant.

**Harness validation before any new number was trusted.** The re-walk
reproduces §21.5's structural census exactly — 316 shape-mismatched, and the
same per-character split (YUN 31, YANG 30, ALEX/IBUKI/URIEN 21, …). The oracle
independently rediscovers **every** cell §8.N and §8.P already enumerated by
hand, and no others (§29.4) — which is what validates it.

### 29.3 Result — all 316, by script

| class | scripts | meaning |
|---|---|---|
| `direct` | **225** | every live cell's raw is directly observed elsewhere, delta confirmed |
| `bracketed` | **60** | remaining cells bracketed by agreeing neighbours, delta confirmed |
| `no_live_cells` | **20** | no live L-cell to adjudicate at all |
| `unresolved` | **4** | at least one cell the oracle declines to confirm; none contradicted |
| **`divergent`** | **7** | at least one cell whose delta the oracle **contradicts** |
| | **316** | |

Their **3,320 live L-cells**: 2,710 `direct` + 465 `bracketed` +
94 `bracket_disagree` + 0 `unbracketed` + **51 `divergent`**.

So **285 of 316 scripts are positively confirmed clean**, 20 have nothing to
adjudicate, and the residue is 11 scripts — 4 unresolved and 7 divergent, every
one named below.

**Why most of them were never a risk.** The dominant structural driver is that
the PS2 release **decimated** long animations. Elena `dmca[90]` runs arcade
`0x3317..0x3332` consecutively; its PS2 counterpart carries
`0x2AF9, 0x2AFB, 0x2AFC, 0x2AFD, 0x2AFF, …` — exactly the *remapped* arcade
values with roughly every fourth frame dropped. The cell counts differ, so the
index diff refused to compare; the sprite content is identical. 194 of the 316
have a `cg_number` multiset that matches outright.

### 29.4 The divergences — 51 cells, 7 scripts

| char | scripts | cells | ours | oracle | groups (ours -> oracle) | status |
|---|---|---|---|---|---|---|
| **TWELVE** | `dmca[3]`, `dmca[90]`, `dmca[91]` | **44** | `-0xB80` (default) | `-0x600` | 4 / 5 -> **6** | **NEW — §8.S** |
| REMY | `dmca[3]`, `dmca[91]` | 5 | `+0` | `+0x20` | 2 -> 2 | known, §8.N |
| AKUMA | `nmca[27]`, `nmca[28]` | 2 | `-3232` | `-3266` | 15 -> 15 | known, §8.P |

**Remy's 5 and Akuma's 2 are not new.** §8.N's "Caution for whoever picks this
up" table names `0x0636` (`dmca[91]` cell 9), `0x0679`/`0x067A` (`dmca[3]`) and
`0x0685` (`dmca[3]`) as raw values with *no cell-aligned PS2 counterpart*, left
deliberately uncovered when item N landed as discrete rows. §8.P names Akuma's
`0x546B` as "a single, unanimous measured delta blocked only by the no-oracle
rule". The oracle rediscovers precisely those cells and no others — independent
confirmation of both write-ups. What it **adds** is that four of §8.N's twenty
"unmeasured" raws are no longer unmeasured: `0x0685` is bracketed by `0x0684`
and `0x0690`, `0x0679`/`0x067A` by `0x0678` and `0x067C`, and `0x0636` by
`0x0601` and `0x0655` — all six witnesses measuring `+0x20`. That is exactly
the "one row with the interpolation stated explicitly" §8.N asked for. Neither
item is changed here; both remain scoped out.

**Twelve's 44 are new, and are an adaptation defect.** Derivation in §8.S.

### 29.5 The unresolved residue — 4 scripts, 94 cells, each named

- **URIEN `yuca[37]`, `yuca[39]`, `yuca[65]` — 25 cells each (75).** Raw
  `0x5315..0x532D`, bracketed by `0x52EC` (measures `-3192`) and `0x5334`
  (measures `-3168`), which disagree. This is §8.D's Urien territory: the
  neighbourhood `0x52DA..0x52EC` carries a per-value delta *staircase*
  (`-3183, -3184, … -3192`), and `0x52D9` itself is observed with **two**
  deltas — the ambiguity §8.D declined to fix and §8.P re-states as unfixable
  by range-table means. Our remap gives `-3168`, matching the upper witness
  only. **Unread:** whether the staircase continues across the gap.
- **REMY `dmca[90]` — 9 cells.** Raw `0x0828..0x082E` and `0x08D6..0x08D7`,
  bracketed by `0x0744` (`+0x20`) and `0x0C01` (`-0x1E0`), which disagree.
  §8.N warned in as many words that `0x0C01` "must **not** be swept into the
  same range"; the gate reproduces that warning as a bracket disagreement.
  **Unread:** which of the two bands these nine distinct raws belong to.
- Remy `dmca[91]`'s 8 further `bracket_disagree` cells and Akuma's 2 sit in
  scripts already classed `divergent`, so they are counted in the cell totals
  but not among the 4 unresolved *scripts*.

Nothing else in the 316 is unread.

### 29.6 §11.4's hardware oracle was NOT used, and could not be

§11.4 offered frame-exact FBNeo injection plus per-frame CPS3 RAM diffing as
the way to settle this item. It was assessed and rejected on three grounds,
the first two measured:

1. **The primary source is gone.** `~/Desktop/3sx-makoto-1f-link-2026-08-29.md`
   does not exist — `~/Desktop` contains no `.md` file at all. The drive's
   `3sx-makoto-1f-link-BACKUP-before-s18.md` is, as its name says, the copy
   taken *before* §18 was written: it contains **zero** occurrences of
   "REAL ARCADE". §11.4's cited §18 is unreadable from either location.
2. **The rig does not run as recorded.** `/Volumes/KimchDrive` is mounted and
   `tools/`, `d2/game_0` (712 frame dumps) and the `in_*.bin` streams are all
   present, and `fbneo-replay-runner`'s `build/release/fbneosdlarm64` still
   exists. But `sweep.sh`, `sweep2.sh` and `sweep3.sh` resolve their
   input-generation scripts through `SP=/private/tmp/.../677e8288-…`, a
   scratchpad from a session that no longer exists.
3. **It is the wrong oracle for this predicate, not merely an impractical
   one.** CPS3 RAM reports `cg_number` in *arcade* numbering — which we already
   have, exactly, from the ROM. The question here is what our **remap** should
   turn that into, and the target namespace is the PS2's. Real hardware cannot
   pin a PS2-side index. Even with the rig working, it could not answer this.
   It remains the right oracle for §11.4's other two offers — Chun-Li's
   blank-CG intent and the `cbca[19..23]` behavioural difference — which are
   questions about what the arcade *draws*.

Practicality is a distant fourth: it is a per-scenario capture rig, and the
three divergent Twelve scripts are damage reactions needing a specific opponent
attack to enter. Nothing was run.

### 29.7 The defence, in code

`cg_audit.py` -> `manu_delta_gate()` re-derives all of the above on every run
and emits one `manu_cg_delta_divergent` violation row per contradicted cell,
carrying the raw, both deltas, both resulting groups, and the two witnesses.
`cg_audit.json` gains twelve `manu_*` stats per character; the report grows a
`manu:` column, reading in aggregate:

```
shape-mismatched (manu) scripts: 316 = 225 direct + 60 bracketed + 20 no-live-cells + 4 unresolved + 7 DIVERGENT
  their live L-cells: 3320 = 2710 direct + 465 bracketed + 94 bracket-disagree + 0 unbracketed + 51 DIVERGENT
```

The five script classes sum to `manu` by construction, so a class that stops
summing is itself a failure. The gate is not a snapshot of today's verdict: it
recomputes the oracle from the shape-ok scripts each run, so widening a
`CgRemapRange` moves a `divergent` row to `direct`/`bracketed` on the next run
without anyone editing a number here, and a *new* hole appears as a new row
rather than as prose going stale.

### 29.8 Corrections to earlier sections (recorded, not silently edited)

- **§11.1's tier-3 framing is too pessimistic for this item.** "State/shape
  divergence — **not** statically discoverable. No table-bounds oracle exists"
  is true of *shape*, but the cell-content question hiding behind the shape
  mismatch is decidable statically, because the adaptation is a per-raw-value
  function. 285 of 316 are positively confirmed with no oracle beyond the PS2
  data already in hand.
- **§11.4's first bullet** — "the 316 shape-mismatched scripts can be
  adjudicated by running the move on real hardware" — is withdrawn for the
  `cg_number` question, per §29.6(3). It stands for the other two bullets.
- **§21.6's headline sentence is inverted relative to its own worked example.**
  It reads "the arcade BE u32 equals the PS2 LE u32 **with its u16 halves
  crossed**" as the converter-artifact signature, but its Gill `atca[15]` c30
  example (`02 C4 60 00` vs `00 60 C4 02`) is *bit-identical*, not crossed —
  and the section later calls those "7 of the 15 are bit-identical". Worked
  through: for two u16 fields A,B stored BE, a correct per-u16 swap yields PS2
  bytes whose LE u32 is `(B<<16)|A` — the **crossed** relation — whereas the
  blanket-u32 swap yields an LE u32 **bit-identical** to the arcade's BE u32,
  with the two fields transposed. So bit-identical is the artifact and crossed
  is the correct conversion; the example and the later sentence are right, and
  the headline sentence is backwards.
- **§8.N's "20 unmeasured raw values"** is now 16: four are bracketed (§29.4).

### 29.9 What this does not establish

- **That the 285 confirmed scripts render correctly** — only that every live
  cell's `cg_number` is remapped by a delta the PS2 data pins. `cg_se`,
  `cg_zoom`, `cg_effect`, `ctr` and the cell *ordering* are separate questions,
  answered for these scripts by §21 and §22, not here.
- **That Twelve's 44 cells are visibly wrong on screen.** The delta is wrong
  against the oracle (§8.S); what the affected frames look like in play was not
  observed, on device or otherwise.
- **The four unresolved scripts.** Named in §29.5 with exactly what is unread
  for each. They are not claimed benign.
- **Anything about the 162 no-counterpart scripts.** They have no PS2 oracle at
  all, are counted in `extra`, not `manu`, and are untouched by this pass.
  §21.5 settled them for the sound namespace only.
- **The oracle assumes one delta per raw value per character**, which is what
  `remap()` can express. Where the shipped data wants two (§8.D, §8.P), the raw
  is dropped from the oracle rather than adjudicated — correct, but it means
  those raws confirm nothing for their neighbours either.
- **Cell ordering within a confirmed script.** The gate is a per-raw-value
  test; it would not notice two live cells of the same script exchanging
  positions if both raws are separately confirmed.
