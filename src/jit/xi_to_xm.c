/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_to_xm.c - Xi IR to Xm lowering for JIT compilation
 *
 * Directly translates Xi SSA values to Xm instructions.
 * Eliminates the Braun SSA reconstruction that the bytecode builder performs,
 * since Xi IR already has SSA form with precise types on every value.
 *
 * Coverage: arithmetic, comparison, bitwise, branches, phi nodes,
 * constants, type conversion, box/unbox.
 */

#include "xi_to_xm.h"
#include "xm.h"
#include "xm_ops.h"
#include "xm_jit_runtime.h"
#include "xm_fold.h"
#include "xm_codegen.h"
#include "xm_liveness2.h"
#include "xm_offsets.h"
#include "../ir/xi_opt.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/object/xstring.h"
#include "../vm/xic_field.h"
#include "../vm/xic_field_table.h"
#include "../vm/xic_method.h"
#include "../runtime/class/xmethod.h"
#include "../runtime/closure/xclosure.h"
#include <string.h>

/* ========== Lowering Context ========== */

#define XI2XM_MAX_TRY_DEPTH 8

typedef struct {
    XiFunc *xi_func;
    XmFunc *xm_func;
    XrProto *proto;
    XiSlotMap *slot_map;
    const XmICSnapshot *ic;
    struct XrayIsolate *isolate;

    /* Xi block id → XmBlock* mapping */
    XmBlock **block_map;
    uint32_t block_map_size;

    /* Xi value id → XmRef mapping */
    XmRef *ref_map;
    uint32_t ref_map_size;

    /* EH: try nesting stack — tracks active exception handlers */
    struct {
        XmBlock *handler; /* catch (or finally) Xm block */
    } try_stack[XI2XM_MAX_TRY_DEPTH];
    int try_depth;

    /* Deopt snapshot counter (monotonically increasing) */
    uint16_t next_deopt_id;

    /* Direct-mapped index: value_id → slot_map entry index.
     * -1 = no entry.  Replaces linear scan in slot_map_bc_pc(). */
    int32_t *slot_idx;
    uint32_t slot_idx_size;

    /* Per-var_id cell pointer cache for needs_cell mutable captures.
     * When multiple closures created in this function capture the same
     * variable, all must share one Cell.  Indexed by var_id (0..255).
     * xm_ref_is_none() means no cell allocated yet. */
    XmRef cell_ref[256];
    bool cell_present[256];

    bool error;
} LowerCtx;

/* ========== Helpers ========== */

/* Check if a type is floating-point */
static bool is_float_type(struct XrType *type) {
    return type && type->kind == XR_KIND_FLOAT;
}

/* Get XmRef for a previously-lowered Xi value */
static XmRef get_ref(LowerCtx *ctx, XiValue *v) {
    XR_DCHECK(v != NULL, "get_ref: NULL value");
    XR_DCHECK(v->id < ctx->ref_map_size, "get_ref: value id out of range");
    return ctx->ref_map[v->id];
}

/* Store XmRef for an Xi value */
static void set_ref(LowerCtx *ctx, uint32_t value_id, XmRef ref) {
    XR_DCHECK(value_id < ctx->ref_map_size, "set_ref: value id out of range");
    ctx->ref_map[value_id] = ref;
}

/* Get XmBlock for an Xi block */
static XmBlock *get_block(LowerCtx *ctx, XiBlock *blk) {
    XR_DCHECK(blk != NULL, "get_block: NULL block");
    XR_DCHECK(blk->id < ctx->block_map_size, "get_block: block id out of range");
    return ctx->block_map[blk->id];
}

/* ========== Slot Map / Deopt Helpers ========== */

/* Look up the bytecode instruction offset for a given Xi value ID.
 * Returns -1 if no mapping exists (value has no IC-relevant bytecode).
 * O(1) via direct-mapped slot_idx table built at init. */
static int slot_map_bc_pc(const LowerCtx *ctx, uint32_t value_id) {
    if (!ctx->slot_idx || value_id >= ctx->slot_idx_size)
        return -1;
    int32_t idx = ctx->slot_idx[value_id];
    if (idx < 0)
        return -1;
    return (int) ctx->slot_map->entries[idx].bc_pc;
}

/* Record a deopt snapshot at the current point for a guard instruction.
 * bc_pc: bytecode PC to resume at on deopt.
 * Returns the deopt_id, or 0xFFFF on failure.
 *
 * Deduplicates by bc_slot: when multiple Xi values map to the same
 * bytecode register, only the latest (in slot_map order = RPO lowering
 * order) is kept.  This prevents stale entries from wasting slots and
 * ensures each bc_slot has exactly one unambiguous value at deopt. */
static uint16_t record_deopt(LowerCtx *ctx, uint32_t bc_pc) {
    XmFunc *func = ctx->xm_func;
    if (func->ndeopt >= XM_MAX_DEOPT_POINTS)
        return 0xFFFF;

    /* Refuse deopt without a valid bytecode anchor.  slot_map_bc_pc
     * returns -1 (UINT32_MAX once cast to uint32_t) when the Xi value
     * has no corresponding bytecode slot; pre-push and recovery would
     * then index proto->code at index 0xFFFFFFFF and crash. */
    if (bc_pc == UINT32_MAX)
        return 0xFFFF;

    uint16_t did = ctx->next_deopt_id++;

    /* Grow deopt_infos if needed */
    if (!func->deopt_infos) {
        func->deopt_infos = (XmDeoptInfo *) xr_calloc(XM_MAX_DEOPT_POINTS, sizeof(XmDeoptInfo));
        if (!func->deopt_infos)
            return 0xFFFF;
    }

    XR_DCHECK(func->ndeopt < XM_MAX_DEOPT_POINTS, "record_deopt: deopt table overflow");
    XmDeoptInfo *info = &func->deopt_infos[func->ndeopt++];
    info->bc_pc = bc_pc;
    info->deopt_id = did;
    info->nslots = 0;
    info->slots = NULL;

    if (ctx->slot_map && ctx->slot_map->count > 0) {
        /* Pass 1: deduplicate by bc_slot — keep latest entry per slot.
         * Entries later in the array are from later RPO blocks, so they
         * represent the most recent definition of that bytecode register. */
        int32_t latest_for_slot[256];
        memset(latest_for_slot, -1, sizeof(latest_for_slot));

        for (uint32_t i = 0; i < ctx->slot_map->count; i++) {
            XiSlotMapEntry *e = &ctx->slot_map->entries[i];
            if (e->value_id >= ctx->ref_map_size)
                continue;
            XmRef ref = ctx->ref_map[e->value_id];
            if (xm_ref_is_none(ref))
                continue;
            latest_for_slot[e->bc_slot] = (int32_t) i;
        }

        /* Count unique live bc_slots */
        uint32_t live_count = 0;
        for (int s = 0; s < 256; s++) {
            if (latest_for_slot[s] >= 0)
                live_count++;
        }

        if (live_count > XM_MAX_DEOPT_SLOTS) {
            /* Too many live slots — refuse this deopt point so the
             * caller falls back to the generic (unspecialized) path
             * rather than restoring a truncated snapshot. */
            func->ndeopt--;
            return 0xFFFF;
        }

        /* Pass 2: populate slots from deduplicated entries */
        XmDeoptSlot *slots =
            (XmDeoptSlot *) xr_calloc(live_count ? live_count : 1, sizeof(XmDeoptSlot));
        if (slots) {
            uint16_t ns = 0;
            for (int s = 0; s < 256; s++) {
                if (latest_for_slot[s] < 0)
                    continue;
                XiSlotMapEntry *e = &ctx->slot_map->entries[latest_for_slot[s]];
                XmRef ref = ctx->ref_map[e->value_id];
                XR_DCHECK(ns < live_count, "record_deopt: slot count mismatch");
                slots[ns].bc_slot = (int16_t) e->bc_slot;
                if (xm_ref_is_vreg(ref)) {
                    uint32_t vi = XM_REF_INDEX(ref);
                    slots[ns].rep =
                        (vi < ctx->xm_func->nvreg) ? ctx->xm_func->vregs[vi].rep : XR_REP_I64;
                } else {
                    slots[ns].rep = XR_REP_I64;
                }
                slots[ns].xr_tag = e->xr_tag;
                slots[ns].value = ref;
                ns++;
            }
            info->slots = slots;
            info->nslots = ns;
        }
    }
    return did;
}

/* ========== IC Query Helpers ========== */

/* Look up field IC for a given bytecode instruction offset.
 * Returns the IC entry if monomorphic Json shape is cached, NULL otherwise. */
static const XrICField *ic_field_lookup(const LowerCtx *ctx, int bc_pc) {
    if (!ctx->ic || !ctx->ic->ic_fields || bc_pc < 0)
        return NULL;
    XrICField *ic = xr_ic_field_table_get(ctx->ic->ic_fields, bc_pc);
    if (!ic)
        return NULL;
    /* Only speculate on monomorphic Json shape access */
    if (ic->json_shape_id == 0)
        return NULL;
    return ic;
}

/* Look up method IC for a given bytecode instruction offset.
 * Returns the IC entry if monomorphic (single klass), NULL otherwise. */
static const XrICMethod *ic_method_lookup(const LowerCtx *ctx, int bc_pc) {
    if (!ctx->ic || !ctx->ic->ic_methods || bc_pc < 0)
        return NULL;
    XrICMethod *ic = xr_ic_method_table_get(ctx->ic->ic_methods, bc_pc);
    if (!ic)
        return NULL;
    /* Only speculate on monomorphic call sites (exactly 1 klass seen) */
    if (ic->count != 1 || ic->is_megamorphic)
        return NULL;
    if (!ic->entries[0].klass || !ic->entries[0].method)
        return NULL;
    return ic;
}

