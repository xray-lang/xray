#!/bin/bash
# AOT per-module incremental object-cache tests (114 separate compilation).
#
# Usage:
#   ./tests/aot/run_aot_incremental_cache.sh [xray_binary]
#
# Verifies that a multi-module native build only recompiles the modules whose
# generated C actually changed, reusing cached objects for the rest. Independent
# scenarios run in parallel by default; each scenario owns its source tree and
# cache so the test still exercises deterministic incremental state.
#
# Environment:
#   XRAY_AOT_TEST_OPT            native C compiler optimization level for this
#                                correctness/cache gate (default: 0)
#   XRAY_AOT_INCREMENTAL_JOBS    parallel scenario workers (default: auto,
#                                capped by XRAY_AOT_INCREMENTAL_MAX_AUTO_JOBS=3;
#                                XRAY_TEST_JOBS also works)
#   XRAY_AOT_FAST_TEST_BUILD     use correctness-test AOT link flags (default:
#                                1); set to 0 to exercise product size flags

set -u

XRAY="${1:-./build/xray}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_aot_incr_cache.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
AOT_OPT_LEVEL="${XRAY_AOT_TEST_OPT:-0}"
REQUESTED_JOBS="${XRAY_AOT_INCREMENTAL_JOBS:-${XRAY_TEST_JOBS:-auto}}"
: "${XRAY_AOT_FAST_TEST_BUILD:=1}"
export XRAY_AOT_FAST_TEST_BUILD
JOBS=1
PASS=0
FAIL=0

trap 'rm -rf "$WORK"' EXIT

echo "=== AOT Incremental Cache Tests ==="
echo "Binary: $XRAY"
echo "Work:   $WORK"
echo "AOT opt: -O$AOT_OPT_LEVEL"

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY"
    exit 1
fi

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
            max_auto="${XRAY_AOT_INCREMENTAL_MAX_AUTO_JOBS:-3}"
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

record_pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

record_fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

show_cache_lines() {
    sed 's/^/      /' "$1" | grep -E 'compiling|cache hit' || true
}

# build_log <cache_dir> <entry_file> <out_binary> <log_file> [extra args...]
# Exercises the explicit --cache-dir flag (precedence over $XRAY_CACHE_DIR).
build_log() {
    local cache="$1" entry="$2" out="$3" log="$4"
    shift 4
    "$XRAY" build --native -O "$AOT_OPT_LEVEL" --verbose --cache-dir "$cache" "$@" \
        -o "$out" "$entry" >"$log" 2>&1
}

require_build() {
    local name="$1" log="$2"
    shift 2
    if "$@"; then
        return 0
    fi
    record_fail "$name: build failed"
    sed 's/^/      /' "$log"
    return 1
}

expect_state() {
    local log="$1" mod="$2" want="$3" name="$4"
    if [ "$want" = "compiling" ]; then
        if grep -q "compiling: $mod " "$log"; then
            record_pass "$name: $mod recompiled"
        else
            record_fail "$name: expected $mod to recompile"
            show_cache_lines "$log"
        fi
    else
        if grep -q "cache hit: $mod " "$log"; then
            record_pass "$name: $mod cache hit"
        else
            record_fail "$name: expected $mod cache hit"
            show_cache_lines "$log"
        fi
    fi
}

expect_output() {
    local bin="$1" want="$2" name="$3" got
    got="$("$bin" 2>/dev/null)"
    if [ "$got" = "$want" ]; then
        record_pass "$name: output '$want'"
    else
        record_fail "$name: output '$got' != '$want'"
    fi
}

run_basic_modules() {
    local dir="$WORK/basic"
    local cache="$dir/.cache"
    local app="$dir/app.xr"
    local lib="$dir/mathlib.xr"

    mkdir -p "$dir"
    echo "--- basic-modules ---"

    cat >"$app" <<'XR_EOF'
import { triple } from "./mathlib"

print(triple(14))
XR_EOF

    cat >"$lib" <<'XR_EOF'
export fn triple(x: int) -> int {
    return x * 3
}
XR_EOF
    require_build "cold" "$dir/log1" \
        build_log "$cache" "$app" "$dir/app1" "$dir/log1" || return 1
    expect_state "$dir/log1" mathlib compiling "cold"
    expect_state "$dir/log1" app compiling "cold"
    expect_output "$dir/app1" "42" "cold"

    cat >"$lib" <<'XR_EOF'
export fn triple(x: int) -> int {
    return x * 5
}
XR_EOF
    require_build "body-change" "$dir/log2" \
        build_log "$cache" "$app" "$dir/app2" "$dir/log2" || return 1
    expect_state "$dir/log2" mathlib compiling "body-change"
    expect_state "$dir/log2" app hit "body-change"
    expect_output "$dir/app2" "70" "body-change"

    cat >"$lib" <<'XR_EOF'
export fn triple(x: int) -> int {
    return x * 5
}

export fn quad(x: int) -> int {
    return x * 4
}
XR_EOF
    require_build "add-export" "$dir/log3" \
        build_log "$cache" "$app" "$dir/app3" "$dir/log3" || return 1
    expect_state "$dir/log3" mathlib compiling "add-export"
    expect_state "$dir/log3" app hit "add-export"
    expect_output "$dir/app3" "70" "add-export"

    cat >"$app" <<'XR_EOF'
import { triple } from "./mathlib"

print(triple(14))
print(triple(2))
XR_EOF
    require_build "entry-only" "$dir/log4" \
        build_log "$cache" "$app" "$dir/app4" "$dir/log4" || return 1
    expect_state "$dir/log4" mathlib hit "entry-only"
    expect_state "$dir/log4" app compiling "entry-only"
    expect_output "$dir/app4" "$(printf '70\n10')" "entry-only"

    require_build "rebuild" "$dir/log5" \
        build_log "$cache" "$app" "$dir/app5" "$dir/log5" --rebuild || return 1
    expect_state "$dir/log5" mathlib compiling "rebuild"
    expect_state "$dir/log5" app compiling "rebuild"
    expect_output "$dir/app5" "$(printf '70\n10')" "rebuild"
}

