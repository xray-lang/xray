/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcgen_prescan.c - Pre-compilation analysis passes for AOT C codegen
 *
 * KEY CONCEPT:
 *   Analysis passes that run before the main codegen body loop in xcgen.c.
 *   These transform or annotate the XIR function to guide C emission:
 *     - Block reachability (BFS from entry)
 *     - Dead vreg elimination (seed-then-propagate)
 *     - Struct promotion (JSON shape → C struct pointer vregs)
 *     - LOAD_FIELD retyping (narrow → TAGGED for non-promoted instances)
 *     - Boolean method result retyping (I64 → TAGGED for true/false print)
 *     - Void return detection (always-null returns → C void)
 *     - Non-escaping closure analysis (eliminate closure allocation)
 *
 * RELATED MODULES:
 *   - xcgen.c: main compilation loop that calls these passes
 *   - xcgen.h: shared types and function declarations
 */

#include "xcgen.h"
#include "../runtime/value/xchunk.h"  // XrProto full definition (for return_type_info)
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../jit/xir_intrinsic.h"
#include "xrt_method_symbols.h"

/* ========== Block Reachability ========== */

XR_FUNC void xcg_compute_reachable(XirFunc *func, bool *reachable) {
    for (uint32_t i = 0; i < func->nblk; i++)
        reachable[i] = false;
    if (func->nblk == 0)
        return;

    // BFS from entry block; stack size scales with block count
    uint32_t stack_buf[256];
    uint32_t *stack =
        (func->nblk <= 256) ? stack_buf : (uint32_t *) xr_malloc(func->nblk * sizeof(uint32_t));
    int top = 0;
    reachable[0] = true;
    stack[top++] = 0;

    while (top > 0) {
        uint32_t bi = stack[--top];
        XirBlock *blk = func->blocks[bi];

        // Check terminator targets (s1=jump/true, s2=false branch)
        // and exception_handler (catch block reachable via longjmp).
        XirBlock *targets[3] = {blk->s1, blk->s2, blk->exception_handler};
        for (int t = 0; t < 3; t++) {
            if (!targets[t])
                continue;
            uint32_t tid = targets[t]->id;
            if (tid < func->nblk && !reachable[tid]) {
                reachable[tid] = true;
                stack[top++] = tid;
            }
        }
    }
    if (stack != stack_buf)
        xr_free(stack);
}

/* ========== Dead Vreg Elimination ========== */

// Mark a ref as used if it's a vreg
static inline void mark_ref_used(bool *used, uint32_t nvreg, XirRef ref) {
    if (xir_ref_is_vreg(ref)) {
        uint32_t vi = XIR_REF_INDEX(ref);
        if (vi < nvreg)
            used[vi] = true;
    }
}

// Offset of jit_call_args[0] in XrCoroutine (matches xcgen_call.c AOT_CALL_ARGS_BASE)
#define XCGEN_CALL_ARGS_BASE_OFFSET 688

// Check if a STORE_CORO writes to jit_call_args slot 0 (closure ptr, dead in AOT).
// Slot 0 is the closure/callee reference, never needed by the AOT C call.
static bool is_store_coro_slot0(XirFunc *func, XirIns *ins) {
    if (ins->op != XIR_STORE_CORO)
        return false;
    if (!xir_ref_is_const(ins->dst))
        return false;
    uint32_t ci = XIR_REF_INDEX(ins->dst);
    if (ci >= func->nconst)
        return false;
    return func->consts[ci].val.i64 == XCGEN_CALL_ARGS_BASE_OFFSET;
}

// Check if a STORE_CORO writes to any jit_call_args slot (argument passing)
static bool is_store_coro_call_args(XirFunc *func, XirIns *ins) {
    if (ins->op != XIR_STORE_CORO)
        return false;
    if (!xir_ref_is_const(ins->dst))
        return false;
    uint32_t ci = XIR_REF_INDEX(ins->dst);
    if (ci >= func->nconst)
        return false;
    return func->consts[ci].val.i64 >= XCGEN_CALL_ARGS_BASE_OFFSET;
}

