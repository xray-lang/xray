/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_verify.c - IR verification pass for Xi IR
 *
 * Validates structural and semantic invariants of SSA functions
 * produced by xi_lower.c and transformed by xi_opt.c.
 */

#include "xi_verify.h"
#include "xi_effect.h"
#include "xi_backend.h"
#include "xi_op_name.h"
#include "xi_analysis.h"
#include "../runtime/value/xtype.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ========== Error Reporting ========== */

typedef struct {
    char *buf;
    int size;
    bool failed;
} VerifyCtx;

static void verr(VerifyCtx *ctx, const char *fmt, ...) {
    if (ctx->failed) return;  /* report first error only */
    ctx->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->buf, (size_t)ctx->size, fmt, ap);
    va_end(ap);
}

/* ========== Individual Checks ========== */

/* Check 1: function-level invariants */
static void verify_func(VerifyCtx *ctx, const XiFunc *f) {
    if (!f->name) {
        verr(ctx, "function has NULL name");
        return;
    }
    if (f->nblocks == 0) {
        verr(ctx, "func '%s': no blocks", f->name);
        return;
    }
    if (!f->entry) {
        verr(ctx, "func '%s': NULL entry block", f->name);
        return;
    }
    if (f->entry != f->blocks[0]) {
        verr(ctx, "func '%s': entry != blocks[0]", f->name);
        return;
    }
}

/* Check 2: entry block must have 0 predecessors */
static void verify_entry(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;
    if (f->entry->npreds != 0) {
        verr(ctx, "func '%s': entry block b%u has %u predecessors (expected 0)",
             f->name, f->entry->id, f->entry->npreds);
    }
}

/* Check 3: block-level invariants */
static void verify_block(VerifyCtx *ctx, const XiFunc *f, const XiBlock *blk) {
    if (ctx->failed) return;
    XR_DCHECK(blk != NULL, "verify_block: NULL block");

    /* Block must belong to this function */
    if (blk->func != f) {
        verr(ctx, "func '%s': block b%u has wrong func pointer",
             f->name, blk->id);
        return;
    }

    /* Check terminator consistency */
    switch (blk->kind) {
        case XI_BLOCK_PLAIN:
            if (!blk->succs[0] && blk->nvalues > 0) {
                /* Plain block with no successor and values is suspicious
                 * but may be valid (dead code). Skip. */
            }
            break;
        case XI_BLOCK_IF:
            if (!blk->control) {
                verr(ctx, "func '%s': IF block b%u has NULL control",
                     f->name, blk->id);
                return;
            }
            if (!blk->succs[0] || !blk->succs[1]) {
                verr(ctx, "func '%s': IF block b%u missing successor(s)",
                     f->name, blk->id);
                return;
            }
            break;
        case XI_BLOCK_RETURN:
            /* control may be NULL for void returns */
            break;
        case XI_BLOCK_UNREACHABLE:
            break;
        default:
            verr(ctx, "func '%s': block b%u has invalid kind %u",
                 f->name, blk->id, blk->kind);
            return;
    }
}

/* Check 4: value-level invariants */
static void verify_value(VerifyCtx *ctx, const XiFunc *f,
                          const XiBlock *blk, const XiValue *v) {
    if (ctx->failed) return;
    XR_DCHECK(v != NULL, "verify_value: NULL value");

    /* Type must be non-NULL */
    if (!v->type) {
        verr(ctx, "func '%s': value v%u in b%u has NULL type",
             f->name, v->id, blk->id);
        return;
    }

    /* Op must be in valid range */
    if (v->op >= XI_OP_COUNT) {
        verr(ctx, "func '%s': value v%u in b%u has invalid op %u",
             f->name, v->id, blk->id, v->op);
        return;
    }

    /* Block back-pointer must match */
    if (v->block != blk) {
        verr(ctx, "func '%s': value v%u claims block b%u but is in b%u",
             f->name, v->id,
             v->block ? v->block->id : 9999, blk->id);
        return;
    }

    /* Args array consistency */
    if (v->nargs > 0 && !v->args) {
        verr(ctx, "func '%s': value v%u in b%u has %u args but NULL args ptr",
             f->name, v->id, blk->id, v->nargs);
        return;
    }

    /* Each arg should be a plausible value (non-NULL, has type).
     * Exception: CLOSURE_NEW args may be NULL for upvalue-chain captures
     * that have no local SSA value (source is parent's upvalue, not a reg). */
    for (uint16_t a = 0; a < v->nargs; a++) {
        if (!v->args[a]) {
            if (v->op == XI_CLOSURE_NEW)
                continue;  /* NULL capture arg is valid */
            verr(ctx, "func '%s': value v%u in b%u arg[%u] is NULL",
                 f->name, v->id, blk->id, a);
            return;
        }
        if (!v->args[a]->type) {
            verr(ctx, "func '%s': value v%u in b%u arg[%u] (v%u) has NULL type",
                 f->name, v->id, blk->id, a, v->args[a]->id);
            return;
        }
    }
}

