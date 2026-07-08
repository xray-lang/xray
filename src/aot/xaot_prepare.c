/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_prepare.c - AOT prepare pass
 */

#include "xaot_prepare.h"
#include "xaot_boundary.h"
#include "xaot_class_native.h"
#include "../base/xglobal_indices.h"
#include "../base/xmalloc.h"
#include "../ir/xi_analysis.h"
#include "../ir/xi_coro_analyze.h"
#include "../ir/xi_effect.h"
#include "../ir/xi_range.h"
#include "../ir/xi_value_query.h"
#include "../shared/xr_array_core.h"
#include <stdlib.h>
#include <string.h>

static bool value_reps_equal(XaotValueRep a, XaotValueRep b) {
    return xaot_value_reps_equal(a, b);
}

static XaotValueRep ptr_value_rep_for_type(const XrType *type) {
    XaotValueRep rep;
    memset(&rep, 0, sizeof(rep));
    rep.kind = XAOT_VALUE_PTR;
    rep.rep = XAOT_REP_PTR;
    rep.type = type;
    rep.c_type = "void *";
    return rep;
}

static bool prepare_bundle_is_freestanding(const XaotBundle *bundle) {
    return bundle && bundle->global_evidence_plan.profile == XG_BUILD_FREESTANDING;
}

static bool prepare_type_is_freestanding_ordinal_enum(const XaotBundle *bundle,
                                                      const XrType *type) {
    return prepare_bundle_is_freestanding(bundle) && type && !type->is_nullable &&
           type->kind == XR_KIND_ENUM && type->enum_type.layout &&
           type->enum_type.layout->is_zero_payload;
}

static XaotValueRep prepare_enum_ordinal_value_rep(const XrType *type) {
    XaotValueRep rep;
    memset(&rep, 0, sizeof(rep));
    rep.kind = XAOT_VALUE_SCALAR;
    rep.rep = XAOT_REP_I64;
    rep.type = type;
    rep.c_type = "int64_t";
    rep.flags = XAOT_VALUE_FLAG_ENUM;
    return rep;
}

static bool prepare_value_is_freestanding_ordinal_enum_member(const XaotBundle *bundle,
                                                              const XiFunc *func,
                                                              const XiValue *value);
static bool prepare_value_is_freestanding_ordinal_enum_compare_member(const XaotBundle *bundle,
                                                                      const XiFunc *func,
                                                                      const XiValue *value);

static void apply_freestanding_enum_ordinal_value_plan(XaotBundle *bundle, XaotValuePlan *vp) {
    if (!vp)
        return;
    if (!prepare_type_is_freestanding_ordinal_enum(bundle, vp->value ? vp->value->type : NULL) &&
        !prepare_value_is_freestanding_ordinal_enum_member(bundle, vp->func, vp->value) &&
        !prepare_value_is_freestanding_ordinal_enum_compare_member(bundle, vp->func, vp->value))
        return;
    vp->rep = prepare_enum_ordinal_value_rep(vp->value->type);
}

static bool value_can_use_native_class_ptr(const XaotBundle *bundle, const XiValue *value) {
    const XrType *type = value ? value->type : NULL;
    if (!bundle || !type || type->is_nullable ||
        (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE) ||
        !type->instance.class_name || !xaot_class_native_data_for_type(bundle, type))
        return false;

    switch (value->op) {
        case XI_CLASS_CREATE:
        case XI_GET_SHARED:
        case XI_IMPORT_REF:
            return false;
        default:
            return true;
    }
}

static bool native_ref_field_type(const XrType *type, uint8_t *out_native) {
    if (!type)
        return false;
    switch (type->kind) {
        case XR_KIND_STRING:
            if (out_native)
                *out_native = XR_NATIVE_STRING;
            return true;
        case XR_KIND_ARRAY:
        case XR_KIND_VIEW:
        case XR_KIND_SPAN:
            if (out_native)
                *out_native = XR_NATIVE_ARRAY_REF;
            return true;
        case XR_KIND_MAP:
            if (out_native)
                *out_native = XR_NATIVE_MAP_REF;
            return true;
        case XR_KIND_SET:
            if (out_native)
                *out_native = XR_NATIVE_SET_REF;
            return true;
        default:
            return false;
    }
}

static bool value_can_use_native_ref_field_ptr(const XaotBundle *bundle, const XiFunc *func,
                                               const XiValue *value) {
    uint8_t expected_native = 0;
    if (!bundle || !func || !value || !native_ref_field_type(value->type, &expected_native))
        return false;
    return xaot_class_native_receiver_ref_field(bundle, func, value, expected_native, NULL, NULL);
}

static void apply_native_class_ptr_value_plan(XaotBundle *bundle, XaotValuePlan *vp) {
    if (!vp)
        return;
    if (value_can_use_native_class_ptr(bundle, vp->value) ||
        value_can_use_native_ref_field_ptr(bundle, vp->func, vp->value))
        vp->rep = ptr_value_rep_for_type(vp->value->type);
}

static void record_value_stats(XaotPrepareStats *stats, XaotValueKind kind) {
    if (!stats)
        return;
    stats->values_total++;
    switch (kind) {
        case XAOT_VALUE_SCALAR:
            stats->values_scalar++;
            break;
        case XAOT_VALUE_TAGGED:
            stats->values_tagged++;
            break;
        case XAOT_VALUE_PTR:
            stats->values_ptr++;
            break;
        case XAOT_VALUE_AGGREGATE:
            stats->values_aggregate++;
            break;
        case XAOT_VALUE_VIEW:
            stats->values_view++;
            break;
        case XAOT_VALUE_VOID:
            stats->values_void++;
            break;
        default:
            break;
    }
}

static void record_container_stats(XaotPrepareStats *stats, const XaotContainerPlan *plan) {
    if (!stats || !plan)
        return;
    stats->containers_total++;
    if ((plan->flags & XAOT_CONTAINER_DIRECT_HELPERS) != 0)
        stats->containers_direct++;
    switch (plan->kind) {
        case XAOT_CONTAINER_ARRAY:
            stats->containers_array++;
            break;
        case XAOT_CONTAINER_MAP:
            stats->containers_map++;
            break;
        case XAOT_CONTAINER_SET:
            stats->containers_set++;
            break;
        default:
            break;
    }
}

static void record_array_storage_stats(XaotPrepareStats *stats, const XaotArrayStoragePlan *plan) {
    if (!stats || !plan)
        return;
    stats->array_storage_total++;
    if ((plan->flags & XAOT_ARRAY_STORAGE_READ) != 0)
        stats->array_storage_read++;
    if ((plan->flags & XAOT_ARRAY_STORAGE_MUTABLE) != 0)
        stats->array_storage_mutable++;
}

static void record_array_cache_stats(XaotPrepareStats *stats, const XaotArrayCachePlan *plan) {
    if (!stats || !plan)
        return;
    stats->array_cache_total++;
    if ((plan->flags & XAOT_ARRAY_CACHE_READ) != 0)
        stats->array_cache_read++;
    if ((plan->flags & XAOT_ARRAY_CACHE_MUTABLE) != 0)
        stats->array_cache_mutable++;
}

static const XiValue *unwrap_identity_value(const XiValue *v) {
    while (v &&
           (v->op == XI_BOX || v->op == XI_UNBOX || xi_copy_is_identity_alias(v) ||
            v->op == XI_MOVE) &&
           v->nargs >= 1) {
        v = v->args[0];
    }
    return v;
}

static bool same_value(const XiValue *a, const XiValue *b) {
    return unwrap_identity_value(a) == unwrap_identity_value(b);
}

static int prepare_enum_member_index(const XiEnumData *ed, const char *member_name) {
    if (!ed || !member_name)
        return -1;
    for (uint32_t i = 0; i < ed->member_count; i++) {
        if (ed->members && ed->members[i].name && strcmp(ed->members[i].name, member_name) == 0)
            return (int) i;
    }
    return -1;
}

static bool prepare_enum_data_is_zero_payload_simple(const XiEnumData *ed) {
    if (!ed || ed->is_adt)
        return false;
    for (uint32_t i = 0; i < ed->member_count; i++) {
        if (ed->members && ed->members[i].payload_count != 0)
            return false;
    }
    return true;
}

static const XiEnumData *prepare_enum_for_import_ref(const XaotBundle *bundle,
                                                     const XiImportRef *ref) {
    if (!bundle || !ref || ref->resolved_mod_index < 0 || ref->resolved_shared_slot < 0 ||
        (uint32_t) ref->resolved_mod_index >= bundle->nmodules)
        return NULL;
    const XiModule *mod = bundle->modules ? bundle->modules[ref->resolved_mod_index] : NULL;
    if (!mod || !mod->slot_enums || ref->resolved_shared_slot >= (int) mod->nslots)
        return NULL;
    return mod->slot_enums[ref->resolved_shared_slot];
}

static const XiEnumData *prepare_enum_for_shared_slot(const XaotBundle *bundle, const XiFunc *func,
                                                      int slot) {
    if (!func || !func->module || slot < 0 || slot >= (int) func->module->nslots)
        return NULL;
    if (func->module->slot_enums && func->module->slot_enums[slot])
        return func->module->slot_enums[slot];
    if (func->module->slot_imports && func->module->slot_imports[slot])
        return prepare_enum_for_import_ref(bundle, func->module->slot_imports[slot]);
    return NULL;
}

static const XiEnumData *prepare_enum_for_shared_value(const XaotBundle *bundle, const XiFunc *func,
                                                       const XiValue *value) {
    const XiValue *origin = unwrap_identity_value(value);
    if (!origin)
        return NULL;
    if (origin->op == XI_GET_SHARED)
        return prepare_enum_for_shared_slot(bundle, func, (int) origin->aux_int);
    if (origin->op == XI_IMPORT_REF && origin->aux)
        return prepare_enum_for_import_ref(bundle, (const XiImportRef *) origin->aux);
    return NULL;
}

static bool prepare_value_is_namespace_field_load(const XiValue *value) {
    const XiValue *origin = unwrap_identity_value(value);
    if (!origin || origin->op != XI_LOAD_FIELD || origin->nargs < 1 || !origin->aux)
        return false;
    const XiValue *receiver = unwrap_identity_value(origin->args[0]);
    return receiver && (receiver->op == XI_GET_SHARED || receiver->op == XI_IMPORT_REF);
}

static bool prepare_value_plan_is_i64_enum(const XaotBundle *bundle, const XiValue *value) {
    const XiValue *origin = unwrap_identity_value(value);
    if (!origin || !origin->type || origin->type->is_nullable || origin->type->kind != XR_KIND_ENUM)
        return false;
    const XaotValuePlan *plan = xaot_bundle_find_value_plan(bundle, origin);
    return plan && xaot_value_storage_rep(plan->rep) == XR_REP_I64;
}

static bool prepare_value_is_freestanding_ordinal_enum_member(const XaotBundle *bundle,
                                                              const XiFunc *func,
                                                              const XiValue *value) {
    if (!prepare_bundle_is_freestanding(bundle))
        return false;
    const XiValue *origin = unwrap_identity_value(value);
    if (!origin || origin->op != XI_LOAD_FIELD || origin->nargs < 1 || !origin->aux)
        return false;
    const XiEnumData *ed = prepare_enum_for_shared_value(bundle, func, origin->args[0]);
    int member_index = prepare_enum_member_index(ed, (const char *) origin->aux);
    if (!prepare_enum_data_is_zero_payload_simple(ed) || member_index < 0)
        return false;
    return !ed->members || ed->members[member_index].payload_count == 0;
}

static bool prepare_value_is_freestanding_ordinal_enum_compare_member(const XaotBundle *bundle,
                                                                      const XiFunc *func,
                                                                      const XiValue *value) {
    if (!prepare_bundle_is_freestanding(bundle) || !func ||
        !prepare_value_is_namespace_field_load(value))
        return false;
    const XiValue *target = unwrap_identity_value(value);
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || (user->op != XI_EQ && user->op != XI_NE) || user->nargs < 2)
                continue;
            const XiValue *lhs = unwrap_identity_value(user->args[0]);
            const XiValue *rhs = unwrap_identity_value(user->args[1]);
            if (lhs == target &&
                (prepare_type_is_freestanding_ordinal_enum(bundle, rhs ? rhs->type : NULL) ||
                 prepare_value_plan_is_i64_enum(bundle, rhs)))
                return true;
            if (rhs == target &&
                (prepare_type_is_freestanding_ordinal_enum(bundle, lhs ? lhs->type : NULL) ||
                 prepare_value_plan_is_i64_enum(bundle, lhs)))
                return true;
        }
    }
    return false;
}

static bool prepare_container_type(XaotBundle *bundle, const XrType *type) {
    XaotContainerPlan scratch;
    XaotContainerTypePlan *plan;

    if (!bundle || !type)
        return true;
    if (xaot_bundle_find_container_plan(bundle, type))
        return true;
    if (!xaot_container_plan_for_type(type, &scratch))
        return true;
    plan = xaot_bundle_add_container_plan(bundle, type);
    if (!plan) {
        bundle->error_msg = "failed to allocate AOT container plan";
        return false;
    }
    record_container_stats(&bundle->stats, &plan->plan);
    return true;
}

static bool prepare_type_plans_for_type(XaotBundle *bundle, const XrType *type, int depth) {
    if (!bundle || !type || depth > 8)
        return true;
    if (!prepare_container_type(bundle, type))
        return false;
    if (!xaot_bundle_prepare_enum_plan_for_type(bundle, type))
        return false;
    switch ((XrTypeKind) type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
        case XR_KIND_SPAN:
        case XR_KIND_VIEW:
        case XR_KIND_POINTER:
            return prepare_type_plans_for_type(bundle, type->container.element_type, depth + 1);
        case XR_KIND_MAP:
            return prepare_type_plans_for_type(bundle, type->map.key_type, depth + 1) &&
                   prepare_type_plans_for_type(bundle, type->map.value_type, depth + 1);
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
        case XR_KIND_INTERFACE:
            for (int i = 0; i < type->instance.type_arg_count; i++) {
                if (!prepare_type_plans_for_type(
                        bundle, type->instance.type_args ? type->instance.type_args[i] : NULL,
                        depth + 1))
                    return false;
            }
            return true;
        case XR_KIND_UNION:
            for (uint8_t i = 0; i < type->union_type.member_count; i++) {
                if (!prepare_type_plans_for_type(
                        bundle, type->union_type.members ? type->union_type.members[i] : NULL,
                        depth + 1))
                    return false;
            }
            return true;
        case XR_KIND_TUPLE:
            for (int i = 0; i < type->tuple.element_count; i++) {
                if (!prepare_type_plans_for_type(
                        bundle, type->tuple.element_types ? type->tuple.element_types[i] : NULL,
                        depth + 1))
                    return false;
            }
            return true;
        case XR_KIND_FIXED_ARRAY:
            return prepare_type_plans_for_type(bundle, type->fixed_array.element_type, depth + 1);
        case XR_KIND_FUNCTION:
            for (int i = 0; i < type->function.param_count; i++) {
                if (!prepare_type_plans_for_type(
                        bundle, type->function.param_types ? type->function.param_types[i] : NULL,
                        depth + 1))
                    return false;
            }
            return prepare_type_plans_for_type(bundle, type->function.return_type, depth + 1);
        default:
            return true;
    }
}

static bool array_elem_plan_for_value(const XaotBundle *bundle, const XiValue *value,
                                      XaotContainerElemPlan *out) {
    const XaotContainerTypePlan *container;

    if (!bundle || !value || !out)
        return false;
    container = xaot_bundle_find_container_plan(bundle, value->type);
    if (!container || container->plan.kind != XAOT_CONTAINER_ARRAY ||
        (container->plan.flags & XAOT_CONTAINER_RAW_DATA) == 0)
        return false;
    *out = container->plan.elem;
    return true;
}

static bool array_value_is_class_field_storage(const XaotBundle *bundle, const XiValue *value,
                                               XaotContainerElemPlan *out_elem,
                                               const XiValue **out_origin) {
    const XiValue *target = unwrap_identity_value(value);
    const XiFunc *func = target && target->block ? target->block->func : NULL;
    XaotContainerElemPlan elem;

    if (!bundle || !target || !func || !array_elem_plan_for_value(bundle, target, &elem))
        return false;
    if (!xaot_class_native_receiver_ref_field(bundle, func, target, XR_NATIVE_ARRAY_REF, NULL,
                                              NULL))
        return false;
    if (out_elem)
        *out_elem = elem;
    if (out_origin)
        *out_origin = target;
    return true;
}

static bool prepare_array_native_receiver_array_store_info(const XaotBundle *bundle,
                                                           const XiFunc *func, const XiValue *value,
                                                           uint16_t expected_idx,
                                                           XaotClassNativeFunc *out_info,
                                                           uint16_t *out_idx) {
    XaotClassNativeFunc info;
    uint16_t idx = 0;

    if (out_info)
        memset(out_info, 0, sizeof(*out_info));
    if (out_idx)
        *out_idx = 0;
    if (!bundle || !func || !value || value->op != XI_STORE_FIELD || value->nargs < 2)
        return false;
    if (!xaot_class_native_receiver_store_field(bundle, func, value, XR_NATIVE_ARRAY_REF, &info,
                                                &idx))
        return false;
    if (expected_idx != UINT16_MAX && idx != expected_idx)
        return false;
    if (out_info)
        *out_info = info;
    if (out_idx)
        *out_idx = idx;
    return true;
}

