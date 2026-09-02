#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

#include <stdbool.h>

#define CFG_KEY_FULLSCREEN "fullscreen"
#define CFG_KEY_WINDOW_WIDTH "window-width"
#define CFG_KEY_WINDOW_HEIGHT "window-height"
#define CFG_KEY_SCALEMODE "scale-mode"
#define CFG_KEY_SCANLINES "scanlines"
#define CFG_DRAW_PLAYERS_ABOVE_HUD "draw-players-above-hud"
/* Balance override. "auto" (default): arcade balance auto-selects at boot
 * when the CPS3 ROM is present and the full character adaptation succeeds,
 * PS2 otherwise. "ps2": force PS2 balance even with a valid ROM. This is a
 * config-file-only knob — no OSD surface. (Replaces the removed
 * "arcade-balance" bool toggle; stale arcade-balance lines in existing
 * configs are ignored.) */
#define CFG_KEY_BALANCE "balance"
#define CFG_KEY_BGM_TYPE "bgm-type"
/* In-game text language. "auto" (default): keep whatever the settings save
 * file holds, or — with no save yet — Get_Default_Language()'s locale-derived
 * pick (src/main.c). "english"/"japanese": force that language at boot, over
 * the settings save. Surfaced on MiSTer as the OSD Language option (status
 * bit [47]); the in-game Screen Adjust row writes this key back so the two
 * never drift (src/port/config/language.c). */
#define CFG_KEY_LANGUAGE "language"
#define CFG_KEY_SOFTWARE_FRAME_MODE "software-frame-mode"
#define CFG_KEY_SUPER_EFFECT_QUALITY "super-effect-quality"
#define CFG_KEY_SHOW_FPS "show-fps"
#define CFG_KEY_VIDEO_DRIVER_ORDER "video-driver-order"
#define CFG_KEY_RENDER_DRIVER_ORDER "render-driver-order"
#define CFG_KEY_ARM_CLOCK "arm-clock"
#define CFG_KEY_GAME_MODE "game-mode"
#define CFG_KEY_HOLD_TO_PAUSE "hold-to-pause"
#define CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED "colorkey-loose-kernel-enabled"
#define CFG_KEY_RGB565_CANVAS_ENABLED "rgb565-canvas-enabled"
/* perf-3 piece-B: 8-pixel packed-store INDEX8->565 kernel kill switch. */
#define CFG_KEY_SOFTWARE_PALETTE_PACKED_8PX_ENABLED "software-palette-packed-8px-enabled"
/* perf-3 piece-C: 16-pixel NEON 565 kernels kill switch. */
#define CFG_KEY_SOFTWARE_NEON_16PX_ENABLED "software-neon-16px-enabled"

/* Direct-P2P (docs/plan-stun-direct-p2p.md Step 5). HOST_PORT == 0 means
 * OS-assigned; DISABLE_UPNP skips the UPnP first-try path; LAST_PEER_CODE
 * is populated at runtime on successful Join; HANDOFF_PATH is the file
 * the wrapper writes into before execve'ing the game (Step 9/11); STUN_
 * TIMEOUT_MS clamps the orchestrator's STUN discovery budget. */
#define CFG_KEY_NETPLAY_DIRECT_P2P_HOST_PORT "netplay-direct-p2p-host-port"
#define CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP "netplay-direct-p2p-disable-upnp"
#define CFG_KEY_NETPLAY_DIRECT_P2P_LAST_PEER_CODE "netplay-direct-p2p-last-peer-code"
#define CFG_KEY_NETPLAY_DIRECT_P2P_HANDOFF_PATH "netplay-direct-p2p-handoff-path"
#define CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS "netplay-direct-p2p-stun-timeout-ms"

/* S7 (docs/plan-netplay-connection.md §9): the NAT-PMP/PCP backend gets
 * its OWN kill switch rather than riding DISABLE_UPNP.
 *
 * DISABLE_UPNP exists for one specific defect — libminiupnpc 2.2.1's
 * upnpDiscover() segfaulting on MiSTer when a Realtek 8821cu USB WiFi
 * adapter sits alongside eth0 (direct_p2p.c try_portmap). A user who
 * sets it to dodge that crash still wants a port mapping, and NAT-PMP/
 * PCP is a completely separate ~60-byte UDP client with no shared code
 * — folding the two switches together would take that mapping away for
 * no reason. Conversely a router whose NAT-PMP implementation misbehaves
 * needs to be turned off WITHOUT losing UPnP. Bool, default false. */
#define CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP "netplay-direct-p2p-disable-natpmp"

