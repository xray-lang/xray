/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_bundle.c - AOT sidecar bundle plan
 */

#include "xaot_bundle.h"
#include "../base/xmalloc.h"
#include "../base/xmemstream.h"
#include "../ir/xi_op_name.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ========== Pointer index (XiValue / XiFunc pointer -> plan row) ========== */

static uint32_t xaot_ptr_hash(const void *p) {
    uint64_t x = (uint64_t) (uintptr_t) p;
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return (uint32_t) x;
}

static bool xaot_ptr_index_rehash(XaotPtrIndex *ix, uint32_t new_cap) {
    XaotPtrIndexSlot *slots = (XaotPtrIndexSlot *) xr_calloc(new_cap, sizeof(XaotPtrIndexSlot));
    uint32_t mask = new_cap - 1;
    uint32_t i;
    if (!slots)
        return false;
    for (i = 0; i < ix->cap; i++) {
        uint32_t j;
        if (!ix->slots[i].key)
            continue;
        j = xaot_ptr_hash(ix->slots[i].key) & mask;
        while (slots[j].key)
            j = (j + 1) & mask;
        slots[j] = ix->slots[i];
    }
    xr_free(ix->slots);
    ix->slots = slots;
    ix->cap = new_cap;
    return true;
}

/* Insert key->idx, keeping the first binding for a key (matches the prior
 * linear scan's "first match wins").  Returns false only on allocation
 * failure, so callers can fail the build instead of leaving a row
 * unindexed (which would make find report a missing plan). */
static bool xaot_ptr_index_put(XaotPtrIndex *ix, const void *key, uint32_t idx) {
    uint32_t mask;
    uint32_t j;
    if (!ix || !key)
        return false;
    if (ix->cap == 0 && !xaot_ptr_index_rehash(ix, 16))
        return false;
    if ((ix->count + 1) * 4 >= ix->cap * 3 && !xaot_ptr_index_rehash(ix, ix->cap * 2))
        return false;
    mask = ix->cap - 1;
    j = xaot_ptr_hash(key) & mask;
    while (ix->slots[j].key) {
        if (ix->slots[j].key == key)
            return true; /* keep first */
        j = (j + 1) & mask;
    }
    ix->slots[j].key = key;
    ix->slots[j].idx = idx;
    ix->count++;
    return true;
}

static bool xaot_ptr_index_get(const XaotPtrIndex *ix, const void *key, uint32_t *out_idx) {
    uint32_t mask;
    uint32_t j;
    if (!ix || !key || ix->cap == 0)
        return false;
    mask = ix->cap - 1;
    j = xaot_ptr_hash(key) & mask;
    while (ix->slots[j].key) {
        if (ix->slots[j].key == key) {
            if (out_idx)
                *out_idx = ix->slots[j].idx;
            return true;
        }
        j = (j + 1) & mask;
    }
    return false;
}

static void xaot_ptr_index_free(XaotPtrIndex *ix) {
    if (!ix)
        return;
    xr_free(ix->slots);
    ix->slots = NULL;
    ix->cap = 0;
    ix->count = 0;
}

static const char *safe_str(const char *s) {
    return s ? s : "?";
}

static const char *arg_class_name(XaotArgClass cls) {
    switch (cls) {
        case XAOT_ARG_VOID:
            return "void";
        case XAOT_ARG_SCALAR:
            return "scalar";
        case XAOT_ARG_PTR:
            return "ptr";
        case XAOT_ARG_AGG_BY_VALUE:
            return "agg-by-value";
        case XAOT_ARG_AGG_BY_REF:
            return "agg-by-ref";
        case XAOT_ARG_TAGGED:
            return "tagged";
        case XAOT_ARG_AOT_CTX:
            return "aot-ctx";
        default:
            return "?";
    }
}

static const char *rep_name(XaotRep rep) {
    const XaotRepInfo *info = xaot_rep_info(rep);
    return info && info->name ? info->name : "?";
}

static uint32_t count_func_values(const XiFunc *func) {
    uint32_t total = 0;
    uint32_t bi;

    if (!func)
        return 0;

    for (bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        const XiPhi *phi;
        if (!blk)
            continue;
        total += blk->nvalues;
        for (phi = blk->phis; phi; phi = phi->next)
            total++;
    }
    return total;
}

static void dump_slot(FILE *out, const char *prefix, const XaotAbiSlot *slot) {
    if (!slot) {
        fprintf(out, "%s class=? kind=? rep=? c_type=?\n", prefix);
        return;
    }
    fprintf(out, "%s class=%s kind=%s rep=%s c_type=%s\n", prefix, arg_class_name(slot->cls),
            xaot_value_kind_name(slot->rep.kind), rep_name(slot->rep.rep), safe_str(slot->c_type));
}

XR_FUNC bool xaot_bundle_init(XaotBundle *bundle, XiModule **modules, uint32_t nmodules,
                              uint32_t entry_module) {
    if (!bundle || !modules || nmodules == 0 || entry_module >= nmodules)
        return false;
    memset(bundle, 0, sizeof(*bundle));
    bundle->modules = modules;
    bundle->nmodules = nmodules;
    bundle->entry_module = entry_module;
    return true;
}

