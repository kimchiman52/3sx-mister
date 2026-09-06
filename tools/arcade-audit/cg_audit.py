#!/usr/bin/env python3
"""
Exhaustive arcade-vs-PS2 CG/index audit for all 20 characters.

Constants are PARSED FROM SOURCE (no hand-copied tables):
  src/arcade/arcade_char_data.c            location_data[], cg_maps[], remap_cg_number()
  src/sf33rd/Source/Game/rendering/texgroup.c   texgrpdat[100] (num_of_1st, apfn, to_chd)
  src/sf33rd/Source/Game/rendering/chren3rd.c   obj_group_table[37664]
  src/sf33rd/Source/Game/effect/effxx.c         effinitjptbl[59]
  src/sf33rd/Source/Game/engine/charset.c       decode_chcmd[125]
  src/sf33rd/Source/Game/effect/eff13.c         tama_data[243]
  src/sf33rd/Source/Game/effect/eff41.c         sa_sign_data[69]
  src/sf33rd/Source/Game/sound/se_data.c        sound_effect_request[1024]
Data sources:
  rom.bin                     decrypted CPS3 sfiii3nr1 (decrypt.py; SIMM sha256 == rom_load.c:41-45)
  SF33RD.AFS                  PS2 game data (AFS entry apfn, tail at to_chd)
"""
import json, re, struct, sys, os

import os as _os

_HERE = _os.path.dirname(_os.path.abspath(__file__))
# Repo root = <repo>/tools/arcade-audit/.. /.. — works in any worktree.
REPO = _os.environ.get("ARCADE_AUDIT_REPO") or _os.path.abspath(_os.path.join(_HERE, "..", ".."))
AFS_PATH = _os.environ.get("ARCADE_AUDIT_AFS") or _os.path.expanduser(
    "~/Library/Application Support/CrowdedStreet/3S-ARM/resources/SF33RD.AFS")
ROM_PATH = _os.environ.get("ARCADE_AUDIT_ROM") or _os.path.join(_HERE, "rom.bin")
ZIP_PATH = _os.environ.get("ARCADE_AUDIT_ROMZIP") or _os.path.expanduser(
    "~/Library/Application Support/CrowdedStreet/3S-ARM/resources/sfiii3nr1.zip")
HERE = _HERE
ROM = open(ROM_PATH, "rb").read()
BASE_OFFSET = 0x6000000

NAMES = ["GILL","ALEX","RYU","YUN","DUDLEY","NECRO","HUGO","IBUKI","ELENA","ORO",
         "YANG","KEN","SEAN","URIEN","AKUMA","CHUNLI","MAKOTO","Q","TWELVE","REMY"]
SECTIONS = ['nmca','dmca','btca','caca','cuca','atca','saca','exca','cbca','yuca','stxy','mvxy',
            'sernd','ovct','ovix','rict','hiit','boda','hana','cata','caua','atta','hosa','atit','prot']
# charid.c:87-96  wk->char_table[koc] = <section>
KOC2SEC = {0:'nmca',1:'dmca',2:'caca',3:'cuca',4:'atca',5:'saca',6:'btca',7:'exca',8:'cbca',9:'yuca'}

def src(p): return open(os.path.join(REPO, p)).read()

# ---------------------------------------------------------------- constants
def parse_location_data():
    s = src("src/arcade/arcade_char_data.c")
    body = s[s.index("static const LocationData location_data[NUM_CHARS] = {"):]
    pairs = re.findall(r'\.(\w+)\s*=\s*\{\s*\.offset\s*=\s*(0x[0-9A-Fa-f]+),\s*\.size\s*=\s*(0x[0-9A-Fa-f]+)\s*\}', body)
    out, cur = [], {}
    for k, o, z in pairs:
        if k in cur: out.append(cur); cur = {}
        cur[k] = (int(o, 16), int(z, 16))
    out.append(cur)
    assert len(out) == 20, len(out)
    return out

