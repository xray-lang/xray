#!/bin/bash
# AOT cross-target smoke tests using Zig as the C toolchain driver.
#
# Usage:
#   XRAY_ZIG=/path/to/zig ./tests/aot/run_aot_cross_smoke.sh [xray_binary]
#
# The test builds a standalone/basic AOT program for the first-phase targets
# and checks that runtime-dependent AOT is still rejected for cross targets
# until per-target runtime objects are available.

set -u

XRAY="${1:-./build/xray}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK="${TMPDIR:-/tmp}/xray_aot_cross_smoke_$$"
BASIC_SRC="$WORK/basic_cross_standalone.xr"
RUNTIME_SRC="$SCRIPT_DIR/coro/typed_channel.xr"
ZIG_BIN="${XRAY_ZIG:-}"

PASS=0
FAIL=0
SKIP=0

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
export ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}"
export ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}"
mkdir -p "$ZIG_GLOBAL_CACHE_DIR" "$ZIG_LOCAL_CACHE_DIR"

cat > "$BASIC_SRC" <<'XR_EOF'
fn answer() -> int {
    return 42
}

answer()
XR_EOF

if [ -z "$ZIG_BIN" ]; then
    ZIG_BIN="$(command -v zig 2>/dev/null || true)"
fi

echo "=== AOT Cross-Target Smoke Tests ==="
echo "Binary: $XRAY"
echo "Work:   $WORK"

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY"
    exit 1
fi

if [ -z "$ZIG_BIN" ] || [ ! -x "$ZIG_BIN" ]; then
    echo "SKIP: Zig not found; set XRAY_ZIG=/path/to/zig"
    exit 77
fi

echo "Zig:    $ZIG_BIN"
echo ""

target_suffix() {
    case "$1" in
        *windows*) printf ".exe" ;;
        *) printf "" ;;
    esac
}

record_pass() {
    echo "PASS"
    PASS=$((PASS + 1))
}

record_fail() {
    echo "FAIL ($1)"
    FAIL=$((FAIL + 1))
}

run_cross_build() {
    local target="$1"
    local suffix
    local out
    local log

    suffix="$(target_suffix "$target")"
    out="$WORK/basic_cross_${target}${suffix}"
    log="$WORK/basic_cross_${target}.log"

    printf "  %-28s" "$target"

    if ! XRAY_ZIG="$ZIG_BIN" "$XRAY" build --native --target "$target" \
            --dump-link-command -o "$out" "$BASIC_SRC" >"$log" 2>&1; then
        record_fail "build failed"
        sed 's/^/    /' "$log" | head -20
        return
    fi

    if grep -q -- "-Wl,-dead_strip" "$log"; then
        record_fail "host-only dead_strip leaked into cross link"
        sed 's/^/    /' "$log" | head -20
        return
    fi

    if [ ! -s "$out" ]; then
        record_fail "missing output"
        return
    fi

    record_pass
    if command -v file >/dev/null 2>&1; then
        file "$out" | sed 's/^/    /'
    fi
}

run_runtime_rejection() {
    local target="x86_64-linux-musl"
    local out="$WORK/runtime_${target}"
    local log="$WORK/runtime_${target}.log"

    printf "  %-28s" "runtime-rejection"

    if XRAY_ZIG="$ZIG_BIN" "$XRAY" build --native --target "$target" \
            -o "$out" "$RUNTIME_SRC" >"$log" 2>&1; then
        record_fail "runtime-dependent cross build unexpectedly succeeded"
        return
    fi

    if ! grep -q "cannot consume runtime objects" "$log"; then
        record_fail "wrong rejection"
        sed 's/^/    /' "$log" | head -20
        return
    fi

    record_pass
}

for target in \
    x86_64-linux-musl \
    aarch64-linux-musl \
    x86_64-windows-gnu \
    aarch64-windows-gnu
do
    run_cross_build "$target"
done

run_runtime_rejection

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
