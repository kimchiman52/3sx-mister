#!/usr/bin/env bash
#
# run-gates -- the declared verification gate set.
#
# WHY THIS FILE IS COMMITTED
# ==========================
#
# The runner this replaces was lane-private scratch (`gate.sh`, deleted by
# ed37cb42 as "throwaway task #103 tooling"). That is precisely the problem it
# was meant to solve: the gate set lived in one agent's temp file and in prose
# inside task briefs, so "the gates" meant whatever the last person happened to
# run. Two defects have now been traced to that shape:
#
#   #76   parked work built only with NETPLAY_TEST_HOOKS=ON and failed the
#         SHIPPED config with three errors, because nothing gated the shipped
#         config.
#   #106  the ed37cb42 merge passed nine harnesses, the shipped-config build
#         and frame-data 94 GREEN, then failed to cross-compile for ARM,
#         because nothing gated the ARM target.
#
# Same shape both times: a configuration nobody gates on goes quietly broken
# while every gate that does run stays green. So the gate set is written down
# here, and -- this is the load-bearing part -- a gate that did NOT run is
# named in the summary rather than omitted from it. A green line that silently
# excludes the ARM target is how #106 happened.
#
# USAGE
#   tools/gates/run-gates.sh              # fast set (no Docker, ~2-4 min)
#   tools/gates/run-gates.sh --arm        # also cross-compile for ARM (slow)
#   tools/gates/run-gates.sh --list       # print the gate set and exit
#
# Exit codes: 0 all run gates green, 1 at least one red, 2 harness error.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${ROOT_DIR}"

run_arm="${GATES_ARM:-0}"
jobs="${GATES_JOBS:-8}"
out_dir="${GATES_OUT:-$(mktemp -d)}"
list_only=0

while [ $# -gt 0 ]; do
    case "$1" in
    --arm) run_arm=1; shift ;;
    --no-arm) run_arm=0; shift ;;
    --jobs) jobs="$2"; shift 2 ;;
    --out) out_dir="$2"; shift 2 ;;
    --list) list_only=1; shift ;;
    -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
mkdir -p "${out_dir}"

GATES=(
    "shipped-config-build|build the SHIPPED config (ENABLE_NETPLAY=ON, NETPLAY_TEST_HOOKS=OFF) -- #76"
    "nptest-build|build the hooks-ON test config the harnesses need"
    "netplay-harnesses|run every --test-* harness with true exit codes"
    "quick-training|the OSD Quick Training sequence (first jump, mid-match re-jump) and the instant-jump spike, each against a SEEDED training config"
    "rendezvous-protocol|node tools/rendezvous-server/__test_protocol.js"
    "key-rate-budget|server per-key cap vs the CLIENT cadences it is derived from -- #123"
    "reclaim-window|slot-reclaim staleness vs the CLIENT cadences it is derived from -- #130"
    "constant-time-compare|the punch-token compare's SHAPE, which no unit test can see -- #132"
    "host-diagnostic-parity|diagnostics the host's fortified libc headers hide -- #106"
    "doc-citation-baselines|tools/doc-citations/check_baselines.py (breach AND slack)"
    "arm-cross-build|cross-compile the shipped config for ARM -- #106 (needs --arm)"
)

if [ "${list_only}" -eq 1 ]; then
    printf '%s\n' "${GATES[@]}" | sed 's/|/\t/'
    exit 0
fi

declare -a NAMES=() STATES=()
FAILED=0

record() {  # record <name> <state>
    NAMES+=("$1"); STATES+=("$2")
    case "$2" in RED|ERROR) FAILED=1 ;; esac
    printf '  %-24s %s\n' "$1" "$2"
}

echo "############ GATE RUN ############"
echo "logs: ${out_dir}"
echo

# ---------------------------------------------------------------------------
# 1/2. The two host configurations. Configure as well as build: a gate that
#      assumes somebody already ran cmake is a gate that silently tests a stale
#      configuration, which is the same class of defect as not running at all.
# ---------------------------------------------------------------------------
echo "=== host builds ==="
cmake -S . -B build/host-release -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_NETPLAY=ON -DNETPLAY_TEST_HOOKS=OFF \
    > "${out_dir}/configure-release.log" 2>&1 \
 && cmake --build build/host-release -j "${jobs}" \
    > "${out_dir}/build-release.log" 2>&1
