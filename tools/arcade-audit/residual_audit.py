#!/usr/bin/env python3
"""
Residual-index (SECOND-DOOR) audit for the arcade CG render path.

`cg_audit.py` checks the FIRST of two ways the sprite path can fault:

    n = wk->cg_number;
    i = obj_group_table[n];            /* (1) n >= 37664 -> OOB read      */
    if (i == 0) return;                /*     gap -> clean skip           */
    if (texgrplds[i].ok == 0) return;  /*     group not loaded -> skip    */
    n -= texgrpdat[i].num_of_1st;      /* (2) THE RESIDUAL — never checked */
    trsbas = (u16*)(texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[n]);
    count = *trsbas;                   /* (3) DEREFERENCE                  */

Nine sites in src/sf33rd/Source/Game/rendering/mtrans.c carry that idiom:
:185, :284, :377 (getObjectHeight), :426, :695, :818, :1099, :1227, :1486.
`cg_audit.py:370-379` flags only (1).  A cg_number that lands in a WRONG BUT
VALID group is filed as class (c) "wrong sprite drawn" — yet if the residual
`n` is negative or past the end of that group's offset table, the fault is the
SAME wild-pointer dereference as the Elena crash (upstream #363), through a
different door.

This script closes that gap.  Three questions, answered statically:

  R1  How long IS each texture group's offset table?
      `mtrans.c:2533` computes it at run time as `*(u32*)trans_table / 4`.
      `texgroup.c:405` / `:567` / `:634` set `trans_table = ldadr`, the base of
      the loaded AFS entry `texgrpdat[grp].apfn`.  So the length is the first
      little-endian u32 of that AFS entry divided by 4 — readable statically
      out of SF33RD.AFS, for all 71 groups obj_group_table can name.

  R2  Is the residual in bounds for EVERY cell?  All 20 characters x 10 script
      tables, plus the OVCT `parts_char` -> `cg_number` path (eff01.c:169).
      The PS2 side is walked as a control: a violation the shipped PS2 data
      also produces from a byte-identical cell is pre-existing, not an
      adaptation defect (the cg_audit.py:303-310 discriminator).

  R3  Which groups can actually be loaded, and by whom?  A violation only
      faults when `texgrplds[i].ok != 0`.  Group ownership is derived from
      `ldreq_tbl[294]` / `ldreq_ix[43][2]` (gd3rd.c:1024, :2791) — the tables
      `Push_LDREQ_Queue_Player` (:405) and `Push_LDREQ_Queue_Union` (:439)
      walk to issue type-1 (texture group) load requests.  Rows 0..19 are the
      20 characters; rows 20..42 are the stage/BG unions (`Push_LDREQ_Queue_BG`
      at :434 adds 20 to its argument).

CONSTANTS ARE PARSED FROM SOURCE AT RUN TIME, not hand-copied — via cg_audit.py
(location_data[], cg_maps[], texgrpdat[], obj_group_table[]) and this script's
own parse of gd3rd.c.  Editing those tables changes the answer automatically.

Outputs `residual_audit.json` beside this script, plus a human-readable report.
Neither cg_audit.py nor data_audit.py is modified; both are imported.
"""
import collections
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cg_audit as CG      # noqa: E402  (parses the shared constants at import)
import data_audit as DA    # noqa: E402  (_walk / arcade_scripts / ps2_scripts)

HERE = CG.HERE
NAMES = CG.NAMES
SECTIONS = CG.SECTIONS
TGD = CG.TGD               # texgrpdat[100]      texgroup.c:41-152
OGT = CG.OGT               # obj_group_table[]   chren3rd.c:8
OGT_N = len(OGT)
AFS_ENT = CG.AFS_ENT
AFS = CG.AFS

OVCT_ELEM = 16             # OverlapPart, read_ovct arcade_char_data.c:370-392
OVCT_PARTS_CHAR_OFF = 14   # last field of the 16-byte element


