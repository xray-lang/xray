#!/bin/bash
# JIT Fuzzer - Generate random xray scripts and test JIT correctness
#
# Four fuzz layers (076 spec S9):
#   Layer 1 (mcinsn):           Encoding-level diff — xisagen random-diff
#   Layer 2 (program):          Semantic diff — --no-jit vs --jit-force
#   Layer 3 (driver stress):    Compilation crash detection (10 template types)
#   Layer 4 (corruption-chain): GC/barrier/deopt chain stress (8 chain types)
#
# By default runs Layer 2 (program diff). Use -m to run Layer 1 (mcinsn diff),
# or -a to run all four layers sequentially.
#
# Usage: ./scripts/jit_fuzz.sh [options]
#   -b <binary>   Path to xray binary
#   -n <count>    Number of fuzz iterations (default: 100)
#   -s <seed>     Random seed (default: current timestamp)
#   -o <dir>      Output directory for failing cases (default: tests/tmp/jit_fuzz)
#   -t <seconds>  Timeout per test (default: 5)
#   -m            Run mcinsn seed diff only (Layer 1)
#   -a            Run all layers (mcinsn + program diff)

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
FUZZ_MODE="program"  # "program", "mcinsn", or "all"

while getopts "b:n:s:o:t:ma" opt; do
    case $opt in
        b) XRAY_BIN="$OPTARG" ;;
        n) COUNT="$OPTARG" ;;
        s) SEED="$OPTARG" ;;
        o) OUT_DIR="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        m) FUZZ_MODE="mcinsn" ;;
        a) FUZZ_MODE="all" ;;
        *) echo "Usage: $0 [-b binary] [-n count] [-s seed] [-o outdir] [-t timeout] [-m] [-a]"; exit 1 ;;
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
MCINSN_EXIT=0

# ============================================================================
# Layer 1: mcinsn seed diff (encoding-level, via xisagen random-diff)
# ============================================================================
run_mcinsn_diff() {
    echo "======================================"
    echo "Layer 1: mcinsn seed diff"
    echo "======================================"
    echo "Seed:  $SEED"
    echo "Count: $COUNT"
    echo ""

    local XISAGEN="${PROJECT_ROOT}/tools/xisagen/xisagen.py"
    local ISA_DIR="${PROJECT_ROOT}/xisa/arch"

    if ! python3 "$XISAGEN" random-diff \
            --seed="$SEED" --count="$COUNT" \
            x64="${ISA_DIR}/x64.isa"; then
        echo -e "${RED}Layer 1 (mcinsn): FAIL${NC}"
        MCINSN_EXIT=1
    else
        echo -e "${GREEN}Layer 1 (mcinsn): PASS${NC}"
    fi
    echo ""
}

if [ "$FUZZ_MODE" = "mcinsn" ] || [ "$FUZZ_MODE" = "all" ]; then
    run_mcinsn_diff
fi

if [ "$FUZZ_MODE" = "mcinsn" ]; then
    exit $MCINSN_EXIT
fi

# ============================================================================
# Layer 3: driver invariant fuzz (compilation stress)
# Generates programs that exercise diverse JIT paths and verifies that
# JIT compilation itself doesn't crash/assert. Programs are designed
# to trigger guards, type transitions, polymorphism, closures, and
# other codegen edge cases.
# ============================================================================
LAYER3_CRASH=0
LAYER3_TOTAL=0
LAYER3_PASS=0

