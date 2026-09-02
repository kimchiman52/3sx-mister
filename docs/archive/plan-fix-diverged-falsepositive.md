# Fix plan — `recording_battle_ahead()` win-pose false-positive DIVERGED

Status: investigation complete, **NO-GO on per-frame ix16**; fix scoped below.
Author evidence base: gate investigation 2026-07-26 (this doc's every claim
cites a `file:line`, a frame-level archive decode, or an observed run).

## 1. Summary of the bug

`ReplayPlayer` mislabels a **correctly-finished** replay as `REPLAY DIVERGED`.
The overlay shows "REPLAY DIVERGED" (status `DESYNCED`) at the match-ending KO
even though the local simulation reproduced the recorded match faithfully.

Root cause is `recording_battle_ahead()` (`src/replay/replay_player.c:1570-1577`,
added in `723b178e`) together with `finish_diverged_game_end()`
(`src/replay/replay_player.c:1588-1613`), reached from the `game_ended()` branch
at `src/replay/replay_player.c:1919-1930`.

`723b178e`'s premise (from its commit message) is:

> the tracker emits a checkpoint ONLY while in active battle
> (runner-track-3sr isInGame gate == ReplayDumpCps3MainRam's, RAM game-state
> == 2 … win-pose/results NOT recorded), so recorded checkpoints beyond
> play_index prove the real match was still being fought.

**That premise is false.** The tracker's in-game gate is game-state offset
`0x15438` `== 2` (`tools/fcade-replays/runner-track-3sr.patch:80-81`,
`kDumpGameStateOffset = 0x15438`, `kDumpInGameState = 2`). `0x15438` is
`G_No[1]` (`G_No` base `0x15436`, `src/arcade/arcade_constants.h:11`). **`G_No[1]`
stays `== 2` through the entire post-KO win pose** — so the tracker keeps
emitting checkpoints during the win pose, and `make_3sr.py` copies every one of
them into the `.3sr` (it extracts from game-start to the end of the archive
segment with **no** re-gate — `tools/fcade-replays/make_3sr.py:202-241`).

Consequently, on any replay whose recorded win-pose tail is longer than one
`checksum_interval` (60 frames) past the KO, `recording_battle_ahead()` sees
"battle checkpoints ahead" that are actually win-pose frames, and reports a
false divergence.

### Ground-truth proof (quark 9791, game_0 — full `.scrd` archive on disk)

Archive decode (`build/gate-verify/scrd/…-9791.7/game_0.scrd`, 4099 frames):

- Round-2 KO lands at **frame 3506-3507**: P2 vital `5 → -1`, `C_No[0]` steps
  `2 → 3 → 6`, `G_No=[2,2,1,1]` (frame-by-frame decode).
- Frames **3507 … 4098 are all win pose**: `G_No[1]==2`, `C_No[0]` ∈ {6,8},
  P2 vital pinned at `-1`. Not a single active-battle frame after 3507.
- `statcheck` (the per-frame-ix16 oracle, `build/host-statcheck`) on the same
  archive: `PASS — compared archive frames 1..3507 of 4099`; its trace ends at
  `f=3507 C=[6,0,0,0]` — i.e. **statcheck's own sim ends the match at the exact
  same KO frame** (3507), and everything up to it matched full RAM.
- The shipped checkpoint player: `REPLAY DIVERGED at frame 3507 … PL_Wins=[2,0]
  … 9 more battle checkpoint(s) ahead (last cp frame 4080)`. Its KO (3507),
  ix16 (`0x000d==13`) and timer (3505) all equal the archive at 3507 → the
  local sim was **frame-exact to the KO**; the "9 battle checkpoints ahead"
  are the win-pose frames 3540-4080.

The KO is correct; the match ended 2-0; the "divergence" is entirely the
win-pose tail tripping `recording_battle_ahead()`.

## 2. Breadth — corpus of 12 games (`.3sr` + `.scrd` both on disk)

| game | shipped player | brk frame | statcheck/archive KO | brk==KO | class |
|------|----------------|-----------|----------------------|---------|-------|
| 7287_g0 | DIVERGED(premature) | 5380 | 5380 | ✅ | **false positive** |
| 7287_g3 | DIVERGED(premature) | 6704 | 6704 | ✅ | **false positive** |
| 9791_g0 | DIVERGED(premature) | 3507 | 3507 | ✅ | **false positive** |
| 2133_g1 | DIVERGED(premature) | 7612 | 7612 | ✅ | **false positive** |
| 2133_g2 | DIVERGED(premature) | 4700 | 4700 | ✅ | **false positive** |
| 2133_g4 | DIVERGED(premature) | 5895 | 5895 | ✅ | **false positive** |
| 4017_g0 | DESYNC(checkpoint) | 6120 | 7492 | — | real desync (statcheck PASS) |
| 4017_g1 | DESYNC(checkpoint) | 2220 | 7643 | — | real desync (statcheck PASS) |
| 2133_g3 | DESYNC(checkpoint) | 2280 | 9814 | — | real desync (statcheck PASS) |
| 2133_g5 | DESYNC(checkpoint) | 3000 | 7949 | — | real desync (statcheck PASS) |
| 8232_g0 | DESYNC(checkpoint) | 2460 | 5295 | — | real desync (statcheck PASS) |
| 6293_g0 | COMPLETE | 7141 (exhausted) | 7517 | — | partial `.3sr` (truncated pre-KO) |

