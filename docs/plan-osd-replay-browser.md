# Plan: Native MiSTer OSD Replay Browser

**Status: PLAN — not started. Written 2026-07-23 against branch
`feat/fcade-replay-browser` @ `123e9d37`.**

This plan supersedes the *browsing* half of the in-game replay browser
(`src/replay/replay_browser*.c`, landed F2a/F2b/F3) with a **native
MiSTer FPGA-OSD menu** in the HPS wrapper (`MiSTer_3S-ARM`). The in-game
replay **player** (`src/replay/replay_player.c`) and its viewer overlay
(`src/replay/replay_overlay.c`) stay; playback is still engine-native.

Every load-bearing claim is cited `file:line`. Facts were re-verified on
2026-07-23 by reading the cited file/line on this branch. Where a fact
could not be verified from code it is flagged **UNVERIFIED**. No pushes
anywhere in this plan (user rule).

---

## 0. Why OSD, and what "solid UX" means here

The current in-game browser draws its list with `SSPutStrProP` on the
**game canvas** at priority 1 while the attract/demo loop keeps running
and fires full-screen `Switch_Screen`→`WipeOut` wipes at `PrioBase[0]`
(frontmost) — verified draw-ordering convention at
`src/sf33rd/Source/Game/rendering/mtrans.c:1526-1536` (lower index = drawn
in front) and `src/netplay/direct_p2p_overlay.c:34-39`. That is the root
of the flicker/blend jank. The FPGA OSD is a **separate hardware overlay**
composited by the scaler over core video
(`build/mister-wrapper-hps/src/osd.cpp` grid is 16 rows × 32 cols; SPI
`OSD_CMD_*`) — it structurally cannot blend or flicker with the game
canvas. Building the browser as OSD menu pages removes the jank *by
construction*.

The wrapper already proves every mechanic the OSD browser needs:

- **Row format + pad-nav template**: the Direct-P2P "Recently Joined"
  submenu `MENU_DIRECT_P2P_JOIN_RECENT`
  (`tools/mister-wrapper/main-mister-full-menu.patch:500-576`) does
  `sprintf(" %-11s  %-10s", code, ago)` + `MenuWrite(n, s, sel==i, 0)` +
  up/down/select over a runtime-loaded array. **NOTE:** this is only the
  row-format + navigation template — it is capped at `DP2P_RECENT_MAX`
  10 (`main-mister-full-menu.patch:63`) and its render body
  (`:526-536`) is a SINGLE `for` pass with NO `firstmenu`/`adjvisible`
  paging, so it never needs to scroll (≤10 rows in a 16-row OSD). A real
  replay catalog (>~14 rows) REQUIRES the paging wrapper below; cloning
  JOIN_RECENT verbatim would let `MenuWrite` silently drop rows past
  `OsdGetSize()` (`menu.cpp:812-816`) and hide an off-screen cursor.
- **Paging engine** `MenuWrite()`
  (`build/mister-wrapper-hps/src/menu.cpp:802-820`): `row = n - firstmenu`;
  off-screen selected rows set `adjvisible`; the caller's `while(1)` loop
  does `if (!menusub) firstmenu = 0; adjvisible = 0; …MenuWrite…;
  if (!adjvisible) break; firstmenu += adjvisible;` to re-render scrolled.
  Cite the loop skeleton at `menu.cpp:2298-2299` and a full working
  submenu instance at `menu.cpp:3227-3306` (video-proc) or `:3551-3606`
  (arcade-DIP). This handles arbitrary-length lists — exactly what a
  replay catalog needs. **This** is the template to clone for the LOCAL/
  REMOTE lists, NOT JOIN_RECENT's single-pass body.
- **On-screen keyboard** (6×7 grid) for text entry: the Direct-P2P join
  OSK, `main-mister-full-menu.patch:66-155` (cells) and `:431-498` (the
  `MENU_DIRECT_P2P_JOIN_ENTRY`/`ENTRY1` render + nav). Reusable for the
  player-name search box.
- **New menu pages** are `case MENU_X:` arms added by the patch; the enum
  is extended at `main-mister-full-menu.patch:13-27`.
- **Launch handoff**: `direct_p2p_handoff_join(code)`
  (`vendor/Main_MiSTer/thirdsarm_wrapper.cpp:2333-2339` arm+restart;
  argv injection at `:2829-2842`) is the payload-carrying template;
  `replay_browser_handoff()` (`thirdsarm_wrapper.cpp:2559-2561`) is the
  no-payload template.

The wrapper links `-lpthread` (`tools/mister-wrapper/Makefile.full.3s-arm:53`)
and does TCP sockets in `menu.cpp`/`user_io.cpp`, but **does not link
SDL**. So the device replay logic (`proxy_client.c`,
`replay_browser_scan.c`) must be **de-SDL'd** to run in the wrapper. The
SDL surface is thin (see §3).

---

## 1. Verified fact base

### 1.1 Branch state — what already exists

`feat/fcade-replay-browser` @ `123e9d37` already landed (verified
`git log --oneline`):

- E1a native Fightcade stream client (`src/replay/fcade_stream.c`,
  commit `8ea3eeaa`).
- C1/C2 runtime `.3sr` player + viewer overlay (`replay_player.c`,
  `replay_overlay.c`, `eb0cd3a9`).
- D2 ARM statcheck parity 15/15 (`257a5aea`).
- F2a in-game LOCAL browser (`replay_browser.c`, `cf652429`).
- F1 fcade-proxy search service (`fa0357f4`).
- F2b remote browse + on-device fetch (`f3c33fd1`).
- F3 remote search sub-tabs/player search (`ad6915a0`).
- F4 **T[31] Replay Browser OSD trigger** (`adefd0ad`) — menu.sv line +
  HPS wrapper `replay_browser_handoff()` that relaunches with
  `--replay-browser` (the in-game browser).
- Storage lifecycle LRU eviction + delete (`replay_storage.c`,
  `8dc14d84`).
- fcade-proxy offline catalog mode + auto-refresh (`fda47117`,
  `123e9d37`).

**Consequence for this plan:** the *device engine* side (player,
overlay, stream, statcheck, scan/proxy_client as SDL modules) is done.
What is NEW: moving BROWSING into the OSD (de-SDL ports of scan +
proxy_client into the wrapper, new `MENU_REPLAY_*` pages), a working
`get3sr` conversion→serve→fetch chain, the during-replay name overlay
repositioning, and the OSD menu reorder.

### 1.2 The OSD trigger today, and how it must change

`menu.sv` already carries the line (verified
`vendor/Menu_MiSTer/menu.sv:291`): `"T[31],Replay Browser;"`. The patch
intercepts it (`main-mister-full-menu.patch:188-198`): today it calls
`replay_browser_handoff()` and closes the OSD, i.e. relaunches the game
into the **in-game** browser via the `--replay-browser` flag
(`thirdsarm_wrapper.cpp:2839-2842`).

**This plan re-points that intercept** at a new OSD menu page family
`MENU_REPLAY_ROOT` (like `bit==29` → `MENU_DIRECT_P2P_ROOT`,
`main-mister-full-menu.patch:184-187`). The browsing then happens *in the
OSD*; selecting a replay hands off `--play-replay <abs-path>` (existing
flag, see §1.4) so the game boots straight into `ReplayPlayer_Init`.

### 1.3 The wrapper build + how to add code

- `tools/mister-wrapper/build-hps.sh`: clones pinned Main_MiSTer
  @ `3380931329b8` (`:18`, `:89-90`), rsyncs the overlay keep-list
  `main-mister-overlay.files` over it (`:91`), applies
  `main-mister-full-menu.patch` (`:92`), copies `Makefile.full.3s-arm`
  as `Makefile.3s-arm` (`:93`), then `make` (local ARM gcc or the
  `3s-mister-arm-wrapper-hps` Docker image, `:175-185`). Output
  `build/mister-wrapper-hps/MiSTer_3S-ARM` (`:187`). Minutes, NOT Quartus.
- **New wrapper `.c` files are auto-compiled**: `Makefile.full.3s-arm:31`
  is `C_SRC = $(wildcard *.c) ...` (glob of the build-src root) and
  `:40` `CPP_SRC = $(filter-out main.cpp,$(wildcard *.cpp))`. To land a
  new file in the build it must be (a) placed under
  `vendor/Main_MiSTer/` and (b) listed in
  `tools/mister-wrapper/main-mister-overlay.files` (the rsync files-from
  manifest, currently `fpga_io.cpp … video.h`) so it is copied into the
  build-src root. `menu.cpp` itself is NOT in the manifest — it comes
  from the upstream clone and is modified ONLY through the patch.
