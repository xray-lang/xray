#!/bin/bash
# Audits zero-cost feature coverage against current AOT plan/filetest evidence.
#
# The inventory deliberately permits explicit gap rows. That keeps the TODO
# surface executable: every zero-cost family is either backed by a current
# plan/dump/filetest pattern or named as unfinished work.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
INVENTORY="${1:-$SCRIPT_DIR/plan_inventory.tsv}"
PASS=0
GAP=0
FAIL=0

if [ ! -f "$INVENTORY" ]; then
    echo "FAIL: inventory not found: $INVENTORY" >&2
    exit 1
fi

search_pattern() {
    local pattern="$1"
    local scope="$2"
    local root="$PROJECT_DIR/$scope"

    if [ ! -e "$root" ]; then
        return 1
    fi
    if command -v rg >/dev/null 2>&1; then
        rg -q -- "$pattern" "$root"
    else
        grep -R -E -q -- "$pattern" "$root"
    fi
}

line_no=0
while IFS= read -r raw || [ -n "$raw" ]; do
    line_no=$((line_no + 1))
    line="${raw%$'\r'}"
    case "$line" in
        ""|\#*) continue ;;
    esac

    old_ifs="$IFS"
    IFS='|'
    read -r feature status pattern scope note extra <<EOF
$line
EOF
    IFS="$old_ifs"

    if [ -n "${extra:-}" ] || [ -z "$feature" ] || [ -z "$status" ] ||
            [ -z "$pattern" ] || [ -z "$scope" ] || [ -z "$note" ]; then
        echo "FAIL line $line_no: expected feature|status|pattern|scope|note"
        FAIL=$((FAIL + 1))
        continue
    fi

    case "$status" in
        covered)
            if search_pattern "$pattern" "$scope"; then
                echo "PASS $feature: $note"
                PASS=$((PASS + 1))
            else
                echo "FAIL $feature: missing pattern '$pattern' under $scope"
                FAIL=$((FAIL + 1))
            fi
            ;;
        gap)
            echo "GAP  $feature: $note"
            GAP=$((GAP + 1))
            ;;
        *)
            echo "FAIL $feature: unknown status '$status'"
            FAIL=$((FAIL + 1))
            ;;
    esac
done < "$INVENTORY"

echo "=== zero-cost inventory: $PASS covered, $GAP gaps, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