/* Forward declaration: lower_closure_new may need to eagerly lower
 * capture values that appear later in the block (hoisted closures). */
static XmRef lower_value(LowerCtx *ctx, XmBlock *blk, XiValue *v);

/* ========== Constant Lowering ========== */

static XmRef lower_const(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->op == XI_CONST, "lower_const: not a constant");
    struct XrType *type = v->type;

    /* Constants must be materialized into vregs via XM_CONST_* instructions.
     * The codegen ret/branch handlers require vreg operands, not pool refs. */
    if (!type || type->kind == XR_KIND_INT || type->kind == XR_KIND_BOOL ||
        type->kind == XR_KIND_NULL) {
        XmRef cref = xm_const_i64(ctx->xm_func, v->aux_int);
        XmRef result = xm_emit_unary(ctx->xm_func, blk, XM_CONST_I64, XR_REP_I64, cref);
        // Annotate bool/null so type_prop preserves the tag distinction
        if (type && type->kind == XR_KIND_BOOL)
            blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_BOOL, 0, 0};
        else if (type && type->kind == XR_KIND_NULL)
            blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_NULL, 0, 0};
        return result;
    }
    if (type->kind == XR_KIND_FLOAT) {
        union {
            int64_t i;
            double d;
        } u;
        u.i = v->aux_int;
        XmRef cref = xm_const_f64(ctx->xm_func, u.d);
        return xm_emit_unary(ctx->xm_func, blk, XM_CONST_F64, XR_REP_F64, cref);
    }
    if (type->kind == XR_KIND_STRING) {
        /* v->aux is a raw C string from the Xi arena.  The JIT needs the
         * XrString* heap object (with GC header) so that jit_value_from_tag
         * can set heap_type correctly for runtime type checks (XR_IS_STRING).
         * Look up the matching XrString from the proto's constant pool. */
        const char *raw = (const char *) v->aux;
        void *xr_str = NULL;
        if (raw && ctx->proto) {
            XrValue *kpool = (XrValue *) ctx->proto->constants.data;
            int nk = ctx->proto->constants.count;
            for (int j = 0; j < nk; j++) {
                if (XR_IS_STRING(kpool[j])) {
                    XrString *s = XR_TO_STRING(kpool[j]);
                    if (s && strcmp(XR_STRING_CHARS(s), raw) == 0) {
                        xr_str = (void *) s;
                        break;
                    }
                }
            }
        }
        XmRef cref = xm_const_ptr(ctx->xm_func, xr_str ? xr_str : v->aux);
        return xm_emit_unary(ctx->xm_func, blk, XM_CONST_PTR, XR_REP_PTR, cref);
    }
    XmRef cref = xm_const_i64(ctx->xm_func, v->aux_int);
    return xm_emit_unary(ctx->xm_func, blk, XM_CONST_I64, XR_REP_I64, cref);
}

/* ========== Arithmetic / Bitwise Lowering ========== */

/* Get the machine rep of a ref (vreg rep or const rep). */
static uint8_t ref_rep(LowerCtx *ctx, XmRef ref) {
    if (xm_ref_is_vreg(ref)) {
        uint32_t vi = XM_REF_INDEX(ref);
        if (vi < ctx->xm_func->nvreg)
            return ctx->xm_func->vregs[vi].rep;
    } else if (xm_ref_is_const(ref)) {
        uint32_t ci = XM_REF_INDEX(ref);
        if (ci < ctx->xm_func->nconst)
            return ctx->xm_func->consts[ci].rep;
    }
    return XR_REP_I64;
}

/* Coerce a ref to I64 rep if it isn't already.
 * F64 → F2I (float-to-int); TAGGED/PTR → UNBOX_I64 (dynamic unbox). */
static XmRef coerce_to_i64(LowerCtx *ctx, XmBlock *blk, XmRef ref) {
    uint8_t rr = ref_rep(ctx, ref);
    if (rr == XR_REP_I64)
        return ref;
    if (rr == XR_REP_F64)
        return xm_emit_unary(ctx->xm_func, blk, XM_F2I, XR_REP_I64, ref);
    return xm_emit_unary(ctx->xm_func, blk, XM_UNBOX_I64, XR_REP_I64, ref);
}

/* Coerce a ref to F64 rep if it isn't already.
 * I64 → I2F (int-to-float); TAGGED/PTR → UNBOX_F64 (dynamic unbox). */
static XmRef coerce_to_f64(LowerCtx *ctx, XmBlock *blk, XmRef ref) {
    uint8_t rr = ref_rep(ctx, ref);
    if (rr == XR_REP_F64)
        return ref;
    if (rr == XR_REP_I64)
        return xm_emit_unary(ctx->xm_func, blk, XM_I2F, XR_REP_F64, ref);
    return xm_emit_unary(ctx->xm_func, blk, XM_UNBOX_F64, XR_REP_F64, ref);
}

static XmRef lower_binary_arith(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 2, "binary arith: expected 2 args");
    XmRef lhs = get_ref(ctx, v->args[0]);
    XmRef rhs = get_ref(ctx, v->args[1]);
    bool is_float = (v->rep == XR_REP_F64);
    uint8_t rep = v->rep;

    uint16_t xm_op;
    switch (v->op) {
        case XI_ADD:
            xm_op = is_float ? XM_FADD : XM_ADD;
            break;
        case XI_SUB:
            xm_op = is_float ? XM_FSUB : XM_SUB;
            break;
        case XI_MUL:
            xm_op = is_float ? XM_FMUL : XM_MUL;
            break;
        case XI_DIV:
            xm_op = is_float ? XM_FDIV : XM_DIV;
            break;
        case XI_MOD:
            xm_op = XM_MOD;
            break;
        case XI_BAND:
            xm_op = XM_AND;
            break;
        case XI_BOR:
            xm_op = XM_OR;
            break;
        case XI_BXOR:
            xm_op = XM_XOR;
            break;
        case XI_SHL:
            xm_op = XM_SHL;
            break;
        case XI_SHR:
            xm_op = XM_SHR;
            break;
        default:
            ctx->error = true;
            return xm_const_i64(ctx->xm_func, 0);
    }

    /* Float operations require F64 operands; coerce if needed
     * (e.g. Json property access returns I64/TAGGED). */
    if (is_float) {
        lhs = coerce_to_f64(ctx, blk, lhs);
        rhs = coerce_to_f64(ctx, blk, rhs);
    }

    return xm_fold_emit(ctx->xm_func, blk, xm_op, rep, lhs, rhs);
}

static XmRef lower_unary(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "unary: expected 1 arg");
    XmRef arg = get_ref(ctx, v->args[0]);
    bool is_float = (v->rep == XR_REP_F64);
    uint8_t rep = v->rep;

    uint16_t xm_op;
    switch (v->op) {
        case XI_NEG:
            xm_op = is_float ? XM_FNEG : XM_NEG;
            break;
        case XI_BNOT:
            xm_op = XM_NOT;
            break;
        case XI_NOT:
            xm_op = XM_NOT;
            break;
        default:
            ctx->error = true;
            return xm_const_i64(ctx->xm_func, 0);
    }

    /* Float unary needs F64 operand */
    if (is_float)
        arg = coerce_to_f64(ctx, blk, arg);

    return xm_fold_emit(ctx->xm_func, blk, xm_op, rep, arg, XM_NONE);
}

/* ========== Comparison Lowering ========== */

static XmRef lower_comparison(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 2, "comparison: expected 2 args");
    XmRef lhs = get_ref(ctx, v->args[0]);
    XmRef rhs = get_ref(ctx, v->args[1]);

    bool is_eq = (v->op == XI_EQ || v->op == XI_EQ_STRICT);
    bool is_ne = (v->op == XI_NE || v->op == XI_NE_STRICT);
    bool lhs_null = v->args[0]->type && v->args[0]->type->kind == XR_KIND_NULL;
    bool rhs_null = v->args[1]->type && v->args[1]->type->kind == XR_KIND_NULL;
    if ((is_eq || is_ne) && (lhs_null || rhs_null)) {
        XmRef val = lhs_null ? rhs : lhs;
        XmRef isnull = xm_emit_unary(ctx->xm_func, blk, XM_RT_ISNULL, XR_REP_I64, val);
        if (is_eq)
            return isnull;
        XmRef zero_ref = xm_const_i64(ctx->xm_func, 0);
        XmRef zero = xm_emit_unary(ctx->xm_func, blk, XM_CONST_I64, XR_REP_I64, zero_ref);
        return xm_emit(ctx->xm_func, blk, XM_EQ, XR_REP_I64, isnull, zero);
    }

    /* Determine if operands are float (check arg type, not result type) */
    bool is_float = is_float_type(v->args[0]->type);

    uint16_t xm_op;
    switch (v->op) {
        case XI_EQ:
            xm_op = is_float ? XM_FEQ : XM_EQ;
            break;
        case XI_NE:
            xm_op = is_float ? XM_FNE : XM_NE;
            break;
        case XI_LT:
            xm_op = is_float ? XM_FLT : XM_LT;
            break;
        case XI_LE:
            xm_op = is_float ? XM_FLE : XM_LE;
            break;
        case XI_GT:
            xm_op = is_float ? XM_FLT : XM_LT;
            break;
        case XI_GE:
            xm_op = is_float ? XM_FLE : XM_LE;
            break;
        case XI_EQ_STRICT:
            xm_op = XM_EQ;
            break;
        case XI_NE_STRICT:
            xm_op = XM_NE;
            break;
        default:
            ctx->error = true;
            return xm_const_i64(ctx->xm_func, 0);
    }

    /* GT/GE: swap operands (Xm only has LT/LE) */
    if (v->op == XI_GT || v->op == XI_GE) {
        XmRef tmp = lhs;
        lhs = rhs;
        rhs = tmp;
    }

    /* Float comparisons require F64 operands */
    if (is_float) {
        lhs = coerce_to_f64(ctx, blk, lhs);
        rhs = coerce_to_f64(ctx, blk, rhs);
    }

    return xm_fold_emit(ctx->xm_func, blk, xm_op, XR_REP_I64, lhs, rhs);
}

