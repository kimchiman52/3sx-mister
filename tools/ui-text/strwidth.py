#!/usr/bin/env python3
"""Measure the on-screen width of proportional-font text.

Mirrors sc_sub.c -> SSGetDrawSizePro exactly: each glyph advances
8 - sideL - sideR pixels, where the two trims come from the ascProData
table. The table is parsed out of sc_sub.c at run time, so this tool cannot
go stale independently of the code it measures.

    python3 tools/ui-text/strwidth.py --selftest
    python3 tools/ui-text/strwidth.py "PRESS ANY BUTTON" "another string"
    printf '%s\n' line1 line2 | python3 tools/ui-text/strwidth.py

Proportional text only (SSPutStrPro / SSPutStrProP). The cell-based paths
(SSPutStr, scfont_*, score8x16_put) advance 8 px per glyph and are anchored
in 8 px columns; do not measure those with this. Budgets and the audit this
tool was written for: docs/ui-text-width.md.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src", "sf33rd", "Source", "Game", "ui", "sc_sub.c")

# Same strings the task brief fixed as the tool's self-check: the refusal line
# that overflowed a user's screen, the overlay's ERROR label, and the title
# prompt. If ascProData ever changes these change with it -- on purpose.
SELFTEST = {
    "Netplay needs arcade balance - CPS3 ROM not found or failed content verification": 560,
    "ERROR": 40,
    "PRESS ANY BUTTON": 120,
}


def load_table(path=SRC):
    with open(path) as f:
        src = f.read()
    m = re.search(r"const u8 ascProData\[128\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if m is None:
        raise SystemExit(f"ascProData[128] not found in {path}")
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})\s*,", m.group(1))]
    if len(vals) != 128:
        raise SystemExit(f"expected 128 ascProData entries, got {len(vals)}")
    return vals


ASC = load_table()


def width(s):
    """Pixel width of s under SSGetDrawSizePro (ix &= 0x7F, then the trims)."""
    total = 0
    for ch in s:
        ix = ord(ch) & 0x7F
        total += 8 - ((ASC[ix] >> 4) & 0xF) - (ASC[ix] & 0xF)
    return total


def selftest():
    ok = True
    for s, expect in SELFTEST.items():
        got = width(s)
        flag = "ok " if got == expect else "BAD"
        if got != expect:
            ok = False
        print(f"{flag} {got:4d}px (expected {expect}) {s!r}")
    return ok


if __name__ == "__main__":
    args = sys.argv[1:]
    if args == ["--selftest"]:
        sys.exit(0 if selftest() else 1)
    for line in args or [l.rstrip("\n") for l in sys.stdin]:
        print(f"{width(line):4d}px  {line!r}")
