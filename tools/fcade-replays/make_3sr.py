#!/usr/bin/env python3
"""SCRD -> per-game `.3sr` converter (plan-fcade-replay-browser.md Step B2).

Format spec: docs/3sr-format.md (read that file first -- this module is a
straight implementation of it, byte offset for byte offset).

PROVENANCE:
  - Game-start detection + setup-block field reads mirror
    src/test/scrd_game.c's ScrdGame_Init (G_No signature, MY_CHAR/SUPER_ARTS/
    NEW_CHALLENGER/PLAYER_COLOR offsets, CHAR_ARCADE_TO_3SX).
  - RNG sync source (Random_ix16/32 from the game-start frame, i.e. frame
    `start_index - 1` in scrd_game.c's own indexing) mirrors
    src/test/statcheck_runner.c:341-346 (PHASE_GAME_TRANSITION).
  - Input words are P1SW_0/P2SW_0 verbatim (arcade-RAM layout), matching
    tools/fcade-replays/decode_inputs.py's cross-check reads and
    arcade_constants.h:27-28.
  - The SCRD container reader (magic/table/zero-run decode) is REUSED from
    decode_inputs.py (read_scrd_table, zero_run_decode, RAM_FRAME_SIZE) --
    not duplicated. Only the per-frame *field extraction* is new here,
    because decode_inputs.py's extract_scrd_words() only pulls out
    P1SW_0/1, P2SW_0/1 and this tool additionally needs the setup-block and
    checksum-window offsets.
  - djb2 checksum: byte-for-byte the codebase's actual src/sf33rd/utils/
    djb2_hash.h algorithm (hash = hash*33 + byte, the ADDITIVE variant --
    see docs/3sr-format.md §4.3 "Deviation note" for why this, not the XOR
    shorthand some casual descriptions call "djb2", is what's implemented
    here: it's the real function already wired through netplay's desync
    detector, so Stage C can reuse it verbatim).

Subcommands:
  generate  <archive.scrd> --out <out.3sr>
            [--meta-out <out.meta.json>] [--quark-json <quark.json>]
            [--summary-json <summary.json>] [--checksum-interval N=60]
  verify    <replay.3sr> [--against-scrd <archive.scrd>]
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

from decode_inputs import (  # noqa: E402  (path insert must precede this)
    RAM_FRAME_SIZE,
    SCRD_FILENAME_RE,
    normalize_quark,
    read_scrd_table,
    zero_run_decode,
)

# --- CPS3 arcade RAM offsets (src/arcade/arcade_constants.h) --------------

GAME_TIMER_OFFSET = 0x1136C
MY_CHAR_OFFSET = 0x11387
SUPER_ARTS_OFFSET = 0x1138B
NEW_CHALLENGER_OFFSET = 0x113DA
G_NO_OFFSET = 0x15436
C_NO_OFFSET = 0x154A6
RANDOM_IX_16_OFFSET = 0x155E8
RANDOM_IX_32_OFFSET = 0x155EA
PLAYER_COLOR_OFFSET = 0x15683
WORK_XYZ_OFFSET = 0x64
PLW_OFFSET = 0x68C6C
PLW_SIZE = 0x498
P1SW_0_OFFSET = 0x6AA8C
P2SW_0_OFFSET = 0x6AA90

# src/constants.h:27,53,63 -- CHAR_AKUMA == 14 in both the CPS3 (arcade,
# 21-id) and non-CPS3 (3SX, 20-id) Character enums; CHAR_ARCADE_TO_3SX
# shifts every arcade id above it down by one (removing CHAR_SHIN_AKUMA's
# gap, which only exists arcade-side).
CHAR_AKUMA = 14


def char_arcade_to_3sx(c: int) -> int:
    return c - 1 if c > CHAR_AKUMA else c


# --- format constants (docs/3sr-format.md §1) -----------------------------

MAGIC = b"3SR1"
VERSION = 1
HEADER_SIZE = 28
DEFAULT_CHECKSUM_INTERVAL = 60

_HEADER_STRUCT = struct.Struct("<4sHHBBBBBBBBHHIHH")
_INPUT_WORD_STRUCT = struct.Struct("<HH")
_CHECKSUM_ENTRY_STRUCT = struct.Struct("<II")

assert _HEADER_STRUCT.size == HEADER_SIZE, _HEADER_STRUCT.size

# --- checksum window (docs/3sr-format.md §4.2), fixed order --------------
# (name, offset, kind) -- kind is "u16" or "s16" (archive-native BE width).

CHECKSUM_FIELDS: tuple[tuple[str, int, str], ...] = (
    ("C_No[0]", C_NO_OFFSET + 0, "u16"),
    ("C_No[1]", C_NO_OFFSET + 2, "u16"),
    ("C_No[2]", C_NO_OFFSET + 4, "u16"),
    ("C_No[3]", C_NO_OFFSET + 6, "u16"),
    ("Game_timer", GAME_TIMER_OFFSET, "u16"),
    ("Random_ix16", RANDOM_IX_16_OFFSET, "s16"),
    ("Random_ix32", RANDOM_IX_32_OFFSET, "s16"),
    ("P1_pos_x", PLW_OFFSET + 0 * PLW_SIZE + WORK_XYZ_OFFSET, "s16"),
    ("P1_pos_y", PLW_OFFSET + 0 * PLW_SIZE + WORK_XYZ_OFFSET + 4, "s16"),
    ("P2_pos_x", PLW_OFFSET + 1 * PLW_SIZE + WORK_XYZ_OFFSET, "s16"),
    ("P2_pos_y", PLW_OFFSET + 1 * PLW_SIZE + WORK_XYZ_OFFSET + 4, "s16"),
    ("P1SW_0", P1SW_0_OFFSET, "u16"),
    ("P2SW_0", P2SW_0_OFFSET, "u16"),
)


def djb2(data: bytes) -> int:
    """src/sf33rd/utils/djb2_hash.h, verbatim: hash=5381; hash = hash*33+byte."""
    h = 5381
    for b in data:
        h = (h * 33 + b) & 0xFFFFFFFF
    return h


def compute_checksum(frame: bytes) -> int:
    buf = bytearray()
    for _name, offset, kind in CHECKSUM_FIELDS:
        if kind == "u16":
            (val,) = struct.unpack_from(">H", frame, offset)
        else:
            (val,) = struct.unpack_from(">h", frame, offset)
            val &= 0xFFFF
        buf += struct.pack("<H", val)
    return djb2(bytes(buf))


# =============================== errors ===================================


class CorruptArchiveError(Exception):
    """The SCRD archive itself is unusable (e.g. all-zero frame table)."""


class ExtractError(Exception):
    """The archive is readable but the expected game-start signature (or
    other required data) was never found."""


class FormatError(Exception):
    """A .3sr file failed structural validation."""


# ============================ setup + game data ============================


@dataclass
class SetupBlock:
    characters: tuple[int, int]
    supers: tuple[int, int]
    colors: tuple[int, int]
    new_challenger: int
    random_ix16: int  # canonicalized to u16 (see docs/3sr-format.md §2)
    random_ix32: int


@dataclass
class ScrdGameData:
    setup: SetupBlock
    p1_words: list[int]
    p2_words: list[int]
    checksum_table: list[tuple[int, int]]  # (frame_index, djb2)
    game_start_frame: int  # SCRD archive frame index S (0-based)
    archive_entry_count: int


def extract_scrd_game(path: Path, checksum_interval: int = DEFAULT_CHECKSUM_INTERVAL) -> ScrdGameData:
    """Mirrors src/test/scrd_game.c's ScrdGame_Init (game-start scan) plus
    the per-frame P1SW_0/P2SW_0 + checksum-window extraction described in
    docs/3sr-format.md. Single forward pass over the SCRD archive, reusing
    decode_inputs.py's table/decompress primitives."""
    data = path.read_bytes()
    table = read_scrd_table(data)

    if table.frame_count == 0:
        raise CorruptArchiveError(f"{path}: SCRD frame table has zero entries")

    if all(off == 0 and size == 0 for off, size in table.entries):
        raise CorruptArchiveError(
            f"{path}: SCRD frame table is all-zero (offset=0,size=0 for all "
            f"{table.frame_count} entries) -- archive is corrupt/truncated, "
            f"cannot extract a .3sr from it"
        )

    accum_int = 0
    game_start_index: Optional[int] = None
    setup: Optional[SetupBlock] = None
    p1_words: list[int] = []
    p2_words: list[int] = []
    checksum_table: list[tuple[int, int]] = []

    for i, (off, size) in enumerate(table.entries):
        compressed = data[off:off + size]
        payload = zero_run_decode(compressed, RAM_FRAME_SIZE)
        payload_int = int.from_bytes(payload, "big")
        accum_int = payload_int if i == 0 else (accum_int ^ payload_int)

        if game_start_index is None:
            frame = accum_int.to_bytes(RAM_FRAME_SIZE, "big")
            g_no_1, g_no_2, g_no_3 = struct.unpack_from(">HHH", frame, G_NO_OFFSET + 2)

            if g_no_1 == 2 and g_no_2 == 0 and g_no_3 == 0:
                characters = frame[MY_CHAR_OFFSET:MY_CHAR_OFFSET + 2]
                supers = frame[SUPER_ARTS_OFFSET:SUPER_ARTS_OFFSET + 2]
                new_challenger = frame[NEW_CHALLENGER_OFFSET]
                colors = frame[PLAYER_COLOR_OFFSET:PLAYER_COLOR_OFFSET + 2]
                (random_ix16,) = struct.unpack_from(">h", frame, RANDOM_IX_16_OFFSET)
                (random_ix32,) = struct.unpack_from(">h", frame, RANDOM_IX_32_OFFSET)

                setup = SetupBlock(
                    characters=(char_arcade_to_3sx(characters[0]), char_arcade_to_3sx(characters[1])),
                    supers=(supers[0], supers[1]),
                    colors=(colors[0], colors[1]),
                    new_challenger=new_challenger,
                    random_ix16=random_ix16 & 0xFFFF,
                    random_ix32=random_ix32 & 0xFFFF,
                )
                game_start_index = i

            continue

        # i > game_start_index: part of the in-game input/checksum stream.
        frame = accum_int.to_bytes(RAM_FRAME_SIZE, "big")
        (p1,) = struct.unpack_from(">H", frame, P1SW_0_OFFSET)
        (p2,) = struct.unpack_from(">H", frame, P2SW_0_OFFSET)
        p1_words.append(p1)
        p2_words.append(p2)

        local_index = len(p1_words) - 1
        if checksum_interval and local_index % checksum_interval == 0:
            checksum_table.append((local_index, compute_checksum(frame)))

    if game_start_index is None or setup is None:
        raise ExtractError(
            f"{path}: no game-start signature frame found "
            f"(G_No[1..3]==2,0,0 never matched across {table.frame_count} frames)"
        )

    return ScrdGameData(
        setup=setup,
        p1_words=p1_words,
        p2_words=p2_words,
        checksum_table=checksum_table,
        game_start_frame=game_start_index,
        archive_entry_count=table.frame_count,
    )


