# Save States & Instant Mode Jump — Research & Design

**Origin:** this document started life as the Desktop research doc
`~/Desktop/3s-arm/docs/3sx-savestates-and-instant-mode-jump-2026-08-29.md`
(written 2026-08-29 against `upstream-engine-fixes` @ `1f981b73`). It was
moved into the repo and re-validated on `lane/training-savestates` @
`762b5052`, and its #1 open question was settled with a prototype
(`src/test/scene_jump_spike.c`, findings SJ-15..SJ-21). Its two sibling docs
(`3sx-training-mode-fixes-2026-08-29.md` = `TM-nn`,
`3sx-makoto-1f-link-2026-08-29.md` = `ML-nn`) **remain outside the repo**, in
that same Desktop directory.

**Status:** research + one DEBUG-gated prototype. Nothing shipped.

---

## READ THIS FIRST — before touching the repo

1. Read `AGENTS.md` at the repo root. Then `docs/mister-runbook.md` before any
   build, package, deploy, or probe work. For anything touching the rollback
   state set, also read `docs/rollback-determinism-harness.md`.
2. **Canonical build:** `tools/mister/build-game.sh --flavor telemetry`.
3. **Device work** goes through `tools/mister/misterctl.sh`. Owned targets are
   only `/media/fat/MiSTer_3S-ARM`, `/media/fat/_Other/3S-ARM.rbf`, and
   `/media/fat/games/3s-arm/`. **Never** `rsync --delete` at `/media/fat`.
4. **Never push to any remote** without an explicit instruction.
5. **Mandatory gate:** any change to `GameState` / the `GS_SAVE` set must be
   re-validated with `tools/rollback-determinism/run.sh`. Also update
   `EXPECTED_GAME_STATE_SIZE` — it now lives in **`src/netplay/game_state.h`**
   (moved there so `mist_handshake.c` can advertise the same `state_ver` on
   every architecture); the `_Static_assert` that enforces it is still in
   `game_state.c`, active on 32-bit builds only.
6. **Citation form in this doc:** durable anchors — `file` -> `symbol`, or
   the exact quoted text of a line. Resolve them by grepping the symbol. A
   bare line number appears only as a hint qualified with the commit it was
   read at (`:NNN @ 762b5052`) and must never be trusted on its own. This
   file is deliberately NOT in `tools/doc-citations/baselines.txt`; do not
   hand-repoint numbers here.

### The three documents

| Doc | IDs | Scope |
|---|---|---|
| `3sx-training-mode-fixes-2026-08-29.md` (Desktop, not in repo) | `TM-nn` | Training-mode bug audit. |
| `3sx-makoto-1f-link-2026-08-29.md` (Desktop, not in repo) | `ML-nn` | Makoto HP Hayate -> SA1 combo-counter investigation. |
| **This one** | `SJ-nn` | Save states, quick-training-mode, instant mode-jump design |

No findings overlap between them.

---

## Findings index

**Cite these IDs, not section numbers.** Sections renumber as the doc grows.

| ID | Finding | Where | Bearing |
|---|---|---|---|
| SJ-01 | A general-purpose snapshot API already exists and is not netplay-shaped | §2.1 | Enables save states |
| SJ-02 | Full snapshot already runs every frame at 60fps on device | §2.2 | Cost is a solved problem |
| SJ-03 | Snapshots are process-local (raw function + data pointers) | §2.4 | In-memory yes, on-disk needs relocation |
| SJ-04 | Same-scene training save state is viable | §3 | Green light |
| SJ-05 | Audio re-fire is the one real blocker for user save states | §3.2 | Must design |
| SJ-06 | The scene side-effect chain is fully enumerated (12 steps) | §4 | Makes instant jump buildable |
| SJ-07 | Netplay orchestration is already scene-independent | §5.2 | Attract-screen wait is free |
| SJ-08 | A connection-status overlay purpose-built for attract/title already exists | §5.3 | UI half already done |
| SJ-09 | `No_Trans` already suppresses all drawing and is already used this way | §5.4 | Black-cover is free |
| SJ-10 | Training settings + char select already persist to disk | §6.1 | Quick-training needs no save state |
| SJ-11 | A working menu-to-match driver exists but is `#if DEBUG` | §6.2 | Promote, don't rewrite |
| SJ-12 | OSD -> game live command channel exists (config rewrite + signal) | §7 | Trigger mechanism ready |
| SJ-13 | Replay/DUMMY RECORDING stores inputs only, no positional state | §6.3 | Not reusable as a save state |
| SJ-14 | Cross-scene save state restore is NOT viable | §2.5 | Rules out one approach |
| SJ-15 | **SETTLED: the chain runs correctly outside task dispatch** — proven by prototype | §4.1 | Open question #1 closed |
| SJ-16 | Step 7's "async gates" are synchronous + idempotent on this port | §4.2 | Only step 10 is multi-frame |
| SJ-17 | Gate cost measured: 24-25 frames stock, 1 frame barrier-forced (host) | §4.3 | Open question #2 closed on host |
| SJ-18 | §4's step 12 (`Game2_2`) is the Reset_Replay path, not the virgin path | §4.4 | Corrects the chain table |
| SJ-19 | `Load_Replay_Sub` is an in-tree menu->match jump that bypasses char select | §4.5 | The template the spike copied |
| SJ-20 | Hardcode census: 5 `Mode_Type = MODE_ARCADE` sites, 11 `Present_Mode` writes | §4.6 | Extends the Reset_Sub0 trap |
| SJ-21 | A training match starts ON its menu; the round blocks until it is dismissed | §4.7 | Any jump must handle it |
| SJ-22 | **The chain is SHIPPED** — promoted out of the spike into `src/scene_jump.c`, driving the OSD "Quick Training" feature | §10 | Design realized |
| SJ-23 | Quick Training reaches the same live match from ANY offline scene, not just the title — via the shipped soft-reset teardown | §10.2 | Edge cases closed |
| SJ-24 | Wrapping the jump in the diagonal wipe (type 1) is sequential with the `No_Trans` cover, never overlapping | §10.3 | Wipe/cover are mutually exclusive |
| SJ-25 | Quick Training reads/writes the SAME persisted training file; the OSD trigger is a live SIGRTMIN+5, no restart. **The original "no drift" half was WRONG** — see §10.4 | §10.4 | No new persistence; live channel |
| SJ-27 | The chain omitted TWO character-select steps: the training-config load and `init_omop()`. The match ran on zeroed settings, zeroed engine DIP tables, and flushed the zeros back over the user's file | §10.6 | A jump must replicate the exit's ENGINE work, not just its scene work |
| SJ-28 | The training-config save is a whole-HARNESS-CLASS hazard, not a Quick Training one: every `--test-enable` session that reaches `Setup_NTr_Data()` persists its own selection over the user's file | §10.7 | Suppressed in `TrainingConfig_Save()` for test sessions |
| SJ-26 | `WipeLimit` is SHARED with the engine's own transitions, and `WipeOut` increments it OUTSIDE its `!No_Trans` guard — a cover hides the drawing, never the counter | §10.5 | Quick Training must defer, not start, on an in-flight wipe |

## Revision log

| Date | Change |
|---|---|
| 2026-08-29 | Initial research. SJ-01..SJ-14. Design for instant mode jump (§5) settled on attract-wait + `No_Trans` cover + chain call. |
| 2026-08-29 | Added READ THIS FIRST preamble (build/device/safety + 3-doc map + rollback-harness gate). |
| 2026-08-30 | Corrected `sizeof(GameState)`: it drifted 17784 -> 17772 between `1f981b73` and `aa2c2bf1`. Read it from source. |
| 2026-08-30 | **Citation audit at `ad480322`.** All 116 `path:line` citations resolved; 10 of 67 checked symbol/range pairs had drifted, §4 systematically. Conclusion then: grep the symbol, don't trust the line. |
| 2026-09-02 | **Moved into the repo** (this file) and re-validated at `762b5052`. Load-bearing citations converted to durable `file -> symbol` anchors — the fix for the drift problem the audit found; the old `path:line` audit table is superseded by the conversion and removed. New findings SJ-15..SJ-21 from the `--test-instant-jump` prototype (built this revision, same commit): §9 open questions #1 and #2 settled. Stale facts fixed: `EXPECTED_GAME_STATE_SIZE` moved to `game_state.h` (17772 @ `762b5052`); `netplay_nav.c` is 494 lines; `Netplay_TickMatchmaking` no longer exists; `--headless` now has consumers. |
| 2026-09-02 | **Quick Training SHIPPED** (this lane, on `a8250882`). The SJ-06 chain promoted from the spike into `src/scene_jump.c`; the OSD feature in `src/quick_training.c`; OSD row + wrapper + signal wired. New findings SJ-22..SJ-25, new §10. The spike now calls the shared chain, so `--test-instant-jump` still proves it (PASS, same numbers: drain=24, jump->live=62). |
| 2026-09-05 | **Fix pass on the review of the above** (same day): P1 — `docs/training-score.md`'s new correction named two functions that do not exist (real sites are `combo_window_push()` / `combo_window_trans()`); `--test-instant-jump` was rewriting the user's training file, now closed at the harness (§10.7, SJ-28). P2 — the teardown restore put the runtime signals back to `SIG_DFL` (terminate) before `ConsoleMode_Exit()`, now `SIG_IGN`; the "SHA256 sweep" justification for the boot window was false (`PORT_MISTER` excludes `CHECKSUM`), `SA_RESTART` added; the DIP masks' "bits 4..11" / "bits 16..19" rationales were false and are now enumerated; the DIP assertion pins a *configuration* and says so; the SELECT-reset stage covers one of four presets and now keys off `Suicide[0]` rather than `routine_no != 4`; a PASS now names its skipped assertions. |
| 2026-09-05 | **Engine defects found by review and fixed** (§10.6, SJ-27): the chain skipped character select's training-config load AND its `init_omop()`, so the match ran on zeroed settings and zeroed engine DIP tables — and then wrote the zeros over the user's config. Measured before/after, both players now land byte-identical to the stock select path. Corrections to this document in the same pass: SJ-25's "byte-identical" claim was true only against an already-zero config (§10.4); the §10.4 RTL bit list omitted `status[28:25]` and `status[46:43]`; §10.5's deferral bound expired into a silent drop and now expires into a start. `SIGRTMIN+5` made non-fatal in the boot and version-skew windows (§10.4, device-unverifiable). |

