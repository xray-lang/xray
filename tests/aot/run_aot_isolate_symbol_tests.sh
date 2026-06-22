#!/bin/bash
# AOT isolate/VM symbol gate for tasks 132/M0 + 133/I0.
#
# Freestanding and runtime-backed AOT samples are hard failures if they link
# xray_core or carry public isolate/VM/compiler/module-loader symbols.
#
# Environment:
#   XRAY_AOT_ISOLATE_JOBS  parallel workers (default: auto, capped by
#                          XRAY_AOT_ISOLATE_MAX_AUTO_JOBS=5;
#                          XRAY_TEST_JOBS also works)
#   XRAY_AOT_ISOLATE_BUILD_CACHE_DIR
#                          persistent AOT object cache for cold symbol-gate
#                          builds (default: build/.xray-test-cache/aot-isolate-objects)
#   XRAY_AOT_ISOLATE_BIN_CACHE_DIR
#                          persistent native binary cache for warm symbol-gate
#                          reruns (default: build/.xray-test-cache/aot-isolate-bin/<toolchain-key>)
#
# Pure tiny AOT cases and runtime-time-sleep are also hard size gates. They keep
# optimized paths from silently drifting back toward VM/toolchain-shaped
# binaries while still allowing crypto/regex-style core archives their own caps.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$PROJECT_DIR/tests/test_common.sh"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_aot_isolate_symbols.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
BUILD_CACHE="${XRAY_AOT_ISOLATE_BUILD_CACHE_DIR:-$(xray_test_shared_cache_dir "$PROJECT_DIR" "aot-isolate-objects")}"
BIN_CACHE="${XRAY_AOT_ISOLATE_BIN_CACHE_DIR:-$(xray_test_stable_cache_dir "$PROJECT_DIR" "aot-isolate-bin" "$XRAY")}"
PASS=0
FAIL=0
REQUESTED_JOBS="${XRAY_AOT_ISOLATE_JOBS:-${XRAY_TEST_JOBS:-auto}}"
JOBS=1

# Keep this intentionally broad. Pure AOT binaries should not expose any public
# isolate API, VM init/cleanup, source compiler entry, analyzer, or module loader
# symbol. Darwin nm prefixes C symbols with '_', so every alternative allows it.
FORBIDDEN_SYMBOL_RE='(^|[^[:alnum:]_])_?(xray_isolate_|xr_vm_|xr_parse|xr_compile|xanalyzer_|xr_load_module_)'
EAGER_SCRIPT_BUILTIN_SYMBOL_RE='(^|[^[:alnum:]_])_?(xr_string_intern_core|xr_string_value)([^[:alnum:]_]|$)'
PURE_TINY_AOT_MAX_BYTES=70000
PURE_CRYPTO_AOT_MAX_BYTES=80000
RUNTIME_TIME_SLEEP_MAX_BYTES=200000

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

is_uint() {
    case "$1" in
        ""|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

detect_cores() {
    local cores=""
    if command -v getconf >/dev/null 2>&1; then
        cores="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
    fi
    if ! is_uint "$cores" && command -v sysctl >/dev/null 2>&1; then
        cores="$(sysctl -n hw.logicalcpu 2>/dev/null || true)"
    fi
    if ! is_uint "$cores" || [ "$cores" -lt 1 ]; then
        cores=1
    fi
    printf '%s\n' "$cores"
}

configure_jobs() {
    local requested="$1" max_auto
    case "$requested" in
        ""|auto)
            JOBS="$(detect_cores)"
            max_auto="${XRAY_AOT_ISOLATE_MAX_AUTO_JOBS:-5}"
            if is_uint "$max_auto" && [ "$max_auto" -gt 0 ] && [ "$JOBS" -gt "$max_auto" ]; then
                JOBS="$max_auto"
            fi
            ;;
        *)
            if is_uint "$requested" && [ "$requested" -gt 0 ]; then
                JOBS="$requested"
            else
                JOBS=1
            fi
            ;;
    esac
}

print_log_tail() {
    local log="$1"
    if [ -f "$log" ]; then
        sed 's/^/      /' "$log" | sed -n '1,120p'
    fi
}