/* ========== Type Conversion Lowering ========== */

static XmRef lower_convert(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "convert: expected 1 arg");
    XmRef arg = get_ref(ctx, v->args[0]);

    bool src_float = is_float_type(v->args[0]->type);
    bool dst_float = is_float_type(v->type);

    if (dst_float) {
        /* int / tagged → float — coerce to F64 (handles I2F + UNBOX_F64) */
        return coerce_to_f64(ctx, blk, arg);
    }
    if (src_float) {
        /* float / tagged → int */
        return coerce_to_i64(ctx, blk, arg);
    }
    /* Same type — identity */
    return arg;
}

static XmRef lower_box(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "box: expected 1 arg");
    XmRef arg = get_ref(ctx, v->args[0]);
    uint8_t arg_r = ref_rep(ctx, arg);

    /* If Xm source is already TAGGED/PTR, the box is redundant
     * (xi_to_xm may have already tagged the value). */
    if (arg_r == XR_REP_TAGGED || arg_r == XR_REP_PTR)
        return arg;

    if (v->args[0]->rep == XR_REP_F64) {
        return xm_emit_unary(ctx->xm_func, blk, XM_BOX_F64, XR_REP_TAGGED, arg);
    }
    return xm_emit_unary(ctx->xm_func, blk, XM_BOX_I64, XR_REP_TAGGED, arg);
}

static XmRef lower_unbox(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "unbox: expected 1 arg");
    XmRef arg = get_ref(ctx, v->args[0]);
    uint8_t arg_r = ref_rep(ctx, arg);

    /* If Xm source is already the target scalar rep, skip the unbox.
     * If source is a different scalar rep, convert instead of unboxing. */
    if (v->rep == XR_REP_F64) {
        if (arg_r == XR_REP_F64)
            return arg;
        if (arg_r == XR_REP_I64)
            return xm_emit_unary(ctx->xm_func, blk, XM_I2F, XR_REP_F64, arg);
        return xm_emit_unary(ctx->xm_func, blk, XM_UNBOX_F64, XR_REP_F64, arg);
    }
    if (arg_r == XR_REP_I64)
        return arg;
    if (arg_r == XR_REP_F64)
        return xm_emit_unary(ctx->xm_func, blk, XM_F2I, XR_REP_I64, arg);
    return xm_emit_unary(ctx->xm_func, blk, XM_UNBOX_I64, XR_REP_I64, arg);
}

/* ========== Call / Closure / Print Lowering ========== */

/* Find the XrProto for a child XiFunc by scanning parent proto's sub-protos.
 * Returns NULL if not found (e.g. cross-module call). */
static struct XrProto *find_callee_proto(LowerCtx *ctx, XiFunc *child_xi) {
    if (!ctx->proto || !child_xi)
        return NULL;
    uint32_t n = ctx->proto->protos.count;
    for (uint32_t i = 0; i < n; i++) {
        struct XrProto *sub = PROTO_PROTO(ctx->proto, i);
        if (sub && sub->xi_func == child_xi)
            return sub;
    }
    return NULL;
}

static XmRef lower_call(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    /* Generic call lowering for XI_CALL and ops delegated to runtime.
     * For XI_CALL: args[0]=callee, args[1..n]=params.
     * For other ops (ITER, CORO, etc.): all args are passed. */
    uint16_t nargs = (v->nargs > 0) ? (v->nargs - 1) : 0;

    /* Collect call args */
    XmRef call_args[17];
    uint16_t total = v->nargs;
    if (total > 16) {
        ctx->error = true;
        return xm_const_i64(ctx->xm_func, 0);
    }
    for (uint16_t i = 0; i < v->nargs; i++)
        call_args[i] = get_ref(ctx, v->args[i]);

    /* CALL_KNOWN optimization: if callee is a local XI_CLOSURE_NEW whose
     * XrProto is known, emit a direct call.  Codegen loads proto->jit_entry
     * for JIT→JIT fast path, falling back to xr_jit_call_func if the callee
     * has not been JIT-compiled yet. */
    if (v->op == XI_CALL && v->nargs >= 1) {
        XiValue *callee_val = v->args[0];
        if (callee_val && callee_val->op == XI_CLOSURE_NEW) {
            XiFunc *child_xi = (XiFunc *) callee_val->aux;
            struct XrProto *callee_proto = find_callee_proto(ctx, child_xi);
            if (callee_proto) {
                uint8_t ret_rep = callee_proto->return_type_info
                                      ? xr_type_rep(callee_proto->return_type_info)
                                      : XR_REP_TAGGED;
                XmRef proto_ref = xm_const_ptr(ctx->xm_func, (void *) callee_proto);
                XmRef nargs_ref = xm_const_i64(ctx->xm_func, (int64_t) nargs);
                XmRef result =
                    xm_emit(ctx->xm_func, blk, XM_CALL_KNOWN, ret_rep, proto_ref, nargs_ref);
                blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
                xm_func_bind_call_args(ctx->xm_func, result, call_args, total);
                return result;
            }
        }
    }

    /* Method IC speculation: if call site is monomorphic, emit
     * GUARD_KLASS(receiver, expected_klass) + CALL_KNOWN(proto). */
    if (v->op == XI_CALL_METHOD && v->nargs >= 1) {
        int bc_pc = slot_map_bc_pc(ctx, v->id);
        const XrICMethod *mic = ic_method_lookup(ctx, bc_pc);
        if (mic) {
            XR_DCHECK(mic->count == 1, "method IC must be monomorphic here");
            XrClass *klass = mic->entries[0].klass;
            XrMethod *method = mic->entries[0].method;
            XR_DCHECK(klass != NULL && method != NULL, "method IC entry must be non-null");

            /* Only speculate on closure methods with a valid proto */
            if (method->type == XMETHOD_CLOSURE && method->as.closure &&
                method->as.closure->proto) {
                struct XrProto *callee_proto = method->as.closure->proto;

                /* Emit GUARD_KLASS on receiver (args[0]) */
                XmRef recv = call_args[0];
                uint16_t did = record_deopt(ctx, (uint32_t) bc_pc);
                if (did == 0xFFFF)
                    goto generic_call; /* deopt overflow */
                XmRef klass_ref = xm_const_ptr(ctx->xm_func, (void *) klass);
                XmRef deopt_ref = xm_const_i64(ctx->xm_func, (int64_t) did);
                xm_emit(ctx->xm_func, blk, XM_GUARD_KLASS, XR_REP_I64, recv, klass_ref);
                blk->ins[blk->nins - 1].dst = deopt_ref;
                blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;

                /* Emit CALL_KNOWN with the IC-resolved proto */
                uint8_t ret_rep = callee_proto->return_type_info
                                      ? xr_type_rep(callee_proto->return_type_info)
                                      : XR_REP_TAGGED;
                XmRef proto_ref = xm_const_ptr(ctx->xm_func, (void *) callee_proto);
                XmRef nargs_c = xm_const_i64(ctx->xm_func, (int64_t) nargs);
                XmRef result =
                    xm_emit(ctx->xm_func, blk, XM_CALL_KNOWN, ret_rep, proto_ref, nargs_c);
                blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
                xm_func_bind_call_args(ctx->xm_func, result, call_args, total);
                return result;
            }
        }
    }

generic_call:
    /* XI_CALL_BUILTIN (dump, copy, StringBuilder, Bytes, cancelled):
     * these are specialized VM ops that don't use xr_jit_invoke_method.
     * Validate the builtin name, then deopt to the VM interpreter. */
    if (v->op == XI_CALL_BUILTIN) {
        const char *bn = (const char *) v->aux;
        int bid = (int) v->aux_int;
        /* Hard fail: reject unknown builtins at JIT lowering time.
         * Known name-based builtins deopt to VM. Numeric-ID builtins
         * (bid > 0) are INVOKE_BUILTIN opcodes and also deopt. */
        if (bn != NULL) {
            static const char *known[] = {"dump",  "copy",          "chr",       "print",
                                          "Bytes", "StringBuilder", "Exception", NULL};
            bool found = false;
            for (int k = 0; known[k]; k++) {
                if (strcmp(bn, known[k]) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "[xi_to_xm] ERROR: unknown builtin name '%s'\n", bn);
                XR_DCHECK(false, "unregistered builtin in JIT lowering");
            }
        } else if (bid < 0) {
            fprintf(stderr, "[xi_to_xm] ERROR: invalid builtin id %d\n", bid);
            XR_DCHECK(false, "invalid builtin id in JIT lowering");
        }
        int bc_pc = slot_map_bc_pc(ctx, v->id);
        uint16_t did = record_deopt(ctx, (uint32_t) bc_pc);
        XmRef deopt_id = xm_const_i64(ctx->xm_func, (int64_t) did);
        xm_emit(ctx->xm_func, blk, XM_DEOPT, XR_REP_I64, deopt_id, XM_NONE);
        blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
        return xm_const_i64(ctx->xm_func, 0);
    }

    /* Method calls (XI_CALL_METHOD) go through xr_jit_invoke_method
     * which resolves the method on the receiver at runtime. */
    if (v->op == XI_CALL_METHOD) {
        /* aux_int = (global_symbol_id << 1) | is_super.
         * The SymbolId is resolved at lowering time (main thread) so the
         * JIT background thread does not need isolate access. */
        int method_sym = (int) (v->aux_int >> 1);
        XR_DCHECK(method_sym > 0, "XI_CALL_METHOD: method_sym=0, lowering failed to "
                                  "resolve SymbolId");
        int bc_pc = slot_map_bc_pc(ctx, v->id);
        uint16_t did = record_deopt(ctx, (uint32_t) bc_pc);
        int64_t encoded = ((int64_t) method_sym << 32) | ((int64_t) (did & 0xFFFF) << 16) |
                          ((int64_t) nargs & 0xFF);
        XmRef encoded_ref = xm_const_i64(ctx->xm_func, encoded);
        XmRef fn_ref = xm_const_ptr(ctx->xm_func, (void *) xr_jit_invoke_method);
        /* Method returns are polymorphic (int, bool, string, null, ptr).
         * Use I64 rep (raw payload in GP register) + TAGGED ctype so
         * the type pass does not narrow, allowing the dynamic tag patch
         * to read the correct runtime tag from vreg_runtime_tags[]. */
        XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_I64, fn_ref, encoded_ref);
        blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
        blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_TAGGED, 0, 0};
        xm_func_bind_call_args(ctx->xm_func, result, call_args, total);
        /* Propagate bc_slot from Xi slot_map so deopt snapshots can
         * reconstruct the correct bytecode register. */
        if (xm_ref_is_vreg(result) && ctx->slot_idx && v->id < ctx->slot_idx_size) {
            int32_t si = ctx->slot_idx[v->id];
            if (si >= 0) {
                uint32_t vi = XM_REF_INDEX(result);
                if (vi < ctx->xm_func->nvreg)
                    ctx->xm_func->vregs[vi].bc_slot = (int16_t) ctx->slot_map->entries[si].bc_slot;
            }
        }
        return result;
    }

    /* Fallback: generic call via xr_jit_call_func bridge.
     * MAY_THROW ensures codegen emits a post-call exception check: the
     * callee can throw any exception and, absent an enclosing try block,
     * the JIT'd caller must deopt so the VM's throw machinery can unwind
     * through bytecode try frames. */
    XmRef nargs_ref = xm_const_i64(ctx->xm_func, (int64_t) nargs);
    XmRef nargs_val = xm_emit_unary(ctx->xm_func, blk, XM_CONST_I64, XR_REP_I64, nargs_ref);
    XmRef fn_ref = xm_const_ptr(ctx->xm_func, (void *) xr_jit_call_func);
    XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_DIRECT, XR_REP_I64, nargs_val, fn_ref);
    blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT | XM_FLAG_MAY_THROW;
    xm_func_bind_call_args(ctx->xm_func, result, call_args, total);
    return result;
}