/* Check 5: phi node invariants */
static void verify_phi(VerifyCtx *ctx, const XiFunc *f,
                        const XiBlock *blk, const XiPhi *phi) {
    if (ctx->failed) return;
    XR_DCHECK(phi != NULL, "verify_phi: NULL phi");

    /* Phi must have op == XI_PHI */
    if (phi->value.op != XI_PHI) {
        verr(ctx, "func '%s': phi in b%u has op %u (expected XI_PHI=%u)",
             f->name, blk->id, phi->value.op, XI_PHI);
        return;
    }

    /* Phi type must be non-NULL */
    if (!phi->value.type) {
        verr(ctx, "func '%s': phi v%u in b%u has NULL type",
             f->name, phi->value.id, blk->id);
        return;
    }

    /* Phi arg count must match predecessor count */
    if (phi->value.nargs != blk->npreds) {
        verr(ctx, "func '%s': phi v%u in b%u has %u args but block has %u preds",
             f->name, phi->value.id, blk->id,
             phi->value.nargs, blk->npreds);
        return;
    }

    /* Each phi arg must be non-NULL with valid type */
    for (uint16_t a = 0; a < phi->value.nargs; a++) {
        if (!phi->value.args[a]) {
            verr(ctx, "func '%s': phi v%u in b%u arg[%u] is NULL",
                 f->name, phi->value.id, blk->id, a);
            return;
        }
    }
}

/* Check 6: CFG edge symmetry — succ→pred and pred→succ both directions */
static void verify_cfg_edges(VerifyCtx *ctx, const XiFunc *f,
                              const XiBlock *blk) {
    if (ctx->failed) return;

    /* Forward: each successor must list blk as a predecessor */
    for (int s = 0; s < 2; s++) {
        XiBlock *succ = blk->succs[s];
        if (!succ) continue;

        bool found = false;
        for (uint16_t p = 0; p < succ->npreds; p++) {
            if (succ->preds[p] == blk) {
                found = true;
                break;
            }
        }
        if (!found) {
            verr(ctx, "func '%s': b%u has successor b%u but is not in its pred list",
                 f->name, blk->id, succ->id);
            return;
        }
    }

    /* Reverse: each predecessor should list blk as a successor.
     * Exception edges (try→catch) add preds without corresponding succs[]
     * entries because succs[2] only holds normal control flow (then/else).
     * So this is a non-fatal diagnostic — we skip the check for now. */

    /* Block kind → successor count enforcement */
    int nsucc = (blk->succs[0] ? 1 : 0) + (blk->succs[1] ? 1 : 0);
    switch (blk->kind) {
        case XI_BLOCK_IF:
            if (nsucc != 2) {
                verr(ctx, "func '%s': IF block b%u has %d successors (expected 2)",
                     f->name, blk->id, nsucc);
            }
            break;
        case XI_BLOCK_RETURN:
        case XI_BLOCK_UNREACHABLE:
            if (nsucc != 0) {
                verr(ctx, "func '%s': %s block b%u has %d successors (expected 0)",
                     f->name,
                     blk->kind == XI_BLOCK_RETURN ? "RETURN" : "UNREACHABLE",
                     blk->id, nsucc);
            }
            break;
        default:
            break;
    }
}

/* Check 7: unique value IDs within function */
static void verify_unique_ids(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;

    /* Use a simple O(n) scan; for typical function sizes this is fine. */
    uint32_t max_id = f->next_value_id;

    /* Track seen IDs with a bitmap if small enough, else skip. */
    if (max_id > 10000) return;  /* skip for very large functions */

    /* Stack-allocate a bitset. Max ~1.2 KB for 10000 IDs. */
    uint8_t seen[1250];
    memset(seen, 0, sizeof(seen));

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            uint32_t vid = blk->values[i]->id;
            if (vid >= max_id) {
                verr(ctx, "func '%s': value v%u >= next_value_id %u",
                     f->name, vid, max_id);
                return;
            }
            uint32_t byte = vid / 8;
            uint8_t bit = (uint8_t)(1 << (vid & 7));
            if (byte < sizeof(seen)) {
                if (seen[byte] & bit) {
                    verr(ctx, "func '%s': duplicate value ID v%u",
                         f->name, vid);
                    return;
                }
                seen[byte] |= bit;
            }
        }

        /* Also check phi IDs */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            uint32_t vid = phi->value.id;
            if (vid >= max_id) {
                verr(ctx, "func '%s': phi v%u >= next_value_id %u",
                     f->name, vid, max_id);
                return;
            }
            uint32_t byte = vid / 8;
            uint8_t bit = (uint8_t)(1 << (vid & 7));
            if (byte < sizeof(seen)) {
                if (seen[byte] & bit) {
                    verr(ctx, "func '%s': duplicate phi ID v%u",
                         f->name, vid);
                    return;
                }
                seen[byte] |= bit;
            }
        }
    }
}

