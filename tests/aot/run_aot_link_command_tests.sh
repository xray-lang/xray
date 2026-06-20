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

CORE_FULL_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_math.xr"
CORE_FULL_BIN="$WORK/core_math_full"
CORE_FULL_LOG="$WORK/core_math_full.log"
if build_native "$CORE_FULL_SRC" "$CORE_FULL_BIN" "$CORE_FULL_LOG"; then
    expect_log_contains "$CORE_FULL_LOG" "Link command:" "core-math-full: emitted link command"
    expect_log_not_contains "$CORE_FULL_LOG" "-lxray_core" "core-math-full: does not link xray_core"
    expect_log_not_contains "$CORE_FULL_LOG" "-lpthread" "core-math-full: does not link pthread"
    expect_log_not_contains "$CORE_FULL_LOG" "-lz" "core-math-full: does not link zlib"
    expect_log_contains "$CORE_FULL_LOG" "-lm" "core-math-full: links math lib only"
    expect_output "$CORE_FULL_BIN" $'9.0\n8.0\n81.0\n7\n1\n2\n2\n1\n0.0\n1.0\n0.0\n0.0\n0.0\n0.0\n0.0\n0.0\n2.0\n3.0\n1.0\n0.0\n1.0\n0.0\n5.0\n3.0\n3.0\n0.0\n0.0\n2.5\n0.0\n0.0\n-1\ntrue\ntrue\nfalse\n3\n7\n5' "core-math-full: binary output"
else
    record_fail "core-math-full: build failed"
    sed 's/^/      /' "$CORE_FULL_LOG" | sed -n '1,120p'
fi

RANDOM_MATH_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_math_random.xr"
RANDOM_MATH_BIN="$WORK/system_math_random"
RANDOM_MATH_LOG="$WORK/system_math_random.log"
if build_native "$RANDOM_MATH_SRC" "$RANDOM_MATH_BIN" "$RANDOM_MATH_LOG"; then
    expect_log_contains "$RANDOM_MATH_LOG" "Link command:" "system-math-random: emitted link command"
    expect_log_not_contains "$RANDOM_MATH_LOG" "-lxray_core" "system-math-random: does not link xray_core"
    expect_log_not_contains "$RANDOM_MATH_LOG" "-lpthread" "system-math-random: does not link pthread"
    expect_log_not_contains "$RANDOM_MATH_LOG" "-lz" "system-math-random: does not link zlib"
    expect_log_contains "$RANDOM_MATH_LOG" "-lxray_aot_core" "system-math-random: links AOT core stdlib only"
    expect_log_contains "$RANDOM_MATH_LOG" "-lm" "system-math-random: links math lib"
    expect_output "$RANDOM_MATH_BIN" $'true\n7' "system-math-random: binary output"
else
    record_fail "system-math-random: build failed"
    sed 's/^/      /' "$RANDOM_MATH_LOG" | sed -n '1,120p'
fi

RANDOM_CRYPTO_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_crypto_random.xr"
RANDOM_CRYPTO_BIN="$WORK/system_crypto_random"
RANDOM_CRYPTO_LOG="$WORK/system_crypto_random.log"
if build_native "$RANDOM_CRYPTO_SRC" "$RANDOM_CRYPTO_BIN" "$RANDOM_CRYPTO_LOG"; then
    expect_log_contains "$RANDOM_CRYPTO_LOG" "Link command:" "system-crypto-random: emitted link command"
    expect_log_not_contains "$RANDOM_CRYPTO_LOG" "-lxray_core" "system-crypto-random: does not link xray_core"
    expect_log_not_contains "$RANDOM_CRYPTO_LOG" "-lpthread" "system-crypto-random: does not link pthread"
    expect_log_not_contains "$RANDOM_CRYPTO_LOG" "-lz" "system-crypto-random: does not link zlib"
    expect_log_not_contains "$RANDOM_CRYPTO_LOG" "-lssl" "system-crypto-random: does not link ssl"
    expect_log_not_contains "$RANDOM_CRYPTO_LOG" "-lcrypto" "system-crypto-random: does not link crypto"
    expect_log_contains "$RANDOM_CRYPTO_LOG" "-lxray_aot_core" "system-crypto-random: links AOT core stdlib only"
    expect_log_contains "$RANDOM_CRYPTO_LOG" "-lm" "system-crypto-random: links math lib"
    expect_output "$RANDOM_CRYPTO_BIN" $'true\ntrue\ntrue' "system-crypto-random: binary output"
