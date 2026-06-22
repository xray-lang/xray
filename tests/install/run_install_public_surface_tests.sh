#!/bin/bash
# Public install surface gate for task 132/M8.
#
# The installed product must not expose libxray_core.a. Installed AOT builds
# should use the precise runtime archives chosen by the link manifest, while
# bytecode embedders use the dedicated VM runtime archive.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$PROJECT_DIR/build}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_install_surface.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
PREFIX="$WORK/prefix"
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

print_log_tail() {
    local log="$1"
    if [ -f "$log" ]; then
        sed 's/^/      /' "$log" | sed -n '1,120p'
    fi
}

expect_file() {
    local path="$1"
    local name="$2"
    if [ -f "$path" ]; then
        record_pass "$name"
    else
        record_fail "$name: missing $path"
    fi
}

expect_executable() {
    local path="$1"
    local name="$2"
    if [ -x "$path" ]; then
        record_pass "$name"
    else
        record_fail "$name: missing executable $path"
    fi
}

echo "=== Install Public Surface Tests ==="
echo "Build:  $BUILD_DIR"
echo "Prefix: $PREFIX"
echo ""

install_log="$WORK/install.log"
if cmake --install "$BUILD_DIR" --prefix "$PREFIX" >"$install_log" 2>&1; then
    record_pass "cmake install"
else
    record_fail "cmake install"
    print_log_tail "$install_log"
fi

expect_executable "$PREFIX/bin/xray" "installed xray executable"
expect_file "$PREFIX/lib/libxray_aot_core.a" "installed xray_aot_core archive"
expect_file "$PREFIX/lib/libxray_rt_coro.a" "installed xray_rt_coro archive"
expect_file "$PREFIX/lib/libxray_vm_runtime.a" "installed xray_vm_runtime archive"

if find "$PREFIX/lib" -name 'libxray_core.a' -print -quit | grep -q .; then
    record_fail "does not install libxray_core.a"
else
    record_pass "does not install libxray_core.a"
fi

runtime_log="$WORK/runtime_time.log"
runtime_bin="$WORK/runtime_time"
if "$PREFIX/bin/xray" build --native --dump-link-command \
        "$PROJECT_DIR/tests/aot/filetests/link/runtime_time.xr" \
        -o "$runtime_bin" >"$runtime_log" 2>&1; then
    record_pass "installed xray builds runtime_time.xr"
else
    record_fail "installed xray builds runtime_time.xr"
    print_log_tail "$runtime_log"
fi

if grep -Fq -- "-lxray_core" "$runtime_log"; then
    record_fail "installed runtime_time link command does not use xray_core"
    print_log_tail "$runtime_log"
else
    record_pass "installed runtime_time link command does not use xray_core"
fi

if [ -x "$runtime_bin" ]; then
    got="$("$runtime_bin" 2>/dev/null)"
    if [ "$got" = "7" ]; then
        record_pass "installed runtime_time binary output"
    else
        record_fail "installed runtime_time binary output: '$got' != '7'"
    fi
else
    record_fail "installed runtime_time binary executable"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