static XmRef lower_closure_new(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    /* XI_CLOSURE_NEW: aux=child XiFunc*, args=capture values.
     * xr_jit_closure_new expects XrProto*, not XiFunc*. Look up the
     * corresponding sub-proto via find_callee_proto. */
    XiFunc *child_xi = (XiFunc *) v->aux;
    struct XrProto *child_proto = find_callee_proto(ctx, child_xi);
    if (!child_proto) {
        ctx->error = true;
        return xm_const_i64(ctx->xm_func, 0);
    }
    XmRef proto_ref = xm_const_ptr(ctx->xm_func, (void *) child_proto);
    XmRef fn_ref = xm_const_ptr(ctx->xm_func, (void *) xr_jit_closure_new);
    XmRef closure_ref = xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_PTR, fn_ref, proto_ref);
    blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;

    /* Populate UPVAL_SRC_REG entries: for each non-NULL capture arg,
     * call xr_jit_closure_set_upval(closure, value, upval_index).
     *
     * For needs_cell captures (hoisted functions), v->args[i] points to a
     * stale braun_read placeholder created before the real initializer ran.
     * Scan the Xi block for the actual definition of the same variable. */
    for (uint16_t i = 0; i < v->nargs; i++) {
        if (!v->args[i])
            continue;

        XiValue *capture_val = v->args[i];
        bool needs_cell =
            (child_xi && i < child_xi->ncaptures) ? child_xi->captures[i].needs_cell : false;

        /* Resolve stale capture: find real definition in the same block */
        if (needs_cell && capture_val->var_id != 0xFF && capture_val->block) {
            XiBlock *xi_blk = capture_val->block;
            uint8_t target_var = capture_val->var_id;
            for (uint32_t j = 0; j < xi_blk->nvalues; j++) {
                XiValue *w = xi_blk->values[j];
                if (!w || w->var_id != target_var || w == capture_val)
                    continue;
                /* Skip null placeholders (the stale value we want to replace) */
                if (w->op == XI_CONST && w->type && w->type->kind == XR_KIND_NULL)
                    continue;
                capture_val = w;
                /* Keep scanning: last non-null definition wins */
            }
        }

        /* Materialize the capture value.  If we resolved to a different
         * (non-stale) Xi value that hasn't been lowered yet (it appears
         * later in the block), lower it now.  For values already lowered,
         * get_ref returns the existing XmRef. */
        XmRef val;
        if (capture_val != v->args[i]) {
            /* Resolved to a later definition — lower it eagerly.
             * Creates a fresh Xm constant materialization; the main loop
             * will produce another one later which is harmless (DCE cleans). */
            val = lower_value(ctx, blk, capture_val);
        } else {
            val = get_ref(ctx, capture_val);
        }

        /* For needs_cell captures, the closure's upvalue must hold a Cell
         * pointer (not the value).  Multiple closures capturing the same
         * variable share ONE Cell so writes are mutually visible.  The
         * cache keyed by var_id mirrors the bytecode emitter's
         * cell_side_reg / cell_created tracking. */
        if (needs_cell && capture_val->var_id != 0xFF) {
            uint8_t vid = capture_val->var_id;
            if (!ctx->cell_present[vid]) {
                /* Allocate cell, initialize from current value.
                 * Use XR_REP_PTR so type prop tags result vreg as VTAG_PTR;
                 * this ensures call_arg_tags[1] = XR_TAG_PTR when this ref
                 * is later passed to xr_jit_closure_set_upval, so the
                 * upvals[] entry stores a tagged PTR (not raw I64). */
                XmRef cn_fn = xm_const_ptr(ctx->xm_func, (void *) xr_jit_cell_new);
                XmRef cn_res = xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_PTR, cn_fn,
                                       xm_const_i64(ctx->xm_func, 0));
                blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
                XmRef cn_args[] = {val};
                xm_func_bind_call_args(ctx->xm_func, cn_res, cn_args, 1);
                ctx->cell_ref[vid] = cn_res;
                ctx->cell_present[vid] = true;
            }
            /* Use cell pointer (instead of raw value) as the upvalue */
            val = ctx->cell_ref[vid];
        }

        XmRef idx = xm_const_i64(ctx->xm_func, i);
        XmRef set_fn = xm_const_ptr(ctx->xm_func, (void *) xr_jit_closure_set_upval);
        XmRef set_res = xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_I64, set_fn, idx);
        blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
        XmRef call_args[] = {closure_ref, val};
        xm_func_bind_call_args(ctx->xm_func, set_res, call_args, 2);
    }

    return closure_ref;
}

static XmRef lower_print(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    /* XI_PRINT: args[0..n]=values, aux_int=flags */
    for (uint16_t i = 0; i < v->nargs; i++) {
        XmRef arg = get_ref(ctx, v->args[i]);
        XmRef flags = xm_const_i64(ctx->xm_func, v->aux_int);
        xm_emit(ctx->xm_func, blk, XM_RT_PRINT, XR_REP_I64, arg, flags);
        blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
    }
    return xm_const_i64(ctx->xm_func, 0);
}

/* ========== Value Lowering Dispatch ========== */