- `cJSON` is standalone C (`third_party/cJSON/cJSON.{c,h}`). To use it in
  the wrapper, copy `cJSON.c`/`cJSON.h` into `vendor/Main_MiSTer/` and
  add both to `main-mister-overlay.files` (so `$(wildcard *.c)` picks up
  `cJSON.c`). **UNVERIFIED**: whether cJSON compiles clean under the
  wrapper's `-Wall -Wextra` set (`Makefile.full.3s-arm:52`); cJSON is
  warning-clean upstream but confirm at build time.

### 1.4 Booting straight into a replay — the `--play-replay` flag EXISTS

Verified: `configuration.replay.play_replay_path` is a real config field,
validated in `src/args.c:165-200` (rejected under STATCHECK `:165-169`,
rejected with `--test-enable` `:175-177`, rejected with netplay flags
`:197-200`). `main.c` acts on it: `--fetch-replay` takes priority and
ignores it (`main.c:1121-1124`); otherwise `if
(configuration.replay.play_replay_path != NULL)` at `main.c:1155` loads
the `.3sr`. `ReplayPlayer_Init(path)` is the loader
(`src/replay/replay_player.h:36`), called once from `main()` after
`read_args`. **So the OSD launch needs NO new game-side flag** — inject
the existing `--play-replay <abs-path>`. `ReplayPlayer_PinConfig()`
(arcade-balance=on + console mode + default mapping) is applied for the
session in `initialize_game()` when active (`replay_player.h:58-63`).

### 1.5 Handoff/argv injection — the payload template

`thirdsarm_wrapper.cpp`:
- `g_replay_browser_armed` (`:59`), `replay_browser_handoff()`
  (`:2559-2561`) arms + requests restart (no payload).
- Payload template: `direct_p2p_handoff_join(code)` writes an intent file
  and arms; `g_dp2p_code_buf`-style state carries the variable string.
- argv injection block (`:2819-2843`): `child_argv` is built from
  `argv[2..]` then conditional flags appended before the `nullptr`;
  parent clears the armed flags post-fork (`:2862-2866`). A new
  `--play-replay <path>` injection clones the `if (g_replay_browser_armed)`
  block (`:2839-2842`) but pushes TWO argv entries (flag + stored path).
- `kRuntimeHome = "/media/fat/games/3s-arm"` (`:64`); the wrapper already
  reads that config file for the `[30]` seed (`:1867`, `:400`+), so it can
  read `replay-proxy-host`/`replay-proxy-port` the same way.
- Exports live in `thirdsarm_wrapper.h` `extern "C"` block (`:19-45`).

### 1.6 The conversion + serve + fetch chain — the gap

From the proxy/tooling investigation (all cited):

- **Proxy protocol** (`tools/fcade-proxy/fcade-proxy.js`): `u32be`
  length prefix + UTF-8 JSON, no magic, no handshake (`:104-110` encode,
  `:668-679` decode; README `:20-45`). This is byte-identical to the
  device stream client's `recv_frame`/`get_u32be`.
- **Op dispatch** `dispatch()` (`:625-636`): only `search` (`:629-631`)
  and `status` (`:632-634`); unknown → `bad_request` (`:635`).
- **`get3sr` is NOT implemented** — README documents the absence at
  `README.md:577-590` ("Not implemented in this build"); the token
  `get3sr` appears nowhere in `fcade-proxy.js`. There is no stub, just
  the `bad_request` default.
- **Catalog** is a static JSON file (`FCADE_CATALOG_FILE`, deployed at
  `/opt/fcade-proxy/catalog.json`, `fcade-proxy.service:12`), re-read on
  mtime change (`loadCatalog()` `:180-224`). Rows carry player names
  (`normalizeRow`/`normalizePlayer` `:138-162`: `name`, `country`,
  `rank`, `score`). The "~295 rows" figure is data-dependent —
  **UNVERIFIED** from code (no committed catalog).
- **Push rails**: `push-catalog.sh` rsyncs ONE file → renamed to
  `catalog.json` (`:11`); `refresh-catalog.sh` watches Downloads →
  validates → `push-catalog.sh` → SSH `status` verify (target alias
  `hetzner-3s-arm` → `46.62.244.55` via `~/.ssh/config`). **Neither can
  ship `.3sr` blobs** — a `.3sr` would be renamed `catalog.json` and
  rejected. `deploy.sh` ships code only (`--delete`, excludes cookie +
  catalog). **A NEW `.3sr` publish rail is required.**
- **`make_3sr.py`** subcommands `generate`/`verify` (`main()` `:632-653`).
  `build_meta` (`:450-496`) writes real `players[]` ONLY from
  `--quark-json` (`:463`); the `--summary-json` (`:482`) and minimal
  (`:491`) branches hard-code `players: []`. Meta fields: `quarkid`,
  `players`, `date` (ms epoch), `duration`, `game_index`, `source`.
  **Characters/colors/supers/RNG are NOT in the meta JSON — they live in
  the binary `.3sr` header** (`encode_3sr`/`parse_3sr` `:331-348`). The
  name overlay reads only `players[].name` from the sidecar
  (`replay_player.c:348-351`), so **the meta MUST carry real names** →
  the pipeline must supply `--quark-json` with a populated `players`
  array. This is the CRITICAL DATA FIX.
- **One quark → N `.3sr`**: fan-out is in `replay_preprocessor.py`
  (one `game_N.scrd` per in-game segment, `:112-123`); `make_3sr.py
  generate` runs once per `game_N.scrd` and emits one `.3sr` +
  `<out>.meta.json`. The N-loop is the caller's, not inside make_3sr.
- **statcheck gate**: `statcheck_runner.py` runs the statcheck build
  `--ram-archive <game_N.scrd> --headless` (`:155`), passes a game iff
  exit code 0 (`:178-181`), exits 0 iff ALL games pass (`:199`). It
  reports pass/fail but **does not publish** — a batch wrapper must gate
  publication on the per-game signal. "Publish only statcheck-clean" is
  not yet a script.

### 1.7 Device de-SDL surface (sizing the wrapper port)

SDL symbol census (verified `grep -oE 'SDL_[A-Za-z]+'`):

- `src/replay/proxy_client.c`: `SDL_strlcpy`, `SDL_SetAtomicInt`/
  `GetAtomicInt` (→ `_Atomic int` / mutex), `SDL_free`/`malloc`,
  `SDL_strcmp`/`strlen`, `SDL_snprintf`, `SDL_CreateThread`/`WaitThread`/
  `Thread` (→ pthread), `SDL_zero`/`zerop` (→ `memset`),
  `SDL_LogError`/`GetError` (→ `fprintf`). All mechanical.
- `src/replay/replay_browser_scan.c`: `SDL_snprintf`, `SDL_strlcpy`,
  `SDL_free`, `SDL_strcmp`, `SDL_PathInfo`/`GetPathInfo`/`PATHTYPE`
  (→ `stat`/`lstat`), `SDL_GlobDirectory` (→ `scandir`/`glob`),
  `SDL_LoadFile` (→ `fopen`/`fread`), `SDL_qsort` (→ `qsort`),
  `SDL_memcpy`. All mechanical.
- `src/replay/fcade_stream.c`: `SDL_Log`, `SDL_Delay` only — but this is
  the raw-stream/FBNeo path and is **NOT needed in the wrapper** (the
  wrapper's remote path fetches ready `.3sr` via `get3sr`, no stream
  decode). Leave `fcade_stream.c` game-side.

The public API shapes to preserve while porting (verified
`src/replay/proxy_client.h`): `ProxySearchParams` (host/port/gameid/
offset/limit/best/since/username), `ProxyRow` (quarkid/date_ms/
duration_secs/p1/p2/emulator/gameid/ranked/num_matches), the async
`SearchAsync`/`PollAsync`/`TakeResults` worker pattern
(`proxy_client.h:95-117`). These become de-SDL'd wrapper C.

### 1.8 The menu reorder (requirement D) — bit map

Current CONF_STR order (verified `vendor/Menu_MiSTer/menu.sv:278-313`):

