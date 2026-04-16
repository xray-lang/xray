/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcgen_call.c - AOT C code generator: function call translation
 *
 * KEY CONCEPT:
 *   Translates XIR call instructions into direct C function calls.
 *   Call arguments are stored in XirFunc's call_arg_pool (indexed by
 *   dst vreg's call_arg_start/call_nargs). Before emitting each CALL,
 *   we load pool entries into cf->call_args[] for uniform access.
 *
 *   CALL_KNOWN(proto_ptr, nargs)
 *     → lookup proto name, emit: result = xr_name(arg1, arg2, ...)
 *   CALL_SELF_DIRECT
 *     → emit: result = self_name(arg0, arg1)
 *   CALL_C(fn_ptr, extra_arg)
 *     → emit C runtime function call
 *
 * RELATED MODULES:
 *   - xcgen_expr.c: delegates call ops here
 *   - xir_builder.c: generates the STORE_CORO + CALL_KNOWN sequence
 */

#include "xcgen.h"
#include "../base/xchecks.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "../jit/xir_sentinels.h"

/*
 * JIT_CALL_ARGS_OFFSET = 688 (from xir_offsets.h)
 * jit_call_args[0] at offset 688 = closure (skip in AOT)
 * jit_call_args[1] at offset 696 = arg0
 * jit_call_args[2] at offset 704 = arg1
 * ...
 * jit_call_args[n] at offset 688 + n*8
 */
#define AOT_CALL_ARGS_BASE  688
#define AOT_CALL_ARGS_SLOT_SIZE  8

// Forward declaration (defined below, after CALL_C helpers)
static void emit_ref_as_tagged(XcgenBuf *b, XirFunc *func, XirRef ref);

/* ========== Call Arg Pool → cf->call_args[] ========== */

// Populate cf->call_args[] from the call_arg_pool for the given CALL ins.
static void load_call_args_from_pool(XirFunc *func, XirIns *ins, XcgenFunc *cf) {
    // Reset call args
    for (int i = 0; i < cf->call_args_cap; i++)
        cf->call_args[i] = XIR_NONE;
    cf->call_args_count = 0;
    if (!xir_ref_is_vreg(ins->dst)) return;
    uint32_t vi = XIR_REF_INDEX(ins->dst);
    if (vi >= func->nvreg) return;
    XirVReg *vreg = &func->vregs[vi];
    if (vreg->call_nargs == 0) return;
    // Ensure capacity
    int needed = (int)vreg->call_nargs;
    if (needed > cf->call_args_cap) {
        int new_cap = cf->call_args_cap;
        while (new_cap < needed) new_cap *= 2;
        cf->call_args = (XirRef *)realloc(cf->call_args, new_cap * sizeof(XirRef));
        for (int k = cf->call_args_cap; k < new_cap; k++)
            cf->call_args[k] = XIR_NONE;
        cf->call_args_cap = new_cap;
    }
    XirRef *pool = func->call_arg_pool;
    uint32_t start = vreg->call_arg_start;
    for (uint16_t i = 0; i < vreg->call_nargs; i++)
        cf->call_args[i] = pool[start + i];
    cf->call_args_count = (int)vreg->call_nargs;
}

/* ========== CALL_KNOWN Translation ========== */

