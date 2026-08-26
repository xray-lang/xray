/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_callable.c - Closed-world function-value target and invoke plans
 */

#include "xaot_callable.h"
#include "xaot_boundary.h"
#include "xaot_struct_name.h"
#include "refine/xr_aot_scalar_value.h"
#include "../base/xglobal_indices.h"
#include "../base/xmalloc.h"
#include "../ir/xi_coro_analyze.h"
#include "../ir/xi_op_name.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CallableSet {
    uint32_t *items; /* indices into XaotBundle.func_plans */
    uint32_t count;
    uint32_t cap;
    bool may_be_null;
} CallableSet;

typedef struct CallableFuncFacts {
    const XiFunc *func;
    CallableSet *values; /* indexed by XiValue.id */
    uint32_t value_count;
    CallableSet returns;
    uint32_t effect_bits;
} CallableFuncFacts;

typedef enum CallableStorageKind {
    CALLABLE_STORAGE_FIELD = 1,
    CALLABLE_STORAGE_INDEX = 2,
    CALLABLE_STORAGE_TUPLE_FIELD = 3,
} CallableStorageKind;

typedef struct CallableStorageFacts {
    uint8_t kind;
    uint64_t owner_key;
    uint32_t member_key;
    CallableSet targets;
} CallableStorageFacts;

typedef struct CallableAnalysis {
    const XaotBundle *bundle;
    CallableFuncFacts *funcs;
    uint8_t *reachable_funcs;  /* indexed by func plan */
    uint8_t *reachable_bodies; /* indexed by global-evidence body */
    CallableSet **module_slots;
    CallableStorageFacts *storage;
    uint32_t nstorage;
    uint32_t storage_cap;
} CallableAnalysis;

/* Canonical Xi records the strongest portable effect of parallel operations:
 * the VM may park and replay them while the AOT runtime executes the operation
 * as a synchronous fork/join boundary.  Callable planning is target-specific,
 * so do not turn an AOT caller into a coroutine solely because it contains a
 * canonical parallel op.  Real suspension in the callback remains rejected by
 * parallel CGen and all other suspend points retain the shared classifier. */
static bool callable_aot_value_may_suspend(const XiFunc *func, const XiValue *value) {
    if (!value)
        return false;
    if (value->op == XI_PAR_FOR || value->op == XI_PAR_MAP || value->op == XI_PAR_REDUCE)
        return false;
    return xi_coro_is_suspend_point(func, value, NULL);
}

static bool callable_reserve(void **items, uint32_t *cap, uint32_t needed, size_t item_size) {
    uint32_t next;
    void *grown;
    if (*cap >= needed)
        return true;
    next = *cap ? *cap : 4;
    while (next < needed) {
        if (next > UINT32_MAX / 2)
            return false;
        next *= 2;
    }
    grown = xr_realloc(*items, (size_t) next * item_size);
    if (!grown)
        return false;
    *items = grown;
    *cap = next;
    return true;
}

static bool callable_set_contains(const CallableSet *set, uint32_t item) {
    if (!set)
        return false;
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->items[i] == item)
            return true;
    }
    return false;
}

static bool callable_set_add(CallableSet *set, uint32_t item, bool *changed) {
    uint32_t pos;
    if (!set)
        return false;
    if (callable_set_contains(set, item))
        return true;
    if (!callable_reserve((void **) &set->items, &set->cap, set->count + 1, sizeof(uint32_t)))
        return false;
    pos = set->count;
    while (pos > 0 && set->items[pos - 1] > item) {
        set->items[pos] = set->items[pos - 1];
        pos--;
    }
    set->items[pos] = item;
    set->count++;
    if (changed)
        *changed = true;
    return true;
}

static bool callable_set_union(CallableSet *dst, const CallableSet *src, bool *changed) {
    if (!dst || !src)
        return false;
    if (src->may_be_null && !dst->may_be_null) {
        dst->may_be_null = true;
        if (changed)
            *changed = true;
    }
    for (uint32_t i = 0; i < src->count; i++) {
        if (!callable_set_add(dst, src->items[i], changed))
            return false;
    }
    return true;
}

static void callable_set_free(CallableSet *set) {
    if (!set)
        return;
    xr_free(set->items);
    memset(set, 0, sizeof(*set));
}

static int callable_func_index(const CallableAnalysis *a, const XiFunc *func) {
    if (!a || !func)
        return -1;
    for (uint32_t i = 0; i < a->bundle->nfunc_plans; i++) {
        if (a->bundle->func_plans[i].func == func)
            return (int) i;
    }
    return -1;
}

XR_FUNC bool xaot_callable_func_has_executable_body_plan(const XaotBundle *bundle,
                                                         const XiFunc *func) {
    if (!func)
        return false;
    if (!bundle || func->xg_body_func_id == XG_NO_ID)
        return !func->is_generic_template;
    bool selected = false;
    for (uint32_t i = 0; i < bundle->ngeneric_body_plans; i++) {
        const XaotGenericBodyPlan *plan = &bundle->generic_body_plans[i];
        switch ((XaotGenericBodyAction) plan->action) {
            case XAOT_GENERIC_BODY_CLONE:
                if (plan->specialized_body_func_id == func->xg_body_func_id)
                    selected = true;
                break;
            case XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY:
            case XAOT_GENERIC_BODY_DIRECT_CONSTRAINT_CALL:
                if (plan->origin_body_func_id == func->xg_body_func_id)
                    selected = true;
                break;
            case XAOT_GENERIC_BODY_REJECT:
                break;
        }
    }
    /* A specialized clone body is executable because its plan selects it; it
     * carries no ABI of its own until then.
     *
     * The origin is a separate question. `is_generic_template` marks the bodies
     * that have no erased ABI at all — those nested in an uninstantiated
     * generic class skeleton (see xi_lower_class.inc.c and
     * pending_body_is_generic_template). Function- and method-level generic
     * origins keep a canonical erased body with a real callable ABI, so a clone
     * plan covering *some* call sites must not retire it: type arguments the
     * frontend cannot spell as a type ref (tuples, unions, shaped Json, ...)
     * still reach the origin erased. Emission stays gated on reachability, so
     * an origin whose call sites all specialized is still dropped. */
    return selected || !func->is_generic_template;
}

static CallableFuncFacts *callable_func_facts(CallableAnalysis *a, const XiFunc *func) {
    int index = callable_func_index(a, func);
    return index >= 0 ? &a->funcs[index] : NULL;
}

static CallableSet *callable_value_set(CallableAnalysis *a, const XiFunc *owner,
                                       const XiValue *value) {
    CallableFuncFacts *facts = callable_func_facts(a, owner);
    if (!facts || !value || value->id >= facts->value_count)
        return NULL;
    return &facts->values[value->id];
}

static uint32_t callable_body_effects(const XaotBundle *bundle, const XiFunc *func) {
    const XgGlobalEvidence *ev = bundle ? bundle->global_evidence_plan.evidence : NULL;
    if (ev && func && func->xg_body_func_id != XG_NO_ID) {
        for (uint32_t i = 0; i < ev->nbodies; i++) {
            if (ev->bodies[i].func_id == func->xg_body_func_id)
                return ev->bodies[i].effect_bits;
        }
    }
    return 0;
}