# ================================ encode ===================================


def encode_3sr(game: ScrdGameData, checksum_interval: int) -> bytes:
    setup = game.setup
    frame_count = len(game.p1_words)
    assert len(game.p2_words) == frame_count
    checksum_count = len(game.checksum_table)

    header = _HEADER_STRUCT.pack(
        MAGIC,
        VERSION,
        HEADER_SIZE,
        setup.characters[0],
        setup.characters[1],
        setup.supers[0],
        setup.supers[1],
        setup.colors[0],
        setup.colors[1],
        setup.new_challenger,
        0,  # pad
        setup.random_ix16,
        setup.random_ix32,
        frame_count,
        checksum_interval,
        checksum_count,
    )

    input_words = bytearray(frame_count * _INPUT_WORD_STRUCT.size)
    for i in range(frame_count):
        _INPUT_WORD_STRUCT.pack_into(input_words, i * _INPUT_WORD_STRUCT.size, game.p1_words[i], game.p2_words[i])

    checksums = bytearray(checksum_count * _CHECKSUM_ENTRY_STRUCT.size)
    for i, (frame_idx, checksum) in enumerate(game.checksum_table):
        _CHECKSUM_ENTRY_STRUCT.pack_into(checksums, i * _CHECKSUM_ENTRY_STRUCT.size, frame_idx, checksum)

    return header + bytes(input_words) + bytes(checksums)