static XmRef lower_value(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    switch (v->op) {
        case XI_CONST:
            return lower_const(ctx, blk, v);

        case XI_PARAM: {
            /* Parameters are pre-mapped during initialization */
            return get_ref(ctx, v);
        }

        /* Binary arithmetic + bitwise */
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV:
        case XI_MOD:
        case XI_BAND:
        case XI_BOR:
        case XI_BXOR:
        case XI_SHL:
        case XI_SHR:
            return lower_binary_arith(ctx, blk, v);

        /* Unary */
        case XI_NEG:
        case XI_BNOT:
        case XI_NOT:
            return lower_unary(ctx, blk, v);

        /* Comparison */
        case XI_EQ:
        case XI_NE:
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE:
        case XI_EQ_STRICT:
        case XI_NE_STRICT:
            return lower_comparison(ctx, blk, v);

        /* Type conversion */
        case XI_CONVERT:
            return lower_convert(ctx, blk, v);
        case XI_BOX:
            return lower_box(ctx, blk, v);
        case XI_UNBOX:
            return lower_unbox(ctx, blk, v);

        /* Explicit width narrowing/widening — ensures correct value range
         * in the JIT register. Unsigned: AND mask. Signed: SHL+SAR. */
        case XI_NARROW_U8:
        case XI_WIDEN_U8: {
            XmRef a = get_ref(ctx, v->args[0]);
            XmRef m = xm_const_i64(ctx->xm_func, 0xFF);
            return xm_emit(ctx->xm_func, blk, XM_AND, XR_REP_I64, a, m);
        }
        case XI_NARROW_U16:
        case XI_WIDEN_U16: {
            XmRef a = get_ref(ctx, v->args[0]);
            XmRef m = xm_const_i64(ctx->xm_func, 0xFFFF);
            return xm_emit(ctx->xm_func, blk, XM_AND, XR_REP_I64, a, m);
        }
        case XI_NARROW_U32:
        case XI_WIDEN_U32: {
            XmRef a = get_ref(ctx, v->args[0]);
            XmRef m = xm_const_i64(ctx->xm_func, 0xFFFFFFFF);
            return xm_emit(ctx->xm_func, blk, XM_AND, XR_REP_I64, a, m);
        }
        case XI_NARROW_I8:
        case XI_WIDEN_I8: {
            XmRef a = get_ref(ctx, v->args[0]);
            XmRef sh = xm_const_i64(ctx->xm_func, 56);
            XmRef t = xm_emit(ctx->xm_func, blk, XM_SHL, XR_REP_I64, a, sh);
            return xm_emit(ctx->xm_func, blk, XM_SHR, XR_REP_I64, t, sh);
        }
        case XI_NARROW_I16:
        case XI_WIDEN_I16: {
            XmRef a = get_ref(ctx, v->args[0]);
            XmRef sh = xm_const_i64(ctx->xm_func, 48);
            XmRef t = xm_emit(ctx->xm_func, blk, XM_SHL, XR_REP_I64, a, sh);
            return xm_emit(ctx->xm_func, blk, XM_SHR, XR_REP_I64, t, sh);
        }
        case XI_NARROW_I32:
        case XI_WIDEN_I32: {
            XmRef a = get_ref(ctx, v->args[0]);
            XmRef sh = xm_const_i64(ctx->xm_func, 32);
            XmRef t = xm_emit(ctx->xm_func, blk, XM_SHL, XR_REP_I64, a, sh);
            return xm_emit(ctx->xm_func, blk, XM_SHR, XR_REP_I64, t, sh);
        }
        case XI_NARROW_F32:
        case XI_WIDEN_F32: {
            /* float precision roundtrip — typed array runtime already handles
             * the actual float32 truncation, so this is a semantic no-op in JIT.
             * If we had FCVTS/FCVTD, we'd emit them here. */
            return get_ref(ctx, v->args[0]);
        }

        /* Null check */
        case XI_ISNULL: {
            XR_DCHECK(v->nargs == 1, "isnull: expected 1 arg");
            XmRef arg = get_ref(ctx, v->args[0]);
            return xm_emit_unary(ctx->xm_func, blk, XM_RT_ISNULL, XR_REP_I64, arg);
        }

        /* Function call */
        case XI_CALL:
            return lower_call(ctx, blk, v);

        /* Closure creation */
        case XI_CLOSURE_NEW:
            return lower_closure_new(ctx, blk, v);

        /* Print */
        case XI_PRINT:
            return lower_print(ctx, blk, v);

        /* Shared (module-level) variables — loaded via runtime bridge.
         * XM_LOAD with a const-ref arg is wrong: xra_arg returns XZR for
         * non-vreg refs, and ARM64 LDR treats XZR-base as SP, loading
         * garbage from the stack instead of the shared array. */
        case XI_GET_SHARED: {
            /* aux_int is the relative shared slot; the runtime bridge
             * needs the absolute index (relative + shared_offset). */
            int so = ctx->proto ? ctx->proto->shared_offset : 0;
            int64_t abs_idx = v->aux_int + so;
            XmRef idx = xm_const_i64(ctx->xm_func, abs_idx);
            XmRef fn_ref = xm_const_ptr(ctx->xm_func, (void *) xr_jit_get_shared);
            XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_I64, fn_ref, idx);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            /* Shared vars are dynamically typed: set XM_TK_TAGGED to
             * prevent the type pass from narrowing to I64, which would
             * skip the vreg_runtime_tags[] dynamic tag patch. */
            blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_TAGGED, 0, 0};
            /* Propagate bc_slot for runtime tag resolution */
            if (xm_ref_is_vreg(result) && ctx->slot_idx && v->id < ctx->slot_idx_size) {
                int32_t si = ctx->slot_idx[v->id];
                if (si >= 0) {
                    uint32_t vi = XM_REF_INDEX(result);
                    if (vi < ctx->xm_func->nvreg)
                        ctx->xm_func->vregs[vi].bc_slot =
                            (int16_t) ctx->slot_map->entries[si].bc_slot;
                }
            }
            return result;
        }
        case XI_SET_SHARED: {
            /* Mirror XI_GET_SHARED: lower as CALL_C to xr_jit_set_shared.
             * The runtime bridge reads the value from jit_ctx->call_args[0]
             * (populated via the call_arg_pool) and the absolute shared
             * index from extra_arg. */
            XR_DCHECK(v->nargs == 1, "set_shared: expected 1 arg");
            XmRef val = get_ref(ctx, v->args[0]);
            int so = ctx->proto ? ctx->proto->shared_offset : 0;
            int64_t abs_idx = v->aux_int + so;
            XmRef idx = xm_const_i64(ctx->xm_func, abs_idx);
            XmRef fn_ref = xm_const_ptr(ctx->xm_func, (void *) xr_jit_set_shared);
            XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_I64, fn_ref, idx);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            XmRef call_args[] = {val};
            xm_func_bind_call_args(ctx->xm_func, result, call_args, 1);
            return val;
        }

        /* Upvalue access — must call runtime bridge (not XM_LOAD which
         * reads JIT frame slots, not closure->upvals[]).
         *
         * For needs_cell captures (mutable shared), the upvalue stores a
         * Cell pointer.  Use the *_cell variants so the runtime helper
         * dereferences/writes through the cell. */
        case XI_LOAD_UPVAL: {
            int upi = (int) v->aux_int;
            bool needs_cell = ctx->xi_func && upi >= 0 && upi < (int) ctx->xi_func->ncaptures &&
                              ctx->xi_func->captures[upi].needs_cell;
            void *bridge = needs_cell ? (void *) xr_jit_upval_cell_get : (void *) xr_jit_upval_get;
            XmRef idx = xm_const_i64(ctx->xm_func, v->aux_int);
            XmRef fn_ref = xm_const_ptr(ctx->xm_func, bridge);
            XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_I64, fn_ref, idx);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_TAGGED, 0, 0};
            return result;
        }
        case XI_STORE_UPVAL: {
            XR_DCHECK(v->nargs == 1, "store_upval: expected 1 arg");
            int upi = (int) v->aux_int;
            bool needs_cell = ctx->xi_func && upi >= 0 && upi < (int) ctx->xi_func->ncaptures &&
                              ctx->xi_func->captures[upi].needs_cell;
            void *bridge = needs_cell ? (void *) xr_jit_upval_cell_set : (void *) xr_jit_upval_set;
            XmRef val = get_ref(ctx, v->args[0]);
            XmRef idx = xm_const_i64(ctx->xm_func, v->aux_int);
            XmRef fn_ref = xm_const_ptr(ctx->xm_func, bridge);
            XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_I64, fn_ref, idx);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            XmRef call_args[] = {val};
            xm_func_bind_call_args(ctx->xm_func, result, call_args, 1);
            return val;
        }

        /* Method call — same as generic call for now */
        case XI_CALL_METHOD:
        case XI_CALL_BUILTIN:
            return lower_call(ctx, blk, v);

        /* Extract i-th result from a multi-return call.  aux_int is the
         * 1-based result index.  Index 1 trivially aliases the first
         * return value (the call's primary x0/x1 result).  Indices >= 2
         * require pulling from coro->jit_ctx->ret_vals[] — codegen does
         * not emit those reads yet, so bail out and let the VM run
         * the function. */
        case XI_EXTRACT: {
            XR_DCHECK(v->nargs == 1, "extract: expected 1 arg");
            if (v->aux_int > 1) {
                ctx->error = true;
                return xm_const_i64(ctx->xm_func, 0);
            }
            return get_ref(ctx, v->args[0]);
        }

        /* Field access — with optional shape-guard speculation from IC */
        case XI_LOAD_FIELD: {
            XR_DCHECK(v->nargs >= 1, "load_field: need obj arg");
            XmRef obj = get_ref(ctx, v->args[0]);

            /* IC speculation: if field IC has monomorphic Json shape,
             * emit GUARD_SHAPE + direct LOAD_FIELD at known offset. */
            int bc_pc = slot_map_bc_pc(ctx, v->id);
            const XrICField *fic = ic_field_lookup(ctx, bc_pc);
            if (fic) {
                uint16_t did = record_deopt(ctx, (uint32_t) bc_pc);
                if (did != 0xFFFF) {
                    XmRef shape_id = xm_const_i64(ctx->xm_func, (int64_t) fic->json_shape_id);
                    XmRef deopt_ref = xm_const_i64(ctx->xm_func, (int64_t) did);
                    xm_emit(ctx->xm_func, blk, XM_GUARD_SHAPE, XR_REP_I64, obj, shape_id);
                    blk->ins[blk->nins - 1].dst = deopt_ref;
                    blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;

                    /* Direct field load at known offset */
                    int64_t byte_off =
                        XM_JSON_FIELDS_OFFSET + (int64_t) fic->json_field_idx * XM_XRVALUE_SIZE;
                    XmRef off = xm_const_i64(ctx->xm_func, byte_off);
                    return xm_emit(ctx->xm_func, blk, XM_LOAD_FIELD, XR_REP_I64, obj, off);
                }
                /* deopt overflow: fall through to generic path */
            }

            /* Property-based access (v->aux = name) on a non-Json receiver
             * (or no IC info available) cannot lower to a direct field load:
             * the byte offset depends on the runtime class shape and is not
             * known at compile time. v->aux_int defaults to 0 here, so a
             * direct LOAD_FIELD would read from the GC header. Bail out and
             * let the VM service the property access via OP_GETPROP. */
            if (v->aux) {
                ctx->error = true;
                return xm_const_i64(ctx->xm_func, 0);
            }
            /* Indexed access path (aux_int = byte offset) — currently unused
             * by the frontend but kept for completeness. */
            XmRef off = xm_const_i64(ctx->xm_func, v->aux_int);
            return xm_emit(ctx->xm_func, blk, XM_LOAD_FIELD, XR_REP_I64, obj, off);
        }
        case XI_STORE_FIELD: {
            XR_DCHECK(v->nargs >= 2, "store_field: need obj + val");
            XmRef obj = get_ref(ctx, v->args[0]);
            XmRef val = get_ref(ctx, v->args[1]);
            /* See XI_LOAD_FIELD above — name-based stores cannot be lowered
             * to a direct memory write because the field byte offset is not
             * materialised at this stage.  The VM's OP_SETPROP path handles
             * shape lookup, setter dispatch, and write barriers correctly. */
            if (v->aux) {
                ctx->error = true;
                return val;
            }
            XmRef off = xm_const_i64(ctx->xm_func, v->aux_int);
            /* Codegen contract for XM_STORE_FIELD:
             *   args[0] = obj, args[1] = val, dst = const(offset),
             *   rep     = xr_tag (XM_SF_TAG_RUNTIME = infer at codegen).
             * Build the instruction directly so we can override dst
             * with the offset constant — xm_emit() always allocates a
             * fresh vreg for dst, which we don't want here. */
            xm_emit(ctx->xm_func, blk, XM_STORE_FIELD, XR_REP_VOID, obj, val);
            XmIns *sf = &blk->ins[blk->nins - 1];
            sf->rep = XM_SF_TAG_RUNTIME;
            sf->dst = off;
            sf->flags |= XM_FLAG_SIDE_EFFECT;
            return val;
        }

        /* Index access */
        case XI_INDEX_GET: {
            XR_DCHECK(v->nargs >= 2, "index_get: need obj + key");
            XmRef obj = get_ref(ctx, v->args[0]);
            XmRef key = get_ref(ctx, v->args[1]);
            XmRef fn_ref = xm_const_ptr(ctx->xm_func, (void *) xr_jit_index_get);
            XmRef extra = xm_const_i64(ctx->xm_func, 0);
            XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_I64, fn_ref, extra);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_TAGGED, 0, 0};
            XmRef args[2] = {obj, key};
            xm_func_bind_call_args(ctx->xm_func, result, args, 2);
            return result;
        }
        case XI_INDEX_SET: {
            XR_DCHECK(v->nargs >= 3, "index_set: need obj + key + val");
            XmRef obj = get_ref(ctx, v->args[0]);
            XmRef key = get_ref(ctx, v->args[1]);
            XmRef val = get_ref(ctx, v->args[2]);
            XmRef fn_ref = xm_const_ptr(ctx->xm_func, (void *) xr_jit_index_set);
            XmRef extra = xm_const_i64(ctx->xm_func, 0);
            XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_I64, fn_ref, extra);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            XmRef args[3] = {obj, key, val};
            xm_func_bind_call_args(ctx->xm_func, result, args, 3);
            return val;
        }

        /* Array / Map creation */
        case XI_ARRAY_NEW: {
            XmRef cap = (v->nargs >= 1) ? get_ref(ctx, v->args[0]) : xm_const_i64(ctx->xm_func, 0);
            return xm_emit_unary(ctx->xm_func, blk, XM_RT_ARRAY_NEW, XR_REP_I64, cap);
        }
        case XI_MAP_NEW: {
            XmRef cap = (v->nargs >= 1) ? get_ref(ctx, v->args[0]) : xm_const_i64(ctx->xm_func, 0);
            return xm_emit_unary(ctx->xm_func, blk, XM_RT_MAP_NEW, XR_REP_I64, cap);
        }

        /* String concatenation — lower as sequential RT calls */
        case XI_STR_CONCAT: {
            if (v->nargs == 0)
                return xm_const_i64(ctx->xm_func, 0);
            XmRef acc = get_ref(ctx, v->args[0]);
            for (uint16_t i = 1; i < v->nargs; i++) {
                XmRef part = get_ref(ctx, v->args[i]);
                acc = xm_emit(ctx->xm_func, blk, XM_RT_ADD, XR_REP_I64, acc, part);
            }
            return acc;
        }

        /* Exception throw — delegates to xr_jit_throw runtime bridge.
         * Codegen checks coro->jit_ctx->exception after the call and
         * branches to exception_handler if non-NULL. */
        case XI_THROW: {
            XR_DCHECK(v->nargs >= 1, "throw: need value arg");
            XmRef val = get_ref(ctx, v->args[0]);
            XmRef fn_ptr = xm_const_ptr(ctx->xm_func, (void *) xr_jit_throw);
            xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_I64, fn_ptr, val);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT | XM_FLAG_MAY_THROW;
            return xm_const_i64(ctx->xm_func, 0);
        }

        /* Iteration — lower as generic calls (runtime handles protocol).
         * The iterator object lives in args[0], not a closure callee, so
         * lower_call's xr_jit_call_func bridge mis-routes the dispatch.
         * Bail until a dedicated helper wiring exists. */
        case XI_ITER_NEW:
        case XI_ITER_NEXT:
        case XI_ITER_VALID:
            ctx->error = true;
            return xm_const_i64(ctx->xm_func, 0);

        /* Coroutine ops — same hazard as struct/iter: args[0] is a
         * channel ptr / buffer size / task ptr, not a closure callee.
         * Bail until xi_to_xm wires the xr_jit_chan_* / xr_jit_go /
         * xr_jit_await helpers with their CALL_C argument layout. */
        case XI_GO:
        case XI_AWAIT:
        case XI_CHAN_SEND:
        case XI_CHAN_RECV:
        case XI_CHAN_TRY_SEND:
        case XI_CHAN_TRY_RECV:
        case XI_YIELD:
        case XI_CHAN_NEW:
            ctx->error = true;
            return xm_const_i64(ctx->xm_func, 0);

        /* Defer — JIT codegen doesn't yet schedule the deferred body
         * onto the cleanup chain.  Silently dropping it (the previous
         * behaviour) caused observable output divergence vs the VM, so
         * bail out and let the VM run functions that contain `defer`. */
        case XI_DEFER:
            ctx->error = true;
            return xm_const_i64(ctx->xm_func, 0);

        /* Json allocation (treated as generic call for now) */
        case XI_JSON_NEW:
        case XI_JSON_INIT_F:
        case XI_JSON_GET_F:
        case XI_JSON_SET_F:
        case XI_JSON_DECODE:
            return lower_call(ctx, blk, v);

        /* Set creation */
        case XI_SET_NEW: {
            XmRef cap = (v->nargs >= 1) ? get_ref(ctx, v->args[0]) : xm_const_i64(ctx->xm_func, 0);
            return xm_emit_unary(ctx->xm_func, blk, XM_RT_MAP_NEW, XR_REP_I64, cap);
        }

        /* Type operations — deopt to VM for runtime type checks */
        case XI_IS:
        case XI_AS:
            return lower_call(ctx, blk, v);

        /* Slice / Range — lower as generic calls */
        case XI_SLICE:
        case XI_RANGE:
            return lower_call(ctx, blk, v);

        /* Multi-return packaging.  Codegen RET only emits x0/x1, so
         * extra values would be dropped — bail out for now. */
        case XI_MULTI_RET:
            if (v->nargs > 1) {
                ctx->error = true;
                return xm_const_i64(ctx->xm_func, 0);
            }
            if (v->nargs == 1)
                return get_ref(ctx, v->args[0]);
            return xm_const_i64(ctx->xm_func, 0);

        /* Identity / type narrowing — transparent passthrough */
        case XI_COPY:
            if (v->nargs >= 1)
                return get_ref(ctx, v->args[0]);
            return xm_const_i64(ctx->xm_func, 0);

        /* Builtins lowered as generic runtime calls */
        case XI_ASSERT:
        case XI_ASSERT_EQ:
        case XI_ASSERT_NE:
        case XI_TYPEOF:
        case XI_GET_BUILTIN:
        case XI_CLASS_CREATE:
            return lower_call(ctx, blk, v);

        case XI_STRUCT_NEW:
        case XI_STRUCT_GET:
        case XI_STRUCT_SET:
            /* Struct ops use a non-closure argument layout (struct_ptr +
             * field_idx + value), and lower_call would mis-route them
             * through xr_jit_call_func.  Bail out until they have proper
             * lowering via the xr_jit_new_struct / xr_jit_struct_*
             * runtime helpers. */
            ctx->error = true;
            return xm_const_i64(ctx->xm_func, 0);

        /* Structured concurrency scope — same hazard as iter/coroutine
         * ops: lower_call would mis-route the call_args layout through
         * xr_jit_call_func, silently dropping the scope body.  Bail. */
        case XI_SCOPE_ENTER:
        case XI_SCOPE_EXIT:
            ctx->error = true;
            return xm_const_i64(ctx->xm_func, 0);

        /* Exception handling */
        case XI_TRY: {
            /* aux = catch XiBlock*, aux_int = has_finally flag.
             * Push handler onto try stack; exception_handler is set
             * on blocks in the propagation pass after lowering. */
            XiBlock *catch_xi = (XiBlock *) v->aux;
            XR_DCHECK(catch_xi != NULL, "XI_TRY: missing catch block");
            XmBlock *catch_xm = get_block(ctx, catch_xi);
            if (ctx->try_depth < XI2XM_MAX_TRY_DEPTH) {
                ctx->try_stack[ctx->try_depth].handler = catch_xm;
                ctx->try_depth++;
            }
            return xm_const_i64(ctx->xm_func, 0);
        }

        case XI_CATCH: {
            /* Load exception from coro->jit_ctx->exception, clear it */
            XmRef exc = xm_emit_unary(ctx->xm_func, blk, XM_CATCH, XR_REP_I64, XM_NONE);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            return exc;
        }

        case XI_FINALLY:
            /* Marker only — no Xm instruction needed */
            return xm_const_i64(ctx->xm_func, 0);

        case XI_END_TRY: {
            if (ctx->try_depth > 0)
                ctx->try_depth--;
            xm_emit(ctx->xm_func, blk, XM_TRY_END, XR_REP_I64, XM_NONE, XM_NONE);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            return xm_const_i64(ctx->xm_func, 0);
        }

        /* Cross-module import ref — resolve via shared array (same as
         * GET_SHARED).  The VM's module loader populates the shared slot
         * at import time; JIT reads it like any other shared variable. */
        case XI_IMPORT_REF: {
            int so = ctx->proto ? ctx->proto->shared_offset : 0;
            int64_t abs_idx = v->aux_int + so;
            XmRef idx = xm_const_i64(ctx->xm_func, abs_idx);
            XmRef fn_ref = xm_const_ptr(ctx->xm_func, (void *) xr_jit_get_shared);
            XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_I64, fn_ref, idx);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_TAGGED, 0, 0};
            return result;
        }

        default:
            /* Truly unknown op — mark error, return dummy */
            ctx->error = true;
            return xm_const_i64(ctx->xm_func, 0);
    }
}

