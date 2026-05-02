/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_printer.c - Xm text dump for debugging
 *
 * KEY CONCEPT:
 *   Human-readable text representation of Xm for debugging and
 *   development. Output format inspired by QBE's textual IR.
 *
 * Example output:
 *   function $fib {
 *     @entry:
 *       v0 =i64 const.i64 #0
 *       v1 =i64 const.i64 #1
 *       jmp @loop
 *     @loop:
 *       v2 =i64 phi @entry:v0, @body:v5
 *       v3 =i64 lt v2, v10
 *       br v3, @body, @exit
 *   }
 */

#include "xm_printer.h"
#include "../base/xchecks.h"
#include <inttypes.h>

static const char *type_names[] = {
    [XR_REP_I64] = "i64",    [XR_REP_F64] = "f64",   [XR_REP_PTR] = "ptr",
    [XR_REP_TAGGED] = "val", [XR_REP_VOID] = "void",
};

static const char *type_name(uint8_t type) {
    if (type <= XR_REP_VOID)
        return type_names[type];
    return "???";
}

static const char *vtag_name(uint8_t vtag) {
    switch (vtag) {
        case VTAG_TAGGED:
            return NULL;  // default, don't show
        case VTAG_I64:
            return "i64";
        case VTAG_F64:
            return "f64";
        case VTAG_PTR:
            return "ptr";
        case VTAG_BOOL:
            return "bool";
        case VTAG_NUMERIC:
            return "numeric";
        case VTAG_NULL:
            return "null";
        default:
            return NULL;
    }
}

void xm_print_ref(FILE *out, XmFunc *func, XmRef ref) {
    XR_DCHECK(out != NULL, "xm_print_ref: NULL out");
    switch (XM_REF_KIND(ref)) {
        case XM_REF_VREG: {
            uint32_t vi = XM_REF_INDEX(ref);
            fprintf(out, "v%u", vi);
            // Show ctype when it carries extra info beyond the machine rep
            if (func && vi < func->nvreg) {
                XmType ct = xm_ref_ctype(func, ref);
                uint8_t tag = type_kind_to_vtag(ct.kind);
                uint8_t mtype = func->vregs[vi].rep;
                bool show = (tag != VTAG_TAGGED) || (tag == VTAG_TAGGED && mtype != XR_REP_TAGGED);
                if (show) {
                    const char *tn = vtag_name(tag);
                    if (tn)
                        fprintf(out, "<%s>", tn);
                    else
                        fprintf(out, "<vtag=%u>", tag);
                }
            }
            break;
        }
        case XM_REF_CONST: {
            uint32_t idx = XM_REF_INDEX(ref);
            if (func && idx < func->nconst) {
                XmConst *c = &func->consts[idx];
                if (c->rep == XR_REP_I64) {
                    fprintf(out, "#%" PRId64, c->val.i64);
                } else if (c->rep == XR_REP_F64) {
                    fprintf(out, "#%g", c->val.f64);
                } else {
                    fprintf(out, "#%p", c->val.ptr);
                }
            } else {
                fprintf(out, "c%u", idx);
            }
            break;
        }
        case XM_REF_SLOT:
            fprintf(out, "s%u", XM_REF_INDEX(ref));
            break;
        case XM_REF_BLOCK:
            fprintf(out, "@b%u", XM_REF_INDEX(ref));
            break;
        case XM_REF_NONE:
            fprintf(out, "_");
            break;
        default:
            fprintf(out, "?ref");
            break;
    }
}

