#!/usr/bin/env bash
#
# repro_win11_coro_burn.sh - burn-in driver for the four Windows
# coroutine regressions that exposed STATUS_HEAP_CORRUPTION in
# May 2026 (1115 cancel, 1109 await_any, 1127 priority, 1128 yield).
#
# Usage: scripts/repro_win11_coro_burn.sh [N]
#
# N defaults to 5 (matches nightly.yml). Each case is run N times
# under --jit-force on the project xray binary. The driver is
# deliberately bash-portable so it runs both natively on Windows
# (Git Bash / WSL) and on Linux/macOS sanitizer hosts during local
# triage; the underlying race is heap corruption rather than truly
# Windows-specific behaviour.
#
# Environment:
#   XRAY_BIN  - xray binary path (default: ./build/xray or ./build/xray.exe)
#

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

N="${1:-5}"
case "$N" in
    ''|*[!0-9]*) echo "FAIL: N must be integer, got: $N" >&2; exit 2 ;;
esac
if [ "$N" -lt 1 ]; then
    echo "FAIL: N must be >= 1" >&2
    exit 2
fi

if [ -z "${XRAY_BIN:-}" ]; then
    if [ -x "${PROJECT_ROOT}/build/Release/xray.exe" ]; then
        XRAY_BIN="${PROJECT_ROOT}/build/Release/xray.exe"
    elif [ -x "${PROJECT_ROOT}/build/Debug/xray.exe" ]; then
        XRAY_BIN="${PROJECT_ROOT}/build/Debug/xray.exe"
    elif [ -x "${PROJECT_ROOT}/build/xray.exe" ]; then
        XRAY_BIN="${PROJECT_ROOT}/build/xray.exe"
    elif [ -x "${PROJECT_ROOT}/build-release/xray" ]; then
        XRAY_BIN="${PROJECT_ROOT}/build-release/xray"
    elif [ -x "${PROJECT_ROOT}/build/xray" ]; then
        XRAY_BIN="${PROJECT_ROOT}/build/xray"
    else
        echo "FAIL: xray binary not found; build first or set XRAY_BIN" >&2
        exit 2
    fi
fi
if [ ! -x "$XRAY_BIN" ]; then
    echo "FAIL: xray binary not executable: $XRAY_BIN" >&2
    exit 2
fi

# The four scenarios that surfaced STATUS_HEAP_CORRUPTION on Windows
# during the May 2026 11-bug burn-in. The bug class is heap
# corruption on coroutine teardown, exercised by cancel / await_any /
# priority scheduling / explicit yield.
cases=(
    "${PROJECT_ROOT}/tests/regression/11_coroutine/1115_cancel.xr"
    "${PROJECT_ROOT}/tests/regression/11_coroutine/1109_await_any.xr"
    "${PROJECT_ROOT}/tests/regression/11_coroutine/1127_coro_priority.xr"
    "${PROJECT_ROOT}/tests/regression/11_coroutine/1128_yield.xr"
)

OUT_DIR="${PROJECT_ROOT}/tests/tmp/win11_coro"
mkdir -p "$OUT_DIR"

PASS=0
FAIL=0
FAIL_LOG="${OUT_DIR}/failures.log"
: >"$FAIL_LOG"

echo "win11-coro-burn: N=${N} bin=${XRAY_BIN}"
echo "win11-coro-burn: cases=${#cases[@]}"

for round in $(seq 1 "$N"); do
    echo "==== round ${round}/${N} ===="
    for c in "${cases[@]}"; do
        if [ ! -f "$c" ]; then
            echo "  SKIP: missing $c"
            continue
        fi
        if "$XRAY_BIN" test --jit-force "$c" >/dev/null 2>&1; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
            {
                echo "==== FAIL round=${round} test=${c} ===="
                "$XRAY_BIN" test --jit-force "$c" 2>&1 | tail -n 30
                echo
            } >>"$FAIL_LOG"
            echo "  FAIL: round=${round} $(basename "$c")"
        fi
    done
done

echo
echo "win11-coro-burn: pass=${PASS} fail=${FAIL} (cases=${#cases[@]} rounds=${N})"
if [ "$FAIL" -gt 0 ]; then
    echo "win11-coro-burn: failure tails -> ${FAIL_LOG}"
    exit 1
fi
exit 0
