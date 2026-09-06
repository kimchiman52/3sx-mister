# CPS3 static disassembly and anchoring

`cps3.py` is the tracked instrument for reading the arcade (CPS3) program.
Before it existed, every arcade-balance lane re-derived SH-2 disassembly by hand
with a throwaway Capstone script in its own scratchpad, and the only surviving
trace was prose in `docs/research-arcade-*.md`. The addresses were re-established
from scratch each time, and the traps below were each paid for at least once.

## Run it

```sh
python3 tools/cps3-disasm/cps3.py selftest          # reproduces documented addresses
python3 tools/cps3-disasm/cps3.py find --u8-list "80,90,50,50,50"
python3 tools/cps3-disasm/cps3.py pin --at 0x061A38C0
python3 tools/cps3-disasm/cps3.py refs 0x061A3890
python3 tools/cps3-disasm/cps3.py dis 0x060C4DF4 --count 16
python3 tools/cps3-disasm/cps3.py fn 0x060C5308
python3 tools/cps3-disasm/cps3.py table 0x061A38C0 --entries 16 \
        --index-by 0x061A3890 --index-entries 21 --index-width 2
python3 tools/cps3-disasm/cps3.py nocall --fn 0x060C2E8C \
        --callee 0x0611DFB8 --control 0x0611E0EE
```

`selftest` is the assertion this README does not restate in prose: every value it
checks is quoted from `docs/research-arcade-balance-desyncs.md` or
`docs/research-arcade-cg-data-accuracy.md`, and it fails loudly if the tool, the
image, or a documented address stops agreeing.

## Dependencies

- **Python 3** and **capstone >= 5** (SH support landed in capstone 5). Present on
  this machine: `/usr/bin/python3` 3.9.6 with capstone 5.0.7.
- **The decrypted image**, `tools/arcade-audit/rom.bin` — 8 MiB, big-endian,
  mapped at `0x06000000`, md5 `909f5abec4b6b21bf7d2a452a03fdfcc`. It is
  **gitignored**, so a fresh clone or a fresh worktree does not have it:

  ```sh
  python3 tools/arcade-audit/decrypt.py      # ~3 s
  ```

  That needs `sfiii3nr1.zip` (`~/Library/Application Support/CrowdedStreet/3S-ARM/resources/`
  by default; override with `ARCADE_AUDIT_ROMZIP`). Point the tool at a different
  image with `--rom` or `ARCADE_AUDIT_ROM`; `--verify-rom` fails unless the md5
  matches.

No Ghidra, no JVM, no project database, no server. Every subcommand is a
sub-second read of a flat file.

## The four traps this encodes

Each was paid for in lane time. They are the reason the tool prints what it
prints, including the parts that look like nagging.

1. **Anchor on a table that occurs EXACTLY ONCE, find its SOLE literal referrer,
   and read the address off the instruction.** That is the house method
   (`docs/research-arcade-cg-data-accuracy.md`, section 23.4). `find` reports the
   occurrence count and says out loud when a pattern is not unique; `pin` refuses
   to continue when it is not.

2. **Pin routines by TABLE, not by position.** `win_jp_tbl` at `0x061A38C0` has
   one literal referrer, inside `win_player` at `0x060C2DDC`, and is indexed by
   `winner_type_tbl` at `0x061A3890` — so `[0]..[15]` genuinely *are*
   `Win_00000..Win_15000`, and `[13]` genuinely is `Win_13000` at `0x060C4D22`.
   Nothing about that came from counting entries or from the order the routines
   appear in the image. `table --index-by` does this composition.