/* Bilateral hole-punch fallback (docs/plan-bilateral-hole-punch.md §Decision 6).
 * DISABLE_BILATERAL is a kill switch back to today's FAILED_SYMMETRIC behavior;
 * SIGNAL_URL points at the rendezvous server (udp://host:port form, placeholder
 * hostname until Step 1 infrastructure lands); SIGNAL_BUDGET_MS bounds the
 * REGISTER/POLL phase; BILATERAL_PUNCH_MS sizes the second Stun_HolePunch
 * window (longer than the initial direct punch to absorb post-signaling
 * clock skew between peers). */
#define CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL "netplay-direct-p2p-disable-bilateral"
#define CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL "netplay-direct-p2p-signal-url"
#define CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_BUDGET_MS "netplay-direct-p2p-signal-budget-ms"
#define CFG_KEY_NETPLAY_DIRECT_P2P_BILATERAL_PUNCH_MS "netplay-direct-p2p-bilateral-punch-ms"

/* Host liveness (docs/plan-netplay-connection.md S1). REGISTER_INTERVAL_MS
 * is the cadence of the host's persistent rendezvous re-REGISTER loop —
 * the host re-REGISTERs for the entire time it displays a room code so
 * the rendezvous session and the host's NAT mappings stay alive while
 * the code is shared out-of-band (minutes, not the old 8 s budget).
 * Floor-clamped to 1000 ms (server rate limit is 10 pkts/s/IP).
 * STUN_KEEPALIVE_MS is the cadence of the host's STUN rebind keepalive
 * while HOST_WAITING — refreshes the advertised NAT mapping and detects
 * public-endpoint drift (<= 0 disables). Both sit under the existing
 * DISABLE_BILATERAL kill switch (the rendezvous loop) / HOST_WAITING
 * lifecycle (the keepalive). */
#define CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS "netplay-direct-p2p-register-interval-ms"
#define CFG_KEY_NETPLAY_DIRECT_P2P_STUN_KEEPALIVE_MS "netplay-direct-p2p-stun-keepalive-ms"

/* Task #119: kill switch for the late-punch rescue layer (late_punch.h)
 * — the connected side answering token-authenticated punches between
 * handoff and GekkoSessionStarted, so a one-sided race outcome degrades
 * to a delayed connect instead of a 15 s hang + burned room code. Bool,
 * default false (layer ON). Checked once, at arm time
 * (Netplay_TickDirectP2P); flipping it restores the pre-#119 window
 * behaviour exactly, which is what makes a field regression
 * attributable to this layer in one config flip. */
#define CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_LATE_PUNCH "netplay-direct-p2p-disable-late-punch"

/* NOTE: the three S5 relay keys that used to live here
 * (netplay-direct-p2p-disable-relay / -force-relay / -relay-budget-ms)
 * were REMOVED with the relay rung itself. dict_iterator() in config.c
 * stores every key it reads without checking it against default_entries,
 * and nothing rejects an unknown key, so a config file still carrying
 * those three lines parses cleanly — they are simply never read. */

/* S6 candidate racing (docs/plan-netplay-connection.md §8).
 *
 * RACE_BUDGET_MS is the WHOLE post-STUN establishment wall clock on both
 * roles — the punch legs and the rendezvous signaling leg run
 * CONCURRENTLY inside it instead of one after another, so it replaces
 * the old serial sum (direct punch + signal budget + bilateral punch) as
 * the thing that bounds a failing attempt. The
 * per-leg keys above still bound their own legs INSIDE this budget.
 * Clamped [2000, 30000]: below ~2 s no leg can complete a round trip to
 * a distant server, and above 30 s the S3 orchestrator deadline is the
 * more meaningful bound. */
#define CFG_KEY_NETPLAY_DIRECT_P2P_RACE_BUDGET_MS "netplay-direct-p2p-race-budget-ms"

/* Rollback prediction window — max frames Gekko will predict ahead of
 * confirmed inputs and, on mispredict, the max rollback depth. Lower
 * values reduce worst-case resim CPU cost (linear in window) at the cost
 * of tolerating less input lag before dropping to stutter. 10 was the
 * upstream GekkoNet default; 6 is our default tuned for MiSTer stock
 * 800 MHz, which has near-zero headroom above 60 fps. */
#define CFG_KEY_NETPLAY_INPUT_PREDICTION_WINDOW "netplay-input-prediction-window"

/* Netplay diagnostics — session-tagged heartbeat extras (UUID, UTC, jitter,
 * cumulative kb_sent/recv) and the post-desync state dump. Recording cost is
 * paid in the production codepath regardless; this gate is an emergency mute
 * for serial-console noise. Default true so a friend running a build for a
 * shared session contributes useful logs without having to opt in. */
#define CFG_KEY_NETPLAY_DIAG_ENABLE "netplay-diag-enable"

