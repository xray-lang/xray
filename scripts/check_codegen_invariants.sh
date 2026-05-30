#!/usr/bin/env bash
# ============================================================================
# check_codegen_invariants.sh
# ----------------------------------------------------------------------------
# Reverse invariants for the codegen layer.
# Runs in PR gate. Failure blocks merge.
#
#   1. Silent fallback (`default: break;`) inside codegen / GC / coro
#        is rejected. The baseline list
#        (tests/baseline_silent_fallback.txt) records only intentional
#        grandfathered occurrences and is expected to remain empty.
#
#   2. Legacy / compat / deprecated wrappers are rejected outright.
#        Match list:  old_emit  legacy_helper  XR_COMPAT  deprecated_call
#
#   3. XmOp ownership and runtime dual-emit guardrails are enforced.
#
#   4. Generated codegen artifacts must match the xisa truth sources.
#
# Exit codes:
#   0   all checks passed
#   1   at least one check failed
#
# Modes:
#   (no arg)             Run all checks. Default in CI.
#   --update-baseline    Regenerate tests/baseline_silent_fallback.txt
#                        from current source. Use only when intentionally
#                        removing a legitimate violation. Never use to
#                        whitelist new violations.
#
# Cross-platform: pure POSIX + grep -E. Tested on Linux, macOS, Git Bash.
# ============================================================================

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

BASELINE_FILE="tests/baseline_silent_fallback.txt"
SCAN_DIRS="src/jit src/runtime/gc src/coro"
LEGACY_DIRS="src"
EXIT=0

# ----------------------------------------------------------------------------
# helpers
# ----------------------------------------------------------------------------
red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
section() { printf '\n=== %s ===\n' "$*"; }

# Scan E.1 silent fallback. Recognises two physical forms:
#   1) single line:  default: break;
#   2) two lines:    default:
#                        break;
# Emits  <relpath>:<line>  per occurrence (line = the "default:" line).
scan_silent_fallback() {
    find ${SCAN_DIRS} -type f -name '*.c' 2>/dev/null | sort | while read -r f; do
        awk -v file="${f}" '
            { lines[NR]=$0 }
            END {
                for (i = 1; i <= NR; i++) {
                    if (lines[i] ~ /^[[:space:]]*default:[[:space:]]*break;[[:space:]]*$/) {
                        printf("%s:%d\n", file, i)
                    } else if (lines[i] ~ /^[[:space:]]*default:[[:space:]]*$/ &&
                               i + 1 <= NR &&
                               lines[i+1] ~ /^[[:space:]]*break;[[:space:]]*$/) {
                        printf("%s:%d\n", file, i)
                    }
                }
            }
        ' "${f}"
    done
}

# ----------------------------------------------------------------------------
# --update-baseline mode
# ----------------------------------------------------------------------------
if [ "${1:-}" = "--update-baseline" ]; then
    section "Regenerating ${BASELINE_FILE}"
    current=$(scan_silent_fallback)
    count=$(printf '%s\n' "${current}" | grep -c . || true)
    {
        echo "# Baseline list of silent fallback (\"default: break;\") violations."
        echo "# Format: <relative-path>:<line>"
        echo "# Generated $(date +%Y-%m-%d) by scripts/check_codegen_invariants.sh --update-baseline"
        echo "# Total: ${count}"
        echo "# Owner: xingleixu"
        echo "# Removal plan: progressively reduce this baseline to 0."
        echo "# Any *new* silent fallback outside this list is rejected by CI."
        printf '%s\n' "${current}"
    } > "${BASELINE_FILE}"
    green "Baseline regenerated with ${count} entries."
    exit 0
fi

# ----------------------------------------------------------------------------
# 1. silent fallback diff vs baseline
# ----------------------------------------------------------------------------
section "1. silent fallback (default: break;)"

if [ ! -f "${BASELINE_FILE}" ]; then
    red "ERR: ${BASELINE_FILE} missing. Run --update-baseline first."
    EXIT=1
