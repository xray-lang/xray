/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcgen.c - AOT C code generator main logic
 *
 * KEY CONCEPT:
 *   Module-level orchestration: creates sections, compiles each XIR function,
 *   generates forward declarations, and assembles the final C source file.
 *
 * RELATED MODULES:
 *   - xcgen_expr.c: expression/instruction translation
 *   - xcgen_stmt.c: control flow (terminators, phi lowering)
 */

#include "xcgen.h"
#include "../runtime/value/xchunk.h"
#include "../base/xchecks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../base/xmalloc.h"

/* ========== Dynamic String Buffer ========== */

void xcgen_buf_init(XcgenBuf *b) {
    XR_DCHECK(b != NULL, "xcgen_buf_init: b is NULL");
    b->cap = 4096;
    b->data = (char *)xr_malloc(b->cap);
    if (!b->data) { b->cap = 0; b->len = 0; return; }
    b->data[0] = '\0';
    b->len = 0;
}

void xcgen_buf_free(XcgenBuf *b) {
    xr_free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void xcgen_buf_ensure(XcgenBuf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t new_cap = b->cap;
        while (b->len + extra + 1 > new_cap) new_cap *= 2;
        char *new_data = (char *)xr_realloc(b->data, new_cap);
        if (!new_data) return;
        b->data = new_data;
        b->cap = new_cap;
    }
}

void xcgen_buf_puts(XcgenBuf *b, const char *s) {
    XR_DCHECK(b != NULL, "xcgen_buf_puts: b is NULL");
    XR_DCHECK(s != NULL, "xcgen_buf_puts: s is NULL");
    size_t slen = strlen(s);
    xcgen_buf_ensure(b, slen);
    memcpy(b->data + b->len, s, slen + 1);
    b->len += slen;
}

void xcgen_buf_append(XcgenBuf *dst, const XcgenBuf *src) {
    if (!src || src->len == 0) return;
    xcgen_buf_ensure(dst, src->len);
    memcpy(dst->data + dst->len, src->data, src->len);
    dst->len += src->len;
    dst->data[dst->len] = '\0';
}

__attribute__((format(printf, 2, 3)))
void xcgen_buf_printf(XcgenBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) return;
    xcgen_buf_ensure(b, (size_t)needed);
    va_start(ap, fmt);
    b->len += vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
}

/* ========== Type Helpers ========== */

const char *xcg_c_type(uint8_t xir_type) {
    switch (xir_type) {
        case XR_REP_F64:    return "double";
        case XR_REP_STR:
        case XR_REP_PTR:
        case XR_REP_TAGGED: return "XrValue";
        case XR_REP_I64:
        default:              return "int64_t";
    }
}

bool xcg_is_float_type(uint8_t xir_type) {
    return xir_type == XR_REP_F64;
}

static bool xcg_is_tagged_type(uint8_t xir_type) {
    return xir_type == XR_REP_STR || xir_type == XR_REP_TAGGED || xir_type == XR_REP_PTR;
}

// Convert XrSlotType (from proto metadata) to C type string
static const char *xcg_c_type_for_slot(uint8_t slot_type) {
    switch (slot_type) {
        case 9: // XR_SLOT_F32
        case 10: /* XR_SLOT_F64 */  return "double";
        case 12: /* XR_SLOT_PTR */  return "XrValue";
        case 0:  /* XR_SLOT_ANY */  return "XrValue";
        default:                    return "int64_t";
    }
}

// Derive C type string directly from XrType* (no lossy downgrade)
static const char *xcg_c_type_for_xrtype(struct XrType *t) {
    if (!t) return "XrValue";
    XrRep rep = xr_type_rep(t);
    switch (rep) {
        case XR_REP_F64:    return "double";
        case XR_REP_I64:    return "int64_t";
        case XR_REP_PTR:
        case XR_REP_STR:
        case XR_REP_TAGGED: return "XrValue";
        default:            return "XrValue";
    }
}

/* ========== Module Lifecycle ========== */

XcgenModule *xcgen_module_new(void) {
    XcgenModule *mod = (XcgenModule *)xr_calloc(1, sizeof(XcgenModule));
    if (!mod) return NULL;
    for (int i = 0; i < XCGEN_SEC_COUNT; i++) {
        xcgen_buf_init(&mod->sections[i]);
    }
    mod->funcs_cap = 16;
    mod->funcs = (XcgenFunc *)xr_calloc(mod->funcs_cap, sizeof(XcgenFunc));
    mod->proto_map_cap = 64;
    mod->proto_map = (XcgenProtoEntry *)xr_malloc(mod->proto_map_cap * sizeof(XcgenProtoEntry));
    if (!mod->funcs || !mod->proto_map) {
        xr_free(mod->funcs);
        xr_free(mod->proto_map);
        xr_free(mod);
        return NULL;
    }
    return mod;
}

