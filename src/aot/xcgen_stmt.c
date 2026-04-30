/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcgen_stmt.c - AOT C code generator: control flow translation
 *
 * KEY CONCEPT:
 *   Translates XIR block terminators (branch/jump/return) and phi
 *   node lowering into C goto/if/return statements.
 *
 * RELATED MODULES:
 *   - xcgen.c: module-level orchestration
 *   - xcgen_expr.c: expression/instruction translation
 */

#include "xcgen.h"
#include "../runtime/value/xchunk.h"
#include "../base/xchecks.h"
#include <stdio.h>

/* ========== Phi Node Lowering ========== */

static bool block_has_phis(XirBlock *blk) {
    return blk->phis != NULL;
}

// Check if a vreg is used by any instruction (not counting phi copies themselves).
// Used to detect dead phi nodes caused by bytecode register reuse.
static bool xcg_vreg_has_ins_use(XirFunc *func, uint32_t vi) {
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (xir_ref_is_vreg(ins->args[0]) && XIR_REF_INDEX(ins->args[0]) == vi)
                return true;
            if (xir_ref_is_vreg(ins->args[1]) && XIR_REF_INDEX(ins->args[1]) == vi)
                return true;
        }
    }
    return false;
}

void xcg_emit_phi_copies_for_edge(XcgenBuf *b, XirFunc *func, XirBlock *from, XirBlock *to) {
    XR_DCHECK(b != NULL, "xcg_emit_phi_copies_for_edge: NULL buf");
    XR_DCHECK(func != NULL, "xcg_emit_phi_copies_for_edge: NULL func");
    XR_DCHECK(from != NULL, "xcg_emit_phi_copies_for_edge: NULL from");
    XR_DCHECK(to != NULL, "xcg_emit_phi_copies_for_edge: NULL to");
    // Find which predecessor index 'from' is in 'to'
    uint32_t pred_idx = 0;
    for (uint32_t i = 0; i < to->npred; i++) {
        if (to->preds[i] == from) {
            pred_idx = i;
            break;
        }
    }

    for (XirPhi *phi = to->phis; phi; phi = phi->next) {
        if (pred_idx < phi->narg && !xir_ref_is_none(phi->args[pred_idx])) {
            XirRef src = phi->args[pred_idx];
            uint8_t dst_type = phi->rep;
            uint8_t src_type = xcg_ref_type(func, src);
            bool dst_tagged =
                (dst_type == XR_REP_STR || dst_type == XR_REP_PTR || dst_type == XR_REP_TAGGED);
            bool src_tagged =
                (src_type == XR_REP_STR || src_type == XR_REP_PTR || src_type == XR_REP_TAGGED);

            xcgen_buf_printf(b, "    phi_v%u = ", XIR_REF_INDEX(phi->dst));
            if (dst_tagged && !src_tagged) {
                // Auto-box: int64_t/double → XrValue
                if (src_type == XR_REP_I64) {
                    xcgen_buf_puts(b, "XR_FROM_INT(");
                    xcg_emit_ref(b, func, src);
                    xcgen_buf_puts(b, ")");
                } else if (src_type == XR_REP_F64) {
                    xcgen_buf_puts(b, "XR_FROM_FLOAT(");
                    xcg_emit_ref(b, func, src);
                    xcgen_buf_puts(b, ")");
                } else {
                    xcg_emit_ref(b, func, src);
                }
            } else if (!dst_tagged && src_tagged) {
                // Auto-unbox: XrValue → int64_t/double
                // Special case: src is a struct (PTR or TAGGED call result) flowing into
                // a native-typed phi. This arises from bytecode register reuse where the
                // same slot carries different-typed values on different paths. When the
                // phi dst is unused (dead), emit a zero literal instead of a bogus unbox.
                uint32_t phi_vi = XIR_REF_INDEX(phi->dst);
                bool phi_dst_dead =
                    (src_type == XR_REP_PTR) ||
                    (src_type == XR_REP_TAGGED && !xcg_vreg_has_ins_use(func, phi_vi));
                if (phi_dst_dead) {
                    if (dst_type == XR_REP_F64)
                        xcgen_buf_puts(b, "0.0 /* dead-phi: struct!=float */");
                    else
                        xcgen_buf_puts(b, "INT64_C(0) /* dead-phi: struct!=int */");
                } else if (dst_type == XR_REP_I64) {
                    xcgen_buf_puts(b, "XR_TO_INT(");
                    xcg_emit_ref(b, func, src);
                    xcgen_buf_puts(b, ")");
                } else if (dst_type == XR_REP_F64) {
                    xcgen_buf_puts(b, "XR_TO_FLOAT(");
                    xcg_emit_ref(b, func, src);
                    xcgen_buf_puts(b, ")");
                } else {
                    xcg_emit_ref(b, func, src);
                }
            } else {
                xcg_emit_ref(b, func, src);
            }
            xcgen_buf_puts(b, ";\n");
        }
    }
}

/* ========== Block Terminators ========== */

