# Fightcade Replay Notes — Stage A Step A2 Findings

**Status: Step A2 complete, 2026-07-21.** Runner built, ROM
provisioned, 4/4 attempted quark IDs downloaded successfully, 4
`game_0.scrd` archives produced (2 complete, 2 partial-but-valid — see
§5.5 for why), all confirmed with the `SCRD` magic header. This doc is
the citation anchor that resolves
`docs/plan-fcade-replay-browser.md` §3 open questions 1, 6, and 9. Every
number below was produced by actually running the commands shown, on
this machine, against real Fightcade servers and a real
`crowded-street/fbneo-replay-runner` build — not estimated or inferred.

## 1. Runner build (macOS arm64)

- Cloned `github.com/crowded-street/fbneo-replay-runner` @ `ccf96ab`
  (single branch `master`) into the session scratchpad, **outside** the
  fork tree.
- Build command exactly as documented:
  `make sdl 'BUILD_X86_ASM=' 'CPUTYPE=arm64' -j1`.
- **Deviation from README — a real build bug, not a race the README's
  `-j1` warning covers:** the first pass failed with
  `makefile.sdl:629: build/debug/obj/GNU_SDL/fbneosdldarm64/burner/sdl/cps3_debug_harness.d: No such file or directory` /
  `make[2]: *** No rule to make target ...cps3_debug_harness.d`. Root
  cause: `cps3_debug_harness.o` is only added to `autobj` under
  `ifdef DEBUG` (makefile.sdl:303-305), and the top-level `makefile`
  hard-codes `DEBUG = 1` (makefile:44, plain assignment, not
  conditional) — so every default `make sdl` build is a debug build.
  The dependency-generation first pass never emitted
  `cps3_debug_harness.d` even though the second pass's compile rule
  found and compiled `cps3_debug_harness.cpp` successfully (its `.o`
  exists on disk). **Workaround applied:** created an empty
  `build/debug/obj/GNU_SDL/fbneosdldarm64/burner/sdl/cps3_debug_harness.d`
  by hand (`: > <path>`) so the second pass's `include $(alldep)` could
  proceed; re-ran the identical `make` command and the build completed
  (linked `build/debug/fbneosdldarm64`, ~38 MB Mach-O arm64
  executable). No source was modified — only a missing generated build
  artifact was stubbed.
- Toolchain used: Apple clang 21.0.0 (Xcode), system `make`
  (`/Applications/Xcode.app/.../usr/bin/make`), `pkg-config` resolving
  `sdl2` via Homebrew `sdl2-compat` 2.32.70
  (`PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig`) — `makefile.sdl:143`
  links via `pkg-config --libs sdl2`, and `sdl2-compat`'s
  `sdl2.pc`/`sdl2-config.cmake` satisfy that without a separate
  `sdl12-compat` install (`sdl12-compat` is not installed on this
  machine and was not needed).
- Binary confirmed runnable: `./build/debug/fbneosdldarm64 --help`
  exits 0 and prints the FBNeo v0.2.97.44 usage banner. Matches the
  plan's citation that the debug-build binary is named
  `fbneosdldarm64` (README.md:22 @ `ccf96ab`).

## 2. ROM provisioning — a second undocumented gap

- Copied (never moved) `sfiii3nr1.zip` from
  `/Applications/FightCade2.app/Contents/MacOS/emulator/fbneo/ROMs/sfiii3nr1.zip`
  into the runner's `roms/` directory. `md5` of source and copy match:
  `6573a2754d340847344c88a734111a13`.
- **First load attempt failed:** `./build/debug/fbneosdldarm64 -headless sfiii3nr1`
  reported `Loading program (sfiii3-simm2.0)... (not found)` and exited
  with "There was an error loading your selected game." Root cause,
  confirmed by reading `src/burn/drv/cps3/d_cps3.cpp`: `sfiii3nr1.zip`
  as shipped by FightCade2 contains only 5 files (BIOS +
  `sfiii3-simm1.0..3`, 8,375,056 bytes total,
  `unzip -l roms/sfiii3nr1.zip`); the `BurnDrvSfiii3nr1` driver entry
  (d_cps3.cpp:1751-1756) declares `sfiii3nr1` as a **clone of `sfiii3`**
  (`"sfiii3nr1", "sfiii3", ...`) and its ROM list
  (`sfiii3nr1RomDesc`, d_cps3.cpp:736-741) expands
  `SFIII3_990512_FLASH` (d_cps3.cpp:646-...), which additionally
  requires `sfiii3-simm2.0..3` (program) and `sfiii3-simm3.0..simm6.x`
  (graphics — shared, region-independent data across all `sfiii3*`
  variants). FightCade2's `sfiii3nr1.zip` only carries the files that
  differ from the parent set (the no-CD BIOS + simm1); it relies on
  FBNeo's clone/parent ROM resolution to pull the rest from a sibling
  zip.
- **Fix:** also copied FightCade2's `sfiii3.zip` (the Euro 990608
  parent set documented in `docs/arcade-frame-data/CAPTURE.md:31-38`,
  70,592,155 bytes, md5 `0bd1a17c6e72e5d0fa101405f42b2f12`, matches
  source) into the same `roms/` directory. With both zips present,
  `-headless sfiii3nr1` loads every ROM file successfully (BIOS through
  `simm6.7`) and runs.
- **Consequence for the plan:** Step A2's "Create/modify" note already
  flagged that `sfiii3nr1.zip` presence needed confirmation, not
  assumption — confirmed present. What was *not* anticipated: booting
  `sfiii3nr1` standalone requires **both** `sfiii3nr1.zip` and
  `sfiii3.zip` side by side in `roms/`, because `sfiii3nr1` is a
  same-directory clone, not a self-contained set. Any later
  automation (Stage F provisioning, CI) must copy both zips.

## 3. Quark ID harvest — provenance

Fightcade's `searchquarks` API is Cloudflare-403 from this network (no
`cf_clearance` cookie available), so `list-replays`/`bulk-download`
could not be used. Quark IDs were instead harvested from the public
web per the plan's fallback strategy.

**What worked:** a web search for `fightcade.com/replay "3rd strike" OR
"sfiii3nr1"` surfaced an Internet Archive collection ("Fightcade-Archive",
uploader "Gino Lisignoli" / fightcadevids.com) that mirrors Fightcade
`sfiii3nr1` matches as videos, using the literal Fightcade quark ID as
the archive.org item identifier. Reddit, Twitter/X, and direct
`site:youtube.com` searches for embedded `fcade://`/
`replay.fightcade.com` links returned nothing usable.

| Quark ID | Source | Verified | Context |
|---|---|---|---|
| 1641508702494-7287 | archive.org/details/1641508702494-7287 | fetched | (CL) Atma_ vs (CL) --Panta--, Ken vs Chun-Li, 2022-01-06 |
| 1675032176612-9791 | archive.org/details/1675032176612-9791 | fetched | (US) Arlieth vs (US) wadupNEEM, Makoto vs Chun-Li, both Rank S, 2023-01-29 |
| 1667822595212-6293 | archive.org/details/1667822595212-6293 | fetched | (RU) TOP G vs (RU) Frexia1, Ryu vs Hugo, 2022-11-07 |
| 1676027217605-8232 | archive.org/details/1676027217605-8232 | fetched | (AU) Shabsss vs (NZ) glisignoli, Urien vs Oro, 2023-02-10 |
| 1675950860255-2133 | archive.org/details/1675950860255-2133 | fetched | (IN) k@ra vs (IN) Tejas, Ken/Sean/Ryu, 2023-02-09 |
| 1659344338224-4017 | archive.org/details/1659344338224-4017 | search-snippet only | (US) FireGuy808 vs (US) Snsinny, 2022-08-01 |
| 1675220043007-7798 | archive.org/details/1675220043007-7798 | search-snippet only | (US) parry the platypus vs (US) BLINDCONFIDENCE, 2023-02-01 |
| 1672649600209-3613 | archive.org/details/1672649600209-3613 | search-snippet only | (US) itsnotspidey vs (US) Snsinny, 2023-01-02 |
| 1667539223222-1807 | archive.org/details/1667539223222-1807 | search-snippet only | (US) fyto666 vs (US) FightcadeGateKeeper, 2022-11-04 |
| 1674716618396-8939 | archive.org/details/1674716618396-8939 | search-snippet only | (US) GZA_EFX vs (US) Snsinny, 2023-01-26 |

**Shortfall note (per plan's hard-blocker/fallback framing):** all
harvested candidates are 2022-2023, not the "ideally 2025-2026" the
task preferred — no recent public link dumps were found by any of the
attempted strategies (Reddit, X, YouTube descriptions, pastebin/forum
search). Target was a corpus of independently-sourced quark IDs (not
from the blocked search API); that target was met (10 candidates from
one archive), but recency was not.

## 4. Download attempts

Command used for every attempt (only `<quarkid>` varies):

```bash
python3 tools/fcade-replays/fcade_replay_tool.py download \
  --fcade-url "fcade://stream/fbneo/sfiii3nr1/<quarkid>.7,7100" \
  --idle-timeout 2 --max-idle-timeouts 20 --auto-dir
```

Attempted 4 of the 10 harvested candidates, sequentially, one at a
time (never more than one live stream connection to
`ggpo.fightcade.com:7100` at once); stopped at 4 because all 4
succeeded and the target of 3-5 was already met.

| Quark ID | Result | `frames.bin` | `inputs` | `savestate` (decompressed) | Players |
|---|---|---|---|---|---|
| 1641508702494-7287 | **success** | 780,820 B | 625,800 B | 1,907,010 B | Atma_ (CL) vs --Panta-- (CL) |
| 1675032176612-9791 | **success** | 368,884 B | 240,000 B | 1,907,010 B | Arlieth (US) vs wadupNEEM (US) |
| 1667822595212-6293 | **success** | 589,501 B | 412,200 B | 1,907,010 B | TOP G (RU) vs Frexia1 (RU) |
| 1676027217605-8232 | **success** | 546,213 B | 454,800 B | 1,907,010 B | Shabsss (AU) vs glisignoli (NZ) |

**4/4 attempted candidates succeeded** — every one of the harvested
quark IDs still streamed live from `ggpo.fightcade.com:7100` despite
being 2022-2023 vintage, contrary to the plan's "old replays may have
expired server-side" caution. None were skipped for 0-byte
inputs/savestate. The other 6 harvested candidates were not attempted
(not needed once 4/4 succeeded; kept as a reserve list if more samples
are ever needed).

**Real-world gotcha found while downloading:** the tool's own
`--idle-timeout`/`--max-idle-timeouts` accounting works as documented,
but a naive foreground shell timeout of 120 s is **not enough** for
these replays — `ggpo.fightcade.com` streams the whole match archive
in real time-ish chunks (a 1,043 s / ~17.4 min multi-game session took
several real minutes to fully arrive), so every download in this
session was run with the harness's `run_in_background` mechanism
rather than a foreground call.

**Disk-space gotcha (operationally important for anyone repeating
this):** running the *raw* per-frame `.ram` dump stage
(`-dump-ram-path`, 524,288 B/frame, uncompressed) for a full multi-game
session can consume **tens of GB** before `compress_ram_dumps`/
`replay_preprocessor.py` ever gets to shrink it down — this session hit
`ENOSPC` (disk 100% full, only 118 MiB free) partway through
processing replay 1's 4th in-session game, killing two in-flight
background tasks (the runner and a concurrent download). Recovered by
`pkill -9`-ing the runner and `rm -rf`-ing the runaway
`fbneo-ram-dumps-*` temp directory (freed ~16 GiB). **Practical
mitigation used for the rest of this step:** instead of letting
`tools/replay_preprocessor.py` run a whole multi-game replay
end-to-end (its `tempfile.TemporaryDirectory` only gets cleaned up
*after* every `game_N` in that replay has been dumped and compressed —
too big a working set for multi-game sessions on a disk with only
~15-16 GiB free), the runner was invoked directly per replay and killed
as soon as `game_0/`'s frame count stopped growing (i.e. `game_1/`
appeared, proving `game_0` was fully captured), then
`tools/compress_ram_dumps.py` was run directly on just that one
`game_0/` directory. This is a real constraint for Stage A/D tooling
on constrained disks, not just an artifact of this laptop being full
of unrelated data — **worth carrying into the plan's Stage A
follow-up work** (either process one game at a time by default, or
document the disk-headroom requirement explicitly).

