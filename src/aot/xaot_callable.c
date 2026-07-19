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
#include "xaot_struct_name.h"
#include "../base/xglobal_indices.h"
#include "../base/xmalloc.h"
#include "../ir/xi_coro_analyze.h"
#include <stdio.h>
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

static bool callable_call_is_function_value(const XiValue *call) {
    /* XI_CALL's operand zero is the callable by construction.  Backend rep
     * selection may erase the operand's source function type (notably after a
     * module/shared load), so target-flow completeness must not depend on that
     * late type annotation surviving. */
    return call && call->op == XI_CALL && call->nargs >= 1 && call->args[0];
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
    if (target) {
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

    if (callable_call_is_function_value(value)) {
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
    a->module_slots = (CallableSet **) xr_calloc(bundle->nmodules, sizeof(*a->module_slots));
    if ((bundle->nfunc_plans && !a->funcs) || (bundle->nmodules && !a->module_slots))
        return false;
    for (uint32_t fi = 0; fi < bundle->nfunc_plans; fi++) {
        CallableFuncFacts *facts = &a->funcs[fi];
        facts->func = bundle->func_plans[fi].func;
        facts->value_count = facts->func ? facts->func->next_value_id : 0;
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
            int target_index = mod->slot_funcs ? callable_func_index(a, mod->slot_funcs[si]) : -1;
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
                    if (!callable_call_is_function_value(call))
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
        XiFunc *func = bundle->func_plans[fi].func;
        for (uint32_t bi = 0; func && bi < func->nblocks; bi++) {
            XiBlock *block = func->blocks[bi];
            if (!block)
                continue;
            for (uint32_t vi = 0; vi < block->nvalues; vi++) {
                XiValue *call = block->values[vi];
                if (!callable_call_is_function_value(call))
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
    ok = callable_analysis_solve(&analysis) && callable_materialize(bundle, &analysis);
    callable_analysis_free(&analysis);
    return ok;
}

static bool callable_verify_error(char *errbuf, size_t errbuf_len, const char *message) {
    if (errbuf && errbuf_len)
        snprintf(errbuf, errbuf_len, "%s", message);
    return false;
}

XR_FUNC bool xaot_callable_plans_verify(const XaotBundle *bundle, char *errbuf, size_t errbuf_len) {
    if (!bundle)
        return callable_verify_error(errbuf, errbuf_len, "missing AOT callable bundle");
    uint32_t seen_targets = 0;
    for (uint32_t i = 0; i < bundle->ncallable_invoke_plans; i++) {
        const XaotCallableInvokePlan *plan = &bundle->callable_invoke_plans[i];
        uint32_t effects = 0;
        if (!plan->owner || !callable_call_is_function_value(plan->call))
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
    return true;
}
