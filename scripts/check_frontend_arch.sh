#!/bin/bash
# check_frontend_arch.sh -- frontend (lexer/parser/format/analyzer/codegen)
# include-boundary lint, plus a few size invariants the refactor plan
# (docs/engineering/frontend_refactor_plan_final.md) calls out.
#
# Run from project root:
#     scripts/check_frontend_arch.sh
#
# Exit 0 if all rules hold, exit 1 on any violation. Designed to be
# wired into CI; no external deps beyond grep / awk / find.

set -euo pipefail

SRC_DIR="src"
ERRORS=0

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; NC=''
fi

fail() { echo -e "  ${RED}FAIL${NC}: $1"; ERRORS=$((ERRORS + 1)); }
pass() { echo -e "  ${GREEN}PASS${NC}: $1"; }
note() { echo -e "  ${YELLOW}NOTE${NC}: $1"; }

echo "============================================"
echo "  xray frontend architecture lint"
echo "============================================"
echo

# --------------------------------------------------------------------
# Rule 1: lexer/ MUST NOT include runtime/ (lexer is L1, runtime is L2)
# --------------------------------------------------------------------
echo "--- R1: lexer/ -> runtime/ ---"
hits=$(grep -rnE '#include[[:space:]]+["<].*runtime/' \
       --include='*.c' --include='*.h' "$SRC_DIR/frontend/lexer/" 2>/dev/null || true)
if [ -n "$hits" ]; then
    fail "lexer/ includes runtime/:"
    echo "$hits" | sed 's/^/      /'
else
    pass "lexer/ does not include runtime/"
fi
echo

# --------------------------------------------------------------------
# Rule 2: parser/ MUST NOT include analyzer/ (P-01)
# parser must be self-sufficient w.r.t. type scope.
# --------------------------------------------------------------------
echo "--- R2: parser/ -> analyzer/ ---"
hits=$(grep -rnE '#include[[:space:]]+["<].*analyzer/' \
       --include='*.c' --include='*.h' "$SRC_DIR/frontend/parser/" 2>/dev/null || true)
if [ -n "$hits" ]; then
    fail "parser/ includes analyzer/:"
    echo "$hits" | sed 's/^/      /'
else
    pass "parser/ does not include analyzer/"
fi
echo

# --------------------------------------------------------------------
# Rule 3: format/ MUST NOT include analyzer/ (F-04)
# formatter must be a pure AST -> text leaf.
# --------------------------------------------------------------------
echo "--- R3: format/ -> analyzer/ ---"
hits=$(grep -rnE '#include[[:space:]]+["<].*analyzer/' \
       --include='*.c' --include='*.h' "$SRC_DIR/frontend/format/" 2>/dev/null || true)
if [ -n "$hits" ]; then
    fail "format/ includes analyzer/:"
    echo "$hits" | sed 's/^/      /'
else
    pass "format/ does not include analyzer/"
fi
echo

# --------------------------------------------------------------------
# Rule 4: format/ MUST NOT include the public xray API headers (F-04).
# Anything used by the formatter must be available below L7.
# --------------------------------------------------------------------
echo "--- R4: format/ -> include/xray*.h ---"
hits=$(grep -rnE '#include[[:space:]]+["<](xray|xray_isolate|xray_embedding)\.h' \
       --include='*.c' --include='*.h' "$SRC_DIR/frontend/format/" 2>/dev/null || true)
if [ -n "$hits" ]; then
    fail "format/ includes public API headers:"
    echo "$hits" | sed 's/^/      /'
else
    pass "format/ does not include public API headers"
fi
echo

# --------------------------------------------------------------------
# Rule 5: frontend/** MUST NOT include xray.h / xray_isolate.h (C-01)
# Internal frontend code must use runtime/* / vm/* internal headers,
# not the L7 public API surface.
# --------------------------------------------------------------------
echo "--- R5: frontend/** -> include/xray.h or include/xray_isolate.h ---"
hits=$(grep -rnE '#include[[:space:]]+["<](xray|xray_isolate)\.h' \
       --include='*.c' --include='*.h' "$SRC_DIR/frontend/" 2>/dev/null || true)
