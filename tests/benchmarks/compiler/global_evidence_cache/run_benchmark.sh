#!/bin/bash
# Compile-time benchmark for global evidence cache phases.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
CASES="${XRAY_GLOBAL_EVIDENCE_BENCH_CASES:-class,generic,closure,static,capability}"
REPEAT="${XRAY_GLOBAL_EVIDENCE_BENCH_REPEAT:-1}"
KEEP="${XRAY_GLOBAL_EVIDENCE_BENCH_KEEP:-0}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_global_evidence_cache_bench.XXXXXX")" || {
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

now_ms() {
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import time; print(int(time.time() * 1000))'
    else
        date +%s000
    fi
}

elapsed_ms() {
    local start="$1" end="$2"
    echo $((end - start))
}

write_class_case() {
    local file="$1" variant="$2" extra="$3"
    local decl_extra=""
    local body_expr="$extra"
    if [ "$variant" = "decl" ]; then
        decl_extra="
fn added_decl(x: int) -> int {
    return x + 1
}
"
    fi
    if [ "$variant" != "base" ]; then
        body_expr="tag()"
    fi
    cat >"$file" <<XR_EOF
interface Shape {
    area() -> int
}

class Circle implements Shape {
    r: int
    constructor(r: int) {
        this.r = r
    }
    area() -> int {
        return this.r * this.r + $body_expr
    }
}

class Square implements Shape {
    side: int
    constructor(side: int) {
        this.side = side
    }
    area() -> int {
        return this.side * this.side
    }
}

fn score(shape: Shape) -> int {
    return shape.area()
}

fn tag() -> int {
    return $extra
}

$decl_extra
print(score(Circle(3)) + score(Square(4)))
XR_EOF
}

write_generic_case() {
    local file="$1" variant="$2" extra="$3"
    local decl_extra=""
    local body_expr="0"
    if [ "$variant" = "decl" ]; then
        decl_extra="
fn added_decl(x: int) -> int {
    return x + 1
}
"
    fi
    if [ "$variant" != "base" ]; then
        body_expr="tag()"
    fi
    cat >"$file" <<XR_EOF
class Box<T> {
    value: T
    constructor(value: T) {
        this.value = value
    }
    get() -> T {
        return this.value
    }
}

fn id<T>(x: T) -> T {
    return x
}

fn tag() -> int {
    return $extra
}

fn wrapper() -> int {
    var box = Box<int>(7)
    return id<int>(box.get()) + $body_expr
}

$decl_extra
print(wrapper())
XR_EOF
}

write_closure_case() {
    local file="$1" variant="$2" extra="$3"
    local decl_extra=""
    local body_expr="$extra"
    if [ "$variant" = "decl" ]; then
        decl_extra="
fn added_decl(x: int) -> int {
    return x + 1
}
"
    fi
    if [ "$variant" != "base" ]; then
        body_expr="tag()"
    fi
    cat >"$file" <<XR_EOF
fn tag() -> int {
    return $extra
}

fn directClosure() -> int {
    const f = fn() -> int {
        return 41 + $body_expr
    }
    return f() + 1
}

fn captured(seed: int) -> int {
    const f = fn() -> int {
        return seed + 2
    }
    return f()
}

$decl_extra
print(directClosure() + captured(1))
XR_EOF
}

write_static_case() {
    local file="$1" variant="$2" extra="$3"
    local decl_extra=""
    local body_expr="0"
    if [ "$variant" = "decl" ]; then
        decl_extra="
fn added_decl(x: int) -> int {
    return x + 1
}
"
    fi
    if [ "$variant" != "base" ]; then
        body_expr="tag()"
    fi
    cat >"$file" <<XR_EOF
struct StaticPair {
    left: int
    right: int
}

fn tag() -> int {
    return $extra
}

fn run() -> int {
    const table: [int; 3] = comptime [1, 2, 3]
    const pair = comptime StaticPair{left: table[0], right: 4}
    const value = comptime 7
    return value + pair.right + $body_expr
}

$decl_extra
print(run())
XR_EOF
}

write_capability_case() {
    local file="$1" variant="$2" extra="$3"
    local decl_extra=""
    local body_expr="$extra"
    if [ "$variant" = "decl" ]; then
        decl_extra="
fn added_decl(x: int) -> int {
    return x + 1
}
"
    fi
    if [ "$variant" != "base" ]; then
        body_expr="tag()"
    fi
    cat >"$file" <<XR_EOF
fn tag() -> int {
    return $extra
}

fn worker(x: int) -> int {
    return x + 1 + $body_expr
}

fn sum(xs: Array<int>) -> int {
    return xs.length
}

shared ch: Channel<Array<int>> = Channel(1)

$decl_extra
var a = go worker(41)
var xs = [1, 2, 3]
var b = go sum(copy(xs))
var ys = [4, 5]
ch.send(move ys)

print(await a)
print(await b)
XR_EOF
}

