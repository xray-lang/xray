#!/bin/bash
# Native AOT debug smoke: verifies `xray build --native -g` produces source-level
# debug info that lldb can break on by .xr file and line.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
PASS=0
FAIL=0

skip() {
    echo "SKIP: $1"
    exit 77
}

fail() {
    echo "FAIL: $1" >&2
    FAIL=$((FAIL + 1))
}

if [ "$(uname -s)" != "Darwin" ]; then
    skip "native debug lldb smoke currently requires Darwin dSYM"
fi

if ! command -v dsymutil >/dev/null 2>&1; then
    skip "dsymutil not found"
fi

if ! command -v lldb >/dev/null 2>&1; then
    skip "lldb not found"
fi

DWARFDUMP="${LLVM_DWARFDUMP:-}"
if [ -z "$DWARFDUMP" ]; then
    if command -v llvm-dwarfdump >/dev/null 2>&1; then
        DWARFDUMP="$(command -v llvm-dwarfdump)"
    elif [ -x /opt/homebrew/opt/llvm/bin/llvm-dwarfdump ]; then
        DWARFDUMP="/opt/homebrew/opt/llvm/bin/llvm-dwarfdump"
    elif [ -x /usr/local/opt/llvm/bin/llvm-dwarfdump ]; then
        DWARFDUMP="/usr/local/opt/llvm/bin/llvm-dwarfdump"
    else
        skip "llvm-dwarfdump not found"
    fi
fi

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_native_debug.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

BIN="$WORK/hello_debug"
SRC="$WORK/debug_locals.xr"
BUILD_LOG="$WORK/build.log"
DWARF_LOG="$WORK/dwarf.log"
LLDB_LOG="$WORK/lldb.log"

cat > "$SRC" <<'XR'
fn compute(seed: int) -> int {
    if (seed <= 0) { return 0 }
    let answer = seed + 1
    let doubled = answer * 2
    print(doubled)
    return compute(seed - seed) + doubled
}
let runtimeSeed = process.args.length > 1000 ? 1 : 20
compute(runtimeSeed)
XR

echo "=== Native Debug Smoke ==="
echo "Binary: $XRAY"

if ! "$XRAY" build --native -g --dump-link-command -o "$BIN" "$SRC" > "$BUILD_LOG" 2>&1; then
    fail "debug build failed"
    sed -n '1,80p' "$BUILD_LOG" >&2
    exit 1
fi

if ! grep -Fq "Compile command:" "$BUILD_LOG"; then
    fail "debug build did not split compile command"
fi
if ! grep -Fq "Debug info command: dsymutil" "$BUILD_LOG"; then
    fail "debug build did not run dsymutil"
fi
if ! grep -Fq -- "-DXRAY_AOT_DEBUG_LOCALS=1" "$BUILD_LOG"; then
    fail "debug build did not enable AOT source-variable locals"
fi

RUN_OUT="$("$BIN" 2>&1)"
if [ "$RUN_OUT" != "42" ]; then
    fail "debug binary output mismatch: $RUN_OUT"
fi

if [ ! -d "$BIN.dSYM" ]; then
    fail "debug build did not produce dSYM"
else
    if ! "$DWARFDUMP" --debug-line "$BIN.dSYM" > "$DWARF_LOG" 2>&1; then
        fail "llvm-dwarfdump failed"
    elif ! grep -Fq 'name: "debug_locals.xr"' "$DWARF_LOG"; then
        fail "dSYM line table does not reference debug_locals.xr"
    fi
fi

if ! lldb --no-lldbinit -b \
        -o "breakpoint set --file debug_locals.xr --line 5" \
        -o run \
        -o "frame variable answer" \
        -o "frame variable doubled" \
        -o bt -- "$BIN" > "$LLDB_LOG" 2>&1; then
    fail "lldb failed"
fi
if ! grep -Fq "stop reason = breakpoint" "$LLDB_LOG"; then
    fail "lldb did not stop at breakpoint"
fi
if ! grep -Fq "debug_locals_compute" "$LLDB_LOG"; then
    fail "lldb backtrace did not show xray-derived function name"
fi
if ! grep -Fq "debug_locals.xr:5" "$LLDB_LOG"; then
    fail "lldb breakpoint/backtrace did not report debug_locals.xr:5"
fi
if ! grep -Eq "answer = 21" "$LLDB_LOG"; then
    fail "lldb did not expose source local answer=21"
fi
if ! grep -Eq "doubled = 42" "$LLDB_LOG"; then
    fail "lldb did not expose source local doubled=42"
fi

if [ "$FAIL" -ne 0 ]; then
    echo "--- build log ---" >&2
    sed -n '1,120p' "$BUILD_LOG" >&2
    echo "--- lldb log ---" >&2
    sed -n '1,160p' "$LLDB_LOG" >&2
    exit 1
fi

PASS=$((PASS + 1))
echo "=== Results: $PASS passed, 0 failed, 0 skipped ==="
