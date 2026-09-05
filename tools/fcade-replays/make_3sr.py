#!/usr/bin/env python3
"""SCRD -> per-game `.3sr` converter (plan-fcade-replay-browser.md Step B2).

Format spec: docs/3sr-format.md (read that file first -- this module is a
straight implementation of it, byte offset for byte offset).

PROVENANCE:
  - Match-start detection, its TWO REJECTIONS, and the setup-block field
    reads mirror src/test/scrd_game.c's ScrdGame_Init (armed-then-confirmed
    G_No signature, the wu_operator pair, MY_CHAR/SUPER_ARTS/
    NEW_CHALLENGER/PLAYER_COLOR offsets, CHAR_ARCADE_TO_3SX). Sharing the
    oracle's predicates is the point: a segment statcheck cannot grade is a
    segment the device viewer cannot play, so this tool must not emit a .3sr
    for one. `generate` exits 2 for a matchless segment and 3 for a
    CPU-recorded one -- the same codes src/main.c gives statcheck -- and
    writes no output file in either case.
  - RNG sync source (Random_ix16/32 from the game-start frame, i.e. frame
    `start_index - 1` in scrd_game.c's own indexing) mirrors
    src/test/statcheck_runner.c's PHASE_GAME_TRANSITION.
  - players_timer (v2 headers only) comes from that SAME frame, and for the
    same reason: it is the third value src/test/statcheck_compare.c ->
    Statcheck_SyncValues seeds there. Without it every effect_G9_init()
    spawn (two random_16() draws apiece) lands on the wrong frame -- see
    src/sf33rd/Source/Game/effect/effg6.c -> effect_G6_move, whose gate reads
    `now_koc & (players_timer + blink_timing)`.
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

Exit codes (`generate`): 0 converted, 1 failure, 2 segment holds no match
(H1), 3 recorded against the CPU (H4b). 2 and 3 mean SKIP, not fail.

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
from dataclasses import dataclass, field, replace
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
# players_timer (CPS3 0x020157CE) -- src/arcade/arcade_constants.h
# PLAYERS_TIMER_OFFSET. Established by disassembly, not by a value scan: see
# src/test/statcheck_compare.c -> Statcheck_SyncValues for the four-referrer
# argument (the effect_G6_move gate plus the three `+1; & 0x7FFF` increment
# sites matching plcnt.c / plcnt2.c / plcnt3.c).
PLAYERS_TIMER_OFFSET = 0x157CE
WORK_XYZ_OFFSET = 0x64
PLW_OFFSET = 0x68C6C
PLW_SIZE = 0x498
# `work.wu.wu_operator` -- src/arcade/arcade_constants.h WORK_WU_OPERATOR_OFFSET.
# The two players' copies sit at PLW_OFFSET + 3 = 0x68C6F and
# PLW_OFFSET + PLW_SIZE + 3 = 0x69107. Non-zero == a human operator drove that
# side; zero == `cpu_algorithm()` did. See `find_match_start` below.
WORK_WU_OPERATOR_OFFSET = 3
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

# The magic is the FAMILY marker and does NOT change across versions: the
# `version` u16 at 0x04 is what discriminates. Changing the magic instead
# would make every magic-gated consumer reject v2 outright -- notably
# fcade-proxy.js `read3srGameData` ("game_N.3sr has a bad magic (not '3SR1')"),
# which would stop the VPS serving newly converted replays until it was
# redeployed. See docs/3sr-format.md section 1.1.
MAGIC = b"3SR1"
VERSION_V1 = 1
VERSION_V2 = 2
HEADER_SIZE_V1 = 28
HEADER_SIZE_V2 = 32
# Header size by version -- the ONLY place the mapping is written down here,
# and the set of versions parse_3sr accepts. `generate` always writes v2
# (encode_3sr picks the version from the data); v1 stays readable forever.
HEADER_SIZE_BY_VERSION = {VERSION_V1: HEADER_SIZE_V1, VERSION_V2: HEADER_SIZE_V2}
DEFAULT_CHECKSUM_INTERVAL = 60

# v1 header, byte-for-byte; a v2 header is this followed by _HEADER_V2_EXT.
_HEADER_STRUCT = struct.Struct("<4sHHBBBBBBBBHHIHH")
# v2 extension at 0x1C: players_timer u16, then a reserved u16 that MUST be 0.
# The reserved pair is not padding-for-padding's-sake: it keeps header_size a
# multiple of 4, so the {u32 frame, u32 djb2} checksum table stays 4-aligned
# relative to the file start exactly as it was in v1.
_HEADER_V2_EXT = struct.Struct("<HH")
_INPUT_WORD_STRUCT = struct.Struct("<HH")
_CHECKSUM_ENTRY_STRUCT = struct.Struct("<II")

assert _HEADER_STRUCT.size == HEADER_SIZE_V1, _HEADER_STRUCT.size
assert _HEADER_STRUCT.size + _HEADER_V2_EXT.size == HEADER_SIZE_V2

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


class NoMatchStartError(ExtractError):
    """No match ever started in this segment (H1,
    docs/research-arcade-balance-desyncs.md). The `G_No[1..3] == (2, 0, 0)`
    triple says only that the Game task is parked on the `Game2_0` slot; a
    segment cut right after a final KO can carry it frozen for its whole
    length. `cmd_generate` turns this into exit code 2, mirroring `src/main.c`'s
    mapping of `SCRD_GAME_INIT_NO_MATCH_START`."""


class CpuPlayerError(ExtractError):
    """The cabinet ran this game against the CPU (H4b,
    docs/research-arcade-balance-desyncs.md) -- `plw[ix].wu.wu_operator` was 0
    on at least one side at the match-start frame, i.e. `Play_Type == 0` with
    `cpu_algorithm()` driving that side.

    The device viewer cannot reproduce such a recording: `ReplayPlayer_Tick`
    (`src/replay/replay_player.c`, `PHASE_CHARACTER_SELECT` case 0) taps
    `SWK_START` for player 2 exactly as the statcheck harness does, so both
    sides always come up with `wu_operator != 0`, and `Player_move()`
    (`src/sf33rd/Source/Game/engine/plmain.c`) then feeds them the replay's stored
    button words -- whereas the recording's CPU side ignored those words and
    ran the AI. `cmd_generate` turns this into exit code 3, mirroring
    `src/main.c`'s mapping of `SCRD_GAME_INIT_CPU_PLAYER`."""


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
    # v2 only. `None` means "this file is v1 and does not carry the field" --
    # NOT "the value was zero". A consumer must leave its own players_timer
    # alone when this is None; writing 0 would be a different behaviour from
    # what every v1 file has always produced.
    players_timer: Optional[int] = None


