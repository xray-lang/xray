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
#include "xi_to_xm_dispatch_gen.h"
#include "xm.h"
#include "xm_ops.h"
#include "xm_jit_runtime.h"
#include "xm_helper_table.h"
#include "xm_fold.h"
#include "xm_codegen.h"
#include "xm_liveness2.h"
#include "xm_offsets.h"
#include "../ir/xi_opt.h"
#include "../ir/xi_op_name.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/object/xstring.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../vm/xic_field.h"
#include "../vm/xic_field_table.h"
#include "../vm/xic_method.h"
#include "../runtime/class/xmethod.h"
#include "../runtime/closure/xclosure.h"
#include "xm_pic.h"
#include <string.h>

/* ========== Lowering Context ========== */

#define XI2XM_TRY_STACK_INIT_CAP 8
/* Helper-call arg buffer is bounded by the per-worker XrJitScratch.call_args[16]
 * ABI array (15 params + closure), so this is a real ABI limit, not a soft cap:
 * lowering a wider helper call cleanly rejects (caller falls back to the
 * interpreter) rather than overrunning the fixed scratch layout. */
#define XI2XM_MAX_HELPER_CALL_ARGS 15
#define XI2XM_NO_BC_SLOT 255
#define XI2XM_CHAN_NEW_DYNAMIC_CAP_FLAG (1ULL << 40)

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

    /* EH: try nesting stack — active exception handler blocks, grown on
     * demand so deeply nested try/catch never silently drops a handler. */
    XmBlock **try_stack;
    int try_depth;
    int try_cap;

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
    uint16_t error_op;
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

static uint8_t helper_call_rep(XmHelperId id) {
    if (id >= XM_HELPER__COUNT)
        return XR_REP_I64;
    uint8_t ret_rep = xm_helper_info[id].ret_rep;
    if (ret_rep == XR_REP_F64 || ret_rep == XR_REP_PTR || ret_rep == XR_REP_I64)
        return ret_rep;
    return XR_REP_I64;
}

static XmType helper_call_ctype(XmHelperId id) {
    if (id >= XM_HELPER__COUNT)
        return XM_TYPE_UNKNOWN;
    if (xm_helper_info[id].ret_rep == XR_REP_PTR && xm_helper_pointer_trust(id) != XM_HPT_GC)
        return XM_TYPE_UNKNOWN;
    return (XmType) {xm_helper_type_kind(id), 0, 0};
}

static XmRef emit_helper_call(LowerCtx *ctx, XmBlock *blk, XmHelperId id, XmRef extra,
                              const XmRef *args, uint16_t nargs) {
    XR_DCHECK(ctx != NULL && ctx->xm_func != NULL, "emit_helper_call: invalid ctx");
    XR_DCHECK(blk != NULL, "emit_helper_call: NULL block");
    XR_DCHECK(id < XM_HELPER__COUNT, "emit_helper_call: invalid helper id");
    XmRef fn_ref = xm_const_ptr(ctx->xm_func, xm_helper_info[id].func);
    XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_C, helper_call_rep(id), fn_ref, extra);
    XmIns *ins = &blk->ins[blk->nins - 1];
    ins->ctype = helper_call_ctype(id);
    if (xm_helper_may_gc(id))
        ins->flags |= XM_FLAG_MAY_GC;
    if (xm_helper_needs_stackmap(id))
        ins->flags |= XM_FLAG_SAFEPOINT;
    if (xm_helper_may_throw(id) || (xm_helper_post_call(id) & XM_HPC_THROW))
        ins->flags |= XM_FLAG_MAY_THROW;
    if (args && nargs > 0)
        xm_func_bind_call_args(ctx->xm_func, result, args, nargs);
    return result;
}

