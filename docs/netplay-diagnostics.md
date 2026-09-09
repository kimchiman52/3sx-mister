# Netplay diagnostics

Instrumentation that lives in `src/netplay/` and `src/port/sdl/sdl_app.c`
to make mid-session disconnects diagnosable post-mortem. Designed for
near-zero per-frame cost on stock 800 MHz MiSTer.

## What it captures

When a netplay session is active, the game writes structured diagnostics
to two locations under the user's pref path (typically
`/media/fat/games/3s-arm/` on MiSTer):

- **`logs/netplay-<utc_ms>.log`** — buffered text log of all
  `[netplay sess=...]` events and per-second heartbeats. Opened once at
  session start, fflush'd at 1 Hz from the heartbeat, closed on session
  end. Survives wrapper-SIGTERM via the `Netplay_FlushDiagnostics` hook.
- **`states/netplay_packet_ring_<utc_ms>_<role>.txt`** — dump of the
  last 512 packet events (send + receive, with timestamp, type, length,
  and peer endpoint) at the moment of disconnect, desync, or SIGTERM.

The session UUID (`[netplay sess=<8 hex>]`) appears on every relevant
log line so two MiSTers' logs can be cross-correlated by session.
Timestamps are wall-clock UTC ms via `clock_gettime(CLOCK_REALTIME)` so
events from both peers align directly.

## Heartbeat line format

Emitted once per ~60 frames (1 Hz) via the existing `[netplay hb]` log
prefix:

```
[netplay hb] f=12345 ping=82 jitter=11 kbps_tx=4.2 kbps_rx=4.1
             rb=2 behind=-0.7
             tx=I:60,A:60,SH:60,NH:2 rx=I:60,A:60,SH:60,NH:2
             rb_hist=0:55,1:3,2:2,3:0,4:0
             holds=1 catchups=2 last_hold_f=12301
```

Fields:

- `f` — current local frame
- `ping`, `jitter` — GekkoNet's avg_ping / jitter (ms)
- `kbps_tx`, `kbps_rx` — current 1-second TX/RX rate (KiB/s). Per
  `third_party/GekkoNet/build/include/net.h:136-137` these are
  per-second rates, not cumulative
- `rb` — rollback frames in the most recent batch
- `behind` — how far the local sim is behind real-time (frames)
- `tx=I:n,A:n,SH:n,NH:n` / `rx=...` — count of each GekkoNet packet type
  sent/received in the last second:
  - `I` = Inputs
  - `A` = InputAck
  - `SH` = SessionHealth
  - `NH` = NetworkHealth
- `rb_hist=0:n,1:n,2:n,3:n,4:n` — rollback-depth histogram bucketed as
  {0, 1-2, 3-4, 5-7, 8+}. Empty Gekko updates are excluded from bucket 0.
- `holds` — outer frames in this one-second window where Gekko produced no
  drawable non-rollback advance. Those frames retain the last complete canvas.
- `catchups` — outer frames that ran the two-update catch-up path.
- `last_hold_f` — most recent completed simulation frame retained by a hold.

The first frame of each consecutive no-draw episode also emits:

```
[netplay sess=abcd1234] no-draw hold f=12301 behind=0.4 catchup=0
                        events=0 advances=0 rollback_adv=0 loads=0 saves=0
```

This is the direct witness for the former one-frame black flash: the software
renderer drains that tick's queued geometry and presents its unchanged canvas.
The retained canvas includes the prior frame's netplay overlays.

A `Netplay_Run` call over 50 ms emits `[netplay-perf ...]`, splitting the old
single update/engine bucket into session polling, `gekko_update_session`, load,
advance, and save time, with event counts and catch-up/hold state. Pair it with
the adjacent `[step0]` and `FRAME OUTLIER` lines to separate network/Gekko,
rollback state work, game simulation, and LDREQ/storage stalls.

The packet-type counts are the highest-signal diagnostic: when a session
goes silent, comparing tx vs rx counts tells you which direction stopped.
"GekkoNet starved", "OS dropped", and "peer-side died" each have a
different signature.

## Watchdog

When the GekkoNet sim hasn't produced a `GekkoAdvanceEvent` for >250 ms
(while the session is RUNNING), a single line is emitted:

```
[netplay sess=abcd1234] WATCHDOG no-advance for 312 ms
                        last_frame=61961 frames_behind=-0.9 recent_rb=3
```

The latch resets on the next advance, so a flapping connection can emit
the line again. This fires *before* GekkoNet's hardcoded 5-second
disconnect timeout, so you see the stall while it's happening.

## Kernel UDP-stack snapshot

Captured at session start and again at session end / on peer-disconnect.
The delta is logged as:

```
[netplay sess=abcd1234] kernel-udp-stats:
  in_errors=0 rcvbuf_errs=0 noport_errs=0
```