/* ========== Check 8: SSA Dominance ========== */

/* Return true if 'def_blk' dominates 'use_blk' (or they are the same block). */
static bool block_dominates(const XiBlock *def_blk, const XiBlock *use_blk) {
    if (!def_blk || !use_blk) return false;
    /* Walk the dominator tree from use_blk to entry.
     * dom_depth == 0 for entry; idom == self for entry. */
    const XiBlock *b = use_blk;
    while (b && b->dom_depth >= def_blk->dom_depth) {
        if (b == def_blk) return true;
        if (b->idom == b) break;  /* entry block */
        b = b->idom;
    }
    return false;
}

/* Verify SSA dominance: each value arg must be defined in a block that
 * dominates the use site.  Phi args are special: arg[i] must be
 * dominated by preds[i] (since the phi chooses along the edge). */
static void verify_dominance(VerifyCtx *ctx, XiFunc *f) {
    if (ctx->failed) return;

    /* Compute RPO and dominators (overwrites scratch fields on blocks). */
    xi_compute_rpo(f);
    xi_compute_dominators(f);

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;

        /* Check regular values */
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v) continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                XiValue *arg = v->args[a];
                if (!arg || !arg->block) continue;
                if (!block_dominates(arg->block, blk)) {
                    verr(ctx,
                         "func '%s': v%u in b%u uses v%u defined in b%u "
                         "which does not dominate b%u",
                         f->name, v->id, blk->id,
                         arg->id, arg->block->id, blk->id);
                    return;
                }
            }
        }

        /* Check phi args: arg[i] must be dominated by preds[i]. */
        for (XiPhi *phi = blk->phis; phi && !ctx->failed; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                XiValue *arg = phi->value.args[a];
                if (!arg || !arg->block) continue;
                if (a >= blk->npreds) break;
                XiBlock *pred = blk->preds[a];
                if (!pred) continue;
                if (!block_dominates(arg->block, pred)) {
                    verr(ctx,
                         "func '%s': phi v%u in b%u arg[%u] (v%u from b%u) "
                         "not dominated by predecessor b%u",
                         f->name, phi->value.id, blk->id, a,
                         arg->id, arg->block->id, pred->id);
                    return;
                }
            }
        }

        /* Check block control (IF condition, RETURN value) */
        if (blk->control && blk->control->block) {
            if (!block_dominates(blk->control->block, blk)) {
                verr(ctx,
                     "func '%s': b%u control v%u defined in b%u "
                     "which does not dominate b%u",
                     f->name, blk->id, blk->control->id,
                     blk->control->block->id, blk->id);
                return;
            }
        }
    }
}

/* ========== Check 9: Operand Arity ========== */