if [ -n "$hits" ]; then
    fail "frontend/** includes public API headers:"
    echo "$hits" | sed 's/^/      /'
else
    pass "frontend/** does not include public API headers"
fi
echo

# --------------------------------------------------------------------
# Rule 6: analyzer/ MUST NOT include codegen/
# analyzer is an upward dependency from codegen, never the other way.
# --------------------------------------------------------------------
echo "--- R6: analyzer/ -> codegen/ ---"
hits=$(grep -rnE '#include[[:space:]]+["<].*codegen/' \
       --include='*.c' --include='*.h' "$SRC_DIR/frontend/analyzer/" 2>/dev/null || true)
if [ -n "$hits" ]; then
    fail "analyzer/ includes codegen/:"
    echo "$hits" | sed 's/^/      /'
else
    pass "analyzer/ does not include codegen/"
fi
echo

# --------------------------------------------------------------------
# Rule 7: codegen/ MUST NOT include format/
# formatter is a leaf and must not be reached from codegen.
# --------------------------------------------------------------------
echo "--- R7: codegen/ -> format/ ---"
hits=$(grep -rnE '#include[[:space:]]+["<].*format/' \
       --include='*.c' --include='*.h' "$SRC_DIR/frontend/codegen/" 2>/dev/null || true)
if [ -n "$hits" ]; then
    fail "codegen/ includes format/:"
    echo "$hits" | sed 's/^/      /'
else
    pass "codegen/ does not include format/"
fi
echo

# --------------------------------------------------------------------
# Rule 8: AstNode must NOT carry compile_type / compile_type_legacy
# (X-01 retired the inline field; types live in xa_node_table only)
# --------------------------------------------------------------------
echo "--- R8: AstNode has no inline semantic-state fields ---"
# Match field declarations only, not commentary references. A field
# declaration in this codebase looks like `... compile_type;` (typed
# member) or `... *compile_type;`; we accept both. The grep is anchored
# so that `// ... compile_type ...` style comments do not trip it.
hits=$(grep -nE '^[^/]*\bcompile_type(_legacy)?[[:space:]]*[;,\[]' \
       "$SRC_DIR/frontend/parser/xast_nodes.h" 2>/dev/null || true)
if [ -n "$hits" ]; then
    fail "xast_nodes.h still declares compile_type / compile_type_legacy:"
    echo "$hits" | sed 's/^/      /'
else
    pass "xast_nodes.h has no compile_type field declaration"
fi
echo

# --------------------------------------------------------------------
# Rule 9: frontend .c size cap (3000 lines, project-wide standard).
# A failure here usually means a hot file needs cohesion-based split.
# --------------------------------------------------------------------
echo "--- R9: frontend .c file size cap (≤ 3000 lines) ---"
oversize=$(find "$SRC_DIR/frontend" -name '*.c' -exec wc -l {} + 2>/dev/null \
           | awk '$1 > 3000 && $2 != "total" { print $1 " " $2 }' || true)
if [ -n "$oversize" ]; then
    fail "frontend .c files over 3000 lines:"
    echo "$oversize" | sed 's/^/      /'
else
    pass "all frontend .c files within size limit"
fi
echo

# --------------------------------------------------------------------
# Rule 10: frontend .h size cap (800 lines).
# --------------------------------------------------------------------
echo "--- R10: frontend .h file size cap (≤ 800 lines) ---"
oversize=$(find "$SRC_DIR/frontend" -name '*.h' -exec wc -l {} + 2>/dev/null \
           | awk '$1 > 800 && $2 != "total" { print $1 " " $2 }' || true)
if [ -n "$oversize" ]; then
    fail "frontend .h files over 800 lines:"
    echo "$oversize" | sed 's/^/      /'
else
    pass "all frontend .h files within size limit"
fi
echo

# --------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------
echo "============================================"
if [ "$ERRORS" -gt 0 ]; then
    echo -e "${RED}Frontend arch lint FAILED ($ERRORS violation(s))${NC}"
    exit 1
else
    echo -e "${GREEN}Frontend arch lint passed${NC}"
    exit 0
fi
