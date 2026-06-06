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
RUN_XRAY_AOT=false
JSON_OUTPUT=""
WORKERS_LIST=""
TEST_FILTER=""
ENABLE_SCHED_STATS=false
BENCH_ARGS=""

TESTS=(
    "spawn"
    "pingpong"
    "ring"
    "skynet"
    "fanout"
    "producer_consumer"
    "parallel_sum"
    "work_pool"
    "work_pool_queue"
    "pipeline"
    "select_multiplex"
    "thundering_herd"
    "timeout_storm"
    "sleep_storm"
    "cancel_storm"
    "priority_latency"
)

usage() {
    cat <<EOF
Usage: $0 [--go] [--aot] [--xray-only] [--aot-only] [--all] [--all-backends] [--json FILE] [--workers LIST] [--tests LIST] [--args ARGS] [--sched-stats] [--xray-bin PATH]

Options:
  --go              Run Go benchmarks in addition to xray.
  --aot             Run xray AOT benchmarks in addition to xray.
  --xray-only       Run only xray benchmarks (default).
  --aot-only        Run only xray AOT benchmarks.
  --all             Run xray and Go benchmarks.
  --all-backends    Run xray, xray AOT, and Go benchmarks.
  --json FILE       Also write machine-readable results as JSON.
  --workers LIST    Comma-separated worker counts, e.g. 1,2,4,8.
  --tests LIST      Comma-separated benchmark names.
  --args ARGS       Space-separated benchmark arguments passed to both xray and Go.
  --sched-stats     Enable XRAY_SCHED_STATS=1 for xray runs and include parsed metrics in JSON.
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
        --aot)
            RUN_XRAY_AOT=true
            shift
            ;;
        --xray-only)
            RUN_GO=false
            RUN_XRAY=true
            RUN_XRAY_AOT=false
            shift
            ;;
        --aot-only)
            RUN_GO=false
            RUN_XRAY=false
            RUN_XRAY_AOT=true
            shift
            ;;
        --all)
            RUN_GO=true
            RUN_XRAY=true
            RUN_XRAY_AOT=false
            shift
            ;;
        --all-backends)
            RUN_GO=true
            RUN_XRAY=true
            RUN_XRAY_AOT=true
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
        --args)
            if [ $# -lt 2 ]; then
                echo "Missing value for --args" >&2
                exit 1
            fi
            BENCH_ARGS="$2"
            shift 2
            ;;
        --sched-stats)
            ENABLE_SCHED_STATS=true
            shift
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

if { $RUN_XRAY || $RUN_XRAY_AOT; } && [ ! -f "$XRAY_BIN" ]; then
    echo -e "${RED}错误: xray 未编译或路径不存在: $XRAY_BIN${NC}" >&2
    exit 1
fi

if $RUN_GO && ! command -v go >/dev/null 2>&1; then
    echo -e "${YELLOW}警告: Go 未安装，跳过 Go 测试${NC}"
    RUN_GO=false
fi

TMP_DIR="$(mktemp -d)"
RESULTS_TSV="$TMP_DIR/results.tsv"
RESULT_DELIM=$'\034'
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

