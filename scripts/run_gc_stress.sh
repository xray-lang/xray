#!/usr/bin/env bash
#
# run_gc_stress.sh - GC-heavy regression burn-in for nightly CI
#
# Usage: scripts/run_gc_stress.sh [rounds]
#
# Cycles through the GC-heavy regression files N times under
# --jit-force or --no-jit as appropriate (1206 exercises the JIT path,
# 1205/1207 are interpreter-only NOJIT tests). Returns 0 only if every
# round of every test passed.
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

# JIT-mode tests: 1206 exercises GC interaction with JIT-compiled
# frames (the path that surfaced Bug #10 / #11 in May 2026).
jit_tests=(
    "${PROJECT_ROOT}/tests/regression/10_stdlib/1206_gc_enhanced.xr"
)
# NOJIT-mode tests: 1205 / 1207 are GC pressure / stress tests that
# tests/known_failures.txt deliberately marks NOJIT (interpreter is
# the canonical baseline; JIT diff has its own coverage in 16_jit/).
nojit_tests=(
    "${PROJECT_ROOT}/tests/regression/10_stdlib/1205_gc_incremental_pressure.xr"
    "${PROJECT_ROOT}/tests/regression/10_stdlib/1207_gc_stress.xr"
)

PASS=0
FAIL=0
FAIL_LOG="${PROJECT_ROOT}/tests/tmp/gc_stress_failures.log"
mkdir -p "$(dirname "$FAIL_LOG")"
: >"$FAIL_LOG"

run_one() {
    local mode="$1"  # jit | nojit
    local test_path="$2"
    local round="$3"

    if [ ! -f "$test_path" ]; then
        echo "  SKIP: missing $test_path"
        return 0
    fi

    local flag
    case "$mode" in
        jit)   flag="--jit-force" ;;
        nojit) flag="--no-jit"   ;;
        *)     echo "FAIL: unknown mode $mode" >&2; return 2 ;;
    esac

    if "$XRAY_BIN" test "$flag" "$test_path" >/dev/null 2>&1; then
        PASS=$((PASS + 1))
        return 0
    fi
    FAIL=$((FAIL + 1))
    {
        echo "==== FAIL round=${round} mode=${mode} test=${test_path} ===="
        "$XRAY_BIN" test "$flag" "$test_path" 2>&1 | tail -n 30
        echo
    } >>"$FAIL_LOG"
    echo "  FAIL: round=${round} ${flag} ${test_path}"
    return 1
}

echo "gc-stress: rounds=${ROUNDS} bin=${XRAY_BIN}"
echo "gc-stress: jit_tests=${#jit_tests[@]} nojit_tests=${#nojit_tests[@]}"

for r in $(seq 1 "$ROUNDS"); do
    echo "==== round ${r}/${ROUNDS} ===="
    for t in "${jit_tests[@]}"; do
        run_one jit "$t" "$r" || true
    done
    for t in "${nojit_tests[@]}"; do
        run_one nojit "$t" "$r" || true
    done
done

echo
echo "gc-stress: pass=${PASS} fail=${FAIL}"
if [ "$FAIL" -gt 0 ]; then
    echo "gc-stress: failure tails -> ${FAIL_LOG}"
    exit 1
fi
exit 0