**Two disjoint failure classes:**

1. **premature-game-end DIVERGED (6/6 = 100% false positive).** Every one fired
   `game_ended()` at the exact archive KO frame — `game_ended()` **never fired
   early**. All were mislabelled by `recording_battle_ahead()` on the win-pose
   tail. Win-pose tails span 256-614 frames (4-10 checkpoints) — always >1
   interval, which is why the 1-interval grace never saves them.

2. **checkpoint DESYNC (5 games).** A *genuine* divergence of the
   checkpoint-only ix16-resync playback: between-checkpoint ix16 drift bit a
   gameplay consumer (dizzy/stun/damage — the mechanism `check_checkpoint`
   already documents at `replay_player.c:2015-2020`) and flipped a **checkpointed**
   field (position/C_No/timer), caught by the 12-field hash **before any KO**.
   `statcheck` (per-frame ix16) reproduces all five frame-exact → per-frame ix16
   *would* fix these. This is a real, **separate** fidelity issue (see §6); it is
   **not** the 5828 bug and it is not fixed by the plan below.

### The 5828 gate case

`diag-5828.3sr` diverges identically to the six proven false positives:
checkpoints 58/59/60 (frames 3420/3480/3540) were **fully exact** (all 13 fields,
incl. positions + ix16, no resync); the break is at frame 3584 with
`C=[6,0,0,0]` (win pose), `PL_Wins=[0,2]`, and "9 more battle checkpoints ahead
(last cp 4140)". Its `.scrd` was not retained (VPS conversion isn't
statcheck-gated) and re-fetching quark `1785094172694-5828` needs a live
Fightcade+FBNeo run, which was not performed — so 5828's KO was not verified
against its own archive directly. But its signature is bit-for-bit the same
class as the six locally-proven false positives, so 5828 is a false positive to
the same confidence.

## 3. Is `game_ended()` reliable? (decides the fix)

Yes, within a single-game `.3sr`. `game_ended()` (`replay_player.c:1501-1503`)
is `PL_Wins[0]==2 || PL_Wins[1]==2`. A `.3sr` is exactly **one game / one match**
(`docs/3sr-format.md` preamble). Once `PL_Wins` reaches 2 the match is over by
definition — there can be no further *battle* in the same file, only win pose.
Evidence: 6/6 premature cases fired at the true archive KO; 0 fired early.

The `723b178e` premise that `game_ended()` "fired prematurely… a round ended
differently" is not supported by any corpus game. The genuine RNG-drift
divergences it worried about **do** exist (the 5 checkpoint DESYNCs) but they
surface through `check_checkpoint`'s 12-field hash *before* a KO, never as an
early `game_ended()`. The original device report behind `723b178e` (quark
`…-4227`: `game_ended` at 4964, checkpoints to ~5520 — a 556-frame tail, squarely
in the corpus win-pose-tail range) is, on this evidence, itself a misdiagnosed
win-pose tail, i.e. a legit match-end.

## 4. Why "play through the win pose" (option d) does NOT work

Tested empirically (env-gated experimental build, reverted): with the
`game_ended()` stop removed, 9791 continues injecting the recorded win-pose
inputs. Checkpoints 61-65 (frames 3600-3840) **verify OK**, then checkpoint 66
(frame 3900) `REPLAY DESYNC` (`live=34468c83 want=c19a41e2`). Process exit 0 —
**no qix crash** through the recorded win pose, but the win pose itself drifts
(its heavy visual-effect `random_16()` traffic diverges the checkpointed fields
under checkpoint-only ix16 resync). So we cannot rely on verifying win-pose
checkpoints; the win pose must be treated as **unverified cosmetic tail**, not as
battle to be checked.

## 5. The fix — options and recommendation

The checkpoint table is only `{frame, djb2}` (`docs/3sr-format.md §4`); a player
holding an old file cannot read a per-checkpoint battle/win-pose flag from it.

