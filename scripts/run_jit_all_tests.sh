#!/bin/bash
# JIT Full Test Suite - Runs all layers of JIT testing
#
# Test Pyramid:
#   L0: Codegen E2E (test_jit_e2e)    - XIR → ARM64 → Execute
#   L1: Builder    (test_xir_builder)  - Bytecode → XIR
#   L2: XIR Passes (test_xir_pass etc) - Optimization correctness
#   L3: Scenarios  (tests/jit/*.xr)    - High-level JIT behavior
#   L4: Diff test  (regression suite)  - --jit-force vs --no-jit
#
# Usage: ./scripts/run_jit_all_tests.sh [options]
#   -l <level>  Run only specific level (0-4, default: all)
#   -b <binary> Path to xray binary
#   -v          Verbose
#   -q          Quick mode (skip L4 diff tests)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LEVEL=""
VERBOSE=0
QUICK=0

# Auto-detect build directory
if [ -f "${PROJECT_ROOT}/build-release/xray" ]; then
    BUILD_DIR="${PROJECT_ROOT}/build-release"
elif [ -f "${PROJECT_ROOT}/build/xray" ]; then
    BUILD_DIR="${PROJECT_ROOT}/build"
else
    BUILD_DIR="${PROJECT_ROOT}/build-release"
fi

while getopts "l:b:vq" opt; do
    case $opt in
        l) LEVEL="$OPTARG" ;;
        b) BUILD_DIR="$(dirname "$OPTARG")" ;;
        v) VERBOSE=1 ;;
        q) QUICK=1 ;;
        *) echo "Usage: $0 [-l level] [-b binary] [-v] [-q]"; exit 1 ;;
    esac
done

XRAY_BIN="${BUILD_DIR}/xray"
TOTAL_PASS=0
TOTAL_FAIL=0
LAYERS_RUN=0

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    GREEN='\033[0;32m' RED='\033[0;31m' BLUE='\033[0;34m' YELLOW='\033[0;33m' NC='\033[0m'
else
    GREEN='' RED='' BLUE='' YELLOW='' NC=''
fi

run_layer() {
    local layer=$1
    local name=$2
    local cmd=$3

    if [ -n "$LEVEL" ] && [ "$LEVEL" != "$layer" ]; then
        return
    fi

    LAYERS_RUN=$((LAYERS_RUN + 1))
    echo ""
    echo -e "${BLUE}═══════════════════════════════════════${NC}"
    echo -e "${BLUE}  L${layer}: ${name}${NC}"
    echo -e "${BLUE}═══════════════════════════════════════${NC}"

    if eval "$cmd"; then
        TOTAL_PASS=$((TOTAL_PASS + 1))
        echo -e "  ${GREEN}L${layer} PASSED${NC}"
    else
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
        echo -e "  ${RED}L${layer} FAILED${NC}"
    fi
}

echo -e "${BLUE}╔═══════════════════════════════════════╗"
echo -e "║    JIT Full Test Suite                ║"
echo -e "╚═══════════════════════════════════════╝${NC}"
echo "Build dir: ${BUILD_DIR}"
echo ""

# L0: Codegen E2E
if [ -f "${BUILD_DIR}/test_jit_e2e" ]; then
    run_layer 0 "Codegen E2E (test_jit_e2e)" \
        "${BUILD_DIR}/test_jit_e2e 2>&1 | tail -3"
else
    echo -e "  ${YELLOW}L0: test_jit_e2e not found, skipping${NC}"
fi

# L1: XIR Builder
if [ -f "${BUILD_DIR}/test_xir_builder" ]; then
    run_layer 1 "XIR Builder (test_xir_builder)" \
        "${BUILD_DIR}/test_xir_builder 2>&1 | tail -3"
else
    echo -e "  ${YELLOW}L1: test_xir_builder not found, skipping${NC}"
fi

# L2: XIR Passes (run all xir tests)
run_l2() {
    local all_pass=true
    for test_bin in test_xir test_xir_bset test_xir_defuse test_xir_fold test_xir_liveness2 test_xir_pass; do
        if [ -f "${BUILD_DIR}/${test_bin}" ]; then
            if ! "${BUILD_DIR}/${test_bin}" > /dev/null 2>&1; then
                echo "    FAIL: ${test_bin}"
                all_pass=false
            else
                local count=$(grep -c 'PASS' <("${BUILD_DIR}/${test_bin}" 2>&1) 2>/dev/null || echo "?")
                echo "    OK: ${test_bin}"
            fi
        fi
    done
    $all_pass
}
run_layer 2 "XIR Passes (6 test binaries)" "run_l2"

# L3: JIT Scenarios
if [ -x "$XRAY_BIN" ]; then
    run_layer 3 "JIT Scenarios (tests/jit/*.xr)" \
        "bash ${SCRIPT_DIR}/run_jit_tests.sh -b ${XRAY_BIN} 2>&1 | tail -15"
fi

# L4: Diff tests (skip in quick mode)
if [ "$QUICK" -eq 0 ] && [ -x "$XRAY_BIN" ]; then
    run_layer 4 "Differential (--jit-force vs --no-jit)" \
        "bash ${SCRIPT_DIR}/run_jit_diff_tests.sh -b ${XRAY_BIN} 2>&1 | tail -20"
elif [ "$QUICK" -eq 1 ]; then
    echo -e "  ${YELLOW}L4: Skipped (quick mode)${NC}"
fi

# Summary
echo ""
echo -e "${BLUE}╔═══════════════════════════════════════╗"
echo -e "║    Summary                            ║"
echo -e "╚═══════════════════════════════════════╝${NC}"
echo "Layers run:    ${LAYERS_RUN}"
echo -e "${GREEN}Layers passed: ${TOTAL_PASS}${NC}"
echo -e "${RED}Layers failed: ${TOTAL_FAIL}${NC}"

if [ "$TOTAL_FAIL" -eq 0 ]; then
    echo ""
    echo -e "${GREEN}All JIT test layers passed!${NC}"
    exit 0
else
    echo ""
    echo -e "${RED}Some JIT test layers failed.${NC}"
    exit 1
fi