build_native() {
    local src="$1"
    local out="$2"
    local log="$3"
    local rel safe key bin_dir cached tmp

    rel="${src#"$PROJECT_DIR"/}"
    safe="$(printf '%s' "${rel%.xr}" | sed 's#[^A-Za-z0-9_.-]#_#g')"
    key="$(xray_test_case_dir_key "$src")"
    bin_dir="$BIN_CACHE/$safe-$key"
    cached="$bin_dir/aot"
    tmp="$bin_dir/aot.$$"

    if [ ! -x "$cached" ]; then
        mkdir -p "$bin_dir" || return 1
        if ! xray_test_lock_dir "$bin_dir.lock"; then
            printf 'cannot lock binary cache: %s\n' "$bin_dir.lock" >"$log"
            return 1
        fi
        if [ ! -x "$cached" ]; then
            rm -f "$tmp"
            if "$XRAY" build --native --dump-link-command --cache-dir "$BUILD_CACHE" -o "$tmp" "$src" \
                    >"$log" 2>&1; then
                mv "$tmp" "$cached"
            else
                rm -f "$tmp"
                xray_test_unlock_dir "$bin_dir.lock"
                return 1
            fi
        else
            printf 'cached: %s\n' "$cached" >"$log"
        fi
        xray_test_unlock_dir "$bin_dir.lock"
    else
        printf 'cached: %s\n' "$cached" >"$log"
    fi

    cp "$cached" "$out"
    chmod +x "$out"
}

binary_size() {
    local bin="$1"
    if stat -f%z "$bin" >/dev/null 2>&1; then
        stat -f%z "$bin"
    else
        stat -c%s "$bin"
    fi
}

dump_symbols() {
    local bin="$1"
    local symbols="$2"
    case "$(uname -s 2>/dev/null)" in
        Darwin)
            nm -U "$bin" >"$symbols" 2>"$symbols.err"
            ;;
        *)
            nm "$bin" >"$symbols" 2>"$symbols.err"
            ;;
    esac
}

expect_log_not_contains() {
    local log="$1"
    local needle="$2"
    local name="$3"
    if grep -Fq -- "$needle" "$log"; then
        record_fail "$name"
        print_log_tail "$log"
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

check_binary_max_size() {
    local actual="$1"
    local max="$2"
    local name="$3"

    if [ "$actual" -le "$max" ]; then
        record_pass "$name: size ${actual} <= ${max} bytes"
    else
        record_fail "$name: size ${actual} > ${max} bytes"
    fi
}

check_no_forbidden_symbols() {
    local bin="$1"
    local slug="$2"
    local name="$3"
    local symbols="$WORK/$slug.symbols"
    local hits="$WORK/$slug.forbidden"

    if ! dump_symbols "$bin" "$symbols"; then
        record_fail "$name: nm failed"
        print_log_tail "$symbols.err"
        return
    fi

    if grep -E "$FORBIDDEN_SYMBOL_RE" "$symbols" >"$hits"; then
        record_fail "$name: forbidden isolate/VM/compiler symbols present"
        sed 's/^/      /' "$hits" | sed -n '1,60p'
    else
        record_pass "$name: no forbidden isolate/VM/compiler symbols"
    fi
}

check_no_eager_script_builtin_symbols() {
    local bin="$1"
    local slug="$2"
    local name="$3"
    local symbols="$WORK/$slug.symbols"
    local hits="$WORK/$slug.eager-script-builtin"

    if ! dump_symbols "$bin" "$symbols"; then
        record_fail "$name: nm failed"
        print_log_tail "$symbols.err"
        return
    fi

    if grep -E "$EAGER_SCRIPT_BUILTIN_SYMBOL_RE" "$symbols" >"$hits"; then
        record_fail "$name: eager script builtin string symbols present"
        sed 's/^/      /' "$hits" | sed -n '1,60p'
    else
        record_pass "$name: no eager script builtin string symbols"
    fi
}

run_freestanding_case() {
    local slug="$1"
    local name="$2"
    local src="$3"
    local want_output="$4"
    local max_size="${5:-}"
    local bin="$WORK/$slug"
    local log="$WORK/$slug.log"
    local size

    echo ""
    echo "-- $name"
    if build_native "$src" "$bin" "$log"; then
        size="$(binary_size "$bin")"
        echo "  size: $size bytes"
        if [ -n "$max_size" ]; then
            check_binary_max_size "$size" "$max_size" "$name"
        fi
        expect_log_not_contains "$log" "-lxray_core" "$name: does not link xray_core"
        check_no_forbidden_symbols "$bin" "$slug" "$name"
        if [ -n "$want_output" ]; then
            expect_output "$bin" "$want_output" "$name: binary output"
        fi
    else
        record_fail "$name: build failed"
        print_log_tail "$log"
    fi
}

run_runtime_case() {
    local slug="$1"
    local name="$2"
    local src="$3"
    local want_output="$4"
    local symbol_profile="${5:-}"
    local max_size="${6:-}"
    local bin="$WORK/$slug"
    local log="$WORK/$slug.log"
    local size

    echo ""
    echo "-- $name"
    if build_native "$src" "$bin" "$log"; then
        size="$(binary_size "$bin")"
        echo "  size: $size bytes"
        if [ -n "$max_size" ]; then
            check_binary_max_size "$size" "$max_size" "$name"
        fi
        expect_log_not_contains "$log" "-lxray_core" "$name: does not link xray_core"
        check_no_forbidden_symbols "$bin" "$slug" "$name"
        if [ "$symbol_profile" = "no_eager_script_builtins" ]; then
            check_no_eager_script_builtin_symbols "$bin" "$slug" "$name"
        fi
        if [ -n "$want_output" ]; then
            expect_output "$bin" "$want_output" "$name: binary output"
        fi
    else
        record_fail "$name: build failed"
        print_log_tail "$log"
    fi
}

run_case_by_id() {
    case "$1" in
        core_math_single_symbol)
            run_freestanding_case \
                "core_math_single_symbol" \
                "core-math-single-symbol" \
                "$PROJECT_DIR/tests/aot/filetests/link/core_math_single_symbol.xr" \
                "9.0" \
                "$PURE_TINY_AOT_MAX_BYTES"
            ;;
        system_time_queries)
            run_freestanding_case \
                "system_time_queries" \
                "system-time-queries" \
                "$PROJECT_DIR/tests/aot/filetests/link/system_time_queries.xr" \
                "" \
                "$PURE_TINY_AOT_MAX_BYTES"
            ;;
        core_crypto)
            run_freestanding_case \
                "core_crypto" \
                "core-crypto" \
                "$PROJECT_DIR/tests/aot/filetests/link/core_crypto.xr" \
                "" \
                "$PURE_CRYPTO_AOT_MAX_BYTES"
            ;;
        runtime_time_sleep)
            run_runtime_case \
                "runtime_time_sleep" \
                "runtime-time-sleep" \
                "$PROJECT_DIR/tests/aot/filetests/link/runtime_time.xr" \
                "7" \
                "no_eager_script_builtins" \
                "$RUNTIME_TIME_SLEEP_MAX_BYTES"
            ;;
        runtime_coro_minimal)
            run_runtime_case \
                "runtime_coro_minimal" \
                "runtime-coro-minimal" \
                "$PROJECT_DIR/tests/aot/coro/spawn_await_yield.xr" \
                "42"
            ;;
    esac
}