void xcgen_register_proto(XcgenModule *mod, void *proto_ptr, const char *c_name) {
    if (!mod || !proto_ptr || !c_name) return;
    if (mod->proto_map_count >= mod->proto_map_cap) {
        uint32_t new_cap = mod->proto_map_cap * 2;
        XcgenProtoEntry *new_map = (XcgenProtoEntry *)xr_realloc(mod->proto_map,
                         new_cap * sizeof(XcgenProtoEntry));
        if (!new_map) return;
        mod->proto_map = new_map;
        mod->proto_map_cap = new_cap;
    }
    XcgenProtoEntry *e = &mod->proto_map[mod->proto_map_count++];
    e->proto_ptr = proto_ptr;
    e->c_name = c_name;
    e->func_idx = -1;  // set after xcgen_compile_func
}

const char *xcg_lookup_proto_name(XcgenModule *mod, void *proto_ptr) {
    for (int i = 0; i < mod->proto_map_count; i++) {
        if (mod->proto_map[i].proto_ptr == proto_ptr)
            return mod->proto_map[i].c_name;
    }
    return NULL;
}

// Returns the index of the compiled function in mod->funcs, or -1 if not found.
// Callers must use mod->funcs[idx] to get the actual XcgenFunc — never cache
// the pointer across calls that may trigger mod->funcs realloc.
int xcg_lookup_proto_func_idx(XcgenModule *mod, void *proto_ptr) {
    for (int i = 0; i < mod->proto_map_count; i++) {
        if (mod->proto_map[i].proto_ptr == proto_ptr)
            return mod->proto_map[i].func_idx;
    }
    return -1;
}

XcgenFunc *xcg_lookup_proto_cf(XcgenModule *mod, void *proto_ptr) {
    int fi = xcg_lookup_proto_func_idx(mod, proto_ptr);
    if (fi >= 0 && fi < mod->nfuncs) return &mod->funcs[fi];
    return NULL;
}

void xcgen_module_free(XcgenModule *mod) {
    if (!mod) return;
    for (int i = 0; i < XCGEN_SEC_COUNT; i++) {
        xcgen_buf_free(&mod->sections[i]);
    }
    for (int i = 0; i < mod->nfuncs; i++) {
        xcgen_buf_free(&mod->funcs[i].body);
        xr_free(mod->funcs[i].vreg_struct_id);
        xr_free(mod->funcs[i].call_args);
    }
    xr_free(mod->funcs);
    xr_free(mod->proto_map);
    xr_free(mod);
}

/* ========== Forward Declaration Generation ========== */

// Get the C type string for a parameter vreg, considering struct promotion.
// Returns "xrs_N*" when the param has been identified as a promoted struct ptr.
static const char *xcg_param_c_type(XcgenModule *mod, XcgenFunc *cf, int param_idx) {
    if (cf->vreg_struct_id && param_idx < (int)cf->xfunc->nvreg) {
        int si = cf->vreg_struct_id[param_idx];
        if (si >= 0 && mod->struct_reg && si < mod->struct_reg->nstructs) {
            return mod->struct_reg->structs[si].c_name;  // e.g. "xrs_7"
            // Caller appends "*"
        }
    }
    // Read type directly from proto (no lossy downgrade through XrSlotType)
    XrProto *proto = cf->xfunc->proto;
    if (proto && proto->param_types && param_idx < proto->param_types_count)
        return xcg_c_type_for_xrtype(proto->param_types[param_idx]);
    return "XrValue";
}

// Returns true if param i is a promoted struct (should be xrs_N*, not XrtValue)
static bool xcg_param_is_struct(XcgenModule *mod, XcgenFunc *cf, int param_idx) {
    if (!cf->vreg_struct_id || param_idx >= cf->num_params) return false;
    int si = cf->vreg_struct_id[param_idx];
    return si >= 0 && mod->struct_reg && si < mod->struct_reg->nstructs;
}

