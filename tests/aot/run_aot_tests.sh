#!/bin/bash
# AOT VM-AOT diff test suite
# Gold standard: diff <(xray run X) <(./aot_X) must be empty
#
# Usage: ./tests/aot/run_aot_tests.sh [xray_binary]
#
# Environment:
#   XRAY_AOT_JOBS       parallel test workers (default: auto, capped by
#                       XRAY_AOT_MAX_AUTO_JOBS=16; XRAY_TEST_JOBS also works)
#   XRAY_AOT_CACHE_DIR  shared native object cache for AOT test builds
#                       (default: build/.xray-test-cache/aot-objects)
#   XRAY_AOT_BIN_CACHE_DIR
#                       cached native test binaries
#                       (default: build/.xray-test-cache/aot-bin/<xray-key>/O<opt>)
#   XRAY_AOT_TEST_OPT   native C compiler optimization level for correctness
#                       gates (default: 0; set to 3 for optimized smoke/CI)
#   XRAY_AOT_SHARD_TOTAL / XRAY_AOT_SHARD_INDEX
#                       run only one stable 0-based shard of the case list
#   XRAY_AOT_FAST_TEST_BUILD
#                       use correctness-test AOT link flags (default: 1);
#                       set to 0 to exercise product size/link flags
#   XRAY_AOT_RUNNER     python (default) or bash
#   XRAY_TEST_DISABLE_RUN_CACHE
#                       set to 1 to force executing cached native binaries and
#                       VM runs even when a previous identical run passed
#   XRAY_AOT_KEEP_WORK  keep temporary outputs on exit for debugging

set -u

XRAY="${1:-./build/xray}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$PROJECT_DIR/tests/test_common.sh"

if [ "${XRAY_AOT_RUNNER:-python}" != "bash" ]; then
    PYTHON_BIN="${PYTHON:-python3}"
    if command -v "$PYTHON_BIN" >/dev/null 2>&1; then
        exec "$PYTHON_BIN" "$SCRIPT_DIR/run_aot_tests_fast.py" "$@"
    fi
    if [ "${XRAY_AOT_RUNNER:-python}" = "python" ]; then
        echo "error: python3 is required for XRAY_AOT_RUNNER=python; set XRAY_AOT_RUNNER=bash to use the legacy runner" >&2
        exit 2
    fi
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/aot_test.XXXXXX")" || {
    echo "error: cannot create temp dir" >&2
    exit 1
}
cleanup() {
    if [ "${XRAY_AOT_KEEP_WORK:-0}" = "1" ]; then
        echo "Work dir: $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

REQUESTED_JOBS="${XRAY_AOT_JOBS:-${XRAY_TEST_JOBS:-auto}}"
AOT_OPT_LEVEL="${XRAY_AOT_TEST_OPT:-0}"
AOT_CACHE="${XRAY_AOT_CACHE_DIR:-$(xray_test_shared_cache_dir "$PROJECT_DIR" "aot-objects")}"
AOT_BIN_CACHE="${XRAY_AOT_BIN_CACHE_DIR:-$(xray_test_stable_cache_dir "$PROJECT_DIR" "aot-bin" "$XRAY")/O$AOT_OPT_LEVEL}"
SHARD_TOTAL="${XRAY_AOT_SHARD_TOTAL:-1}"
SHARD_INDEX="${XRAY_AOT_SHARD_INDEX:-0}"
: "${XRAY_AOT_FAST_TEST_BUILD:=1}"
export XRAY_AOT_FAST_TEST_BUILD
JOBS=1
PASS=0
FAIL=0
SKIP=0
CASE_INDEX=0
SELECTED_CASES=0

echo "=== AOT VM-AOT Diff Tests ==="
echo "Binary: $XRAY"

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
            max_auto="${XRAY_AOT_MAX_AUTO_JOBS:-16}"
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
        echo "error: XRAY_AOT_SHARD_TOTAL must be >= 1" >&2
        exit 2
    fi
    if [ "$SHARD_INDEX" -ge "$SHARD_TOTAL" ]; then
        echo "error: XRAY_AOT_SHARD_INDEX must be in [0,total): index=$SHARD_INDEX total=$SHARD_TOTAL" >&2
        exit 2
    fi
}