static bool array_builtin_is_fresh_storage(const XiValue *value) {
    const char *name;

    if (!value || value->op != XI_CALL_BUILTIN || !value->aux)
        return false;
    name = (const char *) value->aux;
    return strcmp(name, "array_new") == 0 || strcmp(name, "Bytes") == 0 ||
           strcmp(name, "array_with_capacity") == 0 || strcmp(name, "array_filled_new") == 0;
}

static bool array_builtin_forwards_storage(const XiValue *value) {
    const char *name;

    if (!value || value->op != XI_CALL_BUILTIN || !value->aux || value->nargs < 1)
        return false;
    name = (const char *) value->aux;
    return strcmp(name, "array_reserve") == 0 || strcmp(name, "array_resize") == 0;
}

static bool array_method_is_hof_result(const XiValue *value) {
    const char *method;

    if (!value || value->op != XI_CALL_METHOD || !value->aux || value->nargs < 1)
        return false;
    method = (const char *) value->aux;
    return strcmp(method, "map") == 0 || strcmp(method, "filter") == 0;
}

static bool derive_array_storage_plan(const XaotBundle *bundle, const XiValue *array_value,
                                      uint32_t required_flag, XaotContainerElemPlan *out_elem,
                                      const XiValue **out_origin, uint8_t depth) {
    const XiValue *value = unwrap_identity_value(array_value);
    XaotContainerElemPlan self_elem;

    if (!bundle || !value || depth > 8)
        return false;

    if ((required_flag & XAOT_ARRAY_STORAGE_READ) != 0 && value->op == XI_SLICE) {
        return value->nargs >= 1 &&
               derive_array_storage_plan(bundle, value->args[0], XAOT_ARRAY_STORAGE_READ, out_elem,
                                         out_origin, (uint8_t) (depth + 1));
    }

    if (value->op == XI_PHI) {
        bool has_base = false;
        const XiValue *origin = NULL;
        XaotContainerElemPlan first_elem;
        memset(&first_elem, 0, sizeof(first_elem));
        if (value->nargs == 0)
            return false;
        for (uint16_t i = 0; i < value->nargs; i++) {
            XaotContainerElemPlan arg_elem;
            const XiValue *arg_origin = NULL;
            const XiValue *arg = unwrap_identity_value(value->args[i]);
            if (arg == value)
                continue;
            if (!derive_array_storage_plan(bundle, arg, required_flag, &arg_elem, &arg_origin,
                                           (uint8_t) (depth + 1)))
                return false;
            if (!has_base) {
                first_elem = arg_elem;
                origin = arg_origin;
                has_base = true;
            } else if (!first_elem.elem_name || !arg_elem.elem_name ||
                       strcmp(first_elem.elem_name, arg_elem.elem_name) != 0) {
                return false;
            } else if (origin != arg_origin) {
                origin = value;
            }
        }
        if (!has_base)
            return false;
        if (out_elem)
            *out_elem = first_elem;
        if (out_origin)
            *out_origin = origin ? origin : value;
        return true;
    }

    if (!array_elem_plan_for_value(bundle, value, &self_elem))
        return false;

    /* Typed array class fields serve reads and writes alike: the element
     * type is static, and the write paths stay runtime-checked unless a
     * separate proof (fill loop, bounds plan) upgrades them. */
    if (array_value_is_class_field_storage(bundle, value, out_elem, out_origin))
        return true;

    if (value->op == XI_ARRAY_NEW || array_builtin_is_fresh_storage(value)) {
        if (out_elem)
            *out_elem = self_elem;
        if (out_origin)
            *out_origin = value;
        return true;
    }

    if ((required_flag & XAOT_ARRAY_STORAGE_READ) != 0 && value->op == XI_PARAM) {
        if (out_elem)
            *out_elem = self_elem;
        if (out_origin)
            *out_origin = value;
        return true;
    }

    if ((required_flag & XAOT_ARRAY_STORAGE_MUTABLE) != 0 && value->op == XI_PARAM && value->type &&
        value->type->kind == XR_KIND_ARRAY) {
        if (out_elem)
            *out_elem = self_elem;
        if (out_origin)
            *out_origin = value;
        return true;
    }

    if (value->op == XI_LOAD_UPVAL && value->type && value->type->kind == XR_KIND_ARRAY) {
        bool wants_read = (required_flag & XAOT_ARRAY_STORAGE_READ) != 0;
        bool wants_mutable = (required_flag & XAOT_ARRAY_STORAGE_MUTABLE) != 0;
        if (wants_read || (wants_mutable && self_elem.storage_rep != XR_REP_TAGGED)) {
            if (out_elem)
                *out_elem = self_elem;
            if (out_origin)
                *out_origin = value;
            return true;
        }
    }

    if (value->op == XI_AWAIT && value->type && value->type->kind == XR_KIND_ARRAY) {
        bool wants_read = (required_flag & XAOT_ARRAY_STORAGE_READ) != 0;
        bool wants_mutable = (required_flag & XAOT_ARRAY_STORAGE_MUTABLE) != 0;
        if (wants_read || (wants_mutable && self_elem.storage_rep != XR_REP_TAGGED)) {
            if (out_elem)
                *out_elem = self_elem;
            if (out_origin)
                *out_origin = value;
            return true;
        }
    }

    if (array_builtin_forwards_storage(value)) {
        return derive_array_storage_plan(bundle, value->args[0], required_flag, out_elem,
                                         out_origin, (uint8_t) (depth + 1));
    }

    if (array_method_is_hof_result(value)) {
        if (out_elem)
            *out_elem = self_elem;
        if (out_origin)
            *out_origin = value;
        return true;
    }

    return false;
}

static bool prepare_array_storage_value(XaotBundle *bundle, const XiFunc *func,
                                        const XiValue *value) {
    XaotContainerElemPlan read_elem;
    XaotContainerElemPlan mutable_elem;
    const XiValue *read_origin = NULL;
    const XiValue *mutable_origin = NULL;
    const XiValue *target = unwrap_identity_value(value);
    uint32_t flags = 0;

    if (!bundle || !func || !target)
        return false;
    if (xaot_bundle_find_array_storage_plan(bundle, target))
        return true;
    if (derive_array_storage_plan(bundle, target, XAOT_ARRAY_STORAGE_READ, &read_elem, &read_origin,
                                  0))
        flags |= XAOT_ARRAY_STORAGE_READ;
    if (derive_array_storage_plan(bundle, target, XAOT_ARRAY_STORAGE_MUTABLE, &mutable_elem,
                                  &mutable_origin, 0))
        flags |= XAOT_ARRAY_STORAGE_MUTABLE;
    if (flags == 0)
        return true;

    const XaotContainerElemPlan *elem =
        (flags & XAOT_ARRAY_STORAGE_READ) != 0 ? &read_elem : &mutable_elem;
    const XiValue *origin = (flags & XAOT_ARRAY_STORAGE_READ) != 0 ? read_origin : mutable_origin;
    uint32_t before = bundle->narray_storage_plans;
    XaotArrayStoragePlan *plan =
        xaot_bundle_add_array_storage_plan(bundle, func, target, origin, flags, elem);
    if (!plan) {
        bundle->error_msg = "failed to allocate AOT array storage plan";
        return false;
    }
    if (bundle->narray_storage_plans != before)
        record_array_storage_stats(&bundle->stats, plan);
    return true;
}

static bool prepare_func_array_storage_plans(XaotBundle *bundle, XiFunc *func) {
    uint32_t bi;

    if (!bundle || !func)
        return false;
    for (bi = 0; bi < func->nblocks; bi++) {
        XiBlock *blk = func->blocks[bi];
        XiPhi *phi;
        uint32_t vi;
        if (!blk)
            continue;
        for (phi = blk->phis; phi; phi = phi->next) {
            if (!prepare_array_storage_value(bundle, func, &phi->value))
                return false;
        }
        for (vi = 0; vi < blk->nvalues; vi++) {
            if (!prepare_array_storage_value(bundle, func, blk->values[vi]))
                return false;
        }
    }
    return true;
}

static bool value_arg_matches(const XiValue *value, const XiValue *target, uint16_t first_arg) {
    if (!value || !target)
        return false;
    for (uint16_t i = first_arg; i < value->nargs; i++) {
        if (same_value(value->args[i], target))
            return true;
    }
    return false;
}

static bool array_value_has_uncacheable_use(const XiValue *value) {
    const XiValue *target = unwrap_identity_value(value);
    const XiFunc *func = target && target->block ? target->block->func : NULL;

    if (!func || !target)
        return true;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur == target)
                continue;
            switch ((XiOp) cur->op) {
                case XI_INDEX_GET:
                case XI_LOAD_FIELD:
                    break;
                case XI_INDEX_SET:
                case XI_STORE_FIELD:
                case XI_CALL:
                case XI_CALL_BUILTIN:
                case XI_CALL_METHOD:
                case XI_CALL_METHOD_DIRECT:
                case XI_GO:
                case XI_THREAD_SPAWN:
                case XI_SET_SHARED:
                    if (value_arg_matches(cur, target, 0))
                        return true;
                    break;
                default:
                    if ((cur->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)) &&
                        value_arg_matches(cur, target, 0))
                        return true;
                    break;
            }
        }
    }
    return false;
}

static bool array_value_is_cacheable_view(const XiValue *value) {
    const XiValue *target = unwrap_identity_value(value);

    return target && target->op == XI_SLICE;
}

typedef struct PrepareArrayFillLoop {
    const XiValue *origin;
    const XiValue *storage_value;
    const XiValue *cap_value;
    const XiValue *push;
    const XiValue *index_value;
    const XiValue *next_index_value;
    const XiBlock *exit_block;
} PrepareArrayFillLoop;

static bool prepare_func_has_exception_handling(const XiFunc *func) {
    if (!func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *value = blk->values[vi];
            if (value && value->op == XI_TRY)
                return true;
        }
    }
    return false;
}

static uint16_t prepare_array_pred_index(const XiBlock *blk, const XiBlock *pred) {
    if (!blk || !pred)
        return UINT16_MAX;
    for (uint16_t i = 0; i < blk->npreds; i++) {
        if (blk->preds[i] == pred)
            return i;
    }
    return UINT16_MAX;
}

static const XiValue *prepare_array_single_origin(const XiValue *array_value, uint8_t depth) {
    const XiValue *value = unwrap_identity_value(array_value);
    if (!value || depth > 8)
        return NULL;
    if (value->op == XI_ARRAY_NEW)
        return value;
    if (value->op == XI_CALL_BUILTIN) {
        const char *name = (const char *) value->aux;
        if (name && (strcmp(name, "array_new") == 0 || strcmp(name, "Bytes") == 0))
            return value;
    }
    if (value->op != XI_PHI)
        return NULL;

    const XiValue *origin = NULL;
    for (uint16_t i = 0; i < value->nargs; i++) {
        const XiValue *arg = unwrap_identity_value(value->args[i]);
        if (arg == value)
            continue;
        const XiValue *arg_origin = prepare_array_single_origin(arg, (uint8_t) (depth + 1));
        if (!arg_origin)
            return NULL;
        if (origin && origin != arg_origin)
            return NULL;
        origin = arg_origin;
    }
    return origin;
}

static bool prepare_array_value_precedes_in_block(const XiValue *before, const XiValue *after) {
    if (!before || !after || before->block != after->block)
        return false;
    for (uint32_t i = 0; i < after->block->nvalues; i++) {
        const XiValue *cur = after->block->values[i];
        if (cur == before)
            return true;
        if (cur == after)
            return false;
    }
    return false;
}

static const XiValue *prepare_array_class_field_fresh_store_origin(const XaotBundle *bundle,
                                                                   const XiFunc *func,
                                                                   const XiValue *field_value,
                                                                   const XiValue *site) {
    XaotClassNativeFunc info;
    uint16_t field_idx = 0;
    const XiValue *target = unwrap_identity_value(field_value);
    const XiValue *store = NULL;
    const XiValue *origin = NULL;

    if (!bundle || !func || !target || !site ||
        !xaot_class_native_receiver_ref_field(bundle, func, target, XR_NATIVE_ARRAY_REF, &info,
                                              &field_idx))
        return NULL;

    xi_ensure_dominators((XiFunc *) func);

    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            const XiValue *cur_origin;
            if (!prepare_array_native_receiver_array_store_info(bundle, func, cur, field_idx, NULL,
                                                                NULL))
                continue;
            cur_origin = prepare_array_single_origin(cur->args[1], 0);
            if (!cur_origin || store)
                return NULL;
            store = cur;
            origin = cur_origin;
        }
    }
    if (!store || !origin || !xi_dominates(store->block, site->block))
        return NULL;
    if (store->block == site->block && !prepare_array_value_precedes_in_block(store, site))
        return NULL;
    return origin;
}

static bool prepare_array_const_int_value(const XiValue *value, int64_t expected) {
    value = unwrap_identity_value(value);
    return value && value->op == XI_CONST && value->type && value->type->kind == XR_KIND_INT &&
           value->aux_int == expected;
}

static bool prepare_array_is_add_one_from_phi(const XiValue *value, const XiValue *phi) {
    value = unwrap_identity_value(value);
    if (!value || !phi || value->op != XI_ADD || value->nargs < 2)
        return false;
    const XiValue *lhs = unwrap_identity_value(value->args[0]);
    const XiValue *rhs = unwrap_identity_value(value->args[1]);
    return (lhs == phi && prepare_array_const_int_value(rhs, 1)) ||
           (rhs == phi && prepare_array_const_int_value(lhs, 1));
}

static const XiValue *prepare_array_phi_from_add_one(const XiValue *value) {
    value = unwrap_identity_value(value);
    if (!value || value->op != XI_ADD || value->nargs < 2)
        return NULL;
    const XiValue *lhs = unwrap_identity_value(value->args[0]);
    const XiValue *rhs = unwrap_identity_value(value->args[1]);
    if (lhs && lhs->op == XI_PHI && prepare_array_const_int_value(rhs, 1))
        return lhs;
    if (rhs && rhs->op == XI_PHI && prepare_array_const_int_value(lhs, 1))
        return rhs;
    return NULL;
}

static const XiValue *prepare_array_loop_bound_base(const XiValue *bound, const XiBlock *guard,
                                                    const XiBlock *body) {
    const XiValue *value = unwrap_identity_value(bound);
    if (!value)
        return NULL;
    if (value->op != XI_PHI || value->block != guard)
        return value;

    uint16_t body_idx = prepare_array_pred_index(guard, body);
    if (body_idx == UINT16_MAX || body_idx >= value->nargs)
        return NULL;

    const XiValue *base = NULL;
    for (uint16_t i = 0; i < value->nargs; i++) {
        const XiValue *arg = unwrap_identity_value(value->args[i]);
        if (i == body_idx) {
            if (arg != value)
                return NULL;
            continue;
        }
        if (!arg)
            return NULL;
        if (base && base != arg)
            return NULL;
        base = arg;
    }
    return base;
}

static bool prepare_array_loop_index_is_counted(const XiValue *index, const XiBlock *guard,
                                                const XiBlock *body) {
    const XiValue *phi = unwrap_identity_value(index);
    if (!phi || phi->op != XI_PHI || phi->block != guard)
        return false;
    uint16_t body_idx = prepare_array_pred_index(guard, body);
    if (body_idx == UINT16_MAX || body_idx >= phi->nargs)
        return false;

    bool has_zero_base = false;
    for (uint16_t i = 0; i < phi->nargs; i++) {
        const XiValue *arg = unwrap_identity_value(phi->args[i]);
        if (i != body_idx) {
            if (!prepare_array_const_int_value(arg, 0))
                return false;
            has_zero_base = true;
            continue;
        }
        if (!prepare_array_is_add_one_from_phi(arg, phi))
            return false;
    }
    return has_zero_base;
}

static bool prepare_array_block_allows_unchecked_push(const XiBlock *body, const XiValue *push) {
    if (!body || !push)
        return false;
    bool seen_push = false;
    for (uint32_t i = 0; i < body->nvalues; i++) {
        const XiValue *value = body->values[i];
        if (!value)
            continue;
        if (value == push) {
            seen_push = true;
            continue;
        }
        if (!seen_push) {
            if (value->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM))
                return false;
            continue;
        }
        if ((value->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)) &&
            value->op != XI_ERR_CHECK)
            return false;
    }
    return seen_push;
}

static const XiBlock *prepare_array_fill_loop_guard(const XiBlock *body) {
    const XiBlock *guard = NULL;
    if (!body || body->kind != XI_BLOCK_PLAIN || !body->succs[0])
        return NULL;
    for (uint16_t i = 0; i < body->npreds; i++) {
        const XiBlock *pred = body->preds[i];
        if (!pred || pred->kind != XI_BLOCK_IF || pred->succs[0] != body || body->succs[0] != pred)
            continue;
        if (guard && guard != pred)
            return NULL;
        guard = pred;
    }
    return guard;
}

