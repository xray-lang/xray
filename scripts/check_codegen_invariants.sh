#!/usr/bin/env bash
# ============================================================================
# check_codegen_invariants.sh
# ----------------------------------------------------------------------------
# Reverse invariants for the codegen layer.
# Runs in PR gate. Failure blocks merge.
#
#   1. Silent fallback (`default: break;`) inside codegen / GC / coro
#        is rejected. A baseline list (tests/baseline_silent_fallback.txt)
#        records the current 32 grandfathered occurrences; *any new*
#        occurrence outside that list fails.
#
#   2. Legacy / compat / deprecated wrappers are rejected outright.
#        Match list:  old_emit  legacy_helper  XR_COMPAT  deprecated_call
#
#   3. Generated codegen artifacts must match the xisa truth sources.
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
        echo "  Fix the new violation (preferred), or, if it is truly"
        echo "  unreachable, replace with XR_UNREACHABLE(\"...\")."
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
# 3. generated artifact freshness
# ----------------------------------------------------------------------------
section "3. generated artifact freshness"

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

freshness_fail=0
for rel in \
    src/jit/xm_ops_gen.h \
    src/jit/xm_helpers_gen.h \
    src/jit/xm_x64_gen.c \
    src/jit/xm_arm64_gen.c \
    src/jit/xm_riscv64_gen.c \
    tests/unit/jit/test_x64_golden_gen.c \
    tests/unit/jit/test_arm64_golden_gen.c \
    tests/unit/jit/test_riscv64_golden_gen.c
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