static XmRef emit_channel_suspend_call(LowerCtx *ctx, XmBlock *blk, XmHelperId helper,
                                       XmHelperId block_helper, XmRef extra, const XmRef *args,
                                       uint16_t nargs) {
    XmRef call = emit_helper_call(ctx, blk, helper, extra, args, nargs);
    XmRef discard = xm_const_i64(ctx->xm_func, 0);
    XmRef suspend = xm_emit(ctx->xm_func, blk, XM_SUSPEND, helper_call_rep(helper), call, discard);
    blk->ins[blk->nins - 1].ctype = helper_call_ctype(helper);
    if (xm_ref_is_vreg(suspend)) {
        uint32_t vi = XM_REF_INDEX(suspend);
        if (vi < ctx->xm_func->nvreg) {
            uint32_t sid = ctx->xm_func->vregs[vi].call_arg_start;
            if (sid < XM_MAX_SUSPEND_ENTRIES)
                ctx->xm_func->suspend_block_helpers[sid] = xm_helper_func(block_helper);
        }
    }
    return suspend;
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

static void set_result_slot_metadata(LowerCtx *ctx, uint32_t value_id, XmRef ref) {
    if (!ctx || !ctx->xm_func || !xm_ref_is_vreg(ref))
        return;
    if (!ctx->slot_idx || value_id >= ctx->slot_idx_size)
        return;
    int32_t si = ctx->slot_idx[value_id];
    if (si < 0 || ctx->slot_map->entries[si].bc_slot == XI2XM_NO_BC_SLOT)
        return;
    uint32_t vi = XM_REF_INDEX(ref);
    if (vi >= ctx->xm_func->nvreg)
        return;
    ctx->xm_func->vregs[vi].bc_slot = (int16_t) ctx->slot_map->entries[si].bc_slot;
}

static uint8_t xi_value_rep_or_tagged(const XiValue *v) {
    return v ? v->rep : XR_REP_TAGGED;
}

static XrType *xi_static_type_for_value(const LowerCtx *ctx, const XiValue *v) {
    if (v && v->type && v->type->kind != XR_KIND_UNKNOWN)
        return v->type;
    if (!ctx || !ctx->proto || !v || v->op != XI_PARAM || v->aux_int < 0)
        return NULL;
    uint32_t param_index = (uint32_t) v->aux_int;
    if (!ctx->proto->param_types || param_index >= ctx->proto->param_types_count)
        return NULL;
    return ctx->proto->param_types[param_index];
}

static bool xi_value_is_static_channel(const LowerCtx *ctx, const XiValue *v) {
    XrType *type = xi_static_type_for_value(ctx, v);
    return type && type->kind == XR_KIND_CHANNEL;
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

    /* Refuse deopt without a valid bytecode anchor.  slot_map_bc_pc
     * returns -1 (UINT32_MAX once cast to uint32_t) when the Xi value
     * has no corresponding bytecode slot; pre-push and recovery would
     * then index proto->code at index 0xFFFFFFFF and crash. */
    if (bc_pc == UINT32_MAX)
        return 0xFFFF;

    uint16_t did = ctx->next_deopt_id++;

    /* Grow deopt_infos on demand — no fixed ceiling. deopt_id stays in lockstep
     * with the entry index because this is the only path that appends, and it
     * never appends without also bumping next_deopt_id. */
    if (func->ndeopt >= func->deopt_cap) {
        uint32_t new_cap = func->deopt_cap ? func->deopt_cap * 2 : XM_DEOPT_INFOS_INIT_CAP;
        XmDeoptInfo *nt =
            (XmDeoptInfo *) xr_realloc(func->deopt_infos, new_cap * sizeof(XmDeoptInfo));
        if (!nt)
            return 0xFFFF;
        func->deopt_infos = nt;
        func->deopt_cap = new_cap;
    }

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
            if (e->bc_slot == XI2XM_NO_BC_SLOT)
                continue;
            latest_for_slot[e->bc_slot] = (int32_t) i;
        }

        /* Count unique live bc_slots */
        uint32_t live_count = 0;
        for (int s = 0; s < XI2XM_NO_BC_SLOT; s++) {
            if (latest_for_slot[s] >= 0)
                live_count++;
        }

        /* Pass 2: populate slots from deduplicated entries (no slot ceiling —
         * runtime deopt safepoints allocate slots per entry, see
         * a64_build_deopt_safepoints). */
        XmDeoptSlot *slots =
            (XmDeoptSlot *) xr_calloc(live_count ? live_count : 1, sizeof(XmDeoptSlot));
        if (slots) {
            uint16_t ns = 0;
            for (int s = 0; s < XI2XM_NO_BC_SLOT; s++) {
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

/* Attach a PIC to the XmFunc from an IC method table entry.
 * Returns the pic_table index (used as PIC ID in codegen), or -1 on failure. */
static int attach_pic(LowerCtx *ctx, const XrICMethod *ic) {
    if (!ic || ic->count < 2 || ic->is_megamorphic)
        return -1;
    XmFunc *func = ctx->xm_func;
    if (func->npic >= func->pic_cap) {
        uint32_t new_cap = func->pic_cap ? func->pic_cap * 2 : 4;
        struct XmPic *new_tbl = xr_malloc(new_cap * sizeof(struct XmPic));
        if (!new_tbl)
            return -1;
        if (func->pic_table) {
            memcpy(new_tbl, func->pic_table, func->npic * sizeof(struct XmPic));
            xr_free(func->pic_table);
        }
        func->pic_table = new_tbl;
        func->pic_cap = new_cap;
    }
    uint32_t idx = func->npic++;
    xm_pic_import_ic_method(&func->pic_table[idx], ic);
    return (int) idx;
}

/* Look up method IC for a given bytecode instruction offset.
 * Returns the IC entry if monomorphic (single klass), NULL otherwise. */
static int ic_method_speculate_index(const XrICMethod *ic) {
    if (!ic || ic->count == 0 || ic->is_megamorphic)
        return -1;
    if (ic->count == 1)
        return 0;

    int best = 0;
    for (int i = 1; i < ic->count; i++) {
        if (ic->entries[i].hit_count > ic->entries[best].hit_count)
            best = i;
    }
    return best;
}

static const XrICMethod *ic_method_lookup(const LowerCtx *ctx, int bc_pc) {
    if (!ctx->ic || !ctx->ic->ic_methods || bc_pc < 0)
        return NULL;
    XrICMethod *ic = xr_ic_method_table_get(ctx->ic->ic_methods, bc_pc);
    if (!ic)
        return NULL;
    if (ic->count == 0 || ic->is_megamorphic)
        return NULL;
    int idx = ic_method_speculate_index(ic);
    if (idx < 0)
        return NULL;
    if (!ic->entries[idx].klass || !ic->entries[idx].method)
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

static XmRef lower_binary_arith_with_ops(LowerCtx *ctx, XmBlock *blk, XiValue *v, uint16_t int_op,
                                         uint16_t float_op) {
    XR_DCHECK(v->nargs == 2, "binary arith: expected 2 args");
    XmRef lhs = get_ref(ctx, v->args[0]);
    XmRef rhs = get_ref(ctx, v->args[1]);
    bool is_float = (v->rep == XR_REP_F64);
    uint8_t rep = v->rep;

    if (is_float && float_op == XM_OP_COUNT) {
        ctx->error = true;
        return xm_const_i64(ctx->xm_func, 0);
    }

    uint16_t xm_op = is_float ? float_op : int_op;

    /* Float operations require F64 operands; coerce if needed
     * (e.g. Json property access returns I64/TAGGED). */
    if (is_float) {
        lhs = coerce_to_f64(ctx, blk, lhs);
        rhs = coerce_to_f64(ctx, blk, rhs);
    }

    return xm_fold_emit(ctx->xm_func, blk, xm_op, rep, lhs, rhs);
}

static XmRef lower_unary_with_ops(LowerCtx *ctx, XmBlock *blk, XiValue *v, uint16_t int_op,
                                  uint16_t float_op) {
    XR_DCHECK(v->nargs == 1, "unary: expected 1 arg");
    XmRef arg = get_ref(ctx, v->args[0]);
    bool is_float = (v->rep == XR_REP_F64);
    uint8_t rep = v->rep;

    if (is_float && float_op == XM_OP_COUNT) {
        ctx->error = true;
        return xm_const_i64(ctx->xm_func, 0);
    }

    uint16_t xm_op = is_float ? float_op : int_op;

    /* Float unary needs F64 operand */
    if (is_float)
        arg = coerce_to_f64(ctx, blk, arg);

    return xm_fold_emit(ctx->xm_func, blk, xm_op, rep, arg, XM_NONE);
}

static XmRef lower_truthy_value(LowerCtx *ctx, XmBlock *blk, XiValue *arg_v) {
    XmRef arg = get_ref(ctx, arg_v);
    XmRef zero = xm_const_i64(ctx->xm_func, 0);

    if (ref_rep(ctx, arg) == XR_REP_TAGGED) {
        XmRef args[1] = {arg};
        return emit_helper_call(ctx, blk, XM_HELPER_rt_truthy, zero, args, (uint16_t) 1);
    }

    if (arg_v->type && arg_v->type->kind == XR_KIND_NULL)
        return zero;

    if (is_float_type(arg_v->type) || ref_rep(ctx, arg) == XR_REP_F64) {
        XmRef fzero = xm_const_f64(ctx->xm_func, 0.0);
        return xm_fold_emit(ctx->xm_func, blk, XM_FNE, XR_REP_I64, arg, fzero);
    }

    return arg;
}

static XmRef lower_logical_not(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "logical not: expected 1 arg");
    XmRef truthy = lower_truthy_value(ctx, blk, v->args[0]);
    XmRef zero = xm_const_i64(ctx->xm_func, 0);
    return xm_fold_emit(ctx->xm_func, blk, XM_EQ, XR_REP_I64, truthy, zero);
}

static XmRef lower_select_value(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 3, "select: expected cond, true, false");
    XmRef cond = lower_truthy_value(ctx, blk, v->args[0]);
    XmRef true_val = get_ref(ctx, v->args[1]);
    XmRef false_val = get_ref(ctx, v->args[2]);
    xm_emit(ctx->xm_func, blk, XM_SELECT_COND, XR_REP_VOID, cond, XM_NONE);
    return xm_emit(ctx->xm_func, blk, XM_SELECT, v->rep, true_val, false_val);
}

/* ========== Comparison Lowering ========== */

static XmRef lower_comparison_with_ops(LowerCtx *ctx, XmBlock *blk, XiValue *v, uint16_t int_op,
                                       uint16_t float_op, bool reverse_operands, bool is_eq,
                                       bool is_ne) {
    XR_DCHECK(v->nargs == 2, "comparison: expected 2 args");
    XmRef lhs = get_ref(ctx, v->args[0]);
    XmRef rhs = get_ref(ctx, v->args[1]);

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

    if (is_float && float_op == XM_OP_COUNT) {
        ctx->error = true;
        return xm_const_i64(ctx->xm_func, 0);
    }

    uint16_t xm_op = is_float ? float_op : int_op;

    if (reverse_operands) {
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

static XiFunc *resolve_shared_slot_callee(XiFunc *caller, int64_t slot) {
    if (slot < 0)
        return NULL;
    for (XiFunc *f = caller; f; f = f->parent_func) {
        if (!f->shared_slot_funcs || slot >= (int64_t) f->shared_slot_func_count)
            continue;
        XiFunc *callee = f->shared_slot_funcs[slot];
        if (callee)
            return callee;
    }
    return NULL;
}

static XrProto *proto_tree_root(XrProto *proto) {
    while (proto && proto->enclosing)
        proto = proto->enclosing;
    return proto;
}

static XrProto *find_proto_for_xi_func(XrProto *proto, XiFunc *target) {
    if (!proto || !target)
        return NULL;
    if (proto->xi_func == target)
        return proto;
    uint32_t n = proto->protos.count;
    for (uint32_t i = 0; i < n; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        XrProto *found = find_proto_for_xi_func(sub, target);
        if (found)
            return found;
    }
    return NULL;
}

static XrProto *find_known_callee_proto(LowerCtx *ctx, XiValue *callee_val) {
    while (callee_val && callee_val->op == XI_COPY && callee_val->nargs >= 1)
        callee_val = callee_val->args[0];
    if (!ctx || !callee_val)
        return NULL;

    if (callee_val->op == XI_CLOSURE_NEW)
        return find_callee_proto(ctx, (XiFunc *) callee_val->aux);

    if (callee_val->op != XI_GET_SHARED)
        return NULL;

    XiFunc *callee = resolve_shared_slot_callee(ctx->xi_func, callee_val->aux_int);
    XrProto *root = proto_tree_root(ctx->proto);
    return find_proto_for_xi_func(root, callee);
}

static bool jit_builtin_name_is_known(const char *name) {
    static const char *known[] = {
        "array_new",     "Bytes",      "chr",       "copy",       "dump",
        "Exception",     "iter_new",   "iter_next", "iter_valid", "json_get_f",
        "json_init_f",   "json_set_f", "map_new",   "print",      "range",
        "regex_compile", "set_new",    "slice",     "str_concat", "StringBuilder",
        "typeof",        NULL,
    };
    for (int k = 0; known[k]; k++) {
        if (strcmp(name, known[k]) == 0)
            return true;
    }
    return false;
}

static XmRef lower_call_builtin(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    const char *bn = (const char *) v->aux;
    int bid = (int) v->aux_int;
    if (bn != NULL) {
        if (!jit_builtin_name_is_known(bn)) {
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

static bool lower_channel_method_call(LowerCtx *ctx, XmBlock *blk, XiValue *v,
                                      const XmRef *call_args, uint16_t total, uint16_t nargs,
                                      XmRef *out) {
    if (!ctx || !blk || !v || !out || v->op != XI_CALL_METHOD || v->nargs < 1)
        return false;
    if ((v->aux_int & 1) != 0)
        return false;
    if (!xi_value_is_static_channel(ctx, v->args[0]))
        return false;

    int method_sym = (int) (v->aux_int >> 1);
    XmHelperId helper = XM_HELPER__COUNT;
    if (method_sym == SYMBOL_SEND && nargs == 1) {
        int bc_pc = slot_map_bc_pc(ctx, v->id);
        uint16_t did = record_deopt(ctx, (uint32_t) bc_pc);
        if (did == 0xFFFF)
            return false;
        XmRef extra = xm_const_i64(ctx->xm_func, (int64_t) did);
        XmRef result =
            emit_channel_suspend_call(ctx, blk, XM_HELPER_chan_method_send,
                                      XM_HELPER_chan_method_send_block, extra, call_args, total);
        set_result_slot_metadata(ctx, v->id, result);
        *out = result;
        return true;
    }

    if (method_sym == SYMBOL_RECV && nargs == 0) {
        XmRef zero = xm_const_i64(ctx->xm_func, 0);
        XmRef raw =
            emit_channel_suspend_call(ctx, blk, XM_HELPER_chan_method_recv,
                                      XM_HELPER_chan_method_recv_block, zero, call_args, total);
        XmRef wrap_args[1] = {raw};
        XmRef result =
            emit_helper_call(ctx, blk, XM_HELPER_chan_method_recv_wrap, zero, wrap_args, 1);
        set_result_slot_metadata(ctx, v->id, result);
        *out = result;
        return true;
    }

    if (method_sym == SYMBOL_TRYSEND && nargs == 1) {
        helper = XM_HELPER_chan_method_try_send;
    } else if (method_sym == SYMBOL_TRYRECV && nargs == 0) {
        helper = XM_HELPER_chan_method_try_recv;
    } else if (method_sym == SYMBOL_CLOSE && nargs == 0) {
        helper = XM_HELPER_chan_method_close;
    } else if (method_sym == SYMBOL_IS_CLOSED && nargs == 0) {
        helper = XM_HELPER_chan_method_is_closed;
    }

    if (helper == XM_HELPER__COUNT)
        return false;

    XmRef zero = xm_const_i64(ctx->xm_func, 0);
    XmRef result = emit_helper_call(ctx, blk, helper, zero, call_args, total);
    if (helper == XM_HELPER_chan_method_is_closed)
        blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_BOOL, 0, 0};
    set_result_slot_metadata(ctx, v->id, result);
    *out = result;
    return true;
}

static XmRef lower_chan_new(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    uint64_t extra = ((uint64_t) ((uint8_t) v->aux_int)) << 32;
    XmRef args[1];
    uint16_t nargs = 0;

    if (v->nargs >= 1 && v->args[0]) {
        XiValue *cap = v->args[0];
        if (cap->op == XI_CONST && cap->type && cap->type->kind == XR_KIND_INT &&
            cap->aux_int >= 0 && cap->aux_int <= MAXARG_Bx) {
            extra |= (uint32_t) cap->aux_int;
        } else {
            extra |= XI2XM_CHAN_NEW_DYNAMIC_CAP_FLAG;
            args[0] = get_ref(ctx, cap);
            nargs = 1;
        }
    }

    XmRef extra_ref = xm_const_i64(ctx->xm_func, (int64_t) extra);
    XmRef result =
        emit_helper_call(ctx, blk, XM_HELPER_chan_new, extra_ref, nargs ? args : NULL, nargs);
    set_result_slot_metadata(ctx, v->id, result);
    return result;
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

    /* Self-recursive call: Xi encodes "invoke the current function" as
     * CALL <CONST null> args (AOT lowers the null callee to the function's
     * own closure `_cl`; see xi_cgen_value_helpers emit_call_hidden_closure).
     * Route it through XM_CALL_SELF_DIRECT, which BLs straight to our own
     * entry and leaves jit_ctx->call_closure untouched, so the recursive
     * invocation reuses our closure — correct for captured upvalues.  Routing
     * a self-call through CALL_KNOWN/CALL_DIRECT instead would overwrite
     * call_closure from call_args[0] (here the null callee), losing upvalues
     * and dispatching on a null closure (the historic recursion miscompile). */
    if (v->op == XI_CALL && v->nargs >= 1) {
        XiValue *callee_val = v->args[0];
        while (callee_val && callee_val->op == XI_COPY && callee_val->nargs >= 1)
            callee_val = callee_val->args[0];
        if (callee_val && callee_val->op == XI_CONST && callee_val->type &&
            callee_val->type->kind == XR_KIND_NULL) {
            uint8_t ret_rep = (ctx->proto && ctx->proto->return_type_info)
                                  ? xr_type_rep(ctx->proto->return_type_info)
                                  : XR_REP_TAGGED;
            /* Memory-passing self-call (args[0] == NONE): the params occupy
             * call_args[0..nargs-1] with no closure slot, because the codegen
             * passes x1 = &call_args[0] to the normal entry, which loads
             * param i from x1[i]. */
            XmRef self_args[16];
            for (uint16_t i = 0; i < nargs; i++)
                self_args[i] = call_args[i + 1];
            XmRef result =
                xm_emit(ctx->xm_func, blk, XM_CALL_SELF_DIRECT, ret_rep, XM_NONE, XM_NONE);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            if (xm_ref_is_vreg(result)) {
                uint32_t ri = XM_REF_INDEX(result);
                if (ri < ctx->xm_func->nvreg)
                    ctx->xm_func->vregs[ri].callee_proto = ctx->proto;
            }
            xm_func_bind_call_args(ctx->xm_func, result, self_args, nargs);
            return result;
        }
    }

    /* CALL_KNOWN optimization: if callee is a local closure or a top-level
     * function loaded from a shared slot, emit a direct call.  Codegen loads
     * proto->jit_entry for JIT-to-JIT fast path, falling back to
     * xr_jit_call_func if the callee has not been JIT-compiled yet. */
    if (v->op == XI_CALL && v->nargs >= 1) {
        XiValue *callee_val = v->args[0];
        XrProto *callee_proto = find_known_callee_proto(ctx, callee_val);
        if (callee_proto) {
            uint8_t ret_rep = callee_proto->return_type_info
                                  ? xr_type_rep(callee_proto->return_type_info)
                                  : XR_REP_TAGGED;
            XmRef proto_ref = xm_const_ptr(ctx->xm_func, (void *) callee_proto);
            XmRef nargs_ref = xm_const_i64(ctx->xm_func, (int64_t) nargs);
            XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_KNOWN, ret_rep, proto_ref, nargs_ref);
            blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
            xm_func_bind_call_args(ctx->xm_func, result, call_args, total);
            return result;
        }
    }

    if (v->op == XI_CALL_METHOD) {
        XmRef channel_result = XM_NONE;
        if (lower_channel_method_call(ctx, blk, v, call_args, total, nargs, &channel_result))
            return channel_result;
    }

    /* Method IC speculation: if call site is monomorphic, emit
     * GUARD_KLASS(receiver, expected_klass) + CALL_METHOD_KNOWN(proto, closure).
     * Method calls pass the receiver as parameter 0, so this uses the direct
     * parameter ABI instead of the closure-call ABI used by CALL_KNOWN. */
    if (v->op == XI_CALL_METHOD && v->nargs >= 1) {
        int bc_pc = slot_map_bc_pc(ctx, v->id);
        const XrICMethod *mic = ic_method_lookup(ctx, bc_pc);
        if (mic) {
            int ic_idx = ic_method_speculate_index(mic);
            XR_DCHECK(ic_idx >= 0 && ic_idx < mic->count, "method IC speculate index out of range");
            XrClass *klass = mic->entries[ic_idx].klass;
            XrMethod *method = mic->entries[ic_idx].method;
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

                /* Emit CALL_METHOD_KNOWN with the IC-resolved proto/closure. */
                uint8_t ret_rep = callee_proto->return_type_info
                                      ? xr_type_rep(callee_proto->return_type_info)
                                      : XR_REP_TAGGED;
                XmRef proto_ref = xm_const_ptr(ctx->xm_func, (void *) callee_proto);
                XmRef closure_ref = xm_const_ptr(ctx->xm_func, (void *) method->as.closure);
                XmRef result = xm_emit(ctx->xm_func, blk, XM_CALL_METHOD_KNOWN, ret_rep, proto_ref,
                                       closure_ref);
                blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
                xm_func_bind_call_args(ctx->xm_func, result, call_args, total);
                return result;
            }
        }
    }

generic_call:
    /* Method calls (XI_CALL_METHOD) go through xr_jit_invoke_method
     * which resolves the method on the receiver at runtime. */
    if (v->op == XI_CALL_METHOD) {
        /* aux_int = (global_symbol_id << 1) | is_super.
         * The SymbolId is resolved at lowering time (main thread) so the
         * JIT background thread does not need isolate access. */
        bool is_super = (v->aux_int & 1) != 0;
        int method_sym = (int) (v->aux_int >> 1);
        XR_DCHECK(method_sym > 0, "XI_CALL_METHOD: method_sym=0, lowering failed to "
                                  "resolve SymbolId");
        if (is_super) {
            ctx->error = true;
            return xm_const_i64(ctx->xm_func, 0);
        }
        int bc_pc = slot_map_bc_pc(ctx, v->id);

        /* Attach PIC for poly call sites so codegen can emit fast dispatch. */
        if (ctx->ic && ctx->ic->ic_methods && bc_pc >= 0) {
            XrICMethod *mic = xr_ic_method_table_get(ctx->ic->ic_methods, bc_pc);
            if (mic && mic->count >= 2 && !mic->is_megamorphic)
                attach_pic(ctx, mic);
        }

        uint16_t did = record_deopt(ctx, (uint32_t) bc_pc);
        int64_t encoded = ((int64_t) method_sym << 32) | ((int64_t) (did & 0xFFFF) << 16) |
                          ((int64_t) nargs & 0xFF);
        XmRef encoded_ref = xm_const_i64(ctx->xm_func, encoded);
        /* Method returns are polymorphic (int, bool, string, null, ptr).
         * Use I64 rep (raw payload in GP register) + TAGGED ctype so
         * the type pass does not narrow, allowing the dynamic tag patch
         * to read the correct runtime tag from vreg_runtime_tags[]. */
        XmRef result =
            emit_helper_call(ctx, blk, XM_HELPER_invoke_method, encoded_ref, call_args, total);
        /* Propagate bc_slot from Xi slot_map so deopt snapshots can
         * reconstruct the correct bytecode register. */
        if (xm_ref_is_vreg(result) && ctx->slot_idx && v->id < ctx->slot_idx_size) {
            int32_t si = ctx->slot_idx[v->id];
            if (si >= 0 && ctx->slot_map->entries[si].bc_slot != XI2XM_NO_BC_SLOT) {
                uint32_t vi = XM_REF_INDEX(result);
                if (vi < ctx->xm_func->nvreg)
                    ctx->xm_func->vregs[vi].bc_slot = (int16_t) ctx->slot_map->entries[si].bc_slot;
            }
        }
        return result;
    }

    if (v->op == XI_CALL_METHOD_DIRECT) {
        int method_idx = (int) v->aux_int;
        if (method_idx < 0 || method_idx > 0xFFFF || nargs > 0xFF) {
            ctx->error = true;
            return xm_const_i64(ctx->xm_func, 0);
        }
        int64_t encoded =
            ((int64_t) XR_TAG_PTR << 24) | ((int64_t) method_idx << 8) | ((int64_t) nargs & 0xFF);
        return emit_helper_call(ctx, blk, XM_HELPER_invoke_direct,
                                xm_const_i64(ctx->xm_func, encoded), call_args, total);
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
    blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_TAGGED, 0, 0};
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
    XmRef closure_ref = emit_helper_call(ctx, blk, XM_HELPER_closure_new, proto_ref, NULL, 0);

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
                XmRef cn_args[] = {val};
                XmRef cn_res = emit_helper_call(ctx, blk, XM_HELPER_cell_new,
                                                xm_const_i64(ctx->xm_func, 0), cn_args, 1);
                ctx->cell_ref[vid] = cn_res;
                ctx->cell_present[vid] = true;
            }
            /* Use cell pointer (instead of raw value) as the upvalue */
            val = ctx->cell_ref[vid];
        }

        XmRef idx = xm_const_i64(ctx->xm_func, i);
        XmRef call_args[] = {closure_ref, val};
        emit_helper_call(ctx, blk, XM_HELPER_closure_set_upval, idx, call_args, 2);
    }

    return closure_ref;
}

static XmRef lower_print(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    /* XI_PRINT: args[0..n]=values, aux_int=flags.
     * Lower as CALL_C(xr_jit_print, extra_arg) per argument.  Direct
     * XM_RT_PRINT emission silently swallowed every JIT-compiled print
     * because both x64 and arm64 codegen treat XM_RT_PRINT as a NOP
     * (the historical contract was that the builder rewrites it into
     * CALL_C, but no pass actually does so). */
    /* Xi flags layout differs from xr_jit_print's extra_arg encoding:
     *   Xi:           bit0=add_space, bit1=newline, bit4=skip_null
     *   xr_jit_print: bit0=newline,   bit1=add_space (skip_null unused)
     * Swap bit0 and bit1 so the helper sees the right semantics. */
    int64_t xi_flags = v->aux_int;
    int64_t jit_flags = ((xi_flags & 1) << 1) | ((xi_flags & 2) >> 1);

    for (uint16_t i = 0; i < v->nargs; i++) {
        XmRef arg = get_ref(ctx, v->args[i]);
        XmRef extra = xm_const_i64(ctx->xm_func, jit_flags);
        XmRef args[1] = {arg};
        emit_helper_call(ctx, blk, XM_HELPER_print, extra, args, 1);
    }
    return xm_const_i64(ctx->xm_func, 0);
}

static XmRef lower_get_shared(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    int so = ctx->proto ? ctx->proto->shared_offset : 0;
    int64_t abs_idx = v->aux_int + so;
    XmRef idx = xm_const_i64(ctx->xm_func, abs_idx);
    XmRef result = emit_helper_call(ctx, blk, XM_HELPER_get_shared, idx, NULL, 0);
    if (xm_ref_is_vreg(result) && ctx->slot_idx && v->id < ctx->slot_idx_size) {
        int32_t si = ctx->slot_idx[v->id];
        if (si >= 0 && ctx->slot_map->entries[si].bc_slot != XI2XM_NO_BC_SLOT) {
            uint32_t vi = XM_REF_INDEX(result);
            if (vi < ctx->xm_func->nvreg)
                ctx->xm_func->vregs[vi].bc_slot = (int16_t) ctx->slot_map->entries[si].bc_slot;
        }
    }
    return result;
}

static XmRef lower_set_shared(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "set_shared: expected 1 arg");
    XmRef val = get_ref(ctx, v->args[0]);
    int so = ctx->proto ? ctx->proto->shared_offset : 0;
    int64_t abs_idx = v->aux_int + so;
    XmRef idx = xm_const_i64(ctx->xm_func, abs_idx);
    XmRef call_args[] = {val};
    emit_helper_call(ctx, blk, XM_HELPER_set_shared, idx, call_args, 1);
    return val;
}

