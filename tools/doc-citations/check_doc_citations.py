#!/usr/bin/env python3
"""Documentation citation linter (task #77).

WHAT IT PROVES
--------------
That the citations written in this repo's prose -- markdown under docs/ and
comment text inside src/, include/, tools/ -- still point at something that
exists. It does not check whether a claim is TRUE; it checks whether the
evidence a claim points at is REACHABLE. Those are different failures, and
only the second one is mechanical.

Four checks, in descending order of how much harm the failure caused when it
was found by hand:

  C1  file:LINE citations       -- the file exists, the line exists, and the
                                   line still contains what the citation says
                                   it contains (see DRIFT below).
  C2  backticked identifiers    -- a `snake_case` symbol named in prose exists  # doccite:quote
                                   somewhere in the tree's CODE, not merely in
                                   the prose asserting it.
  C3  referenced paths          -- a path mentioned in prose resolves to a
                                   tracked file.
  C4  evidence references       -- "see the <Name> report" style appeals to a
                                   named artifact that has no resolvable path
                                   anywhere in the sentence. Advisory only.

DRIFT, AND WHY THERE ARE NO FINGERPRINTS
----------------------------------------
The interesting C1 failure is not the dangling citation, it is the citation
whose file and line both still exist but now describe something unrelated --
because a later patch inserted lines above it. `effl8.c:11` was cited for  # doccite:quote
`spmv_ng_save` in three places; the symbol is at :13 and line 11 is
`#include <assert.h>`. Both file and line resolve. Nothing dangles.

The obvious fix is a fingerprint: write `effl8.c:13#a3f2` and check the hash.
This linter deliberately does NOT do that, for one reason: a hash a human has
to maintain will rot exactly the way the comments rotted. Every legitimate
line move would demand a doc edit AND a recomputed digest, and the first time
someone is in a hurry they will drop the suffix rather than recompute it. A
scheme whose failure mode is "people stop using it" cannot be the check that
catches people not keeping things up to date.

Instead this uses ANCHOR TOKENS, which authors already write for free. Real
citations name their subject next to the pointer -- "`spmv_ng_save[2]` (u32) |
`.../effl8.c:11`". So: pull identifier-shaped tokens out of the citation's own  # doccite:quote
context, and ask whether any of them appears on the cited line.

The rule that makes this quiet enough to trust is when it stays SILENT:

  anchor found on the cited line        -> OK.
  no anchor found anywhere in the file  -> SILENT. The citation may point at
                                           an unnamed region (a loop body, a
                                           blank line, a brace) and we have no
                                           evidence either way. Absence of
                                           proof is not a finding.
  anchor found ELSEWHERE in the file    -> DRIFT, reported with the line number
                                           where the anchor actually is.

So a drift finding is never an inference. It always carries its own proof:
here is the token, here is the line you cited, here is the line it is on. That
also makes it a one-line fix, which is the point -- see --fix.

An anchor that occurs more than ANCHOR_MAX_HITS times in the file is discarded
as evidence: it is too common to localise anything.

IDENTIFIER NOISE SCOPING (C2)
-----------------------------
`kill_texcash_work` was asserted by a comment in texcash.c and existed nowhere  # doccite:quote
else in the tree. Catching that class means checking backticked prose tokens
against reality, and prose backticks hold a great deal that is not a symbol:
flags, paths, shell fragments, RFC terms, prose emphasis. An identifier check
that fires on those is a check nobody reads.

Scoping, each rule chosen to remove a specific noise source:

  * must fully match [A-Za-z_][A-Za-z0-9_]*   -- removes shell fragments,
    flags (--fast), paths (a/b.c), and anything with punctuation or spaces.  # doccite:quote
  * must contain at least one '_'             -- removes ordinary English
    words, RFC/protocol nouns, and single-word emphasis. snake_case and
    SCREAMING_CASE are near-conclusive evidence of "C symbol in this repo".
  * must be at least MIN_IDENT_LEN chars      -- removes short accidents.
  * `foo()`, `foo[2]`, `*foo`, `a.b`, `a->b`  -- unwrapped first, so call and
    field syntax is checked rather than skipped.

and then the existence test itself, which is the part that actually matters:

  the token must appear as a whole word in the CODE corpus, where the code
  corpus is built with COMMENTS STRIPPED.

Stripping comments is not an optimisation, it is the whole check. A symbol
that only a comment claims exists is precisely the defect; if comment text
counted as evidence of existence, `kill_texcash_work` would have been  # doccite:quote
self-certifying. Comment stripping is a real lexer pass (see strip_comments),
not a regex, because a regex gets // inside a string literal wrong.

Unavoidable residue -- env vars, external API names, planned-but-unbuilt
symbols in plan documents -- goes in allowlist.txt with a reason per entry,
the same convention tools/rollback-determinism/allowlist.txt uses.

SCOPE
-----
Scanned for citations: docs/**.md, top-level *.md, and comment text in
src/**, include/**, tools/** ({.c,.h,.cpp,.hpp,.py,.sh,.js}).

An extension missing from that set does not read as "clean" -- it reads as
findings=0, which is indistinguishable from "checked and fine" unless you also
look at files_scanned. .js was absent until task #119's follow-up, so
tools/rendezvous-server (the production rendezvous server) had never been
checked at all. Known still-unscanned, each carrying real citations in `#`
comments: .yaml/.yml (~125 citation-shaped tokens, mostly tools/frame-data
corpora; already in CODE_CORPUS_EXTS and DECOMMENTABLE_EXTS, so only
CODE_PROSE_EXTS is missing them) and CMakeLists.txt (4; .txt is in
CODE_CORPUS_EXTS but has no decommenter, so it needs one first). Surveyed and
found to carry NO citations, so nothing is being missed there: .sv, .v, .vhd,
.json, .tsv, .tcl, .qsf, .sdc, .inc, .cmake, .service. No .mjs, .ts or .cjs
file is tracked in this repo.

NOT scanned: vendor/. It is third-party-derived; its comments cite an upstream
line numbering we neither control nor may edit, so every finding there would be
unactionable -- the definition of noise. vendor/ IS included in the resolution
corpus, so that OUR docs may legitimately cite into it.

Also not scanned: this tool's own testdata/, which contains deliberately
broken citations as fixtures.

OUTPUT
------
Findings on stdout, one block each, with the evidence inline. The last line is
always the machine-greppable verdict:

    DOCCITE SUMMARY: findings=N errors=N advisories=N files=N ...

Exit codes: 0 = no errors; 1 = errors found; 2 = harness failure.

This file is itself scanned by this tool. Every path and symbol named in this
docstring is checked by the check it documents.
"""

import argparse
import fnmatch
import hashlib
import json
import os
import pickle
import re
import subprocess
import sys
from collections import defaultdict

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))

# ---------------------------------------------------------------------------
# Tunables. Each of these trades recall against noise; the comment on each says
# which direction and why the value is where it is.
# ---------------------------------------------------------------------------

# An anchor token appearing more often than this in the cited file cannot
# localise anything, so it is not admissible as drift evidence.
ANCHOR_MAX_HITS = 12

# Below this length, snake_case tokens are too often accidents of prose.
MIN_IDENT_LEN = 6

# How many extra lines the markdown anchor window reaches on each side of a
# citation's own line, to catch a symbol that hand-wrapped onto a neighbouring
# line (see anchor_tokens). Kept small and symmetric: wide enough for an
# ordinary wrapped sentence or table cell, narrow enough that OWNERSHIP still
# reliably separates one citation's subject from the next row's.
MD_ANCHOR_WINDOW_LINES = 1

# A line carrying this marker is QUOTING a citation, not making one. See the
# quoted() helper in main() for the rule and why it is not an escape hatch.
QUOTE_MARKER = "doccite:quote"

# Severity for the two existence checks whose precision was MEASURED and found
# poor: hand-auditing 9 sampled findings of each gave phantom-path 0/9 and
# phantom-identifier 1/9. The residue is not a scoping bug that one more rule
# would fix -- it is documents citing another worktree, informal scratchpad
# artifacts, external tool and intrinsic names, and numbered-family
# placeholders like nm_NNNNN. All are things this tree genuinely cannot
# resolve and none is a defect.
#
# They stay ON, because this is the check that catches the kill_texcash_work
# class and that class is worth finding. They are ADVISORY, because a finding
# category that is wrong four times out of five must not be able to fail a
# build. Raise to "error" only alongside a fresh precision measurement.
PHANTOM_SEVERITY = "advisory"

# Extensions whose comment text is scanned for citations.
#
# .js earns its place the same way the C extensions did: tools/rendezvous-server
# is the production rendezvous server and its comments carry real file:line
# citations into src/netplay. Before it was listed here, linting that directory
# reported findings=0 over files_scanned=2 -- not "clean", but "not looked at".
CODE_PROSE_EXTS = {".c", ".h", ".cpp", ".hpp", ".py", ".sh", ".js"}

# Extensions treated as "code" when building the identifier existence corpus.
# Deliberately broad: a symbol may be defined in a Makefile, a linker script, a
# JSON config or a shell script and still be a real symbol.
CODE_CORPUS_EXTS = {
    ".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".s", ".S", ".inc",
    ".py", ".sh", ".bash", ".js", ".mk", ".mak", ".cmake", ".txt", ".json",
    ".yml", ".yaml", ".toml", ".cfg", ".ini", ".sv", ".v", ".qsf", ".tcl",
    ".sdc", ".qip", ".ld", ".mra", ".xml", ".rules", ".service", ".conf",
}

# Directories never scanned as citation sources. See SCOPE in the docstring.
# src/imgui is vendored Dear ImGui (src/imgui/imgui/imgui.h:1 reads
# "// dear imgui, v1.92.7") and gets the same treatment as vendor/ for the same
# reason: its comments cite an upstream layout we do not control and must not
# edit.
#
# docs/archive/ is here for a different reason, and it is the load-bearing one.
# An archived document is a dated record: it was true at the commit stamped in
# its header and is not maintained after that. Its line numbers are therefore
# not wrong -- they are correct about a tree that has moved on, which is the
# whole point of keeping the file. Scanning it produced a standing pile of
# `drift` findings that could only ever be discharged by rewriting history to
# match the present, and the repair commits that resulted ("repoint N
# citations ...") were 15% of one 30-hour window's commits while changing
# nothing a reader relies on. So the archive is not scanned, and repointing a
# citation inside it is not work. This is enforced by construction rather than
# by asking people not to: see check_archive_is_not_scanned() in
# test_doc_citations.py, which proves the exclusion fires AND that the same
# citation outside the archive is still caught.
SKIP_SCAN_DIRS = ("vendor/", "src/imgui/", "tools/doc-citations/testdata/",
                  "docs/archive/")