static void emit_call_known(XcgenBuf *b, XirFunc *func, XirIns *ins,
                             XcgenModule *mod, XcgenFunc *cf) {
    XR_DCHECK(b != NULL, "emit_call_known: NULL buf");
    XR_DCHECK(func != NULL, "emit_call_known: NULL func");
    XR_DCHECK(ins != NULL, "emit_call_known: NULL ins");
    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);

    // args[0] = const_ptr(callee_proto), args[1] = const(nargs)
    void *callee_proto = NULL;
    int nargs = 0;

    if (xir_ref_is_const(ins->args[0])) {
        uint32_t ci = XIR_REF_INDEX(ins->args[0]);
        if (ci < func->nconst)
            callee_proto = (void *)(uintptr_t)func->consts[ci].val.raw;
    }
    if (xir_ref_is_const(ins->args[1])) {
        uint32_t ci = XIR_REF_INDEX(ins->args[1]);
        if (ci < func->nconst)
            nargs = (int)func->consts[ci].val.i64;
    } else if (xir_ref_is_vreg(ins->args[1])) {
        // nargs was loaded via XIR_CONST_I64 into a vreg by the builder.
        // Use SSA def pointer for O(1) lookup instead of full scan.
        uint32_t nargs_vi = XIR_REF_INDEX(ins->args[1]);
        bool found_nargs = false;
        if (nargs_vi < func->nvreg && func->vregs[nargs_vi].def) {
            XirIns *def = func->vregs[nargs_vi].def;
            if (def->op == XIR_CONST_I64 && xir_ref_is_const(def->args[0])) {
                uint32_t ci2 = XIR_REF_INDEX(def->args[0]);
                if (ci2 < func->nconst) {
                    nargs = (int)func->consts[ci2].val.i64;
                    found_nargs = true;
                }
            }
        }
        // Fallback: derive from collected call_args when def not found
        if (!found_nargs && cf->call_args_count > 1)
            nargs = cf->call_args_count - 1;
    }

    // Lookup callee C function name and compiled function index (for struct param info).
    // We store func_idx rather than a pointer because mod->funcs may realloc during compilation.
    const char *callee_name     = callee_proto ? xcg_lookup_proto_name(mod, callee_proto) : NULL;
    int         callee_func_idx = callee_proto ? xcg_lookup_proto_func_idx(mod, callee_proto) : -1;

    if (callee_name) {
        // Direct C function call.
        // Check if callee always returns void (null) — never assign in that case.
        bool callee_is_void = false;
        if (callee_func_idx >= 0 && callee_func_idx < mod->nfuncs)
            callee_is_void = mod->funcs[callee_func_idx].void_return;

        // Skip assignment when dst vreg is unused (void call site) OR callee returns void.
        bool dst_used = !callee_is_void && (
                        xir_ref_is_none(ins->dst) ||
                        !xir_ref_is_vreg(ins->dst) ||
                        (dst_idx < func->nvreg && cf->used_vregs && cf->used_vregs[dst_idx]));
        if (dst_used && xir_ref_is_vreg(ins->dst))
            xcgen_buf_printf(b, "    v%u = %s(", dst_idx, callee_name);
        else
            xcgen_buf_printf(b, "    %s(", callee_name);
        // Pass xrt_ctx as implicit first argument
        bool ctx_emitted = true;
        xcgen_buf_puts(b, "xrt_ctx");
        // Pass closure pointer if callee needs it
        bool callee_needs_closure = false;
        if (callee_func_idx >= 0 && callee_func_idx < mod->nfuncs)
            callee_needs_closure = mod->funcs[callee_func_idx].needs_closure_param;
        if (callee_needs_closure && cf->call_args_count > 0 &&
            !xir_ref_is_none(cf->call_args[0])) {
            xcgen_buf_puts(b, ", (xrt_closure_t*)");
            emit_ref_as_tagged(b, func, cf->call_args[0]);
            xcgen_buf_puts(b, ".ptr");
        } else if (callee_needs_closure) {
            xcgen_buf_puts(b, ", NULL");
        }
        // call_args[0] = closure (skip), call_args[1..n] = actual args
        for (int i = 0; i < nargs; i++) {
            if (ctx_emitted || i > 0) xcgen_buf_puts(b, ", ");
            ctx_emitted = false;
            int slot = 1 + i;  // skip slot 0 (closure)
            if (slot < cf->call_args_cap && !xir_ref_is_none(cf->call_args[slot])) {
                XirRef arg_ref = cf->call_args[slot];
                uint8_t arg_type = xcg_ref_type(func, arg_ref);
                bool arg_is_tagged = (arg_type == XR_REP_PTR ||
                                      arg_type == XR_REP_TAGGED ||
                                      arg_type == XR_REP_STR);

                // Check if callee expects a struct ptr for this param index.
                // Re-derive callee_cf from mod->funcs each iteration — mod->funcs
                // may have been reallocated since callee_func_idx was stored.
                bool callee_param_is_struct = false;
                const char *callee_struct_name = NULL;
                if (callee_func_idx >= 0 && callee_func_idx < mod->nfuncs && mod->struct_reg) {
                    XcgenFunc *ccf = &mod->funcs[callee_func_idx];
                    if (ccf->vreg_struct_id && i < ccf->num_params) {
                        int csi = ccf->vreg_struct_id[i];
                        if (csi >= 0 && csi < mod->struct_reg->nstructs) {
                            callee_param_is_struct = true;
                            callee_struct_name = mod->struct_reg->structs[csi].c_name;
                        }
                    }
                }

                // Check if the arg vreg is itself a struct-ptr param (already xrs_N*, not XrtValue)
                bool arg_is_struct_ptr_param = false;
                if (xir_ref_is_vreg(arg_ref) && cf->vreg_struct_id && mod->struct_reg) {
                    uint32_t arg_vi = XIR_REF_INDEX(arg_ref);
                    if (arg_vi < (uint32_t)cf->num_params) {  // use cached num_params
                        int asi = cf->vreg_struct_id[arg_vi];
                        arg_is_struct_ptr_param = (asi >= 0);
                    }
                }

                if (callee_param_is_struct && (arg_is_tagged || arg_is_struct_ptr_param)) {
                    if (arg_is_struct_ptr_param) {
                        // Caller param is already xrs_N*, pass directly (no .ptr needed)
                        xcg_emit_ref(b, func, arg_ref);
                    } else {
                        // Callee expects xrs_N*, caller has XrtValue: cast .ptr
                        xcgen_buf_printf(b, "(%s*)", callee_struct_name);
                        xcg_emit_ref(b, func, arg_ref);
                        xcgen_buf_puts(b, ".ptr");
                    }
                } else if (callee_param_is_struct && !arg_is_tagged) {
                    // Caller has some native type, pass directly
                    xcg_emit_ref(b, func, arg_ref);
                } else {
                    // Normal type coercion: if arg is XrtValue but callee expects native type, unbox
                    XrProto *cp = (XrProto *)callee_proto;
                    uint8_t param_type = 0;
                    if (cp && cp->param_types && i < cp->param_types_count && cp->param_types[i])
                        param_type = xr_type_to_slot_type(cp->param_types[i]);
                    bool param_wants_i64 = (param_type >= 1 && param_type <= 8);
                    bool param_wants_f64 = (param_type == 9 || param_type == 10);
                    if (arg_is_tagged && param_wants_i64) {
                        xcgen_buf_puts(b, "xrt_unbox_int(");
                        xcg_emit_ref(b, func, arg_ref);
                        xcgen_buf_puts(b, ")");
                    } else if (arg_is_tagged && param_wants_f64) {
                        xcgen_buf_puts(b, "xrt_unbox_float(");
                        xcg_emit_ref(b, func, arg_ref);
                        xcgen_buf_puts(b, ")");
                    } else {
                        xcg_emit_ref(b, func, arg_ref);
                    }
                }
            } else {
                xcgen_buf_puts(b, "0");
            }
        }
        xcgen_buf_puts(b, ");\n");
    } else if (callee_proto) {
        // Pure AOT: all functions must be compiled. Emit abort for unresolved protos.
        xcgen_buf_printf(b, "    fprintf(stderr, \"AOT: uncompiled function call\\n\");\n");
        xcgen_buf_puts(b, "    abort();\n");
        cf->needs_runtime = true;
    } else {
        // Unknown proto with no name — emit zero
        xcgen_buf_printf(b, "    v%u = 0;\n", dst_idx);
    }

    // Reset call args buffer
    cf->call_args_count = 0;
    for (int i = 0; i < cf->call_args_cap; i++)
        cf->call_args[i] = XIR_NONE;
}

