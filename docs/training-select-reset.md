# Training-mode SELECT reset

Pressing **SELECT** during training snaps both characters back to the
round-start position — no black wipe, no BGM restart, no round transition.

Implemented in `src/sf33rd/Source/Game/menu/menu.c`
(`Tr_Reset_Check` / `Tr_Reset_Apply` / `Tr_Reset_Read_Input` /
`Tr_Reset_Release_L8` / `Tr_Reset_Finish_Teardown` /
`Tr_Reset_Position_Override` / `Tr_Reset_Clamp_To_Field`), hooked at the top of
`Wait_Pause_in_Tr` ahead of `Control_Player_Tr` — plus one call to
`Tr_Reset_Position_Override` from `plcnt_init` (`engine/plcnt.c`).

Citations here are **symbol-first**: grep the symbol. Any line number is a
timestamp, not an address, and is not maintained (see `AGENTS.md`).

## What ships today

Location (centre / left / right) and swap (original sides / sides swapped)
are two independently latched axes, not one flat preset list —
`Tr_Reset_Location` and `Tr_Reset_Swapped`, both `menu.c` file-statics. A
third latch, `Tr_Reset_Last_Horiz`, records the horizontal of the most recent
horizontal-carrying reset, and is what "the same direction again" is measured
against.

| Input | Location | Swap | Near/wall side | Far side | Camera |
|---|---|---|---|---|---|
| SELECT, no direction | repeat last | repeat last | — | — | — |
| SELECT + ↓ (pure down) | centre | **cleared** | — | — | stage default |
| SELECT + ↑ (pure up) | unchanged | **set** | — | — | unchanged from current location |
| SELECT + ← | left corner | preserved on a repeat, else cleared | P1 or P2, by swap | touching | `bgw[1].l_limit2` |
| SELECT + → | right corner | preserved on a repeat, else cleared | P2 or P1, by swap | touching | `bgw[1].r_limit2` |
| SELECT + ↓← | left corner | exactly as bare ← | | touching | `bgw[1].l_limit2` |
| SELECT + ↓→ | right corner | exactly as bare → | | touching | `bgw[1].r_limit2` |
| SELECT + ↑← | left corner | **set** | P2 cornered | P1, touching | `bgw[1].l_limit2` |
| SELECT + ↑→ | right corner | **set** | P1 cornered | P2, touching | `bgw[1].r_limit2` |
| START + SELECT | Unchanged: soft reset (`Check_Reset_IO`) | | | | |

"Preserved on a repeat" is the rule in `Tr_Reset_Read_Input`, and it has two
conjuncts: the swap bit survives a horizontal press **iff** the location
before that press was not centre **and** the horizontal pressed equals
`Tr_Reset_Last_Horiz`. So:

- pressing the same corner twice keeps whatever sides it had;
- pressing the opposite corner resets to original sides;
- either horizontal taken from centre resets to original sides, whatever
  `Tr_Reset_Last_Horiz` holds.

Worked example, the sequence this was specified against: SELECT+→ (right
corner, original sides — the location was centre, so the swap bit clears),
then SELECT+↑ (right corner, swapped: P1 on the right wall, P2 to its left),
then SELECT+→ again (the location was RIGHT, not centre, and
`Tr_Reset_Last_Horiz` is already RIGHT, so the swap **survives**) — P1 stays
on the right, P2 on the left.

**Every input carrying a horizontal bit updates `Tr_Reset_Last_Horiz`, ↑← and
↑→ included.** They are corner presses like any other, and leaving the latch
alone for them would let the very next SELECT+→ clear the swap that SELECT+↑→
had just asked for — a repeat of the same physical direction silently undoing
itself. ↓ does not touch the latch, and provably cannot need to: ↓ parks the
location at centre, and the not-centre conjunct alone forces the next
horizontal to clear the swap bit whatever the latch holds. (Clearing it on ↓
would therefore be behaviourally identical; not writing it is one fewer
write.)

At centre the arrangement is still the fixed spacing plmv_1020 itself uses:
`centre ∓ 88` (`centre` is `get_center_position()` — 512 on most stages, 464
on stages 0 and 19; 88 is the `step` `player_mv_1000` passes `plmv_1020` on
the `APPEAR_TYPE_NON_ANIMATED` path, which is the one training takes). The
corners do **not** use this spacing — see *Corners: what is computed and
what is not* for the touching-distance derivation, which is BUG 1's fix.

Directions are **screen-absolute**, not relative to whoever pressed SELECT:
← is always the left corner.

**Diagonals resolve to their horizontal component.** Each direction is tested
as a bit, not as an exact word, so ↓↙ / ↓↘ are the corner and ↑↖ / ↑↗ are the
corner *and* the swap; only a pure vertical — no horizontal bit at all —
reaches the centre and swap branches. Up is tested before down inside the
horizontal branch, so a pad reporting both resolves to the up-diagonal.

*Why the horizontal, and what the bit-test rationale does and does not
establish.* The reason for testing bits rather than exact words is that
down-back and down-forward are the ordinary resting stick positions in
training: an exact `held == SWK_DOWN` would swallow the reset for most of
what a player is actually holding. That argument establishes only that a
diagonal must map to **something** rather than be ignored. It does not favour
either component, and in particular it was never an argument for the
vertical: the stick can be at down-back anywhere on the screen, so there is
no position the vertical mapping protects and no trade being made. The
horizontal is what the feature wants — a player crouch-blocking in the corner
who taps SELECT is asking to stay in the corner — so that is what ships.
**Changed 2026-09-07** (see *Increment 5*); before that date the diagonals
resolved to their vertical component and down was tested first.

↓ remains fully **absolute**: it sets centre and clears the swap bit, so ↓
always gives "original sides at centre" no matter what came before. ↑ is the
one axis that touches neither the location nor `Tr_Reset_Last_Horiz` — "same
location, swapped", which is BUG 2's fix; pressing ↑ twice in a row is
idempotent, not a toggle. The horizontals are now neither fully absolute nor
fully relative: the location is absolute, the swap bit follows the repeat
rule above. Bare SELECT repeats the latched (location, swap) pair — see
*Netplay safety*.

**Charge state survives the reset**, for both players — see *Charge state is
preserved* under *Smaller things worth knowing*.

The reset plays the menu confirm one-shot, `SE_selected()`
(`SsRequest(98)`, `sound/sound3rd.c`), on the frame the teardown runs. That is
deliberately earlier than the visual settles — see the frame sequence below —
so the player gets immediate confirmation instead of waiting out the snap.
It is safe mid-round rather than menu-only: `Setup_Pause` and `Setup_Come_Out`
(`system/pause.c`) already call it during live gameplay.

## When it will not fire

Gated in `Tr_Reset_Check`. All of these must hold:

- `Is_Training_Mode(Mode_Type)` — normal or parry training. Excludes
  `MODE_NETWORK`, so the feature is netplay-unreachable by construction.
- `task_ptr->r_no[1] < 2` — not while the pause menu or the
  controller-disconnected screen is up.
- `Allow_a_battle_f != 0` — the round is live. This closes at K.O. with zero
  frames of slack: `Game2_1` runs `Player_control` (which sets
  `Conclusion_Flag`) before `Game_Management` (where `Game_Manage_3rd` clears
  the flag) in the same frame, and the menu task reads it the frame after.
- `Extra_Break == 0` — not during a super-art screen flash, which
  `effect_77_move` raises for 64 frames.
- `(Game_pause & 0x7F) == 0` — not during a round message. **Masked, not
  compared to zero:** `cpLoopTask` (`src/main.c`) ORs in bit 7 under
  `#if defined(DEBUG)` whenever `sysSLOW` is active, and DEBUG is the flavour
  used for on-device testing. A whole-byte comparison would make the feature
  silently dead in exactly the build you would test with.
