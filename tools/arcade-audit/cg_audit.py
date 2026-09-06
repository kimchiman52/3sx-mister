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
import bisect, collections, json, re, struct, sys, os

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
    m0 = re.search(r'case 0:\s*if \(mwk->wu\.cg_type != (\d+)\)', body)
    assert m3 and m4 and m0 and m3.group(1) == '4' and m3.group(3) == '0', "effk7.c K7_move_type_0 case 0/3/4 not found"
    rno, marker, fwd_marker = int(m3.group(2)), int(m4.group(1)), int(m0.group(1))
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
    v = dict(rno=rno, marker=marker, fwd_marker=fwd_marker, table=KOC2SEC[koc], script=ix, tables=ok)
    _K7_CACHE['v'] = v
    return v

# cmd_main.c cmd_data_set() consumes each command record in this order:
#   reset, w_dead, w_dead2, waza_r[0..3], btix, exdt[0..3]   -- btix is word 7.
K7_BTIX_WORD = 7
K7_CMD_NO_BUTTON = 0x80        # pls03.c: `(btix[i] & 0xFF) == 0x80` -> the entry needs no button word
K7_CMD_SLOTS = ((28, 38), (46, 56))   # pls03.c check_special_attack: ground scan, then air scan

_K7_BTIX_CACHE = {}
def k7_input_words():
    """Which word of the seven-word input window each special-move entry reads.  pls03.c sets
    `conpane = &wk->cp->sw_lvbt` in all eight of its scans and then reads `conpane[btix[i] & 0xFF]`;
    structs.h WORK_CP orders that window sw_lvbt, sw_new, sw_old, sw_now, sw_off, sw_chg, old_now.
    This matters to the §26.3 row-8 pre-empt: `plmain.c` -> `Player_move` forces `sw_lvbt = 0` while
    `metamor_over` is set, but cmd_main.c -> `pl_lvr_set` derives sw_old/sw_off/sw_chg/old_now from
    the PREVIOUS frame, so only sw_lvbt/sw_new/sw_now are actually dead on the frame after arming.
    **Measured**: every entry in all 21 arcade command tables reads word 5 (`sw_chg`) or is the
    0x80 no-button sentinel -- so 'input is dead' does not close the 0x40/0x20 cancel paths."""
    if 'v' in _K7_BTIX_CACHE: return _K7_BTIX_CACHE['v']
    t = re.sub(r'//[^\n]*', '', src("src/arcade/arcade_cmd_data.c"))
    arrs = {}
    for m in re.finditer(r'static const s16 (\w+)\[(\d+)\]\s*=\s*\{(.*?)\};', t, re.S):
        vals = [int(x) for x in re.findall(r'-?\d+', m.group(3))]
        assert len(vals) == int(m.group(2)), m.group(1)
        arrs[m.group(1)] = vals
    words, tables = set(), 0
    for m in re.finditer(r'static const (?:const_s16_arr|void\s*\*)\s*(p[0-9A-Fa-f]+_cmd)\[(\d+)\]\s*=\s*\{(.*?)\};', t, re.S):
        names = [x for x in re.split(r'[,\s]+', m.group(3).strip()) if x]
        if len(names) != 56: continue
        tables += 1
        for lo, hi in K7_CMD_SLOTS:
            for n in names[lo:hi]:
                if n in arrs: words.add(arrs[n][K7_BTIX_WORD] & 0xFF)
    assert tables == 21, "expected 21 arcade command tables, parsed %d" % tables
    v = dict(tables=tables, words=sorted(words),
             live=sorted(w for w in words if w != K7_CMD_NO_BUTTON))
    _K7_BTIX_CACHE['v'] = v
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
    # §26.9 bullet 2, now checked rather than inspected: once case 4 has rebound the tables, the
    # post-marker cells' hit indices are read against TWELVE's hiit (charset.c check_cgd_patdat:
    # `cg_ja = hit_ix_table[cg_hit_ix]`) and the C command that ends the script jumps through the
    # rebound char_table into Twelve's own tables (charset.c set_char_move_init2: char_table[koc][ix]).
    tw_hiit = LOC[TWELVE]['hiit'][1] // 16          # structs.h UNK_0: 8 x u16 per entry
    tail_hix, tail_exit = [], None
    if k is not None and cgd >= 4:
        for c in cells[k + 1:]:
            if c[0] == 'C': break
            tail_hix.append(k7_hit_ix(c[1]))
    hix_bad = [h for h in tail_hix if not (0 <= h < tw_hiit)]
    if hix_bad: why.append("post-marker hit index outside Twelve's %d-entry hiit: %s" % (tw_hiit, hix_bad))
    if k is not None:
        for c in cells[k + 1:]:
            if c[0] == 'C':
                tsec = KOC2SEC.get(c[2])
                tail_exit = dict(code=c[1], koc=c[2], ix=c[3], pat=c[4], table=tsec)
                if tsec is None:
                    why.append("post-marker exit names koc %d, which is not a script table" % c[2])
                else:
                    n = len(arc_offsets(*LOC[TWELVE][tsec]))
                    tail_exit['twelve_entries'] = n
                    if not (0 <= c[3] < n):
                        why.append("post-marker exit jumps to Twelve's %s[%d], outside its %d entries" % (tsec, c[3], n))
                break
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
    words = k7_input_words()
    if preempt:
        why.append("cells that can be current at arming and carry a script-installing cancel bit: %d "
                   "(the cancel then still needs meoshi_hit_flag != 0 and waza_flag[i] != 0 at N+1; "
                   "the input-word leg is refuted, not unread -- every entry reads conpane%s = sw_chg, "
                   "which pl_lvr_set derives from the previous frame)" % (len(preempt), words['live']))
    return dict(rebirth=dict(table=reb['table'], script=reb['script'], routine=reb['rno'], marker=reb['marker'],
                             marker_cell=k, cells_before=len(preL), frames_before=sum(r['ctr'] for r in preL),
                             hit_ix_before=hix, canc_before=canc, marker_olc=marker_olc, tail_olc=tail,
                             tail_hit_ix=tail_hix, tail_exit=tail_exit, twelve_hiit=tw_hiit),
                frame_n=boxes, preempt_cells=preempt, input_words=words, unmodelled=(why or None))

_K7_FWD_CACHE = {}
def k7_forward_gate():
    """The FORWARD swap (`effk7.c` -> `K7_move_type_0` case 0), which §25.5 closed by a data sweep and
    §26.9 listed as still not modelled here.  Case 0 fires on a `cg_type 20` cell of TWELVE's own
    script -- the master is still Twelve, so that cell's `cg_olc` was decoded against Twelve's OVIX --
    and then rebinds every table to the TARGET's.  `eff01.c` -> `effect_01_move` restarts the overlay
    that same frame against the new `overlap_char_tbl`, so a marker (or a following cell, decoded
    against the target's OVIX once rebound) selecting a nonzero olc would carry a Twelve part index
    into the target's OVCT.  **Measured**: every live `cg_type 20` cell in Twelve's tables, and every
    cell between it and the next C command, selects `olc 0` -- so nothing is carried and the swap is
    closed for all 20 targets at once, with no per-target table comparison needed.  Anything nonzero
    is reported and keeps the gate OPEN."""
    if 'v' in _K7_FWD_CACHE: return _K7_FWD_CACHE['v']
    reb = parse_k7_rebirth()
    dead = k7_entry_walk(TWELVE)
    markers, why = [], []
    for sec, si, cgd, cells in _all_cells(TWELVE):
        for i, c in enumerate(cells):
            if c[0] != 'L' or c[1]['type'] != reb['fwd_marker']: continue
            tail = []
            for c2 in cells[i + 1:]:
                if c2[0] == 'C': break
                tail.append(c2[1]['olc'] >> 4)
            rec = dict(table=sec, script=si, cell=i, olc=(c[1]['olc'] >> 4), tail_olc=tail,
                       dead=(i in dead[(sec, si)]))
            markers.append(rec)
            if rec['dead']: continue
            if rec['olc']: why.append("Twelve's %s[%d] c%d marker selects olc %d" % (sec, si, i, rec['olc']))
            bad = [e for e in tail if e]
            if bad: why.append("cells after Twelve's %s[%d] c%d marker select olc %s" % (sec, si, i, bad))
    v = dict(marker=reb['fwd_marker'], markers=markers,
             live=len([m for m in markers if not m['dead']]),
             dead=len([m for m in markers if m['dead']]), unmodelled=(why or None))
    _K7_FWD_CACHE['v'] = v
    return v

_K7_DEAD_CACHE = {}
def k7_entry_walk(ci):
    """Which cells of each script can never execute.  §19's convention -- everything after the first
    terminating C command is dead -- is a LINEAR scan, and the format's jumps carry a cell index:
    charset.c -> comm_jmp/comm_jpss/comm_jsr all call `set_char_move_init2(wk, ctc->koc, ctc->ix,
    ctc->pat, ...)`, whose `cg_ix = (ip - 1) * cgd_type - cgd_type` makes `pat` a 1-based cell index.
    So a jump can land AFTER a terminator and revive the cells behind it (measured: Ibuki's
    `saca[27]` c33 and `saca[60..62]` c17, entered by a `comm_rja7`/`comm_jmp` past the terminator).
    This walks instead: entry points are cell 0 plus every landing any C cell in the character's own
    tables names, and each walk runs the SUCCESSOR GRAPH (`_k7_succ`) rather than a straight line --
    a script is not a line either, because six writers of `cg_ix` carry an index that can move the
    cursor backwards or forwards inside the same script (§27.1, restricted to same-frame edges).
    Fail-open everywhere -- a landing outside the script, a koc this model does not map, a command
    code past `decode_chcmd`, or a same-frame edge leaving the parsed cells, marks the whole script
    live.  `dead` therefore never rests on something the model declined to follow."""
    if ci in _K7_DEAD_CACHE: return _K7_DEAD_CACHE[ci]
    allc = list(_all_cells(ci))
    lens = {(sec, si): len(cells) for sec, si, _, cells in allc}
    entries, unknown = {k: {0} for k in lens}, set()
    for sec, si, cgd, cells in allc:
        for c in cells:
            if c[0] != 'C': continue
            _, code, koc, ix, pat = c
            key = (KOC2SEC.get(koc), ix)
            if key[0] is None or key not in lens: continue     # not a reference into this character
            if 0 <= pat - 1 < lens[key]: entries[key].add(pat - 1)
            else: unknown.add(key)                             # fail open: landing off the end
    out = {}
    for sec, si, cgd, cells in allc:
        key = (sec, si)
        if key in unknown: out[key] = set(); continue
        live, stack, opened = set(), sorted(entries[key]), False
        while stack:
            i = stack.pop()
            if not (0 <= i < len(cells)): opened = True; continue   # an edge left the parsed script
            if i in live: continue
            live.add(i)
            succ, unmod = _k7_succ(cells, i, cgd)
            opened = opened or unmod
            stack += succ
        out[key] = set() if opened else set(range(len(cells))) - live
    _K7_DEAD_CACHE[ci] = out
    return out