# File extensions that make a bare token look like a path reference. Used only
# to RECOGNISE a filename (so `menu_network.c` is not mistaken for a symbol).  # doccite:quote
PATH_EXTS = (
    "md|c|h|cpp|hpp|py|sh|js|txt|json|mk|yml|yaml|toml|cfg|ini|rbf|sv|v|qsf|"
    "tcl|sdc|mra|xml|log|csv|zip|bin|elf|map|ld|s|inc|patch|diff"
)

# Extensions whose existence is actually CHECKED. The difference is build
# outputs: 3S-ARM.rbf is referenced correctly and often by README.md and
# AGENTS.md, and is produced by Quartus rather than tracked, so demanding it
# exist in git is a guaranteed false positive.
CHECKED_EXTS = {
    "md", "c", "h", "cpp", "hpp", "py", "sh", "txt", "json", "mk", "yml",
    "yaml", "toml", "cfg", "ini", "sv", "v", "qsf", "tcl", "sdc", "mra",
    "xml", "s", "inc", "ld",
}

# Path prefixes that are outputs or tooling state, never tracked sources.
UNTRACKED_BY_DESIGN = ("build/", "out/", "dist/", ".claude/", "node_modules/",
                       "output_files/", "/media/", "media/fat/")

# ---------------------------------------------------------------------------
# Regexes
# ---------------------------------------------------------------------------

# A slash-bearing path, optionally followed by :LINE or :LINE-LINE.
RE_PATH_CITE = re.compile(
    r"(?<![\w/.:-])"
    r"((?:[\w.+-]+/)+[\w.+-]+\.(?:" + PATH_EXTS + r"))"
    r"(?::(\d+)(?:\s*[-–]\s*(\d+))?)?"
    r"(?![\w])"
)

# A bare filename (no directory) with a code/doc extension. Only honoured
# inside backticks, because a bare filename in free prose is frequently a
# mention rather than a reference.
RE_BARE_FILE = re.compile(
    r"^([\w.+-]+\.(?:" + PATH_EXTS + r"))(?::(\d+)(?:\s*[-–]\s*(\d+))?)?$"
)

# A bare filename carrying a LINE NUMBER, anywhere in prose, backticks or not.
# This is how in-tree comments in this repo actually cite -- texgroup.c:285
# reads "before calling Push_ramcnt_key (texgroup.c:477-479)", and ramcnt.c,  # doccite:quote
# mtrans.c and aboutspr.c are cited the same way. Requiring the :NNN is what
# makes it safe: "see texgroup.c" is a mention, "texgroup.c:477" is a pointer.
RE_BARE_CITE = re.compile(
    r"(?<![\w/.+-])([\w.+-]+\.(?:" + PATH_EXTS + r"))"
    r":(\d+)(?:\s*[-–]\s*(\d+))?(?![\w])"
)

# Extra line numbers hanging off a citation: `texcash.c:361/576/614`.
# Without this only the first number of such a citation was ever checked, and
# in-tree comments in this repo write them this way constantly.
RE_EXTRA_LINES = re.compile(r"[/,](\d+)")

# A line nobody cites on purpose: blank, a lone brace or comment delimiter, or
# a preprocessor include. Used as an anchor-free drift signal -- see
# check_path_cites. Deliberately conservative; a citation landing here is
# wrong regardless of what the prose was talking about.
RE_DEGENERATE = re.compile(r"^\s*(?:[{}();,]*|\*/|/\*+|#include\b.*)\s*$")

RE_BACKTICK = re.compile(r"`([^`\n]{1,200})`")
RE_WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

# `git cat-file --batch` object header: "<sha> blob <size>\n".
RE_CATFILE_HDR = re.compile(rb"([0-9a-f]{40}) blob (\d+)\n")
ZERO_SHA = "0" * 40

# History corpus (see the History class). The pathspecs whose every historical
# revision is indexed. .js is here for the same reason it is in
# CODE_PROSE_EXTS: tools/rendezvous-server is real code whose deleted names
# documents cite.
HISTORY_EXTS = ("*.c", "*.h", "*.cpp", "*.hpp", "*.py", "*.sh", "*.js")

# The stage-1 index cache lives in the git COMMON dir (shared by every linked
# worktree of the repository, invisible to `git status`), keyed on a digest of
# the raw log it was built from. Bump the format when the pickle layout
# changes; a mismatch is just a rebuild.
HISTORY_CACHE_FILE = "doc-citations-history.pickle"
HISTORY_CACHE_FORMAT = 1

# Stage-2 memo: blob sha -> its comment-stripped identifier set. A blob's
# content never changes, so this cache never goes stale; it is simply capped.
# Decommenting is the walk's whole cost (~11 MB/s, char-by-char), and the
# same few hundred blobs are decommented on every run -- a persisted memo
# makes the second run's walk almost free.
HISTORY_BLOB_MEMO_FILE = "doc-citations-history-blobs.pickle"
HISTORY_BLOB_MEMO_FORMAT = 1
HISTORY_BLOB_MEMO_MAX = 6000

# Stage-2 bounds. Decommenting measured ~11 MB/s on this tree; 128 MB is
# ~12 s of walking in the worst case before findings start saying
# "not located" instead of naming a commit. Raw blob bytes are kept resident
# up to HISTORY_BYTES_RESIDENT so the many names one commit removed from one
# file are answered from memory.
HISTORY_STRIP_BUDGET = 128 * 1024 * 1024
HISTORY_BYTES_RESIDENT = 96 * 1024 * 1024
RE_IDENT_FULL = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

# C4: an appeal to a named artifact. Requires (a) an evidence verb, (b) a
# qualifier carrying a digit/uppercase/backtick so that "see the table below"
# cannot fire, (c) an artifact noun.
RE_EVIDENCE_REF = re.compile(
    r"\b(?:see|per|cf\.?|documented in|recorded in|verified in|described in|"
    r"captured in|listed in|from)\s+"
    r"(?:the\s+)?"
    r"([`\"']?[A-Za-z0-9][\w.#/-]*[`\"']?(?:\s+[\w#.-]+){0,2}?)\s+"
    r"(task report|report|write-?up|document|doc|plan|appendix|memo|log|"
    r"transcript|dossier|brief)\b",
    re.IGNORECASE,
)


# ---------------------------------------------------------------------------
# Comment / prose extraction
# ---------------------------------------------------------------------------

def strip_c_comments(text, blank_strings=False):
    """Return (code_only, comment_spans).

    code_only has every comment byte replaced by a space, preserving offsets
    and newlines, so the identifier corpus sees code and not commentary.
    comment_spans is a list of (start, end) offsets of comment bodies.

    A real state machine rather than a regex: `"http://x"` contains // and is
    not a comment, and a regex gets that wrong.

    blank_strings additionally blanks STRING-LITERAL bytes. A literal is prose
    that happens to sit inside the code -- an assertion message naming the
    constant it guards is a MENTION, not a definition -- so anchor selection
    must not count it as code. Measured: without this, `--fix` wanted to
    repoint RACE_PUNCH_SETTLE_MS and race_budget_expired onto the two
    _Static_assert message lines in direct_p2p.c, outranking the `#define` and
    the function's own definition. Comment spans are unaffected either way, so
    prose extraction sees exactly what it saw before.
    """
    out = list(text)
    spans = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == '"' or ch == "'":
            quote = ch
            body = i + 1
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                if text[i] == "\n" and quote == '"':
                    break  # unterminated; bail rather than eat the file
                i += 1
            if blank_strings:
                for k in range(body, min(i, n)):
                    if text[k] != "\n":
                        out[k] = " "
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            start = i
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
            spans.append((start + 2, i))
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            start = i
            out[i] = out[i + 1] = " "
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            end = i
            while i < n and i < end + 2:
                out[i] = " "
                i += 1
            spans.append((start + 2, end))
            continue
        i += 1
    return "".join(out), spans


def strip_hash_comments(text, triple_quotes_are_prose, blank_strings=False):
    """Same contract as strip_c_comments, for Python and shell.

    Python docstrings count as prose: this repo's own tools carry their
    citations there (tools/ldreq-timing/check_ldreq_timing.py cites
    src/port/io/afs.c:366 from inside its module docstring).  # doccite:quote
    """
    out = list(text)
    spans = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if triple_quotes_are_prose and text[i:i + 3] in ('"""', "'''"):
            q = text[i:i + 3]
            start = i
            for k in range(3):
                out[i + k] = " "
            i += 3
            while i < n and text[i:i + 3] != q:
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            end = i
            for k in range(3):
                if i + k < n:
                    out[i + k] = " "
            i = min(n, i + 3)
            spans.append((start + 3, end))
            continue
        if ch == '"' or ch == "'":
            quote = ch
            body = i + 1
            i += 1
            while i < n and text[i] != "\n":
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            if blank_strings:
                for k in range(body, min(i, n)):
                    if text[k] != "\n":
                        out[k] = " "
            continue
        if ch == "#":
            start = i
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
            spans.append((start + 1, i))
            continue
        i += 1
    return "".join(out), spans


def line_index(text):
    """Offsets of each line start, for offset -> line-number conversion."""
    starts = [0]
    for m in re.finditer(r"\n", text):
        starts.append(m.end())
    return starts


def offset_to_line(starts, off):
    lo, hi = 0, len(starts) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if starts[mid] <= off:
            lo = mid
        else:
            hi = mid - 1
    return lo + 1


# Extensions strip_comments_for_ext knows a decommenter for. A strict
# subset of CODE_CORPUS_EXTS -- e.g. .json/.xml/.sv have no entry here
# because no decommenter below handles them, so they stay "don't know,
# don't filter" rather than silently passing every line through as code.
DECOMMENTABLE_EXTS = {
    ".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".js", ".py", ".sh", ".bash",
    ".mk", ".mak", ".cmake", ".tcl", ".cfg", ".ini", ".yml", ".yaml",
    ".toml", ".conf", ".rules", ".service",
}

# Of DECOMMENTABLE_EXTS, the ones where a quoted run is a STRING LITERAL --
# payload text that happens to be spelled in the file, not code that defines or
# uses anything. Only these get blank_strings=True (see strip_comments_for_ext).
#
# The config formats (.yml/.yaml/.toml/.cfg/.ini/.conf/.mk/.tcl/.rules/
# .service) are deliberately absent: there, a quoted run IS the content, quoting
# is optional and inconsistent, and blanking it would delete real data rather
# than prose. Comment stripping still applies to them exactly as before.
STRING_LITERAL_EXTS = {
    ".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".js", ".py", ".sh", ".bash",
}


