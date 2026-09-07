#!/usr/bin/env python3
"""
Arcade-vs-PS2 audit of the 13 NON-script, NON-OVCT/OVIX character-data sections.

`cg_audit.py` covers the 10 script tables (nmca..yuca) plus OVCT/OVIX coverage.
This covers the rest of the 25 sections declared in
`src/arcade/arcade_char_data.h` (CharDataSection):

    STXY MVXY SERND RICT HIIT BODA HANA CATA CAUA ATTA HOSA ATIT PROT

This is where *gameplay* inaccuracy lives and is invisible to a CG audit: a
wrong hitbox, throw position or attack property neither crashes nor corrupts a
sprite.  Upstream issue #325 ("Fix balance disparity between arcade and 3SX").

READ THIS BEFORE INTERPRETING THE OUTPUT
  Unlike `cg_number`, **none of these 13 sections is translated** — they are
  installed raw (arcade_char_data.c:515-525, and only OVCT is touched by
  Apply3SXRenderingConventions).  So PS2 is *not* an oracle for correctness
  here; an arcade-vs-PS2 content difference is the balance change itself, which
  is the point of arcade balance.  What *would* be a defect is:
    (1) a structural misread   — wrong element size / wrong layout model;
    (2) a bounds hazard        — an index the arcade data can produce that
                                 exceeds the arcade section's own length;
    (3) an over-declared span  — location_data[] size past the real data
                                 (feeds ArcadeCharData_ComputeDigest).
  Every bounds hit therefore also carries the discriminator the CG audit
  learned: if the *same cell* on the PS2 side produces the same index, the
  hazard is a pre-existing property of the shipped PS2 build, not an
  adaptation defect.

WHAT IT DOES
  1. Decodes each arcade section out of `rom.bin` exactly as
     `arcade_char_data.c` does (same reader per section, same byte order).
  2. Decodes the PS2 counterpart out of `SF33RD.AFS`
     (entry texgrpdat[char+1].apfn, tail at .to_chd, 25-u32 section header).
  3. Compares element-wise, with RICT handled by its real layout
     ([group][24 opponents] arcade vs [group][20] PS2 — see rict_model()).
  4. Runs the bounds analysis for every indexed section:
       cg_att_ix/cg_hit_ix -> ATIT / HIIT      (charset.c:2699-2705, :2944)
       HIIT.boix/bhix+haix/caix/cuix/atix/hoix -> BODA/HANA/CATA/CAUA/ATTA/HOSA
                                                  (charset.c:2976-2981)
       cg_add_xy, cg_effect==exec_char_asxy -> STXY
                                                  (charset.c:2678, effect.c:443-451)
       cg_se & 0x800            -> SERND        (charset.c:2723-2727)
       cg_rival                 -> RICT         (charset.c:2736, :2900)

CONSTANTS ARE DERIVED, NOT HAND-COPIED
  - location_data[] / cg_maps[] / texgrpdat[] / TERMINATORS: via cg_audit.py,
    which this script imports (cg_audit is NOT modified).
  - element sizes: `section_element_sizes[]` is parsed from
    arcade_char_data.c for each section's C type, then the real sizeof() comes
    from compiling a probe against include/structs.h.
  - CHAR_3SX_TO_ARCADE: parsed from src/constants.h.
  - exec_char_asxy's effinitjptbl slot: parsed from effxx.c.

Outputs `data_audit.json` beside this script, plus a human-readable report.
"""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cg_audit as CG  # noqa: E402  (parses all the shared constants at import)

HERE = CG.HERE
REPO = CG.REPO
ROM = CG.ROM
NAMES = CG.NAMES
SECTIONS = CG.SECTIONS
LOC = CG.LOC
BASE_OFFSET = CG.BASE_OFFSET

TARGETS = ['stxy', 'mvxy', 'sernd', 'rict', 'hiit', 'boda', 'hana',
           'cata', 'caua', 'atta', 'hosa', 'atit', 'prot']

# arcade_char_data.c:515-525 — the reader ArcadeCharData_Init uses per section.
# This decides the arcade byte-order transform and nothing else.
READERS = {
    'stxy': 's16', 'mvxy': 's16', 'sernd': 'sernd', 'rict': 'catch',
    'hiit': 'u16', 'boda': 's16', 'hana': 's16', 'cata': 's16', 'caua': 's16',
    'atta': 's16', 'hosa': 's16', 'atit': 'u8', 'prot': 's16',
}