static void xcgen_emit_forward_decl(XcgenModule *mod, XcgenFunc *cf) {
    XcgenBuf *fwd = &mod->sections[XCGEN_SEC_FORWARD];
    XirFunc *func = cf->xfunc;

    const char *ret_type = cf->void_return ? "void"
        : xcg_c_type_for_xrtype(func->proto ? func->proto->return_type_info : NULL);
    xcgen_buf_printf(fwd, "static %s %s(", ret_type, cf->c_name);
    bool has_params = false;
    xcgen_buf_puts(fwd, "XrtContext");
    has_params = true;
    if (cf->needs_closure_param) {
        xcgen_buf_puts(fwd, ", xrt_closure_t*");
    }
    for (int i = 0; i < func->num_params; i++) {
        if (has_params || i > 0) xcgen_buf_puts(fwd, ", ");
        if (xcg_param_is_struct(mod, cf, i)) {
            xcgen_buf_printf(fwd, "%s*", xcg_param_c_type(mod, cf, i));
        } else {
            xcgen_buf_printf(fwd, "%s", xcg_param_c_type(mod, cf, i));
        }
        has_params = true;
    }
    if (!has_params) xcgen_buf_puts(fwd, "void");
    xcgen_buf_puts(fwd, ");\n");
}

/* ========== Single Function Compilation ========== */

// Block reachability: BFS from entry to find live blocks
static void compute_reachable(XirFunc *func, bool *reachable) {
    for (uint32_t i = 0; i < func->nblk; i++) reachable[i] = false;
    if (func->nblk == 0) return;

    // BFS from entry block; stack size scales with block count
    uint32_t stack_buf[256];
    uint32_t *stack = (func->nblk <= 256) ? stack_buf
                     : (uint32_t *)xr_malloc(func->nblk * sizeof(uint32_t));
    int top = 0;
    reachable[0] = true;
    stack[top++] = 0;

    while (top > 0) {
        uint32_t bi = stack[--top];
        XirBlock *blk = func->blocks[bi];

        // Check terminator targets (s1=jump/true, s2=false branch)
        // Use block->id for O(1) index lookup instead of O(n) pointer scan
        XirBlock *targets[2] = { blk->s1, blk->s2 };
        for (int t = 0; t < 2; t++) {
            if (!targets[t]) continue;
            uint32_t tid = targets[t]->id;
            if (tid < func->nblk && !reachable[tid]) {
                reachable[tid] = true;
                stack[top++] = tid;
            }
        }
    }
    if (stack != stack_buf) xr_free(stack);
}

// Mark a ref as used if it's a vreg
static inline void mark_ref_used(bool *used, uint32_t nvreg, XirRef ref) {
    if (xir_ref_is_vreg(ref)) {
        uint32_t vi = XIR_REF_INDEX(ref);
        if (vi < nvreg) used[vi] = true;
    }
}

// Offset of jit_call_args[0] in XrCoroutine (matches xcgen_call.c AOT_CALL_ARGS_BASE)
#define XCGEN_CALL_ARGS_BASE_OFFSET  688

// Check if a STORE_CORO writes to jit_call_args slot 0 (closure ptr, dead in AOT).
// Slot 0 is the closure/callee reference, never needed by the AOT C call.
static bool is_store_coro_slot0(XirFunc *func, XirIns *ins) {
    if (ins->op != XIR_STORE_CORO) return false;
    if (!xir_ref_is_const(ins->dst)) return false;
    uint32_t ci = XIR_REF_INDEX(ins->dst);
    if (ci >= func->nconst) return false;
    return func->consts[ci].val.i64 == XCGEN_CALL_ARGS_BASE_OFFSET;
}

// Check if a STORE_CORO writes to any jit_call_args slot (argument passing)
static bool is_store_coro_call_args(XirFunc *func, XirIns *ins) {
    if (ins->op != XIR_STORE_CORO) return false;
    if (!xir_ref_is_const(ins->dst)) return false;
    uint32_t ci = XIR_REF_INDEX(ins->dst);
    if (ci >= func->nconst) return false;
    return func->consts[ci].val.i64 >= XCGEN_CALL_ARGS_BASE_OFFSET;
}