| menu.sv line | entry | bit(s) |
|---|---|---|
| 284 | Arcade Balance | `O[30]` |
| 285 | Play Online | `T[29]` |
| 291 | Replay Browser | `T[31]` |
| 292 | `-;` | — |
| 293 | Game Mode | `O[13]` |
| 294 | Hold to Pause | `O[24]` |
| 295 | FPS Counter | `O[11:10]` |
| 296 | Button Check | `T[23]` |
| 297 | `-;` | — |
| 298 | Aspect Ratio | `O[12]` |
| 299 | Vertical Crop | `O[32]` |
| 300 | Crop Offset | `O[36:33]` |
| 301 | Scale | `O[38:37]` |
| 302 | H Size | `O[42:39]` |
| 303 | H Position | `O[28:25]` |
| 304 | V Position | `O[46:43]` |
| 306 | Overclock | `O[20:19]` |
| 308 | Reset to Default | `T[21]` |
| 309 | Restart | `T[22]` |

**Target visible order (requirement D), DISPLAY-only, bits unchanged:**

```
T[29]  Play Online
T[31]  Replay Browser
T[23]  Button Check
-;
O[30]  Arcade Balance
O[13]  Game Mode
O[24]  Hold to Pause
-;
O[11:10] FPS Counter        <- "the rest, unchanged" starts here
O[12]  Aspect Ratio
O[32]  Vertical Crop
... (Crop Offset … Restart unchanged)
```

Every bit index is preserved — only the ordering of the CONF_STR string
literals changes. This is safe with the stale-CFG defenses because:

- **Bit 30 (Arcade Balance O[30])**: the `db58a5f6` defense is the
  wrapper re-seeding `[30]` from the game config AFTER CFG load
  (`thirdsarm_wrapper.cpp:1982`, read at `:1867`; menu.sv comment
  `:280-283`). Reordering the DISPLAY does not touch the bit, so the
  seed defense still fires. **Do not disturb `menu.sv:280-283` or the
  wrapper seed ordering.**
- **Bit 31 (Replay Browser T[31])**: intercept model
  (`main-mister-full-menu.patch:188`); the bit value is never read by
  RTL or wrapper, so stale-CFG taint is harmless (menu.sv comment
  `:286-290`). **Do not disturb `:286-290`.**
- **Button Check T[23]** and **Play Online T[29]** are also intercept-model
  triggers (`main-mister-full-menu.patch:180-187`); they key on the bit
  NUMBER, not on menu position, so moving their display lines needs no
  patch change.

**Quartus implication**: CONF_STR is a compile-time `localparam` stored
in BRAM via `hps_io #(.CONF_STR(CONF_STR), .CONF_STR_BRAM(1))`
(`menu.sv:319`), so ANY reorder ⇒ an RBF rebuild in the Colima
`quartus2` VM (~40-60 min, isolated long-pole; see S5). This is the ONLY
Quartus step in the plan.

### 1.9 During-replay names — HUD feasibility (requirement C)

Battle HUD draw sites (verified):

- Health bars: `vital_parts_allwrite(Pl_Num)`
  (`src/sf33rd/Source/Game/engine/vital.c:88`) → `vital_put`
  (`src/sf33rd/Source/Game/ui/sc_sub.c:1049`). P1 fill x`8..168`
  y`16..24`; P2 fill x`216..376` y`16..24`
  (`sc_sub.c:1079-1088`, backing `:1136-1145`).
- Stun bar directly under: y`24..32` (`sc_sub.c:1221-1222`).
- Face portraits: `player_face()` (`sc_sub.c:1691`),
  `scfont_sqput_face` (`:1704`) — y`24..48`; backing `face_base_put`
  (`:1759`) y`25..45`.
- Character-name plate (sprite, NOT a handle): `player_name()`
  (`sc_sub.c:1481`, `scfont_sqput` `:1498-1499`) — P1 x`48..88`,
  P2 x`296..336`, y`24..32`.
- SA/super-art gauge at bottom: y`210..217` (`sc_sub.c:1183-1184`).

**No player-handle text is drawn on the battle HUD in the base engine.**
The only free-text battle overlays are the Direct-P2P connection overlay
(`direct_p2p_overlay.c:83`, centered y70/100/120) and the replay overlay.

Text primitive: `SSPutStrProP(u16 flag, u16 x, u16 y, u8 atr, u32 vtxcol,
const char* str, u16 priority)` (`sc_sub.c:598`). `flag != 0` centers the
string within `[0, x]` (`:614-615`); `x,y` are raw pixels (no ×8);
priority is a `PrioBase` index (lower = frontmost). Width helper
`SSGetDrawSizePro` (`sc_sub.c:659`).

Names are ALREADY in memory: `ReplayPlayer_GetP1Name()` /
`GetP2Name()` return `meta_p1_name[64]`/`meta_p2_name[64]`
(`replay_player.c:651-656`, parsed + sanitized `:348-351`). The overlay
hook `ReplayOverlay_Draw()` (`replay_overlay.c:105`) is called once per
frame from `main.c:712`, guarded on `ppgScrList.tex == NULL`
(`replay_overlay.c:121`) to avoid a boot-order segfault. Current bottom
label: `draw_status_line()` (`replay_overlay.c:39`) →
`SSPutStrProP(1, 384, 202, 9, 0xFFFFFFFF, line, 1)` (`:64`) drawing
`"REPLAY  <p1> vs <p2>  <date>"` (`RPL_OVL_STATUS_Y = 202`, `:29`).

**Feasibility verdict (requirement C):** The user's IDEAL — a name label
directly under each health bar — is only **partially** feasible. The
strip immediately under the bar (y`24..48`) is OCCUPIED by the stun bar
(y24-32), face portraits (y24-48), and character-name plates (y24-32).
Dropping a ~12-char handle there collides with those elements.

- **Recommended (achievable) health-bar placement**: one row at **y=48**,
  just below the whole top-HUD cluster (portraits/backing end ≈y45-48).
  P1 label left-aligned to the bar's left edge:
  `SSPutStrProP(0, 8, 48, 9, 0xFFFFFFFF, p1name, 1)`. P2 label
  right-aligned to the bar's right edge (flag=1 can't right-anchor, so
  use flag=0 + width): `SSPutStrProP(0, 376 - SSGetDrawSizePro(p2name),
  48, 9, 0xFFFFFFFF, p2name, 1)`. Priority 1 keeps it in front of the HUD
  (index 2) and behind wipes (index 0). Only rarely overlaps a
  high-jumping sprite.
- **Fallback (lowest risk)**: keep the current bottom line but ensure it
  reads `"<p1> vs <p2>"` (the y=202 `draw_status_line`, already wired to
  the name getters). This is the guaranteed-safe option.

S4 implements the y=48 placement behind a small guard and keeps the y=202
line as fallback; if on-TV testing shows collisions the y=48 draw is
dropped and the bottom line remains.

### 1.10 Junk local replays to delete (requirement E)

The 12 "local replays" are QA debris produced by `make_3sr` WITHOUT
`--quark-json` (so `players:[]` and quarkid filenames — the unreadable
naming). Present in the package tree (verified `find build`): e.g.
`build/mister-telemetry-package/replays/sfiii3nr1-1641508702494-7287.7_game_0.3sr`
and siblings (`…_game_N.3sr` + `.meta.json`). Device copies live in
`/media/fat/games/3s-arm/replays/` (config default
`DEFAULT_REPLAY_BROWSER_ROOT`, `src/port/config/config.c:64`). LOCAL
should mean *saved favorites*, not a dump.

---

## 2. Key decisions

1. **LOCAL-first for the OSD (S2 before S3a/S3b), REMOTE value delivered by
   parallel S1.** The novel, highest-risk piece is the OSD menu mechanics
   (de-SDL scan, paging, launch handoff). Proving that end-to-end with a
   LOCAL list needs zero network and no proxy work — it gets a solid,
   scrollable, launchable list on the TV early and de-risks everything.
   The actual *point* (remote download that just plays) is delivered by
   S1 (conversion + `get3sr` + device fetch), which is Mac-/proxy-side
   and **fully parallel** to S2 and to the Quartus long-pole S5. So the
   dependency graph front-loads value without serializing on the risky
   OSD work: `S0 → {S1 ∥ S2 ∥ S5} → S3a (needs S1+S2) → S3b → S4 → S6`.
2. **Reuse `--play-replay`, add no game-side flag.** The launch is
   argv-only (§1.4); the wrapper stores the picked absolute path and
   injects `--play-replay <path>`.
3. **`get3sr` returns, per game, the `.3sr` blob AND its `.meta.json`
   sidecar as base64 over the existing JSON framing** (design detail in
   S1) — no second binary protocol. The sidecar is mandatory: player
   names live ONLY there (`replay_player.c:348-351`), so a `.3sr`-only
   response would play nameless (breaks S4 and the "plays with names"
   criterion). A `.3sr` is ~29KB (`docs/3sr-format.md`); base64 ≈
   39KB per game plus a small meta blob, acceptable over one framed
   response. Multi-game (one
   quark → N) is surfaced as N rows OR an auto-pick of `game_0` with a
   "1/N" hint (S3b decides from row `num_matches`).