static uint32_t callable_func_value_count(const XiFunc *func) {
    uint32_t count = func ? func->next_value_id : 0;
    if (!func)
        return 0;
    for (uint16_t pi = 0; pi < func->nparams; pi++) {
        const XiValue *param = func->params ? func->params[pi] : NULL;
        if (param && param->id < UINT32_MAX && count <= param->id)
            count = param->id + 1;
    }
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (phi->value.id < UINT32_MAX && count <= phi->value.id)
                count = phi->value.id + 1;
        }
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (value && value->id < UINT32_MAX && count <= value->id)
                count = value->id + 1;
        }
    }
    return count;
}

static const XiFunc *callable_resolve_direct_target(const XaotBundle *bundle, const XiFunc *owner,
                                                    const XiValue *call) {
    XaotLeafAggregateTargetView leaf_aggregate = {0};
    XaotLeafAggregateTargetStatus leaf_status = xaot_boundary_leaf_aggregate_call_view(
        bundle, owner, call, &leaf_aggregate, NULL, 0);
    if (leaf_status == XAOT_LEAF_AGGREGATE_TARGET_FOUND)
        return leaf_aggregate.callee;
    if (leaf_status == XAOT_LEAF_AGGREGATE_TARGET_INVALID)
        return NULL;
    XaotDirectI64TargetView direct_i64 = {0};
    XaotDirectI64TargetStatus direct_i64_status = xaot_boundary_direct_i64_call_view(
        bundle, owner, call, &direct_i64, NULL, 0);
    if (direct_i64_status == XAOT_DIRECT_I64_TARGET_FOUND)
        return direct_i64.callee;
    if (direct_i64_status == XAOT_DIRECT_I64_TARGET_INVALID)
        return NULL;
    uint16_t first_arg = 0;
    uint16_t first_param = 0;
    const XiFunc *target =
        xaot_boundary_resolve_direct_call_target(bundle, owner, call, &first_arg);
    if (!target)
        target = xaot_boundary_resolve_constructor_call_target(bundle, owner, call, &first_arg,
                                                               &first_param);
    return target;
}

static bool callable_call_is_function_value(const XaotBundle *bundle, const XiFunc *owner,
                                            const XiValue *call) {
    /* XI_CALL's operand zero is the callable by construction.  Calls whose
     * callee resolves to a named function or constructor use the ordinary
     * direct-call plan.  Every other XI_CALL is a function-value boundary,
     * even if backend rep selection erased its source function type. */
    if (!call || call->op != XI_CALL || call->nargs < 1 || !call->args[0] ||
        callable_resolve_direct_target(bundle, owner, call))
        return false;
    const XiValue *callee = call->args[0];
    /* A raw import reference is a statically named module/foreign boundary.
     * Aliased imported function values flow through ordinary SSA values and
     * are still handled by the closed-world callable analysis. */
    if (callee->op == XI_IMPORT_REF)
        return false;
    return (callee->type && XR_TYPE_IS_FUNCTION(callee->type)) || callee->op == XI_LOAD_UPVAL;
}

/* A generator call is deliberately not an ordinary callable invocation: it
 * captures the arguments into a producer frame and returns an iterator without
 * running the producer body.  The target is nevertheless executable once the
 * iterator is pulled, so callable reachability must retain it and its runtime
 * capabilities.  Keep this as a separate edge instead of classifying GEN_CALL
 * as a function-value call, which would incorrectly project MAY_SUSPEND from
 * the producer onto the synchronous caller. */
static CallableSet *callable_generator_targets(CallableAnalysis *a, const XiFunc *owner,
                                               const XiValue *call) {
    if (!a || !owner || !call || call->op != XI_GEN_CALL || call->nargs < 1 || !call->args[0])
        return NULL;
    return callable_value_set(a, owner, call->args[0]);
}

/* A spawned callable is an executable root even though XI_GO/XI_THREAD_SPAWN
 * do not have synchronous call semantics.  In particular, projecting the
 * child's MAY_SUSPEND effect onto the spawning parent would be wrong, but
 * omitting the edge entirely retires the child from entry/runtime capability
 * planning while the generated executor can still run it. */
static CallableSet *callable_spawn_targets(CallableAnalysis *a, const XiFunc *owner,
                                           const XiValue *spawn) {
    if (!a || !owner || !spawn || (spawn->op != XI_GO && spawn->op != XI_THREAD_SPAWN) ||
        spawn->nargs < 1 || !spawn->args[0])
        return NULL;
    return callable_value_set(a, owner, spawn->args[0]);
}

static const XiModule *callable_module_for_func(const XaotBundle *bundle, const XiFunc *func,
                                                uint32_t *out_index) {
    if (!bundle || !func)
        return NULL;
    for (uint32_t i = 0; i < bundle->nfunc_plans; i++) {
        const XaotFuncPlan *plan = &bundle->func_plans[i];
        if (plan->func != func || plan->module_index >= bundle->nmodules)
            continue;
        if (out_index)
            *out_index = plan->module_index;
        return bundle->modules[plan->module_index];
    }
    return NULL;
}

static CallableSet *callable_module_slot(CallableAnalysis *a, const XiFunc *func, int64_t slot) {
    uint32_t module_index;
    const XiModule *mod = callable_module_for_func(a ? a->bundle : NULL, func, &module_index);
    if (!mod || slot < 0 || slot >= mod->nslots || !a->module_slots[module_index])
        return NULL;
    return &a->module_slots[module_index][slot];
}

static CallableStorageFacts *callable_storage(CallableAnalysis *a, uint8_t kind, uint64_t owner_key,
                                              uint32_t member_key, bool create) {
    if (!a || owner_key == 0)
        return NULL;
    for (uint32_t i = 0; i < a->nstorage; i++) {
        CallableStorageFacts *row = &a->storage[i];
        if (row->kind == kind && row->owner_key == owner_key && row->member_key == member_key)
            return row;
    }
    if (!create)
        return NULL;
    if (!callable_reserve((void **) &a->storage, &a->storage_cap, a->nstorage + 1,
                          sizeof(CallableStorageFacts)))
        return NULL;
    CallableStorageFacts *row = &a->storage[a->nstorage++];
    memset(row, 0, sizeof(*row));
    row->kind = kind;
    row->owner_key = owner_key;
    row->member_key = member_key;
    return row;
}

static uint64_t callable_field_owner_key(const XiValue *access) {
    if (!access || access->nargs < 1)
        return 0;
    if (access->xg_class_field_id != XG_NO_ID)
        return (UINT64_C(1) << 63) | access->xg_class_field_id;
    return xaot_type_fingerprint(access->args[0] ? access->args[0]->type : NULL);
}

static uint32_t callable_field_member_key(const XiValue *access) {
    if (!access)
        return 0;
    if (access->xg_class_field_id != XG_NO_ID)
        return access->xg_class_field_id;
    return (uint32_t) access->aux_int;
}

static uint32_t callable_target_id(const XaotBundle *bundle, uint32_t func_index) {
    const XiFunc *func =
        func_index < bundle->nfunc_plans ? bundle->func_plans[func_index].func : NULL;
    if (func && func->xg_body_func_id != XG_NO_ID)
        return func->xg_body_func_id;
    return func_index + 1;
}