else
    record_fail "system-crypto-random: build failed"
    sed 's/^/      /' "$RANDOM_CRYPTO_LOG" | sed -n '1,120p'
fi

TIME_QUERY_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_time_queries.xr"
TIME_QUERY_BIN="$WORK/system_time_queries"
TIME_QUERY_LOG="$WORK/system_time_queries.log"
if build_native "$TIME_QUERY_SRC" "$TIME_QUERY_BIN" "$TIME_QUERY_LOG"; then
    expect_log_contains "$TIME_QUERY_LOG" "Link command:" "system-time-query: emitted link command"
    expect_log_not_contains "$TIME_QUERY_LOG" "-lxray_core" "system-time-query: does not link xray_core"
    expect_log_not_contains "$TIME_QUERY_LOG" "-lpthread" "system-time-query: does not link pthread"
    expect_log_not_contains "$TIME_QUERY_LOG" "-lz" "system-time-query: does not link zlib"
    expect_log_contains "$TIME_QUERY_LOG" "-lxray_aot_core" "system-time-query: links AOT core stdlib only"
    expect_log_contains "$TIME_QUERY_LOG" "-lm" "system-time-query: links math lib"
    expect_output "$TIME_QUERY_BIN" $'true\ntrue\ntrue\ntrue\ntrue' "system-time-query: binary output"
else
    record_fail "system-time-query: build failed"
    sed 's/^/      /' "$TIME_QUERY_LOG" | sed -n '1,120p'
fi

OS_QUERY_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_os_queries.xr"
OS_QUERY_BIN="$WORK/system_os_queries"
OS_QUERY_LOG="$WORK/system_os_queries.log"
if build_native "$OS_QUERY_SRC" "$OS_QUERY_BIN" "$OS_QUERY_LOG"; then
    expect_log_contains "$OS_QUERY_LOG" "Link command:" "system-os-query: emitted link command"
    expect_log_not_contains "$OS_QUERY_LOG" "-lxray_core" "system-os-query: does not link xray_core"
    expect_log_not_contains "$OS_QUERY_LOG" "-lxray_aot_core" "system-os-query: does not link AOT core"
    expect_log_not_contains "$OS_QUERY_LOG" "-lpthread" "system-os-query: does not link pthread"
    expect_log_not_contains "$OS_QUERY_LOG" "-lz" "system-os-query: does not link zlib"
    expect_log_contains "$OS_QUERY_LOG" "-lm" "system-os-query: links math lib only"
    expect_output "$OS_QUERY_BIN" $'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue' "system-os-query: binary output"
else
    record_fail "system-os-query: build failed"
    sed 's/^/      /' "$OS_QUERY_LOG" | sed -n '1,120p'
fi

OS_EXEC_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_os_exec.xr"
OS_EXEC_BIN="$WORK/system_os_exec"
OS_EXEC_LOG="$WORK/system_os_exec.log"
if build_native "$OS_EXEC_SRC" "$OS_EXEC_BIN" "$OS_EXEC_LOG"; then
    expect_log_contains "$OS_EXEC_LOG" "Link command:" "system-os-exec: emitted link command"
    expect_log_not_contains "$OS_EXEC_LOG" "-lxray_core" "system-os-exec: does not link xray_core"
    expect_log_not_contains "$OS_EXEC_LOG" "-lxray_aot_core" "system-os-exec: does not link AOT core"
    expect_log_not_contains "$OS_EXEC_LOG" "-lpthread" "system-os-exec: does not link pthread"
    expect_log_not_contains "$OS_EXEC_LOG" "-lz" "system-os-exec: does not link zlib"
    expect_log_contains "$OS_EXEC_LOG" "-lm" "system-os-exec: links math lib only"
    expect_output "$OS_EXEC_BIN" $'true\nabc\ntrue\n0\ntrue\ntrue\nerr\n7\ntrue\n0\n200000' "system-os-exec: binary output"
