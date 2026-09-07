# How arcade-accuracy work is actually done here — and whose method it is

This file exists because a session in 2026-09 got the answer wrong and acted on
it. It is provenance and method, not a description of code: nothing here is a
fact a test could hold, which is why it is prose.

## The short version

**Almost the entire method is upstream's** (`crowded-street/3sx`). This fork
supplies corpus scale, two instruments of its own, and a much wider audit scope. Anyone who describes our
approach as methodologically distinct from upstream's is repeating the mistake
this file records.

## What is upstream's

| thing | evidence |
|---|---|
| `ArcadeBalance_IsEnabled()`, the gate every arcade fix hangs on | introduced by Artem Pstygo in "Balance fixes (#196)" 2026-03-26, "Waza syncing (#197)", "Arcade: Decompile pls00 (#205)" |
| **Statcheck**, the replay oracle | `4a86558e` "Restore Statcheck (#247)" 2026-04-24 — *restore*, so it predates that; then `52a395bd` "Statcheck: Test tooling (#268)", `f54cfc55` "Add a short article about Statcheck (#355)" |
| the replay pipeline: `tools/fcade-replays` -> `fbneo-replay-runner` -> SCRD -> per-frame compare | `docs/statcheck.md` as added by `f54cfc55`. **We do not have that file**; read it with `git show f54cfc55:docs/statcheck.md` |
| the SCRD container and its zero-run XOR encoding | same |
| our `src/test/statcheck_compare.c` | the commit that wrote it says so verbatim: "Port upstream's statcheck test_runner/test_runner_compare logic into fork idioms", using "upstream's verbatim bit conversion" |

### "Decompile X" upstream does not mean "add a new function"

`2a0793b7` "CPS3: decompile plcnt (#253)" modifies `plcnt.c`, a file that has
existed since well before that pass (added by upstream in `d61591be`, 2025-10-07,
and *renamed* into `engine/` on 2025-10-16 -- the rename date is not the birth
date), and adds **11 `ArcadeBalance_IsEnabled()` gates
inside it**. Upstream reads the CPS3 disassembly and gates the differences
inside existing PS2-decompiled functions — the same thing this fork does, at a
coarser granularity (a routine at a time rather than a divergence at a time).

## What is actually ours

- **`tools/arcade-audit/`** (`4f1e9397`, 2026-08-29). **Correction, 2026-09-07:**
  an earlier draft of this file claimed upstream has "no audit-family commits".
  That is false and was the most load-bearing error in it. Upstream ships
  `tools/compare_char_data.py` -- "Binary comparison tool (#51)", Artem Pstygo,
  2025-10-13 -- and this tree carries it byte-identical; `#283` (2026-07-20)
  later adds `analyze_rendering_bindings`. Static auditing is **not** this
  fork's invention. What `arcade-audit` adds is **scope**: a 133,901-cell census
  across all 20 characters with a reachability model, where upstream's tool
  compares binaries. Read the difference as scope, not existence. It remains a
  different instrument, not a Statcheck
  variant: it compares ROM data tables against our adapted tables **statically**,
  over all 133,901 cells, whether or not any replay touches them. The
  wrong-sprite cells, and the Elena / Dudley / X.C.O.P.Y. reachability
  questions, were all found here — none of them would surface in a replay.
- **`tools/cps3-disasm/`** (`c18615d3`, 2026-09-06). Built because upstream's
  Ghidra bridge (#258) did not transfer: its value lived in a hand-labelled
  Ghidra database that is not in the commit. See that tool's README.
- **Corpus scale and the conversion fleet** — operational, not methodological.

## The blind spot, which is shared

Statcheck compares RAM state. It therefore cannot see:

1. **Fields it does not compare.** Proven, not assumed: the E9 lane ran a
   control with the `set_field_hosei_flag` pair removed entirely and the corpus
   returned **447/447, identical to the treatment** — see
   `research-arcade-balance-desyncs.md` §E9. A passing corpus said nothing
   about that class either way.
2. **Data-table correctness.** An in-bounds wrong sprite passes every replay.
   That is the gap `tools/arcade-audit/` exists to cover.

**Upstream has this blind spot identically** — it is a property of the shared
oracle, not a difference between the forks. Growing the corpus does not shrink
it. The complementary axis is *runtime differential vs static audit*, and the
static side is the part this fork added.

## Practical consequence

"The replays all pass" is a bounded claim. Before treating it as evidence, check
that `statcheck_compare.c` actually compares the field your change affects.

## Provenance caveats found by review (2026-09-07)

This file was written to correct a misconception and shipped with four of its
own. An adversarial review caught them; they are fixed above, and recorded here
because the pattern matters more than the facts.

- **"Upstream has no audit-family commits"** was false -- see the correction
  above. I checked for an `arcade-audit`-shaped directory and concluded from its
  absence, instead of searching for the capability.
- **`plcnt.c` "has existed since 2025-10-16"** took a `R097` rename for a birth.
  `--diff-filter=A` on the new path reports the rename; the original add is
  earlier and under a different path.
- **`a6940977` is not an ancestor of HEAD.** It lives on
  `feat/fcade-replay-browser`; `statcheck_compare.c` reaches this branch via
  `cdbf5678`. Citing a commit by SHA is not the same as checking it is *in* the
  history you are describing -- especially in a repo built from omnibus squashes,
  where the same content lands under a different SHA.
- The summary said **"one genuinely new instrument"** while the section beneath
  it listed two.

The substantive claim -- that `statcheck_compare.c` is a port of upstream's
`test_runner_compare.c` -- was independently verified and stands: same private
helpers in the same order, public entries renamed.