# ================================ decode ===================================


@dataclass
class Parsed3sr:
    version: int
    header_size: int
    setup: SetupBlock
    frame_count: int
    checksum_interval: int
    checksum_count: int
    p1_words: list[int]
    p2_words: list[int]
    checksum_table: list[tuple[int, int]]
    raw: bytes = field(repr=False)


def parse_3sr(data: bytes) -> Parsed3sr:
    if len(data) < 6:
        raise FormatError(f".3sr file too short to contain even magic+version+header_size ({len(data)} bytes)")

    magic = data[0:4]
    if magic != MAGIC:
        raise FormatError(f"bad magic: expected {MAGIC!r}, got {magic!r}")

    (version, header_size) = struct.unpack_from("<HH", data, 4)
    if version != VERSION:
        raise FormatError(f"unsupported version {version} (this tool only knows version {VERSION})")
    if header_size != HEADER_SIZE:
        raise FormatError(f"unexpected header_size {header_size} for version {version} (expected {HEADER_SIZE})")
    if len(data) < header_size:
        raise FormatError(f".3sr file ({len(data)} bytes) shorter than its own header_size ({header_size})")

    (
        _magic2,
        _version2,
        _header_size2,
        char0,
        char1,
        sup0,
        sup1,
        col0,
        col1,
        new_challenger,
        pad,
        random_ix16,
        random_ix32,
        frame_count,
        checksum_interval,
        checksum_count,
    ) = _HEADER_STRUCT.unpack_from(data, 0)

    if pad != 0:
        raise FormatError(f"reserved pad byte at offset 0x0F is {pad}, expected 0")
    if checksum_interval == 0 and checksum_count != 0:
        raise FormatError(f"checksum_interval==0 but checksum_count=={checksum_count} (must be 0)")

    expected_size = header_size + frame_count * _INPUT_WORD_STRUCT.size + checksum_count * _CHECKSUM_ENTRY_STRUCT.size
    if len(data) != expected_size:
        raise FormatError(
            f"file size {len(data)} != header_size({header_size}) + "
            f"frame_count*4({frame_count * 4}) + checksum_count*8({checksum_count * 8}) "
            f"= {expected_size}"
        )

    p1_words: list[int] = [0] * frame_count
    p2_words: list[int] = [0] * frame_count
    base = header_size
    for i in range(frame_count):
        p1, p2 = _INPUT_WORD_STRUCT.unpack_from(data, base + i * _INPUT_WORD_STRUCT.size)
        p1_words[i] = p1
        p2_words[i] = p2

    checksum_table: list[tuple[int, int]] = []
    base = header_size + frame_count * _INPUT_WORD_STRUCT.size
    for i in range(checksum_count):
        frame_idx, checksum = _CHECKSUM_ENTRY_STRUCT.unpack_from(data, base + i * _CHECKSUM_ENTRY_STRUCT.size)
        if frame_idx >= frame_count:
            raise FormatError(f"checksum table entry {i} references frame {frame_idx} >= frame_count {frame_count}")
        checksum_table.append((frame_idx, checksum))

    for i in range(1, len(checksum_table)):
        if checksum_table[i][0] <= checksum_table[i - 1][0]:
            raise FormatError(
                f"checksum table is not strictly increasing in frame index at entry {i}: "
                f"{checksum_table[i - 1][0]} -> {checksum_table[i][0]}"
            )

    setup = SetupBlock(
        characters=(char0, char1),
        supers=(sup0, sup1),
        colors=(col0, col1),
        new_challenger=new_challenger,
        random_ix16=random_ix16,
        random_ix32=random_ix32,
    )

    return Parsed3sr(
        version=version,
        header_size=header_size,
        setup=setup,
        frame_count=frame_count,
        checksum_interval=checksum_interval,
        checksum_count=checksum_count,
        p1_words=p1_words,
        p2_words=p2_words,
        checksum_table=checksum_table,
        raw=data,
    )


