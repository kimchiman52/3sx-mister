# RFC: Post-match rematch

**Status:** Implemented; pending two-peer device acceptance

**Date:** 2026-09-10
**Scope:** the local Versus and netplay post-match menus.

## Goal

Replace the local Versus and netplay result-menu choices with:

1. **Rematch** (the default selection)
2. **Char Select**
3. **Exit**

A rematch starts another match without visiting character select, retaining both
players' prior character, colour, and Super Art selections. Both players must
confirm Rematch. A Char Select confirmation takes priority over an armed
Rematch and takes both players to the ordinary character-select flow.

Replay saving remains available through other menu paths; this change removes
it from these two post-match menus.

## What exists today

`src/sf33rd/Source/Game/menu/menu.c` -> `VS_Result` already builds a
three-row result menu and starts each visit with row 0 selected. Its current
actions are:

| Row | Current action | Mechanics |
|---|---|---|
| 0 | Continue | Each player confirms independently; once both have confirmed, `VS_Result_Move_Sub` enters result state 6. `VS_Result` then calls `Setup_VS_Mode` and sets `G_No` to `Game12`, the ordinary character-select transition. |
| 1 | Save Replay | One confirmation enters result state 5, whose `Exit_Sub(..., 17)` route dispatches `Save_Replay`. |
| 2 | Exit | One confirmation enters result state 7 and calls `Netplay_HandleMenuExit`. |

So the mutual-consent interaction requested for Rematch is already present,
but it currently means “go to character select,” rather than “start the same
match again.” `Menu_Cursor_X[0..1]` supplies the two visible confirmation
latches, and is already part of the rollback state.

The direct match-start template also exists. `menu.c` -> `Load_Replay_Sub`
sets character, colour, Super Art, operators, stage, and match options, runs
the loading/fade sequence, calls `init_omop`, then performs the same
`Game01_Sub` / `G_No = { 2, 0, 0 }` transition that starts a match. This is a
better template than attempting to automate character select. The normal
character-select exit also proves that its loading sequence has required
engine work: player and background loads, `init_omop`, and the subsequent
match initialisation.

## Proposed behaviour

This behavior applies when `Mode_Type` is `MODE_VERSUS` or `MODE_NETWORK`.

### Selection rules

* **Rematch:** a player confirming row 0 marks that player ready. Both ready
  states start the rematch transition.
* **Char Select:** either player confirming row 1 clears both Rematch-ready
  states and immediately chooses the existing character-select transition.
  It therefore wins over Rematch, including when the inputs arrive in the same
  simulated frame.
* **Exit:** preserve the current unilateral exit behavior. It clears any
  pending Rematch readiness. Netplay uses the existing deferred teardown;
  local Versus follows the ordinary result-menu exit route.
  For a same-frame conflict, the recommended resolution is `Exit`, then
  `Char Select`, then `Rematch`; it avoids holding a player in a live session
  after they asked to leave.
* **Cancel/back:** while ready for Rematch, cancel clears only that player's
  ready state. Otherwise it keeps the current cursor behavior.

The implementation must collect the two players' input edges before choosing
an action. The current helper processes player 0 before player 1 and returns
early after a terminal action; retaining that incidental ordering would make a
same-frame conflict depend on player slot. A small pure resolver should take
both inputs and the two ready latches, return one of `none`, `wait`,
`rematch`, `char-select`, or `exit`, and apply the precedence above.

### Fast match start

On a mutual Rematch, use a shared result-menu transition based on
`Load_Replay_Sub` and the normal select exit:

1. Keep the already selected `My_char`, `Player_Color`, and `Super_Arts` for
   both players; do not enter `Game12` or call selection logic.
2. Keep the current match stage (`bg_w.stage`) and its loaded assets. This is
   the expected “same selections” interpretation and avoids adding a hidden
   stage decision to a one-button rematch. Verify it on device, including
   stage-specific load paths.
3. Run the proven per-match setup and asset barriers: player/background load
   requests, their completion checks, audio transition checks, `init_omop`,
   `Game01_Sub`, `Set_Appear_Type_For_Mode`, texture-cache setup, then the
   `Game02` entry state.
