#!/bin/bash
# AOT filetest scaffold for XAOT plan/dump assertions.
#
# Usage:
#   tests/aot/run_aot_filetests.sh [xray_binary]
#   tests/aot/run_aot_filetests.sh --mode rep [xray_binary]
#
# Environment:
#   XRAY_AOT_FILETEST_JOBS  parallel workers (default: auto, capped by
#                           XRAY_AOT_FILETEST_MAX_AUTO_JOBS=4;
#                           XRAY_TEST_JOBS also works)
#   XRAY_AOT_FILETEST_CACHE_DIR
#                           persistent dump/generated-C cache for warm reruns
#   XRAY_TEST_DISABLE_RUN_CACHE=1
#                           bypass persistent dump/generated-C cache

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$PROJECT_DIR/tests/test_common.sh"
FILETEST_DIR="$SCRIPT_DIR/filetests"
ALL_MODES="rep layout abi boundary container link cgen"

MODE="all"
XRAY="${XRAY_BIN:-}"
VERBOSE=0
KEEP_TMP=0
PASS=0
FAIL=0
SKIP=0
REQUESTED_JOBS="${XRAY_AOT_FILETEST_JOBS:-${XRAY_TEST_JOBS:-auto}}"
JOBS=1

usage() {
    printf '%s\n' \
        "Usage: $0 [options] [xray_binary]" \
        "" \
        "Options:" \
        "  --mode MODE      rep, layout, abi, boundary, container, link, cgen, or all (default: all)" \
        "  --xray PATH      xray binary path (also supports XRAY_BIN or XRAY_BUILD_DIR)" \
        "  -v, --verbose    Show dump excerpts for failures" \
        "  --keep-tmp       Keep temporary dump/build files" \
        "  -h, --help       Show this help"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --mode)
            shift
            if [ "$#" -eq 0 ]; then
                echo "error: --mode requires a value" >&2
                usage >&2
                exit 2
            fi
            MODE="$1"
            ;;
        --mode=*)
            MODE="${1#--mode=}"
            ;;
        --xray)
            shift
            if [ "$#" -eq 0 ]; then
                echo "error: --xray requires a path" >&2
                usage >&2
                exit 2
            fi
            XRAY="$1"
            ;;
        --xray=*)
            XRAY="${1#--xray=}"
            ;;
        -v|--verbose)
            VERBOSE=1
            ;;
        --keep-tmp)
            KEEP_TMP=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            if [ -n "$XRAY" ]; then
                echo "error: unexpected positional argument: $1" >&2
                usage >&2
                exit 2
            fi
            XRAY="$1"
            ;;
    esac
    shift
done

case "$MODE" in
    all)
        SELECTED_MODES="$ALL_MODES"
        ;;
    rep|layout|abi|boundary|container|link|cgen)
        SELECTED_MODES="$MODE"
        ;;
    *)
        echo "error: unsupported mode '$MODE' (expected rep/layout/abi/boundary/container/link/cgen/all)" >&2
        exit 2
        ;;
esac

if [ -z "$XRAY" ]; then
    if [ -n "${XRAY_BUILD_DIR:-}" ]; then
        XRAY="${XRAY_BUILD_DIR}/xray"
    else
        XRAY="${PROJECT_DIR}/build/xray"
    fi
fi

FILETEST_CACHE="${XRAY_AOT_FILETEST_CACHE_DIR:-$(xray_test_stable_cache_dir "$PROJECT_DIR" "aot-filetest-dumps" "$XRAY")}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_aot_filetests.XXXXXX")" || {
    echo "error: cannot create temporary directory" >&2
    exit 1
}
cleanup() {
    if [ "$KEEP_TMP" -eq 0 ]; then
        rm -rf "$WORK"
    else
        echo "Kept temp files: $WORK"
    fi
}
trap cleanup EXIT

