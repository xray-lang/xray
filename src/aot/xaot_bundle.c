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
#include "xaot_struct_name.h"
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

static void xaot_enum_plan_free(XaotEnumPlan *plan) {
    if (!plan)
        return;
    if (plan->owns_members && plan->members) {
        XiEnumMemberData *members = (XiEnumMemberData *) plan->members;
        for (uint32_t i = 0; i < plan->member_count; i++)
            xr_free(members[i].payload_types);
        xr_free(members);
    }
    xr_free(plan->type_args);
    xr_free((void *) plan->c_type);
    memset(plan, 0, sizeof(*plan));
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
    for (uint32_t mi = 0; mi < nmodules; mi++) {
        const XiModule *mod = modules[mi];
        if (!mod || !mod->slot_enums)
            continue;
        for (uint16_t si = 0; si < mod->nslots; si++) {
            const XiEnumData *ed = mod->slot_enums[si];
            if (ed && ed->is_adt && !xaot_bundle_add_enum_plan(bundle, ed, mi)) {
                xaot_bundle_free(bundle);
                return false;
            }
        }
    }
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
    for (i = 0; i < bundle->nenum_plans; i++)
        xaot_enum_plan_free(&bundle->enum_plans[i]);
    xr_free(bundle->enum_plans);
    xr_free(bundle->array_storage_plans);
    xr_free(bundle->array_cache_plans);
    xr_free(bundle->array_class_field_alloc_plans);
    xr_free(bundle->func_attr_plans);
    xr_free(bundle->bounds_plans);
    xr_free(bundle->span_access_plans);
    xr_free(bundle->alias_plans);
    xr_free(bundle->boundary_steps);
    xaot_ptr_index_free(&bundle->value_index);
    xaot_ptr_index_free(&bundle->func_index);
    xaot_ptr_index_free(&bundle->array_storage_index);
    xaot_ptr_index_free(&bundle->array_cache_index);
    xaot_ptr_index_free(&bundle->array_class_field_index);
    xaot_ptr_index_free(&bundle->func_attr_index);
    xaot_ptr_index_free(&bundle->bounds_index);
    xaot_ptr_index_free(&bundle->span_access_index);
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

XR_FUNC XaotValuePlan *xaot_bundle_find_value_plan_mut(XaotBundle *bundle, const XiValue *value) {
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

XR_FUNC XaotEnumPlan *xaot_bundle_add_enum_plan(XaotBundle *bundle, const XiEnumData *enum_data,
                                                uint32_t module_index) {
    XaotEnumPlan *plan;
    char ctype[192];

    if (!bundle || !enum_data || !enum_data->is_adt)
        return NULL;
    plan = (XaotEnumPlan *) xaot_bundle_find_enum_plan(bundle, enum_data);
    if (plan)
        return plan;
    if (module_index >= bundle->nmodules)
        return NULL;
    if (bundle->nenum_plans == bundle->enum_plan_cap) {
        uint32_t new_cap = bundle->enum_plan_cap < 8 ? 8 : bundle->enum_plan_cap * 2;
        XaotEnumPlan *new_plans =
            (XaotEnumPlan *) xr_realloc(bundle->enum_plans, sizeof(XaotEnumPlan) * new_cap);
        if (!new_plans)
            return NULL;
        bundle->enum_plans = new_plans;
        bundle->enum_plan_cap = new_cap;
    }
    const XiModule *mod = bundle->modules ? bundle->modules[module_index] : NULL;
    xaot_enum_c_type_name_for_type(ctype, sizeof(ctype), mod && mod->name ? mod->name : "mod",
                                   enum_data, NULL);
    plan = &bundle->enum_plans[bundle->nenum_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->enum_data = enum_data;
    plan->members = enum_data->members;
    plan->module_index = module_index;
    plan->member_count = enum_data->member_count;
    plan->layout_id = enum_data->layout_id;
    plan->max_payload = enum_data->max_payload > 0 ? (uint16_t) enum_data->max_payload : 0;
    plan->type_arg_count = 0;
    plan->c_type = xr_strdup(ctype);
    if (!plan->c_type) {
        bundle->nenum_plans--;
        bundle->error_msg = "failed to allocate AOT enum plan C type";
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotEnumPlan *xaot_bundle_find_enum_plan(const XaotBundle *bundle,
                                                       const XiEnumData *enum_data) {
    if (!bundle || !enum_data)
        return NULL;
    for (uint32_t i = 0; i < bundle->nenum_plans; i++) {
        if (bundle->enum_plans[i].enum_data == enum_data &&
            bundle->enum_plans[i].type_arg_count == 0)
            return &bundle->enum_plans[i];
    }
    return NULL;
}

static const char *type_enum_name(const XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_ENUM)
        return type->enum_type.enum_name ? type->enum_type.enum_name : type->instance.class_name;
    if ((type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
        type->instance.class_name)
        return type->instance.class_name;
    return NULL;
}

static int type_enum_arg_count(const XrType *type) {
    if (!type || (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE))
        return 0;
    return type->instance.type_arg_count > 0 ? type->instance.type_arg_count : 0;
}

static XrType **type_enum_args(const XrType *type) {
    if (!type || (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE) ||
        type->instance.type_arg_count <= 0)
        return NULL;
    return type->instance.type_args;
}

static bool type_args_match(XrType **a, int ac, XrType **b, int bc) {
    if (ac != bc)
        return false;
    if (ac <= 0)
        return true;
    if (!a || !b)
        return false;
    for (int i = 0; i < ac; i++) {
        if (!xr_type_equals(a[i], b[i]))
            return false;
    }
    return true;
}

XR_FUNC const XaotEnumPlan *xaot_bundle_find_enum_plan_for_type(const XaotBundle *bundle,
                                                                const XrType *type) {
    const char *name = type_enum_name(type);
    int argc = type_enum_arg_count(type);
    XrType **args = type_enum_args(type);
    const XaotEnumPlan *fallback = NULL;

    if (!bundle || !name)
        return NULL;
    for (uint32_t i = 0; i < bundle->nenum_plans; i++) {
        const XaotEnumPlan *plan = &bundle->enum_plans[i];
        const XiEnumData *ed = plan->enum_data;
        if (!ed || !ed->is_adt || !ed->name || strcmp(ed->name, name) != 0)
            continue;
        if (plan->type_arg_count == 0) {
            fallback = plan;
            if (argc == 0)
                return plan;
            continue;
        }
        if (argc > 0 && type_args_match(plan->type_args, plan->type_arg_count, args, argc))
            return plan;
    }
    return argc == 0 ? fallback : NULL;
}

static const XiEnumData *find_enum_data_by_name(const XaotBundle *bundle, const char *name,
                                                uint32_t *module_index_out) {
    if (!bundle || !name)
        return NULL;
    for (uint32_t i = 0; i < bundle->nenum_plans; i++) {
        const XaotEnumPlan *plan = &bundle->enum_plans[i];
        const XiEnumData *ed = plan->enum_data;
        if (plan->type_arg_count == 0 && ed && ed->is_adt && ed->name &&
            strcmp(ed->name, name) == 0) {
            if (module_index_out)
                *module_index_out = plan->module_index;
            return ed;
        }
    }
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules ? bundle->modules[mi] : NULL;
        if (!mod || !mod->slot_enums)
            continue;
        for (uint16_t si = 0; si < mod->nslots; si++) {
            const XiEnumData *ed = mod->slot_enums[si];
            if (ed && ed->is_adt && ed->name && strcmp(ed->name, name) == 0) {
                if (module_index_out)
                    *module_index_out = mi;
                return ed;
            }
        }
    }
    return NULL;
}

static XaotEnumPlan *xaot_bundle_add_concrete_enum_plan(XaotBundle *bundle,
                                                        const XiEnumData *enum_data,
                                                        uint32_t module_index, const XrType *type) {
    XaotEnumPlan *plan;
    char ctype[192];
    int argc = type_enum_arg_count(type);
    XrType **args = type_enum_args(type);

    if (!bundle || !enum_data || !enum_data->is_adt || !type || argc <= 0 || !args)
        return NULL;
    plan = (XaotEnumPlan *) xaot_bundle_find_enum_plan_for_type(bundle, type);
    if (plan)
        return plan;
    if (module_index >= bundle->nmodules)
        return NULL;
    if (bundle->nenum_plans == bundle->enum_plan_cap) {
        uint32_t new_cap = bundle->enum_plan_cap < 8 ? 8 : bundle->enum_plan_cap * 2;
        XaotEnumPlan *new_plans =
            (XaotEnumPlan *) xr_realloc(bundle->enum_plans, sizeof(XaotEnumPlan) * new_cap);
        if (!new_plans)
            return NULL;
        bundle->enum_plans = new_plans;
        bundle->enum_plan_cap = new_cap;
    }

    const XiModule *mod = bundle->modules ? bundle->modules[module_index] : NULL;
    xaot_enum_c_type_name_for_type(ctype, sizeof(ctype), mod && mod->name ? mod->name : "mod",
                                   enum_data, type);
    plan = &bundle->enum_plans[bundle->nenum_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->enum_data = enum_data;
    plan->concrete_type = type;
    plan->module_index = module_index;
    plan->member_count = enum_data->member_count;
    plan->layout_id = enum_data->layout_id;
    plan->max_payload = enum_data->max_payload > 0 ? (uint16_t) enum_data->max_payload : 0;
    plan->type_arg_count = (uint8_t) argc;
    plan->type_args = (XrType **) xr_calloc((size_t) argc, sizeof(XrType *));
    plan->c_type = xr_strdup(ctype);
    if (!plan->type_args || !plan->c_type) {
        bundle->nenum_plans--;
        xaot_enum_plan_free(plan);
        bundle->error_msg = "failed to allocate concrete AOT enum plan";
        return NULL;
    }
    for (int i = 0; i < argc; i++)
        plan->type_args[i] = args[i];

    if (enum_data->type_param_count > 0 && enum_data->type_param_names &&
        enum_data->type_param_count == (uint8_t) argc && enum_data->member_count > 0) {
        XiEnumMemberData *members =
            (XiEnumMemberData *) xr_calloc(enum_data->member_count, sizeof(XiEnumMemberData));
        if (!members) {
            bundle->nenum_plans--;
            xaot_enum_plan_free(plan);
            bundle->error_msg = "failed to allocate concrete AOT enum members";
            return NULL;
        }
        for (uint32_t mi = 0; mi < enum_data->member_count; mi++) {
            const XiEnumMemberData *src = enum_data->members ? &enum_data->members[mi] : NULL;
            members[mi].name = src ? src->name : NULL;
            members[mi].ordinal = src ? src->ordinal : mi;
            members[mi].payload_count = src ? src->payload_count : 0;
            if (src && src->payload_count > 0 && src->payload_types) {
                members[mi].payload_types =
                    (XrType **) xr_calloc((size_t) src->payload_count, sizeof(XrType *));
                if (!members[mi].payload_types) {
                    bundle->nenum_plans--;
                    plan->members = members;
                    plan->owns_members = true;
                    xaot_enum_plan_free(plan);
                    bundle->error_msg = "failed to allocate concrete AOT enum payload types";
                    return NULL;
                }
                for (int pi = 0; pi < src->payload_count; pi++) {
                    members[mi].payload_types[pi] = xr_type_substitute(
                        NULL, src->payload_types[pi], enum_data->type_param_names, args, argc);
                }
            }
        }
        plan->members = members;
        plan->owns_members = true;
    } else {
        plan->members = enum_data->members;
    }
    return plan;
}

XR_FUNC bool xaot_bundle_prepare_enum_plan_for_type(XaotBundle *bundle, const XrType *type) {
    const char *name = type_enum_name(type);
    int argc = type_enum_arg_count(type);
    uint32_t module_index = 0;
    const XiEnumData *ed;

    if (!bundle || !type || !name || argc <= 0)
        return true;
    if (xaot_bundle_find_enum_plan_for_type(bundle, type))
        return true;
    ed = find_enum_data_by_name(bundle, name, &module_index);
    if (!ed || ed->type_param_count == 0)
        return true;
    if (ed->type_param_count != (uint8_t) argc)
        return true;
    if (!xaot_bundle_add_concrete_enum_plan(bundle, ed, module_index, type)) {
        if (!bundle->error_msg)
            bundle->error_msg = "failed to allocate concrete AOT enum plan";
        return false;
    }
    return true;
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

XR_FUNC XaotSpanAccessPlan *xaot_bundle_add_span_access_plan(XaotBundle *bundle, const XiFunc *func,
                                                             const XiValue *value, uint8_t kind,
                                                             uint32_t evidence,
                                                             uint32_t eliminated_checks,
                                                             uint8_t unproven_reason) {
    XaotSpanAccessPlan *plan;

    if (!bundle || !func || !value || kind == 0 ||
        (eliminated_checks == 0) == (unproven_reason == XAOT_SPAN_UNPROVEN_NONE))
        return NULL;
    plan = (XaotSpanAccessPlan *) xaot_bundle_find_span_access_plan(bundle, value);
    if (plan)
        return plan;
    if (bundle->nspan_access_plans == bundle->span_access_plan_cap) {
        uint32_t new_cap =
            bundle->span_access_plan_cap < 16 ? 16 : bundle->span_access_plan_cap * 2;
        XaotSpanAccessPlan *new_plans = (XaotSpanAccessPlan *) xr_realloc(
            bundle->span_access_plans, sizeof(XaotSpanAccessPlan) * new_cap);
        if (!new_plans)
            return NULL;
        bundle->span_access_plans = new_plans;
        bundle->span_access_plan_cap = new_cap;
    }
    plan = &bundle->span_access_plans[bundle->nspan_access_plans++];
    memset(plan, 0, sizeof(*plan));
    plan->func = func;
    plan->value = value;
    plan->kind = kind;
    plan->evidence = evidence;
    plan->eliminated_checks = eliminated_checks;
    plan->unproven_reason = unproven_reason;
    if (!xaot_ptr_index_put(&bundle->span_access_index, value, bundle->nspan_access_plans - 1)) {
        bundle->error_msg = "failed to index AOT Span access plan";
        return NULL;
    }
    return plan;
}

XR_FUNC const XaotSpanAccessPlan *xaot_bundle_find_span_access_plan(const XaotBundle *bundle,
                                                                    const XiValue *value) {
    uint32_t idx;

    if (!bundle || !value)
        return NULL;
    if (xaot_ptr_index_get(&bundle->span_access_index, value, &idx) &&
        idx < bundle->nspan_access_plans)
        return &bundle->span_access_plans[idx];
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

static const char *span_access_kind_name(uint8_t kind) {
    switch ((XaotSpanAccessKind) kind) {
        case XAOT_SPAN_ACCESS_INDEX_GET:
            return "index_get";
        case XAOT_SPAN_ACCESS_INDEX_SET:
            return "index_set";
        case XAOT_SPAN_ACCESS_BYTE_LOAD:
            return "byte_load";
        case XAOT_SPAN_ACCESS_BYTE_STORE:
            return "byte_store";
        case XAOT_SPAN_ACCESS_BYTE_FILL:
            return "byte_fill";
        case XAOT_SPAN_ACCESS_BYTE_COPY:
            return "byte_copy";
        case XAOT_SPAN_ACCESS_BYTE_COMPARE:
            return "byte_compare";
        case XAOT_SPAN_ACCESS_BYTE_COMMON_PREFIX:
            return "byte_common_prefix";
        case XAOT_SPAN_ACCESS_BYTE_REPEAT:
            return "byte_repeat";
        case XAOT_SPAN_ACCESS_SPAN_AS_BYTES:
            return "span_as_bytes";
        case XAOT_SPAN_ACCESS_SPAN_FILL:
            return "span_fill";
        case XAOT_SPAN_ACCESS_SPAN_COPY:
            return "span_copy";
        case XAOT_SPAN_ACCESS_SPAN_COMPARE:
            return "span_compare";
        case XAOT_SPAN_ACCESS_REINTERPRET:
            return "reinterpret";
        default:
            return "unknown";
    }
}

static void print_span_access_bits(FILE *out, uint32_t bits, bool evidence) {
    bool first = true;
#define PRINT_BIT(mask, name)                                                                      \
    do {                                                                                           \
        if ((bits & (mask)) != 0) {                                                                \
            fprintf(out, "%s%s", first ? "" : "+", (name));                                        \
            first = false;                                                                         \
        }                                                                                          \
    } while (0)
    if (evidence) {
        PRINT_BIT(XAOT_SPAN_EV_RECV_AGGREGATE, "recv_agg");
        PRINT_BIT(XAOT_SPAN_EV_RECV_BYTE_SPAN, "byte_span");
        PRINT_BIT(XAOT_SPAN_EV_RECV_POD, "pod");
        PRINT_BIT(XAOT_SPAN_EV_ELEM_MATCH, "elem_match");
        PRINT_BIT(XAOT_SPAN_EV_WRITABLE, "writable");
        PRINT_BIT(XAOT_SPAN_EV_RANGE_PROVEN, "range");
        PRINT_BIT(XAOT_SPAN_EV_LENGTH_REL_PROVEN, "len_rel");
        PRINT_BIT(XAOT_SPAN_EV_BYTE_LEN_NO_OVERFLOW, "no_overflow");
        PRINT_BIT(XAOT_SPAN_EV_DATA_VALID, "data_valid");
        PRINT_BIT(XAOT_SPAN_EV_ENDIAN_CONST, "endian_const");
        PRINT_BIT(XAOT_SPAN_EV_NO_CLOBBER, "no_clobber");
    } else {
        PRINT_BIT(XAOT_SPAN_DROP_BOUNDS, "bounds");
        PRINT_BIT(XAOT_SPAN_DROP_READONLY, "readonly");
        PRINT_BIT(XAOT_SPAN_DROP_TYPE, "type");
        PRINT_BIT(XAOT_SPAN_DROP_POD, "pod");
        PRINT_BIT(XAOT_SPAN_DROP_NULL_DATA, "null_data");
        PRINT_BIT(XAOT_SPAN_DROP_OVERFLOW, "overflow");
        PRINT_BIT(XAOT_SPAN_DROP_HELPER, "helper");
    }
    if (first)
        fprintf(out, "none");
#undef PRINT_BIT
}

static const char *span_access_reason_name(uint8_t reason) {
    switch (reason) {
        case XAOT_SPAN_UNPROVEN_NONE:
            return "none";
        case XAOT_SPAN_UNPROVEN_DYNAMIC_RECV:
            return "dynamic_recv";
        case XAOT_SPAN_UNPROVEN_NOT_BYTE_SPAN:
            return "not_byte_span";
        case XAOT_SPAN_UNPROVEN_NOT_POD:
            return "not_pod";
        case XAOT_SPAN_UNPROVEN_READONLY_MAYBE:
            return "readonly_maybe";
        case XAOT_SPAN_UNPROVEN_RANGE:
            return "range";
        case XAOT_SPAN_UNPROVEN_LENGTH_REL:
            return "length_rel";
        case XAOT_SPAN_UNPROVEN_OVERFLOW:
            return "overflow";
        case XAOT_SPAN_UNPROVEN_DATA_NULL:
            return "data_null";
        case XAOT_SPAN_UNPROVEN_ENDIAN_DYNAMIC:
            return "endian_dynamic";
        case XAOT_SPAN_UNPROVEN_CLOBBER:
            return "clobber";
        case XAOT_SPAN_UNPROVEN_DYNAMIC_BOUNDARY:
            return "dynamic_boundary";
        case XAOT_SPAN_UNPROVEN_ELEM_MISMATCH:
            return "elem_mismatch";
        default:
            return "unknown";
    }
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

    for (uint32_t ei = 0; ei < bundle->nenum_plans; ei++) {
        const XaotEnumPlan *ep = &bundle->enum_plans[ei];
        const XiEnumData *ed = ep->enum_data;
        fprintf(out,
                "enum %u name=%s module=%u members=%u layout_id=%u max_payload=%u "
                "type_args=%u c_type=%s\n",
                ei, safe_str(ed ? ed->name : NULL), ep->module_index, ep->member_count,
                ep->layout_id, (unsigned) ep->max_payload, (unsigned) ep->type_arg_count,
                safe_str(ep->c_type));
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

    for (uint32_t ai = 0; ai < bundle->nspan_access_plans; ai++) {
        const XaotSpanAccessPlan *sp = &bundle->span_access_plans[ai];
        char value_buf[32];
        value_ref(value_buf, sizeof(value_buf), sp->value);
        if (sp->eliminated_checks != 0) {
            fprintf(out, "span-access %u func=%s value=%s kind=%s evidence=", ai,
                    safe_str(sp->func ? sp->func->name : NULL), value_buf,
                    span_access_kind_name(sp->kind));
            print_span_access_bits(out, sp->evidence, true);
            fprintf(out, " drop=");
            print_span_access_bits(out, sp->eliminated_checks, false);
            fprintf(out, "\n");
        } else {
            fprintf(out, "span-access-unproven %u func=%s value=%s kind=%s evidence=", ai,
                    safe_str(sp->func ? sp->func->name : NULL), value_buf,
                    span_access_kind_name(sp->kind));
            print_span_access_bits(out, sp->evidence, true);
            fprintf(out, " reason=%s\n", span_access_reason_name(sp->unproven_reason));
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