// Scan reachable blocks to find which vregs are actually READ.
// Uses a seed-then-propagate approach:
//   Pass 1: seed from terminators, phi nodes, and live side-effect instructions
//   Pass 2: transitive closure — if vreg is used, mark its defining ins args
XR_FUNC void xcg_compute_used_vregs(XirFunc *func, bool *reachable, bool *used) {
    for (uint32_t i = 0; i < func->nvreg; i++)
        used[i] = false;
    // Params are always used
    for (uint32_t i = 0; i < func->num_params; i++)
        used[i] = true;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        if (!reachable[bi])
            continue;
        XirBlock *blk = func->blocks[bi];

        // Phi nodes: dst and all inputs are used
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            mark_ref_used(used, func->nvreg, phi->dst);
            for (uint32_t p = 0; p < phi->narg; p++) {
                mark_ref_used(used, func->nvreg, phi->args[p]);
            }
        }

        // Terminator arg (cond for BR, value for RET)
        mark_ref_used(used, func->nvreg, blk->jmp.arg);

        // Side-effect instructions: mark args + dst as live roots
        // Skip AOT-dead patterns that produce no C output
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (!(ins->flags & XIR_FLAG_SIDE_EFFECT))
                continue;

            // CALL_C: some are dead (GETSHARED closure load), but
            // xr_json_new_with_shape (struct promotion) produces live dst.
            // Mark dst as used so transitive pass picks up STORE_FIELD args.
            // Also mark vreg args directly used by CALL_C (e.g. xr_jit_throw
            // passes exception value in args[1] — a vreg, not via STORE_CORO).
            if (ins->op == XIR_CALL_C || ins->op == XIR_CALL_C_LEAF ||
                ins->op == XIR_CALL_INTRINSIC) {
                if (!xir_ref_is_none(ins->dst) && xir_ref_is_vreg(ins->dst)) {
                    mark_ref_used(used, func->nvreg, ins->dst);
                }
                // Mark vreg operands (args[1] for throw, etc.)
                mark_ref_used(used, func->nvreg, ins->args[1]);
                continue;
            }

            // CALL_KNOWN/CALL_SELF_DIRECT: args are proto const + nargs vreg,
            // both dead in AOT. dst resolved by transitive pass.
            if (ins->op == XIR_CALL_KNOWN || ins->op == XIR_CALL_KNOWN_REG ||
                ins->op == XIR_CALL_SELF_DIRECT)
                continue;

            // STORE_CORO to call_args slots: args[0] is the stored value.
            // Slot 0 (closure ptr) is dead in AOT, but we still mark it for
            // conservativeness — it will be DCE'd by the instruction emitter.
            if (is_store_coro_call_args(func, ins)) {
                if (!is_store_coro_slot0(func, ins))
                    mark_ref_used(used, func->nvreg, ins->args[0]);
                continue;
            }

            // STORE_FIELD: args[0]=base, args[1]=value (dst is const byte_offset)
            // Force-mark all vreg args including the stored value.
            // Also transitively mark the value vreg's def chain.
            if (ins->op == XIR_STORE_FIELD) {
                mark_ref_used(used, func->nvreg, ins->args[0]);
                mark_ref_used(used, func->nvreg, ins->args[1]);
                continue;
            }

            // Other side-effect instructions: mark args + dst as used
            mark_ref_used(used, func->nvreg, ins->args[0]);
            mark_ref_used(used, func->nvreg, ins->args[1]);
            mark_ref_used(used, func->nvreg, ins->dst);
        }
    }

    // Seed from call_arg_pool: mark vreg references in the pool as used.
    // The call_arg_pool stores XirRef entries (vreg/const refs) for each
    // call instruction's arguments, indexed by vreg->call_arg_start.
    // Without this, vregs passed as call arguments but never read by a
    // STORE_CORO or the instruction's direct args would be wrongly DCE'd.
    if (func->call_arg_pool) {
        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            if (!reachable[bi])
                continue;
            XirBlock *blk = func->blocks[bi];
            for (uint32_t i = 0; i < blk->nins; i++) {
                XirIns *ins = &blk->ins[i];
                if (!xir_ref_is_vreg(ins->dst))
                    continue;
                uint32_t vi = XIR_REF_INDEX(ins->dst);
                if (vi >= func->nvreg)
                    continue;
                XirVReg *vreg = &func->vregs[vi];
                if (vreg->call_nargs == 0)
                    continue;
                uint32_t start = vreg->call_arg_start;
                for (uint16_t k = 0; k < vreg->call_nargs; k++) {
                    mark_ref_used(used, func->nvreg, func->call_arg_pool[start + k]);
                }
            }
        }
    }

    // Transitive: if vreg is used, mark its defining instruction's args
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            if (!reachable[bi])
                continue;
            XirBlock *blk = func->blocks[bi];
            for (uint32_t i = 0; i < blk->nins; i++) {
                XirIns *ins = &blk->ins[i];

                // Side-effect instructions with no dst vreg still need arg propagation
                // STORE_FIELD: base=args[0] always needed; value=args[1] always needed
                // STORE_CORO: value=args[0] always needed (already seeded, propagate)
                // STORE/STORE8/STORE16/STORE32/STORE_F32: addr=args[0] + value=args[1]
                if (ins->op == XIR_STORE_FIELD || ins->op == XIR_STORE_CORO ||
                    ins->op == XIR_STORE || ins->op == XIR_STORE8 || ins->op == XIR_STORE16 ||
                    ins->op == XIR_STORE32 || ins->op == XIR_STORE_F32) {
                    if (xir_ref_is_vreg(ins->args[0])) {
                        uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                        if (vi < func->nvreg && !used[vi]) {
                            used[vi] = true;
                            changed = true;
                        }
                    }
                    if ((ins->op == XIR_STORE_FIELD || ins->op == XIR_STORE ||
                         ins->op == XIR_STORE8 || ins->op == XIR_STORE16 ||
                         ins->op == XIR_STORE32 || ins->op == XIR_STORE_F32) &&
                        xir_ref_is_vreg(ins->args[1])) {
                        uint32_t vi = XIR_REF_INDEX(ins->args[1]);
                        if (vi < func->nvreg && !used[vi]) {
                            used[vi] = true;
                            changed = true;
                        }
                    }
                    continue;
                }

                if (xir_ref_is_none(ins->dst) || !xir_ref_is_vreg(ins->dst))
                    continue;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg || !used[dvi])
                    continue;

                // dst is used → mark input args as used.
                // args[1] of CALL_KNOWN/CALL_SELF_DIRECT is the nargs vreg, dead in AOT.
                // args[1] of CALL_C/CALL_C_LEAF is the nargs/encoded constant vreg;
                // emit_call_c chases the def chain directly and never reads the vreg.
                if (xir_ref_is_vreg(ins->args[0])) {
                    uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                    if (vi < func->nvreg && !used[vi]) {
                        used[vi] = true;
                        changed = true;
                    }
                }
                bool skip_args1 = (ins->op == XIR_CALL_KNOWN || ins->op == XIR_CALL_KNOWN_REG ||
                                   ins->op == XIR_CALL_SELF_DIRECT || ins->op == XIR_CALL_C ||
                                   ins->op == XIR_CALL_C_LEAF);
                if (!skip_args1 && xir_ref_is_vreg(ins->args[1])) {
                    uint32_t vi = XIR_REF_INDEX(ins->args[1]);
                    if (vi < func->nvreg && !used[vi]) {
                        used[vi] = true;
                        changed = true;
                    }
                }

                // Propagate liveness through call_arg_pool: when a CALL
                // dst is used, mark all pool-bound arg vregs as used.
                // builder_bind_call_args stores args in vreg's call_arg pool
                // (not STORE_CORO), so the seed pass above doesn't see them.
                bool is_call = (ins->op == XIR_CALL_KNOWN || ins->op == XIR_CALL_KNOWN_REG ||
                                ins->op == XIR_CALL_SELF_DIRECT || ins->op == XIR_CALL_DIRECT ||
                                ins->op == XIR_CALL_C || ins->op == XIR_CALL_C_LEAF);
                if (is_call && func->call_arg_pool) {
                    XirVReg *dv = &func->vregs[dvi];
                    uint32_t ca_start = dv->call_arg_start;
                    uint16_t ca_n = dv->call_nargs;
                    if (ca_start + ca_n <= func->call_arg_pool_used) {
                        for (uint16_t ci = 0; ci < ca_n; ci++) {
                            XirRef ca_ref = func->call_arg_pool[ca_start + ci];
                            if (xir_ref_is_vreg(ca_ref)) {
                                uint32_t cvi = XIR_REF_INDEX(ca_ref);
                                if (cvi < func->nvreg && !used[cvi]) {
                                    used[cvi] = true;
                                    changed = true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

/* ========== Struct Promotion Prescan ========== */

// Pre-scan XIR instructions to identify vregs that will be struct-promoted,
// and retype getprop dst vregs from I64 to TAGGED (XrtValue).
//
// Also infers which parameter vregs are always used as a specific promoted struct.
// When a param vreg appears as the base of LOAD_FIELD/STORE_FIELD with offsets that
// uniquely match a registered struct, the param is tagged in vreg_struct_id.
// This lets field accesses on params use the named struct path (->field) instead
// of raw byte pointer arithmetic, enabling cleaner code and better C optimizer hints.
// Correctness: the named path ((xrs_N*)v.ptr)->field and the raw path
// *(T*)((char*)v.ptr + offset) are bit-for-bit identical because the struct
// preserves XrtValue (16B) layout for all XrtValue fields.
XR_FUNC void xcg_prescan_struct_vregs(XcgenModule *mod, XcgenFunc *cf) {
    if (!mod->struct_reg || mod->struct_reg->nstructs == 0)
        return;
    XirFunc *func = cf->xfunc;

    // Pass 1: after intrinsic resolution, scan CALL_INTRINSIC instructions
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (ins->op != XIR_CALL_INTRINSIC)
                continue;
            if (!xir_ref_is_const(ins->args[0]))
                continue;
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            if (ci >= func->nconst)
                continue;
            int intrin_id = (int) func->consts[ci].val.i64;

            // JSON_NEW_SHAPE: mark dst as promoted struct
            if (intrin_id == XR_INTRIN_JSON_NEW_SHAPE && cf->vreg_struct_id) {
                void *shape_ptr = NULL;
                if (xir_ref_is_const(ins->args[1])) {
                    uint32_t si = XIR_REF_INDEX(ins->args[1]);
                    if (si < func->nconst)
                        shape_ptr = func->consts[si].val.ptr;
                }
                int sidx = shape_ptr ? xcgen_find_struct(mod->struct_reg, shape_ptr) : -1;
                if (sidx >= 0 && xir_ref_is_vreg(ins->dst)) {
                    uint32_t dst_vi = XIR_REF_INDEX(ins->dst);
                    if (dst_vi < func->nvreg)
                        cf->vreg_struct_id[dst_vi] = (int16_t) sidx;
                    cf->needs_gc = true;
                }
            }

            // GETPROP: retype dst from I64 to TAGGED
            // In AOT, getprop returns XrtValue (struct field) not raw int64
            if (intrin_id == XR_INTRIN_GETPROP) {
                if (xir_ref_is_vreg(ins->dst)) {
                    uint32_t dst_vi = XIR_REF_INDEX(ins->dst);
                    if (dst_vi < func->nvreg)
                        func->vregs[dst_vi].rep = XR_REP_TAGGED;
                }
            }
        }
    }

    // Pass 2: infer struct type for parameter vregs from field access patterns.
    // For each param vreg used as LOAD_FIELD/STORE_FIELD base, collect all
    // byte offsets that appear.  A struct matches if ALL observed offsets are
    // valid field offsets in that struct.  On a unique match, tag the param.
    if (!cf->vreg_struct_id || func->num_params == 0)
        return;

    // candidate_struct[param_idx]: struct index that matches so far (-1 = none, -2 = conflict)
    int candidate[32];
    int ncand = (func->num_params < 32) ? func->num_params : 32;
    for (int p = 0; p < ncand; p++)
        candidate[p] = -1;  // -1 = no evidence yet

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            XirRef base_ref = XIR_NONE;
            int64_t offset = -1;

            if (ins->op == XIR_LOAD_FIELD && xir_ref_is_vreg(ins->args[0]) &&
                xir_ref_is_const(ins->args[1])) {
                base_ref = ins->args[0];
                uint32_t oci = XIR_REF_INDEX(ins->args[1]);
                if (oci < func->nconst)
                    offset = func->consts[oci].val.i64;
            } else if (ins->op == XIR_STORE_FIELD && xir_ref_is_vreg(ins->args[0]) &&
                       xir_ref_is_const(ins->dst)) {
                base_ref = ins->args[0];
                uint32_t oci = XIR_REF_INDEX(ins->dst);
                if (oci < func->nconst)
                    offset = func->consts[oci].val.i64;
            }
            if (xir_ref_is_none(base_ref) || offset < 0)
                continue;

            uint32_t bvi = XIR_REF_INDEX(base_ref);
            // Only care about parameter vregs
            if (bvi >= (uint32_t) ncand)
                continue;
            // Skip if already confirmed non-matching or already tagged
            if (candidate[bvi] == -2)
                continue;
            if (cf->vreg_struct_id[bvi] >= 0)
                continue;  // already tagged by pass1

            // Find which registered struct has this offset as a valid field
            int matched_sidx = -1;
            for (int sidx = 0; sidx < mod->struct_reg->nstructs; sidx++) {
                XcgenStruct *st = &mod->struct_reg->structs[sidx];
                if (xcgen_field_by_offset(st, offset) >= 0) {
                    if (matched_sidx == -1) {
                        matched_sidx = sidx;
                    } else {
                        // Offset matches multiple structs — ambiguous, skip
                        matched_sidx = -2;
                        break;
                    }
                }
            }
            if (matched_sidx < 0) {
                // No matching struct or ambiguous: mark as conflict
                candidate[bvi] = -2;
                continue;
            }
            // Check consistency: if we've seen this param before with a different struct
            if (candidate[bvi] == -1) {
                candidate[bvi] = matched_sidx;
            } else if (candidate[bvi] != matched_sidx) {
                candidate[bvi] = -2;  // conflicting struct types observed
            }
        }
    }

    // Apply unambiguous candidates to vreg_struct_id
    for (int p = 0; p < ncand; p++) {
        if (candidate[p] >= 0 && cf->vreg_struct_id[p] < 0) {
            cf->vreg_struct_id[p] = (int16_t) candidate[p];
        }
    }
}

/* ========== Field Load Retyping ========== */

// Retype LOAD_FIELD dst vregs to TAGGED when the base object is NOT a
// promoted struct.  Non-promoted instances store each field as a 16-byte
// XrtValue slot; reading only 8 bytes (I64) loses the tag and breaks
// string/ptr/array fields.  The auto-boxing/unboxing helpers
// (xcg_emit_ref_as_tagged / xcg_emit_ref_as_native) handle downstream
// conversions transparently, so this retyping is safe for all consumers.
//
// Runs unconditionally — does not require struct registration.
XR_FUNC void xcg_retype_field_loads(XcgenFunc *cf) {
    XR_DCHECK(cf != NULL, "xcg_retype_field_loads: NULL cf");
    XirFunc *func = cf->xfunc;
    if (!func)
        return;
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (ins->op != XIR_LOAD_FIELD)
                continue;
            if (!xir_ref_is_vreg(ins->dst))
                continue;
            // Check if base vreg is a promoted struct — if so, the
            // struct-promotion codegen path handles types correctly.
            if (xir_ref_is_vreg(ins->args[0]) && cf->vreg_struct_id) {
                uint32_t base_vi = XIR_REF_INDEX(ins->args[0]);
                if (base_vi < func->nvreg && cf->vreg_struct_id[base_vi] >= 0)
                    continue;  // promoted struct — skip retyping
            }
            uint32_t dst_vi = XIR_REF_INDEX(ins->dst);
            if (dst_vi < func->nvreg)
                func->vregs[dst_vi].rep = XR_REP_TAGGED;
        }
    }
}

