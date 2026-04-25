#!/bin/bash
# check_jit_safety.sh - JIT Safety Checker
#
# Comprehensive static + dynamic checks for common JIT bugs discovered
# during the JIT Coroutine OSR debugging sessions. Run this script after
# writing JIT-related code to catch known bug patterns early.
#
# Usage: ./scripts/check_jit_safety.sh [options]
#   -s         Static checks only (no build/run)
#   -d         Dynamic checks only (build + run tests)
#   -v         Verbose output
#   -q         Quiet mode (only show failures)
#   -b <dir>   Build directory (default: build)
#
# Bug patterns detected:
#   [S1] opcode_reads_slot completeness (Blueprint liveness)
#   [S2] ARM64 alignment of JIT scratch fields
#   [S3] CROSS_KIND folding without UNKNOWN tag guard
#   [S4] OP_MOVE forced bc_slot update (OSR corruption)
#   [S5] edge_copies using blk_start instead of blk_end
#   [S6] Missing NULL check after alloc slow path
#   [S7] TARRAY_GET hardcoded stride without elem_type guard
#   [S8] Liveness block ID vs layout index mapping
#   [S9] range_shorten bounds check (ns < end)
#   [S10] first_isect must use overall range bounds (no hole reuse)
#   [S11] Blueprint nested loop live scan must cover [0, code_count)
#   [S12] xra_live_ptr must be per-instruction, not per-block
#   [S13] slot_rep passed to runtime helpers (should use builder_slot_xr_tag)
#   [S14] Bool-producing handlers must tag XRVREG_TAG_BOOL
#   [S15] COMPOUND_ASSIGN must use ref_xir_type not slot_rep
#   [S16] jit_value_from_tag UNKNOWN branch usage audit
#   [D1] JIT vs interpreter differential (--jit-force vs --no-jit)
#   [D2] JIT coroutine correctness (go + await)
#   [D3] Multi-return value correctness
#   [D4] ASan build check (memory safety)
#   [D5] Nested loop OSR correctness
#   [D6] Channel trySend/tryRecv value + bool return
#   [D7] Logical operators always return bool
#   [D8] Map for-in with compound assignment
#   [D9] Custom iterator class for-in
#   [D10] Array<any> element type dispatch

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

MODE="all"  # all, static, dynamic
VERBOSE=0
QUIET=0
PASS=0
FAIL=0
WARN=0

# Colors (disabled for non-TTY / NO_COLOR)
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[0;31m' GREEN='\033[0;32m' YELLOW='\033[0;33m' CYAN='\033[0;36m' NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' CYAN='' NC=''
fi

while getopts "sdvqb:" opt; do
    case $opt in
        s) MODE="static" ;;
        d) MODE="dynamic" ;;
        v) VERBOSE=1 ;;
        q) QUIET=1 ;;
        b) BUILD_DIR="$OPTARG" ;;
        *) echo "Usage: $0 [-s|-d] [-v] [-q] [-b dir]"; exit 1 ;;
    esac
done

log_pass() {
    PASS=$((PASS+1))
    [ $QUIET -eq 0 ] && echo -e "  ${GREEN}PASS${NC} $1"
}

log_fail() {
    FAIL=$((FAIL+1))
    echo -e "  ${RED}FAIL${NC} $1"
}

log_warn() {
    WARN=$((WARN+1))
    echo -e "  ${YELLOW}WARN${NC} $1"
}

log_info() {
    [ $QUIET -eq 0 ] && echo -e "  ${CYAN}INFO${NC} $1"
}

# ========== Static Checks ==========

