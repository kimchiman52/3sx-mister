#!/usr/bin/env python3
"""Decode fbneo-replay-runner `-13` input records into arcade-layout words,
and (optionally) cross-check the decoded stream against SCRD-archived RAM
dumps for frame alignment.

STAGE / PROVENANCE (plan-fcade-replay-browser.md, Step B1, §2.2/§2.3):

  fbneo-replay-runner's `ReplayApplyFrameInputs` (run.cpp:379-403) consumes
  the `inputs` blob 10 bytes per frame:
      p1_mask = frame[0] | frame[1] << 8   (:392)
      p2_mask = frame[5] | frame[6] << 8   (:393-394)
  and asserts `size % 10 == 0` (:428). `docs/fcade-replay-notes.md` §5.1
  confirms `record_size == 10`, `record_count == 60` for every `-13` message
  observed across all 4 A2 samples, so the flat `inputs` file is simply the
  concatenation of those 10-byte-per-frame records in session order,
  including non-gameplay frames (menus, character select, results) per
  notes §5.3 -- there is no in-game gate on the input stream itself.

  `ReplayBitFromInfo` (run.cpp:328-345) maps each mask's bits to buttons:
  bit 1 = start, bit 2 = up, bit 3 = down, bit 4 = left, bit 5 = right,
  bits 6-11 = fire1..fire6 (LP,MP,HP,LK,MK,HK). Coin is never mapped; bytes
  2-4 and 7-9 of each 10-byte record are ignored (unused by the runner).

  This script converts each fcade mask into a CPS3 **arcade-RAM-layout**
  word (bits 0-3 = up/down/left/right, 4-6 = LP/MP/HP, 7-9 = LK/MK/HK,
  12 = start) -- i.e. the same layout the fork's DEBUG `read_input_buff`
  produces from a raw `P1SW_0`/`P2SW_0` register read (src/test/replay_game.c
  :12-26) and the layout SCRD frames archive at those offsets.

  *** IMPORTANT: this script's decode-mode output is ARCADE-RAM layout. ***
  *** It is NOT the engine's SWK layout (include/sf33rd/AcrSDK/common/    ***
  *** pad.h:6-23) and must NOT be injected into p1sw_buff/p2sw_buff/     ***
  *** p1sw_0 as-is: kicks land on bits 7-9 and start on bit 12, which    ***
  *** the engine reads as SWK_LEFT_SHOULDER/SWK_LEFT_STICK -- silent    ***
  *** input corruption. Any engine-side consumer must additionally      ***
  *** apply the arcade->SWK shift-convert (LK 7->8, MK 8->9, HK 9->10,  ***
  *** start 12->14) described in plan §2.3 and implemented identically  ***
  *** in src/test/replay_game.c:12-26 / upstream test_runner.c:82-128.  ***

  Cross-check mode reads SCRD archives (as produced by
  tools/compress_ram_dumps.py) and extracts, per frame, the BIG-ENDIAN u16
  words at P1SW_0/P2SW_0 (arcade_constants.h:27-28, genuine arcade RAM
  layout per the plan's 2026-07-21 ERRATUM in §2.3 -- NOT the WCP
  `sw_lvbt` mirror, which is already engine-layout and a different archive
  offset entirely) and at P1SW_1/P2SW_1 (P1SW_0/P2SW_0 + 2 bytes each --
  plan §2.4, citing upstream's now-superseded `better-replay-parsing`
  branch `replay_match.c:88-91`: "we read previous inputs because CPS3
  updates input buffers at the end of a frame"). Endianness for these
  reads matches `src/test/scrd_game.c`'s `scrd_read_u16` (SDL_ReadU16BE)
  and `src/test/test_runner_utils.c`'s `read_u16` (also SDL_ReadU16BE) --
  every fixed-offset read out of a decompressed RAM frame in this codebase
  is big-endian, because `run.cpp:167-176` normalizes the raw SH-2 dump by
  reversing bytes within each 32-bit word before writing `frame_NNNNNNNN.ram`
  (and hence before `compress_ram_dumps.py` ever sees it), and
  `ReplayReadBigEndianU16` (run.cpp:132-139) is itself the runner's own
  in-game-state test against that normalized layout.

  The SCRD container format (magic/table/zero-run/XOR-delta) is mirrored
  from `tools/compress_ram_dumps.py`'s encoder, cross-checked against the
  fork's C decoder (`src/test/ram_archive.c`): magic b"SCRD", u16 LE frame
  count, then frame_count * (u32 LE offset, u32 LE size) table entries,
  then per-frame zero-run-encoded payloads. Frame 0's payload is the full
  524288-byte RAM frame (still zero-run encoded, just not XORed against a
  prior frame); every subsequent frame's payload is XORed against the
  previous *reconstructed* frame to recover the actual RAM contents
  (ram_archive.c's `RamArchive_SeekFrame` walks this the same way).

Usage:
  Decode only:
    decode_inputs.py <replay_dir> --csv out.csv --bin out.bin

  Decode + cross-check against one quark's SCRD archives:
    decode_inputs.py <replay_dir> --scrd-dir <dir-of-game_N.scrd> \\
        [--json-report out.json]

  <replay_dir> must contain an `inputs` file (raw concatenated -13 record
  bodies) and, optionally, a `summary.json` (used only for a framing sanity
  check -- record_size/record_count/total length cross against the
  `inputs` file's actual byte count).

  --scrd-dir may point at a directory containing archives for *multiple*
  quarks (e.g. the whole corpus) -- files are filtered to the ones whose
  quark prefix matches <replay_dir>'s basename, after normalizing away an
  optional trailing ".<digits>" fightcade session suffix (e.g. both
  "sfiii3nr1-1641508702494-7287" and "...-7287.7" name the same session;
  confirmed identical via md5 during this step's investigation).
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path
from typing import NamedTuple, Optional

# --- fbneo-replay-runner record framing (run.cpp:379-403, :428) ---------

RECORD_SIZE = 10
P1_MASK_BYTE_OFFSET = 0  # frame[0] | frame[1] << 8  (:392)
P2_MASK_BYTE_OFFSET = 5  # frame[5] | frame[6] << 8  (:393-394)

# fcade mask bit -> arcade-RAM word bit (run.cpp:328-345 x plan Step B1):
#   up 2->0, down 3->1, left 4->2, right 5->3, fire1..6 (bits 6..11) -> 4..9,
#   start 1->12. Coin (never mapped by the runner) and bytes 2-4/7-9 of each
#   record are intentionally not represented here.
FCADE_TO_ARCADE_BITS: tuple[tuple[int, int], ...] = (
    (2, 0),   # up
    (3, 1),   # down
    (4, 2),   # left
    (5, 3),   # right
    (6, 4),   # fire1 -> LP
    (7, 5),   # fire2 -> MP
    (8, 6),   # fire3 -> HP
    (9, 7),   # fire4 -> LK
    (10, 8),  # fire5 -> MK
    (11, 9),  # fire6 -> HK
    (1, 12),  # start
)

# --- CPS3 arcade RAM offsets (src/arcade/arcade_constants.h:27-28; the +2
# "SW_1" latched-previous offsets are documented in plan §2.4, citing
# upstream's better-replay-parsing branch replay_match.c:88-91, not present
# in arcade_constants.h itself since the fork's DEBUG/STATCHECK harnesses
# never read them -- only P1SW_0/P2SW_0). ---------------------------------

P1SW_0_OFFSET = 0x6AA8C
P2SW_0_OFFSET = 0x6AA90
P1SW_1_OFFSET = P1SW_0_OFFSET + 2  # 0x6AA8E
P2SW_1_OFFSET = P2SW_0_OFFSET + 2  # 0x6AA92

RAM_FRAME_SIZE = 524288  # docs/fcade-replay-notes.md §5.4; ram_archive.c RAM_FRAME_SIZE
SCRD_MAGIC = b"SCRD"
SCRD_HEADER_SIZE = 6
SCRD_TABLE_ENTRY_SIZE = 8


# ============================== decode mode ==============================


def fcade_mask_to_arcade_word(mask: int) -> int:
    word = 0
    for fcade_bit, arcade_bit in FCADE_TO_ARCADE_BITS:
        if mask & (1 << fcade_bit):
            word |= 1 << arcade_bit
    return word


def decode_inputs_bytes(data: bytes) -> tuple[list[int], list[int]]:
    """Decode a flat `inputs` file into parallel (p1_words, p2_words) lists,
    one entry per 10-byte record, in arcade-RAM layout."""
    if len(data) % RECORD_SIZE != 0:
        raise ValueError(
            f"inputs data length {len(data)} is not a multiple of "
            f"{RECORD_SIZE} (run.cpp:428 requires size % 10 == 0)"
        )

    frame_count = len(data) // RECORD_SIZE
    p1_words: list[int] = [0] * frame_count
    p2_words: list[int] = [0] * frame_count

    for i in range(frame_count):
        base = i * RECORD_SIZE
        p1_mask = data[base + P1_MASK_BYTE_OFFSET] | (data[base + P1_MASK_BYTE_OFFSET + 1] << 8)
        p2_mask = data[base + P2_MASK_BYTE_OFFSET] | (data[base + P2_MASK_BYTE_OFFSET + 1] << 8)
        p1_words[i] = fcade_mask_to_arcade_word(p1_mask)
        p2_words[i] = fcade_mask_to_arcade_word(p2_mask)

    return p1_words, p2_words


def sanity_check_summary(summary_path: Path, actual_inputs_len: int) -> list[str]:
    """Cross the record framing claimed by summary.json's -13 messages
    against the actual `inputs` file length. Returns a list of warning
    strings (empty if everything lines up); never raises -- this is a
    sanity check, not a hard requirement (summary.json is optional)."""
    warnings: list[str] = []

    if not summary_path.is_file():
        return warnings

    try:
        summary = json.loads(summary_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        warnings.append(f"could not parse {summary_path}: {exc}")
        return warnings

    messages = summary.get("messages", [])
    total_expected = 0
    bad_record_size = set()
    for msg in messages:
        if msg.get("type") != -13:
            continue
        record_size = msg.get("record_size")
        record_count = msg.get("record_count")
        body_len = msg.get("body_len")
        if record_size != RECORD_SIZE:
            bad_record_size.add(record_size)
        expected = msg.get("expected_body_len")
        if expected is not None:
            total_expected += expected
        elif record_size is not None and record_count is not None:
            total_expected += record_size * record_count
        elif body_len is not None:
            total_expected += body_len

    if bad_record_size:
        warnings.append(
            f"summary.json has -13 messages with record_size != {RECORD_SIZE}: "
            f"{sorted(x for x in bad_record_size if x is not None)}"
        )

    if total_expected and total_expected != actual_inputs_len:
        warnings.append(
            f"summary.json -13 messages imply {total_expected} input bytes "
            f"but `inputs` file is {actual_inputs_len} bytes"
        )

    return warnings


def write_csv(path: Path, p1_words: list[int], p2_words: list[int]) -> None:
    with path.open("w", newline="") as f:
        f.write("frame,p1_word,p1_hex,p2_word,p2_hex\n")
        for i, (p1, p2) in enumerate(zip(p1_words, p2_words)):
            f.write(f"{i},{p1},0x{p1:04x},{p2},0x{p2:04x}\n")


def write_bin(path: Path, p1_words: list[int], p2_words: list[int]) -> None:
    """frame_count x {u16 p1, u16 p2} little-endian."""
    with path.open("wb") as f:
        for p1, p2 in zip(p1_words, p2_words):
            f.write(struct.pack("<HH", p1, p2))


# ============================ SCRD reader (Python mirror of
# tools/compress_ram_dumps.py's encoder / src/test/ram_archive.c's
# decoder) ==================================================================


class ScrdFrameTable(NamedTuple):
    frame_count: int
    entries: list[tuple[int, int]]  # (offset, size), absolute file offsets


def read_scrd_table(data: bytes) -> ScrdFrameTable:
    if data[:4] != SCRD_MAGIC:
        raise ValueError("not an SCRD archive (bad magic)")

    (frame_count,) = struct.unpack_from("<H", data, 4)
    entries = []
    for i in range(frame_count):
        off, size = struct.unpack_from("<II", data, SCRD_HEADER_SIZE + i * SCRD_TABLE_ENTRY_SIZE)
        entries.append((off, size))

    return ScrdFrameTable(frame_count, entries)


def zero_run_decode(compressed: bytes, out_size: int) -> bytearray:
    """Inverse of compress_ram_dumps.py's zero_run_encode / mirrors
    src/test/ram_archive.c's decompress_frame: a control byte < 0x80 is a
    literal run of (control + 1) following bytes; a control byte >= 0x80 is
    a run of ((control & 0x7F) + 1) zero bytes (no payload bytes follow)."""
    out = bytearray(out_size)
    i = 0
    o = 0
    n = len(compressed)

    while i < n and o < out_size:
        control = compressed[i]
        i += 1
        if control < 0x80:
            length = control + 1
            out[o:o + length] = compressed[i:i + length]
            i += length
            o += length
        else:
            length = (control & 0x7F) + 1
            o += length  # bytearray is already zero-initialized

    return out


def read_be_u16(frame: bytearray, offset: int) -> int:
    return (frame[offset] << 8) | frame[offset + 1]


class ScrdWords(NamedTuple):
    frame_count: int
    p1sw0: list[int]
    p2sw0: list[int]
    p1sw1: list[int]
    p2sw1: list[int]


def extract_scrd_words(path: Path) -> ScrdWords:
    """Decode an SCRD archive frame-by-frame (frame 0 = raw payload, every
    subsequent frame XORed against the running reconstructed state -- same
    forward-only walk as RamArchive_SeekFrame in src/test/ram_archive.c),
    pulling only the P1SW_0/P2SW_0/P1SW_1/P2SW_1 big-endian u16 words out of
    each reconstructed frame (we never need to keep whole 512KiB frames
    around)."""
    data = path.read_bytes()
    table = read_scrd_table(data)

    p1sw0: list[int] = [0] * table.frame_count
    p2sw0: list[int] = [0] * table.frame_count
    p1sw1: list[int] = [0] * table.frame_count
    p2sw1: list[int] = [0] * table.frame_count

    accum_int = 0
    for i, (off, size) in enumerate(table.entries):
        compressed = data[off:off + size]
        payload = zero_run_decode(compressed, RAM_FRAME_SIZE)
        payload_int = int.from_bytes(payload, "big")

        if i == 0:
            accum_int = payload_int
        else:
            accum_int ^= payload_int

        accum = bytearray(accum_int.to_bytes(RAM_FRAME_SIZE, "big"))
        p1sw0[i] = read_be_u16(accum, P1SW_0_OFFSET)
        p2sw0[i] = read_be_u16(accum, P2SW_0_OFFSET)
        p1sw1[i] = read_be_u16(accum, P1SW_1_OFFSET)
        p2sw1[i] = read_be_u16(accum, P2SW_1_OFFSET)

    return ScrdWords(table.frame_count, p1sw0, p2sw0, p1sw1, p2sw1)


# =========================== alignment search =============================


def pack_pairs(p1: list[int], p2: list[int]) -> bytes:
    parts = [struct.pack("<HH", a, b) for a, b in zip(p1, p2)]
    return b"".join(parts)


class AlignmentResult(NamedTuple):
    offset: int
    agreement_pct: float
    both_match: int
    p1_only_mismatch: int
    p2_only_mismatch: int
    both_mismatch: int
    corroborations: int  # number of independent high-signal probes that agreed on this offset
    mismatch_samples: list[tuple[int, int, int, int, int]]  # (frame, exp_p1, got_p1, exp_p2, got_p2)


def score_alignment(
    decoded_p1: list[int],
    decoded_p2: list[int],
    target_p1: list[int],
    target_p2: list[int],
    offset: int,
    max_samples: int = 20,
) -> AlignmentResult:
    m = len(target_p1)
    both_match = 0
    p1_only_mismatch = 0
    p2_only_mismatch = 0
    both_mismatch = 0
    samples: list[tuple[int, int, int, int, int]] = []

    for i in range(m):
        dp1 = decoded_p1[offset + i]
        dp2 = decoded_p2[offset + i]
        tp1 = target_p1[i]
        tp2 = target_p2[i]
        p1_ok = dp1 == tp1
        p2_ok = dp2 == tp2

        if p1_ok and p2_ok:
            both_match += 1
        elif p1_ok and not p2_ok:
            p2_only_mismatch += 1
        elif p2_ok and not p1_ok:
            p1_only_mismatch += 1
        else:
            both_mismatch += 1

        if not (p1_ok and p2_ok) and len(samples) < max_samples:
            samples.append((i, tp1, dp1, tp2, dp2))

    agreement_pct = (both_match / m * 100.0) if m else 0.0
    return AlignmentResult(
        offset, agreement_pct, both_match, p1_only_mismatch, p2_only_mismatch,
        both_mismatch, 0, samples,
    )


class JitterAnalysis(NamedTuple):
    total_mismatches: int
    recoverable_within_1: int  # exact match found at decoded[offset+i-1] or [offset+i+1]
    recoverable_within_2: int  # additionally recoverable at +/-2
    unrecoverable: int         # not an exact match at any of +/-1, +/-2
    num_runs: int              # runs of consecutive mismatched frame indices
    max_run_length: int
    mean_run_length: float


def analyze_mismatch_jitter(
    decoded_p1: list[int],
    decoded_p2: list[int],
    target_p1: list[int],
    target_p2: list[int],
    offset: int,
    max_shift: int = 2,
) -> Optional[JitterAnalysis]:
    """For every mismatched frame at the given (already-established) offset,
    test whether the target's value for that single frame is an EXACT match
    a small number of frames earlier/later in the decoded stream (+/-1, then
    +/-2). This distinguishes "the whole run is one steady global offset off"
    (which find_best_offset would already have found directly, since it
    scores the full archive at every exact-anchored candidate) from "this
    particular frame lags/leads by one or two frames" -- the latch-delay /
    hitstop / pause-frame jitter the plan's Step B1 explicitly names as an
    expected, not-a-bug source of disagreement. Runs of consecutive
    mismatched indices are also reported, since sustained multi-frame runs
    (rather than isolated single frames) point at a brief span where the two
    recordings' frame *rate* diverged (e.g. a hitstop/freeze), not a single
    edge-detection quirk."""
    n = len(decoded_p1)
    m = len(target_p1)
    mismatch_frames = []

    for i in range(m):
        j = offset + i
        if decoded_p1[j] != target_p1[i] or decoded_p2[j] != target_p2[i]:
            mismatch_frames.append(i)

    if not mismatch_frames:
        return None

    recoverable_1 = 0
    recoverable_2 = 0
    unrecoverable = 0

    for i in mismatch_frames:
        tp1, tp2 = target_p1[i], target_p2[i]
        found = False
        for d in range(1, max_shift + 1):
            for sign in (-1, 1):
                j = offset + i + sign * d
                if 0 <= j < n and decoded_p1[j] == tp1 and decoded_p2[j] == tp2:
                    if d == 1:
                        recoverable_1 += 1
                    else:
                        recoverable_2 += 1
                    found = True
                    break
            if found:
                break
        if not found:
            unrecoverable += 1

    runs: list[int] = []
    run_len = 1
    for k in range(1, len(mismatch_frames)):
        if mismatch_frames[k] == mismatch_frames[k - 1] + 1:
            run_len += 1
        else:
            runs.append(run_len)
            run_len = 1
    runs.append(run_len)

    return JitterAnalysis(
        total_mismatches=len(mismatch_frames),
        recoverable_within_1=recoverable_1,
        recoverable_within_2=recoverable_2,
        unrecoverable=unrecoverable,
        num_runs=len(runs),
        max_run_length=max(runs),
        mean_run_length=sum(runs) / len(runs),
    )


def find_best_offset(
    decoded_p1: list[int],
    decoded_p2: list[int],
    decoded_bytes: bytes,
    target_p1: list[int],
    target_p2: list[int],
    min_signal_frac: float = 0.15,
) -> Optional[AlignmentResult]:
    """Locate the offset in the decoded stream where `target_p1/p2` reads as
    a contiguous run, by searching for an EXACT byte-for-byte substring match
    of a "high-signal" (not-mostly-neutral) probe window and verifying with a
    full-length score. No fuzzy/approximate matching is used to *find* the
    offset -- only to report the resulting agreement once an exact anchor is
    established. If no exact anchor exists at any probe length tried, returns
    None (this is the "no alignment exists" failure mode the plan calls out,
    not something to be papered over)."""
    n = len(decoded_p1)
    m = len(target_p1)
    if m == 0 or n < m:
        return None

    target_pairs = list(zip(target_p1, target_p2))
    target_bytes = pack_pairs(target_p1, target_p2)

    # Evenly spaced candidate probe start positions across the archive,
    # avoiding the extreme ends (character-intro poses / KO freeze frames
    # are more likely to be degenerate there).
    candidate_starts = sorted(
        {int(m * frac) for frac in (0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9)}
    )

    found_offsets: dict[int, int] = {}  # offset -> corroboration count

    for probe_len in (120, 80, 40, 20, 10, 5):
        if probe_len > m:
            continue

        any_signal_probe = False
        for start in candidate_starts:
            if start + probe_len > m:
                continue

            window = target_pairs[start:start + probe_len]
            nonzero = sum(1 for (a, b) in window if a or b)
            if nonzero < probe_len * min_signal_frac:
                continue  # too close to all-neutral; skip (spurious-match risk)
            any_signal_probe = True

            probe_bytes = target_bytes[start * 4:(start + probe_len) * 4]
            positions = []
            idx = decoded_bytes.find(probe_bytes)
            while idx != -1:
                if idx % 4 == 0:
                    positions.append(idx // 4)
                idx = decoded_bytes.find(probe_bytes, idx + 1)

            if len(positions) == 1:
                off = positions[0] - start
                if 0 <= off <= n - m:
                    found_offsets[off] = found_offsets.get(off, 0) + 1

        if found_offsets:
            break  # a shorter/looser probe_len is only tried if longer ones found nothing
        if not any_signal_probe and probe_len == 5:
            # even the shortest probe length never found a high-signal window
            # (session is essentially all-neutral input for this stretch) --
            # nothing more to try.
            break

    if not found_offsets:
        return None

    scored = []
    for off, corro in found_offsets.items():
        result = score_alignment(decoded_p1, decoded_p2, target_p1, target_p2, off)
        scored.append((result.agreement_pct, corro, off, result))

    scored.sort(key=lambda t: (t[0], t[1]), reverse=True)
    best_agreement, best_corro, best_off, best_result = scored[0]
    return best_result._replace(corroborations=best_corro)


# ================================= CLI ====================================


QUARK_SUFFIX_RE = re.compile(r"^(.*)\.(\d+)$")
SCRD_FILENAME_RE = re.compile(r"^(?P<quark>.+)_game_(?P<game>\d+)(?P<partial>_partial)?\.scrd$")


def normalize_quark(name: str) -> str:
    """Strip an optional trailing ".<digits>" fightcade session suffix, e.g.
    "sfiii3nr1-1641508702494-7287.7" -> "sfiii3nr1-1641508702494-7287". Both
    forms were confirmed (this step, via md5) to name the identical session
    for at least one quark in the corpus."""
    m = QUARK_SUFFIX_RE.match(name)
    return m.group(1) if m else name


def find_matching_scrd_files(scrd_dir: Path, replay_dir_name: str) -> list[tuple[int, bool, Path]]:
    """Return [(game_num, is_partial, path), ...] for every .scrd file in
    scrd_dir whose quark prefix normalizes to the same base as
    replay_dir_name, deduped by (game_num, is_partial) -- keeping the first
    match and logging (via printed note, not raised) when a later duplicate
    is skipped."""
    target_base = normalize_quark(replay_dir_name)
    seen: dict[tuple[int, bool], Path] = {}
    results: list[tuple[int, bool, Path]] = []

    for path in sorted(scrd_dir.glob("*.scrd")):
        m = SCRD_FILENAME_RE.match(path.name)
        if not m:
            continue
        quark = m.group("quark")
        if normalize_quark(quark) != target_base:
            continue

        game_num = int(m.group("game"))
        is_partial = m.group("partial") is not None
        key = (game_num, is_partial)

        if key in seen:
            prior = seen[key]
            if prior.stat().st_size == path.stat().st_size:
                print(f"  note: {path.name} duplicates {prior.name} (same size) -- skipping", file=sys.stderr)
            else:
                print(
                    f"  warning: {path.name} and {prior.name} share game_{game_num} "
                    f"but differ in size ({path.stat().st_size} vs {prior.stat().st_size}) "
                    f"-- keeping first seen only", file=sys.stderr,
                )
            continue

        seen[key] = path
        results.append((game_num, is_partial, path))

    results.sort(key=lambda t: (t[0], t[1]))
    return results


def run_cross_check(
    replay_dir: Path,
    scrd_dir: Path,
    decoded_p1: list[int],
    decoded_p2: list[int],
    max_shift: int = 2,
) -> list[dict]:
    decoded_bytes = pack_pairs(decoded_p1, decoded_p2)
    matches = find_matching_scrd_files(scrd_dir, replay_dir.name)

    if not matches:
        print(f"  no .scrd files in {scrd_dir} matched quark base "
              f"{normalize_quark(replay_dir.name)!r}", file=sys.stderr)
        return []

    report = []
    for game_num, is_partial, path in matches:
        label = f"game_{game_num}" + ("_partial" if is_partial else "")
        print(f"  [{label}] {path.name} ...", file=sys.stderr)

        words = extract_scrd_words(path)

        result_sw0 = find_best_offset(
            decoded_p1, decoded_p2, decoded_bytes, words.p1sw0, words.p2sw0,
        )
        result_sw1 = find_best_offset(
            decoded_p1, decoded_p2, decoded_bytes, words.p1sw1, words.p2sw1,
        )

        best_label = None
        best_result = None
        if result_sw0 and result_sw1:
            if result_sw0.agreement_pct >= result_sw1.agreement_pct:
                best_label, best_result = "P1SW_0/P2SW_0", result_sw0
            else:
                best_label, best_result = "P1SW_1/P2SW_1", result_sw1
        elif result_sw0:
            best_label, best_result = "P1SW_0/P2SW_0", result_sw0
        elif result_sw1:
            best_label, best_result = "P1SW_1/P2SW_1", result_sw1

        jitter = None
        if best_result is not None:
            target_p1, target_p2 = (
                (words.p1sw0, words.p2sw0) if best_label == "P1SW_0/P2SW_0" else (words.p1sw1, words.p2sw1)
            )
            jitter = analyze_mismatch_jitter(
                decoded_p1, decoded_p2, target_p1, target_p2, best_result.offset, max_shift=max_shift,
            )

        entry = {
            "quark": replay_dir.name,
            "archive": path.name,
            "game": game_num,
            "partial": is_partial,
            "archive_frame_count": words.frame_count,
            "decoded_frame_count": len(decoded_p1),
            "sw0": result_sw0._asdict() if result_sw0 else None,
            "sw1": result_sw1._asdict() if result_sw1 else None,
            "best_target": best_label,
            "best": best_result._asdict() if best_result else None,
            "jitter": jitter._asdict() if jitter else None,
        }
        report.append(entry)

        if best_result is None:
            print(f"    NO EXACT ALIGNMENT FOUND for {label} (searched both SW_0 and SW_1)",
                  file=sys.stderr)
        else:
            print(
                f"    best={best_label} offset={best_result.offset} "
                f"agreement={best_result.agreement_pct:.2f}% "
                f"(both_match={best_result.both_match}/{words.frame_count}, "
                f"corroborations={best_result.corroborations})",
                file=sys.stderr,
            )
            if jitter:
                pct_1 = jitter.recoverable_within_1 / jitter.total_mismatches * 100.0
                pct_2 = jitter.recoverable_within_2 / jitter.total_mismatches * 100.0
                pct_un = jitter.unrecoverable / jitter.total_mismatches * 100.0
                print(
                    f"    jitter: {jitter.total_mismatches} mismatched frames -- "
                    f"{pct_1:.1f}% resolve at +/-1 frame, {pct_2:.1f}% at +/-2..+/-{max_shift}, "
                    f"{pct_un:.1f}% unrecoverable within +/-{max_shift} "
                    f"({jitter.num_runs} runs, max run {jitter.max_run_length}, "
                    f"mean run {jitter.mean_run_length:.2f})",
                    file=sys.stderr,
                )

    return report


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("replay_dir", type=Path, help="replay output dir containing an `inputs` file")
    parser.add_argument("--csv", type=Path, help="write decoded (frame,p1,p2) rows to this CSV path")
    parser.add_argument("--bin", type=Path, help="write decoded frame_count x {u16 p1, u16 p2} LE to this path")
    parser.add_argument("--scrd-dir", type=Path, help="directory containing this quark's game_N[.].scrd archives (may hold other quarks too -- filtered by name)")
    parser.add_argument("--json-report", type=Path, help="write the cross-check report as JSON to this path")
    parser.add_argument(
        "--max-shift", type=int, default=2,
        help="jitter analysis: max local frame shift (+/-N) to search for an exact match when a frame "
             "mismatches at the established global offset (default: 2). Widening this turns the "
             "previously ad-hoc manual +/-6 checks (Step B1 outlier follow-up on 2133_game_1/game_3) "
             "into a reproducible, shipped option -- exact matching only, no fuzzy comparison.",
    )
    args = parser.parse_args(argv)

    inputs_path = args.replay_dir / "inputs"
    if not inputs_path.is_file():
        print(f"error: {inputs_path} not found", file=sys.stderr)
        return 1

    data = inputs_path.read_bytes()
    p1_words, p2_words = decode_inputs_bytes(data)
    frame_count = len(p1_words)
    print(f"{args.replay_dir.name}: decoded {frame_count} frames from {inputs_path}", file=sys.stderr)

    warnings = sanity_check_summary(args.replay_dir / "summary.json", len(data))
    for w in warnings:
        print(f"  warning: {w}", file=sys.stderr)

    if args.csv:
        write_csv(args.csv, p1_words, p2_words)
        print(f"  wrote {args.csv}", file=sys.stderr)

    if args.bin:
        write_bin(args.bin, p1_words, p2_words)
        print(f"  wrote {args.bin}", file=sys.stderr)

    if args.scrd_dir:
        report = run_cross_check(args.replay_dir, args.scrd_dir, p1_words, p2_words, max_shift=args.max_shift)
        if args.json_report:
            args.json_report.parent.mkdir(parents=True, exist_ok=True)
            args.json_report.write_text(json.dumps(report, indent=2))
            print(f"  wrote {args.json_report}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
