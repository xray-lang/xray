#!/bin/bash
# JIT Differential Test Runner (Parallel)
# Runs tests with --jit-force (threshold=1) vs --no-jit (interpreter only),
# comparing output to detect JIT correctness issues.
#
# WHY --jit-force:
#   Default JIT threshold is 100 calls. Regression tests call @test functions
#   only once, so default mode never actually triggers JIT compilation.
#   --jit-force sets threshold=1, ensuring every function gets JIT compiled.
#
# Usage: ./scripts/run_jit_diff_tests.sh [options]
#   -b <binary>   Path to xray binary (default: auto-detect)
#   -f <filter>   Run only tests matching filter
#   -v            Verbose mode (show diff on mismatch)
#   -t <seconds>  Timeout per test (default: 10)
#   -d <dir>      Test directory (default: tests/regression)
#   -j <N>        Parallel jobs (default: nproc, use -j1 for serial)
#   -J            Also run tests/jit directory

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Auto-detect build directory
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
FILTER=""
VERBOSE=0
TIMEOUT=10
TEST_DIR="${PROJECT_ROOT}/tests/regression"
INCLUDE_JIT_DIR=0
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
ALLOWLIST="${PROJECT_ROOT}/tests/jit/known_failures.txt"
USE_ALLOWLIST=1

while getopts "b:f:vt:d:j:Ja:S" opt; do
    case $opt in
        b) XRAY_BIN="$OPTARG" ;;
        f) FILTER="$OPTARG" ;;
        v) VERBOSE=1 ;;
        t) TIMEOUT="$OPTARG" ;;
        d) TEST_DIR="$OPTARG" ;;
        j) JOBS="$OPTARG" ;;
        J) INCLUDE_JIT_DIR=1 ;;
        a) ALLOWLIST="$OPTARG" ;;
        S) USE_ALLOWLIST=0 ;;
        *) echo "Usage: $0 [-b binary] [-f filter] [-v] [-t timeout] [-d dir] [-j jobs] [-J] [-a allowlist] [-S]"; exit 1 ;;
    esac
done

# Load allowlist (known failures) — bash 3 compatible (no assoc arrays)
KNOWN_FAILURES_FILE=$(mktemp /tmp/jit_diff_known.XXXXXX)
if [ "$USE_ALLOWLIST" -eq 1 ] && [ -f "$ALLOWLIST" ]; then
    # `grep -v '^$'` returns rc=1 when allowlist is effectively empty,
    # which combined with `set -o pipefail` would abort the script.
    # Tolerate empty allowlists explicitly.
    sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "$ALLOWLIST" \
        | { grep -v '^$' || true; } > "$KNOWN_FAILURES_FILE"
fi
is_known_failure() { grep -qxF "$1" "$KNOWN_FAILURES_FILE" 2>/dev/null; }

if [ ! -x "$XRAY_BIN" ]; then
    echo "Error: xray binary not found at $XRAY_BIN"
    exit 1
fi

# Colors (disabled for non-TTY / NO_COLOR)
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    GREEN='\033[0;32m' RED='\033[0;31m' YELLOW='\033[0;33m' BLUE='\033[0;34m' NC='\033[0m'
else
    GREEN='' RED='' YELLOW='' BLUE='' NC=''
fi

# Temp directory for parallel results
TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR; rm -f $KNOWN_FAILURES_FILE" EXIT

# Collect test files
test_files=()
while IFS= read -r -d '' f; do
    test_files+=("$f")
done < <(find "$TEST_DIR" -name "*.xr" -type f -print0 | sort -z)

if [ "$INCLUDE_JIT_DIR" -eq 1 ] && [ -d "${PROJECT_ROOT}/tests/jit" ]; then
    while IFS= read -r -d '' f; do
        test_files+=("$f")
    done < <(find "${PROJECT_ROOT}/tests/jit" -name "*.xr" -type f -print0 | sort -z)
fi

