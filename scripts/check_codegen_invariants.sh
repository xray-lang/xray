#!/usr/bin/env bash
# ============================================================================
# check_codegen_invariants.sh
# ----------------------------------------------------------------------------
# Reverse invariants seed (subset of 076 §7, locked in by 082 task E).
# Runs in PR gate. Failure blocks merge.
#
#   E.1  Silent fallback (`default: break;`) inside codegen / GC / coro
#        is rejected. A baseline list (tests/baseline_silent_fallback.txt)
#        records the current 32 grandfathered occurrences; *any new*
#        occurrence outside that list fails. The baseline shrinks during
#        076 main line S3/S5/S6.
#
#   E.2  Legacy / compat / deprecated wrappers are rejected outright.
#        Match list:  old_emit  legacy_helper  XR_COMPAT  deprecated_call
#
#   E.3  Generated codegen artifacts must never be tracked by git.
#        Match list:  build/generated/
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
        echo "# Removal plan: 076 main line S3/S5/S6 progressively reduces to 0."
        echo "# Any *new* silent fallback outside this list is rejected by CI."
        printf '%s\n' "${current}"
    } > "${BASELINE_FILE}"
    green "Baseline regenerated with ${count} entries."
    exit 0
fi

# ----------------------------------------------------------------------------
# E.1  silent fallback diff vs baseline
# ----------------------------------------------------------------------------
section "E.1  silent fallback (default: break;)"

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
# E.2  legacy wrapper / compat shim
# ----------------------------------------------------------------------------
section "E.2  legacy wrapper / compat shim"

# Word-boundary match; excludes false positives like xm_fold_emit (substring).
pattern='\b(old_emit|legacy_helper|XR_COMPAT|deprecated_call)\b'
legacy_hits=$(grep -rEn --include='*.c' --include='*.h' "${pattern}" ${LEGACY_DIRS} 2>/dev/null || true)

if [ -n "${legacy_hits}" ]; then
    red "FAIL: legacy wrapper / compat shim found:"
    printf '%s\n' "${legacy_hits}" | sed 's/^/  /'
    echo
    echo "  Inline the call or delete the wrapper. Compat shims are"
    echo "  rejected by 076 §7 reverse-invariant #7."
    EXIT=1
else
    green "OK: no legacy wrapper / compat shim."
fi

# ----------------------------------------------------------------------------
# E.3  generated codegen artifacts not in git
# ----------------------------------------------------------------------------
section "E.3  generated codegen artifacts not in git"

# git ls-files lists only tracked files.
generated_tracked=$(git ls-files 2>/dev/null | grep -E '^build/generated/' || true)

if [ -n "${generated_tracked}" ]; then
    red "FAIL: generated artifacts are tracked by git:"
    printf '%s\n' "${generated_tracked}" | sed 's/^/  /'
    echo
    echo "  These files must be regenerated by the build, not committed."
    echo "  Remove with: git rm --cached <path>"
    EXIT=1
else
    green "OK: build/generated/ is not tracked."
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
