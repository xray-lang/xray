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
#include "../runtime/value/xtype.h"  /* XrTypeKind, XR_KIND_* */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/* ========== Op Name Table ========== */

static const char *xi_op_name(uint16_t op) {
    switch (op) {
        case XI_CONST:       return "CONST";
        case XI_PARAM:       return "PARAM";
        case XI_ADD:         return "ADD";
        case XI_SUB:         return "SUB";
        case XI_MUL:         return "MUL";
        case XI_DIV:         return "DIV";
        case XI_MOD:         return "MOD";
        case XI_NEG:         return "NEG";
        case XI_BAND:        return "BAND";
        case XI_BOR:         return "BOR";
        case XI_BXOR:        return "BXOR";
        case XI_BNOT:        return "BNOT";
        case XI_SHL:         return "SHL";
        case XI_SHR:         return "SHR";
        case XI_EQ:          return "EQ";
        case XI_NE:          return "NE";
        case XI_LT:          return "LT";
        case XI_LE:          return "LE";
        case XI_GT:          return "GT";
        case XI_GE:          return "GE";
        case XI_EQ_STRICT:   return "EQ_STRICT";
        case XI_NE_STRICT:   return "NE_STRICT";
        case XI_NOT:         return "NOT";
        case XI_CONVERT:     return "CONVERT";
        case XI_BOX:         return "BOX";
        case XI_UNBOX:       return "UNBOX";
        case XI_LOAD_FIELD:  return "LOAD_FIELD";
        case XI_STORE_FIELD: return "STORE_FIELD";
        case XI_INDEX_GET:   return "INDEX_GET";
        case XI_INDEX_SET:   return "INDEX_SET";
        case XI_JSON_NEW:    return "JSON_NEW";
        case XI_JSON_INIT_F: return "JSON_INIT_F";
        case XI_JSON_GET_F:  return "JSON_GET_F";
        case XI_JSON_SET_F:  return "JSON_SET_F";
        case XI_JSON_DECODE: return "JSON_DECODE";
        case XI_ARRAY_NEW:   return "ARRAY_NEW";
        case XI_MAP_NEW:     return "MAP_NEW";
        case XI_CALL:        return "CALL";
        case XI_CALL_METHOD: return "CALL_METHOD";
        case XI_CALL_BUILTIN:return "CALL_BUILTIN";
        case XI_EXTRACT:     return "EXTRACT";
        case XI_CLOSURE_NEW: return "CLOSURE_NEW";
        case XI_LOAD_UPVAL:  return "LOAD_UPVAL";
        case XI_STORE_UPVAL: return "STORE_UPVAL";
        case XI_GET_SHARED:  return "GET_SHARED";
        case XI_SET_SHARED:  return "SET_SHARED";
        case XI_PRINT:       return "PRINT";
        case XI_GO:          return "GO";
        case XI_AWAIT:       return "AWAIT";
        case XI_CHAN_SEND:       return "CHAN_SEND";
        case XI_CHAN_RECV:       return "CHAN_RECV";
        case XI_CHAN_TRY_SEND:   return "CHAN_TRY_SEND";
        case XI_CHAN_TRY_RECV:   return "CHAN_TRY_RECV";
        case XI_YIELD:       return "YIELD";
        case XI_THROW:       return "THROW";
        case XI_ITER_NEW:    return "ITER_NEW";
        case XI_ITER_NEXT:   return "ITER_NEXT";
        case XI_ITER_VALID:  return "ITER_VALID";
        case XI_DEFER:       return "DEFER";
        case XI_CHAN_NEW:    return "CHAN_NEW";
        case XI_SET_NEW:     return "SET_NEW";
        case XI_STR_CONCAT:  return "STR_CONCAT";
        case XI_IS:          return "IS";
        case XI_AS:          return "AS";
        case XI_SLICE:       return "SLICE";
        case XI_RANGE:       return "RANGE";
        case XI_MULTI_RET:   return "MULTI_RET";
        case XI_ISNULL:      return "ISNULL";
        case XI_PHI:         return "PHI";
        case XI_COPY:        return "COPY";
        case XI_CLASS_CREATE:return "CLASS_CREATE";
        case XI_SCOPE_ENTER: return "SCOPE_ENTER";
        case XI_SCOPE_EXIT:  return "SCOPE_EXIT";
        case XI_TRY:         return "TRY";
        case XI_CATCH:       return "CATCH";
        case XI_FINALLY:     return "FINALLY";
        case XI_END_TRY:     return "END_TRY";
        case XI_ASSERT:      return "ASSERT";
        case XI_ASSERT_EQ:   return "ASSERT_EQ";
        case XI_ASSERT_NE:   return "ASSERT_NE";
        case XI_ASSERT_THROWS: return "ASSERT_THROWS";
        case XI_TYPEOF:      return "TYPEOF";
        case XI_GET_BUILTIN: return "GET_BUILTIN";
        case XI_IMPORT_REF:  return "IMPORT_REF";
        case XI_REGEX_COMPILE: return "REGEX_COMPILE";
        default:             return "???";
    }
}

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

    /* Type annotation */
    fprintf(out, "  ; %s", xi_type_name(v->type));

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
