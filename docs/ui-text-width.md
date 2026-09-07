# On-screen text and the 384 px canvas

Every string the game draws lands on a 384x224 canvas. A message that is
easy to write is very often wider than that, and the failure is silent: the
proportional text path centres whatever it is given, so an over-wide string
spills off **both** edges symmetrically and the player sees the middle of a
sentence. That is what the netplay refusal overlay did — the screen showed
`s arcade balance - CPS3 ROM not found or failed content` because the full
line measured 560 px.

This note records the width rule for each text path, the audit that was run
against every call site, what was found, and what was decided. It cites
`file` -> `symbol`; there are no line numbers here to drift.

## 1. How wide is a string?

There are four text paths in `src/sf33rd/Source/Game/ui/sc_sub.c` and they
have **different** geometry. Measure with the right one.

| path | anchor unit | glyph advance | width of a string |
| --- | --- | --- | --- |
| `SSPutStrPro` / `SSPutStrProP` (proportional) | pixels | `8 - sideL - sideR` per glyph, from `ascProData` | `SSGetDrawSizePro(str)` |
| `SSPutStr` / `SSPutStr2` / `SSPutDec` / `scfont_put` / `scfont_sqput` / `score8x16_put` | 8 px **cells** (x is a column, 0..47) | 8 px | `8 * strlen` |
| `SSPutStr_Bigger` | pixels | `8 * sc` per glyph; a `$` is a 4 px spacer and draws nothing | `8*sc*glyphs + 4*sc*dollars` |
| `SSPutDec3` / `scfont_sqput3` | pixels | 11 px digits; raw texel widths | per call |

Proportional glyphs range from 3 px (`\|`) to 8 px (`W`, `M`, `~`); a space
is 4 px. A rough "8 px per character" estimate over-counts by up to 40 %,
which is how a 40-glyph budget in a comment turns into a 560 px string on
screen — measure, do not estimate.

`SSPutStrProP(flag=1, x, ...)` centres the string in `[0, x]`, so a centred
draw's budget is the value passed as `x` (384 for the canvas). A
left-anchored draw at `x=N` has `384 - N`.

### Measuring on the host

`tools/ui-text/strwidth.py` mirrors `SSGetDrawSizePro` exactly — it parses
`ascProData` out of `sc_sub.c` at run time, so it cannot go stale
independently of the code. Run it from the repo root:

```sh
python3 tools/ui-text/strwidth.py --selftest        # 560 / 40 / 120 px
python3 tools/ui-text/strwidth.py "your new message here"
```

It measures the **proportional** path only. Do not use it on cell-based
draws.

### Measuring at run time

Prefer calling the engine over re-deriving the arithmetic. `sc_sub.h`
exports:

- `SSGetDrawSizePro(str)` — width in px.
- `SSWrapStrPro(str, max_w, lines, max_lines)` — greedy word-wrap into
  `SSProLine` records (offset, length, measured width). Returns the number
  of lines the whole text needs, which can exceed `max_lines`.
- `SSPutStrProWrapP(flag, x, y, line_h, max_w, max_lines, ...)` — draws the
  wrapped lines `line_h` apart. If the text needs more lines than allowed,
  the last drawn line is cut and marked `...` so the loss is visible.
- `SSFitStrPro(str, max_w)` — truncates **in place** with `...` and returns
  the final width. Last resort, for text with no vertical room.

All three measure through `SSGetDrawSizePro`, never through their own
arithmetic, so they cannot disagree with what `SSPutStrProP` draws.

`SSWrapStrPro`'s contract is asserted by `--test-ui-text-units`
(`src/test/test_ui_text.c`), which the gate runner discovers automatically.
It exists because the wrap had no test at all and could not acquire one by
accident: measured 2026-09-05, making it return 0 lines broke **nothing** in
the tree. Neither consumer can see that — `SSPutStrProWrapP` draws zero lines,
i.e. silently nothing, and `dp2p_overlay_log_layout` only logs — so the only
symptom was a blank region on a netplay refusal screen.

The refusal overlay logs its layout once per distinct status
(`direct_p2p_overlay.c` -> `dp2p_overlay_log_layout`), so the wrap can be
checked from a log without seeing the screen. On a host with a discoverable
romset the quickest way to make it fire is the test runner's PS2 pin:

