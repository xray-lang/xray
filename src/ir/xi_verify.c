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
#include "xi_evidence.h"
#include "xi_effect.h"
#include "xi_backend.h"
#include "xi_coro_analyze.h"
#include "xi_ops_gen.h"
#include "xi_verify_gen.h"
#include "xi_op_name.h"
#include "xi_analysis.h"
#include "xi_semantic_intrinsic.h"
#include "xi_own.h"
#include "xi_tbaa.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xffi_sig.h"
#include "../shared/xr_array_core.h"
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
    if (ctx->failed)
        return; /* report first error only */
    ctx->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->buf, (size_t) ctx->size, fmt, ap);
    va_end(ap);
}

/* ========== Individual Checks ========== */

/* Check 1: function-level invariants.
 *
 * Xi IR carries an implicit invariant that block->id equals the block's
 * index in f->blocks[].  Multiple passes (notably SCCP, xi_loop, and
 * codegen lowering) index per-block scratch arrays by block id and pass
 * succ->id directly to mark_edge / headers[] / etc. as an array index.
 * Any pass that permutes f->blocks[] without resyncing block->id
 * silently breaks downstream analyses; verify here so the offending
 * pass is named immediately. */
static void verify_func(VerifyCtx *ctx, const XiFunc *f) {
    if (!f->name) {
        verr(ctx, "function has NULL name");
        return;
    }
    if (f->return_type && xr_type_contains_error(f->return_type)) {
        verr(ctx, "func '%s': return type is compiler-only ErrorType", f->name);
        return;
    }
    if (f->source_var_types) {
        for (uint32_t i = 0; i < f->source_var_count; i++) {
            if (xr_type_contains_error(f->source_var_types[i])) {
                verr(ctx, "func '%s': source var %u uses compiler-only ErrorType", f->name, i);
                return;
            }
        }
    }
    for (uint16_t i = 0; i < f->ncaptures; i++) {
        if (xr_type_contains_error(f->captures[i].type)) {
            verr(ctx, "func '%s': capture %u uses compiler-only ErrorType", f->name, i);
            return;
        }
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
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk) {
            verr(ctx, "func '%s': blocks[%u] is NULL", f->name, b);
            return;
        }
        if (blk->id != b) {
            verr(ctx, "func '%s': blocks[%u]->id is %u (must equal index)", f->name, b, blk->id);
            return;
        }
    }
}

/* Check 2: entry block must have 0 predecessors */
static void verify_entry(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;
    if (f->entry->npreds != 0) {
        verr(ctx, "func '%s': entry block b%u has %u predecessors (expected 0)", f->name,
             f->entry->id, f->entry->npreds);
    }
}

/* Check 3: block-level invariants */
static void verify_block(VerifyCtx *ctx, const XiFunc *f, const XiBlock *blk) {
    if (ctx->failed)
        return;
    XR_DCHECK(blk != NULL, "verify_block: NULL block");

    /* Block must belong to this function */
    if (blk->func != f) {
        verr(ctx, "func '%s': block b%u has wrong func pointer", f->name, blk->id);
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
                verr(ctx, "func '%s': IF block b%u has NULL control", f->name, blk->id);
                return;
            }
            if (!blk->succs[0] || !blk->succs[1]) {
                verr(ctx, "func '%s': IF block b%u missing successor(s)", f->name, blk->id);
                return;
            }
            break;
        case XI_BLOCK_RETURN:
            /* Error-channel propagation terminates a function without
             * producing its normal return value.  It is represented as a
             * RETURN-kind block so CFG consumers still see a terminal edge. */
            if (blk->control && blk->control->op == XI_ERR_RETURN) {
                if (blk->control->nargs != 1 || !blk->control->args[0]) {
                    verr(ctx, "func '%s': error RETURN block b%u has malformed ERR_RETURN v%u",
                         f->name, blk->id, blk->control->id);
                    return;
                }
                break;
            }
            if (!f->return_type) {
                verr(ctx, "func '%s': RETURN block b%u has no function return type", f->name,
                     blk->id);
                return;
            }
            if (f->return_type->kind == XR_KIND_NEVER) {
                verr(ctx, "func '%s': never-returning function has RETURN block b%u", f->name,
                     blk->id);
                return;
            }
            if (f->return_type->kind == XR_KIND_UNIT) {
                if (blk->control) {
                    verr(ctx, "func '%s': unit RETURN block b%u carries unexpected value v%u",
                         f->name, blk->id, blk->control->id);
                    return;
                }
                break;
            }
            /* A generator's declared return type is the Iterator<T> produced
             * when the function is called.  Reaching the end of its resumable
             * body instead signals iterator completion and carries no normal
             * return value. */
            if (f->entry_type == 2 && !blk->control)
                break;
            if (!blk->control) {
                verr(ctx, "func '%s': non-unit RETURN block b%u requires a value", f->name,
                     blk->id);
                return;
            }
            const bool nominal_ref_compatible = blk->control->type &&
                                                (f->return_type->kind == XR_KIND_CLASS ||
                                                 f->return_type->kind == XR_KIND_INSTANCE ||
                                                 f->return_type->kind == XR_KIND_INTERFACE) &&
                                                (blk->control->type->kind == XR_KIND_CLASS ||
                                                 blk->control->type->kind == XR_KIND_INSTANCE ||
                                                 blk->control->type->kind == XR_KIND_INTERFACE);
            if (!blk->control->type ||
                (f->return_type->kind != XR_KIND_UNKNOWN &&
                 blk->control->type->kind != XR_KIND_UNKNOWN &&
                 f->return_type->kind != blk->control->type->kind && !nominal_ref_compatible &&
                 !xr_type_assignable((XrType *) f->return_type, (XrType *) blk->control->type))) {
                verr(ctx,
                     "func '%s': RETURN block b%u value v%u is not assignable to function "
                     "return type (op=%s, result kind=%u, return kind=%u)",
                     f->name, blk->id, blk->control->id, xi_op_name(blk->control->op),
                     blk->control->type->kind, f->return_type->kind);
                return;
            }
            break;
        case XI_BLOCK_UNREACHABLE:
            break;
        default:
            verr(ctx, "func '%s': block b%u has invalid kind %u", f->name, blk->id, blk->kind);
            return;
    }
}

static bool verify_endian_operand(VerifyCtx *ctx, const XiFunc *f, const XiBlock *blk,
                                  const XiValue *owner, const XiValue *endian) {
    if (!endian || !endian->type ||
        (endian->type->kind != XR_KIND_INT && endian->type->kind != XR_KIND_ENUM &&
         endian->type->kind != XR_KIND_UNKNOWN)) {
        verr(ctx, "func '%s': v%u %s in b%u has invalid Endian operand type", f->name, owner->id,
             xi_op_name(owner->op), blk->id);
        return false;
    }
    if (endian->op == XI_CONST &&
        (endian->aux_int < XR_ENDIAN_NATIVE || endian->aux_int > XR_ENDIAN_BE)) {
        verr(ctx, "func '%s': v%u %s in b%u has invalid Endian value %lld", f->name, owner->id,
             xi_op_name(owner->op), blk->id, (long long) endian->aux_int);
        return false;
    }
    return true;
}

static void verify_ptr_memory_contract(VerifyCtx *ctx, const XiFunc *f, const XiBlock *blk,
                                       const XiValue *v) {
    if (ctx->failed || (v->op != XI_PTR_LOAD && v->op != XI_PTR_STORE))
        return;

    const uint16_t expected_nargs = v->op == XI_PTR_LOAD ? 2 : 3;
    if (v->nargs != expected_nargs)
        return; /* The generic arity verifier owns this diagnostic. */
    if (v->aux_int < 0 || v->aux_int > UINT8_MAX) {
        verr(ctx, "func '%s': v%u %s in b%u has out-of-range memory aux %lld", f->name, v->id,
             xi_op_name(v->op), blk->id, (long long) v->aux_int);
        return;
    }

    const uint8_t aux = (uint8_t) v->aux_int;
    const uint8_t valid_bits = XR_FFI_PTR_AUX_TYPE_MASK | XR_FFI_PTR_AUX_UNALIGNED;
    const uint8_t ffi_type = xr_ffi_ptr_aux_type(aux);
    const XrAbiScalarDesc *desc = xr_abi_scalar_desc(ffi_type);
    if ((aux & (uint8_t) ~valid_bits) != 0 || !desc || !desc->is_memory_scalar) {
        verr(ctx, "func '%s': v%u %s in b%u has invalid memory scalar aux 0x%02x", f->name, v->id,
             xi_op_name(v->op), blk->id, aux);
        return;
    }
    if (!v->args[0] || !v->args[0]->type || !XR_TYPE_IS_POINTER(v->args[0]->type)) {
        verr(ctx, "func '%s': v%u %s in b%u address is not a raw pointer", f->name, v->id,
             xi_op_name(v->op), blk->id);
        return;
    }

    const XiValue *endian = v->args[v->op == XI_PTR_LOAD ? 1 : 2];
    if (!verify_endian_operand(ctx, f, blk, v, endian))
        return;
    if (ffi_type == XR_FFI_T_PTR &&
        (endian->op != XI_CONST || endian->aux_int != XR_ENDIAN_NATIVE)) {
        verr(ctx, "func '%s': v%u %s in b%u pointer memory access requires constant Endian.Native",
             f->name, v->id, xi_op_name(v->op), blk->id);
        return;
    }

    if (v->op == XI_PTR_LOAD) {
        const uint8_t result_ffi = xr_ffi_type_from_xrtype(v->type, false);
        if (result_ffi != ffi_type) {
            verr(ctx,
                 "func '%s': v%u PTR_LOAD in b%u result type maps to ABI scalar %u, aux uses %u",
                 f->name, v->id, blk->id, result_ffi, ffi_type);
        }
        return;
    }

    if (!v->type || v->type->kind != XR_KIND_UNIT) {
        verr(ctx, "func '%s': v%u PTR_STORE in b%u must produce unit", f->name, v->id, blk->id);
        return;
    }
    const XiValue *value = v->args[1];
    const uint8_t value_ffi = xr_ffi_type_from_xrtype(value ? value->type : NULL, false);
    if (value_ffi != ffi_type) {
        verr(ctx, "func '%s': v%u PTR_STORE in b%u value type maps to ABI scalar %u, aux uses %u",
             f->name, v->id, blk->id, value_ffi, ffi_type);
    }
}