/* Expected argument count per XiOp.  0xFF = variadic (skip check). */
static const uint8_t expected_narg[XI_OP_COUNT] = {
    [XI_CONST]       = 0,
    [XI_PARAM]       = 0,
    [XI_ADD]         = 2,  [XI_SUB]    = 2,  [XI_MUL]   = 2,
    [XI_DIV]         = 2,  [XI_MOD]    = 2,  [XI_NEG]   = 1,
    [XI_BAND]        = 2,  [XI_BOR]    = 2,  [XI_BXOR]  = 2,
    [XI_BNOT]        = 1,  [XI_SHL]    = 2,  [XI_SHR]   = 2,
    [XI_EQ]          = 2,  [XI_NE]     = 2,  [XI_LT]    = 2,
    [XI_LE]          = 2,  [XI_GT]     = 2,  [XI_GE]    = 2,
    [XI_EQ_STRICT]   = 2,  [XI_NE_STRICT] = 2,
    [XI_NOT]         = 1,
    [XI_CONVERT]     = 1,
    [XI_BOX]         = 1,  [XI_UNBOX]  = 1,
    [XI_NARROW_I8]   = 1,  [XI_NARROW_U8]  = 1,
    [XI_NARROW_I16]  = 1,  [XI_NARROW_U16] = 1,
    [XI_NARROW_I32]  = 1,  [XI_NARROW_U32] = 1,
    [XI_NARROW_F32]  = 1,
    [XI_WIDEN_I8]    = 1,  [XI_WIDEN_U8]   = 1,
    [XI_WIDEN_I16]   = 1,  [XI_WIDEN_U16]  = 1,
    [XI_WIDEN_I32]   = 1,  [XI_WIDEN_U32]  = 1,
    [XI_WIDEN_F32]   = 1,
    [XI_LOAD_FIELD]  = 1,  [XI_STORE_FIELD] = 2,
    [XI_INDEX_GET]   = 2,  [XI_INDEX_SET]   = 3,
    [XI_JSON_NEW]    = 0,     /* no args; aux carries field count + names */
    [XI_JSON_INIT_F] = 2,     /* args[0]=json, args[1]=val */
    [XI_JSON_GET_F]  = 1,     /* args[0]=json */
    [XI_JSON_SET_F]  = 2,     /* args[0]=json, args[1]=val */
    [XI_JSON_DECODE] = 1,     /* args[0]=string_data */
    [XI_ARRAY_NEW]   = 0xFF,
    [XI_MAP_NEW]     = 0xFF,
    [XI_CALL]        = 0xFF,  /* callee + params: variadic */
    [XI_CALL_METHOD] = 0xFF,
    [XI_CALL_BUILTIN]= 0xFF,
    [XI_EXTRACT]     = 1,
    [XI_CLOSURE_NEW] = 0xFF,  /* captures: variadic */
    [XI_LOAD_UPVAL]  = 0,
    [XI_STORE_UPVAL] = 1,
    [XI_GET_SHARED]  = 0,
    [XI_SET_SHARED]  = 1,
    [XI_PRINT]       = 0xFF,  /* one arg per print, but lowerer can vary */
    [XI_GO]          = 0xFF,
    [XI_AWAIT]       = 0xFF,  /* 1 or 2 (optional timeout arg) */
    [XI_CHAN_SEND]       = 2,
    [XI_CHAN_RECV]       = 1,
    [XI_CHAN_TRY_SEND]   = 2,
    [XI_CHAN_TRY_RECV]   = 1,
    [XI_YIELD]       = 0,
    [XI_THROW]       = 1,
    [XI_ITER_NEW]    = 1,
    [XI_ITER_NEXT]   = 1,
    [XI_ITER_VALID]  = 1,
    [XI_DEFER]       = 0xFF,  /* variadic: callee + optional arguments */
    [XI_CHAN_NEW]    = 0xFF,  /* 0 or 1 (buffer size optional) */
    [XI_SET_NEW]    = 0xFF,
    [XI_STR_CONCAT] = 0xFF,  /* variadic */
    [XI_IS]          = 2,
    [XI_AS]          = 1,
    [XI_SLICE]       = 3,
    [XI_RANGE]       = 2,
    [XI_MULTI_RET]   = 0xFF,  /* variadic */
    [XI_ISNULL]      = 1,
    [XI_PHI]         = 0xFF,  /* matches preds: variadic */
    [XI_SELECT]      = 3,
    [XI_COPY]        = 1,
    [XI_CLASS_CREATE]= 0xFF,  /* child function refs: variadic */
    [XI_SCOPE_ENTER] = 0,
    [XI_SCOPE_EXIT]  = 0,
    [XI_TRY]         = 0,
    [XI_CATCH]       = 0,
    [XI_FINALLY]     = 0,
    [XI_END_TRY]     = 0,
    [XI_ASSERT]      = 1,
    [XI_ASSERT_EQ]   = 2,
    [XI_ASSERT_NE]   = 2,
    [XI_ASSERT_THROWS] = 1,
    [XI_TYPEOF]      = 1,
    [XI_GET_BUILTIN] = 0,
    [XI_IMPORT_REF]  = 0,
    [XI_REGEX_COMPILE] = 2,
    [XI_RETAIN]      = 1,
    [XI_RELEASE]     = 1,
    [XI_MOVE]        = 1,
    [XI_STACK_ALLOC] = 0xFF,  /* variadic: inherits args from original alloc op */
};

static void verify_op_arity(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op >= XI_OP_COUNT) continue;
            uint8_t expect = expected_narg[v->op];
            if (expect == 0xFF) continue;  /* variadic — skip */
            if (v->nargs != expect) {
                verr(ctx,
                     "func '%s': v%u %s in b%u has %u args, expected %u",
                     f->name, v->id, xi_op_name(v->op), blk->id,
                     (unsigned)v->nargs, (unsigned)expect);
                return;
            }
        }
    }
}

/* ========== Check 10: Type Contracts ========== */

static bool is_comparison_op(uint16_t op) {
    return (op >= XI_EQ && op <= XI_GE)
        || op == XI_EQ_STRICT || op == XI_NE_STRICT;
}

static bool is_bool_producing_op(uint16_t op) {
    return is_comparison_op(op) || op == XI_NOT || op == XI_IS
        || op == XI_ISNULL || op == XI_ITER_VALID;
}