# Field names, for readable diffs. Each cites the struct it decodes.
HIIT_F = ['boix', 'bhix', 'haix', 'mf', 'caix', 'cuix', 'atix', 'hoix']   # UNK_0, structs.h:66-79
ATIT_F = ['reaction', 'level', 'mkh_ix', 'but_ix', 'dipsw', 'guard', 'dir', 'free',
          'pow', 'impact', 'piyo', 'ng_type', 'hs_me', 'hs_you', 'hit_mark', 'dmg_mark']  # UNK_7, :101-122
RICT_F = ['catch_hos_x', 'catch_hos_y', 'catch_prio', 'catch_flip', 'catch_nix']  # CatchTable, :142-148


# ------------------------------------------------------------------ element sizes
def parse_section_element_types():
    """section_element_sizes[] (arcade_char_data.c:825-851) -> {section: C type}."""
    s = CG.src("src/arcade/arcade_char_data.c")
    i = s.index("static const size_t section_element_sizes[CHAR_DATA_SECTION_COUNT] = {")
    body = s[i:s.index("};", i)]
    out = {}
    for m in re.finditer(r'\[CHAR_DATA_(\w+)\]\s*=\s*(?:sizeof\((\w+)\)|(\d+))', body):
        out[m.group(1).lower()] = m.group(2) if m.group(2) else ('u8' if m.group(3) == '1' else None)
    assert len(out) == 25, out
    return out


def sizeof_types(types):
    """Real sizeof() per C type, by compiling a probe against include/structs.h."""
    types = sorted(set(t for t in types if t and t != 'u8'))
    src = '#include "structs.h"\n#include <stdio.h>\nint main(void){\n'
    for t in types:
        src += '  printf("%s %zu\\n", "' + t + '", sizeof(' + t + '));\n'
    src += '  return 0;}\n'
    with tempfile.TemporaryDirectory() as d:
        c, exe = os.path.join(d, "sz.c"), os.path.join(d, "sz")
        open(c, "w").write(src)
        subprocess.run([os.environ.get("CC", "cc"), "-w", "-I", os.path.join(REPO, "include"),
                        "-o", exe, c], check=True)
        out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    sz = {'u8': 1}
    for line in out.strip().splitlines():
        k, v = line.split()
        sz[k] = int(v)
    return sz


ELEM_TYPE = parse_section_element_types()
SIZEOF = sizeof_types(ELEM_TYPE.values())
ELEM_SIZE = {k: SIZEOF[v] for k, v in ELEM_TYPE.items()}

# Strides the *engine* actually uses where they differ from
# section_element_sizes[] (which only feeds get_ps2_section_span's %-check):
#   mvxy : add_to_mvxy_data / setup_mvxy_data stride 6 s16  (pls02.c:119, :152)
#   stxy : exec_char_asxy reads an s16 pair at data*2       (effect.c:443-451)
#   sernd: read_sernd record is 0x24 = u32 + 16 u16         (arcade_char_data.c:346-370)
STRIDE = dict(ELEM_SIZE)
STRIDE['mvxy'] = 12
STRIDE['stxy'] = 4
STRIDE['sernd'] = 0x24


def parse_char_3sx_to_arcade():
    """#define CHAR_3SX_TO_ARCADE(c) ((c) > CHAR_AKUMA ? (c) + 1 : (c))  — constants.h:62."""
    s = CG.src("src/constants.h")
    m = re.search(r'#define CHAR_3SX_TO_ARCADE\(c\)\s*\(\(c\)\s*>\s*CHAR_(\w+)\s*\?\s*\(c\)\s*\+\s*(\d+)\s*:\s*\(c\)\)', s)
    assert m, "CHAR_3SX_TO_ARCADE not found in constants.h"
    pivot = int(re.search(r'CHAR_%s = (\d+)' % m.group(1), s.split("#else", 1)[1]).group(1))
    bump = int(m.group(2))
    return lambda c: c + bump if c > pivot else c, pivot, bump


TO_ARCADE, ARC_PIVOT, ARC_BUMP = parse_char_3sx_to_arcade()


def effinit_index(fname):
    s = CG.src("src/sf33rd/Source/Game/effect/effxx.c")
    i = s.index("const s32 (*effinitjptbl[59])() = {")
    j = s.index("{", i)
    d, k = 0, j
    while True:
        if s[k] == '{':
            d += 1
        elif s[k] == '}':
            d -= 1
            if d == 0:
                break
        k += 1
    return [x.strip() for x in s[j + 1:k].split(',') if x.strip()].index(fname)


