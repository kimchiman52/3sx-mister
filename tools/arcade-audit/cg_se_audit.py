#!/usr/bin/env python3
"""cg_se sound-code remap audit -- doc item Q (§8.Q / §21).

Asserts, against the decrypted ROM, that the per-character cg_se remap in
src/arcade/arcade_char_data.c (cg_se_maps[] / remap_cg_se) touches EXACTLY
the six divergences of doc §21.7 -- 29 script cells, no more, no fewer --
and that the codes it must NOT touch are indeed outside every pair table.

Two denominators, both asserted:

  script cells      one per (table, script) reference, the way §21.7 and
                    cg_audit.py count -- 29 total;
  buffer positions  distinct u16 slots in the parsed images read_char_table
                    actually rewrites -- 17, fewer than 29 because duplicate
                    script offsets share one cell body (e.g. Alex's
                    yuca[8]/[10]/[12]/[14] are four entries onto one body).

Like the sibling audits, this script imports cg_audit.py for the shared
constants/parsers and does not modify it; the pair tables are PARSED FROM
SOURCE, so editing cg_se_maps[] changes this audit's answer automatically.
Exits 0 only when every assertion holds.
"""
import json, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cg_audit as A

# ---------------------------------------------------------------- expected (doc §21.7)
# (character, from, to, script cells). THE assertion of item Q: literal on
# purpose -- if cg_se_maps[] in the C source drifts from this table, or the
# ROM stops carrying exactly these cells, the audit goes red.
EXPECTED = [
    ("MAKOTO", 0x27E, 0x1DF, 4),
    ("ALEX",   0x2FB, 0x3BF, 4),
    ("ORO",    0x2F8, 0x25C, 13),
    ("ORO",    0x3DF, 0x25D, 2),
    ("YANG",   0x1DF, 0x29E, 5),
    ("AKUMA",  0x37E, 0x130, 1),
]
EXPECTED_TOTAL_CELLS = 29
EXPECTED_TOTAL_POSITIONS = 17

# Codes that must stay OUTSIDE every pair table (per-character keying and
# the deliberate non-actions of §21.9).
MUST_NOT_REMAP = [
    ("URIEN", 0x2FB), ("TWELVE", 0x3DF),               # legitimate voices (§21.8)
    ("YANG", 0x27F), ("YUN", 0x268),                   # mod-32 no-ops
    ("YANG", 0x269),                                   # PS2 silenced; also used correctly
    ("Q", 0x108),                                      # authentic arcade silence (§21.10)
    ("RYU", 0x10A), ("KEN", 0x10A),
    ("SEAN", 0x10A), ("AKUMA", 0x10A),                 # shoto dmca[3] SE PS2 removed
]

# ---------------------------------------------------------------- parse cg_se_maps from source
def parse_cg_se_maps():
    s = A.src("src/arcade/arcade_char_data.c")
    tables = {}
    for m in re.finditer(r'static const CgSeRemapPair (\w+)\[\]\s*=\s*\{(.*?)\};', s, re.S):
        tables[m.group(1)] = [
            (int(p.group(1), 16), int(p.group(2), 16))
            for p in re.finditer(r'\{\s*\.from\s*=\s*(0x[0-9A-Fa-f]+),\s*\.to\s*=\s*(0x[0-9A-Fa-f]+)\s*\}',
                                 m.group(2))
        ]
    blk = re.search(r'static const CharacterCgSeMap cg_se_maps\[NUM_CHARS\]\s*=\s*\{(.*?)\n\};', s, re.S).group(1)
    maps = {n: [] for n in A.NAMES}
    for m in re.finditer(r'\[CHAR_(\w+)\]\s*=\s*\{\s*\.pairs\s*=\s*(\w+)', blk):
        maps[m.group(1)] = tables[m.group(2)]
    return maps

# ---------------------------------------------------------------- exact read_char_table mirror
def walk_cell_positions(ci, sec):
    """Yield (byte_pos, se) for every L cell, mirroring read_char_table
    EXACTLY (linear walk from each distinct sorted offset to the next
    strictly-greater one; NO terminator break -- unlike arc_parse, which
    breaks at TERMINATORS in a table's last script)."""
    off, size = A.LOC[ci][sec]
    ents, p = [], off
    while True:
        v = struct.unpack_from(">I", A.ROM, p)[0]; p += 4
        if v == 0:
            break
        ents.append(v - A.BASE_OFFSET - off)
    # SDL_qsort keeps duplicates; for the earlier of two equal offsets
    # end == start and the C parses nothing, so distinct starts suffice.
    so = sorted(set(ents))
    for i, o in enumerate(so):
        start, end = o - 8, (size if i == len(so) - 1 else so[i + 1] - 8)
        q, lim = off + start, off + end
        cgd = struct.unpack_from(">h", A.ROM, q)[0]
        q += 8
        while q < lim:
            code = struct.unpack_from(">H", A.ROM, q)[0]
            if code < 0x100:
                q += 8 + max(cgd * 4 - 8, 0)
            else:
                yield q, struct.unpack_from(">H", A.ROM, q + 2)[0]
                q += 8 + (8 if cgd >= 4 else 0) + (8 if cgd == 6 else 0)

