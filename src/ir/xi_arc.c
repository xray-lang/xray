/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_arc.c - Precise dup/drop (retain/release) insertion pass
 *
 * Consumes the ownership annotations produced by xi_own (backward
 * ownership inference) and inserts XI_RETAIN (dup) / XI_RELEASE (drop)
 * ops at precise points, following the Perceus owned/borrow model:
 *
 *   - Each RC value has a single owning reference (its definition).
 *   - A consuming use (store/return/throw/call-arg/aggregate element)
 *     takes ownership. The LAST consuming use moves the value out for
 *     free; every EARLIER consuming use needs a `dup` before it so the
 *     owner still has a reference to give away later.
 *   - A value that is never consumed (only borrowed, or dead) is dropped
 *     at its death point, found by backward liveness: after the last use
 *     in the block where it dies, or on the CFG edge where it goes dead
 *     (see insert_drops_at_death). Loop-body temporaries therefore die
 *     once per iteration, not at function exit.
 *
 * This pass MUST run BEFORE backend lowering, while ops are still
 * semantic (STORE_FIELD/ARRAY_NEW/...), because the owned/borrow split
 * is keyed on those ops. Stack/region values (XI_STACK_ALLOC, REGION)
 * carry frame lifetime and get no dup/drop.
 *
 * References: Koka Backend/C/Parc.hs, Roc mono/src/inc_dec.rs.
 */

#include "xi_arc.h"
#include "xi_own.h"
#include "xi_escape.h"
#include "xi_effect.h"
#include "xi_analysis.h"
#include "xi_cfg_edit.h"
#include "xi_core_api.h"
#include "xi_loop.h"
#include "xi_module.h"
#include "xi_coro_analyze.h"
#include "xi_value_query.h"
#include "xi_receiver_alias.h"
#include "xi_builtin_map_entry_iterator_shape.h"
#include "../frontend/analyzer/xa_intrinsic_registry.h"
#include "../runtime/value/xtype.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

#include <string.h>

/* ========== Stack Alloc Rewrite ========== */

static bool stack_alloc_has_const_capacity(const XiValue *v) {
    return v && v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CONST &&
           v->args[0]->aux_int >= 0;
}

static bool stack_alloc_closure_use_is_synchronous_callback(const XiValue *user, uint16_t arg_idx,
                                                            const XiValue *target) {
    if (!user || !target)
        return false;
    if (user->op == XI_CALL)
        return arg_idx == 0;
    if (user->op == XI_PAR_FOR && arg_idx == 3 && user->aux_kind == XI_AUX_KIND_PAR_FOR) {
        const XiParallelForData *data = (const XiParallelForData *) user->aux;
        return data && data->body_func == (const XiFunc *) target->aux;
    }
    if (user->op == XI_PAR_MAP && arg_idx == 3 && user->aux_kind == XI_AUX_KIND_PAR_MAP) {
        const XiParallelMapData *data = (const XiParallelMapData *) user->aux;
        return data && data->body_func == (const XiFunc *) target->aux;
    }
    if (user->op == XI_PAR_REDUCE && arg_idx == 4 && user->aux_kind == XI_AUX_KIND_PAR_REDUCE) {
        const XiParallelReduceData *data = (const XiParallelReduceData *) user->aux;
        return data && data->body_func == (const XiFunc *) target->aux;
    }
    if (user->op == XI_PAR_REDUCE && arg_idx == 5 && user->aux_kind == XI_AUX_KIND_PAR_REDUCE) {
        const XiParallelReduceData *data = (const XiParallelReduceData *) user->aux;
        return data && data->combine_func == (const XiFunc *) target->aux;
    }
    return false;
}

static bool stack_alloc_closure_use_is_scoped_par_for_callback(const XiValue *user,
                                                               uint16_t arg_idx,
                                                               const XiValue *target) {
    if (!user || !target)
        return false;
    if (user->op == XI_PAR_FOR && arg_idx == 3 && user->aux_kind == XI_AUX_KIND_PAR_FOR) {
        const XiParallelForData *data = (const XiParallelForData *) user->aux;
        return data && data->body_func == (const XiFunc *) target->aux;
    }
    if (user->op == XI_PAR_MAP && arg_idx == 3 && user->aux_kind == XI_AUX_KIND_PAR_MAP) {
        const XiParallelMapData *data = (const XiParallelMapData *) user->aux;
        return data && data->body_func == (const XiFunc *) target->aux;
    }
    if (user->op == XI_PAR_REDUCE && arg_idx == 4 && user->aux_kind == XI_AUX_KIND_PAR_REDUCE) {
        const XiParallelReduceData *data = (const XiParallelReduceData *) user->aux;
        return data && data->body_func == (const XiFunc *) target->aux;
    }
    if (user->op == XI_PAR_REDUCE && arg_idx == 5 && user->aux_kind == XI_AUX_KIND_PAR_REDUCE) {
        const XiParallelReduceData *data = (const XiParallelReduceData *) user->aux;
        return data && data->combine_func == (const XiFunc *) target->aux;
    }
    return false;
}

static bool stack_alloc_closure_value_uses_are_synchronous_callbacks(const XiFunc *f,
                                                                     const XiValue *value,
                                                                     const XiValue *closure,
                                                                     uint8_t depth) {
    if (!f || !value || !closure || depth > 8)
        return false;
    bool saw_use = false;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *user = blk->values[i];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != value)
                    continue;
                bool synchronous =
                    stack_alloc_closure_use_is_synchronous_callback(user, a, closure);
                if (!synchronous && a == 0 &&
                    (user->op == XI_COPY || user->op == XI_SOURCE_MOVE ||
                     user->op == XI_OWNER_FORWARD || user->op == XI_BOX || user->op == XI_UNBOX))
                    synchronous = stack_alloc_closure_value_uses_are_synchronous_callbacks(
                        f, user, closure, (uint8_t) (depth + 1));
                if (!synchronous)
                    return false;
                saw_use = true;
            }
        }
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == value)
                    return false;
            }
        }
        if (blk->control == value)
            return false;
    }
    return saw_use;
}

static bool stack_alloc_closure_uses_are_synchronous_callbacks(const XiFunc *f,
                                                               const XiValue *target) {
    return stack_alloc_closure_value_uses_are_synchronous_callbacks(f, target, target, 0);
}

static bool stack_alloc_closure_uses_are_scoped_par_for_callbacks(const XiFunc *f,
                                                                  const XiValue *target) {
    if (!f || !target || !target->aux)
        return false;
    if (target->op != XI_CLOSURE_NEW &&
        !(target->op == XI_STACK_ALLOC && target->aux_int == XI_CLOSURE_NEW))
        return false;
    bool saw_use = false;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *user = blk->values[i];
            if (!user || user == target)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != target)
                    continue;
                if (!stack_alloc_closure_use_is_scoped_par_for_callback(user, a, target))
                    return false;
                saw_use = true;
            }
        }
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == target)
                    return false;
            }
        }
        if (blk->control == target)
            return false;
    }
    return saw_use;
}

static bool stack_alloc_can_preserve_semantics(const XiFunc *f, const XiValue *v) {
    /* A resumable function does not retain its native C stack between
     * suspension points.  Until stack-allocation liveness is partitioned by
     * resume region, ordinary local allocations must stay on the heap even
     * when escape analysis proves they do not leave the function.  Scoped
     * parallel callbacks are the sole exception: CGen materializes their
     * environment inside the synchronous parallel boundary instead of with
     * alloca in the resumable frame. */
    if (f && (f->effect_summary & XI_FLAG_MAY_SUSPEND) != 0) {
        return v && v->op == XI_CLOSURE_NEW &&
               stack_alloc_closure_uses_are_scoped_par_for_callbacks(f, v);
    }
    if (!v || !stack_alloc_has_const_capacity(v))
        return v && v->op == XI_CLOSURE_NEW && v->aux &&
               stack_alloc_closure_uses_are_synchronous_callbacks(f, v);
    switch (v->op) {
        case XI_ARRAY_NEW:
            /* Array literals lower to XI_ARRAY_NEW(cap = element count) followed
             * by XI_INDEX_SET fills, which require the array to start at
             * length == count (the heap path's xrt_array_new_typed presets it).
             * The stack rewrite emits xrt_array_stack_new, which starts at
             * length 0 with a generic element type, so every literal index-set
             * would trap and typed reads would use the wrong stride. Keep arrays
             * on the heap until a length-preserving typed stack allocation
             * exists. */
            return false;
        case XI_MAP_NEW:
        case XI_SET_NEW:
            return v->aux_int == 0;
        case XI_CLOSURE_NEW:
            return v->aux && stack_alloc_closure_uses_are_synchronous_callbacks(f, v);
        default:
            return false;
    }
}

static void rewrite_to_stack(XiValue *v) {
    XR_DCHECK(v != NULL, "rewrite_to_stack: NULL value");
    XR_DCHECK(v->escape == XI_ESC_NONE, "rewrite_to_stack: not NO_ESCAPE");
    v->aux_int = (int32_t) v->op; /* save original op for codegen */
    v->op = XI_STACK_ALLOC;
    v->flags |= XI_FLAG_SIDE_EFFECT;
}

XR_FUNC void xi_stack_alloc_rewrite(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_stack_alloc_rewrite: NULL func");

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_stack_alloc_rewrite(f->children[i]);
    }

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (v->escape != XI_ESC_NONE)
                continue;
            if (!xi_own_value_is_rc(v))
                continue; /* scalars: no alloc */
            if (!xi_op_is_heap_alloc(v->op))
                continue;
            if (!stack_alloc_can_preserve_semantics(f, v)) {
                v->escape = (uint8_t) XI_ESC_ARG;
                continue;
            }
            rewrite_to_stack(v);
        }
    }
}

/* ========== Escaping copy → move (Perceus move generation) ==========
 *
 * XI_COPY is an identity/narrowing alias whose result BORROWS the source
 * (result-ownership = borrowed): it must not get a death-drop of its own,
 * because that would free the shared object out from under the source. That
 * model is correct while the copy result is only borrowed (read). But when a
 * borrow-copy's result is CONSUMED by an owning use — returned, stored into a
 * heap object, sent across a channel, passed as an owned argument — the
 * object's single owning reference ESCAPES through the copy. Leaving it a
 * borrow then makes ARC both release the source (freeing the unique object)
 * and dup the escaping copy (touching freed memory): a use-after-free for the
 * textbook `var b = a; return b`.
 *
 * The fix is to make such a copy an explicit MOVE. XI_OWNER_FORWARD consumes
 * its source and produces a new tracked owner: the existing owned/borrow
 * machinery transfers the source's reference through the move (dup'ing it
 * first only when the source is still live afterwards), then owns every later
 * use of the forwarded SSA value. Tracking the result is essential when a
 * local forwarding edge feeds several branch or loop consumes. This is the
 * Perceus copy→move decision, localized to copies whose result is actually
 * consumed. Copies whose result is only borrowed stay copies. Run before
 * borrow-signature precompute so signatures see the move. */

static bool arc_function_returns_borrow(const XiFunc *f) {
    return f && f->arc_return_ownership.complete &&
           (f->arc_return_ownership.kind == XI_RETURN_OWNERSHIP_BORROWED_PARAM ||
            f->arc_return_ownership.kind == XI_RETURN_OWNERSHIP_BORROWED_STATIC);
}

/* Does `v` have at least one consuming use anywhere in `f` (an owned-store /
 * return / forward / phi / consuming call argument)? */
static bool value_has_consuming_use(const XiFunc *f, const XiValue *v) {
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *u = blk->values[i];
            if (!u || u->op == XI_RETAIN || u->op == XI_RELEASE)
                continue;
            for (uint16_t a = 0; a < u->nargs; a++) {
                if (u->args[a] == v && xi_own_value_arg_is_consuming(u, a))
                    return true;
            }
        }
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == v)
                    return true; /* a phi merge consumes its incoming value */
            }
        }
        if (blk->control == v && blk->kind == XI_BLOCK_RETURN && !arc_function_returns_borrow(f))
            return true;
    }
    return false;
}

/* Convert escaping borrow-copies to moves in `f` (and its nested children).
 * Iterated to a fixpoint so a chain of copies (`c = b; b = a; ...`) all
 * collapse: converting one copy to a move makes its source a consuming use,
 * which can in turn promote an earlier copy. */
static void arc_copy_to_move(XiFunc *f) {
    if (!f)
        return;
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t b = 0; b < f->nblocks; b++) {
            XiBlock *blk = f->blocks[b];
            if (!blk)
                continue;
            for (uint32_t i = 0; i < blk->nvalues; i++) {
                XiValue *v = blk->values[i];
                if (!v || v->op != XI_COPY || v->nargs < 1 || !v->args[0])
                    continue;
                if (xi_copy_is_value_clone(v))
                    continue;
                /* Only RC objects carry ownership; a scalar copy is irrelevant
                 * to ARC and must stay a plain copy. */
                if (!xi_own_value_is_rc(v) || !xi_own_value_is_rc(v->args[0]))
                    continue;
                /* Value-type structs copy-on-assign: XI_COPY makes an
                 * INDEPENDENT struct, not a pointer alias. Turning it into a
                 * move would make a callee mutate the caller's struct (the
                 * `var q = p; f(q)` value-semantics contract). Only reference
                 * types (array/string/map/set/reference instances/closures)
                 * alias on copy and can transfer ownership through a move. */
                if ((v->type && v->type->is_value_type) ||
                    (v->args[0]->type && v->args[0]->type->is_value_type))
                    continue;
                if (!value_has_consuming_use(f, v))
                    continue; /* result only borrowed: keep the borrow-copy */
                v->op = XI_OWNER_FORWARD;
                changed = true;
            }
        }
    }
    for (uint16_t c = 0; c < f->nchildren; c++)
        arc_copy_to_move(f->children[c]);
}

/* ========== dup/drop Insertion ========== */

/* A consuming use site, in program order (block RPO, then index).
 *
 * For a phi use, the consume happens on the CFG edge preds[i] → join, so the
 * site is attributed to the PREDECESSOR block (blk = pred, user = the phi):
 * a dup for a non-last consume must execute only on that incoming path, and
 * the move for a last consume conceptually happens as control leaves the
 * predecessor. */
typedef struct {
    XiBlock *blk;
    XiValue *user;           /* the value whose arg consumes the tracked value */
    uint32_t order;          /* sort key: (rpo << 16) | index_in_block */
    uint16_t phi_pred_index; /* stable predecessor slot for a PHI-edge consume */
} ConsumeSite;

typedef struct {
    ConsumeSite *items;
    uint32_t count;
    uint32_t cap;
} ConsumeSiteVec;

typedef struct {
    XiValue **items;
    uint32_t count;
    uint32_t cap;
} XiValueVec;

typedef struct {
    XiFunc **items;
    uint32_t count;
    uint32_t cap;
} XiFuncVec;

static bool xi_func_vec_push(XiFuncVec *vec, XiFunc *fn) {
    XR_DCHECK(vec != NULL, "xi_func_vec_push: NULL vec");
    if (vec->count == vec->cap) {
        uint32_t new_cap = vec->cap ? vec->cap * 2 : 32;
        XiFunc **grown = (XiFunc **) xr_realloc(vec->items, new_cap * sizeof(XiFunc *));
        if (!grown)
            return false;
        vec->items = grown;
        vec->cap = new_cap;
    }
    vec->items[vec->count++] = fn;
    return true;
}

static bool consume_site_vec_push(ConsumeSiteVec *vec, XiBlock *blk, XiValue *user, uint32_t order,
                                  uint16_t phi_pred_index) {
    XR_DCHECK(vec != NULL, "consume_site_vec_push: NULL vec");
    if (vec->count == vec->cap) {
        uint32_t new_cap = vec->cap ? vec->cap * 2 : 16;
        ConsumeSite *grown = (ConsumeSite *) xr_realloc(vec->items, new_cap * sizeof(ConsumeSite));
        if (!grown)
            return false;
        vec->items = grown;
        vec->cap = new_cap;
    }
    vec->items[vec->count].blk = blk;
    vec->items[vec->count].user = user;
    vec->items[vec->count].order = order;
    vec->items[vec->count].phi_pred_index = phi_pred_index;
    vec->count++;
    return true;
}

static bool xi_value_vec_push(XiValueVec *vec, XiValue *value) {
    XR_DCHECK(vec != NULL, "xi_value_vec_push: NULL vec");
    if (vec->count == vec->cap) {
        uint32_t new_cap = vec->cap ? vec->cap * 2 : 64;
        XiValue **grown = (XiValue **) xr_realloc(vec->items, new_cap * sizeof(XiValue *));
        if (!grown)
            return false;
        vec->items = grown;
        vec->cap = new_cap;
    }
    vec->items[vec->count++] = value;
    return true;
}

/* Side-effecting ops can have RC-ish static types while carrying no usable
 * result. Borrowed loads and unknown call results need distinct ARC modes. */
static uint8_t op_result_ownership(uint16_t op) {
    return xi_op_result_ownership(op);
}

static bool op_has_trackable_result(uint16_t op) {
    return op_result_ownership(op) != XI_GEN_RESULT_OWNERSHIP_NONE;
}

/* A borrowed reference is loaded from a location owned by another object or
 * module slot. Dropping it would free storage still owned elsewhere. */
