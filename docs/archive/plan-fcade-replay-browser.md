# Plan: Fightcade Replay Browser for 3s-arm on MiSTer

**Status: PLAN — not started. Written 2026-07-21 against fork branch
`upstream-engine-fixes` @ `54c95d13` and upstream `crowded-street/3sx`
`upstream/main` @ `03c5d7a6`.**

Investigation notes: every factual claim below was re-verified on
2026-07-21 by reading the cited file/line, running the cited command, or
fetching the cited URL. Upstream content was read via
`git -C /Users/sb/Developer/3sx show upstream/main:<path>` (the local
upstream clone's working tree is on an unrelated dirty branch,
`flatpak-workflow2` — do not use its checked-out files). Line numbers
for upstream files are line numbers **within that git blob at
`03c5d7a6`**, cited as `upstream:<path>:NNN`. Fork citations are plain
`<path>:NNN` against `upstream-engine-fixes` @ `54c95d13`.

---

## 1. Executive summary

**What is being built:** on-MiSTer browsing/search/download/playback of
Fightcade `sfiii3nr1` (SF3 Third Strike arcade) replays, played back
through the 3SX engine itself (full rendering + audio), on the 32-bit
ARM HPS runtime. Delivery is staged so every stage is independently
shippable and abortable:

- **Stage A** — desktop offline pipeline green: port upstream's
  Fightcade tooling + STATCHECK harness into the fork; download real
  replays; measure the fork engine's replay-fidelity pass rate.
- **Stage B** — FBNeo-lite ingestion: a compact device-facing replay
  format ("3SR") generated from the pipeline, plus a decisive
  experiment on decoding the raw Fightcade input stream without FBNeo.
- **Stage C** — runtime replay-playback mode in the core (desktop
  first): a release-compiled replay player with rendering + audio.
- **Stage D** — 32-bit ARM validation on the MiSTer (playback + an
  on-device statcheck run).
- **Stage E** — on-device replay download client (plaintext TCP, no
  TLS needed).
- **Stage F** — browse/search UI in-game + a small HTTPS-fronting
  proxy service on the user's existing VPS.

**Key architectural decisions (each justified in §2/§4):**

1. **The Fightcade input stream (type=-13 records) is fully decoded** —
   verified from the emulator source, not guessed: 10 bytes per frame,
   P1 mask at bytes [0..1] LE, P2 mask at bytes [5..6] LE, bit layout
   start/up/down/left/right/fire1-6 (§2.3). FBNeo is **not** needed to
   obtain per-frame input words.
2. **FBNeo (crowded-street/fbneo-replay-runner) is still needed
   off-device initially** — to establish match-setup state (characters,
   SA, colors, challenger side, RNG seeds) and to serve as the
   frame-by-frame fidelity oracle (statcheck). Whether it can be
   removed entirely (direct-from-boot playback of the raw stream) is a
   Stage B experiment with a defined go/no-go, not an assumption.
3. **Playback = engine-native input injection**, not savestate loading.
   The 3SX engine cannot load FBNeo savestates; cross-arch state is
   explicitly unsupported anyway (§2.6). The fork already contains a
   proven menu-automation + per-frame `p1sw_buff/p2sw_buff` injection
   machine (`src/test/test_runner.c`, DEBUG-only) — Stage C ports that
   pattern into a release-compiled `replay_player` module.
4. **Device-facing format is a compact "3SR" file** (setup block + RNG
   seeds + per-frame input words, 4 bytes/frame — `{u16 p1, u16 p2}`,
   a 2-minute game ≈ 29 KB) rather than SCRD RAM archives (hundreds of
   KB/frame uncompressed). SCRD stays a desktop/CI validation artifact.
5. **Browse/search goes through a proxy on the user's VPS**
   (46.62.244.55, which already runs the Node.js rendezvous server the
   fork ships) because the core has **no TLS stack** and the Fightcade
   API sits behind Cloudflare requiring a browser-derived
   `cf_clearance` cookie (§2.8, §4.5). The proxy can also host the
   FBNeo preprocessing step if the Stage B experiment fails.
6. **UI is an in-game overlay** (SSPutStrProP text path, pad-driven),
   not the FPGA OSD: the OSD is a compile-time `CONF_STR` toggle menu
   with no dynamic list capability (§2.9).

---

## 2. Verified fact base

### 2.1 Upstream Fightcade tooling (all @ upstream/main `03c5d7a6`)

- `upstream:tools/fcade-replays/fcade_replay_tool.py` — three
  subcommands (`download`, `list-replays`, `bulk-download`;
  `build_parser` at :606).
  - Stream download: plaintext TCP to `ggpo.fightcade.com` (:23
    `DEFAULT_HOST`), default port 7100 (:24), local source port 6004
    with ephemeral fallback (`--local-port` default, :585;
    fallback logic :294-317).
  - Handshake (`do_handshake`, :171-200): raw u32be words
    `0x14`; `1,0`; `0,0x1D,1`; read one framed ack; `0x20`; `2`;
    `len,len,token`; `0x20`; `3`; `0x0C`; `len,token` — with a 15 ms
    default inter-send delay (:592 `--send-delay-ms`). Token =
    `<quarkid>.7` (:104-110).
  - Server messages are u32be-length-framed (`recv_frame`, :73-75).
    Known types: `3` metadata (length-prefixed strings — player names
    etc.; `_parse_metadata_type3` :205-220), `-12` zlib-compressed
    savestate (`_parse_minus12` :237-249), `-13` input records with
    u32be `record_size` and `record_count` header
    (`_parse_minus13` :223-234).
  - Outputs per replay dir: `frames.bin`, `summary.json`, `savestate`
    (first decompressed −12), `inputs` (concatenated −13 bodies,
    clamped to `record_size*record_count` each), plus `quark.json` and
    a top-level `bulk_manifest.json` for bulk runs (`download_replay`
    :268-364, `cmd_bulk_download` :484-553).
  - Search API: POST `https://www.fightcade.com/api/` with
    `{req:"searchquarks", gameid, limit, offset[, best, since,
    username]}` (:117-152); needs `Origin`/`Referer`/`User-Agent`
    headers and a Cloudflare `cf_clearance` cookie via `--cookie` or
    `FCADE_COOKIE`; 403 otherwise (:158-165).
- `upstream:tools/replay_preprocessor.py` — runs each downloaded
  replay through a runner executable: `runner <game> -replay-state
  <savestate> -replay-inputs <inputs> -headless -dump-ram-path <dir>`
  (:67-85), collects `game_N.scrd` archives per replay (:88-118).
  **Version-skew caveat:** since #281 it expects the runner to emit
  `.scrd` directly, but the *public* runner emits raw `.ram` frame
  files (§2.4). The #268 version of this script
  (`git show 52a395bd:tools/replay_preprocessor.py`, :15, :103-104)
  instead called `compress_ram_dumps` on the runner's `game_N/` dirs —
  that variant matches the public runner.
- `upstream:tools/compress_ram_dumps.py` — SCRD writer: magic `SCRD`,
  u16 frame count, 8-byte frame table entries, frame 0 stored full,
  later frames XOR-deltas, all zero-run encoded (constants :9-19,
  encoder :22-45).
- `upstream:tools/statcheck_runner.py` — runs a statcheck build with
  `--ram-archive <path> --headless` per `game_N.scrd` (:155), 2 s
  default timeout (:20), kills process groups, writes a failure
  report; exit 0 iff all archives pass (:196-199).