def parse_cg_maps():
    s = src("src/arcade/arcade_char_data.c")
    ranges = {}
    for m in re.finditer(r'static const CgRemapRange (\w+)\[\]\s*=\s*\{(.*?)\};', s, re.S):
        rs = []
        for r in re.finditer(r'\{\s*\.first\s*=\s*(\w+),\s*\.last\s*=\s*(\w+),\s*\.delta\s*=\s*(-?\w+)\s*\}', m.group(2)):
            def num(t):
                if t == 'UINT16_MAX': return 0xFFFF
                neg = t.startswith('-'); t2 = t.lstrip('-')
                v = int(t2, 16) if t2.lower().startswith('0x') else int(t2)
                return -v if neg else v
            rs.append((num(r.group(1)), num(r.group(2)), num(r.group(3))))
        ranges[m.group(1)] = rs
    blk = re.search(r'static const CharacterCgMap cg_maps\[NUM_CHARS\]\s*=\s*\{(.*?)\n\};', s, re.S).group(1)
    maps = {}
    for m in re.finditer(r'\[CHAR_(\w+)\]\s*=\s*\{(.*?)\}\s*,\s*\n', blk + "\n", re.S):
        name, b = m.group(1), m.group(2)
        d = re.search(r'\.default_delta\s*=\s*(-?\w+)', b).group(1)
        neg = d.startswith('-'); d2 = d.lstrip('-')
        dd = (int(d2, 16) if d2.lower().startswith('0x') else int(d2)) * (-1 if neg else 1)
        rr = re.search(r'\.ranges\s*=\s*(\w+)', b)
        maps[name] = {'default_delta': dd, 'ranges': ranges.get(rr.group(1), []) if rr else []}
    assert len(maps) == 20, sorted(maps)
    return [maps[n] for n in NAMES]

def parse_texgrpdat():
    s = src("src/sf33rd/Source/Game/rendering/texgroup.c")
    body = s[s.index("const TexGroupData texgrpdat[100] = {"):]
    body = body[:body.index("};", body.index("texgrpdat[100]"))]
    rows = []
    for m in re.finditer(r'\{\s*(-?\d+)\s*,\s*(-?\d+)[^,]*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(0x[0-9A-Fa-f]+|-?\d+)\s*\}', body):
        g = m.groups()
        rows.append(dict(num_of_1st=int(g[0]), apfn=int(g[1]), conv=int(g[2]), ix1st=int(g[3]),
                         use=int(g[4]), to_tex=int(g[5]),
                         to_chd=int(g[6], 16) if g[6].lower().startswith('0x') else int(g[6])))
    return rows

def parse_obj_group_table():
    s = src("src/sf33rd/Source/Game/rendering/chren3rd.c")
    i = s.index("const u8 obj_group_table[37664]"); j = s.index("{", i); k = s.index("};", j)
    v = [int(x) for x in re.findall(r'\d+', s[j+1:k])]
    assert len(v) == 37664, len(v)
    return v

def count_fnptr_table(path, decl):
    s = src(path); i = s.index(decl); j = s.index("{", i)
    d, k = 0, j
    while True:
        if s[k] == '{': d += 1
        elif s[k] == '}':
            d -= 1
            if d == 0: break
        k += 1
    return len([x for x in s[j+1:k].split(',') if x.strip()])

LOC   = parse_location_data()
CGMAP = parse_cg_maps()
TGD   = parse_texgrpdat()
OGT   = parse_obj_group_table()
N_EFFINIT = count_fnptr_table("src/sf33rd/Source/Game/effect/effxx.c", "const s32 (*effinitjptbl[59])() = {")
N_CHCMD   = count_fnptr_table("src/sf33rd/Source/Game/engine/charset.c", "s32 (*const decode_chcmd[125])() = {")
N_SE      = count_fnptr_table("src/sf33rd/Source/Game/sound/se_data.c", "const se_request sound_effect_request[1024] = {")
N_TAMA    = int(re.search(r'const TAMA tama_data\[(\d+)\]', src("src/sf33rd/Source/Game/effect/eff13.c")).group(1))
N_SASIGN  = int(re.search(r'const s16 sa_sign_data\[(\d+)\]\[5\]', src("src/sf33rd/Source/Game/effect/eff41.c")).group(1))
OGT_N     = len(OGT)
# Parsed, not hardcoded (doc §8, item 7 of the 2026-08-31 cleanup pass): a
# hardcoded 0x400 here would silently desync from the compiler if
# CG_REMAP_CUTOFF ever moved within its _Static_assert's allowed band
# (<= 0x601, arcade_char_data.c).
CG_REMAP_CUTOFF = int(
    re.search(r'#define CG_REMAP_CUTOFF (0x[0-9A-Fa-f]+)', src("src/arcade/arcade_char_data.c")).group(1), 16)

# ---------------------------------------------------------------- range-overlap guard
def check_range_overlaps():
    """Mirrors arcade_char_data.c's validate_cg_ranges(): remap_cg_number
    takes the first matching row and stops, so a later row that shadows an
    earlier one in the same character's table would silently remap to the
    wrong delta with no diagnostic. validate_cg_ranges() is `#if DEBUG`,
    and DEBUG is only defined for CMAKE_BUILD_TYPE=Debug
    (CMakeLists.txt); every shipping pipeline (tools/mister/build-game.sh)
    configures Release, so that guard protects nothing in a shipped build
    (doc §8, item 6 of the 2026-08-31 cleanup pass). This is the guard that
    actually runs, every time the audit runs, regardless of build config."""
    overlaps = []
    for ci in range(20):
        ranges = CGMAP[ci]['ranges']
        for i in range(len(ranges)):
            a = ranges[i]
            if a[0] > a[1]:
                overlaps.append(dict(character=NAMES[ci], kind='inverted', row=i, range=a))
            for j in range(i + 1, len(ranges)):
                b = ranges[j]
                if a[0] <= b[1] and b[0] <= a[1]:
                    overlaps.append(dict(character=NAMES[ci], kind='overlap', row_a=i, a=a, row_b=j, b=b))
    return overlaps

