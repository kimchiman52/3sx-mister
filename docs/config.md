# Config

3S-ARM supports a config file which allows you to change several useful options.

Config location:
- **Windows**: `C:\Users\<username>\AppData\Roaming\CrowdedStreet\3S-ARM\config`
- **Linux**: `~/.local/share/CrowdedStreet/3S-ARM/config`
- **macOS**: `~/Library/Application Support/CrowdedStreet/3S-ARM/config`

## Options

### `fullscreen`

Whether the game should start in fullscreen mode.

### `window-width` / `window-height`

Window dimensions to use when `fullscreen` is set to `false`.

### `scale-mode`

The way the internal 384x224 buffer is scaled.

Possible values:
- `native`: No scaling. Keeps the internal `384x224` image size and centers it. On MiSTer's 384-native analog TV path, this preserves the game's native width in the framebuffer.
- `nearest`
- `linear`
- `soft-linear`: Produces an image with a balance of sharpness and sizing consistency
- `integer`: Produces a pixel-perfect image, but requires a 4K display (⚠️ WARNING: the image is gonna be cropped if your display resolution is smaller than 2688x2016)
- `square-pixels`: Integer (whole-number) scaling with square pixels. Use this if you play on a CRT

### `scanlines`

Defines the strength of the scanline filter (from `0` to `100`). `0` means the filter is disabled.

### `draw-players-above-hud`

Allow characters to render in front of the top HUD similar to Street Fighter IV. May introduce visual abnormalities on certain stages.

With this setting on, these stage decorations are disabled to prevent overlapping with the HUD:
- Chun-Li's stage: bamboo stick on the right
- Makoto's stage: tree on the right
- Yang's stage: rain overlay

### `balance`

Arcade (CPS3) vs PS2 balance **auto-selects at boot** — there is no OSD
toggle. Values:

- `auto` (default): arcade balance is used when a CPS3 ROM source passes
  content verification AND the full 20-character adaptation succeeds;
  anything else falls back to PS2 balance with the reason logged and written
  to `<pref>/balance.status` (line 1: `Arcade (CPS3)` / `PS2`; line 2: the
  reason). Adaptation is all-or-nothing: a single character failing means the
  whole session is PS2, never a per-character mix.
- `ps2`: force PS2 balance even with a valid ROM (config-file-only knob for
  players who own the ROM but prefer PS2 balance).

No ROM ships with 3S-ARM and none is looked for anywhere in the program's own
install directory. ROM discovery tries, in order: the `THIRDSARM_CPS3_ZIP`
env override (dev-only, not a player-facing setting — how CI-style and
cross-arch runs point at a romset outside a MiSTer arcade install entirely),
then five MiSTer arcade ROM directories, in the same relative order MiSTer
Main's own `findGamesDir("mame")` resolution walks them:

| Directory | Why it is searched |
| --- | --- |
| `/media/usb0/mame` | first USB storage slot, bare layout |
| `/media/usb0/games/mame` | first USB storage slot, `games/` layout |
| `/media/fat/mame` | where a hand-assembled set commonly lands |
| `/media/fat/games/mame` | update_all's default, and the verified path |
| `/media/fat/_Arcade/mame` | Main's own last-resort fallback |

Both `sfiii3nr1.zip` and `sfiii3.zip` are checked in each — the two basenames
the shipped jtcps3 MRA itself declares. Entries are matched by content hash
(stored-CRC32 pre-filter + SHA-256 confirmation), so flat, merged, and
subdirectory-variant packagings all work regardless of which directory or
basename supplied them.

This list is deliberately shorter than Main's own 19-directory walk, which
also covers `usb1`–`usb5`, `/media/network` and `/media/fat/cifs`. Those 14
were never exercised by any test or any run, so a typo in one was
indistinguishable from the directory being absent. A miss is no longer
silent, which is what makes the shorter list safe: when no ROM is found the
log now says which of the three things went wrong — no directory existed, a
directory existed but held neither basename, or **a zip was found and
rejected by content verification** (a wrong-revision romset, which
previously produced the same message as having no romset at all).

Notes:
- Replaces the removed `arcade-balance` bool toggle; stale `arcade-balance`
  lines in existing configs are ignored.
