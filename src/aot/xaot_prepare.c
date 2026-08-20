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
#include "emit_c/xr_c_emission_plan.h"
#include "xaot_boundary.h"
#include "xaot_class_native.h"
#include "xaot_callable.h"
#include "xaot_link.h"
#include "refine/xr_aot_representation_refinement.h"
#include "refine/xr_aot_scalar_value.h"
#include "xr_target_aggregate_c_projection.h"
#include "../base/xglobal_indices.h"
#include "../base/xmalloc.h"
#include "../frontend/analyzer/xa_intrinsic_registry.h"
#include "../frontend/analyzer/xbuiltin_receiver_registry.h"
#include "../frontend/analyzer/xa_selection.h"
#include "../ir/xi_analysis.h"
#include "../ir/xi_coro_analyze.h"
#include "../ir/xi_coro_lower.h"
#include "../ir/xi_escape.h"
#include "../ir/xi_effect.h"
#include "../ir/xi_opt.h"
#include "../ir/xi_own.h"
#include "../ir/xi_range.h"
#include "../ir/xi_value_query.h"
#include "../plan/target/xr_target_verify.h"
#include "../shared/xr_array_core.h"
#include <stdlib.h>
#include <string.h>

static bool value_reps_equal(XaotValueRep a, XaotValueRep b) {
    return xaot_value_reps_equal(a, b);
}

static bool prepare_require_target_plans(XaotBundle *bundle) {
    char error[256] = {0};
    XiRepPolicy policy = xi_rep_policy_native_boundary();

    if (!bundle || !bundle->modules || !bundle->target_plans) {
        if (bundle)
            bundle->error_msg = "AOT prepare requires module TargetPlans";
        return false;
    }
    for (uint32_t module_index = 0; module_index < bundle->nmodules; module_index++) {
        const XiModule *module = bundle->modules[module_index];
        const XrTargetPlan *target_plan = bundle->target_plans[module_index];
        if (!module || !module->init || !module->init->semantic_plan || !target_plan ||
            xr_target_plan_semantic_plan(target_plan) != module->init->semantic_plan ||
            xr_target_plan_completed_family_mask(target_plan) != XR_TARGET_REQUIRED_FAMILIES ||
            !xr_target_plan_is_verified(target_plan) ||
            !xr_target_plan_verify(target_plan, error, sizeof(error))) {
            bundle->error_msg = "AOT prepare rejected a missing or corrupt module TargetPlan";
            return false;
        }
        if (bundle->representation_refinements_required) {
            const XrAotRefinementPlan *refinement =
                xaot_bundle_representation_refinement_for_module(bundle, module_index);
            XrAotRefinementPlanView view = xr_aot_refinement_plan_view(refinement);
            if (!refinement ||
                !xaot_bundle_representation_policy_matches(bundle, module_index, &policy) ||
                !xr_aot_representation_materialization_verify(&view, module->init, target_plan,
                                                              &policy, NULL)) {
                bundle->error_msg =
                    "AOT prepare rejected a missing, corrupt, or stale representation refinement";
                return false;
            }
        }
    }
    return true;
}

static bool prepare_target_value_binding(XaotBundle *bundle, const XiFunc *func,
                                         const XiValue *value,
                                         const XrTargetValueRepRecord **out_binding) {
    const XrTargetPlan *target_plan;
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    char error[256] = {0};

    if (out_binding)
        *out_binding = NULL;
    target_plan = xaot_bundle_target_plan_for_func(bundle, func);
    if (!bundle || !func || !value || !out_binding || !target_plan) {
        if (bundle)
            bundle->error_msg = "AOT value lacks exact TargetPlan semantic identity";
        return false;
    }
    if (!xr_aot_scalar_semantic_value_id(target_plan, func, value, &semantic_function,
                                         &semantic_value, error, sizeof(error))) {
        if (xr_aot_rep_adapter_value_is_exact(target_plan, func, value, error, sizeof(error)))
            return true;
        bundle->error_msg = "AOT value lacks exact TargetPlan semantic identity";
        return false;
    }
    (void) semantic_function;
    *out_binding = xr_target_plan_value_rep(target_plan, semantic_value);
    return true;
}

/* RAW_PTR is shared by language Ptr/MutPtr and by the callee-side place of an
 * exact direct-local `ref Array<T>` parameter.  Rebuild the latter identity
 * from frozen SemanticPlan and TargetPlan rows so prepare never guesses the C
 * pointee spelling from the mutable Xi type. */
static const char *prepare_exact_raw_pointer_c_type(const XrTargetPlan *target_plan,
                                                    const XrTargetValueRepRecord *binding,
                                                    const XiValue *value) {
    const char *language_pointer = value ? xaot_raw_pointer_c_type(value->type) : NULL;
    if (language_pointer)
        return language_pointer;
    const XrSemanticPlan *semantic = target_plan ? xr_target_plan_semantic_plan(target_plan) : NULL;
    if (!semantic || !binding)
        return NULL;
    const XrSemanticParameterRecord *parameter = NULL;
    uint32_t parameter_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < (uint32_t) xr_semantic_plan_parameter_count(semantic); i++) {
        const XrSemanticParameterRecord *candidate = xr_semantic_plan_parameter(semantic, i);
        if (!candidate || candidate->value != binding->semantic_value)
            continue;
        if (parameter)
            return NULL;
        parameter = candidate;
        parameter_index = i;
    }
    const XrSemanticTypeRecord *array =
        parameter ? xr_semantic_plan_type(semantic, parameter->type) : NULL;
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    const XrTargetSlotRecord *slot = binding->slot < slot_count ? &slots[binding->slot] : NULL;
    if (!parameter || !array || !register_rep || !memory_rep || !slot ||
        parameter->mode != XR_PARAM_REF || parameter->ownership != XI_OWN_BORROWED ||
        parameter->transfer_mode != XR_TRANSFER_SHARE ||
        (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) != 0 || parameter->reserved != 0 ||
        array->kind != XR_KIND_ARRAY || array->builtin_type != XR_TID_NULL ||
        array->child_count != 1 || array->aggregate_extent != 0 || array->aggregate_align != 0 ||
        array->scalar_rep != XR_SCALAR_REP_NONE ||
        array->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        register_rep->kind != XR_MACHINE_REP_RAW_PTR ||
        memory_rep->kind != XR_MACHINE_REP_RAW_PTR ||
        register_rep->root_kind != XR_TARGET_ROOT_NONE ||
        memory_rep->root_kind != XR_TARGET_ROOT_NONE ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        slot->semantic_value != binding->semantic_value ||
        slot->semantic_operation != XR_SEMANTIC_INDEX_NONE ||
        slot->function != parameter->function || slot->role != XR_TARGET_SLOT_PARAMETER ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_NONE || slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return NULL;
    uint32_t argument_count = 0, call_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target_plan, &argument_count);
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    uint32_t matches = 0;
    for (uint32_t i = 0; arguments && calls && i < argument_count; i++) {
        const XrTargetCallArgumentRecord *argument = &arguments[i];
        if (argument->callee_parameter != parameter_index)
            continue;
        const XrTargetCallRecord *call =
            argument->call < call_count ? &calls[argument->call] : NULL;
        const XrTargetMachineRepRecord *caller_register =
            xr_target_plan_machine_rep(target_plan, argument->register_rep);
        const XrTargetMachineRepRecord *caller_memory =
            xr_target_plan_machine_rep(target_plan, argument->memory_rep);
        if (!call || !caller_register || !caller_memory ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
            call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
            call->callee_function != parameter->function ||
            argument->callee_slot != binding->slot ||
            argument->callee_register_rep != binding->register_rep ||
            argument->callee_memory_rep != binding->memory_rep ||
            argument->mode != XR_TARGET_CALL_REFERENCE ||
            argument->ownership != XR_TARGET_CALL_BORROW ||
            argument->transfer_mode != XR_TRANSFER_SHARE ||
            argument->flags != XR_TARGET_CALL_ARGUMENT_ADDRESSABLE ||
            argument->array_element_storage <= XR_TARGET_ARRAY_STORAGE_NONE ||
            argument->array_element_storage >= XR_TARGET_ARRAY_STORAGE_COUNT ||
            argument->reserved8[0] != 0 || argument->reserved8[1] != 0 ||
            argument->reserved8[2] != 0 || caller_register->kind != XR_MACHINE_REP_DYN_VALUE ||
            caller_memory->kind != XR_MACHINE_REP_DYN_VALUE ||
            caller_register->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
            caller_memory->ownership != XR_TARGET_OWNERSHIP_BORROWED)
            return NULL;
        matches++;
    }
    return matches ? "XrValue *" : NULL;
}

static bool prepare_target_machine_value_rep(const XrTargetPlan *target_plan,
                                             const XrTargetValueRepRecord *binding,
                                             const XiValue *value, XaotValueRep *out) {
    const XrTargetMachineRepRecord *machine;
    const XaotRepInfo *info;
    XaotRep rep;

    if (!target_plan || !binding || !value || !out)
        return false;
    machine = xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    if (!machine)
        return false;
    if (machine->kind == XR_MACHINE_REP_AGGREGATE) {
        XrCAggregateProjection projection = {0};
        if (!xr_c_aggregate_projection(target_plan, binding, &projection))
            return false;
        memset(out, 0, sizeof(*out));
        out->kind = projection.kind == XR_C_AGGREGATE_PROJECTION_NAMED_STRUCT ? XAOT_VALUE_AGGREGATE
                    : (projection.kind == XR_C_AGGREGATE_PROJECTION_FIXED_ARRAY_BACKING ||
                       projection.kind == XR_C_AGGREGATE_PROJECTION_TUPLE_BACKING)
                        ? XAOT_VALUE_TAGGED
                        : XAOT_VALUE_VOID;
        if (out->kind == XAOT_VALUE_VOID)
            return false;
        out->rep = XAOT_REP_TAGGED;
        out->type = value->type;
        out->c_type = xr_strdup(projection.c_type);
        if (!out->c_type)
            return false;
        out->flags = XAOT_VALUE_FLAG_DYNAMIC_C_TYPE | XAOT_VALUE_FLAG_OWNED_C_TYPE;
        if (projection.kind == XR_C_AGGREGATE_PROJECTION_NAMED_STRUCT)
            out->flags |= XAOT_VALUE_FLAG_STRUCT;
        return true;
    }
    switch ((XrMachineRepKind) machine->kind) {
        case XR_MACHINE_REP_VOID:
            rep = XAOT_REP_VOID;
            break;
        case XR_MACHINE_REP_I1:
            rep = XAOT_REP_BOOL;
            break;
        case XR_MACHINE_REP_I8:
            rep = XAOT_REP_I8;
            break;
        case XR_MACHINE_REP_U8:
            rep = XAOT_REP_U8;
            break;
        case XR_MACHINE_REP_I16:
            rep = XAOT_REP_I16;
            break;
        case XR_MACHINE_REP_U16:
            rep = XAOT_REP_U16;
            break;
        case XR_MACHINE_REP_I32:
            rep = XAOT_REP_I32;
            break;
        case XR_MACHINE_REP_U32:
            rep = XAOT_REP_U32;
            break;
        case XR_MACHINE_REP_I64:
            rep = XAOT_REP_I64;
            break;
        case XR_MACHINE_REP_ENUM_ORDINAL:
            rep = XAOT_REP_I64;
            break;
        case XR_MACHINE_REP_U64:
            rep = XAOT_REP_U64;
            break;
        case XR_MACHINE_REP_ISIZE:
            rep = XAOT_REP_ISIZE;
            break;
        case XR_MACHINE_REP_USIZE:
            rep = XAOT_REP_USIZE;
            break;
        case XR_MACHINE_REP_F32:
            rep = XAOT_REP_F32;
            break;
        case XR_MACHINE_REP_F64:
            rep = XAOT_REP_F64;
            break;
        case XR_MACHINE_REP_RUNE:
            rep = XAOT_REP_RUNE;
            break;
        case XR_MACHINE_REP_DYN_VALUE:
            rep = XAOT_REP_TAGGED;
            break;
        case XR_MACHINE_REP_VIEW:
            rep = XAOT_REP_SLICE;
            break;
        case XR_MACHINE_REP_RAW_PTR:
            rep = XAOT_REP_RAWPTR;
            break;
        default:
            return false;
    }
    info = xaot_rep_info(rep);
    if (!info)
        return false;
    memset(out, 0, sizeof(*out));
    out->kind = rep == XAOT_REP_VOID     ? XAOT_VALUE_VOID
                : rep == XAOT_REP_TAGGED ? XAOT_VALUE_TAGGED
                : rep == XAOT_REP_SLICE  ? XAOT_VALUE_AGGREGATE
                : rep == XAOT_REP_RAWPTR ? XAOT_VALUE_PTR
                                         : XAOT_VALUE_SCALAR;
    out->rep = rep;
    out->type = value->type;
    out->c_type = machine->kind == XR_MACHINE_REP_RAW_PTR
                      ? prepare_exact_raw_pointer_c_type(target_plan, binding, value)
                      : info->c_type;
    if (!out->c_type)
        return false;
    if (machine->kind == XR_MACHINE_REP_ENUM_ORDINAL)
        out->flags = XAOT_VALUE_FLAG_ENUM;
    return true;
}

static bool prepare_effective_value_rep(XaotBundle *bundle, const XiFunc *func,
                                        const XiValue *value, XaotValueRep *out) {
    const XrTargetValueRepRecord *binding;
    const XrTargetPlan *target_plan;
    const XaotValuePlan *legacy;

    if (!prepare_target_value_binding(bundle, func, value, &binding))
        return false;
    if (binding) {
        target_plan = xaot_bundle_target_plan_for_func(bundle, func);
        if (!prepare_target_machine_value_rep(target_plan, binding, value, out)) {
            bundle->error_msg = "AOT TargetPlan binding has no supported C value rep";
            return false;
        }
        return true;
    }
    legacy = xaot_bundle_find_value_plan(bundle, value);
    if (!legacy) {
        bundle->error_msg = "AOT unmigrated value has no legacy value plan";
        return false;
    }
    *out = xaot_value_rep_borrow(legacy->rep);
    return true;
}

static void prepare_value_plan_set_rep(XaotValuePlan *plan, XaotValueRep rep) {
    if (!plan)
        return;
    if ((plan->rep.flags & XAOT_VALUE_FLAG_OWNED_C_TYPE) != 0) {
        if (plan->rep.c_type == rep.c_type)
            rep.flags |= XAOT_VALUE_FLAG_OWNED_C_TYPE;
        else
            xaot_value_rep_dispose(&plan->rep);
    }
    /* A borrowed dynamic spelling may originate in another value plan.  Those
     * plans are refined independently during prepare, so the source can drop
     * its old spelling before CGen consumes this row.  Give every persistent
     * value plan its own copy; process-lifetime/static spellings stay borrowed
     * and allocation-free. */
    if (rep.c_type && (rep.flags & XAOT_VALUE_FLAG_DYNAMIC_C_TYPE) != 0 &&
        (rep.flags & XAOT_VALUE_FLAG_OWNED_C_TYPE) == 0) {
        char *owned_c_type = xr_strdup(rep.c_type);
        if (!owned_c_type)
            abort();
        rep.c_type = owned_c_type;
        rep.flags |= XAOT_VALUE_FLAG_OWNED_C_TYPE;
    }
    plan->rep = rep;
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

static bool prepare_type_is_unit_enum_ordinal(const XaotBundle *bundle, const XrType *type) {
    if (!bundle || !type || type->is_nullable || type->kind != XR_KIND_ENUM ||
        !type->enum_type.layout || !type->enum_type.layout->is_zero_payload)
        return false;

    /* Freestanding static enums have always used their declaration ordinal.
     * Hosted code may use that compact representation only when the bundle
     * owns a concrete enum plan.  Prelude/runtime enums such as SendResult
     * cross runtime boundaries as tagged values and must not be reinterpreted
     * as an i64 merely because their source type happens to be unit-only. */
    if (prepare_bundle_is_freestanding(bundle))
        return true;
    const XaotEnumPlan *plan = xaot_bundle_find_enum_plan_for_type(bundle, type);
    return plan && plan->enum_data && plan->member_count > 0 && plan->max_payload == 0;
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

static bool prepare_value_is_unit_enum_ordinal_member(const XaotBundle *bundle, const XiFunc *func,
                                                      const XiValue *value);
static bool prepare_value_is_unit_enum_ordinal_compare_member(const XaotBundle *bundle,
                                                              const XiFunc *func,
                                                              const XiValue *value);

static void apply_unit_enum_ordinal_value_plan(XaotBundle *bundle, XaotValuePlan *vp) {
    if (!vp)
        return;
    if (!prepare_type_is_unit_enum_ordinal(bundle, vp->value ? vp->value->type : NULL) &&
        !prepare_value_is_unit_enum_ordinal_member(bundle, vp->func, vp->value) &&
        !prepare_value_is_unit_enum_ordinal_compare_member(bundle, vp->func, vp->value))
        return;
    prepare_value_plan_set_rep(vp, prepare_enum_ordinal_value_rep(vp->value->type));
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
        case XR_KIND_SLICE:
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
        prepare_value_plan_set_rep(vp, ptr_value_rep_for_type(vp->value->type));
}

static void record_value_stats(XaotPrepareStats *stats, XaotValueKind kind, bool enum_ordinal,
                               bool rep_adapter) {
    if (!stats)
        return;
    stats->values_total++;
    if (rep_adapter) {
        stats->values_rep_adapter++;
        return;
    }
    if (enum_ordinal) {
        stats->values_enum_ordinal++;
        return;
    }
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
        case XAOT_VALUE_VECTOR:
            stats->values_vector++;
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
            xi_op_is_identity_forward(v->op)) &&
           v->nargs >= 1) {
        v = v->args[0];
    }
    return v;
}