static XmRef lower_import_ref(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    int so = ctx->proto ? ctx->proto->shared_offset : 0;
    int64_t abs_idx = v->aux_int + so;
    XmRef idx = xm_const_i64(ctx->xm_func, abs_idx);
    return emit_helper_call(ctx, blk, XM_HELPER_get_shared, idx, NULL, 0);
}

static XmRef lower_get_builtin(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XmRef extra = xm_const_i64(ctx->xm_func, v->aux_int);
    XmRef result = emit_helper_call(ctx, blk, XM_HELPER_getbuiltin, extra, NULL, 0);
    set_result_slot_metadata(ctx, v->id, result);
    return result;
}

static bool lower_upval_needs_cell(const LowerCtx *ctx, const XiValue *v) {
    int upi = (int) v->aux_int;
    return ctx->xi_func && upi >= 0 && upi < (int) ctx->xi_func->ncaptures &&
           ctx->xi_func->captures[upi].needs_cell;
}

static XmRef lower_load_upval(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XmHelperId helper =
        lower_upval_needs_cell(ctx, v) ? XM_HELPER_upval_cell_get : XM_HELPER_upval_get;
    XmRef idx = xm_const_i64(ctx->xm_func, v->aux_int);
    return emit_helper_call(ctx, blk, helper, idx, NULL, 0);
}