rc=$?
[ $rc -eq 0 ] && record shipped-config-build GREEN || {
    record shipped-config-build RED
    grep -E "error:" "${out_dir}/build-release.log" | head -15
}

cmake -S . -B build/host-nptest -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_NETPLAY=ON -DNETPLAY_TEST_HOOKS=ON \
    "-DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS" \
    > "${out_dir}/configure-nptest.log" 2>&1 \
 && cmake --build build/host-nptest -j "${jobs}" \
    > "${out_dir}/build-nptest.log" 2>&1
np_rc=$?
[ $np_rc -eq 0 ] && record nptest-build GREEN || {
    record nptest-build RED
    grep -E "error:" "${out_dir}/build-nptest.log" | head -15
}
echo

# ---------------------------------------------------------------------------
# The resources/ directory every hermetic home below needs a link to.
#
# THIRDSARM_HOME relocates resources/, and a run that reaches the resource flow
# without one does not fail, it HANGS: src/port/resources.c ->
# Resources_RunResourceCopyingFlow() blocks in a modal dialog that
# SDL_VIDEODRIVER=dummy makes invisible (docs/building.md, "Running a host
# build outside your normal home directory").
#
# The discovered harnesses below all exit before that flow today, which is why
# their homes went without a link and nothing hung. That is a property of the
# current sixteen, not of the rule that creates their homes -- one harness that
# gets as far as the resource check turns this gate from red into a wedged run.
# Link it for everyone; a missing resources/ is only fatal to the gate that
# actually needs it (Quick Training, below).
# ---------------------------------------------------------------------------
qt_res=""
for qt_cand in \
    "${HOME}/Library/Application Support/CrowdedStreet/3S-ARM/resources" \
    "${XDG_DATA_HOME:-${HOME}/.local/share}/CrowdedStreet/3S-ARM/resources"; do
    [ -d "${qt_cand}" ] && { qt_res="${qt_cand}"; break; }
done

link_resources() {  # link_resources <home>
    [ -n "${qt_res}" ] && ln -sfn "${qt_res}" "$1/resources"
}

# ---------------------------------------------------------------------------
# 3. Harnesses. Discovered from src/args.c rather than hardcoded, so a new
#    harness joins the gate by existing rather than by somebody remembering to
#    add it here.
# ---------------------------------------------------------------------------
echo "=== netplay harnesses ==="
BIN=""
for cand in build/host-nptest/3S-ARM.app/Contents/MacOS/3S-ARM \
            build/host-nptest/3s-arm build/host-nptest/3S-ARM; do
    [ -x "${cand}" ] && { BIN="${cand}"; break; }
done

if [ "${np_rc}" -ne 0 ] || [ -z "${BIN}" ]; then
    record netplay-harnesses ERROR
    echo "  no test binary; harnesses cannot run"