static bool callable_seed_static_value(CallableAnalysis *a, const XiFunc *owner,
                                       const XiValue *value, bool *changed) {
    const XiFunc *target = NULL;
    CallableSet *set;
    if (!a || !owner || !value)
        return true;
    set = callable_value_set(a, owner, value);
    if (!set)
        return false;
    if (value->op == XI_CONST && value->type && value->type->kind == XR_KIND_NULL) {
        if (!set->may_be_null) {
            set->may_be_null = true;
            if (changed)
                *changed = true;
        }
    } else if ((value->op == XI_CLOSURE_NEW ||
                (value->op == XI_STACK_ALLOC && value->aux_int == XI_CLOSURE_NEW)) &&
               value->aux) {
        target = (const XiFunc *) value->aux;
    } else if (value->op == XI_GET_SHARED) {
        CallableSet *slot = callable_module_slot(a, owner, value->aux_int);
        if (slot && !callable_set_union(set, slot, changed))
            return false;
    } else if (value->op == XI_IMPORT_REF && value->aux) {
        const XiImportRef *ref = (const XiImportRef *) value->aux;
        if (ref->resolved_mod_index >= 0 && ref->resolved_mod_index < (int) a->bundle->nmodules &&
            ref->resolved_shared_slot >= 0) {
            const XiModule *mod = a->bundle->modules[ref->resolved_mod_index];
            if (mod && ref->resolved_shared_slot < mod->nslots && mod->slot_funcs)
                target = mod->slot_funcs[ref->resolved_shared_slot];
        }
    }
    if (target && xaot_callable_func_has_executable_body_plan(a->bundle, target)) {
        int target_index = callable_func_index(a, target);
        if (target_index >= 0 && !callable_set_add(set, (uint32_t) target_index, changed))
            return false;
    }
    return true;
}

static CallableSet *callable_capture_source_set(CallableAnalysis *a, const XiFunc *func,
                                                const XiCapture *capture) {
    const XiFunc *parent = func ? func->parent_func : NULL;
    while (parent && capture) {
        if (capture->source == XI_CAPTURE_SRC_REG)
            return capture->value ? callable_value_set(a, parent, capture->value) : NULL;
        if (capture->source != XI_CAPTURE_SRC_UPVAL || capture->index >= parent->ncaptures)
            return NULL;
        capture = &parent->captures[capture->index];
        parent = parent->parent_func;
    }
    return NULL;
}

static bool callable_propagate_storage(CallableAnalysis *a, const XiFunc *func,
                                       const XiValue *value, bool *changed) {
    CallableSet *value_set = callable_value_set(a, func, value);
    if (!value_set)
        return false;
    switch ((XiOp) value->op) {
        case XI_SET_SHARED: {
            CallableSet *slot = callable_module_slot(a, func, value->aux_int);
            CallableSet *input =
                value->nargs >= 1 ? callable_value_set(a, func, value->args[0]) : NULL;
            return !slot || !input || callable_set_union(slot, input, changed);
        }
        case XI_GET_SHARED: {
            CallableSet *slot = callable_module_slot(a, func, value->aux_int);
            return !slot || callable_set_union(value_set, slot, changed);
        }
        case XI_STORE_FIELD:
        case XI_AGG_SET: {
            CallableSet *input =
                value->nargs >= 2 ? callable_value_set(a, func, value->args[1]) : NULL;
            CallableStorageFacts *field =
                callable_storage(a, CALLABLE_STORAGE_FIELD, callable_field_owner_key(value),
                                 callable_field_member_key(value), input != NULL);
            return !input || !field || callable_set_union(&field->targets, input, changed);
        }
        case XI_LOAD_FIELD:
        case XI_AGG_GET: {
            CallableStorageFacts *field =
                callable_storage(a, CALLABLE_STORAGE_FIELD, callable_field_owner_key(value),
                                 callable_field_member_key(value), false);
            return !field || callable_set_union(value_set, &field->targets, changed);
        }
        case XI_INDEX_SET: {
            CallableSet *input =
                value->nargs >= 3 ? callable_value_set(a, func, value->args[2]) : NULL;
            uint64_t key = value->nargs >= 1 ? xaot_type_fingerprint(value->args[0]->type) : 0;
            CallableStorageFacts *cell =
                callable_storage(a, CALLABLE_STORAGE_INDEX, key, 0, input != NULL);
            return !input || !cell || callable_set_union(&cell->targets, input, changed);
        }
        case XI_ARRAY_PUSH: {
            CallableSet *input =
                value->nargs >= 2 ? callable_value_set(a, func, value->args[1]) : NULL;
            uint64_t key = value->nargs >= 1 ? xaot_type_fingerprint(value->args[0]->type) : 0;
            CallableStorageFacts *cell =
                callable_storage(a, CALLABLE_STORAGE_INDEX, key, 0, input != NULL);
            return !input || !cell || callable_set_union(&cell->targets, input, changed);
        }
        case XI_INDEX_GET: {
            uint64_t key = value->nargs >= 1 ? xaot_type_fingerprint(value->args[0]->type) : 0;
            CallableStorageFacts *cell = callable_storage(a, CALLABLE_STORAGE_INDEX, key, 0, false);
            return !cell || callable_set_union(value_set, &cell->targets, changed);
        }
        case XI_TUPLE_NEW:
            for (uint16_t i = 0; i < value->nargs; i++) {
                CallableSet *input = callable_value_set(a, func, value->args[i]);
                CallableStorageFacts *field =
                    callable_storage(a, CALLABLE_STORAGE_TUPLE_FIELD,
                                     xaot_type_fingerprint(value->type), i, input != NULL);
                if (input && field && !callable_set_union(&field->targets, input, changed))
                    return false;
            }
            return true;
        case XI_TUPLE_GET: {
            uint64_t key = value->nargs >= 1 ? xaot_type_fingerprint(value->args[0]->type) : 0;
            CallableStorageFacts *field = callable_storage(a, CALLABLE_STORAGE_TUPLE_FIELD, key,
                                                           (uint32_t) value->aux_int, false);
            return !field || callable_set_union(value_set, &field->targets, changed);
        }
        default:
            return true;
    }
}