static bool prepare_array_loop_entry_checked(const XiBlock *loop, const XiBlock *backedge,
                                             const XiValue *cap_value) {
    if (!loop || !cap_value)
        return false;
    bool saw_entry = false;
    for (uint16_t i = 0; i < loop->npreds; i++) {
        const XiBlock *pred = loop->preds[i];
        if (pred == loop || pred == backedge)
            continue;
        if (!pred || pred->kind != XI_BLOCK_IF || pred->succs[0] != loop)
            return false;
        const XiValue *cond = unwrap_identity_value(pred->control);
        if (!cond || cond->op != XI_LT || cond->nargs < 2)
            return false;
        if (!prepare_array_const_int_value(cond->args[0], 0) ||
            !same_value(cond->args[1], cap_value))
            return false;
        saw_entry = true;
    }
    return saw_entry;
}

static bool prepare_array_single_block_entry_checked(const XiBlock *loop,
                                                     const XiValue *cap_value) {
    return prepare_array_loop_entry_checked(loop, loop, cap_value);
}

static bool prepare_array_block_has_only_fill_loop_pure_values(const XiBlock *blk) {
    if (!blk)
        return false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *value = blk->values[i];
        if (!value)
            continue;
        if ((value->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)) &&
            value->op != XI_ERR_CHECK)
            return false;
    }
    return true;
}

static bool prepare_array_paths_reach_latch_without_effects(const XiBlock *blk,
                                                            const XiBlock *latch, uint8_t depth) {
    if (!blk || !latch || depth > 12)
        return false;
    if (!prepare_array_block_has_only_fill_loop_pure_values(blk))
        return false;
    if (blk == latch)
        return true;
    if (blk->kind == XI_BLOCK_PLAIN)
        return blk->succs[0] && prepare_array_paths_reach_latch_without_effects(
                                    blk->succs[0], latch, (uint8_t) (depth + 1));
    if (blk->kind == XI_BLOCK_IF)
        return blk->succs[0] && blk->succs[1] &&
               prepare_array_paths_reach_latch_without_effects(blk->succs[0], latch,
                                                               (uint8_t) (depth + 1)) &&
               prepare_array_paths_reach_latch_without_effects(blk->succs[1], latch,
                                                               (uint8_t) (depth + 1));
    return false;
}

static bool prepare_array_header_paths_reach_latch(const XiBlock *header, const XiBlock *latch) {
    if (!header || !latch)
        return false;
    if (header->kind == XI_BLOCK_PLAIN)
        return header->succs[0] &&
               prepare_array_paths_reach_latch_without_effects(header->succs[0], latch, 0);
    if (header->kind == XI_BLOCK_IF)
        return header->succs[0] && header->succs[1] &&
               prepare_array_paths_reach_latch_without_effects(header->succs[0], latch, 0) &&
               prepare_array_paths_reach_latch_without_effects(header->succs[1], latch, 0);
    return false;
}

static const XiValue *prepare_array_fill_receiver_origin(const XaotBundle *bundle,
                                                         const XiFunc *func, const XiValue *push,
                                                         const XiValue **out_storage) {
    if (out_storage)
        *out_storage = NULL;
    if (!push || push->nargs < 1)
        return NULL;

    const XiValue *origin = prepare_array_single_origin(push->args[0], 0);
    if (origin && out_storage)
        *out_storage = origin;
    if (origin)
        return origin;

    origin = prepare_array_class_field_fresh_store_origin(bundle, func, push->args[0], push);
    if (origin && out_storage)
        *out_storage = unwrap_identity_value(push->args[0]);
    return origin;
}

static bool prepare_array_value_available_at(const XiValue *value, const XiValue *site) {
    value = unwrap_identity_value(value);
    if (!value || !site)
        return false;
    if (value->op == XI_PARAM || value->op == XI_CONST)
        return true;
    if (value->block != site->block)
        return false;
    for (uint32_t i = 0; i < site->block->nvalues; i++) {
        const XiValue *cur = site->block->values[i];
        if (cur == value)
            return true;
        if (cur == site)
            return false;
    }
    return false;
}

static bool prepare_array_single_block_fill_loop_match(const XaotBundle *bundle, const XiFunc *func,
                                                       const XiValue *push,
                                                       PrepareArrayFillLoop *out) {
    const XiBlock *loop = push ? push->block : NULL;
    if (!loop || loop->kind != XI_BLOCK_IF || loop->succs[0] != loop)
        return false;
    if (!prepare_array_block_allows_unchecked_push(loop, push))
        return false;
    const XiValue *cond = unwrap_identity_value(loop->control);
    if (!cond || cond->op != XI_LT || cond->nargs < 2)
        return false;
    const XiValue *index = prepare_array_phi_from_add_one(cond->args[0]);
    if (!index || !prepare_array_loop_index_is_counted(index, loop, loop))
        return false;
    const XiValue *cap_value = prepare_array_loop_bound_base(cond->args[1], loop, loop);
    if (!cap_value || !prepare_array_single_block_entry_checked(loop, cap_value))
        return false;
    const XiValue *storage_value = NULL;
    const XiValue *origin = prepare_array_fill_receiver_origin(bundle, func, push, &storage_value);
    if (!origin || !prepare_array_value_available_at(cap_value, origin))
        return false;
    if (out)
        *out = (PrepareArrayFillLoop) {origin, storage_value, cap_value,     push,
                                       index,  cond->args[0], loop->succs[1]};
    return true;
}

static bool prepare_array_branchy_fill_loop_match(const XaotBundle *bundle, const XiFunc *func,
                                                  const XiValue *push, PrepareArrayFillLoop *out) {
    const XiBlock *header = push ? push->block : NULL;
    if (!header || !prepare_array_block_allows_unchecked_push(header, push))
        return false;

    for (uint16_t pi = 0; pi < header->npreds; pi++) {
        const XiBlock *latch = header->preds[pi];
        if (!latch || latch == header || latch->kind != XI_BLOCK_IF || latch->succs[0] != header)
            continue;

        const XiValue *cond = unwrap_identity_value(latch->control);
        if (!cond || cond->op != XI_LT || cond->nargs < 2)
            continue;
        const XiValue *index = prepare_array_phi_from_add_one(cond->args[0]);
        if (!index || !prepare_array_loop_index_is_counted(index, header, latch))
            continue;
        const XiValue *cap_value = prepare_array_loop_bound_base(cond->args[1], header, latch);
        if (!cap_value || !prepare_array_loop_entry_checked(header, latch, cap_value))
            continue;
        if (!prepare_array_header_paths_reach_latch(header, latch))
            continue;

        const XiValue *storage_value = NULL;
        const XiValue *origin =
            prepare_array_fill_receiver_origin(bundle, func, push, &storage_value);
        if (!origin || !prepare_array_value_available_at(cap_value, origin))
            continue;
        if (out)
            *out = (PrepareArrayFillLoop) {origin, storage_value, cap_value,      push,
                                           index,  cond->args[0], latch->succs[1]};
        return true;
    }
    return false;
}

static bool prepare_array_unrotated_branchy_fill_loop_match(const XaotBundle *bundle,
                                                            const XiFunc *func, const XiValue *push,
                                                            PrepareArrayFillLoop *out) {
    const XiBlock *body = push ? push->block : NULL;
    if (!body || !prepare_array_block_allows_unchecked_push(body, push))
        return false;

    for (uint16_t pi = 0; pi < body->npreds; pi++) {
        const XiBlock *guard = body->preds[pi];
        if (!guard || guard->kind != XI_BLOCK_IF || guard->succs[0] != body)
            continue;
        const XiValue *cond = unwrap_identity_value(guard->control);
        if (!cond || cond->op != XI_LT || cond->nargs < 2)
            continue;
        const XiValue *index = unwrap_identity_value(cond->args[0]);
        if (!index)
            continue;

        for (uint16_t gi = 0; gi < guard->npreds; gi++) {
            const XiBlock *latch = guard->preds[gi];
            if (!latch || latch == guard || latch == body || latch->succs[0] != guard)
                continue;
            if (!prepare_array_loop_index_is_counted(index, guard, latch))
                continue;
            const XiValue *cap_value = prepare_array_loop_bound_base(cond->args[1], guard, latch);
            if (!cap_value)
                continue;
            if (!prepare_array_header_paths_reach_latch(body, latch))
                continue;

            const XiValue *storage_value = NULL;
            const XiValue *origin =
                prepare_array_fill_receiver_origin(bundle, func, push, &storage_value);
            if (!origin || !prepare_array_value_available_at(cap_value, origin))
                continue;
            if (out)
                *out = (PrepareArrayFillLoop) {origin, storage_value, cap_value,      push,
                                               index,  NULL,          guard->succs[1]};
            return true;
        }
    }
    return false;
}

static bool prepare_array_fill_loop_match(const XaotBundle *bundle, const XiFunc *func,
                                          const XiValue *push, PrepareArrayFillLoop *out) {
    if (!push || push->op != XI_CALL_METHOD || push->nargs != 2 || !push->block)
        return false;
    const char *method = (const char *) push->aux;
    if (!method || strcmp(method, "push") != 0)
        return false;
    if (prepare_array_single_block_fill_loop_match(bundle, func, push, out))
        return true;
    if (prepare_array_branchy_fill_loop_match(bundle, func, push, out))
        return true;
    if (prepare_array_unrotated_branchy_fill_loop_match(bundle, func, push, out))
        return true;

    const XiBlock *body = push->block;
    if (!body || body->kind != XI_BLOCK_PLAIN || !body->succs[0])
        return false;
    const XiBlock *guard = prepare_array_fill_loop_guard(body);
    if (!guard)
        return false;
    if (!prepare_array_block_allows_unchecked_push(body, push))
        return false;

    const XiValue *cond = unwrap_identity_value(guard->control);
    if (!cond || cond->op != XI_LT || cond->nargs < 2)
        return false;
    const XiValue *index = unwrap_identity_value(cond->args[0]);
    if (!prepare_array_loop_index_is_counted(index, guard, body))
        return false;
    const XiValue *cap_value = prepare_array_loop_bound_base(cond->args[1], guard, body);
    if (!cap_value)
        return false;

    const XiValue *storage_value = NULL;
    const XiValue *origin = prepare_array_fill_receiver_origin(bundle, func, push, &storage_value);
    if (!origin || !prepare_array_value_available_at(cap_value, origin))
        return false;
    if (out)
        *out = (PrepareArrayFillLoop) {origin, storage_value, cap_value,      push,
                                       index,  NULL,          guard->succs[1]};
    return true;
}

static bool prepare_array_value_mutates_origin_directly(const XiValue *value,
                                                        const XiValue *origin) {
    if (!value || !origin)
        return false;
    if (value->op == XI_INDEX_SET && value->nargs >= 1 &&
        prepare_array_single_origin(value->args[0], 0) == origin)
        return true;
    if (value->op == XI_CALL_METHOD && value->nargs >= 1 &&
        prepare_array_single_origin(value->args[0], 0) == origin) {
        const char *method = (const char *) value->aux;
        if (method && (strcmp(method, "map") == 0 || strcmp(method, "filter") == 0 ||
                       strcmp(method, "reduce") == 0))
            return false;
        if (value->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM))
            return true;
    }
    return false;
}

static bool prepare_array_origin_has_only_fill_mutation(const XiFunc *func, const XiValue *origin,
                                                        const XiValue *fill_push) {
    if (!func || !origin || !fill_push)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *value = blk->values[vi];
            if (value == fill_push)
                continue;
            if (prepare_array_value_mutates_origin_directly(value, origin))
                return false;
        }
    }
    return true;
}

static bool prepare_array_unique_fill_loop_for_origin(const XaotBundle *bundle, const XiFunc *func,
                                                      const XiValue *origin,
                                                      PrepareArrayFillLoop *out) {
    if (!func || !origin || !origin->block || origin->block->func != func ||
        prepare_func_has_exception_handling(func))
        return false;

    PrepareArrayFillLoop found;
    bool have = false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            PrepareArrayFillLoop cur;
            const XiValue *value = blk->values[vi];
            if (!prepare_array_fill_loop_match(bundle, func, value, &cur) || cur.origin != origin)
                continue;
            if (have)
                return false;
            found = cur;
            have = true;
        }
    }
    if (!have || !prepare_array_origin_has_only_fill_mutation(func, origin, found.push))
        return false;
    if (out)
        *out = found;
    return true;
}

static bool prepare_array_block_has_no_side_effect_between(const XiValue *start,
                                                           const XiValue *end) {
    if (!start || !end || start->block != end->block)
        return false;
    bool after_start = false;
    for (uint32_t i = 0; i < start->block->nvalues; i++) {
        const XiValue *cur = start->block->values[i];
        if (!cur)
            continue;
        if (cur == start) {
            after_start = true;
            continue;
        }
        if (cur == end)
            return after_start;
        if (after_start && (cur->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)))
            return false;
    }
    return false;
}

static bool prepare_array_origin_is_directly_used_only_by_store(const XiFunc *func,
                                                                const XiValue *origin,
                                                                const XiValue *store) {
    if (!func || !origin || !store)
        return false;
    bool saw_store_use = false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == origin)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (phi->value.args[ai] == origin)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur == origin)
                continue;
            for (uint16_t ai = 0; ai < cur->nargs; ai++) {
                if (cur->args[ai] != origin)
                    continue;
                if (cur == store && ai == 1) {
                    saw_store_use = true;
                    continue;
                }
                return false;
            }
        }
    }
    return saw_store_use;
}

static bool prepare_value_has_cell(const XiFunc *func, const XiValue *target) {
    if (!func || !target || !xi_var_id_is_valid(target->var_id))
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *value = blk->values[vi];
            if (!value ||
                !(value->op == XI_CLOSURE_NEW ||
                  (value->op == XI_STACK_ALLOC && value->aux_int == XI_CLOSURE_NEW)) ||
                !value->aux)
                continue;
            const XiFunc *child = (const XiFunc *) value->aux;
            for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
                const XiCapture *cap = &child->captures[ci];
                if (!cap->needs_cell || cap->source != XI_CAPTURE_SRC_REG)
                    continue;
                const XiValue *cap_value =
                    (ci < value->nargs && value->args[ci]) ? value->args[ci] : cap->value;
                if (cap_value && cap_value->var_id == target->var_id)
                    return true;
            }
        }
    }
    return false;
}

static bool prepare_array_is_native_local_alloc(const XaotBundle *bundle, const XiValue *value) {
    const XiValue *target = unwrap_identity_value(value);
    XaotContainerElemPlan elem;

    if (!target || target != value || !array_elem_plan_for_value(bundle, target, &elem))
        return false;
    if (target->op == XI_ARRAY_NEW)
        return true;
    if (target->op != XI_CALL_BUILTIN || !target->aux)
        return false;
    const char *name = (const char *) target->aux;
    if (strcmp(name, "array_new") == 0)
        return true;
    if (strcmp(name, "array_with_capacity") == 0)
        return true;
    if (strcmp(name, "Bytes") != 0)
        return false;
    if (target->nargs == 0)
        return true;
    return target->nargs == 1 && target->args[0] && target->args[0]->type &&
           target->args[0]->type->kind == XR_KIND_INT;
}

static bool prepare_array_native_local_arg_use_is_safe(const XiValue *user, uint16_t arg_index) {
    if (!user)
        return false;
    switch ((XiOp) user->op) {
        case XI_INDEX_GET:
        case XI_INDEX_SET:
            return arg_index == 0;
        case XI_BYTES_LOAD_U16:
        case XI_BYTES_LOAD_U32:
        case XI_BYTES_LOAD_U64:
        case XI_BYTES_LOAD_F32:
        case XI_BYTES_LOAD_F64:
        case XI_BYTES_STORE_U16:
        case XI_BYTES_STORE_U32:
        case XI_BYTES_STORE_U64:
        case XI_BYTES_STORE_F32:
        case XI_BYTES_STORE_F64:
        case XI_BYTES_SPAN_FILL:
        case XI_BYTES_SPAN_REPEAT:
        case XI_SPAN_AS_BYTES:
        case XI_SPAN_FILL:
        case XI_SPAN_REINTERPRET:
        case XI_ARRAY_DATA_PTR:
        case XI_BYTES_COPY_WITHIN:
        case XI_BYTES_REPEAT_FROM:
            return arg_index == 0;
        case XI_SPAN_COPY:
        case XI_SPAN_COMPARE:
        case XI_BYTES_SPAN_COPY:
        case XI_BYTES_SPAN_COMPARE:
        case XI_BYTES_SPAN_COMMON_PREFIX:
            return arg_index == 0 || arg_index == 1;
        case XI_BYTES_COPY_FROM:
            return arg_index == 0 || arg_index == 1;
        case XI_LOAD_FIELD: {
            const char *field = (const char *) user->aux;
            return arg_index == 0 && field &&
                   (strcmp(field, "length") == 0 || strcmp(field, "size") == 0);
        }
        case XI_CALL_METHOD: {
            const char *method = (const char *) user->aux;
            if (!method)
                return false;
            if (arg_index == 0 &&
                (strcmp(method, "push") == 0 || strcmp(method, "reserve") == 0 ||
                 strcmp(method, "appendFrom") == 0 || strcmp(method, "repeatFrom") == 0))
                return true;
            if (arg_index == 1 && strcmp(method, "appendFrom") == 0)
                return true;
            return false;
        }
        case XI_RETAIN:
        case XI_RELEASE:
            return arg_index == 0;
        case XI_BOX:
        case XI_UNBOX:
        case XI_COPY:
        case XI_MOVE:
            return arg_index == 0;
        default:
            return false;
    }
}

