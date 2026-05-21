#!/bin/bash
# check_architecture.sh - Automated architecture constraint checker for xray
#
# Enforces module layering, file size limits, and forbidden direct calls
# (raw malloc/free, custom hash impls, etc.).
# Run from project root: scripts/check_architecture.sh

set -euo pipefail

SRC_DIR="src"
INC_DIR="include"
ERRORS=0
WARNINGS=0

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[0;31m' YELLOW='\033[1;33m' GREEN='\033[0;32m' CYAN='\033[0;36m' NC='\033[0m'
else
    RED='' YELLOW='' GREEN='' CYAN='' NC=''
fi

fail() { echo -e "  ${RED}FAIL${NC}: $1"; ERRORS=$((ERRORS + 1)); }
warn() { echo -e "  ${YELLOW}WARN${NC}: $1"; WARNINGS=$((WARNINGS + 1)); }
pass() { echo -e "  ${GREEN}PASS${NC}: $1"; }
info() { echo -e "  ${CYAN}INFO${NC}: $1"; }

echo "============================================"
echo "  xray Architecture Constraint Checker"
echo "============================================"
echo ""

# -----------------------------------------------
# Q-1: .c file size limit (3000 lines)
# -----------------------------------------------
echo "--- Q-1: .c file size limit (≤ 3000 lines) ---"
q1_count=0
while IFS= read -r line; do
    lines=$(echo "$line" | awk '{print $1}')
    file=$(echo "$line" | awk '{print $2}')
    if [ "$lines" -gt 3000 ] 2>/dev/null; then
        fail "$file has $lines lines (limit: 3000)"
        q1_count=$((q1_count + 1))
    fi
done < <(find "$SRC_DIR" -name '*.c' -exec wc -l {} + 2>/dev/null | grep -v 'total$')
[ "$q1_count" -eq 0 ] && pass "All .c files within size limit"
echo ""

# -----------------------------------------------
# Q-2: .h file size limit (800 lines)
# -----------------------------------------------
echo "--- Q-2: .h file size limit (≤ 800 lines) ---"
q2_count=0
while IFS= read -r line; do
    lines=$(echo "$line" | awk '{print $1}')
    file=$(echo "$line" | awk '{print $2}')
    if [ "$lines" -gt 800 ] 2>/dev/null; then
        fail "$file has $lines lines (limit: 800)"
        q2_count=$((q2_count + 1))
    fi
done < <(find "$SRC_DIR" -name '*.h' -exec wc -l {} + 2>/dev/null | grep -v 'total$')
[ "$q2_count" -eq 0 ] && pass "All .h files within size limit"
echo ""

# -----------------------------------------------
# Q-3: No direct malloc / free / calloc / realloc
# -----------------------------------------------
# Rule: all heap allocations must go through xr_malloc / xr_free /
#       xr_calloc / xr_realloc.
#
# WHITELIST (legitimate libc allocator use):
#   - src/aot/xrt.h  : self-contained AOT runtime header that is embedded
#     into generated C binaries and must run without any xray/isolate
#     infrastructure. Using libc malloc/free here is intentional.
#   - Lines marked with a trailing "xr:allow-raw-alloc" comment: opt-out
#     annotation for rare legitimate libc allocator pairs, e.g.
#     posix_memalign / _aligned_malloc / _aligned_free which must be
#     matched with libc free/_aligned_free.
#
# We scan BOTH .c and .h files (static-inline helpers in headers can also
# leak raw allocator calls — see audit §6.2).
#
# Exclusions:
#   - The xr_* wrappers themselves (prefix-anchored so "xr_free_tracked"
#     and friends keep matching).
#   - Embedded LuaM_* allocator shim (third-party symbol namespace).
#   - Lines that are #include directives or in-line comments mentioning
#     the banned names.
#   - Whitelist file src/base/xmalloc.h which implements the wrappers
#     (it must call the real allocator exactly once).
#   - Word-boundary-matched stdlib noise like "free_list_head" that
#     happens to contain "free" — we require an opening paren.
echo "--- Q-3: No direct malloc/free/calloc/realloc ---"
q3_hits=$(grep -rnE '\b(malloc|free|calloc|realloc)[[:space:]]*\(' \
        --include='*.c' --include='*.h' \
        --exclude='xmalloc.h' \
        --exclude='xrt.h' \
        "$SRC_DIR" 2>/dev/null \
    | grep -vE '\b(xr|luaM)_(malloc|free|calloc|realloc)\b' \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*|/\*)' \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*#[[:space:]]*include' \
    | grep -vE 'xr:allow-raw-alloc' \
    || true)
if [ -n "$q3_hits" ]; then
    count=$(echo "$q3_hits" | wc -l | tr -d ' ')
    fail "Found $count direct malloc/free/calloc/realloc call(s) (use xr_* wrappers instead)"
    echo "$q3_hits" | head -5 | sed 's/^/    /'
    [ "$count" -gt 5 ] && echo "    ... and $((count - 5)) more"
else
    pass "No direct malloc/free/calloc/realloc found"
fi
echo ""

# -----------------------------------------------
# Q-4: Header export count (≤ 25 per .h)
# -----------------------------------------------
echo "--- Q-4: Header export count (≤ 25 exported functions per .h) ---"
q4_count=0
while IFS= read -r hfile; do
    # Count function declarations (non-static, non-inline, non-macro, non-typedef)
    exports=$(grep -cE '^(XR_FUNC|XRAY_API|XR_FUNC_NORET|XRAY_API_NORET) ' "$hfile" 2>/dev/null || true)
    exports=${exports:-0}
    if [ "$exports" -gt 25 ] 2>/dev/null; then
        warn "$hfile exports $exports functions (limit: 25)"
        q4_count=$((q4_count + 1))
    fi
done < <(find "$SRC_DIR" -name '*.h')
[ "$q4_count" -eq 0 ] && pass "All headers within export limit (or no visibility macros yet)"
echo ""

# -----------------------------------------------
# Q-5: Assert density (target: ≤ 100 lines per assert)
# -----------------------------------------------
echo "--- Q-5: Assert density ---"
total_lines=$(find "$SRC_DIR" -name '*.c' -exec cat {} + 2>/dev/null | wc -l)
total_asserts=$(find "$SRC_DIR" -name '*.c' -exec grep -c 'XR_DCHECK\|XR_CHECK\|XR_ASSERT\|assert(' {} + 2>/dev/null \
    | awk -F: '{s+=$NF}END{print s+0}')
if [ "$total_asserts" -gt 0 ]; then
    density=$((total_lines / total_asserts))
    if [ "$density" -gt 100 ]; then
        warn "Assert density: 1 per $density lines (target: ≤ 100)"
    else
        pass "Assert density: 1 per $density lines"
    fi
else
    warn "No asserts found!"
fi
echo ""

# -----------------------------------------------
# Q-6: static function ratio (target: ≥ 80%)
# -----------------------------------------------
echo "--- Q-6: static function ratio (target: ≥ 80%) ---"
static_funcs=$(grep -rn '^static ' --include='*.c' "$SRC_DIR" 2>/dev/null | grep -c '(' || true)
# Count non-static function definitions (heuristic: lines starting with a type/name and containing '(')
all_func_defs=$(grep -rn '^[a-zA-Z_]' --include='*.c' "$SRC_DIR" 2>/dev/null \
    | grep '(' | grep -v '^.*:#' | grep -v '^\s*//' \
    | grep -v 'typedef\|struct \|enum \|if \|for \|while \|switch \|return \|else\|case \|default:' \
    | wc -l | tr -d ' ')
if [ "$all_func_defs" -gt 0 ]; then
    ratio=$((static_funcs * 100 / all_func_defs))
    if [ "$ratio" -lt 80 ]; then
        warn "static ratio: ${ratio}% (${static_funcs}/${all_func_defs}) — target: ≥ 80%"
    else
        pass "static ratio: ${ratio}% (${static_funcs}/${all_func_defs})"
    fi
else
    info "Could not compute static ratio"
fi
echo ""

# -----------------------------------------------
# Q-7: Visibility macro adoption
# -----------------------------------------------
echo "--- Q-7: Visibility macro adoption ---"
xr_func_count=$(grep -rn 'XR_FUNC ' --include='*.h' "$SRC_DIR" 2>/dev/null | grep -v '#define' | wc -l | tr -d ' ')
xray_api_count=$(grep -rn 'XRAY_API ' --include='*.h' "$SRC_DIR" "$INC_DIR" 2>/dev/null | grep -v '#define' | wc -l | tr -d ' ')
bare_extern_count=$(grep -rn '^[a-zA-Z]' --include='*.h' "$SRC_DIR" 2>/dev/null \
    | grep '(' | grep -v 'static \|inline \|#define\|typedef\|XR_FUNC\|XRAY_API\|XR_DEFINE\|extern' \
    | grep -v '_H$\|_H ' \
    | wc -l | tr -d ' ')
info "XR_FUNC declarations: $xr_func_count"
info "XRAY_API declarations: $xray_api_count"
if [ "$bare_extern_count" -gt 0 ]; then
    warn "~$bare_extern_count function declarations without visibility macro in src/*.h"
else
    pass "All header function declarations have visibility macros"
fi
echo ""

# -----------------------------------------------
# Q-8: Circular header dependencies
# -----------------------------------------------
echo "--- Q-8: Circular header dependencies ---"
cycles=$(python3 -c "
import os, re
from collections import defaultdict

headers = {}
for root, dirs, files in os.walk('$SRC_DIR'):
    for f in files:
        if f.endswith('.h'):
            headers[f] = os.path.join(root, f)

deps = defaultdict(set)
for name, path in headers.items():
    with open(path) as fh:
        for line in fh:
            m = re.match(r'#include\s+[\"<]([^\">/]+\.h)[>\"]', line.strip())
            if m and m.group(1) in headers:
                deps[name].add(m.group(1))

WHITE, GRAY, BLACK = 0, 1, 2
color = {n: WHITE for n in headers}
cycles = []
def dfs(node, path):
    color[node] = GRAY
    path.append(node)
    for nb in deps.get(node, set()):
        if color.get(nb) == GRAY:
            idx = path.index(nb)
            cycles.append(' -> '.join(path[idx:] + [nb]))
        elif color.get(nb, WHITE) == WHITE:
            dfs(nb, path)
    path.pop()
    color[node] = BLACK
for n in list(headers.keys()):
    if color.get(n, WHITE) == WHITE:
        dfs(n, [])
for c in cycles:
    print(c)
" 2>/dev/null || true)
if [ -n "$cycles" ]; then
    count=$(echo "$cycles" | wc -l | tr -d ' ')
    fail "Found $count circular header dependency chain(s):"
    echo "$cycles" | sed 's/^/    /'
else
    pass "No circular header dependencies"
fi
echo ""

# -----------------------------------------------
# Q-9: Include depth from .c files
# -----------------------------------------------
echo "--- Q-9: Top .c files by #include count ---"
while IFS= read -r line; do
    count=$(echo "$line" | awk '{print $1}')
    file=$(echo "$line" | awk '{print $2}')
    if [ "$count" -gt 20 ]; then
        warn "$file has $count #includes (target: ≤ 20)"
    fi
done < <(find "$SRC_DIR" -name '*.c' -exec grep -c '#include' {} + 2>/dev/null \
    | awk -F: '{print $2, $1}' | sort -rn | head -10)
echo ""

# -----------------------------------------------
# Q-10: Comment ratio (target: ≥ 18%)
# -----------------------------------------------
echo "--- Q-10: Comment ratio (target: ≥ 18%) ---"
total_src_lines=$(find "$SRC_DIR" -name '*.c' -exec cat {} + 2>/dev/null | wc -l | tr -d ' ')
comment_lines=$(find "$SRC_DIR" -name '*.c' -exec grep -c '^\s*/\*\|^\s*\*\|^\s*//' {} + 2>/dev/null \
    | awk -F: '{s+=$NF}END{print s+0}')
if [ "$total_src_lines" -gt 0 ]; then
    comment_pct=$((comment_lines * 100 / total_src_lines))
    if [ "$comment_pct" -lt 18 ]; then
        warn "Comment ratio: ${comment_pct}% (${comment_lines}/${total_src_lines}) — target: ≥ 18%"
    else
        pass "Comment ratio: ${comment_pct}% (${comment_lines}/${total_src_lines})"
    fi
fi
echo ""

# -----------------------------------------------
# Q-11: Assert density per module (top deficits)
# -----------------------------------------------
echo "--- Q-11: Assert density per module ---"
for mod_dir in base runtime runtime/value runtime/gc runtime/object runtime/class coro vm jit frontend/codegen frontend/parser api module aot; do
    if [ -d "$SRC_DIR/$mod_dir" ]; then
        c_files=$(find "$SRC_DIR/$mod_dir" -maxdepth 1 -name '*.c' 2>/dev/null)
        [ -z "$c_files" ] && continue
        mod_lines=$(echo "$c_files" | xargs cat 2>/dev/null | wc -l | tr -d ' ')
        mod_asserts=$(echo "$c_files" | xargs grep -c 'XR_DCHECK\|XR_CHECK\|XR_ASSERT\|assert(' 2>/dev/null \
            | awk -F: '{s+=$NF}END{print s+0}' || echo 0)
        [ "$mod_lines" -eq 0 ] && continue
        if [ "$mod_asserts" -gt 0 ]; then
            mod_density=$((mod_lines / mod_asserts))
            label="1/${mod_density}"
        else
            mod_density=999999
            label="NONE"
        fi
        if [ "$mod_asserts" -eq 0 ] && [ "$mod_lines" -gt 100 ]; then
            warn "$mod_dir: $mod_lines lines, 0 asserts"
        elif [ "$mod_asserts" -gt 0 ] && [ "$mod_density" -gt 200 ]; then
            warn "$mod_dir: $mod_lines lines, $mod_asserts asserts ($label) — target: ≤ 1/100"
        else
            info "$mod_dir: $mod_lines lines, $mod_asserts asserts ($label)"
        fi
    fi
done
echo ""

# -----------------------------------------------
# Q-12: Layer violation check (post-903 restructure)
# -----------------------------------------------
echo "--- Q-12: Layer violations (post-restructure) ---"
q12_count=0
# base/ should not include anything from runtime/, coro/, vm/, jit/, frontend/, api/, app/
for bad_dep in runtime coro vm jit frontend api app module aot; do
    hits=$(grep -rn "#include.*\".*/$bad_dep/\|#include.*\"$bad_dep/" --include='*.c' --include='*.h' "$SRC_DIR/base/" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        count=$(echo "$hits" | wc -l | tr -d ' ')
        fail "base/ → $bad_dep/: $count upward includes"
        q12_count=$((q12_count + count))
    fi
done
# runtime/ should not include from vm/, jit/, frontend/, api/, app/
for bad_dep in vm jit frontend api app; do
    hits=$(grep -rn "#include.*/$bad_dep/\|#include.*\"$bad_dep/" --include='*.c' --include='*.h' "$SRC_DIR/runtime/" 2>/dev/null | grep -v 'xvm_call.h\|xstdlib_bridge.h' || true)
    if [ -n "$hits" ]; then
        count=$(echo "$hits" | wc -l | tr -d ' ')
        warn "runtime/ → $bad_dep/: $count upward includes"
        q12_count=$((q12_count + count))
    fi
done
# frontend/ should not include from jit/, aot/, app/
for bad_dep in jit aot app; do
    hits=$(grep -rn "#include.*/$bad_dep/\|#include.*\"$bad_dep/" --include='*.c' --include='*.h' "$SRC_DIR/frontend/" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        count=$(echo "$hits" | wc -l | tr -d ' ')
        fail "frontend/ → $bad_dep/: $count upward includes"
        q12_count=$((q12_count + count))
    fi
done
# ir/ should not include from jit/, aot/, app/ (except xrt_method_symbols.h used by cgen)
for bad_dep in jit app; do
    hits=$(grep -rn "#include.*/$bad_dep/\|#include.*\"$bad_dep/" --include='*.c' --include='*.h' "$SRC_DIR/ir/" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        count=$(echo "$hits" | wc -l | tr -d ' ')
        fail "ir/ → $bad_dep/: $count upward includes"
        q12_count=$((q12_count + count))
    fi
done
[ "$q12_count" -eq 0 ] && pass "No layer violations detected"
echo ""

# -----------------------------------------------
# Q-14: Dead path regression guards
# -----------------------------------------------
echo "--- Q-14: Dead path regression guards ---"
q14_count=0
# M1: aot_mode must not exist in JIT builder
hits=$(grep -rn "aot_mode" --include='*.c' --include='*.h' "$SRC_DIR/jit/" 2>/dev/null || true)
if [ -n "$hits" ]; then
    count=$(echo "$hits" | wc -l | tr -d ' ')
    fail "aot_mode still present in src/jit/ ($count hits)"
    q14_count=$((q14_count + count))
fi
# M2: old import API must not exist
hits=$(grep -rn "xi_cgen_reset_imports\|xi_cgen_add_import" --include='*.c' --include='*.h' "$SRC_DIR/" 2>/dev/null || true)
if [ -n "$hits" ]; then
    count=$(echo "$hits" | wc -l | tr -d ' ')
    fail "xi_cgen_reset_imports/xi_cgen_add_import still present ($count hits)"
    q14_count=$((q14_count + count))
fi
[ "$q14_count" -eq 0 ] && pass "No dead path regressions"
echo ""

# -----------------------------------------------
# Q-13: File header comment check
# -----------------------------------------------
echo "--- Q-13: File header comments ---"
q13_missing=0
q13_total=0
while IFS= read -r cfile; do
    q13_total=$((q13_total + 1))
    first_line=$(head -1 "$cfile" 2>/dev/null)
    if ! echo "$first_line" | grep -q '/\*'; then
        q13_missing=$((q13_missing + 1))
    fi
done < <(find "$SRC_DIR" -name '*.c' -o -name '*.h' | head -200)
if [ "$q13_missing" -gt 0 ]; then
    q13_pct=$((q13_missing * 100 / q13_total))
    warn "$q13_missing/$q13_total files missing header comment (${q13_pct}%)"
else
    pass "All files have header comments"
fi
echo ""

# -----------------------------------------------
# Summary
# -----------------------------------------------
echo "============================================"
echo "  Results: $ERRORS errors, $WARNINGS warnings"
echo "============================================"

if [ "$ERRORS" -gt 0 ]; then
    echo -e "${RED}Architecture check FAILED${NC}"
    exit 1
else
    echo -e "${GREEN}Architecture check passed (with warnings)${NC}"
    exit 0
fi