else
    # DISCOVERY, not a hardcoded list, so a new harness joins this gate by
    # existing rather than by somebody remembering to add it here.
    #
    # The discriminator is precise on purpose. `grep '"test-[a-z0-9-]+"'` over
    # args.c looks like it works and does not: it returns 23 flags, because most
    # `--test-*` options CONFIGURE the interactive test runner (--test-stage
    # takes an integer, --test-p1-character takes a name, --test-enable launches
    # the game) rather than being self-contained harnesses. Running those as if
    # they were harnesses starts the game and hangs the gate -- observed.
    #
    # A harness is an OPT_BOOLEAN whose help text says it runs and EXITS. That
    # is exactly the nine the pre-existing lane-private runner listed by hand.
    #
    # NOT mapfile: macOS ships bash 3.2, which has no `mapfile`. It failed
    # silently, and under `set -u` the next line died on an unbound HARNESSES,
    # so this gate recorded NO state and the summary still printed GREEN -- the
    # very defect this runner exists to prevent, reproduced inside it. The
    # completeness assertion at the bottom is the structural fix for that class;
    # this loop is the proximate one.
    HARNESSES=()
    while IFS= read -r h; do
        [ -n "${h}" ] && HARNESSES+=("${h}")
    done < <(python3 - src/args.c <<'DISCOVER'
import re, sys
src = open(sys.argv[1]).read()
pat = re.compile(
    r'OPT_BOOLEAN\(\s*0\s*,\s*"(test-[a-z0-9-]+)"\s*,\s*[^,]+,\s*((?:"[^"]*"\s*)+)',
    re.S)
for m in pat.finditer(src):
    if "and exit" in m.group(2).replace("\n", " "):
        print(m.group(1))
DISCOVER
)
    # A discovery rule that silently matches nothing would turn this gate into a
    # no-op that reports GREEN. Refuse instead.
    if [ "${#HARNESSES[@]}" -lt 8 ]; then
        record netplay-harnesses ERROR
        echo "  discovered ${#HARNESSES[@]} harnesses in src/args.c (expected >= 8)"
        echo "  -- refusing to pass vacuously on a discovery rule that broke"
    else
        h_failed=0
        for h in "${HARNESSES[@]}"; do
            # Task #125: each harness gets its OWN state directory.
            #
            # These binaries keep config, keymap and the netplay logs/ tree
            # under Paths_GetPrefPath(), which on the host build used to be
            # SDL_GetPrefPath() with no override -- one directory shared by
            # every process on the machine. The netplay session log is named
            # netplay-<utc_ms>.log, so two gate runs that start in the same
            # millisecond open the SAME FILE and interleave writes into it,
            # and --test-connect-observability then reads back whichever
            # netplay-*.log is newest, which is somebody else's. Measured
            # with four concurrent runs: three opened
            # netplay-1788111677761.log and all three validated against
            # netplay-1788111677763.log. That is #125, and it is not
            # specific to test6-byte-budget -- test4-mt-sink failed the same
            # way, and the end-of-run cleanup could remove a live log
            # belonging to another process.
            #
            # THIRDSARM_HOME is honoured on every port as of #125
            # (src/port/paths.c), so one env var makes the whole run
            # hermetic. $$ + the harness name keeps concurrent RUNS apart
            # too, not just concurrent harnesses within one run.
            h_home="${out_dir}/home/$$-${h}"
            mkdir -p "${h_home}"
            link_resources "${h_home}"
            THIRDSARM_HOME="${h_home}" "./${BIN}" "--${h}" > "${out_dir}/${h}.log" 2>&1
            rc=$?
            note=""
            # exit 2 + "not compiled in" is a MISBUILD, never a pass.
            if grep -q "not compiled in" "${out_dir}/${h}.log"; then
                note="  <<< NOT COMPILED IN (misbuild, not a pass)"
            fi
            printf '    %-34s exit=%-3s%s\n' "${h}" "${rc}" "${note}"
            { [ $rc -ne 0 ] || [ -n "${note}" ]; } && h_failed=1
        done
        echo "    harness count = ${#HARNESSES[@]}"
        [ "${h_failed}" -eq 0 ] && record netplay-harnesses GREEN \
                                || record netplay-harnesses RED
    fi
fi
echo

# ---------------------------------------------------------------------------
# 3b. Quick Training -- the OSD T[15] sequence, both fires, plus the spike.
#
# NOT part of the harness discovery above, and it cannot be: --test-quick-training
# is an OPT_INTEGER (it takes the prologue frame to fire at) and it needs
# --test-enable, so the "OPT_BOOLEAN whose help says it runs and exits" rule
# structurally cannot see it. That is how it came to have no routine runner at
# all -- and specifically why --test-quick-training-again had none: measured
# 2026-09-05, `qt_needs_teardown() { return false; }` leaves the single-fire
# run PASSING and is caught ONLY by the second fire, which tears a live match
# down and re-jumps.
#
# THE HOME IS SEEDED, AND THE SEED IS ITSELF AN ASSERTION. Two of the things
# this run must prove are properties of the persisted training config: that the
# match uses the stored characters/arts, and -- the one that cost the
# maintainer their settings -- that the run leaves the file alone. Both are
# vacuous against an absent or all-zero config, which is exactly how the
# original verification missed the bug. So the gate writes a config with
# non-zero contents and non-default characters, then diffs the file afterwards.
#
# `resources/` must be reachable inside the hermetic home or the child HANGS
# rather than fails -- see link_resources() above. These runs are the ones that
# actually get that far, so a missing resources/ is an ERROR here rather than
# something to discover as a wedged process.
# ---------------------------------------------------------------------------
echo "=== quick training ==="
if [ "${np_rc}" -ne 0 ] || [ -z "${BIN}" ]; then
    record quick-training ERROR
    echo "  no test binary; the Quick Training sequence cannot run"