K7_OPND = {'koc': 2, 'ix': 3, 'pat': 4}     # arc_parse's C tuple is ('C', code, koc, ix, pat)

def _k7_succ(cells, i, cgd):
    """Cells of the SAME script the executor can be on next, given it is on cell `i`.  Six writers of
    `cg_ix` besides `+= cgd_type` stay inside the frame (`charset.c` unless noted), and each one can
    revive a cell a linear scan calls dead:

      comm_end (code 2)       `cg_ix = (pat - 2) * cgd_type`, then the dispatch loop's `+= cgd_type`
      comm_ixfw/ixbw (49/50)  `+= (pat - 1) * cgd_type` / `-= (pat + 1) * cgd_type`, then `+=`
      decord_if_jump          32 `decode_chcmd` slots (`parse_decord_slots`): 0x4000 relative forward,
                              0x8000 relative back, 0x2000 `decode_if_lever` (cross-script; the `wca`
                              leg is the edge added below), else `(w - 2) * cgd_type` absolute
      cg_wca_ix               `check_cgd_patdat`: `cg_type & 0x80` sets it, `char_move_wca` /
                              `decode_if_lever[13]` rewinds to `(cg_type & 0x7F) - 1`
      cg_extdat               `hitcheck.c` cases 0x1/0x41/0x81: `((cg_extdat & 0x3F) - 1) * cgd_type`
      cg_eftype               `pls03.c` -> `check_renda_cancel`: `cg_eftype * cgd_type - cgd_type * 2`,
                              guarded by `pls00.c` -> `check_cg_cancel_data`'s `cg_cancel & 16`

    The last three live in the cell word `cg_extdat|cg_cancel|cg_effect|cg_eftype`, which is word 3 of
    the cell and so is only copied for `cgd_type >= 4` (`setupCharTableData`: `cgd_type` u32s from
    `&wk->cg_type`); below that they hold the zero `set_char_move_init` wrote on entry.  For
    `cgd_type 1` the executor's grid (4 B) is finer than the one `arc_parse` decodes on (8 B), so no
    index in such a script is expressible here at all and the whole script returns `unmodelled`
    (23 scripts cast-wide, all `yuca`).
    Returns (same-script successors, unmodelled?) -- unmodelled marks the whole script live."""
    c = cells[i]
    if cgd == 1: return [i + 1], True        # every index in the script is off this grid
    if c[0] == 'L':
        r, out = c[1], [i + 1]
        if cgd >= 4:
            if r['type'] != 0xFF and (r['type'] & 0x80): out.append((r['type'] & 0x7F) - 1)
            if r['ext'] & 0x3F: out.append((r['ext'] & 0x3F) - 1)
            if r['canc'] & 0x10: out.append(r['eftype'] - 1)
        return out, False
    code = c[1]
    if code >= N_CHCMD: return [], True                  # past decode_chcmd: nothing to model
    if code in SPAN_TERMINAL: return [], False           # returns 0, no same-frame successor
    if code == 2: return [c[4] - 1], False               # comm_end
    if code == 49: return [i + c[4]], False              # comm_ixfw
    if code == 50: return [i - c[4]], False              # comm_ixbw
    if code in SPAN_TRIPLE_JUMP or code in SPAN_TRIPLE_STORE:   # cross-script; the entry sweep has it
        return ([i + 1] if (code in SPAN_TRIPLE_STORE or SPAN_TRIPLE_JUMP[code]) else []), False
    if code in SPAN_DECORD:
        out, fall, unmod = [], code in SPAN_DECORD_FALL, False
        for nm in SPAN_DECORD[code]:
            t, f2, bad = _decord(i, c[K7_OPND[nm]])
            out += t; fall = fall or f2; unmod = unmod or bool(bad)
        if fall: out.append(i + 1)
        return out, unmod
    return [i + 1], False                                # every other handler returns 1, cg_ix untouched

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
        # cells no entry point can walk to never execute (doc §19/§24.5:
        # decoder artefacts such as Yang's olc 1264); they are reported `dead`.
        # k7_entry_walk, not a linear terminator scan -- the format's jumps
        # carry a cell index and can land past a terminator (§26.10).
        dead = k7_entry_walk(ci)[(sec, si)]
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


# ---------------------------------------------------------------- over-declared spans: can the executor reach the slack? (doc §27)
#
# `read_char_table` decodes the LAST script of every table up to `location.size`,
# so an over-declared span carries decoded-but-unrelated ROM ("slack") after
# that script's real end. This models every writer of the cell index `cg_ix`
# (charset.c and the C sites that set it from data) as a graph over cell
# POSITIONS -- a frame is (table, script) with that script's cgd stride, and a
# node is a cell index k in that frame (k may be negative or run past the
# script's own cells: the C indexes `set_char_ad + cg_ix` with no bound) -- and
# takes the closure from every entry the C can form. Anything the model cannot
# follow is a reason in `unmodelled`, which keeps the gate OPEN.
#
# Writers (charset.c unless noted), in the order the closure applies them:
#   sequential      check_cm_extended_code `cg_ix += cgd_type`         k -> k+1
#   cg_next_ix      (cgd 6 cell byte) `(cg_next_ix - 1) * cgd_type`     k -> next_ix-1
#   cg_wca_ix       check_cgd_patdat `cg_type & 0x80` -> char_move_wca   k -> (type&0x7F)-1
#   cg_extdat       hitcheck.c `((cg_extdat & 0x3F) - 1) * cgd_type`    k -> (ext&0x3F)-1
#   renda           pls03.c check_renda_cancel (cell canc & 0x10)       k -> eftype-1
#   comm_end        `(ctc->pat - 2) * cgd_type` then += cgd_type          k -> pat-1
#   comm_ixfw/ixbw  `+= (pat-1)*cgd` / `-= (pat+1)*cgd`, then +=          k -> k±pat
#   decord_if_jump  0x4000 rel fwd, 0x8000 rel back, 0x2000 sub-command,
#                   else `(ix - 2) * cgd_type` absolute                  k -> k±(w&0xFF) | w-1
#   jmp/jpss/jsr/rapp*/rja*/rhsja   set_char_move_init2(koc, ix, pat)   -> (table, ix, pat-1)
#   C literals      appear.c / win_pl.c / plpat00.c set_char_move_init2  -> (yuca|saca, ix, ip-1)
#   plpcu.c         char_move_index(curr_rca->catch_nix)                 -> (cuca, *, nix-1)
#   exset_char_move_init (pls00.c, plpdm.c) keeps cg_ix across a switch  -> (target, k)
#   appear.c:816 / win_pl.c:557 carry the current index into nmca[0]/yuca
# Every other `decode_chcmd` handler returns 1 without touching cg_ix
# (grep `cg_ix *=` in charset.c: the sites above are the whole list).
# effk5.c's look-ahead (get_okuri_time) READS cells but only follows 2/49/50
# and stops at every terminator (k5_exc_check == 2), so its reach is a
# subset of this closure. Effects bind their own char_table (eff*.c
# `*ewk->wu.char_table = _..._char_table`); the two that call
# set_char_base_data and then set_char_move_init2 with a non-zero ip
# (eff13 charset 11, effc3 charset 17) bind char_init_data slots that
# copy_char_base_data() overwrites with effect tables, not a player's.

SPAN_TERMINAL = {1, 6, 17, 19, 21, 23, 25, 27, 29, 31, 69, 102, 115, 120}   # return 0, no same-frame successor
SPAN_TRIPLE_JUMP = {3: False, 4: False, 5: True, 54: True, 55: True, 85: True, 86: True}  # code -> falls through too
SPAN_TRIPLE_STORE = {16, 18, 20, 22, 24, 26, 28, 30, 119}                  # rja..rja7, rmja, rhsja
def parse_decord_slots():
    """`decode_chcmd` slots whose handler reaches `charset.c` -> `decord_if_jump`, with the operand fields
    it passes (in source order) and whether the handler can also `return 1` without jumping.  Derived from
    `charset.c` rather than listed by hand: the hand-written table this replaced named 25 slots and the
    source has **32** -- `comm_rngc` (44), `comm_mpcy` (88), `comm_epcy` (89), `comm_myhp` (96),
    `comm_emhp` (97), `comm_s_chg` (117) and `comm_schg2` (118) were missing, and missing a jump edge
    removes reach, which is the direction that fails toward "closed"."""
    s = src("src/sf33rd/Source/Game/engine/charset.c")
    names = [x.strip() for x in re.search(r's32 \(\*const decode_chcmd\[125\]\)\(\) = \{(.*?)\};', s, re.S)
             .group(1).replace('\n', ' ').split(',') if x.strip()]
    slot = {n: i for i, n in enumerate(names)}
    bodies, cur, buf = {}, None, []
    for ln in s.split("\n"):                     # top-level function bodies: a `name(...) {` in column 0
        m = re.match(r'^(?:static\s+)?[A-Za-z_][\w \*]*?([A-Za-z_]\w*)\(.*\)\s*\{\s*(?://.*)?$', ln)
        if m and not ln[:1].isspace():
            if cur: bodies[cur] = "\n".join(buf)
            cur, buf = m.group(1), []
        elif cur is not None:
            buf.append(ln)
    if cur: bodies[cur] = "\n".join(buf)
    opnd, fall = {}, set()
    for n, b in bodies.items():
        if n == 'decord_if_jump' or 'decord_if_jump(' not in b or n not in slot: continue
        seen = []
        for m in re.finditer(r'decord_if_jump\([^,]+, *ctc, *(?:ctc->)?(\w+)\)', b):
            if m.group(1) not in seen: seen.append(m.group(1))
        opnd[slot[n]] = tuple(seen)
        if re.search(r'\breturn 1;', b): fall.add(slot[n])
    assert len(opnd) == 32, len(opnd)
    return opnd, fall

SPAN_DECORD, SPAN_DECORD_FALL = parse_decord_slots()                       # FALL: returns 1 when not taken
SPAN_LEVER_FALL = {0, 11, 12}                                             # decode_if_lever: dummy, nex, nex2
N_IF_LEVER = 16
# set_char_move_init2 literal entries on a player's tables (koc 9 = yuca, 5 = saca), as (sec, ix, ip)
SPAN_C_ENTRIES = [('yuca', 0x17, 9), ('yuca', 12, 19), ('yuca', 0x3D, 4), ('yuca', 0x10, 3), ('yuca', 17, 2),
                  ('yuca', 17, 15), ('yuca', 0xb, 5), ('yuca', 0xC, 2), ('yuca', 0x11, 0x0A),   # appear.c
                  ('yuca', 36, 7),                                                             # win_pl.c
                  ('saca', 60, 8)]                                                             # plpat00.c
RICT_ELEM = 8   # CatchTable, structs.h: hos_x s16, hos_y s16, prio u8, flip u8, nix s16
RICT_NIX_OFF = 6

def parse_dm17_to_nm23():
    m = re.search(r'const s16 dm17_to_nm23_change\[20\] = \{([^}]*)\}', src("src/sf33rd/Source/Game/engine/plpdm.c"))
    v = [int(x) for x in m.group(1).replace('\n', ' ').split(',') if x.strip()]
    assert len(v) == 20
    return v