def main():
    failures = []
    maps = parse_cg_se_maps()

    # 1. Source table == expected table, exactly.
    src_pairs = {(n, f, t) for n, ps in maps.items() for (f, t) in ps}
    exp_pairs = {(n, f, t) for (n, f, t, _) in EXPECTED}
    for extra in sorted(src_pairs - exp_pairs):
        failures.append(f"cg_se_maps[] carries an unexpected pair: {extra[0]} 0x{extra[1]:03X}->0x{extra[2]:03X}")
    for missing in sorted(exp_pairs - src_pairs):
        failures.append(f"cg_se_maps[] is missing a pair: {missing[0]} 0x{missing[1]:03X}->0x{missing[2]:03X}")

    # 2. The must-not-remap codes are outside every pair table.
    for name, code in MUST_NOT_REMAP:
        if any(f == code for (f, _) in maps[name]):
            failures.append(f"{name} 0x{code:03X} must NOT be remapped (doc §21.8/§21.9) but is in cg_se_maps[]")

    # 3. Count script cells (the §21.7 / cg_audit denominator) and distinct
    #    buffer positions (what read_char_table rewrites) per pair.
    cells = {k: [] for k in exp_pairs | src_pairs}
    positions = {k: set() for k in exp_pairs | src_pairs}
    script_secs = list(A.KOC2SEC.values())
    for ci, name in enumerate(A.NAMES):
        wanted = {f: t for (f, t) in maps.get(name, [])}
        if not wanted:
            continue
        tabs = {sec: A.arc_offsets(*A.LOC[ci][sec]) for sec in script_secs}
        for sec in script_secs:
            for si in range(len(tabs[sec])):
                _, acells = A.arc_parse(ci, sec, si, tabs)
                for c in acells:
                    if c[0] == 'L' and (c[1]['se'] >> 4) in wanted:
                        f = c[1]['se'] >> 4
                        cells[(name, f, wanted[f])].append((sec, si))
            for pos, se in walk_cell_positions(ci, sec):
                if (se >> 4) in wanted:
                    f = se >> 4
                    positions[(name, f, wanted[f])].add(pos)

    rows, total_cells, total_pos = [], 0, 0
    for name, f, t, want in EXPECTED:
        got = len(cells.get((name, f, t), []))
        npos = len(positions.get((name, f, t), set()))
        total_cells += got
        total_pos += npos
        rows.append(dict(character=name, arcade=f"0x{f:03X}", ps2=f"0x{t:03X}",
                         cells=got, expected_cells=want, positions=npos,
                         sites=sorted(set(cells.get((name, f, t), [])))))
        if got != want:
            failures.append(f"{name} 0x{f:03X}->0x{t:03X}: {got} script cells, expected {want}")
        print(f"{name:7s} 0x{f:03X}->0x{t:03X}  cells {got:2d}/{want:2d}  positions {npos:2d}  "
              + " ".join(f"{s}[{i}]" for s, i in sorted(set(cells.get((name, f, t), [])))))
    print(f"TOTAL script cells {total_cells} (expected {EXPECTED_TOTAL_CELLS}), "
          f"distinct buffer positions {total_pos} (expected {EXPECTED_TOTAL_POSITIONS})")
    if total_cells != EXPECTED_TOTAL_CELLS:
        failures.append(f"total script cells {total_cells} != {EXPECTED_TOTAL_CELLS}")
    if total_pos != EXPECTED_TOTAL_POSITIONS:
        failures.append(f"total buffer positions {total_pos} != {EXPECTED_TOTAL_POSITIONS}")

    out = dict(rows=rows, total_cells=total_cells, total_positions=total_pos, failures=failures)
    with open(os.path.join(A.HERE, "cg_se_audit.json"), "w") as fh:
        json.dump(out, fh, indent=1)

    if failures:
        print("\nFAIL:")
        for f in failures:
            print(f"  {f}")
        return 1
    print("PASS")
    return 0

if __name__ == "__main__":
    sys.exit(main())
