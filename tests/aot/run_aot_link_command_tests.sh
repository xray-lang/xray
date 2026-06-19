#!/bin/bash
# AOT link-command tests (115 stdlib-neutral ABI / symbol-level linking).
#
# The link manifest is the source of truth, but this smoke verifies the native
# build driver actually obeys it: core math direct calls stay freestanding, while
# runtime-backed stdlib modules still link xray_core and its system deps.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_aot_linkcmd.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
CACHE="$WORK/.cache"
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

build_native() {
    local src="$1"
    local out="$2"
    local log="$3"
    "$XRAY" build --native --dump-link-command --cache-dir "$CACHE" -o "$out" "$src" \
        >"$log" 2>&1
}

expect_log_contains() {
    local log="$1"
    local needle="$2"
    local name="$3"
    if grep -Fq -- "$needle" "$log"; then
        record_pass "$name"
    else
        record_fail "$name"
        sed 's/^/      /' "$log" | sed -n '1,80p'
    fi
}

expect_log_not_contains() {
    local log="$1"
    local needle="$2"
    local name="$3"
    if grep -Fq -- "$needle" "$log"; then
        record_fail "$name"
        sed 's/^/      /' "$log" | sed -n '1,80p'
    else
        record_pass "$name"
    fi
}

expect_output() {
    local bin="$1"
    local want="$2"
    local name="$3"
    local got
    got="$("$bin" 2>/dev/null)"
    if [ "$got" = "$want" ]; then
        record_pass "$name"
    else
        record_fail "$name: output '$got' != '$want'"
    fi
}

echo "=== AOT Link Command Tests ==="
echo "Binary: $XRAY"
echo ""

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

CORE_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_math_single_symbol.xr"
CORE_BIN="$WORK/core_math"
CORE_LOG="$WORK/core_math.log"
if build_native "$CORE_SRC" "$CORE_BIN" "$CORE_LOG"; then
    expect_log_contains "$CORE_LOG" "Link command:" "core-math: emitted link command"
    expect_log_not_contains "$CORE_LOG" "-lxray_core" "core-math: does not link xray_core"
    expect_log_not_contains "$CORE_LOG" "-lpthread" "core-math: does not link pthread"
    expect_log_not_contains "$CORE_LOG" "-lz" "core-math: does not link zlib"
    expect_log_contains "$CORE_LOG" "-lm" "core-math: links math lib only"
    expect_output "$CORE_BIN" "9.0" "core-math: binary output"
else
    record_fail "core-math: build failed"
    sed 's/^/      /' "$CORE_LOG" | sed -n '1,120p'
fi

PATH_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_path.xr"
PATH_BIN="$WORK/core_path"
PATH_LOG="$WORK/core_path.log"
if build_native "$PATH_SRC" "$PATH_BIN" "$PATH_LOG"; then
    expect_log_contains "$PATH_LOG" "Link command:" "core-path: emitted link command"
    expect_log_not_contains "$PATH_LOG" "-lxray_core" "core-path: does not link xray_core"
    expect_log_not_contains "$PATH_LOG" "-lpthread" "core-path: does not link pthread"
    expect_log_not_contains "$PATH_LOG" "-lz" "core-path: does not link zlib"
    expect_log_not_contains "$PATH_LOG" "-lssl" "core-path: does not link ssl"
    expect_log_not_contains "$PATH_LOG" "-lcrypto" "core-path: does not link crypto"
    expect_log_contains "$PATH_LOG" "-lm" "core-path: links math lib only"
    expect_output "$PATH_BIN" $'true\nxray\n/usr/local/bin\n.gz' "core-path: binary output"
else
    record_fail "core-path: build failed"
    sed 's/^/      /' "$PATH_LOG" | sed -n '1,120p'
fi

ENC_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_encoding.xr"
ENC_BIN="$WORK/core_encoding"
ENC_LOG="$WORK/core_encoding.log"
if build_native "$ENC_SRC" "$ENC_BIN" "$ENC_LOG"; then
    expect_log_contains "$ENC_LOG" "Link command:" "core-encoding: emitted link command"
    expect_log_not_contains "$ENC_LOG" "-lxray_core" "core-encoding: does not link xray_core"
    expect_log_not_contains "$ENC_LOG" "-lpthread" "core-encoding: does not link pthread"
    expect_log_not_contains "$ENC_LOG" "-lz" "core-encoding: does not link zlib"
    expect_log_not_contains "$ENC_LOG" "-lssl" "core-encoding: does not link ssl"
    expect_log_not_contains "$ENC_LOG" "-lcrypto" "core-encoding: does not link crypto"
    expect_log_contains "$ENC_LOG" "-lm" "core-encoding: links math lib only"
    expect_output "$ENC_BIN" $'true\nfalse\ntrue\n2\n6' "core-encoding: binary output"
else
    record_fail "core-encoding: build failed"
    sed 's/^/      /' "$ENC_LOG" | sed -n '1,120p'
fi

BASE64_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_base64.xr"
BASE64_BIN="$WORK/core_base64"
BASE64_LOG="$WORK/core_base64.log"
if build_native "$BASE64_SRC" "$BASE64_BIN" "$BASE64_LOG"; then
    expect_log_contains "$BASE64_LOG" "Link command:" "core-base64: emitted link command"
    expect_log_not_contains "$BASE64_LOG" "-lxray_core" "core-base64: does not link xray_core"
    expect_log_not_contains "$BASE64_LOG" "-lpthread" "core-base64: does not link pthread"
    expect_log_not_contains "$BASE64_LOG" "-lz" "core-base64: does not link zlib"
    expect_log_not_contains "$BASE64_LOG" "-lssl" "core-base64: does not link ssl"
    expect_log_not_contains "$BASE64_LOG" "-lcrypto" "core-base64: does not link crypto"
    expect_log_contains "$BASE64_LOG" "-lm" "core-base64: links math lib only"
    expect_output "$BASE64_BIN" $'SGVsbG8=\nHello\nQUI\nAB\nnull\ntrue\nSGVs\n3\n72\n101\n108\ntrue' "core-base64: binary output"
else
    record_fail "core-base64: build failed"
    sed 's/^/      /' "$BASE64_LOG" | sed -n '1,120p'
fi

RUNTIME_SRC="$PROJECT_DIR/tests/aot/filetests/link/runtime_time.xr"
RUNTIME_BIN="$WORK/runtime_time"
RUNTIME_LOG="$WORK/runtime_time.log"
if build_native "$RUNTIME_SRC" "$RUNTIME_BIN" "$RUNTIME_LOG"; then
    expect_log_contains "$RUNTIME_LOG" "Link command:" "runtime-time: emitted link command"
    expect_log_contains "$RUNTIME_LOG" "-lxray_core" "runtime-time: links xray_core"
    expect_log_contains "$RUNTIME_LOG" "-lpthread" "runtime-time: links pthread"
    expect_log_contains "$RUNTIME_LOG" "-lz" "runtime-time: links zlib"
    expect_output "$RUNTIME_BIN" "7" "runtime-time: binary output"
else
    record_fail "runtime-time: build failed"
    sed 's/^/      /' "$RUNTIME_LOG" | sed -n '1,120p'
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