void xcg_emit_terminator(XcgenBuf *b, XirFunc *func, XirBlock *blk, const char *self_name,
                         XcgenFunc *cf) {
    XR_DCHECK(b != NULL, "xcg_emit_terminator: NULL buf");
    XR_DCHECK(func != NULL, "xcg_emit_terminator: NULL func");
    XR_DCHECK(blk != NULL, "xcg_emit_terminator: NULL blk");
    (void) self_name;
    switch (blk->jmp.type) {
        case XIR_JMP_JMP:
            if (blk->s1) {
                if (block_has_phis(blk->s1))
                    xcg_emit_phi_copies_for_edge(b, func, blk, blk->s1);
                xcgen_buf_printf(b, "    goto L%u;\n", blk->s1->id);
            }
            break;

        case XIR_JMP_BR: {
            uint8_t br_type = xcg_ref_type(func, blk->jmp.arg);
            bool br_tagged =
                (br_type == XR_REP_STR || br_type == XR_REP_PTR || br_type == XR_REP_TAGGED);
            xcgen_buf_puts(b, "    if (");
            if (br_tagged) {
                // XrValue → truthy: matches VM semantics
                // (null, false, 0, 0.0 are falsy; everything else is truthy)
                xcgen_buf_puts(b, "xr_truthy(");
                xcg_emit_ref(b, func, blk->jmp.arg);
                xcgen_buf_puts(b, ")");
            } else {
                xcg_emit_ref(b, func, blk->jmp.arg);
            }
            xcgen_buf_puts(b, ") {\n");
            if (blk->s1 && block_has_phis(blk->s1))
                xcg_emit_phi_copies_for_edge(b, func, blk, blk->s1);
            if (blk->s1)
                xcgen_buf_printf(b, "        goto L%u;\n", blk->s1->id);
            xcgen_buf_puts(b, "    } else {\n");
            if (blk->s2 && block_has_phis(blk->s2))
                xcg_emit_phi_copies_for_edge(b, func, blk, blk->s2);
            if (blk->s2)
                xcgen_buf_printf(b, "        goto L%u;\n", blk->s2->id);
            xcgen_buf_puts(b, "    }\n");
            break;
        }

        case XIR_JMP_RET: {
            // Defer cleanup: call deferred closures in LIFO order before return
            if (cf && cf->defer_count > 0) {
                for (int di = cf->defer_count - 1; di >= 0; di--) {
                    int nargs = func->defer_entries[di].arg_count;
                    xcgen_buf_printf(
                        b, "    if (_defer_%d_set) ((void (*)(XrtContext, xrt_closure_t*", di);
                    for (int ai = 0; ai < nargs; ai++)
                        xcgen_buf_puts(b, ", XrValue");
                    xcgen_buf_printf(b,
                                     "))((xrt_closure_t*)_defer_%d.ptr)->fn)"
                                     "(xrt_ctx, (xrt_closure_t*)_defer_%d.ptr",
                                     di, di);
                    for (int ai = 0; ai < nargs; ai++)
                        xcgen_buf_printf(b, ", _defer_%d_arg%d", di, ai);
                    xcgen_buf_puts(b, ");\n");
                }
            }

            // void_return functions always return null; emit bare return
            if (cf && cf->void_return) {
                xcgen_buf_puts(b, "    return;\n");
                break;
            }

            uint8_t val_type = xcg_ref_type(func, blk->jmp.arg);
            uint8_t ret_type = (func->proto && func->proto->return_type_info)
                                   ? xr_type_rep(func->proto->return_type_info)
                                   : XR_REP_TAGGED;
            bool val_tagged =
                (val_type == XR_REP_STR || val_type == XR_REP_PTR || val_type == XR_REP_TAGGED);
            bool ret_tagged =
                (ret_type == XR_REP_STR || ret_type == XR_REP_PTR || ret_type == XR_REP_TAGGED);

            if (!ret_tagged && val_tagged) {
                // Auto-unbox: XrValue → int64_t/double
                if (ret_type == XR_REP_I64) {
                    xcgen_buf_puts(b, "    return XR_TO_INT(");
                    xcg_emit_ref(b, func, blk->jmp.arg);
                    xcgen_buf_puts(b, ");\n");
                    if (cf)
                        cf->needs_runtime = true;
                } else if (ret_type == XR_REP_F64) {
                    xcgen_buf_puts(b, "    return XR_TO_FLOAT(");
                    xcg_emit_ref(b, func, blk->jmp.arg);
                    xcgen_buf_puts(b, ");\n");
                    if (cf)
                        cf->needs_runtime = true;
                } else {
                    xcgen_buf_puts(b, "    return ");
                    xcg_emit_ref(b, func, blk->jmp.arg);
                    xcgen_buf_puts(b, ";\n");
                }
            } else if (ret_tagged && !val_tagged) {
                // Auto-box: int64_t/double → XrValue
                if (val_type == XR_REP_I64) {
                    xcgen_buf_puts(b, "    return XR_FROM_INT(");
                    xcg_emit_ref(b, func, blk->jmp.arg);
                    xcgen_buf_puts(b, ");\n");
                    if (cf)
                        cf->needs_runtime = true;
                } else if (val_type == XR_REP_F64) {
                    xcgen_buf_puts(b, "    return XR_FROM_FLOAT(");
                    xcg_emit_ref(b, func, blk->jmp.arg);
                    xcgen_buf_puts(b, ");\n");
                    if (cf)
                        cf->needs_runtime = true;
                } else {
                    xcgen_buf_puts(b, "    return ");
                    xcg_emit_ref(b, func, blk->jmp.arg);
                    xcgen_buf_puts(b, ";\n");
                }
            } else {
                xcgen_buf_puts(b, "    return ");
                xcg_emit_ref(b, func, blk->jmp.arg);
                xcgen_buf_puts(b, ";\n");
            }
            break;
        }

        case XIR_JMP_UNREACHABLE:
            xcgen_buf_puts(b, "    __builtin_unreachable();\n");
            break;

        default:
            xcgen_buf_puts(b, "    /* no terminator */\n");
            break;
    }
}
