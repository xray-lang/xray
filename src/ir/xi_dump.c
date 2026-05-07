/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_dump.c - Human-readable text dump for the typed SSA IR
 *
 * Output format (inspired by Go SSA):
 *   func add(v0: int, v1: int) -> int {
 *     b0:                          ; entry
 *       v2 = ADD v0 v1             ; int
 *       RETURN v2
 *   }
 */

#include "xi.h"
#include "xi_op_name.h"
#include "../runtime/value/xtype.h"  /* XrTypeKind, XR_KIND_* */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/* ========== Type Name Helper ========== */

static const char *xi_type_name(const struct XrType *type) {
    if (!type) return "?";
    switch (type->kind) {
        case XR_KIND_INT:      return "int";
        case XR_KIND_FLOAT:    return "float";
        case XR_KIND_BOOL:     return "bool";
        case XR_KIND_STRING:   return "string";
        case XR_KIND_NULL:     return "null";
        case XR_KIND_UNKNOWN:  return "any";
        case XR_KIND_VOID:     return "void";
        case XR_KIND_ARRAY:    return "array";
        case XR_KIND_MAP:      return "map";
        case XR_KIND_FUNCTION: return "fn";
        case XR_KIND_INSTANCE: return "instance";
        case XR_KIND_CLASS:    return "class";
        case XR_KIND_ENUM:     return "enum";
        case XR_KIND_JSON:     return "json";
        case XR_KIND_UNION:    return "union";
        case XR_KIND_TUPLE:    return "tuple";
        case XR_KIND_NEVER:    return "never";
        default:               return "?";
    }
}

/* ========== Block Kind Name ========== */

static const char *xi_block_kind_name(uint16_t kind) {
    switch (kind) {
        case XI_BLOCK_PLAIN:       return "plain";
        case XI_BLOCK_IF:          return "if";
        case XI_BLOCK_RETURN:      return "return";
        case XI_BLOCK_UNREACHABLE: return "unreachable";
        default:                   return "?";
    }
}

/* ========== Dump a Single Value ========== */

static void dump_value(FILE *out, const XiValue *v) {
    if (!v) return;

    /* v42 = ADD v3 v5 */
    fprintf(out, "    v%u = %s", v->id, xi_op_name(v->op));

    /* Special handling for constants */
    if (v->op == XI_CONST) {
        if (v->type && v->type->kind == XR_KIND_INT) {
            fprintf(out, " %" PRId64, v->aux_int);
        } else if (v->type && v->type->kind == XR_KIND_FLOAT) {
            double fval;
            memcpy(&fval, &v->aux_int, sizeof(double));
            fprintf(out, " %g", fval);
        } else if (v->type && v->type->kind == XR_KIND_BOOL) {
            fprintf(out, " %s", v->aux_int ? "true" : "false");
        } else if (v->type && v->type->kind == XR_KIND_NULL) {
            fprintf(out, " null");
        } else if (v->type && v->type->kind == XR_KIND_STRING) {
            fprintf(out, " \"%s\"", v->aux ? (const char *) v->aux : "");
        }
    } else if (v->op == XI_PARAM) {
        fprintf(out, " #%" PRId64, v->aux_int);
    } else {
        /* Print args */
        for (uint16_t i = 0; i < v->nargs; i++) {
            if (v->args[i])
                fprintf(out, " v%u", v->args[i]->id);
            else
                fprintf(out, " <nil>");
        }
    }

    /* Auxiliary info for specific ops */
    if ((v->op == XI_LOAD_FIELD || v->op == XI_STORE_FIELD) && v->aux) {
        fprintf(out, " .%s", (const char *) v->aux);
    } else if (v->op == XI_CALL_METHOD || v->op == XI_CALL_BUILTIN ||
               v->op == XI_LOAD_UPVAL || v->op == XI_STORE_UPVAL ||
               v->op == XI_GET_SHARED || v->op == XI_SET_SHARED) {
        fprintf(out, " [aux=%" PRId64 "]", v->aux_int);
    }

    /* Type + rep annotation */
    fprintf(out, "  ; %s", xi_type_name(v->type));

    /* Show explicit rep when it differs from TAGGED (i.e. after select_rep) */
    if (v->rep == XR_REP_I64) fprintf(out, " :i64");
    else if (v->rep == XR_REP_F64) fprintf(out, " :f64");

    /* Show escape level when set (after escape analysis) */
    if (v->escape == 1) fprintf(out, " esc:arg");
    else if (v->escape == 2) fprintf(out, " esc:heap");
    else if (v->escape == 3) fprintf(out, " esc:global");

    if (v->line > 0)
        fprintf(out, " L%u", v->line);

    fprintf(out, "\n");
}

