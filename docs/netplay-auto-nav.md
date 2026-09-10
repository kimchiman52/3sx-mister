# Netplay Auto-Navigation

`src/netplay/netplay_nav.{c,h}` — state machine that drives the game through the natural console-mode menu path (Title → Mode Select → Versus) before starting a netplay session. Introduced in commit `2cbc249b`.

## Why it exists

Before this module, cold-launched netplay (`--direct-p2p-handoff` or `--p2p-remote-ip`) hard-jumped the game engine straight to character select via `setup_vs_mode()` in `src/netplay/netplay.c`. Gameplay worked but **character select and VS pre-match screens rendered only the background** — no character portraits, cursors, or UI. The hard jump skipped the long side-effect chain that `Game0_2 → Loop_Demo/Ck_Coin → Entry_01 → Menu_Task → Mode_Select case 1` normally runs: texture teardown/reload, BGM switch, effect-pool priming.

Rather than manually replicate that chain (whack-a-mole), nav synthesizes `SWK_START` button presses at the right transitions so the game runs its own menu code path for real. `Netplay_BeginDirectP2P()` is only called after char select has been reached through the real flow.

## State machine

```
NAV_IDLE
  └─> NetplayNav_Arm() when p2p_remote_ip set, or from set_netplay_params on
      the orchestrator path
NAV_WAIT_INIT         wait for task[TASK_INIT].condition == 0
NAV_PRESS_COIN        inject SWK_START while G_No[0]==1 (Loop_Demo)
                      → Ck_Coin → Next_Title_Sub → G_No[0]=2, TASK_ENTRY
NAV_PRESS_TITLE       inject SWK_START while Entry_01 is polling
                      → Entry_01_Sub → Request_G_No=1 → Game0_2 runs cases 0..5
                      → case 5 registers TASK_MENU, sets G_No[1]=12
NAV_WAIT_MENU         wait for Menu_Task → After_Title → Mode_Select (case 3)
                      (checks task[TASK_MENU].condition==1 and r_no[0..2])
NAV_DRIVE_VS          force Menu_Cursor_Y[0]=1 (Versus row), pin
                      Interface_Type[0..1]=2 so Connect_Status=1 (skips
                      menu.c:403 cursor-coerce-off-Versus), inject Start;
                      on menu exit apply Mode_Type=MODE_NETWORK override
NAV_WAIT_ORCHESTRATOR wait for Netplay_IsRemoteIpSet() (remote_ip populated
                      by either LAN path or do_handoff() after STUN/UPnP)
NAV_START_NETPLAY     call Netplay_BeginDirectP2P(); setup_vs_mode runs next
                      tick, now on a menu-primed game state
NAV_DONE              one-shot; Tick is a no-op. Real player input is never
                      injected from this point.
```

Each state has a ~10-second safety timeout that falls through to the next state. Timeout paths still reach `NAV_DONE` (better than deadlocking) but char select would render wrong on that peer. The nominal path never uses a timeout.

## Public API

```c
void NetplayNav_Arm(void);       // Arms the state machine. Idempotent.
void NetplayNav_Tick(void);      // Every frame from game_step_0, BEFORE
                                 //   the p1sw_buff → p1sw_0 latch.
bool NetplayNav_IsActive(void);  // true while between NAV_IDLE and NAV_DONE.
void NetplayNav_Reset(void);     // Back to NAV_IDLE. Currently unused —
                                 //   wire into session teardown when
                                 //   multi-session reconnect lands.
```

## Identical-settings enforcement

Different peers may have different saved DIP-switch values (time limit, damage level, etc). `apply_network_mode_override()` pins `save_w[MODE_NETWORK]` to a canonical set on both peers:

- `Time_Limit = 99`
- `Battle_Number[0..1] = 1` (best-of-3 rounds)
- `Damage_Level = 1, Handicap = 0, GuardCheck = 0` (arcade-default damage;
  `Handicap = 0` skips the unused handicap-selection screen while the
  initialized `Vital_Handicap` values remain equal at 7/7)
- Identity `Pad_Infor[0..1].Shot[]`, vibration off

Without this, frame-0 state diverges between peers and Gekko's desync detection terminates the session immediately.

## Console mode is forced

`NetplayNav_Arm()` calls `SDLApp_ForceConsoleGameMode()` because arcade mode's `Loop_Demo` fallback inline-jumps to `G_No[1]=12, Mode_Type=MODE_ARCADE` without registering `TASK_MENU`, which would leave nav stuck in `NAV_WAIT_MENU` until timeout. Console mode always goes through `Game0_2 → TASK_MENU → Mode_Select`, which is the path nav drives.

## Wiring

- `main.c set_netplay_params()`: calls `NetplayNav_Arm()` for both `p2p_remote_ip != NULL` (LAN / localhost) and the direct-P2P orchestrator paths. Not called on the matchmaking path (that drives its own menu flow).
- `main.c game_step_0()`: calls `NetplayNav_Tick()` every frame, placed after `keyConvert()` but before the `p1sw_0 = p1sw_buff` latch (~line 561) so injected presses land the same tick.
- `direct_p2p.c do_handoff()`: no longer calls `Netplay_BeginDirectP2P()`. It still does `Netplay_SetParams + SetRemotePort + SetStunSocket` to publish orchestrator state; nav fires the begin call from `NAV_START_NETPLAY` once `Netplay_IsRemoteIpSet()` returns true.

## When not to use nav

- Matchmaking path: driven by the matchmaking server's own menu flow. Arm is skipped there.
- Menu-driven netplay entry (if re-enabled): if the user is already in the menu and selects a Network option, nav must not run — the menu itself drives the handoff.
- Future session reconnect: `NetplayNav_Reset()` + re-arm would be needed. Not wired yet.

## Test harness

Two local Mac instances:

```
cd "/Users/sb/Library/Application Support/CrowdedStreet/3S-ARM"
/path/to/3S-ARM --p2p-remote-ip=127.0.0.1 --p2p-local-player=1 &
/path/to/3S-ARM --p2p-remote-ip=127.0.0.1 --p2p-local-player=2 &
```

Expect both instances to show `[netplay_nav] state X -> Y` transitions through the full chain and land at `NAV_DONE`, followed by `starting a session` and `🔴 session started`. Session should stay healthy (0 desync, 0 dropped packets, 0 disconnects) for 60+ seconds while both peers sit in char select.

## Related

- `docs/plan-stun-direct-p2p.md` — the plan that introduced the orchestrator this nav module sits on top of.
- `docs/archive/STUN-PORT-STATUS.md` — orchestrator status snapshot.
- Commit `42347261` — the three earlier fixes (TASK_INIT guard, `Netplay_SetRemotePort`, `remote_ip_buf`) this module depends on.