---

## How to extend this doc

1. New finding -> next free `SJ-nn`, index row, section under the right heading.
2. Durable citation on every claim — `file` -> `symbol` or exact quoted line
   text; literal **UNVERIFIED** for anything else. Line numbers only as
   `@ <commit>` hints.
3. Re-validate citations before trusting them — HEAD moves.
4. Record ruled-out approaches too (§2.5, §5.1) so nobody re-treads them.

---

## 1. TL;DR

Three asks, three different answers:

| Ask | Verdict |
|---|---|
| Training save states | **Viable.** Primitive exists and is proven at 60fps. One real blocker (audio). |
| Quick training mode | **Viable, and needs no save state.** Settings already persist; a working driver exists but is DEBUG-gated. |
| Instant jump to netplay Versus | **Viable via the chain (§4) + black cover (§5.4)** — now PROVEN for the training variant by the §4.1 prototype. NOT via a cross-scene save state (§2.5), and NOT via menu automation. |

The instant-jump design in §5 **deletes** `netplay_nav.c` (494 lines at
`762b5052`) rather than adding to it.

---

## 2. The shared foundation — the rollback state system

### 2.1 [SJ-01] The API already exists and is not netplay-shaped

`src/netplay/game_state.h` declares, near its end:

```c
uint32_t save_current_state(void* buffer, int frame);
void     load_state(const struct State* src);
void     save_state(const struct GekkoGameEvent* event);       // Gekko wrapper
void     load_state_from_event(const struct GekkoGameEvent*);  // Gekko wrapper
```

`save_current_state` takes a **plain buffer and a frame number**. The GekkoNet
coupling lives only in the two `*_event` wrappers. Also public:
`GameState_Save(GameState*)` / `GameState_Load(const GameState*)` (same
header).

Container: `typedef struct State { GameState gs; EffectState es; }`
(`game_state.h` -> `struct State`).

### 2.2 [SJ-02] Sizes, measured — and the cost is already proven

Measured 2026-08-29 by compiling a `sizeof` probe against the real build
flags (reproduce per Appendix A):

| | ARM32 (device) | arm64 (host) |
|---|---|---|
| `sizeof(GameState)` | **17,772** at `762b5052` (`game_state.h` -> `EXPECTED_GAME_STATE_SIZE`) | 19,344 @ `1f981b73` |
| `sizeof(EffectState)` | ~229,684 @ `1f981b73` | 459,064 @ `1f981b73` |
| `sizeof(State)` | ~242 KB | ~467 KB |

The ARM32 figure is pinned by `_Static_assert(sizeof(GameState) ==
EXPECTED_GAME_STATE_SIZE, ...)` in `game_state.c`, active on 32-bit builds
only. **This number moves often** (17784 -> 17772 in one day once). Read
`EXPECTED_GAME_STATE_SIZE` from `src/netplay/game_state.h`, never from this
doc. The long comment above the assert narrates the historical growth; do not
quote intermediate numbers from it. The 64-bit tripwire is *deliberately*
disabled (the `#else` branch beside the assert explains why) — the
host/device size difference is expected, not a bug.

Sparse slot budget: `SPARSE_CEILING_BYTES` (`game_state.h`) =
`sizeof(GameState) + SPARSE_HEADER_BYTES + SPARSE_CEILING_SLOTS ×
SPARSE_FRW_SLOT_BYTES` (100 slots × 1792 bytes each on ARM32) ~= **193 KB
per save slot** on ARM32. Ten slots ~= 1.9 MB.

**Cost is already proven on hardware:** `save_state(event)` fires on every
`GekkoSaveEvent` (`src/netplay/netplay.c` -> the `GekkoSaveEvent` case) — the
full snapshot is taken **every frame at 60fps on MiSTer ARM** during netplay.
A user-triggered save is orders of magnitude rarer.

Not a flat POD memcpy: 600+ discrete `SDL_memcpy` calls (the `GS_SAVE` /
`GS_LOAD` macros in `game_state.c`). It does not walk pointers — pointers are
copied by value.

### 2.3 What is covered, and what is deliberately excluded

Covered (representative): `GS_SAVE(plw)`, `GS_SAVE(task)`, all RNG indices,
`GS_SAVE(Score)`, `GS_SAVE(Stop_Combo)`, `GS_SAVE(cmd_sel)`, `GS_SAVE(bg_w)`,
`GS_SAVE(wcp)` / `GS_SAVE(waza_work)`, `GS_SAVE(spmv_ng_save)` — all in
`src/netplay/game_state.c` — plus `EffectState` (the 128-slot effect pool).

**Excluded by design**, with the rationale written down in
`tools/rollback-determinism/allowlist.txt`: sound sinks, render/texture/
palette sinks, loader and RAM-allocator state — `afs_handle`,
`ldreq_result`, `rckeyctr`/`rckeymin`/`rckeyque`. They are kept out because
they are *baseline-nondeterministic* (async disk I/O), so no save set could
rewind them. The definitive disposition comment is the block above
`plt_req` in `src/sf33rd/Source/Game/io/gd3rd.c`.

Additional documented blind spots: heap state and dylib/VRAM state are outside
the image entirely (`docs/rollback-determinism-harness.md`, known limits).

### 2.4 [SJ-03] Snapshots are PROCESS-LOCAL

This decides in-memory vs on-disk. Independently verified:

- `struct _TASK` contains a **raw function pointer**:
  `void (*func_adrs)(struct _TASK* task_ptr)` — `include/structs.h` ->
  `struct _TASK`.
- `task[11]` is saved and restored **wholesale**: `GS_SAVE(task)` /
  `GS_LOAD(task)` in `game_state.c`.
- `WORK` (`include/structs.h`) carries **31 pointer members** — 42 slots
  counting `char_table[12]` — including `target_adrs`, `set_char_ad`,
  `body_adrs`, `hit_ix_table`, `attack_adrs`. `WORK` is embedded in both
  `plw[]` and every one of the 128 effect-pool slots.

**Consequence:** in-memory save states work today (this is exactly what
rollback does). **Persisting a slot to disk across launches requires pointer
relocation** — function pointers move between runs under PIE/ASLR.

### 2.5 [SJ-14] Cross-scene restore is NOT viable

Each of these is independently fatal:

- The excluded categories (§2.3) are precisely the scene-dependent ones.
- `char_init_data[23]` holds 25 pointers per character
  (`src/sf33rd/Source/Game/engine/charid.c` -> `char_init_data`), relocated
  on load in `texgroup.c` -> `q_ldreq_texture_group`.
- `texgrplds[100]` (`src/sf33rd/Source/Game/rendering/texgroup.c` ->
  `texgrplds`) and the ramcnt ledger (`src/sf33rd/Source/Game/system/
  ramcnt.c`) are heap.
- Reloading a different scene re-runs one-shot asset inits whose arcade traps
  are still live (`PPGFile.c`; `docs/rollback-determinism-harness.md` known
  limit 1).

**Do not attempt a save-state-based scene jump.** Use the chain (§4).

---

## 3. [SJ-04] Training save states — viable

### 3.1 Why same-scene restore is safe

Every pointer in `plw[]`/`WORK` targets either a BSS global or a ramcnt block
pinned for the life of the loaded match:

| Field | Target | Binding site |
|---|---|---|
| `sa` | `super_arts[2]` | `engine/plcnt.c` (player init) |
| `py` | `piyori_type[2]` | `engine/plcnt.c` |
| `cb` / `rp` | `combo_type[ix]` / `remake_power[ix]` | `engine/plcnt.c` |
| `cp` | `wcp[2]` | `engine/cmd_main.c` |
| `target_adrs`/`hit_adrs`/`dmg_adrs` | `&plw[(ix+1)&1]` | `engine/plcnt.c` |
| `char_table[12]`, `body_adrs`, … | `char_init_data[charset_id]` | `engine/charid.c` |

Backing arrays are all in the snapshot (`game_state.h` -> `GameState`).

The **effect pool links by s16 indices, not pointers**
(`include/structs.h` -> `WORK.myself`/`before`/`behind`; pool
`uintptr_t frw[EFFECT_MAX][448]` at `effect/effect.c`, `EFFECT_MAX 128` in
`effect/effect.h`), so it snapshots by value — and already does
(`EffectState`, `game_state.h`).

### 3.2 [SJ-05] BLOCKER — audio re-fire

SE requests hit the driver **synchronously** — `SsRequest`
(`sound/sound3rd.c`) -> `sound_request_for_dc` -> `cseTsbRequest`. No sound
state is in any save path: `current_bgm` (`sound3rd.c`), `bgm_req`, ADX
position are all absent from `GameState`/`State`. No rollback mute exists —
netplay's `No_Trans = !render` (`netplay.c`) gates *rendering* only.

Every restore therefore replays hits and voices from the saved frame while BGM
free-runs. Unwired reset helpers exist and are the obvious hook:
`sound_all_off()` (`sound3rd.c`), `BGM_Stop()` (`sound/se.c`).

This is a design task, not a one-liner.

### 3.3 Ranked hazards

**needs-work**
- **ColorRAM only partially covered.** Only 4x12 u16 are saved
  (`game_state.h` -> `effl8_colorram`). A minutes-later restore can leave a
  stale Twelve-metamorph / Makoto-buff palette. Visual, not fatal.
- **`Interrupt_Timer` reseeds RNG offline.** Free-running, not saved;
  `game.c` -> `Game01` case 0 does `Random_ix32 = Interrupt_Timer` when
  `Mode_Type != MODE_NETWORK`. Repeated restores are not bit-reproducible at
  round boundaries.
- **`No_Trans` texture-cache guard.** `game.c` -> `Game_Task` gates
  `texture_cash_update()` on `!No_Trans` (see its long comment). Verify cache
  aging if you ever restore without drawing.

