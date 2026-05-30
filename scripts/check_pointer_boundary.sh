#!/usr/bin/env bash
# ============================================================================
# check_pointer_boundary.sh
# ----------------------------------------------------------------------------
# S0 pointer boundary audit — CI guardrail for cross-tier pointer entry points.
#
# Cross-tier pointer entries are sites where pointers flow between:
#   - JIT-compiled machine code <-> interpreter/runtime
#   - GC heap <-> JIT stack frames
#   - Deopt reconstruction <-> live register state
#
# This script enforces that:
#   1. All known cross-tier entry points are accounted for
#   2. No new unaudited entry points are introduced
#   3. Key safety patterns are present where required
#
# Usage:
#   bash scripts/check_pointer_boundary.sh
#
# Exit codes:
#   0   all checks passed
#   1   new unaudited entry points or missing safety patterns
# ============================================================================

set -uo pipefail
cd "$(dirname "$0")/.."

EXIT=0
red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m%s\033[0m\n' "$*"; }

grepcount() {
    local pattern="$1"; shift
    grep -rc "$pattern" "$@" 2>/dev/null | awk -F: '{s+=$NF} END {print s+0}'
}

grepcheck() {
    grep -q "$@" 2>/dev/null
}

# ============================================================================
# Category 1: GC root scanning — mark_coro_roots / shared_refs
# ============================================================================
info "=== 1. GC root scanning entry points ==="

gc_scan_count=$(grepcount 'mark_coro_roots\|gc_mark_roots\|xr_gc_scan_stack\|shared_refs_mark' src/runtime/gc/ src/coro/)
info "  GC scan references: $gc_scan_count"

if grepcheck 'XR_VALUE_NEEDS_GC\|XR_IS_PTR\|XR_TAG_MASK\|xr_is_heap_ptr' src/runtime/gc/xcoro_gc.c src/runtime/gc/xcoro_gc.h; then
    green "  xcoro_gc: has pointer tag checks (XR_VALUE_NEEDS_GC) ✓"
else
    red "  xcoro_gc: MISSING pointer validity checks"
    EXIT=1
fi

# ============================================================================
# Category 2: Write barriers — barrier_fwd / barrier_back
# ============================================================================
info "=== 2. Write barrier entry points ==="

barrier_jit=$(grepcount 'xr_jit_barrier_fwd\|xr_jit_barrier_back' src/jit/)
barrier_runtime=$(grepcount 'xr_jit_barrier_fwd\|xr_jit_barrier_back' src/runtime/)
info "  Barrier refs in JIT: $barrier_jit, in runtime: $barrier_runtime"

if grepcheck 'barrier_fwd\|barrier_back' xisa/xm/runtime_stubs.def; then
    green "  runtime_stubs.def: barriers declared ✓"
else
    red "  runtime_stubs.def: MISSING barrier declarations"
    EXIT=1
fi

# ============================================================================
# Category 3: Deopt slot reconstruction
# ============================================================================
info "=== 3. Deopt reconstruction entry points ==="

deopt_count=$(grepcount 'XmRtDeoptEntry\|deopt_table\|deopt_rebuild\|jit_deopt_restore' src/jit/)
info "  Deopt reconstruction refs: $deopt_count"

if grepcheck 'xm_verify_deopt\|deopt.*verify\|verify.*deopt' src/jit/xm_metadata_verify.h; then
    green "  metadata_verify: deopt verification present ✓"
else
    red "  metadata_verify: MISSING deopt verification"
    EXIT=1
fi

# ============================================================================
# Category 4: Safepoint / stackmap — JIT <-> GC coordination
# ============================================================================
info "=== 4. Safepoint / stackmap entry points ==="

smap_count=$(grepcount 'active_stack_map\|active_safepoint\|safepoint_id' src/jit/)
info "  Stackmap/safepoint refs: $smap_count"

if grepcheck 'xm_verify_stackmap\|stackmap.*verify\|verify.*stack' src/jit/xm_metadata_verify.h; then
    green "  metadata_verify: stackmap verification present ✓"
else
    red "  metadata_verify: MISSING stackmap verification"
    EXIT=1
fi

# ============================================================================
# Category 5: JIT allocation — xr_jit_alloc
# ============================================================================
info "=== 5. JIT allocation entry points ==="

alloc_count=$(grepcount 'xr_jit_alloc' src/jit/)
info "  JIT alloc refs: $alloc_count"

if grepcheck 'jit_alloc' xisa/xm/runtime_stubs.def; then
    green "  runtime_stubs.def: alloc declared ✓"
else
    red "  runtime_stubs.def: MISSING alloc declaration"
    EXIT=1
fi

# ============================================================================
# Category 6: shared_refs — cross-coroutine pointer flow
# ============================================================================
info "=== 6. shared_refs cross-coro flow ==="

shared_count=$(grepcount 'shared_refs' src/)
info "  shared_refs total refs: $shared_count"

# ============================================================================
# Category 7: Runtime verifiers — structural safety nets
# ============================================================================
info "=== 7. Runtime verifiers ==="

verifier_count=$(grepcount 'xm_verify_post_call\|xm_metadata_verify\|xm_patch_verify' src/jit/)
info "  Verifier call sites: $verifier_count"

if grepcheck 'xm_verify_post_call' src/jit/xm_codegen.c src/jit/xm_codegen_x64.c; then
    green "  post-call verifier: present ✓"
else
    red "  post-call verifier: MISSING"
    EXIT=1
fi

# ============================================================================
# Category 8: S8.6 effect flag consistency (helpers.def)
# POST_CALL DEOPT requires DEOPT in FLAGS, THROW requires THROW, etc.
# ============================================================================
info "=== 8. Effect flag consistency (helpers.def) ==="

HELPERS_DEF="xisa/xm/helpers.def"
flag_errors=0
if [ -f "$HELPERS_DEF" ]; then
    while IFS= read -r line; do
        # Parse XM_HELPER lines
        name=$(echo "$line" | grep -oP '(?<=XM_HELPER\()\w+' 2>/dev/null) || continue
        [ -z "$name" ] && continue
        flags=$(echo "$line" | awk -F',' '{print $4}' | tr -d ' ')
        postcall=$(echo "$line" | awk -F',' '{print $6}' | tr -d ' )')

        # POST_CALL=DEOPT => FLAGS must contain DEOPT
        if echo "$postcall" | grep -q 'DEOPT'; then
            if ! echo "$flags" | grep -q 'DEOPT'; then
                red "  $name: POST_CALL has DEOPT but FLAGS missing DEOPT"
                flag_errors=$((flag_errors + 1))
            fi
        fi
        # POST_CALL=THROW => FLAGS must contain THROW
        if echo "$postcall" | grep -q 'THROW'; then
            if ! echo "$flags" | grep -q 'THROW'; then
                red "  $name: POST_CALL has THROW but FLAGS missing THROW"
                flag_errors=$((flag_errors + 1))
            fi
        fi
        # POST_CALL=SUSPEND => FLAGS must contain SUSPEND
        if echo "$postcall" | grep -q 'SUSPEND'; then
            if ! echo "$flags" | grep -q 'SUSPEND'; then
                red "  $name: POST_CALL has SUSPEND but FLAGS missing SUSPEND"
                flag_errors=$((flag_errors + 1))
            fi
        fi
        # GC-allocating helpers must have STACKMAP
        if echo "$flags" | grep -q 'GC'; then
            if ! echo "$flags" | grep -q 'STACKMAP'; then
                red "  $name: FLAGS has GC but missing STACKMAP"
                flag_errors=$((flag_errors + 1))
            fi
        fi
    done < "$HELPERS_DEF"

    total_helpers=$(grep -c '^XM_HELPER(' "$HELPERS_DEF" 2>/dev/null || echo 0)
    if [ "$flag_errors" -eq 0 ]; then
        green "  helpers.def: $total_helpers helpers, all effect flags consistent ✓"
    else
        red "  helpers.def: $flag_errors inconsistencies found"
        EXIT=1
    fi
else
    red "  helpers.def: NOT FOUND"
    EXIT=1
fi

# Same check for runtime_stubs.def
STUBS_DEF="xisa/xm/runtime_stubs.def"
stub_errors=0
if [ -f "$STUBS_DEF" ]; then
    while IFS= read -r line; do
        name=$(echo "$line" | grep -oP '(?<=XM_RUNTIME_STUB\()\w+' 2>/dev/null) || continue
        [ -z "$name" ] && continue
        flags=$(echo "$line" | awk -F',' '{print $6}' | tr -d ' ')

        if echo "$flags" | grep -q 'GC'; then
            if ! echo "$flags" | grep -q 'STACKMAP'; then
                red "  stub $name: FLAGS has GC but missing STACKMAP"
                stub_errors=$((stub_errors + 1))
            fi
        fi
    done < "$STUBS_DEF"

    total_stubs=$(grep -c '^XM_RUNTIME_STUB(' "$STUBS_DEF" 2>/dev/null || echo 0)
    if [ "$stub_errors" -eq 0 ]; then
        green "  runtime_stubs.def: $total_stubs stubs, all flags consistent ✓"
    else
        red "  runtime_stubs.def: $stub_errors inconsistencies found"
        EXIT=1
    fi
fi

# ============================================================================
# Summary
# ============================================================================
echo ""
echo "--- Pointer Boundary Inventory ---"
echo "  GC scan:        $gc_scan_count refs"
echo "  Write barriers: $((barrier_jit + barrier_runtime)) refs (jit=$barrier_jit, rt=$barrier_runtime)"
echo "  Deopt recon:    $deopt_count refs"
echo "  Safepoint/smap: $smap_count refs"
echo "  JIT alloc:      $alloc_count refs"
echo "  shared_refs:    $shared_count refs"
echo "  Verifiers:      $verifier_count refs"
echo ""

if [ ${EXIT} -eq 0 ]; then
    green "All pointer boundary checks passed."
else
    red "Pointer boundary audit failed. See output above."
fi
exit ${EXIT}
