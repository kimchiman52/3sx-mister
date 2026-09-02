# Work queue — TEMPORARY, dated 2026-08-30

> **DELETE THIS FILE WHEN THE QUEUE IS EMPTY.** It exists only because the
> session's task tooling was withdrawn mid-flight and the queue had nowhere
> else to live. A stale tasklist checked into a repo is worse than none: it
> reads as current long after it stops being true.
>
> **Staleness test, runnable:** every line number below was verified against
> `dcf7631a` on `upstream-engine-fixes`. Run
> `git log --oneline dcf7631a..HEAD -- src/netplay tools/rendezvous-server`
> — if that prints anything, re-verify before trusting a citation here.
> If every entry is closed, `git rm docs/queue.md`.

Findings and citations only. No narrative, no status prose.

---

## #130 — slot-reclaim staleness — CLOSED

Both port-reclaim arms now carry a staleness precondition:
`tools/rendezvous-server/rendezvous-server.js:1362` (slot B, `slotReclaimable(entry, 'B', now)`)
and `:1379` (slot A).

The threshold is a multiple of the slot's **own observed cadence**, not a fixed
window — `slotReclaimIdleMs` (`rendezvous-server.js:739`), measured by
`touchSlot` (`:692`), reset on repoint by `seatSlot` (`:713`).

**No fixed window exists.** Protecting a live host slot with one missed-refresh
of margin needs `W > 10000` (host advertise default 5000 ms,
`src/port/config/config.c:111`); preserving #105 needs `W < 8000` (the
attempt-2 signalling leg, `src/port/config/config.c:105`, ended at
`src/netplay/direct_p2p.c:1958-1959`). The interval is empty.

`PORT_RECLAIM_MISSED_REFRESHES = 6` (`rendezvous-server.js:262`) is not a new
constant: it is `SLOT_STALE_MS / host cadence` = 30000/5000, i.e. the "six
missed refreshes" standard `SLOT_STALE_MS` (`:109`) already encodes, re-expressed
in units of the slot that is actually being reclaimed. Yields:

- joiner-cadence slot (~500 ms) → 3000 ms; fits the 8000 ms retry leg
- host-cadence slot (~5000 ms) → 30000 ms = `SLOT_STALE_MS`; the arms grant
  nothing over host-cadence slots that the pre-existing stale arm did not
- unobserved cadence → `SLOT_STALE_MS` (never 0)

Enforced, not asserted in prose: `tools/rendezvous-server/check_reclaim_window.py`,
gate `reclaim-window` in `tools/gates/run-gates.sh`. Client and server deploy
independently, so no `_Static_assert` or JS assertion can see both sides — same
coupling and same remedy as #123's `check_key_rate_budget.py`.

The understated comment at the old `:160-164` is replaced by
`rendezvous-server.js:160-188` ("THE RESIDUAL, CORRECTED").

**The two 8-caps were NOT linked, with evidence.** `MAX_PORT_RECLAIMS`
(`rendezvous-server.js:194`) and `LATE_PUNCH_MAX_RELEARNS`
(`src/netplay/late_punch.h:190`) cannot compose: a rendezvous DELIVER cannot
reach the late-punch layer at all. `src/netplay/sdl_net_adapter.c:368-373`
destroys every `Rendezvous_HasMagic` datagram before the late-punch call at
`:387-389`, and `LatePunch_HandleDatagram` itself returns false for anything
failing `Stun_HasPunchPrefix` (`src/netplay/late_punch.c:217-219`). A relearn
requires an authenticated `3SX_PUNCH` datagram from the established peer IP on
a new port — the `Stun_IsPunchPayload` token / peer-IP / changed-port gate at
`late_punch.c:271-304`. Linking them would couple two counters
that share no state and no path.

Coverage: `__test_protocol.js` 39 tests (was 36) — `liveHostSlotNotHijackable`,
`unobservedSlotMaximallyProtected`, `portReclaimBudgetExhausted`;
`joinerPortReclaimSameIp` and `portReclaimSlotA` reworked to assert both
directions.

**That result stands, and answers a narrower question than the review asked.**
See #133 below: it settles composition, not separate exercise.

---

## #133 — both-caps, exercised SEPARATELY — SEVERITY ANSWERED: **TAKEOVER**. Mitigation **SHIPPED** (host-side; rig re-run pending).

**The severity analysis below stands as written and is the record of what was
wrong. The mitigation it recommended is now implemented — see "Mitigation —
IMPLEMENTED" at the end of this section for what changed, what it does and does
not buy, and what is still unproven.**

The question: can a same-public-IP room-code holder exercise `MAX_PORT_RECLAIMS`
(`rendezvous-server.js:194`) and `LATE_PUNCH_MAX_RELEARNS`
(`src/netplay/late_punch.h:190`) **separately** in one session? #130 proved only
that one datagram cannot drive both.

**Answer: yes, and the relearn grants a capability a plain room-code join does
not.** Verified:

- **The room code is sufficient to derive the punch token.** The token is
  `SHA-256("3SXR-PT3" || ip || port || nonce)[0..7]` (`src/netplay/rendezvous.c:146-152`,
  derivation `:86-133`), and all three inputs are carried *inside* the room code
  (`src/netplay/room_code.h:9`, `:129`, `:207`; the nonce is the low 32 bits,
  `room_code.h:231-236`). No wire observation and no server access is needed.
- **The caps sit in different session phases**, so they are independently
  reachable: a reclaim acts on the server during signalling, while late-punch is
  armed only *after* `do_handoff` (`src/netplay/netplay.c:2177-2181`).
- **Baseline — what the room code already authorises.** A holder from *any* IP
  can already become the peer: `classify_host_datagram` returns `PEER_PUNCH` on
  token match with no source-IP test (`src/netplay/direct_p2p.c:1065`), captured
  at `:5108-5121`. That is inside the trust boundary, as expected.
- **What exceeds it: post-commitment displacement.** After capture the host
  leaves `HOST_WAITING` (`direct_p2p.c:5118`) and `host_tick_receive` is
  reachable only from that state (`direct_p2p.c:5748-5761`) — so a racing loser
  normally gets no second attempt. `LatePunch_HandleDatagram` is the only path
  that re-points an already-committed peer, and its sole identity check is
  `strcmp(src_ip, s_peer_ip)` (`src/netplay/late_punch.c:280`) — an IP compare
  that **cannot distinguish "the peer's NAT mapping moved" from "a different
  host behind the same public IP"**. One relearn sets `remote_port` and calls
  `SDLNetAdapter_RetargetPeer` (`netplay.c:1419-1421`), after which every
  outbound session datagram goes to the attacker
  (`src/netplay/sdl_net_adapter.c:288-290`).
- **It survives into the match.** `LatePunch_Disarm` (`late_punch.c:187-206`)
  clears the token and arming but **never touches `s_actual_peer` /
  `s_retarget_active`**; only `SDLNetAdapter_SetCanonicalPeer`
  (`sdl_net_adapter.c:53-64`) and `SDLNetAdapter_Destroy` (`:465-466`) do.
- **No persistence past teardown** — teardown disarms and destroys the adapter,
  and the nonce re-rolls per hosting attempt (`direct_p2p.c:3740`). This half of
  the original expectation held.
- **One relearn is enough; the cap of 8 bounds flapping, not capability.**

Two comments were contradicted by the above. **Both corrected** (this commit):
`late_punch.h` now states that the relearn gate is narrower on source IP but
strictly *wider in time*, and that the relearn cap bounds flapping rather than
capability.

### The severity question — SETTLED: takeover, not DoS

Nothing on the path stops a substituted party that is running the same public
build with the same CPS3 ROM. Each downstream gate, read and (where a harness
exists) exercised:

- **MIST compat gate — passes, by construction.** `classify_peer_payload`
  (`src/netplay/mist_handshake.c:314`) accepts on exactly five fields: arch tag
  `"armv7"`, platform tag `"mister"`, `proto_ver`, `state_ver`, and
  `balance_digest`. `build_hash` is a **warning, never a reject**
  (`mist_handshake.c:378-383`). Every one of those five is a compile-time or
  ROM-derived constant of the shipped build; **none is derived from the room
  code, the punch token, the nonce, or the session**. Exercised:
  `--test-mist-compat-gate` `test1_accept` asserts "a different build_hash
  warns, it does not reject" and accepts a payload assembled byte-by-byte from
  public constants alone (`src/netplay/test_mist_compat_gate.c:225-259`).
  Harness green at this tip: **9 tests, 1294 assertions, 0 failures**.
- **Balance digest — derivable, not observed.** `ArcadeBalance_GetDigest()`
  (`src/arcade/arcade_balance.c:179`) returns `ArcadeCharData_ComputeDigest()`
  computed once at boot from the adapted CPS3 ROM data (`:152`). Same ROM
  revision → same digest for every player. It is not a secret and not
  session-bound.
- **Nothing source-gates the ack that completes the handshake.** In
  `mist_handshake_pump`, the `cls == 1` branch returns `MIST_PUMP_OK` with **no
  `from_peer` test** (`src/netplay/mist_handshake.c:743-771`); `from_peer` gates
  only the H-1 hello latch (`:835`) and implicit completion (`:854`). And after
  a relearn the substitute *is* `from_peer` anyway — `late_punch_service` sets
  `remote_port` (`netplay.c:1419`), which `mist_pump_start` copies into
  `io->peer_port` (`netplay.c:1339`) after the `s_hs_peer_refetch` re-resolve
  (`:1309-1314`).
- **GekkoNet authenticates nothing about the remote.** The remote is registered
  once by `"ip:port"` string (`netplay.c:1559-1561`) and matched by
  `GetRemoteHandlesForAddress` (`third_party/GekkoNet/build/include/backend.h:157`).
  The `u16 session magic` (`net.h:40`, `backend.h:76`, `:191`) is negotiated
  **in band** via SyncRequest/SyncResponse (`backend.h:151-153`) toward whatever
  endpoint we send to — which after a relearn is the substitute.
- **The desync checksum does not save us.** `GekkoDesyncDetected` does terminate
  the session (`netplay.c:1818-1851`) — but a substitute that is a real 3SX
  instance simulates the same game and produces matching checksums. The
  checksum only bites a substitute that *cannot* simulate; that variant is the
  DoS floor, not the ceiling.

**Two capture windows, both reachable.** `late_punch_service` runs in
TRANSITIONING (`netplay.c:2319`) and CONNECTING (`:2432`):

1. **Relearn in TRANSITIONING** — `remote_port` moves *before*
   `configure_gekko`, so `SDLNetAdapter_SetCanonicalPeer` (`netplay.c:1599`)
   registers **the substitute as the canonical peer outright**. The legitimate
   peer's datagrams then arrive under a non-canonical address string and
   GekkoNet ignores them. Clean substitution; MIST runs against the substitute.
2. **Relearn in CONNECTING** — after `configure_gekko`, so
   `SDLNetAdapter_RetargetPeer` sets `s_retarget_active`
   (`sdl_net_adapter.c:67-85`): all outbound goes to the substitute
   (`:288-290`) and its inbound is relabelled canonical (`:405`). The
   legitimate peer stops receiving our inputs and times out, leaving the
   substitute as the only live remote.

**The relearn cap is anti-mitigation.** Once `s_relearn_count` reaches
`LATE_PUNCH_MAX_RELEARNS` the endpoint is **frozen** and every later move is
refused (the pre-mitigation relearn-cap block in `late_punch.c`, deleted by
the fix; the refusal now lives in `late_punch_nominate`,
`src/netplay/late_punch.c:95-102`, and refuses at ONE). A same-IP party that
fires 8
token-valid punches from 8 different source ports therefore locks the send
target onto its own 8th port, and the legitimate peer — still punching from its
original mapping — can never be relearned back. The cap converts a flapping
contest into a **deterministic** capture.

**Window size:** up to `40 x 500 ms` of MIST retries
(`src/netplay/mist_handshake.h:250`) plus `CONNECT_TIMEOUT_CONNECTING_MS =
15000` (`src/netplay/connect_fail.h:359`) — roughly 35 s per session.

**What the attacker gains.** With the room code alone, no wire observation, no
server access, and no participation in the original race: they play the match
as the peer. The host believes it is playing its friend; the attacker's inputs
drive the host's rollback engine and the host's inputs are delivered to the
attacker. If the attacker cannot simulate the game, the same primitive is a
clean DoS — the room dies and the code is burned (the nonce re-rolls per
hosting attempt, `direct_p2p.c:3740`). The attacker picks which.

**Precondition, honestly stated.** The substitute's datagrams must reach the
host from a *new* source port on the shared public IP. That requires the host's
NAT to be full-cone or address-restricted; address-and-port-dependent filtering
(`tools/netplay/natmatrix/natns.sh:86`) admits only the exact punched
`ip:port`. **This is exactly the same precondition the feature itself needs** —
the S2 retry's fresh socket also arrives from a new port — so the attack
surface is coextensive with the relearn's working set. It cannot be narrowed by
NAT assumptions without also disabling the rescue.

### Could not verify

- **`_session_magic`'s derivation.** `third_party/GekkoNet` ships headers plus
  `build/lib/libGekkoNet.a` only (`find third_party/GekkoNet -type f` → 10
  files, no `.cpp`), so the seeding of `MessageSystem::_session_magic`
  (`backend.h:191`) could not be read. It cannot be a pre-shared secret with the
  legitimate peer — that peer learns it over the same wire — but the exact
  derivation is unread.
- **No live two-host exercise.** The MiSTer device lock is held by another
  session; nothing was run on hardware. The verdict is a code + unit-harness
  reading, not a demonstrated capture.
- The CGNAT / shared-NAT premise itself is a network property, taken as given.

### Mitigation — the options as weighed (the RECOMMENDED one is now built)

Weighed against the demonstrated availability win (late-punch converted a real
`FAILED_HANDSHAKE` into a connect at +11.4 s):

- **REJECTED — clear `s_actual_peer` / `s_retarget_active` in
  `LatePunch_Disarm`.** This does not work and would break the feature. Disarm
  fires at `GekkoSessionStarted` (`netplay.c:1827`); a window-2 relearn is
  applied to the adapter but *not* to GekkoNet's registration, so clearing the
  retarget at session start would send every packet back to the dead original
  port and kill exactly the session the rescue just saved. It also does not
  touch window 1, where the substitute is already canonical. Cross-session
  hygiene is a non-issue: `SDLNetAdapter_SetCanonicalPeer` (`netplay.c:1599`)
  re-seeds both fields on every `configure_gekko`.
- **REJECTED — default the kill switch on.** `CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_LATE_PUNCH`
  defaults to `false` (`src/port/config/config.c:122`). Flipping it trades a
  measured, every-session availability win against a same-IP-only risk, and
  restores the "15 s hang + dead room" failure the module exists to remove.
  Keep it as the field-attributable escape hatch it is.
- **RECOMMENDED — bind the relearn to something stronger than source IP, and
  make it once-and-done.** Two changes, both inside `late_punch.c`:
  1. **Lower `LATE_PUNCH_MAX_RELEARNS` to 1** (or gate the second and later
     moves behind evidence the first endpoint went quiet). One relearn covers
     the S2 retry, which is the only sequence the header claims as legitimate;
     it removes the 8-port cap-burn that makes capture deterministic, and it
     makes the *first* mover win rather than the last — the legitimate peer,
     which is already punching, rather than a party that has to arrive later.
  2. **Require the relearned endpoint to prove liveness before it becomes the
     send target** — a challenge/response over the existing token
     (nonce-in-punch, echoed) rather than accepting the first token-valid
     datagram from the IP. The token is derivable from the room code, so this
     does not authenticate identity; it does make capture require a live
     round-trip at the exact moment of the move rather than a one-shot spray.
  Neither weakens the rescue: a genuinely retrying joiner answers a challenge
  and moves once.

**The trade.** Keep late-punch on by default and keep its availability win; pay
for it by shrinking the relearn from "8 moves, last one wins, frozen forever" to
"one move, first one wins, liveness-checked". That preserves the +11.4 s rescue
in full — it needs exactly one move — while removing the property that makes the
same-IP capture deterministic rather than a coin flip. Full identity binding is
not available at this layer: the punch token is room-code-derived by design
(`room_code.h:221-236` calls the code key material), so a same-IP holder of the
code is inside the token's trust boundary no matter what this module does.

**No test distinguished the two same-IP cases** at the time this was written.
`src/netplay/test_punch_predicates.c` and `src/netplay/test_late_punch.c`
covered the cap and the foreign-IP refusal; none separated "the peer moved"
from "a second host on the same NAT", and an IP-string compare cannot. That is
now the property the liveness challenge tests, below.

### Mitigation — IMPLEMENTED

Both recommended changes, plus the responder they need on the other side.

**1. `LATE_PUNCH_MAX_RELEARNS` 8 -> 1** (`src/netplay/late_punch.h:163-200`).
The proven rescue needs exactly one move. Critically, the budget is now spent
by `late_punch_promote` (`late_punch.c:139-155`) — i.e. only by an endpoint
that ANSWERED — so an endpoint that merely sends cannot consume it. Without
that, a cap of 1 would be a one-datagram freeze, which is the old failure made
cheaper.

**2. A liveness round trip gates the retarget.** A token-valid punch from a new
port on the peer IP no longer moves anything; it calls `late_punch_nominate`
(`src/netplay/late_punch.c:304`, defined at `src/netplay/late_punch.c:95`). The
module draws 8 CSPRNG bytes with `Stun_MakeProbeNonce`
(`src/netplay/stun.c:167-175`), sends them to the nominee as a 26-byte probe
frame, and retargets only on a RESPONSE that comes back from that exact port
carrying that exact nonce — `Stun_ProbeNonceEqual`
(`src/netplay/late_punch.c:256-258`). No CSPRNG, no challenge, no relearn:
`Stun_MakeProbeNonce` failing is fail-closed
(`src/netplay/late_punch.c:116-124`). An unanswered nomination expires after
`LATE_PUNCH_CHALLENGE_MS` (1500 ms) and costs nothing —
`late_punch_challenge_due` (`src/netplay/late_punch.c:319-339`); one candidate
is held at a time, first come, so a flood cannot make the module challenge
whoever spoke last.

**The frame** (`stun.h`, `STUN_PUNCH_PROBE_*`): `"3SX_PUNCH" | token[8] |
kind[1] | nonce[8]`, 26 bytes. The punch prefix is retained deliberately —
every receive path already drops punch-prefixed traffic before GekkoNet —
`Stun_HasPunchPrefix` (`src/netplay/sdl_net_adapter.c:386-393`) — the MIST pump
offers it to `LatePunch_HandleDatagram` first (`src/netplay/netplay.c:1243-1249`),
and `classify_host_datagram` still ignores it because it demands the exact
17-byte payload: `Stun_IsPunchPayload` (`src/netplay/direct_p2p.c:1063-1066`). Nothing
downstream had to change to stay safe, including on a build that predates the
frame. The kind byte stops a challenge/response pair ping-ponging.

**3. The racing peer answers.** In the rescue the endpoint being challenged has
NOT handed off — it is a `StunPunchLeg` mid-race — so `Stun_PunchOffer` parks
the answer and `Stun_PunchPump` emits it to the challenge's source port
(`stun.c`, `StunPunchLeg.echo_*`). A probe frame carries the same prefix and
token a punch does, so it also confirms/retargets the leg exactly as a punch
would: the rescue does not get slower because the other side now verifies.

**Why a room-code holder cannot predict the challenge.** The token is
room-code-derived and therefore public to them; anything checked against values
they can compute is a slower version of the same hole. The nonce is the one
value in the exchange the nominee did not choose and cannot derive: producing
it requires having RECEIVED our datagram at the address claimed. Blind and
off-path injection is now dead outright — a forged source that cannot receive
can never answer.

**What it does NOT buy, stated plainly.** A same-public-IP party holding the
room code and running this build still receives the challenge and can still
answer it. It can therefore still win the single relearn if it gets there
first. Full identity binding is not available at this layer for the reason
already argued above. What changed is the cost: from "send one datagram, own
the session, deterministically" to "hold a live socket at the claimed endpoint,
beat the real peer's own retry to the single budget, and complete a round trip
inside 1500 ms".

**Unit pins (all shown RED against a mutation).** `--test-punch-predicates` now
runs 10 tests / 1680 assertions (`EXPECTED_TESTS` 7 -> 10, floor 550 -> 1400);
`--test-late-punch` gained A13/A14/A15 and rewrote A3/B3/A9. New coverage: a
nomination alone moves nothing and spends nothing; every single-bit nonce error
is refused; the right nonce from the wrong port or a foreign IP is refused; a
replayed response buys nothing; sixteen challenges never reuse a nonce and are
never all-zero; eight distinct ports after a verified move are all refused
without a challenge being opened; a nominee in flight cannot be displaced; an
unanswered nominee expires without spending the budget and the real peer is
still relearnable after it; the challenge is observed ARRIVING at the nominee
over a real socket and the response is built from the nonce that came off the
wire; and `Stun_PunchOffer`/`Pump` answer a challenge to the port it came from.

**Mutations proved (all RED; object file AND linked binary deleted before each
run, per the doctrine above).** M1 cap 1->8; M2 nomination retargets
immediately (the pre-fix behaviour); M3 nonce check removed; M4 source-port
check removed; M5 budget spent at nomination instead of verification; M6 nonce
compare `|=` -> `^=`; M7 probe length exact -> minimum; M8 a RESPONSE is echoed
too; M9 foreign-IP guard removed from the probe path; M10 the challenge never
expires; M11 the nonce is a constant (fail-open CSPRNG); M12 the candidate slot
is displaceable; M13 the budget is not enforced at nomination; M14 the racing
leg never answers; M15 the parked answer is never sent. M4, M6, M7, M9, M11,
M12 and M13 are caught by `--test-punch-predicates` only; M10, M14 and M15 by
`--test-late-punch` only.

**THE RESCUE STILL WORKS — re-run on the netns rig against the fixed code.**
`tools/netplay/natmatrix/rescue_scenario.sh` is Linux-netns only (`sudo -n ip
netns exec`, `iptables -m string --algo bm --string "3SX_PUNCH"`), so it was run
in a privileged `debian:trixie` container on the Docker Desktop VM kernel
(`6.12.76-linuxkit aarch64`, `xt_string` present), with SDL3/SDL3_net/GekkoNet
built for the guest and the probe configured `session phase ON`. Same rig,
same `--lift-s 11`, one variable:

| phase | before (`81571c85`) | after (this fix) |
| --- | --- | --- |
| control | both sync, host 633 ms | both sync, host 641 ms |
| baseline | host `FAILED_HANDSHAKE`/`DEADLINE`, joiner `FAILED_BILATERAL` | identical |
| rescue | host relearns **x1**, syncs 11423 ms; joiner 32 ms | host relearns **x1**, syncs **11409 ms**; joiner 32 ms |

`RESULT: GREEN`, exit 0. The added round trip costs nothing measurable — the
challenge and its answer both ride the pre-existing punch cadence, and the
joiner's leg confirms on the challenge itself.

**The rig's integrity guards were shown FIRING, not assumed.** A `p2p_probe`
once went 16 hours unlinked while its drivers exited 0, so none of the above is
worth anything until the instrument can fail:

- *vacuous baseline -> 3.* Re-run with `--lift-s 0` so the injection misses:
  baseline ends `HANDOFF/STARTED`, `RESULT: VACUOUS`, exit **3**.
- *rig contamination -> 5.* A two-row `TOPOLOGY_UP_FAILED` `.jsonl` fed to
  `tools/netplay/natmatrix/summarize.py`: `## REFUSING TO SUMMARIZE`, exit **5**.
- *a pass that syncs without a relearn -> 1.* A scratch copy of the scenario
  with `RH_RELEARN` forced to 0 after the scrape: the run genuinely relearned
  once, and the guard still refused it — `RESULT: RED -- the pair synced but the
  host applied no relearn`, exit **1**.

**Still unproven.** Nothing was exercised on the MiSTer device (its lock is held
by another session). The rig exercises the LEGITIMATE mover only — it has no
adversary namespace, so the same-IP capture itself is pinned at unit level, not
on the wire.

**Cross-version degradation — NO LONGER a code-reading claim. Tested by
construction 2026-08-30, and it is confirmed with one thing the claim missed.**

The claim was: a peer on a build without the probe frame consumes the 26-byte
challenge as punch-shaped bad-token traffic and never answers, so no relearn
happens and the pair falls back to the pre-#119 delayed failure — a lost
rescue, not a new failure mode. The old peer was built from the **verbatim**
pre-commit tree (`git archive e5527f5a^`; `stun.c`, `late_punch.c`,
`direct_p2p.c` and `mist_handshake.c` each diffed byte-identical against
`git show e5527f5a^:<path>`), and driven against a peer built from the current
tree whose production `LatePunch` was armed at a stale port so it emitted real
26-byte challenges over a real socket. `e5527f5a` touches neither
`rendezvous.c` nor `room_code.c`, so a real cross-version pair derives the
**same** token: the old peer really does see a correct-token, wrong-length
frame, which is the hardest case for it.

*Can the 26-byte frame be misparsed as a valid 17-byte punch?* **No, and not
by accident either.** Old `Stun_IsPunchPayload` tests length **exactly, first**
— `len != STUN_PUNCH_PAYLOAD_LEN` at `e5527f5a^:src/netplay/stun.c:149`, before
the prefix compare. A sweep of lengths 0–64 carrying the correct token and
prefix accepted **exactly one**, 17, and `classify_host_datagram` returned
`DP2P_HOST_DGRAM_PEER_PUNCH` for exactly that one. No old predicate
prefix-matches and ignores trailing bytes; `Stun_HasPunchPrefix`
(`e5527f5a^:stun.c:142`) is prefix-only but grants nothing — it only routes to
the drop paths. On the wire, 93 real challenges produced **zero** relearns,
**zero** retargets, the leg never confirmed and `tx_prompt` was never set, so
the old side never even schedules a reply. The old retarget
(`e5527f5a^:stun.c:900`, `leg->target_port = src_port`) sits *below* the
bad-token early return at `:865-879` and is unreachable from here. New side,
same run: 12 nominations, 12 expiries, relearn budget never spent — the lost
rescue, exactly as claimed.

*Does the extra 9 bytes trip a length assumption?* **No.** `Stun_PunchOffer`
consumes it, latches `local->diag_punch_bad_token` and returns
(`e5527f5a^:stun.c:865-879`); `LatePunch_HandleDatagram` consumes it and counts
a bad token; `classify_host_datagram` returns IGNORE; the MIST gate's
`mist_handshake_parse_response` returns **-2 (not ours, drop)** on the exact
wire bytes — *identically to a 17-byte punch*, so 26 bytes is not the more
dangerous of the two — and the H-1 implicit-completion path cannot fire because
`buf[0]` is `0x33`, outside its `[1,7]` range. Rebuilt under
`-fsanitize=address,undefined` in a fresh build directory: 54 real frames
through all three old receive paths, **0** findings. No fixed-size copy of
received bytes exists on any of these paths.

**WHAT THE CLAIM MISSED, and it is not nothing.** On the old *host-waiting*
receive path the challenge stream is charged to the host punch-auth gate.
`e5527f5a^:src/netplay/direct_p2p.c:5034` computes
`punch_shaped = Stun_HasPunchPrefix(...)` — **true** for the 26-byte frame,
since that predicate is prefix-only — and `:5059` therefore calls
`host_punch_gate_note_bad`. Modelled call-for-call over 19 s: the challenge
cadence is ~4.9/s (each nomination retransmits at `LATE_PUNCH_CHALLENGE_RETX_MS`
inside its `LATE_PUNCH_CHALLENGE_MS` window, and nominations re-open
indefinitely because `late_punch_nominate` gates on `s_relearn_count`, which
never advances when nobody answers). That crosses `HOST_PUNCH_SRC_MAX_BAD` = 24
in ~5 s and `HOST_PUNCH_TOTAL_REROLL` = 64 in ~13 s
(`e5527f5a^:direct_p2p.c:672-675`). Observed: `punch-gate MUTE ... for 60000 ms`
and a re-roll owed. So the honest statement is **not** "nothing happens" — our
rescue attempt can mute our own IP at the old peer's gate for 60 s and force it
to re-roll its room code (up to `HOST_PUNCH_REROLL_MAX` = 3).

Two limits, stated rather than papered over:

- **Reachability of that path is NOT established by construction.** In the
  canonical #119 rescue the moved endpoint is the *joiner's* fresh socket,
  which runs `Stun_PunchOffer`, not `host_tick_receive` — the clean path, and
  the one fully proven above. The gate path additionally needs the old peer to
  be a **host in `HOST_WAITING` whose observed source port changed** (a NAT
  rebind). The code path and its inputs are proven; the state was not entered.
- **Not examined at all:** whether a *same-version* pair can charge its own
  gate the same way, i.e. whether a new peer's `host_tick_receive` can ever see
  a challenge before `LatePunch_HandleDatagram` does. If it can, this is not a
  cross-version issue at all. Nobody has looked.

No source changed for this; the harness lived outside the repo. Re-open if the
mute is ever seen in the field, or if the same-version question above is
answered yes.

**ANSWERED 2026-08-30 — the same-version case is NOT reachable, and the reason
narrows the cross-version finding above by the same amount.**

*The gate at tip is still prefix-only, and it does charge a challenge.* Nothing
since `e5527f5a` narrowed it. `src/netplay/direct_p2p.c:5034-5035` computes
`punch_shaped = Stun_HasPunchPrefix(dgram->buf, dgram->buflen)`;
`src/netplay/stun.c:142-145` is `len >= STUN_PUNCH_PREFIX_LEN` plus a 9-byte
`memcmp` and reads nothing past byte 9, so a 26-byte probe frame
(`src/netplay/stun.h:147-151`) satisfies it. `classify_host_datagram`
(`direct_p2p.c:1044-1069`) routes that frame to `DP2P_HOST_DGRAM_IGNORE` —
`Stun_IsPunchPayload` demands `len == 17` exactly — and the IGNORE arm charges
it at `direct_p2p.c:5057-5060`. There are no other branches between the test
and the charge.

*Demonstrated on the wire, current tree.* A `p2p_probe` built from this HEAD
was parked in `HOST_WAITING` (`[direct_p2p] HOST_WAITING published ...
public=127.0.0.1:7000`) and fed, in order, 30 datagrams of 26-byte garbage with
a wrong magic and then 70 datagrams of the exact probe-frame shape
(`"3SX_PUNCH" | 8 | 'C' | 8`). Observed:

