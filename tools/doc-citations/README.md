# Documentation citation linter

Checks that the citations written in this repo's prose still point at something
that exists: `file.c:123` line citations, backticked symbol names, and
referenced paths, in `docs/**.md`, the top-level `*.md`, and the comment text
inside `src/`, `include/` and `tools/`.

It checks whether a claim's evidence is **reachable**, not whether the claim is
**true**. Only the first is mechanical.

```sh
python3 tools/doc-citations/check_doc_citations.py              # whole tree
python3 tools/doc-citations/check_doc_citations.py docs/        # one subtree
python3 tools/doc-citations/check_doc_citations.py --errors-only
python3 tools/doc-citations/check_doc_citations.py --json
python3 tools/doc-citations/test_doc_citations.py               # acceptance test
```

Exit codes: `0` no errors, `1` errors, `2` harness failure. The last line is
always a machine-greppable `DOCCITE SUMMARY:` verdict.

**This file states no finding counts.** A README that says "there are 412
findings" is a documentation defect waiting to happen — exactly the thing this
tool exists to catch. Run the tool; the number comes from the summary line.

## Finding codes

Severity is set by **measured precision**, not by how bad the failure sounds.
Every figure below comes from hand-auditing sampled findings against the actual
files; the sample size is given so you can weigh it.

| code | severity | precision | meaning |
| --- | --- | --- | --- |
| `degenerate-target` | error | **9/9** | the cited line is blank, or only a brace. Nobody cites those on purpose. |
| `drift` | error | 23/40 | file and line exist, but the line no longer contains the symbol the citation is about. Carries the line where the symbol actually is. |
| `wrong-path` | error† | 5/9 | the path has never existed, but a file with that basename does — carries the suggestion. |
| `line-out-of-range` | error | 4/9 | the file has fewer lines than the citation claims. |
| `unanchored-citation` | error | 20/20 | a citation into an `anchor-required.txt` file that names nothing checkable. Precision is 20/20 by construction: it reports the ABSENCE of a claim, so it cannot be wrong about one — but see the caveat below. |
| `phantom-path` | advisory | **0/9** | no commit on any ref has ever contained this path. |
| `phantom-identifier` | advisory | **1/9** | a backticked symbol absent from all current code *and* never present **as code** in any historical revision of any source file on any ref. The evidence line says which of those was searched, and says so when the only historical mention was inside a comment. |
| `stale-path` | advisory | — | the path existed and was deleted. A historical record, not a fabrication. |
| `stale-identifier` | advisory | — | the symbol existed as code and is gone. Names where it went: `removed from src/netplay/direct_p2p.c in 2c63adc7`, following renames and merge conflict resolutions. Advisory for the same reason `stale-path` is: it describes removed code, which is a record, not a defect. |
| `unresolvable-evidence` | advisory | — | "see the *X* report" where no path appears anywhere in the sentence. |

† demoted to advisory in RECORD-class documents — see `record-documents.txt`.

**`phantom-path` and `phantom-identifier` are advisory because they measured
0/9 and 1/9.** They stay switched on — `phantom-identifier` is the check that
catches the `kill_texcash_work` class, and that class is worth finding — but a
category that is wrong four times out of five must not be able to fail a build.
Their residue is not a scoping bug awaiting one more rule: it is documents whose
citations are relative to a different worktree, informal scratchpad artifacts,
external tool and NEON intrinsic names, and numbered-family placeholders like
`nm_NNNNN`. Those are things this tree genuinely cannot resolve, and none of
them is a defect. Promote them only together with a fresh measurement.

`line-out-of-range` measuring 4/9 was the surprise — a citation into a file that
is demonstrably too short still looks certain, but most of the misses are docs
that cite a *sibling worktree*. That is why the table exists.

## The three ideas that make it quiet enough to use

**1. Never-existed is a defect; existed-and-was-deleted is not.** A plan
document citing a file the renderer rip-out deleted is a historical record.
A brief citing `tools/mister/build-deps.sh`, a path no commit has ever
contained, is a fabrication. The linter tells them apart by reading git
history, and only the second is an error. This is what took the finding count
from unusable to triageable.

**2. Drift is proven, never inferred.** There are no maintained fingerprints —
a hash a human has to update would rot exactly the way the comments rotted.
Instead the linter takes identifier tokens the author already wrote next to the
citation and asks whether any appears on the cited line. When it cannot find
the token anywhere in the file it stays **silent**: absence of proof is not a
finding. It reports drift only when it can exhibit the token elsewhere in the
same file, so every drift finding carries its own evidence and a one-line fix.

**3. Exoneration is generous, accusation is strict.** A citation is cleared if
*any* symbol from the surrounding prose lands on the cited line; it is accused
only using the tokens nearest to it. Sentences carry several citations, and
letting one borrow its neighbour's subject was the largest false-positive
source measured.

## Configuration

Three files, each requiring a written reason per entry:

- `allowlist.txt` — identifiers that are legitimately not symbols of this repo.
- `record-documents.txt` — globs for proposals and superseded write-ups, where
  naming a not-yet-existing thing is the document's job.