def arcade_id(c):
    """constants.h CHAR_3SX_TO_ARCADE: arcade ids skip 15 (Shin Akuma)."""
    return c + 1 if c > NAMES.index('AKUMA') else c

_FRAMES_CACHE = {}
def span_frames(ci):
    """(table, script) -> (cell-0 position, executor stride cgd_type*4 | None), plus the pointer tables."""
    if ci not in _FRAMES_CACHE:
        tabs = {sec: arc_offsets(*LOC[ci][sec]) for sec in KOC2SEC.values()}
        frames = {}
        for sec, ents in tabs.items():
            off, size = LOC[ci][sec]
            for si, base in enumerate(ents):
                cgd = struct.unpack_from('>h', ROM, off + base - 8)[0]
                frames[(sec, si)] = (base, cgd * 4 if cgd in (1, 2, 4, 6) else None)
        _FRAMES_CACHE[ci] = (tabs, frames)
    return _FRAMES_CACHE[ci]

def _span_cell(ci, sec, pos, st):
    """Decode the cell at span position `pos` in a frame of stride `st`, reading what the C reads: a command
    handler reads the 8-byte UNK11 {code, koc, ix, pat}; a sprite cell is copied cgd_type words."""
    off, size = LOC[ci][sec]
    if pos < 0 or pos + 2 > size: return None
    p = off + pos
    code = struct.unpack_from('>H', ROM, p)[0]
    if code < 0x100:
        if pos + 8 > size: return None
        koc, ix, pat = struct.unpack_from('>hhh', ROM, p + 2)
        return dict(C=True, code=code, koc=koc, ix=ix, pat=pat)
    if pos + st > size: return None
    r = dict(C=False, type=code & 0xFF, ctr=code >> 8, num=struct.unpack_from('>H', ROM, p + 6)[0])
    if st >= 16:
        r['ext'], r['canc'], r['eff'], r['eftype'] = ROM[p+12], ROM[p+13], ROM[p+14], ROM[p+15]
    if st == 24:
        r['rival'] = struct.unpack_from('>H', ROM, p + 18)[0]
        r['next_ix'] = ROM[p+20]
    return r

def _covered(iv):
    """Bytes covered by the union of half-open intervals."""
    n, end = 0, None
    for a, b in sorted(iv):
        if end is None or a > end: n += b - a; end = b
        elif b > end: n += b - end; end = b
    return n

def _decord(k, w):
    """decord_if_jump on operand word w at cell k: same-frame targets, fallthrough?, unmodelled reason."""
    u = w & 0xFFFF; hi = u & 0xE000
    if hi == 0x4000: return [k + (u & 0xFF)], False, None
    if hi == 0x8000: return [k - (u & 0xFF)], False, None
    if hi == 0x2000:
        sub = u & 0xFF
        if sub >= N_IF_LEVER: return [], False, "decode_if_lever[%d] out of range" % sub
        if sub in SPAN_LEVER_FALL: return [], True, None
        return [], False, None          # ret/uja*/umja/back/retmj/abbak: cross-script, seeds cover; wca: edge added at the cell that sets cg_wca_ix
    return [w - 1], False, None

def _carry_targets(ci, f, k):
    """Indices a switch that keeps the WORK's cell state (exset_char_move_init) can land on in the target
    script when the source is node (f, k): the same k, plus the source cell's cg_next_ix / cg_wca_ix, which
    exset does not clear (only set_char_move_init/2 do)."""
    base, st = span_frames(ci)[1][f]
    out = {k}
    c = _span_cell(ci, f[0], base + k * st, st) if st else None
    if c and not c['C']:
        if c.get('next_ix'): out.add(c['next_ix'] - 1)
        if c['type'] != 0xFF and c['type'] & 0x80: out.add((c['type'] & 0x7F) - 1)
    return out

_CSTART = {}
def c_start_tables():
    """Tables the C starts a player script in at cell 0: every literal koc passed to set_char_move_init,
    set_char_move_init2 or set_char_move_init_ca in the engine and animation sources (effects bind their
    own char_table). A table absent here (cbca) is entered only through script data -- jsr/jmp/rja
    triples -- which the closure discovers on reachable cells."""
    if 'v' not in _CSTART:
        kocs = set()
        for d in ("engine", "animation"):
            root = os.path.join(REPO, "src/sf33rd/Source/Game", d)
            for fn in sorted(os.listdir(root)):
                if not fn.endswith('.c'): continue
                t = re.sub(r'//[^\n]*', '', open(os.path.join(root, fn)).read())
                kocs |= set(int(m) for m in re.findall(r'set_char_move_init(?:2|_ca)?\(&?[\w>.-]+, (\d+),', t))
        _CSTART['v'] = set(KOC2SEC[k] for k in kocs if k in KOC2SEC)
        _CSTART['kocs'] = kocs
    return _CSTART['v']

def span_closure(ci, throw_seeds=(), donor_seeds=(), donor_triples=(), stale_only=None):
    """Closure of the cell index over character ci's ten script tables from every entry the C can form.
    `throw_seeds`: cuca entries the throw census produced (span_throw_seeds); `donor_seeds`/`donor_triples`:
    another character's positional registers and stored (koc, ix, pat) applied to THIS character's tables
    (the X.C.O.P.Y. morph, effk7.c K7_move_type_0: the tables are rebound, the WORK's registers are not).
    `stale_only=(setters, consumers)`: register-dataflow mode -- start from the C-side entries only, stop at
    every setter of the register class, and report the consumers reached (they can see a value the
    recipient's own data did not set). Returns nodes, reachable command cells by code, per-table facts and
    `unmodelled`."""
    tabs, frames = span_frames(ci)
    why = []
    for (sec, si), (base, st) in frames.items():
        if st is None: why.append("%s[%d] header cgd not in {1,2,4,6}" % (sec, si))
    def clamp_ip(ip): return max(ip, 1) - 1              # set_char_move_init2: `if (ip <= 0) ip = 1`
    def triple(koc, ix, pat):
        if not (0 <= koc <= 9): return None, "koc %d" % koc
        sec = KOC2SEC[koc]; ix = max(ix, 0)                # `if (index < 0) index = 0`
        if ix >= len(tabs[sec]): return None, "%s[%d] beyond the %d-entry pointer table" % (sec, ix, len(tabs[sec]))
        return ((sec, ix), clamp_ip(pat)), None
    seeds = set((f, 0) for f in frames if f[0] in c_start_tables())
    for sec, ix, ip in SPAN_C_ENTRIES:
        if ix < len(tabs[sec]): seeds.add(((sec, ix), clamp_ip(ip)))
    seeds |= set(throw_seeds)
    for (f, k) in donor_seeds:
        if f in frames: seeds.add((f, k))
    static_seeds = set(seeds)
    by_code = {}                                           # command code -> reachable cells (f, k) carrying it
    hit = set()                                            # stale_only: consumers reached
    for koc, ix, pat in donor_triples:
        t, bad = triple(koc, ix, pat)
        if t: seeds.add(t)
        else: why.append("X.C.O.P.Y. donor jump to %s" % bad)
    setters, consumers = stale_only if stale_only else ((), ())
    dm17 = parse_dm17_to_nm23()[ci]
    oos, edges_unmod = [], []
    def succ(f, k):
        base, st = frames[f]
        if st is None: return []
        sec = f[0]
        c = _span_cell(ci, sec, base + k * st, st)
        if c is None:
            oos.append((f, k, base + k * st)); return []
        out = []
        if not c['C']:
            out.append(k + 1)
            if c.get('next_ix'): out.append(c['next_ix'] - 1)
            if c['type'] != 0xFF and c['type'] & 0x80: out.append((c['type'] & 0x7F) - 1)
            if st >= 16:
                if c['ext'] & 0x3F: out.append((c['ext'] & 0x3F) - 1)
                if c['canc'] & 0x10: out.append(c['eftype'] - 1)
            return out
        code = c['code']
        if code >= N_CHCMD:
            edges_unmod.append("%s[%d] cell %d: command code %d >= decode_chcmd[%d]" % (sec, f[1], k, code, N_CHCMD)); return []
        by_code.setdefault(code, set()).add((f, k))
        if stale_only:
            if code in consumers: hit.add((f, k, code))
            if code in setters: return []                  # the register is (re)set here: stale flow stops
        if code in SPAN_TERMINAL: return []
        if code == 2: return [c['pat'] - 1]
        if code == 49: return [k + c['pat']]
        if code == 50: return [k - c['pat']]
        if code in SPAN_TRIPLE_JUMP or code in SPAN_TRIPLE_STORE:
            t, bad = triple(c['koc'], c['ix'], c['pat'])
            if t: seeds.add(t)
            else: edges_unmod.append("%s[%d] cell %d: jump to %s" % (sec, f[1], k, bad))
            return [k + 1] if (code in SPAN_TRIPLE_STORE or SPAN_TRIPLE_JUMP[code]) else []
        if code in SPAN_DECORD:
            fall = code in SPAN_DECORD_FALL
            for opnd in SPAN_DECORD[code]:
                t, f2, bad = _decord(k, c[opnd])
                out += t; fall = fall or f2
                if bad: edges_unmod.append("%s[%d] cell %d: %s" % (sec, f[1], k, bad))
                u = c[opnd] & 0xFFFF
                if stale_only and (u & 0xE000) == 0x2000 and ('lever', u & 0xFF) in consumers: hit.add((f, k, code))
            if fall: out.append(k + 1)
            return out
        return [k + 1]                                     # every other handler returns 1 and leaves cg_ix alone
    def closure(seeds):
        done = set()
        while True:
            work = [x for x in seeds if x not in done]
            if not work: break
            for x in work:
                stack = [x]
                while stack:
                    n = stack.pop()
                    if n in done: continue
                    done.add(n)
                    f, k = n
                    for k2 in succ(f, k):
                        if (f, k2) not in done: stack.append((f, k2))
            # jumps discovered on reachable cells add seeds (succ() mutates `seeds`); loop until none is new
        return done
    # Carried indices are ONE step each and their source script is fixed by the caller's state, so a carry
    # cannot feed itself: appear.c Appear_14000 copies its index out of yuca[0x3C] into nmca[0]; win_pl.c
    # copies nmca[0]'s index (+1) into yuca[33|35]; plpdm.c Damage_17000 keeps it across the switch into
    # dmca[dm17_to_nm23_change[ci]]; pls00.c keeps Elena's across nmca[36] -> nmca[0]. Out-of-span nodes are
    # already a hazard and are not carried.
    def in_span(f, k):
        base, st = frames[f]
        return st and 0 <= base + k * st and base + k * st + st <= LOC[ci][f[0]][1]
    c0 = closure(set(seeds))
    carried = set()
    if tabs['nmca']:
        if ('yuca', 0x3C) in frames:
            carried |= set((('nmca', 0), k) for (f, k) in c0 if f == ('yuca', 0x3C) and in_span(f, k))       # appear.c:816
        if ci == NAMES.index('ELENA') and ('nmca', 36) in frames:
            for (f, k) in c0:
                if f == ('nmca', 36) and in_span(f, k):
                    carried |= set((('nmca', 0), k2) for k2 in _carry_targets(ci, f, k))                   # pls00.c:1456
    if ('dmca', dm17) in frames:
        for (f, k) in c0:
            if f[0] == 'dmca' and in_span(f, k):
                carried |= set((('dmca', dm17), k2) for k2 in _carry_targets(ci, f, k))                    # plpdm.c:655
    c1 = closure(set(seeds) | carried)
    for ix in (33, 35):                                                                                      # win_pl.c:557 (work 1|3) + 32
        if ('yuca', ix) in frames:
            carried |= set((('yuca', ix), k + 1) for (f, k) in c1 if f == ('nmca', 0) and in_span(f, k))
    oos.clear(); edges_unmod.clear()
    reach = closure(set(seeds) | carried)
    out = {}
    for sec, ents in tabs.items():
        off, size = LOC[ci][sec]
        last = max(range(len(ents)), key=lambda i: ents[i])
        base, st = frames[(sec, last)]
        term_end = None
        if st:
            k = 0
            while base + k * st + 8 <= size:            # as arc_parse: a command needs its 8 read bytes, no more
                code = struct.unpack_from('>H', ROM, off + base + k * st)[0]
                if code < 0x100 and code in TERMINATORS: term_end = base + k * st + 8; break
                k += 1
        ends = [frames[f][0] + k * frames[f][1] + frames[f][1] for (f, k) in reach if f[0] == sec and frames[f][1]]
        reach_end = min(max(ends), size) if ends else 0
        past = sorted([(f[1], k, frames[f][0] + k * frames[f][1]) for (f, k) in reach
                       if f[0] == sec and frames[f][1] and term_end is not None
                       and frames[f][0] + k * frames[f][1] >= term_end])
        # read_char_table's own decode of the last script strides 8 + max(cgd*4-8, 0) for both cell kinds and
        # writes 8 bytes for a command, the stride for a sprite: does its final write straddle the malloc?
        p, over, dst = base, 0, max(st or 8, 8)
        while p < size:
            code = struct.unpack_from('>H', ROM, off + p)[0] if p + 2 <= size else 0x100
            w = 8 if code < 0x100 else dst
            over = max(over, p + w - size)
            p += dst
        out[sec] = dict(declared=size, scripts=len(ents), last=last, cgd=st // 4 if st else None,
                        term_end=term_end, slack=(size - term_end) if term_end is not None else None,
                        reach_end=reach_end, reach_past_term=past,
                        reach_past_term_bytes=(_covered([(min(pos, size), min(pos + frames[(sec, si)][1], size)) for si, k, pos in past])
                                               if term_end is not None else None),
                        decode_overrun=over)
    oos_sec = {}
    for f, k, pos in oos: oos_sec.setdefault(f[0], []).append((f[1], k, pos))
    for sec, lst in sorted(oos_sec.items()):
        why.append("%s: %d reachable cell(s) read outside the declared span, e.g. script %d cell %d at %+d" % ((sec, len(lst)) + sorted(lst)[0]))
    for sec, r in out.items():
        if r['term_end'] is None: why.append("%s: last script has no terminator inside the span" % sec)
    why += sorted(set(edges_unmod))
    return dict(nodes=reach, by_code=by_code, static_seeds=static_seeds, tables=out, oos=oos_sec,
                stale_hits=sorted(hit), unmodelled=(why or None))