def strip_comments_for_ext(text, ext, blank_strings=False):
    """Dispatch to the right decommenter for `ext`. Caller must check
    `ext in DECOMMENTABLE_EXTS` first -- an unrecognized extension falls
    through to returning `text` unchanged, which looks like "no comments
    found" rather than "don't know".

    Shared by code_tokens() (the whole-repo identifier corpus) and
    Repo.code_only_lines() (per-file, line-indexed). They agree on comments.
    They differ, deliberately, on string literals: code_only_lines asks "is
    this line a DEFINITION/USE I should anchor a citation to", for which a
    literal is prose and must not count; code_tokens asks the weaker "does
    this name exist in this tree at all", for which a literal spelling it out
    is still evidence and dropping it would manufacture phantom findings.
    blank_strings is honoured only for STRING_LITERAL_EXTS.
    """
    if ext in (".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".js"):
        return strip_c_comments(text, blank_strings)[0]
    if ext == ".py":
        return strip_hash_comments(text, True, blank_strings)[0]
    if ext in (".sh", ".bash"):
        return strip_hash_comments(text, False, blank_strings)[0]
    if ext in (".mk", ".mak", ".cmake", ".tcl", ".cfg",
               ".ini", ".yml", ".yaml", ".toml", ".conf", ".rules",
               ".service"):
        return strip_hash_comments(text, False)[0]
    return text


# ---------------------------------------------------------------------------
# Repo model
# ---------------------------------------------------------------------------

class Repo:
    def __init__(self, root):
        self.root = root
        self.files = self._tracked()
        self.by_suffix = defaultdict(list)
        self.by_base = defaultdict(list)
        for p in self.files:
            self.by_base[os.path.basename(p)].append(p)
            parts = p.split("/")
            for k in range(len(parts)):
                self.by_suffix["/".join(parts[k:])].append(p)
        self._text_cache = {}
        self._code_tokens = None
        self._code_only_lines_cache = {}

    def _tracked(self):
        out = subprocess.run(
            ["git", "-C", self.root, "ls-files", "-z"],
            capture_output=True, text=True, check=True,
        ).stdout
        return [p for p in out.split("\0") if p]

    def read(self, relpath):
        if relpath not in self._text_cache:
            full = os.path.join(self.root, relpath)
            try:
                with open(full, "r", encoding="utf-8", errors="replace") as fh:
                    self._text_cache[relpath] = fh.read()
            except (OSError, IsADirectoryError):
                self._text_cache[relpath] = None
        return self._text_cache[relpath]

    def lines(self, relpath):
        t = self.read(relpath)
        return t.split("\n") if t is not None else None

    def code_only_lines(self, relpath):
        """Lines of relpath with comment bytes blanked out, same indexing
        as lines(). A regex search against these can only match real code,
        never a comment/prose mention of the same token -- see anchor
        selection in check_path_cites.handle(), which is why this exists:
        a popular identifier is discussed in many comments but
        defined/used in only a few real code lines, and a citation is
        (measured exception aside) about the latter.

        Returns None when this tool has no decommenter for the extension
        -- callers must treat None as "don't know, don't filter", not as
        "no comments found".
        """
        if relpath not in self._code_only_lines_cache:
            ext = os.path.splitext(relpath)[1]
            t = self.read(relpath)
            if t is None or ext not in DECOMMENTABLE_EXTS:
                result = None
            else:
                result = strip_comments_for_ext(
                    t, ext, blank_strings=ext in STRING_LITERAL_EXTS
                ).split("\n")
            self._code_only_lines_cache[relpath] = result
        return self._code_only_lines_cache[relpath]

    def resolve(self, cited):
        """Resolve a cited path. Returns (relpath|None, how).

        how is one of: exact, root, suffix, ambiguous, basename, missing.
        'basename' means the file exists but NOT where the citation says --
        that is the tools/mister/build-deps.sh class and it is an error with a  # doccite:quote
        suggestion attached.
        """
        cited = cited.lstrip("./")
        if cited in self.by_base and len(self.by_base[cited]) == 1 and "/" not in cited:
            return self.by_base[cited][0], "exact"
        if os.path.isfile(os.path.join(self.root, cited)):
            return cited, "exact"
        for pre in ("src", "include", "vendor", "tools"):
            cand = pre + "/" + cited
            if os.path.isfile(os.path.join(self.root, cand)):
                return cand, "root"
        hits = self.by_suffix.get(cited)
        if hits:
            if len(hits) == 1:
                return hits[0], "suffix"
            return None, "ambiguous"
        base = os.path.basename(cited)
        bhits = self.by_base.get(base)
        if bhits:
            return None, "basename"
        return None, "missing"

    def code_tokens(self):
        """Whole-word identifier set over all code, COMMENTS STRIPPED."""
        if self._code_tokens is not None:
            return self._code_tokens
        toks = set()
        for p in self.files:
            ext = os.path.splitext(p)[1]
            if ext and ext not in CODE_CORPUS_EXTS:
                continue
            if p.startswith("docs/") or ext == ".md":
                continue
            # SELF-EXCLUSION. This tool's own fixtures and test name the very
            # symbols they assert do not exist ("kill_texcash_work" appears in
            # test_doc_citations.py as a string literal). Counting those as
            # evidence would make every phantom real the moment a test asserts
            # it -- the linter would silently stop being able to fail. Found by
            # the acceptance test, which is the point of having one.
            if p.startswith("tools/doc-citations/"):
                continue
            t = self.read(p)
            if t is None or "\0" in t[:4096]:
                continue
            if ext in DECOMMENTABLE_EXTS:
                t = strip_comments_for_ext(t, ext)
            for m in RE_WORD.finditer(t):
                toks.add(m.group(0))
        self._code_tokens = toks
        return toks


def doc_class(relpath, record_globs):
    """REFERENCE or RECORD -- which decides whether "this does not exist" is a
    defect or the document's entire point.

    A plan document names files and functions that do not exist BECAUSE IT IS
    PROPOSING THEM. docs/archive/plan-netplay-port.md cites src/netplay/lobby_server.c  # doccite:quote
    throughout; that port was abandoned and the file was never written. Calling
    that an error demands that every finished proposal be rewritten to match a
    tree that overtook it, and it is (measured on this tree) the largest single
    block of findings.

    A reference document and a code comment make no such promise. They describe
    what IS. When they name something that does not exist, a reader following
    the citation finds nothing, which is the failure this linter exists for.

    Applies ONLY to existence findings (phantom-*, wrong-path). Drift and
    line-out-of-range stay errors in every class: they are citations into files
    that exist RIGHT NOW, pointing at the wrong line of them, and that is
    equally wrong and equally mechanical to fix in a plan or a reference.
    """
    for pat in record_globs:
        if fnmatch.fnmatch(relpath, pat):
            return "record"
    return "reference"


def load_record_globs(path):
    globs = []
    if not os.path.isfile(path):
        return globs
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if line:
                globs.append(line)
    return globs


class Removal:
    """Where a symbol that is no longer in the tree went. See History.removal.

    kind is one of:
      "commit"       removed from `path` by `commit`, an ancestor of HEAD
      "uncommitted"  HEAD's path still has it as code; the working tree does not
      "renamed"      path was renamed to moved_to by commit while still
                     carrying it as code; the removal from moved_to was not found
      "gone-path"    HEAD's last indexed revision of `path` has it as code, but the
                     path is not tracked any more and no diff recorded its deletion
      "branch"       removed from `path` by `commit`, which HEAD does not descend from
      "elsewhere"    exists as code in `path` only on a ref HEAD does not descend from
      "unlocated"    named by historical revisions of `paths`, but the search budget
                     ran out before any of them was decommented, so code-or-comment
                     is unknown
      "comments-only" every historical mention, in every path, is inside a comment

    Every kind but the last two means the name existed as code.
    """

    __slots__ = ("kind", "path", "commit", "moved_to", "paths", "examined")

    # Lower is better. removal() stops at the first result of rank 0.
    RANK = {"commit": 0, "uncommitted": 0, "branch": 1, "renamed": 2,
            "gone-path": 2, "elsewhere": 3, "unlocated": 4, "comments-only": 5}

    def __init__(self, kind, path=None, commit=None, moved_to=None,
                 paths=(), examined=()):
        self.kind = kind
        self.path = path
        self.commit = commit
        self.moved_to = moved_to
        self.paths = list(paths)
        self.examined = list(examined)


class History:
    """What this repo has EVER contained, used to separate two different
    failures that look identical to a naive checker.

        docs/archive/plan-perf-2-rgb565-canvas.md cites src/port/sdl/sdl_game_renderer.c  # doccite:quote
        -- that file had 86 commits and was deleted by the renderer rip-out.

        an agent brief cited tools/mister/build-deps.sh  # doccite:quote
        -- no commit in this repository has ever contained that path.

    The first is a historical record: the document was true when written and is
    describing work that has since been removed. Reporting it as an error means
    demanding that finished plan documents be rewritten forever, and produces
    (measured on this tree) over a thousand findings nobody will action.

    The second is a fabrication. It was never true. That is the defect class
    this linter exists for, and it is the one that caused real harm.

    So: existed-and-was-deleted -> advisory. Never-existed -> error.

    Paths come from `git log --all --name-only`, which is exact.

    Identifiers come in two stages, because exactness and speed pull in
    opposite directions here.

    Stage 1, the INDEX: every blob that any revision on any ref ever had at a
    HISTORY_EXTS path -- INCLUDING paths that still exist -- is tokenised raw
    (comments kept) into a per-path token set. This is the cheap, complete
    question "has this name ever appeared in that file, in any form?". It used
    to skip paths still in the tree, which halved the blob count but made the
    checker say "has never existed" about every symbol deleted from a file
    that survived it -- 35 real, shipped names in one document, removed by a
    single commit, all reported as fabrications. A checker that is
    confidently wrong is worse than one that is silent, so that guard is gone.
    The index is 12.6k blobs / 345 MB on this tree and takes ~4 s to build,
    which is why it is cached: a pickle in the git common dir, keyed on a
    digest of the raw log it was built from, so any new commit on any ref
    rebuilds it and an unchanged history loads in ~0.05 s.

    Stage 2, the WALK: only for a name a document actually cites and the
    current code does not have. Each candidate path's revisions are walked
    newest-first, decommented on demand, to find the commit whose diff took
    the name out of that file as CODE. That is what lets the finding say
    "removed from src/netplay/direct_p2p.c in 2c63adc7" instead of "no longer
    exists", and it is also what keeps a name that only ever lived in a
    comment from being called real. Decommenting is ~11 MB/s, so the walk is
    memoised per blob and bounded by HISTORY_STRIP_BUDGET; past the budget a
    finding says so rather than guessing.

    Two git defaults would make the walk lie, and both are overridden. `git
    log --raw` detects renames, so a moved file prints ONE entry naming both
    paths; read naively, the old path appears to live on with the new file's
    content, and a symbol that was really deleted from the new path years
    later gets blamed on the restructure commit that moved it. Rename entries
    are therefore split: the old path ends (kind "renamed") and the new path
    inherits the pre-rename blob as its "old" side, so the walk continues
    across the move. And `git log` prints NO diff for a merge commit, so a
    symbol dropped during conflict resolution -- menu.c lost
    check_netplay_cancelled in merge 9dd530bf that way -- had no removing
    entry at all and was misreported as an uncommitted deletion.
    --full-history --diff-merges=first-parent makes merges print their diff
    against the line of development they landed on -- and because that diff
    is then where the walk first meets a removal that really happened on the
    merged branch, the walk steps down into the merge's side commits and
    names the one that did it (2c63adc7, not the merge that landed it).

    Self-exclusion mirrors Repo.code_tokens: tools/doc-citations/ is never
    indexed, because this tool's own tests name the very symbols they assert
    do not exist.
    """

    def __init__(self, root, enabled=True, use_cache=True):
        self.root = root
        self.enabled = enabled
        self.use_cache = use_cache
        self._paths = None
        self._suffix = None
        self._tokens = None
        # Stage 1 (see class docstring).
        self._index = None        # path -> set of raw tokens over every revision
        self._path_bytes = None   # path -> total blob bytes, for cheapest-first
        self._entries = None      # path -> [(commit, old_blob, new_blob,
                                  #           renamed_to_or_None)], newest first
        self._commits = None      # commit -> (date, subject)
        self._parents = None      # commit -> [parent, ...]
        self._side = {}           # merge -> commits on its merged-in side
        self._head = None         # commits reachable from HEAD
        self._tracked = None      # paths in the working tree (git ls-files)
        self.cache_hit = None     # True/False after the index is loaded
        # Stage 2.
        self._blob_bytes = {}     # blob -> raw bytes (evicted in bulk)
        self._blob_code = {}      # blob -> frozenset of comment-stripped tokens
        self._blob_memo = None    # blob -> b"tok\ntok..." persisted across runs
        self._blob_memo_dirty = False
        self._removals = {}       # token -> Removal or None
        self.stripped_bytes = 0

    def _git(self, args, input=None):
        return subprocess.run(["git", "-C", self.root] + args,
                              capture_output=True, check=True,
                              input=input).stdout

    def paths(self):
        if not self.enabled:
            return None
        if self._paths is None:
            out = self._git(["log", "--all", "--pretty=format:", "--name-only"])
            self._paths = {p for p in out.decode("utf-8", "replace").split("\n")
                           if p.strip()}
        return self._paths

    def has_path(self, cited):
        """True if any commit ever contained this path.

        Suffix-tolerant, because docs routinely drop the src/ prefix and write
        `sf33rd/Source/Game/effect/effl8.c`.
        """
        paths = self.paths()
        if paths is None:
            return None
        cited = cited.lstrip("./")
        if cited in paths:
            return True
        if self._suffix is None:
            self._suffix = set()
            for p in paths:
                parts = p.split("/")
                for k in range(len(parts)):
                    self._suffix.add("/".join(parts[k:]))
        return cited in self._suffix

    # -- stage 1: the index ------------------------------------------------

    def _load_index(self):
        if self._index is not None:
            return
        raw = self._git(["log", "--all", "--topo-order", "--full-history",
                         "--diff-merges=first-parent", "--raw", "--no-abbrev",
                         "--pretty=format:%H%x00%P%x00%as%x00%s", "--"]
                        + list(HISTORY_EXTS))
        entries = defaultdict(list)
        commits = {}
        parents = {}
        blob_paths = defaultdict(list)
        commit = None
        for line in raw.decode("utf-8", "replace").split("\n"):
            if not line:
                continue
            if not line.startswith(":"):
                parts = line.split("\0", 3)
                if len(parts) == 4 and len(parts[0]) == 40:
                    commit = parts[0]
                    parents[commit] = parts[1].split()
                    commits[commit] = (parts[2], parts[3])
                continue
            fields = line.split("\t")
            meta = fields[0].split()
            if len(meta) < 5 or commit is None:
                continue
            old, new, status = meta[2], meta[3], meta[4][0]
            paths = [p for p in fields[1:]
                     if not p.startswith("tools/doc-citations/")]
            if status in "RC" and len(fields) == 3:
                src_p, dst_p = fields[1], fields[2]
                if status == "R" and src_p in paths:
                    entries[src_p].append((commit, old, ZERO_SHA, dst_p))
                if dst_p in paths:
                    entries[dst_p].append(
                        (commit, old if status == "R" else ZERO_SHA, new, None))
                    if new.strip("0") and dst_p not in blob_paths[new]:
                        blob_paths[new].append(dst_p)
                if old.strip("0") and src_p in paths and src_p not in blob_paths[old]:
                    blob_paths[old].append(src_p)
                continue
            for p in paths:
                entries[p].append((commit, old, new, None))
                for sha in (old, new):
                    if sha.strip("0") and p not in blob_paths[sha]:
                        blob_paths[sha].append(p)
        self._entries = entries
        self._commits = commits
        self._parents = parents
        key = hashlib.sha256(b"%d\n" % HISTORY_CACHE_FORMAT + raw).hexdigest()
        cached = self._read_cache(key) if self.use_cache else None
        self.cache_hit = cached is not None
        if cached is None:
            cached = self._build_index(blob_paths)
            if self.use_cache:
                self._write_cache(key, cached)
        self._index, self._path_bytes = cached

    def _build_index(self, blob_paths):
        index = defaultdict(set)
        path_bytes = defaultdict(int)
        if not blob_paths:
            return dict(index), dict(path_bytes)
        body = self._git(["cat-file", "--batch"],
                         input="\n".join(sorted(blob_paths)).encode())
        intern = sys.intern
        pos = 0
        while pos < len(body):
            m = RE_CATFILE_HDR.match(body, pos)
            if not m:  # "<sha> missing" -- cannot happen for shas git just listed
                nl = body.find(b"\n", pos)
                pos = nl + 1 if nl >= 0 else len(body)
                continue
            sha = m.group(1).decode()
            size = int(m.group(2))
            start = m.end()
            data = body[start:start + size]
            pos = start + size + 1
            toks = set(map(intern, RE_WORD.findall(data.decode("utf-8", "replace"))))
            for p in blob_paths[sha]:
                index[p].update(toks)
                path_bytes[p] += size
        return dict(index), dict(path_bytes)

    def _cache_file(self, name=HISTORY_CACHE_FILE):
        try:
            common = self._git(["rev-parse", "--git-common-dir"]).decode().strip()
        except (subprocess.CalledProcessError, OSError):
            return None
        if not os.path.isabs(common):
            common = os.path.join(self.root, common)
        return os.path.join(common, name)

    def _write_pickle(self, path, payload):
        tmp = "%s.%d.tmp" % (path, os.getpid())
        try:
            with open(tmp, "wb") as fh:
                pickle.dump(payload, fh, protocol=pickle.HIGHEST_PROTOCOL)
            os.replace(tmp, path)
        except OSError:
            try:
                os.unlink(tmp)
            except OSError:
                pass

    def _read_cache(self, key):
        path = self._cache_file()
        if not path or not os.path.isfile(path):
            return None
        try:
            with open(path, "rb") as fh:
                data = pickle.load(fh)
            if data.get("format") == HISTORY_CACHE_FORMAT and data.get("key") == key:
                return data["index"], data["path_bytes"]
        except Exception:  # a corrupt or foreign cache is just a cache miss
            return None
        return None

    def _write_cache(self, key, cached):
        path = self._cache_file()
        if path:
            self._write_pickle(path, {"format": HISTORY_CACHE_FORMAT, "key": key,
                                      "index": cached[0], "path_bytes": cached[1]})

    def _load_blob_memo(self):
        if self._blob_memo is not None:
            return self._blob_memo
        self._blob_memo = {}
        path = self._cache_file(HISTORY_BLOB_MEMO_FILE) if self.use_cache else None
        if path and os.path.isfile(path):
            try:
                with open(path, "rb") as fh:
                    data = pickle.load(fh)
                if data.get("format") == HISTORY_BLOB_MEMO_FORMAT:
                    self._blob_memo = data["blobs"]
            except Exception:
                pass
        return self._blob_memo

    def save(self):
        """Persist the stage-2 memo if this run added to it. Call once, at
        the end of a run; cheap when nothing changed."""
        if not (self.use_cache and self._blob_memo_dirty):
            return
        path = self._cache_file(HISTORY_BLOB_MEMO_FILE)
        if path:
            self._write_pickle(path, {"format": HISTORY_BLOB_MEMO_FORMAT,
                                      "blobs": self._blob_memo})
        self._blob_memo_dirty = False

    def tokens(self):
        """Every identifier any indexed revision ever contained, comments
        included. Membership here is necessary but not sufficient for "did
        exist as code" -- see removal()."""
        if not self.enabled:
            return None
        if self._tokens is None:
            self._load_index()
            self._tokens = set()
            for s in self._index.values():
                self._tokens |= s
        return self._tokens

    def where(self, tok):
        """Paths whose history names `tok` anywhere, cheapest-to-walk first."""
        self._load_index()
        hits = [p for p, s in self._index.items() if tok in s]
        hits.sort(key=lambda p: (self._path_bytes.get(p, 0), p))
        return hits

    # -- stage 2: the walk -------------------------------------------------

    def _head_commits(self):
        if self._head is None:
            out = self._git(["rev-list", "HEAD"]).decode()
            self._head = set(out.split())
        return self._head

    def _side_commits(self, merge):
        """Commits a merge brought in: reachable from its second parent and
        not from its first. Empty for a non-merge."""
        if merge not in self._side:
            ps = self._parents.get(merge, [])
            if len(ps) < 2:
                self._side[merge] = frozenset()
            else:
                out = self._git(["rev-list", ps[1], "^" + ps[0]]).decode()
                self._side[merge] = frozenset(out.split())
        return self._side[merge]

    def _refine_merge(self, tok, path, commit, on_head):
        """A merge's first-parent diff is where the walk first sees a removal
        that really happened on the branch it merged. Prefer the branch
        commit that did it; keep the merge only when the removal happened in
        the merge itself (conflict resolution)."""
        while True:
            side = self._side_commits(commit)
            if not side:
                return commit
            for c2, old, new, moved in on_head:
                if (c2 in side and not moved
                        and not self._has_code(tok, new, path)
                        and self._has_code(tok, old, path)):
                    commit = c2
                    break
            else:
                return commit

    def _tracked_paths(self):
        if self._tracked is None:
            out = self._git(["ls-files", "-z"]).decode("utf-8", "replace")
            self._tracked = {p for p in out.split("\0") if p}
        return self._tracked

    def _blob_data(self, blob, path):
        data = self._blob_bytes.get(blob)
        if data is not None:
            return data
        if sum(len(v) for v in self._blob_bytes.values()) > HISTORY_BYTES_RESIDENT:
            self._blob_bytes.clear()
        shas = sorted({s for _c, o, n, _m in self._entries.get(path, ())
                       for s in (o, n) if s.strip("0")})
        body = self._git(["cat-file", "--batch"], input="\n".join(shas).encode())
        pos = 0
        while pos < len(body):
            m = RE_CATFILE_HDR.match(body, pos)
            if not m:
                nl = body.find(b"\n", pos)
                pos = nl + 1 if nl >= 0 else len(body)
                continue
            size = int(m.group(2))
            start = m.end()
            self._blob_bytes[m.group(1).decode()] = body[start:start + size]
            pos = start + size + 1
        return self._blob_bytes.get(blob, b"")

    def _has_code(self, tok, blob, path):
        """Does this revision of `path` contain `tok` OUTSIDE comments?"""
        if not blob.strip("0"):
            return False
        toks = self._blob_code.get(blob)
        if toks is None:
            memo = self._load_blob_memo()
            packed = memo.get(blob)
            if packed is not None:
                toks = frozenset(packed.decode().split("\n"))
                self._blob_code[blob] = toks
                return tok in toks
            data = self._blob_data(blob, path)
            # Whole-word pre-check: a blob that lacks the name even in comments
            # cannot have it as code, and is not worth decommenting. \b, not a
            # lookbehind: the lookbehind form measured 10 s of a 16 s walk.
            if not re.search(rb"\b" + re.escape(tok.encode()) + rb"\b", data):
                return False
            text = data.decode("utf-8", "replace")
            ext = os.path.splitext(path)[1]
            if ext in DECOMMENTABLE_EXTS:
                text = strip_comments_for_ext(text, ext)
            self.stripped_bytes += len(text)
            toks = frozenset(RE_WORD.findall(text))
            self._blob_code[blob] = toks
            if len(memo) < HISTORY_BLOB_MEMO_MAX:
                memo[blob] = "\n".join(sorted(toks)).encode()
                self._blob_memo_dirty = True
        return tok in toks

    def _removal_in(self, tok, path):
        """The Removal of `tok` from `path`, or None if `path` only ever
        mentioned it in comments."""
        ents = self._entries.get(path, [])
        head = self._head_commits()
        on_head = [e for e in ents if e[0] in head]
        off_head = [e for e in ents if e[0] not in head]
        # --topo-order lists a commit before every ancestor of it, and merges
        # print their first-parent diff, so the first HEAD-reachable entry is
        # HEAD's own revision of this path.
        if on_head:
            commit, _old, new, _moved = on_head[0]
            if self._has_code(tok, new, path):
                if path in self._tracked_paths():
                    return Removal("uncommitted", path, commit)
                return Removal("gone-path", path, commit)
            for commit, old, new, moved in on_head:
                if not self._has_code(tok, new, path) and self._has_code(tok, old, path):
                    if moved:
                        return Removal("renamed", path, commit, moved_to=moved)
                    return Removal("commit", path,
                                   self._refine_merge(tok, path, commit, on_head))
        found = None
        for commit, old, new, moved in off_head:
            if self._has_code(tok, new, path):
                found = found or Removal("elsewhere", path, commit)
            elif self._has_code(tok, old, path):
                if moved:
                    found = found or Removal("renamed", path, commit, moved_to=moved)
                    continue
                return Removal("branch", path, commit)
        return found

    def removal(self, tok):
        """Where `tok` went, or None if no indexed revision ever named it (or
        history is disabled). Prefers a HEAD-ancestor answer over a branch
        one, and stops as soon as it has one."""
        if not self.enabled:
            return None
        if tok in self._removals:
            return self._removals[tok]
        paths = self.where(tok)
        result = None
        if paths:
            examined = []
            best = None
            queue = list(paths)
            while queue:
                p = queue.pop(0)
                if self.stripped_bytes > HISTORY_STRIP_BUDGET:
                    best = best or Removal("unlocated", paths=paths, examined=examined)
                    break
                r = self._removal_in(tok, p)
                examined.append(p)
                if r is None:
                    continue
                if best is None or Removal.RANK[r.kind] < Removal.RANK[best.kind]:
                    best = r
                if Removal.RANK[best.kind] == 0:
                    break
                # A rename hands the walk on to the new path, whose history is
                # where the real removal is.
                if r.kind == "renamed" and r.moved_to not in examined \
                        and r.moved_to not in queue:
                    queue.append(r.moved_to)
            result = best or Removal("comments-only", paths=paths, examined=examined)
            result.paths = paths
        self._removals[tok] = result
        return result

    def describe(self, tok, rem):
        """(message, evidence list) for a stale-identifier finding."""
        short = rem.commit[:8] if rem.commit else None
        date, subject = self._commits.get(rem.commit, ("", "")) if rem.commit else ("", "")
        others = [p for p in rem.paths if p != rem.path]
        also = ("; %d other historical file%s also carried it (%s)"
                % (len(others), "" if len(others) == 1 else "s",
                   ", ".join(others[:3]) + (", ..." if len(others) > 3 else ""))
                if others else "")
        if rem.kind == "commit":
            return ("`%s` no longer exists in the tree: removed from `%s` in `%s`"
                    % (tok, rem.path, short),
                    ["%s %s %s" % (short, date, subject),
                     "it did exist as code, so this document is describing "
                     "removed code" + also])
        if rem.kind == "uncommitted":
            return ("`%s` no longer exists in the tree: removed from `%s` by an "
                    "uncommitted change" % (tok, rem.path),
                    ["still present as code at HEAD (%s %s %s)"
                     % (short, date, subject) + also])
        if rem.kind == "renamed":
            return ("`%s` no longer exists in the tree: last seen in `%s`, which "
                    "`%s` renamed to `%s`; its removal from there was not located"
                    % (tok, rem.path, short, rem.moved_to),
                    ["%s %s %s" % (short, date, subject),
                     "it did exist as code" + also])
        if rem.kind == "gone-path":
            return ("`%s` no longer exists in the tree: it went with `%s`, which "
                    "is no longer tracked (last indexed revision `%s`)"
                    % (tok, rem.path, short),
                    ["%s %s %s" % (short, date, subject),
                     "it did exist as code" + also])
        if rem.kind == "branch":
            return ("`%s` no longer exists in the tree: removed from `%s` in `%s`, "
                    "a commit HEAD does not descend from" % (tok, rem.path, short),
                    ["%s %s %s" % (short, date, subject),
                     "it did exist as code on another ref" + also])
        if rem.kind == "elsewhere":
            return ("`%s` is not in this tree: it exists as code in `%s` only on "
                    "a ref HEAD does not descend from (`%s`)"
                    % (tok, rem.path, short),
                    ["%s %s %s" % (short, date, subject) + also])
        # unlocated
        shown = ", ".join(rem.paths[:3]) + (", ..." if len(rem.paths) > 3 else "")
        return ("`%s` no longer exists in the tree: historical revisions of %d "
                "file%s name it (%s), but the removing commit was not located"
                % (tok, len(rem.paths), "" if len(rem.paths) == 1 else "s", shown),
                ["the history walk hit its budget (HISTORY_STRIP_BUDGET) after "
                 "%d of %d files; code-or-comment is unverified for the rest"
                 % (len(rem.examined), len(rem.paths))])


# ---------------------------------------------------------------------------
# Prose units
# ---------------------------------------------------------------------------

class Prose:
    """A contiguous run of prose with a known line span in a known file."""

    __slots__ = ("path", "text", "first_line", "line_of")

    def __init__(self, path, text, first_line, line_of):
        self.path = path
        self.text = text
        self.first_line = first_line
        self.line_of = line_of  # offset-within-text -> absolute file line


def prose_units(repo, relpath):
    text = repo.read(relpath)
    if text is None:
        return []
    ext = os.path.splitext(relpath)[1]
    if ext == ".md":
        starts = line_index(text)
        return [Prose(relpath, text, 1, lambda o, s=starts: offset_to_line(s, o))]
    if ext in (".c", ".h", ".cpp", ".hpp", ".js"):
        _, spans = strip_c_comments(text)
    elif ext == ".py":
        _, spans = strip_hash_comments(text, True)
    elif ext in (".sh", ".bash"):
        _, spans = strip_hash_comments(text, False)
    else:
        return []
    starts = line_index(text)
    # Merge comment spans separated only by whitespace, so a multi-line
    # comment BLOCK is one anchor context rather than N disconnected lines.
    merged = []
    for s, e in spans:
        if merged and text[merged[-1][1]:s].strip() == "":
            merged[-1] = (merged[-1][0], e)
        else:
            merged.append((s, e))
    units = []
    for s, e in merged:
        body = text[s:e]
        if not body.strip():
            continue
        units.append(Prose(relpath, body, offset_to_line(starts, s),
                           lambda o, s=s, st=starts: offset_to_line(st, s + o)))
    return units


# ---------------------------------------------------------------------------
# Identifier candidates
# ---------------------------------------------------------------------------

RE_LOOKS_LIKE_FILENAME = re.compile(r"\.(?:" + PATH_EXTS + r")$")


def unwrap_identifiers(raw):
    """Turn one backtick payload into zero or more identifier candidates."""
    s = raw.strip()
    s = re.sub(r"\(\s*\)$", "", s)          # foo()
    s = re.sub(r"\[[^\]]*\]$", "", s)        # foo[2]
    s = s.lstrip("&*").rstrip(",.;:")
    if not s:
        return []
    # `menu_network.c` is a FILENAME, not a struct access. Without this the  # doccite:quote
    # dotted split below happily proposes `menu_network` as a missing symbol,  # doccite:quote
    # which is both wrong and the single largest source of false identifier
    # findings measured on this tree.
    if RE_LOOKS_LIKE_FILENAME.search(s):
        return []
    # Split struct/member access so `PLW.spmv_ng_flag` checks both halves.
    parts = re.split(r"->|\.|::", s)
    out = []
    for p in parts:
        p = p.strip()
        if RE_IDENT_FULL.match(p):
            out.append(p)
    if len(parts) > 1 and len(out) != len(parts):
        return []  # something in there was not an identifier; not a symbol ref
    return out


def is_symbol_candidate(tok, extra_vocab=None):
    """Could this prose token be naming a symbol?

    The default rule is deliberately blunt -- >= MIN_IDENT_LEN and containing an
    underscore -- because below that, snake_case tokens in prose are more often
    accidents than symbols.

    extra_vocab (task #110) is an escape from that bluntness for the files where
    it does the most damage. In a macro table like src/netplay/game_state.c, the
    thing a citation is about is a save-set MEMBER NAME, and 80 of that file's
    609 members are shorter than MIN_IDENT_LEN -- `bg_w`, `M_Lv`, `VS_Index`.
    The generic rule can never anchor on any of them, so every citation about
    one of them is unanchorable by construction. When the cited file supplies a
    mechanically-derived vocabulary of its own macro arguments, membership in
    that vocabulary is better evidence than any length heuristic, so it is
    accepted directly. This only ever ADDS candidates.
    """
    if extra_vocab and tok in extra_vocab:
        return RE_IDENT_FULL.match(tok) is not None
    return (
        len(tok) >= MIN_IDENT_LEN
        and "_" in tok
        and RE_IDENT_FULL.match(tok) is not None
    )


# An ALL-CAPS macro call taking an identifier as its first argument:
# GS_SAVE(bg_w), GS_LOAD(zoom_request_flag), HASHONE(x). This is how the
# save-set files name their members, and it is the whole vocabulary that
# matters for citations into them.
RE_MACRO_ARG = re.compile(
    r"\b[A-Z][A-Z0-9_]{2,}\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*[,)]")

# A preprocessor definition. Its arguments are formal parameters, not members.
RE_DEFINE_LINE = re.compile(r"^\s*#\s*define\b")

_MACRO_VOCAB_CACHE = {}


def macro_vocabulary(target, flines):
    """Identifiers this file names as macro CALL arguments.

    Mechanical and self-maintaining: it is derived from the file as it is right
    now, so adding a GS_SAVE() line makes that member citable in the same commit
    that adds it, with nobody maintaining a list. Cached per target.

    `#define` lines are skipped, and that exclusion is load-bearing rather than
    tidiness. A macro DEFINITION names its formal parameters, which are not
    symbols anybody cites -- game_state.c:193 defines
    EFFL8_ROW_IN_RANGE(row), and admitting `row` to the vocabulary made the
    English word "row" outrank a correctly-backticked `ca_check_flag` as an
    anchor, producing a confident and wrong drift report against
    docs/research-desync-deep-investigation.md:825 ("if outcome G.2 row 2").
    Call SITES name members; definitions name placeholders.
    """
    if target in _MACRO_VOCAB_CACHE:
        return _MACRO_VOCAB_CACHE[target]
    vocab = set()
    for ln in flines:
        if RE_DEFINE_LINE.match(ln):
            continue
        for m in RE_MACRO_ARG.finditer(ln):
            vocab.add(m.group(1))
    _MACRO_VOCAB_CACHE[target] = vocab
    return vocab


def load_allowlist(path):
    allowed = {}
    if not os.path.isfile(path):
        return allowed
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            tok, _, reason = line.partition("#")
            tok = tok.strip()
            if tok:
                allowed[tok] = reason.strip() or "(no reason given)"
    return allowed


# ---------------------------------------------------------------------------
# Findings
# ---------------------------------------------------------------------------

class Finding:
    def __init__(self, code, severity, path, line, message, evidence=None,
                 suggestion=None, range_cite=False):
        self.code = code
        self.severity = severity  # "error" | "advisory"
        self.path = path
        self.line = line
        self.message = message
        self.evidence = evidence or []
        self.suggestion = suggestion
        # True only for a "drift" finding on a LINE-RANGE citation
        # (`file.c:522-566`). --fix refuses these -- see apply_fixes.
        self.range_cite = range_cite

    def key(self):
        return (self.path, self.line, self.code, self.message)

    def as_dict(self):
        return {
            "code": self.code, "severity": self.severity, "path": self.path,
            "line": self.line, "message": self.message,
            "evidence": self.evidence, "suggestion": self.suggestion,
            "range_cite": self.range_cite,
        }

    def render(self):
        head = "%s:%d: %s [%s] %s" % (
            self.path, self.line, self.severity.upper(), self.code, self.message)
        body = "".join("\n      %s" % e for e in self.evidence)
        if self.suggestion:
            body += "\n      fix: %s" % self.suggestion
        return head + body


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def anchor_tokens(unit, cite_off, cite_text, extra_vocab=None):
    """Identifier-shaped tokens belonging to THIS citation, best evidence first.

    Backticked tokens rank above bare ones: an author who backticked it was
    naming a symbol on purpose. Tokens that are part of the cited path itself
    are excluded -- `effl8` is not evidence about effl8.c.

    OWNERSHIP. A sentence or table row routinely carries several citations:

        | `Bg_Close` at `bg.c:311` | `bg_work_clear` at `bg.c:555` |  # doccite:quote

    Taking every token in the window as an anchor for every citation lets one
    citation borrow its neighbour's subject and produce a confident, wrong
    drift report. Measured on a hand-audited sample, that was the single
    largest false-positive source. So each token is assigned to the citation it
    sits closest to, and a citation only gets the tokens that chose it.
    """
    if unit.path.endswith(".md"):
        # A markdown line (or table row) is the natural unit, widened by
        # MD_ANCHOR_WINDOW_LINES on each side. KNOWN LIMITATION this widening
        # fixes: an author hand-wraps a sentence or a wide table cell across
        # lines, and the symbol backtick lands one line away from its own
        # `file.c:LINE` citation. A same-line-only window then sees zero
        # anchor tokens for a citation that is actually correct, which stayed
        # SILENT (no anchor anywhere -> no finding) when the real target line
        # happened to also look non-degenerate, and reported spurious
        # drift/degenerate-target when it didn't -- a human had to manually
        # reflow the prose onto one line to make the checker agree it was
        # correct. Widening the window a LITTLE is a cheap fix: OWNERSHIP
        # (below) still assigns each token to its nearest citation by
        # position, so a wider window does not let a neighbouring table row's
        # subject bleed onto this citation -- it only admits more candidates
        # to compete for. It is deliberately not widened to the ±400-char
        # block window the comment branch uses below: a markdown table's rows
        # are short and dense, so a wide window would pull in several
        # neighbouring rows' worth of tokens, and OWNERSHIP nearest-wins
        # heuristic gets less reliable the more competing citations are in
        # play. A fully robust fix (e.g. parsing table cell/sentence
        # boundaries) was judged not cheap and is left for a future change.
        starts = line_index(unit.text)
        cite_line = offset_to_line(starts, cite_off)
        lo_line = max(1, cite_line - MD_ANCHOR_WINDOW_LINES)
        hi_line = min(len(starts), cite_line + MD_ANCHOR_WINDOW_LINES)
        lo = starts[lo_line - 1]
        hi = starts[hi_line] if hi_line < len(starts) else len(unit.text)
    else:
        # A comment block, already merged by prose_units.
        lo = unit.text.rfind("\n", 0, max(0, cite_off - 400)) + 1
        hi = unit.text.find("\n", cite_off + 400)
        hi = hi if hi >= 0 else len(unit.text)
    window = unit.text[lo:hi]
    here = cite_off - lo

    cites = [m.start() for m in RE_PATH_CITE.finditer(window)]
    if here not in cites:
        cites.append(here)

    def owns(pos):
        """True if this citation is the nearest one to a token at pos."""
        return min(cites, key=lambda c: (abs(c - pos), c)) == here

    path_words = set(RE_WORD.findall(cite_text))
    ranked = []
    nearby = set()
    for m in RE_BACKTICK.finditer(window):
        for t in unwrap_identifiers(m.group(1)):
            if is_symbol_candidate(t, extra_vocab) and t not in path_words:
                nearby.add(t)
                if owns(m.start()):
                    ranked.append((0, -len(t), t))
    for m in RE_WORD.finditer(window):
        t = m.group(0)
        if is_symbol_candidate(t, extra_vocab) and t not in path_words:
            nearby.add(t)
            if owns(m.start()):
                ranked.append((1, -len(t), t))
    seen, out = set(), []
    for _, _, t in sorted(ranked):
        if t not in seen:
            seen.add(t)
            out.append(t)
    # Asymmetric on purpose. ACCUSING a citation of drift uses only the tokens
    # that chose it (`out`); EXONERATING it uses every symbol in the window
    # (`nearby`). texgroup.c:287 reads "the purge_texture_group(group_num)  # doccite:quote
    # re-entry inside Push_ramcnt_key_original_2 (ramcnt.c:102)": the nearest  # doccite:quote
    # token to the citation is Push_ramcnt_key_original_2, but line 102 really
    # is the purge_texture_group re-entry, so the citation is correct. Being
    # generous about what counts as a hit and strict about what counts as
    # evidence of a miss is what keeps this from crying wolf.
    return out, nearby


def check_path_cites(repo, unit, findings, seen_paths, history, dclass,
                     external_globs, anchor_required_globs=()):
    text = unit.text

    def unresolved(cited, line_no, how):
        """Classify a path that does not resolve. See History's docstring for
        why deleted-then-cited and never-existed are not the same finding."""
        ever = history.has_path(cited)
        if ever:
            findings.append(Finding(
                "stale-path", "advisory", unit.path, line_no,
                "cited path no longer exists: %s" % cited,
                evidence=["this path did exist in history and was deleted, so "
                          "the citation is a historical record, not a "
                          "fabrication"]))
            return
        sev = PHANTOM_SEVERITY if dclass == "reference" else "advisory"
        cands = repo.by_base.get(os.path.basename(cited)) or []
        if cands:
            findings.append(Finding(
                "wrong-path", sev, unit.path, line_no,
                "cited path has never existed: %s" % cited,
                evidence=["no commit on any ref has contained it",
                          "a file with that basename does exist: "
                          + ", ".join(sorted(cands)[:4])],
                suggestion="%s -> %s" % (cited, sorted(cands)[0])))
        else:
            findings.append(Finding(
                "phantom-path", sev, unit.path, line_no,
                "cited path has never existed: %s" % cited,
                evidence=["no commit on any ref has contained this path, and "
                          "no tracked file shares its basename"]))

    def handle(cited, lo, hi, mstart, matched_text):
        # "mtrans.c/texcash.c" names TWO files with a slash between them; it is
        # not one path. Any non-final component carrying a source extension
        # means shorthand, not a directory. Measured as a top false-positive
        # source for phantom-path.
        parts = cited.split("/")
        if len(parts) > 1 and any(
                p.rsplit(".", 1)[-1].lower() in CHECKED_EXTS
                for p in parts[:-1] if "." in p):
            return
        # "a/src/netplay/netplay.c" is a git-diff hunk prefix.
        if parts[0] in ("a", "b") and len(parts) > 2:
            return
        target, how = repo.resolve(cited)
        line_no = unit.line_of(mstart)

        if how in ("missing", "basename"):
            ext = cited.rsplit(".", 1)[-1].lower()
            if ext not in CHECKED_EXTS:
                return  # a build output, not a tracked source
            norm = cited.lstrip("./")
            if any(norm.startswith(p) for p in UNTRACKED_BY_DESIGN):
                return
            # third_party/GekkoNet/build/include/net.h -- a vendored library's
            # generated output. "build/" has to match as a segment anywhere,
            # not only at the front.
            if any(seg in ("build", "out", "dist", "output_files")
                   for seg in norm.split("/")[:-1]):
                return
            if any(fnmatch.fnmatch(norm, g) for g in external_globs):
                return  # deliberately points outside this repository
            unresolved(cited, line_no, how)
            return
        if how == "ambiguous" or target is None:
            return

        seen_paths.add(target)
        if lo is None:
            return

        flines = repo.lines(target)
        if flines is None:
            return
        nlines = len(flines)
        if flines and flines[-1] == "":
            nlines -= 1
        hi_eff = hi if hi is not None else lo
        if lo < 1 or hi_eff > nlines:
            findings.append(Finding(
                "line-out-of-range", "error", unit.path, line_no,
                "cited %s:%s but the file has %d lines"
                % (target, ("%d-%d" % (lo, hi)) if hi else str(lo), nlines)))
            return

        def degenerate():
            """Anchor-free drift signal: the cited line is one nobody cites on
            purpose. texgroup.c:440 is a lone "}", and the citation that landed  # doccite:quote
            there meant :477-479. Checked only AFTER the anchor path, because
            when an anchor exists it names the correct line and this does not.
            """
            span = flines[lo - 1:hi_eff]
            if not span or not all(RE_DEGENERATE.match(x) for x in span):
                return False
            findings.append(Finding(
                "degenerate-target", "error", unit.path, line_no,
                "cited %s:%s, which is not a citable line"
                % (target, ("%d-%d" % (lo, hi)) if hi else str(lo)),
                evidence=["%s:%d is: %s"
                          % (target, lo, flines[lo - 1].strip() or "(blank)")]))
            return True

        # ANCHOR-REQUIRED FILES (task #110).
        #
        # A citation with no anchor is unfalsifiable: `game_state.c:580` claims  # doccite:quote
        # nothing a checker can test, so it is accepted forever no matter how
        # far the file moves underneath it. When select_timer_state was removed
        # from GameState, ~136 such citations shifted tree-wide and the linter
        # reported none of them -- and spot-checks then showed several had been
        # wrong even before the shift.
        #
        # For most of the tree that silence is the right trade: demanding an
        # anchor everywhere would bury real findings under thousands of
        # unactionable ones. But for a macro table on the rollback save/load
        # path it is the worst possible place to be blind. game_state.c is
        # 2,653 lines of 609 GS_SAVE/GS_LOAD pairs; every save-set change moves
        # everything below it, and a wrong line number here sends a future
        # desync investigation to the wrong member.
        #
        # So files listed in anchor-required.txt are held to a higher standard:
        # cite something checkable, or be reported. Note the ORDER -- the
        # vocabulary is computed first so that short member names like `bg_w`
        # can anchor at all (see is_symbol_candidate); reporting a citation as
        # unanchored when the linter simply refused to look at its anchor would
        # be a fabricated finding.
        must_anchor = any(fnmatch.fnmatch(target, g)
                          for g in anchor_required_globs)
        vocab = macro_vocabulary(target, flines) if must_anchor else None
        anchors, nearby = anchor_tokens(unit, mstart, matched_text, vocab)
        if not anchors:
            if degenerate():
                return
            if must_anchor:
                findings.append(Finding(
                    "unanchored-citation", "error", unit.path, line_no,
                    "cited %s:%s with nothing to anchor against"
                    % (target, ("%d-%d" % (lo, hi)) if hi else str(lo)),
                    evidence=[
                        "%s:%d is: %s"
                        % (target, lo, flines[lo - 1].strip()[:110] or "(blank)"),
                        "this file is anchor-required (see "
                        "tools/doc-citations/anchor-required.txt): a bare "
                        "line number here cannot be checked and cannot be "
                        "repaired mechanically",
                        "name the member or symbol the line is supposed to "
                        "hold, e.g. GS_SAVE(bg_w), so the claim becomes "
                        "falsifiable",
                    ]))
            return
        cited_body = "\n".join(flines[lo - 1:hi_eff])
        cited_words = set(RE_WORD.findall(cited_body))
        if nearby & cited_words:
            return  # the cited line names something the prose is discussing

        # No anchor on the cited line. Only claim drift if we can EXHIBIT the
        # anchor somewhere else in the same file. Otherwise stay silent.
        # code_lines mirrors flines 1:1 with comment bytes blanked out (or
        # is None if this extension has no decommenter -- see
        # code_only_lines). Used below to prefer a hit that is real code
        # over one that only appears inside a comment/prose mention: a
        # popular identifier gets discussed in many comments but
        # defined/used in only a handful of real lines, and a citation is,
        # with rare exception, about the latter. Picking the numerically
        # NEAREST hit without this preference is what let `--fix` collapse
        # several distinct citations onto one heavily-commented line (e.g.
        # direct_p2p.c's algorithm-description blocks, which restate
        # `race_budget_ms` / `STUN_PUNCH_CONFIRM_MS` a dozen times each).
        code_lines = repo.code_only_lines(target)
        best = None
        for a in anchors:
            pattern = re.compile(r"\b%s\b" % re.escape(a))
            hits = [i + 1 for i, ln in enumerate(flines) if pattern.search(ln)]
            if not hits:
                continue
            if code_lines is not None:
                code_hits = [h for h in hits if pattern.search(code_lines[h - 1])]
                # A symbol that ONLY ever appears in comments (a concept
                # discussed but not (yet) named in code) has no code hit to
                # prefer -- fall back to the full set rather than going
                # anchor-less, same as before this change.
                if code_hits:
                    hits = code_hits
            # ANCHOR_MAX_HITS is applied AFTER the code-hit narrowing, not
            # before. Applied to the raw count it discards exactly the symbols
            # this checker most needs to anchor: a well-commented one. Measured
            # 2026-09-07 -- `try_portmap` in src/netplay/direct_p2p.c has 20 raw
            # hits (over the cap, so it was dropped and its citation silently
            # exonerated) but only 2 code hits, comfortably under it. Six
            # citations in docs/plan-netplay-connection.md were invisible to the
            # gate that way, four of them genuinely wrong.
            if len(hits) > ANCHOR_MAX_HITS:
                continue
            hits.sort(key=lambda h: abs(h - lo))
            if best is None or len(hits) < len(best[1]):
                best = (a, hits)
        if best is None:
            degenerate()
            return
        a, hits = best
        shown = hits[:3]
        # RANGE CITATIONS. The anchor only locates ONE line (hits[0]) -- it
        # is evidence about where `a` moved TO, not about how the citation's
        # far end should move. `--fix` used to rewrite only the range's
        # START with that single line and leave the OLD END untouched:
        # `file.c:522-566` -> `file.c:2360-566`, a backwards range pointing
        # nowhere, applied silently (task #84). There is no anchor for the
        # end of a range, so there is no coherent way to rewrite both ends
        # mechanically -- guessing (e.g. shifting the end by the same delta
        # as the start) assumes the intervening lines moved uniformly, which
        # a later insertion/deletion inside the range can easily violate.
        # So: refuse. Report the finding, with the one thing that IS known
        # (where the anchor now is), and let a human decide the new range.
        is_range = hi is not None
        if is_range:
            suggestion = (
                "RANGE CITATION -- refused by --fix, repair manually: "
                "anchor `%s` is now at %s:%d (cited range was %s:%d-%d)"
                % (a, target, hits[0], target, lo, hi))
        else:
            suggestion = "%s:%d -> %s:%d" % (target, lo, target, hits[0])
        findings.append(Finding(
            "drift", "error", unit.path, line_no,
            "cited %s:%s for `%s`, but that line does not mention it"
            % (target, ("%d-%d" % (lo, hi)) if hi else str(lo), a),
            evidence=[
                "%s:%d is: %s" % (target, lo, flines[lo - 1].strip()[:110]),
                "`%s` is at %s" % (a, ", ".join("%s:%d" % (target, h) for h in shown)),
            ],
            suggestion=suggestion,
            range_cite=is_range))

    for m in RE_PATH_CITE.finditer(text):
        cited = m.group(1)
        lo = int(m.group(2)) if m.group(2) else None
        hi = int(m.group(3)) if m.group(3) else None
        handle(cited, lo, hi, m.start(), m.group(0))
        # `texcash.c:361/576/614` -- check the trailing numbers too.
        if lo is not None and hi is None:
            pos = m.end()
            while True:
                em = RE_EXTRA_LINES.match(text, pos)
                if not em:
                    break
                handle(cited, int(em.group(1)), None, m.start(), m.group(0))
                pos = em.end()

    # Bare filename + line number, anywhere: `texgroup.c:477-479`. This is the
    # native citation style of this repo's in-tree comments.
    for m in RE_BARE_CITE.finditer(text):
        lo = int(m.group(2))
        hi = int(m.group(3)) if m.group(3) else None
        handle(m.group(1), lo, hi, m.start(), m.group(0))
        if hi is None:
            pos = m.end()
            while True:
                em = RE_EXTRA_LINES.match(text, pos)
                if not em:
                    break
                handle(m.group(1), int(em.group(1)), None, m.start(),
                       m.group(0))
                pos = em.end()

    # A backticked bare filename with NO line number: existence only.
    for m in RE_BACKTICK.finditer(text):
        payload = m.group(1).strip()
        if "/" in payload:
            continue  # already covered by RE_PATH_CITE
        bm = RE_BARE_FILE.match(payload)
        if not bm or bm.group(2):
            continue  # with a line number it was handled by RE_BARE_CITE
        handle(bm.group(1), None, None, m.start(), payload)


def check_identifiers(repo, unit, findings, allowed, stats, history, dclass):
    toks = repo.code_tokens()
    for m in RE_BACKTICK.finditer(unit.text):
        payload = m.group(1)
        if "/" in payload or " " in payload:
            continue
        for tok in unwrap_identifiers(payload):
            if not is_symbol_candidate(tok):
                continue
            stats["ident_checked"] += 1
            if tok in toks or tok in allowed:
                continue
            rem = history.removal(tok)
            if rem is not None and rem.kind != "comments-only":
                message, evidence = history.describe(tok, rem)
                findings.append(Finding(
                    "stale-identifier", "advisory", unit.path,
                    unit.line_of(m.start()), message, evidence=evidence))
                continue
            # A true phantom. Say exactly what was searched: the second clause
            # used to claim "every historical revision of every source file"
            # while the corpus skipped every file still in the tree, and that
            # confidently-wrong text sent a reader to rewrite a document that
            # was correct.
            if not history.enabled:
                evidence = ["absent from every tracked file with comments "
                            "stripped; git history NOT consulted (--no-history), "
                            "so this may be deleted code rather than a fabrication"]
            elif rem is not None:
                shown = ", ".join(rem.paths[:3]) + (", ..." if len(rem.paths) > 3 else "")
                evidence = ["absent from every tracked file with comments "
                            "stripped, and from every historical revision of "
                            "every source file as code: history names it only "
                            "inside comments (%s)" % shown]
            else:
                evidence = ["absent from every tracked file with comments "
                            "stripped, and from every historical revision of "
                            "every source file: only prose asserts this symbol"]
            findings.append(Finding(
                "phantom-identifier",
                PHANTOM_SEVERITY if dclass == "reference" else "advisory",
                unit.path,
                unit.line_of(m.start()),
                "`%s` is referenced but has never existed" % tok,
                evidence=evidence,
                suggestion="correct the name, or add it to "
                           "tools/doc-citations/allowlist.txt with a reason"))


def check_evidence_refs(repo, unit, findings):
    for line_rel, line in enumerate(unit.text.split("\n")):
        for m in RE_EVIDENCE_REF.finditer(line):
            name = m.group(1).strip("`\"' ")
            noun = m.group(2)
            # Only fire on a NAMED artifact: a qualifier carrying a digit or an
            # internal capital. "the table below" must not fire.
            if not re.search(r"\d", name) and not re.search(r"[A-Z]", name[1:]):
                continue
            if re.search(r"\.(md|txt|log|json|csv|pdf)\b", line):
                continue
            if RE_PATH_CITE.search(line) or re.search(r"\]\([^)]+\)", line):
                continue
            if re.search(r"\b(section|chapter|figure|table|step|phase)\b",
                         name, re.IGNORECASE):
                continue
            abs_line = unit.line_of(sum(len(x) + 1 for x in
                                        unit.text.split("\n")[:line_rel]))
            findings.append(Finding(
                "unresolvable-evidence", "advisory", unit.path, abs_line,
                'appeals to "%s %s" but names no file anywhere in the sentence'
                % (name, noun),
                evidence=[line.strip()[:150]],
                suggestion="cite the artifact by path, or state the evidence "
                           "inline"))


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def scan_targets(repo, extra_roots=None):
    out = []
    for p in repo.files:
        if any(p.startswith(d) for d in SKIP_SCAN_DIRS):
            if not (extra_roots and any(p.startswith(r) for r in extra_roots)):
                continue
        ext = os.path.splitext(p)[1]
        if ext == ".md":
            if p.startswith("docs/") or "/" not in p or (
                    extra_roots and any(p.startswith(r) for r in extra_roots)):
                out.append(p)
            continue
        if ext in CODE_PROSE_EXTS and (
                p.startswith("src/") or p.startswith("include/")
                or p.startswith("tools/")
                or (extra_roots and any(p.startswith(r) for r in extra_roots))):
            out.append(p)
    return sorted(set(out))


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Check that citations in docs and comments still resolve.")
    ap.add_argument("--root", default=REPO_ROOT)
    ap.add_argument("paths", nargs="*",
                    help="limit the scan to these repo-relative paths/prefixes")
    ap.add_argument("--checks", default="c1,c2,c3,c4",
                    help="comma list of c1(file:line) c2(identifiers) "
                         "c3(paths) c4(evidence refs). c1 and c3 share a pass.")
    ap.add_argument("--errors-only", action="store_true",
                    help="suppress advisories")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--fix", action="store_true",
                    help="apply unambiguous line-number and path corrections "
                         "in place; never touches prose; refuses range "
                         "citations (file.c:LO-HI) and reports them for "
                         "manual repair instead of guessing a new end")
    ap.add_argument("--baseline", default=None,
                    help="a baseline JSON; only findings absent from it are "
                         "reported as errors")
    ap.add_argument("--write-baseline", default=None)
    ap.add_argument("--allowlist",
                    default=os.path.join(SCRIPT_DIR, "allowlist.txt"))
    ap.add_argument("--external-paths",
                    default=os.path.join(SCRIPT_DIR, "external-paths.txt"),
                    help="globs for paths that deliberately point outside this "
                         "repository and so cannot be resolved here")
    ap.add_argument("--record-globs",
                    default=os.path.join(SCRIPT_DIR, "record-documents.txt"),
                    help="globs for RECORD-class documents (proposals and "
                         "superseded write-ups) where a not-yet-existing "
                         "name is the point rather than a defect")
    ap.add_argument("--anchor-required",
                    default=os.path.join(SCRIPT_DIR, "anchor-required.txt"),
                    help="globs for files where a citation MUST name something "
                         "checkable; a bare file:LINE into one of these is an "
                         "error rather than silently accepted")
    ap.add_argument("--include-testdata", action="store_true",
                    help="also scan tools/doc-citations/testdata (fixtures)")
    ap.add_argument("--no-history", action="store_true",
                    help="skip the git-history corpus (~4s to build, ~0.4s "
                         "from its cache in the git common dir). Without it "
                         "a deleted-code citation cannot be told apart from "
                         "a fabricated one, so everything reports as "
                         "phantom, with evidence text that says history was "
                         "not consulted.")
    ap.add_argument("--no-history-cache", action="store_true",
                    help="rebuild the history index instead of loading the "
                         "cached one, and do not write a cache")
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root)
    if not os.path.isdir(os.path.join(root, ".git")) and not os.path.exists(
            os.path.join(root, ".git")):
        sys.stderr.write("not a git repository: %s\n" % root)
        return 2

    try:
        repo = Repo(root)
    except subprocess.CalledProcessError as exc:
        sys.stderr.write("git ls-files failed: %s\n" % exc)
        return 2

    checks = {c.strip() for c in args.checks.split(",") if c.strip()}
    allowed = load_allowlist(args.allowlist)
    history = History(root, enabled=not args.no_history,
                      use_cache=not args.no_history_cache)
    record_globs = load_record_globs(args.record_globs)
    external_globs = load_record_globs(args.external_paths)
    anchor_required_globs = load_record_globs(args.anchor_required)

    extra = ["tools/doc-citations/testdata"] if args.include_testdata else None
    targets = scan_targets(repo, extra)
    if args.paths:
        pfx = tuple(p.rstrip("/") for p in args.paths)
        targets = [t for t in targets
                   if t in pfx or any(t.startswith(p + "/") for p in pfx)]

    findings = []
    seen_paths = set()
    stats = defaultdict(int)
    for relpath in targets:
        dclass = doc_class(relpath, record_globs)
        for unit in prose_units(repo, relpath):
            if "c1" in checks or "c3" in checks:
                check_path_cites(repo, unit, findings, seen_paths, history,
                                 dclass, external_globs,
                                 anchor_required_globs)
            if "c2" in checks:
                check_identifiers(repo, unit, findings, allowed, stats, history, dclass)
            if "c4" in checks:
                check_evidence_refs(repo, unit, findings)

    if "c1" not in checks:
        findings = [f for f in findings
                    if f.code not in ("drift", "line-out-of-range",
                                      "unanchored-citation")]
    if "c3" not in checks:
        findings = [f for f in findings
                    if f.code not in ("missing-path", "wrong-path")]

    # QUOTING vs ASSERTING. A line may carry the marker below to say "the
    # citation on this line is being quoted, not made". This file needs it:
    # its own docstring shows what a broken citation looks like, and without a
    # way to say so the linter reports its own worked examples as defects.
    #
    # The marker means quoted. It does not mean "ignore this" -- silencing a
    # citation you actually made is what the allowlists are for, and those
    # demand a written reason. Keep it rare; grep for it in review.
    def quoted(f):
        lines = repo.lines(f.path)
        if not lines or not (1 <= f.line <= len(lines)):
            return False
        return QUOTE_MARKER in lines[f.line - 1]

    findings = [f for f in findings if not quoted(f)]
    findings.sort(key=lambda f: (f.path, f.line, f.code))
    history.save()

    baseline_keys = set()
    if args.baseline and os.path.isfile(args.baseline):
        with open(args.baseline, "r", encoding="utf-8") as fh:
            for d in json.load(fh).get("findings", []):
                baseline_keys.add((d["path"], d["line"], d["code"], d["message"]))

    if args.write_baseline:
        with open(args.write_baseline, "w", encoding="utf-8") as fh:
            json.dump({"findings": [f.as_dict() for f in findings]}, fh,
                      indent=1, sort_keys=True)
            fh.write("\n")
        sys.stderr.write("wrote baseline: %s (%d findings)\n"
                         % (args.write_baseline, len(findings)))

    fixed = 0
    range_skipped = 0
    if args.fix:
        fixed, range_skipped = apply_fixes(repo, findings)

    shown = [f for f in findings
             if not (args.errors_only and f.severity == "advisory")]
    new_errors = [f for f in findings
                  if f.severity == "error" and f.key() not in baseline_keys]

    if args.json:
        print(json.dumps({
            "findings": [f.as_dict() for f in shown],
            "counts": counts_of(findings),
            "files_scanned": len(targets),
            "new_errors": len(new_errors),
        }, indent=1, sort_keys=True))
    else:
        for f in shown:
            print(f.render())
            print("")

    c = counts_of(findings)
    # In --json mode the verdict goes to stderr so stdout stays parseable.
    summary = ("DOCCITE SUMMARY: findings=%d errors=%d advisories=%d "
               "new_errors=%d files_scanned=%d fixed=%d range_refused=%d %s"
               % (len(findings), c["error"], c["advisory"], len(new_errors),
                  len(targets), fixed, range_skipped,
                  " ".join("%s=%d" % kv for kv in sorted(c["by_code"].items()))))
    print(summary, file=sys.stderr if args.json else sys.stdout)

    return 1 if new_errors else 0