/* ========== CALL_SELF_DIRECT Translation ========== */

static void emit_call_self_direct(XcgenBuf *b, XirFunc *func, XirIns *ins,
                                   const char *self_name, XcgenFunc *cf) {
    XR_DCHECK(b != NULL, "emit_call_self_direct: NULL buf");
    XR_DCHECK(func != NULL, "emit_call_self_direct: NULL func");
    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);

    xcgen_buf_printf(b, "    v%u = %s(", dst_idx, self_name);
    bool need_comma = true;
    xcgen_buf_puts(b, "xrt_ctx");

    // Two modes: register passing (args in ins->args[]) or memory (via STORE_CORO)
    if (!xir_ref_is_none(ins->args[0])) {
        // Register passing: args directly in ins->args[0..1]
        int nargs = func->num_params;
        for (int a = 0; a < nargs && a < 2; a++) {
            if (need_comma || a > 0) xcgen_buf_puts(b, ", ");
            need_comma = false;
            xcg_emit_ref(b, func, ins->args[a]);
        }
    } else {
        // Memory passing: args were stored via STORE_CORO
        int nargs = func->num_params;
        for (int i = 0; i < nargs; i++) {
            if (need_comma || i > 0) xcgen_buf_puts(b, ", ");
            need_comma = false;
            int slot = 1 + i;  // skip slot 0 (closure)
            if (slot < cf->call_args_cap && !xir_ref_is_none(cf->call_args[slot])) {
                xcg_emit_ref(b, func, cf->call_args[slot]);
            } else {
                xcgen_buf_puts(b, "0");
            }
        }
        // Reset call args buffer
        cf->call_args_count = 0;
        for (int j = 0; j < cf->call_args_cap; j++)
            cf->call_args[j] = XIR_NONE;
    }

    xcgen_buf_puts(b, ");\n");
}

/* ========== CALL_C Translation ========== */

#include "xcgen_bridge.h"

// Auto-box int64_t/double refs to XrtValue for runtime calls
static void emit_ref_as_tagged(XcgenBuf *b, XirFunc *func, XirRef ref) {
    XR_DCHECK(b != NULL, "emit_ref_as_tagged: NULL buf");
    XR_DCHECK(func != NULL, "emit_ref_as_tagged: NULL func");
    uint8_t t = xcg_ref_type(func, ref);
    if (t == XR_REP_I64) {
        xcgen_buf_puts(b, "xrt_box_int(");
        xcg_emit_ref(b, func, ref);
        xcgen_buf_puts(b, ")");
    } else if (t == XR_REP_F64) {
        xcgen_buf_puts(b, "xrt_box_float(");
        xcg_emit_ref(b, func, ref);
        xcgen_buf_puts(b, ")");
    } else {
        xcg_emit_ref(b, func, ref);
    }
}

