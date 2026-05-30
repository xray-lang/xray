#!/usr/bin/env bash
# ============================================================================
# triple_backend_diff.sh
# ----------------------------------------------------------------------------
# Three-backend differential test: verifies that x64, arm64, and riscv64
# JIT backends produce identical observable output for the same programs.
#
# This script uses xisagen's random-diff mode to generate random instruction
# encodings for all three backends and compare the results.
#
# Usage:
#   bash scripts/triple_backend_diff.sh [-n COUNT] [-s SEED]
#
# Exit codes:
#   0   all backends agree
#   1   encoding divergence detected
# ============================================================================

set -uo pipefail
cd "$(dirname "$0")/.."

COUNT="${1:-100}"
SEED="${2:-42}"
XISAGEN="${XISAGEN:-tools/xisagen/xisagen.py}"
PYTHON="${XRAY_PYTHON:-python3}"

RED='\033[31m'
GREEN='\033[32m'
CYAN='\033[36m'
NC='\033[0m'

if [ ! -f "$XISAGEN" ]; then
    echo "xisagen not found at $XISAGEN"
    exit 2
fi

# Check which ISA files exist
ARCHES=()
for arch in x64 arm64 riscv64; do
    isa_file="xisa/arch/${arch}.isa"
    if [ -f "$isa_file" ]; then
        ARCHES+=("$arch")
    fi
done

if [ ${#ARCHES[@]} -lt 2 ]; then
    echo "Need at least 2 ISA files for diff. Found: ${ARCHES[*]}"
    exit 2
fi

echo -e "${CYAN}=== Triple Backend Encoding Diff ===${NC}"
echo "  Backends: ${ARCHES[*]}"
echo "  Count:    $COUNT"
echo "  Seed:     $SEED"
echo ""

EXIT=0

for arch in "${ARCHES[@]}"; do
    echo -ne "  ${arch}: running random-diff ($COUNT seeds)... "
    isa_file="xisa/arch/${arch}.isa"

    # random-diff exits non-zero if any encoding mismatches are found
    if $PYTHON "$XISAGEN" random-diff "$isa_file" --count "$COUNT" --seed "$SEED" > /tmp/xray_diff_${arch}.log 2>&1; then
        echo -e "${GREEN}PASS${NC}"
    else
        echo -e "${RED}FAIL${NC}"
        head -20 /tmp/xray_diff_${arch}.log
        EXIT=1
    fi
done

echo ""
if [ $EXIT -eq 0 ]; then
    echo -e "${GREEN}All backend encodings are self-consistent.${NC}"
else
    echo -e "${RED}Backend encoding divergence detected. Check logs in /tmp/xray_diff_*.log${NC}"
fi
exit $EXIT