static bool prepare_array_value_uses_native_local(const XaotBundle *bundle, const XiFunc *func,
                                                  const XiValue *value) {
    const XiValue *target = unwrap_identity_value(value);
    if (!bundle || !func || !target || target != value ||
        prepare_func_has_exception_handling(func) || prepare_value_has_cell(func, target) ||
        !prepare_array_is_native_local_alloc(bundle, target))
        return false;

    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t argi = 0; argi < phi->value.nargs; argi++) {
                if (phi->value.args[argi] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur == target)
                continue;
            for (uint16_t argi = 0; argi < cur->nargs; argi++) {
                if (cur->args[argi] == target &&
                    !prepare_array_native_local_arg_use_is_safe(cur, argi))
                    return false;
            }
        }
    }
    return true;
}

static bool prepare_array_value_known_nonnegative(const XiValue *value, const XiValue *root,
                                                  uint8_t depth);

static bool prepare_array_phi_arg_nonnegative(const XiValue *phi, const XiValue *arg,
                                              bool *has_base, uint8_t depth) {
    const XiValue *value = unwrap_identity_value(arg);
    if (!value)
        return false;
    if (value == phi)
        return true;
    if (value->op == XI_CONST && value->type && value->type->kind == XR_KIND_INT &&
        value->aux_int >= 0) {
        *has_base = true;
        return true;
    }
    if (value->op == XI_ADD && value->nargs >= 2) {
        const XiValue *lhs = unwrap_identity_value(value->args[0]);
        const XiValue *rhs = unwrap_identity_value(value->args[1]);
        if (lhs == phi && prepare_array_value_known_nonnegative(rhs, phi, (uint8_t) (depth + 1)))
            return true;
        if (rhs == phi && prepare_array_value_known_nonnegative(lhs, phi, (uint8_t) (depth + 1)))
            return true;
    }
    if (prepare_array_value_known_nonnegative(value, phi, (uint8_t) (depth + 1))) {
        *has_base = true;
        return true;
    }
    return false;
}

static bool prepare_array_value_known_nonnegative(const XiValue *value, const XiValue *root,
                                                  uint8_t depth) {
    value = unwrap_identity_value(value);
    if (!value || depth > 8)
        return false;
    if (value == root && depth > 0)
        return false;
    if (value->type && value->type->kind == XR_KIND_INT &&
        xi_range_known_nonneg(xi_range_of(value)))
        return true;
    if (value->op == XI_CONST && value->type && value->type->kind == XR_KIND_INT)
        return value->aux_int >= 0;
    switch ((XiOp) value->op) {
        case XI_NARROW_U8:
        case XI_NARROW_U16:
        case XI_NARROW_U32:
        case XI_WIDEN_U8:
        case XI_WIDEN_U16:
        case XI_WIDEN_U32:
            return true;
        case XI_ADD:
            return value->nargs >= 2 &&
                   prepare_array_value_known_nonnegative(value->args[0], root,
                                                         (uint8_t) (depth + 1)) &&
                   prepare_array_value_known_nonnegative(value->args[1], root,
                                                         (uint8_t) (depth + 1));
        case XI_PHI: {
            bool has_base = false;
            if (value->nargs == 0)
                return false;
            for (uint16_t i = 0; i < value->nargs; i++) {
                if (!prepare_array_phi_arg_nonnegative(value, value->args[i], &has_base,
                                                       (uint8_t) (depth + 1)))
                    return false;
            }
            return has_base;
        }
        default:
            return false;
    }
}

/* Storage identity: same SSA value, or both are receiver refs of the same
 * class-native array field (so .length reads and index ops hit the same
 * backing store even through distinct loads). */
static bool prepare_array_values_share_storage(const XaotBundle *bundle, const XiFunc *func,
                                               const XiValue *lhs, const XiValue *rhs) {
    if (same_value(lhs, rhs))
        return true;

    uint16_t lhs_idx = 0;
    uint16_t rhs_idx = 0;
    return xaot_class_native_receiver_ref_field(bundle, func, unwrap_identity_value(lhs),
                                                XR_NATIVE_ARRAY_REF, NULL, &lhs_idx) &&
           xaot_class_native_receiver_ref_field(bundle, func, unwrap_identity_value(rhs),
                                                XR_NATIVE_ARRAY_REF, NULL, &rhs_idx) &&
           lhs_idx == rhs_idx;
}

static bool prepare_array_length_value_matches(const XaotBundle *bundle, const XiFunc *func,
                                               const XiValue *length_value,
                                               const XiValue *array_value) {
    const XiValue *value = unwrap_identity_value(length_value);
    if (!value || value->op != XI_LOAD_FIELD || value->nargs < 1)
        return false;
    const char *field = (const char *) value->aux;
    if (!field || (strcmp(field, "length") != 0 && strcmp(field, "size") != 0))
        return false;
    return prepare_array_values_share_storage(bundle, func, value->args[0], array_value);
}

static bool prepare_array_control_proves_index_lt_len(const XaotBundle *bundle, const XiFunc *func,
                                                      const XiValue *control,
                                                      const XiValue *array_value,
                                                      const XiValue *index_value,
                                                      const XiValue **out_len,
                                                      uint8_t *fail_reason) {
    const XiValue *value = unwrap_identity_value(control);
    if (!value || value->op != XI_LT || value->nargs < 2)
        return false; /* caller's default reason (no guard) stands */
    if (!same_value(value->args[0], index_value))
        return false;
    if (!prepare_array_length_value_matches(bundle, func, value->args[1], array_value)) {
        /* The shape is `idx < bound` but bound is not this array's length. */
        if (fail_reason && *fail_reason < XAOT_BOUNDS_UNPROVEN_LEN_MISMATCH)
            *fail_reason = XAOT_BOUNDS_UNPROVEN_LEN_MISMATCH;
        return false;
    }
    if (out_len)
        *out_len = unwrap_identity_value(value->args[1]);
    return true;
}

static bool prepare_array_block_has_no_side_effect_after(const XiBlock *blk, const XiValue *start) {
    bool seen = start == NULL;
    bool found = false;
    if (!blk)
        return false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *value = blk->values[i];
        if (!value)
            continue;
        if (value == start) {
            seen = true;
            found = true;
            continue;
        }
        if (seen && (value->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)))
            return false;
    }
    if (!found && start != NULL) {
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *value = blk->values[i];
            if (value && (value->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)))
                return false;
        }
        return true;
    }
    return seen;
}

static bool prepare_array_block_has_no_side_effect_before(const XiBlock *blk,
                                                          const XiValue *target) {
    if (!blk)
        return false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *value = blk->values[i];
        if (!value)
            continue;
        if (value == target)
            return true;
        if (value->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM))
            return false;
    }
    return false;
}

/* Reasons grow monotonically: a later, more specific failure overwrites a
 * generic one but never the other way around (see XAOT_BOUNDS_UNPROVEN_*
 * ordering). */
static void bounds_fail(uint8_t *fail_reason, uint8_t reason) {
    if (fail_reason && *fail_reason < reason)
        *fail_reason = reason;
}

static bool prepare_array_index_access_bounds_proven(const XaotBundle *bundle, const XiFunc *func,
                                                     const XiValue *value, uint8_t *fail_reason) {
    if (!value || (value->op != XI_INDEX_GET && value->op != XI_INDEX_SET) || value->nargs < 2 ||
        !value->block)
        return false;
    const XiValue *array_value = value->args[0];
    const XiValue *index_value = value->args[1];
    if (!prepare_array_value_known_nonnegative(index_value, NULL, 0)) {
        bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_INDEX_RANGE);
        return false;
    }
    if (!prepare_array_block_has_no_side_effect_before(value->block, value)) {
        bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_CLOBBER);
        return false;
    }
    if (value->block->npreds == 0) {
        bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_NO_GUARD);
        return false;
    }
    for (uint16_t i = 0; i < value->block->npreds; i++) {
        const XiBlock *pred = value->block->preds[i];
        const XiValue *len_value = NULL;
        if (!pred || pred->kind != XI_BLOCK_IF || pred->succs[0] != value->block) {
            bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_NO_GUARD);
            return false;
        }
        if (!prepare_array_control_proves_index_lt_len(bundle, func, pred->control, array_value,
                                                       index_value, &len_value, fail_reason)) {
            bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_NO_GUARD);
            return false;
        }
        if (!prepare_array_block_has_no_side_effect_after(pred, len_value)) {
            bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_CLOBBER);
            return false;
        }
    }
    return true;
}

static bool prepare_array_index_set_counted_loop_bounds_proven(const XaotBundle *bundle,
                                                               const XiFunc *func,
                                                               const XiValue *value,
                                                               uint8_t *fail_reason) {
    if (!value || value->op != XI_INDEX_SET || value->nargs < 2 || !value->block)
        return false;
    const XiBlock *loop = value->block;
    if (loop->kind != XI_BLOCK_IF || loop->succs[0] != loop) {
        bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_NO_GUARD);
        return false;
    }
    if (!prepare_array_block_has_no_side_effect_before(loop, value)) {
        bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_CLOBBER);
        return false;
    }
    const XiValue *cond = unwrap_identity_value(loop->control);
    if (!cond || cond->op != XI_LT || cond->nargs < 2) {
        bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_NO_GUARD);
        return false;
    }
    const XiValue *index = prepare_array_phi_from_add_one(cond->args[0]);
    if (!index || !same_value(index, value->args[1])) {
        bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_NO_GUARD);
        return false;
    }
    const XiValue *bound = prepare_array_loop_bound_base(cond->args[1], loop, loop);
    if (!bound) {
        bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_NO_GUARD);
        return false;
    }
    if (!prepare_array_length_value_matches(bundle, func, bound, value->args[0])) {
        bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_LEN_MISMATCH);
        return false;
    }
    if (!prepare_array_loop_index_is_counted(index, loop, loop)) {
        bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_NO_GUARD);
        return false;
    }
    if (!prepare_array_single_block_entry_checked(loop, bound)) {
        bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_NO_GUARD);
        return false;
    }
    return true;
}

/* Unified in-bounds proof for an XI_INDEX_GET / XI_INDEX_SET.  Returns the
 * evidence bits when proven (0 when not); when unproven and out_reason is
 * given, reports the most specific failure across both proof paths.
 * Shared by the bounds-plan pass and the verifier so the proof can never
 * diverge from the plan. */
XR_FUNC uint32_t xaot_prepare_array_access_bounds_evidence(const XaotBundle *bundle,
                                                           const XiFunc *func,
                                                           const XiValue *access,
                                                           uint8_t *out_reason) {
    uint8_t reason = XAOT_BOUNDS_UNPROVEN_NONE;
    if (prepare_array_index_access_bounds_proven(bundle, func, access, &reason)) {
        if (out_reason)
            *out_reason = XAOT_BOUNDS_UNPROVEN_NONE;
        return XAOT_BOUNDS_EV_DOM_GUARD | XAOT_BOUNDS_EV_NONNEG_INDEX | XAOT_BOUNDS_EV_NO_CLOBBER;
    }
    if (prepare_array_index_set_counted_loop_bounds_proven(bundle, func, access, &reason)) {
        if (out_reason)
            *out_reason = XAOT_BOUNDS_UNPROVEN_NONE;
        return XAOT_BOUNDS_EV_COUNTED_LOOP | XAOT_BOUNDS_EV_NONNEG_INDEX |
               XAOT_BOUNDS_EV_NO_CLOBBER;
    }
    if (out_reason)
        *out_reason = reason != XAOT_BOUNDS_UNPROVEN_NONE ? reason : XAOT_BOUNDS_UNPROVEN_NO_GUARD;
    return 0;
}