run_class_symbols() {
    local dir="$WORK/class"
    local cache="$dir/.cache"
    local app="$dir/capp.xr"
    local lib="$dir/shape.xr"

    mkdir -p "$dir"
    echo "--- class-symbols ---"

    cat >"$app" <<'XR_EOF'
import { Box } from "./shape"

let b = new Box(4)
print(b.area())
XR_EOF
    cat >"$lib" <<'XR_EOF'
export class Box {
    side: int
    constructor(s: int) {
        this.side = s
    }
    area() -> int {
        return this.side * this.side
    }
}
XR_EOF
    require_build "class-cold" "$dir/log1" \
        build_log "$cache" "$app" "$dir/capp1" "$dir/log1" || return 1
    expect_state "$dir/log1" shape compiling "class-cold"
    expect_state "$dir/log1" capp compiling "class-cold"
    expect_output "$dir/capp1" "16" "class-cold"

    cat >"$lib" <<'XR_EOF'
export class Box {
    side: int
    constructor(s: int) {
        this.side = s
    }
    area() -> int {
        return this.side * this.side
    }
    perimeter() -> int {
        return this.side * 4
    }
}
XR_EOF
    require_build "class-add-method" "$dir/log2" \
        build_log "$cache" "$app" "$dir/capp2" "$dir/log2" || return 1
    expect_state "$dir/log2" shape compiling "class-add-method"
    expect_state "$dir/log2" capp hit "class-add-method"
    expect_output "$dir/capp2" "16" "class-add-method"
}

run_lto_cache() {
    local dir="$WORK/lto"
    local cache="$dir/.cache"
    local app="$dir/app.xr"
    local lib="$dir/mathlib.xr"

    mkdir -p "$dir"
    echo "--- lto-cache ---"

    cat >"$app" <<'XR_EOF'
import { triple } from "./mathlib"

print(triple(14))
print(triple(2))
XR_EOF
    cat >"$lib" <<'XR_EOF'
export fn triple(x: int) -> int {
    return x * 5
}

export fn quad(x: int) -> int {
    return x * 4
}
XR_EOF
    require_build "lto-cold" "$dir/log1" \
        build_log "$cache" "$app" "$dir/lapp1" "$dir/log1" --lto || return 1
    expect_state "$dir/log1" mathlib compiling "lto-cold"
    expect_output "$dir/lapp1" "$(printf '70\n10')" "lto-cold"

    require_build "lto-warm" "$dir/log2" \
        build_log "$cache" "$app" "$dir/lapp2" "$dir/log2" --lto || return 1
    expect_state "$dir/log2" mathlib hit "lto-warm"
    expect_state "$dir/log2" app hit "lto-warm"
    expect_output "$dir/lapp2" "$(printf '70\n10')" "lto-warm"
}

run_group_by_id() {
    case "$1" in
        basic) run_basic_modules ;;
        class) run_class_symbols ;;
        lto) run_lto_cache ;;
        *) record_fail "unknown scenario: $1"; return 1 ;;
    esac
}

record_group_log() {
    local log="$1"
    cat "$log"
    PASS=$((PASS + $(grep -c '^  PASS:' "$log" || true)))
    FAIL=$((FAIL + $(grep -c '^  FAIL:' "$log" || true)))
}

run_groups_parallel() {
    local jobs="$1"
    local idx=0 scenario log sem token i p pids logs

    mkdir -p "$WORK/logs"
    sem="$WORK/groups.sem"
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
    for scenario in basic class lto; do
        IFS= read -r -n 1 token <&9
        log="$WORK/logs/${idx}_${scenario}.log"
        (
            run_group_by_id "$scenario" >"$log" 2>&1
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
        record_group_log "$log"
    done
}

configure_jobs "$REQUESTED_JOBS"
echo "Jobs:   $JOBS"
echo ""

if [ "$JOBS" -le 1 ]; then
    for scenario in basic class lto; do
        run_group_by_id "$scenario"
    done
else
    run_groups_parallel "$JOBS"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