elif [ -z "${qt_res}" ]; then
    record quick-training ERROR
    echo "  no resources/ directory to link into the hermetic home; a run that reaches"
    echo "  gameplay would BLOCK on the resource-copying dialog rather than fail"
else
    qt_failed=0

    # Deadline-bounded: a wedged child must fail this gate, not hang the run.
    # macOS ships no timeout(1), hence the poll. 300 s is ~15x the ~20 s the
    # two-fire run takes on host.
    qt_run() {  # qt_run <label> <log> <args...>
        local label="$1"; shift
        local log="$1"; shift
        local home="${out_dir}/home/$$-qt-${label}"
        local waited=0
        local pid
        local rc

        rm -rf "${home}"
        mkdir -p "${home}"
        link_resources "${home}"
        python3 - "${home}/training" <<'SEED'
import struct, sys
# TrainingConfigFile (src/port/config/training_config.c): magic "TRN1", v2,
# contents[2][2][7], cursor_x[2], cursor_y[2], super_arts[2], my_char[2].
# contents[0][0][0..3] = ACTION=JUMP, GUARD=RANDOM PARRYING, QUICK STAND=ON,
# STUN=NO GAIN -- all inside max_values, so none is clamped away on load.
c = [0] * 28
c[0], c[1], c[2], c[3] = 2, 5, 1, 2
blob = struct.pack("<II", 0x54524E31, 2) + struct.pack("28b", *c) \
     + struct.pack("2b", 0, 0) + struct.pack("2b", 0, 0) \
     + struct.pack("2b", 1, 2) + struct.pack("2B", 11, 0)
assert len(blob) == 44
open(sys.argv[1], "wb").write(blob)
SEED
        cp "${home}/training" "${home}/training.seeded"

        THIRDSARM_HOME="${home}" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
            "./${BIN}" "$@" > "${log}" 2>&1 &
        pid=$!

        while kill -0 "${pid}" 2>/dev/null; do
            if [ "${waited}" -ge 300 ]; then
                kill -9 "${pid}" 2>/dev/null
                wait "${pid}" 2>/dev/null
                printf '    %-34s TIMEOUT after %ss\n' "${label}" "${waited}"
                qt_failed=1
                return
            fi
            sleep 1
            waited=$((waited + 1))
        done
        wait "${pid}"; rc=$?

        printf '    %-34s exit=%-3s (%ss)\n' "${label}" "${rc}" "${waited}"
        [ "${rc}" -ne 0 ] && qt_failed=1

        # The data-loss assertion. menu.c -> Setup_NTr_Data() opens with
        # TrainingConfig_Save(), so a run that never LOADED the config flushes
        # zeros over it; that is the defect, and this is the direct proof.
        if ! cmp -s "${home}/training.seeded" "${home}/training"; then
            echo "    RED: ${label} rewrote the persisted training config" >&2
            echo "         seeded: $(od -An -tx1 "${home}/training.seeded" | tr -d ' \n')" >&2
            echo "         after:  $(od -An -tx1 "${home}/training" | tr -d ' \n')" >&2
            qt_failed=1
        fi
    }

    qt_run first-jump "${out_dir}/quick-training-1.log" \
        --test-enable --test-quick-training=60
    if ! grep -q "QUICK-TRAINING TEST PASS: 1 sequence(s)" "${out_dir}/quick-training-1.log"; then
        echo "    RED: no single-sequence PASS line (log: ${out_dir}/quick-training-1.log)" >&2
        qt_failed=1
    fi

    qt_run rejump-after-teardown "${out_dir}/quick-training-2.log" \
        --test-enable --test-quick-training=60 --test-quick-training-again=700
    if ! grep -q "QUICK-TRAINING TEST PASS: 2 sequence(s)" "${out_dir}/quick-training-2.log"; then
        echo "    RED: no two-sequence PASS line; the mid-match teardown + re-jump did not complete" >&2
        echo "         (log: ${out_dir}/quick-training-2.log)" >&2
        qt_failed=1
    fi

    # The SPIKE, run here for the config-diff above and nothing else.
    #
    # --test-instant-jump drives the same shared chain and reaches the same
    # menu.c -> Setup_NTr_Data(), but it picks its own characters and arts, so
    # its save wrote the HARNESS's selection over the maintainer's file --
    # measured 2026-09-05 on a seeded home: super_arts 01 02 -> 00 00, my_char
    # 0b 00 -> 03 02. Fixed at the harness (training_config.c ->
    # test_session_owns_training_config), and this is what holds the fix: it is
    # the only session-owning flag the discovery rule at the top of this file
    # structurally cannot see, so without a line here it has no runner at all.
    qt_run instant-jump-spike "${out_dir}/quick-training-3.log" \
        --test-enable --test-instant-jump
    if ! grep -q "SCENE-JUMP PASS:" "${out_dir}/quick-training-3.log"; then
        echo "    RED: no SCENE-JUMP PASS line (log: ${out_dir}/quick-training-3.log)" >&2
        qt_failed=1
    fi

    [ "${qt_failed}" -eq 0 ] && record quick-training GREEN || record quick-training RED