else
    current=$(scan_silent_fallback)
    # Strip CR for cross-platform baseline files (Windows-edited baselines
    # carry CRLF endings; comm requires byte-identical lines).
    baseline=$(grep -v '^#' "${BASELINE_FILE}" | tr -d '\r' | grep -v '^[[:space:]]*$' || true)

    # New violations = lines in current but not in baseline.
    new_violations=$(comm -23 <(printf '%s\n' "${current}" | sort -u) \
                              <(printf '%s\n' "${baseline}" | sort -u))

    # Healed entries = baseline lines no longer present in current.
    healed=$(comm -13 <(printf '%s\n' "${current}" | sort -u) \
                     <(printf '%s\n' "${baseline}" | sort -u))

    if [ -n "${new_violations}" ]; then
        red "FAIL: new silent fallback(s) introduced outside baseline:"
        printf '%s\n' "${new_violations}" | sed 's/^/  /'
        echo
        echo "  Fix the new violation. If the branch is truly invalid,"
        echo "  use an always-on XR_CHECK(false, \"...\") failure."
        echo "  Do NOT regenerate the baseline to whitelist new entries."
        EXIT=1
    else
        green "OK: no new silent fallback."
    fi

    if [ -n "${healed}" ]; then
        yellow "Note: baseline contains entries no longer present in source:"
        printf '%s\n' "${healed}" | sed 's/^/  /'
        echo
        echo "  Run --update-baseline to shrink the baseline."
    fi
fi

# ----------------------------------------------------------------------------
# 2. legacy wrapper / compat shim
# ----------------------------------------------------------------------------
section "2. legacy wrapper / compat shim"

# Word-boundary match; excludes false positives like xm_fold_emit (substring).
pattern='\b(old_emit|legacy_helper|XR_COMPAT|deprecated_call)\b'
legacy_hits=$(grep -rEn --include='*.c' --include='*.h' "${pattern}" ${LEGACY_DIRS} 2>/dev/null || true)

if [ -n "${legacy_hits}" ]; then
    red "FAIL: legacy wrapper / compat shim found:"
    printf '%s\n' "${legacy_hits}" | sed 's/^/  /'
    echo
    echo "  Inline the call or delete the wrapper. Compat shims are"
    echo "  rejected by the codegen reverse invariants."
    EXIT=1
else
    green "OK: no legacy wrapper / compat shim."
fi

# ----------------------------------------------------------------------------
# 3. xisa ownership guardrails
# ----------------------------------------------------------------------------
section "3. xisa ownership guardrails"

manual_xmop_enum=$(grep -rEn --include='*.c' --include='*.h' \
    '(^|[^[:alnum:]_])(enum[[:space:]]+XmOp|}[[:space:]]*XmOp;)' src/jit 2>/dev/null |
    grep -v '^src/jit/xm_ops_gen\.h:' || true)

if [ -n "${manual_xmop_enum}" ]; then
    red "FAIL: hand-written XmOp enum found outside generated metadata:"
    printf '%s\n' "${manual_xmop_enum}" | sed 's/^/  /'
    echo
    echo "  XmOp enum ownership belongs to xisa/xm/ops.def and xm_ops_gen.h."
    EXIT=1
else
    green "OK: XmOp enum ownership is generated-only."
fi

dual_emit_hits=$(grep -rEn --include='*.c' --include='*.h' \
    '\bXR_ENABLE_XISA_DUAL_EMIT\b' src include stdlib 2>/dev/null || true)

if [ -n "${dual_emit_hits}" ]; then
    red "FAIL: runtime dual-emit flag entered source:"
    printf '%s\n' "${dual_emit_hits}" | sed 's/^/  /'
    echo
    echo "  Dual-emit is allowed only as a test oracle, never in runtime source."
    EXIT=1
else
    green "OK: no runtime dual-emit flag in source."
fi

# ----------------------------------------------------------------------------
# 4. generated artifact freshness
# ----------------------------------------------------------------------------
section "4. generated artifact freshness"

TMPDIR_PATH=$(mktemp -d "${TMPDIR:-/tmp}/xray_codegen_gen.XXXXXX")
trap 'rm -rf "${TMPDIR_PATH}"' EXIT
mkdir -p "${TMPDIR_PATH}/src/jit" "${TMPDIR_PATH}/tests/unit/jit"