static bool prepare_span_access_kind_for_value(const XiValue *value, uint8_t *out_kind) {
    if (!value || !out_kind)
        return false;
    switch ((XiOp) value->op) {
        case XI_INDEX_GET:
            *out_kind = XAOT_SPAN_ACCESS_INDEX_GET;
            return true;
        case XI_INDEX_SET:
            *out_kind = XAOT_SPAN_ACCESS_INDEX_SET;
            return true;
        case XI_BYTES_LOAD_U16:
        case XI_BYTES_LOAD_U32:
        case XI_BYTES_LOAD_U64:
        case XI_BYTES_LOAD_F32:
        case XI_BYTES_LOAD_F64:
            *out_kind = XAOT_SPAN_ACCESS_BYTE_LOAD;
            return true;
        case XI_BYTES_STORE_U16:
        case XI_BYTES_STORE_U32:
        case XI_BYTES_STORE_U64:
        case XI_BYTES_STORE_F32:
        case XI_BYTES_STORE_F64:
            *out_kind = XAOT_SPAN_ACCESS_BYTE_STORE;
            return true;
        case XI_BYTES_SPAN_FILL:
            *out_kind = XAOT_SPAN_ACCESS_BYTE_FILL;
            return true;
        case XI_BYTES_SPAN_COPY:
            *out_kind = XAOT_SPAN_ACCESS_BYTE_COPY;
            return true;
        case XI_BYTES_SPAN_COMPARE:
            *out_kind = XAOT_SPAN_ACCESS_BYTE_COMPARE;
            return true;
        case XI_BYTES_SPAN_COMMON_PREFIX:
            *out_kind = XAOT_SPAN_ACCESS_BYTE_COMMON_PREFIX;
            return true;
        case XI_BYTES_SPAN_REPEAT:
            *out_kind = XAOT_SPAN_ACCESS_BYTE_REPEAT;
            return true;
        case XI_SPAN_AS_BYTES:
            *out_kind = XAOT_SPAN_ACCESS_SPAN_AS_BYTES;
            return true;
        case XI_SPAN_FILL:
            *out_kind = XAOT_SPAN_ACCESS_SPAN_FILL;
            return true;
        case XI_SPAN_COPY:
            *out_kind = XAOT_SPAN_ACCESS_SPAN_COPY;
            return true;
        case XI_SPAN_COMPARE:
            *out_kind = XAOT_SPAN_ACCESS_SPAN_COMPARE;
            return true;
        case XI_SPAN_REINTERPRET:
            *out_kind = XAOT_SPAN_ACCESS_REINTERPRET;
            return true;
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT: {
            const char *method = value->aux ? (const char *) value->aux : NULL;
            if (!method || value->nargs != 2 || !value->args[0] || !value->args[0]->type ||
                value->args[0]->type->kind != XR_KIND_SPAN)
                return false;
            if (strcmp(method, "copyFrom") == 0) {
                *out_kind = XAOT_SPAN_ACCESS_BYTE_COPY;
                return true;
            }
            if (strcmp(method, "commonPrefix") == 0) {
                *out_kind = XAOT_SPAN_ACCESS_BYTE_COMMON_PREFIX;
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

static bool prepare_value_plan_is_span_aggregate(const XaotBundle *bundle, const XiValue *value) {
    const XaotValuePlan *plan;
    const XiValue *origin = unwrap_identity_value(value);
    if (!bundle || !origin)
        return false;
    plan = xaot_bundle_find_value_plan(bundle, origin);
    return plan && plan->rep.kind == XAOT_VALUE_AGGREGATE &&
           (plan->rep.flags & XAOT_VALUE_FLAG_SPAN) != 0;
}

static bool prepare_span_elem_plan_for_value(const XaotBundle *bundle, const XiValue *value,
                                             XaotContainerElemPlan *out) {
    XaotContainerPlan plan;
    const XiValue *origin = unwrap_identity_value(value);
    if (!origin || !origin->type || origin->type->kind != XR_KIND_SPAN ||
        !prepare_value_plan_is_span_aggregate(bundle, origin))
        return false;
    if (!xaot_container_plan_for_type(origin->type, &plan) || plan.kind != XAOT_CONTAINER_ARRAY)
        return false;
    if (out)
        *out = plan.elem;
    return true;
}

static bool prepare_span_elem_is_byte(const XaotContainerElemPlan *elem) {
    return elem && elem->elem_name && strcmp(elem->elem_name, "XR_ELEM_U8") == 0;
}

static bool prepare_span_elem_is_pod(const XaotContainerElemPlan *elem) {
    return elem && elem->elem_name && strcmp(elem->elem_name, "XR_ELEM_ANY") != 0 &&
           elem->rep != XAOT_REP_TAGGED;
}

static bool prepare_span_elem_size(const XaotContainerElemPlan *elem, uint8_t *out_size) {
    const XaotRepInfo *info;
    if (!elem || !out_size)
        return false;
    info = xaot_rep_info(elem->rep);
    if (!info || info->size == 0)
        return false;
    *out_size = info->size;
    return true;
}

static bool prepare_span_source_owner_is_writable(const XiValue *value) {
    const XiValue *origin = unwrap_identity_value(value);
    if (!origin || !origin->type)
        return false;
    return (origin->type->kind == XR_KIND_ARRAY || origin->type->kind == XR_KIND_FIXED_ARRAY) &&
           !xr_type_is_const(origin->type);
}

static bool prepare_span_value_writable_proven(const XaotBundle *bundle, const XiValue *value,
                                               uint8_t depth) {
    const XiValue *origin = unwrap_identity_value(value);
    if (!bundle || !origin || depth > 8 || !origin->type || origin->type->kind != XR_KIND_SPAN ||
        !prepare_value_plan_is_span_aggregate(bundle, origin))
        return false;

    switch ((XiOp) origin->op) {
        case XI_SLICE:
            if (origin->nargs < 1)
                return false;
            if (origin->args[0] && origin->args[0]->type &&
                origin->args[0]->type->kind == XR_KIND_SPAN)
                return prepare_span_value_writable_proven(bundle, origin->args[0], depth + 1);
            return prepare_span_source_owner_is_writable(origin->args[0]);
        case XI_SPAN_AS_BYTES:
        case XI_SPAN_REINTERPRET:
            return origin->nargs >= 1 &&
                   prepare_span_value_writable_proven(bundle, origin->args[0], depth + 1);
        case XI_CALL_METHOD:
            return origin->aux && strcmp((const char *) origin->aux, "asSpan") == 0 &&
                   origin->nargs >= 1 && origin->args[0] && origin->args[0]->type &&
                   xr_type_is_named_class(origin->args[0]->type, "Buffer") &&
                   !xr_type_is_const(origin->args[0]->type);
        case XI_PHI: {
            bool saw_arg = false;
            for (uint16_t i = 0; i < origin->nargs; i++) {
                const XiValue *arg = unwrap_identity_value(origin->args[i]);
                if (!arg || arg == origin)
                    continue;
                if (!prepare_span_value_writable_proven(bundle, arg, depth + 1))
                    return false;
                saw_arg = true;
            }
            return saw_arg;
        }
        default:
            return false;
    }
}

static bool prepare_span_elem_matches(const XaotContainerElemPlan *a,
                                      const XaotContainerElemPlan *b) {
    return a && b && a->elem_name && b->elem_name && a->c_type && b->c_type &&
           strcmp(a->elem_name, b->elem_name) == 0 && strcmp(a->c_type, b->c_type) == 0 &&
           a->rep == b->rep && a->storage_rep == b->storage_rep;
}

static bool prepare_endian_member_index(const char *name, int64_t *out_index) {
    if (!name || !out_index)
        return false;
    if (strcmp(name, "Native") == 0) {
        *out_index = XR_ENDIAN_NATIVE;
        return true;
    }
    if (strcmp(name, "LE") == 0) {
        *out_index = XR_ENDIAN_LE;
        return true;
    }
    if (strcmp(name, "BE") == 0) {
        *out_index = XR_ENDIAN_BE;
        return true;
    }
    return false;
}

static bool prepare_value_is_endian_member(const XiValue *value, int64_t *out_index) {
    const XiValue *origin = unwrap_identity_value(value);
    const XiValue *receiver;
    if (!origin || origin->op != XI_LOAD_FIELD || origin->nargs < 1 || !origin->aux)
        return false;
    receiver = unwrap_identity_value(origin->args[0]);
    if (!receiver || receiver->op != XI_GET_BUILTIN || receiver->aux_int != XR_GLOBAL_VAR_ENDIAN)
        return false;
    return prepare_endian_member_index((const char *) origin->aux, out_index);
}

static bool prepare_value_is_const_endian(const XiValue *value, int64_t *out_index) {
    const XiValue *origin;
    if (!out_index)
        return false;
    if (!value) {
        *out_index = XR_ENDIAN_NATIVE;
        return true;
    }
    if (prepare_value_is_endian_member(value, out_index))
        return true;
    origin = unwrap_identity_value(value);
    if (origin && origin->op == XI_CONST && origin->type && origin->type->kind == XR_KIND_INT &&
        origin->aux_int >= XR_ENDIAN_NATIVE && origin->aux_int <= XR_ENDIAN_BE) {
        *out_index = origin->aux_int;
        return true;
    }
    return false;
}

static bool prepare_span_access_is_index(uint8_t kind) {
    return kind == XAOT_SPAN_ACCESS_INDEX_GET || kind == XAOT_SPAN_ACCESS_INDEX_SET;
}

static bool prepare_span_access_is_byte(uint8_t kind) {
    return kind == XAOT_SPAN_ACCESS_BYTE_LOAD || kind == XAOT_SPAN_ACCESS_BYTE_STORE ||
           kind == XAOT_SPAN_ACCESS_BYTE_FILL || kind == XAOT_SPAN_ACCESS_BYTE_COPY ||
           kind == XAOT_SPAN_ACCESS_BYTE_COMPARE || kind == XAOT_SPAN_ACCESS_BYTE_COMMON_PREFIX ||
           kind == XAOT_SPAN_ACCESS_BYTE_REPEAT || kind == XAOT_SPAN_ACCESS_REINTERPRET;
}

static bool prepare_span_access_is_write(uint8_t kind) {
    return kind == XAOT_SPAN_ACCESS_INDEX_SET || kind == XAOT_SPAN_ACCESS_BYTE_STORE ||
           kind == XAOT_SPAN_ACCESS_BYTE_FILL || kind == XAOT_SPAN_ACCESS_BYTE_COPY ||
           kind == XAOT_SPAN_ACCESS_BYTE_REPEAT || kind == XAOT_SPAN_ACCESS_SPAN_FILL ||
           kind == XAOT_SPAN_ACCESS_SPAN_COPY;
}

static uint8_t prepare_span_reason_from_bounds_reason(uint8_t bounds_reason) {
    switch (bounds_reason) {
        case XAOT_BOUNDS_UNPROVEN_LEN_MISMATCH:
            return XAOT_SPAN_UNPROVEN_LENGTH_REL;
        case XAOT_BOUNDS_UNPROVEN_CLOBBER:
            return XAOT_SPAN_UNPROVEN_CLOBBER;
        case XAOT_BOUNDS_UNPROVEN_INDEX_RANGE:
        case XAOT_BOUNDS_UNPROVEN_NO_GUARD:
        default:
            return XAOT_SPAN_UNPROVEN_RANGE;
    }
}

static bool prepare_span_reinterpret_length_relation_proven(const XaotBundle *bundle,
                                                            const XiValue *value,
                                                            uint8_t target_elem_size) {
    XaotContainerElemPlan source_elem;
    uint8_t source_elem_size = 0;
    const XiValue *receiver;

    if (!bundle || !value || target_elem_size == 0 || value->nargs < 1)
        return false;
    receiver = unwrap_identity_value(value->args[0]);
    if (!receiver || receiver->op != XI_SPAN_AS_BYTES || receiver->nargs < 1)
        return false;
    if (!prepare_span_elem_plan_for_value(bundle, receiver->args[0], &source_elem) ||
        !prepare_span_elem_is_pod(&source_elem) ||
        !prepare_span_elem_size(&source_elem, &source_elem_size))
        return false;
    return (source_elem_size % target_elem_size) == 0;
}

XR_FUNC bool xaot_prepare_span_access_plan_for_value(const XaotBundle *bundle, const XiFunc *func,
                                                     const XiValue *value,
                                                     XaotSpanAccessPlan *out) {
    XaotContainerElemPlan recv_elem;
    XaotContainerElemPlan other_elem;
    uint8_t kind = 0;
    uint32_t evidence = 0;
    uint32_t drop = 0;
    uint8_t reason = XAOT_SPAN_UNPROVEN_NONE;
    int64_t endian = XR_ENDIAN_NATIVE;

    if (!out || !prepare_span_access_kind_for_value(value, &kind))
        return false;
    memset(out, 0, sizeof(*out));
    out->func = func;
    out->value = value;
    out->kind = kind;

    if (!value || value->nargs < 1)
        return false;
    if (!value->args[0] || !value->args[0]->type || value->args[0]->type->kind != XR_KIND_SPAN) {
        if (prepare_span_access_is_index(kind))
            return false;
        reason = XAOT_SPAN_UNPROVEN_DYNAMIC_RECV;
        goto done;
    }
    if (!prepare_span_elem_plan_for_value(bundle, value->args[0], &recv_elem)) {
        reason = XAOT_SPAN_UNPROVEN_DYNAMIC_RECV;
        goto done;
    }
    evidence |= XAOT_SPAN_EV_RECV_AGGREGATE;
    if (prepare_span_elem_is_byte(&recv_elem))
        evidence |= XAOT_SPAN_EV_RECV_BYTE_SPAN;
    if (prepare_span_elem_is_pod(&recv_elem))
        evidence |= XAOT_SPAN_EV_RECV_POD;
    if (prepare_span_access_is_write(kind) &&
        prepare_span_value_writable_proven(bundle, value->args[0], 0)) {
        evidence |= XAOT_SPAN_EV_WRITABLE;
        drop |= XAOT_SPAN_DROP_READONLY;
    }

    if (prepare_span_access_is_index(kind)) {
        uint8_t bounds_reason = XAOT_BOUNDS_UNPROVEN_NONE;
        uint32_t bounds_evidence =
            xaot_prepare_array_access_bounds_evidence(bundle, func, value, &bounds_reason);
        if (bounds_evidence != 0) {
            evidence |= XAOT_SPAN_EV_RANGE_PROVEN | XAOT_SPAN_EV_NO_CLOBBER;
            drop |= XAOT_SPAN_DROP_BOUNDS;
        } else {
            reason = prepare_span_reason_from_bounds_reason(bounds_reason);
        }
        goto done;
    }

    if (prepare_span_access_is_byte(kind) && (evidence & XAOT_SPAN_EV_RECV_BYTE_SPAN) == 0) {
        reason = XAOT_SPAN_UNPROVEN_NOT_BYTE_SPAN;
        goto done;
    }

    switch ((XaotSpanAccessKind) kind) {
        case XAOT_SPAN_ACCESS_BYTE_LOAD:
            if (prepare_value_is_const_endian(value->nargs >= 3 ? value->args[2] : NULL, &endian)) {
                (void) endian;
                evidence |= XAOT_SPAN_EV_ENDIAN_CONST;
            }
            drop |= XAOT_SPAN_DROP_TYPE | XAOT_SPAN_DROP_HELPER;
            break;
        case XAOT_SPAN_ACCESS_BYTE_STORE:
            if (prepare_value_is_const_endian(value->nargs >= 4 ? value->args[3] : NULL, &endian)) {
                (void) endian;
                evidence |= XAOT_SPAN_EV_ENDIAN_CONST;
            }
            drop |= XAOT_SPAN_DROP_TYPE | XAOT_SPAN_DROP_HELPER;
            break;
        case XAOT_SPAN_ACCESS_BYTE_FILL:
        case XAOT_SPAN_ACCESS_BYTE_REPEAT:
            drop |= XAOT_SPAN_DROP_TYPE | XAOT_SPAN_DROP_HELPER;
            break;
        case XAOT_SPAN_ACCESS_BYTE_COPY:
        case XAOT_SPAN_ACCESS_BYTE_COMPARE:
        case XAOT_SPAN_ACCESS_BYTE_COMMON_PREFIX:
            if (value->nargs < 2 ||
                !prepare_span_elem_plan_for_value(bundle, value->args[1], &other_elem) ||
                !prepare_span_elem_is_byte(&other_elem)) {
                reason = XAOT_SPAN_UNPROVEN_DYNAMIC_BOUNDARY;
                break;
            }
            evidence |= XAOT_SPAN_EV_ELEM_MATCH;
            drop |= XAOT_SPAN_DROP_TYPE | XAOT_SPAN_DROP_HELPER;
            break;
        case XAOT_SPAN_ACCESS_SPAN_AS_BYTES:
        case XAOT_SPAN_ACCESS_SPAN_FILL:
            if ((evidence & XAOT_SPAN_EV_RECV_POD) == 0) {
                reason = XAOT_SPAN_UNPROVEN_NOT_POD;
                break;
            }
            if (kind == XAOT_SPAN_ACCESS_SPAN_AS_BYTES) {
                uint8_t recv_elem_size = 0;
                evidence |= XAOT_SPAN_EV_LENGTH_REL_PROVEN;
                if (prepare_span_elem_size(&recv_elem, &recv_elem_size) && recv_elem_size == 1) {
                    evidence |= XAOT_SPAN_EV_BYTE_LEN_NO_OVERFLOW;
                    drop |= XAOT_SPAN_DROP_OVERFLOW;
                }
            }
            drop |= XAOT_SPAN_DROP_TYPE | XAOT_SPAN_DROP_POD | XAOT_SPAN_DROP_HELPER;
            break;
        case XAOT_SPAN_ACCESS_SPAN_COPY:
        case XAOT_SPAN_ACCESS_SPAN_COMPARE:
            if ((evidence & XAOT_SPAN_EV_RECV_POD) == 0) {
                reason = XAOT_SPAN_UNPROVEN_NOT_POD;
                break;
            }
            if (value->nargs < 2 ||
                !prepare_span_elem_plan_for_value(bundle, value->args[1], &other_elem) ||
                !prepare_span_elem_is_pod(&other_elem)) {
                reason = XAOT_SPAN_UNPROVEN_DYNAMIC_BOUNDARY;
                break;
            }
            if (!prepare_span_elem_matches(&recv_elem, &other_elem)) {
                reason = XAOT_SPAN_UNPROVEN_ELEM_MISMATCH;
                break;
            }
            evidence |= XAOT_SPAN_EV_ELEM_MATCH;
            drop |= XAOT_SPAN_DROP_TYPE | XAOT_SPAN_DROP_POD | XAOT_SPAN_DROP_HELPER;
            break;
        case XAOT_SPAN_ACCESS_REINTERPRET: {
            uint8_t target_elem_size = (uint8_t) ((value->aux_int >> 8) & 0xff);
            if (target_elem_size == 1 ||
                prepare_span_reinterpret_length_relation_proven(bundle, value, target_elem_size))
                evidence |= XAOT_SPAN_EV_LENGTH_REL_PROVEN;
            drop |= XAOT_SPAN_DROP_TYPE | XAOT_SPAN_DROP_POD | XAOT_SPAN_DROP_HELPER;
            break;
        }
        default:
            reason = XAOT_SPAN_UNPROVEN_DYNAMIC_BOUNDARY;
            break;
    }

done:
    if (drop != 0)
        reason = XAOT_SPAN_UNPROVEN_NONE;
    else if (reason == XAOT_SPAN_UNPROVEN_NONE)
        reason = XAOT_SPAN_UNPROVEN_DYNAMIC_BOUNDARY;
    out->evidence = evidence;
    out->eliminated_checks = drop;
    out->unproven_reason = reason;
    return true;
}

/* Prove every index access in the function and record the result — proven
 * ones with evidence, unproven ones with a reason — in the bounds plan.
 * Emission consults only the plan (no pattern matching in Cgen), so the
 * proof, the verifier and the dump stay in lockstep, and the unproven rows
 * expose the remaining bounds-check budget for audit. */
static bool prepare_func_bounds_plans(XaotBundle *bundle, const XiFunc *func,
                                      const XgBodySummary *body) {
    if (!body)
        return true;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *value = blk->values[vi];
            if (!value || (value->op != XI_INDEX_GET && value->op != XI_INDEX_SET))
                continue;
            uint8_t reason = XAOT_BOUNDS_UNPROVEN_NONE;
            uint32_t evidence =
                xaot_prepare_array_access_bounds_evidence(bundle, func, value, &reason);
            if (!xaot_bundle_add_bounds_plan(bundle, func, value, body, evidence, reason)) {
                bundle->error_msg = "failed to allocate AOT bounds plan";
                return false;
            }
        }
    }
    return true;
}

static bool prepare_func_span_access_plans(XaotBundle *bundle, const XiFunc *func,
                                           const XgBodySummary *body) {
    if (!body)
        return true;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *value = blk->values[vi];
            XaotSpanAccessPlan plan;
            if (!xaot_prepare_span_access_plan_for_value(bundle, func, value, &plan))
                continue;
            if (!xaot_bundle_add_span_access_plan(bundle, func, value, body, plan.kind,
                                                  plan.evidence, plan.eliminated_checks,
                                                  plan.unproven_reason)) {
                bundle->error_msg = "failed to allocate AOT Span access plan";
                return false;
            }
        }
    }
    return true;
}

/* ========== Alias plans (restrict evidence) ========== */

/* Whitelist scan for XAOT_ALIAS_UNIQUE_DATA: every use of the array value
 * must keep element-storage access inside the _adN cache.  Index ops must
 * be bounds-proven (checked slow paths read ->data directly, which would
 * break restrict), the only permitted method call is the proven fill push
 * (emitted as a raw cache store), and anything that could create a second
 * pointer — calls, stores, captures, phi participation — is rejected.
 * Returning the array is fine: the restrict scope ends with the function. */