4. Reset only per-match state through those existing start routines. Preserve
   the live netplay session and its negotiated configuration when one is
   active.

The exact helper should be factored from the existing paths rather than
copying a partial list of resets. In particular, `Game2_0` establishes the
new game's timer, round state, input/replay state, and engine management
state; jumping straight to gameplay would skip those invariants.

## Netplay and rollback design

No new network packet or side channel is needed. During a running session,
`src/netplay/netplay.c` -> `advance_game` supplies both players' input words
to the same simulated game frame, including rollback replay frames. The menu
and the relevant selections already live in `GameState`: this includes
`task`, `G_No`, `Menu_Cursor_X`, `Menu_Cursor_Y`, `My_char`, `Player_Color`,
`Super_Arts`, and `VS_Stage`.

The resolver and the rematch transition must therefore be entirely
simulation-owned. Do not store rematch intent in a file-static variable or
send it through the connection/UI thread. If a new state field is truly
needed, it must be represented in `GameState`, covered by `GS_SAVE` and
`GS_LOAD`, and re-pin `EXPECTED_GAME_STATE_SIZE` on ARM32.

Exit needs special care in netplay. `Netplay_HandleMenuExit` deliberately defers a
running-session teardown until the input-prediction window is final. Its
existing unit test documents the relevant race: a speculative Exit can be
rolled back when an in-flight remote confirmation corrects the timeline to a
mutual menu action. The new resolver must continue to call it only for the
final Exit result; it must never call it while waiting for, or taking, a
Rematch.

## UI work

The stock menu labels are `effect_91` sprite entries. The implementation draws
**REMATCH**, **CHAR SELECT**, and **EXIT** with the proportional UI font in
both target modes. Their measured widths are 56, 84, and 28 pixels,
respectively, so they fit each 192-pixel player half. Device acceptance must
still confirm the ready flash and row-0 default selection visually.

## Implementation plan

1. **Characterize the current path.** Add a narrow scripted/menu test or
   debug trace that records both confirmation latches, the chosen result
   state, and the resulting `G_No` route. Capture the current visual row
   order on desktop or device.
2. **Establish the visual assets.** Add the two labels and prove their
   placement before changing menu behavior.
3. **Introduce the resolver.** Make it pure and cover the complete
   two-player action matrix, especially same-frame Rematch/Char Select/Exit
   conflicts and cancel-after-ready.
4. **Add the fast-start helper.** Factor the shared, complete match-start
   chain from the normal selection/replay paths. Route mutual local Versus and
   netplay Rematch through it. Only netplay waits for its prediction window
   before it begins external loading work.
5. **Validate under rollback.** Exercise confirmation while remote input is
   predicted and then corrected, plus the existing deferred-exit race. Verify
   that no rematch starts early and no session tears down after a corrected
   Rematch.
6. **Run device acceptance.** Test both peers confirming, one waiting, one
   choosing Char Select after the other readies, unilateral Exit from each
   state, and several character/colour/Super Art/stage combinations.

## Required validation

Because this reaches menu state, game start, and rollback state, implementation
will require:

* targeted resolver/menu tests and `src/netplay/test_netplay_units.c`;
* desktop and telemetry MiSTer builds;
* `tools/rollback-determinism/run.sh` if any saved state or match-start
  globals change; and
* a two-peer hardware session that intentionally forces a rollback across a
  menu decision.

The frame-data corpus is not a direct gate for the menu-only portion. It
becomes relevant only if the final fast-start factoring changes game-engine
behavior reachable during gameplay.

## Acceptance criteria

* The local Versus and netplay result menus read **Rematch**, **Char Select**, **Exit**, with
  Rematch selected by default.
* One Rematch confirmation visibly waits; two confirmations start another
  game without character select.
* Characters, colours, Super Arts, and stage match the prior game.
* Char Select always defeats a pending Rematch.
* Exit remains safe through rollback and does not leave either peer stranded
  in a running session.
* Replay saving remains available outside the local Versus and netplay result
  menus.