/* ========== Boolean Method Result Retyping ========== */

// Returns true if the given method symbol returns a boolean value.
static bool is_bool_method(int method_symbol) {
    switch (method_symbol) {
        case XRT_SYM_CONTAINS:
        case XRT_SYM_STARTSWITH:
        case XRT_SYM_ENDSWITH:
        case XRT_SYM_IS_EMPTY:
        case XRT_SYM_INCLUDES:
        case XRT_SYM_HAS:
            return true;
        default:
            return false;
    }
}

// Retype dst vregs of boolean-returning method calls from I64 to TAGGED.
// Without this, the codegen path does xrt_unbox_int(xrt_method_N(...)) which
// discards the BOOL tag, causing xrt_println to print "1"/"0" instead of
// "true"/"false".  By retyping to TAGGED, the fallback xrt_method_N path
// returns XrtValue directly (preserving the tag), and inline paths produce
// xrt_box_bool() instead of raw 1/0.
XR_FUNC void xcg_retype_bool_method_results(XcgenFunc *cf) {
    XR_DCHECK(cf != NULL, "xcg_retype_bool_method_results: NULL cf");
    XirFunc *func = cf->xfunc;
    if (!func)
        return;
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (ins->op != XIR_CALL_INTRINSIC)
                continue;
            if (!xir_ref_is_vreg(ins->dst))
                continue;
            if (!xir_ref_is_const(ins->args[0]))
                continue;
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            if (ci >= func->nconst)
                continue;
            if (func->consts[ci].val.i64 != XR_INTRIN_INVOKE_METHOD)
                continue;
            // Decode method_symbol from args[1]
            int64_t encoded = 0;
            if (!xcg_resolve_const_i64(func, ins->args[1], &encoded))
                continue;
            if (encoded < 0)
                continue;  // TOSTRING, not method call
            int method_symbol = (int) (encoded >> 32);
            if (!is_bool_method(method_symbol))
                continue;
            uint32_t dst_vi = XIR_REF_INDEX(ins->dst);
            if (dst_vi < func->nvreg)
                func->vregs[dst_vi].rep = XR_REP_TAGGED;
        }
    }
}