static bool prepare_alias_array_uses_are_cache_local(const XaotBundle *bundle, const XiFunc *func,
                                                     const XiValue *target,
                                                     const XiValue *fill_push) {
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (same_value(phi->value.args[a], target))
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur == target)
                continue;
            for (uint16_t argi = 0; argi < cur->nargs; argi++) {
                if (!same_value(cur->args[argi], target))
                    continue;
                switch ((XiOp) cur->op) {
                    case XI_INDEX_GET:
                    case XI_INDEX_SET: {
                        const XaotBoundsPlan *bp;
                        if (argi != 0)
                            return false;
                        bp = xaot_bundle_find_bounds_plan(bundle, cur);
                        if (!bp || bp->evidence == 0)
                            return false;
                        break;
                    }
                    case XI_CALL_METHOD:
                        if (cur != fill_push || argi != 0)
                            return false;
                        break;
                    case XI_LOAD_FIELD: {
                        const char *field = (const char *) cur->aux;
                        if (argi != 0 || !field ||
                            (strcmp(field, "length") != 0 && strcmp(field, "size") != 0))
                            return false;
                        break;
                    }
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (argi != 0)
                            return false;
                        break;
                    case XI_BOX:
                    case XI_UNBOX:
                    case XI_COPY:
                    case XI_MOVE:
                        if (argi != 0)
                            return false;
                        break;
                    default:
                        return false;
                }
            }
        }
    }
    return true;
}

/* UNIQUE_DATA proof for an array cache plan: the cached data pointer is the
 * only element-storage pointer to its backing while the function runs.
 * Returns the evidence bits, or 0 when uniqueness cannot be proven.  Shared
 * by the alias-plan pass and the verifier. */
XR_FUNC uint32_t xaot_prepare_array_cache_alias_evidence(const XaotBundle *bundle,
                                                         const XiFunc *func,
                                                         const XaotArrayCachePlan *cache_plan) {
    const XaotArrayStoragePlan *storage;
    const XiValue *target;
    const XiValue *origin;
    const XiValue *fill_push = NULL;
    uint32_t evidence;

    if (!bundle || !func || !cache_plan || cache_plan->func != func)
        return 0;
    /* Views alias foreign storage; class-field caches can be reached through
     * the receiver on other paths.  Both are out of scope for restrict. */
    if (cache_plan->flags & (XAOT_ARRAY_CACHE_VIEW | XAOT_ARRAY_CACHE_CLASS_FIELD))
        return 0;
    if ((cache_plan->flags & (XAOT_ARRAY_CACHE_FILL_LOOP | XAOT_ARRAY_CACHE_NATIVE_LOCAL)) == 0)
        return 0;

    target = unwrap_identity_value(cache_plan->value);
    if (!target)
        return 0;
    storage = xaot_bundle_find_array_storage_plan(bundle, cache_plan->value);
    origin = storage && storage->origin ? unwrap_identity_value(storage->origin) : target;

    /* Backing must be a fresh allocation made by this function. */
    if (!origin || !origin->block || origin->block->func != func)
        return 0;
    if (origin->op != XI_ARRAY_NEW && !array_builtin_is_fresh_storage(origin))
        return 0;
    evidence = XAOT_ALIAS_EV_FRESH_ALLOC;

    if (cache_plan->flags & XAOT_ARRAY_CACHE_FILL_LOOP) {
        PrepareArrayFillLoop fill;
        if (!prepare_array_unique_fill_loop_for_origin(bundle, func, target, &fill))
            return 0;
        fill_push = fill.push;
    }
    if (!prepare_alias_array_uses_are_cache_local(bundle, func, target, fill_push))
        return 0;
    evidence |= XAOT_ALIAS_EV_ALL_ACCESS_RAW | XAOT_ALIAS_EV_USE_WHITELIST;

    /* No second cache over the same backing in this function. */
    for (uint32_t i = 0; i < bundle->narray_cache_plans; i++) {
        const XaotArrayCachePlan *other = &bundle->array_cache_plans[i];
        const XaotArrayStoragePlan *other_storage;
        const XiValue *other_origin;
        if (other == cache_plan || other->func != func)
            continue;
        other_storage = xaot_bundle_find_array_storage_plan(bundle, other->value);
        other_origin = other_storage && other_storage->origin
                           ? unwrap_identity_value(other_storage->origin)
                           : unwrap_identity_value(other->value);
        if (other_origin == origin)
            return 0;
    }
    evidence |= XAOT_ALIAS_EV_SOLE_CACHE;
    return evidence;
}

/* Record restrict-grade uniqueness for every qualifying array cache.
 * XRAY_AOT_NO_RESTRICT=1 suppresses the whole pass (stress mode for
 * regression bisection: a miscompiled restrict is UB, so a global off
 * switch is the fastest way to confirm or rule out alias plans). */
static bool prepare_func_alias_plans(XaotBundle *bundle, const XiFunc *func,
                                     const XgBodySummary *body) {
    const char *off = getenv("XRAY_AOT_NO_RESTRICT");
    if (!body)
        return true;
    if (off && off[0] != '\0' && off[0] != '0')
        return true;
    for (uint32_t i = 0; i < bundle->narray_cache_plans; i++) {
        const XaotArrayCachePlan *cache_plan = &bundle->array_cache_plans[i];
        uint32_t evidence;
        if (cache_plan->func != func)
            continue;
        evidence = xaot_prepare_array_cache_alias_evidence(bundle, func, cache_plan);
        if (evidence == 0)
            continue;
        if (!xaot_bundle_add_alias_plan(bundle, func, cache_plan->value, body,
                                        XAOT_ALIAS_UNIQUE_DATA, evidence)) {
            bundle->error_msg = "failed to allocate AOT alias plan";
            return false;
        }
    }
    return true;
}

XR_FUNC bool xaot_prepare_closure_plan_for_value(const XiFunc *func, const XiValue *value,
                                                 XaotClosurePlan *out) {
    bool stack_closure = false;
    const XiFunc *target;

    if (!func || !value || !out)
        return false;
    if (!value->block || value->block->func != func)
        return false;
    if (value->op == XI_CLOSURE_NEW) {
        stack_closure = false;
    } else if (value->op == XI_STACK_ALLOC && value->aux_int == XI_CLOSURE_NEW) {
        stack_closure = true;
    } else {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->func = func;
    out->value = value;
    out->representation = stack_closure ? XAOT_CLOSURE_STACK : XAOT_CLOSURE_RUNTIME;
    out->evidence = XAOT_CLOSURE_EV_XI_VALUE;
    if (stack_closure)
        out->evidence |= XAOT_CLOSURE_EV_NOESCAPE_STACK;

    target = value->aux ? (const XiFunc *) value->aux : NULL;
    if (!target) {
        out->capture_count = value->nargs;
        out->unproven_reason = XAOT_CLOSURE_UNPROVEN_NO_TARGET;
        return true;
    }

    out->target_func = target;
    out->capture_count = target->ncaptures;
    out->evidence |= XAOT_CLOSURE_EV_TARGET_FUNC;
    if (value->nargs != target->ncaptures) {
        out->unproven_reason = XAOT_CLOSURE_UNPROVEN_CAPTURE_ARITY;
        return true;
    }

    out->evidence |= XAOT_CLOSURE_EV_CAPTURE_ARITY;
    out->unproven_reason = XAOT_CLOSURE_UNPROVEN_NONE;
    return true;
}

static bool prepare_func_closure_plans(XaotBundle *bundle, const XiFunc *func) {
    if (!bundle || !func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XaotClosurePlan derived;
            const XiValue *value = blk->values[vi];
            if (!xaot_prepare_closure_plan_for_value(func, value, &derived))
                continue;
            if (!xaot_bundle_add_closure_plan(bundle, func, value, derived.target_func,
                                              derived.capture_count, derived.representation,
                                              derived.evidence, derived.unproven_reason)) {
                bundle->error_msg = "failed to allocate AOT closure plan";
                return false;
            }
        }
    }
    return true;
}

static uint8_t prepare_transfer_channel_site_kind(const XiValue *site) {
    const char *method;

    if (!site)
        return 0;
    if (site->op == XI_CHAN_SEND)
        return XAOT_TRANSFER_CHAN_SEND;
    if (site->op == XI_CHAN_TRY_SEND)
        return XAOT_TRANSFER_CHAN_TRY_SEND;
    if (site->op != XI_CALL_METHOD || site->nargs < 2 || !xi_value_type_is_channel(site->args[0]))
        return 0;
    method = (const char *) site->aux;
    if (!method)
        return 0;
    if (strcmp(method, "send") == 0)
        return XAOT_TRANSFER_CHAN_SEND;
    if (strcmp(method, "trySend") == 0)
        return XAOT_TRANSFER_CHAN_TRY_SEND;
    if (strcmp(method, "sendTimeout") == 0)
        return XAOT_TRANSFER_CHAN_SEND_TIMEOUT;
    return 0;
}

static uint8_t prepare_transfer_action(uint8_t mode, bool needs_boundary_clone) {
    switch ((XrTransferMode) mode) {
        case XR_TRANSFER_SHARE:
            return XAOT_TRANSFER_ACTION_SHARE;
        case XR_TRANSFER_COPY:
            return needs_boundary_clone ? XAOT_TRANSFER_ACTION_DEEP_COPY
                                        : XAOT_TRANSFER_ACTION_COPY;
        case XR_TRANSFER_MOVE:
            return XAOT_TRANSFER_ACTION_MOVE;
        default:
            return XAOT_TRANSFER_ACTION_REJECT;
    }
}

static XaotTypeKey prepare_transfer_type_key(const XrType *type) {
    XaotContainerPlan container_plan;
    XaotTypeKey key;
    memset(&key, 0, sizeof(key));
    if (type && xaot_container_plan_for_type(type, &container_plan))
        key = container_plan.type_key;
    return key;
}

XR_FUNC bool xaot_prepare_transfer_plan_for_site(const XiFunc *func, const XiValue *site,
                                                 uint16_t transfer_index, XaotTransferPlan *out) {
    const XiValue *value = NULL;
    const XrType *value_type = NULL;
    uint8_t site_kind = 0;
    uint8_t mode = XR_TRANSFER_SHARE;
    uint8_t action;
    uint32_t evidence = XAOT_TRANSFER_EV_SITE;
    bool needs_boundary_clone = false;

    if (!func || !site || !out)
        return false;
    if (!site->block || site->block->func != func)
        return false;

    if (site->op == XI_GO || site->op == XI_THREAD_SPAWN) {
        if ((uint32_t) transfer_index + 1u >= site->nargs)
            return false;
        site_kind = site->op == XI_GO ? XAOT_TRANSFER_GO_ARG : XAOT_TRANSFER_THREAD_ARG;
        value = site->args[transfer_index + 1u];
        mode = xi_go_arg_transfer_mode(site, transfer_index);
    } else {
        site_kind = prepare_transfer_channel_site_kind(site);
        if (site_kind == 0 || transfer_index != 0)
            return false;
        value = site->nargs >= 2 ? site->args[1] : NULL;
        mode = xi_chan_send_transfer_mode(site);
    }

    memset(out, 0, sizeof(*out));
    out->func = func;
    out->site = site;
    out->value = value;
    out->transfer_index = transfer_index;
    out->site_kind = site_kind;
    out->mode = mode;

    if (!value) {
        out->action = XAOT_TRANSFER_ACTION_REJECT;
        out->evidence = evidence;
        out->unproven_reason = XAOT_TRANSFER_UNPROVEN_NO_VALUE;
        return true;
    }
    evidence |= XAOT_TRANSFER_EV_VALUE;

    if (mode > XR_TRANSFER_MOVE) {
        out->action = XAOT_TRANSFER_ACTION_REJECT;
        out->evidence = evidence;
        out->unproven_reason = XAOT_TRANSFER_UNPROVEN_BAD_MODE;
        return true;
    }
    evidence |= XAOT_TRANSFER_EV_MODE;

    value_type = value->type;
    if (value_type) {
        out->value_type = value_type;
        out->value_type_key = prepare_transfer_type_key(value_type);
        evidence |= XAOT_TRANSFER_EV_TYPE;
    }

    needs_boundary_clone = xi_coro_value_needs_boundary_clone(value);
    if (needs_boundary_clone)
        evidence |= XAOT_TRANSFER_EV_BOUNDARY_CLONE;
    action = prepare_transfer_action(mode, needs_boundary_clone);
    out->action = action;
    out->evidence = evidence;
    out->unproven_reason = XAOT_TRANSFER_UNPROVEN_NONE;
    return true;
}

static bool prepare_func_transfer_plans(XaotBundle *bundle, const XiFunc *func) {
    if (!bundle || !func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *site = blk->values[vi];
            if (!site)
                continue;
            if (site->op == XI_GO || site->op == XI_THREAD_SPAWN) {
                for (uint16_t ai = 1; ai < site->nargs; ai++) {
                    XaotTransferPlan derived;
                    uint16_t transfer_index = (uint16_t) (ai - 1);
                    if (!xaot_prepare_transfer_plan_for_site(func, site, transfer_index, &derived))
                        continue;
                    if (!xaot_bundle_add_transfer_plan(
                            bundle, func, site, transfer_index, derived.value, derived.value_type,
                            &derived.value_type_key, derived.site_kind, derived.mode,
                            derived.action, derived.evidence, derived.unproven_reason)) {
                        bundle->error_msg = "failed to allocate AOT transfer plan";
                        return false;
                    }
                }
            } else if (prepare_transfer_channel_site_kind(site) != 0) {
                XaotTransferPlan derived;
                if (!xaot_prepare_transfer_plan_for_site(func, site, 0, &derived))
                    continue;
                if (!xaot_bundle_add_transfer_plan(bundle, func, site, 0, derived.value,
                                                   derived.value_type, &derived.value_type_key,
                                                   derived.site_kind, derived.mode, derived.action,
                                                   derived.evidence, derived.unproven_reason)) {
                    bundle->error_msg = "failed to allocate AOT transfer plan";
                    return false;
                }
            }
        }
    }
    return true;
}

static bool prepare_array_native_local_data_cacheable(const XaotBundle *bundle, const XiFunc *func,
                                                      const XiValue *value) {
    const XiValue *target = unwrap_identity_value(value);
    if (!target || !func || !prepare_array_value_uses_native_local(bundle, func, target))
        return false;

    bool has_index_use = false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur == target)
                continue;
            for (uint16_t argi = 0; argi < cur->nargs; argi++) {
                if (!same_value(cur->args[argi], target))
                    continue;
                switch ((XiOp) cur->op) {
                    case XI_INDEX_GET:
                        if (argi != 0)
                            return false;
                        has_index_use = true;
                        break;
                    case XI_INDEX_SET:
                        if (argi != 0 ||
                            xaot_prepare_array_access_bounds_evidence(bundle, func, cur, NULL) == 0)
                            return false;
                        has_index_use = true;
                        break;
                    case XI_LOAD_FIELD: {
                        const char *field = (const char *) cur->aux;
                        if (argi != 0 || !field ||
                            (strcmp(field, "length") != 0 && strcmp(field, "size") != 0))
                            return false;
                        break;
                    }
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (argi != 0)
                            return false;
                        break;
                    case XI_BOX:
                    case XI_UNBOX:
                    case XI_COPY:
                    case XI_MOVE:
                        if (argi != 0)
                            return false;
                        break;
                    default:
                        return false;
                }
            }
        }
    }

    return has_index_use;
}

static bool prepare_array_value_has_index_get_use(const XiFunc *func, const XiValue *target) {
    target = unwrap_identity_value(target);
    if (!func || !target)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur->op != XI_INDEX_GET || cur->nargs < 1)
                continue;
            if (same_value(cur->args[0], target))
                return true;
        }
    }
    return false;
}

static bool prepare_array_class_field_read_cacheable(const XaotBundle *bundle, const XiFunc *func,
                                                     const XiValue *value) {
    const XiValue *target = unwrap_identity_value(value);
    if (!bundle || !func || !target ||
        !xaot_class_native_receiver_ref_field(bundle, func, target, XR_NATIVE_ARRAY_REF, NULL,
                                              NULL) ||
        !prepare_array_value_has_index_get_use(func, target))
        return false;

    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            if (!cur || cur == target)
                continue;
            if (cur->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM))
                return false;
        }
    }

    return true;
}

static bool prepare_array_cache_value(XaotBundle *bundle, const XiFunc *func,
                                      const XiValue *value) {
    const XiValue *target = unwrap_identity_value(value);
    const XaotArrayStoragePlan *storage;
    uint32_t flags = XAOT_ARRAY_CACHE_DECLARE_LOCAL;
    uint32_t before;
    XaotArrayCachePlan *plan;

    if (!bundle || !func || !target)
        return false;
    if (xaot_bundle_find_array_cache_plan(bundle, target))
        return true;
    storage = xaot_bundle_find_array_storage_plan(bundle, target);
    if (!storage || (storage->flags & XAOT_ARRAY_STORAGE_READ) == 0)
        return true;

    if (array_value_is_cacheable_view(target)) {
        flags |= XAOT_ARRAY_CACHE_READ | XAOT_ARRAY_CACHE_VIEW;
    } else if (array_method_is_hof_result(target) && !array_value_has_uncacheable_use(target)) {
        flags |= XAOT_ARRAY_CACHE_READ | XAOT_ARRAY_CACHE_FRESH_RESULT;
        if ((storage->flags & XAOT_ARRAY_STORAGE_MUTABLE) != 0)
            flags |= XAOT_ARRAY_CACHE_MUTABLE;
    } else if ((storage->flags & XAOT_ARRAY_STORAGE_MUTABLE) != 0 &&
               prepare_array_unique_fill_loop_for_origin(bundle, func, target, NULL)) {
        flags |= XAOT_ARRAY_CACHE_READ | XAOT_ARRAY_CACHE_MUTABLE | XAOT_ARRAY_CACHE_FILL_LOOP;
    } else if ((storage->flags & XAOT_ARRAY_STORAGE_MUTABLE) != 0 &&
               prepare_array_native_local_data_cacheable(bundle, func, target)) {
        flags |= XAOT_ARRAY_CACHE_READ | XAOT_ARRAY_CACHE_MUTABLE | XAOT_ARRAY_CACHE_NATIVE_LOCAL;
    } else if (prepare_array_class_field_read_cacheable(bundle, func, target)) {
        flags |= XAOT_ARRAY_CACHE_READ | XAOT_ARRAY_CACHE_CLASS_FIELD;
    } else {
        return true;
    }

    before = bundle->narray_cache_plans;
    plan = xaot_bundle_add_array_cache_plan(bundle, func, target, target, flags, &storage->elem);
    if (!plan) {
        bundle->error_msg = "failed to allocate AOT array cache plan";
        return false;
    }
    if (bundle->narray_cache_plans != before)
        record_array_cache_stats(&bundle->stats, plan);
    return true;
}

