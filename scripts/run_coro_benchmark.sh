#!/bin/bash
# 作者：xingleixu@gmail.com
# 协程性能对比测试运行脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BENCHMARK_DIR="$PROJECT_DIR/tests/benchmarks/coro"
XRAY_BIN="${XRAY_BIN:-${XRAY_BUILD_DIR:-$PROJECT_DIR/build}/xray}"

# 颜色输出（non-TTY / NO_COLOR 时自动关闭）
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    NC='\033[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    NC=''
fi

RUN_GO=false
RUN_XRAY=true
JSON_OUTPUT=""
WORKERS_LIST=""
TEST_FILTER=""

TESTS=(
    "spawn"
    "pingpong"
    "ring"
    "skynet"
    "fanout"
    "producer_consumer"
    "parallel_sum"
    "work_pool"
    "select_multiplex"
    "thundering_herd"
    "timeout_storm"
    "sleep_storm"
)

usage() {
    cat <<EOF
Usage: $0 [--go] [--xray-only] [--all] [--json FILE] [--workers LIST] [--tests LIST] [--xray-bin PATH]

Options:
  --go              Run Go benchmarks in addition to xray.
  --xray-only       Run only xray benchmarks (default).
  --all             Run xray and Go benchmarks.
  --json FILE       Also write machine-readable results as JSON.
  --workers LIST    Comma-separated worker counts, e.g. 1,2,4,8.
  --tests LIST      Comma-separated benchmark names.
  --xray-bin PATH   Override xray executable path.
  --list            Print benchmark names and exit.
EOF
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --go)
            RUN_GO=true
            shift
            ;;
        --xray-only)
            RUN_GO=false
            RUN_XRAY=true
            shift
            ;;
        --all)
            RUN_GO=true
            RUN_XRAY=true
            shift
            ;;
        --json)
            if [ $# -lt 2 ]; then
                echo "Missing value for --json" >&2
                exit 1
            fi
            JSON_OUTPUT="$2"
            shift 2
            ;;
        --workers)
            if [ $# -lt 2 ]; then
                echo "Missing value for --workers" >&2
                exit 1
            fi
            WORKERS_LIST="$2"
            shift 2
            ;;
        --tests)
            if [ $# -lt 2 ]; then
                echo "Missing value for --tests" >&2
                exit 1
            fi
            TEST_FILTER="$2"
            shift 2
            ;;
        --xray-bin)
            if [ $# -lt 2 ]; then
                echo "Missing value for --xray-bin" >&2
                exit 1
            fi
            XRAY_BIN="$2"
            shift 2
            ;;
        --list)
            printf '%s\n' "${TESTS[@]}"
            exit 0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [ -n "$TEST_FILTER" ]; then
    IFS=',' read -r -a TESTS <<< "$TEST_FILTER"
fi

if [ -n "$WORKERS_LIST" ]; then
    IFS=',' read -r -a WORKERS <<< "$WORKERS_LIST"
else
    WORKERS=("")
fi

if $RUN_XRAY && [ ! -f "$XRAY_BIN" ]; then
    echo -e "${RED}错误: xray 未编译或路径不存在: $XRAY_BIN${NC}" >&2
    exit 1
fi

if $RUN_GO && ! command -v go >/dev/null 2>&1; then
    echo -e "${YELLOW}警告: Go 未安装，跳过 Go 测试${NC}"
    RUN_GO=false
fi

TMP_DIR="$(mktemp -d)"
RESULTS_TSV="$TMP_DIR/results.tsv"
trap 'rm -rf "$TMP_DIR"' EXIT

now_ms() {
    perl -MTime::HiRes=time -e 'printf "%.0f\n", time() * 1000' 2>/dev/null ||
        ruby -e 'puts (Time.now.to_f * 1000).round' 2>/dev/null ||
        awk 'BEGIN { srand(); print srand() * 1000 }'
}