- A hand-added `balance = ps2` line SURVIVES the MiSTer wrapper's config
  rewrites. Every `write_runtime_*_default()` in
  `vendor/Main_MiSTer/thirdsarm_wrapper.cpp` is a line-preserving
  copy-through: it rewrites only the single line whose trimmed key
  `strcasecmp`-matches its own target and emits every other line verbatim
  via `fputs(line, out)`. The 15 targeted keys (`scale-mode`, `arm-clock`,
  `game-mode`, `hold-to-pause`, `language`, `bgm-type`,
  `aspect-ratio`, `h-position`, `v-position-v2`, `v-position`,
  `vertical-crop`, `crop-offset`, `scale`, `h-size`, `show-fps`) do not
  include `balance`, and no writer regenerates the file from a template.
  (`arcade-balance` was on that list until the Arcade Balance OSD row was
  replaced by the read-only Balance status row; the wrapper no longer
  writes that key at all.)
- Netplay arms only in verified-arcade state and the MIST handshake carries a
  digest of the adapted data, so peers always simulate identical balance.
- The test runner picks its balance EXPLICITLY, via `--test-balance ps2|arcade`
  (task #108). `--test-enable` with no `--test-balance` still pins PS2
  regardless of ROM presence, so harness expectations stay hermetic on any
  machine — but that is now a stated default rather than an unavoidable
  consequence of being a test process. `--test-balance arcade` runs the
  shipping CPS3 tables and is a hard requirement: a run that cannot reach
  20/20-adapted arcade balance logs the reason and exits 6 instead of quietly
  falling back to PS2. `--test-scene-preset training-frame-data` (the
  frame-data corpus suite) refuses to start without an explicit
  `--test-balance`; each corpus declares its own via a `balance:` key
  (`tools/frame-data/CORPUS-AUTHORING.md`).

### `bgm-type`

Selects between the PS2 arranged soundtrack (`arranged`, default) and the
original arcade soundtrack (`original`). Mirrors the in-game Sound Options
menu's BGM Type setting (`sys_w.bgm_type`, `BGM_ARRANGED`/`BGM_ORIGINAL` in
`structs.h`).

On MiSTer this is exposed as the **BGM Type** option in the OSD menu (status
bit `[14]`). The wrapper writes the toggle into this config key, so it
persists across launches; like Arcade Balance it applies on the **next game
launch** (use OSD → Restart), because the boot-time override
(`BgmType_ApplyBootOverride()`) only runs once, right after the settings save
file finishes loading.

Two-way sync: changing BGM Type from the in-game Sound Options menu also
writes this config key (`BgmType_PersistToConfig()`), so the OSD reflects an
in-game change at the next launch too, and the two settings never drift
apart from each other.

### `language`

In-game text language. Values:

- `auto` (default): no override — the settings save file's `Language` field
  decides, and with no save yet (or a pre-V2 367-byte save) that field comes
  from `Get_Default_Language()`, which picks Japanese only when `ja` is the
  first `en`/`ja` entry in SDL's preferred-locale list (`src/main.c`).
- `english` / `japanese`: force that language at boot, over the settings save.

On MiSTer this is exposed as the **Language** option on the OSD's Game page
(status bit `[47]`). Like BGM Type it applies on the **next game launch**
(use OSD → Restart), because the boot-time override
(`Language_ApplyBootOverride()`, `src/port/config/language.c`) only runs once,
right after the settings save file finishes loading.

Two-way sync: changing Language from the in-game **Screen Adjust** menu also
writes this key (`Language_PersistToConfig()`), so the OSD reflects an in-game
change at the next launch.

`auto` is a one-shot sentinel: on the first boot that sees it,
`Language_ApplyBootOverride()` writes the resolved language back into the key.
That is what lets the OSD row show the real language instead of defaulting to
English — the wrapper reads this key to seed the status bit and has no other
way to see how the locale/save resolved. Write `auto` back into `config` by
hand to re-derive from the save/locale.

### `software-frame-mode`

Controls whether gameplay uses the 3S-ARM-owned software frame path or the legacy SDL-owned gameplay frame path.

Defaults:
- MiSTer builds: `on`
- Non-MiSTer builds: `off`

Possible values:
- `off`: Keep the existing SDL-owned gameplay frame path
- `on`: Keep the `384x224` gameplay frame in 3S-ARM-owned software memory. On MiSTer, eligible frames present directly through fbdev to avoid SDL readback; when composition or screenshots still need SDL, the frame uploads back to `cps3_canvas`.

### `super-effect-quality`

Controls MiSTer-only rendering optimization during super art activation.

**Dev-only:** the OSD knob has been removed. Flip via `config.ini` for
release builds; the runtime read path is preserved.

Defaults:
- MiSTer builds: `cached-bg`
- Non-MiSTer builds: `full`

Possible values:
- `full`: No reduction — render every frame fully
- `cached-bg`: Cache the rendered background surface on the first super art activation frame, then restore it via fast blit on subsequent frames while rendering characters/effects/HUD fresh at 60fps