// Scan reachable blocks to find which vregs are actually READ.
// Uses a seed-then-propagate approach:
//   Pass 1: seed from terminators, phi nodes, and live side-effect instructions
//   Pass 2: transitive closure — if vreg is used, mark its defining ins args
static void compute_used_vregs(XirFunc *func, bool *reachable, bool *used) {
    for (uint32_t i = 0; i < func->nvreg; i++) used[i] = false;
    // Params are always used
    for (uint32_t i = 0; i < func->num_params; i++) used[i] = true;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        if (!reachable[bi]) continue;
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
            if (!(ins->flags & XIR_FLAG_SIDE_EFFECT)) continue;

            // CALL_C: some are dead (GETSHARED closure load), but
            // xr_json_new_with_shape (struct promotion) produces live dst.
            // Mark dst as used so transitive pass picks up STORE_FIELD args.
            if (ins->op == XIR_CALL_C || ins->op == XIR_CALL_C_LEAF) {
                // Check if this produces a value (has vreg dst)
                if (!xir_ref_is_none(ins->dst) && xir_ref_is_vreg(ins->dst)) {
                    mark_ref_used(used, func->nvreg, ins->dst);
                }
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

    // Transitive: if vreg is used, mark its defining instruction's args
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            if (!reachable[bi]) continue;
            XirBlock *blk = func->blocks[bi];
            for (uint32_t i = 0; i < blk->nins; i++) {
                XirIns *ins = &blk->ins[i];

                // Side-effect instructions with no dst vreg still need arg propagation
                // STORE_FIELD: base=args[0] always needed; value=args[1] always needed
                // STORE_CORO: value=args[0] always needed (already seeded, propagate)
                // STORE/STORE8/STORE16/STORE32/STORE_F32: addr=args[0] + value=args[1]
                if (ins->op == XIR_STORE_FIELD || ins->op == XIR_STORE_CORO ||
                    ins->op == XIR_STORE || ins->op == XIR_STORE8 ||
                    ins->op == XIR_STORE16 || ins->op == XIR_STORE32 ||
                    ins->op == XIR_STORE_F32) {
                    if (xir_ref_is_vreg(ins->args[0])) {
                        uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                        if (vi < func->nvreg && !used[vi]) { used[vi] = true; changed = true; }
                    }
                    if ((ins->op == XIR_STORE_FIELD || ins->op == XIR_STORE ||
                         ins->op == XIR_STORE8 || ins->op == XIR_STORE16 ||
                         ins->op == XIR_STORE32 || ins->op == XIR_STORE_F32) &&
                        xir_ref_is_vreg(ins->args[1])) {
                        uint32_t vi = XIR_REF_INDEX(ins->args[1]);
                        if (vi < func->nvreg && !used[vi]) { used[vi] = true; changed = true; }
                    }
                    continue;
                }

                if (xir_ref_is_none(ins->dst) || !xir_ref_is_vreg(ins->dst))
                    continue;
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi >= func->nvreg || !used[dvi]) continue;

                // dst is used → mark input args as used.
                // args[1] of CALL_KNOWN/CALL_SELF_DIRECT is the nargs vreg, dead in AOT.
                // args[1] of CALL_C/CALL_C_LEAF is the nargs/encoded constant vreg;
                // emit_call_c chases the def chain directly and never reads the vreg.
                if (xir_ref_is_vreg(ins->args[0])) {
                    uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                    if (vi < func->nvreg && !used[vi]) {
                        used[vi] = true; changed = true;
                    }
                }
                bool skip_args1 = (ins->op == XIR_CALL_KNOWN ||
                                   ins->op == XIR_CALL_KNOWN_REG ||
                                   ins->op == XIR_CALL_SELF_DIRECT ||
                                   ins->op == XIR_CALL_C ||
                                   ins->op == XIR_CALL_C_LEAF);
                if (!skip_args1 && xir_ref_is_vreg(ins->args[1])) {
                    uint32_t vi = XIR_REF_INDEX(ins->args[1]);
                    if (vi < func->nvreg && !used[vi]) {
                        used[vi] = true; changed = true;
                    }
                }
            }
        }
    }
}

