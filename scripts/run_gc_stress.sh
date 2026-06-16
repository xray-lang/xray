#!/usr/bin/env bash
#
# run_gc_stress.sh - GC-heavy regression burn-in for nightly CI
#
# Usage: scripts/run_gc_stress.sh [rounds]
#
# Cycles through the GC-heavy regression files N times. Returns 0 only
# if every round of every test passed.
#
# Note on `mode` parameter from 082 plan:
#   The original plan listed sticky/gen/inc/atomic GC modes, but the
#   xray CLI does not expose a `--gc-mode` switch — GC strategy is
#   driven by allocator state at runtime, not by CLI selection. The
#   pragmatic substitute is round-count: ASan/MSan + N rounds is the
#   amplification mechanism that surfaced Bug #8 / #11 in May 2026.
#
# Environment:
#   XRAY_BIN          - xray binary path (default: ./build/xray)
#   GC_STRESS_ROUNDS  - default for the rounds argument (default: 10)
#

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

ROUNDS="${1:-${GC_STRESS_ROUNDS:-10}}"
case "$ROUNDS" in
    ''|*[!0-9]*) echo "FAIL: rounds must be integer, got: $ROUNDS" >&2; exit 2 ;;
esac
if [ "$ROUNDS" -lt 1 ]; then
    echo "FAIL: rounds must be >= 1" >&2
    exit 2
fi

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

# GC-heavy regression files (interpreter is the canonical baseline).
gc_tests=(
    "${PROJECT_ROOT}/tests/regression/10_stdlib/1205_gc_incremental_pressure.xr"
    "${PROJECT_ROOT}/tests/regression/10_stdlib/1206_gc_enhanced.xr"
    "${PROJECT_ROOT}/tests/regression/10_stdlib/1207_gc_stress.xr"
)

PASS=0
FAIL=0
FAIL_LOG="${PROJECT_ROOT}/tests/tmp/gc_stress_failures.log"
mkdir -p "$(dirname "$FAIL_LOG")"
: >"$FAIL_LOG"

run_one() {
    local test_path="$1"
    local round="$2"

    if [ ! -f "$test_path" ]; then
        echo "  SKIP: missing $test_path"
        return 0
    fi

    if "$XRAY_BIN" test "$test_path" >/dev/null 2>&1; then
        PASS=$((PASS + 1))
        return 0
    fi
    FAIL=$((FAIL + 1))
    {
        echo "==== FAIL round=${round} test=${test_path} ===="
        "$XRAY_BIN" test "$test_path" 2>&1 | tail -n 30
        echo
    } >>"$FAIL_LOG"
    echo "  FAIL: round=${round} ${test_path}"
    return 1
}

echo "gc-stress: rounds=${ROUNDS} bin=${XRAY_BIN}"
echo "gc-stress: tests=${#gc_tests[@]}"

for r in $(seq 1 "$ROUNDS"); do
    echo "==== round ${r}/${ROUNDS} ===="
    for t in "${gc_tests[@]}"; do
        run_one "$t" "$r" || true
    done
done

echo
echo "gc-stress: pass=${PASS} fail=${FAIL}"
if [ "$FAIL" -gt 0 ]; then
    echo "gc-stress: failure tails -> ${FAIL_LOG}"
    exit 1
fi
exit 0