# Filter and assign indices
filtered_files=()
for test_file in "${test_files[@]}"; do
    test_name=$(basename "$test_file" .xr)
    if [[ "$test_name" == _* ]]; then
        continue
    fi
    rel_path="${test_file#$PROJECT_ROOT/}"
    if [ -n "$FILTER" ] && [[ ! "$rel_path" == *"$FILTER"* ]]; then
        continue
    fi
    filtered_files+=("$test_file")
done

TOTAL=${#filtered_files[@]}

echo -e "${BLUE}======================================"
echo "JIT Differential Test Runner"
echo "======================================${NC}"
echo "Binary:    $XRAY_BIN"
echo "Test dir:  $TEST_DIR"
echo "Timeout:   ${TIMEOUT}s per mode"
echo "Parallel:  ${JOBS} jobs"
echo "Tests:     ${TOTAL}"
echo "Strategy:  --jit-force vs --no-jit"
echo ""

start_time=$(date +%s)

# Worker function: run one test, write result to tmp file
# Result format: STATUS|rc_interp|rc_jit|rel_path
run_one_test() {
    local idx=$1
    local test_file=$2
    local xray=$3
    local timeout_sec=$4
    local project_root=$5
    local tmp_dir=$6
    local verbose=$7

    local rel_path="${test_file#$project_root/}"
    local result_file="${tmp_dir}/${idx}.result"

    # Run interp and jit in parallel (key optimization: halves per-file time)
    local out_interp_file="${tmp_dir}/${idx}.interp"
    local out_jit_file="${tmp_dir}/${idx}.jit"
    local err_interp_file="${tmp_dir}/${idx}.interp.err"
    local err_jit_file="${tmp_dir}/${idx}.jit.err"
    local rc_interp_file="${tmp_dir}/${idx}.rc_interp"
    local rc_jit_file="${tmp_dir}/${idx}.rc_jit"

    # Subshells use set +e so echo $? runs even after crash signals
    (set +e; timeout "$timeout_sec" "$xray" test --no-jit "$test_file" >"$out_interp_file" 2>"$err_interp_file"; echo $? >"$rc_interp_file") 2>/dev/null &
    local pid_interp=$!
    (set +e; timeout "$timeout_sec" "$xray" test --jit-force "$test_file" >"$out_jit_file" 2>"$err_jit_file"; echo $? >"$rc_jit_file") 2>/dev/null &
    local pid_jit=$!
    wait $pid_interp $pid_jit 2>/dev/null

    local rc_interp=$(cat "$rc_interp_file")
    local rc_jit=$(cat "$rc_jit_file")

    local status=""
    local detail=""

    # Timeout
    if [ "$rc_interp" -eq 124 ] && [ "$rc_jit" -eq 124 ]; then
        status="TIMEOUT_BOTH"
    elif [ "$rc_jit" -eq 124 ]; then
        status="TIMEOUT_JIT"
    # JIT crash (SIGABRT=134, SIGSEGV=139, SIGFPE=136)
    elif [ "$rc_jit" -eq 134 ] || [ "$rc_jit" -eq 139 ] || [ "$rc_jit" -eq 136 ]; then
        if [ "$rc_interp" -eq 0 ] || { [ "$rc_interp" -ne 134 ] && [ "$rc_interp" -ne 139 ] && [ "$rc_interp" -ne 136 ]; }; then
            status="CRASH"
        else
            status="BOTH_FAIL"
        fi
    # Both fail with non-zero
    elif [ "$rc_interp" -ne 0 ] && [ "$rc_jit" -ne 0 ]; then
        status="BOTH_FAIL"
    # Exit code mismatch
    elif [ "$rc_interp" -ne "$rc_jit" ]; then
        status="EXIT_DIFF"
    else
        # Both succeed — compare output
        local norm_interp norm_jit
        norm_interp=$(sed -E -e 's/\([0-9]+ms\)/(_ms)/g' -e 's/[0-9]+ms/_ms/g' <"$out_interp_file")
        norm_jit=$(sed -E -e 's/\([0-9]+ms\)/(_ms)/g' -e 's/[0-9]+ms/_ms/g' <"$out_jit_file")
        if [ "$norm_interp" = "$norm_jit" ]; then
            # Guard: check at least one @test actually executed
            # (summary may appear in stdout or stderr depending on --quiet)
            local exec_i
            exec_i=$(cat "$out_interp_file" "$err_interp_file" 2>/dev/null | grep -oE '[0-9]+ passed' | head -1 | grep -oE '[0-9]+' || true)
            if [ -z "$exec_i" ] || [ "${exec_i:-0}" -eq 0 ]; then
                status="NO_TESTS"
            else
                status="PASS"
            fi
        else
            status="OUTPUT_DIFF"
            if [ "$verbose" -eq 1 ]; then
                detail=$(printf "\n    --- interpreter ---\n%s\n    --- jit ---\n%s\n    ---" \
                    "$(head -5 "$out_interp_file")" "$(head -5 "$out_jit_file")")
            fi
        fi
    fi

    printf "%s|%d|%d|%s|%s\n" "$status" "$rc_interp" "$rc_jit" "$rel_path" "$detail" > "$result_file"
}

# Run all tests in parallel using FIFO semaphore job pool
# Unlike batch wait, a slow/timeout test only blocks one slot, not the whole batch
mkfifo "$TMP_DIR/sem"
exec 3<>"$TMP_DIR/sem"
for ((s=0; s<JOBS; s++)); do echo >&3; done

for i in "${!filtered_files[@]}"; do
    read -u3
    {
        run_one_test "$i" "${filtered_files[$i]}" "$XRAY_BIN" "$TIMEOUT" "$PROJECT_ROOT" "$TMP_DIR" "$VERBOSE"
        echo >&3
    } &
done
wait
exec 3>&-

# Collect and display results (in order)
PASS=0
DIFF_FAIL=0
BOTH_FAIL=0
JIT_CRASH=0
TIMEOUT_COUNT=0
NO_TESTS_COUNT=0
FAILURES=""
KNOWN_COUNT=0
UNEXPECTED_COUNT=0

for i in "${!filtered_files[@]}"; do
    result_file="${TMP_DIR}/${i}.result"
    if [ ! -f "$result_file" ]; then
        printf "  [%-3d] %-55s ${RED}ERROR (no result)${NC}\n" "$((i+1))" "$(basename "${filtered_files[$i]}")"
        continue
    fi

    IFS='|' read -r status rc_interp rc_jit rel_path detail < "$result_file"
    local_idx=$((i + 1))

    case "$status" in
        PASS)
            PASS=$((PASS + 1))
            printf "  [%-3d] %-55s ${GREEN}PASS${NC}\n" "$local_idx" "$rel_path"
            ;;
        NO_TESTS)
            NO_TESTS_COUNT=$((NO_TESTS_COUNT + 1))
            printf "  [%-3d] %-55s ${YELLOW}NO_TESTS${NC}\n" "$local_idx" "$rel_path"
            ;;
        TIMEOUT_BOTH)
            TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
            printf "  [%-3d] %-55s ${YELLOW}TIMEOUT (both)${NC}\n" "$local_idx" "$rel_path"
            ;;
        TIMEOUT_JIT)
            TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
            printf "  [%-3d] %-55s ${YELLOW}TIMEOUT (jit only)${NC}\n" "$local_idx" "$rel_path"
            FAILURES="$FAILURES\n  TIMEOUT(jit): $rel_path"
            ;;
        CRASH)
            JIT_CRASH=$((JIT_CRASH + 1))
            if is_known_failure "$rel_path"; then
                KNOWN_COUNT=$((KNOWN_COUNT + 1))
                printf "  [%-3d] %-55s ${YELLOW}CRASH (known)${NC}\n" "$local_idx" "$rel_path"
            else
                UNEXPECTED_COUNT=$((UNEXPECTED_COUNT + 1))
                printf "  [%-3d] %-55s ${RED}CRASH (jit exit=%d, interp exit=%d)${NC}\n" "$local_idx" "$rel_path" "$rc_jit" "$rc_interp"
                FAILURES="$FAILURES\n  CRASH(jit=$rc_jit): $rel_path"
            fi
            ;;
        BOTH_FAIL)
            BOTH_FAIL=$((BOTH_FAIL + 1))
            printf "  [%-3d] %-55s ${YELLOW}BOTH_FAIL (interp=%d, jit=%d)${NC}\n" "$local_idx" "$rel_path" "$rc_interp" "$rc_jit"
            ;;
        EXIT_DIFF)
            DIFF_FAIL=$((DIFF_FAIL + 1))
            if is_known_failure "$rel_path"; then
                KNOWN_COUNT=$((KNOWN_COUNT + 1))
                printf "  [%-3d] %-55s ${YELLOW}EXIT_DIFF (known)${NC}\n" "$local_idx" "$rel_path"
            else
                UNEXPECTED_COUNT=$((UNEXPECTED_COUNT + 1))
                printf "  [%-3d] %-55s ${RED}EXIT_DIFF (interp=%d, jit=%d)${NC}\n" "$local_idx" "$rel_path" "$rc_interp" "$rc_jit"
                FAILURES="$FAILURES\n  EXIT_DIFF(interp=$rc_interp,jit=$rc_jit): $rel_path"
            fi
            ;;
        OUTPUT_DIFF)
            DIFF_FAIL=$((DIFF_FAIL + 1))
            if is_known_failure "$rel_path"; then
                KNOWN_COUNT=$((KNOWN_COUNT + 1))
                printf "  [%-3d] %-55s ${YELLOW}OUTPUT_DIFF (known)${NC}\n" "$local_idx" "$rel_path"
            else
                UNEXPECTED_COUNT=$((UNEXPECTED_COUNT + 1))
                printf "  [%-3d] %-55s ${RED}OUTPUT_DIFF${NC}\n" "$local_idx" "$rel_path"
                FAILURES="$FAILURES\n  OUTPUT_DIFF: $rel_path"
            fi
            if [ -n "$detail" ]; then
                echo "$detail"
            fi
            ;;
        *)
            printf "  [%-3d] %-55s ${RED}UNKNOWN(%s)${NC}\n" "$local_idx" "$rel_path" "$status"
            ;;
    esac