static XmRef lower_store_upval(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "store_upval: expected 1 arg");
    XmHelperId helper =
        lower_upval_needs_cell(ctx, v) ? XM_HELPER_upval_cell_set : XM_HELPER_upval_set;
    XmRef val = get_ref(ctx, v->args[0]);
    XmRef idx = xm_const_i64(ctx->xm_func, v->aux_int);
    XmRef call_args[] = {val};
    emit_helper_call(ctx, blk, helper, idx, call_args, 1);
    return val;
}

static XmRef lower_throw(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs >= 1, "throw: need value arg");
    XmRef val = get_ref(ctx, v->args[0]);
    XmRef extra = xm_const_i64(ctx->xm_func, 0);
    XmRef args[1] = {val};
    emit_helper_call(ctx, blk, XM_HELPER_throw, extra, args, 1);
    return xm_const_i64(ctx->xm_func, 0);
}

static XmRef lower_ownership_helper(LowerCtx *ctx, XmBlock *blk, XiValue *v, XmHelperId helper,
                                    const char *op_name) {
    (void) op_name;
    XR_DCHECK_FMT(v->nargs >= 1, "%s: need value arg", op_name);
    XmRef val = get_ref(ctx, v->args[0]);
    XmRef extra = xm_const_i64(ctx->xm_func, 0);
    XmRef args[1] = {val};
    emit_helper_call(ctx, blk, helper, extra, args, 1);
    return xm_const_i64(ctx->xm_func, 0);
}