static bool prepare_func_array_cache_plans(XaotBundle *bundle, XiFunc *func) {
    uint32_t bi;

    if (!bundle || !func)
        return false;
    for (bi = 0; bi < func->nblocks; bi++) {
        XiBlock *blk = func->blocks[bi];
        XiPhi *phi;
        uint32_t vi;
        if (!blk)
            continue;
        for (phi = blk->phis; phi; phi = phi->next) {
            if (!prepare_array_cache_value(bundle, func, &phi->value))
                return false;
        }
        for (vi = 0; vi < blk->nvalues; vi++) {
            if (!prepare_array_cache_value(bundle, func, blk->values[vi]))
                return false;
        }
    }
    return true;
}

static bool prepare_array_class_field_alloc_value(XaotBundle *bundle, XiFunc *func,
                                                  const XiValue *value) {
    const XiValue *origin = unwrap_identity_value(value);
    const XaotArrayStoragePlan *storage;
    const XaotArrayCachePlan *cache;
    PrepareArrayFillLoop fill;
    const XiValue *store = NULL;
    XaotClassNativeFunc class_info;
    uint16_t field_idx = 0;

    if (!bundle || !func || !origin ||
        xaot_bundle_find_array_class_field_alloc_plan(bundle, origin))
        return true;
    storage = xaot_bundle_find_array_storage_plan(bundle, origin);
    cache = xaot_bundle_find_array_cache_plan(bundle, origin);
    if (!storage || (storage->flags & XAOT_ARRAY_STORAGE_MUTABLE) == 0 || !cache ||
        (cache->flags & XAOT_ARRAY_CACHE_MUTABLE) == 0 ||
        (cache->flags & XAOT_ARRAY_CACHE_FILL_LOOP) == 0)
        return true;
    if (!prepare_array_unique_fill_loop_for_origin(bundle, func, origin, &fill) ||
        !fill.storage_value)
        return true;

    memset(&class_info, 0, sizeof(class_info));
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *cur = blk->values[vi];
            const XiValue *cur_origin;
            XaotClassNativeFunc cur_info;
            uint16_t cur_idx = 0;
            if (!cur || cur->op != XI_STORE_FIELD || cur->nargs < 2)
                continue;
            cur_origin = prepare_array_single_origin(cur->args[1], 0);
            if (cur_origin != origin)
                continue;
            if (!prepare_array_native_receiver_array_store_info(bundle, func, cur, UINT16_MAX,
                                                                &cur_info, &cur_idx))
                return true;
            if (store)
                return true;
            store = cur;
            class_info = cur_info;
            field_idx = cur_idx;
        }
    }

    if (!store || store->block != origin->block ||
        !prepare_array_block_has_no_side_effect_between(origin, store) ||
        !prepare_array_origin_is_directly_used_only_by_store(func, origin, store))
        return true;

    if (!xaot_bundle_add_array_class_field_alloc_plan(
            bundle, func, origin, store, class_info.class_data, field_idx, &storage->elem)) {
        bundle->error_msg = "failed to allocate AOT array class-field alloc plan";
        return false;
    }
    return true;
}

static bool prepare_func_array_class_field_alloc_plans(XaotBundle *bundle, XiFunc *func) {
    if (!bundle || !func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            if (!prepare_array_class_field_alloc_value(bundle, func, blk->values[vi]))
                return false;
        }
    }
    return true;
}

static bool prepare_func_type_plans(XaotBundle *bundle, XiFunc *func) {
    uint16_t i;

    if (!bundle || !func)
        return false;
    if (!prepare_type_plans_for_type(bundle, func->return_type, 0))
        return false;
    for (i = 0; i < func->nparams; i++) {
        const XiValue *param = func->params ? func->params[i] : NULL;
        if (param && !prepare_type_plans_for_type(bundle, param->type, 0))
            return false;
    }
    return true;
}

static bool prepare_apply_param_abi_value_plan(XaotBundle *bundle, const XiFunc *func,
                                               XaotValuePlan *vp) {
    const XaotFuncPlan *func_plan;
    const XiValue *value;
    uint16_t param_idx;

    if (!bundle || !func || !vp)
        return false;
    value = vp->value;
    if (!value || value->op != XI_PARAM || value->aux_int < 0)
        return true;
    param_idx = (uint16_t) value->aux_int;
    func_plan = xaot_bundle_find_func_plan(bundle, func);
    if (!func_plan || func_plan->abi.kind != XAOT_ABI_NATIVE || param_idx >= func_plan->abi.nparams)
        return true;
    vp->rep = xaot_abi_slot_value_rep(&func_plan->abi.params[param_idx]);
    return true;
}

static bool prepare_func_values(XaotBundle *bundle, XiFunc *func) {
    uint32_t bi;
    if (!bundle || !func)
        return false;
    for (bi = 0; bi < func->nblocks; bi++) {
        XiBlock *blk = func->blocks[bi];
        XiPhi *phi;
        uint32_t vi;
        if (!blk)
            continue;
        for (phi = blk->phis; phi; phi = phi->next) {
            XaotValuePlan *vp = xaot_bundle_add_value_plan(bundle, func, &phi->value);
            if (!vp) {
                bundle->error_msg = "failed to allocate AOT value plan";
                return false;
            }
            apply_native_class_ptr_value_plan(bundle, vp);
            apply_freestanding_enum_ordinal_value_plan(bundle, vp);
            record_value_stats(&bundle->stats, vp->rep.kind);
            if (!prepare_type_plans_for_type(bundle, phi->value.type, 0))
                return false;
        }
        for (vi = 0; vi < blk->nvalues; vi++) {
            XaotValuePlan *vp = xaot_bundle_add_value_plan(bundle, func, blk->values[vi]);
            if (!vp) {
                bundle->error_msg = "failed to allocate AOT value plan";
                return false;
            }
            if (!prepare_apply_param_abi_value_plan(bundle, func, vp))
                return false;
            apply_native_class_ptr_value_plan(bundle, vp);
            apply_freestanding_enum_ordinal_value_plan(bundle, vp);
            record_value_stats(&bundle->stats, vp->rep.kind);
            if (!prepare_type_plans_for_type(bundle, blk->values[vi]->type, 0))
                return false;
        }
    }
    return true;
}

static bool prepare_apply_return_abi_value_plans(XaotBundle *bundle,
                                                 const XaotFuncPlan *func_plan) {
    XaotValueRep ret_rep;

    if (!bundle || !func_plan || !func_plan->func)
        return false;
    ret_rep = xaot_abi_slot_value_rep(&func_plan->abi.ret);
    if (ret_rep.kind != XAOT_VALUE_AGGREGATE)
        return true;
    for (uint32_t bi = 0; bi < func_plan->func->nblocks; bi++) {
        XiBlock *blk = func_plan->func->blocks[bi];
        if (!blk || blk->kind != XI_BLOCK_RETURN || !blk->control)
            continue;
        XaotValuePlan *vp = xaot_bundle_find_value_plan_mut(bundle, blk->control);
        if (!vp) {
            bundle->error_msg = "AOT aggregate return control has no value plan";
            return false;
        }
        vp->rep = ret_rep;
    }
    return true;
}

static bool value_rep_is_struct_aggregate(XaotValueRep rep) {
    return rep.kind == XAOT_VALUE_AGGREGATE && (rep.flags & XAOT_VALUE_FLAG_STRUCT) != 0;
}

static bool value_rep_is_propagating_aggregate(XaotValueRep rep) {
    return rep.kind == XAOT_VALUE_AGGREGATE &&
           (rep.flags & (XAOT_VALUE_FLAG_STRUCT | XAOT_VALUE_FLAG_ENUM)) != 0;
}

enum {
    XAOT_PREPARE_ENUM_AGG_PAYLOAD_CAP = 16
};

static bool prepare_adt_enum_plan_can_use_compact_value(const XaotEnumPlan *plan) {
    const XiEnumData *ed = plan ? plan->enum_data : NULL;
    if (!plan || !ed || !ed->is_adt || plan->max_payload > XAOT_PREPARE_ENUM_AGG_PAYLOAD_CAP)
        return false;
    for (uint32_t i = 0; i < plan->member_count; i++) {
        const XiEnumMemberData *member = plan->members ? &plan->members[i] : NULL;
        if (member && member->payload_count > XAOT_PREPARE_ENUM_AGG_PAYLOAD_CAP)
            return false;
    }
    return true;
}

static bool prepare_compact_adt_value_rep_for_type(const XaotBundle *bundle, const XrType *type,
                                                   XaotValueRep *out_rep) {
    const XaotEnumPlan *plan;
    if (!bundle || !type || !out_rep)
        return false;
    plan = xaot_bundle_find_enum_plan_for_type(bundle, type);
    if (!prepare_adt_enum_plan_can_use_compact_value(plan))
        return false;
    memset(out_rep, 0, sizeof(*out_rep));
    out_rep->kind = XAOT_VALUE_AGGREGATE;
    out_rep->rep = XAOT_REP_TAGGED;
    out_rep->type = type;
    out_rep->c_type = plan && plan->c_type ? plan->c_type : "XrAotEnumAggregate";
    out_rep->flags = XAOT_VALUE_FLAG_ENUM | XAOT_VALUE_FLAG_ENUM_AGGREGATE;
    return true;
}

static bool prepare_parallel_reduce_aggregate_rep(XaotBundle *bundle, const XiValue *value,
                                                  XaotValueRep *out_rep) {
    if (!bundle || !value || value->op != XI_PAR_REDUCE ||
        value->aux_kind != XI_AUX_KIND_PAR_REDUCE || !value->aux || !out_rep)
        return false;
    const XiParallelReduceData *data = (const XiParallelReduceData *) value->aux;
    const XaotFuncPlan *body_plan =
        data->body_func ? xaot_bundle_find_func_plan(bundle, data->body_func) : NULL;
    const XaotFuncPlan *combine_plan =
        data->combine_func ? xaot_bundle_find_func_plan(bundle, data->combine_func) : NULL;
    if (!body_plan || !combine_plan || combine_plan->abi.nparams < 2 || !combine_plan->abi.params)
        return false;

    XaotValueRep body_ret = xaot_abi_slot_value_rep(&body_plan->abi.ret);
    XaotValueRep combine_ret = xaot_abi_slot_value_rep(&combine_plan->abi.ret);
    XaotValueRep combine_arg0 = xaot_abi_slot_value_rep(&combine_plan->abi.params[0]);
    XaotValueRep combine_arg1 = xaot_abi_slot_value_rep(&combine_plan->abi.params[1]);
    if (!value_rep_is_struct_aggregate(body_ret) || !value_reps_equal(body_ret, combine_ret) ||
        !value_reps_equal(body_ret, combine_arg0) || !value_reps_equal(body_ret, combine_arg1))
        return false;
    *out_rep = body_ret;
    return true;
}

static bool prepare_identity_aggregate_rep(XaotBundle *bundle, const XiValue *value,
                                           XaotValueRep *out_rep) {
    if (!bundle || !value || !out_rep || value->nargs < 1)
        return false;
    switch (value->op) {
        case XI_COPY:
        case XI_MOVE:
        case XI_CHECKTYPE:
            break;
        default:
            return false;
    }
    const XaotValuePlan *arg_plan = xaot_bundle_find_value_plan(bundle, value->args[0]);
    if (!arg_plan || !value_rep_is_propagating_aggregate(arg_plan->rep))
        return false;
    *out_rep = arg_plan->rep;
    return true;
}

static void prepare_mark_aggregate_value_rep(XaotBundle *bundle, XiValue *value, XaotValueRep rep,
                                             bool *changed, int depth);

static void prepare_mark_direct_call_aggregate_args(XaotBundle *bundle, XiValue *call,
                                                    XaotValueRep rep, bool *changed, int depth) {
    if (!bundle || !call || !changed || depth > 8)
        return;
    if (call->op != XI_CALL && call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT)
        return;

    const XiFunc *current = call->block ? call->block->func : NULL;
    uint16_t first_arg = 0;
    const XiFunc *target =
        xaot_boundary_resolve_direct_call_target(bundle, current, call, &first_arg);
    const XaotFuncPlan *target_plan = target ? xaot_bundle_find_func_plan(bundle, target) : NULL;
    if (!target_plan || !target_plan->abi.params)
        return;
    uint16_t fixed_params = target->is_vararg ? target->nparams : target_plan->abi.nparams;
    for (uint16_t a = first_arg; a < call->nargs; a++) {
        uint16_t param_idx = (uint16_t) (a - first_arg);
        if (param_idx >= fixed_params)
            break;
        XaotValueRep slot_rep = xaot_abi_slot_value_rep(&target_plan->abi.params[param_idx]);
        if (!value_rep_is_struct_aggregate(slot_rep) || !value_reps_equal(slot_rep, rep))
            continue;
        if (call->args[a])
            prepare_mark_aggregate_value_rep(bundle, call->args[a], rep, changed, depth + 1);
    }
}

static void prepare_mark_aggregate_value_rep(XaotBundle *bundle, XiValue *value, XaotValueRep rep,
                                             bool *changed, int depth) {
    if (!bundle || !value || !changed || depth > 8)
        return;
    XaotValuePlan *vp = xaot_bundle_find_value_plan_mut(bundle, value);
    if (vp && !value_reps_equal(vp->rep, rep)) {
        vp->rep = rep;
        *changed = true;
    }
    switch (value->op) {
        case XI_COPY:
        case XI_MOVE:
        case XI_CHECKTYPE:
            if (value->nargs >= 1)
                prepare_mark_aggregate_value_rep(bundle, value->args[0], rep, changed, depth + 1);
            break;
        case XI_PHI:
            for (uint16_t i = 0; i < value->nargs; i++) {
                if (value->args[i] && value->args[i] != value)
                    prepare_mark_aggregate_value_rep(bundle, value->args[i], rep, changed,
                                                     depth + 1);
            }
            break;
        case XI_PAR_REDUCE:
            if (value->nargs >= 4)
                prepare_mark_aggregate_value_rep(bundle, value->args[3], rep, changed, depth + 1);
            break;
        case XI_CALL:
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
            prepare_mark_direct_call_aggregate_args(bundle, value, rep, changed, depth + 1);
            break;
        default:
            break;
    }
}

static bool prepare_apply_error_channel_aggregate_rep(XaotBundle *bundle, XiValue *value,
                                                      bool *changed) {
    XaotValuePlan *vp;
    XaotValueRep rep;

    if (!bundle || !value || !changed)
        return false;

    if (value->op == XI_ERR_CATCH) {
        vp = xaot_bundle_find_value_plan_mut(bundle, value);
        if (!vp || !prepare_compact_adt_value_rep_for_type(bundle, value->type, &rep))
            return false;
        if (!value_reps_equal(vp->rep, rep)) {
            vp->rep = rep;
            *changed = true;
        }
        prepare_mark_aggregate_value_rep(bundle, value, rep, changed, 0);
        return true;
    }

    if ((value->op == XI_ERR_SET || value->op == XI_ERR_RETURN) && value->nargs >= 1 &&
        value->args[0] &&
        prepare_compact_adt_value_rep_for_type(bundle, value->args[0]->type, &rep)) {
        prepare_mark_aggregate_value_rep(bundle, value->args[0], rep, changed, 0);
        return true;
    }

    return false;
}

