#!/bin/bash
# Codegen quarantine check: prevents new hand-written encoding and lowering
# patterns in quarantined files. Run in CI to enforce the xisa migration
# contract.
#
# Quarantined files must NOT gain:
#   - New x64_emit8/x64_emit32/x64_emit64 calls (raw byte assembly)
#   - New a64_buf_emit(..., 0x...) calls (inline ARM64 bitfield)
#   - New 'case XM_' entries (hand-written opcode handling)
#   - New 'void x64_' / 'void a64_' function definitions (new emit helpers)
#   - New 'case XI_' entries in Xi-to-target lowering roots
#
# This script compares the quarantine baseline counts against the current
# source and fails if any count increases.

set -euo pipefail
cd "$(dirname "$0")/.."

MACHINE_QUARANTINE_FILES=(
    src/jit/xm_x64.c
    src/jit/xm_x64.h
    src/jit/xm_arm64.c
    src/jit/xm_arm64.h
    src/jit/xm_arm64_disasm.h
    src/jit/xm_ops.h
    src/jit/xm_codegen_ins.c
    src/jit/xm_codegen_call.c
    src/jit/xm_codegen_mem.c
    src/jit/xm_codegen_x64_ins.c
    src/jit/xm_codegen_x64_call.c
    src/jit/xm_codegen_x64_mem.c
)

XI_LOWERING_QUARANTINE_FILES=(
    src/jit/xi_to_xm.c
    src/aot/xi_cgen.c
)

BASELINE_FILE="scripts/codegen_quarantine_baseline.txt"
FAIL=0

if [ "${1:-}" = "--update-baseline" ]; then
    echo "# Codegen quarantine baseline (auto-generated)" > "$BASELINE_FILE"
    echo "# Format: filename metric_name count" >> "$BASELINE_FILE"
    for f in "${MACHINE_QUARANTINE_FILES[@]}"; do
        if [ ! -f "$f" ]; then continue; fi
        emit_count=$(grep -c 'x64_emit8\|x64_emit32\|x64_emit64\|a64_buf_emit' "$f" 2>/dev/null || true)
        case_count=$(grep -c 'case XM_' "$f" 2>/dev/null || true)
        func_count=$(grep -c '^void x64_\|^void a64_\|^XR_FUNC void x64_\|^XR_FUNC void a64_' "$f" 2>/dev/null || true)
        : "${emit_count:=0}" "${case_count:=0}" "${func_count:=0}"
        echo "$f emit $emit_count" >> "$BASELINE_FILE"
        echo "$f case_xm $case_count" >> "$BASELINE_FILE"
        echo "$f func_def $func_count" >> "$BASELINE_FILE"
    done
    for f in "${XI_LOWERING_QUARANTINE_FILES[@]}"; do
        if [ ! -f "$f" ]; then continue; fi
        case_count=$(grep -Ec 'case[[:space:]]+XI_[A-Z0-9_]+[[:space:]]*:' "$f" 2>/dev/null || true)
        : "${case_count:=0}"
        echo "$f case_xi $case_count" >> "$BASELINE_FILE"
    done
    echo "Baseline updated: $BASELINE_FILE"
    exit 0
fi

if [ ! -f "$BASELINE_FILE" ]; then
    echo "ERROR: baseline file $BASELINE_FILE not found"
    echo "Run: scripts/check_codegen_quarantine.sh --update-baseline"
    exit 1
fi

echo "Checking codegen quarantine..."

while IFS=' ' read -r file metric baseline_count; do
    # Skip comments
    [[ "$file" == \#* ]] && continue
    [ -z "$file" ] && continue

    if [ ! -f "$file" ]; then continue; fi

    case "$metric" in
        emit)
            current=$(grep -c 'x64_emit8\|x64_emit32\|x64_emit64\|a64_buf_emit' "$file" 2>/dev/null || true); : "${current:=0}"
            ;;
        case_xm)
            current=$(grep -c 'case XM_' "$file" 2>/dev/null || true); : "${current:=0}"
            ;;
        case_xi)
            current=$(grep -Ec 'case[[:space:]]+XI_[A-Z0-9_]+[[:space:]]*:' "$file" 2>/dev/null || true); : "${current:=0}"
            ;;
        func_def)
            current=$(grep -c '^void x64_\|^void a64_\|^XR_FUNC void x64_\|^XR_FUNC void a64_' "$file" 2>/dev/null || true); : "${current:=0}"
            ;;
        *)
            continue
            ;;
    esac

    if [ "$current" -gt "$baseline_count" ]; then
        echo "FAIL: $file: $metric increased ($baseline_count -> $current)"
        FAIL=1
    fi
done < "$BASELINE_FILE"

if [ $FAIL -eq 0 ]; then
    echo "OK: no quarantine violations"
else
    echo ""
    echo "Quarantine violation detected!"
    echo "Quarantined files must not gain new hand-written codegen patterns."
    echo "Use xisa-generated code instead. Do not grow the quarantine baseline."
    exit 1
fi