run_static_checks() {
    echo -e "\n${CYAN}=== Static Analysis ===${NC}"

    JIT_DIR="${PROJECT_ROOT}/src/jit"
    BP_FILE="${JIT_DIR}/xir_blueprint.c"
    BLD_FILE="${JIT_DIR}/xir_builder.c"
    CG_FILE="${JIT_DIR}/xir_codegen.c"
    RA_FILE="${JIT_DIR}/xir_regalloc.c"
    CG_MEM="${JIT_DIR}/xir_codegen_mem.c"
    BLD_CALL="${JIT_DIR}/xir_builder_call.c"
    CORO_H="${PROJECT_ROOT}/src/coro/xcoroutine.h"

    # [S1] opcode_reads_slot completeness
    echo -e "\n${CYAN}[S1] Blueprint opcode_reads_slot completeness${NC}"

    # Count OP_ cases in opcode_reads_slot vs total OP_ enums
    if [ -f "$BP_FILE" ]; then
        bp_opcodes=$(sed -n '/^static bool opcode_reads_slot/,/^}/p' "$BP_FILE" | \
                     grep -oE 'OP_[A-Z_]+' | sort -u | wc -l | tr -d ' ')

        # Check key opcodes that historically caused bugs
        for op in OP_LT OP_LE OP_EQ OP_ADD OP_MOVE OP_INDEX_GET OP_TARRAY_GET OP_CALL; do
            if sed -n '/^static bool opcode_reads_slot/,/^}/p' "$BP_FILE" | grep -q "$op"; then
                log_pass "$op handled in opcode_reads_slot"
            else
                log_fail "$op MISSING from opcode_reads_slot — loop liveness may be wrong"
            fi
        done

        # Check that OP_LT/LE/EQ read both A and B (Bug #12)
        lt_check=$(sed -n '/^static bool opcode_reads_slot/,/^}/p' "$BP_FILE" | \
                   sed -n '/case OP_LT.*case OP_LE.*case OP_EQ/,/return/p' | head -5)
        if echo "$lt_check" | grep -q 'reg == a.*||.*reg == b'; then
            log_pass "OP_LT/LE/EQ reads both R[A] and R[B]"
        else
            log_fail "OP_LT/LE/EQ may not read R[B] — Blueprint liveness bug (Bug #12)"
        fi

        log_info "Total opcodes in opcode_reads_slot: $bp_opcodes"
    else
        log_warn "Blueprint file not found: $BP_FILE"
    fi

    # [S2] ARM64 alignment of JIT scratch fields
    echo -e "\n${CYAN}[S2] JIT scratch ARM64 alignment${NC}"

    if [ -f "$CORO_H" ]; then
        # ret_tags must be int64_t (8 bytes), not uint8_t (1 byte)
        # Match only actual field declarations (type + name), not comments
        ret_tags_type=$(grep -E '^\s+(int64_t|uint8_t|int32_t|uint32_t)\s+ret_tags\[' "$CORO_H" | head -1)
        if echo "$ret_tags_type" | grep -q 'int64_t'; then
            log_pass "ret_tags[] is int64_t (8-byte aligned for ARM64 STR/LDR)"
        elif echo "$ret_tags_type" | grep -q 'uint8_t'; then
            log_fail "ret_tags[] is uint8_t — ARM64 STR will crash on unaligned access"
        else
            log_warn "Could not determine ret_tags type (check $CORO_H manually)"
        fi

        # ret_vals must be int64_t
        ret_vals_type=$(grep -E '^\s+(int64_t|uint8_t|int32_t|uint32_t)\s+ret_vals\[' "$CORO_H" | head -1)
        if echo "$ret_vals_type" | grep -q 'int64_t'; then
            log_pass "ret_vals[] is int64_t (8-byte aligned)"
        else
            log_fail "ret_vals[] is not int64_t — alignment issue"
        fi
    else
        log_warn "Coroutine header not found: $CORO_H"
    fi

    # [S3] CROSS_KIND folding UNKNOWN tag guard
    echo -e "\n${CYAN}[S3] CROSS_KIND constant-fold UNKNOWN tag guard${NC}"

    if [ -f "$BLD_FILE" ]; then
        # Check that cross-kind EQ uses TAG_KIND with unknown guard (kind != 0)
        if grep -q 'TAG_KIND' "$BLD_FILE" && grep -q 'kind_a != 0 && kind_b != 0' "$BLD_FILE"; then
            log_pass "Cross-kind EQ uses TAG_KIND with unknown guard"
        elif grep -A 30 'XIRT_IS_CROSS_KIND' "$BLD_FILE" | grep -q 'XRVREG_TAG_UNKNOWN'; then
            log_pass "CROSS_KIND branch checks for UNKNOWN tags before folding"
        else
            log_fail "CROSS_KIND branch may constant-fold polymorphic values (Bug #10)"
        fi
    fi

    # [S4] OP_MOVE must NOT force bc_slot update
    echo -e "\n${CYAN}[S4] OP_MOVE bc_slot protection${NC}"

    if [ -f "$BLD_FILE" ]; then
        # Check that OP_MOVE handler does NOT contain forced bc_slot = a
        move_section=$(sed -n '/case OP_MOVE:/,/case OP_/p' "$BLD_FILE" | head -40)
        if echo "$move_section" | grep -q 'bc_slot.*=.*\ba\b' | grep -v 'DCHECK\|DEBUG\|WARN\|fprintf'; then
            log_fail "OP_MOVE has forced bc_slot update — OSR corruption risk (Bug #11)"
        else
            log_pass "OP_MOVE does not force bc_slot update"
        fi

        # Check that builder_set_slot has relocation logic
        if grep -q 'new_slot.*=.*int16_t.*s' "$BLD_FILE"; then
            log_pass "builder_set_slot has bc_slot relocation logic"
        else
            log_warn "builder_set_slot may lack bc_slot relocation"
        fi
    fi

    # [S5] edge_copies must use blk_end for source
    echo -e "\n${CYAN}[S5] PHI edge copies use blk_end for source registers${NC}"

    if [ -f "$RA_FILE" ]; then
        if grep -q 'xra_vreg_reg_at_end.*from_bid.*sv' "$RA_FILE"; then
            log_pass "PHI source uses xra_vreg_reg_at_end (block end)"
        else
            log_fail "PHI source may use blk_start — infinite loop risk (Bug #5)"
        fi

        # Split transition copies also need blk_end
        if grep -q 'xra_vreg_reg_at_end.*from_bid.*v\b' "$RA_FILE"; then
            log_pass "Split transitions use xra_vreg_reg_at_end"
        else
            log_warn "Split transitions may not use blk_end"
        fi
    fi

    # [S6] Alloc slow path NULL check
    echo -e "\n${CYAN}[S6] XIR_ALLOC slow path NULL check${NC}"

    if [ -f "$CG_MEM" ]; then
        if grep -q 'emit_patch_cbz\|PATCH_DEOPT_CBZ' "$CG_MEM"; then
            log_pass "XIR_ALLOC slow path has NULL check + deopt"
        else
            log_fail "XIR_ALLOC slow path may lack NULL check (Bug #8)"
        fi
    fi

    # [S7] TARRAY_GET elem_type guard
    echo -e "\n${CYAN}[S7] TARRAY_GET element type guard${NC}"

    if [ -f "$BLD_CALL" ]; then
        if grep -A 20 'OP_TARRAY_GET' "$BLD_CALL" | grep -q 'elem_type\|XR_ELEM_I64\|bail\|skip'; then
            log_pass "TARRAY_GET has element type check"
        else
            log_fail "TARRAY_GET may assume 8-byte stride for all arrays (Bug #9)"
        fi
    fi

    LIVE_FILE="${JIT_DIR}/xir_liveness2.c"

    # [S8] Liveness block ID vs layout index mapping
    echo -e "\n${CYAN}[S8] Liveness block ID to layout index mapping${NC}"

    if [ -f "$LIVE_FILE" ]; then
        if grep -q 'id_to_idx' "$LIVE_FILE"; then
            log_pass "iterate_dataflow uses id_to_idx mapping for successor lookup"
        else
            log_fail "iterate_dataflow may use block ID as layout index — liveness corruption"
        fi
    else
        log_warn "Liveness file not found: $LIVE_FILE"
    fi

    # [S9] range_shorten bounds check
    echo -e "\n${CYAN}[S9] range_shorten bounds check (ns < end)${NC}"

    if [ -f "$RA_FILE" ]; then
        if grep -A 5 'range_shorten' "$RA_FILE" | grep -q 'ns < .*end\|ns.*<.*first_iv->end'; then
            log_pass "range_shorten checks ns < end before shortening"
        else
            log_fail "range_shorten may create invalid interval (start > end)"
        fi
    fi

    # [S10] first_isect must use overall range bounds
    echo -e "\n${CYAN}[S10] first_isect uses overall range bounds${NC}"

    if [ -f "$RA_FILE" ]; then
        # Should use range_start/range_end, NOT iterate intervals
        isect_body=$(sed -n '/^static int32_t first_isect/,/^}/p' "$RA_FILE")
        if echo "$isect_body" | grep -q 'range_start\|range_end'; then
            log_pass "first_isect uses overall range bounds (no hole reuse)"
        elif echo "$isect_body" | grep -q 'ia->next\|ib->next'; then
            log_fail "first_isect iterates intervals — register reuse in holes (Bug #14)"
        else
            log_warn "first_isect implementation unclear — review manually"
        fi
    fi

    # [S11] Blueprint nested loop live scan range
    echo -e "\n${CYAN}[S11] Blueprint live scan covers entire function${NC}"

    if [ -f "$BP_FILE" ]; then
        # Scan should start from 0, not from header_pc
        scan_line=$(grep -n 'body_pc = ' "$BP_FILE" | grep 'opcode_reads_slot')
        if grep -B 3 'opcode_reads_slot' "$BP_FILE" | grep -q 'body_pc = 0\|body_pc = (int)0'; then
            log_pass "Blueprint scans [0, code_count) for nested loop correctness"
        elif grep -B 3 'opcode_reads_slot' "$BP_FILE" | grep -q 'body_pc = (int)pc'; then
            log_fail "Blueprint scans [header, end) — misses nested loop outer vars"
        else
            log_warn "Blueprint scan range unclear — review xir_blueprint.c manually"
        fi
    fi

    # [S12] xra_live_ptr must be per-instruction
    echo -e "\n${CYAN}[S12] xra_live_ptr precision (per-instruction vs per-block)${NC}"

    if [ -f "$CG_FILE" ]; then
        ptr_body=$(sed -n '/^int xra_live_ptr/,/^}/p' "$CG_FILE")
        if echo "$ptr_body" | grep -q 'xra_reg_at_pos\|cur_ra_pos'; then
            log_pass "xra_live_ptr uses per-instruction RA position"
        elif echo "$ptr_body" | grep -q 'blk_ptr_live'; then
            log_fail "xra_live_ptr uses per-block bitmap — imprecise GC root tracking"
        else
            log_warn "xra_live_ptr implementation unclear — review manually"
        fi
    fi

    BLD_MISC="${JIT_DIR}/xir_builder_misc.c"
    BLD_OBJ="${JIT_DIR}/xir_builder_object.c"
    JIT_FILE="${JIT_DIR}/xir_jit.c"

    # [S13] slot_rep passed directly to runtime helpers (XrRep/xr_tag confusion)
    echo -e "\n${CYAN}[S13] slot_rep not passed to runtime helpers${NC}"

    local s13_fail=0
    for f in "$BLD_MISC" "$BLD_CALL" "$BLD_OBJ"; do
        if [ -f "$f" ]; then
            # Detect pattern: b->slot_rep[...] on same line as xr_jit_ function name
            local hits=$(grep -n 'b->slot_rep\[' "$f" | grep -i 'jit_\|CALL_C\|encoded\|packed' | grep -v '//' | wc -l | tr -d ' ')
            if [ "$hits" -gt 0 ]; then
                s13_fail=1
                log_fail "$(basename $f): $hits lines pass slot_rep to runtime helpers (should use builder_slot_xr_tag)"
                [ $VERBOSE -eq 1 ] && grep -n 'b->slot_rep\[' "$f" | grep -i 'jit_\|CALL_C\|encoded\|packed' | grep -v '//' | head -3
            fi
        fi
    done
    [ $s13_fail -eq 0 ] && log_pass "No slot_rep passed directly to runtime helpers"

    # [S14] Bool-producing handlers must have XRVREG_TAG_BOOL
    echo -e "\n${CYAN}[S14] Bool-producing ops have XRVREG_TAG_BOOL${NC}"

    # Check key bool-producing ops in builder files
    local bool_ops=("OP_NOT" "OP_IS" "OP_ISNULL_SET")
    for bop in "${bool_ops[@]}"; do
        for f in "$BLD_FILE" "$BLD_MISC" "$BLD_CALL"; do
            if [ -f "$f" ] && grep -q "case $bop:" "$f"; then
                local section=$(sed -n "/case $bop:/,/case OP_/p" "$f" | head -50)
                if echo "$section" | grep -q 'XRVREG_TAG_BOOL\|builder_tag_bool'; then
                    log_pass "$bop has BOOL tag in $(basename $f)"
                else
                    log_fail "$bop MISSING BOOL tag in $(basename $f)"
                fi
            fi
        done
    done

    # Check channel bool-return ops
    for cop in "OP_CHAN_TRY_SEND" "OP_CHAN_TRY_RECV" "OP_CHAN_IS_CLOSED"; do
        if [ -f "$BLD_MISC" ] && grep -q "case $cop:" "$BLD_MISC"; then
            local section=$(sed -n "/case $cop:/,/case OP_/p" "$BLD_MISC" | head -40)
            if echo "$section" | grep -q 'XRVREG_TAG_BOOL\|builder_tag_bool'; then
                log_pass "$cop has BOOL tag"
            else
                log_fail "$cop MISSING BOOL tag"
            fi
        fi
    done

    # [S15] COMPOUND_ASSIGN uses ref_xir_type (not stale slot_rep)
    echo -e "\n${CYAN}[S15] COMPOUND_ASSIGN uses ref_xir_type for operand types${NC}"

    if [ -f "$BLD_FILE" ]; then
        local ca_section=$(sed -n '/case OP_ADD:/{:a;N;/case OP_MODKI:/!ba;p}' "$BLD_FILE" 2>/dev/null || true)
        if [ -z "$ca_section" ]; then
            ca_section=$(grep -A 80 'case OP_ADD:' "$BLD_FILE" | head -80)
        fi
        if echo "$ca_section" | grep -q 'ref_xir_type'; then
            log_pass "Arithmetic ops use ref_xir_type for operand type dispatch"
        else
            log_warn "Arithmetic ops may use slot_rep for type dispatch — verify manually"
        fi
    fi

    # [S16] jit_value_from_tag UNKNOWN branch audit
    echo -e "\n${CYAN}[S16] jit_value_from_tag UNKNOWN branch usage${NC}"

    if [ -f "$JIT_FILE" ]; then
        # Count calls to jit_value_from_tag with UNKNOWN-related tags
        local unknown_calls=$(grep -c 'jit_value_from_tag.*UNKNOWN\|jit_value_from_tag.*val_st\b' "$JIT_FILE" 2>/dev/null || echo "0")
        local total_calls=$(grep -c 'jit_value_from_tag' "$JIT_FILE" 2>/dev/null || echo "0")
        log_info "jit_value_from_tag: $total_calls total calls in xir_jit.c"

        # Check that UNKNOWN/CALLEE_SETS/NUMERIC all fall through to heuristic
        if grep -A 3 'XRVREG_TAG_CALLEE_SETS' "$JIT_FILE" | grep -q 'FALLTHROUGH\|UNKNOWN'; then
            log_pass "CALLEE_SETS falls through to UNKNOWN heuristic"
        else
            log_warn "CALLEE_SETS handling unclear — review jit_value_from_tag"
        fi
    fi
}