# ---------------------------------------------------------------- remap (mirrors arcade_char_data.c:85-109)
def remap(value, ci):
    if value < CG_REMAP_CUTOFF: return value
    m = CGMAP[ci]; delta = m['default_delta']
    for (f, l, d) in m['ranges']:
        if f <= value <= l: delta = d; break
    adj = value + delta
    return value if (adj < 0 or adj > 0xFFFF) else adj

# Commands whose handler transfers control and never falls through to the next
# cell in linear order (charset.c: check_cm_extended_code breaks when the handler
# returns 0; comm_end rewrites cg_ix). Used ONLY to bound the LAST script of a
# table, whose end offset is location.size and is over-declared for some
# characters (see the "over-declared section size" finding).
TERMINATORS = {1, 2, 3, 4, 6, 17, 19, 21, 23, 25, 27, 29, 31, 69, 102, 115}

# ---------------------------------------------------------------- arcade parsing (mirrors read_char_table)
def arc_offsets(off, size):
    e, p = [], off
    while True:
        v = struct.unpack_from('>I', ROM, p)[0]; p += 4
        if v == 0: break
        e.append(v - BASE_OFFSET - off)
        if len(e) > 4000: raise RuntimeError("runaway offset table")
    return e

def arc_parse(ci, sec, idx, tabs):
    off, size = LOC[ci][sec]; ents = tabs[sec]
    so = sorted(set(ents)); start = ents[idx] - 8
    nxt = [o for o in so if o > ents[idx]]
    last = not nxt
    end = (nxt[0] - 8) if nxt else size
    if start < 0 or off + end > len(ROM) or end <= start: return None, []
    p = off + start
    cgd = struct.unpack_from('>h', ROM, p)[0]
    if cgd not in (1, 2, 4, 6): return cgd, []
    out, q, lim = [], p + 8, off + end
    while q + 8 <= lim:
        code = struct.unpack_from('>H', ROM, q)[0]
        if code < 0x100:
            koc, ix, pat = struct.unpack_from('>hhh', ROM, q + 2)
            out.append(('C', code, koc, ix, pat)); q += 8 + max(cgd * 4 - 8, 0)
            if last and code in TERMINATORS: break
        else:
            se, olc, num = struct.unpack_from('>HHH', ROM, q + 2)
            r = dict(type=code & 0xFF, ctr=code >> 8, se=se, olc=olc, num=num)
            q2 = q + 8
            if cgd >= 4:
                att, hit = struct.unpack_from('>hH', ROM, q2)
                ext, canc, eff, eft = ROM[q2+4], ROM[q2+5], ROM[q2+6], ROM[q2+7]
                r.update(att=att, hit=hit, eff=eff, eftype=eft); q2 += 8
            if cgd == 6: q2 += 8
            out.append(('L', r)); q = q2
    return cgd, out

# ---------------------------------------------------------------- PS2 parsing (AFS tail + 25-offset header)
AFS = open(AFS_PATH, 'rb')
assert AFS.read(4) == b'AFS\x00'
_cnt = struct.unpack('<I', AFS.read(4))[0]
AFS_ENT = [struct.unpack('<II', AFS.read(8)) for _ in range(_cnt)]

def ps2_tail(ci):
    bsd = TGD[ci + 1]
    off, size = AFS_ENT[bsd['apfn']]
    AFS.seek(off + bsd['to_chd'])
    return AFS.read(size - bsd['to_chd']), bsd

def ps2_spans(blob):
    n = 25; offs = list(struct.unpack_from('<25I', blob, 0)); L = len(blob); sp = []
    for s in range(n):
        st = offs[s]; en = L
        for i in range(n):
            if offs[i] > st and offs[i] < en: en = offs[i]
        sp.append((st, en - st))
    return offs, sp

def ps2_offsets(blob, base):
    e, p = [], base
    while True:
        v = struct.unpack_from('<I', blob, p)[0]; p += 4
        if v == 0: break
        e.append(v)
        if len(e) > 4000: raise RuntimeError("runaway")
    return e