static bool op_produces_borrow(uint16_t op) {
    return op_result_ownership(op) == XI_GEN_RESULT_OWNERSHIP_BORROWED;
}

/* XI_COPY is normally an identity alias, but lowering marks source-level
 * value-struct copies as VALUE_CLONE.  That variant allocates independent
 * storage on both backends and therefore produces a fresh owning reference;
 * treating it as the table's ordinary borrowed COPY leaks every clone. */
static bool arc_value_produces_borrow(const XiValue *value) {
    return value && !xi_copy_is_value_clone(value) && op_produces_borrow(value->op);
}

static bool arc_value_produces_owned(const XiValue *value) {
    return value && (xi_copy_is_value_clone(value) ||
                     op_result_ownership(value->op) == XI_GEN_RESULT_OWNERSHIP_OWNED);
}

/* Representation selection runs after ordinary ARC insertion and may wrap a
 * borrowed RC value in BOX/UNBOX/CONVERT adapters.  Those adapters change only
 * the backend representation; they do not acquire ownership.  Late error-edge
 * cleanup discovery must therefore follow the adapter back to its source or a
 * ref-loaded Array<T> is mistaken for a fresh owner and released by the
 * callee.  SOURCE_MOVE/OWNER_FORWARD are deliberately excluded: they perform
 * an ownership transfer rather than preserve a borrow. */
static bool arc_value_is_borrow_alias(const XiValue *value, uint8_t depth) {
    if (!value || depth > 16)
        return false;
    /* A weak field load promotes (W1): its result is a fresh strong reference,
     * not a borrow of something the object owns — the object owns only the
     * handle. Treating it as a borrow would leak the promotion. */
    if (arc_value_produces_borrow(value))
        return true;
    switch (value->op) {
        case XI_BOX:
        case XI_UNBOX:
            return value->nargs == 1 &&
                   arc_value_is_borrow_alias(value->args[0], (uint8_t) (depth + 1));
        case XI_CONVERT:
            /* Only a reference-to-reference representation adapter preserves
             * a borrowed owner.  Semantic conversions such as int -> string
             * allocate a fresh value even when their scalar source came from
             * a borrowed PLACE_LOAD. */
            return value->nargs == 1 && xi_own_value_is_rc(value) &&
                   xi_own_value_is_rc(value->args[0]) &&
                   arc_value_is_borrow_alias(value->args[0], (uint8_t) (depth + 1));
        default:
            return false;
    }
}

/* Is this op a call whose result ownership we cannot determine without a
 * per-callee return-ownership summary? */
static bool op_is_call(uint16_t op) {
    return op_result_ownership(op) == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT;
}

static XiFunc *arc_resolve_callee(const XiFunc *caller, const XiValue *cv);

/* The one place that answers "which function does this call reach". A resolved
 * callee carries the whole-program fixed point for its return ABI, which is
 * stronger evidence than the contract lowering wrote at the call site; sites
 * that fail to resolve silently fall back to the weaker fact instead of
 * failing, so a form missed here becomes a wrong ownership answer rather than
 * an error. Promotion to a tail call changes neither the callee nor the operand
 * naming it, so both call forms resolve the same way. */
static XiFunc *arc_callee_of(const XiFunc *caller, const XiValue *call) {
    if (!call)
        return NULL;
    if ((call->op == XI_CALL || call->op == XI_TAIL_CALL) && call->nargs >= 1)
        return arc_resolve_callee(caller, call->args[0]);
    if (call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT)
        return xi_value_resolve_method_callee(caller, call);
    return NULL;
}

/* Map a semantic callee parameter to the operand that carries it at this call
 * site. Plain calls reserve operand zero for the callable. Namespace calls do
 * the same for the namespace carrier, while source instance calls place the
 * receiver in both callee parameter zero and call operand zero. Keeping this
 * distinction here prevents ownership users from accidentally applying the
 * free-function +1 offset to an instance method's explicit parameters. */
static int32_t arc_call_operand_for_parameter(const XiValue *call, const XiFunc *callee,
                                              int32_t parameter) {
    if (!call || parameter < 0)
        return -1;
    if ((call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT) && callee &&
        callee->has_receiver)
        return parameter;
    return parameter == INT32_MAX ? -1 : parameter + 1;
}

static int32_t arc_call_parameter_for_operand(const XiValue *call, const XiFunc *callee,
                                              uint16_t operand) {
    if (!call)
        return -1;
    if ((call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT) && callee &&
        callee->has_receiver)
        return operand;
    return operand == 0 ? -1 : (int32_t) operand - 1;
}

static bool arc_type_is_raw_pointer(const XrType *type) {
    return type && XR_TYPE_IS_POINTER(type);
}

static bool arc_value_is_raw_pointer_carrier(const XiValue *v) {
    if (!v)
        return false;
    if (arc_type_is_raw_pointer(v->type))
        return true;
    switch (v->op) {
        case XI_ARRAY_DATA_PTR:
            return true;
        case XI_BOX:
        case XI_COPY:
        case XI_CONVERT:
        case XI_PHI:
        case XI_SELECT:
            return v->nargs > 0 && arc_value_is_raw_pointer_carrier(v->args[0]);
        default:
            return false;
    }
}

static bool arc_raw_pointer_borrow_flows_to_user(const XiValue *member, const XiValue *user) {
    if (!member || !user)
        return false;
    if (user->op == XI_ARRAY_DATA_PTR && user->nargs >= 1 && user->args[0] == member)
        return true;
    if ((user->op == XI_CALL_METHOD || user->op == XI_CALL_METHOD_DIRECT) && user->nargs >= 1 &&
        user->args[0] == member && arc_type_is_raw_pointer(user->type) && user->aux &&
        strcmp((const char *) user->aux, "borrowPtr") == 0)
        return true;
    if (!arc_value_is_raw_pointer_carrier(member))
        return false;
    switch (user->op) {
        case XI_ADD:
            /* Ptr<T>.offset / p[i] address arithmetic keeps carrying the same
             * borrowed owner lifetime. The result type gate prevents ordinary
             * integer arithmetic from joining the borrow closure. */
            return arc_type_is_raw_pointer(user->type) && user->nargs >= 1 &&
                   user->args[0] == member;
        case XI_BOX:
        case XI_UNBOX:
        case XI_COPY:
        case XI_CONVERT:
            return user->nargs >= 1 && user->args[0] == member;
        case XI_CALL_BUILTIN:
            if (user->nargs < 1 || user->args[0] != member || !user->aux)
                return false;
            return strcmp((const char *) user->aux, "copy") == 0;
        case XI_PHI:
        case XI_SELECT:
        case XI_CLOSURE_NEW:
        case XI_STACK_ALLOC:
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] == member)
                    return true;
            }
            return false;
        default:
            return false;
    }
}

static bool arc_type_is_span_view(const XrType *type) {
    return type && XR_TYPE_IS_SLICE(type);
}

static bool arc_value_is_span_view_carrier(const XiValue *v) {
    if (!v)
        return false;
    if (arc_type_is_span_view(v->type))
        return true;
    switch (v->op) {
        case XI_COPY:
        case XI_CONVERT:
        case XI_BOX:
        case XI_UNBOX:
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
        case XI_PHI:
        case XI_SELECT:
            return v->nargs > 0 && arc_value_is_span_view_carrier(v->args[0]);
        default:
            return false;
    }
}

static bool arc_span_view_borrow_flows_to_user(const XiValue *member, const XiValue *user) {
    if (!member || !user)
        return false;
    if (user->op == XI_SLICE && arc_type_is_span_view(user->type) && user->nargs >= 1 &&
        user->args[0] == member)
        return true;
    const XiViewSourceEvidence *single = xi_view_evidence_single_source(&user->view_evidence);
    if (user->op == XI_CALL_BUILTIN && arc_type_is_span_view(user->type) && user->nargs >= 1 &&
        user->args[0] == member && user->xa_intrinsic_id == XA_INTRINSIC_STRING_BYTE_SLICE_VIEW &&
        single && single->source_operand == 0)
        return true;
    /* A declared view-return contract can root a Slice in a non-Slice owner.
     * `Buffer.asBytes() -> const Slice<u8> from this` is the minimal shape:
     * the receiver owns provider storage, while the returned value is only a
     * two-word view into that storage.  ViewEvidence is the lowering-sealed
     * authority for this relationship.  Consume it before requiring `member`
     * itself to be a Slice carrier, otherwise ARC releases a class/array owner
     * immediately after the call and leaves the returned view dangling. */
    if ((user->op == XI_CALL || user->op == XI_CALL_METHOD ||
         user->op == XI_CALL_METHOD_DIRECT) &&
        arc_type_is_span_view(user->type) && user->view_evidence.complete &&
        user->view_evidence.sources) {
        for (uint16_t i = 0; i < user->view_evidence.source_count; i++) {
            int16_t operand = user->view_evidence.sources[i].source_operand;
            if (operand >= 0 && (uint16_t) operand < user->nargs &&
                user->args[operand] == member)
                return true;
        }
    }
    if (!arc_value_is_span_view_carrier(member))
        return false;
    if (!arc_type_is_span_view(user->type))
        return false;
    switch (user->op) {
        case XI_SLICE_AS_BYTES:
        case XI_SLICE_REINTERPRET:
        case XI_SLICE_WINDOW:
        case XI_SLICE_FILL:
        case XI_SLICE_COPY:
        case XI_BYTE_SLICE_FILL:
        case XI_BYTE_SLICE_COPY:
        case XI_BYTE_SLICE_REPEAT:
        case XI_BYTE_ARRAY_COPY_WITHIN:
        case XI_BYTE_ARRAY_COPY_FROM:
        case XI_BYTE_ARRAY_APPEND_FROM:
        case XI_BYTE_ARRAY_REPEAT_FROM:
        case XI_COPY:
        case XI_CONVERT:
        case XI_BOX:
        case XI_UNBOX:
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
        case XI_PHI:
        case XI_SELECT:
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] == member)
                    return true;
            }
            return false;
        case XI_SLICE:
            return user->nargs >= 1 && user->args[0] == member;
        default:
            return false;
    }
}

/* ========== Per-callee return-ownership ==========
 *
 * A call result can be promoted from CALL_RESULT to OWNED when the callee
 * is statically known to return a FRESH (+1) reference that aliases none
 * of its arguments. CALL_RESULT mode never inserts an unconsumed drop
 * (the result might alias an argument), so every discarded fresh return
 * leaks; OWNED mode lets the death-point placement reclaim it.
 *
 * Channel receive/try methods materialize a fresh wrapper per call
 * (Recv<T> / SendResult enum instance plus the payload copied into the
 * receiving coroutine's heap). Receive loops would otherwise leak one
 * wrapper per message. This table contains only intrinsic semantic operations
 * that have no native declaration/body; declared native and source functions
 * publish their return-ownership metadata through the analyzer. */
static bool arc_mem_allocator_returns_fresh_buffer(const XiFunc *f, const XiValue *v) {
    if (!v || !xr_type_is_builtin_named_class(v->type, "Buffer"))
        return false;

    const char *member = NULL;
    if ((v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) && v->nargs >= 1) {
        const XiImportRef *ref = xi_value_import_ref(f, v->args[0]);
        if (!ref || !ref->module_path || strcmp(ref->module_path, "mem") != 0 || ref->member_name)
            return false;
        member = (const char *) v->aux;
    } else if (v->op == XI_CALL && v->nargs >= 1) {
        const XiImportRef *ref = xi_value_import_ref(f, v->args[0]);
        if (!ref || !ref->module_path || strcmp(ref->module_path, "mem") != 0)
            return false;
        member = ref->member_name;
    }

    return member && (strcmp(member, "alloc") == 0 || strcmp(member, "allocZeroed") == 0 ||
                      strcmp(member, "allocAligned") == 0);
}

/* Builtin collection/string iterator factories always allocate a new AOT
 * iterator shell. Match both the exact receiver kind and the zero-argument
 * method so user-defined structural lookalikes remain alias-uncertain. */
static bool arc_builtin_iterator_method_returns_fresh(const XiValue *v) {
    if (xi_map_entries_iterator_is_exact(v))
        return true;
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs != 1 ||
        !v->args[0])
        return false;
    const XrType *receiver = v->args[0]->type;
    if (!receiver)
        return false;
    XiMethodSymbolId method = xi_call_method_symbol_id(v);
    switch (receiver->kind) {
        case XR_KIND_STRING:
            return method == XI_METHOD_SYMBOL_RUNES || method == XI_METHOD_SYMBOL_ITERATOR ||
                   method == XI_METHOD_SYMBOL_ENTRIES_ITERATOR;
        case XR_KIND_ARRAY:
        case XR_KIND_JSON:
            return method == XI_METHOD_SYMBOL_ITERATOR ||
                   method == XI_METHOD_SYMBOL_ENTRIES_ITERATOR;
        case XR_KIND_MAP:
            return method == XI_METHOD_SYMBOL_ITERATOR;
        case XR_KIND_SET:
            return method == XI_METHOD_SYMBOL_ITERATOR;
        default:
            return false;
    }
}

/* Bodyless JSON namespace calls need an explicit +1 result contract.  Parsing,
 * stringification, path reads, merge, and JSON.value all return an owned value.
 * The JSON.value encoder recursively materializes containers and retains
 * borrowed string scalars, including a scalar root. */
static bool arc_builtin_json_method_returns_owned(const XiValue *v) {
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs < 1 ||
        !v->args[0] || !v->args[0]->aux || !v->aux)
        return false;
    const XiValue *recv = v->args[0];
    if (recv->op != XI_GET_BUILTIN || strcmp((const char *) recv->aux, "JSON") != 0)
        return false;

    const char *method = (const char *) v->aux;
    if (strcmp(method, "parse") == 0 || strcmp(method, "parseValue") == 0 ||
        strcmp(method, "parseObject") == 0 || strcmp(method, "parseWithRest") == 0 ||
        strcmp(method, "stringify") == 0 || strcmp(method, "merge") == 0 ||
        strcmp(method, "get") == 0 || strcmp(method, "require") == 0 ||
        strcmp(method, "asObject") == 0 || strcmp(method, "asArray") == 0 ||
        strcmp(method, "value") == 0)
        return true;
    return false;
}

static bool call_returns_intrinsic_fresh(const XiFunc *f, const XiValue *v) {
    if (!v)
        return false;
    if (v->op == XI_GEN_CALL)
        return true;
    /* Constructing a class allocates its instance, so the result cannot alias
     * an argument. Lowering proved this (XI_LOWERING_FLAG_CONSTRUCTOR_CALL);
     * the callee's spelling would not, because `super(...)` lowers to the same
     * method call named "constructor" while returning the receiver at +0. */
    if (xi_value_is_constructor_call(v))
        return true;
    if (v && v->op == XI_CALL_BUILTIN && v->nargs == 0 && v->aux &&
        strcmp((const char *) v->aux, "StringBuilder") == 0 &&
        xr_type_is_builtin_named_class(v->type, "StringBuilder"))
        return true;
    if (v->op == XI_CALL_METHOD && v->nargs == 1 && v->args[0] && v->aux &&
        strcmp((const char *) v->aux, "toString") == 0 &&
        xr_type_is_builtin_named_class(v->args[0]->type, "StringBuilder"))
        return true;
    /* Container-building intrinsics allocate a new array on both backends;
     * the result aliases no argument, so it is a fresh +1 the caller owns
     * (and must drop at its death point when never consumed). */
    if (v->op == XI_CALL_BUILTIN &&
        (v->array_intrinsic_kind == XI_ARRAY_INTRINSIC_WITH_CAPACITY ||
         v->array_intrinsic_kind == XI_ARRAY_INTRINSIC_FILLED_NEW ||
         (v->aux && (strcmp((const char *) v->aux, "copy") == 0 ||
                     strcmp((const char *) v->aux, "array_copy_new") == 0))))
        return true;
    if (arc_mem_allocator_returns_fresh_buffer(f, v))
        return true;
    if (arc_builtin_iterator_method_returns_fresh(v))
        return true;
    if (arc_builtin_json_method_returns_owned(v))
        return true;
    if (v->op != XI_CALL_METHOD || v->nargs < 1 || !v->args[0])
        return false;
    const struct XrType *recv_type = v->args[0]->type;
    if (!recv_type || recv_type->kind != XR_KIND_CHANNEL)
        return false;
    const char *method = (const char *) v->aux;
    if (!method)
        return false;
    return strcmp(method, "recv") == 0 || strcmp(method, "tryRecv") == 0 ||
           strcmp(method, "recvOr") == 0 || strcmp(method, "recvTimeout") == 0 ||
           strcmp(method, "trySend") == 0 || strcmp(method, "sendTimeout") == 0;
}

static bool call_returns_fresh(const XiFunc *f, const XiValue *v) {
    if (call_returns_intrinsic_fresh(f, v))
        return true;
    XiFunc *callee = arc_callee_of(f, v);
    if (callee && callee->arc_return_ownership.complete)
        return callee->arc_return_ownership.kind == XI_RETURN_OWNERSHIP_OWNED;
    if (v && v->call_return_ownership.complete)
        return v->call_return_ownership.kind == XI_RETURN_OWNERSHIP_OWNED;
    return false;
}

/* Is this value a candidate for dup/drop? RC type, not a stack/region
 * alloc, not a scalar, produces an owning reference (not a borrow). */