static bool callable_propagate_value(CallableAnalysis *a, const XiFunc *func, const XiValue *value,
                                     bool *changed) {
    CallableSet *dst;
    if (!value)
        return true;
    if (!callable_seed_static_value(a, func, value, changed))
        return false;
    dst = callable_value_set(a, func, value);
    if (!dst)
        return false;

    if (value->op == XI_PHI || xi_copy_is_identity_alias(value) ||
        xi_op_is_identity_forward(value->op) || value->op == XI_BOX || value->op == XI_UNBOX ||
        value->op == XI_CONVERT || value->op == XI_CHECKTYPE) {
        for (uint16_t i = 0; i < value->nargs; i++) {
            CallableSet *src = callable_value_set(a, func, value->args[i]);
            if (src && !callable_set_union(dst, src, changed))
                return false;
        }
    }

    if (value->op == XI_LOAD_UPVAL && value->aux_int >= 0 && value->aux_int < func->ncaptures) {
        const XiCapture *capture = &func->captures[value->aux_int];
        CallableSet *src = callable_capture_source_set(a, func, capture);
        if (src && !callable_set_union(dst, src, changed))
            return false;
    }

    if (!callable_propagate_storage(a, func, value, changed))
        return false;

    if (value->op == XI_CALL || value->op == XI_CALL_METHOD || value->op == XI_CALL_METHOD_DIRECT) {
        uint16_t first_arg = 0;
        uint16_t first_param = 0;
        const XiFunc *direct =
            xaot_boundary_resolve_direct_call_target(a->bundle, func, value, &first_arg);
        if (!direct)
            direct = xaot_boundary_resolve_constructor_call_target(a->bundle, func, value,
                                                                   &first_arg, &first_param);
        CallableFuncFacts *target = callable_func_facts(a, direct);
        if (target) {
            uint16_t argc = value->nargs > first_arg ? (uint16_t) (value->nargs - first_arg) : 0;
            uint16_t nparams = target->func && target->func->nparams > first_param
                                   ? (uint16_t) (target->func->nparams - first_param)
                                   : 0;
            uint16_t n = argc < nparams ? argc : nparams;
            for (uint16_t ai = 0; ai < n; ai++) {
                CallableSet *arg = callable_value_set(a, func, value->args[first_arg + ai]);
                CallableSet *param =
                    callable_value_set(a, target->func, target->func->params[first_param + ai]);
                if (arg && param && !callable_set_union(param, arg, changed))
                    return false;
            }
            if (!callable_set_union(dst, &target->returns, changed))
                return false;
        }
    }

    if (value->op == XI_GEN_CALL) {
        CallableSet *targets = callable_generator_targets(a, func, value);
        if (!targets)
            return false;
        for (uint32_t ti = 0; ti < targets->count; ti++) {
            CallableFuncFacts *target = &a->funcs[targets->items[ti]];
            uint16_t argc = value->nargs - 1;
            uint16_t nparams = target->func ? target->func->nparams : 0;
            uint16_t n = argc < nparams ? argc : nparams;
            for (uint16_t ai = 0; ai < n; ai++) {
                CallableSet *arg = callable_value_set(a, func, value->args[ai + 1]);
                CallableSet *param = callable_value_set(a, target->func, target->func->params[ai]);
                if (arg && param && !callable_set_union(param, arg, changed))
                    return false;
            }
        }
    }

    if (value->op == XI_GO || value->op == XI_THREAD_SPAWN) {
        CallableSet *targets = callable_spawn_targets(a, func, value);
        if (!targets)
            return false;
        for (uint32_t ti = 0; ti < targets->count; ti++) {
            CallableFuncFacts *target = &a->funcs[targets->items[ti]];
            uint16_t argc = value->nargs - 1;
            uint16_t nparams = target->func ? target->func->nparams : 0;
            uint16_t n = argc < nparams ? argc : nparams;
            for (uint16_t ai = 0; ai < n; ai++) {
                CallableSet *arg = callable_value_set(a, func, value->args[ai + 1]);
                CallableSet *param = callable_value_set(a, target->func, target->func->params[ai]);
                if (arg && param && !callable_set_union(param, arg, changed))
                    return false;
            }
        }
    }

    if (callable_call_is_function_value(a->bundle, func, value)) {
        CallableSet *callees = callable_value_set(a, func, value->args[0]);
        if (!callees)
            return false;
        for (uint32_t ti = 0; ti < callees->count; ti++) {
            uint32_t target_index = callees->items[ti];
            CallableFuncFacts *target = &a->funcs[target_index];
            uint16_t argc = value->nargs - 1;
            uint16_t nparams = target->func ? target->func->nparams : 0;
            uint16_t n = argc < nparams ? argc : nparams;
            for (uint16_t ai = 0; ai < n; ai++) {
                CallableSet *arg = callable_value_set(a, func, value->args[ai + 1]);
                CallableSet *param = callable_value_set(a, target->func, target->func->params[ai]);
                if (arg && param && !callable_set_union(param, arg, changed))
                    return false;
            }
            if (!callable_set_union(dst, &target->returns, changed))
                return false;
        }
    }
    return true;
}

static bool callable_propagate_func(CallableAnalysis *a, CallableFuncFacts *facts, bool *changed) {
    const XiFunc *func = facts ? facts->func : NULL;
    if (!func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (!callable_propagate_value(a, func, &phi->value, changed))
                return false;
        }
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            if (!callable_propagate_value(a, func, block->values[vi], changed))
                return false;
        }
        if (block->kind == XI_BLOCK_RETURN && block->control) {
            CallableSet *returned = callable_value_set(a, func, block->control);
            if (returned && !callable_set_union(&facts->returns, returned, changed))
                return false;
        }
    }
    return true;
}

static bool callable_analysis_init(CallableAnalysis *a, const XaotBundle *bundle) {
    if (!a || !bundle)
        return false;
    memset(a, 0, sizeof(*a));
    a->bundle = bundle;
    a->funcs = (CallableFuncFacts *) xr_calloc(bundle->nfunc_plans, sizeof(*a->funcs));
    a->reachable_funcs = (uint8_t *) xr_calloc(bundle->nfunc_plans ? bundle->nfunc_plans : 1, 1);
    a->module_slots = (CallableSet **) xr_calloc(bundle->nmodules, sizeof(*a->module_slots));
    if ((bundle->nfunc_plans && (!a->funcs || !a->reachable_funcs)) ||
        (bundle->nmodules && !a->module_slots))
        return false;
    if (bundle->global_evidence_plan.evidence &&
        bundle->global_evidence_plan.evidence->nbodies > 0) {
        a->reachable_bodies =
            (uint8_t *) xr_calloc(bundle->global_evidence_plan.evidence->nbodies, sizeof(uint8_t));
        if (!a->reachable_bodies)
            return false;
    }
    for (uint32_t fi = 0; fi < bundle->nfunc_plans; fi++) {
        CallableFuncFacts *facts = &a->funcs[fi];
        facts->func = bundle->func_plans[fi].func;
        facts->value_count = callable_func_value_count(facts->func);
        facts->values = (CallableSet *) xr_calloc(facts->value_count, sizeof(CallableSet));
        facts->effect_bits = callable_body_effects(bundle, facts->func);
        if (facts->value_count && !facts->values)
            return false;
        if (facts->func) {
            bool has_portable_parallel_suspend = false;
            bool has_aot_suspend = false;
            for (uint32_t bi = 0; bi < facts->func->nblocks; bi++) {
                const XiBlock *block = facts->func->blocks[bi];
                if (!block)
                    continue;
                for (uint32_t vi = 0; vi < block->nvalues; vi++) {
                    const XiValue *value = block->values[vi];
                    if (value && (value->op == XI_PAR_FOR || value->op == XI_PAR_MAP ||
                                  value->op == XI_PAR_REDUCE))
                        has_portable_parallel_suspend = true;
                    if (callable_aot_value_may_suspend(facts->func, value))
                        has_aot_suspend = true;
                }
            }
            if (has_portable_parallel_suspend && !has_aot_suspend)
                facts->effect_bits &= ~XG_BODY_MAY_SUSPEND;
            if (has_aot_suspend)
                facts->effect_bits |= XG_BODY_MAY_SUSPEND;
        }
    }
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules[mi];
        if (!mod || mod->nslots == 0)
            continue;
        a->module_slots[mi] = (CallableSet *) xr_calloc(mod->nslots, sizeof(CallableSet));
        if (!a->module_slots[mi])
            return false;
        for (uint16_t si = 0; si < mod->nslots; si++) {
            const XiFunc *slot_func = mod->slot_funcs ? mod->slot_funcs[si] : NULL;
            int target_index = xaot_callable_func_has_executable_body_plan(bundle, slot_func)
                                   ? callable_func_index(a, slot_func)
                                   : -1;
            if (target_index >= 0 &&
                !callable_set_add(&a->module_slots[mi][si], (uint32_t) target_index, NULL))
                return false;
        }
    }
    return true;
}

