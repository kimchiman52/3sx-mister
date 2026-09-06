#!/usr/bin/env python3
"""Acceptance test for check_doc_citations.py.

WHAT THIS ASSERTS
-----------------
That the linter still catches the citation defects that were found BY HAND on
2026-08-24, one per case in testdata/known-bad-citations.md. Those six are the
reason the tool exists; a change that stops catching one of them has removed
the only capability that was ever demonstrated.

The test is written so that it FAILS if the linter goes quiet. It asserts, for
each fixture case, that a finding of a specific code lands on a specific line.
It is not a smoke test: deleting any single check, or breaking the anchor
logic, the history corpus, or the path resolver, turns it red. That was
verified by mutation rather than assumed -- see the run recorded in the task
#77 write-up, where inverting the drift comparison and neutering the history
corpus were each confirmed to produce failures.

There is also a NEGATIVE fixture. A checker that reports everything catches all
six too, and would be worthless. testdata/good-citations.md contains correct
citations of every shape the linter understands, and the test requires ZERO
findings on it. Recall and silence are asserted together.

Run:  python3 tools/doc-citations/test_doc_citations.py
Exit: 0 all assertions hold; 1 an assertion failed; 2 harness failure.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
CHECKER = os.path.join(SCRIPT_DIR, "check_doc_citations.py")

# Imported, not copied. The anchor fixture below has to know the cap at which
# the checker gives up on an anchor, and a second hardcoded copy of it would
# drift silently -- which is the same class of bug that rotted this test's
# line numbers. The checker guards main() behind __name__, so importing it
# only binds definitions.
sys.path.insert(0, SCRIPT_DIR)
from check_doc_citations import ANCHOR_MAX_HITS  # noqa: E402
BAD = "tools/doc-citations/testdata/known-bad-citations.md"
GOOD = "tools/doc-citations/testdata/good-citations.md"

# Each entry: (case label, finding code, substring that must appear in the
# finding's message). Line numbers are deliberately NOT asserted -- editing a
# comment in the fixture would then break the test for no reason, which is the
# kind of brittleness that gets tests deleted.
MUST_CATCH = [
    ("1 symbol that never existed",
     "phantom-identifier", "kill_texcash_work"),
    ("2 evidence cited to a document that never existed",
     "unresolvable-evidence", "S7 task report"),
    ("3 line citation that drifted (effl8.c:11 -> :13)",
     "drift", "effl8.c:11"),
    ("5 purge ordering cited at a lone closing brace",
     "drift", "texgroup.c:440-443"),
    ("6 path that never existed at that location",
     "wrong-path", "tools/mister/build-deps.sh"),
    # Task #110. The blind spot anchor-required.txt closes: a bare line number
    # into a macro table, anchored to nothing. If this stops being caught, the
    # ~186 game_state.c citations that motivated the check are silently
    # accepted again.
    ("7 bare line citation into an anchor-required file",
     "unanchored-citation", "src/netplay/game_state.c:580"),
    # A symbol deleted from a file that STILL EXISTS. Until the history corpus
    # stopped skipping current paths, this came back as phantom-identifier
    # with the text "has never existed" -- for 35 real names in one document,
    # all removed by 2c63adc7 -- and that confidently-wrong text sent a lane
    # to rewrite a document that was correct. The needle is the commit: the
    # finding must not merely say "gone", it must say where.
    ("8 symbol deleted from a file that still exists (RELAY_REQ, 2c63adc7)",
     "stale-identifier", "2c63adc7"),
]

# Case 4 is a documented RECALL GAP, asserted as such so that it cannot be
# quietly forgotten. Its prose ("... texcash.c:301/516/554 and PPGWork.c:177
# all skip on be == 0") names no symbol the identifier scoping admits -- its
# real subject is `be`, two characters -- and none of its three target lines is
# blank or a brace. The linter range-checks all three numbers and stays silent
# about drift, which is the designed behaviour: no anchor, no accusation.
# If a future change gives this case a finding, that is an improvement, and
# this assertion is what will tell you it happened.
KNOWN_GAP = ("4 multi-number citation with no symbol anchor",
             "texcash.c:301")

# Case 5's SECOND citation, `ramcnt.c:102`, is a documented CORRECT citation,
# not a defect -- it is the exact scenario anchor_tokens' "Asymmetric on
# purpose" docstring describes by name (same shape, real file texgroup.c:287):
# the nearest token to the citation is `Push_ramcnt_key_original_2`, but the
# cited line really is the `purge_texture_group` re-entry, so the citation is
# correct and must stay SILENT.
#
# Before the per-line anchor-window fix (task #84), that exoneration could not
# reach it: `group_num`, the token that proves the cited line is right, sits
# one line above the citation, and the anchor window was exactly one line
# wide. Verified by reverting the window widening alone (anchor_tokens'
# markdown branch, MD_ANCHOR_WINDOW_LINES) and rerunning this fixture: the
# same-line-only window produces a spurious `drift` finding here suggesting
# `ramcnt.c:102 -> ramcnt.c:93`, i.e. it recommends "fixing" a citation that
# was never broken. That is the "mis-paired" defect and the "one citation
# needed a manual reflow" cost described for task #84 -- reflowing the prose
# onto one line was the only way to get the checker to agree it was correct.
NEGATIVE_CASES = [
    ("5b ramcnt.c:102 exonerated by an anchor one line above the citation",
     "ramcnt.c:102"),
]


def run(*args):
    proc = subprocess.run(
        [sys.executable, CHECKER, "--json", "--include-testdata"] + list(args),
        cwd=REPO_ROOT, capture_output=True, text=True)
    if proc.returncode not in (0, 1):
        sys.stderr.write("checker failed (rc=%d):\n%s\n"
                         % (proc.returncode, proc.stderr))
        raise SystemExit(2)
    return json.loads(proc.stdout)["findings"]


def check_fix_refuses_range_citations():
    """Regression for task #84: `--fix` corrupting range citations.

    Observed bug: `--fix` rewrote only the START of a `file.c:LO-HI` drift
    finding (the anchor only ever locates one line) and left the OLD END in
    place, producing a backwards range pointing nowhere -- `522-566` became
    `2360-566` in the wild, across six citations in one file, silently.

    This reproduces the exact mechanism using the SAME drifted citations as
    known-bad-citations.md cases 3 and 5 (`effl8.c:11` and the range
    `texgroup.c:440-443`), copied into a throwaway linked git worktree so
    `--fix`'s in-place edit never has to touch a tracked file. Confirmed by
    hand before the fix landed: running `--fix` on the real fixture rewrote
    `texgroup.c:440-443` to `texgroup.c:483-443` (start moved, end frozen --
    start > end).

    The fixed behaviour: `--fix` refuses the range citation outright (leaves
    the line byte-for-byte unchanged) while still repairing the ordinary
    single-line drift in the same run, proving the refusal is targeted and
    not a case of --fix having stopped working altogether.
    """
    wt_parent = tempfile.mkdtemp(prefix="doccite-fix-test-")
    wt_dir = os.path.join(wt_parent, "wt")
    failures = []
    try:
        subprocess.run(
            ["git", "worktree", "add", "--detach", wt_dir, "HEAD"],
            cwd=REPO_ROOT, capture_output=True, text=True, check=True)

        fixture_rel = "tools/doc-citations/testdata/fix-range-regression.md"
        fixture_path = os.path.join(wt_dir, fixture_rel)
        content = (
            "# Fixture: --fix must refuse range citations (task #84)\n\n"
            "NOT A REAL DOCUMENT. Reuses the exact drifted citations from "
            "known-bad-citations.md cases 3 and 5 in a throwaway worktree, so "
            "this test never writes to a tracked fixture.\n\n"
            "| Symbol | Declared |\n"
            "| --- | --- |\n"
            "| `spmv_ng_save[2]` (u32) | "
            "`sf33rd/Source/Game/effect/effl8.c:11` |\n\n"
            "The nested purge is bounded: purge_texture_group clears ok\n"
            "before calling Push_ramcnt_key (texgroup.c:440-443), so the\n"
            "purge_texture_group(group_num) re-entry inside\n"
            "Push_ramcnt_key_original_2 (ramcnt.c:102) sees ok == 0.\n"
        )
        with open(fixture_path, "w", encoding="utf-8") as fh:
            fh.write(content)
        subprocess.run(["git", "add", fixture_rel], cwd=wt_dir, check=True,
                       capture_output=True)

        proc = subprocess.run(
            [sys.executable, CHECKER, "--root", wt_dir, "--fix",
             "--include-testdata", fixture_rel],
            cwd=wt_dir, capture_output=True, text=True)
        if proc.returncode not in (0, 1):
            sys.stderr.write("checker (--fix) failed (rc=%d):\n%s\n"
                             % (proc.returncode, proc.stderr))
            raise SystemExit(2)

        with open(fixture_path, encoding="utf-8") as fh:
            fixed = fh.read()

        if "texgroup.c:440-443" not in fixed:
            # Generic on purpose: does not assume the corrupted start is any
            # particular number (e.g. the "483" observed by hand), so this
            # still catches the bug even if the real anchor's line moves.
            failures.append(
                "RANGE CITATION WAS ALTERED: --fix must leave "
                "`texgroup.c:440-443` byte-for-byte untouched (refuse, don't "
                "guess); file now reads: %r" % fixed)
        if "effl8.c:13" not in fixed:
            failures.append(
                "--fix did not repair the ordinary single-line drift "
                "(effl8.c:11 -> :13) in the same run -- refusal of the range "
                "citation should not disable fixing elsewhere: %r" % fixed)
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", wt_dir],
                       cwd=REPO_ROOT, capture_output=True)
    return failures


RE_TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

# The file both anchor cases are built against. A real, heavily-commented
# source file is the point: the defect only appears where a symbol is
# discussed far more often than it is defined.
ANCHOR_TARGET = "src/netplay/direct_p2p.c"

# Symbols, not line numbers. See check_fix_anchors_to_definition_not_mention.
ANCHOR_SYM_COMMENT = "ORCH_JOIN_WORST_CASE_MS"
ANCHOR_SYM_STRING = "race_budget_expired"


def looks_like_prose_line(text):
    """True if `text` is a comment continuation, a comment opener, or a bare
    string-literal line.

    Deliberately a crude textual predicate and deliberately NOT the checker's
    own strip_c_comments/blank_strings machinery. Reusing the code under test
    here would make the central assertion pass vacuously the moment that
    machinery broke -- which is the exact failure this test exists to catch.
    """
    s = text.lstrip()
    return (s.startswith("*") or s.startswith("/*") or s.startswith("//")
            or s.startswith('"'))


def build_anchor_case(lines, symbol, trap_is_string, forbidden_tokens):
    """Locate a (trap, cite, code_lines) triple for `symbol` in `lines`.

    trap  -- a line that only MENTIONS the symbol, in a comment or inside a
             string literal. Chosen as the mention furthest from any real
             code line, so the trap is unambiguous.
    cite  -- a stale line number strictly NEARER to `trap` than to any real
             code line. That is the shape that fools a nearest-line anchor
             search into "repairing" a citation onto the mention.

    Computed rather than hardcoded. The previous version of this test pinned
    six literal line numbers into direct_p2p.c; a few merges moved that file
    ~250 lines and every one of them silently stopped describing what it
    claimed to, so the test failed for reasons that had nothing to do with
    the behaviour under test. Symbols are stable, line numbers are not.

    Returns None if `symbol` cannot host the case (no mention, no code use,
    or more occurrences than ANCHOR_MAX_HITS, at which point the checker
    discards the anchor entirely and there is nothing to assert).
    """
    pat = re.compile(r"\b%s\b" % re.escape(symbol))
    hits = [i for i, ln in enumerate(lines, 1) if pat.search(ln)]
    if not hits or len(hits) > ANCHOR_MAX_HITS:
        return None
    code = [h for h in hits if not looks_like_prose_line(lines[h - 1])]
    if trap_is_string:
        traps = [h for h in hits if lines[h - 1].lstrip().startswith('"')]
    else:
        traps = [h for h in hits
                 if looks_like_prose_line(lines[h - 1])
                 and not lines[h - 1].lstrip().startswith('"')]
    if not code or not traps:
        return None
    trap = max(traps, key=lambda t: min(abs(t - c) for c in code))
    margin = min(abs(trap - c) for c in code)
    for delta in range(2, max(3, margin // 2)):
        for cite in (trap + delta, trap - delta):
            if not 1 <= cite <= len(lines) or cite in hits:
                continue
            if abs(cite - trap) >= min(abs(cite - c) for c in code):
                continue
            body = lines[cite - 1]
            # Substantive, so the degenerate-target path (blank lines, a lone
            # brace) does not pre-empt the anchor path being tested...
            if len(body.strip()) < 12:
                continue
            # ...and sharing no token with the fixture prose, so the checker's
            # "the cited line names something the prose is discussing" escape
            # hatch does not silence the finding either.
            if set(RE_TOKEN.findall(body)) & forbidden_tokens:
                continue
            return trap, cite, code
    return None


def check_fix_anchors_to_definition_not_mention():
    """Regression for task #89's third proven `--fix` defect (#84 fixed the
    other two: corrupted ranges, and inventing repairs for already-correct
    citations), plus the string-literal half of the same defect.

    Observed bug: the anchor-hit search did not distinguish a MENTION of a
    symbol from its actual CODE definition/usage. It took every line
    containing the token and picked whichever was numerically nearest to the
    citation's stale line number. A symbol discussed in a long
    algorithm-description comment block is mentioned many times close
    together, so a citation whose true target had drifted far away could get
    "fixed" onto a nearby mention instead of its real definition. Observed in
    the wild, against the tree as it stood at task #89: `--fix` wanted to
    collapse 5 distinct citations onto `direct_p2p.c:1719` and 2 onto  # doccite:quote
    `direct_p2p.c:1472` -- both inside prose. Those two are a QUOTE of a  # doccite:quote
    historical measurement, not live citations; direct_p2p.c has moved
    several hundred lines since and they are not meant to track it.

    Two mention KINDS, one per case, because they were fixed at different
    times and each can regress without the other:

    - CASE A, comment mention. Fixed by task #89, which taught the anchor
      search to prefer lines that survive comment stripping.
    - CASE B, string literal. The checker's code-only line view (see
      Repo.code_only_lines) stripped comments but not string literals, so a
      symbol named inside a _Static_assert message still counted as code and
      could outrank its own definition. Measured
      before the fix, on the tree at merge 77d8175d: `--fix` moved the
      `race_budget_expired` citation onto direct_p2p.c:1519, the second  # doccite:quote
      line of a _Static_assert message, over the function's own
      definition. After the fix it resolves to the definition. That line
      number is a QUOTE of that one measurement and is not maintained.

    Both cases run against the REAL, unmodified direct_p2p.c, copied into a
    throwaway detached git worktree so --fix's in-place edit never touches a
    tracked file.

    WHAT IS ASSERTED, and why it is not circular: not a line NUMBER, but the
    CONTENT of whatever line --fix chose. The chosen line must name the
    symbol, must not be the mention we baited it with, and must not look like
    a comment or string line under looks_like_prose_line -- a predicate this
    file owns and the checker never sees.
    """
    wt_parent = tempfile.mkdtemp(prefix="doccite-anchor-test-")
    wt_dir = os.path.join(wt_parent, "wt")
    failures = []
    try:
        subprocess.run(
            ["git", "worktree", "add", "--detach", wt_dir, "HEAD"],
            cwd=REPO_ROOT, capture_output=True, text=True, check=True)

        with open(os.path.join(wt_dir, ANCHOR_TARGET), encoding="utf-8") as fh:
            tlines = fh.read().split("\n")

        header = (
            "# Fixture: --fix must anchor to a definition, not a mention\n\n"
            "NOT A REAL DOCUMENT. Built by test_doc_citations.py.\n\n")
        forbidden = set(RE_TOKEN.findall(header)) | {
            ANCHOR_SYM_COMMENT, ANCHOR_SYM_STRING, "Case", "direct_p2p", "c"}

        cases = []
        for label, symbol, is_string in (
                ("A", ANCHOR_SYM_COMMENT, False),
                ("B", ANCHOR_SYM_STRING, True)):
            built = build_anchor_case(tlines, symbol, is_string, forbidden)
            if built is None:
                sys.stderr.write(
                    "harness: case %s cannot be built against %s for `%s` -- "
                    "the symbol no longer has both a %s mention and a code "
                    "use under ANCHOR_MAX_HITS. Pick another symbol; do not "
                    "delete the case.\n"
                    % (label, ANCHOR_TARGET, symbol,
                       "string-literal" if is_string else "comment"))
                raise SystemExit(2)
            trap, cite, code = built
            cases.append((label, symbol, trap, cite, code))

        fixture_rel = "tools/doc-citations/testdata/fix-anchor-mention-regression.md"
        fixture_path = os.path.join(wt_dir, fixture_rel)
        content = header + "".join(
            "Case %s: `%s` (direct_p2p.c:%d).\n\n" % (label, symbol, cite)
            for label, symbol, _trap, cite, _code in cases)
        with open(fixture_path, "w", encoding="utf-8") as fh:
            fh.write(content)
        subprocess.run(["git", "add", fixture_rel], cwd=wt_dir, check=True,
                       capture_output=True)

        proc = subprocess.run(
            [sys.executable, CHECKER, "--root", wt_dir, "--fix",
             "--include-testdata", fixture_rel],
            cwd=wt_dir, capture_output=True, text=True)
        if proc.returncode not in (0, 1):
            sys.stderr.write("checker (--fix) failed (rc=%d):\n%s\n"
                             % (proc.returncode, proc.stderr))
            raise SystemExit(2)

        with open(fixture_path, encoding="utf-8") as fh:
            fixed = fh.read()

        for label, symbol, trap, cite, code in cases:
            row = [ln for ln in fixed.split("\n") if symbol in ln]
            m = re.search(r"direct_p2p\.c:(\d+)", row[0]) if row else None
            if m is None:
                failures.append(
                    "case %s (`%s`): the fixture line lost its citation "
                    "entirely; file now reads: %r" % (label, symbol, fixed))
                continue
            got = int(m.group(1))
            if got == cite:
                failures.append(
                    "case %s (`%s`): --fix left the stale citation at :%d "
                    "unrepaired (real code lines are %s)"
                    % (label, symbol, cite, code))
            elif got == trap:
                failures.append(
                    "ANCHORED TO A MENTION: case %s (`%s`) was repaired onto "
                    ":%d, which is the %s mention it was baited with, not a "
                    "definition or use -- %r"
                    % (label, symbol, trap,
                       "string-literal" if label == "B" else "comment",
                       tlines[got - 1].strip()))
            elif not re.search(r"\b%s\b" % re.escape(symbol),
                               tlines[got - 1]):
                failures.append(
                    "case %s (`%s`): --fix chose :%d, a line that does not "
                    "name the symbol at all -- %r"
                    % (label, symbol, got, tlines[got - 1].strip()))
            elif looks_like_prose_line(tlines[got - 1]):
                failures.append(
                    "ANCHORED TO A MENTION: case %s (`%s`) was repaired onto "
                    ":%d, which is a comment or string-literal line -- %r"
                    % (label, symbol, got, tlines[got - 1].strip()))
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", wt_dir],
                       cwd=REPO_ROOT, capture_output=True)
    return failures


def check_archive_is_not_scanned():
    """docs/archive/ must be invisible to the linter, and only docs/archive/.

    WHY THE EXCLUSION EXISTS. An archived document is stamped with the commit
    it was true at and is not maintained after that. Its line numbers do not
    rot into wrongness; they stay correct about a tree that moved on. Scanning
    it turned that into a standing `drift` backlog whose only discharge was
    rewriting a historical record to match the present -- work that produced
    "repoint N citations" commits and no reader-visible benefit.

    WHY THIS IS A TEST AND NOT A COMMENT. "docs/archive/ is excluded" is a
    claim that can be checked by running something, so it is checked by
    running something. A silent linter is also what a BROKEN linter looks
    like, so silence on its own proves nothing: the same file, byte for byte,
    is planted twice -- once inside docs/archive/ and once outside it -- and
    the test demands silence on the first AND a finding on the second. Without
    the positive control this assertion would still pass if scan_targets()
    stopped returning anything at all.

    Both probes live in a throwaway linked worktree (scan_targets reads `git
    ls-files`, so an untracked file proves nothing -- it would be skipped
    whether or not the exclusion exists, and the test would be vacuous).
    """
    wt_parent = tempfile.mkdtemp(prefix="doccite-archive-test-")
    wt_dir = os.path.join(wt_parent, "wt")
    failures = []
    # Two independently-flagged defects, so the positive control does not rest
    # on one finding code: a drifted anchored citation (the same one as
    # known-bad case 3) and a line number past the end of a real file.
    probe = (
        "# Fixture: docs/archive/ is not scanned\n\n"
        "NOT A REAL DOCUMENT. Planted twice by check_archive_is_not_scanned "
        "in a throwaway worktree.\n\n"
        "| Symbol | Declared |\n"
        "| --- | --- |\n"
        "| `spmv_ng_save[2]` (u32) | "
        "`sf33rd/Source/Game/effect/effl8.c:11` |\n\n"
        "The `SKIP_SCAN_DIRS` tuple is at "
        "tools/doc-citations/check_doc_citations.py:99999999.\n"
    )
    inside_rel = "docs/archive/zz-exclusion-probe.md"
    outside_rel = "docs/zz-exclusion-probe.md"
    try:
        subprocess.run(
            ["git", "worktree", "add", "--detach", wt_dir, "HEAD"],
            cwd=REPO_ROOT, capture_output=True, text=True, check=True)
        for rel in (inside_rel, outside_rel):
            full = os.path.join(wt_dir, rel)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8") as fh:
                fh.write(probe)
        subprocess.run(["git", "add", inside_rel, outside_rel], cwd=wt_dir,
                       check=True, capture_output=True)

        proc = subprocess.run(
            [sys.executable, CHECKER, "--root", wt_dir, "--json",
             inside_rel, outside_rel],
            cwd=wt_dir, capture_output=True, text=True)
        if proc.returncode not in (0, 1):
            sys.stderr.write("checker failed (rc=%d):\n%s\n"
                             % (proc.returncode, proc.stderr))
            raise SystemExit(2)
        findings = json.loads(proc.stdout)["findings"]

        inside = [f for f in findings if f["path"] == inside_rel]
        outside = [f for f in findings if f["path"] == outside_rel]

        if inside:
            failures.append(
                "ARCHIVE WAS SCANNED: %s produced %d finding(s) -- "
                "docs/archive/ must be in SKIP_SCAN_DIRS; got %s"
                % (inside_rel, len(inside),
                   [(f["code"], f["message"]) for f in inside]))
        if not outside:
            failures.append(
                "POSITIVE CONTROL FAILED: the identical probe at %s produced "
                "no finding, so the silence on %s proves nothing about the "
                "archive exclusion -- the linter is not looking at either one"
                % (outside_rel, inside_rel))
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", wt_dir],
                       cwd=REPO_ROOT, capture_output=True)
    return failures


def check_history_tells_stale_from_phantom():
    """The history corpus must answer "did this name ever exist as code?"
    for EVERY path, including paths still in the tree.

    WHY. History.tokens() used to index historical blobs only for paths that
    were no longer tracked (`if p in current: continue`), as a cost saving.
    The consequence was a factually false diagnostic: any symbol deleted from
    a file that survived it was reported as `phantom-identifier` with the
    evidence "absent ... from every historical revision of every source
    file". On docs/plan-netplay-connection.md that text was wrong 65 times
    out of 65, and it caused a real misdiagnosis -- the document was read as
    an unbuilt design whose names never shipped.

    WHAT IS ASSERTED, in a throwaway repository built here so the verdicts
    depend on nothing but the mechanism:

      stale_widget_count   defined in the fixture's widget source, deleted by
                           a later commit while that file lived on
                           -> stale-identifier, and the message names BOTH
                           the path and the commit
      gone_file_widget     deleted together with its file -> stale-identifier
                           naming that file and commit (the case the old corpus
                           already handled; it must not regress)
      never_widget_count   in no revision of anything     -> phantom-identifier
                           with the "every historical revision" evidence
      comment_only_widget  named in a comment in one historical revision,
                           never as code                  -> phantom-identifier,
                           and the evidence says the mention was in a comment
                           (the old raw corpus would have called this stale)
      live_widget_count    still defined                  -> silence

    The checker is run twice: the second run must reproduce the first, which
    is what proves the on-disk history cache (a pickle in the repo's git
    common dir, keyed on the raw log) returns the same corpus it stored.
    """
    tmp = tempfile.mkdtemp(prefix="doccite-history-test-")
    failures = []
    git = ["git", "-C", tmp, "-c", "user.name=doccite",
           "-c", "user.email=doccite@example.invalid",
           "-c", "commit.gpgsign=false"]

    def sh(*args):
        subprocess.run(git + list(args), check=True, capture_output=True)

    def put(rel, text):
        full = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as fh:
            fh.write(text)

    try:
        sh("init", "-q")
        put("src/widget.c",
            "int stale_widget_count = 0;\n"
            "/* comment_only_widget: named here, defined nowhere */\n"
            "int live_widget_count = 1;\n")
        put("src/gone_file.c", "int gone_file_widget = 2;\n")
        sh("add", "-A")
        sh("commit", "-q", "-m", "add widgets")
        put("src/widget.c", "int live_widget_count = 1;\n")
        os.unlink(os.path.join(tmp, "src/gone_file.c"))
        sh("add", "-A")
        sh("commit", "-q", "-m", "drop stale_widget_count and gone_file.c")
        removing = subprocess.run(git + ["rev-parse", "HEAD"], check=True,
                                  capture_output=True, text=True).stdout.strip()
        put("docs/widget-notes.md",
            "# Widget notes\n\n"
            "NOT A REAL DOCUMENT. Built by test_doc_citations.py.\n\n"
            "The counter `stale_widget_count` is reset by `never_widget_count` "
            "after `comment_only_widget` runs; `gone_file_widget` went with "
            "its file, and `live_widget_count` is the survivor.\n")
        sh("add", "-A")
        sh("commit", "-q", "-m", "notes")

        runs = []
        for _ in range(2):
            proc = subprocess.run(
                [sys.executable, CHECKER, "--root", tmp, "--json",
                 "docs/widget-notes.md"],
                cwd=tmp, capture_output=True, text=True)
            if proc.returncode not in (0, 1):
                sys.stderr.write("checker failed (rc=%d):\n%s\n"
                                 % (proc.returncode, proc.stderr))
                raise SystemExit(2)
            runs.append(json.loads(proc.stdout)["findings"])
        findings = runs[0]
        if runs[0] != runs[1]:
            failures.append(
                "HISTORY CACHE CHANGED THE VERDICT: a second run over the same "
                "repository (served from the cached corpus) produced different "
                "findings -- first %s, second %s"
                % ([(f["code"], f["message"]) for f in runs[0]],
                   [(f["code"], f["message"]) for f in runs[1]]))

        def about(sym):
            return [f for f in findings
                    if f["code"] in ("stale-identifier", "phantom-identifier")
                    and ("`%s`" % sym) in f["message"]]

        for sym, path in (("stale_widget_count", "src/widget.c"),
                          ("gone_file_widget", "src/gone_file.c")):
            hits = about(sym)
            if not hits:
                failures.append("NOT CAUGHT: `%s` produced no finding" % sym)
                continue
            f = hits[0]
            if f["code"] != "stale-identifier":
                failures.append(
                    "MISCLASSIFIED: `%s` existed as code in %s and was deleted, "
                    "but is reported as %s: %r -- the checker is claiming a "
                    "real symbol never existed"
                    % (sym, path, f["code"], f["message"]))
                continue
            if "`%s`" % path not in f["message"]:
                failures.append(
                    "NO PATH: the stale finding for `%s` must name `%s`, the "
                    "file it was removed from: %r" % (sym, path, f["message"]))
            if removing[:8] not in f["message"]:
                failures.append(
                    "NO COMMIT: the stale finding for `%s` must name %s, the "
                    "commit that removed it: %r"
                    % (sym, removing[:8], f["message"]))

        hits = about("never_widget_count")
        if not hits or hits[0]["code"] != "phantom-identifier":
            failures.append(
                "`never_widget_count` never existed anywhere and must stay "
                "phantom-identifier; got %s"
                % [(f["code"], f["message"]) for f in hits])
        elif "every historical revision" not in " ".join(hits[0]["evidence"]):
            failures.append(
                "the phantom evidence for `never_widget_count` no longer says "
                "history was searched: %r" % hits[0]["evidence"])

        hits = about("comment_only_widget")
        if not hits or hits[0]["code"] != "phantom-identifier":
            failures.append(
                "`comment_only_widget` was only ever a word in a comment and "
                "must be phantom-identifier, not a symbol that 'existed'; got "
                "%s" % [(f["code"], f["message"]) for f in hits])
        elif "comment" not in " ".join(hits[0]["evidence"]):
            failures.append(
                "the phantom evidence for `comment_only_widget` should say the "
                "historical mention was inside a comment: %r"
                % hits[0]["evidence"])

        if about("live_widget_count"):
            failures.append(
                "SPURIOUS: `live_widget_count` is still defined and produced %s"
                % [(f["code"], f["message"]) for f in about("live_widget_count")])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return failures


def main():
    failures = []

    findings = run(BAD)
    for label, code, needle in MUST_CATCH:
        hit = [f for f in findings
               if f["code"] == code and needle in f["message"]]
        if not hit:
            failures.append(
                "NOT CAUGHT: case %s -- expected a %s finding mentioning %r; "
                "got %s" % (label, code, needle,
                            sorted({f["code"] for f in findings}) or "nothing"))

    gap_label, gap_needle = KNOWN_GAP
    if any(gap_needle in f["message"] for f in findings):
        failures.append(
            "KNOWN GAP CLOSED: case %s now produces a finding. That is good "
            "news -- promote it into MUST_CATCH and delete this assertion."
            % gap_label)

    for label, needle in NEGATIVE_CASES:
        hit = [f for f in findings if needle in f["message"]]
        if hit:
            failures.append(
                "SPURIOUS FINDING: case %s -- expected no finding mentioning "
                "%r, got %s" % (label, needle,
                                [(f["code"], f["message"]) for f in hit]))

    failures.extend(check_fix_refuses_range_citations())
    failures.extend(check_fix_anchors_to_definition_not_mention())
    failures.extend(check_archive_is_not_scanned())
    failures.extend(check_history_tells_stale_from_phantom())

    good = run(GOOD)
    for f in good:
        failures.append(
            "FALSE POSITIVE on the good fixture: %s:%d [%s] %s"
            % (f["path"], f["line"], f["code"], f["message"]))

    for line in failures:
        print("FAIL: " + line)
    print("DOCCITE TEST: must_catch=%d/%d good_fixture_findings=%d failures=%d"
          % (len(MUST_CATCH) - sum(1 for x in failures
                                   if x.startswith("NOT CAUGHT")),
             len(MUST_CATCH), len(good), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