@dataclass
class ScrdGameData:
    setup: SetupBlock
    p1_words: list[int]
    p2_words: list[int]
    checksum_table: list[tuple[int, int]]  # (frame_index, djb2)
    game_start_frame: int  # SCRD archive frame index S (0-based)
    archive_entry_count: int


@dataclass
class MatchStart:
    """Where -- and whether -- a real match starts in one SCRD segment.

    `signature_index` is the archive frame `S` carrying the `(2, 0, 0)` G_No
    triple (the setup block is read there); `start_index` is `S + 1`, the frame
    on which `Game2_0()`'s own writes are visible and the input stream begins.
    See `find_match_start`."""

    setup: SetupBlock
    signature_index: int
    start_index: int
    start_frame: bytes
    wu_operator: tuple[int, int]


def _read_archive_table(path: Path):
    """Opens a SCRD archive and returns its raw bytes plus decoded frame table,
    raising CorruptArchiveError for the two unusable shapes."""
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

    return data, table


def _iter_archive_frames(data: bytes, table):
    """Lazily yields `(archive_frame_index, full RAM frame bytes)`.

    Lazy on purpose: `probe_match_start` abandons the generator the moment the
    match start is found, so an eligibility check on a normal segment decodes
    two frames rather than the whole archive."""
    accum_int = 0
    for i, (off, size) in enumerate(table.entries):
        compressed = data[off:off + size]
        payload = zero_run_decode(compressed, RAM_FRAME_SIZE)
        payload_int = int.from_bytes(payload, "big")
        accum_int = payload_int if i == 0 else (accum_int ^ payload_int)
        yield i, accum_int.to_bytes(RAM_FRAME_SIZE, "big")


def _read_setup_block(frame: bytes) -> SetupBlock:
    """The setup block, read from the signature frame `S` -- field for field
    what `scrd_read_match_setup` (`src/test/scrd_game.c`) reads, plus the RNG
    pair and players_timer that `Statcheck_SyncValues`
    (`src/test/statcheck_compare.c`) is handed from that same frame."""
    characters = frame[MY_CHAR_OFFSET:MY_CHAR_OFFSET + 2]
    supers = frame[SUPER_ARTS_OFFSET:SUPER_ARTS_OFFSET + 2]
    new_challenger = frame[NEW_CHALLENGER_OFFSET]
    colors = frame[PLAYER_COLOR_OFFSET:PLAYER_COLOR_OFFSET + 2]
    (random_ix16,) = struct.unpack_from(">h", frame, RANDOM_IX_16_OFFSET)
    (random_ix32,) = struct.unpack_from(">h", frame, RANDOM_IX_32_OFFSET)
    # Same frame S as the RNG pair, on purpose: this is the frame
    # src/test/statcheck_compare.c -> Statcheck_SyncValues is handed
    # (statcheck_runner.c PHASE_GAME_TRANSITION passes `comparison_index - 1`),
    # and seeding players_timer once from it is what took that harness's G9
    # drift to zero.
    (players_timer,) = struct.unpack_from(">H", frame, PLAYERS_TIMER_OFFSET)

    return SetupBlock(
        characters=(char_arcade_to_3sx(characters[0]), char_arcade_to_3sx(characters[1])),
        supers=(supers[0], supers[1]),
        colors=(colors[0], colors[1]),
        new_challenger=new_challenger,
        random_ix16=random_ix16 & 0xFFFF,
        random_ix32=random_ix32 & 0xFFFF,
        players_timer=players_timer,
    )