These come from `/proc/net/snmp` and reflect kernel-attributed packet
drops. Non-zero values point at OS-level loss (typically
`rcvbuf_errs > 0` means the receive buffer overflowed).

## Packet ring dump format

When `event=peer-disconnected`, `event=desync`, or `Netplay_FlushDiagnostics`
fires, the last 512 send/receive events are dumped newest-to-oldest to
`states/netplay_packet_ring_<utc_ms>_<role>.txt`. `<role>` is one of:

- `host` / `joiner` — peer-disconnected dump
- `host-desync` / `joiner-desync` — desync dump
- `host-sigterm` / `joiner-sigterm` — wrapper-SIGTERM dump

Each line:

```
<utc_ms> <dir> type=<I|A|SH|NH|?> len=<bytes> peer=<ip>:<port>
```

Read backwards from the end to see the moments leading up to the failure.
A 5-second silence appears as a gap in timestamps; a one-direction
failure appears as `dir=tx` continuing while `dir=rx` falls silent.

## Cost

All hot-path additions are O(1):

- Send/recv counters: one array store per packet
- Packet ring: one struct write + index increment per packet
- Watchdog: one timestamp comparison per `process_session` poll
- Histogram: one bucket increment per `process_events` batch
- No-draw/catch-up counters: integer increments; timing uses monotonic reads
  around the existing Netplay/Gekko operations and only formats a line above
  the existing 50 ms outlier threshold

No malloc on the hot path. No syscalls on the hot path (timestamps come
from `clock_gettime` via vDSO; address parsing is cached on peer-change).
String formatting only happens at the heartbeat (1 Hz) and at dump
time (one-shot per disconnect). Disk I/O steady-state is one
`fflush` per second.

## Disabling

Set `netplay-diag-enable = false` in `<pref>/config` to silence the
verbose heartbeat format and skip the desync state dump. The packet
ring, watchdog, kernel-UDP-stats, and the netplay log file are
unconditional — they don't gate on `netplay-diag-enable` because their
diagnostic value vastly outweighs their cost.

## Texture-load skip/trace markers

Two log markers in `last-run.log` (or `backend.log`) come from
`src/sf33rd/Source/Game/rendering/texgroup.c`:

- `[texgroup-skip] ...` — emitted when a texture-load state-machine
  invariant is violated (originally a `while(1){}` arcade-source
  assert that hung the main thread). The load is now skipped with
  `curr->be = 2` and the game keeps advancing. Always on. If you see
  this in the wild, the load did NOT complete — downstream rendering
  may show a missing or stale texture, but the process won't freeze.
  The dup-transfer skip prints `key`, `type`, `id`, `ix`, `apfn`,
  `kokey` so you can correlate with the asset that failed.

- `[texgroup-trace] case=N key=... id=... apfn=...` — gated behind
  `ENABLE_PERF_TELEMETRY` (i.e., on for every telemetry-flavor build,
  off for clean). Prints the state-machine case (3/4/5) on every entry
  to the inner switch in `q_ldreq_texture_group`. Use to capture the
  transition history leading up to a `[texgroup-skip]`.