write_case() {
    local case_name="$1" file="$2" variant="$3" extra="$4"
    case "$case_name" in
        class) write_class_case "$file" "$variant" "$extra" ;;
        generic) write_generic_case "$file" "$variant" "$extra" ;;
        closure) write_closure_case "$file" "$variant" "$extra" ;;
        static) write_static_case "$file" "$variant" "$extra" ;;
        capability) write_capability_case "$file" "$variant" "$extra" ;;
        *)
            echo "FAIL: unknown case '$case_name'" >&2
            return 1
            ;;
    esac
}

build_case() {
    local case_name="$1" phase="$2" file="$3" out="$4" cache="$5" log="$6"
    shift 6
    "$XRAY" build --native --verbose --cache-dir "$cache" "$@" -o "$out" "$file" >"$log" 2>&1
}

extract_summary() {
    local log="$1"
    local summary
    summary="$(grep -E 'evidence cache summary:' "$log" | tail -n 1 || true)"
    if [ -z "$summary" ]; then
        printf 'missing'
    else
        printf '%s' "$summary" | sed -E 's/.*summary: //'
    fi
}

expect_summary() {
    local log="$1" expected="$2" case_name="$3" phase="$4"
    if grep -q "evidence cache summary: $expected" "$log"; then
        return 0
    fi
    echo "FAIL: $case_name/$phase expected evidence cache summary '$expected'" >&2
    grep -E 'evidence cache' "$log" >&2 || true
    FAIL=$((FAIL + 1))
    return 1
}

run_phase() {
    local case_name="$1" phase="$2" file="$3" out="$4" cache="$5" expected="$6"
    shift 6
    local log="$out.log" start end ms summary status
    start="$(now_ms)"
    if build_case "$case_name" "$phase" "$file" "$out" "$cache" "$log" "$@"; then
        status=pass
    else
        status=fail
        FAIL=$((FAIL + 1))
    fi
    end="$(now_ms)"
    ms="$(elapsed_ms "$start" "$end")"
    summary="$(extract_summary "$log")"
    printf '%s\t%s\t%s\t%s\t%s\n' "$case_name" "$phase" "$ms" "$summary" "$status" | tee -a "$RESULTS"
    if [ "$status" = "pass" ] && [ -n "$expected" ]; then
        expect_summary "$log" "$expected" "$case_name" "$phase"
    fi
}

run_one_case() {
    local case_name="$1" iter="$2"
    local dir="$WORK/$case_name-$iter"
    local src="$dir/main.xr"
    local cache="$dir/.cache"
    mkdir -p "$dir"

    write_case "$case_name" "$src" base 0 || {
        FAIL=$((FAIL + 1))
        return 1
    }
    run_phase "$case_name" cold "$src" "$dir/cold" "$cache" "hits=0 misses=4"
    run_phase "$case_name" warm "$src" "$dir/warm" "$cache" "hits=4 misses=0"
    run_phase "$case_name" dump "$src" "$dir/dump" "$cache" "hits=4 misses=0" --dump-global-evidence

    write_case "$case_name" "$src" body 1 || {
        FAIL=$((FAIL + 1))
        return 1
    }
    run_phase "$case_name" body_change "$src" "$dir/body" "$cache" "hits=2 misses=2"

    write_case "$case_name" "$src" decl 2 || {
        FAIL=$((FAIL + 1))
        return 1
    }
    run_phase "$case_name" decl_change "$src" "$dir/decl" "$cache" "hits=0 misses=4"
    run_phase "$case_name" rebuild "$src" "$dir/rebuild" "$cache" "hits=0 misses=4 rebuild" --rebuild
}

printf 'case\tphase\telapsed_ms\tevidence_cache\tstatus\n' | tee "$RESULTS"
iter=1
while [ "$iter" -le "$REPEAT" ]; do
    old_ifs="$IFS"
    IFS=','
    set -- $CASES
    IFS="$old_ifs"
    for case_name in "$@"; do
        [ -n "$case_name" ] || continue
        run_one_case "$case_name" "$iter"
    done
    iter=$((iter + 1))
done

if [ "$FAIL" -eq 0 ]; then
    echo "Benchmark results: $RESULTS" >&2
    exit 0
fi
echo "Benchmark failed with $FAIL issue(s). Workdir: $WORK" >&2
KEEP=1
exit 1