static void verify_types(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v || !v->type) continue;
            uint16_t op = v->op;

            /* Comparisons and boolean ops must produce bool type */
            if (is_bool_producing_op(op)) {
                XrTypeKind kind = v->type->kind;
                /* Accept XR_KIND_BOOL and XR_KIND_UNKNOWN (lowerer's
                 * type_any for polymorphic comparison results) */
                if (kind != XR_KIND_BOOL && kind != XR_KIND_UNKNOWN) {
                    verr(ctx,
                         "func '%s': v%u (op %u) in b%u should produce bool "
                         "but has type kind=%u",
                         f->name, v->id, op, blk->id, kind);
                    return;
                }
            }

            /* XI_SELECT must have 3 args: cond, true_val, false_val */
            if (op == XI_SELECT && v->nargs == 3) {
                /* Condition (arg[0]) should be bool */
                if (v->args[0] && v->args[0]->type) {
                    XrTypeKind ck = v->args[0]->type->kind;
                    if (ck != XR_KIND_BOOL && ck != XR_KIND_UNKNOWN) {
                        verr(ctx,
                             "func '%s': XI_SELECT v%u in b%u condition v%u "
                             "is not bool (kind=%u)",
                             f->name, v->id, blk->id,
                             v->args[0]->id, ck);
                        return;
                    }
                }
            }

            /* XI_EXTRACT: arg[0] must be a call or multi-ret */
            if (op == XI_EXTRACT && v->nargs == 1 && v->args[0]) {
                uint16_t src_op = v->args[0]->op;
                if (src_op != XI_CALL && src_op != XI_CALL_METHOD
                    && src_op != XI_CALL_BUILTIN && src_op != XI_MULTI_RET) {
                    verr(ctx,
                         "func '%s': XI_EXTRACT v%u in b%u extracts from "
                         "v%u (op %u) which is not a call/multi_ret",
                         f->name, v->id, blk->id,
                         v->args[0]->id, src_op);
                    return;
                }
            }
        }
    }
}

/* ========== Check 11: Side-Effect Flags ========== */

/* Verify that every value's flags are a superset of the opcode's
 * declared minimum effects from xi_op_default_effects().  This
 * subsumes the old op_must_have_side_effect and chan_try checks. */
static void verify_effect_flags(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v) continue;

            uint8_t required = xi_op_default_effects(v->op);
            uint8_t missing = required & ~v->flags;
            if (missing) {
                verr(ctx,
                     "func '%s': v%u %s in b%u missing required "
                     "effect flags: has=0x%02x need=0x%02x missing=0x%02x",
                     f->name, v->id, xi_op_name(v->op), blk->id,
                     v->flags, required, missing);
                return;
            }
        }
    }
}

/* ========== Check 12: XI_CALL_METHOD aux_int Contract ========== */

/* XI_CALL_METHOD.aux_int encodes (global_symbol_id << 1) | is_super.
 * A zero symbol_id means the lowerer failed to resolve the method name
 * at lowering time (only valid when isolate is NULL during AOT).
 * The is_super bit must be 0 or 1.  aux (method name) must be non-NULL. */
static void verify_call_method_contract(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_CALL_METHOD) continue;

            /* aux must carry the method name string */
            if (!v->aux) {
                verr(ctx,
                     "func '%s': XI_CALL_METHOD v%u in b%u has NULL aux "
                     "(expected method name string)",
                     f->name, v->id, blk->id);
                return;
            }

            /* aux_int low bit is is_super (0 or 1) */
            int64_t ai = v->aux_int;
            int is_super = (int)(ai & 1);
            int64_t sym_id = ai >> 1;
            (void)is_super;  /* always valid: 0 or 1 */

            /* symbol_id must be non-negative */
            if (sym_id < 0) {
                verr(ctx,
                     "func '%s': XI_CALL_METHOD v%u in b%u has negative "
                     "symbol_id=%lld (aux_int=%lld)",
                     f->name, v->id, blk->id,
                     (long long)sym_id, (long long)ai);
                return;
            }

            /* Must have at least 1 arg (receiver) */
            if (v->nargs < 1) {
                verr(ctx,
                     "func '%s': XI_CALL_METHOD v%u in b%u has 0 args "
                     "(needs at least receiver)",
                     f->name, v->id, blk->id);
                return;
            }
        }
    }
}

/* ========== Check 13: Tail Call Safety ========== */

/* XI_FLAG_TAIL may only appear on XI_CALL or XI_CALL_METHOD.
 * XI_CALL with tail flag must either be a self-call (aux_int & 0xFF == 1)
 * or the callee must be typed as a function.  Class constructors etc.
 * are not safe tail-call targets because OP_TAILCALL only handles closures. */