XR_FUNC void xaot_bundle_free(XaotBundle *bundle) {
    uint32_t i;
    if (!bundle)
        return;
    for (i = 0; i < bundle->nfunc_plans; i++)
        xaot_abi_free(&bundle->func_plans[i].abi);
    xr_free(bundle->func_plans);
    xr_free(bundle->value_plans);
    xr_free(bundle->container_plans);
    xr_free(bundle->array_storage_plans);
    xr_free(bundle->array_cache_plans);
    xr_free(bundle->array_class_field_alloc_plans);
    xr_free(bundle->func_attr_plans);
    xr_free(bundle->bounds_plans);
    xr_free(bundle->alias_plans);
    xr_free(bundle->boundary_steps);
    xaot_ptr_index_free(&bundle->value_index);
    xaot_ptr_index_free(&bundle->func_index);
    xaot_ptr_index_free(&bundle->array_storage_index);
    xaot_ptr_index_free(&bundle->array_cache_index);
    xaot_ptr_index_free(&bundle->array_class_field_index);
    xaot_ptr_index_free(&bundle->func_attr_index);
    xaot_ptr_index_free(&bundle->bounds_index);
    xaot_ptr_index_free(&bundle->alias_index);
    memset(bundle, 0, sizeof(*bundle));
}

XR_FUNC XaotFuncPlan *xaot_bundle_add_func_plan(XaotBundle *bundle, XiFunc *func,
                                                uint32_t module_index, uint16_t depth) {
    XaotFuncPlan *plan;
    if (!bundle || !func || module_index >= bundle->nmodules)
        return NULL;
    if (bundle->nfunc_plans == bundle->func_plan_cap) {
        uint32_t new_cap = bundle->func_plan_cap < 16 ? 16 : bundle->func_plan_cap * 2;
        XaotFuncPlan *new_plans =
            (XaotFuncPlan *) xr_realloc(bundle->func_plans, sizeof(XaotFuncPlan) * new_cap);
        if (!new_plans)
            return NULL;
        bundle->func_plans = new_plans;
        bundle->func_plan_cap = new_cap;
    }
    plan = &bundle->func_plans[bundle->nfunc_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->func = func;
    plan->module_index = module_index;
    plan->depth = depth;
    if (!xaot_ptr_index_put(&bundle->func_index, func, bundle->nfunc_plans - 1)) {
        bundle->error_msg = "failed to index AOT function plan";
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotFuncPlan *xaot_bundle_find_func_plan(const XaotBundle *bundle,
                                                       const XiFunc *func) {
    uint32_t idx;
    if (!bundle || !func)
        return NULL;
    if (xaot_ptr_index_get(&bundle->func_index, func, &idx) && idx < bundle->nfunc_plans)
        return &bundle->func_plans[idx];
    return NULL;
}

XR_FUNC XaotValuePlan *xaot_bundle_add_value_plan(XaotBundle *bundle, const XiFunc *func,
                                                  const XiValue *value) {
    XaotValuePlan *plan;

    if (!bundle || !func || !value)
        return NULL;
    if (bundle->nvalue_plans == bundle->value_plan_cap) {
        uint32_t new_cap = bundle->value_plan_cap < 64 ? 64 : bundle->value_plan_cap * 2;
        XaotValuePlan *new_plans =
            (XaotValuePlan *) xr_realloc(bundle->value_plans, sizeof(XaotValuePlan) * new_cap);
        if (!new_plans)
            return NULL;
        bundle->value_plans = new_plans;
        bundle->value_plan_cap = new_cap;
    }
    plan = &bundle->value_plans[bundle->nvalue_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->func = func;
    plan->value = value;
    plan->rep = xaot_value_rep_for_value(value);
    if (!xaot_ptr_index_put(&bundle->value_index, value, bundle->nvalue_plans - 1)) {
        bundle->error_msg = "failed to index AOT value plan";
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotValuePlan *xaot_bundle_find_value_plan(const XaotBundle *bundle,
                                                         const XiValue *value) {
    uint32_t idx;

    if (!bundle || !value)
        return NULL;
    if (xaot_ptr_index_get(&bundle->value_index, value, &idx) && idx < bundle->nvalue_plans)
        return &bundle->value_plans[idx];
    return NULL;
}

XR_FUNC XaotContainerTypePlan *xaot_bundle_add_container_plan(XaotBundle *bundle,
                                                              const XrType *type) {
    XaotContainerTypePlan *plan;

    if (!bundle || !type)
        return NULL;
    plan = (XaotContainerTypePlan *) xaot_bundle_find_container_plan(bundle, type);
    if (plan)
        return plan;
    if (bundle->ncontainer_plans == bundle->container_plan_cap) {
        uint32_t new_cap = bundle->container_plan_cap < 16 ? 16 : bundle->container_plan_cap * 2;
        XaotContainerTypePlan *new_plans = (XaotContainerTypePlan *) xr_realloc(
            bundle->container_plans, sizeof(XaotContainerTypePlan) * new_cap);
        if (!new_plans)
            return NULL;
        bundle->container_plans = new_plans;
        bundle->container_plan_cap = new_cap;
    }
    plan = &bundle->container_plans[bundle->ncontainer_plans++];
    memset(plan, 0, sizeof(*plan));
    if (!xaot_container_plan_for_type(type, &plan->plan)) {
        bundle->ncontainer_plans--;
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotContainerTypePlan *xaot_bundle_find_container_plan(const XaotBundle *bundle,
                                                                     const XrType *type) {
    uint32_t i;

    if (!bundle || !type)
        return NULL;
    for (i = 0; i < bundle->ncontainer_plans; i++) {
        if (xaot_container_plan_matches_type(&bundle->container_plans[i].plan, type))
            return &bundle->container_plans[i];
    }
    return NULL;
}

XR_FUNC XaotArrayStoragePlan *
xaot_bundle_add_array_storage_plan(XaotBundle *bundle, const XiFunc *func, const XiValue *value,
                                   const XiValue *origin, uint32_t flags,
                                   const XaotContainerElemPlan *elem) {
    XaotArrayStoragePlan *plan;

    if (!bundle || !func || !value || !elem || !elem->elem_name || !elem->c_type ||
        (flags & (XAOT_ARRAY_STORAGE_READ | XAOT_ARRAY_STORAGE_MUTABLE)) == 0)
        return NULL;
    plan = (XaotArrayStoragePlan *) xaot_bundle_find_array_storage_plan(bundle, value);
    if (plan)
        return plan;
    if (bundle->narray_storage_plans == bundle->array_storage_plan_cap) {
        uint32_t new_cap =
            bundle->array_storage_plan_cap < 32 ? 32 : bundle->array_storage_plan_cap * 2;
        XaotArrayStoragePlan *new_plans = (XaotArrayStoragePlan *) xr_realloc(
            bundle->array_storage_plans, sizeof(XaotArrayStoragePlan) * new_cap);
        if (!new_plans)
            return NULL;
        bundle->array_storage_plans = new_plans;
        bundle->array_storage_plan_cap = new_cap;
    }
    plan = &bundle->array_storage_plans[bundle->narray_storage_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->func = func;
    plan->value = value;
    plan->origin = origin ? origin : value;
    plan->flags = flags & (XAOT_ARRAY_STORAGE_READ | XAOT_ARRAY_STORAGE_MUTABLE);
    plan->elem = *elem;
    if (!xaot_ptr_index_put(&bundle->array_storage_index, value,
                            bundle->narray_storage_plans - 1)) {
        bundle->error_msg = "failed to index AOT array storage plan";
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotArrayStoragePlan *xaot_bundle_find_array_storage_plan(const XaotBundle *bundle,
                                                                        const XiValue *value) {
    uint32_t idx;

    if (!bundle || !value)
        return NULL;
    if (xaot_ptr_index_get(&bundle->array_storage_index, value, &idx) &&
        idx < bundle->narray_storage_plans)
        return &bundle->array_storage_plans[idx];
    return NULL;
}

XR_FUNC XaotArrayCachePlan *xaot_bundle_add_array_cache_plan(XaotBundle *bundle, const XiFunc *func,
                                                             const XiValue *value,
                                                             const XiValue *storage_value,
                                                             uint32_t flags,
                                                             const XaotContainerElemPlan *elem) {
    XaotArrayCachePlan *plan;

    if (!bundle || !func || !value || !storage_value || !elem || !elem->elem_name ||
        !elem->c_type || (flags & (XAOT_ARRAY_CACHE_READ | XAOT_ARRAY_CACHE_MUTABLE)) == 0)
        return NULL;
    plan = (XaotArrayCachePlan *) xaot_bundle_find_array_cache_plan(bundle, value);
    if (plan)
        return plan;
    if (bundle->narray_cache_plans == bundle->array_cache_plan_cap) {
        uint32_t new_cap =
            bundle->array_cache_plan_cap < 32 ? 32 : bundle->array_cache_plan_cap * 2;
        XaotArrayCachePlan *new_plans = (XaotArrayCachePlan *) xr_realloc(
            bundle->array_cache_plans, sizeof(XaotArrayCachePlan) * new_cap);
        if (!new_plans)
            return NULL;
        bundle->array_cache_plans = new_plans;
        bundle->array_cache_plan_cap = new_cap;
    }
    plan = &bundle->array_cache_plans[bundle->narray_cache_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->func = func;
    plan->value = value;
    plan->storage_value = storage_value;
    plan->flags = flags & (XAOT_ARRAY_CACHE_READ | XAOT_ARRAY_CACHE_MUTABLE |
                           XAOT_ARRAY_CACHE_DECLARE_LOCAL | XAOT_ARRAY_CACHE_FRESH_RESULT |
                           XAOT_ARRAY_CACHE_VIEW | XAOT_ARRAY_CACHE_FILL_LOOP |
                           XAOT_ARRAY_CACHE_NATIVE_LOCAL | XAOT_ARRAY_CACHE_CLASS_FIELD);
    plan->elem = *elem;
    if (!xaot_ptr_index_put(&bundle->array_cache_index, value, bundle->narray_cache_plans - 1)) {
        bundle->error_msg = "failed to index AOT array cache plan";
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotArrayCachePlan *xaot_bundle_find_array_cache_plan(const XaotBundle *bundle,
                                                                    const XiValue *value) {
    uint32_t idx;

    if (!bundle || !value)
        return NULL;
    if (xaot_ptr_index_get(&bundle->array_cache_index, value, &idx) &&
        idx < bundle->narray_cache_plans)
        return &bundle->array_cache_plans[idx];
    return NULL;
}

XR_FUNC XaotArrayClassFieldAllocPlan *xaot_bundle_add_array_class_field_alloc_plan(
    XaotBundle *bundle, const XiFunc *func, const XiValue *origin, const XiValue *store,
    const XiClassData *class_data, uint16_t field_idx, const XaotContainerElemPlan *elem) {
    XaotArrayClassFieldAllocPlan *plan;

    if (!bundle || !func || !origin || !store || !class_data || !class_data->instance_layout ||
        !elem || !elem->elem_name || !elem->c_type)
        return NULL;
    plan = (XaotArrayClassFieldAllocPlan *) xaot_bundle_find_array_class_field_alloc_plan(bundle,
                                                                                          origin);
    if (plan)
        return plan;
    if (bundle->narray_class_field_alloc_plans == bundle->array_class_field_alloc_plan_cap) {
        uint32_t new_cap = bundle->array_class_field_alloc_plan_cap < 16
                               ? 16
                               : bundle->array_class_field_alloc_plan_cap * 2;
        XaotArrayClassFieldAllocPlan *new_plans = (XaotArrayClassFieldAllocPlan *) xr_realloc(
            bundle->array_class_field_alloc_plans, sizeof(XaotArrayClassFieldAllocPlan) * new_cap);
        if (!new_plans)
            return NULL;
        bundle->array_class_field_alloc_plans = new_plans;
        bundle->array_class_field_alloc_plan_cap = new_cap;
    }
    plan = &bundle->array_class_field_alloc_plans[bundle->narray_class_field_alloc_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->func = func;
    plan->origin = origin;
    plan->store = store;
    plan->class_data = class_data;
    plan->field_idx = field_idx;
    plan->elem = *elem;
    if (!xaot_ptr_index_put(&bundle->array_class_field_index, origin,
                            bundle->narray_class_field_alloc_plans - 1)) {
        bundle->error_msg = "failed to index AOT array class-field alloc plan";
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotArrayClassFieldAllocPlan *
xaot_bundle_find_array_class_field_alloc_plan(const XaotBundle *bundle, const XiValue *origin) {
    uint32_t idx;
    if (!bundle || !origin)
        return NULL;
    if (xaot_ptr_index_get(&bundle->array_class_field_index, origin, &idx) &&
        idx < bundle->narray_class_field_alloc_plans)
        return &bundle->array_class_field_alloc_plans[idx];
    return NULL;
}

XR_FUNC const XaotArrayClassFieldAllocPlan *
xaot_bundle_find_array_class_field_alloc_plan_for_store(const XaotBundle *bundle,
                                                        const XiValue *store) {
    if (!bundle || !store)
        return NULL;
    for (uint32_t i = 0; i < bundle->narray_class_field_alloc_plans; i++) {
        if (bundle->array_class_field_alloc_plans[i].store == store)
            return &bundle->array_class_field_alloc_plans[i];
    }
    return NULL;
}

XR_FUNC const XaotArrayClassFieldAllocPlan *xaot_bundle_find_array_class_field_alloc_plan_for_field(
    const XaotBundle *bundle, const XiFunc *func, const XiClassData *class_data,
    uint16_t field_idx) {
    const XaotArrayClassFieldAllocPlan *found = NULL;
    if (!bundle || !func || !class_data)
        return NULL;
    for (uint32_t i = 0; i < bundle->narray_class_field_alloc_plans; i++) {
        const XaotArrayClassFieldAllocPlan *plan = &bundle->array_class_field_alloc_plans[i];
        if (plan->func != func || plan->class_data != class_data || plan->field_idx != field_idx)
            continue;
        if (found)
            return NULL;
        found = plan;
    }
    return found;
}

XR_FUNC XaotFuncAttrPlan *xaot_bundle_add_func_attr_plan(XaotBundle *bundle, const XiFunc *func,
                                                         uint32_t flags) {
    XaotFuncAttrPlan *plan;

    /* CONST and PURE are mutually exclusive by definition. */
    if (!bundle || !func || flags == 0 ||
        (flags & (XAOT_FN_ATTR_CONST | XAOT_FN_ATTR_PURE)) ==
            (XAOT_FN_ATTR_CONST | XAOT_FN_ATTR_PURE))
        return NULL;
    plan = (XaotFuncAttrPlan *) xaot_bundle_find_func_attr_plan(bundle, func);
    if (plan)
        return plan;
    if (bundle->nfunc_attr_plans == bundle->func_attr_plan_cap) {
        uint32_t new_cap = bundle->func_attr_plan_cap < 16 ? 16 : bundle->func_attr_plan_cap * 2;
        XaotFuncAttrPlan *new_plans = (XaotFuncAttrPlan *) xr_realloc(
            bundle->func_attr_plans, sizeof(XaotFuncAttrPlan) * new_cap);
        if (!new_plans)
            return NULL;
        bundle->func_attr_plans = new_plans;
        bundle->func_attr_plan_cap = new_cap;
    }
    plan = &bundle->func_attr_plans[bundle->nfunc_attr_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->func = func;
    plan->flags = flags & (XAOT_FN_ATTR_CONST | XAOT_FN_ATTR_PURE);
    if (!xaot_ptr_index_put(&bundle->func_attr_index, func, bundle->nfunc_attr_plans - 1)) {
        bundle->error_msg = "failed to index AOT function attribute plan";
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotFuncAttrPlan *xaot_bundle_find_func_attr_plan(const XaotBundle *bundle,
                                                                const XiFunc *func) {
    uint32_t idx;

    if (!bundle || !func)
        return NULL;
    if (xaot_ptr_index_get(&bundle->func_attr_index, func, &idx) && idx < bundle->nfunc_attr_plans)
        return &bundle->func_attr_plans[idx];
    return NULL;
}

XR_FUNC XaotBoundsPlan *xaot_bundle_add_bounds_plan(XaotBundle *bundle, const XiFunc *func,
                                                    const XiValue *access, uint32_t evidence,
                                                    uint8_t unproven_reason) {
    XaotBoundsPlan *plan;

    /* Proven entries carry evidence and no reason; unproven entries carry a
     * reason and no evidence. Anything else is a caller bug. */
    if (!bundle || !func || !access || (evidence == 0) == (unproven_reason == 0))
        return NULL;
    plan = (XaotBoundsPlan *) xaot_bundle_find_bounds_plan(bundle, access);
    if (plan)
        return plan;
    if (bundle->nbounds_plans == bundle->bounds_plan_cap) {
        uint32_t new_cap = bundle->bounds_plan_cap < 16 ? 16 : bundle->bounds_plan_cap * 2;
        XaotBoundsPlan *new_plans =
            (XaotBoundsPlan *) xr_realloc(bundle->bounds_plans, sizeof(XaotBoundsPlan) * new_cap);
        if (!new_plans)
            return NULL;
        bundle->bounds_plans = new_plans;
        bundle->bounds_plan_cap = new_cap;
    }
    plan = &bundle->bounds_plans[bundle->nbounds_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->func = func;
    plan->access = access;
    plan->evidence = evidence;
    plan->unproven_reason = unproven_reason;
    if (!xaot_ptr_index_put(&bundle->bounds_index, access, bundle->nbounds_plans - 1)) {
        bundle->error_msg = "failed to index AOT bounds plan";
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotBoundsPlan *xaot_bundle_find_bounds_plan(const XaotBundle *bundle,
                                                           const XiValue *access) {
    uint32_t idx;

    if (!bundle || !access)
        return NULL;
    if (xaot_ptr_index_get(&bundle->bounds_index, access, &idx) && idx < bundle->nbounds_plans)
        return &bundle->bounds_plans[idx];
    return NULL;
}

XR_FUNC XaotAliasPlan *xaot_bundle_add_alias_plan(XaotBundle *bundle, const XiFunc *func,
                                                  const XiValue *value, uint8_t kind,
                                                  uint32_t evidence) {
    XaotAliasPlan *plan;

    if (!bundle || !func || !value || kind == 0 || evidence == 0)
        return NULL;
    plan = (XaotAliasPlan *) xaot_bundle_find_alias_plan(bundle, value);
    if (plan)
        return plan;
    if (bundle->nalias_plans == bundle->alias_plan_cap) {
        uint32_t new_cap = bundle->alias_plan_cap < 16 ? 16 : bundle->alias_plan_cap * 2;
        XaotAliasPlan *new_plans =
            (XaotAliasPlan *) xr_realloc(bundle->alias_plans, sizeof(XaotAliasPlan) * new_cap);
        if (!new_plans)
            return NULL;
        bundle->alias_plans = new_plans;
        bundle->alias_plan_cap = new_cap;
    }
    plan = &bundle->alias_plans[bundle->nalias_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->func = func;
    plan->value = value;
    plan->kind = kind;
    plan->evidence = evidence;
    if (!xaot_ptr_index_put(&bundle->alias_index, value, bundle->nalias_plans - 1)) {
        bundle->error_msg = "failed to index AOT alias plan";
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotAliasPlan *xaot_bundle_find_alias_plan(const XaotBundle *bundle,
                                                         const XiValue *value) {
    uint32_t idx;

    if (!bundle || !value)
        return NULL;
    if (xaot_ptr_index_get(&bundle->alias_index, value, &idx) && idx < bundle->nalias_plans)
        return &bundle->alias_plans[idx];
    return NULL;
}

XR_FUNC XaotBoundaryStep *xaot_bundle_add_boundary_step(XaotBundle *bundle,
                                                        XaotBoundaryStepKind kind,
                                                        const XiFunc *func, const XiValue *value,
                                                        const XiValue *input,
                                                        XaotBoundaryReason reason) {
    XaotBoundaryStep *step;

    if (!bundle || !func || reason == XAOT_BOUNDARY_NONE)
        return NULL;
    if (bundle->nboundary_steps == bundle->boundary_step_cap) {
        uint32_t new_cap = bundle->boundary_step_cap < 32 ? 32 : bundle->boundary_step_cap * 2;
        XaotBoundaryStep *new_steps = (XaotBoundaryStep *) xr_realloc(
            bundle->boundary_steps, sizeof(XaotBoundaryStep) * new_cap);
        if (!new_steps)
            return NULL;
        bundle->boundary_steps = new_steps;
        bundle->boundary_step_cap = new_cap;
    }
    step = &bundle->boundary_steps[bundle->nboundary_steps++];
    memset(step, 0, sizeof(*step));
    step->kind = kind;
    step->func = func;
    step->value = value;
    step->input = input;
    step->reason = reason;
    step->arg_index = UINT16_MAX;
    if (input) {
        const XaotValuePlan *from = xaot_bundle_find_value_plan(bundle, input);
        if (from)
            step->from_rep = from->rep;
    }
    if (value) {
        const XaotValuePlan *to = xaot_bundle_find_value_plan(bundle, value);
        if (to)
            step->to_rep = to->rep;
    }
    return step;
}

XR_FUNC const XaotBoundaryStep *
xaot_bundle_find_boundary_step(const XaotBundle *bundle, XaotBoundaryStepKind kind,
                               const XiFunc *func, const XiValue *value, const XiValue *input) {
    return xaot_bundle_find_boundary_step_ex(bundle, kind, func, value, input, NULL, UINT16_MAX);
}

XR_FUNC const XaotBoundaryStep *
xaot_bundle_find_boundary_step_ex(const XaotBundle *bundle, XaotBoundaryStepKind kind,
                                  const XiFunc *func, const XiValue *value, const XiValue *input,
                                  const XiFunc *target_func, uint16_t arg_index) {
    uint32_t i;

    if (!bundle || !func)
        return NULL;
    for (i = 0; i < bundle->nboundary_steps; i++) {
        const XaotBoundaryStep *step = &bundle->boundary_steps[i];
        if (step->kind == kind && step->func == func && step->value == value &&
            step->input == input && step->target_func == target_func &&
            step->arg_index == arg_index)
            return step;
    }
    return NULL;
}

static void value_ref(char *buf, size_t bufsz, const XiValue *value) {
    if (!buf || bufsz == 0)
        return;
    if (!value) {
        snprintf(buf, bufsz, "-");
        return;
    }
    snprintf(buf, bufsz, "%s%u", value->op == XI_PHI ? "phi" : "v", value->id);
}

XR_FUNC char *xaot_bundle_dump_plan(const XaotBundle *bundle) {
    char *buf = NULL;
    size_t bufsz = 0;
    FILE *out;
    uint32_t mi;
    uint32_t fi;

    if (!bundle)
        return NULL;

    out = xr_open_memstream(&buf, &bufsz);
    if (!out)
        return NULL;

    fprintf(out, "xaot-plan v0\n");
    fprintf(out, "modules %u entry %u\n", bundle->nmodules, bundle->entry_module);
    for (mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules ? bundle->modules[mi] : NULL;
        const XiFunc *init = mod ? mod->init : NULL;
        fprintf(out, "module %u name=%s path=%s entry=%u funcs=%u\n", mi,
                safe_str(mod ? mod->name : NULL), safe_str(mod ? mod->path : NULL),
                mi == bundle->entry_module ? 1u : 0u,
                init ? (unsigned) (1u + init->nchildren) : 0u);
    }

    for (fi = 0; fi < bundle->nfunc_plans; fi++) {
        const XaotFuncPlan *plan = &bundle->func_plans[fi];
        const XiFunc *func = plan->func;
        const XaotFuncAbi *abi = &plan->abi;
        uint16_t pi;

        fprintf(out,
                "function %u name=%s module=%u depth=%u abi=%s boundary=%s params=%u "
                "ret=%s/%s/%s captures=%u blocks=%u values=%u\n",
                fi, safe_str(func ? func->name : NULL), plan->module_index, (unsigned) plan->depth,
                xaot_abi_kind_name(abi->kind), xaot_boundary_reason_name(abi->boundary_reason),
                (unsigned) abi->nparams, safe_str(abi->ret.c_type),
                xaot_value_kind_name(abi->ret.rep.kind), rep_name(abi->ret.rep.rep),
                func ? (unsigned) func->ncaptures : 0u, func ? (unsigned) func->nblocks : 0u,
                count_func_values(func));
        dump_slot(out, "  ret", &abi->ret);
        for (pi = 0; pi < abi->nparams; pi++) {
            char prefix[32];
            snprintf(prefix, sizeof(prefix), "  param %u", (unsigned) pi);
            dump_slot(out, prefix, &abi->params[pi]);
        }
        for (uint32_t vi = 0; vi < bundle->nvalue_plans; vi++) {
            const XaotValuePlan *vp = &bundle->value_plans[vi];
            if (vp->func != func || !vp->value)
                continue;
            fprintf(out, "  value %s%u op=%s kind=%s rep=%s c_type=%s\n",
                    vp->value->op == XI_PHI ? "phi" : "v", vp->value->id, xi_op_name(vp->value->op),
                    xaot_value_kind_name(vp->rep.kind), rep_name(vp->rep.rep),
                    safe_str(vp->rep.c_type));
        }
    }

    for (uint32_t ci = 0; ci < bundle->ncontainer_plans; ci++) {
        const XaotContainerPlan *cp = &bundle->container_plans[ci].plan;
        fprintf(out, "container %u kind=%s flags=0x%x", ci, xaot_container_kind_name(cp->kind),
                cp->flags);
        if (cp->kind == XAOT_CONTAINER_ARRAY || cp->kind == XAOT_CONTAINER_SET) {
            fprintf(out, " elem=%s/%s/%s", safe_str(cp->elem.elem_name), rep_name(cp->elem.rep),
                    safe_str(cp->elem.c_type));
        } else if (cp->kind == XAOT_CONTAINER_MAP) {
            fprintf(out, " key=%s/%s/%s value=%s/%s/%s", safe_str(cp->key.elem_name),
                    rep_name(cp->key.rep), safe_str(cp->key.c_type), safe_str(cp->value.elem_name),
                    rep_name(cp->value.rep), safe_str(cp->value.c_type));
        }
        fprintf(out, " type-key=%016" PRIx64, cp->type_key.fingerprint);
        fprintf(out, "\n");
    }

    for (uint32_t ai = 0; ai < bundle->narray_storage_plans; ai++) {
        const XaotArrayStoragePlan *ap = &bundle->array_storage_plans[ai];
        char value_buf[32];
        char origin_buf[32];
        value_ref(value_buf, sizeof(value_buf), ap->value);
        value_ref(origin_buf, sizeof(origin_buf), ap->origin);
        fprintf(out, "array-storage %u func=%s value=%s origin=%s flags=0x%x elem=%s/%s/%s\n", ai,
                safe_str(ap->func ? ap->func->name : NULL), value_buf, origin_buf, ap->flags,
                safe_str(ap->elem.elem_name), rep_name(ap->elem.rep), safe_str(ap->elem.c_type));
    }

    for (uint32_t ai = 0; ai < bundle->narray_cache_plans; ai++) {
        const XaotArrayCachePlan *ap = &bundle->array_cache_plans[ai];
        char value_buf[32];
        char storage_buf[32];
        value_ref(value_buf, sizeof(value_buf), ap->value);
        value_ref(storage_buf, sizeof(storage_buf), ap->storage_value);
        fprintf(out, "array-cache %u func=%s value=%s storage=%s flags=0x%x elem=%s/%s/%s\n", ai,
                safe_str(ap->func ? ap->func->name : NULL), value_buf, storage_buf, ap->flags,
                safe_str(ap->elem.elem_name), rep_name(ap->elem.rep), safe_str(ap->elem.c_type));
    }

    for (uint32_t ai = 0; ai < bundle->narray_class_field_alloc_plans; ai++) {
        const XaotArrayClassFieldAllocPlan *ap = &bundle->array_class_field_alloc_plans[ai];
        char origin_buf[32];
        char store_buf[32];
        value_ref(origin_buf, sizeof(origin_buf), ap->origin);
        value_ref(store_buf, sizeof(store_buf), ap->store);
        fprintf(out,
                "array-class-field-alloc %u func=%s origin=%s store=%s class=%s field=%u "
                "elem=%s/%s/%s\n",
                ai, safe_str(ap->func ? ap->func->name : NULL), origin_buf, store_buf,
                safe_str(ap->class_data ? ap->class_data->class_name : NULL),
                (unsigned) ap->field_idx, safe_str(ap->elem.elem_name), rep_name(ap->elem.rep),
                safe_str(ap->elem.c_type));
    }

    for (uint32_t ai = 0; ai < bundle->nfunc_attr_plans; ai++) {
        const XaotFuncAttrPlan *ap = &bundle->func_attr_plans[ai];
        fprintf(out, "fn-attr %u func=%s attr=%s\n", ai, safe_str(ap->func ? ap->func->name : NULL),
                (ap->flags & XAOT_FN_ATTR_CONST) ? "const" : "pure");
    }

    for (uint32_t ai = 0; ai < bundle->nbounds_plans; ai++) {
        const XaotBoundsPlan *bp = &bundle->bounds_plans[ai];
        char access_buf[32];
        const char *op_name =
            bp->access && bp->access->op == XI_INDEX_SET ? "index_set" : "index_get";
        value_ref(access_buf, sizeof(access_buf), bp->access);
        if (bp->evidence != 0) {
            fprintf(out, "bounds %u func=%s access=%s op=%s evidence=%s%s%s\n", ai,
                    safe_str(bp->func ? bp->func->name : NULL), access_buf, op_name,
                    (bp->evidence & XAOT_BOUNDS_EV_DOM_GUARD) ? "dom_guard" : "",
                    (bp->evidence & XAOT_BOUNDS_EV_COUNTED_LOOP) ? "counted_loop" : "",
                    (bp->evidence & XAOT_BOUNDS_EV_NONNEG_INDEX) ? "+nonneg" : "");
        } else {
            static const char *const reason_names[] = {"none", "no_guard", "index_range",
                                                       "len_mismatch", "clobber"};
            const char *reason =
                bp->unproven_reason < 5 ? reason_names[bp->unproven_reason] : "unknown";
            fprintf(out, "bounds-unproven %u func=%s access=%s op=%s reason=%s\n", ai,
                    safe_str(bp->func ? bp->func->name : NULL), access_buf, op_name, reason);
        }
    }

    for (uint32_t ai = 0; ai < bundle->nalias_plans; ai++) {
        const XaotAliasPlan *ap = &bundle->alias_plans[ai];
        char value_buf[32];
        static const char *const kind_names[] = {"none", "unique_data", "unique_recv",
                                                 "unique_param"};
        value_ref(value_buf, sizeof(value_buf), ap->value);
        fprintf(out, "alias %u func=%s value=%s kind=%s evidence=%s%s%s%s\n", ai,
                safe_str(ap->func ? ap->func->name : NULL), value_buf,
                ap->kind < 4 ? kind_names[ap->kind] : "unknown",
                (ap->evidence & XAOT_ALIAS_EV_FRESH_ALLOC) ? "fresh" : "",
                (ap->evidence & XAOT_ALIAS_EV_ALL_ACCESS_RAW) ? "+raw" : "",
                (ap->evidence & XAOT_ALIAS_EV_USE_WHITELIST) ? "+whitelist" : "",
                (ap->evidence & XAOT_ALIAS_EV_SOLE_CACHE) ? "+sole" : "");
    }

    for (uint32_t bi = 0; bi < bundle->nboundary_steps; bi++) {
        const XaotBoundaryStep *step = &bundle->boundary_steps[bi];
        char value_buf[32];
        char input_buf[32];
        char arg_buf[16];
        value_ref(value_buf, sizeof(value_buf), step->value);
        value_ref(input_buf, sizeof(input_buf), step->input);
        if (step->arg_index == UINT16_MAX)
            snprintf(arg_buf, sizeof(arg_buf), "-");
        else
            snprintf(arg_buf, sizeof(arg_buf), "%u", (unsigned) step->arg_index);
        fprintf(out,
                "boundary %u kind=%s func=%s value=%s input=%s reason=%s target=%s arg=%s "
                "from=%s/%s to=%s/%s\n",
                bi, xaot_boundary_step_kind_name(step->kind),
                safe_str(step->func ? step->func->name : NULL), value_buf, input_buf,
                xaot_boundary_reason_name(step->reason),
                safe_str(step->target_func ? step->target_func->name : NULL), arg_buf,
                xaot_value_kind_name(step->from_rep.kind), rep_name(step->from_rep.rep),
                xaot_value_kind_name(step->to_rep.kind), rep_name(step->to_rep.rep));
    }

    fprintf(out,
            "stats functions=%u native=%u tagged=%u coro=%u values=%u boundaries=%u "
            "containers=%u\n",
            bundle->stats.functions_total, bundle->stats.functions_native_abi,
            bundle->stats.functions_tagged_abi, bundle->stats.functions_coro_abi,
            bundle->stats.values_total, bundle->stats.boundary_count,
            bundle->stats.containers_total);
    fprintf(out, "value-stats scalar=%u tagged=%u ptr=%u aggregate=%u view=%u void=%u\n",
            bundle->stats.values_scalar, bundle->stats.values_tagged, bundle->stats.values_ptr,
            bundle->stats.values_aggregate, bundle->stats.values_view, bundle->stats.values_void);
    fprintf(out, "container-stats array=%u map=%u set=%u direct=%u\n",
            bundle->stats.containers_array, bundle->stats.containers_map,
            bundle->stats.containers_set, bundle->stats.containers_direct);
    fprintf(out, "array-storage-stats total=%u read=%u mutable=%u\n",
            bundle->stats.array_storage_total, bundle->stats.array_storage_read,
            bundle->stats.array_storage_mutable);
    fprintf(out, "array-cache-stats total=%u read=%u mutable=%u\n", bundle->stats.array_cache_total,
            bundle->stats.array_cache_read, bundle->stats.array_cache_mutable);

    if (ferror(out)) {
        (void) xr_close_memstream(out, &buf, &bufsz);
        xr_free(buf);
        return NULL;
    }
    if (xr_close_memstream(out, &buf, &bufsz) != 0)
        return NULL;
    return buf;
}