static XmRef xi2xm_const(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_const(ctx, blk, v);
}

static XmRef xi2xm_param(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    (void) blk;
    return get_ref(ctx, v);
}

static XmRef xi2xm_identity(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    (void) blk;
    if (v->nargs >= 1)
        return get_ref(ctx, v->args[0]);
    return xm_const_i64(ctx->xm_func, 0);
}

static XmRef xi2xm_copy(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_identity(ctx, blk, v);
}

static XmRef xi2xm_move(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_identity(ctx, blk, v);
}

static XmRef xi2xm_template_missing(LowerCtx *ctx, XiValue *v) {
    ctx->error = true;
    ctx->error_op = v ? v->op : 0;
    return xm_const_i64(ctx->xm_func, 0);
}

static XmRef xi2xm_template_binary(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XmOp int_op = xi_to_xm_template_int_op(v->op);
    if (int_op == XM_OP_COUNT)
        return xi2xm_template_missing(ctx, v);
    return lower_binary_arith_with_ops(ctx, blk, v, int_op, xi_to_xm_template_float_op(v->op));
}

static XmRef xi2xm_template_unary(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    if (xi_to_xm_template_is_logical_not(v->op))
        return lower_logical_not(ctx, blk, v);
    XmOp int_op = xi_to_xm_template_int_op(v->op);
    if (int_op == XM_OP_COUNT)
        return xi2xm_template_missing(ctx, v);
    return lower_unary_with_ops(ctx, blk, v, int_op, xi_to_xm_template_float_op(v->op));
}

static XmRef xi2xm_template_compare(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XmOp int_op = xi_to_xm_template_int_op(v->op);
    if (int_op == XM_OP_COUNT)
        return xi2xm_template_missing(ctx, v);
    return lower_comparison_with_ops(
        ctx, blk, v, int_op, xi_to_xm_template_float_op(v->op), xi_to_xm_template_swaps_args(v->op),
        xi_to_xm_template_eq_like(v->op), xi_to_xm_template_ne_like(v->op));
}

#define XI2XM_DEFINE_TEMPLATE_BINARY_DRIVER(ident, driver)                                         \
    static XmRef driver(LowerCtx *ctx, XmBlock *blk, XiValue *v) {                                 \
        return xi2xm_template_binary(ctx, blk, v);                                                 \
    }