- `Play_Mode == PLAY_MODE_NORMAL` — blocked in both RECORD and REPLAY. A
  reset is not captured in the 12-bit recording, so a take spanning one would
  replay against different positions than it was recorded at.
- START held on **either** pad disqualifies the frame.

SELECT on the dummy's pad is inert unless the dummy is set to HUMAN —
`Game_Manage_1st` clears `wu_operator` on the dummy in training. That matches
the existing pause path.

## How it works

The reset drives the engine's own start-position path rather than inventing
one. Clearing `routine_no` makes `Player_move` dispatch to `player_mv_0000`,
which re-runs the round-start clears (vitality, hit stop, throw flags, stun,
ukemi, super-art state, and the per-round S.A.GAUGE training option) and hands
off to `player_mv_1000` → `plmv_1010` → `plmv_1020`. Training's appear type is
`APPEAR_TYPE_NON_ANIMATED`, so the step is 88 and `plmv_1020` already writes
exactly the centre preset with the correct `rl_flag` per side. **The centre
preset needs no position override at all**; the other three overwrite what
`plmv_1020` wrote, on the frame it writes it.

It takes three frames, and the parts do not land together:

| Frame | What happens | What the player sees |
|---|---|---|
| N | Teardown; `player_mv_0000` runs | Effects vanish, bars refill, combo counter clears, camera snaps to stage default — but **the characters have not moved yet**. `SE_selected()` fires here. |
| N+1 | `player_mv_1000` → `plmv_1010` → `plmv_1020`, then `Tr_Reset_Position_Override` | Characters teleport to the preset; for a corner the camera lands the same frame |
| N+2 | `pli_1000` sets `routine_no[0] = 4` | Input comes back |

So there is a one-frame window (~17 ms) where the camera has already jumped but
the characters have not, and input returns one frame after the visual settles.
Whether that reads as a clean snap or a visible hitch is a device question — it
is the first thing to watch for on a TV. The sound firing on frame N is partly
there to cover it.

### Why `pcon_rno` is written

`check_lever_data` (`engine/pls00.c`) is the sole entry point for pad input
into a player, and is itself gated on `routine_no[0] == 4`. The reset chain
runs 0 → 1 → 3 and stops there; `player_mv_3000` is `Player_normal` and
nothing else, and never calls it. The only writers of `routine_no[0] = 4` are
`pli_1000` (`engine/plcnt.c`) and `plcnt_b_init` case 1 (`engine/plcnt2.c`),
both reachable only while `pcon_rno[0] == 0` — and a live round sits at
`pcon_rno[0] == 1`.

So a reset that stops at `routine_no` leaves **both pads and the dummy CPU
permanently inert** until the player exits to the training menu. The
`pcon_rno` write is what hands them back.

`pcon_rno[1] = 2` rather than 0 is deliberate. `appear_initalize[]` is
`{ init_app_10000, init_app_10000, init_app_20000, init_app_30000 }` and
`APPEAR_TYPE_NON_ANIMATED` is 0, so training dispatches to `init_app_10000`.
Its case 0 is `pli_0000` → `SDL_zeroa(plw)` + `setup_base_and_other_data`,
the full round-boot re-init, which would clobber `wu.target_adrs` (dereferenced
every frame by `set_rl_waza`) and re-run the training-only E3/E4 works.
Entering at case 2 skips it and still ends at `pli_1000`.

This is not invented: `Game_Manage_2_3`'s training branch already does exactly
this, down to the `pcon_rno[1] = 2` constant.

### Why the position override runs from `plcnt_init`, not from the menu task

The design doc's §11 step 5 says to override the positions "after `Player_move`
has run for both players". That instruction predates the `pcon_rno` fix and no
longer names a reachable moment from `menu.c`.

`Tr_Reset_Check` is called from `Wait_Pause_in_Tr`, which runs in **`TASK_MENU`
(3), before `TASK_GAME` (5)** — `cpLoopTask` (`src/main.c`) walks `task[i]`
ascending. So the menu task can never see the positions `plmv_1020` wrote in
the same frame. Writing an override from there on frame N+1 is overwritten by
`plmv_1020` later that frame; writing it on N+2 shows the player one frame at
centre before the snap.

The override therefore runs **inside `TASK_GAME`, at the end of `plcnt_init`
(`engine/plcnt.c`), immediately after `move_player_work()`.** That is the only
point in the frame that both sees the new positions and still precedes every
consumer of them:

| Consumer | Reads | Runs |
|---|---|---|
| `add_next_position` | `xyz[0].disp.pos` → `wu.position_x` | later in `Player_control` |
| `check_cg_zoom` | `xyz[0].disp.pos` → `wu.scr_mv_x` (when `scr_pos_set_flag`) | later in `Player_control` |
| `TATE00` | `bg_w.bgw[1]` → camera + every parallax layer | after `Player_control` in `Game2_1` |
| `reqPlayerDraw` → `mtrans.c` | `cg_flip ^ rl_flag` | after `TATE00` in `Game2_1` |

The cost is one call and one predicate test. `plcnt_init` is
`player_main_process[0]`, so it only runs while `pcon_rno[0] == 0` — during a
round appear, never during live play — and the override early-returns unless
`Tr_Reset_Position_Pending`, a `menu.c` file-static that only `Tr_Reset_Apply`
sets, behind the `Is_Training_Mode` gate. It is inert in arcade and netplay by
construction.

Two alternatives were rejected against the code:

- **`Game_Management`** also runs after `Player_control` in `Game2_1`, and
  `Game_Manage_2_3`'s training branch is the precedent for engine surgery from
  there. But it runs *after* `TATE00`, so the camera write would need a
  hand-rolled `bg_pos_hosei2()` + `Bg_Family_Set()` that publishes `bgw[1]` at
  the corner while every other parallax layer still sits at its stage default,
  and `wu.scr_mv_x` would be a frame stale.
- **Suppressing the render for the extra frame** (`disp_flag = 0` through N+1,
  override on N+2 from the menu task) costs a second invisible frame and still
  leaves the camera and the characters landing on different frames.

`Tr_Reset_Position_Pending` fires on the unique state `plw[0].wu.routine_no[0]
== plw[1].wu.routine_no[0] == 3`: `player_mv_0000` leaves both at 1 on frame N,
`plmv_1010` sets 3 on N+1, and on N+2 `pli_1000` has already set 4 by the time
the override looks — `init_app_10000` runs before `move_player_work` inside
`plcnt_init`. That state is *not* unique to a SELECT reset, though — an
ordinary training round appear reaches it too — so `Training_Init` clears the
latch, on the same reasoning as the teardown latch it clears beside it.

### Corners: what is computed and what is not

- **No wall arithmetic for the near player.** `set_field_hosei_flag`
  (`engine/pls02.c`) clamps to `[scrl + satse, scrr - satse]`, with
  `satse[20]` the per-character half-width used only for the *field* wall.
  The override writes X *to the wall* and re-runs the clamp pair in
  `move_P1_move_P2`'s exact shape (`scrr` first, `scrl` only if the first
  reported the player was inside). Writing to the wall rather than past it also
  degrades gracefully if `bg_app` / `bg_app_stop` ever suppress the clamp.
  `near_ix` — which of the two players ends up on the wall — is the only thing
  the swap bit changes about a corner reset; the wall side itself still comes
  from `Tr_Reset_Location` alone.