4. **Conversion stays Mac-side**; a NEW rail publishes statcheck-clean
   `.3sr` blobs to the VPS; `get3sr` serves them. VPS-side FBNeo is
   unverified (Linux build + ROM/legal) and out of scope.
5. **Menu reorder is display-only** (bits preserved) and isolated to the
   single Quartus build (S5).
6. **Retire the in-game browser for BROWSING** (S6): delete or hide
   `replay_browser*.c`; keep `replay_player.c` + the repositioned overlay.

---

## 3. Deliberately out of scope

- VPS-side FBNeo conversion (Linux build unverified; ROM/legal). All
  conversion is Mac-side batch.
- TLS on the device / on-device Cloudflare (`cf_clearance` is
  browser-derived; the proxy terminates it — unchanged from F1).
- The raw-stream + on-device FBNeo playback path (`fcade_stream.c`) as a
  browsing route — remote playback now goes through pre-made `.3sr`.
- Rebuilding the in-game replay PLAYER or the statcheck harness.
- Any release ZIP/SHA/publish (user rules: no premature release, no push).
- Scoped/dynamic per-launch config pinning (the session-wide
  `ReplayPlayer_PinConfig` stays as-is).

---

## 4. Stage map & dependencies

```
S0  delete junk local replays (device + package)         [off-machine + local]
S1  conversion pipeline + get3sr + device fetch          ∥  (Mac/proxy/device engine)
S2  OSD browser scaffolding — LOCAL list end-to-end      ∥  (wrapper HPS; de-SDL scan)
S5  menu reorder (Quartus RBF)                           ∥  (isolated long-pole; start early)
S3a de-SDL proxy port + get3sr client (wrapper, no UI)     (needs S1 + S2)
S3b OSD REMOTE pages: search UI + OSK + download-launch     (needs S3a + S2)
S4  during-replay names (health-bar y=48 or fallback)      (independent; needs 1.9 facts)
S6  retire in-game browser for browsing                    (after S2/S3b prove the OSD)
```

Each stage below is sized ≤ ~2h agent work and carries all eight
`/implement` fields. Build with `tools/mister/build-game.sh --flavor
telemetry` (runtime, AGENTS.md:19-21), `tools/mister-wrapper/build-hps.sh`
(wrapper), Quartus in the `quartus2` VM (RBF, S5 only). Deploy per
`docs/mister-runbook.md` via `tools/mister/misterctl.sh`; never edit
`build/` (AGENTS.md:15). No pushes.

---

## Stage S0 — Delete the junk local replays

**Title:** Purge QA-debris `.3sr` from the package tree and the device.

**Why:** LOCAL must mean saved favorites, not a dump of unnamed
(`players:[]`, quarkid-filename) QA outputs (§1.10). They also make the
new OSD LOCAL list look broken (unreadable rows) on first light.

**Read first:** §1.10; `AGENTS.md:5-10` (delete scope is ONLY inside
`/media/fat/games/3s-arm/`); memory `feedback-no-rsync-delete`;
`docs/mister-runbook.md` deploy section; `src/port/config/config.c:64`
(root path).

**Create/modify:**
- Local package tree: remove the debris `.3sr` + `.meta.json` from
  `build/mister-telemetry-package/replays/` (and any
  `build/mister-*-install/replays/`). Regeneration of the package will
  not re-add them once the source-of-debris (unqualified `make_3sr`
  runs) is retired. No tracked source changes — these are gitignored
  build artifacts.
- Device: over SSH, delete the debris files strictly under
  `/media/fat/games/3s-arm/replays/` (per-file `rm` of the known
  basenames — NOT `rm -rf` of the dir, NOT `rsync --delete`).

**Success criteria:**
- `find build -name '*.3sr'` shows the debris gone.
- SSH `ls /media/fat/games/3s-arm/replays/` shows only intended
  favorites (or empty). SSH-verifiable.
- No file outside `/media/fat/games/3s-arm/replays/` is touched.

**Dependencies:** none.

**What NOT to do:** no `rsync --delete`; no `rm -rf` of the replays dir
or any parent; nothing outside the owned subtree; do not delete a replay
the user has marked/kept (confirm the debris list against the user's
favorites first — if unsure, ask which to keep).

**Failure/fallback:** deletion is per-basename and reversible from the
package tree; if the device list is ambiguous, leave device files and
only clean the package tree, surfacing the ambiguity to the user.

---

## Stage S1 — Conversion pipeline + `get3sr` + device fetch (kill NEEDS CONVERSION)

**Title:** Mac batch that produces named, statcheck-clean `.3sr`; a new
proxy `get3sr` op; the device-side fetch that yields a PLAYABLE file.

**Why:** the remote value prop. Today a remote download is a raw stream
that shows "NEEDS CONVERSION" (`docs/fcade-replay-notes.md:1074,1092`).
This stage makes a remote pick download a ready `.3sr` that plays. It is
Mac/proxy/device-engine work, fully parallel to the OSD (S2) and Quartus
(S5).

**Read first:** §1.6; `tools/fcade-replays/make_3sr.py` (`build_meta`
`:450-496`, `cmd_generate` `:502-537`, `main` `:632-653`);
`tools/replay_preprocessor.py` (fan-out `:112-123`);
`tools/statcheck_runner.py` (gate `:155,178-199`);
`tools/fcade-proxy/fcade-proxy.js` (`dispatch` `:625-636`, `handleSearch`
`:550-608`, framing `:104-110`/`:668-679`, catalog `:180-224`);
`tools/fcade-proxy/README.md:577-590` (get3sr absence),
`push-catalog.sh`, `refresh-catalog.sh`, `deploy.sh`;
`src/replay/proxy_client.{c,h}`; `docs/3sr-format.md`;
`docs/fcade-replay-notes.md` §9 (D2 parity), §11 (wave-2), B3.

**Create/modify:**
1. **make_3sr data fix (the CRITICAL fix):** ensure each `.meta.json`
   carries real `players[]`. Options (pick the least-invasive that a
   batch driver can feed): (a) always pass `--quark-json` built from the
   catalog row (name each `players[i].name`), or (b) extend `build_meta`
   to accept a `--players "P1,P2"` shorthand when no quark JSON exists.
   Prefer (a) — the catalog row already has the names (§1.6). Add a
   header comment in `make_3sr.py` documenting that `players:[]` is a
   data bug the batch must not reproduce.
2. **Batch driver** `tools/fcade-replays/publish_3sr.py` (new): for a set
   of quarkids (from the catalog): download stream → `replay_preprocessor`
   (FBNeo runner @ `ccf96ab`, needs `sfiii3nr1.zip` + parent
   `sfiii3.zip`) → `game_N.scrd` → `statcheck_runner.py` GATE (only
   proceed for games that exit 0) → `make_3sr generate` per clean
   `game_N.scrd` WITH the catalog row's players → collect
   `<quarkid>/game_N.3sr` + `.meta.json`. Emit a small
   `published_manifest.json` (quarkid → [game_index…]) for the proxy.
   Drop the ~8% that diverge (statcheck-fail) — do not publish them.
3. **`.3sr` publish rail** `tools/fcade-proxy/push-3sr.sh` (new): rsync
   the per-quark `.3sr`/`.meta.json` tree to a VPS dir (e.g.
   `/opt/fcade-proxy/3sr/<quarkid>/game_N.3sr`) via the same
   `hetzner-3s-arm` SSH alias (no `--delete`; do NOT reuse
   `push-catalog.sh`, which renames to `catalog.json`). Document the
   layout in README.
