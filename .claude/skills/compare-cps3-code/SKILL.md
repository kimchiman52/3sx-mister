---
name: compare-cps3-code
description: Read the arcade (CPS3) program and compare it with this port's C, using tools/cps3-disasm/cps3.py over the decrypted SH-2 image. Use whenever a task needs a CPS3 address, needs to know whether the arcade makes a call the port makes (or does not), needs a routine pinned to an address, or asks to implement CPS3-matching behaviour behind the arcade-balance flag. Do NOT re-derive SH-2 disassembly by hand.
---

# Compare CPS3 code

The arcade program is an 8 MiB big-endian SH-2 image mapped at `0x06000000`.
`tools/cps3-disasm/cps3.py` is the tracked instrument for reading it. Do not
write a throwaway Capstone script; every lane that did burned hours
re-establishing addresses this tool now reproduces in a second.

Read `tools/cps3-disasm/README.md` once before the first query in a session. It
holds the operator detail — how to rebuild the gitignored image, what the
dependencies are, and why this is not Ghidra.

## First: is the image there?

```sh
python3 tools/cps3-disasm/cps3.py selftest
```

`tools/arcade-audit/rom.bin` is **gitignored**, so a fresh worktree does not have
it. `python3 tools/arcade-audit/decrypt.py` rebuilds it in about 3 seconds from
`sfiii3nr1.zip`. `selftest` also proves the tool still agrees with every address
the docs already established; if it fails, stop and say so — do not work around
it.

## The method, in the order to run it

**Never pin a routine by position, by name, or by the shape of its opening
lines.** The only admissible pin is: a unique data anchor, its sole literal
referrer, and the address read off the instruction.

1. **Anchor.** Find a table or byte string that occurs **exactly once**.

   ```sh
   python3 tools/cps3-disasm/cps3.py find --u8-list "80,90,50,50,50"
   python3 tools/cps3-disasm/cps3.py find --s16-list "..." --align 2
   python3 tools/cps3-disasm/cps3.py find --u32 0x0611DFB8 --align 4
   ```

   Not unique means not an anchor. Widen the pattern (more of the table) until it
   is, or pick a different table.

2. **Pin.** One command runs uniqueness, sole-referrer and the read-off:

   ```sh
   python3 tools/cps3-disasm/cps3.py pin --u8-list "80,90,50,50,50"
   python3 tools/cps3-disasm/cps3.py pin --at 0x061A38C0     # anchor address known
   ```

   More than one literal referrer is not a pin. Say so rather than picking one.

3. **Pin the family by TABLE, not by position.** A jump table plus the type table
   that indexes it is what makes `[13]` *be* `Win_13000`:

   ```sh
   python3 tools/cps3-disasm/cps3.py table 0x061A38C0 --entries 16 \
           --index-by 0x061A3890 --index-entries 21 --index-width 2
   ```

   Check the index element width against the disassembly (a `shll` before the
   load means 16-bit entries, `shll2` means 32-bit) — guessing it silently
   produces a plausible, wrong table.

4. **Read it.**

   ```sh
   python3 tools/cps3-disasm/cps3.py dis 0x060C4DF4 --count 16
   python3 tools/cps3-disasm/cps3.py fn 0x060C5308 --disasm
   ```

   `dis` annotates every pc-relative load with the value it loads, which is where
   masks, work-RAM addresses and table bases come from.

## Proving the arcade does NOT do something

This is the class of claim that has produced this repo's real findings, and the
class that is easiest to get wrong.

```sh
python3 tools/cps3-disasm/cps3.py nocall --fn 0x060C2E8C \
        --callee 0x0611DFB8 --control 0x0611E0EE
```

**A resolved-call census is a LOWER BOUND. Absence from it proves nothing.**
`bonus_game_win_pause` calls `set_field_hosei_flag` four times; the fourth
(`0x060C5384`) is a `jsr @r11` loaded 90 bytes earlier, and a register-tracking
call graph saw only three. `fn` prints `UNRESOLVED` rows for exactly this reason
— never read past them.

A negative needs all of:

- **a positive control** — a callee known to be present must show up in the same
  scan, or the scan is measuring nothing;
- **the `bsr` reach check** — `bsr` spans ±0x1000, so a distant callee needs a
  literal, but a near one does not;
- **the disassembly read** — the literal scan is a **screen**, not the verdict. A
  routine with no pool of its own borrows the **next** routine's (`mov.l
  @(disp,PC)` reaches 255 longwords forward and never backward), and a target
  within ±255 of a pool literal is reached as base + displacement with no literal
  at all.

State a negative as "no literal, `bsr` out of reach, control present, disassembly
read over N bytes" — never as "it is not in the call list".

## Reporting a finding

Cite the **durable anchor**, not a line number: the anchor table and its address,
the sole referrer, the routine address, and the instruction addresses the claim
rests on. That is the form `docs/research-arcade-balance-desyncs.md` uses and the
form the next lane can re-run.

Say plainly which of these a claim rests on, because they have different
standing: disassembly alone, disassembly plus a corpus verdict that moved, or
disassembly plus a corpus that could not adjudicate either way.

## Implementing CPS3 behaviour

When a divergence is real and the fix is safe, gate it — the port's PS2 behaviour
must stay bit-identical when the flag is off:

```c
#include "arcade/arcade_balance.h"

if (ArcadeBalance_IsEnabled()) {
    /* CPS3 behaviour */
} else {
    /* existing PS2 behaviour */
}
```

Prefer the narrowest conditional around the actual difference over duplicating a
function. If CPS3 *omits* something the port does, guard the port-only behaviour
with `!ArcadeBalance_IsEnabled()`.

`src/sf33rd/`, `src/arcade/`, `tools/arcade-audit/*.py` and
`docs/research-arcade-*.md` are frequently owned by a concurrent lane — check
before editing them.

## What this skill is not

It is not a decompiler. There is no C, and there are no symbol names in the
image — every arcade routine name in this repo's docs is *our* name, established
by anchor. Upstream's Ghidra-based `compare-cps3-code` (`68a3eeaf`) is a
different instrument that needs a labelled Ghidra project this repo does not
have; `tools/cps3-disasm/README.md` records what was measured and why it was not
adopted.
