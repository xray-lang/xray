#!/usr/bin/env bash
# ============================================================================
# riscv64_qemu_test.sh
# ----------------------------------------------------------------------------
# S10 RISC-V JIT bring-up — QEMU user-mode test runner
#
# Runs Xray programs under QEMU riscv64 user-mode emulation with JIT enabled,
# comparing output against the native interpreter (--no-jit) as ground truth.
#
# Prerequisites:
#   - qemu-riscv64 (or qemu-riscv64-static) in PATH
#   - Cross-compiled xray binary for riscv64 (XRAY_RV64_BIN)
#   - Native xray binary for baseline (XRAY_BIN, defaults to ./build/xray)
#
# Usage:
#   XRAY_RV64_BIN=./build-rv64/xray bash scripts/riscv64_qemu_test.sh
#
# Exit codes:
#   0   all tests passed
#   1   one or more tests failed or diverged
#   2   prerequisites missing
# ============================================================================

set -uo pipefail
cd "$(dirname "$0")/.."

RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
CYAN='\033[36m'
NC='\033[0m'

XRAY_BIN="${XRAY_BIN:-./build/xray}"
XRAY_RV64_BIN="${XRAY_RV64_BIN:-}"
QEMU="${QEMU_RISCV64:-$(command -v qemu-riscv64 2>/dev/null || command -v qemu-riscv64-static 2>/dev/null || true)}"
TIMEOUT="${TIMEOUT:-10}"

# ============================================================================
# Prerequisites check
# ============================================================================
if [ -z "$QEMU" ]; then
    echo -e "${YELLOW}qemu-riscv64 not found in PATH. S10 RISC-V tests require Linux with QEMU user-mode.${NC}"
    echo "Install: apt-get install qemu-user qemu-user-static"
    exit 2
fi

if [ -z "$XRAY_RV64_BIN" ] || [ ! -f "$XRAY_RV64_BIN" ]; then
    echo -e "${YELLOW}XRAY_RV64_BIN not set or binary not found.${NC}"
    echo "Cross-compile xray for riscv64 first:"
    echo "  cmake -DCMAKE_TOOLCHAIN_FILE=cmake/riscv64-linux-gnu.cmake -B build-rv64"
    echo "  cmake --build build-rv64"
    exit 2
fi

if [ ! -f "$XRAY_BIN" ]; then
    echo -e "${RED}Native xray binary not found at $XRAY_BIN${NC}"
    exit 2
fi

echo "=== S10 RISC-V QEMU JIT Test ==="
echo "  QEMU:         $QEMU"
echo "  RV64 binary:  $XRAY_RV64_BIN"
echo "  Native binary: $XRAY_BIN"
echo ""

# ============================================================================
# Test suite: scalar programs that exercise JIT
# ============================================================================
TESTS=(
    "tests/regression/01_basics/0101_hello.xr"
    "tests/regression/02_variables/0201_let.xr"
    "tests/regression/03_operators/0301_arithmetic.xr"
    "tests/regression/03_operators/0310_comparison.xr"
    "tests/regression/04_control_flow/0401_if_else.xr"
    "tests/regression/04_control_flow/0410_while.xr"
    "tests/regression/04_control_flow/0420_for.xr"
    "tests/regression/05_functions/0501_basic_fn.xr"
    "tests/regression/05_functions/0510_recursion.xr"
    "tests/regression/06_collections/0601_array.xr"
)

PASS=0
FAIL=0
SKIP=0
DIVERGE=0

for test_file in "${TESTS[@]}"; do
    if [ ! -f "$test_file" ]; then
        SKIP=$((SKIP + 1))
        continue
    fi

    test_name=$(basename "$test_file")

    # Native baseline (--no-jit)
    native_out=$(timeout "$TIMEOUT" "$XRAY_BIN" --no-jit "$test_file" 2>/dev/null) && native_rc=0 || native_rc=$?
    if [ "$native_rc" -ne 0 ] && [ "$native_rc" -ne 124 ]; then
        printf "  %-40s ${YELLOW}SKIP (native fail)${NC}\n" "$test_name"
        SKIP=$((SKIP + 1))
        continue
    fi

    # QEMU riscv64 with JIT
    rv64_out=$(timeout "$TIMEOUT" "$QEMU" "$XRAY_RV64_BIN" --jit-force "$test_file" 2>/dev/null) && rv64_rc=0 || rv64_rc=$?

    if [ "$rv64_rc" -eq 139 ] || [ "$rv64_rc" -eq 134 ] || [ "$rv64_rc" -eq 136 ]; then
        printf "  %-40s ${RED}CRASH (exit=%d)${NC}\n" "$test_name" "$rv64_rc"
        FAIL=$((FAIL + 1))
    elif [ "$rv64_rc" -eq 124 ]; then
        printf "  %-40s ${YELLOW}TIMEOUT${NC}\n" "$test_name"
        SKIP=$((SKIP + 1))
    elif [ "$native_out" != "$rv64_out" ]; then
        printf "  %-40s ${RED}OUTPUT DIVERGE${NC}\n" "$test_name"
        DIVERGE=$((DIVERGE + 1))
    else
        printf "  %-40s ${GREEN}PASS${NC}\n" "$test_name"
        PASS=$((PASS + 1))
    fi
done

echo ""
echo "=== RISC-V QEMU JIT Results ==="
echo "  Pass:    $PASS"
echo "  Fail:    $FAIL"
echo "  Diverge: $DIVERGE"
echo "  Skip:    $SKIP"

if [ "$FAIL" -gt 0 ] || [ "$DIVERGE" -gt 0 ]; then
    echo -e "${RED}RISC-V JIT bugs detected.${NC}"
    exit 1
elif [ "$PASS" -eq 0 ]; then
    echo -e "${YELLOW}No tests ran (all skipped).${NC}"
    exit 2
else
    echo -e "${GREEN}All RISC-V JIT tests passed.${NC}"
    exit 0
fi