EXEC_CHAR_ASXY = effinit_index("exec_char_asxy")


# ------------------------------------------------------------------ section decode
def _swap16(buf):
    n = len(buf) // 2
    return struct.pack('<%dH' % n, *struct.unpack('>%dH' % n, buf[:n * 2])) + buf[n * 2:]


def arc_section(ci, sec):
    """Arcade section bytes, transformed exactly as ArcadeCharData_Init does."""
    off, size = LOC[ci][sec]
    raw = ROM[off:off + size]
    kind = READERS[sec]
    if kind == 'u8':
        return raw
    if kind in ('s16', 'u16'):
        return _swap16(raw)
    if kind == 'catch':                       # arcade_char_data.c:396-412
        out = bytearray(raw)
        for p in range(0, len(raw) - 7, 8):
            out[p:p + 4] = _swap16(raw[p:p + 4])
            out[p + 6:p + 8] = _swap16(raw[p + 6:p + 8])
        return bytes(out)
    if kind == 'sernd':                       # arcade_char_data.c:346-370
        n = size // 0x24
        out = bytearray()
        for i in range(n):
            v = struct.unpack_from('>I', raw, i * 4)[0]
            out += struct.pack('<I', (v - (BASE_OFFSET + off)) & 0xFFFFFFFF)
        out += _swap16(raw[n * 4: n * 4 + n * 32])
        out += raw[len(out):]
        return bytes(out)
    raise AssertionError(kind)


def ps2_section(blob, spans, sec):
    b, z = spans[SECTIONS.index(sec)]
    return blob[b:b + z]


# ------------------------------------------------------------------ RICT layout model
def rict_model(a, p):
    """RICT is [group][opponent]: 24 opponent slots arcade, 20 PS2.

    charset.c:2736/:2900   curr_rca = rival_catch_tbl + cg_rival + catch_table_offset(...)
    charset.c:2658-2664    catch_table_offset = CHAR_3SX_TO_ARCADE(thrown) - 24  (arcade)
                                              = thrown - 20                      (PS2)
    So the subtracted constant IS the per-group opponent count, and cg_rival is
    (group+1)*count. Arcade slot CHAR_3SX_TO_ARCADE(j) corresponds to PS2 slot j.
    Returns (ok, groups, mismatches) where a mismatch is a mapped pair whose
    8 bytes differ.
    """
    es = 8
    na, np_ = len(a) // es, len(p) // es
    if np_ == 0 or na * 20 != np_ * 24:
        return False, 0, []
    g = np_ // 20
    if g * 24 != na:
        return False, 0, []
    mism = []
    for gi in range(g):
        for j in range(20):
            ai, pi = gi * 24 + TO_ARCADE(j), gi * 20 + j
            ea, ep = a[ai * es:ai * es + es], p[pi * es:pi * es + es]
            if ea != ep:
                mism.append(dict(group=gi, opponent=NAMES[j],
                                 arcade=dict(zip(RICT_F, struct.unpack('<hhBBh', ea))),
                                 ps2=dict(zip(RICT_F, struct.unpack('<hhBBh', ep)))))
    return True, g, mism