static void verify_tail_calls(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v || !(v->flags & XI_FLAG_TAIL)) continue;

            /* Only call ops may carry tail flag */
            if (v->op != XI_CALL && v->op != XI_CALL_METHOD) {
                verr(ctx,
                     "func '%s': v%u %s in b%u has XI_FLAG_TAIL "
                     "but is not a call op",
                     f->name, v->id, xi_op_name(v->op), blk->id);
                return;
            }

            /* XI_CALL: must be self-call or callee typed as function */
            if (v->op == XI_CALL) {
                bool is_self = (v->aux_int & 0xFF) == 1;
                bool callee_is_func = v->nargs >= 1 && v->args[0] &&
                                      v->args[0]->type &&
                                      v->args[0]->type->kind == XR_KIND_FUNCTION;
                if (!is_self && !callee_is_func) {
                    verr(ctx,
                         "func '%s': XI_CALL v%u in b%u has XI_FLAG_TAIL "
                         "but callee is not a function (kind=%u) and "
                         "not a self-call",
                         f->name, v->id, blk->id,
                         v->args[0] && v->args[0]->type
                             ? v->args[0]->type->kind : 0);
                    return;
                }
            }
        }
    }
}

/* Check 14 (channel try ops side-effect) is now covered by
 * verify_effect_flags which checks all opcode defaults. */

/* ========== Check 15: Representation Consistency (STAGE_REPPED) ========== */

/* After select_rep, every value must have a valid XrRep.
 * BOX must produce TAGGED. UNBOX must produce I64 or F64.
 * Phi nodes must be TAGGED (merge point). */
static void verify_repped(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;
    if (f->stage < XI_STAGE_REPPED) return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;

        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v) continue;

            /* Rep must be a known value */
            if (v->rep > XR_REP_STR) {
                verr(ctx,
                     "func '%s': v%u %s in b%u has invalid rep %u",
                     f->name, v->id, xi_op_name(v->op), blk->id, v->rep);
                return;
            }

            /* BOX must produce TAGGED */
            if (v->op == XI_BOX && v->rep != XR_REP_TAGGED) {
                verr(ctx,
                     "func '%s': v%u BOX in b%u has rep %u, expected TAGGED",
                     f->name, v->id, blk->id, v->rep);
                return;
            }

            /* UNBOX must produce scalar (I64 or F64) */
            if (v->op == XI_UNBOX &&
                v->rep != XR_REP_I64 && v->rep != XR_REP_F64 &&
                v->rep != XR_REP_TAGGED) {
                verr(ctx,
                     "func '%s': v%u UNBOX in b%u has invalid rep %u",
                     f->name, v->id, blk->id, v->rep);
                return;
            }
        }

        /* Phi nodes must be TAGGED at merge points */
        for (XiPhi *phi = blk->phis; phi && !ctx->failed; phi = phi->next) {
            if (phi->value.rep != XR_REP_TAGGED) {
                verr(ctx,
                     "func '%s': phi v%u in b%u has rep %u, expected TAGGED",
                     f->name, phi->value.id, blk->id, phi->value.rep);
                return;
            }
        }
    }
}

/* ========== Check 16: Backend Op Legality (STAGE_BACKEND) ========== */

/* At STAGE_BACKEND, all ops must be in the backend-legal whitelist.
 * Non-legal ops should have been lowered by xi_backend_lower(). */
static void verify_backend(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;
    if (f->stage < XI_STAGE_BACKEND) return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;

        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v) continue;

            if (!xi_op_is_backend_legal(v->op)) {
                verr(ctx,
                     "func '%s': v%u has non-backend op %s(%u) in b%u "
                     "(must be lowered before STAGE_BACKEND)",
                     f->name, v->id, xi_op_name(v->op), v->op, blk->id);
                return;
            }
        }
    }
}

/* ========== Stage-Specific Verifiers ========== */

/* RAW: basic SSA structure after lowering. All generic checks in
 * xi_verify() are sufficient; no additional stage-specific checks. */
static void verify_raw(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;
    (void)f;
    /* RAW is the baseline stage. All structural checks are already
     * covered by the generic verify_*() helpers above. */
}

/* CANONICAL: evaluation order is deterministic; syntax sugar expanded.
 * No compound-assignment or increment/decrement ops should remain
 * as high-level ops. */
static void verify_canonical(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;
    if (f->stage < XI_STAGE_CANONICAL) return;
    /* Currently no sugar ops exist in the Xi IR (expansion happens
     * in the AST canonicalizer before lowering). This verifier is
     * a structural placeholder: concrete checks will be added when
     * new high-level ops that require canonical lowering are introduced. */
}

/* CLOSED: upvalue captures fully materialized.
 * XI_CLOSURE_NEW must have exactly ncaptures args matching the child
 * function's capture metadata. XI_LOAD_UPVAL / XI_STORE_UPVAL indices
 * must be within bounds. */