XI_TO_XM_TEMPLATE_BINARY_DRIVERS(XI2XM_DEFINE_TEMPLATE_BINARY_DRIVER)

#undef XI2XM_DEFINE_TEMPLATE_BINARY_DRIVER

#define XI2XM_DEFINE_TEMPLATE_UNARY_DRIVER(ident, driver)                                          \
    static XmRef driver(LowerCtx *ctx, XmBlock *blk, XiValue *v) {                                 \
        return xi2xm_template_unary(ctx, blk, v);                                                  \
    }

XI_TO_XM_TEMPLATE_UNARY_DRIVERS(XI2XM_DEFINE_TEMPLATE_UNARY_DRIVER)

#undef XI2XM_DEFINE_TEMPLATE_UNARY_DRIVER

static XmRef xi2xm_select(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_select_value(ctx, blk, v);
}

static XmRef xi2xm_get_shared(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_get_shared(ctx, blk, v);
}

static XmRef xi2xm_set_shared(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_set_shared(ctx, blk, v);
}

static XmRef xi2xm_import_ref(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_import_ref(ctx, blk, v);
}

static XmRef xi2xm_closure_new(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_closure_new(ctx, blk, v);
}

static XmRef xi2xm_load_upval(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_load_upval(ctx, blk, v);
}

static XmRef xi2xm_store_upval(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_store_upval(ctx, blk, v);
}

static XmRef xi2xm_throw(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_throw(ctx, blk, v);
}

static XmRef xi2xm_try(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    (void) blk;
    XiBlock *catch_xi = (XiBlock *) v->aux;
    XR_DCHECK(catch_xi != NULL, "XI_TRY: missing catch block");
    XmBlock *catch_xm = get_block(ctx, catch_xi);
    if (ctx->try_depth >= ctx->try_cap) {
        int new_cap = ctx->try_cap ? ctx->try_cap * 2 : XI2XM_TRY_STACK_INIT_CAP;
        XmBlock **grown =
            (XmBlock **) xr_realloc(ctx->try_stack, (size_t) new_cap * sizeof(XmBlock *));
        if (!grown) {
            ctx->error = true;
            ctx->error_op = v->op;
            return xm_const_i64(ctx->xm_func, 0);
        }
        ctx->try_stack = grown;
        ctx->try_cap = new_cap;
    }
    ctx->try_stack[ctx->try_depth++] = catch_xm;
    return xm_const_i64(ctx->xm_func, 0);
}

static XmRef xi2xm_catch(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    (void) v;
    XmRef exc = xm_emit_unary(ctx->xm_func, blk, XM_CATCH, XR_REP_I64, XM_NONE);
    blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
    return exc;
}

static XmRef xi2xm_end_try(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    (void) v;
    if (ctx->try_depth > 0)
        ctx->try_depth--;
    xm_emit(ctx->xm_func, blk, XM_TRY_END, XR_REP_I64, XM_NONE, XM_NONE);
    blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
    return xm_const_i64(ctx->xm_func, 0);
}

/* The native XM_RETAIN/RELEASE inline treats the operand register as a raw
 * GC pointer. That is only valid when the value is a pointer-or-null at
 * runtime (the inline null-checks the payload). xr_type_rep == XR_REP_PTR
 * captures exactly that set (heap kinds, nullable heap, and all-pointer
 * unions); everything else (scalars, mixed unions, unknown) keeps the tagged
 * C-helper path, which does the runtime XR_IS_PTR check. */
static bool rc_operand_is_heap_ptr(const XiValue *v) {
    return v && v->nargs >= 1 && v->args[0] && xr_type_rep(v->args[0]->type) == XR_REP_PTR;
}

static XmRef xi2xm_retain(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    if (rc_operand_is_heap_ptr(v)) {
        XmRef val = get_ref(ctx, v->args[0]);
        xm_emit_unary(ctx->xm_func, blk, XM_RETAIN, XR_REP_VOID, val);
        blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
        return xm_const_i64(ctx->xm_func, 0);
    }
    return lower_ownership_helper(ctx, blk, v, XM_HELPER_rc_dup, "retain");
}

static XmRef xi2xm_release(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    if (rc_operand_is_heap_ptr(v)) {
        XmRef val = get_ref(ctx, v->args[0]);
        xm_emit_unary(ctx->xm_func, blk, XM_RELEASE, XR_REP_VOID, val);
        blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
        return xm_const_i64(ctx->xm_func, 0);
    }
    return lower_ownership_helper(ctx, blk, v, XM_HELPER_rc_drop, "release");
}

#define XI2XM_DEFINE_TEMPLATE_COMPARE_DRIVER(ident, driver)                                        \
    static XmRef driver(LowerCtx *ctx, XmBlock *blk, XiValue *v) {                                 \
        return xi2xm_template_compare(ctx, blk, v);                                                \
    }

XI_TO_XM_TEMPLATE_COMPARE_DRIVERS(XI2XM_DEFINE_TEMPLATE_COMPARE_DRIVER)

#undef XI2XM_DEFINE_TEMPLATE_COMPARE_DRIVER

static XmRef xi2xm_zero_extend(LowerCtx *ctx, XmBlock *blk, XiValue *v, int64_t mask) {
    XmRef a = get_ref(ctx, v->args[0]);
    XmRef m = xm_const_i64(ctx->xm_func, mask);
    return xm_emit(ctx->xm_func, blk, XM_AND, XR_REP_I64, a, m);
}

static XmRef xi2xm_sign_extend_shift(LowerCtx *ctx, XmBlock *blk, XiValue *v, int64_t shift) {
    XmRef a = get_ref(ctx, v->args[0]);
    XmRef sh = xm_const_i64(ctx->xm_func, shift);
    XmRef t = xm_emit(ctx->xm_func, blk, XM_SHL, XR_REP_I64, a, sh);
    return xm_emit(ctx->xm_func, blk, XM_SHR, XR_REP_I64, t, sh);
}

static XmRef xi2xm_narrow_i8(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_sign_extend_shift(ctx, blk, v, 56);
}

static XmRef xi2xm_narrow_u8(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_zero_extend(ctx, blk, v, 0xFF);
}

static XmRef xi2xm_narrow_i16(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_sign_extend_shift(ctx, blk, v, 48);
}

static XmRef xi2xm_narrow_u16(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_zero_extend(ctx, blk, v, 0xFFFF);
}

static XmRef xi2xm_narrow_i32(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_sign_extend_shift(ctx, blk, v, 32);
}

static XmRef xi2xm_narrow_u32(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_zero_extend(ctx, blk, v, 0xFFFFFFFF);
}

static XmRef xi2xm_narrow_f32(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    (void) blk;
    return get_ref(ctx, v->args[0]);
}

static XmRef xi2xm_widen_i8(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_sign_extend_shift(ctx, blk, v, 56);
}

static XmRef xi2xm_widen_u8(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_zero_extend(ctx, blk, v, 0xFF);
}

static XmRef xi2xm_widen_i16(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_sign_extend_shift(ctx, blk, v, 48);
}

static XmRef xi2xm_widen_u16(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_zero_extend(ctx, blk, v, 0xFFFF);
}

static XmRef xi2xm_widen_i32(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_sign_extend_shift(ctx, blk, v, 32);
}

static XmRef xi2xm_widen_u32(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_zero_extend(ctx, blk, v, 0xFFFFFFFF);
}

static XmRef xi2xm_widen_f32(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    (void) blk;
    return get_ref(ctx, v->args[0]);
}