done

echo ""
echo -e "${BLUE}======================================"
echo "JIT Differential Test Summary"
echo "======================================${NC}"
end_time=$(date +%s)
elapsed=$((end_time - start_time))
echo "Total:      $TOTAL"
echo "Elapsed:    ${elapsed}s"
echo -e "${GREEN}Pass:       $PASS${NC}"
echo -e "${RED}Diff fail:  $DIFF_FAIL${NC}"
echo -e "${RED}JIT crash:  $JIT_CRASH${NC}"
echo -e "${YELLOW}Both fail:  $BOTH_FAIL${NC}  (pre-existing, not JIT bugs)"
echo -e "${YELLOW}Timeout:    $TIMEOUT_COUNT${NC}"
if [ "$NO_TESTS_COUNT" -gt 0 ]; then
    echo -e "${YELLOW}No tests:   $NO_TESTS_COUNT${NC}  (files with no @test functions)"
fi
if [ "$USE_ALLOWLIST" -eq 1 ]; then
    echo -e "${YELLOW}Known:      $KNOWN_COUNT${NC}  (in allowlist)"
    echo -e "${RED}Unexpected: $UNEXPECTED_COUNT${NC}  (NEW regressions)"
fi

if [ -n "$FAILURES" ]; then
    echo ""
    echo -e "${RED}Unexpected JIT failures (NOT in allowlist):${NC}"
    echo -e "$FAILURES"
fi

echo ""
if [ "$UNEXPECTED_COUNT" -eq 0 ]; then
    if [ "$KNOWN_COUNT" -gt 0 ]; then
        echo -e "${GREEN}All JIT differential tests passed (${KNOWN_COUNT} known failures in allowlist).${NC}"
    else
        echo -e "${GREEN}All JIT differential tests passed!${NC}"
    fi
    exit 0
else
    echo -e "${RED}${UNEXPECTED_COUNT} unexpected JIT failure(s) detected!${NC}"
    exit 1
fi
