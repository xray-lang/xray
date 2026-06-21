#!/bin/bash
# run_backend_diff.sh - cross-backend differential test (107).
#
# Runs the same .xr program through VM and AOT, and asserts
# their observable output is byte-for-byte identical: stdout, exit code, and
# (normalized) stderr. This is the safety net for 108-118: any change that
# alters observable behavior must be caught here.
#
# Usage:
#   tests/diff/run_backend_diff.sh [xray_binary]
#
# Environment:
#   XRAY_BIN            xray binary (default: build/xray; also XRAY_BUILD_DIR)
#   XRAY_DIFF_BACKENDS  comma list subset of vm,aot (default: vm,aot)
#   XRAY_DIFF_JOBS      parallel case workers (default: auto, capped by
#                       XRAY_DIFF_MAX_AUTO_JOBS=16; XRAY_TEST_JOBS also works)
#   XRAY_DIFF_SHARD_TOTAL / XRAY_DIFF_SHARD_INDEX
#                       run only one stable 0-based shard of the case list
#   XRAY_DIFF_EXTRA_CASES_FILE
#                       newline case manifest, relative to repo root or absolute
#                       (default: tests/diff/coro_regression_cases.txt; empty disables)
#   XRAY_DIFF_CASES_FILE
#                       optional base case manifest replacing tests/diff/cases/**/*.xr
#   XRAY_DIFF_CACHE_DIR shared native object cache for AOT backend builds
#                       (default: build/.xray-test-cache/aot-objects)
#   XRAY_DIFF_BIN_CACHE_DIR
#                       cached AOT backend test binaries
#                       (default: build/.xray-test-cache/backend-diff-bin/<xray-key>/O<opt>)
#   XRAY_AOT_TEST_OPT   AOT C compiler optimization level for correctness gates
#                       (default: 0; set to 3 for optimized smoke/CI)
#
# Per-case optional sidecar:
#   <case>.args    first line = whitespace-separated program arguments
#   <case>.xr first line may carry "// anchor: <tag>" for failure labeling.
#
# Cases live in tests/diff/cases/**/*.xr; files whose basename starts with
# '_' are skipped (shared helpers / fixtures).
#
# Written for bash 3.2 portability (no associative arrays): per-backend exit
# codes are stashed in files, not a declare -A map.
#
# Exit: 0 all backends agree on every case; 1 any divergence/failure.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$PROJECT_DIR/tests/test_common.sh"
CASE_DIR="$SCRIPT_DIR/cases"
NORMALIZE="$SCRIPT_DIR/normalize.sed"
EXTRA_CASES_FILE="${XRAY_DIFF_EXTRA_CASES_FILE-$SCRIPT_DIR/coro_regression_cases.txt}"
BASE_CASES_FILE="${XRAY_DIFF_CASES_FILE:-}"

XRAY="${1:-${XRAY_BIN:-}}"
if [ -z "$XRAY" ]; then
    if [ -n "${XRAY_BUILD_DIR:-}" ]; then
        XRAY="$XRAY_BUILD_DIR/xray"
    else
        XRAY="$PROJECT_DIR/build/xray"
    fi
fi

BACKENDS="${XRAY_DIFF_BACKENDS:-vm,aot}"
# Observable contract = stdout + exit code (matches tests/aot/run_aot_tests.sh).
# stderr is backend-diagnostic-heavy (AOT cc/build logs, native-class
# warnings), so it is NOT part of pass/fail unless XRAY_DIFF_STDERR=1.
DIFF_STDERR="${XRAY_DIFF_STDERR:-0}"
REQUESTED_JOBS="${XRAY_DIFF_JOBS:-${XRAY_TEST_JOBS:-auto}}"
AOT_OPT_LEVEL="${XRAY_AOT_TEST_OPT:-0}"
AOT_CACHE="${XRAY_DIFF_CACHE_DIR:-$(xray_test_shared_cache_dir "$PROJECT_DIR" "aot-objects")}"
AOT_BIN_CACHE="${XRAY_DIFF_BIN_CACHE_DIR:-$(xray_test_stable_cache_dir "$PROJECT_DIR" "backend-diff-bin" "$XRAY")/O$AOT_OPT_LEVEL}"
SHARD_TOTAL="${XRAY_DIFF_SHARD_TOTAL:-1}"
SHARD_INDEX="${XRAY_DIFF_SHARD_INDEX:-0}"
SINGLE_CASE="${XRAY_DIFF_SINGLE_CASE:-}"
SINGLE_ID="${XRAY_DIFF_SINGLE_ID:-0}"
TAB="$(printf '\t')"
JOBS=1

