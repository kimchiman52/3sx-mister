# Plan: Live Server-Side Transcode-and-Stream for Fightcade Replays

**Status: INVESTIGATION + PLAN — nothing implemented. Written 2026-07-24
against branch `feat/fcade-replay-browser` (HEAD `fd09bfd8`).**

Goal UX: MiSTer browses the catalog → selects a replay → playback starts in
**seconds** and runs in real time while the server relays the GGPO stream —
a Twitch-VOD model, not "wait for the whole conversion, then watch."

Every load-bearing claim below cites `file:line`, a command + its observed
output, or a doc section. Facts were verified on 2026-07-24 by reading the
cited code, parsing the committed wire captures, probing the live GGPO
server once (bounded, single connection), and inspecting the live VPS.
Where something could not be verified it is flagged **UNVERIFIED** or
**MEASURED-ONCE**. No pushes, no code changes anywhere in this plan
(user rules).

---

## 0. The crux answer, up front

> **Does producing a playable replay require running FBNeo over the WHOLE
> match (full RAM-dump emulation), or only a one-time transform of the
> initial savestate, after which the raw input stream drives playback?**

**Answer: neither extreme. Playback needs (a) a 12-byte setup block that
only FBNeo can produce — extracted from ONE frame (the game-start
signature frame), not the whole match — plus (b) a per-frame input-word
stream, plus (c) the stream offset of that game-start frame. FBNeo must
*consume* the whole session server-side (it is the only way to reach each
game's start-signature frame and to know the offset), but it does so as a
lightweight real-time tracker gated by stream arrival — NOT as a
20-minute-blocking, RAM-dump-to-disk batch job. Playback on the device can
begin the moment the first game's signature frame is reached, which the
stream delivers in seconds. Live streaming is FEASIBLE.**

Deciding citations:

1. **What a `.3sr` is:** 28-byte setup header + `frame_count × 4` input
   words + sparse checksum table (`docs/3sr-format.md` §1). Setup = P1/P2
   character, super art, color, `new_challenger`, `Random_ix16/32` — all
   read from the **single** SCRD frame matching the game-start signature
   `G_No[1]==2 && G_No[2]==0 && G_No[3]==0`
   (`tools/fcade-replays/make_3sr.py:202-230`; format §2: "there is only
   one frame read for the whole setup block"). Input words are the
   per-frame `P1SW_0`/`P2SW_0` u16s (`make_3sr.py:232-241`).
2. **Size reconciliation (input-words-only confirmed):** the pool's
   ~18 KB / ~4,400-frame figure is exactly the format:
   `28 + 4400×4 + ceil(4400/60)×8 = 18,220 B`. Verified against a real
   published file pulled from the live VPS store this session
   (`/opt/fcade-proxy/3sr/1784875689006-4849/game_0.3sr`): parse output
   `magic=b'3SR1' … frame_count=487 interval=60 checksums=9
   expected_size=2048 actual=2048 match=True bytes/frame=4.205`. A `.3sr`
   is 4 B/frame of inputs + amortized checksums — **no per-frame game
   state** (format §6: "No savestate / full RAM snapshot").
3. **The raw −13 input stream alone drives in-game playback:** Step B3's
   POSITIVE sub-finding (commit `747ef313`, DEBUG mode in
   `src/test/test_runner.c:1289-1480`): *"re-anchored to B1's SCRD-derived
   per-game offset at round-init, 9791 game_0 reproduces FRAME-EXACT from
   cold boot with NO RNG sync (4097/4098 positions exact; 99.9% incl.
   timer+vitals). The engine plays raw −13 in-game runs bit-faithfully;
   only setup + offset need SCRD/3SR."* (Caveats in §1.4.)
4. **But setup/offset are NOT reconstructible without FBNeo:** the same
   B3 record is a NO-GO on self-contained playback — the stream carries no
   coin and no START press in the pre-game prefix; char-select outcome
   depends on cursor/state resident in the (engine-unloadable) FBNeo
   savestate; the port's char-select→round-start transition runs 56/57
   frames longer than the arcade's and is not cross-quark constant
   (commit `747ef313` message; `docs/fcade-replay-notes.md:1064-1069`).

So: **one FBNeo pass over the session, in-process, no RAM dumps to disk,
emitting setup+offset at each game start and (optionally) input
words + checksum fields per frame — that is the entire server-side
transcode.** The full-RAM-dump/statcheck machinery is the *offline
verification* harness, not a playback requirement (§1.5).

---

## 1. Verified fact base

### 1.1 What the device player consumes, and how it starts

`src/replay/replay_player.c`:

- `ReplayPlayer_Init` loads the **whole** `.3sr` up front (`SDL_LoadFile`,
  `:392`), validates `size == header + frame_count*4 + checksum_count*8`
  **exactly** (`:459-464` — a growing file is rejected today), converts
  every input word arcade→SWK at load (`:492-510`).
- The phase machine (`:860-991`) then drives **our own engine's console
  char-select** using only the setup block: title mash → menu →
  `Last_My_char2/Last_Super_Arts` from header (`:877-881`), cursor
  placement via a 20-entry lookup (`:736-738`, `:898-905`), color taps
  (`:921-922`), `New_Challenger` (`:914`), RNG seed at the exact
  statcheck sync point once `G_No[1]==2` (`:948-955`). In `PHASE_GAME` it
  injects one word pair per tick (`:970-973`) into
  `p1sw_buff/p2sw_buff` (`:989-990`), hooked from `src/main.c:668`
  (tick) / `:775` (epilogue) / `:1156` (`--play-replay` boot load).
- Divergence detection is the sparse 13-field djb2 checkpoint table
  (`:1113-1197`); on mismatch → `REPLAY DESYNC` + stop injecting
  (`:1186-1196`). It never needs the SCRD archive on-device.
- Measured navigation overhead: a 5,380-frame (89.7 s) replay completed in
  **99 s wall** on-device (`docs/fcade-replay-notes.md:1024-1027`) —
  ≈ **9.3 s** of boot/menu/char-select navigation before gameplay.

**Setup is per-replay data, engine-agnostic in mechanism:** the phase
machine is the same for every match; only the 12 header bytes vary.

### 1.2 What `make_3sr.py generate` consumes today, and why

`cmd_generate` (`make_3sr.py:519-543`) reads **only** the `.scrd` archive:
`extract_scrd_game()` walks the RAM-dump frames once, taking the setup
block from the single signature frame (`:202-230`) and, for every later
frame, the two input words + (every 60th frame) the 13-field checksum
(`:232-241`). `--quark-json` feeds only the `.meta.json` names sidecar
(`:447-513`). So today's pipeline needs full-match RAM dumps because the
**input words and checksums are read back out of emulated RAM** — but
each of those fields is readable in-process from FBNeo's live RAM without
ever writing a 512 KB/frame dump (the runner already does in-process RAM
field reads every frame: `run.cpp:146-182`, big-endian normalize +
`kDumpGameStateOffset` in-game check).

### 1.3 The GGPO wire protocol — shape, order, pacing (measured)

- **Framing:** u32be length + payload (`tools/fcade-replays/
  fcade_replay_tool.py:73-75`); handshake `:171-202`. Message types:
  `1` ack, `3` metadata (player-name strings, `:205-221`), `−12` = zlib
  savestate, sent ONCE (`:237-249`; always 1,907,010 B decompressed,
  `docs/fcade-replay-notes.md` §5.2), `−13` = input records, 10 B/frame,
  `record_count` always 60 → 600 B = 1.0 s of session per message
  (notes §5.1, 2,888 messages across 4 replays, zero variation).
- **Order:** in all 4 committed captures
  (`tools/fcade-replays/output/*/summary.json`, parsed this session) the
  sequence is `(0,1) (1,3) (2,−12) (3,−13) (4,−13) …` — **the full
  savestate arrives before the first input record, ~0.8 s after
  connect.** First-frame latency is therefore NOT gated on emulating past
  char-select before the savestate exists; the savestate IS the starting
  point and it arrives immediately.
- **No client pacing:** the receive loop is a bare `recv_frame()` loop
  with idle-timeout accounting and no acks/sleeps
  (`fcade_replay_tool.py:333-362`; identical device port
  `src/replay/fcade_stream.c:774-819`). The server decides the rate.
- **Measured pacing (MEASURED-ONCE, live probe 2026-07-24):** a bounded
  90 s instrumented capture against a fresh catalog quark
  (`1784897786212-2077`, 207.9 s duration) from this Mac:

  ```
  connect_rtt=0.121s
    t=   0.318s  type=1    len=8
    t=   0.516s  type=3    len=55
    t=   0.819s  type=-12  len=122345
    t=   0.819s  type=-13  len=612
    ...
  minus13_msgs=182 frames~=10920 stream_seconds~=182.0
  first_-13_at=0.819s last_-13_at=30.126s wall_span=29.307s
  pacing_ratio stream/wall = 6.18x  (1.0 = exactly real-time)
  gap_ms min=0 p50=166 p90=169 max=198 mean=162
  ```

  **The server delivers ~6.2× faster than real time** (one 60-frame
  message every ~166 ms), then EOFs cleanly. The entire 3.5-minute
  session arrived in 30 s. This *revises* the working assumption that
  "a 20-min replay takes ~20 min to pull": at this rate a 20-min
  session's inputs arrive in ~3¼ min. It is consistent with the only
  prior wall-clock observation ("a 1,043 s session took several real
  minutes to fully arrive", `docs/fcade-replay-notes.md:155-161`); the
  ~20-min figure describes the *whole offline pipeline* (download +
  tens-of-GB RAM dumping + compress + statcheck), not the wire. One
  sample only — S0 re-measures across quark ages/lengths.
- **No Cloudflare on the stream port:** every download in this program's
  history (Mac and on-device) hit `ggpo.fightcade.com:7100` over plain
  TCP with no cookie and succeeded (notes §4 4/4, §8.4 6/6, §11
  on-device fetch), during the same period the `searchquarks` HTTPS API
  was Cloudflare-403 (notes §3; `fcade_replay_tool.py:158-165`;
  `tools/fcade-proxy/fcade-proxy.js:399-404` classifies the 403;
  `tools/fcade-proxy/README.md:13-14`: "the ungated Fightcade *stream*
  protocol — no cookie, no TLS"). The MiSTer itself can open this socket
  (kernel 5.15 IPv4 POSIX TCP; `fcade_stream.c` is already on-device and
  verified: notes `:1088-1092`).

### 1.4 The B3 evidence — exactly what is and is not proven

Proven (commit `747ef313`; harness `src/test/test_runner.c:1289-1480`,
flags `src/args.c:518-561`):

- **NO-GO** on device-self-contained playback (no FBNeo anywhere): no
  coin/START in the pre-game stream, char-select state lives in the
  savestate, transition-length skew 56/57 frames and not constant.
- **POSITIVE**: with the SCRD-derived per-game offset, feeding the raw
  decoded −13 words (arcade→SWK converted, `test_runner.c:1321`) from the
  engine's own cold boot reproduces `9791/game_0` frame-exact —
  4,097/4,098 positions exact, no RNG sync needed.

Honest caveats on the POSITIVE:

- **One quark.** Not yet replicated across the corpus.
- It ran in the DEBUG test runner with **game-mode=arcade** pinned
  (`test_runner.c:1344`), not the shipped player's console-mode phase
  machine; the shipped player's proven input source is the RAM-observed
  words, whose agreement with the decoded −13 stream is high but not
  100% (B1's jitter analysis exists precisely because of ±1/±2-frame
  latch/hitstop mismatches — `tools/fcade-replays/decode_inputs.py:
  394-477`).
- **Consequence for the design:** the recommended architecture does NOT
  bet on raw −13 fidelity. Since FBNeo runs server-side anyway, the
  tracker relays the **RAM-observed `P1SW_0/P2SW_0` words** — i.e. the
  byte-identical content a `.3sr` would carry — so live playback inherits
  the exact fidelity of the proven offline pipeline. Raw −13 relay
  remains a validated-fallback simplification if S0 replicates B3's
  positive more widely.

### 1.5 What the RAM-dump/statcheck stage is FOR

Two separable roles, currently fused:

1. **Production inputs** (setup + words + checksums) — today read from
   `.scrd` dumps (§1.2), but every field is a fixed-offset RAM read that
   the runner can do in-process per frame. **Not inherently a batch/disk
   stage.**
2. **Verification** — `tools/statcheck_runner.py:150-199` runs the
   statcheck engine build per archive and gates on exit 0; it produces
   nothing, only PASS/FAIL. `publish_3sr.py`'s gate drops divergent games
   before publishing (notes §12). **A live stream cannot be
   pre-verified** — there is no complete archive before playback. The
   observed genuine-divergence rate: 1 of 14 measurable corpus archives
   (7.1%; `2133/game_0`, 1-pixel drift at frame 2,248 ≈ 37 s in —
   notes §8.5), and 5/5 clean in the newer S1 batch (notes §12). The
   sparse checkpoint detector still runs during live playback (the
   tracker computes the same 13-field hashes from live FBNeo RAM), so a
   divergent stream aborts with the existing `REPLAY DESYNC` UX at
   ≤ 60-frame granularity — it is caught, just not *before* you start
   watching.

### 1.6 Server + device assets already in place

- **Proxy**: framed-JSON TCP service on the VPS, ops `search`/`status`/
  `get3sr` (`fcade-proxy.js:879-891`; framing `:136-140`), serving a
  live 295-row catalog and a 28-quark / 2.6 MB `.3sr` store (checked
  live this session). Device client `src/replay/proxy_client.{c,h}`
  with async workers and a 256 KiB frame cap (`proxy_client.c:46`).
- **Device GGPO client** `fcade_stream.c` (plain POSIX TCP, every build
  flavor) — relevant to Option C only.
- **OSD/wrapper**: REMOTE tab + download-that-plays landed (`fd09bfd8`);
  argv-injection handoff template for booting the game with
  `--play-replay <path>` (`docs/plan-osd-replay-browser.md` §1.4-1.5,
  citing `thirdsarm_wrapper.cpp:2819-2843`).
- **Auto-convert pool**: daily Mac LaunchAgent converts newest-N catalog
  quarks and pushes `.3sr`s (`tools/fcade-replays/auto-convert/README.md`)
  — the "watch pre-converted replays" path that live streaming
  complements, not replaces.

### 1.7 VPS + FBNeo feasibility (verified live; build UNVERIFIED)

Checked over SSH this session (`hetzner-3s-arm`):
**aarch64**, Ubuntu 24.04.3 LTS, 2 vCPU, 3.8 GB RAM, 32 GB free disk,
node v20.20.2, `fcade-proxy` service active.

> The task brief assumed an x86_64 VPS build — **the VPS is ARM64.**

FBNeo runner (`crowded-street/fbneo-replay-runner` @ `ccf96ab`, inspected
from a fresh clone):

- The only proven build is macOS **arm64** (`make sdl 'BUILD_X86_ASM='
  'CPUTYPE=arm64'`, notes §1) — same CPU family as the VPS, which helps
  (no x86-asm paths involved).
- `makefile.sdl:141-146` has a Linux branch, but it links
  `-lSDL -lGL -lGLU` (SDL1-style name + OpenGL) while the code is
  `-DBUILD_SDL2` (`:365`); Darwin uses `pkg-config sdl2`. **A Linux
  aarch64 build is plausible but UNVERIFIED and will need a small
  makefile fix** (pkg-config sdl2 + mesa GL or GL-stub for headless).
  `make` hard-pins `DEBUG=1` (`makefile:44`) — the known
  `cps3_debug_harness.d` first-pass bug (notes §1) will recur.
- Headless mode is real and cheap: `-headless` skips all video/audio
  (`src/burner/sdl/main.cpp:102-104`; `RunFrame` headless branch does no
  draw/sound — `run.cpp:545-560`), i.e. emulation runs unthrottled.
  **Emulation speed on the 2-vCPU aarch64 VPS is UNVERIFIED** — the live
  design only requires FBNeo ≥ ~1.2× real-time (it consumes at stream
  arrival rate, up to 6.2×; anything ≥ 1× keeps the device fed). S0
  measures it.
- The runner loads the entire `inputs` file at startup
  (`ReplayInit`, `run.cpp:405-437`) and consumes exactly one 10-byte
  record per emulated frame (`ReplayApplyFrameInputs`, `run.cpp:379-403`)
  — live mode needs a **tail-follow/stream-feed modification** (small:
  the 1-record-per-frame contract already exists; only the "EOF =
  finished" assumption changes).

### 1.8 Real-time viewing — what is actually inherent

The wire is ~6.2× real-time (§1.3); the **engine** plays at 60 fps. So:

- You watch at 1× — there is no skimming a 20-min replay in 2 min in v1
  (the player has no seek; an inputs-only format can only fast-forward by
  emulating faster, never rewind without re-simulation).
- But the stream *outruns* playback ~6:1 — the buffer only grows, a full
  20-min match is fully on-disk ~3¼ min in, and the completed stream file
  IS a normal `.3sr` (recommended design, §3.A) that lands in the LOCAL
  list for later 0-latency rewatching.

---

## 2. The character-select question — explicit verdict

**No. No engineering effort to "match the CPS3 char-select to ours" is
needed, and none should be spent.** The MiSTer never reconstructs the
CPS3 char-select. The interface between the two worlds is the 12-byte
setup block: FBNeo (server-side, from the savestate) resolves what the
players actually picked; our player then drives **our own** console-mode
char-select to the same outcome with cursor/color/SA automation that
already ships and is corpus-proven (`replay_player.c:736-937`; 13/14
desktop PASS + 15/15 ARM verdict parity, notes §8.5/§9.4; live-catalog
batch 5/5, notes §12). B3's NO-GO (§1.4) proves the alternative
(reconstructing char-select from the stream) is structurally impossible —
and the shipped design simply never needs it. The setup block is
per-replay *data*, not per-replay *engineering*.

---

## 3. Architecture options

### Option A — Server live transcode + relay, "3SR-as-a-stream" (RECOMMENDED)

VPS, on viewer request: connect to `ggpo.fightcade.com:7100`, feed
savestate+inputs to a tail-follow FBNeo tracker (headless, in-process RAM
reads, **no dumps, no disk**). At each game-start signature the tracker
emits the setup block + stream offset; every subsequent in-game frame it
emits the RAM-observed `P1SW_0/P2SW_0` pair, plus the 13-field checksum
every 60 frames — i.e. it emits **exactly the bytes of a `.3sr`,
incrementally**. The proxy relays these as framed chunks over the existing
device protocol (new `watch` op). The device appends them to a growing
per-game `.3sr` file and a streaming-aware player variant starts playback
once a start-buffer threshold is met.

- **First-frame latency** ≈ connect+savestate (~1 s, measured) +
  pre-game prefix arrival (prefix length UNVERIFIED; at 6.2× a 60 s
  char-select prefix ≈ 10 s wall) + device boot/nav (~9.3 s, measured,
  §1.1) + start buffer (~3-5 s). **Estimate: ~15-30 s to first gameplay
  frame.** Steady state: real-time, buffer growing ~5 stream-sec per
  wall-sec.
- **Where FBNeo runs:** VPS, one instance per active watch session.
- **Protocol changes:** one new proxy op (`watch`) streaming framed
  chunks (chunked to respect the device's 256 KiB frame cap,
  `proxy_client.c:46`); a session-teardown message.
- **Device changes:** streaming input source for `ReplayPlayer`
  (replace the load-whole-file + exact-size check, `replay_player.c:
  392,459-464`, with header-then-append consumption; stall = hold the
  tick instead of `finish("inputs-exhausted")`, `:960-962`); wrapper
  handoff flag (`--play-replay-live <quarkid>` cloning the §1.6 argv
  template); OSD "WATCH" action.
- **Failure/desync:** divergence caught by the existing checkpoint
  detector at ≤ 60-frame granularity (same UX as today, §1.5) — but no
  pre-play statcheck guarantee (~7% historical divergence rate, §1.5).
  GGPO EOF mid-match / VPS death → player sees stream end → clean abort
  overlay. Completed streams persist as ordinary `.3sr` files (free
  VOD-cache side effect).
- **VPS load:** one FBNeo per viewer (CPU: S0 measures; RAM ~ tens of
  MB + 1.9 MB savestate; network ~4 KB/s per leg — trivial). Cap
  concurrent sessions (2-vCPU box).
- **Complexity:** highest of the options — runner patch + proxy op +
  streaming player. But every fidelity-bearing byte is identical to the
  proven offline pipeline.

### Option B — On-demand server VOD conversion ("convert-on-select"), no streaming player

Same VPS FBNeo, but batch: viewer selects → server pulls the stream
(≈ duration ÷ 6.2), converts (FBNeo tracker keeps pace with arrival, so
conversion finishes ~when the download does), then the existing `get3sr`
path serves it; the OSD shows progress. **Zero device-engine changes**
(the shipped player + `ProxyClient_Fetch3sr` already work end-to-end,
notes §12).

- First frame: ~duration/6.2 + fetch + nav — a 5-min match ≈ ~1 min, a
  20-min session ≈ ~3½ min. Not Twitch-VOD, but far from "wait 20 min."
- Can even keep a statcheck gate **if** the engine builds on Linux
  aarch64 (UNVERIFIED); without it, same desync tradeoff as A.
- Strictly a subset of A's server work (S0-S2 below) — it is the natural
  intermediate milestone, not a rival.

### Option C — Hybrid: device pulls −13 directly from GGPO; server supplies only setup+offset

The device already speaks the GGPO protocol (`fcade_stream.c`, on-device
verified). Server runs the FBNeo tracker just far enough to emit
setup+offset per game; the device opens its **own** ggpo connection and
decodes −13 records itself (bit-map is documented,
`decode_inputs.py:105-121`, and already ported once in the B3 harness).

**Rejected:**
- Requires **two simultaneous ggpo connections for the same quark**
  (server tracker + device) — server tolerance UNVERIFIED, and this
  program has deliberately kept to one live stream connection at a time
  (notes §4).
- Abandons RAM-observed words + streamed checksums for raw −13 words —
  the one-sample B3 positive (§1.4 caveats) would carry the whole
  fidelity load, with no divergence detector at all.
- The bandwidth it saves the VPS is ~4 KB/s — noise.
- Most new device code of any option (a −13 decoder + aligner in the
  engine or wrapper).

### Option D — Fully self-contained on-device playback (no server)

**Already NO-GO by experiment** (B3, §1.4): setup cannot be reconstructed
from the stream. Listed only for completeness.

### Recommendation

**Option A, staged through Option B.** S0-S2 build the shared foundation
(VPS FBNeo + tracker); S2 ships Option B as a working milestone (remote
picks become watchable in ~duration/6 with zero device risk); S3-S5 add
the streaming relay + streaming player to reach seconds-scale start. If
S0b (FBNeo-on-VPS) fails its gate, Option B on the Mac-side auto-convert
rail (already shipping) remains the floor, and Option A is re-evaluated
on different hosting.

---

## 4. Stage map & dependencies

```
S0a  spike: prefix-only .3sr + shipped-player fidelity     (Mac, repo tools)
S0b  spike: FBNeo on the VPS — build + speed + tail-follow (VPS)
S1   runner live-tracker mode (setup/words/checksums emit)  (needs S0b)
S2   proxy convert-on-select + get3sr wait UX  [= Option B] (needs S1)
S3   proxy `watch` op — chunked 3SR-stream relay            (needs S1)
S4   device streaming player + buffer/stall policy          (needs S3; S0a informs)
S5   OSD/wrapper WATCH wiring                               (needs S4)
S6   hardening: multi-game, desync UX, limits, cleanup      (needs S4/S5)
```

---

## Stage S0a — SPIKE: prefix-only conversion + shipped-player fidelity

**Title:** Prove the two crux assumptions with tools already in the repo,
before any server work.

**Why:** the riskiest *fidelity* assumptions are (1) a `.3sr` built
without full-match RAM dumps is byte-equivalent where it matters, and
(2) the **shipped** player (console-mode phase machine — not B3's
arcade-mode DEBUG harness) plays a stream-fed word sequence identically.

**Create/modify (scratchpad-only, nothing lands in the repo):**
- For ≥ 3 corpus quarks that already have full-pipeline `.3sr`s:
  re-derive the setup block from ONLY the signature frame (that is
  already all `extract_scrd_game` reads — assert byte-equality of the
  28-byte header), and build a variant input table from the **decoded
  −13 stream** at the B1 offset (`decode_inputs.py --bin` + the B1
  alignment) instead of the RAM-observed words. Diff the two word tables;
  quantify B1 jitter frames.
- Play both variants with the shipped desktop player
  (`--play-replay`, `SDL_VIDEODRIVER=dummy`); compare
  `REPLAY COMPLETE … checksums=N/N` outcomes. (The RAM-word variant's
  checksums are the oracle; the −13 variant tests whether Option A's
  raw-relay simplification is ever safe.)
- Re-measure GGPO pacing on 3 more quarks including one ≥ 15 min and one
  2022-vintage (probe script exists in the session scratchpad:
  `pacing_probe.py`; keep single-connection politeness).
- Measure the pre-game prefix length distribution (first −13 record →
  game_0 signature) across those quarks — it is the dominant unknown in
  the first-frame latency budget.

**Success criteria:** setup-block byte-equality on all quarks; RAM-word
prefix variant plays checksum-clean wherever the original did; a written
GO/qualified-GO/NO-GO on the −13-word variant; pacing + prefix numbers
recorded in `docs/fcade-replay-notes.md`.

**Dependencies:** none (corpus `.scrd`s regenerable via
`publish_3sr.py`; B1 tooling committed).

**What NOT to do:** no engine changes; no new formats; do not "fix"
jitter frames by fuzzy matching (B1's own rule).

**Failure/fallback:** if even RAM-word prefix variants misbehave in the
shipped player, the whole plan stops here and the finding is recorded —
that would contradict the shipped pipeline and demands investigation
first. If only the −13 variant fails, Option A proceeds with
tracker-observed words (the recommended path anyway).

---

## Stage S0b — SPIKE: FBNeo on the VPS (build, speed, tail-follow)

**Title:** Prove the runner builds and keeps pace on the aarch64 VPS.

**Why:** the riskiest *infrastructure* assumption. Everything server-side
depends on it.

**Create/modify (VPS + local runner clone; no repo changes):**
- Build the runner @ `ccf96ab` on the VPS: `make sdl 'BUILD_X86_ASM='
  'CPUTYPE=arm64'` with the expected small `makefile.sdl` lib-line fix
  (§1.7) and the known `cps3_debug_harness.d` stub workaround (notes §1).
  Provision `sfiii3nr1.zip` + parent `sfiii3.zip` (both required — notes
  §2; copy from the Mac, never commit).
- Run one downloaded session headless with `-replay-state`/
  `-replay-inputs`; measure emulated frames/sec (target ≥ 2× real-time
  for comfortable stream-following on this 2-vCPU box; the hard floor is
  ~1.2×).
- Tail-follow PoC: feed a growing `inputs` file (append in 600 B chunks
  at 6 Hz to simulate the wire) through a minimal runner patch that
  waits at EOF instead of finishing (`ReplayApplyFrameInputs`'s
  `gReplayFinished` at `run.cpp:386-389` is the single decision point).

**Success criteria:** clean build; measured fps figure recorded; the
tail-follow run tracks the growing file end-to-end and the per-frame
in-game gate (`run.cpp:178-179`) fires at the same frame indices as a
whole-file run.

**Dependencies:** none (parallel with S0a). VPS disk/CPU are the user's;
keep the build tree under a few GB (32 GB free measured).

**What NOT to do:** no RAM dumping to disk on the VPS (the §4 ENOSPC
lesson); no service wiring yet; do not leave a runner looping unattended.

**Failure/fallback:** build failure → try Ubuntu's SDL2 + mesa dev
packages; speed < 1.2× → Option A NO-GO on this box (record the number);
Option B remains viable Mac-side (auto-convert rail), or the user decides
on a bigger/x86 VPS.

---

## Stage S1 — Runner live-tracker mode

**Title:** A runner mode that emits the `.3sr` byte stream incrementally
instead of RAM dumps.

**Create/modify (runner fork; repo gains only docs):** a
`-track-3sr <out-dir>` mode: per emulated frame, read the fixed offsets
(`arcade_constants.h` values mirrored in `make_3sr.py:59-72`) from the
normalized RAM view the runner already builds in-process
(`run.cpp:165-176`); detect the game-start signature; write per game:
setup record (once), appended word pairs, checksum entries every 60
frames, and a game-boundary/end marker. Also emit the game's stream
offset (the runner's own consumed-record count at the signature frame —
this replaces B1's offline cross-correlation; S0a verified the
equivalence oracle).

**Success criteria:** for ≥ 3 corpus quarks, the tracker's completed
output is **byte-identical** to `make_3sr.py generate`'s `.3sr` for the
same game (this is the whole point of relaying RAM-observed words —
diffable equivalence with the proven pipeline). Tail-follow input works
(S0b PoC folded in).

**Dependencies:** S0b.

**What NOT to do:** do not invent a new on-wire format here — the
tracker's output IS `.3sr` bytes (+ a tiny manifest); framing is S3's
job. Do not write 512 KB frames anywhere.

**Failure/fallback:** if some checksum field can't be replicated
in-process, fall back to words-only emission (checksum table empty is
legal: `checksum_interval == 0`, format §1) and record the lost
divergence-detection capability.

---

## Stage S2 — Convert-on-select on the VPS (ships Option B)

**Title:** Proxy `convert` op: pull + track + serve via the existing
`get3sr`, with progress.

**Create/modify:** proxy-side orchestration (node spawns the ggpo pull —
a port of `fcade_replay_tool.py download` or reuse of the runner's own
socket feed — plus the S1 tracker), a per-quark job state machine
(`queued/pulling/converting/ready/failed`), a `convertstatus` op for the
OSD progress row, concurrency cap (1-2 jobs), and completed output
dropped into the existing `3sr/<quarkid>/` store so `get3sr`
(`fcade-proxy.js:657+`) serves it unchanged. Device: OSD row state
"CONVERTING… n%" then the existing download-that-plays flow (`fd09bfd8`).

**Success criteria:** a catalog quark with no pre-converted `.3sr`
becomes watchable end-to-end from the OSD in ≈ duration/6 + ε, with no
device-engine change; `status` still reports catalog mode; store growth
bounded.

**Dependencies:** S1. (Optional: statcheck gate iff the engine proves
buildable on Linux aarch64 — separate spike, UNVERIFIED, not blocking.)

**What NOT to do:** no streaming yet; no device changes; don't run more
than 2 concurrent ggpo pulls (politeness precedent, notes §4).

**Failure/fallback:** this stage IS the fallback for everything after
it. If it works and the user is satisfied with ~minutes-scale VOD, S3-S5
are optional.

---

## Stage S3 — Proxy `watch` op: chunked 3SR-stream relay

**Title:** Stream the tracker's output to the device as it is produced.

**Create/modify:** new proxy op `watch {quarkid}`: starts (or attaches
to) a live convert job and streams framed chunks: `{setup}` then
repeated `{words[], checksums[]}` then `{game_end}`/`{session_end}`/
`{error}` — each frame ≤ 64 KiB (well under the device's 256 KiB cap,
`proxy_client.c:46`). Base64-in-JSON first (matches every existing op);
binary sub-frames only if measurement demands (the S1 rationale in
`plan-osd-replay-browser.md` Stage S1 "Failure/fallback" is the
precedent). Multiple viewers of one quark attach to one tracker.

**Success criteria:** a desktop test client (extended `proxy_client`)
receives setup within ~2 s + prefix-time of the request and words at
≥ 6× real-time; chunk cadence and totals match the S1 tracker's file
output byte-for-byte.

**Dependencies:** S1, S2's job machinery.

**What NOT to do:** no device UI; no un-framed side channels; the ggpo
leg stays server-side only.

**Failure/fallback:** if long-lived framed streaming through the proxy
proves awkward, degrade to chunked polling (`watchpoll {quarkid, from}`)
— same bytes, worse latency by one poll interval.

**IMPLEMENTED 2026-07-24 (chose the polling shape up front).** The op is
`watchpoll {quarkid, from?, game_index?}` — offset-based chunked polling,
NOT a long-lived framed stream. The fallback in this stage's own note was
adopted as the primary shape because it is strictly better matched to the
two hard constraints: the device client is lock-step
one-request-one-response (`src/replay/proxy_client.h:8-10`) and S4's player
is "play a growing `.3sr` file", so each poll's body bytes append 1:1 to a
local growing file (the `get3sr`/`ProxyClient_Fetch3sr` write-as-you-go
pattern, unchanged). Decisive third reason: the S1 tracker patches the
header's `frame_count`/`checksum_count` IN PLACE at finalize
(`runner-track-3sr.patch` `Track3srFinalizeGame` `fseek(20)`/`fseek(26)`) and
appends the checksum table only at finalize, so pure forward concatenation of
a growing file can never equal the final file — polling re-sends the 28-byte
header (the only mutated region) in a separate `header_b64` field every poll
while the body (offset ≥ 28) is append-only, making the reconstruction
byte-identical to the completed `game_N.3sr`. `watch`-op code:
`tools/fcade-proxy/fcade-proxy.js` `requestWatch()` + `WATCH_HEADER_BYTES`/
`WATCH_CHUNK_BYTES` (48 KiB raw → 64 KiB base64, under the 256 KiB device cap
`proxy_client.c:46`); wire/README contract in `tools/fcade-proxy/README.md`
"watch (live stream relay, Stage S3)"; test `tools/fcade-proxy/__test_watchpoll.js`
(byte-identity via sha256/cmp, first-byte latency, one-job-per-quark,
late-attach, clean mid-watch failure). A long-lived framed stream (shape (a))
remains a future latency optimization that would not change the byte contract.

---

## Stage S4 — Device streaming player

**Title:** `ReplayPlayer` learns to play a file that is still growing.

**Create/modify:** a streaming input source in
`src/replay/replay_player.c`: accept a setup header without the final
`frame_count` (or with a growing on-disk `.3sr` written by the fetch
worker), relax the exact-size check (`:459-464`) for streaming sessions,
start the phase machine on setup arrival, gate `PHASE_GAME` entry on a
start buffer (config key, default ~5 s of words), and on buffer underrun
**stall the injection tick** (hold the frame, overlay "BUFFERING…")
instead of `finish("inputs-exhausted")` (`:960-962`). On `game_end`,
finalize the file into a normal `.3sr` (+ `.meta.json` from the catalog
row) so it joins the LOCAL list. Fetch side: extend the
`ProxyClient_Fetch3sr` worker pattern (`proxy_client.h:188-198`) with a
`watch` consumer.

**Success criteria:** desktop: watching a live-converted quark starts
within the §3.A latency budget and completes checksum-clean; killing the
proxy mid-match produces a clean abort overlay, never a hang or crash;
a completed watch's file byte-matches the server's `.3sr`.

**Dependencies:** S3; S0a's fidelity verdict.

**What NOT to do:** no seek/rewind; no attempt to hide a genuine desync
(hard rule: wrong data never ships silently); don't touch netplay
guards (`:819-824`).

**Failure/fallback:** if in-place engine stalling proves disruptive,
raise the start buffer until underruns are unobservable (at 6× arrival
the buffer only grows after start; underrun implies a server stall, which
should surface anyway).

---

## Stage S5 — OSD/wrapper WATCH wiring

**Title:** REMOTE tab row action: WATCH NOW.

**Create/modify:** wrapper handoff `--play-replay-live <quarkid>`
(clone the two-argv injection template,
`plan-osd-replay-browser.md` §1.5 / `thirdsarm_wrapper.cpp:2819-2843`);
game-side arg (mutually exclusive with `--play-replay`/netplay flags,
same `args.c` guard pattern `:165-200`); OSD row states (WATCH /
CONVERTING / READY / FAILED) driven by `convertstatus`.

**Success criteria:** TV test — select a never-converted catalog quark,
watch it start in seconds-scale; the OSD reflects job state truthfully.

**Dependencies:** S4.

**Failure/fallback:** the S2 (Option B) flow stays as the row's behavior
if live handoff needs another iteration.

---

## Stage S6 — Hardening

Multi-game sessions (tracker segments games; player auto-advances or
returns to browser between games — v1 player is one-game-per-file by
design, `replay_player.c:801-807`); desync UX copy; concurrent-viewer
cap + idle teardown of ggpo pulls; VPS store eviction for stream-cache
files; probe-script politeness limits documented; wrapper log lines for
diagnosis.

---

## 5. Risk register

| # | Risk | Evidence / status | Mitigation |
|---|---|---|---|
| 1 | FBNeo won't build or is too slow on the 2-vCPU aarch64 VPS | UNVERIFIED; Linux lib line stale (`makefile.sdl:144-146`) | S0b gate before any dependent work; fallback = Option B on Mac rail, or user re-hosts |
| 2 | Stream pacing varies (old quarks, server load, long sessions) | MEASURED-ONCE 6.18× (§1.3); historical notes consistent | S0a re-measures 3+ quarks incl. long/old; design only needs ≥ 1× |
| 3 | No pre-play statcheck → mid-watch desync | 1/14 corpus divergence (7.1%, notes §8.5); 5/5 clean newer batch (notes §12) | Checkpoint detector retained (§1.5): abort with overlay, ≤ 60-frame detection; honest UX copy; optional future Linux-engine statcheck running ahead of the viewer |
| 4 | −13-word fidelity beyond one quark (only if raw relay is chosen) | B3 positive = 1 quark, arcade-mode harness (§1.4) | Recommended design relays RAM-observed words — no exposure; S0a quantifies anyway |
| 5 | Pre-game prefix length unknown → first-frame latency estimate soft | UNVERIFIED distribution; B3 anchor sweep hints ~450-512 frames for one quark | S0a measures; worst case adds prefix/6.2 seconds |
| 6 | Device buffer underrun mid-match | Arrival 6× > consumption 1×; underrun ⇒ server stall | Start buffer + stall overlay (S4); at 6× the buffer strictly grows after start |
| 7 | GGPO server behavior changes (pacing, handshake, concurrency caps, expiry) | Protocol is reverse-engineered; 2022 quarks still streamed in 2026 (notes §4) | Tool IS the spec (`fcade_stream.c:3-7`); keep the Python reference tool as canary; typed errors end-to-end |
| 8 | VPS concurrency (N viewers × FBNeo on 2 vCPU) | 3.8 GB RAM / 2 vCPU measured | Hard cap (1-2 sessions) + attach-don't-fork for same-quark viewers (S3) |
| 9 | Long-lived proxy streams vs. existing lock-step client assumptions | `proxy_client` is one-request-one-response today (`proxy_client.h:8-10`) | S3 keeps framing identical; fallback chunked polling |
| 10 | Politeness/ToS posture toward Fightcade infra | One-connection-at-a-time precedent (notes §4); catalog already rate-limited (stealth-catalog plan §1.2) | Session caps, idle teardown, no unattended loops in spikes |

---

## 6. Open questions

1. **Prefix length distribution** (first input record → game_0
   signature) — dominates the first-frame budget. S0a measures.
2. **Does ggpo permit two concurrent connections to one quark?** Only
   relevant to rejected Option C and to multi-viewer attach semantics;
   currently avoided by design (server-side single pull + fan-out).
3. **Does the 6.18× rate hold for 15-20-min and 2022-vintage quarks?**
   MEASURED-ONCE. S0a.
4. **Is the engine buildable on Linux aarch64** (for an optional
   server-side statcheck running ahead of the viewer)? UNVERIFIED; not
   on the critical path.
5. **Multi-game watch UX** — auto-advance vs. return-to-browser between
   games of one session (v1 player is single-game by design).
6. **Fast-forward** — inputs-only playback could run > 60 fps up to the
   buffered head (ARM statcheck ran headless at ~145 fps, notes §9.3),
   but rendering/audio-coupled fast-forward is unexplored. Out of scope
   for v1; noted because the 6× buffer makes it *possible* later.

---

## 7. Summary of verdicts

- **Crux:** playback = 12-byte setup (one FBNeo-reached frame) + input
  words + offset. FBNeo must consume the whole session server-side but
  only as a real-time in-process tracker; playback starts as soon as the
  first game's signature frame arrives (~seconds). **Live streaming is
  feasible.** (§0)
- **Recommended:** Option A ("3SR-as-a-stream" relay) staged through
  Option B (convert-on-select VOD), sharing S0-S2. First frame ~15-30 s
  (A) vs ~duration/6 (B) vs ~3.5 s/min-of-match today's full-wait.
- **First cheap spike:** S0a (Mac, repo tools only — prefix-equivalence
  + shipped-player fidelity + pacing/prefix measurement) alongside S0b
  (VPS FBNeo build + speed gate). Either failing reshapes the plan
  before any protocol or engine work starts.
- **Char-select:** **no matching effort needed, ever** — FBNeo resolves
  it server-side into the setup block; the shipped phase machine
  reproduces it engine-natively (§2).
- **Honest costs:** watching is 1× real-time (no skim in v1); no
  pre-play divergence guarantee for live streams (~7% historical rate,
  detected-not-prevented at ≤ 60 frames); FBNeo-on-VPS is the single
  gating unknown, and the VPS is **aarch64**, not x86_64.
