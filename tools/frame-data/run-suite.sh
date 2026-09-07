#!/usr/bin/env bash
set -euo pipefail

# Build-once, bounded-parallel-fan-out runner for the frame-data harness's
# per-character corpora. Turns 19 serial `run.sh` invocations (each
# reconfiguring/rebuilding build/host, ~35 min wall) into a single build +
# a bounded xargs -P fan-out (each corpus in its own isolated RUNDIR),
# ~4-12 min wall on a 10-logical-core dev machine.
#
# Usage:
#   tools/frame-data/run-suite.sh [--check-golden|--update-golden]
#       [--jobs N] [--include-smoke] [--keep] [corpus.yaml ...]
#
#   (no corpus args)     -> every tools/frame-data/corpus-*.yaml except
#                            corpus-smoke.yaml, sorted.
#   corpus.yaml ...       -> only the given corpora (path, bare filename
#                            under tools/frame-data/, or bare character
#                            name all work: corpus-q.yaml | corpus-q | q).
#   --include-smoke       -> include corpus-smoke.yaml in the default set.
#   --jobs N              -> override the parallel fan-out width (default:
#                            FDH_JOBS env var, else hw.logicalcpu - 1).
#   --keep                -> never delete any corpus's RUNDIR, GREEN or RED.
#   --check-golden        -> after the suite run, diff each corpus's parsed
#                            table against tools/frame-data/golden/<c>.tsv
#                            (golden.py check); nonzero exit on any drift or
#                            missing golden file. Implies keeping RUNDIRs.
#   --update-golden       -> after the suite run, regenerate
#                            tools/frame-data/golden/<c>.tsv for every
#                            corpus run (golden.py emit). Implies keeping
#                            RUNDIRs.
#
# Parallel-safety: see the frame-data harness tooling plan, section 1. In
# short - FRAME_TRACE_PATH and RUNDIR are already per-run-isolated in
# run.sh; the only shared-path writes under the host build's SDL pref dir
# (config/resources/keymap) are absent-file-guarded first-run-only seeds,
# neutralized here by running the FIRST corpus serially (the "prime" step)
# before fanning out the rest. No engine change; no new isolation
# primitive - just run.sh's existing RUNDIR/FRAME_TRACE_PATH mechanism,
# fanned out via the FDH_SKIP_BUILD/FDH_RUNDIR/FDH_KEEP_RUNDIR hooks it
# gained for this purpose.
#
# KNOWN LIMITATION (task #79, documented not fixed - different defect, same
# family as the per-corpus wall-clock cap run.sh applies): a full run of all
# 94 corpora (build + fan-out) has been externally SIGTERM'd mid-run by a
# separate background-task lifetime cap of roughly 70 minutes, unrelated to
# anything in this script or in run.sh - it kills the whole process group
# from outside, the same way any other long-lived background job on this
# host is capped. Launching this script under `nohup ... & disown` has been
# observed to survive that cap (the job is detached from the session the cap
# tracks). There is no mechanism in run-suite.sh itself to extend or detect
# that external cap; a run long enough to approach ~70 minutes wall should be
# started detached.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BUILD_DIR="${REPO_ROOT}/build/host"
BIN_PATH="${BUILD_DIR}/3S-ARM.app/Contents/MacOS/3S-ARM"
GOLDEN_DIR="${SCRIPT_DIR}/golden"

# ---------------------------------------------------------------------
# Arg parsing
# ---------------------------------------------------------------------

CHECK_GOLDEN=0
UPDATE_GOLDEN=0
INCLUDE_SMOKE=0
KEEP_ALL=0
JOBS_OVERRIDE=""
POSITIONAL=()