**benign**
- **LDREQ / `q_ldreq` split** — real gap, but unreachable mid-match: every
  `Push_LDREQ_Queue*` call site is in `demo00.c`, `next_cpu.c`, `win.c`,
  `ranking.c`, `sel_pl.c`, `menu.c` — none in `engine/` or `effect/`
  (re-verified at `762b5052`: 20 sites in those screen/menu files, 0 under
  `engine/` or `effect/`).
- **eff79 `OK_Appear79`/`Extra_Counter`** — FIXED (`GS_SAVE(Extra_Counter)`
  in `game_state.c`); char-select-scoped anyway.
- **`ca_check_flag`** — FIXED (`GS_SAVE(ca_check_flag)` in `game_state.c`).
- **`fd_prev_active_cgix_tick`** — unsaved (`engine/workuser.c`), consumers
  are frame-data-overlay only (`engine/charset.c`).

---

## 4. [SJ-06] THE SCENE SIDE-EFFECT CHAIN

**This is the key deliverable of this research**, re-verified step by step at
`762b5052` and then **proven executable outside task dispatch** (§4.1).

Everything a jump into a running match must reproduce, in order. "Where"
names the function that performs the step on the stock path; grep it.

| # | Action | Where (stock path) |
|---|---|---|
| 1 | `TexRelease(601)`, `title_tex_flag = 0` | `game.c` -> `Game0_2` case 3 |
| 2 | `Purge_mmtm_area(2)` then `Make_texcash_of_list(2)` | `game.c` -> `Game0_2` case 4 |
| 3 | `BGM_Request(65)` | `game.c` -> `Game0_2` case 5 |
| 4 | `Menu_Init`: `All_Clear_Suicide`, `pulpul_stop`, `bg_etc_write_ex(2)`, `Setup_Virtual_BG`/`Setup_BG(1/2)`, `effect_57_init`, **`load_any_texture_patnum(0x7F30, 0xC, 0)`** | `menu.c` -> `Menu_Init` |
| 5 | `Menu_Common_Init()`, `Clear_Personal_Data(0/1)`, `Vital_Handicap[ix][*] = 7`, `VS_Stage = 0x14`, `Order[]`/`Order_Dir[]`/`Order_Timer[]`/`Message_Data[]` | `menu.c` -> `Mode_Select` case 0 |
| 6 | Effect-pool priming: `effect_57_init`, `effect_04_init`, 7x `effect_61_init` | `menu.c` -> `Mode_Select` case 0 |
| 7 | `checkAdxFileLoaded()`, `checkSelObjFileLoaded()` — **synchronous, NOT async; see SJ-16** | `menu.c` -> `Mode_Select` case 1 |
| 8 | `Setup_VS_Mode`: `r_no[0]=5`, `cpExitTask(TASK_SAVER)`, `plw[].wu.wu_operator=1`, `Operator_Status`, 4x `grade_check_work_1st_init`, `Setup_Training_Difficulty`; mode set (`Mode_Type`, `Present_Mode`, `Decide_ID`/`Champion`/`Pause_ID`/`Training_ID`/`New_Challenger`, `TrainingConfig_RestoreCharSelect`, `cpExitTask(TASK_ENTRY)`) | `menu.c` -> `Setup_VS_Mode`; mode set in `Mode_Select` case 3 |
| 9 | `effect_work_init()` | `game.c` -> `Game12_2` |
| — | *(character select scene runs here on the stock path — see §4.5 for what replaces it in a jump)* | `game.c` -> `Game01` / `screen/sel_pl.c` |
| 10 | **GATE**: `Check_LDREQ_Clear()` — `== 0` is `fatal_error("Load queue failed to drain in time")` | `game.c` -> `Game2_0` (also `Game2_2`) |
| 11 | `System_all_clear_Level_B()`, `All_Clear_Random_ix/Timer/ETC` (mode-dependent), `C_No[0..3]=0`, `clear_hit_queue`, `bg_work_clear`, `win_lose_work_clear`, `player_face_init`, `G_Timer = 10`, `TATE00` | `game.c` -> `Game2_0` |
| 12 | `effect_work_quick_init()`, `Bg_On_R(stage_bgw_number)` — **Reset_Replay path only; see SJ-18** | `game.c` -> `Game2_2` |

Also on the path: `Game01` case 0 — `S_No[]=0`, `SsBgmHalfVolume(0)`,
`BGM_Request(53|66)`, `Break_Into = 0`, `Stop_Combo = 0`, RNG seeding,
`init_slow_flag`, `System_all_clear_Level_B`, `pulpul_stop`,
`init_pulpul_work` (`game.c` -> `Game01`).

**Traps when calling this outside its normal task context:** see §4.6
(hardcode census) and §4.7 (the training menu blocks the round).

Related existing teardown entry points, if useful:
`Soft_Reset_Sub()` (`system/sys_sub.c`, the most complete),
`Back_to_Mode_Select()` (`menu.c`),
`Reset_Training()` (`menu.c`; its round reinit is
`C_No[0] = 1; G_No[2] = 5`).

### 4.1 [SJ-15] SETTLED — the chain DOES run outside task dispatch

**The prototype exists and passes:** `src/test/scene_jump_spike.c`
(`#if DEBUG`, armed by `--test-instant-jump`, hooked in
`test_runner.c` -> `TestRunner_Prologue`). From a cold boot it mashes START
to the title screen, then in ONE prologue call — outside any `task[]`
dispatch, before that frame's `Game_Task` — performs steps 1-9 plus the
char-select replacement (§4.5), pushes the player/BG loads, parks the scene
in `Game12_0` behind `No_Trans`, waits for the drain, flips to
`Game2_0`, dismisses the training menu (§4.7), lifts the cover, and
verifies. Run it:

```sh
build/host/3S-ARM.app/Contents/MacOS/3S-ARM \
  --test-enable --test-instant-jump [--ldreq-barrier-force] \
  [--test-p1-character N --test-p2-character N --test-stage N]
```

Measured host result (Debug build, 2026-09-02, defaults Yun vs Ryu, stage 2):

```
SCENE-JUMP PASS: jump@43 entered@67 live@105 verified@285 — drain=24 frames,
jump->live=62 frames (Mode_Type=3 Play_Type=1 operators=1/1 chars=3/2 stage=2)
```

and with `--ldreq-barrier-force`: `drain=1 frames, jump->live=39 frames`.
A live-window screenshot shows a fully healthy match: both sprites, full HUD
(health bars, training infinity timer, nameplates, portraits, stun/SA
gauges) — i.e. NOT the §5.1 failure. Also passes with other
character/stage combinations and without `--test-pin-rng`.

**What "task dispatch context" actually is, mechanically.** The dispatcher
(`src/main.c` -> `cpLoopTask`) does exactly one thing per frame per task:
`task_ptr->func_adrs(task_ptr)` when `condition == 1`. Everything else the
chain's steps consume is ordinary global state:

