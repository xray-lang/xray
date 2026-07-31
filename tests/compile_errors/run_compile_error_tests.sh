#!/bin/bash
# run_compile_error_tests.sh
# Test that certain code files produce compile errors (not crashes).
#
# Each `<case>.xr` must be rejected by the COMPILER: a non-zero exit alone does
# not count, because a program that compiles and then panics also exits
# non-zero. A recognisable compiler diagnostic must appear in the output.
#
# Sibling files:
#   <case>.xr.expected          every non-empty line must appear in the output.
#                               Pins the error code and wording.
#   <case>.xr.expected-runtime  same matching, but declares that the compiler
#                               does NOT reject this case and the program only
#                               traps at run time. Reported separately as a
#                               compile-time coverage gap, never as a pass.
#                               Rename to .expected once the diagnostic moves to
#                               compile time.
#
# Writing a good case: make the missing diagnostic the ONLY defect, so the
# program would otherwise compile and run to completion. If the body also blows
# up at run time for an unrelated reason, the case can look "rejected" for the
# wrong reason.
#
# Execution model: cases are independent, so they run in parallel (one xray
# process each) via `xargs -P`. Each case writes a self-contained result file
# under a category subdirectory mirroring the source layout; the parent then
# reads those back in sorted (category/filename) order, so the printed report
# and the pass/fail/runtime-only tallies are byte-for-byte deterministic
# regardless of the order the cases actually finished in.
#   XRAY_TEST_JOBS   parallelism (default: number of CPUs)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Accept XRAY or XRAY_BIN (t.sh and other callers use the latter); fall back to
# the in-tree default.
XRAY="${XRAY:-${XRAY_BIN:-$SCRIPT_DIR/../../build/xray}}"

if [ ! -x "$XRAY" ]; then
    echo "Error: xray not found at $XRAY"
    echo "Build xray first or set XRAY environment variable"
    exit 1
fi

PARALLEL_JOBS=${XRAY_TEST_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}

# Colors
RED=$'\033[0;31m'
GREEN=$'\033[0;32m'
YELLOW=$'\033[0;33m'
NC=$'\033[0m' # No Color

# Per-case results land here, one file per case under a category subdirectory
# that mirrors the source tree. The whole tree is thrown away on exit.
RESULTS_DIR="$(mktemp -d "${TMPDIR:-/tmp}/xray-compile-errors.XXXXXX")"
trap 'rm -rf "${RESULTS_DIR}"' EXIT

# ---------------------------------------------------------------------------
# run_one_case <case.xr>
#
# Classifies a single case and writes ONE result file. The file's first line is
# a machine verdict for the tally (PASS / FAIL / RUNTIME); the remaining lines
# are the human-readable report block the parent prints verbatim. Keeping both
# in the same file is what lets the parent stay agnostic to completion order.
# The result path mirrors <category>/<filename> so the parent's category
# subdirectories already exist (created before fan-out — no concurrent mkdir).
# ---------------------------------------------------------------------------
run_one_case() {
    local file="$1"
    # Pure parameter expansion — no dirname/basename forks on the hot path.
    #   file = .../compile_errors/<category>/<case>.xr
    local d="${file%/*}"          # .../compile_errors/<category>
    local dir="${d}/"
    local category="${d##*/}"
    local filename="${file##*/}"

    local result_file="${RESULTS_DIR}/${category}/${filename}.result"

    # A case in this suite must be rejected by the COMPILER, and a non-zero exit
    # alone does not prove that: a program that compiles and then panics exits
    # non-zero too. Such a case used to be scored a pass with its .expected file
    # never consulted, so it stayed green even when the diagnostic it was written
    # to pin had stopped firing entirely. Requiring a recognisable compiler
    # diagnostic closes that hole; a bare runtime panic carries none. The
    # accepted diagnostic shapes are `error[E0123]:` / `error:` from the
    # analyzer, `Error:` from module resolution, and the `[xcompiler] ... failed
    # at <stage>:` line from the Xi pipeline.
    local typepath="$dir"
    if [ -n "${XRAY_TYPEPATH:-}" ]; then
        typepath="$dir:$XRAY_TYPEPATH"
    fi

    local raw_output exit_code output
    raw_output=$(XRAY_TYPEPATH="$typepath" "$XRAY" "$file" 2>&1)
    exit_code=$?
    # xray emits plain, un-coloured diagnostics whenever stderr is not a
    # terminal (see xr_diag_use_color in src/frontend/xdiag_fmt.h), which is
    # always the case here since the output is captured through a pipe. So there
    # are no ANSI escapes to strip — no per-case sed fork, no bash pattern pass.
    output="$raw_output"

    # A `.expected-runtime` sibling declares that the compiler does not currently
    # reject this case at all and the program only traps once it runs. Such a
    # case still has to be rejected, and its message still has to match, but it
    # is reported apart from the compile-time passes so the gap stays visible.
    local expected_file kind
    if [ -f "${file}.expected-runtime" ]; then
        expected_file="${file}.expected-runtime"
        kind="runtime"
    else
        expected_file="${file}.expected"
        kind="compile"
    fi

    local missing=""
    if [ -f "$expected_file" ]; then
        while IFS= read -r line; do
            [ -z "$line" ] && continue
            # bash substring test — quoted $line matches literally, no grep fork.
            if [[ "$output" != *"$line"* ]]; then
                missing="$missing"$'\n'"      - $line"
            fi
        done < "$expected_file"
    fi

    if [ "$exit_code" -eq 0 ]; then
        {
            printf 'FAIL\n'
            printf '  %s✗%s %s - should have failed but succeeded\n' "$RED" "$NC" "$filename"
        } > "$result_file"
    elif [ "$kind" = "compile" ] && ! [[ "$output" =~ $XR_DIAG_RE ]]; then
        {
            printf 'FAIL\n'
            printf '  %s✗%s %s - exited non-zero with no compiler diagnostic (a run-time panic is not a compile error)\n' "$RED" "$NC" "$filename"
            printf '    Output: %s\n' "$output"
        } > "$result_file"
    elif [ -n "$missing" ]; then
        {
            printf 'FAIL\n'
            printf '  %s✗%s %s - error message missing expected lines:%b\n' "$RED" "$NC" "$filename" "$missing"
            printf '    Output: %s\n' "$output"
        } > "$result_file"
    elif [ "$kind" = "runtime" ]; then
        {
            printf 'RUNTIME\n'
            printf '  %s~%s %s - rejected, but only at run time\n' "$YELLOW" "$NC" "$filename"
        } > "$result_file"
    else
        {
            printf 'PASS\n'
            printf '  %s✓%s %s - correctly rejected\n' "$GREEN" "$NC" "$filename"
        } > "$result_file"
    fi
}
# Accepted compiler-diagnostic shapes: `error[E0123]:` / `error:` from the
# analyzer, `Error:` from module resolution, and the `[xcompiler] ... failed at
# <stage>:` line from the Xi pipeline. A bare runtime panic matches none of
# these. Matched with bash `[[ =~ ]]` (no grep fork); validated equivalent to
# the previous `grep -qE` across all cases.
XR_DIAG_RE='(^|[^A-Za-z])([Ee]rror(\[E[0-9]+\])?:|\[xcompiler\].*failed at )'

