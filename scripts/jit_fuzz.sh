#!/bin/bash
# JIT Fuzzer - Generate random xray scripts and test JIT correctness
#
# Strategy:
#   1. Generate random .xr scripts combining various language features
#   2. Run each with --no-jit (baseline) and --jit-force (JIT)
#   3. Compare output: any diff = potential JIT bug
#   4. Save failing scripts for reproduction
#
# Usage: ./scripts/jit_fuzz.sh [options]
#   -b <binary>   Path to xray binary
#   -n <count>    Number of fuzz iterations (default: 100)
#   -s <seed>     Random seed (default: current timestamp)
#   -o <dir>      Output directory for failing cases (default: tests/tmp/jit_fuzz)
#   -t <seconds>  Timeout per test (default: 5)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Auto-detect binary
if [ -f "${PROJECT_ROOT}/build-release/xray" ]; then
    XRAY_BIN="${PROJECT_ROOT}/build-release/xray"
elif [ -f "${PROJECT_ROOT}/build/xray" ]; then
    XRAY_BIN="${PROJECT_ROOT}/build/xray"
else
    echo "Error: xray binary not found"
    exit 1
fi

COUNT=100
SEED=$(date +%s)
OUT_DIR="${PROJECT_ROOT}/tests/tmp/jit_fuzz"
TIMEOUT=5

while getopts "b:n:s:o:t:" opt; do
    case $opt in
        b) XRAY_BIN="$OPTARG" ;;
        n) COUNT="$OPTARG" ;;
        s) SEED="$OPTARG" ;;
        o) OUT_DIR="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        *) echo "Usage: $0 [-b binary] [-n count] [-s seed] [-o outdir] [-t timeout]"; exit 1 ;;
    esac
done

mkdir -p "$OUT_DIR"

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    GREEN='\033[0;32m' RED='\033[0;31m' YELLOW='\033[0;33m' NC='\033[0m'
else
    GREEN='' RED='' YELLOW='' NC=''
fi

TOTAL=0
PASS=0
DIFF=0
CRASH=0
TIMEOUT_COUNT=0

echo "JIT Fuzzer"
echo "Binary:     $XRAY_BIN"
echo "Iterations: $COUNT"
echo "Seed:       $SEED"
echo "Output:     $OUT_DIR"
echo ""

# Template library: building blocks for random scripts
# Each template is a function that generates a random xr snippet

gen_arithmetic() {
    local ops=('+' '-' '*' '/')
    local op=${ops[$((RANDOM % 4))]}
    local a=$((RANDOM % 1000))
    local b=$((RANDOM % 999 + 1))
    echo "fn fuzz_arith_${1}(x: int, y: int) : int {"
    echo "    return x ${op} y"
    echo "}"
    echo ""
    echo "let r_${1} = fuzz_arith_${1}(${a}, ${b})"
    echo "print(r_${1})"
}

gen_comparison() {
    local ops=('<' '<=' '>' '>=' '==' '!=')
    local op=${ops[$((RANDOM % 6))]}
    local a=$((RANDOM % 100))
    local b=$((RANDOM % 100))
    echo "fn fuzz_cmp_${1}(x: int, y: int) : bool {"
    echo "    return x ${op} y"
    echo "}"
    echo ""
    echo "let c_${1} = fuzz_cmp_${1}(${a}, ${b})"
    echo "print(c_${1})"
}

gen_loop_sum() {
    local n=$((RANDOM % 50 + 10))
    echo "fn fuzz_sum_${1}(n: int) : int {"
    echo "    let s = 0"
    echo "    let i = 0"
    echo "    while (i < n) {"
    echo "        s = s + i"
    echo "        i = i + 1"
    echo "    }"
    echo "    return s"
    echo "}"
    echo ""
    echo "print(fuzz_sum_${1}(${n}))"
}

gen_array_ops() {
    local n=$((RANDOM % 20 + 5))
    echo "fn fuzz_arr_${1}(n: int) : int {"
    echo "    let arr = []"
    echo "    let i = 0"
    echo "    while (i < n) {"
    echo "        arr.push(i * 2)"
    echo "        i = i + 1"
    echo "    }"
    echo "    let sum = 0"
    echo "    i = 0"
    echo "    while (i < arr.length) {"
    echo "        sum = sum + arr[i]"
    echo "        i = i + 1"
    echo "    }"
    echo "    return sum"
    echo "}"
    echo ""
    echo "print(fuzz_arr_${1}(${n}))"
}

gen_conditional() {
    local a=$((RANDOM % 100))
    local b=$((RANDOM % 100))
    echo "fn fuzz_cond_${1}(x: int, y: int) : int {"
    echo "    if (x > y) {"
    echo "        return x - y"
    echo "    } else {"
    echo "        return y - x"
    echo "    }"
    echo "}"
    echo ""
    echo "print(fuzz_cond_${1}(${a}, ${b}))"
}

gen_nested_call() {
    echo "fn fuzz_inner_${1}(x: int) : int {"
    echo "    return x * x"
    echo "}"
    echo ""
    echo "fn fuzz_outer_${1}(x: int) : int {"
    echo "    return fuzz_inner_${1}(x) + fuzz_inner_${1}(x + 1)"
    echo "}"
    echo ""
    local a=$((RANDOM % 50))
    echo "print(fuzz_outer_${1}(${a}))"
}