- **No corner arithmetic for the far player either, and no fixed gap.** An
  earlier revision placed the far player a hardcoded 176 from the clamped near
  one — the same separation the *centre* preset uses — which left the same
  visible gap in the corner as standing at centre (the design doc's own BUG 1).
  `satse` is not the right table to fix it with either: it is the field-wall
  half-width, not the body-to-body one. The real body-to-body distance is the
  per-character, per-animation-cel **pushbox**, `wu.h_hos->hos_box`
  (`charset.c`: `h_hos = hosei_adrs + cg_ja.hoix`, i.e. ROM data selected by
  the current sprite cel, not a constant this code could hardcode or derive
  from source alone — same category as the `cg_zoom` data noted under
  *Unverified in simulation*, below).

  `check_body_touch` (`engine/pls02.c`) is the engine's own body-collision
  push-apart, and it already runs every frame — including during an ordinary
  appear, so this is not new machinery, just reused earlier. But calling it
  as-is is not safe from here: it applies `meri_case_switch` to damp the push
  into a gradual per-frame shove (correct for a live hit, wrong for an instant
  snap — it would take several visible frames to fully separate), and its
  one/two branch selection is gated on the `ichikannkei` global, which
  `move_player_work` computes once per frame **before** `Player_move` runs —
  so on this exact frame it still reflects the *pre-reset* positions and
  cannot be trusted for where the corner is placing these two now.

  The override sidesteps both problems rather than reimplementing them: which
  player is near and which is far is already known from `Tr_Reset_Location`
  and `Tr_Reset_Swapped`, so `ichikannkei`'s branch logic is not needed at
  all. It sets `rl_flag` for both players from that known geometry (the wall
  player always faces the far one — `set_rl_waza` has not run for these
  positions yet, and `hit_check_subroutine` mirrors each pushbox off
  `rl_flag`), places the far player exactly on top of the near one, and calls
  `Tr_Reset_Body_Separation` (`menu.c`, a local helper, see Increment 4) — the
  **raw**, undamped overlap depth, from the same X/Y arithmetic
  `check_body_touch` uses before applying `meri_case_switch`. **Not
  `hit_check_subroutine` directly**: that function collapses its two
  candidate separations to `min(d2, d3-d2)`, which is only correct for a
  live, lightly-interpenetrating hit; at this override's zero-starting-
  separation stack, a given corner needs a *specific* one of the two
  candidates, not whichever is smaller, so `Tr_Reset_Body_Separation`
  duplicates the arithmetic and returns the corner-selected candidate
  instead — see Increment 4 for the bug this replaced and how it was
  verified against real character data. With that selection, the returned
  value *is* the exact pixel gap the two pushboxes need: the far player is
  placed `near ± raw_meri` (away from the wall) in one write, then
  re-clamped to the field. No loop, no multi-frame convergence, no visible
  chase — it lands on the same frame as everything else the override writes.

  `hos_box[0] == 0` is `check_body_touch`'s own "no pushbox this pose" guard,
  copied verbatim; if either box is disabled, or the boxes somehow don't
  overlap at zero separation, the override falls back to the old fixed
  176 spacing rather than leaving the two stacked on the wall. That fallback
  is believed unreachable for a standing appear pose but is not proven —
  see *Unverified in simulation*.

  This makes the touching distance genuinely **character-pair-dependent** by
  construction: it is read fresh from each character's own ROM pushbox data
  every time the corner preset fires, not a constant tuned for one matchup.
  It has now been spot-checked against real character data (a separate,
  read-only ROM audit checkout, not this worktree) — see Increment 4 for the
  full derivation, the standing-pose pushbox values for all 20 characters,
  and why the fix does not change behavior for any of them despite being a
  genuine correctness fix. See *Verification status*.
- **The camera must move before the players.** `set_scrrrl` (`engine/plcnt.c`)
  is the only writer of `scrl` / `scrr` and derives them from
  `get_center_position()`; `Player_control` runs it at the top of the frame,
  against the stage-default camera `compel_bg_init_position` left on frame N.
  So the override writes `bg_w.bgw[1]`, then re-runs `set_scrrrl` itself, and
  only then places the players.
- **A direct `bgw[1]` write, not `compel_bg_init_position`**, which snaps to
  the stage default. Not convergence either: `bg_base_x_move_check` moves the
  base layer at most `bg_w.max_x` (8) per frame scaled by `remake_x_mvstep`
  (× 80/100) — 6 px — so the camera would visibly chase.
- **No non-base parallax layer needs an explicit write.** `TATE00` runs later
  in the same frame; each `BGxxx` mover sets `bgw_ptr = &bg_w.bgw[1]` and runs
  `bg_base_move_common` *before* any other layer, which publishes
  `bg_w.bg2_sp_x2 = wxy[0].disp.pos - pos_x_work`, and `bg_x_move_check` then
  recomputes every other layer **absolutely** from it —
  `wxy[0].cal = xy[0].cal = speed_x * bg_w.bg2_sp_x2` — rather than
  accumulating. Each mover ends with `bg_pos_hosei2()` and a `Bg_Family_Set`
  variant, so the override deliberately does *not* call those itself: doing so
  would publish a half-updated frame with the base at the limit and the rest at
  their stage default.

  This is **half** of the design doc's §12 item 3, not all of it — see the
  next bullet for the part a `bgw[1]` write alone genuinely does not cover.
- **The X chase copy must be cleared, or the camera write is silently
  swallowed.** `bg_pos_hosei2` reads `bgw[bg_no].chase_xy[0].disp.pos` instead
  of `wxy[0].disp.pos` whenever `bg_w.chase_flag & 0xF` is set;
  `chase_xy_move` re-publishes `bg_w.bg2_sp_x2` from `chase_xy[0]` *after*
  `bg_base_x_move_check` published it from `wxy[0]` (`bg_base_move_common`
  order is base-x, base-y, chase); and `bg_base_x_move_check` re-syncs
  `chase_xy[0].disp.pos = wxy[0].disp.pos` only while the flag is **clear**.
  With the flag set, every layer therefore sits on the chase camera — near the
  stage default — while both players are already in the corner and
  `scrl` / `scrr` are already at the limit, until the chase expires and the
  camera snaps.

  It is reachable. `chase_start_check` (`stage/bg_sub.c`) arms
  `chase_flag |= 2`, `chase_time_x = 6` on the frame a zoom request *drops* —
  exactly what a reset taken during a super produces — and
  `compel_bg_init_position` does not clear `chase_flag`. So the flag is
  cleared twice: once in `Tr_Reset_Apply` before `compel_bg_init_position`, so
  frame N's own `bg_pos_hosei2()` publishes the stage default (this covers the
  centre and swap presets too — it was a hole in increment 1), and again in the
  corner branch of the override, because `chase_start_check` runs from `TATE00`
  *after* `Tr_Reset_Apply` and re-arms the settle chase on that same frame N.
  The override clears only the X nibble; `Tr_Reset_Apply` clears both axes,
  matching `bg_initialize`'s `bg_w.old_chase_flag = bg_w.chase_flag = 0;`.
  `old_chase_flag` has no reader anywhere in the tree and is cleared only to
  keep the idiom; `chase_x` / `chase_time_x` are left alone, which is the same
  residue a chase that ends normally leaves.
- The camera cannot drift back off the limit — but **not** because the clamp is
  unconditional. `bg_base_x_move_check`'s whole move-and-clamp block sits
  inside `if (!bg_stop && !bg_app_stop)`; only the `bg2_sp_x` / `bg2_sp_x2`
  publish and the `chase_xy` sync are outside it. Both halves hold anyway: with
  `bg_stop` / `bg_app_stop` set the move is skipped entirely so there is
  nothing to drift, and with them clear both corner presets leave both players
  in the same half of the screen, so the scroll target (`scr_11_20` for the
  left corner, `scr_10_22` for the right) lies past the limit and the clamp
  pins `wxy[0]` / `xy[0]` to it.

  `bg_stop` is genuinely reachable on the override frame: `effect_I3_move`
  (`effect/effi3.c`) sets it at `routine_no[0] == 0` and clears it only at
  case 2, and its work has id 183 while `erase_extra_plef_work` sweeps list 3
  for `0x91` / `0x93` / `0x94` only, so the work survives the teardown. Note
  `bg_app` gates `zoom_ud_check`, not this — the two flags that gate the field
  clamp in `move_P1_move_P2` (and therefore in `Tr_Reset_Clamp_To_Field`) are
  `bg_app` and `bg_app_stop`, which is a different pair.