- `cpReadyTask` (`main.c`) zeroes the `_TASK` struct and sets
  `condition = 2`; `cpLoopTask` case 2 flips 2 -> 1 **without calling**, so a
  readied task first runs 1 frame later (2 frames if the readier's TaskID is
  greater than the readied task's — the loop has already passed that slot).
  `cpExitTask` is `SDL_zero(task[num])`. Case 3 of the condition switch has
  **no writer anywhere at `762b5052`** (`grep 'condition = 3'` is empty) —
  it is dead.
- `r_no[]`, `timer`, `free[]` live in the task struct and are the state
  machines' program counters; `G_No[]`/`S_No[]`/`C_No[]`/`E_No[]` are plain
  globals (`GS_SAVE`d) the dispatcher never touches. A direct caller that
  writes them gets exactly the same dispatch next frame.
- The multi-frame steps do not re-enter *themselves*; they return and are
  re-dispatched after the rest of the frame (loader pump, effect movers,
  fades, `AFS_RunServer`) has run. That is the only real service dispatch
  provides: **something else runs between two calls of the same step.**

So a direct back-to-back call of the chain works if and only if each
cross-frame dependency is either completed synchronously or left to the
normal loop behind the cover. The complete list of cross-frame dependencies
found (nothing else in steps 1-12 is multi-frame):

| Dependency | Stock mechanism | Jump treatment (proven) |
|---|---|---|
| LDREQ queue drain (step 10's gate) | one `ldreq_pump_head()` per frame (`game.c` -> `Game_Task` tail -> `Check_LDREQ_Queue`) | wait behind cover (24 f) or force the task-#66 barrier (`Ldreq_SetBarrierForced`) for a same-frame drain |
| `Switch_Screen` wipes / `FadeIn`/`FadeOut` | wipe/fade advances per frame | skip entirely — cosmetic under `No_Trans`; do not call `Switch_Screen_Init` and no wipe state is armed |
| `cpReadyTask` arming latency | condition 2 -> 1 -> run | irrelevant; the jump exits tasks rather than readying them (round init readies TASK_MENU itself) |
| Round intro (`manage.c` -> `Game_Manage_2_x`, `Cover_Timer`) | frame-driven | left to run behind the cover (~38 frames) |
| Training-menu dismissal (§4.7) | player input | injected confirm press behind the cover |

**Boundary of the finding:** proven for the offline training jump on host.
The netplay-Versus variant (same chain, `MODE_NETWORK`) and on-device
behaviour are expected to follow but are **UNVERIFIED** — the spike does not
exercise `Wait_Seek_Time`'s network arm or GekkoNet session start.

### 4.2 [SJ-16] Step 7 is synchronous and idempotent, not an async gate

The original doc called steps 7 and 10 "the only genuine floor — async disk
I/O". Half right. Step 7 is **synchronous blocking** on this port:

- `checkAdxFileLoaded` (`sound/sound3rd.c`) busy-loops
  `do { key = load_it_use_any_key(fnum, 21, 0); } while (key == 0)` and
  early-returns when `adx_NowOnMemoryType == sys_w.bgm_type`.
- `checkSelObjFileLoaded` (`rendering/texgroup.c`) busy-loops
  `load_it_use_this_key(...)` and early-returns when
  `omSelObjNowOnMemoryType == mpp_w.language`.
- Both bottom out in `io/gd3rd.c` -> `load_it_use_this_key`, which calls
  `fsFileReadSync` — the read completes within the call.

So step 7 costs zero *frames* by construction (wall-clock only, inside one
frame), and calling it twice is free. Only step 10 — the `q_ldreq[16]` queue
(`io/gd3rd.c`) — is genuinely multi-frame on the stock path, and the netplay
LDREQ **frame barrier** already built for task #66 (`io/gd3rd.c` ->
`Check_LDREQ_Queue` drain loop, `Ldreq_BarrierActive`,
`Ldreq_SetBarrierForced`, pumped by `AFS_PumpBlocking`) collapses it to one
frame, bounded by `LDREQ_BARRIER_BUDGET_MS` / `LDREQ_BARRIER_MAX_STEPS`.

### 4.3 [SJ-17] Gate cost, measured (settles open question #2 on host)

Host, Debug build, warm page cache, `SDL_VIDEODRIVER=dummy`, 2026-09-02:

- **Stock cadence:** the jump's pushes (2 players + 1 stage) drain in
  **24-25 frames** — one queue-head pump per frame.
- **Barrier-forced:** the same drain completes inside the push frame:
  `[ldreq-barrier] drained in 23 steps / 4 ms / 2271232 bytes` — so the
  black cover's I/O floor is **1 frame** on host.
- End to end behind the cover: jump -> `Allow_a_battle_f` in **62 frames**
  (stock drain) / **39 frames** (barrier), dominated by the round intro
  (`Cover_Timer = 24` from the select-exit path plus `Game_Manage_2_x`), not
  by I/O.
- On the stock menu path the gate is even cheaper: the tracked run
  (`--ldreq-trace` on the `training-yun-ryu-ryu-stage` preset) shows the
  queue's last busy frame ~70 frames before `Game2_0` runs its check —
  character select absorbs the whole drain.

**Device numbers are pending.** The transferable figure is the byte count
(2,271,232 bytes for Yun+Ryu+stage 2 — the same bytes are read from SD on
MiSTer), and the `[ldreq-barrier] drained in ...` telemetry line prints on
device builds (`ENABLE_PERF_TELEMETRY`), so one device run of the spike
answers it by measurement.

### 4.4 [SJ-18] Step 12 belongs to Reset_Replay, not the virgin path

`Game2_2` — the step-12 row (`effect_work_quick_init()`, the `Bg_On_R`
loop) — is dispatched at `G_No[2] == 2`, and the only writer of
`G_No[2] = 2` under `Game02` at `762b5052` is `menu.c` -> `Reset_Replay`.
The virgin select->battle path is `Game2_0` (sets `G_No[2] = 3`) ->
`Game2_3` (counts `G_Timer` 10 down) -> `Game2_1` (live). On that path the
BG layers are activated by `stage/bg.c` (`Bg_Texture_Load_EX` and round
effects such as `effd3.c`), not by `game.c`. The jump therefore does NOT
perform step 12; the spike confirms nothing misses it.

### 4.5 [SJ-19] `Load_Replay_Sub` is the in-tree char-select bypass

`menu.c` -> `Load_Replay_Sub` already jumps from a menu into a running match
with **no character select**, for MODE_REPLAY. Its sequence is the template
for what replaces the select scene in any jump (the spike copies it):

1. `cpExitTask(TASK_ENTRY)`; `Play_Mode = 3`.
2. `Mode_Type`/`Present_Mode`, `plw[ix].wu.wu_operator`, `Operator_Status`,
   `My_char`, `Super_Arts`, `Player_Color`, `Vital_Handicap`, `bg_w.stage`,
   `save_w[3]` settings; `cpExitTask(TASK_SAVER)`.
3. `System_all_clear_Level_B`, `pulpul` reset, VS-splash effects.
4. `BGM_Request(51)`, `Purge_memory_of_kind_of_key(0xC)`,
   **`Push_LDREQ_Queue_Player(0/1, My_char[..])`, `Push_LDREQ_Queue_BG(stage)`**.
5. Wait on `Check_PL_Load() && Check_LDREQ_Queue_BG(bg_w.stage) &&
   adx_now_playend() && sndCheckVTransStatus(0)`.
6. `Game01_Sub()`, `Cover_Timer`, `set_hitmark_color`,
   `Purge_texcash_of_list(3)` + `Make_texcash_of_list(3)`,
   `G_No[1] = 2; G_No[2] = 0`, `Sel_Arts_Complete`, `cpExitTask(TASK_MENU)`.

What character select otherwise contributes, for the record: the
`Push_LDREQ_Queue_Player` loads as cursors move, `My_char`/`Super_Arts`/
`Player_Color` writes, `Setup_ID()`, stage choice, and the select-exit block
in `Game01`'s default case (`Game01_Sub`, `Cover_Timer = 24`, texcash group
3 swap, `appear_type`, `set_hitmark_color`).

### 4.6 [SJ-20] Hardcode census — every helper that force-writes the mode

`Mode_Type = MODE_ARCADE` appears at exactly 5 sites at `762b5052`:

| Site | Guard |
|---|---|
| `system/sys_sub.c` -> `Reset_Sub0` (with `Present_Mode = 1`) | none — fires on every soft reset |
| `menu.c` -> `Mode_Select` case 0 (with `Present_Mode = 1`) | none — fires on every mode-menu entry |
| `menu.c` -> `Mode_Select` case 3, cursor 0 | the arcade choice itself (intended) |
| `game.c` -> `Game0_2` case 5 | `SDLApp_IsArcadeGameMode()` |
| `game.c` -> `Loop_Demo` arcade reroute | `SDLApp_IsArcadeGameMode()` |

`Present_Mode` writers: `init3rd.c` (=1 at boot), `game.c` ->
`Next_Title_Sub` (=1), `game.c` -> `Loop_Demo` case 0 (=0), `game.c` ->
`Next_Demo_Loop` (=0), `demo/demo02.c` (=0), `menu.c` -> `Mode_Select`
case 0 (=1), the training confirm (=4), `menu.c` training/option entries
(=4 / =5), `menu.c` -> `Load_Replay_Sub` (=3), `sys_sub.c` -> `Reset_Sub0`
(=1).

**Rule:** any reuse of a reset/menu helper must write `Mode_Type` /
`Present_Mode` *after* the helper, never before. The spike orders its writes
accordingly and never calls `Reset_Sub0` or enters `Mode_Select` at all.

### 4.7 [SJ-21] A training match starts ON its menu — the round blocks on it

Two facts any jump must respect (both bit the prototype first):

- `manage.c` -> `Game_Manage_1st` readies `TASK_MENU` with `r_no[0] = 7`
  (`MENU_STATE_TRAINING_MENU`) in training, and `Game_Manage_2_1` case 1
  holds the round at `C_No[1] == 1` until `task[TASK_MENU].r_no[0] == 10`
  (`Wait_Pause_in_Tr`). The menu must be dismissed — input-driven, the way
  `test_runner.c` -> `PHASE_GAME_TRANSITION` does it (cursor 0 + confirm) —
  or `Allow_a_battle_f` is never set (`Game_Manage_2_4` case 3).
- Step 4's `load_any_texture_patnum(0x7F30, 0xC, 0)` (`menu.c` ->
  `Menu_Init`) is load-bearing for that menu: skipping it produced repeated
  `ppgCheckTextureNumber ... FAIL:handle=0` log lines at round start.
  (A baseline set of those lines — nums 154-155, 214-219 — appears on the
  stock preset path too; only the *additional* ones were the spike's fault,
  and all of them stop once the match is live.)

---

## 5. Instant mode jump — the design

### 5.1 Why the original hard jump failed (ruled out, do not retry as-is)

Cold-launched netplay once called `setup_vs_mode()` directly. Result:
"character select and VS pre-match screens rendered only the background — no
character portraits, cursors, or UI… The hard jump skipped the long
side-effect chain" (`docs/netplay-auto-nav.md`, "Why it exists").

That was **step 8 alone, with steps 1-7 and 9-11 missing**. The approach was
not wrong; the other steps were undiscovered. §4.1's prototype is the same
idea done with the full chain — and it renders a complete match.

### 5.2 [SJ-07] Netplay orchestration is ALREADY scene-independent

`src/main.c` -> `game_step_0`, the no-active-session branch (the `else` after
the `Netplay_GetSessionState() != NETPLAY_SESSION_IDLE` and replay-hold
branches), at `762b5052`:

```c
njUserMain();              // the ENTIRE normal game: attract, title, demo, menus
seqsBeforeProcess();
ReplayOverlay_Draw();
ReplayShuffle_Draw();
njdp2d_draw();
seqsAfterProcess();
Netplay_TickDirectP2P();
defer_direct_p2p_handoff_tick();   // #if defined(ENABLE_NETPLAY)
DirectP2P_Tick();          // UPnP lease renewal runs on a side thread
```

STUN, UPnP, hole-punch and peer discovery **already tick every frame
regardless of scene**. Waiting on attract/title costs nothing and requires no
changes. (`Netplay_TickMatchmaking`, quoted in the 2026-08-29 version, no
longer exists at `762b5052` — zero grep hits; the matchmaking layer went away
with the direct-P2P consolidation.)

The only thing coupling netplay to the menus is `NetplayNav_Tick()`
(`main.c` -> `game_step_0`) — a separate concern layered on top.

### 5.3 [SJ-08] A status overlay for attract/title already exists

`src/netplay/direct_p2p_overlay.c`, from its own header comment:

> "Renders through the existing SSPutStrPro native-text path into the 384x224
> game canvas, so the overlay shows while the main menu is visible… Priority 1
> sits above the title sequence logo, attract-mode game frames, and 'PRESS ANY
> BUTTON'"

Drawn once per frame from `NetplayScreen_Render()`
(`src/port/sdl/netplay_screen.c`, called from `sdl_app.c`), so it is
independent of game scene. No-ops when `DirectP2P_GetState() ==
DIRECT_P2P_IDLE`. Priority constant `DP2P_OVL_PRIO 1`
(`direct_p2p_overlay.c`) — note the engine convention, LOWER = in front.

