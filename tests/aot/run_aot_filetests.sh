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

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
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

expect_args() {
    sed -n 's/^args=//p' "$1" 2>/dev/null | sed -n '1p'
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
            echo "    expected args=, contains=, not_contains=, regex=, not_regex=, c_contains=, c_regex=, or skip="
            FAIL=$((FAIL + 1))
            return 1
        fi

        case "$key" in
            args)
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

run_one() {
    local mode="$1"
    local xr_file="$2"
    local test_name base dump c_out expect extra_args

    base="$(basename "$xr_file" .xr)"
    test_name="$(rel_path "$xr_file")"
    dump="$WORK/${mode}_${base}.dump"
    c_out="$WORK/${mode}_${base}.c"
    expect="${xr_file%.xr}.expect"
    extra_args="$(expect_args "$expect")"

    printf '  %-9s %-48s' "$mode" "$test_name"

    local build_args=(build --native --dump-xaot-plan --dump-link-manifest)
    if [ -n "$extra_args" ]; then
        local expect_build_args=($extra_args)
        build_args+=("${expect_build_args[@]}")
    fi
    build_args+=(-c -o "$c_out" "$xr_file")

    if ! "$XRAY" "${build_args[@]}" > "$dump" 2>&1; then
        echo "FAIL (dump command failed)"
        show_excerpt "$dump"
        FAIL=$((FAIL + 1))
        return 1
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

run_mode_parallel() {
    local mode="$1"
    local dir="$2"
    local jobs="$3"
    local list="$WORK/${mode}.list"
    local idx=0 f log status sem token i p pids logs

    : >"$list"
    for f in "$dir"/*.xr; do
        [ -f "$f" ] || continue
        is_collected_test "$f" || continue
        printf '%s\n' "$f" >>"$list"
    done

    mkdir -p "$WORK/logs"
    sem="$WORK/${mode}.sem"
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
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        IFS= read -r -n 1 token <&9
        log="$WORK/logs/${mode}_${idx}.log"
        status="$WORK/logs/${mode}_${idx}.status"
        (
            run_one "$mode" "$f" >"$log" 2>&1
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

for mode in $SELECTED_MODES; do
    dir="$FILETEST_DIR/$mode"
    [ -d "$dir" ] || continue
    echo "--- $mode ---"
    if [ "$JOBS" -le 1 ]; then
        for f in "$dir"/*.xr; do
            [ -f "$f" ] || continue
            is_collected_test "$f" || continue
            run_one "$mode" "$f"
        done
    else
        run_mode_parallel "$mode" "$dir" "$JOBS"
    fi
    echo ""
done

echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