- **`hosei_amari` is zeroed after the clamps.** `set_field_hosei_flag` records
  `pl->hosei_amari = -hami`, the push-back distance. A walk overshoots a wall
  by a few pixels; a preset that writes X exactly to `scrl` / `scrr` overshoots
  by a whole `satse[player_number]` (24–40 px). `check_damage_hosei`
  (`engine/plcnt.c`) runs later in the same frame and copies it into
  `muriyari_ugoku` unconditionally, and `effect_02_move` adds that to hit marks
  mastered by the player. It is inert today —
  `check_damage_hosei_dageki` needs `dm_hos_flag` and
  `check_damage_hosei_nage` needs `tsukami_f` / `tsukamare_f`, all three of
  which `player_mv_0000` cleared on frame N with nothing able to set them
  during the appear, and `effect_02` reads `muriyari_ugoku` only in its case 0,
  the frame it is created. But list 2 is not one of the lists
  `erase_extra_plef_work` sweeps and `effect_02_move` answers `Suicide[6]`, not
  `Suicide[0]`, so hit marks *do* outlive the reset. One line removes the
  question. `micchaku_flag` / `hos_fi_flag` are left as the clamp set them:
  "pinned to this wall" is exactly true and several consumers rely on it.

### Facing is recomputed, never flipped

`set_rl_waza` (`engine/pls01.c`) derives the desired facing every frame from
relative X alone, and `move_player_work` calls it for both players **before**
`Player_move` — i.e. from the pre-reset positions. `plmv_1020` then hardcodes
`rl_flag` from `wu.id`. So the override re-derives `rl_flag` with
`set_rl_waza`'s own rule applied to the new positions, and sets `rl_waza` to
match so the next frame agrees with this one. Flipping instead is only
accidentally correct for the swap preset and wrong for both corners; without
the `rl_waza` write the neutral stance re-latches `rl_flag = rl_waza` and the
pair render back-to-back for a frame.

`mtrans.c` takes the sprite flip from `cg_flip ^ rl_flag` at draw time and
`reqPlayerDraw` runs later in the same frame, so the write lands on the frame
the characters move, not the one after.

`wu.target_adrs`, which `set_rl_waza` dereferences, survives the teardown —
see correction 6 below.

## Four defects handled, with their evidence

### 1. `Suicide[0]` must be cleared by this code

The effect teardown needs a one-frame `Suicide[0]` pulse so list-5 effects take
their own exit paths — notably the super-art shadow, which clears the global
`SA_shadow_on` only from `effect_J4_move`'s exit branch. `erase_extra_plef_work`
never walks list 5, so without the pulse the stage stays dark for good after a
reset during a super.

But **nothing on an in-round path clears the flag.** `All_Clear_Suicide` is
called only from `Game_Manage_1st`, `Menu_Init`, `Sel_PL_Cont_1st`, `Win_1st`
and `After_Bonus_1st` — all screen/phase entry. `Game2_5` gets away with a bare
set because the round-restart flow later reaches `Game_Manage_2_0` /
`Game_Manage_2_2`, which write `Suicide[0] = 0` explicitly. Leaving it set makes
every effect that reads it suicide on creation.

### 2. The pulse destroys `effect_84`, and it is not transient

`effect_84` is the round-message controller singleton (list 4, id 84).
`erase_extra_plef_work`'s list-4 filters are `0x81` / `0x25` / `0xAC`, so it
never frees it — the pulse is the only thing that does, and it is created only
by `Game_Manage_2_2` and `Game_Manage_12_0`.

It is the only writer of `request_message = 0` outside those screen-boundary
phases, and three phases spin on that clear (`Game_Manage_5_2`,
`Game_Manage_7_5`, `Game_Manage_12_3`). `effect_56_init`, which draws the
message, is called only from `effect_84_move`. Lose the singleton and no
K.O. / TIME UP message renders for the rest of the session, and the
`Game_pause = 1` it raises over the K.O. window stops happening at all.

`Tr_Reset_Finish_Teardown` rebuilds it the frame after the pulse, mirroring
`Game_Manage_2_2`'s clear-then-init ordering and its retry-on-pool-exhaustion
convention (`effect_84_init` returns -1 when `pull_effect_work(4)` fails).

**This is reachable in training, not theoretical.** `Demo_Flag` is nonzero
during training — despite the name, it means "a real credited session is
running" — so `settle_check` does issue `request_center_message(0)` on a
training K.O.

### 3. Makoto's Tanden Renki palette (pre-existing engine bug)

`erase_extra_plef_work` frees the whole of list 6 with
`effect_work_list_init(6, -1)`, which releases each work through
`push_effect_work` **without running its move function**. `effect_L8` (Makoto's
Tanden Renki) latches two ColorRAM rows into its own `frw` slot and restores
them only from `effect_L8_move`'s case 1 — which therefore never runs — and
`push_effect_work` then `SDL_zeroa`s the slot holding the only saved copy.

Makoto stays tinted for the rest of the session, and it does not self-heal: the
next Tanden Renki latches the already-tinted rows as its "old" colours. The
`Suicide[0]` pulse does not help — `effect_L8_move` reads no `Suicide` entry at
all.

`Tr_Reset_Release_L8` walks list 6 and drives L8's own exit path before the
free. Fixed on this side rather than in `erase_extra_plef_work`, which is
shared with `Game2_5`.

**Not introduced here.** `Game2_5` tears down identically, so a stock round
restart during Tanden Renki leaks the same palette. A training reset only makes
it reachable at any moment instead of only at a round boundary.

### 4. Twelve's invisibility strands three brightness fields

`effect_L0_move` case 1 guards its restore with
`dead_f == 0 && Suicide[0] == 0` — inverted relative to the pulse — so the
restore is skipped and the work freed. `disp_flag` is covered by the explicit
write, and `my_col_mode` / `my_col_code` by `set_char_base_data`, but
`my_bright_type`, `my_bright_level` and `my_clear_level` are covered by nothing.

Zeroed in the per-player loop rather than by fixing `effl0.c`'s guard: those
fields live in `plw`, which is `GS_SAVE`'d, so changing the guard would be a
simulation change on the shared arcade/netplay path for a training-only feature.

## Smaller things worth knowing

- **FIRST ATTACK is preserved, deliberately.** `combo_cont_init()` zeroes
  `first_attack` (`engine/cmb_win.c`) along with the rest of the combo/score
  state. That is right at a real round start, but a SELECT reset is not a new
  round — it repositions the players and leaves the round running — so
  re-arming it made the FIRST ATTACK banner fire again on the next hit after
  *every* reset. `Tr_Reset_Apply` now saves the value and restores it around
  that one call. `combo_cont_init` itself is untouched, so every other caller
  keeps the round-start behaviour. Maintainer request, 2026-09-03.

  **The centre preset's FIRST ATTACK preservation** has been held by a test
  since 2026-09-05, and nothing held it before that — a tree-wide
  `grep -rl "Tr_Reset\|select-reset" tools src/test` returned nothing. The
  assertion rides on the Quick Training harness (`--test-quick-training`,
  `src/quick_training.c` -> `qt_test_select_reset_tick`), which is already
  sitting in a live unpaused `PLAY_MODE_NORMAL` round — exactly
  `Tr_Reset_Check`'s precondition set. It plants a sentinel in `first_attack`,
  injects one SELECT edge, waits for the `Suicide[0]` pulse (the positive
  marker that the reset actually fired, without which "preserved" is satisfied
  by a reset that never happened; `Tr_Reset_Apply` sets it and
  `Tr_Reset_Finish_Teardown` clears it on the next frame, so in a live round it
  has exactly one producer), and checks the sentinel both then and once the
  round hands control back. Deleting the restore line fails it.

  **Scope, stated because the name of the section oversells it.** It covers
  one of the four presets. `Tr_Reset_Read_Input` latches a location or a swap
  only when a direction is held, and the harness injects a bare `SWK_BACK`;
  `Tr_Reset_Location` is a zero-init file-static, i.e. `TR_LOC_CENTRE`, so
  `Tr_Reset_Position_Pending = !(TR_LOC_CENTRE && !Swapped)` is false and
  `Tr_Reset_Position_Override` — the corner/swap camera and separation work
  that most of this document is about — never runs. What DOES run for the
  centre preset is all of `Tr_Reset_Apply`, including the `Suicide[0]` pulse,
  `erase_extra_plef_work`, `combo_cont_init` with the FIRST ATTACK
  save/restore, `pcon_rno[1] = 2`, and the
  `bg_w.old_chase_flag = bg_w.chase_flag = 0` clear. It is
  `Tr_Reset_Position_Override`'s own second `chase_flag` clear, on the corner
  branch, that is untested. Corner, swap and the corner-swap combination have
  no test at all.

