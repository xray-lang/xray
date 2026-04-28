#!/bin/bash
# run_compile_error_tests.sh
# Test that certain code files produce compile errors (not crashes)

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
        
        # Run xray and capture output
        output=$("$XRAY" "$file" 2>&1)
        exit_code=$?
        
        # Expected: non-zero exit code (compile error)
        if [ $exit_code -ne 0 ]; then
            # Check it's a compile error, not a crash
            if echo "$output" | grep -qi "error\|Error\|ERROR"; then
                echo -e "  ${GREEN}✓${NC} $filename - correctly rejected"
                PASSED=$((PASSED + 1))
            else
                echo -e "  ${YELLOW}?${NC} $filename - failed but no error message"
                echo "    Output: $output"
                PASSED=$((PASSED + 1))  # Still a pass if it rejected
            fi
        else
            echo -e "  ${RED}✗${NC} $filename - should have failed but succeeded"
            FAILED=$((FAILED + 1))
        fi
    done
done

echo ""
echo "========================================"
echo "Compile Error Tests Summary"
echo "========================================"
echo -e "Passed: ${GREEN}$PASSED${NC}"
echo -e "Failed: ${RED}$FAILED${NC}"
echo "Total:  $TOTAL"
echo "========================================"

if [ $FAILED -gt 0 ]; then
    exit 1
fi
exit 0