/* ========== Dump a Phi Node ========== */

static void dump_phi(FILE *out, const XiPhi *phi) {
    if (!phi) return;
    const XiValue *v = &phi->value;
    fprintf(out, "    v%u = PHI", v->id);
    for (uint16_t i = 0; i < v->nargs; i++) {
        if (v->args[i])
            fprintf(out, " [b%u:v%u]", v->block->preds[i]->id, v->args[i]->id);
        else
            fprintf(out, " [b%u:<nil>]",
                    (i < v->block->npreds) ? v->block->preds[i]->id : 0);
    }
    fprintf(out, "  ; %s\n", xi_type_name(v->type));
}

/* ========== Dump a Block ========== */

static void dump_block(FILE *out, const XiBlock *blk) {
    if (!blk) return;

    fprintf(out, "  b%u:", blk->id);

    /* Predecessor list */
    if (blk->npreds > 0) {
        fprintf(out, "  ; preds:");
        for (uint16_t i = 0; i < blk->npreds; i++)
            fprintf(out, " b%u", blk->preds[i]->id);
    }
    fprintf(out, "\n");

    /* Phi nodes */
    for (XiPhi *phi = blk->phis; phi; phi = phi->next)
        dump_phi(out, phi);

    /* Instructions */
    for (uint32_t i = 0; i < blk->nvalues; i++)
        dump_value(out, blk->values[i]);

    /* Terminator */
    switch (blk->kind) {
        case XI_BLOCK_PLAIN:
            if (blk->succs[0])
                fprintf(out, "    JMP b%u\n", blk->succs[0]->id);
            break;
        case XI_BLOCK_IF:
            fprintf(out, "    IF v%u -> b%u b%u\n",
                    blk->control ? blk->control->id : 0,
                    blk->succs[0] ? blk->succs[0]->id : 0,
                    blk->succs[1] ? blk->succs[1]->id : 0);
            break;
        case XI_BLOCK_RETURN:
            if (blk->control)
                fprintf(out, "    RET v%u\n", blk->control->id);
            else
                fprintf(out, "    RET\n");
            break;
        case XI_BLOCK_UNREACHABLE:
            fprintf(out, "    UNREACHABLE\n");
            break;
    }
}

/* ========== Dump a Function ========== */

void xi_func_dump(const XiFunc *f, void *stream) {
    if (!f) return;
    FILE *out = stream ? (FILE *) stream : stdout;

    /* Header (includes stage for diagnostics) */
    fprintf(out, "func %s [%s](", f->name ? f->name : "<anonymous>",
            xi_stage_name(f->stage));
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (i > 0) fprintf(out, ", ");
        if (f->params[i])
            fprintf(out, "v%u: %s", f->params[i]->id,
                    xi_type_name(f->params[i]->type));
        else
            fprintf(out, "?: ?");
    }
    fprintf(out, ") -> %s {\n", xi_type_name(f->return_type));

    /* Blocks */
    for (uint32_t i = 0; i < f->nblocks; i++)
        dump_block(out, f->blocks[i]);

    fprintf(out, "}\n");

    /* Nested functions */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        fprintf(out, "\n");
        xi_func_dump(f->children[i], stream);
    }
}