/* ========== Void Return Detection ========== */

// Returns true if every block terminator in this function is a JMP_RET returning null.
// JMP_RET is a block terminator in blk->jmp, not an instruction in blk->ins[].
// When true, the AOT function can be emitted as void instead of XrtValue.
XR_FUNC bool xcg_detect_void_return(XirFunc *func) {
    // Only TAGGED returns can be void when every return value is null.
    XrRep ret_rep = (func->proto && func->proto->return_type_info)
                        ? xr_type_rep(func->proto->return_type_info)
                        : XR_REP_TAGGED;
    if (ret_rep != XR_REP_TAGGED)
        return false;
    bool found_ret = false;
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (blk->jmp.type != XIR_JMP_RET)
            continue;
        found_ret = true;
        XirRef rv = blk->jmp.arg;
        // XIR_NONE arg means implicit null return (RETURN0)
        if (xir_ref_is_none(rv))
            continue;
        if (xir_ref_is_const(rv)) {
            // Const return: check if it's the null constant (ptr/tagged type, value=0)
            uint32_t ci = XIR_REF_INDEX(rv);
            if (ci < func->nconst) {
                uint8_t ct = func->consts[ci].rep;
                if ((ct == XR_REP_PTR || ct == XR_REP_TAGGED) && func->consts[ci].val.i64 == 0)
                    continue;
            }
            return false;
        }
        if (xir_ref_is_vreg(rv)) {
            // Vreg: trace back to its single definition; accept XIR_CONST_PTR(null)
            uint32_t vi = XIR_REF_INDEX(rv);
            bool is_null_vreg = false;
            for (uint32_t bi2 = 0; bi2 < func->nblk && !is_null_vreg; bi2++) {
                XirBlock *blk2 = func->blocks[bi2];
                for (uint32_t ii = 0; ii < blk2->nins; ii++) {
                    XirIns *def = &blk2->ins[ii];
                    if (!xir_ref_is_vreg(def->dst))
                        continue;
                    if (XIR_REF_INDEX(def->dst) != vi)
                        continue;
                    if (def->op == XIR_CONST_PTR && xir_ref_is_const(def->args[0])) {
                        uint32_t ci2 = XIR_REF_INDEX(def->args[0]);
                        if (ci2 < func->nconst && func->consts[ci2].val.i64 == 0)
                            is_null_vreg = true;
                    }
                    break;
                }
            }
            if (!is_null_vreg)
                return false;
            continue;
        }
        return false;
    }
    return found_ret;
}