# ---------------------------------------------------------------- R1
def group_extents():
    """obj_group_table -> {group: (lo, hi)} for every group it can name."""
    ext = collections.defaultdict(list)
    for cg, g in enumerate(OGT):
        if g:
            ext[g].append(cg)
    out = {}
    for g, cgs in ext.items():
        assert cgs[-1] - cgs[0] + 1 == len(cgs), "group %d is not contiguous" % g
        out[g] = (cgs[0], cgs[-1])
    return out


def resolve_group(row):
    """texgroup.c:184-187 — the group number a texgrpdat row publishes into."""
    n1 = row['num_of_1st']
    return OGT[n1 + 1] if n1 == 0 else OGT[n1]


def offset_table_lengths(ext):
    """R1: length of each group's trans_table offset array, from SF33RD.AFS.

    mtrans.c:2533  n = *(u32*)grplds->trans_table / 4;
    texgroup.c:405 curr->lds->trans_table = ldadr;      (base of AFS entry apfn)
    """
    info = {}
    for g in sorted(ext):
        row = TGD[g]
        apfn = row['apfn']
        assert apfn != -1, "group %d resolves to a texgrpdat row with apfn == -1" % g
        off, size = AFS_ENT[apfn]
        AFS.seek(off)
        head = AFS.read(4)
        first = struct.unpack('<I', head)[0]
        length = first // 4
        # Structural validation: the array must be strictly increasing, every
        # entry must land between the end of the array itself and to_tex (where
        # the texture table starts), and entry 0 must equal 4 * length.
        AFS.seek(off)
        blob = AFS.read(min(size, row['to_tex'] + 8))
        offs = list(struct.unpack_from('<%dI' % length, blob, 0))
        mono = all(offs[i + 1] > offs[i] for i in range(length - 1))
        inrange = all(4 * length <= x <= row['to_tex'] for x in offs)
        lo, hi = ext[g]
        info[g] = dict(group=g, apfn=apfn, afs_offset=off, afs_size=size,
                       declared_use=row['use'], to_tex=row['to_tex'],
                       num_of_1st=row['num_of_1st'], cg_lo=lo, cg_hi=hi,
                       extent=hi - lo + 1, table_len=length,
                       first_u32=first, monotonic=mono, entries_in_range=inrange,
                       self_consistent=(offs[0] == 4 * length),
                       shortfall=max(0, (hi - row['num_of_1st'] + 1) - length))
    return info


