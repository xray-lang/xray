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

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
XRAY="${XRAY:-$SCRIPT_DIR/../../build/xray}"

if [ ! -x "$XRAY" ]; then
    echo "Error: xray not found at $XRAY"
    echo "Build xray first or set XRAY environment variable"
    exit 1
fi

PASSED=0
FAILED=0
TOTAL=0
# Cases that are only rejected once the program runs. Counted separately so the
# missing compile-time coverage stays visible instead of hiding inside PASSED.
RUNTIME_ONLY=0
RUNTIME_ONLY_LIST=""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color

echo "Running compile error tests..."
echo "========================================"

for dir in "$SCRIPT_DIR"/*/; do
    category=$(basename "$dir")
    echo ""
    echo "Category: $category"
    echo "----------------------------------------"

    for file in "$dir"*.xr; do
        [ -f "$file" ] || continue

        TOTAL=$((TOTAL + 1))
        filename=$(basename "$file")

        # Run xray and capture output; strip ANSI color escape codes so
        # .expected substring matching is robust regardless of terminal.
        typepath="$dir"
        if [ -n "${XRAY_TYPEPATH:-}" ]; then
            typepath="$dir:$XRAY_TYPEPATH"
        fi
        raw_output=$(XRAY_TYPEPATH="$typepath" "$XRAY" "$file" 2>&1)
        exit_code=$?
        output=$(printf '%s' "$raw_output" | sed -E $'s/\x1B\\[[0-9;]*[a-zA-Z]//g')

        # A case in this suite must be rejected by the COMPILER, and a non-zero
        # exit alone does not prove that: a program that compiles and then
        # panics exits non-zero too. Such a case used to be scored a pass with
        # its .expected file never consulted, so it stayed green even when the
        # diagnostic it was written to pin had stopped firing entirely.
        # Requiring a recognisable compiler diagnostic closes that hole; a
        # bare runtime panic carries none. The accepted diagnostic shapes are
        # `error[E0123]:` / `error:` from the analyzer, `Error:` from module
        # resolution, and the `[xcompiler] ... failed at <stage>:` line from the
        # Xi pipeline.
        # A `.expected-runtime` sibling declares that the compiler does not
        # currently reject this case at all and the program only traps once it
        # runs. Such a case still has to be rejected, and its message still has
        # to match, but it is reported apart from the compile-time passes so the
        # gap is visible. Delete the file (rename it to `.expected`) once the
        # diagnostic moves to compile time.
        runtime_expected_file="${file}.expected-runtime"
        if [ -f "$runtime_expected_file" ]; then
            expected_file="$runtime_expected_file"
            kind="runtime"
        else
            expected_file="${file}.expected"
            kind="compile"
        fi

        missing=""
        if [ -f "$expected_file" ]; then
            while IFS= read -r line; do
                # Skip empty lines
                [ -z "$line" ] && continue
                if ! echo "$output" | grep -qF "$line"; then
                    missing="$missing\n      - $line"
                fi
            done < "$expected_file"
        fi

        if [ $exit_code -eq 0 ]; then
            echo -e "  ${RED}✗${NC} $filename - should have failed but succeeded"
            FAILED=$((FAILED + 1))
        elif [ "$kind" = "compile" ] && ! echo "$output" |
            grep -qE '(^|[^A-Za-z])([Ee]rror(\[E[0-9]+\])?:|\[xcompiler\].*failed at )'; then
            echo -e "  ${RED}✗${NC} $filename - exited non-zero with no compiler diagnostic (a run-time panic is not a compile error)"
            echo "    Output: $output"
            FAILED=$((FAILED + 1))
        elif [ -n "$missing" ]; then
            echo -e "  ${RED}✗${NC} $filename - error message missing expected lines:$missing"
            echo "    Output: $output"
            FAILED=$((FAILED + 1))
        elif [ "$kind" = "runtime" ]; then
            echo -e "  ${YELLOW}~${NC} $filename - rejected, but only at run time"
            RUNTIME_ONLY=$((RUNTIME_ONLY + 1))
            RUNTIME_ONLY_LIST="$RUNTIME_ONLY_LIST\n  - $category/$filename"
        else
            echo -e "  ${GREEN}✓${NC} $filename - correctly rejected"
            PASSED=$((PASSED + 1))
        fi
    done
done

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

if [ $FAILED -gt 0 ]; then
    exit 1
fi
exit 0