fi
echo

# ---------------------------------------------------------------------------
# 4. Rendezvous protocol.
# ---------------------------------------------------------------------------
echo "=== rendezvous protocol ==="
if [ -f tools/rendezvous-server/__test_protocol.js ] && command -v node >/dev/null; then
    node tools/rendezvous-server/__test_protocol.js > "${out_dir}/protocol.log" 2>&1
    [ $? -eq 0 ] && record rendezvous-protocol GREEN || {
        record rendezvous-protocol RED; tail -5 "${out_dir}/protocol.log"; }
else
    record rendezvous-protocol ERROR
    echo "  __test_protocol.js or node is missing"
fi
echo

# ---------------------------------------------------------------------------
# 4b. Cross-repo constant coupling -- task #123.
#
# rendezvous-server.js sizes KEY_RATE_LIMIT_PER_WINDOW from constants in the C
# CLIENT, and the two deploy INDEPENDENTLY: the server is a long-lived VPS
# process, the client ships in a release ZIP. No build can catch the drift (no
# TU and no module sees both a C literal and a JS const), and the symptom is
# not a crash -- it is a production room whose host liveness REGISTERs get
# rate-dropped until the code on the host's screen stops working. Same shape,
# and the same remedy, as tools/ldreq-timing/check_barrier_budget.py.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 4c. The punch-token compare's shape -- task #132.
#
# Stun_IsPunchPayload compares the room-code-derived token with an
# accumulator so a wrong guess costs the same time wherever it goes wrong.
# An early-exiting rewrite is BEHAVIOURALLY IDENTICAL -- same verdict for
# every input -- so --test-punch-predicates stays green against it while the
# token becomes recoverable byte by byte from off-path. A timing measurement
# over a 17-byte compare is noise, and a flaky gate is worse than none, so the
# property is checked where it is actually expressed: the shape of the loop.
# ---------------------------------------------------------------------------
echo "=== constant-time compare ==="
python3 tools/gates/check_constant_time_compare.py \
    > "${out_dir}/constant-time-compare.log" 2>&1
rc=$?
if [ $rc -eq 0 ]; then
    record constant-time-compare GREEN
elif [ $rc -eq 1 ]; then
    record constant-time-compare RED; cat "${out_dir}/constant-time-compare.log"
else
    record constant-time-compare ERROR; cat "${out_dir}/constant-time-compare.log"
fi
echo

echo "=== key-rate budget ==="
python3 tools/rendezvous-server/check_key_rate_budget.py \
    > "${out_dir}/key-rate-budget.log" 2>&1
case $? in
0) record key-rate-budget GREEN ;;
1) record key-rate-budget RED
   grep -E "FAIL|legit peak|required cap" "${out_dir}/key-rate-budget.log" | head -10 ;;
*) record key-rate-budget ERROR; tail -8 "${out_dir}/key-rate-budget.log" ;;
esac
echo

# ---------------------------------------------------------------------------
# 4c. Slot-reclaim staleness window -- task #130.
#
# Same cross-repo coupling as 4b and the same reason it cannot be an
# assertion: the port-reclaim staleness precondition is a multiple of the
# slot's OBSERVED cadence, and the multiple is derived from client constants
# (the host advertise interval, the in-race REGISTER cadence, the signalling
# leg) that ship in a release ZIP while the server runs on a VPS. Drift in
# either direction is silent and neither is a crash: too small reopens the
# live-slot hijack #130 closed, too large reopens the #105 retry lockout.
# ---------------------------------------------------------------------------
echo "=== reclaim window ==="
python3 tools/rendezvous-server/check_reclaim_window.py \
    > "${out_dir}/reclaim-window.log" 2>&1