else
    record_fail "system-os-exec: build failed"
    sed 's/^/      /' "$OS_EXEC_LOG" | sed -n '1,120p'
fi

SYSTEM_IO_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_io_basic.xr"
SYSTEM_IO_BIN="$WORK/system_io_basic"
SYSTEM_IO_LOG="$WORK/system_io_basic.log"
if build_native "$SYSTEM_IO_SRC" "$SYSTEM_IO_BIN" "$SYSTEM_IO_LOG"; then
    expect_log_contains "$SYSTEM_IO_LOG" "Link command:" "system-io-basic: emitted link command"
    expect_log_not_contains "$SYSTEM_IO_LOG" "-lxray_core" "system-io-basic: does not link xray_core"
    expect_log_not_contains "$SYSTEM_IO_LOG" "-lxray_aot_core" "system-io-basic: does not link AOT core"
    expect_log_not_contains "$SYSTEM_IO_LOG" "-lpthread" "system-io-basic: does not link pthread"
    expect_log_not_contains "$SYSTEM_IO_LOG" "-lz" "system-io-basic: does not link zlib"
    expect_log_not_contains "$SYSTEM_IO_LOG" "-lssl" "system-io-basic: does not link ssl"
    expect_log_not_contains "$SYSTEM_IO_LOG" "-lcrypto" "system-io-basic: does not link crypto"
    expect_log_contains "$SYSTEM_IO_LOG" "-lm" "system-io-basic: links math lib only"
    expect_output "$SYSTEM_IO_BIN" $'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\n5\ntrue\ntrue\n2\nhello\nworld\ntrue\n3\n65\n0\n255\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue' "system-io-basic: binary output"
else
    record_fail "system-io-basic: build failed"
    sed 's/^/      /' "$SYSTEM_IO_LOG" | sed -n '1,120p'
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
    expect_output "$PATH_BIN" $'true\ntrue\ntrue\nxray\n/usr/local/bin\n.gz\n/usr/bin/xray\nbaz\n/foo\n.\n../lib\n../foobar\nbin\n..\nfoo/bar\nfoo/bar/baz\n/usr/local/bin\nfoo/bar\n/bar/baz\n/x\ntrue\nx\n/foo/bar\n/bar/baz\n/var\n/\n/\n/home/user\nfile.txt\nfile\n.txt\nfoo.txt\nfoo\n.txt\n/home/user/file.txt\n/home/user/file.txt\narchive.tar' "core-path: binary output"
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
    expect_output "$ENC_BIN" $'48656c6c6f\n5\n72\n111\nHello\ntrue\ntrue\n0\ntrue\nfalse\ntrue\n2\n6\n0\n1\n4\n65\n0\n66\n0\nAB\n0\n65\nAB\nAB\nA\nfalse\ntrue' "core-encoding: binary output"
else
    record_fail "core-encoding: build failed"
    sed 's/^/      /' "$ENC_LOG" | sed -n '1,120p'
fi