static void verify_buffer_materialize_contract(VerifyCtx *ctx, const XiFunc *f, const XiBlock *blk,
                                               const XiValue *v) {
    if (ctx->failed || !v || v->op != XI_BUFFER_MATERIALIZE)
        return;
    if (v->nargs != 1)
        return;
    if (!v->args[0] || !v->args[0]->type || !xr_type_is_named_class(v->args[0]->type, "Buffer")) {
        verr(ctx, "func '%s': v%u BUFFER_MATERIALIZE in b%u does not consume Buffer", f->name,
             v->id, blk->id);
        return;
    }
    uint8_t code = XI_BUFFER_MATERIALIZE_CODE(v->aux_int);
    uint32_t size = XI_BUFFER_MATERIALIZE_SIZE(v->aux_int);
    uint16_t align = XI_BUFFER_MATERIALIZE_ALIGN(v->aux_int);
    if (size == 0 || align == 0) {
        verr(ctx, "func '%s': v%u BUFFER_MATERIALIZE in b%u has empty layout evidence", f->name,
             v->id, blk->id);
        return;
    }
    if (code == XI_BUFFER_MATERIALIZE_AGGREGATE) {
        const XrAggregateLayout *layout = (const XrAggregateLayout *) v->aux;
        if (!layout || layout->total_size != size || layout->alignment != align ||
            !xr_aggregate_layout_is_headerless(layout))
            verr(ctx,
                 "func '%s': v%u BUFFER_MATERIALIZE in b%u has inconsistent aggregate evidence",
                 f->name, v->id, blk->id);
        return;
    }
    uint8_t result_code = xr_ffi_type_from_xrtype(v->type, false);
    if (!xr_ffi_type_is_memory_scalar(code) || result_code != code || v->aux != NULL)
        verr(ctx, "func '%s': v%u BUFFER_MATERIALIZE in b%u has inconsistent scalar evidence",
             f->name, v->id, blk->id);
}

static bool verify_type_reject_error(VerifyCtx *ctx, const XiFunc *f, const char *owner,
                                     uint32_t owner_id, const XrType *type) {
    if (!xr_type_contains_error(type))
        return false;
    verr(ctx, "func '%s': %s v%u uses compiler-only ErrorType", f->name, owner, owner_id);
    return true;
}

static bool verify_view_root_matches(const XiValue *value, uint32_t root_value_id) {
    for (uint8_t depth = 0; value && depth < 16; depth++) {
        if (value->id == root_value_id)
            return true;
        if (value->nargs != 1 || !value->args[0])
            return false;
        switch ((XiOp) value->op) {
            case XI_BOX:
            case XI_UNBOX:
            case XI_COPY:
            case XI_CONVERT:
            case XI_SOURCE_MOVE:
            case XI_OWNER_FORWARD:
                value = value->args[0];
                break;
            default:
                return false;
        }
    }
    return false;
}

/* Check 4: value-level invariants */
static void verify_value(VerifyCtx *ctx, const XiFunc *f, const XiBlock *blk, const XiValue *v) {
    if (ctx->failed)
        return;
    XR_DCHECK(v != NULL, "verify_value: NULL value");

    /* Type must be non-NULL */
    if (!v->type) {
        verr(ctx, "func '%s': value v%u in b%u has NULL type", f->name, v->id, blk->id);
        return;
    }
    if (verify_type_reject_error(ctx, f, "value", v->id, v->type))
        return;

    /* Op must be in valid range */
    if (v->op >= XI_OP_COUNT) {
        verr(ctx, "func '%s': value v%u in b%u has invalid op %u", f->name, v->id, blk->id, v->op);
        return;
    }

    if (v->op == XI_PHI) {
        verr(ctx,
             "func '%s': value-list entry v%u in b%u is XI_PHI; phi nodes must live on blk->phis",
             f->name, v->id, blk->id);
        return;
    }

    /* Block back-pointer must match */
    if (v->block != blk) {
        verr(ctx, "func '%s': value v%u claims block b%u but is in b%u", f->name, v->id,
             v->block ? v->block->id : 9999, blk->id);
        return;
    }

    /* Args array consistency */
    if (v->nargs > 0 && !v->args) {
        verr(ctx, "func '%s': value v%u in b%u has %u args but NULL args ptr", f->name, v->id,
             blk->id, v->nargs);
        return;
    }

    /* Each arg should be a plausible value (non-NULL, has type).
     * Exception: CLOSURE_NEW args may be NULL for upvalue-chain captures
     * that have no local SSA value (source is parent's upvalue, not a reg). */
    for (uint16_t a = 0; a < v->nargs; a++) {
        if (!v->args[a]) {
            if (v->op == XI_CLOSURE_NEW)
                continue; /* NULL capture arg is valid */
            verr(ctx, "func '%s': value v%u in b%u arg[%u] is NULL", f->name, v->id, blk->id, a);
            return;
        }
        if (!v->args[a]->type) {
            verr(ctx, "func '%s': value v%u in b%u arg[%u] (v%u) has NULL type", f->name, v->id,
                 blk->id, a, v->args[a]->id);
            return;
        }
        if (verify_type_reject_error(ctx, f, "value arg", v->args[a]->id, v->args[a]->type))
            return;
    }

    if (v->view_evidence.complete) {
        const XiViewEvidence *view = &v->view_evidence;
        if (!XR_TYPE_IS_SLICE(v->type)) {
            verr(ctx, "func '%s': non-Slice v%u carries ViewEvidence", f->name, v->id);
            return;
        }
        if (view->origin == XI_VIEW_ORIGIN_NONE || view->origin == XI_VIEW_ORIGIN_MULTI ||
            view->origin == XI_VIEW_ORIGIN_UNKNOWN) {
            verr(ctx, "func '%s': Slice v%u has incomplete ViewEvidence origin=%u", f->name, v->id,
                 (unsigned) view->origin);
            return;
        }
        if (view->capability != 1 && view->capability != 2) {
            verr(ctx, "func '%s': Slice v%u has invalid ViewEvidence capability=%u", f->name, v->id,
                 (unsigned) view->capability);
            return;
        }
        if (view->origin == XI_VIEW_ORIGIN_STATIC) {
            if (view->source_operand != -1 || view->root_value_id != 0) {
                verr(ctx, "func '%s': static Slice v%u names a caller-local root", f->name, v->id);
                return;
            }
        } else {
            if (view->source_operand < 0 || view->source_operand >= (int16_t) v->nargs ||
                !v->args[view->source_operand] ||
                !verify_view_root_matches(v->args[view->source_operand], view->root_value_id)) {
                verr(ctx, "func '%s': Slice v%u has stale/invalid ViewEvidence root operand",
                     f->name, v->id);
                return;
            }
        }
    }
    if (v->op == XI_SLICE_FROM_PTR && !v->view_evidence.complete) {
        verr(ctx, "func '%s': raw-to-Slice v%u lacks complete ViewEvidence", f->name, v->id);
        return;
    }
    if ((v->op == XI_CALL || v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) &&
        XR_TYPE_IS_SLICE(v->type) && !v->view_evidence.complete) {
        verr(ctx, "func '%s': Slice-returning call v%u lacks complete ViewEvidence", f->name,
             v->id);
        return;
    }
}

/* Check 5: phi node invariants */
static void verify_phi(VerifyCtx *ctx, const XiFunc *f, const XiBlock *blk, const XiPhi *phi) {
    if (ctx->failed)
        return;
    XR_DCHECK(phi != NULL, "verify_phi: NULL phi");

    /* Phi must have op == XI_PHI */
    if (phi->value.op != XI_PHI) {
        verr(ctx, "func '%s': phi in b%u has op %u (expected XI_PHI=%u)", f->name, blk->id,
             phi->value.op, XI_PHI);
        return;
    }

    /* Phi type must be non-NULL */
    if (!phi->value.type) {
        verr(ctx, "func '%s': phi v%u in b%u has NULL type", f->name, phi->value.id, blk->id);
        return;
    }
    if (verify_type_reject_error(ctx, f, "phi", phi->value.id, phi->value.type))
        return;

    /* Phi arg count must match predecessor count */
    if (phi->value.nargs != blk->npreds) {
        verr(ctx, "func '%s': phi v%u in b%u has %u args but block has %u preds", f->name,
             phi->value.id, blk->id, phi->value.nargs, blk->npreds);
        return;
    }

    /* Each phi arg must be non-NULL with valid type */
    for (uint16_t a = 0; a < phi->value.nargs; a++) {
        if (!phi->value.args[a]) {
            verr(ctx, "func '%s': phi v%u in b%u arg[%u] is NULL", f->name, phi->value.id, blk->id,
                 a);
            return;
        }
        if (verify_type_reject_error(ctx, f, "phi arg", phi->value.args[a]->id,
                                     phi->value.args[a]->type))
            return;
    }
}

