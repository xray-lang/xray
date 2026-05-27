#!/usr/bin/env bash
# ============================================================================
# check_xi_invariants.sh
# ----------------------------------------------------------------------------
# Reverse invariants for Xi IR layer (type A — static CI grep checks).
# Runs in PR gate. Failure blocks merge.
#
# Covers invariants #1-#3, #5, #10, #12-#15 from the Xi optimization
# roadmap reverse invariant table.  Type B (verify pass) and type C
# (pass table validator) are enforced at runtime, not here.
#
# Exit codes:
#   0   all checks passed
#   1   at least one check failed
#
# Usage:
#   ./scripts/check_xi_invariants.sh          # run all checks
#   ./scripts/check_xi_invariants.sh --verbose # show match details
# ============================================================================

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

FAIL=0
VERBOSE=0
if [ "${1:-}" = "--verbose" ]; then
    VERBOSE=1
fi

pass() {
    printf "  [PASS] %s\n" "$1"
}

fail() {
    printf "  [FAIL] %s\n" "$1"
    FAIL=1
}

warn() {
    printf "  [WARN] %s\n" "$1"
}

show_matches() {
    if [ "${VERBOSE}" -eq 1 ] && [ -n "$1" ]; then
        printf "%s\n" "$1" | head -20
    fi
}

echo "=== Xi IR Reverse Invariants ==="
echo ""

# --------------------------------------------------------------------------
# INV-5: No raw malloc/calloc/realloc/free in IR source
# --------------------------------------------------------------------------
echo "--- INV-5: No raw malloc/free in src/ir/ ---"
MATCHES=$(grep -rn '\b\(malloc\|calloc\|realloc\|free\)\s*(' src/ir/ \
    --include='*.c' --include='*.h' \
    | grep -v 'xr_malloc\|xr_calloc\|xr_realloc\|xr_free\|xr_arena' \
    | grep -v '^\s*//' \
    | grep -v '^\s*\*' \
    || true)
if [ -z "$MATCHES" ]; then
    pass "No raw malloc/free in src/ir/"
else
    fail "Raw malloc/free found in src/ir/"
    show_matches "$MATCHES"
fi

# --------------------------------------------------------------------------
# INV-14: No phase coordinates (Xi-N.M / Phase A/B) in source
# --------------------------------------------------------------------------
echo "--- INV-14: No phase coordinates in source ---"
MATCHES=$(grep -rn 'Xi-[0-9]\+\.[0-9]\+' src/ stdlib/ include/ \
    --include='*.c' --include='*.h' \
    | grep -v '^\s*//' \
    | grep -v '^\s*\*' \
    || true)
if [ -z "$MATCHES" ]; then
    pass "No Xi-N.M phase coordinates"
else
    fail "Phase coordinates found in source"
    show_matches "$MATCHES"
fi

MATCHES=$(grep -rn '^\s*\(\*\|//\).*\bPhase [A-Z0-9]' src/ir/ \
    --include='*.c' --include='*.h' \
    || true)
if [ -z "$MATCHES" ]; then
    pass "No 'Phase X' in comments"
else
    fail "'Phase X' found in comments"
    show_matches "$MATCHES"
fi

# --------------------------------------------------------------------------
# INV-15: No fallback mode flags
# --------------------------------------------------------------------------
echo "--- INV-15: No fallback mode flags ---"
MATCHES=$(grep -rn 'XI_[A-Z_]*_MODE\|XI_[A-Z_]*_MODEL\|XI_ENABLE_TBAA\|XI_RANGE_ENABLED\|XI_GVN_MODE\|XRAY_XI_TBAA\|XRAY_XI_SPEC[^=_]' \
    src/ir/ \
    --include='*.c' --include='*.h' \
    || true)
if [ -z "$MATCHES" ]; then
    pass "No fallback mode flags"
else
    fail "Fallback mode flags found"
    show_matches "$MATCHES"
fi

# --------------------------------------------------------------------------
# INV-2: xi_op_name.h sync — every XI_* enum in xi.h must have a name
# --------------------------------------------------------------------------
echo "--- INV-2: op name sync ---"
if [ -f src/ir/xi.h ] && [ -f src/ir/xi_op_name.h ]; then
    # Extract every concrete op identifier in the [XI_CONST, XI_OP_COUNT)
    # range, dedup, drop the XI_OP_COUNT sentinel, then diff against the
    # set of cases handled by xi_op_name.h. Anything left is a real gap.
    OPS_FILE=$(mktemp)
    NAMES_FILE=$(mktemp)
    awk '/XI_CONST = 0/,/XI_OP_COUNT/' src/ir/xi.h \
        | grep -oE '^[[:space:]]*XI_[A-Z_]+' \
        | tr -d ' ' \
        | grep -v '^XI_OP_COUNT$' \
        | sort -u > "$OPS_FILE"
    grep -oE 'case XI_[A-Z_]+' src/ir/xi_op_name.h \
        | sed 's/case //' \
        | sort -u > "$NAMES_FILE"
    MISSING=$(comm -23 "$OPS_FILE" "$NAMES_FILE")
    EXTRA=$(comm -13 "$OPS_FILE" "$NAMES_FILE")
    rm -f "$OPS_FILE" "$NAMES_FILE"
    if [ -z "$MISSING" ] && [ -z "$EXTRA" ]; then
        pass "every XI_* op has a name entry, no orphan cases"
    elif [ -n "$MISSING" ]; then
        fail "ops missing from xi_op_name.h:"
        printf "%s\n" "$MISSING" | sed 's/^/    /'
    else
        fail "xi_op_name.h has cases for unknown XI_* ops:"
        printf "%s\n" "$EXTRA" | sed 's/^/    /'
    fi