LOG_CONST_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_log_constants.xr"
LOG_CONST_BIN="$WORK/core_log_constants"
LOG_CONST_LOG="$WORK/core_log_constants.log"
if build_native "$LOG_CONST_SRC" "$LOG_CONST_BIN" "$LOG_CONST_LOG"; then
    expect_log_contains "$LOG_CONST_LOG" "Link command:" "core-log-constants: emitted link command"
    expect_log_not_contains "$LOG_CONST_LOG" "-lxray_core" "core-log-constants: does not link xray_core"
    expect_log_not_contains "$LOG_CONST_LOG" "-lxray_aot_core" "core-log-constants: does not link AOT core"
    expect_log_not_contains "$LOG_CONST_LOG" "-lpthread" "core-log-constants: does not link pthread"
    expect_log_not_contains "$LOG_CONST_LOG" "-lz" "core-log-constants: does not link zlib"
    expect_log_not_contains "$LOG_CONST_LOG" "-lssl" "core-log-constants: does not link ssl"
    expect_log_not_contains "$LOG_CONST_LOG" "-lcrypto" "core-log-constants: does not link crypto"
    expect_log_contains "$LOG_CONST_LOG" "-lm" "core-log-constants: links math lib only"
    expect_output "$LOG_CONST_BIN" $'10\n20\n30\n40\n50\ntrue\ntrue' "core-log-constants: binary output"
else
    record_fail "core-log-constants: build failed"
    sed 's/^/      /' "$LOG_CONST_LOG" | sed -n '1,120p'
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

URL_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_url.xr"
URL_BIN="$WORK/core_url"
URL_LOG="$WORK/core_url.log"
if build_native "$URL_SRC" "$URL_BIN" "$URL_LOG"; then
    expect_log_contains "$URL_LOG" "Link command:" "core-url: emitted link command"
    expect_log_not_contains "$URL_LOG" "-lxray_core" "core-url: does not link xray_core"
    expect_log_not_contains "$URL_LOG" "-lpthread" "core-url: does not link pthread"
    expect_log_not_contains "$URL_LOG" "-lz" "core-url: does not link zlib"
    expect_log_not_contains "$URL_LOG" "-lssl" "core-url: does not link ssl"
    expect_log_not_contains "$URL_LOG" "-lcrypto" "core-url: does not link crypto"
    expect_log_contains "$URL_LOG" "-lm" "core-url: links math lib only"
    expect_output "$URL_BIN" $'hello%20%E4%B8%96%E7%95%8C%21\nhello 世界!\na+b%2Bc\na b+c\nhttps:\nexample.com\n8080\n/path\n?q=1\n#top\nexample.com:8080\nhttps://example.com:8080\nhttps://example.com:8080/path?q=1#top\nhello world\na&b\nmsg=hello+world&key=a%26b\nhttps://example.com/a/b/d.html\n/api/v1/users' "core-url: binary output"
else
    record_fail "core-url: build failed"
    sed 's/^/      /' "$URL_LOG" | sed -n '1,120p'
fi

COMPRESS_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_compress.xr"
COMPRESS_BIN="$WORK/core_compress"
COMPRESS_LOG="$WORK/core_compress.log"
if build_native "$COMPRESS_SRC" "$COMPRESS_BIN" "$COMPRESS_LOG"; then
    expect_log_contains "$COMPRESS_LOG" "Link command:" "core-compress: emitted link command"
    expect_log_not_contains "$COMPRESS_LOG" "-lxray_core" "core-compress: does not link xray_core"
    expect_log_not_contains "$COMPRESS_LOG" "-lpthread" "core-compress: does not link pthread"
    expect_log_not_contains "$COMPRESS_LOG" "-lz" "core-compress: does not link zlib"
    expect_log_not_contains "$COMPRESS_LOG" "-lssl" "core-compress: does not link ssl"
    expect_log_not_contains "$COMPRESS_LOG" "-lcrypto" "core-compress: does not link crypto"
    expect_log_contains "$COMPRESS_LOG" "-lxray_aot_core" "core-compress: links AOT core stdlib only"
    expect_log_contains "$COMPRESS_LOG" "-lm" "core-compress: links math lib only"
    expect_output "$COMPRESS_BIN" $'3421780262\n0\n4157704578\n300286872\n1\n93061621\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\nfalse\nfalse\ntrue\ntrue\ntrue' "core-compress: binary output"
else
    record_fail "core-compress: build failed"
    sed 's/^/      /' "$COMPRESS_LOG" | sed -n '1,120p'
fi

