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
#include "../ir/xi_rep.h"
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
        XmBlock *handler;   /* catch (or finally) Xm block */
    } try_stack[XI2XM_MAX_TRY_DEPTH];
    int try_depth;

    /* Deopt snapshot counter (monotonically increasing) */
    uint16_t next_deopt_id;

    /* Direct-mapped index: value_id → slot_map entry index.
     * -1 = no entry.  Replaces linear scan in slot_map_bc_pc(). */
    int32_t *slot_idx;
    uint32_t slot_idx_size;

    bool error;
} LowerCtx;

/* ========== Helpers ========== */

/* Map Xi IR type to Xm machine representation.
 * Heap-allocated types (string, array, etc.) use PTR rep so codegen
 * correctly emits XR_TAG_PTR in call_arg_tags[]. */
static uint8_t xi_type_to_rep(struct XrType *type) {
    if (!type) return XR_REP_I64;
    switch (type->kind) {
        case XR_KIND_INT:       return XR_REP_I64;
        case XR_KIND_FLOAT:     return XR_REP_F64;
        case XR_KIND_BOOL:      return XR_REP_I64;
        case XR_KIND_NULL:      return XR_REP_I64;
        case XR_KIND_VOID:      return XR_REP_I64;
        case XR_KIND_STRING:
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_BYTES:
        case XR_KIND_CHANNEL:
        case XR_KIND_JSON:
        case XR_KIND_INSTANCE:
        case XR_KIND_INTERFACE:
        case XR_KIND_CLASS:
        case XR_KIND_FUNCTION:
        case XR_KIND_ENUM:      return XR_REP_PTR;
        default:                return XR_REP_TAGGED;
    }
}

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
    if (!ctx->slot_idx || value_id >= ctx->slot_idx_size) return -1;
    int32_t idx = ctx->slot_idx[value_id];
    if (idx < 0) return -1;
    return (int)ctx->slot_map->entries[idx].bc_pc;
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

    uint16_t did = ctx->next_deopt_id++;

    /* Grow deopt_infos if needed */
    if (!func->deopt_infos) {
        func->deopt_infos = (XmDeoptInfo *)xr_calloc(
            XM_MAX_DEOPT_POINTS, sizeof(XmDeoptInfo));
        if (!func->deopt_infos) return 0xFFFF;
    }

    XR_DCHECK(func->ndeopt < XM_MAX_DEOPT_POINTS,
              "record_deopt: deopt table overflow");
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
            if (e->value_id >= ctx->ref_map_size) continue;
            XmRef ref = ctx->ref_map[e->value_id];
            if (xm_ref_is_none(ref)) continue;
            latest_for_slot[e->bc_slot] = (int32_t)i;
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
        XmDeoptSlot *slots = (XmDeoptSlot *)xr_calloc(
            live_count ? live_count : 1, sizeof(XmDeoptSlot));
        if (slots) {
            uint16_t ns = 0;
            for (int s = 0; s < 256; s++) {
                if (latest_for_slot[s] < 0) continue;
                XiSlotMapEntry *e =
                    &ctx->slot_map->entries[latest_for_slot[s]];
                XmRef ref = ctx->ref_map[e->value_id];
                XR_DCHECK(ns < live_count,
                          "record_deopt: slot count mismatch");
                slots[ns].bc_slot = (int16_t)e->bc_slot;
                if (xm_ref_is_vreg(ref)) {
                    uint32_t vi = XM_REF_INDEX(ref);
                    slots[ns].rep = (vi < ctx->xm_func->nvreg)
                        ? ctx->xm_func->vregs[vi].rep : XR_REP_I64;
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
    if (!ctx->ic || !ctx->ic->ic_fields || bc_pc < 0) return NULL;
    XrICField *ic = xr_ic_field_table_get(ctx->ic->ic_fields, bc_pc);
    if (!ic) return NULL;
    /* Only speculate on monomorphic Json shape access */
    if (ic->json_shape_id == 0) return NULL;
    return ic;
}

/* Look up method IC for a given bytecode instruction offset.
 * Returns the IC entry if monomorphic (single klass), NULL otherwise. */
static const XrICMethod *ic_method_lookup(const LowerCtx *ctx, int bc_pc) {
    if (!ctx->ic || !ctx->ic->ic_methods || bc_pc < 0) return NULL;
    XrICMethod *ic = xr_ic_method_table_get(ctx->ic->ic_methods, bc_pc);
    if (!ic) return NULL;
    /* Only speculate on monomorphic call sites (exactly 1 klass seen) */
    if (ic->count != 1 || ic->is_megamorphic) return NULL;
    if (!ic->entries[0].klass || !ic->entries[0].method) return NULL;
    return ic;
}

/* ========== Constant Lowering ========== */

static XmRef lower_const(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->op == XI_CONST, "lower_const: not a constant");
    struct XrType *type = v->type;

    /* Constants must be materialized into vregs via XM_CONST_* instructions.
     * The codegen ret/branch handlers require vreg operands, not pool refs. */
    if (!type || type->kind == XR_KIND_INT || type->kind == XR_KIND_BOOL ||
        type->kind == XR_KIND_NULL) {
        XmRef cref = xm_const_i64(ctx->xm_func, v->aux_int);
        return xm_emit_unary(ctx->xm_func, blk, XM_CONST_I64, XR_REP_I64, cref);
    }
    if (type->kind == XR_KIND_FLOAT) {
        union { int64_t i; double d; } u;
        u.i = v->aux_int;
        XmRef cref = xm_const_f64(ctx->xm_func, u.d);
        return xm_emit_unary(ctx->xm_func, blk, XM_CONST_F64, XR_REP_F64, cref);
    }
    if (type->kind == XR_KIND_STRING) {
        /* v->aux is a raw C string from the Xi arena.  The JIT needs the
         * XrString* heap object (with GC header) so that jit_value_from_tag
         * can set heap_type correctly for runtime type checks (XR_IS_STRING).
         * Look up the matching XrString from the proto's constant pool. */
        const char *raw = (const char *)v->aux;
        void *xr_str = NULL;
        if (raw && ctx->proto) {
            XrValue *kpool = (XrValue *)ctx->proto->constants.data;
            int nk = ctx->proto->constants.count;
            for (int j = 0; j < nk; j++) {
                if (XR_IS_STRING(kpool[j])) {
                    XrString *s = XR_TO_STRING(kpool[j]);
                    if (s && strcmp(XR_STRING_CHARS(s), raw) == 0) {
                        xr_str = (void *)s;
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
    bool is_float = is_float_type(v->type);
    uint8_t rep = is_float ? XR_REP_F64 : XR_REP_I64;

    uint16_t xm_op;
    switch (v->op) {
        case XI_ADD:  xm_op = is_float ? XM_FADD : XM_ADD; break;
        case XI_SUB:  xm_op = is_float ? XM_FSUB : XM_SUB; break;
        case XI_MUL:  xm_op = is_float ? XM_FMUL : XM_MUL; break;
        case XI_DIV:  xm_op = is_float ? XM_FDIV : XM_DIV; break;
        case XI_MOD:  xm_op = XM_MOD; break;
        case XI_BAND: xm_op = XM_AND; break;
        case XI_BOR:  xm_op = XM_OR;  break;
        case XI_BXOR: xm_op = XM_XOR; break;
        case XI_SHL:  xm_op = XM_SHL; break;
        case XI_SHR:  xm_op = XM_SHR; break;
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
    bool is_float = is_float_type(v->type);
    uint8_t rep = is_float ? XR_REP_F64 : XR_REP_I64;

    uint16_t xm_op;
    switch (v->op) {
        case XI_NEG:  xm_op = is_float ? XM_FNEG : XM_NEG; break;
        case XI_BNOT: xm_op = XM_NOT; break;
        case XI_NOT:  xm_op = XM_NOT; break;
        default:
            ctx->error = true;
            return xm_const_i64(ctx->xm_func, 0);
    }

    /* Float unary needs F64 operand */
    if (is_float)
        arg = coerce_to_f64(ctx, blk, arg);

    return xm_fold_emit_unary(ctx->xm_func, blk, xm_op, rep, arg);
}

/* ========== Comparison Lowering ========== */

static XmRef lower_comparison(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 2, "comparison: expected 2 args");
    XmRef lhs = get_ref(ctx, v->args[0]);
    XmRef rhs = get_ref(ctx, v->args[1]);

    /* Determine if operands are float (check arg type, not result type) */
    bool is_float = is_float_type(v->args[0]->type);

    uint16_t xm_op;
    switch (v->op) {
        case XI_EQ: xm_op = is_float ? XM_FEQ : XM_EQ; break;
        case XI_NE: xm_op = is_float ? XM_FNE : XM_NE; break;
        case XI_LT: xm_op = is_float ? XM_FLT : XM_LT; break;
        case XI_LE: xm_op = is_float ? XM_FLE : XM_LE; break;
        case XI_GT: xm_op = is_float ? XM_FLT : XM_LT; break;
        case XI_GE: xm_op = is_float ? XM_FLE : XM_LE; break;
        case XI_EQ_STRICT: xm_op = XM_EQ; break;
        case XI_NE_STRICT: xm_op = XM_NE; break;
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

    if (!src_float && dst_float) {
        /* int → float */
        return xm_emit_unary(ctx->xm_func, blk, XM_I2F, XR_REP_F64, arg);
    } else if (src_float && !dst_float) {
        /* float → int */
        return xm_emit_unary(ctx->xm_func, blk, XM_F2I, XR_REP_I64, arg);
    }
    /* Same type — identity */
    return arg;
}

static XmRef lower_box(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "box: expected 1 arg");
    XmRef arg = get_ref(ctx, v->args[0]);
    struct XrType *src_type = v->args[0]->type;

    if (is_float_type(src_type)) {
        return xm_emit_unary(ctx->xm_func, blk, XM_BOX_F64, XR_REP_I64, arg);
    }
    return xm_emit_unary(ctx->xm_func, blk, XM_BOX_I64, XR_REP_I64, arg);
}

static XmRef lower_unbox(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "unbox: expected 1 arg");
    XmRef arg = get_ref(ctx, v->args[0]);

    if (is_float_type(v->type)) {
        return xm_emit_unary(ctx->xm_func, blk, XM_UNBOX_F64, XR_REP_F64, arg);
    }
    return xm_emit_unary(ctx->xm_func, blk, XM_UNBOX_I64, XR_REP_I64, arg);
}

/* ========== Call / Closure / Print Lowering ========== */

/* Find the XrProto for a child XiFunc by scanning parent proto's sub-protos.
 * Returns NULL if not found (e.g. cross-module call). */
static struct XrProto *find_callee_proto(LowerCtx *ctx, XiFunc *child_xi) {
    if (!ctx->proto || !child_xi) return NULL;
    uint32_t n = ctx->proto->protos.count;
    for (uint32_t i = 0; i < n; i++) {
        struct XrProto *sub = PROTO_PROTO(ctx->proto, i);
        if (sub && sub->xi_func == child_xi) return sub;
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
            XiFunc *child_xi = (XiFunc *)callee_val->aux;
            struct XrProto *callee_proto = find_callee_proto(ctx, child_xi);
            if (callee_proto) {
                uint8_t ret_rep = callee_proto->return_type_info
                    ? xr_type_rep(callee_proto->return_type_info)
                    : XR_REP_TAGGED;
                XmRef proto_ref = xm_const_ptr(ctx->xm_func,
                                                  (void *)callee_proto);
                XmRef nargs_ref = xm_const_i64(ctx->xm_func,
                                                  (int64_t)nargs);
                XmRef result = xm_emit(ctx->xm_func, blk,
                                          XM_CALL_KNOWN, ret_rep,
                                          proto_ref, nargs_ref);
                blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
                xm_func_bind_call_args(ctx->xm_func, result,
                                         call_args, total);
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
            XR_DCHECK(klass != NULL && method != NULL,
                      "method IC entry must be non-null");

            /* Only speculate on closure methods with a valid proto */
            if (method->type == XMETHOD_CLOSURE && method->as.closure &&
                method->as.closure->proto) {
                struct XrProto *callee_proto = method->as.closure->proto;

                /* Emit GUARD_KLASS on receiver (args[0]) */
                XmRef recv = call_args[0];
                uint16_t did = record_deopt(ctx, (uint32_t)bc_pc);
                if (did == 0xFFFF) goto generic_call;  /* deopt overflow */
                XmRef klass_ref = xm_const_ptr(ctx->xm_func, (void *)klass);
                XmRef deopt_ref = xm_const_i64(ctx->xm_func, (int64_t)did);
                xm_emit(ctx->xm_func, blk, XM_GUARD_KLASS, XR_REP_I64,
                         recv, klass_ref);
                blk->ins[blk->nins - 1].dst = deopt_ref;
                blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;

                /* Emit CALL_KNOWN with the IC-resolved proto */
                uint8_t ret_rep = callee_proto->return_type_info
                    ? xr_type_rep(callee_proto->return_type_info)
                    : XR_REP_TAGGED;
                XmRef proto_ref = xm_const_ptr(ctx->xm_func,
                                                  (void *)callee_proto);
                XmRef nargs_c = xm_const_i64(ctx->xm_func, (int64_t)nargs);
                XmRef result = xm_emit(ctx->xm_func, blk,
                                          XM_CALL_KNOWN, ret_rep,
                                          proto_ref, nargs_c);
                blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
                xm_func_bind_call_args(ctx->xm_func, result,
                                         call_args, total);
                return result;
            }
        }
    }

generic_call:
    /* XI_CALL_BUILTIN (dump, copy, StringBuilder, Bytes, cancelled):
     * these are specialized VM ops that don't use xr_jit_invoke_method.
     * Emit unconditional deopt so the interpreter handles them. */
    if (v->op == XI_CALL_BUILTIN) {
        int bc_pc = slot_map_bc_pc(ctx, v->id);
        uint16_t did = record_deopt(ctx, (uint32_t)bc_pc);
        XmRef deopt_id = xm_const_i64(ctx->xm_func, (int64_t)did);
        xm_emit(ctx->xm_func, blk, XM_DEOPT, XR_REP_I64,
                 deopt_id, XM_NONE);
        blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
        return xm_const_i64(ctx->xm_func, 0);
    }

    /* Method calls (XI_CALL_METHOD) go through xr_jit_invoke_method
     * which resolves the method on the receiver at runtime. */
    if (v->op == XI_CALL_METHOD) {
        /* aux_int = (global_symbol_id << 1) | is_super.
         * The SymbolId is resolved at lowering time (main thread) so the
         * JIT background thread does not need isolate access. */
        int method_sym = (int)(v->aux_int >> 1);
        XR_DCHECK(method_sym > 0,
                  "XI_CALL_METHOD: method_sym=0, lowering failed to "
                  "resolve SymbolId");
        int bc_pc = slot_map_bc_pc(ctx, v->id);
        uint16_t did = record_deopt(ctx, (uint32_t)bc_pc);
        int64_t encoded = ((int64_t)method_sym << 32) |
                          ((int64_t)(did & 0xFFFF) << 16) |
                          ((int64_t)nargs & 0xFF);
        XmRef encoded_ref = xm_const_i64(ctx->xm_func, encoded);
        XmRef fn_ref = xm_const_ptr(ctx->xm_func,
                                       (void *)xr_jit_invoke_method);
        /* Method returns are polymorphic (int, bool, string, null, ptr).
         * Use I64 rep (raw payload in GP register) + TAGGED ctype so
         * the type pass does not narrow, allowing the dynamic tag patch
         * to read the correct runtime tag from slot_runtime_tags[]. */
        XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_C,
                                  XR_REP_I64, fn_ref, encoded_ref);
        blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
        blk->ins[blk->nins - 1].ctype = (XmType){XM_TK_TAGGED, 0, 0};
        xm_func_bind_call_args(ctx->xm_func, result, call_args, total);
        /* Propagate bc_slot from Xi slot_map so the codegen can store
         * the dynamic return tag into slot_runtime_tags[]. Without this,
         * Xi IR vregs default to bc_slot=-1 and the tag is lost. */
        if (xm_ref_is_vreg(result) && ctx->slot_idx &&
            v->id < ctx->slot_idx_size) {
            int32_t si = ctx->slot_idx[v->id];
            if (si >= 0) {
                uint32_t vi = XM_REF_INDEX(result);
                if (vi < ctx->xm_func->nvreg)
                    ctx->xm_func->vregs[vi].bc_slot =
                        (int16_t)ctx->slot_map->entries[si].bc_slot;
            }
        }
        return result;
    }

    /* Fallback: generic call via xr_jit_call_func bridge */
    XmRef nargs_ref = xm_const_i64(ctx->xm_func, (int64_t)nargs);
    XmRef nargs_val = xm_emit_unary(ctx->xm_func, blk, XM_CONST_I64,
                                       XR_REP_I64, nargs_ref);
    XmRef fn_ref = xm_const_ptr(ctx->xm_func, (void *)xr_jit_call_func);
    XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_DIRECT,
                              XR_REP_I64, nargs_val, fn_ref);
    blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
    xm_func_bind_call_args(ctx->xm_func, result, call_args, total);
    return result;
}

static XmRef lower_closure_new(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    /* XI_CLOSURE_NEW: aux=child XiFunc*, args=capture values */
    XmRef proto_ref = xm_const_ptr(ctx->xm_func, v->aux);
    XmRef fn_ref = xm_const_ptr(ctx->xm_func, (void *)xr_jit_closure_new);
    XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_C,
                              XR_REP_I64, proto_ref, fn_ref);
    blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
    return result;
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
        case XI_ADD: case XI_SUB: case XI_MUL: case XI_DIV: case XI_MOD:
        case XI_BAND: case XI_BOR: case XI_BXOR: case XI_SHL: case XI_SHR:
            return lower_binary_arith(ctx, blk, v);

        /* Unary */
        case XI_NEG: case XI_BNOT: case XI_NOT:
            return lower_unary(ctx, blk, v);

        /* Comparison */
        case XI_EQ: case XI_NE: case XI_LT: case XI_LE: case XI_GT: case XI_GE:
        case XI_EQ_STRICT: case XI_NE_STRICT:
            return lower_comparison(ctx, blk, v);

        /* Type conversion */
        case XI_CONVERT:
            return lower_convert(ctx, blk, v);
        case XI_BOX:
            return lower_box(ctx, blk, v);
        case XI_UNBOX:
            return lower_unbox(ctx, blk, v);

        /* Null check */
        case XI_ISNULL: {
            XR_DCHECK(v->nargs == 1, "isnull: expected 1 arg");
            XmRef arg = get_ref(ctx, v->args[0]);
            XmRef zero = xm_const_i64(ctx->xm_func, 0);
            return xm_emit(ctx->xm_func, blk, XM_EQ, XR_REP_I64, arg, zero);
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
            XmRef fn_ref = xm_const_ptr(ctx->xm_func,
                                           (void *)xr_jit_get_shared);
            XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_C,
                                      XR_REP_I64, fn_ref, idx);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            /* Shared vars are dynamically typed: set XM_TK_TAGGED to
             * prevent the type pass from narrowing to I64, which would
             * skip the slot_runtime_tags[] dynamic tag patch. */
            blk->ins[blk->nins - 1].ctype = (XmType){XM_TK_TAGGED, 0, 0};
            /* Propagate bc_slot for runtime tag resolution */
            if (xm_ref_is_vreg(result) && ctx->slot_idx &&
                v->id < ctx->slot_idx_size) {
                int32_t si = ctx->slot_idx[v->id];
                if (si >= 0) {
                    uint32_t vi = XM_REF_INDEX(result);
                    if (vi < ctx->xm_func->nvreg)
                        ctx->xm_func->vregs[vi].bc_slot =
                            (int16_t)ctx->slot_map->entries[si].bc_slot;
                }
            }
            return result;
        }
        case XI_SET_SHARED: {
            XR_DCHECK(v->nargs == 1, "set_shared: expected 1 arg");
            XmRef val = get_ref(ctx, v->args[0]);
            XmRef idx = xm_const_i64(ctx->xm_func, v->aux_int);
            xm_emit(ctx->xm_func, blk, XM_STORE, XR_REP_I64, idx, val);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            return val;
        }

        /* Upvalue access */
        case XI_LOAD_UPVAL: {
            XmRef idx = xm_const_i64(ctx->xm_func, v->aux_int);
            return xm_emit_unary(ctx->xm_func, blk, XM_LOAD, XR_REP_I64, idx);
        }
        case XI_STORE_UPVAL: {
            XR_DCHECK(v->nargs == 1, "store_upval: expected 1 arg");
            XmRef val = get_ref(ctx, v->args[0]);
            XmRef idx = xm_const_i64(ctx->xm_func, v->aux_int);
            xm_emit(ctx->xm_func, blk, XM_STORE, XR_REP_I64, idx, val);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            return val;
        }

        /* Method call — same as generic call for now */
        case XI_CALL_METHOD:
        case XI_CALL_BUILTIN:
            return lower_call(ctx, blk, v);

        /* Extract multi-return result — placeholder (returns call ref) */
        case XI_EXTRACT: {
            XR_DCHECK(v->nargs == 1, "extract: expected 1 arg");
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
                uint16_t did = record_deopt(ctx, (uint32_t)bc_pc);
                if (did != 0xFFFF) {
                    XmRef shape_id = xm_const_i64(ctx->xm_func,
                                                     (int64_t)fic->json_shape_id);
                    XmRef deopt_ref = xm_const_i64(ctx->xm_func, (int64_t)did);
                    xm_emit(ctx->xm_func, blk,
                             XM_GUARD_SHAPE, XR_REP_I64,
                             obj, shape_id);
                    blk->ins[blk->nins - 1].dst = deopt_ref;
                    blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;

                    /* Direct field load at known offset */
                    int64_t byte_off = XM_JSON_FIELDS_OFFSET
                                       + (int64_t)fic->json_field_idx * XM_XRVALUE_SIZE;
                    XmRef off = xm_const_i64(ctx->xm_func, byte_off);
                    return xm_emit(ctx->xm_func, blk,
                                     XM_LOAD_FIELD, XR_REP_I64, obj, off);
                }
                /* deopt overflow: fall through to generic path */
            }

            /* Generic fallback: offset from aux_int */
            XmRef off = xm_const_i64(ctx->xm_func, v->aux_int);
            return xm_emit(ctx->xm_func, blk, XM_LOAD_FIELD, XR_REP_I64, obj, off);
        }
        case XI_STORE_FIELD: {
            XR_DCHECK(v->nargs >= 2, "store_field: need obj + val");
            XmRef obj = get_ref(ctx, v->args[0]);
            XmRef val = get_ref(ctx, v->args[1]);
            XmRef off = xm_const_i64(ctx->xm_func, v->aux_int);
            xm_emit(ctx->xm_func, blk, XM_STORE_FIELD, XR_REP_I64, off, obj);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            /* Bind val as extra arg for codegen */
            XmRef args[2] = { obj, val };
            (void)args;
            return val;
        }

        /* Index access */
        case XI_INDEX_GET: {
            XR_DCHECK(v->nargs >= 2, "index_get: need obj + key");
            XmRef obj = get_ref(ctx, v->args[0]);
            XmRef key = get_ref(ctx, v->args[1]);
            return xm_emit(ctx->xm_func, blk, XM_RT_INDEX_GET, XR_REP_I64, obj, key);
        }
        case XI_INDEX_SET: {
            XR_DCHECK(v->nargs >= 3, "index_set: need obj + key + val");
            XmRef obj = get_ref(ctx, v->args[0]);
            XmRef key = get_ref(ctx, v->args[1]);
            XmRef val = get_ref(ctx, v->args[2]);
            XmRef result = xm_emit(ctx->xm_func, blk, XM_RT_INDEX_SET,
                                      XR_REP_I64, obj, key);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            (void)val;
            return result;
        }

        /* Array / Map creation */
        case XI_ARRAY_NEW: {
            XmRef cap = (v->nargs >= 1) ? get_ref(ctx, v->args[0])
                                         : xm_const_i64(ctx->xm_func, 0);
            return xm_emit_unary(ctx->xm_func, blk, XM_RT_ARRAY_NEW, XR_REP_I64, cap);
        }
        case XI_MAP_NEW: {
            XmRef cap = (v->nargs >= 1) ? get_ref(ctx, v->args[0])
                                         : xm_const_i64(ctx->xm_func, 0);
            return xm_emit_unary(ctx->xm_func, blk, XM_RT_MAP_NEW, XR_REP_I64, cap);
        }

        /* String concatenation — lower as sequential RT calls */
        case XI_STR_CONCAT: {
            if (v->nargs == 0) return xm_const_i64(ctx->xm_func, 0);
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
            XmRef fn_ptr = xm_const_ptr(ctx->xm_func,
                                           (void *)xr_jit_throw);
            xm_emit(ctx->xm_func, blk, XM_CALL_C,
                     XR_REP_I64, fn_ptr, val);
            blk->ins[blk->nins - 1].flags |=
                XM_FLAG_SIDE_EFFECT | XM_FLAG_MAY_THROW;
            return xm_const_i64(ctx->xm_func, 0);
        }

        /* Iteration — lower as generic calls (runtime handles protocol) */
        case XI_ITER_NEW:
        case XI_ITER_NEXT:
        case XI_ITER_VALID:
            return lower_call(ctx, blk, v);

        /* Coroutine ops — lower as generic calls */
        case XI_GO:
        case XI_AWAIT:
        case XI_CHAN_SEND:
        case XI_CHAN_RECV:
        case XI_YIELD:
        case XI_CHAN_NEW:
            return lower_call(ctx, blk, v);

        /* Defer — not yet supported in JIT path, skip gracefully */
        case XI_DEFER:
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
            XmRef cap = (v->nargs >= 1) ? get_ref(ctx, v->args[0])
                                         : xm_const_i64(ctx->xm_func, 0);
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

        /* Multi-return packaging */
        case XI_MULTI_RET:
            if (v->nargs >= 1) return get_ref(ctx, v->args[0]);
            return xm_const_i64(ctx->xm_func, 0);

        /* Identity / type narrowing — transparent passthrough */
        case XI_COPY:
            if (v->nargs >= 1) return get_ref(ctx, v->args[0]);
            return xm_const_i64(ctx->xm_func, 0);

        /* Builtins lowered as generic runtime calls */
        case XI_ASSERT:
        case XI_ASSERT_EQ:
        case XI_ASSERT_NE:
        case XI_TYPEOF:
        case XI_GET_BUILTIN:
        case XI_CLASS_CREATE:
            return lower_call(ctx, blk, v);

        /* Structured concurrency scope — runtime calls */
        case XI_SCOPE_ENTER:
        case XI_SCOPE_EXIT:
            return lower_call(ctx, blk, v);

        /* Exception handling */
        case XI_TRY: {
            /* aux = catch XiBlock*, aux_int = has_finally flag.
             * Push handler onto try stack; exception_handler is set
             * on blocks in the propagation pass after lowering. */
            XiBlock *catch_xi = (XiBlock *)v->aux;
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
            XmRef exc = xm_emit_unary(ctx->xm_func, blk,
                                         XM_CATCH, XR_REP_I64, XM_NONE);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            return exc;
        }

        case XI_FINALLY:
            /* Marker only — no Xm instruction needed */
            return xm_const_i64(ctx->xm_func, 0);

        case XI_END_TRY: {
            if (ctx->try_depth > 0)
                ctx->try_depth--;
            xm_emit(ctx->xm_func, blk, XM_TRY_END,
                     XR_REP_I64, XM_NONE, XM_NONE);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            return xm_const_i64(ctx->xm_func, 0);
        }

        /* Cross-module import ref — resolved at AOT cgen time, not JIT */
        case XI_IMPORT_REF:
            return xm_const_i64(ctx->xm_func, 0);

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
        uint8_t rep = xi_type_to_rep(pv->type);
        XmPhi *xm_phi = xm_add_phi(ctx->xm_func, xm_blk, rep);
        XR_DCHECK(xm_phi != NULL, "lower_phis: xm_add_phi returned NULL");
        set_ref(ctx, pv->id, xm_phi->dst);
    }
}