```sh
build/host-nptest/3S-ARM.app/Contents/MacOS/3S-ARM \
  --test-enable --p2p-local-player 1 --p2p-remote-ip 127.0.0.1
# [direct_p2p] overlay line 3: 2 line(s) needed, 2 shown, budget 368 px x 8 lines (total 416 px): "..."
# [direct_p2p]   line 1: 360 px "Netplay needs arcade balance - test runner pins PS2"
# [direct_p2p]   line 2: 52 px "balance"
```

Note that a fresh, empty `THIRDSARM_HOME` boots with no log output at all
on the host (observed while setting this up; not investigated here), so
run it against the normal pref path.

## 2. Worst case, not the literal

The literal at a call site is rarely the widest thing it can draw. Before
deciding a site is safe, substitute the longest value each `%s` / `%d` can
carry, from the buffer or type that feeds it:

- `Netplay_RefuseArm` (`src/netplay/netplay.c`) composes
  `"Netplay needs arcade balance - %s"` with `ArcadeBalance_GetReason()`;
  that is `ps2_reason[192]` in `src/arcade/arcade_balance.c`, filled by
  every `set_ps2_reason(...)` call. The measured compositions:

  | reason (`set_ps2_reason` site) | composed width |
  | --- | --- |
  | `CPS3 ROM not found or failed content verification` | 560 px |
  | `config override balance=ps2` | 412 px |
  | `arcade adaptation failed for character %d` | 507 px |
  | `test runner pins PS2 balance (--test-balance ps2)` | 561 px |
  | `... bad char-data location for character %d (apfn=... afs_size=...)` | 1126 px |
  | `... could not read PS2 char data for character %d (AFS file ... )` | 1035 px |
  | `not initialized` | 314 px |

  Every reachable refusal overflowed a 384 px line, not only the one that
  was reported. The two `adapt_all_characters` reasons were additionally
  being cut at `char msg[120]` in `Netplay_RefuseArm` and at
  `s_status[128]` in `src/netplay/direct_p2p.c` before they reached the
  screen at all.

- `DirectP2P_GetStatusText()` returns `s_status`, written by `set_status`.
  Its literal sources are every `ConnectFail_UserText` return in
  `src/netplay/connect_fail.c` (widest: `Matchmaking refused us. Update, or
  host must forward.` at 376 px), the `set_status(...)` / `set_fail_msg`
  literals in `direct_p2p.c` (widest: `Code was being probed. Share the NEW
  code.` at 294 px), the minute counter `Waiting for player 2... (%u min)`
  (262 px at ten digits), the MIST reject reasons
  (`s_mist_reject_reason[128]` in `netplay.c`), and the refusal text above.

- Room code (overlay line 2): `ROOM_CODE_DISPLAY_LEN` in
  `src/netplay/room_code.h` is 12 glyphs, at most 96 px.

- Replay HUD names: `meta_p1_name[64]` / `meta_p2_name[64]` in
  `src/replay/replay_player.c`, filled by `copy_sanitized_name` from an
  external sidecar — 63 printable glyphs, up to ~500 px, plus the ` [S]`
  rank tag from `compose_name_label`.

- Localised text: the `src/sf33rd/Source/Game/message/en/` tables are
  consumed by `Setup_Message` (`effect/eff45.c`) ->
  `get_message_conn_data` (`effect/effb6.c`), a sprite-cell path. Neither
  file calls any `sc_sub.c` glyph routine, so they are outside this audit.

## 3. Audit result

Run at `1d00f359` over every caller of the routines in §1 outside
`sc_sub.c`, plus `sc_sub.c`'s own callers. Widths from
`tools/ui-text/strwidth.py` (proportional) or the cell/pixel arithmetic in
§1.

### Overflowed — fixed in this change

