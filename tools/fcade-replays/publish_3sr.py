#!/usr/bin/env python3
"""Mac-side batch converter: catalog quarkids -> statcheck-clean, named
`.3sr` + `.meta.json` pairs, ready for `push-3sr.sh`.

docs/plan-osd-replay-browser.md Stage S1, item 2. This is the "kill NEEDS
CONVERSION" pipeline: download a Fightcade replay stream, run it through the
FBNeo replay runner (crowded-street/fbneo-replay-runner @ ccf96ab -- see
tools/replay_preprocessor.py's header for the exact version-skew reasoning),
compress the resulting per-frame RAM dumps to a `.scrd` archive per in-session
game, then gate each archive TWICE before publishing it:

  1. ELIGIBILITY (`make_3sr.probe_match_start`). Does this segment hold a match
     two humans played? A segment holding no match at all (H1) or one the
     cabinet ran against the CPU (H4b) is skipped outright -- the device viewer
     forces two operators, so a `.3sr` built from either desyncs by frame 60,
     measured. This is why a quark can legitimately yield fewer games than
     `quark.json.num_matches`.
  2. CORRECTNESS (`tools/statcheck_runner.py`). Only statcheck-clean games get
     published -- a divergent game is dropped, never shipped.

Then `make_3sr.py generate --quark-json <catalog row>` runs, so every published
`.meta.json` carries REAL `players[]`/`date`/`duration` (the make_3sr.py header
comment above `build_meta()` explains why this is non-negotiable: the sidecar is
the ONLY source of player names on-device).

One quark -> zero or more `<out-dir>/<quarkid>/game_N.3sr` +
`game_N.meta.json` pairs (only the clean ones), plus a top-level
`published_manifest.json` recording quarkid -> published / skipped (by reason) /
divergent / failed game indices for operator visibility. Those four are kept
apart deliberately: see `QuarkOutcome`.

DISK SAFETY (the documented A4 ENOSPC lesson -- docs/fcade-replay-notes.md
section 4 "Disk-space gotcha"): the FBNeo runner writes RAW per-frame `.ram`
dumps (524,288 bytes/frame, uncompressed) for the ENTIRE session before
`tools/replay_preprocessor.py`'s existing helper ever compresses anything --
a multi-game session can hit tens of GB before compression starts, which is
exactly what caused an ENOSPC + killed background tasks in that session. This
tool does NOT reuse that whole-session-then-compress pattern. Instead it
polls the runner's own dump directory while the runner is still running and
compresses+deletes each `game_N/` the MOMENT the runner's own game-boundary
detection proves it complete (creation of `game_(N+1)/` -- see
`ReplayDumpCps3MainRam()` in the runner's `run.cpp`, which only creates the
next game's directory on an in-game -> not-in-game transition). At most one
game's raw frames are ever on disk at a time. Across a batch of MULTIPLE
quarks, quarks are also processed strictly one at a time end-to-end (download
-> run -> gate -> publish -> cleanup) before the next quark starts, for the
same reason.

Usage:
  publish_3sr.py --catalog CATALOG.json --runner RUNNER_EXE \\
      --statcheck STATCHECK_EXE --out-dir OUT_DIR \\
      [--quark QUARKID [--quark QUARKID ...]] [--gameid sfiii3nr1]
      [--limit N] [--work-dir DIR] [--keep-work] [--force-download]
      [--statcheck-timeout SECS] [--checksum-interval N] [--fail-fast]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

HERE = Path(__file__).resolve().parent
TOOLS_DIR = HERE.parent
sys.path.insert(0, str(HERE))       # fcade_replay_tool, make_3sr, decode_inputs
sys.path.insert(0, str(TOOLS_DIR))  # replay_preprocessor, compress_ram_dumps, statcheck_runner

from fcade_replay_tool import ReplayTarget, download_replay  # noqa: E402
from compress_ram_dumps import compress_ram_dumps  # noqa: E402
from make_3sr import (  # noqa: E402
    CorruptArchiveError,
    CpuPlayerError,
    ExtractError,
    NoMatchStartError,
    probe_match_start,
)

MAKE_3SR = HERE / "make_3sr.py"
STATCHECK_RUNNER = TOOLS_DIR / "statcheck_runner.py"

DEFAULT_GAMEID = "sfiii3nr1"
DEFAULT_EMULATOR = "fbneo"
DEFAULT_HOST = "ggpo.fightcade.com"
DEFAULT_PORT = 7100
DEFAULT_RUNNER_POLL_S = 2.0
DEFAULT_RUNNER_TIMEOUT_S = 900.0
DEFAULT_STATCHECK_TIMEOUT_S = 30.0


class PublishError(RuntimeError):
    """Hard failure for one quark (download/runner failure) -- distinct from
    a statcheck-gate rejection, which is an expected, per-game outcome."""


@dataclass
class GameOutcome:
    game_index: int
    scrd_path: Path
    clean: bool
    reason: str = ""
    out_3sr: Optional[Path] = None
    out_meta: Optional[Path] = None


@dataclass
class QuarkOutcome:
    """Per-quark tally. The three drop buckets are kept SEPARATE on purpose:
    they mean different things and anyone counting `divergent` across manifests
    to size the engine's remaining bug list must not be handed segments that
    were never comparable in the first place.

      published  -- a `.3sr` was written.
      skipped    -- reason -> game indices. INELIGIBLE segments: the recording
                    itself cannot be replayed by the device viewer, whatever
                    the engine does. "no-match-start" (H1) and "cpu-player"
                    (H4b), both detected by `make_3sr.probe_match_start`.
                    Expected, not a defect; this is why a quark can yield fewer
                    games than `quark.json.num_matches`.
      divergent  -- statcheck ran on a comparable segment and the engine
                    disagreed with the CPS3 recording. A real worklist item.
      failed     -- the converter itself broke (corrupt archive, make_3sr
                    error). An operator problem, not an engine one.
    """

    quarkid: str
    published: list[int] = field(default_factory=list)
    skipped: dict[str, list[int]] = field(default_factory=dict)
    divergent: list[int] = field(default_factory=list)
    failed: list[int] = field(default_factory=list)
    error: Optional[str] = None

    def skip(self, reason: str, game_index: int) -> None:
        self.skipped.setdefault(reason, []).append(game_index)


# ============================ catalog ============================


def load_catalog(path: Path) -> dict[str, dict]:
    data = json.loads(path.read_text(encoding="utf-8"))
    rows = data.get("rows") if isinstance(data, dict) else None
    if not isinstance(rows, list):
        raise PublishError(f"{path}: catalog JSON must be an object with a 'rows' array")
    by_id: dict[str, dict] = {}
    for row in rows:
        if isinstance(row, dict) and isinstance(row.get("quarkid"), (str, int)):
            by_id[str(row["quarkid"])] = row
    return by_id


# ============================ disk-safe runner drive ============================


def _game_dirs(dump_root: Path) -> dict[int, Path]:
    out: dict[int, Path] = {}
    if not dump_root.is_dir():
        return out
    for child in dump_root.iterdir():
        if child.is_dir() and child.name.startswith("game_"):
            try:
                idx = int(child.name[len("game_") :])
            except ValueError:
                continue
            out[idx] = child
    return out


def run_replay_disk_safe(
    runner: Path,
    replay_dir: Path,
    gameid: str,
    out_dir: Path,
    force: bool,
    poll_interval_s: float,
    timeout_s: float,
    log,
) -> list[Path]:
    """Runs the FBNeo runner against `replay_dir` (must contain `savestate` +
    `inputs`), compressing+deleting each `game_N/` raw-dump directory the
    moment the runner proves it complete (a higher-numbered game_N appeared),
    rather than waiting for the whole session to finish. Returns the list of
    written `.scrd` paths (game index order). Raises PublishError on a hard
    runner failure (nonzero exit with zero games produced, or a timeout)."""

    with tempfile.TemporaryDirectory(prefix="publish3sr-ramdump-") as dump_name:
        dump_root = Path(dump_name)
        command = [
            str(runner),
            gameid,
            "-replay-state",
            str(replay_dir / "savestate"),
            "-replay-inputs",
            str(replay_dir / "inputs"),
            "-headless",
            "-dump-ram-path",
            str(dump_root),
        ]
        log(f"  runner: {' '.join(command)}")

        runner_log_path = out_dir / f"{replay_dir.name}.runner.log"
        out_dir.mkdir(parents=True, exist_ok=True)
        written: list[Path] = []
        settled: set[int] = set()

        with runner_log_path.open("wb") as runner_log:
            proc = subprocess.Popen(command, stdout=runner_log, stderr=subprocess.STDOUT)
            start = time.monotonic()
            try:
                while True:
                    rc = proc.poll()
                    dirs = _game_dirs(dump_root)
                    if dirs:
                        highest = max(dirs)
                        # Every game strictly below the highest-seen index is
                        # provably complete (the runner only creates game_(N+1)
                        # on N's in-game -> not-in-game transition) -- compress
                        # + delete it now, don't wait for the process to exit.
                        for idx in sorted(dirs):
                            if idx in settled:
                                continue
                            if idx < highest or rc is not None:
                                # The runner sometimes creates a trailing
                                # game_(N+1) directory at the very end of a
                                # replay with NO frames in it (a spurious
                                # in-game -> not-in-game boundary fired right at
                                # exit). compress_ram_dumps() raises
                                # "no RAM dump frames found" on an empty dir --
                                # which, uncaught, crashes the ENTIRE batch on
                                # one quark. Treat an empty game dir as a
                                # non-game: drop it and move on.
                                if not any(p.is_file() for p in dirs[idx].iterdir()):
                                    log(f"  skipping empty game_{idx} dir (no RAM frames -- spurious end-of-replay boundary)")
                                    shutil.rmtree(dirs[idx], ignore_errors=True)
                                    settled.add(idx)
                                    continue
                                # Name the archive `<quark-token>_game_<N>.scrd`
                                # so make_3sr.py's SCRD_FILENAME_RE
                                # (decode_inputs.py:566,
                                # `^(?P<quark>.+)_game_(?P<game>\d+)...\.scrd$`)
                                # matches and build_meta() records the real
                                # integer game_index instead of null (P-2). A
                                # bare `game_N.scrd` has no `_game_` prefix and
                                # never matched.
                                out_path = out_dir / replay_dir.name / f"{replay_dir.name}_game_{idx}.scrd"
                                frame_count = compress_ram_dumps(dirs[idx], out_path, force=force)
                                log(f"  compressed game_{idx} ({frame_count} frames) -> {out_path}")
                                shutil.rmtree(dirs[idx], ignore_errors=True)
                                written.append(out_path)
                                settled.add(idx)
                    if rc is not None:
                        break
                    if time.monotonic() - start > timeout_s:
                        proc.kill()
                        proc.wait()
                        raise PublishError(
                            f"runner timed out after {timeout_s:g}s on {replay_dir.name} "
                            f"(see {runner_log_path})"
                        )
                    time.sleep(poll_interval_s)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()

        if not written:
            raise PublishError(
                f"runner produced no game_N RAM-dump directories for {replay_dir.name} "
                f"(exit={proc.returncode}; see {runner_log_path})"
            )
        if proc.returncode != 0:
            log(
                f"  warning: runner exited {proc.returncode} for {replay_dir.name} but "
                f"{len(written)} game(s) were captured before exit -- keeping them "
                f"(see {runner_log_path})"
            )

        return written


# ============================ statcheck gate ============================


def statcheck_gate(statcheck_exe: Path, scrd_path: Path, timeout_s: float, log) -> tuple[bool, str]:
    """Runs tools/statcheck_runner.py against exactly one archive (a fresh
    temp dir containing only `scrd_path`, symlinked so no copy is needed).
    Returns (clean, detail)."""

    with tempfile.TemporaryDirectory(prefix="publish3sr-gate-") as gate_root_name:
        gate_root = Path(gate_root_name)
        gate_replay_dir = gate_root / scrd_path.parent.name
        gate_replay_dir.mkdir(parents=True, exist_ok=True)
        # statcheck_runner.find_game_archives discovers archives with a
        # FULLMATCH on r"game_(\d+)\.scrd$" -- it does NOT accept publish's
        # "<token>_game_N.scrd" naming (that prefix exists only so make_3sr's
        # SCRD_FILENAME_RE records the integer game_index, P-2). Symlink the
        # gate archive under a BARE game_<N>.scrd name so it is discovered;
        # otherwise a statcheck-CLEAN game is silently mis-reported as
        # "divergent" ("no game_N.scrd archives found" -> rc=1, on stderr).
        _gm = re.search(r"_game_(\d+)\.scrd$", scrd_path.name)
        _gate_name = f"game_{_gm.group(1)}.scrd" if _gm else scrd_path.name
        link_path = gate_replay_dir / _gate_name
        try:
            link_path.symlink_to(scrd_path.resolve())
        except OSError:
            shutil.copy2(scrd_path, link_path)  # symlink unsupported -- fall back to a copy

        command = [
            sys.executable,
            str(STATCHECK_RUNNER),
            str(statcheck_exe),
            str(gate_root),
            "--timeout",
            str(timeout_s),
        ]
        # SDL_VIDEODRIVER/SDL_AUDIODRIVER=dummy: the statcheck executable is a
        # full SDL app (real Cocoa window + Metal renderer by default, even
        # for --headless statcheck runs -- confirmed via `Selected video
        # driver: cocoa` / `Selected renderer: metal` in its own log). Under
        # heavy host contention (shared-machine load average > 8 measured
        # during this batch's development), real window/GPU compositing was
        # observed to make an otherwise-1.5s statcheck run take OVER 2
        # MINUTES and still not finish -- forcing the dummy video+audio
        # drivers bypasses all of that and reliably finishes in ~1-2s
        # regardless of contention (measured: 1.487s wall, 94% cpu, on the
        # exact same archive that hung past 120s with the default cocoa
        # driver). This does not change what statcheck measures (engine
        # state, not pixels) -- only how it presents/does not present a
        # window while measuring it.
        env = dict(os.environ)
        env["SDL_VIDEODRIVER"] = "dummy"
        env["SDL_AUDIODRIVER"] = "dummy"
        proc = subprocess.run(command, capture_output=True, text=True, env=env)
        clean = proc.returncode == 0
        tail = "\n".join((proc.stdout or "").splitlines()[-10:])
        log(f"    statcheck {scrd_path.name}: {'PASS' if clean else 'FAIL'} (rc={proc.returncode})")
        return clean, tail


# ============================ make_3sr ============================


def generate_3sr(scrd_path: Path, out_3sr: Path, out_meta: Path, quark_json: Path, checksum_interval: int, log) -> int:
    """Returns make_3sr.py `generate`'s own exit code: 0 converted, 1 failure,
    2 segment holds no match, 3 recorded against the CPU. Non-zero always means
    nothing was written."""
    out_3sr.parent.mkdir(parents=True, exist_ok=True)
    command = [
        sys.executable,
        str(MAKE_3SR),
        "generate",
        str(scrd_path),
        "--out",
        str(out_3sr),
        "--meta-out",
        str(out_meta),
        "--quark-json",
        str(quark_json),
        "--checksum-interval",
        str(checksum_interval),
    ]
    proc = subprocess.run(command, capture_output=True, text=True)
    if proc.returncode != 0:
        # 2 = no match in segment, 3 = recorded against the CPU (make_3sr.py
        # `cmd_generate`, same codes as src/main.c gives statcheck). Both are
        # SKIPS, not failures -- and both should already have been caught by
        # the probe_match_start() gate above, so reaching here means the two
        # disagreed and is worth saying out loud.
        verb = "SKIPPED" if proc.returncode in (2, 3) else "FAILED"
        log(f"    make_3sr {verb} (rc={proc.returncode}) for {scrd_path.name}: {proc.stderr.strip()}")
        return proc.returncode
    log(f"    make_3sr OK: {out_3sr}")
    return 0


# ============================ per-quark pipeline ============================


def publish_quark(
    quarkid: str,
    row: dict,
    *,
    runner: Path,
    statcheck_exe: Path,
    out_dir: Path,
    work_dir: Path,
    host: str,
    keep_work: bool,
    force_download: bool,
    statcheck_timeout: float,
    checksum_interval: int,
    runner_timeout: float,
    log,
) -> QuarkOutcome:
    outcome = QuarkOutcome(quarkid=quarkid)
    gameid = row.get("gameid") or DEFAULT_GAMEID
    emulator = row.get("emulator") or DEFAULT_EMULATOR
    token = f"{quarkid}.7"

    quark_work = work_dir / quarkid
    download_dir = quark_work / token
    scrd_dir = quark_work / "scrd"

    try:
        # --- 1. download the stream (idempotent: reuse an existing download
        # unless --force-download) ---
        have_download = (download_dir / "savestate").is_file() and (download_dir / "inputs").is_file()
        if have_download and not force_download:
            log(f"  download: reusing existing {download_dir}")
        else:
            target = ReplayTarget(emulator=emulator, game=gameid, token=token, port=DEFAULT_PORT)
            log(f"  downloading fcade://stream/{emulator}/{gameid}/{token},{DEFAULT_PORT} -> {download_dir}")
            try:
                download_replay(
                    target=target,
                    host=host,
                    out_dir=download_dir,
                    timeout=10.0,
                    idle_timeout=2.0,
                    max_idle_timeouts=20,
                    max_frames=200_000,
                    local_port=6004,
                    send_delay_ms=15.0,
                )
            except Exception as exc:  # noqa: BLE001
                raise PublishError(f"download failed: {exc}") from exc

        if not (download_dir / "savestate").is_file() or not (download_dir / "inputs").is_file():
            raise PublishError("download produced no savestate/inputs (expired quark?)")

        # --- 2. run the FBNeo runner, disk-safe (one game's raw dump at a time) ---
        scrd_paths = run_replay_disk_safe(
            runner=runner,
            replay_dir=download_dir,
            gameid=gameid,
            out_dir=scrd_dir,
            force=True,
            poll_interval_s=DEFAULT_RUNNER_POLL_S,
            timeout_s=runner_timeout,
            log=log,
        )

        # --- 3. write ONE quark.json (real catalog row) reused for every
        # game's --quark-json (players/date/duration are session-wide, not
        # per-game) ---
        quark_json_path = quark_work / "quark.json"
        quark_json_path.write_text(json.dumps(row, indent=2), encoding="utf-8")

        # --- 4. per game: eligibility gate, statcheck gate, publish ---
        for scrd_path in sorted(scrd_paths, key=lambda p: int(p.stem.split("_")[-1])):
            game_index = int(scrd_path.stem.split("_")[-1])

            # ELIGIBILITY FIRST, in-process and cheap (probe_match_start stops
            # decoding at the match start). A segment that holds no match (H1)
            # or that the cabinet ran against the CPU (H4b) is not something
            # statcheck can grade OR the device viewer can play -- a `.3sr`
            # made from one desyncs by frame 60, measured. Running the gate
            # first means every statcheck FAIL below is a REAL divergence, so
            # the `divergent` list in published_manifest.json finally means
            # what its name says.
            try:
                probe_match_start(scrd_path)
            except NoMatchStartError as exc:
                outcome.skip("no-match-start", game_index)
                log(f"    SKIPPED game_{game_index} (no-match-start -- segment holds no match, "
                    f"NOT a divergence): {exc}")
                continue
            except CpuPlayerError as exc:
                outcome.skip("cpu-player", game_index)
                log(f"    SKIPPED game_{game_index} (cpu-player -- recorded against the CPU, "
                    f"NOT a divergence): {exc}")
                continue
            except (CorruptArchiveError, ExtractError) as exc:
                outcome.failed.append(game_index)
                log(f"    FAILED game_{game_index} (unreadable archive): {exc}")
                continue

            clean, detail = statcheck_gate(statcheck_exe, scrd_path, statcheck_timeout, log)
            if not clean:
                outcome.divergent.append(game_index)
                log(f"    DROPPED game_{game_index} (statcheck-divergent): {detail}")
                continue

            out_3sr = out_dir / quarkid / f"game_{game_index}.3sr"
            out_meta = out_dir / quarkid / f"game_{game_index}.meta.json"
            rc = generate_3sr(scrd_path, out_3sr, out_meta, quark_json_path, checksum_interval, log)
            if rc == 0:
                outcome.published.append(game_index)
            elif rc == 2:
                outcome.skip("no-match-start", game_index)
            elif rc == 3:
                outcome.skip("cpu-player", game_index)
            else:
                outcome.failed.append(game_index)

    except PublishError as exc:
        outcome.error = str(exc)
        log(f"  ERROR: {exc}")
    except Exception as exc:  # noqa: BLE001
        # A single malformed quark (unexpected runner/compress/make_3sr state)
        # must NEVER abort an unattended batch -- record it and move on.
        outcome.error = f"unexpected: {exc.__class__.__name__}: {exc}"
        log(f"  ERROR (unexpected -- skipping quark): {exc.__class__.__name__}: {exc}")
    finally:
        if not keep_work:
            shutil.rmtree(quark_work, ignore_errors=True)

    return outcome


# ============================ CLI ============================


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--catalog", type=Path, required=True, help="catalog.json (tools/fcade-proxy 'rows' shape)")
    p.add_argument("--runner", type=Path, required=True, help="FBNeo replay runner executable")
    p.add_argument("--statcheck", type=Path, required=True, help="statcheck executable (THREESX_STATCHECK build)")
    p.add_argument("--out-dir", type=Path, required=True, help="output dir for <quarkid>/game_N.3sr + .meta.json")
    p.add_argument("--quark", action="append", default=[], help="quarkid to publish (repeatable); default: all catalog rows for --gameid")
    p.add_argument("--gameid", default=DEFAULT_GAMEID, help="Fightcade game id filter when --quark is not given")
    p.add_argument("--limit", type=int, default=None, help="cap the number of quarks processed (default-all mode only)")
    p.add_argument("--work-dir", type=Path, default=None, help="scratch dir (default: a fresh tempdir, cleaned per-quark)")
    p.add_argument("--keep-work", action="store_true", help="keep per-quark scratch (download/scrd) after processing -- debugging only")
    p.add_argument("--force-download", action="store_true", help="re-download even if a prior download for this quark is present")
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--statcheck-timeout", type=float, default=DEFAULT_STATCHECK_TIMEOUT_S)
    p.add_argument("--runner-timeout", type=float, default=DEFAULT_RUNNER_TIMEOUT_S)
    p.add_argument("--checksum-interval", type=int, default=60)
    p.add_argument("--fail-fast", action="store_true", help="stop at the first quark-level hard error (download/runner failure)")
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    def log(msg: str) -> None:
        print(msg, file=sys.stderr)

    if not args.runner.is_file() or not (args.runner.stat().st_mode & 0o111):
        log(f"error: runner not found/executable: {args.runner}")
        return 1
    if not args.statcheck.is_file() or not (args.statcheck.stat().st_mode & 0o111):
        log(f"error: statcheck executable not found/executable: {args.statcheck}")
        return 1

    catalog = load_catalog(args.catalog)
    log(f"loaded {len(catalog)} catalog rows from {args.catalog}")

    if args.quark:
        quark_ids = args.quark
        missing = [q for q in quark_ids if q not in catalog]
        if missing:
            log(f"error: quarkid(s) not found in catalog: {missing}")
            return 1
    else:
        quark_ids = [qid for qid, row in catalog.items() if row.get("gameid") == args.gameid]
        quark_ids.sort(key=lambda qid: catalog[qid].get("date") or 0, reverse=True)
        if args.limit is not None:
            quark_ids = quark_ids[: args.limit]

    if not quark_ids:
        log("error: no quarkids to process (empty catalog match)")
        return 1

    work_dir = args.work_dir or Path(tempfile.mkdtemp(prefix="publish3sr-work-"))
    work_dir.mkdir(parents=True, exist_ok=True)
    log(f"work dir: {work_dir}")

    outcomes: list[QuarkOutcome] = []
    for i, quarkid in enumerate(quark_ids, start=1):
        log(f"[{i}/{len(quark_ids)}] {quarkid}")
        outcome = publish_quark(
            quarkid,
            catalog[quarkid],
            runner=args.runner,
            statcheck_exe=args.statcheck,
            out_dir=args.out_dir,
            work_dir=work_dir,
            host=args.host,
            keep_work=args.keep_work,
            force_download=args.force_download,
            statcheck_timeout=args.statcheck_timeout,
            checksum_interval=args.checksum_interval,
            runner_timeout=args.runner_timeout,
            log=log,
        )
        outcomes.append(outcome)
        if outcome.error and args.fail_fast:
            log("--fail-fast: stopping")
            break

    # `skipped` and `failed` are separate keys, not folded into `divergent`:
    # see QuarkOutcome's docstring. A reader counting engine divergences must
    # read `divergent` and nothing else, and a reader asking "why did this quark
    # produce fewer games than num_matches" reads `skipped`.
    manifest = {
        "generated_at": int(time.time() * 1000),
        "gameid": args.gameid,
        "quarks": {
            o.quarkid: {
                "published": o.published,
                "skipped": o.skipped,
                "divergent": o.divergent,
                "failed": o.failed,
                "error": o.error,
            }
            for o in outcomes
        },
    }
    args.out_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = args.out_dir / "published_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    log(f"wrote {manifest_path}")

    total_published = sum(len(o.published) for o in outcomes)
    total_divergent = sum(len(o.divergent) for o in outcomes)
    total_failed = sum(len(o.failed) for o in outcomes)
    total_errors = sum(1 for o in outcomes if o.error)
    skipped_by_reason: dict[str, int] = {}
    for o in outcomes:
        for reason, games in o.skipped.items():
            skipped_by_reason[reason] = skipped_by_reason.get(reason, 0) + len(games)
    skip_detail = ", ".join(f"{n} {reason}" for reason, n in sorted(skipped_by_reason.items())) or "none"
    log(
        f"=== {len(outcomes)} quark(s) processed: {total_published} game(s) published, "
        f"{sum(skipped_by_reason.values())} skipped as ineligible ({skip_detail}), "
        f"{total_divergent} statcheck-divergent, {total_failed} converter failure(s), "
        f"{total_errors} quark error(s) ==="
    )

    if not args.keep_work and not args.work_dir:
        shutil.rmtree(work_dir, ignore_errors=True)

    return 0 if total_published > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