PASS=0
FAIL=0
SKIP=0

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_backend_diff.XXXXXX")" || {
    echo "error: cannot create temp dir" >&2
    exit 1
}
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

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
            max_auto="${XRAY_DIFF_MAX_AUTO_JOBS:-16}"
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

validate_shard_config() {
    if ! is_uint "$SHARD_TOTAL" || ! is_uint "$SHARD_INDEX"; then
        echo "error: shard config must be numeric: total=$SHARD_TOTAL index=$SHARD_INDEX" >&2
        exit 2
    fi
    if [ "$SHARD_TOTAL" -lt 1 ]; then
        echo "error: XRAY_DIFF_SHARD_TOTAL must be >= 1" >&2
        exit 2
    fi
    if [ "$SHARD_INDEX" -ge "$SHARD_TOTAL" ]; then
        echo "error: XRAY_DIFF_SHARD_INDEX must be in [0,total): index=$SHARD_INDEX total=$SHARD_TOTAL" >&2
        exit 2
    fi
}

backend_enabled() {
    case ",$BACKENDS," in
        *",$1,"*) return 0 ;;
        *) return 1 ;;
    esac
}

normalize() {
    if [ -f "$NORMALIZE" ]; then
        sed -f "$NORMALIZE" "$1"
    else
        cat "$1"
    fi
}