static bool tracks_rc(const XiValue *v) {
    if (!v)
        return false;
    if (v->op == XI_STACK_ALLOC)
        return v->aux_int == XI_CLOSURE_NEW && xi_own_value_is_rc(v);
    if (v->op != XI_PARAM && !op_has_trackable_result(v->op))
        return false; /* side-effect op: no owning result */
    /* Call results: precise per-callee summaries classify fresh (+1) results
     * as owned and argument/static aliases as borrowed. An unresolved,
     * foreign, or mixed result remains conservative CALL_RESULT. We still
     * track it (so a result consumed by more than one use
     * gets a dup before each non-last use, preventing a use-after-free when
     * the first consumer moves/frees it), but process_value_ex uses the
     * CALL_RESULT mode which never inserts an unconsumed drop — dropping an
     * aliased borrow would be a use-after-free. Adding dups is always sound;
     * the only cost is that an unresolved discarded fresh return may leak. */
    if (op_is_call(v->op))
        return xi_own_value_is_rc(v);
    /* A heap allocation is tracked on the strength of what it IS, never of what
     * a later pass might turn it into. The XI_STACK_ALLOC case above is the
     * only "this one has frame lifetime" answer.
     *
     * Skipping NO_ESCAPE heap allocs here used to stand in for "stack alloc
     * rewrite will get this one", but that pass runs only when backend
     * lowering does — AOT. It is also what promotes an allocation it cannot
     * stack-allocate to ESC_ARG (xi_stack_alloc_rewrite), which is what put
     * those values back in scope for tracking. On the VM neither half runs, so
     * a NO_ESCAPE allocation stayed on the heap with nothing to release it:
     * every non-escaping closure leaked (2M closures => 143 MB max RSS). */
    return xi_own_value_is_rc(v);
}

/* ========== Cross-function borrow signatures ==========
 *
 * A free function that only borrows an argument (reads it, never stores,
 * returns, or forwards it) must not have that argument MOVED into it: the
 * callee never releases a borrowed parameter (param_is_borrowed), so a moved-in
 * reference would leak. The caller therefore keeps ownership of a borrowed
 * argument and drops it at the argument's death point.
 *
 * Caller and callee read the SAME conservative signature (BORROWED only when
 * proven), computed once per function on its pre-ARC IR and cached on the
 * XiFunc, so the two sides always agree (callee does not drop, caller does).
 *
 * The agreement holds across module boundaries too: multi-module drivers
 * compile in topological order and resolve import refs before running ARC on
 * a module, so a cross-module callee is always fully compiled and its cached
 * signature frozen by the time any caller reads it. A callee that stays
 * unresolved (dynamic call, uncompiled module) keeps the moved-argument
 * convention, whose failure direction is a leak, never a double free. */

/* Resolve a call's callee value to its XiFunc, or NULL when not statically
 * known. Covers the cases ARC can use: a direct closure (XiFunc* in aux), a
 * top-level function loaded from a shared slot, identity COPY chains, and a
 * member imported from another module whose ref was resolved before ARC ran
 * (multi-module drivers resolve imports in topological order, so the callee
 * module is fully compiled and its borrow signature is frozen). */
static XiFunc *arc_resolve_callee(const XiFunc *caller, const XiValue *cv) {
    if (!cv)
        return NULL;
    if (cv->op == XI_CLOSURE_NEW && cv->aux)
        return (XiFunc *) cv->aux;
    if (cv->op == XI_STACK_ALLOC && cv->aux_int == XI_CLOSURE_NEW && cv->aux)
        return (XiFunc *) cv->aux;
    if (cv->op == XI_GET_SHARED) {
        int64_t slot = cv->aux_int;
        for (const XiFunc *fn = caller; fn; fn = fn->parent_func) {
            if (fn->shared_slot_funcs && slot >= 0 && slot < (int64_t) fn->shared_slot_func_count &&
                fn->shared_slot_funcs[slot])
                return fn->shared_slot_funcs[slot];
        }
        /* Not a module-local function slot: the slot may hold a member
         * imported from another module — fall through to the import-ref
         * lookup below. */
    } else if (xi_copy_is_identity_alias(cv) && cv->nargs >= 1) {
        return arc_resolve_callee(caller, cv->args[0]);
    }
    const XiImportRef *ref = xi_value_import_ref(caller, cv);
    return ref ? ref->resolved_func : NULL;
}

static XiReturnOwnership arc_return_ownership(uint8_t kind, int16_t param_index, bool complete) {
    XiReturnOwnership result;
    result.kind = kind;
    result.param_index = param_index;
    result.complete = complete;
    return result;
}

static XiReturnOwnership arc_return_unknown(void) {
    return arc_return_ownership(XI_RETURN_OWNERSHIP_UNKNOWN, -1, false);
}

#define XI_RETURN_OWNERSHIP_NULL_JOIN UINT8_MAX

static XiReturnOwnership arc_return_null_join(void) {
    return arc_return_ownership(XI_RETURN_OWNERSHIP_NULL_JOIN, -1, true);
}

static bool arc_return_ownership_equal(XiReturnOwnership a, XiReturnOwnership b) {
    return a.kind == b.kind && a.param_index == b.param_index && a.complete == b.complete;
}

static XiReturnOwnership arc_return_value_ownership(XiFunc *f, XiValue *value, uint8_t depth) {
    if (!f || !value || depth > 32)
        return arc_return_unknown();

    for (uint16_t p = 0; p < xi_func_semantic_param_count(f); p++) {
        if (f->params[p] == value && f->arc_borrow_sig && f->arc_borrow_sig->valid &&
            p < f->arc_borrow_sig->nparams && f->arc_borrow_sig->param_own[p] == XI_OWN_OWNED)
            return arc_return_ownership(XI_RETURN_OWNERSHIP_OWNED, -1, true);
        if (f->params[p] == value)
            return arc_return_ownership(XI_RETURN_OWNERSHIP_BORROWED_PARAM, (int16_t) p, true);
    }

    if ((xi_copy_is_identity_alias(value) || xi_op_is_identity_forward(value->op)) &&
        value->nargs >= 1) {
        return arc_return_value_ownership(f, value->args[0], (uint8_t) (depth + 1));
    }

    if ((value->op == XI_PHI || value->op == XI_SELECT) && value->nargs > 0) {
        XiReturnOwnership merged = arc_return_unknown();
        for (uint16_t i = 0; i < value->nargs; i++) {
            XiReturnOwnership arm =
                arc_return_value_ownership(f, value->args[i], (uint8_t) (depth + 1));
            if (!arm.complete)
                return arc_return_unknown();
            if (arm.kind == XI_RETURN_OWNERSHIP_NULL_JOIN)
                continue;
            if (!merged.complete)
                merged = arm;
            else if (arm.kind != merged.kind || arm.param_index != merged.param_index)
                return arc_return_unknown();
        }
        return merged.complete ? merged : arc_return_null_join();
    }

    if (value->op == XI_CONST && value->type && value->type->kind == XR_KIND_NULL)
        return arc_return_null_join();
    if (value->op == XI_CONST || value->op == XI_GET_SHARED || value->op == XI_IMPORT_REF)
        return arc_return_ownership(XI_RETURN_OWNERSHIP_BORROWED_STATIC, -1, true);

    if (op_is_call(value->op)) {
        if (call_returns_intrinsic_fresh(f, value))
            return arc_return_ownership(XI_RETURN_OWNERSHIP_OWNED, -1, true);
        if (xi_call_result_aliases_receiver(value) && value->nargs >= 1)
            return arc_return_value_ownership(f, value->args[0], (uint8_t) (depth + 1));
        XiFunc *callee = arc_callee_of(f, value);
        XiReturnOwnership summary = callee && callee->arc_return_ownership.complete
                                        ? callee->arc_return_ownership
                                        : value->call_return_ownership;
        if (summary.complete) {
            if (summary.kind != XI_RETURN_OWNERSHIP_BORROWED_PARAM)
                return summary;
            int32_t actual = arc_call_operand_for_parameter(value, callee, summary.param_index);
            if (actual < 0 || (uint32_t) actual >= value->nargs)
                return arc_return_unknown();
            return arc_return_value_ownership(f, value->args[(uint16_t) actual],
                                              (uint8_t) (depth + 1));
        }
        return arc_return_unknown();
    }

    if ((value->op == XI_BOX || value->op == XI_UNBOX || value->op == XI_CONVERT) &&
        value->nargs == 1 && arc_value_is_borrow_alias(value, 0))
        return arc_return_value_ownership(f, value->args[0], (uint8_t) (depth + 1));
    if (arc_value_produces_borrow(value))
        return arc_return_unknown();
    if (arc_value_produces_owned(value))
        return arc_return_ownership(XI_RETURN_OWNERSHIP_OWNED, -1, true);
    return arc_return_unknown();
}

XR_FUNC XiReturnOwnership xi_arc_value_return_ownership(const XiFunc *function,
                                                        const XiValue *value) {
    XiReturnOwnership ownership =
        arc_return_value_ownership((XiFunc *) function, (XiValue *) value, 0);
    if (ownership.complete && ownership.kind == XI_RETURN_OWNERSHIP_NULL_JOIN)
        return arc_return_ownership(XI_RETURN_OWNERSHIP_BORROWED_STATIC, -1, true);
    return ownership;
}

XR_FUNC uint8_t xi_arc_value_result_ownership(const XiFunc *function, const XiValue *value) {
    if (!value)
        return XI_GEN_RESULT_OWNERSHIP_NONE;
    if (xi_copy_is_value_clone(value))
        return XI_GEN_RESULT_OWNERSHIP_OWNED;
    if (arc_value_is_borrow_alias(value, 0))
        return XI_GEN_RESULT_OWNERSHIP_BORROWED;
    /* A receiver-returning builtin forwards the receiver's ownership; it does
     * not manufacture a fresh owner.  The explicit alias is lowering-owned,
     * so this projection never recovers the contract from an opcode or name. */
    if (value->result_alias_operand >= 0 && (uint16_t) value->result_alias_operand < value->nargs &&
        value->args) {
        const XiValue *source = value->args[value->result_alias_operand];
        if (source && source->op == XI_PARAM) {
            uint8_t parameter_ownership = xi_arc_parameter_ownership(function, source);
            if (parameter_ownership == XI_OWN_BORROWED)
                return XI_GEN_RESULT_OWNERSHIP_BORROWED;
            if (parameter_ownership == XI_OWN_OWNED)
                return XI_GEN_RESULT_OWNERSHIP_OWNED;
        }
        uint8_t source_ownership = xi_arc_value_result_ownership(function, source);
        if (source_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED ||
            source_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED)
            return source_ownership;
    }
    if (!op_is_call(value->op))
        return op_result_ownership(value->op);
    XiReturnOwnership ownership = xi_arc_value_return_ownership(function, value);
    if (!ownership.complete)
        return XI_GEN_RESULT_OWNERSHIP_CALL_RESULT;
    return ownership.kind == XI_RETURN_OWNERSHIP_OWNED ? XI_GEN_RESULT_OWNERSHIP_OWNED
                                                       : XI_GEN_RESULT_OWNERSHIP_BORROWED;
}

XR_FUNC int16_t xi_arc_value_alias_operand(const XiFunc *function, const XiValue *value) {
    if (!function || !value)
        return -1;
    if (value->result_alias_operand >= 0)
        return value->result_alias_operand;
    if (value->nargs == 1 && value->op == XI_CHECKTYPE)
        return 0;
    if (value->nargs == 1 &&
        (value->op == XI_BOX || value->op == XI_UNBOX || value->op == XI_CONVERT) &&
        arc_value_is_borrow_alias(value, 0))
        return 0;
    if (!op_is_call(value->op))
        return -1;
    XiFunc *callee = arc_callee_of(function, value);
    XiReturnOwnership summary = callee && callee->arc_return_ownership.complete
                                    ? callee->arc_return_ownership
                                    : value->call_return_ownership;
    if (!summary.complete || summary.kind != XI_RETURN_OWNERSHIP_BORROWED_PARAM ||
        summary.param_index < 0)
        return -1;
    int32_t operand = arc_call_operand_for_parameter(value, callee, summary.param_index);
    return operand >= 0 && (uint32_t) operand < value->nargs && operand <= INT16_MAX
               ? (int16_t) operand
               : -1;
}

static XiReturnOwnership arc_infer_return_ownership(XiFunc *f) {
    if (!f || !xi_own_function_return_is_rc(f))
        return arc_return_unknown();
    XiReturnOwnership result = arc_return_unknown();
    bool seen = false;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk || blk->kind != XI_BLOCK_RETURN || !blk->control)
            continue;
        XiReturnOwnership current = arc_return_value_ownership(f, blk->control, 0);
        if (!current.complete)
            return arc_return_unknown();
        if (current.kind == XI_RETURN_OWNERSHIP_NULL_JOIN)
            continue;
        if (!seen) {
            result = current;
            seen = true;
        } else if (result.kind != current.kind || result.param_index != current.param_index) {
            return arc_return_unknown();
        }
    }
    return seen ? result : arc_return_ownership(XI_RETURN_OWNERSHIP_BORROWED_STATIC, -1, true);
}

/* Return fn's cached borrow signature, computing it (on the pre-ARC IR) on
 * first use. Arena-allocated on fn; lives until fn is destroyed. Returns NULL
 * only on allocation failure, in which case callers conservatively treat
 * parameters as owned. The pointer is cached before analysis so it also guards
 * against unbounded recursion through the call graph. */
static const XiBorrowSig *arc_get_borrow_sig(XiFunc *fn) {
    if (!fn)
        return NULL;
    if (fn->arc_borrow_sig)
        return fn->arc_borrow_sig;
    XiBorrowSig *sig = (XiBorrowSig *) xi_func_arena_alloc(fn, sizeof(XiBorrowSig));
    if (!sig)
        return NULL;
    memset(sig, 0, sizeof(*sig));
    fn->arc_borrow_sig = sig;
    xi_ensure_rpo(fn);
    xi_ensure_dominators(fn);
    XiOwnResult own;
    if (xi_own_analyze(fn, &own)) {
        *sig = own.sig;
        xi_own_free(&own);
    }
    return sig;
}

/* Does `callee` only borrow its parameter `pidx`? Conservative: false unless
 * proven borrowed (so the caller never wrongly skips a real consume). */
static bool arc_callee_borrows_param(XiFunc *callee, uint16_t pidx) {
    const XiBorrowSig *sig = arc_get_borrow_sig(callee);
    return sig && sig->valid && pidx < sig->nparams && sig->param_own[pidx] == XI_OWN_BORROWED;
}

/* Is call argument `a` of `user` bound to a parameter its statically known
 * callee only borrows? A borrowed argument is not consumed: the caller keeps
 * ownership and drops it at the argument's death point, because the callee
 * never releases a borrowed parameter. args[0] is the callee (plain call) or
 * the namespace receiver (module-member call). A namespace call therefore
 * maps argument `a` to parameter `a - 1`; a source instance call includes its
 * receiver as callee parameter zero and maps operand `a` to parameter `a`. */
static bool arc_call_arg_is_callee_borrowed(XiFunc *f, const XiValue *user, uint16_t a) {
    if (a < 1)
        return false;
    XiFunc *callee = arc_callee_of(f, user);
    int32_t parameter = arc_call_parameter_for_operand(user, callee, a);
    if (parameter < 0 || parameter > UINT16_MAX)
        return false;
    /* The call-site return contract is the only ownership evidence when a
     * relative module function has no live XiFunc pointer in this compilation.
     * A statically resolved callee is stronger: its fixed-point parameter mode
     * and return ABI must be consumed as one coherent contract. Mixing a stale
     * BORROWED_PARAM call-site result with an OWNED callee ABI would leave the
     * argument in the caller while the callee assumes it was moved. */
    if (!callee && user->call_return_ownership.complete &&
        user->call_return_ownership.kind == XI_RETURN_OWNERSHIP_BORROWED_PARAM &&
        user->call_return_ownership.param_index == (int16_t) parameter)
        return true;
    if (!callee)
        return false;
    if (callee->is_vararg && (uint32_t) parameter >= callee->nparams)
        parameter = callee->nparams;
    return arc_callee_borrows_param(callee, (uint16_t) parameter);
}

XR_FUNC bool xi_arc_operand_consumes(const XiFunc *function, const XiValue *operation,
                                     uint16_t operand) {
    if (!function || !operation || operand >= operation->nargs)
        return false;
    const XiValue *argument = operation->args[operand];
    if (stack_alloc_closure_use_is_scoped_par_for_callback(operation, operand, argument))
        return argument && argument->op != XI_STACK_ALLOC;
    if (!xi_own_value_arg_is_consuming(operation, operand))
        return false;
    /* PSC4 proves that this exact leaf value aggregate has value semantics and
     * no managed leaves.  A direct-call use copies the value; it cannot move an
     * RC ownership token that does not exist.  Keep this refinement row-bound:
     * the global aggregate type shape remains conservatively RC-managed. */
    if (xi_own_value_is_psc_leaf_aggregate(argument))
        return false;
    if (operation->op == XI_STACK_ALLOC &&
        stack_alloc_closure_uses_are_scoped_par_for_callbacks((XiFunc *) function, operation))
        return false;
    return !arc_call_arg_is_callee_borrowed((XiFunc *) function, operation, operand);
}