def ps2_parse(blob, base, size, ents, idx):
    so = sorted(set(ents)); start = ents[idx] - 8
    nxt = [o for o in so if o > ents[idx]]
    last = not nxt
    end = (nxt[0] - 8) if nxt else size
    if start < 0 or end <= start or base + end > len(blob): return None, []
    p = base + start
    cgd = struct.unpack_from('<h', blob, p)[0]
    if cgd not in (1, 2, 4, 6): return cgd, []
    out, q, lim = [], p + 8, base + end
    while q + 8 <= lim:
        code = struct.unpack_from('<H', blob, q)[0]
        if code < 0x100:
            koc, ix, pat = struct.unpack_from('<hhh', blob, q + 2)
            out.append(('C', code, koc, ix, pat)); q += 8 + max(cgd * 4 - 8, 0)
            if last and code in TERMINATORS: break
        else:
            se, olc, num = struct.unpack_from('<HHH', blob, q + 2)
            r = dict(type=code & 0xFF, ctr=code >> 8, se=se, olc=olc, num=num)
            q2 = q + 8
            if cgd >= 4:
                hit, att = struct.unpack_from('<Hh', blob, q2)
                ext, canc, eff, eft = blob[q2+4], blob[q2+5], blob[q2+6], blob[q2+7]
                r.update(att=att, hit=hit, eff=eff, eftype=eft); q2 += 8
            if cgd == 6: q2 += 8
            out.append(('L', r)); q = q2
    return cgd, out

# ---------------------------------------------------------------- OVCT reachability (doc §24)
# Which OVCT parts can eff01.c ever index for a character? An `arcade_count >
# ps2_count` tail (Elena, parts 85-90) is only a hazard if some part index in
# it is reachable. Every writer of the part index, from the code:
#   charset.c   check_cgd_data:  wk->cg_olc_ix >>= 4; wk->cg_olc = wk->olc_ix_table[wk->cg_olc_ix];
#               (both copies) -> the cell's olc word >> 4 selects an OVIX entry,
#               whose four s16 slots are the part indices, one per overlap type
#   plcnt.c     plcnt_init:      wk->wu.cg_olc_ix = 0            (OVIX[0])
#   plpdm.c     Player_damage:   wk->wu.cg_olc_ix = datadrs[3]   (exdm_ix_data[b][character][3], NOT shifted)
#   eff01.c     effect_01_move:  restart at the master's part index, then on
#               timer expiry cg_ix = parts_nix if nonzero else cg_ix + 1
#               (get_new_parts_data adds 1 only when the master's
#               player_number == 0, i.e. the character is Gill)
# So: seeds = OVIX[e].olc_ix[0..3] for every e a cell (or plcnt/plpdm) can
# emit, then the timer walk's closure over parts_nix. The walk is modelled
# with NO timing constraint (any hold length), so the closure is an upper
# bound on what the C can index. A negative or past-the-end index is recorded
# in `past_end`, not expanded: the C would read outside the table there.
OVCT_ELEM, OVIX_ELEM = 16, 8
OVCT_NIX_OFF = 12          # OverlapPart.parts_nix (u16), structs.h

def _olc_indices(cells):
    pre, post = set(), set()
    term = False
    for c in cells:
        if c[0] == 'C':
            if c[1] in TERMINATORS: term = True
            continue
        (post if term else pre).add(c[1]['olc'] >> 4)
    return pre, post

def emitted_olc_indices(ci):
    """(pre-terminator, post-terminator) sets of `olc >> 4` over every L cell in
    every script of the character's ten arcade tables."""
    tabs = {sec: arc_offsets(*LOC[ci][sec]) for sec in KOC2SEC.values()}
    pre, post = set(), set()
    for sec in KOC2SEC.values():
        for si in range(len(tabs[sec])):
            a, b = _olc_indices(arc_parse(ci, sec, si, tabs)[1])
            pre |= a; post |= b
    return pre, post

def ps2_olc_indices(ci):
    blob, bsd = ps2_tail(ci)
    offs, sp = ps2_spans(blob)
    pre, post = set(), set()
    for sec in KOC2SEC.values():
        b, z = sp[SECTIONS.index(sec)]
        ents = ps2_offsets(blob, b)
        for si in range(len(ents)):
            a, c = _olc_indices(ps2_parse(blob, b, z, ents, si)[1])
            pre |= a; post |= c
    return pre, post