#include "xcgen_bridge.h"

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
static void prescan_struct_vregs(XcgenModule *mod, XcgenFunc *cf) {
    if (!mod->struct_reg || mod->struct_reg->nstructs == 0)
        return;
    XirFunc *func = cf->xfunc;

    // Pass 1: mark vregs created by xr_json_new_with_shape (allocations)
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (!xir_ref_is_const(ins->args[0])) continue;
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            if (ci >= func->nconst || func->consts[ci].rep != XR_REP_PTR) continue;
            void *fn_ptr = func->consts[ci].val.ptr;

            // CALL_C(xr_json_new_with_shape): mark dst as promoted struct
            if (ins->op == XIR_CALL_C && fn_ptr == (void *)xr_json_new_with_shape
                && cf->vreg_struct_id) {
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
                        cf->vreg_struct_id[dst_vi] = (int16_t)sidx;
                    cf->needs_gc = true;
                }
            }

            // CALL_C_LEAF(xr_jit_getprop): retype dst from I64 to TAGGED
            // In AOT, getprop returns XrtValue (struct field) not raw int64
            if (ins->op == XIR_CALL_C_LEAF && fn_ptr == (void *)xr_jit_getprop) {
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
    if (!cf->vreg_struct_id || func->num_params == 0) return;

    // candidate_struct[param_idx]: struct index that matches so far (-1 = none, -2 = conflict)
    int candidate[32];
    int ncand = (func->num_params < 32) ? func->num_params : 32;
    for (int p = 0; p < ncand; p++) candidate[p] = -1;  // -1 = no evidence yet

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            XirRef base_ref = XIR_NONE;
            int64_t offset = -1;

            if (ins->op == XIR_LOAD_FIELD && xir_ref_is_vreg(ins->args[0])
                && xir_ref_is_const(ins->args[1])) {
                base_ref = ins->args[0];
                uint32_t oci = XIR_REF_INDEX(ins->args[1]);
                if (oci < func->nconst) offset = func->consts[oci].val.i64;
            } else if (ins->op == XIR_STORE_FIELD && xir_ref_is_vreg(ins->args[0])
                       && xir_ref_is_const(ins->dst)) {
                base_ref = ins->args[0];
                uint32_t oci = XIR_REF_INDEX(ins->dst);
                if (oci < func->nconst) offset = func->consts[oci].val.i64;
            }
            if (xir_ref_is_none(base_ref) || offset < 0) continue;

            uint32_t bvi = XIR_REF_INDEX(base_ref);
            // Only care about parameter vregs
            if (bvi >= (uint32_t)ncand) continue;
            // Skip if already confirmed non-matching or already tagged
            if (candidate[bvi] == -2) continue;
            if (cf->vreg_struct_id[bvi] >= 0) continue;  // already tagged by pass1

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
            cf->vreg_struct_id[p] = (int16_t)candidate[p];
        }
    }
}

// Returns true if every block terminator in this function is a JMP_RET returning null.
// JMP_RET is a block terminator in blk->jmp, not an instruction in blk->ins[].
// When true, the AOT function can be emitted as void instead of XrtValue.
static bool xcgen_detect_void_return(XirFunc *func) {
    // Only TAGGED returns can be void (any-typed → always returns null)
    XrRep ret_rep = (func->proto && func->proto->return_type_info)
        ? xr_type_rep(func->proto->return_type_info) : XR_REP_TAGGED;
    if (ret_rep != XR_REP_TAGGED) return false;
    bool found_ret = false;
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (blk->jmp.type != XIR_JMP_RET) continue;
        found_ret = true;
        XirRef rv = blk->jmp.arg;
        // XIR_NONE arg means implicit null return (RETURN0)
        if (xir_ref_is_none(rv)) continue;
        if (xir_ref_is_const(rv)) {
            // Const return: check if it's the null constant (ptr/tagged type, value=0)
            uint32_t ci = XIR_REF_INDEX(rv);
            if (ci < func->nconst) {
                uint8_t ct = func->consts[ci].rep;
                if ((ct == XR_REP_PTR || ct == XR_REP_TAGGED) &&
                    func->consts[ci].val.i64 == 0)
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
                    if (!xir_ref_is_vreg(def->dst)) continue;
                    if (XIR_REF_INDEX(def->dst) != vi) continue;
                    if (def->op == XIR_CONST_PTR && xir_ref_is_const(def->args[0])) {
                        uint32_t ci2 = XIR_REF_INDEX(def->args[0]);
                        if (ci2 < func->nconst && func->consts[ci2].val.i64 == 0)
                            is_null_vreg = true;
                    }
                    break;
                }
            }
            if (!is_null_vreg) return false;
            continue;
        }
        return false;
    }
    return found_ret;
}