XR_FUNC uint8_t xi_arc_parameter_ownership(const XiFunc *function, const XiValue *parameter) {
    if (!function || !parameter || parameter->op != XI_PARAM || parameter->aux_int < 0)
        return XI_OWN_NONE;
    uint32_t index = (uint32_t) parameter->aux_int;
    if ((function->receiver_borrowed && index == 0) || function->operator_borrowed)
        return xi_own_value_is_rc(parameter) ? XI_OWN_BORROWED : XI_OWN_NONE;
    if (function->arc_borrow_sig && function->arc_borrow_sig->valid &&
        index < function->arc_borrow_sig->nparams)
        return function->arc_borrow_sig->param_own[index];
    if (function->is_vararg && index == function->nparams)
        return xi_own_value_is_rc(parameter) ? XI_OWN_OWNED : XI_OWN_NONE;
    return xi_own_value_is_rc(parameter) ? XI_OWN_OWNED : XI_OWN_NONE;
}

/* Collect consuming uses of `target` across the function, in program order.
 * The list grows dynamically; silently dropping a late consume would be a
 * memory-safety bug because ARC would miss a required retain. */
static bool collect_consume_sites(XiFunc *f, XiValue *target, ConsumeSiteVec *sites) {
    XR_DCHECK(sites != NULL, "collect_consume_sites: NULL sites");
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *user = blk->values[i];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != target)
                    continue;
                if (user->op == XI_STACK_ALLOC &&
                    stack_alloc_closure_uses_are_scoped_par_for_callbacks(f, user))
                    continue;
                if (!xi_own_value_arg_is_consuming(user, a))
                    continue;
                /* A call argument the callee only borrows is not consumed: the
                 * caller keeps ownership and drops it at its death point (the
                 * callee never releases a borrowed parameter). */
                if (arc_call_arg_is_callee_borrowed(f, user, a))
                    continue;
                if (!consume_site_vec_push(sites, blk, user, (blk->rpo << 16) | (i & 0xFFFF),
                                           UINT16_MAX))
                    return false;
                break; /* one consume record per user is enough */
            }
        }
        /* Phi uses consume the incoming value on its edge. Phis live on
         * blk->phis (not in blk->values[]), so scan them explicitly. The
         * same value may flow in on several edges; each edge is an
         * independent consume site (no dedup). */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] != target)
                    continue;
                /* A loop-carried phi may forward its own current SSA owner on
                 * a backedge. That is slot continuity, not a second consume;
                 * retaining it once per iteration leaks unboundedly. */
                if (target == &phi->value)
                    continue;
                XiBlock *pred = (a < blk->npreds) ? blk->preds[a] : NULL;
                if (!pred)
                    continue;
                /* 0xFFFE = end of the predecessor block: after every value
                 * index, before a return terminator's 0xFFFF. */
                if (!consume_site_vec_push(sites, pred, &phi->value, (pred->rpo << 16) | 0xFFFE, a))
                    return false;
            }
        }
        /* Block control (return value) consumes the value. */
        if (blk->control == target && blk->kind == XI_BLOCK_RETURN) {
            if (!consume_site_vec_push(sites, blk, NULL, (blk->rpo << 16) | 0xFFFF, UINT16_MAX))
                return false;
        }
    }
    return true;
}

static int cmp_site(const void *pa, const void *pb) {
    const ConsumeSite *a = pa, *b = pb;
    if (a->order < b->order)
        return -1;
    if (a->order > b->order)
        return 1;
    return 0;
}

/* Insert a XI_RETAIN(target) immediately before `user` in `blk`.
 * Since dup has no result dependency, placing it just before the user is
 * correct; we insert after the value preceding `user`. */
static void insert_dup_before(XiFunc *f, XiBlock *blk, XiValue *user, XiValue *target) {
    /* Find the slot before `user` to use as anchor (insert-after). */
    uint32_t user_idx = blk->nvalues; /* sentinel: not found */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] == user) {
            user_idx = i;
            break;
        }
    }
    XiValue *anchor = (user_idx > 0 && user_idx < blk->nvalues) ? blk->values[user_idx - 1] : NULL;
    XiValue *dup = xi_value_insert_after(f, blk, anchor, XI_RETAIN, target->type, 1);
    XR_DCHECK(dup != NULL, "xi_arc: failed to create RETAIN");
    dup->args[0] = target;
    dup->flags = XI_FLAG_SIDE_EFFECT;
    dup->escape = target->escape;
    /* xi_value_insert_after(anchor=NULL) appends to the block tail, but when the
     * consume is the block's first value the dup must sit at the HEAD so it runs
     * BEFORE the consume. This is critical for `go closure(...)`: the spawned
     * coroutine can run on another worker the instant GO returns, so a dup that
     * landed after GO would let the coroutine release the closure before this +1
     * executes (a cross-thread use-after-free / "closure invalid"). Rotate the
     * appended dup to the front. */
    if (user_idx == 0 && blk->nvalues > 1 && blk->values[blk->nvalues - 1] == dup) {
        for (uint32_t i = blk->nvalues - 1; i > 0; i--)
            blk->values[i] = blk->values[i - 1];
        blk->values[0] = dup;
    }
}

static void insert_dup_after(XiFunc *f, XiBlock *blk, XiValue *anchor, XiValue *target) {
    XiValue *dup = xi_value_insert_after(f, blk, anchor, XI_RETAIN, target->type, 1);
    XR_DCHECK(dup != NULL, "xi_arc: failed to create RETAIN");
    dup->args[0] = target;
    dup->flags = XI_FLAG_SIDE_EFFECT;
    dup->escape = target->escape;
    /* A NULL anchor from phi_dup_anchor means the block is nothing but its
     * trailing XI_RELEASE run: the dup belongs BEFORE those releases, at the
     * head. xi_value_insert_after(NULL) appends to the tail instead, which
     * places the retain AFTER the releases. When the promoted phi value aliases
     * a loop-carried owner released in that same run (a ref-loaded Array<T>
     * reloaded via PLACE_LOAD), releasing first drops the shared object to zero
     * and frees it, so the trailing retain resurrects freed memory. Rotate the
     * appended dup to the head, matching insert_dup_before's first-value case. */
    if (anchor == NULL && blk->nvalues > 1 && blk->values[blk->nvalues - 1] == dup) {
        for (uint32_t i = blk->nvalues - 1; i > 0; i--)
            blk->values[i] = blk->values[i - 1];
        blk->values[0] = dup;
    }
}

/* Anchor for an end-of-block dup (a phi-edge consume): the last value that is
 * NOT part of the block's trailing XI_RELEASE run. A value promoted for a phi
 * edge must be retained BEFORE any death-drop at the block end, otherwise a
 * release of its source (e.g. the parent of a borrowed projection) would free
 * it first — a use-after-free. Retain-before-release is always safe, so placing
 * the dup ahead of the trailing releases fixes the ordering for both insertion
 * orders (the source drop and the projection dup are inserted by separate
 * passes). */
static XiValue *phi_dup_anchor(XiBlock *blk) {
    uint32_t n = blk->nvalues;
    while (n > 0 && blk->values[n - 1] && blk->values[n - 1]->op == XI_RELEASE)
        n--;
    return n > 0 ? blk->values[n - 1] : NULL;
}

/* Insert a XI_RELEASE(target) right after `anchor` in `blk`. */
static void insert_drop_after(XiFunc *f, XiBlock *blk, XiValue *anchor, XiValue *target) {
    XiValue *drop = xi_value_insert_after(f, blk, anchor, XI_RELEASE, target->type, 1);
    XR_DCHECK(drop != NULL, "xi_arc: failed to create RELEASE");
    drop->args[0] = target;
    drop->flags = XI_FLAG_SIDE_EFFECT;
    drop->escape = target->escape;
}

/* ========== Death-point drop placement (unconsumed owned values) ==========
 *
 * A value that is owned but never consumed must be released exactly once on
 * every path that executes its definition. Releasing at function returns is
 * wrong twice over: a definition inside a loop body or branch arm does not
 * dominate the returns (a dominance-gated return drop then silently skips
 * it, leaking one object per loop iteration), and even where it would be
 * sound it pins the object far past its last use.
 *
 * Placement is classic backward liveness over the CFG:
 *
 *   live_out(B) = OR over successors S of live_in(S)
 *   live_in(B)  = has_use(B) || live_out(B)        for B != def block
 *   live_in(def block) = false                      (the def kills upward)
 *
 * SSA guarantees every use is dominated by the def, and the def-block kill
 * stops backward propagation there, so the live region stays inside the
 * def's dominance region: a drop can never execute before the definition.
 *
 * The live→dead frontier is crossed exactly once per execution (liveness is
 * monotone along any path), and a release is placed on each frontier point:
 *
 *   - death in block: live at entry (or the def block) but dead at exit →
 *     release right after the last use in that block (right after the def
 *     itself when the value is never used).
 *   - death on edge: B is live-out only because of another successor →
 *     release at the head of the dead successor when this edge is its only
 *     entry; otherwise the edge is split so the release cannot execute on
 *     paths that never ran the definition. */

typedef struct {
    uint8_t has_use;           /* target is used by a value in this block */
    uint8_t live_in;           /* target live at block entry */
    uint8_t live_out;          /* target live at block exit */
    uint8_t use_at_end;        /* last use is the block terminator (control) */
    uint8_t moved_in_block;    /* a consuming MOVE transfers the owner in this block */
    uint8_t moved_on_phi_edge; /* a phi MOVE transfers it on one or more outgoing edges */
    uint32_t last_use;         /* index in blk->values of the last use */
} ArcLive;

static bool arc_target_available_at_try(const XiValue *target, const XiValue *try_op) {
    if (!target || !try_op || !target->block || !try_op->block)
        return false;
    if (target->block != try_op->block)
        return xi_dominates(target->block, try_op->block);
    if (target->op == XI_PARAM || target->op == XI_PHI)
        return true;
    for (uint32_t i = 0; i < try_op->block->nvalues; i++) {
        XiValue *value = try_op->block->values[i];
        if (value == target)
            return true;
        if (value == try_op)
            return false;
    }
    return false;
}

/* A source-level panic handler is an executable successor of XI_TRY even
 * though it is intentionally absent from the ordinary two-way CFG.  When an
 * owner available at registration is used in the protected body, keep that
 * owner live through every matching XI_END_TRY.  Death-frontier placement then
 * emits a physical RELEASE on both the normal exit and the panic handler; it
 * also prevents a normal-path death drop from running while the handler can
 * still unwind to the same owner.  Static-cleanup TRY regions already carry
 * their explicit cleanup frontier and must not be duplicated here. */
static void arc_seed_user_try_cleanup_uses(XiFunc *f, XiValue *target, ArcLive *live) {
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *registration = f->blocks[b];
        if (!registration)
            continue;
        for (uint32_t i = 0; i < registration->nvalues; i++) {
            XiValue *try_op = registration->values[i];
            if (!try_op || try_op->op != XI_TRY || !try_op->aux ||
                try_op->aux_int == XI_TRY_AUX_STATIC_CLEANUP ||
                !arc_target_available_at_try(target, try_op))
                continue;
            XiBlock *body_entry = registration->succs[0];
            bool protected_use = false;
            for (uint32_t u = 0; body_entry && u < f->nblocks && !protected_use; u++) {
                XiBlock *candidate = f->blocks[u];
                if (candidate && live[u].has_use && xi_dominates(body_entry, candidate))
                    protected_use = true;
            }
            if (!protected_use)
                continue;
            for (uint32_t e = 0; e < f->nblocks; e++) {
                XiBlock *exit = f->blocks[e];
                if (!exit)
                    continue;
                for (uint32_t v = 0; v < exit->nvalues; v++) {
                    XiValue *end_try = exit->values[v];
                    if (!end_try || end_try->op != XI_END_TRY || end_try->aux != try_op)
                        continue;
                    live[e].has_use = 1;
                    if (!live[e].use_at_end && live[e].last_use < v)
                        live[e].last_use = v;
                }
            }
        }
    }
}

/* Insert a XI_RELEASE(target) as the first instruction of `blk`. Needed for
 * edge deaths, where the release must run before anything else in the dead
 * successor. */
static void insert_drop_at_head(XiFunc *f, XiBlock *blk, XiValue *target) {
    XiValue *drop = xi_value_insert_after(f, blk, NULL, XI_RELEASE, target->type, 1);
    XR_DCHECK(drop != NULL, "xi_arc: failed to create RELEASE");
    drop->args[0] = target;
    drop->flags = XI_FLAG_SIDE_EFFECT;
    drop->escape = target->escape;
    /* xi_value_insert_after(anchor=NULL) appends; rotate it to the front. */
    for (uint32_t i = blk->nvalues - 1; i > 0; i--)
        blk->values[i] = blk->values[i - 1];
    blk->values[0] = drop;
}

/* Split the CFG edge pred→succ with a fresh PLAIN block. Phi args in succ
 * keep their positions (the pred slot is replaced in place). Returns the new
 * block, or NULL on failure. */
static XiBlock *arc_split_edge(XiFunc *f, XiBlock *pred, XiBlock *succ) {
    XiBlock *mid = xi_block_new(f);
    if (!mid)
        return NULL;
    mid->kind = XI_BLOCK_PLAIN;
    mid->succs[0] = succ;
    bool ok = xi_cfg_replace_successor(pred, succ, mid);
    ok = ok && xi_cfg_replace_pred(succ, pred, mid);
    ok = ok && xi_cfg_append_pred(mid, pred, NULL, 0);
    XR_DCHECK(ok, "xi_arc: edge split failed");
    return ok ? mid : NULL;
}

/* Collect `target` plus the transitive closure of borrowed projections
 * (GETFIELD and friends, Ptr/MutPtr data-pointer borrows, and Slice value
 * views). A borrow reads through the owner without holding a reference, so the
 * owner must outlive the projection's last use — otherwise the release would
 * free storage a live borrow still points into. Returns a heap array (caller
 * frees) with the count in *out_count, or NULL on OOM. */
static XiValue **arc_collect_borrow_closure(XiFunc *f, XiValue *target, uint32_t *out_count) {
    enum {
        ARC_TRACK_INIT = 16
    };
    XiValue **tracked = (XiValue **) xr_malloc(ARC_TRACK_INIT * sizeof(XiValue *));
    if (!tracked)
        return NULL;
    uint32_t ntracked = 0, track_cap = ARC_TRACK_INIT;
    tracked[ntracked++] = target;
    for (uint32_t scan = 0; scan < ntracked; scan++) {
        XiValue *member = tracked[scan];
        for (uint32_t b = 0; b < f->nblocks; b++) {
            XiBlock *blk = f->blocks[b];
            if (!blk)
                continue;
            for (uint32_t i = 0; i < blk->nvalues; i++) {
                XiValue *u = blk->values[i];
                if (!u)
                    continue;
                bool is_member_borrow = false;
                if (arc_raw_pointer_borrow_flows_to_user(member, u) ||
                    arc_span_view_borrow_flows_to_user(member, u)) {
                    is_member_borrow = true;
                } else if (arc_value_produces_borrow(u) && xi_own_type_may_be_ref(u->type)) {
                    /* A projection (GETFIELD / INDEX_GET / ...) borrows through
                     * any argument that is the tracked member. The result need
                     * only be a POSSIBLE reference: a Json field typed `null`
                     * from its initializer still aliases the owner's storage
                     * once it holds an object, so its narrow static type must
                     * not exclude it from the owner's borrow closure. */
                    for (uint16_t a = 0; a < u->nargs; a++) {
                        if (u->args[a] == member) {
                            is_member_borrow = true;
                            break;
                        }
                    }
                } else if (xi_call_result_aliases_receiver(u)) {
                    /* A declared `return self` result IS the receiver, so the
                     * receiver must outlive it. Listed first because some of
                     * these members lower to XI_CALL_BUILTIN intrinsics
                     * (array_reserve / array_resize), which the method-call
                     * test below never sees. */
                    is_member_borrow = u->args[0] == member;
                } else if ((u->op == XI_CALL_METHOD || u->op == XI_CALL_METHOD_DIRECT) &&
                           xi_own_value_is_rc(u) && !call_returns_fresh(f, u)) {
                    /* A method whose RC result may alias its receiver — a getter
                     * like Map.get hands back a stored reference,
                     * not a fresh +1 — keeps the receiver (arg 0) live until the
                     * result's last use; otherwise releasing the receiver frees
                     * storage the borrowed result still points into. Proven
                     * fresh-returning callees return an independent +1 and do not
                     * pin the receiver. */
                    is_member_borrow = u->nargs >= 1 && u->args[0] == member;
                }
                if (!is_member_borrow)
                    continue;
                bool seen = false;
                for (uint32_t t = 0; t < ntracked; t++) {
                    if (tracked[t] == u) {
                        seen = true;
                        break;
                    }
                }
                if (seen)
                    continue;
                if (ntracked == track_cap) {
                    uint32_t new_cap = track_cap * 2;
                    XiValue **grown = (XiValue **) xr_realloc(tracked, new_cap * sizeof(XiValue *));
                    if (!grown) {
                        xr_free(tracked);
                        return NULL;
                    }
                    tracked = grown;
                    track_cap = new_cap;
                }
                tracked[ntracked++] = u;
            }
            /* Phi nodes are stored on blk->phis rather than blk->values[].
             * They still propagate borrowed raw pointers and Slice views. If
             * they are omitted here, the incoming projection is counted only
             * as an edge use and its owner can be dropped before the phi's
             * downstream uses (most visibly at a loop header).
             *
             * A non-dominated join is different: the phi merges independent
             * owners from mutually exclusive paths, so it is an ownership
             * transfer boundary for this incoming value. Extending one
             * branch-local owner through that phi would place its death drop
             * in a block where the owner was never defined. Only propagate
             * the borrow closure when the original owner dominates the phi;
             * the incoming edge remains a consuming transfer otherwise. */
            for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
                XiValue *u = &phi->value;
                if (target->block && !xi_dominates(target->block, blk))
                    continue;
                if (!arc_raw_pointer_borrow_flows_to_user(member, u) &&
                    !arc_span_view_borrow_flows_to_user(member, u))
                    continue;
                bool seen = false;
                for (uint32_t t = 0; t < ntracked; t++) {
                    if (tracked[t] == u) {
                        seen = true;
                        break;
                    }
                }
                if (seen)
                    continue;
                if (ntracked == track_cap) {
                    uint32_t new_cap = track_cap * 2;
                    XiValue **grown = (XiValue **) xr_realloc(tracked, new_cap * sizeof(XiValue *));
                    if (!grown) {
                        xr_free(tracked);
                        return NULL;
                    }
                    tracked = grown;
                    track_cap = new_cap;
                }
                tracked[ntracked++] = u;
            }
        }
    }
    *out_count = ntracked;
    return tracked;
}