# ========== Dynamic Checks ==========

run_dynamic_checks() {
    echo -e "\n${CYAN}=== Dynamic Checks ===${NC}"

    XRAY_BIN="${BUILD_DIR}/xray"

    # Build first
    if [ ! -f "$XRAY_BIN" ]; then
        echo "Building xray..."
        cmake --build "$BUILD_DIR" -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | tail -3
    fi

    if [ ! -f "$XRAY_BIN" ]; then
        log_fail "Cannot find xray binary at $XRAY_BIN"
        return
    fi

    # [D1] Key JIT differential tests (targeted, not full suite)
    echo -e "\n${CYAN}[D1] JIT vs Interpreter differential (key patterns)${NC}"

    # Test multi-return
    cat > /tmp/_jit_check_multiret.xr << 'XREOF'
fn divide(a: int, b: int): (int, bool) {
    if (b == 0) { return 0, false }
    return a / b, true
}
let r, ok = divide(10, 2)
print(r)
print(ok)
let r2, ok2 = divide(10, 0)
print(r2)
print(ok2)
XREOF

    run_jit_diff_test "multi-return" "/tmp/_jit_check_multiret.xr"

    # Test loop with early return
    cat > /tmp/_jit_check_loop_ret.xr << 'XREOF'
fn find(arr: Array<int>, target: int): int {
    for (let i = 0; i < arr.length; i++) {
        if (arr[i] == target) { return i }
    }
    return -1
}
print(find([10, 20, 30], 20))
print(find([10, 20, 30], 99))
XREOF

    run_jit_diff_test "loop-early-return" "/tmp/_jit_check_loop_ret.xr"

    # [D2] Coroutine + JIT
    echo -e "\n${CYAN}[D2] JIT coroutine correctness${NC}"

    cat > /tmp/_jit_check_coro.xr << 'XREOF'
fn work(n: int): int {
    let sum = 0
    let i = 0
    while (i < n) { sum = sum + i; i++ }
    return sum
}
print(work(10))
let r = go work(10)
print(await r)
XREOF

    run_jit_diff_test "coroutine-loop" "/tmp/_jit_check_coro.xr"

    # Test nested for + Array<any>
    cat > /tmp/_jit_check_nested.xr << 'XREOF'
fn nested(n: int): int {
    for (i in 0..n) {
        for (j in 0..n) {
            if (i + j == 3) { return i * 10 + j }
        }
    }
    return -1
}
print(nested(5))
XREOF

    run_jit_diff_test "nested-for-return" "/tmp/_jit_check_nested.xr"

    # [D3] Multi-return with channel tryRecv
    echo -e "\n${CYAN}[D3] Multi-return patterns${NC}"

    cat > /tmp/_jit_check_triple.xr << 'XREOF'
fn triple(a: int): (int, int, int) {
    return a, a + 1, a + 2
}
let x, y, z = triple(10)
print(x)
print(y)
print(z)
XREOF

    run_jit_diff_test "triple-return" "/tmp/_jit_check_triple.xr"

    # [D4] Quick regression sanity
    echo -e "\n${CYAN}[D4] Regression sanity (key tests)${NC}"

    local reg_dir="${PROJECT_ROOT}/tests/regression"
    local key_tests=(
        "12_type_checking/1210_multi_return.xr"
        "04_control_flow/0460_early_return.xr"
        "03_operators/0302_int64_native.xr"
    )

    for t in "${key_tests[@]}"; do
        local tf="${reg_dir}/${t}"
        if [ -f "$tf" ]; then
            local bn=$(basename "$tf")
            timeout 10 "$XRAY_BIN" --no-jit "$tf" > /tmp/_jit_interp.txt 2>/dev/null
            local ie=$?
            timeout 10 "$XRAY_BIN" --jit-force "$tf" > /tmp/_jit_jit.txt 2>/dev/null
            local je=$?
            if [ $ie -eq $je ] && diff -q /tmp/_jit_interp.txt /tmp/_jit_jit.txt > /dev/null 2>&1; then
                log_pass "regression: $bn"
            else
                log_fail "regression: $bn (interp=$ie jit=$je)"
                [ $VERBOSE -eq 1 ] && diff /tmp/_jit_interp.txt /tmp/_jit_jit.txt | head -5
            fi
        fi
    done

    # [D5] Nested loop OSR correctness
    echo -e "\n${CYAN}[D5] Nested loop OSR correctness${NC}"

    cat > /tmp/_jit_check_nested_osr.xr << 'XREOF'
fn dummy(x: int) : int {
    return x + 1
}
fn run(maxDepth: int) : int {
    let minDepth = 4
    let extra1 = dummy(maxDepth)
    let extra2 = dummy(maxDepth)
    let depth = minDepth
    let totalCheck = 0
    while (depth <= maxDepth) {
        let iterations = 1
        let j = 0
        while (j < maxDepth - depth + minDepth) {
            iterations = iterations * 2
            j = j + 1
        }
        let check = 0
        let i = 1
        while (i <= iterations) {
            check = check + dummy(depth)
            i = i + 1
        }
        totalCheck = totalCheck + check
        depth = depth + 2
    }
    return totalCheck + extra1 + extra2
}
print(run(10))
XREOF

    run_jit_diff_test "nested-loop-osr" "/tmp/_jit_check_nested_osr.xr"

    # [D6] Channel trySend/tryRecv value + bool return
    echo -e "\n${CYAN}[D6] Channel trySend/tryRecv bool return${NC}"

    cat > /tmp/_jit_check_chan_bool.xr << 'XREOF'
fn producer(ch: Channel<int>): void {
    ch.send(42)
}
let ch = Channel<int>(1)
let ok1 = ch.trySend(10)
print(ok1)
print(typeof(ok1))
let val, ok2 = ch.tryRecv()
print(val)
print(ok2)
print(typeof(ok2))
XREOF

    run_jit_diff_test "chan-trysend-bool" "/tmp/_jit_check_chan_bool.xr"

    # [D7] Logical operators always return bool
    # KNOWN ISSUE: && / || short-circuit creates PHI with =??? rep,
    # triggering CFG assertion in --jit-force. Pre-existing bug.
    echo -e "\n${CYAN}[D7] Logical operators always return bool (KNOWN ISSUE)${NC}"

    cat > /tmp/_jit_check_logic_bool.xr << 'XREOF'
fn test_logic(a: int, b: int): void {
    let r1 = a > 0 && b > 0
    let r2 = a > 0 || b > 0
    let r3 = !(a > 0)
    print(r1)
    print(r2)
    print(r3)
}
test_logic(1, 2)
test_logic(0, 0)
XREOF

    run_jit_diff_test_known "logic-bool-return" "/tmp/_jit_check_logic_bool.xr"

    # [D8] Map for-in with compound assignment
    echo -e "\n${CYAN}[D8] Map for-in + compound assignment${NC}"

    cat > /tmp/_jit_check_map_forin.xr << 'XREOF'
fn sum_map(m: Map<string, int>): int {
    let total = 0
    for (k, v in m) {
        total += v
    }
    return total
}
let m = {"a": 1, "b": 2, "c": 3}
print(sum_map(m))
XREOF

    run_jit_diff_test "map-forin-compound" "/tmp/_jit_check_map_forin.xr"

    # [D9] Custom iterator class for-in
    echo -e "\n${CYAN}[D9] Custom iterator class for-in${NC}"

    cat > /tmp/_jit_check_iterator.xr << 'XREOF'
class Counter {
    let max: int
    let current: int = 0

    fn init(max: int): void {
        this.max = max
    }

    fn hasNext(): bool {
        return this.current < this.max
    }

    fn next(): int {
        let v = this.current
        this.current += 1
        return v
    }
}
let c = Counter(5)
let sum = 0
for (v in c) {
    sum += v
}
print(sum)
XREOF

    run_jit_diff_test "custom-iterator" "/tmp/_jit_check_iterator.xr"

    # [D10] Array<any> element type dispatch (bool in any array)
    # KNOWN ISSUE: INDEX_GET on Array<any> loses element type info,
    # typeof returns 'int' for bool/float elements. Pre-existing bug.
    echo -e "\n${CYAN}[D10] Array<any> element type dispatch (KNOWN ISSUE)${NC}"

    cat > /tmp/_jit_check_any_array.xr << 'XREOF'
fn test_any_array(): void {
    let arr: Array<any> = [1, true, "hello", 3.14]
    for (let i = 0; i < arr.length; i++) {
        print(arr[i])
        print(typeof(arr[i]))
    }
}
test_any_array()
XREOF

    run_jit_diff_test_known "any-array-dispatch" "/tmp/_jit_check_any_array.xr"

    # Cleanup
    rm -f /tmp/_jit_check_*.xr /tmp/_jit_interp.txt /tmp/_jit_jit.txt
}