/* Check 6: CFG edge symmetry — succ→pred and pred→succ both directions */
static void verify_cfg_edges(VerifyCtx *ctx, const XiFunc *f, const XiBlock *blk) {
    if (ctx->failed)
        return;

    /* Forward: each successor must list blk as a predecessor */
    for (int s = 0; s < 2; s++) {
        XiBlock *succ = blk->succs[s];
        if (!succ)
            continue;

        bool found = false;
        for (uint16_t p = 0; p < succ->npreds; p++) {
            if (succ->preds[p] == blk) {
                found = true;
                break;
            }
        }
        if (!found) {
            verr(ctx, "func '%s': b%u has successor b%u but is not in its pred list", f->name,
                 blk->id, succ->id);
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
                verr(ctx, "func '%s': IF block b%u has %d successors (expected 2)", f->name,
                     blk->id, nsucc);
            }
            break;
        case XI_BLOCK_RETURN:
        case XI_BLOCK_UNREACHABLE:
            if (nsucc != 0) {
                verr(ctx, "func '%s': %s block b%u has %d successors (expected 0)", f->name,
                     blk->kind == XI_BLOCK_RETURN ? "RETURN" : "UNREACHABLE", blk->id, nsucc);
            }
            break;
        case XI_BLOCK_PLAIN:
            break;
    }
}

/* Check 7: unique value IDs within function */
static void verify_unique_ids(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;

    /* Use a simple O(n) scan; for typical function sizes this is fine. */
    uint32_t max_id = f->next_value_id;

    /* Track seen IDs with a bitmap if small enough, else skip. */
    if (max_id > 10000)
        return; /* skip for very large functions */

    /* Stack-allocate a bitset. Max ~1.2 KB for 10000 IDs. */
    uint8_t seen[1250];
    memset(seen, 0, sizeof(seen));

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            uint32_t vid = blk->values[i]->id;
            if (vid >= max_id) {
                verr(ctx, "func '%s': value v%u >= next_value_id %u", f->name, vid, max_id);
                return;
            }
            uint32_t byte = vid / 8;
            uint8_t bit = (uint8_t) (1 << (vid & 7));
            if (byte < sizeof(seen)) {
                if (seen[byte] & bit) {
                    verr(ctx, "func '%s': duplicate value ID v%u", f->name, vid);
                    return;
                }
                seen[byte] |= bit;
            }
        }

        /* Also check phi IDs */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            uint32_t vid = phi->value.id;
            if (vid >= max_id) {
                verr(ctx, "func '%s': phi v%u >= next_value_id %u", f->name, vid, max_id);
                return;
            }
            uint32_t byte = vid / 8;
            uint8_t bit = (uint8_t) (1 << (vid & 7));
            if (byte < sizeof(seen)) {
                if (seen[byte] & bit) {
                    verr(ctx, "func '%s': duplicate phi ID v%u", f->name, vid);
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
    if (!def_blk || !use_blk)
        return false;
    /* Walk the dominator tree from use_blk to entry.
     * dom_depth == 0 for entry; idom == self for entry. */
    const XiBlock *b = use_blk;
    while (b && b->dom_depth >= def_blk->dom_depth) {
        if (b == def_blk)
            return true;
        if (b->idom == b)
            break; /* entry block */
        b = b->idom;
    }
    return false;
}

/* Verify SSA dominance: each value arg must be defined in a block that
 * dominates the use site.  Phi args are special: arg[i] must be
 * dominated by preds[i] (since the phi chooses along the edge). */
static void verify_dominance(VerifyCtx *ctx, XiFunc *f) {
    if (ctx->failed)
        return;

    /* Ensure RPO and dominators are up to date (cached). */
    xi_ensure_dominators(f);

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;

        /* Check regular values */
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                XiValue *arg = v->args[a];
                if (!arg || !arg->block)
                    continue;
                if (!block_dominates(arg->block, blk)) {
                    verr(ctx,
                         "func '%s': v%u in b%u uses v%u defined in b%u "
                         "which does not dominate b%u",
                         f->name, v->id, blk->id, arg->id, arg->block->id, blk->id);
                    return;
                }
            }
        }

        /* Check phi args: arg[i] must be dominated by preds[i]. */
        for (XiPhi *phi = blk->phis; phi && !ctx->failed; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                XiValue *arg = phi->value.args[a];
                if (!arg || !arg->block)
                    continue;
                if (a >= blk->npreds)
                    break;
                XiBlock *pred = blk->preds[a];
                if (!pred)
                    continue;
                if (!block_dominates(arg->block, pred)) {
                    verr(ctx,
                         "func '%s': phi v%u in b%u arg[%u] (v%u from b%u) "
                         "not dominated by predecessor b%u",
                         f->name, phi->value.id, blk->id, a, arg->id, arg->block->id, pred->id);
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
                     f->name, blk->id, blk->control->id, blk->control->block->id, blk->id);
                return;
            }
        }
    }
}

/* ========== Check 9: Operand Arity ========== */

/* Expected argument count per XiOp comes from generated Xi metadata. */
static void verify_op_arity(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op >= XI_OP_COUNT)
                continue;
            uint8_t expect = xi_generated_op_arity(v->op);
            if (expect == XI_OP_ARITY_VARIADIC)
                continue; /* variadic: skip */
            if (v->nargs != expect) {
                verr(ctx, "func '%s': v%u %s in b%u has %u args, expected %u", f->name, v->id,
                     xi_op_name(v->op), blk->id, (unsigned) v->nargs, (unsigned) expect);
                return;
            }
        }
    }
}

/* ========== Check 10: Type Contracts ========== */

static bool verify_type_is_boolish(const XrType *type) {
    return type && (type->kind == XR_KIND_BOOL || type->kind == XR_KIND_UNKNOWN);
}

static bool verify_type_is_truth_testable(const XrType *type) {
    if (!type)
        return false;
    switch (type->kind) {
        case XR_KIND_UNIT:
        case XR_KIND_NEVER:
            return false;
        default:
            return true;
    }
}

static bool verify_type_assignable_or_unknown(XrType *target, XrType *source) {
    if (!target || !source)
        return true;
    if (target->kind == XR_KIND_UNKNOWN || source->kind == XR_KIND_UNKNOWN)
        return true;
    return xr_type_assignable(target, source);
}

static bool verify_exact_bit_native_type(int64_t native_type) {
    switch ((uint8_t) native_type) {
        case XR_NATIVE_I64: /* also canonical `int`, with 64-bit bit semantics */
        case XR_NATIVE_I8:
        case XR_NATIVE_I16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return native_type >= 0 && native_type <= UINT8_MAX;
        default:
            return false;
    }
}

static bool verify_exact_bit_contract(VerifyCtx *ctx, const XiFunc *f, const XiBlock *blk,
                                      const XiValue *v) {
    bool rotate = v->op == XI_BIT_ROTL || v->op == XI_BIT_ROTR;
    bool mul_high = v->op == XI_BIT_MUL_HIGH;
    bool two_arg = rotate || mul_high;
    bool receiver_result = two_arg || v->op == XI_BIT_BSWAP;
    bool query = v->op == XI_BIT_POPCOUNT || v->op == XI_BIT_CLZ || v->op == XI_BIT_CTZ;
    if (!two_arg && !receiver_result && !query)
        return true;

    uint16_t expected_args = two_arg ? 2 : 1;
    if (v->nargs != expected_args || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_INT || v->args[0]->type->is_nullable) {
        verr(ctx, "func '%s': v%u %s in b%u requires a non-null exact integer receiver", f->name,
             v->id, xi_op_name(v->op), blk->id);
        return false;
    }
    if (!verify_exact_bit_native_type(v->aux_int) ||
        v->args[0]->type->native_width != (uint8_t) v->aux_int) {
        verr(ctx, "func '%s': v%u %s in b%u has native type %lld but receiver width tag is %u",
             f->name, v->id, xi_op_name(v->op), blk->id, (long long) v->aux_int,
             (unsigned) v->args[0]->type->native_width);
        return false;
    }
    /* The second operand is an integer value; accept XR_KIND_UNKNOWN because
     * module-level const operands load through GET_SHARED as polymorphic `any`
     * (the same relaxation MUL and the Endian operand check already allow). */
    if (two_arg) {
        const XrType *t1 = v->args[1] ? v->args[1]->type : NULL;
        bool ok =
            t1 && ((t1->kind == XR_KIND_INT && !t1->is_nullable) || t1->kind == XR_KIND_UNKNOWN);
        if (!ok) {
            verr(ctx, "func '%s': v%u %s in b%u requires an integer second operand", f->name, v->id,
                 xi_op_name(v->op), blk->id);
            return false;
        }
    }
    if (mul_high &&
        (v->aux_int != XR_NATIVE_U8 && v->aux_int != XR_NATIVE_U16 && v->aux_int != XR_NATIVE_U32 &&
         v->aux_int != XR_NATIVE_U64 && v->aux_int != XR_NATIVE_USIZE)) {
        verr(ctx, "func '%s': v%u %s in b%u requires an unsigned exact-width receiver", f->name,
             v->id, xi_op_name(v->op), blk->id);
        return false;
    }
    if (mul_high && !v->args[1]) {
        /* The source method registry already proves the rhs exact-width type.
         * Optimizer representation selection may legitimately turn an
         * imported constant into UNKNOWN-typed GET_SHARED/UNBOX values.  From
         * semantic Xi onward the stable intrinsic identity plus aux_int width
         * is the proof consumed by VM/AOT, so only operand presence remains a
         * stage-local invariant here. */
        verr(ctx, "func '%s': v%u %s in b%u requires an rhs operand", f->name, v->id,
             xi_op_name(v->op), blk->id);
        return false;
    }
    if (receiver_result && !xr_type_equals((XrType *) v->type, (XrType *) v->args[0]->type)) {
        verr(ctx, "func '%s': v%u %s in b%u must preserve the exact receiver type", f->name, v->id,
             xi_op_name(v->op), blk->id);
        return false;
    }
    if (query && (!v->type || v->type->kind != XR_KIND_INT || v->type->native_width != 0)) {
        verr(ctx, "func '%s': v%u %s in b%u must return int", f->name, v->id, xi_op_name(v->op),
             blk->id);
        return false;
    }
    return true;
}