static bool prepare_apply_aggregate_value_plans_once(XaotBundle *bundle, XiFunc *func,
                                                     bool *changed) {
    if (!bundle || !func || !changed)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *blk = func->blocks ? func->blocks[bi] : NULL;
        if (!blk)
            continue;
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            XiValue *value = &phi->value;
            XaotValuePlan *vp = xaot_bundle_find_value_plan_mut(bundle, value);
            if (vp && value_rep_is_propagating_aggregate(vp->rep))
                prepare_mark_aggregate_value_rep(bundle, value, vp->rep, changed, 0);
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *value = blk->values ? blk->values[vi] : NULL;
            XaotValuePlan *vp = xaot_bundle_find_value_plan_mut(bundle, value);
            XaotValueRep rep;
            memset(&rep, 0, sizeof(rep));
            if (!value || !vp)
                continue;
            if (value_rep_is_propagating_aggregate(vp->rep)) {
                prepare_mark_aggregate_value_rep(bundle, value, vp->rep, changed, 0);
                continue;
            }
            if (prepare_apply_error_channel_aggregate_rep(bundle, value, changed))
                continue;
            if (!prepare_parallel_reduce_aggregate_rep(bundle, value, &rep) &&
                !prepare_identity_aggregate_rep(bundle, value, &rep))
                continue;
            if (!value_reps_equal(vp->rep, rep)) {
                vp->rep = rep;
                *changed = true;
            }
            prepare_mark_aggregate_value_rep(bundle, value, rep, changed, 0);
        }
    }
    return true;
}

static bool prepare_apply_aggregate_value_plans(XaotBundle *bundle, XiFunc *func) {
    bool changed = false;
    for (int pass = 0; pass < 8; pass++) {
        changed = false;
        if (!prepare_apply_aggregate_value_plans_once(bundle, func, &changed))
            return false;
        if (!changed)
            return true;
    }
    return true;
}

static bool prepare_direct_call_arg_boundary(XaotBundle *bundle, const XaotFuncPlan *caller_plan,
                                             const XiValue *call, const XiFunc *target,
                                             const XaotFuncPlan *target_plan, uint16_t arg_index,
                                             const XiValue *arg) {
    const XaotValuePlan *arg_plan;
    const XaotAbiSlot *slot;
    XaotValueRep slot_rep;
    XaotBoundaryStep *step;

    if (!bundle || !caller_plan || !call || !target || !target_plan || !arg)
        return false;
    if (arg_index >= target_plan->abi.nparams || !target_plan->abi.params) {
        bundle->error_msg = "AOT direct call argument count exceeds target ABI";
        return false;
    }

    arg_plan = xaot_bundle_find_value_plan(bundle, arg);
    if (!arg_plan) {
        bundle->error_msg = "AOT direct call argument has no value plan";
        return false;
    }
    slot = &target_plan->abi.params[arg_index];
    slot_rep = xaot_abi_slot_value_rep(slot);
    if (value_reps_equal(arg_plan->rep, slot_rep))
        return true;

    step = xaot_bundle_add_boundary_step(bundle, XAOT_BOUNDARY_STEP_DIRECT_CALL_ARG,
                                         caller_plan->func, call, arg, XAOT_BOUNDARY_DIRECT_CALL);
    if (!step) {
        bundle->error_msg = "failed to allocate AOT direct call argument boundary";
        return false;
    }
    step->target_func = target;
    step->arg_index = arg_index;
    step->from_rep = arg_plan->rep;
    step->to_rep = slot_rep;
    bundle->stats.boundary_count++;
    return true;
}

static bool prepare_direct_call_ret_boundary(XaotBundle *bundle, const XaotFuncPlan *caller_plan,
                                             const XiValue *call, const XiFunc *target,
                                             const XaotFuncPlan *target_plan) {
    XaotValuePlan *call_plan;
    XaotValueRep ret_rep;
    XaotBoundaryStep *step;

    if (!bundle || !caller_plan || !call || !target || !target_plan)
        return false;
    if (target_plan->abi.ret.cls == XAOT_ARG_VOID)
        return true;

    call_plan = xaot_bundle_find_value_plan_mut(bundle, call);
    if (!call_plan) {
        bundle->error_msg = "AOT direct call result has no value plan";
        return false;
    }
    ret_rep = xaot_abi_slot_value_rep(&target_plan->abi.ret);
    if (ret_rep.kind == XAOT_VALUE_AGGREGATE) {
        call_plan->rep = ret_rep;
        return true;
    }
    if (value_reps_equal(ret_rep, call_plan->rep))
        return true;

    step = xaot_bundle_add_boundary_step(bundle, XAOT_BOUNDARY_STEP_DIRECT_CALL_RET,
                                         caller_plan->func, call, NULL, XAOT_BOUNDARY_DIRECT_CALL);
    if (!step) {
        bundle->error_msg = "failed to allocate AOT direct call return boundary";
        return false;
    }
    step->target_func = target;
    step->from_rep = ret_rep;
    step->to_rep = call_plan->rep;
    bundle->stats.boundary_count++;
    return true;
}

static bool prepare_direct_call_boundaries(XaotBundle *bundle, const XaotFuncPlan *caller_plan,
                                           const XiValue *call) {
    const XiFunc *target;
    const XaotFuncPlan *target_plan;
    uint16_t first_arg;
    uint16_t call_arg_count;
    uint16_t a;

    if (!bundle || !caller_plan || !caller_plan->func || !call)
        return true;
    if (call->op != XI_CALL && call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT)
        return true;
    target = xaot_boundary_resolve_direct_call_target(bundle, caller_plan->func, call, &first_arg);
    if (!target)
        return true;
    target_plan = xaot_bundle_find_func_plan(bundle, target);
    if (!target_plan) {
        bundle->error_msg = "AOT direct call target has no function plan";
        return false;
    }
    call_arg_count = call->nargs > first_arg ? (uint16_t) (call->nargs - first_arg) : 0;
    if (target->is_vararg) {
        /* Fixed args map to their ABI slots; the trailing args are collected
         * into the rest Array at the call site (boxed to tagged), so they need
         * no per-arg boundary and may exceed the fixed-parameter count. */
        uint16_t fixed = target->nparams;
        if (call_arg_count < fixed) {
            bundle->error_msg = "AOT direct call has fewer arguments than target ABI";
            return false;
        }
        for (a = first_arg; a < (uint16_t) (first_arg + fixed); a++) {
            if (!prepare_direct_call_arg_boundary(bundle, caller_plan, call, target, target_plan,
                                                  (uint16_t) (a - first_arg), call->args[a]))
                return false;
        }
    } else {
        if (call_arg_count > target_plan->abi.nparams) {
            bundle->error_msg = "AOT direct call has more arguments than target ABI";
            return false;
        }
        for (a = first_arg; a < call->nargs; a++) {
            if (!prepare_direct_call_arg_boundary(bundle, caller_plan, call, target, target_plan,
                                                  (uint16_t) (a - first_arg), call->args[a]))
                return false;
        }
    }
    return prepare_direct_call_ret_boundary(bundle, caller_plan, call, target, target_plan);
}

static bool prepare_func_boundary_steps(XaotBundle *bundle, const XaotFuncPlan *func_plan) {
    uint32_t bi;

    if (!bundle || !func_plan || !func_plan->func)
        return false;

    if (func_plan->abi.boundary_reason != XAOT_BOUNDARY_NONE) {
        XaotBoundaryStep *step =
            xaot_bundle_add_boundary_step(bundle, XAOT_BOUNDARY_STEP_FUNC_ABI, func_plan->func,
                                          NULL, NULL, func_plan->abi.boundary_reason);
        if (!step) {
            bundle->error_msg = "failed to allocate AOT function boundary step";
            return false;
        }
        step->from_rep = func_plan->abi.ret.rep;
        step->to_rep = func_plan->abi.ret.rep;
        bundle->stats.boundary_count++;
    }

    for (bi = 0; bi < func_plan->func->nblocks; bi++) {
        XiBlock *blk = func_plan->func->blocks[bi];
        uint32_t vi;
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            XiValue *value = blk->values[vi];
            if (!value)
                continue;
            if (value->op == XI_BOX || value->op == XI_UNBOX) {
                XaotBoundaryReason reason;
                XaotBoundaryStep *step;
                if (value->nargs < 1 || !value->args || !value->args[0]) {
                    bundle->error_msg = "AOT box/unbox boundary has no input value";
                    return false;
                }
                reason = value->op == XI_BOX ? XAOT_BOUNDARY_BOX : XAOT_BOUNDARY_UNBOX;
                step =
                    xaot_bundle_add_boundary_step(bundle, XAOT_BOUNDARY_STEP_VALUE_REP,
                                                  func_plan->func, value, value->args[0], reason);
                if (!step) {
                    bundle->error_msg = "failed to allocate AOT value boundary step";
                    return false;
                }
                bundle->stats.boundary_count++;
            }
            if ((value->op == XI_CALL || value->op == XI_CALL_METHOD ||
                 value->op == XI_CALL_METHOD_DIRECT) &&
                !prepare_direct_call_boundaries(bundle, func_plan, value))
                return false;
        }
    }
    return true;
}

/* Calls transfer control to bodies this scan does not inspect, so they
 * disqualify a function even when a pass has cleared their effect flags. */
static bool func_attr_op_is_call_like(uint16_t op) {
    switch ((XiOp) op) {
        case XI_CALL:
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
        case XI_TAIL_CALL:
        case XI_CALL_BUILTIN:
        case XI_CLOSURE_NEW:
        case XI_GO:
        case XI_THREAD_SPAWN:
        case XI_AWAIT:
        case XI_PRINT:
            return true;
        default:
            return false;
    }
}

static bool func_attr_op_is_closed_world_call_like(uint16_t op) {
    switch ((XiOp) op) {
        case XI_CALL:
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
        case XI_TAIL_CALL:
            return true;
        default:
            return false;
    }
}

static bool func_attr_value_is_ignorable_err_check(const XiValue *value) {
    return value && value->op == XI_ERR_CHECK &&
           (!value->type || value->type->kind != XR_KIND_BOOL);
}

/* Ops that observe mutable non-local state. Forced into the "reads memory"
 * bucket even if a pass cleared READS_MEM, because a const function must
 * not depend on state that can change between calls. */
static bool func_attr_op_reads_nonlocal(uint16_t op) {
    switch ((XiOp) op) {
        case XI_LOAD_UPVAL:
        case XI_GET_SHARED:
        case XI_GET_GLOBAL:
            return true;
        default:
            return false;
    }
}

static const XgBodySummary *prepare_find_body_summary_for_func(const XaotBundle *bundle,
                                                               const XiFunc *func,
                                                               uint32_t module_index,
                                                               bool is_module_init) {
    const XgGlobalEvidence *ev = bundle ? bundle->global_evidence_plan.evidence : NULL;
    const XgBodySummary *found = NULL;
    uint32_t name_id = 0;
    XgModuleId module_id = (XgModuleId) (module_index + 1u);

    if (!ev || !func)
        return NULL;
    if (!is_module_init) {
        if (!func->name)
            return NULL;
        name_id = xg_name_id(func->name);
    }
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        const XgBodySummary *body = &ev->bodies[i];
        if (is_module_init) {
            if (body->kind != XG_BODY_MODULE_INIT || body->module_id != module_id)
                continue;
        } else {
            if (body->kind == XG_BODY_MODULE_INIT || body->name_id != name_id)
                continue;
        }
        if (found)
            return NULL;
        found = body;
    }
    return found;
}

static bool func_attr_body_summary_disqualifies(const XaotBundle *bundle, const XgBodySummary *body,
                                                uint32_t *out_effect_bits) {
    const XgGlobalEvidence *ev = bundle ? bundle->global_evidence_plan.evidence : NULL;
    uint32_t effect_bits = 0;

    if (!body || !xg_body_effects_compose_closed_world_calls(ev, body, &effect_bits))
        return true;
    if (out_effect_bits)
        *out_effect_bits = effect_bits;
    return (effect_bits & (XG_BODY_MAY_THROW | XG_BODY_MAY_SUSPEND | XG_BODY_MAY_ALLOC |
                           XG_BODY_MAY_MUTATE | XG_BODY_MAY_CALL_NATIVE)) != 0;
}

/* Prove a function free of observable effects so Cgen can emit
 * __attribute__((const)) (touches no memory) or ((pure)) (reads only).
 * Evidence is the body summary plus per-value effect flags; the verifier
 * re-checks both.
 * Disqualification is not an error — the function simply gets no plan. */
static bool prepare_func_attr_plan(XaotBundle *bundle, const XiFunc *func,
                                   const XgBodySummary *body) {
    bool reads_mem;
    bool closed_world_calls_composed;
    uint32_t closed_world_call_ops = 0;
    uint32_t closed_world_callsite_count = 0;
    uint32_t composed_effect_bits = 0;
    uint32_t bi, vi;

    if (!bundle || !func)
        return false;
    if (func_attr_body_summary_disqualifies(bundle, body, &composed_effect_bits))
        return true;
    closed_world_calls_composed = (body->effect_bits & XG_BODY_MAY_CALL) != 0;
    closed_world_callsite_count = closed_world_calls_composed ? body->callsite_count : 0;
    reads_mem = ((composed_effect_bits & XG_BODY_MAY_READ_MEM) != 0) || func->ncaptures > 0;
    for (bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            bool composed_call = false;
            if (!v)
                continue;
            if (func_attr_op_is_call_like(v->op)) {
                if (!(closed_world_calls_composed && func_attr_op_is_closed_world_call_like(v->op)))
                    return true;
                closed_world_call_ops++;
                if (closed_world_call_ops > closed_world_callsite_count)
                    return true;
                composed_call = true;
            }
            if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND |
                            XI_FLAG_WRITES_MEM) &&
                !func_attr_value_is_ignorable_err_check(v) && !composed_call)
                return true;
            if (!composed_call) {
                if (xi_op_allocates(v->op))
                    return true;
                if ((v->flags & XI_FLAG_READS_MEM) || func_attr_op_reads_nonlocal(v->op))
                    reads_mem = true;
            }
        }
    }
    if (!xaot_bundle_add_func_attr_plan(bundle, func,
                                        reads_mem ? XAOT_FN_ATTR_PURE : XAOT_FN_ATTR_CONST, body)) {
        bundle->error_msg = "failed to allocate AOT function attribute plan";
        return false;
    }
    return true;
}

static bool prepare_func_recursive(XaotBundle *bundle, XiFunc *func, uint32_t module_index,
                                   uint16_t depth, bool is_module_init) {
    XaotFuncPlan *plan;
    const XgBodySummary *body;
    uint16_t ci;

    if (!bundle || !func)
        return false;
    plan = xaot_bundle_add_func_plan(bundle, func, module_index, depth);
    if (!plan) {
        bundle->error_msg = "failed to allocate AOT function plan";
        return false;
    }
    if (!prepare_func_type_plans(bundle, func))
        return false;
    if (!xaot_abi_build_func(&plan->abi, bundle, func, is_module_init)) {
        bundle->error_msg = "failed to build AOT function ABI";
        return false;
    }

    bundle->stats.functions_total++;
    if (plan->abi.kind == XAOT_ABI_NATIVE)
        bundle->stats.functions_native_abi++;
    else if (plan->abi.kind == XAOT_ABI_CORO)
        bundle->stats.functions_coro_abi++;
    else
        bundle->stats.functions_tagged_abi++;

    body = prepare_find_body_summary_for_func(bundle, func, module_index, is_module_init);

    if (!prepare_func_values(bundle, func))
        return false;
    if (!prepare_apply_return_abi_value_plans(bundle, plan))
        return false;
    if (!prepare_func_array_storage_plans(bundle, func))
        return false;
    if (!prepare_func_array_cache_plans(bundle, func))
        return false;
    if (!prepare_func_array_class_field_alloc_plans(bundle, func))
        return false;
    if (!prepare_func_bounds_plans(bundle, func, body))
        return false;
    if (!prepare_func_span_access_plans(bundle, func, body))
        return false;
    /* After bounds plans: the uniqueness proof requires every index access
     * to be raw (bounds-proven), which it reads from the bounds plans. */
    if (!prepare_func_alias_plans(bundle, func, body))
        return false;
    if (!prepare_func_closure_plans(bundle, func))
        return false;
    if (!prepare_func_transfer_plans(bundle, func))
        return false;
    /* Module inits are procedural entry points (called once, ordering-
     * sensitive); attribute plans only apply to ordinary functions. */
    if (!is_module_init && !prepare_func_attr_plan(bundle, func, body))
        return false;

    for (ci = 0; ci < func->nchildren; ci++) {
        if (!prepare_func_recursive(bundle, func->children[ci], module_index,
                                    (uint16_t) (depth + 1), false))
            return false;
    }
    if (!prepare_apply_aggregate_value_plans(bundle, func))
        return false;
    return true;
}

XR_FUNC bool xaot_prepare_bundle(XaotBundle *bundle, XaotPrepareStats *out_stats) {
    uint32_t mi;
    if (!bundle || !bundle->modules)
        return false;

    memset(&bundle->stats, 0, sizeof(bundle->stats));
    bundle->error_msg = NULL;

    for (mi = 0; mi < bundle->nmodules; mi++) {
        XiModule *mod = bundle->modules[mi];
        if (!mod || !mod->init) {
            bundle->error_msg = "module has no Xi init function";
            return false;
        }
        if (!prepare_func_recursive(bundle, mod->init, mi, 0, true))
            return false;
    }
    if (!xaot_bundle_sync_transfer_capability_plans(bundle)) {
        bundle->error_msg = "failed to sync AOT transfer capability plan";
        return false;
    }
    for (mi = 0; mi < bundle->nfunc_plans; mi++) {
        if (!prepare_func_boundary_steps(bundle, &bundle->func_plans[mi]))
            return false;
    }

    if (out_stats)
        *out_stats = bundle->stats;
    return true;
}