**The "wait" UI is already built.**

### 5.4 [SJ-09] `No_Trans` gives the black cover for free

`No_Trans` is the engine-wide suppress-all-drawing flag — every draw entry
early-outs on it (e.g. `sc_sub.c` draw helpers; `game.c` -> `Game_Task`
gates `texture_cash_update` on it, see its comment). Setters: `init3rd.c`,
`game.c` -> `Game_Task` (the sysFF loop), `screen/entry.c`, and **netplay
already uses it exactly this way**: `No_Trans = !render` in `netplay.c`
blanks rollback re-simulation frames.

Hold it across the transition; clear it when the match is up. §4.1's
prototype did exactly this; the gates become invisible rather than merely
short.

### 5.5 The resulting design

1. Player picks netplay from the OSD.
2. Game **stays on attract/title** — a real screen, not a puppeted menu. The
   existing overlay (§5.3) shows connection status.
3. Orchestration runs in the background. **Already does; no changes** (§5.2).
4. On peer ready: set `No_Trans`, run the chain (§4 — the §4.1 spike is the
   working skeleton), clear `No_Trans`.
5. Match appears. One cut, black in between — measured 39-62 frames for the
   training variant on host (§4.3).

Quick training is the same, minus step 3 — and is exactly what the spike
already does.

### 5.6 This DELETES code

`src/netplay/netplay_nav.c` (494 lines at `762b5052`) goes away entirely.
Both of its jobs disappear: the wait moves to attract, where orchestration
was already running; the traversal becomes the chain call behind black.

For reference, what nav does today: one `SWK_START` per
`NAV_PRESS_DEBOUNCE_FRAMES` (`netplay_nav.c`) across three press/wait
states, each advancing on an *engine* condition, not a timer. Safety
timeouts are 600 frames and `NAV_WAIT_ORCH_TIMEOUT_FRAMES`.
**UNVERIFIED:** no nominal frame count for a full nav traversal exists in
the code or the doc; it is dominated by fades the module does not control
(four `FadeOut(1, 0xFF, 8)` in `Game0_2` alone, plus `Cover_Timer` waits of
23-24). Measure before quoting a number.

**One wrinkle:** nav also serves as the wait for STUN/UPnP to produce a remote
IP (`NAV_WAIT_ORCHESTRATOR`). Removing it means the wait needs an explicit
home — which is exactly what §5.3's overlay on attract provides.

---

## 6. Quick training mode

### 6.1 [SJ-10] Settings and character select ALREADY persist

`TrainingConfigFile` — 44 bytes (pinned by `_Static_assert`), magic `"TRN1"`,
version 2 (`src/port/config/training_config.c`) — stores
`contents[2][2][7]` (all training-menu cells), `cursor_x[2]`, `cursor_y[2]`,
`super_arts[2]`, `my_char[2]`.

Path: `Paths_GetPrefPath() + "training"` (`training_config.c`) ->
`/media/fat/games/3s-arm/training` on MiSTer (`src/port/paths.c`;
`THIRDSARM_HOME` overrides, honoured on every port).

Written at three sites: `Soft_Reset_Sub()` when in training (`sys_sub.c`),
`Setup_NTr_Data()` (`menu.c`), `Character_Change()` (`menu.c`). Read into
both `Training[0]` and `Training[2]` from `Default_Training_Data(0)`
(`menu.c`).

**`TrainingConfig_RestoreCharSelect()`** (`training_config.c`) already
restores `Cursor_X/Cursor_Y/Arts_Y/Last_Super_Arts/Last_My_char2`, and is
already called on both training entry paths (`menu.c` -> `Mode_Select`
training confirm; `menu.c` -> `Training_Mode`).

**So "last settings" needs no new persistence work at all.**

### 6.2 [SJ-11] A working menu-to-match driver exists, but is DEBUG-gated

`--test-scene-preset` supports 16 presets (`src/args.c`; enum
`TestScenePreset` in `src/test/test_runner.c`); two are training —
`training-yun-ryu-ryu-stage` and `training-frame-data`.

How it reaches a live training match — hybrid, phase machine in
`test_runner.c` (`Phase` enum, ticked from `TestRunner_Prologue()`):
`PHASE_TITLE` mashes `SWK_START` until `task[TASK_MENU].r_no[0..2] ==
{0,1,3}`; `PHASE_MENU` writes `Menu_Cursor_Y[0]` then mashes confirm;
`PHASE_CHARACTER_SELECT` hard-writes `Cursor_X/Cursor_Y` and
`My_char/Last_My_char2/Arts_Y/Super_Arts/Sel_Arts_Complete/Used_char` +
`Setup_ID()` (`maybe_force_training_scene_character_and_super_state` /
`maybe_force_training_scene_super_confirm`); `PHASE_GAME_TRANSITION` waits on
`training_mode_gameplay_started()` — `Mode_Type == MODE_NORMAL_TRAINING &&
Allow_a_battle_f != 0 && Game_pause == 0 && Pause_Down == 0`.

**It renders normally** — it is a prologue hook in the ordinary frame loop
(`src/main.c` -> `game_step_0` calls `TestRunner_Prologue` under
`#if defined(DEBUG)`), and `tools/mister/perf-sampler.sh` drives these
presets on real MiSTer hardware to measure on-screen FPS.

*Correction at `762b5052`:* the 2026-08-29 claim that `--headless` "has no
consumer anywhere" is stale — `configuration.headless` is now read by
`src/test/statcheck_compare.c` and `src/port/sdl/sdl_app.c`.

**The blocker:** `test_runner.c` opens `#if defined(DEBUG)` (stubs in the
`#else`), and the `main.c` call sites are `#if defined(DEBUG)`. `DEBUG`
comes only from `$<$<CONFIG:Debug>:DEBUG>` (`CMakeLists.txt`). Not in
shipped builds.

Note this driver still puppets menus — for a *clean* jump prefer the chain
(§4): the §4.1 spike reaches the same destination without touching select at
all, reusing this driver's char-select hard-writes as plain assignments.

### 6.3 [SJ-13] Replay / DUMMY RECORDING is inputs-only

Not reusable as a save state. `Setup_Replay_Buff()` (`system/sys_sub.c`)
writes one `u16` per input change into
`Replay_w.io_unit.key_buff[2][7198]` (`include/structs.h`): low 12 bits =
button word, high 4 bits = repeat count-1. Source is `p1sw_0`/`p2sw_0` only
(`sys_sub.c` -> `Get_Replay`).

The only non-input data is the header `Rep_Game_Infor[10]`
(`sys_sub.c` -> `Get_Replay_Header`): characters, super arts, colors,
stage, direction, vital handicap, RNG seeds, `Champion`, `Control_Time`,
`Difficulty`. **No coordinates, no velocities, no health, no frame state.**

Because nothing positional is stored, replay restores by **re-running the
round** through `Reset_Training` (`menu.c`) — a full wipe plus
`C_No[0] = 1; G_No[2] = 5` round reinit back to default spawn positions.

---

## 7. [SJ-12] The OSD -> game trigger channel

Live, mid-run, **no process relaunch**. Mechanism is config-file rewrite plus
a UNIX signal. Re-verified at `762b5052` by symbol.

Wrapper side: `poll_status_changes()`
(`vendor/Main_MiSTer/thirdsarm_wrapper.cpp`) reads OSD status bits (FPS,
overclock, game mode, BGM, language, hold-to-pause). On change it atomically
rewrites `<THIRDSARM_HOME>/config` via `config.tmp` + `rename`, then signals
the child.

Signals (`thirdsarm_wrapper.cpp`, the `kRuntime*Signal` constants):

| Signal | Purpose |
|---|---|
| `SIGUSR1` (`kRuntimeFpsToggleSignal`) | FPS overlay toggle |
| `SIGRTMIN+2` (`kRuntimeArmClockCycleSignal`) | ARM clock cycle |
| `SIGRTMIN+3` (`kRuntimeGameModeCycleSignal`) | Game mode cycle |
| `SIGRTMIN+4` (`kRuntimeHoldToPauseCycleSignal`) | Hold-to-pause cycle |

Game side: `on_shutdown_signal()` sets flags (`src/main.c`), drained each
frame by `handle_signal_requests()`, each handler re-reading its key off disk
(`src/port/sdl/sdl_app.c`).

**To add a quick-training trigger:** a new `SIGRTMIN+5`, a new `CONF_STR` bit
in `vendor/Menu_MiSTer/menu.sv` (format `"P1O[13],Game Mode,Console,Arcade;"`,
`"T[29],Play Online;"`), and a game-side handler. The `--direct-p2p-handoff`
intent-file + argv path (`thirdsarm_wrapper.cpp`) is only needed when the
game must actually restart.

Non-`CONF_STR` screens (Button Check, Direct-P2P) are added via
`tools/mister-wrapper/main-mister-full-menu.patch`.

---

## 8. Build vs. already-built

| Piece | Status |
|---|---|
| Snapshot save/load primitive | **Built** (`game_state.h` -> `save_current_state`), proven at 60fps |
| Sparse effect-pool encoding | **Built** (`game_state.h` -> `SPARSE_CEILING_BYTES`) |
| Scene-independent netplay orchestration | **Built** (`main.c` -> `game_step_0`) |
| Connection status overlay over attract/title | **Built** (`direct_p2p_overlay.c`) |
| Black-cover mechanism | **Built** (`No_Trans`, `netplay.c`) |
| Training settings + char-select persistence | **Built** (`training_config.c`) |
| OSD -> game signal channel | **Built** (4 signals; add a 5th) |
| **The chain function (§4)** | **SHIPPED** — `src/scene_jump.c`, driving OSD Quick Training (§10, SJ-22); spike calls it and still PASSES. Device test + netplay variant remain |
| **Quick Training feature (§10)** | **SHIPPED** — `src/quick_training.c` + OSD row + wrapper signal; host-verified, device run pending (SJ-22..SJ-27). Two engine defects found by review 2026-09-05 and fixed; see §10.6 |
| **Audio suppression on restore (§3.2)** | **TO BUILD** |
| Promote test-runner driver out of `#if DEBUG` | **TO BUILD** (optional; chain is cleaner) |
| Save-state slot UI / hotkeys | **TO BUILD** |
| Pointer relocation for on-disk slots | **TO BUILD** (only if disk persistence wanted) |

