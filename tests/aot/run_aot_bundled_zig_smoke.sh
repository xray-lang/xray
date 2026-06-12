#!/bin/bash
# Verify that an installed/package-like Xray layout can discover bundled Zig.
#
# The smoke constructs:
#   <tmp>/pkg/bin/xray
#   <tmp>/pkg/libexec/xray/zig/zig
#
# Then it runs xray with XRAY_ZIG unset and a restricted PATH, so success means
# discovery came from the bundled layout rather than the developer shell.

set -u

XRAY="${1:-./build/xray}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_aot_bundled_zig.XXXXXX")" || {
    echo "error: cannot create temporary directory" >&2
    exit 1
}
PKG="$WORK/pkg"
BASIC_SRC="$WORK/basic_bundled_zig.xr"
OUT="$WORK/basic_bundled_zig"
DOCTOR_LOG="$WORK/doctor.log"
BUILD_LOG="$WORK/build.log"
RESTRICTED_PATH="/usr/bin:/bin"

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

resolve_path() {
    local path="$1"
    local link
    local dir

    while [ -L "$path" ]; do
        link="$(readlink "$path")"
        case "$link" in
            /*) path="$link" ;;
            *) path="$(dirname "$path")/$link" ;;
        esac
    done
    dir="$(cd -P "$(dirname "$path")" && pwd)"
    printf '%s/%s\n' "$dir" "$(basename "$path")"
}

find_zig() {
    local cand

    if [ -n "${XRAY_ZIG:-}" ]; then
        case "$XRAY_ZIG" in
            */*) cand="$XRAY_ZIG" ;;
            *) cand="$(command -v "$XRAY_ZIG" 2>/dev/null || true)" ;;
        esac
        if [ -n "$cand" ] && [ -x "$cand" ]; then
            printf '%s\n' "$cand"
            return 0
        fi
    fi

    command -v zig 2>/dev/null || true
}

run_with_bundled_env() {
    (
        unset XRAY_ZIG
        PATH="$RESTRICTED_PATH"
        export PATH
        "$@"
    )
}

ZIG_BIN="$(find_zig)"
if [ -z "$ZIG_BIN" ] || [ ! -x "$ZIG_BIN" ]; then
    echo "SKIP: Zig not found; set XRAY_ZIG=/path/to/zig or install Zig"
    exit 77
fi

ZIG_REAL="$(resolve_path "$ZIG_BIN")"
ZIG_ROOT="$(dirname "$ZIG_REAL")"
if [ ! -d "$ZIG_ROOT/lib" ]; then
    echo "SKIP: Zig root has no lib directory, cannot model bundled Zig: $ZIG_ROOT"
    exit 77
fi

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY"
    exit 1
fi

mkdir -p "$PKG/bin" "$PKG/libexec/xray"
ln -s "$XRAY" "$PKG/bin/xray"
ln -s "$ZIG_ROOT" "$PKG/libexec/xray/zig"

cat > "$BASIC_SRC" <<'XR_EOF'
fn answer() -> int {
    return 42
}

answer()
XR_EOF

EXPECTED_ZIG="$PKG/bin/../libexec/xray/zig/zig"

echo "=== AOT Bundled Zig Smoke ==="
echo "Binary:       $XRAY"
echo "Zig root:     $ZIG_ROOT"
echo "Package root: $PKG"
echo "Expected Zig: $EXPECTED_ZIG"
echo ""

if ! run_with_bundled_env "$PKG/bin/xray" toolchain doctor >"$DOCTOR_LOG" 2>&1; then
    echo "FAIL: bundled toolchain doctor failed"
    sed 's/^/    /' "$DOCTOR_LOG" | head -40
    exit 1
fi

if ! grep -Fq "$EXPECTED_ZIG" "$DOCTOR_LOG"; then
    echo "FAIL: doctor did not report bundled Zig path"
    sed 's/^/    /' "$DOCTOR_LOG" | head -40
    exit 1
fi

if ! run_with_bundled_env "$PKG/bin/xray" build --native --target x86_64-linux-musl \
        --dump-link-command -o "$OUT" "$BASIC_SRC" >"$BUILD_LOG" 2>&1; then
    echo "FAIL: bundled Zig cross build failed"
    sed 's/^/    /' "$BUILD_LOG" | head -60
    exit 1
fi

if ! grep -Fq "$EXPECTED_ZIG" "$BUILD_LOG"; then
    echo "FAIL: link command did not use bundled Zig path"
    sed 's/^/    /' "$BUILD_LOG" | head -60
    exit 1
fi

if [ ! -s "$OUT" ]; then
    echo "FAIL: missing cross build output"
    exit 1
fi

echo "PASS: bundled Zig discovery works with package layout"
if command -v file >/dev/null 2>&1; then
    file "$OUT" | sed 's/^/    /'
fi
