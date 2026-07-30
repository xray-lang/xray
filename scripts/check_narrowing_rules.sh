#!/bin/bash
# check_narrowing_rules.sh -- three-way reconciliation for flow-sensitive
# type narrowing (LANGUAGE_SPEC §2.13):
#
#   1. every rule N-x declared in the spec section is cited by at least one
#      conformance test under tests/compile_errors/narrowing or
#      tests/regression/16_narrowing;
#   2. every rule is cited by the analyzer source that implements it, so a
#      rule can never be silently dropped from the compiler;
#   3. the spec section, the MCP knowledge card, and the E0379 error-code row
#      all exist and reference each other.
#
# Narrowing feeds code generation (the narrowed type is what lowering sees),
# so an unimplemented or untested rule is a VM/AOT divergence risk, not a
# documentation nit. That is why this is fail-closed.
#
# Run from project root:
#     scripts/check_narrowing_rules.sh
#
# Exit 0 if all rules hold, exit 1 on any violation. No external deps beyond
# grep / sed / sort.

set -euo pipefail

SPEC_SECTION="spec/source/sections/003-2-type-system.md"
CARD="spec/source/cards/topics/narrowing.json"
ERROR_CODES="spec/source/sections/019-18-error-codes.md"
TEST_DIRS="tests/compile_errors/narrowing tests/regression/16_narrowing"
IMPL_FILES="src/frontend/analyzer/xanalyzer_flow.c
src/frontend/analyzer/xanalyzer_flow.h
src/frontend/analyzer/xanalyzer_visitor.c
src/frontend/analyzer/xanalyzer_visitor_expr.c
src/frontend/analyzer/xanalyzer_visitor_call.c
src/frontend/analyzer/xanalyzer_visitor_stmt.c
src/frontend/analyzer/xanalyzer_visitor_internal.h
src/frontend/parser/xparse_decl.c
src/frontend/parser/xast_nodes_expr.h"

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
else
    RED=''; GREEN=''; NC=''
fi

ERRORS=0
fail() { echo -e "  ${RED}FAIL${NC}: $1"; ERRORS=$((ERRORS + 1)); }
pass() { echo -e "  ${GREEN}PASS${NC}: $1"; }

echo "============================================"
echo "Narrowing rule reconciliation (spec §2.13)"
echo "============================================"

for f in "${SPEC_SECTION}" "${CARD}" "${ERROR_CODES}"; do
    if [ ! -f "${f}" ]; then
        fail "missing source of truth: ${f}"
        echo "aborting: cannot reconcile without ${f}"
        exit 1
    fi
done

# Rule ids declared in the spec: bold `**N-4 ...**` / `**N-11.1 ...**` markers.
RULES=$(grep -o '\*\*N-[0-9][0-9.]*' "${SPEC_SECTION}" | sed 's/\*\*//' | sort -u)
RULE_COUNT=$(printf '%s\n' "${RULES}" | grep -c . || true)

if [ "${RULE_COUNT}" -lt 10 ]; then
    fail "expected at least 10 narrowing rules in ${SPEC_SECTION}, found ${RULE_COUNT}"
else
    pass "${RULE_COUNT} narrowing rules declared in ${SPEC_SECTION}"
fi

echo ""
echo "-- rule -> conformance test"
for dir in ${TEST_DIRS}; do
    if [ ! -d "${dir}" ]; then
        fail "missing conformance test directory: ${dir}"
    fi
done

for rule in ${RULES}; do
    # A test cites a rule either in prose ("N-4") or through its file name
    # (n04_..., n11_1_...). Both forms count.
    slug=$(printf '%s' "${rule}" | sed 's/^N-//; s/\./_/g')
    padded=$(printf 'n%02d' "${slug%%_*}" 2>/dev/null || printf 'n%s' "${slug%%_*}")
    if grep -Rqs -- "${rule}" ${TEST_DIRS} 2>/dev/null; then
        pass "${rule} cited by a conformance test"
    elif ls ${TEST_DIRS} 2>/dev/null | grep -qs "^${padded}"; then
        pass "${rule} covered by a ${padded}* test file"
    else
        fail "${rule} has no conformance test under: ${TEST_DIRS}"
    fi
done

echo ""
echo "-- rule -> analyzer implementation"
for rule in ${RULES}; do
    if grep -qs -- "${rule}" ${IMPL_FILES} 2>/dev/null; then
        pass "${rule} cited by the implementation"
    else
        fail "${rule} is not referenced by any implementation file; either implement it or drop the rule"
    fi
done

echo ""
echo "-- cross references"
if grep -qs 'E0379' "${SPEC_SECTION}"; then
    pass "§2.13 references E0379"
else
    fail "§2.13 must state which error code its null rules raise (E0379)"
fi

if grep -qs '2.13' "${ERROR_CODES}"; then
    pass "E0379 row points back at §2.13"
else
    fail "the E0379 row in ${ERROR_CODES} must point at §2.13"
fi

if grep -qs 'narrowing' "${CARD}" && grep -qs 'E0379' "${CARD}"; then
    pass "MCP knowledge card covers narrowing and E0379"
else
    fail "${CARD} must cover narrowing and cite E0379"
fi

# The diagnostic text is the user-facing half of N-12; keep it discoverable.
if grep -qs 'possibly-null value' src/frontend/analyzer/xanalyzer_visitor_expr.c; then
    pass "E0379 diagnostic text present in the analyzer"
else
    fail "E0379 diagnostic text missing from the analyzer"
fi

echo ""
echo "============================================"
if [ "${ERRORS}" -eq 0 ]; then
    echo -e "${GREEN}All narrowing reconciliation checks passed${NC}"
    exit 0
fi
echo -e "${RED}${ERRORS} narrowing reconciliation failure(s)${NC}"
exit 1