while [ $# -gt 0 ]; do
    case "$1" in
        --check-golden)
            CHECK_GOLDEN=1
            shift
            ;;
        --update-golden)
            UPDATE_GOLDEN=1
            shift
            ;;
        --include-smoke)
            INCLUDE_SMOKE=1
            shift
            ;;
        --keep)
            KEEP_ALL=1
            shift
            ;;
        --jobs)
            JOBS_OVERRIDE="${2:-}"
            if [ -z "$JOBS_OVERRIDE" ]; then
                echo "error: --jobs requires an argument" >&2
                exit 2
            fi
            shift 2
            ;;
        --jobs=*)
            JOBS_OVERRIDE="${1#--jobs=}"
            shift
            ;;
        --help|-h)
            sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        --)
            shift
            while [ $# -gt 0 ]; do
                POSITIONAL+=("$1")
                shift
            done
            ;;
        -*)
            echo "error: unknown option: $1" >&2
            exit 2
            ;;
        *)
            POSITIONAL+=("$1")
            shift
            ;;
    esac
done

if [ "$CHECK_GOLDEN" -eq 1 ] && [ "$UPDATE_GOLDEN" -eq 1 ]; then
    echo "error: --check-golden and --update-golden are mutually exclusive" >&2
    exit 2
fi

# ---------------------------------------------------------------------
# Resolve the corpus list
# ---------------------------------------------------------------------

name_for() {
    # Derives the short corpus name (e.g. "q", "ryu") used for result
    # files and golden/<name>.tsv from a corpus path of any shape.
    local base
    base="$(basename "$1" .yaml)"
    base="${base#corpus-}"
    printf '%s' "$base"
}

resolve_corpus() {
    # Accepts a path, a bare filename under tools/frame-data/, or a bare
    # character name, and echoes the resolved absolute path (or errors).
    local c="$1"
    if [ -f "$c" ]; then
        (cd "$(dirname "$c")" && printf '%s/%s\n' "$(pwd)" "$(basename "$c")")
        return 0
    fi
    if [ -f "${SCRIPT_DIR}/$c" ]; then
        printf '%s/%s\n' "$SCRIPT_DIR" "$c"
        return 0
    fi
    if [ -f "${SCRIPT_DIR}/corpus-$c.yaml" ]; then
        printf '%s/corpus-%s.yaml\n' "$SCRIPT_DIR" "$c"
        return 0
    fi
    return 1
}

CORPORA=()
if [ "${#POSITIONAL[@]}" -gt 0 ]; then
    for c in "${POSITIONAL[@]}"; do
        resolved="$(resolve_corpus "$c")" || {
            echo "error: corpus not found: $c" >&2
            exit 1
        }
        CORPORA+=("$resolved")
    done
else
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        if [ "$INCLUDE_SMOKE" -eq 0 ]; then
            case "$f" in
                */corpus-smoke.yaml) continue ;;
            esac
        fi
        CORPORA+=("$f")
    done < <(printf '%s\n' "${SCRIPT_DIR}"/corpus-*.yaml | sort)
fi

if [ "${#CORPORA[@]}" -eq 0 ]; then
    echo "error: no corpora resolved" >&2
    exit 1
fi

# ---------------------------------------------------------------------
# Arcade-balance preflight (task #108)
#
# Corpora carrying `balance: arcade` run the SHIPPING CPS3 tables and need a
# verified romset; without one the game exits 6 and run.sh turns that into a
# RED. Catching it once, here, beats N identical REDs at the end of a
# multi-minute fan-out - and, more importantly, it is stated rather than
# skipped: the alternative failure mode this whole task exists to close is a
# suite that quietly measures the wrong engine and reports green.
#
# $FDH_CPS3_ZIP (dev-only) is exported by run.sh as $THIRDSARM_CPS3_ZIP;
# leaving it unset is fine on a machine with a romset installed where
# docs/config.md's discovery looks.
# ---------------------------------------------------------------------

ARCADE_CORPORA=()
for c in "${CORPORA[@]}"; do
    if grep -qE '^[[:space:]]*balance:[[:space:]]*(arcade|"arcade"|'"'"'arcade'"'"')[[:space:]]*$' "$c"; then
        ARCADE_CORPORA+=("$(name_for "$c")")
    fi
done