Notes:
- This setting is only active on MiSTer builds. On non-MiSTer builds the config key is parsed but behaves like `full`.
- Detection uses `sa_stop_check()` (cinematic freeze signal) which fires for all characters and all super arts. A grace period of ~15 frames after the signal drops covers the zoom-out transition.

### `show-fps`

Whether to draw a small FPS readout while the game is running.

Defaults:
- `false`

Notes:
- On MiSTer fbdev output, the overlay is drawn at the bottom-center of the active picture area so it stays away from overscan-prone corners.
- The overlay is opt-in and uses a lightweight cached label update path instead of perf capture telemetry.

### `replays-root`

Directory holding the cached `.3sr` replay set. Both a flat layout and one
level of subdirectories are supported. Each `<name>.3sr` may have a
`<name>.meta.json` sidecar (player names + date) used for display labels; a
missing or corrupt sidecar falls back to the filename.

Defaults:
- MiSTer builds: `/media/fat/games/3s-arm/replays`
- Miyoo Mini Plus builds: `/mnt/SDCARD/Roms/PORTS/Games/3s-arm/replays`
- Other builds: `./replays` (relative to the working directory)

Notes:
- Formerly `replay-browser-root`. It was renamed when the in-game pad-driven
  replay browser was removed; the directory itself and its layout are
  unchanged, so an existing replay root needs no migration — only the key
  name in `config.txt` changes.
- The MiSTer OSD Replay menu in the HPS wrapper does **not** read this key.
  It carries its own hardcoded copy of the MiSTer path (`REPLAY_LOCAL_ROOT`),
  so the two must be kept in step if the default ever moves.
- Also read by the `replays-max-mb` eviction sweep below.

### `replays-max-mb`

Caps the total size, in megabytes, of raw Fightcade-fetch stream payloads
(`frames.bin`, `inputs`, `savestate`, `summary.json` — the bulky per-replay
files a fetch writes alongside its tiny `.3sr` + `.meta.json`) kept under
`replays-root`. When the cap is exceeded, the oldest fetch directories
(LRU by mtime) have their raw files evicted first, down to the cap or until
every fetch directory has been swept.

Defaults:
- `200`

Notes:
- `0` disables eviction entirely (delete-only): raw files accumulate
  unbounded until removed manually.
- Eviction NEVER touches `.3sr` or `.meta.json` — only the four raw
  basenames above, and only ones that pass path validation (realpath under
  `replays-root`, and not a symlink — a symlinked entry is refused
  outright, never followed). A fetch directory is only removed once it is
  completely empty (its `.3sr`/`.meta.json`, if present, keep it around).
- A per-directory "last used" timestamp is the max mtime among its present
  raw files; directories are evicted oldest-timestamp-first.
- The in-game caller that used to trigger this sweep was the pad-driven
  replay browser, now removed. The storage module
  (`src/replay/replay_storage.c`) is retained and unchanged; the replacement
  shuffle viewer takes over as its caller.

### `replay-proxy-host` / `replay-proxy-port`

Host and TCP port of the VPS `fcade-proxy` (`tools/fcade-proxy`) used to search
Fightcade for replays. The proxy terminates Cloudflare/TLS on the server; the
device speaks only a plain length-framed JSON protocol, so no TLS or cookie
ever lives on-device.

Defaults:
- `replay-proxy-host`: `""` (empty — **remote browsing disabled**; the device
  is local-only)