def aliased_rows(ext):
    """Every texgrpdat row that resolves to each group, with its table length.

    A group can be published by more than one row (language / stage variants —
    e.g. checkSelObjFileLoaded, texgroup.c:550-555, picks texgrpdat[0x62] or
    texgrpdat[0x17] for group 23).  If two rows for one group disagreed on the
    table length the static bound would be ambiguous; this reports that.
    """
    out = collections.defaultdict(list)
    for i, row in enumerate(TGD):
        if row['apfn'] == -1:
            continue
        g = resolve_group(row)
        if g == 0 or g not in ext:
            continue
        off, size = AFS_ENT[row['apfn']]
        AFS.seek(off)
        out[g].append(dict(row=i, apfn=row['apfn'],
                           table_len=struct.unpack('<I', AFS.read(4))[0] // 4))
    return out


# ---------------------------------------------------------------- R3
def parse_ldreq_tables():
    """gd3rd.c ldreq_tbl[294] (:1024) + ldreq_ix[43][2] (:2791) -> ownership."""
    s = CG.src("src/sf33rd/Source/Game/io/gd3rd.c")

    def brace_block(marker):
        i = s.index(marker)
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
        return s[j + 1:k]

    def rows(body):
        return [[int(x, 0) for x in re.findall(r'0x[0-9A-Fa-f]+|\d+', m.group(1))]
                for m in re.finditer(r'\{([^{}]*)\}', body)]

    tbl = rows(brace_block("const LDREQ_TBL ldreq_tbl[294] = {"))
    ix = rows(brace_block("const s16 ldreq_ix[43][2] = {"))
    assert len(tbl) == 294 and len(ix) == 43, (len(tbl), len(ix))

    owners = collections.defaultdict(list)
    for row, (start, count) in enumerate(ix):
        # gd3rd.c:405-431 Push_LDREQ_Queue_Player(id, ix) for ix < 20;
        # gd3rd.c:434-437 Push_LDREQ_Queue_BG(ix) -> Union(ix + 20).
        tag = ("char:" + NAMES[row]) if row < 20 else ("stage:%d" % (row - 20))
        for e in tbl[start:start + count]:
            if e[0] == 1:               # type 1 == q_ldreq_texture_group
                owners[resolve_group(TGD[e[1]])].append(tag)
    return owners


def reachability(group, owners):
    """Human-readable reachability verdict for one group."""
    who = owners.get(group)
    if not who:
        return "UNKNOWN-LOADER", "no type-1 ldreq_tbl entry resolves to this group"
    chars = sorted({w[5:] for w in who if w.startswith("char:")})
    stages = sorted({w[6:] for w in who if w.startswith("stage:")})
    if chars and not stages:
        if len(chars) == 1:
            return "PER-CHARACTER", "loaded only while %s is in the match" % chars[0]
        return "PER-CHARACTER", "loaded while any of %s is in the match" % ", ".join(chars)
    if stages and not chars:
        return "PER-STAGE", "loaded only on stage union %s" % ", ".join(stages)
    return "MIXED", "characters %s; stages %s" % (", ".join(chars), ", ".join(stages))


# ---------------------------------------------------------------- R2
def classify(value, lengths):
    """The renderer's own decision tree, for one cg_number.

    Returns (verdict, group, residual, table_len).
    """
    if value >= OGT_N:
        return 'ogt_oob', None, None, None                 # class (a), cg_audit's
    g = OGT[value]
    if g == 0:
        return 'gap', 0, None, None                        # clean skip
    n = value - TGD[g]['num_of_1st']
    L = lengths[g]['table_len']
    if n < 0:
        return 'residual_negative', g, n, L
    if n >= L:
        return 'residual_past_end', g, n, L
    return 'ok', g, n, L


def wild_offset(group, n, lengths):
    """The u32 the renderer would actually read at ((u32*)trans_table)[n]."""
    info = lengths[group]
    byte = 4 * n
    if byte + 4 > info['afs_size']:
        return None, 'index is past the end of the loaded file itself'
    AFS.seek(info['afs_offset'] + byte)
    v = struct.unpack('<I', AFS.read(4))[0]
    note = 'lands inside the loaded file' if v < info['afs_size'] \
        else 'PAST EOF of the %d-byte loaded file' % info['afs_size']
    return v, note


def audit():
    ext = group_extents()
    lengths = offset_table_lengths(ext)
    aliases = aliased_rows(ext)
    owners = parse_ldreq_tables()

    result = dict(
        groups={str(g): lengths[g] for g in lengths},
        group_aliases={str(g): aliases[g] for g in aliases if len(aliases[g]) > 1},
        group_owners={str(g): sorted(set(owners.get(g, []))) for g in lengths},
        length_vs_extent_mismatches=[],
        alias_length_disagreements=[],
        script_violations=[],
        script_violations_ps2_control=[],
        wrong_group_census={},
        ovct_violations=[],
        ovct_violations_ps2_control=[],
        stats=dict(cells=0, ok=0, gap=0, ogt_oob=0,
                   residual_negative=0, residual_past_end=0),
    )

    for g, info in sorted(lengths.items()):
        if info['table_len'] != info['extent'] or not info['monotonic'] \
                or not info['entries_in_range'] or not info['self_consistent']:
            result['length_vs_extent_mismatches'].append(info)
    for g, rows in aliases.items():
        if len({r['table_len'] for r in rows}) > 1:
            result['alias_length_disagreements'].append(dict(group=g, rows=rows))

    for ci in range(20):
        arc = DA.arcade_scripts(ci)
        blob, _bsd = CG.ps2_tail(ci)
        _offs, spans = CG.ps2_spans(blob)
        ps2 = DA.ps2_scripts(blob, spans)

        for sec, scripts in arc.items():
            for si, cells in enumerate(scripts):
                pcells = ps2[sec][si] if si < len(ps2[sec]) else None
                shape_ok = pcells is not None and len(pcells) == len(cells)
                for k, c in enumerate(cells):
                    result['stats']['cells'] += 1
                    raw = c['num']
                    rm = CG.remap(raw, ci)
                    verdict, g, n, L = classify(rm, lengths)
                    result['stats'][verdict if verdict != 'ok' else 'ok'] += 1
                    ps2num0 = pcells[k]['num'] if shape_ok else None
                    # Census of every cell that lands in a group that is NOT the
                    # character's own (own_group == ci + 1) — cg_audit.py's
                    # class (c) `c_mismatch_other_group`.  Ranked by whether the
                    # landing group can be loaded at all, which is what decides
                    # whether a wrong sprite is ever actually drawn.
                    if verdict != 'gap' and g not in (None, 0, ci + 1) \
                            and ps2num0 is not None and ps2num0 != rm:
                        key = "%d" % g
                        cen = result['wrong_group_census'].setdefault(
                            key, dict(group=g, cells=0, chars={},
                                      reachability=reachability(g, owners)[0],
                                      loaded_when=reachability(g, owners)[1]))
                        cen['cells'] += 1
                        cen['chars'][NAMES[ci]] = cen['chars'].get(NAMES[ci], 0) + 1
                    if not verdict.startswith('residual'):
                        continue
                    ps2num = ps2num0
                    pv = classify(ps2num, lengths)[0] if ps2num is not None else None
                    val, note = wild_offset(g, n, lengths)
                    kind, why = reachability(g, owners)
                    result['script_violations'].append(dict(
                        cls=verdict, char=NAMES[ci], table=sec, script=si,
                        cell=c['cell'], raw=raw, remapped=rm, group=g,
                        residual=n, table_len=L,
                        after_terminator=bool(c['after_term']),
                        ps2=ps2num, ps2_verdict=pv,
                        pre_existing_in_ps2=(pv is not None and pv.startswith('residual')),
                        read_u32=val, read_note=note,
                        group_reachability=kind, group_loaded_when=why,
                        owning_group_of_char=ci + 1))

        # PS2 control: the same walk over the shipped PS2 scripts.
        for sec, scripts in ps2.items():
            for si, cells in enumerate(scripts):
                for c in cells:
                    verdict, g, n, L = classify(c['num'], lengths)
                    if verdict.startswith('residual') or verdict == 'ogt_oob':
                        result['script_violations_ps2_control'].append(dict(
                            cls=verdict, char=NAMES[ci], table=sec, script=si,
                            cell=c['cell'], cg=c['num'], group=g, residual=n,
                            table_len=L, after_terminator=bool(c['after_term'])))

        # ---- OVCT parts_char -> cg_number  (eff01.c:169)
        off, size = CG.LOC[ci]['ovct']
        a_cnt = size // OVCT_ELEM
        a_char = [struct.unpack_from('>H', CG.ROM, off + i * OVCT_ELEM + OVCT_PARTS_CHAR_OFF)[0]
                  for i in range(a_cnt)]
        b, z = spans[SECTIONS.index('ovct')]
        p_cnt = z // OVCT_ELEM
        p_char = [struct.unpack_from('<H', blob, b + i * OVCT_ELEM + OVCT_PARTS_CHAR_OFF)[0]
                  for i in range(p_cnt)]
        # arcade_char_data.c:670, :679-685 — parts_char is overwritten from PS2
        # for i < common_count and kept RAW past it.
        common = min(a_cnt, p_cnt)
        post = p_char[:common] + a_char[common:]
        # This loop is deliberately reachability-blind: it bounds-checks EVERY
        # table slot. `part_reachable` says whether any writer of the part
        # index can ever land on that slot (cg_audit.ovct_reachability, doc
        # §24) — a violation on an unreachable slot is latent data, not a door.
        # If that closure is UNMODELLED for this character (an emitted olc index
        # past the OVIX, cg_audit._closure), it is an under-approximation and
        # `part_reachable = False` would be a benign verdict resting on it, so
        # every slot is reported reachable instead (doc §31.8).
        _rr = CG.ovct_reachability(ci)['arcade']
        part_reach = set(_rr['reach'])
        reach_unmodelled = bool(_rr['unmodelled'])
        for i, v in enumerate(post):
            verdict, g, n, L = classify(v, lengths)
            if verdict == 'ok' or verdict == 'gap':
                continue
            kind, why = reachability(g, owners) if g else ('N/A', 'index is past obj_group_table')
            result['ovct_violations'].append(dict(
                cls=verdict, char=NAMES[ci], part=i, parts_char=v, group=g,
                residual=n, table_len=L, patched_from_ps2=(i < common),
                part_reachable=(i in part_reach or reach_unmodelled),
                part_reach_unmodelled=reach_unmodelled,
                arcade_parts=a_cnt, ps2_parts=p_cnt,
                group_reachability=kind, group_loaded_when=why))
        for i, v in enumerate(p_char):
            verdict, g, n, L = classify(v, lengths)
            if verdict in ('ok', 'gap'):
                continue
            result['ovct_violations_ps2_control'].append(dict(
                cls=verdict, char=NAMES[ci], part=i, parts_char=v,
                group=g, residual=n, table_len=L))

    return result


# ---------------------------------------------------------------- report
def main():
    res = audit()
    json.dump(res, open(os.path.join(HERE, "residual_audit.json"), "w"), indent=1)

    print("constants: obj_group_table=%d texgrpdat=%d groups_named=%d"
          % (OGT_N, len(TGD), len(res['groups'])))
    print()
    print("R1 — offset-table length per texture group "
          "(*(u32*)trans_table / 4, mtrans.c:2533)")
    hdr = "%-4s %-6s %-7s %-7s %-8s %-8s %-6s %s" % (
        "grp", "apfn", "num1st", "cg_hi", "extent", "tbl_len", "short", "loaded when")
    print(hdr)
    print("-" * len(hdr))
    for gs in sorted(res['groups'], key=int):
        g = res['groups'][gs]
        who = res['group_owners'][gs]
        print("%-4d %-6d %-7d %-7d %-8d %-8d %-6d %s"
              % (g['group'], g['apfn'], g['num_of_1st'], g['cg_hi'], g['extent'],
                 g['table_len'], g['shortfall'],
                 ", ".join(who) if who else "(no ldreq_tbl type-1 entry)"))
    print()
    ok_struct = all(g['monotonic'] and g['entries_in_range'] and g['self_consistent']
                    for g in res['groups'].values())
    print("structural validation (strictly increasing, every entry in "
          "[4*len, to_tex], entry0 == 4*len): %s for all %d groups"
          % ("PASS" if ok_struct else "FAIL", len(res['groups'])))
    # A negative residual needs a group whose obj_group_table run STARTS below
    # its own num_of_1st.  If none does, `n -= texgrpdat[i].num_of_1st` can
    # never go negative for any value that survives the `i == 0` early-out --
    # which also makes getObjectHeight's u16 arithmetic (mtrans.c:377, where a
    # negative would wrap to ~65500 instead) unreachable by this route.
    neg = [g for g in res['groups'].values() if g['cg_lo'] < g['num_of_1st']]
    print("groups whose obj_group_table run starts below their own num_of_1st "
          "(a negative residual needs one): %d%s"
          % (len(neg), "" if not neg else " -> " + str([g['group'] for g in neg])))
    mism = [g for g in res['groups'].values() if g['table_len'] != g['extent']]
    print("groups where table_len != obj_group_table extent: %d" % len(mism))
    for g in mism:
        print("   group %-3d extent=%-6d table_len=%-6d  %s"
              % (g['group'], g['extent'], g['table_len'],
                 "SHORT BY %d — cg %d..%d are unbacked"
                 % (g['shortfall'], g['num_of_1st'] + g['table_len'], g['cg_hi'])
                 if g['shortfall'] else "table has %d spare entries"
                 % (g['table_len'] - g['extent'])))
    if res['alias_length_disagreements']:
        print("ALIASED ROWS DISAGREE ON LENGTH:", res['alias_length_disagreements'])
    else:
        print("aliased texgrpdat rows all agree on table_len: yes")
    print()

    st = res['stats']
    print("R2 — residual bounds check, all 20 characters x 10 script tables")
    print("  cells walked      : %d" % st['cells'])
    print("  in bounds         : %d" % st['ok'])
    print("  blank / table gap : %d" % st['gap'])
    print("  obj_group_table OOB (class (a), cg_audit.py's check) : %d" % st['ogt_oob'])
    print("  residual < 0                                         : %d" % st['residual_negative'])
    print("  residual >= offset-table length                      : %d" % st['residual_past_end'])
    print()
    v = res['script_violations']
    live = [x for x in v if not x['after_terminator'] and not x['pre_existing_in_ps2']]
    print("  script violations : %d total, %d pre-terminator and not pre-existing in PS2"
          % (len(v), len(live)))
    if v:
        h = "%-7s %-5s %-4s %-4s %-7s %-7s %-4s %-7s %-7s %-6s %s" % (
            "char", "table", "scr", "cell", "raw", "remap", "grp", "residual",
            "tbl_len", "afterT", "reachability")
        print("  " + h)
        print("  " + "-" * len(h))
        for x in v:
            print("  %-7s %-5s %-4d %-4d %-7d %-7d %-4d %-7d %-7d %-6s %s"
                  % (x['char'], x['table'], x['script'], x['cell'], x['raw'],
                     x['remapped'], x['group'], x['residual'], x['table_len'],
                     x['after_terminator'], x['group_loaded_when']))
            print("        PS2 counterpart cg=%s (%s) | ((u32*)trans_table)[%d] = %s — %s"
                  % (x['ps2'], x['ps2_verdict'], x['residual'],
                     ("0x%08X" % x['read_u32']) if x['read_u32'] is not None else "n/a",
                     x['read_note']))
    print()
    pc = res['script_violations_ps2_control']
    pre = [x for x in pc if not x['after_terminator']]
    print("  PS2 control walk  : %d hits, %d of them pre-terminator "
          "(post-terminator hits are decoder artefacts — see doc §15.7)"
          % (len(pc), len(pre)))
    for x in pre:
        print("     %s" % x)
    print()

    print("R3 — where the wrong-group (class (c)) cells land, ranked by reachability")
    cen = sorted(res['wrong_group_census'].values(), key=lambda x: -x['cells'])
    h = "%-5s %-7s %-16s %-46s %s" % ("grp", "cells", "reachability", "loaded when", "characters")
    print("  " + h)
    print("  " + "-" * len(h))
    for c in cen:
        chars = ", ".join("%s(%d)" % (k, v) for k, v in
                          sorted(c['chars'].items(), key=lambda kv: -kv[1]))
        print("  %-5d %-7d %-16s %-46s %s"
              % (c['group'], c['cells'], c['reachability'], c['loaded_when'][:46], chars))
    print("  total wrong-group cells: %d" % sum(c['cells'] for c in cen))
    print()

    print("R2b — OVCT parts_char -> cg_number (eff01.c:169)")
    ov = res['ovct_violations']
    print("  post-adaptation violations : %d  (every table slot, reachable or not)" % len(ov))
    for x in ov:
        print("     %-7s part %-4d parts_char=%-7d %-18s patched_from_ps2=%-5s part_reachable=%-5s %s"
              % (x['char'], x['part'], x['parts_char'], x['cls'],
                 x['patched_from_ps2'], x['part_reachable'], x['group_loaded_when']))
    print("  on a REACHABLE part         : %d  (cg_audit.ovct_reachability, doc §24 — the invariant)"
          % len([x for x in ov if x['part_reachable']]))
    print("  PS2 control                : %d" % len(res['ovct_violations_ps2_control']))
    for x in res['ovct_violations_ps2_control']:
        print("     %s" % x)
    print()
    print("wrote", os.path.join(HERE, "residual_audit.json"))


if __name__ == "__main__":
    main()
