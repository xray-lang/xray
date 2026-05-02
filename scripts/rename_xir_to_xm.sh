#!/usr/bin/env bash
# rename_xir_to_xm.sh — Mechanical rename: Xir → Xm (Xray Machine IR)
#
# Renames files and symbols in src/jit/, tests/unit/jit/, tests/unit/ir/,
# and any cross-references in src/ir/, src/vm/, src/coro/, src/runtime/,
# src/api/, src/app/, src/aot/.
#
# Usage:  scripts/rename_xir_to_xm.sh
# Safety: Run from repo root. Creates no backup (use git to revert).

set -euo pipefail
export LC_ALL=C
export LANG=C
cd "$(git rev-parse --show-toplevel)"

echo "=== Step 1: Rename files with git mv ==="

# src/jit/xir_* → src/jit/xm_*
for f in src/jit/xir_*.c src/jit/xir_*.h; do
    [ -f "$f" ] || continue
    new=$(echo "$f" | sed 's|/xir_|/xm_|')
    git mv "$f" "$new"
    echo "  $f → $new"
done

# src/jit/xir.c/h → src/jit/xm.c/h (main IR files)
for f in src/jit/xir.c src/jit/xir.h; do
    [ -f "$f" ] || continue
    new=$(echo "$f" | sed 's|/xir\.|/xm.|')
    git mv "$f" "$new"
    echo "  $f → $new"
done

# src/jit/xi_to_xir.* → src/jit/xi_to_xm.*
for f in src/jit/xi_to_xir.*; do
    [ -f "$f" ] || continue
    new=$(echo "$f" | sed 's|xi_to_xir|xi_to_xm|')
    git mv "$f" "$new"
    echo "  $f → $new"
done

# tests/unit/jit/test_xir* → tests/unit/jit/test_xm*
for f in tests/unit/jit/test_xir*; do
    [ -f "$f" ] || continue
    new=$(echo "$f" | sed 's|test_xir|test_xm|')
    git mv "$f" "$new"
    echo "  $f → $new"
done

# tests/unit/ir/test_xi_to_xir* → tests/unit/ir/test_xi_to_xm*
for f in tests/unit/ir/test_xi_to_xir*; do
    [ -f "$f" ] || continue
    new=$(echo "$f" | sed 's|xi_to_xir|xi_to_xm|')
    git mv "$f" "$new"
    echo "  $f → $new"
done

echo ""
echo "=== Step 2: Symbol rename in all source files ==="

# Collect all C/H files that may contain xir/Xir/XIR references
FILES=$(find src/ tests/unit/ -name '*.c' -o -name '*.h' -o -name '*.inc.c' 2>/dev/null)
FILES="$FILES tests/unit/CMakeLists.txt"

for f in $FILES; do
    [ -f "$f" ] || continue
    # Skip files with no xir/Xir/XIR references (fast path)
    grep -q 'xir\|Xir\|XIR' "$f" 2>/dev/null || continue

    # 1. Multi-word compounds first (avoid partial matches)
    sed -i '' -e 's/xi_to_xir/xi_to_xm/g' -e 's/XI_TO_XIR/XI_TO_XM/g' "$f"

    # 2. Type names: Xir[A-Z]... → Xm[A-Z]...
    sed -i '' 's/Xir\([A-Z][A-Za-z0-9]*\)/Xm\1/g' "$f"

    # 3. Macro prefix: XIR_ → XM_
    sed -i '' 's/XIR_/XM_/g' "$f"

    # 4. Function/variable prefix: xir_ → xm_
    sed -i '' 's/xir_/xm_/g' "$f"

    # 5. Include paths: "xir.h" → "xm.h", "xir_xxx" → "xm_xxx"
    sed -i '' -e 's|"xir\.h"|"xm.h"|g' -e 's|"xir_|"xm_|g' "$f"
done

echo ""
echo "=== Step 3: Fix CMakeLists.txt references ==="
# The main CMakeLists.txt uses GLOB so no individual file lists to update.
# But tests/unit/CMakeLists.txt may reference test files by name.
if [ -f tests/unit/CMakeLists.txt ]; then
    sed -i '' \
        -e 's/test_xir/test_xm/g' \
        -e 's/xi_to_xir/xi_to_xm/g' \
        -e 's/xir_/xm_/g' \
        tests/unit/CMakeLists.txt
    echo "  Updated tests/unit/CMakeLists.txt"
fi

echo ""
echo "=== Done! ==="
echo "Next steps:"
echo "  1. Review: git diff --stat"
echo "  2. Build:  cd build && cmake .. && cmake --build . -j8"
echo "  3. Test:   ctest --output-on-failure"
echo "  4. Commit: git add -A && git commit -m 'Rename Xir to Xm (Xray Machine IR)'"
