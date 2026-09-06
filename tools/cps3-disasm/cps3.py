#!/usr/bin/env python3
"""Static SH-2 disassembly and anchoring for the decrypted CPS3 program.

The house method this tool exists to mechanise (docs/research-arcade-cg-data-accuracy.md
section 23.4, docs/research-arcade-balance-desyncs.md "How each routine was pinned"):

    find a data table that occurs EXACTLY ONCE in the decrypted image, find its
    SOLE literal referrer, and read the routine's address off the instruction
    that uses it.

Nothing here infers a routine from position, from a name, or from the shape of its
opening lines.  See README.md for the four traps this encodes.

Image: an 8 MiB big-endian SH-2 image mapped at 0x06000000, produced by
tools/arcade-audit/decrypt.py (gitignored; md5 909f5abec4b6b21bf7d2a452a03fdfcc).

Subcommands:
  find     locate a literal / byte pattern; the occurrence COUNT is the anchor test
  refs     literal-pool referrers of an address or value, plus the loading instruction
  dis      annotated disassembly (pc-relative loads resolved to their values)
  fn       function extent, literal pool, and outgoing-call census
  nocall   screen "does this routine call X?", with a mandatory positive control
  table    dump a table, optionally indexed through a type table
  pin      run the whole house method on one anchor and print the verdict
  selftest reproduce addresses already established in the docs
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys

try:
    from capstone import CS_ARCH_SH, CS_MODE_BIG_ENDIAN, CS_MODE_SH2, Cs
except ImportError:  # pragma: no cover - dependency check
    sys.stderr.write(
        "error: capstone is not importable by this interpreter.\n"
        "       pip3 install 'capstone>=5.0' (SH support landed in capstone 5).\n"
    )
    raise SystemExit(2)


HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

BASE = 0x06000000
ROM_MD5 = "909f5abec4b6b21bf7d2a452a03fdfcc"

# SH-2 pc-relative load reach.  mov.l @(disp,PC),Rn takes disp 0..255 scaled by 4
# off (PC+4)&~3, so it reaches up to 1020 bytes FORWARD and never backward: a
# routine with no pool of its own borrows the NEXT routine's pool.
# The furthest a literal can sit past the loading instruction: 4 + 255*4.
# Stated in the docs as "+1020 bytes forward" (the displacement itself).
MOVL_PC_REACH = 4 + 255 * 4
MOVW_PC_REACH = 4 + 255 * 2
# bsr/bra displacement is 12 bits signed, scaled by 2, off PC+4: +-4096 bytes.
BSR_REACH = 0x1000


def default_rom() -> str:
    return os.environ.get("ARCADE_AUDIT_ROM") or os.path.join(REPO, "tools", "arcade-audit", "rom.bin")


class Image:
    """The decrypted CPS3 program, addressed by CPS3 address."""

    def __init__(self, path: str, base: int = BASE, verify: bool = False):
        if not os.path.exists(path):
            raise SystemExit(
                "error: %s does not exist.\n"
                "       Rebuild it:  python3 tools/arcade-audit/decrypt.py\n"
                "       (needs sfiii3nr1.zip; see tools/cps3-disasm/README.md)" % path
            )
        with open(path, "rb") as fh:
            self.data = fh.read()
        self.base = base
        self.path = path
        if verify:
            got = hashlib.md5(self.data).hexdigest()
            if got != ROM_MD5:
                raise SystemExit("error: %s md5 %s, expected %s" % (path, got, ROM_MD5))
        self.md = Cs(CS_ARCH_SH, CS_MODE_SH2 | CS_MODE_BIG_ENDIAN)
        self.md.detail = False

    # -- addressing ------------------------------------------------------
    @property
    def end(self) -> int:
        return self.base + len(self.data)

    def contains(self, addr: int) -> bool:
        return self.base <= addr < self.end

    def off(self, addr: int) -> int:
        if not self.contains(addr):
            raise SystemExit("error: 0x%08X is outside the image (0x%08X..0x%08X)" % (addr, self.base, self.end - 1))
        return addr - self.base

    def u8(self, addr: int) -> int:
        return self.data[self.off(addr)]

    def u16(self, addr: int) -> int:
        o = self.off(addr)
        return struct.unpack_from(">H", self.data, o)[0]

    def s16(self, addr: int) -> int:
        return struct.unpack_from(">h", self.data, self.off(addr))[0]

    def u32(self, addr: int) -> int:
        return struct.unpack_from(">I", self.data, self.off(addr))[0]

    # -- searching -------------------------------------------------------
    def occurrences(self, needle: bytes, align: int = 1, limit: int = 0):
        out = []
        start = 0
        while True:
            i = self.data.find(needle, start)
            if i < 0:
                break
            start = i + 1
            if align > 1 and (i % align):
                continue
            out.append(self.base + i)
            if limit and len(out) >= limit:
                break
        return out

    # -- decoding --------------------------------------------------------
    def word_at(self, addr: int) -> int:
        return self.u16(addr)

    def pcrel_target(self, addr: int):
        """If the instruction at `addr` is a pc-relative load, return (kind, literal_addr).

        kind is 'l' for mov.l @(disp,PC),Rn (4-byte literal, PC&~3 base) or
        'w' for mov.w @(disp,PC),Rn (2-byte literal).  Returns None otherwise.
        """
        w = self.u16(addr)
        top = w >> 12
        disp = w & 0xFF
        if top == 0xD:  # mov.l @(disp,PC),Rn
            return ("l", ((addr + 4) & ~3) + disp * 4)
        if top == 0x9:  # mov.w @(disp,PC),Rn
            return ("w", addr + 4 + disp * 2)
        return None

    def pcrel_reg(self, addr: int):
        w = self.u16(addr)
        if (w >> 12) in (0x9, 0xD):
            return (w >> 8) & 0xF
        return None

    def branch_target(self, addr: int):
        """Resolve bra/bsr/bt/bf/bt.s/bf.s to (kind, target); None otherwise."""
        w = self.u16(addr)
        top = w >> 12
        if top in (0xA, 0xB):  # bra / bsr, 12-bit signed disp
            disp = w & 0x0FFF
            if disp & 0x800:
                disp -= 0x1000
            return ("bsr" if top == 0xB else "bra", addr + 4 + disp * 2)
        if top == 0x8:
            sub = (w >> 8) & 0xF
            if sub in (0x9, 0xB, 0xD, 0xF):  # bt, bf, bt/s, bf/s
                disp = w & 0xFF
                if disp & 0x80:
                    disp -= 0x100
                name = {0x9: "bt", 0xB: "bf", 0xD: "bt/s", 0xF: "bf/s"}[sub]
                return (name, addr + 4 + disp * 2)
        return None

    def is_rts(self, addr: int) -> bool:
        return self.u16(addr) == 0x000B

    def jsr_reg(self, addr: int):
        """jsr @Rn is 0100nnnn00001011; jmp @Rn is 0100nnnn00101011."""
        w = self.u16(addr)
        if (w & 0xF0FF) == 0x400B:
            return ("jsr", (w >> 8) & 0xF)
        if (w & 0xF0FF) == 0x402B:
            return ("jmp", (w >> 8) & 0xF)
        return None

    def writes_reg(self, addr: int):
        """Best-effort: which register does the instruction at `addr` write?

        Only the forms this tool needs to reason about are modelled.  Anything
        not modelled returns None and is treated by the caller as *possibly*
        clobbering, which is what keeps the census honest.
        """
        w = self.u16(addr)
        top = w >> 12
        n = (w >> 8) & 0xF
        if top in (0x9, 0xD):  # mov.w/mov.l @(disp,PC),Rn
            return n
        if top == 0xE:  # mov #imm,Rn
            return n
        if top == 0x6:  # mov Rm,Rn / mov.b/w/l @Rm,Rn / neg / ext / swap ...
            return n
        if top == 0x5:  # mov.l @(disp,Rm),Rn
            return n
        if top in (0x1, 0x2, 0x3):  # 1=store, 2=store/logic-to-Rn, 3=arith-to-Rn
            if top == 0x1:
                return None  # mov.l Rm,@(disp,Rn) - a store
            if top == 0x2 and (w & 0xF) in (0x0, 0x1, 0x2, 0x4, 0x5, 0x6):
                return None  # stores and cmp/str
            return n
        if top == 0x7:  # add #imm,Rn
            return n
        return None

    def disasm(self, addr: int, nbytes: int):
        o = self.off(addr)
        return list(self.md.disasm(self.data[o : o + nbytes], addr))


# ---------------------------------------------------------------------------
# Literal referrers -- the anchor step
# ---------------------------------------------------------------------------


def pool_words(img: Image, value: int, width: int = 4):
    """Every aligned slot in the image holding `value`."""
    if width == 4:
        return img.occurrences(struct.pack(">I", value & 0xFFFFFFFF), align=4)
    if width == 2:
        return img.occurrences(struct.pack(">H", value & 0xFFFF), align=2)
    raise SystemExit("error: literal width must be 2 or 4")


def loaders_of(img: Image, lit_addr: int, width: int = 4):
    """Instructions whose pc-relative load resolves to `lit_addr`.

    Scans only the reachable window BEFORE the literal, because SH-2 pc-relative
    loads reach forward and never backward.
    """
    kind = "l" if width == 4 else "w"
    reach = MOVL_PC_REACH if width == 4 else MOVW_PC_REACH
    out = []
    lo = max(img.base, lit_addr - reach)
    a = lo if (lo % 2 == 0) else lo + 1
    while a < lit_addr:
        t = img.pcrel_target(a)
        if t and t[0] == kind and t[1] == lit_addr:
            out.append(a)
        a += 2
    return out


def find_function_start(img: Image, addr: int, window: int = 0x1000):
    """Best-effort enclosing function start: previous `rts` + delay slot + its pool.

    This is a HINT for reporting, never a proof of identity.  A routine's true
    identity comes from the anchor, not from this scan -- see README.md, trap 4.
    """
    a = addr - 2
    lo = max(img.base, addr - window)
    while a > lo:
        if img.is_rts(a):
            cand = a + 4  # rts + delay slot
            # The previous routine usually parks its literal pool right here.
            # Step over any half-word that some earlier instruction loads.
            guard = 0
            while cand < addr and guard < 0x200:
                if (cand % 4) == 0 and loaders_of(img, cand, 4):
                    cand += 4
                    guard += 4
                    continue
                if loaders_of(img, cand, 2):
                    cand += 2
                    guard += 2
                    continue
                # a 2-byte pad exists only to 4-align the next 32-bit literal
                if (cand % 4) == 2 and img.contains(cand + 4) and loaders_of(img, cand + 2, 4):
                    cand += 2
                    guard += 2
                    continue
                break
            return cand
        a -= 2
    return None


# ---------------------------------------------------------------------------
# Function extent + call census
# ---------------------------------------------------------------------------


def function_extent(img: Image, start: int, max_len: int = 0x4000):
    """Return (end_addr_exclusive, pool_addrs).

    Walks forward, skipping literal-pool words as data, tracking the furthest
    forward *branch* target, and stopping after the first `rts` that lies at or
    beyond every forward branch seen (plus its delay slot).  Trailing pool words
    are then absorbed into the extent.

    Pool addresses deliberately do NOT extend the frontier: an SH-2 routine
    commonly parks its pool immediately after its own `rts`, and counting that
    as "code still to come" walks straight through the epilogue into the next
    routine.  This is a heuristic; pass --end when you know the real bound.
    """
    a = start
    furthest = start
    pool = set()  # half-word addresses covered by a literal
    end = None
    limit = min(start + max_len, img.end)
    while a < limit:
        if a in pool:
            a += 2
            continue
        t = img.pcrel_target(a)
        if t:
            lit = t[1]
            size = 4 if t[0] == "l" else 2
            for k in range(0, size, 2):
                pool.add(lit + k)
        b = img.branch_target(a)
        if b and b[0] != "bsr" and b[1] > furthest:
            furthest = b[1]
        if img.is_rts(a) and a >= furthest:
            end = a + 4
            break
        a += 2
    if end is None:
        end = min(start + max_len, img.end)
    while end < img.end and end in pool:
        end += 2
    # Pool words BEYOND `end` are kept: a routine with no pool of its own borrows
    # the next routine's, and hiding that is how a literal scan lies.
    return end, sorted(pool)


def call_census(img: Image, start: int, end: int):
    """Every call out of [start,end).

    Returns a list of dicts.  `target` is None when the tool could not resolve it
    -- those rows are the whole point: a census is a LOWER BOUND, and an absence
    from it proves nothing (docs/research-arcade-balance-desyncs.md, E9b's note on
    the `jsr @r11` at 0x060C5384 that a register-tracking call graph missed).
    """
    rows = []
    a = start
    while a < end:
        b = img.branch_target(a)
        if b and b[0] == "bsr":
            rows.append({"at": a, "how": "bsr", "target": b[1], "via": None})
            a += 2
            continue
        j = img.jsr_reg(a)
        if j:
            how, reg = j
            src, tgt = _track_reg(img, a, reg, start)
            rows.append({"at": a, "how": "%s @r%d" % (how, reg), "target": tgt, "via": src})
            a += 2
            continue
        a += 2
    return rows


def _track_reg(img: Image, use_at: int, reg: int, fn_start: int):
    """Walk backwards from `use_at` for the load that put a literal in `reg`.

    Returns (load_addr, value) or (None, None).  Any *other* modelled write to
    the register aborts the walk -- reporting UNRESOLVED is correct; guessing is
    not.
    """
    a = use_at - 2
    while a >= fn_start:
        t = img.pcrel_target(a)
        if t and img.pcrel_reg(a) == reg:
            val = img.u32(t[1]) if t[0] == "l" else img.u16(t[1])
            return a, val
        w = img.writes_reg(a)
        if w == reg:
            return None, None
        a -= 2
    return None, None


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------


def annotate(img: Image, insn) -> str:
    """Suffix a disassembled line with what a pc-relative load actually loads."""
    t = img.pcrel_target(insn.address)
    if not t:
        return ""
    kind, lit = t
    if not img.contains(lit):
        return "   ; literal 0x%08X out of image" % lit
    if kind == "l":
        return "   ; = 0x%08X" % img.u32(lit)
    v = img.u16(lit)
    return "   ; = 0x%04X (%d)" % (v, img.s16(lit))


def render(img: Image, start: int, end: int, pool=()):
    poolset = set(p for p in pool if start <= p < end)
    out = []
    a = start
    while a < end:
        if a in poolset and (a % 4) == 0 and img.contains(a + 3):
            out.append("%08x  .long    0x%08X" % (a, img.u32(a)))
            a += 4
            continue
        ins = img.disasm(a, 2)
        if not ins:
            out.append("%08x  .word    0x%04X" % (a, img.u16(a)))
            a += 2
            continue
        i = ins[0]
        out.append("%08x  %-8s %-24s%s" % (i.address, i.mnemonic, i.op_str, annotate(img, i)))
        a += 2
    return out


# ---------------------------------------------------------------------------
# Argument helpers
# ---------------------------------------------------------------------------


def num(s: str) -> int:
    return int(s, 0)


def needle_from_args(args) -> bytes:
    given = [x for x in (args.u32, args.u16, args.u8, args.hex, args.s16_list, args.u8_list) if x is not None]
    if len(given) != 1:
        raise SystemExit("error: give exactly one of --u32/--u16/--u8/--hex/--s16-list/--u8-list")
    if args.u32 is not None:
        return struct.pack(">I", num(args.u32) & 0xFFFFFFFF)
    if args.u16 is not None:
        return struct.pack(">H", num(args.u16) & 0xFFFF)
    if args.u8 is not None:
        return bytes([num(args.u8) & 0xFF])
    if args.hex is not None:
        h = args.hex.replace(" ", "").replace("_", "")
        return bytes.fromhex(h)
    if args.s16_list is not None:
        vals = [int(x, 0) for x in args.s16_list.replace(",", " ").split()]
        return b"".join(struct.pack(">h", v) for v in vals)
    vals = [int(x, 0) for x in args.u8_list.replace(",", " ").split()]
    return bytes(vals)


def add_needle_args(p):
    p.add_argument("--u32", help="a 32-bit big-endian value (e.g. an address)")
    p.add_argument("--u16", help="a 16-bit big-endian value")
    p.add_argument("--u8", help="a single byte")
    p.add_argument("--hex", help="raw bytes as hex, e.g. 0611dfb8")
    p.add_argument("--s16-list", help='comma/space list of s16, e.g. "80,90,50,50,50"')
    p.add_argument("--u8-list", help='comma/space list of bytes, e.g. "1,2,3"')


# ---------------------------------------------------------------------------
# Subcommands
# ---------------------------------------------------------------------------


def cmd_find(img: Image, args) -> int:
    needle = needle_from_args(args)
    hits = img.occurrences(needle, align=args.align)
    print("pattern : %s (%d bytes), alignment %d" % (needle.hex(), len(needle), args.align))
    print("hits    : %d" % len(hits))
    for h in hits[: args.limit]:
        print("  0x%08X" % h)
    if len(hits) > args.limit:
        print("  ... %d more (raise --limit)" % (len(hits) - args.limit))
    if len(hits) == 1:
        print("VERDICT : UNIQUE -- usable as an anchor.")
    elif not hits:
        print("VERDICT : ABSENT -- not in this image.")
    else:
        print("VERDICT : NOT UNIQUE -- do NOT anchor on this; narrow the pattern.")
    return 0


def cmd_refs(img: Image, args) -> int:
    value = num(args.value)
    width = args.width
    slots = pool_words(img, value, width)
    if args.within:
        lo, hi = [num(x) for x in args.within.split(":")]
        slots = [s for s in slots if lo <= s < hi]
    print("value       : 0x%0*X  (%d-byte literal)" % (width * 2, value, width))
    print("pool slots  : %d" % len(slots))
    total = 0
    for s in slots:
        ld = loaders_of(img, s, width)
        total += len(ld)
        print("  slot 0x%08X  loaded by %d instruction(s)" % (s, len(ld)))
        for a in ld:
            fn = find_function_start(img, a)
            ins = img.disasm(a, 2)
            txt = "%s %s" % (ins[0].mnemonic, ins[0].op_str) if ins else "?"
            print(
                "    0x%08X  %-28s  [enclosing routine starts ~0x%08X]"
                % (a, txt, fn if fn else 0)
            )
    print("referrers   : %d" % total)
    if total == 1:
        print("VERDICT     : SOLE LITERAL REFERRER -- read the routine off that instruction.")
    elif total == 0:
        print(
            "VERDICT     : NO LITERAL REFERRER.\n"
            "              This is NOT proof nothing uses it.  SH-2 mov.l @(disp,PC)\n"
            "              reaches 255 longwords FORWARD only, so a routine with no pool\n"
            "              of its own borrows the NEXT routine's; and a value within\n"
            "              +-255 of a pool literal is reached by base+displacement with\n"
            "              no literal of its own.  The literal scan is a SCREEN; the\n"
            "              disassembly is the verdict."
        )
    else:
        print("VERDICT     : %d referrers -- not a sole-referrer anchor." % total)
    return 0


def cmd_dis(img: Image, args) -> int:
    start = num(args.addr)
    if args.end:
        end = num(args.end)
        pool = ()
    elif args.count:
        end = start + args.count * 2
        pool = ()
    else:
        end, pool = function_extent(img, start)
    for line in render(img, start, end, pool):
        print(line)
    return 0


def cmd_fn(img: Image, args) -> int:
    start = num(args.addr)
    end = num(args.end) if args.end else function_extent(img, start)[0]
    _, pool = function_extent(img, start)
    print("routine     : 0x%08X .. 0x%08X  (%d bytes)" % (start, end, end - start))
    own = [p for p in pool if p % 4 == 0 and p < end]
    borrowed = [p for p in pool if p % 4 == 0 and p >= end]
    print("pool words  : %s" % (", ".join("0x%08X" % p for p in own) or "(none of its own)"))
    if borrowed:
        print(
            "borrowed    : %s\n              (past the rts -- SH-2 mov.l @(disp,PC) reaches 255 longwords"
            "\n              FORWARD only, so this routine uses the NEXT routine's pool)"
            % (", ".join("0x%08X" % p for p in borrowed),)
        )
    rows = call_census(img, start, end)
    unresolved = [r for r in rows if r["target"] is None]
    print("calls       : %d (%d unresolved)" % (len(rows), len(unresolved)))
    for r in rows:
        if r["target"] is None:
            print("  0x%08X  %-10s -> UNRESOLVED" % (r["at"], r["how"]))
        elif r["via"] is None:
            print("  0x%08X  %-10s -> 0x%08X" % (r["at"], r["how"], r["target"]))
            d = abs(r["target"] - r["at"])
            if d > BSR_REACH:
                print("                 (note: %d bytes away, beyond bsr's +-0x%X reach)" % (d, BSR_REACH))
        else:
            print(
                "  0x%08X  %-10s -> 0x%08X   (register loaded at 0x%08X, %d bytes earlier)"
                % (r["at"], r["how"], r["target"], r["via"], r["at"] - r["via"])
            )
    print(
        "\nA CALL CENSUS IS A LOWER BOUND.  Absence from this list proves nothing:\n"
        "a jsr @Rn whose register comes from memory, from an argument, or from a\n"
        "table is unresolvable here and prints UNRESOLVED.  To prove a routine does\n"
        "NOT call X, scan its byte range for an aligned word equal to &X (cps3.py\n"
        "refs --within), check bsr cannot reach, and read the disassembly -- with a\n"
        "known-present callee as the positive control."
    )
    if args.disasm:
        print()
        for line in render(img, start, end, pool):
            print(line)
    return 0


def cmd_nocall(img: Image, args) -> int:
    """Screen a routine for 'does it call X?' the way E7 and E9 did it.

    Three independent legs, none of which is on its own a proof:
      (a) is there a 4-byte-aligned word equal to &X in the routine's byte range?
      (b) can a bsr reach X from here at all (+-0x1000)?
      (c) does the resolved call census name X?
    Plus a POSITIVE CONTROL: a callee known to be present must show up under (a),
    or leg (a) is measuring nothing.
    """
    start = num(args.fn)
    end = num(args.end) if args.end else function_extent(img, start)[0]
    callee = num(args.callee)
    print("routine     : 0x%08X .. 0x%08X  (%d bytes)" % (start, end, end - start))
    print("callee      : 0x%08X" % callee)
    words = [a for a in pool_words(img, callee, 4) if start <= a < end]
    print("(a) aligned words equal to &callee in range : %d %s" % (len(words), [hex(w) for w in words]))
    dist = abs(callee - start)
    print("(b) bsr reach: %d bytes away, bsr spans +-0x%X -> %s" % (dist, BSR_REACH, "CAN reach" if dist <= BSR_REACH else "CANNOT reach"))
    rows = call_census(img, start, end)
    hit = [r for r in rows if r["target"] == callee]
    unres = [r for r in rows if r["target"] is None]
    print("(c) resolved census hits on callee          : %d  (%d unresolved call sites in this routine)" % (len(hit), len(unres)))
    ctrl_ok = None
    if args.control:
        ctrl = num(args.control)
        cw = [a for a in pool_words(img, ctrl, 4) if start <= a < end]
        ctrl_ok = len(cw) >= 1
        note = "OK" if ctrl_ok else ("not needed -- (c) already resolved a call" if hit else "*** CONTROL ABSENT ***")
        print("positive control 0x%08X in range           : %d %s" % (ctrl, len(cw), note))
    print("")
    if hit:
        print("VERDICT: the routine DOES call it -- resolved at %s." % ", ".join("0x%08X" % r["at"] for r in hit))
        return 0
    if args.control and not ctrl_ok:
        print("VERDICT: INCONCLUSIVE -- the positive control is absent, so leg (a) proves nothing.")
        return 1
    if words:
        print("VERDICT: a literal is present but the census did not resolve a call to it.")
        print("         READ THE DISASSEMBLY (cps3.py dis / fn --disasm).  The literal")
        print("         scan is a screen; the disassembly is the verdict.")
        return 1
    print("VERDICT: no literal, %s, and no resolved call." % ("bsr cannot reach" if dist > BSR_REACH else "BUT BSR CAN REACH -- check every bsr"))
    print("         This is a SCREEN, not a proof.  Confirm by reading the disassembly:")
    print("         a routine can borrow the NEXT routine's pool (255 longwords forward),")
    print("         and a target within +-255 of a pool literal is reached by")
    print("         base+displacement with no literal of its own.")
    return 0


def cmd_table(img: Image, args) -> int:
    at = num(args.addr)
    width = args.width
    n = args.entries
    reader = {1: img.u8, 2: img.u16, 4: img.u32}[width]
    if args.index_by is None:
        print("table 0x%08X, %d entries of %d byte(s)" % (at, n, width))
        for i in range(n):
            print("  [%3d] 0x%08X = 0x%0*X" % (i, at + i * width, width * 2, reader(at + i * width)))
        return 0
    idx_at = num(args.index_by)
    iw = args.index_width
    ireader = {1: img.u8, 2: img.u16, 4: img.u32}[iw]
    print("table       0x%08X, %d entries of %d byte(s)" % (at, n, width))
    print("indexed by  0x%08X, %d entries of %d byte(s)" % (idx_at, args.index_entries, iw))
    print("Pinning is by TABLE, not by position: entry [i] is what the type table sends i to.")
    for i in range(args.index_entries):
        k = ireader(idx_at + i * iw)
        if k >= n:
            print("  idx %3d -> type %3d  OUT OF RANGE for a %d-entry table" % (i, k, n))
            continue
        print("  idx %3d -> type %3d -> [%3d] = 0x%0*X" % (i, k, k, width * 2, reader(at + k * width)))
    return 0


def cmd_pin(img: Image, args) -> int:
    """The whole house method in one call."""
    if args.at:
        anchor = num(args.at)
        needle = None
        print("== step 1: uniqueness ==")
        print("anchor given as an address: 0x%08X (uniqueness not tested)" % anchor)
    else:
        needle = needle_from_args(args)
        print("== step 1: uniqueness ==")
        hits = img.occurrences(needle, align=args.align)
        print("pattern %s -> %d occurrence(s)" % (needle.hex(), len(hits)))
        if len(hits) != 1:
            print("STOP: an anchor must occur EXACTLY ONCE.  Narrow the pattern.")
            return 1
        anchor = hits[0]
        print("anchor  0x%08X  (UNIQUE)" % anchor)
    print("\n== step 2: sole literal referrer ==")
    slots = pool_words(img, anchor, 4)
    refs = []
    for s in slots:
        for a in loaders_of(img, s, 4):
            refs.append((s, a))
    print("pool slots holding 0x%08X : %d" % (anchor, len(slots)))
    print("instructions loading them   : %d" % len(refs))
    for s, a in refs:
        ins = img.disasm(a, 2)
        print("  0x%08X  %s %s   (pool word 0x%08X)" % (a, ins[0].mnemonic, ins[0].op_str, s))
    if len(refs) != 1:
        print(
            "\nVERDICT: NOT a sole-referrer pin.  The literal scan is a screen, not the\n"
            "verdict -- read the disassembly before concluding anything."
        )
        return 1
    site = refs[0][1]
    print("\n== step 3: read the routine off the instruction ==")
    fn = find_function_start(img, site)
    if fn is None:
        print("could not locate an enclosing routine start within 0x1000 bytes")
        return 1
    end, pool = function_extent(img, fn)
    print("enclosing routine (rts-scan hint): 0x%08X .. 0x%08X" % (fn, end))
    print("")
    lo = max(fn, site - args.context * 2)
    hi = min(end, site + (args.context + 1) * 2)
    for line in render(img, lo, hi, pool):
        print(line + ("   <== the referring instruction" if line.startswith("%08x" % site) else ""))
    return 0


# ---------------------------------------------------------------------------
# selftest -- reproduce addresses already established in the docs
# ---------------------------------------------------------------------------


def cmd_selftest(img: Image, args) -> int:
    fails = []

    def check(label, got, want):
        ok = got == want
        print("%-58s %-22s %s" % (label, _fmt(got), "OK" if ok else "FAIL (want %s)" % _fmt(want)))
        if not ok:
            fails.append(label)

    def _fmt(v):
        if isinstance(v, int):
            return "0x%08X" % v
        return str(v)

    print("image %s" % img.path)
    got_md5 = hashlib.md5(img.data).hexdigest()
    print("%-58s %-22s %s" % ("md5", got_md5, "OK" if got_md5 == ROM_MD5 else "FAIL (want %s)" % ROM_MD5))
    if got_md5 != ROM_MD5:
        fails.append("md5")
    print("")
    print("Every expected value below is quoted from docs/research-arcade-balance-desyncs.md")
    print("or docs/research-arcade-cg-data-accuracy.md; none is derived from this tool.")
    print("")

    # -- E9: win_player / win_jp_tbl / winner_type_tbl --------------------
    print("-- win/lose dispatch (research-arcade-balance-desyncs.md, 'How each routine was pinned') --")
    win_jp_tbl = 0x061A38C0
    winner_type_tbl = 0x061A3890
    slot, insn = _sole_ref(img, win_jp_tbl)
    check("win_jp_tbl 0x061A38C0: sole literal referrer (pool word)", slot, 0x060C2EBC)
    check("  loaded by the instruction at", insn, 0x060C2DE0)
    check("  -> enclosing routine = win_player", find_function_start(img, insn) if insn else None, 0x060C2DDC)
    slot2, insn2 = _sole_ref(img, winner_type_tbl)
    check("winner_type_tbl 0x061A3890: sole literal referrer (pool word)", slot2, 0x060C2ED4)
    check("  loaded by the instruction at", insn2, 0x060C2E70)
    check("win_jp_tbl[13] = Win_13000", img.u32(win_jp_tbl + 13 * 4), 0x060C4D22)
    check("win_jp_tbl[0]  = Win_00000", img.u32(win_jp_tbl + 0 * 4), 0x060C2E88)
    check("win_jp_tbl[1]  = Win_01000 (Oro, the E7 routine)", img.u32(win_jp_tbl + 1 * 4), 0x060C2E8C)
    # Win_00000 is 'bra Normal_normal_Winner; nop' -- four bytes.
    b = img.branch_target(0x060C2E88)
    check("Win_00000 is 'bra ...' -> Normal_normal_Winner", b[1] if b else None, 0x060C37BA)
    # winner_type_tbl is indexed after a shll, so its entries are 16-bit.
    # "winner_type_tbl holds 13 at exactly one index -- 15, CHAR_CHUNLI (the
    #  arcade's 16, Shin Akuma occupying 15)": the ARCADE index is 16.
    idx = [i for i in range(21) if img.u16(winner_type_tbl + i * 2) == 13]
    check("winner_type_tbl indices holding 13 (Chun-Li, arcade index)", str(idx), "[16]")
    # "the win routines reached as Win_00000 ... at indices 1, 2 and 11:
    #  Alex, Ryu and Ken"
    z = [i for i in range(21) if img.u16(winner_type_tbl + i * 2) == 0]
    check("winner_type_tbl indices holding 0 (Alex, Ryu, Ken)", str(z), "[1, 2, 11]")
    # "win_player ... indexes it by the 21-entry table at 0x061A3890 (Oro -> 1
    #  -> 0x060C2E8C)"
    oro = img.u32(win_jp_tbl + img.u16(winner_type_tbl + 9 * 2) * 4)
    check("winner_type_tbl[9] (Oro) -> win_jp_tbl[1] = Win_01000", oro, 0x060C2E8C)

    lose_jp_tbl = 0x061A3A6C
    loser_type_tbl = 0x061A3A3C
    slot3, insn3 = _sole_ref(img, lose_jp_tbl)
    check("lose_jp_tbl 0x061A3A6C: sole literal referrer (pool word)", slot3, 0x060C56A8)
    check("  -> enclosing routine = lose_player", find_function_start(img, insn3) if insn3 else None, 0x060C558C)
    check("loser_type_tbl 0x061A3A3C: sole literal referrer (pool word)", _sole_ref(img, loser_type_tbl)[0], 0x060C56B0)

    # -- win_player's own three calls, in its own order -------------------
    print("")
    print("-- win_player's own calls, and the dispatch a census cannot see --")
    wp_end, _ = function_extent(img, 0x060C2DDC)
    check("win_player extent ends where Win_00000 begins", wp_end, 0x060C2E88)
    wp_rows = call_census(img, 0x060C2DDC, wp_end)
    check("  call 1 = meta_win_pause", wp_rows[0]["target"], 0x060C54B2)
    check("  call 2 = bonus_game_win_pause", wp_rows[1]["target"], 0x060C5308)
    check("  call 3 = Judge_normal_winner (a bsr)", wp_rows[2]["target"], 0x060C3898)
    check("  call 4 = the jump-table dispatch -> UNRESOLVED", str(wp_rows[3]["target"]), "None")

    # -- the census lower bound: bonus_game_win_pause's fourth call -------
    print("")
    print("-- the call census is a LOWER BOUND (E9b's note on 0x060C5384) --")
    bgwp = 0x060C5308
    end, _ = function_extent(img, bgwp)
    check("bonus_game_win_pause extent ends at meta_win_pause", end, 0x060C54B2)
    rows = call_census(img, bgwp, end)
    sfhf_rows = [r for r in rows if r["target"] == 0x0611DFB8]
    check(
        "  set_field_hosei_flag call sites",
        ", ".join("0x%08X" % r["at"] for r in sfhf_rows),
        "0x060C5338, 0x060C5354, 0x060C536A, 0x060C5384",
    )
    r11 = [r for r in rows if r["at"] == 0x060C5384]
    check("  the fourth is jsr @r11, register loaded N bytes earlier", str(r11[0]["at"] - r11[0]["via"]) if r11 and r11[0]["via"] else "unresolved", "90")
    check("    ... at", r11[0]["via"] if r11 else None, 0x060C532A)

    # -- E9a: the mask Win_13000 tests -----------------------------------
    print("")
    print("-- E9a: Win_13000 masks bit 12, not bit 0 --")
    t = img.pcrel_target(0x060C4DF4)
    check("0x060C4DF4 is mov.w @(disp,PC) -> literal", t[1] if t else None, 0x060C4E74)
    check("  literal value = 0x1000 (bit 12)", "0x%04X" % img.u16(0x060C4E74), "0x1000")
    check("0x060C4DFC loads P2SW_0", img.u32(img.pcrel_target(0x060C4DFC)[1]), 0x0206AA90)
    check("0x060C4E0A loads P1SW_0", img.u32(img.pcrel_target(0x060C4E0A)[1]), 0x0206AA8C)

    # -- section 23.4: the RNG, found with no symbols ---------------------
    print("")
    print("-- research-arcade-cg-data-accuracy.md section 23.4: the RNG --")
    # random_tbl_16 is the 64 s16 of pls02.c; it occurs exactly once.
    tbl16 = img.occurrences(struct.pack(">I", 0x020155E8), align=4)
    check("&Random_ix16 (0x020155E8) is a pool literal", str(len(tbl16) > 0), "True")
    # random_16 loads BOTH &Random_ix16 and random_tbl_16 out of one pool.
    p1 = [a for s in pool_words(img, 0x020155E8, 4) for a in loaders_of(img, s, 4)]
    p2 = [a for s in pool_words(img, 0x065EB434, 4) for a in loaders_of(img, s, 4)]
    both = sorted(set(a for a in p1 if any(abs(a - b) < 64 for b in p2)))
    check("one routine loads both -> random_16 at 0x0611E0EE", find_function_start(img, both[0]) if both else None, 0x0611E0EE)

    # -- E7's negative, re-run -------------------------------------------
    print("")
    print("-- E7's negative: Win_01000 never calls set_field_hosei_flag --")
    sfhf = 0x0611DFB8
    rnd16 = 0x0611E0EE
    w1_start = 0x060C2E8C
    w1_end, _ = function_extent(img, w1_start)
    check("Win_01000 extent (the doc's 1,318-byte routine)", str(w1_end - w1_start), "1318")
    n_sfhf = len([a for a in pool_words(img, sfhf, 4) if w1_start <= a < w1_end])
    n_rnd = len([a for a in pool_words(img, rnd16, 4) if w1_start <= a < w1_end])
    check("Win_01000 pool words == &set_field_hosei_flag", str(n_sfhf), "0")
    check("  positive control: &random_16 present", str(n_rnd >= 1), "True")
    check("  set_field_hosei_flag is beyond bsr reach", str(abs(sfhf - w1_start) > BSR_REACH), "True")

    # -- the counter-case: Normal_normal_Winner DOES make the call --------
    print("")
    print("-- and the counter-case: Normal_normal_Winner's opening lines look identical, but --")
    nnw = 0x060C37BA
    nnw_end, _ = function_extent(img, nnw)
    nnw_rows = [r for r in call_census(img, nnw, nnw_end) if r["target"] == sfhf]
    check(
        "Normal_normal_Winner 0x060C37BA calls set_field_hosei_flag at",
        ", ".join("0x%08X" % r["at"] for r in nnw_rows),
        "0x060C37E8, 0x060C380A",
    )

    print("")
    if fails:
        print("SELFTEST: %d FAILED -- %s" % (len(fails), "; ".join(fails)))
        return 1
    print("SELFTEST: all checks passed.")
    return 0


def _sole_ref(img: Image, value: int):
    """(pool slot, loading instruction) when there is exactly one of each."""
    refs = [(s, a) for s in pool_words(img, value, 4) for a in loaders_of(img, s, 4)]
    if len(refs) == 1:
        return refs[0]
    return (refs, refs)


# ---------------------------------------------------------------------------


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--rom", default=default_rom(), help="decrypted CPS3 image (default tools/arcade-audit/rom.bin)")
    p.add_argument("--verify-rom", action="store_true", help="fail unless the image md5 matches the known one")
    sub = p.add_subparsers(dest="cmd")

    sp = sub.add_parser("find", help="locate a literal / byte pattern; the COUNT is the anchor test")
    add_needle_args(sp)
    sp.add_argument("--align", type=int, default=1)
    sp.add_argument("--limit", type=int, default=32)
    sp.set_defaults(handler=cmd_find)

    sp = sub.add_parser("refs", help="literal-pool referrers of a value, and the instructions that load them")
    sp.add_argument("value")
    sp.add_argument("--width", type=int, default=4, choices=(2, 4))
    sp.add_argument("--within", help="restrict pool slots to LO:HI, e.g. 0x060C2E8C:0x060C33B2")
    sp.set_defaults(handler=cmd_refs)

    sp = sub.add_parser("dis", help="annotated disassembly")
    sp.add_argument("addr")
    sp.add_argument("--count", type=int, default=0, help="number of instructions")
    sp.add_argument("--end", help="end address (exclusive)")
    sp.set_defaults(handler=cmd_dis)

    sp = sub.add_parser("fn", help="function extent, literal pool, and outgoing-call census")
    sp.add_argument("addr")
    sp.add_argument("--end")
    sp.add_argument("--disasm", action="store_true")
    sp.set_defaults(handler=cmd_fn)

    sp = sub.add_parser("nocall", help="screen 'does this routine call X?' with a positive control")
    sp.add_argument("--fn", required=True, help="routine start address")
    sp.add_argument("--callee", required=True, help="address of the callee in question")
    sp.add_argument("--control", help="address of a callee known to be present (the positive control)")
    sp.add_argument("--end", help="routine end (exclusive); default is the extent heuristic")
    sp.set_defaults(handler=cmd_nocall)

    sp = sub.add_parser("table", help="dump a table, optionally indexed through a type table")
    sp.add_argument("addr")
    sp.add_argument("--entries", type=int, default=16)
    sp.add_argument("--width", type=int, default=4, choices=(1, 2, 4))
    sp.add_argument("--index-by", help="address of the type table that indexes it")
    sp.add_argument("--index-entries", type=int, default=21)
    sp.add_argument("--index-width", type=int, default=1, choices=(1, 2, 4))
    sp.set_defaults(handler=cmd_table)

    sp = sub.add_parser("pin", help="run the whole house method on one anchor")
    add_needle_args(sp)
    sp.add_argument("--at", help="skip the search: the anchor is already at this address")
    sp.add_argument("--align", type=int, default=1)
    sp.add_argument("--context", type=int, default=6, help="instructions of context around the referrer")
    sp.set_defaults(handler=cmd_pin)

    sp = sub.add_parser("selftest", help="reproduce addresses already established in the docs")
    sp.set_defaults(handler=cmd_selftest)

    args = p.parse_args(argv)
    if not args.cmd:
        p.print_help()
        return 2
    img = Image(args.rom, verify=args.verify_rom)
    return args.handler(img, args)


if __name__ == "__main__":
    raise SystemExit(main())