check_runtime_archive() {
    local dir archive
    dir="$(cd "$(dirname "$XRAY")" && pwd)"
    archive="$dir/libxray_rt_coro.a"

    echo ""
    echo "-- runtime-archive/xray_rt_coro"
    if [ ! -f "$archive" ]; then
        record_fail "runtime-archive/xray_rt_coro: archive missing at $archive"
        return
    fi
    check_no_forbidden_symbols "$archive" "xray_rt_coro_archive" "runtime-archive/xray_rt_coro"
}

record_parallel_result() {
    local log="$1"
    cat "$log"
    PASS=$((PASS + $(grep -c '^  PASS:' "$log" || true)))
    FAIL=$((FAIL + $(grep -c '^  FAIL:' "$log" || true)))
}

run_cases_parallel() {
    local jobs="$1"
    local idx=0 case_id log sem token i p pids logs

    mkdir -p "$WORK/logs"
    sem="$WORK/cases.sem"
    if ! mkfifo "$sem"; then
        echo "error: cannot create worker semaphore" >&2
        FAIL=$((FAIL + 1))
        return 1
    fi
    exec 9<>"$sem"
    rm -f "$sem"

    i=0
    while [ "$i" -lt "$jobs" ]; do
        printf '.' >&9
        i=$((i + 1))
    done

    pids=""
    logs=""
    for case_id in \
        core_math_single_symbol \
        system_time_queries \
        core_crypto \
        runtime_time_sleep \
        runtime_coro_minimal
    do
        IFS= read -r -n 1 token <&9
        log="$WORK/logs/${idx}_${case_id}.log"
        (
            run_case_by_id "$case_id" >"$log" 2>&1
            printf '.' >&9
        ) &
        pids="$pids $!"
        logs="$logs $log"
        idx=$((idx + 1))
    done

    for p in $pids; do
        wait "$p"
    done
    exec 9>&-
    exec 9<&-

    for log in $logs; do
        record_parallel_result "$log"
    done
}

configure_jobs "$REQUESTED_JOBS"

echo "=== AOT Isolate Symbol Gate ==="
echo "Binary: $XRAY"
echo "Jobs:   $JOBS"
echo "Cache:  $BUILD_CACHE"
echo "BinCache: $BIN_CACHE"
echo ""

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

check_runtime_archive

if [ "$JOBS" -le 1 ]; then
    for case_id in \
        core_math_single_symbol \
        system_time_queries \
        core_crypto \
        runtime_time_sleep \
        runtime_coro_minimal
    do
        run_case_by_id "$case_id"
    done
else
    run_cases_parallel "$JOBS"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