- Relevant upstream commits (verified present):
  `fc1f304f` "Fightcade replay downloader (#141)", `17c88628` "Replay
  bulk download (#261)", `32b2da3f` "Replay tool improvements (#265)",
  `3376518f` "Fcade replay tool improvements (#286)", `52a395bd`
  "Statcheck: Test tooling (#268)", `0e815462` "Statcheck: RAM dump
  compression (#266)", `81cae3d4` "Statcheck: Tooling improvements
  (#281)" (whose first bullet is "Consume SCRD archives instead of RAM
  dumps").

### 2.2 The FBNeo runner (`fbneo-replay-runner`) — provenance and behavior

- Source: **`github.com/crowded-street/fbneo-replay-runner`** (public;
  discovered via the crowded-street org listing; org has exactly 3
  repos: 3sx, 3s-decomp, fbneo-replay-runner). Cloned to scratchpad;
  single branch `master`, HEAD `ccf96ab` (2026-03-30) — **3.5 months
  older than upstream's #281 tooling**, hence the `.ram`-vs-`.scrd`
  skew above.
- README: "Allows playing back replays from fightcade.com… generating
  per-frame RAM dumps in a matter of seconds". Build (macOS):
  `make sdl 'BUILD_X86_ASM=' 'CPUTYPE=arm64' -j1` (README notes `-j1`
  is required — dependency race). README run example invokes
  `./build/fbneosdldarm64` (README.md:22 @ ccf96ab — the debug-build
  binary name; release and debug outputs live in separate build
  folders, so the exact name/dir varies by build config). ROM
  `sfiii3nr1.zip` goes in `roms/`.
- Replay implementation is `src/burner/sdl/run.cpp` in that repo:
  - Savestate: FBNeo GGPO blob — optional `'GGPO'` int header +
    headerSize + nAcbVersion, then a full `BurnAreaScan` payload
    loaded via a custom `BurnAcb` write callback
    (`ReplayLoadStateBlob`, run.cpp:285-323).
  - **Input stream decode (`ReplayApplyFrameInputs`,
    run.cpp:379-403): the `inputs` blob is consumed 10 bytes per
    frame; `masks[0] = frame[0] | frame[1]<<8` (P1),
    `masks[1] = frame[5] | frame[6]<<8` (P2) (:392-394); total size
    must be a multiple of 10 (:428). Bit mapping
    (`ReplayBitFromInfo`, :328-345): bit 1 = start, 2 = up, 3 = down,
    4 = left, 5 = right, bits 6-11 = fire1..fire6 (LP,MP,HP,LK,MK,HK;
    ":342 `n + 5; // fire 1..6 => LP..HK bits 6..11`"). Coin is never
    mapped; bytes 2-4/7-9 of each record are ignored.**
  - RAM dumps: CPS3 main RAM (512 KiB — matches `RAM_FRAME_SIZE
    524288` in upstream ram_archive.c:8) is dumped whenever the SH-2
    PC hits `0x06094d98` (run.cpp:76), bytes reversed within each
    32-bit word (:168), written as `game_%u/frame_%08u.ram` (:199) only
    while big-endian u16 at RAM offset `0x15438` == 2 ("in game",
    :77-78 — the same offset the fork calls `GAME_ROUTINE_OFFSET`,
    src/test/test_runner.c:31). A new `game_N` directory starts on
    each in-game→out-of-game transition (:181-186).

### 2.3 The −13 decode assessment (the "can we drop FBNeo?" question)

**Confirmed (cited above, §2.2):** −13 record bodies are per-frame
fixed 10-byte records carrying both players' input masks; the mapping
to game buttons is explicit in `ReplayBitFromInfo`.

**Two distinct input-word layouts exist on the 3SX side — do NOT
conflate them:**

- **Arcade RAM layout** (the word archived in CPS3 RAM at
  `P1SW_0`/`P2SW_0` and in the WCP `sw_lvbt` mirror — i.e. what SCRD
  frames contain and what the −13 decode targets): bits 0-3 =
  up/down/left/right, 4-6 = LP/MP/HP, **7-9 = LK/MK/HK, 12 = start**.
- **Engine SWKey layout** (what `p1sw_buff`/`p1sw_0` actually carry at
  runtime — include/sf33rd/AcrSDK/common/pad.h:6-23): bits 0-3 =
  directions, 4-6 = LP/MP/HP (SWK_WEST/NORTH/RIGHT_SHOULDER),
  **8-10 = LK/MK/HK (SWK_SOUTH/EAST/RIGHT_TRIGGER), 14 = start
  (SWK_START)**.
- **Conversion (arcade → SWK): LK 7→8, MK 8→9, HK 9→10,
  start 12→14** — implemented identically in the fork's
  `read_input_buff` (src/test/replay_game.c:12-26) and upstream's
  (upstream:src/test/test_runner.c:82-128, reading the WCP mirror).
  **Every `p1sw_buff`/`p2sw_buff`/`p1sw_0` injection site must consume
  SWK-layout words. Feeding arcade-layout words verbatim would put HK
  on SWK_LEFT_SHOULDER (bit 7) and start on SWK_LEFT_STICK (bit 12) —
  silent, total input corruption.** Steps B3, C1 and E1b carry this
  conversion requirement explicitly.

> **ERRATUM (2026-07-21, A3b review round-1 finding P-2):** the
> "Arcade RAM layout" bullet above is wrong to lump the WCP `sw_lvbt`
> mirror in with `P1SW_0`/`P2SW_0`. There are two distinct ARCHIVED
> input sources in an SCRD frame, and they are NOT the same layout:
>
> - **`P1SW_0`/`P2SW_0` words** — genuine arcade RAM layout (kicks
>   bits 7-9, start bit 12). These need the shift-convert described
>   above before any SWK-layout consumer can use them
>   (`src/test/replay_game.c:12-26`).
> - **WCP `sw_lvbt` mirror** — already ENGINE layout (kicks bits
>   8-10, bit 7 unused), because it is generated by the engine's own
>   `Convert_Data` table (`src/sf33rd/Source/Game/system/sys_sub.c:67`,
>   consumed by `Convert_User_Setting` at `sys_sub.c:101`), not read
>   raw off the arcade bus. The fork's statcheck runner
>   (`src/test/statcheck_runner.c`'s `read_input_buff`) reads `sw_lvbt`
>   and bit-tests 4-6/8-10 directly — it must NOT apply the arcade→SWK
>   shift-convert (7→8/8→9/9→10) on top, or kicks silently double-shift
>   onto the wrong SWK bits.
>
> Net effect on this plan: **§4.2 stays correct as written** — the 3SR
> extractor reads `P1SW_0`/`P2SW_0` (real arcade layout) and does need
> the shift-convert at load time. Only the statcheck runner's WCP-based
> path is exempt from that conversion.

So a pure-C translation fcade-mask → arcade word → SWK word exists and
is table-driven at every hop.

**Still unverified (no sample data exists on disk — searched
`/Users/sb/Developer/3sx{,-mister}` for `frames.bin`, `summary.json`,
`inputs`, `savestate`, `*.scrd`; all output dirs empty):**

- Actual `record_size`/`record_count` values in −13 headers (expect
  `record_size==10` given the runner's stride, but unconfirmed).
- What the first −12 savestate actually contains / where in the
  session timeline it sits (power-on boot vs mid-attract vs later).
  The runner *always* starts from the savestate, never from cold boot.
- Whether the −13 stream covers pre-game frames (character select) —
  strongly implied by the runner design (it plays inputs from the
  savestate point and *discovers* game boundaries by watching RAM) and
  by `quark.json.num_matches` (multi-game replays), but not proven.
- Savestate size (affects download time/storage).

**Consequence:** decoding −13 into input words is a solved problem;
*playing those inputs through 3SX without FBNeo* additionally requires
the 3SX engine to reproduce the arcade's frame-for-frame behavior from
the stream's starting state — which is exactly what the savestate
encodes and what we cannot load. Stage B therefore (a) ships the
decoder + the SCRD-derived compact format (no risk), and (b) runs a
bounded experiment on direct-from-savestate-free playback with a
defined go/no-go (§6, Step B3).

### 2.4 Upstream STATCHECK harness (@ `03c5d7a6`)

- CMake: `option(THREESX_STATCHECK …)` (upstream CMakeLists.txt line 22);
  defines `STATCHECK` (:75), swaps input driver defines
  `CRS_INPUT_DRIVER_SDL` → `CRS_INPUT_DRIVER_STATCHECK` (:85-86),
  suppresses `CHECKSUM` in Release when statcheck is on (:73). Sources
  are picked up by `file(GLOB_RECURSE GAME_SRC … src/*.c)` (:41) —
  same glob pattern as the fork (`GLOB_RECURSE GAME_SRC`, CMakeLists.txt:185), so newly added
  `src/**/*.c` files need no CMake source-list edits in either repo.
- `upstream:src/test/ram_archive.{c,h}` — SCRD reader: magic check,
  LE u16 entry count, LE u32 offset/size table, zero-run decode
  (:10-33), XOR-delta seek in either direction (:85-107),
  `RAM_FRAME_SIZE` 524288 (:8). Frames are exposed as `SDL_IOStream`s.
- `upstream:src/test/replay_game.{c,h}` — scans SCRD frames for the
  game-start signature `G_No[1]==2 && G_No[2]==0 && G_No[3]==0`
  (upstream replay_game.c line 36), then reads characters (`MY_CHAR_OFFSET`), SAs,
  `NEW_CHALLENGER_OFFSET`, `PLAYER_COLOR_OFFSET` (:39-49) and maps
  arcade→3SX character ids (`CHAR_ARCADE_TO_3SX`, :10-15).
- `upstream:src/test/test_runner.c` — phase machine
  (PHASE_TITLE→…→PHASE_GAME, :22-29) that drives menus by mashing
  buttons, seeds `Last_My_char2`/`Last_Super_Arts` (:193-196), sets
  cursors, colors via `color_to_keys` (:35-49), sets `New_Challenger`
  from the dump so "the game selects the correct stage" (:230),
  RNG-syncs from the pre-game frame (`sync_values` call :265; the
  function itself syncs only `Random_ix16`/`Random_ix32`,
  upstream:src/test/test_runner_compare.c:488-511), then per frame
  reads both players' inputs from the archived WCP `sw_lvbt` mirror
  (:277-278; offsets `WCP_OFFSET`/`+0x406`, :83) and injects them via
  `StatcheckInput_SetButtonState` (:150,288-289). Inter-round skips
  keyed on `C_NO_OFFSET`/`SCENE_CUT_OFFSET` (:153-158). Epilogue
  compares engine RAM vs the archive frame (`compare_values`) and
  exits 0/1.
- `upstream:src/platform/input/statcheck/statcheck_input.{c,h}` —
  trivial 2-pad injected-state input driver.
- `upstream:src/platform/app/sdl/sdl_headless_app.c` — statcheck main
  loop: `TestRunner_Prologue(); Main_StepFrame(); TestRunner_Epilogue();
  Main_FinishFrame();` (:59-72), dispatched from
  upstream:src/platform/app/sdl/sdl_app.c:504.
- `upstream:src/args.c` — `--ram-archive` (required, :51-53,75) and
  `--headless` (:76-78) under `#if STATCHECK`.
- `upstream:src/arcade/arcade_constants.h` — the CPS3 RAM offset map
  (GAME_TIMER 0x1136C … P1SW_0 0x6AA8C/P2SW_0 0x6AA90; PLW_OFFSET
  0x68C6C, WCP_OFFSET 0x26318, RANDOM_IX_16/32 0x155E8/0x155EA,
  SCENE_CUT 0x16D30, NEW_CHALLENGER 0x113DA, PLAYER_COLOR 0x15683).
- `upstream/better-replay-parsing` branch (merge-base `14999ade`,
  i.e. **old**, pre-#266/#268): adds `src/test/replay_match.c/.h` +
  `stb_ds.h`; parses inputs from per-frame `.ram` files at
  `P1SW_1_OFFSET 0x6AA8E`/`P2SW_1_OFFSET 0x6AA92` ("we read previous
  inputs because CPS3 updates input buffers at the end of a frame",
  replay_match.c:88-91) and tracks per-round segments. Useful as a
  reference for round/game segmentation; not a port target (superseded
  by main's SCRD flow).

### 2.5 Fork state (differences that matter)

- **The fork's `src/test/` is a heavily diverged DEBUG-only harness,
  not upstream's STATCHECK harness.** All fork test files are gated
  `#if DEBUG` (src/test/test_runner.c:1, replay_game.c:1, etc.;
  test_runner.h:1 also `|| ENABLE_PERF_TELEMETRY` for phase-name
  stubs). There is **no** `STATCHECK`/`THREESX_STATCHECK` anywhere in
  the fork's CMakeLists (grep: zero hits), no
  `src/test/ram_archive.c`, no `src/platform/input/` at all
  (`src/platform/` contains only `app/` and `video/`), no
  `sdl_headless_app.c`. The fork app loop is `src/main.c`
  `game_step_0()` (:541) calling `TestRunner_Prologue()` at :578 when
  `configuration.test.enabled` (CLI `--test-enable`, the
  `OPT_BOOLEAN(0, "test-enable", ...)` entry at src/args.c:508).
- Fork input injection: `TestRunner_Prologue` writes `p1sw_buff` /
  `p2sw_buff` directly (src/test/test_runner.c:1294-1475);
  `game_step_0` then latches `p1sw_0 = p1sw_buff` at src/main.c:596-597
  (guarded by `(Play_Mode != 3 && Play_Mode != 1) || Game_pause !=
  0x81`, :591). Netplay instead writes `p1sw_0/p2sw_0` directly in
  `advance_game` (src/netplay/netplay.c:900-901). There is **no**
  `CRS_INPUT_DRIVER_*` abstraction in the fork.
- Fork's replay-from-dump path already exists and works in DEBUG: the
  test runner's `initialize_data` (src/test/test_runner.c:1203-1282)
  parses per-frame `.ram` dumps (`frame_%08d.ram` under
  `--test-states`, src/test/test_runner_utils.c:9-14), extracts
  characters/SAs, collects per-frame P1SW/P2SW words, and the phase
  machine plays them back through a real rendered match. A second
  richer parser exists at src/test/replay_game.c (colors +
  new_challenger + `CHAR_ARCADE_TO_3SX`, :34-94) with header
  src/test/replay_game.h defining `ReplayGame`/`ReplayGame_Parse`/
  `ReplayGame_Destroy` — **symbol-collides with upstream's
  same-named STATCHECK files** (upstream replay_game.h declares
  `ReplayGame`/`ReplayGame_Init`/`ReplayGame_Destroy`); both would
  define `ReplayGame_Destroy` if DEBUG and STATCHECK were ever both
  on. The port must keep them mutually exclusive (§6 Steps A3a/A3b).
- Fork already has: `src/arcade/arcade_constants.h` (same offsets as
  upstream minus `SCENE_CUT_OFFSET`/`RANDOM_IX_*` — **has**
  RANDOM_IX_16/32 (:13-14) and everything else except upstream's
  `SCENE_CUT_OFFSET 0x16D30`, which is absent — diff by inspection),
  `stb_ds.h` (included by src/test/replay_game.c:8), and the #268
  `assert_equals` macro (commit `a9a4c11c`, +21 lines in
  src/test/test_runner_compare.c).
- Fork's stale Fightcade tool: `fcade-replays/fcade_replay_tool.py`
  (409 lines, old path at repo root, `download` subcommand only —
  `grep -c add_parser` = 1). Superseded by upstream
  `tools/fcade-replays/` (§2.1).
- The fork's celebrated "1,296 PASS / 53 XFAIL" suite (that figure is the
  state as of 2026-07-18 and is quoted here as a dated reference; the LIVE
  canon lives in `docs/frame-data-synthesis.md` § "Verification canon
  (live)", where it is generated from the golden tables rather than typed)
  is the
  **frame-data corpus suite** (`tools/frame-data/check_frame_data.py`
  diffing `frame_trace.c` logs against a scraped-oracle
  `expected.json`; docstring check_frame_data.py:1-27), driven by
  `.fdi` input scripts in training mode (src/test/input_script.h). It
  is a *frame-data measurement* suite, *not* a replay/statcheck suite —
  its XFAILs are measurement disputes vs a secondary oracle
  (docs/arcade-frame-data/CAPTURE.md:11-27 explains the oracle
  hierarchy). It shares infrastructure with replay playback only at
  the `TestRunner_Prologue` injection point. **The fork has never run
  upstream's Fightcade statcheck; its replay-fidelity pass rate is
  unknown** and is the single most important number Stage A produces.
- `docs/archive/plan-netplay-phase6.md:619-651` (Step 13) shows a previous
  "remote replay browser" concept — RmlUi `rmlui_network_replay_picker`
  against the 3sxtra lobby server. That entire RmlUi/3sxtra port was
  **abandoned 2026-04-29** (memory: project-netplay-port-strategy;
  RmlUi is docs-only in the fork — no source). Do not resurrect it;
  it is cited here only as prior art for "read-only replay picker".

### 2.6 Determinism & native replay machinery

- Netplay rollback with per-frame desync checksums proves same-arch
  determinism of the engine loop (GekkoNet `GekkoDesyncDetected`
  handling with local/remote checksums, src/netplay/netplay.c:1027-1040;
  djb2-sectioned state checksums in src/netplay/game_state.c:1462-1545).
- Cross-arch (32-bit MiSTer vs 64-bit desktop) determinism is
  **explicitly unsupported**: src/netplay/game_state.c:72-80 ("A MiSTer
  (32-bit) peer and a desktop (64-bit) peer will desync … within
  seconds"). Consequence: desktop statcheck results do NOT
  automatically transfer to ARM; Stage D re-runs statcheck on-device.
- The original game ships its own replay recorder: RLE-condensed
  12-bit input words + 4-bit repeat count (`Get_Replay`/
  `Setup_Replay_Buff`/`Replay`,
  src/sf33rd/Source/Game/system/sys_sub.c:1239-1355), buffer
  `_REPLAY_W.io_unit.key_buff[2][7198]` (include/structs.h:997),
  header with RNG seeds/characters/SA/colors
  (`Setup_Replay_Header`/`Get_Replay_Header`, sys_sub.c:1146-1200).
  This is the strongest possible precedent: **the engine already
  supports input-replay playback into `p1sw_0`/`p2sw_0`**
  (sys_sub.c:1330-1345) with RNG-seed restore. It is PS2-mode-shaped
  (MODE_REPLAY, VM memory-card format) and not directly reusable for
  arcade-mode Fightcade content, but its injection point and header
  fields define what a faithful setup block needs.

### 2.7 Networking primitives in the fork

- **No HTTP client, no TLS protocol stack.** Case-insensitive grep for
  `http` over `src/**/*.{c,h}` hits only imgui vendored headers and a
  comment in net_tuning.h; grep for `mbedtls|openssl|ssl_|tls` hits
  only a false positive (`tls` = "top-left s" texcoord variable,
  src/platform/video/software/software_renderer.c:699). Link set:
  `m, libminizip-ng.a, libtfpsacrypto.a, ZLIB::ZLIB, stdc++`
  (CMakeLists.txt:583-589) — `tf-psa-crypto` is the Mbed-TLS
  *crypto-only* package (fetched from
  `github.com/Mbed-TLS/TF-PSA-Crypto/releases`, build-deps.sh:944-984;
  used for PSA SHA-256, src/utils/sha256.c:17-29). No `libmbedtls`
  (TLS layer) is built or linked.
- **zlib IS already linked** (`find_package(ZLIB REQUIRED)`
  CMakeLists.txt:538, `ZLIB::ZLIB` :587; in-tree consumer
  src/sf33rd/Source/Compress/zlibApp.c:9) — the −12 savestate inflate
  needs nothing new.
- TCP client via SDL3_net exists (matchmaking `NET_CreateClient`,
  src/netplay/matchmaking.c:77, with `NET_GetAddressStatus` DNS
  states :74-76); raw POSIX UDP + `getaddrinfo` also in use
  (src/netplay/stun.c:250-258). SDL3_net + GekkoNet + miniupnpc are
  linked only under `ENABLE_NETPLAY` (CMakeLists.txt:598-609).
- Rendezvous server: Node.js UDP service (tools/rendezvous-server/
  `rendezvous-server.js`, `package.json`, systemd unit, `deploy.sh`),
  binary protocol REGISTER(28)/DELIVER(32)/POLL(28) with magic
  `0x33535852` (`MAGIC`, tools/rendezvous-server/__test_protocol.js:22);
  live at `udp://46.62.244.55:3478` (the
  `CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL` default,
  src/port/config/config.c:104).
- `third_party/cJSON/{cJSON.c,cJSON.h}` is vendored but currently
  **unwired** (zero grep hits in CMakeLists.txt/build-deps.sh) —
  available for device-side JSON if wanted.

### 2.8 Fightcade service surface

- Replay metadata schema (verified from
  `xBiggs/fightcade-api` — fetched
  `src/fightcade-api.ts` via `gh api`, ReplaySchema at :126-139):
  `quarkid, channelname, date (ms epoch), duration (s), emulator,
  gameid, num_matches?, players[], ranked, replay_file?,
  realtime_views?, saved_views?`; Player = `name, country, rank?,
  score?, gameinfo?` (:94-100).
- Stream URL scheme `fcade://stream/<emulator>/<gameid>/<quarkid>,<port>`
  (upstream tool `parse_fcade_url`, :78; list rows built with token
  `f'{quarkid}.7'` and port 7100, :99-113).
- The search API's Cloudflare gate is browser-derived and expiring;
  upstream's own workflow is "copy cookie from a browser session"
  (upstream:tools/fcade-replays/README.md, Notes section). The
  **stream** endpoint (`ggpo.fightcade.com:7100`) has no such gate —
  plaintext TCP, no cookie (the entire `download` path never sends
  one).
- Fightcade's FBNeo is public: `github.com/fightcadeorg/fightcade-fbneo`
  (referenced in docs/arcade-frame-data/CAPTURE.md:64-66); the replay
  runner is a fork of it (§2.2).

### 2.9 UI surfaces & wrapper

- FPGA OSD: compile-time `CONF_STR` in vendor/Menu_MiSTer/menu.sv
  (:278-290; "O[30],Arcade Balance,Off,On;" at :280, "T[29],Play
  Online;" at :281 — line numbers at the pinned `54c95d13`; the file
  gained 4 lines by current HEAD), rendered by the vendored wrapper
  (`OsdSetSize(8)` — 8 rows — vendor/Main_MiSTer/thirdsarm_wrapper.cpp:1863;
  status reads via `user_io_status_get("[NN]")`, e.g. :1859 for
  Arcade Balance). Upstream MiSTer `menu.cpp` is **not** vendored
  (`ls vendor/Main_MiSTer/*.cpp` — no menu.cpp), so there is no
  file-browser/list widget to reuse. Verdict: OSD is fine for a
  single "Replays" trigger, unusable for a dynamic browse list.
- In-game dynamic text: `SSPutStrProP(flag=1, width, y, atr, color,
  str, priority)` centers text on the 384x224 canvas
  (src/sf33rd/Source/Game/ui/sc_sub.h:45; usage pattern + priority
  conventions documented in src/netplay/direct_p2p_overlay.c:23-39,
  which draws 3 dynamic lines during P2P setup). The overlay itself is
  render-only (no input handling; 114 lines).
- Wrapper→game mode-launch pattern (the model for launching a replay
  browser/player): OSD intent → wrapper writes an intent file
  (`/tmp/3s-arm-netplay.handoff`, thirdsarm_wrapper.cpp:68) →
  `direct_p2p_arm_and_restart()` (:2325-2327) → wrapper relaunches the
  runtime injecting `--direct-p2p-handoff <path>` into child argv
  (:2797-2812). `kRuntimeHome = "/media/fat/games/3s-arm"` (:56) is
  the writable runtime home (config, logs).
- In-game "cold-launch straight into a mode" precedent:
  `NetplayNav_Tick` auto-drives Title→Menu→VS-mode setup with injected
  `SWK_START` presses (src/netplay/netplay_nav.c:1-58 state machine
  comment; called from src/main.c:589 before the input latch).
- ImGui is `#if DEBUG` only (src/imgui/imgui_wrapper.c:1); RmlUi has
  no source in the fork.

### 2.10 Build & deploy (fork)

- Canonical MiSTer build: `tools/mister/build-game.sh --flavor
  telemetry` (AGENTS.md:20-21; always prefer telemetry per
  AGENTS.md:19). Inner configure is hardcoded
  `-DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON
  -DENABLE_PERF_TELEMETRY=<flag>` with `EXTRA_CMAKE_ARGS` tokens
  appended **after** (build-game.sh:172-174) — so an
  `EXTRA_CMAKE_ARGS="-DTHREESX_STATCHECK=ON"` (Stage D) needs no
  script change, and a later `-DCMAKE_BUILD_TYPE=…` token would win
  over the hardcoded one (CMake last-wins).
- Desktop build: `sh build-deps.sh --profile desktop` then
  `CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build
  build --parallel` (docs/building.md:60-70). DEBUG features come from
  `$<${DEBUG_HOOKS_GENEX}:DEBUG>` (CMakeLists.txt:271).
- Deploy: `misterctl.sh deploy --src build/mister-<flavor>-package`
  to `/media/fat/games/3s-arm/` (docs/mister-runbook.md:341-395;
  MISTER_PASSWORD=1; lock/busy preflight described there). RBF is a
  separate wrapper deploy — not touched by this plan.
- `file(GLOB_RECURSE GAME_SRC CONFIGURE_DEPENDS src/*.c)`
  (`GLOB_RECURSE GAME_SRC`, CMakeLists.txt:185) — new C files under `src/` are auto-picked-up.

---

## 3. Open questions & risks

Each: what's unknown → why it matters → how/when resolved → fallback.

1. **−13 header values & exact stream timeline** (record_size actually
   10? does the stream start at power-on? does it include character
   select? savestate size?). Matters for Stage B/E design and storage
   math. Resolved: Stage A Step A2 downloads real replays and inspects
   `summary.json` + hexdumps. Fallback: none needed — A2 is cheap and
   unconditional.
2. **Fork engine's Fightcade-replay fidelity (statcheck pass rate).**
   The fork's engine has diverged (perf work, netplay hooks, MiSTer
   rendering) and only a small set of upstream engine fixes is
   cherry-picked (23 commits on `mister..upstream-engine-fixes`
   including follow-ups/docs — `git log --oneline
   mister..upstream-engine-fixes | wc -l` = 23); upstream built statcheck
   specifically because engine inaccuracies break replay playback. A
   low pass rate turns "watch a replay" into "watch a replay diverge".
   Resolved: Stage A Step A4 produces the number; Stage C consumes
   only replays that statcheck passes end-to-end (browser can
   pre-filter server-side later). Fallback: cherry-pick more upstream
   engine-accuracy fixes (the current branch exists precisely for
   this) and re-measure; ship playback gated to verified replays.
3. **Direct-from-boot playback without FBNeo (Stage B experiment).**
   Unknown whether 3SX from its own power-on state, fed raw −13
   inputs, stays aligned with the arcade session (frame offset,
   attract/menu timing, RNG init). Matters: removes desktop/VPS
   preprocessing from the loop entirely. Resolved: Step B3 measures
   alignment against SCRD ground truth. Fallback (default path):
   SCRD-derived 3SR generation stays off-device (desktop now, proxy
   VPS in Stage F).
4. **32-bit ARM behavior divergence.** game_state.c:72-80 documents
   32-vs-64-bit desync; a replay that passes desktop statcheck may
   still diverge on the MiSTer. Resolved: Stage D Step D2 builds the
   statcheck flavor for ARM and runs the same archives on-device.
   Fallback: if ARM-specific divergence appears, it is an engine bug
   class the netplay work already hunts (same-arch MiSTer↔MiSTer
   netplay is the shipping config); file and fix per-site, or restrict
   Stage C/E to "best effort" playback with a divergence detector
   (compare a few checksum frames from the 3SR file at runtime).
5. **`cf_clearance` lifetime & automation.** The search API needs a
   browser-derived Cloudflare cookie that expires (upstream pastes it
   manually, §2.8). Matters only for Stage F browse/search (stream
   downloads are ungated). Resolved: Stage F proxy design (§ Step F1)
   keeps the cookie server-side with a manual-refresh admin path
   first; catalog caching means one refresh serves many device
   queries. Fallback: scheduled headless-browser refresh on the VPS,
   or a fully offline update_all-style catalog file pushed to SD.
6. **Public runner vs #281 skew** (`.ram` vs `.scrd` emission). The
   public `fbneo-replay-runner` @ ccf96ab emits raw `.ram`; upstream's
   current preprocessor expects `.scrd` from the runner. Resolved:
   Stage A ports the tool pair pinned to the *public* runner's
   contract (use `compress_ram_dumps` after the runner, as the #268
   preprocessor did — `git show 52a395bd:tools/replay_preprocessor.py`).
   Fallback: if crowded-street pushes their newer runner, drop our
   compress step and re-sync with upstream's #281 script verbatim.
7. **Runner buildability where we need it.** Verified README recipe
   only for macOS/arm64 (`make sdl 'BUILD_X86_ASM=' 'CPUTYPE=arm64'
   -j1`). Stage F wants it on a Linux x86 VPS; the makefile.sdl exists
   but a Linux build is unverified. Resolved: attempted during Step F1
   provisioning. Fallback: preprocess on the Mac (Stage A tooling) and
   rsync 3SR files to the VPS catalog, or run the macOS binary in CI.
8. **Legal/ToS posture.** The stream protocol was reverse-engineered
   upstream ("work-in-progress reverse engineering helper",
   upstream:tools/fcade-replays/fcade_replay_tool.py:2); Fightcade's
   ToS were not reviewed in this investigation (not searched — out of
   scope). Matters for public release framing. Resolved: user
   decision before any public release of Stage E/F builds (releases
   are user's own repo per memory feedback-releases-are-ours).
   Fallback: keep Stage E/F builds private/friend-only.
9. **MiSTer storage headroom.** Free space on the target SD was not
   measured (device not probed during planning). 3SR files are tiny
   (4 B/frame ≈ 14 KB/min + setup block, §4.2), but SCRD archives for
   Stage D validation can be large (frame 0 stored full = 512 KiB
   pre-RLE; delta frames unknown without samples). Resolved: Step A2
   measures real sizes; Step D2 checks `df` before copying. Fallback:
   validate on-device with a small archive subset.

---

## 4. Architecture decisions in detail

### 4.1 Playback = input injection through the existing latch

The only injection point that reaches every consumer correctly is the
`p1sw_buff → p1sw_0` latch in `game_step_0` (src/main.c:591-599); the
DEBUG test runner (src/test/test_runner.c:1294-1475) and netplay
(src/netplay/netplay.c:900-901) both feed it. The replay player reuses
the *fork's* phase machine (title→menu→char-select→game), not
upstream's, because the fork's already handles this repo's menu flow,
stage overrides and training quirks, and is field-proven by the
frame-data suite. Match setup is seeded exactly as upstream statcheck
does: `Last_My_char2`/`Last_Super_Arts` (fork test_runner.c:1321-1323
≙ upstream :193-196), cursor tables (fork :73-75), color keys
(upstream :35-49 — fork currently lacks color selection; port it),
`New_Challenger` for stage choice (upstream :230), RNG sync
(`Random_ix16/32`, upstream compare.c:488-511). **Layout caveat
(§2.3): `p1sw_buff`/`p1sw_0` carry engine SWK-layout words, not
arcade-RAM-layout words — every replay source (SCRD, 3SR, −13 decode)
produces arcade-layout words and must be shifted through the
`read_input_buff`-style conversion before injection.**

### 4.2 3SR: the device-facing replay format (new, this repo)

Per game (a Fightcade replay has `num_matches` games): a small header
`{magic "3SR1", game meta (characters[2], supers[2], colors[2],
new_challenger, seeded Random_ix16/Random_ix32), frame_count,
optional sparse checksum table}` + `frame_count × {u16 p1, u16 p2}`
input words in **arcade-RAM layout** (exactly as archived at
`P1SW_0`/`P2SW_0`; consumers convert to engine SWK layout at load
time per §2.3). Everything in the header is exactly the set
upstream statcheck seeds before PHASE_GAME (§4.1) — no more, no less.
Sizes: 2-minute game ≈ 7,200 frames ≈ 29 KB. Generated (Stage B) from
SCRD archives by a Python extractor (same reads replay_game.c/
test_runner.c already do); later possibly on-device from −13 (Step B3
gate).

### 4.3 Statcheck port strategy (fork-shaped, not verbatim)

Upstream's harness assumes three things the fork does not have: the
`CRS_INPUT_DRIVER_*` abstraction, `src/platform/app/sdl/` app drivers,
and `args.statcheck`. Porting verbatim is impossible; porting the
*logic* is straightforward because the fork's own DEBUG runner is the
same lineage. Plan: bring `ram_archive.{c,h}` verbatim (pure format
code, zero engine deps), bring upstream `replay_game`/`test_runner`
logic as new STATCHECK-gated files with **renamed symbols**
(`ScrdGame_*`, `Statcheck_*`) so DEBUG and STATCHECK can never
collide (`ReplayGame_Destroy` exists in both trees, §2.5), inject via
`p1sw_buff` (fork style, SWK-layout words per §2.3) instead of an
input driver, add a `--ram-archive` arg, and gate with a new
`THREESX_STATCHECK` CMake option mirroring
upstream CMakeLists.txt lines 22, 73 and 75. Add `message(FATAL_ERROR)` if both
Debug config and THREESX_STATCHECK are selected.

**Headless caveat (verified):** the fork's `--headless` flag is parsed
but consumed by *nothing* — grep over `src/` hits only the
configuration field (`bool headless`, src/configuration.h:204) and the
arg definition (`OPT_BOOLEAN(0, "headless", ...)`, src/args.c:298),
zero readers — and the fork's main loop is real-time
frame-paced (`target_frame_time_ns = 1e9/TARGET_FPS`,
src/port/sdl/sdl_app.c:89). Upstream's statcheck loop is by contrast
uncapped and render-free (sdl_headless_app.c:59-72). A STATCHECK build
that runs windowed at 1× real time would (a) take the full match
duration per archive and (b) be killed by `tools/statcheck_runner.py`'s
2 s default timeout (:20). Step A3b therefore wires `--headless` for
real in the STATCHECK build: skip presentation and skip the frame-pace
wait (uncapped stepping), adapting the semantics of upstream's
`sdl_headless_app.c` into the fork's loop.

### 4.4 On-device downloader placement: game process, worker thread

The wrapper could do it, but the game process already owns SDL3_net,
zlib, the config system, and the UI that must show progress; the
direct-P2P orchestrator already establishes the worker-thread +
polled-state pattern (src/netplay/direct_p2p_overlay.c:9-14 reads
state non-blockingly). Downloader = plain POSIX/SDL3_net TCP client
implementing §2.1's framing/handshake, zlib-inflating −12, writing
`/media/fat/games/3s-arm/replays/<quarkid>/…`. No TLS anywhere on
this path (verified: the Python downloader sends no cookie and speaks
raw TCP).

### 4.5 Browse/search: proxy on the user's VPS (chosen), alternatives rejected

- **(a) CHOSEN — VPS proxy.** The user already operates a Node.js
  rendezvous service on 46.62.244.55 with a deploy script + systemd
  unit (§2.7). A sibling Node service ("fcade-proxy") terminates
  HTTPS+Cloudflare toward fightcade.com (Node has TLS built in;
  cookie kept server-side), exposes a dead-simple length-framed JSON
  protocol over plain TCP to the device, and caches search pages so
  most device queries never touch Fightcade. Natural extension of
  existing ops; device side needs only what it already links
  (SDL3_net TCP + cJSON, wired in Steps F2a/F2b).
- **(b) REJECTED for now — TLS in the core.** Would require building
  mbedtls (TLS layer) for the armhf cross sysroot on top of the
  existing tf-psa-crypto, plus an HTTP/1.1 client, plus **Cloudflare
  cf_clearance still can't be obtained on-device** (browser-derived).
  TLS alone doesn't solve the API gate — the proxy is needed anyway,
  so on-core TLS buys nothing for strictly more build-system risk.
- **(c) FALLBACK — offline catalog file.** update_all-style: a cron
  job (anywhere) writes a catalog JSON + pre-made 3SR files; user
  syncs to SD; browser reads local files only. Zero new device
  networking. Kept as the degraded mode of (a) — the browser UI reads
  a local catalog format that the proxy client merely refreshes, so
  (c) is (a) minus the refresh.

### 4.6 UI: in-game overlay browser

OSD gets one new static line at most (launch trigger; same
`T[NN]`-style pattern as "Play Online", menu.sv:281). The browser
itself is an in-game screen: `SSPutStrProP` text (≈8-10 rows of list
at 12-16 px pitch fits 384x224 comfortably — direct_p2p_overlay uses
y=70/100/120 for 3 lines), pad-edge navigation reading `p1sw_0`/
`p1sw_1` rising edges (the engine's own convention — see the
rising-edge comment `~p*sw_1 & p*sw_0 & SWK_START` above
`NetplayNav_Tick()`, src/main.c:584-589). Search text entry (player names) via a
character-grid picker driven by the pad — same interaction as the
game's own name-entry; keyboard optional later (wrapper input side,
out of scope).

---

## 5. Stage map & dependencies

```
A1 → A2 → A4            (tooling → samples → fidelity number)
A2 → A3a → A3b → A4     (statcheck engine port: scaffolding → runner)
A4 → D2                 (ARM statcheck — run immediately after A4;
                         closes the 32-bit-divergence risk (risk 4)
                         before any C-stage work is built)
A2 → B1 → B2 → C1       (decoder → 3SR format → runtime player)
B1 → B3                 (experiment; gates Stage E's conversion placement)
C1 → C2 → C3 → D1       (player → controls → desktop QA → device)
A2 → E1a                (desktop stream-client bring-up)
E1a + D1 + B3 → E1b     (on-device fetch + conversion placement)
C2 → F2a                (local-only browser UI)
F2a + E1b + F1 → F2b    (remote browse/download integration)
F2b → F3                (search & filters)
F1 independent server-side; F2b depends on F1 (or its offline fallback)
F4 (launch wiring + storage lifecycle): after F2b
```

Every stage ends at a shippable state: A = CI-style fidelity tooling;
B = offline "make me a 3SR" pipeline; C = "watch this 3SR file"
desktop; D = same on MiSTer (pre-fetched replays — the original
"Stage B" of the request); E = on-device fetch by quark id; F = full
browse/search/download/watch. Note D2 is scheduled out of stage order
(right after A4): its only real prerequisites are the statcheck build
(A3b) and the desktop baselines (A4), and it retires the plan's
biggest unknown — 32-bit ARM divergence — before the Stage B/C player
work is invested.

---

## 6. Step sequence

Conventions for every step: run from repo root
`/Users/sb/Developer/3sx-mister`; MiSTer builds use
`tools/mister/build-game.sh --flavor telemetry` (AGENTS.md:19-21);
never edit `build/` (AGENTS.md:15); deploys only via
`tools/mister/misterctl.sh` per docs/mister-runbook.md:341-395; no
pushes; commit per repo convention. Steps are sized ≤ ~2 h of agent
work.

---

### Step A1 — Port the Fightcade replay tooling (Python) into the fork

**Why:** everything downstream (samples, statcheck, 3SR) consumes
these tools; the fork's only copy is the stale 409-line pre-#261
`fcade-replays/fcade_replay_tool.py`.

**Read first:**
- `upstream:tools/fcade-replays/fcade_replay_tool.py` +
  `upstream:tools/fcade-replays/README.md` +
  `upstream:tools/fcade-replays/.gitignore` (via
  `git -C /Users/sb/Developer/3sx show upstream/main:<path>`)
- `upstream:tools/compress_ram_dumps.py`
- `upstream:tools/statcheck_runner.py`
- `git -C /Users/sb/Developer/3sx show 52a395bd:tools/replay_preprocessor.py`
  (the `.ram`-consuming #268 variant) **and** the current
  `upstream:tools/replay_preprocessor.py` (#281 `.scrd` variant)
- fork `fcade-replays/fcade_replay_tool.py` (to confirm it is a
  strict ancestor before deleting)

**Create/modify:**
- `tools/fcade-replays/fcade_replay_tool.py`, `README.md`,
  `.gitignore` — verbatim from upstream/main.
- `tools/compress_ram_dumps.py` — verbatim from upstream/main.
- `tools/replay_preprocessor.py` — **the #268 variant** (imports
  `compress_ram_dumps`, consumes the public runner's `game_N/*.ram`
  dirs), with a header comment citing the skew (§3 item 6) and
  upstream #281 for future re-sync.
- `tools/statcheck_runner.py` — verbatim from upstream/main; note it
  imports `rich` (statcheck_runner.py:16) — add `rich` to a
  `tools/requirements-python.txt` if the fork lacks one (upstream has
  `tools/requirements-python.txt`; check and port).
- Delete `fcade-replays/` (stale root-level copy) in the same commit,
  README noting the move.

**Success criteria:**
- `python3 tools/fcade-replays/fcade_replay_tool.py --help` lists
  `download`, `list-replays`, `bulk-download`.
- `python3 tools/replay_preprocessor.py --help` and
  `python3 tools/statcheck_runner.py --help` exit 0.
- `python3 -c "import sys; sys.path.insert(0,'tools'); import
  compress_ram_dumps"` succeeds.
- `git status` shows `fcade-replays/` gone, new files under `tools/`.

**Depends on:** nothing.

**What NOT to do:** no engine/C changes; no edits to the tools beyond
the documented #268 pin and requirements; do not run downloads yet.

**If it fails:** trivial file-copy step — failures mean upstream
paths moved; re-verify with
`git -C /Users/sb/Developer/3sx ls-tree upstream/main tools/`.

---

### Step A2 — Build the public FBNeo runner; download & dissect real samples

**Why:** resolves open questions 1, 6, 9 (−13 header values, stream
timeline, sizes) with primary evidence; produces the raw material for
every later step. No repo-code dependency on A1's port besides the
tool itself.

**Read first:**
- README of `github.com/crowded-street/fbneo-replay-runner`
  (build: `make sdl 'BUILD_X86_ASM=' 'CPUTYPE=arm64' -j1`; needs
  `roms/sfiii3nr1.zip`)
- `src/burner/sdl/run.cpp` in that repo (:379-403 input decode,
  :146-215 dump logic)
- `tools/fcade-replays/README.md` (ported in A1) for command lines

**Create/modify:**
- Clone the runner **outside the repo** (e.g. `~/Developer/` or
  scratch; it must not enter the fork tree). Build it; obtain
  `sfiii3nr1.zip` from the user as an **explicit user-provided
  input** — docs/arcade-frame-data/CAPTURE.md:31-38 documents the
  local FightCade2 app's ROM directory but for `sfiii3.zip` (the Euro
  990608 set), NOT the `sfiii3nr1.zip` the runner README requires;
  whether the nr1 set is present there must be confirmed with the
  user, not assumed.
- Download 3-5 replays: `list-replays` needs `FCADE_COOKIE` from the
  user's browser (ask once); `download --fcade-url` for any known
  quark URL needs no cookie.
- Run the runner + `tools/replay_preprocessor.py` (A1) over them →
  `game_N.scrd` per game.
- Write findings into `docs/fcade-replay-notes.md` (new, small):
  observed `record_size`/`record_count` per −13 message
  (`summary.json`), savestate byte size and decompressed size,
  `inputs` size vs `duration*60*10`, whether early frames show menu/
  char-select activity (hexdump first records), `.scrd` sizes per
  game, and the first-game start offset. This doc is the citation
  anchor replacing every "unverified" in §2.3.

**Success criteria:**
- Runner binary builds and `-headless` run over one replay produces
  `game_1/frame_00000000.ram` files of exactly 524,288 bytes each.
- `compress_ram_dumps`-produced `.scrd` opens under the A3a/A3b
  harness later (deferred check) — for now `xxd game_1.scrd | head -1`
  shows `SCRD`.
- `docs/fcade-replay-notes.md` records the numbers above with exact
  commands.

**Depends on:** A1 (tool + preprocessor). User inputs:
`sfiii3nr1.zip` (explicit — not covered by CAPTURE.md, which documents
`sfiii3.zip`; see above), `FCADE_COOKIE` (only for list; direct URLs
work without).

**What NOT to do:** don't commit ROMs, savestates, or any downloaded
replay content (respect upstream's `.gitignore` pattern for outputs);
don't bulk-download hundreds yet (be a polite client; ≤5 replays,
default `--delay`).

**If it fails:**
- Runner build failure on macOS: retry `-j1` (documented race); if
  clang errors, check the repo's appveyor/makefile.sdl for expected
  SDL1.2/SDL2 deps and install via brew.
- Stream connect timeouts: retry with `--local-port 0`, raise
  `--max-idle-timeouts` (README example uses 20). If Fightcade
  changed the handshake, capture divergence in the notes doc and stop
  — that's a plan-level input, not a workaround situation.

---

### Step A3a — STATCHECK scaffolding: CMake gate, SCRD reader, args (fork-shaped)

**Why:** the build-system + format-reader half of the statcheck port
(§4.3), carved out so each /implement cycle stays ≤ ~2 h: after this
step a STATCHECK-gated build exists, reads SCRD archives, and provably
does not disturb normal builds — before any runner logic lands.

**Read first:**
- Fork: `src/test/replay_game.{c,h}` (symbol-collision counterpart),
  `src/args.c` (`--test-*`, `--headless` blocks), `src/configuration.h`
  (TestRunnerConfiguration), `CMakeLists.txt` (defines block :124
  area, glob :93), `src/arcade/arcade_constants.h`.
- Upstream (via `git show upstream/main:`): `src/test/ram_archive.{c,h}`,
  `src/test/replay_game.{c,h}`, `src/args.c` statcheck block,
  `CMakeLists.txt:22,73-90`.

**Create/modify:**
- `CMakeLists.txt` — `option(THREESX_STATCHECK "" OFF)`; when ON:
  `add_compile_definitions(STATCHECK)` for the target, and
  `message(FATAL_ERROR …)` if `CMAKE_BUILD_TYPE` is Debug (symbol
  collision guard, §2.5). Keep telemetry/netplay flags orthogonal.
- `src/test/ram_archive.{c,h}` — verbatim upstream, keeping
  upstream's `#if STATCHECK` gate (the CMake define above matches).
- `src/test/scrd_game.{c,h}` (new) — upstream `replay_game.c` logic
  with renamed type/symbols (`ScrdGame`, `ScrdGame_Init/Destroy`),
  `#if STATCHECK`.
- `src/args.c`/`src/configuration.h` — `--ram-archive` string under
  `#if STATCHECK`, required-check mirroring upstream args.c:50-54.
  (The existing `--headless` flag stays as-is here; it is currently
  parsed but read by nothing — A3b wires its consumption.)
- Port `SCENE_CUT_OFFSET 0x16D30` into
  `src/arcade/arcade_constants.h` (missing in fork, §2.5).
- A tiny STATCHECK-only smoke path: on startup with `--ram-archive`,
  open the archive, log frame count + parsed `ScrdGame` setup fields,
  and exit 0 — verifiable without any runner logic.

**Success criteria:**
- Desktop: `CC=clang cmake -B build-statcheck
  -DCMAKE_BUILD_TYPE=Release -DTHREESX_STATCHECK=ON && cmake --build
  build-statcheck --parallel` builds clean (**-Werror-sensitive**: the
  fork builds with warnings promoted in places — see commit
  `6740d726` precedent for -Werror fallout).
- Smoke: `./build-statcheck/<binary> --ram-archive <A2 game_1.scrd>`
  logs the expected frame count and characters/SA from A2's notes.
- `-DTHREESX_STATCHECK=ON` + Debug config errors out at configure
  time (collision guard fires).
- Normal builds unaffected: `tools/mister/build-game.sh --flavor
  telemetry` still builds byte-identical behavior (no STATCHECK code
  compiled — verify `nm`/`strings` for `Statcheck`/`Scrd` absence, and
  that the option defaults OFF).

**Depends on:** A2 (`.scrd` sample for the smoke).

**What NOT to do:** do not refactor the fork toward upstream's
platform/input-driver architecture; do not copy upstream test files
over same-named fork files (§2.5 collision trap); no runner/compare
logic yet (A3b).

**If it fails:** mostly-mechanical step — if `ram_archive.c` verbatim
doesn't compile against the fork's SDL3 version, adapt includes only;
if the glob picks files up into DEBUG builds unexpectedly, check the
`#if STATCHECK` gates before touching CMake.

---

### Step A3b — STATCHECK runner + compare + headless uncapped loop

**Why:** the fidelity oracle proper — phase machine, per-frame
injection, RAM compare — plus the loop change that makes statcheck
runs take seconds instead of real-game minutes (§4.3 headless caveat).

**Read first:**
- Fork: `src/test/test_runner.c` (whole file — the phase machine you
  are mirroring), `src/test/test_runner_compare.c` (fork version +
  commit `a9a4c11c`), `src/test/test_runner_utils.{c,h}`,
  `src/main.c` `game_step_0()` (:634-757), `src/port/sdl/sdl_app.c` (frame pacing :89 and
  the present path).
- Upstream (via `git show upstream/main:`): `src/test/test_runner.c`,
  `src/test/test_runner_compare.c` (full — the compare set is richer
  than the fork's), `src/platform/app/sdl/sdl_headless_app.c`
  (:59-72 — the uncapped loop being adapted).

**Create/modify:**
- `src/test/statcheck_runner.{c,h}` (new) — upstream `test_runner.c`
  logic under renamed entry points `StatcheckRunner_Init/Prologue/
  Epilogue/Destroy`, injecting via `p1sw_buff/p2sw_buff` (translate
  upstream's `SWKey`-based `input_buffers` — the SWK_* constants exist
  in the fork, sf33rd/AcrSDK/common/pad.h — dropping the
  `apply_input_buffer`/`StatcheckInput` indirection entirely).
  **Layout invariant (§2.3): words read from the SCRD archive
  (`P1SW_0`/WCP mirror) are arcade-RAM layout; every value written to
  `p1sw_buff`/`p2sw_buff` must first go through the
  `read_input_buff`-style arcade→SWK conversion (LK 7→8, MK 8→9,
  HK 9→10, start 12→14) — exactly as both existing parsers do.**
- `src/test/statcheck_compare.{c,h}` (new) — upstream
  `test_runner_compare.c`'s `compare_values`/`sync_values` under
  `#if STATCHECK`, renamed `Statcheck_CompareValues`/
  `Statcheck_SyncValues`. **`assert_equals` compile-gate:** the fork's
  macro (ported by `a9a4c11c`) lives *inside* `#if DEBUG` in
  src/test/test_runner_compare.c (:1 gate, macro at :27) — invisible
  to a Release STATCHECK build. Extract it into a small shared header
  (e.g. `src/test/test_assert.h`) included by both the DEBUG compare
  and the new statcheck compare; do not duplicate the macro body.
  Where upstream's compare reads fields the fork's engine structs
  renamed, resolve against fork headers (`plcnt.h`, `cmb_win.h`,
  `count.h`) — this is the expected divergence hot spot.
- `src/main.c` — in `game_step_0`, add
  `#if STATCHECK  StatcheckRunner_Prologue();  #endif` beside the
  existing DEBUG hook (:576-580) and the epilogue beside :671; guard
  so DEBUG test runner and statcheck can't both run (they can't be
  co-compiled anyway).
- **Wire `--headless` for real (STATCHECK builds):** the flag is
  currently parsed but consumed by nothing (§4.3 — only
  src/configuration.h:89 and src/args.c:220 mention it). Under
  `#if STATCHECK`, when `configuration.headless` is set: skip window
  presentation and skip the `target_frame_time_ns` pacing wait
  (src/port/sdl/sdl_app.c:90) so frames step uncapped — the fork-shaped
  equivalent of upstream's `sdl_headless_app.c:59-72` loop. Non-
  STATCHECK builds keep the flag exactly as inert as it is today.

**Success criteria:**
- `./build-statcheck/<binary> --ram-archive <A2 game_1.scrd>
  --headless` runs to completion **in wall-clock seconds, not
  match-duration minutes** (uncapped loop verified); exit 0 =
  full-game RAM match, exit 1 prints the first mismatching
  `assert_equals` with frame number.
- `python3 tools/statcheck_runner.py <binary> <A2 output dir>` prints
  per-game ✔/✘ and a totals line — with its 2 s default timeout
  workable, or `--timeout` documented from measured per-game time.
- A3a's "normal builds unaffected" criterion re-verified after the
  main.c/sdl_app.c edits.

**Depends on:** A3a, A2 (`.scrd` samples to run against).

**What NOT to do:** do not refactor the fork toward upstream's
platform/input-driver architecture; do not touch the DEBUG test
runner's behavior (the frame-data suite must stay green: run
`tools/frame-data/` suite spot checks if `src/test/` shared files are
touched); do not modify `test_runner_compare.c`'s existing fork
content beyond the `assert_equals` extraction; do not inject
arcade-layout words into `p1sw_buff` (see layout invariant above).

**If it fails:**
- Compare-field mismatches (upstream struct names vs fork): resolve
  per-field against fork headers; if a compared field genuinely
  doesn't exist in the fork engine, drop it from the compare with an
  inline citation comment — narrower compare beats no harness.
- First-frame desync immediately after PHASE_GAME: check the two
  known alignment subtleties — `sync_values` must read frame
  `start_index-1` (upstream test_runner.c:272-273, the
  `RamArchive_GetFrame(..., comparison_index - 1)` + `sync_values()`
  pair in `PHASE_GAME_TRANSITION`; `comparison_index = game.start_index`
  at :171) and inputs are the
  *latched previous frame* convention (better-replay-parsing
  replay_match.c:88-91). Compare against the fork's own working
  `initialize_data` flow before suspecting the engine. If inputs look
  wholesale-wrong (walking backward, random supers), suspect a missed
  arcade→SWK conversion first.

---

### Step A4 — Bulk run: measure the fork's replay fidelity number

**Why:** converts "unknown fidelity" (risk 2) into a measured pass
rate; defines which replays Stage C+ may play; creates the regression
harness for future engine cherry-picks.

**Read first:** `tools/statcheck_runner.py` (A1),
`docs/fcade-replay-notes.md` (A2), upstream PR titles for context
(`git -C /Users/sb/Developer/3sx log --oneline --grep=Statcheck`).

**Create/modify:**
- Bulk-download ~50-100 replays (`bulk-download --max-duration 300
  --keep-going`, cookie from user), preprocess all, run
  `statcheck_runner.py` with `--timeout` tuned from A3b's measured
  per-game time (the uncapped headless loop makes this seconds/game).
- Record: pass rate, failure taxonomy (first-divergence frame
  distribution, repeat offenders by character/mechanic) appended to
  `docs/fcade-replay-notes.md`.
- Optional (cheap, high value): run the same archive set against an
  upstream-built 3sx statcheck binary (build in the upstream clone —
  worktree from `03c5d7a6`, NOT the dirty flatpak branch) for an
  upstream-vs-fork pass-rate baseline.

**Success criteria:** a written pass-rate table with commands; ≥1
replay passing end-to-end (else Stage C is blocked and engine-fix
cherry-picking becomes the next work item).

**Depends on:** A1, A2, A3a, A3b.

**What NOT to do:** don't chase individual engine divergences yet
(that's follow-on engine work with its own plans); don't hammer
Fightcade (respect `--delay`, spread over a session).

**If it fails:** pass rate ≈ 0 → suspect harness alignment before
engine (A3b failure modes); verify with the upstream-binary baseline —
if upstream passes the same archives and the fork doesn't, bisect the
fork's engine divergence using the cherry-pick branch.

---

### Step B1 — −13 decoder + cross-validation against SCRD

**Why:** proves our record-format understanding end-to-end on real
data and produces the input-word translation table Stage C/E use.

**Read first:** §2.2/§2.3 citations (runner run.cpp:328-403), fork
`src/test/replay_game.c:12-26` (CPS3 word layout),
`docs/fcade-replay-notes.md`.

**Create/modify:**
- `tools/fcade-replays/decode_inputs.py` (new): reads a replay dir's
  `inputs` (+ `summary.json` for record framing), emits per-frame
  `(p1_word, p2_word)` in **arcade-RAM layout** (fcade bit →
  arcade-word bit table: up 2→0, down 3→1, left 4→2, right 5→3,
  fire1-6 6..11→4..9, start 1→12), plus a `--csv`/`--bin` output.
  **Note this table's output is the arcade layout — directly
  comparable to SCRD-archived `P1SW_0` bytes, but NOT injectable into
  `p1sw_buff` as-is; any engine-side consumer additionally applies the
  arcade→SWK shift (§2.3).**
- Cross-check mode: given the replay's SCRD archives, align the
  decoded stream against the archive's per-frame `P1SW_0/P2SW_0`
  (offsets arcade_constants.h:26-27) by sliding-window match; report
  offset + mismatch count per game. This *measures* the frame
  alignment between raw stream and in-game frames (pauses, transition
  frames, dropped frames).

**Success criteria:** for ≥3 A2 replays: decoder runs; cross-check
reports a consistent alignment with ≥99% in-game frame agreement (or
documents exactly where/why it disagrees — that result feeds B3's
go/no-go).

**Depends on:** A2 (samples + SCRD).

**What NOT to do:** no C code yet; don't "fix" mismatches by fuzzy
matching — understand them (latch-delay off-by-one is expected per
better-replay-parsing's comment).

**If it fails:** if no alignment exists at any offset, the −13 stream
is not the plain per-frame stream the runner treats it as for this
title — re-examine `record_size` from summary.json and the runner's
binding order; document and fall back to SCRD-only ingestion (B2
unaffected).

---

### Step B2 — Define & generate the 3SR format

**Why:** the device-facing artifact (§4.2); decouples on-device
playback from both FBNeo and SCRD bulk.

**Read first:** §4.1/§4.2; upstream test_runner.c:180-268 (exact
seed set); fork test_runner.c:1203-1282 and replay_game.c (existing
extraction); upstream compare.c:488-511 (`sync_values`);
sys_sub.c:1146-1200 (native header fields as a completeness
checklist).

**Create/modify:**
- `docs/3sr-format.md` (new, one page): byte-exact layout, versioned
  magic `3SR1`.
- `tools/fcade-replays/make_3sr.py` (new): SCRD → per-game `.3sr`
  (setup block from the game-start frame per upstream replay_game.c
  reads; `Random_ix16/32` from frame `start_index-1`; inputs from
  per-frame `P1SW_0/P2SW_0`; optional every-N-frames djb2 of a small
  RAM window for runtime divergence detection — reuse offsets already
  in the compare set).
- Quark metadata sidecar: emit `<quarkid>.meta.json` (players, date,
  duration — subset of quark.json) next to the `.3sr` for the browser
  UI.

**Success criteria:** `.3sr` files generated for every A4-passing
replay; a `verify` subcommand re-reads and round-trips; sizes match
the ~4 B/frame + header expectation.

**Depends on:** A2 (SCRD), A4 (list of passing replays), B1 (only for
shared decode tables).

**What NOT to do:** don't put stage index in the setup block as a
direct write — stage selection flows from `New_Challenger` + RNG like
upstream does (:230); adding a stage override is a Stage C playback
option, not format truth.

**If it fails:** if some setup field can't be recovered from SCRD
(e.g. colors offset wrong for this ROM revision), fall back to
defaults + document; colors are cosmetic, characters/SA/RNG are the
correctness core.

---

### Step B3 — EXPERIMENT: direct-from-boot playback of the raw stream (go/no-go)

**Why:** if 3SX can play the *entire* −13 stream from its own cold
boot (character select included), Stage E needs no SCRD/FBNeo at all —
device downloads become self-contained. This is the "remove FBNeo
from the pipeline" question, run as a bounded experiment.

**Read first:** B1's alignment report; fork test_runner phase
machine; `docs/fcade-replay-notes.md` (savestate timeline finding —
this experiment only makes sense if the stream demonstrably starts at
a reproducible boot state).

**Create/modify:**
- Extend the DEBUG test runner (desktop only, no new build flavor)
  with a `--test-fcade-inputs <decoded.bin>` mode: from PHASE_INIT,
  bypass the menu-driving mash logic and feed the decoded stream into
  `p1sw_buff/p2sw_buff` every frame from the earliest boot-stable
  point — **converting each arcade-layout word to SWK layout at the
  injection site (read_input_buff-style shifts, §2.3); the decoded
  stream is NOT injectable verbatim, and a missed conversion here
  would masquerade as a NO-GO** — and instrument divergence via the
  A3b compare against the corresponding SCRD frames.
- Timebox: 1 session. Outcome recorded in `docs/fcade-replay-notes.md`
  as GO (alignment achieved with a deterministic prefix rule) or
  NO-GO (with the observed failure mode).

**Success criteria:** a written GO/NO-GO with evidence. GO =
character select reproduces the SCRD-recorded characters/SA and the
first game passes the RAM compare. NO-GO is a fully acceptable
outcome — the plan's default path (SCRD off-device) already works.

**Depends on:** A3b (compare), B1 (decoder + alignment).

**What NOT to do:** do not sink days into boot-state archaeology; do
not add hacks to force alignment (RNG pokes etc.) beyond what the
savestate/notes justify — a hacked alignment would silently break on
other replays.

**If it fails (NO-GO):** Stage E ships with proxy-side 3SR conversion
(Stage F1 hosts the preprocessing); on-device downloads still work
for "download now, watch after proxy converts" or direct-3SR fetch
from the proxy.

---

### Step C1 — Runtime replay player module (release-compiled, desktop first)

**Why:** the user-facing playback engine — Stage C/D's core.

**Read first:** fork `src/test/test_runner.c` (phase machine,
:1294-1475), upstream test_runner.c (color keys :35-49,
new_challenger :230), `src/main.c:540-680`, `src/netplay/netplay_nav.c`
(mode-launch precedent), `src/args.c` + `src/configuration.h`
(arg wiring pattern), `docs/3sr-format.md` (B2),
`src/port/config/config.c` (config-key pattern).

**Create/modify:**
- `src/replay/replay_player.{c,h}` (new; NOT under `#if DEBUG` — a
  small always-compiled module, ~400 lines): loads a `.3sr`, runs a
  phase machine cloned from the fork test runner's
  PHASE_TITLE→…→PHASE_GAME with upstream's color/new_challenger/RNG
  seeding, feeds `p1sw_buff/p2sw_buff` — **converting each 3SR
  arcade-layout word to SWK layout at load or injection
  (read_input_buff-style shifts, §2.3; 3SR stores arcade-layout words
  per §4.2)** — detects game end (`game_ended`/`PL_Wins[i]==2`,
  upstream test_runner.c:74-76), advances to the next game in
  multi-game replays or exits to title.
- `src/main.c` — hook `ReplayPlayer_Tick()` beside NetplayNav_Tick
  (:589) gated on an active session; must run before the :591 latch.
- `src/args.c`/`configuration.h` — `--play-replay <path.3sr>`.
- Rendering + audio stay fully on (this is the difference from
  statcheck: normal frame pacing, `No_Trans` untouched).
- Divergence detector (cheap): if the 3SR carries checksum samples
  (B2), compare every N frames; on mismatch overlay "REPLAY DESYNCED"
  via SSPutStrProP and stop injecting (freeing the pads).

**Success criteria:**
- Desktop Release build (`CC=clang cmake -B build
  -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel`)
  clean; binary size delta trivial.
- `./build/<binary> --play-replay <file.3sr>` boots, auto-navigates,
  plays the correct characters/SA/stage, match visually plays out,
  returns to title at end; run 3 different replays.
- With no `--play-replay`, behavior is bit-identical (module inert).
- MiSTer build still clean: `tools/mister/build-game.sh --flavor
  telemetry`.

**Depends on:** B2 (format + files). A4's pass list defines the test
corpus.

**What NOT to do:** no UI/browser yet; no netplay interaction —
refuse to start if `Netplay_GetSessionState() != NETPLAY_SESSION_IDLE`
(src/main.c:687 shows the session branch that would conflict); don't
touch the DEBUG runner; don't implement fast-forward/seek.

**If it fails:**
- Menu automation stalls in release build: the DEBUG runner may rely
  on Debug-only globals (`Debug_w[DEBUG_STAGE_SELECT]`,
  test_runner.c:1116/1146) — replace stage forcing with the
  New_Challenger+RNG mechanism (correct one anyway).
- Playback diverges where statcheck passed: compare frame-pacing
  paths (statcheck ran headless through the same `game_step_0`;
  differences point at pause/vsync-coupled logic — check
  `Game_pause` interactions at src/main.c:666).

---

### Step C2 — Playback controls & lifecycle polish

**Why:** minimum viewer UX: leave a replay, survive bad files.

**Read first:** `src/netplay/direct_p2p_overlay.c` (overlay pattern),
sc_sub.h:45, C1 module.

**Create/modify:** in `src/replay/replay_player.c` +
`src/replay/replay_overlay.c` (new): hold-START-to-exit (mirror the
"Hold to Pause" UX the fork ships — OSD bit [24], the
`"P1O[24],Hold to Pause,Off,On;"` option row at menu.sv:322;
engine-side hold detection on `p1sw_0`), status line during playback
(players/date from `.meta.json` sidecar via cJSON — wire
`third_party/cJSON/cJSON.c` into the build here, first use), robust
error paths (truncated/corrupt 3SR → clean return to title with
overlay message).

**Success criteria:** desktop: exit works mid-match and returns to a
playable title screen (input restored); corrupt-file fixture shows
the error overlay and doesn't crash (ASan run:
`-DCMAKE_BUILD_TYPE=Debug` smoke of the parser only — full Debug
build conflicts with nothing here since replay_player is not
DEBUG-gated).

**Depends on:** C1.

**What NOT to do:** no pause/rewind/seek (out of scope §8); no
speed controls.

**If it fails:** exit-path task-state corruption is the risk —
follow `handle_disconnection`'s cleanup precedent
(src/netplay/netplay.c:910-918 `Soft_Reset_Sub()` after session end).

---

### Step C3 — Desktop QA pass with /verify discipline (verification-only)

**Why:** gates device work on a healthy desktop player. **This step is
deliberately verification-only — zero code changes**; it stays
separate from C2 so the gate into Stage D is an explicit, measured
result rather than a success criterion buried in an implementation
step. Any fix it uncovers is executed as a re-entry into B2/C1/C2, not
inside C3.

**Read first:** A4 pass list, C1/C2 code.

**Create/modify:** nothing new — run the player over ≥10 A4-passing
3SR files end-to-end; log outcomes in `docs/fcade-replay-notes.md`
(watched-to-completion count, divergence-detector fires, visual
anomalies).

**Success criteria:** ≥90% of statcheck-passing replays play to
completion without the divergence detector firing; zero crashes.

**Depends on:** C1, C2.

**What NOT to do:** don't paper over a systematic divergence class by
lowering the detector sensitivity.

**If it fails:** classify — file-generation bug (fix B2), player
seeding bug (fix C1), or engine divergence (goes on the engine-fix
queue with the A4 taxonomy).

---

### Step D1 — On-device playback of pre-fetched replays (the request's "Stage B")

**Why:** first user-visible milestone on real hardware.

**Read first:** `docs/mister-runbook.md` (whole build/deploy/probe
flow), AGENTS.md safety rules (deploy scope), memory:
reference-mister-credentials (host .171/.188, `nc -z` first).

**Create/modify:**
- Build: `tools/mister/build-game.sh --flavor telemetry`.
- Copy a handful of `.3sr` + `.meta.json` files to
  `/media/fat/games/3s-arm/replays/` (inside the owned subtree,
  AGENTS.md:6-7) via `misterctl.sh` (respect lock/busy preflight,
  runbook :360-376).
- Launch mechanism for testing (no UI yet): temporary — add
  `--play-replay` to the runtime launch args by invoking the runtime
  directly over SSH the way `misterctl.sh run`/probe does (runbook
  :288 scripts/run-3s-arm.sh), or a one-line config key
  (`CFG_KEY_REPLAY_AUTOPLAY_PATH`) read at boot — prefer the config
  key (pattern: src/port/config/config.c defaults table) since the
  wrapper controls argv. If the config key is added, document it in
  `docs/config.md` in the same change (AGENTS.md:38 ties config-key
  changes to that doc).

**Success criteria:** on-device: replay plays with correct
characters/stage at full speed (telemetry FPS overlay — memory:
headless perf unreliable, use show-fps), audio on, hold-START exits;
`misterctl.sh` probe shows a clean process exit after; repeat for 3
replays including one multi-game.

**Depends on:** C3.

**What NOT to do:** no `rsync --delete` anywhere near `/media/fat`
(memory: feedback-no-rsync-delete); don't deploy the wrapper/RBF —
game-only deploy; don't leave test replays outside
`/media/fat/games/3s-arm/`.

**If it fails:** perf (frame drops): telemetry overlay first —
playback adds near-zero cost over normal gameplay (input injection
only), so drops indicate the divergence detector or file I/O in the
frame loop — move file reads to load-time (they already should be).
Divergence on ARM but not desktop → that's risk 4; capture the frame
number and go to D2 for the systematic answer.

---

### Step D2 — ARM statcheck run on the MiSTer

**Why:** closes the 32-bit question (risk 4) with data: does the ARM
build produce the same frames as the arcade for these replays?
**Scheduled immediately after A4** (see §5): its only real
prerequisites are the statcheck build and the desktop baselines, and
answering the 32-bit-divergence question early de-risks all Stage B/C
player work.

**Read first:** A3a CMake wiring; `tools/mister/build-game.sh` **in
full** — specifically: flavors are restricted to
`telemetry|clean|both` (arg validation :91-99), build/install/package
dirs are keyed on flavor only (`build/mister-${flavor_name}{,-install,
-package}` inside the container workdir, :156-158), `EXTRA_CMAKE_ARGS`
forwarding (:109-113, :167-177), and the container-side CMake cache at
`/work-mister/build/mister-<flavor>/` **persists across invocations**
(the rsync into the container excludes `build/`, and nothing wipes the
cache); runbook deploy section; A2 notes (.scrd sizes) for the space
check.

**Create/modify:**
- Build: `EXTRA_CMAKE_ARGS="-DTHREESX_STATCHECK=ON"
  tools/mister/build-game.sh --flavor telemetry`. **Isolation
  handling (required — the script offers no custom flavor names, and
  a naive run poisons the canonical telemetry tree twice over):**
  1. Immediately after the build, move the host outputs aside:
     `mv build/mister-telemetry-package build/mister-statcheck-package`
     (and likewise `-install`) — the canonical
     `build/mister-telemetry-package` that `misterctl.sh deploy`
     deploys from must never contain a statcheck binary.
  2. Reset the poisoned CMake cache: `THREESX_STATCHECK=ON` persists
     in the container-side cache (CMake `option()` cache semantics),
     so every later plain telemetry build would silently keep
     statcheck on. After the statcheck build, delete the container
     build dir (`docker exec 3s-mister-arm-build rm -rf
     /work-mister/build/mister-telemetry`) or rebuild once with an
     explicit `EXTRA_CMAKE_ARGS="-DTHREESX_STATCHECK=OFF"`.
  3. Re-run a plain `tools/mister/build-game.sh --flavor telemetry`
     and re-verify A3a's "normal builds unaffected" criterion
     (`strings`/`nm` shows no `Statcheck` symbols in
     `build/mister-telemetry-install/bin/3s-arm`) before any
     subsequent deploy.
- Copy the statcheck binary alongside the deployed game (e.g.
  `/media/fat/games/3s-arm/bin/3s-arm-statcheck`) — do NOT deploy
  over the normal game binary.
- Copy a subset of `.scrd` archives (space-checked with `df -h
  /media/fat` first) to `/media/fat/games/3s-arm/replays-scrd/`.
- Run over SSH: `./3s-arm-statcheck --ram-archive <path> --headless`
  per archive (a tiny shell loop; or run `tools/statcheck_runner.py`
  from the Mac pointing at an sshfs/scp-local copy — simplest is the
  on-device loop with exit-code collection).
- Record ARM-vs-desktop pass-rate delta in
  `docs/fcade-replay-notes.md`.

**Success criteria:** ≥10 archives run on-device; pass/fail per
archive recorded; ideally delta = 0 vs desktop on the same archives;
post-run: canonical telemetry build re-verified statcheck-free (step 3
above) — this criterion is part of D2, not optional cleanup.

**Depends on:** A3b, A4 (desktop baselines). D1 is NOT a
prerequisite — the deploy plumbing D2 needs is fully covered by
docs/mister-runbook.md, and running D2 early is the point.

**What NOT to do:** don't fill the SD (check `df`, copy ≤2 GB); don't
run while the user is playing (busy preflight); don't replace the
deployed game binary; don't leave `build/mister-telemetry-*` or the
container cache in a statcheck-tainted state (isolation steps above
are mandatory).

**If it fails:** ARM-only mismatches are real engine findings — dump
the first-divergence frame/field per archive; cross-reference the
netplay desync class list (memory: ca_check_flag, eff79) before
opening new investigations.

---

### Step E1a — Fightcade stream client, desktop bring-up

**Why:** the protocol-client core of Stage E, fully testable on
desktop against real Fightcade before any device work; split from the
device half so each /implement cycle stays ≤ ~2 h.

**Read first:** §2.1 handshake/framing citations (the Python tool IS
the spec — read `do_handshake`/`recv_frame`/`download_replay` fully),
`src/netplay/matchmaking.c` (SDL3_net TCP client pattern + state
machine), `src/netplay/direct_p2p.c` (worker thread + polled state
pattern), CMakeLists.txt:598-603 (SDL3_net is ENABLE_NETPLAY-only —
decide: gate the downloader on ENABLE_NETPLAY too (MiSTer builds ship
with netplay), or use plain POSIX sockets to avoid the coupling;
**decision: plain POSIX TCP**, matching stun.c precedent, so the
downloader exists in every flavor).

**Create/modify:**
- `src/replay/fcade_stream.{c,h}` (new): blocking-socket client on a
  worker thread (SDL_CreateThread — SDL3 core is always linked):
  connect `ggpo.fightcade.com:7100` (getaddrinfo, stun.c:254
  precedent), send the §2.1 handshake byte sequence verbatim
  (u32be words, 15 ms pacing), read length-framed messages, handle
  types 3/−12/−13: write `savestate` (zlib inflate — `#include
  <zlib.h>`, already linked), append −13 bodies to `inputs`, save
  `summary`-equivalent minimal JSON. Idle-timeout termination
  (2 s × 10, tool defaults :590-601). Local port: ephemeral only
  (the 6004 bind is a Python-tool nicety; tool itself falls back,
  :294-317).
- `--fetch-replay <fcade-url>` CLI for integration testing against
  real Fightcade; output dir configurable (desktop default under the
  working dir).

**Success criteria:** desktop `--fetch-replay` downloads a known
quark; resulting `inputs`/`savestate` byte-identical to the Python
tool's output for the same quark (diff).

**Depends on:** A2 (a known quark + Python-tool reference output to
diff against). No B/C/D dependency — pure protocol work.

**What NOT to do:** no TLS, no API/search calls (the stream endpoint
only); no retry storms (single retry with ephemeral port, then
surface the error); don't block the main loop (worker thread + polled
state like direct_p2p); no device deployment in this step.

**If it fails:** compare against a fresh Python-tool run of the same
quark (protocol drift vs implementation bug); packet-capture on the
desktop build — never debug the protocol on-device.

---

### Step E1b — On-device fetch, storage & conversion placement

**Why:** lands Stage E on the MiSTer: device-side storage, and the
B3-gated decision of where `inputs`→3SR conversion runs.

**Read first:** E1a module; `docs/mister-runbook.md` deploy section;
B3's GO/NO-GO record in `docs/fcade-replay-notes.md`; B1's decode
table (`tools/fcade-replays/decode_inputs.py`);
`src/port/config/config.c` (config-key pattern); §2.3 (input-word
layouts).

**Create/modify:**
- Storage: `/media/fat/games/3s-arm/replays/<quarkid>/` — config key
  for the replays root (default derived from the runtime home) +
  matching `docs/config.md` entry (AGENTS.md:38 ties config-key
  changes to that doc).
- Post-download conversion: per B3 outcome — GO: on-device
  `inputs`→3SR conversion in C (port B1's fcade→arcade decode table;
  the 3SR output stays arcade-layout words per §4.2 — **the arcade→SWK
  shift (§2.3) belongs to the player's load path, not the file**);
  NO-GO: leave raw + mark "needs conversion" for the proxy path
  (Stage F), and keep direct-3SR downloads from the proxy as the
  primary UX.

**Success criteria:** MiSTer: fetch on-device over the wired GbE
(IPv4-only stack, memory: reference-mister-network-stack) completes;
files land under `replays/`; a subsequent D1-style playback of the
converted/matched 3SR works.

**Depends on:** E1a (client), D1 (on-device playback to consume it),
B3 (conversion-placement decision), B1 (decode table if GO).

**What NOT to do:** no `rsync --delete`; nothing written outside
`/media/fat/games/3s-arm/`; no TLS/API calls; don't debug protocol
issues on-device (reproduce with E1a's desktop CLI first).

**If it fails:** if the on-device fetch diverges from the desktop
behavior, suspect the IPv4-only/DNS environment first
(reference-mister-network-stack) and capture from the desktop side;
conversion bugs reproduce offline against A2 sample data.

---

### Step F1 — VPS proxy service (search + catalog + optional conversion)

**Why:** the only viable search/browse path (§4.5): Cloudflare +
TLS terminate on the VPS; device speaks plain TCP.

**Read first:** `tools/rendezvous-server/rendezvous-server.js`
(style/ops conventions), `deploy.sh`, `rendezvous-server.service`;
`fcade_replay_tool.py` `search_quarks` (:117-165) for the exact
upstream API request; open question 7 (runner on Linux).

**Create/modify:**
- `tools/fcade-proxy/` (new in repo): Node.js service (no external
  service deps beyond Node stdlib fetch/TLS), TCP listener on a new
  port (e.g. 3479), length-framed JSON protocol:
  `{op:"search", gameid, offset, limit, best?, since?, username?}` →
  cached `searchquarks` rows (subset: quarkid, date, duration,
  players[name,country,rank,score], ranked, num_matches);
  `{op:"get3sr", quarkid}` → 3SR bytes if converted (F1b);
  `{op:"status"}`. Cookie (`FCADE_COOKIE`) via env/file with an admin
  refresh note in README; response cache (15 min for page 0, longer
  for `best`).
- systemd unit + deploy.sh mirroring rendezvous-server's.
- **F1b (optional within this step if time allows, else fold into a
  follow-up):** conversion worker — try building fbneo-replay-runner
  on the VPS (`make sdl` for linux/x86_64); if it builds, wire
  download→runner→compress→make_3sr for requested quarks; if not,
  document per open question 7 fallback.
- Server-side only; **no pushes/deploys without the user** — provide
  the deploy commands, user runs them (VPS is the user's box; deploy
  is an off-machine action per memory feedback-no-go-gates).

**Success criteria:** local run (`node fcade-proxy.js` with a
supplied cookie) answers a framed-JSON search over `nc`/test script
(ship a `__test_protocol.js`-style self-test like the rendezvous
server has); Cloudflare 403 path returns a typed error the client
can render ("catalog stale").

**Depends on:** conceptually on B2's format for get3sr; search alone
has no repo dependency.

**What NOT to do:** don't proxy arbitrary URLs (fixed op set); don't
store credentials in-repo; don't touch the rendezvous server's
process/port.

**If it fails:** if fightcade.com blocks the VPS IP even with a valid
cookie (Cloudflare IP binding — cf_clearance can be IP-bound), the
documented fallback is catalog generation on the user's Mac (where
the browser cookie originates) pushed to the VPS as static data —
same client protocol, different refresher.

---

### Step F2a — In-game browser UI: local list + play

**Why:** the browse/pick/watch loop over *local* replays — no network
dependency; doubles as the browser's permanent offline mode. Split
from the remote half so each /implement cycle stays ≤ ~2 h.

**Read first:** §4.6; `src/netplay/direct_p2p_overlay.c` (text
rendering), `src/port/sdl/netplay_screen.c` (screen-state pattern),
main.c:583-599 (input edges), sc_sub.h glyph APIs (SSPutStr sizes),
C2 overlay code.

**Create/modify:**
- `src/replay/replay_browser.{c,h}` (new): a screen reachable from a
  launch trigger (first cut: config key/CLI `--replay-browser`,
  wrapper OSD wiring comes in F4): renders a paged list (≈8 rows —
  date, P1 vs P2 names, duration, rank glyphs later) from local
  `replays/*/meta.json`; pad navigation (up/down/page L/R,
  SOUTH=select, hold START=back); select → play via C1.
- Config keys: replays dir (shared with E1b if that landed first —
  add only if missing), browser launch key — each with a matching
  `docs/config.md` entry (AGENTS.md:38).

**Success criteria:** desktop + MiSTer: browser lists local replays
and plays them; back-out returns to a playable title screen; empty
`replays/` dir renders a sane empty state, no crash.

**Depends on:** C2 (overlay + cJSON wiring, playback lifecycle).

**What NOT to do:** no networking in this step (proxy/download is
F2b); no free-text search (F3); no thumbnails; no scrolling
animations; don't let the browser run while a netplay session is
active (same guard as C1).

**If it fails:** UI glitching over game scenes → priority/atr issues:
follow direct_p2p_overlay's priority notes (:34-39) and render on the
title screen state only (enter the browser from title, where the
netplay overlay already draws safely).

---

### Step F2b — Remote browse + download integration

**Why:** connects the browser to the F1 proxy and the E1a/E1b
downloader — the full Stage F pick→download→watch loop.

**Read first:** F1 protocol + `tools/fcade-proxy/` code; E1a/E1b
client; F2a browser code; `src/port/config/config.c` (config-key
pattern).

**Create/modify:**
- `src/replay/proxy_client.{c,h}` (new): POSIX TCP + framed JSON
  (cJSON parse — wired in C2), non-blocking via the same worker
  pattern; graceful offline mode = local-only list (F2a behavior).
- `src/replay/replay_browser.c` — remote results merged/paged into
  the list; select → download via the E1a stream client / proxy
  `get3sr` → play via C1.
- Config keys: proxy host/port (default the VPS IP, pattern of
  config.c:85), catalog page size — each with a matching
  `docs/config.md` entry (AGENTS.md:38).

**Success criteria:** desktop: browser lists local + remote replays
(against a locally-run F1 proxy), full pick→download→watch loop
works; MiSTer: same over the LAN/WAN; offline (proxy down) degrades
to F2a's local-only behavior with a visible "offline" hint.

**Depends on:** F2a, E1b, F1.

**What NOT to do:** no free-text search yet (F3); don't block the
main loop on proxy calls; don't cache-bust the proxy per keystroke
(page-granular requests only); same netplay-session guard.

**If it fails:** protocol issues reproduce against the locally-run F1
proxy with the self-test script; UI issues fall back to F2a's
title-screen-only rendering rule.

---

### Step F3 — Search & filters

**Why:** completes "browse/search recent or best" from the feature
definition.

**Read first:** F1 protocol; `search_quarks` params (:117-152 —
`best`, `since`, `username`); the game's name-entry UI for a
pad-driven character picker precedent (grep `Name_Entry`/ranking
input in `src/sf33rd/Source/Game/ui/` at implementation time).

**Create/modify:** browser gains: tabs Recent / Best / By-player;
By-player opens a pad-driven A-Z/0-9 grid to compose a username
(≤16 chars) → proxy `{op:"search", username}`; duration filter
(<2 min / <5 min / all) client-side on the `duration` field.

**Success criteria:** each tab returns correct, distinct results
verified against the Fightcade website for the same query; username
search for a known player (e.g. the user's own handle) finds their
replays.

**Depends on:** F2b.

**What NOT to do:** no fuzzy search, no ranked-only toggle unless
trivial (`ranked` field is present in rows).

**If it fails:** `best`/`since` semantics drift (upstream README's
monthly-best example pairs `--best --since <ms>`): mirror exactly
what the website sends (observable via the tool) and encode in F1,
not the client.

---

### Step F4 — OSD launch wiring + storage lifecycle

**Why:** polish to shippable: enter the browser from the MiSTer OSD;
keep the SD tidy.

**Read first:** menu.sv CONF_STR (:278-290), wrapper trigger/argv
pattern (thirdsarm_wrapper.cpp:2171-2213 T-triggers, :3028-3040 argv
injection, :2543 `direct_p2p_arm_and_restart`), docs/mister-wrapper.md, AGENTS.md
FPGA-build note (:23 — CONF_STR changes require a Quartus wrapper-core
rebuild in the colima VM; **scope check: a new `T[NN]` line changes
the RBF**).
**Decision to minimize scope:** reuse the *game-side* entry instead —
add "REPLAYS" to the in-game title/menu path (no RBF change), and
optionally later an OSD trigger. If the user wants OSD-first, that
becomes a separate wrapper-core task (Quartus build, memory:
feedback-quartus-nohup/fast-build).
- Storage lifecycle: config `replays_max_mb` (default e.g. 200 MB);
  LRU eviction of raw stream files first (savestate is the bulk),
  keep `.3sr`+meta (tiny); "delete replay" action in the browser.

**Create/modify:** `src/replay/replay_browser.c` (menu entry +
delete/evict), `src/port/config/config.c` (keys), docs updates:
`docs/config.md` for the new key(s) (`replays_max_mb` etc. — the
"Load docs/config.md when changing config keys" rule, AGENTS.md:65)
and `docs/mister-runbook.md` short section (replays
dir, eviction, proxy config).

**Success criteria:** fresh boot → title → REPLAYS → browse →
download → watch → exit → title, all on-device without SSH; eviction
demonstrably triggers under a lowered test cap.

**Depends on:** F2b (F3 optional).

**What NOT to do:** no RBF/Quartus work in this step; eviction must
never touch anything outside `replays/` (path-validate, AGENTS.md:10).

**If it fails:** title-menu integration is the risky half (menu task
r_no machinery) — fallback is the config/CLI launch (D1 mechanism)
plus a documented `Scripts/`-side launcher, shipping the feature
without engine-menu surgery.

---

## 7. Cross-cutting risks & pitfalls (per-step reminders)

- **Fork/upstream divergence in `src/test/`** is the #1 porting trap:
  same filenames, same-name symbols, different gating and
  architecture (§2.5). Never copy upstream test files over fork ones;
  always the rename-and-adapt route (§4.3).
- **-Werror / warnings:** fork builds treat warnings seriously
  (commit `6740d726` removed an unused table for -Werror). New code:
  compile both desktop clang and the Docker cross clang-20 before
  declaring a step done.
- **Netplay interaction:** replay player/browser must hard-refuse to
  run alongside a netplay session (src/main.c:612 branch); netplay
  files are rollback-sensitive — do not add unsaved globals to
  anything the rollback path touches (memory: eff79/ca_check_flag
  class bugs).
- **Telemetry flavor default** for every MiSTer build
  (memory: feedback-always-telemetry; AGENTS.md:19).
- **Debug builds for live tests:** when the friend/user tests
  downloads or playback on-device, ship diagnostics on
  (memory: feedback-debug-build-for-live-tests) — the telemetry
  flavor + the player's log lines cover this.
- **Version skew watch:** upstream's tooling moves (4 tool PRs in 5
  months); before each stage that touches ported tools, re-check
  `git -C /Users/sb/Developer/3sx fetch upstream && git log
  upstream/main -- tools/` for changes, and grep-compare content
  (memory: verify-content-not-shas).
- **Politeness/ToS:** bulk operations rate-limited (`--delay`), proxy
  caches aggressively, device never scrapes (open question 8 gates
  public release).

## 8. Deliberately out of scope

- Rewind/seek/fast-forward/slow-mo inside playback (input-replay
  cannot seek backward without savestates; would require our own
  periodic engine snapshots — future work).
- Recording *our own* matches to 3SR/Fightcade format (the native
  `Get_Replay` recorder exists, sys_sub.c:1248 — separate feature).
- Uploading/sharing replays; FightcadeVids integration (the API has
  video URL ops — xBiggs api :174-192 — unrelated to engine playback).
- OSD/FPGA (RBF) changes — explicitly deferred out of F4; no Quartus
  builds in this plan.
- RmlUi or any GUI-toolkit resurrection (abandoned 2026-04-29).
- Non-sfiii3nr1 games; non-MiSTer ports (Miyoo) — nothing here should
  break them (all new code is platform-neutral C), but no
  Miyoo-specific validation.
- Cross-arch netplay determinism fixes (tracked elsewhere;
  memory: project-cross-arch-netplay-recipe).
- Automated cf_clearance harvesting (headless browser on VPS) —
  documented as the F1 fallback path only; manual refresh ships
  first.