static XmRef xi2xm_isnull(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "isnull: expected 1 arg");
    XmRef arg = get_ref(ctx, v->args[0]);
    return xm_emit_unary(ctx->xm_func, blk, XM_RT_ISNULL, XR_REP_I64, arg);
}

static XmRef xi2xm_convert(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_convert(ctx, blk, v);
}

static XmRef xi2xm_box(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_box(ctx, blk, v);
}

static XmRef xi2xm_unbox(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_unbox(ctx, blk, v);
}

static XmRef xi2xm_load_field(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs >= 1, "load_field: need obj arg");
    XmRef obj = get_ref(ctx, v->args[0]);

    if (v->aux) {
        ctx->error = true;
        return xm_const_i64(ctx->xm_func, 0);
    }
    XmRef off = xm_const_i64(ctx->xm_func, v->aux_int);
    return xm_emit(ctx->xm_func, blk, XM_LOAD_FIELD, XR_REP_I64, obj, off);
}

static XmRef xi2xm_store_field(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs >= 2, "store_field: need obj + val");
    XmRef obj = get_ref(ctx, v->args[0]);
    XmRef val = get_ref(ctx, v->args[1]);
    if (v->aux) {
        ctx->error = true;
        return val;
    }
    XmRef off = xm_const_i64(ctx->xm_func, v->aux_int);
    xm_emit(ctx->xm_func, blk, XM_STORE_FIELD, XR_REP_VOID, obj, val);
    XmIns *sf = &blk->ins[blk->nins - 1];
    sf->rep = XM_SF_TAG_RUNTIME;
    sf->dst = off;
    sf->flags |= XM_FLAG_SIDE_EFFECT;
    return val;
}

static XmRef xi2xm_index_get(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs >= 2, "index_get: need obj + key");
    XmRef obj = get_ref(ctx, v->args[0]);
    XmRef key = get_ref(ctx, v->args[1]);
    XmRef extra = xm_const_i64(ctx->xm_func, 0);
    XmRef args[2] = {obj, key};
    return emit_helper_call(ctx, blk, XM_HELPER_index_get, extra, args, 2);
}

static XmRef xi2xm_index_set(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs >= 3, "index_set: need obj + key + val");
    XmRef obj = get_ref(ctx, v->args[0]);
    XmRef key = get_ref(ctx, v->args[1]);
    XmRef val = get_ref(ctx, v->args[2]);
    XmRef extra = xm_const_i64(ctx->xm_func, 0);
    XmRef args[3] = {obj, key, val};
    emit_helper_call(ctx, blk, XM_HELPER_index_set, extra, args, 3);
    return val;
}

static XmRef xi2xm_deopt_to_vm(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    int bc_pc = slot_map_bc_pc(ctx, v->id);
    uint16_t did = record_deopt(ctx, (uint32_t) bc_pc);
    if (did == 0xFFFF) {
        ctx->error = true;
        return xm_const_i64(ctx->xm_func, 0);
    }
    XmRef deopt_id = xm_const_i64(ctx->xm_func, (int64_t) did);
    xm_emit(ctx->xm_func, blk, XM_DEOPT, XR_REP_I64, deopt_id, XM_NONE);
    blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
    return xm_const_i64(ctx->xm_func, 0);
}

static XmRef xi2xm_assert(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_deopt_to_vm(ctx, blk, v);
}

static XmRef xi2xm_assert_eq(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_deopt_to_vm(ctx, blk, v);
}

static XmRef xi2xm_assert_ne(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_deopt_to_vm(ctx, blk, v);
}

static XmRef xi2xm_typeof(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_deopt_to_vm(ctx, blk, v);
}

static XmRef xi2xm_get_builtin(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_get_builtin(ctx, blk, v);
}

static XmRef xi2xm_print(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_print(ctx, blk, v);
}

static XmRef xi2xm_call_builtin(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_call_builtin(ctx, blk, v);
}

static XmRef xi2xm_call(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_call(ctx, blk, v);
}

static XmRef xi2xm_chan_new(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return lower_chan_new(ctx, blk, v);
}

/* Direct `ch.send(x)` op (XI_CHAN_SEND: args[0]=channel, args[1]=value).
 * Fused channel ops use the bytecode-shaped helper pair, avoiding the
 * source-level method/ADT bridge used by XI_CALL_METHOD. */
static XmRef xi2xm_chan_send(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    if (v->nargs < 2 || !v->args[0] || !v->args[1]) {
        ctx->error = true;
        ctx->error_op = v->op;
        return xm_const_i64(ctx->xm_func, 0);
    }
    XmRef call_args[2];
    call_args[0] = get_ref(ctx, v->args[0]); /* channel (receiver) */
    call_args[1] = get_ref(ctx, v->args[1]); /* send value */
    XmRef extra = xm_const_i64(ctx->xm_func, 0);
    XmRef result = emit_channel_suspend_call(ctx, blk, XM_HELPER_chan_send,
                                             XM_HELPER_chan_send_block, extra, call_args, 2);
    set_result_slot_metadata(ctx, v->id, result);
    return result;
}

/* Fused `match (ch.recv())` payload op (XI_CHAN_RECV: args[0]=channel).
 * Returns the raw payload; XI_CHAN_RECV_STATUS projects the paired ok bit. */
static XmRef xi2xm_chan_recv(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    if (v->nargs < 1 || !v->args[0]) {
        ctx->error = true;
        ctx->error_op = v->op;
        return xm_const_i64(ctx->xm_func, 0);
    }
    XmRef call_args[1];
    call_args[0] = get_ref(ctx, v->args[0]); /* channel */
    XmRef extra = xm_const_i64(ctx->xm_func, 0);
    XmRef result = emit_channel_suspend_call(ctx, blk, XM_HELPER_chan_recv,
                                             XM_HELPER_chan_recv_block, extra, call_args, 1);
    set_result_slot_metadata(ctx, v->id, result);
    return result;
}

/* Readiness bit of a fused recv (XI_CHAN_RECV_STATUS: args[0]=the paired
 * XI_CHAN_RECV value). The paired direct recv helper parks the raw 0/1 status
 * in scratch call_args[1]; the recv value is kept as an ordering dependency. */
static XmRef xi2xm_chan_recv_status(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    if (v->nargs < 1 || !v->args[0]) {
        ctx->error = true;
        ctx->error_op = v->op;
        return xm_const_i64(ctx->xm_func, 0);
    }
    (void) get_ref(ctx, v->args[0]); /* paired recv value (ordering only) */
    XmRef status_off = xm_const_i64(ctx->xm_func, XM_JIT_CALL_ARGS_OFFSET + 8);
    XmRef result = xm_emit_unary(ctx->xm_func, blk, XM_LOAD_CORO, XR_REP_I64, status_off);
    blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_BOOL, 0, 0};
    set_result_slot_metadata(ctx, v->id, result);
    return result;
}

static XmRef xi2xm_err_check(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XmRef zero = xm_const_i64(ctx->xm_func, 0);
    XmRef tag = emit_helper_call(ctx, blk, XM_HELPER_pending_error_tag, zero, NULL, 0);

    if (v->type && v->type->kind == XR_KIND_BOOL) {
        XmRef has_error = xm_fold_emit(ctx->xm_func, blk, XM_NE, XR_REP_I64, tag, zero);
        blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_BOOL, 0, 0};
        return has_error;
    }

    int bc_pc = slot_map_bc_pc(ctx, v->id);
    uint16_t did = record_deopt(ctx, (uint32_t) bc_pc);
    if (did == 0xFFFF) {
        ctx->error = true;
        ctx->error_op = v->op;
        return xm_const_i64(ctx->xm_func, 0);
    }

    XmRef no_error = xm_fold_emit(ctx->xm_func, blk, XM_EQ, XR_REP_I64, tag, zero);
    blk->ins[blk->nins - 1].ctype = (XmType) {XM_TK_BOOL, 0, 0};
    XmRef deopt_id = xm_const_i64(ctx->xm_func, (int64_t) did);
    xm_emit(ctx->xm_func, blk, XM_GUARD_NONNULL, XR_REP_VOID, no_error, XM_NONE);
    blk->ins[blk->nins - 1].dst = deopt_id;
    blk->ins[blk->nins - 1].flags |= XM_FLAG_SIDE_EFFECT;
    return xm_const_i64(ctx->xm_func, 0);
}

