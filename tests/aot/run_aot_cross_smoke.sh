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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_aot_cross_smoke.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
BASIC_SRC="$WORK/basic_cross_standalone.xr"
TIME_SRC="$WORK/time_cross_standalone.xr"
SHARED_DIR="$WORK/shared_cross"
SHARED_SRC="$SHARED_DIR/main.xr"
RUNTIME_SRC="$SCRIPT_DIR/coro/typed_channel.xr"
ZIG_BIN="${XRAY_ZIG:-}"

PASS=0
FAIL=0
SKIP=0

trap 'rm -rf "$WORK"' EXIT

cat > "$BASIC_SRC" <<'XR_EOF'
fn answer() -> int {
    return 42
}

answer()
XR_EOF

mkdir -p "$SHARED_DIR"
cat > "$SHARED_SRC" <<'XR_EOF'
export fn answer() -> int {
    return 42
}
XR_EOF
cat > "$SHARED_DIR/xray.toml" <<'TOML_EOF'
[package]
name = "test/windows-shared-cross"
version = "0.1.0"

[[export.c]]
xray = "answer"
symbol = "xr_answer"
visibility = "default"
header = true
TOML_EOF

cat > "$TIME_SRC" <<'XR_EOF'
import time

print(time.now() > 0)
print(time.nanos() > 0)
print(time.clock() >= 0)
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

XRAY_DIR="$(cd "$(dirname "$XRAY")" && pwd)"
ZIG_CACHE_ROOT="${XRAY_ZIG_CACHE_ROOT:-$XRAY_DIR/zig-cache}"
export ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$ZIG_CACHE_ROOT/global}"
export ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}"
mkdir -p "$ZIG_GLOBAL_CACHE_DIR" "$ZIG_LOCAL_CACHE_DIR"

echo "Zig:    $ZIG_BIN"
echo "Cache:  $ZIG_GLOBAL_CACHE_DIR"
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

record_parallel_result() {
    local log="$1"
    cat "$log"
    if grep -q "FAIL" "$log"; then
        FAIL=$((FAIL + 1))
    elif grep -q "PASS" "$log"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL (worker produced no result): $(basename "$log")"
    fi
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

run_windows_time_build() {
    local target="x86_64-windows-gnu"
    local out="$WORK/time_${target}.exe"
    local log="$WORK/time_${target}.log"

    printf "  %-28s" "windows-time"

    if ! XRAY_ZIG="$ZIG_BIN" "$XRAY" build --native --target "$target" \
            --dump-link-command -o "$out" "$TIME_SRC" >"$log" 2>&1; then
        record_fail "build failed"
        sed 's/^/    /' "$log" | head -20
        return
    fi
    if grep -q -- "-lxray_aot_core" "$log"; then
        record_fail "host AOT core leaked into cross link"
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

run_windows_shared_build() {
    local target="x86_64-windows-gnu"
    local out="$WORK/shared_${target}.dll"
    local log="$WORK/shared_${target}.log"

    printf "  %-28s" "windows-shared"

    if ! XRAY_ZIG="$ZIG_BIN" "$XRAY" build --native --shared --target "$target" \
            --dump-link-command -o "$out" "$SHARED_SRC" >"$log" 2>&1; then
        record_fail "build failed"
        sed 's/^/    /' "$log" | head -20
        return
    fi
    if ! grep -q -- "-shared" "$log" || grep -q -- "-dynamiclib" "$log"; then
        record_fail "wrong target shared-library flag"
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

mkdir -p "$WORK/logs"
pids=""
logs=""
for target in \
    i386-linux-musl \
    x86_64-linux-musl \
    aarch64-linux-musl \
    powerpc64-linux-musl \
    x86_64-windows-gnu \
    aarch64-windows-gnu
do
    log="$WORK/logs/${target}.log"
    ( run_cross_build "$target" >"$log" 2>&1 ) &
    pids="$pids $!"
    logs="$logs $log"
done

runtime_log="$WORK/logs/runtime_rejection.log"
( run_runtime_rejection >"$runtime_log" 2>&1 ) &
pids="$pids $!"
logs="$logs $runtime_log"

time_log="$WORK/logs/windows_time.log"
( run_windows_time_build >"$time_log" 2>&1 ) &
pids="$pids $!"
logs="$logs $time_log"

shared_log="$WORK/logs/windows_shared.log"
( run_windows_shared_build >"$shared_log" 2>&1 ) &
pids="$pids $!"
logs="$logs $shared_log"

for p in $pids; do
    wait "$p"
done

for log in $logs; do
    record_parallel_result "$log"
done

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