- **Charge state is preserved, for both players.** Players hold back or
  down-back to build a Sonic Boom / Headbutt-class charge *while reaching for
  SELECT*, and the reset was wiping it.

  *Where the charge lives.* `check_1` (`engine/cmd_main.c`) is the charge
  parser: while the held lever matches `waza_ptr->w_lvr` it counts `free1` /
  `free2` down and raises `uni0.tame.flag` once the count expires, then fires
  on release. Every field it touches is in `waza_work[id][slot]`
  (`engine/cmd_data.h`). The lever samples it reads — `chk_pl->sw_lever`, out
  of `t_pl_lvr[]` — are re-derived from the pad every frame by `sw_pick_up`
  and are not state a reset can lose.

  *What was wiping it.* Exactly one thing on this path:
  `setup_any_data()` → `set_base_data_tiny()` → `cmd_init()` (`engine/plcnt.c`,
  `engine/cmd_main.c`), which clears `waza_work[id]` wholesale and then has
  `waza_compel_all_init` rebuild the per-slot table data. Nothing else in the
  three-frame chain calls `cmd_init`: `player_mv_0000` does not, `pli_1000`
  does not, and `init_app_10000`'s cases 2 and 3 do not — case 0 (`pli_0000`)
  is the one that would, and this reset deliberately enters at case 2 to skip
  it. The wipe is observable, not theoretical: `Player_move` calls
  `waza_check()` every frame *ahead of* its `routine_no` dispatch, so on frame
  N the cleared slots are re-walked from `w_type == 0` by `check_init` and the
  charge restarts from scratch.

  *The fix.* `Tr_Reset_Apply` snapshots `waza_work[0..1]` before
  `setup_any_data()` and copies the per-slot parser state back after — the
  same save-and-restore shape FIRST ATTACK uses, for the same reason: the
  engine function stays untouched and every other caller keeps the round-start
  behaviour.

  *What is restored and what is not.* `w_type`, `w_int`, `free1`, `free2`,
  `free3`, `w_lvr`, `w_ptr`, `uni0` and `shot_ok`, and nothing else.
  `w_dead` / `w_dead2` are left as `cmd_data_set` has just rebuilt them from
  the command table. `wcp[]` is left entirely alone: `waza_flag` is the
  parser's *output* — non-zero means "this command just completed, count it
  down" — so putting it back would let a motion finished a frame before the
  reset come out after it, which is not what "keep my charge" asks for.
  `waza_compel_all_init` rebuilds `waza_flag`'s live/`-1` pattern identically
  for the same character, so the restored slots stay addressable.

  *The guard, and why it is exactly one comparison.* `get_commands()` picks
  the table `w_ptr` points into from three inputs: `ArcadeBalance_IsEnabled()`,
  `cmd_sel[id]` and `player_number`. The first is latched once in
  `ArcadeBalance_Init()` at boot (`is_enabled` has no other writer) and cannot
  move mid-round; the second is written only by `init_omop()`
  (`system/sysdir.c`), which runs at scene entry, never inside a live round.
  `player_number` is the one that *can* move here — `set_base_data_tiny`
  reassigns it from `My_char[]`, which is how a Twelve mid-X.C.O.P.Y. gets his
  own commands back — and a `w_ptr` saved against the copied character's table
  would point at the wrong move. So the restore is skipped for a player whose
  character changed across `setup_any_data()`. That reset drops the charge,
  which is correct: the slots no longer mean the same moves.

  All 56 entries are carried, in both balance modes. `cmd_init` already
  preserves 48..55 under arcade (`WAZA_WORK_CARRIED_FIRST`), so there the
  copy-back is a no-op for them; under PS2 balance carrying them too applies
  the same intent uniformly, and this reset is not a round start in either
  mode.

  *It collides with none of the four documented defects.* None of them reads
  `waza_work` or `wcp`: defect 1 is the `Suicide[0]` pulse, defect 2 the
  `effect_84` singleton, defect 3 `effect_L8`'s ColorRAM rows, defect 4
  `effect_L0`'s three brightness fields. The restore is confined to
  `waza_work`, so nothing is traded. `waza_work` is `GS_SAVE`'d, but the
  FIRST ATTACK argument covers it unchanged: training is netplay-unreachable.

  **Not preserved, and out of scope:** the *facing* the charge was built
  against. A swap preset moves a player to the other side of the opponent and
  `rl_flag` is recomputed, so a held "back" becomes "forward" and the charge
  breaks on the next frame — correctly, since `check_1` reads a
  facing-relative lever. Nothing here can or should stop that.

- **Velocity, acceleration and sub-pixel position are zeroed.** `player_mv_0000`
  never touches `wu.mvxy`, so a reset mid-dash would otherwise carry the
  momentum straight back out of the start position — the same defect
  `input_script_apply_teleport` has. `plmv_1020` writes only `.disp.pos`, so
  `.disp.low` is cleared separately or the "same" reset lands a fraction of a
  pixel differently every time.
- **`Appear_end` is cleared.** `player_mv_1000` increments it on the
  NON_ANIMATED path and nothing in a live round takes it back down, so it would
  climb 2 at a time toward signed overflow (UB).
- **Accepted loss:** `effect_work_list_init(0, 0)` frees every id-0 hitbox
  overlay, and `setup_any_data` re-creates them only for the two players, so an
  already-airborne projectile loses its box for the rest of its life. Visible
  only with hitbox display on.
- **Accepted ordering:** SELECT on frame N then START on frame N+1 fires a reset
  before the soft-reset chord arms. Damage is bounded — `TASK_RESET` runs before
  `TASK_MENU`, `Reset_Move`'s `effect_work_init` wipes the pool, and
  `Menu_Task`'s `nowSoftReset()` early return stops this code running at all
  until recovery.

## Netplay safety

Netplay-invisible by construction: gated on `Is_Training_Mode`, which excludes
`MODE_NETWORK`, and `Wait_Pause_in_Tr` is unreachable online regardless.

The one call that lives on the shared simulation path — `plcnt_init` →
`Tr_Reset_Position_Override` — is gated twice over: `Tr_Reset_Position_Pending`
is a `menu.c` file-static written only by `Tr_Reset_Apply`, itself behind
`Is_Training_Mode`, and the override re-tests `Is_Training_Mode` before doing
anything. The static is zero on both peers in every netplay frame, so the call
is deterministic and cannot be observed by a rollback.

The preset latch is a `menu.c` file-static, **not** a `GameState` field, on
purpose. `MIST_STATE_VER` is `sizeof(GameState)` (`src/netplay/mist_handshake.c`),
so a field there would force an `EXPECTED_GAME_STATE_SIZE` re-pin *and* a
`MIST_PROTO_VER` bump for a mode netplay cannot reach.