if ! python3 tools/xisagen/xisagen.py ops xisa/xm/ops.def \
        "${TMPDIR_PATH}/src/jit/xm_ops_gen.h" >/dev/null 2>&1; then
    red "FAIL: xisagen ops failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py helpers xisa/xm/helpers.def \
        "${TMPDIR_PATH}/src/jit/xm_helpers_gen.h" >/dev/null 2>&1; then
    red "FAIL: xisagen helpers failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py runtime-stubs xisa/xm/runtime_stubs.def \
        "${TMPDIR_PATH}/src/jit/xm_runtime_stubs_gen.h" >/dev/null 2>&1; then
    red "FAIL: xisagen runtime-stubs failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py x64impl xisa/arch/x64.isa \
        "${TMPDIR_PATH}/src/jit" >/dev/null 2>&1; then
    red "FAIL: xisagen x64 implementation failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py arm64impl xisa/arch/arm64.isa \
        "${TMPDIR_PATH}/src/jit" >/dev/null 2>&1; then
    red "FAIL: xisagen arm64 implementation failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py riscv64impl xisa/arch/riscv64.isa \
        "${TMPDIR_PATH}/src/jit" >/dev/null 2>&1; then
    red "FAIL: xisagen riscv64 implementation failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py golden xisa/arch/x64.isa \
        "${TMPDIR_PATH}/tests/unit/jit/test_x64_golden_gen.c" >/dev/null 2>&1; then
    red "FAIL: xisagen x64 golden failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py golden xisa/arch/arm64.isa \
        "${TMPDIR_PATH}/tests/unit/jit/test_arm64_golden_gen.c" >/dev/null 2>&1; then
    red "FAIL: xisagen arm64 golden failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py golden xisa/arch/riscv64.isa \
        "${TMPDIR_PATH}/tests/unit/jit/test_riscv64_golden_gen.c" >/dev/null 2>&1; then
    red "FAIL: xisagen riscv64 golden failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py dispatch-coverage xisa/xm/isel.def \
        "${TMPDIR_PATH}/src/jit/xm_dispatch_coverage_gen.h" >/dev/null 2>&1; then
    red "FAIL: xisagen dispatch-coverage failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py dispatch-meta xisa/xm/isel.def xisa/xm/ops.def \
        "${TMPDIR_PATH}/src/jit/xm_dispatch_meta_gen.h" >/dev/null 2>&1; then
    red "FAIL: xisagen dispatch-meta failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py dispatch-emit xisa/xm/isel.def \
        "${TMPDIR_PATH}/src/jit/xm_dispatch_emit_gen.h" >/dev/null 2>&1; then
    red "FAIL: xisagen dispatch-emit failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py patch-ranges \
        "${TMPDIR_PATH}/src/jit/xm_patch_ranges_gen.h" \
        x64=xisa/arch/x64.isa \
        arm64=xisa/arch/arm64.isa \
        riscv64=xisa/arch/riscv64.isa >/dev/null 2>&1; then
    red "FAIL: xisagen patch-ranges failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py disasm xisa/arch/arm64.isa \
        "${TMPDIR_PATH}/src/jit" >/dev/null 2>&1; then
    red "FAIL: xisagen arm64 disasm failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py disasm xisa/arch/riscv64.isa \
        "${TMPDIR_PATH}/src/jit" >/dev/null 2>&1; then
    red "FAIL: xisagen riscv64 disasm failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py disasm-test xisa/arch/arm64.isa \
        "${TMPDIR_PATH}/tests/unit/jit/test_arm64_disasm_gen.c" >/dev/null 2>&1; then
    red "FAIL: xisagen arm64 disasm-test failed."
    EXIT=1
fi
if ! python3 tools/xisagen/xisagen.py disasm-test xisa/arch/riscv64.isa \
        "${TMPDIR_PATH}/tests/unit/jit/test_riscv64_disasm_gen.c" >/dev/null 2>&1; then
    red "FAIL: xisagen riscv64 disasm-test failed."
    EXIT=1
fi

freshness_fail=0
for rel in \
    src/jit/xm_ops_gen.h \
    src/jit/xm_helpers_gen.h \
    src/jit/xm_runtime_stubs_gen.h \
    src/jit/xm_x64_gen.c \
    src/jit/xm_arm64_gen.c \
    src/jit/xm_riscv64_gen.c \
    src/jit/xm_arm64_disasm_gen.c \
    src/jit/xm_arm64_disasm_gen.h \
    src/jit/xm_riscv64_disasm_gen.c \
    src/jit/xm_riscv64_disasm_gen.h \
    src/jit/xm_dispatch_coverage_gen.h \
    src/jit/xm_dispatch_meta_gen.h \
    src/jit/xm_dispatch_emit_gen.h \
    src/jit/xm_patch_ranges_gen.h \
    tests/unit/jit/test_x64_golden_gen.c \
    tests/unit/jit/test_arm64_golden_gen.c \
    tests/unit/jit/test_riscv64_golden_gen.c \
    tests/unit/jit/test_arm64_disasm_gen.c \
    tests/unit/jit/test_riscv64_disasm_gen.c