- `anchor-required.txt` — globs for files where a bare `file:LINE` is an ERROR
  rather than silently accepted, because the file is a dense table whose line
  numbers move in bulk. Listed files also contribute their own macro arguments
  as anchor candidates, so short member names can anchor at all.
- `external-paths.txt` — paths that deliberately point at another repository or
  another machine, which no amount of checking this tree can resolve.

Keep all three short. Every entry is a place the linter has been told to stop
looking.

## Known limits

- **Drift precision is 23/40** on this tree, from two independent hand audits
  (110 findings were audited in total across five audits; the earlier 70 scored
  38/70 against a version before the anchor-ownership fix). That is a triage
  tool, not a gate. Do not run `--fix` over drift findings unattended; read the
  evidence line, which is printed precisely so you can judge in one glance.
- **`degenerate-target` is the only check that measured perfect** (9/9). If you
  want one thing wired into CI, it is that one.
- **A citation with no symbol near it cannot be drift-checked.** `texcash.c:301/516/554`
  in a sentence whose subject is `be` gets its line numbers range-checked and
  nothing more. This is asserted as a known gap by the acceptance test so that
  it cannot be quietly forgotten.
- **A name that only ever lived in a comment is a phantom**, and the evidence
  says so ("history names it only inside comments (src/x.c)"). That is the
  honest verdict: nothing a reader could have called ever existed. Until
  2026-09 the opposite limit held -- a symbol deleted from a file that still
  existed was reported as "has never existed" -- and that text was false for
  65 of 65 findings on one document. See "The history corpus" below.
- **The history walk is bounded** by `HISTORY_STRIP_BUDGET`. Past it a
  stale finding says "the removing commit was not located" instead of naming
  one; it never falls back to calling the symbol a phantom. On this tree the
  full scan uses under 100 MB of the budget.
- **History is consulted only for `HISTORY_EXTS`** (C, C++, Python, shell,
  JavaScript). A symbol that existed only in, say, a `.tcl` file is a phantom.
- **Cross-repository citations are indistinguishable** from local ones and must
  be listed in `external-paths.txt` by hand.
- `vendor/` and `src/imgui/` are not scanned — third-party line numbering we do
  not control — but both remain resolvable *targets*.

## Testing

`test_doc_citations.py` runs the linter against two fixtures: the six citation
defects found by hand on 2026-08-24 (all must be caught) and a set of correct
citations of every shape (none may be reported). Both halves matter — a checker
that reports everything catches all six too.

The test was confirmed able to fail by mutation: neutering the history corpus,
inverting the drift comparison, and making the path resolver accept anything
each turn it red.

`check_history_tells_stale_from_phantom()` builds a throwaway repository of
its own -- a symbol deleted from a surviving file, one deleted with its file,
one that never existed, one that only ever appeared in a comment, one still
alive -- and asserts each verdict, that the stale findings name the path and
the removing commit, and that a second run served from the on-disk cache says
the same thing. Case 8 of the known-bad fixture covers the same defect against
real history (`RELAY_REQ`, removed by `2c63adc7`).

## The history corpus

`stale-*` versus `phantom-*` rests on what git history has ever contained, and
identifiers are answered in two stages because exactness and speed pull apart.

**Stage 1, the index.** Every blob any revision on any ref ever had at a
`HISTORY_EXTS` path is tokenised raw -- comments kept -- into a per-path token
set. This answers "has this name ever appeared in that file, in any form?"
completely and cheaply. It used to skip every path still in the tree, which
halved the work but made the checker claim "has never existed" about every
symbol deleted from a file that survived it. On this tree the index is 12.6k
blobs (345 MB) and takes ~4.4 s to build against ~2.5 s for the old half,
which is why it is cached: `doc-citations-history.pickle` (5 MB) in the git
common dir, shared by every worktree, keyed on a digest of the raw log it was
built from. Any new commit on any ref rebuilds it; an unchanged history loads
in ~0.05 s. `--no-history-cache` rebuilds and does not write.

**Stage 2, the walk.** Only for a name a document cites and the current code
lacks. Each candidate path's revisions are walked newest-first, decommented on
demand, to find the commit whose diff took the name out of that file *as
code*. That is what lets a finding say `removed from src/netplay/direct_p2p.c
in 2c63adc7`, and what keeps a name that only ever lived in a comment from
being called real. Decommenting is the whole cost (~11 MB/s, char by char),
so blob → stripped-token-set is memoised in `doc-citations-history-blobs.pickle`
beside the index; a blob's content never changes, so that memo never goes
stale and is merely capped.

Two git defaults would make the walk lie and are overridden. `git log --raw`
detects renames, so a moved file prints one entry naming both paths; read
naively the old path appears to live on with the new file's content, and a
symbol really deleted from the new path years later gets blamed on the
restructure that moved it. Rename entries are split so the walk follows the
move. And `git log` prints no diff for a merge commit, so a symbol dropped
during conflict resolution -- `menu.c` lost `check_netplay_cancelled` in
merge `9dd530bf` -- had no removing entry at all. `--full-history
--diff-merges=first-parent` makes merges print their diff against the line of
development they landed on.

