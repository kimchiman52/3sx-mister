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
  src/sf33rd/Source/Game/engine/plpdm.c         exdm_ix_data[2][20][5]  (OVCT reachability, doc §24)
  src/sf33rd/Source/Game/engine/hitcheck.c      sel_hs_add_tbl[6] + 16  (dangling-walk hold model, doc §25)
  src/sf33rd/Source/Game/effect/effk7.c         K7_move_type_0 case 3 routine / case 4 marker  (X.C.O.P.Y. reverse swap, doc §26)
  src/sf33rd/Source/Game/engine/plpatuni.c      Att_METAMOR_REBIRTH -> set_char_move_init(koc, ix)
  src/sf33rd/Source/Game/engine/plpatNN.c       plNN_exatt_table[18] (all 20 dispatch the rebirth routine to Att_METAMOR_REBIRTH)
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
                r.update(att=att, hit=hit, ext=ext, canc=canc, eff=eff, eftype=eft); q2 += 8
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
                r.update(att=att, hit=hit, ext=ext, canc=canc, eff=eff, eftype=eft); q2 += 8
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

# ---------------------------------------------------------------- dangling-walk hold model (doc §25)
# A walk that leaves its table (`past_end` above) is a hazard only if the
# master can HOLD the selecting `olc` for as long as eff01.c's timer walk needs
# to get there. Modelled from the code, not from play:
#
#   eff01.c  effect_01_move   `--cg_ctr` runs once per frame in which
#                             !Game_pause && !EXE_flag && mwk->sa_stop_flag == 0
#                             and the master's cg_olc.olc_ix[type] still equals
#                             the cached selection (any change restarts at the
#                             seed; 0 goes dormant). Game_pause/EXE_flag freeze
#                             the player too (plcnt.c Player_control), so they
#                             are symmetric and drop out.
#   charset.c check_cgd_patdat  the master's selection changes only on a cell
#                             decode, and a player decodes cells only from
#                             char_move() -- which plmain.c check_hit_stop()
#                             withholds while hit_stop > 0 (a NEGATIVE hit_stop
#                             calls char_move itself, so it holds nothing).
#
# So the hold, in effect frames, is: the run's own script frames (sum of `ctr`
# over the consecutive cells that select the same OVIX index) + every POSITIVE
# hit_stop applied to the master while it is on those cells. Every writer of a
# player's hit_stop that leaves the player on its current script:
#   hitcheck.c  dm_status_copy     as->hit_stop = as->att.hs_me          own contact (hit or guard)
#   hitcheck.c  set_paring_status  as->hit_stop = sel_hs_add_tbl[i] + 16  own attack parried
#   plpdm.c     damage_atemi_setup ek->hit_stop = wk->att.hs_you        own attack absorbed by an
#                                  atemi (comm_atmf scripts: Dudley saca[65..68], Remy saca[50..53])
#   plcnt.c     aiuchi KO          2 / 4  (both players already in damage; below the parry value)
# Everything else moves the player to a damage/catch/caught state first --
# hitplpl.c plef_at_vs_player_damage_union writes ds->routine_no[1] = 1 and
# routine_no[3] = 0 at contact, so check_hit_stop()'s dm_stop branch reaches
# Player_damage (a new script, hence a new `olc`) on the next frame. A contact
# needs att_hit_ok, which only a RENEWAL cell (negative `att`, charset.c
# set_new_attnum) sets and which hitcheck.c clears on any contact, so each
# renewal cell in the run buys at most ONE positive hit_stop.
#
# The model is deliberately conservative where the script is not a plain run:
# a run that contains a C command (a loop or a jump could re-enter it, comm_stop
# could freeze the master mid-run) or that touches a script boundary (the hold
# could continue from another script) is reported `unmodelled`, which keeps the
# exit flagged as reachable. Only a run bounded on both sides by an L cell
# selecting a different OVIX index, with no C cell inside, gets a bound.
def parse_parry_hit_stop():
    """hitcheck.c set_paring_status: `as->wu.hit_stop = sel_hs_add_tbl[hsadix] + 16`."""
    s = src("src/sf33rd/Source/Game/engine/hitcheck.c")
    tbl = re.search(r'const s16 sel_hs_add_tbl\[\d+\] = \{([^}]*)\};', s)
    add = re.search(r'hit_stop = sel_hs_add_tbl\[hsadix\] \+ (\d+);', s)
    assert tbl and add, "hitcheck.c parry hit-stop not found"
    return max(int(v) for v in tbl.group(1).split(',')) + int(add.group(1))