static void verify_types(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        if (blk->kind == XI_BLOCK_IF && blk->control &&
            !verify_type_is_truth_testable(blk->control->type)) {
            uint32_t kind =
                blk->control->type ? (uint32_t) blk->control->type->kind : XR_KIND_COUNT;
            verr(ctx, "func '%s': IF block b%u control v%u is not truth-testable (kind=%u)",
                 f->name, blk->id, blk->control->id, kind);
            return;
        }
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v || !v->type)
                continue;
            uint16_t op = v->op;

            if (!verify_exact_bit_contract(ctx, f, blk, v))
                return;

            if (xi_verify_generated_op_has_check(op, XI_VERIFY_CHECK_OBSOLETE)) {
                verr(ctx, "func '%s': obsolete multi-return op %u in b%u; use tuple values instead",
                     f->name, op, blk->id);
                return;
            }

            /* Comparisons and boolean ops must produce bool type */
            if (xi_verify_generated_op_has_check(op, XI_VERIFY_CHECK_BOOL_RESULT)) {
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
            if (xi_verify_generated_op_has_check(op, XI_VERIFY_CHECK_SELECT_CONTRACT) &&
                v->nargs == 3) {
                /* Condition (arg[0]) should be bool */
                if (v->args[0] && v->args[0]->type) {
                    if (!verify_type_is_boolish(v->args[0]->type)) {
                        verr(ctx,
                             "func '%s': XI_SELECT v%u in b%u condition v%u "
                             "is not bool (kind=%u)",
                             f->name, v->id, blk->id, v->args[0]->id, v->args[0]->type->kind);
                        return;
                    }
                }
                if (v->args[1] && v->args[1]->type &&
                    !verify_type_assignable_or_unknown(v->type, v->args[1]->type)) {
                    verr(ctx,
                         "func '%s': XI_SELECT v%u in b%u true arm v%u "
                         "is not assignable to result type",
                         f->name, v->id, blk->id, v->args[1]->id);
                    return;
                }
                if (v->args[2] && v->args[2]->type &&
                    !verify_type_assignable_or_unknown(v->type, v->args[2]->type)) {
                    verr(ctx,
                         "func '%s': XI_SELECT v%u in b%u false arm v%u "
                         "is not assignable to result type",
                         f->name, v->id, blk->id, v->args[2]->id);
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
    if (ctx->failed)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;

            uint8_t required = xi_op_default_effects(v->op);
            uint8_t missing = required & ~v->flags;
            if (missing) {
                if (v->op == XI_GET_BUILTIN) {
                    fprintf(stderr,
                            "[xi_diag] verify GET_BUILTIN v%u flags=0x%02x ptr=%p aux_int=%lld "
                            "aux=%p\n",
                            v->id, v->flags, (void *) v, (long long) v->aux_int, v->aux);
                }
                verr(ctx,
                     "func '%s': v%u %s in b%u missing required "
                     "effect flags: has=0x%02x need=0x%02x missing=0x%02x",
                     f->name, v->id, xi_op_name(v->op), blk->id, v->flags, required, missing);
                return;
            }
        }
    }
}

/* ========== Check 11b: Constructive Error Checks (task 216) ========== */

/* Error checks are generated CONSTRUCTIVELY by callee effect: xi_lower emits an
 * XI_ERR_CHECK only immediately after a producer that may raise, in the same
 * block. This verifies the machine-checkable form of "a NO_THROW callsite has no
 * error check": every block that holds an XI_ERR_CHECK must also hold a producer
 * that may throw (a value flagged MAY_THROW, or an XI_SCOPE_EXIT — a linked
 * scope re-raises the first child failure through the error channel without
 * carrying the flag on the exit value).
 *
 * Gated to the CLOSED stage only: that is the freshly lowered shape verified at
 * the pre-optimization barriers (before escape/arc insertion and before the
 * optimizer, which legitimately relocates error checks across blocks and folds
 * proven-nothrow producers). Enforcing it later would reject valid optimizer
 * output. */
static void verify_error_check_producers(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;
    if (f->stage != XI_STAGE_CLOSED)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        bool has_err_check = (blk->control && blk->control->op == XI_ERR_CHECK);
        bool has_producer = false;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (v->op == XI_ERR_CHECK)
                has_err_check = true;
            else if ((v->flags & XI_FLAG_MAY_THROW) || v->op == XI_SCOPE_EXIT)
                has_producer = true;
        }
        if (has_err_check && !has_producer) {
            verr(ctx,
                 "func '%s': b%u holds XI_ERR_CHECK with no may-throw producer "
                 "(task 216: error checks are generated by callee effect)",
                 f->name, blk->id);
            return;
        }
    }
}

/* ========== Check 12: XI_CALL_METHOD aux_int Contract ========== */

/* XI_CALL_METHOD.aux_int encodes (global_symbol_id << 1) | is_super.
 * A zero symbol_id means the lowerer failed to resolve the method name
 * at lowering time (only valid when isolate is NULL during AOT).
 * The is_super bit must be 0 or 1.  aux (method name) must be non-NULL. */
static void verify_call_method_contract(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_CALL_METHOD)
                continue;

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
            int is_super = (int) (ai & 1);
            int64_t sym_id = ai >> 1;
            (void) is_super; /* always valid: 0 or 1 */

            /* symbol_id must be non-negative */
            if (sym_id < 0) {
                verr(ctx,
                     "func '%s': XI_CALL_METHOD v%u in b%u has negative "
                     "symbol_id=%lld (aux_int=%lld)",
                     f->name, v->id, blk->id, (long long) sym_id, (long long) ai);
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

static void verify_call_method_direct_contract(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_CALL_METHOD_DIRECT)
                continue;
            if (!v->aux) {
                verr(ctx,
                     "func '%s': XI_CALL_METHOD_DIRECT v%u in b%u has NULL aux "
                     "(expected method name string)",
                     f->name, v->id, blk->id);
                return;
            }
            if (v->nargs < 1) {
                verr(ctx,
                     "func '%s': XI_CALL_METHOD_DIRECT v%u in b%u has 0 args "
                     "(needs at least receiver)",
                     f->name, v->id, blk->id);
                return;
            }
            if (v->aux_int < 0 || v->aux_int > 255) {
                verr(ctx,
                     "func '%s': XI_CALL_METHOD_DIRECT v%u in b%u has invalid "
                     "method index=%lld",
                     f->name, v->id, blk->id, (long long) v->aux_int);
                return;
            }
            if (v->nargs - 1 > 127) {
                verr(ctx,
                     "func '%s': XI_CALL_METHOD_DIRECT v%u in b%u has too many "
                     "arguments=%u",
                     f->name, v->id, blk->id, (unsigned) (v->nargs - 1));
                return;
            }
        }
    }
}

/* ========== Check 13: ref/move call plans ========== */

static bool verify_is_call_bound_place(const XiValue *v) {
    if (!v)
        return false;
    if (v->op == XI_LOCAL_ADDR)
        return true;
    if (v->op != XI_PARAM || !v->block || !v->block->func || v->aux_int < 0 ||
        v->aux_int > UINT16_MAX)
        return false;
    XrParamMode mode = xi_func_param_passing_mode(v->block->func, (uint16_t) v->aux_int);
    if (mode == XR_PARAM_REF)
        return true;
    if (xi_value_is_read_place_param(v))
        return true;
    const XiFunc *func = v->block->func;
    return mode == XR_PARAM_READ && v->aux_int == 0 && func->receiver_call_place && func->params &&
           func->params[0] == v;
}

