#!/usr/bin/env bash
# ===========================================================================
# DAP Native Backend Test Runner
#
# Verifies `xray dap --native`: source-level debugging of a `.xr` program
# compiled to a native binary, driven through lldb-dap. Skips cleanly when
# lldb-dap (or python3) is unavailable, so CI hosts without the LLVM tools
# do not fail.
#
# Usage:
#   scripts/run_dap_native_tests.sh
#   XRAY_LLDB_DAP=/path/to/lldb-dap scripts/run_dap_native_tests.sh
# ===========================================================================

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[1;31m' GREEN='\033[1;32m' BLUE='\033[1;34m' YELLOW='\033[1;33m' NC='\033[0m'
else
    RED='' GREEN='' BLUE='' YELLOW='' NC=''
fi

# Auto-detect build directory.
if [ -n "${XRAY_BUILD_DIR:-}" ]; then
    BUILD_DIR="${XRAY_BUILD_DIR}"
elif [ -f "${PROJECT_ROOT}/build/xray" ]; then
    BUILD_DIR="${PROJECT_ROOT}/build"
elif [ -f "${PROJECT_ROOT}/build-release/xray" ]; then
    BUILD_DIR="${PROJECT_ROOT}/build-release"
else
    BUILD_DIR="${PROJECT_ROOT}/build"
fi
XRAY_BIN="${BUILD_DIR}/xray"

FIXTURE_DIR="${PROJECT_ROOT}/tests/regression/dap/fixtures"
DRIVER="${PROJECT_ROOT}/tests/regression/dap/native_driver.py"

echo -e "${BLUE}======================================"
echo "DAP Native Backend Tests"
echo -e "======================================${NC}"

# --- Preconditions (skip, do not fail, when unmet) -------------------------
skip() {
    echo -e "${YELLOW}SKIP${NC} — $1"
    exit 77
}

[ -f "$XRAY_BIN" ] || skip "xray binary not found at ${XRAY_BIN} (build first)"
command -v python3 >/dev/null 2>&1 || skip "python3 not found"
if [ "$(uname -s)" = "Darwin" ] &&
        ! xcrun -f debugserver >/dev/null 2>&1 &&
        ! command -v debugserver >/dev/null 2>&1; then
    skip "debugserver not found; install full Xcode or provide debugserver to run native DAP tests"
fi

# Locate lldb-dap the same way the native bridge does.
find_lldb_dap() {
    if [ -n "${XRAY_LLDB_DAP:-}" ] && [ -x "${XRAY_LLDB_DAP}" ]; then
        echo "${XRAY_LLDB_DAP}"; return 0
    fi
    for c in \
        /opt/homebrew/opt/llvm/bin/lldb-dap \
        /usr/local/opt/llvm/bin/lldb-dap \
        /usr/bin/lldb-dap \
        /usr/local/bin/lldb-dap \
        /Library/Developer/CommandLineTools/usr/bin/lldb-dap; do
        [ -x "$c" ] && { echo "$c"; return 0; }
    done
    command -v lldb-dap 2>/dev/null && return 0
    return 1
}

LLDB_DAP="$(find_lldb_dap || true)"
[ -n "$LLDB_DAP" ] || skip "lldb-dap not found (set XRAY_LLDB_DAP to enable native DAP tests)"
echo "Using lldb-dap: ${LLDB_DAP}"

# --- Run -------------------------------------------------------------------
PASS=0
FAIL=0

run_case() {
    local name="$1" prog="$2" line="$3"
    printf "[%-28s] line %s ... \n" "$name" "$line"
    if XRAY_LLDB_DAP="$LLDB_DAP" python3 "$DRIVER" "$XRAY_BIN" "$prog" "$line"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_case "native_bp.xr" "${FIXTURE_DIR}/native_bp.xr" 10

echo ""
echo -e "${BLUE}======================================"
echo "Summary: pass=${PASS} fail=${FAIL}"
echo -e "======================================${NC}"

if [ "$FAIL" -gt 0 ]; then
    echo -e "${RED}FAILED${NC}"
    exit 1
fi
echo -e "${GREEN}ALL PASSED${NC}"
exit 0