```
ADVISORY ... ignored unauthenticated datagram #1 ... (len=26)                 <- garbage, no "punch-shaped"
ADVISORY ... datagram #50 ... (len=26, punch-shaped: peer build too old ...)
punch-gate MUTE 127.0.0.1 for 60000 ms after 24 bad-token punches (session total 24)
punch-gate RE-ROLL #1/3 after 64 bad-token punches — room code regenerated
```

The session total was 24 at the mute, i.e. **none** of the 30 garbage
datagrams was charged and **all** of the probe frames were: the instrument
discriminates, and it can fail. (The token bytes were random. That is
immaterial and is the point — the charge decision reads bytes 0-8 only.)

*So the whole question is which peer can be in `HOST_WAITING` at a challenged
port, and the answer is none of them.* A challenge is only ever sent to
`(s_peer_ip, s_cand_port)` (`src/netplay/late_punch.c:383`), and `s_cand_port`
is set only by `late_punch_nominate` (`:127`), reached only from `:304` after
`Stun_IsPunchPayload` passed (`:271`) — so **the challenged port is by
construction a port that just emitted a valid 17-byte punch**. In the shipped
netplay code there are exactly three emitters of that payload:
`Stun_PunchPump` (`stun.c:926`), `LatePunch_Tick` (`late_punch.c:390`), and the
host's echo of a payload it just validated (`direct_p2p.c:5108`). (Complete:
every other `NET_SendDatagram` in `src/netplay` sends a 20-byte STUN request,
a 26-byte probe, a `'3SXR'` frame, a MIST frame or a GekkoNet frame.) Each one
is paired with a reader that parses probe frames:

- `Stun_PunchPump` runs only inside `p2p_race` (`direct_p2p.c:1973`, its only
  caller), which drains its own socket every iteration and offers every
  non-`'3SXR'`, non-STUN datagram to `Stun_PunchOffer` (`:2231-2237`), which
  parses the probe and answers it (`stun.c:961-981`).
- `LatePunch_Tick`'s socket is post-handoff, read by `netplay.c:1244` and
  `sdl_net_adapter.c:388`, both of which call `LatePunch_HandleDatagram`, which
  parses the probe and echoes it (`late_punch.c:225-249`).
- the host echo at `direct_p2p.c:5108` is followed unconditionally by
  `set_state(DIRECT_P2P_HANDOFF)` at `:5118` in the same call, so that socket
  is in the previous case before any reply can arrive.

`host_tick_receive` has exactly one caller — `DirectP2P_Tick`'s
`DIRECT_P2P_HOST_WAITING` arm (`direct_p2p.c:5748-5761`) — and in `HOST_WAITING`
the host emits no punch at all: the arm's sends are `rend_q_drain` (`'3SXR'`,
`:936`), `host_stun_keepalive_tick` (a STUN binding request) and that one echo.
A joiner never publishes `HOST_WAITING` at all; it is written only by
`host_thread_fn` (`:3789`) and by the M1 bilateral-failure return (`:5968`,
guarded by `s_work.role == ROLE_HOST`, `:5928`).

*Confirmed on the wire, both directions.* The `#119` rescue rig was re-run with
**both** peers built from this HEAD (`rescue_scenario.sh`, fullcone x fullcone,
`--lift-s 11`): **GREEN** — baseline `FAILED_HANDSHAKE` / `FAILED_BILATERAL`,
rescue host `relearned x1` and synced at 11443 ms. In it the host opened four
nomination windows against the joiner's retry port 45065 and the joiner logged
`STUN: Hole punch SUCCESS — liveness probe from peer` (`stun.c:972`) — i.e. the
challenge was consumed by `Stun_PunchOffer`, in `p2p_race`, exactly as above.
`grep -c "HOST_WAITING published"` over all six phase logs: **1** on each host
log, **0** on every joiner log. Zero `punch-gate` lines and zero
`ignored unauthenticated datagram` lines in any of the six.

*The one residual, named rather than closed.* The M1 path (`:5968`) returns the
HOST's socket — which *did* emit punches from `p2p_race` — to `HOST_WAITING`.
A challenge landing in that gap would be charged. Getting one there needs the
joiner to have nominated a host port, which needs the host's observed source
port to differ from the port the joiner handed off to; and the handoff port IS
the observed source port (`Stun_PunchOffer` sets `leg->target_port = src_port`,
`stun.c:977`, read back by `Stun_PunchEndpoint`, `stun.c:1036`; the
non-`REAL` oracle branch at `direct_p2p.c:1841-1844` is test-only —
`PUNCH_ORACLE` is `DP2P_PUNCH_REAL` unconditionally in the shipped build,
`:1443`). So it additionally needs a NAT rebind of the host's mapping
mid-connect. **Not entered — could not verify.** Tried: the fullcone x fullcone
rig has no rebind injection, and `natns.sh` exposes no knob for one. Bounded if
it ever happens: one nomination window is at most 8 datagrams
(`LATE_PUNCH_CHALLENGE_MS` 1500 / `LATE_PUNCH_CHALLENGE_RETX_MS` 200,
`late_punch.h:199-200`), well under `HOST_PUNCH_SRC_MAX_BAD` = 24, and it
cannot renew, because a `HOST_WAITING` host emits no punch to re-nominate on.

**AND THE CROSS-VERSION FINDING ABOVE NARROWS THE SAME WAY.** The old peer's
gate needs the identical state. An old peer at a challenged port is, by the
same construction, inside the old `p2p_race` — where the old `Stun_PunchOffer`
consumes the frame at its bad-token early return (`e5527f5a^:stun.c:865-879`)
and does not charge anything. The measured 60 s MUTE and the owed re-roll are
real *given* an old host in `HOST_WAITING` with a moved port, and that state is
still the unproven step for both versions. The cross-version case is therefore
**not independently worth a fix**, and could not be fixed on our side anyway:
the charge happens in the *old* build's gate, and the only new-build lever is
to emit fewer challenges — the canonical rescue above needed **four** nomination
windows, so any cap tight enough to stay under 24 charges would have broken it.

**RECOMMENDED FIX (not applied — reporting first).** Make the charge decision at
`direct_p2p.c:5057-5060` kind-aware: skip `host_punch_gate_note_bad` when
`Stun_ParseProbePayload(dgram->buf, dgram->buflen, s_work.punch_token, NULL,
NULL)` succeeds (the token is already in scope at `:5006`). Roughly four lines.
Why this one and not the alternatives:

- *Length-exact instead* (charge only `len == STUN_PUNCH_PAYLOAD_LEN`) is
  cheaper and worse: it would also stop charging the 9-byte legacy punch from a
  pre-S4a build, which is precisely the traffic the gate's own rationale names
  as chargeable (`direct_p2p.c:635-637`) and which the advisory reports as
  "peer build too old".
- *Exempt a frame we sent to a nominee* does not apply. The side that charges is
  the one in `HOST_WAITING`; its `LatePunch` is disarmed and it never nominated
  anyone, so there is nothing on that side to exempt against.

It opens no hole in either constraint the design rests on.
`sdl_net_adapter.c:386-393` still drops punch-prefixed traffic before GekkoNet
(different file, untouched), and `classify_host_datagram` still returns IGNORE
for 26 bytes, so a probe frame still cannot be accepted as the peer — only the
*accounting* changes, not the routing. It is not a brute-force escape either:
`Stun_ParseProbePayload` verifies the token in constant time
(`stun.h:165-172`), so only a party that already holds the token is exempted,
and a 26-byte frame can never yield `PEER_PUNCH` regardless.

**Whether to take it: yes, but as hygiene, not as a live-defect fix.** On the
evidence above it is unreachable in the canonical rescue and bounded in the one
residual, so it does not gate the alpha. Its cost is the full netplay gate set
(harnesses + shipped-config build + ARM cross-build) for four lines.

One stale comment found while reading, left alone: `test_punch_predicates.c:1188-1191`
says the host gate "still ignores this (direct_p2p.c:1063-1066)". True of the
routing, false of the accounting — IGNORE is the arm that charges. Worth
correcting whenever that file is next touched.

GATES: none run and none apply — no compiled code changed. Frame-data is out of
scope and was skipped deliberately: this is pre-session transport, not
simulation, and nothing on the frame path was read or written.

---

## #129 — ROM search trim + a legible miss — CLOSED