/* ========== Block Lowering ========== */

/* Lower all phi nodes in a block */
static void lower_phis(LowerCtx *ctx, XiBlock *xi_blk, XmBlock *xm_blk) {
    for (XiPhi *phi = xi_blk->phis; phi; phi = phi->next) {
        XiValue *pv = &phi->value;
        uint8_t rep = pv->rep ? pv->rep : XR_REP_TAGGED;
        XmPhi *xm_phi = xm_add_phi(ctx->xm_func, xm_blk, rep);
        XR_DCHECK(xm_phi != NULL, "lower_phis: xm_add_phi returned NULL");
        set_ref(ctx, pv->id, xm_phi->dst);
    }
}

/* Set phi arguments after all blocks are lowered (all refs resolved) */
static void resolve_phi_args(LowerCtx *ctx, XiBlock *xi_blk, XmBlock *xm_blk) {
    uint32_t pred_idx = 0;
    (void) pred_idx;

    for (XiPhi *phi = xi_blk->phis; phi; phi = phi->next) {
        XiValue *pv = &phi->value;
        XR_DCHECK(pv->nargs == xi_blk->npreds, "phi arg count must match predecessor count");

        /* Find matching XmPhi by dst ref */
        XmRef phi_ref = get_ref(ctx, pv);
        XmPhi *xm_phi = xm_blk->phis;
        while (xm_phi && xm_phi->dst != phi_ref)
            xm_phi = xm_phi->next;
        XR_DCHECK(xm_phi != NULL, "resolve_phi_args: no matching XmPhi");

        for (uint16_t i = 0; i < pv->nargs; i++) {
            XmRef arg_ref = get_ref(ctx, pv->args[i]);
            xm_phi_set_arg(xm_phi, i, arg_ref);
        }
    }
}