def arc_atit_hs(ci):
    """(max positive hs_me, max |hs_you|) over the character's arcade ATIT
    records (structs.h UNK_7: 16 bytes, hs_me at +12, hs_you at +13, s8)."""
    off, size = LOC[ci]['atit']
    me = you = 0
    for i in range(size // 16):
        a, b = struct.unpack_from('>bb', ROM, off + i * 16 + 12)
        me = max(me, a); you = max(you, abs(b))
    return me, you

def _all_cells(ci):
    tabs = {sec: arc_offsets(*LOC[ci][sec]) for sec in KOC2SEC.values()}
    for sec in KOC2SEC.values():
        for si in range(len(tabs[sec])):
            cgd, cells = arc_parse(ci, sec, si, tabs)
            yield sec, si, cgd, cells

_ATEMI_CACHE = {}
def atemi_hit_stop_max():
    """Max |hs_you| over the ATIT of every character whose arcade scripts carry
    a `comm_atmf` (decode_chcmd[100]) with a nonzero koc -- the value
    damage_atemi_setup hands the ATTACKER as hit_stop is the atemi performer's
    current att.hs_you, and its whole ATIT bounds that whatever was loaded."""
    if 'v' in _ATEMI_CACHE: return _ATEMI_CACHE['v']
    who, best = [], 0
    for ci in range(20):
        if any(c[0] == 'C' and c[1] == 100 and c[2] != 0 for _, _, _, cells in _all_cells(ci) for c in cells):
            who.append(NAMES[ci]); best = max(best, arc_atit_hs(ci)[1])
    _ATEMI_CACHE['v'] = (best, who)
    return _ATEMI_CACHE['v']

def _walk_frames(nix, timers, seed):
    """Effect frames from seed until the walk index leaves the table (or None if
    it never does). parts_timer is a u8 loaded into the u8 cg_ctr and
    pre-decremented, so a 0 timer is 256 frames."""
    p, total, seen = seed, 0, set()
    while 0 <= p < len(nix):
        if p in seen: return None
        seen.add(p); total += timers[p] or 256
        p = nix[p] if nix[p] else p + 1
    return total

def olc_runs(ci, k):
    """Maximal runs of consecutive cells selecting OVIX index k in every arcade
    script of the character. Each run: table, script, cell span, script frames
    (sum of ctr), renewal cells, and why it is unmodelled if it is."""
    out = []
    for sec, si, cgd, cells in _all_cells(ci):
        n = len(cells); i = 0
        while i < n:
            c = cells[i]
            if not (c[0] == 'L' and (c[1]['olc'] >> 4) == k): i += 1; continue
            j = i; frames = renew = 0; ccodes = []
            while j < n and not (cells[j][0] == 'L' and (cells[j][1]['olc'] >> 4) != k):
                if cells[j][0] == 'C': ccodes.append(cells[j][1])
                else:
                    r = cells[j][1]; frames += r['ctr']
                    # charset.c check_cgd_patdat: `cg_att_ix >>= 6` (arithmetic), and
                    # set_new_attnum re-arms att_hit_ok iff the result is negative --
                    # i.e. iff the s16 `att` word itself is negative.
                    if 'att' in r and r['att'] < 0: renew += 1
                j += 1
            why = None
            if ccodes: why = "C cells inside run: %s" % ccodes
            elif i == 0: why = "run starts at script start"
            elif j >= n: why = "run reaches script end"
            out.append(dict(table=sec, script=si, cells="%d-%d" % (i, j - 1), frames=frames, renewals=renew, unmodelled=why))
            i = j
    return out

def ovct_dangling_hold(ci, rr):
    """For every arcade walk exit in rr['arcade']['past_end']: the seeds whose
    walk reaches it, the frames the walk needs, every olc run that can install
    each seed, and the bound on how long the master can hold it."""
    nix = arc_ovct_nix(ci); ovix = arc_ovix(ci)
    off, size = LOC[ci]['ovct']
    timers = [ROM[off + i * OVCT_ELEM + 8] for i in range(size // OVCT_ELEM)]   # OverlapPart.parts_timer (u8 at +8)
    own_me, _ = arc_atit_hs(ci)
    parry = parse_parry_hit_stop()
    atemi, atemi_who = atemi_hit_stop_max()
    hs_max = max(own_me, parry, atemi, 4)
    hs = dict(own_hs_me=own_me, parry=parry, atemi=atemi, atemi_characters=atemi_who, aiuchi_ko=4, per_renewal=hs_max)
    exits = {}
    for exit_ in rr['arcade']['past_end']:
        seeds = {}
        for s in rr['arcade']['seeds']:
            p, seen = s, set()
            while 0 <= p < len(nix) and p not in seen:
                seen.add(p); p = nix[p] if nix[p] else p + 1
            if p != exit_: continue
            need = _walk_frames(nix, timers, s)
            ks = sorted(k for k, e in enumerate(ovix) if s in e)
            runs = [dict(olc=k, **r) for k in ks for r in olc_runs(ci, k)]
            exdm = sorted(e for e in EXDM_OLC_IX[ci] if e in ks)
            for r in runs:
                r['hold_max'] = None if r['unmodelled'] else r['frames'] + r['renewals'] * hs_max
            holds = [r['hold_max'] for r in runs]
            unmod = [r for r in runs if r['unmodelled']] or exdm
            hold_max = None if unmod else (max(holds) if holds else 0)
            seeds[s] = dict(need=need, olc=ks, exdm_olc=exdm, runs=runs, hold_max=hold_max,
                            reachable=(hold_max is None or hold_max >= need))
        exits[exit_] = dict(seeds=seeds, reachable=any(v['reachable'] for v in seeds.values()) or not seeds)
    return dict(hit_stop=hs, exits=exits)


# ---------------------------------------------------------------- X.C.O.P.Y. reverse swap (doc §26)
# effk7.c K7_move_type_0 rebinds the master's tables to the TARGET's on a
# `cg_type 20` cell (case 0) and back to Twelve's on a `cg_type 30` cell
# (case 4).  At either swap the master's cg_olc still holds the selection its
# current cell decoded against the OLD character's OVIX, and eff01.c consumes
# it against the NEW overlap_char_tbl the same frame (Game2_1: Player_control,
# then reqPlayerDraw -> move_effect_work(6) is K7, then Basic_Sub_Ex ->
# move_effect_work(1) is the overlay, then hit_check_main_process).  §25.5
# closed the forward swap by data.  This closes the reverse one:
#
#   arming   case 4 is written only by case 3, which forces the master to
#            routine (4, RNO, 0), cg_type 0, cg_hit_ix 0, cg_ja = hit_ix_table[0].
#            plpat.c Player_attack -> plxx_extra_attack_table[player_number] ->
#            plNN_exatt_table[RNO - 16] == Att_METAMOR_REBIRTH in all 20 tables
#            (whatever character the master is bound to) -> set_char_move_init
#            (KOC, IX): the target's saca[1], decoded from cell 0 at once.
#   window   case 3 fires at frame N.  saca[1] goes in at N+1; its own
#            `cg_type 30` cell decodes at N+1+sum(ctr before it) and case 4
#            fires that frame and frees K7 (case 5 -> routine 2).  For case 4
#            to fire on ANY OTHER cell the master must leave saca[1] first.
#            Every writer of a live player's routine/script, from the code:
#            - contact (hitcheck.c): the master must be in the hit queue --
#              hit_push_request skips cg_hit_ix == 0, so the cells before the
#              marker must decode to hit index 0 (check_cgd_patdat case 4:
#              ((att<<16 | hit) * 8) >> 16 & 0x1FF).  At frame N it was queued
#              with its old cg_hit_ix, but case 3 already re-pointed
#              h_bod/h_han/h_att/h_hos/h_cau at hit_ix_table[0]
#              (set_jugde_area), so hiit[0]'s rows must all be empty
#              (attack_hit_check: dmdat_adrs[i][1] == 0 -> continue;
#              catch_hit_check: sh[1] == 0 -> continue).
#            - process_attack (pls00.c, run by check_lever_data BEFORE the
#              state handler every frame): the cancel_timer block needs
#              cancel_timer != 0 (zeroed every normal-state frame by
#              setup_normal_process_flags, set only by plpat.c
#              get_cancel_timer, which Att_METAMOR_REBIRTH never calls);
#              check_ashimoto_ex needs bs2_on_car (set only under
#              Bonus_Game_Flag == 20, where Att_METAMORPHOSE never creates
#              K7); check_cg_cancel_data needs cg_cancel != 0 -- the cells
#              before the marker must carry canc 0; jumping_cg_type_check
#              leaves the state on cg_type 0xFF/64/2/3/7, and 31/40 are
#              Att_METAMOR_REBIRTH's own branches -- the cells before the
#              marker must be type 0.
#            - the N+1 pre-empt: at N+1 process_attack runs on the cell that
#              was current when case 3 fired (saca[1] is not in yet), with
#              sw_lvbt forced 0 by metamor_over (plmain.c Player_move, cleared
#              only later that frame by Att_METAMOR_REBIRTH case 0).
#              check_cg_cancel_data can still install a script from a
#              buffered command and a stale meoshi_hit_flag if that cell's
#              canc has a bit in K7_CANCEL_BITS.  Which cell can be current:
#              case 3 needs routine_no[1] == 0, guard_flag != 3, hit_stop == 0.
#              Normal state runs nmca.  A transition INTO normal written inside
#              a state handler leaves that state's script current for the
#              frame: from attack only on a K7_END_TYPES cell (Player_attack
#              sets guard_flag 3 first; jumping_guard_type_check is the only
#              clear inside it), never from catch/caught (plpca.c/plpcu.c set
#              3), from damage on any dmca/btca cell (Damage_04000 etc. set 0).
#              A transition written outside Player_control (hitcheck.c parry)
#              gets its nmca install in the next Player_control, before K7.
#              yuca is win/lose only (animation/*.c), where pcon_rno[0] == 2
#              sends case 3 to state 9 instead.
#            - settle (plcnt.c): every routine write is behind footwork_check
#              (normal AND standing) or nekorobi_check (damage) or a fresh
#              init_app_30000; move_player_work keeps the rebirth moving.
#            - Game_pause/EXE_flag freeze K7 too; the opponent's SA stop
#              (comm_stop -> hit_stop > 0) withholds char_move while K7 waits
#              on cg_type: nothing reorders the cells.
#            - round init / training reset: erase_extra_plef_work frees list 6
#              (K7 is id 207 on list 6) and set_base_data(_tiny) restores
#              My_char.  Netplay rollback restores the whole pool.
#   marker   the saca[1] marker cell must select olc 0 (every overlay dormant
#            at the rebind); the cells after it up to the first C cell are
#            decoded against TWELVE's OVIX once the tables are rebound and must
#            stay inside Twelve's OVIX/OVCT (the C cell then jumps through the
#            rebound char_table into Twelve's own scripts).
#
# Anything the model has not read is reported `unmodelled` and keeps the gate
# OPEN.  Independently, k7_foreign_cells() computes for every `cg_type 30`
# cell with a live selection outside saca[1] what a swap there WOULD consume:
# the target's OVIX entry's parts on Twelve's OVCT and the following cells'
# olc on Twelve's OVIX, with the PS2 data as the §6.1 control.  A foreign
# cell is a hazard only if the gate is open AND the consequence leaves
# Twelve's tables.
K7_CANCEL_BITS = 0x68          # check_cg_cancel_data: 0x40 SA, 0x20 special/taunt, 0x08 meoshi -- the paths that can
                               # install a script from a buffered command / stale meoshi_hit_flag.  0x04 (check_nm_attack)
                               # cannot: it needs shot_data_convert(sw_now) >= 0, i.e. one of shot_prio's six buttons,
                               # and with sw_lvbt forced 0 the only bits pl_lvr_set (cmd_main.c) can put in sw_0 are the
                               # release-derived 0x80/0x800, which shot_prio does not list.  0x10 (renda) rewinds the
                               # current script without a routine write; 0x02/0x01 need meoshi_hit_flag AND a lever.
K7_END_TYPES = (0xFF, 64, 2, 3, 7)   # pls00.c jumping_guard_type_check: the only cg_types that clear guard_flag in Player_attack
K7_ENTRY_TABLES = ('nmca', 'dmca', 'btca')      # scripts that can be current when case 3 fires (see above)
K7_ATTACK_TABLES = ('atca', 'saca', 'exca', 'cbca')
K7_HIT_IX_MASK = 0x1FF
K7_BOX_STRIDE = dict(boda=32, hana=32, cata=8, caua=8, atta=32, hosa=8)   # structs.h UNK_1..UNK_6
TWELVE = NAMES.index('TWELVE')

_K7_CACHE = {}
def parse_k7_rebirth():
    """effk7.c K7_move_type_0: case 3 writes the master's routine (4, RNO, 0) and case 4 waits for
    `cg_type != MARKER`; plpatuni.c Att_METAMOR_REBIRTH installs (KOC, IX); every plNN_exatt_table
    must dispatch RNO - 16 to Att_METAMOR_REBIRTH (plpat.c Player_attack indexes it by player_number,
    i.e. by whichever character the master is bound to)."""
    if 'v' in _K7_CACHE: return _K7_CACHE['v']
    k7 = src("src/sf33rd/Source/Game/effect/effk7.c")
    body = k7[k7.index("void K7_move_type_0(WORK_Other* ewk, PLW* mwk) {"):]
    m3 = re.search(r'case 3:.*?mwk->wu\.routine_no\[1\] = (\d+);\s*mwk->wu\.routine_no\[2\] = (\d+);\s*mwk->wu\.routine_no\[3\] = (\d+);', body, re.S)
    m4 = re.search(r'case 4:\s*if \(mwk->wu\.cg_type != (\d+)\)', body)
    assert m3 and m4 and m3.group(1) == '4' and m3.group(3) == '0', "effk7.c K7_move_type_0 case 3/4 not found"
    rno, marker = int(m3.group(2)), int(m4.group(1))
    uni = src("src/sf33rd/Source/Game/engine/plpatuni.c")
    fb = uni[uni.index("void Att_METAMOR_REBIRTH(PLW* wk) {"):]
    mi = re.search(r'set_char_move_init\(&wk->wu, (\d+), (\d+)\);', fb)
    assert mi, "Att_METAMOR_REBIRTH install not found"
    koc, ix = int(mi.group(1)), int(mi.group(2))
    ok = 0
    for f in sorted(os.listdir(os.path.join(REPO, "src/sf33rd/Source/Game/engine"))):
        if not re.match(r'plpat\d\d\.c$', f): continue
        t = re.sub(r'//[^\n]*', '', src("src/sf33rd/Source/Game/engine/" + f))
        m = re.search(r'exatt_table\[(\d+)\]\)\(PLW\*\s*\w*\)\s*=\s*\{(.*?)\};', t, re.S)
        if not m: continue
        ents = [e.strip() for e in m.group(2).split(',') if e.strip()]
        assert len(ents) == int(m.group(1)), f
        if ents[rno - 16] == 'Att_METAMOR_REBIRTH': ok += 1
    assert ok == 20, "Att_METAMOR_REBIRTH is not entry %d of all 20 exatt tables (%d)" % (rno - 16, ok)
    v = dict(rno=rno, marker=marker, table=KOC2SEC[koc], script=ix, tables=ok)
    _K7_CACHE['v'] = v
    return v

def k7_hit_ix(r):
    """charset.c check_cgd_patdat case 4: st.w.h = cg_att_ix; st.w.l = cg_hit_ix; st.l *= 8; cg_hit_ix = st.w.h & 0x1FF."""
    return ((((r['att'] & 0xFFFF) << 16) | r['hit']) * 8 >> 16) & K7_HIT_IX_MASK

def hiit0_boxes(ci):
    """hit_ix_table[0] (charid.c: wk->hit_ix_table = cdat->hiit; structs.h UNK_0: boix bhix haix mf caix
    cuix atix hoix, 8 x u16) and every box row a DEFENDER is tested on through it: body_dm[boix] and
    hand_dm[bhix + haix] (4 rows each), att_box[atix] rows 2-3, hos_box[hoix] (attack_hit_check's
    dmdat_adrs[0..10]) and cau_box[cuix] (catch_hit_check).  A row whose [1] is 0 is skipped."""
    off, size = LOC[ci]['hiit']
    h = struct.unpack_from('>8H', ROM, off)
    boix, bhix, haix, mf, caix, cuix, atix, hoix = h
    def row(sec, idx):
        o, z = LOC[ci][sec]; st = K7_BOX_STRIDE[sec]
        assert (idx + 1) * st <= z, (NAMES[ci], sec, idx)
        return struct.unpack_from('>%dh' % (st // 2), ROM, o + idx * st)
    body, hand, att = row('boda', boix), row('hana', bhix + haix), row('atta', atix)
    hos, cau = row('hosa', hoix), row('caua', cuix)
    live = [body[i * 4 + 1] for i in range(4)] + [hand[i * 4 + 1] for i in range(4)] + [att[9], att[13], hos[1], cau[1]]
    return dict(entry=list(h), rows_live=[v for v in live if v], empty=not any(live))

def k7_swap_gate(ci):
    """Can K7_move_type_0 case 4 fire on any cell other than the rebirth script's own marker while the
    master is bound to character ci?  Returns the facts the model rests on and `unmodelled` = the list
    of reasons it cannot close the gate (None when it can)."""
    reb = parse_k7_rebirth()
    tabs = {sec: arc_offsets(*LOC[ci][sec]) for sec in KOC2SEC.values()}
    cgd, cells = arc_parse(ci, reb['table'], reb['script'], tabs)
    why = []
    k = next((i for i, c in enumerate(cells) if c[0] == 'L' and c[1]['type'] == reb['marker']), None)
    pre = cells[:k] if k is not None else cells
    if k is None: why.append("rebirth script has no cg_type %d cell" % reb['marker'])
    if cgd < 4: why.append("rebirth script cgd %d carries no hit/canc words" % cgd)
    preC = [c[1] for c in pre if c[0] == 'C']
    if preC: why.append("C cells before the marker: %s" % preC)
    preL = [c[1] for c in pre if c[0] == 'L']
    bad = [(i, r['type']) for i, r in enumerate(preL) if r['type'] != 0]
    if bad: why.append("non-zero cg_type before the marker: %s" % bad)
    hix = [k7_hit_ix(r) for r in preL] if cgd >= 4 else []
    if any(hix): why.append("hit index before the marker: %s" % hix)
    canc = [r['canc'] for r in preL] if cgd >= 4 else []
    if any(canc): why.append("cancel bits before the marker: %s" % canc)
    boxes = hiit0_boxes(ci)
    if not boxes['empty']: why.append("hit_ix_table[0] selects live box rows: %s" % boxes['rows_live'])
    marker_olc = (cells[k][1]['olc'] >> 4) if k is not None else None
    if marker_olc: why.append("marker cell selects olc %d" % marker_olc)
    tw_ovix, tw_nix = arc_ovix(TWELVE), arc_ovct_nix(TWELVE)
    tail = []
    if k is not None:
        for c in cells[k + 1:]:
            if c[0] == 'C': break
            tail.append(c[1]['olc'] >> 4)
    tail_bad = [e for e in tail if e >= len(tw_ovix) or any(p and not (0 <= p < len(tw_nix)) for p in tw_ovix[e])]
    if tail_bad: why.append("cells after the marker select outside Twelve's tables: %s" % tail_bad)
    preempt = []
    for sec in K7_ENTRY_TABLES:
        for si in range(len(tabs[sec])):
            for i, c in enumerate(arc_parse(ci, sec, si, tabs)[1]):
                if c[0] == 'L' and c[1].get('canc', 0) & K7_CANCEL_BITS:
                    preempt.append(dict(table=sec, script=si, cell=i, type=c[1]['type'], canc=c[1]['canc']))
    for sec in K7_ATTACK_TABLES:
        for si in range(len(tabs[sec])):
            for i, c in enumerate(arc_parse(ci, sec, si, tabs)[1]):
                if c[0] == 'L' and c[1]['type'] in K7_END_TYPES and c[1].get('canc', 0) & K7_CANCEL_BITS:
                    preempt.append(dict(table=sec, script=si, cell=i, type=c[1]['type'], canc=c[1]['canc']))
    if preempt: why.append("cells that can be current at arming and carry a script-installing cancel bit: %d" % len(preempt))
    return dict(rebirth=dict(table=reb['table'], script=reb['script'], routine=reb['rno'], marker=reb['marker'],
                             marker_cell=k, cells_before=len(preL), frames_before=sum(r['ctr'] for r in preL),
                             hit_ix_before=hix, canc_before=canc, marker_olc=marker_olc, tail_olc=tail),
                frame_n=boxes, preempt_cells=preempt, unmodelled=(why or None))

def _k7_consequence(parts, tail, t_ovix, t_nix):
    """What a swap consumes on Twelve's tables: `parts` (the target's OVIX entry, restarted on Twelve's
    OVCT) and `tail` (the following cells' olc, decoded against Twelve's OVIX)."""
    if parts is None: return dict(oob=True, detail=[["target-ovix-oob", None]])
    oob = []
    for p in parts:
        if not p: continue
        if not (0 <= p < len(t_nix)): oob.append(["part", p])
        elif _walk_frames(t_nix, [1] * len(t_nix), p) is not None: oob.append(["walk-exit", p])
    for e in tail:
        if e >= len(t_ovix): oob.append(["tail-olc", e])
        else:
            for p in t_ovix[e]:
                if p and not (0 <= p < len(t_nix)): oob.append(["tail-part", p])
    return dict(oob=bool(oob), detail=oob)

def k7_foreign_cells(ci):
    """Every `cg_type 30` cell outside the rebirth script that selects a live olc -- the cells the gate
    protects -- with what case 4 WOULD consume there, arcade and PS2 (§6.1 control)."""
    reb = parse_k7_rebirth()
    tw_ovix, tw_nix = arc_ovix(TWELVE), arc_ovct_nix(TWELVE)
    ptw_ovix, ptw_nix = ps2_ovix_nix(TWELVE)
    ovix = arc_ovix(ci); povix, pnix = ps2_ovix_nix(ci)
    blob, bsd = ps2_tail(ci); offs, sp = ps2_spans(blob)
    out = []
    for sec, si, cgd, cells in _all_cells(ci):
        if sec == reb['table'] and si == reb['script']: continue
        # cells past a terminating C command never execute (doc §19/§24.5:
        # decoder artefacts such as Yang's olc 1264); they are reported `dead`.
        term, dead = False, set()
        for i, c in enumerate(cells):
            if c[0] == 'C' and c[1] in TERMINATORS: term = True
            elif term: dead.add(i)
        hits = [i for i, c in enumerate(cells) if c[0] == 'L' and c[1]['type'] == reb['marker'] and (c[1]['olc'] >> 4)]
        if not hits: continue
        b, z = sp[SECTIONS.index(sec)]; ents = ps2_offsets(blob, b)
        pcells = ps2_parse(blob, b, z, ents, si)[1] if si < len(ents) else None
        for i in hits:
            e = cells[i][1]['olc'] >> 4
            parts = list(ovix[e]) if e < len(ovix) else None
            tail = []
            for c2 in cells[i + 1:]:
                if c2[0] == 'C': break
                tail.append(c2[1]['olc'] >> 4)
            same = (pcells is not None and i < len(pcells) and pcells[i][0] == 'L'
                    and pcells[i][1]['type'] == reb['marker'] and (pcells[i][1]['olc'] >> 4) == e)
            ptail = []
            if same:
                for c2 in pcells[i + 1:]:
                    if c2[0] == 'C': break
                    ptail.append(c2[1]['olc'] >> 4)
            out.append(dict(table=sec, script=si, cell=i, olc=e, parts=parts, tail_olc=tail, dead=(i in dead),
                            twelve=_k7_consequence(parts, tail, tw_ovix, tw_nix),
                            ps2_same_cell=same,
                            ps2_twelve=(_k7_consequence(list(povix[e]) if e < len(povix) else None, ptail, ptw_ovix, ptw_nix) if same else None)))
    return out


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
        # Dangling-walk hold model (doc §25): an exit past the table is a
        # hazard only if the master can hold the selecting olc for `need`
        # frames; `ovct_walk_past_end_reachable` lists the exits it can (or
        # that the model cannot bound).
        hold = ovct_dangling_hold(ci, rr)
        rec['ovct_dangling_hold'] = hold
        # X.C.O.P.Y. reverse swap (doc §26): can K7 case 4 fire on a cell that
        # selects a live olc, so that a part index decoded against THIS
        # character's OVIX is consumed against Twelve's table?  `k7_gate` is
        # the arming/window model ('closed' or 'unmodelled'); the foreign-cell
        # consequences are computed regardless of it.
        gate = k7_swap_gate(ci); foreign = k7_foreign_cells(ci)
        rec['xcopy_case4'] = dict(gate=gate, foreign_cells=foreign)
        rec['stats'] = dict(cells=cells_seen, ovct_arcade=a_ovct, ovct_ps2=p_ovct,
                            ovix_arcade=a_ovix, ovix_ps2=p_ovix,
                            ovct_unpatched_tail=max(0, a_ovct - p_ovct),
                            ovct_reach_max=(max(reach) if reach else -1),
                            ovct_reach_unpatched=len([p for p in reach if p >= common]),
                            ovct_walk_past_end=rr['arcade']['past_end'],
                            ovct_walk_past_end_ps2=rr['ps2']['past_end'],
                            ovct_walk_past_end_reachable=[e for e, v in hold['exits'].items() if v['reachable']],
                            ovct_walk_hold=[(e, max(d['hold_max'] for d in v['seeds'].values()),
                                             min(d['need'] for d in v['seeds'].values()))
                                            for e, v in hold['exits'].items() if not v['reachable']],
                            ovix_oob_pre_terminator=rr['arcade']['ovix_oob_pre'],
                            ovix_arcade_shorter_by=max(0, p_ovix - a_ovix),
                            k7_foreign_cells=len([f for f in foreign if not f['dead']]),
                            k7_foreign_dead=len([f for f in foreign if f['dead']]),
                            k7_gate=('closed' if gate['unmodelled'] is None else 'unmodelled'),
                            k7_foreign_oob=len([f for f in foreign if not f['dead'] and f['twelve']['oob']]),
                            k7_foreign_oob_ps2=len([f for f in foreign if not f['dead'] and f['ps2_same_cell'] and f['ps2_twelve']['oob']]),
                            k7_foreign_ps2_differs=len([f for f in foreign if not f['dead'] and not f['ps2_same_cell']]), **cls)
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
           % ("char","cells","(a)","(b)","(c)wg","(c)og","manu","extra","se","eff","tama","sasi","code","koc","sidx","ovct a/p reach  ovix a/p  xcopy"))
    print(hdr); print("-"*len(hdr))
    T = {}
    def ovct_flag(s):
        # doc §24: the tail is a hazard only if a reachable part index lands in it.
        if s['ovct_reach_unpatched']:
            return "TAIL-REACHED(%d)!" % s['ovct_reach_unpatched']
        if s['ovct_walk_past_end_reachable']:
            return "walk>end%s%s" % (s['ovct_walk_past_end_reachable'], "(ps2 too)" if s['ovct_walk_past_end_ps2'] else "(arcade-only)")
        if s['ovct_walk_past_end']:
            # doc §25: the walk leaves the table, but no writer can hold the
            # selecting olc for the frames the walk needs (hold bound / need).
            return "walk>end-unreached[%s]" % ", ".join("%d:hold<=%d/%d" % t for t in s['ovct_walk_hold'])
        if s['ovct_unpatched_tail']:
            return "tail-unreached(%d)" % s['ovct_unpatched_tail']
        return "ok"
    def xcopy_flag(s):
        # doc §26: a `cg_type 30` cell outside the rebirth script that selects a
        # live olc is a hazard only if K7 case 4 can fire there (gate open) AND
        # what it would consume leaves Twelve's tables.
        n, dead = s['k7_foreign_cells'], s['k7_foreign_dead']
        tag = "(%d dead)" % dead if dead else ""
        if n + dead == 0: return "xcopy:none"
        if s['k7_gate'] == 'closed': return "xcopy:gated(%d)%s" % (n + dead, tag)
        if s['k7_foreign_oob']:
            return "xcopy:FOREIGN-OOB(%d/%d)%s!" % (s['k7_foreign_oob'], n, "(ps2 too)" if s['k7_foreign_oob_ps2'] else "(arcade-only)")
        return "xcopy:unmodelled(%d,in-range)%s" % (n, tag)
    for n in NAMES:
        r = res[n]; s = r['stats']
        for k, v in s.items(): T[k] = T.get(k, 0) + (v if isinstance(v, int) else 0)
        print("%-7s %5d | %4d %4d %5d %5d %5d %5d | %5d %5d %5d %5d %5d %5d %5d | %d/%d r<=%d %s  %d/%d %s  %s"
              % (n, s['cells'], s['a_oob'], s['b_gap'], s['c_wrong_group'], s['c_same_group'], s['needs_manual'],
                 s['extra_script'],
                 s['se_oob'], s['eff_oob'], s['tama_oob'], s['sasign_oob'], s['code_oob'], s['koc_oob'], s['idx_oob'],
                 s['ovct_arcade'], s['ovct_ps2'], s['ovct_reach_max'], ovct_flag(s),
                 s['ovix_arcade'], s['ovix_ps2'], "short" if s['ovix_arcade_shorter_by'] else "ok", xcopy_flag(s)))
    print("-"*len(hdr))
    print("TOTAL         | %4d %4d %5d %5d %5d %5d | %5d %5d %5d %5d %5d %5d %5d"
          % (T['a_oob'], T['b_gap'], T['c_wrong_group'], T['c_same_group'], T['needs_manual'], T['extra_script'],
             T['se_oob'], T['eff_oob'], T['tama_oob'], T['sasign_oob'], T['code_oob'], T['koc_oob'], T['idx_oob']))
    print("cells audited:", T['cells'])
