#!/bin/bash
# Compare Xray extern-layout introspection with the host C compiler ABI.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
CC_BIN="${CC:-cc}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_extern_layout.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "FAIL: C compiler not found: $CC_BIN" >&2
    exit 1
fi

if ! "$CC_BIN" -std=c11 -Wall -Wextra -Werror \
    "$SCRIPT_DIR/extern_layout_probe.c" -o "$WORK/c_probe"; then
    echo "FAIL: C extern-layout probe did not compile" >&2
    exit 1
fi

if ! "$WORK/c_probe" >"$WORK/c.out"; then
    echo "FAIL: C extern-layout probe did not run" >&2
    exit 1
fi

XRAY_SOURCE="$PROJECT_DIR/tests/diff/cases/semantics/ffi/extern_layout_introspection.xr"
if ! "$XRAY" run "$XRAY_SOURCE" >"$WORK/xray.out" 2>"$WORK/xray.err"; then
    echo "FAIL: Xray extern-layout probe did not run" >&2
    sed 's/^/    /' "$WORK/xray.err" >&2
    exit 1
fi

if ! diff -u "$WORK/c.out" "$WORK/xray.out"; then
    echo "FAIL: Xray extern layout differs from the host C ABI" >&2
    exit 1
fi

echo "PASS: Xray extern layout matches the host C ABI"