static bool verify_arg_access_matches_mode(XrParamMode mode, XrCallArgAccess access) {
    if (mode == XR_PARAM_READ)
        return access == XR_CALL_ARG_PLAIN;
    if (mode == XR_PARAM_REF)
        return access == XR_CALL_ARG_REF;
    return mode == XR_PARAM_MOVE && (access == XR_CALL_ARG_MOVE || access == XR_CALL_ARG_PLAIN);
}

/* Representation selection may insert a BOX/UNBOX bridge between the
 * source-level ownership edge and a tagged call boundary.  The move proof is
 * semantic, so follow only those representation wrappers; do not accept a
 * general COPY as a substitute for an explicit source move. */
static bool verify_call_arg_has_source_move(const XiValue *value) {
    for (uint8_t depth = 0; value && depth < 8; depth++) {
        if (value->op == XI_SOURCE_MOVE)
            return true;
        if (value->nargs != 1 || !value->args[0])
            return false;
        switch ((XiOp) value->op) {
            case XI_BOX:
            case XI_UNBOX:
            case XI_ENUM_DESCRIPTOR_BOX:
            case XI_ENUM_DESCRIPTOR_UNBOX:
                value = value->args[0];
                break;
            default:
                return false;
        }
    }
    return false;
}

static bool verify_call_plan_receiver(VerifyCtx *ctx, const XiFunc *f, const XiValue *call,
                                      const XiCallArgPlan *receiver) {
    XiValue *place = receiver->place;
    bool receiver_mode =
        receiver->param_mode == XR_PARAM_READ || receiver->param_mode == XR_PARAM_REF;
    if (!receiver_mode || receiver->access != XR_CALL_ARG_PLAIN || !receiver->addressable ||
        receiver->lifetime != XI_PLACE_LIFETIME_CALL_BOUND ||
        receiver->escape != XI_PLACE_ESCAPE_NONE || !place || place != call->args[0] ||
        !verify_is_call_bound_place(place)) {
        verr(ctx, "func '%s': call v%u receiver is not a verified nonescaping call-bound place",
             f->name, call->id);
        return false;
    }
    if (place->op == XI_LOCAL_ADDR) {
        if (place->nargs != 1 || !place->args[0] ||
            (receiver->origin != XI_PLACE_ORIGIN_STACK_LOCAL &&
             receiver->origin != XI_PLACE_ORIGIN_PROJECTION_TEMP)) {
            verr(ctx, "func '%s': call v%u receiver has invalid local-place origin", f->name,
                 call->id);
            return false;
        }
        bool source_var = receiver->origin == XI_PLACE_ORIGIN_STACK_LOCAL;
        if (source_var != xi_var_id_is_valid(receiver->origin_var_id) ||
            (source_var && receiver->origin_var_id >= f->source_var_count)) {
            verr(ctx, "func '%s': call v%u receiver has inconsistent local origin variable",
                 f->name, call->id);
            return false;
        }
    } else if (receiver->origin != XI_PLACE_ORIGIN_PARAM ||
               !xi_var_id_is_valid(receiver->origin_var_id)) {
        verr(ctx, "func '%s': call v%u receiver has invalid parameter origin", f->name, call->id);
        return false;
    }
    return true;
}

static bool verify_call_plan_value(VerifyCtx *ctx, const XiFunc *f, const XiBlock *blk,
                                   XiValue *v) {
    const XiCallPlan *plan = v->call_plan;
    bool call_op = v->op == XI_CALL || v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT;
    if (!plan) {
        if (!call_op)
            return true;
        for (uint16_t a = 0; a < v->nargs; a++) {
            if (verify_is_call_bound_place(v->args[a])) {
                verr(ctx,
                     "func '%s': call v%u in b%u passes call-bound place v%u without a call plan",
                     f->name, v->id, blk->id, v->args[a]->id);
                return false;
            }
        }
        return true;
    }
    if (!call_op) {
        verr(ctx, "func '%s': non-call v%u %s in b%u carries a call plan", f->name, v->id,
             xi_op_name(v->op), blk->id);
        return false;
    }
    if (v->nargs < 1 || plan->nargs != (uint16_t) (v->nargs - 1)) {
        verr(ctx, "func '%s': call v%u in b%u plan nargs=%u does not match call nargs=%u", f->name,
             v->id, blk->id, (unsigned) plan->nargs, v->nargs > 0 ? (unsigned) (v->nargs - 1) : 0u);
        return false;
    }
    if (!plan->verified || (!plan->has_receiver && plan->nargs == 0) ||
        (plan->nargs > 0 && !plan->args)) {
        verr(ctx, "func '%s': call v%u in b%u has an unverified or empty call plan", f->name, v->id,
             blk->id);
        return false;
    }

    bool saw_contract = false;
    if (plan->has_receiver) {
        if (!verify_call_plan_receiver(ctx, f, v, &plan->receiver))
            return false;
        saw_contract = true;
    }
    for (uint16_t a = 0; a < plan->nargs; a++) {
        const XiCallArgPlan *arg_plan = &plan->args[a];
        if (!xr_param_mode_is_valid(arg_plan->param_mode) ||
            !xr_call_arg_access_is_valid(arg_plan->access)) {
            verr(ctx, "func '%s': call v%u plan arg %u has invalid mode/access", f->name, v->id,
                 (unsigned) a + 1);
            return false;
        }
        if (!verify_arg_access_matches_mode(arg_plan->param_mode, arg_plan->access)) {
            verr(ctx, "func '%s': call v%u plan arg %u mode=%s has incompatible access=%s", f->name,
                 v->id, (unsigned) a + 1, xr_param_mode_label(arg_plan->param_mode),
                 xr_call_arg_access_label(arg_plan->access));
            return false;
        }
        if (arg_plan->param_mode == XR_PARAM_READ && !arg_plan->place) {
            if (arg_plan->addressable || arg_plan->origin != XI_PLACE_ORIGIN_NONE ||
                arg_plan->lifetime != XI_PLACE_LIFETIME_NONE ||
                arg_plan->escape != XI_PLACE_ESCAPE_NONE ||
                arg_plan->origin_var_id != XI_NO_VAR_ID) {
                verr(ctx, "func '%s': call v%u read arg %u has incomplete place metadata", f->name,
                     v->id, (unsigned) a + 1);
                return false;
            }
            continue;
        }
        saw_contract = true;
        if (arg_plan->param_mode == XR_PARAM_MOVE) {
            if (arg_plan->place || arg_plan->addressable ||
                arg_plan->origin != XI_PLACE_ORIGIN_NONE ||
                arg_plan->lifetime != XI_PLACE_LIFETIME_NONE ||
                arg_plan->escape != XI_PLACE_ESCAPE_NONE ||
                arg_plan->origin_var_id != XI_NO_VAR_ID) {
                verr(ctx, "func '%s': call v%u move arg %u carries ref-place metadata", f->name,
                     v->id, (unsigned) a + 1);
                return false;
            }
            if (arg_plan->access == XR_CALL_ARG_MOVE &&
                !verify_call_arg_has_source_move(v->args[a + 1])) {
                verr(ctx, "func '%s': call v%u move arg %u lacks source-move IR", f->name, v->id,
                     (unsigned) a + 1);
                return false;
            }
            continue;
        }

        XiValue *place = arg_plan->place;
        if (!arg_plan->addressable || arg_plan->lifetime != XI_PLACE_LIFETIME_CALL_BOUND ||
            arg_plan->escape != XI_PLACE_ESCAPE_NONE || !place || place != v->args[a + 1] ||
            !verify_is_call_bound_place(place)) {
            verr(ctx, "func '%s': call v%u plan arg %u is not a nonescaping call-bound place",
                 f->name, v->id, (unsigned) a + 1);
            return false;
        }
        if (place->op == XI_LOCAL_ADDR) {
            if (place->nargs != 1 || !place->args[0] ||
                (arg_plan->origin != XI_PLACE_ORIGIN_STACK_LOCAL &&
                 arg_plan->origin != XI_PLACE_ORIGIN_PROJECTION_TEMP)) {
                verr(ctx, "func '%s': call v%u plan arg %u has invalid local-place origin", f->name,
                     v->id, (unsigned) a + 1);
                return false;
            }
            bool source_var = arg_plan->origin == XI_PLACE_ORIGIN_STACK_LOCAL;
            if (source_var != xi_var_id_is_valid(arg_plan->origin_var_id) ||
                (source_var && arg_plan->origin_var_id >= f->source_var_count)) {
                verr(ctx, "func '%s': call v%u plan arg %u has inconsistent local origin variable",
                     f->name, v->id, (unsigned) a + 1);
                return false;
            }
        } else if (arg_plan->origin != XI_PLACE_ORIGIN_PARAM ||
                   !xi_var_id_is_valid(arg_plan->origin_var_id)) {
            verr(ctx, "func '%s': call v%u plan arg %u has invalid parameter origin", f->name,
                 v->id, (unsigned) a + 1);
            return false;
        }
    }
    if (!saw_contract) {
        verr(ctx, "func '%s': call v%u in b%u has a call plan with no place/ref/move contract",
             f->name, v->id, blk->id);
        return false;
    }
    return true;
}

static void verify_call_plans(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;
    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (v && !verify_call_plan_value(ctx, f, blk, v))
                return;
        }
    }
}