def counts_of(findings):
    by_code = defaultdict(int)
    sev = defaultdict(int)
    for f in findings:
        by_code[f.code] += 1
        sev[f.severity] += 1
    return {"error": sev["error"], "advisory": sev["advisory"],
            "by_code": dict(by_code)}


def apply_fixes(repo, findings):
    """Apply only mechanical corrections: a line number or a path spelling.

    Never rewrites prose. A finding whose suggestion is not an exact textual
    substitution present on the cited line is skipped.

    RANGE CITATIONS ARE REFUSED, not fixed. A drift finding on a `file.c:LO-HI`
    citation carries range_cite=True and a suggestion that is deliberately NOT
    in the "old -> new" mechanical-substitution shape apply_fixes elsewhere
    depends on (see Finding.range_cite's docstring at its construction site).
    Skipping them here is a second, independent guard on top of that shape
    mismatch: even if a future suggestion string for a range finding
    accidentally looked like "old -> new", the range_cite flag alone is
    checked first and stops it before the regex substitution runs.

    Returns (fixed_count, range_skipped_count).
    """
    edits = defaultdict(list)
    range_skipped = 0
    for f in findings:
        if f.code not in ("drift", "wrong-path") or not f.suggestion:
            continue
        if f.range_cite:
            range_skipped += 1
            continue
        old, _, new = f.suggestion.partition(" -> ")
        edits[f.path].append((f.line, old.strip(), new.strip()))
    n = 0
    for path, items in edits.items():
        full = os.path.join(repo.root, path)
        with open(full, "r", encoding="utf-8") as fh:
            lines = fh.read().split("\n")
        changed = False
        for line_no, old, new in items:
            if not (1 <= line_no <= len(lines)):
                continue
            # Match the cited path suffix so a doc that writes the path without
            # its src/ prefix is still rewritten correctly.
            oldp, _, oldl = old.rpartition(":")
            newp, _, newl = new.rpartition(":")
            if oldp == newp and oldl.isdigit() and newl.isdigit():
                pat = re.compile(r"((?:[\w.+-]+/)*%s):%s\b"
                                 % (re.escape(os.path.basename(oldp)),
                                    re.escape(oldl)))
                repl, k = pat.subn(lambda m: "%s:%s" % (m.group(1), newl),
                                   lines[line_no - 1])
            else:
                repl, k = lines[line_no - 1].replace(old, new), \
                    lines[line_no - 1].count(old)
            if k:
                lines[line_no - 1] = repl
                changed = True
                n += k
        if changed:
            with open(full, "w", encoding="utf-8") as fh:
                fh.write("\n".join(lines))
    return n, range_skipped


if __name__ == "__main__":
    sys.exit(main())