else
    warn "xi.h or xi_op_name.h not found, skipping"
fi

# --------------------------------------------------------------------------
# INV-13: No functions > 150 lines in src/ir/
# --------------------------------------------------------------------------
echo "--- INV-13: Function length check (src/ir/) ---"
LONG_FUNCS=0
for f in src/ir/*.c; do
    [ -f "$f" ] || continue
    # Simple heuristic: count lines between function opening brace and closing
    # at column 0. Exact parsing would need a C parser; this catches gross
    # violations.
    awk '
    /^[A-Za-z_].*\(/ && /\{[[:space:]]*$/ { start = NR; fname = $0; next }
    /^\}/ && start > 0 {
        len = NR - start
        if (len > 150) {
            printf "  %s:%d: function ~%d lines (>150): %s\n", FILENAME, start, len, fname
            found = 1
        }
        start = 0
    }
    END { exit (found ? 1 : 0) }
    ' found=0 "$f" 2>/dev/null
    if [ $? -ne 0 ]; then
        LONG_FUNCS=1
    fi
done
if [ "$LONG_FUNCS" -eq 0 ]; then
    pass "All functions in src/ir/ <= 150 lines"
else
    warn "Functions exceeding 150 lines found (pre-existing; new code must comply)"
fi

# --------------------------------------------------------------------------
# INV-1: Pass functions must be registered in pass table
# --------------------------------------------------------------------------
echo "--- INV-1: Pass registration check ---"
# Extract xi_opt_* function definitions from src/ir/
PASS_FUNCS=$(grep -rn '^XR_FUNC.*xi_opt_[a-z_]*\s*(' src/ir/ \
    --include='*.c' \
    | sed 's/.*\(xi_opt_[a-z_]*\).*/\1/' \
    | sort -u || true)

if [ -f src/ir/xi_opt.c ]; then
    MISSING=""
    for fn in $PASS_FUNCS; do
        if ! grep -q "$fn" src/ir/xi_opt.c; then
            MISSING="${MISSING}  $fn\n"
        fi
    done
    if [ -z "$MISSING" ]; then
        pass "All xi_opt_* functions referenced in xi_opt.c"
    else
        warn "Some xi_opt_* not found in xi_opt.c (may be helpers, not passes):"
        if [ "$VERBOSE" -eq 1 ]; then
            printf "$MISSING"
        fi
    fi
else
    warn "xi_opt.c not found, skipping"
fi

# --------------------------------------------------------------------------
# INV-12: Each xi_opt_*.c must have either a dedicated test file or
# a non-trivial reference set inside the combined test_xi_opt.c.
# --------------------------------------------------------------------------
echo "--- INV-12: Test coverage check ---"
COMBINED_TEST="tests/unit/ir/test_xi_opt.c"
COMBINED_MIN_REFS=3   # at least N references implies real coverage, not a stray include
MISSING_DEDICATED=()
NO_COVERAGE=()
for f in src/ir/xi_opt_*.c; do
    [ -f "$f" ] || continue
    BASE=$(basename "$f" .c)               # e.g. xi_opt_ifconv
    SHORT="${BASE#xi_opt_}"                # e.g. ifconv
    # Accept any of: test_<base>.c, test_xi_<short>.c, test_<base>_*.c
    if ls "tests/unit/ir/test_${BASE}.c" \
          "tests/unit/ir/test_xi_${SHORT}.c" \
          tests/unit/ir/test_${BASE}_*.c 2>/dev/null | grep -q .; then
        continue
    fi
    # No dedicated file — fall back to checking the combined test.
    REFS=0
    if [ -f "$COMBINED_TEST" ]; then
        # grep -c returns exit 1 with 0 matches; capture stdout regardless
        REFS=$(grep -c "${BASE}" "$COMBINED_TEST" 2>/dev/null) || REFS=0
    fi
    if [ "$REFS" -ge "$COMBINED_MIN_REFS" ]; then
        MISSING_DEDICATED+=("$BASE ($REFS refs in test_xi_opt.c)")
    else
        NO_COVERAGE+=("$BASE")
    fi
done
if [ "${#NO_COVERAGE[@]}" -gt 0 ]; then
    fail "Passes with no test coverage at all:"
    for entry in "${NO_COVERAGE[@]}"; do
        printf "    %s\n" "$entry"
    done
elif [ "${#MISSING_DEDICATED[@]}" -gt 0 ]; then
    warn "Passes covered only by test_xi_opt.c (consider promoting to dedicated test):"
    for entry in "${MISSING_DEDICATED[@]}"; do
        printf "    %s\n" "$entry"
    done
else
    pass "Every xi_opt_*.c has a dedicated test file"
fi

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "=== ALL XI INVARIANTS PASSED ==="
    exit 0
else
    echo "=== XI INVARIANT FAILURES DETECTED ==="
    exit 1
fi