rel_case_name() {
    case "$1" in
        "$SCRIPT_DIR"/*) printf '%s' "${1#"$SCRIPT_DIR"/}" ;;
        *) printf '%s' "$(basename "$1")" ;;
    esac
}

safe_case_name() {
    printf '%s' "${1%.xr}" | sed 's#[^A-Za-z0-9_.-]#_#g'
}

configure_jobs "$REQUESTED_JOBS"
validate_shard_config
echo "Jobs:   $JOBS"
echo "Cache:  $AOT_CACHE"
echo "BinCache: $AOT_BIN_CACHE"
echo "AOT opt: -O$AOT_OPT_LEVEL"
if [ "$SHARD_TOTAL" -gt 1 ]; then
    echo "Shard: $SHARD_INDEX / $SHARD_TOTAL"
fi
echo ""

run_test() {
    local xr_file="$1"
    local rel_name test_name safe_name case_key case_work bin_dir bin_out tmp_bin vm_out aot_out
    local run_key_material run_key run_dir
    local test_args=()
    local vm_rc=0
    local aot_rc=0

    rel_name="$(rel_case_name "$xr_file")"
    test_name="${rel_name%.xr}"
    safe_name="$(safe_case_name "$rel_name")"
    case_key="$(xray_test_case_dir_key "$xr_file")"
    case_work="$WORK/$safe_name"
    mkdir -p "$case_work" || {
        printf "  %-42sFAIL (cannot create work dir)\n" "$test_name"
        FAIL=$((FAIL + 1))
        return 1
    }
    bin_dir="$AOT_BIN_CACHE/$safe_name-$case_key"
    bin_out="$bin_dir/aot"
    tmp_bin="$bin_dir/aot.$$"
    vm_out="$case_work/vm.out"
    aot_out="$case_work/aot.out"

    case "$(basename "$xr_file" .xr)" in
        process_args*) test_args=("100000" "abc") ;;
    esac

    run_key_material="args:"
    if [ "${#test_args[@]}" -gt 0 ]; then
        run_key_material="$run_key_material
$(printf '%s\n' "${test_args[@]}")"
    fi
    run_key_material="$run_key_material
XRAY_CORO_DETERMINISTIC=${XRAY_CORO_DETERMINISTIC:-}
XRAY_CORO_SEED=${XRAY_CORO_SEED:-}"
    run_key="$(xray_test_string_key "$run_key_material")"
    run_dir="$bin_dir/run-$run_key"

    printf "  %-42s" "$test_name"

    # Step 1: Build .xr → native binary through the public AOT path.
    # The final executable is content-addressed by xray binary + local test
    # sources + opt level. Warm reruns skip frontend/codegen/link entirely.
    if [ ! -x "$bin_out" ]; then
        mkdir -p "$bin_dir" || {
            echo "FAIL (cannot create binary cache dir)"
            FAIL=$((FAIL + 1))
            return 1
        }
        if ! xray_test_lock_dir "$bin_dir.lock"; then
            echo "FAIL (cannot lock binary cache)"
            FAIL=$((FAIL + 1))
            return 1
        fi
        if [ ! -x "$bin_out" ]; then
            local ok=0
            for attempt in 1 2 3; do
                rm -f "$tmp_bin"
                if "$XRAY" build --native -O "$AOT_OPT_LEVEL" --cache-dir "$AOT_CACHE" "$xr_file" \
                        -o "$tmp_bin" \
                        >/dev/null 2>&1; then
                    mv "$tmp_bin" "$bin_out"
                    ok=1; break
                fi
            done
            rm -f "$tmp_bin"
            if [ "$ok" -eq 0 ]; then
                xray_test_unlock_dir "$bin_dir.lock"
                # Positive dirs must build; a persistent failure is a regression,
                # not a skip (expected-unsupported cases live under negative/).
                echo "FAIL (native build failed after retries)"
                FAIL=$((FAIL + 1))
                return 1
            fi
        fi
        xray_test_unlock_dir "$bin_dir.lock"
    fi

    if [ "${XRAY_TEST_DISABLE_RUN_CACHE:-0}" != "1" ] && [ -f "$run_dir/pass" ]; then
        echo "PASS (cached)"
        PASS=$((PASS + 1))
        rm -rf "$case_work"
        return 0
    fi

    # Step 2: Run VM and AOT, capturing stdout AND exit code (if-form keeps
    # set -e from aborting on a non-zero program exit).
    if [ "${#test_args[@]}" -gt 0 ]; then
        if "$XRAY" run "$xr_file" -- "${test_args[@]}" > "$vm_out" 2>/dev/null; then vm_rc=0; else vm_rc=$?; fi
        if "$bin_out" "${test_args[@]}" > "$aot_out" 2>/dev/null; then aot_rc=0; else aot_rc=$?; fi
    else
        if "$XRAY" run "$xr_file" > "$vm_out" 2>/dev/null; then vm_rc=0; else vm_rc=$?; fi
        if "$bin_out" > "$aot_out" 2>/dev/null; then aot_rc=0; else aot_rc=$?; fi
    fi

    # Step 3: Compare exit codes, then stdout
    if [ "$vm_rc" != "$aot_rc" ]; then
        echo "FAIL (exit code: VM=$vm_rc AOT=$aot_rc)"
        FAIL=$((FAIL + 1))
        return 1
    elif diff -u "$vm_out" "$aot_out" > /dev/null 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
        if [ "${XRAY_TEST_DISABLE_RUN_CACHE:-0}" != "1" ] &&
                xray_test_lock_dir "$run_dir.lock"; then
            if [ ! -f "$run_dir/pass" ]; then
                mkdir -p "$run_dir"
                cp "$vm_out" "$run_dir/vm.out"
                cp "$aot_out" "$run_dir/aot.out"
                printf '%s\n' "$vm_rc" >"$run_dir/vm.rc"
                printf '%s\n' "$aot_rc" >"$run_dir/aot.rc"
                : >"$run_dir/pass"
            fi
            xray_test_unlock_dir "$run_dir.lock"
        fi
        rm -rf "$case_work"
        return 0
    else
        echo "FAIL (output mismatch)"
        echo "    VM:  $(head -5 "$vm_out" | tr '\n' '|')"
        echo "    AOT: $(head -5 "$aot_out" | tr '\n' '|')"
        FAIL=$((FAIL + 1))
        return 1
    fi
}

run_negative_test() {
    local xr_file="$1"
    local rel_name test_name safe_name case_work bin_out log_out

    rel_name="$(rel_case_name "$xr_file")"
    test_name="${rel_name%.xr}"
    safe_name="$(safe_case_name "$rel_name")"
    case_work="$WORK/$safe_name"
    mkdir -p "$case_work" || {
        printf "  %-42sFAIL (cannot create work dir)\n" "$test_name"
        FAIL=$((FAIL + 1))
        return 1
    }
    bin_out="$case_work/aot"
    log_out="$case_work/build.log"

    printf "  %-42s" "$test_name"

    if "$XRAY" build --native -O "$AOT_OPT_LEVEL" --cache-dir "$AOT_CACHE" "$xr_file" \
            -o "$bin_out" \
            >"$log_out" 2>&1; then
        echo "FAIL (unexpected AOT success)"
        FAIL=$((FAIL + 1))
        return 1
    fi

    if grep -Eq "unsupported .*coroutine Xi op|unsupported AOT sync call to suspendable function|unsupported AOT indirect call|exceptions inside AOT coroutine are unsupported|unsupported Xi op ERR_|semantic analysis failed|: error: " "$log_out"; then
        echo "PASS (rejected)"
        PASS=$((PASS + 1))
        rm -rf "$case_work"
        return 0
    else
        echo "FAIL (wrong rejection)"
        echo "    $(head -5 "$log_out" | tr '\n' '|')"
        FAIL=$((FAIL + 1))
        return 1
    fi
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
                [ -n "$first_line" ] || echo "  <worker>                                  FAIL (worker exited $rc)"
            fi
            ;;
    esac
}

run_case_file() {
    local kind="$1"
    local f="$2"
    case "$kind" in
        negative) run_negative_test "$f" ;;
        *) run_test "$f" ;;
    esac
}

run_list_parallel() {
    local kind="$1"
    local list="$2"
    local jobs="$3"
    local idx=0 f log status sem token i p pids logs
    mkdir -p "$WORK/logs"

    sem="$WORK/${kind}.sem"
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
        log="$WORK/logs/${kind}_${idx}.log"
        status="$WORK/logs/${kind}_${idx}.status"
        (
            run_case_file "$kind" "$f" >"$log" 2>&1
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
        record_case_log "$log" "$status"
    done
}

run_section() {
    local title="$1"
    local kind="$2"
    local dir="$3"
    local list="$WORK/${title}.list"
    local f

    if [ -d "$dir" ]; then
        echo "--- $title ---"
        : >"$list"
        for f in "$dir"/*.xr; do
            [ -f "$f" ] || continue
            if [ $((CASE_INDEX % SHARD_TOTAL)) -eq "$SHARD_INDEX" ]; then
                printf '%s\n' "$f" >>"$list"
                SELECTED_CASES=$((SELECTED_CASES + 1))
            fi
            CASE_INDEX=$((CASE_INDEX + 1))
        done
        if [ "$JOBS" -le 1 ]; then
            while IFS= read -r f; do
                [ -n "$f" ] || continue
                run_case_file "$kind" "$f"
            done <"$list"
        else
            run_list_parallel "$kind" "$list" "$JOBS"
        fi
        echo ""
    fi
}

append_section() {
    local dir="$1"
    local list="$2"
    local f
    [ -d "$dir" ] || return 0
    for f in "$dir"/*.xr; do
        [ -f "$f" ] || continue
        if [ $((CASE_INDEX % SHARD_TOTAL)) -eq "$SHARD_INDEX" ]; then
            printf '%s\n' "$f" >>"$list"
            SELECTED_CASES=$((SELECTED_CASES + 1))
        fi
        CASE_INDEX=$((CASE_INDEX + 1))
    done
}

run_positive_sections() {
    local list="$WORK/positive.list"
    : >"$list"
    append_section "$SCRIPT_DIR/basic" "$list"
    append_section "$SCRIPT_DIR/modules" "$list"
    append_section "$SCRIPT_DIR/coro" "$list"

    if [ ! -s "$list" ]; then
        return 0
    fi

    echo "--- positive ---"
    if [ "$JOBS" -le 1 ]; then
        while IFS= read -r f; do
            [ -n "$f" ] || continue
            run_case_file "positive" "$f"
        done <"$list"
    else
        run_list_parallel "positive" "$list" "$JOBS"
    fi
    echo ""
}

# Run all .xr files in test directories
run_positive_sections
run_section "negative" "negative" "$SCRIPT_DIR/negative"

if [ "$SHARD_TOTAL" -gt 1 ]; then
    echo "Shard cases: $SELECTED_CASES / $CASE_INDEX"
    echo ""
fi

echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