`sizeof(GameState)` is unchanged at 17772 and `MIST_PROTO_VER` remains 4,
because the feature adds no `GameState` field at all -- `Tr_Reset_Location`,
`Tr_Reset_Swapped` and the other reset-latch statics are plain `menu.c`
file-statics (see above). The `_Static_assert` in `game_state.c` that pins
`EXPECTED_GAME_STATE_SIZE` only evaluates on a 32-bit build
(`#if UINTPTR_MAX == 0xffffffff`; the `#else` arm says so explicitly: "64-bit
build: tripwires are disabled"), so it does not run, and proves nothing, on a
64-bit host compile. Everything the feature writes (`plw`, `bg_w`, `Suicide`,
`pcon_rno`) is already rollback-tracked.

## Arcade balance

Works under `ArcadeBalance_IsEnabled()`, which is the device default. Nothing
new sits inside a `!ArcadeBalance_IsEnabled()` branch, and no new consumer of
that predicate was added. `plmv_1020`'s position and `rl_flag` writes sit
outside its arcade-gated tail, and `erase_extra_plef_work`, `setup_any_data`
and `set_base_data_tiny` are ungated.

## Corrections to the design doc

The design doc is `~/Desktop/3sx-training-reset-position-2026-08-30.md`
(not in-tree). Eight of its claims are wrong, incomplete or now resolved.

1. **§11's recipe omits `pcon_rno` entirely.** This is the most important gap —
   following §11 as written produces a reset after which nobody can move.
2. **§7 cites the wrong template.** It presents `init_app_20000` and says "uses
   no `pli_0000()` and no `SDL_zeroa(plw)`; don't add them". Training dispatches
   to `init_app_10000`, which *does* reach `pli_0000`. The instruction described
   a path training never takes.
3. **§4.2's ordering claim is wrong**, and the ordering does not matter.
   `Game2_5` sets `Suicide[0]` and calls `erase_extra_plef_work` in the same
   frame with no move pass between. Ordering is irrelevant because
   `erase_extra_plef_work` never walks list 5.
4. **§4.2 never says to clear `Suicide[0]`**, and nothing else will. See defect 1.
5. **§12 item 4 (effect L8 ColorRAM latch) is real** — it was listed as "same
   hazard class, not separately verified". See defect 3.
6. **§12 item 2 (`wu.target_adrs` across the teardown) resolves safe.**
   `setup_any_data` goes through `set_base_data_tiny`, which does not write
   `target_adrs`; only the full `set_base_data` does, and the pointer targets the
   file-scope `plw[2]` array so it cannot dangle.
7. **§11 step 5's "after `Player_move` has run" names no reachable moment
   from `menu.c`.** It predates the `pcon_rno` fix; the reset now spans three
   frames and `Wait_Pause_in_Tr` runs in `TASK_MENU`, before `TASK_GAME`. See
   *Why the position override runs from `plcnt_init`*.
8. **§12 item 3 (whether a `bgw[1]` write alone suffices) resolves in two
   halves, not one.** The *non-base parallax layers* need no explicit
   handling: `bg_x_move_check` recomputes each of them absolutely from
   `bg_w.bg2_sp_x2`, which `bg_base_x_move_check` publishes from `bgw[1]`. But
   `bgw[1]`'s own **chase copy** does need handling — with
   `bg_w.chase_flag & 0xF` set, `bg_pos_hosei2` reads `chase_xy[0]` and the
   `wxy[0]` write is never published at all. See *Corners: what is computed and
   what is not*. Anyone reading this item as simply "closed" will ship the
   corner presets with a camera that ignores them after a reset taken during a
   super.

   Partial verification of the surrounding claim: all 22 `ta_move_tbl` slots
   (`stage/tate00.c`), 20 distinct movers, were audited and every one reaches
   `bg_pos_hosei2()` unconditionally. Two publish through variants rather than
   plain `Bg_Family_Set()` — `BG020` uses `Bg_Family_Set_appoint(1)` +
   `Bg_Family_Set_2_appoint(0)` + `Bg_Family_Set_appoint(2)`, `BG040` uses
   `Bg_Family_Set_2()` — so "ends with `bg_pos_hosei2()` + `Bg_Family_Set()`"
   is loose wording, though the conclusion holds. **Open:** `BG000`'s base
   layer dispatches `{ bg0001_init00, bg0000_demo, bg_base_move_common }` on
   `bgw_ptr->r_no_0`, so at `r_no_0 == 1` it runs `bg0000_demo` instead of
   `bg_base_move_common` — and `bg0000_init00` sets `bg_app = 1` on that path.
   Whether that state can still be live mid-round in training on stage 0 was
   not cleared; `bg_pos_hosei2()` still runs unconditionally in `BG000`, so the
   camera write itself is unaffected, but `bg_app` is one of the two flags that
   suppress the field clamp.

## Verification status

Built for ARM (`tools/mister/build-game.sh --flavor telemetry`), clean.
`sizeof(GameState)` and `MIST_PROTO_VER` confirmed unchanged.

**Deliberately skipped**, per the design doc's §13: the frame-data suite (no
simulation-logic change — the reset writes only training-gated state) and the
desync/rollback gates (training-only, netplay-unreachable, no `GameState`
change).

### Increment 3: corner touching-distance and composable up-swap

Fixed two user-reported bugs, both on device (Makoto vs Ryu): the corner
presets left the same gap the centre preset does (no actual "touching"), and
↑ always recentred-and-swapped instead of swapping in place at whatever
corner was already latched. Both are covered above (the *What ships today*
table, *Corners: what is computed and what is not*, and the header comment on
`Tr_Reset_Read_Input`).

Verified this round: host build (`cmake -B build/host && cmake --build
build/host -j8`), clean, no warnings in `menu.c`. `sizeof(GameState)` staying
at 17772 is a structural fact about this diff, not something this build
proved: `Tr_Reset_Location` and `Tr_Reset_Swapped` are plain `menu.c`
file-statics, not `GameState` fields, same as the latch they replace, so no
`GameState` layout change is possible here regardless of what any build
checks. The `game_state.c` `_Static_assert` that pins `EXPECTED_GAME_STATE_SIZE`
does **not** run on this host build at all — it sits inside
`#if UINTPTR_MAX == 0xffffffff`, and the host is 64-bit macOS, so the `#else`
arm ("64-bit build: tripwires are disabled") is what actually compiled; the
assert evaluates nothing here. It would need a 32-bit or ARM cross-compile to
mean anything as a build-time check. **Not built for ARM or deployed to
device this round** — no ROM/character-data assets are present in this
worktree, so an ARM build was not possible here; everything below the build
is a mechanical/code trace, not an observed result. See the new checklist
items below.

### Increment 4: corner separation used the wrong candidate of a min()

A review of Increment 3 found that `hit_check_subroutine`'s return is not the
right quantity for the corner override to apply directly. That function
computes two X-axis candidate separations for a pair of pushboxes — the far
box's right edge distance from the near box's left edge (`d2`), and the near
box's right edge distance from the far box's left edge (`d3-d2`) — and returns
`min(d2, d3-d2)`, because during live combat the boxes are only lightly
interpenetrating and the smaller candidate is the true shallow-penetration
depth on whichever side they actually overlap. That reasoning does not hold at
the override's zero-separation stack: the far player is placed exactly on top
of the near one, not lightly overlapping it, and a given corner always needs
the *same* one of the two candidates — d3-d2 for the left corner, d2 for the
right (near player mirrored vs. unmirrored respectively) — not whichever is
smaller. The two candidates are equal only when the near/far box pair is
exactly X-symmetric about the character origin as a pair; otherwise `min()`
silently picks the wall-toward quantity instead of the wall-away one for one
of the two corners, under-separating the pair by up to `2·|c_near + c_far|`
where `c = hd[0] + hd[1]/2` is each box's own centre offset.