- **(a) Trust `game_ended()` → `COMPLETE`.** Drop `recording_battle_ahead()` and
  `finish_diverged_game_end()`; the `game_ended()` branch becomes an
  unconditional `finish("game-ended")`. Zero format change, works on every
  existing `.3sr`, and is correct because `PL_Wins==2` in a one-game file means
  the match is over. Residual risk: a wrong-early-KO in the <60-frame window
  after the last passed checkpoint would be a false `COMPLETE` — unobserved in
  12/12, and strictly no worse than pre-`723b178e`.
- **(b) Gate on local win-pose state.** Adds nothing over (a): both a legit and a
  hypothetical wrong-early KO show `C_No[0]>=6` locally, so local state cannot
  distinguish them. Rejected.
- **(c) Per-checkpoint battle-flag in the `.3sr`.** Tracker marks win-pose
  checkpoints (or stops emitting them once `PL_Wins` hits `Battle_Number`);
  player ignores flagged/absent ones. Robust for *new* files and would also let
  a genuine wrong-early-KO be detected — but needs a format/tracker change and
  every existing file still needs (a) as the fallback.
- **(d) Play through + verify win pose.** Rejected (see §4 — win pose desyncs).

**Recommendation: (a) as the fix, with (c) as optional later hardening.** The
genuine-premature-end detection that `723b178e` wanted is *already* provided by
`check_checkpoint` (it caught 5/5 genuine divergences before any KO); the
PL_Wins-vs-"battle-ahead" heuristic adds only false positives on top of it.

## 6. Note on per-frame ix16 (the original hypothesis) — NO-GO

Per-frame `Random_ix16` sync (statcheck's per-frame dirty-sync,
`src/test/statcheck_compare.c:239-241`) is **not** the fix for this bug:
`statcheck` lands the KO at the identical frame as the checkpoint player on all
6 premature cases — it changes the DIVERGED verdict by **zero** frames, and the
win-pose tail (hence the false positive) is intrinsic to the recording. Do
**not** add a per-frame ix16 lane for this bug. (It *would* independently fix the
5 checkpoint-DESYNC games — a distinct fidelity improvement, ~+8 KB/file per
`docs/3sr-format.md §1`, tracked separately, not part of this plan.)

## 7. Staged plan

- **S1 — Player fix (option a).** In `ReplayPlayer_Tick` PHASE_GAME
  (`replay_player.c:1919-1930`) replace the `game_ended()` branch with an
  unconditional `finish("game-ended")`. Delete `recording_battle_ahead()`
  (`:1570-1577`) and `finish_diverged_game_end()` (`:1588-1613`) and the now-dead
  `log_live_fields` forward-decl use if it becomes unused. Keep `check_checkpoint`
  and its honest `REPLAY DESYNC` untouched (that path stays the real divergence
  detector).
  - **Gate:** all 6 false-positive `.3sr` (7287_g0/g3, 9791_g0, 2133_g1/g2/g4)
    now report `REPLAY COMPLETE`; 6293_g0 still `COMPLETE`; the 5 checkpoint-DESYNC
    `.3sr` still `REPLAY DESYNC` at the same frames (regression guard that we did
    not weaken real detection). Harness already exists:
    `scratchpad/gate2/serial_player.sh`.

- **S2 — Terminal-overlay wording + teardown.** Ensure the `COMPLETE` terminal
  path freezes and lingers exactly as today (`finish()` sets `s_stall_frame`,
  `tick_terminal` holds — `replay_player.c:1519-1557,1619-1654`); confirm the CLI
  and browser teardowns both take the `COMPLETE` branch cleanly (no
  `DESYNCED`-only assumptions downstream in the overlay / browser
  `tick_launching`).
  - **Gate:** on-device (or headed host) a finished replay shows "REPLAY
    COMPLETE" and returns to the browser/menu without the qix crash.

- **S3 — Device confirmation on the real 5828 stream.** Re-watch/replay
  `1785094172694-5828` end-to-end; expect `REPLAY COMPLETE` (P2 2-0), no false
  DIVERGED.

- **S4 (optional, later) — Tracker hardening (option c).** Stop emitting
  checkpoints once `PL_Wins` reaches `save_w[Present_Mode].Battle_Number`
  (win-pose start), so new `.3sr` last-cp ≈ true KO. Lets a future
  `recording_battle_ahead`-style check flag a *genuine* wrong-early-KO without
  false positives. Old files keep working via S1. Needs `runner-track-3sr.patch`
  edit + `.3sr` regen; independent of S1-S3.

## 8. Backward compatibility

S1 is player-only and format-neutral: every existing `.3sr` (with or without a
win-pose checkpoint tail) plays correctly. No `3SR2` bump. S4, if pursued, only
changes which checkpoints *new* files carry; the S1 player treats any file the
same way regardless of tail length.
