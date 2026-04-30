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
 *     → closure creation or unknown target fallback
 *   CALL_INTRINSIC(id)
 *     → delegated to xcg_emit_call_intrinsic() in xcgen_intrinsic.c
 *   CALL_DIRECT(closure)
 *     → indirect call via closure function pointer
 *
 * RELATED MODULES:
 *   - xcgen_intrinsic.c: intrinsic lowering (CALL_INTRINSIC dispatch)
 *   - xcgen_expr.c: delegates call ops here
 *   - xir_builder.c: generates the STORE_CORO + CALL_KNOWN sequence
 */

#include "xcgen.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xstring.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "xrt_method_symbols.h"  // XRT_SYM_* constants only (avoids xrt_arc.h)

/*
 * JIT_CALL_ARGS_OFFSET = 688 (from xir_offsets.h)
 * jit_call_args[0] at offset 688 = closure (skip in AOT)
 * jit_call_args[1] at offset 696 = arg0
 * jit_call_args[2] at offset 704 = arg1
 * ...
 * jit_call_args[n] at offset 688 + n*8
 */
#define AOT_CALL_ARGS_BASE 688
#define AOT_CALL_ARGS_SLOT_SIZE 8

// Resolve a constant int64 value from an XIR ref, tracing through MOV chains.
// Returns true if resolved, writing to *out_val.
bool xcg_resolve_const_i64(XirFunc *func, XirRef ref, int64_t *out_val) {
    XR_DCHECK(func != NULL && out_val != NULL, "xcg_resolve_const_i64: NULL arg");
    if (xir_ref_is_const(ref)) {
        uint32_t ci = XIR_REF_INDEX(ref);
        if (ci < func->nconst) {
            *out_val = func->consts[ci].val.i64;
            return true;
        }
        return false;
    }
    if (!xir_ref_is_vreg(ref))
        return false;
    uint32_t vi = XIR_REF_INDEX(ref);
    for (int hop = 0; hop < 8 && vi < func->nvreg; hop++) {
        bool found = false;
        for (uint32_t bi = 0; bi < func->nblk && !found; bi++) {
            XirBlock *blk = func->blocks[bi];
            for (uint32_t ii = 0; ii < blk->nins; ii++) {
                XirIns *def = &blk->ins[ii];
                if (!xir_ref_is_vreg(def->dst) || XIR_REF_INDEX(def->dst) != vi)
                    continue;
                if (def->op == XIR_CONST_I64 && xir_ref_is_const(def->args[0])) {
                    uint32_t ci = XIR_REF_INDEX(def->args[0]);
                    if (ci < func->nconst) {
                        *out_val = func->consts[ci].val.i64;
                        return true;
                    }
                    return false;
                }
                if (def->op == XIR_MOV && xir_ref_is_vreg(def->args[0])) {
                    vi = XIR_REF_INDEX(def->args[0]);
                    found = true;
                    break;
                }
                return false;  // unknown def
            }
        }
        if (!found)
            break;
    }
    return false;
}

// Find the defining instruction for a vreg, tracing through MOV chains (max 8 hops).
// Returns the non-MOV defining XirIns*, or NULL if not found.
XirIns *xcg_find_def(XirFunc *func, XirRef ref) {
    XR_DCHECK(func != NULL, "xcg_find_def: func is NULL");
    if (!xir_ref_is_vreg(ref))
        return NULL;
    uint32_t vi = XIR_REF_INDEX(ref);
    for (int hop = 0; hop < 8 && vi < func->nvreg; hop++) {
        // Fast path: use SSA def pointer if available
        if (func->vregs[vi].def) {
            XirIns *def = func->vregs[vi].def;
            if (def->op == XIR_MOV && xir_ref_is_vreg(def->args[0])) {
                vi = XIR_REF_INDEX(def->args[0]);
                continue;
            }
            return def;
        }
        // Slow path: scan all blocks
        bool found = false;
        for (uint32_t bi = 0; bi < func->nblk && !found; bi++) {
            XirBlock *blk = func->blocks[bi];
            for (uint32_t ii = 0; ii < blk->nins; ii++) {
                XirIns *def = &blk->ins[ii];
                if (!xir_ref_is_vreg(def->dst) || XIR_REF_INDEX(def->dst) != vi)
                    continue;
                if (def->op == XIR_MOV && xir_ref_is_vreg(def->args[0])) {
                    vi = XIR_REF_INDEX(def->args[0]);
                    found = true;
                    break;
                }
                return def;
            }
        }
        if (!found)
            break;
    }
    return NULL;
}