json_words_array() {
    local words=()
    if [ -n "${1:-}" ]; then
        read -r -a words <<< "$1"
    fi
    printf '['
    local first_word=true
    for word in "${words[@]}"; do
        if ! $first_word; then
            printf ', '
        fi
        first_word=false
        json_string "$word"
    done
    printf ']'
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
    local metrics=$9
    printf '%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n' \
        "$test_name" "$RESULT_DELIM" \
        "$runtime" "$RESULT_DELIM" \
        "$workers" "$RESULT_DELIM" \
        "$status" "$RESULT_DELIM" \
        "$exit_code" "$RESULT_DELIM" \
        "$wall_ms" "$RESULT_DELIM" \
        "$reported_time_ms" "$RESULT_DELIM" \
        "$throughput" "$RESULT_DELIM" \
        "$metrics" >> "$RESULTS_TSV"
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

extract_kv_metric() {
    local label=$1
    local key=$2
    local file=$3
    awk -v label="$label" -v key="$key" '
        index($0, label) == 1 {
            for (i = 1; i <= NF; i++) {
                if ($i ~ "^" key "=") {
                    value = $i
                    sub("^" key "=", "", value)
                    sub(/[^0-9.-].*$/, "", value)
                    print value
                    exit
                }
            }
        }
    ' "$file"
}

extract_reported_time_ms() {
    local file=$1
    local value
    value=$(extract_first_number "^总时间:|^Total time:|^Total Time:" "$file")
    if [ -n "$value" ]; then
        printf '%s\n' "$value"
        return
    fi
    extract_first_number "^时间:|^Time:" "$file"
}

extract_reported_throughput() {
    local file=$1
    local value
    value=$(extract_first_number "^吞吐量:|^throughput|^Throughput" "$file")
    if [ -n "$value" ]; then
        printf '%s\n' "$value"
        return
    fi
    extract_first_number "^速度:|^Speed:" "$file"
}

metric_pair() {
    local name=$1
    local value=$2
    printf '%s=' "$name"
    if [[ ${value:-} =~ ^-?[0-9]+([.][0-9]+)?$ ]]; then
        printf '%s' "$value"
    else
        printf 'null'
    fi
}

collect_sched_metrics() {
    local file=$1
    local metrics=()
    metrics+=("$(metric_pair dispatch_local_runq "$(extract_kv_metric "Dispatch mix:" "local_runq" "$file")")")
    metrics+=("$(metric_pair dispatch_lifo "$(extract_kv_metric "Dispatch mix:" "lifo" "$file")")")
    metrics+=("$(metric_pair dispatch_steal_direct "$(extract_kv_metric "Dispatch mix:" "steal_direct" "$file")")")
    metrics+=("$(metric_pair dispatch_lifo_share "$(extract_kv_metric "Dispatch mix:" "lifo_share" "$file")")")
    metrics+=("$(metric_pair dispatch_inject_pull "$(extract_kv_metric "Dispatch mix:" "inject_pull" "$file")")")
    metrics+=("$(metric_pair dispatch_stolen_items "$(extract_kv_metric "Dispatch mix:" "stolen_items" "$file")")")
    metrics+=("$(metric_pair dispatch_cont_steal "$(extract_kv_metric "Dispatch mix:" "cont_steal" "$file")")")
    metrics+=("$(metric_pair steal_attempts "$(extract_kv_metric "Steal:" "attempts" "$file")")")
    metrics+=("$(metric_pair steal_success "$(extract_kv_metric "Steal:" "success" "$file")")")
    metrics+=("$(metric_pair steal_success_ratio "$(extract_kv_metric "Steal:" "success_ratio" "$file")")")
    metrics+=("$(metric_pair steal_items_per_success "$(extract_kv_metric "Steal:" "items_per_success" "$file")")")
    metrics+=("$(metric_pair steal_direct_dispatch "$(extract_kv_metric "Steal:" "direct_dispatch" "$file")")")
    metrics+=("$(metric_pair steal_skipped "$(extract_kv_metric "Steal:" "skipped" "$file")")")
    metrics+=("$(metric_pair steal_backoff "$(extract_kv_metric "Steal:" "backoff" "$file")")")
    metrics+=("$(metric_pair steal_no_candidate "$(extract_kv_metric "Steal:" "no_candidate" "$file")")")
    metrics+=("$(metric_pair steal_fresh_reject "$(extract_kv_metric "Steal:" "fresh_reject" "$file")")")
    metrics+=("$(metric_pair steal_candidate_scans "$(extract_kv_metric "Steal:" "candidate_scans" "$file")")")
    metrics+=("$(metric_pair steal_throttle_wait "$(extract_kv_metric "Steal:" "throttle_wait" "$file")")")
    metrics+=("$(metric_pair steal_scans_per_attempt "$(extract_kv_metric "Steal:" "scans_per_attempt" "$file")")")
    metrics+=("$(metric_pair steal_defer_ratio "$(extract_kv_metric "Steal:" "defer_ratio" "$file")")")
    metrics+=("$(metric_pair steal_skip_ratio "$(extract_kv_metric "Steal:" "skip_ratio" "$file")")")
    metrics+=("$(metric_pair runnable_wait_total_ms "$(extract_kv_metric "Runnable wait:" "total_ms" "$file")")")
    metrics+=("$(metric_pair runnable_wait_avg_ms "$(extract_kv_metric "Runnable wait:" "avg_ms" "$file")")")
    metrics+=("$(metric_pair runnable_wait_p50_ms "$(extract_kv_metric "Runnable wait:" "p50_ms" "$file")")")
    metrics+=("$(metric_pair runnable_wait_p95_ms "$(extract_kv_metric "Runnable wait:" "p95_ms" "$file")")")
    metrics+=("$(metric_pair runnable_wait_p99_ms "$(extract_kv_metric "Runnable wait:" "p99_ms" "$file")")")
    metrics+=("$(metric_pair runnable_wait_max_ms "$(extract_kv_metric "Runnable wait:" "max_ms" "$file")")")
    metrics+=("$(metric_pair runnable_wait_dispatches "$(extract_kv_metric "Runnable wait:" "dispatches" "$file")")")
    metrics+=("$(metric_pair priority_boost "$(extract_kv_metric "Runnable wait:" "priority_boost" "$file")")")
    metrics+=("$(metric_pair lifo_gate_total "$(extract_kv_metric "LIFO gate:" "total" "$file")")")
    metrics+=("$(metric_pair lifo_gate_budget "$(extract_kv_metric "LIFO gate:" "budget" "$file")")")
    metrics+=("$(metric_pair lifo_gate_backlog "$(extract_kv_metric "LIFO gate:" "backlog" "$file")")")
    metrics+=("$(metric_pair lifo_gate_priority "$(extract_kv_metric "LIFO gate:" "priority" "$file")")")
    metrics+=("$(metric_pair lifo_gate_ratio "$(extract_kv_metric "LIFO gate:" "gate_ratio" "$file")")")
    metrics+=("$(metric_pair fast_dispatch_hits "$(extract_kv_metric "Fast dispatch:" "hits" "$file")")")
    metrics+=("$(metric_pair fast_dispatch_budget_stop "$(extract_kv_metric "Fast dispatch:" "budget_stop" "$file")")")
    metrics+=("$(metric_pair fast_dispatch_empty "$(extract_kv_metric "Fast dispatch:" "empty" "$file")")")
    metrics+=("$(metric_pair fast_dispatch_hit_share "$(extract_kv_metric "Fast dispatch:" "hit_share" "$file")")")
    metrics+=("$(metric_pair coro_pool_deferred_recycle "$(extract_kv_metric "Coro pool:" "deferred_recycle" "$file")")")
    metrics+=("$(metric_pair coro_pool_local_get "$(extract_kv_metric "Coro pool:" "local_get" "$file")")")
    metrics+=("$(metric_pair coro_pool_global_free_get "$(extract_kv_metric "Coro pool:" "global_free_get" "$file")")")
    metrics+=("$(metric_pair coro_pool_arena_cache_get "$(extract_kv_metric "Coro pool:" "arena_cache_get" "$file")")")
    metrics+=("$(metric_pair coro_pool_arena_batch_get "$(extract_kv_metric "Coro pool:" "arena_batch_get" "$file")")")
    metrics+=("$(metric_pair coro_pool_miss "$(extract_kv_metric "Coro pool:" "miss" "$file")")")
    metrics+=("$(metric_pair coro_pool_local_put "$(extract_kv_metric "Coro pool:" "local_put" "$file")")")
    metrics+=("$(metric_pair coro_pool_global_return "$(extract_kv_metric "Coro pool:" "global_return" "$file")")")
    metrics+=("$(metric_pair coro_pool_local_free "$(extract_kv_metric "Coro pool:" "local_free" "$file")")")
    metrics+=("$(metric_pair chan_wake_alloc "$(extract_kv_metric "Channel wake commands:" "alloc" "$file")")")
    metrics+=("$(metric_pair chan_wake_free "$(extract_kv_metric "Channel wake commands:" "free" "$file")")")
    metrics+=("$(metric_pair chan_wake_dispatch "$(extract_kv_metric "Channel wake commands:" "dispatch" "$file")")")
    metrics+=("$(metric_pair chan_wake_drain "$(extract_kv_metric "Channel wake commands:" "drain" "$file")")")
    metrics+=("$(metric_pair chan_wake_coalesce "$(extract_kv_metric "Channel wake commands:" "coalesce" "$file")")")
    metrics+=("$(metric_pair chan_wake_stale "$(extract_kv_metric "Channel wake commands:" "stale" "$file")")")
    metrics+=("$(metric_pair chan_wake_forward "$(extract_kv_metric "Channel wake commands:" "forward" "$file")")")
    metrics+=("$(metric_pair chan_wake_cross_worker "$(extract_kv_metric "Channel wake diagnostics:" "cross_worker" "$file")")")
    metrics+=("$(metric_pair chan_wake_drain_per_dispatch "$(extract_kv_metric "Channel wake diagnostics:" "drain_per_dispatch" "$file")")")
    metrics+=("$(metric_pair chan_wake_coalesce_ratio "$(extract_kv_metric "Channel wake diagnostics:" "coalesce_ratio" "$file")")")
    metrics+=("$(metric_pair chan_wake_stale_ratio "$(extract_kv_metric "Channel wake diagnostics:" "stale_ratio" "$file")")")
    metrics+=("$(metric_pair chan_wake_forward_per_dispatch "$(extract_kv_metric "Channel wake diagnostics:" "forward_per_dispatch" "$file")")")
    metrics+=("$(metric_pair chan_no_waiter_buffer "$(extract_kv_metric "Channel hot path:" "no_waiter_buffer" "$file")")")
    metrics+=("$(metric_pair chan_kind_spsc "$(extract_kv_metric "Channel logical shape transitions:" "spsc" "$file")")")
    metrics+=("$(metric_pair chan_kind_mpsc "$(extract_kv_metric "Channel logical shape transitions:" "mpsc" "$file")")")
    metrics+=("$(metric_pair chan_kind_work_queue "$(extract_kv_metric "Channel logical shape transitions:" "work_queue" "$file")")")
    metrics+=("$(metric_pair chan_kind_mpmc "$(extract_kv_metric "Channel logical shape transitions:" "mpmc" "$file")")")
    metrics+=("$(metric_pair chan_worker_kind_spsc "$(extract_kv_metric "Channel worker shape transitions:" "spsc" "$file")")")
    metrics+=("$(metric_pair chan_worker_kind_mpsc "$(extract_kv_metric "Channel worker shape transitions:" "mpsc" "$file")")")
    metrics+=("$(metric_pair chan_worker_kind_work_queue "$(extract_kv_metric "Channel worker shape transitions:" "work_queue" "$file")")")
    metrics+=("$(metric_pair chan_worker_kind_mpmc "$(extract_kv_metric "Channel worker shape transitions:" "mpmc" "$file")")")
    metrics+=("$(metric_pair chan_kind_generic_op "$(extract_kv_metric "Channel logical ops:" "generic" "$file")")")
    metrics+=("$(metric_pair chan_kind_rendezvous_op "$(extract_kv_metric "Channel logical ops:" "rendezvous" "$file")")")
    metrics+=("$(metric_pair chan_kind_spsc_op "$(extract_kv_metric "Channel logical ops:" "spsc" "$file")")")
    metrics+=("$(metric_pair chan_kind_mpsc_op "$(extract_kv_metric "Channel logical ops:" "mpsc" "$file")")")
    metrics+=("$(metric_pair chan_kind_work_queue_op "$(extract_kv_metric "Channel logical ops:" "work_queue" "$file")")")
    metrics+=("$(metric_pair chan_kind_mpmc_op "$(extract_kv_metric "Channel logical ops:" "mpmc" "$file")")")
    metrics+=("$(metric_pair chan_worker_kind_generic_op "$(extract_kv_metric "Channel worker-shape ops:" "generic" "$file")")")
    metrics+=("$(metric_pair chan_worker_kind_rendezvous_op "$(extract_kv_metric "Channel worker-shape ops:" "rendezvous" "$file")")")
    metrics+=("$(metric_pair chan_worker_kind_spsc_op "$(extract_kv_metric "Channel worker-shape ops:" "spsc" "$file")")")
    metrics+=("$(metric_pair chan_worker_kind_mpsc_op "$(extract_kv_metric "Channel worker-shape ops:" "mpsc" "$file")")")
    metrics+=("$(metric_pair chan_worker_kind_work_queue_op "$(extract_kv_metric "Channel worker-shape ops:" "work_queue" "$file")")")
    metrics+=("$(metric_pair chan_worker_kind_mpmc_op "$(extract_kv_metric "Channel worker-shape ops:" "mpmc" "$file")")")
    metrics+=("$(metric_pair chan_lock_fast "$(extract_kv_metric "Channel lock:" "fast" "$file")")")
    metrics+=("$(metric_pair chan_lock_contended "$(extract_kv_metric "Channel lock:" "contended" "$file")")")
    metrics+=("$(metric_pair chan_lock_slow "$(extract_kv_metric "Channel lock:" "slow" "$file")")")
    metrics+=("$(metric_pair chan_lock_contention_pct "$(extract_kv_metric "Channel lock:" "contention" "$file")")")
    metrics+=("$(metric_pair chan_lock_recheck "$(extract_kv_metric "Channel lock wait:" "recheck" "$file")")")
    metrics+=("$(metric_pair chan_lock_spin "$(extract_kv_metric "Channel lock wait:" "spin" "$file")")")
    metrics+=("$(metric_pair chan_lock_yield "$(extract_kv_metric "Channel lock wait:" "yield" "$file")")")
    metrics+=("$(metric_pair chan_lock_sleep "$(extract_kv_metric "Channel lock wait:" "sleep" "$file")")")
    metrics+=("$(metric_pair chan_lock_sleep_rate "$(extract_kv_metric "Channel lock wait:" "sleep_rate" "$file")")")
    metrics+=("$(metric_pair chan_buffer_fast_try "$(extract_kv_metric "Channel buffered fast path:" "try" "$file")")")
    metrics+=("$(metric_pair chan_buffer_fast_hit "$(extract_kv_metric "Channel buffered fast path:" "hit" "$file")")")
    metrics+=("$(metric_pair chan_buffer_fast_miss "$(extract_kv_metric "Channel buffered fast path:" "miss" "$file")")")
    metrics+=("$(metric_pair chan_buffer_fast_busy "$(extract_kv_metric "Channel buffered fast path:" "busy" "$file")")")
    metrics+=("$(metric_pair chan_buffer_fast_hit_rate "$(extract_kv_metric "Channel buffered fast path:" "hit_rate" "$file")")")
    metrics+=("$(metric_pair chan_buffer_fast_busy_rate "$(extract_kv_metric "Channel buffered fast path:" "busy_rate" "$file")")")
    metrics+=("$(metric_pair chan_buffer_fast_miss_rate "$(extract_kv_metric "Channel buffered fast path:" "miss_rate" "$file")")")
    metrics+=("$(metric_pair chan_send_direct "$(extract_kv_metric "Channel ops:" "send_direct" "$file")")")
    metrics+=("$(metric_pair chan_recv_direct "$(extract_kv_metric "Channel ops:" "recv_direct" "$file")")")
    metrics+=("$(metric_pair chan_send_buffer "$(extract_kv_metric "Channel ops:" "send_buffer" "$file")")")
    metrics+=("$(metric_pair chan_recv_buffer "$(extract_kv_metric "Channel ops:" "recv_buffer" "$file")")")
    metrics+=("$(metric_pair chan_send_block "$(extract_kv_metric "Channel ops:" "send_block" "$file")")")
    metrics+=("$(metric_pair chan_recv_block "$(extract_kv_metric "Channel ops:" "recv_block" "$file")")")
    metrics+=("$(metric_pair chan_sendq_dequeue "$(extract_kv_metric "Channel waitq:" "sendq_dequeue" "$file")")")
    metrics+=("$(metric_pair chan_recvq_dequeue "$(extract_kv_metric "Channel waitq:" "recvq_dequeue" "$file")")")
    metrics+=("$(metric_pair chan_ready_wake "$(extract_kv_metric "Channel waitq:" "ready_wake" "$file")")")
    metrics+=("$(metric_pair chan_close_ready_wake "$(extract_kv_metric "Channel waitq:" "close_ready_wake" "$file")")")
    metrics+=("$(metric_pair chan_close_send_waiter "$(extract_kv_metric "Channel waitq:" "close_send" "$file")")")
    metrics+=("$(metric_pair chan_close_recv_waiter "$(extract_kv_metric "Channel waitq:" "close_recv" "$file")")")
    metrics+=("$(metric_pair chan_close_deferred_send "$(extract_kv_metric "Channel close fanout:" "deferred_send" "$file")")")
    metrics+=("$(metric_pair chan_close_deferred_recv "$(extract_kv_metric "Channel close fanout:" "deferred_recv" "$file")")")
    metrics+=("$(metric_pair chan_close_local_workers "$(extract_kv_metric "Channel close fanout:" "local_workers" "$file")")")
    metrics+=("$(metric_pair chan_close_remote_workers "$(extract_kv_metric "Channel close fanout:" "remote_workers" "$file")")")
    metrics+=("$(metric_pair work_queue_push "$(extract_kv_metric "WorkQueue:" "push" "$file")")")
    metrics+=("$(metric_pair work_queue_pop_local "$(extract_kv_metric "WorkQueue:" "pop_local" "$file")")")
    metrics+=("$(metric_pair work_queue_pop_steal "$(extract_kv_metric "WorkQueue:" "pop_steal" "$file")")")
    metrics+=("$(metric_pair work_queue_pop_empty "$(extract_kv_metric "WorkQueue:" "pop_empty" "$file")")")
    metrics+=("$(metric_pair work_queue_steal_share "$(extract_kv_metric "WorkQueue:" "steal_share" "$file")")")
    metrics+=("$(metric_pair work_queue_block "$(extract_kv_metric "WorkQueue:" "block" "$file")")")
    metrics+=("$(metric_pair work_queue_wake "$(extract_kv_metric "WorkQueue:" "wake" "$file")")")
    metrics+=("$(metric_pair work_queue_close "$(extract_kv_metric "WorkQueue:" "close" "$file")")")
    metrics+=("$(metric_pair work_queue_close_wake "$(extract_kv_metric "WorkQueue:" "close_wake" "$file")")")
    metrics+=("$(metric_pair timeout_yield_retry "$(extract_kv_metric "Timeout:" "yield_retry" "$file")")")
    metrics+=("$(metric_pair timeout_event_block "$(extract_kv_metric "Timeout:" "event_block" "$file")")")
    metrics+=("$(metric_pair timer_fire "$(extract_kv_metric "Timer:" "fire" "$file")")")
    metrics+=("$(metric_pair timer_cancel_local "$(extract_kv_metric "Timer:" "cancel_local" "$file")")")
    metrics+=("$(metric_pair timer_cancel_remote "$(extract_kv_metric "Timer:" "cancel_remote" "$file")")")
    metrics+=("$(metric_pair timer_cancel_process "$(extract_kv_metric "Timer:" "cancel_process" "$file")")")
    metrics+=("$(metric_pair timer_cancel_duplicate "$(extract_kv_metric "Timer cancel queue:" "duplicate" "$file")")")
    metrics+=("$(metric_pair timer_cancel_drain_batches "$(extract_kv_metric "Timer cancel queue:" "drain_batches" "$file")")")
    metrics+=("$(metric_pair timer_cancel_drain_nodes "$(extract_kv_metric "Timer cancel queue:" "drain_nodes" "$file")")")
    metrics+=("$(metric_pair timer_cancel_drain_max "$(extract_kv_metric "Timer cancel queue:" "drain_max" "$file")")")
    metrics+=("$(metric_pair handoff_reuse "$(extract_kv_metric "Handoff:" "reuse" "$file")")")
    metrics+=("$(metric_pair handoff_create "$(extract_kv_metric "Handoff:" "create" "$file")")")
    metrics+=("$(metric_pair handoff_cap_hit "$(extract_kv_metric "Handoff:" "cap_hit" "$file")")")
    metrics+=("$(metric_pair inject_push "$(extract_kv_metric "Inject:" "push" "$file")")")
    metrics+=("$(metric_pair inject_pop "$(extract_kv_metric "Inject:" "pop" "$file")")")
    metrics+=("$(metric_pair inject_spill "$(extract_kv_metric "Inject:" "spill" "$file")")")
    metrics+=("$(metric_pair inject_push_batches "$(extract_kv_metric "Inject:" "push_batches" "$file")")")
    metrics+=("$(metric_pair inject_pop_batches "$(extract_kv_metric "Inject:" "pop_batches" "$file")")")
    metrics+=("$(metric_pair inject_len "$(extract_kv_metric "Inject:" "len" "$file")")")
    metrics+=("$(metric_pair inject_pop_push_ratio "$(extract_kv_metric "Inject diagnostics:" "pop_push_ratio" "$file")")")
    metrics+=("$(metric_pair inject_spill_ratio "$(extract_kv_metric "Inject diagnostics:" "spill_ratio" "$file")")")
    metrics+=("$(metric_pair inject_pending_est "$(extract_kv_metric "Inject diagnostics:" "pending_est" "$file")")")
    metrics+=("$(metric_pair inject_worker_pull "$(extract_kv_metric "Inject diagnostics:" "worker_pull" "$file")")")
    metrics+=("$(metric_pair inject_pull_pop_delta "$(extract_kv_metric "Inject diagnostics:" "pull_pop_delta" "$file")")")
    metrics+=("$(metric_pair inject_push_items_per_batch "$(extract_kv_metric "Inject diagnostics:" "push_items_per_batch" "$file")")")
    metrics+=("$(metric_pair inject_pop_items_per_batch "$(extract_kv_metric "Inject diagnostics:" "pop_items_per_batch" "$file")")")
    metrics+=("$(metric_pair async_submit "$(extract_kv_metric "Async:" "submit" "$file")")")
    metrics+=("$(metric_pair async_complete "$(extract_kv_metric "Async:" "complete" "$file")")")
    metrics+=("$(metric_pair async_reject "$(extract_kv_metric "Async:" "reject" "$file")")")
    local IFS=';'
    printf '%s' "${metrics[*]}"
}

run_one() {
    local test_name=$1
    local runtime=$2
    local workers=$3
    local test_dir="$BENCHMARK_DIR/$test_name"
    local out_file="$TMP_DIR/${test_name}_${runtime}_${workers:-default}.log"
    local exit_code=0
    local start_ms end_ms wall_ms reported_time_ms throughput status metrics
    local bench_args=()
    if [ -n "$BENCH_ARGS" ]; then
        read -r -a bench_args <<< "$BENCH_ARGS"
    fi

    start_ms=$(now_ms)
    set +e
    if [ "$runtime" = "xray" ]; then
        local xr_file="$test_dir/$test_name.xr"
        if [ ! -f "$xr_file" ]; then
            set -e
            record_result "$test_name" "$runtime" "$workers" "missing" 127 0 "" "" ""
            echo -e "${YELLOW}跳过: $xr_file 不存在${NC}"
            return
        fi
        local xray_cmd=("$XRAY_BIN" run "$xr_file")
        if [ ${#bench_args[@]} -gt 0 ]; then
            xray_cmd+=(-- "${bench_args[@]}")
        fi
        if [ "$ENABLE_SCHED_STATS" = true ] && [ -n "$workers" ]; then
            XRAY_WORKERS="$workers" XRAY_SCHED_STATS=1 "${xray_cmd[@]}" > "$out_file" 2>&1
        elif [ "$ENABLE_SCHED_STATS" = true ]; then
            XRAY_SCHED_STATS=1 "${xray_cmd[@]}" > "$out_file" 2>&1
        elif [ -n "$workers" ]; then
            XRAY_WORKERS="$workers" "${xray_cmd[@]}" > "$out_file" 2>&1
        else
            "${xray_cmd[@]}" > "$out_file" 2>&1
        fi
        exit_code=$?
    elif [ "$runtime" = "xray-aot" ]; then
        local xr_file="$test_dir/$test_name.xr"
        if [ ! -f "$xr_file" ]; then
            set -e
            record_result "$test_name" "$runtime" "$workers" "missing" 127 0 "" "" ""
            echo -e "${YELLOW}跳过: $xr_file 不存在${NC}"
            return
        fi
        local aot_bin="$TMP_DIR/${test_name}_${workers:-default}_aot"
        local build_log="$TMP_DIR/${test_name}_${workers:-default}_aot_build.log"
        if ! "$XRAY_BIN" build --native "$xr_file" -o "$aot_bin" > "$build_log" 2>&1; then
            set -e
            end_ms=$(now_ms)
            wall_ms=$((end_ms - start_ms))
            cat "$build_log"
            record_result "$test_name" "$runtime" "$workers" "build-fail" 125 "$wall_ms" "" "" ""
            echo -e "${RED}${runtime} 构建失败${NC}"
            return
        fi
        start_ms=$(now_ms)
        if [ "$ENABLE_SCHED_STATS" = true ] && [ -n "$workers" ]; then
            XRAY_WORKERS="$workers" XRAY_SCHED_STATS=1 "$aot_bin" "${bench_args[@]}" > "$out_file" 2>&1
        elif [ "$ENABLE_SCHED_STATS" = true ]; then
            XRAY_SCHED_STATS=1 "$aot_bin" "${bench_args[@]}" > "$out_file" 2>&1
        elif [ -n "$workers" ]; then
            XRAY_WORKERS="$workers" "$aot_bin" "${bench_args[@]}" > "$out_file" 2>&1
        else
            "$aot_bin" "${bench_args[@]}" > "$out_file" 2>&1
        fi
        exit_code=$?
    else
        local go_file="$test_dir/$test_name.go"
        if [ ! -f "$go_file" ]; then
            set -e
            record_result "$test_name" "$runtime" "$workers" "missing" 127 0 "" "" ""
            echo -e "${YELLOW}跳过: $go_file 不存在${NC}"
            return
        fi
        local go_bin="$TMP_DIR/${test_name}_${workers:-default}_go"
        local build_log="$TMP_DIR/${test_name}_${workers:-default}_go_build.log"
        if ! (cd "$test_dir" && go build -o "$go_bin" "$test_name.go") > "$build_log" 2>&1; then
            set -e
            end_ms=$(now_ms)
            wall_ms=$((end_ms - start_ms))
            cat "$build_log"
            record_result "$test_name" "$runtime" "$workers" "build-fail" 125 "$wall_ms" "" "" ""
            echo -e "${RED}${runtime} 构建失败${NC}"
            return
        fi
        local go_cmd=("$go_bin")
        if [ ${#bench_args[@]} -gt 0 ]; then
            go_cmd+=("${bench_args[@]}")
        fi
        start_ms=$(now_ms)
        if [ -n "$workers" ]; then
            (cd "$test_dir" && GOMAXPROCS="$workers" "${go_cmd[@]}") > "$out_file" 2>&1
        else
            (cd "$test_dir" && "${go_cmd[@]}") > "$out_file" 2>&1
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

    reported_time_ms=$(extract_reported_time_ms "$out_file")
    throughput=$(extract_reported_throughput "$out_file")
    if { [ "$runtime" = "xray" ] || [ "$runtime" = "xray-aot" ]; } &&
        [ "$ENABLE_SCHED_STATS" = true ]; then
        metrics=$(collect_sched_metrics "$out_file")
    else
        metrics=""
    fi
    record_result "$test_name" "$runtime" "$workers" "$status" "$exit_code" "$wall_ms" \
        "$reported_time_ms" "$throughput" "$metrics"
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
        printf '  "benchmark_args": '
        json_words_array "$BENCH_ARGS"
        printf ',\n'
        printf '  "run_xray": %s,\n' "$RUN_XRAY"
        printf '  "run_xray_aot": %s,\n' "$RUN_XRAY_AOT"
        printf '  "run_go": %s,\n' "$RUN_GO"
        printf '  "sched_stats": %s,\n' "$ENABLE_SCHED_STATS"
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
        while IFS="$RESULT_DELIM" read -r test_name runtime workers status exit_code wall_ms reported_time_ms throughput metrics; do
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
            printf ', "metrics": {'
            if [ -n "$metrics" ]; then
                local first_metric=true
                local metric_items=()
                local old_ifs=$IFS
                IFS=';' read -r -a metric_items <<< "$metrics"
                IFS=$old_ifs
                for item in "${metric_items[@]}"; do
                    local metric_name=${item%%=*}
                    local metric_value=${item#*=}
                    if ! $first_metric; then
                        printf ','
                    fi
                    first_metric=false
                    printf ' '
                    json_string "$metric_name"
                    printf ': '
                    json_number_or_null "$metric_value"
                done
                if ! $first_metric; then
                    printf ' '
                fi
            fi
            printf '}'
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
if $RUN_XRAY_AOT; then
    echo "xray AOT: enabled"
fi
if [ -n "$WORKERS_LIST" ]; then
    echo "workers: $WORKERS_LIST"
fi
if [ "$ENABLE_SCHED_STATS" = true ]; then
    echo "scheduler stats: enabled"
fi
if [ -n "$BENCH_ARGS" ]; then
    echo "args: $BENCH_ARGS"
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

    if $RUN_XRAY_AOT; then
        for workers in "${WORKERS[@]}"; do
            if [ -n "$workers" ]; then
                echo -e "${YELLOW}[xray AOT workers=$workers]${NC}"
            else
                echo -e "${YELLOW}[xray AOT]${NC}"
            fi
            run_one "$test" "xray-aot" "$workers"
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