def arc_ovix(ci):
    off, size = LOC[ci]['ovix']
    return [struct.unpack_from('>4h', ROM, off + i * OVIX_ELEM) for i in range(size // OVIX_ELEM)]

def arc_ovct_nix(ci):
    off, size = LOC[ci]['ovct']
    return [struct.unpack_from('>H', ROM, off + i * OVCT_ELEM + OVCT_NIX_OFF)[0] for i in range(size // OVCT_ELEM)]

def ps2_ovix_nix(ci):
    blob, bsd = ps2_tail(ci)
    offs, sp = ps2_spans(blob)
    b, z = sp[SECTIONS.index('ovix')]
    ovix = [struct.unpack_from('<4h', blob, b + i * OVIX_ELEM) for i in range(z // OVIX_ELEM)]
    b, z = sp[SECTIONS.index('ovct')]
    nix = [struct.unpack_from('<H', blob, b + i * OVCT_ELEM + OVCT_NIX_OFF)[0] for i in range(z // OVCT_ELEM)]
    return ovix, nix

def parse_exdm_olc_ix():
    """plpdm.c exdm_ix_data[2][20][5]: column [3] is written straight into
    cg_olc_ix (unshifted) with the CHARACTER as the middle subscript
    (plcnt.c: wk->player_number = My_char[ix]). Returns {character: set}."""
    s = src("src/sf33rd/Source/Game/engine/plpdm.c")
    i = s.index("const u16 exdm_ix_data[2][20][5] = {"); j = s.index("};", i)
    rows = re.findall(r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', s[i:j])
    assert len(rows) == 40, len(rows)
    out = {ci: set() for ci in range(20)}
    for n, row in enumerate(rows):
        out[n % 20].add(int(row[3]))
    return out

EXDM_OLC_IX = parse_exdm_olc_ix()

def _closure(ovix, nix, olc_indices):
    ovix_oob = sorted(e for e in olc_indices if e >= len(ovix))
    seeds = set()
    for e in olc_indices:
        if 0 <= e < len(ovix):
            for v in ovix[e]:
                if v != 0: seeds.add(v)
    reach, past_end, stack = set(), set(), sorted(seeds)
    while stack:
        p = stack.pop()
        if p in reach or p in past_end: continue
        if p < 0 or p >= len(nix): past_end.add(p); continue
        reach.add(p)
        stack.append(nix[p] if nix[p] else p + 1)
    return dict(seeds=sorted(seeds), reach=sorted(reach), past_end=sorted(past_end), ovix_oob=ovix_oob)

_REACH_CACHE = {}

def ovct_reachability(ci):
    """Arcade-data reachable OVCT part set for character ci, plus the same
    model run over the PS2 data as the §6.1 control. Cached per character."""
    if ci in _REACH_CACHE: return _REACH_CACHE[ci]
    pre, post = emitted_olc_indices(ci)
    idx = pre | post | {0} | EXDM_OLC_IX[ci]
    a = _closure(arc_ovix(ci), arc_ovct_nix(ci), idx)
    a['ovix_oob_pre'] = sorted(e for e in pre if e >= len(arc_ovix(ci)))
    a['ovix_oob_post'] = sorted(e for e in post if e >= len(arc_ovix(ci)))
    ppre, ppost = ps2_olc_indices(ci)
    povix, pnix = ps2_ovix_nix(ci)
    p = _closure(povix, pnix, ppre | ppost | {0} | EXDM_OLC_IX[ci])
    r = dict(arcade=a, ps2=p, arcade_entries=len(arc_ovct_nix(ci)), ps2_entries=len(pnix))
    _REACH_CACHE[ci] = r
    return r

# ---------------------------------------------------------------- SA naming for saca scripts
def sa_labels(ci):
    """map saca script index -> list of SA-table slots that select it (asstbl.c 9900_g/_a arcade rows)."""
    s = src("src/bin2obj/asstbl.c")
    lab = {}
    arcade_ci = ci + 1 if ci > 14 else ci   # CHAR_3SX_TO_ARCADE, constants.h:62 (CHAR_AKUMA=14)
    for tname, tag in (("asstbl_lv_9900_g_arcade", "g"), ("asstbl_lv_9900_a_arcade", "a")):
        i = s.index("const AS %s[21][72] = {" % tname); j = s.index("{", i)
        d, k = 0, j
        while True:
            if s[k] == '{': d += 1
            elif s[k] == '}':
                d -= 1
                if d == 0: break
            k += 1
        body = s[j+1:k]; rows, depth, st = [], 0, None
        for p, ch in enumerate(body):
            if ch == '{':
                if depth == 0: st = p
                depth += 1
            elif ch == '}':
                depth -= 1
                if depth == 0: rows.append(body[st+1:p])
        row = rows[arcade_ci]
        for n, e in enumerate(re.findall(r'\{([^{}]*)\}', row)):
            nums = re.findall(r'=\s*(-?\d+)', e)
            if len(nums) == 3:
                lab.setdefault(int(nums[1]), []).append("9900_%s[%d]" % (tag, n))
    return lab

# ---------------------------------------------------------------- audit
def audit(cgmap_override=None, quiet=False):
    global CGMAP
    saved = CGMAP
    if cgmap_override is not None: CGMAP = cgmap_override
    result = {}
    for ci in range(20):
        rec = dict(name=NAMES[ci], own_group=ci + 1, violations=[], stats={})
        arc_tabs = {}
        for koc, sec in KOC2SEC.items():
            arc_tabs[sec] = arc_offsets(*LOC[ci][sec])
        blob, bsd = ps2_tail(ci)
        offs, sp = ps2_spans(blob)
        ps2_tabs = {}
        for koc, sec in KOC2SEC.items():
            b, z = sp[SECTIONS.index(sec)]
            ps2_tabs[sec] = (b, z, ps2_offsets(blob, b))
        salab = sa_labels(ci)
        cells_seen = 0
        cls = dict(a_oob=0, b_gap=0, c_wrong_group=0, c_same_group=0, needs_manual=0,
                   extra_script=0, extra_cells=0,
                   se_oob=0, eff_oob=0, tama_oob=0, sasign_oob=0, code_oob=0, koc_oob=0, idx_oob=0)
        for koc, sec in KOC2SEC.items():
            an, pn = len(arc_tabs[sec]), len(ps2_tabs[sec][2])
            for si in range(an):
                acgd, acells = arc_parse(ci, sec, si, arc_tabs)
                pcells = None
                if si < pn:
                    pcgd, pcells = ps2_parse(blob, ps2_tabs[sec][0], ps2_tabs[sec][1], ps2_tabs[sec][2], si)
                elif acells:
                    # si >= pn: the PS2 offset table is SHORTER than the arcade
                    # one (a real 0x00000000 terminator word, not truncation --
                    # doc §11.2), so this script has NO PS2 counterpart at all --
                    # not "shape differs" (needs_manual below), no oracle exists,
                    # period. Previously invisible: pcells stayed None and
                    # needs_manual's `pcells is not None` guard skipped it
                    # entirely. Counted here so it shows up somewhere.
                    lcells = [c for c in acells if c[0] == 'L']
                    if lcells:
                        cls['extra_script'] += 1
                        cls['extra_cells'] += len(lcells)
                        rec['violations'].append(dict(cls='extra_script_no_oracle', table=sec, script=si,
                                                      arc_cells=len(acells), l_cells=len(lcells)))
                shape_ok = (pcells is not None and len(pcells) == len(acells)
                            and all(a[0] == p[0] for a, p in zip(acells, pcells)))
                if pcells is not None and not shape_ok and acells:
                    cls['needs_manual'] += 1
                    rec['violations'].append(dict(cls='needs_manual_diff', table=sec, script=si,
                                                  arc_cells=len(acells),
                                                  ps2_cells=(len(pcells) if pcells is not None else None),
                                                  sa=salab.get(si) if sec == 'saca' else None))
                for cidx, c in enumerate(acells):
                    pcell = pcells[cidx] if (shape_ok) else None
                    # A value identical on both sides is a PRE-EXISTING property of the
                    # shipped PS2 data, not an arcade-adaptation defect; only divergences
                    # are in scope. `same` == True -> skip.
                    def same(field, idx=None):
                        if pcell is None: return False
                        if pcell[0] == 'C' and idx is not None: return pcell[idx] == field
                        if pcell[0] == 'L' and isinstance(idx, str): return pcell[1].get(idx) == field
                        return False
                    if c[0] == 'C':
                        code, kc, ix, pat = c[1], c[2], c[3], c[4]
                        pre = (pcell is not None and pcell[0] == 'C'
                               and pcell[1] == code and pcell[2] == kc and pcell[3] == ix)
                        if pre: continue
                        if code >= N_CHCMD:
                            cls['code_oob'] += 1
                            rec['violations'].append(dict(cls='a_code_oob', table=sec, script=si, cell=cidx, code=code))
                        if code in (3, 4, 5):   # jmp/jpss/jsr
                            if kc < 0 or kc >= 12:
                                cls['koc_oob'] += 1
                                rec['violations'].append(dict(cls='a_koc_oob', table=sec, script=si, cell=cidx, koc=kc, ix=ix))
                            elif kc in KOC2SEC:
                                nn = len(arc_tabs[KOC2SEC[kc]])
                                if ix < 0 or ix >= nn:
                                    cls['idx_oob'] += 1
                                    rec['violations'].append(dict(cls='a_script_idx_oob', table=sec, script=si, cell=cidx,
                                                                  dest=KOC2SEC[kc], ix=ix, dest_entries=nn))
                            else:
                                cls['koc_oob'] += 1
                                rec['violations'].append(dict(cls='a_koc_unset', table=sec, script=si, cell=cidx, koc=kc))
                        if code == 43:          # comm_exec
                            if kc < 0 or kc >= N_EFFINIT:
                                cls['eff_oob'] += 1
                                rec['violations'].append(dict(cls='a_effinit_oob', table=sec, script=si, cell=cidx, eff=kc, data=ix))
                            elif kc == 2 and ix >= N_TAMA:
                                cls['tama_oob'] += 1
                                rec['violations'].append(dict(cls='a_tama_oob', table=sec, script=si, cell=cidx, tama=ix))
                            elif kc == 13 and ix >= N_SASIGN:
                                cls['sasign_oob'] += 1
                                rec['violations'].append(dict(cls='a_sasign_oob', table=sec, script=si, cell=cidx, idx=ix))
                        continue
                    r = c[1]; cells_seen += 1
                    pr = pcell[1] if (pcell is not None and pcell[0] == 'L') else None
                    se = r['se'] >> 4
                    # cg_se >>= 4 then bit 0x800 selects the per-character random-SE
                    # table (charset.c:2721-2727); only the non-random path indexes
                    # sound_effect_request[] directly.
                    if (se & 0x800) == 0 and se >= N_SE and not (pr and pr['se'] == r['se']):
                        cls['se_oob'] += 1
                        rec['violations'].append(dict(cls='a_se_oob', table=sec, script=si, cell=cidx, se=se))
                    ef, eft = r.get('eff', 0), r.get('eftype', 0)
                    if ef and not (pr and pr.get('eff') == ef and pr.get('eftype') == eft):
                        if ef >= N_EFFINIT:
                            cls['eff_oob'] += 1
                            rec['violations'].append(dict(cls='a_effinit_oob', table=sec, script=si, cell=cidx, eff=ef, data=eft))
                        elif ef == 2 and eft >= N_TAMA:
                            cls['tama_oob'] += 1
                            rec['violations'].append(dict(cls='a_tama_oob', table=sec, script=si, cell=cidx, tama=eft))
                        elif ef == 13 and eft >= N_SASIGN:
                            cls['sasign_oob'] += 1
                            rec['violations'].append(dict(cls='a_sasign_oob', table=sec, script=si, cell=cidx, idx=eft))
                    raw = r['num']; rm = remap(raw, ci)
                    grp = OGT[rm] if rm < OGT_N else None
                    ps2num = pcells[cidx][1]['num'] if (shape_ok and pcells[cidx][0] == 'L') else None
                    v = dict(table=sec, script=si, cell=cidx, raw=raw, remapped=rm, group=grp,
                             ps2=ps2num, ps2_group=(OGT[ps2num] if (ps2num is not None and ps2num < OGT_N) else None),
                             confidence=('high' if shape_ok else 'low-shape-differs'),
                             sa=salab.get(si) if sec == 'saca' else None)
                    if rm >= OGT_N:
                        cls['a_oob'] += 1; v['cls'] = 'a_ogt_oob'; rec['violations'].append(v)
                    elif grp == 0 and rm != 0:
                        cls['b_gap'] += 1; v['cls'] = 'b_group_gap'; rec['violations'].append(v)
                    elif ps2num is not None and ps2num != rm:
                        if grp != ci + 1:
                            cls['c_wrong_group'] += 1; v['cls'] = 'c_mismatch_other_group'
                        else:
                            cls['c_same_group'] += 1; v['cls'] = 'c_mismatch_own_group'
                        rec['violations'].append(v)
        # OVCT / OVIX counts
        a_ovct = LOC[ci]['ovct'][1] // 16; a_ovix = LOC[ci]['ovix'][1] // 8
        p_ovct = sp[SECTIONS.index('ovct')][1] // 16; p_ovix = sp[SECTIONS.index('ovix')][1] // 8
        over = []
        for koc2, sec2 in KOC2SEC.items():
            off2, size2 = LOC[ci][sec2]; mx = max(arc_tabs[sec2])
            if size2 - mx > 0x400:
                over.append(dict(table=sec2, declared=size2, max_script_offset=mx, slack=size2 - mx,
                                 ps2_span=sp[SECTIONS.index(sec2)][1]))
        rec['over_declared_sections'] = over
        # OVCT reachability (doc §24). `ovct_unpatched_tail` is the COUNT of
        # parts past common_count (kept raw by Apply3SXRenderingConventions);
        # `ovct_reach_unpatched` is how many of those any writer can index.
        # The tail is a hazard only when the second number is nonzero.
        rr = ovct_reachability(ci)
        common = min(a_ovct, p_ovct)
        reach = rr['arcade']['reach']
        # Part sets are stored as [lo, hi] runs so the JSON stays readable
        # (Ibuki's reachable set alone is 2,285 indices).
        def runs(xs):
            out = []
            for x in xs:
                if out and x == out[-1][1] + 1: out[-1][1] = x
                else: out.append([x, x])
            return ["%d" % a if a == b else "%d-%d" % (a, b) for a, b in out]
        rec['ovct_reachability'] = dict(
            arcade=dict(entries=rr['arcade_entries'], seeds=runs(rr['arcade']['seeds']),
                        reach=runs(reach), past_end=rr['arcade']['past_end'],
                        ovix_oob_pre_terminator=rr['arcade']['ovix_oob_pre'],
                        ovix_oob_post_terminator=rr['arcade']['ovix_oob_post']),
            ps2=dict(entries=rr['ps2_entries'], seeds=runs(rr['ps2']['seeds']),
                     reach=runs(rr['ps2']['reach']), past_end=rr['ps2']['past_end'],
                     ovix_oob=rr['ps2']['ovix_oob']))
        rec['stats'] = dict(cells=cells_seen, ovct_arcade=a_ovct, ovct_ps2=p_ovct,
                            ovix_arcade=a_ovix, ovix_ps2=p_ovix,
                            ovct_unpatched_tail=max(0, a_ovct - p_ovct),
                            ovct_reach_max=(max(reach) if reach else -1),
                            ovct_reach_unpatched=len([p for p in reach if p >= common]),
                            ovct_walk_past_end=rr['arcade']['past_end'],
                            ovct_walk_past_end_ps2=rr['ps2']['past_end'],
                            ovix_oob_pre_terminator=rr['arcade']['ovix_oob_pre'],
                            ovix_arcade_shorter_by=max(0, p_ovix - a_ovix), **cls)
        result[NAMES[ci]] = rec
    CGMAP = saved
    return result

if __name__ == "__main__":
    print("constants: obj_group_table=%d effinitjptbl=%d decode_chcmd=%d sound_effect_request=%d tama_data=%d sa_sign_data=%d cg_remap_cutoff=0x%x"
          % (OGT_N, N_EFFINIT, N_CHCMD, N_SE, N_TAMA, N_SASIGN, CG_REMAP_CUTOFF))

    overlaps = check_range_overlaps()
    if overlaps:
        print("FATAL: %d CgRemapRange overlap/inversion(s) found (doc §8, item C/6):" % len(overlaps))
        for o in overlaps:
            if o['kind'] == 'inverted':
                print("  %s row %d: first > last: %r" % (o['character'], o['row'], o['range']))
            else:
                print("  %s: row %d %r overlaps row %d %r" % (o['character'], o['row_a'], o['a'], o['row_b'], o['b']))
        sys.exit(1)
    print("range-overlap check: 0 overlaps, 0 inversions (20/20 characters)")

    res = audit()
    json.dump(res, open(os.path.join(HERE, "cg_audit.json"), "w"), indent=1)
    hdr = ("%-7s %5s | %4s %4s %5s %5s %5s %5s | %5s %5s %5s %5s %5s %5s %5s | %s"
           % ("char","cells","(a)","(b)","(c)wg","(c)og","manu","extra","se","eff","tama","sasi","code","koc","sidx","ovct a/p reach  ovix a/p"))
    print(hdr); print("-"*len(hdr))
    T = {}
    def ovct_flag(s):
        # doc §24: the tail is a hazard only if a reachable part index lands in it.
        if s['ovct_reach_unpatched']:
            return "TAIL-REACHED(%d)!" % s['ovct_reach_unpatched']
        if s['ovct_walk_past_end']:
            return "walk>end%s%s" % (s['ovct_walk_past_end'], "(ps2 too)" if s['ovct_walk_past_end_ps2'] else "(arcade-only)")
        if s['ovct_unpatched_tail']:
            return "tail-unreached(%d)" % s['ovct_unpatched_tail']
        return "ok"
    for n in NAMES:
        r = res[n]; s = r['stats']
        for k, v in s.items(): T[k] = T.get(k, 0) + (v if isinstance(v, int) else 0)
        print("%-7s %5d | %4d %4d %5d %5d %5d %5d | %5d %5d %5d %5d %5d %5d %5d | %d/%d r<=%d %s  %d/%d %s"
              % (n, s['cells'], s['a_oob'], s['b_gap'], s['c_wrong_group'], s['c_same_group'], s['needs_manual'],
                 s['extra_script'],
                 s['se_oob'], s['eff_oob'], s['tama_oob'], s['sasign_oob'], s['code_oob'], s['koc_oob'], s['idx_oob'],
                 s['ovct_arcade'], s['ovct_ps2'], s['ovct_reach_max'], ovct_flag(s),
                 s['ovix_arcade'], s['ovix_ps2'], "short" if s['ovix_arcade_shorter_by'] else "ok"))
    print("-"*len(hdr))
    print("TOTAL         | %4d %4d %5d %5d %5d %5d | %5d %5d %5d %5d %5d %5d %5d"
          % (T['a_oob'], T['b_gap'], T['c_wrong_group'], T['c_same_group'], T['needs_manual'], T['extra_script'],
             T['se_oob'], T['eff_oob'], T['tama_oob'], T['sasign_oob'], T['code_oob'], T['koc_oob'], T['idx_oob']))
    print("cells audited:", T['cells'])