Suggested order: productize the chain (unblocks both jump features) -> OSD
trigger -> training save states (in-memory) -> disk persistence if still
wanted.

---

## 9. Open questions / UNVERIFIED

1. ~~Does the chain run correctly outside its normal task dispatch context?~~
   **SETTLED — yes, and SHIPPED.** SJ-15 (§4.1): direct chain call, `No_Trans`
   cover, screenshot-verified healthy match, PASS on host. Now promoted into
   `src/scene_jump.c` and driving the OSD Quick Training feature (§10, SJ-22).
   Remaining unverified slice: the netplay-Versus variant, and on-device
   behaviour of Quick Training (the OSD/wrapper/signal path is patch-verified
   only — SJ-25 boundary).
2. ~~How many frames do the two async gates actually cost?~~ **SETTLED on
   host.** SJ-16/SJ-17: step 7 costs 0 frames (synchronous); step 10 costs
   24-25 frames at stock cadence or 1 frame barrier-forced (4 ms /
   2,271,232 bytes for Yun+Ryu+stage 2, warm cache). The cover is bounded by
   the round intro (39-62 frames to live), not I/O. **Device measurement
   pending** — run the spike on MiSTer and read the `[ldreq-barrier]` line.
3. **Nominal nav traversal frame count** — no measurement exists (§5.6).
   Needed only if you want a before/after number.
4. **Can training assets be pinned** so a boot-time pre-warm snapshot stays
   restorable (the "truly instant" quick-training variant)? Would require
   preventing `Purge_mmtm_area` from reclaiming them. Not investigated.
5. **`sag_union` gauge-type coverage** and other items are tracked in the
   companion training-mode doc (Desktop, outside the repo), not here.

---

## 10. Quick Training — the shipped feature

Built this lane (on `a8250882`). Selecting "Quick Training" at the top of the
MiSTer OSD dismisses the OSD and jumps the running game straight into a live
training match with the last-used characters and settings — no menus, no
character select. It is the §6 quick-training-mode design (SJ-10/SJ-11) built
on the §4.1 chain (SJ-15), and it reuses the spike's proof rather than
re-deriving it.

### 10.1 [SJ-22] The chain is now shipped code — the spike calls it

The SJ-06 chain was promoted verbatim out of `src/test/scene_jump_spike.c`
(`#if DEBUG`) into `src/scene_jump.c` (always built). Public surface
(`scene_jump.h`):

- `SceneJump_ExecuteTrainingChain(params)` — the whole chain as one direct
  call (sets `No_Trans = 1`, returns the stage used).
- `SceneJump_TrainingLoadsDrained(stage)` — the step-10 drain predicate.
- `SceneJump_EnterBattleScene()` — the `Game2_0` flip.
- `SceneJump_TrainingMenuDismissTick(parity)` — the SJ-21 menu-dismissal +
  liveness predicate.

Two consumers: `src/quick_training.c` (the feature) and the spike
(`scene_jump_spike.c` -> `SceneJumpSpike_Tick`, now a thin harness driver over
these calls). So **`--test-instant-jump` still proves the exact code the
feature runs** — re-run this revision on host, unchanged result:
`SCENE-JUMP PASS ... drain=24 frames, jump->live=62 frames` (Yun vs Ryu,
stage 2, Debug, `SDL_VIDEODRIVER=dummy`).

One chain addition over the spike: `SceneJump_ExecuteTrainingChain` accepts
`stage < 0` to mean "derive the stock choice", calling
`sel_pl.c -> Setup_Battle_Country` (the challenger's character id, with the
Q / double-Q random-stage special cases) — what character select would have
picked. The spike always passed an explicit stage, so this arm is feature-only.

### 10.2 [SJ-23] Edge cases — any offline scene reaches the match

The spike only ever ran from the title. The feature must fire from wherever
the player has the OSD open. `src/quick_training.c` (`QuickTraining_Tick`, a
`QtPhase` machine) handles it:

- **Refused** (logged, no-op) when the engine is not the feature's to drive:
  a netplay session (`Netplay_GetSessionState() != NETPLAY_SESSION_IDLE`),
  direct-P2P orchestration (`DirectP2P_GetState() != DIRECT_P2P_IDLE`),
  netplay nav (`NetplayNav_IsActive()`), or a replay / shuffle-viewer session
  (`ReplayPlayer_IsActive()` / `ReplayShuffle_IsEnabled()`). This is the
  netplay prohibition the constraints demand — see §10.3.
- **From the title idle** (`G_No {2,0,1}`): fire the chain directly, as the
  spike does.
- **From any other offline scene** — attract, a menu, character select, a
  live match, win/continue/ranking, even the pause menu — tear down to the
  title first via the shipped soft-reset flow (`Game_pause = 0x81`,
  `Request_LDREQ_Break`, `effect_work_init`, then
  `sys_sub.c -> Soft_Reset_Sub` once the break lands — the
  `reset.c -> Reset_Move`/`Reset_Wait` sequence, the same one
  `replay_player.c` and the netplay-disconnect path use), then coin back to
  the title and fire. `qt_needs_teardown()` is the discriminator
  (`G_No[0] == 2` outside `Game00` idle/dash).
- **Pre-coin** (boot `G_No[0] == 0`, attract `G_No[0] == 1`): coin in with an
  injected `SWK_START`, no teardown.
- A request **while a sequence is already running** is ignored.
- Every phase has a watchdog; a blown one calls `qt_fail()`, which recovers to
  a clean title via `Soft_Reset_Sub` so the device is never bricked.

Verified on host (`--test-quick-training <frame>` fires the feature's request
without the signal; `--test-quick-training-again` schedules a second): PASS
firing from boot (`G_No 0/0/0/0`), from attract (`G_No 1/1/0/0`), and — the
mid-match re-entry case — a second fire while the first match is live
(`G_No 2/2/1/0` -> teardown -> live again). `QUICK-TRAINING TEST PASS: 2
sequence(s) verified`.

### 10.3 [SJ-24] The wipe wraps the cover, sequentially — and stays offline

The maintainer asked for the jump to happen inside the game's diagonal wipe,
not a bare black cut. The sequence (`quick_training.c`):

1. `Switch_Screen_Init(0)` then `Switch_Screen(1)` per frame until it returns
   1 — the diagonal/checkered wipe-OUT (`sys_sub.c` -> `Switch_Screen`,
   `sc_sub.c` -> `WipeOut`; **type 1** is the diagonal look).
2. THEN `No_Trans = 1` and the chain + drain + menu dismissal run fully
   hidden.
3. `No_Trans = 0`, then `Switch_Screen_Init(0)` + `Switch_Screen_Revival(1)`
   per frame — the wipe-IN over the live match.

`WipeOut`/`WipeIn` both early-out on `if (!No_Trans)` (`sc_sub.c`), so the
wipe and the black cover **cannot draw at the same time**; the code is
strictly sequential (cover raised only after the wipe-out completes, lowered
before the wipe-in starts) — getting this wrong yields an invisible wipe.
`WipeIn`'s `WipeLimit == 0` frame fully covers (its own comment), so the
cover-to-wipe handoff has no one-frame gap.

**Rollback / netplay constraint (verified).** `Exec_Wipe` / `Exec_Wipe_F` are
in the `GS_SAVE` set (`game_state.c`), so driving the wipe is
rollback-visible. The wipe is driven **only** from `QuickTraining_Tick`, which
runs offline: `qt_refusal()` rejects every request while any session /
orchestration / nav / replay is active (§10.2), and the tick additionally
aborts defensively if a session ever activates mid-sequence. So the wipe is
unreachable from a live netplay session, exactly as required. This lane
touches neither `GameState` nor the `GS_SAVE` set — `EXPECTED_GAME_STATE_SIZE`
is unchanged (17772) and the rollback-determinism harness did not need to run.

### 10.4 [SJ-25] Trigger + settings — live signal, no restart, no drift

- **Persistence: none added** (SJ-10). Characters/arts come from the persisted
  training config via a new `TrainingConfig_GetLastUsed(chars, arts)`
  (`training_config.c`), which reads the same `TrainingConfigFile` (magic
  `"TRN1"`, `my_char[2]`/`super_arts[2]`) that
  `TrainingConfig_RestoreCharSelect` reads, with the same per-slot range
  clamps. Defaults when the file is missing: Yun vs Ryu, first super art.

  **The "leaves the file byte-identical" claim below this line was wrong, and
  the way it was wrong is worth keeping.** It was true only because the config
  the run was verified against already had an all-zero `contents` block: the
  chain never loaded the settings, so the zeros it then SAVED back matched what
  was there and the file really did come back byte-identical. Against a config
  with anything in it the same run destroyed it — measured 2026-09-05, ACTION/
  GUARD/QUICK-STAND/STUN `02 05 01 02` -> `00 00 00 00` after one Quick
  Training. §10.6 has the mechanism. A round-trip test whose input is the
  identity element tests nothing, and this is what that costs.