/* Cell-wrap helper: if var_id `vid` is cellified, emit cell_get_direct and
 * return the dereferenced value's XmRef.  Returns XM_NONE if not cellified. */
static XmRef maybe_cell_get(LowerCtx *ctx, XmBlock *blk, uint8_t vid) {
    if (vid == 0xFF || !ctx->cell_present[vid])
        return XM_NONE;
    XmRef cg_fn = xm_const_ptr(ctx->xm_func, (void *) xr_jit_cell_get_direct);
    XmRef result =
        xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_I64, cg_fn, xm_const_i64(ctx->xm_func, 0));
    blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
    blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_TAGGED, 0, 0};
    XmRef args[] = {ctx->cell_ref[vid]};
    xm_func_bind_call_args(ctx->xm_func, result, args, 1);
    return result;
}

/* Cell-wrap helper: if var_id `vid` is cellified, emit cell_set_direct
 * to mirror `val` back into the shared cell.  No-op otherwise. */
static void maybe_cell_set(LowerCtx *ctx, XmBlock *blk, uint8_t vid, XmRef val) {
    if (vid == 0xFF || !ctx->cell_present[vid])
        return;
    XmRef cs_fn = xm_const_ptr(ctx->xm_func, (void *) xr_jit_cell_set_direct);
    XmRef result =
        xm_emit(ctx->xm_func, blk, XM_CALL_C, XR_REP_I64, cs_fn, xm_const_i64(ctx->xm_func, 0));
    blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
    XmRef args[] = {ctx->cell_ref[vid], val};
    xm_func_bind_call_args(ctx->xm_func, result, args, 2);
}

/* Lower a single block's instructions */
static void lower_block_values(LowerCtx *ctx, XiBlock *xi_blk, XmBlock *xm_blk) {
    for (uint32_t i = 0; i < xi_blk->nvalues; i++) {
        XiValue *v = xi_blk->values[i];
        if (!v)
            continue;
        XmRef ref = lower_value(ctx, xm_blk, v);
        set_ref(ctx, v->id, ref);
    }
}

/* Set block terminator */
static void lower_terminator(LowerCtx *ctx, XiBlock *xi_blk, XmBlock *xm_blk) {
    switch (xi_blk->kind) {
        case XI_BLOCK_PLAIN:
            if (xi_blk->succs[0]) {
                XmBlock *target = get_block(ctx, xi_blk->succs[0]);
                xm_block_set_jmp(xm_blk, target);
            }
            break;

        case XI_BLOCK_IF: {
            XR_DCHECK(xi_blk->control != NULL, "IF block has no control");
            XR_DCHECK(xi_blk->succs[0] != NULL, "IF block has no then successor");
            XR_DCHECK(xi_blk->succs[1] != NULL, "IF block has no else successor");
            XmRef cond = get_ref(ctx, xi_blk->control);
            XmBlock *if_true = get_block(ctx, xi_blk->succs[0]);
            XmBlock *if_false = get_block(ctx, xi_blk->succs[1]);
            xm_block_set_br(xm_blk, cond, if_true, if_false);
            break;
        }

        case XI_BLOCK_RETURN: {
            XmRef ret_val;
            if (xi_blk->control) {
                ret_val = get_ref(ctx, xi_blk->control);
            } else {
                /* No explicit return value (void return): produce a NULL
                 * pointer.  Codegen RET reads the const's rep to pick the
                 * return tag; PTR-with-raw-0 is reconstructed as
                 * XR_TAG_NULL by jit_value_from_tag().  Using i64 0 here
                 * would tag the value as I64 and break "result == null"
                 * checks on the caller side. */
                ret_val = xm_const_ptr(ctx->xm_func, NULL);
            }
            xm_block_set_ret(xm_blk, ret_val);
            break;
        }

        case XI_BLOCK_UNREACHABLE:
            /* No successors — leave as unreachable */
            break;

        default:
            ctx->error = true;
            break;
    }
}

/* ========== Deopt Snapshot Liveness Refinement ========== */

/* Return true if opcode is a guard that carries a deopt_id in its dst field. */
static bool is_guard_op(uint16_t op) {
    return op == XM_GUARD_TAG || op == XM_GUARD_BOUNDS || op == XM_GUARD_NONNULL ||
           op == XM_GUARD_CLASS || op == XM_GUARD_KLASS || op == XM_GUARD_SHAPE ||
           op == XM_TAG_CHECK || op == XM_DEOPT;
}

/* Extract deopt_id from a guard instruction's dst const ref.
 * Returns 0xFFFF if no valid deopt_id is found. */
static uint16_t guard_deopt_id(const XmFunc *func, const XmIns *ins) {
    if (xm_ref_is_none(ins->dst) || !xm_ref_is_const(ins->dst))
        return 0xFFFF;
    uint32_t ci = XM_REF_INDEX(ins->dst);
    if (ci >= func->nconst)
        return 0xFFFF;
    return (uint16_t) func->consts[ci].val.raw;
}

/* Trim deopt snapshots to values live at each guard point.
 *
 * Algorithm:
 *   1. Compute per-block live_in / live_out via standard backward dataflow.
 *   2. For each block, walk instructions backward computing a running live
 *      set.  When a guard instruction is encountered, intersect its deopt
 *      snapshot with the current live set — removing dead slots.
 *
 * Soundness: a slot whose Xm vreg is dead at the guard cannot be
 * needed after deopt, because the optimized code has no further use
 * and the interpreter resumes at the guard's bc_pc where the value
 * is equally unreachable.  Constants are always retained (they have
 * no vreg liveness). */
