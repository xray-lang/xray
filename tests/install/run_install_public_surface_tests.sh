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
CORE_PREFIX="$WORK/core-prefix"
PASS=0
FAIL=0
HOST_TARGET="$($BUILD_DIR/xray toolchain list --target native --json | \
    python3 -c 'import json,sys; print(json.load(sys.stdin)["normalizedTarget"])')"

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

expect_absent() {
    local path="$1"
    local name="$2"
    if [ ! -e "$path" ]; then
        record_pass "$name"
    else
        record_fail "$name: unexpected $path"
    fi
}

echo "=== Install Public Surface Tests ==="
echo "Build:  $BUILD_DIR"
echo "Prefix: $PREFIX"
echo ""

core_install_log="$WORK/core-install.log"
if cmake --install "$BUILD_DIR" --prefix "$CORE_PREFIX" --component XrayCore \
        >"$core_install_log" 2>&1; then
    record_pass "Core component install"
else
    record_fail "Core component install"
    print_log_tail "$core_install_log"
fi
expect_executable "$CORE_PREFIX/bin/xray" "Core installed xray executable"
expect_file "$CORE_PREFIX/lib/xray/vm/$HOST_TARGET/libxray_vm_runtime.a" "Core installed VM runtime"
expect_file "$CORE_PREFIX/lib/xray/stdlib/path/path.xr" "Core installed stdlib"
expect_file "$CORE_PREFIX/share/xray/install/install-marker.json" "Core installed marker"
expect_file "$CORE_PREFIX/share/xray/install/payload-manifest.json" "Core payload manifest"
expect_absent "$CORE_PREFIX/include/xray/xray.h" "Core excludes SDK headers"
expect_absent "$CORE_PREFIX/lib/xray/aot/$HOST_TARGET/libxray_aot_core.a" \
    "Core excludes AOT SDK archive"

if python3 "$PROJECT_DIR/scripts/verify_payload_manifest.py" \
        --root "$CORE_PREFIX" --binary "$CORE_PREFIX/bin/xray"; then
    record_pass "Core payload manifest and binary identity"
else
    record_fail "Core payload manifest and binary identity"
fi

core_eval="$($CORE_PREFIX/bin/xray -e 'print("ok")' 2>&1)"
if [ "$core_eval" = "ok" ]; then
    record_pass "Core eval smoke"
else
    record_fail "Core eval smoke: '$core_eval'"
fi
cp "$PROJECT_DIR/tests/test_harness/single_pass.xr" "$WORK/core_smoke.xr"
if "$CORE_PREFIX/bin/xray" check "$WORK/core_smoke.xr" >/dev/null 2>&1; then
    record_pass "Core check smoke"
else
    record_fail "Core check smoke"
fi
if "$CORE_PREFIX/bin/xray" fmt "$WORK/core_smoke.xr" >/dev/null 2>&1 && \
        "$CORE_PREFIX/bin/xray" fmt --check "$WORK/core_smoke.xr" >/dev/null 2>&1; then
    record_pass "Core fmt smoke"
else
    record_fail "Core fmt smoke"
fi
if "$CORE_PREFIX/bin/xray" test --quiet "$WORK/core_smoke.xr" >/dev/null 2>&1; then
    record_pass "Core test smoke"
else
    record_fail "Core test smoke"
fi
if "$CORE_PREFIX/bin/xray" compile "$WORK/core_smoke.xr" -o "$WORK/core_smoke.xrc" \
        >/dev/null 2>&1 && [ -f "$WORK/core_smoke.xrc" ]; then
    record_pass "Core compile smoke"
else
    record_fail "Core compile smoke"
fi

install_log="$WORK/install.log"
if cmake --install "$BUILD_DIR" --prefix "$PREFIX" >"$install_log" 2>&1; then
    record_pass "cmake install"
else
    record_fail "cmake install"
    print_log_tail "$install_log"
fi

expect_executable "$PREFIX/bin/xray" "installed xray executable"
expect_file "$PREFIX/lib/xray/aot/$HOST_TARGET/libxray_aot_core.a" \
    "installed xray_aot_core archive"
expect_file "$PREFIX/lib/xray/aot/$HOST_TARGET/libxray_rt_coro.a" \
    "installed xray_rt_coro archive"
expect_file "$PREFIX/lib/xray/aot/$HOST_TARGET/manifest.json" "installed runtime manifest"
expect_file "$PREFIX/lib/xray/vm/$HOST_TARGET/libxray_vm_runtime.a" \
    "installed xray_vm_runtime archive"
expect_file "$PREFIX/lib/xray/sdk/src/aot/xrt.h" "installed private AOT SDK"
expect_file "$PREFIX/lib/xray/stdlib/path/path.xr" "installed stdlib source"
expect_file "$PREFIX/share/xray/install/install-marker.json" "installed payload marker"
expect_file "$PREFIX/share/xray/install/payload-manifest.json" "installed payload manifest"

if python3 "$PROJECT_DIR/scripts/verify_payload_manifest.py" \
        --root "$PREFIX" --binary "$PREFIX/bin/xray"; then
    record_pass "full payload manifest and binary identity"
else
    record_fail "full payload manifest and binary identity"
fi

if find "$PREFIX/lib" -name 'libxray_core.a' -print -quit | grep -q .; then
    record_fail "does not install libxray_core.a"
else
    record_pass "does not install libxray_core.a"
fi

runtime_log="$WORK/runtime_time.log"
runtime_bin="$WORK/runtime_time"
cp "$PROJECT_DIR/tests/aot/filetests/link/runtime_time.xr" "$WORK/runtime_time.xr"
if (cd "$WORK" && unset XRAY_INCLUDE XRAY_LIB XRAY_STDLIB_PATH && \
        export XDG_CONFIG_HOME="$WORK/config" XDG_CACHE_HOME="$WORK/cache" && \
        "$PREFIX/bin/xray" build --native --dump-link-command \
        "$WORK/runtime_time.xr" -o "$runtime_bin") >"$runtime_log" 2>&1; then
    record_pass "installed xray builds runtime_time.xr"
else
    record_fail "installed xray builds runtime_time.xr"
    print_log_tail "$runtime_log"
fi

if grep -Fq "$PROJECT_DIR/src/aot" "$runtime_log"; then
    record_fail "installed AOT command is independent of the source tree"
    print_log_tail "$runtime_log"
else
    record_pass "installed AOT command is independent of the source tree"
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