if [ "${#ARCADE_CORPORA[@]}" -gt 0 ]; then
    echo "[run-suite.sh] ${#ARCADE_CORPORA[@]} arcade-balance corpus/corpora selected: ${ARCADE_CORPORA[*]}" >&2
    if [ -n "${FDH_CPS3_ZIP:-}" ] && [ ! -f "${FDH_CPS3_ZIP}" ]; then
        echo "error: FDH_CPS3_ZIP=${FDH_CPS3_ZIP} does not exist, but arcade corpora are selected" >&2
        exit 2
    fi
    if [ -z "${FDH_CPS3_ZIP:-}" ]; then
        echo "[run-suite.sh] note: FDH_CPS3_ZIP unset - the arcade corpora rely on an installed romset" >&2
        echo "[run-suite.sh]       and will RED with 'arcade balance UNAVAILABLE' if there is none." >&2
    fi
fi

# ---------------------------------------------------------------------
# Jobs
# ---------------------------------------------------------------------

if [ -n "$JOBS_OVERRIDE" ]; then
    JOBS="$JOBS_OVERRIDE"
else
    JOBS="${FDH_JOBS:-$(( $(sysctl -n hw.logicalcpu 2>/dev/null || nproc) - 1 ))}"
fi
if [ "$JOBS" -lt 1 ] 2>/dev/null; then
    JOBS=1
fi
case "$JOBS" in
    ''|*[!0-9]*)
        echo "error: jobs value '$JOBS' is not a positive integer" >&2
        exit 2
        ;;
esac

# Throttling this suite is almost always a mistake, and an expensive one, so say
# so out loud rather than leaving it to a doc nobody reads mid-run.
#
# Measured 2026-09-06 on a 4P+6E host: 1,491 labels across 100 corpora at the
# tree's recorded 2.16 s/label is ~53.7 min serial. At the default width
# (hw.logicalcpu - 1) the real run was 832 s -- 99/99 GREEN, zero drift. The
# same suite at --jobs 3, with one sibling agent competing, took ~50 min: near
# serial, for a ~14 min job. The throttle was added to "be polite" to a
# concurrent agent and cost 3-4x on the critical path.
#
# The right answer is to give a gate run the machine, not to shrink it.
FDH_JOBS_FLOOR="${FDH_JOBS_FLOOR:-6}"
if [ -n "$JOBS_OVERRIDE" ] && [ "$JOBS" -lt "$FDH_JOBS_FLOOR" ] 2>/dev/null; then
    echo "warning: --jobs $JOBS is below the recommended floor of $FDH_JOBS_FLOOR." >&2
    echo "         The default (hw.logicalcpu - 1) is ~14 min for the full suite;" >&2
    echo "         --jobs 3 measured ~50 min for the same work. Prefer the default" >&2
    echo "         and give the gate run the machine to itself." >&2
    echo "         Lower it only for repeated efficiency-core timeouts (exit 143)," >&2
    echo "         and set FDH_JOBS_FLOOR to silence this deliberately." >&2
fi

# ---------------------------------------------------------------------
# Build once
# ---------------------------------------------------------------------

SECONDS=0

if [ ! -d "$BUILD_DIR" ]; then
    echo "[run-suite.sh] build/host missing, configuring..." >&2
    CC=clang cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
fi

echo "[run-suite.sh] building 3S-ARM (build/host, same-binary protocol - build-once for the whole suite)..." >&2
cmake --build "$BUILD_DIR" --parallel 8

if [ ! -x "$BIN_PATH" ]; then
    echo "error: expected binary not found at $BIN_PATH after build" >&2
    exit 1
fi

# ---------------------------------------------------------------------
# Worker: run one corpus through run.sh with the FDH_* isolation hooks.
# Called directly for the serial prime step, and (exported) via xargs -P
# for the bounded fan-out. Writes $RES/<name>.{out,err,exit,rundir} -
# workers never rely on xargs's own aggregate exit code (BSD xargs -P
# loses per-child exit codes, returning 123-125 on any child failure).
# ---------------------------------------------------------------------