def _sanity_check_ranges(parsed: Parsed3sr) -> list[str]:
    """Plausibility checks beyond structural parsing -- warnings, not hard
    errors (a future ROM revision or edge case could legitimately differ),
    but worth surfacing during `verify`."""
    warnings: list[str] = []

    for i, c in enumerate(parsed.setup.characters):
        if not (0 <= c < 20):
            warnings.append(f"characters[{i}]={c} out of expected 3SX range [0,19]")

    for i, s in enumerate(parsed.setup.supers):
        if not (0 <= s <= 2):
            warnings.append(f"supers[{i}]={s} out of expected Super_Arts range [0,2]")

    for i, c in enumerate(parsed.setup.colors):
        if not (0 <= c <= 12):
            warnings.append(f"colors[{i}]={c} out of expected Player_Color range [0,12]")

    if parsed.setup.new_challenger not in (0, 1):
        warnings.append(f"new_challenger={parsed.setup.new_challenger} is not 0 or 1")

    if parsed.checksum_interval and parsed.checksum_count:
        expected_count = (parsed.frame_count + parsed.checksum_interval - 1) // parsed.checksum_interval
        if parsed.checksum_count != expected_count:
            warnings.append(
                f"checksum_count={parsed.checksum_count} does not match the expected sampling "
                f"count ceil(frame_count/checksum_interval)={expected_count}"
            )
        for i, (frame_idx, _checksum) in enumerate(parsed.checksum_table):
            if frame_idx != i * parsed.checksum_interval:
                warnings.append(
                    f"checksum table entry {i} covers frame {frame_idx}, expected "
                    f"{i * parsed.checksum_interval} (i*checksum_interval)"
                )

    return warnings