static void xcgen_compile_function_body(XcgenModule *mod, XcgenFunc *cf) {
    XR_DCHECK(mod != NULL, "xcgen_compile_function_body: NULL mod");
    XR_DCHECK(cf != NULL, "xcgen_compile_function_body: NULL cf");
    XR_DCHECK(cf->xfunc != NULL, "xcgen_compile_function_body: NULL xfunc");
    XirFunc *func = cf->xfunc;
    XcgenBuf *b = &cf->body;

    const char *tagged_type = "XrValue";
    const char *ret_type = xcg_c_type_for_xrtype(
        func->proto ? func->proto->return_type_info : NULL);

    // Block reachability analysis
    bool reachable_buf[256];
    bool *reachable = (func->nblk <= 256) ? reachable_buf : xr_calloc(func->nblk, 1);
    if (!reachable) return;
    compute_reachable(func, reachable);

    // Vreg usage analysis
    bool used_buf[512];
    bool *used = (func->nvreg <= 512) ? used_buf : xr_calloc(func->nvreg, 1);
    if (!used) { if (func->nblk > 256) xr_free(reachable); return; }
    compute_used_vregs(func, reachable, used);
    cf->used_vregs = used;

    // --- Phase 1: locals buffer ---
    // Collect all local variable declarations into a separate buffer.
    // This allows stmts generation to append new locals at any point
    // without interleaving them with executable code.
    XcgenBuf locals;
    xcgen_buf_init(&locals);

    // Classify vreg types for grouped declarations
    bool has_int_locals = false, has_float_locals = false, has_tagged_locals = false;
    for (uint32_t i = func->num_params; i < func->nvreg; i++) {
        if (cf->vreg_struct_id && cf->vreg_struct_id[i] >= 0) {
            has_tagged_locals = true;  // promoted struct → XrtValue
            continue;
        }
        uint8_t vt = func->vregs[i].rep;
        if (xcg_is_float_type(vt)) has_float_locals = true;
        else if (xcg_is_tagged_type(vt)) has_tagged_locals = true;
        else has_int_locals = true;
    }
    // Phi auto-boxing may create implicit runtime dependencies
    if (has_tagged_locals) cf->needs_runtime = true;

    // Declare ALL local vregs grouped by type.
    // Declaring all (not just 'used') avoids use-before-def errors when
    // DCE/const_prop eliminate def instructions but xcgen still emits the vreg.
    if (has_int_locals) {
        xcgen_buf_puts(&locals, "    int64_t");
        bool first = true;
        for (uint32_t i = func->num_params; i < func->nvreg; i++) {
            if (cf->vreg_struct_id && cf->vreg_struct_id[i] >= 0) continue;
            uint8_t vt = func->vregs[i].rep;
            if (!xcg_is_float_type(vt) && !xcg_is_tagged_type(vt)) {
                xcgen_buf_printf(&locals, "%s v%u = 0", first ? " " : ", ", i);
                first = false;
            }
        }
        xcgen_buf_puts(&locals, ";\n");
    }
    if (has_float_locals) {
        xcgen_buf_puts(&locals, "    double");
        bool first = true;
        for (uint32_t i = func->num_params; i < func->nvreg; i++) {
            if (cf->vreg_struct_id && cf->vreg_struct_id[i] >= 0) continue;
            if (xcg_is_float_type(func->vregs[i].rep)) {
                xcgen_buf_printf(&locals, "%s v%u = 0.0", first ? " " : ", ", i);
                first = false;
            }
        }
        xcgen_buf_puts(&locals, ";\n");
    }
    if (has_tagged_locals) {
        xcgen_buf_printf(&locals, "    %s", tagged_type);
        bool first = true;
        for (uint32_t i = func->num_params; i < func->nvreg; i++) {
            bool is_promoted = (cf->vreg_struct_id && cf->vreg_struct_id[i] >= 0);
            if (is_promoted || xcg_is_tagged_type(func->vregs[i].rep)) {
                xcgen_buf_printf(&locals, "%s v%u = {0}", first ? " " : ", ", i);
                first = false;
            }
        }
        xcgen_buf_puts(&locals, ";\n");
    }

    // Phi temp variables (only for reachable blocks)
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        if (!reachable[bi]) continue;
        XirBlock *blk = func->blocks[bi];
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            uint32_t vi = XIR_REF_INDEX(phi->dst);
            xcgen_buf_printf(&locals, "    %s phi_v%u;\n",
                             xcg_c_type(phi->rep), vi);
        }
    }

    // Exception handling local
    if (cf->needs_exception)
        xcgen_buf_printf(&locals, "    %s xrt_exception = {0};\n", tagged_type);

    if (locals.len > 0)
        xcgen_buf_puts(&locals, "\n");

    // --- Phase 2: stmts buffer ---
    // Emit basic blocks (skip unreachable) into a separate stmts buffer.
    XcgenBuf stmts;
    xcgen_buf_init(&stmts);

    // ARC mode: no shadow stack needed.
    // Object lifetime is managed by retain/release inserted by XIR_RETAIN/XIR_RELEASE.
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        if (!reachable[bi]) continue;
        XirBlock *blk = func->blocks[bi];

        // Label
        xcgen_buf_printf(&stmts, "L%u:", blk->id);
        if (blk->label)
            xcgen_buf_printf(&stmts, " /* %s */", blk->label);
        xcgen_buf_puts(&stmts, "\n");

        // Copy phi temps to actual variables
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            uint32_t vi = XIR_REF_INDEX(phi->dst);
            xcgen_buf_printf(&stmts, "    v%u = phi_v%u;\n", vi, vi);
        }

        // Instructions (skip dead: unused dst + no side effects)
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (!(ins->flags & XIR_FLAG_SIDE_EFFECT) &&
                !xir_ref_is_none(ins->dst) && xir_ref_is_vreg(ins->dst)) {
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi < func->nvreg && !used[dvi]) continue;
            }
            xcg_emit_instruction(&stmts, func, ins, cf->c_name, mod, cf);
        }

        // Terminator
        xcg_emit_terminator(&stmts, func, blk, cf->c_name, cf);
    }

    // Suppress unused label warnings
    xcgen_buf_puts(&stmts, "    (void)0;\n");

    // --- Assemble: signature + locals + stmts → body ---
    // Override ret_type if function always returns null
    if (cf->void_return) ret_type = "void";
    xcgen_buf_printf(b, "static %s %s(", ret_type, cf->c_name);
    bool has_sig_params = false;
    xcgen_buf_puts(b, "XrtContext xrt_ctx");
    has_sig_params = true;
    if (cf->needs_closure_param) {
        xcgen_buf_puts(b, ", xrt_closure_t *xrt_cl");
    }
    for (int i = 0; i < func->num_params; i++) {
        if (has_sig_params || i > 0) xcgen_buf_puts(b, ", ");
        if (xcg_param_is_struct(mod, cf, i)) {
            xcgen_buf_printf(b, "%s* v%d", xcg_param_c_type(mod, cf, i), i);
        } else {
            xcgen_buf_printf(b, "%s v%d", xcg_param_c_type(mod, cf, i), i);
        }
        has_sig_params = true;
    }
    if (!has_sig_params) xcgen_buf_puts(b, "void");
    xcgen_buf_puts(b, ") {\n");
    xcgen_buf_puts(b, "    (void)xrt_ctx;\n");
    if (cf->needs_closure_param) {
        xcgen_buf_puts(b, "    (void)xrt_cl;\n");
    }
    xcgen_buf_append(b, &locals);
    xcgen_buf_append(b, &stmts);
    xcgen_buf_puts(b, "}\n");

    xcgen_buf_free(&locals);
    xcgen_buf_free(&stmts);

    // Free heap-allocated analysis buffers if used
    if (func->nblk > 256) xr_free(reachable);
    if (func->nvreg > 512) xr_free(used);
}