void xm_print_ins(FILE *out, XmFunc *func, XmIns *ins) {
    XR_DCHECK(out != NULL, "xm_print_ins: NULL out");
    XR_DCHECK(func != NULL, "xm_print_ins: NULL func");
    XR_DCHECK(ins != NULL, "xm_print_ins: NULL ins");
    fprintf(out, "    ");

    // Print destination
    if (!xm_ref_is_none(ins->dst)) {
        xm_print_ref(out, func, ins->dst);
        fprintf(out, " =%s ", type_name(ins->rep));
    }

    // Print opcode
    fprintf(out, "%s", xm_op_name(ins->op));

    // Print arguments
    if (!xm_ref_is_none(ins->args[0])) {
        fprintf(out, " ");
        xm_print_ref(out, func, ins->args[0]);
    }
    if (!xm_ref_is_none(ins->args[1])) {
        fprintf(out, ", ");
        xm_print_ref(out, func, ins->args[1]);
    }

    // Print flags
    if (ins->flags) {
        fprintf(out, "  ;");
        if (ins->flags & XM_FLAG_SAFEPOINT)
            fprintf(out, " safepoint");
        if (ins->flags & XM_FLAG_MAY_THROW)
            fprintf(out, " may_throw");
        if (ins->flags & XM_FLAG_SIDE_EFFECT)
            fprintf(out, " side_effect");
    }

    fprintf(out, "\n");
}

static void print_block_label(FILE *out, XmBlock *blk) {
    if (blk->label) {
        fprintf(out, "@%s", blk->label);
    } else {
        fprintf(out, "@b%u", blk->id);
    }
}

void xm_print_block(FILE *out, XmFunc *func, XmBlock *blk) {
    XR_DCHECK(out != NULL, "xm_print_block: NULL out");
    XR_DCHECK(func != NULL, "xm_print_block: NULL func");
    XR_DCHECK(blk != NULL, "xm_print_block: NULL blk");
    // Block header
    fprintf(out, "  ");
    print_block_label(out, blk);
    fprintf(out, ":");

    // Predecessors
    if (blk->npred > 0) {
        fprintf(out, "  ; preds:");
        for (uint32_t i = 0; i < blk->npred; i++) {
            fprintf(out, " ");
            print_block_label(out, blk->preds[i]);
        }
    }
    fprintf(out, "\n");

    // Phi nodes
    for (XmPhi *phi = blk->phis; phi; phi = phi->next) {
        fprintf(out, "    ");
        xm_print_ref(out, func, phi->dst);
        fprintf(out, " =%s phi", type_name(phi->rep));
        for (uint16_t i = 0; i < phi->narg; i++) {
            if (i > 0)
                fprintf(out, ",");
            fprintf(out, " ");
            if (i < blk->npred) {
                print_block_label(out, blk->preds[i]);
                fprintf(out, ":");
            }
            xm_print_ref(out, func, phi->args[i]);
        }
        fprintf(out, "\n");
    }

    // Instructions
    for (uint32_t i = 0; i < blk->nins; i++) {
        xm_print_ins(out, func, &blk->ins[i]);
    }

    // Terminator
    fprintf(out, "    ");
    switch (blk->jmp.type) {
        case XM_JMP_JMP:
            fprintf(out, "jmp ");
            if (blk->s1)
                print_block_label(out, blk->s1);
            break;
        case XM_JMP_BR:
            fprintf(out, "br ");
            xm_print_ref(out, func, blk->jmp.arg);
            fprintf(out, ", ");
            if (blk->s1)
                print_block_label(out, blk->s1);
            fprintf(out, ", ");
            if (blk->s2)
                print_block_label(out, blk->s2);
            break;
        case XM_JMP_RET:
            fprintf(out, "ret ");
            xm_print_ref(out, func, blk->jmp.arg);
            break;
        case XM_JMP_UNREACHABLE:
            fprintf(out, "unreachable");
            break;
        default:
            fprintf(out, "; <no terminator>");
            break;
    }
    fprintf(out, "\n");
}

void xm_print_func(FILE *out, XmFunc *func) {
    XR_DCHECK(out != NULL, "xm_print_func: NULL out");
    XR_DCHECK(func != NULL, "xm_print_func: NULL func");
    fprintf(out, "function $%s {\n", func->name ? func->name : "<anon>");

    for (uint32_t i = 0; i < func->nblk; i++) {
        xm_print_block(out, func, func->blocks[i]);
        if (i + 1 < func->nblk)
            fprintf(out, "\n");
    }

    fprintf(out, "}\n");

    // Stats
    fprintf(out, "; %u blocks, %u vregs, %u constants\n", func->nblk, func->nvreg, func->nconst);
}
