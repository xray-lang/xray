#!/bin/bash
# Shared binding copy lowering smoke.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
SRC="$PROJECT_DIR/tests/ir/shared_copy/shared_copy_to_shared_bytecode.xr"
PASS=0
FAIL=0

record_pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

record_fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

echo "=== Xi Shared Copy Tests ==="
echo "Binary: $XRAY"
echo ""

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_shared_copy.XXXXXX")" || exit 1
cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

"$XRAY" "$SRC" >"$WORK/run.out" 2>"$WORK/run.err"
if [ "$(cat "$WORK/run.out")" = "2" ]; then
    record_pass "shared copy: program output"
else
    record_fail "shared copy: unexpected output"
    sed 's/^/      stdout: /' "$WORK/run.out" | sed -n '1,40p'
    sed 's/^/      stderr: /' "$WORK/run.err" | sed -n '1,40p'
fi

"$XRAY" run --dump-bytecode "$SRC" >"$WORK/dump.out" 2>"$WORK/dump.err"
if grep -Eq '^[0-9]+.*[[:space:]]TO_SHARED[[:space:]]' "$WORK/dump.out"; then
    record_pass "shared copy: bytecode uses TO_SHARED"
else
    record_fail "shared copy: missing TO_SHARED bytecode"
    sed 's/^/      /' "$WORK/dump.out" | sed -n '1,120p'
fi

if grep -Eq '^[0-9]+.*[[:space:]]COPY[[:space:]]' "$WORK/dump.out"; then
    record_fail "shared copy: bytecode still uses COPY before shared store"
    sed 's/^/      /' "$WORK/dump.out" | sed -n '1,120p'
else
    record_pass "shared copy: bytecode avoids COPY"
fi

echo ""
echo "Passed: $PASS"
echo "Failed: $FAIL"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