def find_match_start(path: Path, frames) -> MatchStart:
    """The producer half of `ScrdGame_Init` (`src/test/scrd_game.c`) -- the same
    two predicates, so this tool converts exactly the segments the oracle can
    grade and the device viewer can play.

    1. ARMED, then CONFIRMED (H1). `G_No[1..3] == (2, 0, 0)` says only "the Game
       task is parked on the Game2_0 slot"; a segment cut right after a final KO
       can carry that triple frozen for its whole length. The signature of a
       match that actually started is the visible effect of `Game2_0()`
       (`game.c`) having run -- it writes `Game_timer = 0; C_No[0..3] = 0;
       G_No[2] = 3;` in one frame -- so the frame AFTER the triple must show
       `Game_timer == 0 && G_No[2] == 3`. Otherwise: NoMatchStartError.

    2. TWO HUMAN OPERATORS (H4b). `plw[0].wu.wu_operator` and
       `plw[1].wu.wu_operator`, read at the confirmed start frame, must both be
       non-zero. A zero means `cpu_algorithm()` drove that side and the stored
       button words were never what moved the character -- see CpuPlayerError.

    `frames` is a `_iter_archive_frames` generator; on success it is left
    positioned just past the returned `start_frame`, so a caller can go straight
    on to reading the input stream."""
    armed_setup: Optional[SetupBlock] = None
    armed_index = -1
    frame_count = 0

    for i, frame in frames:
        frame_count = i + 1

        if armed_setup is not None:
            (game_timer,) = struct.unpack_from(">H", frame, GAME_TIMER_OFFSET)
            (g_no_2_after,) = struct.unpack_from(">H", frame, G_NO_OFFSET + 4)

            if game_timer == 0 and g_no_2_after == 3:
                operators = (
                    frame[PLW_OFFSET + WORK_WU_OPERATOR_OFFSET],
                    frame[PLW_OFFSET + PLW_SIZE + WORK_WU_OPERATOR_OFFSET],
                )
                if operators[0] == 0 or operators[1] == 0:
                    raise CpuPlayerError(
                        f"{path}: recorded against the CPU -- wu_operator = "
                        f"{operators} at the match-start frame (archive frame {i}), so the "
                        f"cabinet ran Play_Type == 0 with cpu_algorithm() on at least one "
                        f"side; the device viewer forces two operators and cannot reproduce "
                        f"it (H4b)"
                    )
                return MatchStart(
                    setup=armed_setup,
                    signature_index=armed_index,
                    start_index=i,
                    start_frame=frame,
                    wu_operator=operators,
                )

        g_no_1, g_no_2, g_no_3 = struct.unpack_from(">HHH", frame, G_NO_OFFSET + 2)
        if g_no_1 == 2 and g_no_2 == 0 and g_no_3 == 0:
            armed_setup = _read_setup_block(frame)
            armed_index = i
        else:
            armed_setup = None
            armed_index = -1

    raise NoMatchStartError(
        f"{path}: no match start across {frame_count} frames -- the (2,0,0) G_No "
        f"triple is never followed by Game2_0()'s Game_timer=0 / G_No[2]=3 "
        f"(segment holds no match) (H1)"
    )


def probe_match_start(path: Path) -> MatchStart:
    """Eligibility probe: does this segment hold a match the device viewer can
    play at all? Raises NoMatchStartError / CpuPlayerError / CorruptArchiveError
    exactly as `extract_scrd_game` would, but stops decoding at the start frame
    on a normal segment. Used by publish_3sr.py to skip -- rather than convert
    and then drop -- an ineligible segment."""
    data, table = _read_archive_table(path)
    return find_match_start(path, _iter_archive_frames(data, table))