# ================================ meta.json =================================

# CRITICAL DATA-FIX NOTE (docs/plan-osd-replay-browser.md Stage S1): the
# `summary.json` and "minimal" branches below deliberately write
# `players: []` -- that is a known, intentional data GAP in this function's
# fallback paths, not a target to "fix" here. A `.3sr` produced with an empty
# `players[]` sidecar is exactly what upstream calls "NEEDS CONVERSION" limbo:
# ReplayPlayer_Init's meta-sidecar parser (src/replay/replay_player.c:346-352)
# and its accessors (:651-656) read player NAMES from `players[].name` only --
# there is nowhere else a name can come from once the `.3sr` is on-device. Any
# caller that cares about names in the OSD rows or the in-replay name overlay
# (i.e. every real publish path) MUST supply `--quark-json` built from a real
# catalog/quark row (option (a) below) so the FIRST branch (quark.json) is the
# one that actually runs -- never let a batch fall through to the
# summary/minimal branches and call the result "published". The Mac-side
# batch converter (tools/fcade-replays/publish_3sr.py) enforces this by always
# writing a per-quark `quark.json` (verbatim catalog row: quarkid/players/
# date/duration) and passing `--quark-json` on every `generate` call it makes.


def build_meta(
    scrd_path: Path,
    quark_json_path: Optional[Path],
    summary_json_path: Optional[Path],
) -> dict:
    m = SCRD_FILENAME_RE.match(scrd_path.name)
    game_index = int(m.group("game")) if m else None
    quark_from_name = normalize_quark(m.group("quark")) if m else scrd_path.stem

    if quark_json_path is not None and quark_json_path.is_file():
        quark = json.loads(quark_json_path.read_text())
        return {
            "quarkid": quark.get("quarkid", quark_from_name),
            "players": quark.get("players", []),
            "date": quark.get("date"),
            "duration": quark.get("duration"),
            "game_index": game_index,
            "source": "quark.json",
        }

    if summary_json_path is not None and summary_json_path.is_file():
        summary = json.loads(summary_json_path.read_text())
        date_ms = None
        if "downloaded_at_unix" in summary:
            date_ms = int(summary["downloaded_at_unix"]) * 1000
        quarkid = quark_from_name
        game = summary.get("game")
        token = summary.get("token")
        if game and token:
            quarkid = f"{game}-{token}"
        return {
            "quarkid": quarkid,
            "players": [],
            "date": date_ms,
            "duration": None,
            "game_index": game_index,
            "source": "summary.json (players/duration unavailable -- schema has no such fields)",
        }

    return {
        "quarkid": quark_from_name,
        "players": [],
        "date": None,
        "duration": None,
        "game_index": game_index,
        "source": "minimal (no quark.json or summary.json found)",
    }