rel_path() {
    case "$1" in
        "$PROJECT_DIR"/*) printf '%s' "${1#"$PROJECT_DIR"/}" ;;
        *) printf '%s' "$1" ;;
    esac
}

append_case_manifest() {
    local manifest="$1"
    local manifest_path
    [ -n "$manifest" ] || return 0
    case "$manifest" in
        /*) manifest_path="$manifest" ;;
        *) manifest_path="$PROJECT_DIR/$manifest" ;;
    esac
    [ -f "$manifest_path" ] || return 1

    while IFS= read -r f; do
        case "$f" in
            ""|\#*) continue ;;
            /*) printf '%s\n' "$f" ;;
            *) printf '%s/%s\n' "$PROJECT_DIR" "$f" ;;
        esac
    done <"$manifest_path"
}

# run_backend KIND CASE OUT_PREFIX [args...]
# Writes: OUT_PREFIX.out (stdout), OUT_PREFIX.err (normalized stderr),
#         OUT_PREFIX.rc (exit code as text).
run_backend() {
    local kind="$1" case="$2" out_prefix="$3"
    shift 3
    local raw_err="$out_prefix.rawerr"
    local rc=0
    case "$kind" in
        vm)
            if [ "$#" -gt 0 ]; then
                "$XRAY" run "$case" -- "$@" >"$out_prefix.out" 2>"$raw_err" || rc=$?
            else
                "$XRAY" run "$case" >"$out_prefix.out" 2>"$raw_err" || rc=$?
            fi
            ;;
        aot)
            local rel safe key bin_dir bin tmp_bin
            rel="$(rel_path "$case")"
            safe="$(printf '%s' "${rel%.xr}" | sed 's#[^A-Za-z0-9_.-]#_#g')"
            key="$(xray_test_case_dir_key "$case")"
            bin_dir="$AOT_BIN_CACHE/$safe-$key"
            bin="$bin_dir/aot"
            tmp_bin="$bin_dir/aot.$$"
            if [ ! -x "$bin" ]; then
                mkdir -p "$bin_dir"
                if ! xray_test_lock_dir "$bin_dir.lock"; then
                    echo "cannot lock binary cache: $bin_dir.lock" >"$out_prefix.buildlog"
                    echo "BUILDFAIL" >"$out_prefix.out"
                    : >"$raw_err"
                    rc=200
                elif [ ! -x "$bin" ]; then
                    rm -f "$tmp_bin"
                    if ! "$XRAY" build --native -O "$AOT_OPT_LEVEL" --cache-dir "$AOT_CACHE" "$case" \
                            -o "$tmp_bin" >"$out_prefix.buildlog" 2>&1; then
                        echo "BUILDFAIL" >"$out_prefix.out"
                        : >"$raw_err"
                        rc=200
                    else
                        mv "$tmp_bin" "$bin"
                    fi
                    rm -f "$tmp_bin"
                    xray_test_unlock_dir "$bin_dir.lock"
                else
                    printf 'cached: %s\n' "$bin" >"$out_prefix.buildlog"
                    xray_test_unlock_dir "$bin_dir.lock"
                fi
            else
                printf 'cached: %s\n' "$bin" >"$out_prefix.buildlog"
            fi
            if [ "$rc" -eq 200 ]; then
                :
            else
                if [ "$#" -gt 0 ]; then
                    "$bin" "$@" >"$out_prefix.out" 2>"$raw_err" || rc=$?
                else
                    "$bin" >"$out_prefix.out" 2>"$raw_err" || rc=$?
                fi
            fi
            ;;
    esac
    normalize "$raw_err" >"$out_prefix.err"
    printf '%s' "$rc" >"$out_prefix.rc"
}

run_case() {
    local case="$1"
    local case_id="${2:-0}"
    local name base anchor argfile
    name="$(rel_path "$case")"
    base="$(printf '%05d_%s' "$case_id" "$(printf '%s' "${name%.xr}" | sed 's#[^A-Za-z0-9_.-]#_#g')")"
    printf '  %-84s' "$name"

    anchor="$(sed -n '1{s#^// *anchor: *##p;}' "$case" 2>/dev/null)"

    # Per-case backend restriction. A case may carry, within its first 5 lines,
    #   // diff-backends: vm,aot
    # to opt out of backends that have a known, tracked divergence.  The
    # exclusion is printed (never silent) so the net still documents what it
    # is not yet covering.
    local case_backends
    case_backends="$(sed -n '1,5{s#^// *diff-backends: *##p;}' "$case" 2>/dev/null | head -n1)"

    local has_args=0
    local argline=""
    argfile="${case%.xr}.args"
    if [ -f "$argfile" ]; then
        argline="$(head -n1 "$argfile")"
        has_args=1
    fi

    local enabled="" excluded=""
    for b in vm aot; do
        backend_enabled "$b" || continue
        if [ -n "$case_backends" ]; then
            case ",$case_backends," in
                *",$b,"*) ;;
                *) excluded="$excluded $b"; continue ;;
            esac
        fi
        enabled="$enabled $b"
    done
    # count enabled
    local nb=0
    for b in $enabled; do nb=$((nb + 1)); done
    if [ "$nb" -lt 2 ]; then
        echo "SKIP (need >=2 backends; case=${case_backends:-all} global=$BACKENDS)"
        SKIP=$((SKIP + 1))
        return 0
    fi

    for b in $enabled; do
        if [ "$has_args" -eq 1 ]; then
            # word-split argline intentionally
            # shellcheck disable=SC2086
            run_backend "$b" "$case" "$WORK/${base}.$b" $argline
        else
            run_backend "$b" "$case" "$WORK/${base}.$b"
        fi
    done

    local ref=""
    for b in $enabled; do ref="$b"; break; done
    local ref_out="$WORK/${base}.$ref.out"
    local ref_err="$WORK/${base}.$ref.err"
    local ref_rc cur_rc
    ref_rc="$(cat "$WORK/${base}.$ref.rc")"
    local mismatch=""
    local other=""
    for b in $enabled; do
        [ "$b" = "$ref" ] && continue
        cur_rc="$(cat "$WORK/${base}.$b.rc")"
        if [ "$cur_rc" != "$ref_rc" ]; then
            mismatch="exit code ($ref=$ref_rc $b=$cur_rc)"; other="$b"; break
        fi
        if ! diff -q "$ref_out" "$WORK/${base}.$b.out" >/dev/null 2>&1; then
            mismatch="stdout ($ref vs $b)"; other="$b"; break
        fi
        if [ "$DIFF_STDERR" = "1" ] && ! diff -q "$ref_err" "$WORK/${base}.$b.err" >/dev/null 2>&1; then
            mismatch="stderr ($ref vs $b)"; other="$b"; break
        fi
    done

    if [ -z "$mismatch" ]; then
        if [ -n "$excluded" ]; then
            echo "PASS (excl:$excluded)"
        else
            echo "PASS"
        fi
        PASS=$((PASS + 1))
        return 0
    fi

    echo "FAIL ($mismatch)${anchor:+  [anchor: $anchor]}"
    for b in $enabled; do
        echo "      $b: rc=$(cat "$WORK/${base}.$b.rc")  stdout: $(head -3 "$WORK/${base}.$b.out" | tr '\n' '|')"
    done
    case "$mismatch" in
        stdout*) diff -u "$ref_out" "$WORK/${base}.$other.out" | sed -n '1,30p' | sed 's/^/      /' ;;
        stderr*) diff -u "$ref_err" "$WORK/${base}.$other.err" | sed -n '1,30p' | sed 's/^/      /' ;;
    esac
    FAIL=$((FAIL + 1))
    return 1
}

record_case_log() {
    local log="$1" status="$2" first_line rc
    cat "$log"

    first_line="$(sed -n '1p' "$log")"
    rc="$(cat "$status" 2>/dev/null || printf '99')"
    case "$first_line" in
        *PASS*) PASS=$((PASS + 1)) ;;
        *SKIP*) SKIP=$((SKIP + 1)) ;;
        *)
            FAIL=$((FAIL + 1))
            if [ "$rc" -eq 0 ] 2>/dev/null; then
                :
            else
                [ -n "$first_line" ] || echo "  <worker>                                                                FAIL (worker exited $rc)"
            fi
            ;;
    esac
}

run_cases_parallel() {
    local list="$1"
    local jobs="$2"
    local idx case log status sem token i p pids logs
    mkdir -p "$WORK/logs"

    sem="$WORK/worker.sem"
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

    while IFS="$TAB" read -r idx case; do
        [ -n "$case" ] || continue
        IFS= read -r -n 1 token <&9
        log="$WORK/logs/$idx.log"
        status="$WORK/logs/$idx.status"
        (
            run_case "$case" "$idx" >"$log" 2>&1
            printf '%s\n' "$?" >"$status"
            printf '.' >&9
        ) &
        pids="$pids $!"
        logs="$logs $log"
    done <"$list"

    for p in $pids; do
        wait "$p"
    done
    exec 9>&-
    exec 9<&-

    for log in $logs; do
        status="${log%.log}.status"
        record_case_log "$log" "$status"
    done
}

configure_jobs "$REQUESTED_JOBS"
validate_shard_config

if [ -n "$SINGLE_CASE" ]; then
    run_case "$SINGLE_CASE" "$SINGLE_ID"
    exit $?
fi

echo "=== Backend Differential (VM / AOT) ==="
echo "Binary:   $XRAY"
echo "Backends: $BACKENDS"
echo "Jobs:     $JOBS"
echo "AOT opt:  -O$AOT_OPT_LEVEL"
echo "Cache:    $AOT_CACHE"
echo "BinCache: $AOT_BIN_CACHE"
if [ "$SHARD_TOTAL" -gt 1 ]; then
    echo "Shard:    $SHARD_INDEX / $SHARD_TOTAL"
fi
echo ""

if ! command -v "$XRAY" >/dev/null 2>&1 && [ ! -x "$XRAY" ]; then
    echo "SKIP: xray binary not found: $XRAY"
    echo "=== Results: 0 passed, 0 failed, 0 skipped ==="
    exit 0
fi

if [ ! -d "$CASE_DIR" ]; then
    echo "SKIP: no case dir $CASE_DIR"
    echo "=== Results: 0 passed, 0 failed, 0 skipped ==="
    exit 0
fi

CASE_LIST="$WORK/cases.list"
if [ -n "$BASE_CASES_FILE" ]; then
    if ! append_case_manifest "$BASE_CASES_FILE" | sort >"$CASE_LIST"; then
        echo "error: base case manifest not found: $BASE_CASES_FILE" >&2
        exit 2
    fi
else
    find "$CASE_DIR" -name '*.xr' | sort >"$CASE_LIST"
fi
if [ -n "$EXTRA_CASES_FILE" ]; then
    append_case_manifest "$EXTRA_CASES_FILE" >>"$CASE_LIST" || true
fi

CASE_RUN_LIST="$WORK/cases.run.list"
: >"$CASE_RUN_LIST"
CASE_INDEX=0
while IFS= read -r f; do
    [ -f "$f" ] || continue
    case "$(basename "$f")" in
        _*) continue ;;
    esac
    if [ $((CASE_INDEX % SHARD_TOTAL)) -eq "$SHARD_INDEX" ]; then
        printf '%s\t%s\n' "$CASE_INDEX" "$f" >>"$CASE_RUN_LIST"
    fi
    CASE_INDEX=$((CASE_INDEX + 1))
done <"$CASE_LIST"

if [ "$SHARD_TOTAL" -gt 1 ]; then
    SELECTED_CASES="$(wc -l <"$CASE_RUN_LIST" | tr -d ' ')"
    echo "Cases:    $SELECTED_CASES / $CASE_INDEX"
    echo ""
fi

if [ "$JOBS" -le 1 ]; then
    while IFS="$TAB" read -r idx f; do
        [ -n "$f" ] || continue
        run_case "$f" "$idx"
    done <"$CASE_RUN_LIST"
else
    run_cases_parallel "$CASE_RUN_LIST" "$JOBS"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