**Fix:** `Tr_Reset_Body_Separation`, a new static helper right above
`Tr_Reset_Position_Override`, duplicates `hit_check_subroutine`'s X/Y overlap
arithmetic verbatim (that function has other call sites — hitcheck.c:1848,
:1988 — that must keep taking the min, so it is not changed) but returns the
corner-selected candidate instead of the min. The call site passes
`Tr_Reset_Location == TR_LOC_LEFT` as the selector. Everything else about the
approach (per-character ROM pushbox data, instant snap, the `hos_box[0] == 0`
fallback) is unchanged.

**Verified this round, three ways:**

1. *Synthetic arithmetic*, compiled and run (not hand-traced): a C harness
   (`hit_check_subroutine` and `Tr_Reset_Body_Separation` copied verbatim,
   plus a small driver that stacks two boxes at zero separation, applies the
   candidate as a push, and re-runs `hit_check_subroutine` to measure residual
   overlap) reproduces the predicted bug exactly — a back-skewed symmetric
   matchup (`hd0=-30, hd1=40`) leaves `residual_overlap=40` under the old
   code in both corners, and `0` under the fix, in every case tried
   (symmetric, forward-skew, back-skew, and two genuinely mismatched
   near/far shapes).
2. *Host build*: `cmake -B build/host && cmake --build build/host -j8` is
   clean, no warnings in `menu.c`.
3. *Real ROM pushbox data*: the "no ROM/character-data assets in this
   worktree" limitation noted in Increment 3 was about this worktree, not the
   machine — a sibling checkout
   (`3sx-mister-arcade/tools/arcade-audit`, read-only, its own
   `cg_audit.py`/`data_audit.py`) already decodes every character's HOSA
   (pushbox) table out of the decrypted CPS3 ROM. Used it (via a private
   script, not committed to that repo) to pull the actual standing/appear-pose
   pushbox for all 20 characters. The pose itself is traced through source,
   not assumed: `plmv_1010` sets `routine_no = [3,0,1,0]`; `Player_normal`
   dispatches on `routine_no[2]==1` to `Normal_01000` (`plpnm.c`), whose
   `routine_no[3]==0` branch calls `set_char_move_init(&wk->wu, 0, 0)`
   (`charset.c`) — `char_table[0][0]`, i.e. `KOC2SEC[0]` = the `nmca` script
   table, script index 0, cell 0 — the exact cel active when
   `Tr_Reset_Position_Override` reads `h_hos` later the same frame.

   **Result: every one of the 20 characters' standing/appear pushboxes is
   exactly origin-symmetric** (`hd0 == -hd1/2`, e.g. Ryu `(-25, 50, 0, 84)`,
   Makoto `(-25, 50, 0, 71)`, Hugo — the widest, `hd1=60` — `(-30, 60, 0,
   101)`, Yun — the narrowest, `hd1=42` — `(-21, 42, 0, 70)`). That is exactly
   the condition under which the old `min()` code and the fix agree, so **the
   bug was latent, not visible on any pad with the shipped roster**: Ryu vs.
   Makoto (the pair actually tested on device) and Hugo vs. Yun were both
   re-run through the same compiled harness using these real values in all
   four corner/near-far combinations, and old and fixed code produce the
   identical separation (50 for Ryu/Makoto, 51 for Hugo/Yun) with zero
   residual either way. The fix is not a behavior change for today's roster —
   it is a correctness fix for an assumption the old code depended on without
   checking. That the assumption happens to hold is not something this code
   should keep relying on: a full census of every live HOSA entry (any
   `hos_box[0] != 0`) across all 20 characters — 657 entries, covering every
   pose, not just standing — found 475 of them (72.3%) are **not**
   origin-symmetric. Standing poses are uniformly symmetric in this ROM, but
   nothing enforces that as an invariant, so a future balance change to a
   standing box, or this path ever being reached from a non-standing pose,
   could reintroduce the shortfall silently were the fix not in place.

**Not built for ARM or deployed to device this round** — this worktree still
has no ROM/character-data assets to link into a running build here, only the
separately-checked-out ROM used for the read-only data audit above; an
on-device retest of Makoto vs Ryu (or any pair) is still needed to confirm the
visible behavior, though the geometry itself is now verified against the real
data those characters ship with, not a synthetic stand-in.

### Increment 5: horizontal diagonals, the repeat rule, and charge preservation

Three changes, one commit, 2026-09-07.

**1. Diagonals now resolve to their horizontal component.** ↓↙ / ↓↘ are the
corner, not centre; ↑↖ / ↑↗ are the corner *and* the swap. Only a pure
vertical reaches the centre or swap branch. The order of tests in
`Tr_Reset_Read_Input` is now horizontal-first (with up tested inside the
horizontal branch), where it was vertical-first.

Record of what this replaced, because the old rationale is still quoted
elsewhere: down was tested first, and the stated reason was that down-back and
down-forward are the resting stick positions and an exact word match "would
swallow the reset for most of what a player actually holds". That reason is
still true and still the reason the code tests bits rather than words — but it
only establishes that a diagonal must map to *something*. It was never an
argument for the vertical specifically. A player holding down-back gets a
reset under either mapping; the old one gave a recentre, the new one gives a
corner, and neither is safer, because the stick can be at down-back anywhere
on the screen. There is no position to guard, so no downside is being accepted
here — the horizontal is simply the mapping the feature wants.

**2. A third latch, `Tr_Reset_Last_Horiz`, and the repeat rule.** A horizontal
press keeps the swap bit iff the location it is leaving was not centre and the
horizontal equals the last horizontal pressed; otherwise it clears. ↑← / ↑→
set the swap bit outright. The new latch is needed because "the same direction
again" is defined against the last horizontal *pressed*, not against the
location the players ended up at, and ↑← latches a corner without the rule
ever running. Every horizontal-carrying input updates it, up-diagonals
included — see *What ships today* for why, and for the ↓ case that provably
does not need it.

`Tr_Reset_Last_Horiz` is a `menu.c` file-static like the two latches beside
it, so `sizeof(GameState)` and `MIST_PROTO_VER` are structurally untouched
again.

**3. Charge state is preserved across the reset**, for both players — the
whole derivation is under *Smaller things worth knowing*. In short: the charge
lives in `waza_work[id][slot]`, the only thing on this path that wiped it was
`setup_any_data()` → `set_base_data_tiny()` → `cmd_init()`, and
`Tr_Reset_Apply` now snapshots and restores the per-slot parser state around
that call. It reaches none of the four documented defects, which is checked
explicitly there.

**Verified this round:** host build clean
(`CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release`, then
`cmake --build build --parallel`), `tools/mister/build-game.sh --flavor
telemetry` clean (exit 0), `tools/doc-citations/check_baselines.py`
`scopes=11 breached=0`.

**Deliberately skipped:** the statcheck corpus sweep and the frame-data golden
suite. This is training-mode menu code behind a SELECT gesture and neither gate
can reach it — the statcheck oracle compares a CPS3 capture against the
simulation on a corpus of recorded matches, and the golden suite compares
frame-data labels; neither drives `Wait_Pause_in_Tr`, and no simulation
function was changed. (The charge restore writes `waza_work`, which statcheck
*does* compare — but only inside `Tr_Reset_Apply`, which no corpus can enter.)

**UNVERIFIED AT RUNTIME.** None of this can be exercised headlessly: there is
no harness that presses SELECT with a direction held. The Quick Training
harness (`--test-quick-training`) covers only the bare-SELECT centre preset and
was not extended. Every row of the input table, and the charge behaviour, is a
code trace only. The device checklist below carries the items.

### Still outstanding — needs a human with a pad, under arcade balance

- [ ] **Acceptance criterion:** after a reset, both pads control their
      characters again and the dummy resumes its configured behaviour. This is
      simulation-traced only, and it is the defect that made the first draft
      unusable.
- [ ] Does the three-frame gap read as a clean snap or a visible hitch? Is the
      one frame rendered at the old position (the position lands on N+1)
      noticeable?