static bool verify_call_plan_accepts_place_at(const XiValue *user, uint16_t arg_index,
                                              const XiValue *place) {
    if (!user || !user->call_plan || arg_index >= user->nargs)
        return false;
    if (arg_index == 0)
        /* Read receivers are also represented by a call-bound place so the
         * callee can borrow a stable address without copying a large value.
         * The receiver plan already proves addressability and noescape. */
        return user->call_plan->has_receiver && user->call_plan->receiver.place == place;
    if (arg_index > user->call_plan->nargs)
        return false;
    const XiCallArgPlan *arg_plan = &user->call_plan->args[arg_index - 1];
    return (arg_plan->param_mode == XR_PARAM_REF || arg_plan->param_mode == XR_PARAM_READ) &&
           arg_plan->place == place;
}

static void verify_place_uses(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *place = blk->values[i];
            if (!verify_is_call_bound_place(place))
                continue;
            if (place->op == XI_LOCAL_ADDR && (place->nargs != 1 || !place->args[0] ||
                                               verify_is_call_bound_place(place->args[0]))) {
                verr(ctx, "func '%s': XI_LOCAL_ADDR v%u in b%u has invalid source", f->name,
                     place->id, blk->id);
                return;
            }
            if (place->op == XI_PARAM && (place->aux_int < 0 || place->aux_int >= f->nparams)) {
                verr(ctx, "func '%s': call-bound XI_PARAM v%u has invalid index=%lld", f->name,
                     place->id, (long long) place->aux_int);
                return;
            }

            for (uint32_t ub = 0; ub < f->nblocks && !ctx->failed; ub++) {
                XiBlock *user_blk = f->blocks[ub];
                if (!user_blk)
                    continue;
                if (user_blk->control == place) {
                    verr(ctx, "func '%s': call-bound place v%u escapes through block control",
                         f->name, place->id);
                    return;
                }
                for (XiPhi *phi = user_blk->phis; phi; phi = phi->next) {
                    for (uint16_t a = 0; a < phi->value.nargs; a++) {
                        if (phi->value.args[a] == place) {
                            verr(ctx, "func '%s': call-bound place v%u escapes through phi v%u",
                                 f->name, place->id, phi->value.id);
                            return;
                        }
                    }
                }
                for (uint32_t ui = 0; ui < user_blk->nvalues; ui++) {
                    XiValue *user = user_blk->values[ui];
                    if (!user)
                        continue;
                    for (uint16_t a = 0; a < user->nargs; a++) {
                        if (user->args[a] != place)
                            continue;
                        bool allowed = false;
                        if (user->op == XI_PLACE_LOAD && a == 0) {
                            allowed = true;
                        } else if (user->op == XI_PLACE_STORE && a == 0) {
                            allowed = true;
                        } else if ((user->op == XI_CALL || user->op == XI_CALL_METHOD ||
                                    user->op == XI_CALL_METHOD_DIRECT) &&
                                   verify_call_plan_accepts_place_at(user, a, place)) {
                            allowed = true;
                        }
                        if (!allowed) {
                            verr(ctx, "func '%s': call-bound place v%u escapes to v%u %s arg %u",
                                 f->name, place->id, user->id, xi_op_name(user->op), (unsigned) a);
                            return;
                        }
                    }
                }
            }
        }
    }

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || (v->op != XI_PLACE_LOAD && v->op != XI_PLACE_STORE))
                continue;
            if (v->nargs < 1 || !verify_is_call_bound_place(v->args[0])) {
                verr(ctx, "func '%s': v%u %s does not consume a call-bound place", f->name, v->id,
                     xi_op_name(v->op));
                return;
            }
        }
    }
}

static void verify_place_suspend_intervals(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;

    bool needs_liveness = false;
    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (v->call_plan && xi_coro_is_suspend_point(f, v, NULL)) {
                verr(ctx,
                     "func '%s': call v%u carries a call-bound place across a suspension point",
                     f->name, v->id);
                break;
            }
            if ((v->op == XI_PARAM && verify_is_call_bound_place(v)) ||
                (v->op == XI_PLACE_LOAD && xi_own_type_may_be_ref(v->type)))
                needs_liveness = true;
        }
    }
    if (ctx->failed || !needs_liveness)
        return;

    XiLiveness *live = xi_compute_liveness((XiFunc *) f);
    if (!live) {
        verr(ctx, "func '%s': cannot compute call-bound place liveness", f->name);
        return;
    }

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;

            if (v->op == XI_PARAM && verify_is_call_bound_place(v) &&
                xi_coro_value_live_across_suspend(f, live, v, NULL)) {
                verr(ctx,
                     "func '%s': call-bound parameter place v%u is live across a suspension "
                     "point",
                     f->name, v->id);
                break;
            }

            if (v->op == XI_PLACE_LOAD && v->nargs == 1 && v->args[0] &&
                xi_own_type_may_be_ref(v->type) &&
                xi_coro_value_live_across_suspend(f, live, v, NULL)) {
                verr(ctx, "func '%s': borrowed place load v%u is live across a suspension point",
                     f->name, v->id);
                break;
            }
        }
    }

    xi_liveness_free(live);
}

/* ========== Check 14: Tail Call Safety ========== */

/* XI_FLAG_TAIL may only appear on call ops.
 * XI_CALL with tail flag must either be a self-call (aux_int & 0xFF == 1)
 * or the callee must be typed as a function.  Class constructors etc.
 * are not safe tail-call targets because OP_TAILCALL only handles closures. */
static void verify_tail_calls(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v || !(v->flags & XI_FLAG_TAIL))
                continue;

            /* Only call ops may carry tail flag */
            if (v->op != XI_CALL && v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) {
                verr(ctx,
                     "func '%s': v%u %s in b%u has XI_FLAG_TAIL "
                     "but is not a call op",
                     f->name, v->id, xi_op_name(v->op), blk->id);
                return;
            }

            /* XI_CALL: must be self-call or callee typed as function */
            if (v->op == XI_CALL) {
                bool is_self = (v->aux_int & 0xFF) == 1;
                bool callee_is_func = v->nargs >= 1 && v->args[0] && v->args[0]->type &&
                                      v->args[0]->type->kind == XR_KIND_FUNCTION;
                if (!is_self && !callee_is_func) {
                    verr(ctx,
                         "func '%s': XI_CALL v%u in b%u has XI_FLAG_TAIL "
                         "but callee is not a function (kind=%u) and "
                         "not a self-call",
                         f->name, v->id, blk->id,
                         v->args[0] && v->args[0]->type ? v->args[0]->type->kind : 0);
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
 * BOX must produce TAGGED. UNBOX must produce I64 or F64. */
static void verify_repped(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;
    if (f->stage < XI_STAGE_REPPED)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;

        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;

            /* Rep must be a known value */
            if (v->rep >= XR_REP_COUNT) {
                verr(ctx, "func '%s': v%u %s in b%u has invalid rep %u", f->name, v->id,
                     xi_op_name(v->op), blk->id, v->rep);
                return;
            }

            /* BOX must produce TAGGED */
            if (v->op == XI_BOX && v->rep != XR_REP_TAGGED) {
                verr(ctx, "func '%s': v%u BOX in b%u has rep %u, expected TAGGED", f->name, v->id,
                     blk->id, v->rep);
                return;
            }
            if (v->op == XI_ENUM_DESCRIPTOR_BOX && v->rep != XR_REP_TAGGED) {
                verr(ctx, "func '%s': v%u ENUM_DESCRIPTOR_BOX in b%u has rep %u, expected TAGGED",
                     f->name, v->id, blk->id, v->rep);
                return;
            }

            /* UNBOX must produce a native boundary rep or remain tagged if no unbox exists. */
            if (v->op == XI_UNBOX && v->rep != XR_REP_I64 && v->rep != XR_REP_F64 &&
                v->rep != XR_REP_PTR && v->rep != XR_REP_RAWPTR && v->rep != XR_REP_TAGGED) {
                verr(ctx, "func '%s': v%u UNBOX in b%u has invalid rep %u", f->name, v->id, blk->id,
                     v->rep);
                return;
            }
            if (v->op == XI_ENUM_DESCRIPTOR_UNBOX && v->rep != XR_REP_I64) {
                verr(ctx, "func '%s': v%u ENUM_DESCRIPTOR_UNBOX in b%u has rep %u, expected I64",
                     f->name, v->id, blk->id, v->rep);
                return;
            }
        }

        /* Phi nodes follow backend policy: VM-style pipelines can keep them
         * tagged, while AOT can keep scalar phis native. */
        for (XiPhi *phi = blk->phis; phi && !ctx->failed; phi = phi->next) {
            if (phi->value.rep >= XR_REP_COUNT) {
                verr(ctx, "func '%s': phi v%u in b%u has invalid rep %u", f->name, phi->value.id,
                     blk->id, phi->value.rep);
                return;
            }
        }
    }
}

/* ========== Check 16: Backend Op Legality (STAGE_BACKEND) ========== */

/* At STAGE_BACKEND, all ops must be in the backend-legal whitelist.
 * Non-legal ops should have been lowered by xi_backend_lower(). */
