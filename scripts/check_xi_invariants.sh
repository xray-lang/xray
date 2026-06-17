#!/usr/bin/env bash
# ============================================================================
# check_xi_invariants.sh
# ----------------------------------------------------------------------------
# Reverse invariants for Xi IR layer (type A — static CI grep checks).
# Runs in PR gate. Failure blocks merge.
#
# Covers invariants #1-#3, #5, #10, #12-#15 plus source-level
# fail-fast guards.  Type B (verify pass) and type C (pass table
# validator) are enforced at runtime, not here.
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
# INV-X: No silent default branches in IR source
# --------------------------------------------------------------------------
echo "--- INV-X: No silent default branches in src/ir/ ---"
MATCHES=$(find src/ir -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null | sort | while read -r f; do
    awk -v file="$f" '
        { lines[NR] = $0 }
        END {
            for (i = 1; i <= NR; i++) {
                if (lines[i] ~ /^[[:space:]]*default:[[:space:]]*break;[[:space:]]*$/) {
                    printf("%s:%d:%s\n", file, i, lines[i])
                } else if (lines[i] ~ /^[[:space:]]*default:[[:space:]]*$/ &&
                           i + 1 <= NR &&
                           lines[i + 1] ~ /^[[:space:]]*break;[[:space:]]*$/) {
                    printf("%s:%d:%s\n", file, i, lines[i])
                }
            }
        }
    ' "$f"
done || true)
if [ -z "$MATCHES" ]; then
    pass "No default: break silent fallbacks in src/ir/"
else
    fail "Silent default branches found in src/ir/"
    show_matches "$MATCHES"
fi

# --------------------------------------------------------------------------
# INV-2: generated op name sync — every XI_* enum in xi.h must have a name
# --------------------------------------------------------------------------
echo "--- INV-2: op name sync ---"
if [ -f src/ir/xi.h ] && [ -f src/ir/xi_ops_gen.h ]; then
    # Extract every concrete op identifier in the [XI_CONST, XI_OP_COUNT)
    # range, dedup, drop the XI_OP_COUNT sentinel, then diff against the
    # set of cases handled by generated Xi metadata. Anything left is a real gap.
    OPS_FILE=$(mktemp)
    NAMES_FILE=$(mktemp)
    awk '/XI_CONST = 0/,/XI_OP_COUNT/' src/ir/xi.h \
        | grep -oE '^[[:space:]]*XI_[A-Z0-9_]+' \
        | tr -d ' ' \
        | grep -v '^XI_OP_COUNT$' \
        | sort -u > "$OPS_FILE"
    grep -oE 'case XI_[A-Z0-9_]+' src/ir/xi_ops_gen.h \
        | sed 's/case //' \
        | grep -v '^XI_OP_COUNT$' \
        | sort -u > "$NAMES_FILE"
    MISSING=$(comm -23 "$OPS_FILE" "$NAMES_FILE")
    EXTRA=$(comm -13 "$OPS_FILE" "$NAMES_FILE")
    rm -f "$OPS_FILE" "$NAMES_FILE"
    if [ -z "$MISSING" ] && [ -z "$EXTRA" ]; then
        pass "every XI_* op has a generated name entry, no orphan cases"
    elif [ -n "$MISSING" ]; then
        fail "ops missing from generated Xi metadata:"
        printf "%s\n" "$MISSING" | sed 's/^/    /'
    else
        fail "generated Xi metadata has cases for unknown XI_* ops:"
        printf "%s\n" "$EXTRA" | sed 's/^/    /'
    fi
else
    warn "xi.h or xi_ops_gen.h not found, skipping"
fi

# --------------------------------------------------------------------------
# INV-2B: generated Xi/AOT metadata must match its source descriptions
# --------------------------------------------------------------------------
echo "--- INV-2B: generated Xi/AOT source sync ---"
GEN_LOG=$(mktemp)
if scripts/check_xi_aot_generated_sync.sh >"$GEN_LOG" 2>&1; then
    pass "generated Xi/AOT metadata is in sync"
    if [ "${VERBOSE}" -eq 1 ]; then
        cat "$GEN_LOG"
    fi
else
    fail "generated Xi/AOT metadata is stale or cannot be regenerated"
    cat "$GEN_LOG" | sed 's/^/    /'
fi
rm -f "$GEN_LOG"