- [ ] Reset during a super art — no lingering screen darkening (`SA_shadow_on`).
- [ ] Reset during Makoto's Tanden Renki, **then a second Tanden Renki** — the
      palette bug does not self-heal, so one reset is not a sufficient test.
- [ ] Reset during Twelve's invisibility fade — brightness restored.
- [ ] Reset mid-dash — no residual momentum.
- [ ] K.O. after a reset — the round message still renders (defect 2).
- [ ] Both characters visible, combo counter cleared, camera correct.
- [ ] Reset mid-throw and mid-super-freeze.
- [ ] Down-back / down-forward reset does not feel accidental during blockstring
      practice — note it now gives the **corner**, not centre.
- [ ] START + SELECT soft reset still works from inside training.

#### The full input table, one row at a time (Increment 5, all unverified)

Start each row from a known state. Under arcade balance, both pads on HUMAN so
SELECT is live on each.

- [ ] **SELECT alone** repeats the last (location, swap) pair exactly,
      including a swapped corner.
- [ ] **↓ + SELECT** (pure down) → centre, original sides, whatever the swap
      bit was.
- [ ] **↑ + SELECT** (pure up) → sides swapped at the **current** location; the
      location does not move.
- [ ] **← + SELECT** from centre → left corner, **original sides** (the swap
      bit clears even if it was set).
- [ ] **→ + SELECT** from centre → right corner, **original sides**.
- [ ] **← + SELECT twice in a row** → the second press keeps the sides the
      first left (swap preserved on a repeat).
- [ ] **← + SELECT then → + SELECT** → the right corner with **original
      sides** (opposite horizontal clears the swap).
- [ ] **↓← + SELECT** behaves exactly like bare ← (left corner), and **↓→**
      exactly like bare → — *not* centre. This is the inverted behaviour;
      confirm it explicitly.
- [ ] **↑← + SELECT** → left corner **and** swapped, in one press. **↑→** →
      right corner and swapped.
- [ ] **↑→ + SELECT then → + SELECT** → the swap **survives** (the up-diagonal
      updated the last-horizontal latch). If the second press un-swaps, the
      latch update on up-diagonals regressed.
- [ ] **The named sequence:** → + SELECT, then ↑ + SELECT, then → + SELECT
      again ⇒ **1P on the right, 2P on the left** — i.e. the swap survives the
      third press. This is the acceptance case for the repeat rule.
- [ ] **START + SELECT** is still the soft reset and never a position reset.

#### Charge preservation (Increment 5, unverified)

- [ ] Pick a charge character (Urien, Remy, Alex, Hugo, Q). Hold **back** for
      ~2 s, then press **SELECT + ←** and *immediately* release back and press
      the button — **the charge move still comes out.** Before this change it
      did not.
- [ ] The same with **down-back → SELECT + ↓** (a pure-down recentre) for a
      down-charge move.
- [ ] The same on **2P's pad**, dummy set to HUMAN — preservation is for both
      players.
- [ ] **Negative control:** a *partial* charge (hold back ~0.3 s) plus a reset
      still does **not** produce the move. The restore keeps the charge where
      it was, it does not complete it.
- [ ] **No stray move on reappear.** Do a clean QCF+P motion and hit SELECT
      within a couple of frames — the fireball must **not** come out after the
      reset. `wcp[].waza_flag` is deliberately left rebuilt for this reason;
      if a move fires, the restore reached too far.
- [ ] **Twelve mid-X.C.O.P.Y.** reset — no crash, no wrong move coming out.
      The restore is skipped when `player_number` changed; this is the row that
      exercises that guard.

Per preset (↑ / ← / →), under arcade balance:

- [ ] Positions **and facing** are right on frame 1, not frame 2 — no one-frame
      back-to-back render.
- [ ] Pure ↓ afterwards recentres and un-swaps regardless of history (↓ is the
      one fully absolute direction left; ← / → are absolute in location only).
- [ ] Corner touching, no gap: ← and → put the two characters **touching**, not
      the centre preset's spacing, and the pushbox-derived distance holds up
      for a wide character (Hugo) and a narrow one (Ibuki/Yun) — this is BUG 1.
      Watch specifically for the fallback firing (176 spacing again, i.e. the
      old gap) — that would mean `hos_box[0] == 0` was hit for a standing
      appear pose, which is believed unreachable but not proven.
- [ ] **↑ is composable, not absolute (BUG 2):** SELECT+→ then SELECT+↑ leaves
      both players in the **right** corner, touching, sides swapped — not
      recentred. Same for SELECT+← then SELECT+↑ in the left corner. Pressing
      ↑ a second time in a row is a no-op (still "this location, swapped"),
      not a toggle back to original sides.
- [ ] Corner camera lands on the same frame as the characters, with **no
      visible chase** and no parallax layer left behind at its stage default —
      including after a swap-in-place (↑ at a corner), which moves no camera.
- [ ] The same, **taken mid-super** (a Super Art with a camera move, e.g. a
      cinematic freeze) — this is the case an in-flight `chase_flag` used to
      break, and the one that exercises both chase clears.
- [ ] Bare SELECT repeats the last (location, swap) combination, including a
      swapped corner.
- [ ] ↑↖ / ↑↗ now put the players in that **corner** with sides swapped (they
      no longer swap in place), and ↓↙ / ↓↘ put them in that **corner** with
      original sides (they no longer centre). Both are the Increment 5
      inversion — confirm neither still resolves vertically.
- [ ] A reset that is interrupted (training menu, soft reset) does not leave a
      preset armed for the next round's appear.

### Unverified in simulation

- Whether the corner camera write ever fights `check_cg_zoom`'s zoom request.
  The *chase* half of that question is now closed and fixed — see the
  `chase_flag` bullet under *Corners*. What remains: `check_cg_zoom` runs
  later in the same frame as the override (`Player_control` order is
  `set_scrrrl` → `player_main_process[0]` → … → `check_cg_zoom`), and
  `chase_start_check` runs later still, inside `TATE00`. If the appear pattern
  ever carried a `wu.cg_zoom` with bits `0xE200` set, `chase_start_check` could
  arm a **fresh** chase after the override cleared the flag, seeding
  `chase_xy[0]` from the previous frame's `abs_x` — i.e. the stage default.
  `cg_zoom` is per-animation-frame ROM data written through
  `setupCharTableData`, so it could not be read out of the tree; the standing
  appear pattern is assumed to request no camera move, and that assumption is
  **not verified**. Symptom if wrong: the corner camera chases in from the
  stage default over ~6 frames instead of landing.
  `compel_bg_init_position` runs `Zoomf_Init()` and resets `bg_f_x` /
  `frame_flag`, so a reset does start from an un-zoomed camera.
- Whether `bg_app` / `bg_app_stop` can be non-zero on the override frame. Both
  gate the field clamp in `move_P1_move_P2` and the override copies that guard;
  the `Allow_a_battle_f` gate in `Tr_Reset_Check` argues they cannot be, but
  that was not proven. Writing X to the wall rather than past it means the
  failure mode is a half-body-width gap, not a character stuck outside the
  field.
- Whether skipping `plcnt_move` for the three reset frames — so `time_over_check`,
  `settle_check` and the training `dm_nodeathattack` write do not run — has any
  observable effect. Argued inert (no player can act, attacks were torn down,
  training forces `Counter_hi = -1`) but not proven exhaustively.
- Whether a training round can reach the `Game_Manage_5_2` / `7_5` / `12_3`
  stalls at all. The normal K.O. path cannot — `Game_Manage_6th` case 1 and
  `Game_Manage_7_3` both take the `Is_Training_Mode` branch to
  `C_No[0] = 12; End_Training = 1`. `Conclusion_Type` 1 (double K.O.) and 2
  (time over) were not traced. The structural defect is verified regardless.