static void callable_analysis_free(CallableAnalysis *a) {
    if (!a)
        return;
    if (a->funcs) {
        for (uint32_t fi = 0; fi < a->bundle->nfunc_plans; fi++) {
            for (uint32_t vi = 0; vi < a->funcs[fi].value_count; vi++)
                callable_set_free(&a->funcs[fi].values[vi]);
            xr_free(a->funcs[fi].values);
            callable_set_free(&a->funcs[fi].returns);
        }
    }
    if (a->module_slots) {
        for (uint32_t mi = 0; mi < a->bundle->nmodules; mi++) {
            const XiModule *mod = a->bundle->modules[mi];
            if (a->module_slots[mi] && mod) {
                for (uint16_t si = 0; si < mod->nslots; si++)
                    callable_set_free(&a->module_slots[mi][si]);
            }
            xr_free(a->module_slots[mi]);
        }
    }
    for (uint32_t i = 0; i < a->nstorage; i++)
        callable_set_free(&a->storage[i].targets);
    xr_free(a->storage);
    xr_free(a->reachable_bodies);
    xr_free(a->reachable_funcs);
    xr_free(a->module_slots);
    xr_free(a->funcs);
    memset(a, 0, sizeof(*a));
}

static bool callable_analysis_solve(CallableAnalysis *a) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t fi = 0; fi < a->bundle->nfunc_plans; fi++) {
            if (!callable_propagate_func(a, &a->funcs[fi], &changed))
                return false;
        }
    }

    changed = true;
    while (changed) {
        changed = false;
        for (uint32_t fi = 0; fi < a->bundle->nfunc_plans; fi++) {
            CallableFuncFacts *facts = &a->funcs[fi];
            const XiFunc *func = facts->func;
            for (uint32_t bi = 0; func && bi < func->nblocks; bi++) {
                const XiBlock *block = func->blocks[bi];
                if (!block)
                    continue;
                for (uint32_t vi = 0; vi < block->nvalues; vi++) {
                    const XiValue *call = block->values[vi];
                    const XiFunc *direct = callable_resolve_direct_target(a->bundle, func, call);
                    CallableFuncFacts *direct_facts = callable_func_facts(a, direct);
                    if (direct_facts) {
                        uint32_t next = facts->effect_bits | direct_facts->effect_bits;
                        if (next != facts->effect_bits) {
                            facts->effect_bits = next;
                            changed = true;
                        }
                    }
                    if (!callable_call_is_function_value(a->bundle, func, call))
                        continue;
                    CallableSet *targets = callable_value_set(a, func, call->args[0]);
                    for (uint32_t ti = 0; targets && ti < targets->count; ti++) {
                        uint32_t effects = a->funcs[targets->items[ti]].effect_bits;
                        uint32_t next = facts->effect_bits | effects;
                        if (next != facts->effect_bits) {
                            facts->effect_bits = next;
                            changed = true;
                        }
                    }
                }
            }
        }
    }
    return true;
}

static bool callable_refresh_reachable_funcs(CallableAnalysis *a, bool *changed) {
    const XgGlobalEvidence *ev = a && a->bundle ? a->bundle->global_evidence_plan.evidence : NULL;
    if (!a)
        return false;
    if (!ev || !a->reachable_bodies) {
        for (uint32_t fi = 0; fi < a->bundle->nfunc_plans; fi++) {
            if (!a->reachable_funcs[fi]) {
                a->reachable_funcs[fi] = 1;
                if (changed)
                    *changed = true;
            }
        }
        return true;
    }
    for (uint32_t fi = 0; fi < a->bundle->nfunc_plans; fi++) {
        const XiFunc *func = a->funcs[fi].func;
        if (!func || func->xg_body_func_id == XG_NO_ID)
            continue;
        for (uint32_t bi = 0; bi < ev->nbodies; bi++) {
            if (ev->bodies[bi].func_id == func->xg_body_func_id && a->reachable_bodies[bi] &&
                !a->reachable_funcs[fi]) {
                a->reachable_funcs[fi] = 1;
                if (changed)
                    *changed = true;
                break;
            }
        }
    }
    return true;
}

static bool callable_mark_reachable_func(CallableAnalysis *a, const XiFunc *func, bool *changed) {
    const XgGlobalEvidence *ev = a && a->bundle ? a->bundle->global_evidence_plan.evidence : NULL;
    int index = callable_func_index(a, func);
    if (!a || !func || index < 0)
        return true;
    if (a->reachable_funcs[index])
        return true;
    if (!ev || !a->reachable_bodies || func->xg_body_func_id == XG_NO_ID) {
        if (!a->reachable_funcs[index]) {
            a->reachable_funcs[index] = 1;
            if (changed)
                *changed = true;
        }
        return true;
    }
    for (uint32_t bi = 0; bi < ev->nbodies; bi++) {
        if (ev->bodies[bi].func_id != func->xg_body_func_id)
            continue;
        if (!a->reachable_bodies[bi]) {
            a->reachable_bodies[bi] = 1;
            if (changed)
                *changed = true;
        }
        break;
    }
    /* Do not replay pre-optimization evidence call edges here. Inlining may
     * have removed the only call to a specialized HOF body; treating that
     * stale edge as executable makes its now-dead open callback parameter
     * reject the build. The fixed point below walks the final Xi graph and is
     * the authoritative source of executable direct/closure edges. */
    return callable_refresh_reachable_funcs(a, changed);
}

static bool callable_mark_exported_class_methods(CallableAnalysis *a, const XiModule *mod,
                                                 const XiClassData *cd, bool *changed) {
    if (!a || !mod || !mod->init || !cd || !cd->child_idx)
        return true;
    uint16_t total = (uint16_t) (cd->ninst + cd->nstat);
    if (total > cd->nmethod)
        total = cd->nmethod;
    for (uint16_t mi = 0; mi < total; mi++) {
        uint16_t child = cd->child_idx[mi];
        if (child < mod->init->nchildren &&
            !callable_mark_reachable_func(a, mod->init->children[child], changed))
            return false;
    }
    return true;
}

static bool callable_func_is_generated_generic_specialization(const XaotBundle *bundle,
                                                              const XiFunc *func) {
    if (!bundle || !func || func->xg_body_func_id == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < bundle->ngeneric_body_plans; i++) {
        if (bundle->generic_body_plans[i].specialized_body_func_id == func->xg_body_func_id)
            return true;
    }
    return false;
}