static void emit_call_c(XcgenBuf *b, XirFunc *func, XirIns *ins,
                        XcgenFunc *cf, XcgenModule *mod) {
    XR_DCHECK(b != NULL, "emit_call_c: NULL buf");
    XR_DCHECK(func != NULL, "emit_call_c: NULL func");
    XR_DCHECK(ins != NULL, "emit_call_c: NULL ins");
    const char *tagged_type = "XrValue";
    // Check if this CALL_C targets a known JIT helper that maps to AOT runtime
    if (xir_ref_is_const(ins->args[0])) {
        uint32_t ci = XIR_REF_INDEX(ins->args[0]);
        if (ci < func->nconst && func->consts[ci].rep == XR_REP_PTR) {
            void *fn_ptr = func->consts[ci].val.ptr;

            // Json struct promotion: CALL_C(xr_json_new_with_shape, shape_ptr)
            if (fn_ptr == (void *)xr_json_new_with_shape && mod->struct_reg) {
                // Extract shape pointer from args[1] (const ptr)
                void *shape_ptr = NULL;
                if (xir_ref_is_const(ins->args[1])) {
                    uint32_t si = XIR_REF_INDEX(ins->args[1]);
                    if (si < func->nconst)
                        shape_ptr = func->consts[si].val.ptr;
                }
                int sidx = shape_ptr ? xcgen_find_struct(mod->struct_reg, shape_ptr) : -1;
                if (sidx >= 0) {
                    XcgenStruct *st = &mod->struct_reg->structs[sidx];
                    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
                    // ARC-allocate the struct (initial rc=1), wrap as tagged ptr
                    xcgen_buf_printf(b, "    v%u = (%s){.ptr = xrt_arc_alloc(sizeof(%s)), .tag = XRT_TAG_PTR};\n",
                                     dst_idx, tagged_type, st->c_name);
                    // If struct has PTR fields, set type tag + HAS_DEINIT for recursive release
                    if (!st->all_native) {
                        xcgen_buf_printf(b,
                            "    { XrtArcHdr *_h = XRT_ARC_HDR(v%u.ptr);"
                            " _h->type = %d; _h->flags |= XRT_ARC_HAS_DEINIT; }\n",
                            dst_idx, sidx);
                    }
                    // Track this vreg as a promoted struct
                    if (cf->vreg_struct_id && dst_idx < func->nvreg)
                        cf->vreg_struct_id[dst_idx] = (int16_t)sidx;
                    cf->needs_gc = true;
                    cf->call_args_count = 0;
                    return;
                }
                // Not promotable: fall through to default suppression
                cf->call_args_count = 0;
                return;
            }

            // GETPROP: CALL_C_LEAF(xr_jit_getprop, symbol_id)
            // obj in call_args[0], symbol_id in args[1] (vreg from CONST_I64)
            if (fn_ptr == (void *)xr_jit_getprop && mod->struct_reg) {
                uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
                // Extract symbol_id from args[1] — may be const ref or vreg
                // Trace through MOV chains (from GVN/CSE) to find CONST_I64
                int64_t symbol_id = -1;
                XirRef trace_ref = ins->args[1];
                for (int trace_depth = 0; trace_depth < 8; trace_depth++) {
                    if (xir_ref_is_const(trace_ref)) {
                        uint32_t ci2 = XIR_REF_INDEX(trace_ref);
                        if (ci2 < func->nconst)
                            symbol_id = func->consts[ci2].val.i64;
                        break;
                    }
                    if (!xir_ref_is_vreg(trace_ref)) break;
                    uint32_t vi = XIR_REF_INDEX(trace_ref);
                    bool found = false;
                    for (uint32_t bi2 = 0; bi2 < func->nblk && !found; bi2++) {
                        XirBlock *blk2 = func->blocks[bi2];
                        for (uint32_t ii = 0; ii < blk2->nins && !found; ii++) {
                            XirIns *def = &blk2->ins[ii];
                            if (!xir_ref_is_vreg(def->dst) ||
                                XIR_REF_INDEX(def->dst) != vi) continue;
                            if (def->op == XIR_CONST_I64) {
                                trace_ref = def->args[0];  // follow to const
                                found = true;
                            } else if (def->op == XIR_MOV) {
                                trace_ref = def->args[0];  // follow MOV chain
                                found = true;
                            } else {
                                break;  // unknown def, give up
                            }
                        }
                    }
                    if (!found) break;
                }
                // O(log n) lookup: binary search on sorted field_entries
                const XcgenFieldEntry *fe = xcgen_find_field_by_symbol(
                        mod->struct_reg, (uint32_t)symbol_id);
                if (fe) {
                    XcgenStruct *st = &mod->struct_reg->structs[fe->struct_idx];
                    xcgen_buf_printf(b, "    v%u = ((%s*)v%u.ptr)->%s;\n",
                                     dst_idx, st->c_name,
                                     XIR_REF_INDEX(cf->call_args[0]),
                                     st->fields[fe->field_idx].name);
                    cf->call_args_count = 0;
                    return;
                }
                // Inline property dispatch for known symbols (no VM dependency)
                uint8_t dst_type = xcg_ref_type(func, ins->dst);
                // SYMBOL_LENGTH(2), SYMBOL_SIZE(1), SYMBOL_IS_EMPTY(3)
                if ((symbol_id == 1 || symbol_id == 2 || symbol_id == 3) &&
                    cf->call_args_count >= 1) {
                    if (dst_type == XR_REP_I64) {
                        xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                        emit_ref_as_tagged(b, func, cf->call_args[0]);
                        xcgen_buf_printf(b, ", %lldLL).i;\n", (long long)symbol_id);
                    } else {
                        xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                        emit_ref_as_tagged(b, func, cf->call_args[0]);
                        xcgen_buf_printf(b, ", %lldLL);\n", (long long)symbol_id);
                    }
                    cf->needs_runtime = true;
                    cf->call_args_count = 0;
                    return;
                }
                // Fallback for unknown symbols: use inline xrt_getprop
                if (cf->call_args_count >= 1) {
                    if (dst_type == XR_REP_I64) {
                        xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                        emit_ref_as_tagged(b, func, cf->call_args[0]);
                        xcgen_buf_printf(b, ", %lldLL).i;\n", (long long)symbol_id);
                    } else if (dst_type == XR_REP_F64) {
                        xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                        emit_ref_as_tagged(b, func, cf->call_args[0]);
                        xcgen_buf_printf(b, ", %lldLL).f;\n", (long long)symbol_id);
                    } else {
                        xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                        emit_ref_as_tagged(b, func, cf->call_args[0]);
                        xcgen_buf_printf(b, ", %lldLL);\n", (long long)symbol_id);
                    }
                    cf->needs_runtime = true;
                } else {
                    // No call args — emit zero
                    if (dst_type == XR_REP_I64) {
                        xcgen_buf_printf(b, "    v%u = 0;\n", dst_idx);
                    } else if (dst_type == XR_REP_F64) {
                        xcgen_buf_printf(b, "    v%u = 0.0;\n", dst_idx);
                    } else {
                        xcgen_buf_printf(b, "    v%u = (%s){.i = 0, .tag = XRT_TAG_NULL};\n", dst_idx, tagged_type);
                        cf->needs_runtime = true;
                    }
                }
                cf->call_args_count = 0;
                return;
            }

            if (fn_ptr == (void *)xr_jit_index_get && cf->call_args_count >= 2) {
                // INDEX_GET: v{dst} = xrt_index_get(obj, key)
                // For known I64/F64 result, emit direct payload access to avoid unbox overhead:
                //   int64_t v = xrt_array_get_i64(arr, idx)  →  a->data[idx].i
                uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
                uint8_t dst_type = xcg_ref_type(func, ins->dst);
                uint8_t key_type = xcg_ref_type(func, cf->call_args[1]);
                bool key_is_i64 = (key_type == XR_REP_I64);
                if (dst_type == XR_REP_I64 && key_is_i64) {
                    // Direct payload access: ((xrt_array_t*)arr.ptr)->data[idx].i
                    xcgen_buf_printf(b, "    v%u = xrt_array_get_i(", dst_idx);
                    emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, ");\n");
                } else if (dst_type == XR_REP_F64 && key_is_i64) {
                    xcgen_buf_printf(b, "    v%u = xrt_array_get_f(", dst_idx);
                    emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, ");\n");
                } else if (dst_type == XR_REP_I64) {
                    xcgen_buf_printf(b, "    v%u = xrt_unbox_int(xrt_index_get(", dst_idx);
                    emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    emit_ref_as_tagged(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, "));\n");
                } else if (dst_type == XR_REP_F64) {
                    xcgen_buf_printf(b, "    v%u = xrt_unbox_float(xrt_index_get(", dst_idx);
                    emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    emit_ref_as_tagged(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, "));\n");
                } else {
                    xcgen_buf_printf(b, "    v%u = xrt_index_get(", dst_idx);
                    emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    emit_ref_as_tagged(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, ");\n");
                }
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }

            if (fn_ptr == (void *)xr_jit_index_set && cf->call_args_count >= 3) {
                // INDEX_SET: xrt_index_set(obj, key, val)
                // For known I64/F64 val with I64 key, emit xrt_array_set_i/f for direct store
                uint8_t key_type = xcg_ref_type(func, cf->call_args[1]);
                uint8_t val_type = xcg_ref_type(func, cf->call_args[2]);
                bool key_is_i64 = (key_type == XR_REP_I64);
                if (key_is_i64 && val_type == XR_REP_I64) {
                    xcgen_buf_puts(b, "    xrt_array_set_i(");
                    emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref(b, func, cf->call_args[2]);
                    xcgen_buf_puts(b, ");\n");
                } else if (key_is_i64 && val_type == XR_REP_F64) {
                    xcgen_buf_puts(b, "    xrt_array_set_f(");
                    emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref(b, func, cf->call_args[2]);
                    xcgen_buf_puts(b, ");\n");
                } else {
                    xcgen_buf_puts(b, "    xrt_index_set(");
                    emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    emit_ref_as_tagged(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, ", ");
                    emit_ref_as_tagged(b, func, cf->call_args[2]);
                    xcgen_buf_puts(b, ");\n");
                }
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }

            // Throw: CALL_C(xr_jit_throw, exception_value)
            // In AOT, store exception value to xrt_exception local
            // Control flow (goto catch) is already handled by builder
            if (fn_ptr == (void *)xr_jit_throw) {
                xcgen_buf_puts(b, "    xrt_exception = ");
                emit_ref_as_tagged(b, func, ins->args[1]);
                xcgen_buf_puts(b, ";\n");
                cf->needs_exception = true;
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }

            // Typed array get: CALL_C(xr_jit_tarray_get, unused)
            // call_args[0] = array, call_args[1] = index
            if (fn_ptr == (void *)xr_jit_tarray_get) {
                uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
                xcgen_buf_printf(b, "    v%u = xrt_tarray_get(", dst_idx);
                if (cf->call_args_count > 0)
                    emit_ref_as_tagged(b, func, cf->call_args[0]);
                else
                    xcgen_buf_printf(b, "(%s){0}", tagged_type);
                xcgen_buf_puts(b, ", ");
                if (cf->call_args_count > 1)
                    xcg_emit_ref(b, func, cf->call_args[1]);
                else
                    xcgen_buf_puts(b, "0");
                xcgen_buf_puts(b, ");\n");
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }

            // Typed array set: CALL_C(xr_jit_tarray_set, unused)
            // call_args[0] = array, call_args[1] = index, call_args[2] = value
            if (fn_ptr == (void *)xr_jit_tarray_set) {
                xcgen_buf_puts(b, "    xrt_tarray_set(");
                if (cf->call_args_count > 0)
                    emit_ref_as_tagged(b, func, cf->call_args[0]);
                else
                    xcgen_buf_printf(b, "(%s){0}", tagged_type);
                xcgen_buf_puts(b, ", ");
                if (cf->call_args_count > 1)
                    xcg_emit_ref(b, func, cf->call_args[1]);
                else
                    xcgen_buf_puts(b, "0");
                xcgen_buf_puts(b, ", ");
                if (cf->call_args_count > 2)
                    xcg_emit_ref(b, func, cf->call_args[2]);
                else
                    xcgen_buf_puts(b, "0");
                xcgen_buf_puts(b, ");\n");
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }

            // StringBuilder new: CALL_C(xrt_strbuf_new_sentinel, 0)
            if (fn_ptr == (void *)xrt_strbuf_new_sentinel) {
                uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
                xcgen_buf_printf(b, "    v%u = xrt_strbuf_new();\n", dst_idx);
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }

            // StringBuilder append: CALL_C(xrt_strbuf_append_sentinel, 0)
            // call_args[0] = sb, call_args[1] = value
            if (fn_ptr == (void *)xrt_strbuf_append_sentinel) {
                xcgen_buf_puts(b, "    xrt_strbuf_append(");
                if (cf->call_args_count > 0)
                    emit_ref_as_tagged(b, func, cf->call_args[0]);
                else
                    xcgen_buf_printf(b, "(%s){0}", tagged_type);
                xcgen_buf_puts(b, ", ");
                if (cf->call_args_count > 1)
                    emit_ref_as_tagged(b, func, cf->call_args[1]);
                else
                    xcgen_buf_printf(b, "(%s){0}", tagged_type);
                xcgen_buf_puts(b, ");\n");
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }

            // StringBuilder finish: CALL_C(xrt_strbuf_finish_sentinel, 0)
            // call_args[0] = sb
            if (fn_ptr == (void *)xrt_strbuf_finish_sentinel) {
                uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
                xcgen_buf_printf(b, "    v%u = xrt_strbuf_finish(", dst_idx);
                if (cf->call_args_count > 0)
                    emit_ref_as_tagged(b, func, cf->call_args[0]);
                else
                    xcgen_buf_printf(b, "(%s){0}", tagged_type);
                xcgen_buf_puts(b, ");\n");
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }

            // Builtin method invocation: CALL_C(xrt_invoke_method_sentinel, encoded)
            // encoded = (method_symbol << 32) | nargs
            if (fn_ptr == (void *)xrt_invoke_method_sentinel) {
                // Decode method_symbol and nargs from args[1] (const i64).
                // Follow MOV chains: CSE may replace CONST_I64 with MOV of earlier vreg.
                int64_t encoded = 0;
                if (xir_ref_is_vreg(ins->args[1])) {
                    uint32_t vi = XIR_REF_INDEX(ins->args[1]);
                    // Chase up to 8 MOV hops to find the CONST_I64 definition
                    for (int hop = 0; hop < 8 && vi < func->nvreg; hop++) {
                        bool found = false;
                        for (uint32_t bi2 = 0; bi2 < func->nblk && !found; bi2++) {
                            XirBlock *blk2 = func->blocks[bi2];
                            for (uint32_t ii2 = 0; ii2 < blk2->nins; ii2++) {
                                XirIns *def = &blk2->ins[ii2];
                                if (!xir_ref_is_vreg(def->dst)) continue;
                                if (XIR_REF_INDEX(def->dst) != vi) continue;
                                if (def->op == XIR_CONST_I64 &&
                                    xir_ref_is_const(def->args[0])) {
                                    uint32_t ci2 = XIR_REF_INDEX(def->args[0]);
                                    if (ci2 < func->nconst)
                                        encoded = func->consts[ci2].val.i64;
                                    found = true;
                                    hop = 8; // done
                                    break;
                                }
                                if (def->op == XIR_MOV &&
                                    xir_ref_is_vreg(def->args[0])) {
                                    vi = XIR_REF_INDEX(def->args[0]);
                                    found = true; // follow the chain
                                    break;
                                }
                                // Any other def: give up
                                found = true; hop = 8;
                                break;
                            }
                        }
                        if (!found) break;
                    }
                }
                uint32_t dst_idx = XIR_REF_INDEX(ins->dst);

                if (encoded < 0) {
                    // TOSTRING: encoded = -(slot_hint + 1)
                    // call_args[0] = value to convert
                    int slot_hint = (int)(-(encoded + 1));
                    xcgen_buf_printf(b, "    v%u = xrt_tostring(", dst_idx);
                    if (cf->call_args_count > 0)
                        emit_ref_as_tagged(b, func, cf->call_args[0]);
                    else
                        xcgen_buf_printf(b, "(%s){0}", tagged_type);
                    xcgen_buf_printf(b, ", %d);\n", slot_hint);
                } else {
                    // Method invocation: encoded = (method_symbol << 32) | nargs
                    int method_symbol = (int)(encoded >> 32);
                    int nargs = (int)(encoded & 0xFF);

                    // Inline float math methods when receiver is F64 and dst is F64.
                    // This eliminates xrt_box_float + xrt_method_0 + xrt_unbox_float.
                    XirRef recv_ref = (cf->call_args_count > 0) ? cf->call_args[0] : XIR_NONE;
                    uint8_t recv_type = xir_ref_is_none(recv_ref)
                                        ? XR_REP_TAGGED : xcg_ref_type(func, recv_ref);
                    uint8_t dst_vtype = (dst_idx < func->nvreg)
                                        ? func->vregs[dst_idx].rep : XR_REP_TAGGED;
                    bool recv_is_float = xcg_is_float_type(recv_type);
                    bool dst_is_float  = xcg_is_float_type(dst_vtype);

                    // XRT_SYM_* values (from xrt.h)
                    const char *math_fn = NULL;
                    if (recv_is_float && dst_is_float && nargs == 0) {
                        switch (method_symbol) {
                            case 59: math_fn = "floor"; break; // XRT_SYM_FLOOR
                            case 60: math_fn = "ceil";  break; // XRT_SYM_CEIL
                            case 61: math_fn = "round"; break; // XRT_SYM_ROUND
                            case 62: math_fn = "fabs";  break; // XRT_SYM_ABS
                            case 63: math_fn = "sqrt";  break; // XRT_SYM_SQRT
                            default: break;
                        }
                    }
                    if (recv_is_float && dst_is_float && nargs == 1 && method_symbol == 64) {
                        // XRT_SYM_POW: pow(receiver, arg)
                        XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
                        uint8_t arg1_type = xir_ref_is_none(arg1)
                                            ? XR_REP_TAGGED : xcg_ref_type(func, arg1);
                        if (xcg_is_float_type(arg1_type) || arg1_type == XR_REP_I64) {
                            xcgen_buf_printf(b, "    v%u = pow(", dst_idx);
                            xcg_emit_ref(b, func, recv_ref);
                            xcgen_buf_puts(b, ", ");
                            xcg_emit_ref(b, func, arg1);
                            xcgen_buf_puts(b, ");\n");
                            cf->needs_runtime = true;
                            cf->call_args_count = 0;
                            return;
                        }
                    }
                    if (math_fn) {
                        xcgen_buf_printf(b, "    v%u = %s(", dst_idx, math_fn);
                        xcg_emit_ref(b, func, recv_ref);
                        xcgen_buf_puts(b, ");\n");
                        cf->needs_runtime = true;
                        cf->call_args_count = 0;
                        return;
                    }

                    // Inline I64 methods when receiver is I64
                    bool recv_is_int = (recv_type == XR_REP_I64);
                    bool dst_is_int  = (dst_vtype == XR_REP_I64);
                    if (recv_is_int && dst_is_int && nargs == 0 && method_symbol == 62) {
                        // XRT_SYM_ABS: abs(v) = v < 0 ? -v : v
                        xcgen_buf_printf(b, "    v%u = (", dst_idx);
                        xcg_emit_ref(b, func, recv_ref);
                        xcgen_buf_puts(b, " < 0) ? -(");
                        xcg_emit_ref(b, func, recv_ref);
                        xcgen_buf_puts(b, ") : ");
                        xcg_emit_ref(b, func, recv_ref);
                        xcgen_buf_puts(b, ";\n");
                        cf->call_args_count = 0;
                        return;
                    }

                    // Inline string.length when receiver is STR and dst is I64
                    bool recv_is_str = (recv_type == XR_REP_STR);
                    if (recv_is_str && dst_is_int && nargs == 0 &&
                        (method_symbol == 2 || method_symbol == 1)) {
                        // XRT_SYM_LENGTH(2) or XRT_SYM_SIZE(1)
                        xcgen_buf_printf(b, "    v%u = (int64_t)strlen((const char *)", dst_idx);
                        xcg_emit_ref(b, func, recv_ref);
                        xcgen_buf_puts(b, ".ptr);\n");
                        cf->needs_runtime = true;
                        cf->call_args_count = 0;
                        return;
                    }

                    // Fixed-arity fallback: xrt_method_N (no varargs, inlinable)
                    if (nargs == 0) {
                        if (dst_is_float)
                            xcgen_buf_printf(b, "    v%u = xrt_unbox_float(xrt_method_0(",
                                             dst_idx);
                        else if (dst_is_int)
                            xcgen_buf_printf(b, "    v%u = xrt_unbox_int(xrt_method_0(",
                                             dst_idx);
                        else
                            xcgen_buf_printf(b, "    v%u = xrt_method_0(", dst_idx);
                        if (cf->call_args_count > 0)
                            emit_ref_as_tagged(b, func, cf->call_args[0]);
                        else
                            xcgen_buf_printf(b, "(%s){0}", tagged_type);
                        xcgen_buf_printf(b, ", %d", method_symbol);
                    } else if (nargs == 1) {
                        if (dst_is_float)
                            xcgen_buf_printf(b, "    v%u = xrt_unbox_float(xrt_method_1(",
                                             dst_idx);
                        else if (dst_is_int)
                            xcgen_buf_printf(b, "    v%u = xrt_unbox_int(xrt_method_1(",
                                             dst_idx);
                        else
                            xcgen_buf_printf(b, "    v%u = xrt_method_1(", dst_idx);
                        if (cf->call_args_count > 0)
                            emit_ref_as_tagged(b, func, cf->call_args[0]);
                        else
                            xcgen_buf_printf(b, "(%s){0}", tagged_type);
                        xcgen_buf_printf(b, ", %d", method_symbol);
                        if (cf->call_args_count > 1) {
                            xcgen_buf_puts(b, ", ");
                            emit_ref_as_tagged(b, func, cf->call_args[1]);
                        } else {
                            xcgen_buf_printf(b, ", (%s){0}", tagged_type);
                        }
                    } else {
                        // 2+ args: use xrt_method_2
                        if (dst_is_float)
                            xcgen_buf_printf(b, "    v%u = xrt_unbox_float(xrt_method_2(",
                                             dst_idx);
                        else if (dst_is_int)
                            xcgen_buf_printf(b, "    v%u = xrt_unbox_int(xrt_method_2(",
                                             dst_idx);
                        else
                            xcgen_buf_printf(b, "    v%u = xrt_method_2(", dst_idx);
                        if (cf->call_args_count > 0)
                            emit_ref_as_tagged(b, func, cf->call_args[0]);
                        else
                            xcgen_buf_printf(b, "(%s){0}", tagged_type);
                        xcgen_buf_printf(b, ", %d", method_symbol);
                        for (int ai = 0; ai < 2; ai++) {
                            xcgen_buf_puts(b, ", ");
                            if (ai + 1 < cf->call_args_count)
                                emit_ref_as_tagged(b, func, cf->call_args[ai + 1]);
                            else
                                xcgen_buf_printf(b, "(%s){0}", tagged_type);
                        }
                    }
                    if (dst_is_float || dst_is_int)
                        xcgen_buf_puts(b, "));\n");
                    else
                        xcgen_buf_puts(b, ");\n");
                }
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }

            // Closure creation: CALL_C(child_proto_ptr, nupvals)
            // Recognize by checking if fn_ptr is a registered proto
            const char *closure_fn = xcg_lookup_proto_name(mod, fn_ptr);
            if (closure_fn) {
                uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
                // Extract nupvals from args[1]
                int64_t nupvals = 0;
                if (xir_ref_is_vreg(ins->args[1])) {
                    // Trace through CONST_I64 def
                    uint32_t vi = XIR_REF_INDEX(ins->args[1]);
                    for (uint32_t bi2 = 0; bi2 < func->nblk; bi2++) {
                        XirBlock *blk2 = func->blocks[bi2];
                        for (uint32_t ii = 0; ii < blk2->nins; ii++) {
                            XirIns *def = &blk2->ins[ii];
                            if (xir_ref_is_vreg(def->dst) &&
                                XIR_REF_INDEX(def->dst) == vi &&
                                def->op == XIR_CONST_I64 &&
                                xir_ref_is_const(def->args[0])) {
                                uint32_t ci2 = XIR_REF_INDEX(def->args[0]);
                                if (ci2 < func->nconst)
                                    nupvals = func->consts[ci2].val.i64;
                            }
                        }
                    }
                }
                xcgen_buf_printf(b, "    v%u = xrt_closure_new((void*)%s, %d);\n",
                                 dst_idx, closure_fn, (int)nupvals);
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }
        }
    }
    // Default: suppress (dead code in AOT for GETSHARED etc.)
    cf->call_args_count = 0;
}