4. **Proxy `get3sr` op** (`fcade-proxy.js`): add a `handleGet3sr(req)`
   and a `dispatch` branch (`:635` region). Request
   `{op:"get3sr", quarkid, game_index?}`. Response over the existing JSON
   framing:
   `{ok:true, quarkid, games:[{game_index, size, b64, meta_size, meta_b64}]}`
   where `b64` is base64 of the `.3sr` bytes read from
   `<3sr-root>/<quarkid>/game_N.3sr` AND `meta_b64` is base64 of the
   **sibling `.meta.json` bytes** read from
   `<3sr-root>/<quarkid>/game_N.meta.json` (the sidecar `push-3sr.sh`
   already ships to the VPS in step 3). The meta sidecar is the ONLY
   source of player names — the `.3sr` binary header has none
   (`replay_player.c:348-351` parses `players[].name` from the sidecar;
   getters `:651-656`), so the device MUST receive it or every downstream
   name overlay (S4) and the "plays with names" success criterion below
   are unreachable. Returning the raw `.meta.json` bytes preserves the
   canonical statcheck-clean names that `publish_3sr.py` wrote — do NOT
   have the device synthesize a sidecar from a search `ProxyRow` (a
   standalone `get3sr`-by-quarkid has no prior search row). If
   `game_index` given, return just that one; else return all games
   (manifest). The proxy MUST refuse (typed error) to serve a game whose
   sidecar is missing or whose `players[]` is empty, so a nameless replay
   never reaches the device. Typed errors for missing/oversize (mirror
   the existing typed-error set). Guard blob size (a full match ≈29KB
   `.3sr` + a small `.meta.json`; cap generously). Add README "Requests"
   entry, removing the "not implemented" note.
5. **Device fetch** `ProxyClient_Fetch3sr()` (new, in
   `src/replay/proxy_client.c`/`.h`, game-side — mirrors
   `ProxyClient_Search`): send `get3sr`, read framed JSON, then for each
   game base64-decode BOTH blobs and write BOTH sidecars —
   `b64` → `<replay-root>/<quarkid>/game_N.3sr` and
   `meta_b64` → `<replay-root>/<quarkid>/game_N.meta.json` — return the
   `.3sr` path(s). Writing the `.meta.json` is mandatory: without it
   `ReplayPlayer_Init` finds no sidecar and the name overlay is blank
   (`replay_player.c:341-352`). Async wrapper like
   `SearchAsync`/`TakeResults` (`proxy_client.h:95-117`) so the game
   never blocks. Reroute the in-game REMOTE SELECT to it (interim, until
   S3b moves browsing to OSD) so the round-trip is testable on desktop.

**Success criteria:**
- `publish_3sr.py` over 3-5 real quarks yields
  `<quarkid>/game_N.3sr` with `.meta.json` whose `players[]` has real
  names (verify `jq .players`); statcheck-fail games are absent.
- Proxy `get3sr` served locally (node) returns, per game, a `b64` that
  base64-decodes to bytes whose first 4 = the `.3sr` magic
  (`docs/3sr-format.md`) and that `make_3sr.py verify` accepts, AND a
  `meta_b64` that base64-decodes to valid JSON whose `players[]` has real
  names (`jq .players`).
- Desktop game: `ProxyClient_Fetch3sr` against the local proxy writes
  BOTH `game_N.3sr` and `game_N.meta.json`; `ReplayPlayer_Init` loads the
  `.3sr`, reads the sidecar, and plays with names visible in the overlay
  — i.e. **no NEEDS CONVERSION**. Desktop-testable.