# ------------------------------------------------------------------ script cells (both sides)
def _walk(buf, base, size, ents, si, be, dead=None):
    """Decode one script's cells, including the cgd==6 tail cg_audit.py skips.

    Mirrors read_char_table (arcade_char_data.c:150-212) / the WORK cell layout
    (structs.h:316-334). `be` selects the arcade (big-endian ROM) or PS2
    (little-endian, already in memory order) field order.

    `dead`: the set of UNREACHABLE cell positions for this script, from
    cg_audit.py -> k7_entry_walk (arcade only; there is no PS2 reachability
    model, and none is needed -- PS2 cells serve only as the §6.1 oracle).
    Positions count C and L cells alike, which is the numbering k7_entry_walk
    uses; `cell`/`n` counts L cells only, which is what the bounds rows report.
    The two walks stride identically for both cell kinds and take the same
    last-script break, so position k here IS position k there (asserted by
    `_assert_walk_alignment`).
    """
    e = '>' if be else '<'
    so = sorted(set(ents))
    start = ents[si] - 8
    nxt = [o for o in so if o > ents[si]]
    last = not nxt
    end = (nxt[0] - 8) if nxt else size
    if start < 0 or end <= start or base + end > len(buf):
        return None, []
    p = base + start
    cgd = struct.unpack_from(e + 'h', buf, p)[0]
    if cgd not in (1, 2, 4, 6):
        return cgd, []
    out, q, lim, n, term, pos = [], p + 8, base + end, 0, False, 0
    while q + 8 <= lim:
        code = struct.unpack_from(e + 'H', buf, q)[0]
        if code < 0x100:
            q += 8 + max(cgd * 4 - 8, 0)
            pos += 1
            if code in CG.TERMINATORS:
                # An unconditional control transfer. Both walks stop here only
                # for the LAST script (whose end is the declared location size).
                # "after one of these" is NOT a reachability test in either
                # direction -- §26.10.2 withdrew that convention, because a
                # jump's `pat` is a 1-based cell index and can land past a
                # terminator. It is kept as the descriptive statistic §15.7
                # measured; the verdict comes from `dead` below.
                if last:
                    break
                term = True
            continue
        se, olc, num = struct.unpack_from(e + 'HHH', buf, q + 2)
        r = dict(cell=n, pos=pos, cgd=cgd, ctype=code & 0xFF, se=se, olc_ix=olc, num=num,
                 after_term=term, dead=(None if dead is None else (pos in dead)))
        pos += 1
        q2 = q + 8
        if cgd >= 4:
            if be:
                att, hit = struct.unpack_from('>hH', buf, q2)      # ROM order: att, hit
            else:
                hit, att = struct.unpack_from('<Hh', buf, q2)      # memory order: hit, att
            r.update(att_ix=att, hit_ix=hit, effect=buf[q2 + 6], eftype=buf[q2 + 7])
            q2 += 8
        if cgd == 6:
            zoom, rival, add_xy = struct.unpack_from(e + 'HHH', buf, q2)
            r.update(rival=rival, add_xy=add_xy)
            q2 += 8
        out.append(r)
        n += 1
        q = q2
    return cgd, out


def _assert_walk_alignment(ci, sec, si, ents, cells):
    """`dead` positions come from cg_audit's `arc_parse`; the cells come from `_walk`. The mapping is
    only valid if the two decoders visit the same byte positions, so it is asserted rather than
    assumed: same cell count including commands, and every L cell at the same ordinal."""
    a = CG.arc_parse(ci, sec, si, {sec: ents})[1]
    lpos = [k for k, c in enumerate(a) if c[0] == 'L']
    assert len(lpos) == len(cells), "%s %s[%d]: %d L cells in arc_parse, %d in _walk" % (
        NAMES[ci], sec, si, len(lpos), len(cells))
    for j, c in enumerate(cells):
        assert lpos[j] == c['pos'], "%s %s[%d] cell %d: position %d vs %d" % (
            NAMES[ci], sec, si, j, lpos[j], c['pos'])


def arcade_scripts(ci, check=False):
    """The arcade side, with `dead` from cg_audit -> k7_entry_walk on every cell.

    §15.7 split these rows on whether the cell precedes its script's first terminator. That
    convention was PROVEN UNSOUND (§26.10.2): `comm_jmp`/`comm_jpss`/`comm_jsr` pass `pat` to
    `set_char_move_init2` as a 1-based cell index (`cg_ix = (ip - 1) * cgd_type - cgd_type`), so a
    jump can land past a terminator, and §28.2 found three cells a stronger model revives that way.
    The verdict is therefore taken from the entry-point closure `cg_audit.py` already uses for its own
    `*_oob` classes -- imported, not reimplemented, so the two audits cannot drift apart."""
    out = {}
    dead_all = CG.k7_entry_walk(ci)
    for _koc, sec in CG.KOC2SEC.items():
        off, size = LOC[ci][sec]
        ents = CG.arc_offsets(off, size)
        out[sec] = [(_walk(ROM, off, size, ents, si, True, dead_all[(sec, si)])[1])
                    for si in range(len(ents))]
        if check:
            for si in range(len(ents)):
                _assert_walk_alignment(ci, sec, si, ents, out[sec][si])
    return out


def ps2_scripts(blob, spans):
    out = {}
    for _koc, sec in CG.KOC2SEC.items():
        b, z = spans[SECTIONS.index(sec)]
        ents = CG.ps2_offsets(blob, b)
        out[sec] = [(_walk(blob, b, z, ents, si, False)[1]) for si in range(len(ents))]
    return out