XcgenFunc *xcgen_compile_func(XcgenModule *mod, XirFunc *xfunc, const char *c_name) {
    if (!mod || !xfunc || !c_name) return NULL;

    // Grow func array if needed
    if (mod->nfuncs >= mod->funcs_cap) {
        uint32_t new_cap = mod->funcs_cap * 2;
        XcgenFunc *new_funcs = (XcgenFunc *)xr_realloc(mod->funcs,
                                          new_cap * sizeof(XcgenFunc));
        if (!new_funcs) return NULL;
        mod->funcs = new_funcs;
        mod->funcs_cap = new_cap;
    }

    XcgenFunc *cf = &mod->funcs[mod->nfuncs];
    memset(cf, 0, sizeof(*cf));
    cf->xfunc = xfunc;
    cf->c_name = c_name;
    cf->num_params = xfunc->num_params;
    xcgen_buf_init(&cf->body);

    // Pre-scan: detect closure param and exception handling needs
    for (uint32_t bi = 0; bi < xfunc->nblk; bi++) {
        XirBlock *blk = xfunc->blocks[bi];
        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            uint8_t op = blk->ins[ii].op;
            if (op == XIR_LOAD_UPVAL) {
                cf->needs_closure_param = true;
            }
            if (op == XIR_STORE_UPVAL && !xir_ref_is_vreg(blk->ins[ii].dst)) {
                cf->needs_closure_param = true;
            }
            if (op == XIR_CATCH) {
                cf->needs_exception = true;
            }
        }
    }

    // Detect void-return functions (always return null) before forward decl
    cf->void_return = xcgen_detect_void_return(xfunc);

    // Initialize struct promotion tracking (before forward decl so param types are known)
    if (xfunc->nvreg > 0 && mod->struct_reg && mod->struct_reg->nstructs > 0) {
        cf->vreg_struct_id = (int16_t *)xr_malloc(xfunc->nvreg * sizeof(int16_t));
        if (!cf->vreg_struct_id) return NULL;
        for (uint32_t vi = 0; vi < xfunc->nvreg; vi++)
            cf->vreg_struct_id[vi] = -1;
        prescan_struct_vregs(mod, cf);
    } else {
        cf->vreg_struct_id = NULL;
    }

    // Generate forward declaration (after struct prescan so param struct types are known)
    xcgen_emit_forward_decl(mod, cf);

    // Initialize call args buffer (heap-allocated, grows on demand)
    cf->call_args_cap = XCGEN_MAX_CALL_ARGS;
    cf->call_args = (XirRef *)xr_malloc(cf->call_args_cap * sizeof(XirRef));
    cf->call_args_count = 0;
    for (int i = 0; i < cf->call_args_cap; i++)
        cf->call_args[i] = XIR_NONE;

    // Compile function body
    xcgen_compile_function_body(mod, cf);

    // Backfill func_idx into proto_map so callers can look up struct param info
    for (int i = 0; i < mod->proto_map_count; i++) {
        if (mod->proto_map[i].c_name == c_name) {
            mod->proto_map[i].func_idx = mod->nfuncs;  // not yet incremented
            break;
        }
    }

    mod->nfuncs++;
    return cf;
}

