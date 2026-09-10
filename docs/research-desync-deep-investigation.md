# Deep Investigation: MiSTer ↔ MiSTer Desync + Black Background
Date: 2026-04-24
Author: deep-investigation agent

## STATUS 2026-08-30 — Candidate R2 re-audited: sound when written, never validated, now inert

Read this before acting on anything below. The document's single most
load-bearing conclusion is **Candidate R2 / Candidate 0a**: that
`chainex_check[2][36]` escapes rollback and is the root of the frame-1274
MiSTer↔MiSTer desync. Three things have happened to it since 2026-04-24.

**1. The mechanism is real and the code is where the document says it is.**
`u8 chainex_check[2][36]` is declared at
`src/sf33rd/Source/Game/system/sysdir.c:12`. There are exactly nine
speculative writes, all of the form `chainex_check[wk->wu.id][... - 20] = 1`,
in four functions of `src/sf33rd/Source/Game/engine/pls03.c` —
`check_full_gauge_attack`, `check_full_gauge_attack2`,
`check_super_arts_attack_dc`, `check_special_attack` — and exactly eight
gating reads in the same file, each guarded by
`wk->spmv_ng_flag2 & DIP2_UNKNOWN_23` (or `DIP2_UNKNOWN_22`). Clearing is
`clear_chainex_check` in `src/sf33rd/Source/Game/engine/plcnt.c:1438`.
An earlier revision of this document placed the write "inside `pl_step_25`";
**no function of that name has ever existed in this tree** (`git log --all -S
pl_step_25` finds the identifier only in this document's own text). The names
above are the real ones. The state machine the passage meant is the `nm_NNNNN`
family in `src/sf33rd/Source/Game/engine/pls00.c`, dispatched by
`process_normal`; `nm_01000`, `nm_02000` and their siblings are what call the
four `pls03` checkers. Nothing named `pl_step` exists anywhere in `src/` or
`include/`. §G.3's "16+ distinct write sites" is also an
overcount — there are nine writes and eight reads.

**2. The conclusion was never validated. It was skipped straight to the fix.**
§G.1 proposed *detection first* — hash `chainex_check` without saving it, and
read the answer off which way the desync frame moved (§G.2's three outcomes).
That experiment was never run. What landed instead is §G.5, the fix:
`chainex_check` is a `GameState` member (`src/netplay/game_state.h`, field
`chainex_check`), copied in `GameState_Save` and restored in `GameState_Load`,
and folded into the focused checksum in `save_current_state`
(`src/netplay/game_state.c`). So **every "NOT in `GS_SAVE`" claim below is
stale** — it has been saved since 2026-04-24. And because the discriminator
was skipped, R2 was neither confirmed nor eliminated: no repro of the
frame-1274 desync exists anywhere in this tree after the fix, and the string
`1274` appears in no other file. The desync was closed by assertion, not by
measurement.

That matters because this document has a demonstrated miss of exactly this
shape. **Candidate 0b exonerated `spmv_ng_save[2]` as "dead code"** on the
grounds that `player_number == 16` is Chun-Li. `CPS3` is not defined in this
build, so `constants.h` makes 16 Makoto — the code was live, and it was a real
mid-battle desync, fixed months later in `9357ef6c`. The reasoning recorded in
`src/netplay/game_state.h` beside the `spmv_ng_save` field. A hand
risk-classification of 258 globals produced at least one confident false
negative; treat its confident positive with the same suspicion.

**3. In netplay the mechanism is now inert, for a reason unrelated to this
investigation.** Upstream `4485a438` ("Statcheck: Fix arcade mode desync",
#267, on this branch 2026-07-21) put **all nine** writes behind
`if (!ArcadeBalance_IsEnabled())`. Netplay then became arcade-only:
`Netplay_ArmAllowed` (`src/netplay/netplay.h`) gates every session entry path
on verified-arcade balance, with `setup_vs_mode` (`src/netplay/netplay.c`)
logging an error if a path reaches it without one. Under arcade balance
nothing writes `chainex_check`, so in any netplay session it is provably an
all-zero array and the eight gates never trip. The `GameState` coverage is not
wasted — `ArcadeBalance_Init` pins PS2 for `--test-enable` runs, which is the
balance the rollback-determinism harness exercises — but **R2 cannot be the
cause of a netplay desync on this branch, and re-running §G.1 as written would
now measure nothing.** Any repro attempt must force PS2 balance
(`balance=ps2`) or it is testing dead code.

**What this means for the reader.** §A's static sweep is cited by
`docs/rollback-determinism-harness.md` as the method that harness exists to
*replace*, not as a premise — nothing downstream is built on R2 being true.
The line-number citations throughout the sections below were exact at
2026-04-24 and have since moved; they are left as written rather than
mass-repointed, per `AGENTS.md`. Prefer the symbol names in this block.

## Evidence summary

1. **Desync at frame 1274** (~21s of play at 60fps) in a **MiSTer ↔ MiSTer** session. Both peers are armv7 32-bit, same binary from same commit. Log line:
   ```
   ⚠️ desync detected at frame 1274  local=0x8f8bdf16 remote=0x87be602b
   [netplay] desync at frame 1274 — terminating session
   ```
2. **Visual symptom: background was all black** immediately before / during the desync. Foreground fighters rendered normally until the cutoff.
3. Earlier Mac↔Mac localhost test (commit `11bdfc13`) ran 3000+ frames with no desync → rules out a pure deterministic-bug that would fire independent of network conditions.
4. Two stock-clocked MiSTers (800 MHz). `input_prediction_window` was 10 at the time of the desync (default since changed to 6).
5. Desync detection is enabled in Release (`config.desync_detection = true` at `src/netplay/netplay.c:491`). Checksum mechanism is GekkoNet's `SessionHealthMsg`, over `u32` produced by `save_current_state` at `src/netplay/game_state.c:1596`.
6. No `states/desync_F*.txt` from the current MiSTer build because `dump_desync_state` is `#if DEBUG` only (`src/netplay/game_state.c:1778`). DEBUG flavor is built but live test is hours/days away.

### Ground-truth notes on the binary

- `file build/mister-runtime-package/bin/3s-arm` reports: `ELF 32-bit LSB pie executable, ARM, EABI5 version 1 ... BuildID[sha1]=ff65afb75fad2cb04ead9cda63ad124278486aac`. **PIE is enabled** — every process load has a different base address, so every function/const-data pointer differs between the two peers. This is important for anything that compares or memcpy's pointer bytes into cross-peer hashable state.
- Focused checksum lives in `src/netplay/game_state.c:1596-1750`. **Release builds compute and emit the checksum** (see the `#if DEBUG` split at `game_state.c:1608,1729-1747`). The DEBUG-gated part is only the per-subsystem ring-buffer + file dumps, NOT the hash itself.
- 32-bit ARM makes the "suspicious pointer sweep" at `src/netplay/game_state.c:1658-1665` a no-op in practice: the heuristic compares a `uint64_t` fused from two adjacent 32-bit words against `0x100000000ULL`. Confirmed in `docs/archive/research-3sxtra-netplay-port.md:559-576`.
  - **[REVISED 2026-04-24]**: this claim is partially wrong. On little-endian 32-bit ARM, `(uint64_t*)&plw_scratch[p]` reads pairs of adjacent 32-bit words. The fused value `v = (high32 << 32) | low32` exceeds `0x100000000` whenever the upper 32 bits are nonzero (i.e. whenever the second 32-bit field in the pair is any value ≥ 1 *and* the pair as a whole exceeds 2³²). The `(v >> 47) == 0` gate then zeros only pairs where the top 17 bits are zero — still matches a huge swath of legitimate 32-bit game-state pairs (any field pair where the second field is in `[0x0001..0x7FFF]`). The sweep therefore **actively rewrites non-pointer PLW bytes on 32-bit**, not zero of them. Both 32-bit peers rewrite the same bytes deterministically given the same state, so this does not by itself cause divergence — but it shrinks the effective hashed surface and could mask or reveal state drift unpredictably when combined with other divergences.

## Evidence refinement (2026-04-24, rollback-axis) [REVISED 2026-04-24 rollback-axis]

**Supersedes both prior passes' framings.** The primary axis is **whether real rollback occurs during gameplay** — not architecture (32-bit vs 64-bit). The arch-correlation in the prior passes is incidental: MiSTer happens to be the slow, real-RTT peer in our test matrix, but the bug mechanism is rollback-unsafe state, not word-width-specific layout.

Three-scenario correlation under the rollback-axis interpretation:

| Scenario | Arch | RTT | Rollback depth | Black BG | Desync |
|---|---|---|---|---|---|
| Mac ↔ Mac (localhost) | both 64 | ~0 ms | ~0 (no speculation ever diverges from confirmed inputs) | No | No |
| MiSTer ↔ Mac (cross-arch, internet) | mixed | ~50 ms | real (both sides) | **Both peers show black** | Yes |
| MiSTer ↔ MiSTer (same-arch, internet) | both 32 | ~20 ms | real (both sides) | **Both peers show black** | Yes, at frame ~1274 |

Localhost Mac↔Mac runs clean because at 0 ms RTT Gekko's speculative advance never diverges from confirmed inputs — `step_game` runs once per frame and is never rolled back. With any real RTT, Gekko rolls back and resims across every late input; if any sim-path global escapes `GS_SAVE` / `GS_LOAD`, rollback contamination becomes possible.

### Layer-scoped BG black

Per the user: the BG black is **layer-scoped**. Health bars, SA meter, character sprites, HUD, hit effects all continue drawing normally. Only the stage BG goes black. This matches `BG_Draw_System()` at `src/sf33rd/Source/Game/system/sys_sub.c:899-930`, which is the ONE code path that emits BG-tile polygons (`scr_trans`) and which has exactly two control flags — both covered by `GS_SAVE` but NOT hashed:

- `bg_disp_off` (u8; decl at `src/sf33rd/Source/Game/stage/bg.c:44`, written at `src/sf33rd/Source/Game/stage/bg.c:1508` inside `Bg_Disp_Switch`, and at `src/sf33rd/Source/Game/stage/bg.c:240`, `src/sf33rd/Source/Game/stage/bg_sub.c:1111,1202`, `src/sf33rd/Source/Game/ending/end_main.c:250` in init/reset paths; saved at `src/netplay/game_state.c:596`): when non-zero, `src/sf33rd/Source/Game/system/sys_sub.c:911` routes every layer through `scr_calc()` instead of `scr_trans()` — no BG polygons emitted. Sprites/HUD untouched.
- `Screen_Switch_Buffer` (u16 bitmask; written only by `Bg_On_R/W`, `Bg_Off_R/W` at `src/sf33rd/Source/Game/stage/bg.c:1423-1443` and by `Scrn_Renew` at `src/sf33rd/Source/Game/stage/bg.c:1445-1447`; saved at `src/netplay/game_state.c:582`): per-layer bit; cleared layers skipped at `src/sf33rd/Source/Game/system/sys_sub.c:907`. Again sprites/HUD untouched.

The sprite/HUD pipeline (`seqs_w`, `mts[]`, `njdp2d_w`, HUD poly submit at `src/port/sdl/sdl_game_renderer.c`) is completely independent of these two flags — matching the observation.

### Symmetric contamination

Per the user: both peers go black **simultaneously**. They share the same GekkoNet-authoritative input stream and roll back through the same moments. If rollback writes a stale value into an unsaved flag, both peers see the same stale value on resim — so both peers' `bg_disp_off` or `Screen_Switch_Buffer` exits fail at the same frame. The filter for any candidate root cause is therefore:

1. Mechanism fires **during speculative rollback** (not only during confirmed-only advance, so the Mac↔Mac clean baseline is naturally explained).
2. Mechanism produces **symmetric contamination** (both peers compute identical stale state on resim; the BG symptom is concurrent).
3. Mechanism plausibly gates the **BG-off exit path** (or equivalently, the code that clears `bg_disp_off` / re-sets `Screen_Switch_Buffer` bits) or feeds a downstream PLW divergence that in turn drives one of those flags.

A field that writes via a brief speculative event — hit lands, SA activates, projectile spawns, seraph fires, Twelve metamorphose — then fails to clear because rollback erased the real-advance's "exit" transition, is the signature. Such fields escape `GS_SAVE` and persist between `load_state → advance_game` cycles.

### Why frame 1274 specifically

Frame 1274 is ~21 s into the match. Plenty of time for the first SA bar to fill, the first projectile cross-fire, a Gill seraph fireball, or a Twelve X.C.O.P.Y. transform. Each late-input triggers a small rollback; after thousands of rollbacks the odds that one straddles a speculative set/unset boundary on an unsaved flag approach 1. Frame 1274 is the first divergence big enough to flip a byte in the hashed `plw_scratch` — not a scheduled timer.

### Why PIE and cross-arch are red herrings for THIS bug

Prior passes weighted PIE pointer divergence and cross-arch layout heavily. Both matter in theory but not for this repro:

- PIE: `sanitize_plw_pointers` at `src/netplay/game_state.c:1522-1568` zeros pointer members of PLW before hashing. Post-sanitization, the hashed bytes are position/status/counter integers — identical on both peers.
- Cross-arch: not relevant to MiSTer↔MiSTer (both armv7 32-bit, same binary from same commit).

Both prior framings collapse to the same observable (desync on a hashed-byte mismatch) but the mechanism is rollback-unsafe state — not pointer values or word width.

## Evidence refinement (2026-04-24, 32-bit axis) [SUPERSEDED 2026-04-24 rollback-axis]

> **[SUPERSEDED 2026-04-24 rollback-axis]** — The analysis in this section has been superseded by the rollback-axis framing above. It is preserved for history. The factual observations (both peers go black simultaneously; Mac↔Mac localhost runs clean; desync at frame 1274) are correct. The interpretation that "32-bit-ness" is the primary correlation is now understood to be incidental: MiSTer hardware is both 32-bit AND the slow networked peer, and the rollback-axis framing explains all the same evidence without invoking word-width-specific behavior.

**The "black background" symptom correlates sharply with the presence of at least one 32-bit peer, not with asymmetric one-peer divergence.** The prior report assumed the black BG was asymmetric (one peer goes black, the other doesn't); that assumption is **incorrect**.

Three-scenario correlation (live tests):

| Scenario | Peer A arch | Peer B arch | Rollbacks occur? | Black BG on peer A | Black BG on peer B | Desync fires |
|---|---|---|---|---|---|---|
| Mac ↔ Mac (localhost) | x86_64/arm64 (64-bit) | same (64-bit) | No (~0 RTT) | No (3000+ frames clean) | No | No |
| MiSTer ↔ Mac (cross-arch) | armv7 (32-bit) | 64-bit | Yes | Yes | Yes | Yes (expected — `SessionHealthMsg` struct-layout mismatch) |
| MiSTer ↔ MiSTer (same-arch) | armv7 (32-bit) | armv7 (32-bit) | Yes | Yes | Yes | Yes (at frame 1274) |

Correct failure model (pre-revision): **the sim takes the same bad BG-off path on ALL peers when at least one peer is 32-bit**. "Black BG" is symmetric across peers in every observed case. Desync is caused by a parallel divergence (different mechanism, or the same root mechanism expressed through residual asymmetry like rollback-depth differences) that is correlated with the same pressure that produces the black BG.

Implications for the prior ranking (pre-revision):

- Any candidate whose mechanism predicts "only one peer goes black" is **ruled out** as the dominant explanation for the BG symptom. `Bg_Disp_Switch(1)` firing on one peer only (prior Candidate 1) does NOT fit. Effects firing asymmetrically (part of prior Candidate 4) do NOT fit.
- Any candidate whose mechanism is **32-bit-specific** or **rollback-only** (and therefore dormant on the Mac↔Mac zero-RTT baseline) moves up in rank.
- The prior top pick (Candidate 0a: `chainex_check` rollback-unsafe) is re-evaluated below: its mechanism produces symmetric stale state when both peers speculate identically, so it could plausibly fit "both peers black" — but only indirectly, and the cross-peer HASH divergence still requires rollback-depth asymmetry.

## Section A — State-coverage audit

> **⚠ CORRECTION 2026-08-30 — §A.2.2 / §A.2.3 / §A.2.4 are cited as the source
> of 16 live suppressions in `tools/rollback-determinism/allowlist.txt`. Three
> of those sixteen were re-derived against the code on 2026-08-30 and FAILED.**
>
> - **The classification method here is a category judgement, not a read
>   trace.** §A.2.2/.3/.4 sort globals by subsystem and conclude "write-only
>   fan-out; not fed back" from the subsystem name. Re-tracing the reads found
>   `cseSysWork` gating `ldreq_result` and hence `Exit_No`/`Exit_Timer`/`G_No`/
>   `G_Timer`; `mts_ok` gating PLW canon fields `my_clear_level`/
>   `current_colcd` and the saved `frw` field `my_trans_mode`; and `ppwork`
>   gating a write into `Convert_Buff`. All four destinations are in the
>   rollback save set. That is on top of `spmv_ng_save` (§A.2.1, exonerated on
>   the wrong character enum) and `ColorRAM` (§A.2.2, which fed saved `frw[]`).
> - **The subsystem lists do not cover what cites them.** Of the 16 allowlist
>   entries filed under these three sections, only five — `ColorRAM`,
>   `colPalBuffDC`, `mts_ok`, `ppwork`, `vib_req` — are actually NAMED here.
>   §A.2.3 names `bgm_*`/`adx_*` and none of the six sound entries; §A.2.4
>   names `PrioBase`/`cmtx`/`bg_priority`/`TopHUD*` and none of the eight
>   render entries. Eight of the sixteen are outside this sweep's universe
>   (`src/sf33rd/Source/Game/`) altogether — they live in `src/port/`,
>   `src/platform/`, `src/main.c` and `AcrSDK/`, so §A never examined them.
>
> **Do not use §A.2.x to justify a new suppression.** The per-entry read traces
> now live in `tools/rollback-determinism/allowlist.txt`, next to the
> suppressions they license. Every "NOT in `GS_SAVE`" claim below is also stale
> since 2026-04-24 — see the STATUS block at the top of this document.

This section enumerates every mutable global in `src/sf33rd/Source/Game/` and classifies it by (a) whether it is copied into the save/restore `State` struct via `GS_SAVE/GS_LOAD` in `src/netplay/game_state.c`, (b) whether it feeds the focused checksum that GekkoNet compares across peers at `src/netplay/game_state.c:1670-1728`, and (c) whether it is simulation-mutated during battle.

### A.1 Complete State struct inventory

The State struct is defined at `src/netplay/game_state.h:711-714`:
```c
typedef struct State {
    GameState gs;
    EffectState es;
} State;
```

`GameState` has 604 individually-named fields (`src/netplay/game_state.h:29-709`). `EffectState` has 7 fields that mirror the effect pool (`src/netplay/game_state.h:19-27`). `sizeof(GameState) == 17580` on 32-bit (pinned at `src/netplay/game_state.c:52`).

**Every** `GS_SAVE(...)` invocation in `src/netplay/game_state.c:78-756` (save path) and the matching `GS_LOAD(...)` at `src/netplay/game_state.c:760-1438` (load path) was extracted:

```sh
grep -oE 'GS_SAVE\([a-zA-Z_][a-zA-Z_0-9]*\)' src/netplay/game_state.c | sed 's/.*(//;s/)//' | sort -u | wc -l
# → 604 distinct names
```

The save and load sets are identical. Two fields are commented out on both sides (`Demo_Timer` at `src/netplay/game_state.c:513,1195` and `Condense_Buff` at `src/netplay/game_state.c:514,1196`). These are saved fields in `GameState` (`src/netplay/game_state.h:466-467`) that are NOT persisted across rollback — they drift silently per peer.

### A.1.1 Focused checksum whitelist (what GekkoNet actually compares)

The cross-peer checksum hashes only these bytes (`src/netplay/game_state.c:1670-1728`):

| Field | File:Line | Notes |
|---|---|---|
| `plw_scratch[0]` | `game_state.c:1673` | `sizeof(PLW)` bytes, after `sanitize_plw_pointers` + `sanitize_work_rendering` |
| `plw_scratch[1]` | `game_state.c:1674` | ditto |
| `Random_ix16`, `Random_ix32` | `1677-1678` | RNG index (battle) |
| `Random_ix16_ex`, `Random_ix32_ex` | `1679-1680` | RNG index (ex) |
| `Random_ix16_com`, `Random_ix32_com` | `1681-1682` | RNG index (COM) |
| `Random_ix16_ex_com`, `Random_ix32_ex_com` | `1683-1684` | RNG index (ex/COM) |
| `Round_num` | `1687` | |
| `Round_Level` | `1688` | |
| `Round_Result` | `1689` | |
| `PL_Wins` | `1690` | `u8[2]` |
| `Conclusion_Type` | `1691` | |
| `win_type` | `1692` | `u8[2][4]` |
| `My_char` | `1695` | Selected character |
| `Super_Arts` | `1696` | Selected super |
| `combo_type` | `1701` | **Fork-exclusive** (research §19 risk 1) |
| `remake_power` | `1702` | **Fork-exclusive** |
| `Attack_Flag` | `1705` | |
| `Counter_Attack` | `1706` | |
| `Guard_Flag` | `1707` | |
| `Flip_Flag` | `1708` | |
| `Lie_Flag` | `1709` | |
| `Attack_Counter` | `1710` | |
| `Bullet_No` | `1711` | |
| `Bullet_Counter` | `1712` | |
| `paring_counter` | `1713` | |
| `Present_Mode` | `1716` | |
| `VS_Stage` | `1717` | |
| `SLOW_timer`, `SLOW_flag`, `EXE_flag` | `1720-1722` | |
| `super_arts` | `1725` | `SA_WORK[2]` |
| `piyori_type` | `1726` | `PiyoriType[2]` |
| `Max_vitality` | `1727` | |

**Total fields hashed: ~30 named fields + 2 × sizeof(PLW).** Every other saved field (several hundred) is restored on rollback but NOT compared across peers. Divergence in an unhashed-but-saved field only produces a desync when it eventually corrupts a hashed field.

Fields of `GameState` that are SAVED but NOT in the hash (partial list of the notable ones, not exhaustive — there are roughly 570 such fields):
- **Entire `task[11]` array including `func_adrs` function pointer** — saved at `game_state.c:529,1211`. Cross-peer pointer values differ under PIE. Not hashed, but restored wholesale on rollback. Safe because the restore is from local buffer.
- **All BG sim state**: `bg_w`, `Screen_Switch`, `Screen_Switch_Buffer`, `rw_num`, `rw_bg_flag[4]`, `tokusyu_stage`, `rw_gbix[13]`, `stage_flash`, `stage_ftimer`, `yang_ix_plus`, `yang_ix`, `yang_timer`, `ending_flag`, `end_prm[8]`, `gouki_end_gbix[16]`, `rw3col_ptr` (pointer), `bg_disp_off`, `bgPalCodeOffset[8]`, `rw_dat[20]` (contains pointer fields `.rwd_ptr`, `.brw_ptr`). Saved at `game_state.c:580-598`. **Not hashed**. See §C for why this matters.
- **BG scroll / family positions**: `bg_pos[8]`, `fm_pos[8]`, `bg_prm[8]` — `game_state.c:739-741`. Not hashed.
- **`system_timer`** (u32 counter incremented at `src/sf33rd/Source/Game/game.c:158`) — saved, not hashed.
- **`Gill_Appear_Flag`** — saved, not hashed.
- **`scr_sc`, `X_Adjust`, `Y_Adjust`, `BgMATRIX[9]`** — saved, not hashed.
- **Effect pool** (`frw`, `head_ix`, `tail_ix`, `exec_tm`, `frwctr`, `frwctr_min`, `frwque`) — saved via `EffectState` in `game_state.c:1494-1500`, **not hashed at all**.
- **`combo_type`, `remake_power`** are ALSO top-level — these ARE hashed (`1701-1702`).

### A.2 Globals NOT covered by State — ranked by simulation activity

Computed by set-difference of the non-const mutable globals in `src/sf33rd/Source/Game/` (928 hits, 853 unique names after dedup) vs. the 604 names in the `GS_SAVE` whitelist. 258 names are uncovered. The list below classifies by simulation relevance.

#### A.2.1 Uncovered globals that ARE written during `step_game` and plausibly affect sim flow

| Name | File:Line | Write site | Read in sim? | Risk |
|---|---|---|---|---|
| **`spmv_ng_save[2]` (u32)** | `sf33rd/Source/Game/effect/effl8.c:13` | `effl8.c:50` (`spmv_ng_save[ewk->master_id] = mwk->spmv_ng_flag`) | `effl8.c:63` (`mwk->spmv_ng_flag = spmv_ng_save[ewk->master_id]`) | **HIGH — writes back into a HASHED field (PLW.spmv_ng_flag).** Triggered by `effect_L8_init` at `engine/plpat17.c:245` gated by `wk->player_number != 16` (only CHAR_CHUNLI=16 passes). Rollback-unsafe: a save happens in routine_no[0]==0, restore in routine_no[0]==1. If rollback crosses between those states, the restore reads a stale spmv_ng_save value, polluting hashed PLW.spmv_ng_flag. See Candidate 0. |
| `omop_spmv_ng_table[2]` (u32) | `sf33rd/Source/Game/engine/plcnt.c:97` | `effect/effe3.c:126` / `effe4.c:88` (training-mode only); `sysdir.c:92-97,115` (init) | `sysdir.c:72` (via `sag_ikinari_max()`, only when `Demo_Flag` set; not active in MODE_NETWORK battle) | LOW — effe3/effe4 guarded by `Is_Training_Mode(Mode_Type)` (see `effect/effe3.c:23`, `effect/effe4.c:23`), which is false in MODE_NETWORK. `sag_ikinari_max` reader gated by `Demo_Flag` which is 0 during match. `plcnt.c:95` comment: `// FIXME: might not be necessary to put in GameState`. |
| `omop_spmv_ng_table2[2]` (u32) | `sf33rd/Source/Game/engine/plcnt.c:98` | same | same | LOW (same gating) |
| `grdb[2][2][2]` (s16) | `sf33rd/Source/Game/engine/hitcheck.c:32` | `hitcheck.c:43-44,48-49` via `make_red_blocking_time` | `hitcheck.c:1150,1206` | MEDIUM — written at stage/cmd init via `cmd_data_set` → `make_red_blocking_time`. Stable mid-battle if cmd init ran identically on both peers. |
| `grdb2[2][2]` (s16) | `sf33rd/Source/Game/engine/hitcheck.c:33` | `hitcheck.c:53-54` | `hitcheck.c:1047` | MEDIUM — same |
| `ca_check_flag` (s8) | `sf33rd/Source/Game/engine/hitcheck.c:38` | `plcnt.c:642` (`pli_1000`), `plcnt.c:1137` (`setup_settle_rno`); `plmain.c:1173,1191`; `plcnt2.c:109` | `hitcheck.c:64` | MEDIUM — toggled each sim tick based on player state. If PLW is synced, this derives deterministically. But rollback did NOT restore it from saved state at the time of this audit (not in `GS_SAVE`; saved since `7e07db3f`), so speculative-sim residue persisted unless re-computed next tick. Affects which hit-check branch runs per-tick. |
| `hs[32]`, `hpq_in`, `mkm_wk[32]`, `q_hit_push[32]` | `hitcheck.c:31,37,36,35` | `clear_hit_queue` at `hitcheck.c:1913-1916` each tick, + fills during `hit_push_request` / `attack_hit_check` | `hitcheck.c:82-227` during the same tick | LOW — cleared at end of each `hit_check_main_process` call (`hitcheck.c:74` → `clear_hit_queue`). Scratch buffer; no cross-tick residue. |
| `cmd_id`, `cmd_tbl_ptr`, `sw_work`, `chk_pl`, `waza_type[2]`, `waza_ptr`, `cmd_pl` | `engine/cmd_data.c:13-19` | `cmd_main.c:26,34,77,101,106` (set at start of each player's `waza_check`/`key_thru`/`cmd_move`) | Read throughout `cmd_main.c:372-962` during the same call chain | LOW — all are scratch pointers/indices set at start of each invocation (`cmd_main.c:24-30,32-37,97-119`). Not persisted between calls in a semantically meaningful way. |
| `bcdext` (s16) | `sf33rd/Source/Game/effect/effa5.c:16` | set to 0 at `effa5.c:71` before `sbcd()` call each tick | read in `sbcd()` at `effa5.c:20,30,32` | LOW — reset before each `sbcd` call; never read when `effect_A5_move` is not active (battle has no A5 effect). |
| `s_bcd_carry` (s16) | `sf33rd/Source/Game/select_timer.c:38` | `select_timer_sbcd()` at `select_timer.c:69,71` | same | NONE in-battle — `select_timer.c` is not invoked by engine today per comment at `select_timer.c:16-17`. Present only for GameState rollback coverage. **REMOVED 2026-08-29 (task #109): `select_timer.c` was deleted outright — it had zero call sites anywhere in `src/` or `tests/`, and upstream `33dfd75b` (#216) had already replaced it with effect A5. `s_bcd_carry`/`select_timer_sbcd()` no longer exist, so this row is a historical record and the `select_timer.c` citations in it are deliberately dangling.** |
| `vib_sel[2]` (s8) | `sf33rd/Source/Game/engine/plcnt.c:102` | `sysdir.c:121-122`; effect code | writers only during init/training | LOW — only written during init or training paths; not active in VS battle after setup. |
| `start_hold_counter[2]` (u16) | `sf33rd/Source/Game/system/pause.c:24` | `pause.c:62-65,68` (in `Pause_Check`) | `pause.c:62,68` | NONE — `Pause_Task` short-circuits on `Mode_Type != MODE_NETWORK` (`pause.c:50`); `TASK_PAUSE` is explicitly `cpExitTask`ed in `setup_vs_mode` (`netplay.c:173`). |
| `training_hitbox_display_enabled` (bool) | `sys_sub.c:42` | `sys_sub.c:247`; only touched in training-menu flow | training only | NONE in VS netplay |
| `training_input_history*` | `ui/sc_sub.c:62-64` | training | training only | NONE in VS netplay |
| `bg_fastpath_*` (file-static u8/f32) | `sf33rd/Source/Game/stage/bg.c:52-55` | `bg.c:606-609,611` (inside `scr_trans`) | `bg.c:1180-1187` (inside `ppgCalScrPosition`) | NONE — both sites are the rendering path (BG polygon emission). Never read by sim. |
| `Extra_Counter[2]`, `OK_Appear79[2]` | `effect/eff79.c:33-34` | `eff79.c:57,77` | `eff79.c:77` | NONE in battle — effect 79 is the SA-select plate appear animation; not active mid-match. |
| `Candidate_Buff[16]` (u8) | `system/sys_sub.c:40` | `sys_sub.c:1699,1747,1763,1776` in `Setup_Candidate_Buff` + `Initialize_EM_Candidate` | `sys_sub.c:1762,1775` via `random_16()` index | NONE in VS — arcade-mode EM (boss candidate) selection only. Gated by mode. |
| `chk77_flag` (s16) | `effect/eff77.c:17` | `eff77.c` internals | same | MEDIUM — eff77 is the Gill seraph preparation (stage 18+, Gill stage BG). Writes `Bg_Off_R` at `eff77.c:71,92`. If this effect activates on one peer but not the other due to an upstream micro-divergence, BG goes off on one peer only. See §C.6. |
| `effa6_pos_*_1p/2p` | `effect/effa6.c:16-20` | eff A6 internals | same | eff A6 is character-select BG motion; inactive mid-battle. |
| `hnc_timer`, `hnc_end_timer`, `hnc_col` | `effect/effa2.c:14-16` | eff A2 | same | eff A2 = "HUNGER CHECK" popup; only in bonus/demo. Not VS battle. |
| `ci_col`, `ci_timer` | `effect/eff56.c:14-15` | `eff56.c`; paired with saved `ci_pointer` | `ci_pointer`, `ci_col`, `ci_timer` all SAVED at `game_state.c:703-705`. Wait — actually `ci_col` and `ci_timer` ARE in save list. **Re-check my diff:** these names DO appear in saved_names.txt — they are IN the save set. The uncovered.txt false-positive here. |
| `roll_rate`, `roll_rate_t`, `roll_rate2`, `roll_rate_t2`, `roll_stop` | `effect/effh6.c:16-17` + others | `effh6.c:33-34`; `screen/staff.c` | same | NONE in battle — effect H6 is credit-roll / staff-roll scroll. Only active in ending screen. |
| `OP_W op_w`, `op_bg_mvxy`, `op_104_sound`, `op_demo_index`, `op_end_flag`, `op_obj_disp`, `op_plmove_timer`, `op_scrn_end`, `op_sound_status`, `op_timer0` | `opening/opening.c` | opening state | same | NONE — opening/attract-mode only. After `setup_vs_mode`, opening is off. |
| `pul[2]`, `ppwork[2]`, `pulpara[]`, `pulreq[]`, `ot_pulpara[]`, `ot_pulreq[]`, `ot_pulreq_xx[]`, `q_ldreq[16]`, `ldreq_break`, `ldreq_result`, `plt_req[2]`, `vib_req[2][2]` | `io/pulpul.c`, `io/gd3rd.c` | pulpul / load-queue state | consumed by HW pipeline (rumble) and texture loader | LOW — output-side queues. Sim writes, hardware consumes. No feedback to sim state. |
| `mts[24]`, `mts_ok[24]`, `mts_ob_curr_stage` | `rendering/aboutspr.c:20-21`, `rendering/texcash.c:84` | render-side sprite dispatch | same | LOW — render-side. `mts_ob_curr_stage` is texture-load stage, set during stage init, not mid-battle. |
| `dmwk_moji`, `dmwk_kage` (WORK) | `rendering/aboutspr.c:22-23` | render-side dummy WORK structs for text/shadow drawing | same | LOW — render-side placeholder WORKs. Not sim state. |
| `fd_dat` (FadeData) | `ui/sc_sub.c:395` | `fade_cont_init` (`sc_sub.c:2309-2314`), `fade_cont_main` | `sc_sub.c:2319-2333` | MEDIUM — fade state driven by `Fade_Flag` (saved), `Fade_Number` (saved). `fd_dat.fade_kind`, `.fade`, `.fade_prio` derived from `fade_data_tbl[Fade_Number]` (const). Not rollback-restored. If fade starts mid-sim, fd_dat is set; if rollback undoes the Fade_Flag toggle, fd_dat retains the stale fade params. Next tick re-computes from Fade_Flag=0, fd_dat stays stale but unused. Not a direct divergence source. |
| `bg_priority[4]` (u8) | `stage/bg.c:28` | `bg.c:355,358,471,474,523` only in `Bg_Texture_Load_*` | rendering | NONE — set only during stage texture load, not per-frame. Stable mid-battle. |
| `bgpoly[4]`, `scrDrawPos[4]`, `scrscrntex[4]` | `scrDrawPos`/`bgpoly` at `stage/bg.c:29-30`; `scrscrntex` at `ui/sc_sub.c:393` | stage/bg rendering | rendering | NONE — render-side only. |
| `col3rd_w`, `palFormConv`, `palFormRam`, `palFormSrc`, `colPalBuffDC[1024]`, `ColorRAM[512][64]`, `Color7`, `curr_bright`, `hc3alpha`, `hi_meta[2][2][64]`, `metamor_original[2][5][64]` | `rendering/color3rd.c:54-60`, `rendering/meta_col.c:10-11`, `rendering/mtrans.c:36` | palette transfer and metamorphose state | consumed by palette upload pipeline | LOW — rendering side. Sim writes palette requests (through `push_color_trans_req`), consumed by GPU upload. No feedback. |
| `njdp2d_w` (NJDP2D_W) | `rendering/dc_ghost.c:41` | `njdp2d_sort` fills; `njdp2d_draw` drains | read by renderer during `njdp2d_draw` | NONE — drained once per sim tick at `src/netplay/netplay.c:587`. No cross-tick residue. |
| `cmtx` (MTX) | `rendering/dc_ghost.c:42` | `njUnitMatrix`, `njTranslate`, `njScale` | render pipeline | NONE — render-side active matrix; not sim state. |
| `scr_bcm[4]` (const u16*) | `stage/bg_data.c:30` | Set by `Bg_Texture_Load_*`, `Ed_Kakikae_Set` at stage load | BG rendering | NONE — init-time only. |
| `TopHUDFacePriority`, `TopHUDPriority`, `TopHUDShadowPriority`, `TopHUDVitalPriority` | HUD priority levels | set during init | read during render | NONE — config, not sim. |
| `PrioBase[]`, `PrioBaseOriginal[]` | `rendering/mtrans.c:43-44` | stage/mtrans init | rendering | NONE — init only. |
| `texgrplds[100]`, `gSeqStatus[?]` | texgroup state | `rendering/texgroup.c:33` | texture loader | LOW — texture load state. |
| `io_w` (IO) | `io/ioconv.c:15` | `keyConvert` (per real frame) | only read by `keyConvert` itself | NONE — per-peer local input scratch. Netplay overwrites p*sw_* from Gekko's event inputs before `step_game` (`netplay.c:595-598`). |
| `p1sw_buff`, `p2sw_buff`, `p3sw_buff`, `p4sw_buff` | input buffers | `ioconv.c:123-124`, `keyConvert` | `get_inputs` at `netplay.c:551` | NONE — local input buffer. Each peer sends own to Gekko; Gekko returns both peers' inputs to both peers (identical). |
| `p1sw_0/_1`, `p2sw_0/_1`, `p3sw_0/_1`, `p4sw_0/_1` | input state | `advance_game` at `netplay.c:595-598` rewrites from Gekko event | sim reads | NONE — overwritten by Gekko-sourced inputs before every sim tick. |
| `Interrupt_Timer` (u32) | `system/work_sys.c:34` | `main.c:608` once per REAL frame | `game.c:352-353` only `if (Mode_Type != MODE_NETWORK)`; `aboutspr.c:541-649` (render) | LOW — render-side in netplay mode. Read-as-RNG-seed only when NOT MODE_NETWORK. |
| `No_Trans` (u8) | `system/work_sys.c:29` | `netplay.c:583` (set each `step_game`); `game.c:138-149` | sim flow control | LOW — set each sim tick, drives "no transient/rendering" flag. Deterministic based on render boolean. |
| `sysFF`, `sysSLOW`, `Slow_Timer` | `debug/Debug.c:28-30` | `main.c:500-515` (DEBUG path); `Debug.c:77` | `game.c:142-144` | LOW — Release builds: `sysFF = 1` always, not modified. |
| `Debug_w[72]`, `Debug_ID`, `Debug_Index`, `Debug_Pause`, `Deley_Debug_*`, `check_screen_*`, `check_time_*`, `Rec_Time[2]`, `Record_Timer`, `time_check[4]`, `time_check_ix`, `debug_menu_active`, `Test_Cursor` | debug globals | only mutated in debug menu | same | NONE — Release binary has `Debug_w[72] = {0}` at `debug_config.c:126`; all debug writes are DEBUG-only. |
| `staff_r_no`, `staffroll_end`, `name_ptr` | `screen/staff.c`, `screen/n_input.c` | ending screens | same | NONE in battle. |
| `Message_Data[4]`, `mmes_already`, `keep_mes_no`, `old_mes_no_pl`, `Training_Auto_Start`, `Training_Menu_From_Pause`, `Training[3]`, `tr_data` | message/training state | message code and training menu | same | NONE — training globals inactive in VS, message globals are for demo scenes. |

#### A.2.2 Uncovered globals that are writes-only from sim (output fan-out; not fed back)

- `vib_req[2][2]` (s16) — `io/pulpul.c:190,251,325`. Rumble request queue; read only by rumble HW pipeline.
- `pul[2]`, `ppwork[2]`, `pulpara[]`, `pulreq[]`, `ot_pulpara[]`, `ot_pulreq[]`, `ot_pulreq_xx[]`, `q_ldreq[16]` — rumble/queue state in `pulpul.c` and `gd3rd.c`. Sim-set, hardware-consumed.
- `col3rd_w`, `palFormRam`, `palFormSrc`, `palFormConv`, `colPalBuffDC`, `ColorRAM`, `col3rd_w.upBits`, `col3rd_w.req[]` — palette transfer state in `rendering/color3rd.c:54-60`. Written by sim (`push_color_trans_req`), consumed by renderer.
- `njdp2d_w` (2D polygon sort buffer) — `rendering/dc_ghost.c:41`. Written by sim `njdp2d_sort`, drained once per sim tick by `njdp2d_draw` at `src/netplay/netplay.c:587`. No cross-tick residue.
- `seqs_w`, `mts[]`, `mts_ok[]` — sprite dispatch buffers in `rendering/mtrans.c`, `rendering/aboutspr.c`. Drained each frame via `seqsBeforeProcess`/`seqsAfterProcess`.
- `charsel_active_effect_count` — perf telemetry in `effect/effect.c:21` (only when `ENABLE_PERF_TELEMETRY`). Assigned outright as `EFFECT_MAX - frwctr` each rendered frame; its consumer is the DEBUG FPS overlay's sticky peak (`fps_overlay_peak_active_effects`, rendered as the trailing `e<peak>` field of the overlay label). No feedback into sim.
- `charsel_frame_portrait_tiles` / `charsel_frame_plate_tiles` (formerly `rendering/mtrans.c`) and `perf_super_art_command_telemetry[2]` (formerly `engine/pls03.c`) — **deleted, task #68.** All were write-only with zero readers. The tile counters additionally keyed on `mt->id == 13 / == 14`, but `texcash.c:660` assigns `mts[ix].id = ix`, so `id` is a cache-slot index and the branch never identified a portrait or a plate. The super-art counters' designed sink was the `test_state` object of the perf-capture JSON, which no longer exists (see `perf_capture_write_summary` in `port/sdl/sdl_app.c`).

#### A.2.3 Uncovered globals — sound/BGM state

Sound output state, not read back into sim logic. Confirmed by inspection of all usages in `sound/sound3rd.c:39-51` and cross-refs: `bgm_exe`, `bgm_req`, `bgm_fade`, `bgm_fade_ix`, `bgm_half_down`, `bgm_level`, `bgm_seamless_always`, `bgm_selectorAC/DC`, `bgm_tableAC/DC`, `bgm_exdataAC/DC`, `bgm_vol_mix`, `bgm_vol_now`, `current_bgm`, `music_scene`, `music_time`, `se_level`, `adx_EmSel`, `adx_VS`, `adx_stm_work`.

All write-only from sim; consumed by SsBgm* / ADX hardware pipeline.

#### A.2.4 Uncovered globals — render-side matrices / priority

- `PrioBase[PRIO_BASE_SIZE]`, `PrioBaseOriginal[PRIO_BASE_SIZE]` (f32) — `rendering/mtrans.c:43-44`. Initialized at startup; read by renderer.
- `cmtx` (MTX) — `rendering/dc_ghost.c:42`. Current rendering matrix. Written by `njUnitMatrix`/`njTranslate`/`njScale`, read by drawing.
- `bg_priority[4]`, `bgpoly[4]`, `scrDrawPos[4]`, `scrscrntex[4]` — BG rendering primitives in `stage/bg.c:26-28` and elsewhere.
- `TopHUDFacePriority`, `TopHUDPriority`, `TopHUDShadowPriority`, `TopHUDVitalPriority` (int) — HUD Z priorities, static config.

#### A.2.5 Uncovered globals — debug / pause / training / ending

Inactive in MODE_NETWORK battle:
- `Debug_w[72]` — `src/sf33rd/Source/Game/debug/debug_config.c:126`. Release builds: all-zero static array (see `debug_config.c:122-128`). Deterministic.
- `Debug_ID`, `Debug_Index`, `Debug_Pause`, `sysFF`, `sysSLOW`, `Slow_Timer`, `check_screen_*`, `check_time_*`, `Rec_Time[2]`, `Record_Timer`, `time_check[4]`, `time_check_ix` — `debug/Debug.c:21-38`.
- `debug_menu_active`, `debug_config` — debug-only.
- Ending-path globals: `end_0_1_time`, `end_5_flag`, `end_etc_flag`, `end_fade_flag`, `end_fade_timer`, `end_name_cut`, `end_no_cut`, `end_staff_flag`, `END_OF_95`, `ending_all_end`, `fade_prio`, `staffroll_end`, `name_ptr`. Not active mid-battle.
- Opening-path globals: `op_w`, `op_bg_mvxy[3]`, `op_104_sound[7]`, `op_demo_index`, `op_end_flag`, `op_obj_disp`, `op_plmove_timer`, `op_scrn_end`, `op_sound_status`, `op_timer0`, `opening.c` internals.
- Name-entry globals: `Name_00[2]`, `Name_Input_f`, `name_limit_timer[2]`, `name_timer`, `naming_cnt[2]`, `name_wk[2]`, `rank_name_w[2]`, `msgNum`, `sc_name_wk`.
- Ranking: `Present_Data[2]`, `Ranking_Data[20]`.

#### A.2.6 Uncovered globals — HUD/UI overlays

- `omop_*` array set: `omop_b_block_ix[2]`, `omop_cockpit`, `omop_dokidoki`, `omop_guard_distance_ix[2]`, `omop_otedama_ix[2]`, `omop_r_block_ix[2]`, `omop_round_timer`, `omop_sa_bar_disp[2]`, `omop_sa_gauge_ix[2]`, `omop_sag_len_ix[2]`, `omop_sag_max_ix[2]`, `omop_st_bar_disp[2]`, `omop_stun_gauge_add[2]`, `omop_stun_gauge_len[2]`, `omop_stun_gauge_rcv[2]`, `omop_use_ex_gauge_ix[2]`, `omop_vital_init[2]`, `omop_vital_ix[2]`, `omop_vt_bar_disp[2]` — all set by `init_omop` / `get_system_direction_parameter` / `get_extra_option_parameter` in `system/sysdir.c:125-250`. Driven by per-peer `save_w[MODE_NETWORK].extra_option` and `system_dir[2]`, neither of which is forced to a canonical value in `setup_vs_mode`.
- `Interface_Type[2]` (u8) — `io/ioconv.c:86-89`. Rendering-side input type indicator.
- `io_w` (IO) — `io/ioconv.c:15`. Input conversion intermediate. In netplay, advance_game overwrites `p1sw_0`, `p1sw_1`, `p2sw_0`, `p2sw_1` from GekkoNet inputs at `src/netplay/netplay.c:595-598` **before** `step_game` runs. io_w contents do not affect the sim in netplay mode.
- `Rnd` — unused in active codepaths (checked by `grep -n`).
- `letter_counter`, `letter_stack` — text rendering intermediate.
- `picon_no`, `picon_level` — playtime indicator.
- `ne_col`, `ne_flash_flag`, `ne_timer` — UI/nav anims.
- `hi_meta`, `metamor_original` — color-morph render tables (rendering only).
- `Com_Vital_Unit_Data` — const lookup.

#### A.2.7 Uncovered globals — miscellaneous

- `Interrupt_Timer` (u32) — `system/work_sys.c:34`. Incremented each REAL frame at `main.c:608`. Read by renderer at `rendering/aboutspr.c:541-649` (palette flash alpha). Read ALSO by sim at `game.c:352-353` as RNG seed, BUT only in the `Mode_Type != MODE_NETWORK` branch — netplay calls `Setup_Net_Random_ix()` at `game.c:355` instead, which zeros all RNG indices (`system/sys_sub.c:1429-1436`). Not a sim-relevance risk.
- `cmtx`, `BgMATRIX[9]` — `BgMATRIX` IS saved and loaded (`game_state.c:751,1433`), but never read by sim; render-only. `cmtx` is render-only, NOT in State.
- `curr_bright` (s32) — `rendering/mtrans.c:36`. Global brightness for render.
- `title_tex_flag` — boot-only (`game.c:1660`).
- `c_kakikae`, `c_number`, `g_kakikae[2]`, `g_number[2]`, `nosekae` — bg_data.c locals, ARE in the saved set actually (grep confirms `GS_SAVE(c_kakikae)` at `game_state.c:690`, etc.) → false alarm in the diff.

### A.3 Per-subsystem breakdown

#### A.3.1 Background / stage

- **Saved but NOT hashed**: `bg_w` (`game_state.c:580`), `Screen_Switch` / `Screen_Switch_Buffer` (`game_state.c:581-582`), `rw_num` / `rw_bg_flag` (`game_state.c:583-584`), `tokusyu_stage` (`game_state.c:585`), `rw_gbix[13]` (`game_state.c:738`), `stage_flash` / `stage_ftimer` / `yang_ix_plus` / `yang_ix` / `yang_timer` / `ending_flag` / `end_prm[8]` / `gouki_end_gbix[16]` / `rw3col_ptr` / `bg_disp_off` / `bgPalCodeOffset[8]` / `rw_dat[20]` (`game_state.c:587-598`), `Gill_Appear_Flag` (`game_state.c:743`).
- **Pointers in saved BG state**: `rw3col_ptr` (const u32*), `rw_dat[].rwd_ptr` and `.brw_ptr` (const s16*). Advance per-frame via `rwd_ptr++` at `stage/bg.c:106-107,728-732,747,772-773` etc. **Under PIE each peer has DIFFERENT pointer VALUES for these**, but the advance count is deterministic. Checksum doesn't compare them so this is not a divergence source on its own — but `memcpy` of `rw_dat` bytes from save buffer to live globals on rollback copies local pointer bytes, which is fine because the rollback is from LOCAL saved state.
- **NOT saved, render-only**: `bg_priority[4]` (`stage/bg.c:28`; only written in `Bg_Texture_Load_*` during stage setup), `bgpoly[4]`, `scrDrawPos[4]`, `bg_fastpath_*` (`stage/bg.c:55-58`).
- **Writer paths per-frame during sim**: `Bg_On_R/W` / `Bg_Off_R/W` at `stage/bg.c:1423-1443`. Each updates `Screen_Switch` AND `Screen_Switch_Buffer` in one shot. `Scrn_Renew()` at `stage/bg.c:1445-1447` overwrites `Screen_Switch_Buffer = Screen_Switch` once per REAL frame in `game_step_1` at `main.c:611`. This is NOT called from `step_game` — see §D.
- **Black-background mechanism**: `BG_Draw_System()` at `sf33rd/Source/Game/system/sys_sub.c:899-930`: if `bg_disp_off == 0` and `Screen_Switch_Buffer & mask` is false for all 4 layers, no `scr_trans()` calls fire → BG renders as backcolor-clear. See §C.
- **Init-only globals that are const-lookup-driven**: `etcBgPalCnvTable[7]` / `etcBgGixCnvTable[7][16]` saved and hashed indirectly (whole `bg_w` memcpy).

#### A.3.2 Effects

- `frw[EFFECT_MAX][448]` (effect pool bytes), `head_ix[8]`, `tail_ix[8]`, `exec_tm[8]`, `frwctr`, `frwctr_min`, `frwque[EFFECT_MAX]` — all in `EffectState` (`game_state.h:19-27`), all saved/loaded via `gather_state` / `load_state` at `game_state.c:1487-1501,1759-1771`.
- **Effects are NOT in the focused checksum whitelist.** The `effects` sectioned-checksum at `game_state.c:1741` is literally set to `0` (see comment structure at `game_state.c:1731-1744`). On live-desync detection, the effect pool could be arbitrarily different between peers and no alarm fires — as long as the PLW + game-globals in the whitelist match.
- Effects DO mutate master PLW fields. Examples:
  - `effect/effk5.c:124-125` writes pointer fields `mwk->h_bod` and `mwk->h_han` on master PLW — sanitized by `sanitize_work_pointers` so harmless to the checksum.
  - `effect/effl0.c:40-58` writes `mwk->wu.disp_flag`, `mwk->wu.my_bright_type`, `my_bright_level`, `my_col_mode`, `my_clear_level` — render-side PLW fields; `my_col_code`, `current_colcd`, `colcd`, `extra_col`, `extra_col_2` sanitized in `sanitize_work_rendering` at `game_state.c:1545-1551`.
  - `effect/effe3.c:70-127`, `effect/effe4.c:43-89` write `mwk->spmv_ng_flag`, `spmv_ng_flag2` — gated by `Is_Training_Mode(Mode_Type)` at `effe3.c:23`, `effe4.c:23`, so NOT active in MODE_NETWORK.
  - `effect/effd9.c:109,125` writes `mwk->wu.extra_col` — sanitized.

#### A.3.3 Sound / SE

Saved: `Music_Fade` / `BGM_Vol` / `BGM_Timer[2]` / `BGM_No[2]` / `PB_Music_Off` / `Shin_Gouki_BGM` / `Last_Called_SE` — all in `game_state.c` save list. These are gameplay-controlled sound REQUESTS.

Not saved: the sound mixer state (`bgm_exe`, `bgm_req`, `current_bgm`, `bgm_fade_ix`, etc.). Those consume the saved requests but never feed back into gameplay state. Not a desync source.

#### A.3.4 RNG

Eight RNG indices in state and in hash: `Random_ix16`, `Random_ix32`, `_ex`, `_com`, `_ex_com` variants (`game_state.c:442-443,481-482,523-528` save; `1677-1684` hash). Plus `Random_ix16_bg` (saved in `GameState_Save`/`GameState_Load` just after `Random_ix32_ex_com`, NOT hashed). Its save/load pair was briefly removed by the upstream #298 port (`e57b16bd`, 2026-08-23) on the rationale that "the state deciding when to draw from it isn't saved"; that is false on this fork — `random_16_bg()` is called only from `scr_trans()` (`stage/bg.c:811,948,949`), whose four write targets (`stage_flash`, `stage_ftimer`, `rw_dat[0]`, `rw3col_ptr`) are all GS_SAVEd — so the rollback-determinism harness caught the index and all three saved consumers going DIVERGENT(+FEEDBACK) from frame 347 and the pair was restored.

RNG impl at `sf33rd/Source/Game/engine/pls02.c:617-699`. `random_16` / `random_32` / `random_16_ex` / `random_32_ex` / `random_16_com` / `random_32_com` / `random_16_ex_com` / `random_32_ex_com`. Each increments its index, ANDs with a mask, returns table lookup. **All tables are const** (verified by: `grep -n 'random_tbl' src/sf33rd/Source/Game/engine/pls02.c` → file-local `static const` arrays).

Seed reset: `Setup_Net_Random_ix()` at `sys_sub.c:1429-1436` zeros ix16/ix32/ix16_ex/ix32_ex. Called from `Game01_Sub` at `game.c:355` when `Mode_Type == MODE_NETWORK`. _com variants are NOT explicitly reset by `Setup_Net_Random_ix` — but `setup_vs_mode` zeros them at `netplay.c:330-335`.

Deterministic assuming:
1. Both peers reach `Setup_Net_Random_ix` at the same conceptual frame.
2. Both peers call `random_16()` etc. the same number of times in the same order.

One subtlety: `random_16_com` / `random_32_com` / `random_*_ex_com` check `if (Play_Mode == 0)` at `pls02.c:662,678,692,707`. If `Play_Mode` differs (Play_Mode IS hashed? — yes, implicitly via `Present_Mode` which is hashed; `Play_Mode` itself is saved at `game_state.c:389` but NOT in the hash list at `game_state.c:1716`). If `Play_Mode` differs between peers, the _com variants would route through different RNG streams silently until one of them corrupts PLW.

#### A.3.5 Super art / ghost / training

- Super: `super_arts[2]` IS hashed. `piyori_type[2]` hashed. `SA_shadow_on` saved, not hashed.
- Ghost / replay: `Replay_Status[2]`, `Replay_w` (not in State — NOT hashed, NOT saved). `Replay_w` is set from menu.c save/load only; in netplay, `Replay_Status[0..1] = 0` in setup_vs_mode (`netplay.c:208-209`) so replay path is inert.
- Training: all training globals are gated by `Is_Training_Mode(Mode_Type)` which is false for MODE_NETWORK.

#### A.3.6 Input beyond PLW

The simulation reads `p1sw_0`, `p1sw_1`, `p2sw_0`, `p2sw_1`, `PLsw[0-1][0-1]`, `plsw_00[2]`, `plsw_01[2]`, `Lever_Buff[2]`, `Lever_Pool[2]`, `Lever_Store[2][3]`, `Resume_Lever[2][20]`, `Free_Lever[2]`, `Lever_LR[2]`, `Lever_Squat[2]`, `Guard_Type[2]`, `PLsw`, etc. ALL of these are in the save list and most are in PLW (hashed). `p1sw_0` through `p2sw_1` are top-level globals that are overwritten at the start of `advance_game` at `src/netplay/netplay.c:595-598` before any sim runs.

`p1sw_buff` / `p2sw_buff` / `p3sw_buff` / `p4sw_buff` are the local-pad-to-gekko input buffer. Not in State, but overwritten by `keyConvert()` each REAL frame. In netplay, only `get_inputs()` at `netplay.c:546-553` reads them (OR'd for local_inputs), then Gekko's advance-event stuffs peer inputs into `p1sw_0` / `p2sw_0`. No cross-peer divergence because peer inputs come from Gekko.

#### A.3.7 Config-driven gameplay flags

`setup_vs_mode` at `src/netplay/netplay.c:147-378` forces these before session start:
- `save_w[MODE_NETWORK].Time_Limit = 99` (line 216)
- `save_w[MODE_NETWORK].Battle_Number[0..1] = 2 / 1` (lines 217-218, 286-287)
- `save_w[MODE_NETWORK].Damage_Level = 1` (arcade-default service setting)
- `save_w[MODE_NETWORK].Handicap = 0` (line 220)
- `save_w[MODE_NETWORK].GuardCheck = 0` (line 221)
- `save_w[MODE_NETWORK].Pad_Infor[p].Shot[s] = identity[s]` (lines 267-272)
- `save_w[MODE_NETWORK].Pad_Infor[p].Vibration = 0` (line 271)
- various zeros for Check_Buff, Convert_Buff, timers, etc.

**NOT forced by `setup_vs_mode`**:
- `save_w[MODE_NETWORK].extra_option` — read at `init_omop` (`sys/sysdir.c:101`) to set `omop_*` globals. Each peer brings their own.
- `save_w[MODE_NETWORK].AnalogStick` — read at `sys/sys_sub.c:598,636` to drive `mpp_w.useAnalogStickData`. Local to each peer's input processing.
- `system_dir[2]` contents — `init_omop` reads `system_dir[2]` via `get_system_direction_parameter` at `sys/sysdir.c:100`. `system_dir` IS in State (saved at `game_state.c:753` as part of `ck_ex_option`? — checking...). Actually: `system_dir[6]` is defined in `system/work_sys.c:46` and is NOT in `GS_SAVE`. **system_dir is NOT in State.**
- `Direction_Working[6]` saved at `game_state.c:391`. In-state.

**Implication**: if the two peers have different `save_w[MODE_NETWORK].extra_option` contents or different `system_dir[2]` contents (e.g., one peer had "chip damage" enabled via debug menu, the other not), `init_omop` would seed different `omop_spmv_ng_table`/`omop_spmv_ng_table2`/`omop_b_block_ix`/`omop_r_block_ix`/`grdb`/`grdb2` values. Of those:
- `omop_b_block_ix`, `omop_r_block_ix`, `grdb`, `grdb2` are NOT in State and NOT hashed — but their effects feed into `wcp[].reset[i]` which IS in PLW and IS hashed.
- `spmv_ng_flag` / `spmv_ng_flag2` LIVES ON PLW (`structs.h:518-519`) and IS hashed. So if the two peers computed different values at init, **the hash would catch the desync on frame 1, not frame 1274**.

This rules out per-peer `extra_option`/`system_dir` as a 1274-frame bug — it would fire immediately.

## Section B — Source-of-divergence audit

Exhaustive scan of the simulation path for sources of non-determinism.

### B.1 Wall-clock / monotonic time reads

```sh
grep -rn 'time(\|clock_gettime\|gettimeofday\|SDL_GetTicks\|SDL_GetPerformanceCounter\|SDL_GetTicksNS' src/sf33rd/ --include='*.c' --include='*.h'
```

Results:

| File:Line | Call | Sim/render? | Feeds State? | Risk |
|---|---|---|---|---|
| `sf33rd/Source/Game/game.c:174,176,580,582,595,597` | `SDL_GetTicksNS` | Measurement around sim+render; deltas accumulate in perf counters only (`perf_dispatch_ns`, `perf_game_logic_ns`, `perf_sprite_submit_ns`). | No. `perf_*_ns` never writes State. | NONE |
| `sf33rd/Source/Game/rendering/mtrans.c:1649,1661,1669,1674` (all inside `seqsAfterProcess`) | `SDL_GetTicksNS` | Render. | No. | NONE |
| `src/port/sdl/sdl_game_renderer.c` | multiple | Render. | No. | NONE |
| `src/netplay/netplay.c:422-428,435-438` | `SDL_GetTicks`/`SDL_Delay` | MIST handshake only (pre-session). | No. | NONE — runs before `GekkoSessionStarted`. |

No wall-clock reads in the sim path. **Safe.**

### B.2 Floating-point math

```sh
grep -rn '\b(sin|cos|tan|sqrt|atan|atan2|log|exp|pow)\b' src/sf33rd/Source/Game/engine/ src/sf33rd/Source/Game/effect/ --include='*.c'
```
Returns only method-name false positives (`renbanshot_conpaneshot`, `att.pow`, etc.). **No transcendental math in the sim.**

```sh
grep -rn '\bfloat\b\|\bdouble\b\|\bf32\b' src/sf33rd/Source/Game/engine/ --include='*.c'
```
Returns nothing. Engine is integer-only.

Sim uses `f32` only via `scr_sc` at `system/work_sys.c:40` (BG zoom scale) and `PrioBase`/`PrioBaseOriginal` (priority LUT in `rendering/mtrans.c:43-44`). `scr_sc` IS saved (`game_state.c:745`). `PrioBase` is init-only.

`scr_sc` is written by `Zoom_Value_Set` at `stage/bg.c:1291-1310` which performs `1.0f / (add + work)` with integer-derived inputs. ARMv7 with `-mfpu=neon-vfpv3 -mfloat-abi=hard` (confirmed at `CMakeLists.txt:451-455`) produces IEEE-754 semantics. Two identical binaries producing identical floats given identical inputs — deterministic.

### B.3 Uninitialized reads / BSS leakage

```sh
grep -rEn '^static\s+(s8|u8|s16|u16|s32|u32|int|char|bool|s64|u64|f32|float|double)\s+[a-zA-Z_][a-zA-Z_0-9]*\s*[=\[;]' src/sf33rd/Source/Game/ --include='*.c' | grep -v 'static const' | head
```

All non-const file-static mutable variables have default-BSS zero initialization on startup or explicit initializers. The binary is the same on both peers; the BSS is identical.

### B.4 `rand()`, `srand`, `rng_*`, `random_*` outside the engine's own `random_*` family

```sh
grep -rEn '\b(rand\(|srand\()\b' src/sf33rd/Source/Game/ --include='*.c'
```
Only hits in `sf33rd/Source/PS2/mc/savesub.c:*` (memory card save stub). Not in the sim path.

```sh
grep -rn 'random_16\|random_32' src/sf33rd/Source/Game/ --include='*.c' | wc -l
```
Many — all route through `pls02.c:617-699` deterministic LUTs. §A.3.4 analysis confirms.

### B.5 Compile-time-dependent macros (`__TIME__`, `__DATE__`, `__FILE__`)

```sh
grep -rn '__TIME__\|__DATE__\|__FILE__' src/sf33rd/Source/Game/ --include='*.c' --include='*.h'
```
Hits in `sf33rd/AcrSDK/common/flps2debug.h` and a couple of log macros — but all are on paths gated `#if DEBUG` or ENABLE_PERF_TELEMETRY log strings. Not in sim paths.

### B.6 `#ifdef` that could produce different code in "identical builds"

- `PORT_MISTER` / `PORT_SDL` / `ENABLE_PERF_TELEMETRY` / `DEBUG` — all set at build time; same on both peers since they run the same binary.
- `LOSSY_ADAPTER` in `src/netplay/netplay.c:45` — default off, so no divergence.

### B.7 Summary table

| Candidate | Where | Sim? | Risk |
|---|---|---|---|
| `SDL_GetTicksNS` | game.c perf counters | No | NONE |
| `SDL_GetTicks`/`SDL_Delay` | MIST handshake | No | NONE (pre-session) |
| Float trig | Nowhere in sim | — | NONE |
| `rand`/`srand` | savesub only | No | NONE |
| random_16/32 | pls02.c, deterministic LUT | Yes | NONE (covered by hash) |
| Wall-clock macros | debug/perf | No | NONE |
| `#ifdef` deltas | Build-time | No | NONE (same binary) |

**No non-deterministic source found in the sim path.** The divergence must come from:
1. A global that's written by sim but NOT saved — uncovered in A.2 — AND that somehow flows INTO a hashed field eventually.
2. Or struct-padding/alignment that gcc happens to clobber on one peer and not the other (non-compiler-deterministic — but both peers run the same binary, so the same codegen).
3. Or a pointer that slips through `sanitize_*_pointers` into the hash.
4. Or CPU-pressure-induced behavior change (see §D).

### B.8 (b) 32-bit arithmetic audit [ADDED 2026-04-24]

Motivated by the corrected evidence: the black-BG symptom correlates with the presence of at least one 32-bit peer. This subsection enumerates every 32-bit-vs-64-bit code quirk found in the sim path that could (a) affect same-arch 32-bit determinism, (b) corrupt hashed state asymmetrically, or (c) produce a shared-but-wrong code path only on 32-bit.

#### B.8.1 Arch-gated preprocessor

- `grep -rn "PORT_MISTER" src/sf33rd/Source/Game/ --include='*.c' --include='*.h'` returns **zero** hits. The MiSTer-vs-SDL split is entirely inside `src/port/` and `src/main.c`; no sim-path code is PORT_MISTER-gated. Rules out pathway (a) "PORT_MISTER-gated sim code executing different logic on MiSTer."
- `grep -rn "__arm__\|__aarch64__\|__LP64__\|_LP64\|__ILP32__\|__SIZEOF_POINTER__"` in `src/sf33rd/Source/Game/` and `src/netplay/` returns **zero** hits. No arch-specific preprocessor branches inside the sim.
- `#if UINTPTR_MAX == 0xffffffff` at `src/netplay/game_state.c:51` — the **only** arch-gated construct in `src/netplay/`. Gates the `_Static_assert(sizeof(GameState) == 17580)` tripwire (32-bit only). On 64-bit, `sizeof(GameState) == 19128` (measured via standalone compile at `/tmp/size_check3.c`; confirms the 32-bit vs 64-bit size delta). Does not affect sim behavior.

#### B.8.2 Pointer-sized game state that differs between 32-bit and 64-bit

Inventory of struct fields used in the sim that are `void*`, `u8*`, `u16*`, `u32*`, `s16*`, `intptr_t` or `uintptr_t` (i.e. whose SIZE depends on arch):

| Struct | Field | File:Line | In GS_SAVE? | In hash? | Implication |
|---|---|---|---|---|---|
| `WORK` (part of PLW) | `target_adrs`, `hit_adrs`, `dmg_adrs` | `include/structs.h:210-212` | via `plw[2]` save | zeroed by `sanitize_work_pointers` at `game_state.c:1509-1541` | Both archs zero these before hashing. Safe. |
| `WORK` | `suzi_offset`, `char_table[12]`, `se_random_table`, `step_xy_table`, `move_xy_table`, `overlap_char_tbl`, `olc_ix_table`, `rival_catch_tbl`, `curr_rca`, `set_char_ad`, `hit_ix_table`, `body_adrs`, `h_bod`, `hand_adrs`, `h_han`, `dumm_adrs`, `h_dumm`, `catch_adrs`, `h_cat`, `caught_adrs`, `h_cau`, `attack_adrs`, `h_att`, `h_eat`, `hosei_adrs`, `h_hos`, `att_ix_table`, `my_effadrs` | `include/structs.h:250,288-297,356-373,432` | via plw | zeroed | Safe. |
| `PLW` | `cp`, `dm_step_tbl`, `as`, `sa`, `py`, `cb`, `rp` | `include/structs.h:525,565,571-574,593` | saved | zeroed by `sanitize_plw_pointers` at `game_state.c:1557-1568` | Safe. |
| `WORK_Other` | `my_master` | `include/structs.h:651` | via `frw[]` in EffectState (saved wholesale) | **not hashed** (effect pool excluded from whitelist) | Safe for hash. Cross-arch cannot use same bytes (different size), but same-arch MiSTer↔MiSTer is identical. |
| `WORK_Other_CONN` / `WORK_Other_JUDGE` | `my_master` | `include/structs.h:673,685` | saved in frw | not hashed | same |
| `WAZA_WORK` | `s16* w_ptr` | `include/structs.h:2019` | `GS_SAVE(waza_work)` at `game_state.c:555` | **not hashed** (only RNG/PLW/combat flags/round state are hashed) | Saved bytes include a pointer under PIE. The pointer value differs per peer (ASLR/PIE on armv7), but since `waza_work` is not in the focused hash, this does not cross-peer-mismatch. Safe from the checksum's perspective. |
| effect pool `frw[EFFECT_MAX][448]` | — | `src/sf33rd/Source/Game/effect/effect.c:29` | via EffectState; `src/netplay/game_state.h:27` | not hashed | Storage type is `uintptr_t` — 4 bytes × 448 = 1792 bytes per slot on 32-bit; 8 bytes × 448 = 3584 bytes per slot on 64-bit. Effect pool is totally different size between 32-bit and 64-bit builds. Same-arch pair identical; cross-arch pair fundamentally diverges. |
| `cmd_main.c:99,1739,1746,1752,1780,1792,1801,1810,1819,cmd_main.h:48` | `intptr_t* adrs` (reading pointer-sized slots of `pl_cmd`/`pl_CMD`) | `engine/cmd_main.c:99,1739,1746,1752,1780,1792,1801,1810,1819` | pointers are const globals, not saved | not hashed | `pl_cmd[20]`/`pl_CMD[20]` are `void*` arrays (`engine/cmd_data.c:1107,1110`). Stride = `sizeof(void*)` — 4 on 32-bit, 8 on 64-bit. Reading `adrs[j]` via `intptr_t*` matches stride correctly on each arch. Safe. |

#### B.8.3 Sizeof-dependent types that flow into state

- `sizeof(GameState)` — 17580 on 32-bit (pinned at `src/netplay/game_state.c:52`), 19128 on 64-bit (measured). Delta = 1548 bytes = pointer-field expansion + alignment changes.
- `sizeof(PLW)` — 1304 on 64-bit (measured). Specific 32-bit value unknown but necessarily smaller. `sizeof(PLW)` is used as the hash stride at `src/netplay/game_state.c:1673,1674`. Same-arch peers hash the same byte-count and byte-layout. Cross-arch peers hash different byte counts + layouts → catastrophic cross-arch divergence.
- `sizeof(uint64_t)` in the PLW pointer sweep at `src/netplay/game_state.c:1658-1665` — stride is 8 bytes on both archs, but meaning of each 8-byte word differs:
  - On 64-bit: each word IS a single field (pointer or wide integer). The `v > 0x100000000 && (v >> 47) == 0` heuristic zeros 48-bit canonical user-space pointers. Works as intended.
  - On 32-bit: each word fuses TWO adjacent 32-bit fields. The heuristic zeros any pair where the second field is in `[1..0x7FFF]` and the resulting pair exceeds 2³². This **corrupts** legitimate non-pointer game state on 32-bit — but both 32-bit peers do it identically, so same-arch hash still matches. **REVISED** — the prior report at `src/netplay/game_state.c:22` described this as a no-op on 32-bit; see the revised note at the top of this document. It's not a no-op; it's a same-direction rewrite.

#### B.8.4 `long` / manual byte manipulation / bitfields / endian assumptions

- `grep -rn "\blong\b"` in sim source: **zero** `long`-typed variables. The codebase uses `s16`/`s32`/`u8`/`u16`/`u32`/`s64`/`u64`/`f32` exclusively. Removes the ILP32 (4-byte `long`) vs LP64 (8-byte `long`) concern.
- `grep -rn "BIG_ENDIAN\|LITTLE_ENDIAN\|__BYTE_ORDER"` in sim source: zero hits. No manual endian handling. Both target archs are little-endian.
- `grep -rn "bitfield\|: [0-9]\+"` — no C bitfields in gameplay structs (verified by `include/structs.h` scan). Bitfields would be implementation-defined; absence rules them out.
- Manual struct byte-manipulation (`memcpy`, `memcmp`, `offsetof`) in save path: only `SDL_memcpy` of full struct + arrays (`src/netplay/game_state.c:75,1501-1514,1658`). No alignment-sensitive partial copies.
- `1L` / `1UL` integer-width constants: only `0x100000000ULL` at `src/netplay/game_state.c:1662` (explicit 64-bit literal, works on both archs).

#### B.8.5 Conclusion of (b) audit

The sim-path audit finds **no 32-bit-specific integer-overflow, truncation, or alignment divergence that would silently flip game state on 32-bit but not 64-bit** within same-arch peers. The struct-size deltas between 32-bit and 64-bit are large (PLW, GameState, EffectState all differ by hundreds of bytes) but do not break determinism *within* a 32-bit-only peer pair. The PLW pointer-sweep heuristic at `game_state.c:1658-1665` is arch-broken (it mangles non-pointer bytes on 32-bit) but deterministically so — both 32-bit peers mangle identically.

Therefore, the hypothesis "a 32-bit-specific silent integer/alignment bug corrupts hashed state on MiSTer↔MiSTer" has **no supporting evidence** from the code. The 32-bit correlation must be explained by (c) rollback-only mechanics triggered by CPU pressure that only materializes on 32-bit MiSTer hardware.

## Section C — Black-background trace

### C.1 What draws the background?

1. Root entry: `BG_Draw_System()` at `sf33rd/Source/Game/system/sys_sub.c:899-930`. Called from `Game2_4` / `Game10` / `Game12` / `Game_Management` etc. in `sf33rd/Source/Game/game.c:657,742,1476,1468`.
2. For each of 4 BG layers (0-3), if `Screen_Switch_Buffer & (1 << i)` is set, call `scr_trans(i)` at `stage/bg.c:553-657`. Otherwise call `scr_calc(i)` at `stage/bg.c:1259-1266` (which only updates `BgMATRIX` without emitting polygons).
3. `scr_trans` pushes BG tile polygons via `bgDrawOneScreen` / `bgDrawOneChip` / `ppgCalScrPosition` (`stage/bg.c:553-757`). These eventually feed `SDLGameRenderer_Submit*` in `port/sdl/sdl_game_renderer.c`.
4. If `bg_disp_off != 0`, the loop at `sys_sub.c:912-916` skips `scr_trans` and only calls `scr_calc` — no BG polygons emitted.

**Short version**: BG polygons fire only when `bg_disp_off == 0` AND `Screen_Switch_Buffer & mask != 0` for that layer.

### C.2 What state controls BG visibility?

1. `Screen_Switch_Buffer` (u16 bitmask, 1 bit per layer). Saved at `game_state.c:582`.
2. `bg_disp_off` (u8 bool). Saved at `game_state.c:596`.
3. `Unsubstantial_BG[4]` (u8 per layer) — used in the `Play_Game == 0` branch of `BG_Draw_System` at `sys_sub.c:919-924`. In battle `Play_Game == 1`, so this branch is inactive (`sys_sub.c:925`). Saved at `game_state.c:373`.
4. `bg_prm[i].bg_h_shift`, `bg_prm[i].bg_v_shift` — BG parameters set by `Irl_Scrn` at `stage/bg.c:1460-1469` (scroll). Saved at `game_state.c:883` (`GS_SAVE(bg_prm)`).
5. `bg_w.stage` — which stage (1-21) is loaded. Writes texture via `Bg_Texture_Load_EX`. Saved via `bg_w` at `game_state.c:737` (`GS_SAVE(bg_w)`).

### C.3 How does BG state evolve per frame during a match?

- `Family_Move()` at `stage/bg.c:1471-1489` is called from `BG_Draw_System` (`Play_Game == 1` branch at `sys_sub.c:925-926`). It calls `scr_calc(i)` for each active family layer, updating `BgMATRIX[i+1]`.
- BG scroll: during sim, effects and bg_sub.c code update `bg_pos[i].scr_x.long_pos` / `scr_y.long_pos` (game_state-saved fields). Once per REAL frame, `Irl_Family()` at `stage/bg.c:1449-1458` publishes `bg_pos[].scr_x.long_pos → bg_pos[].scr_x_buff.long_pos`. Then `Irl_Scrn()` computes `bg_prm[i].bg_h_shift = scrn_adgjust_x + bg_pos[i].scr_x_buff.word_pos.h`. **Both `Irl_Family` and `Irl_Scrn` run in `game_step_1` at `main.c:611-613`, NOT in `step_game`.**
- Screen_Switch writers:
  - `Bg_On_R/W` at `stage/bg.c:1423-1430` (set bit + mirror to Buffer)
  - `Bg_Off_R/W` at `stage/bg.c:1433-1442` (clear bit + mirror)
  - `Scrn_Renew` at `stage/bg.c:1445-1447` (mirror Switch → Buffer; runs in game_step_1)
- Examples of mid-battle `Bg_Off_R` callers (these turn off BG layers during sim):
  - `effect/effd3.c:56,97,156` — Yang-related effects (stage 10)
  - `effect/eff77.c:71,92` — Gill seraph sequence
  - `effect/effk8.c:38,52` — Seraph form (`Bg_Disp_Switch(0)` at `effk8.c:38,52`; also sets `bg_disp_off = 1` at `effk8.c:27`)

### C.4 What makes BG go all-black?

Direct mechanisms, from most likely to least likely:

1. **`bg_disp_off` set to 1** (`Bg_Disp_Switch(1)` called by `effect_K8_move` at `sf33rd/Source/Game/effect/effk8.c:27`). If set on one peer but not the other, the toggle bifurcates. `bg_disp_off` is saved (`game_state.c:596`) but NOT hashed. Would produce black BG on one peer silently.
2. **`Screen_Switch_Buffer == 0`** (all layers off). Combination of `Bg_Off_R/W` calls. Same bits shared with `Screen_Switch`.
3. **`Scrn_Renew` sees a `Screen_Switch == 0`** at the real-frame boundary. Each `Bg_Off_*` writes BOTH `Screen_Switch` and `Screen_Switch_Buffer`; `Scrn_Renew` just re-mirrors. So this collapses to case 2.
4. **BG stage texture not loaded / palette all zero** — would require stage reload during battle, does not happen mid-round.
5. **BG task exited** — TASK_GAME is the only task dispatching BG_Draw_System via Game_Management. TASK_GAME is not exited mid-battle.
6. **Z-sort bug / render failure** — would affect a single peer's render path only, not checksum. Also would not explain the foreground fighters still rendering correctly.
7. **Dirty-rect tracking bug** — per `MEMORY.md` reference `project-ppg-dirty-rect-corruption.md`, PPG dirty-rect tracking can corrupt options menu text. It's plausible this extends to BG tiles. But the reported symptom is "black background during play", not corruption — so likely not this.
8. **sa_bg_cache pointer stale** — the cache surface at `port/sdl/sdl_game_renderer.c:151-167` could restore a bad cached background. BUT this is render-side only: the cache restore at `port/sdl/sdl_game_renderer.c:3718` memcpys cached BG pixels into `software_frame_surface`. Does NOT write any game state (`grep -n 'sa_bg_cache' src/port/sdl/sdl_game_renderer.c` — all writes are to local/render-side fields, no Game_ / PLW / State writes). **sa_bg_cache cannot cause a desync by itself.** It could explain the visual-only "black BG" on one peer without desync — but the user reports desync, so something else is simultaneous.

### C.5 Does any BG state live OUTSIDE the State struct?

Yes — already enumerated in A.3.1. But the two that WOULD explain the symptom (`bg_disp_off`, `Screen_Switch`/`Screen_Switch_Buffer`) are IN the State struct. They just aren't HASHED.

Corollary: if `bg_disp_off` or `Screen_Switch_Buffer` diverges between peers, the rollback+resim should **converge back** because they're restored from local saved state. The only way they stay divergent is if the CODE PATH that writes them differs between peers — which means something upstream already diverged.

### C.6 Single identified mechanism most consistent with symptoms

The "black BG" is a rendering-visible consequence of `bg_disp_off == 1` and/or `Screen_Switch_Buffer == 0` on the local peer. The most direct trigger that could fire mid-battle is `effect_K8_move` (`stage seraph form`, Gill-related). `effect_K8_init` is triggered indirectly via Gill's character code (stage000 / Gill char_table).

Ranking by evidence:
- (a) Both peers chose Gill, or Gill's seraph effect fires mid-match during a SA animation → effect_K8_move activates → BG turned off.
- (b) Without the peer-specific input, I cannot confirm whether Gill was in play. **Unverified — requires live match metadata or DEBUG dump to confirm.**

**The black-background symptom by itself is explained by `bg_disp_off` or `Screen_Switch_Buffer` being manipulated during a SA/seraph sequence. The DESYNC at the same frame is a different mechanism that must corrupt a hashed field.**

## Section D — CPU/timing interaction

### D.1 Does GekkoNet converge if rollback can't complete in one frame?

Looking at the GekkoNet public API at `third_party/GekkoNet/build/include/gekkonet.h`:
- Events are drained via `gekko_update_session` (returns `GekkoGameEvent**`). Client must process all of them.
- `gekko_network_poll` can be called separately.
- `input_prediction_window` at `GekkoConfig:67` caps speculative-advance depth.
- `GekkoDesyncDetected` event type at `gekkonet.h:144` — fires when remote checksum differs from local.

No public API exposes a "rollback couldn't complete" fallback. All internal. I do not have the `.cpp` source (only headers and the static lib `libGekkoNet.a`), so I cannot verify if there's a give-up-and-advance path. **Unverified from code.**

If Gekko DOES proceed without rollback after a budget exceedance, the local peer would advance from a speculative state while the remote advances from a confirmed state → immediate desync. This would be catastrophic at any frame count, not specifically 1274. **Unverified.**

### D.2 Timing-dependent early-exit in our advance/save/load?

- `save_state` at `src/netplay/game_state.c:1753-1757`: unconditional. No timing early-exit.
- `load_state` at `src/netplay/game_state.c:1759-1771`: unconditional memcpys.
- `advance_game` at `src/netplay/netplay.c:591-604`: unconditional `step_game`.
- No `if (deadline_exceeded) return;` or similar in any of these. **Safe.**

### D.3 Does pacer output feed simulation?

The frame pacer in `src/port/sdl/sdl_app.c` manages wall-clock to 60 Hz. Looking for pacer-to-sim feedback:

```sh
grep -rn 'precise_delay_ns\|pacer\|vsync_feedback' src/port/sdl/ --include='*.c' --include='*.h' | head -20
```

Only render-scheduling. No sim feedback. The perf counters in `game.c` (`perf_dispatch_ns`, etc.) track sim timing but only for telemetry — not fed back.

**Verdict: pacer cannot feed sim divergence.**

### D.4 `sa_bg_cache` write-back to game state?

```sh
grep -n '^\s*\(plw\|super_arts\|Round\|Random\|Attack\)\|G_No\|My_char' src/port/sdl/sdl_game_renderer.c | head -10
```
Returns nothing. No sim-state writes from the renderer.

The super-effect burst reduction at `apply_super_effect_burst_reduction_after_sort` (`sdl_game_renderer.c:3652-3900`) only reads `scr_sc`, `scrn_adgjust_x/y`, `render_task_count`, `render_tasks[]`. All reads — no writes to game-state globals. **Confirmed: sa_bg_cache does NOT write sim state.**

### D.5 CPU-pressure implications

With prediction window = 10 and stock 800 MHz, the rollback budget can exceed one frame (16.67 ms). Several code paths do take shortcuts under load:

1. `step_logic` / `run_netplay` / `step_game` all run unconditionally. No fast-forward skip based on real time.
2. `catch_up = need_to_catch_up() && (frame_skip_timer == 0)` at `netplay.c:748` — runs an extra sim tick if `frames_behind >= 1`. Both peers compute this independently based on their own `gekko_frames_ahead`. If the two peers' `frames_behind` differ, one may run extra sim ticks while the other does not. But the sim is driven by GekkoNet events (Load/Advance/Save), and `gekko_update_session` returns the authoritative list of events. Extra `step_logic` calls don't dispatch Advance events if none are pending. So this is safe.

### D.6 (c) Rollback-only failure modes [ADDED 2026-04-24]

Mac↔Mac runs clean because zero RTT → zero rollbacks. MiSTer↔MiSTer hits both rollbacks AND the black-BG/desync symptom. This subsection enumerates code paths that only exercise under rollback pressure.

#### D.6.1 Rollback flow, authoritative

From `src/netplay/netplay.c:684-714` (`process_events`):
- `GekkoLoadEvent` → `load_state_from_event` → `GameState_Load` (bulk memcpy of 604 GS_LOAD fields) + `SDL_copya(frw, es->frw)` (bulk effect pool restore).
- `GekkoAdvanceEvent` → `advance_game` → `step_game` → `njUserMain` → `cpLoopTask` → TASK_GAME's `func_adrs` (=`Game_Management`). The `rolling_back` field at `GekkoGameEvent.data.adv.rolling_back` is NOT propagated into sim; `advance_game` only uses it to suppress rendering (`!rolling_back`). The sim itself cannot distinguish speculative from confirmed advance.
- `GekkoSaveEvent` → `save_state` → `gather_state` + focused checksum.

Therefore any side effect `step_game` makes to state NOT in `GS_SAVE`/`EffectState` persists across rollbacks indefinitely until overwritten.

#### D.6.2 Function-static locals inside sim-path functions

`grep -rEn "^\s+static\s" src/sf33rd/Source/Game/ --include='*.c'` (function-scoped statics only):

| File:Line | Static variable | In sim path? | Rollback-safe? |
|---|---|---|---|
| ~~`rendering/color3rd.c:548`~~ | ~~`static u8 clut_tbl[32] = {...}`~~ | NO — render-side palette LUT initializer | Const-initialized (never written). Safe. |

**STALE 2026-08-30 — `clut_tbl` no longer exists.** It was deleted from `color3rd.c` by `b2c79d7c` ("giblet renderer and presenter", 2026-05-05); `grep -rn 'clut_tbl' src/` now returns nothing. The "only one function-scope static" conclusion is also stale: `grep -rEn "^\s+static\s" src/sf33rd/Source/Game/ --include='*.c'` currently returns 17 hits. None is a sim-path mutable counter:

- **`static const` lookup tables** (never written): `ui/sc_sub.c:323-324`, `menu/menu.c:714,715,812,4695`.
- **`static int cached = -1` env-var memos** (resolved once from `getenv`, then read-only): `ui/frame_trace.c:56,71`, `ui/frame_data_overlay.c:2589`, `engine/charset.c:46`.
- **DEBUG-only log budgets**, all inside `#if defined(DEBUG)` blocks: `system/sys_sub.c:927-928`, `stage/bg.c:270,1207-1208`.
- **Render-side scratch arrays** in `njdp2d_draw`: `rendering/dc_ghost.c:268-270`.

No lingering `static int counter = 0` pattern on the sim path. **Conclusion unchanged: safe.**

File-scope statics (writable, non-const) in sim source:

| File:Line | Variable | Written during battle? | In GS_SAVE? | Risk |
|---|---|---|---|---|
| `effect/effa5.c:15` | `static s16 bcdext` | Yes (pre-`sbcd`) | No | LOW — always reset to 0 before use at `effa5.c:71`. Effect A5 inactive mid-battle. |
| `system/pause.c:24` | `static u16 start_hold_counter[2]` | No (gated by `Mode_Type != MODE_NETWORK`) | No | NONE in MODE_NETWORK. |
| `stage/bg.c:52-55` | `static u8 bg_fastpath_active`, `static f32 bg_fastpath_scroll_x/y/z` | Written every REAL frame by `scr_trans` (`bg.c:606-611`) | No | NONE — render-only; read only by `ppgCalScrPosition` which is render-side. Does not feed sim. |
| `system/sys_sub.c:42` | `static bool training_hitbox_display_enabled` | Only in training menu | No | NONE in VS netplay. |

**Also**: `save_current_state` at `src/netplay/game_state.c:1642` declares `static PLW plw_scratch[2]`. This is a save-path scratch that's overwritten via `SDL_memcpy` before use at `game_state.c:1644`. No rollback leak — the static storage is never read before being overwritten. **Safe.**

Conclusion: no function-static variable leaks sim state between rollback iterations.

#### D.6.3 File-level globals written during `step_game` that are NOT in `GS_SAVE`

Re-ranking the prior §A.2.1 list under the "both peers go black" constraint. A global qualifies as a candidate divergence root ONLY if it (i) is written during battle sim, (ii) is read during battle sim, (iii) is not in State, (iv) can flow into a hashed field.

| Global | File:Line (write) | File:Line (read) | Fed to hash? | Rollback-symmetric? |
|---|---|---|---|---|
| **`chainex_check[2][36]`** | `engine/pls03.c:169,241,330,402,592,711,924,1023`; cleared by `clear_chainex_check` called from `engine/pls00.c:795,805,855,897,963,980,990,1040,1082,1151`, `engine/plpdm.c:195`, `engine/plpnm.c:81` | `engine/pls03.c:80,156,249,325,442,518,695,797` (gated by `wk->spmv_ng_flag2 & DIP2_UNKNOWN_23` and `DIP2_UNKNOWN_22`) | Yes, indirectly — gates PLW writes to `wk->sa->mp`, `wk->as`, `wk->permited_koa` at `pls03.c:166-170,238-242,327-331,399-403,589-593`, all of which are hashed via `plw_scratch[p]`. | **Mixed** — if both peers speculate with identical input prediction and identical starting state, they set the same `chainex_check` bits during speculation, and after rollback both have the same stale bits → symmetric. If peers roll back different depths, the set-of-bits leftover differs → asymmetric. |
| `ca_check_flag` (s8) | `engine/plcnt.c:639,1124`, `plmain.c:1187,1205`, `plcnt2.c:109` | `engine/hitcheck.c:64` | Gates hit-check branches that write PLW fields | **Symmetric if deterministically recomputed each tick**; the question is whether `ca_check_flag` is written at the START of the tick's processing (overwriting stale value). Need to re-audit. |
| `grdb[2][2][2]`, `grdb2[2][2]` (s16) | `engine/hitcheck.c:43-49,53-54` via `make_red_blocking_time`/`cmd_data_set` | `hitcheck.c:1047,1150,1206` | Feeds into `hit_check_main_process` which writes PLW fields | Set at cmd-init (stage/char load); stable mid-battle. **Symmetric.** |
| `chk77_flag` (s16) | `effect/eff77.c:159` (init to 0) | inside effect 77 | Effect 77 writes to PLW via `plw[0/1].wu.disp_flag` at `eff77.c:50-51,110-111` | **Symmetric** if both peers enter eff77 at the same frame. |
| `hs[32]`, `hpq_in`, `mkm_wk[32]`, `q_hit_push[32]` | per-tick fills in `hit_push_request` / `attack_hit_check` | same tick in `hit_check_main_process` | Yes, indirectly | **Cleared by `clear_hit_queue` at hitcheck.c:1913-1916 each tick** — no cross-tick residue. Safe. |

**Most of the uncovered-but-sim-touching globals are scratch buffers that self-reset each tick. `chainex_check` is the notable exception** — it is SET on one tick and cleared by a TRANSITION on another tick, so its lifetime spans multiple ticks, making it rollback-sensitive.

#### D.6.4 Rollback-unsafe mechanism analysis: `chainex_check` re-evaluated

Symmetry analysis (addressing the user's question in the prompt):

- **Scenario 1 (zero RTT, Mac↔Mac)**: no rollback. `chainex_check` is set and cleared in natural order. No residue. No divergence. Matches observation.
- **Scenario 2 (both peers roll back same depth K)**: both peers speculate to frame N, both set `chainex_check` during speculation. Both load state from frame N-K. `chainex_check` retains speculative-era bits on BOTH peers identically. Both resim frame N-K..N with the SAME stale bits. Both produce the SAME (wrong) hashed PLW state. **No desync fires** (both checksums match each other), but both go down the "chainex gated off" path → possibly triggers the "both peers go black" scenario if that gated path reaches `Bg_Off_R`.
- **Scenario 3 (peers roll back DIFFERENT depths)**: typical network-jitter pattern. Peer A rolls back 8 frames, peer B rolls back 6. Peer A's speculative pass set `chainex_check[id][ix]` at relative frame (say) -3 during speculation. Peer B's speculative pass set it at relative frame -1. After load_state, peer A has MORE speculative-set bits, peer B has FEWER. Resim paths diverge on the gating check at `pls03.c:122,194,...`. **Desync fires**. Black BG could also fire on just one peer OR both, depending on which `chainex_check` bits linger.

Observation: the MiSTer↔MiSTer desync at frame 1274 with BOTH peers showing black BG is consistent with Scenario 2 (both go black symmetrically from stale chainex_check) combined with some OTHER mechanism (rollback-depth-asymmetric) causing the actual checksum mismatch. OR it is consistent with Scenario 3 where the gating paths diverge enough to produce a matching black BG on both peers AND different hashed-state bits.

`chainex_check` still fits the evidence but it is NOT a "both peers go black" mechanism alone — it needs a partner divergence source. This downgrades its standalone rank.

#### D.6.5 Other rollback-symmetric mechanisms

- **Effect-pool timing** (`exec_tm`, `frwctr`, `head_ix`, `tail_ix`, `frwque[]`): ARE in `EffectState`, restored wholesale on rollback. Symmetric across rollbacks. **Not a divergence source.**
- **RNG indices** (`Random_ix16`/`32`/`_ex`/`_com`/`_ex_com`): ARE in State and IN the hash. Symmetric.
- **`system_timer`** (`sf33rd/Source/Game/game.c:156`): incremented each `step_game` call, saved at `game_state.c`. IS saved. Symmetric.
- **PIE-dependent pointer VALUES in saved structs** (task[], waza_work[].w_ptr, WORK_Other.my_master, WORK.*_adrs, etc.): local to each peer, restored from own local save. Internal consistency maintained. Cannot desync the focused hash because PLW pointer fields are zeroed by `sanitize_*_pointers`.

#### D.6.6 What IS the rollback-asymmetric mechanism?

Given the analysis above, candidate mechanisms that could produce a CHECKSUM MISMATCH under rollback-depth-asymmetric conditions while ALSO producing a symmetric black-BG outcome:

1. **`chainex_check` Scenario 3** — asymmetric bit-residue after different rollback depths. Fits desync. Partially fits black-BG (indirect — via downstream PLW-state divergence that triggers effect-77/effect-K8 asymmetrically).
2. **GekkoNet internal rollback-budget exhaustion** (if the library commits a speculative frame when it can't resim in time). Fits desync on MiSTer (CPU-bound). Does NOT by itself explain symmetric black-BG. Unverified — GekkoNet source not available.
3. **Unhashed saved state accumulating asymmetric drift, then converging to a black-BG path on both peers via a deterministic-on-current-state mechanism, while drifting further in unhashed regions**. Mechanism-only; no specific field identified.

#### D.6.7 Heap allocations during rollback

`grep -rn "malloc\|SDL_malloc\|SDL_calloc\|calloc" src/sf33rd/Source/Game/ src/netplay/` returns zero hits inside the sim path (all mallocs are in `sound3rd.c:224` [boot init] and `src/netplay/direct_p2p.c:2212,2372` / `src/netplay/sdl_net_adapter.c:423,426,430` / `src/netplay/stun.c:581` (`StunDnsJob` alloc) [transport, outside sim]). **No heap allocation during `step_game` → no memory-ordering rollback bugs.**

## Section E — Upstream cross-reference

### E.1 Comparison against 3sxtra (`/tmp/3sxtra`)

- Both `src/netplay/game_state.c` files have similar `GS_SAVE` coverage. Diff:
  ```
  comm -13 /tmp/3sxtra_saved.txt /tmp/saved_names.txt
  # (our extras over 3sxtra): combo_type, Disp_Input_History, remake_power
  ```
  We save 3 extras; 3sxtra saves NOTHING we don't. Coverage parity or better.
- Focused checksum fields: identical set, PLUS our fork hashes `combo_type` and `remake_power` per `research-3sxtra-netplay-port.md:1305-1311`. No upstream field that we miss.

### E.2 Research doc §19 risks cross-reference

From `docs/archive/research-3sxtra-netplay-port.md:1303-1344`:

| Risk | Status in our code |
|---|---|
| R1: combo_type/remake_power storage mismatch | **ADDRESSED** — hashed at `game_state.c:1715-1716`. |
| R2: WORK.operator rename + cb/rp divergence | **ADDRESSED** — `sanitize_plw_pointers` zeros `cb`/`rp` at `game_state.c:1566-1567`. |
| R3: Hitcheck turbo micro-optimization | **NOT CHERRY-PICKED** — our `hitcheck.c` retains the original switch form. Not a concern. |
| R4: `select_timer_state` module missing | **PORTED** — module present at `src/sf33rd/Source/Game/select_timer.c` and `select_timer_state` saved in the GameState save/load pair. BUT comment at `select_timer.c:12-17` says the module is NOT invoked by the engine today — `effa5.c` still drives `Select_Timer`. `select_timer_state.step`, `.is_running`, `.timer` all stay zero across the whole match, saved identically on both peers. Not a live risk in battle. **SUPERSEDED 2026-08-29 (task #109): the module and the `select_timer_state` GameState member are both GONE.** Because the field was permanently zero and nothing called `SelectTimer_Init/Run/Finish`, removing it re-converges us with upstream `33dfd75b` (#216), which had deleted the module and the member when it moved the countdown into effect A5. The ARM32 struct-size pin in game_state.c was re-measured from 17784 to 17772 in the same commit. The GameState line numbers this row originally quoted were already stale before the removal and have been dropped rather than re-pinned at a member that no longer exists. |
| R5: Release-build checksum coverage gap | **ADDRESSED** — focused checksum is unconditional of DEBUG (`game_state.c:1610-1765`). |
| Honorable mention: `_Static_assert` on `sizeof(GameState)` / `sizeof(_TASK)` | **PRESENT** at `game_state.c:55-64`. |

### E.3 Other netplay docs

- `docs/archive/plan-netplay-port.md` — implementation plan, no additional risks listed.
- `docs/archive/research-3sxtra-netplay-port.md` §9.7 "Cross-architecture netplay compatibility" — 64-bit/32-bit cross-play fundamentally broken because focused checksum hashes PLW as BYTES not as SEMANTICS, and struct layouts diverge between word sizes. Not relevant for our MiSTer↔MiSTer case (both ARMv7 32-bit).
- `docs/agent-memory/netplay-*.md` — exist but mostly process notes.

### E.4 Upstream non-determinism sources the port doesn't fully address

- `Play_Mode` read by `random_*_com` / `random_*_ex_com` at `pls02.c:662,678,692,707`. `Play_Mode` saved at `game_state.c:389` but NOT hashed at `game_state.c:1670-1728`. In netplay, `setup_vs_mode` sets `Play_Mode = 0` at `netplay.c:207`. As long as both peers stay in Play_Mode=0, the _com variants fall through to `random_*()` (non-_com) — same deterministic LUT. Unverified whether any code mid-battle sets Play_Mode != 0. If so, the _com indices would advance at the divergent moment.
- `save_w[MODE_NETWORK].extra_option` — see §A.3.7. Per-peer DIP switch config. Feeds `omop_spmv_ng_table` which is then OR'd into `PLW.spmv_ng_flag` (which IS hashed). Would produce a frame-1 desync, not a frame-1274 desync.

## Section F — Bg_Disp_Switch(1) / BG-off exit audit [ADDED 2026-04-24 rollback-axis]

Per the user's Task 4, enumerate every call site that sets the BG-dark state, trace each to its paired "exit" that restores BG visibility, and check whether the exit's gating state is in `GS_SAVE`.

### F.1 `Bg_Disp_Switch(on_off)` direct writes to `bg_disp_off`

Sim-path callers (via `grep -rn 'Bg_Disp_Switch' src`):

| Caller | File:Line | Enters / Exits | Gating state for exit |
|---|---|---|---|
| `effect_K8_move` routine_no[0]==0 | `src/sf33rd/Source/Game/effect/effk8.c:27` | Enter (`Bg_Disp_Switch(1)`) | none — unconditional on `case 0:` first-pass |
| `effect_K8_move` routine_no[0]==1, `dead_f != 0` branch | `src/sf33rd/Source/Game/effect/effk8.c:38` | Exit (`Bg_Disp_Switch(0)`) | `ewk->wu.dead_f` (WORK_Other, saved via EffectState). Rollback-safe. |
| `effect_K8_move` routine_no[0]==1, dir-change branch | `src/sf33rd/Source/Game/effect/effk8.c:52` | Exit (`Bg_Disp_Switch(0)`) | `ewk->wu.dir_old != mwk->now_koc \|\| ewk->wu.dir_step != mwk->char_index`. WORK_Other fields saved via EffectState; `mwk->now_koc`, `mwk->char_index` are PLW fields, hashed. Rollback-safe. |

**Every `Bg_Disp_Switch(1)` entry has a paired `Bg_Disp_Switch(0)` exit in the same effect function, gated by state that IS `GS_SAVE`-covered.** No unpaired enter. No unsaved gating flag on the exit path. Conclusion: the `bg_disp_off`-via-`Bg_Disp_Switch` path is rollback-safe.

### F.2 Direct writes to `bg_disp_off` that bypass `Bg_Disp_Switch`

| File:Line | Write | Context | Rollback-safe? |
|---|---|---|---|
| `src/sf33rd/Source/Game/stage/bg.c:240` | `bg_disp_off = 0` | inside `Stage_Init` / init-time path | YES — init-only, never during rollback |
| `src/sf33rd/Source/Game/stage/bg_sub.c:1111` | `bg_disp_off = 0` | inside `bg_initialize` | YES — called at stage load, not during mid-match rollback |
| `src/sf33rd/Source/Game/stage/bg_sub.c:1202` | `bg_disp_off = 0` | inside `bg_etc_write` | YES — called at stage-switch / ending transitions, not during in-match rollback |
| `src/sf33rd/Source/Game/ending/end_main.c:250` | `bg_disp_off = 0` | ending path | YES — post-match |
| `src/sf33rd/Source/Game/stage/bg.c:1508` | `bg_disp_off = on_off` | inside `Bg_Disp_Switch` itself | (covered by F.1) |

All direct writes are init-time or ending-time paths. None are steady-state sim-path writers. **No hidden direct writes bypass the F.1 analysis.**

### F.3 `Bg_Off_R` / `Bg_Off_W` / `Bg_On_R` / `Bg_On_W` (layer-bit manipulators)

These functions, defined at `src/sf33rd/Source/Game/stage/bg.c:1423-1447`, modify `Screen_Switch` and `Screen_Switch_Buffer` simultaneously. Both vars are in `GS_SAVE` (`src/netplay/game_state.c:581-582`). `Scrn_Renew` at `bg.c:1445` re-mirrors; but it's called from `game_step_1` at `src/main.c:611` which runs ONCE per real frame, NOT during rollback.

Sim-path callers where the ENTER and EXIT transitions are paired in the same effect / sequence:

| Effect | Enter | Exit | Gating state |
|---|---|---|---|
| `effect_77_move` (Gill seraph / SA freeze) | `eff77.c:71,92` (`Bg_Off_R(1<<i)` per mask) | `eff77.c:118` (`Bg_On_R(1<<i)` per mask) | Exit gate: `old_rno[0] <= 0` at `eff77.c:107`. `old_rno[0]` is WORK_Other, saved via EffectState. Exit ALSO implicitly gated by `Game_pause == 0 && EXE_flag == 0` at `eff77.c:104` (both saved). **Rollback-safe.** |
| `akebono_finish` (Ryu/Ken SA ending, stage 4 Akebono) | `effd3.c:56` (`Bg_Off_R`) | `effd3.c:114` (`Bg_On_R`) | Exit gate: `ewk->wu.dir_timer == 0` decrementing from `ake_timer_tbl[12] = 20`. WORK_Other field, saved. **Rollback-safe.** |
| `syungoku_finish` (Gouki's Shun Goku Satsu SA) | `effd3.c:156` (`Bg_Off_R`) | `effd3.c:196` (`Bg_On_R`) | Exit gate: `ewk->wu.old_rno[0]` non-zero → jumps to routine_no[0]=3. WORK_Other, saved. Plus `akebono_flag = 0` at `effd3.c:187` — `akebono_flag` IS saved. **Rollback-safe.** |
| `effect_77_init` | `eff77.c:171` (`sa_pa_flag = 1`) | (paired with eff77_move routine exit) | `sa_pa_flag` is saved. Rollback-safe. |

**All Bg_Off_R/Bg_On_R pairs in the sim path use exit conditions that are either in `GS_SAVE` (`akebono_flag`, `sa_pa_flag`, `Game_pause`, `EXE_flag`) or live in the effect-pool WORK_Other fields (saved via `EffectState`).**

### F.4 What's NOT in GS_SAVE that could force the BG-off path to fire redundantly

The candidate mechanism from the user's corrected framing is: a **speculative** enter happens (e.g., speculative seraph fires → `seraph_flag = 1`, `Bg_Disp_Switch(1)`), rollback restores `seraph_flag` and `bg_disp_off` (both saved), but some UPSTREAM flag that drives the entry decision is NOT saved. On resim, the upstream flag is stale and re-triggers the entry. Let me enumerate the upstream triggers:

- `effect_K8_init` caller: `src/sf33rd/Source/Game/engine/plpat00.c` / Gill-specific pattern code. Triggered by PLW state (`wu.pat_status`, `routine_no[*]`, `as->...`) — all saved. Rollback-safe.
- `effect_77_init` caller: chained from `pls03.c` SA-firing paths. Triggered by PLW state + SA gauge. Saved.
- `effect_D3_init` caller: `engine/pls03.c` Akuma SGS / Ryu SA ending. PLW state. Saved.

The only upstream triggers that DON'T depend on PLW state are:
- `chainex_check[wk->wu.id][...]` gates at `pls03.c:122,194,283,355,521,640,845,943` — if set (speculatively), BLOCKS the SA-chain attempt. This indirectly affects which effect spawns; not a direct BG-off trigger but a blocker that forces a DIFFERENT SA outcome. See Candidate R2.
- `ca_check_flag` — gates `catch_hit_check` which writes `hs[]`. Indirectly affects PLW dm_* fields → indirectly affects whether a SA even fires. See Candidate R3.
- `grdb`/`grdb2` — only changed via metamorphose. See Candidate R4.

### F.5 Finding: no direct unsaved BG-exit gating state

The audit finds **no unsaved flag that directly gates the BG-off exit path**. All `Bg_Disp_Switch(0)` / `Bg_On_R` / `Scrn_Renew` exits are gated by state that is either in `GS_SAVE` or in the effect pool. This is surprising under the user's hypothesis — it means the BG-black symptom must be a **downstream** effect of a PLW divergence that makes the enter-SA-freeze path fire without a matching exit, OR a genuine desync of `Screen_Switch_Buffer` caused by different Bg_On_R/Bg_Off_R call SEQUENCES between peers (even though both flags are saved).

Reconciling with symmetric BG: under the rollback-axis framing, the sequence-difference explanation is:
1. Speculative pass sets `Screen_Switch_Buffer` bits via `Bg_Off_R(mask)` at `eff77.c:71`.
2. Rollback restores `Screen_Switch_Buffer` to its pre-speculation value (correct).
3. Resim re-executes the same `Bg_Off_R(mask)` call because PLW-driven state is identical (correct).
4. **But the EXIT `Bg_On_R(mask)` at `eff77.c:118` depends on `old_rno[0]` decrementing to <= 0 each tick. If `old_rno[0]` is per-effect-instance WORK_Other, it IS saved. The decrement is driven by `Game_pause==0 && EXE_flag==0` — both saved. So the exit must fire exactly as expected.**

In short — the Bg_Disp_Switch/Bg_Off_R machinery appears rollback-safe when examined directly. The divergence must come from **a non-BG upstream flag that makes the enter happen at a different moment between peers, or makes the exit NOT happen at the expected moment**.

### F.6 Finding stated explicitly (per user request: "if you can't find an exit path, say so")

For every `Bg_Disp_Switch(1)` call (only site: `effk8.c:27`), the corresponding `Bg_Disp_Switch(0)` exits are found (`effk8.c:38,52`). For every `Bg_Off_R(mask)` call in the sim path (at `eff77.c:71,92`, `effd3.c:56,156`), the corresponding `Bg_On_R(mask)` exits are found (`eff77.c:118`, `effd3.c:114,196`). All exit gates use `GS_SAVE`-covered or effect-pool (EffectState-covered) state.

**No direct exit-path failure identified.** The BG-black symptom therefore cannot be explained by a simple "the exit flag is unsaved so rollback leaves the BG off." It must be an upstream PLW / effect-pool divergence that causes the exit sequence to be skipped at the routine-level. The rollback-unsafe flags in Candidates R2-R4 are the only plausible upstream divergence sources identified so far.

## Section G — Minimum-change elimination experiment [ADDED 2026-04-24 rollback-axis]

Per Task 5: a single code change that would either prove or disprove the rollback-unsafe-BG-flag mechanism.

### G.1 Proposed instrumentation (NOT a fix — detection only)

**Add `chainex_check[2][36]` to the focused checksum.** One line, one location:

- File: `src/netplay/game_state.c`
- Line: after line 1728 (the last `djb2_update_mem` in the focused-hash list)
- Line to add: `h = djb2_update_mem(h, (const uint8_t*)chainex_check, sizeof(chainex_check));`
- Required: add `extern u8 chainex_check[2][36];` near the top of `src/netplay/game_state.c` after other externs (around line 40).

**No `GS_SAVE(chainex_check)` / `GS_LOAD(chainex_check)` yet.** Adding save/restore would be the FIX. We want the DETECTION first.

### G.2 What this experiment tells us

Three possible outcomes on next MiSTer↔MiSTer repro (with DEBUG build for `dump_desync_state`):

| Outcome | Interpretation |
|---|---|
| **Desync fires EARLIER than frame 1274** (say, frame 400-800) | `chainex_check` diverges between peers before 1274. Rollback-unsafe chainex_check is a real contributor. Confirms Candidate R2. Next step: add `GS_SAVE`/`GS_LOAD` coverage and retest. |
| **Desync fires at approximately the same frame (1250-1300)** | `chainex_check` does diverge, but so do other things at the same moment. Chainex_check is correlated but not singly responsible. Need to add additional detection fields (`ca_check_flag`, `grdb`). |
| **Desync fires at LATER frame or doesn't fire** | `chainex_check` is not a contributor; the earlier desync at 1274 was a symptom of something else. Eliminates Candidate R2. Focus shifts to Candidate R3 (`ca_check_flag`) and GekkoNet budget (Candidate R6). |

### G.3 Why chainex_check specifically

Of the five actionable candidates (R2-R6), chainex_check is:
1. The only one with a confirmed file-level static-like declaration and clear speculative-write-without-matching-clear structure.
2. The only one with 16+ distinct write sites in the sim path (high probability of firing during any given session).
3. The cheapest to instrument (one line of code).
4. Directly implicates a gate flag, so a symmetric speculative-set produces identical PLW divergence on both peers — the symmetric-BG symptom is explained.

Other candidates need more complex instrumentation (`ca_check_flag` is 1-byte so easy, but writes are many; `grdb` is 16 bytes and only fires during Twelve matches; `omop_spmv_ng_table` is 8 bytes per peer).

### G.4 Secondary instrumentation (only if G.1 is inconclusive)

If outcome G.2 row 2 (same frame): add a second `djb2_update_mem` line at `src/netplay/game_state.c:1729` for `ca_check_flag`:

```c
h = djb2_update_mem(h, (const uint8_t*)&ca_check_flag, sizeof(ca_check_flag));
```

Requires `extern s8 ca_check_flag;` near the file top.

If outcome G.2 row 3 (no effect): the culprit is not an unsaved file-level global. Proceed to GekkoNet rollback-budget instrumentation (log `frame_max_rollback` at `netplay.c:729-738` in DEBUG builds).

### G.5 Not proposed: the fix itself

Per the user's constraint, this section proposes detection only. A subsequent fix (if R2 is confirmed) would be:

- Add `u8 chainex_check[2][36];` to `GameState` (`src/netplay/game_state.h:29-709`).
- Add `GS_SAVE(chainex_check)` at the appropriate section of `src/netplay/game_state.c`.
- Add `GS_LOAD(chainex_check)` in `load_state`.
- Remove the `extern` and the detection `djb2_update_mem` call — they're no longer needed once the value is part of State proper.

But first confirm the mechanism with the instrumentation-only change.

## Ranked candidate causes (2026-04-24, rollback-axis) [REVISED 2026-04-24 rollback-axis]

**Primary filter (rollback-axis):** under the revised framing, a candidate must pass three filters:

1. **Fires during speculative rollback** (not only during confirmed advance). Otherwise it would fire on Mac↔Mac localhost, contradicting the clean 3000+ frame baseline.
2. **Produces symmetric contamination** (both peers compute identical stale state on resim). Otherwise one peer would go black and the other wouldn't — contradicting "both peers show black."
3. **Plausibly gates the BG-off exit** (`bg_disp_off = 0` / `Bg_On_R(mask)` sites) OR produces a downstream PLW divergence that in turn prevents those exits from firing. Otherwise it's a generic desync candidate, not a black-BG explanation.

Under these filters, the previously top-ranked "GekkoNet budget exhaustion" drops in rank — it's unfalsifiable from code, doesn't directly gate a BG exit path, and is symmetric only by coincidence. The new top candidates are rollback-unsafe exit-gating flags inside the effect system itself.

### Candidate R1 (ELIMINATED on audit): `aku_flag` / `sa_pa_flag` / `seraph_flag` / `akebono_flag` — SA/EX-freeze exit flags

**Rank: ruled out; included for completeness.** (The user's Task 3 specifically asked to audit this class; the audit confirms these flags are all saved.)

- **Declarations (both in `GS_SAVE`)**: `aku_flag` and `sa_pa_flag` are both saved (`src/netplay/game_state.c` grep: `GS_SAVE(aku_flag); GS_SAVE(sa_pa_flag)`). So are `akebono_flag`, `seraph_flag`, `EXE_flag`, `Game_pause`, `Extra_Break`, `Pause_Hit_Marks`, `Suicide[8]`, `Conclusion_Flag`, `Conclusion_Type`.
- **Status vs filter 1 (fires during rollback)**: the exit transitions from these states live at `src/sf33rd/Source/Game/effect/eff77.c:104-125` (`sa_pa_flag = 0` when `old_rno[0] <= 0`), `src/sf33rd/Source/Game/effect/effd3.c:121,187,218` (`akebono_flag = 0`), `src/sf33rd/Source/Game/effect/effk8.c:26,39,51` (`seraph_flag` toggles), and `src/sf33rd/Source/Game/engine/manage.c:231,385` (`seraph_flag = 0`, `Extra_Break = 0` at end-of-round). These states are set/cleared through effect-pool `routine_no[0]` machinery, which IS saved via `EffectState`. **Filter 1 passes via a different route**: the flag writes happen on the sim path, but the read sites at `src/sf33rd/Source/Game/stage/ta_sub.c:163` (`obr_no_disp_check`) feed `disp_pos_trans_entry`/`entry5` which suppresses sprite rendering. The BG is suppressed earlier — via the `Bg_Off_R` calls at `eff77.c:71,92` and `effd3.c:56,97,156` that clear `Screen_Switch_Buffer` bits. `Screen_Switch_Buffer` IS saved, so rollback restores it correctly. **Net: the flags themselves are all GS_SAVE-covered. R1 is ruled out as a standalone divergence source for the reason that the prior pass already noted — no unsaved BG-state in this chain.**
- **But there is a wrinkle**: the exit code at `eff77.c:104-125` depends on `ewk->wu.old_rno[0]` (WORK-local in the effect pool, saved) decrementing on each tick where `Game_pause` and `EXE_flag` are both zero. If during rollback the `Game_pause`/`EXE_flag` flags flip (both ARE saved, so they shouldn't), the decrement rate could drift. Verified: both flags are saved at `src/netplay/game_state.c`. **Filter 2 and Filter 3: safe.**
- **Outcome**: **R1 is mechanism-clean**. It is included here explicitly because the user asked for the audit; the good news is that the SA-freeze / seraph / shun-goku-satsu exit flags are all rollback-safe.

### Candidate R2 (NEW TOP): `chainex_check[2][36]` — rollback-unsafe EX-SA chain gating (was Candidate 0a)

**Rank: #1 actionable candidate under the rollback-axis framing (promoted from #2 under the 32-bit-axis framing).**

- **Declaration**: `src/sf33rd/Source/Game/system/sysdir.c:12`. `u8 chainex_check[2][36]`. **NOT in `GS_SAVE` as of 2026-04-24 — no longer true.** It is now a `GameState` member (`src/netplay/game_state.h`, field `chainex_check`), copied in `GameState_Save` and restored in `GameState_Load` (`src/netplay/game_state.c`). See the STATUS block at the top.
- **Sim-path writes**: `src/sf33rd/Source/Game/engine/pls03.c:169,241,330,402,592,711,924,1023,1040` (sets `chainex_check[wk->wu.id][...-20] = 1` at six distinct EX-SA cancel/chain success branches).
- **Sim-path reads**: `src/sf33rd/Source/Game/engine/pls03.c:122,194,283,355,521,640,845,943` (if set, inhibit further chain-ex attempt; skip writes to `wk->sa->...`, `wk->as`, `hissatsu_setup_union(wk, ...)`).
- **Clear sites**: `src/sf33rd/Source/Game/engine/plcnt.c:1438-1444` (`clear_chainex_check(ix)`), called from at least `src/sf33rd/Source/Game/engine/pls00.c:795,805,855,897,963,980,990,1040,1082,1151`, `src/sf33rd/Source/Game/engine/plpdm.c:195`, `src/sf33rd/Source/Game/engine/plpnm.c:87`.
- **Filter 1 (rollback)**: passes. Written during speculative SA chain attempts; rollback does not restore; stale bits persist on resim.
- **Filter 2 (symmetry)**: passes IF both peers roll back through the same chain-ex attempt. The set sites all fire on the same `pls03` chain-check transition — reached from the `nm_NNNNN` normal-state family in `src/sf33rd/Source/Game/engine/pls00.c` via `process_normal` — which is `routine_no` + `pat_status` driven via PLW — both of which ARE hashed. So both peers' speculative passes set the SAME `chainex_check` bits; after load_state neither peer has the bits cleared; both resim with the same stale bits → **symmetric stale state**.
- **Filter 3 (gates BG exit)**: **partial.** chainex_check does not directly write `bg_disp_off` or `Screen_Switch_Buffer`. But the gated code path writes `wk->sa->mp`, `wk->as`, `wk->sa->ok`, and calls `hissatsu_setup_union(...)`. If the SA setup is suppressed because `chainex_check` is stuck at 1, the SA cancel/chain doesn't fire → `effect_77_init`/`effect_K8_init` may not be called → the "entering SA freeze → Bg_Off_R → dark BG" path doesn't execute *for the speculative-set moment*. BUT the NEGATIVE scenario is more interesting: if the speculative pass set `chainex_check = 1` and then the EXIT transition to clear it never fires (e.g., SA ends and clear_chainex_check would be called inside the SA finish, but the stale value was SET during a speculative attempt that the confirmed advance never actually made), the stale 1 lingers. Subsequent real chain-ex attempts are silently blocked, causing a downstream PLW divergence that cascades into different effect spawns → eventually either `effect_77` or `effect_K8` fires on both peers but with different timing, leaving `Screen_Switch_Buffer` and `bg_disp_off` in a desynchronized state.
- **Concrete failure scenario**: frame N-K: peer A & B both speculate a chain-ex success; set `chainex_check[0][gix] = 1`. Frame N-K+1 (confirmed): the chain-ex attempt actually does NOT succeed (different inputs than speculated), so the clear path at `plcnt.c:1438` is never hit. Load_state rolls back PLW but not `chainex_check` → bit stays 1. Subsequent frames: the next legitimate chain-ex attempt for that gauge-index is silently blocked on BOTH peers identically (symmetric). Eventually the inhibited chain causes a divergent SA routine between the two peers (e.g., peer A's next SA firing takes slightly different `wk->as` path because the earlier chain was blocked, while peer B's does differently due to rollback-depth asymmetry). Frame 1274: divergence crosses into a hashed byte.
- **Best-available code-visible candidate**. File:line evidence is strong.

### Candidate R3 (NEW, MEDIUM): `ca_check_flag` (catch/air-recovery check flag)

**Rank: #2 actionable candidate.**

- **Declaration**: `src/sf33rd/Source/Game/engine/hitcheck.c:39`. `s8 ca_check_flag`. **NOT in `GS_SAVE` as of 2026-04-24 — no longer true.** Saved since `7e07db3f`; it is a `GameState` member (`src/netplay/game_state.h`, field `ca_check_flag`).
- **Sim-path writes (transition-triggered)**: `src/sf33rd/Source/Game/game.c:554` (`Game2_0`), `:655` (`Game2_2`), `:1364` (`Game09`) (round-init, set to 1); `src/sf33rd/Source/Game/engine/plcnt.c:642` (`pli_1000` → set to 1 when both players' `routine_no[0]` reaches 3 and `Allow_a_battle_f` is set, i.e. entering "round start" state); `src/sf33rd/Source/Game/engine/plcnt.c:1137` (`setup_settle_rno` → cleared to 0 when a player dies); `src/sf33rd/Source/Game/engine/plcnt2.c:109` (set to 1 on round-start); `src/sf33rd/Source/Game/engine/plmain.c:1173,1191` (both in `check_omop_vital`: set to 1 when player_number==0 enters routine_no[1]==4, routine_no[2]==21; cleared to 0 when `vital_new < 0`).
- **Sim-path reads**: `src/sf33rd/Source/Game/engine/hitcheck.c:63` — `if (ca_check_flag) catch_hit_check();` — gates a per-tick call to `catch_hit_check` which writes to PLW (`hs[].flag.results`, `wk->wu.dm_*`, etc. via `set_caught_status` at `hitcheck.c:87`).
- **Filter 1 (rollback)**: passes. Written during specific PLW-transition events (entering routine_no[1]==4, player dies) and during round starts. Rollback does not restore; whatever value it had at the end of the speculative pass persists.
- **Filter 2 (symmetry)**: mixed. The writes at plmain.c:1187,1205 are gated by PLW state which IS hashed; so both peers transition identically → both set/clear identically → symmetric stale residue after rollback.
- **Filter 3 (gates BG exit)**: INDIRECT. If `ca_check_flag` is stuck at 1 (speculatively set during a near-death air recovery that the confirmed advance didn't reach), `catch_hit_check` fires an extra time on both peers. That extra catch attempt writes PLW (`hs[i]`, `wk->wu.dm_koa`, `dm_piyo`, `dm_vital`). If the extra write happens at a frame where neither peer's PLW should have been catch-checked, both peers now have slightly-wrong PLW state — still symmetric — which then drives `effect_77`/`effect_K8` asymmetrically as rollback depths drift.
- **Concrete failure scenario**: late-round, player 1 is at low vitality. Speculative pass: P1 enters routine_no[1]==4, routine_no[2]==21 (a specific super-art-absorb state); `ca_check_flag = 1`. Confirmed advance: different inputs, P1 never enters that state. Load_state: PLW restored to pre-state, but `ca_check_flag` stays 1. Next real frame: `hit_check_main_process` calls `catch_hit_check` unexpectedly; an "ambient" catch-hit fires, writing PLW. Both peers have the same stale flag, both fire the same ambient catch, both PLWs mutate identically (symmetric). But the extra mutation pushes PLW state to a specific corner where a subsequent chain-ex attempt diverges between peers due to rollback-depth-asymmetric PLW register state. Cascades to the hashed-byte divergence at frame 1274.

### Candidate R4 (NEW, LOW-MEDIUM): metamorphose-triggered `grdb` / `grdb2` LUT contamination via `cmd_id`

**Rank: #3 actionable candidate.**

- **Declarations**: `src/sf33rd/Source/Game/engine/hitcheck.c:32-33`. `s16 grdb[2][2][2]`, `s16 grdb2[2][2]`. **NOT in `GS_SAVE` (confirmed).**
- **Sim-path writes**: `src/sf33rd/Source/Game/engine/hitcheck.c:44-45,49-50,54-55` inside `make_red_blocking_time`. Caller chain: `cmd_data_set` (`engine/cmd_main.c:66`) → called from `waza_compel_all_init` (`cmd_main.c:1778-1843`) → called from `cmd_init` (`cmd_main.c:76-85`) → called from `set_base_data` and `set_base_data_metamorphose` (`engine/plcnt.c:1341,1367`).
- **Sim-path reads**: `src/sf33rd/Source/Game/engine/hitcheck.c:1047,1150,1206` (in `attack_hit_check` / `catch_hit_check` / `set_struck_status` via `wcp[cmd_id].reset[i]` and adjacent paths). Affects red-parry frame counts, which affect per-hit guard/parry outcome → PLW-hashed `paring_counter`, `Guard_Flag`, etc.
- **Filter 1 (rollback)**: passes only if `set_base_data_metamorphose` fires mid-round (Twelve's X.C.O.P.Y. SA). Round-init `set_base_data` fires once at round-start, before steady-state play. The metamorphose call at `effk7.c:72,135,176` runs mid-round when Twelve transforms.
- **Filter 2 (symmetry)**: both peers speculate the same Twelve transform (same PLW state → same routine decision). Both peers set identical `grdb`/`grdb2` values. Rollback: PLW restored, but `grdb`/`grdb2` retain post-metamorphose values. If the metamorphose never actually happened in confirmed advance (peer inputs differed from speculation), the stale grdb is wrong for BOTH peers identically → **symmetric stale state**.
- **Filter 3 (gates BG exit)**: INDIRECT. `grdb` affects red-parry windows, which affect `paring_counter` (hashed). Subsequent parry attempts produce slightly different frame-count PLW outcomes on both peers. Symmetric. Eventually drives a divergent SA/chain path that triggers different effects → indirectly affects BG-off timing.
- **Why lower-ranked**: only fires in Twelve matches. Frame-1274 desync doesn't necessarily implicate Twelve.

### Candidate R5 (NEW, LOW): `omop_spmv_ng_table` / `omop_spmv_ng_table2` — per-player DIP flags

**Rank: #4 actionable candidate.**

- **Declarations**: `src/sf33rd/Source/Game/engine/plcnt.c:95-96`. `u32 omop_spmv_ng_table[2]`, `u32 omop_spmv_ng_table2[2]`. **NOT in `GS_SAVE` (confirmed; FIXME comment at `plcnt.c:95` already flags this).**
- **Sim-path writes**: `src/sf33rd/Source/Game/system/sysdir.c:92-97,115-120,130-131,135-157,186-268` (set during `init_omop` via `get_system_direction_parameter` + `get_extra_option_parameter`). Also `src/sf33rd/Source/Game/effect/effe3.c:70-127` and `src/sf33rd/Source/Game/effect/effe4.c:43-89` — but both gated by `Is_Training_Mode(Mode_Type)` which is FALSE in MODE_NETWORK (confirmed at `effe3.c:23`, `effe4.c:23`).
- **Sim-path reads**: `src/sf33rd/Source/Game/engine/plcnt.c:1349,1368,1386` — `wk->spmv_ng_flag = omop_spmv_ng_table[wk->wu.id]` in `set_base_data`/`set_base_data_metamorphose`/`set_base_data_tiny`.
- **Filter 1 (rollback)**: `init_omop` is called ONCE at stage init, before gameplay; not re-called mid-round. But `set_base_data_metamorphose` IS called mid-round (Twelve transforms) and reads `omop_spmv_ng_table[dmid]` into PLW. If a peer's `omop_spmv_ng_table` was modified after `init_omop` — which does not happen in netplay mode since effe3/effe4 are gated out — it would propagate into hashed PLW. Confirmed that this cannot happen in netplay.
- **Outcome**: **not a live risk in MODE_NETWORK** (gated out). Left in list for completeness.

### Candidate R6: GekkoNet rollback-budget exhaustion [REVISED, DEMOTED 2026-04-24 rollback-axis]

**Rank: background hypothesis (was #1 under 32-bit axis).**

- **Demotion rationale under rollback-axis**: the mechanism is unfalsifiable from code (library source unavailable), doesn't DIRECTLY gate the BG-off path, and its "symmetric degradation" is a property we assumed without evidence. More importantly, if GekkoNet were committing speculative frames, we'd see random-frame desyncs correlated with CPU spikes — but frame 1274 is reproducible-ish, suggesting a deterministic mechanism. Keep as a backup hypothesis.
- **File:Line**: prebuilt static library `third_party/GekkoNet/build/lib/libGekkoNet.a`; public headers at `third_party/GekkoNet/build/include/gekkonet.h`; client at `src/netplay/netplay.c:684-714`.
- **Fits filter 1 (rollback)**: yes by construction.
- **Fits filter 2 (symmetry)**: unverified — depends on library behavior under budget exhaustion.
- **Fits filter 3 (gates BG)**: no direct path.
- **Retained for live instrumentation**: `frame_max_rollback` counter at `src/netplay/netplay.c:80,713,729-738`. If DEBUG logs show both peers hitting budget-exceed around frame 1274, revisit.

---

## Ranked candidate causes (2026-04-24, 32-bit axis) [SUPERSEDED 2026-04-24 rollback-axis]

> **[SUPERSEDED 2026-04-24 rollback-axis]** — the ranking below was produced under the 32-bit-correlation framing. It has been superseded by the rollback-axis ranking above. The individual candidate mechanisms (esp. `chainex_check`) are still relevant and are re-ranked under the new framing.

**Primary filter: symmetric-vs-asymmetric axis.** The corrected evidence (see "Evidence refinement") says BOTH peers show black BG whenever at least one 32-bit peer is in the session. A candidate must therefore explain (or at minimum be compatible with):

- **(S1) Symmetric black BG** on both peers whenever a 32-bit peer is present, AND
- **(S2) Checksum mismatch at some frame** when rollbacks are actually occurring (i.e. not on Mac↔Mac localhost zero-RTT).

A candidate that predicts only one peer goes black (asymmetric) is ruled out as a standalone explanation for the BG symptom. A candidate that predicts both peers match but go black symmetrically does not by itself explain the desync and needs a partner mechanism.

### Candidate 6 (NEW TOP): GekkoNet rollback-budget exhaustion under 32-bit CPU pressure

**Rank: #1**. Best fit to the "both peers go black AND desync only when 32-bit peer is present" correlation, albeit with caveats that require live instrumentation to fully verify.

- **File:Line**: Library `third_party/GekkoNet/build/lib/libGekkoNet.a` (prebuilt static library). Public API headers at `third_party/GekkoNet/build/include/gekkonet.h`. Client code at `src/netplay/netplay.c:684-714` (`process_events`). `frame_max_rollback` stats at `src/netplay/netplay.c:80,713,729-738`.
- **Mechanism**: On 800 MHz MiSTer with `input_prediction_window = 10` at the time of the crash (per Evidence §4), a burst of rollbacks near frame 1274 takes longer to resim than the 16.67ms frame budget. If GekkoNet falls back to committing a speculative frame when the budget is exceeded — either internally or by way of its state machine timing out — both peers' rollback machinery temporarily fails to converge. Mac↔Mac localhost has effectively zero rollbacks (0 RTT) so this path never triggers. Mac↔MiSTer and MiSTer↔MiSTer both have real rollbacks; MiSTer hardware also has reduced CPU headroom.
- **Why it fits (S1 symmetric BG)**: under budget exhaustion, both peers enter the same "degraded" rollback-processing mode at roughly the same wall-clock time. If that mode mis-sequences the restore/resim/save events, effect-77 or the BG state transitions in `effect_77_move` / `effect_K8_move` and their BG-off calls (`Bg_Off_R` at `effect/eff77.c:71,92`; `Bg_Disp_Switch` at `effect/effk8.c:27,38,52`, all inside `effect_K8_move`) may execute out-of-order or twice, leaving `Screen_Switch_Buffer` zero or `bg_disp_off` set on BOTH peers. Same-direction degradation = symmetric black BG.
- **Why it fits (S2 desync)**: budget exhaustion is not perfectly synchronized between peers (network jitter, CPU scheduling). Even if the degradation mode is the same on both peers, the precise sequence of Load/Advance/Save events processed differs by one or more frames. Different event orderings → different final State → different checksums.
- **Why MiSTer↔MiSTer specifically**: stock 800MHz ARM is strictly slower than any 64-bit x86/arm64 Mac CPU. Two MiSTers both hit the budget ceiling; Mac↔Mac stays below it.
- **Unverified**: the library source is not available in-tree, so "GekkoNet's behavior under budget exhaustion" is hypothesis-only. Could be disambiguated by upstream source inspection or targeted experiments (`input_prediction_window` = 3 to reduce rollback cost; monitor `frame_max_rollback` at `netplay.c:80,713-734`).
- **Partial weakness**: does not uniquely identify WHICH Advance/Save ordering is corrupted. Needs DEBUG ring-buffer dump to confirm which field diverged first.
- **Symmetric/asymmetric**: matches "symmetric BG + asymmetric checksum" exactly. Highest-rated candidate under the new constraint.

### Candidate 0a [REVISED 2026-04-24]: `chainex_check[2][36]` is not rollback-restored

**Rank: #2** (previously #1).

- **File:Line**: `src/sf33rd/Source/Game/system/sysdir.c:12` (decl); `engine/pls03.c:122,169,194,241,283,330,355,402,521,592,640,711,845,924,943,1023` (reads + writes); `engine/plcnt.c:1414-1420` (`clear_chainex_check`); `engine/pls00.c:795,805,855,897,963,980,990,1040,1082,1151`, `engine/plpdm.c:195`, `engine/plpnm.c:87` (clear sites). NOT in `GS_SAVE` **at the time of writing only** — now a `GameState` member, see the STATUS block at the top.
- **Mechanism**: see §D.6.4. Speculative sim sets `chainex_check[id][ix]` to 1; rollback load does not restore; resim inherits stale bits from the speculative pass.
- **Symmetry analysis** (new, requested by caller):
  - Scenario 2 (both peers roll back same depth K): both set identical speculative bits → both stale-identical after load → **symmetric**. Produces matching (wrong) hashes → **no desync** but potentially produces "both go black" if the downstream PLW state ends up in an `effect_77`/`effect_K8` trigger path deterministically.
  - Scenario 3 (peers roll back different depths): residue differs → **asymmetric** → desync.
  - Scenario 1 (no rollback, Mac↔Mac): no speculative residue → no divergence. Clean run. Consistent with observed Mac↔Mac baseline.
- **Fit to corrected evidence**: chainex_check ALONE can produce either symmetric-BG-no-desync (Scenario 2) OR asymmetric-state-with-desync (Scenario 3) but not both in the same run. The observed symptom is BOTH. Chainex_check therefore is either:
  - (a) a partner mechanism that produces the BG symmetry (Scenario 2 pathway) while something else — likely GekkoNet budget exhaustion (Candidate 6) — produces the checksum mismatch, OR
  - (b) rank-downgraded from standalone #1 to supporting #2 under the new constraint.
- **Why rank dropped**: previously rated #1 because it cleanly explained the desync. Under the new "both peers black" constraint, it no longer cleanly explains the BG part.
- **Why still #2**: still the only identified file-static-that-escapes-rollback pattern in the live sim path (spmv_ng_save is dead code; Candidate 0b). Still the highest-risk individual saved-state gap we can name with file:line evidence. The symmetric-BG / asymmetric-desync dual pattern above is still plausible with chainex_check as the desync-side contributor.
- **Smoking-gun test (unchanged)**: add `chainex_check` to the focused hash at `src/netplay/game_state.c:1728`. If desync STILL fires with it hashed, chainex_check is not the divergence root. If desync STOPS firing, it was.

### Candidate 1 [REVISED 2026-04-24]: `bg_disp_off` / `Screen_Switch_Buffer` divergence driven by an upstream divergence, BG toggle on only one peer

**Rank: downgraded — mostly ruled out as standalone explanation** (previously mid-rank).

- **File:Line**: `stage/bg.c:1507-1509` (`Bg_Disp_Switch`); `sys_sub.c:899-930` (`BG_Draw_System`); `effect/effk8.c:27,38,52` (only in-battle caller of `Bg_Disp_Switch`); `effect/eff77.c:71,92` (sim-side `Bg_Off_R` callers).
- **Symmetry**: this candidate explicitly assumed ONE peer enters `effect_K8_move`/`eff77` while the OTHER does not → one peer black, other not. **Directly contradicts the corrected evidence "both peers go black."** As an asymmetric-BG candidate, **ruled out**.
- **Residual plausibility**: if BOTH peers enter the BG-off path at the same frame but with slightly different sub-timer state (e.g., different `ewk->wu.routine_no[0]` progression), they could both go black but for different durations. Still asymmetric in duration, not ruled out for a 1-frame window.
- **Net**: prior Candidate 1 as stated is the wrong model. Demoted to incidental — black BG is now modeled as symmetric (Candidate 6 mechanism).

### Candidate 4 [REVISED 2026-04-24]: Effect pool divergence

**Rank: low** (previously low).

- **File:Line**: `src/sf33rd/Source/Game/effect/effect.c:29` (`uintptr_t frw[EFFECT_MAX][448]`); EffectState at `src/netplay/game_state.h:19-27`; saved via `gather_state` at `src/netplay/game_state.c:1487-1500`; NOT hashed (effect-pool section checksum is hardcoded to 0 at `src/netplay/game_state.c:1741`).
- **Symmetry**: effect pool is fully saved/restored on rollback. Both peers restore the same pool on load_state. Divergence only comes from a PLW mutation that isn't perfectly sanitized — if both peers go through the same mutation deterministically, both diverge the same way = symmetric. If only one diverges = asymmetric.
- **Fit**: symmetric effect-pool drift would match "both go black" (both spawn/kill the same BG-affecting effect) but would NOT by itself trigger a desync (same effect state on both). Asymmetric drift would trigger desync but not "both go black."
- **As a partner mechanism**: could pair with Candidate 6 similar to Candidate 0a. Rank: low — no specific case identified, and the effect pool is fully restored on rollback.

### Candidate 0b: `spmv_ng_save[2]` rollback-unsafe but dead code

**Rank: none** (dead code, unchanged from prior report). **[CORRECTION 2026-08-23: THIS EXONERATION WAS WRONG — the code is LIVE and the symbol has been added to GameState. See below.]**

- ~~Reconfirmed: `plpat17.c` is MAKOTO attack code, so the `wk->player_number != 16` gate at `effl8.c:111-113` never hits CHUNLI → L8 effect never fires in live play. Left in the list as demonstration that the "static-save-across-rollback" pattern exists in the codebase.~~
- **CORRECTION (2026-08-23)**: the reasoning above used CPS3 character numbering (CHUNLI=16, MAKOTO=17, `constants.h:29-30`), but `CPS3` is NOT defined in this build (`CMakeLists.txt` "Feature toggles" block leaves `# CPS3` commented out), so the active enum is `constants.h:54-55`: CHUNLI=15, **MAKOTO=16**. The `wk->player_number != 16 → return 0` gate at `effl8.c:111` therefore passes for exactly the character `plpat17.c` serves. Verified dispatch chain: `plpat.c:95` `plxx_extra_attack_table[wk->player_number]` → table entry index 16 = `pl17_extra_attack` (`plpat.c:776-781`) → `pl17_exatt_table[routine_no[2]-16]` entry 3 = `Att_PL17_AT2` (`plpat17.c:26,311-316`) → `effect_L8_init(wk)` at `plpat17.c:245` on `cg_type == 10`. The L8 effect fires in live Makoto play; its `spmv_ng_save` latch/restore (`effl8.c:50,63`) writes back into the checksummed `PLW.spmv_ng_flag`, so a rollback straddling the latch/restore window desyncs — same class as `chainex_check`/`ca_check_flag`/`Color7`. **Fixed 2026-08-23: `spmv_ng_save[2]` added to GameState save/load, the desync checksum, and the FH_* per-field triage table (`src/netplay/game_state.c`), EXPECTED_GAME_STATE_SIZE re-pinned 17676 → 17684.** Other rows in this document that describe `spmv_ng_save` as "dead code" (the §uncovered-globals table row, Candidate ranking notes, and appendix A.2 lists) predate this correction and should be read against this note.

### Candidate 2: Unhashed-but-saved BG state accumulating divergence

**Rank: low** (unchanged). No path from `Screen_Switch`/`bg_disp_off` into hashed fields. Would explain BG but not desync.

### Candidate 3: `scr_sc` / floating-point divergence

**Rank: rejected** (unchanged). `scr_sc` is not hashed and not read by sim.

### Candidate 5: `task[].func_adrs` pointer poisoning

**Rank: rejected** (unchanged). `task[]` not hashed.

### Candidate 7: Unknown unhashed mutable global

**Rank: background risk, not disambiguated.** Re-audited under D.6.3 — no newly identified high-risk item beyond those already listed.

### Summary of the new top-candidate ranking

| Rank | Candidate | Fits symmetric BG? | Fits asymmetric desync? | File:Line |
|---|---|---|---|---|
| 1 | GekkoNet rollback-budget exhaustion (C6) | Yes (degraded mode fires on both peers simultaneously) | Yes (event-ordering jitter between peers) | `third_party/GekkoNet/build/lib/libGekkoNet.a` (source unavailable); client at `src/netplay/netplay.c:1895-1965` (`process_events`) |
| 2 | `chainex_check` rollback-unsafe (C0a) | Yes if Scenario 2 | Yes if Scenario 3 | `sysdir.c:12`; `pls03.c:80,129`; `plcnt.c:1438` (`clear_chainex_check`) |
| 3 | Effect pool divergence (C4) | Symmetric if both diverge identically | Asymmetric — low likelihood | `effect.c:29`; `game_state.h:19-27`; `game_state.c:1741` |
| — | BG toggle asymmetry (C1) | **No** — ruled out by corrected evidence | Yes | `effk8.c:27`; `sys_sub.c:922` (`BG_Draw_System`) |
| — | Unhashed BG state accumulation (C2) | Yes | No | `sys_sub.c:922` (`BG_Draw_System`), `sys_sub.c:943` (the `bg_disp_off` / `Screen_Switch_Buffer` layer test) |
| — | `spmv_ng_save` (C0b) | N/A — dead code | N/A | `effl8.c:13` |
| — | `scr_sc` FP (C3) | N/A | No | render-side |
| — | `task[]` pointer (C5) | N/A | No | not hashed |

**No single candidate dominates** under the new constraint. Candidate 6 is the best fit but requires GekkoNet source or live instrumentation to verify. Candidate 0a is the best code-visible candidate but needs both Scenarios 2 and 3 to occur together to fully explain the observation.

**Disambiguating experiments**:

1. Reduce `input_prediction_window` from 10 → 3 (config key `CFG_KEY_NETPLAY_INPUT_PREDICTION_WINDOW`). If the desync frame-number increases or the desync stops firing, budget exhaustion (C6) is implicated.
2. Hash `chainex_check` (add `djb2_update_mem` call at `src/netplay/game_state.c:1742`). If desync fires earlier, C0a is implicated.
3. Add effect-pool checksum (populate the currently-zero `sc.effects` at `src/netplay/game_state.c:1741`). If the effects section diverges before the combined hash, C4 is implicated.
4. Inspect upstream GekkoNet source at the Team Rerostar / originally-sourced Git repo. Verify rollback-budget behavior.
5. Run DEBUG build on two MiSTers — `dump_desync_state` at `src/netplay/game_state.c:1778` will write `states/desync_F1274_plw0.bin` / `_plw1.bin` and the ring buffer. Binary-diff the PLW dumps to identify the first divergent byte.

## Reproduction / elimination experiments

Each hypothesis has a minimal experiment to confirm/rule out. **No fixes proposed here.**

### For Candidate 0a (`chainex_check` rollback-unsafe)

1. **Quickest confirmation** (no live session): add a DEBUG log of `chainex_check[0][*]`, `chainex_check[1][*]` contents once per second during a local 2-peer test. If they ever become non-zero while PLW state is synced, there's a write path we need to rollback-restore.
2. **Smoking-gun test**: in DEBUG build, add `chainex_check` to the focused checksum via `h = djb2_update_mem(h, (const uint8_t*)chainex_check, sizeof(chainex_check));` at `src/netplay/game_state.c:1728`. Repro the desync. If the desync now fires EARLIER than frame 1274, `chainex_check` is the root. If it fires at the SAME frame or later, it's not.
3. **Minimal fix sketch (for elimination experiment ONLY — not part of this report's recommendations)**: adding `GS_SAVE(chainex_check)` / `GS_LOAD(chainex_check)` to game_state.c at the corresponding bg_data section. Also requires adding `extern u8 chainex_check[2][36]` to game_state.h and appending to the GameState struct. After the change, re-run MiSTer↔MiSTer and check whether the frame-1274 desync disappears.

### For Candidate 1 (hashed-state divergence triggering asymmetric BG toggle) [DEPRECATED 2026-04-24 — symmetric BG rules out this mechanism as standalone]

Retained for completeness; under the corrected evidence the BG is symmetric so these experiments primarily serve to rule OUT Candidate 1, not confirm it.

1. **Dump the ring buffer from the DEBUG build** when the next desync fires. `dump_desync_state` at `src/netplay/game_state.c:1790-1845` writes `states/desync_F<N>.txt` with 20 frames of per-subsystem checksums, plus `states/desync_F<N>_plw0.bin`, `desync_F<N>_plw1.bin`, `desync_F<N>_state.bin`. Comparing the two peers' `.bin` files will pinpoint WHICH bytes diverged — and therefore which PLW field or RNG or global is the root corruption source.
2. **Add a log of `bg_disp_off`, `Screen_Switch`, `Screen_Switch_Buffer`, `seraph_flag` once per second** (every 60 frames) in the DEBUG build. Confirms whether the black BG is coincident with a `Bg_Disp_Switch(1)` call AND whether BOTH peers' sim-side state is equal at that moment (expected per corrected evidence).
3. **Check if Gill was in play at the moment of the desync** — user-facing info. Under the new model, Gill presence is less relevant since the BG symptom is symmetric.

### For Candidate 2 (unhashed BG state)

1. **Temporarily add `bg_disp_off`, `Screen_Switch`, `Screen_Switch_Buffer` to the focused checksum** via `h = djb2_update_mem(h, (const uint8_t*)&gs->bg_disp_off, sizeof(gs->bg_disp_off));` etc. at `src/netplay/game_state.c:1728`. If the desync fires EARLIER than frame 1274, BG-state divergence is the upstream trigger.
2. Alternatively — log the raw bytes of Screen_Switch/bg_disp_off once per second. Compare between peers after desync fires.

### For Candidate 4 (effect pool divergence)

1. Add effect-pool checksum to the focused hash: hash `frwctr`, `frwctr_min`, `head_ix[]`, `tail_ix[]`, `exec_tm[]` (not `frw[]` — too big and has render fields). Populate the currently-zero `sc.effects` at `game_state.c:1741`.
2. If the effects checksum diverges BEFORE the combined checksum does, effect pool is the root.

### For Candidate 6 (rollback budget) [RENAMED 2026-04-24 — new top candidate; prior report used "Candidate 5" for this but renumbered]

1. Record the `frame_max_rollback` value at `src/netplay/netplay.c:713,729-738` per second in DEBUG logs during the match. Also log per-event rollback depth via `event->data.adv.rolling_back` at `src/netplay/netplay.c:698`.
2. If rollback-count correlates with the desync timing (e.g., both peers saw 8+ frame rollbacks right before 1274), budget exhaustion is suspect.
3. Alternative test: reduce prediction window to 3-4 via `CFG_KEY_NETPLAY_INPUT_PREDICTION_WINDOW` and see if the desync frame-count increases proportionally to the lower budget (or if desync stops firing entirely).
4. **New instrumentation request**: compare rollback DEPTH (`frames_rolled_back` accumulator at `netplay.c:700`) between peers. If the two peers roll back different depths per session event, Scenario 3 of Candidate 0a applies and chainex_check asymmetry is implicated. If depths match but desync still fires, the mismatch is in GekkoNet internal state sequencing.
5. **Unavoidable step**: obtain upstream GekkoNet source to confirm its budget-exhaustion behavior. Prebuilt `.a` cannot answer this.

### For Candidate 7 (unhashed global)

1. Lift the hash coverage. Replace the focused-hash function at `save_current_state` with a "hash the ENTIRE State struct (minus PLW pointers) wholesale" — 241 KB instead of ~10 KB. This would detect ANY divergence. Measure overhead (djb2 is ~1 GB/s so should still be sub-millisecond).
2. If the desync still fires at 1274 with the maximum-coverage hash, the divergence is in a field that IS currently hashed — in which case the DEBUG dump's PLW binary diff is the next diagnostic.

## Unverified questions

Things the code couldn't answer — need the live DEBUG dump, or upstream GekkoNet source, or user-provided match metadata.

1. **Does GekkoNet commit a speculative frame when rollback exceeds CPU budget?** Cannot answer from headers alone. Need `third_party/GekkoNet/*.cpp` or the upstream Git repo.
2. **Was Gill in play?** The user's match metadata at frame 1274. Impacts whether `effect_K8_move` is the BG toggle.
3. **What stage was loaded?** `VS_Stage` IS hashed (`game_state.c:1717`), so peers agree. But specific stages (Yang 3 Yun's stage, Gill's temple, Gouki's secret stage) have more complex BG effects. Stage 3 specifically has `rw_gbix[13]` cycling and `stage03_flash_tbl` flashing at `stage/bg.c:93-108`.
4. **Is `input_prediction_window` = 10 the fault by itself?** Mac↔Mac with same window ran 3000+ frames clean. Suggests the window is a contributing factor (CPU pressure) but not the root.
5. **Is the "black background" purely a render-time visual, or is the sim-side `bg_disp_off` actually 1?** Need a DEBUG log at the `BG_Draw_System` entry point.
6. **Is `sa_bg_cache_surface_valid` logged along with the black frame?** If the sa_bg_cache was invalidated and a re-cache was deferred, the blit might fall back to a black surface.
7. **Any non-pointer field alignment/padding discrepancy in the PLW struct that GCC doesn't zero?** Would need to run `offsetof` for every PLW field and compare with a hex-dump of a sanitized plw_scratch on both peers — requires DEBUG dump.

---

## Summary [REVISED 2026-04-24 rollback-axis]

The primary variable is **whether Gekko's speculative advance actually rolls back during gameplay** — not architecture, not word-width, not PIE. Mac↔Mac localhost runs clean because the speculative-advance path is effectively unexercised at 0 ms RTT. Any session with real RTT exercises it. If any sim-path global escapes `GS_SAVE` / `GS_LOAD`, rollback contamination becomes possible and both peers converge on the same stale state symmetrically, because both peers roll back through the same moments.

- **Top actionable candidate is Candidate R2 (`chainex_check` rollback-unsafe)**, rank #1. Decl at `src/sf33rd/Source/Game/system/sysdir.c:12`; write at `src/sf33rd/Source/Game/engine/pls03.c:129` (and eight other pls03 sites); read at `src/sf33rd/Source/Game/engine/pls03.c:80` (and seven other pls03 gates). Not in `GS_SAVE` **when this was written; saved since 2026-04-24, and inert in netplay since `4485a438` — see the STATUS block at the top.** Under the rollback-axis framing, the speculative-write symmetry + stale-read-after-rollback mechanism produces the symmetric "both peers go black" pattern (indirect, via PLW/SA-routine divergence downstream of the gate) AND the asymmetric checksum mismatch (when rollback depths differ between peers).
- **Secondary candidates**: R3 (`ca_check_flag`, `engine/hitcheck.c:39`), R4 (`grdb`/`grdb2` via Twelve metamorphose, `engine/hitcheck.c:33-34`), R5 (`omop_spmv_ng_table` gated out in MODE_NETWORK). R6 (GekkoNet rollback-budget exhaustion) DEMOTED from prior #1 because it doesn't gate a BG-off path directly and is unfalsifiable from code.
- **Bg_Disp_Switch exit audit (§F)** finds NO unsaved flag directly gating a BG-off exit. Every `Bg_Disp_Switch(1)` call in `effect_K8_move` has a matching `Bg_Disp_Switch(0)` in the same function, gated by `GS_SAVE`-covered or EffectState-covered state. Every `Bg_Off_R(mask)` in sim effects has a matching `Bg_On_R(mask)` in the same effect. The BG-black symptom therefore must be a DOWNSTREAM effect of a PLW or routine-state divergence that shifts WHEN or WHETHER those paired exits fire — not a missing exit flag.
- **Elimination experiment (§G)**: add `h = djb2_update_mem(h, (const uint8_t*)chainex_check, sizeof(chainex_check));` at `src/netplay/game_state.c:1742`, plus `extern u8 chainex_check[2][36];` near the top. If next MiSTer↔MiSTer repro desync fires EARLIER than frame 1274, chainex_check is implicated. If it fires at the same frame, chainex_check is one of multiple contributors. If it fires later or not at all, chainex_check is eliminated.
- **Checksum coverage** (unchanged): parity with 3sxtra plus combo_type/remake_power (`src/netplay/game_state.c:1670-1728`). State coverage: 604 saved fields; 258 uncovered, ~10 have sim-side footprint. All re-audited against the rollback-axis filter.

**New top candidate, one-line**: **`chainex_check[2][36]`** (decl `src/sf33rd/Source/Game/system/sysdir.c:12`; speculative-write in the EX-SA chain-success branch of `check_full_gauge_attack` (`src/sf33rd/Source/Game/engine/pls03.c:129`); gating read at the head of the same branch (`pls03.c:80`) that suppresses further PLW mutation when set) — fires during speculative rollback (filter 1), sets identical bits on both peers (filter 2, because the PLW state driving the write is itself hashed), and indirectly drives the BG-black symptom via downstream SA-routine divergence when the speculative-set bit lingers past the rollback and inhibits the next real chain-ex attempt (filter 3).

**Why the prior #1 (Candidate 6 GekkoNet budget exhaustion) was demoted**: it's a library-internal mechanism we cannot verify from code (library source not in tree); it doesn't directly gate the BG-off path; its "symmetric degradation" was an unsupported assumption. It remains a backup hypothesis, testable by reducing `input_prediction_window` to 3 and monitoring `frame_max_rollback` at `src/netplay/netplay.c:729-738`.

---

## Appendix A — Complete list of 258 mutable globals NOT in the `GS_SAVE` set

Generated by:
```sh
# Extract non-const non-static globals from src/sf33rd/Source/Game/
grep -rEn '^(u8|s8|u16|s16|u32|s32|u64|s64|int|char|bool|f32|f64|float|double|struct|[A-Z][A-Za-z_0-9]+)\s+[a-zA-Z_][a-zA-Z_0-9]*(\[|\s*=|;)' src/sf33rd/Source/Game/ --include='*.c' | \
    grep -v 'const\|static\|return' | awk -F: '{print $NF}' | \
    awk '{for(i=1;i<=NF;i++) if ($i ~ /^[a-zA-Z_][a-zA-Z_0-9]*[\[\;]/) {gsub(/[\[\;=].*/,"",$i); print $i;}}' | sort -u > /tmp/globals_names.txt

# Extract the saved set from netplay/game_state.c's GS_SAVE macros
grep -oE 'GS_SAVE\([a-zA-Z_][a-zA-Z_0-9]*\)' src/netplay/game_state.c | \
    sed 's/GS_SAVE(//;s/)//' | sort -u > /tmp/saved_names.txt

# Set difference: names in globals but not in saved
comm -23 /tmp/globals_names.txt /tmp/saved_names.txt
```

All 258 names, grouped and annotated in A.2.1-A.2.7 above. Full flat list:

```
adx_EmSel                     adx_stm_work                  adx_VS
bdl_index                     bg_priority                   bgm_exdataAC
bgm_exdataDC                  bgm_exe                       bgm_fade
bgm_fade_ix                   bgm_half_down                 bgm_level
bgm_req                       bgm_seamless_always           bgm_selectorAC
bgm_selectorDC                bgm_tableAC                   bgm_tableDC
bgm_vol_mix                   bgm_vol_now                   bgpoly
ca_check_flag                 Candidate_Buff                chainex_check
char_init_data                check_screen_L                check_screen_S
check_time_L                  check_time_S                  chk77_flag
cmd_id                        cmtx                          col3rd_w
Color7                        ColorRAM                      colPalBuffDC
Com_Vital_Unit_Data           CONTINUE_X                    control_pl_rno
control_player                curr_bright                   current_bgm
Debug_ID                      Debug_Index                   debug_menu_active
Debug_Pause                   Debug_w                       Decide_Stage
Deley_Debug_No                Deley_Debug_No2               Deley_Debug_Timer
Deley_Debug_Timer2            Disp_Bonus_Contents           Disp_Size_H
Disp_Size_V                   dmwk_kage                     dmwk_moji
e_line_step                   eff_hit_data                  eff48_adrs_tbl
effa6_pos_x_1p                effa6_pos_x_2p                effa6_pos_y_1p
effa6_pos_y_2p                effa6_pos_z_1p                efff9_message
efff9_PL_NO                   efff9_suicide                 efff9_txt_point
end_0_1_time                  end_5_flag                    end_etc_flag
end_fade_flag                 end_fade_timer                end_name_cut
end_no_cut                    END_OF_95                     end_staff_flag
ending_all_end                exec_tm                       Extra_Counter
fade_prio                     fd_dat                        frwctr
frwctr_min                    frwque                        GAME_OVER_X
gill_quake_flag               gill_quake_flag2              grdb
grdb2                         gSeqStatus                    hc3alpha
head_ix                       hi_meta                       hnc_col
hnc_end_timer                 hnc_timer                     hpq_in
hs                            Interface_Type                Interrupt_Timer
io_w                          ioconv_table                  keep_mes_no
ldreq_break                   ldreq_result                  letter_counter
letter_stack                  Lv                            lvr_chk_tbl
MANAGE_X                      Message_Data                  metamor_original
mkm_wk                        mmes_already                  mts
mts_ob_curr_stage             mts_ok                        music_scene
music_time                    n_disp_flag                   Name_00
Name_Input_f                  name_limit_timer              name_timer
name_wk                       naming_cnt                    ne_col
ne_flash_flag                 ne_timer                      njdp2d_w
No_Trans                      OK_Appear79                   omop_b_block_ix
omop_cockpit                  omop_dokidoki                 omop_guard_distance_ix
omop_otedama_ix               omop_r_block_ix               omop_round_timer
omop_sa_bar_disp              omop_sa_gauge_ix              omop_sag_len_ix
omop_sag_max_ix               omop_st_bar_disp              omop_stun_gauge_add
omop_stun_gauge_len           omop_stun_gauge_rcv           omop_use_ex_gauge_ix
omop_vital_init               omop_vital_ix                 omop_vt_bar_disp
op_104_sound                  op_bg_mvxy                    op_demo_index
op_end_flag                   op_obj_disp                   op_plmove_timer
op_scrn_end                   op_sound_status               op_timer0
op_w                          ot_all_of                     ot_cgf
ot_int                        ot_mot                        ot_mot_of
ot_pat                        ot_pat_of                     ot_pulpara
ot_pulreq                     ot_pulreq_xx                  p1sw_0
p1sw_1                        p1sw_buff                     p2sw_0
p2sw_1                        p2sw_buff                     p3sw_0
p3sw_1                        p3sw_buff                     p4sw_0
p4sw_1                        p4sw_buff                     palFormConv
palFormRam                    palFormSrc                    PASSIVE_X
PAUSE_X                       permission_player             picon_level
picon_no                      Play_Type_1st                 plt_req
ppwork                        Present_Data                  PrioBase
PrioBaseOriginal              pul                           pulpara
pulpul_scene                  pulreq                        q_ldreq
r_no_plus                     rank_name_w                   Ranking_Data
rckey_work                    rckeyctr                      rckeymin
rckeyque                      Rec_Time                      Record_Timer
Reset_Status                  RESET_X                       Rnd
RND_95                        roll_rate                     roll_rate_t
roll_rate_t2                  roll_rate2                    roll_stop
sa_frame                      sc_name_wk                    scrDrawPos
scrscrntex                    sdeb                          se_level
SEL_CPU_X                     SEL_PL_X                      seqs_w
Slow_Timer                    spmv_ng_save                  staff_r_no
Start_X                       sw_work                       sysFF
sysSLOW                       system_dir                    tail_ix
Test_Cursor                   texgrplds                     time_check
time_check_ix                 title_tex_flag                TopHUDFacePriority
TopHUDPriority                TopHUDShadowPriority          TopHUDVitalPriority
tr_data                       Training                      Training_Auto_Start
Training_Menu_From_Pause      vib_req                       vib_sel
waza_type                     WIN_X                         wr5_index
```

### Appendix A note: false-positives in the set-difference

Three names may be false positives (the diff sees them as uncovered, but they ARE saved):
- `ci_col`, `ci_timer` — declared at `effect/eff56.c:14-15` but saved at `src/netplay/game_state.c:703-705`. Check shows GS_SAVE(ci_col) / GS_SAVE(ci_timer) both present; the tools/grep pattern above likely miscounted for these.
- A few `exec_tm`, `head_ix`, `tail_ix`, `frwctr`, `frwctr_min`, `frwque` — these ARE in EffectState (saved via `gather_state` at `game_state.c:1487-1501`), but the extraction tool only scans `GS_SAVE(...)` macros in GameState. They are correctly handled.

Names confirmed as truly uncovered (NOT in State, NOT in EffectState): `spmv_ng_save`, `chainex_check`, and the render/sound/debug/training/ending sets called out in A.2.2-A.2.7.

---

Single-line ranked guess [REVISED 2026-04-24 rollback-axis]:
1. **`chainex_check[2][36]` rollback-unsafe (Candidate R2)** — `src/sf33rd/Source/Game/system/sysdir.c:12` (decl); speculative-write at `src/sf33rd/Source/Game/engine/pls03.c:129` (and 8 other pls03 sites); gating read at `src/sf33rd/Source/Game/engine/pls03.c:80` (and 7 other gates). Not in `GS_SAVE` **when this was written; saved since 2026-04-24, and inert in netplay since `4485a438` — see the STATUS block at the top.** Passes all three rollback-axis filters (fires during rollback, symmetric contamination, indirectly gates BG-off via downstream SA-routine divergence when speculative-set bits linger past rollback).
2. **`ca_check_flag` (Candidate R3)** — `src/sf33rd/Source/Game/engine/hitcheck.c:38`. Gates `catch_hit_check()` at `hitcheck.c:63` which writes to PLW. Set during speculative PLW state transitions (`engine/plmain.c:1187`); stale after rollback.
3. **`grdb[2][2][2]` / `grdb2[2][2]` via Twelve metamorphose (Candidate R4)** — `engine/hitcheck.c:33-34`. Only diverges when `set_base_data_metamorphose` is speculated but not confirmed.

Previously ranked #1 but now demoted under rollback-axis evidence: **Candidate 6 (GekkoNet rollback-budget exhaustion)** — moved to background hypothesis because (a) library source unavailable to verify, (b) doesn't directly gate a BG-off path, (c) "symmetric degradation" is an unsupported assumption. Retained as fallback, testable via `input_prediction_window` reduction.

**Single most-likely pick** (per the caller's request):
- **Subsystem**: EX-SA chain-ex gating via a rollback-unsafe file-level static.
- **Declaration file:line**: `src/sf33rd/Source/Game/system/sysdir.c:12` (`u8 chainex_check[2][36]`).
- **Sim-path write file:line**: `src/sf33rd/Source/Game/engine/pls03.c:129` (speculative set during a successful EX-SA chain fire inside `check_full_gauge_attack`; also at `pls03.c:205` (`check_full_gauge_attack`), `:298,374` (`check_full_gauge_attack2`), `:495,571` (`check_super_arts_attack_dc`), `:776,879,900` (`check_special_attack`)).
- **Why it fits all three rollback-axis filters**:
  - Filter 1 (fires during rollback): Yes. The speculative pass inside Gekko's advance-window writes `chainex_check[id][gix] = 1` on an SA-chain success. Rollback's `load_state` does not restore this because it's not in `GameState`. Resim reads the stale 1 and inhibits the next legitimate chain attempt.
  - Filter 2 (symmetric contamination): Yes. Both peers speculate through the same hashed PLW state (routine_no, pat_status, sa-gauge-index all in PLW), so both set identical chainex_check bits. After load_state neither peer has them cleared. Both resim with identical stale bits.
  - Filter 3 (gates BG-off exit path): Indirectly. Stuck chainex_check silently blocks a chain-ex attempt → PLW takes a different SA routine (different `wk->as`, `wk->sa->mp`, `wk->permited_koa` path) → different effect_77/effect_K8 spawn timing → `Bg_Off_R` / `Bg_Disp_Switch(1)` fire on both peers but at a timing that diverges from the exit-clear sequence, leaving both peers' `Screen_Switch_Buffer`/`bg_disp_off` stuck in the "dark" state. Hashed-PLW divergence between peers at this same moment is driven by rollback-depth asymmetry in the speculative-vs-confirmed chain-ex timing, flipping a byte in `plw_scratch` by frame 1274.

Caveats:
- The "indirect BG-off path" in Filter 3 is structural, not observationally confirmed. The §F.5 audit finds no direct unsaved BG-exit gate, which means the mechanism must work downstream via PLW-routine divergence. This is the best-fit available and the proposed §G.1 experiment will confirm or eliminate it with one line of code.
- If the §G.1 detection experiment shows the desync frame does NOT shift when `chainex_check` is hashed, move to Candidate R3 (`ca_check_flag`) next.
- If neither R2 nor R3 shifts the frame, fall back to Candidate R6 (GekkoNet budget) with the `input_prediction_window = 3` experiment.