static void refine_deopt_liveness(XmFunc *func) {
    XR_DCHECK(func != NULL, "refine_deopt_liveness: NULL func");
    if (func->ndeopt == 0 || func->nblk == 0)
        return;

    /* Compute dataflow liveness */
    XmLive live;
    memset(&live, 0, sizeof(live));
    xm_live_compute(&live, func);
    if (!live.blocks)
        return; /* allocation failure — skip refinement */

    /* Per-instruction live set (reused across blocks) */
    XmBSet cur;
    xm_bset_init(&cur, func->nvreg);

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk || blk->nins == 0)
            continue;

        /* Start from live_out of this block */
        xm_bset_copy(&cur, &live.blocks[bi].live_out);

        /* Walk instructions backward */
        for (int32_t ii = (int32_t) blk->nins - 1; ii >= 0; ii--) {
            XmIns *ins = &blk->ins[ii];

            /* If this is a guard, refine its deopt snapshot NOW
             * (cur contains liveness AFTER this instruction) */
            if (is_guard_op(ins->op)) {
                uint16_t did = guard_deopt_id(func, ins);
                if (did < func->ndeopt) {
                    XmDeoptInfo *info = &func->deopt_infos[did];
                    if (info->slots && info->nslots > 0) {
                        /* Compact: keep only live or constant slots */
                        uint16_t w = 0;
                        for (uint16_t r = 0; r < info->nslots; r++) {
                            XmRef ref = info->slots[r].value;
                            bool keep = true;
                            if (xm_ref_is_vreg(ref)) {
                                uint32_t vi = XM_REF_INDEX(ref);
                                keep = (vi < func->nvreg) && xm_bset_has(&cur, vi);
                            }
                            /* Constants always kept (no vreg) */
                            if (keep) {
                                if (w != r)
                                    info->slots[w] = info->slots[r];
                                w++;
                            }
                        }
                        info->nslots = w;
                    }
                }
            }

            /* Update liveness: remove def, add uses */
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t dv = XM_REF_INDEX(ins->dst);
                if (dv < func->nvreg)
                    xm_bset_clr(&cur, dv);
            }
            for (int a = 0; a < 2; a++) {
                if (xm_ref_is_vreg(ins->args[a])) {
                    uint32_t av = XM_REF_INDEX(ins->args[a]);
                    if (av < func->nvreg)
                        xm_bset_set(&cur, av);
                }
            }
        }
    }

    xm_bset_free(&cur);
    xm_live_free(&live);
}

/* ========== Main Entry Point ========== */

XR_FUNC struct XmFunc *xi_to_xm_lower(XiFunc *xi_func, struct XrProto *proto, XiSlotMap *slot_map,
                                      const XmICSnapshot *ic, struct XrayIsolate *isolate) {
    XR_DCHECK(xi_func != NULL, "xi_to_xm_lower: NULL xi_func");

    /* Ensure representations are populated (idempotent if already done). */
    if (xi_func->stage < XI_STAGE_REPPED) {
        xi_opt_select_rep(xi_func);
        xi_opt_box_elim(xi_func);
    }

    XmFunc *func = xm_func_new(xi_func->name);
    if (!func)
        return NULL;
    /* Link the XmFunc back to its source proto so codegen / passes can
     * read declared return type, param types, etc. */
    func->proto = proto;

    LowerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.xi_func = xi_func;
    ctx.xm_func = func;
    ctx.proto = proto;
    ctx.slot_map = slot_map;
    ctx.ic = ic;
    ctx.isolate = isolate;
    ctx.error = false;

    /* Allocate block map */
    ctx.block_map_size = xi_func->next_block_id;
    ctx.block_map = (XmBlock **) xr_calloc(ctx.block_map_size, sizeof(XmBlock *));
    if (!ctx.block_map) {
        xm_func_destroy(func);
        return NULL;
    }

    /* Allocate ref map */
    ctx.ref_map_size = xi_func->next_value_id;
    ctx.ref_map = (XmRef *) xr_calloc(ctx.ref_map_size, sizeof(XmRef));
    if (!ctx.ref_map) {
        xr_free(ctx.block_map);
        xm_func_destroy(func);
        return NULL;
    }

    /* Build direct-mapped index: value_id → slot_map entry.
     * Replaces O(n) linear scan with O(1) lookup per IC query. */
    ctx.slot_idx = NULL;
    ctx.slot_idx_size = 0;
    if (slot_map && slot_map->count > 0) {
        ctx.slot_idx_size = ctx.ref_map_size;
        ctx.slot_idx = (int32_t *) xr_malloc(ctx.slot_idx_size * sizeof(int32_t));
        if (ctx.slot_idx) {
            memset(ctx.slot_idx, 0xFF, ctx.slot_idx_size * sizeof(int32_t));
            for (uint32_t i = 0; i < slot_map->count; i++) {
                uint32_t vid = slot_map->entries[i].value_id;
                if (vid < ctx.slot_idx_size)
                    ctx.slot_idx[vid] = (int32_t) i;
            }
        }
    }

    /* Allocate param vregs FIRST: codegen prologue assumes consecutive
     * vregs 0..num_params-1 correspond to function arguments loaded from
     * the args_ptr array. Must precede any other xm_new_vreg calls. */
    func->num_params = xi_func->nparams;
    for (uint16_t i = 0; i < xi_func->nparams; i++) {
        XiValue *param = xi_func->params[i];
        uint8_t rep = param ? (param->rep ? param->rep : XR_REP_TAGGED) : XR_REP_I64;
        XmRef vreg = xm_new_vreg(func, rep);
        if (param)
            set_ref(&ctx, param->id, vreg);
        /* Params occupy bytecode slots 0..n-1. Setting bc_slot enables
         * deopt snapshots to reconstruct the correct bytecode registers. */
        uint32_t vi = XM_REF_INDEX(vreg);
        if (vi < func->nvreg)
            func->vregs[vi].bc_slot = (int16_t) i;
    }

    /* Create all XmBlocks upfront (so forward jumps resolve) */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk)
            continue;
        XmBlock *xm_blk = xm_func_add_block(func, NULL);
        XR_DCHECK(xm_blk != NULL, "xi_to_xm_lower: block allocation failed");
        ctx.block_map[xi_blk->id] = xm_blk;
    }

    /* Set up predecessor edges.  Xi records exception edges (try -> catch)
     * on the catch block's preds[] without listing the try block in its
     * succs[]; Xm's verifier walks pred <-> succ bidirectionally and would
     * fail on those.  Filter to preds whose normal succs[] actually point
     * back at this block — exception entry into catch blocks is preserved
     * separately via XmBlock.exception_handler. */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk)
            continue;
        XmBlock *xm_blk = get_block(&ctx, xi_blk);
        for (uint16_t p = 0; p < xi_blk->npreds; p++) {
            XiBlock *xi_pred = xi_blk->preds[p];
            if (!xi_pred)
                continue;
            if (xi_pred->succs[0] != xi_blk && xi_pred->succs[1] != xi_blk)
                continue; /* exception edge — keep out of Xm's normal CFG */
            XmBlock *pred = get_block(&ctx, xi_pred);
            xm_block_add_pred(xm_blk, pred, func->arena);
        }
    }

    /* Lower phi nodes (create XmPhi with dst, defer arg resolution) */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk)
            continue;
        XmBlock *xm_blk = get_block(&ctx, xi_blk);
        lower_phis(&ctx, xi_blk, xm_blk);
    }

    /* Lower all block instructions + propagate exception handlers.
     * XI_TRY / XI_END_TRY push/pop ctx.try_stack during lowering.
     * After lowering each block's values, the current try_depth tells
     * us whether the block is inside a protected region.  We set
     * exception_handler *before* lowering (from the depth that was
     * active at block entry) so that codegen emits EH checks for
     * calls within this block. */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk)
            continue;
        XmBlock *xm_blk = get_block(&ctx, xi_blk);

        /* Snapshot handler before lowering (XI_TRY may change depth).
         * If this block is itself the handler of the topmost try (catch
         * or finally), look past it: a throw inside the handler must
         * propagate to the outer try, never re-enter the same handler
         * (otherwise CALL_C in the catch body would CBNZ back to the
         * top of the catch and spin forever). */
        XmBlock *handler_before = NULL;
        for (int td = ctx.try_depth - 1; td >= 0; td--) {
            XmBlock *cand = ctx.try_stack[td].handler;
            if (cand != xm_blk) {
                handler_before = cand;
                break;
            }
        }
        xm_blk->exception_handler = handler_before;

        lower_block_values(&ctx, xi_blk, xm_blk);
    }

    /* Resolve phi arguments (now all refs are populated) */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk)
            continue;
        XmBlock *xm_blk = get_block(&ctx, xi_blk);
        resolve_phi_args(&ctx, xi_blk, xm_blk);
    }

    /* Set block terminators */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk)
            continue;
        XmBlock *xm_blk = get_block(&ctx, xi_blk);
        lower_terminator(&ctx, xi_blk, xm_blk);
    }

    /* Cleanup */
    xr_free(ctx.slot_idx);
    xr_free(ctx.block_map);
    xr_free(ctx.ref_map);

    if (ctx.error) {
        xm_func_destroy(func);
        return NULL;
    }

    /* Post-pass: trim deopt snapshots to only live values.
     * Compute Xm-level liveness, then for each guard's snapshot
     * remove slots whose value vreg is dead at the guard point. */
    refine_deopt_liveness(func);

    return func;
}