gen_string_ops() {
    local words=("hello" "world" "foo" "bar" "xray" "test")
    local w1=${words[$((RANDOM % 6))]}
    local w2=${words[$((RANDOM % 6))]}
    echo "fn fuzz_str_${1}(a: string, b: string) : string {"
    echo "    return a + \" \" + b"
    echo "}"
    echo ""
    echo "print(fuzz_str_${1}(\"${w1}\", \"${w2}\"))"
}

gen_json_ops() {
    local a=$((RANDOM % 100))
    local b=$((RANDOM % 100))
    echo "fn fuzz_json_${1}(x: int, y: int) : int {"
    echo "    let obj = { \"a\": x, \"b\": y }"
    echo "    return obj.a + obj.b"
    echo "}"
    echo ""
    echo "print(fuzz_json_${1}(${a}, ${b}))"
}

gen_closure() {
    local base=$((RANDOM % 50))
    echo "fn fuzz_closure_${1}(base: int) {"
    echo "    fn add(x: int) : int {"
    echo "        return base + x"
    echo "    }"
    echo "    print(add(10))"
    echo "}"
    echo ""
    echo "fuzz_closure_${1}(${base})"
}

gen_bitwise() {
    local ops=('&' '|' '^')
    local op=${ops[$((RANDOM % 3))]}
    local a=$((RANDOM % 256))
    local b=$((RANDOM % 256))
    echo "fn fuzz_bit_${1}(x: int, y: int) : int {"
    echo "    return x ${op} y"
    echo "}"
    echo ""
    echo "print(fuzz_bit_${1}(${a}, ${b}))"
}

# Generator list
generators=(gen_arithmetic gen_comparison gen_loop_sum gen_array_ops gen_conditional gen_nested_call gen_string_ops gen_json_ops gen_closure gen_bitwise)
num_generators=${#generators[@]}

for ((iter=1; iter<=COUNT; iter++)); do
    RANDOM=$((SEED + iter))
    TOTAL=$((TOTAL + 1))

    # Generate script with 3-6 random snippets
    num_snippets=$((RANDOM % 4 + 3))
    tmp_file=$(mktemp "${OUT_DIR}/fuzz_XXXXXX.xr")

    echo "// Fuzz test: seed=$((SEED + iter)), snippets=$num_snippets" > "$tmp_file"
    echo "" >> "$tmp_file"

    for ((s=0; s<num_snippets; s++)); do
        gen_idx=$((RANDOM % num_generators))
        ${generators[$gen_idx]} "${iter}_${s}" >> "$tmp_file"
        echo "" >> "$tmp_file"
    done

    # Run with --no-jit (baseline)
    out_interp=$(timeout "$TIMEOUT" "$XRAY_BIN" --no-jit "$tmp_file" 2>/dev/null) && rc_interp=0 || rc_interp=$?

    # Run with --jit-force
    out_jit=$(timeout "$TIMEOUT" "$XRAY_BIN" --jit-force "$tmp_file" 2>/dev/null) && rc_jit=0 || rc_jit=$?

    # Skip if interpreter itself fails (script generation issue)
    if [ "$rc_interp" -ne 0 ] && [ "$rc_interp" -ne 124 ]; then
        rm -f "$tmp_file"
        continue
    fi

    # Check for JIT crash
    if [ "$rc_jit" -eq 139 ] || [ "$rc_jit" -eq 134 ] || [ "$rc_jit" -eq 136 ]; then
        CRASH=$((CRASH + 1))
        saved="${OUT_DIR}/crash_${iter}.xr"
        cp "$tmp_file" "$saved"
        printf "  [%-4d] ${RED}CRASH (exit=%d)${NC} → %s\n" "$iter" "$rc_jit" "$saved"
        rm -f "$tmp_file"
        continue
    fi

    # Check for JIT timeout
    if [ "$rc_jit" -eq 124 ]; then
        TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
        rm -f "$tmp_file"
        continue
    fi

    # Compare output
    if [ "$out_interp" = "$out_jit" ]; then
        PASS=$((PASS + 1))
        rm -f "$tmp_file"
    else
        DIFF=$((DIFF + 1))
        saved="${OUT_DIR}/diff_${iter}.xr"
        cp "$tmp_file" "$saved"
        printf "  [%-4d] ${RED}OUTPUT_DIFF${NC} → %s\n" "$iter" "$saved"
        rm -f "$tmp_file"
    fi

    # Progress every 25 iterations
    if [ $((iter % 25)) -eq 0 ]; then
        printf "  ... %d/%d (pass=%d, crash=%d, diff=%d)\n" "$iter" "$COUNT" "$PASS" "$CRASH" "$DIFF"
    fi
done

echo ""
echo "======================================"
echo "JIT Fuzz Summary"
echo "======================================"
echo "Total:    $TOTAL"
echo -e "${GREEN}Pass:     $PASS${NC}"
echo -e "${RED}Crash:    $CRASH${NC}"
echo -e "${RED}Diff:     $DIFF${NC}"
echo -e "${YELLOW}Timeout:  $TIMEOUT_COUNT${NC}"

if [ "$CRASH" -gt 0 ] || [ "$DIFF" -gt 0 ]; then
    echo ""
    echo "Failing cases saved to: $OUT_DIR"
    echo -e "${RED}JIT bugs detected by fuzzing.${NC}"
    exit 1
else
    echo ""
    echo -e "${GREEN}No JIT bugs found in $COUNT iterations.${NC}"
    # Clean up empty fuzz dir
    rmdir "$OUT_DIR" 2>/dev/null || true
    exit 0
fi