/* Set phi arguments after all blocks are lowered (all refs resolved) */
static void resolve_phi_args(LowerCtx *ctx, XiBlock *xi_blk, XmBlock *xm_blk) {
    uint32_t pred_idx = 0;
    (void)pred_idx;

    for (XiPhi *phi = xi_blk->phis; phi; phi = phi->next) {
        XiValue *pv = &phi->value;
        XR_DCHECK(pv->nargs == xi_blk->npreds,
                  "phi arg count must match predecessor count");

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

/* Lower a single block's instructions */
static void lower_block_values(LowerCtx *ctx, XiBlock *xi_blk, XmBlock *xm_blk) {
    for (uint32_t i = 0; i < xi_blk->nvalues; i++) {
        XiValue *v = xi_blk->values[i];
        if (!v) continue;
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
                ret_val = xm_const_i64(ctx->xm_func, 0);
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
    return op == XM_GUARD_TAG || op == XM_GUARD_BOUNDS ||
           op == XM_GUARD_NONNULL || op == XM_GUARD_CLASS ||
           op == XM_GUARD_KLASS || op == XM_GUARD_SHAPE ||
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
    return (uint16_t)func->consts[ci].val.raw;
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
    if (!live.blocks) return;  /* allocation failure — skip refinement */

    /* Per-instruction live set (reused across blocks) */
    XmBSet cur;
    xm_bset_init(&cur, func->nvreg);

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk || blk->nins == 0) continue;

        /* Start from live_out of this block */
        xm_bset_copy(&cur, &live.blocks[bi].live_out);

        /* Walk instructions backward */
        for (int32_t ii = (int32_t)blk->nins - 1; ii >= 0; ii--) {
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
                                keep = (vi < func->nvreg) &&
                                       xm_bset_has(&cur, vi);
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

XR_FUNC struct XmFunc *xi_to_xm_lower(XiFunc *xi_func,
                                          struct XrProto *proto,
                                          XiSlotMap *slot_map,
                                          const XmICSnapshot *ic,
                                          struct XrayIsolate *isolate) {
    XR_DCHECK(xi_func != NULL, "xi_to_xm_lower: NULL xi_func");

    XmFunc *func = xm_func_new(xi_func->name);
    if (!func) return NULL;

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
    ctx.block_map = (XmBlock **)xr_calloc(ctx.block_map_size, sizeof(XmBlock *));
    if (!ctx.block_map) { xm_func_destroy(func); return NULL; }

    /* Allocate ref map */
    ctx.ref_map_size = xi_func->next_value_id;
    ctx.ref_map = (XmRef *)xr_calloc(ctx.ref_map_size, sizeof(XmRef));
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
        ctx.slot_idx = (int32_t *)xr_malloc(
            ctx.slot_idx_size * sizeof(int32_t));
        if (ctx.slot_idx) {
            memset(ctx.slot_idx, 0xFF,
                   ctx.slot_idx_size * sizeof(int32_t));
            for (uint32_t i = 0; i < slot_map->count; i++) {
                uint32_t vid = slot_map->entries[i].value_id;
                if (vid < ctx.slot_idx_size)
                    ctx.slot_idx[vid] = (int32_t)i;
            }
        }
    }

    /* Allocate param vregs FIRST: codegen prologue assumes consecutive
     * vregs 0..num_params-1 correspond to function arguments loaded from
     * the args_ptr array. Must precede any other xm_new_vreg calls. */
    func->num_params = xi_func->nparams;
    for (uint16_t i = 0; i < xi_func->nparams; i++) {
        XiValue *param = xi_func->params[i];
        uint8_t rep = param ? xi_type_to_rep(param->type) : XR_REP_I64;
        XmRef vreg = xm_new_vreg(func, rep);
        if (param)
            set_ref(&ctx, param->id, vreg);
    }

    /* Create all XmBlocks upfront (so forward jumps resolve) */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk) continue;
        XmBlock *xm_blk = xm_func_add_block(func, NULL);
        XR_DCHECK(xm_blk != NULL, "xi_to_xm_lower: block allocation failed");
        ctx.block_map[xi_blk->id] = xm_blk;
    }

    /* Set up predecessor edges */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk) continue;
        XmBlock *xm_blk = get_block(&ctx, xi_blk);
        for (uint16_t p = 0; p < xi_blk->npreds; p++) {
            XmBlock *pred = get_block(&ctx, xi_blk->preds[p]);
            xm_block_add_pred(xm_blk, pred, func->arena);
        }
    }

    /* Lower phi nodes (create XmPhi with dst, defer arg resolution) */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk) continue;
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
        if (!xi_blk) continue;
        XmBlock *xm_blk = get_block(&ctx, xi_blk);

        /* Snapshot handler before lowering (XI_TRY may change depth) */
        XmBlock *handler_before = (ctx.try_depth > 0)
            ? ctx.try_stack[ctx.try_depth - 1].handler : NULL;
        xm_blk->exception_handler = handler_before;

        lower_block_values(&ctx, xi_blk, xm_blk);
    }

    /* Resolve phi arguments (now all refs are populated) */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk) continue;
        XmBlock *xm_blk = get_block(&ctx, xi_blk);
        resolve_phi_args(&ctx, xi_blk, xm_blk);
    }

    /* Set block terminators */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk) continue;
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