3. **A resolved-call census UNDER-COUNTS.** `bonus_game_win_pause`
   (`0x060C5308`) makes four calls to `set_field_hosei_flag`; the fourth, at
   `0x060C5384`, is a `jsr @r11` whose `r11` was loaded 90 bytes earlier at
   `0x060C532A`, and a register-tracking call graph resolved only three of the
   four. **Absence from a census proves nothing.** `fn` prints `UNRESOLVED` rows
   rather than dropping them, and prints the lower-bound warning every time. To
   prove a routine does *not* call something, use `nocall`, which runs the three
   legs E7 used — aligned-word scan over the routine's range, `bsr` reach, and
   the resolved census — and demands a **positive control**, a callee known to be
   present, because a literal scan with no control measures nothing.

   Two ways a literal scan lies, both printed by the tool when relevant:
   SH-2 `mov.l @(disp,PC)` reaches 255 longwords **forward and never backward**,
   so a routine with no pool of its own borrows the **next** routine's (`fn`
   labels those `borrowed`); and a target within ±255 of a pool literal is
   reached as base + displacement with no literal of its own. The literal scan is
   a **screen**; the disassembly is the verdict.

4. **Byte-identical opening lines are a coincidence of shape, not behaviour.**
   In the port, `Normal_normal_Winner`'s first ten lines are byte-identical to
   `Win_01000`'s (`src/sf33rd/Source/Game/animation/win_pl.c`, the "standing
   worry" comment), and that shape cost a lane real time on the assumption that
   the arcade's `Normal_normal_Winner` would therefore share `Win_01000`'s
   missing `set_field_hosei_flag` pair. It does not: arcade `0x060C37BA` calls
   the pair at `0x060C37E8`/`0x060C380A`, while arcade `Win_01000` (`0x060C2E8C`)
   has no `0x0611DFB8` word in 1,318 bytes. Both directions are in `selftest`,
   and the two arcade routines do not even open alike. Shape is not evidence:
   identity comes from the anchor; the enclosing-routine address
   `refs` prints is an `rts`-scan **hint**, and the extent `fn` reports is a
   heuristic — pass `--end` when the real bound is known.

## Why this is not Ghidra (measured, 2026-09-06)

Upstream shipped a Ghidra bridge and a `compare-cps3-code` skill on top of it in
`68a3eeaf` ("Add a skill for CPS3 code comparison", PR #258) —
`tools/ghidra/ghidra_query.py` + `GhidraQuery.py`, driving `pyghidraRun` headless
against a `.gpr` named by `GHIDRA_PROJECT` / `GHIDRA_PROGRAM`. Those files are in
our history but were never in our tree. Recovering them was tried and rejected,
on measurement rather than preference:

- **The bridge needs a CPS3 Ghidra project, and there is none here.** The only
  `.gpr` on this machine is an unrelated game's. The upstream skill's own text
  ("ask the user to label the relevant symbols in Ghidra") says what it is really
  built on: a **hand-curated, labelled** database. That is a reverse-engineering
  asset, not a tool, and porting the wrapper does not port it.
- **Creating one from our image does not substitute.** `analyzeHeadless` imported
  `rom.bin` as `SuperH:BE:32:SH-2` at base `0x06000000` and auto-analysed it in
  **5m49s**. Result: **1,894** functions, **0** with a non-default name, **0**
  user-defined symbols, and only **13 of the 30** routine entry points our docs
  have already established exist as functions at all. `Win_13000`
  (`0x060C4D22`) — the E9a routine — is not among them, because it is reachable
  only through a jump table copied to the stack and dispatched by `jsr @r1`, which
  is exactly the case flow analysis cannot follow and `table --index-by` can.
- **The bridge as written could not run here anyway**: `pyghidraRun` prompts
  interactively to install PyGhidra and dies on EOF, and `java` is not on `PATH`
  (a JDK exists at `/opt/homebrew/opt/openjdk@21`, unlinked).

None of that says Ghidra is bad. It says the value in upstream's setup is the
labelled project, we do not have one, and a Ghidra dependency would buy us a
slower, heavier path to a strictly worse answer than a flat-file Capstone scan.
If a curated CPS3 project ever exists here, recover the bridge with
`git show 68a3eeaf:tools/ghidra/ghidra_query.py` and
`git show 68a3eeaf:tools/ghidra/GhidraQuery.py`; the two tools answer different
questions and can coexist.

## What this tool does not do

- No decompiler. It gives disassembly, literals, tables and call structure — not
  C. Reading behaviour out of it is the lane's job.
- No symbol names. Names in this repo's arcade docs are *our* names for arcade
  routines, established by anchor. The image has none.
- No writing. It never touches `src/`.
