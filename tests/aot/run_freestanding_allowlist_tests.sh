#!/bin/bash
# Verify the freestanding stdlib allowlist by compiling each committed probe
# into a no-libc relocatable object and checking its undefined-symbol surface.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
ALLOWLIST="${XRAY_FREESTANDING_ALLOWLIST:-$PROJECT_DIR/stdlib/freestanding_allowlist}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_freestanding_allowlist.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
BUILD_CACHE="$WORK/.cache"
PASS=0
FAIL=0

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

record_pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

record_fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

nm_undefined_normalized() {
    nm -u "$1" 2>&1 |
        sed '/^[[:space:]]*$/d' |
        sed 's/.*[[:space:]]//; s/^_//'
}

undefined_allowed() {
    case "$1" in
        xr_hook_panic|xr_hook_write) return 0 ;;
        memcpy|memmove|memset|memcmp) return 0 ;;
        *) return 1 ;;
    esac
}

check_probe() {
    local module="$1"
    local probe_rel="$2"
    local probe="$PROJECT_DIR/$probe_rel"
    local out="$WORK/${module}.o"
    local log="$WORK/${module}.log"
    local bad=""
    local sym

    if [ ! -f "$probe" ]; then
        record_fail "$module: probe source missing ($probe_rel)"
        return
    fi

    if ! "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
            --dump-link-command \
            --cache-dir "$BUILD_CACHE" -o "$out" "$probe" >"$log" 2>&1; then
        record_fail "$module: probe build failed"
        sed 's/^/      /' "$log" | sed -n '1,120p'
        return
    fi

    if ! grep -Fq -- "-nostdlib" "$log"; then
        record_fail "$module: probe link command missing -nostdlib"
        sed 's/^/      /' "$log" | sed -n '1,80p'
        return
    fi

    if [ ! -f "$out" ]; then
        record_fail "$module: expected object not produced"
        sed 's/^/      /' "$log" | sed -n '1,80p'
        return
    fi

    while IFS= read -r sym; do
        [ -z "$sym" ] && continue
        if ! undefined_allowed "$sym"; then
            bad="${bad}${sym}
"
        fi
    done <<EOF
$(nm_undefined_normalized "$out")
EOF

    if [ -n "$bad" ]; then
        record_fail "$module: unexpected undefined symbols"
        printf '%s' "$bad" | sed 's/^/      /'
        return
    fi

    record_pass "$module: freestanding probe"
}

if [ ! -f "$ALLOWLIST" ]; then
    echo "FAIL: freestanding allowlist missing: $ALLOWLIST" >&2
    exit 1
fi

while IFS= read -r raw || [ -n "$raw" ]; do
    line="${raw%%#*}"
    set -- $line
    [ "$#" -eq 0 ] && continue
    if [ "$#" -ne 2 ]; then
        record_fail "bad allowlist line: $raw"
        continue
    fi
    check_probe "$1" "$2"
done < "$ALLOWLIST"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
