#!/usr/bin/env bash
# ============================================================================
# codegen_diff.sh
# ----------------------------------------------------------------------------
# Local reproduction of external assembler differential testing.
# Combines golden-row llvm-mc diff, generated disasm round-trip, and
# (future) random-seed differential into a single local entry point.
#
# Usage:
#   bash scripts/codegen_diff.sh              # run all available checks
#   bash scripts/codegen_diff.sh --golden     # golden-row llvm-mc only
#   bash scripts/codegen_diff.sh --disasm     # generated disasm round-trip only
#   bash scripts/codegen_diff.sh --random     # random-seed x64 differential only
#   bash scripts/codegen_diff.sh --ctest      # golden + disasm ctest targets only
#
# Environment:
#   RANDOM_DIFF_SEED=N    seed for random differential (default 42)
#   RANDOM_DIFF_COUNT=N   combos per mcinsn (default 20; CI PR=20, nightly=1000)
#
# Prerequisites:
#   - build/ must exist (cmake --build build)
#   - llvm-mc on PATH or at /opt/homebrew/opt/llvm/bin/llvm-mc
#
# Exit codes:
#   0   all checks passed
#   1   at least one check failed
# ============================================================================

set -euo pipefail
cd "$(dirname "$0")/.."

EXIT=0
MODE="${1:-all}"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m%s\033[0m\n' "$*"; }

# Detect and report llvm-mc version
LLVM_MC=$(command -v llvm-mc 2>/dev/null || echo "")
if [ -z "$LLVM_MC" ]; then
    for p in /opt/homebrew/opt/llvm/bin/llvm-mc /usr/local/opt/llvm/bin/llvm-mc; do
        [ -x "$p" ] && LLVM_MC="$p" && break
    done
fi
if [ -n "$LLVM_MC" ]; then
    LLVM_VER=$("$LLVM_MC" --version 2>&1 | grep -oE 'version [0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)
    info "llvm-mc: $LLVM_MC ($LLVM_VER)"
else
    info "llvm-mc: not found (golden-row checks will soft-skip)"
fi

# --- Helper: run check and track exit code ---
run_check() {
    local label="$1"; shift
    info "=== ${label} ==="
    if "$@"; then
        green "  PASS"
    else
        red "  FAIL"
        EXIT=1
    fi
    echo
}

# ============================================================================
# 1. Golden-row llvm-mc external diff
# ============================================================================
if [ "$MODE" = "all" ] || [ "$MODE" = "--golden" ]; then
    run_check "golden-row llvm-mc diff (arm64 + riscv64 + x64)" \
        python3 tools/xisagen/xisagen.py llvm-mc-check \
            arm64=xisa/arch/arm64.isa \
            riscv64=xisa/arch/riscv64.isa \
            x64=xisa/arch/x64.isa
fi

# ============================================================================
# 2. Fixed-bit encoding uniqueness + golden reverse decoding
# ============================================================================
if [ "$MODE" = "all" ] || [ "$MODE" = "--disasm" ]; then
    run_check "fixed-bit encoding uniqueness + golden reverse decoding (arm64 + riscv64)" \
        python3 tools/xisagen/xisagen.py disasm-check \
            arm64=xisa/arch/arm64.isa \
            riscv64=xisa/arch/riscv64.isa
fi

# ============================================================================
# 3. Random-seed external differential (x64)
# ============================================================================
if [ "$MODE" = "all" ] || [ "$MODE" = "--random" ]; then
    SEED="${RANDOM_DIFF_SEED:-42}"
    COUNT="${RANDOM_DIFF_COUNT:-20}"
    run_check "random-seed differential (x64, seed=${SEED}, count=${COUNT})" \
        python3 tools/xisagen/xisagen.py random-diff \
            --seed="${SEED}" --count="${COUNT}" \
            x64=xisa/arch/x64.isa
fi

# ============================================================================
# 4. Generated golden tests (ctest)
# ============================================================================
if [ "$MODE" = "all" ] || [ "$MODE" = "--ctest" ]; then
    if [ -d build ]; then
        PATTERN="test_(arm64|riscv64|x64)_(golden|disasm)|test_dispatch_(emit|meta)|test_arm64_dispatch|test_codegen_dispatch"
        MATCH_COUNT=$(ctest --test-dir build -N -R "${PATTERN}" 2>/dev/null | grep -c 'Test #' || true)
        if [ "${MATCH_COUNT}" -gt 0 ]; then
            run_check "generated golden + disasm ctest targets (${MATCH_COUNT} tests)" \
                ctest --test-dir build --output-on-failure -R "${PATTERN}"
        else
            info "=== generated golden ctest targets ==="
            red "  No matching ctest targets found in build/"
            EXIT=1
        fi
    else
        info "=== generated golden ctest targets ==="
        red "  build/ directory not found — run cmake --build build first"
        EXIT=1
    fi
fi

# ============================================================================
# Summary
# ============================================================================
echo "---"
if [ ${EXIT} -eq 0 ]; then
    green "All codegen differential checks passed."
else
    red "One or more differential checks failed. See output above."
fi
exit ${EXIT}