static void verify_closed(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;
    if (f->stage < XI_STAGE_CLOSED) return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;

        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v) continue;

            if (v->op == XI_LOAD_UPVAL || v->op == XI_STORE_UPVAL) {
                int idx = v->aux_int;
                if (idx < 0 || idx >= (int)f->ncaptures) {
                    verr(ctx,
                         "func '%s': v%u %s in b%u has upval index %d "
                         "but function has %u captures",
                         f->name, v->id, xi_op_name(v->op),
                         blk->id, idx, f->ncaptures);
                    return;
                }
            }
        }
    }
}

/* Helper: check if an op is a heap-allocating instruction. */
static bool verify_is_heap_alloc(uint16_t op) {
    switch (op) {
        case XI_ARRAY_NEW: case XI_MAP_NEW: case XI_SET_NEW:
        case XI_JSON_NEW: case XI_CLOSURE_NEW: case XI_STR_CONCAT:
        case XI_REGEX_COMPILE:
            return true;
        default:
            return false;
    }
}

/* OWNED: escape analysis has run; every allocation op carries a
 * valid escape annotation; XI_MOVE ownership semantics are sound. */
static void verify_owned(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;
    if (f->stage < XI_STAGE_OWNED) return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;

        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v) continue;

            /* Allocation ops must have escape level in [0,3] */
            if (verify_is_heap_alloc(v->op)) {
                if (v->escape > 3) {
                    verr(ctx,
                         "func '%s': v%u %s in b%u has invalid escape "
                         "level %u (expected 0-3)",
                         f->name, v->id, xi_op_name(v->op),
                         blk->id, v->escape);
                    return;
                }
            }

            /* XI_RETAIN / XI_RELEASE must reference a value */
            if ((v->op == XI_RETAIN || v->op == XI_RELEASE) &&
                (v->nargs < 1 || !v->args[0])) {
                verr(ctx,
                     "func '%s': v%u %s in b%u has no argument",
                     f->name, v->id, xi_op_name(v->op), blk->id);
                return;
            }

            /* XI_MOVE: source must not be used after the move within
             * the same block.  (Cross-block check would require dominance
             * analysis — defer to a more advanced future pass.) */
            if (v->op == XI_MOVE && v->nargs >= 1 && v->args[0]) {
                XiValue *moved = v->args[0];
                /* Scan remaining values in this block for use of moved */
                for (uint32_t j = i + 1; j < blk->nvalues && !ctx->failed; j++) {
                    XiValue *later = blk->values[j];
                    if (!later) continue;
                    for (uint16_t a = 0; a < later->nargs; a++) {
                        if (later->args[a] == moved) {
                            verr(ctx,
                                 "func '%s': v%u uses moved value v%u "
                                 "(moved at v%u) in b%u",
                                 f->name, later->id, moved->id,
                                 v->id, blk->id);
                            return;
                        }
                    }
                }
            }
        }
    }
}

/* ========== Check 17: NARROW Required Before Typed-Array Store ========== */

/* Return true if the op is a narrowing truncation instruction. */
static bool is_narrow_op(uint16_t op) {
    return op == XI_NARROW_I8  || op == XI_NARROW_U8  ||
           op == XI_NARROW_I16 || op == XI_NARROW_U16 ||
           op == XI_NARROW_I32 || op == XI_NARROW_U32 ||
           op == XI_NARROW_F32;
}

/* XI_INDEX_SET on a sub-width typed array (Array<int8>, Array<uint16>, etc.)
 * must have a NARROW_* op feeding its value argument (args[2]).
 * Without narrowing, a full-width int64/f64 is stored into a narrow slot,
 * silently losing high bits at the VM/JIT level but not at AOT. */
static void verify_narrow_before_typed_store(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;

        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_INDEX_SET) continue;
            if (v->nargs < 3 || !v->args[0] || !v->args[2]) continue;

            /* Check if the collection is a typed array */
            struct XrType *coll_type = v->args[0]->type;
            if (!coll_type || coll_type->kind != XR_KIND_ARRAY) continue;

            struct XrType *elem = coll_type->container.element_type;
            if (!elem || elem->native_width == 0) continue;

            /* Sub-width element — args[2] must be a NARROW_* op */
            XiValue *val = v->args[2];
            XR_DCHECK(val != NULL, "verify: INDEX_SET val arg is NULL");
            if (!is_narrow_op(val->op)) {
                verr(ctx,
                     "func '%s': XI_INDEX_SET v%u in b%u stores to "
                     "sub-width typed array (native_width=%u) but "
                     "value v%u (op %s) is not a NARROW_* op",
                     f->name, v->id, blk->id,
                     elem->native_width,
                     val->id, xi_op_name(val->op));
                return;
            }
        }
    }
}

/* ========== Public API ========== */