def span_throw_census(ci, nodes):
    """(cg_rival values, cmyd.ix values) on character ci's REACHABLE cells -- what ci can do as a thrower.
    cmyd.ix is written only by comm_ydat (code 33), which no script of any character carries on either
    release (measured: 0 cells arcade, 0 cells PS2; comm_ngme/comm_ngem write cmyd.pat only), so it holds
    its initial 0 and the caught player always runs cuca[0]. A ydat cell that DID turn up is added."""
    tabs, frames = span_frames(ci)
    rivals, ydat = set(), {0}
    for (f, k) in nodes:
        base, st = frames[f]
        c = _span_cell(ci, f[0], base + k * st, st) if st else None
        if c is None: continue
        if c['C'] and c['code'] == 33: ydat.add(c['ix'])
        elif not c['C'] and c.get('rival'): rivals.add(c['rival'])
    return rivals, ydat

def span_throw_seeds(ci, census):
    """Cells the CAUGHT player ci can be started on by a throw: plpcu.c runs cuca[cmyd.ix] (the thrower's
    comm_ydat operand) and char_move_index(curr_rca->catch_nix), curr_rca being the thrower's RICT row
    `cg_rival + CHAR_3SX_TO_ARCADE(caught) - 24` (charset.c check_cgd_patdat, catch_table_offset). cmyd
    persists across the thrower's script switches, so every ydat of a thrower pairs with every rival group
    it selects. Returns (seeds, notes): notes are pairings the model cannot place."""
    seeds, notes = set(), []
    n_cuca = len(span_frames(ci)[0]['cuca'])
    for tj, (rivals, ydat) in census.items():
        off, size = LOC[tj]['rict']; n_rict = size // RICT_ELEM
        for g in rivals:
            row = g + arcade_id(ci) - 24
            if not (0 <= row < n_rict):
                notes.append("%s cg_rival %d row %d for caught %s outside its %d-row RICT" % (NAMES[tj], g, row, NAMES[ci], n_rict)); continue
            nix = struct.unpack_from('>h', ROM, off + row * RICT_ELEM + RICT_NIX_OFF)[0]
            for s_ in ydat:
                if 0 <= s_ < n_cuca: seeds.add((('cuca', s_), nix - 1))
                else: notes.append("%s ydat ix %d beyond caught %s's %d-entry cuca table" % (NAMES[tj], s_, NAMES[ci], n_cuca))
    return seeds, sorted(set(notes))

# Register classes the X.C.O.P.Y. morph can carry across tables (charset.c). For each: the commands that
# SET it in script data, the commands that CONSUME it, and what the donor's value is. A class whose setter
# is C code (cmb2: setup_comm_retmj in char_move_cmms3; cmb3: setup_comm_abbak in hitcheck.c; cmbk also
# via char_move_cmja/cmms/cmhs) can hold the donor's value at ANY of the donor's reachable cells.
SPAN_REG = {
    'cmbk': dict(set={3, 4, 17, 19, 21, 23, 25, 27, 29, 31, 120, 54, 55, 85, 86}, use={69, ('lever', 10)}, val='pos', c_side=True),
    'cmsw': dict(set={5}, use={6, ('lever', 1)}, val='pos', c_side=False),
    'cmms': dict(set={30}, use={31, ('lever', 9)}, val='triple', c_side=False),
    'cmhs': dict(set={119}, use={120}, val='triple', c_side=False),
    'cmlp': dict(set={12}, use={13, ('lever', 11)}, val='pos', c_side=False),
    'cml2': dict(set={14}, use={15, ('lever', 12)}, val='pos', c_side=False),
    'cmb2': dict(set=set(), use={102, ('lever', 14)}, val='word', c_side=True),
    'cmb3': dict(set=set(), use={115, ('lever', 15)}, val='pos', c_side=True),
}
for _n in range(1, 8):
    SPAN_REG['cmja%d' % _n] = dict(set={16 + 2 * (_n - 1)}, use={17 + 2 * (_n - 1), ('lever', _n + 1)}, val='triple', c_side=False)

def span_stale(ci, throw_seeds):
    """Per register class: the recipient's consumer cells reachable from a C-side entry without passing a
    setter of that class -- the only places a value carried in by the morph could be consumed."""
    out = {}
    for name, r in SPAN_REG.items():
        res = span_closure(ci, throw_seeds=throw_seeds, stale_only=(r['set'], r['use']))
        out[name] = res['stale_hits']
    return out

def span_donor(ci, res, recipient_stale):
    """What character ci's WORK carries into a recipient's tables across the morph, per register class,
    limited to the classes the recipient can consume stale. The stored-triple classes (cmja1..7, cmms,
    cmhs) are bounded: the (koc, ix, pat) operands of ci's reachable rja*/rmja/rhsja cells. The positional
    classes hold a CELL POSITION of ci's data (`cg_ix/cgd + 2`, or the raw cg_ix for cmb2) and are also
    written by C code at any cell (char_move_cmja/cmms/cmhs, char_move_cmms3, hitcheck.c), so their value
    set is every reachable cell of ci; applying that to the recipient's same-numbered scripts is sound but
    unbounded, and is returned as a reason instead of a seed. Returns (triples, reasons)."""
    tabs, frames = span_frames(ci)
    triples, why = set(), []
    def cell(f, k):
        base, st = frames[f]
        return _span_cell(ci, f[0], base + k * st, st) if st else None
    for name, r in SPAN_REG.items():
        hits = recipient_stale.get(name)
        if not hits: continue
        if r['val'] == 'triple':
            for code in r['set']:
                for (f, k) in res['by_code'].get(code, ()):
                    c = cell(f, k); triples.add((c['koc'], c['ix'], c['pat']))
        else:
            n = sum(len(res['by_code'].get(code, ())) for code in r['set'])
            why.append("%s: %d consumer(s) reachable without a setter (e.g. %s[%d] cell %d) can see %s's %s position%s"
                       % (name, len(hits), hits[0][0][0], hits[0][0][1], hits[0][1], NAMES[ci],
                          ("any" if r['c_side'] else str(n)), " (C-side setter)" if r['c_side'] else ""))
    return triples, why