// Maximum upvalues for non-escaping closure inlining (stack array limit).
#define XCGEN_MAX_NONESC_UPVALS 16

// Collect upvalue value refs from STORE_UPVAL instructions targeting cl_vreg.
// Fills out_refs[0..nupvals-1] with the XirRef of each upvalue value.
// Returns the number of upvals successfully collected.
static int xcg_collect_upval_refs(XirFunc *func, uint32_t cl_vreg, int nupvals, XirRef *out_refs) {
    XR_DCHECK(func != NULL, "xcg_collect_upval_refs: NULL func");
    XR_DCHECK(out_refs != NULL, "xcg_collect_upval_refs: NULL out_refs");
    for (int i = 0; i < nupvals; i++)
        out_refs[i] = XIR_NONE;
    int found = 0;
    for (uint32_t bi = 0; bi < func->nblk && found < nupvals; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t ii = 0; ii < blk->nins && found < nupvals; ii++) {
            XirIns *ins = &blk->ins[ii];
            if (ins->op != XIR_STORE_UPVAL)
                continue;
            // New convention: args[0]=closure(vreg), dst=idx(const)
            if (!xir_ref_is_vreg(ins->args[0]) || XIR_REF_INDEX(ins->args[0]) != cl_vreg)
                continue;
            // Extract upval index from dst (const ref)
            int64_t uv_idx = 0;
            if (xir_ref_is_const(ins->dst)) {
                uint32_t ci = XIR_REF_INDEX(ins->dst);
                if (ci < func->nconst)
                    uv_idx = func->consts[ci].val.i64;
            }
            if (uv_idx >= 0 && uv_idx < nupvals) {
                out_refs[uv_idx] = ins->args[1];
                found++;
            }
        }
    }
    return found;
}

/* ========== Call Arg Pool → cf->call_args[] ========== */

// Populate cf->call_args[] from the call_arg_pool for the given CALL ins.
static void load_call_args_from_pool(XirFunc *func, XirIns *ins, XcgenFunc *cf) {
    // Reset call args
    for (int i = 0; i < cf->call_args_cap; i++)
        cf->call_args[i] = XIR_NONE;
    cf->call_args_count = 0;
    if (!xir_ref_is_vreg(ins->dst))
        return;
    uint32_t vi = XIR_REF_INDEX(ins->dst);
    if (vi >= func->nvreg)
        return;
    XirVReg *vreg = &func->vregs[vi];
    if (vreg->call_nargs == 0)
        return;
    // Ensure capacity (aborts on OOM — consistent with the xr_malloc /
    // xr_free pair that owns cf->call_args elsewhere in xcgen).
    int needed = (int) vreg->call_nargs;
    if (needed > cf->call_args_cap) {
        int new_cap = cf->call_args_cap;
        while (new_cap < needed)
            new_cap *= 2;
        XR_REALLOC_OR_ABORT(cf->call_args, new_cap * sizeof(XirRef), "xcgen cf->call_args");
        for (int k = cf->call_args_cap; k < new_cap; k++)
            cf->call_args[k] = XIR_NONE;
        cf->call_args_cap = new_cap;
    }
    XirRef *pool = func->call_arg_pool;
    uint32_t start = vreg->call_arg_start;
    for (uint16_t i = 0; i < vreg->call_nargs; i++)
        cf->call_args[i] = pool[start + i];
    cf->call_args_count = (int) vreg->call_nargs;
}