/* Seed per-block use flags for the tracked set (owner + borrowed
 * projections). A projection consumed by a phi extends the owner's live
 * range to the predecessor's end; a block terminator use is a use at
 * end-of-block. */
static void arc_seed_uses(XiFunc *f, XiValue **tracked, uint32_t ntracked, ArcLive *live,
                          const uint32_t *pos_by_id) {
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        ArcLive *li = &live[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *u = blk->values[i];
            if (!u || u->op == XI_RETAIN || u->op == XI_RELEASE)
                continue;
            bool uses_tracked = false;
            for (uint16_t a = 0; a < u->nargs && !uses_tracked; a++) {
                for (uint32_t t = 0; t < ntracked; t++) {
                    if (u->args[a] == tracked[t]) {
                        uses_tracked = true;
                        break;
                    }
                }
            }
            if (uses_tracked) {
                li->has_use = 1;
                li->last_use = i; /* ascending i: ends at the latest use */
            }
        }
        /* Phi consumes of a projection happen on the incoming edge: treat
         * as a use at the end of the predecessor block. */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                XiBlock *pred = (a < blk->npreds) ? blk->preds[a] : NULL;
                if (!pred || !pos_by_id[pred->id])
                    continue;
                for (uint32_t t = 0; t < ntracked; t++) {
                    if (phi->value.args[a] == tracked[t]) {
                        ArcLive *pl = &live[pos_by_id[pred->id] - 1];
                        pl->has_use = 1;
                        pl->use_at_end = 1;
                        break;
                    }
                }
            }
        }
        /* Only RETURN and IF read block->control as a terminator operand.
         * UNREACHABLE blocks retain the thrown value there as diagnostic
         * metadata, but XI_THROW already performs the sole consuming use.
         * Counting both forces a spurious retain before rethrow and leaks one
         * reference on every exceptional exit. */
        if (blk->kind == XI_BLOCK_RETURN || blk->kind == XI_BLOCK_IF) {
            for (uint32_t t = 0; t < ntracked; t++) {
                if (blk->control == tracked[t]) {
                    li->has_use = 1;
                    li->use_at_end = 1;
                    break;
                }
            }
        }
    }
}

/* A phi consumes its incoming owner on the predecessor edge. Keep this query
 * independent of liveness bookkeeping so release placement can fail closed if
 * either representation drifts. */
static bool arc_edge_forwards_target_to_phi(const XiBlock *pred, const XiBlock *succ,
                                            const XiValue *target) {
    if (!pred || !succ || !target)
        return false;
    for (const XiPhi *phi = succ->phis; phi; phi = phi->next) {
        for (uint16_t a = 0; a < phi->value.nargs && a < succ->npreds; a++) {
            if (succ->preds[a] == pred && phi->value.args[a] == target)
                return true;
        }
    }
    return false;
}

static bool arc_edge_forwards_target_to_self_phi(const XiBlock *pred, const XiBlock *succ,
                                                 const XiValue *target) {
    if (!pred || !succ || !target)
        return false;
    for (const XiPhi *phi = succ->phis; phi; phi = phi->next) {
        if (&phi->value != target)
            continue;
        for (uint16_t a = 0; a < phi->value.nargs && a < succ->npreds; a++) {
            if (succ->preds[a] == pred && phi->value.args[a] == target)
                return true;
        }
    }
    return false;
}

static bool arc_block_forwards_target_to_distinct_phi(const XiBlock *block, const XiValue *target) {
    if (!block || !target)
        return false;
    for (unsigned s = 0; s < 2; s++) {
        const XiBlock *successor = block->succs[s];
        if (successor && arc_edge_forwards_target_to_phi(block, successor, target) &&
            !arc_edge_forwards_target_to_self_phi(block, successor, target))
            return true;
    }
    return false;
}

/* Place releases on the live→dead frontier for `target`. Returns true if a
 * CFG edge was split (caller must invalidate analyses). */
static bool arc_place_frontier_drops(XiFunc *f, XiValue *target, const ArcLive *live,
                                     const uint32_t *pos_by_id, const XiBlock *def_blk,
                                     bool frame_pinned) {
    bool split_any = false;
    uint32_t def_pos = pos_by_id[def_blk->id] ? pos_by_id[def_blk->id] - 1 : 0;
    /* Snapshot the block count. arc_split_edge() below appends fresh
     * edge-split blocks (each fully handled at creation), which have no
     * entry in live[] or pos_by_id[] — both were sized to the pre-split
     * CFG. Iterating the live f->nblocks would revisit those blocks and
     * read past the end of live[]. */
    uint32_t nblocks = f->nblocks;
    for (uint32_t b = 0; b < nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        const ArcLive *li = &live[b];
        bool holds_value = li->live_in || b == def_pos;
        if (!holds_value)
            continue;

        if (!li->live_out) {
            /* A regular consuming MOVE executes before the terminator and
             * transfers the owner on every outgoing path. */
            if (li->moved_in_block)
                continue;
            /* A phi MOVE transfers only on its particular outgoing edge. The
             * owner must still be dropped on sibling edges which neither
             * transfer nor keep it live. */
            bool forwarded_to_phi = false;
            for (int s = 0; s < 2 && !forwarded_to_phi; s++)
                forwarded_to_phi = arc_edge_forwards_target_to_phi(blk, blk->succs[s], target);
            if (li->moved_on_phi_edge) {
                for (int s = 0; s < 2; s++) {
                    XiBlock *succ = blk->succs[s];
                    if (!succ || arc_edge_forwards_target_to_phi(blk, succ, target))
                        continue;
                    if (succ->npreds == 1) {
                        insert_drop_at_head(f, succ, target);
                    } else {
                        XiBlock *mid = arc_split_edge(f, blk, succ);
                        if (mid) {
                            insert_drop_after(f, mid, NULL, target);
                            split_any = true;
                        }
                    }
                }
                continue;
            }
            if (forwarded_to_phi) {
                /* A frame-pinned owner is duplicated for a distinct PHI: the
                 * PHI receives the retained reference while the old frame
                 * owner dies on that exact edge. A self-PHI loop edge instead
                 * carries the same frame owner into its next iteration and
                 * must not release it. Keep the disposition edge-specific so
                 * a branch can contain both shapes without over-releasing the
                 * self edge or leaking the distinct transfer. */
                for (int s = 0; s < 2; s++) {
                    XiBlock *succ = blk->succs[s];
                    if (!succ || arc_edge_forwards_target_to_self_phi(blk, succ, target))
                        continue;
                    if (succ->npreds == 1) {
                        insert_drop_at_head(f, succ, target);
                    } else {
                        XiBlock *mid = arc_split_edge(f, blk, succ);
                        if (mid) {
                            insert_drop_after(f, mid, NULL, target);
                            split_any = true;
                        }
                    }
                }
                continue;
            }
            if (li->use_at_end && blk->control == target && blk->kind != XI_BLOCK_RETURN) {
                /* A branch control is read by the terminator after every
                 * ordinary value in the block.  Inserting a death-drop after
                 * the last ordinary value therefore releases it before that
                 * final read.  End the owner on each outgoing edge instead;
                 * this also prevents if-conversion from hoisting the drop in
                 * front of the SELECT that replaces the branch. */
                for (int s = 0; s < 2; s++) {
                    XiBlock *succ = blk->succs[s];
                    if (!succ)
                        continue;
                    if (succ->npreds == 1) {
                        insert_drop_at_head(f, succ, target);
                    } else {
                        XiBlock *mid = arc_split_edge(f, blk, succ);
                        if (mid) {
                            insert_drop_after(f, mid, NULL, target);
                            split_any = true;
                        }
                    }
                }
                continue;
            }
            /* Death in block: release after the last use (after the def
             * itself when the block never uses the value). */
            XiValue *anchor;
            if (li->has_use)
                anchor = li->use_at_end ? (blk->nvalues > 0 ? blk->values[blk->nvalues - 1] : NULL)
                                        : blk->values[li->last_use];
            else if (b == def_pos && target->op != XI_PARAM)
                anchor = target;
            else {
                /* Unused owned parameter: dead on entry. */
                insert_drop_at_head(f, blk, target);
                continue;
            }
            insert_drop_after(f, blk, anchor, target);
            continue;
        }

        /* Death on edge: live out of this block, but a successor is dead. */
        for (int s = 0; s < 2; s++) {
            XiBlock *sb = blk->succs[s];
            if (!sb || !pos_by_id[sb->id])
                continue;
            if (arc_edge_forwards_target_to_phi(blk, sb, target)) {
                /* A frame-pinned distinct PHI receives a retained reference,
                 * so the old frame-slot owner still dies on this edge.  The
                 * predecessor can remain live-out because a sibling path
                 * keeps using the old slot; that must not suppress the
                 * selected edge's release.  A self-PHI carries the same slot
                 * into the next iteration and therefore keeps its owner. */
                if (frame_pinned && !arc_edge_forwards_target_to_self_phi(blk, sb, target) &&
                    !live[pos_by_id[sb->id] - 1].live_in) {
                    if (sb->npreds == 1) {
                        insert_drop_at_head(f, sb, target);
                    } else {
                        XiBlock *mid = arc_split_edge(f, blk, sb);
                        if (mid) {
                            insert_drop_after(f, mid, NULL, target);
                            split_any = true;
                        }
                    }
                }
                continue;
            }
            if (live[pos_by_id[sb->id] - 1].live_in)
                continue;
            if (sb->npreds == 1) {
                insert_drop_at_head(f, sb, target);
            } else {
                XiBlock *mid = arc_split_edge(f, blk, sb);
                if (mid) {
                    insert_drop_after(f, mid, NULL, target);
                    split_any = true;
                }
            }
        }
    }
    return split_any;
}

/* Compute backward liveness of `target` (plus its RC-typed borrowed-projection
 * closure) over the CFG. On success allocates *out_live (one ArcLive per block
 * position) and *out_pos_by_id (block-id -> position+1, 0 = absent); the caller
 * frees both. The def block's live_in is forced false (the definition kills
 * everything above it), bounding the live region inside the def's dominance
 * region. Returns false on OOM. */
static bool arc_compute_liveness(XiFunc *f, XiValue *target, ArcLive **out_live,
                                 uint32_t **out_pos_by_id) {
    XiBlock *def_blk = target->block ? target->block : f->entry;
    if (!def_blk)
        return false;

    /* Dense block-id → position map (ids survive block compaction, so the
     * id space can be larger than nblocks). */
    uint32_t max_id = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        if (f->blocks[b] && f->blocks[b]->id > max_id)
            max_id = f->blocks[b]->id;
    }
    uint32_t *pos_by_id = (uint32_t *) xr_calloc(max_id + 1, sizeof(uint32_t));
    ArcLive *live = (ArcLive *) xr_calloc(f->nblocks, sizeof(ArcLive));
    if (!pos_by_id || !live) {
        if (pos_by_id)
            xr_free(pos_by_id);
        if (live)
            xr_free(live);
        return false;
    }
    for (uint32_t b = 0; b < f->nblocks; b++) {
        if (f->blocks[b])
            pos_by_id[f->blocks[b]->id] = b + 1; /* 0 = absent */
    }

    uint32_t ntracked = 0;
    XiValue **tracked = arc_collect_borrow_closure(f, target, &ntracked);
    if (!tracked) {
        /* OOM: skip placement for this value (leak, never a wrong drop). */
        xr_free(pos_by_id);
        xr_free(live);
        return false;
    }
    arc_seed_uses(f, tracked, ntracked, live, pos_by_id);
    arc_seed_user_try_cleanup_uses(f, target, live);
    xr_free(tracked);

    /* Backward liveness to fixpoint. The def block's live_in is forced
     * false: the definition kills everything above it, which both bounds
     * the live region and makes per-iteration loop deaths land inside the
     * loop body instead of leaking across the back edge. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t b = f->nblocks; b-- > 0;) {
            XiBlock *blk = f->blocks[b];
            if (!blk)
                continue;
            bool out = false;
            for (int s = 0; s < 2 && !out; s++) {
                XiBlock *sb = blk->succs[s];
                if (sb && pos_by_id[sb->id])
                    out = live[pos_by_id[sb->id] - 1].live_in;
            }
            bool in = (blk == def_blk) ? false : (live[b].has_use || out);
            if (out != (bool) live[b].live_out || in != (bool) live[b].live_in) {
                live[b].live_out = out;
                live[b].live_in = in;
                changed = true;
            }
        }
    }

    *out_live = live;
    *out_pos_by_id = pos_by_id;
    return true;
}

/* Place releases for an owned value with no consuming use. Returns true if
 * an edge was split (CFG analyses must be invalidated by the caller). */
static bool insert_drops_at_death(XiFunc *f, XiValue *target, bool frame_pinned) {
    XiBlock *def_blk = target->block ? target->block : f->entry;
    if (!def_blk)
        return false;
    ArcLive *live = NULL;
    uint32_t *pos_by_id = NULL;
    if (!arc_compute_liveness(f, target, &live, &pos_by_id))
        return false;
    bool split_any = arc_place_frontier_drops(f, target, live, pos_by_id, def_blk, frame_pinned);
    xr_free(pos_by_id);
    xr_free(live);
    return split_any;
}

/* Is `target` still live immediately after the consume at `site`? A consuming
 * use is a MOVE (no dup) only when the value is dead right after it on every
 * path; otherwise the owner still needs a reference afterwards (a later use in
 * the same block, or a use in a successor) so the consume must dup first.
 *
 * Deciding by liveness rather than static program order is what makes mutually
 * exclusive branch arms each MOVE: a consume in one arm is not "live after"
 * just because the other arm also consumes — the arms are not on a common path.
 * `live`/`pos_by_id` come from arc_compute_liveness (uses = consume + borrow
 * closure), so "live after" correctly accounts for borrows-after-consume too. */