# --------------------------------------------------------------------------
# INV-13: No functions > 150 lines in src/ir/
# --------------------------------------------------------------------------
echo "--- INV-13: Function length check (src/ir/) ---"
# Allowlist: pre-existing long functions grandfathered before strict enforcement.
# New code must not exceed 150 lines per function. Entries are "file:line" pairs.
# Each entry MUST be reviewed and removed when the function is refactored.
INV13_ALLOWLIST=(
    "xi_emit.c:xi_emit"
    "xi_emit_cf.c:emit_block"
    "xi_emit_object.c:emit_class_create_impl"
    "xi_escape.c:use_escape_level"
    "xi_loop.c:xi_compute_loops"
    "xi_lower_class.inc.c:xi_lower_class_decl"
    "xi_lower_expr.c:lower_call"
    "xi_lower_expr.c:lower_new_expr"
    "xi_lower_expr.c:xi_lower_expr"
    "xi_lower_stmt.c:lower_try_catch_impl"
)
is_allowlisted() {
    local file="$1" fname="$2"
    local base; base=$(basename "$file")
    for entry in "${INV13_ALLOWLIST[@]}"; do
        local afile="${entry%%:*}"
        local afunc="${entry##*:}"
        if [ "$base" = "$afile" ] && echo "$fname" | grep -q "$afunc"; then
            return 0
        fi
    done
    return 1
}
LONG_FUNCS=0
NEW_LONG=0
for f in src/ir/*.c; do
    [ -f "$f" ] || continue
    awk '
    /^[A-Za-z_].*\(/ && /\{[[:space:]]*$/ { start = NR; fname = $0; next }
    /^\}/ && start > 0 {
        len = NR - start
        if (len > 150) {
            printf "%s|%d|%d|%s\n", FILENAME, start, len, fname
        }
        start = 0
    }
    ' "$f" 2>/dev/null | while IFS='|' read -r file line len fname; do
        if is_allowlisted "$file" "$fname"; then
            if [ "${VERBOSE}" -eq 1 ]; then
                printf "  [ALLOW] %s:%d: ~%d lines: %s\n" "$file" "$line" "$len" "$fname"
            fi
        else
            printf "  [NEW-VIOLATION] %s:%d: ~%d lines (>150): %s\n" "$file" "$line" "$len" "$fname"
            echo "FAIL" > /tmp/xi_inv13_fail 2>/dev/null || true
        fi
    done
done
if [ -f /tmp/xi_inv13_fail ]; then
    rm -f /tmp/xi_inv13_fail
    fail "New functions exceeding 150 lines found in src/ir/"
else
    rm -f /tmp/xi_inv13_fail
    pass "No new functions exceed 150 lines in src/ir/"
fi

# --------------------------------------------------------------------------
# INV-10: Pipeline stats slot sync (pass table entries match stats capacity)
# --------------------------------------------------------------------------
echo "--- INV-10: Pipeline stats slot sync ---"
if [ -f src/ir/xi_opt.c ] && [ -f src/ir/xi_pass.h ]; then
    PASS_COUNT=$(awk '/static const XiPassDesc xi_pass_table\[\]/{in_table=1} in_table {print} in_table && /^};/{exit}' \
        src/ir/xi_opt.c \
        | grep -cE '\{"[a-z0-9_]+",' || true)
    MAX_STATS=$(grep -oE 'XI_MAX_PASS_STATS[[:space:]]+[0-9]+' src/ir/xi_pass.h \
        | grep -oE '[0-9]+' || true)
    if [ -z "$MAX_STATS" ]; then
        warn "XI_MAX_PASS_STATS not found in xi_pass.h"
    elif [ "$PASS_COUNT" -gt "$MAX_STATS" ]; then
        fail "Pass table has $PASS_COUNT entries but XI_MAX_PASS_STATS is only $MAX_STATS"
    else
        pass "Pass table ($PASS_COUNT entries) fits within XI_MAX_PASS_STATS ($MAX_STATS)"
    fi
else
    warn "xi_opt.c or xi_pass.h not found, skipping"
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
        fail "xi_opt_* functions not found in xi_opt.c (must register or mark as static helper):"
        printf "$MISSING"
    fi
else
    warn "xi_opt.c not found, skipping"
fi

# --------------------------------------------------------------------------
# INV-11: XRAY_XI_PASS must cover the whole pass table
# --------------------------------------------------------------------------
echo "--- INV-11: XRAY_XI_PASS pass table coverage ---"
if [ -f src/ir/xi_opt.c ]; then
    PASS_NAMES=$(awk '/static const XiPassDesc xi_pass_table\[\]/{in_table=1} in_table {print} in_table && /^};/{exit}' \
        src/ir/xi_opt.c \
        | grep -oE '\{"[a-z0-9_]+",' \
        | sed -E 's/\{"([^"]+)",/\1/' \
        | sort || true)
    DUP_NAMES=$(printf "%s\n" "$PASS_NAMES" | uniq -d)
    if ! grep -q 'XRAY_XI_PASS' src/ir/xi_opt.c || ! grep -q 'pass_index_by_name' src/ir/xi_opt.c; then
        fail "XRAY_XI_PASS parser or pass_index_by_name is missing"
    elif [ -n "$DUP_NAMES" ]; then
        fail "Duplicate Xi pass names:"
        printf "%s\n" "$DUP_NAMES" | sed 's/^/    /'
    else
        pass "XRAY_XI_PASS uses the single pass table and pass names are unique"
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
    fail "Passes covered only by test_xi_opt.c (must have dedicated test):"
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