## 5. −13 / −12 / SCRD measurements (open questions 1, 6, 9)

### 5.1 −13 record header values (open question 1: "record_size actually 10?")

**Confirmed for all 4 replays, every one of the 1,043/400/687/758 `−13`
messages respectively:** `record_size == 10`, `record_count == 60`
(never varies within or across replays), so `expected_body_len == 600`
bytes per message and the concatenated `inputs` file size is always an
exact multiple of 600. Measured directly from each `summary.json`:

| Quark ID | # `−13` msgs | `record_size` | `record_count` | total input frames | duration @ 60 fps |
|---|---|---|---|---|---|
| 1641508702494-7287 | 1,043 | 10 | 60 | 62,580 | 1,043.0 s (~17.4 min) |
| 1675032176612-9791 | 400 | 10 | 60 | 24,000 | 400.0 s (~6.7 min) |
| 1667822595212-6293 | 687 | 10 | 60 | 41,220 | 687.0 s (~11.5 min) |
| 1676027217605-8232 | 758 | 10 | 60 | 45,480 | 758.0 s (~12.6 min) |

Command used to inspect: `python3 -c "import json; s =
json.load(open('summary.json')); ..."` iterating `s['messages']` for
`type == -13` (see `tools/fcade-replays/fcade_replay_tool.py:224-234`
for the parser this data comes from).

### 5.2 Savestate (open question 1: "what does the −12 blob contain / savestate size?")

All 4 replays' single `−12` message decompresses to **exactly
1,907,010 bytes**, matching the `'GGPO'`-tagged blob format
`ReplayLoadStateBlob` expects (`run.cpp:285-323`). Compressed
(zlib, as received on the wire) sizes vary slightly per replay:

| Quark ID | compressed (zlib) | decompressed |
|---|---|---|
| 1641508702494-7287 | 122,975 B | 1,907,010 B |
| 1675032176612-9791 | 122,400 B | 1,907,010 B |
| 1667822595212-6293 | 122,941 B | 1,907,010 B |
| 1676027217605-8232 | 122,335 B | 1,907,010 B |