do
    if ! cmp -s "${TMPDIR_PATH}/${rel}" "${rel}"; then
        red "FAIL: generated artifact is stale: ${rel}"
        freshness_fail=1
    fi
done

if [ "${freshness_fail}" -eq 0 ]; then
    green "OK: generated artifacts match xisa sources."
else
    echo
    echo "  Regenerate tracked artifacts with the build or tools/xisagen/xisagen.py."
    EXIT=1
fi

section "5. helper CALL_C metadata path"

manual_callc=$(awk '
    /xm_emit[[:space:]]*\(/ {
        in_emit = 1
        start = NR
        text = $0
        if ($0 ~ /;/) {
            if (text ~ /XM_CALL_C/ && text !~ /helper_call_rep/)
                printf("src/jit/xi_to_xm.c:%d:%s\n", start, text)
            in_emit = 0
            text = ""
        }
        next
    }
    in_emit {
        text = text " " $0
        if ($0 ~ /;/) {
            if (text ~ /XM_CALL_C/ && text !~ /helper_call_rep/)
                printf("src/jit/xi_to_xm.c:%d:%s\n", start, text)
            in_emit = 0
            text = ""
        }
    }
' src/jit/xi_to_xm.c)

if [ -n "${manual_callc}" ]; then
    red "FAIL: xi_to_xm.c emits XM_CALL_C outside emit_helper_call:"
    printf '%s\n' "${manual_callc}" | sed 's/^/  /'
    echo
    echo "  Route JIT runtime helper calls through emit_helper_call so"
    echo "  ret_rep, ctype, flags, pointer trust, and post-call protocol"
    echo "  come from xm_helper_table."
    EXIT=1
else
    green "OK: xi_to_xm.c uses helper metadata wrapper for XM_CALL_C."
fi

registered_helper_re=$(awk '
    /^[[:space:]]*XM_HELPER[[:space:]]*\(/ {
        name = $0
        sub(/^[[:space:]]*XM_HELPER[[:space:]]*\(/, "", name)
        sub(/,.*/, "", name)
        gsub(/[[:space:]]/, "", name)
        if (name != "") {
            if (out != "")
                out = out "|"
            out = out name
        }
    }
    END { print out }
' xisa/xm/helpers.def)

if [ -z "${registered_helper_re}" ]; then
    red "FAIL: helper registry names could not be read from xisa/xm/helpers.def"
    EXIT=1
else
    direct_registered_helpers=$(find src/jit -type f -name 'xm_codegen*.c' 2>/dev/null |
        sort |
        while read -r f; do
            awk -v file="${f}" -v helper_re="${registered_helper_re}" '
                $0 ~ ("\\(uintptr_t\\)[[:space:]]*&?xr_jit_(" helper_re ")([^[:alnum:]_]|$)") ||
                $0 ~ ("=[[:space:]]*\\(void \\*\\)[[:space:]]*xr_jit_(" helper_re ")([^[:alnum:]_]|$)") ||
                $0 ~ ("&xr_jit_(" helper_re ")([^[:alnum:]_]|$)") {
                    printf("%s:%d:%s\n", file, NR, $0)
                }
            ' "${f}"
        done)

    if [ -n "${direct_registered_helpers}" ]; then
        red "FAIL: registered helper function pointer bypasses xm_helper_table:"
        printf '%s\n' "${direct_registered_helpers}" | sed 's/^/  /'
        echo
        echo "  Load registered helper pointers via xm_helper_func(XM_HELPER_*)"
        echo "  instead of direct xr_jit_* addresses."
        EXIT=1
    else
        green "OK: registered helper pointers come from xm_helper_table."
    fi
fi

# ----------------------------------------------------------------------------
# 6. backend dispatch coverage matches isel.def
# ----------------------------------------------------------------------------
section "6. backend dispatch coverage"

if python3 scripts/check_codegen_dispatch.py >/tmp/xray_dispatch_check.log 2>&1; then
    green "OK: per-backend dispatch tables match isel.def manifest."
    sed -n 's/^check_codegen_dispatch: \(arm64\|x64\|riscv64\):/  /p' \
        /tmp/xray_dispatch_check.log || true
else
    red "FAIL: backend dispatch coverage drifted from isel.def:"
    cat /tmp/xray_dispatch_check.log | sed 's/^/  /'
    EXIT=1
fi

# ----------------------------------------------------------------------------
# 7. xr_jit_* helper address closure
# ----------------------------------------------------------------------------
section "7. helper address closure"

if python3 scripts/check_codegen_helpers.py >/tmp/xray_helpers_check.log 2>&1; then
    green "OK: every direct xr_jit_* reference is registered or declared as a runtime stub."
    sed -n 's/^check_codegen_helpers:   /  /p' /tmp/xray_helpers_check.log || true
else
    red "FAIL: unclassified xr_jit_* direct address in src/jit/*.c:"
    cat /tmp/xray_helpers_check.log | sed 's/^/  /'
    EXIT=1
fi

# ----------------------------------------------------------------------------
# 8. fixed-bit encoding uniqueness (arm64 + riscv64)
# ----------------------------------------------------------------------------
section "8. fixed-bit encoding uniqueness"

if python3 tools/xisagen/xisagen.py disasm-check \
        arm64=xisa/arch/arm64.isa \
        riscv64=xisa/arch/riscv64.isa \
        >/tmp/xray_disasm_check.log 2>&1; then
    green "OK: no decode-ambiguous mcinsn pairs in arm64 / riscv64."
    sed -n 's/^xisagen disasm-check: /  /p' /tmp/xray_disasm_check.log || true
else
    red "FAIL: new mcinsn pair is decode-ambiguous (overlapping fixed bits):"
    cat /tmp/xray_disasm_check.log | sed 's/^/  /'
    echo
    echo "  Either correct the .isa encoding, or — if this is an intentional"
    echo "  assembler-pseudo overlap — add the pair to _EXPECTED_ENCODING_OVERLAPS"
    echo "  in tools/xisagen/xisagen.py with a rationale."
    EXIT=1
fi

# ----------------------------------------------------------------------------
# 9. external assembler diff (llvm-mc) — soft-skipped when llvm-mc absent
# ----------------------------------------------------------------------------
section "9. external assembler diff (llvm-mc)"

if python3 tools/xisagen/xisagen.py llvm-mc-check \
        arm64=xisa/arch/arm64.isa \
        riscv64=xisa/arch/riscv64.isa \
        x64=xisa/arch/x64.isa \
        >/tmp/xray_llvm_mc.log 2>&1; then
    if grep -q 'llvm-mc not found' /tmp/xray_llvm_mc.log; then
        green "OK: llvm-mc unavailable on this host (soft-skip)."
    else
        green "OK: every :golden-asm row matches llvm-mc (or is encoding-pinned)."
        sed -n 's/^xisagen: /  /p' /tmp/xray_llvm_mc.log || true
        sed -n 's/^xisagen llvm-mc-check: /  /p' /tmp/xray_llvm_mc.log || true
    fi
else
    red "FAIL: external assembler diff disagrees with .isa:"
    cat /tmp/xray_llvm_mc.log | sed 's/^/  /'
    echo
    echo "  Either correct the .isa encoding/golden-bytes, or — if llvm-mc"
    echo "  assembled a semantically-equivalent shorter form — add the mcinsn"
    echo "  to _LLVM_MC_SKIP_MCINSNS in tools/xisagen/xisagen.py with rationale."
    EXIT=1
fi

# ----------------------------------------------------------------------------
# 8. S0 pointer boundary audit
# ----------------------------------------------------------------------------
section "S0 pointer boundary audit"
if bash scripts/check_pointer_boundary.sh; then
    green "Pointer boundary checks passed."
else
    red "Pointer boundary audit failed."
    EXIT=1
fi

# ----------------------------------------------------------------------------
# summary
# ----------------------------------------------------------------------------
section "summary"
if [ ${EXIT} -eq 0 ]; then
    green "All reverse-invariant seed checks passed."
else
    red "One or more checks failed. See output above."
fi
exit ${EXIT}