static XmRef xi2xm_class_create(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_deopt_to_vm(ctx, blk, v);
}

static XmRef xi2xm_is(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    return xi2xm_deopt_to_vm(ctx, blk, v);
}

static XmRef xi2xm_array_new(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XmRef cap = (v->nargs >= 1) ? get_ref(ctx, v->args[0]) : xm_const_i64(ctx->xm_func, 0);
    return xm_emit_unary(ctx->xm_func, blk, XM_RT_ARRAY_NEW, XR_REP_I64, cap);
}

static XmRef xi2xm_map_new(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XmRef cap = (v->nargs >= 1) ? get_ref(ctx, v->args[0]) : xm_const_i64(ctx->xm_func, 0);
    return xm_emit_unary(ctx->xm_func, blk, XM_RT_MAP_NEW, XR_REP_I64, cap);
}

static XmRef xi2xm_set_new(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XmRef cap = (v->nargs >= 1) ? get_ref(ctx, v->args[0]) : xm_const_i64(ctx->xm_func, 0);
    return xm_emit_unary(ctx->xm_func, blk, XM_RT_MAP_NEW, XR_REP_I64, cap);
}

static XmRef xi2xm_tuple_new(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    if (v->nargs > XI2XM_MAX_HELPER_CALL_ARGS) {
        /* Exceeds the call_args[16] ABI array; reject this function for JIT
         * (caller runs it interpreted) and name the op so the bail is
         * diagnosable rather than silent. */
        ctx->error = true;
        ctx->error_op = v->op;
        return xm_const_i64(ctx->xm_func, 0);
    }

    XmRef args[XI2XM_MAX_HELPER_CALL_ARGS];
    for (uint16_t i = 0; i < v->nargs; i++)
        args[i] = get_ref(ctx, v->args[i]);

    XmRef arity = xm_const_i64(ctx->xm_func, (int64_t) v->nargs);
    return emit_helper_call(ctx, blk, XM_HELPER_tuple_new, arity, args, v->nargs);
}

static XmRef xi2xm_tuple_get(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "tuple_get: expected tuple arg");
    if (v->aux_int < 0 || v->aux_int > UINT16_MAX) {
        ctx->error = true;
        return xm_const_i64(ctx->xm_func, 0);
    }

    XmRef tuple = get_ref(ctx, v->args[0]);
    XmRef index = xm_const_i64(ctx->xm_func, v->aux_int);
    XmRef args[1] = {tuple};
    return emit_helper_call(ctx, blk, XM_HELPER_tuple_get, index, args, 1);
}

static XmRef xi2xm_str_concat(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    if (v->nargs == 0)
        return xm_const_i64(ctx->xm_func, 0);
    XmRef acc = get_ref(ctx, v->args[0]);
    for (uint16_t i = 1; i < v->nargs; i++) {
        XmRef part = get_ref(ctx, v->args[i]);
        acc = xm_emit(ctx->xm_func, blk, XM_RT_ADD, XR_REP_I64, acc, part);
    }
    return acc;
}

static XmRef xi2xm_reject_unsupported(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    (void) blk;
    ctx->error = true;
    ctx->error_op = v ? v->op : XI_OP_COUNT;
    return xm_const_i64(ctx->xm_func, 0);
}

static bool xi_to_xm_lower_generated(LowerCtx *ctx, XmBlock *blk, XiValue *v, XmRef *out) {
    XR_DCHECK(out != NULL, "xi_to_xm_lower_generated: NULL out");
    switch (v->op) {
#define XI2XM_GENERATED_CASE(op, name, driver)                                                     \
    case XI_##op:                                                                                  \
        (void) name;                                                                               \
        *out = driver(ctx, blk, v);                                                                \
        return true;
        XI_TO_XM_LOWERING_DRIVERS(XI2XM_GENERATED_CASE)
#undef XI2XM_GENERATED_CASE
        case XI_OP_COUNT:
            break;
    }
    return false;
}

/* ========== Value Lowering Dispatch ========== */

static XmRef lower_value(LowerCtx *ctx, XmBlock *blk, XiValue *v) {
    XmRef generated = XM_NONE;
    if (xi_to_xm_lower_generated(ctx, blk, v, &generated))
        return generated;

    ctx->error = true;
    ctx->error_op = v ? v->op : XI_OP_COUNT;
    return xm_const_i64(ctx->xm_func, 0);
}

/* ========== Block Lowering ========== */

/* Lower all phi nodes in a block */
static void lower_phis(LowerCtx *ctx, XiBlock *xi_blk, XmBlock *xm_blk) {
    for (XiPhi *phi = xi_blk->phis; phi; phi = phi->next) {
        XiValue *pv = &phi->value;
        uint8_t rep = xi_value_rep_or_tagged(pv);
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

/* Lower a single block's instructions */
static void lower_block_values(LowerCtx *ctx, XiBlock *xi_blk, XmBlock *xm_blk) {
    for (uint32_t i = 0; i < xi_blk->nvalues; i++) {
        XiValue *v = xi_blk->values[i];
        if (!v)
            continue;
        XmRef ref = lower_value(ctx, xm_blk, v);
        if (ctx->error && ctx->error_op == XI_OP_COUNT)
            ctx->error_op = v->op;
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
           op == XM_GUARD_CLASS || op == XM_GUARD_KLASS || op == XM_TAG_CHECK || op == XM_DEOPT;
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
    ctx.error_op = XI_OP_COUNT;

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
    for (uint32_t i = 0; i < ctx.ref_map_size; i++)
        ctx.ref_map[i] = XM_NONE;

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
        uint8_t rep = param ? xi_value_rep_or_tagged(param) : XR_REP_I64;
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

        /* Propagate Xi block frequency to Xm for codegen fall-through decisions. */
        if (xi_blk->frequency > 0) {
            uint32_t freq = xi_blk->frequency;
            xm_blk->branch_taken_pct = (freq > 100) ? 100 : (uint8_t) freq;
        }

        /* Snapshot handler before lowering (XI_TRY may change depth).
         * If this block is itself the handler of the topmost try (catch
         * or finally), look past it: a throw inside the handler must
         * propagate to the outer try, never re-enter the same handler
         * (otherwise CALL_C in the catch body would CBNZ back to the
         * top of the catch and spin forever). */
        XmBlock *handler_before = NULL;
        for (int td = ctx.try_depth - 1; td >= 0; td--) {
            XmBlock *cand = ctx.try_stack[td];
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
    xr_free(ctx.try_stack);

    if (ctx.error) {
        if (ctx.error_op != XI_OP_COUNT) {
            fprintf(stderr, "[xi_to_xm] lowering failed at %s\n", xi_op_name(ctx.error_op));
        }
        xm_func_destroy(func);
        return NULL;
    }

    /* Post-pass: trim deopt snapshots to only live values.
     * Compute Xm-level liveness, then for each guard's snapshot
     * remove slots whose value vreg is dead at the guard point. */
    refine_deopt_liveness(func);

    return func;
}
