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
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xstring.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "../jit/xir_intrinsic.h"
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

// Forward declaration (defined below, after CALL_C helpers)
static void emit_ref_as_tagged(XcgenBuf *b, XirFunc *func, XirRef ref);

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
        int inst_bytes = ctor_nfields * 16;  // sizeof(XrtValue) per field
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
                b, "      %s _inst = xrt_mkptr(xrt_obj_alloc(_tid_%s, %d), XRT_TAG_PTR);\n",
                tagged_type, cls->class_name, inst_bytes);
        } else {
            // Fallback: no class info → raw allocation (legacy path)
            xcgen_buf_printf(b, "    { %s _inst = xrt_mkptr(xrt_arc_alloc(%d), XRT_TAG_PTR);\n",
                             tagged_type, inst_bytes);
        }
        xcgen_buf_printf(b, "      %s(xrt_ctx, _inst", callee_name);
        // Pass call_args[1..nargs] as constructor args (skip closure at [0])
        for (int i = 0; i < nargs; i++) {
            xcgen_buf_puts(b, ", ");
            int slot = 1 + i;
            if (slot < cf->call_args_cap && !xir_ref_is_none(cf->call_args[slot]))
                emit_ref_as_tagged(b, func, cf->call_args[slot]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
        }
        xcgen_buf_puts(b, ");\n");
        xcgen_buf_printf(b, "      v%u = _inst; }\n", dst_idx);
        cf->needs_gc = true;
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
                xcgen_buf_printf(b, "    v%u = xrt_unbox_int(%s(", dst_idx, callee_name);
            else if (needs_unbox_f64)
                xcgen_buf_printf(b, "    v%u = xrt_unbox_float(%s(", dst_idx, callee_name);
            else if (needs_box_i64)
                xcgen_buf_printf(b, "    v%u = xrt_box_int(%s(", dst_idx, callee_name);
            else if (needs_box_f64)
                xcgen_buf_printf(b, "    v%u = xrt_box_float(%s(", dst_idx, callee_name);
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
            // Non-escaping callee: pass upvalue values as direct XrtValue args
            uint32_t cl_vreg = 0;
            if (xir_ref_is_vreg(cf->call_args[0]))
                cl_vreg = XIR_REF_INDEX(cf->call_args[0]);
            XirRef upval_refs[XCGEN_MAX_NONESC_UPVALS];
            xcg_collect_upval_refs(func, cl_vreg, callee_nupvals, upval_refs);
            for (int u = 0; u < callee_nupvals; u++) {
                xcgen_buf_puts(b, ", ");
                if (!xir_ref_is_none(upval_refs[u]))
                    emit_ref_as_tagged(b, func, upval_refs[u]);
                else
                    xcgen_buf_puts(b, "(XrtValue){0}");
            }
        } else if (callee_needs_closure && cf->call_args_count > 0 &&
                   !xir_ref_is_none(cf->call_args[0])) {
            xcgen_buf_puts(b, ", (xrt_closure_t*)");
            emit_ref_as_tagged(b, func, cf->call_args[0]);
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

                // Check if the arg vreg is itself a struct-ptr param (already xrs_N*, not XrtValue)
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
                        // Callee expects xrs_N*, caller has XrtValue: cast .ptr
                        xcgen_buf_printf(b, "(%s*)", callee_struct_name);
                        xcg_emit_ref(b, func, arg_ref);
                        xcgen_buf_puts(b, ".ptr");
                    }
                } else if (callee_param_is_struct && !arg_is_tagged) {
                    // Caller has some native type, pass directly
                    xcg_emit_ref(b, func, arg_ref);
                } else {
                    // Normal type coercion: if arg is XrtValue but callee expects native type,
                    // unbox
                    XrProto *cp = (XrProto *) callee_proto;
                    uint8_t param_type = 0;
                    if (cp && cp->param_types && i < cp->param_types_count && cp->param_types[i])
                        param_type = xr_type_to_slot_type(cp->param_types[i]);
                    bool param_wants_i64 =
                        (param_type == XR_SLOT_I64 || param_type == XR_SLOT_BOOL);
                    bool param_wants_f64 = (param_type == XR_SLOT_F64);
                    if (arg_is_tagged && param_wants_i64) {
                        xcgen_buf_puts(b, "xrt_unbox_int(");
                        xcg_emit_ref(b, func, arg_ref);
                        xcgen_buf_puts(b, ")");
                    } else if (arg_is_tagged && param_wants_f64) {
                        xcgen_buf_puts(b, "xrt_unbox_float(");
                        xcg_emit_ref(b, func, arg_ref);
                        xcgen_buf_puts(b, ")");
                    } else if (!arg_is_tagged && !param_wants_i64 && !param_wants_f64) {
                        // Arg is native but callee expects XrValue: auto-box
                        if (arg_type == XR_REP_I64) {
                            xcgen_buf_puts(b, "xrt_box_int(");
                            xcg_emit_ref(b, func, arg_ref);
                            xcgen_buf_puts(b, ")");
                        } else if (arg_type == XR_REP_F64) {
                            xcgen_buf_puts(b, "xrt_box_float(");
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

/* ========== CALL_C Translation ========== */

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

/* ========== CALL_INTRINSIC Translation (table-driven) ========== */

/* Resolve the intrinsic ID from a CALL_INTRINSIC instruction.
 * After xir_resolve_intrinsics(), args[0] is a const i64 holding the ID. */
static int resolve_intrinsic_id(XirFunc *func, XirIns *ins) {
    int64_t id = XR_INTRIN_NONE;
    if (xir_ref_is_const(ins->args[0])) {
        uint32_t ci = XIR_REF_INDEX(ins->args[0]);
        if (ci < func->nconst)
            id = func->consts[ci].val.i64;
    }
    return (int) id;
}

static void emit_call_intrinsic(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf,
                                 XcgenModule *mod) {
    XR_DCHECK(b != NULL, "emit_call_intrinsic: NULL buf");
    XR_DCHECK(func != NULL, "emit_call_intrinsic: NULL func");
    XR_DCHECK(ins != NULL, "emit_call_intrinsic: NULL ins");
    const char *tagged_type = "XrValue";
    int intrin = resolve_intrinsic_id(func, ins);

    switch (intrin) {

    case XR_INTRIN_JSON_NEW_SHAPE: {
        if (!mod->struct_reg) {
            cf->call_args_count = 0;
            return;
        }
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
            xcgen_buf_printf(
                b, "    v%u = (%s){.ptr = xrt_arc_alloc(sizeof(%s)), .tag = XRT_TAG_PTR};\n",
                dst_idx, tagged_type, st->c_name);
            if (!st->all_native) {
                xcgen_buf_printf(b,
                                 "    { XrtArcHdr *_h = XRT_ARC_HDR(v%u.ptr);"
                                 " _h->type = %d; _h->flags |= XRT_ARC_HAS_DEINIT; }\n",
                                 dst_idx, sidx);
            }
            if (cf->vreg_struct_id && dst_idx < func->nvreg)
                cf->vreg_struct_id[dst_idx] = (int16_t) sidx;
            cf->needs_gc = true;
        }
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_GETPROP: {
        uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
        int64_t symbol_id = -1;
        xcg_resolve_const_i64(func, ins->args[1], &symbol_id);
        if (mod->struct_reg) {
            const XcgenFieldEntry *fe =
                xcgen_find_field_by_symbol(mod->struct_reg, (uint32_t) symbol_id);
            if (fe) {
                XcgenStruct *st = &mod->struct_reg->structs[fe->struct_idx];
                xcgen_buf_printf(b, "    v%u = ((%s*)v%u.ptr)->%s;\n", dst_idx, st->c_name,
                                 XIR_REF_INDEX(cf->call_args[0]),
                                 st->fields[fe->field_idx].name);
                cf->call_args_count = 0;
                return;
            }
        }
        uint8_t dst_type = xcg_ref_type(func, ins->dst);
        if ((symbol_id == 1 || symbol_id == 2 || symbol_id == 3) && cf->call_args_count >= 1) {
            if (dst_type == XR_REP_I64) {
                xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                emit_ref_as_tagged(b, func, cf->call_args[0]);
                xcgen_buf_printf(b, ", %lldLL).i;\n", (long long) symbol_id);
            } else {
                xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                emit_ref_as_tagged(b, func, cf->call_args[0]);
                xcgen_buf_printf(b, ", %lldLL);\n", (long long) symbol_id);
            }
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }
        if (cf->call_args_count >= 1) {
            if (dst_type == XR_REP_I64) {
                xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                emit_ref_as_tagged(b, func, cf->call_args[0]);
                xcgen_buf_printf(b, ", %lldLL).i;\n", (long long) symbol_id);
            } else if (dst_type == XR_REP_F64) {
                xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                emit_ref_as_tagged(b, func, cf->call_args[0]);
                xcgen_buf_printf(b, ", %lldLL).f;\n", (long long) symbol_id);
            } else {
                xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                emit_ref_as_tagged(b, func, cf->call_args[0]);
                xcgen_buf_printf(b, ", %lldLL);\n", (long long) symbol_id);
            }
            cf->needs_runtime = true;
        } else {
            if (dst_type == XR_REP_I64)
                xcgen_buf_printf(b, "    v%u = 0;\n", dst_idx);
            else if (dst_type == XR_REP_F64)
                xcgen_buf_printf(b, "    v%u = 0.0;\n", dst_idx);
            else {
                xcgen_buf_printf(b, "    v%u = (%s){.i = 0, .tag = XRT_TAG_NULL};\n", dst_idx,
                                 tagged_type);
                cf->needs_runtime = true;
            }
        }
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_INDEX_GET: {
        uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
        if (cf->call_args_count >= 2) {
            uint8_t dst_type = xcg_ref_type(func, ins->dst);
            uint8_t key_type = xcg_ref_type(func, cf->call_args[1]);
            bool key_is_i64 = (key_type == XR_REP_I64);
            if (dst_type == XR_REP_I64 && key_is_i64) {
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
        } else {
            xcgen_buf_printf(b, "    v%u = xrt_index_get(", dst_idx);
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
        }
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_INDEX_SET: {
        if (cf->call_args_count >= 3) {
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
        } else {
            xcgen_buf_puts(b, "    xrt_index_set(");
            emit_ref_as_tagged(b, func, cf->call_args[0]);
            xcgen_buf_puts(b, ", ");
            emit_ref_as_tagged(b, func, cf->call_args[1]);
            xcgen_buf_puts(b, ", ");
            if (cf->call_args_count > 2)
                emit_ref_as_tagged(b, func, cf->call_args[2]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ");\n");
        }
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_THROW: {
        XirIns *throw_ins = ins;
        bool has_local_catch = false;
        for (uint32_t tbi = 0; tbi < func->nblk; tbi++) {
            XirBlock *tblk = func->blocks[tbi];
            for (uint32_t ti = 0; ti < tblk->nins; ti++) {
                if (&tblk->ins[ti] == throw_ins) {
                    has_local_catch = (tblk->exception_handler != NULL);
                    goto throw_done;
                }
            }
        }
    throw_done:
        if (has_local_catch) {
            xcgen_buf_puts(b, "    xrt_exception = ");
            emit_ref_as_tagged(b, func, ins->args[1]);
            xcgen_buf_puts(b, ";\n");
            cf->needs_exception = true;
        } else {
            xcgen_buf_puts(b, "    xrt_throw_exc(");
            emit_ref_as_tagged(b, func, ins->args[1]);
            xcgen_buf_puts(b, ");\n");
        }
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_TARRAY_GET: {
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

    case XR_INTRIN_TARRAY_SET: {
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

    case XR_INTRIN_MAP_GET: {
        uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
        xcgen_buf_printf(b, "    v%u = xrt_map_get((xrt_map_t *)", dst_idx);
        if (cf->call_args_count > 0)
            xcg_emit_ref(b, func, cf->call_args[0]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_puts(b, ".ptr, ");
        if (cf->call_args_count > 1)
            emit_ref_as_tagged(b, func, cf->call_args[1]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_MAP_SET: {
        xcgen_buf_puts(b, "    xrt_map_set((xrt_map_t *)");
        if (cf->call_args_count > 0)
            xcg_emit_ref(b, func, cf->call_args[0]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_puts(b, ".ptr, ");
        if (cf->call_args_count > 1)
            emit_ref_as_tagged(b, func, cf->call_args[1]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_puts(b, ", ");
        if (cf->call_args_count > 2)
            emit_ref_as_tagged(b, func, cf->call_args[2]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_MAP_INCREMENT: {
        xcgen_buf_puts(b, "    { xrt_map_t *_m = (xrt_map_t *)");
        if (cf->call_args_count > 0)
            xcg_emit_ref(b, func, cf->call_args[0]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_puts(b, ".ptr; ");
        xcgen_buf_puts(b, "XrtValue _k = ");
        if (cf->call_args_count > 1)
            emit_ref_as_tagged(b, func, cf->call_args[1]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_puts(b, "; XrtValue _cv = xrt_map_get(_m, _k); ");
        xcgen_buf_puts(b, "xrt_map_set(_m, _k, xrt_box_int((_cv.tag == XRT_TAG_I64 ? _cv.i "
                          ": 0) + 1)); }\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_STRBUF_NEW: {
        uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
        xcgen_buf_printf(b, "    v%u = xrt_strbuf_new();\n", dst_idx);
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_STRBUF_APPEND: {
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

    case XR_INTRIN_STRBUF_FINISH: {
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

    case XR_INTRIN_INVOKE_METHOD: {
        int64_t encoded = 0;
        xcg_resolve_const_i64(func, ins->args[1], &encoded);
        uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
        uint8_t dst_vtype = (dst_idx < func->nvreg) ? func->vregs[dst_idx].rep : XR_REP_TAGGED;
        bool dst_is_float = xcg_is_float_type(dst_vtype);
        bool dst_is_int = (dst_vtype == XR_REP_I64);

        if (encoded < 0) {
            int slot_hint = (int) (-(encoded + 1));
            xcgen_buf_printf(b, "    v%u = xrt_tostring(", dst_idx);
            if (cf->call_args_count > 0)
                emit_ref_as_tagged(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_printf(b, ", %d);\n", slot_hint);
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        int method_symbol = (int) (encoded >> 32);
        int nargs = (int) (encoded & 0xFF);
        XirRef recv_ref = (cf->call_args_count > 0) ? cf->call_args[0] : XIR_NONE;
        uint8_t recv_type =
            xir_ref_is_none(recv_ref) ? XR_REP_TAGGED : xcg_ref_type(func, recv_ref);
        bool recv_is_float = xcg_is_float_type(recv_type);
        bool recv_is_int = (recv_type == XR_REP_I64);
        bool recv_is_str = (recv_type == XR_REP_STR);
        bool recv_is_tagged = (recv_type == XR_REP_PTR || recv_type == XR_REP_TAGGED);

        /* Inline float math: floor/ceil/round/abs/sqrt/pow */
        const char *math_fn = NULL;
        if (recv_is_float && dst_is_float && nargs == 0) {
            switch (method_symbol) {
                case XRT_SYM_FLOOR: math_fn = "floor"; break;
                case XRT_SYM_CEIL:  math_fn = "ceil";  break;
                case XRT_SYM_ROUND: math_fn = "round"; break;
                case XRT_SYM_ABS:   math_fn = "fabs";  break;
                case XRT_SYM_SQRT:  math_fn = "sqrt";  break;
                default: break;
            }
        }
        if (recv_is_float && dst_is_float && nargs == 1 && method_symbol == XRT_SYM_POW) {
            XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
            uint8_t a1t = xir_ref_is_none(arg1) ? XR_REP_TAGGED : xcg_ref_type(func, arg1);
            if (xcg_is_float_type(a1t) || a1t == XR_REP_I64) {
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

        /* Inline I64 abs */
        if (recv_is_int && dst_is_int && nargs == 0 && method_symbol == XRT_SYM_ABS) {
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

        /* Inline string methods: length/size/isEmpty/contains/indexOf/startsWith/endsWith */
        if (recv_is_str && nargs == 0) {
            if (dst_is_int && (method_symbol == XRT_SYM_LENGTH || method_symbol == XRT_SYM_SIZE)) {
                xcgen_buf_printf(b, "    v%u = (int64_t)strlen((const char *)", dst_idx);
                xcg_emit_ref(b, func, recv_ref);
                xcgen_buf_puts(b, ".ptr);\n");
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }
            if ((dst_is_int || dst_vtype == XR_REP_TAGGED) && method_symbol == XRT_SYM_IS_EMPTY) {
                if (dst_vtype == XR_REP_TAGGED)
                    xcgen_buf_printf(b, "    v%u = xrt_box_bool(", dst_idx);
                else
                    xcgen_buf_printf(b, "    v%u = ", dst_idx);
                xcgen_buf_puts(b, "(((const char *)");
                xcg_emit_ref(b, func, recv_ref);
                xcgen_buf_puts(b, ".ptr)[0] == '\\0') ? 1 : 0");
                if (dst_vtype == XR_REP_TAGGED)
                    xcgen_buf_puts(b, ");");
                else
                    xcgen_buf_puts(b, ";");
                xcgen_buf_puts(b, "\n");
                cf->call_args_count = 0;
                return;
            }
        }
        if (recv_is_str && nargs == 1) {
            XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
            uint8_t a1t = xir_ref_is_none(arg1) ? XR_REP_TAGGED : xcg_ref_type(func, arg1);
            bool a1_str = (a1t == XR_REP_STR);
            if ((dst_is_int || dst_vtype == XR_REP_TAGGED) && a1_str &&
                method_symbol == XRT_SYM_CONTAINS) {
                if (dst_vtype == XR_REP_TAGGED)
                    xcgen_buf_printf(b, "    v%u = xrt_box_bool(", dst_idx);
                else
                    xcgen_buf_printf(b, "    v%u = ", dst_idx);
                xcgen_buf_puts(b, "strstr((const char *)");
                xcg_emit_ref(b, func, recv_ref);
                xcgen_buf_puts(b, ".ptr, (const char *)");
                xcg_emit_ref(b, func, arg1);
                xcgen_buf_puts(b, ".ptr) ? 1 : 0");
                if (dst_vtype == XR_REP_TAGGED)
                    xcgen_buf_puts(b, ");");
                else
                    xcgen_buf_puts(b, ";");
                xcgen_buf_puts(b, "\n");
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }
            if (dst_is_int && a1_str && method_symbol == XRT_SYM_INDEXOF) {
                xcgen_buf_printf(b, "    { const char *_s = (const char *)", dst_idx);
                xcg_emit_ref(b, func, recv_ref);
                xcgen_buf_puts(b, ".ptr; const char *_p = strstr(_s, (const char *)");
                xcg_emit_ref(b, func, arg1);
                xcgen_buf_printf(b, ".ptr); v%u = _p ? (int64_t)(_p - _s) : -1; }\n", dst_idx);
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }
            if ((dst_is_int || dst_vtype == XR_REP_TAGGED) && a1_str &&
                method_symbol == XRT_SYM_STARTSWITH) {
                xcgen_buf_printf(b, "    { const char *_s = (const char *)");
                xcg_emit_ref(b, func, recv_ref);
                xcgen_buf_puts(b, ".ptr; const char *_p = (const char *)");
                xcg_emit_ref(b, func, arg1);
                xcgen_buf_puts(b, ".ptr; size_t _pl = strlen(_p); ");
                if (dst_vtype == XR_REP_TAGGED)
                    xcgen_buf_printf(b, "v%u = xrt_box_bool((strlen(_s) >= _pl && "
                                     "memcmp(_s, _p, _pl) == 0) ? 1 : 0); }\n", dst_idx);
                else
                    xcgen_buf_printf(b, "v%u = (strlen(_s) >= _pl && memcmp(_s, _p, _pl) "
                                     "== 0) ? 1 : 0; }\n", dst_idx);
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }
            if ((dst_is_int || dst_vtype == XR_REP_TAGGED) && a1_str &&
                method_symbol == XRT_SYM_ENDSWITH) {
                xcgen_buf_printf(b, "    { const char *_s = (const char *)");
                xcg_emit_ref(b, func, recv_ref);
                xcgen_buf_puts(b, ".ptr; size_t _sl = strlen(_s); const char *_p = (const char *)");
                xcg_emit_ref(b, func, arg1);
                xcgen_buf_puts(b, ".ptr; size_t _pl = strlen(_p); ");
                if (dst_vtype == XR_REP_TAGGED)
                    xcgen_buf_printf(b, "v%u = xrt_box_bool((_sl >= _pl && memcmp(_s + "
                                     "_sl - _pl, _p, _pl) == 0) ? 1 : 0); }\n", dst_idx);
                else
                    xcgen_buf_printf(b, "v%u = (_sl >= _pl && memcmp(_s + _sl - _pl, _p, "
                                     "_pl) == 0) ? 1 : 0; }\n", dst_idx);
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }
        }

        /* Inline array push/pop */
        if (recv_is_tagged && nargs == 1 && method_symbol == XRT_SYM_PUSH) {
            XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
            xcgen_buf_puts(b, "    xrt_array_push(");
            xcg_emit_ref(b, func, recv_ref);
            xcgen_buf_puts(b, ", ");
            emit_ref_as_tagged(b, func, arg1);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }
        if (recv_is_tagged && nargs == 0 && method_symbol == XRT_SYM_POP) {
            xcgen_buf_printf(b, "    { xrt_array_t *_a = (xrt_array_t *)");
            xcg_emit_ref(b, func, recv_ref);
            xcgen_buf_printf(b, ".ptr; v%u = (_a->len > 0) ? _a->data[--_a->len] "
                             ": (XrtValue){0}; }\n", dst_idx);
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        /* Inline map.get/set/has */
        if (recv_is_tagged && nargs == 1 && method_symbol == XRT_SYM_GET) {
            XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
            xcgen_buf_printf(b, "    v%u = xrt_map_get((xrt_map_t *)", dst_idx);
            xcg_emit_ref(b, func, recv_ref);
            xcgen_buf_puts(b, ".ptr, ");
            emit_ref_as_tagged(b, func, arg1);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }
        if (recv_is_tagged && nargs == 2 && method_symbol == XRT_SYM_SET) {
            XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
            XirRef arg2 = (cf->call_args_count > 2) ? cf->call_args[2] : XIR_NONE;
            xcgen_buf_puts(b, "    xrt_map_set((xrt_map_t *)");
            xcg_emit_ref(b, func, recv_ref);
            xcgen_buf_puts(b, ".ptr, ");
            emit_ref_as_tagged(b, func, arg1);
            xcgen_buf_puts(b, ", ");
            emit_ref_as_tagged(b, func, arg2);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }
        if (recv_is_tagged && nargs == 1 && method_symbol == XRT_SYM_HAS) {
            XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
            xcgen_buf_printf(b, "    { xrt_map_t *_m = (xrt_map_t *)");
            xcg_emit_ref(b, func, recv_ref);
            xcgen_buf_puts(b, ".ptr; int64_t _found = 0; ");
            xcgen_buf_puts(b, "for (int64_t _i = 0; _i < _m->len; _i++) ");
            xcgen_buf_puts(b, "if (xrt_key_eq(_m->entries[_i].key, ");
            emit_ref_as_tagged(b, func, arg1);
            if (dst_vtype == XR_REP_TAGGED)
                xcgen_buf_printf(b, ")) { _found = 1; break; } v%u = xrt_box_bool(_found); }\n",
                                dst_idx);
            else
                xcgen_buf_printf(b, ")) { _found = 1; break; } v%u = _found; }\n", dst_idx);
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        /* Fixed-arity fallback: xrt_method_N */
        if (nargs == 0) {
            if (dst_is_float)
                xcgen_buf_printf(b, "    v%u = xrt_unbox_float(xrt_method_0(", dst_idx);
            else if (dst_is_int)
                xcgen_buf_printf(b, "    v%u = xrt_unbox_int(xrt_method_0(", dst_idx);
            else
                xcgen_buf_printf(b, "    v%u = xrt_method_0(", dst_idx);
            if (cf->call_args_count > 0)
                emit_ref_as_tagged(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_printf(b, ", %d", method_symbol);
        } else if (nargs == 1) {
            if (dst_is_float)
                xcgen_buf_printf(b, "    v%u = xrt_unbox_float(xrt_method_1(", dst_idx);
            else if (dst_is_int)
                xcgen_buf_printf(b, "    v%u = xrt_unbox_int(xrt_method_1(", dst_idx);
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
            if (dst_is_float)
                xcgen_buf_printf(b, "    v%u = xrt_unbox_float(xrt_method_2(", dst_idx);
            else if (dst_is_int)
                xcgen_buf_printf(b, "    v%u = xrt_unbox_int(xrt_method_2(", dst_idx);
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
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_GET_SHARED: {
        int64_t shared_idx = -1;
        if (xcg_resolve_const_i64(func, ins->args[1], &shared_idx) && shared_idx >= 0) {
            uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
            uint8_t dst_type = (dst_idx < func->nvreg) ? func->vregs[dst_idx].rep : XR_REP_TAGGED;
            bool dst_tagged = (dst_type == XR_REP_STR || dst_type == XR_REP_PTR ||
                               dst_type == XR_REP_TAGGED);
            if (dst_tagged)
                xcgen_buf_printf(b, "    v%u = xrt_shared[%d];\n", dst_idx, (int) shared_idx);
            else
                xcgen_buf_printf(b, "    v%u = xrt_shared[%d].i;\n", dst_idx, (int) shared_idx);
            XcgenCompilation *comp = mod->comp;
            XR_DCHECK(comp != NULL, "emit_call_intrinsic GETSHARED: NULL comp");
            if ((int) shared_idx > comp->max_shared_index)
                comp->max_shared_index = (int) shared_idx;
            cf->needs_runtime = true;
        } else {
            xcgen_buf_puts(b, "    /* GETSHARED: unresolved index */\n");
        }
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_SET_SHARED: {
        int64_t encoded = -1;
        if (xcg_resolve_const_i64(func, ins->args[1], &encoded) && encoded >= 0) {
            int shared_idx = (int) (encoded & 0xFFFF);
            xcgen_buf_printf(b, "    xrt_shared[%d] = ", shared_idx);
            if (cf->call_args_count > 0)
                emit_ref_as_tagged(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ";\n");
            XcgenCompilation *comp = mod->comp;
            XR_DCHECK(comp != NULL, "emit_call_intrinsic SETSHARED: NULL comp");
            if (shared_idx > comp->max_shared_index)
                comp->max_shared_index = shared_idx;
            cf->needs_runtime = true;
        } else {
            xcgen_buf_puts(b, "    /* SETSHARED: unresolved encoded */\n");
        }
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_PRINT: {
        int64_t flags = 0;
        xcg_resolve_const_i64(func, ins->args[1], &flags);
        int newline = (int) (flags & 1);
        int add_space = (int) ((flags >> 1) & 1);
        if (add_space)
            xcgen_buf_puts(b, "    printf(\" \");\n");
        xcgen_buf_printf(b, "    %s(", newline ? "xrt_println" : "xrt_print");
        if (cf->call_args_count > 0)
            emit_ref_as_tagged(b, func, cf->call_args[0]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_RT_ADD:
    case XR_INTRIN_RT_SUB:
    case XR_INTRIN_RT_MUL:
    case XR_INTRIN_RT_DIV:
    case XR_INTRIN_RT_MOD: {
        const char *arith_name = NULL;
        switch (intrin) {
            case XR_INTRIN_RT_ADD: arith_name = "xrt_add"; break;
            case XR_INTRIN_RT_SUB: arith_name = "xrt_sub"; break;
            case XR_INTRIN_RT_MUL: arith_name = "xrt_mul"; break;
            case XR_INTRIN_RT_DIV: arith_name = "xrt_div"; break;
            case XR_INTRIN_RT_MOD: arith_name = "xrt_mod"; break;
            default: arith_name = "xrt_add"; break;
        }
        uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
        uint8_t dst_type = (dst_idx < func->nvreg) ? func->vregs[dst_idx].rep : XR_REP_TAGGED;
        bool di = (dst_type == XR_REP_I64);
        bool df = (dst_type == XR_REP_F64);
        if (di)
            xcgen_buf_printf(b, "    v%u = xrt_unbox_int(%s(", dst_idx, arith_name);
        else if (df)
            xcgen_buf_printf(b, "    v%u = xrt_unbox_float(%s(", dst_idx, arith_name);
        else
            xcgen_buf_printf(b, "    v%u = %s(", dst_idx, arith_name);
        if (cf->call_args_count > 0)
            emit_ref_as_tagged(b, func, cf->call_args[0]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_puts(b, ", ");
        if (cf->call_args_count > 1)
            emit_ref_as_tagged(b, func, cf->call_args[1]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        if (di || df)
            xcgen_buf_puts(b, "));\n");
        else
            xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_SUBSTRING: {
        /* call_args: [0]=str, [1]=start, [2]=end */
        uint32_t di = XIR_REF_INDEX(ins->dst);
        xcgen_buf_printf(b, "    v%u = xrt_method_2(", di);
        if (cf->call_args_count > 0)
            emit_ref_as_tagged(b, func, cf->call_args[0]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_printf(b, ", %d, ", XRT_SYM_SUBSTRING);
        if (cf->call_args_count > 1)
            emit_ref_as_tagged(b, func, cf->call_args[1]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_puts(b, ", ");
        if (cf->call_args_count > 2)
            emit_ref_as_tagged(b, func, cf->call_args[2]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_STR_REPEAT: {
        /* call_args: [0]=str, [1]=count */
        uint32_t di = XIR_REF_INDEX(ins->dst);
        xcgen_buf_printf(b, "    v%u = xrt_method_1(", di);
        if (cf->call_args_count > 0)
            emit_ref_as_tagged(b, func, cf->call_args[0]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_printf(b, ", %d, ", XRT_SYM_REPEAT);
        if (cf->call_args_count > 1)
            emit_ref_as_tagged(b, func, cf->call_args[1]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_CHR: {
        /* call_args: [0]=code_point (i64) */
        uint32_t di = XIR_REF_INDEX(ins->dst);
        xcgen_buf_printf(b, "    { char _cb[5] = {0}; int64_t _cp = ");
        if (cf->call_args_count > 0)
            xcg_emit_ref(b, func, cf->call_args[0]);
        else
            xcgen_buf_puts(b, "0");
        xcgen_buf_printf(b, "; if (_cp >= 0 && _cp < 128) { _cb[0] = (char)_cp; } ");
        xcgen_buf_printf(b, "v%u = xrt_str_concat(_cb, \"\"); }\n", di);
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    case XR_INTRIN_TYPEOF:
        /* No specialized AOT lowering yet */
        cf->call_args_count = 0;
        return;

    default:
        fprintf(stderr, "AOT: unhandled intrinsic %d\n", intrin);
        cf->call_args_count = 0;
        return;
    }
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
            xcgen_buf_printf(b, "    /* non-escaping closure %s: no allocation */\n",
                             closure_fn);
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
        xcgen_buf_printf(b, "    v%u = xrt_closure_new((void*)%s, %d);\n", dst_idx,
                         closure_fn, (int) nupvals);
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }
    /* Unknown CALL_C target: suppress (not a known proto or intrinsic) */
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
            emit_call_intrinsic(b, func, ins, cf, mod);
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
            emit_ref_as_tagged(b, func, closure_ref);
            xcgen_buf_puts(b, ".ptr)->fn)(");
            xcgen_buf_puts(b, "xrt_ctx, ");
            emit_ref_as_tagged(b, func, closure_ref);
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
