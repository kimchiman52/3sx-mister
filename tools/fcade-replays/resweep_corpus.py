#!/usr/bin/env python3
"""Re-run statcheck over an already-harvested corpus and write the verdicts back.

Why this exists separately from each corpus's own `analyze.py`: `analyze.py`
re-derives the expensive classification (a full `zero_run_decode` pass per
archive for `wu_operator` / `bg_w.stage`, plus `probe_match_start`) as well as
the verdict.  That classification is a property of the ARCHIVE and never
changes; only the verdict moves when the engine or the oracle moves.  So a
re-sweep reads the existing `manifest.json`, keeps every classification field
verbatim, and refreshes just the `statcheck` sub-record.

It also fixes the reproducibility gap that made the old manifests unverifiable:
**every segment's full statcheck output is stored on disk**, under
`<root>/sweeps/<tag>/logs/`, so a later reader can confirm a verdict without
owning the binary that produced it.  `<root>/sweeps/<tag>/sweep.json` records
the binary path, its size/mtime, the repo commit, and the per-segment result.

Usage:

    python3 tools/fcade-replays/resweep_corpus.py <corpus-root> --tag <name> \
        [--statcheck PATH] [--jobs N] [--timeout SECS] [--no-manifest]

`--no-manifest` runs the sweep and writes `sweeps/<tag>/` but leaves
`manifest.json` / `manifest.tsv` alone (use it for a control run whose verdicts
should not become the corpus's recorded state).

ALWAYS passes `--headless`: without it a FAILING statcheck raises SIGSTOP and
parks in process state `TN` forever.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_STATCHECK = REPO / 'build/statcheck-verify/3S-ARM.app/Contents/MacOS/3S-ARM'

FAIL_RE = re.compile(r'^(/[^\s:]+\.c):(\d+): (.+)$')
FRAME_RE = re.compile(r'^statcheck: FAIL at archive frame (\d+)')
SEED_RE = re.compile(r'^statcheck-seed: (CLEAN|DIRTY) at seed frame (\d+)[\s\-(]*(\d+)?')
PASS_RE = re.compile(r'^statcheck: PASS')
VERDICTS = {0: 'pass', 1: 'divergent', 2: 'no-match', 3: 'cpu-player', 4: 'seed-gap'}


def run_one(archive: Path, statcheck: Path, log_path: Path, timeout: int) -> dict:
    env = dict(os.environ)
    env['SDL_VIDEODRIVER'] = 'dummy'
    env['SDL_AUDIODRIVER'] = 'dummy'
    t = time.time()
    try:
        p = subprocess.run([str(statcheck), '--ram-archive', str(archive), '--headless'],
                           capture_output=True, text=True, timeout=timeout, env=env)
        rc, out, err = p.returncode, p.stdout or '', p.stderr or ''
    except subprocess.TimeoutExpired as exc:
        rc = None
        out = exc.stdout.decode('utf-8', 'replace') if isinstance(exc.stdout, bytes) else (exc.stdout or '')
        err = exc.stderr.decode('utf-8', 'replace') if isinstance(exc.stderr, bytes) else (exc.stderr or '')
    secs = round(time.time() - t, 1)

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(
        f"# statcheck {statcheck}\n# archive {archive}\n# rc {rc}\n# secs {secs}\n"
        f"--- stdout ---\n{out}\n--- stderr ---\n{err}\n")

    assert_line = fail_frame = seed = seed_fields = pass_line = None
    seed_lines = []
    for line in (out + '\n' + err).splitlines():
        ls = line.strip()
        m = FAIL_RE.match(ls)
        if m:
            assert_line = ls
        m2 = FRAME_RE.match(ls)
        if m2:
            fail_frame = int(m2.group(1))
        m3 = SEED_RE.match(ls)
        if m3:
            seed = m3.group(1)
            seed_fields = int(m3.group(3)) if m3.group(3) else 0
        if ls.startswith('statcheck-seed:'):
            seed_lines.append(ls)
        if PASS_RE.match(ls):
            pass_line = ls
    return {"rc": rc, "verdict": 'timeout' if rc is None else VERDICTS.get(rc, f'rc{rc}'),
            "assert": assert_line, "fail_frame": fail_frame, "seed": seed,
            "seed_fields": seed_fields, "seed_lines": seed_lines,
            "pass_line": pass_line, "secs": secs, "log": None}


def tsv_row(r: dict, cols: list) -> list:
    ops = ";".join(f"({a['pair'][0]},{a['pair'][1]})x{a['last'] - a['first'] + 1}"
                   for a in (r.get('operator_runs') or []))
    sc = r['statcheck']
    by_col = {
        'quarkid': r['quarkid'],
        'game_index': r['game_index'],
        'players': "/".join(x or '' for x in (r.get('players') or [])),
        'characters': "/".join(r.get('character_names') or [str(c) for c in (r.get('characters') or [])]),
        'stage': r.get('stage_name') or '',
        'frame_count': r.get('frame_count'),
        'eligibility': r.get('eligibility'),
        'wu_operator': tuple(r.get('wu_operator')) if r.get('wu_operator') else '',
        'operator_pairs': ops,
        'statcheck_rc': sc['rc'],
        'statcheck_verdict': sc['verdict'],
        'fail_frame': sc['fail_frame'],
        'seed': sc.get('seed') or '',
        'assert': sc.get('assert') or '',
        'published': r.get('published'),
        'archive': r['archive'],
        'log': sc.get('log') or '',
    }
    return [str(by_col.get(c, '')) for c in cols]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('root', type=Path)
    ap.add_argument('--tag', required=True, help='name for this sweep under <root>/sweeps/')
    ap.add_argument('--statcheck', type=Path, default=DEFAULT_STATCHECK)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--no-manifest', action='store_true')
    args = ap.parse_args()

    root: Path = args.root
    manifest = json.loads((root / 'manifest.json').read_text())
    records = manifest['records']
    sweep_dir = root / 'sweeps' / args.tag
    (sweep_dir / 'logs').mkdir(parents=True, exist_ok=True)

    st = args.statcheck.stat()
    commit = subprocess.run(['git', '-C', str(REPO), 'rev-parse', 'HEAD'],
                            capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(['git', '-C', str(REPO), 'status', '--porcelain'],
                           capture_output=True, text=True).stdout.strip()

    def work(r):
        name = f"{r['quarkid']}_game_{r['game_index']}.log"
        res = run_one(Path(r['archive']), args.statcheck, sweep_dir / 'logs' / name, args.timeout)
        res['log'] = f"sweeps/{args.tag}/logs/{name}"
        return r, res

    t0 = time.time()
    done = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for r, res in ex.map(work, records):
            prev = (r.get('statcheck') or {}).get('verdict')
            r['statcheck'] = res
            done += 1
            moved = '' if prev == res['verdict'] else f"   MOVED {prev} -> {res['verdict']}"
            print(f"[{done}/{len(records)}] {r['quarkid']} game_{r['game_index']}: "
                  f"{res['verdict']}{moved}", flush=True)

    counts = {}
    for r in records:
        counts[r['statcheck']['verdict']] = counts.get(r['statcheck']['verdict'], 0) + 1
    eligible = [r for r in records if r.get('eligible')]
    elig_counts = {}
    for r in eligible:
        elig_counts[r['statcheck']['verdict']] = elig_counts.get(r['statcheck']['verdict'], 0) + 1

    sweep_meta = {
        "tag": args.tag,
        "generated_at": int(time.time() * 1000),
        "elapsed_secs": round(time.time() - t0, 1),
        "repo": str(REPO), "commit": commit, "worktree_dirty": bool(dirty),
        "statcheck_binary": str(args.statcheck),
        "statcheck_size": st.st_size, "statcheck_mtime": int(st.st_mtime),
        "segments": len(records), "eligible": len(eligible),
        "verdicts": counts, "eligible_verdicts": elig_counts,
        "records": [{"quarkid": r['quarkid'], "game_index": r['game_index'],
                     "eligible": r.get('eligible'), "eligibility": r.get('eligibility'),
                     "archive": r['archive'], **r['statcheck']} for r in records],
    }
    (sweep_dir / 'sweep.json').write_text(json.dumps(sweep_meta, indent=1) + "\n")

    if not args.no_manifest:
        manifest['statcheck_binary'] = str(args.statcheck)
        manifest['last_sweep'] = {k: v for k, v in sweep_meta.items() if k != 'records'}
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=1) + "\n")

        header = (root / 'manifest.tsv').read_text().splitlines()[0].split('\t')
        if 'log' not in header:
            header.append('log')
        with open(root / 'manifest.tsv', 'w') as f:
            f.write("\t".join(header) + "\n")
            for r in records:
                f.write("\t".join(tsv_row(r, header)) + "\n")

    print(f"\n{root.name} [{args.tag}]: {len(records)} segments, "
          f"{len(eligible)} eligible -> {elig_counts}")
    print(f"all segments -> {counts}")
    print(f"wrote {sweep_dir/'sweep.json'} + {len(records)} logs"
          + ("" if args.no_manifest else f", refreshed {root/'manifest.json'} / manifest.tsv"))
    return 0


if __name__ == '__main__':
    sys.exit(main())