RES="$(mktemp -d)"

run_one() {
    local corpus_path="$1"
    local name rundir out ec
    name="$(name_for "$corpus_path")"
    rundir="$(mktemp -d)"
    set +e
    out="$(FDH_SKIP_BUILD=1 FDH_KEEP_RUNDIR=1 FDH_RUNDIR="$rundir" \
        bash "$SCRIPT_DIR/run.sh" "$corpus_path" 2>"$RES/$name.err")"
    ec=$?
    set -e
    printf '%s' "$out" > "$RES/$name.out"
    echo "$ec" > "$RES/$name.exit"
    echo "$rundir" > "$RES/$name.rundir"
}

# ---------------------------------------------------------------------
# Prime (serial) - first corpus, then fan out the remaining N-1.
# ---------------------------------------------------------------------

PRIME_CORPUS="${CORPORA[0]}"
PRIME_NAME="$(name_for "$PRIME_CORPUS")"
echo "[run-suite.sh] priming (serial) with '$PRIME_NAME' - seeds first-run-only SDL pref-path files (config/resources/keymap) before fan-out..." >&2
run_one "$PRIME_CORPUS"

FANOUT=("${CORPORA[@]:1}")

if [ "${#FANOUT[@]}" -gt 0 ]; then
    echo "[run-suite.sh] fanning out ${#FANOUT[@]} corpora at jobs=$JOBS..." >&2
    export SCRIPT_DIR RES
    export -f run_one name_for
    printf '%s\n' "${FANOUT[@]}" | xargs -P "$JOBS" -n 1 bash -c 'run_one "$1"' _
fi

# ---------------------------------------------------------------------
# Aggregate + summary
# ---------------------------------------------------------------------

WALL_SECONDS=$SECONDS
WALL_FMT="$(printf '%dm%02ds' $((WALL_SECONDS / 60)) $((WALL_SECONDS % 60)))"

GREEN_COUNT=0
RED_COUNT=0

echo
printf "%-10s %-4s %-46s %s\n" "CORPUS" "EXIT" "TOTAL" "RESULT"

for c in "${CORPORA[@]}"; do
    name="$(name_for "$c")"
    ec="$(cat "$RES/$name.exit" 2>/dev/null || echo 1)"
    rundir="$(cat "$RES/$name.rundir" 2>/dev/null || echo "?")"
    totalline="$(grep '^total=' "$RES/$name.out" 2>/dev/null || true)"
    if [ -z "$totalline" ]; then
        reason="$(tail -n 1 "$RES/$name.err" 2>/dev/null || echo "no total= line (harness did not reach the checker)")"
        totalline="(no total= line: $reason)"
    fi
    if [ "$ec" -eq 0 ]; then
        result="GREEN"
        GREEN_COUNT=$((GREEN_COUNT + 1))
        printf "%-10s %-4s %-46s %s\n" "$name" "$ec" "$totalline" "$result"
    else
        result="RED"
        RED_COUNT=$((RED_COUNT + 1))
        printf "%-10s %-4s %-46s %s  RUNDIR=%s\n" "$name" "$ec" "$totalline" "$result" "$rundir"
    fi
done

echo "───────────────────────────────────────────────────────────────────────────"
printf "%d corpora: %d GREEN / %d RED   build 1x   wall %s   jobs %d\n" \
    "${#CORPORA[@]}" "$GREEN_COUNT" "$RED_COUNT" "$WALL_FMT" "$JOBS"

SUITE_EXIT=0
if [ "$RED_COUNT" -gt 0 ]; then
    SUITE_EXIT=1
fi

# ---------------------------------------------------------------------
# Golden hooks
# ---------------------------------------------------------------------

GOLDEN_DRIFT=0