# ================================== CLI =====================================


def cmd_generate(args: argparse.Namespace) -> int:
    scrd_path: Path = args.archive
    out_path: Path = args.out

    try:
        game = extract_scrd_game(scrd_path, checksum_interval=args.checksum_interval)
    except CorruptArchiveError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except ExtractError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    payload = encode_3sr(game, checksum_interval=args.checksum_interval)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(payload)

    frame_count = len(game.p1_words)
    print(
        f"{scrd_path.name}: game-start at archive frame {game.game_start_frame} of "
        f"{game.archive_entry_count}; wrote {out_path} "
        f"(frame_count={frame_count}, checksum_count={len(game.checksum_table)}, "
        f"{len(payload)} bytes)",
        file=sys.stderr,
    )

    meta_out = args.meta_out
    if meta_out is None:
        meta_out = out_path.with_suffix(".meta.json")

    meta = build_meta(scrd_path, args.quark_json, args.summary_json)
    meta_out.parent.mkdir(parents=True, exist_ok=True)
    meta_out.write_text(json.dumps(meta, indent=2) + "\n")
    print(f"  wrote {meta_out} ({meta['source']})", file=sys.stderr)

    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    path: Path = args.replay
    data = path.read_bytes()

    try:
        parsed = parse_3sr(data)
    except FormatError as exc:
        print(f"FAIL: {path}: {exc}", file=sys.stderr)
        return 1

    warnings = _sanity_check_ranges(parsed)
    for w in warnings:
        print(f"  warning: {w}", file=sys.stderr)

    # Round-trip: re-serialize the parsed structure and diff against the
    # original bytes. This is the strongest structural check available
    # without the source SCRD archive (the .3sr alone cannot recompute its
    # own RAM checksums -- they were sampled from archive frames that no
    # longer exist in this file).
    game_for_roundtrip = ScrdGameData(
        setup=parsed.setup,
        p1_words=parsed.p1_words,
        p2_words=parsed.p2_words,
        checksum_table=parsed.checksum_table,
        game_start_frame=-1,  # not stored in the format, not needed for round-trip
        archive_entry_count=-1,
    )
    reencoded = encode_3sr(game_for_roundtrip, checksum_interval=parsed.checksum_interval)
    if reencoded != data:
        print(
            f"FAIL: {path}: round-trip re-serialization does not match the original file "
            f"byte-for-byte ({len(reencoded)} vs {len(data)} bytes)",
            file=sys.stderr,
        )
        return 1

    print(
        f"PASS (structural): {path}: version={parsed.version} frame_count={parsed.frame_count} "
        f"checksum_interval={parsed.checksum_interval} checksum_count={parsed.checksum_count} "
        f"characters={parsed.setup.characters} supers={parsed.setup.supers} "
        f"colors={parsed.setup.colors} new_challenger={parsed.setup.new_challenger} "
        f"random_ix16={parsed.setup.random_ix16} random_ix32={parsed.setup.random_ix32} "
        f"round-trip=OK",
        file=sys.stderr,
    )

    if args.against_scrd is None:
        return 0

    try:
        rederived = extract_scrd_game(args.against_scrd, checksum_interval=parsed.checksum_interval)
    except (CorruptArchiveError, ExtractError) as exc:
        print(f"FAIL: --against-scrd re-derivation failed: {exc}", file=sys.stderr)
        return 1

    mismatches: list[str] = []

    if rederived.setup != parsed.setup:
        mismatches.append(f"setup block differs: .3sr={parsed.setup} scrd-rederived={rederived.setup}")

    if len(rederived.p1_words) != parsed.frame_count:
        mismatches.append(
            f"frame_count differs: .3sr={parsed.frame_count} scrd-rederived={len(rederived.p1_words)}"
        )
    else:
        for i in range(parsed.frame_count):
            if rederived.p1_words[i] != parsed.p1_words[i] or rederived.p2_words[i] != parsed.p2_words[i]:
                mismatches.append(
                    f"input word mismatch at frame {i}: .3sr=(0x{parsed.p1_words[i]:04x},"
                    f"0x{parsed.p2_words[i]:04x}) scrd-rederived=(0x{rederived.p1_words[i]:04x},"
                    f"0x{rederived.p2_words[i]:04x})"
                )
                if len(mismatches) > 20:
                    mismatches.append("  ... (further input mismatches suppressed)")
                    break

    if rederived.checksum_table != parsed.checksum_table:
        mismatches.append(
            f"checksum table differs: .3sr has {len(parsed.checksum_table)} entries, "
            f"scrd-rederived has {len(rederived.checksum_table)} entries"
        )

    if mismatches:
        print(f"FAIL: --against-scrd full re-derivation mismatch for {path}:", file=sys.stderr)
        for m in mismatches[:25]:
            print(f"  {m}", file=sys.stderr)
        return 1

    print(f"PASS (against-scrd): {path} matches a full re-derivation from {args.against_scrd}", file=sys.stderr)
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p_gen = sub.add_parser("generate", help="SCRD archive -> .3sr + .meta.json sidecar")
    p_gen.add_argument("archive", type=Path, help="input .scrd archive")
    p_gen.add_argument("--out", type=Path, required=True, help="output .3sr path")
    p_gen.add_argument("--meta-out", type=Path, default=None, help="output .meta.json path (default: <out>.meta.json)")
    p_gen.add_argument("--quark-json", type=Path, default=None, help="optional quark.json for the sidecar")
    p_gen.add_argument("--summary-json", type=Path, default=None, help="optional summary.json fallback for the sidecar")
    p_gen.add_argument("--checksum-interval", type=int, default=DEFAULT_CHECKSUM_INTERVAL,
                        help=f"sample a checksum every N in-game frames, 0 to disable (default {DEFAULT_CHECKSUM_INTERVAL})")
    p_gen.set_defaults(func=cmd_generate)

    p_ver = sub.add_parser("verify", help="structurally validate a .3sr (optionally against its source .scrd)")
    p_ver.add_argument("replay", type=Path, help="input .3sr file")
    p_ver.add_argument("--against-scrd", type=Path, default=None,
                        help="re-derive from this .scrd archive and compare full contents")
    p_ver.set_defaults(func=cmd_verify)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