/* ========== Non-Escaping Closure Analysis ========== */
//
// A closure created in function F is "non-escaping" if the vreg holding it
// is ONLY used as:
//   (a) dst of XIR_STORE_UPVAL (setting upvalues on the child closure), or
//   (b) call_arg_pool slot 0 of a XIR_CALL_KNOWN targeting the same proto.
// Any other reference (returned, stored to heap, passed as regular arg,
// indirect call via CALL_DIRECT) means the closure escapes.
//
// When non-escaping, the child function receives upvalues as extra XrtValue
// parameters instead of through xrt_closure_t*, eliminating:
//   - xrt_closure_new heap allocation
//   - xrt_cl->upvals[i] indirect memory access
//   - closure object entirely

// Resolve a const ref in a XIR function to its raw pointer value (for fn_ptr).
static void *resolve_const_ptr(XirFunc *func, XirRef ref) {
    if (xir_ref_is_const(ref)) {
        uint32_t ci = XIR_REF_INDEX(ref);
        if (ci < func->nconst)
            return (void *) (uintptr_t) func->consts[ci].val.raw;
    }
    return NULL;
}

// Lookup XcgenProtoEntry by proto pointer (global compilation registry).
// Used by xcgen_compile_func for non-escaping closure detection.
XR_FUNC XcgenProtoEntry *xcg_lookup_proto_entry(XcgenModule *mod, void *proto_ptr) {
    XR_DCHECK(mod != NULL && mod->comp != NULL, "xcg_lookup_proto_entry: NULL");
    XcgenCompilation *comp = mod->comp;
    for (int i = 0; i < comp->proto_map_count; i++) {
        if (comp->proto_map[i].proto_ptr == proto_ptr)
            return &comp->proto_map[i];
    }
    return NULL;
}