static void verify_backend(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;
    if (f->stage < XI_STAGE_BACKEND)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;

        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;

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
    if (ctx->failed)
        return;
    (void) f;
    /* RAW is the baseline stage. All structural checks are already
     * covered by the generic verify_*() helpers above. */
}

/* CANONICAL: evaluation order is deterministic; syntax sugar expanded.
 * No compound-assignment or increment/decrement ops should remain
 * as high-level ops. */
static void verify_canonical(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;
    if (f->stage < XI_STAGE_CANONICAL)
        return;
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
    if (ctx->failed)
        return;
    if (f->stage < XI_STAGE_CLOSED)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;

        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;

            if (v->op == XI_LOAD_UPVAL || v->op == XI_STORE_UPVAL) {
                int idx = v->aux_int;
                if (idx < 0 || idx >= (int) f->ncaptures) {
                    verr(ctx,
                         "func '%s': v%u %s in b%u has upval index %d "
                         "but function has %u captures",
                         f->name, v->id, xi_op_name(v->op), blk->id, idx, f->ncaptures);
                    return;
                }
            }
        }
    }
}

/* OWNED: escape analysis has run; every allocation op carries a
 * valid escape annotation; ownership forwarding preserves value types. */
static void verify_owned(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;
    if (f->stage < XI_STAGE_OWNED)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;

        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;

            /* Allocation ops must have escape level in [0,3] */
            if (xi_op_allocates(v->op)) {
                if (v->escape > 3) {
                    verr(ctx,
                         "func '%s': v%u %s in b%u has invalid escape "
                         "level %u (expected 0-3)",
                         f->name, v->id, xi_op_name(v->op), blk->id, v->escape);
                    return;
                }
            }

            /* RC ops must reference a value */
            if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && (v->nargs < 1 || !v->args[0])) {
                verr(ctx, "func '%s': v%u %s in b%u has no argument", f->name, v->id,
                     xi_op_name(v->op), blk->id);
                return;
            }

            /* XI_OWNER_FORWARD is an ownership-edge marker, not an SSA invalidation:
             * a preceding RETAIN, a borrowed call result, or a branch-local
             * consume can legitimately leave the same SSA carrier usable.
             * The old same-block "no later use" heuristic rejected valid ARC
             * output while missing cross-block errors.  Verify only the local
             * executable contract here; the ARC ownership proof owns the
             * global consume accounting. */
            if (v->op == XI_OWNER_FORWARD && v->nargs >= 1 && v->args[0]) {
                XiValue *moved = v->args[0];
                if (!v->type || !moved->type ||
                    (v->type->kind != XR_KIND_UNKNOWN && moved->type->kind != XR_KIND_UNKNOWN &&
                     v->type->kind != moved->type->kind &&
                     !xr_type_assignable(v->type, moved->type))) {
                    verr(ctx, "func '%s': XI_OWNER_FORWARD v%u in b%u changes value type", f->name,
                         v->id, blk->id);
                    return;
                }
            }
        }
    }
}

/* LOWERED: target-independent coroutine/callable and semantic lowering facts
 * are explicit. Selective lowering is represented per function, not as a
 * skippable global stage. */
static void verify_lowered(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed || f->stage < XI_STAGE_LOWERED)
        return;
    const XiLoweringFacts *facts = &f->lowering_facts;
    if (!facts->initialized) {
        verr(ctx, "func '%s': Lowered stage has no XiLoweringFacts", f->name);
        return;
    }
    if (!facts->semantic_ops_lowered) {
        verr(ctx, "func '%s': target-independent semantic lowering is incomplete", f->name);
        return;
    }
    if (facts->coroutine_required && !facts->coroutine_lowered) {
        verr(ctx, "func '%s': required coroutine lowering is incomplete", f->name);
        return;
    }
    if (facts->callable_required && !facts->callable_lowered) {
        verr(ctx, "func '%s': required callable lowering is incomplete", f->name);
        return;
    }

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (v->xa_intrinsic_id != XA_INTRINSIC_NONE) {
                char intrinsic_error[192] = {0};
                if (!xi_semantic_intrinsic_verify_value(v, f->stage, intrinsic_error,
                                                        sizeof(intrinsic_error))) {
                    verr(ctx, "func '%s': v%u in b%u violates semantic intrinsic contract: %s",
                         f->name, v->id, blk->id, intrinsic_error);
                    return;
                }
            } else if ((xi_op_class(v->op) == XI_GEN_CLASS_VECTOR &&
                        xi_vec_shape_is_explicit(v->aux_int)) ||
                       v->op == XI_BIT_ROTL || v->op == XI_BIT_ROTR || v->op == XI_BIT_BSWAP ||
                       v->op == XI_BIT_POPCOUNT || v->op == XI_BIT_CLZ || v->op == XI_BIT_CTZ ||
                       v->op == XI_BIT_MUL_HIGH) {
                verr(ctx,
                     "func '%s': canonical semantic op v%u %s in b%u has no intrinsic identity",
                     f->name, v->id, xi_op_name(v->op), blk->id);
                return;
            }
        }
    }
}

/* ========== Check 17: NARROW Required Before Typed-Array Store ========== */

static bool native_width_needs_narrow(uint8_t native_width) {
    switch (native_width) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_F32:
            return true;
        default:
            return false;
    }
}

static const char *native_width_result_name(uint8_t native_width) {
    switch (native_width) {
        case XR_NATIVE_I8:
            return "i8";
        case XR_NATIVE_U8:
            return "u8";
        case XR_NATIVE_I16:
            return "i16";
        case XR_NATIVE_U16:
            return "u16";
        case XR_NATIVE_I32:
            return "i32";
        case XR_NATIVE_U32:
            return "u32";
        case XR_NATIVE_F32:
            return "f32";
        default:
            return NULL;
    }
}

static bool value_has_native_width_result(const XiValue *v, uint8_t native_width) {
    const char *expected = native_width_result_name(native_width);
    const char *actual = v ? xi_generated_op_result_native_type(v->op) : NULL;
    return expected && actual && strcmp(expected, actual) == 0;
}

/* XI_INDEX_SET on a sub-width typed array (Array<int8>, Array<uint16>, etc.)
 * must have a value argument whose op declares the matching native result width.
 * Without width-specific narrowing, a full-width int64/f64 can be stored into a
 * narrow slot, silently losing high bits at the VM level but not at AOT. */
static void verify_narrow_before_typed_store(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed)
        return;

    for (uint32_t b = 0; b < f->nblocks && !ctx->failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;

        for (uint32_t i = 0; i < blk->nvalues && !ctx->failed; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_INDEX_SET)
                continue;
            if (v->nargs < 3 || !v->args[0] || !v->args[2])
                continue;

            /* Check if the collection is a typed array */
            struct XrType *coll_type = v->args[0]->type;
            if (!coll_type ||
                (coll_type->kind != XR_KIND_ARRAY && coll_type->kind != XR_KIND_SLICE &&
                 coll_type->kind != XR_KIND_SLICE))
                continue;

            struct XrType *elem = coll_type->container.element_type;
            if (!elem || !native_width_needs_narrow(elem->native_width))
                continue;

            /* Sub-width element: args[2] must produce the exact native width. */
            XiValue *val = v->args[2];
            XR_DCHECK(val != NULL, "verify: INDEX_SET val arg is NULL");
            const char *expected = native_width_result_name(elem->native_width);
            const char *actual = xi_generated_op_result_native_type(val->op);
            if (!value_has_native_width_result(val, elem->native_width)) {
                verr(ctx,
                     "func '%s': XI_INDEX_SET v%u in b%u stores to "
                     "sub-width typed array (native_width=%u, expected %s) but "
                     "value v%u (op %s, native result %s) does not match",
                     f->name, v->id, blk->id, elem->native_width, expected ? expected : "none",
                     val->id, xi_op_name(val->op), actual ? actual : "none");
                return;
            }
        }
    }
}

/* ========== Check 18: Coroutine Plan Consistency ========== */

static bool coro_plan_value_belongs_to_func(const XiFunc *f, const XiValue *target) {
    if (!f || !target)
        return false;

    for (uint16_t i = 0; i < f->nparams; i++) {
        if (f->params && f->params[i] == target)
            return true;
    }

    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        if (blk->control == target)
            return true;
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (&phi->value == target)
                return true;
        }
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i] == target)
                return true;
        }
    }

    return false;
}

static bool coro_plan_has_slot_for_value(const XiCoroPlan *plan, const XiValue *value) {
    if (!plan || !value || !plan->slots)
        return false;
    for (uint32_t i = 0; i < plan->nslots; i++) {
        if (plan->slots[i].value == value)
            return true;
    }
    return false;
}