- **Trigger: a live signal, no restart** (SJ-12). The OSD row is
  `"T[15],Quick Training;"` at the top of `vendor/Menu_MiSTer/menu.sv`
  `CONF_STR` (bit 15 verified free: RTL reads only `[4] [5] [8:6] [9] [12]
  [28:25] [32] [36:33] [38:37] [42:39] [46:43]`, the wrapper reads none of 15;
  `cfg[15]` is a different wire, the native-video menumask). The two ranges in
  bold-free type here — `status[28:25]` and `status[46:43]`, the `h_offset` /
  `v_offset` inputs to the video instance — were MISSING from this list as
  originally written. Neither contains bit 15, so the conclusion stands, but
  the list was not the whole set: `grep -n 'status\[' vendor/Menu_MiSTer/menu.sv`
  is, and it returns 12 lines — eleven reads plus one comment (`// "Full"
  (status[12]=1) sends ARX=0/ARY=0 ...`) — against the nine originally
  listed. The wrapper intercepts the OSD
  select in `tools/mister-wrapper/main-mister-full-menu.patch`
  (`bit == 15 && p[0] == 'T'`), calls the new
  `thirdsarm_wrapper.cpp -> quick_training_signal()` — which raises
  `SIGRTMIN+5` (`kRuntimeQuickTrainingSignal`) at the child, following the
  four existing runtime signals, with **no** restart / argv / handoff file —
  and then **dismisses the OSD itself** (`menustate = MENU_NONE1`; Watch
  Replays gets dismissal free from its restart, this one must do it
  explicitly). Game side: `main.c -> on_shutdown_signal` sets a flag drained
  by `handle_signal_requests` -> `QuickTraining_Request()`, and
  `QuickTraining_Tick()` runs from `game_step_0` before the pad latch (the
  `NetplayNav_Tick` slot, so it owns the pads while a sequence runs). The
  `--direct-p2p-handoff` restart path is deliberately NOT used (§7) — this is
  a live in-process jump.
- **The signal had to be made non-fatal (2026-09-05).** The default action for
  `SIGRTMIN+n` is **terminate** — real-time signals have no default-ignore
  disposition (POSIX.1-2017 XSH 2.4.3; `signal(7)`). `quick_training_signal()`
  sends on `g_child_pid > 0` alone, which is true from the instant `fork()`
  returns, so an OSD press could kill the runtime in two windows: **boot**,
  because the game installed its handlers only inside `MAIN_PHASE_INIT`, past
  `ConsoleMode_Enter()`; and **version skew**, a new RBF + new wrapper driving an older `MiSTer_3S-ARM` game binary
  with no handler for the signal at all. Two changes, because neither covers
  the other:
  - the wrapper sets `SIG_IGN` on all five runtime signals in the CHILD before
    `execve()`. `SIG_IGN` is the one disposition `execve()` preserves, so the
    ignore survives into the game and the game's own `sigaction()` overrides it
    the moment it installs a handler. This is the only half that can help an
    old game binary. The shutdown signals are deliberately excluded —
    `SIGTERM` is how the handoff paths ask the child to exit.
  - the game splits `install_runtime_signal_handlers()` (SIGUSR1 +
    `SIGRTMIN+2..+5`) out of `install_shutdown_signal_handlers()` and installs
    it at the TOP of `loop()`, before the phase machine. The shutdown set stays
    where it is on purpose: those already default to terminate, so there is no
    window to close, and catching them earlier would be worse —
    `MAIN_PHASE_COPYING_RESOURCES` can block in a modal dialog that a
    flag-setting handler cannot end but the default action can.

  - the restore on the way out (`restore_shutdown_signal_handlers`) puts the
    five runtime signals back to **`SIG_IGN`, not `SIG_DFL`**. `SIG_DFL` for
    these is *terminate*, and that restore runs before
    `if (console_mode_entered) ConsoleMode_Exit()` has taken the MiSTer VT out
    of `KD_GRAPHICS` — so an OSD press in between would have killed the process
    with the console unrestored, reopening the same hole. The window is new:
    the restore used to be reached only when `MAIN_PHASE_INIT` completed, and
    `runtime_handlers_installed` is now set unconditionally at the top of
    `loop()`, so every early exit passes through it. The shutdown signals still
    restore to `SIG_DFL`.

  **What the boot window is NOT.** An earlier revision of this section, and of
  the comment on `install_runtime_signal_handlers()`, blamed
  "`Resources_Check()`'s SHA256 sweep of every resource file". That is wrong
  twice. `CMakeLists.txt` gates `CHECKSUM` on
  `$<AND:$<CONFIG:Release>,$<NOT:$<OR:$<BOOL:${PORT_MISTER}>,...>>>`, so
  **`PORT_MISTER` excludes it** — on the MiSTer build `Resources_Check()` is a
  `file_exists()` and `return true`. And where `CHECKSUM` *is* on it hashes
  exactly one file, `Resources_GetAFSPath()`, not every resource. The window is
  real; the reason was invented. What it actually contains is everything
  `main()` runs before `loop()`, then `ConsoleMode_Enter()` — and, when the
  resources are absent, the whole of `MAIN_PHASE_COPYING_RESOURCES`, which
  blocks in a modal dialog indefinitely.

  A corollary of installing these earlier: the handlers are now live across
  `Resources_Check()`'s `SDL_ReadIO` loop, which breaks on `bytes_read <= 0`,
  so an `EINTR` would silently truncate the hash. Unreachable on MiSTer for the
  `CHECKSUM` reason above, but live on a Linux desktop Release build, so
  `install_runtime_signal_handlers()` sets `SA_RESTART`. The shutdown set
  deliberately does not — there the interruption is how a blocked syscall gets
  to notice `shutdown_signal`.

  **Neither half is confirmable on the host**: `#ifdef SIGRTMIN` is false on
  macOS, so the whole `SIGRTMIN` block compiles only for Linux/MiSTer, and the
  wrapper is ARM-only. Host builds prove it compiles and changes nothing there;
  a device run is what would prove the delivery.

**Boundary of what is verified.** Host only: the chain, the phase machine, the
edge cases, the wipe sequencing, and the persistence round-trip are all
host-verified (Debug, `SDL_VIDEODRIVER=dummy`), and both host Debug and host
Release compile clean. Read "the persistence round-trip" with §10.6: as
originally run it proved less than it looked like it proved. **The OSD row, the wrapper interception + dismissal,
and the SIGRTMIN+5 delivery are compile-/patch-verified only** — the menu.sv
row, the wrapper C++, and the signal path cannot be exercised without the
MiSTer hardware, and the ARM build + device deploy were out of scope for this
lane. The patch was confirmed to apply cleanly against the pinned upstream
`menu.cpp` (`3380931329b8...`). A device run is still needed to confirm
end-to-end OSD behaviour.

### 10.5 [SJ-26] `WipeLimit` is shared, and a cover does not stop it

Found 2026-09-02, after §10.3 shipped, from a parallel investigation into
driving the wipe from a replay viewer. It applies here too, narrowly.

`ui/sc_sub.c` -> `WipeOut` / `WipeIn` and `system/sys_sub.c` ->
`Switch_Screen_Init` / `Switch_Screen` / `Switch_Screen_Revival` operate on
**one** module-level counter, `WipeLimit`, which the engine's own transitions
already drive (`Switch_Screen_Init` call sites throughout `game.c`, plus
`Sel_PL_Cont_*`, `Game_Manage_2_1`). Two consequences:

1. **`Switch_Screen_Init` resets the counter.** It calls `WipeInit()`
   (`WipeLimit = 0`) and also overwrites `Forbid_Break`, `Gap_Timer`,
   `Stop_SG`, `Escape_SS`. Starting a Quick Training wipe on top of an engine
   transition restarts that transition's wipe from zero and stomps its flags.
2. **A cover suppresses the drawing, not the counter.** In `WipeOut`, the
   `if (!No_Trans)` guard wraps only the `njDrawPolygon2D` loop —
   `WipeLimit += 1;` sits *outside* it. So an engine wipe keeps advancing
   while `No_Trans` is held.

**In netplay this class is a desync**, because `Exec_Wipe` is in the save set
(`GS_SAVE(Exec_Wipe)` / `GS_SAVE(Exec_Wipe_F)`, `game_state.c`) and gates
effect routines -> RNG consumption. **That consequence does not reach Quick
Training**: `qt_refusal()` rejects netplay sessions, direct-P2P orchestration,
netplay nav, replay playback and the shuffle viewer, and the tick aborts if a
session activates mid-sequence. The reachable damage here is an offline engine
transition being silently retimed.

**Fix: defer, don't refuse.** `QuickTraining_Tick` now holds the request while
`Exec_Wipe != 0` rather than consuming it, bounded by `QT_DEFER_MAX_FRAMES`
(240). Refusing was rejected: the row is the first entry in the OSD, and a
press that silently did nothing would read as a bug.

**Correction, 2026-09-05: the bound must expire into a START, not a drop.** As
first written the deadline set `qt_request = false` with only an `SDL_Log` —
which reproduces the very outcome the deferral exists to avoid, four seconds
later and with the evidence only in a log file the player cannot see. The
wrapper has already closed the OSD by the time the game sees the signal, so a
drop *is* "the menu shut and nothing happened". Starting over the transition
is measurably recoverable — `Switch_Screen_Init` rewrites every field it
stomps, and the sequence normalizes `Forbid_Break`/`Stop_SG` at `QT_WIPE_IN`
(as does `qt_fail()`). So the deadline now falls through and starts the jump.

The deadline is also **measured unreachable** on every path exercised so far:
15 runs, N ∈ {20..240}, PASS-frame minus N exactly 302 every time. It converts
a hypothetical silent drop into a hypothetical visible jump; the residual is a
retimed transition.

**Unproven, and recorded as such:** that "a transition still holding
`Exec_Wipe` after 240 frames is broken on its own account". The engine's own
wipes are tens of frames, but the training-pause exit in `menu.c` case 2 sits
under `if (Check_Pad_in_Pause(task_ptr) == 0)` and was not chased to a
conclusion, so a legitimate long hold has not been ruled out. The deadline is a
choice between two bad outcomes on a path nothing has reached, not a claim
about the engine.

**Also recorded, because it is easy to assume otherwise: there is no
checkerboard wipe.** `WipeOut` offers exactly two shapes — type 0 is a
28-band horizontal venetian blind (`for (i = 224; i > 0; i -= 8)`), and any
non-zero type is a 76-band diagonal (`for (i = -224; i < 384; i += 8)`, with
`wipe_p[2].x = 224.0f + wipe_p[0].x` doing the skew). Quick Training uses
**type 1, the diagonal**, confirmed by the maintainer on a live host run
2026-09-02. The `hnc_wipe*` table is the "Here Comes A New Challenger"
banner, not a screen wipe.

### 10.6 [SJ-27] The chain skipped the character-select EXIT's engine work