def decoded_indices(cell):
    """charset.c:2698-2705 -> the ATIT and HIIT indices the engine actually uses.

        st.w.h = cg_att_ix; st.w.l = cg_hit_ix;    (LoHi16 = {s16 l; s16 h;})
        cg_att_ix >>= 6;                           -> ATIT index (sign-extended)
        st.l *= 8; cg_hit_ix = st.w.h & 0x1FF;     -> HIIT index
      set_new_attnum (charset.c:2927-2944) negates a negative cg_att_ix before
      `wk->att = *(wk->att_ix_table + wk->cg_att_ix)`, and only runs when the
      shifted value is non-zero.  Equivalently the 32-bit (att:hit) word is a
      bitfield: bits 0-12 cg_meoshi, bits 13-21 HIIT index, bits 22-31 ATIT index.
    """
    if 'att_ix' not in cell:
        return None, None
    att, hit = cell['att_ix'], cell['hit_ix'] & 0xFFFF
    atit = att >> 6                                  # arithmetic shift on s16
    combined = ((att & 0xFFFF) << 16) | hit
    hiit = ((combined * 8) >> 16) & 0x1FF
    return (abs(atit) if atit != 0 else None), hiit


def cell_indices(cell, n_stxy_hint=None):
    """All section indices a single cell can produce -> {section: [indices]}."""
    out = {}
    a_ix, h_ix = decoded_indices(cell)
    if a_ix is not None:
        out['atit'] = [a_ix]
    if h_ix is not None:
        out['hiit'] = [h_ix]
    st = []
    if cell.get('add_xy'):
        st += [cell['add_xy'], cell['add_xy'] + 1]        # charset.c:2678-2687
    if cell.get('effect') == EXEC_CHAR_ASXY:
        st += [cell['eftype'] * 2, cell['eftype'] * 2 + 1]  # effect.c:443-451
    if st:
        out['stxy'] = st
    se = cell['se'] >> 4                                   # charset.c:2721-2727
    if se & 0x800:
        out['sernd'] = [se & 0x7FF]
    if cell.get('rival'):
        out['rival'] = [cell['rival']]
    return out