CRYPTO_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_crypto.xr"
CRYPTO_BIN="$WORK/core_crypto"
CRYPTO_LOG="$WORK/core_crypto.log"
if build_native "$CRYPTO_SRC" "$CRYPTO_BIN" "$CRYPTO_LOG"; then
    expect_log_contains "$CRYPTO_LOG" "Link command:" "core-crypto: emitted link command"
    expect_log_not_contains "$CRYPTO_LOG" "-lxray_core" "core-crypto: does not link xray_core"
    expect_log_not_contains "$CRYPTO_LOG" "-lpthread" "core-crypto: does not link pthread"
    expect_log_not_contains "$CRYPTO_LOG" "-lz" "core-crypto: does not link zlib"
    expect_log_not_contains "$CRYPTO_LOG" "-lssl" "core-crypto: does not link ssl"
    expect_log_not_contains "$CRYPTO_LOG" "-lcrypto" "core-crypto: does not link crypto"
    expect_log_contains "$CRYPTO_LOG" "-lxray_aot_core" "core-crypto: links AOT core stdlib only"
    expect_log_contains "$CRYPTO_LOG" "-lm" "core-crypto: links math lib only"
    expect_output "$CRYPTO_BIN" $'true\nfalse\nfalse\ntrue\ntrue\nfalse\ntrue\ntrue\ntrue\ntrue\ntrue\n900150983cd24fb0d6963f7d28e17f72\na9993e364706816aba3e25717850c26c9cd0d89d\nba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\nddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f\n9d5c73ef85594d34ec4438b7c97e51d8\n104152c5bfdca07bc633eebd46199f0255c9f49d\n5031fe3d989c6d1537a013fa6e739da23463fdaec3b70137d828e36ace221bd0\n3c5953a18f7303ec653ba170ae334fafa08e3846f2efe317b87efce82376253cb52a8c31ddcde5a3a2eee183c2b34cb91f85e64ddbc325f7692b199473579c58\ntrue' "core-crypto: binary output"
else
    record_fail "core-crypto: build failed"
    sed 's/^/      /' "$CRYPTO_LOG" | sed -n '1,120p'
fi

REGEX_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_regex.xr"
REGEX_BIN="$WORK/core_regex"
REGEX_LOG="$WORK/core_regex.log"
if build_native "$REGEX_SRC" "$REGEX_BIN" "$REGEX_LOG"; then
    expect_log_contains "$REGEX_LOG" "Link command:" "core-regex: emitted link command"
    expect_log_not_contains "$REGEX_LOG" "-lxray_core" "core-regex: does not link xray_core"
    expect_log_not_contains "$REGEX_LOG" "-lpthread" "core-regex: does not link pthread"
    expect_log_not_contains "$REGEX_LOG" "-lz" "core-regex: does not link zlib"
    expect_log_not_contains "$REGEX_LOG" "-lssl" "core-regex: does not link ssl"
    expect_log_not_contains "$REGEX_LOG" "-lcrypto" "core-regex: does not link crypto"
    expect_log_contains "$REGEX_LOG" "-lxray_aot_core" "core-regex: links AOT core stdlib only"
    expect_log_contains "$REGEX_LOG" "-lm" "core-regex: links math lib only"
    expect_output "$REGEX_BIN" $'a\\.b\\*c\\?\nplain\nx\\+y\n\\[abc\\]\ntrue\nfalse\ntrue\nfalse\n3\n1\ntrue\n2\nabc123\nabc\n123\ntrue\nabc123\n0\n6\n3\nabc123\nabc\n123\ntrue\nabc123\n0\n6\nabc\ntrue\n2\nabc123\n0\n123\nxyz456\n7\n1\nabc123\n0\n0\naN b22 c333\naN bN cN\n123-abc xyz\nabc\n3\na\nb\nc\n2\na\nb, c' "core-regex: binary output"
else
    record_fail "core-regex: build failed"
    sed 's/^/      /' "$REGEX_LOG" | sed -n '1,120p'
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
