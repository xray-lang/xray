#!/bin/bash
# Native runtime benchmark runner for AOT zero-cost evidence paths.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
MANIFEST="${XRAY_ZERO_COST_BENCH_MANIFEST:-$SCRIPT_DIR/benchmarks.tsv}"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
CASES="${XRAY_ZERO_COST_BENCH_CASES:-all}"
REPEAT="${XRAY_ZERO_COST_BENCH_REPEAT:-1}"
N="${XRAY_ZERO_COST_BENCH_N:-20000}"
OPT="${XRAY_ZERO_COST_BENCH_OPT:-2}"
KEEP="${XRAY_ZERO_COST_BENCH_KEEP:-0}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_zero_cost_runtime_bench.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
RESULTS="$WORK/results.tsv"
FAIL=0

cleanup() {
    if [ "$KEEP" = "1" ]; then
        echo "Kept benchmark workdir: $WORK" >&2
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

is_uint() {
    case "$1" in
        ""|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

if ! is_uint "$REPEAT" || [ "$REPEAT" -lt 1 ]; then
    REPEAT=1
fi
if ! is_uint "$N" || [ "$N" -lt 1 ]; then
    N=20000
fi

now_ms() {
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import time; print(int(time.time() * 1000))'
    else
        date +%s000
    fi
}

case_selected() {
    local name="$1" item
    if [ "$CASES" = "all" ] || [ -z "$CASES" ]; then
        return 0
    fi
    old_ifs="$IFS"
    IFS=','
    set -- $CASES
    IFS="$old_ifs"
    for item in "$@"; do
        [ "$item" = "$name" ] && return 0
    done
    return 1
}

run_case() {
    local name="$1" source_rel="$2" expected="$3" iter="$4"
    local source="$SCRIPT_DIR/$source_rel"
    local out="$WORK/${name}_${iter}"
    local build_log="$out.build.log"
    local run_log="$out.run.log"
    local start end build_ms run_ms got status

    if [ ! -f "$source" ]; then
        printf '%s\t%s\t0\t0\tmissing-source\tfail\n' "$name" "$iter" | tee -a "$RESULTS"
        FAIL=$((FAIL + 1))
        return 1
    fi

    start="$(now_ms)"
    if "$XRAY" build --native -O "$OPT" -o "$out" "$source" >"$build_log" 2>&1; then
        status=pass
    else
        status=fail
        FAIL=$((FAIL + 1))
    fi
    end="$(now_ms)"
    build_ms=$((end - start))

    got=""
    run_ms=0
    if [ "$status" = "pass" ]; then
        start="$(now_ms)"
        XRAY_ZERO_COST_BENCH_N="$N" "$out" >"$run_log" 2>&1
        rc=$?
        end="$(now_ms)"
        run_ms=$((end - start))
        got="$(cat "$run_log")"
        if [ "$rc" -ne 0 ]; then
            status=fail
            FAIL=$((FAIL + 1))
        elif [ "$got" != "$expected" ]; then
            status=fail
            FAIL=$((FAIL + 1))
        fi
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$iter" "$build_ms" "$run_ms" "$got" "$status" | tee -a "$RESULTS"
    if [ "$status" != "pass" ]; then
        sed 's/^/    build: /' "$build_log" >&2
        sed 's/^/    run: /' "$run_log" >&2 2>/dev/null || true
    fi
}

printf 'case\titeration\tbuild_ms\trun_ms\toutput\tstatus\n' | tee "$RESULTS"

iter=1
while [ "$iter" -le "$REPEAT" ]; do
    while IFS= read -r raw || [ -n "$raw" ]; do
        line="${raw%$'\r'}"
        case "$line" in
            ""|\#*) continue ;;
        esac
        old_ifs="$IFS"
        IFS='|'
        read -r name source expected extra <<EOF
$line
EOF
        IFS="$old_ifs"
        if [ -n "${extra:-}" ] || [ -z "$name" ] || [ -z "$source" ] || [ -z "$expected" ]; then
            echo "FAIL: bad manifest row: $line" >&2
            FAIL=$((FAIL + 1))
            continue
        fi
        if case_selected "$name"; then
            run_case "$name" "$source" "$expected" "$iter"
        fi
    done < "$MANIFEST"
    iter=$((iter + 1))
done

if [ "$FAIL" -eq 0 ]; then
    echo "Benchmark results: $RESULTS" >&2
    exit 0
fi
echo "Benchmark failed with $FAIL issue(s). Workdir: $WORK" >&2
KEEP=1
exit 1