rel_path() {
    case "$1" in
        "$PROJECT_DIR"/*) printf '%s' "${1#"$PROJECT_DIR"/}" ;;
        *) printf '%s' "$1" ;;
    esac
}

is_collected_test() {
    case "$(basename "$1")" in
        _*) return 1 ;;
        *.xr) return 0 ;;
        *) return 1 ;;
    esac
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
            max_auto="${XRAY_AOT_FILETEST_MAX_AUTO_JOBS:-4}"
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

count_selected_tests() {
    local count=0
    local mode dir f
    for mode in $SELECTED_MODES; do
        dir="$FILETEST_DIR/$mode"
        [ -d "$dir" ] || continue
        for f in "$dir"/*.xr; do
            [ -f "$f" ] || continue
            is_collected_test "$f" || continue
            count=$((count + 1))
        done
    done
    printf '%s' "$count"
}

first_nonempty_line() {
    sed -n '/[^[:space:]]/{p;q;}' "$1" 2>/dev/null
}

safe_case_name() {
    printf '%s' "${1%.xr}" | sed 's#[^A-Za-z0-9_.-]#_#g'
}

expect_args() {
    sed -n 's/^args=//p' "$1" 2>/dev/null | sed -n '1p'
}

expect_status() {
    local status
    status="$(sed -n 's/^status=//p' "$1" 2>/dev/null | sed -n '1p')"
    if [ -z "$status" ]; then
        printf '%s\n' "pass"
    else
        printf '%s\n' "$status"
    fi
}

probe_dump_support() {
    local probe_src="$WORK/probe.xr"
    local probe_c="$WORK/probe.c"
    printf 'fn answer() -> int {\n    return 42\n}\nprint(answer())\n' > "$probe_src"
    "$XRAY" build --native --dump-xaot-plan --dump-link-manifest -c -o "$probe_c" "$probe_src" \
        > "$WORK/probe.log" 2>&1
}

show_excerpt() {
    if [ "$VERBOSE" -eq 1 ] && [ -f "$1" ]; then
        sed -n '1,40s/^/    /p' "$1"
    fi
}

check_expect() {
    local expect="$1"
    local dump="$2"
    local c_out="$3"
    local line raw key value
    local line_no=0
    local checks=0

    if [ ! -f "$expect" ]; then
        echo "SKIP (missing expect)"
        SKIP=$((SKIP + 1))
        return 0
    fi

    while IFS= read -r raw || [ -n "$raw" ]; do
        line_no=$((line_no + 1))
        line="${raw%$'\r'}"
        case "$line" in
            ""|\#*) continue ;;
        esac

        key="${line%%=*}"
        value="${line#*=}"
        if [ "$key" = "$line" ]; then
            echo "FAIL (bad expect directive: $(rel_path "$expect"):$line_no)"
            echo "    expected args=, status=, contains=, not_contains=, regex=, not_regex=, c_contains=, c_regex=, or skip="
            FAIL=$((FAIL + 1))
            return 1
        fi

        case "$key" in
            args)
                continue
                ;;
            status)
                continue
                ;;
            skip)
                echo "SKIP ($value)"
                SKIP=$((SKIP + 1))
                return 0
                ;;
            contains)
                checks=$((checks + 1))
                if ! grep -Fq -- "$value" "$dump"; then
                    echo "FAIL (missing: $value)"
                    show_excerpt "$dump"
                    FAIL=$((FAIL + 1))
                    return 1
                fi
                ;;
            not_contains)
                checks=$((checks + 1))
                if grep -Fq -- "$value" "$dump"; then
                    echo "FAIL (unexpected: $value)"
                    show_excerpt "$dump"
                    FAIL=$((FAIL + 1))
                    return 1
                fi
                ;;
            regex)
                checks=$((checks + 1))
                if ! grep -Eq -- "$value" "$dump"; then
                    echo "FAIL (missing regex: $value)"
                    show_excerpt "$dump"
                    FAIL=$((FAIL + 1))
                    return 1
                fi
                ;;
            not_regex)
                checks=$((checks + 1))
                if grep -Eq -- "$value" "$dump"; then
                    echo "FAIL (unexpected regex: $value)"
                    show_excerpt "$dump"
                    FAIL=$((FAIL + 1))
                    return 1
                fi
                ;;
            c_contains)
                checks=$((checks + 1))
                if ! grep -Fq -- "$value" "$c_out"; then
                    echo "FAIL (missing generated C: $value)"
                    show_excerpt "$c_out"
                    FAIL=$((FAIL + 1))
                    return 1
                fi
                ;;
            c_not_contains)
                checks=$((checks + 1))
                if grep -Fq -- "$value" "$c_out"; then
                    echo "FAIL (unexpected generated C: $value)"
                    show_excerpt "$c_out"
                    FAIL=$((FAIL + 1))
                    return 1
                fi
                ;;
            c_regex)
                checks=$((checks + 1))
                if ! grep -Eq -- "$value" "$c_out"; then
                    echo "FAIL (missing generated C regex: $value)"
                    show_excerpt "$c_out"
                    FAIL=$((FAIL + 1))
                    return 1
                fi
                ;;
            c_not_regex)
                checks=$((checks + 1))
                if grep -Eq -- "$value" "$c_out"; then
                    echo "FAIL (unexpected generated C regex: $value)"
                    show_excerpt "$c_out"
                    FAIL=$((FAIL + 1))
                    return 1
                fi
                ;;
            *)
                echo "FAIL (unknown expect directive: $(rel_path "$expect"):$line_no)"
                echo "    $key"
                FAIL=$((FAIL + 1))
                return 1
                ;;
        esac
    done < "$expect"

    if [ "$checks" -eq 0 ]; then
        echo "SKIP (expect pending)"
        SKIP=$((SKIP + 1))
        return 0
    fi

    echo "PASS"
    PASS=$((PASS + 1))
    return 0
}

run_dump_command() {
    local out_c="$1"
    local out_dump="$2"
    local xr_file="$3"
    local extra_args="$4"
    local build_args=(build --native --dump-xaot-plan --dump-link-manifest)

    if [ -n "$extra_args" ]; then
        local expect_build_args=($extra_args)
        build_args+=("${expect_build_args[@]}")
    fi
    build_args+=(-c -o "$out_c" "$xr_file")

    "$XRAY" "${build_args[@]}" >"$out_dump" 2>&1
}

run_one() {
    local mode="$1"
    local xr_file="$2"
    local case_key="${3:-}"
    local test_name base dump c_out expect extra_args expected_status rel safe args_key
    local cache_dir cached_dump cached_c tmp_dump tmp_c

    base="$(basename "$xr_file" .xr)"
    test_name="$(rel_path "$xr_file")"
    dump="$WORK/${mode}_${base}.dump"
    c_out="$WORK/${mode}_${base}.c"
    expect="${xr_file%.xr}.expect"
    extra_args="$(expect_args "$expect")"
    expected_status="$(expect_status "$expect")"

    printf '  %-9s %-48s' "$mode" "$test_name"

    case "$expected_status" in
        pass|fail)
            ;;
        *)
            echo "FAIL (unknown expected status: $expected_status)"
            FAIL=$((FAIL + 1))
            return 1
            ;;
    esac

    if [ "$expected_status" = "fail" ]; then
        rm -f "$dump" "$c_out"
        if run_dump_command "$c_out" "$dump" "$xr_file" "$extra_args"; then
            echo "FAIL (dump command unexpectedly succeeded)"
            show_excerpt "$dump"
            FAIL=$((FAIL + 1))
            return 1
        fi
        check_expect "$expect" "$dump" "$c_out"
        return $?
    fi

    rel="$(rel_path "$xr_file")"
    safe="$(safe_case_name "$rel")"
    if [ -z "$case_key" ]; then
        case_key="$(xray_test_case_dir_key "$xr_file")"
    fi
    args_key="$(xray_test_string_key "$extra_args")"
    cache_dir="$FILETEST_CACHE/$mode/$safe-$case_key-$args_key"
    cached_dump="$cache_dir/dump"
    cached_c="$cache_dir/generated.c"
    tmp_dump="$cache_dir/dump.$$"
    tmp_c="$cache_dir/generated.c.$$"

    if [ "${XRAY_TEST_DISABLE_RUN_CACHE:-0}" != "1" ] &&
            [ -f "$cached_dump" ] && [ -f "$cached_c" ]; then
        cp "$cached_dump" "$dump"
        cp "$cached_c" "$c_out"
    else
        if [ "${XRAY_TEST_DISABLE_RUN_CACHE:-0}" != "1" ]; then
            mkdir -p "$cache_dir" || {
                echo "FAIL (cannot create cache dir)"
                FAIL=$((FAIL + 1))
                return 1
            }
            if ! xray_test_lock_dir "$cache_dir.lock"; then
                echo "FAIL (cannot lock cache dir)"
                FAIL=$((FAIL + 1))
                return 1
            fi
            if [ -f "$cached_dump" ] && [ -f "$cached_c" ]; then
                cp "$cached_dump" "$dump"
                cp "$cached_c" "$c_out"
                xray_test_unlock_dir "$cache_dir.lock"
            else
                rm -f "$tmp_dump" "$tmp_c"
                if run_dump_command "$tmp_c" "$tmp_dump" "$xr_file" "$extra_args"; then
                    mv "$tmp_dump" "$cached_dump"
                    mv "$tmp_c" "$cached_c"
                    cp "$cached_dump" "$dump"
                    cp "$cached_c" "$c_out"
                    xray_test_unlock_dir "$cache_dir.lock"
                else
                    cp "$tmp_dump" "$dump" 2>/dev/null || true
                    rm -f "$tmp_dump" "$tmp_c"
                    xray_test_unlock_dir "$cache_dir.lock"
                    echo "FAIL (dump command failed)"
                    show_excerpt "$dump"
                    FAIL=$((FAIL + 1))
                    return 1
                fi
            fi
        elif ! run_dump_command "$c_out" "$dump" "$xr_file" "$extra_args"; then
            echo "FAIL (dump command failed)"
            show_excerpt "$dump"
            FAIL=$((FAIL + 1))
            return 1
        fi
    fi

    check_expect "$expect" "$dump" "$c_out"
}

record_filetest_log() {
    local log="$1" status="$2" first_line rc
    cat "$log"

    first_line="$(sed -n '1p' "$log")"
    rc="$(cat "$status" 2>/dev/null || printf '99')"
    case "$first_line" in
        *PASS*) PASS=$((PASS + 1)) ;;
        *SKIP*) SKIP=$((SKIP + 1)) ;;
        *)
            FAIL=$((FAIL + 1))
            if [ "$rc" -ne 0 ] 2>/dev/null && [ -z "$first_line" ]; then
                echo "  <worker>                                                       FAIL (worker exited $rc)"
            fi
            ;;
    esac
}

run_selected_parallel() {
    local jobs="$1"
    local list="$WORK/selected.list"
    local mode dir f dir_key idx=0 log status sem token i p pids logs
    local tab

    tab="$(printf '\t')"
    : >"$list"
    for mode in $SELECTED_MODES; do
        dir="$FILETEST_DIR/$mode"
        [ -d "$dir" ] || continue
        dir_key="$(xray_test_case_dir_key "$dir/__dir_key__")"
        for f in "$dir"/*.xr; do
            [ -f "$f" ] || continue
            is_collected_test "$f" || continue
            printf '%s\t%s\t%s\n' "$mode" "$dir_key" "$f" >>"$list"
        done
    done

    mkdir -p "$WORK/logs"
    sem="$WORK/selected.sem"
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
    while IFS="$tab" read -r mode dir_key f; do
        [ -n "$f" ] || continue
        IFS= read -r -n 1 token <&9
        log="$WORK/logs/selected_${idx}.log"
        status="$WORK/logs/selected_${idx}.status"
        (
            run_one "$mode" "$f" "$dir_key" >"$log" 2>&1
            printf '%s\n' "$?" >"$status"
            printf '.' >&9
        ) &
        pids="$pids $!"
        logs="$logs $log"
        idx=$((idx + 1))
    done <"$list"

    for p in $pids; do
        wait "$p"
    done
    exec 9>&-
    exec 9<&-

    for log in $logs; do
        status="${log%.log}.status"
        record_filetest_log "$log" "$status"
    done
}

configure_jobs "$REQUESTED_JOBS"

echo "=== AOT Filetests (XAOT plan scaffold) ==="
echo "Binary: $XRAY"
echo "Mode:   $MODE"
echo "Jobs:   $JOBS"
echo "Cache:  $FILETEST_CACHE"
echo ""

TEST_COUNT="$(count_selected_tests)"

if ! command -v "$XRAY" >/dev/null 2>&1 && [ ! -x "$XRAY" ]; then
    SKIP="$TEST_COUNT"
    echo "SKIP: xray binary not found or not executable: $XRAY"
    echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
    exit 0
fi

if ! probe_dump_support; then
    SKIP="$TEST_COUNT"
    echo "SKIP: xray build --native --dump-xaot-plan 待实现; $TEST_COUNT filetest(s) not run"
    reason="$(first_nonempty_line "$WORK/probe.log")"
    if [ -n "$reason" ]; then
        echo "Reason: $reason"
    fi
    echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
    exit 0
fi

if [ "$TEST_COUNT" -eq 0 ]; then
    echo "SKIP: no AOT filetests found for mode '$MODE'"
    echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
    exit 0
fi

if [ "$JOBS" -le 1 ]; then
    for mode in $SELECTED_MODES; do
        dir="$FILETEST_DIR/$mode"
        [ -d "$dir" ] || continue
        echo "--- $mode ---"
        for f in "$dir"/*.xr; do
            [ -f "$f" ] || continue
            is_collected_test "$f" || continue
            run_one "$mode" "$f"
        done
        echo ""
    done
else
    echo "--- selected ---"
    run_selected_parallel "$JOBS"
    echo ""
fi

echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
