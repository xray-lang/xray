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

allowlist_modules() {
    while IFS= read -r raw || [ -n "$raw" ]; do
        # Task 257 owns cross-platform runner text normalization.  Strip the
        # Windows record terminator before tokenizing a governed text file.
        raw="${raw%$'\r'}"
        line="${raw%%#*}"
        set -- $line
        [ "$#" -eq 0 ] && continue
        if [ "$#" -eq 2 ]; then
            printf '%s\n' "$1"
        fi
    done < "$ALLOWLIST" | sort
}

analyzer_allowed_modules() {
    sed -n '/XR_FUNC bool xa_freestanding_stdlib_module_allowed/,/^}/p' \
        "$PROJECT_DIR/src/frontend/analyzer/xanalyzer_visitor.c" |
        grep -o 'strcmp(module_name, "[^"]*")' |
        sed 's/strcmp(module_name, "\([^"]*\)")/\1/' |
        sort
}

check_analyzer_allowlist_sync() {
    local expected="$WORK/allowlist_modules.txt"
    local actual="$WORK/analyzer_modules.txt"
    allowlist_modules >"$expected"
    analyzer_allowed_modules >"$actual"
    if diff -u "$expected" "$actual" >"$WORK/allowlist_sync.diff"; then
        record_pass "analyzer module allowlist matches stdlib/freestanding_allowlist"
        return
    fi
    record_fail "analyzer module allowlist drifted from stdlib/freestanding_allowlist"
    sed 's/^/      /' "$WORK/allowlist_sync.diff"
}

nm_undefined_normalized() {
    if command -v nm >/dev/null 2>&1; then
        nm -u "$1" 2>&1 |
            sed '/^[[:space:]]*$/d' |
            sed 's/.*[[:space:]]//; s/^_//'
        return
    fi
    if command -v dumpbin >/dev/null 2>&1; then
        # MSVC's COFF dumper is the canonical Windows provider.  Only external
        # undefined rows are dependencies; section/debug rows are definitions.
        dumpbin /symbols "$1" 2>&1 |
            sed -n 's/^.*UNDEF.*External.*|[[:space:]]*//p' |
            sed 's/^_//'
        return
    fi
    echo "__XRAY_SYMBOL_INSPECTOR_MISSING__"
}

undefined_allowed() {
    case "$1" in
        __XRAY_SYMBOL_INSPECTOR_MISSING__) return 1 ;;
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

    if ! grep -Fq -- "-nostdlib" "$log" &&
            ! grep -Fq -- "COFF relocatable: one amalgamated translation unit; no link stage" "$log"; then
        record_fail "$module: probe lacks a freestanding relocatable provider action"
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

check_analyzer_allowlist_sync

while IFS= read -r raw || [ -n "$raw" ]; do
    raw="${raw%$'\r'}"
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
