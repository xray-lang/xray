#!/usr/bin/env bash
#
# repro_jit_force_burn.sh - burn-in driver for one regression test
#
# Usage: scripts/repro_jit_force_burn.sh <test_file> <N>
#
# Runs the named .xr file N times under --jit-force, prints a one-line
# summary per run (idx | exit | duration_ms), and on each non-zero exit
# captures the last 30 lines of stderr to a per-failure log under
# tests/tmp/burn/<basename>/. Exit code is 0 only if every run was 0.
#
# Intended for surfacing timing-sensitive / heisenbug-class regressions
# that pass once but fail after a handful of replays. Used both
# interactively when triaging a flaky test and from CI to enforce a
# minimum repro rate on nightly.
#
# Environment knobs:
#   XRAY_BIN       - path to xray binary (default: build/xray-release or build/xray)
#   XRAY_SUBCMD    - xray subcommand (default: test, since burn-in targets
#                    @test-marked regression files; set to empty to invoke
#                    the binary directly for plain scripts)
#   TIMEOUT_SEC    - per-run wall-clock guard (default: 60)
#   ASAN_OPTIONS   - forwarded to child if set
#   MSAN_OPTIONS   - forwarded to child if set
#   UBSAN_OPTIONS  - forwarded to child if set
#

set -uo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <test_file> <N>" >&2
    echo "  test_file: path to a single .xr regression file" >&2
    echo "  N:         positive integer, number of repeats" >&2
    exit 2
fi

TEST_FILE="$1"
N="$2"

if [ ! -f "$TEST_FILE" ]; then
    echo "FAIL: test file not found: $TEST_FILE" >&2
    exit 2
fi

case "$N" in
    ''|*[!0-9]*)
        echo "FAIL: N must be a positive integer, got: $N" >&2
        exit 2
        ;;
esac
if [ "$N" -lt 1 ]; then
    echo "FAIL: N must be >= 1" >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Auto-detect xray binary unless caller pinned one.
if [ -z "${XRAY_BIN:-}" ]; then
    if [ -x "${PROJECT_ROOT}/build-release/xray" ]; then
        XRAY_BIN="${PROJECT_ROOT}/build-release/xray"
    elif [ -x "${PROJECT_ROOT}/build/xray" ]; then
        XRAY_BIN="${PROJECT_ROOT}/build/xray"
    elif [ -x "${PROJECT_ROOT}/build/xray.exe" ]; then
        XRAY_BIN="${PROJECT_ROOT}/build/xray.exe"
    else
        echo "FAIL: xray binary not found; build first or set XRAY_BIN" >&2
        exit 2
    fi
fi
if [ ! -x "$XRAY_BIN" ]; then
    echo "FAIL: xray binary not executable: $XRAY_BIN" >&2
    exit 2
fi

TIMEOUT_SEC="${TIMEOUT_SEC:-60}"
SUBCMD="${XRAY_SUBCMD-test}"
BASENAME="$(basename "$TEST_FILE" .xr)"
OUT_DIR="${PROJECT_ROOT}/tests/tmp/burn/${BASENAME}"
mkdir -p "$OUT_DIR"

# Pick a portable monotonic millisecond timer.
now_ms() {
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import time; print(int(time.monotonic()*1000))'
    elif command -v perl >/dev/null 2>&1; then
        perl -MTime::HiRes=time -e 'printf "%d", time()*1000'
    else
        # Coarse fallback: seconds * 1000 (still useful for trend).
        echo $(($(date +%s) * 1000))
    fi
}

# Pick a portable timeout wrapper.
TIMEOUT_CMD=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD="timeout ${TIMEOUT_SEC}s"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_CMD="gtimeout ${TIMEOUT_SEC}s"
fi

PASS=0
FAIL=0
FAIL_FIRST_IDX=-1
FAIL_FIRST_RC=0

echo "burn-in: $TEST_FILE  N=$N  bin=$XRAY_BIN"
echo "logs:    $OUT_DIR/"
printf "%-5s  %-5s  %s\n" "idx" "exit" "ms"

for i in $(seq 1 "$N"); do
    log="${OUT_DIR}/run_${i}.stderr"
    t_start=$(now_ms)
    if [ -n "$SUBCMD" ]; then
        if [ -n "$TIMEOUT_CMD" ]; then
            # shellcheck disable=SC2086
            $TIMEOUT_CMD "$XRAY_BIN" "$SUBCMD" --jit-force "$TEST_FILE" >/dev/null 2>"$log"
            rc=$?
        else
            "$XRAY_BIN" "$SUBCMD" --jit-force "$TEST_FILE" >/dev/null 2>"$log"
            rc=$?
        fi
    else
        if [ -n "$TIMEOUT_CMD" ]; then
            # shellcheck disable=SC2086
            $TIMEOUT_CMD "$XRAY_BIN" --jit-force "$TEST_FILE" >/dev/null 2>"$log"
            rc=$?
        else
            "$XRAY_BIN" --jit-force "$TEST_FILE" >/dev/null 2>"$log"
            rc=$?
        fi
    fi
    t_end=$(now_ms)
    dur=$((t_end - t_start))

    printf "%-5d  %-5d  %d\n" "$i" "$rc" "$dur"

    if [ "$rc" -eq 0 ]; then
        PASS=$((PASS + 1))
        rm -f "$log"
    else
        FAIL=$((FAIL + 1))
        if [ "$FAIL_FIRST_IDX" -lt 0 ]; then
            FAIL_FIRST_IDX="$i"
            FAIL_FIRST_RC="$rc"
        fi
        # Trim to last 30 lines for compact triage.
        if [ -s "$log" ]; then
            tail -n 30 "$log" >"${log}.tail"
            mv "${log}.tail" "$log"
        fi
    fi
done

echo
echo "summary: pass=${PASS} fail=${FAIL} N=${N}"
if [ "$FAIL" -gt 0 ]; then
    echo "first failure: run #${FAIL_FIRST_IDX} exit=${FAIL_FIRST_RC}"
    echo "stderr tails:  ${OUT_DIR}/run_*.stderr"
    exit 1
fi
exit 0