- `replay-proxy-port`: `3479` (the proxy's default port)

Notes:
- **The game does not read these keys.** The consumer is the MiSTer OSD
  Replay menu in the HPS wrapper, which parses them out of the on-device
  config file (`RpConfigLoadFrom()` in `vendor/Main_MiSTer/replay_proxy.c`).
  The game's only role is that its config defaults table is what seeds the
  file, so the two rows must stay in `src/port/config/config.c` even though
  nothing under `src/` reads them.
- The in-game REMOTE browse tab that once used these is gone with the
  in-game browser; remote search now lives entirely in the OSD menu.
- Selecting a remote result downloads the **raw** Fightcade stream
  (`inputs`/`savestate`/`frames.bin`/`summary.json`) into
  `<replays-root>/<quarkid>/`. Because on-device conversion is a NO-GO
  (Step B3), the downloaded dir is **not playable as-is**. Convert it
  off-device (`tools/replay_preprocessor.py` +
  `tools/fcade-replays/make_3sr.py`) into a `.3sr` and push that back to the
  root — see the runbook's Replays section.
- Each completed download runs the `replays-max-mb` eviction sweep (above).

### `video-driver-order`

Comma-separated SDL video backend preference list passed via `SDL_HINT_VIDEO_DRIVER` before SDL init.

Example:
- `kmsdrm,offscreen,dummy`

### `render-driver-order`

Comma-separated SDL renderer backend preference list passed via `SDL_HINT_RENDER_DRIVER` before renderer creation.

Example:
- `software`

### Direct-P2P (`netplay-direct-p2p-*`)

Runtime knobs for the STUN/UPnP-based direct-peer connect flow
(docs/plan-stun-direct-p2p.md). Users coordinate the peer code
out-of-band (text message, voice) and paste it into the wrapper OSD.

`netplay-direct-p2p-host-port` (int, default `0`)
- UDP port the host wants to bind for Direct-P2P. `0` means
  OS-assigned ephemeral. Advisory — if the port is already bound the
  OS falls back to ephemeral; the public port reported to the peer
  is whatever the NAT maps it to, not what you requested.

`netplay-direct-p2p-disable-upnp` (bool, default `false`)
- Skip the UPnP IGD first-try path. Set `true` if your router
  misbehaves on UPnP and fails slowly (some routers reply to
  `UPNP_Discover` but then hang on `AddPortMapping`). Since S7 this
  does **not** disable the NAT-PMP/PCP backend — that has its own key
  below — and the STUN fallback always runs regardless.

`netplay-direct-p2p-disable-natpmp` (bool, default `false`)
- Skip the NAT-PMP (RFC 6886) / PCP (RFC 6887) port-mapping backend
  that runs when UPnP IGD discovery fails
  (docs/plan-netplay-connection.md §9). It is a hand-rolled UDP client
  against port 5351 of the default gateway — no new library — and it
  tries PCP first, downgrading to NAT-PMP when the gateway answers
  `UNSUPP_VERSION` with version 0 (RFC 6887 §9). Set `true` if your
  router's NAT-PMP implementation misbehaves, or to keep the host from
  spending its truncated 4 s probe budget on a gateway you know is
  silent. Gateway lookup is Linux-only (`/proc/net/route`); on other
  platforms this backend reports no gateway and never sends anything,
  so the key has no effect there. STUN fallback always runs regardless.

`netplay-direct-p2p-last-peer-code` (string, no default)
- Populated at runtime on a successful Join to enable quick-rejoin
  on a "match lost" reconnect flow. Not persisted across runs in
  this phase (Config_Save is a stub until a future cleanup).

`netplay-direct-p2p-handoff-path` (string, default
`/tmp/3s-arm-netplay.handoff`)
- File the wrapper writes Host/Join intent into before exec'ing the
  game binary. tmpfs path; contents are readable only by root (mode
  `0600`) and consumed-and-unlinked by the game on first read.

`netplay-direct-p2p-stun-timeout-ms` (int, default `4000`)
- Overall wall-clock budget for STUN discovery. Since S2
  (docs/plan-netplay-connection.md §4) all four servers are probed in
  PARALLEL from the one socket with per-server retransmits at
  0/500/1500 ms, DNS resolves concurrently on a side thread, and a
  numeric-IP fallback list covers a dead resolver — the first
  parseable Binding Response wins. Clamped to 1000–15000 ms: the floor
  because below one RTO the retransmit ladder is meaningless, the
  ceiling because `Stun_Discover` is not cancellable mid-run and
  cancelling Host/Join mid-discovery blocks the game thread for up to
  this budget. (Pre-S2 this key was read by nothing; the serial
  implementation had its own hard-coded ~2 s/server budget.)

`netplay-direct-p2p-disable-bilateral` (bool, default `false`)
- If true, disables the bilateral hole-punch fallback. Direct-P2P will
  terminate with "Cannot reach peer. Possible Symmetric NAT." on punch
  timeout, matching behavior before the bilateral feature landed.

`netplay-direct-p2p-signal-url` (string, default
`udp://46.62.244.55:3478`)
- Rendezvous server URL for bilateral hole-punch endpoint exchange.
  Only used when the direct punch fails; never contacted on LAN sessions.
  UDP scheme only.

`netplay-direct-p2p-signal-budget-ms` (int, default `8000`)
- Maximum wall-clock time spent waiting for the rendezvous server to
  pair us with our peer before giving up and reporting bilateral failure.
- Since S1 (docs/plan-netplay-connection.md) this bounds the JOINER's
  signaling phase only; the host's re-REGISTER loop is persistent and
  paced by `netplay-direct-p2p-register-interval-ms` instead.

`netplay-direct-p2p-bilateral-punch-ms` (int, default `5000`)
- Length of the bilateral Stun_HolePunch window, in milliseconds.
  Longer than the initial direct punch window to accommodate post-
  signaling clock skew between peers. Raised from 3000 in S2
  (docs/plan-netplay-connection.md §4): the two sides' punch windows
  start skewed by DELIVER-arrival timing, and both loops tolerate
  stray datagrams, so extra overlap only costs failure-case wait.

`netplay-direct-p2p-race-budget-ms` (int, default `8000`)
- S6 candidate racing (docs/plan-netplay-connection.md §8): the WHOLE
  post-STUN establishment wall clock, on both roles. Since S6 the punch
  candidates and the rendezvous signaling leg run CONCURRENTLY on the
  one worker thread instead of one after another, so this replaces the
  old serial sum (direct punch + signal budget + bilateral punch =
  15500 ms) as the bound on a failing attempt. The per-leg keys above
  still bound their own legs INSIDE it: `signal-budget-ms` the
  rendezvous leg, `bilateral-punch-ms` each punch leg's lifetime.
  Clamped to [2000, 30000]: below 2000 ms no leg completes a round trip
  to a distant server, and above 30000 ms the S3 orchestrator deadline
  is the more meaningful bound. Note that a terminal failure is still
  retried once with a fresh local port (S2), so the user-visible worst
  case is about twice this. (The relay leg this paragraph used to
  mention no longer exists — the S5 relay was removed; see
  docs/plan-netplay-connection.md §7.)

`netplay-direct-p2p-register-interval-ms` (int, default `5000`)
- Host liveness (S1): cadence of the host's persistent rendezvous
  re-REGISTER loop while the room code is displayed. Keeps the
  rendezvous session and the host->server NAT mapping alive for as
  long as the host is advertising. Floor-clamped to 1000 ms (the
  server rate-limits at 10 packets/s/IP). The whole loop is disabled
  by `netplay-direct-p2p-disable-bilateral`.

`netplay-direct-p2p-stun-keepalive-ms` (int, default `15000`)
- Host liveness (S1): cadence of the STUN rebind keepalive while
  waiting for a joiner. Refreshes the advertised NAT mapping and
  detects public-endpoint drift; a drift must be confirmed by two
  consecutive keepalives before the room code is re-encoded and
  re-displayed with an explicit "Network changed" status (a NAT that
  rebinds on every keepalive therefore never churns the code). The
  default sits below common NAT UDP idle timeouts (~30 s) so the
  mapping is held rather than lost. Set to `0` (or negative) to
  disable; values below 5000 ms are clamped up.

### Netplay tuning

`netplay-input-prediction-window` (int, default `8`)
- GekkoNet rollback prediction window — max frames the local sim is
  allowed to predict ahead of confirmed inputs and, on mispredict, the
  max rollback depth. Lower values cut worst-case resim CPU (linear in
  the window) at the cost of tolerating less input lag before the
  client falls back to stutter. Upstream GekkoNet default is 10; we
  use 8 by default tuned for stock-clock MiSTer (800 MHz, near-zero
  headroom above 60 fps).

`netplay-diag-enable` (bool, default `true`)
- Master gate for session-tagged diagnostics: per-session UUID, UTC
  timestamp, jitter, kbps_tx/rx heartbeat extras, and the
  post-disconnect desync state dump. Recording cost is paid in the
  production codepath regardless; this knob is an emergency mute for
  serial-console noise. Default `true` so a friend running a build for
  a shared session contributes useful logs without having to opt in.
- See [docs/netplay-diagnostics.md](netplay-diagnostics.md) for the
  full set of instrumentation, log formats, and dump file locations.

`netplay-sparse-effect-save-enabled` (bool, default `true`)
- Enable the Option A sparse effect-pool GameState save path. When on
  (the default), each rollback save serializes only the active slots
  of the effect work pool (frw[128]) and reconstructs canonical empty
  slots on load. Typical per-frame state size drops from ~247 KB to
  60–120 KB depending on activity, shrinking GekkoNet's per-save
  checksum/transmission/load memcpy proportionally. Cross-peer desync
  detection is unaffected (the focused checksum never walked frw[]).
  Set `false` to force the legacy full-state save path for A/B parity
  testing without rebuilding. Active-slot counts above the 70-slot
  ceiling are auto-handled: the save falls back to a full-state pack
  for that frame and emits a deduped warning. Empirical peak from the
  on-device Pk\<N\> peak watcher across full-roster super-art-heavy
  sessions was 57; the 70 ceiling is a deliberate ~1.23x margin.