- Runtime builds clean (`build-game.sh --flavor telemetry`; -Werror per
  the repo's promoted-warnings history).
- SSH-verifiable once pushed: proxy `status` still `mode:catalog`; a
  device fetch log line shows the written `.3sr` path.

**Dependencies:** S0 (clean slate). User inputs: `sfiii3nr1.zip` +
`sfiii3.zip` (ROMs), `FCADE_COOKIE` for catalog refresh (search is
already cookie-free via cached catalog). VPS deploy of the updated
`fcade-proxy.js` + the `.3sr` tree (off-machine; user-driven).

**What NOT to do:** do not attempt VPS-side FBNeo; do not commit ROMs or
replay blobs; do not reuse `push-catalog.sh` for `.3sr`; do not publish
statcheck-fail games; do not add a second binary wire protocol — base64
in the existing JSON frame.

**Failure/fallback:** if base64-in-JSON proves too heavy, fall back to a
length-framed binary sub-frame after a JSON header (still one connection,
same `u32be` framing). If the VPS `.3sr` tree is not yet deployed, the
device fetch degrades to `PROXY_ERR_UPSTREAM` and the OSD (S3b) shows
"try again later" — browsing/local playback unaffected.

---

## Stage S2 — OSD browser scaffolding: LOCAL list end-to-end

**Title:** `MENU_REPLAY_*` page family in the wrapper + de-SDL local scan
+ `--play-replay` launch handoff — a scrollable, launchable LOCAL list.

**Why:** proves the OSD browser mechanics (the novel risk) with zero
network. Delivers a solid, flicker-free list on the TV early
(decision §2.1). Everything remote (S3a/S3b) builds on this scaffolding.

**Read first:** §1.2, §1.3, §1.5, §1.7; the Direct-P2P submenu template
in `main-mister-full-menu.patch` (enum `:13-27`, intercept `:180-198`,
`MENU_DIRECT_P2P_ROOT`/`ROOT1` `:370-429`, `JOIN_RECENT` row-format +
pad-nav template `:500-576` (single-pass, capped at 10 — NOT a paging
proof; see §0), `CONFIRM` handoff `:578-593`); `menu.cpp:802-820`
(MenuWrite, drops rows past `OsdGetSize()` at `:812-816`), the paging
`while(1)` loop skeleton `:2298-2299` and a full working submenu
instance `:3227-3306` (video-proc) or `:3551-3606` (arcade-DIP) — clone
THESE for the scrollable catalog; `thirdsarm_wrapper.cpp:2333-2339` (arm),
`:2819-2843` (argv injection), `:2862-2866` (parent clear),
`thirdsarm_wrapper.h:19-45`; `src/replay/replay_browser_scan.{c,h}` (the
SDL scan to port); `docs/3sr-format.md`; `docs/mister-wrapper.md`.

**Create/modify:**
- **De-SDL scan** `vendor/Main_MiSTer/replay_scan.c` (+ `.h`) — port
  `replay_browser_scan.c` logic, replacing the SDL shims (§1.7:
  `SDL_GlobDirectory`→`scandir`/`glob`, `SDL_LoadFile`→`fopen`/`fread`,
  `SDL_GetPathInfo`→`stat`, `SDL_qsort`→`qsort`, string/mem shims →
  libc). Reads `<root>` flat + one subdir level for `*.3sr`, derives the
  `<name>.meta.json` sidecar, parses player names/date via cJSON.
  Produces a sorted `ReplayRow[]` (quarkid/p1/p2/date/path). Add to
  `main-mister-overlay.files`.
- **cJSON into the wrapper**: copy `third_party/cJSON/cJSON.{c,h}` into
  `vendor/Main_MiSTer/`; add both to `main-mister-overlay.files` so
  `$(wildcard *.c)` compiles `cJSON.c`.
- **Launch handoff** `thirdsarm_wrapper.cpp`/`.h`: add
  `void replay_play_handoff(const char* path_3sr);` — store the abs path
  in a file-scope buffer `g_replay_play_path[512]`, set a new
  `g_replay_play_armed`, `g_wrapper_restart_requested = 1`, SIGTERM the
  child (clone `direct_p2p_arm_and_restart` `:2333-2339`). In the argv
  block (`:2839-2842` sibling) push `--play-replay` + `g_replay_play_path`
  when armed; clear the parent copy post-fork (`:2862-2866`). Log
  `replay_play_arm=1` for SSH verification.
- **OSD pages** in `main-mister-full-menu.patch`: extend the MENU enum
  (`:13-27`) with `MENU_REPLAY_ROOT/ROOT1`, `MENU_REPLAY_LOCAL/LOCAL1`
  (and stubs `MENU_REPLAY_REMOTE*` filled by S3b). Re-point the `bit==31`
  intercept (`:188-198`) to `menustate = MENU_REPLAY_ROOT` instead of
  `replay_browser_handoff()`. Add the page arms (clone
  `MENU_DIRECT_P2P_ROOT`): ROOT = two-entry tabs (Remote / Local /
  Exit); LOCAL = scrollable `MenuWrite` list of
  `sprintf(" %-14s vs %-14s  %s", p1, p2, date)` rows wrapped in the FULL
  paging `while(1)` loop (skeleton `menu.cpp:2298-2299`; full instance
  `:3227-3306`/`:3551-3606`) — NOT JOIN_RECENT's single-pass `for` body,
  which would drop rows past `OsdGetSize()`; up/down/select; select →
  `replay_play_handoff(row.path)` then close OSD (clone
  `MENU_DIRECT_P2P_CONFIRM` `:578-593`). Empty list → a sane "No saved
  replays yet." panel (clone `JOIN_RECENT` empty branch `:510-521`).
- Regenerate the patch by editing the APPLIED
  `build/mister-wrapper-hps/src/menu.cpp` after `build-hps.sh
  --prepare-source`, then `git -C build/mister-wrapper-hps/src diff
  menu.cpp > tools/mister-wrapper/main-mister-full-menu.patch` (per the
  F4 patch procedure). Do not hand-count hunk offsets.

**Success criteria:**
- `build-hps.sh` completes; the regenerated patch applies cleanly;
  `cJSON.c` + `replay_scan.c` compile under `-Wall -Wextra`.
- `strings build/mister-wrapper-hps/MiSTer_3S-ARM | grep -- --play-replay`
  finds the injected literal; grep confirms the intercept re-point and
  the arm/argv/clear wiring.
- SSH-verifiable: `misterctl.sh run-wrapper --runtime-arg …` +
  osd-wrapper.log shows a child launch injecting `--play-replay <path>`
  and the `replay_play_arm=1` marker after a simulated select (or after
  the physical select, checkable post-hoc).
- Human TV/pad ONLY: OSD "Replay Browser" opens the ROOT page; LOCAL tab
  shows a scrollable, readable "P1 vs P2  date" list that pages with
  up/down and does not flicker; selecting a row closes the OSD and the
  game boots straight into that replay (overlay shows the names).

**Dependencies:** S0 (clean local list); the local `.3sr` used for the
TV test should be a NAMED one from S1's `publish_3sr.py` (or a
hand-`--quark-json`'d file) so the row is readable — but S2 does not
otherwise depend on S1.

**What NOT to do:** do not link SDL into the wrapper; do not block the
menu loop on file IO beyond a bounded scan (local scan is fast, but cap
the row count); do not write a handoff FILE (argv path is the payload);
do not touch the `[30]` seed or the menu.sv defense comments; do not
hand-edit patch hunk headers; do not remove the in-game browser yet (S6).

**Failure/fallback:** if the de-SDL scan misbehaves, the OSD LOCAL page
can render an empty-state panel and the in-game `--replay-browser` path
still exists as a fallback until S6. If the paging `while(1)` wrapper is
omitted or wired wrong, a long list does NOT scroll — `MenuWrite` silently
drops every row past `OsdGetSize()` (`menu.cpp:812-816`) and an
off-screen cursor becomes invisible; the fix is to clone the full paging
instance (`menu.cpp:3227-3306`/`:3551-3606`), not JOIN_RECENT's
single-pass body. As an interim guard, cap the visible list and add a
"showing N of M" hint.

---

## Stage S3a — De-SDL proxy port into the wrapper (search + get3sr client)

**Title:** Port `proxy_client.c` into the wrapper as a de-SDL'd
`replay_proxy.c` — pthread async search worker + the `get3sr` client that
downloads a PLAYABLE `.3sr` **and its `.meta.json` sidecar** — with no OSD
UI yet.

**Why:** the remote path's engine, split out from the OSD UI so each half
fits the ≤~2h budget. This stage stands up the network/threading/decode
plumbing that S3b's pages call. No menu work here — it is provable by a
tiny driver against the live proxy.

**Read first:** S1 + S2 outputs; §1.6, §1.7; `src/replay/proxy_client.c`
(the SDL client to port — `ProxyClient_Search` sync core, `SearchAsync`/
`PollAsync`/`TakeResults` worker `proxy_client.h:95-117`,
`ProxyClient_Fetch3sr` from S1 §5 as the game-side reference); the SDL
census (§1.7: atomics → `_Atomic`/mutex, `SDL_CreateThread`/`WaitThread`
→ pthread, string/mem/log → libc/`fprintf`); `fcade-proxy.js`
`handleSearch` `:550-608` + the new `get3sr` (S1 §4) for the wire shapes;
framing `:104-110`/`:668-679`; `thirdsarm_wrapper.cpp` config-read
pattern (`:400`+, `:1867`); `src/port/config/config.c:90-91` (proxy
host/port keys, default host `""`); the wrapper `-lpthread` link
(`Makefile.full.3s-arm:53`); `third_party/cJSON` already vendored into
the wrapper by S2.

**Create/modify:**
- **De-SDL proxy client** `vendor/Main_MiSTer/replay_proxy.c` (+ `.h`) —
  port `proxy_client.c`: SDL atomics → `_Atomic`/mutex, `SDL_CreateThread`/
  `WaitThread` → pthread, string/mem/log shims → libc/`fprintf`. Preserve
  the `ProxySearchParams`/`ProxyRow`/async API shape (§1.7) so S3b's UI
  polls the same state machine. Add to `main-mister-overlay.files` so
  `$(wildcard *.c)` compiles it.
- **`get3sr` client** in `replay_proxy.c`: send `{op:"get3sr", quarkid,
  game_index?}`, read the framed JSON, and for each game base64-decode
  BOTH `b64`→`<replay-root>/<quarkid>/game_N.3sr` and
  `meta_b64`→`<replay-root>/<quarkid>/game_N.meta.json` (the meta write
  is mandatory — names live only in the sidecar, P-1.1/§1.6). Return the
  written `.3sr` path(s). Wrap in the same pthread async pattern (a
  `Fetch3srAsync`/`PollAsync`/`TakeResult` trio) so S3b never blocks the
  menu loop.
- **Config read**: read `replay-proxy-host`/`replay-proxy-port` from the
  game config file (wrapper already reads it, §1.5); expose an accessor
  S3b can query to decide whether REMOTE is enabled (host `""` → disabled).

**Success criteria:**
- `build-hps.sh` completes; `replay_proxy.c` compiles under
  `-Wall -Wextra`; the wrapper still links (`-lpthread` present).
- SSH-verifiable via a tiny built-in test path or `run-wrapper` driver
  (no OSD): a search against the live proxy (`46.62.244.55:3479`) logs a
  framed request + parsed row count; a `get3sr` for a known quark writes
  BOTH `game_N.3sr` (first 4 bytes = `.3sr` magic, `docs/3sr-format.md`)
  AND `game_N.meta.json` (whose `players[]` has real names) under the
  replay root.
- The async worker returns results without blocking the caller (poll →
  DONE transition observed in the log).

**Dependencies:** S1 (get3sr proxy op + published `.3sr`/`.meta.json`
tree + wire shapes) and S2 (cJSON vendored into the wrapper; the replay
root + overlay-files manifest mechanics). No dependency on S3b.

**What NOT to do:** do not link SDL into the wrapper; do not build any
OSD page here (that is S3b); do not block on connect/recv on the caller
thread (pthread worker only); do not write the `.3sr` without its
`.meta.json` (nameless replays); no TLS/cookie on device; no second wire
protocol (base64 in the existing JSON frame).

**Failure/fallback:** if the pthread port is racy, gate the worker behind
a single mutex + condition and keep the API synchronous-looking to the
caller (poll returns RUNNING until the worker joins). If the live proxy
`.3sr` tree is not yet deployed, the fetch returns `PROXY_ERR_UPSTREAM`
and S3b renders "try again later" — search still works.

---

## Stage S3b — OSD REMOTE pages: search UI + OSK + download-and-launch

**Title:** The `MENU_REPLAY_REMOTE*` pages — recent/best/by-player search
with the OSK, async "Searching…/Downloading…" rendering, and a picked
remote replay downloaded via S3a's `get3sr` client then launched by S2's
`--play-replay` handoff.

**Why:** the actual point surfaced in the OSD — browse Fightcade replays
by readable name and play one with no conversion step. Builds on S2's
scaffolding (ROOT page, paging list, launch handoff) and S3a's de-SDL
proxy/`get3sr` engine.

**Read first:** S2 + S3a outputs; §1.6, §1.7; `replay_proxy.{c,h}` from
S3a (the async search + `get3sr` API); the OSK template
`main-mister-full-menu.patch:66-155` (cells) + `:431-498` (ENTRY render +
nav); the paging `while(1)` loop skeleton `menu.cpp:2298-2299` + full
instance `:3227-3306`/`:3551-3606` (for the scrollable result list — NOT
JOIN_RECENT's single-pass body); `MENU_DIRECT_P2P_CONFIRM` handoff
`:578-593`; `MENU_REPLAY_ROOT` from S2; `osd.cpp` (32-col grid, for row
budgeting).

**Create/modify:**
- **OSD REMOTE pages** in `main-mister-full-menu.patch`: extend the enum
  (`:13-27`) filling the S2 stubs — `MENU_REPLAY_REMOTE` (sub-tabs:
  Recent / Best / By Player), `MENU_REPLAY_REMOTE_SEARCH` (the 6×7 OSK
  cloned from Direct-P2P join for the player-name query),
  `MENU_REPLAY_REMOTE_LIST` (scrollable results wrapped in the FULL
  paging `while(1)` loop — render a "Searching…" state while
  `PollAsync` is RUNNING, then the rows on DONE; never block the loop).
- **Download flow**: select a row → S3a's `Fetch3srAsync(quarkid[,
  game_index])` → "Downloading…" → on success
  `replay_play_handoff(written_path)` (S2) → close OSD. Multi-game (row
  `num_matches > 1`): either list the N games as a sub-list or auto-pick
  game_0 with a "1/N" hint (choose per how `get3sr` returns the manifest,
  S1 §4).
- **Disabled state**: if S3a's config accessor reports host `""`, the
  REMOTE tab shows "Remote browsing disabled (set replay-proxy-host)".
- Wire the REMOTE tab into `MENU_REPLAY_ROOT` from S2.
- Regenerate the patch via the F4 procedure (edit the APPLIED
  `build/mister-wrapper-hps/src/menu.cpp` after `build-hps.sh
  --prepare-source`, then `git -C … diff menu.cpp > …full-menu.patch`).
  Do not hand-count hunk offsets.

**Success criteria:**
- `build-hps.sh` completes; the regenerated patch applies cleanly; the
  async search/download never blocks the menu (the "Searching…"/
  "Downloading…" states render and the pad stays responsive).
- SSH-verifiable: run-wrapper drives a search against the live proxy
  (`46.62.244.55:3479`); osd-wrapper.log shows the framed request, a
  parsed row count, and (on a select) the `get3sr` fetch writing a
  `.3sr` + `.meta.json` then `replay_play_arm=1` + child relaunch with
  `--play-replay`.
- Human TV/pad ONLY: REMOTE tab lists readable "P1 vs P2  date" rows;
  By-Player OSK filters; picking a row downloads and boots straight into
  a PLAYING replay with names — **no NEEDS CONVERSION anywhere**.

**Dependencies:** S3a (de-SDL proxy + `get3sr` client) and S2 (OSD
scaffolding, ROOT page, paging list, launch handoff). Transitively S1
(published `.3sr`/`.meta.json` tree + proxy `get3sr` op).

**What NOT to do:** no blocking network in the menu loop (call S3a's
async worker only); no second wire protocol; do not clone JOIN_RECENT's
single-pass body for the result list (use the full paging loop); do not
truncate so aggressively that real Fightcade handles become ambiguous —
budget the 32-col OSD row (see risks); do not fetch on every cursor move
(fetch only on explicit select).

**Failure/fallback:** if the proxy/`.3sr` tree is unreachable, the tab
shows the typed error hint and LOCAL/playback are unaffected. If 32-col
truncation of long handles is unacceptable, add a detail line (second
row per entry) or a "hold to see full name" panel.

---

## Stage S4 — During-replay player names (health-bar placement + fallback)

**Title:** Draw P1/P2 names during playback — under the health bars
(y=48) with the bottom `"P1 vs P2"` line as the guaranteed fallback.

**Why:** requirement C — names should be visible during the replay,
ideally by each character's health bar/portrait. Names are already in
memory (§1.9); this is a small, well-scoped draw change.

**Read first:** §1.9; `src/replay/replay_overlay.c` (whole file —
`draw_status_line` `:39-64`, `ReplayOverlay_Draw` `:105`, tex guard
`:121`, states `:135-140`, y constants `:29-31`); `sc_sub.c:598-666`
(`SSPutStrProP`, `SSGetDrawSizePro`); the HUD coordinate cites
(`vital.c:88`, `sc_sub.c:1049,1079-1088,1691,1704,1481,1498-1499`);
`replay_player.c:651-656` (name getters).

**Create/modify:**
- `src/replay/replay_overlay.c`: add a sibling `draw_name_labels()`
  called from `ReplayOverlay_Draw` in the NAVIGATING/PLAYING case
  (alongside `draw_status_line`, under the same `ppgScrList.tex` guard).
  P1 label `SSPutStrProP(0, 8, 48, 9, 0xFFFFFFFF, p1name, 1)`; P2 label
  `SSPutStrProP(0, 376 - SSGetDrawSizePro(p2name), 48, 9, 0xFFFFFFFF,
  p2name, 1)`. NULL-guard each name getter (as `draw_status_line`
  already does). Gate the y=48 labels behind a small compile/config
  switch (`RPL_OVL_HUD_NAMES`) so they can be disabled if TV testing
  shows collisions.
- Keep `draw_status_line` at y=202 as the fallback line; when HUD labels
  are ON, optionally shorten the bottom line to just the date (avoid
  duplicate names) — decide after TV review.

**Success criteria:**
- Desktop build clean; overlay draws without touching player state
  (read-only; C1 checksum cadence unchanged — verify the desync frame
  count is identical with labels on/off on a known replay).
- Human TV ONLY: during a replay, P1 name sits under the left health bar
  and P2 under the right at y=48, legible, not colliding with
  portraits/stun/char-name in normal play, in front of the HUD.
- Fallback verified: with `RPL_OVL_HUD_NAMES` off, the bottom
  `"<p1> vs <p2>  <date>"` line renders as before.

**Dependencies:** none structural (works with any named `.3sr`); reads
best against an S1-named file so labels are non-empty.

**What NOT to do:** do not draw in the y`24..48` strip (occupied —
§1.9); do not mutate player/engine state from the overlay; do not raise
the priority into wipe territory (keep priority 1); do not assume glyph
widths — right-anchor P2 via `SSGetDrawSizePro`, never flag=1.

**Failure/fallback:** if the y=48 row collides with gameplay sprites on
TV, disable `RPL_OVL_HUD_NAMES` and ship the bottom `"P1 vs P2"` line
(the fallback is already the shipping behavior). Either way names are
shown.

---

## Stage S5 — OSD menu reorder (Quartus RBF) — ISOLATED long-pole

**Title:** Reorder the CONF_STR display (bits preserved) and rebuild the
RBF in the `quartus2` VM.

**Why:** requirement D — the visible menu order. CONF_STR is compiled
into BRAM, so this is the ONLY Quartus step. Start it EARLY and keep it
isolated so the fast HPS/runtime iterations (S1-S4/S6) are never blocked
on the ~40-60 min compile.

**Read first:** §1.8; `vendor/Menu_MiSTer/menu.sv:278-318` (CONF_STR +
`hps_io` + the `:280-283` bit-30 and `:286-290` bit-31 defense comments);
`thirdsarm_wrapper.cpp:1867,1982` (the `[30]` seed defense);
`main-mister-full-menu.patch:180-198` (T[23]/T[29]/T[31] intercepts, so
you confirm none key on menu position); `docs/mister-runbook.md`
(Quartus, RBF deploy `_Other/`); `docs/agent-memory/mister-wrapper-quartus.md`;
`tools/mister-wrapper/build-core.sh` (prepare_source token patch, dated
rbf copy); memory `feedback-quartus-nohup`, `feedback-quartus-fast-build`,
`feedback-quartus-process-mgmt`, `feedback-read-runbooks-before-deploy`.

**Create/modify:**
- `vendor/Menu_MiSTer/menu.sv` ONLY: reorder the CONF_STR string literals
  to the target order in §1.8 (Play Online, Replay Browser, Button Check,
  `-;`, Arcade Balance, Game Mode, Hold to Pause, `-;`, FPS Counter, then
  the rest unchanged). Move the `O[30]` line together WITH its
  `:280-283` defense comment (keep the comment adjacent to its bit). Keep
  the `:286-290` bit-31 comment adjacent to `T[31]`. **Change ordering
  only — never a bit index, never the `wire [46:0] status` width
  (`:317`), never the `db58a5f6` seed ordering in the wrapper.**
- Launch the Quartus compile detached in the `quartus2` VM (nohup; never
  a foreground `colima ssh` — SSH times out and kills it). Verify launch
  within ~60s (pgrep + tail the log) before walking away; NEVER kill a
  running build; never a second concurrent compile. Produce the dated
  `build/mister-wrapper-core/3S-ARM_YYYYMMDD.rbf`.

**Success criteria:**
- Generated `build/mister-wrapper-core/src/3S-ARM.sv` contains the new
  CONF_STR order AND the `3S-ARM;;` identity token (prepare_source ran).
- Quartus log ends BUILD_DONE with no fit/asm errors and a clean
  `.sta.summary` (timing pass on clk_sys/clk_pix — comment/ordering-only
  RTL delta, so timing should be unchanged from the last passing build).
- Fresh-mtime RBF of plausible size; dated copy present.
- Human TV ONLY (after S-deploy pairing): the OSD shows the new order;
  every moved trigger still works (Play Online, Replay Browser, Button
  Check fire; Arcade Balance/Game Mode/Hold to Pause toggle correctly);
  a stale-CFG device (old `3S-ARM.CFG`) still boots with Arcade Balance
  seeded correctly (bit-30 defense intact).

**Dependencies:** none to START (independent RTL). Deploy pairs with the
S2/S3b wrapper (a new-order RBF with an old wrapper still works because
intercepts key on bit number, but ship the pair together to avoid a dead
`MENU_REPLAY_*` line).

**What NOT to do:** never reassign a bit; never widen/alter `status`;
never touch the `[30]` seed ordering or the two defense comment blocks
beyond keeping them adjacent to their bits; never kill/relaunch a running
Quartus build; never deploy the RBF to `menu.rbf` — only
`/media/fat/_Other/3S-ARM_YYYYMMDD.rbf`.

**Failure/fallback:** CONF_STR compile error → check quoting/semicolons
against neighbors and that prepare_source's token patch still matched.
Timing failure (unlikely for an ordering-only delta) → one clean rebuild;
if it persists, revert `menu.qsf` fast settings for this one build and
accept the longer compile (never ship without `.sta` clean).

---

## Stage S6 — Retire the in-game browser (for browsing)

**Title:** Remove/hide `replay_browser*.c` as a browse surface; keep the
player + repositioned overlay.

**Why:** requirement E — the OSD is now the browser; the in-game browser
was the jank source and is superseded for browsing. Do this only after
S2/S3b prove the OSD path on-device.

**Read first:** `src/replay/replay_browser.{c,h}`,
`replay_browser_scan.{c,h}` (device SDL copies), `src/main.c:44-45,
655-676, 1098-1103` (browser wiring), `src/configuration.h:107-118`
(`--replay-browser` field), `src/args.c:602-614` (the flag);
`thirdsarm_wrapper.cpp:2559-2561` (`replay_browser_handoff`, now unused
by the OSD after S2 re-points bit==31).

**Create/modify (decide with the user):**
- **Option A — hidden fallback (recommended interim):** keep
  `--replay-browser` and `replay_browser*.c` compiled but no longer
  reachable from the OSD (S2 re-pointed bit==31). Leave
  `replay_browser_handoff()` in the wrapper as dead-but-harmless, or gate
  it behind an undocumented env for emergencies. Zero deletion risk.
- **Option B — delete:** remove `replay_browser.{c,h}`,
  `replay_browser_scan.{c,h}` (device copies — the wrapper has its own
  de-SDL `replay_scan.c`), the `--replay-browser` flag/field, the
  `main.c` browser tick/draw wiring, and `replay_browser_handoff()` +
  `g_replay_browser_armed` + the `--replay-browser` argv injection in the
  wrapper. Keep `replay_player.c`, `replay_overlay.c`, `fcade_stream.c`,
  `proxy_client.c` (still used by the game for the S1 fetch/desktop
  testing) and the de-SDL wrapper copies.

**Success criteria:**
- Runtime + wrapper build clean after the chosen option.
- Grep confirms no remaining OSD path reaches the in-game browser
  (bit==31 → `MENU_REPLAY_ROOT` only).
- On-device: launching a replay via the OSD still works; no dead menu
  line; (Option B) `--replay-browser` is gone from `--help`.

**Dependencies:** S2 + S3a + S3b landed and TV-verified (do not remove the
fallback before the replacement is proven).

**What NOT to do:** do not delete `replay_player.c` or the overlay; do
not delete the game-side `proxy_client.c`/`fcade_stream.c` still used by
S1's fetch/desktop path; do not remove the `--play-replay` flag; decide
delete-vs-hide WITH the user (irreversible deletion of a working
fallback).

**Failure/fallback:** default to Option A (hide) if there is any doubt —
it carries zero risk and can become Option B later.

---

## 5. Per-stage risk register

- **De-SDL pitfalls (S2 scan / S3a proxy):** SDL atomics have acquire/release
  semantics; a naive `volatile int` swap can race on ARM. Use `_Atomic`
  or a pthread mutex for the async worker state. `SDL_GlobDirectory`
  pattern semantics differ from `glob(3)` (case, hidden files) — match
  the existing scan's filter exactly. `SDL_strlcpy` truncation semantics
  must be preserved (bounded copies, always NUL-terminate).
- **Blocking network in the menu loop (S3b, via S3a's worker):** the OSD `HandleUI` runs on
  the wrapper's UI thread; ANY synchronous connect/recv freezes the OSD.
  All proxy search/fetch MUST be the pthread worker + polled state; the
  menu renders "Searching…/Downloading…" and keeps reading the pad.
- **32-col truncation (S2 local / S3b remote):** the OSD grid is 32 cols
  (`osd.cpp`); real Fightcade handles can exceed the budget after
  " vs " + date. Truncate with an ellipsis and consider a two-line
  entry or a detail panel; never silently drop distinguishing suffix.
- **Quartus long-pole (S5):** ~40-60 min emulated compile; nohup only,
  never kill, never concurrent. Isolated so S1-S4/S6 proceed in parallel
  (memory `feedback-parallel-builds`).
- **Stale-CFG bit safety (S5):** the reorder is display-only; the bit-30
  seed defense (`thirdsarm_wrapper.cpp:1982`) and bit-31 intercept model
  keep old `3S-ARM.CFG` files safe — but ONLY if no bit is reassigned and
  the seed ordering is untouched.
- **ARM cross-build (S2/S3a/S3b/S6):** the wrapper is `arm-none-linux-gnueabihf`
  with `-Wall -Wextra` and specific `-Wno-*` (`Makefile.full.3s-arm:52`);
  the runtime cross is clang-20 with promoted warnings. New C must be
  warning-clean on BOTH. cJSON under the wrapper flags is UNVERIFIED —
  confirm at first build.
- **get3sr blob framing (S1):** base64-in-JSON is simplest but ~1.33×
  size; a full match ≈29KB → ~39KB, fine for one framed response, but cap
  and reject oversize. Multi-game manifests can be large if a quark has
  many games — page or per-game fetch.
- **Data-fix regression (S1):** if the batch ever runs `make_3sr` without
  populated players, `players:[]` returns and the OSD rows/overlay names
  go blank again — the `publish_3sr.py` driver must fail loudly when a
  catalog row lacks names.
- **Deploy pairing (S3b/S5):** a new-order/new-page RBF with an old
  wrapper (or vice-versa) can show a dead line; ship RBF + wrapper +
  runtime together for the OSD milestones.

---

## 6. Open questions (could not be resolved from code alone)

1. **`get3sr` multi-game UX**: return all N games as a manifest (device
   picks) vs auto-serve `game_0`? Depends on how often a quark has >1
   game and whether the user wants per-game selection. Design in S1/S3b;
   the row's `num_matches` (`ProxyRow.num_matches`) is available to
   decide.
2. **`.3sr` VPS layout + `get3sr` root**: exact server path for the
   blob tree (`/opt/fcade-proxy/3sr/<quarkid>/`?) — a VPS-ops decision.
   (The related question of refusing to serve a game whose meta lacks
   names is now DECIDED: S1 §4 mandates the proxy return the `.meta.json`
   and reject a missing/empty-`players[]` sidecar.)
3. **Catalog ↔ published-`.3sr` coupling**: should the catalog only
   advertise quarks that have a published statcheck-clean `.3sr`
   (so every REMOTE row is guaranteed playable)? That is the cleanest UX
   but couples the two rails. Undecided.
4. **cJSON under the wrapper warning set**: UNVERIFIED whether
   `cJSON.c` compiles clean under `-Wall -Wextra -Wno-*`
   (`Makefile.full.3s-arm:52`). Confirm at S2 first build; may need a
   targeted `-Wno-*` for cJSON only.
5. **"~295 catalog rows"**: UNVERIFIED from code (no committed catalog).
   Confirm against the live proxy `status` before relying on the figure.
6. **Health-bar y=48 collision in real matches**: UNVERIFIED whether a
   high-jumping fighter reaches y≈48 (stage-camera dependent). Resolved
   only by TV testing in S4; the `RPL_OVL_HUD_NAMES` switch + y=202
   fallback cover the downside.
7. **Delete vs hide the in-game browser (S6)**: a user decision (removing
   a working fallback is irreversible) — default to hide.