def extract_scrd_game(path: Path, checksum_interval: int = DEFAULT_CHECKSUM_INTERVAL) -> ScrdGameData:
    """Mirrors src/test/scrd_game.c's ScrdGame_Init (match-start scan AND its
    two rejections -- see `find_match_start`) plus the per-frame
    P1SW_0/P2SW_0 + checksum-window extraction described in
    docs/3sr-format.md. Single forward pass over the SCRD archive, reusing
    decode_inputs.py's table/decompress primitives."""
    data, table = _read_archive_table(path)
    frames = _iter_archive_frames(data, table)
    start = find_match_start(path, frames)

    p1_words: list[int] = []
    p2_words: list[int] = []
    checksum_table: list[tuple[int, int]] = []

    def append_in_game_frame(frame: bytes) -> None:
        (p1,) = struct.unpack_from(">H", frame, P1SW_0_OFFSET)
        (p2,) = struct.unpack_from(">H", frame, P2SW_0_OFFSET)
        p1_words.append(p1)
        p2_words.append(p2)

        local_index = len(p1_words) - 1
        if checksum_interval and local_index % checksum_interval == 0:
            checksum_table.append((local_index, compute_checksum(frame)))

    # The confirmed start frame (S + 1) is itself the first in-game frame; the
    # signature frame S contributes the setup block and no input word.
    append_in_game_frame(start.start_frame)
    for _i, frame in frames:
        append_in_game_frame(frame)

    return ScrdGameData(
        setup=start.setup,
        p1_words=p1_words,
        p2_words=p2_words,
        checksum_table=checksum_table,
        game_start_frame=start.signature_index,
        archive_entry_count=table.frame_count,
    )


# ================================ encode ===================================


def encode_3sr(game: ScrdGameData, checksum_interval: int) -> bytes:
    """Version is chosen by the DATA, not by a flag: a setup block carrying a
    players_timer encodes as v2, one without it (i.e. re-encoding a parsed v1
    file) encodes as v1. That is what keeps `verify`'s byte-for-byte
    round-trip meaningful for the ~22k v1 files already in the wild."""
    setup = game.setup
    frame_count = len(game.p1_words)
    assert len(game.p2_words) == frame_count
    checksum_count = len(game.checksum_table)

    version = VERSION_V2 if setup.players_timer is not None else VERSION_V1
    header_size = HEADER_SIZE_BY_VERSION[version]

    header = _HEADER_STRUCT.pack(
        MAGIC,
        version,
        header_size,
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

    if version == VERSION_V2:
        header += _HEADER_V2_EXT.pack(setup.players_timer, 0)

    assert len(header) == header_size, (len(header), header_size)

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
    if version not in HEADER_SIZE_BY_VERSION:
        known = ", ".join(str(v) for v in sorted(HEADER_SIZE_BY_VERSION))
        raise FormatError(f"unsupported version {version} (this tool knows versions {known})")
    expected_header_size = HEADER_SIZE_BY_VERSION[version]
    if header_size != expected_header_size:
        raise FormatError(
            f"unexpected header_size {header_size} for version {version} (expected {expected_header_size})"
        )
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

    players_timer: Optional[int] = None
    if version >= VERSION_V2:
        (players_timer, reserved) = _HEADER_V2_EXT.unpack_from(data, HEADER_SIZE_V1)
        if reserved != 0:
            raise FormatError(f"reserved u16 at offset 0x1E is {reserved}, expected 0")
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
        players_timer=players_timer,
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

    # Exit codes deliberately match `src/main.c`'s statcheck mapping so a caller
    # can use one table for the oracle and the producer: 0 = converted,
    # 1 = failure, 2 = segment holds no match, 3 = recorded against the CPU.
    # 2 and 3 are NOT failures -- they are "this segment is not convertible",
    # and a caller must skip the segment, never publish a .3sr for it.
    try:
        game = extract_scrd_game(scrd_path, checksum_interval=args.checksum_interval)
    except NoMatchStartError as exc:
        print(f"skip: {exc}", file=sys.stderr)
        return 2
    except CpuPlayerError as exc:
        print(f"skip: {exc}", file=sys.stderr)
        return 3
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
        f"players_timer={'absent (v1)' if parsed.setup.players_timer is None else parsed.setup.players_timer} "
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

    # A re-derivation always carries players_timer (the writer is v2), so a v1
    # file under test can never match on that field. Compare it out rather than
    # reporting every pre-v2 file as corrupt.
    rederived_setup = rederived.setup
    if parsed.setup.players_timer is None:
        rederived_setup = replace(rederived_setup, players_timer=None)

    if rederived_setup != parsed.setup:
        mismatches.append(f"setup block differs: .3sr={parsed.setup} scrd-rederived={rederived_setup}")

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