static bool arc_phi_edge_owner_live_after(XiFunc *f, XiValue *target, const ConsumeSite *site) {
    if (!f || !target || !site || !site->blk || !site->user || site->user->op != XI_PHI ||
        !site->user->block)
        return true;
    uint32_t tracked_count = 0;
    XiValue **tracked = arc_collect_borrow_closure(f, target, &tracked_count);
    uint32_t max_block_id = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        if (f->blocks[b] && f->blocks[b]->id > max_block_id)
            max_block_id = f->blocks[b]->id;
    }
    uint8_t *reachable = (uint8_t *) xr_calloc((size_t) max_block_id + 1u, sizeof(*reachable));
    XiBlock **queue = (XiBlock **) xr_malloc((size_t) f->nblocks * sizeof(*queue));
    if (!tracked || !reachable || !queue) {
        xr_free(tracked);
        xr_free(reachable);
        xr_free(queue);
        return true;
    }
    uint32_t head = 0, tail = 0;
    reachable[site->user->block->id] = 1;
    queue[tail++] = site->user->block;
    bool found = false;
    while (head < tail && !found) {
        XiBlock *block = queue[head++];
        if (!block)
            continue;
        /* Re-entering the target's definition block starts the next dynamic
         * loop instance. Uses beyond that PHI/definition belong to the next
         * token, not to the owner consumed on the current edge. */
        if (block == target->block)
            continue;
        for (uint32_t i = 0; i < block->nvalues && !found; i++) {
            XiValue *user = block->values[i];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs && !found; a++) {
                for (uint32_t t = 0; t < tracked_count; t++) {
                    XiValue *member = tracked[t];
                    if (member != target &&
                        (!member->block || !xi_dominates(member->block, site->blk)))
                        continue;
                    if (user->args[a] == member)
                        found = true;
                }
            }
        }
        for (uint32_t t = 0; t < tracked_count && !found; t++) {
            XiValue *member = tracked[t];
            if (member != target && (!member->block || !xi_dominates(member->block, site->blk)))
                continue;
            if (block->control == member)
                found = true;
        }
        for (unsigned s = 0; s < 2; s++) {
            XiBlock *successor = block->succs[s];
            if (!successor || successor->id > max_block_id || reachable[successor->id])
                continue;
            /* A PHI reads only the operand on the edge that actually reaches
             * it. Scanning every PHI operand after merely reaching its block
             * confuses mutually exclusive paths and, in loops, treats a
             * future iteration's incoming token as a use of the current one.
             * Inspect the operand for block->successor before enqueueing that
             * exact edge instead. Re-entering the target definition starts a
             * new dynamic token and is deliberately excluded. */
            if (successor != target->block) {
                for (XiPhi *phi = successor->phis; phi && !found; phi = phi->next) {
                    for (uint16_t a = 0; a < phi->value.nargs && a < successor->npreds && !found;
                         a++) {
                        if (successor->preds[a] != block)
                            continue;
                        for (uint32_t t = 0; t < tracked_count; t++) {
                            XiValue *member = tracked[t];
                            if (member != target &&
                                (!member->block || !xi_dominates(member->block, site->blk)))
                                continue;
                            if (phi->value.args[a] == member) {
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (found)
                break;
            reachable[successor->id] = 1;
            queue[tail++] = successor;
        }
    }
    xr_free(tracked);
    xr_free(reachable);
    xr_free(queue);
    return found;
}

static bool consume_is_live_after(XiFunc *f, XiValue *target, const ConsumeSite *site,
                                  const ArcLive *live, const uint32_t *pos_by_id) {
    if (!site->blk || !pos_by_id[site->blk->id])
        return true; /* unknown: conservatively dup (never a wrong move) */
    const ArcLive *li = &live[pos_by_id[site->blk->id] - 1];
    uint32_t idx = site->order & 0xFFFF;
    /* Phi-edge (0xFFFE) and return (0xFFFF) consumes sit at block end: the
     * value is only still live afterwards if a successor uses it. */
    if (site->user && site->user->op == XI_PHI)
        return arc_phi_edge_owner_live_after(f, target, site);
    if (idx >= 0xFFFE)
        return li->live_out;
    /* Regular consume at index idx: live after iff a later ordinary use exists
     * in this block, the value is consumed by a phi/return at block end, or it
     * escapes into a successor. Phi inputs are edge uses recorded with
     * use_at_end rather than a blk->values[] index, so omitting that flag makes
     * `consume(v); phi [this_block:v]` wrongly move v into the first consume. */
    return li->use_at_end || (li->has_use && li->last_use > idx) || li->live_out;
}

/* Process one tracked value: insert dup before every consuming use except
 * the last, and a drop if it is never consumed.
 *
 * `mode` selects the ownership convention:
 *   OWN_OWNED   — the function holds an owning reference (a fresh local
 *                 allocation, or an owned parameter). The last consuming use
 *                 moves it out (no dup); earlier consuming uses dup first; if
 *                 never consumed it is dropped at function exit.
 *   OWN_BORROWED — the value is owned by someone else (a method's `this`
 *                 receiver, operator operands, a captured-by-value the caller
 *                 keeps). EVERY consuming use dups first; never dropped.
 *   OWN_CALL_RESULT — a call result whose ownership we cannot statically
 *                 prove: it may be a fresh (+1) reference or an alias of an
 *                 argument (e.g. Map.get returns stored data). UNKNOWN uses
 *                 the fail-closed borrowed convention: EVERY consuming use
 *                 dups first, and no unconsumed drop is inserted. Skipping a
 *                 final retain would transfer a reference the function never
 *                 owned and let the consumer free storage still held by the
 *                 container. This can leak a fresh result; per-callee return
 *                 summaries refine that conservative case. */
typedef enum {
    OWN_OWNED = 0,
    OWN_BORROWED,
    OWN_CALL_RESULT,
} XiArcOwnMode;

/* ========== Coroutine-frame pinning ==========
 *
 * A value that occupies a persistent coroutine frame slot must keep its
 * owning reference in that slot until an explicit RELEASE gives it up: the
 * AOT backend releases every listed slot when the frame is torn down — at
 * normal completion, recycle, and cancellation alike — and its lowering of
 * an explicit frame-slot RELEASE also nulls the slot. A consuming MOVE out
 * of such a slot would leave a stale owner behind that the frame teardown
 * releases a second time (double free of e.g. a closure capture cell that
 * two spawned closures share). Forcing every value-level consume of a
 * frame-pinned value to dup preserves the slot's reference; the death-drop
 * pass then places the explicit RELEASE at the value's death point. Return
 * consumes are exempt: the coroutine done-value path retains the returned
 * value separately to compensate for the slot release.
 *
 * The suspend-point scan runs without a resolver, which keeps every
 * MAY_SUSPEND-flagged call a suspend point. That over-approximates the
 * backend plan — its resolver can only refine suspend points away — so
 * every value the backend actually places in a frame slot is pinned here;
 * an over-approximated value merely carries one extra dup/drop pair. */
static XiLiveness *arc_coro_frame_pin_liveness(XiFunc *f) {
    bool suspends = false;
    for (uint32_t b = 0; b < f->nblocks && !suspends; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; blk && i < blk->nvalues; i++) {
            if (xi_coro_is_suspend_point(f, blk->values[i], NULL)) {
                suspends = true;
                break;
            }
        }
    }
    return suspends ? xi_compute_liveness(f) : NULL;
}

static bool arc_value_is_coro_frame_pinned(const XiFunc *f, const XiLiveness *coro_live,
                                           const XiValue *target) {
    return coro_live && xi_coro_value_is_logical_member(f, target, coro_live, NULL);
}

/* A return terminator in a function whose own return ownership is a PROVEN
 * borrow forwards that borrow; it does not transfer a reference. Retaining
 * there raises the count by one that nobody releases: the callee's published
 * signature tells every caller the result is +0, so no caller drops it.
 *
 * `fn w(t: string) -> string { return f(t) }` where `f` is BORROWED_PARAM(0)
 * is the minimal shape — w is BORROWED_PARAM(0) too, yet the result of `f(t)`
 * is OWN_CALL_RESULT (not OWNED, so not statically fresh) and every consume of
 * an OWN_CALL_RESULT/OWN_BORROWED value dups. The dup landed on the return.
 *
 * Only a `complete` borrow signature relaxes this. UNKNOWN keeps the retain,
 * whose failure direction is a leaked count rather than a use-after-free. */
static bool arc_return_forwards_borrow(const XiFunc *f) {
    return f && f->arc_return_ownership.complete &&
           (f->arc_return_ownership.kind == XI_RETURN_OWNERSHIP_BORROWED_PARAM ||
            f->arc_return_ownership.kind == XI_RETURN_OWNERSHIP_BORROWED_STATIC);
}

static bool insert_dup_at_consume_site(XiFunc *f, XiValue *target, ConsumeSiteVec *sites,
                                       uint32_t site_index) {
    XR_DCHECK(sites != NULL && site_index < sites->count,
              "insert_dup_at_consume_site: invalid site");
    ConsumeSite *site = &sites->items[site_index];
    if (!site->user && arc_return_forwards_borrow(f))
        return false;
    if (!site->user || site->user->op == XI_PHI) {
        XiBlock *placement = site->blk;
        bool split = false;
        if (site->user && site->user->op == XI_PHI && site->user->block && site->blk->succs[0] &&
            site->blk->succs[1]) {
            XiBlock *original_pred = site->blk;
            XiBlock *join = site->user->block;
            bool has_direct_edge =
                original_pred->succs[0] == join || original_pred->succs[1] == join;
            if (has_direct_edge) {
                placement = arc_split_edge(f, original_pred, join);
                split = placement != NULL;
            } else if (site->phi_pred_index < join->npreds) {
                /* Death-frontier placement can split this edge after consume
                 * sites have been collected but before their retains are
                 * inserted. xi_cfg_replace_pred preserves the PHI predecessor
                 * slot, so recover the exact edge block from that stable slot. */
                XiBlock *candidate = join->preds[site->phi_pred_index];
                bool candidate_from_original = false;
                for (uint16_t p = 0; candidate && p < candidate->npreds; p++) {
                    if (candidate->preds[p] == original_pred) {
                        candidate_from_original = true;
                        break;
                    }
                }
                bool candidate_targets_join =
                    candidate && (candidate->succs[0] == join || candidate->succs[1] == join);
                if (candidate_from_original && candidate_targets_join)
                    placement = candidate;
            }
            XR_DCHECK(placement != original_pred,
                      "xi_arc: PHI edge no longer has a unique split placement");
            if (placement != original_pred) {
                site->blk = placement;
                /* Several PHIs can consume the same owner on one CFG edge.
                 * Their sites were collected before this split, so retarget
                 * the remaining members of that exact edge group to the
                 * single edge block. Each consume still gets its own retain,
                 * but the logical edge is split only once. */
                for (uint32_t i = site_index + 1; i < sites->count; i++) {
                    ConsumeSite *later = &sites->items[i];
                    if (later->blk == original_pred && later->user && later->user->op == XI_PHI &&
                        later->user->block == join && later->phi_pred_index == site->phi_pred_index)
                        later->blk = placement;
                }
            }
        }
        XiValue *last = phi_dup_anchor(placement);
        insert_dup_after(f, placement, last, target);
        return split;
    }
    insert_dup_before(f, site->blk, site->user, target);
    return false;
}

static bool process_call_result_consumes(XiFunc *f, XiValue *target, ConsumeSiteVec *sites) {
    /* UNKNOWN is fail-closed as a +0 borrow. A last consuming use transfers an
     * owning reference to its consumer, so it needs a retain just as much as
     * every earlier consume. Liveness decides only how many independent
     * owners are needed; it cannot prove that the original result was +1. */
    bool split_any = false;
    for (uint32_t i = 0; i < sites->count; i++)
        split_any |= insert_dup_at_consume_site(f, target, sites, i);
    return split_any;
}

static bool process_value_ex(XiFunc *f, XiValue *target, XiArcOwnMode mode,
                             const XiLiveness *coro_live) {
    ConsumeSiteVec sites = {0};
    if (!collect_consume_sites(f, target, &sites)) {
        xr_free(sites.items);
        XR_CHECK(false, "xi_arc: out of memory collecting consume sites");
        return false;
    }

    bool frame_pinned = arc_value_is_coro_frame_pinned(f, coro_live, target);

    if (sites.count == 0) {
        /* Never consumed. Only an OWNED value is dropped (at its death
         * point). A borrowed value is owned by the caller; a call result
         * may be an alias — in both cases we must not drop it here. */
        bool split_any = mode == OWN_OWNED ? insert_drops_at_death(f, target, frame_pinned) : false;
        xr_free(sites.items);
        return split_any;
    }

    qsort(sites.items, (size_t) sites.count, sizeof(ConsumeSite), cmp_site);

    if (mode == OWN_BORROWED) {
        /* Borrowed: the function owns no reference, so every consuming use
         * must dup first; nothing is moved out and nothing is dropped. */
        bool split_any = false;
        for (uint32_t i = 0; i < sites.count; i++)
            split_any |= insert_dup_at_consume_site(f, target, &sites, i);
        xr_free(sites.items);
        return split_any;
    }

    if (mode == OWN_CALL_RESULT) {
        bool split_any = process_call_result_consumes(f, target, &sites);
        xr_free(sites.items);
        return split_any;
    }

    /* OWNED: a consuming use moves the owned reference out only when the value
     * is dead immediately after it (last use on every path through it);
     * otherwise the owner still needs a reference afterwards, so dup first.
     * The decision is by liveness, not static program order, so two mutually
     * exclusive branch arms that each consume the value both MOVE — the old
     * "dup all but the last in program order" wrongly dup'd the earlier arm,
     * leaking that reference whenever that arm ran. Paths that reach the
     * value's death WITHOUT consuming it (a branch arm that only borrows it)
     * get a death-drop, so the owner is released exactly once on every path. */
    ArcLive *live = NULL;
    uint32_t *pos_by_id = NULL;
    if (!arc_compute_liveness(f, target, &live, &pos_by_id)) {
        /* OOM: safe count-raising fallback — dup all but the last consume in
         * program order, no death-drops. Never a wrong move or double free.
         * A frame-pinned value dups at the last value consume too: no
         * death-drop exists here, so its slot reference is the one the frame
         * teardown release list gives up. */
        bool split_any = false;
        for (uint32_t i = 0; i < sites.count; i++) {
            if (!frame_pinned && i + 1 == sites.count)
                break;
            if (sites.items[i].user == NULL)
                continue;
            split_any |= insert_dup_at_consume_site(f, target, &sites, i);
        }
        xr_free(sites.items);
        return split_any;
    }

    /* Decide move vs dup per site and record whether the MOVE happens in the
     * block or on a particular phi edge. Record decisions before insertion:
     * the drop pass indexes blocks by their pre-dup positions, and dups are
     * pointer-anchored so they are inserted afterwards. */
    bool *moves = (bool *) xr_calloc(sites.count, sizeof(bool));
    if (!moves) {
        xr_free(pos_by_id);
        xr_free(live);
        xr_free(sites.items);
        XR_CHECK(false, "xi_arc: out of memory recording consume moves");
        return false;
    }
    for (uint32_t i = 0; i < sites.count; i++) {
        moves[i] = !consume_is_live_after(f, target, &sites.items[i], live, pos_by_id) &&
                   !(frame_pinned && sites.items[i].user != NULL);
        if (moves[i] && sites.items[i].blk && pos_by_id[sites.items[i].blk->id]) {
            ArcLive *site_live = &live[pos_by_id[sites.items[i].blk->id] - 1];
            if (sites.items[i].user && sites.items[i].user->op == XI_PHI)
                site_live->moved_on_phi_edge = 1;
            else
                site_live->moved_in_block = 1;
        }
    }

    /* Place death-drops where the owner dies without a move (a borrow-only
     * branch arm, or a path that never touches it). */
    XiBlock *def_blk = target->block ? target->block : f->entry;
    bool split_any =
        def_blk && arc_place_frontier_drops(f, target, live, pos_by_id, def_blk, frame_pinned);

    /* Insert the dups last (pointer-anchored, so the drop insertions above that
     * shifted block indices do not matter). */
    for (uint32_t i = 0; i < sites.count; i++) {
        if (moves[i])
            continue; /* MOVE: the consume transfers the owned reference */
        if (sites.items[i].user == NULL)
            continue; /* return terminator: last use, never live after */
        split_any |= insert_dup_at_consume_site(f, target, &sites, i);
    }

    xr_free(moves);
    xr_free(pos_by_id);
    xr_free(live);
    xr_free(sites.items);
    return split_any;
}

/* Does the borrow signature prove parameter `pv` is only borrowed (never
 * stored/returned/forwarded)? Maps the param SSA value to its index via
 * f->params[], then reads sig->param_own[idx]. The signature is computed by
 * xi_own from real liveness + consume classification. Returns false when out
 * of range or unproven (conservative: treat as owned). */
static bool param_is_borrowed(const XiFunc *f, const XiValue *pv, const XiBorrowSig *sig) {
    if (!sig || !sig->valid)
        return false;
    for (uint16_t p = 0; p < xi_func_semantic_param_count(f) && p < sig->nparams; p++) {
        if (f->params[p] == pv)
            return sig->param_own[p] == XI_OWN_BORROWED;
    }
    return false;
}

/* Does parameter `p` of `f` have a consuming use, accounting for callee borrow
 * signatures? Mirrors collect_consume_sites' consume detection (owned store /
 * return / phi / consuming call argument) but skips call arguments the callee
 * only borrows — those keep the parameter live in the caller instead of moving
 * it in. Reads callee signatures from their cached (fixpoint-state) sig, so it
 * is the per-iteration transfer function of the borrow fixpoint. */
static bool param_has_consuming_use(XiFunc *f, XiValue *p) {
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *u = blk->values[i];
            if (!u || u->op == XI_RETAIN || u->op == XI_RELEASE)
                continue;
            for (uint16_t a = 0; a < u->nargs; a++) {
                if (u->args[a] != p || !xi_own_value_arg_is_consuming(u, a))
                    continue;
                if (u->op == XI_STACK_ALLOC &&
                    stack_alloc_closure_uses_are_scoped_par_for_callbacks(f, u))
                    continue;
                if (arc_call_arg_is_callee_borrowed(f, u, a))
                    continue; /* callee borrows this arg → not a consume */
                return true;
            }
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == p)
                    return true; /* phi merge consumes its incoming value */
            }
        }
        if (blk->control == p && blk->kind == XI_BLOCK_RETURN && !arc_function_returns_borrow(f))
            return true;
    }
    return false;
}

/* Initialize every reachable function's borrow signature OPTIMISTICALLY (all
 * RC parameters borrowed) and collect the functions for the fixpoint. The
 * cached pointer doubles as the visited marker, so call-graph cycles
 * terminate. The optimistic seed is what lets mutually recursive functions
 * converge to the greatest borrow set. */