/* ========== CALL_KNOWN Translation ========== */

static void emit_call_known(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenModule *mod,
                            XcgenFunc *cf) {
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
            callee_proto = (void *) (uintptr_t) func->consts[ci].val.raw;
    }
    if (xir_ref_is_const(ins->args[1])) {
        uint32_t ci = XIR_REF_INDEX(ins->args[1]);
        if (ci < func->nconst)
            nargs = (int) func->consts[ci].val.i64;
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
                    nargs = (int) func->consts[ci2].val.i64;
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
    const char *callee_name = callee_proto ? xcg_lookup_proto_name(mod, callee_proto) : NULL;
    int callee_func_idx = callee_proto ? xcg_lookup_proto_func_idx(mod, callee_proto) : -1;

    // Detect class constructor call: GETSHARED loaded a class, CALL uses nargs < numparams.
    // In this case, the caller passes N args but the constructor expects N+1 (this, args...).
    // We must allocate the instance and prepend it as the first argument.
    XrProto *cp = (XrProto *) callee_proto;
    bool is_ctor_call = false;
    int ctor_nfields = 0;
    if (cp && cp->name && callee_name && strcmp(XR_STRING_CHARS(cp->name), "constructor") == 0 &&
        nargs < cp->numparams) {
        is_ctor_call = true;
        // Compute instance size from max TFIELD_SET field index in constructor bytecode
        int max_field = -1;
        uint32_t cc = PROTO_CODE_COUNT(cp);
        for (uint32_t ci2 = 0; ci2 < cc; ci2++) {
            XrInstruction cins = PROTO_CODE(cp, ci2);
            if (GET_OPCODE(cins) == OP_TFIELD_SET) {
                int fi = GETARG_B(cins);
                if (fi > max_field)
                    max_field = fi;
            }
        }
        ctor_nfields = (max_field >= 0) ? max_field + 1 : 2;
    }

    if (is_ctor_call && callee_name) {
        // Class constructor call: allocate instance via xrt_obj_alloc + type registration
        int inst_bytes = ctor_nfields * 16;  // sizeof(XrValue) per field
        const char *tagged_type = "XrValue";

        // Lookup class metadata for type registration
        const XcgenClassInfo *cls = xcgen_lookup_class(mod->comp, callee_proto);
        if (cls) {
            // Emit lazy type registration: static _tid_ClassName variable
            // initialized once via xrt_type_register.
            // Parent type lookup: search class_infos for parent's type_id var.
            const char *parent_tid = "0";
            char parent_buf[160];
            if (cls->parent_name) {
                snprintf(parent_buf, sizeof(parent_buf), "_tid_%s", cls->parent_name);
                parent_tid = parent_buf;
            }
            xcgen_buf_printf(b,
                             "    { if (!_tid_%s) _tid_%s = xrt_type_register(\"%s\", %s,"
                             " NULL, 0, NULL, %d);\n",
                             cls->class_name, cls->class_name, cls->class_name, parent_tid,
                             inst_bytes);
            xcgen_buf_printf(
                b, "      %s _inst = xr_mkptr(xrt_obj_alloc(_tid_%s, %d), XR_TAG_PTR);\n",
                tagged_type, cls->class_name, inst_bytes);
        } else {
            // Fallback: no class info → raw allocation (legacy path)
            xcgen_buf_printf(b, "    { %s _inst = xr_mkptr(xrt_arc_alloc(%d), XR_TAG_PTR);\n",
                             tagged_type, inst_bytes);
        }
        xcgen_buf_printf(b, "      %s(xrt_ctx, _inst", callee_name);
        // Pass call_args[1..nargs] as constructor args (skip closure at [0])
        for (int i = 0; i < nargs; i++) {
            xcgen_buf_puts(b, ", ");
            int slot = 1 + i;
            if (slot < cf->call_args_cap && !xir_ref_is_none(cf->call_args[slot]))
                xcg_emit_ref_as_tagged(b, func, cf->call_args[slot]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
        }
        xcgen_buf_puts(b, ");\n");
        xcgen_buf_printf(b, "      v%u = _inst; }\n", dst_idx);
        cf->call_args_count = 0;
        for (int i = 0; i < cf->call_args_cap; i++)
            cf->call_args[i] = XIR_NONE;
        return;
    }

    if (callee_name) {
        // Direct C function call.
        // Check if callee always returns void (null) — never assign in that case.
        bool callee_is_void = false;
        if (callee_func_idx >= 0 && callee_func_idx < mod->nfuncs)
            callee_is_void = mod->funcs[callee_func_idx].void_return;

        // Skip assignment when dst vreg is unused (void call site) OR callee returns void.
        bool dst_used = !callee_is_void &&
                        (xir_ref_is_none(ins->dst) || !xir_ref_is_vreg(ins->dst) ||
                         (dst_idx < func->nvreg && cf->used_vregs && cf->used_vregs[dst_idx]));
        // Auto-unbox/box: bridge callee return type ↔ dst vreg type.
        // Callee's C return type depends on its proto's return_type_info.
        uint8_t dst_rep = (dst_idx < func->nvreg) ? func->vregs[dst_idx].rep : XR_REP_TAGGED;
        uint8_t callee_ret_rep =
            (cp && cp->return_type_info) ? xr_type_rep(cp->return_type_info) : XR_REP_TAGGED;
        bool callee_ret_tagged = (callee_ret_rep == XR_REP_PTR || callee_ret_rep == XR_REP_STR ||
                                  callee_ret_rep == XR_REP_TAGGED);
        bool needs_unbox_i64 = (dst_used && dst_rep == XR_REP_I64 && callee_ret_tagged);
        bool needs_unbox_f64 = (dst_used && dst_rep == XR_REP_F64 && callee_ret_tagged);
        bool needs_box_i64 = (dst_used && callee_ret_rep == XR_REP_I64 &&
                              (dst_rep == XR_REP_PTR || dst_rep == XR_REP_TAGGED));
        bool needs_box_f64 = (dst_used && callee_ret_rep == XR_REP_F64 &&
                              (dst_rep == XR_REP_PTR || dst_rep == XR_REP_TAGGED));
        if (dst_used && xir_ref_is_vreg(ins->dst)) {
            if (needs_unbox_i64)
                xcgen_buf_printf(b, "    v%u = XR_TO_INT(%s(", dst_idx, callee_name);
            else if (needs_unbox_f64)
                xcgen_buf_printf(b, "    v%u = XR_TO_FLOAT(%s(", dst_idx, callee_name);
            else if (needs_box_i64)
                xcgen_buf_printf(b, "    v%u = XR_FROM_INT(%s(", dst_idx, callee_name);
            else if (needs_box_f64)
                xcgen_buf_printf(b, "    v%u = XR_FROM_FLOAT(%s(", dst_idx, callee_name);
            else
                xcgen_buf_printf(b, "    v%u = %s(", dst_idx, callee_name);
        } else
            xcgen_buf_printf(b, "    %s(", callee_name);
        // Pass xrt_ctx as implicit first argument
        bool ctx_emitted = true;
        xcgen_buf_puts(b, "xrt_ctx");
        // Pass closure pointer (escaping) or upvalue params (non-escaping).
        bool callee_needs_closure = false;
        bool callee_non_escaping = false;
        int callee_nupvals = 0;
        if (callee_func_idx >= 0 && callee_func_idx < mod->nfuncs) {
            callee_needs_closure = mod->funcs[callee_func_idx].needs_closure_param;
            callee_non_escaping = mod->funcs[callee_func_idx].non_escaping;
            callee_nupvals = mod->funcs[callee_func_idx].num_upvals;
        } else {
            // Callee not yet compiled — check proto_map for non-escaping flag
            XcgenCompilation *comp = mod->comp;
            for (int pi = 0; pi < comp->proto_map_count; pi++) {
                if (comp->proto_map[pi].proto_ptr == callee_proto) {
                    callee_non_escaping = comp->proto_map[pi].non_escaping;
                    callee_nupvals = comp->proto_map[pi].num_upvals;
                    break;
                }
            }
            // Derive upval info from proto when proto_map doesn't have it yet
            if (callee_nupvals == 0 && cp) {
                int proto_nupvals = (int) PROTO_UPVAL_COUNT(cp);
                if (proto_nupvals > 0) {
                    callee_nupvals = proto_nupvals;
                    callee_needs_closure = true;
                }
            }
        }
        if (callee_non_escaping && callee_nupvals > 0 &&
            callee_nupvals <= XCGEN_MAX_NONESC_UPVALS && cf->call_args_count > 0 &&
            !xir_ref_is_none(cf->call_args[0])) {
            // Non-escaping callee: pass upvalue values as direct XrValue args
            uint32_t cl_vreg = 0;
            if (xir_ref_is_vreg(cf->call_args[0]))
                cl_vreg = XIR_REF_INDEX(cf->call_args[0]);
            XirRef upval_refs[XCGEN_MAX_NONESC_UPVALS];
            xcg_collect_upval_refs(func, cl_vreg, callee_nupvals, upval_refs);
            for (int u = 0; u < callee_nupvals; u++) {
                xcgen_buf_puts(b, ", ");
                if (!xir_ref_is_none(upval_refs[u]))
                    xcg_emit_ref_as_tagged(b, func, upval_refs[u]);
                else
                    xcgen_buf_puts(b, "(XrValue){0}");
            }
        } else if (callee_needs_closure && cf->call_args_count > 0 &&
                   !xir_ref_is_none(cf->call_args[0])) {
            xcgen_buf_puts(b, ", (xrt_closure_t*)");
            xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
            xcgen_buf_puts(b, ".ptr");
        } else if (callee_needs_closure) {
            xcgen_buf_puts(b, ", NULL");
        }
        // call_args[0] = closure (skip), call_args[1..n] = actual args
        for (int i = 0; i < nargs; i++) {
            if (ctx_emitted || i > 0)
                xcgen_buf_puts(b, ", ");
            ctx_emitted = false;
            int slot = 1 + i;  // skip slot 0 (closure)
            if (slot < cf->call_args_cap && !xir_ref_is_none(cf->call_args[slot])) {
                XirRef arg_ref = cf->call_args[slot];
                uint8_t arg_type = xcg_ref_type(func, arg_ref);
                bool arg_is_tagged =
                    (arg_type == XR_REP_PTR || arg_type == XR_REP_TAGGED || arg_type == XR_REP_STR);

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

                // Check if the arg vreg is itself a struct-ptr param (already xrs_N*, not XrValue)
                bool arg_is_struct_ptr_param = false;
                if (xir_ref_is_vreg(arg_ref) && cf->vreg_struct_id && mod->struct_reg) {
                    uint32_t arg_vi = XIR_REF_INDEX(arg_ref);
                    if (arg_vi < (uint32_t) cf->num_params) {  // use cached num_params
                        int asi = cf->vreg_struct_id[arg_vi];
                        arg_is_struct_ptr_param = (asi >= 0);
                    }
                }

                if (callee_param_is_struct && (arg_is_tagged || arg_is_struct_ptr_param)) {
                    if (arg_is_struct_ptr_param) {
                        // Caller param is already xrs_N*, pass directly (no .ptr needed)
                        xcg_emit_ref(b, func, arg_ref);
                    } else {
                        // Callee expects xrs_N*, caller has XrValue: cast .ptr
                        xcgen_buf_printf(b, "(%s*)", callee_struct_name);
                        xcg_emit_ref(b, func, arg_ref);
                        xcgen_buf_puts(b, ".ptr");
                    }
                } else if (callee_param_is_struct && !arg_is_tagged) {
                    // Caller has some native type, pass directly
                    xcg_emit_ref(b, func, arg_ref);
                } else {
                    // Normal type coercion: if arg is XrValue but callee expects native type,
                    // unbox
                    XrProto *cp = (XrProto *) callee_proto;
                    uint8_t param_type = 0;
                    if (cp && cp->param_types && i < cp->param_types_count && cp->param_types[i])
                        param_type = xr_type_to_slot_type(cp->param_types[i]);
                    bool param_wants_i64 =
                        (param_type == XR_SLOT_I64 || param_type == XR_SLOT_BOOL);
                    bool param_wants_f64 = (param_type == XR_SLOT_F64);
                    if (arg_is_tagged && param_wants_i64) {
                        xcgen_buf_puts(b, "XR_TO_INT(");
                        xcg_emit_ref(b, func, arg_ref);
                        xcgen_buf_puts(b, ")");
                    } else if (arg_is_tagged && param_wants_f64) {
                        xcgen_buf_puts(b, "XR_TO_FLOAT(");
                        xcg_emit_ref(b, func, arg_ref);
                        xcgen_buf_puts(b, ")");
                    } else if (!arg_is_tagged && !param_wants_i64 && !param_wants_f64) {
                        // Arg is native but callee expects XrValue: auto-box
                        if (arg_type == XR_REP_I64) {
                            xcgen_buf_puts(b, "XR_FROM_INT(");
                            xcg_emit_ref(b, func, arg_ref);
                            xcgen_buf_puts(b, ")");
                        } else if (arg_type == XR_REP_F64) {
                            xcgen_buf_puts(b, "XR_FROM_FLOAT(");
                            xcg_emit_ref(b, func, arg_ref);
                            xcgen_buf_puts(b, ")");
                        } else {
                            xcg_emit_ref(b, func, arg_ref);
                        }
                    } else {
                        xcg_emit_ref(b, func, arg_ref);
                    }
                }
            } else {
                xcgen_buf_puts(b, "0");
            }
        }
        if (needs_unbox_i64 || needs_unbox_f64 || needs_box_i64 || needs_box_f64)
            xcgen_buf_puts(b, "));\n");
        else
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

static void emit_call_self_direct(XcgenBuf *b, XirFunc *func, XirIns *ins, const char *self_name,
                                  XcgenFunc *cf) {
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
            if (need_comma || a > 0)
                xcgen_buf_puts(b, ", ");
            need_comma = false;
            xcg_emit_ref(b, func, ins->args[a]);
        }
    } else {
        // Memory passing: args were stored via STORE_CORO
        int nargs = func->num_params;
        for (int i = 0; i < nargs; i++) {
            if (need_comma || i > 0)
                xcgen_buf_puts(b, ", ");
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

/* ========== CALL_C Translation (closure creation + fallback) ========== */

static void emit_call_c(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf, XcgenModule *mod) {
    XR_DCHECK(b != NULL, "emit_call_c: NULL buf");
    XR_DCHECK(func != NULL, "emit_call_c: NULL func");
    XR_DCHECK(ins != NULL, "emit_call_c: NULL ins");
    /* After xir_resolve_intrinsics(), only non-intrinsic CALL_C remain:
     * closure creation (fn_ptr is a registered proto) and unknown targets. */
    if (!xir_ref_is_const(ins->args[0])) {
        cf->call_args_count = 0;
        return;
    }
    uint32_t ci = XIR_REF_INDEX(ins->args[0]);
    if (ci >= func->nconst || func->consts[ci].rep != XR_REP_PTR) {
        cf->call_args_count = 0;
        return;
    }
    void *fn_ptr = func->consts[ci].val.ptr;

    /* Closure creation: CALL_C(child_proto_ptr, nupvals)
     * Recognize by checking if fn_ptr is a registered proto. */
    const char *closure_fn = xcg_lookup_proto_name(mod, fn_ptr);
    if (closure_fn) {
        uint32_t dst_idx = XIR_REF_INDEX(ins->dst);

        /* Check if the child closure is non-escaping */
        bool child_non_esc = false;
        XcgenCompilation *comp2 = mod->comp;
        for (int pi = 0; pi < comp2->proto_map_count; pi++) {
            if (comp2->proto_map[pi].proto_ptr == fn_ptr) {
                child_non_esc = comp2->proto_map[pi].non_escaping;
                break;
            }
        }
        if (child_non_esc) {
            xcgen_buf_printf(b, "    /* non-escaping closure %s: no allocation */\n", closure_fn);
            cf->call_args_count = 0;
            return;
        }

        /* Escaping closure: allocate at runtime */
        int64_t nupvals = 0;
        if (xir_ref_is_vreg(ins->args[1])) {
            uint32_t vi = XIR_REF_INDEX(ins->args[1]);
            for (uint32_t bi2 = 0; bi2 < func->nblk; bi2++) {
                XirBlock *blk2 = func->blocks[bi2];
                for (uint32_t ii = 0; ii < blk2->nins; ii++) {
                    XirIns *def = &blk2->ins[ii];
                    if (xir_ref_is_vreg(def->dst) && XIR_REF_INDEX(def->dst) == vi &&
                        def->op == XIR_CONST_I64 && xir_ref_is_const(def->args[0])) {
                        uint32_t ci2 = XIR_REF_INDEX(def->args[0]);
                        if (ci2 < func->nconst)
                            nupvals = func->consts[ci2].val.i64;
                    }
                }
            }
        }
        xcgen_buf_printf(b, "    v%u = xrt_closure_new((void*)%s, %d);\n", dst_idx, closure_fn,
                         (int) nupvals);
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }
    /* Unknown CALL_C target after intrinsic resolution.
     * This should not happen — emit a compile-time warning so it is
     * visible rather than silently producing undefined register values. */
    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
    xcgen_buf_printf(b, "    /* WARNING: unresolved CALL_C fn_ptr=%p → v%u undefined */\n", fn_ptr,
                     dst_idx);
    fprintf(stderr, "AOT warning: unresolved CALL_C fn_ptr=%p in %s (dst v%u)\n", fn_ptr,
            cf->c_name ? cf->c_name : "?", dst_idx);
    cf->call_args_count = 0;
}

/* ========== Public Entry Point ========== */

bool xcg_emit_call_instruction(XcgenBuf *b, XirFunc *func, XirIns *ins, const char *self_name,
                               XcgenModule *mod, XcgenFunc *cf) {
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

        case XIR_CALL_INTRINSIC:
            load_call_args_from_pool(func, ins, cf);
            xcg_emit_call_intrinsic(b, func, ins, cf, mod);
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
                        if (xir_ref_is_vreg(def->dst) && XIR_REF_INDEX(def->dst) == vi &&
                            def->op == XIR_CONST_I64 && xir_ref_is_const(def->args[0])) {
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
            for (int i = 0; i < (int) nargs; i++) {
                XirRef arg_ref = cf->call_args[1 + i];
                uint8_t arg_type = xcg_ref_type(func, arg_ref);
                xcgen_buf_printf(b, ", %s", xcg_c_type(arg_type));
            }
            xcgen_buf_puts(b, "))((xrt_closure_t*)");
            xcg_emit_ref_as_tagged(b, func, closure_ref);
            xcgen_buf_puts(b, ".ptr)->fn)(");
            xcgen_buf_puts(b, "xrt_ctx, ");
            xcg_emit_ref_as_tagged(b, func, closure_ref);
            for (int i = 0; i < (int) nargs; i++) {
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