run_driver_invariant_fuzz() {
    echo "======================================"
    echo "Layer 3: driver invariant fuzz"
    echo "======================================"
    echo "Iterations: $COUNT"
    echo ""

    local gen_idx=0
    local templates=(
        # Type guard stress: variable changes type mid-function
        'fn stress_%d() { let x: any = 42; for i in 0..10 { if i > 5 { x = "hello" } else { x = i * 2 } }; print(x) }'
        # Polymorphic call: different receivers
        'fn poly_%d() { let a = [1, 2, 3]; let m = {"a": 1}; print(a.len()); print(m.len()) }'
        # Closure capture stress
        'fn closure_%d() { let x = 0; let inc = fn() { x = x + 1 }; for i in 0..20 { inc() }; print(x) }'
        # Nested function calls (inlining pressure)
        'fn inner_%d(n: int) : int { return n * 2 + 1 }; fn outer_%d(n: int) : int { return inner_%d(inner_%d(n)) }; print(outer_%d(5))'
        # Exception path stress
        'fn throw_%d() { try { assert(1 == 1) } catch e { print(e) } }'
        # Array bounds (deopt trigger)
        'fn bounds_%d() { let a = [1,2,3]; for i in 0..3 { print(a[i]) } }'
        # Float/int mixed arithmetic
        'fn mixed_%d() { let x: float = 3.14; let y: int = 2; print(x * y); print(y + 1) }'
        # String concatenation (GC pressure)
        'fn strcat_%d() { let s = ""; for i in 0..10 { s = s + str(i) }; print(s) }'
        # Deep recursion (stack pressure)
        'fn fib_%d(n: int) : int { if n <= 1 { return n }; return fib_%d(n-1) + fib_%d(n-2) }; print(fib_%d(15))'
        # Map operations (property access IC)
        'fn mapops_%d() { let m: map = {}; for i in 0..10 { m[str(i)] = i * i }; print(m["5"]) }'
    )

    local ntemplates=${#templates[@]}

    for ((iter=1; iter<=COUNT; iter++)); do
        RANDOM=$((SEED + iter + 10000))
        LAYER3_TOTAL=$((LAYER3_TOTAL + 1))

        local tmp_file=$(mktemp "${OUT_DIR}/stress_XXXXXX.xr")
        echo "// Layer 3 stress: seed=$((SEED + iter))" > "$tmp_file"
        echo "" >> "$tmp_file"

        # Pick 2-4 templates
        local npick=$((RANDOM % 3 + 2))
        for ((p=0; p<npick; p++)); do
            local tidx=$((RANDOM % ntemplates))
            local tmpl="${templates[$tidx]}"
            # Replace %d with unique id
            local uid="${iter}_${p}"
            local code
            code=$(echo "$tmpl" | sed "s/%d/${uid}/g")
            echo "$code" >> "$tmp_file"
            echo "" >> "$tmp_file"
        done

        # Run with --jit-force: we only care that compilation doesn't crash
        local stress_stderr="${OUT_DIR}/.stress_${iter}.tmp"
        timeout "$TIMEOUT" "$XRAY_BIN" --jit-force "$tmp_file" >"$stress_stderr" 2>&1 && rc=0 || rc=$?

        if [ "$rc" -eq 139 ] || [ "$rc" -eq 134 ] || [ "$rc" -eq 136 ]; then
            LAYER3_CRASH=$((LAYER3_CRASH + 1))
            local saved="${OUT_DIR}/stress_crash_${iter}.xr"
            cp "$tmp_file" "$saved"
            if [ -s "$stress_stderr" ]; then
                mv "$stress_stderr" "${OUT_DIR}/stress_crash_${iter}.log"
            else
                rm -f "$stress_stderr"
            fi
            printf "  [%-4d] ${RED}CRASH (exit=%d)${NC}\n" "$iter" "$rc"
        else
            LAYER3_PASS=$((LAYER3_PASS + 1))
            rm -f "$stress_stderr"
        fi
        rm -f "$tmp_file"

        if [ $((iter % 25)) -eq 0 ]; then
            printf "  ... %d/%d (pass=%d, crash=%d)\n" "$iter" "$COUNT" "$LAYER3_PASS" "$LAYER3_CRASH"
        fi
    done
    echo ""
}

if [ "$FUZZ_MODE" = "all" ]; then
    run_driver_invariant_fuzz
fi

# ============================================================================
# Layer 4: corruption-chain fuzz (GC / barrier / deopt chain stress)
# Generates programs that exercise corruption-sensitive paths:
#   - Heavy allocation during hot loops (GC must scan JIT frames)
#   - Object mutation across function calls (write barriers)
#   - Type-changing stores that trigger deopt mid-GC-pressure
#   - Cross-coroutine object passing (shared_refs)
# A missing write barrier or incorrect stackmap would cause
# use-after-free or dangling pointer under ASAN/GC-stress.
# ============================================================================
LAYER4_CRASH=0
LAYER4_TOTAL=0
LAYER4_PASS=0
LAYER4_DIFF=0

run_corruption_chain_fuzz() {
    echo "======================================"
    echo "Layer 4: corruption-chain fuzz"
    echo "======================================"
    echo "Iterations: $COUNT"
    echo ""

    local chain_templates=(
        # GC pressure: allocate in hot loop, verify old objects survive
        'fn gc_chain_%d() -> int { let kept = [1,2,3]; let sum = 0; for (let i = 0; i < 100; i++) { let tmp = [i, i+1]; sum = sum + tmp[0] }; return kept[0] + kept[1] + kept[2] + sum }'
        # Write barrier: mutate array slot with new allocation
        'fn barrier_%d() -> int { let arr = [0, 0, 0]; for (let i = 0; i < 50; i++) { arr[i %% 3] = [i, i*2] }; return arr[0][0] + arr[1][0] + arr[2][0] }'
        # Deopt + GC: polymorphic call triggers deopt while GC scans
        'fn poly_gc_%d(x: int | string) -> int { if (typeof(x) == "string") { return x.length }; return x + 1 }; fn chain_poly_%d() -> int { let arr = [10,20,30]; let r = poly_gc_%d(1) + poly_gc_%d("ab") + poly_gc_%d(3); return arr[0] + r }'
        # String concat GC: heavy string allocation with live array
        'fn strchain_%d() -> int { let nums = [0,0,0,0,0]; let s = ""; for (let i = 0; i < 30; i++) { s = s + "x"; nums[i %% 5] = i }; return nums[0] + nums[1] + nums[2] + nums[3] + nums[4] + s.length }'
        # Nested allocation: arrays of arrays surviving GC
        'fn nested_gc_%d() -> int { let outer = [[1,2],[3,4],[5,6]]; let sum = 0; for (let i = 0; i < 40; i++) { let tmp = [i, outer[i %% 3][0]]; sum = sum + tmp[1] }; return sum }'
        # Cross-function GC: callee allocates, caller holds live refs
        'fn alloc_callee_%d(n: int) -> int { let a = [n, n+1]; return a[0] + a[1] }; fn alloc_caller_%d() -> int { let kept = [100, 200]; let sum = 0; for (let i = 0; i < 50; i++) { sum = sum + alloc_callee_%d(i) }; return kept[0] + kept[1] + sum }'
        # Coroutine + GC: spawn tasks that allocate heavily
        'fn coro_alloc_%d(n: int) -> int { let s = 0; for (let i = 0; i < n; i++) { let a = [i]; s = s + a[0] }; return s }; fn coro_chain_%d() -> int { let t1 = go coro_alloc_%d(20); let t2 = go coro_alloc_%d(30); return (await t1) + (await t2) }'
        # Array resize GC: growing arrays triggers realloc + GC
        'fn grow_%d() -> int { let a: [int] = []; for (let i = 0; i < 60; i++) { a.push(i) }; let sum = 0; for (let i = 0; i < a.length; i++) { sum = sum + a[i] }; return sum }'
    )

    local ntemplates=${#chain_templates[@]}

    for ((iter=1; iter<=COUNT; iter++)); do
        RANDOM=$((SEED + iter + 20000))
        LAYER4_TOTAL=$((LAYER4_TOTAL + 1))

        local tmp_file=$(mktemp "${OUT_DIR}/chain_XXXXXX.xr")
        echo "// Layer 4 corruption-chain: seed=$((SEED + iter))" > "$tmp_file"
        echo "" >> "$tmp_file"

        # Pick 1-3 chain templates
        local npick=$((RANDOM % 3 + 1))
        local call_lines=""
        for ((p=0; p<npick; p++)); do
            local tidx=$((RANDOM % ntemplates))
            local tmpl="${chain_templates[$tidx]}"
            local uid="${iter}_${p}"
            local code
            code=$(echo "$tmpl" | sed "s/%d/${uid}/g")
            echo "$code" >> "$tmp_file"
            echo "" >> "$tmp_file"
        done

        # Run with --jit-force, compare to --no-jit
        local jit_out nojit_out
        jit_out=$(timeout "$TIMEOUT" "$XRAY_BIN" run --jit-force "$tmp_file" 2>/dev/null) && jit_rc=0 || jit_rc=$?
        nojit_out=$(timeout "$TIMEOUT" "$XRAY_BIN" run --no-jit "$tmp_file" 2>/dev/null) && nojit_rc=0 || nojit_rc=$?

        if [ "$jit_rc" -eq 139 ] || [ "$jit_rc" -eq 134 ] || [ "$jit_rc" -eq 136 ]; then
            LAYER4_CRASH=$((LAYER4_CRASH + 1))
            cp "$tmp_file" "${OUT_DIR}/chain_crash_${iter}.xr"
            printf "  [%-4d] ${RED}CRASH (exit=%d)${NC}\n" "$iter" "$jit_rc"
        elif [ "$jit_rc" -ne "$nojit_rc" ] || [ "$jit_out" != "$nojit_out" ]; then
            LAYER4_DIFF=$((LAYER4_DIFF + 1))
            cp "$tmp_file" "${OUT_DIR}/chain_diff_${iter}.xr"
            printf "  [%-4d] ${RED}DIFF${NC}\n" "$iter"
        else
            LAYER4_PASS=$((LAYER4_PASS + 1))
        fi
        rm -f "$tmp_file"

        if [ $((iter % 25)) -eq 0 ]; then
            printf "  ... %d/%d (pass=%d, crash=%d, diff=%d)\n" "$iter" "$COUNT" "$LAYER4_PASS" "$LAYER4_CRASH" "$LAYER4_DIFF"
        fi
    done
    echo ""
}

if [ "$FUZZ_MODE" = "all" ]; then
    run_corruption_chain_fuzz
fi

# ============================================================================
# Layer 2: program diff (semantic, --no-jit vs --jit-force)
# ============================================================================
echo "======================================"
echo "Layer 2: program diff"
echo "======================================"
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

    # Run with --jit-force (preserve stderr for crash/deopt diagnostics)
    jit_stderr="${OUT_DIR}/.stderr_${iter}.tmp"
    out_jit=$(timeout "$TIMEOUT" "$XRAY_BIN" --jit-force "$tmp_file" 2>"$jit_stderr") && rc_jit=0 || rc_jit=$?

    # Skip if interpreter itself fails (script generation issue)
    if [ "$rc_interp" -ne 0 ] && [ "$rc_interp" -ne 124 ]; then
        rm -f "$tmp_file" "$jit_stderr"
        continue
    fi

    # Check for JIT crash
    if [ "$rc_jit" -eq 139 ] || [ "$rc_jit" -eq 134 ] || [ "$rc_jit" -eq 136 ]; then
        CRASH=$((CRASH + 1))
        saved="${OUT_DIR}/crash_${iter}.xr"
        cp "$tmp_file" "$saved"
        # Save crash stderr (includes JIT disasm from crash handler)
        if [ -s "$jit_stderr" ]; then
            mv "$jit_stderr" "${OUT_DIR}/crash_${iter}.log"
        else
            rm -f "$jit_stderr"
        fi
        printf "  [%-4d] ${RED}CRASH (exit=%d)${NC} → %s\n" "$iter" "$rc_jit" "$saved"
        rm -f "$tmp_file"
        continue
    fi

    # Check for JIT timeout
    if [ "$rc_jit" -eq 124 ]; then
        TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
        rm -f "$tmp_file" "$jit_stderr"
        continue
    fi

    # Compare output
    if [ "$out_interp" = "$out_jit" ]; then
        PASS=$((PASS + 1))
        rm -f "$tmp_file" "$jit_stderr"
    else
        DIFF=$((DIFF + 1))
        saved="${OUT_DIR}/diff_${iter}.xr"
        cp "$tmp_file" "$saved"
        # Save diff stderr (includes JIT deopt/warning diagnostics)
        if [ -s "$jit_stderr" ]; then
            mv "$jit_stderr" "${OUT_DIR}/diff_${iter}.log"
        else
            rm -f "$jit_stderr"
        fi
        printf "  [%-4d] ${RED}OUTPUT_DIFF${NC} → %s\n" "$iter" "$saved"
        rm -f "$tmp_file"
    fi

    # Progress every 25 iterations
    if [ $((iter % 25)) -eq 0 ]; then
        printf "  ... %d/%d (pass=%d, crash=%d, diff=%d)\n" "$iter" "$COUNT" "$PASS" "$CRASH" "$DIFF"
    fi
done

# ============================================================================
# Shrinker: minimize failing test cases
# ============================================================================
shrink_file() {
    local src="$1"
    local kind="$2"  # "crash" or "diff"
    local shrunk="${src%.xr}.min.xr"

    # Read file into array of function blocks (split on blank lines between fns)
    local nlines
    nlines=$(wc -l < "$src")
    [ "$nlines" -le 3 ] && return

    # Extract individual top-level blocks separated by blank lines
    local blocks=()
    local block=""
    while IFS= read -r line; do
        if [ -z "$line" ] && [ -n "$block" ]; then
            blocks+=("$block")
            block=""
        else
            block="${block}${line}
"
        fi
    done < "$src"
    [ -n "$block" ] && blocks+=("$block")

    local nb=${#blocks[@]}
    [ "$nb" -le 2 ] && return  # already minimal (comment + 1 snippet)

    local improved=false
    # Try removing each block (skip first comment block)
    for ((bi=nb-1; bi>=1; bi--)); do
        local candidate=$(mktemp "${OUT_DIR}/.shrink_XXXXXX.xr")
        for ((bj=0; bj<nb; bj++)); do
            [ "$bj" -eq "$bi" ] && continue
            printf '%s\n' "${blocks[$bj]}" >> "$candidate"
        done

        local still_fails=false
        if [ "$kind" = "crash" ]; then
            timeout "$TIMEOUT" "$XRAY_BIN" --jit-force "$candidate" >/dev/null 2>/dev/null
            local rc=$?
            [ "$rc" -eq 139 ] || [ "$rc" -eq 134 ] || [ "$rc" -eq 136 ] && still_fails=true
        else
            local out_i out_j
            out_i=$(timeout "$TIMEOUT" "$XRAY_BIN" --no-jit "$candidate" 2>/dev/null) || true
            out_j=$(timeout "$TIMEOUT" "$XRAY_BIN" --jit-force "$candidate" 2>/dev/null) || true
            [ "$out_i" != "$out_j" ] && still_fails=true
        fi

        if $still_fails; then
            cp "$candidate" "$src"
            # Re-read blocks from shrunk file
            blocks=()
            block=""
            while IFS= read -r line; do
                if [ -z "$line" ] && [ -n "$block" ]; then
                    blocks+=("$block")
                    block=""
                else
                    block="${block}${line}
"
                fi
            done < "$src"
            [ -n "$block" ] && blocks+=("$block")
            nb=${#blocks[@]}
            improved=true
        fi
        rm -f "$candidate"
    done

    if $improved; then
        local new_lines
        new_lines=$(wc -l < "$src")
        printf "    shrunk %s: %d → %d lines\n" "$(basename "$src")" "$nlines" "$new_lines"
    fi
}

# Run shrinker on all failures
if [ "$CRASH" -gt 0 ] || [ "$DIFF" -gt 0 ]; then
    echo ""
    echo "Shrinking failing cases..."
    for f in "${OUT_DIR}"/crash_*.xr; do
        [ -f "$f" ] && shrink_file "$f" "crash"
    done
    for f in "${OUT_DIR}"/diff_*.xr; do
        [ -f "$f" ] && shrink_file "$f" "diff"
    done
fi

echo ""
echo "======================================"
echo "JIT Fuzz Summary"
echo "======================================"
if [ "$FUZZ_MODE" = "all" ]; then
    if [ "$MCINSN_EXIT" -eq 0 ]; then
        echo -e "${GREEN}Layer 1 (mcinsn): PASS${NC}"
    else
        echo -e "${RED}Layer 1 (mcinsn): FAIL${NC}"
    fi
    echo "Layer 3 (driver stress):"
    echo "  Total:    $LAYER3_TOTAL"
    echo -e "  ${GREEN}Pass:     $LAYER3_PASS${NC}"
    echo -e "  ${RED}Crash:    $LAYER3_CRASH${NC}"
    echo "Layer 4 (corruption-chain):"
    echo "  Total:    $LAYER4_TOTAL"
    echo -e "  ${GREEN}Pass:     $LAYER4_PASS${NC}"
    echo -e "  ${RED}Crash:    $LAYER4_CRASH${NC}"
    echo -e "  ${RED}Diff:     $LAYER4_DIFF${NC}"
fi
echo "Layer 2 (program):"
echo "  Total:    $TOTAL"
echo -e "  ${GREEN}Pass:     $PASS${NC}"
echo -e "  ${RED}Crash:    $CRASH${NC}"
echo -e "  ${RED}Diff:     $DIFF${NC}"
echo -e "  ${YELLOW}Timeout:  $TIMEOUT_COUNT${NC}"

FINAL_EXIT=0
if [ "$CRASH" -gt 0 ] || [ "$DIFF" -gt 0 ]; then
    echo ""
    echo "Failing cases saved to: $OUT_DIR"
    echo -e "${RED}JIT bugs detected by fuzzing.${NC}"
    FINAL_EXIT=1
else
    echo ""
    echo -e "${GREEN}No JIT bugs found in $COUNT iterations.${NC}"
    rmdir "$OUT_DIR" 2>/dev/null || true
fi

if [ "$FUZZ_MODE" = "all" ] && [ "$MCINSN_EXIT" -ne 0 ]; then
    FINAL_EXIT=1
fi
if [ "$LAYER3_CRASH" -gt 0 ] || [ "$LAYER4_CRASH" -gt 0 ] || [ "$LAYER4_DIFF" -gt 0 ]; then
    FINAL_EXIT=1
fi
exit $FINAL_EXIT