static const XiFunc *callable_array_hof_target_is_exact(
    const CallableAnalysis *analysis, const XiFunc *owner,
    const XiValue *value) {
    const XaotBundle *bundle = analysis ? analysis->bundle : NULL;
    const XrSemanticPlan *semantic = bundle && owner
        ? xaot_bundle_program_semantic_for_func(bundle, owner, NULL) : NULL;
    const XrTargetPlan *target = semantic
        ? xaot_bundle_program_target_plan(bundle) : NULL;
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    char error[192] = {0};
    if (!analysis || !bundle || !owner || !value || !target || !semantic ||
        !xr_target_plan_is_verified(target) ||
        (xr_target_plan_completed_family_mask(target) &
         XR_TARGET_FAMILY_ARRAY_HOF_RESULT_STORAGE) == 0 ||
        !xr_aot_scalar_semantic_value_id(target, owner, value,
                                         &semantic_function, &semantic_value,
                                         error, sizeof(error)))
        return NULL;

    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate =
            xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->function != semantic_function ||
            candidate->result_value != semantic_value)
            continue;
        if (operation)
            return NULL;
        operation = candidate;
        operation_index = i;
    }
    uint8_t expected_target_kind = XR_TARGET_ARRAY_HOF_NONE;
    uint8_t expected_live_kind = XI_ARRAY_HOF_NONE;
    if (operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_HOF) {
        switch (operation->array_hof_kind) {
            case XR_SEM_ARRAY_HOF_MAP:
                expected_target_kind = XR_TARGET_ARRAY_HOF_MAP;
                expected_live_kind = XI_ARRAY_HOF_MAP;
                break;
            case XR_SEM_ARRAY_HOF_FILTER:
                expected_target_kind = XR_TARGET_ARRAY_HOF_FILTER;
                expected_live_kind = XI_ARRAY_HOF_FILTER;
                break;
            case XR_SEM_ARRAY_HOF_REDUCE:
                expected_target_kind = XR_TARGET_ARRAY_HOF_REDUCE;
                expected_live_kind = XI_ARRAY_HOF_REDUCE;
                break;
            default: break;
        }
    }
    if (!operation || expected_target_kind == XR_TARGET_ARRAY_HOF_NONE ||
        value->array_hof_kind != expected_live_kind)
        return NULL;

    const XrTargetCallRecord *call = NULL;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return NULL;
        call = &calls[i];
    }
    if (!call || call->caller_function != semantic_function ||
        call->callee_function != operation->callable_function ||
        call->result_value != semantic_value ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_ARRAY_HOF ||
        call->target_kind != XR_TARGET_CALL_TARGET_ARRAY_HOF ||
        call->array_hof_kind != expected_target_kind ||
        call->argument_count !=
            (expected_target_kind == XR_TARGET_ARRAY_HOF_REDUCE ? 3u : 2u) ||
        call->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE ||
        call->array_result_element_storage == XR_TARGET_ARRAY_STORAGE_NONE)
        return NULL;

    const XiFunc *callee = NULL;
    for (uint32_t i = 0; i < bundle->nfunc_plans; i++) {
        const XiFunc *candidate = bundle->func_plans[i].func;
        if (!candidate || candidate->semantic_plan != semantic ||
            candidate->semantic_plan_function_index != call->callee_function)
            continue;
        if (callee)
            return NULL;
        callee = candidate;
    }
    return callee;
}

static bool callable_analysis_solve_reachability(CallableAnalysis *a) {
    const XaotBundle *bundle = a ? a->bundle : NULL;
    bool changed = false;
    if (!a || !bundle)
        return false;
    if (!bundle->global_evidence_plan.evidence)
        return callable_refresh_reachable_funcs(a, &changed);

    /* Every module initializer executes in topological order before the entry
     * body. Seed all of them so their final Xi direct edges participate in the
     * same precise reachability closure as the entry initializer. */
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *module = bundle->modules[mi];
        if (module && module->init && !callable_mark_reachable_func(a, module->init, &changed))
            return false;
    }
    for (uint32_t fi = 0; fi < bundle->nfunc_plans; fi++) {
        const XiFunc *func = a->funcs[fi].func;
        if (!func)
            continue;
        if (func->export_plan || func->link_plan || func->entry_plan ||
            (func->is_generic_template &&
             xaot_callable_func_has_executable_body_plan(bundle, func))) {
            if (!callable_mark_reachable_func(a, func, &changed))
                return false;
        }
    }
    if (bundle->artifact_kind == XAOT_ARTIFACT_SHARED_LIBRARY) {
        for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
            const XiModule *mod = bundle->modules[mi];
            if (!mod)
                continue;
            for (uint16_t ei = 0; ei < mod->nexports; ei++) {
                const XiModuleExport *exp = &mod->exports[ei];
                if (exp->function &&
                    !callable_func_is_generated_generic_specialization(bundle, exp->function) &&
                    xaot_callable_func_has_executable_body_plan(bundle, exp->function) &&
                    !callable_mark_reachable_func(a, exp->function, &changed))
                    return false;
                if (exp->class_data &&
                    !callable_mark_exported_class_methods(a, mod, exp->class_data, &changed))
                    return false;
            }
        }
    }

    do {
        changed = false;
        if (!callable_refresh_reachable_funcs(a, &changed))
            return false;
        for (uint32_t fi = 0; fi < bundle->nfunc_plans; fi++) {
            const XiFunc *func = a->funcs[fi].func;
            if (!a->reachable_funcs[fi] || !func)
                continue;
            for (uint32_t bi = 0; bi < func->nblocks; bi++) {
                const XiBlock *block = func->blocks[bi];
                if (!block)
                    continue;
                for (uint32_t vi = 0; vi < block->nvalues; vi++) {
                    const XiValue *call = block->values[vi];
                    /* Creating a top-level closure in module init does not by
                     * itself execute its body. Direct calls and closed
                     * function-value target sets below add the real edge. */
                    const XiFunc *direct = callable_resolve_direct_target(a->bundle, func, call);
                    if (direct && !callable_mark_reachable_func(a, direct, &changed))
                        return false;
                    const XiFunc *hof_target =
                        callable_array_hof_target_is_exact(a, func, call);
                    if (hof_target &&
                        !callable_mark_reachable_func(a, hof_target, &changed))
                        return false;
                    if (call->op == XI_GEN_CALL) {
                        const CallableSet *targets = callable_generator_targets(a, func, call);
                        if (!targets || targets->count == 0)
                            return false;
                        for (uint32_t ti = 0; ti < targets->count; ti++) {
                            const XiFunc *target = a->funcs[targets->items[ti]].func;
                            if (!callable_mark_reachable_func(a, target, &changed))
                                return false;
                        }
                    }
                    if (call->op == XI_GO || call->op == XI_THREAD_SPAWN) {
                        const CallableSet *targets = callable_spawn_targets(a, func, call);
                        if (!targets || targets->count == 0)
                            return false;
                        for (uint32_t ti = 0; ti < targets->count; ti++) {
                            const XiFunc *target = a->funcs[targets->items[ti]].func;
                            if (!callable_mark_reachable_func(a, target, &changed))
                                return false;
                        }
                    }
                    if (!callable_call_is_function_value(a->bundle, func, call))
                        continue;
                    const CallableSet *targets = callable_value_set(a, func, call->args[0]);
                    for (uint32_t ti = 0; targets && ti < targets->count; ti++) {
                        const XiFunc *target = a->funcs[targets->items[ti]].func;
                        if (!callable_mark_reachable_func(a, target, &changed))
                            return false;
                    }
                }
            }
        }
    } while (changed);
    return true;
}