// Check if vreg cl_vreg (holding a closure of cl_proto) escapes from func.
// Returns true if the closure does NOT escape.
static bool closure_is_non_escaping(XirFunc *func, uint32_t cl_vreg, void *cl_proto,
                                    XcgenModule *mod XR_UNUSED) {
    XR_DCHECK(func != NULL, "closure_is_non_escaping: NULL func");
    XR_DCHECK(cl_vreg < func->nvreg, "closure_is_non_escaping: invalid vreg");

    // Scan all instruction args for references to cl_vreg.
    // STORE_UPVAL(args[0]=cl_vreg) is safe; any other use escapes.
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            XirIns *ins = &blk->ins[ii];
            // Check dst: Definition site (CALL_C creating the closure) is ok
            if (xir_ref_is_vreg(ins->dst) && XIR_REF_INDEX(ins->dst) == cl_vreg) {
                if (ins->op == XIR_CALL_C || ins->op == XIR_CALL_C_LEAF)
                    continue;
                return false;  // unexpected redefinition
            }
            // Check args[0..1] for any reference to cl_vreg
            for (int a = 0; a < 2; a++) {
                if (xir_ref_is_vreg(ins->args[a]) && XIR_REF_INDEX(ins->args[a]) == cl_vreg) {
                    // STORE_UPVAL(args[0]=cl_vreg) is safe (upval init)
                    if (ins->op == XIR_STORE_UPVAL && a == 0)
                        continue;
                    return false;  // used as arg → escaping
                }
            }
        }
        // Also check PHI nodes
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t p = 0; p < phi->narg; p++) {
                if (xir_ref_is_vreg(phi->args[p]) && XIR_REF_INDEX(phi->args[p]) == cl_vreg) {
                    return false;  // flows through PHI → might escape
                }
            }
        }
    }

    // Scan call_arg_pool for references to cl_vreg.
    // Slot 0 of a CALL_KNOWN targeting the same proto is safe; anything else escapes.
    if (func->call_arg_pool) {
        for (uint32_t vi = 0; vi < func->nvreg; vi++) {
            XirVReg *vr = &func->vregs[vi];
            if (vr->call_nargs == 0)
                continue;
            for (uint16_t si = 0; si < vr->call_nargs; si++) {
                XirRef ref = func->call_arg_pool[vr->call_arg_start + si];
                if (!xir_ref_is_vreg(ref) || XIR_REF_INDEX(ref) != cl_vreg)
                    continue;
                // cl_vreg found in call arg pool at slot si
                if (si != 0)
                    return false;  // not closure slot → escaping
                // Slot 0: verify it's a CALL_KNOWN targeting the same proto
                XirIns *def = vr->def;
                if (!def)
                    return false;
                if (def->op != XIR_CALL_KNOWN && def->op != XIR_CALL_KNOWN_REG)
                    return false;  // not a direct call → escaping
                void *callee = resolve_const_ptr(func, def->args[0]);
                if (callee != cl_proto)
                    return false;  // different target → escaping
            }
        }
    }

    return true;  // all uses are safe → non-escaping
}