static void verify_coro_plan(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed || !f->coro_plan)
        return;

    const XiCoroPlan *plan = f->coro_plan;
    if (plan->is_coroutine != (plan->nstates > 0)) {
        verr(ctx,
             "func '%s': coro plan is_coroutine=%u but nstates=%u "
             "(expected is_coroutine == nstates>0)",
             f->name, (unsigned) plan->is_coroutine, plan->nstates);
        return;
    }
    if (plan->nstates > 0 && !plan->points) {
        verr(ctx, "func '%s': coro plan has %u states but NULL points", f->name, plan->nstates);
        return;
    }
    if (plan->nslots > 0 && !plan->slots) {
        verr(ctx, "func '%s': coro plan has %u slots but NULL slots", f->name, plan->nslots);
        return;
    }

    for (uint32_t i = 0; i < plan->nstates; i++) {
        const XiCoroSuspendPoint *point = &plan->points[i];
        if (point->state_id != i + 1) {
            verr(ctx, "func '%s': coro point[%u] has state_id=%u (expected %u)", f->name, i,
                 point->state_id, i + 1);
            return;
        }
        if (!point->op) {
            verr(ctx, "func '%s': coro point[%u] has NULL op", f->name, i);
            return;
        }
        if (!coro_plan_value_belongs_to_func(f, point->op)) {
            verr(ctx, "func '%s': coro point[%u] op v%u does not belong to function", f->name, i,
                 point->op->id);
            return;
        }
        if (point->kind > XI_CORO_SUSP_CALL) {
            verr(ctx, "func '%s': coro point[%u] has invalid kind %u", f->name, i,
                 (unsigned) point->kind);
            return;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (plan->points[j].op == point->op) {
                verr(ctx, "func '%s': coro point[%u] duplicates point[%u] op v%u", f->name, i, j,
                     point->op->id);
                return;
            }
        }
        if (point->nlive > 0 && !point->live) {
            verr(ctx, "func '%s': coro point[%u] has %u live values but NULL live set", f->name, i,
                 point->nlive);
            return;
        }
        for (uint32_t j = 0; j < point->nlive; j++) {
            XiValue *live = point->live[j];
            if (!live) {
                verr(ctx, "func '%s': coro point[%u] live[%u] is NULL", f->name, i, j);
                return;
            }
            if (!coro_plan_value_belongs_to_func(f, live)) {
                verr(ctx, "func '%s': coro point[%u] live[%u] v%u does not belong to function",
                     f->name, i, j, live->id);
                return;
            }
            if (!coro_plan_has_slot_for_value(plan, live)) {
                verr(ctx, "func '%s': coro point[%u] live[%u] v%u has no frame slot", f->name, i, j,
                     live->id);
                return;
            }
        }
    }

    uint32_t roots = 0;
    uint32_t releases = 0;
    for (uint32_t i = 0; i < plan->nslots; i++) {
        const XiCoroSlot *slot = &plan->slots[i];
        if (!slot->value) {
            verr(ctx, "func '%s': coro slot[%u] has NULL value", f->name, i);
            return;
        }
        if (!coro_plan_value_belongs_to_func(f, slot->value)) {
            verr(ctx, "func '%s': coro slot[%u] v%u does not belong to function", f->name, i,
                 slot->value->id);
            return;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (plan->slots[j].value == slot->value) {
                verr(ctx, "func '%s': coro slot[%u] duplicates slot[%u] value v%u", f->name, i, j,
                     slot->value->id);
                return;
            }
        }
        if (slot->kind > XI_CORO_SLOT_VALUE) {
            verr(ctx, "func '%s': coro slot[%u] has invalid kind %u", f->name, i,
                 (unsigned) slot->kind);
            return;
        }
        if (slot->type != slot->value->type) {
            verr(ctx, "func '%s': coro slot[%u] v%u type does not match value type", f->name, i,
                 slot->value->id);
            return;
        }
        if (slot->logical_rep >= XR_REP_COUNT) {
            verr(ctx, "func '%s': coro slot[%u] v%u has invalid logical_rep %u", f->name, i,
                 slot->value->id, (unsigned) slot->logical_rep);
            return;
        }
        if (slot->frame_root && !slot->is_root) {
            verr(ctx, "func '%s': coro slot[%u] v%u is frame_root without is_root", f->name, i,
                 slot->value->id);
            return;
        }
        if (slot->frame_release && !slot->needs_release) {
            verr(ctx, "func '%s': coro slot[%u] v%u is frame_release without needs_release",
                 f->name, i, slot->value->id);
            return;
        }
        if (slot->frame_release && !slot->live_across) {
            verr(ctx, "func '%s': coro slot[%u] v%u is frame_release without live_across", f->name,
                 i, slot->value->id);
            return;
        }
        if (slot->frame_root)
            roots++;
        if (slot->frame_release)
            releases++;
    }

    if (plan->root_count != roots) {
        verr(ctx, "func '%s': coro root_count=%u but slots contain %u frame roots", f->name,
             plan->root_count, roots);
        return;
    }
    if (plan->release_count != releases) {
        verr(ctx, "func '%s': coro release_count=%u but slots contain %u frame releases", f->name,
             plan->release_count, releases);
        return;
    }
}

/* ========== Public API ========== */

static void verify_tbaa_annotations(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed || !xi_evidence_domain_is_proven_current(f, XI_EVD_ALIAS))
        return;
    for (uint32_t bi = 0; bi < f->nblocks && !ctx->failed; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues && !ctx->failed; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            bool is_mem = xi_is_memory_op(v->op);
            if (is_mem && v->mem_group == XI_MEM_NONE) {
                verr(ctx, "v%u (%s): memory op has XI_MEM_NONE after TBAA annotation", v->id,
                     xi_op_name(v->op));
            } else if (!is_mem && !xi_is_memory_clobber(v->op) && v->op != XI_CALL &&
                       v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT &&
                       v->op != XI_CALL_BUILTIN && v->mem_group != XI_MEM_NONE) {
                /* Memory-clobber ops and calls legitimately carry a TOP mem_group. */
                verr(ctx, "v%u (%s): non-memory op has mem_group=%u (expected XI_MEM_NONE)", v->id,
                     xi_op_name(v->op), v->mem_group);
            }
        }
    }
}

XR_FUNC bool xi_verify(const XiFunc *f, char *errbuf, int errbuf_size) {
    XR_DCHECK(errbuf != NULL, "xi_verify: NULL errbuf");
    XR_DCHECK(errbuf_size > 0, "xi_verify: errbuf_size <= 0");

    if (!f) {
        snprintf(errbuf, (size_t) errbuf_size, "NULL function pointer");
        return false;
    }

    VerifyCtx ctx = {.buf = errbuf, .size = errbuf_size, .failed = false};
    errbuf[0] = '\0';

    /* Function-level */
    verify_func(&ctx, f);
    if (ctx.failed)
        return false;

    /* Entry block */
    verify_entry(&ctx, f);
    if (ctx.failed)
        return false;

    /* Per-block checks */
    for (uint32_t b = 0; b < f->nblocks && !ctx.failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) {
            verr(&ctx, "func '%s': blocks[%u] is NULL", f->name, b);
            break;
        }

        verify_block(&ctx, f, blk);
        if (ctx.failed)
            break;

        /* Values in this block */
        for (uint32_t i = 0; i < blk->nvalues && !ctx.failed; i++) {
            if (!blk->values[i]) {
                verr(&ctx, "func '%s': b%u values[%u] is NULL", f->name, blk->id, i);
                break;
            }
            verify_value(&ctx, f, blk, blk->values[i]);
            verify_ptr_memory_contract(&ctx, f, blk, blk->values[i]);
            verify_buffer_materialize_contract(&ctx, f, blk, blk->values[i]);
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

    /* Constructive error checks (task 216): no XI_ERR_CHECK without a throwing
     * producer in its block, checked on the freshly lowered (CLOSED) shape. */
    if (!ctx.failed) {
        verify_error_check_producers(&ctx, f);
    }

    /* Type contracts for bool-producing ops and XI_SELECT operands. */
    if (!ctx.failed) {
        verify_types(&ctx, f);
    }

    /* SSA dominance (requires RPO + dominator computation).
     * Cast away const: xi_compute_rpo/dominators write scratch fields
     * (rpo, idom, dom_depth) but do not modify the IR semantics. */
    if (!ctx.failed) {
        verify_dominance(&ctx, (XiFunc *) f);
    }

    /* XI_CALL_METHOD aux_int encoding contract */
    if (!ctx.failed) {
        verify_call_method_contract(&ctx, f);
    }

    if (!ctx.failed) {
        verify_call_method_direct_contract(&ctx, f);
    }

    /* Ref/out call plans and their call-bound, nonescaping place uses. */
    if (!ctx.failed) {
        verify_call_plans(&ctx, f);
    }
    if (!ctx.failed) {
        verify_place_uses(&ctx, f);
    }
    if (!ctx.failed) {
        verify_place_suspend_intervals(&ctx, f);
    }

    /* Tail call safety: only on call ops with valid callee */
    if (!ctx.failed) {
        verify_tail_calls(&ctx, f);
    }

    /* Coroutine plan consistency, when a pass has attached one. */
    if (!ctx.failed) {
        verify_coro_plan(&ctx, f);
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

    verify_tbaa_annotations(&ctx, f);

    return !ctx.failed;
}

XR_FUNC bool xi_verify_stage(const XiFunc *f, XiStage stage, char *errbuf, int errbuf_size) {
    /* Run all generic checks first */
    if (!xi_verify(f, errbuf, errbuf_size))
        return false;

    VerifyCtx ctx = {.buf = errbuf, .size = errbuf_size, .failed = false};
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
    if (!ctx.failed && stage >= XI_STAGE_LOWERED)
        verify_lowered(&ctx, f);
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
                 f->name, f->invariant_mask, missing, xi_stage_name(f->stage));
        }
    }

    return !ctx.failed;
}
