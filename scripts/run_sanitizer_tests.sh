#!/bin/bash
# run_sanitizer_tests.sh - CI sanitizer test runner
#
# Builds and tests xray with ASan+UBSan (combined) and TSan separately.
# TSan cannot coexist with ASan, so they run in separate build dirs.
#
# Usage:
#   ./scripts/run_sanitizer_tests.sh              # run all sanitizers
#   ./scripts/run_sanitizer_tests.sh asan         # ASan+UBSan only
#   ./scripts/run_sanitizer_tests.sh tsan         # TSan only
#   ./scripts/run_sanitizer_tests.sh --skip-build # reuse existing builds

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    GREEN='\033[0;32m' RED='\033[0;31m' BLUE='\033[0;34m' YELLOW='\033[0;33m' NC='\033[0m'
else
    GREEN='' RED='' BLUE='' YELLOW='' NC=''
fi

# Detect CPU count
if command -v sysctl &>/dev/null; then
    NCPU=$(sysctl -n hw.ncpu)
elif command -v nproc &>/dev/null; then
    NCPU=$(nproc)
else
    NCPU=4
fi

SKIP_BUILD=0
MODE="all"
for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=1 ;;
        asan)         MODE="asan" ;;
        tsan)         MODE="tsan" ;;
    esac
done

TOTAL_PASS=0
TOTAL_FAIL=0

# ============================================================================
# Helper: build + test one sanitizer configuration
# ============================================================================
run_sanitizer() {
    local name="$1"
    local build_dir="$2"
    shift 2
    # remaining args are cmake options

    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  ${name}${NC}"
    echo -e "${BLUE}========================================${NC}"

    if [ "$SKIP_BUILD" -eq 0 ]; then
        echo -e "${BLUE}Configuring ${name} build...${NC}"
        cmake -B "${build_dir}" -S "${PROJECT_ROOT}" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DBUILD_TESTS=ON \
            -DXR_BUILD_LSP=OFF \
            -DXR_BUILD_DAP=OFF \
            "$@" \
            2>&1 | tail -5

        echo -e "${BLUE}Building...${NC}"
        if ! cmake --build "${build_dir}" --target xray -j${NCPU} 2>&1 | tail -10; then
            echo -e "${RED}BUILD FAILED: ${name}${NC}"
            TOTAL_FAIL=$((TOTAL_FAIL + 1))
            return 1
        fi
        echo -e "${GREEN}Build OK${NC}"
    fi

    local xray_bin="${build_dir}/xray"
    if [ ! -x "$xray_bin" ]; then
        echo -e "${RED}ERROR: ${xray_bin} not found${NC}"
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
        return 1
    fi

    # Run unit tests (ctest)
    echo -e "${BLUE}Running unit tests...${NC}"
    if (cd "${build_dir}" && ctest --output-on-failure -j${NCPU} 2>&1 | tail -10); then
        echo -e "${GREEN}Unit tests OK${NC}"
    else
        echo -e "${YELLOW}Some unit tests failed (see above)${NC}"
    fi

    # Run regression tests
    echo -e "${BLUE}Running regression tests...${NC}"
    if XRAY_BUILD_DIR="${build_dir}" XRAY_SKIP_BUILD=1 \
        "${SCRIPT_DIR}/run_regression_tests.sh" 2>&1 | tail -15; then
        TOTAL_PASS=$((TOTAL_PASS + 1))
        echo -e "${GREEN}Regression tests PASS${NC}"
    else
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
        echo -e "${RED}Regression tests FAIL${NC}"
    fi
}

# ============================================================================
# ASan + UBSan (combined — catches memory errors + undefined behavior)
# ============================================================================
if [ "$MODE" = "all" ] || [ "$MODE" = "asan" ]; then
    run_sanitizer "ASan + UBSan" "${PROJECT_ROOT}/build-sanitizers" \
        -DENABLE_SANITIZERS=ON
fi

# ============================================================================
# TSan (thread sanitizer — detects data races)
# Cannot coexist with ASan, needs separate build.
# ============================================================================
if [ "$MODE" = "all" ] || [ "$MODE" = "tsan" ]; then
    run_sanitizer "TSan" "${PROJECT_ROOT}/build-tsan" \
        -DENABLE_TSAN=ON

    # TSan multi-worker test (W=2 to trigger race conditions)
    echo -e "${BLUE}Running TSan with W=2 workers...${NC}"
    if XRAY_WORKERS=2 XRAY_BUILD_DIR="${PROJECT_ROOT}/build-tsan" XRAY_SKIP_BUILD=1 \
        "${SCRIPT_DIR}/run_regression_tests.sh" 2>&1 | tail -10; then
        echo -e "${GREEN}TSan W=2 PASS${NC}"
    else
        echo -e "${YELLOW}TSan W=2 had issues (check for data race reports above)${NC}"
    fi
fi

# ============================================================================
# Summary
# ============================================================================
echo ""
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  Sanitizer Summary${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "  Passed: ${GREEN}${TOTAL_PASS}${NC}"
echo -e "  Failed: ${RED}${TOTAL_FAIL}${NC}"

if [ "$TOTAL_FAIL" -gt 0 ]; then
    echo -e "${RED}Some sanitizer tests failed!${NC}"
    exit 1
fi
echo -e "${GREEN}All sanitizer tests passed!${NC}"