// Pre-scan a function for child closure creation sites and run escape analysis.
// Marks non-escaping closures in proto_map so child functions get the right
// signature when compiled later.
XR_FUNC void xcg_prescan_closure_escape(XcgenModule *mod, XirFunc *func) {
    XR_DCHECK(mod != NULL, "xcg_prescan_closure_escape: NULL mod");
    XR_DCHECK(func != NULL, "xcg_prescan_closure_escape: NULL func");

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            XirIns *ins = &blk->ins[ii];
            if (ins->op != XIR_CALL_C && ins->op != XIR_CALL_C_LEAF)
                continue;
            if (!xir_ref_is_vreg(ins->dst))
                continue;

            // Try to resolve fn_ptr: first from call_arg_pool, then from args[0]
            uint32_t dst_vi = XIR_REF_INDEX(ins->dst);
            void *fn_ptr = NULL;
            if (func->call_arg_pool && func->vregs[dst_vi].call_nargs > 0) {
                XirRef pool_ref = func->call_arg_pool[func->vregs[dst_vi].call_arg_start];
                fn_ptr = resolve_const_ptr(func, pool_ref);
            }
            if (!fn_ptr)
                fn_ptr = resolve_const_ptr(func, ins->args[0]);
            if (!fn_ptr)
                continue;

            // Check if fn_ptr is a registered proto (closure creation)
            XcgenProtoEntry *entry = xcg_lookup_proto_entry(mod, fn_ptr);
            if (!entry)
                continue;

            // Resolve nupvals
            int64_t nupvals = 0;
            xcg_resolve_const_i64(func, ins->args[1], &nupvals);
            if (nupvals <= 0)
                continue;

            // Run escape analysis
            if (closure_is_non_escaping(func, dst_vi, fn_ptr, mod)) {
                entry->non_escaping = true;
                entry->num_upvals = (int) nupvals;
            }
        }
    }
}