Found by review 2026-09-05 from a maintainer report of "the dummy parries at
random", and measured before and after. **`SceneJump_ExecuteTrainingChain()`
replicated character select's SCENE work and omitted two of its ENGINE steps.**
Both were needed; fixing either alone leaves a visible bug.

**(1) The training config was never loaded — and was then destroyed.** The
chain set `mpp_w.initTrainingData = true`, but the only consumer of that flag
is `menu.c` -> `Default_Training_Data(0)`, whose only caller is `sel_pl.c` ->
`Switch_Work` case 1 — character select, which the jump skips. So nothing
called `TrainingConfig_Load()`. The match then ran on a zeroed `Training[]`,
and the training-menu dismissal the chain drives reaches `menu.c` ->
`Setup_NTr_Data()`, whose **first statement is `TrainingConfig_Save()`** — which
flushed those zeros over the user's file. Measured on a seeded scratch home:

```
seeded  contents[0][0][0..3] = 02 05 01 02   (ACTION=JUMP, GUARD=RANDOM
                                              PARRYING, QUICK STAND=ON,
                                              STUN=NO GAIN)
after one --test-quick-training=60 run:   00 00 00 00
```

Fixed by calling `Default_Training_Data(0)` in the chain, in character
select's own position (after `Present_Mode = 4`, which its body reads).

**(2) `init_omop()` never ran early enough, so the engine DIP tables were
zero.** This is what made the dummy appear to parry. `pls00.c` ->
`process_damage()` opens with `if (wk->wu.routine_no[3] == 0)` and nests inside
it an `if (!(wk->spmv_ng_flag & DIP_SEMI_AUTO_PARRY_DISABLED))` that rewrites
`routine_no[2]` **4->31, 5->32, 6->33, 7->34** — 4/5/6 are the ground-guard
states `hitcheck.c` -> `defense_ground_cps3()` selects and 7 the air-guard
state, 31-35 the parry states — so with that bit clear every guard became a
parry, and `set_guard_status()` suppressed the guard spark on top.

The bit is set by `sysdir.c` -> `init_omop()` and reaches the players through
`plcnt.c` -> `set_base_data()`, which copies `omop_spmv_ng_table[]` into
`plw[].spmv_ng_flag`. The stock path calls `init_omop()` at `sel_pl.c` ->
`Exit_6th`, **before** the round boots. The jump path never did.
`menu.c` -> `Normal_Training`'s `init_omop()` does run on the jump path, but a
scene too late: `set_base_data()` had already copied the zero table, and
`effe3.c` -> `effect_E3_move()`'s tail
(`omop_spmv_ng_table[id] = mwk->spmv_ng_flag`) then wrote those stale flags
back over the freshly-correct table — measured `omop0` going `0x0B2CE8E0` ->
`0x00000000`. `effe3.c` repairs only bits 6/7 and only for the dummy, which is
why the dummy read `0xC0` and the player `0x00000000`.

**The blast radius was wider than the dummy.** Measured `plw[0]`:

| | `spmv_ng_flag` | `spmv_ng_flag2` |
| --- | --- | --- |
| jump path, before | `0x00000000` | `0x000D0000` |
| jump path, after | `0x0B2CE8E0` | `0x03FF002E` |
| stock character select | `0x0B2CE8E0` | `0x03FF002E` |

i.e. the match had no air-guard, auto-guard, auto-parry, anti-air-parry,
absolute-guard, chip-damage, wall-jump, air-jump, air-recovery or
air-knockdown restrictions, and special-to-special cancel, all-normals-
cancellable, SA-to-SA cancel and chain combos were all **enabled**. Quick
Training was not running the shipping engine. The dummy lands the same way
(`0x0B2CE070`/`0x03FF002E` on both paths with the same config), the difference
being `effect_E3_move`'s guard-setting writes, which were enumerated rather
than taken as a range: its bit-level `spmv_ng_flag` writes land in bits
{4, 6, 7, 8, 9, 10, 11} and its `spmv_ng_flag2` writes in {9, 16, 18, 19, 26},
plus one whole-word restore of the `master_ng_flag`/`master_ng_flag2` snapshot
`effect_E3_init()` took from the same fields.

**Fix: `init_omop()` in `SceneJump_EnterBattleScene()`**, which is the
`Exit_6th` slot exactly — loads drained, scene about to flip — and therefore
ahead of `setup_base_and_other_data()` / `set_base_data()`. That also defuses
`effect_E3_move`'s write-back without touching it: with the table correct
before the copy, the value written back is the correct one plus the training
settings, which is what it is on the stock path. **This last part is reasoning,
not something under test** — the harness reads `plw[]`, and
`omop_spmv_ng_table[]` itself is never asserted.

**What the harness now holds**, so neither can regress silently
(`--test-quick-training`, `src/quick_training.c`):

- both players' `spmv_ng_flag`/`flag2` carry the bits `effect_E3_move`
  provably cannot reach (DIP1 bits 5/13/24/25, DIP2 bits 1/2/3/20/21), all of
  which were zero before the fix. **What that pins is a CONFIGURATION, not a
  parity.** Every one of those bits is conditional inside `sysdir.c` ->
  `get_system_direction_parameter()` on a `system_dir[N].contents[p][i] == 0`
  test, or — for `DIP_SEMI_AUTO_PARRY_DISABLED` — on
  `omop_guard_type[extra_option.contents[0][3]]`, whose index-2 entry does not
  carry that bit at all. Both `system_dir[1]` and `save_w[]` are persisted
  (`savesub.c` -> `serialize_sysdir` / `serialize_settings`), so a home whose
  SYSTEM DIRECTION or EXTRA OPTION settings differ from `Dir_Default_Data`
  fails this spuriously; and a defect that runs `init_omop()` with the wrong
  inputs (it branches on `Mode_Type`, `Demo_Flag`, `Present_Mode` and
  `Direction_Working[]` to choose the slot) still passes whenever the wrong
  slot happens to agree with the defaults;
- the live `Training[0].contents` equals a SNAPSHOT of the on-disk block taken
  at frame 0, and the file still equals that snapshot at the end. Both halves
  are needed and the snapshot is load-bearing: comparing the live globals to a
  *fresh* read at the end passes on the broken build, because the broken build
  saved its zeros and the two zeroed things then agree. Measured — that is
  exactly how the first attempt at this assertion went green on a deliberately
  broken tree;
- `My_char`/`Super_Arts` equal the snapshot too, for the same reason
  (`TrainingConfig_Save` writes them out of the live globals).

**`--test-instant-jump` is unchanged**, before and after, to the frame:
`jump@43 entered@67 live@105 verified@285 — drain=24, jump->live=62`. The spike
asserts liveness and frame accounting, neither of which this fix moves; it was
verifying a non-shipping engine configuration and reporting PASS, which is the
gap §10.6's assertions close.

### 10.7 [SJ-28] The harness class that eats the maintainer's training file

The `Setup_NTr_Data()` write in §10.6 is not Quick Training's alone. **Every**
`--test-enable` session that reaches the training pause menu hits it, and every
one of them dictates its own characters, arts, stage and training slots — so
the save writes the *harness's* selection over the user's. Quick Training is
the exception only because it takes its selection off the same file it is
about to write back. Measured 2026-09-05 against seeded hermetic homes:

```
--test-instant-jump                     super_arts 01 02 -> 00 00
                                        my_char    0b 00 -> 03 02
--test-enable --test-scene-preset training-yun-ryu-ryu-stage
                                        cursor  00 00 00 00 -> 06 05 01 02
                                        arts             .. -> 02 00
```

This is pre-existing and it destroyed a real config. **Fixed at the harness**,
not by asking callers to remember an env var: `TrainingConfig_Save()` refuses
the write under `#if defined(DEBUG)` for any test session that owns the
selection (`training_config.c` -> `test_session_owns_training_config()`),
which is `configuration.test.enabled` minus Quick Training. A per-flag opt-in
was rejected for the reason the gate's harness discovery exists — it makes
safety something the next harness author has to remember, and the discovery
rule (an `OPT_BOOLEAN` whose help says it runs and exits) structurally cannot
see a session-owning flag like `--test-instant-jump`. Quick Training stays
exempt because suppressing its save would make both halves of its
byte-identity assertion pass on an empty write.

The spike now runs inside the `quick-training` gate for the same seeded-home
config diff, which is the only runner it can have.

## Appendix A — reproducing the measurements

Sizes in §2.2 came from compiling a probe with the project's real flags:

```sh
DEF=$(grep '^C_DEFINES'  build/host/CMakeFiles/3s-arm.dir/flags.make | sed 's/^C_DEFINES = //')
INC=$(grep '^C_INCLUDES' build/host/CMakeFiles/3s-arm.dir/flags.make | sed 's/^C_INCLUDES = //')
# probe.c: #include "netplay/game_state.h" + printf("%zu", sizeof(GameState));
eval clang -std=gnu11 -arch arm64 $DEF $INC probe.c -o probe && ./probe
```

For the ARM32 number without a full toolchain, use the redeclaration trick the
`game_state.c` comment itself documents:

```c
#include "netplay/game_state.h"
extern char probe[sizeof(GameState)];
extern char probe[1];   /* error message reveals the real size */
```
compiled with `clang --target=arm-linux-gnueabihf -fsyntax-only`.

The §4.3 numbers come from running the spike (command in §4.1) and from
`--ldreq-trace <csv> --ldreq-trace-frames N` on the
`training-yun-ryu-ryu-stage` preset (columns include `ldreq_clear` and
`pl_load` per frame).

## Appendix B — provenance

The 2026-08-29 research was done by parallel subagents plus direct
verification; its Appendix B flagged §4's chain table — assembled from one
agent's trace — as the highest-value / highest-risk content, to be re-verified
before building against it.

That re-verification happened in this revision (2026-09-02, at `762b5052`):
every §4 row was re-read directly in source, two content errors were found
and recorded (SJ-16: step 7 is not async; SJ-18: step 12 is the Reset_Replay
path), and the chain was then executed for real by the §4.1 prototype. The
§4.1/§4.3 measurements are first-hand from that prototype's logs and a
screenshot of the live match, not agent-reported.