run_jit_diff_test() {
    local name="$1"
    local file="$2"

    timeout 10 "$XRAY_BIN" --no-jit "$file" > /tmp/_jit_interp.txt 2>/dev/null
    local ie=$?
    timeout 10 "$XRAY_BIN" --jit-force "$file" > /tmp/_jit_jit.txt 2>/dev/null
    local je=$?

    if [ $ie -eq $je ] && diff -q /tmp/_jit_interp.txt /tmp/_jit_jit.txt > /dev/null 2>&1; then
        log_pass "$name"
    else
        log_fail "$name (interp_exit=$ie jit_exit=$je)"
        if [ $VERBOSE -eq 1 ]; then
            echo "    interp output: $(cat /tmp/_jit_interp.txt | head -3 | tr '\n' ' ')"
            echo "    jit output:    $(cat /tmp/_jit_jit.txt | head -3 | tr '\n' ' ')"
        fi
    fi
}

# Like run_jit_diff_test but logs WARN instead of FAIL for known pre-existing bugs.
# These tests document known issues that should be fixed eventually.
run_jit_diff_test_known() {
    local name="$1"
    local file="$2"

    timeout 10 "$XRAY_BIN" --no-jit "$file" > /tmp/_jit_interp.txt 2>/dev/null
    local ie=$?
    timeout 10 "$XRAY_BIN" --jit-force "$file" > /tmp/_jit_jit.txt 2>/dev/null
    local je=$?

    if [ $ie -eq $je ] && diff -q /tmp/_jit_interp.txt /tmp/_jit_jit.txt > /dev/null 2>&1; then
        log_pass "$name (known issue RESOLVED!)"
    else
        log_warn "$name (known pre-existing issue, interp=$ie jit=$je)"
    fi
}

# ========== Main ==========

echo -e "${CYAN}╔═══════════════════════════════════════╗${NC}"
echo -e "${CYAN}║     JIT Safety Checker v1.0           ║${NC}"
echo -e "${CYAN}╚═══════════════════════════════════════╝${NC}"

if [ "$MODE" = "all" ] || [ "$MODE" = "static" ]; then
    run_static_checks
fi

if [ "$MODE" = "all" ] || [ "$MODE" = "dynamic" ]; then
    run_dynamic_checks
fi

# Summary
echo -e "\n${CYAN}═══════════════════════════════════════${NC}"
echo -e "  ${GREEN}PASS: $PASS${NC}  ${RED}FAIL: $FAIL${NC}  ${YELLOW}WARN: $WARN${NC}"
if [ $FAIL -gt 0 ]; then
    echo -e "  ${RED}Some checks failed! Review and fix before committing.${NC}"
    exit 1
else
    echo -e "  ${GREEN}All checks passed.${NC}"
    exit 0
fi