case $? in
0) record reclaim-window GREEN ;;
1) record reclaim-window RED
   grep -E "FAIL|threshold|signal leg" "${out_dir}/reclaim-window.log" | head -10 ;;
*) record reclaim-window ERROR; tail -8 "${out_dir}/reclaim-window.log" ;;
esac
echo

# ---------------------------------------------------------------------------
# 5. Host diagnostic parity -- task #106.
# ---------------------------------------------------------------------------
echo "=== host diagnostic parity ==="
"${SCRIPT_DIR}/host-diagnostic-parity.sh" > "${out_dir}/diag-parity.log" 2>&1
case $? in
0) record host-diagnostic-parity GREEN ;;
1) record host-diagnostic-parity RED
   grep -E "warning:|error:" "${out_dir}/diag-parity.log" | head -10 ;;
*) record host-diagnostic-parity ERROR
   tail -5 "${out_dir}/diag-parity.log" ;;
esac
echo

# ---------------------------------------------------------------------------
# 6. Doc-citation ceilings. Fails on breach AND on slack, so a scope cannot
#    quietly un-clean itself and an improvement cannot go unrecorded.
# ---------------------------------------------------------------------------
echo "=== doc-citation baselines ==="
python3 tools/doc-citations/check_baselines.py > "${out_dir}/baselines.log" 2>&1
case $? in
0) record doc-citation-baselines GREEN ;;
1) record doc-citation-baselines RED
   grep -E "BREACH|SLACK|BAD" "${out_dir}/baselines.log" | head -10 ;;
*) record doc-citation-baselines ERROR; tail -5 "${out_dir}/baselines.log" ;;
esac
echo

# ---------------------------------------------------------------------------
# 7. ARM cross-build -- task #106.
# ---------------------------------------------------------------------------
echo "=== ARM cross-build ==="
if [ "${run_arm}" -eq 1 ]; then
    tools/mister/build-game.sh --flavor telemetry \
        --lane "${GATES_ARM_LANE:-mister}" \
        --wait-for-lane "${GATES_ARM_WAIT:-1800}" \
        > "${out_dir}/arm-build.log" 2>&1
    case $? in
    0) record arm-cross-build GREEN ;;
    *) record arm-cross-build RED
       grep -E "error:|Refusing|No space" "${out_dir}/arm-build.log" | head -10 ;;
    esac
else
    record arm-cross-build "NOT RUN"
    echo "  pass --arm (or GATES_ARM=1) to cross-compile. Until then this run"
    echo "  says nothing about the ARM target -- see the summary line."
fi
echo

# ---------------------------------------------------------------------------
# Summary. Every gate is named, including the ones that did not run.
# ---------------------------------------------------------------------------
echo "############ SUMMARY ############"

# COMPLETENESS ASSERTION.
#
# Every gate declared in GATES must have recorded a state. Without this, a gate
# that dies before calling `record` -- a missing shell builtin, an unbound
# variable under `set -u`, an early `exit` in a helper -- simply vanishes from
# the summary, and the verdict is computed over the gates that survived. That
# is not a hypothetical: the harness gate did exactly this on macOS bash 3.2
# (`mapfile: command not found`) and the run still printed GREEN.
#
# A gate that produced no state is a harness error, never a pass.
for spec in "${GATES[@]}"; do
    want="${spec%%|*}"
    found=0
    for n in "${NAMES[@]:-}"; do
        [ "${n}" = "${want}" ] && { found=1; break; }
    done
    if [ "${found}" -eq 0 ]; then
        NAMES+=("${want}"); STATES+=("NO RESULT")
        FAILED=1
    fi
done

skipped=""
for i in "${!NAMES[@]}"; do
    printf '  %-24s %s\n' "${NAMES[$i]}" "${STATES[$i]}"
    [ "${STATES[$i]}" = "NOT RUN" ] && skipped="${skipped} ${NAMES[$i]}"
    [ "${STATES[$i]}" = "NO RESULT" ] && \
        echo "      ^ this gate recorded nothing -- it did not run to completion"
done
verdict=$([ "${FAILED}" -eq 0 ] && echo GREEN || echo RED)
if [ -n "${skipped}" ]; then
    echo "GATES RESULT: ${verdict} -- NOT RUN:${skipped}"
    echo "  This result does not cover the gate(s) listed as NOT RUN."
else
    echo "GATES RESULT: ${verdict} (full set)"
fi
exit "${FAILED}"