- `[ramcnt-skip] <func> ...` — gated behind `ENABLE_PERF_TELEMETRY`.
  Emitted from `src/sf33rd/Source/Game/system/ramcnt.c` when a memory-
  key invariant is violated (originally seven `while(1){}` arcade-
  source asserts that hung the main thread). The diagnostic always
  follows the original Japanese arcade error string (e.g. "TEXCASH
  KEY PUSH ERROR", "メモリの確保に失敗しました。") and reports the
  function name plus the locals that drove the trip — `key`, `type`,
  `rckeyctr`, `memreq`, `kokey`, `group`, etc. Affected functions
  return a graceful sentinel (`-1` for `Pull_ramcnt_key`, `0` for
  `Get_ramcnt_address` / `Get_size_data_ramcnt_key`, plain `return;`
  for the void variants). Callers still have to reject those sentinels before
  dereferencing them. The stage-background loader now preflights its type-0x12
  source before mutating render state; `[bg-texture-skip]` means a legitimate
  cache teardown was observed after that source had already been purged.

- `[ppgfile-skip] <func> palette-load-failed total=... c_mode=... koCmpr=...` —
  gated behind `ENABLE_PERF_TELEMETRY`. Emitted from
  `src/sf33rd/Source/Common/PPGFile.c` when `ppgSetupPalChunk` enters
  its `error_handler` (palette malloc / decompress / handle-create
  failure). Originally a `while(1){}` arcade-source assert at the end
  of `error_handler` that hung the main thread silently when the
  upstream condition didn't itself log. Now returns `-1` so callers
  fall through; rendering may show a missing palette but the process
  won't freeze. The other 16 PPGFile traps are not yet replaced — most
  have a Japanese arcade error string immediately above them, so a
  hang there will still produce a Japanese `flLogOut` line as the last
  log entry before the freeze.

- `[gd3rd-skip] <func> ...` — gated behind `ENABLE_PERF_TELEMETRY`.
  Emitted from `src/sf33rd/Source/Game/io/gd3rd.c` in
  `load_it_use_any_key2` when (a) the requested AFS file number is out
  of range (originally a silent `while(1){}` after the Japanese log
  "ファイルナンバーに異常があります"), or (b) `Pull_ramcnt_key`
  returned -1 — the new guard prevents the bad key from feeding into
  `Get_ramcnt_address` / `load_it_use_this_key` which otherwise
  dereference at addr 0. Function returns 0 (load-failed) in both
  cases.

- `[mtrans-skip] patcash_acquire ...` — gated behind
  `ENABLE_PERF_TELEMETRY`. Emitted from
  `src/sf33rd/Source/Game/rendering/mtrans.c` when the 64-entry ext
  pattern collection cannot supply an instance, in two flavours:
  `live-list-full kazu=64` (the live list is at capacity) and
  `no-dead-instance kazu=N` (nothing is expired). Pre-fix there was no
  guard at all: the append ran unchecked into `adr[64]`, which is
  `&patt[0]` to the byte on both ABIs, and `get_free_patcash_index`
  handed back a still-live `patt[0]` on exhaustion. The result was a
  wild pointer in the live list that `texture_cash_update()` then
  wrote through, plus a permanent x16/x32 slot leak. The sprite is now
  dropped for one frame instead. If you see this in the wild, one
  character or effect vanished for a frame — and something is putting
  65 distinct cg codes on one texture cache, which is worth chasing.

- `[texcash-skip] <func> ...` — gated behind `ENABLE_PERF_TELEMETRY`.
  Emitted from `src/sf33rd/Source/Game/rendering/texcash.c` for the
  three arcade `do{...}while(1)` traps in the ext cache:
  * `x16-refcount-underflow` / `x32-refcount-underflow slot=N time=-1
    ... (clamped to 0)` — a slot's refcount went negative, i.e. it was
    released more times than it was acquired. Was `CACHE MISS x16/x32`
    plus an infinite loop. The slot is clamped to the fully-released
    value 0 and the release completes.
  * `mapping-miss num=N i=N map16=... cp16=... (resynced from map;
    release proceeds)` — an expiring instance's `x16`/`x32` tally
    disagrees with its own `map`. Was `MAPPING MISS` plus an infinite
    loop. The tally is restored from the map (the map is the
    authoritative record of what was acquired) and the release still
    runs, deliberately: skipping it would strand every slot the
    instance holds.
  `Debug_w[11]` is still set in all three cases, so the on-screen
  free-area report and `search_texcash_free_area()` keep reporting.
  `--test-texcash-bounds` (Debug build with `-DENABLE_NETPLAY_TESTS`)
  exercises all three guards and their neutralizations.

## MIST handshake version gate — what it does and does not guarantee

Before GekkoNet starts, peers exchange a `MIST` hello carrying
`proto_ver` and `state_ver = sizeof(GameState)` plus the build's git
short hash (`src/netplay/mist_handshake.h`, R-1). A `state_ver` or
`proto_ver` mismatch hard-rejects the session with an on-screen reason;
a `build_hash` difference only logs a WARNING.

**Guaranteed rejected:** any pair whose `GameState` size differs — every
layout re-pin (field added/removed/retyped) — and every pre-R-1 build
(classified legacy).

**NOT caught (deliberate residual):** builds that differ without
changing `sizeof(GameState)` —

- sim-logic changes with no state-field change (balance tweaks, engine
  branch fixes),
- same-size field reorders/type swaps inside `GameState`,
- save/load format changes that leave the struct untouched (e.g. a
  `SPARSE_CEILING_SLOTS` divergence in the sparse effect-pool format).

Such pairs connect with only the build-hash WARNING in the log and can
still desync mid-match; runtime desync detection (always on) is the
backstop that catches them. Rejecting on `build_hash` instead would
block every rebuild — including provably-compatible ones — from playing
each other, which is worse for self-built peers than tolerating the
rare silently-incompatible pair. When shipping a known same-size
incompatibility, bump `MIST_PROTO_VER` to force the reject.

## Audio log noise suppression

`SDL_SetLogPriority(SDL_LOG_CATEGORY_AUDIO, SDL_LOG_PRIORITY_CRITICAL)`
is called after `SDL_Init` to suppress ~100 ALSA underrun lines per
session that come from libasound via SDL3, not from our code. Real
audio init failures still surface via the non-AUDIO-category SDL_Log
calls.