static bool callable_append_plan(XaotBundle *bundle, const CallableAnalysis *a,
                                 uint32_t owner_index, const XiValue *call) {
    const CallableFuncFacts *owner = &a->funcs[owner_index];
    const CallableSet *targets = &owner->values[call->args[0]->id];
    XaotCallableInvokePlan *plan;
    uint32_t effect_bits = 0;
    uint64_t signature = xaot_type_fingerprint(call->args[0]->type);
    uint32_t evidence = XAOT_CALLABLE_EV_SIGNATURE | XAOT_CALLABLE_EV_XI_FLOW;

    if (!callable_reserve((void **) &bundle->callable_invoke_plans,
                          &bundle->callable_invoke_plan_cap, bundle->ncallable_invoke_plans + 1,
                          sizeof(XaotCallableInvokePlan)))
        return false;
    if (targets->count > UINT16_MAX ||
        !callable_reserve(
            (void **) &bundle->callable_target_cases, &bundle->callable_target_case_cap,
            bundle->ncallable_target_cases + targets->count, sizeof(XaotCallableTargetCase)))
        return false;

    plan = &bundle->callable_invoke_plans[bundle->ncallable_invoke_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->owner = owner->func;
    plan->call = call;
    plan->signature_key = signature;
    plan->target_start = bundle->ncallable_target_cases;
    plan->target_count = (uint16_t) targets->count;
    plan->evidence = evidence;

    for (uint32_t ti = 0; ti < targets->count; ti++) {
        uint32_t target_index = targets->items[ti];
        XaotCallableTargetCase *target =
            &bundle->callable_target_cases[bundle->ncallable_target_cases++];
        memset(target, 0, sizeof(*target));
        target->target_func = a->funcs[target_index].func;
        target->signature_key = signature;
        target->target_id = callable_target_id(bundle, target_index);
        target->effect_bits = a->funcs[target_index].effect_bits;
        effect_bits |= target->effect_bits;
    }
    plan->effect_bits = effect_bits;
    if (targets->count == 0) {
        plan->action = XAOT_CALLABLE_REJECT;
        plan->unproven_reason = XAOT_CALLABLE_UNPROVEN_EMPTY_TARGET_SET;
    } else {
        plan->evidence |= XAOT_CALLABLE_EV_CLOSED_TARGET_SET | XAOT_CALLABLE_EV_TARGET_EFFECTS;
        plan->unproven_reason = XAOT_CALLABLE_PROVEN;
        if (targets->count == 1) {
            plan->action = (effect_bits & XG_BODY_MAY_SUSPEND) ? XAOT_CALLABLE_DIRECT_SUSPEND
                                                               : XAOT_CALLABLE_DIRECT_SYNC;
        } else {
            plan->action = XAOT_CALLABLE_TARGET_SWITCH;
        }
    }
    return true;
}

static bool callable_materialize(XaotBundle *bundle, const CallableAnalysis *a) {
    for (uint32_t fi = 0; fi < bundle->nfunc_plans; fi++) {
        bundle->func_plans[fi].reachable = a->reachable_funcs[fi] != 0;
        bundle->func_plans[fi].may_suspend = (a->funcs[fi].effect_bits & XG_BODY_MAY_SUSPEND) != 0;
    }

    for (uint32_t fi = 0; fi < bundle->nfunc_plans; fi++) {
        XiFunc *func = bundle->func_plans[fi].func;
        if (!xaot_callable_func_has_executable_body_plan(bundle, func))
            continue;
        for (uint32_t bi = 0; func && bi < func->nblocks; bi++) {
            XiBlock *block = func->blocks[bi];
            if (!block)
                continue;
            for (uint32_t vi = 0; vi < block->nvalues; vi++) {
                XiValue *call = block->values[vi];
                if (!callable_call_is_function_value(bundle, func, call))
                    continue;
                const CallableSet *targets = call->args[0]->id < a->funcs[fi].value_count
                                                 ? &a->funcs[fi].values[call->args[0]->id]
                                                 : NULL;
                /* A guarded optional call whose entire closed-world value set
                 * is the null constant has no executable invocation edge. */
                if (targets && targets->count == 0 && targets->may_be_null)
                    continue;
                /* A proven target set is useful even when the independent
                 * body-reachability graph has not yet discovered the owner:
                 * projecting its effects may be what turns an enclosing
                 * callback chain into a coroutine.  An open set rejects only
                 * at a proven-reachable boundary; dead library helpers do not
                 * poison an executable whole-program build. */
                if ((!targets || targets->count == 0) && !a->reachable_funcs[fi])
                    continue;
                if (!callable_append_plan(bundle, a, fi, call))
                    return false;
                const XaotCallableInvokePlan *plan =
                    &bundle->callable_invoke_plans[bundle->ncallable_invoke_plans - 1];
                if ((plan->effect_bits & XG_BODY_MAY_SUSPEND) != 0)
                    call->flags |= XI_FLAG_MAY_SUSPEND;
            }
        }
    }
    return true;
}

XR_FUNC bool xaot_callable_plans_build(XaotBundle *bundle) {
    CallableAnalysis analysis;
    bool ok;
    if (!bundle)
        return false;
    bundle->has_callable_reachability = false;
    xr_free(bundle->callable_invoke_plans);
    xr_free(bundle->callable_target_cases);
    bundle->callable_invoke_plans = NULL;
    bundle->ncallable_invoke_plans = 0;
    bundle->callable_invoke_plan_cap = 0;
    bundle->callable_target_cases = NULL;
    bundle->ncallable_target_cases = 0;
    bundle->callable_target_case_cap = 0;
    if (!callable_analysis_init(&analysis, bundle)) {
        callable_analysis_free(&analysis);
        return false;
    }
    ok = callable_analysis_solve(&analysis) && callable_analysis_solve_reachability(&analysis);
    if (ok)
        ok = callable_materialize(bundle, &analysis);
    callable_analysis_free(&analysis);
    bundle->has_callable_reachability = ok;
    return ok;
}

/* Human-readable provenance for an indirect callee the closed-world planner
 * cannot resolve. The op is the callee value's op: an upvalue capture, a copy
 * (a map lookup lowers to one), or a collection element read. */
static const char *callable_unprovable_provenance_hint(uint16_t op) {
    switch ((XiOp) op) {
        case XI_LOAD_UPVAL:
            return "a captured value (an upvalue)";
        case XI_COPY:
            return "a copied value, such as a map lookup result";
        case XI_INDEX_GET:
            return "an element read from a collection";
        default:
            return "an opaque data-flow path";
    }
}

static bool callable_verify_error(char *errbuf, size_t errbuf_len, const char *message) {
    if (errbuf && errbuf_len)
        snprintf(errbuf, errbuf_len, "%s", message);
    return false;
}

static bool callable_rederive_matches(const XaotBundle *bundle, char *errbuf, size_t errbuf_len) {
    XaotBundle expected = *bundle;
    expected.func_plans = NULL;
    if (bundle->nfunc_plans > 0) {
        expected.func_plans =
            (XaotFuncPlan *) xr_malloc(sizeof(*expected.func_plans) * bundle->nfunc_plans);
        if (!expected.func_plans)
            return callable_verify_error(errbuf, errbuf_len,
                                         "failed to copy AOT function effects for rederivation");
        memcpy(expected.func_plans, bundle->func_plans,
               sizeof(*expected.func_plans) * bundle->nfunc_plans);
    }
    expected.callable_invoke_plans = NULL;
    expected.ncallable_invoke_plans = 0;
    expected.callable_invoke_plan_cap = 0;
    expected.callable_target_cases = NULL;
    expected.ncallable_target_cases = 0;
    expected.callable_target_case_cap = 0;
    if (!xaot_callable_plans_build(&expected)) {
        xr_free(expected.func_plans);
        return callable_verify_error(errbuf, errbuf_len,
                                     "failed to rederive AOT callable invoke plans");
    }

    bool verify_reachability = false;
    if (bundle->has_callable_reachability) {
        for (uint32_t i = 0; i < bundle->nlink_dependency_plans; i++) {
            if (bundle->link_dependency_plans[i].owner_func_id != XG_NO_ID) {
                verify_reachability = true;
                break;
            }
        }
    }
    bool matches = expected.ncallable_invoke_plans == bundle->ncallable_invoke_plans &&
                   expected.ncallable_target_cases == bundle->ncallable_target_cases;
    for (uint32_t i = 0; matches && i < bundle->nfunc_plans; i++)
        matches = expected.func_plans[i].func == bundle->func_plans[i].func &&
                  (!verify_reachability ||
                   expected.func_plans[i].reachable == bundle->func_plans[i].reachable) &&
                  expected.func_plans[i].may_suspend == bundle->func_plans[i].may_suspend;
    for (uint32_t i = 0; matches && i < expected.ncallable_invoke_plans; i++) {
        const XaotCallableInvokePlan *a = &expected.callable_invoke_plans[i];
        const XaotCallableInvokePlan *b = &bundle->callable_invoke_plans[i];
        matches = a->owner == b->owner && a->call == b->call &&
                  a->signature_key == b->signature_key && a->effect_bits == b->effect_bits &&
                  a->target_start == b->target_start && a->target_count == b->target_count &&
                  a->action == b->action && a->evidence == b->evidence &&
                  a->unproven_reason == b->unproven_reason;
    }
    for (uint32_t i = 0; matches && i < expected.ncallable_target_cases; i++) {
        const XaotCallableTargetCase *a = &expected.callable_target_cases[i];
        const XaotCallableTargetCase *b = &bundle->callable_target_cases[i];
        matches = a->target_func == b->target_func && a->signature_key == b->signature_key &&
                  a->target_id == b->target_id && a->effect_bits == b->effect_bits;
    }
    xr_free(expected.callable_invoke_plans);
    xr_free(expected.callable_target_cases);
    xr_free(expected.func_plans);
    return matches ? true
                   : callable_verify_error(errbuf, errbuf_len,
                                           "AOT callable invoke plans differ from rederivation");
}

XR_FUNC bool xaot_callable_plans_verify(const XaotBundle *bundle, char *errbuf, size_t errbuf_len) {
    if (!bundle)
        return callable_verify_error(errbuf, errbuf_len, "missing AOT callable bundle");
    uint32_t seen_targets = 0;
    for (uint32_t i = 0; i < bundle->ncallable_invoke_plans; i++) {
        const XaotCallableInvokePlan *plan = &bundle->callable_invoke_plans[i];
        uint32_t effects = 0;
        if (!plan->owner || !callable_call_is_function_value(bundle, plan->owner, plan->call))
            return callable_verify_error(errbuf, errbuf_len,
                                         "AOT callable invoke plan has stale callsite");
        if (plan->target_start != seen_targets ||
            plan->target_start + plan->target_count > bundle->ncallable_target_cases)
            return callable_verify_error(errbuf, errbuf_len, "AOT callable target range is stale");
        for (uint16_t ti = 0; ti < plan->target_count; ti++) {
            const XaotCallableTargetCase *target =
                xaot_bundle_callable_target_case(bundle, plan, ti);
            if (!target || !target->target_func || target->signature_key != plan->signature_key)
                return callable_verify_error(errbuf, errbuf_len,
                                             "AOT callable target case is stale");
            if (!xaot_callable_func_has_executable_body_plan(bundle, target->target_func))
                return callable_verify_error(
                    errbuf, errbuf_len, "AOT callable target is an unselected generic template");
            for (uint16_t tj = 0; tj < ti; tj++) {
                const XaotCallableTargetCase *prior =
                    xaot_bundle_callable_target_case(bundle, plan, tj);
                if (prior && prior->target_id == target->target_id)
                    return callable_verify_error(errbuf, errbuf_len,
                                                 "AOT callable target set is not unique");
            }
            effects |= target->effect_bits;
        }
        if (effects != plan->effect_bits)
            return callable_verify_error(errbuf, errbuf_len,
                                         "AOT callable effect composition is stale");
        if (plan->target_count == 0) {
            if (plan->action != XAOT_CALLABLE_REJECT ||
                plan->unproven_reason == XAOT_CALLABLE_PROVEN)
                return callable_verify_error(errbuf, errbuf_len,
                                             "open AOT callable boundary did not reject");
            if (errbuf && errbuf_len)
                snprintf(errbuf, errbuf_len,
                         "native compilation cannot prove the target of an indirect call at "
                         "line %u: the callee comes from %s, whose target set the closed-world "
                         "AOT backend cannot determine statically. Pass the function directly as "
                         "an argument, or bind it to a local initialized from a lambda or a named "
                         "function so the target is provable. The bytecode VM runs this program "
                         "unchanged.",
                         plan->call->line,
                         callable_unprovable_provenance_hint(plan->call->args[0]->op));
            return false;
        } else {
            uint8_t expected = plan->target_count > 1
                                   ? XAOT_CALLABLE_TARGET_SWITCH
                                   : ((effects & XG_BODY_MAY_SUSPEND) ? XAOT_CALLABLE_DIRECT_SUSPEND
                                                                      : XAOT_CALLABLE_DIRECT_SYNC);
            if (plan->action != expected || plan->unproven_reason != XAOT_CALLABLE_PROVEN ||
                (plan->evidence & (XAOT_CALLABLE_EV_CLOSED_TARGET_SET | XAOT_CALLABLE_EV_SIGNATURE |
                                   XAOT_CALLABLE_EV_TARGET_EFFECTS | XAOT_CALLABLE_EV_XI_FLOW)) !=
                    (XAOT_CALLABLE_EV_CLOSED_TARGET_SET | XAOT_CALLABLE_EV_SIGNATURE |
                     XAOT_CALLABLE_EV_TARGET_EFFECTS | XAOT_CALLABLE_EV_XI_FLOW))
                return callable_verify_error(errbuf, errbuf_len,
                                             "AOT callable invoke action is stale");
        }
        if (((plan->effect_bits & XG_BODY_MAY_SUSPEND) != 0) !=
            ((plan->call->flags & XI_FLAG_MAY_SUSPEND) != 0))
            return callable_verify_error(errbuf, errbuf_len,
                                         "AOT callable suspend effect was not projected to Xi");
        seen_targets += plan->target_count;
    }
    if (seen_targets != bundle->ncallable_target_cases)
        return callable_verify_error(errbuf, errbuf_len,
                                     "AOT callable target table has unreachable rows");
    return callable_rederive_matches(bundle, errbuf, errbuf_len);
}