static void arc_init_sigs_collect(XiFunc *f, XiFuncVec *vec) {
    if (!f || f->arc_borrow_sig)
        return;
    XiBorrowSig *sig = (XiBorrowSig *) xi_func_arena_alloc(f, sizeof(XiBorrowSig));
    if (!sig)
        return; /* OOM: leave NULL; arc_get_borrow_sig falls back per-function */
    memset(sig, 0, sizeof(*sig));
    uint16_t semantic_params = xi_func_semantic_param_count(f);
    uint16_t n = semantic_params > XI_OWN_MAX_PARAMS ? XI_OWN_MAX_PARAMS : semantic_params;
    sig->nparams = (uint8_t) n;
    sig->valid = true;
    for (uint16_t p = 0; p < n; p++) {
        XiValue *pv = f->params[p];
        sig->param_own[p] = (pv && xi_own_value_is_rc(pv)) ? XI_OWN_BORROWED : XI_OWN_NONE;
    }
    f->arc_borrow_sig = sig;
    if (!xi_func_vec_push(vec, f))
        return;
    for (uint16_t i = 0; i < f->nchildren; i++)
        arc_init_sigs_collect(f->children[i], vec);
    for (uint16_t i = 0; i < f->shared_slot_func_count; i++)
        arc_init_sigs_collect(f->shared_slot_funcs[i], vec);
}

static void arc_mark_closure_return_abis(XiFunc *function) {
    if (!function)
        return;
    for (uint16_t i = 0; i < function->nchildren; i++)
        arc_mark_closure_return_abis(function->children[i]);
    for (uint32_t b = 0; b < function->nblocks; b++) {
        XiBlock *block = function->blocks[b];
        for (uint32_t v = 0; block && v < block->nvalues; v++) {
            XiValue *closure = block->values[v];
            if (!closure || closure->op != XI_CLOSURE_NEW || !closure->aux)
                continue;
            XiFunc *body = (XiFunc *) closure->aux;
            if (body->return_type && xi_own_function_return_is_rc(body))
                body->requires_owned_indirect_return = true;
        }
    }
}

/* Compute every reachable function's borrow signature to a fixpoint on the
 * PRE-ARC IR (Roc crate::borrow model). Each parameter starts borrowed and is
 * demoted to owned only when proven to have a consuming use given the current
 * callee signatures; iterating to stability resolves transitive forwarding
 * (a parameter passed only to borrowing callees stays borrowed) and mutual
 * recursion. Demotion is monotone, so the loop terminates.
 *
 * This refines the per-function seed (xi_own's intraprocedural signature) with
 * cross-function information. It is correctness-safe either way — a borrowed
 * parameter dups at each consume, an owned one moves — so a less-precise result
 * only costs extra dup/drop, never RC balance. Signatures must be final before
 * any RETAIN/RELEASE is inserted, since callers read callee signatures. */
XR_FUNC void xi_arc_analyze_contracts(XiFunc *f) {
    /* OWNER_FORWARD is a semantic ownership relation, not a physical RC
     * instruction. Normalize escaping borrow-copies before computing any
     * signature so every consumer, including a no-ARC pipeline, sees the same
     * contract graph. */
    arc_copy_to_move(f);
    arc_mark_closure_return_abis(f);
    XiFuncVec vec = {0};
    arc_init_sigs_collect(f, &vec);
    uint8_t *fixed_return = (uint8_t *) xr_calloc(vec.count, sizeof(*fixed_return));
    if (vec.count > 0 && !fixed_return) {
        xr_free(vec.items);
        XR_CHECK(false, "xi_arc: out of memory sealing return contracts");
        return;
    }
    for (uint32_t i = 0; i < vec.count; i++) {
        XiFunc *fn = vec.items[i];
        if (fn->requires_owned_indirect_return) {
            fn->arc_return_ownership = arc_return_ownership(XI_RETURN_OWNERSHIP_OWNED, -1, true);
            fixed_return[i] = 1u;
        } else {
            fixed_return[i] = fn->arc_return_ownership.complete ? 1u : 0u;
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < vec.count; i++) {
            XiFunc *fn = vec.items[i];
            XiBorrowSig *sig = fn->arc_borrow_sig;
            if (!sig || !sig->valid)
                continue;
            for (uint16_t p = 0; p < sig->nparams; p++) {
                if (sig->param_own[p] != XI_OWN_BORROWED)
                    continue;
                if (p < xi_func_semantic_param_count(fn) && fn->params[p] &&
                    param_has_consuming_use(fn, fn->params[p])) {
                    sig->param_own[p] = XI_OWN_OWNED;
                    changed = true;
                }
            }
        }
    }

    /* Return summaries use the same reachable-function set and pre-ARC IR.
     * Seed local RC-returning functions with the only optimistic result that
     * cannot manufacture an alias (OWNED), then repeatedly evaluate the
     * complete equations. A recursive factory remains OWNED only when every
     * grounded return and dependency agrees. Mixed/foreign provenance drives
     * the SCC to UNKNOWN; a non-converging equation set is reset fail-closed. */
    for (uint32_t i = 0; i < vec.count; i++) {
        XiFunc *fn = vec.items[i];
        if (fixed_return[i])
            continue;
        fn->arc_return_ownership = xi_own_function_return_is_rc(fn)
                                       ? arc_return_ownership(XI_RETURN_OWNERSHIP_OWNED, -1, true)
                                       : arc_return_unknown();
    }
    changed = true;
    uint32_t rounds = 0;
    uint32_t max_rounds = vec.count > 0 ? vec.count * 4u + 1u : 1u;
    while (changed && rounds++ < max_rounds) {
        changed = false;
        for (uint32_t i = 0; i < vec.count; i++) {
            if (fixed_return[i])
                continue;
            XiFunc *fn = vec.items[i];
            XiReturnOwnership next = arc_infer_return_ownership(fn);
            if (arc_return_ownership_equal(fn->arc_return_ownership, next))
                continue;
            fn->arc_return_ownership = next;
            changed = true;
        }
    }
    if (changed) {
        for (uint32_t i = 0; i < vec.count; i++) {
            if (!fixed_return[i])
                vec.items[i]->arc_return_ownership = arc_return_unknown();
        }
    }

    /* Every source-defined function must publish an exact reference-return
     * contract before caller ARC placement.  A return whose provenance could
     * not be proven as a stable borrow uses the owned ABI: borrowed and
     * unknown values are retained at the return edge, while an existing local
     * owner is moved.  Leaving that contract UNKNOWN makes both sides retain
     * defensively; after inlining the two increments survive under one SSA
     * name and leak. */
    for (uint32_t i = 0; i < vec.count; i++) {
        XiFunc *fn = vec.items[i];
        if (!fn || fixed_return[i] || fn->is_extern || !xi_own_function_return_is_rc(fn) ||
            fn->arc_return_ownership.complete)
            continue;
        fn->arc_return_ownership = arc_return_ownership(XI_RETURN_OWNERSHIP_OWNED, -1, true);
    }
    xr_free(fixed_return);
    xr_free(vec.items);
}

static XiArcOwnMode arc_target_own_mode(const XiFunc *f, const XiValue *target,
                                        const XiBorrowSig *own_sig, const XiValue *borrowed_recv) {
    if (target == borrowed_recv)
        return OWN_BORROWED;
    if (f->operator_borrowed && target->op == XI_PARAM)
        return OWN_BORROWED;
    if (target->op == XI_PARAM && param_is_borrowed(f, target, own_sig))
        return OWN_BORROWED;
    if (arc_value_is_borrow_alias(target, 0))
        return OWN_BORROWED;
    /* A declared `return self` result (xi_receiver_alias) is the receiver's own
     * reference under a second SSA name, handed back at +0. That is a BORROW,
     * exactly like the ops whose ops.def result-ownership column already says
     * BORROWED for the same shape (xi.byte.array.append.from and friends).
     *
     * OWN_CALL_RESULT is unsound for it: that mode skips the retain at a
     * consume which is the result's last use, so `return a.reverse()` moves out
     * a reference the function never held while `a`'s own death-drop frees the
     * object (contract C1, then C2 on the caller's release). OWN_BORROWED
     * retains at EVERY consuming use and never drops, which is the convention
     * a +0 alias actually has. */
    if (xi_call_result_aliases_receiver(target))
        return OWN_BORROWED;
    if (op_is_call(target->op) && !call_returns_fresh(f, target))
        return OWN_CALL_RESULT;
    return OWN_OWNED;
}

static void arc_insert_rec(XiFunc *f) {
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            arc_insert_rec(f->children[i]);
    }

    /* RPO + dominators are needed to order consume sites across blocks and to
     * gate drop placement (a drop at a return block is only sound where the
     * value's definition dominates it). */
    xi_ensure_rpo(f);
    xi_ensure_dominators(f);

    /* Borrow signature: which parameters does this function only borrow (never
     * store/return/forward)? A borrowed parameter must NOT be dropped by the
     * callee — the caller retains ownership (Perceus borrowed-parameter rule).
     * Cached on the pre-ARC IR (xi_arc_analyze_contracts) and shared with callers,
     * which use it to keep ownership of a borrowed argument rather than moving
     * it into a callee that never releases it. */
    const XiBorrowSig *own_sig = arc_get_borrow_sig(f);

    /* Snapshot the set of tracked values first: we mutate blocks while
     * inserting, so collect targets up front. The list is dynamic; dropping
     * later RC values would leave required retain/release ops uninjected. */
    XiValueVec targets = {0};

    for (uint16_t p = 0; p < xi_func_semantic_param_count(f); p++) {
        if (f->params[p] && tracks_rc(f->params[p]) && !xi_value_vec_push(&targets, f->params[p])) {
            xr_free(targets.items);
            XR_CHECK(false, "xi_arc: out of memory collecting ARC targets");
            return;
        }
    }
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            /* Params can appear in the entry block's value list; they are
             * already collected (with the correct param mode) by the params
             * loop above. Collecting them again here would process each twice
             * and insert a duplicate death-drop on every non-consuming branch
             * path (a double-free). */
            if (v && v->op != XI_PARAM &&
                stack_alloc_closure_uses_are_scoped_par_for_callbacks(f, v))
                continue;
            if (v && v->op != XI_PARAM && tracks_rc(v) && !xi_value_vec_push(&targets, v)) {
                xr_free(targets.items);
                XR_CHECK(false, "xi_arc: out of memory collecting ARC targets");
                return;
            }
        }
        /* Phi results are owning SSA values too. They live on blk->phis rather
         * than blk->values, so omitting them transfers ownership into a value
         * ARC never tracks; a second consume after the join then uses a freed
         * object. */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (tracks_rc(&phi->value) && !xi_value_vec_push(&targets, &phi->value)) {
                xr_free(targets.items);
                XR_CHECK(false, "xi_arc: out of memory collecting ARC phi targets");
                return;
            }
        }
    }

    /* The borrowed method receiver (params[0] when receiver_borrowed) is
     * passed without a caller dup, so it must be processed under the
     * borrowed convention: dup at consuming uses, never drop. Operator
     * methods receive ALL their params borrowed (the VM operator dispatch
     * leaves operands live in the caller's registers). */
    XiValue *borrowed_recv = (f->receiver_borrowed && f->nparams > 0) ? f->params[0] : NULL;

    /* Whole-function liveness for coroutine-frame pinning. NULL when the
     * function has no suspend point, which disables pinning entirely. Drop
     * placement can split CFG edges, so recompute after any target that
     * changed the CFG — the cached bitsets are sized and indexed for the
     * block set they were computed on. */
    XiLiveness *coro_live = arc_coro_frame_pin_liveness(f);

    bool cfg_changed = false;
    for (uint32_t i = 0; i < targets.count; i++) {
        XiArcOwnMode mode = arc_target_own_mode(f, targets.items[i], own_sig, borrowed_recv);
        bool changed = process_value_ex(f, targets.items[i], mode, coro_live);
        cfg_changed |= changed;
        if (changed && coro_live) {
            xi_liveness_free(coro_live);
            coro_live = xi_compute_liveness(f);
        }
    }
    xr_free(targets.items);
    if (coro_live)
        xi_liveness_free(coro_live);

    if (cfg_changed)
        xi_cfg_invalidate(f);
}

/* A tail call rewrites the current frame and jumps (OP_TAILCALL /
 * OP_INVOKE_TAIL): nothing after it in the block ever executes. ARC puts a
 * value's drop at its death point, and for anything still live across the call
 * that lands AFTER it — so every one of those drops silently never runs, and
 * the whole frame's worth of objects leaks.
 *
 * Lowering already clears XI_FLAG_TAIL when a defer is pending, for exactly
 * this reason. Releases need the same treatment; ARC is the first pass that
 * knows they exist, so the flag is withdrawn here.
 *
 * Losing the optimization is the right trade: reusing one stack frame is worth
 * far less than not leaking every owned local at that call. A tail call with
 * nothing to release keeps the flag, which covers the recursion-depth cases
 * the optimization exists for. */
static void arc_withdraw_tail_flag_before_releases(XiFunc *f) {
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            arc_withdraw_tail_flag_before_releases(f->children[i]);
    }
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        /* Backwards, so "is there a release after this value" is one bit of
         * state. A tail call is emitted as part of a return, so the release
         * that outlives it is in the same block. */
        bool release_follows = false;
        for (int32_t vi = (int32_t) blk->nvalues - 1; vi >= 0; vi--) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->op == XI_RELEASE) {
                release_follows = true;
                continue;
            }
            if ((v->flags & XI_FLAG_TAIL) && release_follows)
                v->flags &= (uint8_t) ~XI_FLAG_TAIL;
        }
    }
}

/* ========== Release-before-redefinition ordering ==========
 *
 * The VM emitter coalesces every SSA value of one source variable onto a
 * single register. Two values of the same variable are distinct in SSA, so
 * their relative order is free here, but after coalescing a later definition
 * overwrites the register an earlier definition's pending RELEASE still reads:
 *
 *     v46 = RETAIN v17           ; dup the incoming value
 *     v23 = OWNER_FORWARD v17    ; MOVE cur_reg, v17_reg   <- overwrites
 *     v45 = RELEASE v12          ; DROP cur_reg            <- drops the NEW value
 *
 * The drop then releases the object just stored instead of the one being
 * replaced, so `cur = cur.field` inside a loop frees the field it just read.
 * AOT is unaffected: it gives every SSA value its own C variable.
 *
 * Normalizing the order here rather than in the emitter keeps one rule for
 * both backends. The release moves ahead of the first definition that shares
 * its coalescing variable, but never ahead of a retain: retain-before-release
 * is what makes self-assignment safe, and the phi-edge dup relies on it. */
static void arc_order_release_before_var_redef(XiFunc *f) {
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            arc_order_release_before_var_redef(f->children[i]);
    }
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *rel = blk->values[vi];
            if (!rel || rel->op != XI_RELEASE || rel->nargs < 1)
                continue;
            XiValue *target = rel->args[0];
            if (!target || !xi_var_id_is_valid(target->var_id))
                continue;
            /* Only the loop-carried shape is reordered: a phi holds the
             * variable's incoming value and an ownership transfer in the same
             * block rebinds it. Widening this to every same-variable
             * definition also reorders releases the borrow and ref-parameter
             * paths depend on, which is a different question from the register
             * hazard being fixed here. */
            if (target->op != XI_PHI)
                continue;

            /* A release may never precede the definition of what it releases,
             * so a block-local target fixes the earliest legal position. A
             * target defined elsewhere (a phi, or an earlier block) is already
             * live on entry and constrains nothing here. */
            uint32_t lower = 0;
            for (uint32_t j = 0; j < vi; j++) {
                if (blk->values[j] == target) {
                    lower = j + 1;
                    break;
                }
            }

            /* First same-variable redefinition ahead of this release. */
            uint32_t redef = vi;
            for (uint32_t j = lower; j < vi; j++) {
                XiValue *d = blk->values[j];
                if (!d || d == target)
                    continue;
                if (d->op != XI_OWNER_FORWARD && d->op != XI_SOURCE_MOVE)
                    continue;
                if (d->var_id == target->var_id) {
                    redef = j;
                    break;
                }
            }
            if (redef == vi)
                continue;

            /* The release may not move ahead of a reader of the same value, and
             * may not move ahead of a retain: retain-before-release is what
             * makes self-assignment and the phi-edge dup safe. Both push the
             * destination back; if either pushes it past the redefinition the
             * hazard cannot be fixed by ordering alone, so leave it in place. */
            uint32_t dest = redef;
            for (uint32_t j = redef; j < vi; j++) {
                XiValue *d = blk->values[j];
                if (!d)
                    continue;
                if (d->op == XI_RETAIN) {
                    dest = j + 1;
                    continue;
                }
                for (uint16_t a = 0; a < d->nargs; a++) {
                    if (d->args[a] == target) {
                        dest = j + 1;
                        break;
                    }
                }
            }
            if (dest > redef || dest >= vi)
                continue;

            memmove(&blk->values[dest + 1], &blk->values[dest],
                    (size_t) (vi - dest) * sizeof(XiValue *));
            blk->values[dest] = rel;
        }
    }
}

/* ========== Adjacent retain/release ordering ==========
 *
 * Phi-edge ownership promotion and death-point cleanup are computed in
 * separate walks.  Their insertions can therefore leave a pure RC run in
 * either order:
 *
 *     RELEASE old
 *     RETAIN  incoming
 *
 * The values are distinct SSA definitions, but VM register coalescing may map
 * both to the same source-variable register.  When the two definitions hold
 * the same object (the normal ref-parameter loop shape), DROP destroys the
 * last reference before DUP can acquire the replacement owner.  AOT happens
 * to avoid the UAF because it keeps distinct C temporaries, but the ownership
 * contract must not depend on backend register allocation.
 *
 * RETAIN has no user-visible side effect and acquiring replacement ownership
 * before relinquishing old ownership is the universally safe order, including
 * self-assignment.  Stable-partition each contiguous RC-only run so retains
 * execute before releases; never cross a non-RC instruction. */