**Negative result, so nobody retries it:** decommenting every historical blob
up front, to make stage 1 exact on its own, measured 30 s on this tree. The
raw index plus on-demand walk is the design because of that number.

Measured on this tree at 7f4f0d1a, full scan: 13.7 s before; 26.8 s with
both caches cold; 18.3 s warm. `docs/plan-netplay-connection.md` alone:
5.6 s before, 4.0 s warm. The 11-scope baseline gate, which runs the linter
eleven times and so paid the old ~2.5 s corpus build eleven times: 61 s
before, 40 s warm.

## Baselining

The tree carries a large pre-existing backlog, so a gate must fail only on
*new* findings:

```sh
python3 tools/doc-citations/check_doc_citations.py --write-baseline b.json
python3 tools/doc-citations/check_doc_citations.py --baseline b.json   # exit 1 only on new errors
```

The baseline is a derived artifact and is **not** committed (`.gitignore`) —
a checked-in list of known-bad citations would itself go stale, which is the
failure this tool exists to catch. Generate it in CI at the pinned commit.

## The anchor model, and what it still cannot see (task #110)

A citation is checked by finding an **anchor** — a symbol named in the prose
around it — and asking whether the cited line mentions that symbol. Everything
follows from that, including the limits.

A citation that names no symbol has no anchor, so there is nothing to test, so
it is accepted. Forever. That silence is deliberate for most of the tree:
demanding an anchor everywhere would bury the real findings. But it fails badly
in one shape — a long mechanical table where every insertion moves everything
below it. `src/netplay/game_state.c` is 609 `GS_SAVE`/`GS_LOAD` pairs on the
rollback save/load path, and a wrong line number there sends a desync
investigation to the wrong member. Files listed in `anchor-required.txt` are
therefore held to a higher standard: cite something checkable, or be reported.

Two supporting details matter more than they look:

- **Short members can now anchor.** The generic rule needs a token of at least
  `MIN_IDENT_LEN` characters containing an underscore. 80 of game_state.c's 609
  members are shorter than that — `bg_w`, `M_Lv`, `VS_Index` — so requiring an
  anchor without this would demand something the checker had already decided it
  could not see. Anchor-required files supply a vocabulary derived from their
  own macro CALL sites (see `macro_vocabulary`).
- **`#define` lines are excluded from that vocabulary**, because a macro
  definition names formal parameters, not members. Admitting them let the
  English word "row" — from `EFFL8_ROW_IN_RANGE(row)` — outrank a correctly
  backticked `ca_check_flag` and produce a confident, wrong drift report.

**The caveat on `unanchored-citation`'s precision.** It reports that a citation
makes no checkable claim, which is a fact about the prose, not a guess about
the code. What it cannot tell you is whether the citation is *also* wrong. Of
the 20 this class started at, measurement showed most were **never right** —
authored against a working tree that a later squash removed, so they were
internally consistent when written and match no commit in this repository.
Repointing such a number would manufacture a citation nobody ever verified, so
they are reported and left alone. `--fix` does not touch this code, and cannot:
there is no anchor to derive a target from.

Nineteen of them survive, and as of task #117 they are no longer under a
ceiling: every one lives in a historical research or plan document, and holding
those at an exact count made each unrelated lane re-audit citations that
describe a tree their own commit replaced. The rule itself is still enforced —
`baselines.txt` holds `src` at zero `unanchored-citation` findings, and the
eight document ceilings are all-codes and so forbid the shape too. What was
dropped is the archive, not the check.

## `docs/archive/` is not scanned

Task #117 released archived documents from the ceilings. It did not stop the
linter reading them, so they went on producing `drift` findings that were
repaired by hand in commits whose entire subject was "repoint N citations" —
and repairing one means editing a historical record so that it describes a tree
it was not written against, which makes the record worse, not better.

So `docs/archive/` is in `SKIP_SCAN_DIRS`. An archived document carries a
header naming the commit it was true at; its line numbers are correct about
that commit and are not claims about now. This is strictly stronger than the
RECORD class in `record-documents.txt`, which only demotes *existence* findings
to advisory and leaves `drift` and `line-out-of-range` as errors — those two
being exactly the backlog the archive accumulated.

The exclusion is asserted, not asked for. `check_archive_is_not_scanned()` in
`test_doc_citations.py` plants the same file twice in a throwaway worktree,
once inside `docs/archive/` and once outside it, and requires silence on the
first **and** a finding on the second. The positive control is the point: a
linter that has stopped scanning anything at all is also silent, and without
the second probe this assertion would pass for that reason. It also plants the
probes as *tracked* files, because `scan_targets` reads `git ls-files` — an
untracked probe would be skipped whether or not the exclusion existed, and the
test would prove nothing.

Verified by mutation rather than assumed: deleting `"docs/archive/"` from the
tuple makes the test report three findings on the inside probe (`drift`,
`line-out-of-range`, `phantom-identifier`) and fail.