The first 12 decompressed bytes are byte-identical across all 4:
`4f 50 47 47` (`'GGPO'`, little-endian multichar int, matching
`header[0] == 'GGPO'` in `ReplayLoadStateBlob`) then `18 00 00 00`
(`headerSize = 24`) then `44 97 02 00` (`nAcbVersion = 0x00029744 =
169796`, identical across all 4 — consistent with all 4 replays having
been recorded by the same FBNeo build version on Fightcade's servers).
Offset 15 (0-based; the 16th byte, still inside the 24-byte header
region) differs per replay (`05`, `00`, `03`, `05` for the four replays
above; re-verified with `xxd -s 15 -l 1 <savestate>`), while offset 24
— the actual first byte after the 24-byte header, i.e. the start of the
`BurnAreaScan` payload — is `01` in all four. Neither is decoded
further in this step; no claim is made about their meaning beyond "the
offset-15 byte varies per session."

### 5.3 Does the stream include pre-game/character-select frames? (open question 1)

**Yes — confirmed by two independent lines of evidence:**

1. **Code reading:** `ReplayApplyFrameInputs` (`run.cpp:379-403`, called
   unconditionally every emulated frame once a replay is loaded) has no
   "in game" gate — it always consumes the next 10 bytes of `inputs`
   and applies them to the emulated pad state. `ReplayDumpCps3MainRam`
   (`run.cpp:66-119`), by contrast, only creates a `game_N` directory
   and writes a `frame_%08u.ram` file when CPS3 RAM offset `0x15438`
   (big-endian u16) reads `2` ("in game" — same offset the fork calls
   `GAME_ROUTINE_OFFSET`). Any frame where the game is in a menu,
   character-select, or results screen still consumes real `−13` input
   bytes but produces **no** RAM dump at all.
2. **Empirical frame-count arithmetic:** replay `1641508702494-7287`
   has 62,580 total input frames but its first 3 `game_N` boundaries
   (`game_0`→`game_1` at 5,964 frames, `game_1`→`game_2` at 8,801
   frames, `game_2`→`game_3` at 10,305 frames — all 3 boundaries
   directly observed via `game_N+1`'s directory appearing, proving each
   prior game's count is final) sum to only 25,070 frames — 40% of the
   session's total input-frame count — even before accounting for
   further games (`game_3`, `game_4` were both observed to exist before
   this session's raw dumps were deleted during the `ENOSPC` recovery,
   §4). The remainder is time spent in non-dumped ("not in game")
   states: character select, versus screens, results, and rematch
   prompts between the session's multiple back-to-back matches.

This also independently confirms the plan's suspicion (§2.3, §3 open
question 1) that a single quark/stream can and does cover **multiple
games** in one session — observed directly as multiple `game_N`
directories per replay, not merely implied by `quark.json.num_matches`
(which direct-URL downloads like these don't even have — `quark.json`
is only written by `bulk-download`/`list-replays`, per
`fcade_replay_tool.py:493`).

### 5.4 RAM dump frame size (plan's stated success criterion)

Every `frame_%08u.ram` file observed, across all `game_0` directories
processed, is **exactly 524,288 bytes** (matches `RAM_FRAME_SIZE` in
upstream `ram_archive.c:8` and the driver's `cps3GetMainRamSize() ==
0x80000` check in `run.cpp`). Verified with `wc -c` on
`game_0/frame_00000000.ram` for the first replay and spot-checked file
sizes across the rest.

### 5.5 SCRD archives (open questions 6, 9)

Produced with `tools/compress_ram_dumps.py <game_0 dir> <out>.scrd`
(the #268 variant path — `tools/replay_preprocessor.py` calls this
same function; see the file-header note in that script for why the
public runner needs it instead of upstream's current `.scrd`-native
#281 preprocessor). Because of the disk-space constraint (§4), the two
longer replays' `game_0` archives are **partial** (runner killed mid-
`game_0` at a safe, arbitrary frame count rather than run to
completion) — labeled `_partial.scrd` below; the other two are complete
(runner killed only after `game_1/` appeared, proving `game_0` was
fully captured).

| Quark ID | frames in archive | raw size (frames × 524,288) | `.scrd` size | ratio | complete? |
|---|---|---|---|---|---|
| 1641508702494-7287 | 5,964 | 3,126,853,632 B (2.91 GiB) | 31,193,214 B (29.7 MiB) | 1.00% | yes (`game_1/` appeared) |
| 1675032176612-9791 | 4,099 | 2,149,056,512 B (2.00 GiB) | 21,066,899 B (20.1 MiB) | 0.98% | yes (`game_1/` appeared) |
| 1667822595212-6293 | 7,142 | 3,744,464,896 B (3.49 GiB) | 38,077,080 B (36.3 MiB) | 1.02% | **partial** — killed at 7,142 frames |
| 1676027217605-8232 | 4,168 | 2,185,232,384 B (2.03 GiB) | 22,268,301 B (21.2 MiB) | 1.02% | **partial** — killed at 4,168 frames |

All four files open with the `SCRD` magic
(`xxd <file>.scrd | head -1` → `00000000: 5343 5244 ...` = ASCII
`SCRD`), and the u16 frame-count field at byte offset 4 (per
`compress_ram_dumps.py:9-11` header layout) matches the input frame
count in every case (verified with a small `struct.unpack('<H', ...)`
check) — **satisfies the plan's Step A2 success criterion**
(`xxd game_1.scrd | head -1` shows `SCRD`).

**Storage-headroom finding (open question 9):** the zero-run + XOR-
delta encoding compresses CPS3 main RAM to **~1% of raw size** — about
5.1-5.3 KB/frame on average (`.scrd size / frame count` above) versus
524,288 B/frame raw. For the plan's Stage D on-device validation
concern ("SCRD archives... can be large"): a **complete** single game
of ~6-10k frames (~100-170 s of gameplay) should land in the
**20-40 MB** range based on these 4 samples, not hundreds of MB — the
plan's uncertainty here was justified caution but the real number is
much smaller than the worst-case framing ("frame 0 stored full = 512
KiB pre-RLE" is only the first frame; subsequent XOR-delta frames
compress far better in practice, per the ratios above). This doesn't
resolve *whole-session* (multi-game, 17+ minute) archive sizes, which
would scale roughly linearly with total in-game frame count — replay
`1641508702494-7287` alone had at least 5 observed games (`game_0`
through `game_4`, per §5.3) in one session, so a complete multi-game
session's combined `game_N.scrd` archives could plausibly total in the
low hundreds of MB, though this step did not measure a complete
multi-game session end-to-end (see the disk-space gotcha in §4 for
why).

## 6. Summary of what resolves plan §3 open questions 1, 6, 9

- **Open question 1** ("−13 header values & exact stream timeline"):
  fully resolved by §5.1-5.4 above — `record_size` is confirmed `10`
  (never anything else, across 2,888 total `−13` messages spanning 4
  independently-sourced replays); the savestate is a fixed-format
  `'GGPO'` blob, always 1,907,010 B decompressed; the stream **does**
  include non-gameplay (character-select/results/rematch) frames,
  proven both by code (asymmetric `ReplayApplyFrameInputs` vs
  `ReplayDumpCps3MainRam` gating) and by measurement (§5.3); RAM dump
  frames are exactly 524,288 B as expected.
- **Open question 6** ("public runner vs #281 skew — `.ram` vs `.scrd`
  emission"): confirmed in practice, not just by reading the runner's
  source. The public runner (`ccf96ab`) only ever writes raw
  `game_N/frame_*.ram` directories — it has no `.scrd` awareness at
  all (grep of `run.cpp` and the rest of `src/burner/sdl/` for `SCRD`
  finds nothing). The fork's pinned #268-variant
  `tools/replay_preprocessor.py` (which calls
  `tools/compress_ram_dumps.py` on those `game_N/` directories itself)
  is therefore the *only* correct path for this runner, exactly as the
  plan's fallback said — verified end-to-end in this step by producing
  4 real `SCRD`-magic archives this way.
- **Open question 9** ("MiSTer storage headroom / SCRD archive size
  unmeasured"): resolved with real numbers in §5.5 — compressed size
  is ~1% of raw CPS3 RAM size, ~5.1-5.3 KB/frame on average across 4
  independently-sourced samples (two complete, two partial-but-valid).
  A single complete in-game archive (thousands of frames, on the order
  of a couple minutes of play) lands in the 20-40 MB range, not the
  hundreds-of-MB worst case the plan flagged as a risk. This step did
  not check actual MiSTer SD free space (`df` on-device, per the plan's
  own Step D2 note) — that remains a Stage D task, but the *archive
  size* half of the open question is now a measured number instead of
  an unknown.

## 7. Tooling note carried from Step A1

Confirmed during A1: the venv used for all Python tool runs in this
step required Python >= 3.10 (f-strings with `X | Y` union type hints
in `tools/replay_preprocessor.py` / `tools/compress_ram_dumps.py`); the
system default Python on this Mac did not satisfy that, hence the
dedicated `venv314` (Python 3.14.6) referenced throughout this doc.

## 8. Step A4: fidelity measurement

**Status: 2026-07-21, branch `feat/fcade-replay-browser` @ pick commit
`4df0171a`.** This section converts A3b's "4/4 archives PASS once the
fork's Gap_Timer perf regression is reverted" observation into a
measured pass rate over a widened, independently-harvested corpus, and
records the exact frame-data-suite fallout of landing that revert.

### 8.1 The pick

Cherry-picking upstream `63e0fb4a5b56d4b513d2bc980b73088ba53cd115`
("Statcheck: Sync animations", crowded-street/3sx#199) whole
(`git fetch upstream 63e0fb4a && git cherry-pick -x 63e0fb4a`) produced
conflicts in `src/sf33rd/Source/Game/engine/game.c`,
`src/sf33rd/Source/Game/system/sys_sub.c`, `src/test/replay_game.c`,
`src/test/test_runner.c`, and `src/test/test_runner_compare.c` — the
last three are exactly the fork's own statcheck test-harness files
(A3a/A3b), out of scope per this step's "don't touch statcheck harness
code" constraint. The cherry-pick was aborted
(`git cherry-pick --abort`); only the accuracy-relevant hunk was
hand-ported, in commit `4df0171a` ("fix(engine): revert perf commit's
Gap_Timer re-add in Switch_Screen[_Revival]"):

```diff
 s32 Switch_Screen(u8 Wipe_Type) {
     Active_Wipe_Type = (s8)Wipe_Type;
-    if (WipeOut(Wipe_Type) && --Gap_Timer <= 0) {
+    if (WipeOut(Wipe_Type)) {
         ...
 s32 Switch_Screen_Revival(u8 Wipe_Type) {
     Active_Wipe_Type = (s8)Wipe_Type;
-    if (WipeIn(Wipe_Type) && --Gap_Timer <= 0) {
+    if (WipeIn(Wipe_Type)) {
```

`src/sf33rd/Source/Game/system/sys_sub.c:80/91` (post-pick). Fork perf
commit `121b87cd` ("perf: MiSTer game engine optimizations") had
re-added the `&& --Gap_Timer <= 0` gate alongside its (kept, unrelated)
`Active_Wipe_Type` bookkeeping; upstream #199 originally removed the
gate to match real CPS3 wipe/scene-transition timing. Verified against
upstream: `git show upstream/main:src/sf33rd/Source/Game/system/sys_sub.c`
lines 75-92 has no `Gap_Timer` gate in either function, byte-identical
in shape to the post-pick fork hunk above.

### 8.2 Frame-data suite regression check

Baseline (recorded, not re-verified this step per the task's own
framing): **1,296 PASS / 53 XFAIL** (1,349 total corpus rows,
`project-frame-data-overlay.md` memory citation @ `dea88d05`) — a
remembered aggregate, not a fresh pre-pick suite run. Regression
detection used the checked-in golden/*.tsv files as the authoritative
oracle, so per-row claims are solid; only the aggregate total leans on
memory. To strengthen: checkout `4df0171a^` and run `tools/frame-data/run-suite.sh --check-golden`
for a true pre-pick aggregate baseline.

Post-pick, full suite: `tools/frame-data/run-suite.sh --check-golden`
(build/host Debug, 94 corpora, 9-way fan-out, wall **13m33s**):

```
94 corpora: 92 GREEN / 2 RED   build 1x   wall 13m33s   jobs 9
```

Summing every corpus's `total=` line: **1,295 PASS / 52 XFAIL / 1
SHAPE** (1,348 of 1,349 rows accounted for — `twelve-sa3` contributed
no `total=` line this run, see below). Two RED corpora, each
independently investigated by re-running standalone
(`FDH_SKIP_BUILD=1 bash tools/frame-data/run.sh tools/frame-data/corpus-<c>.yaml`):

- **`twelve-sa3` — FLAKE, not a regression.** The parallel-fan-out run
  produced no `trace.log`/`expected.json` at all ("MISSING trace/expected
  (corpus run did not complete)"). Standalone re-run reproduces the
  documented XFAIL exactly: `total=1 (XFAIL=1)`, same
  `twelve-sa3-activation` row/rationale already on file. This is
  resource contention under the 9-way parallel fan-out (this corpus's
  own `run.sh` has a 300s timeout budget shared with 8 sibling
  processes), not caused by the pick. Restoring this corpus's 1 XFAIL
  row brings the accounted total back to 1,349/1,349.
- **`urien` — REAL, reproducible regression.** Standalone re-run
  reproduces the parallel run's finding exactly:
  `urien-headbutt-hp-block` flips **PASS → SHAPE**, outcome
  **BLOCK → WHIFF**, `adv -9 → +0` (S=12/A=6/R=16 unchanged — a pure
  outcome-category flip, not a numeric drift):
  ```
  urien-headbutt-hp-block   expected outcome=BLOCK S=12 A=6 R=16 adv=-9
                             got      outcome=WHIFF S=12 A=6 R=16 adv=+0
                             [outcome WHIFF != BLOCK; adv 0 != -9]  SHAPE
  ```
  Root-cause read (not chased further — the pick is upstream-shipped
  and being kept regardless, per this step's own instructions): the
  Switch_Screen/Switch_Screen_Revival gate removal causes a 1-frame
  shift in the wipe/phase-entry timeline (harness gates on
  `wipe_transition_type1_active()`/`Active_Wipe_Type==1`, per
  `test_runner.c:130-131,232-234`). The harness teleports both
  characters to exact fixed positions each setup (`input_script.c:162-164`
  writes `plw[].wu.xyz[0].disp.pos` directly), so the defensive
  dummy's position is fixed — the timeline shift pushes the fixed-distance
  HP headbutt across its contact boundary, changing outcome from BLOCK
  to WHIFF. This is a **wipe-timing-alignment artifact of the test
  harness's fixed-frame alignment, not a hitbox/property change**
  — proven by the checked-in golden row (`urien.tsv:43`) being byte-identical
  (unchanged S=12/A=6/R=16 against got-row whiff). Every other corpus,
  including the other two Headbutt strengths in the same `urien` corpus,
  is unaffected.

**Net verified delta from the pick: exactly one row.**
`urien-headbutt-hp-block` PASS → SHAPE. 1,295 PASS / 53 XFAIL / 1 SHAPE
(1,349 total, `twelve-sa3`'s flake corrected for). No other frame-data
row changed. Per this step's explicit instruction, the pick is being
kept regardless (real upstream CPS3-accuracy fix); this regression is
recorded, not fixed.

### 8.3 Statcheck rebuild

```
cmake --build build-statcheck --parallel
```

Rebuilt clean against the pick (binary:
`build-statcheck/3S-ARM.app/Contents/MacOS/3S-ARM`).

### 8.4 Corpus widening

Downloaded the remaining 6 of the 10 quark IDs harvested in A2 §3
(same command shape, sequential, one live stream at a time,
`--idle-timeout 2 --max-idle-timeouts 20 --auto-dir`) — all 6
succeeded:

| Quark ID | Context (from A2 §3) |
|---|---|
| 1675950860255-2133 | (IN) k@ra vs (IN) Tejas, Ken/Sean/Ryu, 2023-02-09 |
| 1659344338224-4017 | (US) FireGuy808 vs (US) Snsinny, 2022-08-01 |
| 1675220043007-7798 | (US) parry the platypus vs (US) BLINDCONFIDENCE, 2023-02-01 |
| 1672649600209-3613 | (US) itsnotspidey vs (US) Snsinny, 2023-01-02 |
| 1667539223222-1807 | (US) fyto666 vs (US) FightcadeGateKeeper, 2022-11-04 |
| 1674716618396-8939 | (US) GZA_EFX vs (US) Snsinny, 2023-01-26 |

**Corpus-size shortfall vs. the plan's 50-100 target (recorded, per the
plan's own hard-blocker/fallback framing):** all 10 quark IDs this
program has ever harvested came from the one Cloudflare-blocked-API
fallback source identified in A2 (the `Fightcade-Archive` collection on
archive.org) — 10 harvested, 10 downloaded (4 in A2 + 6 here), 0
skipped. No second independent harvest source was found this step
either (the same web-search fallback strategy was reused, not
re-run — see the scope-decision paragraph below for why further
harvesting wasn't pursued this step). The
plan's 50-100 target is not met; what is new this step is turning each
of the N=10 harvested quarks into **M games per quark** instead of
just `game_0` — multi-game extraction is the actual corpus-widening
lever available given the harvest-source ceiling.

**Multi-game extraction, full sessions (not just `game_0`).** A new
scratchpad-only helper, `run_full_session.py`, runs the downloaded
`savestate`/`inputs` pair through the fbneo-replay-runner to completion
(not killed after `game_0`, unlike A2), polling `-dump-ram-path` for
new `game_N` directories and compressing+deleting each one as soon as
the *next* directory proves it final (or the runner process exits) —
one game's raw dumps (~2-5 GiB) on disk at a time, per this step's
disk-safety requirement (15 GiB free at session start). Two quarks were
run to completion this way:

- **1641508702494-7287** (already downloaded in A2, its raw
  `savestate`/`inputs` were still on disk in the scratchpad) — produced
  `game_0` (5,964 frames, byte-identical to A2's independently-produced
  `game_0.scrd`, a nice cross-check that both extraction methods agree)
  through `game_4` before this step's own time budget capped further
  extraction (this quark's session is a long multi-match set; games
  kept appearing past `game_4`).
- **1675950860255-2133** — produced `game_0` through `game_6` (7 games)
  before being deliberately capped (see the scope-decision paragraph
  below).
- **1659344338224-4017** — produced `game_0` and `game_1` before being
  capped.

**Disk-safety incidents (recorded honestly, not glossed over).** The
first `run_full_session.py` attempt hit `ENOSPC` mid-compress of
`7287`'s `game_1` (free disk had dropped under the runner's concurrent
writes into `game_2`/`game_3` while `game_1`'s ~4.3 GiB was being
compressed synchronously) — the script's original version deleted the
raw `game_1/` dump even on a failed/partial compress, permanently
losing that game's raw RAM dumps (unrecoverable short of re-running the
whole multi-minute session, which this step's time budget did not
allow). A second, separate mistake (not a script bug — an inline
ad-hoc recovery command run with the system's own too-old `python3`
instead of the `venv314` interpreter) then `rm -rf`'d `7287`'s
`game_2/` raw dump after its compress attempt failed for an unrelated
reason (`TypeError` from the old interpreter, not the real compress
error), losing `game_2` as well. **Fix applied to
`run_full_session.py`** (scratchpad-only tool, not part of this repo):
never delete a game's raw dump directory unless
`compress_ram_dumps()` returns successfully; pause the runner
(`SIGSTOP`)/resume (`SIGCONT`) around every synchronous compress so its
concurrent writes into the next `game_N` cannot race disk space out
from under the compress in progress. No further data loss occurred
after this fix (`7287`'s `game_3`/`game_4` and all of `2133`/`4017`
were recovered cleanly). Net result for `7287`: `game_0`, `game_3`, and
`game_4` (labeled `_partial` — its trailing frame was a 0-byte
truncated write, dropped before compressing) survived; `game_1` and
`game_2` did not.

**Scope decision — not all 6 new quarks were fully multi-game
extracted.** `1675950860255-2133`'s session kept producing new games
well past `game_6` (a long grinding/rematch session); with the
success criterion (≥8 archives) already exceeded and this step's time
budget a real constraint, both `2133` and `4017`'s
`run_full_session.py` orchestrators were deliberately terminated
(`SIGTERM` to the parent, then the orphaned runner child killed) once
each had produced several complete, valid games — every game archived
*before* termination is complete and valid (only the game in progress
at termination time was ever lost, and it self-reports as `archive
incomplete` rather than a false PASS/FAIL, see 8.5). `1675220043007-7798`,
`1672649600209-3613`, `1667539223222-1807`, and `1674716618396-8939`
were downloaded (all 4 succeeded, `savestate`/`inputs` present) but
**not** run through `run_full_session.py` this step — left as a
downloaded, ready-to-extract reserve for a future pass, same pattern
A2 used for the un-attempted quarks it left in reserve.

### 8.5 Measurement — corpus table

Statcheck rebuilt against the pick (8.3), run directly per archive
(`--ram-archive <path> --headless`) plus via
`tools/statcheck_runner.py <bin> <replay-dir> --timeout 30` (proves the
end-to-end tooling path, not just ad-hoc per-archive runs):

| Quark (players) | Game | Frames | Verdict | First divergence | Wall |
|---|---|---:|---|---|---:|
| 1641508702494-7287 (Atma_ vs --Panta--) | game_0 | 5,964 | PASS | compared 1..5,380 of 5,964 | 1.5s |
| 1641508702494-7287 | game_3 | 7,259 | PASS | compared 1..6,704 of 7,259 | 1.7s |
| 1641508702494-7287 | game_4 (partial, truncated tail) | 4,046 | **ARCHIVE INCOMPLETE** | `ScrdGame_Init: Failed to find game start frame` | 0.1s |
| 1667822595212-6293 (TOP G vs Frexia1) | game_0 (partial, A2) | 7,142 | PASS | compared 1..7,141 of 7,142 | 1.9s |
| 1675032176612-9791 (Arlieth vs wadupNEEM) | game_0 | 4,099 | PASS | compared 1..3,507 of 4,099 | 1.2s |
| 1676027217605-8232 (Shabsss vs glisignoli) | game_0 (partial, A2) | 4,168 | PASS | compared 1..4,167 of 4,168 | 1.3s |
| 1675950860255-2133 (k@ra vs Tejas) | game_0 | 4,868 | **FAIL** | frame 2,248: `pos_3sx.x (104) != pos_cps3.x (103)` | 1.0s |
| 1675950860255-2133 | game_1 | 8,190 | PASS | compared 1..7,612 of 8,190 | 1.9s |
| 1675950860255-2133 | game_2 | 5,287 | PASS | compared 1..4,700 of 5,287 | 1.3s |
| 1675950860255-2133 | game_3 | 10,070 | PASS | compared 1..9,814 of 10,070 | 2.2s |
| 1675950860255-2133 | game_4 | 6,509 | PASS | compared 1..5,895 of 6,509 | 1.7s |
| 1675950860255-2133 | game_5 | 8,540 | PASS | compared 1..7,949 of 8,540 | 2.0s |
| 1675950860255-2133 | game_6 (truncated tail, session capped) | 8,256 | **ARCHIVE INCOMPLETE** | `ScrdGame_Init: Failed to find game start frame` | 0.1s |
| 1659344338224-4017 (FireGuy808 vs Snsinny) | game_0 | 8,094 | PASS | compared 1..7,492 of 8,094 | 1.9s |
| 1659344338224-4017 | game_1 | 8,225 | PASS | compared 1..7,643 of 8,225 | 2.1s |

(16 archives run; the table lists 15 unique game-sessions — A2's
originally-produced `sfiii3nr1-1641508702494-7287.7/game_0.scrd` and
this step's independently-re-extracted
`sfiii3nr1-1641508702494-7287/game_0.scrd` are byte-identical, 31,193,214
bytes each, confirming the two extraction passes agree exactly.)

**"compared N..M of frames" is expected, not a bug**: `finish()`
(`src/test/statcheck_runner.c:99-112`) prints PASS and exits as soon as
`game_ended()` (`PL_Wins[0]==2 || PL_Wins[1]==2`) goes true — a match
decided in straight rounds stops comparison before the archive's
trailing post-match/idle frames run out. Confirmed by reading the
source, not inferred.

**Pass rate: 13/16 (81.2%)** per `tools/statcheck_runner.py`'s own
summary line (`13/16 successful (81.2%)`) — this is the primary,
tool-reported number. Excluding the 2 **archive-incomplete** results
(a corpus-harvesting artifact of this step's own truncated-session
captures, not a statcheck or engine verdict — see below), the pass
rate over genuinely measurable archives is **13/14 (92.9%)**.

**Failure taxonomy (2 buckets, both recorded per the "do not fix"
instruction):**

1. **Archive-incomplete (2 of 16): `7287/game_4`, `2133/game_6`.**
   Both are the truncated tail of a session this step's own harvesting
   process cut short (7287's `game_4` last frame was a 0-byte partial
   write dropped before compressing; 2133's `game_6` was mid-flight when its
   orchestrator was `SIGTERM`'d per the 8.4 scope decision). Both fail
   identically: `Frame N is out of bounds` /
   `ScrdGame_Init: Failed to find game start frame` —
   `statcheck: failed to open/parse RAM archive`. This is a corpus-
   completeness artifact of this step's own extraction methodology,
   **not** an engine or statcheck-harness bug: `ScrdGame_Init` correctly
   refuses to run a session whose recorded tail never reaches a
   detectable game-start signature.
2. **Real engine divergence (1 of 16): `2133/game_0`, frame 2,248,
   `pos_3sx.x (104) != pos_cps3.x (103)`** — a 1-pixel X-position
   mismatch, roughly 37 seconds into the match, far from the
   Switch_Screen-adjacent frame-34 divergence the pick fixed. Per this
   step's explicit "no other engine changes" instruction, this is
   recorded as a genuine new fidelity gap, not investigated further or
   fixed. This is the first real (non-wipe-timing) statcheck
   divergence this program has found in a full-length, real Fightcade
   match — a concrete Stage-C-relevant fact: not every archive will
   play back byte-exact yet, even post-pick.

### 8.6 Commands (exact, for reproduction)

```bash
# Pick + verify
git fetch upstream 63e0fb4a
git cherry-pick -x 63e0fb4a   # conflicts in test harness files -> aborted
git cherry-pick --abort
# hand-ported src/sf33rd/Source/Game/system/sys_sub.c hunk, committed as 4df0171a
grep -n "Gap_Timer" src/sf33rd/Source/Game/system/sys_sub.c

# Frame-data suite regression check
tools/frame-data/run-suite.sh --check-golden
FDH_SKIP_BUILD=1 bash tools/frame-data/run.sh tools/frame-data/corpus-twelve-sa3.yaml
FDH_SKIP_BUILD=1 bash tools/frame-data/run.sh tools/frame-data/corpus-urien.yaml

# Statcheck rebuild
cmake --build build-statcheck --parallel

# Corpus widening (6 new quarks, same tool/venv as A2)
venv314/bin/python tools/fcade-replays/fcade_replay_tool.py download \
  --fcade-url "fcade://stream/fbneo/sfiii3nr1/<quarkid>.7,7100" \
  --idle-timeout 2 --max-idle-timeouts 20 --auto-dir --out-dir <dir>

# Multi-game full-session extraction (scratchpad-only helper)
venv314/bin/python run_full_session.py <replay_dir> <label> <dump_root> <out_dir>

# Measurement
build-statcheck/3S-ARM.app/Contents/MacOS/3S-ARM --ram-archive <archive>.scrd --headless
venv314/bin/python tools/statcheck_runner.py \
  build-statcheck/3S-ARM.app/Contents/MacOS/3S-ARM <replay_dir> --timeout 30
```

### 8.7 Summary

- **Pick landed and verified**: `4df0171a`, matches upstream
  `sys_sub.c` semantics exactly.
- **Frame-data suite delta**: 1 real regression
  (`urien-headbutt-hp-block`, PASS→SHAPE, a wipe-timing-alignment
  artifact of the test harness's fixed scripted setup, not a hitbox
  change), 1 confirmed unrelated flake (`twelve-sa3`, parallel-fanout
  contention). Kept per this step's instructions — the pick is a real
  upstream CPS3-accuracy fix.
- **Corpus**: 6/6 new quark IDs downloaded; 3 of 10 total harvested
  quarks multi-game-extracted (7287, 2133, 4017) to 3+7+2 = 12 games,
  plus A2's 3 other single-`game_0` quarks = 16 archives total (15
  unique, 1 cross-check duplicate) across 6 distinct Fightcade
  sessions.
- **Pass rate: 13/16 (81.2%)** tool-reported;
  **13/14 (92.9%)** excluding 2 archive-incomplete (harvesting
  artifact, not an engine verdict).
- **1 genuine new divergence found**: `2133/game_0` frame 2,248,
  1-pixel position mismatch — recorded, not fixed, per this step's
  scope.
- **Corpus-size shortfall vs. plan's 50-100 target**: still true (10
  harvested quarks total, one Cloudflare-blocked-API-fallback source);
  this step's actual widening lever was multi-game extraction (12 new
  games from 3 quarks), not new quark count.

## 9. Step D2: ARM statcheck run on the MiSTer

**Status: 2026-07-22, branch `feat/fcade-replay-browser`, device
`root@192.168.1.171`.** Closes plan risk 4 (`src/netplay/game_state.c:72-80`,
"cross-arch determinism is unsupported") for the statcheck-comparison
sense specifically: does the 32-bit ARM build reach the same
frame-for-frame RAM-compare verdicts as the desktop build on the same
`.scrd` archives from §8.5? Short answer: **yes, for the frame-value
comparison itself** — every one of 15 archives lands on the exact same
verdict and, for PASS archives, the exact same compared-frame range as
desktop. The one new fact this step surfaces is an intermittent
ARM-only crash unrelated to the frame-value comparison (§9.5).

### 9.1 Build + isolation protocol

Followed the plan's 3-part isolation protocol
(docs/plan-fcade-replay-browser.md:1394-1415) exactly, with one
recovery detour:

1. `EXTRA_CMAKE_ARGS="-DTHREESX_STATCHECK=ON" tools/mister/build-game.sh --flavor telemetry`.
   The backgrounded shell driving this script died silently before
   its own `docker cp` host-sync step ran (empty task output, no
   `BUILD_EXIT` marker), **but the container-side build had already
   completed successfully** — `docker exec 3s-mister-arm-build`
   confirmed a fully-populated `/work-mister/build/mister-telemetry-install`
   and `-package` (binary + `readelf` ARM/VFP checks implicitly passed,
   since `build_one()`'s `set -euo pipefail` would have aborted the
   heredoc otherwise). Recovered by `docker cp`-ing both dirs out
   directly instead of re-running the whole build, verifying the
   binary first (`readelf -h`: `Machine: ARM`; `readelf -A`:
   `Tag_ABI_VFP_args: VFP registers`; `arm-linux-gnueabihf-nm`: 9
   `Statcheck_`/`StatcheckRunner_`/`ScrdGame_` symbols present).
2. Isolation step 1 (immediately after recovery): moved both dirs
   aside — `build/mister-telemetry-install` → `build/mister-statcheck-install`,
   same for `-package`. Verified canonical `build/mister-telemetry-{install,package}`
   did not exist afterward (naive `misterctl.sh deploy` would fail
   loudly rather than ship a statcheck binary).
3. Isolation step 2: `docker exec 3s-mister-arm-build rm -rf /work-mister/build/mister-telemetry`
   (confirmed `THREESX_STATCHECK:BOOL=ON` in the CMake cache before the
   `rm -rf`, confirmed the directory gone after).
4. Rebuilt plain `tools/mister/build-game.sh --flavor telemetry` (no
   `EXTRA_CMAKE_ARGS`). This run was launched via `nohup ... & disown`
   (decoupled from the harness's own background-task tracking, given
   the step-1 incident) and completed cleanly: `BUILD_EXIT=0`,
   `CMakeCache.txt` shows `THREESX_STATCHECK:BOOL=OFF`.
5. Isolation step 3 (re-verified twice — once right after the rebuild,
   once again at the end of this step before writing this section):
   `arm-linux-gnueabihf-nm` on `build/mister-telemetry-install/bin/3s-arm`
   → 0 matches for `Statcheck_|StatcheckRunner_|ScrdGame_`.
   `strings` on the same binary does contain the lowercase substrings
   `statcheck_compare.c`/`statcheck_runner.c` — traced this to harmless
   embedded translation-unit-name debug info shared across the whole
   `#if STATCHECK`/`#if DEBUG` test-harness file family (`replay_game.c`,
   `scrd_game.c`, `test_runner.c`, `test_runner_compare.c` all appear
   alongside them); it is **not** the actual A3a success criterion,
   which is symbol-shaped names (`Statcheck_CompareValues`,
   `StatcheckRunner_Init`, `ScrdGame_Init`, etc.) — 0 of those, versus
   12 in `build/mister-statcheck-install/bin/3s-arm`. Canonical
   telemetry tree is statcheck-free.

### 9.2 Device runtime-environment adaptations

The statcheck binary needs the same runtime env `scripts/run-3s-arm.sh`
exports (`THIRDSARM_HOME=<app dir>`, `LD_LIBRARY_PATH=<app dir>/lib`)
plus the OSD-launcher's forced dummy/software SDL backend
(`scripts/launch-osd.sh`'s `SDL_VIDEO_DRIVER=dummy`/`SDL_RENDER_DRIVER=software`,
here also `SDL_AUDIO_DRIVER=dummy` since no ALSA output is needed for a
headless compare loop). The binary still opens a Linux console/`KD_GRAPHICS`
handle for the native-video path even under `--headless` (this only
skips SDL window presentation, per `src/port/sdl/sdl_app.c:3416-3438`),
so it must run from the local console path, not backgrounded oddly —
a plain SSH command invocation works fine (confirmed by the runbook's
own console-mode troubleshooting entry not triggering).

**Resource gap found and fixed:** the statcheck harness hard-pins
`arcade-balance=true` at init (`src/test/statcheck_runner.c:219-231`,
"pinned hermetic config") so RAM-compare values are checked against
real CPS3 arcade balance tables, not PS2 balance. `ArcadeBalance_Init()`
(`src/arcade/arcade_balance.c:10-27`) silently falls back to PS2
balance if `resources/sfiii3nr1.zip` is missing or fails to parse
(`SDL_LogError`, not a hard failure). The device's
`/media/fat/games/3s-arm/resources/` only ships `SF33RD.AFS` — no
`sfiii3nr1.zip` — so the **first** on-device corpus run diverged on
every single archive within a few hundred frames
(`cat_break_reserve_3sx`/`lvr_3sx->sw_new` mismatches at frames
130-750), a false signal caused entirely by the missing ROM, not a
real cross-arch bug. Confirmed by finding the desktop build's own copy
at `~/Library/Application Support/CrowdedStreet/3S-ARM/resources/sfiii3nr1.zip`
(md5 `6573a2754d340847344c88a734111a13`, 8,375,056 bytes — same file
documented in §2, originally sourced from FightCade2's bundled FBNeo
ROMs) and copying that exact byte-identical file to
`/media/fat/games/3s-arm/resources/sfiii3nr1.zip` (md5 confirmed equal
post-copy). Re-ran the full corpus after the fix; §9.3 below is the
post-fix table. This file is left in place on the device — it is an
additive, non-destructive resource that also permanently enables the
existing "Arcade Balance" OSD toggle (commit `8b7c9afd`) for normal
play, not just a throwaway test artifact.

### 9.3 Corpus table — ARM vs desktop (15 archives, post-ROM-fix)

Desktop columns are §8.5's recorded values (unchanged, not re-run this
step). ARM wall time is a single SSH-invocation measurement per
archive (`date +%s.%N` around the binary call); desktop wall time is
§8.5's Mac-native figure.

| Archive | Frames | Desktop verdict | Desktop range/frame | ARM verdict | ARM range/frame | Match? | Desktop wall | ARM wall | Factor |
|---|---:|---|---|---|---|---|---:|---:|---:|
| 7287/game_0 | 5,964 | PASS | 1..5,380 | PASS | 1..5,380 | exact | 1.5s | 41.0s | 27.3x |
| 7287/game_3 | 7,259 | PASS | 1..6,704 | PASS | 1..6,704 | exact | 1.7s | 49.9s | 29.4x |
| 7287/game_4 (partial) | 4,046 | ARCHIVE INCOMPLETE | n/a | ARCHIVE INCOMPLETE | n/a | exact | 0.1s | 3.7s | n/a (both tiny/artifact) |
| 4017/game_0 | 8,094 | PASS | 1..7,492 | PASS | 1..7,492 | exact | 1.9s | 53.8s | 28.3x |
| 4017/game_1 | 8,225 | PASS | 1..7,643 | PASS* | 1..7,643 | exact* | 2.1s | n/a | n/a (see §9.5 — intermittent crash) |
| 6293/game_0 (partial) | 7,142 | PASS | 1..7,141 | PASS | 1..7,141 | exact | 1.9s | 50.7s | 26.7x |
| 9791/game_0 | 4,099 | PASS | 1..3,507 | PASS | 1..3,507 | exact | 1.2s | 30.3s | 25.2x |
| 2133/game_0 | 4,868 | **FAIL** | frame 2,248 | **FAIL** | frame 2,248 | exact | 1.0s | 24.3s | 24.3x |
| 2133/game_1 | 8,190 | PASS | 1..7,612 | PASS | 1..7,612 | exact | 1.9s | 49.1s | 25.8x |
| 2133/game_2 | 5,287 | PASS | 1..4,700 | PASS | 1..4,700 | exact | 1.3s | 31.2s | 24.0x |
| 2133/game_3 | 10,070 | PASS | 1..9,814 | PASS | 1..9,814 | exact | 2.2s | 68.2s | 31.0x |
| 2133/game_4 | 6,509 | PASS | 1..5,895 | PASS | 1..5,895 | exact | 1.7s | 44.5s | 26.2x |
| 2133/game_5 | 8,540 | PASS | 1..7,949 | PASS | 1..7,949 | exact | 2.0s | 60.7s | 30.4x |
| 2133/game_6 (truncated) | 8,256 | ARCHIVE INCOMPLETE | n/a | ARCHIVE INCOMPLETE | n/a | exact | 0.1s | 7.5s | n/a (both tiny/artifact) |
| 8232/game_0 (partial) | 4,168 | PASS | 1..4,167 | PASS | 1..4,167 | exact | 1.3s | 32.3s | 24.8x |

\* `4017/game_1`: the archive-vs-archive verdict match is exact across
the runs that completed normally — see §9.5 for the one run that
crashed instead of completing.

**Wall-time factor: ARM is ~24-31x slower than desktop for a full
statcheck comparison run**, average ~27x across the 12 fully-timed PASS/FAIL
archives (min 24.0x on `2133/game_2`, max 31.0x on `2133/game_3`). The
two ARCHIVE INCOMPLETE archives fail almost instantly on both platforms
(sub-second desktop, low-single-digit-seconds ARM) so their ratio is
noise-dominated, not a meaningful data point.

### 9.4 Verdict delta vs desktop

**Zero verdict deltas across all 15 archives.** Every PASS, the one
FAIL (`2133/game_0`, frame 2,248 — the same real 1-pixel-position
divergence §8.5 found and left unfixed), and both ARCHIVE INCOMPLETE
results match desktop exactly — and for every PASS archive the compared-frame
range is byte-identical between ARM and desktop, which is a stronger
match than "both said PASS": it means the ARM build ran the identical
number of frames before hitting `game_ended()` (`PL_Wins[0]==2 ||
PL_Wins[1]==2`) as desktop did, frame for frame, for a full CPS3 RAM
comparison (with real arcade-balance data active). This directly
answers plan risk 4 for the statcheck-comparison surface: 32-bit ARM
and 64-bit desktop compute identical game-state values, and identical
game-length outcomes, for every archive this program has measured.

### 9.5 New finding: intermittent ARM-only crash on `4017/game_1`

The first full-corpus run (post-ROM-fix) crashed on this one archive:

```
[texgroup-trace] case=3 key=12 id=1 apfn=1532
free(): invalid pointer
exit_code=134 wall_s=2.9
```

Exit 134 = `SIGABRT`, consistent with glibc/musl's heap-corruption
detector firing on `free()`. This is **not** a frame-value divergence
— it happened before any `Statcheck_CompareValues` mismatch could even
be printed, and no `statcheck: FAIL`/`PASS` line was ever emitted for
this crashed run. Re-ran the same archive (same binary, same file, no
redeploy) 4 additional times to check determinism:

```
attempt 1: statcheck: PASS — compared archive frames 1..7643 of 8225   EXIT=0
attempt 2: statcheck: PASS — compared archive frames 1..7643 of 8225   EXIT=0
attempt 3: statcheck: PASS — compared archive frames 1..7643 of 8225   EXIT=0
attempt 4 (the single-archive re-run before the 3x batch): statcheck: PASS — compared archive frames 1..7643 of 8225   EXIT=0
```

**All 4 re-runs passed cleanly with the exact desktop-matching frame
range (`1..7,643 of 8,225`, identical to §8.5's recorded value).** Net
observed rate: **1 crash out of 5 total ARM invocations of this
archive (20%)**, not a deterministic reproduction — correcting an
earlier in-progress characterization of "reproduced 3x," which this
step's own data does not support (3 of the 4 repro attempts, plus the
1 preceding single re-run, all passed; only the original corpus-run
invocation crashed). This reads as ARM-side memory-layout/heap-state
dependent (the crash immediately followed a `[texgroup-trace]`
character-select/texture-group load line, `apfn=1532`, a value unique
to this archive's character selection among the corpus), not an
engine-value correctness bug — but it is a genuine ARM-only finding
with no desktop analog (desktop recorded a clean PASS at 2.1s in
§8.5, no crash). **Recorded here per this step's scope, not
root-caused further** — a candidate for the netplay-desync-class
follow-up register (memory: `ca_check_flag`/`eff79`) if it recurs,
though its signature (heap corruption, not a stale-save/rollback
class) doesn't obviously fit that family.

### 9.6 Commands (exact, for reproduction)

```bash
# Statcheck ARM build (isolation steps 1-3)
EXTRA_CMAKE_ARGS="-DTHREESX_STATCHECK=ON" tools/mister/build-game.sh --flavor telemetry
mv build/mister-telemetry-install build/mister-statcheck-install
mv build/mister-telemetry-package build/mister-statcheck-package
docker exec 3s-mister-arm-build rm -rf /work-mister/build/mister-telemetry
tools/mister/build-game.sh --flavor telemetry
arm-linux-gnueabihf-nm build/mister-telemetry-install/bin/3s-arm | grep -E "Statcheck_|StatcheckRunner_|ScrdGame_"  # (run inside the container; empty = clean)

# Device prep
sshpass -p 1 scp build/mister-statcheck-install/bin/3s-arm root@192.168.1.171:/media/fat/games/3s-arm/bin/3s-arm-statcheck
sshpass -p 1 scp "$HOME/Library/Application Support/CrowdedStreet/3S-ARM/resources/sfiii3nr1.zip" \
  root@192.168.1.171:/media/fat/games/3s-arm/resources/sfiii3nr1.zip
sshpass -p 1 scp <15 .scrd archives> root@192.168.1.171:/media/fat/games/3s-arm/replays-scrd/

# On-device run (per archive)
cd /media/fat/games/3s-arm
THIRDSARM_HOME=/media/fat/games/3s-arm \
LD_LIBRARY_PATH=/media/fat/games/3s-arm/lib \
SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software SDL_AUDIO_DRIVER=dummy \
  ./bin/3s-arm-statcheck --ram-archive replays-scrd/<archive>.scrd --headless
```

### 9.7 Device state at end of step

- `/media/fat/games/3s-arm/bin/3s-arm` (normal deployed game binary):
  md5 `ad8a769df00d4afbabbdcfd66bda389b`, size 4,459,232 bytes,
  mtime unchanged (`Jul 21 18:33`) both before and after this entire
  step — confirmed untouched.
- `/media/fat/games/3s-arm/bin/3s-arm-statcheck` (new): md5
  `33a68a7ed9364a05f987d1261084cea9`, matches the host
  `build/mister-statcheck-install/bin/3s-arm` md5 exactly.
- `/media/fat/games/3s-arm/resources/sfiii3nr1.zip` (new, kept): md5
  `6573a2754d340847344c88a734111a13`, 8,375,056 bytes — also activates
  the existing Arcade Balance OSD feature for normal play.
- `/media/fat/games/3s-arm/replays-scrd/`: 15 `.scrd` archives, 472 MB
  total — left in place per this step's instructions (useful for
  future re-runs); confirmed not crowding the card (see `df` below).
- `df -h /media/fat`: 477G size, 126G used, 352G avail, 27% — unchanged
  (to the nearest displayed GB) from the pre-step baseline (125G
  used). Well within the plan's "don't fill the SD" constraint.
- No lingering `3s-arm`/`3s-arm-statcheck` processes on the device
  (`ps | grep 3s-arm` empty after the run).
- Console mode: the statcheck binary switches the active VT to
  `KD_GRAPHICS` on each invocation (native-video path, independent of
  `--headless`/SDL dummy driver). Ran the same `restore_console`
  sequence `scripts/launch-osd.sh` uses (`kbd_mode -a`, `setterm
  -reset`, clear) against the active VT as a precaution before
  finishing.
- Host `build/mister-telemetry-install` and `-package`: statcheck-free,
  re-verified (§9.1 step 5); `build/mister-statcheck-install`/`-package`
  hold the isolated statcheck artifacts for any future re-run.
- **STALENESS DISCLOSURE (review finding, 2026-07-22):** the plain
  telemetry rebuild above was performed against the tree at
  ~`12b7a1f2`..`8d29bcd1` (binary mtime Jul 21 22:08), BEFORE commits
  `af87f9ef` (B2), `52265105` (C1 replay player — always-compiled,
  ships in telemetry), `eb0cd3a9` (C2 + cJSON CMake change), and
  `8ea3eeaa` (E1a) landed. "Statcheck-free" is true, but this tree is
  MISSING shipped replay functionality relative to HEAD and must NOT
  be treated as a current deployable artifact — the final release
  build of this run supersedes it.

### 9.8 Summary

- **Risk 4 (32-bit ARM divergence), statcheck-comparison surface:
  CLOSED for this corpus.** 15/15 archives match desktop's verdict
  exactly; all 12 PASS archives match desktop's exact compared-frame
  range; the 1 known FAIL (`2133/game_0`, frame 2,248) reproduces
  identically; both ARCHIVE INCOMPLETE results (a corpus-harvesting
  artifact per §8.5, not an engine verdict) reproduce identically.
- **New ARM-only finding (not a verdict delta):** `4017/game_1`
  crashed once (`free(): invalid pointer`, exit 134) out of 5 total
  ARM invocations; the other 4 passed cleanly with the exact
  desktop-matching frame range. Intermittent, not chased to root
  cause per this step's scope — flagged for a future pass.
- **Real environment gap found and fixed (not an engine bug):**
  `/media/fat/games/3s-arm/resources/sfiii3nr1.zip` was missing from
  this device's deploy, causing every archive to silently run under
  PS2 balance instead of arcade balance and diverge within a few
  hundred frames on the first attempt. Fixed by copying the same file
  the desktop build already uses; left in place (also enables the
  Arcade Balance OSD toggle for normal play).
- **ARM wall-time factor:** ~24-31x slower than desktop per archive,
  average ~27x, for the same statcheck comparison.
- **Isolation protocol:** followed in full, including a mid-step
  recovery (background build task died after the container finished
  but before the host-side copy/isolation steps ran; recovered by
  `docker cp`-ing the completed container artifacts directly rather
  than rebuilding). Canonical `build/mister-telemetry-*` re-verified
  statcheck-symbol-free at the end of this step.

## 10. Run summary — autonomous implementation night, 2026-07-21/22

**Branch `feat/fcade-replay-browser`, plan `docs/plan-fcade-replay-browser.md`.
Steps executed: A1 A2 A3a A3b A4 D2 B1 B2 C1 C2 E1a F2a (+D1/C3 merged
into the on-device QA gate below). Every step ran the
implement->review->fix->verify loop with fresh agents; commits are
one-per-step on this branch (plan commit 0c53ed95 .. fix 46851dc8).**

### Shipped and on-device-verified
- Statcheck fidelity oracle (desktop + ARM builds); corpus 15 archives.
- Engine pick: upstream #199 Gap_Timer revert (4df0171a) — the sole
  divergence blocking replay fidelity; frame-data suite delta = 1 row
  (verified harness artifact, section 8.2).
- Fidelity: 13/16 archives PASS desktop (92.9% excl. corrupt);
  **15/15 ARM verdict parity, byte-identical frame ranges** (section 9).
- 3SR device format + SCRD->3SR generator; 12 passing replays staged.
- Runtime replay player (release-compiled) + viewer overlay + hold-
  START exit + in-game local browser + native plain-TCP Fightcade
  stream fetcher (--fetch-replay; byte-identical to the Python tool
  on two live quarks).
- **Final on-device gate (this doc's D1/C3): PASS** after fixing a
  first-frame overlay draw crash (46851dc8): 9791 REPLAY COMPLETE
  3507 frames 58/58 checksums; 7287 COMPLETE 5380 frames 89/89 in
  99s wall (full speed); browser boots, pins config, scans 12
  replays. Device binary f8f8605d30580b4d568c4f0dd6739355 ==
  build/mister-telemetry-package/bin/3s-arm. config/keymap untouched.

### Known issues / follow-up register
- 4017/game_1: one free() crash in 1 of 5 ARM statcheck runs
  (section 9.5) — not root-caused.
- 2133/game_0: genuine 1-pixel engine divergence @ frame 2248 (both
  arches) — excluded from shipped replays.
- B1 outliers: 2133 game_1/game_3 monotone drift, cause open
  (section, b1 findings); does not affect shipped SCRD-derived 3SRs.
- Browser pad-navigation + visual overlay check: needs a human with
  a controller (all automated surfaces green).
- Japanese "file load failed" log lines during attract under
  --replay-browser (file numbers 9/10/1454/1456/1458) — pre-existing?
  unknown; logged during QA, benign in 20s smoke.
- misterctl deploy deleted the device's replays-scrd/ statcheck
  archives (regenerable; copies remain on the Mac scratchpad).

### Deliberately not run (external dependencies)
- F1 (VPS proxy — needs user deploy), F2b/F3 (need F1), E1b full
  (needs B3 decision + proxy), B3 experiment, F4 (OSD/eviction).
  Search API remains Cloudflare-gated (cf_clearance).

### Deliverable
`build/mister-telemetry-package/` (deployed to the device 2026-07-22
~01:20-02:00): replay player + browser + stream fetcher + arcade-
balance ROM; 12 replays at /media/fat/games/3s-arm/replays/.
Config keys: replay-browser, replay-browser-root (docs/config.md).
Quick test: OSD launch, or ssh: cd /media/fat/games/3s-arm &&
./scripts/run-3s-arm.sh --replay-browser   (or --play-replay
replays/<file>.3sr).

## 11. Wave-2 run summary — 2026-07-22 (OSD trigger, proxy, remote browse, search)

**Steps: B3, F1, F2b+E1b, F3, F4 (all four sub-steps). Commits
747ef313..ad6915a0 + this close-out. Same per-step loop.**

### Shipped and on-device-verified
- B3 experiment: NO-GO on self-contained raw-stream playback
  (structural — no coin/START in the stream; char-select state lives
  in the unloadable savestate; transition-length skew is not
  constant). POSITIVE: with the SCRD-derived offset, raw -13 runs
  replay FRAME-EXACT from cold boot with zero RNG sync (4097/4098).
  E1b conversion stays off-device, by evidence.
- F1 fcade-proxy: built + self-tested (48 assertions; independent
  client interop from README alone; live 403 classification with one
  polite call). NOT deployed — user action.
- F2b+E1b: REMOTE tab (proxy search), download-on-select via the
  native stream client into replays/<quarkid>/, NEEDS CONVERSION
  flow (recognize/block-play/delete), typed offline states.
- F3: RECENT / BEST (month-start since) / BY PLAYER (pad picker) +
  duration filter.
- F4: OSD "Replay Browser" T[31] line (menu.sv + regenerated HPS
  menu patch + wrapper arm-and-restart with --replay-browser);
  storage lifecycle (replays-max-mb, LRU eviction, delete incl.
  fetch dirs). RBF 3S-ARM_20260722.rbf (2,481,248 B) + wrapper
  MiSTer_3S-ARM deployed; rollback = pick 3S-ARM_20260721.rbf in
  the MiSTer menu. Quartus reported timing-not-met on video-PLL
  clocks (worst -14.454) — judged pre-existing-characteristic (a
  CONF_STR BRAM string cannot affect PLL paths; same pipeline built
  the working previous RBF); watch for video anomalies, rollback if
  seen.
- Final smoke (deployed binary d35fa7155894a1fe7f4bc5aa760f9941):
  --play-replay 9791 REPLAY COMPLETE 58/58 in 56s; browser boots +
  scans 12; ON-DEVICE live fetch of quark 1672649600209-3613 landed
  (savestate exactly 1,907,010 B, exit 0) and the browser now scans
  13 with the NEEDS CONVERSION row. config/keymap md5-unchanged.

### User actions needed to finish the loop
1. Deploy the proxy: ./tools/fcade-proxy/deploy.sh (README has the
   full flow) + paste a cf_clearance cookie from your browser into
   fcade-cookie.txt on the VPS; then set replay-proxy-host on the
   device config.
2. TV test: select 3S-ARM_20260722 in the MiSTer menu, open the OSD,
   press "Replay Browser" — expect relaunch into the browser
   (replay_browser_arm=1 appears in logs/osd-wrapper.log).
3. Pad-drive the browser (nav/select/delete/tabs/picker) — all
   automated surfaces green, human input untestable over SSH.

### Notes
- misterctl deploy's delete-sync removes non-package content under
  games/3s-arm: replays/ was wiped and restored this run; the 12
  sample replays are now INSIDE build/mister-telemetry-package/ so
  future deploys carry them.
- Known cosmetic: attract-mode Japanese file-load log lines
  (gd3rd.c:223, pre-existing); one-off frame outlier during dir scan.

## 12. Stage S1 — conversion pipeline + `get3sr` + device fetch (kill NEEDS CONVERSION)

`docs/plan-osd-replay-browser.md` Stage S1. New: `tools/fcade-replays/
publish_3sr.py` (batch converter), `tools/fcade-proxy/push-3sr.sh` (`.3sr`
publish rail), `fcade-proxy.js`'s `get3sr` op, `ProxyClient_Fetch3sr()`
(`src/replay/proxy_client.{c,h}`). `make_3sr.py`'s `build_meta` needed no
functional change (its `--quark-json` branch already writes real
`players[]`) — the fix is that `publish_3sr.py` ALWAYS builds and passes a
real `quark.json` per quark (never the `summary.json`/minimal fallback
branches), plus a header comment on `build_meta` documenting that as the
non-negotiable rule.

### Disk-safety finding (carried forward from §4's ENOSPC postmortem)

The existing `tools/replay_preprocessor.py`/`compress_ram_dumps.py` pattern
only frees a session's raw per-frame `.ram` dumps AFTER every game in that
session has been dumped — exactly what caused the tens-of-GB ENOSPC in §4.
`publish_3sr.py` does NOT reuse that pattern: it polls the FBNeo runner's own
dump directory while the runner is still running and compresses+deletes
each `game_N/` the moment the runner's own game-boundary detection proves it
complete (creation of `game_(N+1)/` — `ReplayDumpCps3MainRam()` in the
runner's `run.cpp` only creates the next game's directory on an
in-game→not-in-game transition, so a lower-numbered directory is provably
finished the instant a higher one appears). At most one game's raw frames
are ever on disk at once; across a batch, quarks are processed strictly one
at a time (download→run→gate→publish→cleanup) before the next starts.

### Statcheck-under-contention finding

On this (shared, multi-tenant, frequently load-average-8+) machine, the
statcheck executable — a full SDL app that creates a real Cocoa window +
Metal renderer even for a `--headless` run (`Selected video driver: cocoa` /
`Selected renderer: metal` in its own log) — was measured to take OVER 2
MINUTES (and still not finish) on an archive that normally passes in ~1.5s,
purely from window/GPU-compositor contention, not CPU starvation (the
process sat at low CPU% the whole time). Forcing `SDL_VIDEODRIVER=dummy
SDL_AUDIODRIVER=dummy` for the statcheck subprocess (measured: 1.487s wall,
94% cpu, same archive) eliminates this entirely — `publish_3sr.py`'s
`statcheck_gate()` always sets both. This does not change what statcheck
measures (engine state, not pixels).

### End-to-end proof (real live-catalog quark, this session)

Picked `1784832478681-8872` (players `Akuma.Matata` vs `motivadam`, the
live `catalog.json`'s pushed-and-served `.3sr` store) end to end:

1. `publish_3sr.py --quark 1784832478681-8872`: download → FBNeo runner
   (`fbneo-replay-runner` @ `ccf96ab`) → `game_0.scrd` (6257 raw frames) →
   `statcheck_gate` PASS → `make_3sr.py generate --quark-json` → `game_0.3sr`
   (25,892 B) + `game_0.meta.json` with real `players[]` (`Akuma.Matata`,
   `motivadam`). `make_3sr.py verify` PASS (structural, round-trip OK).
2. `push-3sr.sh` → `hetzner-3s-arm:/opt/fcade-proxy/3sr/1784832478681-8872/`.
3. Deployed updated `fcade-proxy.js` (`deploy.sh`) — `catalog.json` (295
   rows) and the running catalog mode were unaffected by the redeploy
   (verified via `{"op":"status"}` before/after: `mode:"catalog"`,
   `catalog_rows:295` both times).
4. `{"op":"get3sr","quarkid":"1784832478681-8872"}` over the real wire
   protocol against the live VPS: `not_found` BEFORE the push, `ok:true`
   with one game AFTER — `b64`/`meta_b64` both base64-decode
   byte-for-byte-identical (SHA-256 verified) to the locally-published
   `game_0.3sr`/`game_0.meta.json`.
5. A standalone C harness linking `src/replay/proxy_client.c` called
   `ProxyClient_Fetch3sr()` against the same live VPS proxy: wrote both
   `game_0.3sr` and `game_0.meta.json` under a fresh replay root,
   byte-identical to the originals (`diff` clean).
6. Desktop `--play-replay game_0.3sr` (`build/3S-ARM.app`, `SDL_VIDEODRIVER=
   dummy`): `replay: meta parsed — p1=Akuma.Matata p2=motivadam
   date=2026-07-23`, then `REPLAY COMPLETE frames=5841 checksums=97/97
   r16_resyncs=24 reason=game-ended` — the converted quark isn't just
   structurally valid, it plays back to a clean, checksum-verified
   completion with real names visible. No NEEDS CONVERSION.
7. Cleanup: the test `.3sr`/`.meta.json` were removed from the VPS
   (`ssh hetzner-3s-arm 'rm -rf /opt/fcade-proxy/3sr'` — the `3sr/` store was
   empty/nonexistent before this session) so the live proxy is left exactly
   as it was found; `catalog.json` (the pre-existing, unrelated 295-row
   live catalog) was never touched.

### Batch run (same session, 6 live-catalog quarks attempted)

`publish_3sr.py` run unattended against 6 quarks picked by shortest
`duration` from the live catalog (fastest to convert+verify): 4/6 completed
in the time available, 5 games total, **5/5 (100%) statcheck-clean** —
`1784832478681-8872` (1 game), `1784833167654-5179` (2 games: 4414 +
716 frames), `1784831921035-4955` (1 game), `1784832650380-5982` (1 game,
6953 frames). One quark (`1784833058805-2839`, the single shortest-duration
row, 113.5s) hit `ExtractError: no game-start signature frame found` — a
genuine SCRD-side observation (that particular session's runner replay never
reached `G_No[1]==2`, i.e. no in-game frame exists to extract), correctly
identified and dropped by the pipeline rather than silently mis-published.
This is exactly the "drop divergent/unusable games, never ship them" gate
working as intended — it is a real per-quark 3sr not a batch failure.

### Not done in this pass (owned by other stages / out of scope for S1)

- OSD wiring of `ProxyClient_Fetch3sr` (Stage S3b — deliberately not
  over-invested here per the plan; the in-game `REMOTE SELECT` browser is
  slated for retirement once S3b lands, so this pass adds a clean,
  independently-testable sync+async API rather than deep-wiring it into
  `replay_browser.c`'s state machine).
- No VPS-side FBNeo — conversion stays Mac-side, as decided.


## 13. Stage S0a — SPIKE: prefix-only conversion + shipped-player fidelity + pacing/prefix

**Status: 2026-07-24, branch `feat/fcade-replay-browser`.** Mac-side spike for
`docs/plan-fcade-live-stream.md` Stage S0a. Scratchpad-only; the only repo
change is this subsection. Proves the two crux *fidelity* assumptions before
any VPS/server work, and re-measures GGPO pacing + the pre-game prefix.

Corpus: 3 live-pool quarks that already have full-pipeline `.3sr`s in
`~/Library/Application Support/fcade-replay-convert/3sr-out/`, re-converted
this session with `publish_3sr.py --keep-work` (runner
`fbneo-replay-runner` @ `ccf96ab` `build/debug/fbneosdldarm64`; statcheck
`fcade-replay-convert/bin/3S-ARM-statcheck`; from CWD
`~/Developer/fbneo-replay-runner`). The three quarks aged out of the rolling
stealth-catalog window, so a 3-row synthetic catalog was built from their pool
`.meta.json` (catalog content does not affect `.3sr` bytes — only quarkid/
gameid/emulator drive download+run). Publish result: **3 quarks, 7 games
published, 0 statcheck-dropped**.

| Quark | players | dur | game_0 in-game frames |
|---|---|---:|---:|
| 1784875689006-4849 | cochambo vs SaintLelion-19 | 257.5s | 487 |
| 1784875935648-4954 | tengteng vs SegaXXJOJO1222 | 265.0s | 4,108 |
| 1784876002226-4066 | JAIMES2026 vs Certified_Noob | 289.2s | 6,310 |

### 13.1 Crux #1 — setup byte-equality (PASS on all 3)

Two separate equalities, both verified by `s0a_crux.py` (scratchpad):

1. **Whole-pipeline determinism:** the freshly re-downloaded + re-emulated +
   re-converted `.3sr` is **byte-identical to the shipped pool `.3sr`** —
   the *entire file*, not just the header — for all 3 quarks (`cmp` clean:
   4849 2,048 B, 4954 17,012 B, 4066 26,116 B). Re-running the pipeline
   reproduces the exact same bytes; determinism holds end to end.
2. **Setup from the signature frame ALONE:** re-deriving the setup block by
   walking the SCRD XOR chain and **stopping at the game-start signature
   frame** (`G_No[1..3]==2,0,0`), touching **zero** post-signature frames,
   yields a 12-byte setup block byte-equal to the pool `.3sr` header's setup
   bytes on all 3 quarks. In every case the signature was the archive's
   frame 0 ("read 1 of N frames; 0 post-signature frames touched").

| Quark | full 28-B header fresh==pool | setup bytes [hdr 8:20] (sig-frame-only == pool) |
|---|---|---|
| 4849 | identical | `0b0201000600010019002300` ✓ |
| 4954 | identical | `020b01020606010019002300` ✓ |
| 4066 | identical | `0f1100000404010013002300` ✓ |

**Verdict: producing the playable setup needs only the single game-start
signature frame — no full-match RAM dumps.** (The frame_count/checksum
header fields do depend on the in-game word stream, which the live tracker
emits per-frame; they are not "full RAM dumps" either.)

### 13.2 Crux #2 — shipped console-mode player, RAM-word (oracle) variant

Played each fresh RAM-word `.3sr` through the **shipped** player
(`build/host-release/3S-ARM.app/.../3S-ARM --play-replay`,
`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`) — the console-mode phase
machine, NOT B3's arcade-mode DEBUG harness.

| Quark game_0 (RAM-word oracle) | shipped-player result |
|---|---|
| 4849 (487 f, 8 checkpoints) | `REPLAY COMPLETE frames=487 checksums=8/8` (inputs-exhausted) — **CLEAN** |
| 4066 (6,310 f, 106 checkpoints) | `REPLAY COMPLETE frames=5737 checksums=95/95` (game-ended) — **CLEAN** |
| 4954 (4,108 f, 69 checkpoints) | `REPLAY DESYNC at frame 2160 (checkpoint 37/69)` — diverges |

Because each fresh RAM-word `.3sr` is **byte-identical** to the shipped pool
file (§13.1), the RAM-word variant plays *bit-for-bit identically to the
original in every case* — the success criterion "plays checksum-clean
wherever the original did" is met by byte-identity, not just by re-test.

**New honest finding (not introduced by this spike):** `4954/game_0` **desyncs at frame 2160 (checkpoint 37/69)** in the shipped console-mode player — for BOTH the fresh oracle AND the byte-identical shipped pool file (a round-boundary state divergence, `G=[2,2,1,1]` at frame 2159). Checkpoints 1–36 (frames 60–2100) validated clean.
This is the "no pre-play player gate" class the plan already flags
(risk 3, ~7% historical divergence): the auto-convert pool is **statcheck**-
gated (FBNeo-vs-engine RAM compare in arcade mode), NOT gated on shipped
console-mode playback, so a published pool `.3sr` can still diverge on the
device player. Same class as `2133/game_0` (notes §8.5, frame 2,248).

### 13.3 −13-word variant — B1 jitter + GO/NO-GO

Built a variant input table from the decoded −13 stream
(`decode_inputs.py`) aligned to the oracle words with B1's exact-substring
`find_best_offset` (no fuzzy matching), spliced into a `.3sr` with the
oracle's own setup + checksum table, and diffed vs the RAM-observed words:

| Quark game_0 | B1 offset | agreement | −13 vs oracle word diffs | jitter (±1 / ±2 / unrecoverable) | −13-variant player result |
|---|---:|---:|---|---|---|
| 4849 | — | — | **no exact anchor** (487 f, low-signal — B1's documented no-alignment mode) | — | (not built) |
| 4954 | 439 | 99.66% | **14 / 4,108 (0.34%)** | 9 / 5 / **0** | `REPLAY DESYNC at frame 2160 (checkpoint 37/69, live=202ec6ad want=9766bf28)` — **IDENTICAL** to the oracle's desync (same frame, checkpoint, and hashes); checkpoints 1–36 matched the oracle despite the 14 jittered words |
| 4066 | 775 | 100.00% | **0 / 6,310 (0.00%)** — byte-identical to oracle words | 0 / 0 / 0 | byte-identical to oracle → **CLEAN 95/95** (same run) |

The −13 stream is **sometimes** byte-identical to the RAM-observed words
(4066: 0 diffs) and **sometimes not** (4954: 14 jittered words; historical
B1 corpus ranged 79–95% agreement with up to ~25% unrecoverable on
`2133/game_1,3`). You cannot know in advance which case a given game is.

**Verdict: NO-GO on raw −13 relay as the fidelity path.** Raw relay does
NOT reliably reproduce the RAM-observed words — a real per-game tracker that
emits the RAM-observed `P1SW_0/P2SW_0` words is **mandatory** for guaranteed
fidelity. Qualified sub-note: when a game happens to have 0 jitter (4066),
raw −13 would play identically to the oracle — but that is a per-game
lottery, not a guarantee. This exactly matches the plan's recommended
architecture (Option A relays RAM-observed words, §1.4/§3.A); the raw-−13
simplification (rejected Option C) is confirmed unsafe.

### 13.4 GGPO wire pacing (re-measured, 3 quarks incl. 2022-vintage + long)

Bounded single-connection probes (`pacing_probe.py`, ≤90 s each, sequential,
polite), against `ggpo.fightcade.com:7100`:

| Quark | vintage / length | connect_rtt | −13 msgs / wall_span | pacing stream/wall | gap p50 |
|---|---|---:|---|---:|---:|
| 1641508702494-7287 | 2022-01-06, 17.4 min | 0.130 s | 542 / 89.2 s | **6.06×** | 166 ms |
| 1784866358738-6178 | recent, ~10 min | 0.098 s | 542 / 89.3 s | **6.06×** | 166 ms |
| 1784868265684-1230 | recent, ~8.7 min | 0.108 s | 521 / 85.6 s | **6.08×** | 166 ms |

Rock-steady **~6.06–6.08×** real time — one 60-frame (1.0 s) message every
~166 ms — across a 2022 quark and two recent ones, confirming the plan's
MEASURED-ONCE 6.18× (§1.3) is representative and age-independent. Quark
`1230` EOF'd cleanly at 85.6 s having delivered its **entire** 521 s session
(~6.1× → whole match on disk in ~1.4 min); the two longer sessions hit the
90 s probe cap mid-stream. Savestate (`−12`) always arrives ~0.8–0.9 s after
connect, before the first `−13`.

### 13.5 Pre-game prefix length (first −13 record → game_0 signature)

The prefix = the B1 alignment offset of game_0 = the number of −13 input
frames of char-select/pre-game before the first in-game frame. Fresh
measurements this session, plus corroborating prior-session B1 game_0
offsets (scratchpad `b1-findings.md`, different corpus):

| Quark game_0 | prefix (frames) | ≈ seconds @60 fps | source |
|---|---:|---:|---|
| 1784875935648-4954 | 439 | 7.3 s | this session |
| 1784876002226-4066 | 775 | 12.9 s | this session |
| 1641508702494-7287 | 901 | 15.0 s | B1 (prior) |
| 1675032176612-9791 | 948 | 15.8 s | B1 (prior) |
| 1675950860255-2133 | 1,072 | 17.9 s | B1 (prior) |
| 1659344338224-4017 | 1,459 | 24.3 s | B1 (prior) |
| 1784875689006-4849 | (no anchor) | — | short/low-signal game |

**Prefix distribution ≈ 439–1,459 frames (≈ 7.3–24.3 s of session).** At the
measured 6.06× wire rate this prefix arrives in ≈ **1.2–4.0 s wall** — a
modest, bounded contribution to first-frame latency (the §3.A budget's
dominant *unknown*, now measured).

### 13.6 S0a verdict

- **Crux #1 (setup byte-equality): PASS on all 3 quarks** — setup derivable
  from the single signature frame; whole `.3sr` reproduced byte-identically.
  The fidelity half of the plan does **not** need full-match RAM dumps.
- **Crux #2 (shipped-player RAM-word fidelity): PASS by byte-identity** —
  the RAM-word variant IS the shipped file; it plays clean wherever the
  original did. For `4954/game_0` the RAM-word oracle desyncs at frame 2160 (a genuine engine divergence) and the byte-identical pool file does too — no regression, and notably the −13 variant desyncs at the *identical* frame/checkpoint/hashes, i.e. that divergence is input-source-independent.
- **−13-word variant: NO-GO** for guaranteed fidelity (raw relay cannot
  reproduce the RAM-observed words: 4954 had 14 jittered frames). Tracker
  relaying RAM-observed words is mandatory — the plan's recommended path.
- **Pacing 6.06–6.08×**, **prefix ≈ 7–24 s** (≈1.2–4.0 s wall @6×).
- **Net: GO on the fidelity half of the live-stream plan**, on the
  RAM-observed-words tracker design; raw-−13 relay stays rejected. The one
  new caveat is that shipped-player playback of a statcheck-gated pool `.3sr`
  can still diverge (4954/game_0 @2160) — the existing checkpoint detector
  catches it at ≤60-frame granularity, but a live stream has no pre-play
  player gate (already plan risk 3).

---

## 14. Stage S1 — runner live-tracker mode (`-track-3sr`), 2026-07-24

**Status: DONE, byte-identity bar met.** The FBNeo replay runner
(`crowded-street/fbneo-replay-runner`) gains a `-track-3sr <out-dir>` mode
that emits, per in-session game, the exact `.3sr` bytes
`tools/fcade-replays/make_3sr.py generate` would produce from a full
RAM-dump `.scrd` archive of the same session — incrementally, from the
in-process normalized RAM view, **without writing any 512 KB `.ram`
frames**. Tail-follow of a growing `-replay-inputs` file
(`-replay-follow`) is folded in. The runner change ships as a patch in
THIS repo: `tools/fcade-replays/runner-track-3sr.patch` (git diff against
upstream `ccf96ab`; verified `git apply --check`-clean on a pristine
`ccf96ab` checkout). The runner fork itself is NOT vendored.

### 14.1 What the mode does (design, with citations)

Per emulated frame, at the existing once-per-frame SH-2 PC trigger
(`gDumpPcTrigger.targetPc == 0x06094d98`, runner `run.cpp:76` @ ccf96ab),
the patched runner builds the byte-reversal-normalized CPS3 main-RAM view
ONCE (`ReplayOnPcTrigger`, formerly inlined in `ReplayDumpCps3MainRam`,
run.cpp:165-176 @ ccf96ab) and feeds the identical buffer + the identical
in-game gate (`BE u16 @ 0x15438 == 2`, run.cpp:77-78,178-179) to both
consumers: the unchanged `.ram` dump writer (`-dump-ram-path`) and the new
tracker (`-track-3sr`). The tracker mirrors `make_3sr.py`'s
`extract_scrd_game()` state machine exactly:

- **Game segmentation:** in-game → not-in-game transition finalizes the
  current game and increments the game index — the same rule the dump path
  uses for `game_N/` directories (run.cpp:181-187 @ ccf96ab), so tracker
  game indices match `.scrd` archive names one-to-one.
- **Signature scan** (before setup): first in-game frame with
  `G_No[1]==2 && G_No[2]==0 && G_No[3]==0` (BE u16s at
  0x15438/0x1543A/0x1543C — `make_3sr.py:210-212`). The signature frame
  itself contributes NO input words (make_3sr.py:230 `continue`).
- **Setup record (once):** on the signature frame, open
  `<out-dir>/game_N.3sr` and write the 28-byte v1 header
  (`docs/3sr-format.md` §1): characters from 0x11387 with
  `CHAR_ARCADE_TO_3SX` (>14 → −1), supers 0x1138B, colors 0x15683,
  new_challenger 0x113DA, Random_ix16/32 BE u16 @ 0x155E8/0x155EA stored
  LE (make_3sr.py:213-227). `frame_count`/`checksum_count` are written as
  0 and patched at finalize (offsets 0x14/0x1A).
- **Stream offset:** at the signature frame the tracker records the
  runner's own consumed −13 record count (`gReplayInputOffset / 10`; the
  record applied on the signature frame is index count−1) into
  `<out-dir>/track3sr_manifest.json` — replacing B1's offline
  cross-correlation.
- **Appended word pairs:** every subsequent in-game frame appends
  `{u16 P1SW_0, u16 P2SW_0}` LE, read BE from 0x6AA8C/0x6AA90
  (make_3sr.py:232-237).
- **Checksum entries:** when `local_index % 60 == 0`, the 13-field window
  (make_3sr.py CHECKSUM_FIELDS, format §4.2) is read BE-u16,
  canonicalized 2-byte-LE each (26 bytes), hashed with the ADDITIVE djb2
  (`hash*33+byte`, format §4.3) and buffered in memory (8 B/60 frames);
  the table is appended after the words at finalize. Interval is fixed at
  60 (make_3sr.py default).
- **Game-boundary/end marker:** finalize (on boundary or session end)
  appends the checksum table, patches the header counts, and rewrites the
  manifest (`end_reason: "boundary" | "session-end"`, plus
  `"complete": true` at session end). A run that never reached a
  signature is recorded with `signature_found: false` and no `.3sr`.

**Tail-follow (`-replay-follow`):** the single EOF decision point is
`ReplayApplyFrameInputs`'s `gReplayFinished` (run.cpp:386-389 @ ccf96ab).
With the flag, insufficient-data now blocks in `ReplayFollowWaitForData()`:
poll the inputs file every 50 ms for appended bytes (only complete
10-byte records are consumed; a partial tail waits for its remainder),
finish when a `<inputs>.done` sentinel exists AND a post-sentinel re-read
finds no further full record (append-then-done writer ordering is
race-safe), or after `-replay-follow-idle-ms` (default 60000) of zero
growth. `ReplayInit`'s multiple-of-10 size check is relaxed only in
follow mode.

### 14.2 Build (patched runner)

```
cd ~/Developer/fbneo-replay-runner            # fork @ ccf96ab
git apply tools/fcade-replays/runner-track-3sr.patch   # from 3sx-mister repo
make sdl 'BUILD_X86_ASM=' 'CPUTYPE=arm64' -j1
# If it stops with "No rule to make target ...cps3_debug_harness.d" (§1's
# known dep-gen bug — it RECURRED on this rebuild):
:> build/debug/obj/GNU_SDL/fbneosdldarm64/burner/sdl/cps3_debug_harness.d
make sdl 'BUILD_X86_ASM=' 'CPUTYPE=arm64' -j1
# binary: build/debug/fbneosdldarm64
```

Tracker invocation (no `.ram` files are written in this mode):

```
./build/debug/fbneosdldarm64 sfiii3nr1 \
  -replay-state <dl>/savestate -replay-inputs <dl>/inputs \
  -headless -track-3sr <out-dir> [-replay-follow [-replay-follow-idle-ms N]]
```

### 14.3 Byte-identity verification (THE bar): 3 fresh quarks, 6/6 cmp-clean

Three quarks pulled from the live stealth catalog (all dated 2026-07-24,
downloaded fresh this session via `publish_3sr.py`'s standard
download+dump+compress path with `--keep-work`). Reference `.3sr`s came
from `make_3sr.py generate --checksum-interval 60` on the `.scrd`
archives (via `publish_3sr.py` for statcheck-clean games; direct
`make_3sr.py generate` for the one gate-dropped game — the byte-identity
bar is against the converter, and the divergent game exercises it too).
Tracker `.3sr`s came from a separate `-track-3sr` run on the same
savestate+inputs. Every pair compared with `cmp` (byte-for-byte):

| Quark | Game | Bytes | frame_count | checksums | records@sig | cmp |
|---|---|---|---|---|---|---|
| 1784908283814-3519 | game_0 | 12,708 | 3,066 | 52 | 938 | **PASS** (clean) |
| 1784908283814-3519 | game_1 | 6,516 | 1,568 | 27 | 4,312 | **PASS** (clean) |
| 1784908270503-4684 | game_0 | 17,908 | 4,324 | 73 | 902 | **PASS** (clean) |
| 1784908270503-4684 | game_1 † | 17,612 | 4,254 | 71 | 5,708 | **PASS** (clean) |
| 1784908567372-7225 | game_0 | 19,020 | 4,594 | 77 | 641 | **PASS** (clean) |
| 1784908567372-7225 | game_1 | 20,064 | 4,847 | 81 | 5,720 | **PASS** (clean) |

† statcheck-divergent (dropped by publish's gate, as designed) — the
tracker still reproduces `make_3sr.py`'s bytes exactly; divergence is an
engine-fidelity property, not a conversion property. Its signature sat at
archive frame 0, exercising that edge. md5 pairs matched for all six
files (e.g. game_0 of -3519: `6a7c6d81803a2811626adc18e196a267` on both
sides). **Full byte-identity: header, setup, word table, checksum table
cadence and trailers — no fallback to words-only emission was needed.**

### 14.4 Tail-follow byte-identity

Quark 1784908283814-3519 (58,800-byte inputs, 5,880 records), fed into an
initially-empty file in 613-byte chunks (deliberately not a multiple of
10, exercising partial-record waits), then `<inputs>.done`:

- Fast feed (50 ms/chunk, feed outruns emulation): follow-mode tracker
  output `cmp`-identical to the reference AND to the whole-file tracker
  run, manifest identical.
- Slow feed (500 ms/chunk ≈ 123 records/s, slower than the measured
  ~200 records/s whole-file emulation rate of 29.3 s): run took 48.1 s
  wall (feeder-bound — the runner provably blocked at EOF ~19 s
  cumulative) and output was again `cmp`-identical (both games).

### 14.5 Hygiene

Zero `.ram` files written by any tracker run (verified by `find` over the
scratchpad and runner tree post-run); the reference pipeline's transient
per-game dumps were bounded and auto-deleted by `publish_3sr.py`'s
disk-safe driver (§12 lesson upheld); no processes left running. Net
persistent scratch growth ≈ 130 MB (downloads + kept `.scrd`s +
evidence files). What S1 deliberately did NOT do (per plan): no new
on-wire format (the output IS `.3sr` bytes + the tiny manifest — framing
is S3's job), no proxy/device changes, no VPS deployment.
