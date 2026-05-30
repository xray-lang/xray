#!/usr/bin/env bash
# JIT Fuzz Dashboard — runs all fuzz layers and generates a summary report.
#
# Usage: ./scripts/jit_fuzz_dashboard.sh [-n count] [-s seed] [-o report_file]
#
# Runs jit_fuzz.sh -a and collects results into a JSON + text report.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

COUNT=50
SEED=$(date +%s)
REPORT_FILE=""

while getopts "n:s:o:" opt; do
    case $opt in
        n) COUNT="$OPTARG" ;;
        s) SEED="$OPTARG" ;;
        o) REPORT_FILE="$OPTARG" ;;
        *) echo "Usage: $0 [-n count] [-s seed] [-o report_file]"; exit 1 ;;
    esac
done

FUZZ_OUT="${PROJECT_ROOT}/tests/tmp/jit_fuzz"
TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

if [ -z "$REPORT_FILE" ]; then
    REPORT_FILE="${PROJECT_ROOT}/tests/tmp/jit_fuzz_report_$(date +%Y%m%d_%H%M%S).txt"
fi
mkdir -p "$(dirname "$REPORT_FILE")"

echo "======================================"
echo "JIT Fuzz Dashboard"
echo "======================================"
echo "Count:     $COUNT"
echo "Seed:      $SEED"
echo "Report:    $REPORT_FILE"
echo ""

START_TIME=$(date +%s)

RAW_OUTPUT=$(NO_COLOR=1 bash "${SCRIPT_DIR}/jit_fuzz.sh" -a -n "$COUNT" -s "$SEED" 2>&1) && FUZZ_EXIT=0 || FUZZ_EXIT=$?

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

# Parse results from clean (NO_COLOR) output using awk for robustness
extract_section() {
    local header="$1"
    echo "$RAW_OUTPUT" | awk "/$header/{found=1;next} /^Layer |^===/{found=0} found{print}"
}

L1_STATUS=$(echo "$RAW_OUTPUT" | grep -m1 "Layer 1" | grep -oE 'PASS|FAIL' || echo "N/A")

L2_SECTION=$(extract_section "Layer 2")
L2_PASS=$(echo "$L2_SECTION" | grep -m1 "Pass:" | grep -oE '[0-9]+' || echo "0")
L2_CRASH=$(echo "$L2_SECTION" | grep -m1 "Crash:" | grep -oE '[0-9]+' || echo "0")
L2_DIFF=$(echo "$L2_SECTION" | grep -m1 "Diff:" | grep -oE '[0-9]+' || echo "0")

L3_SECTION=$(extract_section "driver stress")
L3_TOTAL=$(echo "$L3_SECTION" | grep -m1 "Total:" | grep -oE '[0-9]+' || echo "0")
L3_PASS=$(echo "$L3_SECTION" | grep -m1 "Pass:" | grep -oE '[0-9]+' || echo "0")
L3_CRASH=$(echo "$L3_SECTION" | grep -m1 "Crash:" | grep -oE '[0-9]+' || echo "0")

L4_SECTION=$(extract_section "corruption-chain")
L4_TOTAL=$(echo "$L4_SECTION" | grep -m1 "Total:" | grep -oE '[0-9]+' || echo "0")
L4_PASS=$(echo "$L4_SECTION" | grep -m1 "Pass:" | grep -oE '[0-9]+' || echo "0")
L4_CRASH=$(echo "$L4_SECTION" | grep -m1 "Crash:" | grep -oE '[0-9]+' || echo "0")
L4_DIFF=$(echo "$L4_SECTION" | grep -m1 "Diff:" | grep -oE '[0-9]+' || echo "0")

# Count failure files
CRASH_FILES=$(find "$FUZZ_OUT" -name "crash_*.xr" -o -name "chain_crash_*.xr" -o -name "stress_crash_*.xr" 2>/dev/null | wc -l | tr -d ' ')
DIFF_FILES=$(find "$FUZZ_OUT" -name "diff_*.xr" -o -name "chain_diff_*.xr" 2>/dev/null | wc -l | tr -d ' ')

# Generate report
{
    echo "======================================"
    echo "JIT Fuzz Dashboard Report"
    echo "======================================"
    echo ""
    echo "Timestamp:  $TIMESTAMP"
    echo "Seed:       $SEED"
    echo "Count:      $COUNT"
    echo "Duration:   ${ELAPSED}s"
    echo "Exit code:  $FUZZ_EXIT"
    echo ""
    echo "--- Layer Results ---"
    echo ""
    printf "%-25s %s\n" "Layer 1 (mcinsn):" "$L1_STATUS"
    echo ""
    printf "%-25s %-8s %-8s %-8s\n" "" "Total" "Pass" "Fail"
    printf "%-25s %-8s %-8s %-8s\n" "Layer 2 (program):" "$COUNT" "$L2_PASS" "$((L2_CRASH + L2_DIFF))"
    printf "%-25s %-8s %-8s %-8s\n" "Layer 3 (driver stress):" "$L3_TOTAL" "$L3_PASS" "$L3_CRASH"
    printf "%-25s %-8s %-8s %-8s\n" "Layer 4 (corruption):" "$L4_TOTAL" "$L4_PASS" "$((L4_CRASH + L4_DIFF))"
    echo ""
    echo "--- Failure Artifacts ---"
    echo ""
    echo "Crash files: $CRASH_FILES"
    echo "Diff files:  $DIFF_FILES"
    if [ "$CRASH_FILES" -gt 0 ] || [ "$DIFF_FILES" -gt 0 ]; then
        echo ""
        echo "Files:"
        find "$FUZZ_OUT" -name "*.xr" -o -name "*.log" 2>/dev/null | sort | while read -r f; do
            echo "  $f"
        done
    fi
    echo ""
    echo "--- Verdict ---"
    echo ""
    if [ "$FUZZ_EXIT" -eq 0 ]; then
        echo "PASS: No JIT bugs found in $COUNT iterations across all layers."
    else
        echo "FAIL: JIT bugs detected. See failure artifacts above."
    fi
    echo ""
    echo "======================================"
} | tee "$REPORT_FILE"

echo ""
echo "Report saved to: $REPORT_FILE"
exit "$FUZZ_EXIT"