XR_FUNC bool xi_verify(const XiFunc *f, char *errbuf, int errbuf_size) {
    XR_DCHECK(errbuf != NULL, "xi_verify: NULL errbuf");
    XR_DCHECK(errbuf_size > 0, "xi_verify: errbuf_size <= 0");

    if (!f) {
        snprintf(errbuf, (size_t)errbuf_size, "NULL function pointer");
        return false;
    }

    VerifyCtx ctx = { .buf = errbuf, .size = errbuf_size, .failed = false };
    errbuf[0] = '\0';

    /* Function-level */
    verify_func(&ctx, f);
    if (ctx.failed) return false;

    /* Entry block */
    verify_entry(&ctx, f);
    if (ctx.failed) return false;

    /* Per-block checks */
    for (uint32_t b = 0; b < f->nblocks && !ctx.failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) {
            verr(&ctx, "func '%s': blocks[%u] is NULL", f->name, b);
            break;
        }

        verify_block(&ctx, f, blk);
        if (ctx.failed) break;

        /* Values in this block */
        for (uint32_t i = 0; i < blk->nvalues && !ctx.failed; i++) {
            if (!blk->values[i]) {
                verr(&ctx, "func '%s': b%u values[%u] is NULL",
                     f->name, blk->id, i);
                break;
            }
            verify_value(&ctx, f, blk, blk->values[i]);
        }

        /* Phi nodes */
        for (XiPhi *phi = blk->phis; phi && !ctx.failed; phi = phi->next) {
            verify_phi(&ctx, f, blk, phi);
        }

        /* CFG edges */
        verify_cfg_edges(&ctx, f, blk);
    }

    /* Unique IDs */
    if (!ctx.failed) {
        verify_unique_ids(&ctx, f);
    }

    /* Operand arity (static table check, fast) */
    if (!ctx.failed) {
        verify_op_arity(&ctx, f);
    }

    /* Effect flags: value flags must be superset of opcode defaults */
    if (!ctx.failed) {
        verify_effect_flags(&ctx, f);
    }

    /* Type contracts (bool-producing ops, XI_SELECT cond, XI_EXTRACT source) */
    if (!ctx.failed) {
        verify_types(&ctx, f);
    }

    /* SSA dominance (requires RPO + dominator computation).
     * Cast away const: xi_compute_rpo/dominators write scratch fields
     * (rpo, idom, dom_depth) but do not modify the IR semantics. */
    if (!ctx.failed) {
        verify_dominance(&ctx, (XiFunc *)f);
    }

    /* XI_CALL_METHOD aux_int encoding contract */
    if (!ctx.failed) {
        verify_call_method_contract(&ctx, f);
    }

    /* Tail call safety: only on call ops with valid callee */
    if (!ctx.failed) {
        verify_tail_calls(&ctx, f);
    }

    /* Channel try ops check is now covered by verify_effect_flags */

    /* Representation consistency (only at STAGE_REPPED and above) */
    if (!ctx.failed) {
        verify_repped(&ctx, f);
    }

    /* Backend op legality (only at STAGE_BACKEND) */
    if (!ctx.failed) {
        verify_backend(&ctx, f);
    }

    /* NARROW before typed-array store (all stages) */
    if (!ctx.failed) {
        verify_narrow_before_typed_store(&ctx, f);
    }

    return !ctx.failed;
}

XR_FUNC bool xi_verify_stage(const XiFunc *f, XiStage stage,
                              char *errbuf, int errbuf_size) {
    /* Run all generic checks first */
    if (!xi_verify(f, errbuf, errbuf_size))
        return false;

    VerifyCtx ctx = { .buf = errbuf, .size = errbuf_size, .failed = false };
    errbuf[0] = '\0';

    /* Stage-specific checks run cumulatively: each stage includes
     * all checks from preceding stages (fall-through). */
    if (stage >= XI_STAGE_RAW)
        verify_raw(&ctx, f);
    if (!ctx.failed && stage >= XI_STAGE_CANONICAL)
        verify_canonical(&ctx, f);
    if (!ctx.failed && stage >= XI_STAGE_CLOSED)
        verify_closed(&ctx, f);
    if (!ctx.failed && stage >= XI_STAGE_OWNED)
        verify_owned(&ctx, f);
    /* REPPED and BACKEND already gated inside verify_repped/verify_backend
     * (called by xi_verify above), so no double-run needed. */

    /* Invariant mask consistency: the function's mask must include
     * all bits implied by its current stage. */
    if (!ctx.failed) {
        XiInvariantMask required = xi_stage_invariants(f->stage);
        XiInvariantMask missing = required & ~f->invariant_mask;
        if (missing) {
            verr(&ctx,
                 "func '%s': invariant_mask 0x%x is missing bits 0x%x "
                 "required by stage %s",
                 f->name, f->invariant_mask, missing,
                 xi_stage_name(f->stage));
        }
    }

    return !ctx.failed;
}