| site | worst-case string | width | budget | over by | fix |
| --- | --- | --- | --- | --- | --- |
| `src/netplay/direct_p2p_overlay.c` -> `DirectP2P_DrawOverlay`, line 3 | any refusal composition (table in §2) | 412–1126 px | 384 centred | 28–742 px | word-wrap: `SSPutStrProWrapP`, 368 px per line, 12 px pitch, up to 8 lines from y=120 |
| `src/netplay/netplay.c` -> `Netplay_RefuseArm` `msg[120]` | prefix + `ps2_reason[192]` | ~150 glyphs | 119 glyphs | cut | `msg[256]` |
| `src/netplay/direct_p2p.c` -> `s_status[128]` | same text via `DirectP2P_RefuseSession` | ~150 glyphs | 127 glyphs | cut | `s_status[256]`; `report_connect_outcome`'s `line[]` widened by the same 128 |
| `src/replay/replay_overlay.c` -> `ReplayOverlay_Draw`, `REPLAY_PLAYER_DESYNCED` | `REPLAY DIVERGED (frame %u) - can't reproduce this recording exactly` | 455 px (1 digit) – 530 px (10 digits) | 384 centred | 71–146 px | word-wrap at y=100, 368 px, up to 3 lines |
| `src/replay/replay_overlay.c` -> `draw_name_labels` | 63-glyph handle + ` [S]` | up to ~500 px | 376 from x=8 (P1); 376 to the right edge (P2) | up to ~130 px, and the two labels collide long before that | each label fitted to 176 px with `SSFitStrPro` (half the 368 px row, minus an 8 px gap) |
| `src/sf33rd/Source/Game/ui/sc_sub.c` -> `draw_training_input_history`, P2 column | `%2u` + direction + six buttons = 72 px | 320 + 72 = 392 px | 384 | 8 px | P2 anchor 320 -> 304, giving P2 the same 8 px edge margin P1 has |

### Overflows deliberately — left alone

`src/sf33rd/Source/Game/debug/Debug.c` -> `Check_Check_Screen` draws
`SSPutStr(50, ..., "0123456789")`, columns 50..59 = 400..480 px. It is the
screen-position ruler of the debug check screen, reachable only with
`Debug_w[70] == -16` and `test_flag` clear. Running off the edge is what a
ruler is for.

### Checked and cleared

Proportional (`SSPutStrPro*`):

- `src/sf33rd/Source/Game/system/sys_sub.c` -> `Disp_Copyright`: 337 px and
  349 px centred in 386 (x = 24 / 18, right edge 361 / 367); second line
  193 px left-anchored at that x.
- `src/sf33rd/Source/Game/system/pause.c` ->
  `dispControllerWasRemovedMessage`: x = 132, widest line 121 px -> 253.
- `src/port/sdl/netplay_screen.c` -> `NetplayScreen_Render`:
  `CONNECTING...` 88, `Connected!` 76, `Match found!` 88;
  `Netplay_GetConnectStatusText` (`s_connect_status[96]`, three formats)
  widest 348 px at a ten-digit second count.