/* ========== Public Entry Point ========== */

bool xcg_emit_call_instruction(XcgenBuf *b, XirFunc *func, XirIns *ins,
                                const char *self_name, XcgenModule *mod,
                                XcgenFunc *cf) {
    XR_DCHECK(b != NULL, "xcg_emit_call_instruction: b is NULL");
    XR_DCHECK(func != NULL, "xcg_emit_call_instruction: func is NULL");
    XR_DCHECK(ins != NULL, "xcg_emit_call_instruction: ins is NULL");
    switch (ins->op) {
        case XIR_STORE_CORO: {
            // STORE_CORO is no longer used for call args (migrated to call_arg_pool).
            // Remaining uses: multi-return values, deopt blocks — suppress in AOT.
            return true;
        }

        case XIR_CALL_SELF_DIRECT:
            load_call_args_from_pool(func, ins, cf);
            emit_call_self_direct(b, func, ins, self_name, cf);
            return true;

        case XIR_CALL_KNOWN:
        case XIR_CALL_KNOWN_REG:
            load_call_args_from_pool(func, ins, cf);
            emit_call_known(b, func, ins, mod, cf);
            return true;

        case XIR_CALL_C:
            load_call_args_from_pool(func, ins, cf);
            emit_call_c(b, func, ins, cf, mod);
            return true;

        case XIR_CALL_C_LEAF:
            load_call_args_from_pool(func, ins, cf);
            emit_call_c(b, func, ins, cf, mod);
            return true;

        case XIR_CALL_DIRECT: {
            load_call_args_from_pool(func, ins, cf);
            // Indirect call via closure: call_args[0]=closure, call_args[1..n]=args
            // Extract nargs from ins->args[0] (vreg from CONST_I64)
            int64_t nargs = 0;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                for (uint32_t bi2 = 0; bi2 < func->nblk; bi2++) {
                    XirBlock *blk2 = func->blocks[bi2];
                    for (uint32_t ii = 0; ii < blk2->nins; ii++) {
                        XirIns *def = &blk2->ins[ii];
                        if (xir_ref_is_vreg(def->dst) &&
                            XIR_REF_INDEX(def->dst) == vi &&
                            def->op == XIR_CONST_I64 &&
                            xir_ref_is_const(def->args[0])) {
                            uint32_t ci2 = XIR_REF_INDEX(def->args[0]);
                            if (ci2 < func->nconst)
                                nargs = func->consts[ci2].val.i64;
                        }
                    }
                }
            }
            // Fallback: derive nargs from collected call_args
            // call_args[0]=closure, call_args[1..n]=actual args
            if (nargs == 0 && cf->call_args_count > 1)
                nargs = cf->call_args_count - 1;

            uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
            uint8_t ret_type = xcg_ref_type(func, ins->dst);
            const char *tagged_type = "XrValue";
            const char *ret_c = xcg_c_type(ret_type);

            // Build function pointer type cast: ret_type (*)(XrtContext, Tagged, arg_types...)
            // Closure is call_args[0], actual args are call_args[1..nargs]
            XirRef closure_ref = cf->call_args[0];

            xcgen_buf_printf(b, "    v%u = ((%s (*)(XrtContext, %s", dst_idx, ret_c, tagged_type);
            for (int i = 0; i < (int)nargs; i++) {
                XirRef arg_ref = cf->call_args[1 + i];
                uint8_t arg_type = xcg_ref_type(func, arg_ref);
                xcgen_buf_printf(b, ", %s", xcg_c_type(arg_type));
            }
            xcgen_buf_puts(b, "))((xrt_closure_t*)");
            emit_ref_as_tagged(b, func, closure_ref);
            xcgen_buf_puts(b, ".ptr)->fn)(");
            xcgen_buf_puts(b, "xrt_ctx, ");
            emit_ref_as_tagged(b, func, closure_ref);
            for (int i = 0; i < (int)nargs; i++) {
                xcgen_buf_puts(b, ", ");
                xcg_emit_ref(b, func, cf->call_args[1 + i]);
            }
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;

            // Reset call args
            cf->call_args_count = 0;
            for (int i = 0; i < cf->call_args_cap; i++)
                cf->call_args[i] = XIR_NONE;
            return true;
        }

        default:
            return false;  // not a call instruction
    }
}