# ------------------------------------------------------------------ the audit
def audit():
    result = {}
    for ci in range(20):
        blob, _bsd = CG.ps2_tail(ci)
        _offs, spans = CG.ps2_spans(blob)
        rec = dict(name=NAMES[ci], sections={}, bounds=[], notes=[])
        arc = {s: arc_section(ci, s) for s in TARGETS}
        ps2 = {s: ps2_section(blob, spans, s) for s in TARGETS}

        # ---------------- content compare
        for sec in TARGETS:
            a, p = arc[sec], ps2[sec]
            es = STRIDE[sec]
            an, pn = len(a) // es, len(p) // es
            n = min(len(a), len(p))
            d = dict(elem_size=es, declared_elem_size=ELEM_SIZE[sec], elem_type=ELEM_TYPE[sec],
                     arcade_bytes=len(a), ps2_bytes=len(p), arcade_elems=an, ps2_elems=pn,
                     prefix_identical=(a[:n] == p[:n]),
                     arcade_extra_bytes=max(0, len(a) - len(p)),
                     ps2_extra_bytes=max(0, len(p) - len(a)), diffs=[], model=None)
            if sec == 'rict':
                ok, g, mism = rict_model(a, p)
                d['model'] = dict(kind='group_x_opponent', ok=ok, groups=g,
                                  arcade_slots=24, ps2_slots=20)
                d['differing'] = len(mism)
                d['identical'] = (g * 20 - len(mism)) if ok else 0
                d['diffs'] = mism[:200]
            else:
                ident, diffs = 0, []
                for i in range(min(an, pn)):
                    ea, ep = a[i * es:(i + 1) * es], p[i * es:(i + 1) * es]
                    if ea == ep:
                        ident += 1
                        continue
                    if sec == 'hiit':
                        va = struct.unpack('<8H', ea)
                        vp = struct.unpack('<8H', ep)
                        f = [dict(field=HIIT_F[k], arcade=va[k], ps2=vp[k])
                             for k in range(8) if va[k] != vp[k]]
                    elif sec == 'atit':
                        f = []
                        for k in range(16):
                            va, vp = ea[k], ep[k]
                            if va == vp:
                                continue
                            if k in (12, 13):          # hs_me / hs_you are s8
                                va = va - 256 if va > 127 else va
                                vp = vp - 256 if vp > 127 else vp
                            f.append(dict(field=ATIT_F[k], arcade=va, ps2=vp,
                                          xor=(ea[k] ^ ep[k])))
                    else:
                        va = struct.unpack('<%dh' % (es // 2), ea)
                        vp = struct.unpack('<%dh' % (es // 2), ep)
                        f = [dict(field=k, arcade=va[k], ps2=vp[k])
                             for k in range(es // 2) if va[k] != vp[k]]
                    diffs.append(dict(index=i, fields=f))
                d['identical'] = ident
                d['differing'] = len(diffs)
                d['diffs'] = diffs[:200]
            rec['sections'][sec] = d

        # ---------------- bounds, with the PS2 discriminator
        counts_a = {s: len(arc[s]) // ELEM_SIZE[s] for s in TARGETS}
        counts_p = {s: len(ps2[s]) // ELEM_SIZE[s] for s in TARGETS}
        counts_a['stxy'] = len(arc['stxy']) // 2      # engine indexes stxy s16-wise
        counts_p['stxy'] = len(ps2['stxy']) // 2
        counts_a['sernd'] = len(arc['sernd']) // 0x24
        counts_p['sernd'] = len(ps2['sernd']) // 0x24

        asc, psc = arcade_scripts(ci, check=True), ps2_scripts(blob, spans)
        maxix = {}
        for sec, cells in asc.items():
            for si, acells in enumerate(cells):
                pcells = psc[sec][si] if si < len(psc[sec]) else None
                aligned = (pcells is not None and len(pcells) == len(acells)
                           and all(a['cgd'] == p['cgd'] for a, p in zip(acells, pcells)))
                for k, c in enumerate(acells):
                    ix = cell_indices(c)
                    pix = cell_indices(pcells[k]) if aligned else None
                    for tgt, vals in ix.items():
                        lim = counts_a.get(tgt)
                        for v in vals:
                            maxix[tgt] = max(maxix.get(tgt, -1), v)
                        if tgt == 'rival':
                            continue
                        for v in vals:
                            if lim is not None and v >= lim:
                                if pix is None:
                                    verdict = 'no_oracle_shape_differs'
                                elif v in pix.get(tgt, []):
                                    verdict = 'pre_existing_in_ps2'
                                else:
                                    verdict = 'ARCADE_ONLY'
                                rec['bounds'].append(dict(
                                    kind='%s_oob' % tgt, table=sec, script=si, cell=k,
                                    index=v, arcade_entries=lim, ps2_entries=counts_p.get(tgt),
                                    after_terminator=bool(c.get('after_term')),
                                    dead=bool(c.get('dead')),
                                    verdict=verdict))
        # cg_rival: the engine reads rival_catch_tbl[cg_rival + arcade_id - 24],
        # so the highest touched element is cg_rival - 1 (arcade_id max 23).
        bad_rival = []
        for sec, cells in asc.items():
            for si, cl in enumerate(cells):
                for k, c in enumerate(cl):
                    r = c.get('rival', 0)
                    if r and (r % 24 or r - 1 >= counts_a['rict']):
                        bad_rival.append(dict(table=sec, script=si, cell=k, cg_rival=r,
                                              after_terminator=bool(c.get('after_term')),
                                              dead=bool(c.get('dead'))))
        rec['notes'].append(dict(check='rict_rival_model',
                                 bad=len(bad_rival),
                                 bad_live=sum(1 for b in bad_rival if not b['dead']),
                                 bad_before_terminator=sum(1 for b in bad_rival
                                                           if not b['after_terminator']),
                                 examples=bad_rival[:8]))
        rmax = maxix.get('rival', -1)
        rec['notes'].append(dict(check='rict_rival_range', max_cg_rival=rmax,
                                 arcade_elems=counts_a['rict'],
                                 rival_multiple_of_24=(rmax % 24 == 0) if rmax > 0 else None,
                                 highest_touched=(rmax - 1) if rmax > 0 else None,
                                 in_bounds=(rmax - 1 < counts_a['rict']) if rmax > 0 else None))

        # ---------------- HIIT fields -> BODA/HANA/CATA/CAUA/ATTA/HOSA
        # set_jugde_area (charset.c:2975-2981) indexes with them, unchecked.
        sub_pairs = (('boda', 'boix'), ('hana', 'bhix+haix'), ('cata', 'caix'),
                     ('caua', 'cuix'), ('atta', 'atix'), ('hosa', 'hoix'))
        submax = {}

        def hiit_fields(buf, i):
            return struct.unpack_from('<8H', buf, i * 16)

        for tgt, expr in sub_pairs:
            for i in range(counts_a['hiit']):
                f = hiit_fields(arc['hiit'], i)
                idx = (f[1] + f[2]) if expr == 'bhix+haix' else f[HIIT_F.index(expr)]
                submax[tgt] = max(submax.get(tgt, -1), idx)
                if idx >= counts_a[tgt]:
                    pv = None
                    if i < counts_p['hiit']:
                        g = hiit_fields(ps2['hiit'], i)
                        pv = (g[1] + g[2]) if expr == 'bhix+haix' else g[HIIT_F.index(expr)]
                    verdict = ('pre_existing_in_ps2'
                               if (pv == idx and idx >= counts_p[tgt]) else
                               ('no_oracle_shape_differs' if pv is None else 'ARCADE_ONLY'))
                    rec['bounds'].append(dict(kind='%s_oob_from_hiit' % tgt, hiit_entry=i,
                                              field=expr, index=idx,
                                              arcade_entries=counts_a[tgt],
                                              ps2_entries=counts_p[tgt], verdict=verdict))
        # hosei_adrs is also read at [hoix+1] (effc2.c:879).
        if submax.get('hosa', -1) + 1 >= counts_a['hosa']:
            pre = (submax['hosa'] + 1 >= counts_p['hosa'])
            rec['notes'].append(dict(check='hosa_hoix_plus1', max_hoix=submax['hosa'],
                                     arcade_entries=counts_a['hosa'],
                                     ps2_entries=counts_p['hosa'],
                                     verdict='pre_existing_in_ps2' if pre else 'ARCADE_ONLY'))

        rec['stats'] = dict(counts_arcade=counts_a, counts_ps2=counts_p,
                            max_index_used=maxix, hiit_field_max=submax,
                            bounds_violations=len(rec['bounds']))
        result[NAMES[ci]] = rec
    return result


# ------------------------------------------------------------------ report
if __name__ == "__main__":
    print("derived constants:")
    print("  element sizes  " + "  ".join("%s=%s(%d)" % (s, ELEM_TYPE[s], ELEM_SIZE[s]) for s in TARGETS))
    print("  engine strides " + ", ".join("%s=%d" % (s, STRIDE[s]) for s in TARGETS if STRIDE[s] != ELEM_SIZE[s]))
    print("  CHAR_3SX_TO_ARCADE: c > %d ? c + %d : c" % (ARC_PIVOT, ARC_BUMP))
    print("  exec_char_asxy = effinitjptbl[%d]" % EXEC_CHAR_ASXY)
    print()

    res = audit()
    json.dump(res, open(os.path.join(HERE, "data_audit.json"), "w"), indent=1)

    print("=== per-character x per-section: arcade_elems/ps2_elems  (= identical, Dn = n differ,")
    print("    P! = common prefix differs, +n/-n = arcade span longer/shorter in bytes) ===")
    hdr = "%-7s | %s" % ("char", " ".join("%-14s" % s.upper() for s in TARGETS))
    print(hdr)
    print("-" * len(hdr))
    tot = {s: dict(ident=0, diff=0, chr_diff=0, chr_size=0) for s in TARGETS}
    for n in NAMES:
        cols = []
        for s in TARGETS:
            d = res[n]['sections'][s]
            tag = ""
            if not d['prefix_identical']:
                tag += "P!"
            if d['differing']:
                tag += "D%d" % d['differing']
                tot[s]['chr_diff'] += 1
            if d['arcade_extra_bytes'] or d['ps2_extra_bytes']:
                tag += "+%d" % d['arcade_extra_bytes'] if d['arcade_extra_bytes'] else "-%d" % d['ps2_extra_bytes']
                tot[s]['chr_size'] += 1
            tot[s]['ident'] += d['identical']
            tot[s]['diff'] += d['differing']
            cols.append("%-14s" % ("%d/%d%s" % (d['arcade_elems'], d['ps2_elems'], tag or "=")))
        print("%-7s | %s" % (n, " ".join(cols)))
    print()
    print("%-6s %9s %9s %9s %9s   %s" % ("sect", "identical", "differing", "chars_dif", "chars_sz", "verdict"))
    for s in TARGETS:
        t = tot[s]
        v = ("IDENTICAL" if (t['diff'] == 0 and t['chr_size'] == 0) else
             ("CONTENT-IDENTICAL, SPAN DIFFERS" if t['diff'] == 0 else "DIFFERS"))
        print("%-6s %9d %9d %9d %9d   %s" % (s, t['ident'], t['diff'], t['chr_diff'], t['chr_size'], v))

    print()
    print("=== classified differences ===")
    for s in TARGETS:
        allf = {}
        for n in NAMES:
            for d in res[n]['sections'][s]['diffs']:
                for f in (d.get('fields') or []):
                    key = f['field'] if isinstance(f, dict) else f
                    allf[key] = allf.get(key, 0) + 1
                if s == 'rict':
                    for k in RICT_F:
                        if d['arcade'][k] != d['ps2'][k]:
                            allf[k] = allf.get(k, 0) + 1
        if allf:
            print("  %-6s %s" % (s, sorted(allf.items(), key=lambda x: -x[1])))
    # ATIT: how many `level` diffs are exactly the jump_att_flag bit (charset.c:2947)
    lv = x40 = 0
    for n in NAMES:
        for d in res[n]['sections']['atit']['diffs']:
            for f in d['fields']:
                if f['field'] == 'level':
                    lv += 1
                    x40 += (f['xor'] == 0x40)
    print("  atit   `level` diffs %d, of which exactly bit 0x40 (jump_att_flag): %d" % (lv, x40))

    print()
    print("=== bounds hazards (arcade index >= arcade section length) ===")
    agg = {}
    for n in NAMES:
        for b in res[n]['bounds']:
            agg.setdefault((b['kind'], b['verdict']), []).append(n)
    if not agg:
        print("  none")
    for (kind, verdict), chars in sorted(agg.items()):
        u = sorted(set(chars))
        print("  %-24s %-26s %4d hits over %2d chars: %s"
              % (kind, verdict, len(chars), len(u), ",".join(u)))
    print()
    print("  split by REACHABILITY -- cg_audit.py's k7_entry_walk: cell 0 of every script, plus")
    print("  every landing a C cell names, plus the entries the C forms, closed over the six")
    print("  intra-script writers of cg_ix, failing OPEN wherever the model cannot follow.")
    print("  A `live` row is a finding; a `dead` row has lost its excuse only if it flips.")
    for kind in sorted({b['kind'] for n in NAMES for b in res[n]['bounds']}):
        rows = [b for n in NAMES for b in res[n]['bounds'] if b['kind'] == kind]
        live = sum(1 for b in rows if not b.get('dead'))
        print("    %-24s live %4d | dead %4d" % (kind, live, len(rows) - live))
    print()
    print("  the same rows on §15.7's WITHDRAWN criterion (§26.10.2: a jump's `pat` is a 1-based")
    print("  cell index, so `after a terminator` is neither necessary nor sufficient for dead).")
    print("  Kept because the DISTRIBUTION is a real measurement; it carries no verdict:")
    for kind in sorted({b['kind'] for n in NAMES for b in res[n]['bounds']}):
        rows = [b for n in NAMES for b in res[n]['bounds'] if b['kind'] == kind]
        pre = sum(1 for b in rows if not b.get('after_terminator'))
        agree = sum(1 for b in rows if bool(b.get('after_terminator')) == bool(b.get('dead')))
        print("    %-24s before-terminator %4d | after-terminator %4d | agrees with reach %d/%d"
              % (kind, pre, len(rows) - pre, agree, len(rows)))
    print()
    for n in NAMES:
        for note in res[n]['notes']:
            if note['check'] == 'rict_rival_model' and note['bad']:
                print("  RICT cg_rival off-model for %-7s: %d cells (%d live by reach; %d of them "
                      "before a script terminator)"
                      % (n, note['bad'], note['bad_live'], note['bad_before_terminator']))
    bad = [n for n in NAMES for note in res[n]['notes']
           if note['check'] == 'hosa_hoix_plus1' and note['verdict'] == 'ARCADE_ONLY']
    print("  hosa[hoix+1] (effc2.c:879) reaches past the arcade table for: %s"
          % (",".join(bad) if bad else "no character beyond what PS2 also does"))