- `src/port/sdl/netstats_renderer.c` -> `NetstatsRenderer_Render`:
  `R:%d P:%d` at most 196 px, left-anchored at x = 2.
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c` ->
  `frame_data_overlay_draw`: `S%d A%d R%d T%d ` + `%+d` over `s32` frame
  counts, 255 px at five digits each; centred by its own measured width.
- `src/replay/replay_overlay.c` -> `draw_exit_hint` 212 px,
  `REPLAY COMPLETE` 116 px; `src/replay/replay_shuffle.c` ->
  `draw_skip_hint` 212 px, `NEXT REPLAY...` 96 px. Both hints measure
  widest with the pip bar full, and both draw at `RPL_OVL_HINT_Y` (214) —
  but never in the same frame: `draw_exit_hint` self-gates off on a
  viewer-owned launch, where START is hold-to-skip rather than hold-to-exit.
- `direct_p2p_overlay.c` lines 1 and 2: mode labels (`CONNECTING` widest)
  and the 12-glyph room code.
- The `DirectP2P_GetStatusText` literal set (§2): all 58–376 px. They fit
  384 unwrapped; under the 368 px wrap budget the single 376 px string now
  takes two lines.

Cell-based (48 columns):

- `src/sf33rd/Source/Game/screen/entry.c`: `PRESS ANY BUTTON` col 16 -> 32,
  `PRESS 2P START` col 30 -> 44, `DE_X = {2, 27}` (`init3rd.c`) +
  `   PRESS 2P START` (17) -> 44, `     GAME OVER` (14) -> 41.
- `sys_sub.c` -> `Disp_Win_Record_Sub`: `zz` in {5, 43}; `WINS` at 43 ->
  47. `Disp_Score`: `Coin_Message_Data[3] = {17, 37}`, last digit column
  at most 38. `Disp_Personal_Count` passes `size = 0`, which `SSPutDec`
  returns on without drawing.
- `pause.c` / `menu.c`: `1P PAUSE` col 20 -> 28.
- `src/sf33rd/Source/Game/effect/eff10.c` (`button_string_data` via
  `SSPutStr2`): both `Setup_Button_Sub` callers pass x = 6, so `PLAYER 1`
  (row 7) at 6+29 -> 43, the 7-glyph button names (row 2) at 6+25 -> 38,
  `VIBRATION OFF` (row 3) at 6+22 -> 41; literal anchors are at most
  `0x1A` = 26 with a 16-glyph string at 19 -> 35.
- `count.c` timer cells 21..26 + at most 4; `flash_lp.c` / `eff92.c`
  `vmark_tbl` max 33 + 2; `vital.c` `Pl_Num * 27 + 21` = 48 (flush with
  the edge); `spgauge.c` `sa_frame` cells at most 47 (`sa_frame[3][48]`);
  `n_input.c` `rank_display_set` 36 + 1 + 2; `sc_sub.c` `player_name`
  37 + 5, `player_face` 43 + 5, grade plate 41 + 1 + 5, `ci_tbl` at most
  40, `nwdata_tbl` at most 40, `stun_gauge_waku_write` 37.

`SSPutStr_Bigger` (pixel anchors):

- `src/sf33rd/Source/Game/effect/effa3.c` `Letter_Data_A3` against the
  `effect_A3_init` anchors in `menu.c`: row 6 (268 px, `DIFFICULTY$...`)
  at 48 -> 316; row 16 (196) at 64 -> 260; groups 17–20 (at most 72) at
  264 -> 336; groups 2–5 (at most 120) at 230 -> 350; `option_groups`
  (at most 76) at 230 -> 306; row 0 (136) at 120 -> 256; row 11 (136) at
  112 -> 248; row 21 (72) at 51; row 22 (88) at 136 -> 224.
- `menu.c` -> `Training_Menu` `training_letter_data`: widest
  `BUTTON CONFIG.` at 0x8F -> 255, `PARRYING TRAINING` at 0x73 -> 251.
- `sc_sub.c` -> `draw_training_input_history` P1 column: 8 + 72 = 80.
- `sc_sub.c` -> `Training_Data_Disp`: `SSPutDec3` at 208 + 158 = 366 with
  11 px digits -> 377; `scfont_sqput3` at 208 + 76.

Not re-measured: `combo_message_set` / `combo_pts_set` (called from
`engine/cmb_win.c`) and `naming_set`, whose columns come from engine
tables rather than literals. They are cell-based and were not changed by
this work; they are listed so the next pass knows they were seen, not
cleared.

## 4. Decisions

- **Wrap, not truncate, for the refusal overlay.** The reason text is the
  only thing that tells a player *why* netplay refused; cutting it or
  shortening the source strings would destroy that. Line 2 (room code) is
  empty in the error state and nothing else draws below y=120 while the
  orchestrator owns the screen (`NetplayScreen_Render` returns right after
  `DirectP2P_DrawOverlay`), so the vertical room is real. 368 px keeps an
  8 px margin against the canvas edge on a CRT with underscan; 12 px pitch
  leaves 4 px between 8 px glyph rows; 8 lines end at y=212 inside 224.
- **The buffers were widened rather than the message shortened.** A
  refusal that reads `arcade adaptation failed: bad char-data location for
  character 19 (apfn=... to_chd=... afs_size=...)` is the whole diagnostic
  for a broken data install; truncating it at 119 glyphs is exactly the
  silent loss this note is about.
- **Truncation for the replay HUD names, and only there.** The row sits at
  y=48 directly above the play-field; a second line would land on the
  fighters, and the handles are external input we cannot shorten at the
  source. `SSFitStrPro` marks the cut with `...`.
- **The wrap helper lives in `sc_sub.c`, not in a netplay file**, because
  the next over-wide string will come from somewhere else (the replay
  overlay already had one).
- **Nothing here touches `GameState` / `GS_SAVE`.** Text layout is
  render-side only; `tools/rollback-determinism/run.sh` was not re-run and
  `EXPECTED_GAME_STATE_SIZE` is unchanged.

## 5. When adding a string

1. Decide which path draws it (§1) and what its anchor leaves as budget.
2. Substitute the widest value every format field can carry (§2).
3. Measure — `strwidth.py` for proportional text, arithmetic for the rest.
4. If it can exceed the budget, wrap (`SSPutStrProWrapP`) where there is
   vertical room; fit (`SSFitStrPro`) only where there is none; and say
   which in the comment at the call site.
5. Check the buffers on the way to the draw call hold the widest string,
   or the wrap will lay out a message that was already cut.