json_string() {
    local s=${1//\\/\\\\}
    s=${s//\"/\\\"}
    s=${s//$'\n'/\\n}
    s=${s//$'\r'/\\r}
    s=${s//$'\t'/\\t}
    printf '"%s"' "$s"
}

json_number_or_null() {
    if [[ ${1:-} =~ ^-?[0-9]+([.][0-9]+)?$ ]]; then
        printf '%s' "$1"
    else
        printf 'null'
    fi
}

record_result() {
    local test_name=$1
    local runtime=$2
    local workers=$3
    local status=$4
    local exit_code=$5
    local wall_ms=$6
    local reported_time_ms=$7
    local throughput=$8
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$test_name" "$runtime" "$workers" "$status" \
        "$exit_code" "$wall_ms" "$reported_time_ms" "$throughput" >> "$RESULTS_TSV"
}

extract_first_number() {
    local pattern=$1
    local file=$2
    awk -v pat="$pattern" '
        $0 ~ pat {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^-?[0-9]+([.][0-9]+)?$/) {
                    print $i
                    exit
                }
            }
        }
    ' "$file"
}

run_one() {
    local test_name=$1
    local runtime=$2
    local workers=$3
    local test_dir="$BENCHMARK_DIR/$test_name"
    local out_file="$TMP_DIR/${test_name}_${runtime}_${workers:-default}.log"
    local exit_code=0
    local start_ms end_ms wall_ms reported_time_ms throughput status

    start_ms=$(now_ms)
    set +e
    if [ "$runtime" = "xray" ]; then
        local xr_file="$test_dir/$test_name.xr"
        if [ ! -f "$xr_file" ]; then
            set -e
            record_result "$test_name" "$runtime" "$workers" "missing" 127 0 "" ""
            echo -e "${YELLOW}跳过: $xr_file 不存在${NC}"
            return
        fi
        if [ -n "$workers" ]; then
            XRAY_WORKERS="$workers" "$XRAY_BIN" run "$xr_file" > "$out_file" 2>&1
        else
            "$XRAY_BIN" run "$xr_file" > "$out_file" 2>&1
        fi
        exit_code=$?
    else
        local go_file="$test_dir/$test_name.go"
        if [ ! -f "$go_file" ]; then
            set -e
            record_result "$test_name" "$runtime" "$workers" "missing" 127 0 "" ""
            echo -e "${YELLOW}跳过: $go_file 不存在${NC}"
            return
        fi
        if [ -n "$workers" ]; then
            (cd "$test_dir" && GOMAXPROCS="$workers" go run "$test_name.go") > "$out_file" 2>&1
        else
            (cd "$test_dir" && go run "$test_name.go") > "$out_file" 2>&1
        fi
        exit_code=$?
    fi
    set -e
    end_ms=$(now_ms)
    wall_ms=$((end_ms - start_ms))

    cat "$out_file"
    if [ "$exit_code" -eq 0 ]; then
        status="ok"
    else
        status="fail"
        echo -e "${RED}${runtime} 测试失败: exit=$exit_code${NC}"
    fi

    reported_time_ms=$(extract_first_number "时间:|Time:" "$out_file")
    throughput=$(extract_first_number "吞吐量:|throughput|Throughput" "$out_file")
    record_result "$test_name" "$runtime" "$workers" "$status" "$exit_code" "$wall_ms" \
        "$reported_time_ms" "$throughput"
}

write_json_results() {
    local output=$1
    mkdir -p "$(dirname "$output")"
    {
        echo "{"
        printf '  "schema": "xray.coro_benchmark.v1",\n'
        printf '  "generated_at": '
        json_string "$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
        printf ',\n'
        printf '  "xray_bin": '
        json_string "$XRAY_BIN"
        printf ',\n'
        printf '  "run_xray": %s,\n' "$RUN_XRAY"
        printf '  "run_go": %s,\n' "$RUN_GO"
        printf '  "workers": '
        if [ -z "$WORKERS_LIST" ]; then
            printf 'null'
        else
            printf '['
            local first_worker=true
            for w in "${WORKERS[@]}"; do
                if ! $first_worker; then
                    printf ', '
                fi
                first_worker=false
                json_number_or_null "$w"
            done
            printf ']'
        fi
        printf ',\n'
        printf '  "results": [\n'
        local first=true
        while IFS=$'\t' read -r test_name runtime workers status exit_code wall_ms reported_time_ms throughput; do
            if ! $first; then
                printf ',\n'
            fi
            first=false
            printf '    { "test": '
            json_string "$test_name"
            printf ', "runtime": '
            json_string "$runtime"
            printf ', "workers": '
            json_number_or_null "$workers"
            printf ', "status": '
            json_string "$status"
            printf ', "exit_code": '
            json_number_or_null "$exit_code"
            printf ', "wall_ms": '
            json_number_or_null "$wall_ms"
            printf ', "reported_time_ms": '
            json_number_or_null "$reported_time_ms"
            printf ', "throughput": '
            json_number_or_null "$throughput"
            printf ' }'
        done < "$RESULTS_TSV"
        printf '\n  ]\n'
        echo "}"
    } > "$output"
}

echo "=========================================="
echo "  xray vs Go 协程性能对比测试"
echo "=========================================="
echo "xray: $XRAY_BIN"
if [ -n "$WORKERS_LIST" ]; then
    echo "workers: $WORKERS_LIST"
fi
echo ""

for test in "${TESTS[@]}"; do
    echo -e "${GREEN}>>> $test${NC}"
    echo "----------------------------------------"

    if $RUN_XRAY; then
        for workers in "${WORKERS[@]}"; do
            if [ -n "$workers" ]; then
                echo -e "${YELLOW}[xray workers=$workers]${NC}"
            else
                echo -e "${YELLOW}[xray]${NC}"
            fi
            run_one "$test" "xray" "$workers"
            echo ""
        done
    fi

    if $RUN_GO; then
        for workers in "${WORKERS[@]}"; do
            if [ -n "$workers" ]; then
                echo -e "${YELLOW}[Go GOMAXPROCS=$workers]${NC}"
            else
                echo -e "${YELLOW}[Go]${NC}"
            fi
            run_one "$test" "go" "$workers"
            echo ""
        done
    fi

    echo ""
done

if [ -n "$JSON_OUTPUT" ]; then
    write_json_results "$JSON_OUTPUT"
    echo "JSON: $JSON_OUTPUT"
fi

echo "=========================================="
echo "  测试完成"
echo "=========================================="