export -f run_one_case
export XRAY RESULTS_DIR RED GREEN YELLOW NC XR_DIAG_RE

echo "Running compile error tests... (${PARALLEL_JOBS} parallel)"
echo "========================================"

# Enumerate cases with exactly the glob the old nested loops used — one
# directory level of categories, `*.xr` directly inside each — so the case set
# is identical (and it stays bash-3.2 compatible: no mapfile). Pre-create each
# category's result subdirectory here so the parallel workers never race on
# mkdir.
CASES=()
for dir in "$SCRIPT_DIR"/*/; do
    [ -d "$dir" ] || continue
    mkdir -p "${RESULTS_DIR}/$(basename "$dir")"
    for file in "$dir"*.xr; do
        [ -f "$file" ] || continue
        CASES+=("$file")
    done
done
if [ "${#CASES[@]}" -eq 0 ]; then
    echo "No compile-error cases found under $SCRIPT_DIR"
    exit 1
fi

# NUL-delimited so paths with spaces are safe; -n batches several cases per
# bash so the interpreter start-up is amortized instead of paid once per case.
printf '%s\0' "${CASES[@]}" |
    xargs -0 -P "${PARALLEL_JOBS}" -n 16 bash -c 'for f in "$@"; do run_one_case "$f"; done' _

# ---------------------------------------------------------------------------
# Collect. Walk the result files in sorted order so output is grouped by
# category exactly like before, printing a header when the category changes.
# ---------------------------------------------------------------------------
PASSED=0
FAILED=0
TOTAL=0
RUNTIME_ONLY=0
RUNTIME_ONLY_LIST=""
prev_category=""

while IFS= read -r result_file; do
    [ -f "$result_file" ] || continue
    TOTAL=$((TOTAL + 1))

    # Parse category/filename by parameter expansion — this loop runs once per
    # case, so basename/dirname here would be thousands of serial forks.
    #   result_file = $RESULTS_DIR/<category>/<filename>.result
    rf_dir="${result_file%/*}"
    category="${rf_dir##*/}"
    filename="${result_file##*/}"
    filename="${filename%.result}"

    if [ "$category" != "$prev_category" ]; then
        echo ""
        echo "Category: $category"
        echo "----------------------------------------"
        prev_category="$category"
    fi

    # Read the verdict (first line) and stream the report block (the rest) in a
    # single fork-free pass. A per-file head + tail was ~3s of pure fork cost
    # across the corpus — larger than the actual test work.
    verdict=""
    _first=1
    while IFS= read -r _line || [ -n "$_line" ]; do
        if [ "$_first" = 1 ]; then
            verdict="$_line"
            _first=0
        else
            printf '%s\n' "$_line"
        fi
    done < "$result_file"

    case "$verdict" in
        PASS)    PASSED=$((PASSED + 1)) ;;
        FAIL)    FAILED=$((FAILED + 1)) ;;
        RUNTIME)
            RUNTIME_ONLY=$((RUNTIME_ONLY + 1))
            RUNTIME_ONLY_LIST="$RUNTIME_ONLY_LIST"$'\n'"  - $category/$filename"
            ;;
    esac
done < <(find "${RESULTS_DIR}" -name '*.result' -type f | sort)

echo ""
echo "========================================"
echo "Compile Error Tests Summary"
echo "========================================"
echo -e "Passed: ${GREEN}$PASSED${NC}"
echo -e "Failed: ${RED}$FAILED${NC}"
if [ "$RUNTIME_ONLY" -gt 0 ]; then
    echo -e "Runtime-only (compile-time coverage gap): ${YELLOW}$RUNTIME_ONLY${NC}"
fi
echo "Total:  $TOTAL"
echo "========================================"
if [ "$RUNTIME_ONLY" -gt 0 ]; then
    echo ""
    echo "These cases are rejected only once the program runs; the compiler"
    echo "accepts them. Each carries a .expected-runtime sibling saying so."
    echo "Rename it to .expected when the diagnostic moves to compile time:"
    echo -e "$RUNTIME_ONLY_LIST"
fi

if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