def _donor_to_frames(cj, seeds):
    """Map a donor's positional seeds onto character cj's frames; a raw word offset that is not a whole cell
    of cj's script is returned as a note (comm_retmj would resume mid-cell)."""
    tabs, frames = span_frames(cj)
    out, notes = set(), []
    for (f, k) in seeds:
        if f not in frames or frames[f][1] is None: continue
        if isinstance(k, tuple):
            w = k[1]; cgd = frames[f][1] // 4
            if w % cgd: notes.append("donor word offset %d in %s[%d] is not a cell of cgd %d" % (w, f[0], f[1], cgd))
            else: out.add((f, w // cgd))
        else: out.add((f, k))
    return out, notes

_SPAN_ALL = None
def span_results():
    """All 20 characters, to a fixpoint over the two cross-character couplings: the throw census (a thrower's
    reachable cg_rival cells seed the caught player's cuca[0]) and the X.C.O.P.Y. morph (Twelve's WORK
    registers against every target's tables, and each target's against Twelve's, per register class and
    only where the recipient has a stale-reachable consumer). Returns per character the plain closure
    (`base`, throws only) and the morph closure (`xcopy`), each with `throw_notes`; `xcopy` also carries
    `stale` (the recipient's stale-reachable consumers) and `donor_notes`."""
    global _SPAN_ALL
    if _SPAN_ALL is not None: return _SPAN_ALL
    census = {ci: (set(), set()) for ci in range(20)}
    base = {ci: span_closure(ci) for ci in range(20)}
    tseeds = {ci: set() for ci in range(20)}
    for _ in range(8):
        new = {ci: span_throw_census(ci, base[ci]['nodes']) for ci in range(20)}
        if new == census: break
        census = new
        tseeds = {ci: span_throw_seeds(ci, census)[0] for ci in range(20)}
        base = {ci: span_closure(ci, throw_seeds=tseeds[ci]) for ci in range(20)}
    else:
        raise RuntimeError("throw census did not converge")
    stale = {ci: span_stale(ci, tseeds[ci]) for ci in range(20)}
    xc = dict(base); xnotes = {ci: [] for ci in range(20)}
    for _ in range(8):
        prev = {ci: len(xc[ci]['nodes']) for ci in range(20)}
        tw_trip, tw_why = set(), []
        for cj in range(20):
            if cj == TWELVE: continue
            t_, w_ = span_donor(cj, xc[cj], stale[TWELVE]); tw_trip |= t_; tw_why += ["from %s -- %s" % (NAMES[cj], x) for x in w_]
        xnotes[TWELVE] = tw_why
        xc[TWELVE] = span_closure(TWELVE, throw_seeds=tseeds[TWELVE], donor_triples=tw_trip)
        for cj in range(20):
            if cj == TWELVE: continue
            t_, w_ = span_donor(TWELVE, xc[TWELVE], stale[cj])
            xnotes[cj] = ["from TWELVE -- %s" % x for x in w_]
            xc[cj] = span_closure(cj, throw_seeds=tseeds[cj], donor_triples=t_)
        census2 = {ci: span_throw_census(ci, xc[ci]['nodes']) for ci in range(20)}
        if census2 != census:
            census = census2; tseeds = {ci: span_throw_seeds(ci, census)[0] for ci in range(20)}; continue
        if all(len(xc[ci]['nodes']) == prev[ci] for ci in range(20)): break
    else:
        raise RuntimeError("X.C.O.P.Y. closure did not converge")
    for ci in range(20):
        base[ci]['throw_notes'] = span_throw_seeds(ci, census)[1]
        xc[ci]['throw_notes'] = base[ci]['throw_notes']
        xc[ci]['donor_notes'] = sorted(set(xnotes[ci]))
        xc[ci]['stale'] = {k: v for k, v in stale[ci].items() if v}
    _SPAN_ALL = dict(base=base, xcopy=xc)
    return _SPAN_ALL

HIIT_ELEM = 16      # UNK_0, structs.h: boix bhix haix mf caix cuix atix hoix (8 x u16)
def caua_hosa_fit(ci):
    """CAUA is indexed only by HIIT.cuix (charset.c `wk->h_cau = wk->caught_adrs + wk->cg_ja.cuix`) and HOSA
    by HIIT.hoix (`wk->h_hos = wk->hosei_adrs + wk->cg_ja.hoix`) plus the literal hosei_adrs[1] (pls01.c,
    pls02.c, pls03.c). Every HIIT row is taken, reachable or not."""
    off, size = LOC[ci]['hiit']; n = size // HIIT_ELEM
    cu = [struct.unpack_from('>H', ROM, off + i * HIIT_ELEM + 10)[0] for i in range(n)]
    ho = [struct.unpack_from('>H', ROM, off + i * HIIT_ELEM + 14)[0] for i in range(n)]
    blob, bsd = ps2_tail(ci); offs, sp = ps2_spans(blob)
    out = {}
    for sec, idx in (('caua', cu), ('hosa', ho)):
        decl = LOC[ci][sec][1] // 8; ps2 = sp[SECTIONS.index(sec)][1] // 8
        mx = max(idx + ([1] if sec == 'hosa' else []))
        out[sec] = dict(declared=decl, ps2=ps2, max_index=mx, over_declared=max(0, decl - ps2),
                        tail_reached=(mx >= ps2), fits=(mx == ps2 - 1))
    return out

# ---------------------------------------------------------------- shape-mismatched scripts: is the CG remap confirmed? (doc §29)
_MANU_CACHE = {}
def manu_delta_gate(ci):
    """Adjudicate `cg_number` for every shape-mismatched script -- the `manu` column.

    A shape mismatch means `audit()` cannot pair arcade cell i with PS2 cell i, so it SKIPS the
    class-(c) wrong-sprite check for every cell of that script (`shape_ok` gates `ps2num`).  That
    is the whole content of the "316 shape-divergent scripts" item: not that anything was found,
    but that nothing was looked at.  This asks the same question without needing the pairing.

    What class (c) really tests is the ADAPTATION -- `remap()` must send an arcade raw cg_number
    to the index the PS2 tables use for that sprite.  `remap` is a pure function of the raw value
    (a piecewise range shift, `arcade_char_data.c`), so that is a per-RAW-VALUE property, not a
    per-cell one: a raw value appearing anywhere in a SHAPE-OK script of the same character is
    pinned by that script's PS2 counterpart, whichever script later asks for it.  Cells are
    therefore adjudicated by raw value, against an oracle built from the shape-ok scripts alone:

      direct            the raw is itself observed, and our delta equals the observed one
      bracketed         the raw is unobserved, but the nearest observed raw below AND above both
                        measure the SAME delta and ours equals it -- the value sits strictly
                        inside a band the oracle pins on both sides
      bracket_disagree  the two bracketing observations measure different deltas: the band is not
                        uniform across the gap, so nothing is confirmed
      unbracketed       no observation below, or none above
      divergent         an observation -- direct, or a bracketing agreement -- CONTRADICTS our
                        delta.  This is the class-(c) finding the shape mismatch was hiding.

    Fails toward divergence throughout, per the house rule that nothing unmodelled may land on
    "benign": `bracket_disagree` and `unbracketed` are NOT benign verdicts, and a raw observed
    with two different deltas is DROPPED from the oracle (`conflict`) rather than settled by a
    majority -- so an ambiguous raw can never confirm anything, only fail to.  Reachability comes
    from `k7_entry_walk` alone: §26.10.2 withdrew "past the first terminator" as a reachability
    test, so this gate does not use it, and adjudicates every cell the entry-point closure leaves
    live -- including post-terminator ones, which is the stricter choice.
    """
    if ci in _MANU_CACHE: return _MANU_CACHE[ci]
    arc = {sec: arc_offsets(*LOC[ci][sec]) for sec in KOC2SEC.values()}
    blob, bsd = ps2_tail(ci); offs, sp = ps2_spans(blob)
    pt = {}
    for sec in KOC2SEC.values():
        b, z = sp[SECTIONS.index(sec)]
        pt[sec] = (b, z, ps2_offsets(blob, b))
    parsed, obs, conflict = [], {}, set()
    for sec in KOC2SEC.values():                     # KOC2SEC is insertion-ordered: deterministic
        pn = len(pt[sec][2])
        for si in range(len(arc[sec])):
            a = arc_parse(ci, sec, si, arc)[1]
            p = ps2_parse(blob, pt[sec][0], pt[sec][1], pt[sec][2], si)[1] if si < pn else None
            ok = (p is not None and len(p) == len(a) and all(x[0] == y[0] for x, y in zip(a, p)))
            parsed.append((sec, si, a, p, ok))
            if not ok: continue
            for x, y in zip(a, p):
                if x[0] != 'L': continue
                r, d = x[1]['num'], y[1]['num'] - x[1]['num']
                if r in obs and obs[r] != d: conflict.add(r)   # two deltas for one raw: unusable
                obs[r] = d
    for r in conflict: obs.pop(r, None)
    keys = sorted(obs)
    dead_all = k7_entry_walk(ci)
    scripts, rows = {}, []
    cellcls = dict(direct=0, bracketed=0, bracket_disagree=0, unbracketed=0, divergent=0)
    for sec, si, a, p, ok in parsed:
        if p is None or ok or not a: continue        # exactly audit()'s `needs_manual_diff` set
        dead = dead_all[(sec, si)]
        vs = []
        for i, c in enumerate(a):
            if c[0] != 'L' or i in dead: continue
            raw = c[1]['num']; rm = remap(raw, ci); ours = rm - raw
            if raw in obs:
                want = obs[raw]; lo = hi = raw
                v = 'direct' if want == ours else 'divergent'
            else:
                k = bisect.bisect_left(keys, raw)
                lo = keys[k - 1] if k > 0 else None
                hi = keys[k] if k < len(keys) else None
                if lo is None or hi is None: want, v = None, 'unbracketed'
                elif obs[lo] != obs[hi]:     want, v = None, 'bracket_disagree'
                else:
                    want = obs[lo]
                    v = 'bracketed' if want == ours else 'divergent'
            vs.append(v); cellcls[v] += 1
            if v == 'divergent':
                rows.append(dict(cls='manu_cg_delta_divergent', table=sec, script=si, cell=i,
                                 raw=raw, remapped=rm, group=(OGT[rm] if rm < OGT_N else None),
                                 delta=ours, oracle_delta=want, oracle_remapped=raw + want,
                                 oracle_group=(OGT[raw + want] if 0 <= raw + want < OGT_N else None),
                                 witness_lo=lo, witness_hi=hi, dead=False))
        if not vs:                                                    k = 'no_live_cells'
        elif 'divergent' in vs:                                       k = 'divergent'
        elif 'unbracketed' in vs or 'bracket_disagree' in vs:         k = 'unresolved'
        elif 'bracketed' in vs:                                       k = 'bracketed'
        else:                                                         k = 'direct'
        scripts[(sec, si)] = k
    out = dict(scripts=scripts, rows=rows, cells=cellcls,
               oracle_raws=len(obs), oracle_conflicts=len(conflict),
               script_cls=collections.Counter(scripts.values()))
    _MANU_CACHE[ci] = out
    return out
# ---------------------------------------------------------------- decoder grid phase (doc §29)
#
# WHAT THIS SETTLES.  §21.6 recorded a class of "converter artifact" cells, identified by a u32
# relation between the two releases' bytes at the same offset, and used it to explain away most
# shape-mismatch findings and every phantom code.  The relation is real; the mechanism was not.
# The two releases' cell record is NOT a uniform byte-swap of each other -- it is a per-FIELD
# transform, and which permutation is "genuine" depends on which word of the record you are on
# (`arc_parse` / `ps2_parse` above are the ground truth for the layout, and `include/structs.h`
# for the field widths):
#
#   word 0  cg_type|cg_ctr (u16), cg_se (u16)      -> per-u16 byte swap        GEN[0] = (1,0,3,2)
#   word 1  cg_olc_ix (u16), cg_number (u16)       -> per-u16 byte swap, and cg_number is
#                                                     additionally remap_cg_number()d
#   word 2  arcade att,hit  vs  PS2 hit,att        -> the PAIR is exchanged AND each u16 swapped,
#                                                     i.e. all four bytes reversed  GEN[2] = (3,2,1,0)
#   word 3  cg_extdat|cg_cancel|cg_effect|cg_eftype (four u8) -> bytes unchanged  GEN[3] = (0,1,2,3)
#   word 4  cg_zoom (u16), cg_rival (u16)          -> per-u16 byte swap
#   word 5  cg_add_xy (u16), cg_next_ix|cg_status  -> per-u16 byte swap -- MEASURED, and unlike
#                                                     word 3 the u8 pair swaps too: the arcade
#                                                     carries next_ix in the low byte of a BE u16
#                                                     (Gill atca[15] c28, arcade `.. 00 DB`, PS2
#                                                     `.. DB 00`, next_ix 0xDB on both)
#
# So "arcade BE u32 == PS2 LE u32, bit-identical" -- the signature §21.6's re-derivation used -- is
# GEN[2].  It is the NORMAL relation for the att/hit word of every genuinely converted cell in the
# game, and it is an anomaly only when it turns up at a word that is not word 2.  That happens when
# the decoder's cell grid is not the grid the data is on: past a script's real end the bytes belong
# to whatever follows, whose record boundary and record length need not be the ones this script's
# 8-byte header declares.  A cell decoded there is a GRID PHANTOM -- every field the decoder reports
# for it is a field of some other word -- which is why such cells manufacture out-of-range sound
# codes, out-of-range effect indices and out-of-range `koc`s on both sides at once.
#
# HOW THE GRID IS ESTABLISHED.  For every script that exists in both releases with the SAME
# cgd_type, over the COMMON PREFIX of the two spans (both start at the 8-byte header, so they are
# aligned there whether or not the tails are), each 4-byte block gets the set of word-roles whose
# transform explains the observed arcade-vs-PS2 bytes.  The walk starts on the grid the header declares
# (period cgd, phase 0) and holds that grid until it is CONTRADICTED -- and even then only switches
# if some other (period, phase) explains at least GRID_MIN_RECORDS whole records from that point.
# A block no role explains is a content divergence, not a phase change: it leaves the grid alone.
#
# FAIL TOWARD "REAL FINDING".  `phantom` is the verdict that excuses a finding, so it is the one
# that has to be earned: a cell is `phantom` only when some word of it is positively assigned a role
# that is not its own.  No PS2 counterpart, a counterpart of a different length or cgd_type, a block
# the walk could not place -- all of those come back `unmodelled` or `no_oracle`, never `phantom`.
GRID_PERIODS = (2, 4, 6)
GRID_GEN = {0: (1,0,3,2), 1: (1,0,3,2), 2: (3,2,1,0), 3: (0,1,2,3), 4: (1,0,3,2), 5: (1,0,3,2)}
GRID_MIN_RECORDS = 4        # a phase switch must explain this many whole records.  Not a fitted
                            # constant (doc §29.3 sweeps it): the aligned/phantom/unmodelled counts
                            # are bit-identical for 4, 5 and 6, and the verdict on every violation
                            # row -- and the assertion below -- is identical for every value 2..12.

def _grid_perm(a, pm): return (a[pm[0]], a[pm[1]], a[pm[2]], a[pm[3]])

def _grid_roles(a, p, ci):
    """Word-roles whose genuine cross-release transform explains this 4-byte block."""
    out = set()
    for r, pm in GRID_GEN.items():
        if p == _grid_perm(a, pm): out.add(r)
    if 1 not in out:
        # word 1's low half is cg_number, which the port remaps; olc must still match verbatim.
        if ((a[0] << 8) | a[1]) == (p[0] | (p[1] << 8)) and remap((a[2] << 8) | a[3], ci) == (p[2] | (p[3] << 8)):
            out.add(1)
    return out

def _grid_script_bytes(ci):
    """(sec, si, cgd, arcade bytes, ps2 bytes) for every script whose two releases can be compared
    byte for byte: present on both sides with the same cgd_type.  The spans are the ones
    `arc_parse` / `ps2_parse` decode (entry offset - 8, i.e. the 8-byte header included); both start
    at the header, so they are aligned over their COMMON PREFIX even when the two declared lengths
    differ -- and they often do, because the last script of an over-declared table runs to
    `location.size` (doc §27).  Only the prefix is returned; a cell past it has no oracle."""
    arc_tabs = {sec: arc_offsets(*LOC[ci][sec]) for sec in KOC2SEC.values()}
    blob, bsd = ps2_tail(ci)
    offs, sp = ps2_spans(blob)
    for sec in KOC2SEC.values():
        b, z = sp[SECTIONS.index(sec)]
        pents = ps2_offsets(blob, b)
        aents = arc_tabs[sec]
        aoff, asize = LOC[ci][sec]
        aso, pso = sorted(set(aents)), sorted(set(pents))
        for si in range(min(len(aents), len(pents))):
            astart, pstart = aents[si] - 8, pents[si] - 8
            an = [o for o in aso if o > aents[si]]
            pn = [o for o in pso if o > pents[si]]
            aend = (an[0] - 8) if an else asize
            pend = (pn[0] - 8) if pn else z
            if astart < 0 or pstart < 0 or aend <= astart or pend <= pstart: continue
            if aoff + aend > len(ROM) or b + pend > len(blob): continue
            L = min(aend - astart, pend - pstart)
            if L <= 8: continue
            acgd = struct.unpack_from('>h', ROM, aoff + astart)[0]
            pcgd = struct.unpack_from('<h', blob, b + pstart)[0]
            if acgd != pcgd or acgd not in (1, 2, 4, 6): continue
            yield sec, si, acgd, ROM[aoff + astart:aoff + astart + L], blob[b + pstart:b + pstart + L]

_GRID_CACHE = {}
def grid_phase(ci):
    """Per script: which decoded cells sit on the data's own record grid and which do not.
    Returns {(sec, si): {'verdict': {cell: 'aligned'|'phantom'|'unmodelled'},
                         'switches': [[block, period, phase], ...],
                         'signature': [cells whose word 0 is bit-identical BE-vs-LE],
                         'prefix_cells': cells covered by the compared prefix}}.
    Scripts with no byte-comparable counterpart are absent from the map entirely (`no_oracle`);
    read a cell's verdict through `grid_cell_verdict`, which handles both that and `past_prefix`."""
    if ci in _GRID_CACHE: return _GRID_CACHE[ci]
    out = {}
    for sec, si, cgd, ab, pb in _grid_script_bytes(ci):
        nb = (len(ab) - 8) // 4
        S = [_grid_roles(tuple(ab[8+4*j:12+4*j]), tuple(pb[8+4*j:12+4*j]), ci) for j in range(nb)]
        role = [None] * nb
        P, f, j, switches = cgd, 0, 0, []
        while j < nb:
            if ((j + f) % P) in S[j]:
                role[j] = (j + f) % P; j += 1; continue
            best = None
            for P2 in GRID_PERIODS:
                for f2 in range(P2):
                    if (P2, f2) == (P, f): continue
                    k = j
                    while k < nb and ((k + f2) % P2) in S[k]: k += 1
                    if best is None or k > best[0]: best = (k, P2, f2)
            if best is not None and (best[0] - j) >= min(GRID_MIN_RECORDS * cgd, nb - j):
                _, P, f = best
                switches.append([j, P, f])
                role[j] = (j + f) % P; j += 1
            else:
                role[j] = None; j += 1          # a content divergence: the grid is unchanged
        verdict, sig = {}, []
        for k in range(nb // cgd):
            v = 'aligned'
            for w in range(cgd):
                j = cgd * k + w
                r = role[j]
                if r == w: continue
                if r is not None: v = 'phantom'; break
                # The walk declined to place this block.  If nothing explains it at all it is a plain
                # content divergence and says nothing about the grid; if some OTHER role explains it,
                # the declared grid is contradicted here and this cell cannot be called aligned --
                # but neither is it positively off-grid, so it is `unmodelled`, not `phantom`.
                if w in S[j] or not S[j]: continue
                v = 'unmodelled'; break
            verdict[k] = v
            o = 8 + 4 * cgd * k
            a32 = struct.unpack_from('>I', ab, o)[0]
            p32 = struct.unpack_from('<I', pb, o)[0]
            if a32 == p32 and a32 != (((p32 << 16) | (p32 >> 16)) & 0xFFFFFFFF): sig.append(k)
        out[(sec, si)] = dict(verdict=verdict, switches=switches, signature=sig, prefix_cells=nb // cgd)
    _GRID_CACHE[ci] = out
    return out

def grid_cell_verdict(g, cell):
    """`no_oracle`  the PS2 has no script at this index, or it is a different cgd_type;
       `past_prefix` the arcade span outruns the PS2 one and this cell is in the arcade-only tail --
                     the PS2 has no bytes there, so no byte test of any kind applies;
       `unmodelled`  in the compared prefix, but the walk could not place some word of the cell;
       `aligned` / `phantom` as in the header comment.  Only `phantom` excuses a finding."""
    if g is None: return 'no_oracle'
    if cell >= g['prefix_cells']: return 'past_prefix'
    return g['verdict'].get(cell, 'unmodelled')
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
        # Every OOB-index class is split live/dead: `se_oob` counts the violations on a cell some
        # entry point can reach, `se_oob_dead` the ones no entry point can (doc §26.10.2's
        # `k7_entry_walk`, which fails toward live for anything it cannot follow).  Dead rows are
        # NOT suppressed -- a data change that revives one has to be visible as a live row appearing,
        # which it cannot be if the row was never emitted.
        cls = dict(a_oob=0, b_gap=0, c_wrong_group=0, c_same_group=0, needs_manual=0,
                   extra_script=0, extra_cells=0,
                   se_oob=0, eff_oob=0, tama_oob=0, sasign_oob=0, code_oob=0, koc_oob=0, idx_oob=0,
                   se_oob_dead=0, eff_oob_dead=0, tama_oob_dead=0, sasign_oob_dead=0,
                   code_oob_dead=0, koc_oob_dead=0, idx_oob_dead=0,
                   oob_phantom=0, oob_not_phantom=0)
        for koc, sec in KOC2SEC.items():
            an, pn = len(arc_tabs[sec]), len(ps2_tabs[sec][2])
            for si in range(an):
                acgd, acells = arc_parse(ci, sec, si, arc_tabs)
                dead = k7_entry_walk(ci)[(sec, si)]   # cells no entry point reaches (cached per character)
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
                gridsc = grid_phase(ci).get((sec, si))
                shape_ok = (pcells is not None and len(pcells) == len(acells)
                            and all(a[0] == p[0] for a, p in zip(acells, pcells)))
                if pcells is not None and not shape_ok and acells:
                    cls['needs_manual'] += 1
                    # doc §29: a C-vs-L shape mismatch is explained when the decoder is off the data's
                    # own record grid somewhere in the script; `no_oracle` means the two releases'
                    # spans are not byte-comparable at all, so nothing explains it either way.
                    rec['violations'].append(dict(cls='needs_manual_diff', table=sec, script=si,
                                                  arc_cells=len(acells),
                                                  ps2_cells=(len(pcells) if pcells is not None else None),
                                                  sa=salab.get(si) if sec == 'saca' else None,
                                                  grid=('no_oracle' if gridsc is None else
                                                        'phantom' if 'phantom' in gridsc['verdict'].values()
                                                        else 'aligned' if len(acells) <= gridsc['prefix_cells']
                                                        else 'aligned_prefix'),
                                                  grid_prefix_cells=(None if gridsc is None else gridsc['prefix_cells'])))
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
                    def viol(counter, kind, **kw):
                        """Record one OOB-index violation on this cell and count it under the live or the
                        dead half of `counter` -- never suppress the dead one (see `cls` above).
                        `grid` is the independent second axis (doc §29): `phantom` says the cell is
                        decoded off the data's own record grid, so the out-of-range index is a field of
                        some other word and not a value anything authored.  Anything the grid model
                        cannot place is NOT `phantom`, so it stays a finding to adjudicate."""
                        d = cidx in dead
                        g = grid_cell_verdict(gridsc, cidx)
                        cls['oob_phantom' if g == 'phantom' else 'oob_not_phantom'] += 1
                        cls[counter + '_dead' if d else counter] += 1
                        rec['violations'].append(dict(cls=kind, table=sec, script=si, cell=cidx, **kw,
                                                      dead=d, grid=g))
                    if c[0] == 'C':
                        code, kc, ix, pat = c[1], c[2], c[3], c[4]
                        pre = (pcell is not None and pcell[0] == 'C'
                               and pcell[1] == code and pcell[2] == kc and pcell[3] == ix)
                        if pre: continue
                        if code >= N_CHCMD:
                            viol('code_oob', 'a_code_oob', code=code)
                        if code in (3, 4, 5):   # jmp/jpss/jsr
                            if kc < 0 or kc >= 12:
                                viol('koc_oob', 'a_koc_oob', koc=kc, ix=ix)
                            elif kc in KOC2SEC:
                                nn = len(arc_tabs[KOC2SEC[kc]])
                                if ix < 0 or ix >= nn:
                                    viol('idx_oob', 'a_script_idx_oob', dest=KOC2SEC[kc], ix=ix, dest_entries=nn)
                            else:
                                viol('koc_oob', 'a_koc_unset', koc=kc)
                        if code == 43:          # comm_exec
                            if kc < 0 or kc >= N_EFFINIT:
                                viol('eff_oob', 'a_effinit_oob', eff=kc, data=ix)
                            elif kc == 2 and ix >= N_TAMA:
                                viol('tama_oob', 'a_tama_oob', tama=ix)
                            elif kc == 13 and ix >= N_SASIGN:
                                viol('sasign_oob', 'a_sasign_oob', idx=ix)
                        continue
                    r = c[1]; cells_seen += 1
                    pr = pcell[1] if (pcell is not None and pcell[0] == 'L') else None
                    se = r['se'] >> 4
                    # cg_se >>= 4 then bit 0x800 selects the per-character random-SE
                    # table (charset.c:2721-2727); only the non-random path indexes
                    # sound_effect_request[] directly.
                    if (se & 0x800) == 0 and se >= N_SE and not (pr and pr['se'] == r['se']):
                        viol('se_oob', 'a_se_oob', se=se)
                    ef, eft = r.get('eff', 0), r.get('eftype', 0)
                    if ef and not (pr and pr.get('eff') == ef and pr.get('eftype') == eft):
                        if ef >= N_EFFINIT:
                            viol('eff_oob', 'a_effinit_oob', eff=ef, data=eft)
                        elif ef == 2 and eft >= N_TAMA:
                            viol('tama_oob', 'a_tama_oob', tama=eft)
                        elif ef == 13 and eft >= N_SASIGN:
                            viol('sasign_oob', 'a_sasign_oob', idx=eft)
                    raw = r['num']; rm = remap(raw, ci)
                    grp = OGT[rm] if rm < OGT_N else None
                    ps2num = pcells[cidx][1]['num'] if (shape_ok and pcells[cidx][0] == 'L') else None
                    v = dict(table=sec, script=si, cell=cidx, raw=raw, remapped=rm, group=grp,
                             ps2=ps2num, ps2_group=(OGT[ps2num] if (ps2num is not None and ps2num < OGT_N) else None),
                             confidence=('high' if shape_ok else 'low-shape-differs'),
                             sa=salab.get(si) if sec == 'saca' else None, dead=(cidx in dead),
                             grid=grid_cell_verdict(gridsc, cidx))
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
        # doc §29: re-ask the class-(c) question for the shape-mismatched (`manu`) scripts, which
        # the cell-index diff above skipped entirely.  Rows are emitted in the gate's own
        # (section, script, cell) order, which is deterministic, so the JSON stays byte-stable.
        mg = manu_delta_gate(ci)
        rec['violations'].extend(mg['rows'])
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
        # the forward swap (case 0) is a property of Twelve's own scripts, so it is
        # computed once and recorded on his row; every row carries the verdict.
        fwd = k7_forward_gate()
        if ci == TWELVE: rec['xcopy_case0'] = fwd
        # Over-declared spans (doc §27): the executor's reach over every script table, in the plain
        # closure (`base`: own data + throws) and with the X.C.O.P.Y. register carry-over (`xcopy`).
        # `real_end` is the last byte any reachable cell touches or the first terminator's read-end,
        # whichever is later; `junk` is what read_char_table decodes and ComputeDigest hashes past it.
        sr = span_results(); sb, sx = sr['base'][ci], sr['xcopy'][ci]
        spans = {}
        for sec, t in sb['tables'].items():
            # Real end = the last byte any reachable cell touches, or the first terminator's read-end
            # (§19.7's metric), whichever is later. Cells reachable past the first terminator are script
            # data the census (arc_parse stops there) never classified: bound them here like class (a)/(b).
            term = t['term_end'] or 0
            real_end = max(term, t['reach_end'])
            past = []
            for si, k, pos in t['reach_past_term']:
                st = span_frames(ci)[1][(sec, si)][1]
                c = _span_cell(ci, sec, pos, st)
                d = dict(script=si, cell=k, pos=pos)
                if c is None: d['cls'] = 'oos'
                elif c['C']: d.update(code=c['code'], koc=c['koc'], ix=c['ix'], pat=c['pat'])
                else:
                    rm = remap(c['num'], ci); grp = OGT[rm] if rm < OGT_N else None
                    d.update(type=c['type'], ctr=c['ctr'], raw=c['num'], remapped=rm, group=grp,
                             cls=('a_ogt_oob' if rm >= OGT_N else 'b_group_gap' if (grp == 0 and rm != 0) else 'ok'))
                past.append(d)
            spans[sec] = dict(declared=t['declared'], scripts=t['scripts'], last_script=t['last'], cgd=t['cgd'],
                              term_end=t['term_end'], slack_by_terminator=t['slack'],
                              reach_end=t['reach_end'], real_end=real_end, junk=t['declared'] - real_end,
                              past_terminator_cells=past,
                              past_terminator_bytes=t['reach_past_term_bytes'],
                              past_terminator_bytes_xcopy=sx['tables'][sec]['reach_past_term_bytes'],
                              decode_overrun=t['decode_overrun'])
        # Decoder grid phase (doc §29). `signature` is the u32 byte relation §21.6's re-derivation
        # used -- arcade BE == PS2 LE at the same offset, on a cell's word 0.  It is GEN[2], the
        # att/hit word's normal relation, so a hit means the decoder is reading word 2 as word 0.
        # `signature_not_phantom` is the assertion: if the byte signature ever fires on a cell the
        # grid walk calls aligned, one of the two models is wrong and the run says so.
        gp = grid_phase(ci)
        gstats = dict(scripts=len(gp), switch_scripts=0, aligned=0, phantom=0, unmodelled=0,
                      signature=0, signature_not_phantom=0, signature_unmodelled=0)
        grid_detail = {}
        for (sec2, si2), g in sorted(gp.items()):
            if g['switches']: gstats['switch_scripts'] += 1
            for v2 in g['verdict'].values(): gstats[v2] += 1
            gstats['signature'] += len(g['signature'])
            gstats['signature_not_phantom'] += len([k2 for k2 in g['signature']
                                                    if g['verdict'].get(k2) not in ('phantom', 'unmodelled')])
            gstats['signature_unmodelled'] += len([k2 for k2 in g['signature'] if g['verdict'].get(k2) == 'unmodelled'])
            if g['switches'] or g['signature']:
                grid_detail["%s[%d]" % (sec2, si2)] = dict(
                    switches=g['switches'], signature=g['signature'],
                    phantom_cells=len([k2 for k2, v2 in g['verdict'].items() if v2 == 'phantom']))
        rec['grid_phase'] = dict(off_grid_scripts=grid_detail, **gstats)
        chf = caua_hosa_fit(ci)
        # Reason lists come out of span_closure() as sets, whose iteration order varies
        # per process (PYTHONHASHSEED). Order carries no meaning here -- these are
        # human-readable "why the gate stayed open" strings -- but an unsorted set made
        # cg_audit.json non-byte-reproducible, which silently broke the "the JSON
        # regenerates identically" check this file is verified with. Sort at emission.
        _rs = lambda v: (sorted(v) if v else v)
        rec['span_reach'] = dict(tables=spans, reachable_cells=len(sb['nodes']), reachable_cells_xcopy=len(sx['nodes']),
                                 unmodelled=_rs(sb['unmodelled']), throw_notes=_rs(sb['throw_notes']),
                                 xcopy=dict(unmodelled=_rs(sx['unmodelled']), donor_notes=_rs(sx['donor_notes']),
                                            stale_consumers={k: len(v) for k, v in sx['stale'].items()}),
                                 caua_hosa=chf)
        span_why = list(sb['unmodelled'] or []) + list(sb['throw_notes'] or [])
        span_why_x = list(sx['unmodelled'] or []) + list(sx['throw_notes'] or []) + list(sx['donor_notes'] or [])
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
                            k7_fwd_gate=('closed' if fwd['unmodelled'] is None else 'unmodelled'),
                            k7_foreign_oob=len([f for f in foreign if not f['dead'] and f['twelve']['oob']]),
                            k7_foreign_oob_ps2=len([f for f in foreign if not f['dead'] and f['ps2_same_cell'] and f['ps2_twelve']['oob']]),
                            k7_foreign_ps2_differs=len([f for f in foreign if not f['dead'] and not f['ps2_same_cell']]),
                            span_over_declared=len([t for t in spans.values() if t['slack_by_terminator']]),
                            span_slack_bytes=sum(t['slack_by_terminator'] or 0 for t in spans.values()),
                            span_junk_bytes=sum(t['junk'] for t in spans.values()),
                            span_past_terminator_cells=sum(len(t['past_terminator_cells']) for t in spans.values()),
                            span_past_terminator_bytes=sum(t['past_terminator_bytes'] or 0 for t in spans.values()),
                            span_past_terminator_bad=sum(1 for t in spans.values() for d in t['past_terminator_cells'] if d.get('cls') not in (None, 'ok')),
                            span_past_terminator_bytes_xcopy=sum(t['past_terminator_bytes_xcopy'] or 0 for t in spans.values()),
                            span_decode_overrun=max(t['decode_overrun'] for t in spans.values()),
                            span_gate=('closed' if not span_why else 'unmodelled'),
                            span_gate_reasons=len(span_why),
                            span_gate_xcopy=('closed' if not span_why_x else 'unmodelled'),
                            span_gate_xcopy_reasons=len(span_why_x),
                            grid_scripts=gstats['scripts'], grid_switch_scripts=gstats['switch_scripts'],
                            grid_aligned=gstats['aligned'], grid_phantom=gstats['phantom'],
                            grid_unmodelled=gstats['unmodelled'],
                            grid_signature=gstats['signature'],
                            grid_signature_not_phantom=gstats['signature_not_phantom'],
                            grid_signature_unmodelled=gstats['signature_unmodelled'],
                            caua_hosa_over_declared=chf['caua']['over_declared'] + chf['hosa']['over_declared'],
                            caua_hosa_tail_reached=int(chf['caua']['tail_reached'] or chf['hosa']['tail_reached']),
                            # doc §29: the `manu` scripts, adjudicated per raw cg_number instead of
                            # per cell index.  `manu_divergent` counts scripts carrying at least one
                            # cell whose remap delta the oracle contradicts -- the class-(c) finding
                            # the shape mismatch was hiding.  `manu_unresolved` is the honest
                            # residue: no confirmation either way, never read as benign.
                            manu_direct=mg['script_cls'].get('direct', 0),
                            manu_bracketed=mg['script_cls'].get('bracketed', 0),
                            manu_no_live_cells=mg['script_cls'].get('no_live_cells', 0),
                            manu_unresolved=mg['script_cls'].get('unresolved', 0),
                            manu_divergent=mg['script_cls'].get('divergent', 0),
                            manu_oracle_raws=mg['oracle_raws'],
                            manu_oracle_conflicts=mg['oracle_conflicts'],
                            **{'manu_cells_' + k: v for k, v in sorted(mg['cells'].items())},
                            **cls)
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
    # The seven OOB-index columns read live+dead: `0+31` is 31 violations, none of them on a cell
    # any entry point can reach (doc §21.6, §26.10.2).  The two halves sum to the single number these
    # columns used to carry, so a row that used to read `31` reads `0+31` and nothing was dropped.
    hdr = ("%-7s %5s | %4s %4s %5s %5s %5s %5s | %6s %6s %6s %6s %6s %6s %6s | %s"
           % ("char","cells","(a)","(b)","(c)wg","(c)og","manu","extra",
              "se l+d","eff","tama","sasi","code","koc","sidx","ovct a/p reach  ovix a/p  xcopy  slack  manu"))
    print(hdr); print("-"*len(hdr))
    T = {}
    OOB_COLS = ('se_oob', 'eff_oob', 'tama_oob', 'sasign_oob', 'code_oob', 'koc_oob', 'idx_oob')
    def oob_cols(s):
        # live+dead per class. Dead means no entry point reaches the cell (k7_entry_walk); the row is
        # still emitted and still counted, so a data change that revives one shows up as live.
        return " ".join("%6s" % ("%d+%d" % (s[k], s[k + '_dead'])) for k in OOB_COLS)
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
    def slack_flag(s):
        # doc §27: an over-declared span is a hazard only if a reachable cell lies past the real script
        # data. `dead(n,B)`: n over-declared tables, B junk bytes, nothing reaches them; `+xc` says the
        # X.C.O.P.Y. closure agrees. `unmodelled(k)`: k reasons keep the gate open (listed in the JSON).
        if s['span_past_terminator_bad']:
            return "slack:REACHED-OOB(%d)!" % s['span_past_terminator_bad']
        tag = "" if s['span_over_declared'] == 0 else "dead(%d,%dB)" % (s['span_over_declared'], s['span_junk_bytes'])
        if s['span_past_terminator_cells']: tag += "+past(%d,in-bounds)" % s['span_past_terminator_cells']
        g = ("closed" if s['span_gate'] == 'closed' else "unmodelled(%d)" % s['span_gate_reasons'])
        gx = ("closed" if s['span_gate_xcopy'] == 'closed' else "unmodelled(%d)" % s['span_gate_xcopy_reasons'])
        return "slack:%s%s%s+xc:%s" % ("none " if not tag else tag + " ", g, "", gx)
    def manu_flag(s):
        # doc §29: the `manu` scripts adjudicated per raw cg_number.  `ok(d+b)` means every live
        # cell's remap delta is confirmed -- d directly observed, b bracketed by agreeing
        # neighbours.  `?n` is the unconfirmed residue (never benign); `DIVERGENT` is a contradiction.
        if not s['needs_manual']: return "manu:none"
        t = "manu:%d/%d" % (s['manu_direct'] + s['manu_bracketed'] + s['manu_no_live_cells'], s['needs_manual'])
        t += "(d%d+b%d+z%d)" % (s['manu_direct'], s['manu_bracketed'], s['manu_no_live_cells'])
        if s['manu_unresolved']: t += " ?%d" % s['manu_unresolved']
        if s['manu_divergent']:
            t += " DIVERGENT(%d scripts,%d cells)!" % (s['manu_divergent'], s['manu_cells_divergent'])
        return t
    for n in NAMES:
        r = res[n]; s = r['stats']
        for k, v in s.items(): T[k] = T.get(k, 0) + (v if isinstance(v, int) else 0)
        print("%-7s %5d | %4d %4d %5d %5d %5d %5d | %s | %d/%d r<=%d %s  %d/%d %s  %s  %s  %s"
              % ((n, s['cells'], s['a_oob'], s['b_gap'], s['c_wrong_group'], s['c_same_group'], s['needs_manual'],
                  s['extra_script'], oob_cols(s))
                 + (s['ovct_arcade'], s['ovct_ps2'], s['ovct_reach_max'], ovct_flag(s),
                    s['ovix_arcade'], s['ovix_ps2'], "short" if s['ovix_arcade_shorter_by'] else "ok",
                    xcopy_flag(s), slack_flag(s), manu_flag(s))))
    print("-"*len(hdr))
    print("TOTAL         | %4d %4d %5d %5d %5d %5d | %s"
          % (T['a_oob'], T['b_gap'], T['c_wrong_group'], T['c_same_group'], T['needs_manual'], T['extra_script'],
             oob_cols(T)))
    print("cells audited:", T['cells'])
    # doc §29: the 316 shape-mismatched scripts, adjudicated per raw cg_number.  The five script
    # classes sum to `manu`; the cell classes sum to every live L-cell in those scripts.
    print("shape-mismatched (manu) scripts: %d = %d direct + %d bracketed + %d no-live-cells + %d unresolved + %d DIVERGENT"
          % (T['needs_manual'], T['manu_direct'], T['manu_bracketed'], T['manu_no_live_cells'],
             T['manu_unresolved'], T['manu_divergent']))
    print("  their live L-cells: %d = %d direct + %d bracketed + %d bracket-disagree + %d unbracketed + %d DIVERGENT"
          % (sum(T['manu_cells_' + k] for k in ('direct', 'bracketed', 'bracket_disagree', 'unbracketed', 'divergent')),
             T['manu_cells_direct'], T['manu_cells_bracketed'], T['manu_cells_bracket_disagree'],
             T['manu_cells_unbracketed'], T['manu_cells_divergent']))
    for n in NAMES:
        d = [v for v in res[n]['violations'] if v['cls'] == 'manu_cg_delta_divergent']
        for (sec, si) in sorted(set((v['table'], v['script']) for v in d)):
            g = [v for v in d if v['table'] == sec and v['script'] == si]
            print("  %s %s[%d]: %d cell(s) where the oracle contradicts the remap -- raw 0x%04X..0x%04X, "
                  "ours delta %+d -> group %s, oracle %+d -> group %s"
                  % (n, sec, si, len(g), min(v['raw'] for v in g), max(v['raw'] for v in g),
                     g[0]['delta'], g[0]['group'], g[0]['oracle_delta'], g[0]['oracle_group']))
    # doc §29: the decoder grid, and what it does to the OOB findings. The "converter artifact"
    # class §21.6 named does not exist -- the u32 byte relation it was identified by is the att/hit
    # word's own relation, so every hit is a cell decoded off the data's record grid.
    print("grid phase: %d byte-comparable scripts, %d with a phase switch; cells %d aligned / %d phantom / %d unmodelled; byte signature (arcade BE u32 == PS2 LE u32 at a cell's word 0): %d, of which %d phantom, %d unmodelled, %d aligned"
          % (T['grid_scripts'], T['grid_switch_scripts'], T['grid_aligned'], T['grid_phantom'],
             T['grid_unmodelled'], T['grid_signature'],
             T['grid_signature'] - T['grid_signature_unmodelled'] - T['grid_signature_not_phantom'],
             T['grid_signature_unmodelled'], T['grid_signature_not_phantom']))
    if T['grid_signature_not_phantom']:
        print("FATAL: the byte signature fired on a cell the grid walk calls aligned -- the two models disagree (doc §29)")
        sys.exit(1)
    print("OOB-index rows: %d explained by the grid (phantom), %d not -- every one of those must stand on its own (doc §29.5)"
          % (T['oob_phantom'], T['oob_not_phantom']))
    nmg = {}
    for n in NAMES:
        for v in res[n]['violations']:
            if v['cls'] == 'needs_manual_diff': nmg[v['grid']] = nmg.get(v['grid'], 0) + 1
    print("shape-mismatched scripts by grid: %s" % ", ".join("%s %d" % (k, nmg[k]) for k in sorted(nmg)))
    for n in NAMES:
        for v in res[n]['violations']:
            if v.get('grid') != 'phantom' and v['cls'].startswith('a_'):
                print("  %-7s %-5s %4d c%-3d %-16s grid=%-9s dead=%s"
                      % (n, v['table'], v['script'], v['cell'], v['cls'], v['grid'], v['dead']))
    # doc §27: the over-declared spans, and what the digest hashes past the real data
    digest_in = sum(LOC[ci][sec][1] for ci in range(20) for sec in SECTIONS)
    junk = T['span_junk_bytes'] + sum(res[n]['stats']['caua_hosa_over_declared'] * 8 for n in NAMES)
    print("over-declared script spans: %d (slack by first terminator %d B; junk past the real end %d B; cells reachable past a first terminator: %d base / %d B xcopy, %d out of bounds)"
          % (T['span_over_declared'], T['span_slack_bytes'], T['span_junk_bytes'], T['span_past_terminator_cells'], T['span_past_terminator_bytes_xcopy'], T['span_past_terminator_bad']))
    print("CAUA/HOSA over-declared elements: %d (tail reached: %d); read_char_table decode overrun: %d B max"
          % (sum(res[n]['stats']['caua_hosa_over_declared'] for n in NAMES), T['caua_hosa_tail_reached'], max(res[n]['stats']['span_decode_overrun'] for n in NAMES)))
    print("digest input: %d B over 500 spans, of which %d B (%.2f%%) is decoded ROM past the real data"
          % (digest_in, junk, 100.0 * junk / digest_in))
    print("slack gate: base closed %d/20, xcopy closed %d/20"
          % (len([n for n in NAMES if res[n]['stats']['span_gate'] == 'closed']), len([n for n in NAMES if res[n]['stats']['span_gate_xcopy'] == 'closed'])))
    for n in NAMES:
        for sec, t in res[n]['span_reach']['tables'].items():
            if t['past_terminator_cells']:
                print("  %s %s: %d reachable cell(s) past the first terminator (script %d cell %d ..), real end %d of %d, junk %d B, classes %s"
                      % (n, sec, len(t['past_terminator_cells']), t['past_terminator_cells'][0]['script'], t['past_terminator_cells'][0]['cell'],
                         t['real_end'], t['declared'], t['junk'], sorted(set(d.get('cls', 'C') for d in t['past_terminator_cells']))))
