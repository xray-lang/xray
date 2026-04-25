#!/bin/bash
# run_multiworker_tests.sh - Run regression tests with multiple worker counts
#
# Ensures scheduler correctness under different concurrency levels.
# Usage:
#   ./scripts/run_multiworker_tests.sh              # use debug build
#   XRAY_BUILD_DIR=build-release ./scripts/run_multiworker_tests.sh  # use release build

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    GREEN='\033[0;32m' RED='\033[0;31m' BLUE='\033[0;34m' YELLOW='\033[1;33m' NC='\033[0m'
else
    GREEN='' RED='' YELLOW='' BLUE='' NC=''
fi

# Auto-detect build directory
if [ -n "${XRAY_BUILD_DIR}" ]; then
    BUILD_DIR="${XRAY_BUILD_DIR}"
elif [ -f "${PROJECT_ROOT}/build/xray" ]; then
    BUILD_DIR="${PROJECT_ROOT}/build"
elif [ -f "${PROJECT_ROOT}/build-release/xray" ]; then
    BUILD_DIR="${PROJECT_ROOT}/build-release"
else
    BUILD_DIR="${PROJECT_ROOT}/build"
fi

XRAY_BIN="${BUILD_DIR}/xray"

# Build once
echo -e "${BLUE}Building ${BUILD_DIR}...${NC}"
cmake --build "${BUILD_DIR}" --target xray -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | tail -3
echo -e "${GREEN}Build complete${NC}"

TOTAL_PASS=0
TOTAL_FAIL=0

for W in 1 2 4; do
    echo ""
    echo -e "${BLUE}========== XRAY_WORKERS=$W ==========${NC}"
    if XRAY_WORKERS=$W XRAY_BUILD_DIR="${BUILD_DIR}" XRAY_SKIP_BUILD=1 \
        "${SCRIPT_DIR}/run_regression_tests.sh"; then
        echo -e "${GREEN}W=$W: PASS${NC}"
        TOTAL_PASS=$((TOTAL_PASS + 1))
    else
        echo -e "${RED}W=$W: FAIL${NC}"
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
    fi
done

echo ""
echo -e "${BLUE}========== Summary ==========${NC}"
echo -e "Pass: ${GREEN}${TOTAL_PASS}${NC}  Fail: ${RED}${TOTAL_FAIL}${NC}"

if [ "$TOTAL_FAIL" -gt 0 ]; then
    exit 1
fi
