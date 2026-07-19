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
    if (!func || !func->is_generic_template)
        return func != NULL;
    if (!bundle || func->xg_body_func_id == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < bundle->ngeneric_body_plans; i++) {
        const XaotGenericBodyPlan *plan = &bundle->generic_body_plans[i];
        XgFuncId selected = XG_NO_ID;
        switch ((XaotGenericBodyAction) plan->action) {
            case XAOT_GENERIC_BODY_CLONE:
                selected = plan->specialized_body_func_id;
                break;
            case XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY:
            case XAOT_GENERIC_BODY_DIRECT_CONSTRAINT_CALL:
                selected = plan->origin_body_func_id;
                break;
            case XAOT_GENERIC_BODY_REJECT:
                break;
        }
        if (selected == func->xg_body_func_id)
            return true;
    }
    return false;
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
    return (callee->type && XR_TYPE_IS_FUNCTION(callee->type)) || callee->op == XI_LOAD_UPVAL;
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
    if ((value->op == XI_CLOSURE_NEW ||
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

    if (value->op == XI_PHI || xi_copy_is_identity_alias(value) || value->op == XI_MOVE ||
        value->op == XI_BOX || value->op == XI_UNBOX || value->op == XI_CONVERT ||
        value->op == XI_CHECKTYPE) {
        for (uint16_t i = 0; i < value->nargs; i++) {
            CallableSet *src = callable_value_set(a, func, value->args[i]);
            if (src && !callable_set_union(dst, src, changed))
                return false;
        }
    }

    if (value->op == XI_LOAD_UPVAL && value->aux_int >= 0 && value->aux_int < func->ncaptures) {
        const XiCapture *capture = &func->captures[value->aux_int];
        if (capture->value && func->parent_func) {
            CallableSet *src = callable_value_set(a, func->parent_func, capture->value);
            if (src && !callable_set_union(dst, src, changed))
                return false;
        }
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
            for (uint32_t bi = 0; bi < facts->func->nblocks; bi++) {
                const XiBlock *block = facts->func->blocks[bi];
                if (!block)
                    continue;
                for (uint32_t vi = 0; vi < block->nvalues; vi++) {
                    const XiValue *value = block->values[vi];
                    if (value && xi_coro_is_suspend_point(facts->func, value, NULL))
                        facts->effect_bits |= XG_BODY_MAY_SUSPEND;
                }
            }
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
    if (!xg_body_reachability_mark_closed_world_calls(ev, func->xg_body_func_id,
                                                      a->reachable_bodies, ev->nbodies))
        return false;
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

static bool callable_analysis_solve_reachability(CallableAnalysis *a) {
    const XaotBundle *bundle = a ? a->bundle : NULL;
    bool changed = false;
    if (!a || !bundle)
        return false;
    if (!bundle->global_evidence_plan.evidence)
        return callable_refresh_reachable_funcs(a, &changed);

    if (bundle->entry_module < bundle->nmodules) {
        const XiModule *entry = bundle->modules[bundle->entry_module];
        if (entry && entry->init && !callable_mark_reachable_func(a, entry->init, &changed))
            return false;
    }
    for (uint32_t fi = 0; fi < bundle->nfunc_plans; fi++) {
        const XiFunc *func = a->funcs[fi].func;
        if (!func)
            continue;
        if (func->c_export || func->aot_used || func->aot_naked || func->aot_weak ||
            func->aot_section || (func->aot_interrupt_abi && func->aot_interrupt_abi[0]) ||
            (func->is_generic_template &&
             xaot_callable_func_has_executable_body_plan(bundle, func))) {
            if (!callable_mark_reachable_func(a, func, &changed))
                return false;
        }
    }
    if (!bundle->emit_program_main) {
        for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
            const XiModule *mod = bundle->modules[mi];
            if (!mod)
                continue;
            for (uint16_t ei = 0; ei < mod->nexports; ei++) {
                const XiModuleExport *exp = &mod->exports[ei];
                if (exp->function && !callable_mark_reachable_func(a, exp->function, &changed))
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
                    if ((call->op == XI_CLOSURE_NEW ||
                         (call->op == XI_STACK_ALLOC && call->aux_int == XI_CLOSURE_NEW)) &&
                        call->aux &&
                        !callable_mark_reachable_func(a, (const XiFunc *) call->aux, &changed))
                        return false;
                    const XiFunc *direct = callable_resolve_direct_target(a->bundle, func, call);
                    if (direct && !callable_mark_reachable_func(a, direct, &changed))
                        return false;
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
    for (uint32_t fi = 0; fi < bundle->nfunc_plans; fi++)
        bundle->func_plans[fi].may_suspend = (a->funcs[fi].effect_bits & XG_BODY_MAY_SUSPEND) != 0;

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
    return ok;
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

    bool matches = expected.ncallable_invoke_plans == bundle->ncallable_invoke_plans &&
                   expected.ncallable_target_cases == bundle->ncallable_target_cases;
    for (uint32_t i = 0; matches && i < bundle->nfunc_plans; i++)
        matches = expected.func_plans[i].func == bundle->func_plans[i].func &&
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
                         "open AOT callable target set owner=%s call=v%u line=%u callee-op=%s "
                         "callee-id=v%u callee-aux=%" PRId64 " reason=%u",
                         plan->owner->name ? plan->owner->name : "?", plan->call->id,
                         plan->call->line, xi_op_name((XiOp) plan->call->args[0]->op),
                         plan->call->args[0]->id, plan->call->args[0]->aux_int,
                         plan->unproven_reason);
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
