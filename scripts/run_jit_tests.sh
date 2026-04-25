#!/bin/bash
# JIT Test Suite Runner
# Usage: ./scripts/run_jit_tests.sh [options]
#   -b <binary>   Path to xray binary (default: ./build-release/xray)
#   -f <filter>   Run only tests matching filter (e.g. "arithmetic")
#   -v            Verbose mode (show output on failure)
#   -t <seconds>  Timeout per test (default: 10)

set -euo pipefail

XRAY_BIN="${XRAY_BIN:-./build-release/xray}"
FILTER=""
VERBOSE=0
TIMEOUT=10
TEST_DIR="$(cd "$(dirname "$0")/../tests/jit" && pwd)"

while getopts "b:f:vt:" opt; do
    case $opt in
        b) XRAY_BIN="$OPTARG" ;;
        f) FILTER="$OPTARG" ;;
        v) VERBOSE=1 ;;
        t) TIMEOUT="$OPTARG" ;;
        *) echo "Usage: $0 [-b binary] [-f filter] [-v] [-t timeout]"; exit 1 ;;
    esac
done

if [ ! -x "$XRAY_BIN" ]; then
    echo "Error: xray binary not found at $XRAY_BIN"
    echo "Build with: cmake --build build-release --target xray"
    exit 1
fi

TOTAL=0
PASS=0
FAIL=0
SKIP=0
TIMEOUT_COUNT=0
CRASH_COUNT=0
FAILURES=""

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    GREEN='\033[0;32m' RED='\033[0;31m' YELLOW='\033[0;33m' NC='\033[0m'
else
    GREEN='' RED='' YELLOW='' NC=''
fi

for test_file in "$TEST_DIR"/*.xr; do
    [ -f "$test_file" ] || continue

    test_name=$(basename "$test_file" .xr)

    # Apply filter
    if [ -n "$FILTER" ] && [[ ! "$test_name" == *"$FILTER"* ]]; then
        continue
    fi

    TOTAL=$((TOTAL + 1))

    # Extract expected output from first comment line: // EXPECTED: <output>
    expected=""
    while IFS= read -r line; do
        if [[ "$line" =~ ^//\ EXPECTED:\ (.*) ]]; then
            if [ -n "$expected" ]; then
                expected="$expected
${BASH_REMATCH[1]}"
            else
                expected="${BASH_REMATCH[1]}"
            fi
        elif [[ "$line" =~ ^//\ SKIP ]]; then
            SKIP=$((SKIP + 1))
            printf "  [%-3d] %-45s ... ${YELLOW}SKIP${NC}\n" "$TOTAL" "$test_name"
            continue 2
        elif [[ ! "$line" =~ ^// ]]; then
            break
        fi
    done < "$test_file"

    if [ -z "$expected" ]; then
        SKIP=$((SKIP + 1))
        printf "  [%-3d] %-45s ... ${YELLOW}SKIP (no EXPECTED)${NC}\n" "$TOTAL" "$test_name"
        continue
    fi

    # Run test with timeout, suppress stderr (JIT debug output)
    actual=$(timeout "$TIMEOUT" "$XRAY_BIN" "$test_file" 2>/dev/null) || rc=$?
    rc=${rc:-0}

    if [ "$rc" -eq 124 ]; then
        TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
        printf "  [%-3d] %-45s ... ${YELLOW}TIMEOUT${NC}\n" "$TOTAL" "$test_name"
        FAILURES="$FAILURES\n  TIMEOUT: $test_name"
        continue
    fi

    if [ "$rc" -eq 134 ] || [ "$rc" -eq 139 ] || [ "$rc" -eq 136 ]; then
        CRASH_COUNT=$((CRASH_COUNT + 1))
        printf "  [%-3d] %-45s ... ${RED}CRASH (exit=$rc)${NC}\n" "$TOTAL" "$test_name"
        FAILURES="$FAILURES\n  CRASH($rc): $test_name"
        continue
    fi

    if [ "$rc" -ne 0 ]; then
        FAIL=$((FAIL + 1))
        printf "  [%-3d] %-45s ... ${RED}FAIL (exit=$rc)${NC}\n" "$TOTAL" "$test_name"
        if [ "$VERBOSE" -eq 1 ]; then
            echo "    Output: $actual"
        fi
        FAILURES="$FAILURES\n  FAIL($rc): $test_name"
        continue
    fi

    if [ "$actual" = "$expected" ]; then
        PASS=$((PASS + 1))
        printf "  [%-3d] %-45s ... ${GREEN}PASS${NC}\n" "$TOTAL" "$test_name"
    else
        FAIL=$((FAIL + 1))
        printf "  [%-3d] %-45s ... ${RED}FAIL${NC}\n" "$TOTAL" "$test_name"
        if [ "$VERBOSE" -eq 1 ]; then
            echo "    Expected: $expected"
            echo "    Actual:   $actual"
        fi
        FAILURES="$FAILURES\n  MISMATCH: $test_name"
    fi
done

echo ""
echo "======================================"
echo "JIT Test Summary"
echo "======================================"
echo "Total:   $TOTAL"
echo "Pass:    $PASS"
echo "Fail:    $FAIL"
echo "Crash:   $CRASH_COUNT"
echo "Timeout: $TIMEOUT_COUNT"
echo "Skip:    $SKIP"

if [ -n "$FAILURES" ]; then
    echo ""
    echo "Failures:"
    echo -e "$FAILURES"
fi

if [ "$FAIL" -eq 0 ] && [ "$CRASH_COUNT" -eq 0 ] && [ "$TIMEOUT_COUNT" -eq 0 ]; then
    echo ""
    echo "🎉 All JIT tests passed!"
    exit 0
else
    exit 1
fi
