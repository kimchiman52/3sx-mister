# Score in training mode

Training mode shows the score readout. Retail PS2 does not.

This is a **deliberate divergence from stock**, unconditional and not
toggleable — there is no config key and no training-menu row. Maintainer
decision, 2026-09-03.

Citations here are **symbol-first**: grep the symbol. Any line number is a
timestamp, not an address, and is not maintained (see `AGENTS.md`).

## The change

One early return deleted from `system/sys_sub.c` -> `Score_Sub()`. That guard
was the entire suppression mechanism; nothing else was added.

## Why it was one line

Only the **draw** was ever suppressed. Everything else already worked in
training and needed no change:

- **Accumulation was never gated.** `engine/cmb_win.c` -> `SCORE_PLUS()` has no
  training branch (its only condition is `ArcadeBalance_IsEnabled()`, for caps
  and column choice), and neither does the chain feeding it —
  `combo_control()` -> `check_and_set_combo()` / `hit_combo_check()` /
  `super_arts_finish_check()` -> `combo_window_push()` -> `SCORE_PLUS()`. That
  chain demonstrably runs in training: it also drives the combo counter and
  calls `training_disp_data_set()`.
- **The call site already ran.** `game.c` -> `Game2_1()` calls `Score_Sub()`
  inside the same `Disp_Cockpit` block that draws the vital/SA/stun gauges and
  names, which are visible in training.
- **The font was already resident.** `score8x16_put` -> `scfont_sqput`
  (`ui/sc_sub.c`) is the same atlas path the combo "PTS" digits use, and those
  already draw in training. No CG or texture work.
- **Row 0 was free.** Training overlays start lower — attack data at y=48/58/68
  (`sc_sub.c` -> `Training_Data_Disp()`), input history at `line_y_base = 58`
  (`sc_sub.c` -> `draw_training_input_history()`).

## Provenance: stock behaviour, not a port decision

The guard arrived with the function's original matched decompilation of the PS2
binary — upstream `468c38b8` ("SYS_sub.c (#212)") replaced the
`INCLUDE_ASM(... Score_Sub)` stub with C containing
`if (Mode_Type == 3 || Mode_Type == 4) { return; }` (3 and 4 being the two
training modes). Every later touch was mechanical: `50413821` renamed the
condition to `Is_Training_Mode(Mode_Type)`, `a752e2ca` renamed
`operator` -> `wu_operator` and introduced `TopHUDPriority`.

So this is **adding** something retail never had, not restoring something the
port dropped. The CPS3 arcade board has no training mode at all, so there is no
arcade precedent in either direction.

## Two behaviours that look like bugs and are not

**1. Training scores read low.** They are combo, first-attack and reversal
points only. Round-end bonuses never accrue, because `engine/manage.c` ->
`Game_Manage_7_1()` short-circuits on KO in training
(`if (Is_Training_Mode(Mode_Type)) { C_No[0] = 12; End_Training = 1; return; }`)
before `Game_Manage_8_0()` runs `Pool_Score` / `Additional_Bonus`, and before
`grade.c`'s `Score[WGJ_Target][...] += bonus_point`. Do not "fix" this by
re-gating the display.

**2. The number persists across dummy KOs.** The training KO-reset branch in
`manage.c` zeroes only `Score[0][2]` / `Score[1][2]`, so the displayed column
keeps accumulating within a session. It clears on character change, via
`game.c` -> `Before_Select_Sub()` -> `Clear_Personal_Data()`.

A CPU dummy's side shows nothing, because of the per-player
`wu_operator == 0` `continue` inside `Score_Sub`'s loop. That is the same rule
arcade uses, and is not training-specific.

## Why no toggle

A config key was considered and rejected as unnecessary — the maintainer wants
it always on. Recorded so nobody adds one back reflexively.

A training-menu row was also priced and rejected as disproportionate: the
FRAME DATA row (`118f7350`) consumed the last free slot in both
`TrainingData.contents` (7/7, and `training_config.c` requires it to match the
first 7 columns of `Menu_Max_Data_Tr`) and `effect/effa3.c` ->
`Letter_Data_A3` row 6 (9/9). A new row would force a struct widening with its
`_Static_assert`s, a `TrainingConfigFile` **version 2 -> 3 migration**, and
cursor-bound changes across roughly ten files.

## Netplay

Not a concern: training is unreachable in netplay. `src/netplay/` sets
`Mode_Type = MODE_NETWORK` and never references `MODE_NORMAL_TRAINING`,
`MODE_PARRY_TRAINING` or `Is_Training_Mode`. `Score_Sub` consumes no RNG, and
`Score` / `Keep_Score` / `Stop_Update_Score` / `Disp_Score_Buff` were already
in the rollback save set before this change.

Nothing leaks into rankings or a save file. `Check_Ranking()` is called only
from `screen/entry.c`'s name-entry (arcade game-over flow, unreachable from
training), and `Before_Select_Sub()` -> `Clear_Personal_Data()` zeroes
`Score[PL][0]` and `Continue_Coin` before any new session. This was already
true before the change — the score was being accumulated either way.