static bool prepare_builtin_receiver_pod_span_elem(const XrType *type) {
    if (!type || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return true;
        default:
            return false;
    }
}

static bool prepare_builtin_receiver_registry_matches(const XrType *receiver_type,
                                                      XaBuiltinReceiverKind kind) {
    switch (kind) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
            return receiver_type && receiver_type->kind == XR_KIND_INT &&
                   !receiver_type->is_nullable;
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            return xr_type_is_exact_unsigned_integer(receiver_type);
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return xr_type_is_u8_array(receiver_type);
        case XA_BUILTIN_RECEIVER_ARRAY:
            return receiver_type && receiver_type->kind == XR_KIND_ARRAY;
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return xr_type_is_u8_slice(receiver_type);
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return receiver_type && receiver_type->kind == XR_KIND_SLICE &&
                   prepare_builtin_receiver_pod_span_elem(receiver_type->container.element_type);
    }
    return false;
}

static bool prepare_call_method_matches_receiver_registry_id(const XiValue *call,
                                                             XaBuiltinReceiverMethodId method_id) {
    const XaBuiltinReceiverMethodSpec *spec = xa_builtin_receiver_method_by_id(method_id);
    const XiValue *v = unwrap_identity_value(call);
    if (!spec || !v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || !v->aux ||
        v->nargs < 1 || !v->args[0])
        return false;
    return strcmp((const char *) v->aux, spec->source_name) == 0 &&
           prepare_builtin_receiver_registry_matches(v->args[0]->type, spec->receiver);
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

static bool prepare_value_is_unit_enum_ordinal_member(const XaotBundle *bundle, const XiFunc *func,
                                                      const XiValue *value) {
    const XiValue *origin = unwrap_identity_value(value);
    if (!origin || origin->op != XI_LOAD_FIELD || origin->nargs < 1 || !origin->aux)
        return false;
    const XiEnumData *ed = prepare_enum_for_shared_value(bundle, func, origin->args[0]);
    int member_index = prepare_enum_member_index(ed, (const char *) origin->aux);
    if (!prepare_enum_data_is_zero_payload_simple(ed) || member_index < 0)
        return false;
    return !ed->members || ed->members[member_index].payload_count == 0;
}

static bool prepare_value_is_unit_enum_ordinal_compare_member(const XaotBundle *bundle,
                                                              const XiFunc *func,
                                                              const XiValue *value) {
    if (!func || !prepare_value_is_namespace_field_load(value))
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
                (prepare_type_is_unit_enum_ordinal(bundle, rhs ? rhs->type : NULL) ||
                 prepare_value_plan_is_i64_enum(bundle, rhs)))
                return true;
            if (rhs == target &&
                (prepare_type_is_unit_enum_ordinal(bundle, lhs ? lhs->type : NULL) ||
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
        case XR_KIND_SLICE:
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
        case XR_KIND_ENUM:
            for (int i = 0; i < type->enum_type.type_arg_count; i++) {
                if (!prepare_type_plans_for_type(
                        bundle, type->enum_type.type_args ? type->enum_type.type_args[i] : NULL,
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
                if (!prepare_type_plans_for_type(bundle, xr_type_function_param_type(type, i),
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

    if (!value || value->op != XI_CALL_BUILTIN)
        return false;
    if (value->array_intrinsic_kind == XI_ARRAY_INTRINSIC_WITH_CAPACITY ||
        value->array_intrinsic_kind == XI_ARRAY_INTRINSIC_FILLED_NEW)
        return true;
    if (!value->aux)
        return false;
    name = (const char *) value->aux;
    return strcmp(name, "array_new") == 0 || strcmp(name, "array_copy_new") == 0;
}

static bool array_builtin_forwards_storage(const XiValue *value) {
    const char *name;

    if (!value || value->op != XI_CALL_BUILTIN || value->nargs < 1)
        return false;
    if (value->xa_intrinsic_id == XA_INTRINSIC_ARRAY_RESERVE)
        return true;
    if (!value->aux)
        return false;
    name = (const char *) value->aux;
    return strcmp(name, "array_resize") == 0;
}

static bool array_hof_call_is_exact(XaotBundle *bundle, const XiValue *value,
                                    bool require_owned_array_result) {
    const XiFunc *function = value && value->block ? value->block->func : NULL;
    const XrTargetValueRepRecord *binding = NULL;
    if (!bundle || !function || !value ||
        !prepare_target_value_binding(bundle, function, value, &binding) || !binding)
        return false;
    const XrTargetPlan *target = xaot_bundle_target_plan_for_func(bundle, function);
    const XrSemanticPlan *semantic = target ? xr_target_plan_semantic_plan(target) : NULL;
    if (!target || !semantic || !xr_target_plan_is_verified(target) ||
        (xr_target_plan_completed_family_mask(target) &
         XR_TARGET_FAMILY_ARRAY_HOF_RESULT_STORAGE) == 0)
        return false;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->function != function->semantic_plan_function_index ||
            candidate->result_value != binding->semantic_value)
            continue;
        if (operation)
            return false;
        operation = candidate;
        operation_index = i;
    }
    uint8_t expected_semantic = operation ? operation->array_hof_kind : XR_SEM_ARRAY_HOF_NONE;
    uint8_t expected_target =
        expected_semantic == XR_SEM_ARRAY_HOF_MAP      ? XR_TARGET_ARRAY_HOF_MAP
        : expected_semantic == XR_SEM_ARRAY_HOF_FILTER ? XR_TARGET_ARRAY_HOF_FILTER
        : expected_semantic == XR_SEM_ARRAY_HOF_REDUCE ? XR_TARGET_ARRAY_HOF_REDUCE
                                                       : XR_TARGET_ARRAY_HOF_NONE;
    uint8_t expected_live = expected_target == XR_TARGET_ARRAY_HOF_MAP      ? XI_ARRAY_HOF_MAP
                            : expected_target == XR_TARGET_ARRAY_HOF_FILTER ? XI_ARRAY_HOF_FILTER
                            : expected_target == XR_TARGET_ARRAY_HOF_REDUCE ? XI_ARRAY_HOF_REDUCE
                                                                            : XI_ARRAY_HOF_NONE;
    if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_HOF ||
        expected_target == XR_TARGET_ARRAY_HOF_NONE || value->op != XI_CALL_METHOD ||
        value->array_hof_kind != expected_live)
        return false;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    uint16_t expected_arguments = expected_target == XR_TARGET_ARRAY_HOF_REDUCE ? 3u : 2u;
    if (!call || call->caller_function != function->semantic_plan_function_index ||
        call->callee_function != operation->callable_function ||
        call->result_value != binding->semantic_value ||
        call->argument_count != expected_arguments ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_ARRAY_HOF ||
        call->target_kind != XR_TARGET_CALL_TARGET_ARRAY_HOF ||
        call->array_hof_kind != expected_target ||
        call->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE ||
        call->array_result_element_storage == XR_TARGET_ARRAY_STORAGE_NONE)
        return false;
    if (!require_owned_array_result)
        return true;
    if (expected_target != XR_TARGET_ARRAY_HOF_MAP && expected_target != XR_TARGET_ARRAY_HOF_FILTER)
        return false;
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target, binding->memory_rep);
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target, &slot_count);
    const XrTargetSlotRecord *slot =
        slots && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    return register_rep && memory_rep && slot && register_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
           memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
           slot->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           slot->ownership == XR_TARGET_OWNERSHIP_OWNED;
}

static bool array_hof_result_is_exact(XaotBundle *bundle, const XiValue *value) {
    return array_hof_call_is_exact(bundle, value, true);
}

static bool prepare_target_binding_has_machine_kind(XaotBundle *bundle, const XiValue *value,
                                                    XrMachineRepKind kind) {
    const XrTargetValueRepRecord *binding = NULL;
    const XiFunc *function = value && value->block ? value->block->func : NULL;
    if (!bundle || !function || !prepare_target_value_binding(bundle, function, value, &binding) ||
        !binding)
        return false;
    const XrTargetPlan *target_plan = xaot_bundle_target_plan_for_func(bundle, function);
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    return register_rep && memory_rep && register_rep->kind == kind && memory_rep->kind == kind;
}

static bool derive_array_storage_plan(XaotBundle *bundle, const XiValue *array_value,
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

    /* A ref parameter is the address of the callee's XrValue slot, not an
     * Array value.  Only its exact PLACE_LOAD result participates in Array
     * storage.  The verified TargetPlan RAW_PTR/DYN pair is the prior identity;
     * no parameter name, source spelling, or generic Array type inference is
     * accepted here. */
    if (value->op == XI_PARAM &&
        prepare_target_binding_has_machine_kind(bundle, value, XR_MACHINE_REP_RAW_PTR))
        return false;
    if (value->op == XI_PLACE_LOAD && value->nargs == 1 && value->args &&
        prepare_target_binding_has_machine_kind(bundle, value, XR_MACHINE_REP_DYN_VALUE) &&
        prepare_target_binding_has_machine_kind(bundle, value->args[0], XR_MACHINE_REP_RAW_PTR)) {
        if (out_elem)
            *out_elem = self_elem;
        if (out_origin)
            *out_origin = value;
        return true;
    }

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

    if (value->type && xaot_type_contains_unresolved_type_param(value->type))
        return false;

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

    if (array_hof_result_is_exact(bundle, value)) {
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
                case XI_LEN:
                    break;
                case XI_RETAIN:
                case XI_RELEASE:
                    /* Reference-count bookkeeping never replaces or mutates
                     * the Array payload.  A verified cache remains live until
                     * the ownership operation itself executes. */
                    if (!value_arg_matches(cur, target, 0))
                        break;
                    if (cur->nargs != 1 || !same_value(cur->args[0], target))
                        return true;
                    break;
                case XI_ERR_CHECK:
                    /* Cleanup operands are consumed only on the cold error
                     * edge.  They release the owner but do not mutate its
                     * element storage before any cached access. */
                    for (uint16_t arg = 0; arg < cur->nargs; arg++) {
                        if (same_value(cur->args[arg], target) &&
                            arg < XI_ERR_CHECK_CLEANUP_ARG_BASE)
                            return true;
                    }
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
        if (name && (strcmp(name, "array_new") == 0 || strcmp(name, "array_copy_new") == 0))
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
    if (!push || push->op != XI_CALL_METHOD || push->nargs != 2 || !push->block ||
        !prepare_call_method_matches_receiver_registry_id(push,
                                                          XA_BUILTIN_RECEIVER_METHOD_ARRAY_PUSH))
        return false;
    /* Keep plan construction in lockstep with C emission.  When the global
     * capacity pass attached a site, raw fill-loop stores are legal only if
     * that site proved an exact one-shot reservation.  Otherwise Cgen keeps
     * the growing runtime push, so caching array->data here would leave a
     * stale pointer after the first relocation. */
    if (push->xg_capacity_op_id != XG_NO_ID) {
        const XaotCapacityPlan *plan =
            xaot_bundle_find_capacity_plan(bundle, (XgCapacityOpId) push->xg_capacity_op_id);
        const uint32_t required = XAOT_CAPACITY_EV_EXACT_COUNT | XAOT_CAPACITY_EV_LOOP_APPEND |
                                  XAOT_CAPACITY_EV_NO_CLOBBER;
        if (!plan || plan->action != XAOT_CAPACITY_RESERVE_ONCE ||
            (plan->evidence & required) != required)
            return false;
    }
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

static bool prepare_array_value_mutates_origin_directly(XaotBundle *bundle, const XiValue *value,
                                                        const XiValue *origin) {
    if (!value || !origin)
        return false;
    if (value->op == XI_INDEX_SET && value->nargs >= 1 &&
        prepare_array_single_origin(value->args[0], 0) == origin)
        return true;
    if (value->op == XI_CALL_METHOD && value->nargs >= 1 &&
        prepare_array_single_origin(value->args[0], 0) == origin) {
        if (array_hof_call_is_exact(bundle, value, false))
            return false;
        if (value->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM))
            return true;
    }
    return false;
}

static bool prepare_array_origin_has_only_fill_mutation(XaotBundle *bundle, const XiFunc *func,
                                                        const XiValue *origin,
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
            if (prepare_array_value_mutates_origin_directly(bundle, value, origin))
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
    if (!have || !prepare_array_origin_has_only_fill_mutation(bundle, func, origin, found.push))
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
    return strcmp(name, "array_copy_new") == 0;
}

static bool prepare_array_native_local_arg_use_is_safe(const XiValue *user, uint16_t arg_index) {
    if (!user)
        return false;
    switch ((XiOp) user->op) {
        case XI_INDEX_GET:
        case XI_INDEX_SET:
            return arg_index == 0;
        case XI_BYTE_SLICE_LOAD_U16:
        case XI_BYTE_SLICE_LOAD_U32:
        case XI_BYTE_SLICE_LOAD_U64:
        case XI_BYTE_SLICE_LOAD_F32:
        case XI_BYTE_SLICE_LOAD_F64:
        case XI_BYTE_SLICE_STORE_U16:
        case XI_BYTE_SLICE_STORE_U32:
        case XI_BYTE_SLICE_STORE_U64:
        case XI_BYTE_SLICE_STORE_F32:
        case XI_BYTE_SLICE_STORE_F64:
        case XI_BYTE_SLICE_FILL:
        case XI_BYTE_SLICE_REPEAT:
        case XI_SLICE_WINDOW:
        case XI_SLICE_AS_BYTES:
        case XI_SLICE_FILL:
        case XI_SLICE_REINTERPRET:
        case XI_ARRAY_DATA_PTR:
        case XI_BYTE_ARRAY_COPY_WITHIN:
        case XI_BYTE_ARRAY_REPEAT_FROM:
            return arg_index == 0;
        case XI_SLICE_COPY:
        case XI_SLICE_COMPARE:
        case XI_BYTE_SLICE_COPY:
        case XI_BYTE_SLICE_COMPARE:
        case XI_BYTE_SLICE_COMMON_PREFIX:
            return arg_index == 0 || arg_index == 1;
        case XI_BYTE_ARRAY_APPEND_FROM:
        case XI_BYTE_ARRAY_COPY_FROM:
            return arg_index == 0 || arg_index == 1;
        case XI_LEN:
            return arg_index == 0;
        case XI_CALL_BUILTIN:
            return user->xa_intrinsic_id == XA_INTRINSIC_ARRAY_RESERVE && arg_index == 0;
        case XI_CALL_METHOD: {
            /* Native-local representation keeps the array object itself as a
             * direct pointer; relocating its backing data does not invalidate
             * that pointer.  Data-cache eligibility is checked separately. */
            if (arg_index == 0 && (prepare_call_method_matches_receiver_registry_id(
                                       user, XA_BUILTIN_RECEIVER_METHOD_ARRAY_PUSH) ||
                                   prepare_call_method_matches_receiver_registry_id(
                                       user, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM) ||
                                   prepare_call_method_matches_receiver_registry_id(
                                       user, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_REPEAT_FROM)))
                return true;
            if (arg_index == 1 && prepare_call_method_matches_receiver_registry_id(
                                      user, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM))
                return true;
            return false;
        }
        case XI_RETAIN:
        case XI_RELEASE:
            return arg_index == 0;
        case XI_BOX:
        case XI_UNBOX:
        case XI_COPY:
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
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
    /* Sequential strip/tail loops legitimately form a chain of non-negative
     * induction phis (for example 32-byte, 8-byte, 4-byte, then byte tails).
     * The root checks below still reject cycles; this limit is only a bounded
     * walk guard and must not truncate those ordinary SSA chains. */
    if (!value || depth > 32)
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
    if (!value || value->op != XI_LEN || value->nargs != 1)
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

/* Slice descriptors are immutable values: once a branch establishes
 * `index < len(slice)`, later calls may change the pointed-to bytes but cannot
 * change that descriptor's length.  Accept the equivalent true/false branch
 * spellings and carry the fact through dominance.  This deliberately remains
 * exact-SSA (no alias or interval guessing) and is restricted to Slice so an
 * Array mutation cannot invalidate the proof. */
static bool prepare_slice_dominating_index_guard_bounds_proven(const XaotBundle *bundle,
                                                               const XiFunc *func,
                                                               const XiValue *array_value,
                                                               const XiValue *index_value,
                                                               const XiBlock *site) {
    const XiValue *receiver = unwrap_identity_value(array_value);
    if (!bundle || !func || !receiver || !receiver->type || receiver->type->kind != XR_KIND_SLICE ||
        !index_value || !site)
        return false;

    xi_ensure_dominators((XiFunc *) func);
    for (const XiBlock *guard = site->idom; guard; guard = guard->idom) {
        if (guard->kind != XI_BLOCK_IF || !guard->succs[0] || !guard->succs[1])
            continue;
        bool true_path = xi_dominates(guard->succs[0], site);
        bool false_path = xi_dominates(guard->succs[1], site);
        if (true_path == false_path)
            continue;

        const XiValue *condition = unwrap_identity_value(guard->control);
        if (!condition || condition->nargs < 2)
            continue;
        const XiValue *lhs = unwrap_identity_value(condition->args[0]);
        const XiValue *rhs = unwrap_identity_value(condition->args[1]);
        bool lhs_index = same_value(lhs, index_value);
        bool rhs_index = same_value(rhs, index_value);
        bool lhs_length = prepare_array_length_value_matches(bundle, func, lhs, receiver);
        bool rhs_length = prepare_array_length_value_matches(bundle, func, rhs, receiver);

        if ((true_path && condition->op == XI_LT && lhs_index && rhs_length) ||
            (true_path && condition->op == XI_GT && lhs_length && rhs_index) ||
            (false_path && condition->op == XI_GE && lhs_index && rhs_length) ||
            (false_path && condition->op == XI_LE && lhs_length && rhs_index))
            return true;
    }
    return false;
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

typedef struct PrepareAffineOffset {
    const XiValue *base;
    int64_t constant;
} PrepareAffineOffset;

static bool prepare_affine_add_constant(int64_t lhs, int64_t rhs, int64_t *out) {
    if (!out || (rhs > 0 && lhs > INT64_MAX - rhs) || (rhs < 0 && lhs < INT64_MIN - rhs))
        return false;
    *out = lhs + rhs;
    return true;
}

/* Decompose only the narrow affine forms emitted for Slice offsets and their
 * guards. The dynamic base remains an exact SSA identity; this never widens a
 * relational fact into an interval assumption. */
static bool prepare_affine_offset(const XiValue *value, PrepareAffineOffset *out, uint8_t depth) {
    const XiValue *current = unwrap_identity_value(value);
    if (!current || !out || depth > 8)
        return false;
    if (current->op == XI_CONST && current->type && current->type->kind == XR_KIND_INT) {
        out->base = NULL;
        out->constant = current->aux_int;
        return true;
    }
    if ((current->op == XI_ADD || current->op == XI_SUB) && current->nargs >= 2) {
        PrepareAffineOffset lhs;
        PrepareAffineOffset rhs;
        if (prepare_affine_offset(current->args[0], &lhs, (uint8_t) (depth + 1)) &&
            prepare_affine_offset(current->args[1], &rhs, (uint8_t) (depth + 1))) {
            int64_t rhs_constant = rhs.constant;
            if (current->op == XI_SUB) {
                if (rhs.base || rhs_constant == INT64_MIN)
                    goto non_affine;
                rhs_constant = -rhs_constant;
            }
            if (lhs.base && rhs.base)
                goto non_affine;
            out->base = lhs.base ? lhs.base : rhs.base;
            return prepare_affine_add_constant(lhs.constant, rhs_constant, &out->constant);
        }
    }

non_affine:
    out->base = current;
    out->constant = 0;
    return true;
}

static int64_t prepare_byte_access_width(const XiValue *value) {
    if (!value)
        return 0;
    switch ((XiOp) value->op) {
        case XI_BYTE_SLICE_LOAD_U16:
        case XI_BYTE_SLICE_STORE_U16:
            return 2;
        case XI_BYTE_SLICE_LOAD_U32:
        case XI_BYTE_SLICE_LOAD_F32:
        case XI_BYTE_SLICE_STORE_U32:
        case XI_BYTE_SLICE_STORE_F32:
            return 4;
        case XI_BYTE_SLICE_LOAD_U64:
        case XI_BYTE_SLICE_LOAD_F64:
        case XI_BYTE_SLICE_STORE_U64:
        case XI_BYTE_SLICE_STORE_F64:
            return 8;
        default:
            return 0;
    }
}

/* Prove a fixed-width byte access from a dominating relational guard:
 *
 *     while (i <= len(bytes) - stripe) {
 *         bytes.load<T>(i + lane, ...)
 *     }
 *
 * The same Slice receiver, the same affine loop base, non-negativity and
 * lane+sizeof(T)<=stripe are all required. This removes only a redundant
 * local bounds branch; malformed inputs still fail at the checked boundary. */
static bool prepare_span_byte_guard_bounds_proven(const XaotBundle *bundle, const XiFunc *func,
                                                  const XiValue *value) {
    int64_t width = prepare_byte_access_width(value);
    if (!bundle || !func || !value || width == 0 || value->nargs < 2 || !value->block)
        return false;
    const XiValue *receiver = unwrap_identity_value(value->args[0]);
    const XiValue *offset = unwrap_identity_value(value->args[1]);
    PrepareAffineOffset access;
    if (!receiver || !offset || !prepare_array_value_known_nonnegative(offset, NULL, 0) ||
        !prepare_affine_offset(offset, &access, 0) || !access.base)
        return false;

    xi_ensure_dominators((XiFunc *) func);
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *guard = func->blocks[bi];
        if (!guard || guard->kind != XI_BLOCK_IF || !guard->succs[0] ||
            !(guard->succs[0] == value->block || xi_dominates(guard->succs[0], value->block)))
            continue;
        const XiValue *condition = unwrap_identity_value(guard->control);
        if (!condition || (condition->op != XI_LE && condition->op != XI_LT) ||
            condition->nargs < 2)
            continue;
        PrepareAffineOffset lhs;
        PrepareAffineOffset rhs;
        if (!prepare_affine_offset(condition->args[0], &lhs, 0) ||
            !prepare_affine_offset(condition->args[1], &rhs, 0) || !lhs.base || !rhs.base ||
            !same_value(lhs.base, access.base) ||
            !prepare_array_length_value_matches(bundle, func, rhs.base, receiver))
            continue;
        int64_t required = 0;
        if (!prepare_affine_add_constant(access.constant, width, &required) ||
            !prepare_affine_add_constant(required, rhs.constant, &required) ||
            !prepare_affine_add_constant(required, -lhs.constant, &required))
            continue;
        /* lhs < rhs grants one additional integer unit compared with <=. */
        if (required <= (condition->op == XI_LT ? 1 : 0))
            return true;
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
    if (!prepare_array_value_known_nonnegative(index_value, NULL, 0) &&
        !xi_value_known_nonnegative_at(func, index_value, value->block)) {
        bounds_fail(fail_reason, XAOT_BOUNDS_UNPROVEN_INDEX_RANGE);
        return false;
    }
    if (prepare_slice_dominating_index_guard_bounds_proven(bundle, func, array_value, index_value,
                                                           value->block))
        return true;
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
            *out_kind = XAOT_SLICE_ACCESS_INDEX_GET;
            return true;
        case XI_INDEX_SET:
            *out_kind = XAOT_SLICE_ACCESS_INDEX_SET;
            return true;
        case XI_BYTE_SLICE_LOAD_U16:
        case XI_BYTE_SLICE_LOAD_U32:
        case XI_BYTE_SLICE_LOAD_U64:
        case XI_BYTE_SLICE_LOAD_F32:
        case XI_BYTE_SLICE_LOAD_F64:
            *out_kind = XAOT_SLICE_ACCESS_BYTE_LOAD;
            return true;
        case XI_BYTE_SLICE_STORE_U16:
        case XI_BYTE_SLICE_STORE_U32:
        case XI_BYTE_SLICE_STORE_U64:
        case XI_BYTE_SLICE_STORE_F32:
        case XI_BYTE_SLICE_STORE_F64:
            *out_kind = XAOT_SLICE_ACCESS_BYTE_STORE;
            return true;
        case XI_BYTE_SLICE_FILL:
            *out_kind = XAOT_SLICE_ACCESS_BYTE_FILL;
            return true;
        case XI_BYTE_SLICE_COPY:
            *out_kind = XAOT_SLICE_ACCESS_BYTE_COPY;
            return true;
        case XI_BYTE_SLICE_COMPARE:
            *out_kind = XAOT_SLICE_ACCESS_BYTE_COMPARE;
            return true;
        case XI_BYTE_SLICE_COMMON_PREFIX:
            *out_kind = XAOT_SLICE_ACCESS_BYTE_COMMON_PREFIX;
            return true;
        case XI_BYTE_SLICE_REPEAT:
            *out_kind = XAOT_SLICE_ACCESS_BYTE_REPEAT;
            return true;
        case XI_SLICE_AS_BYTES:
            *out_kind = XAOT_SLICE_ACCESS_SLICE_AS_BYTES;
            return true;
        case XI_SLICE_FILL:
            *out_kind = XAOT_SLICE_ACCESS_SLICE_FILL;
            return true;
        case XI_SLICE_COPY:
            *out_kind = XAOT_SLICE_ACCESS_SLICE_COPY;
            return true;
        case XI_SLICE_COMPARE:
            *out_kind = XAOT_SLICE_ACCESS_SLICE_COMPARE;
            return true;
        case XI_SLICE_REINTERPRET:
            *out_kind = XAOT_SLICE_ACCESS_REINTERPRET;
            return true;
        case XI_VEC_LOAD:
            *out_kind = XAOT_SLICE_ACCESS_VEC_LOAD;
            return true;
        case XI_VEC_STORE:
            *out_kind = XAOT_SLICE_ACCESS_VEC_STORE;
            return true;
        case XI_SLICE_WINDOW:
            *out_kind = XAOT_SLICE_ACCESS_WINDOW;
            return true;
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
            if (value->nargs != 2)
                return false;
            if (prepare_call_method_matches_receiver_registry_id(
                    value, XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COPY_FROM)) {
                *out_kind = XAOT_SLICE_ACCESS_BYTE_COPY;
                return true;
            }
            if (prepare_call_method_matches_receiver_registry_id(
                    value, XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COMMON_PREFIX)) {
                *out_kind = XAOT_SLICE_ACCESS_BYTE_COMMON_PREFIX;
                return true;
            }
            return false;
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
           (plan->rep.flags & XAOT_VALUE_FLAG_SLICE) != 0;
}

static bool prepare_span_elem_plan_for_value(const XaotBundle *bundle, const XiValue *value,
                                             XaotContainerElemPlan *out) {
    XaotContainerPlan plan;
    const XiValue *origin = unwrap_identity_value(value);
    if (!origin || !origin->type || origin->type->kind != XR_KIND_SLICE ||
        !prepare_value_plan_is_span_aggregate(bundle, origin))
        return false;
    if (!xaot_container_plan_for_type(origin->type, &plan) || plan.kind != XAOT_CONTAINER_ARRAY)
        return false;
    if (out)
        *out = plan.elem;
    return true;
}

static bool prepare_span_elem_is_byte(const XaotContainerElemPlan *elem) {
    return elem && xr_type_is_exact_u8(elem->type);
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
    if (!bundle || !origin || depth > 8 || !origin->type || origin->type->kind != XR_KIND_SLICE ||
        !prepare_value_plan_is_span_aggregate(bundle, origin))
        return false;

    switch ((XiOp) origin->op) {
        case XI_SLICE:
        case XI_SLICE_WINDOW:
            if (origin->nargs < 1)
                return false;
            if (origin->args[0] && origin->args[0]->type &&
                origin->args[0]->type->kind == XR_KIND_SLICE)
                return prepare_span_value_writable_proven(bundle, origin->args[0], depth + 1);
            return prepare_span_source_owner_is_writable(origin->args[0]);
        case XI_SLICE_AS_BYTES:
        case XI_SLICE_REINTERPRET:
            return origin->nargs >= 1 &&
                   prepare_span_value_writable_proven(bundle, origin->args[0], depth + 1);
        case XI_CALL_METHOD:
            return origin->aux && strcmp((const char *) origin->aux, "asMutBytes") == 0 &&
                   origin->nargs >= 1 && origin->args[0] && origin->args[0]->type &&
                   xr_type_is_builtin_named_class(origin->args[0]->type, "Buffer") &&
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

static bool prepare_span_value_readonly_proven(const XaotBundle *bundle, const XiValue *value,
                                               uint8_t depth) {
    const XiValue *origin = unwrap_identity_value(value);
    if (!bundle || !origin || depth > 8 || !origin->type || origin->type->kind != XR_KIND_SLICE ||
        !prepare_value_plan_is_span_aggregate(bundle, origin))
        return false;

    switch ((XiOp) origin->op) {
        case XI_SLICE:
        case XI_SLICE_WINDOW:
            if (origin->nargs < 1 || !origin->args[0] || !origin->args[0]->type)
                return false;
            if (origin->args[0]->type->kind == XR_KIND_SLICE)
                return prepare_span_value_readonly_proven(bundle, origin->args[0], depth + 1);
            return xr_type_is_const(origin->args[0]->type) ||
                   origin->args[0]->type->kind == XR_KIND_STRING;
        case XI_SLICE_AS_BYTES:
        case XI_SLICE_REINTERPRET:
            return origin->nargs >= 1 &&
                   prepare_span_value_readonly_proven(bundle, origin->args[0], depth + 1);
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
            return origin->aux && strcmp((const char *) origin->aux, "asBytes") == 0 &&
                   origin->nargs >= 1 && origin->args[0] && origin->args[0]->type &&
                   xr_type_is_builtin_named_class(origin->args[0]->type, "Buffer");
        case XI_PHI: {
            bool saw_arg = false;
            for (uint16_t i = 0; i < origin->nargs; i++) {
                const XiValue *arg = unwrap_identity_value(origin->args[i]);
                if (!arg || arg == origin)
                    continue;
                if (!prepare_span_value_readonly_proven(bundle, arg, depth + 1))
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
    return kind == XAOT_SLICE_ACCESS_INDEX_GET || kind == XAOT_SLICE_ACCESS_INDEX_SET;
}

static bool prepare_span_access_is_byte(uint8_t kind) {
    return kind == XAOT_SLICE_ACCESS_BYTE_LOAD || kind == XAOT_SLICE_ACCESS_BYTE_STORE ||
           kind == XAOT_SLICE_ACCESS_BYTE_FILL || kind == XAOT_SLICE_ACCESS_BYTE_COPY ||
           kind == XAOT_SLICE_ACCESS_BYTE_COMPARE || kind == XAOT_SLICE_ACCESS_BYTE_COMMON_PREFIX ||
           kind == XAOT_SLICE_ACCESS_BYTE_REPEAT || kind == XAOT_SLICE_ACCESS_REINTERPRET;
}

static bool prepare_span_access_is_write(uint8_t kind) {
    return kind == XAOT_SLICE_ACCESS_INDEX_SET || kind == XAOT_SLICE_ACCESS_BYTE_STORE ||
           kind == XAOT_SLICE_ACCESS_BYTE_FILL || kind == XAOT_SLICE_ACCESS_BYTE_COPY ||
           kind == XAOT_SLICE_ACCESS_BYTE_REPEAT || kind == XAOT_SLICE_ACCESS_SLICE_FILL ||
           kind == XAOT_SLICE_ACCESS_SLICE_COPY || kind == XAOT_SLICE_ACCESS_VEC_STORE;
}

static bool prepare_span_int_expr_equal(const XiValue *lhs, const XiValue *rhs) {
    lhs = unwrap_identity_value(lhs);
    rhs = unwrap_identity_value(rhs);
    if (lhs == rhs)
        return lhs != NULL;
    return lhs && rhs && lhs->op == XI_CONST && rhs->op == XI_CONST && lhs->type && rhs->type &&
           lhs->type->kind == XR_KIND_INT && rhs->type->kind == XR_KIND_INT &&
           lhs->aux_int == rhs->aux_int;
}

static bool prepare_span_len_matches_source(const XiValue *value, const XiValue *source) {
    value = unwrap_identity_value(value);
    return value && value->op == XI_LEN && value->nargs == 1 && same_value(value->args[0], source);
}

static bool prepare_span_len_minus_count_matches(const XiValue *value, const XiValue *source,
                                                 const XiValue *count) {
    value = unwrap_identity_value(value);
    if (!value || value->nargs < 2)
        return false;
    if (value->op == XI_SUB && prepare_span_len_matches_source(value->args[0], source) &&
        prepare_span_int_expr_equal(value->args[1], count))
        return true;
    if (value->op == XI_ADD && prepare_span_len_matches_source(value->args[0], source)) {
        const XiValue *rhs = unwrap_identity_value(value->args[1]);
        const XiValue *count_value = unwrap_identity_value(count);
        return rhs && count_value && rhs->op == XI_CONST && count_value->op == XI_CONST &&
               rhs->type && count_value->type && rhs->type->kind == XR_KIND_INT &&
               count_value->type->kind == XR_KIND_INT && count_value->aux_int >= 0 &&
               rhs->aux_int == -count_value->aux_int;
    }
    return false;
}

static uint16_t prepare_span_negated_cmp_op(uint16_t op) {
    switch ((XiOp) op) {
        case XI_EQ:
            return XI_NE;
        case XI_NE:
            return XI_EQ;
        case XI_LT:
            return XI_GE;
        case XI_LE:
            return XI_GT;
        case XI_GT:
            return XI_LE;
        case XI_GE:
            return XI_LT;
        default:
            return XI_OP_COUNT;
    }
}

static bool prepare_span_condition_proves_start_nonnegative(const XiValue *condition,
                                                            const XiValue *start, bool truth) {
    condition = unwrap_identity_value(condition);
    if (!condition)
        return false;
    if (condition->op == XI_NOT && condition->nargs >= 1)
        return prepare_span_condition_proves_start_nonnegative(condition->args[0], start, !truth);
    if (((truth && condition->op == XI_BAND) || (!truth && condition->op == XI_BOR)) &&
        condition->nargs >= 2)
        return prepare_span_condition_proves_start_nonnegative(condition->args[0], start, truth) ||
               prepare_span_condition_proves_start_nonnegative(condition->args[1], start, truth);
    if (condition->nargs < 2)
        return false;

    uint16_t op = truth ? condition->op : prepare_span_negated_cmp_op(condition->op);
    const XiValue *lhs = unwrap_identity_value(condition->args[0]);
    const XiValue *rhs = unwrap_identity_value(condition->args[1]);
    if (op == XI_OP_COUNT || !lhs || !rhs)
        return false;
    if (prepare_span_int_expr_equal(lhs, start) && prepare_array_const_int_value(rhs, 0))
        return op == XI_GE || op == XI_EQ || op == XI_GT;
    if (prepare_array_const_int_value(lhs, 0) && prepare_span_int_expr_equal(rhs, start))
        return op == XI_LE || op == XI_EQ || op == XI_LT;
    return false;
}

static bool prepare_span_condition_proves_window_upper(const XiValue *condition,
                                                       const XiValue *source, const XiValue *start,
                                                       const XiValue *count, bool truth) {
    condition = unwrap_identity_value(condition);
    if (!condition)
        return false;
    if (condition->op == XI_NOT && condition->nargs >= 1)
        return prepare_span_condition_proves_window_upper(condition->args[0], source, start, count,
                                                          !truth);
    if (((truth && condition->op == XI_BAND) || (!truth && condition->op == XI_BOR)) &&
        condition->nargs >= 2)
        return prepare_span_condition_proves_window_upper(condition->args[0], source, start, count,
                                                          truth) ||
               prepare_span_condition_proves_window_upper(condition->args[1], source, start, count,
                                                          truth);
    if (condition->nargs < 2)
        return false;

    uint16_t op = truth ? condition->op : prepare_span_negated_cmp_op(condition->op);
    const XiValue *lhs = unwrap_identity_value(condition->args[0]);
    const XiValue *rhs = unwrap_identity_value(condition->args[1]);
    if (op == XI_OP_COUNT || !lhs || !rhs)
        return false;
    if (prepare_span_int_expr_equal(lhs, start) &&
        prepare_span_len_minus_count_matches(rhs, source, count))
        return op == XI_LE || op == XI_LT || op == XI_EQ;
    if (prepare_span_len_minus_count_matches(lhs, source, count) &&
        prepare_span_int_expr_equal(rhs, start))
        return op == XI_GE || op == XI_GT || op == XI_EQ;

    if (prepare_array_const_int_value(start, 0)) {
        if (prepare_span_len_matches_source(lhs, source) && prepare_span_int_expr_equal(rhs, count))
            return op == XI_GE || op == XI_GT || op == XI_EQ;
        if (prepare_span_int_expr_equal(lhs, count) && prepare_span_len_matches_source(rhs, source))
            return op == XI_LE || op == XI_LT || op == XI_EQ;
    }
    return false;
}

static bool prepare_span_assert_precedes_window(const XiValue *assertion, const XiValue *window) {
    if (!assertion || !window || !assertion->block || !window->block)
        return false;
    if (assertion->block != window->block)
        return xi_dominates(assertion->block, window->block);
    for (uint32_t i = 0; i < window->block->nvalues; i++) {
        const XiValue *value = window->block->values[i];
        if (value == assertion)
            return true;
        if (value == window)
            return false;
    }
    return false;
}

/* A successful XI_ASSERT is a checked capability boundary: code after it may
 * consume the asserted relation without repeating the same bounds branch.
 * The proof stays deliberately narrow (start >= 0 and start <= len-count), is
 * recorded in the AOT plan, and is independently re-derived by the verifier. */
static bool prepare_span_window_assert_bounds_proven(const XiFunc *func, const XiValue *window) {
    if (!func || !window || window->op != XI_SLICE_WINDOW || window->nargs != 3 || !window->block)
        return false;
    const XiValue *source = window->args[0];
    const XiValue *start = window->args[1];
    const XiValue *count = window->args[2];
    bool start_nonnegative = prepare_array_value_known_nonnegative(start, NULL, 0);
    bool count_nonnegative = prepare_array_value_known_nonnegative(count, NULL, 0);
    bool upper_proven = false;

    xi_ensure_dominators((XiFunc *) func);
    for (uint32_t bi = 0; bi < func->nblocks && (!start_nonnegative || !upper_proven); bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues && (!start_nonnegative || !upper_proven); vi++) {
            const XiValue *value = block->values[vi];
            if (!value || value->op != XI_ASSERT || value->nargs < 1 || value->aux_int != 0 ||
                !prepare_span_assert_precedes_window(value, window))
                continue;
            if (!start_nonnegative)
                start_nonnegative =
                    prepare_span_condition_proves_start_nonnegative(value->args[0], start, true);
            if (!upper_proven)
                upper_proven = prepare_span_condition_proves_window_upper(value->args[0], source,
                                                                          start, count, true);
        }
    }
    return start_nonnegative && count_nonnegative && upper_proven;
}

/* A fail-closed branch is the ordinary-control-flow spelling of the same
 * capability established by assert:
 *
 *     if (start < 0) return error
 *     if (count < 0) return error
 *     if (start > len(slice) - count) return error
 *     slice.window(start, count)
 *
 * Only the exact surviving branch, exact SSA operands, and an immutable Slice
 * descriptor are accepted.  This keeps the proof stable across intervening
 * calls while avoiding source-level performance assertions. */
static bool prepare_span_window_dominating_guard_bounds_proven(const XiFunc *func,
                                                               const XiValue *window) {
    if (!func || !window || window->op != XI_SLICE_WINDOW || window->nargs != 3 || !window->block)
        return false;
    const XiValue *source = unwrap_identity_value(window->args[0]);
    const XiValue *start = window->args[1];
    const XiValue *count = window->args[2];
    if (!source || !source->type || source->type->kind != XR_KIND_SLICE)
        return false;

    bool start_nonnegative = prepare_array_value_known_nonnegative(start, NULL, 0) ||
                             xi_value_known_nonnegative_at(func, start, window->block);
    bool count_nonnegative = prepare_array_value_known_nonnegative(count, NULL, 0) ||
                             xi_value_known_nonnegative_at(func, count, window->block);
    bool upper_proven = false;

    xi_ensure_dominators((XiFunc *) func);
    for (const XiBlock *guard = window->block->idom;
         guard && (!start_nonnegative || !count_nonnegative || !upper_proven);
         guard = guard->idom) {
        if (guard->kind != XI_BLOCK_IF || !guard->succs[0] || !guard->succs[1])
            continue;
        bool true_path = xi_dominates(guard->succs[0], window->block);
        bool false_path = xi_dominates(guard->succs[1], window->block);
        if (true_path == false_path)
            continue;
        bool truth = true_path;
        if (!start_nonnegative)
            start_nonnegative =
                prepare_span_condition_proves_start_nonnegative(guard->control, start, truth);
        if (!count_nonnegative)
            count_nonnegative =
                prepare_span_condition_proves_start_nonnegative(guard->control, count, truth);
        if (!upper_proven)
            upper_proven = prepare_span_condition_proves_window_upper(guard->control, source, start,
                                                                      count, truth);
    }
    return start_nonnegative && count_nonnegative && upper_proven;
}

static const XiValue *prepare_span_access_receiver(const XiValue *value, uint8_t kind) {
    if (!value)
        return NULL;
    if (kind == XAOT_SLICE_ACCESS_VEC_STORE)
        return value->nargs >= 2 ? value->args[1] : NULL;
    return value->nargs >= 1 ? value->args[0] : NULL;
}

/* A strict span.window(start, count) is a checked capability for exactly
 * `count` elements.  A vector access at a constant relative offset can
 * therefore reuse the window guard and address the original span directly.
 * The verifier re-derives this plan before CGen; no unchecked source API or
 * backend pattern guess is involved. */
static bool prepare_vector_window_bounds_proven(const XiValue *value, uint8_t kind) {
    const XiValue *receiver = unwrap_identity_value(prepare_span_access_receiver(value, kind));
    const XiValue *offset = NULL;
    const XiValue *count = NULL;
    int64_t offset_value;
    int64_t count_value;
    uint8_t lanes;

    if (!value || !receiver || receiver->op != XI_SLICE_WINDOW || receiver->nargs != 3 ||
        !xi_vec_shape_is_explicit(value->aux_int))
        return false;
    offset = unwrap_identity_value(kind == XAOT_SLICE_ACCESS_VEC_STORE
                                       ? (value->nargs >= 3 ? value->args[2] : NULL)
                                       : (value->nargs >= 2 ? value->args[1] : NULL));
    count = unwrap_identity_value(receiver->args[2]);
    if (!offset || !count || offset->op != XI_CONST || count->op != XI_CONST || !offset->type ||
        offset->type->kind != XR_KIND_INT || !count->type || count->type->kind != XR_KIND_INT)
        return false;
    offset_value = offset->aux_int;
    count_value = count->aux_int;
    lanes = xi_vec_shape_lanes(value->aux_int);
    return lanes != 0 && offset_value >= 0 && count_value >= (int64_t) lanes &&
           offset_value <= count_value - (int64_t) lanes;
}

static uint8_t prepare_span_reason_from_bounds_reason(uint8_t bounds_reason) {
    switch (bounds_reason) {
        case XAOT_BOUNDS_UNPROVEN_LEN_MISMATCH:
            return XAOT_SLICE_UNPROVEN_LENGTH_REL;
        case XAOT_BOUNDS_UNPROVEN_CLOBBER:
            return XAOT_SLICE_UNPROVEN_CLOBBER;
        case XAOT_BOUNDS_UNPROVEN_INDEX_RANGE:
        case XAOT_BOUNDS_UNPROVEN_NO_GUARD:
        default:
            return XAOT_SLICE_UNPROVEN_RANGE;
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
    if (!receiver || receiver->op != XI_SLICE_AS_BYTES || receiver->nargs < 1)
        return false;
    if (!prepare_span_elem_plan_for_value(bundle, receiver->args[0], &source_elem) ||
        !prepare_span_elem_is_pod(&source_elem) ||
        !prepare_span_elem_size(&source_elem, &source_elem_size))
        return false;
    return (source_elem_size % target_elem_size) == 0;
}

static bool prepare_span_reinterpret_byte_len_no_overflow_proven(const XiValue *value) {
    const XiValue *receiver;

    if (!value || value->nargs < 1)
        return false;
    receiver = unwrap_identity_value(value->args[0]);
    return receiver && receiver->op == XI_SLICE_AS_BYTES;
}

XR_FUNC bool xaot_prepare_span_access_plan_for_value(const XaotBundle *bundle, const XiFunc *func,
                                                     const XiValue *value,
                                                     XaotSliceAccessPlan *out) {
    XaotContainerElemPlan recv_elem;
    XaotContainerElemPlan other_elem;
    uint8_t kind = 0;
    uint32_t evidence = 0;
    uint32_t drop = 0;
    uint8_t reason = XAOT_SLICE_UNPROVEN_NONE;
    int64_t endian = XR_ENDIAN_NATIVE;
    const XiValue *receiver;

    if (!out || !prepare_span_access_kind_for_value(value, &kind))
        return false;
    memset(out, 0, sizeof(*out));
    out->func = func;
    out->value = value;
    out->kind = kind;

    if (!value || value->nargs < 1)
        return false;
    receiver = prepare_span_access_receiver(value, kind);
    if (!receiver || !receiver->type || receiver->type->kind != XR_KIND_SLICE) {
        if (prepare_span_access_is_index(kind))
            return false;
        reason = XAOT_SLICE_UNPROVEN_DYNAMIC_RECV;
        goto done;
    }
    if (!prepare_span_elem_plan_for_value(bundle, receiver, &recv_elem)) {
        reason = XAOT_SLICE_UNPROVEN_DYNAMIC_RECV;
        goto done;
    }
    evidence |= XAOT_SLICE_EV_RECV_AGGREGATE;
    if (prepare_span_elem_is_byte(&recv_elem))
        evidence |= XAOT_SLICE_EV_RECV_BYTE_SLICE;
    if (prepare_span_elem_is_pod(&recv_elem))
        evidence |= XAOT_SLICE_EV_RECV_POD;
    if (prepare_span_access_is_write(kind)) {
        if (prepare_span_value_writable_proven(bundle, receiver, 0)) {
            evidence |= XAOT_SLICE_EV_WRITABLE;
            drop |= XAOT_SLICE_DROP_READONLY;
        } else if (prepare_span_value_readonly_proven(bundle, receiver, 0)) {
            evidence |= XAOT_SLICE_EV_READONLY;
        }
    }

    if (prepare_span_access_is_index(kind)) {
        uint8_t bounds_reason = XAOT_BOUNDS_UNPROVEN_NONE;
        uint32_t bounds_evidence =
            xaot_prepare_array_access_bounds_evidence(bundle, func, value, &bounds_reason);
        if (bounds_evidence != 0) {
            evidence |= XAOT_SLICE_EV_RANGE_PROVEN | XAOT_SLICE_EV_NO_CLOBBER;
            drop |= XAOT_SLICE_DROP_BOUNDS;
        } else {
            reason = prepare_span_reason_from_bounds_reason(bounds_reason);
        }
        goto done;
    }

    if (prepare_span_access_is_byte(kind) && (evidence & XAOT_SLICE_EV_RECV_BYTE_SLICE) == 0) {
        reason = XAOT_SLICE_UNPROVEN_NOT_BYTE_SLICE;
        goto done;
    }

    switch ((XaotSliceAccessKind) kind) {
        case XAOT_SLICE_ACCESS_BYTE_LOAD:
            if (prepare_value_is_const_endian(value->nargs >= 3 ? value->args[2] : NULL, &endian)) {
                (void) endian;
                evidence |= XAOT_SLICE_EV_ENDIAN_CONST;
            }
            if (prepare_span_byte_guard_bounds_proven(bundle, func, value)) {
                evidence |= XAOT_SLICE_EV_RANGE_PROVEN | XAOT_SLICE_EV_NO_CLOBBER;
                drop |= XAOT_SLICE_DROP_BOUNDS;
            }
            drop |= XAOT_SLICE_DROP_TYPE | XAOT_SLICE_DROP_HELPER;
            break;
        case XAOT_SLICE_ACCESS_BYTE_STORE:
            if (prepare_value_is_const_endian(value->nargs >= 4 ? value->args[3] : NULL, &endian)) {
                (void) endian;
                evidence |= XAOT_SLICE_EV_ENDIAN_CONST;
            }
            drop |= XAOT_SLICE_DROP_TYPE | XAOT_SLICE_DROP_HELPER;
            break;
        case XAOT_SLICE_ACCESS_BYTE_FILL:
        case XAOT_SLICE_ACCESS_BYTE_REPEAT:
            drop |= XAOT_SLICE_DROP_TYPE | XAOT_SLICE_DROP_HELPER;
            break;
        case XAOT_SLICE_ACCESS_BYTE_COPY:
        case XAOT_SLICE_ACCESS_BYTE_COMPARE:
        case XAOT_SLICE_ACCESS_BYTE_COMMON_PREFIX:
            if (value->nargs < 2 ||
                !prepare_span_elem_plan_for_value(bundle, value->args[1], &other_elem) ||
                !prepare_span_elem_is_byte(&other_elem)) {
                reason = XAOT_SLICE_UNPROVEN_DYNAMIC_BOUNDARY;
                break;
            }
            evidence |= XAOT_SLICE_EV_ELEM_MATCH;
            drop |= XAOT_SLICE_DROP_TYPE | XAOT_SLICE_DROP_HELPER;
            break;
        case XAOT_SLICE_ACCESS_SLICE_AS_BYTES:
        case XAOT_SLICE_ACCESS_SLICE_FILL:
            if ((evidence & XAOT_SLICE_EV_RECV_POD) == 0) {
                reason = XAOT_SLICE_UNPROVEN_NOT_POD;
                break;
            }
            if (kind == XAOT_SLICE_ACCESS_SLICE_AS_BYTES) {
                uint8_t recv_elem_size = 0;
                evidence |= XAOT_SLICE_EV_LENGTH_REL_PROVEN;
                if (prepare_span_elem_size(&recv_elem, &recv_elem_size) && recv_elem_size == 1) {
                    evidence |= XAOT_SLICE_EV_BYTE_LEN_NO_OVERFLOW;
                    drop |= XAOT_SLICE_DROP_OVERFLOW;
                }
            }
            drop |= XAOT_SLICE_DROP_TYPE | XAOT_SLICE_DROP_POD | XAOT_SLICE_DROP_HELPER;
            break;
        case XAOT_SLICE_ACCESS_SLICE_COPY:
        case XAOT_SLICE_ACCESS_SLICE_COMPARE:
            if ((evidence & XAOT_SLICE_EV_RECV_POD) == 0) {
                reason = XAOT_SLICE_UNPROVEN_NOT_POD;
                break;
            }
            if (value->nargs < 2 ||
                !prepare_span_elem_plan_for_value(bundle, value->args[1], &other_elem) ||
                !prepare_span_elem_is_pod(&other_elem)) {
                reason = XAOT_SLICE_UNPROVEN_DYNAMIC_BOUNDARY;
                break;
            }
            if (!prepare_span_elem_matches(&recv_elem, &other_elem)) {
                reason = XAOT_SLICE_UNPROVEN_ELEM_MISMATCH;
                break;
            }
            evidence |= XAOT_SLICE_EV_ELEM_MATCH;
            drop |= XAOT_SLICE_DROP_TYPE | XAOT_SLICE_DROP_POD | XAOT_SLICE_DROP_HELPER;
            break;
        case XAOT_SLICE_ACCESS_REINTERPRET: {
            uint8_t target_elem_size = (uint8_t) ((value->aux_int >> 8) & 0xff);
            if (target_elem_size == 1 ||
                prepare_span_reinterpret_length_relation_proven(bundle, value, target_elem_size))
                evidence |= XAOT_SLICE_EV_LENGTH_REL_PROVEN;
            if (prepare_span_reinterpret_byte_len_no_overflow_proven(value)) {
                evidence |= XAOT_SLICE_EV_BYTE_LEN_NO_OVERFLOW;
                drop |= XAOT_SLICE_DROP_OVERFLOW;
            }
            drop |= XAOT_SLICE_DROP_TYPE | XAOT_SLICE_DROP_POD | XAOT_SLICE_DROP_HELPER;
            break;
        }
        case XAOT_SLICE_ACCESS_VEC_LOAD:
        case XAOT_SLICE_ACCESS_VEC_STORE:
            if (prepare_vector_window_bounds_proven(value, kind)) {
                evidence |= XAOT_SLICE_EV_RANGE_PROVEN | XAOT_SLICE_EV_NO_CLOBBER;
                drop |= XAOT_SLICE_DROP_BOUNDS;
            } else {
                reason = XAOT_SLICE_UNPROVEN_RANGE;
            }
            break;
        case XAOT_SLICE_ACCESS_WINDOW:
            if (prepare_span_window_assert_bounds_proven(func, value)) {
                evidence |= XAOT_SLICE_EV_RANGE_PROVEN | XAOT_SLICE_EV_NO_CLOBBER |
                            XAOT_SLICE_EV_ASSERT_GUARD;
                drop |= XAOT_SLICE_DROP_BOUNDS;
            } else if (prepare_span_window_dominating_guard_bounds_proven(func, value)) {
                evidence |= XAOT_SLICE_EV_RANGE_PROVEN | XAOT_SLICE_EV_NO_CLOBBER |
                            XAOT_SLICE_EV_DOMINATING_GUARD;
                drop |= XAOT_SLICE_DROP_BOUNDS;
            } else {
                reason = XAOT_SLICE_UNPROVEN_RANGE;
            }
            break;
        default:
            reason = XAOT_SLICE_UNPROVEN_DYNAMIC_BOUNDARY;
            break;
    }

done:
    if (drop != 0)
        reason = XAOT_SLICE_UNPROVEN_NONE;
    else if (reason == XAOT_SLICE_UNPROVEN_NONE)
        reason = XAOT_SLICE_UNPROVEN_DYNAMIC_BOUNDARY;
    out->evidence = evidence;
    out->eliminated_checks = drop;
    out->unproven_reason = reason;
    return true;
}

/* Prove every index access in the function and record the result —proven
 * ones with evidence, unproven ones with a reason —in the bounds plan.
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
            XaotSliceAccessPlan plan;
            if (!xaot_prepare_span_access_plan_for_value(bundle, func, value, &plan))
                continue;
            if (!xaot_bundle_add_span_access_plan(bundle, func, value, body, plan.kind,
                                                  plan.evidence, plan.eliminated_checks,
                                                  plan.unproven_reason)) {
                bundle->error_msg = "failed to allocate AOT Slice access plan";
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
 * pointer —calls, stores, captures, phi participation —is rejected.
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
                    case XI_LEN:
                        if (argi != 0)
                            return false;
                        break;
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (argi != 0)
                            return false;
                        break;
                    case XI_BOX:
                    case XI_UNBOX:
                    case XI_COPY:
                    case XI_SOURCE_MOVE:
                    case XI_OWNER_FORWARD:
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

static bool xaot_closure_target_can_be_direct_symbol(const XiFunc *target) {
    const XiCoroPlan *plan = target ? target->coro_plan : NULL;
    return target && target->ncaptures == 0 && plan && xi_coro_plan_is_current(target, plan) &&
           !plan->is_coroutine;
}

static bool xaot_array_hof_callback_is_exact(const XaotBundle *bundle, const XiFunc *owner,
                                             const XiValue *call_value, const XiValue *callback,
                                             const XiFunc *callback_target) {
    const XrTargetPlan *target =
        bundle && owner ? xaot_bundle_target_plan_for_func(bundle, owner) : NULL;
    const XrSemanticPlan *semantic = target ? xr_target_plan_semantic_plan(target) : NULL;
    uint32_t call_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t call_semantic_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t callback_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t callback_semantic_value = XR_SEMANTIC_INDEX_NONE;
    char error[192] = {0};
    if (!bundle || !owner || !call_value || !callback || !callback_target || !target || !semantic ||
        !xr_target_plan_is_verified(target) ||
        (xr_target_plan_completed_family_mask(target) &
         XR_TARGET_FAMILY_ARRAY_HOF_RESULT_STORAGE) == 0 ||
        !xr_aot_scalar_semantic_value_id(target, owner, call_value, &call_function,
                                         &call_semantic_value, error, sizeof(error)) ||
        !xr_aot_scalar_semantic_value_id(target, owner, callback, &callback_function,
                                         &callback_semantic_value, error, sizeof(error)) ||
        call_function != callback_function || callback_target->semantic_plan != semantic ||
        callback_target->semantic_plan_function_index == XR_SEMANTIC_INDEX_NONE)
        return false;

    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->function != call_function ||
            candidate->result_value != call_semantic_value)
            continue;
        if (operation)
            return false;
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
            default:
                break;
        }
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint16_t expected_operands = expected_target_kind == XR_TARGET_ARRAY_HOF_REDUCE ? 3u : 2u;
    if (!operation || expected_target_kind == XR_TARGET_ARRAY_HOF_NONE ||
        call_value->array_hof_kind != expected_live_kind ||
        operation->callable_function != callback_target->semantic_plan_function_index ||
        operation->operand_count != expected_operands || !operands ||
        operation->operand_begin > operand_count ||
        expected_operands > operand_count - operation->operand_begin ||
        operands[operation->operand_begin + 1u].value != callback_semantic_value)
        return false;

    const XrTargetCallRecord *call = NULL;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    return call && call->caller_function == call_function &&
           call->callee_function == operation->callable_function &&
           call->result_value == call_semantic_value && call->argument_count == expected_operands &&
           call->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_HOF &&
           call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_HOF &&
           call->array_hof_kind == expected_target_kind &&
           call->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE &&
           call->array_result_element_storage != XR_TARGET_ARRAY_STORAGE_NONE;
}

static bool xaot_closure_value_uses_direct_symbol(const XaotBundle *bundle, const XiFunc *owner,
                                                  const XiValue *value, const XiFunc *target,
                                                  int depth) {
    if (!owner || !value || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != value)
                    continue;
                switch ((XiOp) user->op) {
                    case XI_CALL:
                        if (ai != 0)
                            return false;
                        break;
                    case XI_CALL_METHOD:
                        if (ai != 1 ||
                            !xaot_array_hof_callback_is_exact(bundle, owner, user, value, target))
                            return false;
                        break;
                    case XI_BOX:
                    case XI_UNBOX:
                    case XI_COPY:
                    case XI_SOURCE_MOVE:
                    case XI_OWNER_FORWARD:
                        if (ai != 0 || !xaot_closure_value_uses_direct_symbol(bundle, owner, user,
                                                                              target, depth + 1))
                            return false;
                        break;
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (ai != 0)
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

XR_FUNC bool xaot_prepare_closure_plan_for_value(const XaotBundle *bundle, const XiFunc *func,
                                                 const XiValue *value, XaotClosurePlan *out) {
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
    if (xaot_closure_target_can_be_direct_symbol(target)) {
        if (xaot_closure_value_uses_direct_symbol(bundle, func, value, target, 0)) {
            out->representation = XAOT_CLOSURE_DIRECT_SYMBOL;
            out->evidence &= ~XAOT_CLOSURE_EV_NOESCAPE_STACK;
            out->evidence |= XAOT_CLOSURE_EV_DIRECT_SYMBOL;
        }
    }
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
            if (!xaot_prepare_closure_plan_for_value(bundle, func, value, &derived))
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

static const XiValue *prepare_transfer_source_move(const XiValue *value) {
    const XiValue *v = value;
    while (v && v->nargs >= 1 &&
           (v->op == XI_BOX || v->op == XI_UNBOX || xi_copy_is_identity_alias(v) ||
            v->op == XI_OWNER_FORWARD))
        v = v->args[0];
    return v && v->op == XI_SOURCE_MOVE ? v : NULL;
}

static bool prepare_transfer_type_is_sync_handle(const XrType *type) {
    return xi_type_is_channel(type) || xi_type_is_task(type) || xi_type_is_thread(type) ||
           xi_type_is_named_instance(type, "Atomic") ||
           xi_type_is_named_instance(type, "WorkQueue") ||
           xi_type_is_named_instance(type, "ResultGroup") ||
           xi_type_is_named_instance(type, "CountdownLatch") ||
           xi_type_is_named_instance(type, "Semaphore") ||
           xi_type_is_named_instance(type, "EventCount");
}

/* Builtin native handles whose only constructor allocates on the shared system
 * heap with an atomic refcount (xr_sysheap_alloc_shared), exactly like Channel
 * and Atomic: NetConn and NetListener. There is no user-constructible or
 * execution-local path to a value of these types, so the static type alone
 * proves the value is always cross-execution share-safe. Requiring the builtin
 * class (class_ref == NULL) rejects a user class that reuses the name. */
static bool prepare_transfer_type_is_shared_native_handle(const XrType *type) {
    return xr_type_is_builtin_named_class(type, "NetConn") ||
           xr_type_is_builtin_named_class(type, "NetListener");
}

static bool prepare_transfer_type_is_inline(const XrType *type) {
    if (!type)
        return false;
    switch (type->kind) {
        case XR_KIND_NULL:
        case XR_KIND_BOOL:
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_RUNE:
        case XR_KIND_ENUM:
            return true;
        case XR_KIND_UNION:
            for (uint8_t i = 0; i < type->union_type.member_count; i++) {
                if (!prepare_transfer_type_is_inline(type->union_type.members[i]))
                    return false;
            }
            return true;
        default:
            return false;
    }
}

static uint8_t prepare_transfer_action(const XiValue *value, uint8_t mode) {
    const XiValue *origin = unwrap_identity_value(value);
    const XrType *type = value && value->type ? value->type : (origin ? origin->type : NULL);
    switch ((XrTransferMode) mode) {
        case XR_TRANSFER_SHARE:
            if (type && prepare_transfer_type_is_sync_handle(type))
                return XR_TRANSFER_SYNC_SHARE;
            if (type && prepare_transfer_type_is_shared_native_handle(type))
                return XR_TRANSFER_SYNC_SHARE;
            if (type && prepare_transfer_type_is_inline(type))
                return XR_TRANSFER_INLINE_COPY;
            if (type && xr_type_is_const((XrType *) type))
                return XR_TRANSFER_CONST_SHARE;
            if (origin && origin->op == XI_CONST && type && type->kind == XR_KIND_STRING)
                return XR_TRANSFER_CONST_SHARE;
            return XR_TRANSFER_REJECT;
        case XR_TRANSFER_COPY:
            return XR_TRANSFER_EXPLICIT_COPY;
        case XR_TRANSFER_MOVE:
            return prepare_transfer_source_move(value) ? XR_TRANSFER_MOVE_UNIQUE
                                                       : XR_TRANSFER_REJECT;
        default:
            return XR_TRANSFER_REJECT;
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
    const XiValue *move = NULL;

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
    out->transfer_plan_id = (site->id << 8u) ^ ((uint32_t) transfer_index + 1u);
    out->target_domain = XR_STORAGE_TRANSFERABLE;
    out->cost_class = XAOT_TRANSFER_COST_REJECTED;

    if (!value) {
        out->action = XR_TRANSFER_REJECT;
        out->evidence = evidence;
        out->unproven_reason = XAOT_TRANSFER_UNPROVEN_NO_VALUE;
        return true;
    }
    evidence |= XAOT_TRANSFER_EV_VALUE;

    if (mode > XR_TRANSFER_MOVE) {
        out->action = XR_TRANSFER_REJECT;
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
    action = prepare_transfer_action(value, mode);
    if (action == XR_TRANSFER_MOVE_UNIQUE) {
        move = prepare_transfer_source_move(value);
        if (!move || move->move_evidence_id == 0 || move->move_storage_plan_id == 0) {
            action = XR_TRANSFER_REJECT;
            out->unproven_reason = XAOT_TRANSFER_UNPROVEN_NO_MOVE_PROOF;
        } else if (move->move_source_domain != XR_STORAGE_TRANSFERABLE) {
            action = XR_TRANSFER_REJECT;
            out->unproven_reason = XAOT_TRANSFER_UNPROVEN_DOMAIN;
        } else {
            out->move_proof_id = move->move_evidence_id;
            out->storage_plan_id = move->move_storage_plan_id;
            out->source_domain = move->move_source_domain;
            out->source_capability = move->move_source_capability;
            out->target_capability = move->move_target_capability;
            out->drop_action = XAOT_TRANSFER_DROP_HANDOFF;
            out->cost_class = XAOT_TRANSFER_COST_O1;
            evidence |= XAOT_TRANSFER_EV_STORAGE_PLAN | XAOT_TRANSFER_EV_MOVE_PROOF |
                        XAOT_TRANSFER_EV_DOMAIN | XAOT_TRANSFER_EV_CAPABILITY;
        }
    } else if (action == XR_TRANSFER_EXPLICIT_COPY) {
        out->source_domain = XR_STORAGE_EXEC_LOCAL;
        out->drop_action = XAOT_TRANSFER_DROP_CLONE;
        out->cost_class = needs_boundary_clone ? XAOT_TRANSFER_COST_ON : XAOT_TRANSFER_COST_O1;
        evidence |= XAOT_TRANSFER_EV_DOMAIN;
    } else if (action == XR_TRANSFER_CONST_SHARE) {
        out->source_domain = XR_STORAGE_CONST_SHARED;
        out->target_domain = XR_STORAGE_CONST_SHARED;
        out->drop_action = XAOT_TRANSFER_DROP_RETAIN;
        out->cost_class = XAOT_TRANSFER_COST_O1;
        evidence |= XAOT_TRANSFER_EV_DOMAIN | XAOT_TRANSFER_EV_CAPABILITY;
    } else if (action == XR_TRANSFER_SYNC_SHARE) {
        out->source_domain = XR_STORAGE_SYNC_SHARED;
        out->target_domain = XR_STORAGE_SYNC_SHARED;
        out->drop_action = XAOT_TRANSFER_DROP_RETAIN;
        out->cost_class = XAOT_TRANSFER_COST_O1;
        evidence |= XAOT_TRANSFER_EV_DOMAIN | XAOT_TRANSFER_EV_CAPABILITY;
    } else if (action == XR_TRANSFER_INLINE_COPY) {
        out->source_domain = XR_STORAGE_EXEC_LOCAL;
        out->drop_action = XAOT_TRANSFER_DROP_NONE;
        out->cost_class = XAOT_TRANSFER_COST_O1;
        evidence |= XAOT_TRANSFER_EV_DOMAIN;
    } else if (out->unproven_reason == XAOT_TRANSFER_UNPROVEN_NONE) {
        out->unproven_reason = mode == XR_TRANSFER_MOVE ? XAOT_TRANSFER_UNPROVEN_NO_MOVE_PROOF
                                                        : XAOT_TRANSFER_UNPROVEN_CAPABILITY;
    }
    out->action = action;
    out->evidence = evidence;
    if (action != XR_TRANSFER_REJECT)
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
                    if (derived.action == XR_TRANSFER_REJECT) {
                        bundle->error_msg = "cross-execution transfer lacks a verified plan";
                        return false;
                    }
                    if (!xaot_bundle_add_transfer_plan(bundle, &derived)) {
                        bundle->error_msg = "failed to allocate AOT transfer plan";
                        return false;
                    }
                }
            } else if (prepare_transfer_channel_site_kind(site) != 0) {
                XaotTransferPlan derived;
                if (!xaot_prepare_transfer_plan_for_site(func, site, 0, &derived))
                    continue;
                if (derived.action == XR_TRANSFER_REJECT) {
                    bundle->error_msg = "cross-execution transfer lacks a verified plan";
                    return false;
                }
                if (!xaot_bundle_add_transfer_plan(bundle, &derived)) {
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
                    case XI_LEN:
                        if (argi != 0)
                            return false;
                        break;
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (argi != 0)
                            return false;
                        break;
                    case XI_BOX:
                    case XI_UNBOX:
                    case XI_COPY:
                    case XI_SOURCE_MOVE:
                    case XI_OWNER_FORWARD:
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
            /* ARC bookkeeping mutates only reference counts.  It cannot
             * replace the class field or mutate the typed-array payload, so
             * it does not invalidate a cached data pointer. */
            if ((cur->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM)) != 0 &&
                cur->op != XI_RETAIN && cur->op != XI_RELEASE)
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
    } else if (array_hof_result_is_exact(bundle, target) &&
               !array_value_has_uncacheable_use(target)) {
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

static uint32_t prepare_enum_metadata_use_bit(uint8_t field) {
    switch ((XaEnumMetaField) field) {
        case XA_ENUM_META_VARIANTS:
        case XA_ENUM_META_LENGTH:
            return XAOT_ENUM_USE_COUNT;
        case XA_ENUM_META_ORDINAL:
            return XAOT_ENUM_USE_ORDINAL;
        case XA_ENUM_META_NAME:
            return XAOT_ENUM_USE_VARIANT_NAME;
        case XA_ENUM_META_PAYLOAD_COUNT:
        case XA_ENUM_META_IS_UNIT:
        case XA_ENUM_META_PAYLOADS:
            return XAOT_ENUM_USE_PAYLOAD_COUNT;
        case XA_ENUM_META_PAYLOAD_INDEX:
            return XAOT_ENUM_USE_PAYLOAD_INDEX;
        case XA_ENUM_META_PAYLOAD_NAME:
            return XAOT_ENUM_USE_PAYLOAD_NAME;
        case XA_ENUM_META_PAYLOAD_TYPE:
            return XAOT_ENUM_USE_PAYLOAD_TYPE;
        default:
            return 0;
    }
}

static bool prepare_enum_domain_value(XaotBundle *bundle, const XiValue *value) {
    const XrType *owner;
    XaotEnumPlan *plan;

    if (!bundle)
        return false;
    if (!value)
        return true;
    owner = value->enum_metadata_owner;
    if (!owner && xr_type_is_enum_metadata(value->type))
        owner = xr_type_enum_metadata_owner(value->type);
    if (!owner)
        return true;
    if (owner->kind != XR_KIND_ENUM || !owner->enum_type.layout) {
        bundle->error_msg = "enum-domain Xi value has no concrete enum owner";
        return false;
    }
    if (!xaot_bundle_prepare_enum_plan_for_type(bundle, owner))
        return false;
    plan = (XaotEnumPlan *) xaot_bundle_find_enum_plan_for_type(bundle, owner);
    if (!plan && value->enum_metadata_kind == XR_ENUM_METADATA_NONE) {
        /* Prelude enums such as Endian use the existing static-enum lowering
         * and are not module slot enum domains.  Their ordinary value
         * `.name`/`.ordinal` loads may carry best-effort metadata provenance,
         * but they must not be mistaken for a task-210 descriptor domain. */
        return true;
    }
    if (!plan) {
        bundle->error_msg = "enum-domain Xi value has no AOT enum plan";
        return false;
    }
    if (value->aux_kind == XI_AUX_KIND_ENUM_CASE) {
        plan->value_iteration_reachable = true;
        plan->descriptor_use_bits |= XAOT_ENUM_USE_COUNT;
    }
    if (value->op == XI_ENUM_VARIANT_AT) {
        plan->variant_iteration_reachable = true;
        plan->descriptor_use_bits |= XAOT_ENUM_USE_COUNT;
    }
    plan->descriptor_use_bits |= prepare_enum_metadata_use_bit(value->enum_metadata_field);
    if (xr_type_is_enum_metadata(value->type) && value->escape != XI_ESC_NONE &&
        plan->descriptor_escape_kind < XAOT_ENUM_DESCRIPTOR_TYPED_VALUE)
        plan->descriptor_escape_kind = XAOT_ENUM_DESCRIPTOR_TYPED_VALUE;
    if (value->op == XI_ENUM_DESCRIPTOR_BOX)
        plan->descriptor_escape_kind = XAOT_ENUM_DESCRIPTOR_ERASED_BOX;
    return true;
}

static bool prepare_func_enum_domain_plan(XaotBundle *bundle, const XiFunc *func) {
    if (!bundle || !func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks ? func->blocks[bi] : NULL;
        if (!block)
            continue;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (!prepare_enum_domain_value(bundle, &phi->value))
                return false;
        }
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            if (!prepare_enum_domain_value(bundle, block->values ? block->values[vi] : NULL))
                return false;
        }
    }
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
            const XrTargetValueRepRecord *binding = NULL;
            if (!prepare_target_value_binding(bundle, func, &phi->value, &binding))
                return false;
            if (binding) {
                const XrTargetPlan *target_plan = xaot_bundle_target_plan_for_func(bundle, func);
                XaotValueRep target_rep;
                if (!prepare_target_machine_value_rep(target_plan, binding, &phi->value,
                                                      &target_rep)) {
                    bundle->error_msg = "AOT TargetPlan binding has no supported C value rep";
                    return false;
                }
                record_value_stats(&bundle->stats, target_rep.kind, false, false);
                bool type_plans = prepare_type_plans_for_type(bundle, phi->value.type, 0);
                xaot_value_rep_dispose(&target_rep);
                if (!type_plans)
                    return false;
                continue;
            }
            XaotValuePlan *vp = xaot_bundle_add_value_plan(bundle, func, &phi->value);
            if (!vp) {
                bundle->error_msg = "failed to allocate AOT value plan";
                return false;
            }
            apply_native_class_ptr_value_plan(bundle, vp);
            apply_unit_enum_ordinal_value_plan(bundle, vp);
            const bool enum_ordinal = xaot_value_plan_is_exact_enum_ordinal_family(bundle, vp);
            const bool rep_adapter = xaot_value_plan_is_exact_rep_adapter(bundle, vp);
            if ((vp->value->backend_origin != XI_BACKEND_VALUE_NONE) != rep_adapter) {
                bundle->error_msg = "AOT prepare refused an inexact representation adapter row";
                return false;
            }
            if ((vp->rep.kind == XAOT_VALUE_SCALAR || vp->rep.kind == XAOT_VALUE_VOID) &&
                !enum_ordinal && !rep_adapter) {
                bundle->error_msg = "AOT prepare refused an unbound legacy scalar value row";
                return false;
            }
            record_value_stats(&bundle->stats, vp->rep.kind, enum_ordinal, rep_adapter);
            if (!prepare_type_plans_for_type(bundle, phi->value.type, 0))
                return false;
        }
        for (vi = 0; vi < blk->nvalues; vi++) {
            const XrTargetValueRepRecord *binding = NULL;
            if (!prepare_target_value_binding(bundle, func, blk->values[vi], &binding))
                return false;
            if (binding) {
                const XrTargetPlan *target_plan = xaot_bundle_target_plan_for_func(bundle, func);
                XaotValueRep target_rep;
                if (!prepare_target_machine_value_rep(target_plan, binding, blk->values[vi],
                                                      &target_rep)) {
                    bundle->error_msg = "AOT TargetPlan binding has no supported C value rep";
                    return false;
                }
                record_value_stats(&bundle->stats, target_rep.kind, false, false);
                bool type_plans = prepare_type_plans_for_type(bundle, blk->values[vi]->type, 0);
                xaot_value_rep_dispose(&target_rep);
                if (!type_plans)
                    return false;
                continue;
            }
            XaotValuePlan *vp = xaot_bundle_add_value_plan(bundle, func, blk->values[vi]);
            if (!vp) {
                bundle->error_msg = "failed to allocate AOT value plan";
                return false;
            }
            /* A local value-struct constructor is concrete even when it does
             * not reach a native function boundary.  Seed its POD aggregate
             * representation here so a semantic XI_COPY can become an
             * independent C aggregate value and subsequent AGG_SET operations
             * stay on the stack.  Previously only returns/arguments seeded
             * this representation, so `var p = makePair(); p.x += 1` boxed p
             * and routed the copy through xrt_value_clone_for_coro. */
            if (blk->values[vi]->op == XI_AGG_NEW ||
                (blk->values[vi]->op == XI_JSON_DECODE &&
                 (blk->values[vi]->lowering_flags & XI_LOWERING_FLAG_JSON_TYPED_PARSE) != 0)) {
                XaotValueRep aggregate_rep =
                    xaot_abi_native_value_rep(bundle, func, blk->values[vi]);
                if (aggregate_rep.kind == XAOT_VALUE_AGGREGATE &&
                    (aggregate_rep.flags & XAOT_VALUE_FLAG_STRUCT) != 0)
                    prepare_value_plan_set_rep(vp, aggregate_rep);
            }
            if (blk->values[vi]->xa_intrinsic_id != 0 && blk->values[vi]->op >= XI_VEC_LOAD &&
                blk->values[vi]->op <= XI_VEC_REDUCE_ADD) {
                XaotValueRep intrinsic_rep =
                    xaot_abi_native_value_rep(bundle, func, blk->values[vi]);
                if (intrinsic_rep.kind != XAOT_VALUE_TAGGED)
                    prepare_value_plan_set_rep(vp, intrinsic_rep);
            }
            apply_native_class_ptr_value_plan(bundle, vp);
            apply_unit_enum_ordinal_value_plan(bundle, vp);
            const bool enum_ordinal = xaot_value_plan_is_exact_enum_ordinal_family(bundle, vp);
            const bool rep_adapter = xaot_value_plan_is_exact_rep_adapter(bundle, vp);
            if ((vp->value->backend_origin != XI_BACKEND_VALUE_NONE) != rep_adapter) {
                bundle->error_msg = "AOT prepare refused an inexact representation adapter row";
                return false;
            }
            if ((vp->rep.kind == XAOT_VALUE_SCALAR || vp->rep.kind == XAOT_VALUE_VOID) &&
                !enum_ordinal && !rep_adapter) {
                bundle->error_msg = "AOT prepare refused an unbound legacy scalar value row";
                return false;
            }
            record_value_stats(&bundle->stats, vp->rep.kind, enum_ordinal, rep_adapter);
            if (!prepare_type_plans_for_type(bundle, blk->values[vi]->type, 0))
                return false;
        }
    }
    return true;
}

static unsigned prepare_vector_lane_bytes(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_U8:
            return 1;
        case XR_NATIVE_U32:
            return 4;
        case XR_NATIVE_U64:
            return 8;
        default:
            return 0;
    }
}

static const char *prepare_vector_native_c_type(uint32_t features, uint8_t native_type,
                                                uint8_t lanes, bool scalable) {
    unsigned width = prepare_vector_lane_bytes(native_type) * (unsigned) lanes;
    if (scalable && (features & XAOT_SIMD_FEATURE_SVE) != 0) {
        if (native_type == XR_NATIVE_U8)
            return "svuint8_t";
        if (native_type == XR_NATIVE_U32)
            return "svuint32_t";
        if (native_type == XR_NATIVE_U64)
            return "svuint64_t";
    }
    if (width == 16 && (features & (XAOT_SIMD_FEATURE_VSX | XAOT_SIMD_FEATURE_LSX)) != 0) {
        if (native_type == XR_NATIVE_U8)
            return "xr_v16u8";
        if (native_type == XR_NATIVE_U32)
            return "xr_v4u32";
        if (native_type == XR_NATIVE_U64)
            return "xr_v2u64";
    }
    if (width == 16 && (features & XAOT_SIMD_FEATURE_NEON) != 0) {
        if (native_type == XR_NATIVE_U8)
            return "uint8x16_t";
        if (native_type == XR_NATIVE_U32)
            return "uint32x4_t";
        if (native_type == XR_NATIVE_U64)
            return "uint64x2_t";
    }
    if (width == 16 && (features & XAOT_SIMD_FEATURE_SSE2) != 0)
        return "__m128i";
    if (width == 32 && (features & XAOT_SIMD_FEATURE_AVX2) != 0)
        return "__m256i";
    if (width == 64 && (features & XAOT_SIMD_FEATURE_AVX512) != 0)
        return "__m512i";
    return NULL;
}

static bool prepare_vector_native_op_supported(uint32_t features, const XiValue *value) {
    if (!value || !xi_vec_shape_is_explicit(value->aux_int))
        return false;
    uint8_t lanes = xi_vec_shape_lanes(value->aux_int);
    uint8_t native_type = xi_vec_shape_native_type(value->aux_int);
    unsigned width = prepare_vector_lane_bytes(native_type) * (unsigned) lanes;
    bool neon = (features & XAOT_SIMD_FEATURE_NEON) != 0;
    bool x86 = (features & XAOT_SIMD_FEATURE_SSE2) != 0;
    bool avx2 = (features & XAOT_SIMD_FEATURE_AVX2) != 0;
    bool avx512 = (features & XAOT_SIMD_FEATURE_AVX512) != 0;
    bool vsx = (features & XAOT_SIMD_FEATURE_VSX) != 0;
    bool lsx = (features & XAOT_SIMD_FEATURE_LSX) != 0;
    bool sve = (features & XAOT_SIMD_FEATURE_SVE) != 0;
    bool scalable = xi_vec_shape_is_scalable(value->aux_int);
    if (!prepare_vector_native_c_type(features, native_type, lanes, scalable))
        return false;
    if (scalable) {
        if (!sve)
            return false;
        switch ((XiOp) value->op) {
            case XI_VEC_LOAD:
            case XI_VEC_STORE:
            case XI_VEC_SPLAT:
            case XI_VEC_ADD:
            case XI_VEC_SUB:
            case XI_VEC_MUL:
            case XI_VEC_BIT_AND:
            case XI_VEC_BIT_OR:
            case XI_VEC_BIT_XOR:
            case XI_VEC_BIT_NOT:
            case XI_VEC_SHL:
            case XI_VEC_SHR:
            case XI_VEC_REINTERPRET:
                return true;
            case XI_VEC_SHUFFLE:
                return (value->aux_int & XI_VEC_SHAPE_SWAP_ADJACENT) != 0;
            case XI_VEC_WIDEN_MUL:
                return native_type == XR_NATIVE_U64 && lanes == 8;
            default:
                return false;
        }
    }
    switch ((XiOp) value->op) {
        case XI_VEC_LOAD:
        case XI_VEC_STORE:
        case XI_VEC_SPLAT:
        case XI_VEC_EXTRACT:
        case XI_VEC_REPLACE:
        case XI_VEC_BIT_AND:
        case XI_VEC_BIT_OR:
        case XI_VEC_BIT_XOR:
        case XI_VEC_BIT_NOT:
        case XI_VEC_REINTERPRET:
        case XI_VEC_REDUCE_ADD:
            return true;
        case XI_VEC_ADD:
        case XI_VEC_SUB:
            return native_type != XR_NATIVE_U8 || neon || x86 || vsx || lsx;
        case XI_VEC_MUL:
            if (native_type == XR_NATIVE_U64)
                return lsx;
            return neon || vsx || lsx || (native_type == XR_NATIVE_U32 && avx2);
        case XI_VEC_SHL:
        case XI_VEC_SHR:
            return native_type == XR_NATIVE_U32 || native_type == XR_NATIVE_U64;
        case XI_VEC_SHUFFLE:
            if ((value->aux_int & XI_VEC_SHAPE_UNZIP) != 0)
                return (neon || x86 || vsx || lsx) && width == 16 && native_type == XR_NATIVE_U32;
            if (vsx || lsx)
                return width == 16;
            if (neon)
                return width == 16;
            if (!x86)
                return false;
            if (width == 64)
                return avx512 && (value->aux_int & XI_VEC_SHAPE_SWAP_ADJACENT) != 0 &&
                       (native_type == XR_NATIVE_U32 || native_type == XR_NATIVE_U64);
            return native_type != XR_NATIVE_U8 || avx2;
        case XI_VEC_WIDEN_MUL:
            if ((value->aux_int & XI_VEC_SHAPE_CONTIGUOUS_HALF) != 0)
                return (neon || x86 || vsx || lsx) && native_type == XR_NATIVE_U64 && lanes == 2;
            return native_type == XR_NATIVE_U64 &&
                   (lanes == 2 || (lanes == 4 && avx2) || (lanes == 8 && avx512));
        default:
            return false;
    }
}

static bool prepare_vector_op_has_vector_result(const XiValue *value) {
    if (!value)
        return false;
    switch ((XiOp) value->op) {
        case XI_VEC_LOAD:
        case XI_VEC_SPLAT:
        case XI_VEC_REPLACE:
        case XI_VEC_ADD:
        case XI_VEC_SUB:
        case XI_VEC_MUL:
        case XI_VEC_BIT_AND:
        case XI_VEC_BIT_OR:
        case XI_VEC_BIT_XOR:
        case XI_VEC_BIT_NOT:
        case XI_VEC_SHL:
        case XI_VEC_SHR:
        case XI_VEC_REINTERPRET:
        case XI_VEC_SHUFFLE:
        case XI_VEC_WIDEN_MUL:
            return true;
        default:
            return false;
    }
}

static XaotValueRep prepare_vector_rep(const XaotValueRep *base, const XiValue *value,
                                       uint32_t features) {
    XaotValueRep rep = base ? *base : xaot_value_rep_for_value(value);
    uint8_t native_type = xi_vec_shape_native_type(value->aux_int);
    uint8_t lanes = xi_vec_shape_lanes(value->aux_int);
    rep.kind = XAOT_VALUE_VECTOR;
    rep.c_type = prepare_vector_native_c_type(features, native_type, lanes,
                                              xi_vec_shape_is_scalable(value->aux_int));
    rep.flags &= ~(XAOT_VALUE_FLAG_DYNAMIC_C_TYPE | XAOT_VALUE_FLAG_OWNED_C_TYPE);
    rep.vector_native_type = native_type;
    rep.vector_lanes = lanes;
    rep.vector_width_bytes = (uint8_t) (prepare_vector_lane_bytes(native_type) * (unsigned) lanes);
    return rep;
}

static bool prepare_vector_reps_same(XaotValueRep a, XaotValueRep b) {
    return a.kind == XAOT_VALUE_VECTOR && b.kind == XAOT_VALUE_VECTOR &&
           a.vector_native_type == b.vector_native_type && a.vector_lanes == b.vector_lanes &&
           a.vector_width_bytes == b.vector_width_bytes && a.c_type && b.c_type &&
           strcmp(a.c_type, b.c_type) == 0;
}

static bool prepare_vector_propagate_identity_plan(XaotBundle *bundle, XaotValuePlan *plan) {
    const XiValue *value = plan ? plan->value : NULL;
    if (!value ||
        (value->op != XI_COPY && !xi_op_is_identity_forward(value->op) && value->op != XI_PHI) ||
        value->nargs == 0)
        return false;
    const XaotValuePlan *first = xaot_bundle_find_value_plan(bundle, value->args[0]);
    if (!first || first->rep.kind != XAOT_VALUE_VECTOR)
        return false;
    for (uint16_t i = 1; i < value->nargs; i++) {
        const XaotValuePlan *incoming = xaot_bundle_find_value_plan(bundle, value->args[i]);
        if (!incoming || !prepare_vector_reps_same(first->rep, incoming->rep))
            return false;
    }
    if (prepare_vector_reps_same(plan->rep, first->rep))
        return false;
    prepare_value_plan_set_rep(plan, xaot_value_rep_borrow(first->rep));
    plan->rep.type = value->type;
    return true;
}

static bool prepare_vector_user_accepts_native(const XaotBundle *bundle, const XiValue *user) {
    if (!bundle || !user)
        return false;
    if (user->op == XI_ERR_CHECK || user->op == XI_RETAIN || user->op == XI_RELEASE)
        return true;
    if (prepare_vector_native_op_supported(bundle->target_simd_features, user))
        return true;
    if (user->op == XI_COPY || xi_op_is_identity_forward(user->op) || user->op == XI_PHI) {
        const XaotValuePlan *plan = xaot_bundle_find_value_plan(bundle, user);
        return plan && plan->rep.kind == XAOT_VALUE_VECTOR;
    }
    return false;
}

static bool prepare_vector_value_has_unsupported_use(const XaotBundle *bundle, const XiFunc *func,
                                                     const XiValue *value) {
    if (!bundle || !func || !value)
        return true;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        if (block->control == value)
            return true;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (phi->value.args[ai] == value &&
                    !prepare_vector_user_accepts_native(bundle, &phi->value))
                    return true;
            }
        }
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *user = block->values[vi];
            if (!user)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] == value && !prepare_vector_user_accepts_native(bundle, user))
                    return true;
            }
        }
    }
    return false;
}

static bool prepare_vector_identity_inputs_still_native(const XaotBundle *bundle,
                                                        const XiValue *value,
                                                        XaotValueRep expected) {
    if (!bundle || !value ||
        (value->op != XI_COPY && !xi_op_is_identity_forward(value->op) && value->op != XI_PHI))
        return true;
    for (uint16_t i = 0; i < value->nargs; i++) {
        const XaotValuePlan *incoming = xaot_bundle_find_value_plan(bundle, value->args[i]);
        if (!incoming || !prepare_vector_reps_same(expected, incoming->rep))
            return false;
    }
    return true;
}

static void prepare_recount_value_stats(XaotBundle *bundle) {
    if (!bundle)
        return;
    bundle->stats.values_total = 0;
    bundle->stats.values_scalar = 0;
    bundle->stats.values_tagged = 0;
    bundle->stats.values_ptr = 0;
    bundle->stats.values_aggregate = 0;
    bundle->stats.values_vector = 0;
    bundle->stats.values_view = 0;
    bundle->stats.values_void = 0;
    bundle->stats.values_enum_ordinal = 0;
    bundle->stats.values_rep_adapter = 0;
    for (uint32_t module_index = 0; module_index < bundle->nmodules; module_index++) {
        const XrTargetPlan *target_plan = xaot_bundle_target_plan_for_module(bundle, module_index);
        uint32_t binding_count = 0;
        const XrTargetValueRepRecord *bindings =
            xr_target_plan_value_reps(target_plan, &binding_count);
        for (uint32_t binding_index = 0; binding_index < binding_count; binding_index++) {
            const XrTargetMachineRepRecord *machine =
                xr_target_plan_machine_rep(target_plan, bindings[binding_index].memory_rep);
            record_value_stats(&bundle->stats,
                               machine && machine->kind == XR_MACHINE_REP_VOID ? XAOT_VALUE_VOID
                                                                               : XAOT_VALUE_SCALAR,
                               false, false);
        }
    }
    for (uint32_t i = 0; i < bundle->nvalue_plans; i++)
        record_value_stats(
            &bundle->stats, bundle->value_plans[i].rep.kind,
            xaot_value_plan_is_exact_enum_ordinal_family(bundle, &bundle->value_plans[i]),
            xaot_value_plan_is_exact_rep_adapter(bundle, &bundle->value_plans[i]));
}

static void prepare_target_vector_value_plans(XaotBundle *bundle) {
    if (!bundle || bundle->target_simd_features == 0)
        return;
    for (uint32_t i = 0; i < bundle->nvalue_plans; i++) {
        XaotValuePlan *plan = &bundle->value_plans[i];
        const XiValue *value = plan->value;
        const XaotFuncPlan *func_plan = xaot_bundle_find_func_plan(bundle, plan->func);
        if (!func_plan || func_plan->abi.kind == XAOT_ABI_CORO)
            continue;
        if (prepare_vector_op_has_vector_result(value) &&
            prepare_vector_native_op_supported(bundle->target_simd_features, value))
            prepare_value_plan_set_rep(
                plan, prepare_vector_rep(&plan->rep, value, bundle->target_simd_features));
    }
    for (uint32_t iteration = 0; iteration < bundle->nvalue_plans; iteration++) {
        bool changed = false;
        for (uint32_t i = 0; i < bundle->nvalue_plans; i++)
            changed =
                prepare_vector_propagate_identity_plan(bundle, &bundle->value_plans[i]) || changed;
        if (!changed)
            break;
    }
    for (uint32_t iteration = 0; iteration < bundle->nvalue_plans; iteration++) {
        bool changed = false;
        for (uint32_t i = 0; i < bundle->nvalue_plans; i++) {
            XaotValuePlan *plan = &bundle->value_plans[i];
            if (plan->rep.kind != XAOT_VALUE_VECTOR)
                continue;
            if (!prepare_vector_identity_inputs_still_native(bundle, plan->value, plan->rep) ||
                prepare_vector_value_has_unsupported_use(bundle, plan->func, plan->value)) {
                prepare_value_plan_set_rep(
                    plan, xaot_abi_native_value_rep(bundle, plan->func, plan->value));
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    prepare_recount_value_stats(bundle);
}

static bool prepare_func_fixed_bytes_plans(XaotBundle *bundle, const XiFunc *func) {
    if (!bundle || !func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (!value || (value->op != XI_FIXED_BYTES_CONST && value->op != XI_STATIC_BYTES_PTR))
                continue;
            if (!xaot_bundle_add_fixed_bytes_plan(bundle, func, value)) {
                bundle->error_msg = "failed to derive AOT fixed-bytes plan";
                return false;
            }
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
        prepare_value_plan_set_rep(vp, xaot_value_rep_borrow(ret_rep));
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

static bool prepare_compact_adt_value_rep_for_type(const XaotBundle *bundle, const XrType *type,
                                                   XaotValueRep *out_rep) {
    const XaotEnumPlan *plan;
    if (!bundle || !type || !out_rep)
        return false;
    plan = xaot_bundle_find_enum_plan_for_type(bundle, type);
    if (!plan || plan->scalar_action != XAOT_ENUM_SCALAR_COMPACT_AGGREGATE)
        return false;
    memset(out_rep, 0, sizeof(*out_rep));
    out_rep->kind = XAOT_VALUE_AGGREGATE;
    out_rep->rep = XAOT_REP_TAGGED;
    out_rep->type = type;
    out_rep->c_type = plan && plan->c_type ? plan->c_type : "XrAotEnumAggregate";
    out_rep->flags = XAOT_VALUE_FLAG_ENUM;
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
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
        case XI_CHECKTYPE:
            break;
        default:
            return false;
    }
    const XaotValuePlan *arg_plan = xaot_bundle_find_value_plan(bundle, value->args[0]);
    if (!arg_plan || !value_rep_is_propagating_aggregate(arg_plan->rep))
        return false;
    *out_rep = xaot_value_rep_borrow(arg_plan->rep);
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
        prepare_value_plan_set_rep(vp, xaot_value_rep_borrow(rep));
        *changed = true;
    }
    switch (value->op) {
        case XI_COPY:
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
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
            prepare_value_plan_set_rep(vp, xaot_value_rep_borrow(rep));
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

/* A typed catch is lowered as ERR_CATCH(any) -> IS(T) -> AS(T).  In the
 * freestanding profile, payload enums live in the separate aggregate error
 * channel and zero-payload enums use a native ordinal.  Propagate the typed
 * AS representation back to ERR_CATCH so codegen consumes the correct
 * channel without boxing or an invalid tagged/native assignment. */
static bool prepare_apply_freestanding_typed_catch_rep(XaotBundle *bundle, XiValue *value,
                                                       bool *changed) {
    if (!prepare_bundle_is_freestanding(bundle) || !value || value->op != XI_AS ||
        value->nargs < 1 || !value->args[0] || value->args[0]->op != XI_ERR_CATCH)
        return false;

    XaotValueRep rep;
    if (!prepare_compact_adt_value_rep_for_type(bundle, value->type, &rep)) {
        if (!prepare_type_is_unit_enum_ordinal(bundle, value->type))
            return false;
        rep = prepare_enum_ordinal_value_rep(value->type);
    }

    XaotValuePlan *narrow_plan = xaot_bundle_find_value_plan_mut(bundle, value);
    XaotValuePlan *catch_plan = xaot_bundle_find_value_plan_mut(bundle, value->args[0]);
    if (!narrow_plan || !catch_plan)
        return false;
    if (!value_reps_equal(narrow_plan->rep, rep)) {
        prepare_value_plan_set_rep(narrow_plan, xaot_value_rep_borrow(rep));
        *changed = true;
    }
    if (!value_reps_equal(catch_plan->rep, rep)) {
        prepare_value_plan_set_rep(catch_plan, xaot_value_rep_borrow(rep));
        *changed = true;
    }
    return true;
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
            if (prepare_apply_freestanding_typed_catch_rep(bundle, value, changed))
                continue;
            /* ERR_SET/ERR_RETURN carry the thrown value, not the enclosing
             * function's ordinary result.  A value-struct return can make the
             * statement itself aggregate-shaped; resolve the error channel
             * first so that shape is never propagated into a payload enum and
             * forced back through the hosted boxed-error path. */
            if (prepare_apply_error_channel_aggregate_rep(bundle, value, changed))
                continue;
            if (value_rep_is_propagating_aggregate(vp->rep)) {
                prepare_mark_aggregate_value_rep(bundle, value, vp->rep, changed, 0);
                continue;
            }
            if (!prepare_parallel_reduce_aggregate_rep(bundle, value, &rep) &&
                !prepare_identity_aggregate_rep(bundle, value, &rep))
                continue;
            if (!value_reps_equal(vp->rep, rep)) {
                prepare_value_plan_set_rep(vp, xaot_value_rep_borrow(rep));
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
    const XaotAbiSlot *slot;
    XaotValueRep arg_rep;
    XaotValueRep slot_rep;
    XaotBoundaryStep *step;

    if (!bundle || !caller_plan || !call || !target || !target_plan || !arg)
        return false;
    if (arg_index >= target_plan->abi.nparams || !target_plan->abi.params) {
        bundle->error_msg = "AOT direct call argument count exceeds target ABI";
        return false;
    }

    if (!prepare_effective_value_rep(bundle, caller_plan->func, arg, &arg_rep))
        return false;
    slot = &target_plan->abi.params[arg_index];
    slot_rep = xaot_abi_slot_value_rep(slot);
    if (value_reps_equal(arg_rep, slot_rep))
        return true;

    step = xaot_bundle_add_boundary_step(bundle, XAOT_BOUNDARY_STEP_DIRECT_CALL_ARG,
                                         caller_plan->func, call, arg, XAOT_BOUNDARY_DIRECT_CALL);
    if (!step) {
        bundle->error_msg = "failed to allocate AOT direct call argument boundary";
        return false;
    }
    step->target_func = target;
    step->arg_index = arg_index;
    step->from_rep = xaot_value_rep_borrow(arg_rep);
    step->to_rep = xaot_value_rep_borrow(slot_rep);
    bundle->stats.boundary_count++;
    return true;
}

static bool prepare_direct_call_ret_boundary(XaotBundle *bundle, const XaotFuncPlan *caller_plan,
                                             const XiValue *call, const XiFunc *target,
                                             const XaotFuncPlan *target_plan) {
    XaotValuePlan *call_plan;
    XaotValueRep call_rep;
    XaotValueRep ret_rep;
    XaotBoundaryStep *step;

    if (!bundle || !caller_plan || !call || !target || !target_plan)
        return false;
    if (target_plan->abi.ret.cls == XAOT_ARG_VOID)
        return true;

    if (!prepare_effective_value_rep(bundle, caller_plan->func, call, &call_rep))
        return false;
    ret_rep = xaot_abi_slot_value_rep(&target_plan->abi.ret);
    if (ret_rep.kind == XAOT_VALUE_AGGREGATE) {
        call_plan = xaot_bundle_find_value_plan_mut(bundle, call);
        if (!call_plan) {
            bundle->error_msg = "AOT aggregate direct call result has no legacy value plan";
            return false;
        }
        prepare_value_plan_set_rep(call_plan, xaot_value_rep_borrow(ret_rep));
        return true;
    }
    if (value_reps_equal(ret_rep, call_rep))
        return true;

    step = xaot_bundle_add_boundary_step(bundle, XAOT_BOUNDARY_STEP_DIRECT_CALL_RET,
                                         caller_plan->func, call, NULL, XAOT_BOUNDARY_DIRECT_CALL);
    if (!step) {
        bundle->error_msg = "failed to allocate AOT direct call return boundary";
        return false;
    }
    step->target_func = target;
    step->from_rep = xaot_value_rep_borrow(ret_rep);
    step->to_rep = xaot_value_rep_borrow(call_rep);
    bundle->stats.boundary_count++;
    return true;
}

static bool prepare_seed_direct_call_aggregate_returns(XaotBundle *bundle, XiFunc *func) {
    if (!bundle || !func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *blk = func->blocks ? func->blocks[bi] : NULL;
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *call = blk->values ? blk->values[vi] : NULL;
            const XiFunc *target;
            const XaotFuncPlan *target_plan;
            XaotValuePlan *call_plan;
            XaotValueRep ret_rep;
            if (!call || (call->op != XI_CALL && call->op != XI_CALL_METHOD &&
                          call->op != XI_CALL_METHOD_DIRECT))
                continue;
            target = xaot_boundary_resolve_direct_call_target(bundle, func, call, NULL);
            target_plan = target ? xaot_bundle_find_func_plan(bundle, target) : NULL;
            if (!target_plan)
                continue;
            ret_rep = xaot_abi_slot_value_rep(&target_plan->abi.ret);
            if (ret_rep.kind != XAOT_VALUE_AGGREGATE)
                continue;
            call_plan = xaot_bundle_find_value_plan_mut(bundle, call);
            if (!call_plan) {
                bundle->error_msg = "AOT aggregate direct call result has no value plan";
                return false;
            }
            prepare_value_plan_set_rep(call_plan, xaot_value_rep_borrow(ret_rep));
        }
    }
    return true;
}

/* Consume the frozen SOURCE_EXPORT argument rows before the legacy local-call
 * ABI seeding path. Cross-module calls are identified only by the verified
 * SemanticPlan/TargetPlan content address and callee stable ID; no live Xi
 * name, type, shared-slot, or import spelling participates in the decision. */
static bool prepare_seed_source_export_call_place_reps(XaotBundle *bundle, XiFunc *func,
                                                       XiValue *call, bool *out_source) {
    const XrTargetPlan *caller_target;
    const XrSemanticPlan *caller_semantic;
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls;
    const XrTargetCallRecord *source_call = NULL;
    const XrSemanticOperationRecord *source_operation = NULL;

    if (out_source)
        *out_source = false;
    if (!bundle || !func || !call || !out_source)
        return false;
    caller_target = xaot_bundle_target_plan_for_func(bundle, func);
    caller_semantic = caller_target ? xr_target_plan_semantic_plan(caller_target) : NULL;
    if (!caller_target || !caller_semantic ||
        !xr_aot_scalar_semantic_value_id(caller_target, func, call, &semantic_function,
                                         &semantic_value, NULL, 0))
        return true;
    calls = xr_target_plan_calls(caller_target, &call_count);
    for (uint32_t i = 0; i < call_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(caller_semantic, calls[i].semantic_operation);
        if (!operation || operation->function != semantic_function ||
            operation->result_value != semantic_value || operation->opcode != call->op)
            continue;
        if (calls[i].target_kind != XR_TARGET_CALL_TARGET_SOURCE_EXPORT ||
            calls[i].calling_convention != XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT)
            continue;
        if (source_call) {
            bundle->error_msg = "AOT source-export call has ambiguous TargetPlan authority";
            return false;
        }
        source_call = &calls[i];
        source_operation = operation;
    }
    if (!source_call)
        return true;
    *out_source = true;

    const XrSemanticDependencyRecord *dependency =
        xr_semantic_plan_dependency(caller_semantic, source_call->source_dependency);
    const XrTargetPlan *callee_target = NULL;
    const XrSemanticPlan *callee_semantic = NULL;
    for (uint32_t i = 0; dependency && i < bundle->nmodules; i++) {
        const XrTargetPlan *candidate = bundle->target_plans ? bundle->target_plans[i] : NULL;
        if (!candidate || !xr_fingerprint_equal(xr_target_plan_semantic_fingerprint(candidate),
                                                dependency->semantic_fingerprint))
            continue;
        if (callee_target) {
            bundle->error_msg = "AOT source-export dependency TargetPlan is ambiguous";
            return false;
        }
        callee_target = candidate;
        callee_semantic = xr_target_plan_semantic_plan(candidate);
    }
    if (!callee_target || !callee_semantic) {
        bundle->error_msg = "AOT source-export dependency TargetPlan is missing";
        return false;
    }

    uint32_t export_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticSourceExportRecord *source_export = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_source_export_count(callee_semantic); i++) {
        const XrSemanticSourceExportRecord *candidate =
            xr_semantic_plan_source_export(callee_semantic, i);
        if (!candidate || !xr_stable_id_equal(candidate->id, source_call->source_export_identity))
            continue;
        if (source_export) {
            bundle->error_msg = "AOT source-export identity is ambiguous";
            return false;
        }
        source_export = candidate;
        export_index = i;
    }
    const XrSemanticFunctionRecord *callee_function =
        source_export ? xr_semantic_plan_function(callee_semantic, source_export->function) : NULL;
    if (!source_export || export_index != source_call->source_export || !callee_function ||
        !xr_stable_id_equal(callee_function->id, source_call->source_callee_identity) ||
        source_call->argument_count != callee_function->parameter_count ||
        call->nargs != (uint16_t) (source_call->argument_count + 1u)) {
        bundle->error_msg = "AOT source-export call disagrees with dependency authority";
        return false;
    }

    const XaotFuncPlan *callee_func_plan = NULL;
    for (uint32_t i = 0; i < bundle->nfunc_plans; i++) {
        const XaotFuncPlan *candidate = &bundle->func_plans[i];
        if (!candidate->func || candidate->func->semantic_plan != callee_semantic ||
            candidate->func->semantic_plan_function_index != source_export->function)
            continue;
        if (callee_func_plan) {
            bundle->error_msg = "AOT source-export callee function is ambiguous";
            return false;
        }
        callee_func_plan = candidate;
    }
    if (!callee_func_plan || callee_func_plan->abi.nparams != source_call->argument_count ||
        (source_call->argument_count != 0 && !callee_func_plan->abi.params)) {
        bundle->error_msg = "AOT source-export callee ABI is incomplete";
        return false;
    }

    uint32_t argument_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(caller_target, &argument_count);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(caller_semantic, &operand_count);
    if ((uint64_t) source_call->argument_begin + source_call->argument_count >
            (uint64_t) argument_count ||
        (uint64_t) source_operation->operand_begin + source_operation->operand_count >
            (uint64_t) operand_count) {
        bundle->error_msg = "AOT source-export argument authority is out of bounds";
        return false;
    }
    for (uint16_t ordinal = 0; ordinal < source_call->argument_count; ordinal++) {
        const XrTargetCallArgumentRecord *argument =
            &arguments[source_call->argument_begin + ordinal];
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(callee_semantic, callee_function->parameter_begin + ordinal);
        const XrSemanticOperandRecord *operand =
            &operands[source_operation->operand_begin + ordinal + 1u];
        XiValue *place = call->args[ordinal + 1u];
        uint32_t place_function = XR_SEMANTIC_INDEX_NONE;
        uint32_t place_value = XR_SEMANTIC_INDEX_NONE;
        if (!parameter || !operand || argument->call != source_call->id ||
            argument->ordinal != ordinal ||
            argument->callee_parameter != callee_function->parameter_begin + ordinal ||
            argument->semantic_operand != source_operation->operand_begin + ordinal + 1u ||
            argument->semantic_value != operand->value ||
            argument->callee_slot != XR_SEMANTIC_INDEX_NONE ||
            argument->callee_register_rep != argument->register_rep ||
            argument->callee_memory_rep != argument->memory_rep ||
            !xr_aot_scalar_semantic_value_id(caller_target, func, place, &place_function,
                                             &place_value, NULL, 0) ||
            place_function != semantic_function || place_value != argument->semantic_value) {
            bundle->error_msg = "AOT source-export argument lacks exact TargetPlan identity";
            return false;
        }
        if (argument->mode != XR_TARGET_CALL_REFERENCE)
            continue;
        const XaotAbiSlot *slot = &callee_func_plan->abi.params[ordinal];
        const XrTargetMachineRepRecord *machine =
            xr_target_plan_machine_rep(caller_target, argument->memory_rep);
        if (parameter->mode != XR_PARAM_REF || argument->ownership != XR_TARGET_CALL_WRITEBACK ||
            argument->flags != XR_TARGET_CALL_ARGUMENT_ADDRESSABLE || !machine ||
            machine->kind != XR_MACHINE_REP_RAW_PTR || !place || place->op != XI_LOCAL_ADDR ||
            place->nargs != 1 || !place->args[0] ||
            (slot->flags & XAOT_ABI_SLOT_BORROWED_PLACE) == 0 || slot->rep.rep != XAOT_REP_RAWPTR ||
            !slot->rep.c_type) {
            bundle->error_msg = "AOT source-export ref argument lacks exact place ABI";
            return false;
        }
        /* Target-bound semantic values deliberately have no mutable legacy
         * value plan. The row proves the call-side pointer depth; C emission
         * consumes the callee ABI cast while LOCAL_ADDR keeps its TargetPlan
         * RAW_PTR storage. Mutating the backend adapter feeding LOCAL_ADDR
         * would destroy its independently verified representation identity. */
    }
    return true;
}

static bool prepare_seed_direct_call_place_reps(XaotBundle *bundle, XiFunc *func) {
    if (!bundle || !func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *blk = func->blocks ? func->blocks[bi] : NULL;
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *call = blk->values ? blk->values[vi] : NULL;
            uint16_t first_arg = 0;
            if (!call || (call->op != XI_CALL && call->op != XI_CALL_METHOD &&
                          call->op != XI_CALL_METHOD_DIRECT))
                continue;
            bool source_export = false;
            if (!prepare_seed_source_export_call_place_reps(bundle, func, call, &source_export))
                return false;
            if (source_export)
                continue;
            const XiFunc *target =
                xaot_boundary_resolve_direct_call_target(bundle, func, call, &first_arg);
            const XaotFuncPlan *target_plan =
                target ? xaot_bundle_find_func_plan(bundle, target) : NULL;
            if (!target_plan || !target_plan->abi.params)
                continue;
            for (uint16_t a = first_arg; a < call->nargs; a++) {
                uint16_t param_idx = (uint16_t) (a - first_arg);
                if (param_idx >= target_plan->abi.nparams)
                    break;
                const XaotAbiSlot *slot = &target_plan->abi.params[param_idx];
                XiValue *place = call->args[a];
                if ((slot->flags & XAOT_ABI_SLOT_BORROWED_PLACE) == 0 || !place ||
                    place->op != XI_LOCAL_ADDR || place->nargs != 1 || !place->args[0])
                    continue;
                /* Preserve the complete borrowed-place C type on the address
                 * value itself.  A ref Ptr<T> slot is `const void **`, while
                 * the source pointer local is `const void *`; flattening the
                 * XI_LOCAL_ADDR plan back to a generic raw pointer makes an
                 * otherwise verified direct call rely on an incompatible C
                 * pointer conversion. */
                XaotValuePlan *place_plan = xaot_bundle_find_value_plan_mut(bundle, place);
                if (place_plan)
                    prepare_value_plan_set_rep(place_plan, xaot_value_rep_borrow(slot->rep));
                if (value_rep_is_struct_aggregate(slot->pointee_rep)) {
                    bool changed = false;
                    prepare_mark_aggregate_value_rep(bundle, place->args[0], slot->pointee_rep,
                                                     &changed, 0);
                } else if (slot->pointee_rep.kind != XAOT_VALUE_VIEW) {
                    /* The callee dereferences exactly the ABI pointee type.
                     * Keep the address-taken local in that representation too;
                     * otherwise `ref Class` can pass an 8-byte native-pointer
                     * local to an XrValue* ABI and the callee reads a different
                     * object than the source place.  A VIEW pointee is instead
                     * a projection ABI (for example `ref [u32; 4]`); its source
                     * remains the owning aggregate and the call boundary
                     * projects the element pointer. */
                    XaotValuePlan *source = xaot_bundle_find_value_plan_mut(bundle, place->args[0]);
                    if (source)
                        prepare_value_plan_set_rep(source,
                                                   xaot_value_rep_borrow(slot->pointee_rep));
                }
            }
        }
    }
    return true;
}

static bool prepare_seed_place_load_aggregate_reps(XaotBundle *bundle, XiFunc *func) {
    if (!bundle || !func)
        return false;
    const XaotFuncPlan *func_plan = xaot_bundle_find_func_plan(bundle, func);
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *blk = func->blocks ? func->blocks[bi] : NULL;
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *load = blk->values ? blk->values[vi] : NULL;
            if (!load || load->op != XI_PLACE_LOAD || load->nargs != 1 || !load->args[0])
                continue;
            XiValue *place = load->args[0];
            XaotValueRep pointee;
            memset(&pointee, 0, sizeof(pointee));
            if (place->op == XI_PARAM && place->aux_int >= 0 && func_plan &&
                func_plan->abi.params && place->aux_int < func_plan->abi.nparams) {
                const XaotAbiSlot *slot = &func_plan->abi.params[place->aux_int];
                if ((slot->flags & XAOT_ABI_SLOT_BORROWED_PLACE) != 0)
                    pointee = slot->pointee_rep;
            } else if (place->op == XI_LOCAL_ADDR && place->nargs == 1 && place->args[0]) {
                const XaotValuePlan *source = xaot_bundle_find_value_plan(bundle, place->args[0]);
                if (source)
                    pointee = source->rep;
            }
            if (!value_rep_is_struct_aggregate(pointee))
                continue;
            XaotValuePlan *load_plan = xaot_bundle_find_value_plan_mut(bundle, load);
            if (load_plan)
                prepare_value_plan_set_rep(load_plan, xaot_value_rep_borrow(pointee));
        }
    }
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
        step->from_rep = xaot_value_rep_borrow(func_plan->abi.ret.rep);
        step->to_rep = xaot_value_rep_borrow(func_plan->abi.ret.rep);
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
    XgModuleId module_id = (XgModuleId) (module_index + 1u);

    if (!ev || !func || func->xg_body_func_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        const XgBodySummary *body = &ev->bodies[i];
        if (body->func_id != (XgFuncId) func->xg_body_func_id)
            continue;
        if (body->module_id != module_id)
            return NULL;
        if ((body->kind == XG_BODY_MODULE_INIT) != is_module_init)
            return NULL;
        return body;
    }
    return NULL;
}

static bool func_attr_body_summary_disqualifies(const XaotBundle *bundle, const XgBodySummary *body,
                                                uint32_t *out_effect_bits) {
    const XgGlobalEvidence *ev = bundle ? bundle->global_evidence_plan.evidence : NULL;
    uint32_t effect_bits = 0;

    if (!body || !xg_body_effects_compose_closed_world_calls(ev, body, &effect_bits))
        return true;
    if (out_effect_bits)
        *out_effect_bits = effect_bits;
    return (effect_bits & (XG_BODY_MAY_ERROR | XG_BODY_MAY_PANIC | XG_BODY_MAY_SUSPEND |
                           XG_BODY_MAY_ALLOC | XG_BODY_MAY_MUTATE | XG_BODY_MAY_CALL_NATIVE)) != 0;
}

/* Prove a function free of observable effects so Cgen can emit
 * __attribute__((const)) (touches no memory) or ((pure)) (reads only).
 * Evidence is the body summary plus per-value effect flags; the verifier
 * re-checks both.
 * Disqualification is not an error —the function simply gets no plan. */
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
    if (!prepare_func_fixed_bytes_plans(bundle, func))
        return false;
    if (!prepare_func_enum_domain_plan(bundle, func))
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

/* Materialize foreign declarations from executable callsites, never from the
 * mere presence of an extern item.  This is the pruning boundary that keeps
 * unused symbols and dylibs out of generated units. */
static bool prepare_func_extern_decls(XaotBundle *bundle, const XiFunc *func) {
    if (!bundle || !func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks ? func->blocks[bi] : NULL;
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *call = block->values ? block->values[vi] : NULL;
            uint16_t first_arg = 0;
            if (!call)
                continue;
            if ((call->op == XI_CLOSURE_NEW ||
                 (call->op == XI_STACK_ALLOC && call->aux_int == XI_CLOSURE_NEW)) &&
                call->aux && ((XiFunc *) call->aux)->is_extern) {
                if (xaot_bundle_extern_closure_is_used(bundle, func, call) &&
                    !xaot_bundle_register_extern_decl(bundle, (XiFunc *) call->aux, call->line))
                    return false;
                continue;
            }
            if (call->op != XI_CALL && call->op != XI_CALL_METHOD &&
                call->op != XI_CALL_METHOD_DIRECT)
                continue;
            XiFunc *target =
                (XiFunc *) xaot_boundary_resolve_direct_call_target(bundle, func, call, &first_arg);
            (void) first_arg;
            if (target && target->is_extern &&
                !xaot_bundle_register_extern_decl(bundle, target, call->line))
                return false;
        }
    }
    return true;
}

XR_FUNC bool xaot_prepare_bundle(XaotBundle *bundle, XaotPrepareStats *out_stats) {
    uint32_t mi;
    if (!bundle || !bundle->modules)
        return false;

    memset(&bundle->stats, 0, sizeof(bundle->stats));
    bundle->error_msg = NULL;
    if (!prepare_require_target_plans(bundle))
        return false;

    for (mi = 0; mi < bundle->nmodules; mi++) {
        XiModule *mod = bundle->modules[mi];
        if (!mod || !mod->init) {
            bundle->error_msg = "module has no Xi init function";
            return false;
        }
        if (!prepare_func_recursive(bundle, mod->init, mi, 0, true))
            return false;
    }
    /* Function-value flow is whole-program: function/ABI rows for every
     * module must exist before argument/return/storage edges can converge. */
    if (!xaot_callable_plans_build(bundle)) {
        bundle->error_msg = "failed to build closed-world callable invoke plans";
        return false;
    }
    if (!xaot_entry_plan_derive(bundle, bundle->global_evidence_plan.evidence,
                                bundle->global_evidence_plan.profile, &bundle->entry_plan) ||
        bundle->entry_plan.unproven_reason != XR_ENTRY_PROVEN) {
        bundle->error_msg = "failed to refresh entry plan from callable reachability";
        return false;
    }
    bundle->has_entry_plan = true;
    for (mi = 0; mi < bundle->nfunc_plans; mi++) {
        if (!prepare_func_extern_decls(bundle, bundle->func_plans[mi].func))
            return false;
    }
    if (!xaot_bundle_sync_transfer_capability_plans(bundle)) {
        bundle->error_msg = "failed to sync AOT transfer capability plan";
        return false;
    }
    /* Cross-module value-aggregate calls can only be resolved after every
     * module/function ABI plan exists. Seed their result representation first,
     * then rerun the local aggregate propagation so copies and method receiver
     * arguments inherit the exact target ABI instead of remaining tagged. */
    for (mi = 0; mi < bundle->nfunc_plans; mi++) {
        XiFunc *func = (XiFunc *) bundle->func_plans[mi].func;
        if (!prepare_seed_direct_call_aggregate_returns(bundle, func))
            return false;
        if (!prepare_seed_direct_call_place_reps(bundle, func))
            return false;
    }
    for (mi = 0; mi < bundle->nfunc_plans; mi++) {
        XiFunc *func = (XiFunc *) bundle->func_plans[mi].func;
        if (!prepare_apply_aggregate_value_plans(bundle, func))
            return false;
    }
    for (mi = 0; mi < bundle->nfunc_plans; mi++) {
        XiFunc *func = (XiFunc *) bundle->func_plans[mi].func;
        if (!prepare_seed_place_load_aggregate_reps(bundle, func))
            return false;
    }
    for (mi = 0; mi < bundle->nfunc_plans; mi++) {
        XiFunc *func = (XiFunc *) bundle->func_plans[mi].func;
        if (!prepare_apply_aggregate_value_plans(bundle, func))
            return false;
    }
    prepare_target_vector_value_plans(bundle);
    for (mi = 0; mi < bundle->nfunc_plans; mi++) {
        if (!prepare_func_boundary_steps(bundle, &bundle->func_plans[mi]))
            return false;
    }

    if (out_stats)
        *out_stats = bundle->stats;
    return true;
}