/* Sparse effect-pool save (Option A). Default true — only off for A/B
 * parity-testing. When false, save_state writes a full sizeof(State)
 * blob (the legacy path), bypassing the sparse encoding entirely.
 * load_state auto-detects format by buffer length so flipping the flag
 * mid-session is safe (although not exposed via the in-game UI). */
#define CFG_KEY_NETPLAY_SPARSE_EFFECT_SAVE_ENABLED "netplay-sparse-effect-save-enabled"

/* Root directory of the cached `.3sr` replay set (flat + one level of
 * subdirs). Formerly `replay-browser-root`, renamed when the pad-driven
 * in-game browser was cut; the shuffle viewer that replaces it enumerates
 * the same directory. The default lives in config.c (DEFAULT_REPLAYS_ROOT).
 *
 * KEEP IN STEP: the HPS OSD wrapper has its own hardcoded copy of this path
 * (REPLAY_LOCAL_ROOT in tools/mister-wrapper/main-mister-full-menu.patch).
 * Changing the MiSTer value here without changing that literal silently
 * splits the game and the OSD onto two different replay directories. */
#define CFG_KEY_REPLAYS_ROOT "replays-root"

/* Replay storage lifecycle (docs/plan-fcade-replay-browser.md Step F4/Step 3).
 * REPLAYS_MAX_MB caps the total size, in megabytes, of raw Fightcade-fetch
 * stream payloads (frames.bin/inputs/savestate/summary.json — see
 * src/replay/replay_storage.h) kept under the replays root. When the
 * cap is exceeded, the oldest (LRU by mtime) fetch directories have their raw
 * files evicted first; `.3sr` and `.meta.json` are never touched by
 * eviction. 0 disables eviction entirely (delete-only). Default 200 MB. */
#define CFG_KEY_REPLAYS_MAX_MB "replays-max-mb"

/* Remote replay browse over the VPS fcade-proxy (tools/fcade-proxy), spoken
 * as plain TCP. REPLAY_PROXY_HOST is the proxy host/IP; "" (the default)
 * leaves remote browsing DISABLED so a no-config boot is local-only.
 * REPLAY_PROXY_PORT is the proxy's TCP port (proxy default 3479).
 *
 * DO NOT DELETE THESE AS "UNUSED". Nothing under src/ reads either key —
 * the consumer is the HPS OSD wrapper, not the game. config.c's
 * write_defaults() seeds the on-device config file from default_entries[],
 * and vendor/Main_MiSTer/replay_proxy.c -> RpConfigLoadFrom() parses that
 * same file looking for exactly the two literal key names below. Dropping
 * either row silently breaks the wrapper's proxy configuration with no
 * game-side symptom. See docs/config.md. */
#define CFG_KEY_REPLAY_PROXY_HOST "replay-proxy-host"
#define CFG_KEY_REPLAY_PROXY_PORT "replay-proxy-port"

/// Initialize config system
void Config_Init();

/// Destroy resources used by config system
void Config_Destroy();

/// Get the value associated with the given key as a `bool`
/// @return The value associated with `key` if `key` is among entries and the value's type is `bool`, `false` otherwise
bool Config_GetBool(const char* key);

/// Get the value associated with the given key as an `int`
/// @return The value associated with `key` if `key` is among entries and the value's type is `int`, `0` otherwise
int Config_GetInt(const char* key);

/// Get the value associated with the given key as a `string`
/// @return The value associated with `key` if `key` is among entries and the value's type is `string`, `NULL` otherwise
const char* Config_GetString(const char* key);

/// Check whether the config file explicitly provided the given key
bool Config_HasExplicitKey(const char* key);

/// Set (or update) the string value associated with the given key in the
/// in-memory entries table.
/// NOTE: the update lives only in memory until Config_Save() is called;
/// Config_Save() is a minimal stub today (see config.c) and does not yet
/// rewrite config.ini on disk. Direct-P2P runtime keys (HOST_PORT,
/// LAST_PEER_CODE) use this path for within-session updates.
void Config_SetString(const char* key, const char* value);

/* Bool sibling of Config_SetString, same in-memory-only semantics.
 * Exists because several netplay kill switches are CFG_BOOL and
 * Config_GetBool returns false for any entry whose type is not CFG_BOOL
 * — so a test (or any runtime toggle) could NOT flip them via
 * Config_SetString, which would install a CFG_STRING entry that
 * Config_GetBool then silently ignores. Notably
 * CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP has no default_entries[] row,
 * so there is no coercion path either. */
void Config_SetBool(const char* key, bool value);

/// Persist the current in-memory config entries to the on-disk config file.
/// See note on Config_SetString — current implementation is a no-op that
/// logs once so callers have a stable symbol to call; real write-back is
/// deferred until a caller actually needs it.
void Config_Save(void);

#endif