static void arc_order_adjacent_rc_runs(XiFunc *f) {
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            arc_order_adjacent_rc_runs(f->children[i]);
    }
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t vi = 1; vi < blk->nvalues; vi++) {
            XiValue *retain = blk->values[vi];
            if (!retain || retain->op != XI_RETAIN)
                continue;
            uint32_t dest = vi;
            while (dest > 0) {
                XiValue *prev = blk->values[dest - 1];
                if (!prev || prev->op != XI_RELEASE)
                    break;
                blk->values[dest] = prev;
                dest--;
            }
            blk->values[dest] = retain;
        }
    }
}

XR_FUNC void xi_arc_insert(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_arc_insert: NULL func");
    /* Production runs contract analysis as its own pipeline stage. Standalone
     * IR clients may request insertion directly, so enforce the prerequisite
     * without making physical ARC the source of the contracts. */
    if (!f->arc_borrow_sig)
        xi_arc_analyze_contracts(f);
    arc_insert_rec(f);
    arc_order_release_before_var_redef(f);
    arc_order_adjacent_rc_runs(f);
    arc_withdraw_tail_flag_before_releases(f);
}

/* Return the nearest producer associated with a unit ERR_CHECK.  ARC ops can
 * legally be inserted between the producer and the check, so adjacency is not
 * sufficient.  Stop at a previous check: pending-error boundaries never
 * borrow a producer across another already-consumed boundary. */
static XiValue *arc_err_check_source(XiValue *check) {
    if (!check || check->op != XI_ERR_CHECK || !check->block)
        return NULL;
    XiBlock *blk = check->block;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] != check)
            continue;
        while (i > 0) {
            XiValue *candidate = blk->values[--i];
            if (!candidate)
                continue;
            if (candidate->op == XI_ERR_CHECK)
                return NULL;
            if ((candidate->flags & XI_FLAG_MAY_THROW) != 0 || candidate->op == XI_SCOPE_EXIT)
                return candidate;
        }
        break;
    }
    return NULL;
}

static bool arc_value_is_defined_before_block_index(const XiValue *target, const XiBlock *blk,
                                                    uint32_t index) {
    if (!target || !blk)
        return false;
    /* A definition in another block is available on the error edge only when
     * it dominates the check.  Treating every cross-block definition as
     * available lets branch-local owners leak into an unrelated cold cleanup;
     * CGen would then emit a release of a temporary that is not declared on
     * that path. */
    if (target->block != blk)
        return target->block && xi_dominates(target->block, blk);
    if (target->op == XI_PHI)
        return true;
    for (uint32_t i = 0; i < index; i++) {
        if (blk->values[i] == target)
            return true;
    }
    return false;
}

static bool arc_value_live_after_block_index(const ArcLive *live, uint32_t index) {
    return live &&
           (live->live_out || live->use_at_end || (live->has_use && live->last_use > index));
}

/* Attach error-edge owner operands for one function after ordinary
 * ARC elimination.  The temporary vectors intentionally live off-arena: all
 * liveness queries run before any ERR_CHECK operands are published, so one
 * check's cleanup list cannot extend another target's normal-path liveness. */
static void arc_attach_error_cleanups_func(XiFunc *f) {
    XiValueVec checks = {0};
    XiValueVec targets = {0};
    const XiBorrowSig *own_sig = arc_get_borrow_sig(f);
    XiValue *borrowed_recv = (f->receiver_borrowed && f->nparams > 0) ? f->params[0] : NULL;

    /* The optimization pipeline may have invalidated CFG analysis after the
     * ordinary ARC pass.  Error-cleanup publication performs dominance
     * queries of its own, so refresh the cached tree at the final IR shape. */
    xi_ensure_dominators(f);

    for (uint16_t p = 0; p < xi_func_semantic_param_count(f); p++) {
        if (f->params[p] && tracks_rc(f->params[p]) && !xi_value_vec_push(&targets, f->params[p]))
            goto oom;
    }
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            /* Optimizers may retain a unit check after folding its producer to
             * a proven-nothrow form.  CGen erases that check, so it has no
             * error edge and must not gain cleanup operands (which would also
             * make otherwise-dead owners look live on the hot path). */
            if (v->op == XI_ERR_CHECK && (!v->type || v->type->kind != XR_KIND_BOOL) &&
                v->nargs == 0 && arc_err_check_source(v) != NULL && !xi_value_vec_push(&checks, v))
                goto oom;
            if (v->op == XI_PARAM || stack_alloc_closure_uses_are_scoped_par_for_callbacks(f, v))
                continue;
            if (tracks_rc(v) && !xi_value_vec_push(&targets, v))
                goto oom;
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (tracks_rc(&phi->value) && !xi_value_vec_push(&targets, &phi->value))
                goto oom;
        }
    }
    if (checks.count == 0)
        goto done;

    XiValueVec *cleanups = (XiValueVec *) xr_calloc(checks.count, sizeof(*cleanups));
    if (!cleanups)
        goto oom;

    /* Reverse definition order gives simultaneously-dead owners the same
     * destructor ordering as the ordinary repeated insert-after death drops. */
    for (uint32_t ti = targets.count; ti-- > 0;) {
        XiValue *target = targets.items[ti];
        if (arc_target_own_mode(f, target, own_sig, borrowed_recv) != OWN_OWNED)
            continue;
        ArcLive *live = NULL;
        uint32_t *pos_by_id = NULL;
        if (!arc_compute_liveness(f, target, &live, &pos_by_id)) {
            for (uint32_t ci = 0; ci < checks.count; ci++)
                xr_free(cleanups[ci].items);
            xr_free(cleanups);
            goto oom;
        }
        for (uint32_t ci = 0; ci < checks.count; ci++) {
            XiValue *check = checks.items[ci];
            XiBlock *blk = check->block;
            if (!blk || !pos_by_id[blk->id])
                continue;
            uint32_t check_index = UINT32_MAX;
            for (uint32_t i = 0; i < blk->nvalues; i++) {
                if (blk->values[i] == check) {
                    check_index = i;
                    break;
                }
            }
            if (check_index == UINT32_MAX ||
                !arc_value_is_defined_before_block_index(target, blk, check_index) ||
                !arc_value_live_after_block_index(&live[pos_by_id[blk->id] - 1], check_index))
                continue;
            if (!xi_value_vec_push(&cleanups[ci], target)) {
                xr_free(pos_by_id);
                xr_free(live);
                for (uint32_t cj = 0; cj < checks.count; cj++)
                    xr_free(cleanups[cj].items);
                xr_free(cleanups);
                goto oom;
            }
        }
        xr_free(pos_by_id);
        xr_free(live);
    }

    for (uint32_t ci = 0; ci < checks.count; ci++) {
        uint32_t nargs = cleanups[ci].count;
        XR_CHECK(nargs <= UINT16_MAX, "xi_arc: too many unit ERR_CHECK cleanup owners");
        XiValue **args = NULL;
        if (nargs != 0) {
            args = (XiValue **) xi_func_arena_alloc(f, (uint32_t) ((size_t) nargs * sizeof(*args)));
            XR_CHECK(args != NULL, "xi_arc: out of memory publishing ERR_CHECK cleanups");
            for (uint32_t i = 0; i < cleanups[ci].count; i++)
                args[i] = cleanups[ci].items[i];
        }
        checks.items[ci]->args = args;
        checks.items[ci]->nargs = (uint16_t) nargs;
        xr_free(cleanups[ci].items);
    }
    xr_free(cleanups);
    goto done;

oom:
    XR_CHECK(false, "xi_arc: out of memory attaching ERR_CHECK cleanup owners");
done:
    xr_free(targets.items);
    xr_free(checks.items);
}

XR_FUNC void xi_arc_attach_error_cleanups(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_arc_attach_error_cleanups: NULL func");
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_arc_attach_error_cleanups(f->children[i]);
    }
    arc_attach_error_cleanups_func(f);
}

/* ========== Dup/Drop Elimination (copy→move optimization) ========== */

/* Remove value at index 'idx' from block, shifting subsequent values down. */
static void arc_remove_value(XiBlock *blk, uint32_t idx) {
    XR_DCHECK(blk != NULL, "arc_remove_value: NULL block");
    XR_DCHECK(idx < blk->nvalues, "arc_remove_value: index out of bounds");
    for (uint32_t j = idx; j + 1 < blk->nvalues; j++)
        blk->values[j] = blk->values[j + 1];
    blk->nvalues--;
}

/* Count how many times value `v` is used as an arg across the entire function,
 * excluding uses in XI_RETAIN and XI_RELEASE ops (those are the RC machinery
 * we are trying to eliminate). */
static int count_real_uses(const XiFunc *f, const XiValue *v) {
    int count = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *user = blk->values[i];
            if (!user)
                continue;
            if (user->op == XI_RETAIN || user->op == XI_RELEASE)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] == v)
                    count++;
            }
        }
        /* Phi uses (phis live on blk->phis, not in blk->values[]). */
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == v)
                    count++;
            }
        }
        /* Return terminator */
        if (blk->control == v)
            count++;
    }
    return count;
}

static bool function_has_target_op(const XiFunc *f, uint16_t op, const XiValue *target) {
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *value = blk->values[i];
            if (value && value->op == op && value->nargs >= 1 && value->args[0] == target)
                return true;
        }
    }
    return false;
}

/* Pattern 2 is a move optimization, so it is valid only for values whose
 * consumer may receive the existing owned reference. Borrowed values own no
 * reference in this function; their retain before a consuming use is the
 * transfer itself and must not be removed. */
static bool arc_elim_can_remove_single_consumer_retain(const XiFunc *f, const XiValue *target) {
    if (!f || !target)
        return false;
    if (target->op == XI_PARAM) {
        if (f->receiver_borrowed && f->nparams > 0 && f->params[0] == target)
            return false;
        if (f->operator_borrowed)
            return false;
        const XiBorrowSig *sig = f->arc_borrow_sig;
        if (!sig || !sig->valid)
            return false;
        if (param_is_borrowed(f, target, sig))
            return false;
        return true;
    }
    if (arc_value_is_borrow_alias(target, 0))
        return false;
    if (op_is_call(target->op))
        return call_returns_fresh(f, target);
    return true;
}

/* A static single-consumer use inside a loop may execute more than once for
 * one owner produced outside that loop.  Its retain replenishes ownership on
 * every iteration and cannot be folded into a one-time move.  Values produced
 * in the same innermost loop have a fresh ownership epoch for every dynamic
 * use, so they remain eligible for the copy-to-move optimization. */
static bool arc_retain_reuses_owner_across_iterations(const XiLoopInfo *loops,
                                                      const XiBlock *retain_block,
                                                      const XiValue *target) {
    if (!loops || !retain_block || retain_block->id >= loops->nblocks)
        return false;
    const XiLoop *retain_loop = loops->block_to_loop[retain_block->id];
    if (!retain_loop)
        return false;
    if (!target || !target->block || target->block->id >= loops->nblocks)
        return true;
    return loops->block_to_loop[target->block->id] != retain_loop;
}

/* Single-block dup/drop pair elimination.
 *
 * Pattern 1 — "Redundant bracket":
 *   XI_RETAIN(v) at position i
 *   XI_RELEASE(v) at position j  (j > i)
 *   v is NOT used by any instruction between i and j (exclusive)
 *   → The pair is dead: the retain increments RC only for the release to
 *     decrement it back. Remove both.
 *
 * Pattern 2 — "Single-consumer forward":
 *   XI_RETAIN(v) at position i
 *   A single consuming use of v at position k (i < k)
 *   XI_RELEASE(v) does NOT exist elsewhere (no double-drop)
 *   The value v has exactly 1 real (non-RC) use in the function
 *   v is a true owned/fresh value, not a borrowed projection/parameter
 *   the consuming block is not in a CFG cycle
 *   → The retain is redundant because there is only one consumer: it already
 *     receives ownership via the last-use move rule. Remove the retain.
 *
 * We apply Pattern 1 iteratively (removing a pair may expose new pairs). */
static int elim_block(XiBlock *blk, const XiFunc *f, const XiLiveness *coro_live,
                      const XiLoopInfo *loops) {
    int eliminated = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *dup = blk->values[i];
            if (!dup || dup->op != XI_RETAIN)
                continue;
            XR_DCHECK(dup->nargs >= 1 && dup->args[0] != NULL, "arc_elim: malformed RETAIN");
            XiValue *target = dup->args[0];

            /* Pattern 1: look for a matching XI_RELEASE(target) in the same
             * block with no intervening use of target. */
            for (uint32_t j = i + 1; j < blk->nvalues; j++) {
                XiValue *drop = blk->values[j];
                if (!drop)
                    continue;
                /* If target is used between i and j, the retain is needed. */
                if (drop->op != XI_RELEASE) {
                    /* Check if this instruction uses target. */
                    bool uses_target = false;
                    for (uint16_t a = 0; a < drop->nargs; a++) {
                        if (drop->args[a] == target) {
                            uses_target = true;
                            break;
                        }
                    }
                    if (uses_target)
                        break; /* target is consumed between dup/drop — keep */
                    continue;
                }
                /* Found a RELEASE. Check if it drops the same target. */
                if (drop->nargs < 1 || drop->args[0] != target)
                    continue;
                if (arc_value_is_coro_frame_pinned(f, coro_live, target) &&
                    arc_block_forwards_target_to_distinct_phi(blk, target))
                    break;
                /* Match! Remove both the RETAIN (at i) and RELEASE (at j).
                 * Remove j first (higher index) to keep i valid. */
                arc_remove_value(blk, j);
                arc_remove_value(blk, i);
                eliminated++;
                changed = true;
                break;
            }
            if (changed)
                break; /* restart scan from the beginning */
        }
    }

    /* Most blocks contain no remaining retain after Pattern 1.  Avoid a graph
     * walk for those blocks; a block that reaches Pattern 2 computes cycle
     * membership once and shares it across all of its candidates. */
    bool has_retain = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] && blk->values[i]->op == XI_RETAIN) {
            has_retain = true;
            break;
        }
    }
    if (!has_retain)
        return eliminated;

    /* Pattern 2: single-consumer retain elimination.
     * After removing redundant bracket pairs, check remaining RETAINs:
     * if the target value has exactly 1 real use (excluding RC ops) in the
     * function, the retain is unnecessary — ownership flows directly. */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *dup = blk->values[i];
        if (!dup || dup->op != XI_RETAIN)
            continue;
        XiValue *target = dup->args[0];
        if (!target)
            continue;

        int real_uses = count_real_uses(f, target);
        if (real_uses <= 1 && !arc_retain_reuses_owner_across_iterations(loops, blk, target) &&
            value_has_consuming_use(f, target) &&
            arc_elim_can_remove_single_consumer_retain(f, target) &&
            !arc_value_is_coro_frame_pinned(f, coro_live, target) &&
            !function_has_target_op(f, XI_RELEASE, target)) {
            /* The value flows to at most one real consumer; the retain is
             * dead weight only when that use consumes ownership. A sole
             * borrowing use can intentionally sit between RETAIN and RELEASE
             * to create an independent owned alias; removing that retain
             * turns the alias into a dangling reference.
             *
             * A retain of an owner produced outside its consuming loop is
             * also excluded: one static consumer can execute repeatedly, so
             * the retain replenishes ownership on every iteration. Removing
             * it turns the first iteration into a move and leaves the next
             * iteration with a dangling reference. A value produced inside
             * the same innermost loop has a fresh owner each iteration and is
             * still eligible for forwarding.
             *
             * An explicit release of the same owner also excludes the
             * rewrite: the retain then materializes a distinct owner for a
             * PHI or other consuming alias, and removing only the retain
             * makes the explicit drop over-release the incoming reference.
             *
             * A coroutine frame member is excluded for the same reason in a
             * different shape: its retain is not a forwarding copy but the
             * reference the consumer keeps while the frame slot holds its
             * own, which the slot's own release gives up. Removing it
             * re-creates the stale moved-out slot the pinning prevents. */
            arc_remove_value(blk, i);
            eliminated++;
            i--; /* re-examine the slot */
        }
    }

    return eliminated;
}

XR_FUNC int xi_arc_elim(XiFunc *f) {
    if (!f)
        return 0;

    int total = 0;

    /* Process children first (bottom-up). */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            total += xi_arc_elim(f->children[i]);
    }

    /* Same pinning view as insertion: single-consumer retains of coroutine
     * frame members must survive elimination. Insertion may have split CFG
     * edges, so re-establish RPO before computing liveness. */
    xi_ensure_rpo(f);
    const XiLoopInfo *loops = xi_ensure_loops(f);
    XiLiveness *coro_live = arc_coro_frame_pin_liveness(f);

    /* Eliminate within each block. */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk || blk->nvalues == 0)
            continue;
        total += elim_block(blk, f, coro_live, loops);
    }

    if (coro_live)
        xi_liveness_free(coro_live);
    return total;
}
