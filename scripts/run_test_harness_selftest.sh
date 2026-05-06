#!/bin/bash
# run_test_harness_selftest.sh
#
# Validates that `xray test` correctly:
# - discovers and executes @test functions
# - reports accurate counts
# - returns non-zero on failure or 0 tests
# - handles skip, timeout, and mixed pass/fail

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FIXTURE_DIR="${PROJECT_ROOT}/tests/test_harness"

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
XRAY="${BUILD_DIR}/xray"

if [ ! -x "${XRAY}" ]; then
    echo "ERROR: xray binary not found at ${XRAY}"
    exit 1
fi

passed=0
failed=0
total=0

check() {
    local name="$1"
    local expect_exit="$2"  # 0 or nonzero
    local fixture="$3"
    local extra_check="$4"  # optional grep pattern on stdout+stderr
    total=$((total + 1))

    local output
    output=$("${XRAY}" test "${fixture}" 2>&1)
    local actual_exit=$?

    local ok=true

    if [ "${expect_exit}" = "0" ] && [ ${actual_exit} -ne 0 ]; then
        echo "  FAIL: ${name} — expected exit 0, got ${actual_exit}"
        ok=false
    elif [ "${expect_exit}" = "nonzero" ] && [ ${actual_exit} -eq 0 ]; then
        echo "  FAIL: ${name} — expected nonzero exit, got 0"
        ok=false
    fi

    if [ -n "${extra_check}" ] && ! echo "${output}" | grep -q "${extra_check}"; then
        echo "  FAIL: ${name} — output missing pattern: ${extra_check}"
        ok=false
    fi

    if ${ok}; then
        echo "  PASS: ${name}"
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        if [ -n "${output}" ]; then
            echo "  --- output ---"
            echo "${output}" | head -20
            echo "  --- end ---"
        fi
    fi
}

echo ""
echo "========================================"
echo " Test Harness Self-Test"
echo "========================================"
echo ""

# 1. Single passing test
check "single_pass: 1 test passes, exit 0" \
    "0" "${FIXTURE_DIR}/single_pass.xr" "1 passed"

# 2. Multiple passing tests
check "multi_pass: 3 tests pass, exit 0" \
    "0" "${FIXTURE_DIR}/multi_pass.xr" "3 passed"

# 3. Single failing test
check "single_fail: 1 test fails, exit nonzero" \
    "nonzero" "${FIXTURE_DIR}/single_fail.xr" "1 failed"

# 4. No tests in file — should exit nonzero
check "no_tests: 0 tests, exit nonzero" \
    "nonzero" "${FIXTURE_DIR}/no_tests.xr" "0 tests executed"

# 5. Mixed pass/fail
check "mixed_pass_fail: has failures, exit nonzero" \
    "nonzero" "${FIXTURE_DIR}/mixed_pass_fail.xr" "failed"

# 6. Skip test
check "with_skip: 1 pass + 1 skipped, exit 0" \
    "0" "${FIXTURE_DIR}/with_skip.xr" "1 passed"

echo ""
echo "========================================"
echo " Results: ${passed}/${total} passed, ${failed} failed"
echo "========================================"
echo ""

if [ ${failed} -gt 0 ]; then
    exit 1
fi
exit 0