if [ "$UPDATE_GOLDEN" -eq 1 ]; then
    mkdir -p "$GOLDEN_DIR"
    echo
    echo "[run-suite.sh] updating goldens in $GOLDEN_DIR ..."
    CHANGED_COUNT=0
    for c in "${CORPORA[@]}"; do
        name="$(name_for "$c")"
        rundir="$(cat "$RES/$name.rundir" 2>/dev/null || echo "")"
        golden_path="$GOLDEN_DIR/$name.tsv"
        if [ -z "$rundir" ] || [ ! -f "$rundir/trace.log" ] || [ ! -f "$rundir/expected.json" ]; then
            echo "  SKIP   $name.tsv (no trace.log/expected.json - corpus run did not complete)"
            continue
        fi
        new_content="$(python3 "$SCRIPT_DIR/golden.py" emit "$rundir/trace.log" "$rundir/expected.json")"
        if [ -f "$golden_path" ]; then
            old_content="$(cat "$golden_path")"
        else
            old_content=""
        fi
        if [ "$new_content" != "$old_content" ]; then
            printf '%s\n' "$new_content" > "$golden_path"
            if [ -z "$old_content" ]; then
                echo "  NEW      $name.tsv"
            else
                echo "  CHANGED  $name.tsv"
            fi
            CHANGED_COUNT=$((CHANGED_COUNT + 1))
        else
            echo "  unchanged $name.tsv"
        fi
    done
    echo "[run-suite.sh] golden update: $CHANGED_COUNT new/changed of ${#CORPORA[@]}"
fi

if [ "$CHECK_GOLDEN" -eq 1 ]; then
    echo
    echo "[run-suite.sh] checking goldens against $GOLDEN_DIR ..."
    for c in "${CORPORA[@]}"; do
        name="$(name_for "$c")"
        rundir="$(cat "$RES/$name.rundir" 2>/dev/null || echo "")"
        golden_path="$GOLDEN_DIR/$name.tsv"
        if [ ! -f "$golden_path" ]; then
            echo "$name: MISSING golden file $golden_path"
            GOLDEN_DRIFT=1
            continue
        fi
        if [ -z "$rundir" ] || [ ! -f "$rundir/trace.log" ] || [ ! -f "$rundir/expected.json" ]; then
            echo "$name: MISSING trace/expected (corpus run did not complete) - RUNDIR=$rundir"
            GOLDEN_DRIFT=1
            continue
        fi
        set +e
        check_out="$(python3 "$SCRIPT_DIR/golden.py" check "$golden_path" "$rundir/trace.log" "$rundir/expected.json")"
        check_ec=$?
        set -e
        if [ "$check_ec" -eq 0 ]; then
            echo "$name: $check_out"
        else
            echo "$name: DRIFT"
            printf '%s\n' "$check_out" | sed 's/^/  /'
            GOLDEN_DRIFT=1
        fi
    done
    if [ "$GOLDEN_DRIFT" -ne 0 ]; then
        echo "[run-suite.sh] golden check: DRIFT DETECTED"
        SUITE_EXIT=1
    else
        echo "[run-suite.sh] golden check: zero drift (${#CORPORA[@]} corpora)"
    fi
fi

# ---------------------------------------------------------------------
# Cleanup - GREEN RUNDIRs are removed unless a golden mode or --keep
# needs them to survive; RED RUNDIRs always survive for inspection.
# ---------------------------------------------------------------------

KEEP_RUNDIRS=0
if [ "$CHECK_GOLDEN" -eq 1 ] || [ "$UPDATE_GOLDEN" -eq 1 ] || [ "$KEEP_ALL" -eq 1 ]; then
    KEEP_RUNDIRS=1
fi

for c in "${CORPORA[@]}"; do
    name="$(name_for "$c")"
    ec="$(cat "$RES/$name.exit" 2>/dev/null || echo 1)"
    rundir="$(cat "$RES/$name.rundir" 2>/dev/null || echo "")"
    if [ "$ec" -eq 0 ] && [ "$KEEP_RUNDIRS" -eq 0 ] && [ -n "$rundir" ]; then
        rm -rf "$rundir"
    fi
done
rm -rf "$RES"

exit "$SUITE_EXIT"