Candidate set cut from 19 directories x 2 basenames (38) to 5 x 2 (10):
`src/arcade/arcade_char_data.c`. Kept `/media/fat/games/mame` (update_all's
default, and the only path ever exercised), `/media/fat/mame`,
`/media/fat/_Arcade/mame` (Main's own fallback) and both `/media/usb0`
layouts. Dropped `usb1`–`usb5`, `/media/network`, `/media/fat/cifs` — on the
target device `/media/network` and `/media/fat/cifs` do not exist even as
mount points, while `/media/usb0`..`/media/usb7` do. Nothing was kept against
the brief.

Upstream order re-verified against Main_MiSTer @ `915ca339`, and **three
citations in the pre-existing comment were wrong**: the order list is
`file_io.cpp:1048-1056` (not `:1048-1055`, which cut off
`/media/fat/<prefix>/<dir>` — the one path that actually works), the bare
`/media/fat/mame` check is `file_io.cpp:1118-1122` (not `:1124-1127`, which is
the `games/` branch), and `findPrefixDir` ends at `:1133` (not `:1132`).

A miss now reads differently in each of the three cases — no directory
existed / a directory existed but held neither basename / a zip was found and
**rejected by content verification** (the wrong-revision case, previously
indistinguishable from having no ROM). Bounded: a normal miss is one line.
`rom_load.c` untouched — the classification is done by `SDL_GetPathInfo` in
`arcade_char_data.c` and never gates a `Rom_Load` call, so what gets FOUND is
unchanged.

Induced, not asserted: the no-directory case and the found-and-rejected case
were each provoked against the real binary and print different text (a
valid-zip-with-wrong-contents drives the second). The directory-exists-but-no-
basename case needs a writable `/media`, so it is induced on the device under a
tmpfs overlay rather than on the host.

---

## #131 — review batch (five items) — CLOSED

All five landed as separate commits; full gate set GREEN including ARM.
Frame-data was **skipped deliberately**: nothing here reaches the simulation
(orchestrator timing arithmetic, a socket-lifetime guard, a Python gate, JS
logging, and comments).

1. **Host ladder undercounted the worker startup delay — FIXED** (`dfae3c13`).
   `ORCH_HOST_LADDER_MS` (`src/netplay/direct_p2p.c:6158`) now multiplies
   `WORKER_STARTUP_DELAY_MS` by `1 + HOST_STUN_MAX_RETRIES`: the S2 ladder
   respawns `host_thread_fn` (`:5856`) and the delay is that worker's first
   statement (`:3518`), so it is paid per rung. Assert `[D]` re-derived
   86450 → **87050** (`:6289`); shipped defaults 42,450 → 43,050; the `[C]`
   inversion corner 30,450 → 31,050 (still under postwait's 31,300, so the
   max stays load-bearing); the #96 upper guard 120,200 → 120,800.
   The joiner term was already correct and is untouched — its delay is taken
   once (`:4334`), OUTSIDE the `JOIN_MAX_ATTEMPTS` loop.
   **Perturbed to confirm:** reverting the macro fails the build on `[D]`
   with `200 + 11250 + 4*15000 + 3*5000 = 86450 >= 87050`.

2. **Late-punch socket invalidation — GUARD ADDED, no live bug**
   (`6b17dc73`). `Netplay_SetStunSocket` (`src/netplay/netplay.c:2756`) now
   calls `LatePunch_Disarm()` on the destroy branch only.
   **The failing case could NOT be constructed, and the attempt failed on a
   real structural guard rather than on luck:** `do_handoff` (`direct_p2p.c:4522`)
   is the only production caller, it nulls `s_work.stun.socket` immediately
   after (`:4532`), and Tick's HANDOFF case re-enters `join_tick_handoff`
   only while that field is non-NULL (`:5783`) — so a second in-session
   handoff is unreachable. Both terminal paths disarm before the destroy by
   explicit ordering (`netplay.c:2543` vs `:2562`; SessionStarted `:1798`).
   What remains is that the invariant lives in a *different* TU, so it is now
   pinned by `test_late_punch` A12 (`src/netplay/test_late_punch.c:621`),
   which fails with the disarm removed.

3. **Rate-cap gate vs its prose — GATE TIGHTENED, prose is authoritative**
   (`15b7d40e`). `check_key_rate_budget.py:250` now solves for the smallest
   INTEGER `k`: `k = max(MIN_KEY_TO_IP_RATIO, ceil(absorption/ip_cap))`,
   giving **30** instead of 23.
   Chosen over relaxing the prose because (a) the gate's own docstring
   already quoted "the smallest integer k … is k = 3", so the file
   contradicted itself and the prose half was right; (b) at 23 the
   under-attack headroom is 13 of 13 — exactly zero — against the 1.5x the
   derivation claims, so documenting 23 would have licensed a capless cap;
   (c) `__test_protocol.js` already enforced the UPPER half (`cap <= 3 x`
   per-IP) but also passed at 24, so nothing closed the band.
   `docs/plan-netplay-connection.md` already said 30 and needed no edit.
   **Falsified by sweep:** cap 22/23/24/29 → exit 1, cap 30 → exit 0. The
   old gate passed 24.

4. **Unthrottled WARNs — FIXED, and it was three sites not one** (`a1928795`).
   `notePushLost` (`rendezvous-server.js:1201`) now routes through
   `noteThrottled`. Auditing every `logWarn` found two *worse* post-cookie
   per-packet sites, both of which fire with **no attacker present**:
   "REGISTER NAT mismatch" (`:1249`, once per REGISTER for any
   port-rewriting NAT) and "REGISTER … for full session" (`:1420`, once per
   REGISTER from each of dialers 3..N). `notePushLost` was in fact the
   quietest of the three — bounded by the cookied per-IP gate *and* by
   single-observation clearing. All three converted; each keeps a
   file-chosen CONSTANT reason (noteMap keys on it) with the variable parts
   in `detail`. `pushStats` still increments unconditionally and
   `reportPushStats` already emits the aggregate, so no data is lost.

5. **Doc drift, three sites — FIXED** (`2ae601d4`).
   - `netplay_nav.c:133` 120.2 s → **87.05 s**, with a note that it is prose
     about a derived, `_Static_assert`-pinned number and has now rotted twice.
   - `direct_p2p.c:2643` "10 s ceiling" → **11.25 s** (6000 + 3 x 1750 =
     `PORTMAP_PROBE_BUDGET_MS`, which the loop below enforces).
   - `gen_plw_canon_fields.py:21` stated its descend/emit rule exactly
     BACKWARDS; `parse_plw` descends into non-unions and emits unions whole.
     Generated output unaffected — `--check` passes either way (399 fields,
     38 pointers, sizeof=1092/1304).
   - 11 citations in `docs/plan-netplay-connection.md` follow the +2/+6 line
     shift. Verified as a shift (the doc had 0 errors immediately prior), not
     taken from the linter's suggestions — four of those point at a different
     occurrence of the anchor. `--fix` was not run.

**Gates:** `tools/gates/run-gates.sh --arm` → all 9 GREEN, exit 0. 11
harnesses, every one exit 0, zero "not compiled in" in any log.
`__test_protocol.js` 39/39. Doc-citation baselines 11 scopes, 0 breached,
0 slack. ARM cross-build GREEN in the aarch64 container.

## #132 — coverage plan — PRIORITY 1 CLOSED

Three commits. Priority 2 and 3 remain open, unchanged, below.

**What was covered, and what each test catches.** Every test below was shown
RED against a specific mutation of the code it pins; a test that has never
been red proves nothing. Each red was confirmed after deleting BOTH the object
file AND the linked binary — Unix Makefiles compares mtimes at one-second
granularity, so rewriting a source inside the same second leaves the harness
silently re-running the previous binary (observed twice while building the
proof, once as a one-step lag and once as an every-other-step lag).

### `--test-mist-compat-gate` (`3faacc32`) — 9 tests, 1294 assertions

`classify_peer_payload` (`src/netplay/mist_handshake.c:314`), `parse_header`
`:210`, and the four bounds-checked readers `:236/:255/:263/:271`, reached
through six thin trampolines in the existing `ENABLE_NETPLAY_TESTS` block
(`mist_handshake.c:1003-1057`). NOT the `mist_handshake_gate_next` `:894`
production-export shape: promoting the classifier to a shipped symbol would
widen the compat gate's API for no shipped caller. The `s_balance_digest`
global needed no extraction at all — `mist_handshake_set_balance_digest`
(`:110`) is already a production setter.

| test | mutation it was proved against |
| --- | --- |
| `accept` | build_hash difference becomes a reject |
| `truncate` | `read_u64be` bound off by one |
| `reasons` | arch reject reports the platform code |
| `order` | `state_ver` checked before `proto_ver` |
| `strings` | `read_cstr` advances by the TRUNCATED copy length |
| `header` | declared-vs-received length check removed |
| `readers` | `read_u16be` becomes little-endian |
| `digest` | digest compared on its 32-bit DISPLAY fold |
| `text` | the arch reason text ignores `text_cap` |

The truncation sweep asserts a reason per prefix byte: `[0,20]` MALFORMED,
`[21,23]` LEGACY, `[24,31]` MALFORMED, `32` accept. The LEGACY window is three
bytes wide between two MALFORMED regions, which is what makes an off-by-one in
any reader visible.

### `--test-punch-predicates` (`80327519`) — 7 tests, 744 assertions

`Stun_IsBindingResponse` (`src/netplay/stun.c:1146`) had zero tests despite
gating three receive paths (`direct_p2p.c:1060`, `:2230`,
`sdl_net_adapter.c:343`). `Stun_IsPunchPayload` `:147` had a four-row truth
table at `test_stun_mock.c:861-894` that said nothing about WHICH bytes it
compares. Now: all 64 token bits, all 72 prefix bits, the length swept -2..64,
plus a FOLD trap and an ORDER trap. Those two earn their place — replacing
`diff |=` with `diff ^=` at `stun.c:160` passes all 136 single-bit flips and
fails only those two assertions.

Three late-punch decisions the socket harness cannot observe, because seeing a
datagram at all means advancing past `LATE_PUNCH_TX_INTERVAL_MS`, by which
point the cadenced keepalive has fired anyway: a foreign-IP valid token must
schedule no answer (`late_punch.c:116-123` says so; nothing enforced it),
the capped move must change nothing and keep targeting the LEARNED endpoint,
and an accepted relearn must move the target and hand netplay.c the same port.
`LatePunch_TestPeek` (read-only, `ENABLE_NETPLAY_TESTS` only) is what makes
the send target observable without a socket.

Mutations proved: N1 binding-response floor 20→16, N2 prefix memcmp 9→1 byte,
N3 length becomes a minimum, N3b loop drops the last token byte, N3c
accumulator becomes an xor fold, N4 foreign-IP guard removed, N5 cap removed,
N6 accepted relearn does not move the target, N6b the capped move moves it,
N7 a wrong-token punch schedules an answer.

### `constant-time-compare` gate (`c62f2792`) — gate count 9 → 10

`tools/gates/check_constant_time_compare.py`. An early-exiting rewrite of the
punch-token compare is BEHAVIOURALLY IDENTICAL — same verdict for every input
— so the 136-bit sweep passes against it unchanged and no unit test can see
it. A timing measurement over a 17-byte compare is noise. So the property is
checked where it is expressed: the loop still runs over
`STUN_PUNCH_TOKEN_LEN`, accumulates with `|=`, and contains no `return`,
`break`, `goto`, `continue`, `if`, `?:` or short-circuit. Red-proved four
ways; a renamed function exits **2** (ERROR), because a check that silently
finds nothing is worse than no check.

**Anti-vacuity, all three devices proved to fire** in both harnesses: literal
`EXPECTED_TESTS` (commenting one call out → "ran 8, expected exactly 9"), the
assertion floor ("only 527 assertion(s) ran, floor is 550"), and per-sweep
case floors. Both stubs say "not compiled **in**", the spelling
`tools/gates/run-gates.sh:204` greps for; both exit **2** against the
shipped-config binary.

**Judged NOT worth testing, with reasons:**
- The `*off + n > payload_len` form in the fixed-width readers wraps for an
  offset near `SIZE_MAX`. Unreachable by construction — `*off` is only ever
  advanced by `read_cstr` and by the readers themselves, all of which keep it
  in `[0, payload_len]`. A test would pin a precondition the code cannot
  reach; the harness sweeps all 153 `(off, len)` pairs inside the real
  invariant instead and names the residual in a comment.
- Whether the relearn-cap path ALSO raises the prompt-answer flag. The flag is
  sticky until `LatePunch_Tick` sends (`late_punch.c:342`), so after the eight
  accepted moves an answer is legitimately already owed. Separating the two
  needs a Tick, which needs a socket, which is `test_late_punch.c`'s job.
  Faking it would be the vacuous version.
- Constant-time TIMING itself — see the gate above.

**Frame-data: SKIPPED, deliberately.** Nothing in this lane reaches the
simulation. Two new test translation units, six test-only trampolines, one
read-only test-only accessor, and a Python source-shape check; the only
non-test source edits are inside `#ifdef ENABLE_NETPLAY_TESTS`.

**Priority 2 — CLOSED.** See `--test-netplay-units` below.

**Priority 3 — CLOSED.** See `--test-netplay-units` below.

### `--test-netplay-units` — priorities 2 and 3, one harness

`src/netplay/test_netplay_units.c`. **15 tests, 1059 assertions, 0.18 s.**
Registered the same way every other harness is: a `Configuration` field, an
`OPT_BOOLEAN` in `src/args.c`, a forward decl and a dispatch `if` in
`src/main.c`. Harness count 13 → 14.

**P3 — what moved, and the number that justifies it.** Per-test wall clock was
measured directly (temporary `SDL_GetTicks` instrumentation around each call in
`Netplay_Test_BilateralPunch`), not estimated:

| test | ms |
| --- | --- |
| `test_race_worst_case_timing` | 26447 |
| `test_race_two_peer_convergence` | 17956 |
| `test_s7_lost_mapping` | 11776 |
| `test_joiner_cookie_handshake` | 10481 |
| `test_joiner_self_deliver` | 10236 |
| `test_s7_review_fixes` | 7549 |
| `test_race_rearm_releases_address_ref` | 6589 |
| ... 26 more | |
| **whole harness** | **110471** |

The harness is one linear dispatch, so what an engineer iterating on a block
actually pays is the CUMULATIVE cost to REACH it. Summing the dispatch order:
the NAT-PMP/PCP wire codec sat **~88 s** in; the ladder-shape and renewal-
cadence tables sat **~90 s** in; the earliest of the moved blocks
(`test_failure_taxonomy`) sat **~13 s** in. **All 13 now run in 0.18 s.**

**Be precise about what did NOT change: the gate's total wall clock.**
`--test-bilateral-punch` was 110.47 s before and 110.24 s after — noise. The
extracted blocks were *pure*, so they were never what cost the time; the
scenario tests that do cost it are all still there, which is the instruction and
also correct (a mocked unit would pass vacuously in their place). The win is the
cost to REACH a decision, not the cost of the suite. The 47 s + 31 s figures in
the original framing did not reproduce: `test_host_cookie_rejected` measures
**745 ms**, not 31 s — it drives a mocked clock and only *reports*
`waited_ms=30060`.

The 13: `session_key_stability`, `lan_bypass`, `failure_taxonomy`,
`host_datagram_gate`, `rendezvous_cookie_codec`, `rendezvous_frame_router`,
`punch_gate_throttle`, `race_budget_wrap_safety`, `natpmp_codec` (the whole
RFC 6886/6887 build+parse section), `natpmp_ladder_shape`, `pcp_short_error`,
`nonpublic_gate`, `portmap_renew_cadence`. Moved, not copied — nothing is
asserted twice, `test_bilateral_punch.c` went 8312 → 6840 lines and kept every
scenario test.

**P2a — the ORCH cascade, derived independently.** `unit_orch_cascade`
(`src/netplay/test_netplay_units.c:1827`). The `ORCH_*` macros are file-local to
`direct_p2p.c` and `DirectP2P_OrchWorstCaseMsForRole` reads its budgets from
live config, so the arithmetic could only be reached by writing config and
reading a role bound back — which pins the SLOPE and never the value. New hook
`DirectP2P_TestHook_OrchCascade` (`src/netplay/direct_p2p.c:6396`) evaluates the
real macros at CALLER-SUPPLIED budgets, no config, no clock. The test then
re-lists the primitives (`HOST_STUN_MAX_RETRIES`, `WORKER_STARTUP_DELAY_MS`,
`PORTMAP_PROBE_BUDGET_MS`, `RESOLVE_POLL_MAX_MS`, `JOIN_MAX_ATTEMPTS`,
`STUN_PUNCH_CONFIRM_MS`, the four clamps) as its OWN literals and recomputes the
ladder itself. 7 (stun, race) rows × 6 derived values, plus both slopes swept,
plus the corner where the `MAX` branch flips.

That duplication is the alarm, and it is deliberate: change
`WORKER_STARTUP_DELAY_MS` in `direct_p2p.c` and this test goes red, so the
change becomes a decision instead of a silent slide. The four numbers #131 got
wrong are ALSO pinned as literals (ceiling ladder 87050, shipped defaults 43050,
the corner, and #96's 120800 upper guard), because a careless "fix the test"
would edit the macro and the re-derivation together and leave the table green.

**P2b — natpmp timeout math takes the clock.** `np_remaining_ms`
(`src/netplay/natpmp.c:920`), the new `np_step_deadline` (`:932`, hoisted out of
`np_transact`'s inline expression) and `np_phase_deadline` (`:1156`) all take
`now_ms` instead of calling `SDL_GetTicks()`. The callers still read the clock —
this is a testability seam, not a virtual clock. Hooks
`Natpmp_TestHook_RemainingMs` / `_StepDeadline` / `_PhaseDeadline`
(`src/netplay/natpmp.c:869`) forward to the production functions rather than
reimplement them. `unit_natpmp_deadline_math` (`:1976`) then sweeps what elapsed
time cannot produce on demand: an already-expired deadline, the exact deadline
edge, the 2000000000 ms clamp, a phase ceiling truncating a rung, a suppressed
rung inheriting the whole phase, out-of-range steps, `now` up near 2^64, and
three back-to-back phases fitting `NATPMP_PROBE_BUDGET_MS` exactly — the H-6
property, in microseconds instead of the 3896 ms it took to measure.

**Mutations (object file AND linked binary deleted before each run).**
V1 a unit dropped from the dispatch → RED (`EXPECTED_TESTS`).
V2 the RFC 6886 ladder grown back to 5 rungs → RED.
V3 the #131 bug restored (startup delay paid once) **with every ORCH
`_Static_assert` neutralised**, so the unit test is the only detector left →
RED.
V4 a primitive changed under the test (`HOST_STUN_RETRY_BACKOFF_MS` 5000→4000),
asserts neutralised → RED ("macro says 40050, the independently derived value is
43050").
V5 the `MAX` dropped from `ORCH_HOST_WORST_CASE_MS`, asserts neutralised → RED
at the corner.
V6b `np_remaining_ms` one ms too generous → RED.
V7 `np_step_deadline` drops the phase ceiling → RED.
V8 `np_remaining_ms` drops the 2000000000 clamp → RED.

**V6 is an EQUIVALENT mutant, not a gap.** `now_ms >= deadline_ms` →
`now_ms > deadline_ms` survived, and it should: at `now == deadline` the
fall-through computes `left = 0`, `0 > 2000000000u` is false, and it returns 0 —
byte-identical behaviour for every input. The early return is an underflow
guard, not a boundary decision. V6b exists because of it.

**Non-goals, deliberately:** the punch race, split brain, the PROMISED
LISTENING INTERVAL, the reclaim boundary, GekkoNet/rollback determinism, UPnP.
All interaction bugs, where a mocked unit passes vacuously.

---

## Smaller open items

- **#125 — CLOSED.** `--test-connect-observability` flaked only under
  concurrent gate runs. **Reproduced first** (4 concurrent runs, 2 rounds: 3 of
  the 4 failed in round 2; a later 12-run batch failed 8 times), then fixed,
  then re-run 16/16 clean.

  **The theory in this line was half right and the diagnosis it implied was
  wrong.** It is not one test and it is not only the directory. Three distinct
  mechanisms, all measured:

  1. **Shared log FILENAME.** The session log is
     `<PrefPath>logs/netplay-<utc_ms>.log` — `netplay_log_open`
     (`src/netplay/netplay.c:529`) — so
     processes that start in the same millisecond open the SAME FILE. Measured:
     three of four concurrent runs all opened
     `netplay-1788111677761.log`. Symptom: `test4-mt-sink: 4 of 800 MT lines
     missing or torn` — interleaved writers, nothing wrong with the sink.
  2. **The harness picked someone else's file.** `obs_find_new_log` took the
     NEWEST `netplay-*.log` in the directory past a timestamp. Measured: those
     same three processes all validated against `netplay-1788111677763.log`, a
     fourth process's file, and reported `800 of 800 MT lines missing or torn`
     about a file they had never written a line to. The end-of-run cleanup
     removed that file too — i.e. it could delete a live log belonging to
     another process.
  3. **Cross-process truncation.** `fopen(path, "w")` on a colliding name, plus
     the #44 prune keeping the 20 newest, produced
     `session file grew from 261995 to 0 bytes AFTER the TRUNCATED marker` and
     `wrote 20000 padded lines and the session file never published a TRUNCATED
     marker`. That second one is the reported `test6-byte-budget` symptom.

  **Why the directory had to be the fix and not just the accessor.** With the
  accessor fix alone (each process reading its own path), 5 of 12 concurrent
  runs still failed — mechanisms 1 and 3 are writer-side. Measured, both ways.

  **Fix, three parts.** `Paths_GetPrefPath` (`src/port/paths.c:46`) now honours
  `THIRDSARM_HOME` on **every** port, not just MiSTer/Miyoo. On the host build
  there was previously no way at all to move this directory: `SDL_GetPrefPath`
  goes through `NSApplicationSupportDirectory` on macOS, which ignores `$HOME`
  — measured, a harness run with `HOME=/tmp/fakehome1` still wrote to
  `~/Library/Application Support/CrowdedStreet/3S-ARM/`. `tools/gates/run-gates.sh:198`
  then gives every harness invocation its own `$$`-keyed home, which makes
  concurrent RUNS hermetic too, not just concurrent harnesses within a run. And
  `Netplay_TestHook_SessionLogPath` (`src/netplay/netplay.c:515`) hands the test
  the path this process actually opened, so the harness is correct even in a
  shared directory.

  **Result:** 16/16 concurrent runs clean, 0 failures.

  **Deliberately NOT fixed, with the reason.** The production filename
  collision itself. It would need `netplay-<ms>-<pid>.log` and a widening of
  `netplay_log_name_stamp` (`src/netplay/netplay.c:376`), which the file calls
  "the entire safety argument for the prune". It cannot bite in practice: two
  3S-ARM instances on one machine is not a supported configuration, and the
  natmatrix probes link `netplay_stub.c` (`netplay_stub.c:42`), so they write no
  session log at all. Widening a safety-critical predicate to paper over missing
  test isolation is the wrong trade.
- **#128** — five reproducible file-load failures (file numbers 9, 10, 1454,
  1456, 1458). **The failures are CLOSED**: fixed by `522e574c`, and verified on
  hardware under #140 (five before, zero after, on the device). The 379 ms
  startup outliers bundled into this item were **not** the same bug and are
  still open — see #141.
- **#141** — the ~415 ms stall at **frame 449** is **identified and CLOSED**
  2026-08-30: it is `OPBG_Init()` (`src/sf33rd/Source/Game/opening/opening.c`),
  the one-shot build of the opening-demo background, reached at a fixed sequence
  position via `Title()`. It contains **no disk I/O** — 271 ms tilemap melt,
  105 ms PPG decompress, 33 ms texture-cache build — which is why the AFS fix
  never moved it. Inherent one-shot work; **no fix applied** (moving it off a
  displayed frame is a state-machine restructure for a cosmetic boot hitch).
  See the full "#141" section below.
- **#108** — CLOSED 2026-08-30, both mechanisms. Two residuals remain open and
  are named below; see the full "#108" section further down this file.

---

## Open structural gaps

*(none open)*

**Closed 2026-08-30 (task #122 test commit), recorded so the list does not
re-open them:**

- `src/netplay/test_sparse_effect_save.c` printed "not compiled **with**" where
  `tools/gates/run-gates.sh:204` greps "not compiled **in**". Fixed. The same
  evasion was found and fixed in `src/test/test_texcash_bounds.c`.
- `tools/netplay/natmatrix/mech_matrix.sh` fell off the end of the file and
  exited 0 regardless of per-rep `rc` — the last statement was an `echo`, with
  no stage-rc propagation of the kind `tools/netplay/natmatrix/run_all.sh:27-31`
  grew. **Closed 2026-08-30.** It now classifies every rep against
  `rig/punch_mech.py`'s rc vocabulary (0/10 = finding, 20 and anything else =
  the rig did not run the trial) and exits 0 / 4 (contaminated) / 3 (vacuous),
  the same three codes `run_matrix.sh` uses. Proved with a red/green pair on a
  stubbed copy of the driver: the pre-fix script exits 0 with every rep at
  rc=20; the fixed script exits 3 there, 4 on a mixed run (rep 2 rig error,
  reps 1+3 scored), 3 when the topology never comes up, and 0 both on all-rc-0
  and on all-rc-10 — the second confirming a legitimate negative finding is
  still green. No past grid is in doubt: both grids in `docs/nat-matrix.md`
  were read from the JSONL rows, which have always carried `host_rc`/`join_rc`,
  and no script in the tree ever invoked `mech_matrix.sh` to consult its rc.

---

## Needs the user at the machine

- TV + pad confirmation of the shortened select countdown — measured **at the
  source of the rendered digits, never at the glass**.

---

*Verified at `dcf7631a` (`upstream-engine-fixes`), i.e. with task #122's
client-half (`bfa20e56`) and test (`dcf7631a`) commits applied. Re-verify every
line number if the tree has moved.*

---

## #134 — the five boot-time AFS load failures — CLOSED (root-caused and fixed)

Every device smoke run logged exactly five
`ファイルの読み込みに失敗しました。ファイル番号：N` lines for N = 9, 10, 1454,
1456, 1458, and nothing consumed the error. **Not a missing asset and not a cut
PS2 asset — a lost wakeup in the port's async I/O layer.**

**The numbers are AFS entry indices**, emitted by `load_it_use_this_key`
(`src/sf33rd/Source/Game/io/gd3rd.c:355-380`, the message at `:378`). All five
resolve, and all five are physically present in the shipped archive — parsed
from `SF33RD.AFS` (1535 entries, format per `src/port/io/afs.c:89-160`):

| fnum | name | size | verdict |
| --- | --- | --- | --- |
| 9 | `default.bin` | 61,440 | present, read inside EOF |
| 10 | `scrscrn.ppg` | 50,348 | present, read inside EOF |
| 1454 | `ef02_usa.bin` | 1,128,236 | present, read inside EOF |
| 1456 | `ef06.bin` | 595,800 | present, read inside EOF |
| 1458 | `ef40.bin` | 170,688 | present, read inside EOF |

The sibling "file number is abnormal" message (`gd3rd.c:311-312`, the
`AFS_GetFileCount` bound test and the `flLogOut` it guards) was never logged,
which already ruled out an out-of-range index.

**Root cause.** `AFS_ReadSync` (`src/port/io/afs.c`) drained the async queue and
decided whose completion it had by reading `request->index` **after**
`process_asyncio_outcome` had run. That handler `SDL_zerop()`s a slot whose
deferred close has landed, which resets `index` to 0 — so every such completion
was indistinguishable from slot 0's, and `AFS_ReadSync(0, ...)` returned while
its own read was still in flight. `fsCheckFileReaded` then saw
`AFS_READ_STATE_READING`, `fsFileReadSync` reported failure, and the retry loop
in `load_it_use_this_key` (`gd3rd.c:355-380`) logged and retried — the retry succeeding, because nothing was
ever wrong with the file. Second half of the same defect: `AFS_Open` reuses the
first free slot, so a CLOSE completion left over from the slot's previous user
carried the same index and would end the wait just as wrongly.

**Fixed** by reading the slot identity before the handler can zero it and by
ending the wait only on this slot's own `SDL_ASYNCIO_TASK_READ`.

**Reproduced and proven on the host, no device needed.** The bug is in shared
port code, not MiSTer-specific:

```
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  build/host-release/3S-ARM.app/Contents/MacOS/3S-ARM --headless --perf-capture 600
```

Before: the same five numbers, in call order `9, 1456, 1458, 10, 1454`
(`Init_load_on_memory_data` at `src/sf33rd/Source/Game/rendering/aboutspr.c:102-109`,
then `Scrscreen_Init` at `src/sf33rd/Source/Game/ui/sc_sub.c:425-437`, then
`checkSelObjFileLoaded` at `src/sf33rd/Source/Game/rendering/texgroup.c:558`).
After: zero, with 600 frames still completing.

**REFUTED on hardware — see #140.** The frame ordinal added by `9a597cd7`
shows the big outlier is at **frame 449** in every run, while all five failures
complete before frame 10, and a diagnostic build with `afs.c` alone reverted
puts it at 376.5-416.7 ms *with* the failures against 413.0-415.0 ms *without*
them. The paragraph below is left as written, as the reasoning that was
falsified.

**The 379 ms startup outliers are a consequence, not a separate cause.** The
outlier test (`src/port/sdl/sdl_app.c:3627`, threshold 50 ms — its comment
still says 25 ms and is stale) measures a window that encloses the blocking
loads (`src/main.c:902-904`). Each failure made the game read the file twice —
for fnum 1454 that is 2 x 551 x 2048 = 2,256,896 bytes of blocking I/O in one
frame. The host reproduces the failures but logs **no** outlier at all, so the
379 ms figure is the device's slower storage crossing the threshold on the
doubled read. **Not verified on hardware** — the device lock was held.

---

## Citation-cost measurement (analysis only, no scheme landed)

Re-measured at `522e574c`, linter `check_doc_citations.py` sha256
`3acc6c44c93fd0d6cf567acf5fff76501f8e2a4eacb2c5ba005102eadc508073`. **The
working tree also carried three other lanes' uncommitted edits at the time**,
so these are counts over that tree, not over the commit alone.

That caveat is not decoration. The same script run twenty minutes earlier at
`81571c85` gave 3,521 line anchors, 1,020 clean and 996 moved, against 3,540 /
1,014 / 1,017 here. A ceiling fitted to either number would have been red at
the other — which is the concrete form of the hazard that sank the previous
`<tree> [unanchored-citation]` ceiling.

- **3,540** line-anchored citations tree-wide; 12,015 positionless references
  (3,042 bare paths + 8,973 backticked symbols). Line anchors are 22.8% of all
  references.
- Only **2,217 of the 3,540 make a checkable claim at all**. Of those: 45.7%
  clean, 45.9% merely moved, and **8.4% (186) name a file that has never
  contained the symbol the prose claims.** The linter is silent on that last
  class by design ("absence of proof is not a finding"), so the class most
  likely to be a genuine defect is the one nothing reports. That 186 was
  identical at both commits.
- Line-anchor invalidation over the last 30 h alone: **69 of 283** citations
  carrying a uniquely-locatable symbol (24.4%) had that symbol move, i.e. would
  have needed a hand repoint. The symbol itself stayed resolvable in every one.
- **22 commits on this branch have "repoint" as their subject**, all inside that
  same 30 h window (1,019 insertions / 486 deletions across 84 file-touches).
- In the 11 enforced scopes: **85.2% of resolvable citations are pure pointers**;
  only 14.8% restate a value the code defines (17.6% under a looser literal
  test). Hand-auditing 12 sampled "stale value" findings, every one I could
  check concretely was a false positive — bare line numbers in prose, markdown
  table row bleed, or a value the code computes rather than spells. **Value
  duplication is not a significant staleness surface in the enforced scopes.**

---

## #108 — the corpus suite could not exercise the engine that ships — CLOSED (two residuals named)

Both mechanisms in the original report were real, are independent, and are
now both addressed. Everything below is measured on this tree, not inferred.

### 1. The balance pin — replaced by a DECLARED balance

`arcade_balance.c` used to pin PS2 for any process with
`configuration.test.enabled`, before `ArcadeCharData_Init` was even called.
`tools/frame-data/run.sh` passes `--test-enable`, so all 94 corpora measured
the PORT engine while a device auto-selects ARCADE the moment its romset
verifies. `sag_union_0/1/3` (`src/sf33rd/Source/Game/engine/plmain.c:642`,
`:691`, `:790`) were unreachable from automation by construction.

What changed:

- `--test-balance ps2|arcade` (`src/args.c`) — the balance a harness run
  exercises is now STATED. `--test-scene-preset training-frame-data` REFUSES
  to start without it, so the frame-data suite cannot go back to inheriting
  its engine by omission.
- PS2 remains the DEFAULT for `--test-enable` with no `--test-balance`, so the
  netplay / perf / rollback harnesses resolve identically on every machine
  whether or not a romset is installed. That default is now a documented
  choice rather than an unnoticed side effect.
- `--test-balance arcade` is a REQUIREMENT, not a preference: a run that
  cannot reach 20/20-adapted arcade balance logs the reason and exits **6**
  (distinct from 1 args / 3 `input_script.c` / 4 `rollback_determinism.c` /
  5 `ldreq_timing_trace.c`). It cannot fall back to PS2 and report green.
- Corpora declare `balance:` (`tools/frame-data/compile_corpus.py`); it is
  written into every `meta.json` unconditionally, and `run.sh` always passes
  it. `run-suite.sh` preflights the romset once instead of REDing N times.

Verified: `/Users/sb/Developer/fbneo-replay-runner/roms/sfiii3nr1.zip` carries
all four SIMM slices matching the SHA-256s pinned at
`src/arcade/rom_load.c:42-45` (`sfiii3.zip` in the same directory matches
none). Under the harness it yields `Arcade balance auto-selected: CPS3 ROM
verified, 20/20 characters adapted (digest eab701778c8b20ad)` — the same
digest the device reports. Negative control, same command with no romset:
exit 6, `--test-balance arcade was requested but arcade balance is
UNAVAILABLE`.

### 2. The RED proof — re-runnable, and it fails as required

`corpus-yun-sa3-arcade.yaml` reaches `sag_union_1`. Re-apply the pre-`ad411df5`
sign at that function's activation arm (`store -= 1` → `store -= -1`),
rebuild, and run it:

```
shipped fix   3 PASS                                        GREEN
flip reverted yun-sa3-arc-stock-spent   PASS->SHAPE  HIT->WHIFF  S 22->9  A 6->0  R 17->0
              yun-sa3-arc-stock-spent-2 PASS->SHAPE  HIT->WHIFF  S 22->9  A 6->0  R 17->0
              checker exit 1, golden.py exit 1        RED
```

i.e. with the bug restored the install re-activates from a stock that should
be empty — the shipped-hardware behaviour — and the corpus fails. A green run
with the flip reverted would have meant the arcade path still was not
executing.

`gauge_type` census, measured under arcade balance with a temporary probe in
`sag_union`'s arcade branch (probe removed; `plmain.c` is unmodified). Exactly
seven (character, super-art) pairs dispatch to `sag_union_1`, all with
`store_max=1` — independently reproducing the seven install supers named in
the report: **yun SA3, oro SA1, oro SA3, yang SA3, makoto SA3, q SA3,
twelve SA3**.

### 3. The present-mode early return — got past, not removed

`effa5.c:49-51` early-returns for `Present_Mode` 4/5, and the frame-data
preset boots training mode, so the select-timer runner is ENTERED and does
nothing. Reproduced exactly: **280 entries, 280 early returns,
`Present_Mode=[4]`, 0 ticks** — the reported baseline, to the entry.

The early return is correct game behaviour (a training select screen has no
countdown) and is NOT touched. Instead:

- `frame_select_timer_probe()` (`src/sf33rd/Source/Game/ui/frame_trace.c`),
  env-gated on `FD_SELECT_PROBE`, records one row per `effect_A5_move()`
  entry including whether it returned early. Deliberately not routed through
  `frame_trace_annotate()`, which is training-mode-gated — the exact state a
  select observation must not be in.
- `--test-select-dwell-frames N` makes the runner idle on select before
  driving cursors. Without it the runner clears select in a handful of
  frames, far short of `UNIT_OF_TIMER_MAX` (50, `src/constants.h:6`).
- `tools/frame-data/check-select-timer.py` runs both engines and checks the
  observation instead of assuming it.

Measured:

```
PASS  ps2     -- entries=622 early_returns=0 Present_Mode=[1] select_timer_ticks=11 Select_Timer=0x30->0x18
PASS  arcade  -- entries=622 early_returns=0 Present_Mode=[1] select_timer_ticks=11 Select_Timer=0x30->0x18
BASELINE training-frame-data -- entries=280 early_returns=280 Present_Mode=[4] select_timer_ticks=0
```

### Canon — regenerated, not typed

`96` corpora / `1,403` rows / `1,348` PASS / `55` XFAIL, summed from
`tools/frame-data/golden/*.tsv` by `check-canon-numbers.py`. All nine
`<!-- canon:KEY -->` markers updated with their values; `--check` reports
`status=OK ... mismatched=0 unbound=0`.

### RESIDUALS — open, not closed

- **R1. Arcade dummy BLOCK is not trustworthy.** Under arcade balance the
  port's `check_illegal_lever_data()` normalization is skipped
  (`plmain.c:52-55`), and a `dummy: stand` entry that BLOCKs under PS2 was
  observed to HIT under arcade with every numeric field unchanged
  (`corpus-smoke.yaml`'s `close-lp-block-vs-stand`). Cause not isolated. Per
  `CORPUS-AUTHORING.md` Phase 6 an unexplained divergence is a STOP, so all
  22 of `corpus-q.yaml`'s BLOCK entries are OMITTED from
  `corpus-q-arcade.yaml` rather than papered over with 22 xfails. **Arcade
  BLOCK coverage is not claimed.**
- **R2. `q-crmp-hit-capture-a/-b` measure `adv=-2` under arcade** where the
  PS2 twin measures the oracle's `-1`; outcome/S/A/R identical. Carried as
  the 2 XFAILs of the new ARCADE-VS-PORT-DIVERGENCE class with a stated
  reopen condition. A work item, not a completion state.
- **R3. Arcade coverage is 2 corpora of 96.** 49 of `corpus-q.yaml`'s 51
  non-BLOCK entries pass under arcade with the PS2-authored expectations
  unchanged, which is evidence the remaining 18 characters would port
  cheaply — but that is an argument, not a measurement. The other 18
  characters have no arcade corpus.
- **R4. `--test-balance arcade` needs a romset**, so the arcade corpora
  cannot run in an environment that has none. They RED loudly there rather
  than skipping, which is the correct failure but is not a CI story.

---

## #135 — docs/archive/, and the end of the repoint tax — CLOSED, with two carried items

Measured at `9a6a7f1f` before the change: **1,207 citation errors tree-wide**,
against 22 commits on this branch whose subject is "repoint" — 15% of a 30-hour
window spent moving line numbers. Enforcement had already been narrowed to 11
zero-ceiling scopes (#117) and that worked: every recent repoint commit touched
files *outside* those scopes. The remaining cost was voluntary.

**Moved** (`e052550e`): 17 documents into `docs/archive/`, each stamped with the
commit it was last substantively true at — derived by walking history for the
last diff that changed something other than digits, so a repoint commit cannot
pass itself off as the document being current. No citation was repointed on the
way in.

**Excluded**: `docs/archive/` is in `SKIP_SCAN_DIRS`
(`tools/doc-citations/check_doc_citations.py`). This is strictly stronger than
the RECORD class it replaces there, which left `drift` and `line-out-of-range`
as errors. Proven by `check_archive_is_not_scanned()` in
`test_doc_citations.py`: the same file planted twice in a throwaway worktree,
inside and outside the archive, requiring silence on one and a finding on the
other. Mutation-checked — removing the tuple entry makes it fail with three
findings on the inside probe.

**Written down**: `AGENTS.md` §Documentation and `CLAUDE.md` — read the archive
for *why* and for negative results, never for current facts; do not repoint
citations in unenforced files (`tools/doc-citations/baselines.txt` is the
enforced set and nothing else); write the assertion, not prose restating code.

### DONE 2026-08-30 — R1. `docs/plan-netplay-phase6.md` is archived

It self-declared "**HISTORICAL — deprecated**" in its own header and the RmlUi
lobby it planned was removed. It stayed in `docs/` only because three of its six
inbound references (`src/configuration.h`, `src/netplay/netplay.c`,
`src/netplay/netplay.h`) were in files another lane held uncommitted edits in on
2026-08-30; moving it would have left dangling paths that lane could not have
been expected to fix. The other three were `include/structs.h`,
`src/netplay/test_event_queue.c`, `src/netplay/test_mist_handshake.c`.

That lane committed (`b5f03f8a`) and `src/netplay` went quiet, so this is
done: `git mv` into `docs/archive/`, the standard stamp (*true at* `a752e2ca`
— its last substantively-true commit; `21ea5411` was a citation repoint and
`e052550e` only rewrote its two parent-document paths, and neither may
masquerade as the doc being current), and all **eight** inbound path references
updated — the six above plus `docs/plan-fcade-replay-browser.md` and
`docs/plan-stun-direct-p2p.md`, which both cite it with line ranges.

Those two line ranges are deliberately **not** repointed even though the stamp
shifted the archived file by four lines. Same call the 17-document archive
commit made and wrote down: moving a document must not itself become a repoint
commit, and `docs/archive/` is not scanned at all, so nothing there is
checkable any more.

**Where to find it:** the move landed inside `c1233209`, whose subject is
*"docs(queue): repoint the #108 romset citation to the digest lines
themselves"* and says nothing about it. Two lanes were committing in the same
checkout, and the other one ran an **unscoped** `git commit` while this change
was staged, so it swept all ten files in under its own message. Nothing was
lost or crossed — both lanes' content is intact in that commit — and it was
left unrewritten on purpose: amending shared history under a live concurrent
lane trades a cosmetic problem for a real one. Recorded here because
`git log -- docs/archive/plan-netplay-phase6.md` otherwise leads to a subject
line that disclaims the change. **The rule that would have prevented it:**
always `git commit -- <paths>`, never bare `git commit`, in a shared checkout.

### CARRIED — R2. the black-BG bug is OPEN and its four documents say so

Not a docs item — a defect item surfaced by classifying them. `Fix B` shipped
(`src/sf33rd/Source/Game/stage/bg.c:295-315`, forced texture-handle teardown
before repopulate) but `docs/fix-plan-bg-texture-rollback.md` demotes it to a
safety net in its own post-mortem, because `Bg_Texture_Load_EX` is not reached
on most resim frames. Its named winner is **Fix E.3** — force `bg_routine = 0`
at the top of `TATE00` when the cache is detectably torn down. That guard does
not exist: `src/sf33rd/Source/Game/stage/tate00.c` has three pre-2026 commits
and `TATE00()` is the unmodified "before" state the doc quotes. The bug appears
nowhere in this queue and has no owner.

### RE-LITIGATED AND LEFT — `load_it_use_this_key`'s unbounded retry (gd3rd.c)

`src/sf33rd/Source/Game/io/gd3rd.c:355-380`. `while (1)` around
`fsOpen` / `fsFileReadSync`: a read that keeps failing logs
`ファイルの読み込みに失敗しました。ファイル番号：%d` (`:378`) and retries
forever. Raised again on 2026-08-30 as a brick-prevention gap.

**It is not a gap. It is a decision.** This exact site — named by function, not
by line — was audited in the 2026-04-29 arcade `while(1)` trap sweep, which
replaced 12+ such traps across `texgroup.c`, `ramcnt.c`, `PPGFile.c` and
`gd3rd.c` itself (`load_it_use_any_key2`'s fnum-out-of-range trap and its
`Pull_ramcnt_key` guard both became `[gd3rd-skip]`, `docs/netplay-diagnostics.md:180-186`).
`load_it_use_this_key` was put on the sweep's **deliberately deferred** list by
explicit user choice, on the reasoning that it is *not a brick*: unlike the
empty `while(1){}` traps, it logs on every iteration, so a stuck load leaves a
growing trail rather than a silent freeze. At the sweep-era tree
(`3f020a54:src/sf33rd/Source/Game/io/gd3rd.c`) the function is at `:199` and
the loop at `:204`; it is textually unchanged since.

Recorded here because until now the decision lived only in a session memory,
which is why it came back around. **Do not "fix" it without re-opening the
decision with the user.** For whoever does re-open it, the facts that bear on
the terminal behaviour:

- Callers cannot escalate. `load_it_use_any_key2` (`gd3rd.c:331`) turns a
  non-zero return into `return size`, and `texgroup.c:558` assigns the result
  to `rnum`. Neither has a "load failed, abort" path to reach.
- **There is no error-reporting mechanism left to reuse.** The arcade
  `ERR_STOP` macro was deleted during the same sweep; every surviving mention
  is a comment recording that it used to be there (`ramcnt.c:59`, `:79`, `:127`,
  `:141`, `:155`, `:200`, `:232`; `texcash.c:502`). The established convention
  is the `[*-skip]` log-and-continue marker, gated behind
  `ENABLE_PERF_TELEMETRY`.
- The trigger is gone. `522e574c` root-caused the five boot-time failures that
  were exercising this loop to a lost wakeup in `AFS_ReadSync`
  (`src/port/io/afs.c`), not to bad data — see "#134" above. The loop now fires
  on genuinely corrupt or truncated `SF33RD.AFS` only.

### CLOSED — the never-existed citation class, and why it is not getting a gate

A citation can fail in three ways, and the linter only reports one of them. It
finds an **anchor** — a symbol named in the prose beside the citation — and asks
whether the cited line mentions it. If not, it looks for that anchor elsewhere
in the same file and reports `drift`, carrying the line it found. When the
anchor appears **nowhere in the cited file** there is nothing to exhibit, so it
stays silent by design — the `best is None` branch in the path checker. Those
are citations that misdirect a reader and that nothing will ever flag.

Measured by instrumenting that branch: of 2,510 citations making a checkable
claim, 1,141 clean, 1,002 merely moved, 99 whose anchor is too common to
localise, and **268 never-existed**. By scope: **51 in `docs/archive/`** (not
scanned, not maintained, left alone), **141 in live prose**, **76 in source
comments** — 217 live.

All 217 were hand-audited. **106 were false positives** of the heuristic and
**110 were real**; none was undeterminable. 91 are repaired in `d1a8da8d`; 19
were in files other lanes had open and are handed off below.

**The false-positive rate is the finding.** 106/217 = 48.8% overall, splitting
hard by corpus: 38% in prose documents, **73.8% in source comments**, where a
comment's own vocabulary gets scraped and then tested against a different file.
Precision as a *detector* is far below even that: in one batch only 1 of 9
defects was actually surfaced BY the never signal — the other 8 had a real
anchor present in the target file and were ordinary `drift`; they were found by
reading the prose, not by following the signal. **So this class is not becoming
a check.** A gate that is wrong half the time gets disabled, which is worse
than no gate.

Three false-positive causes are structural and would defeat any tightening:
citations into JSON/CSV oracle files, where symbol matching is meaningless;
citations naming another repository or a sibling worktree; and **dated
measurement records** — the fortify blind sweep cites a `snprintf` diagnostic
and pins the measurement to tree `ed37cb42`, where that line really is the
offending call. Repointing it would falsify the record, and nothing mechanical
tells that apart from a defect.

**What IS worth a one-off sweep** is the narrow slice where the target lies
outside `src/` and `include/`: `CMakeLists.txt`, `build-deps.sh`,
`tools/**/*.js`, `vendor/`, `third_party/`. There the rate inverts — 8 of 11
such items were real — because nothing in this tree line-indexes those files and
they grow by hundreds of lines between citations. Every audited citation into
the rendezvous server was stale, each by a *different* amount, and each was
exact at its own citing commit. That is a sweep, not a standing gate.

**Two escalations were checked and are NOT gate holes.** Both were reported as
"the `src` anchor-required ERROR gate is being bypassed"; neither survived.

- *The bare-shorthand form.* Claim: citing `game_state.c` by basename plus a
  line number slips past the gate that catches the full path. It does not.

**CLOSED by `2178b7da`.**

Both `process_session` cases now latch a reason before teardown, through the
same notify/park path the connect-phase failures use. Two codes appended to
`ConnectFailCode`: `CONNECT_FAIL_PEER_DISCONNECTED` ("Opponent disconnected.")
and `CONNECT_FAIL_DESYNC_DETECTED` ("Session desynced. Ending match.") — 22
and 31 chars against a real 48-char ceiling (8px cells, 384px canvas,
`SSPutStrTexInputPro`).

Kept in the same taxonomy, not a parallel one: the enum already carries
post-handoff session-gate failures and `DirectP2P_NotifySessionFailed`'s doc
comment already generalized to "ANY post-handoff session failure".
`direct_p2p.c` needed no plumbing — the park mechanism was already generic
over the code.

The reason demonstrably reaches the player, traced link by link: notify sets
`s_status`/`fail_code`/latch at frame N; `handle_disconnection` runs
`Soft_Reset_Sub` and sets EXITING; frame N+1 the teardown callback consumes
the latch and parks `DIRECT_P2P_FAILED_HANDSHAKE` instead of IDLE; from N+2
`NetplayScreen_Render`'s `ns==IDLE && DirectP2P_GetState()!=IDLE` branch draws
"ERROR" plus the reason at priority 1, above attract and PRESS ANY BUTTON.
Nothing clears it on the way out — the state is terminal, the only production
`DirectP2P_Cancel` caller is the user leaving the network menu, and surviving
`Soft_Reset_Sub` is the documented R-1 design of the park.

Notify is first-wins. One event batch carrying both a disconnect and a desync
ran both notifies, last winning the text. A second reason is now dropped while
one is latched. This cannot swallow a better reason: the MIST reject and
CONNECTING-timeout paths each set EXITING inside their own `switch` case and
never reach the event loop. The latch is cleared at teardown AND at
`BeginHost`/`BeginJoin`/`Cancel`, so nothing leaks across sessions — checked
because `DirectP2P_NotifySessionRejected` routes through
`NotifySessionFailed` and inherits the guard.

Pinned by three tests in `test_bilateral_punch.c`: 42 (each code parks
FAILED_HANDSHAKE, never IDLE, with distinct text), 43 (the extracted
`session_fail_code_for_event` mapping, including unrelated events staying at
NONE), 44 (first-wins, with the guard observably firing).

**RESIDUAL — the call wiring is NOT pinned.** Deleting both
`DirectP2P_NotifySessionFailed` calls in `process_session` leaves tests
42/43/44 green: they drive the mechanism and the mapping directly, never
through `process_session`, which has no test seam. Closing this needs that
seam plus two real UDP peers carried past the MIST handshake — new scope, not
smuggled in here. The overlay's actual rendering for this flow is also
unverified by automation and belongs to an on-device test.

Event queue untouched — its removal is #152.

Gates GREEN; `arm-cross-build` NOT RUN. `check_baselines.py`: `scopes=11
breached=0 slack=0` after repointing five citations these edits drifted in
`docs/plan-netplay-connection.md`.
  `Repo.resolve()` returns the shorthand as an `exact` hit on
  `src/netplay/game_state.c` when the basename is unique, so both spellings
  reach the anchor-required check identically. Planted in a throwaway worktree,
  both produce the same `unanchored-citation` error. The real cause of the
  silence in the `mtrans.c` comment is that it is *not* anchor-free — it names
  `reserv_add_y`, so `unanchored-citation` correctly declines to fire — and
  `reserv_add_y` occurs zero times in `game_state.c`, so the drift check goes
  silent. Never-existed, correctly sighted and misattributed. Positive control,
  same worktree and same cited line: an anchor that IS present elsewhere in the
  target (`Pause_Down`) produces `drift`. Silence tracks the anchor's absence
  from the target file, not the citation's spelling.
- *The `strcmp` citation in `test_punch_predicates.c`.* Claim: a visible drift
  sitting unreported. It was reported all along, as `degenerate-target` rather
  than `drift`, because task #133 shifted `late_punch.c` and left that line a
  lone brace. Repaired in `ce463fb6`, anchored to `LatePunch_HandleDatagram`
  rather than to either of its two compare sites.

**Handed off, by owning lane.** Named by symbol, not by line: the verified
targets are in the audit, and re-deriving them is one instrumented run of the
path checker. Writing the numbers here would plant exactly the rotting list
this entry is about — and the first draft of this entry did, adding 13 findings
of its own.

- `src/netplay/*` — 10 defects over 9 sites: `direct_p2p.c` cites the miniupnpc
  install block in `CMakeLists.txt` and `handleRegister` in the rendezvous
  server; `game_state.h` cites `Color7` in `sel_pl.c`; `late_punch.h` cites the
  arcade balance digest compute and getter; `net_tuning.h` cites the SDL3_net
  pin in `build-deps.sh`; `netplay.c` cites two `G_No[1] = 6` sites in
  `game.c`; `test_connect_observability.c` cites `encodeNack`;
  `test_punch_predicates.c` cites the foreign-IP branch of `late_punch.c`;
  `test_rendezvous_wire.c` cites the NACK reason block.
- everything else — 7 defects over 6 sites: `charset.c` cites
  `frame_data_overlay_tick` and `njUserMain` in `main.c`; `gd3rd.c` cites the
  GekkoNet pin in `build-deps.sh`; `frame_data_overlay.c` cites the same two
  `main.c` calls; `ldreq_timing_trace.h` cites the `Netplay_ArmAllowed` guard
  in `netplay_nav.c`. `mtrans.c` and `texgroup_window_probe.h` share one claim
  and must name `GS_SAVE(plw)` plus the `X(reserv_add_y)` entry in
  `plw_canon_fields.h` — `game_state.c` is anchor-required, so a bare line
  number there is itself an error.
- `src/arcade/*` and `tools/frame-data/*`: **zero** true defects.
- `tools/netplay/natmatrix/rig/natpmp_mock.py` cites the conntrack sysctls for
  the `MASQUERADE --random-fully` emulation; the rule is in the `symmetric)`
  arm of `natns.sh`.

**One `--fix` trap worth recording.** In `fix-plan-bg-texture-rollback.md` the
linter's own suggestion for a `bg_w` claim points at `GS_LOAD(Screen_Switch_Buffer)`
instead of `GS_LOAD(bg_w)` two lines earlier. Applying it would make the
citation *look* repaired and leave it wrong — a fresh instance of why `--fix`
is banned.

## #136 — #108's two residuals, both isolated — CLOSED

Task #108 left two things as "cause not isolated": a dummy that blocks under
PS2 and takes the hit under arcade (which cost 22 BLOCK entries), and two rows
measuring `adv=-2` against an oracle of `-1`. Both are now isolated to a
specific line, and both were harness defects rather than engine divergences.
It also left "the other 18 characters port cheaply" as a labelled argument;
that is now partly measured.

Everything below is measured on this tree.

### R1 — the dummy could not block, and it is one missing term

`dummy: stand` writes training guard slot 2 (ALL GUARD), which clears
`DIP_AUTO_GUARD_DISABLED` (`effe3.c:211-216`). Only `defense_ground_ps2` reads
that bit back — as `ags`, `hitcheck.c:1365-1366`, consumed at `:1496` as
`if (!ds->auto_guard && !ags && ...)`. `defense_ground_cps3` never mentions it;
its gate is the bare `if (!ds->auto_guard)` at `hitcheck.c:1309`. And
`Control_Player_Tr()` forces the dummy's input word to neutral for
`DUMMY_ACTION_STAND` (`menu.c:3798-3827`), so `saishin_lvdir & gddir` is false
and `defense_ground_cps3` returns 2 — took the hit. Reproduced on
`corpus-smoke.yaml`'s `close-lp-block-vs-stand`: `outcome=BLOCK` under
`--test-balance ps2`, `outcome=HIT` under `--test-balance arcade`, every
numeric field identical.

So: category "the dummy isn't in a blocking state", not "arcade guards
differently" and not an engine bug. `auto_guard` is the field CPS3's own
defense path provides for exactly this; nothing sets it in a normal round
(`player_mv_0000` zeroes it, `plmain.c:123`; only the bonus-stage init sets it,
`plmain2.c:101`) because CPS3 has no training mode to set it from. The harness
now sets it in `input_script_apply_guard_mode()`
(`src/test/input_script.c`), re-asserted per tick because the PS2 paths zero it
whenever `Play_Mode != 0` (`hitcheck.c:1106`, `:1371`).

**Restored: all 22.** `corpus-q-arcade.yaml` goes from 51 entries to all 73 of
`corpus-q.yaml`'s, 73 PASS / 0 XFAIL, and its parsed table is row-for-row
identical in content to `golden/q.tsv`. No `expect:` was re-derived or relaxed.
`corpus-hugo-arcade.yaml` adds 10 more BLOCK entries, Twelve 6, Remy 5 — 43
arcade BLOCK rows where there were 0.

**One genuine engine difference did fall out**, and it is the interesting part:
arcade requires a LOW to be blocked CROUCHING. `defense_ground_cps3`'s `case 8`
demands the crouch bit (`hitcheck.c:1320-1321`); `defense_ground_ps2`'s same
case carries an `&& ags == 0` escape (`:1508`, and `:1516` for the overhead
case) that lets a STANDING ALL-GUARD dummy block a low. Six entries across the
four corpora needed `dummy: crouch` where their PS2 twin got away with
`dummy: stand` (Q 2, Hugo 2, Remy 1, Twelve 1). That is stated at each entry, not papered over.

### R2 — `adv=-2` was a dead dummy, not a balance change

Neither "the arcade oracle is -2" nor "an off-by-one in the adv measurement".

`check_omop_vital()` — the port's EXTRA OPTIONS vitality restore
(`plmain.c:1134-1244`, fed by `sysdir.c:126-127`) — is arcade-skipped at
`plmain.c:335-337`. Under PS2 it walks the dummy back to 160 during every
`inter_entry_wait`; under arcade the dummy's health only ever goes down, and a
73-entry corpus grinds it to zero partway through. `same_dm_stop()` then fires
its nearly-dead branch — `(ds->vital_new - ds->dm_vital) < -2`,
`hitcheck.c:1023-1039` — and overrides the defender's hitstop with
`-att.hs_me`. Measured with a temporary probe inside `same_dm_stop`, on
`q-crmp-hit-capture-a`:

```
arcade  vnew=0    dmvital=10  delta=-10  hsme=9  hsyou=-10  fires=1
ps2     vnew=160  dmvital=13  delta=147  hsme=9  hsyou=-10  fires=0
```

`att.dipsw`, `att.hs_me` and `att.hs_you` are IDENTICAL across the two engines
at that call; the only differing input is the dummy's vitality. `dm_stop`
becomes -9 instead of -10, the dummy leaves hitstun one frame early
(`def_idle_F` 5818 vs 5819 against an identical `atk_idle_F` 5820), and cr.MP
reads `adv=-2`. Corroborated three ways before the probe: the same corpus text
with only `balance:` flipped gives -1 under ps2 and -2 under arcade (so it is
not corpus composition), and the entry run STANDALONE gives -1 under BOTH
engines (so it is not the move).

Fixed in the harness (`input_script_restore_vitality()`, called at each entry's
`L` directive), not by re-enabling `check_omop_vital` under arcade — that call
is a shipping-behaviour decision the harness has no business changing. Both
rows now PASS at the oracle's -1. **Blast radius, since it was a real
measurement error:** any arcade entry after the dummy hits 0 vitality, on an
attack whose `att.dipsw & 1` is set and whose `hs_me != -hs_you`. In
`corpus-q-arcade.yaml` that was exactly these two — the two other
already-fired calls (`hsme=8 hsyou=-8`) produced the same value either way,
which is why the defect showed up as 2 rows and not 20.

### R3 — the extrapolation, bounded rather than extended

Not a port of 18 characters. Three characters picked for distance from Q and
Yun, run under arcade AND under ps2 from the identical file (the control that
separates engine from corpus composition, since a slice does not reproduce a
full corpus's carried state):

| corpus | scope | arcade | vs its own ps2 run |
|---|---|---|---|
| `corpus-hugo-arcade.yaml` | all 30 of `corpus-hugo.yaml` — heaviest body, command-grab machinery (SDB/Moonsault/Meat Squasher) | 30/30 PASS | byte-identical, and also identical to committed `golden/hugo.tsv` |
| `corpus-twelve-arcade.yaml` | 17 of 59 — A.X.E., N.D.L., normals, UOH, throw, jump control | 17/17 PASS | byte-identical |
| `corpus-remy-arcade.yaml` | 16 of 56 — Light of Virtue, Rising Rage Flash, command normal, UOH, throw, jump control | 16/16 PASS | byte-identical |

Across those 63 entries exactly ONE class of arcade-vs-port divergence appeared
— the low-block stance rule above, 4 entries (Hugo 2, Remy 1, Twelve 1). Everything else ported with its
`expect:` untouched.

**Claimed after this work:** arcade coverage on 5 corpora / 139 rows, all PASS
(Q 73, Hugo 30, Twelve 17, Remy 16, Yun SA3 3). **Still extrapolation:** the
other 15 characters, and the ~82 Remy/Twelve entries outside these slices. The
extrapolation is better supported than it was — it now rests on 139 measured
rows spanning 5 characters including a full non-Q corpus, instead of 49 rows of
one character — but it is still an argument for the remaining 15.

### Two harness hazards found on the way, both BALANCE-INDEPENDENT

Neither is an arcade property; both reproduce under `--test-balance ps2` and
are simply invisible in the existing PS2 corpora because no PS2 entry sits
where they bite. Both are now written into `CORPUS-AUTHORING.md` Phase 4a.

1. **Fresh-DOWN parry window.** Forcing the dummy to crouch drives a new DOWN
   edge on the frame the `G` directive runs, opening a low-parry window. An
   attack with a very short startup lands inside it: `h-crshort-block` (S=3)
   read `PARRY adv=-11` instead of `BLOCK` under BOTH engines. `wait 20;`
   prepended to the input closes it; `h-crforward-block` (S=8) was already
   clear.
2. **Stance carryover into the next entry.** A `dummy: crouch` entry leaves the
   dummy crouching into the FIRST entry that follows. A 3-entry control corpus
   `[q-crmk-multimove-merge(crouch), crmp-a, crmp-b]` measured `adv=+0` for
   crmp-a and `-1` for crmp-b under BOTH `--test-balance arcade` AND
   `--test-balance ps2`, and `-1/-1` under both with that entry `dummy: stand`
   or absent. +0 is exactly `q.json`'s `Crouch_hit_advantage` for Crouching
   Strong. Handled in `corpus-q-arcade.yaml` by moving the one affected entry
   past the pair.

   NOT fixed in the harness. The real fix is to apply the next entry's `G`
   BEFORE the `inter_entry_wait` rather than after it, so the dummy settles into
   its stance during the wait — but that would re-measure every existing PS2
   corpus that has a crouch entry followed by another entry, which is a
   different (and larger) piece of work than this task. **Reopen condition:** if
   a future corpus needs a `dummy: crouch` entry immediately before an entry
   whose `adv` it asserts, this is the thing to fix rather than to route around.

### Canon

Summed from `tools/frame-data/golden/*.tsv` by
`tools/frame-data/check-canon-numbers.py`, never from a summary line:

  **corpora 96 -> 99, rows 1,403 -> 1,488, PASS 1,348 -> 1,435, XFAIL 55 -> 53.**

The XFAIL count returns to the pre-#108 53. None of those 53 moved in either
direction across #108 or #136. `run-suite.sh --check-golden` on the committed
tree: 99 GREEN / 0 RED, zero drift, 13m41s wall.

### Task-number correction

This lane is #136. Its first three commits (`04f1b3a5`, `58e2f021`,
`6fcc4c0d`) carry `[task #133]` in their subject lines, which is wrong — #133
is the live netplay both-caps item above. The number is corrected in every
file; the three subject lines are left alone because another lane committed on
top of them before the mistake was caught, and rewriting shared history to fix
a subject line is not worth orphaning someone else's work.

### OPEN — section A's confident positive was never tested either, and 16 allowlist entries rest on its method

The citation audit flagged `research-desync-deep-investigation.md` for placing
its most load-bearing conclusion "inside `pl_step_25`", a function that has
never existed. Repairing the pointer was the smaller half. Checking the claim
under it produced three findings, recorded in a STATUS block at the top of that
document and summarised here.

**The mechanism is real; only the names were invented.** `chainex_check[2][36]`
is declared in `system/sysdir.c`; nine speculative writes live in
`check_full_gauge_attack`, `check_full_gauge_attack2`,
`check_super_arts_attack_dc` and `check_special_attack` in `engine/pls03.c`,
with eight gating reads in the same file and `clear_chainex_check` in
`engine/plcnt.c`. The callers are the `nm_NNNNN` normal-state family in
`engine/pls00.c` dispatched by `process_normal` — that is the state machine
`pl_step_25` was standing in for; nothing named `pl_step` exists in `src/` or
`include/`. The April line numbers were exact at `17ab61e7`, verified by
reading that blob rather than inferring it.

**The conclusion was never tested.** Section G proposed detection FIRST — hash
`chainex_check` without saving it and read the answer off which way the desync
frame moved — and said in as many words that adding save/restore "would be the
FIX. We want the DETECTION first." G.1 was skipped and G.5 shipped:
`chainex_check` has been a `GameState` member since 2026-04-24. So Candidate R2
was neither confirmed nor eliminated, and the frame number the whole
investigation is named for appears in no other file in this tree. **Not a
defect to fix — a conclusion that is still open, currently written down as
settled.**

**And in netplay it is now inert.** Upstream `4485a438` ("Statcheck: Fix arcade
mode desync", #267) put all nine writes behind `!ArcadeBalance_IsEnabled()`,
and netplay then became arcade-only via `Netplay_ArmAllowed`. In any netplay
session `chainex_check` is provably an all-zero array and the eight gates never
trip. It stays live only under the PS2 balance `ArcadeBalance_Init` pins for
`--test-enable`.

**What would settle it, offline, no device.** Drop the `chainex_check` restore
from `GameState_Load` (leave the field and the save), run the
rollback-determinism harness `--thorough` on macOS — it runs PS2 balance, so
the writes are live — and see whether `chainex_check` lands in the divergent
set. If it never diverges across the 21 scenarios, that is not an exoneration:
it means the scripted supers never reach an EX-SA *chain* branch, which a
counter on the nine write sites settles in one run. Either way it is the first
measurement this candidate has ever had. The harness is macOS-only by policy
and is somebody's gate, so this is a scheduled experiment, not a drive-by.

**What actually rests on section A.** Not the harness's existence —
`rollback-determinism-harness.md` cites §A as the method it replaces, naming
the `spmv_ng_save` false negative, and that framing is now recorded as
two-sided. The real dependency is `tools/rollback-determinism/allowlist.txt`:
**16 of its 40 entries** come from §A's per-subsystem classification — the
sound-sink block from §A.2.3, the render/texture/palette block from §A.2.2 and
§A.2.4, the rumble block from §A.2.2. Those entries suppress harness failures
on the strength of a hand "write-only fan-out, cannot feed back" judgement —
the same reasoning pattern this tree has now caught being wrong **three
separate times**: `spmv_ng_save` exonerated on the wrong character enum; the
loader block's own `CORRECTION 2026-08-24` recording that
"consumed by the loader, not read by the simulation" was false for
`ldreq_result` "and it is the reason this block hides a real desync class"; and
`ColorRAM` — an entry inside one of the §A blocks — carrying a PARTIALLY
EXEMPT note because the `effl8` row-prefixes fed back into saved `frw[]`
bytes. The loader block is not §A-sourced; the shared thing is the judgement,
not the citation. Three for three, against an allowlist whose own header says "when in doubt, leave it OUT — a
catalogued finding beats a hidden one". **Nothing here is known wrong. The
point is that its provenance has a measured error rate and the entries have
never been re-derived from anything but the document.** Re-deriving the 16 is a
bounded job: each needs one answer, whether any simulation read reaches it.
(The harness document's two other §A cross-references — §A.3.7 for per-peer
`extra_option`/`system_dir` seeds, §A.3.1 for the `rwd_ptr`/`brw_ptr` walk —
are informational, not premises. Nothing else in `docs/`, `tools/` or `src/`
cites this conclusion.)

**Citations left alone deliberately.** The line numbers throughout the desync
document were exact at their citing tree and it is not in
`tools/doc-citations/baselines.txt`, so per `AGENTS.md` they are not
mass-repointed. What was corrected is the prose that is now FALSE: the
"NOT in `GS_SAVE`" claims for `chainex_check` and `ca_check_flag` (both saved
since 2026-04-24 and `7e07db3f`), the invented `pl_step` family, and the
headline one-liner, re-anchored to `check_full_gauge_attack`.

### CLOSED — the outside-src slice, swept once; and what the linter cannot see

The never-existed audit deferred one slice for a one-off sweep: citations whose
TARGET lies outside `src/` and `include/`, where 8 of 11 audited were real. Run
in full: **232 citations, 51 citing files, 40 target files** — `tools/**` 115,
`docs/**` 40, `CMakeLists.txt` 34, `vendor/` 15, `AGENTS.md` 14,
`third_party/` 8, `build-deps.sh` 5, `README.md` 1. All 232 hand-audited.
**~55 real defects carrying 61 wrong pointers**, ~148 correct, ~29 owned by
other lanes, none undeterminable in the fixable set. The inversion holds.

**The cause is mechanical and will not fix itself.** `is_symbol_candidate`
requires an identifier of six-plus characters **containing an underscore**. The
rendezvous server is camelCase — `touchSlot`, `notePushLost`, `encodeNack`,
`slotReclaimable` — and so are most CMake, shell and SystemVerilog identifiers.
No citation into `rendezvous-server.js`, `CMakeLists.txt`, `build-deps.sh` or
`thirdsarm_wrapper.cpp` can be anchored, therefore none can drift, therefore
they rot in permanent silence. This is not a tuning gap; it is the anchor rule
meeting a naming convention. **Still not a gate** — relaxing the underscore
rule would flood every prose document with English words — but it is why this
slice needs re-sweeping when those files move again, and why a citation into
them should carry a symbol name in the prose even though nothing checks it.

Worst of the 55, by what a reader would have concluded: the analog-CRT memory
pointed a thousand-odd lines short of the `video_refresh_yc_mode()` call that
runs *before the game starts*. That call is
`vendor/Main_MiSTer/thirdsarm_wrapper.cpp:2905`; the file also holds two
**teardown** calls to the same function, at `:2322` and `:2392`. A reader
following the stale number would have read the bug backwards. Second: a `CMakeLists.txt` citation whose *spelling* had gone
stale as well — `$<$<CONFIG:Debug>:DEBUG>` is now
`$<${DEBUG_HOOKS_GENEX}:DEBUG>`, so the prose was corrected, not the pointer.

Left as records with reasons stated in place: JSON oracles (every one checked
out); commit-pinned measurements, including `plan-fcade-replay-browser.md`'s
`menu.sv` numbers, which say in the document that they are at `54c95d13`; foreign trees (`pr-243:`, `upstream/main:`);
and the `plan-bilateral-hole-punch-review.md` rows that quote superseded plan
text — though the present-tense "Actual:" assertions beside them were
repointed. All 22 `AGENTS.md` and `README.md` citations were already correct.

**One unresolved.** `docs/plan-stun-direct-p2p.md` cites
`docs/archive/plan-netplay-phase6.md:433-437` for an `r_no[1]=6` routing claim;
that range is now a file list. `docs/archive/` is unscanned and unmaintained,
so it is flagged rather than guessed at.

**The 19 handed-off never-existed repairs are done** — ten in `src/netplay`
(including the `chainex_check` field comment, which cited two `pls03.c` lines
that have never held that field in any revision), seven elsewhere, plus the
`natpmp_mock.py` conntrack claim. Two mechanical lessons that cost a rework
each and are worth knowing before the next batch:

- **Repairs must be line-count neutral.** Growing a heavily-cited file rots
  every citation below the edit. One `src/netplay/game_state.h` repair
  genuinely needed the space and moved `chainex_check` eleven lines, which
  broke a pointer in `fix-plan-bg-texture-rollback.md`; that one was followed
  and repaired. `docs/plan-netplay-connection.md` is an enforced scope at
  ceiling 0 and cites `test_bilateral_punch.c` by line, so a one-line growth
  there would have breached a gate.
- **Anchor ownership splits only on a full-path citation**, because
  `anchor_tokens` builds its list with `RE_PATH_CITE`, which requires a slash.
  Two bare `main.c:NNN` numbers in one sentence share every anchor and
  manufacture a false drift; spelling one of them `src/main.c:NNN` separates
  them.

## #137 — the black-BG fix that was never implemented, and the punch-gate charge — IN PROGRESS

Taking `build/host-bgfix` (a private configure dir, NOT the shared `build/host`)
for the rollback-determinism-style repro of the black-BG symptom. The shared
`build/host` tree is left alone for the allowlist lane. Working-tree edits are
confined to `src/sf33rd/Source/Game/stage/tate00.c`,
`src/netplay/direct_p2p.c`, `src/netplay/test_punch_predicates.c` and this file
while that lane runs.

### CLOSED — the 16 §A-derived allowlist entries, re-derived; three failed

Answers the OPEN item above ("section A's confident positive was never tested
either, and 16 allowlist entries rest on its method"). Each of the sixteen was
given the one question that matters — does any simulation read reach it — and
the read traces now live per-entry in `tools/rollback-determinism/allowlist.txt`
rather than in a document that drifts. **No suppression was removed**; three
need a decision first.

**Provenance, and how weak it turned out to be.** The 16 are the whole of three
blocks: sound (`ram`, `sdeb`, `sdbd`, `cseSysWork`, `PhdAddr`, `gpTsb`),
render/palette (`palettes`, `ColorRAM`, `colPalBuffDC`, `latest_texture_spec`,
`mts_ok`, `texcash_melt_buffer_mem`, `tpu_free_mem`, `sa_frame`), rumble
(`ppwork`, `vib_req`). All 16 have been there since `837e2ba0` and the
provenance IS recorded — as a §A citation on each block header, never
per-entry. Checking it: only **five** (`ColorRAM`, `colPalBuffDC`, `mts_ok`,
`ppwork`, `vib_req`) are named anywhere in §A.2.2/.3/.4. §A.2.3 names `bgm_*`
and `adx_*`, not one of the six sound entries; §A.2.4 names `PrioBase`/`cmtx`/
`bg_priority`/`TopHUD*`, not one of the eight render entries. And **eight** sit
outside §A.2's stated sweep universe (`src/sf33rd/Source/Game/`) entirely —
`ram` (`port/sound/`), `cseSysWork`/`PhdAddr`/`gpTsb` (`AcrSDK/…/CapSndEng/`),
`palettes`/`latest_texture_spec` (`platform/video/software/`),
`texcash_melt_buffer_mem`/`tpu_free_mem` (`src/main.c`). Eleven of the sixteen
were category extrapolations from a block header, not findings.

**12 CONFIRMED write-only, each with its read trace recorded.** `ram`, `sdeb`,
`sdbd`, `PhdAddr`, `gpTsb`, `palettes`, `latest_texture_spec`, `colPalBuffDC`,
`sa_frame`, `texcash_melt_buffer_mem`, `tpu_free_mem`, `vib_req`. Two reason
strings were wrong about the mechanism while reaching the right verdict:
`latest_texture_spec` is not a batching memo — nothing ever compares it, so no
stale value can gate work; and nothing in the pad pipeline reads `vib_req`.

**3 WRONG.**

- **`cseSysWork` — a live feedback path, and the highest-value finding here.**
  `SpuBankId[]` is read by `cseGetIdStoredBd` and gates the head of
  `q_ldreq_color_data`'s `type == 10` arm (`rendering/color3rd.c`): taking it
  retires the request in-frame via `*curr->result |= lpr_wrdata[curr->id]`,
  skipping `fsOpen`, `Pull_ramcnt_key`, the async read and the SPU upload.
  `curr->result` is `&ldreq_result[i]`, which `Check_LDREQ_Queue_Player/_Union`
  read, which gate `Exit_6th` (`Exit_No`, `Exit_Timer`) and `Bonus_Sub`'s stage
  wait (`G_No`, `G_Timer`) — all four in `GS_SAVE`. The write that makes it
  stateful, `cseSendBd2SpuWithId`, runs on the simulation tick, so a
  speculative leg flips the bank id, the rollback does not rewind it, and the
  confirmed leg takes the other arm. **This also collapses a standing
  mis-reading**: `docs/rollback-determinism-harness.md` describes `PhdAddr`,
  `cseSysWork`, `gpTsb`, `ldreq_result`, `ram`, `rckeyctr`, `sdbd` as a
  seven-member "wall-clock cluster" of independent sinks paced by disk timing.
  All seven are written on the one `q_ldreq_color_data` type-10 path this
  branch gates. One mechanism, not seven.
- **`mts_ok`.** `get_my_trans_mode` returns `-1` on `be == 0`, latched into
  `ewk->wu.my_trans_mode` (a PLW canon field, in the saved `frw` pool, not
  sanitized) at ~110 effect sites. And `Mtrans_use_trans_mode`'s
  `if (mts_ok[wk->my_mts].be == 0) return;` sits **above** the `No_Trans` early
  return, defeating the invariant the comment two lines below it was written to
  protect, and skipping `wk->my_clear_level = 0x90` and
  `wk->current_colcd &= 0x1FF` — both `GS_SAVE(plw)` canon fields.
- **`ppwork`.** `ok_dev` gates `Convert_Buff[1][id][8] = 0` in
  `Button_Config_Sub`, and `Convert_Buff` is saved. Blocked today by three
  independent gates (`Pause_Task`'s `Mode_Type != MODE_NETWORK`,
  `cpExitTask(TASK_MENU)` in `setup_vs_mode`, and `SDL_zeroa(Convert_Buff)`
  making the write a no-op). Kept on **unreachability**, which is weaker than
  write-only-ness; a voiding condition naming all three gates is on the entry.

**1 CANNOT DETERMINE — `ColorRAM`'s residual claim.** The entry said the array
outside the saved `effl8` window has "no path back into simulation state".
A second path exists: `metamor_color_store` reads ColorRAM rows into
`metamor_original` (**not** in `GS_SAVE`), and `metamor_color_restore` writes it
back to `ColorRAM[i + wkid*16]` and `[+8]` — for `i == 0`, exactly the saved
rows, and the second arm is a cross-row launder (row 8 receives what was stored
from row 0). `effl8`'s `save_old_color_data` then latches those bytes into
saved `frw`. What is unproven is realizability: `metamor_color_store`'s only
callers are in `init_trans_color_ram` case 3, the async LDREQ completion path,
and whether a type-3 colour load can complete inside a rollback window with
Twelve in the match was not established by reading. Settle it with a harness run
on a Twelve mirror with `ColorRAM` de-allowlisted. The same false "only path"
sentence in `src/netplay/game_state.c`'s `GameState_Load` comment was corrected
in place (comment-only, line-count neutral).

**What removing the three would unhide.** `cseSysWork` should surface as
DIVERGENT ahead of `ldreq_result`/`rckeyctr` on the select-phase scenarios —
the technique that convicted `ColorRAM` (compare the suspect's `last` divergent
frame against the consumer's `first`). `mts_ok` needs a scenario that straddles
a `make_texcash_work` call with a rollback window; the two fast scenarios report
`feedback=0` today, which is scenario-bounded evidence, not exoneration — that
is exactly the state `ColorRAM` was in before `makoto-sa3-super` was added.
`ppwork` should surface as DIVERGENT with no FEEDBACK consumer, matching the
unreachability argument.

**Carried, not fixed.** `docs/rollback-determinism-harness.md` says "41
entries" in six places; the count has been 40 since `830784e2` removed
`perf_super_art_command_telemetry`. Left alone — the file is outside
`tools/doc-citations/baselines.txt` and this lane did not otherwise edit it.

## #138 — the three falsified suppressions, de-allowlisted and run — CLOSED

Took the **shared `build/host`** tree (Debug, host) for the rollback-determinism
runs; #137 was on its own `build/host-bgfix`, so the two never collided. Edit
set: `tools/rollback-determinism/allowlist.txt` and this file. **No simulation
code changed**, so the 99-corpus frame-data suite, the shipped-config build and
the ARM cross-build were all skipped — nothing this lane touched can reach them.

### The runs, and the delta per de-allowlisting

Seven harness runs, all `tools/rollback-determinism/run.sh select`
(`select_depth=8`, `select_period=8` unless stated), same Debug `build/host`
binary, `--allowlist` pointed at a copy of the file with exactly one line
removed so the delta is attributable.

| run | divergent | feedback | delta vs baseline |
|---|---|---|---|
| baseline (stock allowlist) | 4 | 0 | — |
| `cseSysWork` removed | 6 | 0 | **+`cseSysWork`** ×2 scenarios, 2 frames each |
| `mts_ok` removed | 4 | 0 | **none** — the pattern never matched |
| `ppwork` removed | 5 | 0 | **+`ppwork`** ×1 scenario, 270 frames |
| `ColorRAM` removed | 6 | 0 | **+`ColorRAM`** ×2, 2 frames each |
| `cseSysWork` removed, `--select-rollback-period 1` | 8 | 0 | `cseSysWork` widens to 19 frames; `Candidate_Buff` as documented |
| `ColorRAM` removed, Twelve-vs-Makoto | 3 (1 scenario) | 0 | `ColorRAM` gains frames 1059,1060 |

The baseline's four DIVERGENT rows were **not** escapees:
`flLogOut.bflLogOutFirst` (a diagnostic latch, now allowlisted — see below) and
`TATE00.bgx_n` (#137's uncommitted `#if defined(DEBUG)` counter in `tate00.c`,
not in `HEAD`). With this commit's allowlist the gate reports **divergent=2,
feedback=0**, and both remaining rows are `TATE00.bgx_n`. On a tree without
#137's instrumentation that is divergent=0.

### `cseSysWork` — the prediction is REFUTED, and it is NOT the LDREQ leak

The prediction on record was that `cseSysWork` would surface DIVERGENT *ahead
of* `ldreq_result`/`rckeyctr`. It surfaces **behind** them, in both scenarios
and at both cadences. First divergent frame, ryu-ken / makoto:

    rckeyctr 208/208  plt_req 208/208  afs_handle 208/208  q_ldreq 208/208
    ldreq_result 209/209  rckeymin 211/210  ColorRAM 212/213
    ram 214/215  cseSysWork 214/215  PhdAddr 215/216  gpTsb 215/216  sdbd 215/216

That is one LDREQ pipeline pass in source order, and `cseSysWork` sits where its
own writer sits — `cseSendBd2SpuWithId`, reached in `q_ldreq_color_data`'s
`case 4`, after the async read completes. So in these runs it is a **downstream
symptom** of the LDREQ phase shift, the status the entry already gives `sdbd` —
not the cause of `ldreq_result`'s divergence. It self-heals in 2 frames (both
legs write the same bank id), and the window closes 111 frames before in-game.

Adversarial control, because a 2-frame window is a weak negative:
`--select-rollback-period 1` widens `cseSysWork`'s disagreement to **19
consecutive frames** across the whole of character select, and `Exit_No`,
`Exit_Timer`, `G_No` and `G_Timer` are still byte-identical on all 1500 frames
of both scenarios. Ordering unchanged.

**Not the same mechanism as the LDREQ rollback leak — they are mirror images on
one path.** The leak is a SAVED gate (`SP_No`, incremented by `Sel_PL_3rd`) with
an UNSAVED side-effect ledger (`q_ldreq`, `texgrplds`, the rckey allocator), so
rollback *replays* the side effect and it accumulates: a heap leak ending in a
crash, invisible to this harness by construction. `cseSysWork` is an UNSAVED
gate in front of a branch whose consumer state IS saved, so rollback can make
the two legs take *different arms*: a divergence, which is what the harness is
for. Same `q_ldreq_color_data` pump, opposite save-set asymmetry; fixing one
does not fix the other. Incidentally the leak's own mechanism was observed live
in these runs — the rollback leg's log carries twelve `[ldreq-dedupe]
Push_LDREQ_Queue dropped duplicate` lines and neither baseline carries any.

**Nothing was fixed here, and nothing needed to be.** The escapee did not
realise, and the LDREQ ledger was not touched.

### `mts_ok` — null result, and it is now a reachability entry

De-allowlisted, `mts_ok` does not enter the A1-vs-B diff **at all** — byte
identical on every one of 1500 frames in both scenarios. Established by reading
why: every purge/make pair on the two slots `plw[]` draws with
(`my_mts` 3 and 4, from `setup_base_and_other_data`) is **same-frame** —
`Purge_texcash_of_list(3)` is immediately followed by `Make_texcash_of_list(3)`
at all six call sites, and `pto_list[3]`/`mto_list[3]` both name slots 3 and 4.
The one site that leaves a PLW slot purged across frames is the bonus-stage
entry in `bbbscom2.c`, and it is unreachable in `MODE_NETWORK`: `Bonus_Type`'s
only non-zero writer is `Check_Bonus_Type()` inside `Next_CPU`/Game05, which
Game03's `case MODE_VERSUS: case MODE_NETWORK:` arm diverts past — the same gate
the `plt_req` proof rests on.

Also a **correction to the correction**: only `my_clear_level` sits above the
`No_Trans` early return in `Mtrans_use_trans_mode`. `current_colcd &= 0x1FF` is
*below* it, so it is already skipped on every `No_Trans` frame by design; the
`be` gate adds a skip on non-`No_Trans` frames, which is a real risk but not the
invariant the quoted comment protects.

### `ppwork` — behaves exactly as the unreachability argument predicts

DIVERGENT without FEEDBACK, `ryu-ken-basic-exchange` only, frames 639..1477,
270 divergent frames; `Convert_Buff` byte-identical throughout. All three
voiding gates re-verified at tip: `Pause_Task`'s `Mode_Type != MODE_NETWORK`
(now further narrowed by `!Is_Training_Mode`), `cpExitTask(TASK_MENU)` and
`SDL_zeroa(Convert_Buff)` in `setup_vs_mode`, and the constant-0 writes in
`Button_Config_Sub`.

### `ColorRAM` — the row arithmetic, and the prescribed scenario was wrong

Pinned rows are **0, 8, 16, 24, entries [0..11] only** (12 of 64 u16).
`metamor_color_restore` writes rows `wkid*16 + 0..3`, `wkid*16 + 8..11` and
`mcs_sel_tbl[wkid]`, all 64 entries — so its `i == 0` arm writes **all four
pinned rows**. The sets are neither disjoint nor covered. But the only metamor
cell that can reach saved state is `metamor_original[wkid][0][0..11]`, because
`save_old_color_data` reads exactly `EFFL8_COLOR_ENTRIES` s16 from
`ColorRAM[row]` — the sole ColorRAM→`frw` read in the tree lies inside the
pinned window. Rows 1-3 / 9-11 / 17-19 / 25-27 / 504 / 508 have no reader that
reaches saved state.

**A Twelve mirror cannot settle it.** Every `metamor_color_restore` passes the
Twelve player's own slot, and `effl8`'s `master_id` is the Makoto player's own
slot; one slot cannot be both, so a mirror never spawns `effl8` at all. The
scenario is **Twelve vs Makoto**. Run: `metamor_original` and `hi_meta` go from
one distinct hash (never written) to two, so the metamor path is entered for the
first time in this harness; `ColorRAM` gains two new in-battle divergent frames
(1059, 1060); and `metamor_original` and `frw` are byte-identical between
baseline and rollback on all 1500 frames.

**Residual, narrowed and named:** `perf_metamorphose_active_frames` never left
its initial value, so the X.C.O.P.Y. *super* did not activate — only the
round-init restore in `player_mv_0000` ran. The store side is exercised and
clean; `effect_K7`'s in-battle restore is still unexercised. Settling it needs a
preset that lands X.C.O.P.Y. mid-battle and asserts
`perf_metamorphose_active_frames > 0`, then a re-run.

### New allowlist entry

`flLogOut.bflLogOutFirst` — `flLogOut`'s one-shot "write the acrout.txt banner"
latch. One read, its own `if (bflLogOutFirst != 0)`; not extern'd, address never
taken, no simulation reader. It was surfacing DIVERGENT on the stock allowlist
because the `ENABLE_PERF_TELEMETRY` `flLogOut` in `Push_LDREQ_Queue`'s
duplicate-drop guard fires only on a resim leg. Same class as the
`*.s_log_count` / `*.s_dbg_count` entries beside it. This takes the file to
**41 entries**, which incidentally makes the "41 entries" count in
`docs/rollback-determinism-harness.md` true again — it was stale at 40 (see the
carried item under #137's allowlist re-derivation above). Still not editing that
doc from this lane.

### #137 — build-tree note

`build/host` was held by the allowlist lane's `check_rollback_determinism.py`
run for most of this lane, so every measurement above was taken in private
configure dirs (`build/host-bgfix` Debug, `build/host-np137` with
`NETPLAY_TEST_HOOKS` + `ENABLE_NETPLAY_TESTS`, `build/host-rel137` Release).
`build/host` is taken only for the one thing that hardcodes it,
`tools/frame-data/run-suite.sh`, and only once that run had exited.
Edit set: `src/sf33rd/Source/Game/stage/{bg.c,bg.h,tate00.c}`,
`src/netplay/{direct_p2p.c,direct_p2p.h,test_punch_predicates.c}`,
`docs/fix-plan-bg-texture-rollback.md`, this file.

## #139 — the LDREQ rollback leak, measured instead of inferred — CLOSED, and the dedupe is not what closed it

Asked whether the `[ldreq-dedupe]` guard (`Push_LDREQ_Queue`, io/gd3rd.c) and
the #72-widened `Ldreq_BarrierActive()` close the task-50 duplicate-load leak,
or merely suppress the duplicate PUSH while the ramcnt ledger keeps
accumulating. Twelve dedupe drops in #138's rollback leg was the observation
under test, not the answer.

**Answer: the leak is CLOSED — permanent leak 0 bytes, measured — and the
`[ldreq-dedupe]` guard contributes NOTHING to that under production
conditions, where it never fires at all.** The load-bearing guard is the
`q_ldreq_texture_group()` case-2 reclaim (`rendering/texgroup.c`, the
`purge_texture_group(curr->group)` immediately before `Pull_ramcnt_key`).

**Build tree**: `build/host` was never taken. Private configure dir
`build/host-ldreq139` (Debug, stock options) for the harness runs; a private
detached worktree (removed at the end) for the instrumented and neutralized
builds. Repo edit set: **this file only** — no source changed.

### 1. What the dedupe guard actually does — and when it does nothing

It drops the push **before touching any ledger**. The `return 1` inside the
scan (`Push_LDREQ_Queue`, io/gd3rd.c) precedes the free-slot scan, the
`q_ldreq[i] = ldreq[0]` write and the `*q_ldreq[i].result &= ~masknum` clear,
all of which are below it. A dropped push therefore adds no queue slot, no
ramcnt key and no bytes. It is a clean suppression, not a suppression that
hides an accumulation.

**But it cannot fire in a live session.** Its match condition requires
`q_ldreq[i].be != 0` — the original still queued. The task-#66/#72 barrier
guarantees the opposite: while `Ldreq_BarrierActive()` is true,
`Check_LDREQ_Queue()` drains the queue to empty inside the frame that pumped
it, and `Check_LDREQ_Queue()` runs once per simulated frame (`Game_Task`,
game.c) including on every rollback resim frame
(`rbd_speculative_advance` → `njUserMain`). So by the time a rolled-back
confirm re-issues the request, every slot has `be == 0` and the scan misses.
Measured, same scenario and cadence, barrier off vs `--ldreq-barrier-force`:

| select cadence | barrier | dedupe drops | dup-transfer | reclaim |
|---|---|---|---|---|
| period 8, depth 8 | off | 7 | 1 | 1 |
| period 8, depth 8 | **on** | **0** | 3 | 3 |
| period 1, depth 8 | off | 55 | 4 | 4 |
| period 1, depth 8 | **on** | **0** | 24 | 24 |

The twelve dedupe lines #138 saw were an artifact of the harness running
without a session. In the field that number is zero. The dedupe's own comment
already says depth >= 3 hands the job to the reclaim; this adds that the
barrier makes it structural rather than depth-dependent.

### 2. What `Ldreq_BarrierActive()` covers — and what it does not

It is called from exactly **one** production site, `Check_LDREQ_Queue()` in
io/gd3rd.c, plus one telemetry probe in the same file and the two Debug
instruments (`src/test/texgroup_window_probe.c`,
`src/test/ldreq_timing_trace.c`). The three `src/netplay/netplay.c`
"references" are **prose inside `#if ENABLE_PERF_TELEMETRY` comment blocks on
`Ldreq_LogSessionProbe` calls — not call sites.** #72's widening (IDLE and
EXITING out, TRANSITIONING/CONNECTING/RUNNING in) changes *which frames drain*,
nothing about the replay window.

**It does not close the replay window, and it does not narrow it. It widens
what the replay can reach** — by draining the head before the duplicate
arrives, it is precisely what takes the dedupe out of play. The file already
says the barrier buys wall-clock invariance, not rollback invariance; this
lane's numbers are the concrete cost of that distinction.

### 3. The measurement

Per-frame ramcnt readout (`mmGetRemainder` / `mmGetRemainderMin` /
`rckeyctr` / `rckeymin` / live-key count / live-byte sum) added to
`RollbackDeterminism_FrameEnd`, in the private worktree, uncommitted and
discarded with it. The per-symbol capture hash **cannot** answer the byte
question: `rckey_work` and `rckey_mmobj` hold raw arena addresses that differ
between two runs whose argv lengths differ, which is why the driver has always
listed them as baseline NOISE. (First attempt at this got the wrong answer for
a dumber reason — `Stream.rows` are raw bytes, not per-symbol words, so
`rows[f][i]` indexes a byte. Corrected before anything was concluded from it.)

`char06-pressure-super` (Hugo — the 2026-08-24 repro), 2400 frames,
`--ldreq-barrier-force` on every run so the loader behaves as it does in a live
session:

| select cadence | frames where the ledger differs | peak transient | **permanent leak** |
|---|---|---|---|
| period 8, depth 8 (production prediction window) | 2 (208-209) | 1,722,496 B | **0 B** |
| period 1, depth 8 (a rollback every select frame) | 14 (193-209) | 3,342,400 B | **0 B** |

Final frame 2399 identical field for field in both:
`rem=3,084,800 remmin=3,084,800 rckeyctr=37 rckeymin=37 keys=26 bytes=11,070,812`.

**The transient is a phase LEAD, not a leak.** Frame by frame, the rollback
run's free-byte figure at 193-199 is exactly the baseline's at 200-202, and at
203-209 exactly the baseline's at 210+:

```
frame |    rem_A |    rem_B |    delta | keys A/B
  192 | 11212864 | 11212864 |        0 | 10/10
  193 | 11212864 |  7870464 | -3342400 | 10/11
  199 | 11212864 |  7870464 | -3342400 | 10/11
  200 |  7870464 |  7870464 |        0 | 11/11
  203 |  7870464 |  6147968 | -1722496 | 11/13
  209 |  7870464 |  6147968 | -1722496 | 11/13
  210 |  6147968 |  6147968 |        0 | 13/13
```

The rollback leg finishes the load ~7 frames early and then agrees — the same
shape as the `plt_req` phase lead #69 closed with proof. The -3,342,400 delta
is the SAME NUMBER as the original leak (3,342,336 B block + 64 B cell header);
the difference is that it now comes back.

**Minimum free memory over the whole run is IDENTICAL**: 3,084,800 B, reached
at frames 330-332 in both runs. Rollback does not cost the allocator one byte
of worst-case headroom.

### 4. The positive control — the instrument can see the leak

Same instrumented build with the case-2 reclaim neutralized to
`if (0 && curr->lds->ok && ...)`, object file and binary deleted before
rebuilding:

- **Sanity, no-rollback vs no-rollback, tip build vs neutralized build:
  identical on all 2400 frames**, final row identical. Neutralizing the reclaim
  changes nothing offline, because it only fires on a duplicate.
- **Neutralized + rollback (period 8, depth 8, barrier on): diverges at frame
  200 and NEVER reconverges** — 2200 differing frames, peak deficit 5,064,896 B,
  final `rem=51,648` vs `3,084,800`, final live bytes `14,100,720` vs
  `11,070,812`. dup-transfer 3, reclaim 0, and **27 `[ramcnt-skip]` allocation
  failures** where the fixed build has zero.

So the zero on the tip build is a measurement, not a blind spot.

### 5. The SIGSEGV — not reachable, by two independent margins

1. **The precondition never arises.** Zero `[ramcnt-skip]` lines in every
   rollback run of the fixed build, and minimum free arena identical to
   baseline at 3,084,800 B against a 393,216 B round-init request — a 7.8x
   margin, not the 135,680 B the 2026-08-24 diagnosis measured.
2. **Even with the leak restored it does not crash.** The neutralized control
   exhausts the arena (51,648 B free), takes 27 allocation failures, and still
   **exits 0** — task-50's fix 4 catches the `-1` / NULL sentinel at both
   `make_texcash_work` key0 sites and the key1 site (`rendering/texcash.c`,
   the `key0-alloc-failed` / `key1-alloc-failed` bail-outs) instead of carrying
   it into `ppgSetupTexChunkSeqs`. The crash is closed independently of the
   leak.

`errors=0` in all four harness runs of the fixed build, including the
`char06-pressure-super` scenario the harness doc still records as
"deterministically segfaults ... at round-init boundaries".

### 6. Scope note — the reclaim covers every enqueue site, not just `Sel_PL_3rd`

The 2026-08-24 note listed `Push_LDREQ_Queue_BG` (screen/sel_pl.c) and
`Push_LDREQ_Queue_Player` (screen/sel_pl.c) as siblings fix 2 does not cover.
That is wrong: the reclaim lives in `q_ldreq_texture_group()`, the **type-1
handler**, not in any pusher, so it covers all ten enqueue sites
(demo00.c, ranking.c, next_cpu.c, win.c, sel_pl.c, menu.c) uniformly.
The other handler, `q_ldreq_color_data()` (types 2-5,
`rendering/color3rd.c`), holds no single-slot key holder to strand: every
terminal arm releases `curr->key` — `init_trans_color_ram()` ends each of its
type arms with `Push_ramcnt_key(key)`, the type-10 arm releases in `case 5`,
and the `case 3` / `case 4` error arms release before resetting. A duplicate
colour load therefore allocates and frees inside the same drain, which is why
`rckeyque` and `mts_ok` come out byte-identical.

### 7. Gates

- **Rollback-determinism harness: RUN, 5 configurations** (4 driver invocations
  of `char06-pressure-super` at select 8/8 and 1/8, with and without
  `--ldreq-barrier-force`, plus the neutralized control). `errors=0`
  everywhere. Divergent rows: `bg_fastpath_scroll_x` in all four (task #137's
  black-BG bug — these runs were built at `237789a7`, i.e. BEFORE `9f2981ea`
  landed that fix, so it is expected), plus `Candidate_Buff` first=193
  frames=7 at select period 1, which reproduces the documented open red
  symbol-for-symbol, and `q_ldreq` first=193 frames=7 at select period 1 with
  the barrier forced, which reproduces the documented noise-unmasking.
- **99-corpus frame-data suite: SKIPPED, no simulation code changed.** The only
  repo edit in this lane is `docs/queue.md`. The instrumented and neutralized
  builds lived in a worktree that has been removed.
- **Shipped-config build and ARM cross-build: SKIPPED, no compiled code
  changed** in the repo for the same reason.
- **Citation baselines: run at the end** (see below).

### 8. Carried — two stale claims in `docs/rollback-determinism-harness.md`, NOT edited from this lane

Recorded rather than repaired, following the convention #137 used for the same
file:

1. The known-limit-1 paragraph says `char06-pressure-super`'s rollback run
   "deterministically segfaults when a speculative leg crosses a round
   (re)initialization", with the `ppgSetupTexChunkSeqs` `adrs=NULL` stack. Four
   runs at tip say `errors=0`. The paragraph two sentences above it already
   says that segfault is fixed as of task 50; the two halves contradict.
2. The same section says select depth is `min(--rollback-depth,
   --rbd-select-rollback-depth)` at `rollback_determinism.c:498`. The two knobs
   have been independent since #69 and the code that says so is the block
   comment in `RollbackDeterminism_PreFrame`.

### 9. Recommendation, not taken (report-before-fix)

The regression risk task 50's own commit named is still live and is now
sharper: a test written against the **dedupe** licenses deleting the guard that
actually fixes this, and under the barrier the dedupe counter is **zero**, so
any test asserting `dedupe > 0` is asserting a harness artifact. A regression
test for this bug should assert on the ledger — `mmGetRemainder(&rckey_mmobj)`
equal between a rollback and a no-rollback leg at end of run — with
`--ldreq-barrier-force` and select depth >= 3. The instrument that measures it
is ~15 lines in `RollbackDeterminism_FrameEnd`. Not landed here.

## #137 — the black-BG fix that was never implemented — CLOSED, and E.3 was not it

**The diagnosis half-survives, and the prescribed fix does not survive at
all.** Both halves were re-derived against the tree rather than read off
`docs/fix-plan-bg-texture-rollback.md`, whose pointers that document itself
has a history of getting wrong.

**What survives.** The reachability chain is exactly as written and still
holds. `Bg_Texture_Load_EX` has one gameplay caller (`bg_initialize`), which
has one caller (`ta0_init00`), which runs only on the `bg_routine == 0` arm of
`TATE00`'s dispatch and increments `bg_routine` immediately. `bg_w` is saved
whole (`GS_SAVE(bg_w)` / `GS_LOAD(bg_w)`); nothing in `game_state.c` names
`ppgBgTex`. So a cache torn down while `bg_routine > 0` has no repair path.
That is now measured, not argued. One injected `Bg_Close`-shaped tear-down
mid-match (at `TATE00` call 600 of 1171, pinned RNG, `basic-exchange` preset),
counted over the identical window in all four runs (`TATE00` call >= 602):

| | drawn | skipped |
|---|---|---|
| before fix, control  | 6840 | 0 |
| before fix, injected | **0** | **6840** |
| after fix, control   | 6840 | 0 |
| after fix, injected  | 6840 | 0 |

Before the fix `be` never returns to 1 for the remaining 570 frames. After it,
the cache is back on the very next frame.

**Why Fix E.3 would have broken the game, twice.**

- *Its predicate is wrong.* E.3 scans `ppgBgTex[0 .. scrno-1]`. The loader
  fills `ppgBgTex[stg .. stg+scrno)`, where `stg` is the first non-zero
  `stage_bgw_number[stage][]` entry. Measured on the harness's stage 11:
  `stage_bgw_number[11] = [0,1,0]`, `scrno == 1`, and the HEALTHY steady state
  is `be = 0,1,0`. E.3 would have read `ppgBgTex[0]` — legitimately empty —
  and fired on every frame of a healthy match, re-running a 1-5 ms loader and
  `bg_initialize` per frame.
- *Its action is the desync class it was written to avoid.* The trigger is a
  `be` flag, which is not rollback-covered. The action writes `bg_w` (saved)
  and, through `ta0_init00`, calls `random_16()`, advancing `Random_ix16`
  (saved). Saved state would become a function of each peer's local rollback
  history.

**What shipped instead.** `Bg_Texture_Rollback_Repair` in `bg.c`, called from
`TATE00` under `bg_routine > 0`: correct `stg`-based predicate, re-runs the
loader, and snapshots-and-restores every `GS_SAVE`-covered global the loader
writes (`bgPalCodeOffset[]`, `ending_flag`, `Screen_Switch`,
`Screen_Switch_Buffer`). A stage-bounds guard keeps a garbage restored
`bg_w.stage` from indexing off `stage_bgw_number[22][3]`, and a suppression
latch keeps a cache that cannot be rebuilt from costing a loader pass per
frame.

Proof, all on the host runner at 1500 frames of `basic-exchange`:
on a healthy match the loader still runs exactly once (2 entries in the
injected run, 1 in the control), so the guard is free on the happy path; and a
per-frame whole-image hash diff of control vs
injected-and-repaired — symbol map generated from that exact binary, both runs
spawned with ASLR disabled — shows **zero GS_SAVE-covered symbols differing**
across all 1500 frames. The only symbol that first diverges at the injection
frame is `Bg_Texture_Load_EX.s_btle_count`, the DEBUG loader-entry counter.

**Verdict on Fix B: it stays, and it was incomplete.** Not a spare safety net.
It released the four `Texture` chunks but not the two `Palette` chunks the
same function sets up, so re-entering `Bg_Texture_Load_EX` outside the
`Bg_Close` path **hung** the process in `ppgSetupPalChunk`'s
`if (pch->be) while (1) {}` re-entry trap with the Ake palette still live —
confirmed by stack sample, not inferred. Extended with
`ppgReleasePaletteHandle` for `ppgAkePal` and `ppgAkanePal`. Without those two
lines the repair above trades a black background for a wedged game, which is
worse. Anyone who had implemented E.3 as written would have hit the same trap.

**What is still NOT established, and cannot be from here.** That *rollback* is
what tears the cache down in the field.

- The document's own evidence cannot show it. The `ppgCheckTextureNumber`
  classifier budgets 64 prints per `Play_Game` transition, and both quoted runs
  saturate it exactly — 547+69+24 = 640 = 10x64, 336+24+24 = 384 = 6x64 — so
  the samples are the first 64 checks after each transition, which is precisely
  the window where the pre-match tear-down legitimately leaves `be == 0`. That
  is also why "24 FAIL:be=0" is identical before and after Fix B. The document
  separately concludes its 69 `FAIL:handle=0` are correct arcade data.
- The rollback-determinism harness cannot show it either, for two independent
  structural reasons. Its cycle gate covers only `TestRunner_IsPhaseActive`
  "game" and "character-select" and deliberately excludes the transition phases
  — which is where `Bg_Close` lives. `Bg_Close` has exactly two callers:
  `System_all_clear_Level_B` (`sys_sub.c`) and the endings path
  (`end_main.c`); of `System_all_clear_Level_B`'s call sites in `game.c` only
  one is inside the Game02 battle dispatcher, and it is `Game2_0` — the same
  frame that then reloads. The other is in `Game01`.
  And `ppgBgTex` is a 192-byte pointer-bearing struct array, so the
  whole-pointer canonicalization rule (`size == sizeof(void*)`) can never fire
  on it and it lands in the baseline-noise list — confirmed in a
  `--scenario ryu-ken-basic-exchange` run where `ppgBgTex` appears in the
  excluded-noise set and the rollback leg produced zero `FAIL:be=0`.

So the repair is justified by the latent defect it is measured to close, not
by a reproduced field trigger. **Reopen condition:** if a black-BG report
arrives with a log showing `FAIL:be=0` at a frame far from a `Play_Game`
transition, the trigger is real and worth chasing to its source; the repair
will have masked the symptom by then, so raise the classifier's budget before
concluding anything from a quiet log.

**Gates.** Full frame-data suite, `--check-golden`: **99 corpora, 99 GREEN,
zero golden drift**, summed 1488 rows = 1435 PASS + 53 XFAIL — identical to the
canon derived independently from `golden/*.tsv`. Nothing moved.

That took two passes and the reason is worth writing down, because the first
pass looks like a regression and is not one. The five `*-arcade` corpora RED as
"MISSING trace/expected" whenever `FDH_CPS3_ZIP` is unset and no romset is
installed — `run-suite.sh` says so in its own first three lines before it builds
anything. Re-run as
`FDH_CPS3_ZIP=<...>/sfiii3nr1.zip tools/frame-data/run-suite.sh --check-golden
hugo-arcade q-arcade remy-arcade twelve-arcade yun-sa3-arcade`, all five are
GREEN with zero drift (30+73+16+17+3 = 139 rows, all PASS). A bare
`run-suite.sh` on a machine without the romset therefore reports 94/5 and
"DRIFT DETECTED" from five corpora that never ran, which is not the same
statement as drift.

Also green: shipped-config Release host build, ARM cross-build
(`build-game.sh --flavor telemetry`), all 14 netplay harnesses, doc-citation
baselines 0 breached.

**Skipped, with reason: on-device.** The MiSTer lock is held by a wedged
external process. Nothing here is confirmed on hardware; the ARM cross-build
proves it compiles for the target and nothing more. Also skipped: the
rollback-determinism harness as a *verdict* on this change — it is structurally
blind to `ppgBgTex` (see above), so running it would have produced a green that
means nothing. It was used as an instrument instead: its capture, symbol map and
no-ASLR spawn are what produced the saved-state-neutrality diff.

**Two operational notes.**

- *The wait-for-the-tree poller deadlocked on itself.* A background job that
  waited for `check_rollback_determinism.py` to leave `ps` never fired, because
  the poller's own `zsh -c` command line contains that string — the python
  source is embedded in it. It matched itself and would have waited out its
  whole budget while `build/host` sat free. The brief warns about bare
  `pgrep -f`; the same trap catches `ps ax | grep` from inside the command being
  grepped for. Match on something the poller cannot contain (the process's
  binary path, or a marker file the watched job writes).
- *#138 landed while this lane's queue text sat in the working tree*, so two of
  its commits carry this lane's `docs/queue.md` appends. No source file was
  cross-staged in either direction — both lanes staged explicit paths. One
  side effect: #138's prose points at "the carried item under #137's allowlist
  re-derivation", and #137 has no allowlist re-derivation; that carried item is
  #138's own. Left as written rather than edited from this lane.

---

## #140 — the AFS fix on hardware, and the 379 ms link REFUTED — CLOSED

Device `192.168.1.171`. Runtime built from committed `9a597cd7` in a worktree
(`--flavor both`, `mode=arm-cross-build`, `platform=linux/arm64`); `HEAD`
`cd4ae0e5` differs from it only in `docs/queue.md`, so the binary is
content-identical to `HEAD` for every compiled source. Deployed binary on
device `md5 1b6dc20f37a720b563345709301b6702`, 4,521,348 bytes — byte-identical
to that build's `build/mister-telemetry-package/bin/3s-arm`.

### 1. The five failures are gone

Fresh boot, `timeout 45 scripts/launch-osd.sh`, 136 log lines:

```
--- AFS load-failure count ---
0
--- the five fnums, any mention ---
(none)
```

Three further runs of the same binary: `failures=0` each. So five before, zero
after — now on hardware, not only on the host.

### 2. `SF33RD.AFS` is the same file everywhere

| copy | md5 | sha256 (first 16) | size |
| --- | --- | --- | --- |
| device `/media/fat/games/3s-arm/resources/` | `cc788f2ba398c7e4…` | `f9fa50f3a124ec9f…` | 642,492,416 |
| host runtime `~/Library/Application Support/CrowdedStreet/3S-ARM/resources/` | `cc788f2ba398c7e4…` | `f9fa50f3a124ec9f…` | 642,492,416 |
| `/Volumes/KimchDrive/Games/ps2/` | `cc788f2ba398c7e4…` | `f9fa50f3a124ec9f…` | 642,492,416 |

Full: `md5 cc788f2ba398c7e464736f4b6d00bc82`,
`sha256 f9fa50f3a124ec9fa9465aa9c8546c2d867887eb39f711a070762a0324ba5604`.
All three identical by content, not merely by size. The failures were never
about which file the device had.

### 3. The 379 ms link is REFUTED — and it is now measured, not argued

This supersedes the claim under #128 that "the 379 ms startup outliers are a
consequence, not a separate cause".

`9a597cd7` put a frame ordinal on the outlier line
(`src/port/sdl/sdl_app.c:3638`, threshold 50 ms), which is what made the
question answerable. A **diagnostic binary** was then built from `HEAD` with
`src/port/io/afs.c` alone reverted to `522e574c^`
(`md5 69a13d93dcacfbba58d7a413c1af5466`) — so the failures come back while the
frame ordinal stays. It was installed by copying over `bin/3s-arm`, with the
verified binary saved on-device first and restored on script exit; the restore
is confirmed below.

| build | failures | frame=2 | frame=9 | **frame=449** |
| --- | --- | --- | --- | --- |
| pre-fix run 1 | 5 | 56.8 ms | 54.7 ms | **416.7 ms** |
| pre-fix run 2 | 4 | 52.1 ms | — | **376.5 ms** |
| fixed run 1 | 0 | — | 52.0 ms | **415.0 ms** |
| fixed run 2 | 0 | 83.0 ms | — | **413.6 ms** |
| fixed run 3 | 0 | — | — | **413.0 ms** |

The big outlier is at **frame 449** in every run of both builds, and its size is
unchanged by the fix (376–417 ms with the failures, 413–415 ms without them).
Pre-fix run 2's **376.5 ms** is the "379 ms" of the original report.

The five failures all land in the first ten frames — interleaved with the
`frame=2` and `frame=9` outlier lines in log order:

```
94:ファイルの読み込みに失敗しました。ファイル番号：9
96:ファイルの読み込みに失敗しました。ファイル番号：1456
98:ファイルの読み込みに失敗しました。ファイル番号：1458
100:ファイルの読み込みに失敗しました。ファイル番号：10
102:FRAME OUTLIER: frame=2 total=56.8ms update=51.3 render=0.5 present=5.0
103:ファイルの読み込みに失敗しました。ファイル番号：1454
105:FRAME OUTLIER: frame=9 total=54.7ms update=48.6 render=0.4 present=5.7
111:FRAME OUTLIER: frame=449 total=416.7ms update=408.3 render=0.3 present=8.1
```

Frame 449 is ~440 frames after the last failure. A doubled read that finishes
by frame 10 cannot be what a frame-449 outlier is made of, and removing the
doubled reads entirely does not shrink it. **The outlier is a separate,
unexplained ~400 ms stall at a fixed frame — reproducible, and now
addressable.** The boot-time `frame=2`/`frame=9` outliers survive the fix too
(52.0 ms and 83.0 ms with zero failures), so they are not the doubled reads
either.

Not a regression introduced here: it is present in both builds. Opened as a
follow-up rather than chased in this lane.

### 4. #129's device leg — the trimmed search, on the real device

All three cases induced against the deployed binary. The two induced misses use
a **tmpfs overlay** over `/media/fat/games/mame`, so the real romset is never
moved, renamed or deleted; the script refuses to run a case if the overlay is
not empty, and unmounts on exit.

- **Found** — `ArcadeCharData: CPS3 ROM load satisfied by
  /media/fat/games/mame/sfiii3.zip`, then `Arcade balance auto-selected: CPS3
  ROM verified, 20/20 characters adapted (digest eab701778c8b20ad)`. The
  trimmed 5x2 search finds the romset on hardware.
- **Directory exists, holds neither basename** — `no CPS3 ROM -- 1 of the 5
  arcade ROM directories searched exist (/media/fat/games/mame), but none of
  them holds sfiii3nr1.zip or sfiii3.zip.`
- **Zip found, contents rejected** — four `Rom_Load: … no entry matched …`
  lines, then `… /media/fat/games/mame/sfiii3nr1.zip exists but FAILED CONTENT
  VERIFICATION -- wrong romset revision, not a missing ROM`, then `no CPS3 ROM
  -- 1 candidate zip(s) were FOUND and REJECTED by content verification …
  out of 10 probed location(s).`

`10 probed location(s)` is the trim itself, observed rather than asserted: 5
directories x 2 basenames. Afterwards: `no tmpfs on /media/fat/games/mame` and
the real 5.2 GB romset directory listing back in place.

The staged `device-129.sh` from the previous attempt was **not** used as
written: its first step is `misterctl.sh deploy --src build/mister-telemetry-package`,
which would have deployed the *main checkout's* package — a different, older
binary (`md5 d50ceb6dc0fb78681a9da6a305da8f5a`, 4,521,316 bytes, built 16:45 by
an unrelated lane) — over the build under test.

### 5. What actually held the lock for 7h48m

`ConnectTimeout`/`ConnectionAttempts` govern connection *setup*. Once a session
is established they are never consulted again, so a remote that dies mid-command
leaves the local `ssh` blocked on a socket that will never speak again.
`ServerAliveInterval`/`ServerAliveCountMax` are the mechanism that tells a
*quiet* session from a *dead* one: the probe rides the transport and a live sshd
answers it whether or not the remote command is producing output, so a long
silent harness run is never at risk while a dead peer is reaped in
INTERVAL x COUNTMAX. Set to **15 x 8 = 120 s** in `mister_ssh_password_args`,
`mister_ssh_key_only_args`, and both rsync `-e` command strings.

Wedge model: `ssh` is pointed at a local TCP relay which is then `SIGSTOP`ped —
it stops forwarding both ways with nothing torn down, no FIN and no RST, so the
peer is silent forever.

Establishment is confirmed by asking the **device** whether the remote command
is running, not by grepping local output. That detail is the whole reason the
first attempt at this proof reported a failure: `expect` echoes its own `spawn`
command line, which contains the marker string, so the grep matched at t=1s and
the relay was stopped **mid-authentication** — where `ServerAliveInterval` is
not yet armed, and where nothing reaps the session. Two runs "failed" at t+201s
and t+241s for that reason, and the fix was never at fault.

| arg vector | outcome |
| --- | --- |
| `mister_ssh_password_args()` with `ServerAlive*` stripped (byte-for-byte the committed pre-fix vector) | `t+201s -- local ssh STILL BLOCKED (budget 200s). HUNG.` at `0:00.00` CPU |
| `mister_ssh_password_args()` as shipped, 15 x 8 | `RESULT: local ssh EXITED 133s after the peer went silent` / `ssh said: Timeout, server 127.0.0.1 not responding.` |

`0:00.00` CPU on a blocked control is the same signature the 7h48m holder had.
A separate run of the shipped vector, with `ssh -vv`, shows the mechanism
firing: `Timeout, server 127.0.0.1 not responding.`, and at 3 x 3 the same
vector reaps in 12s — the reap time tracks INTERVAL x COUNTMAX as designed.

The production wiring is not inferred either. The real `misterctl.sh exec`
invocation used for the device runs above spawns:

```
spawn ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ConnectionAttempts=1
  -o ServerAliveInterval=15 -o ServerAliveCountMax=8 -o PubkeyAuthentication=no
  -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1 root@192.168.1.171 …
```

### 6. A max-age lock bound would be a regression, not an addition

`mister_lock_acquire` reclaims on **liveness** — `kill -0` on the recorded pid
fails, `rm -rf`, retry. That predicate is correct and it worked; the gap was
that the owner never died, which is what the keepalive fix addresses at source.

A max-age that *reclaims* cannot tell a wedged holder from a legitimately slow
one: `mister_transfer_timeout` is 1200 s by default, and a harness sweep runs
minutes beyond that. Any bound loose enough not to kill a healthy deploy is far
too loose to have helped here, and a bound tight enough to have helped would
put two writers on the device — worse than a stale lock. So age is **reported,
never enforced**: `mister_lock_status` now prints `owner_age_seconds` and
`owner_age_human`. What was missing on 2026-08-30 was not a policy but the
number — the owner file records a wall-clock date and left the subtraction to
whoever read it. Verified under `bash` (which is `misterctl.sh`'s shebang):

```
lock_state=held
owner_pid=46826
owner_live=1
owner_age_seconds=0
owner_age_human=0h00m00s
```

### 7. Residual — the authentication phase is still unbounded

`ConnectTimeout` covers TCP connect; `ServerAlive*` covers an established
session. A peer that accepts the connection and then goes silent **during
authentication** is covered by neither, and that is exactly the state the first
(flawed) proof runs sat in for 201 s and 241 s. Not fixed here, and not
observed in the wild — the 2026-08-30 wedge was on an established session
running a harness. Recorded so it is not rediscovered as a surprise.

### 8. Gates

- **Skipped, deliberately.** Frame-data, netplay harnesses and ARM were skipped
  for `mister-common.sh` and the `docs/queue.md` prose: shell tooling and text
  reach no compiled artifact.
- **Covered for `sdl_app.c`.** The frame-ordinal change was committed at
  `9a597cd7` and built there with `--flavor both` (shipped/clean *and*
  telemetry), `mode=arm-cross-build` — so the shipped-config build and the ARM
  cross-build both ran on exactly that source, and the resulting telemetry
  binary is the one that produced every device measurement above.
- `bash -n tools/mister/mister-common.sh` clean.
- Citation baselines: `BASELINE SUMMARY: scopes=11 breached=0 slack=0`.
- Canon untouched: 99 corpora / 1,488 rows / 1,435 PASS / 53 XFAIL. No corpus,
  simulation or netplay source was modified in this lane.

### 9. Device left clean

`SF33RD.AFS` untouched; the verified binary restored and re-hashed on-device
(`1b6dc20f37a720b563345709301b6702`); no tmpfs mounted; scratch files removed;
no processes left running. Nothing was deleted from the device at any point and
no `rsync --delete` was issued.

---

## #141 — the frame-449 boot stall, named — CLOSED (identified; no fix applied)

Device `192.168.1.171`, six bounded boots of an instrumented telemetry build
(three per instrumentation round). The stall is **`OPBG_Init()`** — the
one-shot build of the opening-demo background — and it contains **no disk I/O
at all**.

### 1. What frame 449 is doing

`update_ns` (`src/port/sdl/sdl_app.c:3413`) brackets exactly one call,
`game_step_0()` (`src/main.c`), so a `FRAME OUTLIER ... update=` line already
localised the stall to that function. Phase checkpoints inside it, and four
brackets inside `OPBG_Init()`, name it precisely. Round 2, three consecutive
runs, verbatim:

```
[opbg-init] total=407.7ms chunk1st=0.0 tex_loop=104.9 textures=90 texcash9=32.8 melt=270.1 tail=0.0 src_bytes=449032
[step0] total=412.7ms afs=0.0 input=0.1 nav=0.0 engine=412.6 seqs=0.0 netplay=0.0 probes=0.0 trace=0.0 effect=0.0 flip=0.0 | syncread=0.0ms n=0 bytes=0 | G_No=1/2/0/0 E_No=0/3/0/0 menu_cond=0 menu_r_no=0/0/0/0 Play_Mode=0 Mode_Type=0
FRAME OUTLIER: frame=449 total=435.1ms update=429.8 render=0.3 present=5.1

[opbg-init] total=409.2ms chunk1st=0.0 tex_loop=105.4 textures=90 texcash9=32.7 melt=271.0 tail=0.0 src_bytes=449032
[step0] total=414.7ms afs=0.0 input=0.1 nav=0.0 engine=414.6 ... | syncread=0.0ms n=0 bytes=0 | G_No=1/2/0/0 E_No=0/3/0/0 ...
FRAME OUTLIER: frame=449 total=422.1ms update=416.8 render=0.3 present=5.1

[opbg-init] total=409.0ms chunk1st=0.0 tex_loop=104.9 textures=90 texcash9=32.5 melt=271.6 tail=0.0 src_bytes=449032
[step0] total=415.8ms afs=0.0 input=0.1 nav=0.0 engine=415.7 ... | syncread=0.0ms n=0 bytes=0 | G_No=1/2/0/0 E_No=0/3/0/0 ...
FRAME OUTLIER: frame=449 total=422.9ms update=417.3 render=0.3 present=5.4
```

Every phase but `engine` is 0.0–0.1 ms, and `engine` is within 5 ms of
`OPBG_Init`'s own total. The breakdown of that ~408 ms, stable to ±1 ms across
six runs:

| phase | site | cost |
| --- | --- | --- |
| `ppgSetupTexChunk_1st` | `src/sf33rd/Source/Game/opening/opening.c:238` | 0.0 ms |
| 90× `ppgSetupTexChunk_2nd`/`_3rd` | `opening.c:243-246` | **105 ms** |
| `make_texcash_work(9)` | `opening.c:258` | 33 ms |
| `mlt_obj_melt2(&mts[9], 0x8C40)` | `opening.c:262` | **271 ms** |
| `sound_trg_init` + `opening_init` + `Zoom_Value_Set` | `opening.c:271-273` | 0.0 ms |

The source is AFS entry 76, `Opening.ppg`, **449,032 bytes** — `src_bytes` above
matches the archive exactly (parsed from `SF33RD.AFS` per the format at
`src/port/io/afs.c:144-221`). Note `textures=90`, not the 91 that
`ppgSetupTexChunk_1st` is passed.

**There is no I/O in this frame.** The per-frame blocking-read ledger reports
`syncread=0.0ms n=0 bytes=0` on frame 449 in all six runs. This is pure CPU:
PPG decompression, tile decompression, and swizzled copies.

### 2. It is NOT the renderer's redundant re-conversion — measured, and refuted

`Renderer_UnlockTexture()` is literally `Renderer_CreateTexture()`
(`src/platform/video/software/software_renderer.c`), which for PSMT8 rescans
the whole surface for `max_index` and for PSMCT16 mallocs and converts it. That
made "the melt re-converts every page it dirties" a natural suspect. A ledger
of every `Renderer_CreateTexture()` call, sampled per phase, says otherwise:

```
[opbg-tex] tex_loop calls=90 px=5898240 ms=8.1 | melt calls=76 px=4980736 ms=7.2
[opbg-tex] tex_loop calls=90 px=5898240 ms=8.2 | melt calls=76 px=4980736 ms=7.2
[opbg-tex] tex_loop calls=90 px=5898240 ms=7.9 | melt calls=76 px=4980736 ms=7.4
```

**15 ms of 408.** Texture conversion is 3.7% of the stall. The remaining ~97 ms
of `tex_loop` is `ppgDecompress` + `ppgChangeDataEndian`
(`src/sf33rd/Source/Common/PPGFile.c`), and the remaining ~264 ms of the melt is
`lz_ext_p6_fx` per tile, `njReLoadTexturePartNumG`'s swizzled copies, and
`search_trsptr` (`src/sf33rd/Source/Game/rendering/mtrans.c:131-157`), which
rescans the tail of the whole tilemap once per melted tile.

### 3. Why 449 specifically

`G_No=1/2` on the outlier frame is the answer, read out of the running game
rather than inferred: `Main_Jmp_Tbl` (`src/sf33rd/Source/Game/game.c:120`) is
`{ Wait_Auto_Load, Loop_Demo, Game }`, so `G_No[0]=1` is `Loop_Demo`;
and the `G_No[1]==2` arm of that dispatcher calls `Title()`
(`src/sf33rd/Source/Game/game.c:1699`). `Title()` case
1 (`src/sf33rd/Source/Game/demo/demo01.c:32-40`) counts `D_Timer` from 20 down
to 0 and then calls `opening_demo()`, whose `D_No[3]==0` arm
(`src/sf33rd/Source/Game/opening/opening.c:73-78`) calls `OPBG_Init()` once.

Everything upstream of that is a fixed frame count, which is why the number
never moves. The warning screen is dead code: `D_No[1] = 5` is overwritten by
`D_No[1] = 9` on the next statement (`src/sf33rd/Source/Game/demo/demo00.c:45-46`),
so the 120- and 180-frame warning holds never run. That leaves the Capcom-logo
sequence, which is all constant timers and fades — `D_Timer = 10`, a 62-frame
logo move, two 43-frame fades, `D_Timer = 256`
(`src/sf33rd/Source/Game/demo/demo00.c:114-197`). The only variable step in the
whole chain is a two-frame LDREQ wait. 449 is a **sequence position**, not a
time.

### 4. The smaller boot outliers are a DIFFERENT cause

Frame 2 reproduced in round 2 and carries its own ledger:

```
[afs-sync] sync fnum=7 name=SE.bd bytes=1345536 ms=4.6
[afs-sync] sync fnum=1456 name=ef06.bin bytes=595968 ms=2.7
[afs-sync] sync fnum=10 name=scrscrn.ppg bytes=51200 ms=20.0
[step0] total=66.4ms ... engine=66.2 ... | syncread=25.5ms n=7 bytes=1019904 | G_No=0/0/0/0 E_No=0/0/0/0 ...
FRAME OUTLIER: frame=2 total=75.4ms update=69.9 render=0.3 present=5.2
```

`G_No=0/0/0/0` is `Wait_Auto_Load`, i.e. still inside `Init_Task`. **25.5 ms of
that 66 ms is seven blocking archive reads** — the exact opposite of frame 449,
which has zero. That is also why the small outliers are intermittent (frame 2
was 75.4 / 50.2 / 51.1 ms in round 2 and below the 50 ms threshold in all three
runs of round 1, and #140's table shows the same flicker): their size is set by
how the SD card and the page cache behave on that boot. Frame 9's outlier is the
same class: the case-0 arm of `CAPCOM_Logo`
(`src/sf33rd/Source/Game/demo/demo00.c:114`) fires `checkAdxFileLoaded()`,
`preloadIntroBgm()` and `checkSelObjFileLoaded()`
(`src/sf33rd/Source/Game/demo/demo00.c:121-123`) — all blocking, back to
back.

**Verdict: 52.0 / 83.0 ms and 415 ms are unrelated.** The small ones are disk;
the big one has no disk in it.

### 5. A third, previously unreported outlier

`FRAME OUTLIER: frame=2812 total=57.8ms update=22.5 render=29.7 present=5.5`
appeared in **all six runs**, ±1 ms. It is render-dominated, so it is neither of
the two classes above. Not investigated here; recorded so it is not rediscovered
as a surprise.

### 6. Fixable or inherent

**Inherent as work, movable as placement — but only by a restructure.**

The 408 ms is real one-shot work that must finish before the opening demo can
draw a frame, and it does not repeat: `opening_demo()` case 1 tears it down with
`purge_texcash_work(9)` / `TexRelease_OP()` when the demo ends
(`opening.c:80-86`). It is not avoidable work in the sense of being wasted — the
one plausible waste, the renderer re-conversion, was measured at 15 ms.

It *can* be taken off a displayed frame. The frame immediately before it is idle
by construction: `Title()` case 1 spends 20 frames doing nothing but
`D_Timer -= 1` (`demo01.c:32-40`) — roughly 334 ms of budget sitting directly in
front of a 415 ms stall. The 90-texture loop (105 ms) is a plain `for` over
independent textures and would chunk into it trivially. `mlt_obj_melt2` (271 ms)
would not: it carries loop state (`cd16`/`cd32`) and mutates the tilemap in
place, so spreading it needs a resumable form plus a new `D_No[3]` state.

**Recommendation: do not fix it now.** It is one hitch, once per boot, during a
fade between the Capcom logo and the title, in offline attract — it cannot reach
gameplay or a netplay session, and it does not grow. The restructure buys a
cosmetic boot improvement in exchange for reordering the opening state machine,
which is the class of change that introduces init bugs. If it is ever taken up,
the order is: chunk the 90-texture loop into the existing 20-frame countdown
first (25% of the stall, no state machine change), and treat the melt separately.

Nothing was hoisted or reordered in this lane.

### 7. What was added

Diagnostic instrumentation only, all under `ENABLE_PERF_TELEMETRY`, all of it
counters and clock reads around calls that are otherwise untouched — no call is
added, removed, reordered or made conditional:

- `src/main.c` — ten phase checkpoints across `game_step_0()` and a `[step0]`
  line above the same 50 ms threshold `src/port/sdl/sdl_app.c:3638` uses.
- `src/port/io/afs.c` / `.h` — a per-frame ledger of blocking archive reads
  (`AFS_ReadSync`, `AFS_ReadRange`) plus an `[afs-sync]` line naming the archive
  member for any that blocks ≥ 2 ms.
- `src/sf33rd/Source/Game/opening/opening.c` — four brackets in `OPBG_Init()`.
- `src/platform/video/software/software_renderer.c` and
  `include/rendering/game_renderer.h` — a monotonic `Renderer_CreateTexture()`
  ledger, sampled per `OPBG_Init` phase.

Measured overhead of the renderer ledger: 15.3 ms across 166 calls in the frame
it measures, i.e. ~92 µs/call, of which two `SDL_GetTicksNS()` reads are ~0.1 µs
— 0.1%.

### 8. Correction to the brief

The brief said the 50 ms threshold sits at `sdl_app.c:3627` with a stale "25 ms"
comment. Neither holds at `ca9a6b3b`: the test is `sdl_app.c:3638` and its
comment at `:3633` already reads "exceed 50 ms of work". No comment was changed.

### 9. Gates

- **ARM cross-build: run.** `--flavor telemetry`, `mode=arm-cross-build`,
  `platform=linux/arm64`, lane `mister`. The binary that produced every
  measurement above is that build's `build/mister-telemetry-package/bin/3s-arm`,
  `md5 9fb3045599bf0e235fbea08d52878445`, confirmed byte-identical on-device
  after upload.
- **Shipped-config build: run.** `--flavor clean` selects `build_one clean OFF`
  (`tools/mister/build-game.sh:812`), which reaches the cmake flag at
  `tools/mister/build-game.sh:772`; the run logged
  `-- ENABLE_PERF_TELEMETRY=OFF`. That is the configuration in which every block
  above compiles out, including `game_step_0()`'s no-op macro forms.
- **Frame-data: skipped, correctly.** Nothing in this lane touches simulation —
  the changes are boot-path asset timing, a renderer counter, and log lines.
  Canon untouched: 99 corpora / 1,488 rows / 1,435 PASS / 53 XFAIL.

### 10. Device left clean

Binary saved on-device before each round and restored after both; final
`md5sum bin/3s-arm` = `1b6dc20f37a720b563345709301b6702`, 4,521,348 bytes —
identical to the pre-lane hash. `ls bin/ | grep task141` → no leftovers. Nothing
was deleted, no `rsync --delete` was issued, `menu.rbf` was never touched, and
no process was left running.

## #142 — the lock release any caller could switch off — CLOSED

`mister_lock_acquire` ended with `trap 'mister_lock_release' EXIT INT TERM HUP`
(`tools/mister/mister-common.sh`). Bash has exactly one EXIT trap slot, so a
caller writing the ordinary

```sh
mister_lock_acquire
trap 'my_cleanup' EXIT
```

replaced the release outright. A lane hit this today with its own device script.

### 1. Mechanism, reproduced rather than believed

A caller script sourcing `mister-common.sh`, acquiring, then installing its own
EXIT trap and exiting 0:

```
trap -- 'echo "CALLER-TRAP-RAN"' EXIT     <- trap -p EXIT inside the script
CALLER-TRAP-RAN
RESULT: LOCK LEAKED (dir still present)
pid=60879
```

`trap -p EXIT` prints only the caller's handler; `mister_lock_release` is gone
from the slot. `misterctl.sh lock-status` against that directory then answers

```
lock_state=held
owner_pid=60879
owner_live=0
```

Severity is bounded, and was checked rather than assumed: the owner *exits*, so
`mister_lock_acquire`'s liveness reap (`kill -0` fails → `rm -rf` → retry) does
free it for the next acquirer. This is not the 7h48m wedge of task #141's
neighbour — that was a live-but-hung owner. It is a latent footgun: stale state,
a `lock-status` that reports `held` for nobody, and a release path in shared
tooling that any caller can disable by writing correct-looking shell.

### 2. The fix, and the two that were rejected

Rejected — **chain whatever EXIT trap already exists at acquire time**. It fixes
nothing. The clobbering trap is installed *after* the acquire, which is the only
order that reads naturally, and is the order the lane actually wrote.

Rejected — **a watchdog process that outlives the shell**. Genuinely
un-clobberable, but it leaves a stray process for the life of every lock and it
races a later acquirer for the right to delete the directory: parent dies, a
second lane reaps and re-acquires, the orphaned watchdog then deletes the *new*
owner's lock. That trades a stale directory for two writers on the device.

Rejected — **a documented `mister_lock_trap_add` helper**. The brief's constraint
is that callers are ordinary shell scripts written by many different lanes and
the fix must not require them to remember anything. A helper only works when
everybody calls it, which is the same defect with more documentation.

Shipped — **interpose at the one place the caller must pass through: `trap`
itself.** `mister-common.sh` now defines a shell function named `trap` that
shadows the builtin for any script that sources the file. The invariant is *while
the lock is held, the EXIT trap ends with `mister_lock_release`*. Callers keep
writing ordinary shell and nobody has to remember anything.

It is deliberately narrow. Everything that is not an EXIT handler installed or
cleared while the lock is held — a bare `trap`, `trap -p`, `trap -l`, any
non-EXIT signal, and every call made before the acquire or after the release —
reaches `builtin trap` byte-for-byte unchanged. `trap - EXIT` and `trap '' EXIT`
get the bare release back rather than an empty slot. The release is chained
*after* the caller's handler, never before, so a handler that tears down device
state still runs while the lock is held.

Two companion changes, both needed by the first:

- `mister_lock_acquire` now calls `builtin trap` for its own install, so it does
  not chain the release onto itself.
- `mister_lock_release` grew a `BASH_SUBSHELL` guard. A subshell inherits
  `MISTER_LOCK_HELD` and shares `$$` with its parent, so once a caller's
  `( trap ... EXIT; ... )` gets the release chained onto it, the subshell's exit
  would delete a lock the parent still holds. `BASH_SUBSHELL` is the only
  workable spelling here: `$$` is identical in both, and `BASHPID` did not exist
  before bash 4.0 while macOS ships 3.2.57. Verified present and correct on
  3.2.57 (`top=0, sub=1, cmdsub=1, pipeseg=1`). `owner_pid` also became `local`;
  it was leaking into caller scope.

### 3. Proof

`tools/mister/tests/lock-trap-test.sh`, 28 assertions, no device and no network.
Every case asserts both halves — the lock is free afterwards **and** the caller's
own handler ran, because a chain that drops the caller's cleanup would be a worse
bug than the leak it fixes.

- T1 the reported shape; T2 the same under `set -e` with the script dying on a
  failed command (`status=1` reaches the handler); T3 `trap - EXIT`; T4 one
  handler over `EXIT INT TERM HUP`, asserting the non-EXIT signals are installed
  verbatim; T5 a non-EXIT trap leaves the release slot alone; T6 pre-acquire
  calls are not chained; T7 the subshell guard (`STILL-HELD-AFTER-SUBSHELL`);
  T8 post-release calls pass through; T10 the real in-tree caller shape —
  perf-sampler.sh's four traps plus a handler that disarms all four and exits
  (`perf-sampler.sh:787`, `:811`, `:904`), asserting no double release and no
  re-entry.
- **T9 is the negative control.** It rebuilds a copy of `mister-common.sh` with
  the wrapper stripped out, runs T1's script against it, and requires
  `lock_after=leaked`. Without it the other 26 assertions prove nothing.

`28 passed, 0 failed`. Existing suites re-run green against the change:
`deploy-prune-test.sh` 33/33, `osd-launcher-test.sh` 24/24.

Residual, recorded not hidden: a caller whose own EXIT handler ends in `exit`
terminates the shell before the chained release runs, because bash does not
continue an EXIT trap past `exit`. That leaks exactly as it does today, so the
wrapper is never worse. `perf-sampler.sh` is the one in-tree caller shaped that
way and it already calls `mister_lock_release` itself.

### 4. The other traps, audited

`mister-common.sh` had exactly one trap, the one fixed. It is now the only
`builtin trap` call in the file.

`mister-common.sh` is also the only sourced *library* in `tools/` that installs
a trap at all. The other one, `tools/mister/docker-host.sh` (sourced by
`build-game.sh` and `setup-build-container.sh`), contains zero `trap`
occurrences, so it has nothing a caller could clobber. Every other trap in
`tools/` is in a leaf script that installs it for itself and is nobody's
library: `package.sh`, `perf-sampler.sh`, `build-game.sh`, the netplay
`natmatrix` scripts, `bench-rect-batching.sh`, `publish-release.sh`,
`build-quartus-image.sh`, and the two test harnesses. The `trap cleanup EXIT`
at `perf-sampler.sh:946` is inside a heredoc for the *remote* shell, not a
local trap.

### 5. Keepalives, re-confirmed after editing the same file

Today's `182d473a` armed `ServerAliveInterval=15` / `ServerAliveCountMax=8`.
Confirmed still present on all four vectors by evaluating the functions, not by
reading them:

- `mister_ssh_password_args` → `... -o ServerAliveInterval=15 -o ServerAliveCountMax=8 ...`
- `mister_ssh_key_only_args` → same
- `mister_rsync_ssh_password_command` → same, inside the rsync `-e` string
- `mister_rsync_ssh_key_only_command` → same

And observed live on the wire: the `health` run below spawned
`ssh ... -o ServerAliveInterval=15 -o ServerAliveCountMax=8 ... root@192.168.1.171`.

The recorded residual is untouched and still open: the ssh **authentication
phase** is bounded by nothing, because keepalives are not armed until the
session is established. Not trivial, so out of scope here.

### 6. Device

End-to-end on `192.168.1.171`, read-only throughout.

`lock-status` → `lock_state=free`; `misterctl.sh health` → `__MISTER_HEALTH_OK__`,
`Linux MiSTer 5.15.1-MiSTer #1 SMP Wed Apr 2 20:01:54 CST 2025 armv7l`,
`__AFS_OK__`; `lock-status` → `lock_state=free`.

Then the lane's own shape against the real device: a script that acquires,
installs `trap my_cleanup EXIT`, confirms `lock_state=held` mid-run, runs a
read-only `mister_ssh_exec`, and exits. Output: `held: lock_state=held`,
`__READONLY_OK__`, `LANE-CLEANUP-RAN`, and `lock_state=free` afterwards — the
release survived and the caller's handler ran, on hardware.

No `rsync --delete`, `menu.rbf` untouched, nothing deleted, no process left
running.

### 7. Gates

- `bash -n` on every `tools/mister/*.sh` and `tools/mister/tests/*.sh`: clean.
- `tools/mister/tests/lock-trap-test.sh` 28/28 (new), `deploy-prune-test.sh`
  33/33, `osd-launcher-test.sh` 24/24.
- `shellcheck` **skipped — not installed on this machine** (`command -v
  shellcheck` empty). The two `# shellcheck disable=SC2086` comments in the
  wrapper are written for whoever runs it next; they are not claimed as verified.
- Citation baselines run at the end.
- **Frame-data, netplay harnesses, ARM cross-build and every build: skipped,
  correctly.** This lane touches two shell files and reaches no compiled
  artifact. Canon untouched: 99 corpora / 1,488 rows / 1,435 PASS / 53 XFAIL.

---

# Netplay review batch (#143–#152) — OPEN

Source: the 2026-08-31 five-lane netplay review. Every citation below was read
at `5b2c4ed2` on `upstream-engine-fixes`. Symbols are the durable anchor; line
numbers are hints qualified by that commit.

Prior findings re-verified as ALREADY FIXED, recorded so they are not re-opened:
host liveness (`host_rendezvous_thread_fn`, budget-less 5 s re-REGISTER),
any-datagram slot capture (`classify_host_datagram` fails closed), punch
retarget (`Stun_PunchOffer`), host CGNAT gate (`direct_p2p_ip_is_nonpublic`),
both GekkoNet remote-crash bugs (`OnInputs` bounds + serializer resize cap,
patched in `build-deps.sh` at `GEKKONET_REF=7be848c`), and H-6 `spmv_ng_save`
rollback-unsafety (saved, restored and hashed in `game_state.c`).

## #143 — the checksum hashes six globals `setup_vs_mode` never equalizes — CLOSED

`save_current_state` (`src/netplay/game_state.c`) latches `battle_start_frame`
on the first Gekko save, so the focused desync checksum covers character
select, not battle only.

Five hashed fields are absent from the PHASE 3 equalizer block
(`setup_vs_mode`, `src/netplay/netplay.c:1138` @ `5b2c4ed2`, comment "Zero
checksummed globals not covered above"). `grep -n 'ca_check_flag\|combo_type\|
remake_power\|Color7\|spmv_ng_save\|chainex_check' src/netplay/netplay.c`
returns nothing at `5b2c4ed2`:

| field | defined | zeroed only by |
|---|---|---|
| `ca_check_flag` | `engine/hitcheck.c` | round settle (`setup_settle_rno`, `plcnt.c`) |
| `combo_type[2]`, `remake_power[2]` | `engine/plcnt.c` | `combo_cont_init` (`cmb_win.c`), battle init |
| `Color7[2]` | `screen/sel_pl.c` | nothing |
| `spmv_ng_save[2]` | `effect/effl8.c` | nothing |
| `chainex_check[2][36]` | `system/sysdir.c` | `clear_chainex_check` (`plcnt.c`), `!ArcadeBalance_IsEnabled()`-gated |

Failure: host plays one offline match, backs out, hosts; joiner boots fresh.
Checksums differ from the first confirmed frame -> `GekkoDesyncDetected` ->
`handle_disconnection()` before the match starts. Two fresh loopback instances
carry identical staleness, so the existing harnesses structurally cannot see it.

Structural gap: PHASE 3 is a hand-maintained blocklist that must move in
lockstep with the hash whitelist, and nothing ties them together. The save set
has `--test-gs-coverage` for exactly this class; the hash-input set has no
analogue.

**CLOSED by `d6c4ae31` + `7fd54359` + `bcccded8`.**

Six fields, not five — the table above counts `combo_type`/`remake_power` as
one row. All six now reset at the end of PHASE 3.

The safety question that mattered was the inverse of the bug: does any reset
stomp a value the synced sim legitimately carries in, turning a false desync
into a real one? Answered by tracing every writer of all six through the
engine. All are battle- or select-scope and run after `setup_vs_mode` returns;
nothing `setup_vs_mode` itself calls beforehand (`Clear_Personal_Data`,
`grade_check_work_1st_init`, `Setup_Training_Difficulty`,
`System_all_clear_Level_B`, `Clear_Flash_Init`) references any of them, and
neither does the TRANSITIONING window. `Color7` has no other zeroing path at
all, so its reset is load-bearing rather than belt-and-braces.

`chainex_check` is deliberate defense-in-depth, not a live fix: all nine
`= 1` writers are `!ArcadeBalance_IsEnabled()`-gated, `is_enabled` is written
only by `ArcadeBalance_Init` (single call site, `main.c`), and
`Netplay_ArmAllowed()` is literally `return ArcadeBalance_IsEnabled()`. In any
shipped process that can arm netplay the array is provably already zero.

`h` is unchanged — the wire checksum's field set, order and function are
untouched, preserving the #148 (`bb2076de`) compatibility proof. Confirmed by
diff boundary and by `nm` on the shipped build: none of the new
`Netplay_Test_*` symbols are present.

The structural half, and its honest limit. `equalizer_scope_pin_check`
(`test_gs_coverage.c`) trips when `FH_COUNT` moves off 39 and NAMES the
offending field via `FH_NAMES`, with 3-step remediation. **It fails the
`--test-gs-coverage` harness run, not compilation** — `d6c4ae31`'s commit
message says "fails the build", which is wrong in kind. And the pin is
satisfiable by bumping `EQUALIZER_DIRTY_FH_COUNT_PINNED` alone, without
adding the PHASE 3 reset: a first-trip tripwire, not a proof of coverage. The
stronger form (dirty all 39 fields with in-range values rather than 6) was
considered and rejected on OOB risk. That is the residual gap; the in-file
comments state it accurately.

The asymmetric-history repro (host with prior-match residue vs fresh-boot
joiner) is the shape no prior harness could produce — all of them run two
identically-clean instances, which is why this survived. Verified
non-tautological: at `4678df6e` netplay.c contains zero occurrences of any of
the six.

Review follow-ups in `bcccded8`: the `ca_check_flag` comment claimed an
exhaustive writer list while omitting both `plcnt.c` sites, including
`setup_settle_rno` — the very site this entry's table names; the coverage
checks could have passed vacuously on all-zero hashes had the FH block ever
stopped running (now assertion-pinned, proven by gating it out); and
`FH_NAMES` had no arity guard, so a future enum entry without a name string
would have made the pin's own "name the field" step a NULL `%s`. Now a
`_Static_assert`.

NOT fixed, recorded: the six globals are hand-`extern`'d in three TUs and C
does not check extern-vs-definition agreement, so a resize in `sysdir.c`
would silently zero the wrong extent. `ca_check_flag` and `chainex_check`
have headers that would give the compiler the check. Existing repo
convention; consolidating is a far wider change than this task.

Citation fallout: `d6c4ae31`'s +79 lines moved `NETPLAY_SESSION_CONNECTING`
from `netplay.c:2428` to `:2497`, breaching the enforced set. Repointed in
`7fd54359` after confirming the two 16-line ranges are byte-identical. The
implementing commit reported this breach as "pre-existing and unrelated"; it
was neither.

Gates GREEN; `arm-cross-build` NOT RUN (accumulated-branch gate).
`check_baselines.py`: `scopes=11 breached=0 slack=0`. Rollback determinism
fast mode on the code commit: `verdict=PASS divergent=0 feedback=0`.

## #144 — mid-game disconnect and desync reach the player as silence — CLOSED

`process_session` (`src/netplay/netplay.c`) cases `GekkoPlayerDisconnected`
(`:1779`) and `GekkoDesyncDetected` (`:1847`) log, `push_event`, and call
`handle_disconnection` (`:1882`). Neither calls `DirectP2P_NotifySessionFailed`
— its only callers are the handshake reject (`:2410`) and the CONNECTING
timeout (`:2470`). `direct_p2p_on_teardown` parks `FAILED_HANDSHAKE` only when
`s_handshake_reject_latched` is set, else `set_state(DIRECT_P2P_IDLE)`.

`Netplay_PollEvent` (`netplay.c:2946`) has no production consumer: the only
other definition is `netplay_stub.c`, and every call site is a test.

Result: connect-phase failures surface honestly (S3), the RUNNING phase does
not. "Opponent left" and "desynced" are distinguishable only in the log file.

## #145 — `Netplay_HandleMenuExit` latches teardown from speculative frames — CLOSED (fixed)

`VS_Result` case 7 (`src/sf33rd/Source/Game/menu/menu.c:3339` @ `5b2c4ed2`)
calls `Netplay_HandleMenuExit()`, which sets `session_state =
NETPLAY_SESSION_EXITING` — a global outside `GameState`, so no `GekkoLoadEvent`
undoes it. `VS_Result` runs under `Menu_Task` inside `step_game()`, which
`advance_game()` executes for predicted and replayed frames alike. No call in
the chain consults `rolling_back` or confirmed-frame status.

Failure: on the result menu both players act inside the prediction window; a
rollback erases the local quit, EXITING stays latched. One side tears down
"user-canceled", the other rematches into the 5 s `DISCONNECT_TIMEOUT`.

**CLOSED by `1707e55c`.** Trigger falsification-tested first: **REACHABLE**,
but narrower than filed.

The only divergent transition in `VS_Result_Move_Sub` is `r_no[2]=6` (rematch
completion, requires the other player's `Menu_Cursor_X` set). Presses reaching
`7`/`99` land in the same exit arm and diverge nothing; `r_no[2]=5` is
unreachable in `MODE_NETWORK` (`After_VS_Move_Sub` skip=1 removes that cursor
row, `Game03` zeroes `Cursor_Y_Pos`). So the race is precisely: local arms a
rematch, disarms, quits — all inside the prediction window — while the peer's
confirm, made against the still-armed cursor, is in flight. On the corrected
timeline `VS_Result_Select_Sub`'s player-0-first short-circuit consumes the
frame and the local press is LOST, not deferred (input is edge-detected,
`~plsw_01 & plsw_00`).

Fix: RUNNING-state exits latch `s_menu_exit_request_frame` (stamped from
`s_sim_frame`, set at the top of `advance_game` so replay legs stamp the
replayed frame); `Netplay_Run` fires only once `head − R ≥ W`. A
`GekkoLoadEvent` below R cancels; a corrected timeline that still quits
re-latches. TRANSITIONING/CONNECTING exit immediately — not Gekko-simulated.

**The `≥ W` bound is exactly tight, verified in both directions against the
source `build-deps.sh` actually builds** (ref `7be848c`; the two facts the bound
rests on live in `input.cpp` and `game_session.cpp`'s `UpdateSession`, both
verified unchanged — **note `game_session.cpp` is no longer upstream-identical
as of #146 `eb0a428b`, which patches two assert sites in it; the prediction
and rollback-ordering code the bound depends on is untouched**). `HandleInputPrediction`
caps a predicted span at W consecutive frames; `UpdateSession` rolls back
before every advance. One frame looser and a fully-predicted `[R, R+W−1]` is
legal — the bug survives. One frame stricter and a quit after the peer goes
silent never fires, because prediction carries the head to exactly `R+W`.

Mutual clean quit cannot deadlock: case 7 is shared deterministic state, so
both peers execute it at the same simulated frame, both defer, both keep
sending inputs, both fire.

**ACCEPTED, not fixed:** peer goes silent within ~W frames of the quit → head
never reaches `R+W` → the exit arrives via the 5 s `DISCONNECT_TIMEOUT` and
reads "Opponent disconnected." instead of "user-canceled". Bounded, rare, and
arguably the truer cause. A wall-clock fire-anyway fallback is NOT the remedy —
it reintroduces the speculative teardown this task removes, because an
uncancelled request during a stall can still be rolled past when the stall
resolves. A `menu-exit-superseded` log line preserves the attribution.

Side effect: `session_state` no longer mutates mid-simulated-frame, so
`Ldreq_BarrierActive` (`gd3rd.c`) holds through the frame instead of dropping
the instant case 7 ran.

Pinned by `unit_menu_exit_deferral` (fails under `> W` and under `≥ W+1`,
across windows 1/8/32). NOT covered: the latch/cancel/fire wiring itself —
needs two live peers, the same seam gap #144 records for `process_session`.

**Reopen if:** (a) `GEKKONET_REF` moves off `7be848c` and the prediction cap or
the rollback-before-advance ordering changes — re-derive the bound; (b) any new
simulated path calls `Netplay_HandleMenuExit` or mutates `session_state` (today
`menu.c` `VS_Result` case 7 is the only sim caller); (c)
`VS_Result_Move_Sub` gains a non-exit transition reachable in `MODE_NETWORK`
beyond `r_no[2]=6`.

## #146 — GekkoNet `session_health` grows without bound from wire frames — CLOSED

`OnSessionHealth` (GekkoNet `backend.cpp`, ref `7be848c`) inserts
`session_health[frame]` with a wire-controlled `Frame` (i32) from
`SessionHealthMsg`. Its own cleanup erases only `frame < last_added_input - 128`;
`SessionIntegrityCheck` erases only keys matching a `local_health` entry, and
`local_health` is bounded near the confirmed frame. Attacker-chosen far-future
frames survive both.

Reachable in the shipped config: `configure_gekko` (`netplay.c`) sets
`desync_detection = true`, `limited_saving = 0`. Post-sync peer only (needs
`_session_magic`), so opponent-controlled rather than pre-auth. One map node per
packet, no receive-side rate limit, on a ~1 GB device.

Not covered by the R-2 patch set in `build-deps.sh`.

**CLOSED by `eb0a428b`.** Mechanism falsified in source at pinned `7be848c`
before patching: `SessionHealthMsg::frame` (i32) flows wire →
`OnSessionHealth` → `SetChecksum` → `session_health[frame]` unchecked; the
erase loop trims only below `last_added_input − 128` and
`SessionIntegrityCheck` erases only keys `local_health` reaches, so far-future
keys survive both; the shipped config reaches it.

**H-1:** insertion bounded to `±128` around `last_added_input`. A **window,
not a size cap** — `SessionIntegrityCheck` keys the comparison on exactly
these frames, so a cap could evict a live near-confirmed key and silently
blind detection, which #143 and #148 depend on. Lower edge = upstream's own
erase threshold (drops nothing legit); upper edge ≫ any real peer, whose
health frame always lags the inputs it has sent us (worst-case skew =
prediction window ≤32 + input delay ≈ 34). Residual attacker allocation
≈ 12–16 KB for one remote: OOM eliminated, not attenuated.

No new indexing assumption: **upstream already performs the identical
`_net_player_queue[player->handle]` read two lines below the insertion
point.** The early `return` ≡ `break` — nothing follows that loop.

**S-1:** reject `SpectatorInputs` when the session has locals (we set
`max_spectators = 0` and are never a spectator; a genuine spectator session
has no locals and is unaffected).

**A-1:** remove `assert(last_added - last_recv <= 128)`. Defense-in-depth
only — **GekkoNet is built Release in every configuration here**, so the
assert was already inert in every shipped artifact; the game's debug flavor
changes the game's flags, not this library's.

**A-2:** the one with a real shipped payoff, and NOT the debug-abort fix it
resembles. `assert(sav->frame == confirmed)` is compiled out in Release, so
upstream today *proceeds* and sends a checksum stamped with the wrong frame,
**fabricating a false desync on the peer**. Now skips, leaving
`_last_sent_healthcheck` unadvanced so the next confirmed frame retries.

Applied with R-2 discipline (pre-condition source form asserted exactly once,
then guard post-condition asserted), so a `GEKKONET_REF` bump that drifts
refuses to build. Cache check gains a `#146` sentinel in `backend.h`.
`GEKKONET_REF` unchanged.

**Verified live, not merely built:** recipe replayed against a pristine clone
with every anchor matching once; resulting headers byte-identical to the
cached ones; guards confirmed in the **disassembly** of the built
`libGekkoNet.a` (H-1's window compare ahead of `SetChecksum`, A-2's compare
ahead of the `_last_sent_healthcheck` store, S-1's early return).

Gates GREEN, harness 13; `arm-cross-build` NOT RUN. Baselines `scopes=11
breached=0 slack=0`.

**Follow-up worth its own entry:** migrate the in-line `perl -0pi`
substitutions to a numbered `.patch` series, so a future ref bump gets a
reviewable diff and `git apply --check` becomes the drift detector instead of
hand-written pre/post greps. Deliberately not folded in here — it would
rewrite the lines `eb0a428b` adds.

## #147 — the portmap quiescence gate covers removal and renewal, not spawn — CLOSED (confirmed, fixed)

`upnp_ensure_cached` (`src/netplay/upnp.c:48`) writes process statics
(`s_cached_urls`, `s_cached_data`, `s_cache_valid` `:37`, `FreeUPNPUrls` on the
error path). A probe that overruns `PORTMAP_PROBE_BUDGET_MS` is detached, not
joined (`try_portmap` timeout branch; `join_portmap_collect` /
`join_portmap_reset` detach branches, `src/netplay/direct_p2p.c`).

Removal and renewal are gated on quiescence (`upnp_renew_join_and_discard`,
the `join_portmap_quiescent` verdict). Spawning is not: `DirectP2P_Cancel` ->
`DirectP2P_BeginHost` resets `s_portmap_failed_port` and starts a fresh
`upnp_worker_fn` into the same statics. Two threads then populate and
`FreeUPNPUrls` one cache: data race, double-free class.

Window: the file's own measurement records an 11,543 ms probe against an
11,250 ms deadline, so detach is routine; a wedged TCP connect extends it to the
OS SYN timeout. PLAUSIBLE — the race is derived from the code, not observed.

**CLOSED by `400c7c3c`. Falsify verdict: CONFIRMED, not falsified.**

Re-verified first-hand rather than from the filing: `upnp.c` has no mutex, no
generation counter, no cancel latch; `upnp_worker_fn` reads only its heap job
plus config, so a detached worker cannot bail early; and **`natpmp.c`'s own
`KNOWN HOLE (review L-3)` comment describes this exact sequence**, ending
"neither was done here". #96's latch had closed the FAILED_STUN auto-retry
route that comment names, but `Begin*` resets that latch and discards
`join_portmap_reset`'s verdict — so `Cancel` → `BeginHost` stayed live.

Fix: the timed-out worker is parked **joinable** in a single straggler slot
instead of `SDL_DetachThread`'ed, and every probe spawn site refuses to start a
second worker until `portmap_backends_quiescent()` has joined it. Host ladder
and joiner rescue both skip with an attributed connect-log line.
**Not** latched into `s_portmap_failed_port` — "we did not ask" is not a
measured verdict and would corrupt #96's semantics. `upnp_renew_tick` consults
no gate deliberately: a renewal requires a live mapping and a running straggler
excludes one; that argument was verified through all four of its cases.

Waiting instead of skipping was rejected — it would block the hosting worker
for the SYN timeout at the moment the user pressed Host. Skipping degrades to
the supported no-mapping outcome and self-heals next press.

No leak, no stall: `SDL_WaitThread` is only ever called after the thread
reports COMPLETE; the reap also reclaims the job block the old detach path
leaked *by design*. Worst case at process exit is one zombie thread handle.

Slot park/refuse/reap pinned by test 45 through the production entry points.
**Real miniupnpc concurrency has no mock and the spawn-site gates are
inspection-covered only** — stated, not papered over.

Residual for separate queueing: the module now has five hand-maintained
worker handle/job pairs. A single owned worker-slot abstraction, or a mutex
around the two backends' process globals (which would make the spawn gate
unnecessary), is the systematic version.

## #148 — three `game_state.c` items: one diagnostic, two latent — CLOSED

1. `sc.globals = h ^ sc.plw0 ^ sc.plw1` (`save_current_state`,
   `src/netplay/game_state.c:2362`). djb2 does not compose by XOR, so the dump's
   "globals" column is a mixture, not a section hash: a pure-PLW divergence
   moves both columns in a cross-peer diff. Diagnostic only; misdirects triage.
2. The sparse overflow guard tests accounting (`EFFECT_MAX - frwctr`,
   `:2471`) while `pack_sparse_state` (`:1932`) emits every `be_flag != 0` slot.
   Safe today because `pull_effect_work` decrements `frwctr` before any
   `be_flag = 1` write and `push_effect_work` zeroes before freeing
   (`effect.c`), but nothing asserts it. A wrong-direction break overruns the
   Gekko state slot by 20 bytes.
3. `load_state_from_event` (`:2526`) runs `GameState_Load` before
   `unpack_sparse_state` (`:1988`); on rejection (`:2540`) GameState is at frame
   F and the effect pool at the previous frame. Silent inconsistency rather than
   session abort. Unreachable unless Gekko corrupts its own ring.

**CLOSED by `bb2076de`.** As-built, with the two facts a later reader needs:

`h` is unchanged. Verified by mechanical enumeration, not by inspecting the
macro: 39 field sites extracted from the pre-change `save_current_state` and
diffed against the post-change `HASH_GLOBAL` sequence — identical fields,
order, sizes and seed. Every original call already passed `sizeof(field)`
matching its own address operand, so the macro could not introduce an
array/scalar discrepancy. This is the property that matters: a changed `h`
makes every peer on an older build see a false desync.

`sc.globals` DOES change value. A dump diff between this build and an older
one shows the globals column differing by construction on every frame;
`combined`/`plw0`/`plw1` stay comparable and the wire checksum is `h`. No
tooling parses the column (`tools/compare_states.py` is a DWARF struct
differ; the only other reference writes a placeholder and never reads it).

Item 2 as built: `pack_sparse_state` counts `be_flag != 0` itself, asserts
`beflag_count <= EFFECT_MAX - frwctr` as a tripwire, and returns `0` for the
full-state fallback when its own count exceeds the ceiling. The invariant it
no longer trusts was re-verified in `effect.c`: `pull_effect_work` decrements
`frwctr` as part of the index expression, `push_effect_work` zeroes the slot
before returning it to the free list, and many `eff*.c` sites set
`be_flag = 1` several lines after the pull — so strict inequality is the
normal case, not a theoretical one.

Item 3 as built: `sparse_state_is_valid` is a pure predicate run BEFORE
`GameState_Load`. `GekkoGameEvent` carries no status channel, so neither
shape can signal the library; restoring nothing re-simulates from a
self-consistent state, where the partial restore left a frame-F/frame-F−1
mixture and named the wrong frame in the log.

`SPARSE_CEILING_SLOTS` unchanged at 100 (`game_state.h:865`).

Review found zero P-1 and four P-2. Three fixed: the popcount test never
reached the branch it was named for (the length check rejected first, so that
branch had zero coverage while the diff cited it as proof); the two-predicate
story was written out in four places against the assertion-over-prose rule;
`sparse_state_is_valid`'s `active_count` bound tightened from `EFFECT_MAX` to
`SPARSE_CEILING_SLOTS`. The fourth was the mixed-build triage note, recorded
above and in the commit rather than in code.

Inherited, NOT fixed here: `test_unpack_rejects_bad_count` has the same shape
the popcount test did (`bogus = 99`, rejected by the length check before
reaching the branch). Pre-existing, out of scope for #148.

Gates GREEN; `arm-cross-build` NOT RUN (accumulated-branch gate).
Rollback determinism fast mode: `verdict=PASS divergent=0 feedback=0`.

## #149 — accept-side gaps: terminal handshake frames and empty datagrams — CLOSED

1. `mist_handshake_pump` (`src/netplay/mist_handshake.c:717`) returns
   `MIST_PUMP_OK` for `cls == 1` and `MIST_PUMP_FAIL` for `cls == -1` without
   consulting `from_session_peer`; that source check gates only the H-1 latch
   and implicit completion. Anyone who knows the punched 4-tuple can kill a
   pairing with one spoofed REJECT, or satisfy the gate with a forged ACK (field
   values are public to any same-build peer).
2. `receive_data` (`src/netplay/sdl_net_adapter.c:327`) guards the counter
   block on `buflen > 0` but not the result-building block: a zero-length
   datagram yields `SDL_malloc(0)` with `data_len = 0` handed to GekkoNet, and
   every non-MIST/punch/rendezvous datagram from any host is forwarded before
   any source filter.

**CLOSED by `a609842e`.**

(1) was **THREE** ungated terminal returns, not two — `cls == 1` → OK,
`cls == -1` → FAIL, and the H-2a path where an incompatible non-MALFORMED
hello also returned FAIL (a spoofed mismatched build hash could kill a
pairing). All three now require `from_peer`. Third-party hellos still draw
their reject/ack reply; only the verdicts are gated. The gate is an
address+port match, **not authentication** — identity is the token-authenticated
punch upstream, and the comment says so. Tests r13–r15 assert the absence of
the wrong outcome, not merely a timeout.

Nothing legitimate regresses: `from_session_peer` is set in `mist_pump_start`
before any slice runs, and a mid-handshake rebind is repaired by late_punch's
refetch inside the 40-attempt budget. A permanently mismatched tuple now times
out where it once fast-failed — but that configuration could never complete
GekkoNet sync anyway, so only the failure shape of a dead corner changed.

(2) zero-length datagrams dropped outright; source filter accepts canonical ∪
actual peer, falls open while canonical is unset, sits after the
STUN/rendezvous/punch guards (so LatePunch still sees a rebinding peer's punch)
and before the #119 translation.

**The safety argument was closed at source, not assumed.** Upstream GekkoNet at
pinned `7be848c` matches inbound by byte-comparing the presented address
string, and every state-changing handler gates on it including sync completion
— so no endpoint this filter rejects could have produced a working session. A
hostname canonical (possible via the LAN CLI) was already inbound-dead at the
engine, so the filter is **provably never stricter than GekkoNet**.

It is also strictly better than "drops what GekkoNet ignored":
`OnSyncResponse`'s Connected arm increments and replies via `SendSyncResponse`
to whatever address the packet arrived from, **with no address check** — a
foreign datagram carrying the guessable session magic drew a reply. The filter
stops it reaching `ParsePacket`.

Both drop paths are counted and surfaced in the packet-ring dump, **split by
reason**. Without that, a filter wrong in the field would present as zero rx —
indistinguishable from the commonest NAT failure, on a project whose netplay
forensics read exactly this ring.

## #150 — six connection-path minors — CLOSED (all six)

1. `Stun_Discover` (`src/netplay/stun.c:790`) falls back to
   `result->local_port = result->public_port` when `getsockname` (`:794`)
   fails. On a non-port-preserving NAT that is wrong, and
   `join_portmap_spawn(s_work.stun.local_port)` then maps an internal port the
   socket is not bound to — silently disabling the #121 symmetric-joiner rescue
   on exactly the NATs it exists for.
2. `set_status`'s comment justifies a non-atomic write with
   "DirectP2P_GetStatusText copies out of the buffer eagerly";
   `DirectP2P_GetStatusText` returns the raw `s_status` pointer. Tearing is
   bounded (memset-first keeps it NUL-terminated) — the defect is the claim.
3. `DirectP2P_Cancel` is documented "Safe to call from any thread"
   (`src/netplay/direct_p2p.h`) but runs `join_portmap_reset` / `portmap_remove`,
   whose own comment requires the main thread.
4. `host_tick_receive` processes one datagram per frame in `HOST_WAITING`;
   the punch gate mutes punch-shaped traffic only, so arbitrary garbage
   consumes the frame's single receive. The race loop already drains greedily.
5. `Upnp_RemoveMapping` hardcodes `"UDP"` in `UPNP_DeletePortMapping` while
   `Upnp_AddMapping` takes `protocol` as a parameter. Latent; all call sites UDP.
6. `Stun_Discover` compares `NET_GetAddressStatus(bind_addr) != 1` against a
   magic `1` rather than `NET_SUCCESS`.

**CLOSED by `400c7c3c`. All six.**

(1) `local_port = 0` now means unknown, warned at the source; the #121 rescue
skips with an attributed line rather than asking the gateway to map a port the
socket is not bound to. Sole production consumer (`join_portmap_spawn`)
verified; the sibling `Stun_DecodeEndpoint` fallback (no production callers)
reports 0 too, pinned by a codec case. (2) `set_status`'s comment no longer
claims a copy-out the API never provided; the real contract is stated at both
ends. (3) `DirectP2P_Cancel` documented main-thread-only, matching its
callees. (4) `HOST_WAITING` drains up to `HOST_TICK_RECV_BATCH` (16) per
frame, stopping on state exit so session datagrams are never consumed; the
>960/s residual is documented. (5) `Upnp_RemoveMapping` deletes with the
protocol the mapping was created with (`UpnpMapping.protocol`), exact fallback
for pre-existing structs. (6) `!= NET_SUCCESS`.

Tests 46 and 47 close the **previously uncovered** mute → re-roll →
legitimate-joiner-recovers path end to end over the real socket.

**Worth knowing beyond this task — the citation checker cannot catch a wrong
repoint.** Its ownership pass accepts *any* anchor token from the citation's
surrounding context appearing on the cited line, so when two symbols share a
sentence, a repoint to the wrong one passes. `--fix` produced seven wrong
anchors here (`signal_cookie_publish` → `signal_cookie_snapshot`, the two
natpmp anchors crossed with each other, `try_handle_deliver` → a thread spawn,
`seed_port` → a socket guard) and `check_baselines.py` stayed green
throughout. All were re-derived by reading their targets. **Do not trust
`--fix` inside the enforced set.**

## #151 — a third-party POLL refreshes a session's TTL — CLOSED

`handlePoll` (`tools/rendezvous-server/rendezvous-server.js:1473`) sets
`entry.lastTouch = nowMs()` (`:1482`) inside `if (entry)` and *before* the
`endpointEq` slot-match branches, so the `else` arm ("source isn't a registered
endpoint") refreshes the entry too. `handleRegister`'s SESSION_FULL branch
returns before its own `lastTouch` write, so the two verbs disagree.

Any cookied holder of a session key, seated in neither slot, keeps the entry
alive past `SESSION_TTL_MS` indefinitely by polling — pinning one of the
creator IP's key slots and one of `MAX_SESSIONS`. Production clients never send
POLL (`Rendezvous_BuildPoll` has no non-test call site), so exposure is small.

**CLOSED by `a609842e`.** The refresh now lives inside the two `endpointEq`
seated arms; the "source isn't a registered endpoint" arm refreshes nothing,
agreeing with `handleRegister`'s SESSION_FULL early return. #130 preserved — a
seated poller still drives `touchSlot` with the same stamp. Wire format
untouched: an unseated poller still receives its zeroed DELIVER.

`pollTtlRefreshSeatedOnly` (EXPECTED_TESTS 39 → 40) pins both directions and
was **confirmed to fail against the pre-change server**, including that a
past-TTL entry polled by a cookied third party is evicted by the production
sweep (`sweepSessions`), not a test-only helper.

In-tree only. The pending prod redeploy (v1 server vs v2 client) is unchanged
and remains the real field blocker.

## #152 — dead weight, three items — CLOSED (all three deleted)

1. `matchmaking.c` (legacy TCP lobby client) is still wired into
   `netplay.c` (`Matchmaking_Start` `:2224`, `Matchmaking_Run` `:2233`) despite
   direct-P2P being lobby-unaware by locked decision. `Matchmaking_Run`'s
   `SDL_sscanf` result is unchecked: a malformed server line yields `player = 0`
   -> `player_number = -1` -> `configure_gekko` adds two remote actors and no
   local actor. Bounded by the 15 s CONNECTING deadline; server is the user's own.
2. The `NetplayEvent` queue has no consumer (#144), and its comments promise UI
   reactions that do not exist.
3. `tools/netplay/fake-peer.py` and `tools/test_matchmaking_server.py` describe
   the pre-3SXR protocol (7-char IDs, TCP/UDP 9000/9001) and no longer match any
   shipped path.

Keep-or-delete was a scope decision, not a defect fix. **Decided 2026-08-31
by the maintainer: delete all three.**

- Item 1: remove `matchmaking.c`, `matchmaking_stub.c`, the `netplay.c` call
  sites and the CLI/menu entry points. Deleting the path removes the unchecked
  `SDL_sscanf` rather than repairing code the shipped flow never runs, and
  retires a second unauthenticated connection path.
- Item 2: remove the `NetplayEvent` queue. #144 routes real failure surfacing
  through the direct-P2P notify latch instead, which leaves this queue
  permanently orphaned. **Sequencing: lands only after #144 merges** — both
  touch the same `netplay.c` failure paths.
- Item 3: remove `tools/netplay/fake-peer.py` and
  `tools/test_matchmaking_server.py`.

**CLOSED by `4fe47024`.**

**Correct the premise first: matchmaking was NOT dead code.** This entry, and
the review that produced it, both said "wired into netplay.c" and "the shipped
flow never uses it". Understated. Uncatalogued consumers found during the
deletion: `menu.c` called `Netplay_BeginMatchmaking()` **unconditionally** on
two entry points alongside `Netplay_BeginDirectP2P()`;
`src/port/sdl/netplay_screen.c` carried a live per-frame render branch drawing
"Connecting to server… / Finding match… / Matchmaking error";
`Soft_Reset_Sub()` called `Netplay_CancelMatchmaking()` ungated; `main.c`
ticked it every frame and branched on it in `set_netplay_params`.

It was **live-wired but provably inert**, and that is what the deletion rests
on. `Netplay_BeginMatchmaking()` returned at its
`matchmaking_server_ip == NULL` guard; the field had exactly one writer, the
`--matchmaking-ip` flag; nothing in the tree passes it. So `Matchmaking_Start`
never ran, state stayed at the `MATCHMAKING_IDLE` static initializer, and every
consumer was dead on arrival — the render branch gates on a non-IDLE state, all
three cancels hit the idle guard, `Matchmaking_GetSocket()` returned NULL
(socket created only in a never-entered state), and
`check_netplay_cancelled()` always returned false, making its bare-cursor
replacement behaviour-identical.

**Zero on-device behaviour change.** Only external surface removed is
developer-facing: `--matchmaking-ip`, `--matchmaking-port`, and two
arg-combination error messages.

Item 2 removed cleanly: 7 `push_event()` sites deleted as pure statement
removals, with #144's notify calls and #145's deferred-exit machinery
re-verified intact around them. Harness discovery 14 → 13 (floor is 8);
`args.c`/`main.c`/`configuration.h`/`CMakeLists.txt` consistent.

Four comments naming the deleted `test_event_queue.c` were accurate before
this task and wrong after it, so they were repaired here — rewritten to
describe the gating pattern rather than name a sibling that could be deleted
next. `game_state.c`'s edit is comment-only; `h` untouched.

**Discovered, NOT fixed — `ENABLE_NETPLAY=OFF` has never compiled.** `args.c`
and unrelated files (e.g. `eff61.c`) use bare `#if ENABLE_NETPLAY` /
`#if NETPLAY_ENABLED`; both macros are defined only when the option is ON, so
under `-Wundef -Werror` the preprocessor kills the build before any link.
Pre-existing, confirmed against HEAD, and no gate builds that configuration.
Consequence: `netplay_stub.c` is theatrical — nothing compiles it — so its
trims cannot break anything. **Worth its own entry if the OFF build is ever
meant to work.**

Gates GREEN, harness count 13; `arm-cross-build` NOT RUN.
`check_baselines.py`: `scopes=11 breached=0 slack=0`.

---

## #153 — `ENABLE_NETPLAY=OFF` has never compiled — OPEN

Surfaced by #152 (`4fe47024`) while proving the deletion safe in every
configuration; **pre-existing, not caused by it**.

`src/args.c` uses bare `#if ENABLE_NETPLAY`; `src/sf33rd/Source/Game/effect/eff61.c`
uses bare `#if NETPLAY_ENABLED`. Both macros are defined only by
`target_compile_definitions(...)` inside `if(ENABLE_NETPLAY)` in `CMakeLists.txt`,
so with the option OFF they are undefined. The project builds with `-Wundef`
and `-Werror`, so the preprocessor fails the build before any link step.
Confirmed against `HEAD` — the same bare directives predate the #152 diff.

Consequences worth knowing:

- **No gate builds this configuration**, so nothing would catch a regression
  in it. `tools/gates/run-gates.sh` builds `ENABLE_NETPLAY=ON` twice
  (hooks OFF and ON) and cross-compiles the shipped config for ARM.
- `src/netplay/netplay_stub.c` exists to satisfy that configuration's link.
  **No buildable configuration compiles it**, so its contents are unverified
  by anything — it is maintained on faith.

Decide deliberately, do not drift: either the OFF build is meant to work — in
which case it needs the bare `#if`s converted and a gate that builds it — or it
is not, in which case `netplay_stub.c` and the `ENABLE_NETPLAY` option itself
are dead weight and should follow #152's matchmaking client out of the tree.

Not fixed under #152: that task's mandate was three approved deletions, and
this is a scope decision the maintainer has not made.

## #154 — migrate the GekkoNet patch set to a reviewable `.patch` series — OPEN

Surfaced by #146 (`eb0a428b`). Deliberately not folded in, because it would
rewrite the exact lines that commit adds.

`build-deps.sh` now carries two hardening sets (R-2 and #146) applied as
in-line `perl -0pi` substitutions, each wrapped in hand-written pre-condition
and post-condition `grep`s so a `GEKKONET_REF` bump that drifts the source
refuses to build. The fail-loud discipline works — #146's replay against a
pristine clone proved every anchor matches exactly once — but the durable
record of what we changed in a security-sensitive dependency is a shell script,
not a diff.

A numbered `.patch` series against `7be848c` would make the hardening set
reviewable as ordinary diffs and let `git apply --check` be the drift detector
instead of bespoke greps. That matters most at the moment it is hardest: a
future ref bump, when someone must decide whether each guard still applies.

Note the existing cache sentinels are proxies. The R-2 sentinel lives in
`compression.h` and the #146 sentinel in `backend.h`, while several guards land
in `backend.cpp`, `game_session.cpp` and `zpp/serializer.h` — files not copied
into the cached include tree. Defensible only because every patch applies in
the same atomic rebuild block; a `.patch` series would make that structural
rather than argued.

## #155 — room code v4: drop the nonce, keep version + check (12 chars) — CLOSED

**Maintainer decision, 2026-08-31.** The v3 code is 18 chars (20 displayed with
its two hyphen groups) and that is hurting the thing the code exists for:
being read aloud to a friend and typed on a controller with an on-screen
keyboard.

Where the length actually went (v1 was 11 = 10 payload + 1 check):

- **+6 payload chars = the 32-bit nonce.** `ROOM_CODE_NONCE_BITS 32`;
  48 fixed bits (ip 32 + port 16) + 32 = 80 = 16 × 5.
- **+1 = the version prefix.**
- **The check digit cost NOTHING.** v1 already had one. v3 fixed its
  arithmetic (the ISO 7064 recurrence was summed mod 37 instead of mod 36,
  collapsing 37 product states onto 36 outputs). Do not "save" a character
  here: without it the payload is dense, every well-formed string decodes to
  *some* real ip:port, and a typo silently becomes a code for somewhere else —
  measured 1 in 781 undetected substitutions on v1/v2, versus 0 of 2,520,000
  on v3.

### v4 shape

`1 version + 10 payload + 1 check = 12 chars`, no hyphen groups needed.
Length map stays collision-free: 11 = v1, **12 = v4**, 14 = v2, 18 = v3, all
legacy lengths still recognized so old codes report OLD_FORMAT rather than
failing as garbage.

Version char and length dispatch are **kept deliberately, having been
questioned**: they are not what stops an incompatible *build* from pairing
(the MIST handshake's `state_ver`/`proto_ver`/arch/digest gate does that, and
a format change implies a build change, so the handshake would have rejected
the session anyway). What the local check buys is that a misparse produces a
**wrong address, not a wrong peer** — there is no handshake to defer to,
because nobody is there. Length dispatch already makes different-length
formats fail clean as MALFORMED; the version char covers the same-length case.
Cheap, so it stays.

### The consequence to accept, in writing

The rendezvous session key and the S4a punch token are **derived from the code
payload**. Removing the nonce makes both a pure function of (ip, port), which
is public. That reopens exactly v1's exposure: someone who knows or scans your
IP can derive the session key and **squat or race the pairing**.

Accepted, because the ceiling is "a stranger joins your match" or "your friend
has to retry", this is friend-to-friend P2P, and the host-side punch-gate mute
(S4-review HIGH-1b) now backstops it — a defence v1 never had. Not accepted
silently: if the punch gate is ever weakened, this decision reopens.

### Keep the nonce machinery ALIVE

`RoomCode_GenerateNonce`, the `Csprng_Bytes` hard-fail, and the
domain-separated `3SXR-SK3` / `3SXR-PT3` derivations stay **compiled and
unit-tested** — merely unused by the code string. **Do not park them behind an
`#if`.** `LOSSY_ADAPTER` was exactly that shape and silently stopped compiling
when the warning set tightened (see `4ca4c0de`); the rot cost us the only
local way to exercise rollback. Dormant-but-built is the requirement.

Why they will be wanted: a **room list** changes the delivery channel. Today
the nonce must ride in the code, because the joiner derives the punch token
before any contact and the session key is what *indexes* the rendezvous slot,
so the server cannot deliver it. A list breaks that circularity — pick a room,
receive the nonce out-of-band — restoring unguessable derivations at **zero
character cost**.

### Notes for the implementer

- Breaking change: a v4 client cannot pair with a v3 client (the punch-token
  derivation changes, not just the string). Assess whether `MIST_PROTO_VER`
  should bump.
- `ROOM_CODE_REDACT_CHARS` (8) was sized for an 18-char code; re-derive for 12.
- Keep the corrected ISO 7064 recurrence and its full-alphabet sweep; extend
  the sweep to the new length rather than trusting the old result.

**CLOSED.** v4 = version char `'4'` + 10 payload + 1 check = **12 chars, no
hyphen groups** (`ROOM_CODE_DISPLAY_LEN == ROOM_CODE_CHAR_LEN`). Length
dispatch collision-free across all four: 11 = v1, **12 = v4**, 14 = v2,
18 = v3 — v3 newly demoted to legacy, recognized with the *corrected*
mod-36 recurrence (its arithmetic was already right; only its length is
legacy now). Domain strings bumped `3SXR-SK3/PT3` → `SK4/PT4`, so a v3
payload can never land in a v4 derivation.

Sweep **re-measured on the new geometry**, not carried forward:
**0 undetected of 1,680,000** substitutions across 4,000 codes.

`ROOM_CODE_REDACT_CHARS` 8 → **3**: a v4 code carries no secret, so masking
is hygiene (don't hand out a paste-ready code), not confidentiality. The 3
masked chars include the check digit, so a redacted string can never decode.

`MIST_PROTO_VER` **not bumped** — a v4 client cannot reach a MIST handshake
with a v3 peer at all (different rendezvous slot from the SK4 bump, mismatched
punch token), so the incompatibility is caught strictly earlier.

### The re-roll: deleted, then restored dormant — read this before re-wiring

Removing the nonce made the S4-review HIGH-1b **re-roll escalation inert**:
`host_reroll_room_code` re-encoded from the *same* `(ip, port)` and never
re-bound anything, so with a fixed nonce it reproduces a byte-identical code.
The implement pass deleted it. Review then found that re-wiring it as-is would
be **worse than inert — an active mute bypass**: the escalation called
`host_punch_gate_clear_mutes()` every 64 charged bad punches, so an attacker
grinding tokens would clear their own mute every 64 datagrams against a code
that never changed.

Resolution (maintainer's call): **the code is restored and compiled; the
wiring stays out.** `host_reroll_room_code` and `host_punch_gate_clear_mutes`
are non-`static` with unconditional declarations in `direct_p2p.h` — kept
alive by external linkage, the same way `RoomCode_GenerateNonce` is. **Not**
behind a `#if`, and deliberately **not** behind a `DirectP2P_TestHook_*`
wrapper: every such wrapper lives inside `#ifdef NETPLAY_TEST_HOOKS`, which
the shipped-config gate builds with **OFF**, so a test-hook-only reference
would vanish in exactly the build that matters — recreating the
`LOSSY_ADAPTER` rot by omission instead of by `#if`. Verified: the symbol is
present in the shipped `host-release` binary.

Re-wire only when a room list restores an out-of-band nonce, and re-wire
**both together** — re-roll is meaningless without fresh entropy, and
mute-clearing is a gift to an attacker without a genuinely new code.

### The mute's coverage shrank — correcting this entry's own earlier wording

This entry originally called the mute "a defence v1 never had", which
overstates what it now does. Under v3 an attacker had to **grind** for a valid
token and the mute cut that off. Under v4 an attacker who knows the host's
endpoint **computes the correct token directly** and is accepted on the first
punch — the throttle does nothing for them. What the mute still covers is
punch-shaped **wrong-token** traffic (non-punch-shaped garbage is deliberately
not charged, per `host_ignore_is_chargeable`). That is consistent with the
accepted risk recorded above, but it is a smaller backstop than first written.

Nonce machinery (`RoomCode_GenerateNonce`, the `Csprng_Bytes` hard-fail, the
SK4/PT4 derivations) stays compiled and unit-tested by
`nonce_machinery_behavior`, unused by the code string, awaiting the room list.

Wrapper: `main-mister-full-menu.patch`'s hand-edited hunk header verified
arithmetically (6 context + 139 added = 145) **and test-applied with
`git apply`** against pinned upstream `menu.cpp` — clean. Consequence: the OSK
now caps entry at 12 chars, so an 18-char v3 code can no longer be *typed* to
reach the "older version" message; only recent-joins and handoff files reach
it. Harmless, since v3/v4 cannot pair regardless.

Review: **zero P-1**, four P-2 (stale re-roll promise in a live log line;
self-contradictory pad-tolerance prose; `RoomCode_Redact` legacy-truncation
comment; a note about retired domain strings in an ungated probe tool) — first
three fixed, fourth left as flagged. 34 citation repoints spot-checked correct.
Gates GREEN, harness 13; baselines `scopes=11 breached=0 slack=0`.

**NOT verified:** the wrapper OSD on real hardware (no gate builds it), and no
live v4 pairing between two machines.

## #156 — the '1' glyph renders corrupt everywhere — CLOSED 2026-09-02

> **ROOT-CAUSED AND FIXED.** Not a tile-content bug, and not an August
> regression. It is a **nibble-parity bug in the macOS-only NEON 4bpp blit
> tail**, dating to `b2c79d7c` (giblet renderer, 2026-05-05).
>
> `sw_blit_neon.c` -> `sw_blit_indexed4_row()`: when `x_lsb != 0`, a pre-step
> consumes the odd texel and advances `packed` to a low-nibble boundary, and the
> SIMD body advances 8 bytes per 16 texels — so the scalar tail always resumes
> on parity 0. The tail instead started at `local_x = 1`, **transposing every
> remaining texel pair**. Fixed by starting parity at 0, matching the
> already-correct `sw_blit_neon_armv7.c` (`int parity = 0;`).
>
> **Trigger: any unscaled 4bpp blit starting at an odd texel x.**
> `SSPutStrTexInputPro` gives `'1'` a `sideL` of 1 (`ascProData['1'] = 0x12`)
> → u=137, odd. `'1'` is the only digit or uppercase letter with odd `sideL`,
> which is why it was the only corrupt character. The same bug corrupts the
> training-mode readout digits, where glyphs sit at `u = digit * 11`, so **odd
> digits corrupt and even digits are clean** — proven by running the kernel
> against chunk 4 of the real `scrscrn.ppg` from `SF33RD.AFS`: `'5'`'s
> `.#########.` becomes `.########.#` (the reported stray bar), `'3'` breaks at
> both edges, `'2'` is bit-identical.
>
> **macOS-only because of compile gates, not timing.** The NEON file's gate
> requires `__ARM_NEON && __aarch64__ && !CRS_SW_CANVAS_16BPP`; MiSTer sets
> `CRS_SW_CANVAS_16BPP=1` so the TU is empty, and Windows x86 has no
> `__ARM_NEON`. An **ARM64 Windows** build would be exposed — unverified
> whether any tester runs one.
>
> **Two conclusions below are superseded — left in place deliberately, as the
> record of how a plausible reading of correct evidence still pointed the wrong
> way.** (a) "Therefore the tile's CONTENT is wrong" — the tiles are fine; every
> observation supporting it is also consistent with a sampler that transposes
> pairs. (b) The prescribed bisect over ~12 August commits would have found
> nothing; the defect predates all of them by three months.

User-reported 2026-08-31, with a screenshot of the netplay stats overlay
(`R:0 P:16`) where the `1` is a garbled block. **Every `1` in the UI is
affected; every other character renders correctly.**

### Established, by reading the code and by user observation

- **Exactly one 8x8 atlas tile is wrong.** In the same string, `R`, `:`, `0`,
  `P` and `6` are all correct.
- **Static and changing `1`s are equally corrupt** (user-confirmed). That rules
  out the stale-pixel hypothesis: a narrow glyph failing to overpaint a wider
  predecessor would only corrupt values that CHANGE.
- **The width table is not the bug.** `ascProData['1'] = 0x12` (left trim 1,
  right trim 2, advance 5px) is the only non-zero entry among the digits, but
  it is ORIGINAL: the pre-refactor decimal array in the same file's history had
  `18` at index 49 too. `73d05703` reformatted decimal to commented hex and
  transcribed it correctly.
- **The sampler is self-consistent.** `SSPutStrTexInputPro` samples
  `u+sideL .. u+8-sideR`, draws a quad of width `slide = 8-sideL-sideR`, and
  returns `slide`; `SSPutStrProP` advances by exactly that. No overlap, no gap.

**Therefore the tile's CONTENT is wrong** — the bug is in how that tile is
loaded, decoded or cached, not in the text-drawing path.

### Explicitly NOT the April dirty-rect bug

`project-ppg-dirty-rect-corruption` (resolved 2026-04-07 by INDEX8
rasterization) is a different, older issue and its notes should not be trusted
here — the user states this regression is from **this month**. Do not re-derive
from that entry.

### Next step: bisect, not more reading

~12 August commits touch rendering/textures/texture-cache — `texcash` pattern
append bounds and refcount traps (`8d623eae`), `texgroup` reclaim (`731d22a0`),
the hash/pattern-bound assertions (`658e0026`, `aba90c32`), the rolled-back
char-select texture-block fix (`2e7778f1`). Reading diffs cannot distinguish
them; building can. ~5 builds with a human glancing at any `1`.

Unrelated to the netplay work in #143-#155; do not conflate.

## #157 — desktop netplay launcher (SDL3) + netplay.status export — CLOSED

Goal: host or join a netplay game from Windows/Mac/Linux without hand-writing a
handoff file. Must serve MiSTer<->desktop and desktop<->desktop; MiSTer<->MiSTer
already works through the OSD and is out of scope.

### Why this is small

**The handoff file is already a control API.** The game self-navigates from a
cold launch: write `mode=host|join`, `port=`, `peer_code=`, pass
`--direct-p2p-handoff <path>`, and `netplay_nav` injects START presses to walk
Title -> Mode Select -> Versus and calls `Netplay_BeginDirectP2P()` at
character select, unaided (`src/netplay/direct_p2p_handoff.h`, `src/main.c`,
`src/netplay/netplay_nav.h`). The MiSTer OSD is one caller. This is a second.
No engine or protocol change is required.

Hosting on desktop ALREADY works end to end today — the code renders in the
overlay. The only missing capability is **entering** a code.

### Part 1 — `netplay.status` (the enabling change)

Mirror `arcade_balance.c`'s `write_status_file()` exactly (same pref-path
location, same line-oriented shape, same "write on every resolution" habit):

    <pref>/netplay.status
      line 1: state   -- HOSTING | IDLE
      line 2: room code, or empty when not hosting

Written where the host publishes (`direct_p2p.c`, the "HOST_WAITING published"
site) and cleared on teardown/cancel so a stale code can never be read back.

Note the log deliberately REDACTS the code (`RoomCode_Redact`, 3 chars). This
file is the unredacted surface, which is the point — it is local, in the user's
own pref dir, and it is what a launcher (and later a lobby agent) consumes
instead of screen-scraping.

### Part 2 — the launcher

A **second CMake target** in this project, not a separate app: SDL3 is already
vendored and built for all four targets, so this adds zero runtime dependencies
and ships inside the existing package. A friend double-clicks one thing.

Everything it needs exists in the vendored SDL3, verified present:
`SDL_CreateProcess` (portable spawn -- no per-platform fork/exec),
`SDL_SetClipboardText`, `SDL_StartTextInput`.

Modes:
1. **Host** -- write `mode=host`, spawn the game, poll `netplay.status`,
   display the code and copy it to the clipboard.
2. **Join** -- text field for the code, write `mode=join` + `peer_code=`, spawn.
3. **LAN** -- `--p2p-remote-ip <ip> --p2p-local-player <1|2>`, no rendezvous,
   no room code. Cheap to include and it is the configuration that would have
   saved most of the 2026-08-31 cross-arch debugging session.

Validate a pasted code with `RoomCode_Decode` BEFORE spawning, so a typo
reports "invalid room code" in the launcher instead of costing a 15 s punch
against a stranger's address. v4 codes are 12 chars; the decoder also reports
OLD_FORMAT for 11/14/18-char legacy codes, which the launcher should surface as
"code is from an older version" rather than a generic failure.

### Constraints

- Do not change the handoff format, the wire protocol, or `MIST_PROTO_VER`.
- Do not regress the MiSTer OSD path -- it writes the same handoff file.
- The launcher must not depend on Python or any runtime the game does not
  already ship.

**CLOSED.** Part 1: `netplay_status_publish()` mirrors `arcade_balance.c`'s
`write_status_file()`. Hooked into `set_state()` — the single writer of
`s_state` — so HOSTING/IDLE tracks HOST_WAITING **by construction** rather than
by enumerating exits. Three gaps closed deliberately: `host_commit_endpoint()`
refreshes on a drift re-encode (the one code change with no state transition),
`DirectP2P_Init()` clears at boot (SIGKILL-while-hosting leaves HOSTING on
disk), and the launcher rewrites IDLE before every host spawn so anything it
reads was written by the child it spawned. Unredacted on purpose: local pref
dir, and it exists so a launcher need not screen-scrape.

Part 2: `3s-netplay-launcher`, desktop-only, excluded from ARM/Miyoo/fbdev so
the cross-build never sees it. `SDL_RenderDebugText` for UI — deliberately
independent of the game's proportional-font atlas, so #156's corrupt `'1'`
cannot reach a screen whose whole job is displaying room codes.
`SDL_CreateProcess` for spawn (no per-platform fork/exec), clipboard, text
input. Host / Join / LAN plus CLI equivalents. `RoomCode_Decode` runs BEFORE
spawn; MALFORMED / OLD_FORMAT / FUTURE_VERSION each get their own actionable
message instead of one generic failure.

Verified live on macOS: host end-to-end (launcher → game → HOST_WAITING → code
in `netplay.status` → clipboard), join validate-then-spawn, LAN spawn reaching
nav-armed Versus, and a crash-stale code cleared by the next boot.

**Residual:** Windows/Linux launcher builds are untested (the target compiles
only where it is configured, and only macOS was exercised); no two-machine
host↔join through the launcher yet; no macOS packaging rule for the launcher.
Also pre-existing and out of scope: a desktop with no CPS3 romset refuses to
arm netplay, so the host screen sits at "waiting for the room code" — the
romset search covers the five `/media/...` MiSTer paths plus the
`THIRDSARM_CPS3_ZIP` dev hook only.

**Post-review fixes.** The review (run after shipping, at the maintainer's
request for speed) found one P-1 with an external victim and it is fixed:
**nothing on the game's own shutdown path clears `netplay.status`**, so a
graceful quit while hosting left `HOSTING`+code on disk and the launcher kept
presenting a dead endpoint as live — the user would hand that code to someone
who then punched a stranger's address for 15 s. The three holes the original
commit closed all clear *before the next host*, not while the launcher is still
showing the last one. Fixed in the launcher by making the **child's liveness
the authority, not the file**: a code is live only while the process that
published it still runs.

Also fixed: `start_host()` cleared the status file *before* refusing a
duplicate spawn, wiping the code of a game that was genuinely still hosting
(and since the game only republishes on a transition or drift re-encode, the
launcher then waited forever). And `ROOM_CODE_FUTURE_VERSION` told the user to
update the game for *any* 12-char string not starting with `4` — including a
mis-heard first character, which is a typo. Reworded to say so.

**Citation correction.** The original commit claimed each repoint was "read at
its target". That was false for 8 of them, which were delta-preserved rather
than read — they were already wrong at HEAD and stayed wrong. Worse, one
**commit-pinned** citation was actively corrupted:
`9240aa50:direct_p2p.c:1341-1364` was delta-shifted to `:1393-1416`, which is a
struct-field block, not the analysis the prose cites. A SHA-pinned citation
refers to a snapshot and must never move with the working tree. Restored and
verified against `git show`. The 8 pre-existing wrong anchors are NOT fixed
here — they predate this task and the checker cannot see them.

**Still open:** the unserialized pair in `set_state()` (atomic store then
unlocked file IO) can, on a Cancel→re-host race, leave `HOSTING` on disk for
the rest of the process. PLAUSIBLE, structural; the launcher's liveness check
now masks the user-visible symptom.

---

## #158 — visible seams across sprite chips during supers — CAUSE B FIXED, CAUSE A OPEN (decision)

User-reported 2026-09-02: "during some supers, while the character is
performing the super action there will be noticeable seams during the
animation." Observed on Makoto SA1 and Q SA1, "and a few others."

**Reproduces on MiSTer as well as macOS** (user-confirmed). That is the load-
bearing fact: it excludes `sw_blit_neon.c` entirely — MiSTer sets
`CRS_SW_CANVAS_16BPP=1`, so that TU compiles to nothing there — and therefore
excludes #156's nibble-parity bug and every sibling sampler in that file. The
two reports are unrelated despite both being glyph/sprite corruption on macOS.

### Why supers and not normal moves

Per-frame ROM data carries a zoom field. `charset.c` -> `setupCharTableData()`
bulk-copies the frame record into WORK (which holds `u16 cg_zoom`); `bg_sub.c`
-> `check_cg_zoom()` reads `plw[0].wu.cg_zoom` / `plw[1].wu.cg_zoom` and sets
`zoom_request_flag`/`zoom_request_level`; `zoom_ud_check()` ramps it; `bg.c` ->
`Zoom_Value_Set()` sets `scr_sc` (`scr_sc = 64.0f / zadd;`); `scr_calc()` /
`scr_calc2()` bake `njScale` into `BgMATRIX`; `mtrans.c` -> `mlt_obj_matrix()`
applies it to every character chip.

At `scr_sc == 1.0` geometry is integer and `rasterize_textured` takes the
unscaled exact-copy path — clean. A super whose frames carry nonzero `cg_zoom`
routes the whole character through the **scaled** rasterizer, where both causes
below live. (Which specific SAs carry nonzero `cg_zoom` is CPS3 ROM data and
was not verified.)

### Cause B — FIXED 2026-09-02

`software_renderer.c` -> `rasterize_textured()`. `u_start_hi_fx` is built with a
`+ 1.0f` lead. The **unscaled** reverse start pays that back in full
(`src_x_start_rev` subtracts `0x10000`), but the **scaled** reverse start
subtracted only `du_fx` — correct solely when `du == 1`. At any other zoom the
flipped start was off by `(1 - du)` texels; at `scr_sc = 64/42` the first pixel
of every flipped chip sampled texel 16 of a 16-texel cell, i.e. a foreign column
from the adjacent atlas cell, at every chip edge.

Corroborating asymmetry: the y-flip start has no `+ 1.0f`
(`v_start_fx = (v_hi - flt_v_lead) * 65536`) and is correct. Forward scaled
paths never leave the cell.

Fixed by subtracting a full texel, mirroring the forward mapping. Identical
behaviour at `du == 1`. **Affects flipped sprites only** — it is not the whole
bug.

### Cause A — OPEN, needs a decision, not a patch

`mtrans.c` -> `seqsStoreChip()` snaps all four quad corners with `SDL_roundf`
(added by `244074bc`, "perf: integer-snap sprite positions", 2026-04-03;
content-verified present on `mister`).

Geometry stays gapless — abutting chips share bit-identical floats, so both
round identically — but each chip's *rounded width* differs (16 texels over
18px vs 19px), so `flt_du = (u_hi - u_lo) / flt_w` **varies per chip** and the u
accumulator **restarts per chip**. Simulated at `scr_sc = 64/56`: duplicated-
column cadence is `2,5,6,6,8,8,8,…,7,5,6`, jumping at every chip edge (chips
alternating du=0.842/0.889); with the snap removed, du is a uniform 0.875 and
duplication spreads evenly across boundaries — an ordinary upscale. That
per-band phase jump is the reported "assembled from mismatched pieces" look.

**The decision:** the snap exists to route sprites into the exact-copy fast
path. Skipping it under zoom sends supers through the slower gather rasterizer —
a frame-time change on MiSTer during the most effect-heavy moment in the game.
The narrow form is to skip the snap only when the transform is not
translation-only: the perf rationale does not apply under zoom anyway, because
the sprite takes the scaled path regardless.

**Held by the maintainer 2026-09-02 pending that call.** Until it lands, seams
persist for non-flipped chips.

### Not verified

Which SAs carry nonzero `cg_zoom`; the relative visual weight of A vs B in the
user's screenshots. The latter needs a frame dump — `tools/frame-data/run.sh`
can drive supers scripted but has no framebuffer-dump hook today.