/* ========== Final Source Assembly ========== */

char *xcgen_emit_source(XcgenModule *mod) {
    if (!mod) return NULL;

    XcgenBuf out;
    xcgen_buf_init(&out);

    // Auto-generated header comment
    xcgen_buf_puts(&out, "/* Auto-generated by xray build --native (transpile to C) */\n\n");

    // Check if any function needs runtime
    // main() always needs runtime for xrt_bump_init, xrt_println etc.
    bool any_needs_runtime = (mod->sections[XCGEN_SEC_MAIN].len > 0);
    for (int i = 0; i < mod->nfuncs; i++) {
        if (mod->funcs[i].needs_runtime) { any_needs_runtime = true; break; }
    }

    // Headers section (always emit standard headers)
    xcgen_buf_puts(&out,
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include <stdint.h>\n"
        "#include <inttypes.h>\n"
    );
    if (any_needs_runtime) {
        xcgen_buf_puts(&out, "#include \"xrt.h\"\n");
    }
    xcgen_buf_puts(&out, "\n");
    if (mod->sections[XCGEN_SEC_HEADERS].len > 0) {
        xcgen_buf_puts(&out, mod->sections[XCGEN_SEC_HEADERS].data);
        xcgen_buf_puts(&out, "\n");
    }

    // Struct typedefs (from Json promotion)
    if (mod->struct_reg && mod->struct_reg->nstructs > 0) {
        xcgen_emit_all_typedefs(&out, mod->struct_reg);
    }
    // Deinit dispatch table + xrt_arc_deinit definition (always emitted;
    // no-op when no structs have PTR fields)
    xcgen_emit_struct_deinits(&out, mod->struct_reg);

    // Types section
    if (mod->sections[XCGEN_SEC_TYPES].len > 0) {
        xcgen_buf_puts(&out, mod->sections[XCGEN_SEC_TYPES].data);
        xcgen_buf_puts(&out, "\n");
    }

    // Forward declarations
    if (mod->sections[XCGEN_SEC_FORWARD].len > 0) {
        xcgen_buf_puts(&out, "/* Forward declarations */\n");
        xcgen_buf_puts(&out, mod->sections[XCGEN_SEC_FORWARD].data);
        xcgen_buf_puts(&out, "\n");
    }

    // Static data
    if (mod->sections[XCGEN_SEC_DATA].len > 0) {
        xcgen_buf_puts(&out, mod->sections[XCGEN_SEC_DATA].data);
        xcgen_buf_puts(&out, "\n");
    }

    // Function bodies
    for (int i = 0; i < mod->nfuncs; i++) {
        xcgen_buf_puts(&out, mod->funcs[i].body.data);
        xcgen_buf_puts(&out, "\n");
    }

    // Main section
    if (mod->sections[XCGEN_SEC_MAIN].len > 0) {
        xcgen_buf_puts(&out, mod->sections[XCGEN_SEC_MAIN].data);
    }

    char *result = out.data;
    // Don't free out.data — ownership transfers to caller
    return result;
}
